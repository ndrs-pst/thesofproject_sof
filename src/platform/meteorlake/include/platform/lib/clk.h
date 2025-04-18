/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright(c) 2022 Intel Corporation. All rights reserved.
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 *         Keyon Jie <yang.jie@linux.intel.com>
 *         Rander Wang <rander.wang@intel.com>
 */

#ifdef __SOF_LIB_CLK_H__

#ifndef __PLATFORM_LIB_CLK_H__
#define __PLATFORM_LIB_CLK_H__

#include <ace/lib/clk.h>

#define CLK_MAX_CPU_HZ		CONFIG_XTENSA_CCOUNT_HZ

#define CPU_WOVCRO_FREQ_IDX	0

#define CPU_IPLL_FREQ_IDX	1

#define CPU_LOWEST_FREQ_IDX	CPU_WOVCRO_FREQ_IDX

#define CPU_DEFAULT_IDX		CPU_IPLL_FREQ_IDX

#define NUM_CPU_FREQ		2

#define PRIMARY_CORE_BASE_CPS_USAGE 20000
#define SECONDARY_CORE_BASE_CPS_USAGE 10000

#endif /* __PLATFORM_LIB_CLK_H__ */

#else

#error "This file shouldn't be included from outside of sof/lib/clk.h"

#endif /* __SOF_LIB_CLK_H__ */
