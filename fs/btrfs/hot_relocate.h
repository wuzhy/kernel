/*
 * fs/btrfs/hot_relocate.h
 *
 * Copyright (C) 2013 IBM Corp. All rights reserved.
 * Written by Zhi Yong Wu <wuzhy@linux.vnet.ibm.com>
 *	      Ben Chociej <bchociej@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 */

#ifndef __HOT_RELOCATE__
#define __HOT_RELOCATE__

#include <linux/hot_tracking.h>
#include "ctree.h"
#include "btrfs_inode.h"
#include "volumes.h"

#define HOT_RELOC_INTERVAL  120
#define HOT_RELOC_THRESHOLD 150
#define HOT_RELOC_MAX_ITEMS 250

#define HEAT_MAX_VALUE    (MAP_SIZE - 1)
#define HIGH_WATER_LEVEL  75 /* when to raise the threshold */
#define LOW_WATER_LEVEL   50 /* when to lower the threshold */
#define THRESH_UP_SPEED   10 /* how much to raise it by */
#define THRESH_DOWN_SPEED 1  /* how much to lower it by */
#define THRESH_MAX_VALUE  100

struct hot_reloc {
	struct btrfs_fs_info *fs_info;
	struct list_head hot_relocq[MAX_RELOC_TYPES];
	int thresh;
	struct task_struct *hot_reloc_kthread;
	struct mutex hot_reloc_mutex;
};

int hot_relocate_init(struct btrfs_fs_info *fs_info);
void hot_relocate_exit(struct btrfs_fs_info *fs_info);

#endif /* __HOT_RELOCATE__ */
