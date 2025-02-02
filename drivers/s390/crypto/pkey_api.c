/*
 *  pkey device driver
 *
 *  Copyright IBM Corp. 2017
 *  Author(s): Harald Freudenberger
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License (version 2 only)
 * as published by the Free Software Foundation.
 *
 */

#define KMSG_COMPONENT "pkey"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/kallsyms.h>
#include <linux/debugfs.h>
#include <linux/random.h>
#include <asm/zcrypt.h>
#include <asm/cpacf.h>
#include <asm/pkey.h>
#include <crypto/aes.h>

#include "zcrypt_api.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("IBM Corporation");
MODULE_DESCRIPTION("s390 protected key interface");

/* Size of parameter block used for all cca requests/replies */
#define PARMBSIZE 512

/* Size of vardata block used for some of the cca requests/replies */
#define VARDATASIZE 4096

/* mask of available pckmo subfunctions, fetched once at module init */
static cpacf_mask_t pckmo_functions;

/*
 * debug feature data and functions
 */

static debug_info_t *debug_info;

#define DEBUG_DBG(...)	debug_sprintf_event(debug_info, 6, ##__VA_ARGS__)
#define DEBUG_INFO(...) debug_sprintf_event(debug_info, 5, ##__VA_ARGS__)
#define DEBUG_WARN(...) debug_sprintf_event(debug_info, 4, ##__VA_ARGS__)
#define DEBUG_ERR(...)	debug_sprintf_event(debug_info, 3, ##__VA_ARGS__)

static void __init pkey_debug_init(void)
{
	debug_info = debug_register("pkey", 1, 1, 4 * sizeof(long));
	debug_register_view(debug_info, &debug_sprintf_view);
	debug_set_level(debug_info, 3);
}

static void __exit pkey_debug_exit(void)
{
	debug_unregister(debug_info);
}

/* Key token types */
#define TOKTYPE_NON_CCA		0x00 /* Non-CCA key token */
#define TOKTYPE_CCA_INTERNAL	0x01 /* CCA internal key token */

/* For TOKTYPE_NON_CCA: */
#define TOKVER_PROTECTED_KEY	0x01 /* Protected key token */

/* For TOKTYPE_CCA_INTERNAL: */
#define TOKVER_CCA_AES		0x04 /* CCA AES key token */

/* header part of a key token */
struct keytoken_header {
	u8  type;     /* one of the TOKTYPE values */
	u8  res0[3];
	u8  version;  /* one of the TOKVER values */
	u8  res1[3];
} __packed;

/* inside view of a secure key token (only type 0x01 version 0x04) */
struct secaeskeytoken {
	u8  type;     /* 0x01 for internal key token */
	u8  res0[3];
	u8  version;  /* should be 0x04 */
	u8  res1[1];
	u8  flag;     /* key flags */
	u8  res2[1];
	u64 mkvp;     /* master key verification pattern */
	u8  key[32];  /* key value (encrypted) */
	u8  cv[8];    /* control vector */
	u16 bitsize;  /* key bit size */
	u16 keysize;  /* key byte size */
	u8  tvv[4];   /* token validation value */
} __packed;

/* inside view of a protected key token (only type 0x00 version 0x01) */
struct protaeskeytoken {
	u8  type;     /* 0x00 for PAES specific key tokens */
	u8  res0[3];
	u8  version;  /* should be 0x01 for protected AES key token */
	u8  res1[3];
	u32 keytype;  /* key type, one of the PKEY_KEYTYPE values */
	u32 len;      /* bytes actually stored in protkey[] */
	u8  protkey[MAXPROTKEYSIZE]; /* the protected key blob */
} __packed;

/*
 * Simple check if the token is a valid CCA secure AES key
 * token. If keybitsize is given, the bitsize of the key is
 * also checked. Returns 0 on success or errno value on failure.
 */
static int check_secaeskeytoken(const u8 *token, int keybitsize)
{
	struct secaeskeytoken *t = (struct secaeskeytoken *) token;

	if (t->type != TOKTYPE_CCA_INTERNAL) {
		DEBUG_ERR(
			"%s secure token check failed, type mismatch 0x%02x != 0x%02x\n",
			__func__, (int) t->type, TOKTYPE_CCA_INTERNAL);
		return -EINVAL;
	}
	if (t->version != TOKVER_CCA_AES) {
		DEBUG_ERR(
			"%s secure token check failed, version mismatch 0x%02x != 0x%02x\n",
			__func__, (int) t->version, TOKVER_CCA_AES);
		return -EINVAL;
	}
	if (keybitsize > 0 && t->bitsize != keybitsize) {
		DEBUG_ERR(
			"check_secaeskeytoken secure token check failed, bitsize mismatch %d != %d\n",
			(int) t->bitsize, keybitsize);
		return -EINVAL;
	}

	return 0;
}

/*
 * Allocate consecutive memory for request CPRB, request param
 * block, reply CPRB and reply param block and fill in values
 * for the common fields. Returns 0 on success or errno value
 * on failure.
 */
static int alloc_and_prep_cprbmem(size_t paramblen,
				  u8 **pcprbmem,
				  struct CPRBX **preqCPRB,
				  struct CPRBX **prepCPRB)
{
	u8 *cprbmem;
	size_t cprbplusparamblen = sizeof(struct CPRBX) + paramblen;
	struct CPRBX *preqcblk, *prepcblk;

	/*
	 * allocate consecutive memory for request CPRB, request param
	 * block, reply CPRB and reply param block
	 */
	cprbmem = kmalloc(2 * cprbplusparamblen, GFP_KERNEL);
	if (!cprbmem)
		return -ENOMEM;
	memset(cprbmem, 0, 2 * cprbplusparamblen);

	preqcblk = (struct CPRBX *) cprbmem;
	prepcblk = (struct CPRBX *) (cprbmem + cprbplusparamblen);

	/* fill request cprb struct */
	preqcblk->cprb_len = sizeof(struct CPRBX);
	preqcblk->cprb_ver_id = 0x02;
	memcpy(preqcblk->func_id, "T2", 2);
	preqcblk->rpl_msgbl = cprbplusparamblen;
	if (paramblen) {
		preqcblk->req_parmb =
			((u8 *) preqcblk) + sizeof(struct CPRBX);
		preqcblk->rpl_parmb =
			((u8 *) prepcblk) + sizeof(struct CPRBX);
	}

	*pcprbmem = cprbmem;
	*preqCPRB = preqcblk;
	*prepCPRB = prepcblk;

	return 0;
}

/*
 * Free the cprb memory allocated with the function above.
 * If the scrub value is not zero, the memory is filled
 * with zeros before freeing (useful if there was some
 * clear key material in there).
 */
static void free_cprbmem(void *mem, size_t paramblen, int scrub)
{
	if (scrub)
		memzero_explicit(mem, 2 * (sizeof(struct CPRBX) + paramblen));
	kfree(mem);
}

/*
 * Helper function to prepare the xcrb struct
 */
static inline void prep_xcrb(struct ica_xcRB *pxcrb,
			     u16 cardnr,
			     struct CPRBX *preqcblk,
			     struct CPRBX *prepcblk)
{
	memset(pxcrb, 0, sizeof(*pxcrb));
	pxcrb->agent_ID = 0x4341; /* 'CA' */
	pxcrb->user_defined = (cardnr == 0xFFFF ? AUTOSELECT : cardnr);
	pxcrb->request_control_blk_length =
		preqcblk->cprb_len + preqcblk->req_parml;
	pxcrb->request_control_blk_addr = (void *) preqcblk;
	pxcrb->reply_control_blk_length = preqcblk->rpl_msgbl;
	pxcrb->reply_control_blk_addr = (void *) prepcblk;
}

