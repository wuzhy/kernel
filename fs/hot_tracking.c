/*
 * fs/hot_tracking.c
 *
 * Copyright (C) 2013 IBM Corp. All rights reserved.
 * Written by Zhi Yong Wu <wuzhy@linux.vnet.ibm.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 */

#include <linux/list.h>
#include <linux/err.h>
#include <linux/spinlock.h>
#include "hot_tracking.h"

/* kmem_cache pointers for slab caches */
static struct kmem_cache *hot_inode_item_cachep __read_mostly;
static struct kmem_cache *hot_range_item_cachep __read_mostly;

static void hot_range_item_init(struct hot_range_item *hr,
			struct hot_inode_item *he, loff_t start)
{
	kref_init(&hr->refs);
	hr->start = start;
	hr->len = 1 << RANGE_BITS;
	hr->hot_inode = he;
}

static void hot_range_item_free_cb(struct rcu_head *head)
{
	struct hot_range_item *hr = container_of(head,
				struct hot_range_item, rcu);

	kmem_cache_free(hot_range_item_cachep, hr);
}

static void hot_range_item_free(struct kref *kref)
{
	struct hot_range_item *hr = container_of(kref,
				struct hot_range_item, refs);

	rb_erase(&hr->rb_node, &hr->hot_inode->hot_range_tree);

	call_rcu(&hr->rcu, hot_range_item_free_cb);
}

static void hot_range_item_get(struct hot_range_item *hr)
{
        kref_get(&hr->refs);
}

/*
 * Drops the reference out on hot_range_item by one
 * and free the structure if the reference count hits zero
 */
static void hot_range_item_put(struct hot_range_item *hr)
{
        kref_put(&hr->refs, hot_range_item_free);
}

/*
 * Free the entire hot_range_tree.
 */
static void hot_range_tree_free(struct hot_inode_item *he)
{
	struct rb_node *node;
	struct hot_range_item *hr;

	/* Free hot inode and range trees on fs root */
	spin_lock(&he->i_lock);
	node = rb_first(&he->hot_range_tree);
	while (node) {
		hr = rb_entry(node, struct hot_range_item, rb_node);
		node = rb_next(node);
		hot_range_item_put(hr);
	}
	spin_unlock(&he->i_lock);
}

static void hot_inode_item_init(struct hot_inode_item *he,
			struct hot_info *root, u64 ino)
{
	kref_init(&he->refs);
	he->ino = ino;
	he->hot_root = root;
	spin_lock_init(&he->i_lock);
}

static void hot_inode_item_free_cb(struct rcu_head *head)
{
	struct hot_inode_item *he = container_of(head,
				struct hot_inode_item, rcu);

	kmem_cache_free(hot_inode_item_cachep, he);
}

static void hot_inode_item_free(struct kref *kref)
{
	struct hot_inode_item *he = container_of(kref,
				struct hot_inode_item, refs);

	rb_erase(&he->rb_node, &he->hot_root->hot_inode_tree);
	hot_range_tree_free(he);

	call_rcu(&he->rcu, hot_inode_item_free_cb);
}

static void hot_inode_item_get(struct hot_inode_item *he)
{
        kref_get(&he->refs);
}

/*
 * Drops the reference out on hot_inode_item by one
 * and free the structure if the reference count hits zero
 */
void hot_inode_item_put(struct hot_inode_item *he)
{
        kref_put(&he->refs, hot_inode_item_free);
}

/*
 * Initialize kmem cache for hot_inode_item and hot_range_item.
 */
void __init hot_cache_init(void)
{
	hot_inode_item_cachep = KMEM_CACHE(hot_inode_item,
			SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD);
	if (!hot_inode_item_cachep)
		return;

	hot_range_item_cachep = KMEM_CACHE(hot_range_item,
			SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD);
	if (!hot_range_item_cachep)
		kmem_cache_destroy(hot_inode_item_cachep);
}

static struct hot_info *hot_tree_init(struct super_block *sb)
{
	struct hot_info *root;
	int i, j;

	root = kzalloc(sizeof(struct hot_info), GFP_NOFS);
	if (!root) {
		printk(KERN_ERR "%s: Failed to malloc memory for "
				"hot_info\n", __func__);
		return ERR_PTR(-ENOMEM);
	}

	root->hot_inode_tree = RB_ROOT;
	spin_lock_init(&root->t_lock);

	return root;
}

/*
 * Frees the entire hot tree.
 */
static void hot_tree_exit(struct hot_info *root)
{
	struct hot_inode_item *he;
	struct rb_node *node;

	spin_lock(&root->t_lock);
	node = rb_first(&root->hot_inode_tree);
	while (node) {
		he = rb_entry(node, struct hot_inode_item, rb_node);
		node = rb_next(node);
		hot_inode_item_put(he);
	}
	spin_unlock(&root->t_lock);
}

/*
 * Initialize the data structures for hot tracking.
 * This function will be called by *_fill_super()
 * when filesystem is mounted.
 */
int hot_track_init(struct super_block *sb)
{
	struct hot_info *root;
	int ret = 0;

	if (!hot_inode_item_cachep || !hot_range_item_cachep) {
		ret = -ENOMEM;
		goto err;
	}

	root = hot_tree_init(sb);
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		goto err;
	}

	sb->s_hot_root = root;
	sb->s_flags |= MS_HOTTRACK;

	printk(KERN_INFO "VFS: Turning on hot tracking\n");

	return ret;

err:
	sb->s_hot_root = NULL;

	printk(KERN_ERR "VFS: Fail to turn on hot tracking\n");

	return ret;
}
EXPORT_SYMBOL(hot_track_init);

/*
 * This function will be called by *_put_super()
 * when filesystem is umounted, or also by *_fill_super()
 * in some exceptional cases.
 */
void hot_track_exit(struct super_block *sb)
{
	struct hot_info *root = sb->s_hot_root;

	sb->s_hot_root = NULL;
	sb->s_flags &= ~MS_HOTTRACK;
	hot_tree_exit(root);
	rcu_barrier();
	kfree(root);
}
EXPORT_SYMBOL(hot_track_exit);
