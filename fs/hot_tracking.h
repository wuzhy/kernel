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

void __init hot_cache_init(void);
void hot_inode_item_put(struct hot_inode_item *he);

#endif /* __HOT_TRACKING__ */