/*
 * Helper function which calls zcrypt_send_cprb with
 * memory management segment adjusted to kernel space
 * so that the copy_from_user called within this
 * function do in fact copy from kernel space.
 */
static inline int _zcrypt_send_cprb(struct ica_xcRB *xcrb)
{
	int rc;
	mm_segment_t old_fs = get_fs();

	set_fs(KERNEL_DS);
	rc = zcrypt_send_cprb(xcrb);
	set_fs(old_fs);

	return rc;
}

/*
 * Generate (random) AES secure key.
 */
int pkey_genseckey(u16 cardnr, u16 domain,
		   u32 keytype, struct pkey_seckey *seckey)
{
	int i, rc, keysize;
	int seckeysize;
	u8 *mem;
	struct CPRBX *preqcblk, *prepcblk;
	struct ica_xcRB xcrb;
	struct kgreqparm {
		u8  subfunc_code[2];
		u16 rule_array_len;
		struct lv1 {
			u16 len;
			char  key_form[8];
			char  key_length[8];
			char  key_type1[8];
			char  key_type2[8];
		} lv1;
		struct lv2 {
			u16 len;
			struct keyid {
				u16 len;
				u16 attr;
				u8  data[SECKEYBLOBSIZE];
			} keyid[6];
		} lv2;
	} *preqparm;
	struct kgrepparm {
		u8  subfunc_code[2];
		u16 rule_array_len;
		struct lv3 {
			u16 len;
			u16 keyblocklen;
			struct {
				u16 toklen;
				u16 tokattr;
				u8  tok[0];
				/* ... some more data ... */
			} keyblock;
		} lv3;
	} *prepparm;

	/* get already prepared memory for 2 cprbs with param block each */
	rc = alloc_and_prep_cprbmem(PARMBSIZE, &mem, &preqcblk, &prepcblk);
	if (rc)
		return rc;

	/* fill request cprb struct */
	preqcblk->domain = domain;

	/* fill request cprb param block with KG request */
	preqparm = (struct kgreqparm *) preqcblk->req_parmb;
	memcpy(preqparm->subfunc_code, "KG", 2);
	preqparm->rule_array_len = sizeof(preqparm->rule_array_len);
	preqparm->lv1.len = sizeof(struct lv1);
	memcpy(preqparm->lv1.key_form,	 "OP      ", 8);
	switch (keytype) {
	case PKEY_KEYTYPE_AES_128:
		keysize = 16;
		memcpy(preqparm->lv1.key_length, "KEYLN16 ", 8);
		break;
	case PKEY_KEYTYPE_AES_192:
		keysize = 24;
		memcpy(preqparm->lv1.key_length, "KEYLN24 ", 8);
		break;
	case PKEY_KEYTYPE_AES_256:
		keysize = 32;
		memcpy(preqparm->lv1.key_length, "KEYLN32 ", 8);
		break;
	default:
		DEBUG_ERR(
			"pkey_genseckey unknown/unsupported keytype %d\n",
			keytype);
		rc = -EINVAL;
		goto out;
	}
	memcpy(preqparm->lv1.key_type1,  "AESDATA ", 8);
	preqparm->lv2.len = sizeof(struct lv2);
	for (i = 0; i < 6; i++) {
		preqparm->lv2.keyid[i].len = sizeof(struct keyid);
		preqparm->lv2.keyid[i].attr = (i == 2 ? 0x30 : 0x10);
	}
	preqcblk->req_parml = sizeof(struct kgreqparm);

	/* fill xcrb struct */
	prep_xcrb(&xcrb, cardnr, preqcblk, prepcblk);

	/* forward xcrb with request CPRB and reply CPRB to zcrypt dd */
	rc = _zcrypt_send_cprb(&xcrb);
	if (rc) {
		DEBUG_ERR(
			"pkey_genseckey zcrypt_send_cprb (cardnr=%d domain=%d) failed with errno %d\n",
			(int) cardnr, (int) domain, rc);
		goto out;
	}

	/* check response returncode and reasoncode */
	if (prepcblk->ccp_rtcode != 0) {
		DEBUG_ERR(
			"pkey_genseckey secure key generate failure, card response %d/%d\n",
			(int) prepcblk->ccp_rtcode,
			(int) prepcblk->ccp_rscode);
		rc = -EIO;
		goto out;
	}

	/* process response cprb param block */
	prepcblk->rpl_parmb = ((u8 *) prepcblk) + sizeof(struct CPRBX);
	prepparm = (struct kgrepparm *) prepcblk->rpl_parmb;

	/* check length of the returned secure key token */
	seckeysize = prepparm->lv3.keyblock.toklen
		- sizeof(prepparm->lv3.keyblock.toklen)
		- sizeof(prepparm->lv3.keyblock.tokattr);
	if (seckeysize != SECKEYBLOBSIZE) {
		DEBUG_ERR(
			"pkey_genseckey secure token size mismatch %d != %d bytes\n",
			seckeysize, SECKEYBLOBSIZE);
		rc = -EIO;
		goto out;
	}

	/* check secure key token */
	rc = check_secaeskeytoken(prepparm->lv3.keyblock.tok, 8*keysize);
	if (rc) {
		rc = -EIO;
		goto out;
	}

	/* copy the generated secure key token */
	memcpy(seckey->seckey, prepparm->lv3.keyblock.tok, SECKEYBLOBSIZE);

out:
	free_cprbmem(mem, PARMBSIZE, 0);
	return rc;
}
EXPORT_SYMBOL(pkey_genseckey);

/*
 * Generate an AES secure key with given key value.
 */
int pkey_clr2seckey(u16 cardnr, u16 domain, u32 keytype,
		    const struct pkey_clrkey *clrkey,
		    struct pkey_seckey *seckey)
{
	int rc, keysize, seckeysize;
	u8 *mem;
	struct CPRBX *preqcblk, *prepcblk;
	struct ica_xcRB xcrb;
	struct cmreqparm {
		u8  subfunc_code[2];
		u16 rule_array_len;
		char  rule_array[8];
		struct lv1 {
			u16 len;
			u8  clrkey[0];
		} lv1;
		struct lv2 {
			u16 len;
			struct keyid {
				u16 len;
				u16 attr;
				u8  data[SECKEYBLOBSIZE];
			} keyid;
		} lv2;
	} *preqparm;
	struct lv2 *plv2;
	struct cmrepparm {
		u8  subfunc_code[2];
		u16 rule_array_len;
		struct lv3 {
			u16 len;
			u16 keyblocklen;
			struct {
				u16 toklen;
				u16 tokattr;
				u8  tok[0];
				/* ... some more data ... */
			} keyblock;
		} lv3;
	} *prepparm;

	/* get already prepared memory for 2 cprbs with param block each */
	rc = alloc_and_prep_cprbmem(PARMBSIZE, &mem, &preqcblk, &prepcblk);
	if (rc)
		return rc;

	/* fill request cprb struct */
	preqcblk->domain = domain;

