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

static void hot_comm_item_init(struct hot_comm_item *ci, int type)
{
	kref_init(&ci->refs);
	clear_bit(HOT_IN_LIST, &ci->delete_flag);
	clear_bit(HOT_DELETING, &ci->delete_flag);
	INIT_LIST_HEAD(&ci->track_list);
	memset(&ci->hot_freq_data, 0, sizeof(struct hot_freq_data));
	ci->hot_freq_data.avg_delta_reads = (u64) -1;
	ci->hot_freq_data.avg_delta_writes = (u64) -1;
	ci->hot_freq_data.flags = type;
}

static void hot_range_item_init(struct hot_range_item *hr,
			struct hot_inode_item *he, loff_t start)
{
	hr->start = start;
	hr->len = hot_shift(1, RANGE_BITS, true);
	hr->hot_inode = he;
	hr->storage_type = -1;
	hot_comm_item_init(&hr->hot_range, TYPE_RANGE);
}

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

/*
 * root->t_lock or he->i_lock, and root->m_lock
 * are acquired in this function
 */
static void hot_comm_item_unlink(struct hot_info *root,
				struct hot_comm_item *ci)
{
	if (!test_and_set_bit(HOT_DELETING, &ci->delete_flag)) {
		bool flag = false;
		spin_lock(&root->m_lock);
		if (test_and_clear_bit(HOT_IN_LIST, &ci->delete_flag)) {
			list_del_rcu(&ci->track_list);
			flag = true;
		}
		spin_unlock(&root->m_lock);

		if (flag) {
			atomic_dec(&root->hot_map_nr);
			hot_comm_item_put(ci);
		}

		if (ci->hot_freq_data.flags == TYPE_RANGE) {
			struct hot_range_item *hr = container_of(ci,
					struct hot_range_item, hot_range);
			struct hot_inode_item *he = hr->hot_inode;

			spin_lock(&he->i_lock);
			rb_erase(&ci->rb_node, &he->hot_range_tree);
			spin_unlock(&he->i_lock);
		} else {
			spin_lock(&root->t_lock);
			rb_erase(&ci->rb_node, &root->hot_inode_tree);
			spin_unlock(&root->t_lock);
		}

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

static void hot_inode_item_init(struct hot_inode_item *he,
			struct hot_info *hot_root, u64 ino)
{
	he->i_ino = ino;
	he->hot_root = hot_root;
	spin_lock_init(&he->i_lock);
	hot_comm_item_init(&he->hot_inode, TYPE_INODE);
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

/* root->t_lock is acquired in this function. */
struct hot_inode_item
*hot_inode_item_lookup(struct hot_info *root, u64 ino, int alloc)
{
	struct rb_node **p;
	struct rb_node *parent = NULL;
	struct hot_comm_item *ci;
	struct hot_inode_item *he, *he_new = NULL;

	/* walk tree to find insertion point */
redo:
	spin_lock(&root->t_lock);
	p = &root->hot_inode_tree.rb_node;
	while (*p) {
		parent = *p;
		ci = rb_entry(parent, struct hot_comm_item, rb_node);
		he = container_of(ci, struct hot_inode_item, hot_inode);
		if (ino < he->i_ino)
			p = &(*p)->rb_left;
		else if (ino > he->i_ino)
			p = &(*p)->rb_right;
		else {
			hot_comm_item_get(&he->hot_inode);
			spin_unlock(&root->t_lock);
			if (he_new)
				/*
				 * Lost the race. Somebody else inserted
				 * the item for the inode. Free the
				 * newly allocated item.
				 */
				kmem_cache_free(hot_inode_item_cachep, he_new);

			if (test_bit(HOT_DELETING, &he->hot_inode.delete_flag))
				return ERR_PTR(-ENOENT);

			return he;
		}
	}

	if (he_new) {
		rb_link_node(&he_new->hot_inode.rb_node, parent, p);
		rb_insert_color(&he_new->hot_inode.rb_node,
				&root->hot_inode_tree);
		hot_comm_item_get(&he_new->hot_inode);
		spin_unlock(&root->t_lock);
		return he_new;
	}
	spin_unlock(&root->t_lock);

	if (!alloc)
		return ERR_PTR(-ENOENT);

	he_new = kmem_cache_zalloc(hot_inode_item_cachep, GFP_NOFS);
	if (!he_new)
		return ERR_PTR(-ENOMEM);

	hot_inode_item_init(he_new, root, ino);

	goto redo;
}
EXPORT_SYMBOL_GPL(hot_inode_item_lookup);

void hot_inode_item_delete(struct inode *inode)
{
	struct hot_info *root = inode->i_sb->s_hot_root;
	struct hot_inode_item *he;

	if (!root || !S_ISREG(inode->i_mode))
		return;

	he = hot_inode_item_lookup(root, inode->i_ino, 0);
	if (IS_ERR(he))
		return;

	hot_comm_item_put(&he->hot_inode); /* for lookup */
	hot_comm_item_unlink(root, &he->hot_inode);
}
EXPORT_SYMBOL_GPL(hot_inode_item_delete);

/* he->i_lock is acquired in this function. */
struct hot_range_item
*hot_range_item_lookup(struct hot_inode_item *he, loff_t start, int alloc)
{
	struct rb_node **p;
	struct rb_node *parent = NULL;
	struct hot_comm_item *ci;
	struct hot_range_item *hr, *hr_new = NULL;

	start = hot_shift(start, RANGE_BITS, true);

	/* walk tree to find insertion point */
redo:
	spin_lock(&he->i_lock);
	p = &he->hot_range_tree.rb_node;
	while (*p) {
		parent = *p;
		ci = rb_entry(parent, struct hot_comm_item, rb_node);
		hr = container_of(ci, struct hot_range_item, hot_range);
		if (start < hr->start)
			p = &(*p)->rb_left;
		else if (start > (hr->start + hr->len - 1))
			p = &(*p)->rb_right;
		else {
			hot_comm_item_get(&hr->hot_range);
			spin_unlock(&he->i_lock);
			if(hr_new)
				/*
				 * Lost the race. Somebody else inserted
				 * the item for the range. Free the
				 * newly allocated item.
				 */
				kmem_cache_free(hot_range_item_cachep, hr_new);

			if (test_bit(HOT_DELETING, &hr->hot_range.delete_flag))
				return ERR_PTR(-ENOENT);

			return hr;
		}
	}

	if (hr_new) {
		rb_link_node(&hr_new->hot_range.rb_node, parent, p);
		rb_insert_color(&hr_new->hot_range.rb_node,
				&he->hot_range_tree);
		hot_comm_item_get(&hr_new->hot_range);
		spin_unlock(&he->i_lock);
		return hr_new;
	}
	spin_unlock(&he->i_lock);

	if (!alloc)
		return ERR_PTR(-ENOENT);

	hr_new = kmem_cache_zalloc(hot_range_item_cachep, GFP_NOFS);
	if (!hr_new)
		return ERR_PTR(-ENOMEM);

	hot_range_item_init(hr_new, he, start);

	goto redo;
}
EXPORT_SYMBOL_GPL(hot_range_item_lookup);

/*
 * This function does the actual work of updating
 * the frequency numbers.
 *
 * avg_delta_{reads,writes} are indeed a kind of simple moving
 * average of the time difference between each of the last
 * 2^(FREQ_POWER) reads/writes. If there have not yet been that
 * many reads or writes, it's likely that the values will be very
 * large; They are initialized to the largest possible value for the
 * data type. Simply, we don't want a few fast access to a file to
 * automatically make it appear very hot.
 */
static void hot_freq_calc(struct timespec old_atime,
		struct timespec cur_time, u64 *avg)
{
	struct timespec delta_ts;
	u64 new_delta;

	delta_ts = timespec_sub(cur_time, old_atime);
	new_delta = timespec_to_ns(&delta_ts) >> FREQ_POWER;

	*avg = (*avg << FREQ_POWER) - *avg + new_delta;
	*avg = *avg >> FREQ_POWER;
}

static void hot_freq_update(struct hot_info *root,
		struct hot_comm_item *ci, bool write)
{
	struct timespec cur_time = current_kernel_time();
	struct hot_freq_data *freq_data = &ci->hot_freq_data;

	if (write) {
		freq_data->nr_writes += 1;
		hot_freq_calc(freq_data->last_write_time,
				cur_time,
				&freq_data->avg_delta_writes);
		freq_data->last_write_time = cur_time;
	} else {
		freq_data->nr_reads += 1;
		hot_freq_calc(freq_data->last_read_time,
				cur_time,
				&freq_data->avg_delta_reads);
		freq_data->last_read_time = cur_time;
	}
}

/*
 * hot_temp_calc() is responsible for distilling the six heat
 * criteria down into a single temperature value for the data,
 * which is an integer between 0 and HEAT_MAX_VALUE.
 *
 * With the six values, we first do some very rudimentary
 * "normalizations" to each metric such that they affect the
 * final temperature calculation exactly the right way. It's
 * important to note that we still weren't really sure that
 * these six adjustments were exactly right.
 * They could definitely use more tweaking and adjustment,
 * especially in terms of the memory footprint they consume.
 *
 * Next, we take the adjusted values and shift them down to
 * a manageable size, whereafter they are weighted using the
 * the *_COEFF_POWER values and combined to a single temperature
 * value.
 */
static u32 hot_temp_calc(struct hot_comm_item *ci)
{
	u32 result = 0;
	struct hot_freq_data *freq_data = &ci->hot_freq_data;

	struct timespec ckt = current_kernel_time();
	u64 cur_time = timespec_to_ns(&ckt);
	u32 nrr_heat, nrw_heat;
	u64 ltr_heat, ltw_heat, avr_heat, avw_heat;

	nrr_heat = (u32)hot_shift((u64)freq_data->nr_reads,
					NRR_MULTIPLIER_POWER, true);
	nrw_heat = (u32)hot_shift((u64)freq_data->nr_writes,
					NRW_MULTIPLIER_POWER, true);

	ltr_heat =
	hot_shift((cur_time - timespec_to_ns(&freq_data->last_read_time)),
			LTR_DIVIDER_POWER, false);
	ltw_heat =
	hot_shift((cur_time - timespec_to_ns(&freq_data->last_write_time)),
			LTW_DIVIDER_POWER, false);

	avr_heat =
	hot_shift((((u64) -1) - freq_data->avg_delta_reads),
			AVR_DIVIDER_POWER, false);
	avw_heat =
	hot_shift((((u64) -1) - freq_data->avg_delta_writes),
			AVW_DIVIDER_POWER, false);

	/* ltr_heat is now guaranteed to be u32 safe */
	if (ltr_heat >= hot_shift((u64) 1, 32, true))
		ltr_heat = 0;
	else
		ltr_heat = hot_shift((u64) 1, 32, true) - ltr_heat;

	/* ltw_heat is now guaranteed to be u32 safe */
	if (ltw_heat >= hot_shift((u64) 1, 32, true))
		ltw_heat = 0;
	else
		ltw_heat = hot_shift((u64) 1, 32, true) - ltw_heat;

	/* avr_heat is now guaranteed to be u32 safe */
	if (avr_heat >= hot_shift((u64) 1, 32, true))
		avr_heat = (u32) -1;

	/* avw_heat is now guaranteed to be u32 safe */
	if (avw_heat >= hot_shift((u64) 1, 32, true))
		avw_heat = (u32) -1;

	nrr_heat = (u32)hot_shift((u64)nrr_heat,
		(3 - NRR_COEFF_POWER), false);
	nrw_heat = (u32)hot_shift((u64)nrw_heat,
		(3 - NRW_COEFF_POWER), false);
	ltr_heat = hot_shift(ltr_heat, (3 - LTR_COEFF_POWER), false);
	ltw_heat = hot_shift(ltw_heat, (3 - LTW_COEFF_POWER), false);
	avr_heat = hot_shift(avr_heat, (3 - AVR_COEFF_POWER), false);
	avw_heat = hot_shift(avw_heat, (3 - AVW_COEFF_POWER), false);

	result = nrr_heat + nrw_heat + (u32) ltr_heat +
		(u32) ltw_heat + (u32) avr_heat + (u32) avw_heat;

	return result;
}

/*
 * Calculate a new temperature and, if necessary,
 * move the list_head corresponding to this inode or range
 * to the proper list with the new temperature.
 */
static bool hot_map_update(struct hot_info *root,
			struct hot_comm_item *ci)
{
	u32 temp = hot_temp_calc(ci);
	u8 cur_temp, prev_temp;
	bool flag = false;

	cur_temp = (u8)hot_shift((u64)temp,
				(32 - MAP_BITS), false);
	prev_temp = (u8)hot_shift((u64)ci->hot_freq_data.last_temp,
				(32 - MAP_BITS), false);

	if (cur_temp != prev_temp) {
		u32 type = ci->hot_freq_data.flags;
		spin_lock(&root->m_lock);
		if (test_and_clear_bit(HOT_IN_LIST, &ci->delete_flag)) {
			list_del_rcu(&ci->track_list);
			flag = true;
		}
		spin_unlock(&root->m_lock);

		if (!flag) {
			atomic_inc(&root->hot_map_nr);
			hot_comm_item_get(ci);
		}

		spin_lock(&root->m_lock);
		if (test_bit(HOT_DELETING, &ci->delete_flag)) {
			spin_unlock(&root->m_lock);
			return true;
		}
		set_bit(HOT_IN_LIST, &ci->delete_flag);
		list_add_tail_rcu(&ci->track_list,
				&root->hot_map[type][cur_temp]);
		spin_unlock(&root->m_lock);

		ci->hot_freq_data.last_temp = temp;
	}

	return false;
}

/*
 * Update temperatures for each range item for aging purposes.
 * If one hot range item is old, it will be aged out.
 */
static void hot_range_update(struct hot_inode_item *he,
				struct hot_info *root)
{
	struct rb_node *node;
	struct hot_comm_item *ci;

	rcu_read_lock();
	node = rb_first(&he->hot_range_tree);
	while (node) {
		ci = rb_entry(node, struct hot_comm_item, rb_node);
		node = rb_next(node);
		if (test_bit(HOT_DELETING, &ci->delete_flag) ||
			hot_map_update(root, ci))
			continue;
	}
	rcu_read_unlock();
}

/* Temperature compare function*/
static int hot_temp_cmp(void *priv, struct list_head *a,
				struct list_head *b)
{
	struct hot_comm_item *ap = container_of(a,
			struct hot_comm_item, track_list);
	struct hot_comm_item *bp = container_of(b,
			struct hot_comm_item, track_list);

	int diff = ap->hot_freq_data.last_temp
			- bp->hot_freq_data.last_temp;
	if (diff > 0)
		return -1;
	if (diff < 0)
		return 1;
	return 0;
}

static void hot_item_evictor(struct hot_info *root, unsigned long work,
			unsigned long (*work_get)(struct hot_info *root))
{
	int i;

	if (work <= 0)
		return;

	for (i = 0; i < MAP_SIZE; i++) {
		struct hot_comm_item *ci;
		unsigned long work_prev;

		rcu_read_lock();
		if (list_empty(&root->hot_map[TYPE_INODE][i])) {
			rcu_read_unlock();
			continue;
		}

		list_for_each_entry_rcu(ci, &root->hot_map[TYPE_INODE][i],
					track_list) {
			work_prev = work_get(root);
			hot_comm_item_unlink(root, ci);
			work -= (work_prev - work_get(root));
			if (work <= 0)
				break;
		}
		rcu_read_unlock();

		if (work <= 0)
			break;
	}
}

/*
 * Every sync period we update temperatures for
 * each hot inode item and hot range item for aging
 * purposes.
 */
static void hot_update_worker(struct work_struct *work)
{
	struct hot_info *root = container_of(to_delayed_work(work),
					struct hot_info, update_work);
	struct rb_node *node;
	struct hot_comm_item *ci;
	struct hot_inode_item *he;
	int i, j;

	rcu_read_lock();
	node = rb_first(&root->hot_inode_tree);
	while (node) {
		ci = rb_entry(node, struct hot_comm_item, rb_node);
		node = rb_next(node);
		if (test_bit(HOT_DELETING, &ci->delete_flag) ||
			hot_map_update(root, ci))
			continue;
		he = container_of(ci, struct hot_inode_item, hot_inode);
		hot_range_update(he, root);
	}
	rcu_read_unlock();

	/* Sort temperature map info based on last temperature*/
	for (i = 0; i < MAP_SIZE; i++) {
		for (j = 0; j < MAX_TYPES; j++) {
			spin_lock(&root->m_lock);
			list_sort(NULL, &root->hot_map[j][i], hot_temp_cmp);
			spin_unlock(&root->m_lock);
		}
	}

	/* Instert next delayed work */
	queue_delayed_work(root->update_wq, &root->update_work,
		msecs_to_jiffies(HOT_UPDATE_INTERVAL * MSEC_PER_SEC));
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

static inline unsigned long hot_nr_get(struct hot_info *root)
{
	return (unsigned long)atomic_read(&root->hot_map_nr);
}

static void hot_prune_map(struct hot_info *root, unsigned long nr)
{
	hot_item_evictor(root, nr, hot_nr_get);
}

/* The shrinker callback function */
static int hot_track_prune(struct shrinker *shrink,
			struct shrink_control *sc)
{
	struct hot_info *root =
		container_of(shrink, struct hot_info, hot_shrink);

	if (sc->nr_to_scan == 0)
		return atomic_read(&root->hot_map_nr) / 2;

	if (!(sc->gfp_mask & __GFP_FS))
		return -1;

	hot_prune_map(root, sc->nr_to_scan);

	return atomic_read(&root->hot_map_nr);
}

/*
 * Main function to update i/o access frequencies, and it will be called
 * from read/writepages() hooks, which are read_pages(), do_writepages(),
 * do_generic_file_read(), and __blockdev_direct_IO().
 */
void hot_update_freqs(struct inode *inode, loff_t start,
			size_t len, int rw)
{
	struct hot_info *root = inode->i_sb->s_hot_root;
	struct hot_inode_item *he;
	struct hot_range_item *hr;
	u64 range_size;
	loff_t cur, end;

	if (!root || (len == 0) || !S_ISREG(inode->i_mode))
		return;

	he = hot_inode_item_lookup(root, inode->i_ino, 1);
	if (IS_ERR(he))
		return;

	hot_freq_update(root, &he->hot_inode, rw);

	/*
	 * Align ranges on range size boundary
	 * to prevent proliferation of range structs
	 */
	range_size  = hot_shift(1, RANGE_BITS, true);
	end = hot_shift((start + len + range_size - 1),
			RANGE_BITS, false);
	cur = hot_shift(start, RANGE_BITS, false);
	for (; cur < end; cur++) {
		hr = hot_range_item_lookup(he, cur, 1);
		if (IS_ERR(hr)) {
			WARN(1, "hot_range_item_lookup returns %ld\n",
				PTR_ERR(hr));
			hot_comm_item_put(&he->hot_inode);
			return;
		}

		hot_freq_update(root, &hr->hot_range, rw);

		hot_comm_item_put(&hr->hot_range);
	}

	hot_comm_item_put(&he->hot_inode);
}
EXPORT_SYMBOL_GPL(hot_update_freqs);

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
	atomic_set(&root->hot_map_nr, 0);

	for (i = 0; i < MAP_SIZE; i++) {
		for (j = 0; j < MAX_TYPES; j++)
			INIT_LIST_HEAD(&root->hot_map[j][i]);
	}

	root->update_wq = alloc_workqueue(
			"hot_update_wq", WQ_NON_REENTRANT, 0);
	if (!root->update_wq) {
		printk(KERN_ERR "%s: Failed to create "
				"hot update workqueue\n", __func__);
		kfree(root);
		return ERR_PTR(-ENOMEM);
	}

	/* Initialize hot tracking wq and arm one delayed work */
	INIT_DELAYED_WORK(&root->update_work, hot_update_worker);
	queue_delayed_work(root->update_wq, &root->update_work,
		msecs_to_jiffies(HOT_UPDATE_INTERVAL * MSEC_PER_SEC));

	/* Register a shrinker callback */
	root->hot_shrink.shrink = hot_track_prune;
	root->hot_shrink.seeks = DEFAULT_SEEKS;
	register_shrinker(&root->hot_shrink);

	return root;
}

/*
 * Frees the entire hot tree.
 */
static void hot_tree_exit(struct hot_info *root)
{
	struct rb_node *node;
	struct hot_comm_item *ci;

	unregister_shrinker(&root->hot_shrink);
	cancel_delayed_work_sync(&root->update_work);
	destroy_workqueue(root->update_wq);

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
