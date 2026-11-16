// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 *
 * Native provider for the bootloader-loaded RV SCP interface used on corot.
 * Reserved-memory setup and firmware release follow corot-t-oss scp_helper.c.
 */
#include <linux/arm-smccc.h>
#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/soc/mediatek/mtk_sip_svc.h>
#include "scp_native.h"
#include "scp_reservedmem_define.h"

#define SCP_RESERVED_MEM_COMPAT "mediatek,reserve-memory-scp_share"
#define SCP_MEMORY_TABLE_CELLS 3

struct scp_native *scp_native;
static bool scp_memory_ready;
static phys_addr_t scp_reserved_phys;
static phys_addr_t scp_reserved_size;
static BLOCKING_NOTIFIER_HEAD(scp_ready_notifier);

/* Register this carveout with the OF reserved-memory lookup service. */
static int __init scp_reserve_mem_init(unsigned long node, struct reserved_mem *rmem)
{
	scp_reserved_phys = rmem->base;
	scp_reserved_size = rmem->size;
	pr_info("SCP shared reserved memory: %pa size %pa\n",
		&scp_reserved_phys, &scp_reserved_size);
	return 0;
}
/* Linux 7.2 registers an operations table rather than an init function.
 * The vendor scp_mem_key binding maps the region in probe; it does not
 * assign a DMA pool through memory-region/device_init.
 */
static const struct reserved_mem_ops scp_reserve_mem_ops = {
	.node_init = scp_reserve_mem_init,
};
RESERVEDMEM_OF_DECLARE(scp_reserve_mem, SCP_RESERVED_MEM_COMPAT,
		      &scp_reserve_mem_ops);

phys_addr_t scp_get_reserve_mem_phys(enum scp_reserve_mem_id_t id)
{
	if ((unsigned int)id >= NUMS_MEM_ID || !smp_load_acquire(&scp_memory_ready))
		return 0;
	return scp_reserve_mblock[id].start_phys;
}
EXPORT_SYMBOL_GPL(scp_get_reserve_mem_phys);

phys_addr_t scp_get_reserve_mem_virt(enum scp_reserve_mem_id_t id)
{
	if ((unsigned int)id >= NUMS_MEM_ID || !smp_load_acquire(&scp_memory_ready))
		return 0;
	return scp_reserve_mblock[id].start_virt;
}
EXPORT_SYMBOL_GPL(scp_get_reserve_mem_virt);

phys_addr_t scp_get_reserve_mem_size(enum scp_reserve_mem_id_t id)
{
	if ((unsigned int)id >= NUMS_MEM_ID || !smp_load_acquire(&scp_memory_ready))
		return 0;
	return scp_reserve_mblock[id].size;
}
EXPORT_SYMBOL_GPL(scp_get_reserve_mem_size);

unsigned int is_scp_ready(enum scp_core_id id)
{
	struct scp_native *scp = READ_ONCE(scp_native);

	return id == SCP_A_ID && scp && READ_ONCE(scp->ready);
}
EXPORT_SYMBOL_GPL(is_scp_ready);

void scp_A_register_notify(struct notifier_block *nb)
{
	blocking_notifier_chain_register(&scp_ready_notifier, nb);
	if (is_scp_ready(SCP_A_ID))
		nb->notifier_call(nb, SCP_EVENT_READY, NULL);
}
EXPORT_SYMBOL_GPL(scp_A_register_notify);

void scp_A_unregister_notify(struct notifier_block *nb)
{
	blocking_notifier_chain_unregister(&scp_ready_notifier, nb);
}
EXPORT_SYMBOL_GPL(scp_A_unregister_notify);

void scp_notify_ready(void)
{
	blocking_notifier_call_chain(&scp_ready_notifier, SCP_EVENT_READY, NULL);
}

int scp_smc(unsigned int function, unsigned int operation, unsigned int argument)
{
	struct arm_smccc_res res;

	arm_smccc_smc(function, operation, argument, 0, 0, 0, 0, 0, &res);
	return (int)res.a0;
}

