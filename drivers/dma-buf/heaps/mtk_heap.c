// SPDX-License-Identifier: GPL-2.0
#include <linux/dma-buf.h>
#include <linux/slab.h>
#include <uapi/linux/dma-buf.h>
#include "mtk_heap.h"

long mtk_dma_buf_set_name(struct dma_buf *dmabuf, const char *buf)
{
	char *name = NULL;
	const char *old;

	if (IS_ERR_OR_NULL(dmabuf))
		return -EINVAL;
	if (buf) {
		name = kstrndup(buf, DMA_BUF_NAME_LEN - 1, GFP_KERNEL);
		if (!name)
			return -ENOMEM;
	}
	spin_lock(&dmabuf->name_lock);
	old = dmabuf->name;
	dmabuf->name = name;
	spin_unlock(&dmabuf->name_lock);
	kfree(old);
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_dma_buf_set_name);
