/*
 * Procedures for creating, accessing and interpreting the device tree.
 *
 * Paul Mackerras	August 1996.
 * Copyright (C) 1996-2005 Paul Mackerras.
 *
 *  Adapted for 64bit PowerPC by Dave Engebretsen and Peter Bergner.
 *    {engebret|bergner}@us.ibm.com
 *
 *  Adapted for sparc and sparc64 by David S. Miller davem@davemloft.net
 *
 *  Reconsolidated from arch/x/kernel/prom.c by Stephen Rothwell and
 *  Grant Likely.
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */
#include <linux/ctype.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/proc_fs.h>

#include "of_private.h"

LIST_HEAD(aliases_lookup);

struct device_node *of_allnodes;
EXPORT_SYMBOL(of_allnodes);
struct device_node *of_chosen;
struct device_node *of_aliases;

static struct kset *of_kset;

/*
 * Used to protect the of_aliases; but also overloaded to hold off addition of
 * nodes to sysfs
 */
DEFINE_MUTEX(of_aliases_mutex);

/* use when traversing tree through the allnext, child, sibling,
 * or parent members of struct device_node.
 */
DEFINE_RAW_SPINLOCK(devtree_lock);

int of_n_addr_cells(struct device_node *np)
{
	const __be32 *ip;

	do {
		if (np->parent)
			np = np->parent;
		ip = of_get_property(np, "#address-cells", NULL);
		if (ip)
			return be32_to_cpup(ip);
	} while (np->parent);
	/* No #address-cells property for the root node */
	return OF_ROOT_NODE_ADDR_CELLS_DEFAULT;
}
EXPORT_SYMBOL(of_n_addr_cells);

int of_n_size_cells(struct device_node *np)
{
	const __be32 *ip;

	do {
		if (np->parent)
			np = np->parent;
		ip = of_get_property(np, "#size-cells", NULL);
		if (ip)
			return be32_to_cpup(ip);
	} while (np->parent);
	/* No #size-cells property for the root node */
	return OF_ROOT_NODE_SIZE_CELLS_DEFAULT;
}
EXPORT_SYMBOL(of_n_size_cells);

#ifdef CONFIG_NUMA
int __weak of_node_to_nid(struct device_node *np)
{
	return NUMA_NO_NODE;
}
#endif

#if defined(CONFIG_OF_DYNAMIC)
/**
 *	of_node_get - Increment refcount of a node
 *	@node:	Node to inc refcount, NULL is supported to
 *		simplify writing of callers
 *
 *	Returns node.
 */
struct device_node *of_node_get(struct device_node *node)
{
	if (node)
		kobject_get(&node->kobj);
	return node;
}
EXPORT_SYMBOL(of_node_get);

static inline struct device_node *kobj_to_device_node(struct kobject *kobj)
{
	return container_of(kobj, struct device_node, kobj);
}

/**
 *	of_node_release - release a dynamically allocated node
 *	@kref:  kref element of the node to be released
 *
 *	In of_node_put() this function is passed to kref_put()
 *	as the destructor.
 */
static void of_node_release(struct kobject *kobj)
{
	struct device_node *node = kobj_to_device_node(kobj);
	struct property *prop = node->properties;

	/* We should never be releasing nodes that haven't been detached. */
	if (!of_node_check_flag(node, OF_DETACHED)) {
		pr_err("ERROR: Bad of_node_put() on %s\n", node->full_name);
		dump_stack();
		return;
	}

	if (!of_node_check_flag(node, OF_DYNAMIC))
		return;

	while (prop) {
		struct property *next = prop->next;
		kfree(prop->name);
		kfree(prop->value);
		kfree(prop);
		prop = next;

		if (!prop) {
			prop = node->deadprops;
			node->deadprops = NULL;
		}
	}
	kfree(node->full_name);
	kfree(node->data);
	kfree(node);
}

/**
 *	of_node_put - Decrement refcount of a node
 *	@node:	Node to dec refcount, NULL is supported to
 *		simplify writing of callers
 *
 */
void of_node_put(struct device_node *node)
{
	if (node)
		kobject_put(&node->kobj);
}
EXPORT_SYMBOL(of_node_put);
#else
static void of_node_release(struct kobject *kobj)
{
	/* Without CONFIG_OF_DYNAMIC, no nodes gets freed */
}
#endif /* CONFIG_OF_DYNAMIC */

struct kobj_type of_node_ktype = {
	.release = of_node_release,
};

static ssize_t of_node_property_read(struct file *filp, struct kobject *kobj,
				struct bin_attribute *bin_attr, char *buf,
				loff_t offset, size_t count)
{
	struct property *pp = container_of(bin_attr, struct property, attr);
	return memory_read_from_buffer(buf, count, &offset, pp->value, pp->length);
}

/* always return newly allocated name, caller must free after use */
static const char *safe_name(struct kobject *kobj, const char *orig_name)
{
	const char *name = orig_name;
	struct kernfs_node *kn;
	int i = 0;

	/* don't be a hero. After 16 tries give up */
	while (i < 16 && (kn = sysfs_get_dirent(kobj->sd, name))) {
		sysfs_put(kn);
		if (name != orig_name)
			kfree(name);
		name = kasprintf(GFP_KERNEL, "%s#%i", orig_name, ++i);
	}

	if (name == orig_name) {
		name = kstrdup(orig_name, GFP_KERNEL);
	} else {
		pr_warn("device-tree: Duplicate name in %s, renamed to \"%s\"\n",
			kobject_name(kobj), name);
	}
	return name;
}

static int __of_add_property_sysfs(struct device_node *np, struct property *pp)
{
	int rc;

	/* Important: Don't leak passwords */
	bool secure = strncmp(pp->name, "security-", 9) == 0;

	sysfs_bin_attr_init(&pp->attr);
	pp->attr.attr.name = safe_name(&np->kobj, pp->name);
	pp->attr.attr.mode = secure ? S_IRUSR : S_IRUGO;
	pp->attr.size = secure ? 0 : pp->length;
	pp->attr.read = of_node_property_read;

	rc = sysfs_create_bin_file(&np->kobj, &pp->attr);
	WARN(rc, "error adding attribute %s to node %s\n", pp->name, np->full_name);
	return rc;
}

static int __of_node_add(struct device_node *np)
{
	const char *name;
	struct kobject *parent;
	struct property *pp;
	int rc;

	np->kobj.kset = of_kset;
	if (!np->parent) {
		/* Nodes without parents are new top level trees */
		name = safe_name(&of_kset->kobj, "base");
		parent = NULL;
	} else {
		name = safe_name(&np->parent->kobj, kbasename(np->full_name));
		parent = &np->parent->kobj;
	}
	if (!name)
		return -ENOMEM;
	rc = kobject_add(&np->kobj, parent, "%s", name);
	kfree(name);
	if (rc)
		return rc;

	for_each_property_of_node(np, pp)
		__of_add_property_sysfs(np, pp);

	return 0;
}

int of_node_add(struct device_node *np)
{
	int rc = 0;

	BUG_ON(!of_node_is_initialized(np));

	/*
	 * Grab the mutex here so that in a race condition between of_init() and
	 * of_node_add(), node addition will still be consistent.
	 */
	mutex_lock(&of_aliases_mutex);
	if (of_kset)
		rc = __of_node_add(np);
	else
		/* This scenario may be perfectly valid, but report it anyway */
		pr_info("of_node_add(%s) before of_init()\n", np->full_name);
	mutex_unlock(&of_aliases_mutex);
	return rc;
}