static bool scp_dt_enabled(struct device_node *node, const char *property)
{
	const char *value;

	return !of_property_read_string(node, property, &value) &&
	       !strcmp(value, "enable");
}

static void scp_revoke_memory(void *data)
{
	smp_store_release(&scp_memory_ready, false);
}

static int scp_reserve_memory_ioremap(struct platform_device *pdev,
				    struct scp_native *scp)
{
	struct device_node *mem_node;
	struct reserved_mem *rmem;
	struct device *dev = &pdev->dev;
	DECLARE_BITMAP(seen, NUMS_MEM_ID);
	void __iomem *virt;
	const char *key;
	u64 offset = 0, end, aligned;
	u32 cells[3];
	int count, ret, i;

	BUILD_BUG_ON(ARRAY_SIZE(scp_reserve_mblock) != NUMS_MEM_ID);
	ret = of_property_read_string(dev->of_node, "scp_mem_key", &key);
	if (ret)
		return dev_err_probe(dev, ret, "missing scp_mem_key\n");
	mem_node = of_find_compatible_node(NULL, NULL, key);
	if (!mem_node)
		return dev_err_probe(dev, -ENODEV, "SCP shared-memory node missing\n");
	if (!of_property_read_bool(mem_node, "no-map")) {
		of_node_put(mem_node);
		return dev_err_probe(dev, -EINVAL, "SCP shared memory must be no-map\n");
	}
	rmem = of_reserved_mem_lookup(mem_node);
	of_node_put(mem_node);
	if (!rmem)
		return dev_err_probe(dev, -ENODEV, "SCP shared memory is not reserved\n");

	/* This RV SCP remap window is fixed in the platform firmware. */
	if (rmem->base < 0x40000000ULL || !rmem->size ||
	    check_add_overflow((u64)rmem->base, (u64)rmem->size, &end) ||
	    end > 0x90000000ULL)
		return dev_err_probe(dev, -ERANGE, "SCP shared memory outside remap window\n");
	scp->mem_phys = rmem->base;
	scp->mem_size = rmem->size;

	count = of_property_count_u32_elems(dev->of_node, "scp-mem-tbl");
	if (count <= 0 || count % SCP_MEMORY_TABLE_CELLS)
		return dev_err_probe(dev, -EINVAL, "invalid scp-mem-tbl\n");
	bitmap_zero(seen, NUMS_MEM_ID);
	for (i = 0; i < NUMS_MEM_ID; i++) {
		scp_reserve_mblock[i].alignment = 0;
		scp_reserve_mblock[i].size = 0;
		scp_reserve_mblock[i].start_phys = 0;
		scp_reserve_mblock[i].start_virt = 0;
	}
	for (i = 0; i < count; i += SCP_MEMORY_TABLE_CELLS) {
		ret = of_property_read_u32_index(dev->of_node, "scp-mem-tbl", i, &cells[0]);
		ret |= of_property_read_u32_index(dev->of_node, "scp-mem-tbl", i + 1, &cells[1]);
		ret |= of_property_read_u32_index(dev->of_node, "scp-mem-tbl", i + 2, &cells[2]);
		if (ret || cells[0] >= NUMS_MEM_ID || test_and_set_bit(cells[0], seen) ||
		    (cells[2] && !is_power_of_2(cells[2])))
			return dev_err_probe(dev, -EINVAL, "invalid SCP memory entry %d\n", i / 3);

		/* Corot overrides ID 0 with the secure monitor's dump allocation.
		 * Omitting this 0x480000 region shifts AOD into another user's buffer.
		 */
		if (cells[0] == SCP_A_SECDUMP_MEM_ID && scp->secure_dump) {
			ret = of_property_read_u32(dev->of_node, "secure_dump_size", &cells[1]);
			if (ret || !cells[1])
				return dev_err_probe(dev, -EINVAL, "invalid secure_dump_size\n");
		}
		scp_reserve_mblock[cells[0]].size = cells[1];
		scp_reserve_mblock[cells[0]].alignment = cells[2];
	}
	if (scp->secure_dump && !test_bit(SCP_A_SECDUMP_MEM_ID, seen))
		return dev_err_probe(dev, -EINVAL, "secure-dump memory entry missing\n");

