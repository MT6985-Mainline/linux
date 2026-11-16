# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_DMABUF_HEAPS_SYSTEM)	+= system_heap.o
obj-$(CONFIG_DMABUF_HEAPS_CMA)		+= cma_heap.o

ccflags-y += -I$(srctree)/drivers/iommu
ccflags-y += -I$(srctree)/drivers/misc/mediatek/trusted_mem
obj-$(CONFIG_DMABUF_HEAPS_MTK_SECURE) += mtk_sec_heap.o

obj-$(CONFIG_DMABUF_HEAPS) += mtk_heap.o