#if defined(CONFIG_OF_DYNAMIC)
static void of_node_remove(struct device_node *np)
{
	struct property *pp;

	BUG_ON(!of_node_is_initialized(np));

	/* only remove properties if on sysfs */
	if (of_node_is_attached(np)) {
		for_each_property_of_node(np, pp)
			__of_sysfs_remove_bin_file(np, pp);
		kobject_del(&np->kobj);
	}

	/* finally remove the kobj_init ref */
	of_node_put(np);
}
#endif

static int __init of_init(void)
{
	struct device_node *np;

	/* Create the kset, and register existing nodes */
	mutex_lock(&of_aliases_mutex);
	of_kset = kset_create_and_add("devicetree", NULL, firmware_kobj);
	if (!of_kset) {
		mutex_unlock(&of_aliases_mutex);
		return -ENOMEM;
	}
	for_each_of_allnodes(np)
		__of_node_add(np);
	mutex_unlock(&of_aliases_mutex);

#if !defined(CONFIG_PROC_DEVICETREE)
	/* Symlink to the new tree when PROC_DEVICETREE is disabled */
	if (of_allnodes)
		proc_symlink("device-tree", NULL, "/sys/firmware/devicetree/base");
#endif /* CONFIG_PROC_DEVICETREE */

	return 0;
}
core_initcall(of_init);

static struct property *__of_find_property(const struct device_node *np,
					   const char *name, int *lenp)
{
	struct property *pp;

	if (!np)
		return NULL;

	for (pp = np->properties; pp; pp = pp->next) {
		if (of_prop_cmp(pp->name, name) == 0) {
			if (lenp)
				*lenp = pp->length;
			break;
		}
	}

	return pp;
}

struct property *of_find_property(const struct device_node *np,
				  const char *name,
				  int *lenp)
{
	struct property *pp;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	pp = __of_find_property(np, name, lenp);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);

	return pp;
}
EXPORT_SYMBOL(of_find_property);

struct device_node *__of_find_all_nodes(struct device_node *prev)
{
	struct device_node *np;
	if (!prev) {
		np = of_allnodes;
	} else if (prev->child) {
		np = prev->child;
	} else {
		/* Walk back up looking for a sibling, or the end of the structure */
		np = prev;
		while (np->parent && !np->sibling)
			np = np->parent;
		np = np->sibling; /* Might be null at the end of the tree */
	}
	return np;
}

/**
 * of_find_all_nodes - Get next node in global list
 * @prev:	Previous node or NULL to start iteration
 *		of_node_put() will be called on it
 *
 * Returns a node pointer with refcount incremented, use
 * of_node_put() on it when done.
 */
struct device_node *of_find_all_nodes(struct device_node *prev)
{
	struct device_node *np;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	np = __of_find_all_nodes(prev);
	of_node_get(np);
	of_node_put(prev);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return np;
}
EXPORT_SYMBOL(of_find_all_nodes);

/*
 * Find a property with a given name for a given node
 * and return the value.
 */
static const void *__of_get_property(const struct device_node *np,
				     const char *name, int *lenp)
{
	struct property *pp = __of_find_property(np, name, lenp);

	return pp ? pp->value : NULL;
}

/*
 * Find a property with a given name for a given node
 * and return the value.
 */
const void *of_get_property(const struct device_node *np, const char *name,
			    int *lenp)
{
	struct property *pp = of_find_property(np, name, lenp);

	return pp ? pp->value : NULL;
}
EXPORT_SYMBOL(of_get_property);

/** Checks if the given "compat" string matches one of the strings in
 * the device's "compatible" property
 */
static int __of_device_is_compatible(const struct device_node *device,
				     const char *compat)
{
	const char* cp;
	int cplen, l;

	cp = __of_get_property(device, "compatible", &cplen);
	if (cp == NULL)
		return 0;
	while (cplen > 0) {
		if (of_compat_cmp(cp, compat, strlen(compat)) == 0)
			return 1;
		l = strlen(cp) + 1;
		cp += l;
		cplen -= l;
	}

	return 0;
}

/** Checks if the given "compat" string matches one of the strings in
 * the device's "compatible" property
 */
int of_device_is_compatible(const struct device_node *device,
		const char *compat)
{
	unsigned long flags;
	int res;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	res = __of_device_is_compatible(device, compat);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return res;
}
EXPORT_SYMBOL(of_device_is_compatible);

/**
 * of_machine_is_compatible - Test root of device tree for a given compatible value
 * @compat: compatible string to look for in root node's compatible property.
 *
 * Returns true if the root node has the given value in its
 * compatible property.
 */
int of_machine_is_compatible(const char *compat)
{
	struct device_node *root;
	int rc = 0;

	root = of_find_node_by_path("/");
	if (root) {
		rc = of_device_is_compatible(root, compat);
		of_node_put(root);
	}
	return rc;
}
EXPORT_SYMBOL(of_machine_is_compatible);

/**
 *  __of_device_is_available - check if a device is available for use
 *
 *  @device: Node to check for availability, with locks already held
 *
 *  Returns 1 if the status property is absent or set to "okay" or "ok",
 *  0 otherwise
 */
static int __of_device_is_available(const struct device_node *device)
{
	const char *status;
	int statlen;

	status = __of_get_property(device, "status", &statlen);
	if (status == NULL)
		return 1;

	if (statlen > 0) {
		if (!strcmp(status, "okay") || !strcmp(status, "ok"))
			return 1;
	}

	return 0;
}

/**
 *  of_device_is_available - check if a device is available for use
 *
 *  @device: Node to check for availability
 *
 *  Returns 1 if the status property is absent or set to "okay" or "ok",
 *  0 otherwise
 */
int of_device_is_available(const struct device_node *device)
{
	unsigned long flags;
	int res;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	res = __of_device_is_available(device);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return res;

}
EXPORT_SYMBOL(of_device_is_available);

/**
 *	of_get_parent - Get a node's parent if any
 *	@node:	Node to get parent
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_get_parent(const struct device_node *node)
{
	struct device_node *np;
	unsigned long flags;

	if (!node)
		return NULL;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	np = of_node_get(node->parent);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return np;
}
EXPORT_SYMBOL(of_get_parent);

/**
 *	of_get_next_parent - Iterate to a node's parent
 *	@node:	Node to get parent of
 *
 * 	This is like of_get_parent() except that it drops the
 * 	refcount on the passed node, making it suitable for iterating
 * 	through a node's parents.
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_get_next_parent(struct device_node *node)
{
	struct device_node *parent;
	unsigned long flags;

	if (!node)
		return NULL;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	parent = of_node_get(node->parent);
	of_node_put(node);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return parent;
}
EXPORT_SYMBOL(of_get_next_parent);

static struct device_node *__of_get_next_child(const struct device_node *node,
						struct device_node *prev)
{
	struct device_node *next;

	if (!node)
		return NULL;

	next = prev ? prev->sibling : node->child;
	for (; next; next = next->sibling)
		if (of_node_get(next))
			break;
	of_node_put(prev);
	return next;
}
#define __for_each_child_of_node(parent, child) \
	for (child = __of_get_next_child(parent, NULL); child != NULL; \
	     child = __of_get_next_child(parent, child))

/**
 *	of_get_next_child - Iterate a node childs
 *	@node:	parent node
 *	@prev:	previous child of the parent node, or NULL to get first
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_get_next_child(const struct device_node *node,
	struct device_node *prev)
{
	struct device_node *next;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	next = __of_get_next_child(node, prev);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return next;
}
EXPORT_SYMBOL(of_get_next_child);

/**
 *	of_get_next_available_child - Find the next available child node
 *	@node:	parent node
 *	@prev:	previous child of the parent node, or NULL to get first
 *
 *      This function is like of_get_next_child(), except that it
 *      automatically skips any disabled nodes (i.e. status = "disabled").
 */