	/* The firmware walks the entire ID table, including zero-sized entries. */
	for (i = 0; i < NUMS_MEM_ID; i++) {
		struct scp_reserve_mblock *block = &scp_reserve_mblock[i];

		if (block->alignment) {
			if (check_add_overflow(offset, (u64)block->alignment - 1, &aligned))
				return -EOVERFLOW;
			offset = aligned & ~((u64)block->alignment - 1);
		}
		if (check_add_overflow(offset, block->size, &end) || end > scp->mem_size)
			return dev_err_probe(dev, -EOVERFLOW, "SCP memory table exceeds carveout at ID %d\n", i);
		block->start_phys = scp->mem_phys + offset;
		/* Keep offsets here until the complete layout is validated and mapped. */
		block->start_virt = offset;
		offset = end;
	}
	virt = devm_ioremap_wc(dev, scp->mem_phys, scp->mem_size);
	if (!virt)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, scp_revoke_memory, scp);
	if (ret)
		return ret;
	for (i = 0; i < NUMS_MEM_ID; i++)
		scp_reserve_mblock[i].start_virt += (uintptr_t)virt;
	smp_store_release(&scp_memory_ready, true);
	dev_info(dev, "shared memory %pa/%pa, used %#llx; AOD %#llx/%#llx\n",
		 &scp->mem_phys, &scp->mem_size, offset,
		 scp_reserve_mblock[SCP_AOD_MEM_ID].start_phys,
		 scp_reserve_mblock[SCP_AOD_MEM_ID].size);
	return 0;
}

static void scp_infra_unmap(void *address)
{
	iounmap(address);
}

static int scp_map_infra(struct scp_native *scp)
{
	struct device_node *node;
	struct resource res;
	int ret;

	node = of_find_compatible_node(NULL, NULL, "mediatek,scpinfra");
	if (!node)
		return -ENODEV;
	ret = of_address_to_resource(node, 0, &res);
	of_node_put(node);
	if (ret)
		return ret;
	if (resource_size(&res) < SCP_INFRA_IRQ_CLEAR + sizeof(u32))
		return -EINVAL;
	/* The vendor SCP always-on registers share the INFRACFG mapping. */
	scp->infra = ioremap(res.start, resource_size(&res));
	if (!scp->infra)
		return -ENOMEM;
	return devm_add_action_or_reset(scp->dev, scp_infra_unmap, scp->infra);
}

static void scp_release_clock(void *data)
{
	scp_clock_release(data);
}

static void scp_reset_provider(void *data)
{
	struct scp_native *scp = data;

	WRITE_ONCE(scp->stopping, true);
	WRITE_ONCE(scp->ready, false);
	WRITE_ONCE(scp->firmware_ready, false);
	smp_store_release(&scp->boot_released, false);
	smp_store_release(&scp_memory_ready, false);
	WRITE_ONCE(scp_native, NULL);
	scp_ipidev.prdata = NULL;
	scp_mboxdev.prdata = NULL;
	scp_mboxdev.pin_send_table = NULL;
	scp_mboxdev.pin_recv_table = NULL;
	scp_mboxdev.info_table = NULL;
	scp_mboxdev.count = 0;
	scp_mboxdev.send_count = 0;
	scp_mboxdev.recv_count = 0;
	scp_mbox_info = NULL;
	scp_mbox_pin_send = NULL;
	scp_mbox_pin_recv = NULL;
}

static void scp_cancel_work(void *data)
{
	struct scp_native *scp = data;

	WRITE_ONCE(scp->stopping, true);
	WRITE_ONCE(scp->firmware_ready, false);
	WRITE_ONCE(scp->ready, false);
	cancel_delayed_work_sync(&scp->boot_timeout_work);
	cancel_work_sync(&scp->ready_work);
}

static void scp_unregister_ipi(void *data)
{
	mtk_ipi_device_unregister(&scp_ipidev);
}

static void scp_disable_irq_wake(void *data)
{
	disable_irq_wake((unsigned long)data);
}

