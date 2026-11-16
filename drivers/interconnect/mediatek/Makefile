# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_INTERCONNECT_MTK_DVFSRC_EMI) += icc-emi.o
obj-$(CONFIG_INTERCONNECT_MTK_EMI) += mtk-emi.o
obj-$(CONFIG_INTERCONNECT_MTK_MMQOS_COMMON) += mmqos-common.o
mmqos-common-y := mmqos-mtk.o mmqos-hrt.o
obj-$(CONFIG_INTERCONNECT_MTK_MMQOS_MT6985) += mmqos-mt6985.o
ccflags-y += -I$(srctree)/drivers/misc/mediatek/mtk-interconnect
ccflags-y += -I$(src)
obj-$(CONFIG_INTERCONNECT_MTK_MT8183) += mt8183.o
obj-$(CONFIG_INTERCONNECT_MTK_MT8195) += mt8195.o
obj-$(CONFIG_INTERCONNECT_MTK_MT8195) += mt8196.o