struct device_node *of_get_next_available_child(const struct device_node *node,
	struct device_node *prev)
{
	struct device_node *next;
	unsigned long flags;

	if (!node)
		return NULL;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	next = prev ? prev->sibling : node->child;
	for (; next; next = next->sibling) {
		if (!__of_device_is_available(next))
			continue;
		if (of_node_get(next))
			break;
	}
	of_node_put(prev);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return next;
}
EXPORT_SYMBOL(of_get_next_available_child);

/**
 *	of_get_child_by_name - Find the child node by name for a given parent
 *	@node:	parent node
 *	@name:	child name to look for.
 *
 *      This function looks for child node for given matching name
 *
 *	Returns a node pointer if found, with refcount incremented, use
 *	of_node_put() on it when done.
 *	Returns NULL if node is not found.
 */
struct device_node *of_get_child_by_name(const struct device_node *node,
				const char *name)
{
	struct device_node *child;

	for_each_child_of_node(node, child)
		if (child->name && (of_node_cmp(child->name, name) == 0))
			break;
	return child;
}
EXPORT_SYMBOL(of_get_child_by_name);

/**
 *	of_find_node_by_path - Find a node matching a full OF path
 *	@path:	The full path to match
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_find_node_by_path(const char *path)
{
	struct device_node *np;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	for_each_of_allnodes(np) {
		if (np->full_name && (of_node_cmp(np->full_name, path) == 0)
		    && of_node_get(np))
			break;
	}
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return np;
}
EXPORT_SYMBOL(of_find_node_by_path);

/**
 *	of_find_node_by_name - Find a node by its "name" property
 *	@from:	The node to start searching from or NULL, the node
 *		you pass will not be searched, only the next one
 *		will; typically, you pass what the previous call
 *		returned. of_node_put() will be called on it
 *	@name:	The name string to match against
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_find_node_by_name(struct device_node *from,
	const char *name)
{
	struct device_node *np;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	for_each_of_allnodes_from(from, np)
		if (np->name && (of_node_cmp(np->name, name) == 0)
		    && of_node_get(np))
			break;
	of_node_put(from);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return np;
}
EXPORT_SYMBOL(of_find_node_by_name);

/**
 *	of_find_node_by_type - Find a node by its "device_type" property
 *	@from:	The node to start searching from, or NULL to start searching
 *		the entire device tree. The node you pass will not be
 *		searched, only the next one will; typically, you pass
 *		what the previous call returned. of_node_put() will be
 *		called on from for you.
 *	@type:	The type string to match against
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_find_node_by_type(struct device_node *from,
	const char *type)
{
	struct device_node *np;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	for_each_of_allnodes_from(from, np)
		if (np->type && (of_node_cmp(np->type, type) == 0)
		    && of_node_get(np))
			break;
	of_node_put(from);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return np;
}
EXPORT_SYMBOL(of_find_node_by_type);

/**
 *	of_find_compatible_node - Find a node based on type and one of the
 *                                tokens in its "compatible" property
 *	@from:		The node to start searching from or NULL, the node
 *			you pass will not be searched, only the next one
 *			will; typically, you pass what the previous call
 *			returned. of_node_put() will be called on it
 *	@type:		The type string to match "device_type" or NULL to ignore
 *	@compatible:	The string to match to one of the tokens in the device
 *			"compatible" list.
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_find_compatible_node(struct device_node *from,
	const char *type, const char *compatible)
{
	struct device_node *np;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	for_each_of_allnodes_from(from, np) {
		if (type
		    && !(np->type && (of_node_cmp(np->type, type) == 0)))
			continue;
		if (__of_device_is_compatible(np, compatible) &&
		    of_node_get(np))
			break;
	}
	of_node_put(from);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return np;
}
EXPORT_SYMBOL(of_find_compatible_node);

/**
 *	of_find_node_with_property - Find a node which has a property with
 *                                   the given name.
 *	@from:		The node to start searching from or NULL, the node
 *			you pass will not be searched, only the next one
 *			will; typically, you pass what the previous call
 *			returned. of_node_put() will be called on it
 *	@prop_name:	The name of the property to look for.
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_find_node_with_property(struct device_node *from,
	const char *prop_name)
{
	struct device_node *np;
	struct property *pp;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	for_each_of_allnodes_from(from, np) {
		for (pp = np->properties; pp; pp = pp->next) {
			if (of_prop_cmp(pp->name, prop_name) == 0) {
				of_node_get(np);
				goto out;
			}
		}
	}
out:
	of_node_put(from);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return np;
}
EXPORT_SYMBOL(of_find_node_with_property);

static
const struct of_device_id *__of_match_node(const struct of_device_id *matches,
					   const struct device_node *node)
{
	if (!matches)
		return NULL;

	while (matches->name[0] || matches->type[0] || matches->compatible[0]) {
		int match = 1;
		if (matches->name[0])
			match &= node->name
				&& !strcmp(matches->name, node->name);
		if (matches->type[0])
			match &= node->type
				&& !strcmp(matches->type, node->type);
		if (matches->compatible[0])
			match &= __of_device_is_compatible(node,
							   matches->compatible);
		if (match)
			return matches;
		matches++;
	}
	return NULL;
}

/**
 * of_match_node - Tell if an device_node has a matching of_match structure
 *	@matches:	array of of device match structures to search in
 *	@node:		the of device structure to match against
 *
 *	Low level utility function used by device matching.
 */
const struct of_device_id *of_match_node(const struct of_device_id *matches,
					 const struct device_node *node)
{
	const struct of_device_id *match;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	match = __of_match_node(matches, node);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return match;
}
EXPORT_SYMBOL(of_match_node);

/**
 *	of_find_matching_node_and_match - Find a node based on an of_device_id
 *					  match table.
 *	@from:		The node to start searching from or NULL, the node
 *			you pass will not be searched, only the next one
 *			will; typically, you pass what the previous call
 *			returned. of_node_put() will be called on it
 *	@matches:	array of of device match structures to search in
 *	@match		Updated to point at the matches entry which matched
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_find_matching_node_and_match(struct device_node *from,
					const struct of_device_id *matches,
					const struct of_device_id **match)
{
	struct device_node *np;
	const struct of_device_id *m;
	unsigned long flags;

	if (match)
		*match = NULL;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	for_each_of_allnodes_from(from, np) {
		m = __of_match_node(matches, np);
		if (m && of_node_get(np)) {
			if (match)
				*match = m;
			break;
		}
	}
	of_node_put(from);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return np;
}
EXPORT_SYMBOL(of_find_matching_node_and_match);

/**
 * of_modalias_node - Lookup appropriate modalias for a device node
 * @node:	pointer to a device tree node
 * @modalias:	Pointer to buffer that modalias value will be copied into
 * @len:	Length of modalias value
 *
 * Based on the value of the compatible property, this routine will attempt
 * to choose an appropriate modalias value for a particular device tree node.
 * It does this by stripping the manufacturer prefix (as delimited by a ',')
 * from the first entry in the compatible list property.
 *
 * This routine returns 0 on success, <0 on failure.
 */
int of_modalias_node(struct device_node *node, char *modalias, int len)
{
	const char *compatible, *p;
	int cplen;

	compatible = of_get_property(node, "compatible", &cplen);
	if (!compatible || strlen(compatible) > cplen)
		return -ENODEV;
	p = strchr(compatible, ',');
	strlcpy(modalias, p ? p + 1 : compatible, len);
	return 0;
}
EXPORT_SYMBOL_GPL(of_modalias_node);

