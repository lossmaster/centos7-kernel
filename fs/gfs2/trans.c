/*
 * Copyright (C) Sistina Software, Inc.  1997-2003 All rights reserved.
 * Copyright (C) 2004-2006 Red Hat, Inc.  All rights reserved.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License version 2.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/buffer_head.h>
#include <linux/kallsyms.h>
#include <linux/gfs2_ondisk.h>

#include "gfs2.h"
#include "incore.h"
#include "glock.h"
#include "inode.h"
#include "log.h"
#include "lops.h"
#include "meta_io.h"
#include "trans.h"
#include "util.h"
#include "trace_gfs2.h"

int gfs2_trans_begin(struct gfs2_sbd *sdp, unsigned int blocks,
		     unsigned int revokes)
{
	struct gfs2_trans *tr;
	int error;

	BUG_ON(current->journal_info);
	BUG_ON(blocks == 0 && revokes == 0);

	if (!test_bit(SDF_JOURNAL_LIVE, &sdp->sd_flags))
		return -EROFS;

	tr = kzalloc(sizeof(struct gfs2_trans), GFP_NOFS);
	if (!tr)
		return -ENOMEM;

	tr->tr_ip = (unsigned long)__builtin_return_address(0);
	tr->tr_blocks = blocks;
	tr->tr_revokes = revokes;
	tr->tr_reserved = 1;
	if (blocks)
		tr->tr_reserved += 6 + blocks;
	if (revokes)
		tr->tr_reserved += gfs2_struct2blk(sdp, revokes,
						   sizeof(u64));
	INIT_LIST_HEAD(&tr->tr_databuf);
	INIT_LIST_HEAD(&tr->tr_buf);

	sb_start_intwrite(sdp->sd_vfs);
	gfs2_holder_init(sdp->sd_trans_gl, LM_ST_SHARED, 0, &tr->tr_t_gh);

	error = gfs2_glock_nq(&tr->tr_t_gh);
	if (error)
		goto fail_holder_uninit;

	error = gfs2_log_reserve(sdp, tr->tr_reserved);
	if (error)
		goto fail_gunlock;

	current->journal_info = tr;

	return 0;

fail_gunlock:
	gfs2_glock_dq(&tr->tr_t_gh);

fail_holder_uninit:
	sb_end_intwrite(sdp->sd_vfs);
	gfs2_holder_uninit(&tr->tr_t_gh);
	kfree(tr);

	return error;
}

/**
 * gfs2_log_release - Release a given number of log blocks
 * @sdp: The GFS2 superblock
 * @blks: The number of blocks
 *
 */

static void gfs2_log_release(struct gfs2_sbd *sdp, unsigned int blks)
{

	atomic_add(blks, &sdp->sd_log_blks_free);
	trace_gfs2_log_blocks(sdp, blks);
	gfs2_assert_withdraw(sdp, atomic_read(&sdp->sd_log_blks_free) <=
				  sdp->sd_jdesc->jd_blocks);
	up_read(&sdp->sd_log_flush_lock);
}

static void gfs2_print_trans(struct gfs2_sbd *sdp, const struct gfs2_trans *tr)
{
	fs_warn(sdp, "Transaction created at: %pSR\n", (void *)tr->tr_ip);
	fs_warn(sdp, "blocks=%u revokes=%u reserved=%u touched=%u\n",
		tr->tr_blocks, tr->tr_revokes, tr->tr_reserved,
		test_bit(TR_TOUCHED, &tr->tr_flags));
	fs_warn(sdp, "Buf %u/%u Databuf %u/%u Revoke %u/%u\n",
		tr->tr_num_buf_new, tr->tr_num_buf_rm,
		tr->tr_num_databuf_new, tr->tr_num_databuf_rm,
		tr->tr_num_revoke, tr->tr_num_revoke_rm);
}

