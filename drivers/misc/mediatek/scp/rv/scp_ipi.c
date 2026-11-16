// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 * DT pin layout and awake handshakes from corot-t-oss scp_wrapper_ipi.c and
 * scp_awake.c, with checked native-kernel allocation and error handling.
 */
#include <linux/bitmap.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/soc/mediatek/mtk_sip_svc.h>
#include "scp_native.h"
#include "scp_ipi_table.h"

static int scp_layout_mailbox(unsigned int mbox)
{
	unsigned int index = 0, words, i;
	struct mtk_mbox_info *info = &scp_mbox_info[mbox];

	/* RV mailbox IRQ indices count 64-bit slots; API lengths count words.
	 * Firmware packs all send pins first, followed by all receive pins.
	 */
	for (i = 0; i < scp_mboxdev.send_count; i++) {
		struct mtk_mbox_pin_send *pin = &scp_mbox_pin_send[i];

		if (pin->mbox != mbox)
			continue;
		words = DIV_ROUND_UP(pin->msg_size, 2);
		if (index + words > 32 || (index + words) * 2 > info->slot)
			return -EOVERFLOW;
		pin->pin_index = index;
		pin->offset = index * 2;
		index += words;
	}
	for (i = 0; i < scp_mboxdev.recv_count; i++) {
		struct mtk_mbox_pin_recv *pin = &scp_mbox_pin_recv[i];

		if (pin->mbox != mbox)
			continue;
		words = DIV_ROUND_UP(pin->msg_size, 2);
		if (index + words > 32 || (index + words) * 2 > info->slot)
			return -EOVERFLOW;
		pin->pin_index = index;
		pin->offset = index * 2;
		index += words;
	}
	return 0;
}