	/* fill request cprb param block with CM request */
	preqparm = (struct cmreqparm *) preqcblk->req_parmb;
	memcpy(preqparm->subfunc_code, "CM", 2);
	memcpy(preqparm->rule_array, "AES     ", 8);
	preqparm->rule_array_len =
		sizeof(preqparm->rule_array_len) + sizeof(preqparm->rule_array);
	switch (keytype) {
	case PKEY_KEYTYPE_AES_128:
		keysize = 16;
		break;
	case PKEY_KEYTYPE_AES_192:
		keysize = 24;
		break;
	case PKEY_KEYTYPE_AES_256:
		keysize = 32;
		break;
	default:
		DEBUG_ERR(
			"pkey_clr2seckey unknown/unsupported keytype %d\n",
			keytype);
		rc = -EINVAL;
		goto out;
	}
	preqparm->lv1.len = sizeof(struct lv1) + keysize;
	memcpy(preqparm->lv1.clrkey, clrkey->clrkey, keysize);
	plv2 = (struct lv2 *) (((u8 *) &preqparm->lv2) + keysize);
	plv2->len = sizeof(struct lv2);
	plv2->keyid.len = sizeof(struct keyid);
	plv2->keyid.attr = 0x30;
	preqcblk->req_parml = sizeof(struct cmreqparm) + keysize;

	/* fill xcrb struct */
	prep_xcrb(&xcrb, cardnr, preqcblk, prepcblk);

	/* forward xcrb with request CPRB and reply CPRB to zcrypt dd */
	rc = _zcrypt_send_cprb(&xcrb);
	if (rc) {
		DEBUG_ERR(
			"pkey_clr2seckey zcrypt_send_cprb (cardnr=%d domain=%d) failed with errno %d\n",
			(int) cardnr, (int) domain, rc);
		goto out;
	}

	/* check response returncode and reasoncode */
	if (prepcblk->ccp_rtcode != 0) {
		DEBUG_ERR(
			"pkey_clr2seckey clear key import failure, card response %d/%d\n",
			(int) prepcblk->ccp_rtcode,
			(int) prepcblk->ccp_rscode);
		rc = -EIO;
		goto out;
	}

	/* process response cprb param block */
	prepcblk->rpl_parmb = ((u8 *) prepcblk) + sizeof(struct CPRBX);
	prepparm = (struct cmrepparm *) prepcblk->rpl_parmb;

	/* check length of the returned secure key token */
	seckeysize = prepparm->lv3.keyblock.toklen
		- sizeof(prepparm->lv3.keyblock.toklen)
		- sizeof(prepparm->lv3.keyblock.tokattr);
	if (seckeysize != SECKEYBLOBSIZE) {
		DEBUG_ERR(
			"pkey_clr2seckey secure token size mismatch %d != %d bytes\n",
			seckeysize, SECKEYBLOBSIZE);
		rc = -EIO;
		goto out;
	}

	/* check secure key token */
	rc = check_secaeskeytoken(prepparm->lv3.keyblock.tok, 8*keysize);
	if (rc) {
		rc = -EIO;
		goto out;
	}

	/* copy the generated secure key token */
	memcpy(seckey->seckey, prepparm->lv3.keyblock.tok, SECKEYBLOBSIZE);

out:
	free_cprbmem(mem, PARMBSIZE, 1);
	return rc;
}
EXPORT_SYMBOL(pkey_clr2seckey);

/*
 * Derive a proteced key from the secure key blob.
 */
int pkey_sec2protkey(u16 cardnr, u16 domain,
		     const struct pkey_seckey *seckey,
		     struct pkey_protkey *protkey)
{
	int rc;
	u8 *mem;
	struct CPRBX *preqcblk, *prepcblk;
	struct ica_xcRB xcrb;
	struct uskreqparm {
		u8  subfunc_code[2];
		u16 rule_array_len;
		struct lv1 {
			u16 len;
			u16 attr_len;
			u16 attr_flags;
		} lv1;
		struct lv2 {
			u16 len;
			u16 attr_len;
			u16 attr_flags;
			u8  token[0];	      /* cca secure key token */
		} lv2 __packed;
	} *preqparm;
	struct uskrepparm {
		u8  subfunc_code[2];
		u16 rule_array_len;
		struct lv3 {
			u16 len;
			u16 attr_len;
			u16 attr_flags;
			struct cpacfkeyblock {
				u8  version;  /* version of this struct */
				u8  flags[2];
				u8  algo;
				u8  form;
				u8  pad1[3];
				u16 keylen;
				u8  key[64];  /* the key (keylen bytes) */
				u16 keyattrlen;
				u8  keyattr[32];
				u8  pad2[1];
				u8  vptype;
				u8  vp[32];  /* verification pattern */
			} keyblock;
		} lv3 __packed;
	} *prepparm;

	/* get already prepared memory for 2 cprbs with param block each */
	rc = alloc_and_prep_cprbmem(PARMBSIZE, &mem, &preqcblk, &prepcblk);
	if (rc)
		return rc;

	/* fill request cprb struct */
	preqcblk->domain = domain;

	/* fill request cprb param block with USK request */
	preqparm = (struct uskreqparm *) preqcblk->req_parmb;
	memcpy(preqparm->subfunc_code, "US", 2);
	preqparm->rule_array_len = sizeof(preqparm->rule_array_len);
	preqparm->lv1.len = sizeof(struct lv1);
	preqparm->lv1.attr_len = sizeof(struct lv1) - sizeof(preqparm->lv1.len);
	preqparm->lv1.attr_flags = 0x0001;
	preqparm->lv2.len = sizeof(struct lv2) + SECKEYBLOBSIZE;
	preqparm->lv2.attr_len = sizeof(struct lv2)
		- sizeof(preqparm->lv2.len) + SECKEYBLOBSIZE;
	preqparm->lv2.attr_flags = 0x0000;
	memcpy(preqparm->lv2.token, seckey->seckey, SECKEYBLOBSIZE);
	preqcblk->req_parml = sizeof(struct uskreqparm) + SECKEYBLOBSIZE;

	/* fill xcrb struct */
	prep_xcrb(&xcrb, cardnr, preqcblk, prepcblk);

	/* forward xcrb with request CPRB and reply CPRB to zcrypt dd */
	rc = _zcrypt_send_cprb(&xcrb);
	if (rc) {
		DEBUG_ERR(
			"pkey_sec2protkey zcrypt_send_cprb (cardnr=%d domain=%d) failed with errno %d\n",
			(int) cardnr, (int) domain, rc);
		goto out;
	}

	/* check response returncode and reasoncode */
	if (prepcblk->ccp_rtcode != 0) {
		DEBUG_ERR(
			"pkey_sec2protkey unwrap secure key failure, card response %d/%d\n",
			(int) prepcblk->ccp_rtcode,
			(int) prepcblk->ccp_rscode);
		rc = -EIO;
		goto out;
	}
	if (prepcblk->ccp_rscode != 0) {
		DEBUG_WARN(
			"pkey_sec2protkey unwrap secure key warning, card response %d/%d\n",
			(int) prepcblk->ccp_rtcode,
			(int) prepcblk->ccp_rscode);
	}

	/* process response cprb param block */
	prepcblk->rpl_parmb = ((u8 *) prepcblk) + sizeof(struct CPRBX);
	prepparm = (struct uskrepparm *) prepcblk->rpl_parmb;

	/* check the returned keyblock */
	if (prepparm->lv3.keyblock.version != 0x01) {
		DEBUG_ERR(
			"pkey_sec2protkey reply param keyblock version mismatch 0x%02x != 0x01\n",
			(int) prepparm->lv3.keyblock.version);
		rc = -EIO;
		goto out;
	}