/**
 * of_find_node_by_phandle - Find a node given a phandle
 * @handle:	phandle of the node to find
 *
 * Returns a node pointer with refcount incremented, use
 * of_node_put() on it when done.
 */
struct device_node *of_find_node_by_phandle(phandle handle)
{
	struct device_node *np;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	for_each_of_allnodes(np)
		if (np->phandle == handle)
			break;
	of_node_get(np);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return np;
}
EXPORT_SYMBOL(of_find_node_by_phandle);

/**
 * of_property_count_elems_of_size - Count the number of elements in a property
 *
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @elem_size:	size of the individual element
 *
 * Search for a property in a device node and count the number of elements of
 * size elem_size in it. Returns number of elements on sucess, -EINVAL if the
 * property does not exist or its length does not match a multiple of elem_size
 * and -ENODATA if the property does not have a value.
 */
int of_property_count_elems_of_size(const struct device_node *np,
				const char *propname, int elem_size)
{
	struct property *prop = of_find_property(np, propname, NULL);

	if (!prop)
		return -EINVAL;
	if (!prop->value)
		return -ENODATA;

	if (prop->length % elem_size != 0) {
		pr_err("size of %s in node %s is not a multiple of %d\n",
		       propname, np->full_name, elem_size);
		return -EINVAL;
	}

	return prop->length / elem_size;
}
EXPORT_SYMBOL_GPL(of_property_count_elems_of_size);

/**
 * of_find_property_value_of_size
 *
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @len:	requested length of property value
 *
 * Search for a property in a device node and valid the requested size.
 * Returns the property value on success, -EINVAL if the property does not
 *  exist, -ENODATA if property does not have a value, and -EOVERFLOW if the
 * property data isn't large enough.
 *
 */
static void *of_find_property_value_of_size(const struct device_node *np,
			const char *propname, u32 len)
{
	struct property *prop = of_find_property(np, propname, NULL);

	if (!prop)
		return ERR_PTR(-EINVAL);
	if (!prop->value)
		return ERR_PTR(-ENODATA);
	if (len > prop->length)
		return ERR_PTR(-EOVERFLOW);

	return prop->value;
}

/**
 * of_property_read_u32_index - Find and read a u32 from a multi-value property.
 *
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @index:	index of the u32 in the list of values
 * @out_value:	pointer to return value, modified only if no error.
 *
 * Search for a property in a device node and read nth 32-bit value from
 * it. Returns 0 on success, -EINVAL if the property does not exist,
 * -ENODATA if property does not have a value, and -EOVERFLOW if the
 * property data isn't large enough.
 *
 * The out_value is modified only if a valid u32 value can be decoded.
 */
int of_property_read_u32_index(const struct device_node *np,
				       const char *propname,
				       u32 index, u32 *out_value)
{
	const u32 *val = of_find_property_value_of_size(np, propname,
					((index + 1) * sizeof(*out_value)));

	if (IS_ERR(val))
		return PTR_ERR(val);

	*out_value = be32_to_cpup(((__be32 *)val) + index);
	return 0;
}
EXPORT_SYMBOL_GPL(of_property_read_u32_index);

/**
 * of_property_read_u8_array - Find and read an array of u8 from a property.
 *
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @out_value:	pointer to return value, modified only if return value is 0.
 * @sz:		number of array elements to read
 *
 * Search for a property in a device node and read 8-bit value(s) from
 * it. Returns 0 on success, -EINVAL if the property does not exist,
 * -ENODATA if property does not have a value, and -EOVERFLOW if the
 * property data isn't large enough.
 *
 * dts entry of array should be like:
 *	property = /bits/ 8 <0x50 0x60 0x70>;
 *
 * The out_value is modified only if a valid u8 value can be decoded.
 */
int of_property_read_u8_array(const struct device_node *np,
			const char *propname, u8 *out_values, size_t sz)
{
	const u8 *val = of_find_property_value_of_size(np, propname,
						(sz * sizeof(*out_values)));

	if (IS_ERR(val))
		return PTR_ERR(val);

	while (sz--)
		*out_values++ = *val++;
	return 0;
}
EXPORT_SYMBOL_GPL(of_property_read_u8_array);

/**
 * of_property_read_u16_array - Find and read an array of u16 from a property.
 *
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @out_value:	pointer to return value, modified only if return value is 0.
 * @sz:		number of array elements to read
 *
 * Search for a property in a device node and read 16-bit value(s) from
 * it. Returns 0 on success, -EINVAL if the property does not exist,
 * -ENODATA if property does not have a value, and -EOVERFLOW if the
 * property data isn't large enough.
 *
 * dts entry of array should be like:
 *	property = /bits/ 16 <0x5000 0x6000 0x7000>;
 *
 * The out_value is modified only if a valid u16 value can be decoded.
 */
int of_property_read_u16_array(const struct device_node *np,
			const char *propname, u16 *out_values, size_t sz)
{
	const __be16 *val = of_find_property_value_of_size(np, propname,
						(sz * sizeof(*out_values)));

	if (IS_ERR(val))
		return PTR_ERR(val);

	while (sz--)
		*out_values++ = be16_to_cpup(val++);
	return 0;
}
EXPORT_SYMBOL_GPL(of_property_read_u16_array);

/**
 * of_property_read_u32_array - Find and read an array of 32 bit integers
 * from a property.
 *
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @out_value:	pointer to return value, modified only if return value is 0.
 * @sz:		number of array elements to read
 *
 * Search for a property in a device node and read 32-bit value(s) from
 * it. Returns 0 on success, -EINVAL if the property does not exist,
 * -ENODATA if property does not have a value, and -EOVERFLOW if the
 * property data isn't large enough.
 *
 * The out_value is modified only if a valid u32 value can be decoded.
 */
int of_property_read_u32_array(const struct device_node *np,
			       const char *propname, u32 *out_values,
			       size_t sz)
{
	const __be32 *val = of_find_property_value_of_size(np, propname,
						(sz * sizeof(*out_values)));

	if (IS_ERR(val))
		return PTR_ERR(val);

	while (sz--)
		*out_values++ = be32_to_cpup(val++);
	return 0;
}
EXPORT_SYMBOL_GPL(of_property_read_u32_array);

/**
 * of_property_read_u64 - Find and read a 64 bit integer from a property
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @out_value:	pointer to return value, modified only if return value is 0.
 *
 * Search for a property in a device node and read a 64-bit value from
 * it. Returns 0 on success, -EINVAL if the property does not exist,
 * -ENODATA if property does not have a value, and -EOVERFLOW if the
 * property data isn't large enough.
 *
 * The out_value is modified only if a valid u64 value can be decoded.
 */
int of_property_read_u64(const struct device_node *np, const char *propname,
			 u64 *out_value)
{
	const __be32 *val = of_find_property_value_of_size(np, propname,
						sizeof(*out_value));

	if (IS_ERR(val))
		return PTR_ERR(val);

	*out_value = of_read_number(val, 2);
	return 0;
}
EXPORT_SYMBOL_GPL(of_property_read_u64);

/**
 * of_property_read_u64_array - Find and read an array of 64 bit integers
 * from a property.
 *
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @out_values:	pointer to return value, modified only if return value is 0.
 * @sz:		number of array elements to read
 *
 * Search for a property in a device node and read 64-bit value(s) from
 * it. Returns 0 on success, -EINVAL if the property does not exist,
 * -ENODATA if property does not have a value, and -EOVERFLOW if the
 * property data isn't large enough.
 *
 * The out_values is modified only if a valid u64 value can be decoded.
 */