int scp_ipi_tables_init(struct platform_device *pdev, struct scp_native *scp)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	DECLARE_BITMAP(seen_send, SCP_IPI_COUNT);
	DECLARE_BITMAP(seen_recv, SCP_IPI_COUNT);
	struct resource *res;
	u32 cells[7], count, mode = 0;
	unsigned int recv_cells, i, j;
	char name[16];
	int send_elements, recv_elements, ret;

	ret = of_property_read_u32(node, "mbox_count", &count);
	if (ret || !count || count > SCP_MBOX_TOTAL)
		return -EINVAL;
	ret = of_property_read_u32(node, "#recv_cells_mode", &mode);
	if ((ret && ret != -EINVAL) || mode > 1)
		return -EINVAL;
	recv_cells = mode == 1 ? 7 : 4;
	send_elements = of_property_count_u32_elems(node, "send_table");
	recv_elements = of_property_count_u32_elems(node, "recv_table");
	if (send_elements <= 0 || send_elements % 3 || recv_elements <= 0 ||
	    recv_elements % recv_cells || send_elements / 3 > SCP_IPI_COUNT ||
	    recv_elements / recv_cells > SCP_IPI_COUNT)
		return -EINVAL;

	scp_mboxdev.count = count;
	scp_mboxdev.send_count = send_elements / 3;
	scp_mboxdev.recv_count = recv_elements / recv_cells;
	scp_mbox_info = devm_kcalloc(dev, count, sizeof(*scp_mbox_info), GFP_KERNEL);
	scp_mbox_pin_send = devm_kcalloc(dev, scp_mboxdev.send_count,
				       sizeof(*scp_mbox_pin_send), GFP_KERNEL);
	scp_mbox_pin_recv = devm_kcalloc(dev, scp_mboxdev.recv_count,
				       sizeof(*scp_mbox_pin_recv), GFP_KERNEL);
	if (!scp_mbox_info || !scp_mbox_pin_send || !scp_mbox_pin_recv)
		return -ENOMEM;
	scp_mboxdev.info_table = scp_mbox_info;
	scp_mboxdev.pin_send_table = scp_mbox_pin_send;
	scp_mboxdev.pin_recv_table = scp_mbox_pin_recv;
	for (i = 0; i < count; i++) {
		snprintf(name, sizeof(name), "mbox%u_base", i);
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
		if (!res || resource_size(res) != 64 * MBOX_SLOT_SIZE)
			return -EINVAL;
		scp_mbox_info[i].id = i;
		scp_mbox_info[i].slot = 64;
		scp_mbox_info[i].is64d = true;
		scp_mbox_info[i].mbdev = &scp_mboxdev;
	}

	bitmap_zero(seen_send, SCP_IPI_COUNT);
	bitmap_zero(seen_recv, SCP_IPI_COUNT);
	for (i = 0; i < scp_mboxdev.send_count; i++) {
		for (j = 0; j < 3; j++) {
			ret = of_property_read_u32_index(node, "send_table", i * 3 + j, &cells[j]);
			if (ret)
				return ret;
		}
		if (cells[0] >= SCP_IPI_COUNT || cells[1] >= count || !cells[2] ||
		    cells[2] > 64 || test_and_set_bit(cells[0], seen_send))
			return -EINVAL;
		scp_mbox_pin_send[i].chan_id = cells[0];
		scp_mbox_pin_send[i].mbox = cells[1];
		scp_mbox_pin_send[i].msg_size = cells[2];
	}
	for (i = 0; i < scp_mboxdev.recv_count; i++) {
		memset(cells, 0, sizeof(cells));
		for (j = 0; j < recv_cells; j++) {
			ret = of_property_read_u32_index(node, "recv_table",
						 i * recv_cells + j, &cells[j]);
			if (ret)
				return ret;
		}
		if (cells[0] >= SCP_IPI_COUNT || cells[1] >= count || !cells[2] ||
		    cells[2] > 64 || cells[3] > MBOX_RECV_ACK || cells[4] > 1 ||
		    cells[5] > MBOX_BUF_COPY_DONE || cells[6] > MBOX_CB_IN_PROCESS ||
		    test_and_set_bit(cells[0], seen_recv))
			return -EINVAL;
		scp_mbox_pin_recv[i].chan_id = cells[0];
		scp_mbox_pin_recv[i].mbox = cells[1];
		scp_mbox_pin_recv[i].msg_size = cells[2];
		scp_mbox_pin_recv[i].recv_opt = cells[3];
		scp_mbox_pin_recv[i].lock = cells[4];
		scp_mbox_pin_recv[i].buf_full_opt = cells[5];
		scp_mbox_pin_recv[i].cb_ctx_opt = cells[6];
	}
	for (i = 0; i < count; i++) {
		ret = scp_layout_mailbox(i);
		if (ret)
			return ret;
	}
	if (!test_bit(IPI_OUT_SCP_AOD, seen_send) ||
	    !test_bit(IPI_IN_SCP_AOD, seen_recv))
		return dev_err_probe(dev, -ENODEV, "SCP AOD channels absent from firmware table\n");
	dev_info(dev, "%u mailboxes, %u send pins, %u receive pins\n", count,
		 scp_mboxdev.send_count, scp_mboxdev.recv_count);
	return 0;
}

static int scp_awake_request(struct scp_native *scp, u32 request)
{
	u32 status;
	int ret;

	writel(SCP_AWAKE_KEY | request, scp->infra + SCP_INFRA_IRQ_SET);
	ret = readl_poll_timeout_atomic(scp->infra + SCP_INFRA_IRQ_SET, status,
			 (status & SCP_AWAKE_KEY) != SCP_AWAKE_KEY || !(status & request),
			 10, SCP_AWAKE_TIMEOUT_US);
	writel(SCP_AWAKE_KEY | request, scp->infra + SCP_INFRA_IRQ_CLEAR);
	if (!ret && (status & SCP_AWAKE_KEY) != SCP_AWAKE_KEY)
		ret = -EIO;
	if (ret) {
		WRITE_ONCE(scp->ready, false);
		WRITE_ONCE(scp->firmware_ready, false);
		dev_err_ratelimited(scp->dev, "SCP awake request %#x failed: %d, status %#x\n",
				    request, ret, status);
	}
	return ret;
}