	/* copy the tanslated protected key */
	switch (prepparm->lv3.keyblock.keylen) {
	case 16+32:
		protkey->type = PKEY_KEYTYPE_AES_128;
		break;
	case 24+32:
		protkey->type = PKEY_KEYTYPE_AES_192;
		break;
	case 32+32:
		protkey->type = PKEY_KEYTYPE_AES_256;
		break;
	default:
		DEBUG_ERR("pkey_sec2protkey unknown/unsupported keytype %d\n",
			  prepparm->lv3.keyblock.keylen);
		rc = -EIO;
		goto out;
	}
	protkey->len = prepparm->lv3.keyblock.keylen;
	memcpy(protkey->protkey, prepparm->lv3.keyblock.key, protkey->len);

out:
	free_cprbmem(mem, PARMBSIZE, 0);
	return rc;
}
EXPORT_SYMBOL(pkey_sec2protkey);

/*
 * Create a protected key from a clear key value.
 */
int pkey_clr2protkey(u32 keytype,
		     const struct pkey_clrkey *clrkey,
		     struct pkey_protkey *protkey)
{
	long fc;
	int keysize;
	u8 paramblock[64];

	switch (keytype) {
	case PKEY_KEYTYPE_AES_128:
		keysize = 16;
		fc = CPACF_PCKMO_ENC_AES_128_KEY;
		break;
	case PKEY_KEYTYPE_AES_192:
		keysize = 24;
		fc = CPACF_PCKMO_ENC_AES_192_KEY;
		break;
	case PKEY_KEYTYPE_AES_256:
		keysize = 32;
		fc = CPACF_PCKMO_ENC_AES_256_KEY;
		break;
	default:
		DEBUG_ERR("pkey_clr2protkey unknown/unsupported keytype %d\n",
			  keytype);
		return -EINVAL;
	}

	/*
	 * Check if the needed pckmo subfunction is available.
	 * These subfunctions can be enabled/disabled by customers
	 * in the LPAR profile or may even change on the fly.
	 */
	if (!cpacf_test_func(&pckmo_functions, fc)) {
		DEBUG_ERR("%s pckmo functions not available\n", __func__);
		return -EOPNOTSUPP;
	}

	/* prepare param block */
	memset(paramblock, 0, sizeof(paramblock));
	memcpy(paramblock, clrkey->clrkey, keysize);

	/* call the pckmo instruction */
	cpacf_pckmo(fc, paramblock);

	/* copy created protected key */
	protkey->type = keytype;
	protkey->len = keysize + 32;
	memcpy(protkey->protkey, paramblock, keysize + 32);

	return 0;
}
EXPORT_SYMBOL(pkey_clr2protkey);

/*
 * query cryptographic facility from adapter
 */
static int query_crypto_facility(u16 cardnr, u16 domain,
				 const char *keyword,
				 u8 *rarray, size_t *rarraylen,
				 u8 *varray, size_t *varraylen)
{
	int rc;
	u16 len;
	u8 *mem, *ptr;
	struct CPRBX *preqcblk, *prepcblk;
	struct ica_xcRB xcrb;
	struct fqreqparm {
		u8  subfunc_code[2];
		u16 rule_array_len;
		char  rule_array[8];
		struct lv1 {
			u16 len;
			u8  data[VARDATASIZE];
		} lv1;
		u16 dummylen;
	} *preqparm;
	size_t parmbsize = sizeof(struct fqreqparm);
	struct fqrepparm {
		u8  subfunc_code[2];
		u8  lvdata[0];
	} *prepparm;

	/* get already prepared memory for 2 cprbs with param block each */
	rc = alloc_and_prep_cprbmem(parmbsize, &mem, &preqcblk, &prepcblk);
	if (rc)
		return rc;

	/* fill request cprb struct */
	preqcblk->domain = domain;

	/* fill request cprb param block with FQ request */
	preqparm = (struct fqreqparm *) preqcblk->req_parmb;
	memcpy(preqparm->subfunc_code, "FQ", 2);
	strncpy(preqparm->rule_array, keyword, sizeof(preqparm->rule_array));
	preqparm->rule_array_len =
		sizeof(preqparm->rule_array_len) + sizeof(preqparm->rule_array);
	preqparm->lv1.len = sizeof(preqparm->lv1);
	preqparm->dummylen = sizeof(preqparm->dummylen);
	preqcblk->req_parml = parmbsize;

	/* fill xcrb struct */
	prep_xcrb(&xcrb, cardnr, preqcblk, prepcblk);

	/* forward xcrb with request CPRB and reply CPRB to zcrypt dd */
	rc = _zcrypt_send_cprb(&xcrb);
	if (rc) {
		DEBUG_ERR(
			"query_crypto_facility zcrypt_send_cprb (cardnr=%d domain=%d) failed with errno %d\n",
			(int) cardnr, (int) domain, rc);
		goto out;
	}

	/* check response returncode and reasoncode */
	if (prepcblk->ccp_rtcode != 0) {
		DEBUG_ERR(
			"query_crypto_facility unwrap secure key failure, card response %d/%d\n",
			(int) prepcblk->ccp_rtcode,
			(int) prepcblk->ccp_rscode);
		rc = -EIO;
		goto out;
	}

	/* process response cprb param block */
	prepcblk->rpl_parmb = ((u8 *) prepcblk) + sizeof(struct CPRBX);
	prepparm = (struct fqrepparm *) prepcblk->rpl_parmb;
	ptr = prepparm->lvdata;

	/* check and possibly copy reply rule array */
	len = *((u16 *) ptr);
	if (len > sizeof(u16)) {
		ptr += sizeof(u16);
		len -= sizeof(u16);
		if (rarray && rarraylen && *rarraylen > 0) {
			*rarraylen = (len > *rarraylen ? *rarraylen : len);
			memcpy(rarray, ptr, *rarraylen);
		}
		ptr += len;
	}
	/* check and possible copy reply var array */
	len = *((u16 *) ptr);
	if (len > sizeof(u16)) {
		ptr += sizeof(u16);
		len -= sizeof(u16);
		if (varray && varraylen && *varraylen > 0) {
			*varraylen = (len > *varraylen ? *varraylen : len);
			memcpy(varray, ptr, *varraylen);
		}
		ptr += len;
	}

out:
	free_cprbmem(mem, parmbsize, 0);
	return rc;
}

/*
 * Fetch the current and old mkvp values via
 * query_crypto_facility from adapter.
 */
static int fetch_mkvp(u16 cardnr, u16 domain, u64 mkvp[2])
{
	int rc, found = 0;
	size_t rlen, vlen;
	u8 *rarray, *varray, *pg;

	pg = (u8 *) __get_free_page(GFP_KERNEL);
	if (!pg)
		return -ENOMEM;
	rarray = pg;
	varray = pg + PAGE_SIZE/2;
	rlen = vlen = PAGE_SIZE/2;

	rc = query_crypto_facility(cardnr, domain, "STATICSA",
				   rarray, &rlen, varray, &vlen);
	if (rc == 0 && rlen > 8*8 && vlen > 184+8) {
		if (rarray[8*8] == '2') {
			/* current master key state is valid */
			mkvp[0] = *((u64 *)(varray + 184));
			mkvp[1] = *((u64 *)(varray + 172));
			found = 1;
		}
	}

	free_page((unsigned long) pg);

	return found ? 0 : -ENOENT;
}

/* struct to hold cached mkvp info for each card/domain */
struct mkvp_info {
	struct list_head list;
	u16 cardnr;
	u16 domain;
	u64 mkvp[2];
};

/* a list with mkvp_info entries */
static LIST_HEAD(mkvp_list);
static DEFINE_SPINLOCK(mkvp_list_lock);

