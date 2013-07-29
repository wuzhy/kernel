/*
 *  include/linux/hot_tracking.h
 *
 * This file has definitions for VFS hot tracking
 * structures etc.
 *
 * Copyright (C) 2013 IBM Corp. All rights reserved.
 * Written by Zhi Yong Wu <wuzhy@linux.vnet.ibm.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 */

#ifndef _LINUX_HOTTRACK_H
#define _LINUX_HOTTRACK_H

#include <linux/types.h>
#include <linux/slab.h>
#include <linux/rbtree.h>
#include <linux/kref.h>
#include <linux/fs.h>

#define MAP_BITS 8
#define MAP_SIZE (1 << MAP_BITS)

/* values for hot_freq flags */
enum {
	TYPE_INODE = 0,
	TYPE_RANGE,
	MAX_TYPES,
};

/*
 * A frequency data struct holds values that are used to
 * determine temperature of files and file ranges. These structs
 * are members of hot_inode_item and hot_range_item
 */
struct hot_freq {
	struct timespec last_read_time;
	struct timespec last_write_time;
	u32 nr_reads;
	u32 nr_writes;
	u64 avg_delta_reads;
	u64 avg_delta_writes;
	u32 last_temp;
};

/* An item representing an inode and its access frequency */
struct hot_inode_item {
	struct hot_freq freq;           /* frequency data */
	struct kref refs;
	struct rb_node rb_node;         /* rbtree index */
	struct rcu_head rcu;
	struct list_head track_list;    /* link to *_map[] */
	struct rb_root hot_range_tree;	/* tree of ranges */
	spinlock_t i_lock;		/* protect above tree */
	struct hot_info *hot_root;	/* associated hot_info */
	u64 ino;			/* inode number from inode */
};

/*
 * An item representing a range inside of
 * an inode whose frequency is being tracked
 */
struct hot_range_item {
	struct hot_freq freq;                   /* frequency data */
	struct kref refs;
	struct rb_node rb_node;                 /* rbtree index */
	struct rcu_head rcu;
	struct list_head track_list;            /* link to *_map[] */
	struct hot_inode_item *hot_inode;	/* associated hot_inode_item */
	loff_t start;				/* offset in bytes */
	size_t len;				/* length in bytes */
};

struct hot_info {
	struct rb_root hot_inode_tree;
	struct list_head hot_map[MAX_TYPES][MAP_SIZE];	/* map of inode temp */
	spinlock_t t_lock;		/* protect tree and map for inode item */
	spinlock_t m_lock;		/* protect map for range item */
	atomic_long_t hot_cnt;
	struct workqueue_struct *update_wq;
	struct delayed_work update_work;
	struct shrinker hot_shrink;
};

extern int hot_track_init(struct super_block *sb);
extern void hot_track_exit(struct super_block *sb);
extern void hot_freqs_update(struct inode *inode, loff_t start,
			size_t len, int rw);

#endif  /* _LINUX_HOTTRACK_H */