static int scp_enable_irq_wake(struct device *dev, int irq)
{
	int ret = enable_irq_wake(irq);

	if (ret)
		return ret;
	return devm_add_action_or_reset(dev, scp_disable_irq_wake,
				       (void *)(unsigned long)irq);
}

static int scp_device_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct scp_native *scp;
	struct resource *res;
	char name[32];
	int ret, i, irq;

	if (READ_ONCE(scp_native))
		return -EBUSY;
	scp = devm_kzalloc(dev, sizeof(*scp), GFP_KERNEL);
	if (!scp)
		return -ENOMEM;
	scp->dev = dev;
	spin_lock_init(&scp->awake_lock);
	INIT_WORK(&scp->ready_work, scp_ready_work);
	INIT_DELAYED_WORK(&scp->boot_timeout_work, scp_boot_timeout_work);
	if (!scp_dt_enabled(dev->of_node, "core_0"))
		return -ENODEV;
	scp->secure_dump = scp_dt_enabled(dev->of_node, "secure_dump");
	scp->pm_notify = scp_dt_enabled(dev->of_node, "scp_pm_notify");
	ret = of_property_read_u32(dev->of_node, "core_nums", &scp->core_count);
	if (ret || !scp->core_count || scp->core_count > ARRAY_SIZE(scp->core))
		return dev_err_probe(dev, -EINVAL, "invalid core_nums\n");
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "scp_sram_base");
	ret = of_property_read_u32(dev->of_node, "scp_sramSize", &scp->tcm_size);
	if (ret || !res || !scp->tcm_size || scp->tcm_size > resource_size(res))
		return dev_err_probe(dev, -EINVAL, "invalid SCP TCM size\n");

	/* Hold the actual 26 MHz source while touching SCP registers and booting. */
	ret = scp_calibration_prepare(scp);
	if (ret)
		return dev_err_probe(dev, ret, "SCP clock/calibration setup failed\n");
	ret = devm_add_action_or_reset(dev, scp_release_clock, scp);
	if (ret)
		return ret;
	scp->cfg = devm_platform_ioremap_resource_byname(pdev, "scp_cfgreg");
	if (IS_ERR(scp->cfg))
		return PTR_ERR(scp->cfg);
	scp->clkctrl = devm_platform_ioremap_resource_byname(pdev, "scp_clkreg");
	if (IS_ERR(scp->clkctrl))
		return PTR_ERR(scp->clkctrl);
	for (i = 0; i < scp->core_count; i++) {
		snprintf(name, sizeof(name), "scp_cfgreg_core%d", i);
		scp->core[i] = devm_platform_ioremap_resource_byname(pdev, name);
		if (IS_ERR(scp->core[i]))
			return PTR_ERR(scp->core[i]);
	}
	ret = scp_map_infra(scp);
	if (ret)
		return dev_err_probe(dev, ret, "SCP always-on wake registers unavailable\n");

	WRITE_ONCE(scp_native, scp);
	scp_ipidev.prdata = scp;
	scp_mboxdev.prdata = scp;
	ret = devm_add_action_or_reset(dev, scp_reset_provider, scp);
	if (ret)
		return ret;
	ret = scp_reserve_memory_ioremap(pdev, scp);
	if (ret)
		return ret;
	ret = scp_ipi_tables_init(pdev, scp);
	if (ret)
		return dev_err_probe(dev, ret, "invalid SCP mailbox/IPI layout\n");
	/* Revoke exported pointers before devres frees the parsed pin tables. */
	devm_remove_action(dev, scp_reset_provider, scp);
	ret = devm_add_action_or_reset(dev, scp_reset_provider, scp);
	if (ret)
		return ret;

	/* Endpoints and the ready/error/ACK handlers exist before IRQs can fire. */
	ret = mtk_ipi_device_register(&scp_ipidev, pdev, &scp_mboxdev, SCP_IPI_COUNT);
	if (ret)
		return dev_err_probe(dev, ret, "SCP IPI device registration failed\n");
	ret = devm_add_action_or_reset(dev, scp_unregister_ipi, scp);
	if (ret)
		return ret;
	ret = scp_ipi_register_handlers(scp);
	if (ret)
		return dev_err_probe(dev, ret, "SCP IPI handler registration failed\n");
	for (i = 0; i < scp_mboxdev.count; i++) {
		ret = mtk_mbox_probe(pdev, &scp_mboxdev, i);
		if (ret)
			return dev_err_probe(dev, ret, "SCP mailbox %d probe failed\n", i);
		ret = scp_enable_irq_wake(dev, scp_mbox_info[i].irq_num);
		if (ret)
			return dev_err_probe(dev, ret, "SCP mailbox %d cannot wake AP\n", i);
	}
	for (i = 0; i < scp->core_count; i++) {
		snprintf(name, sizeof(name), "ipc%d", i);
		irq = platform_get_irq_byname(pdev, name);
		if (irq < 0)
			return irq;
		ret = devm_request_irq(dev, irq, scp_ipc_irq, 0, name, scp);
		if (ret)
			return ret;
		ret = scp_enable_irq_wake(dev, irq);
		if (ret)
			return ret;
	}
	/* This action runs before devres frees IRQs, endpoints and their buffers. */
	ret = devm_add_action_or_reset(dev, scp_cancel_work, scp);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, scp);
	schedule_delayed_work(&scp->boot_timeout_work,
			      msecs_to_jiffies(SCP_BOOT_TIMEOUT_MS));
	if (scp->secure_dump) {
		ret = scp_smc(SCP_SIP_CONTROL, SCP_SIP_RESET_RELEASE, 0);
		if (ret)
			return dev_err_probe(dev, ret, "secure monitor refused SCP firmware release\n");
	} else {
		writel(scp->mem_phys, scp->core[0] + SCP_DRAM_RESV_ADDR);
		writel(scp->mem_size, scp->core[0] + SCP_DRAM_RESV_SIZE);
		writel(1, scp->core[0] + SCP_CORE_RSTN_CLR);
		mb();
	}
	smp_store_release(&scp->boot_released, true);
	/* A preloaded firmware READY may have arrived while IRQs were set up. */
	if (READ_ONCE(scp->firmware_ready))
		schedule_work(&scp->ready_work);
	dev_info(dev, "RV SCP released; waiting for firmware READY and calibration ACKs\n");
	return 0;
}