int of_property_read_u64_array(const struct device_node *np,
			       const char *propname, u64 *out_values,
			       size_t sz)
{
	const __be32 *val = of_find_property_value_of_size(np, propname,
						(sz * sizeof(*out_values)));

	if (IS_ERR(val))
		return PTR_ERR(val);

	while (sz--) {
		*out_values++ = of_read_number(val, 2);
		val += 2;
	}
	return 0;
}

/**
 * of_property_read_string - Find and read a string from a property
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @out_string:	pointer to null terminated return string, modified only if
 *		return value is 0.
 *
 * Search for a property in a device tree node and retrieve a null
 * terminated string value (pointer to data, not a copy). Returns 0 on
 * success, -EINVAL if the property does not exist, -ENODATA if property
 * does not have a value, and -EILSEQ if the string is not null-terminated
 * within the length of the property data.
 *
 * The out_string pointer is modified only if a valid string can be decoded.
 */
int of_property_read_string(struct device_node *np, const char *propname,
				const char **out_string)
{
	struct property *prop = of_find_property(np, propname, NULL);
	if (!prop)
		return -EINVAL;
	if (!prop->value)
		return -ENODATA;
	if (strnlen(prop->value, prop->length) >= prop->length)
		return -EILSEQ;
	*out_string = prop->value;
	return 0;
}
EXPORT_SYMBOL_GPL(of_property_read_string);

/**
 * of_property_match_string() - Find string in a list and return index
 * @np: pointer to node containing string list property
 * @propname: string list property name
 * @string: pointer to string to search for in string list
 *
 * This function searches a string list property and returns the index
 * of a specific string value.
 */
int of_property_match_string(struct device_node *np, const char *propname,
			     const char *string)
{
	struct property *prop = of_find_property(np, propname, NULL);
	size_t l;
	int i;
	const char *p, *end;

	if (!prop)
		return -EINVAL;
	if (!prop->value)
		return -ENODATA;

	p = prop->value;
	end = p + prop->length;

	for (i = 0; p < end; i++, p += l) {
		l = strnlen(p, end - p) + 1;
		if (p + l > end)
			return -EILSEQ;
		pr_debug("comparing %s with %s\n", string, p);
		if (strcmp(string, p) == 0)
			return i; /* Found it; return index */
	}
	return -ENODATA;
}
EXPORT_SYMBOL_GPL(of_property_match_string);

/**
 * of_property_read_string_util() - Utility helper for parsing string properties
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @out_strs:	output array of string pointers.
 * @sz:		number of array elements to read.
 * @skip:	Number of strings to skip over at beginning of list.
 *
 * Don't call this function directly. It is a utility helper for the
 * of_property_read_string*() family of functions.
 */
int of_property_read_string_helper(struct device_node *np, const char *propname,
				   const char **out_strs, size_t sz, int skip)
{
	struct property *prop = of_find_property(np, propname, NULL);
	int l = 0, i = 0;
	const char *p, *end;

	if (!prop)
		return -EINVAL;
	if (!prop->value)
		return -ENODATA;
	p = prop->value;
	end = p + prop->length;

	for (i = 0; p < end && (!out_strs || i < skip + sz); i++, p += l) {
		l = strnlen(p, end - p) + 1;
		if (p + l > end)
			return -EILSEQ;
		if (out_strs && i >= skip)
			*out_strs++ = p;
	}
	i -= skip;
	return i <= 0 ? -ENODATA : i;
}
EXPORT_SYMBOL_GPL(of_property_read_string_helper);

/**
 * of_parse_phandle - Resolve a phandle property to a device_node pointer
 * @np: Pointer to device node holding phandle property
 * @phandle_name: Name of property holding a phandle value
 * @index: For properties holding a table of phandles, this is the index into
 *         the table
 *
 * Returns the device_node pointer with refcount incremented.  Use
 * of_node_put() on it when done.
 */
struct device_node *of_parse_phandle(const struct device_node *np,
				     const char *phandle_name, int index)
{
	const __be32 *phandle;
	int size;

	phandle = of_get_property(np, phandle_name, &size);
	if ((!phandle) || (size < sizeof(*phandle) * (index + 1)))
		return NULL;

	return of_find_node_by_phandle(be32_to_cpup(phandle + index));
}
EXPORT_SYMBOL(of_parse_phandle);

/**
 * of_parse_phandle_with_args() - Find a node pointed by phandle in a list
 * @np:		pointer to a device tree node containing a list
 * @list_name:	property name that contains a list
 * @cells_name:	property name that specifies phandles' arguments count
 * @index:	index of a phandle to parse out
 * @out_args:	optional pointer to output arguments structure (will be filled)
 *
 * This function is useful to parse lists of phandles and their arguments.
 * Returns 0 on success and fills out_args, on error returns appropriate
 * errno value.
 *
 * Caller is responsible to call of_node_put() on the returned out_args->node
 * pointer.
 *
 * Example:
 *
 * phandle1: node1 {
 * 	#list-cells = <2>;
 * }
 *
 * phandle2: node2 {
 * 	#list-cells = <1>;
 * }
 *
 * node3 {
 * 	list = <&phandle1 1 2 &phandle2 3>;
 * }
 *
 * To get a device_node of the `node2' node you may call this:
 * of_parse_phandle_with_args(node3, "list", "#list-cells", 1, &args);
 */
static int __of_parse_phandle_with_args(const struct device_node *np,
					const char *list_name,
					const char *cells_name, int index,
					struct of_phandle_args *out_args)
{
	const __be32 *list, *list_end;
	int rc = 0, size, cur_index = 0;
	uint32_t count = 0;
	struct device_node *node = NULL;
	phandle phandle;

	/* Retrieve the phandle list property */
	list = of_get_property(np, list_name, &size);
	if (!list)
		return -ENOENT;
	list_end = list + size / sizeof(*list);

	/* Loop over the phandles until all the requested entry is found */
	while (list < list_end) {
		rc = -EINVAL;
		count = 0;

		/*
		 * If phandle is 0, then it is an empty entry with no
		 * arguments.  Skip forward to the next entry.
		 */
		phandle = be32_to_cpup(list++);
		if (phandle) {
			/*
			 * Find the provider node and parse the #*-cells
			 * property to determine the argument length
			 */
			node = of_find_node_by_phandle(phandle);
			if (!node) {
				pr_err("%s: could not find phandle\n",
					 np->full_name);
				goto err;
			}
			if (of_property_read_u32(node, cells_name, &count)) {
				pr_err("%s: could not get %s for %s\n",
					 np->full_name, cells_name,
					 node->full_name);
				goto err;
			}

			/*
			 * Make sure that the arguments actually fit in the
			 * remaining property data length
			 */
			if (list + count > list_end) {
				pr_err("%s: arguments longer than property\n",
					 np->full_name);
				goto err;
			}
		}

		/*
		 * All of the error cases above bail out of the loop, so at
		 * this point, the parsing is successful. If the requested
		 * index matches, then fill the out_args structure and return,
		 * or return -ENOENT for an empty entry.
		 */
		rc = -ENOENT;
		if (cur_index == index) {
			if (!phandle)
				goto err;

			if (out_args) {
				int i;
				if (WARN_ON(count > MAX_PHANDLE_ARGS))
					count = MAX_PHANDLE_ARGS;
				out_args->np = node;
				out_args->args_count = count;
				for (i = 0; i < count; i++)
					out_args->args[i] = be32_to_cpup(list++);
			} else {
				of_node_put(node);
			}

			/* Found it! return success */
			return 0;
		}

		of_node_put(node);
		node = NULL;
		list += count;
		cur_index++;
	}