static int mkvp_cache_fetch(u16 cardnr, u16 domain, u64 mkvp[2])
{
	int rc = -ENOENT;
	struct mkvp_info *ptr;

	spin_lock_bh(&mkvp_list_lock);
	list_for_each_entry(ptr, &mkvp_list, list) {
		if (ptr->cardnr == cardnr &&
		    ptr->domain == domain) {
			memcpy(mkvp, ptr->mkvp, 2 * sizeof(u64));
			rc = 0;
			break;
		}
	}
	spin_unlock_bh(&mkvp_list_lock);

	return rc;
}

static void mkvp_cache_update(u16 cardnr, u16 domain, u64 mkvp[2])
{
	int found = 0;
	struct mkvp_info *ptr;

	spin_lock_bh(&mkvp_list_lock);
	list_for_each_entry(ptr, &mkvp_list, list) {
		if (ptr->cardnr == cardnr &&
		    ptr->domain == domain) {
			memcpy(ptr->mkvp, mkvp, 2 * sizeof(u64));
			found = 1;
			break;
		}
	}
	if (!found) {
		ptr = kmalloc(sizeof(*ptr), GFP_ATOMIC);
		if (!ptr) {
			spin_unlock_bh(&mkvp_list_lock);
			return;
		}
		ptr->cardnr = cardnr;
		ptr->domain = domain;
		memcpy(ptr->mkvp, mkvp, 2 * sizeof(u64));
		list_add(&ptr->list, &mkvp_list);
	}
	spin_unlock_bh(&mkvp_list_lock);
}

static void mkvp_cache_scrub(u16 cardnr, u16 domain)
{
	struct mkvp_info *ptr;

	spin_lock_bh(&mkvp_list_lock);
	list_for_each_entry(ptr, &mkvp_list, list) {
		if (ptr->cardnr == cardnr &&
		    ptr->domain == domain) {
			list_del(&ptr->list);
			kfree(ptr);
			break;
		}
	}
	spin_unlock_bh(&mkvp_list_lock);
}

static void __exit mkvp_cache_free(void)
{
	struct mkvp_info *ptr, *pnext;

	spin_lock_bh(&mkvp_list_lock);
	list_for_each_entry_safe(ptr, pnext, &mkvp_list, list) {
		list_del(&ptr->list);
		kfree(ptr);
	}
	spin_unlock_bh(&mkvp_list_lock);
}

/*
 * Search for a matching crypto card based on the Master Key
 * Verification Pattern provided inside a secure key.
 */
int pkey_findcard(const struct pkey_seckey *seckey,
		  u16 *pcardnr, u16 *pdomain, int verify)
{
	struct secaeskeytoken *t = (struct secaeskeytoken *) seckey;
	struct zcrypt_device_status_ext *device_status;
	u16 card, dom;
	u64 mkvp[2];
	int i, rc, oi = -1;

	/* mkvp must not be zero */
	if (t->mkvp == 0)
		return -EINVAL;

	/* fetch status of all crypto cards */
	device_status = kvmalloc_array(MAX_ZDEV_ENTRIES_EXT,
				       sizeof(struct zcrypt_device_status_ext),
				       GFP_KERNEL);
	if (!device_status)
		return -ENOMEM;
	zcrypt_device_status_mask_ext(device_status);

	/* walk through all crypto cards */
	for (i = 0; i < MAX_ZDEV_ENTRIES_EXT; i++) {
		card = AP_QID_CARD(device_status[i].qid);
		dom = AP_QID_QUEUE(device_status[i].qid);
		if (device_status[i].online &&
		    device_status[i].functions & 0x04) {
			/* an enabled CCA Coprocessor card */
			/* try cached mkvp */
			if (mkvp_cache_fetch(card, dom, mkvp) == 0 &&
			    t->mkvp == mkvp[0]) {
				if (!verify)
					break;
				/* verify: fetch mkvp from adapter */
				if (fetch_mkvp(card, dom, mkvp) == 0) {
					mkvp_cache_update(card, dom, mkvp);
					if (t->mkvp == mkvp[0])
						break;
				}
			}
		} else {
			/* Card is offline and/or not a CCA card. */
			/* del mkvp entry from cache if it exists */
			mkvp_cache_scrub(card, dom);
		}
	}
	if (i >= MAX_ZDEV_ENTRIES_EXT) {
		/* nothing found, so this time without cache */
		for (i = 0; i < MAX_ZDEV_ENTRIES_EXT; i++) {
			if (!(device_status[i].online &&
			      device_status[i].functions & 0x04))
				continue;
			card = AP_QID_CARD(device_status[i].qid);
			dom = AP_QID_QUEUE(device_status[i].qid);
			/* fresh fetch mkvp from adapter */
			if (fetch_mkvp(card, dom, mkvp) == 0) {
				mkvp_cache_update(card, dom, mkvp);
				if (t->mkvp == mkvp[0])
					break;
				if (t->mkvp == mkvp[1] && oi < 0)
					oi = i;
			}
		}
		if (i >= MAX_ZDEV_ENTRIES_EXT && oi >= 0) {
			/* old mkvp matched, use this card then */
			card = AP_QID_CARD(device_status[oi].qid);
			dom = AP_QID_QUEUE(device_status[oi].qid);
		}
	}
	if (i < MAX_ZDEV_ENTRIES_EXT || oi >= 0) {
		if (pcardnr)
			*pcardnr = card;
		if (pdomain)
			*pdomain = dom;
		rc = 0;
	} else
		rc = -ENODEV;

	kvfree(device_status);
	return rc;
}
EXPORT_SYMBOL(pkey_findcard);

/*
 * Find card and transform secure key into protected key.
 */
int pkey_skey2pkey(const struct pkey_seckey *seckey,
		   struct pkey_protkey *protkey)
{
	u16 cardnr, domain;
	int rc, verify;

	/*
	 * The pkey_sec2protkey call may fail when a card has been
	 * addressed where the master key was changed after last fetch
	 * of the mkvp into the cache. So first try without verify then
	 * with verify enabled (thus refreshing the mkvp for each card).
	 */
	for (verify = 0; verify < 2; verify++) {
		rc = pkey_findcard(seckey, &cardnr, &domain, verify);
		if (rc)
			continue;
		rc = pkey_sec2protkey(cardnr, domain, seckey, protkey);
		if (rc == 0)
			break;
	}

	if (rc)
		DEBUG_DBG("pkey_skey2pkey failed rc=%d\n", rc);

	return rc;
}
EXPORT_SYMBOL(pkey_skey2pkey);

/*
 * Verify key and give back some info about the key.
 */
int pkey_verifykey(const struct pkey_seckey *seckey,
		   u16 *pcardnr, u16 *pdomain,
		   u16 *pkeysize, u32 *pattributes)
{
	struct secaeskeytoken *t = (struct secaeskeytoken *) seckey;
	u16 cardnr, domain;
	u64 mkvp[2];
	int rc;

	/* check the secure key for valid AES secure key */
	rc = check_secaeskeytoken((u8 *) seckey, 0);
	if (rc)
		goto out;
	if (pattributes)
		*pattributes = PKEY_VERIFY_ATTR_AES;
	if (pkeysize)
		*pkeysize = t->bitsize;

	/* try to find a card which can handle this key */
	rc = pkey_findcard(seckey, &cardnr, &domain, 1);
	if (rc)
		goto out;

	/* check mkvp for old mkvp match */
	rc = mkvp_cache_fetch(cardnr, domain, mkvp);
	if (rc)
		goto out;
	if (t->mkvp == mkvp[1] && t->mkvp != mkvp[0]) {
		DEBUG_DBG("pkey_verifykey secure key has old mkvp\n");
		if (pattributes)
			*pattributes |= PKEY_VERIFY_ATTR_OLD_MKVP;
	}

	if (pcardnr)
		*pcardnr = cardnr;
	if (pdomain)
		*pdomain = domain;

out:
	DEBUG_DBG("pkey_verifykey rc=%d\n", rc);
	return rc;
}
EXPORT_SYMBOL(pkey_verifykey);