int scp_awake_lock(void *data)
{
	struct scp_native *scp = READ_ONCE(scp_native);
	unsigned long flags;
	int ret = 0;

	if (!scp || (data && data != scp) || !smp_load_acquire(&scp->boot_released) ||
	    !READ_ONCE(scp->firmware_ready) ||
	    READ_ONCE(scp->stopping))
		return -EHOSTDOWN;
	spin_lock_irqsave(&scp->awake_lock, flags);
	if (!READ_ONCE(scp->firmware_ready) || READ_ONCE(scp->stopping)) {
		ret = -EHOSTDOWN;
		goto out;
	}
	if (!scp->awake_count)
		ret = scp_awake_request(scp, SCP_AP_AWAKE_LOCK);
	if (!ret)
		scp->awake_count++;
out:
	spin_unlock_irqrestore(&scp->awake_lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(scp_awake_lock);

int scp_awake_unlock(void *data)
{
	struct scp_native *scp = READ_ONCE(scp_native);
	unsigned long flags;
	int ret = 0;

	if (!scp || (data && data != scp))
		return -EHOSTDOWN;
	spin_lock_irqsave(&scp->awake_lock, flags);
	if (!scp->awake_count) {
		ret = -EINVAL;
		goto out;
	}
	if (scp->awake_count == 1)
		ret = scp_awake_request(scp, SCP_AP_AWAKE_UNLOCK);
	if (!ret)
		scp->awake_count--;
out:
	spin_unlock_irqrestore(&scp->awake_lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(scp_awake_unlock);

int scp_clear_spm(void *data)
{
	struct scp_native *scp = data;

	if (!scp || READ_ONCE(scp->stopping))
		return -ESHUTDOWN;
	writel(1, scp->cfg + SCP_TO_SPM_REG);
	return 0;
}

static int scp_ready_handler(unsigned int id, void *data, void *message,
			     unsigned int length)
{
	struct scp_native *scp = data;
	u32 image_size;

	if (length != sizeof(image_size) || READ_ONCE(scp->stopping))
		return -EINVAL;
	image_size = *(u32 *)message;
	if (image_size != scp->tcm_size) {
		dev_err(scp->dev, "SCP READY image size %#x differs from DT %#x\n",
			image_size, scp->tcm_size);
		return -EINVAL;
	}
	/* Readiness comes only from the real firmware message, never from probe. */
	if (!READ_ONCE(scp->firmware_ready)) {
		WRITE_ONCE(scp->firmware_ready, true);
		if (smp_load_acquire(&scp->boot_released))
			schedule_work(&scp->ready_work);
	}
	return 0;
}

static int scp_error_handler(unsigned int id, void *data, void *message,
			     unsigned int length)
{
	struct scp_native *scp = data;
	struct scp_error_info *error = message;

	if (length != sizeof(*error))
		return -EINVAL;
	error->context[sizeof(error->context) - 1] = '\0';
	dev_err(scp->dev, "SCP error on IPI %u: case %u sensor %u: %s\n",
		id, error->case_id, error->sensor_id, error->context);
	return 0;
}

int scp_ipi_register_handlers(struct scp_native *scp)
{
	const unsigned int ready_id[] = { IPI_IN_SCP_READY_0, IPI_IN_SCP_READY_1 };
	const unsigned int error_id[] = { IPI_IN_SCP_ERROR_INFO_0, IPI_IN_SCP_ERROR_INFO_1 };
	const unsigned int sleep_id[] = { IPI_OUT_C_SLEEP_0, IPI_OUT_C_SLEEP_1 };
	unsigned int i, ready_count = 0;
	int ret;

	for (i = 0; i < scp->core_count; i++) {
		struct mtk_mbox_pin_recv *pin = scp_ipidev.table[ready_id[i]].pin_recv;

		if (pin) {
			if (pin->msg_size != 1 || pin->cb_ctx_opt != MBOX_CB_IN_ISR ||
			    pin->recv_opt != MBOX_RECV_MESSAGE)
				return -EINVAL;
			ret = mtk_ipi_register(&scp_ipidev, ready_id[i], scp_ready_handler,
					       scp, &scp->ready_msg[i]);
			if (ret)
				return ret;
			ready_count++;
		}
		pin = scp_ipidev.table[error_id[i]].pin_recv;
		if (pin) {
			if (pin->msg_size * MBOX_SLOT_SIZE != sizeof(scp->error_msg[i]) ||
			    pin->cb_ctx_opt != MBOX_CB_IN_ISR)
				return -EINVAL;
			ret = mtk_ipi_register(&scp_ipidev, error_id[i], scp_error_handler,
					       scp, &scp->error_msg[i]);
			if (ret)
				return ret;
		}
		pin = scp_ipidev.table[sleep_id[i]].pin_recv;
		if (pin) {
			if (pin->msg_size != 1 || pin->recv_opt != MBOX_RECV_ACK)
				return -EINVAL;
			ret = mtk_ipi_register(&scp_ipidev, sleep_id[i], NULL, scp,
					       &scp->sleep_ack[i]);
			if (ret)
				return ret;
		} else if (scp->do_calibration) {
			return -ENODEV;
		}
	}
	return ready_count ? 0 : -ENODEV;
}

void scp_ready_work(struct work_struct *work)
{
	struct scp_native *scp = container_of(work, struct scp_native, ready_work);
	int ret;

	if (READ_ONCE(scp->stopping) || !smp_load_acquire(&scp->boot_released))
		return;
	writel(0xff, scp->cfg + SCP_TO_SPM_REG);
	ret = scp_calibration_sync(scp);
	if (!ret)
		ret = scp_clock_release(scp);
	if (ret || READ_ONCE(scp->stopping) || !READ_ONCE(scp->firmware_ready)) {
		WRITE_ONCE(scp->ready, false);
		WRITE_ONCE(scp->firmware_ready, false);
		dev_err(scp->dev, "SCP READY calibration/resource handshake failed: %d\n", ret);
		return;
	}
	cancel_delayed_work(&scp->boot_timeout_work);
	WRITE_ONCE(scp->ready, true);
	dev_info(scp->dev, "RV SCP firmware ready; low-power clock handoff complete\n");
	scp_notify_ready();
}

void scp_boot_timeout_work(struct work_struct *work)
{
	struct scp_native *scp = container_of(to_delayed_work(work),
					     struct scp_native, boot_timeout_work);

	if (!READ_ONCE(scp->ready) && !READ_ONCE(scp->stopping))
		dev_err(scp->dev, "SCP firmware did not become ready within %u ms (core0 PC %#x)\n",
			SCP_BOOT_TIMEOUT_MS, readl(scp->core[0] + SCP_CORE_MON_PC));
}

irqreturn_t scp_ipc_irq(int irq, void *data)
{
	struct scp_native *scp = data;
	unsigned int i;
	bool handled = false;
	int ret;

	for (i = 0; i < scp->core_count; i++) {
		if (!(readl(scp->core[i] + SCP_CORE_WDT_IRQ) & BIT(0)))
			continue;
		handled = true;
		WRITE_ONCE(scp->ready, false);
		WRITE_ONCE(scp->firmware_ready, false);
		dev_err_ratelimited(scp->dev, "SCP core%u watchdog, PC %#x; service unavailable\n",
				    i, readl(scp->core[i] + SCP_CORE_MON_PC));
		if (scp->secure_dump) {
			ret = scp_smc(SCP_SIP_CONTROL, SCP_SIP_WDT_CLEAR, i);
			if (ret)
				dev_err_ratelimited(scp->dev, "SCP watchdog clear rejected: %d\n", ret);
		} else {
			writel(BIT(0), scp->core[i] + SCP_CORE_WDT_IRQ);
		}
	}
	return handled ? IRQ_HANDLED : IRQ_NONE;
}

int scp_pm_notify(struct scp_native *scp, unsigned int event)
{
	const unsigned int channels[] = { IPI_OUT_SCP_PM_NOTIFY_0, IPI_OUT_SCP_PM_NOTIFY_1 };
	unsigned int i;
	int ret;

	if (!scp || !scp->pm_notify)
		return 0;
	if (!READ_ONCE(scp->ready))
		return -EHOSTDOWN;
	for (i = 0; i < scp->core_count; i++) {
		ret = mtk_ipi_send(&scp_ipidev, channels[i], IPI_SEND_POLLING,
				   &event, 1, 20);
		if (ret) {
			dev_err(scp->dev, "SCP PM event %u send failed: %d\n", event, ret);
			return ret;
		}
	}
	return 0;
}
