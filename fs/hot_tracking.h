/*
 * fs/hot_tracking.h
 *
 * Copyright (C) 2013 IBM Corp. All rights reserved.
 * Written by Zhi Yong Wu <wuzhy@linux.vnet.ibm.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 */

#ifndef __HOT_TRACKING__
#define __HOT_TRACKING__

#include <linux/hot_tracking.h>

/* size of sub-file ranges */
#define RANGE_BITS 20
#define FREQ_POWER 4

void __init hot_cache_init(void);
void hot_inode_item_put(struct hot_inode_item *he);
struct hot_inode_item *hot_inode_item_lookup(struct hot_info *root, u64 ino);
void hot_inode_item_unlink(struct inode *inode);

#endif /* __HOT_TRACKING__ */