/*
 * Generate a random protected key
 */
int pkey_genprotkey(__u32 keytype, struct pkey_protkey *protkey)
{
	struct pkey_clrkey clrkey;
	int keysize;
	int rc;

	switch (keytype) {
	case PKEY_KEYTYPE_AES_128:
		keysize = 16;
		break;
	case PKEY_KEYTYPE_AES_192:
		keysize = 24;
		break;
	case PKEY_KEYTYPE_AES_256:
		keysize = 32;
		break;
	default:
		DEBUG_ERR("%s unknown/unsupported keytype %d\n", __func__,
			  keytype);
		return -EINVAL;
	}

	/* generate a dummy random clear key */
	get_random_bytes(clrkey.clrkey, keysize);

	/* convert it to a dummy protected key */
	rc = pkey_clr2protkey(keytype, &clrkey, protkey);
	if (rc)
		return rc;

	/* replace the key part of the protected key with random bytes */
	get_random_bytes(protkey->protkey, keysize);

	return 0;
}
EXPORT_SYMBOL(pkey_genprotkey);

/*
 * Verify if a protected key is still valid
 */
int pkey_verifyprotkey(const struct pkey_protkey *protkey)
{
	unsigned long fc;
	struct {
		u8 iv[AES_BLOCK_SIZE];
		u8 key[MAXPROTKEYSIZE];
	} param;
	u8 null_msg[AES_BLOCK_SIZE];
	u8 dest_buf[AES_BLOCK_SIZE];
	unsigned int k;

	switch (protkey->type) {
	case PKEY_KEYTYPE_AES_128:
		fc = CPACF_KMC_PAES_128;
		break;
	case PKEY_KEYTYPE_AES_192:
		fc = CPACF_KMC_PAES_192;
		break;
	case PKEY_KEYTYPE_AES_256:
		fc = CPACF_KMC_PAES_256;
		break;
	default:
		DEBUG_ERR("%s unknown/unsupported keytype %d\n", __func__,
			  protkey->type);
		return -EINVAL;
	}

	memset(null_msg, 0, sizeof(null_msg));

	memset(param.iv, 0, sizeof(param.iv));
	memcpy(param.key, protkey->protkey, sizeof(param.key));

	k = cpacf_kmc(fc | CPACF_ENCRYPT, &param, null_msg, dest_buf,
		      sizeof(null_msg));
	if (k != sizeof(null_msg)) {
		DEBUG_ERR("%s protected key is not valid\n", __func__);
		return -EKEYREJECTED;
	}

	return 0;
}
EXPORT_SYMBOL(pkey_verifyprotkey);

/*
 * Transform a non-CCA key token into a protected key
 */
static int pkey_nonccatok2pkey(const __u8 *key, __u32 keylen,
			       struct pkey_protkey *protkey)
{
	struct keytoken_header *hdr = (struct keytoken_header *)key;
	struct protaeskeytoken *t;

	switch (hdr->version) {
	case TOKVER_PROTECTED_KEY:
		if (keylen != sizeof(struct protaeskeytoken))
			return -EINVAL;

		t = (struct protaeskeytoken *)key;
		protkey->len = t->len;
		protkey->type = t->keytype;
		memcpy(protkey->protkey, t->protkey,
		       sizeof(protkey->protkey));

		return pkey_verifyprotkey(protkey);
	default:
		DEBUG_ERR("%s unknown/unsupported non-CCA token version %d\n",
			  __func__, hdr->version);
		return -EINVAL;
	}
}

/*
 * Transform a CCA internal key token into a protected key
 */
static int pkey_ccainttok2pkey(const __u8 *key, __u32 keylen,
			       struct pkey_protkey *protkey)
{
	struct keytoken_header *hdr = (struct keytoken_header *)key;

	switch (hdr->version) {
	case TOKVER_CCA_AES:
		if (keylen != sizeof(struct secaeskeytoken))
			return -EINVAL;

		return pkey_skey2pkey((struct pkey_seckey *)key,
				      protkey);
	default:
		DEBUG_ERR("%s unknown/unsupported CCA internal token version %d\n",
			  __func__, hdr->version);
		return -EINVAL;
	}
}

/*
 * Transform a key blob (of any type) into a protected key
 */
int pkey_keyblob2pkey(const __u8 *key, __u32 keylen,
		      struct pkey_protkey *protkey)
{
	struct keytoken_header *hdr = (struct keytoken_header *)key;

	if (keylen < sizeof(struct keytoken_header))
		return -EINVAL;

	switch (hdr->type) {
	case TOKTYPE_NON_CCA:
		return pkey_nonccatok2pkey(key, keylen, protkey);
	case TOKTYPE_CCA_INTERNAL:
		return pkey_ccainttok2pkey(key, keylen, protkey);
	default:
		DEBUG_ERR("%s unknown/unsupported blob type %d\n", __func__,
			  hdr->type);
		return -EINVAL;
	}
}
EXPORT_SYMBOL(pkey_keyblob2pkey);

/*
 * File io functions
 */

