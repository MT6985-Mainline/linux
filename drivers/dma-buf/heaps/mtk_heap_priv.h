/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2021 MediaTek Inc. */
#ifndef _MTK_DMABUFHEAP_DEBUG_H
#define _MTK_DMABUFHEAP_DEBUG_H
#include <linux/dma-heap.h>
#include <linux/dma-buf.h>
#include <linux/dma-resv.h>
#include <linux/seq_file.h>
#include <linux/sched/clock.h>
#include <dt-bindings/memory/mtk-memory-port.h>

/* The corot IOMMU ABI has three tables and sixteen domains. */
#define MTK_M4U_TAB_NR_MAX 3
#ifndef MTK_M4U_DOM_NR_MAX
#define MTK_M4U_DOM_NR_MAX 16
#endif
#define dmabuf_dump(file, fmt, args...) do { \
	if (file) \
		seq_printf(file, fmt, ##args); \
	else \
		pr_info(fmt, ##args); \
} while (0)
struct mtk_heap_priv_info {
	void (*show)(struct dma_heap *heap, void *seq_file, int flag);
	int (*buf_priv_dump)(const struct dma_buf *dmabuf,
			     struct dma_heap *heap, void *seq_file);
};
struct dma_heap_attachment {
	struct device *dev;
	struct sg_table *table;
	struct list_head list;
	bool mapped;
	bool uncached;
};
struct mtk_heap_dev_info {
	struct device *dev;
	enum dma_data_direction direction;
	unsigned long map_attrs;
};
static inline void dmabuf_release_check(const struct dma_buf *dmabuf)
{
	if (!dma_resv_trylock(dmabuf->resv))
		return;
	WARN_ON(!list_empty(&dmabuf->attachments));
	dma_resv_unlock(dmabuf->resv);
}
#endif
