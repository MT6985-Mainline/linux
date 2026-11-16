/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (c) 2018 MediaTek Inc.
 */
#ifndef __SOC_MTK_DVFSRC_H
#define __SOC_MTK_DVFSRC_H

#include <linux/kconfig.h>
#include <linux/types.h>

struct device;

#define MTK_DVFSRC_CMD_BW_REQUEST	0
#define MTK_DVFSRC_CMD_OPP_REQUEST	1
#define MTK_DVFSRC_CMD_VCORE_REQUEST	2
#define MTK_DVFSRC_CMD_DRAM_REQUEST	3
#define MTK_DVFSRC_CMD_HRTBW_REQUEST	4
#define MTK_DVFSRC_CMD_VSCP_REQUEST	5
#define MTK_DVFSRC_CMD_PEAK_BW_REQUEST	6
#define MTK_DVFSRC_CMD_EMICLK_REQUEST	7

#define MTK_DVFSRC_CMD_FORCEOPP_REQUEST	16


#define MTK_DVFSRC_CMD_VCORE_LEVEL_QUERY 0
#define MTK_DVFSRC_CMD_VSCP_LEVEL_QUERY	1
#define MTK_DVFSRC_CMD_DRAM_LEVEL_QUERY	2
#define MTK_DVFSRC_CMD_CURR_LEVEL_QUERY	3

#if IS_ENABLED(CONFIG_MTK_DVFSRC)
int mtk_dvfsrc_send_request_v2(const struct device *dev, u32 cmd, u64 data);
int mtk_dvfsrc_query_info_v2(const struct device *dev, u32 cmd, int *data);
#else
static inline int mtk_dvfsrc_send_request_v2(const struct device *dev, u32 cmd,
					   u64 data)
{ return -ENODEV; }
static inline int mtk_dvfsrc_query_info_v2(const struct device *dev, u32 cmd,
					int *data)
{ return -ENODEV; }

#endif /* CONFIG_MTK_DVFSRC */

#endif