static long pkey_unlocked_ioctl(struct file *filp, unsigned int cmd,
				unsigned long arg)
{
	int rc;

	switch (cmd) {
	case PKEY_GENSECK: {
		struct pkey_genseck __user *ugs = (void __user *) arg;
		struct pkey_genseck kgs;

		if (copy_from_user(&kgs, ugs, sizeof(kgs)))
			return -EFAULT;
		rc = pkey_genseckey(kgs.cardnr, kgs.domain,
				    kgs.keytype, &kgs.seckey);
		DEBUG_DBG("pkey_ioctl pkey_genseckey()=%d\n", rc);
		if (rc)
			break;
		if (copy_to_user(ugs, &kgs, sizeof(kgs)))
			return -EFAULT;
		break;
	}
	case PKEY_CLR2SECK: {
		struct pkey_clr2seck __user *ucs = (void __user *) arg;
		struct pkey_clr2seck kcs;

		if (copy_from_user(&kcs, ucs, sizeof(kcs)))
			return -EFAULT;
		rc = pkey_clr2seckey(kcs.cardnr, kcs.domain, kcs.keytype,
				     &kcs.clrkey, &kcs.seckey);
		DEBUG_DBG("pkey_ioctl pkey_clr2seckey()=%d\n", rc);
		if (rc)
			break;
		if (copy_to_user(ucs, &kcs, sizeof(kcs)))
			return -EFAULT;
		memzero_explicit(&kcs, sizeof(kcs));
		break;
	}
	case PKEY_SEC2PROTK: {
		struct pkey_sec2protk __user *usp = (void __user *) arg;
		struct pkey_sec2protk ksp;

		if (copy_from_user(&ksp, usp, sizeof(ksp)))
			return -EFAULT;
		rc = pkey_sec2protkey(ksp.cardnr, ksp.domain,
				      &ksp.seckey, &ksp.protkey);
		DEBUG_DBG("pkey_ioctl pkey_sec2protkey()=%d\n", rc);
		if (rc)
			break;
		if (copy_to_user(usp, &ksp, sizeof(ksp)))
			return -EFAULT;
		break;
	}
	case PKEY_CLR2PROTK: {
		struct pkey_clr2protk __user *ucp = (void __user *) arg;
		struct pkey_clr2protk kcp;

		if (copy_from_user(&kcp, ucp, sizeof(kcp)))
			return -EFAULT;
		rc = pkey_clr2protkey(kcp.keytype,
				      &kcp.clrkey, &kcp.protkey);
		DEBUG_DBG("pkey_ioctl pkey_clr2protkey()=%d\n", rc);
		if (rc)
			break;
		if (copy_to_user(ucp, &kcp, sizeof(kcp)))
			return -EFAULT;
		memzero_explicit(&kcp, sizeof(kcp));
		break;
	}
	case PKEY_FINDCARD: {
		struct pkey_findcard __user *ufc = (void __user *) arg;
		struct pkey_findcard kfc;

		if (copy_from_user(&kfc, ufc, sizeof(kfc)))
			return -EFAULT;
		rc = pkey_findcard(&kfc.seckey,
				   &kfc.cardnr, &kfc.domain, 1);
		DEBUG_DBG("pkey_ioctl pkey_findcard()=%d\n", rc);
		if (rc)
			break;
		if (copy_to_user(ufc, &kfc, sizeof(kfc)))
			return -EFAULT;
		break;
	}
	case PKEY_SKEY2PKEY: {
		struct pkey_skey2pkey __user *usp = (void __user *) arg;
		struct pkey_skey2pkey ksp;

		if (copy_from_user(&ksp, usp, sizeof(ksp)))
			return -EFAULT;
		rc = pkey_skey2pkey(&ksp.seckey, &ksp.protkey);
		DEBUG_DBG("pkey_ioctl pkey_skey2pkey()=%d\n", rc);
		if (rc)
			break;
		if (copy_to_user(usp, &ksp, sizeof(ksp)))
			return -EFAULT;
		break;
	}
	case PKEY_VERIFYKEY: {
		struct pkey_verifykey __user *uvk = (void __user *) arg;
		struct pkey_verifykey kvk;

		if (copy_from_user(&kvk, uvk, sizeof(kvk)))
			return -EFAULT;
		rc = pkey_verifykey(&kvk.seckey, &kvk.cardnr, &kvk.domain,
				    &kvk.keysize, &kvk.attributes);
		DEBUG_DBG("pkey_ioctl pkey_verifykey()=%d\n", rc);
		if (rc)
			break;
		if (copy_to_user(uvk, &kvk, sizeof(kvk)))
			return -EFAULT;
		break;
	}
	case PKEY_GENPROTK: {
		struct pkey_genprotk __user *ugp = (void __user *) arg;
		struct pkey_genprotk kgp;

		if (copy_from_user(&kgp, ugp, sizeof(kgp)))
			return -EFAULT;
		rc = pkey_genprotkey(kgp.keytype, &kgp.protkey);
		DEBUG_DBG("%s pkey_genprotkey()=%d\n", __func__, rc);
		if (rc)
			break;
		if (copy_to_user(ugp, &kgp, sizeof(kgp)))
			return -EFAULT;
		break;
	}
	case PKEY_VERIFYPROTK: {
		struct pkey_verifyprotk __user *uvp = (void __user *) arg;
		struct pkey_verifyprotk kvp;

		if (copy_from_user(&kvp, uvp, sizeof(kvp)))
			return -EFAULT;
		rc = pkey_verifyprotkey(&kvp.protkey);
		DEBUG_DBG("%s pkey_verifyprotkey()=%d\n", __func__, rc);
		break;
	}
	case PKEY_KBLOB2PROTK: {
		struct pkey_kblob2pkey __user *utp = (void __user *) arg;
		struct pkey_kblob2pkey ktp;
		__u8 __user *ukey;
		__u8 *kkey;

		if (copy_from_user(&ktp, utp, sizeof(ktp)))
			return -EFAULT;
		if (ktp.keylen < MINKEYBLOBSIZE ||
		    ktp.keylen > MAXKEYBLOBSIZE)
			return -EINVAL;
		ukey = ktp.key;
		kkey = kmalloc(ktp.keylen, GFP_KERNEL);
		if (kkey == NULL)
			return -ENOMEM;
		if (copy_from_user(kkey, ukey, ktp.keylen)) {
			kfree(kkey);
			return -EFAULT;
		}
		rc = pkey_keyblob2pkey(kkey, ktp.keylen, &ktp.protkey);
		DEBUG_DBG("%s pkey_keyblob2pkey()=%d\n", __func__, rc);
		kfree(kkey);
		if (rc)
			break;
		if (copy_to_user(utp, &ktp, sizeof(ktp)))
			return -EFAULT;
		break;
	}
	default:
		/* unknown/unsupported ioctl cmd */
		return -ENOTTY;
	}

	return rc;
}

/*
 * Sysfs and file io operations
 */

/*
 * Sysfs attribute read function for all protected key binary attributes.
 * The implementation can not deal with partial reads, because a new random
 * protected key blob is generated with each read. In case of partial reads
 * (i.e. off != 0 or count < key blob size) -EINVAL is returned.
 */
static ssize_t pkey_protkey_aes_attr_read(u32 keytype, bool is_xts, char *buf,
					  loff_t off, size_t count)
{
	struct protaeskeytoken protkeytoken;
	struct pkey_protkey protkey;
	int rc;

	if (off != 0 || count < sizeof(protkeytoken))
		return -EINVAL;
	if (is_xts)
		if (count < 2 * sizeof(protkeytoken))
			return -EINVAL;

	memset(&protkeytoken, 0, sizeof(protkeytoken));
	protkeytoken.type = TOKTYPE_NON_CCA;
	protkeytoken.version = TOKVER_PROTECTED_KEY;
	protkeytoken.keytype = keytype;

	rc = pkey_genprotkey(protkeytoken.keytype, &protkey);
	if (rc)
		return rc;

	protkeytoken.len = protkey.len;
	memcpy(&protkeytoken.protkey, &protkey.protkey, protkey.len);

	memcpy(buf, &protkeytoken, sizeof(protkeytoken));

	if (is_xts) {
		rc = pkey_genprotkey(protkeytoken.keytype, &protkey);
		if (rc)
			return rc;

		protkeytoken.len = protkey.len;
		memcpy(&protkeytoken.protkey, &protkey.protkey, protkey.len);

		memcpy(buf + sizeof(protkeytoken), &protkeytoken,
		       sizeof(protkeytoken));

		return 2 * sizeof(protkeytoken);
	}

	return sizeof(protkeytoken);
}

static ssize_t protkey_aes_128_read(struct file *filp,
				    struct kobject *kobj,
				    struct bin_attribute *attr,
				    char *buf, loff_t off,
				    size_t count)
{
	return pkey_protkey_aes_attr_read(PKEY_KEYTYPE_AES_128, false, buf,
					  off, count);
}

static ssize_t protkey_aes_192_read(struct file *filp,
				    struct kobject *kobj,
				    struct bin_attribute *attr,
				    char *buf, loff_t off,
				    size_t count)
{
	return pkey_protkey_aes_attr_read(PKEY_KEYTYPE_AES_192, false, buf,
					  off, count);
}

static ssize_t protkey_aes_256_read(struct file *filp,
				    struct kobject *kobj,
				    struct bin_attribute *attr,
				    char *buf, loff_t off,
				    size_t count)
{
	return pkey_protkey_aes_attr_read(PKEY_KEYTYPE_AES_256, false, buf,
					  off, count);
}