	/*
	 * Unlock node before returning result; will be one of:
	 * -ENOENT : index is for empty phandle
	 * -EINVAL : parsing error on data
	 * [1..n]  : Number of phandle (count mode; when index = -1)
	 */
	rc = index < 0 ? cur_index : -ENOENT;
 err:
	if (node)
		of_node_put(node);
	return rc;
}

int of_parse_phandle_with_args(const struct device_node *np, const char *list_name,
				const char *cells_name, int index,
				struct of_phandle_args *out_args)
{
	if (index < 0)
		return -EINVAL;
	return __of_parse_phandle_with_args(np, list_name, cells_name, index, out_args);
}
EXPORT_SYMBOL(of_parse_phandle_with_args);

/**
 * of_count_phandle_with_args() - Find the number of phandles references in a property
 * @np:		pointer to a device tree node containing a list
 * @list_name:	property name that contains a list
 * @cells_name:	property name that specifies phandles' arguments count
 *
 * Returns the number of phandle + argument tuples within a property. It
 * is a typical pattern to encode a list of phandle and variable
 * arguments into a single property. The number of arguments is encoded
 * by a property in the phandle-target node. For example, a gpios
 * property would contain a list of GPIO specifies consisting of a
 * phandle and 1 or more arguments. The number of arguments are
 * determined by the #gpio-cells property in the node pointed to by the
 * phandle.
 */
int of_count_phandle_with_args(const struct device_node *np, const char *list_name,
				const char *cells_name)
{
	return __of_parse_phandle_with_args(np, list_name, cells_name, -1, NULL);
}
EXPORT_SYMBOL(of_count_phandle_with_args);

#if defined(CONFIG_OF_DYNAMIC)
static int of_property_notify(int action, struct device_node *np,
			      struct property *prop)
{
	struct of_prop_reconfig pr;

	/* only call notifiers if the node is attached */
	if (!of_node_is_attached(np))
		return 0;

	pr.dn = np;
	pr.prop = prop;
	return of_reconfig_notify(action, &pr);
}
#else
static int of_property_notify(int action, struct device_node *np,
			      struct property *prop)
{
	return 0;
}
#endif

/**
 * of_add_property - Add a property to a node
 */
int of_add_property(struct device_node *np, struct property *prop)
{
	struct property **next;
	unsigned long flags;
	int rc;

	rc = of_property_notify(OF_RECONFIG_ADD_PROPERTY, np, prop);
	if (rc)
		return rc;

	prop->next = NULL;
	raw_spin_lock_irqsave(&devtree_lock, flags);
	next = &np->properties;
	while (*next) {
		if (strcmp(prop->name, (*next)->name) == 0) {
			/* duplicate ! don't insert it */
			raw_spin_unlock_irqrestore(&devtree_lock, flags);
			return -1;
		}
		next = &(*next)->next;
	}
	*next = prop;
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	if (rc)
		return rc;

	if (of_node_is_attached(np))
		__of_add_property_sysfs(np, prop);

#ifdef CONFIG_PROC_DEVICETREE
	/* try to add to proc as well if it was initialized */
	if (np->pde)
		proc_device_tree_add_prop(np->pde, prop);
#endif /* CONFIG_PROC_DEVICETREE */

	return 0;
}

void __of_sysfs_remove_bin_file(struct device_node *np, struct property *prop)
{
	sysfs_remove_bin_file(&np->kobj, &prop->attr);
	kfree(prop->attr.attr.name);
}

/**
 * of_remove_property - Remove a property from a node.
 *
 * Note that we don't actually remove it, since we have given out
 * who-knows-how-many pointers to the data using get-property.
 * Instead we just move the property to the "dead properties"
 * list, so it won't be found any more.
 */
int of_remove_property(struct device_node *np, struct property *prop)
{
	struct property **next;
	unsigned long flags;
	int found = 0;
	int rc;

	rc = of_property_notify(OF_RECONFIG_REMOVE_PROPERTY, np, prop);
	if (rc)
		return rc;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	next = &np->properties;
	while (*next) {
		if (*next == prop) {
			/* found the node */
			*next = prop->next;
			prop->next = np->deadprops;
			np->deadprops = prop;
			found = 1;
			break;
		}
		next = &(*next)->next;
	}
	raw_spin_unlock_irqrestore(&devtree_lock, flags);

	if (!found)
		return -ENODEV;

	/* at early boot, bail hear and defer setup to of_init() */
	if (!of_kset)
		return 0;

	__of_sysfs_remove_bin_file(np, prop);

#ifdef CONFIG_PROC_DEVICETREE
	/* try to remove the proc node as well */
	if (np->pde)
		proc_device_tree_remove_prop(np->pde, prop);
#endif /* CONFIG_PROC_DEVICETREE */

	return 0;
}

/*
 * of_update_property - Update a property in a node, if the property does
 * not exist, add it.
 *
 * Note that we don't actually remove it, since we have given out
 * who-knows-how-many pointers to the data using get-property.
 * Instead we just move the property to the "dead properties" list,
 * and add the new property to the property list
 */
int of_update_property(struct device_node *np, struct property *newprop)
{
	struct property **next, *oldprop;
	unsigned long flags;
	int rc, found = 0;

	rc = of_property_notify(OF_RECONFIG_UPDATE_PROPERTY, np, newprop);
	if (rc)
		return rc;

	if (!newprop->name)
		return -EINVAL;

	oldprop = of_find_property(np, newprop->name, NULL);
	if (!oldprop)
		return of_add_property(np, newprop);

	raw_spin_lock_irqsave(&devtree_lock, flags);
	next = &np->properties;
	while (*next) {
		if (*next == oldprop) {
			/* found the node */
			newprop->next = oldprop->next;
			*next = newprop;
			oldprop->next = np->deadprops;
			np->deadprops = oldprop;
			found = 1;
			break;
		}
		next = &(*next)->next;
	}
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	if (rc)
		return rc;

	/* Update the sysfs attribute */
	if (oldprop)
		__of_sysfs_remove_bin_file(np, oldprop);
	__of_add_property_sysfs(np, newprop);

	if (!found)
		return -ENODEV;

#ifdef CONFIG_PROC_DEVICETREE
	/* try to add to proc as well if it was initialized */
	if (np->pde)
		proc_device_tree_update_prop(np->pde, newprop, oldprop);
#endif /* CONFIG_PROC_DEVICETREE */

	return 0;
}

#if defined(CONFIG_OF_DYNAMIC)
/*
 * Support for dynamic device trees.
 *
 * On some platforms, the device tree can be manipulated at runtime.
 * The routines in this section support adding, removing and changing
 * device tree nodes.
 */

static BLOCKING_NOTIFIER_HEAD(of_reconfig_chain);

int of_reconfig_notifier_register(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&of_reconfig_chain, nb);
}
EXPORT_SYMBOL_GPL(of_reconfig_notifier_register);

int of_reconfig_notifier_unregister(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&of_reconfig_chain, nb);
}
EXPORT_SYMBOL_GPL(of_reconfig_notifier_unregister);

int of_reconfig_notify(unsigned long action, void *p)
{
	int rc;

	rc = blocking_notifier_call_chain(&of_reconfig_chain, action, p);
	return notifier_to_errno(rc);
}

#ifdef CONFIG_PROC_DEVICETREE
static void of_add_proc_dt_entry(struct device_node *dn)
{
	struct proc_dir_entry *ent;

	ent = proc_mkdir(strrchr(dn->full_name, '/') + 1, dn->parent->pde);
	if (ent)
		proc_device_tree_add_node(dn, ent);
}
#else
static void of_add_proc_dt_entry(struct device_node *dn)
{
	return;
}
#endif

