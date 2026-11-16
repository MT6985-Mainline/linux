/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 * Device definitions from corot-t-oss scp_ipi_table.h.
 */
#ifndef _SCP_IPI_TABLE_H_
#define _SCP_IPI_TABLE_H_

struct mtk_mbox_info *scp_mbox_info;
struct mtk_mbox_pin_recv *scp_mbox_pin_recv;
struct mtk_mbox_pin_send *scp_mbox_pin_send;

struct mtk_mbox_device scp_mboxdev = {
	.name = "scp_mboxdev",
	.post_cb = scp_clear_spm,
};

struct mtk_ipi_device scp_ipidev = {
	.name = "scp_ipidev",
	.id = IPI_DEV_SCP,
	.mbdev = &scp_mboxdev,
	.pre_cb = scp_awake_lock,
	.post_cb = scp_awake_unlock,
};
EXPORT_SYMBOL(scp_ipidev);

#endif
