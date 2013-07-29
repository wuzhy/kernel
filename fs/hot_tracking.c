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
#include <linux/sched.h>
#include "hot_tracking.h"

int sysctl_hot_mem_high_thresh __read_mostly = 0;
int sysctl_hot_update_interval __read_mostly = 150;

/* kmem_cache pointers for slab caches */
static struct kmem_cache *hot_inode_item_cachep __read_mostly;
static struct kmem_cache *hot_range_item_cachep __read_mostly;

static void hot_range_item_init(struct hot_range_item *hr,
			struct hot_inode_item *he, loff_t start)
{
	kref_init(&hr->refs);
	INIT_LIST_HEAD(&hr->track_list);
	hr->freq.avg_delta_reads = (u64) -1;
	hr->freq.avg_delta_writes = (u64) -1;
	hr->start = start;
	hr->len = 1 << RANGE_BITS;
	hr->hot_inode = he;
	atomic_long_inc(&he->hot_root->hot_cnt);
	hot_mem_limit_add(he->hot_root, sizeof(struct hot_range_item));
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
	struct hot_info *root = hr->hot_inode->hot_root;

	rb_erase(&hr->rb_node, &hr->hot_inode->hot_range_tree);
	spin_lock(&root->m_lock);
	if (!list_empty(&hr->track_list))
		list_del_init(&hr->track_list);
	spin_unlock(&root->m_lock);

