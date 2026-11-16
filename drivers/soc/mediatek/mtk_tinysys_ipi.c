// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/sched/clock.h>
#include <linux/rpmsg.h>
#include <linux/rpmsg/mtk_rpmsg_mbox.h>
#include <linux/soc/mediatek/mtk-mbox.h>
#include <linux/soc/mediatek/mtk_tinysys_ipi.h>

#define MS_TO_NS(x) ((u64)(x) * 1000000ULL)
#define ts_before(ts) (cpu_clock(0) < (ts))
#define ipi_delay() udelay(10)

#define ipi_echo(en, fmt, args...) \
	({ if (en) pr_info(fmt, ##args); })


static void ipi_isr_cb(struct mtk_mbox_pin_recv *pin, void *priv);

static int ipi_check_channel(struct mtk_ipi_device *ipidev, int id)
{
	if (!ipidev || !READ_ONCE(ipidev->ipi_inited) || !ipidev->table)
		return IPI_DEV_ILLEGAL;
	if (id < 0 || id >= ipidev->channel_count)
		return IPI_UNAVAILABLE;
	return 0;
}

static void ipi_free_channels(struct mtk_ipi_chan_table *table,
			      unsigned int count, struct mtk_rpmsg_device *rpdev)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		if (table[i].ept)
			rpmsg_destroy_ept(table[i].ept);
		kfree(table[i].rpchan);
	}
	kfree(table);
	if (rpdev)
		device_unregister(&rpdev->rpdev.dev);
}

/* WAIT callers use the mutex; a pin flag also serializes them with atomic
 * POLLING callers. A bad context or an occupied pin is an error, not a panic.
 */
static int ipi_lock_send(struct mtk_mbox_pin_send *pin, int opt,
			 unsigned long *flags)
{
	if (opt != IPI_SEND_WAIT && opt != IPI_SEND_POLLING)
		return -EINVAL;
	if (opt == IPI_SEND_WAIT) {
		if (in_atomic() || in_interrupt() || irqs_disabled())
			return -EWOULDBLOCK;
		mutex_lock(&pin->mutex_send);
	}
	spin_lock_irqsave(&pin->pin_lock, *flags);
	if (pin->lock == MBOX_PIN_BUSY) {
		spin_unlock_irqrestore(&pin->pin_lock, *flags);
		if (opt == IPI_SEND_WAIT)
			mutex_unlock(&pin->mutex_send);
		return IPI_PIN_BUSY;
	}
	pin->lock = MBOX_PIN_BUSY;
	if (opt == IPI_SEND_WAIT)
		spin_unlock_irqrestore(&pin->pin_lock, *flags);
	return 0;
}

static void ipi_unlock_send(struct mtk_mbox_pin_send *pin, int opt,
			    unsigned long flags)
{
	if (opt == IPI_SEND_WAIT)
		spin_lock_irqsave(&pin->pin_lock, flags);
	pin->lock = MBOX_DONE;
	spin_unlock_irqrestore(&pin->pin_lock, flags);
	if (opt == IPI_SEND_WAIT)
		mutex_unlock(&pin->mutex_send);
}


static void ipi_monitor(struct mtk_ipi_device *ipidev, int id, int stage)
{
	unsigned long flags = 0;
	struct mtk_ipi_chan_table *chan;

	chan = &ipidev->table[id];

	spin_lock_irqsave(&ipidev->lock_monitor, flags);
	switch (stage) {
	case SEND_MSG:
		chan->ipi_seqno++;
		chan->ipi_record[0].idx = 0;
		chan->ipi_record[0].ts = cpu_clock(0);
		chan->ipi_record[1].idx = 4;
		chan->ipi_record[1].ts = 0;
		chan->ipi_record[2].idx = 5;
		chan->ipi_record[2].ts = 0;
		ipi_echo(ipidev->mbdev->log_enable,
			"%s: IPI_%d send msg (#%d)\n",
			ipidev->name, id, chan->ipi_seqno);
		break;
	case ISR_RECV_MSGV:
		chan->ipi_seqno++;
		chan->ipi_record[0].idx = 1;
		chan->ipi_record[0].ts = cpu_clock(0);
		chan->ipi_record[1].idx = 2;
		chan->ipi_record[1].ts = 0;
		chan->ipi_record[2].idx = 3;
		chan->ipi_record[2].ts = 0;
		break;
	case RECV_MSG:
		chan->ipi_record[1].ts = cpu_clock(0);
		ipi_echo(ipidev->mbdev->log_enable,
			"%s: IPI_%d recv msg (#%d)\n",
			ipidev->name, id, chan->ipi_seqno);
		break;
	case RECV_ACK:
		chan->ipi_record[2].ts = cpu_clock(0);
		ipi_echo(ipidev->mbdev->log_enable,
			"%s: IPI_%d recv ack (#%d)\n",
			ipidev->name, id, chan->ipi_seqno);
		break;
	case ISR_RECV_ACK:
		chan->ipi_record[1].ts = cpu_clock(0);
		break;
	case SEND_ACK:
		chan->ipi_record[2].ts = cpu_clock(0);
		ipi_echo(ipidev->mbdev->log_enable,
			"%s: IPI_%d send ack (#%d)\n",
			ipidev->name, id, chan->ipi_seqno);
		break;
	default:
		break;
	}

	chan->ipi_stage = stage;

	spin_unlock_irqrestore(&ipidev->lock_monitor, flags);
}

static void ipi_timeout_dump(struct mtk_ipi_device *ipidev, int ipi_id)
{
	unsigned long flags = 0;
	struct mtk_ipi_chan_table *chan;

	chan = &ipidev->table[ipi_id];

	spin_lock_irqsave(&ipidev->lock_monitor, flags);

	pr_err("Error: %s IPI %d timeout at %lld (last done is IPI %d)\n",
		 ipidev->name, ipi_id, cpu_clock(0), ipidev->ipi_last_done);

	pr_err("IPI %d: seqno=%d, state=%d, t%d=%lld, t%d=%lld, t%d=%lld (trysend %d, polling %d\n",
		ipi_id, chan->ipi_seqno, chan->ipi_stage,
		chan->ipi_record[0].idx, chan->ipi_record[0].ts,
		chan->ipi_record[1].idx, chan->ipi_record[1].ts,
		chan->ipi_record[2].idx, chan->ipi_record[2].ts,
		chan->trysend_count, chan->polling_count);

	spin_unlock_irqrestore(&ipidev->lock_monitor, flags);
}

void ipi_monitor_dump(struct mtk_ipi_device *ipidev)
{
	int i;
	unsigned int ipi_chan_count;
	unsigned long flags = 0;
	struct mtk_ipi_chan_table *chan;

	ipi_chan_count = ipidev->mrpdev->rpdev.src;

	pr_info("%s dump IPIMonitor:\n", ipidev->name);

	spin_lock_irqsave(&ipidev->lock_monitor, flags);

	for (i = 0; i < ipi_chan_count; i++) {
		chan = &ipidev->table[i];
		if (chan->ipi_stage == UNUSED)
			continue;
		pr_info("IPI %d: seqno=%d, state=%d, t%d=%lld, t%d=%lld, t%d=%lld\n",
			i, chan->ipi_seqno, chan->ipi_stage,
			chan->ipi_record[0].idx, chan->ipi_record[0].ts,
			chan->ipi_record[1].idx, chan->ipi_record[1].ts,
			chan->ipi_record[2].idx, chan->ipi_record[2].ts);
	}

	spin_unlock_irqrestore(&ipidev->lock_monitor, flags);
}
EXPORT_SYMBOL(ipi_monitor_dump);

int mtk_ipi_device_register(struct mtk_ipi_device *ipidev,
		struct platform_device *pdev, struct mtk_mbox_device *mbox,
		unsigned int ipi_chan_count)
{
	struct mtk_ipi_chan_table *table;
	struct mtk_rpmsg_device *rpdev;
	char name[RPMSG_NAME_SIZE];
	unsigned int i;
	int ret;

	if (!ipidev || !pdev || !mbox || !ipi_chan_count || !mbox->count ||
	    mbox->count > 16 || !mbox->info_table ||
	    (mbox->send_count && !mbox->pin_send_table) ||
	    (mbox->recv_count && !mbox->pin_recv_table))
		return -EINVAL;
	if (!ipidev->name)
		return -ENXIO;
	if (ipidev->table || ipidev->mrpdev || ipidev->ipi_inited)
		return -EBUSY;
	table = kcalloc(ipi_chan_count, sizeof(*table), GFP_KERNEL);
	if (!table)
		return -ENOMEM;
	for (i = 0; i < ipi_chan_count; i++) {
		table[i].ipi_stage = UNUSED;
		atomic_set(&table[i].holder, 0);
	}
	for (i = 0; i < mbox->send_count; i++) {
		struct mtk_mbox_pin_send *pin = &mbox->pin_send_table[i];

		if (pin->chan_id >= ipi_chan_count || pin->mbox >= mbox->count ||
		    !pin->msg_size || pin->pin_index >= 32 ||
		    pin->offset + pin->msg_size > mbox->info_table[pin->mbox].slot ||
		    table[pin->chan_id].pin_send) {
			ret = -EINVAL;
			goto free_table;
		}
		table[pin->chan_id].pin_send = pin;
		mutex_init(&pin->mutex_send);
		init_completion(&pin->comp_ack);
		spin_lock_init(&pin->pin_lock);
		pin->lock = MBOX_DONE;
	}
	for (i = 0; i < mbox->recv_count; i++) {
		struct mtk_mbox_pin_recv *pin = &mbox->pin_recv_table[i];

		if (pin->chan_id >= ipi_chan_count || pin->mbox >= mbox->count ||
		    !pin->msg_size || pin->pin_index >= 32 ||
		    pin->offset + pin->msg_size > mbox->info_table[pin->mbox].slot ||
		    table[pin->chan_id].pin_recv) {
			ret = -EINVAL;
			goto free_table;
		}
		table[pin->chan_id].pin_recv = pin;
		init_completion(&pin->notify);
		spin_lock_init(&pin->pin_lock);
	}
	rpdev = mtk_rpmsg_create_device(pdev, mbox, ipi_chan_count);
	if (!rpdev) {
		ret = IPI_RPMSG_ERR;
		goto free_table;
	}
	for (i = 0; i < ipi_chan_count; i++) {
		snprintf(name, sizeof(name), "%s_ipi#%u", ipidev->name, i);
		table[i].rpchan = mtk_rpmsg_create_channel(rpdev, i, name);
		if (!table[i].rpchan) {
			ret = -ENOMEM;
			goto free_device;
		}
		table[i].ept = rpmsg_create_ept(&rpdev->rpdev, NULL,
					       table[i].rpchan, table[i].rpchan->info);
		if (!table[i].ept) {
			ret = -ENOMEM;
			goto free_device;
		}
	}
	mutex_init(&ipidev->mutex_ipi_reg);
	spin_lock_init(&ipidev->lock_monitor);
	ipidev->ipi_last_done = -1;
	ipidev->mrpdev = rpdev;
	ipidev->table = table;
	ipidev->channel_count = ipi_chan_count;
	ipidev->mbdev = mbox;
	mbox->ipi_cb = ipi_isr_cb;
	mbox->ipi_priv = ipidev;
	WRITE_ONCE(ipidev->ipi_inited, 1);
	pr_info("%s (with %u IPI) has registered.\n", ipidev->name, ipi_chan_count);
	return IPI_ACTION_DONE;
free_device:
	ipi_free_channels(table, ipi_chan_count, rpdev);
	return ret;
free_table:
	kfree(table);
	return ret;
}
EXPORT_SYMBOL(mtk_ipi_device_register);

void mtk_ipi_device_unregister(struct mtk_ipi_device *ipidev)
{
	unsigned long flags;
	unsigned int i;

	if (!ipidev || !ipidev->table)
		return;
	/* The provider has already stopped mailbox IRQs and dependent users. */
	WRITE_ONCE(ipidev->ipi_inited, 0);
	ipidev->mbdev->ipi_cb = NULL;
	ipidev->mbdev->ipi_priv = NULL;
	for (i = 0; i < ipidev->mbdev->recv_count; i++) {
		struct mtk_mbox_pin_recv *pin = &ipidev->mbdev->pin_recv_table[i];

		spin_lock_irqsave(&pin->pin_lock, flags);
		pin->mbox_pin_cb = NULL;
		pin->prdata = NULL;
		pin->pin_buf = NULL;
		spin_unlock_irqrestore(&pin->pin_lock, flags);
		complete_all(&pin->notify);
	}
	ipi_free_channels(ipidev->table, ipidev->channel_count, ipidev->mrpdev);
	ipidev->table = NULL;
	ipidev->mrpdev = NULL;
	ipidev->channel_count = 0;
}
EXPORT_SYMBOL(mtk_ipi_device_unregister);

int mtk_ipi_device_reset(struct mtk_ipi_device *ipidev)
{
	int index, chan_count;
	unsigned long flags = 0;
	struct mtk_ipi_chan_table *chan_table;

	if (!ipidev->table)
		return -ENXIO;

	ipidev->ipi_inited = 0;
	chan_count = ipidev->mrpdev->rpdev.src;
	chan_table = ipidev->table;

	spin_lock_irqsave(&ipidev->lock_monitor, flags);

	for (index = 0; index < chan_count; index++) {
		chan_table[index].ipi_stage = UNUSED;
		chan_table[index].ipi_seqno = 0;
	}

	ipidev->ipi_last_done = -1;

	spin_unlock_irqrestore(&ipidev->lock_monitor, flags);

	mtk_mbox_reset_record(ipidev->mbdev);

	ipidev->ipi_inited = 1;

	pr_info("%s (with %d IPI) has reset.\n", ipidev->name, chan_count);

	return IPI_ACTION_DONE;
}
EXPORT_SYMBOL(mtk_ipi_device_reset);

int mtk_ipi_register(struct mtk_ipi_device *ipidev, int ipi_id,
		mbox_pin_cb_t cb, void *prdata, void *msg)
{
	struct mtk_mbox_pin_recv *pin;
	unsigned long flags;
	int ret = ipi_check_channel(ipidev, ipi_id);

	if (ret)
		return ret;
	if (!msg)
		return IPI_NO_MSGBUF;
	if (in_atomic() || in_interrupt() || irqs_disabled())
		return -EWOULDBLOCK;
	pin = ipidev->table[ipi_id].pin_recv;
	if (!pin)
		return IPI_UNAVAILABLE;
	mutex_lock(&ipidev->mutex_ipi_reg);
	spin_lock_irqsave(&pin->pin_lock, flags);
	if (pin->pin_buf) {
		ret = IPI_DUPLEX;
	} else {
		pin->mbox_pin_cb = cb;
		pin->prdata = prdata;
		pin->pin_buf = msg;
	}
	spin_unlock_irqrestore(&pin->pin_lock, flags);
	mutex_unlock(&ipidev->mutex_ipi_reg);
	return ret;
}
EXPORT_SYMBOL(mtk_ipi_register);

int mtk_ipi_unregister(struct mtk_ipi_device *ipidev, int ipi_id)
{
	struct mtk_mbox_pin_recv *pin;
	unsigned long flags;
	int ret = ipi_check_channel(ipidev, ipi_id);

	if (ret)
		return ret;
	if (in_atomic() || in_interrupt() || irqs_disabled())
		return -EWOULDBLOCK;
	pin = ipidev->table[ipi_id].pin_recv;
	if (!pin)
		return IPI_UNAVAILABLE;
	mutex_lock(&ipidev->mutex_ipi_reg);
	/* The pin lock drains any ISR currently using the consumer's buffer. */
	spin_lock_irqsave(&pin->pin_lock, flags);
	pin->mbox_pin_cb = NULL;
	pin->pin_buf = NULL;
	pin->prdata = NULL;
	spin_unlock_irqrestore(&pin->pin_lock, flags);
	complete(&pin->notify);
	spin_lock_irqsave(&ipidev->lock_monitor, flags);
	ipidev->table[ipi_id].ipi_stage = UNUSED;
	ipidev->table[ipi_id].ipi_seqno = 0;
	spin_unlock_irqrestore(&ipidev->lock_monitor, flags);
	mutex_unlock(&ipidev->mutex_ipi_reg);
	return IPI_ACTION_DONE;
}
EXPORT_SYMBOL(mtk_ipi_unregister);

int mtk_ipi_send(struct mtk_ipi_device *ipidev, int ipi_id,
		int opt, void *data, int len, int timeout)
{
	struct mtk_mbox_pin_send *pin;
	unsigned long flags = 0;
	u64 timeover;
	int ret = ipi_check_channel(ipidev, ipi_id);

	if (ret)
		return ret;
	if (!data || len < 0 || timeout < 0)
		return -EINVAL;
	pin = ipidev->table[ipi_id].pin_send;
	if (!pin)
		return IPI_UNAVAILABLE;
	if (len > pin->msg_size)
		return IPI_NO_MEMORY;
	if (!len)
		len = pin->msg_size;
	ret = ipi_lock_send(pin, opt, &flags);
	if (ret)
		return ret;
	if (ipidev->pre_cb && ipidev->pre_cb(ipidev->prdata)) {
		ipi_unlock_send(pin, opt, flags);
		return IPI_PRE_CB_FAIL;
	}
	timeover = cpu_clock(0) + MS_TO_NS(timeout);
	ret = rpmsg_trysend(ipidev->table[ipi_id].ept, data, len);
	ipidev->table[ipi_id].trysend_count = 1;
	while (ret == MBOX_PIN_BUSY && ts_before(timeover)) {
		ipi_delay();
		ret = rpmsg_trysend(ipidev->table[ipi_id].ept, data, len);
		ipidev->table[ipi_id].trysend_count++;
	}
	if (!ret) {
		ipi_monitor(ipidev, ipi_id, SEND_MSG);
		ipidev->ipi_last_done = ipi_id;
	}
	ipi_unlock_send(pin, opt, flags);
	if (ipidev->post_cb && ipidev->post_cb(ipidev->prdata))
		return IPI_POST_CB_FAIL;
	if (ret == MBOX_PIN_BUSY) {
		ipi_timeout_dump(ipidev, ipi_id);
		return IPI_PIN_BUSY;
	}
	if (ret) {
		pr_warn("%s IPI %d send fail (%d)\n", ipidev->name, ipi_id, ret);
		return IPI_RPMSG_ERR;
	}
	return IPI_ACTION_DONE;
}
EXPORT_SYMBOL(mtk_ipi_send);

int mtk_ipi_recv(struct mtk_ipi_device *ipidev, int ipi_id)
{
	struct mtk_mbox_pin_recv *pin;
	int ret = ipi_check_channel(ipidev, ipi_id);

	if (ret)
		return ret;

	pin = ipidev->table[ipi_id].pin_recv;
	if (!pin)
		return IPI_UNAVAILABLE;

	/* receive the ipi from ISR */
	if (in_atomic() || in_interrupt() || irqs_disabled())
		return -EWOULDBLOCK;
	ret = wait_for_completion_interruptible(&pin->notify);
	if (ret)
		return ret;
	if (!READ_ONCE(pin->pin_buf))
		return IPI_NO_MSGBUF;

	ipi_monitor(ipidev, ipi_id, RECV_MSG);
	ipidev->ipi_last_done = ipi_id;

	if (pin->mbox_pin_cb && pin->cb_ctx_opt == MBOX_CB_IN_PROCESS)
		pin->mbox_pin_cb(ipi_id, pin->prdata, pin->pin_buf,
			pin->msg_size * MBOX_SLOT_SIZE);

	return IPI_ACTION_DONE;
}
EXPORT_SYMBOL(mtk_ipi_recv);

int mtk_ipi_send_compl(struct mtk_ipi_device *ipidev, int ipi_id,
		int opt, void *data, int len, unsigned long timeout)
{
	struct mtk_mbox_pin_send *send;
	struct mtk_mbox_pin_recv *recv;
	struct mtk_ipi_chan_table *chan;
	unsigned long flags = 0;
	u64 timeover, remaining;
	int ret = ipi_check_channel(ipidev, ipi_id);

	if (ret)
		return ret;
	if (!data || len < 0 || timeout > U64_MAX / 1000000ULL)
		return -EINVAL;
	chan = &ipidev->table[ipi_id];
	send = chan->pin_send;
	recv = chan->pin_recv;
	if (!send || !recv)
		return IPI_UNAVAILABLE;
	if (!READ_ONCE(recv->pin_buf))
		return IPI_NO_MSGBUF;
	if (len > send->msg_size)
		return IPI_NO_MEMORY;
	if (!len)
		len = send->msg_size;
	ret = ipi_lock_send(send, opt, &flags);
	if (ret)
		return ret;
	if (ipidev->pre_cb && ipidev->pre_cb(ipidev->prdata)) {
		ipi_unlock_send(send, opt, flags);
		return IPI_PRE_CB_FAIL;
	}
	reinit_completion(&recv->notify);
	atomic_set(&chan->holder, 1);
	timeover = cpu_clock(0) + MS_TO_NS(timeout);
	ret = rpmsg_trysend(chan->ept, data, len);
	chan->trysend_count = 1;
	chan->polling_count = 0;
	while (ret == MBOX_PIN_BUSY && ts_before(timeover)) {
		ipi_delay();
		ret = rpmsg_trysend(chan->ept, data, len);
		chan->trysend_count++;
	}
	if (ret) {
		pr_warn("%s IPI %d send fail (%d)\n", ipidev->name, ipi_id, ret);
		ret = ret == MBOX_PIN_BUSY ? IPI_PIN_BUSY : IPI_RPMSG_ERR;
		goto unlock;
	}
	ipi_monitor(ipidev, ipi_id, SEND_MSG);
	ret = 0;
	if (opt == IPI_SEND_POLLING) {
		do {
			chan->polling_count++;
			if (mtk_mbox_polling(ipidev->mbdev, recv->mbox,
					     recv->pin_buf, recv) == MBOX_DONE ||
			    try_wait_for_completion(&recv->notify)) {
				ret = 1;
				break;
			}
			ipi_delay();
		} while (ts_before(timeover));
	} else {
		remaining = ts_before(timeover) ? timeover - cpu_clock(0) : 0;
		ret = wait_for_completion_timeout(&recv->notify, nsecs_to_jiffies(remaining));
	}
	if (ret > 0 && READ_ONCE(recv->pin_buf)) {
		ipi_monitor(ipidev, ipi_id, RECV_ACK);
		ipidev->ipi_last_done = ipi_id;
		ret = IPI_ACTION_DONE;
	} else {
		mtk_mbox_dump_recv_pin(ipidev->mbdev, recv);
		ipi_timeout_dump(ipidev, ipi_id);
		if (ipidev->timeout_handler)
			ipidev->timeout_handler(ipi_id);
		ret = IPI_COMPL_TIMEOUT;
	}
unlock:
	atomic_set(&chan->holder, 0);
	ipi_unlock_send(send, opt, flags);
	if (ipidev->post_cb && ipidev->post_cb(ipidev->prdata))
		return IPI_POST_CB_FAIL;
	return ret;
}
EXPORT_SYMBOL(mtk_ipi_send_compl);

int mtk_ipi_recv_reply(struct mtk_ipi_device *ipidev, int ipi_id,
		void *reply_data, int len)
{
	struct mtk_mbox_pin_send *pin_s;
	struct mtk_mbox_pin_recv *pin_r;
	unsigned long flags = 0;
	int ret = ipi_check_channel(ipidev, ipi_id);

	if (ret)
		return ret;

	pin_r = ipidev->table[ipi_id].pin_recv;
	pin_s = ipidev->table[ipi_id].pin_send;
	if (!pin_r || !pin_s)
		return IPI_UNAVAILABLE;

	if (!reply_data || len < 0)
		return -EINVAL;
	if (len > pin_s->msg_size)
		return IPI_NO_MEMORY;
	else if (len == 0)
		len = pin_s->msg_size;

	/* recvice the IPI message*/
	if (in_atomic() || in_interrupt() || irqs_disabled())
		return -EWOULDBLOCK;
	ret = wait_for_completion_interruptible(&pin_r->notify);
	if (ret)
		return ret;
	if (!READ_ONCE(pin_r->pin_buf))
		return IPI_NO_MSGBUF;

	ipi_monitor(ipidev, ipi_id, RECV_MSG);

	if (pin_r->mbox_pin_cb && pin_r->cb_ctx_opt == MBOX_CB_IN_PROCESS)
		pin_r->mbox_pin_cb(ipi_id, pin_r->prdata, pin_r->pin_buf,
			pin_r->msg_size * MBOX_SLOT_SIZE);

	/* send the response*/
	if (ipidev->pre_cb && ipidev->pre_cb(ipidev->prdata)) {
		pr_notice("Error: IPI [%s] pre_cb fail\n", ipidev->table[ipi_id].rpchan->info.name);
		return IPI_PRE_CB_FAIL;
	}

	/* lock this pin until send ack*/
	spin_lock_irqsave(&pin_s->pin_lock, flags);

	ret = rpmsg_trysend(ipidev->table[ipi_id].ept, reply_data, len);

	if (ret == IPI_ACTION_DONE) {
		ipi_monitor(ipidev, ipi_id, SEND_ACK);
		ipidev->ipi_last_done = ipi_id;
	}

	spin_unlock_irqrestore(&pin_s->pin_lock, flags);

	if (ipidev->post_cb && ipidev->post_cb(ipidev->prdata)) {
		pr_notice("Error: IPI [%s] post_cb fail\n",
			ipidev->table[ipi_id].rpchan->info.name);
		return IPI_POST_CB_FAIL;
	}

	if (ret == MBOX_PIN_BUSY)
		return IPI_PIN_BUSY;
	else if (ret != IPI_ACTION_DONE) {
		pr_warn("%s IPI %d reply fail (%d)\n",
			ipidev->name, ipi_id, ret);
		return IPI_RPMSG_ERR;
	}

	return ret;
}
EXPORT_SYMBOL(mtk_ipi_recv_reply);

void mtk_ipi_tracking(struct mtk_ipi_device *ipidev, bool en)
{
	ipidev->mbdev->log_enable = en;
	pr_info("%s IPI tracking %s\n", ipidev->name, en ? "on" : "off");
}
EXPORT_SYMBOL(mtk_ipi_tracking);

static void ipi_isr_cb(struct mtk_mbox_pin_recv *pin, void *priv)
{
	struct mtk_ipi_device *ipidev = priv;
	int ipi_id = pin->chan_id;

	if (ipi_check_channel(ipidev, ipi_id))
		return;

	if (pin->recv_opt == MBOX_RECV_MESSAGE) {
		ipi_monitor(ipidev, ipi_id, ISR_RECV_MSGV);
		complete(&pin->notify);
	} else if (atomic_read(&ipidev->table[ipi_id].holder)) {
		ipi_monitor(ipidev, ipi_id, ISR_RECV_ACK);
		complete(&pin->notify);
	}

}

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MediaTek Tinysys IPI driver");
