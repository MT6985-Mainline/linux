/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 * Native RV SCP interface, derived from the corot-t-oss SCP driver.
 */
#ifndef _SCP_NATIVE_H_
#define _SCP_NATIVE_H_

#include <linux/clk.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/notifier.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include "scp.h"

#define SCP_TO_SPM_REG		0x94
#define SCP_DRAM_RESV_ADDR	0x44
#define SCP_DRAM_RESV_SIZE	0x48
#define SCP_CORE_RSTN_CLR	0x0
#define SCP_CORE_WDT_IRQ		0x30
#define SCP_CORE_MON_PC		0x80
#define SCP_INFRA_IRQ_SET	0xb14
#define SCP_INFRA_IRQ_CLEAR	0xb18
#define SCP_AWAKE_KEY		0xa0
#define SCP_AP_AWAKE_LOCK	BIT(0)
#define SCP_AP_AWAKE_UNLOCK	BIT(1)
#define SCP_AWAKE_TIMEOUT_US	1000000
#define SCP_BOOT_TIMEOUT_MS	10000

/* Exact corot firmware SiP operations from scp_helper.h / scp_dvfs.h. */
#define SCP_SIP_CONTROL		MTK_SIP_SMC_CMD(0x528)
#define SCP_SIP_DVFS_CONTROL	MTK_SIP_SMC_CMD(0x232)
#define SCP_SIP_RESET_RELEASE	3
#define SCP_SIP_WDT_CLEAR	8
#define SCP_SIP_RESOURCE_REQ	1
#define SCP_SIP_ULPOSC_ON	2
#define SCP_SIP_ULPOSC_OFF	3
#define SCP_RESOURCE_RELEASE	0
#define SCP_RESOURCE_26M	BIT(0)
#define SCP_SYNC_ULPOSC_CALI	7

struct scp_reserve_mblock {
	enum scp_reserve_mem_id_t num;
	u32 alignment;
	u64 start_phys;
	u64 start_virt;
	u64 size;
};

struct scp_ulposc_cali {
	u32 target;
	u32 config[3];
	u16 value;
	u16 extension;
};

struct scp_error_info {
	u32 case_id;
	u32 sensor_id;
	char context[32];
};

struct scp_native {
	struct device *dev;
	void __iomem *cfg;
	void __iomem *core[2];
	void __iomem *infra;
	void __iomem *clkctrl;
	phys_addr_t mem_phys;
	phys_addr_t mem_size;
	u32 tcm_size;
	u32 core_count;
	bool secure_dump;
	bool pm_notify;
	bool stopping;
	bool firmware_ready;
	bool boot_released;
	bool ready;
	bool resource_held;
	bool clock_held;
	bool do_calibration;
	bool bypass_phase1;
	spinlock_t awake_lock;
	unsigned int awake_count;
	struct clk *clk_mux;
	struct clk *clk_26m;
	struct regmap *fmeter;
	struct regmap *ulposc;
	struct scp_ulposc_cali *calibration;
	u32 calibration_count;
	u32 ready_msg[2];
	u32 sleep_ack[2];
	struct scp_error_info error_msg[2];
	struct work_struct ready_work;
	struct delayed_work boot_timeout_work;
};

extern struct scp_native *scp_native;
extern struct mtk_mbox_info *scp_mbox_info;
extern struct mtk_mbox_pin_send *scp_mbox_pin_send;
extern struct mtk_mbox_pin_recv *scp_mbox_pin_recv;

int scp_smc(unsigned int function, unsigned int operation, unsigned int argument);
int scp_calibration_prepare(struct scp_native *scp);
int scp_calibration_sync(struct scp_native *scp);
int scp_clock_release(struct scp_native *scp);
int scp_ipi_tables_init(struct platform_device *pdev, struct scp_native *scp);
int scp_ipi_register_handlers(struct scp_native *scp);
void scp_ready_work(struct work_struct *work);
void scp_boot_timeout_work(struct work_struct *work);
irqreturn_t scp_ipc_irq(int irq, void *data);
int scp_clear_spm(void *data);
int scp_pm_notify(struct scp_native *scp, unsigned int event);
void scp_notify_ready(void);

#endif