	atomic_long_dec(&root->hot_cnt);
	hot_mem_limit_sub(root, sizeof(struct hot_range_item));
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

static struct hot_range_item
*hot_range_item_alloc(struct hot_inode_item *he, loff_t start)
{
	struct rb_node **p;
	struct rb_node *parent = NULL;
	struct hot_range_item *hr, *hr_new = NULL;
	u32 temp;
	u8 temp_cur;

	start = start << RANGE_BITS;

	/* walk tree to find insertion point */
redo:
	spin_lock(&he->i_lock);
	p = &he->hot_range_tree.rb_node;
	while (*p) {
		parent = *p;
		hr = rb_entry(parent, struct hot_range_item, rb_node);
		if (start < hr->start)
			p = &(*p)->rb_left;
		else if (start > (hr->start + hr->len - 1))
			p = &(*p)->rb_right;
		else {
			hot_range_item_get(hr);
			if (hr_new) {
				/*
				 * Lost the race. Somebody else inserted
				 * the item for the range. Free the
				 * newly allocated item.
				 */
				atomic_long_dec(&he->hot_root->hot_cnt);
				hot_mem_limit_sub(he->hot_root,
						sizeof(struct hot_range_item));
				kmem_cache_free(hot_range_item_cachep, hr_new);
			}
			spin_unlock(&he->i_lock);

			return hr;
		}
	}

	if (hr_new) {
		rb_link_node(&hr_new->rb_node, parent, p);
		rb_insert_color(&hr_new->rb_node, &he->hot_range_tree);
		temp = hot_temp_calc(&hr_new->freq);
		temp_cur = (u8)(temp >> (32 - MAP_BITS));
		spin_lock(&he->hot_root->m_lock);
		list_add_tail(&hr_new->track_list,
			&he->hot_root->hot_map[TYPE_RANGE][temp_cur]);
		spin_unlock(&he->hot_root->m_lock);
		hot_range_item_get(hr_new); /* For the caller */
		spin_unlock(&he->i_lock);
		return hr_new;
	}
        spin_unlock(&he->i_lock);

	hr_new = kmem_cache_zalloc(hot_range_item_cachep, GFP_NOFS);
	if (!hr_new)
		return ERR_PTR(-ENOMEM);

	hot_range_item_init(hr_new, he, start);

	cond_resched();

	goto redo;
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

static void hot_range_map_update(struct hot_info *root,
			struct hot_range_item *hr)
{
	u32 temp = hot_temp_calc(&hr->freq);
	u8 temp_cur = (u8)(temp >> (32 - MAP_BITS));
	u8 temp_prev = (u8)(hr->freq.last_temp >> (32 - MAP_BITS));

	spin_lock(&root->m_lock);
	if (!list_empty(&hr->track_list)
		&& (temp_cur != temp_prev)) {
		hr->freq.last_temp = temp;
		list_del_init(&hr->track_list);
		list_add_tail(&hr->track_list,
			&root->hot_map[TYPE_RANGE][temp_cur]);
	}
	spin_unlock(&root->m_lock);
}

/*
 * Update temperatures for each range item for aging purposes.
 * If one hot range item is old, it will be aged out.
 */
static void hot_range_tree_update(struct hot_inode_item *he,
				struct hot_info *root)
{
	struct rb_node *node;
	struct hot_range_item *hr;

	rcu_read_lock();
	node = rb_first(&he->hot_range_tree);
	while (node) {
		hr = rb_entry(node, struct hot_range_item, rb_node);
		node = rb_next(node);
		hot_range_map_update(root, hr);
	}
	rcu_read_unlock();
}

static void hot_inode_item_init(struct hot_inode_item *he,
			struct hot_info *root, u64 ino)
{
	kref_init(&he->refs);
	INIT_LIST_HEAD(&he->track_list);
	he->freq.avg_delta_reads = (u64) -1;
	he->freq.avg_delta_writes = (u64) -1;
	he->ino = ino;
	he->hot_root = root;
	spin_lock_init(&he->i_lock);
	atomic_long_inc(&root->hot_cnt);
	hot_mem_limit_add(root, sizeof(struct hot_inode_item));
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
	if (!list_empty(&he->track_list))
		list_del_init(&he->track_list);
	hot_range_tree_free(he);

	atomic_long_dec(&he->hot_root->hot_cnt);
	hot_mem_limit_sub(he->hot_root, sizeof(struct hot_inode_item));
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

static struct hot_inode_item
*hot_inode_item_alloc(struct hot_info *root, u64 ino)
{
	struct rb_node **p;
	struct rb_node *parent = NULL;
	struct hot_inode_item *he, *he_new = NULL;
	u32 temp;
	u8 temp_cur;

	/* walk tree to find insertion point */
redo:
	spin_lock(&root->t_lock);
	p = &root->hot_inode_tree.rb_node;
	while (*p) {
		parent = *p;
		he = rb_entry(parent, struct hot_inode_item, rb_node);
		if (ino < he->ino)
			p = &(*p)->rb_left;
		else if (ino > he->ino)
			p = &(*p)->rb_right;
		else {
			hot_inode_item_get(he);
			if (he_new) {
				/*
				 * Lost the race. Somebody else inserted
				 * the item for the inode. Free the
				 * newly allocated item.
				 */
				atomic_long_dec(&root->hot_cnt);
				hot_mem_limit_sub(root,
						sizeof(struct hot_inode_item));
				kmem_cache_free(hot_inode_item_cachep, he_new);
			}
			spin_unlock(&root->t_lock);

			return he;
		}
	}

	if (he_new) {
		rb_link_node(&he_new->rb_node, parent, p);
		rb_insert_color(&he_new->rb_node, &root->hot_inode_tree);
		temp = hot_temp_calc(&he_new->freq);
		temp_cur = (u8)(temp >> (32 - MAP_BITS));
		list_add_tail(&he_new->track_list,
			&root->hot_map[TYPE_INODE][temp_cur]);
		hot_inode_item_get(he_new); /* For the caller */
		spin_unlock(&root->t_lock);
		return he_new;
	}
	spin_unlock(&root->t_lock);

	he_new = kmem_cache_zalloc(hot_inode_item_cachep, GFP_NOFS);
	if (!he_new)
		return ERR_PTR(-ENOMEM);

	hot_inode_item_init(he_new, root, ino);

	cond_resched();

	goto redo;
}

struct hot_inode_item
*hot_inode_item_lookup(struct hot_info *root, u64 ino)
{
	struct rb_node **p;
	struct rb_node *parent = NULL;
	struct hot_inode_item *he;

	/* walk tree to find insertion point */
	spin_lock(&root->t_lock);
	p = &root->hot_inode_tree.rb_node;
	while (*p) {
		parent = *p;
		he = rb_entry(parent, struct hot_inode_item, rb_node);
		if (ino < he->ino)
			p = &(*p)->rb_left;
		else if (ino > he->ino)
			p = &(*p)->rb_right;
		else {
			hot_inode_item_get(he);
			spin_unlock(&root->t_lock);

			return he;
		}
	}
	spin_unlock(&root->t_lock);

	return ERR_PTR(-ENOENT);
}

void hot_inode_item_unlink(struct inode *inode)
{
	struct hot_info *root = inode->i_sb->s_hot_root;
	struct hot_inode_item *he;

	if (!(inode->i_sb->s_flags & MS_HOTTRACK)
		|| !S_ISREG(inode->i_mode))
		return;

	he = hot_inode_item_lookup(root, inode->i_ino);
	if (IS_ERR(he))
                return;

	spin_lock(&root->t_lock);
	hot_inode_item_put(he);
	hot_inode_item_put(he); /* For the caller */
	spin_unlock(&root->t_lock);
}

/*
 * Calculate a new temperature and, if necessary,
 * move the list_head corresponding to this inode or range
 * to the proper list with the new temperature.
 */
static void hot_inode_map_update(struct hot_info *root,
			struct hot_inode_item *he)
{
	u32 temp = hot_temp_calc(&he->freq);
	u8 temp_cur = (u8)(temp >> (32 - MAP_BITS));
	u8 temp_prev = (u8)(he->freq.last_temp >> (32 - MAP_BITS));

	spin_lock(&root->t_lock);
	if (!list_empty(&he->track_list)
		&& (temp_cur != temp_prev)) {
		he->freq.last_temp = temp;
		list_del_init(&he->track_list);
		list_add_tail(&he->track_list,
			&root->hot_map[TYPE_INODE][temp_cur]);
	}
	spin_unlock(&root->t_lock);
}

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
		struct hot_freq *freq, bool write)
{
	struct timespec cur_time = current_kernel_time();

	if (write) {
		freq->nr_writes += 1;
		hot_freq_calc(freq->last_write_time,
				cur_time,
				&freq->avg_delta_writes);
		freq->last_write_time = cur_time;
	} else {
		freq->nr_reads += 1;
		hot_freq_calc(freq->last_read_time,
				cur_time,
				&freq->avg_delta_reads);
		freq->last_read_time = cur_time;
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
u32 hot_temp_calc(struct hot_freq *freq)
{
	u32 result = 0;

	struct timespec ckt = current_kernel_time();
	u64 cur_time = timespec_to_ns(&ckt);
	u32 nrr_heat, nrw_heat;
	u64 ltr_heat, ltw_heat, avr_heat, avw_heat;

	nrr_heat = (u32)(freq->nr_reads << NRR_MULTIPLIER_POWER);
	nrw_heat = (u32)(freq->nr_writes << NRW_MULTIPLIER_POWER);

	ltr_heat = (cur_time - timespec_to_ns(&freq->last_read_time))
			>> LTR_DIVIDER_POWER;
	ltw_heat = (cur_time - timespec_to_ns(&freq->last_write_time))
			>> LTW_DIVIDER_POWER;

	avr_heat = (((u64) -1) - freq->avg_delta_reads)
			>> AVR_DIVIDER_POWER;
	avw_heat = (((u64) -1) - freq->avg_delta_writes)
			>> AVW_DIVIDER_POWER;

	/* ltr_heat is now guaranteed to be u32 safe */
	if (ltr_heat >= ((u64)1 << 32))
		ltr_heat = 0;
	else
		ltr_heat = ((u64)1 << 32) - ltr_heat;

	/* ltw_heat is now guaranteed to be u32 safe */
	if (ltw_heat >= ((u64)1 << 32))
		ltw_heat = 0;
	else
		ltw_heat = ((u64)1 << 32) - ltw_heat;

	/* avr_heat is now guaranteed to be u32 safe */
	if (avr_heat >= ((u64)1 << 32))
		avr_heat = (u32)-1;

	/* avw_heat is now guaranteed to be u32 safe */
	if (avw_heat >= ((u64)1 << 32))
		avw_heat = (u32)-1;

	nrr_heat = (u32)((u64)nrr_heat >> (3 - NRR_COEFF_POWER));
	nrw_heat = (u32)((u64)nrw_heat >> (3 - NRW_COEFF_POWER));
	ltr_heat = (ltr_heat >> (3 - LTR_COEFF_POWER));
	ltw_heat = (ltw_heat >> (3 - LTW_COEFF_POWER));
	avr_heat = (avr_heat >> (3 - AVR_COEFF_POWER));
	avw_heat = (avw_heat >> (3 - AVW_COEFF_POWER));

	result = nrr_heat + nrw_heat + (u32) ltr_heat +
		(u32) ltw_heat + (u32) avr_heat + (u32) avw_heat;

	return result;
}

static unsigned long hot_item_evict(struct hot_info *root, unsigned long work,
			unsigned long (*work_get)(struct hot_info *root))
{
	long budget = work;
	unsigned long freed = 0;
	int i;

	for (i = 0; i < MAP_SIZE; i++) {
		struct hot_inode_item *he, *next;

		spin_lock(&root->t_lock);
		if (list_empty(&root->hot_map[TYPE_INODE][i])) {
			spin_unlock(&root->t_lock);
			continue;
		}

		list_for_each_entry_safe(he, next,
			&root->hot_map[TYPE_INODE][i], track_list) {
			long work_prev, delta;

			if (atomic_read(&he->refs.refcount) > 1)
				continue;
			work_prev = work_get(root);
			hot_inode_item_put(he);
			delta = work_prev - work_get(root);
			budget -= delta;
			freed += delta;
			if (unlikely(budget <= 0))
				break;
		}
		spin_unlock(&root->t_lock);

		if (unlikely(budget <= 0))
			break;

		cond_resched();
	}

	return freed;
}

static void hot_mem_evict(struct hot_info *root)
{
	unsigned long sum, thresh;

	if (sysctl_hot_mem_high_thresh == 0) 
		return;

	sum = hot_mem_limit_sum(root);
	/* Note: sysctl_** is in the unit of 1M bytes */
	thresh = sysctl_hot_mem_high_thresh;
	thresh *= 1024 * 1024;
	if (sum <= thresh)
		return;

	hot_item_evict(root, sum - thresh, hot_mem_limit_sum);
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
	struct hot_inode_item *he;
	struct rb_node *node;

	hot_mem_evict(root);

	rcu_read_lock();
	node = root->hot_inode_tree.rb_node;
	while (node) {
		he = rb_entry(node, struct hot_inode_item, rb_node);
		node = rb_next(node);
		hot_inode_map_update(root, he);
		hot_range_tree_update(he, root);
	}
	rcu_read_unlock();

	/* Instert next delayed work */
	queue_delayed_work(root->update_wq, &root->update_work,
		msecs_to_jiffies(sysctl_hot_update_interval * MSEC_PER_SEC));
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

static unsigned long hot_track_shrink_count(struct shrinker *shrink,
			struct shrink_control *sc)
{
	struct hot_info *root =
		container_of(shrink, struct hot_info, hot_shrink);

	return (unsigned long)atomic_long_read(&root->hot_cnt);
}

static inline unsigned long hot_cnt_get(struct hot_info *root)
{
	return (unsigned long)atomic_long_read(&root->hot_cnt);
}

static unsigned long hot_prune_map(struct hot_info *root, unsigned long nr)
{
	return hot_item_evict(root, nr, hot_cnt_get);
}

/* The shrinker callback function */
static unsigned long hot_track_shrink_scan(struct shrinker *shrink,
			struct shrink_control *sc)
{
	struct hot_info *root =
		container_of(shrink, struct hot_info, hot_shrink);
	unsigned long freed;

	if (!(sc->gfp_mask & __GFP_FS))
		return SHRINK_STOP;

	freed =  hot_prune_map(root, sc->nr_to_scan);

	return freed;
}

/*
 * Main function to update i/o access frequencies, and it will be called
 * from read/writepages() hooks, which are read_pages(), do_writepages(),
 * do_generic_file_read(), and __blockdev_direct_IO().
 */
inline void hot_freqs_update(struct inode *inode, loff_t start,
			size_t len, int rw)
{
	struct hot_info *root = inode->i_sb->s_hot_root;
	struct hot_inode_item *he;
	struct hot_range_item *hr;
	u64 range_size;
	loff_t cur, end;

	if (!(inode->i_sb->s_flags & MS_HOTTRACK) || (len == 0)
		|| !S_ISREG(inode->i_mode) || !inode->i_nlink)
		return;

	he = hot_inode_item_alloc(root, inode->i_ino);
	if (IS_ERR(he))
		return;

	hot_freq_update(root, &he->freq, rw);

	/*
	 * Align ranges on range size boundary
	 * to prevent proliferation of range structs
	 */
	range_size  = 1 << RANGE_BITS;
	end = (start + len + range_size - 1) >> RANGE_BITS;
	cur = start >> RANGE_BITS;
	for (; cur < end; cur++) {
		hr = hot_range_item_alloc(he, cur);
		if (IS_ERR(hr)) {
			WARN(1, "hot_range_item_alloc returns %ld\n",
				PTR_ERR(hr));
			return;
		}

		hot_freq_update(root, &hr->freq, rw);

		spin_lock(&he->i_lock);
		hot_range_item_put(hr);
		spin_unlock(&he->i_lock);
	}

	spin_lock(&root->t_lock);
	hot_inode_item_put(he);
	spin_unlock(&root->t_lock);
}
EXPORT_SYMBOL(hot_freqs_update);

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
	atomic_long_set(&root->hot_cnt, 0);

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
		msecs_to_jiffies(sysctl_hot_update_interval * MSEC_PER_SEC));

	/* Register a shrinker callback */
	root->hot_shrink.count_objects = hot_track_shrink_count;
	root->hot_shrink.scan_objects = hot_track_shrink_scan;
	root->hot_shrink.seeks = DEFAULT_SEEKS;
	root->hot_shrink.flags = SHRINKER_NUMA_AWARE;
	register_shrinker(&root->hot_shrink);

	return root;
}

/*
 * Frees the entire hot tree.
 */
static void hot_tree_exit(struct hot_info *root)
{
	struct hot_inode_item *he;
	struct rb_node *node;

	unregister_shrinker(&root->hot_shrink);
	cancel_delayed_work_sync(&root->update_work);
	destroy_workqueue(root->update_wq);

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

	hot_mem_limit_init(root);
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