void gfs2_trans_end(struct gfs2_sbd *sdp)
{
	struct gfs2_trans *tr = current->journal_info;
	s64 nbuf;
	current->journal_info = NULL;

	if (!test_bit(TR_TOUCHED, &tr->tr_flags)) {
		gfs2_log_release(sdp, tr->tr_reserved);
		if (tr->tr_t_gh.gh_gl) {
			gfs2_glock_dq(&tr->tr_t_gh);
			gfs2_holder_uninit(&tr->tr_t_gh);
			kfree(tr);
		}
		sb_end_intwrite(sdp->sd_vfs);
		return;
	}

	nbuf = tr->tr_num_buf_new + tr->tr_num_databuf_new;
	nbuf -= tr->tr_num_buf_rm;
	nbuf -= tr->tr_num_databuf_rm;

	if (gfs2_assert_withdraw(sdp, (nbuf <= tr->tr_blocks) &&
				       (tr->tr_num_revoke <= tr->tr_revokes)))
		gfs2_print_trans(sdp, tr);

	gfs2_log_commit(sdp, tr);
	if (tr->tr_t_gh.gh_gl) {
		gfs2_glock_dq(&tr->tr_t_gh);
		gfs2_holder_uninit(&tr->tr_t_gh);
		if (!test_bit(TR_ATTACHED, &tr->tr_flags))
			kfree(tr);
	}
	up_read(&sdp->sd_log_flush_lock);

	if (sdp->sd_vfs->s_flags & MS_SYNCHRONOUS)
		gfs2_log_flush(sdp, NULL);
	sb_end_intwrite(sdp->sd_vfs);
}

static struct gfs2_bufdata *gfs2_alloc_bufdata(struct gfs2_glock *gl,
					       struct buffer_head *bh,
					       const struct gfs2_log_operations *lops)
{
	struct gfs2_bufdata *bd;

	bd = kmem_cache_zalloc(gfs2_bufdata_cachep, GFP_NOFS | __GFP_NOFAIL);
	bd->bd_bh = bh;
	bd->bd_gl = gl;
	bd->bd_ops = lops;
	INIT_LIST_HEAD(&bd->bd_list);
	bh->b_private = bd;
	return bd;
}

/**
 * gfs2_trans_add_data - Add a databuf to the transaction.
 * @gl: The inode glock associated with the buffer
 * @bh: The buffer to add
 *
 * This is used in journaled data mode.
 * We need to journal the data block in the same way as metadata in
 * the functions above. The difference is that here we have a tag
 * which is two __be64's being the block number (as per meta data)
 * and a flag which says whether the data block needs escaping or
 * not. This means we need a new log entry for each 251 or so data
 * blocks, which isn't an enormous overhead but twice as much as
 * for normal metadata blocks.
 */
void gfs2_trans_add_data(struct gfs2_glock *gl, struct buffer_head *bh)
{
	struct gfs2_trans *tr = current->journal_info;
	struct gfs2_sbd *sdp = gl->gl_name.ln_sbd;
	struct gfs2_bufdata *bd;

	lock_buffer(bh);
	if (buffer_pinned(bh)) {
		set_bit(TR_TOUCHED, &tr->tr_flags);
		goto out;
	}
	gfs2_log_lock(sdp);
	bd = bh->b_private;
	if (bd == NULL) {
		gfs2_log_unlock(sdp);
		unlock_buffer(bh);
		if (bh->b_private == NULL)
			bd = gfs2_alloc_bufdata(gl, bh, &gfs2_databuf_lops);
		else
			bd = bh->b_private;
		lock_buffer(bh);
		gfs2_log_lock(sdp);
	}
	gfs2_assert(sdp, bd->bd_gl == gl);
	set_bit(TR_TOUCHED, &tr->tr_flags);
	if (list_empty(&bd->bd_list)) {
		set_bit(GLF_LFLUSH, &bd->bd_gl->gl_flags);
		set_bit(GLF_DIRTY, &bd->bd_gl->gl_flags);
		gfs2_pin(sdp, bd->bd_bh);
		tr->tr_num_databuf_new++;
		list_add_tail(&bd->bd_list, &tr->tr_databuf);
	}
	gfs2_log_unlock(sdp);
out:
	unlock_buffer(bh);
}

