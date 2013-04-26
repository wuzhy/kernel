/*
 *  include/linux/hot_tracking.h
 *
 * This file has definitions for VFS hot data tracking
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

#ifdef __KERNEL__

#include <linux/rbtree.h>
#include <linux/kref.h>
#include <linux/fs.h>

#define MAP_BITS 8
#define MAP_SIZE (1 << MAP_BITS)

/* values for hot_freq_data flags */
enum {
	TYPE_INODE = 0,
	TYPE_RANGE,
	MAX_TYPES,
};

enum {
	HOT_DELETING,
	HOT_IN_LIST,
};

/*
 * A frequency data struct holds values that are used to
 * determine temperature of files and file ranges. These structs
 * are members of hot_inode_item and hot_range_item
 */
struct hot_freq_data {
	struct timespec last_read_time;
	struct timespec last_write_time;
	u32 nr_reads;
	u32 nr_writes;
	u64 avg_delta_reads;
	u64 avg_delta_writes;
	u32 flags;
	u32 last_temp;
};

/* The common info for both following structures */
struct hot_comm_item {
	struct hot_freq_data hot_freq_data;	/* frequency data */
	struct kref refs;
	struct rb_node rb_node;			/* rbtree index */
	unsigned long delete_flag;
	struct rcu_head c_rcu;
	struct list_head track_list;		/* link to *_map[] */
};

/* An item representing an inode and its access frequency */
struct hot_inode_item {
	struct hot_comm_item hot_inode; /* node in hot_inode_tree */
	struct rb_root hot_range_tree;	/* tree of ranges */
	spinlock_t i_lock;		/* protect above tree */
	struct hot_info *hot_root;	/* associated hot_info */
	u64 i_ino;			/* inode number from inode */
};

/*
 * An item representing a range inside of
 * an inode whose frequency is being tracked
 */
struct hot_range_item {
	struct hot_comm_item hot_range;
	struct hot_inode_item *hot_inode;	/* associated hot_inode_item */
	loff_t start;				/* offset in bytes */
	size_t len;				/* length in bytes */
	int storage_type;			/* type of storage */
};

struct hot_info {
	struct rb_root hot_inode_tree;
	spinlock_t t_lock;				/* protect above tree */
	struct list_head hot_map[MAX_TYPES][MAP_SIZE];	/* map of inode temp */
	spinlock_t m_lock;
	struct workqueue_struct *update_wq;
	struct delayed_work update_work;
};

extern void __init hot_cache_init(void);
extern int hot_track_init(struct super_block *sb);
extern void hot_track_exit(struct super_block *sb);
extern void hot_comm_item_put(struct hot_comm_item *ci);
extern void hot_update_freqs(struct inode *inode, loff_t start,
				size_t len, int rw);
extern struct hot_inode_item *hot_inode_item_lookup(struct hot_info *root,
						u64 ino, int alloc);
extern struct hot_range_item *hot_range_item_lookup(struct hot_inode_item *he,
						loff_t start, int alloc);
extern void hot_inode_item_delete(struct inode *inode);

static inline u64 hot_shift(u64 counter, u32 bits, bool dir)
{
	if (dir)
		return counter << bits;
	else
		return counter >> bits;
}

static inline void hot_comm_item_get(struct hot_comm_item *ci)
{
	kref_get(&ci->refs);
}

#endif /* __KERNEL__ */

#endif  /* _LINUX_HOTTRACK_H */
