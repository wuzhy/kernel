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

#include <linux/workqueue.h>
#include <linux/hot_tracking.h>

#define HOT_UPDATE_INTERVAL 150
#define HOT_AGE_INTERVAL 300

/* size of sub-file ranges */
#define RANGE_BITS 20
#define FREQ_POWER 4

/* NRR/NRW heat unit = 2^X accesses */
#define NRR_MULTIPLIER_POWER 20 /* NRR - number of reads since mount */
#define NRR_COEFF_POWER 0
#define NRW_MULTIPLIER_POWER 20 /* NRW - number of writes since mount */
#define NRW_COEFF_POWER 0

/* LTR/LTW heat unit = 2^X ns of age */
#define LTR_DIVIDER_POWER 30 /* LTR - time elapsed since last read(ns) */
#define LTR_COEFF_POWER 1
#define LTW_DIVIDER_POWER 30 /* LTW - time elapsed since last write(ns) */
#define LTW_COEFF_POWER 1

/*
 * AVR/AVW cold unit = 2^X ns of average delta
 * AVR/AVW heat unit = HEAT_MAX_VALUE - cold unit
 */
#define AVR_DIVIDER_POWER 40 /* AVR - average delta between recent reads(ns) */
#define AVR_COEFF_POWER 0
#define AVW_DIVIDER_POWER 40 /* AVW - average delta between recent writes(ns) */
#define AVW_COEFF_POWER 0

#endif /* __HOT_TRACKING__ */