void gfs2_trans_add_meta(struct gfs2_glock *gl, struct buffer_head *bh)
{

	struct gfs2_sbd *sdp = gl->gl_name.ln_sbd;
	struct gfs2_bufdata *bd;
	struct gfs2_meta_header *mh;
	struct gfs2_trans *tr = current->journal_info;

	lock_buffer(bh);
	if (buffer_pinned(bh)) {
		set_bit(TR_TOUCHED, &tr->tr_flags);
		goto out;
	}
	gfs2_log_lock(sdp);
	bd = bh->b_private;
	if (bd == NULL) {
		gfs2_log_unlock(sdp);
		unlock_buffer(bh);
		lock_page(bh->b_page);
		if (bh->b_private == NULL)
			bd = gfs2_alloc_bufdata(gl, bh, &gfs2_buf_lops);
		else
			bd = bh->b_private;
		unlock_page(bh->b_page);
		lock_buffer(bh);
		gfs2_log_lock(sdp);
	}
	gfs2_assert(sdp, bd->bd_gl == gl);
	set_bit(TR_TOUCHED, &tr->tr_flags);
	if (!list_empty(&bd->bd_list))
		goto out_unlock;
	set_bit(GLF_LFLUSH, &bd->bd_gl->gl_flags);
	set_bit(GLF_DIRTY, &bd->bd_gl->gl_flags);
	mh = (struct gfs2_meta_header *)bd->bd_bh->b_data;
	if (unlikely(mh->mh_magic != cpu_to_be32(GFS2_MAGIC))) {
		fs_err(sdp, "Attempting to add uninitialised block to "
		       "journal (inplace block=%lld)\n",
		       (unsigned long long)bd->bd_bh->b_blocknr);
		BUG();
	}
	if (unlikely(gfs2_withdrawn(sdp))) {
		fs_info(sdp, "GFS2:adding buf while withdrawn! 0x%llx\n",
			(unsigned long long)bd->bd_bh->b_blocknr);
	}
	gfs2_pin(sdp, bd->bd_bh);
	mh->__pad0 = cpu_to_be64(0);
	mh->mh_jid = cpu_to_be32(sdp->sd_jdesc->jd_jid);
	list_add(&bd->bd_list, &tr->tr_buf);
	tr->tr_num_buf_new++;
out_unlock:
	gfs2_log_unlock(sdp);
out:
	unlock_buffer(bh);
}

void gfs2_trans_add_revoke(struct gfs2_sbd *sdp, struct gfs2_bufdata *bd)
{
	struct gfs2_trans *tr = current->journal_info;

	BUG_ON(!list_empty(&bd->bd_list));
	gfs2_add_revoke(sdp, bd);
	set_bit(TR_TOUCHED, &tr->tr_flags);
	tr->tr_num_revoke++;
}

void gfs2_trans_add_unrevoke(struct gfs2_sbd *sdp, u64 blkno, unsigned int len)
{
	struct gfs2_bufdata *bd, *tmp;
	struct gfs2_trans *tr = current->journal_info;
	unsigned int n = len;

	gfs2_log_lock(sdp);
	list_for_each_entry_safe(bd, tmp, &sdp->sd_log_le_revoke, bd_list) {
		if ((bd->bd_blkno >= blkno) && (bd->bd_blkno < (blkno + len))) {
			list_del_init(&bd->bd_list);
			gfs2_assert_withdraw(sdp, sdp->sd_log_num_revoke);
			sdp->sd_log_num_revoke--;
			kmem_cache_free(gfs2_bufdata_cachep, bd);
			tr->tr_num_revoke_rm++;
			if (--n == 0)
				break;
		}
	}
	gfs2_log_unlock(sdp);
}

