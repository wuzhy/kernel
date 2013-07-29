/*
 *  include/uapi/linux/hot_tracking.h
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

#ifndef _UAPI_HOTTRACK_H
#define _UAPI_HOTTRACK_H

#include <linux/types.h>

struct hot_heat_info {
	__u8 live;
	__u8 resv[3];
	__u32 temp;
	__u64 avg_delta_reads;
	__u64 avg_delta_writes;
	__u64 last_read_time;
	__u64 last_write_time;
	__u32 num_reads;
	__u32 num_writes;
	__u64 future[4]; /* For future expansions */
};

#endif /* _UAPI_HOTTRACK_H */