/**
 * of_attach_node - Plug a device node into the tree and global list.
 */
int of_attach_node(struct device_node *np)
{
	unsigned long flags;
	int rc;
	const __be32 *phandle;
	int sz;

	rc = of_reconfig_notify(OF_RECONFIG_ATTACH_NODE, np);
	if (rc)
		return rc;

	raw_spin_lock_irqsave(&devtree_lock, flags);

	np->name = __of_get_property(np, "name", NULL) ? : "<NULL>";
	np->type = __of_get_property(np, "device_type", NULL) ? : "<NULL>";

	phandle = __of_get_property(np, "phandle", &sz);
	if (!phandle)
		phandle = __of_get_property(np, "linux,phandle", &sz);
	if (IS_ENABLED(PPC_PSERIES) && !phandle)
		phandle = __of_get_property(np, "ibm,phandle", &sz);
	np->phandle = (phandle && (sz >= 4)) ? be32_to_cpup(phandle) : 0;

	np->sibling = np->parent->child;
	np->parent->child = np;
	raw_spin_unlock_irqrestore(&devtree_lock, flags);

	of_node_add(np);
	of_add_proc_dt_entry(np);
	return 0;
}

#ifdef CONFIG_PROC_DEVICETREE
static void of_remove_proc_dt_entry(struct device_node *dn)
{
	proc_remove(dn->pde);
}
#else
static void of_remove_proc_dt_entry(struct device_node *dn)
{
	return;
}
#endif

/**
 * of_detach_node - "Unplug" a node from the device tree.
 *
 * The caller must hold a reference to the node.  The memory associated with
 * the node is not freed until its refcount goes to zero.
 */
int of_detach_node(struct device_node *np)
{
	struct device_node *parent;
	unsigned long flags;
	int rc = 0;

	rc = of_reconfig_notify(OF_RECONFIG_DETACH_NODE, np);
	if (rc)
		return rc;

	raw_spin_lock_irqsave(&devtree_lock, flags);

	if (of_node_check_flag(np, OF_DETACHED)) {
		/* someone already detached it */
		raw_spin_unlock_irqrestore(&devtree_lock, flags);
		return rc;
	}

	parent = np->parent;
	if (!parent) {
		raw_spin_unlock_irqrestore(&devtree_lock, flags);
		return rc;
	}

	if (parent->child == np)
		parent->child = np->sibling;
	else {
		struct device_node *prevsib;
		for (prevsib = np->parent->child;
		     prevsib->sibling != np;
		     prevsib = prevsib->sibling)
			;
		prevsib->sibling = np->sibling;
	}

	of_node_set_flag(np, OF_DETACHED);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);

	of_remove_proc_dt_entry(np);
	of_node_remove(np);
	return rc;
}
#endif /* defined(CONFIG_OF_DYNAMIC) */

static void of_alias_add(struct alias_prop *ap, struct device_node *np,
			 int id, const char *stem, int stem_len)
{
	ap->np = np;
	ap->id = id;
	strncpy(ap->stem, stem, stem_len);
	ap->stem[stem_len] = 0;
	list_add_tail(&ap->link, &aliases_lookup);
	pr_debug("adding DT alias:%s: stem=%s id=%i node=%s\n",
		 ap->alias, ap->stem, ap->id, of_node_full_name(np));
}

/**
 * of_alias_scan - Scan all properties of 'aliases' node
 *
 * The function scans all the properties of 'aliases' node and populate
 * the the global lookup table with the properties.  It returns the
 * number of alias_prop found, or error code in error case.
 *
 * @dt_alloc:	An allocator that provides a virtual address to memory
 *		for the resulting tree
 */
void of_alias_scan(void * (*dt_alloc)(u64 size, u64 align))
{
	struct property *pp;

	of_chosen = of_find_node_by_path("/chosen");
	if (of_chosen == NULL)
		of_chosen = of_find_node_by_path("/chosen@0");
	of_aliases = of_find_node_by_path("/aliases");
	if (!of_aliases)
		return;

	for_each_property_of_node(of_aliases, pp) {
		const char *start = pp->name;
		const char *end = start + strlen(start);
		struct device_node *np;
		struct alias_prop *ap;
		int id, len;

		/* Skip those we do not want to proceed */
		if (!strcmp(pp->name, "name") ||
		    !strcmp(pp->name, "phandle") ||
		    !strcmp(pp->name, "linux,phandle"))
			continue;

		np = of_find_node_by_path(pp->value);
		if (!np)
			continue;

		/* walk the alias backwards to extract the id and work out
		 * the 'stem' string */
		while (isdigit(*(end-1)) && end > start)
			end--;
		len = end - start;

		if (kstrtoint(end, 10, &id) < 0)
			continue;

		/* Allocate an alias_prop with enough space for the stem */
		ap = dt_alloc(sizeof(*ap) + len + 1, 4);
		if (!ap)
			continue;
		memset(ap, 0, sizeof(*ap) + len + 1);
		ap->alias = start;
		of_alias_add(ap, np, id, start, len);
	}
}

/**
 * of_alias_get_id - Get alias id for the given device_node
 * @np:		Pointer to the given device_node
 * @stem:	Alias stem of the given device_node
 *
 * The function travels the lookup table to get alias id for the given
 * device_node and alias stem.  It returns the alias id if find it.
 */
int of_alias_get_id(struct device_node *np, const char *stem)
{
	struct alias_prop *app;
	int id = -ENODEV;

	mutex_lock(&of_aliases_mutex);
	list_for_each_entry(app, &aliases_lookup, link) {
		if (strcmp(app->stem, stem) != 0)
			continue;

		if (np == app->np) {
			id = app->id;
			break;
		}
	}
	mutex_unlock(&of_aliases_mutex);

	return id;
}
EXPORT_SYMBOL_GPL(of_alias_get_id);

const __be32 *of_prop_next_u32(struct property *prop, const __be32 *cur,
			       u32 *pu)
{
	const void *curv = cur;

	if (!prop)
		return NULL;

	if (!cur) {
		curv = prop->value;
		goto out_val;
	}

	curv += sizeof(*cur);
	if (curv >= prop->value + prop->length)
		return NULL;

out_val:
	*pu = be32_to_cpup(curv);
	return curv;
}
EXPORT_SYMBOL_GPL(of_prop_next_u32);

const char *of_prop_next_string(struct property *prop, const char *cur)
{
	const void *curv = cur;

	if (!prop)
		return NULL;

	if (!cur)
		return prop->value;

	curv += strlen(cur) + 1;
	if (curv >= prop->value + prop->length)
		return NULL;

	return curv;
}
EXPORT_SYMBOL_GPL(of_prop_next_string);

/**
 * of_graph_parse_endpoint() - parse common endpoint node properties
 * @node: pointer to endpoint device_node
 * @endpoint: pointer to the OF endpoint data structure
 *
 * The caller should hold a reference to @node.
 */
int of_graph_parse_endpoint(const struct device_node *node,
			    struct of_endpoint *endpoint)
{
	struct device_node *port_node = of_get_parent(node);

	memset(endpoint, 0, sizeof(*endpoint));

	endpoint->local_node = node;
	/*
	 * It doesn't matter whether the two calls below succeed.
	 * If they don't then the default value 0 is used.
	 */
	of_property_read_u32(port_node, "reg", &endpoint->port);
	of_property_read_u32(node, "reg", &endpoint->id);

	of_node_put(port_node);

	return 0;
}
EXPORT_SYMBOL(of_graph_parse_endpoint);