static int scp_suspend_noirq(struct device *dev)
{
	return scp_pm_notify(dev_get_drvdata(dev), 0);
}

static int scp_resume_noirq(struct device *dev)
{
	return scp_pm_notify(dev_get_drvdata(dev), 1);
}

static const struct dev_pm_ops scp_pm_ops = {
	.suspend_noirq = scp_suspend_noirq,
	.resume_noirq = scp_resume_noirq,
};

static void scp_shutdown(struct platform_device *pdev)
{
	struct scp_native *scp = platform_get_drvdata(pdev);
	u32 command[2] = { 5, 1 }; /* AP_SHUTDOWN from vendor scp_scpctl.c */
	int ret;

	if (!scp || !READ_ONCE(scp->ready))
		return;
	ret = mtk_ipi_send(&scp_ipidev, IPI_OUT_SCPCTL_1, IPI_SEND_POLLING,
			   command, ARRAY_SIZE(command), 20);
	if (ret)
		dev_warn(&pdev->dev, "SCP shutdown message failed: %d\n", ret);
	mdelay(10);
	WRITE_ONCE(scp->stopping, true);
}

static const struct of_device_id scp_of_match[] = {
	{ .compatible = "mediatek,scp" },
	{ }
};

static struct platform_driver scp_driver = {
	.probe = scp_device_probe,
	.shutdown = scp_shutdown,
	.driver = {
		.name = "mtk-rv-scp",
		.of_match_table = scp_of_match,
		.pm = &scp_pm_ops,
		.suppress_bind_attrs = true,
	},
};

/* The service must be registered before built-in display/AOD consumers. */
static int __init scp_native_init(void)
{
	return platform_driver_register(&scp_driver);
}
subsys_initcall(scp_native_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek bootloader-loaded RV SCP memory and IPI provider");
