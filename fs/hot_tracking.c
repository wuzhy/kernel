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
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/list_sort.h>
#include <linux/limits.h>
#include "hot_tracking.h"

/* kmem_cache pointers for slab caches */
static struct kmem_cache *hot_inode_item_cachep __read_mostly;
static struct kmem_cache *hot_range_item_cachep __read_mostly;

static void hot_inode_item_free(struct kref *kref);

static void hot_comm_item_free_cb(struct rcu_head *head)
{
	struct hot_comm_item *ci = container_of(head,
				struct hot_comm_item, c_rcu);

	if (ci->hot_freq_data.flags == TYPE_RANGE) {
		struct hot_range_item *hr = container_of(ci,
				struct hot_range_item, hot_range);
		kmem_cache_free(hot_range_item_cachep, hr);
	} else {
		struct hot_inode_item *he = container_of(ci,
				struct hot_inode_item, hot_inode);
		kmem_cache_free(hot_inode_item_cachep, he);
	}
}

static void hot_range_item_free(struct kref *kref)
{
	struct hot_comm_item *ci = container_of(kref,
		struct hot_comm_item, refs);
	struct hot_range_item *hr = container_of(ci,
		struct hot_range_item, hot_range);

	hr->hot_inode = NULL;

	call_rcu(&hr->hot_range.c_rcu, hot_comm_item_free_cb);
}

/*
 * Drops the reference out on hot_comm_item by one
 * and free the structure if the reference count hits zero
 */
void hot_comm_item_put(struct hot_comm_item *ci)
{
	kref_put(&ci->refs, (ci->hot_freq_data.flags == TYPE_RANGE) ?
			hot_range_item_free : hot_inode_item_free);
}
EXPORT_SYMBOL_GPL(hot_comm_item_put);

static void hot_comm_item_unlink(struct hot_info *root,
				struct hot_comm_item *ci)
{
	if (!test_and_set_bit(HOT_DELETING, &ci->delete_flag)) {
		hot_comm_item_put(ci);
	}
}

/*
 * Frees the entire hot_range_tree.
 */
static void hot_range_tree_free(struct hot_inode_item *he)
{
	struct hot_info *root = he->hot_root;
	struct rb_node *node;
	struct hot_comm_item *ci;

	/* Free hot inode and range trees on fs root */
	rcu_read_lock();
	node = rb_first(&he->hot_range_tree);
	while (node) {
		ci = rb_entry(node, struct hot_comm_item, rb_node);
		node = rb_next(node);
		hot_comm_item_unlink(root, ci);
	}
	rcu_read_unlock();

}

static void hot_inode_item_free(struct kref *kref)
{
	struct hot_comm_item *ci = container_of(kref,
			struct hot_comm_item, refs);
	struct hot_inode_item *he = container_of(ci,
			struct hot_inode_item, hot_inode);

	hot_range_tree_free(he);
	he->hot_root = NULL;

	call_rcu(&he->hot_inode.c_rcu, hot_comm_item_free_cb);
}

/*
 * Initialize kmem cache for hot_inode_item and hot_range_item.
 */
void __init hot_cache_init(void)
{
	hot_inode_item_cachep = kmem_cache_create("hot_inode_item",
			sizeof(struct hot_inode_item), 0,
			SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD,
			NULL);
	if (!hot_inode_item_cachep)
		return;

	hot_range_item_cachep = kmem_cache_create("hot_range_item",
			sizeof(struct hot_range_item), 0,
			SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD,
			NULL);
	if (!hot_range_item_cachep)
		kmem_cache_destroy(hot_inode_item_cachep);
}
EXPORT_SYMBOL_GPL(hot_cache_init);

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
	spin_lock_init(&root->m_lock);

	for (i = 0; i < MAP_SIZE; i++) {
		for (j = 0; j < MAX_TYPES; j++)
			INIT_LIST_HEAD(&root->hot_map[j][i]);
	}

	return root;
}

/*
 * Frees the entire hot tree.
 */
static void hot_tree_exit(struct hot_info *root)
{
	struct rb_node *node;
	struct hot_comm_item *ci;

	rcu_read_lock();
	node = rb_first(&root->hot_inode_tree);
	while (node) {
		struct hot_inode_item *he;
		ci = rb_entry(node, struct hot_comm_item, rb_node);
		he = container_of(ci, struct hot_inode_item, hot_inode);
		node = rb_next(node);
		hot_comm_item_unlink(root, &he->hot_inode);
	}
	rcu_read_unlock();
}

/*
 * Initialize the data structures for hot tracking.
 * This function will be called by *_fill_super()
 * when filesystem is mounted.
 */
int hot_track_init(struct super_block *sb)
{
	struct hot_info *root;

	root = hot_tree_init(sb);
	if (IS_ERR(root))
		return PTR_ERR(root);

	sb->s_hot_root = root;

	printk(KERN_INFO "VFS: Turning on hot data tracking\n");

	return 0;
}
EXPORT_SYMBOL_GPL(hot_track_init);

/*
 * This function will be called by *_put_super()
 * when filesystem is umounted, or also by *_fill_super()
 * in some exceptional cases.
 */
void hot_track_exit(struct super_block *sb)
{
	struct hot_info *root = sb->s_hot_root;

	hot_tree_exit(root);
	sb->s_hot_root = NULL;
	kfree(root);
}
EXPORT_SYMBOL_GPL(hot_track_exit);
