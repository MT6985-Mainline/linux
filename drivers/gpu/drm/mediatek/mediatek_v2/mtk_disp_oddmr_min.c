// SPDX-License-Identifier: GPL-2.0
/*
 * corot r235: minimal MT6985 ODDMR0 bring-up driver.
 *
 * The vendor driver is 4547 lines of demura tuning; with no demura
 * tables the block only needs the vendor first-boot sequence to pass
 * pixels (mtk_disp_oddmr.c: mtk_oddmr_od_prepare / mtk_oddmr_dmr_prepare):
 *   OD_UDMA_CTR_0 (0x100) = 0x880   (od_clk_en)
 *   TOP_CRP_BYPASS (0x1D8) = 1      ("crop off, add for first boot")
 *   DMR_EN (0x404) = 1
 * Everything else stays at reset/LK state.
 */
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "mtk_drm_crtc.h"
#include "mtk_drm_ddp_comp.h"
#include "mtk_dump.h"

#define DISP_ODDMR_OD_UDMA_CTR_0	0x0100
#define DISP_ODDMR_TOP_CRP_BYPSS	0x01D8
#define DISP_ODDMR_REG_DMR_EN		0x0404

struct mtk_disp_oddmr {
	struct mtk_ddp_comp ddp_comp;
};

static inline struct mtk_disp_oddmr *comp_to_oddmr(struct mtk_ddp_comp *comp)
{
	return container_of(comp, struct mtk_disp_oddmr, ddp_comp);
}

static void mtk_oddmr_start(struct mtk_ddp_comp *comp, struct cmdq_pkt *handle)
{
	DDPINFO("%s start\n", mtk_dump_comp_str(comp));
}

static void mtk_oddmr_stop(struct mtk_ddp_comp *comp, struct cmdq_pkt *handle)
{
	DDPINFO("%s stop\n", mtk_dump_comp_str(comp));
}

static void mtk_oddmr_prepare(struct mtk_ddp_comp *comp)
{
	struct mtk_disp_oddmr *priv = comp_to_oddmr(comp);
	void __iomem *b = comp->regs;

	DDPINFO("%s prepare\n", mtk_dump_comp_str(comp));
	mtk_ddp_comp_clk_prepare(comp);

	if (!b) {
		DDPINFO("%s no regs\n", mtk_dump_comp_str(comp));
		return;
	}
	/* vendor first-boot sequence */
	writel(0x0880, b + DISP_ODDMR_OD_UDMA_CTR_0);
	writel(1, b + DISP_ODDMR_TOP_CRP_BYPSS);
	writel(1, b + DISP_ODDMR_REG_DMR_EN);
	pr_err("COROT r235: oddmr prepared: udma=0x%08x crp=0x%08x dmr=0x%08x\n",
	       readl(b + DISP_ODDMR_OD_UDMA_CTR_0),
	       readl(b + DISP_ODDMR_TOP_CRP_BYPSS),
	       readl(b + DISP_ODDMR_REG_DMR_EN));
}

static void mtk_oddmr_unprepare(struct mtk_ddp_comp *comp)
{
	DDPINFO("%s unprepare\n", mtk_dump_comp_str(comp));
	mtk_ddp_comp_clk_unprepare(comp);
}

static const struct mtk_ddp_comp_funcs mtk_disp_oddmr_min_funcs = {
	.start = mtk_oddmr_start,
	.stop = mtk_oddmr_stop,
	.prepare = mtk_oddmr_prepare,
	.unprepare = mtk_oddmr_unprepare,
};

static int mtk_disp_oddmr_bind(struct device *dev, struct device *master,
			       void *data)
{
	struct mtk_disp_oddmr *priv = dev_get_drvdata(dev);
	struct drm_device *drm_dev = data;
	int ret;

	ret = mtk_ddp_comp_register(drm_dev, &priv->ddp_comp);
	if (ret < 0) {
		dev_err(dev, "Failed to register component %s: %d\n",
			dev->of_node->full_name, ret);
		return ret;
	}
	return 0;
}

static void mtk_disp_oddmr_unbind(struct device *dev, struct device *master,
				  void *data)
{
	struct mtk_disp_oddmr *priv = dev_get_drvdata(dev);
	struct drm_device *drm_dev = data;

	mtk_ddp_comp_unregister(drm_dev, &priv->ddp_comp);
}

static const struct component_ops mtk_disp_oddmr_component_ops = {
	.bind = mtk_disp_oddmr_bind,
	.unbind = mtk_disp_oddmr_unbind,
};

static int mtk_disp_oddmr_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_disp_oddmr *priv;
	enum mtk_ddp_comp_id comp_id;
	int ret;

	pr_err("COROT r235: oddmr probe+\n");

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	comp_id = mtk_ddp_comp_get_id(dev->of_node, MTK_DISP_ODDMR);
	if ((int)comp_id < 0) {
		dev_err(dev, "Failed to identify by alias: %d\n", comp_id);
		return comp_id;
	}

	ret = mtk_ddp_comp_init(dev, dev->of_node, &priv->ddp_comp, comp_id,
				&mtk_disp_oddmr_min_funcs);
	if (ret) {
		dev_err(dev, "Failed to initialize component: %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, priv);
	mtk_ddp_comp_pm_enable(&priv->ddp_comp);

	ret = component_add(dev, &mtk_disp_oddmr_component_ops);
	if (ret != 0) {
		dev_err(dev, "Failed to add component: %d\n", ret);
		mtk_ddp_comp_pm_disable(&priv->ddp_comp);
		return ret;
	}

	pr_err("COROT r235: oddmr probe- ok\n");
	return 0;
}

static void mtk_disp_oddmr_remove(struct platform_device *pdev)
{
	struct mtk_disp_oddmr *priv = dev_get_drvdata(&pdev->dev);

	component_del(&pdev->dev, &mtk_disp_oddmr_component_ops);
	mtk_ddp_comp_pm_disable(&priv->ddp_comp);
}

static const struct of_device_id mtk_disp_oddmr_min_dt_match[] = {
	{ .compatible = "mediatek,mt6985-disp-oddmr" },
	{},
};

MODULE_DEVICE_TABLE(of, mtk_disp_oddmr_min_dt_match);

struct platform_driver mtk_disp_oddmr_min_driver = {
	.probe = mtk_disp_oddmr_probe,
	.remove = mtk_disp_oddmr_remove,
	.driver = {
		.name = "mediatek-disp-oddmr-min",
		.owner = THIS_MODULE,
		.of_match_table = mtk_disp_oddmr_min_dt_match,
	},
};