/**
 * of_graph_get_port_by_id() - get the port matching a given id
 * @parent: pointer to the parent device node
 * @id: id of the port
 *
 * Return: A 'port' node pointer with refcount incremented. The caller
 * has to use of_node_put() on it when done.
 */
struct device_node *of_graph_get_port_by_id(struct device_node *parent, u32 id)
{
	struct device_node *node, *port;

	node = of_get_child_by_name(parent, "ports");
	if (node)
		parent = node;

	for_each_child_of_node(parent, port) {
		u32 port_id = 0;

		if (of_node_cmp(port->name, "port") != 0)
			continue;
		of_property_read_u32(port, "reg", &port_id);
		if (id == port_id)
			break;
	}

	of_node_put(node);

	return port;
}
EXPORT_SYMBOL(of_graph_get_port_by_id);

/**
 * of_graph_get_next_endpoint() - get next endpoint node
 * @parent: pointer to the parent device node
 * @prev: previous endpoint node, or NULL to get first
 *
 * Return: An 'endpoint' node pointer with refcount incremented. Refcount
 * of the passed @prev node is not decremented, the caller have to use
 * of_node_put() on it when done.
 */
struct device_node *of_graph_get_next_endpoint(const struct device_node *parent,
					struct device_node *prev)
{
	struct device_node *endpoint;
	struct device_node *port = NULL;

	if (!parent)
		return NULL;

	if (!prev) {
		struct device_node *node;
		/*
		 * It's the first call, we have to find a port subnode
		 * within this node or within an optional 'ports' node.
		 */
		node = of_get_child_by_name(parent, "ports");
		if (node)
			parent = node;

		port = of_get_child_by_name(parent, "port");

		if (port) {
			/* Found a port, get an endpoint. */
			endpoint = of_get_next_child(port, NULL);
			of_node_put(port);
		} else {
			endpoint = NULL;
		}

		if (!endpoint)
			pr_err("%s(): no endpoint nodes specified for %s\n",
			       __func__, parent->full_name);
		of_node_put(node);
	} else {
		port = of_get_parent(prev);
		if (!port)
			/* Hm, has someone given us the root node ?... */
			return NULL;

		/* Avoid dropping prev node refcount to 0. */
		of_node_get(prev);
		endpoint = of_get_next_child(port, prev);
		if (endpoint) {
			of_node_put(port);
			return endpoint;
		}

		/* No more endpoints under this port, try the next one. */
		do {
			port = of_get_next_child(parent, port);
			if (!port)
				return NULL;
		} while (of_node_cmp(port->name, "port"));

		/* Pick up the first endpoint in this port. */
		endpoint = of_get_next_child(port, NULL);
		of_node_put(port);
	}

	return endpoint;
}
EXPORT_SYMBOL(of_graph_get_next_endpoint);

/**
 * of_graph_get_endpoint_by_regs() - get endpoint node of specific identifiers
 * @parent: pointer to the parent device node
 * @port_reg: identifier (value of reg property) of the parent port node
 * @reg: identifier (value of reg property) of the endpoint node
 *
 * Return: An 'endpoint' node pointer which is identified by reg and at the same
 * is the child of a port node identified by port_reg. reg and port_reg are
 * ignored when they are -1.
 */
struct device_node *of_graph_get_endpoint_by_regs(
	const struct device_node *parent, int port_reg, int reg)
{
	struct of_endpoint endpoint;
	struct device_node *node, *prev_node = NULL;

	while (1) {
		node = of_graph_get_next_endpoint(parent, prev_node);
		of_node_put(prev_node);
		if (!node)
			break;

		of_graph_parse_endpoint(node, &endpoint);
		if (((port_reg == -1) || (endpoint.port == port_reg)) &&
			((reg == -1) || (endpoint.id == reg)))
			return node;

		prev_node = node;
	}

	return NULL;
}

/**
 * of_graph_get_remote_endpoint() - get remote endpoint node
 * @node: pointer to a local endpoint device_node
 *
 * Return: Remote endpoint node associated with remote endpoint node linked
 *	   to @node. Use of_node_put() on it when done.
 */
struct device_node *of_graph_get_remote_endpoint(const struct device_node *node)
{
	/* Get remote endpoint node. */
	return of_parse_phandle(node, "remote-endpoint", 0);
}
EXPORT_SYMBOL(of_graph_get_remote_endpoint);

/**
 * of_graph_get_port_parent() - get port's parent node
 * @node: pointer to a local endpoint device_node
 *
 * Return: device node associated with endpoint node linked
 *	   to @node. Use of_node_put() on it when done.
 */
struct device_node *of_graph_get_port_parent(struct device_node *node)
{
	unsigned int depth;

	/* Walk 3 levels up only if there is 'ports' node. */
	for (depth = 3; depth && node; depth--) {
		node = of_get_next_parent(node);
		if (depth == 2 && of_node_cmp(node->name, "ports"))
			break;
	}
	return node;
}
EXPORT_SYMBOL(of_graph_get_port_parent);

/**
 * of_graph_get_remote_port_parent() - get remote port's parent node
 * @node: pointer to a local endpoint device_node
 *
 * Return: Remote device node associated with remote endpoint node linked
 *	   to @node. Use of_node_put() on it when done.
 */
struct device_node *of_graph_get_remote_port_parent(
			       const struct device_node *node)
{
	struct device_node *np;

	/* Get remote endpoint node. */
	np = of_graph_get_remote_endpoint(node);

	return of_graph_get_port_parent(np);
}
EXPORT_SYMBOL(of_graph_get_remote_port_parent);

/**
 * of_graph_get_remote_port() - get remote port node
 * @node: pointer to a local endpoint device_node
 *
 * Return: Remote port node associated with remote endpoint node linked
 *	   to @node. Use of_node_put() on it when done.
 */
struct device_node *of_graph_get_remote_port(const struct device_node *node)
{
	struct device_node *np;

	/* Get remote endpoint node. */
	np = of_graph_get_remote_endpoint(node);
	if (!np)
		return NULL;
	return of_get_next_parent(np);
}
EXPORT_SYMBOL(of_graph_get_remote_port);

int of_graph_get_endpoint_count(const struct device_node *np)
{
	struct device_node *endpoint;
	int num = 0;

	for_each_endpoint_of_node(np, endpoint)
		num++;

	return num;
}
EXPORT_SYMBOL(of_graph_get_endpoint_count);

/**
 * of_graph_get_remote_node() - get remote parent device_node for given port/endpoint
 * @node: pointer to parent device_node containing graph port/endpoint
 * @port: identifier (value of reg property) of the parent port node
 * @endpoint: identifier (value of reg property) of the endpoint node
 *
 * Return: Remote device node associated with remote endpoint node linked
 *	   to @node. Use of_node_put() on it when done.
 */
struct device_node *of_graph_get_remote_node(const struct device_node *node,
					     u32 port, u32 endpoint)
{
	struct device_node *endpoint_node, *remote;

	endpoint_node = of_graph_get_endpoint_by_regs(node, port, endpoint);
	if (!endpoint_node) {
		pr_debug("no valid endpoint (%d, %d) for node %s\n",
			 port, endpoint, node->full_name);
		return NULL;
	}

	remote = of_graph_get_remote_port_parent(endpoint_node);
	of_node_put(endpoint_node);
	if (!remote) {
		pr_debug("no valid remote node\n");
		return NULL;
	}

	if (!of_device_is_available(remote)) {
		pr_debug("not available for remote node\n");
		return NULL;
	}

	return remote;
}
EXPORT_SYMBOL(of_graph_get_remote_node);