static ssize_t protkey_aes_128_xts_read(struct file *filp,
					struct kobject *kobj,
					struct bin_attribute *attr,
					char *buf, loff_t off,
					size_t count)
{
	return pkey_protkey_aes_attr_read(PKEY_KEYTYPE_AES_128, true, buf,
					  off, count);
}

static ssize_t protkey_aes_256_xts_read(struct file *filp,
					struct kobject *kobj,
					struct bin_attribute *attr,
					char *buf, loff_t off,
					size_t count)
{
	return pkey_protkey_aes_attr_read(PKEY_KEYTYPE_AES_256, true, buf,
					  off, count);
}

static BIN_ATTR_RO(protkey_aes_128, sizeof(struct protaeskeytoken));
static BIN_ATTR_RO(protkey_aes_192, sizeof(struct protaeskeytoken));
static BIN_ATTR_RO(protkey_aes_256, sizeof(struct protaeskeytoken));
static BIN_ATTR_RO(protkey_aes_128_xts, 2 * sizeof(struct protaeskeytoken));
static BIN_ATTR_RO(protkey_aes_256_xts, 2 * sizeof(struct protaeskeytoken));

static struct bin_attribute *protkey_attrs[] = {
	&bin_attr_protkey_aes_128,
	&bin_attr_protkey_aes_192,
	&bin_attr_protkey_aes_256,
	&bin_attr_protkey_aes_128_xts,
	&bin_attr_protkey_aes_256_xts,
	NULL
};

static struct attribute_group protkey_attr_group = {
	.name	   = "protkey",
	.bin_attrs = protkey_attrs,
};

/*
 * Sysfs attribute read function for all secure key ccadata binary attributes.
 * The implementation can not deal with partial reads, because a new random
 * protected key blob is generated with each read. In case of partial reads
 * (i.e. off != 0 or count < key blob size) -EINVAL is returned.
 */
static ssize_t pkey_ccadata_aes_attr_read(u32 keytype, bool is_xts, char *buf,
					  loff_t off, size_t count)
{
	int rc;

	if (off != 0 || count < sizeof(struct secaeskeytoken))
		return -EINVAL;
	if (is_xts)
		if (count < 2 * sizeof(struct secaeskeytoken))
			return -EINVAL;

	rc = pkey_genseckey(-1, -1, keytype, (struct pkey_seckey *)buf);
	if (rc)
		return rc;

	if (is_xts) {
		buf += sizeof(struct pkey_seckey);
		rc = pkey_genseckey(-1, -1, keytype, (struct pkey_seckey *)buf);
		if (rc)
			return rc;

		return 2 * sizeof(struct secaeskeytoken);
	}

	return sizeof(struct secaeskeytoken);
}

static ssize_t ccadata_aes_128_read(struct file *filp,
				    struct kobject *kobj,
				    struct bin_attribute *attr,
				    char *buf, loff_t off,
				    size_t count)
{
	return pkey_ccadata_aes_attr_read(PKEY_KEYTYPE_AES_128, false, buf,
					  off, count);
}

static ssize_t ccadata_aes_192_read(struct file *filp,
				    struct kobject *kobj,
				    struct bin_attribute *attr,
				    char *buf, loff_t off,
				    size_t count)
{
	return pkey_ccadata_aes_attr_read(PKEY_KEYTYPE_AES_192, false, buf,
					  off, count);
}

static ssize_t ccadata_aes_256_read(struct file *filp,
				    struct kobject *kobj,
				    struct bin_attribute *attr,
				    char *buf, loff_t off,
				    size_t count)
{
	return pkey_ccadata_aes_attr_read(PKEY_KEYTYPE_AES_256, false, buf,
					  off, count);
}

static ssize_t ccadata_aes_128_xts_read(struct file *filp,
					struct kobject *kobj,
					struct bin_attribute *attr,
					char *buf, loff_t off,
					size_t count)
{
	return pkey_ccadata_aes_attr_read(PKEY_KEYTYPE_AES_128, true, buf,
					  off, count);
}

static ssize_t ccadata_aes_256_xts_read(struct file *filp,
					struct kobject *kobj,
					struct bin_attribute *attr,
					char *buf, loff_t off,
					size_t count)
{
	return pkey_ccadata_aes_attr_read(PKEY_KEYTYPE_AES_256, true, buf,
					  off, count);
}

static BIN_ATTR_RO(ccadata_aes_128, sizeof(struct secaeskeytoken));
static BIN_ATTR_RO(ccadata_aes_192, sizeof(struct secaeskeytoken));
static BIN_ATTR_RO(ccadata_aes_256, sizeof(struct secaeskeytoken));
static BIN_ATTR_RO(ccadata_aes_128_xts, 2 * sizeof(struct secaeskeytoken));
static BIN_ATTR_RO(ccadata_aes_256_xts, 2 * sizeof(struct secaeskeytoken));

static struct bin_attribute *ccadata_attrs[] = {
	&bin_attr_ccadata_aes_128,
	&bin_attr_ccadata_aes_192,
	&bin_attr_ccadata_aes_256,
	&bin_attr_ccadata_aes_128_xts,
	&bin_attr_ccadata_aes_256_xts,
	NULL
};

static struct attribute_group ccadata_attr_group = {
	.name	   = "ccadata",
	.bin_attrs = ccadata_attrs,
};

static const struct file_operations pkey_fops = {
	.owner		= THIS_MODULE,
	.open		= nonseekable_open,
	.llseek		= no_llseek,
	.unlocked_ioctl = pkey_unlocked_ioctl,
};

static struct miscdevice pkey_dev = {
	.name	= "pkey",
	.minor	= MISC_DYNAMIC_MINOR,
	.mode	= 0666,
	.fops	= &pkey_fops,
};

/*
 * Module init
 */
int __init pkey_init(void)
{
	cpacf_mask_t kmc_functions;
	int ret;

	/*
	 * The pckmo instruction should be available - even if we don't
	 * actually invoke it. This instruction comes with MSA 3 which
	 * is also the minimum level for the kmc instructions which
	 * are able to work with protected keys.
	 */
	if (!cpacf_query(CPACF_PCKMO, &pckmo_functions))
		return -EOPNOTSUPP;

	/* check for kmc instructions available */
	if (!cpacf_query(CPACF_KMC, &kmc_functions))
		return -EOPNOTSUPP;
	if (!cpacf_test_func(&kmc_functions, CPACF_KMC_PAES_128) ||
	    !cpacf_test_func(&kmc_functions, CPACF_KMC_PAES_192) ||
	    !cpacf_test_func(&kmc_functions, CPACF_KMC_PAES_256))
		return -EOPNOTSUPP;

	pkey_debug_init();

	ret = misc_register(&pkey_dev);
	if (ret)
		return ret;

	ret = sysfs_create_group(&pkey_dev.this_device->kobj, &protkey_attr_group);
	if (ret)
		goto out_err;

	ret = sysfs_create_group(&pkey_dev.this_device->kobj, &ccadata_attr_group);
	if (ret)
		goto out_err2;

	return 0;

out_err2:
	sysfs_remove_group(&pkey_dev.this_device->kobj, &protkey_attr_group);
out_err:
	misc_deregister(&pkey_dev);
	pkey_debug_exit();
	return ret;
}

/*
 * Module exit
 */
static void __exit pkey_exit(void)
{
	sysfs_remove_group(&pkey_dev.this_device->kobj, &protkey_attr_group);
	sysfs_remove_group(&pkey_dev.this_device->kobj, &ccadata_attr_group);
	misc_deregister(&pkey_dev);
	mkvp_cache_free();
	pkey_debug_exit();
}

module_init(pkey_init);
module_exit(pkey_exit);
