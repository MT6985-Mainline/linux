// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015 Pengutronix, Sascha Hauer <kernel@pengutronix.de>
 */
#include <linux/clk.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_opp.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

#include "scpsys.h"
#include "mtk-scpsys.h"

#include <linux/notifier.h>

#define MTK_POLL_DELAY_US		10
#define MTK_POLL_TIMEOUT		USEC_PER_SEC
#define MTK_POLL_IRQ_DELAY_US		3
#define MTK_POLL_IRQ_TIMEOUT		USEC_PER_SEC
#define MTK_POLL_HWV_PREPARE_CNT	100
#define MTK_POLL_HWV_PREPARE_US		2

#define MTK_SCPD_CAPS(_scpd, _x)	((_scpd)->data->caps & (_x))

#define SPM_VDE_PWR_CON			0x0210
#define SPM_MFG_PWR_CON			0x0214
#define SPM_VEN_PWR_CON			0x0230
#define SPM_ISP_PWR_CON			0x0238
#define SPM_DIS_PWR_CON			0x023c
#define SPM_CONN_PWR_CON		0x0280
#define SPM_VEN2_PWR_CON		0x0298
#define SPM_AUDIO_PWR_CON		0x029c	/* MT8173, MT2712 */
#define SPM_BDP_PWR_CON			0x029c	/* MT2701 */
#define SPM_ETH_PWR_CON			0x02a0
#define SPM_HIF_PWR_CON			0x02a4
#define SPM_IFR_MSC_PWR_CON		0x02a8
#define SPM_MFG_2D_PWR_CON		0x02c0
#define SPM_MFG_ASYNC_PWR_CON		0x02c4
#define SPM_USB_PWR_CON			0x02cc
#define SPM_USB2_PWR_CON		0x02d4	/* MT2712 */
#define SPM_ETHSYS_PWR_CON		0x02e0	/* MT7622 */
#define SPM_HIF0_PWR_CON		0x02e4	/* MT7622 */
#define SPM_HIF1_PWR_CON		0x02e8	/* MT7622 */
#define SPM_WB_PWR_CON			0x02ec	/* MT7622 */

#define SPM_PWR_STATUS			0x060c
#define SPM_PWR_STATUS_2ND		0x0610

#define PWR_RST_B_BIT			BIT(0)
#define PWR_ISO_BIT			BIT(1)
#define PWR_ON_BIT			BIT(2)
#define PWR_ON_2ND_BIT			BIT(3)
#define PWR_CLK_DIS_BIT			BIT(4)
#define PWR_SRAM_CLKISO_BIT		BIT(5)
#define PWR_SRAM_ISOINT_B_BIT		BIT(6)
#define PWR_RTFF_SAVE			BIT(24)
#define PWR_RTFF_NRESTORE		BIT(25)
#define PWR_RTFF_CLK_DIS		BIT(28)
#define PWR_ACK				BIT(30)
#define PWR_ACK_2ND			BIT(31)

#define PWR_STATUS_CONN			BIT(1)
#define PWR_STATUS_DISP			BIT(3)
#define PWR_STATUS_MFG			BIT(4)
#define PWR_STATUS_ISP			BIT(5)
#define PWR_STATUS_VDEC			BIT(7)
#define PWR_STATUS_BDP			BIT(14)
#define PWR_STATUS_ETH			BIT(15)
#define PWR_STATUS_HIF			BIT(16)
#define PWR_STATUS_IFR_MSC		BIT(17)
#define PWR_STATUS_USB2			BIT(19)	/* MT2712 */
#define PWR_STATUS_VENC_LT		BIT(20)
#define PWR_STATUS_VENC			BIT(21)
#define PWR_STATUS_MFG_2D		BIT(22)	/* MT8173 */
#define PWR_STATUS_MFG_ASYNC		BIT(23)	/* MT8173 */
#define PWR_STATUS_AUDIO		BIT(24)	/* MT8173, MT2712 */
#define PWR_STATUS_USB			BIT(25)	/* MT8173, MT2712 */
#define PWR_STATUS_ETHSYS		BIT(24)	/* MT7622 */
#define PWR_STATUS_HIF0			BIT(25)	/* MT7622 */
#define PWR_STATUS_HIF1			BIT(26)	/* MT7622 */
#define PWR_STATUS_WB			BIT(27)	/* MT7622 */

/* Preserve the shared MT6895 bus type numbering and bindings. */
static const char * const scpsys_bus_list[] = {
	[IFR_TYPE] = "infracfg",
	[SMI_TYPE] = "smi_comm",
	[VLP_TYPE] = "vlpcfg",
	[MFGRPC_TYPE] = "mfgrpc",
	[NEMI_TYPE] = "nemi_bus",
	[SEMI_TYPE] = "semi_bus",
};

static bool scpsys_init_flag;
static BLOCKING_NOTIFIER_HEAD(scpsys_notifier_list);

int register_scpsys_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&scpsys_notifier_list, nb);
}
EXPORT_SYMBOL_GPL(register_scpsys_notifier);

int unregister_scpsys_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&scpsys_notifier_list, nb);
}
EXPORT_SYMBOL_GPL(unregister_scpsys_notifier);

static struct apu_callbacks *g_apucb;

void register_apu_callback(struct apu_callbacks *apucb)
{
	g_apucb = apucb;
}
EXPORT_SYMBOL_GPL(register_apu_callback);

static int __scpsys_domain_is_on(void __iomem *addr, u32 sta_mask)
{
	u32 status = readl(addr) & sta_mask;

	/*
	 * A domain is on when both status bits are set. If only one is set
	 * return an error. This happens while powering up a domain
	 */

	if (status == sta_mask)
		return true;
	if (!status)
		return false;

	return -EINVAL;
}

static int scpsys_sta_is_on(struct scp_domain *scpd)
{
	struct scp *scp = scpd->scp;

	return __scpsys_domain_is_on(scp->base + scp->ctrl_reg.pwr_sta_offs,
			scpd->data->sta_mask);
}

static int scpsys_sta_2nd_is_on(struct scp_domain *scpd)
{
	struct scp *scp = scpd->scp;

	return __scpsys_domain_is_on(scp->base + scp->ctrl_reg.pwr_sta2nd_offs,
			scpd->data->sta_mask);
}

static int scpsys_pwr_ack_is_on(struct scp_domain *scpd)
{
	struct scp *scp = scpd->scp;

	return __scpsys_domain_is_on(scp->base + scpd->data->ctl_offs,
			PWR_ACK);
}

static int scpsys_pwr_ack_2nd_is_on(struct scp_domain *scpd)
{
	struct scp *scp = scpd->scp;

	return __scpsys_domain_is_on(scp->base + scpd->data->ctl_offs,
			PWR_ACK_2ND);
}

static int scpsys_regulator_is_enabled(struct scp_domain *scpd)
{
	if (!scpd->supply)
		return 0;

	return regulator_is_enabled(scpd->supply);
}

static int scpsys_regulator_enable(struct scp_domain *scpd)
{
	if (!scpd->supply)
		return 0;

	return regulator_enable(scpd->supply);
}

static int scpsys_regulator_disable(struct scp_domain *scpd)
{
	if (!scpd->supply)
		return 0;

	return regulator_disable(scpd->supply);
}

static void scpsys_clk_disable(struct clk *clk[], int max_num)
{
	int i;

	for (i = max_num - 1; i >= 0; i--)
		clk_disable_unprepare(clk[i]);
}

static int scpsys_clk_enable(struct clk *clk[], int max_num)
{
	int i, ret = 0;

	for (i = 0; i < max_num && clk[i]; i++) {
		ret = clk_prepare_enable(clk[i]);
		if (ret) {
			scpsys_clk_disable(clk, i);
			break;
		}
	}

	return ret;
}

static int scpsys_sram_on(struct scp_domain *scpd, void __iomem *ctl_addr,
			u32 set_bits, u32 ack_bits, bool wait_ack)
{
	u32 ack_mask, ack_sta;
	u32 val;
	int tmp;
	int ret = 0;

	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_SRAM_SLP)) {
		ack_mask = ack_bits;
		ack_sta = ack_mask;
		val = readl(ctl_addr) | set_bits;
	} else {
		ack_mask = ack_bits;
		ack_sta = 0;
		val = readl(ctl_addr) & ~set_bits;
	}

	writel(val, ctl_addr);

	/* Either wait until SRAM_PDN_ACK all 0 or have a force wait */
	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_FWAIT_SRAM)) {
		/*
		 * Currently, MTK_SCPD_FWAIT_SRAM is necessary only for
		 * MT7622_POWER_DOMAIN_WB and thus just a trivial setup
		 * is applied here.
		 */
		usleep_range(12000, 12100);
	} else {
		/* Either wait until SRAM_PDN_ACK all 1 or 0 */
		if (wait_ack) {
			ret = readl_poll_timeout_atomic(ctl_addr, tmp,
				(tmp & ack_mask) == ack_sta,
				MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
			if (ret < 0)
				return ret;
		}
	}

	return ret;
}

static int scpsys_sram_off(struct scp_domain *scpd, void __iomem *ctl_addr,
			u32 set_bits, u32 ack_bits, bool wait_ack)
{
	u32 val;
	u32 ack_mask, ack_sta;
	int tmp;
	int ret = 0;

	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_SRAM_SLP)) {
		ack_mask = ack_bits;
		ack_sta = 0;
		val = readl(ctl_addr) & ~set_bits;
	} else {
		ack_mask = ack_bits;
		ack_sta = ack_mask;
		val = readl(ctl_addr) | set_bits;
	}
	writel(val, ctl_addr);

	/* Either wait until SRAM_PDN_ACK all 1 or 0 */
	if (wait_ack)
		ret = readl_poll_timeout_atomic(ctl_addr, tmp,
				(tmp & ack_mask) == ack_sta,
				MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);

	return ret;
}

static int scpsys_sram_enable(struct scp_domain *scpd, void __iomem *ctl_addr)
{
	u32 val;
	int ret = 0;

	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_SRAM_SLP))
		ret = scpsys_sram_on(scpd, ctl_addr, scpd->data->sram_slp_bits,
				scpd->data->sram_slp_ack_bits, true);
	else
		ret = scpsys_sram_on(scpd, ctl_addr, scpd->data->sram_pdn_bits,
				scpd->data->sram_pdn_ack_bits, true);

	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_SRAM_ISO)) {
		val = readl(ctl_addr) | PWR_SRAM_ISOINT_B_BIT;
		writel(val, ctl_addr);
		udelay(1);
		val &= ~PWR_SRAM_CLKISO_BIT;
		writel(val, ctl_addr);
	}

	return ret;
}

static int scpsys_sram_disable(struct scp_domain *scpd, void __iomem *ctl_addr)
{
	u32 val;
	int ret = 0;

	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_SRAM_ISO)) {
		val = readl(ctl_addr) | PWR_SRAM_CLKISO_BIT;
		writel(val, ctl_addr);
		val &= ~PWR_SRAM_ISOINT_B_BIT;
		writel(val, ctl_addr);
		udelay(1);
	}

	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_SRAM_SLP))
		ret = scpsys_sram_off(scpd, ctl_addr, scpd->data->sram_slp_bits,
				scpd->data->sram_slp_ack_bits, true);
	else
		ret = scpsys_sram_off(scpd, ctl_addr, scpd->data->sram_pdn_bits,
				scpd->data->sram_pdn_ack_bits, true);

	return ret;
}

static int scpsys_sram_table_enable(struct scp_domain *scpd)
{
	const struct sram_ctl *sram_table = scpd->data->sram_table;
	struct scp *scp = scpd->scp;
	int ret = 0;
	int i;

	for (i = 0; i < MAX_SRAM_STEPS; i++) {
		if (sram_table[i].offs) {
			void __iomem *ctl_addr = scp->base + sram_table[i].offs;

			ret = scpsys_sram_on(scpd, ctl_addr, sram_table[i].msk,
					sram_table[i].ack_msk, sram_table[i].wait_ack);
			if (ret)
				return ret;
		}
	}

	return ret;
}

static int scpsys_sram_table_disable(struct scp_domain *scpd)
{
	const struct sram_ctl *sram_table = scpd->data->sram_table;
	struct scp *scp = scpd->scp;
	int ret = 0;
	int i;

	for (i = 0; i < MAX_SRAM_STEPS; i++) {
		if (sram_table[i].offs) {
			void __iomem *ctl_addr = scp->base + sram_table[i].offs;

			ret = scpsys_sram_off(scpd, ctl_addr, sram_table[i].msk,
					sram_table[i].ack_msk, sram_table[i].wait_ack);
			if (ret)
				return ret;
		}
	}

	return ret;
}

static int set_bus_protection(struct regmap *map, struct bus_prot *bp)
{
	u32 val = 0;
	int ret, retry;

	if (!map)
		return -ENODEV;

	/* Retry the write until the protection enable bits are latched. */
	for (retry = 0; retry <= 10; retry++) {
		if (bp->set_ofs)
			ret = regmap_write(map, bp->set_ofs, bp->mask);
		else
			ret = regmap_update_bits(map, bp->en_ofs,
						bp->mask, bp->mask);
		if (ret)
			return ret;

		ret = regmap_read(map, bp->en_ofs, &val);
		if (ret)
			return ret;
		if ((val & bp->mask) == bp->mask)
			break;
	}
	if (retry > 10)
		return -ETIMEDOUT;

	ret = regmap_read_poll_timeout_atomic(map, bp->sta_ofs,
			val, (val & bp->ack_mask) == bp->ack_mask,
			MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
	if (ret)
		pr_err("%s: status %#x, ack mask %#x\n",
		       __func__, val, bp->ack_mask);

	return ret;
}

static int clear_bus_protection(struct regmap *map, struct bus_prot *bp)
{
	u32 val = 0;
	int ret;

	if (!map)
		return -ENODEV;

	if (bp->clr_ofs)
		ret = regmap_write(map, bp->clr_ofs, bp->mask);
	else
		ret = regmap_update_bits(map, bp->en_ofs, bp->mask, 0);
	if (ret || bp->ignore_clr_ack)
		return ret;

	ret = regmap_read_poll_timeout_atomic(map, bp->sta_ofs,
			val, !(val & bp->ack_mask),
			MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
	if (ret)
		pr_err("%s: status %#x, ack mask %#x\n",
		       __func__, val, bp->ack_mask);

	return ret;
}

static int scpsys_bus_protect_disable(struct scp_domain *scpd, unsigned int index)
{
	struct scp *scp = scpd->scp;
	const struct bus_prot *bp_table = scpd->data->bp_table;
	int i;

	for (i = index; i >= 0; i--) {
		struct bus_prot bp = bp_table[i];
		struct regmap *map;
		int ret;

		if (!bp.type)
			continue;
		if (bp.type >= scp->num_bus_type)
			return -EINVAL;
		map = scp->bus_regmap[bp.type];
		if (!map)
			return -ENODEV;

		ret = clear_bus_protection(map, &bp);
		if (ret) {
			dev_err(scp->dev, "%s: cannot release bus protection step %d: %d\n",
				scpd->data->name, i, ret);
			return ret;
		}
	}

	return 0;
}

static int scpsys_bus_protect_enable(struct scp_domain *scpd)
{
	struct scp *scp = scpd->scp;
	const struct bus_prot *bp_table = scpd->data->bp_table;
	int i;

	for (i = 0; i < MAX_STEPS; i++) {
		struct bus_prot bp = bp_table[i];
		struct regmap *map;
		int ret;

		if (!bp.type)
			continue;
		if (bp.type >= scp->num_bus_type)
			return -EINVAL;
		map = scp->bus_regmap[bp.type];
		if (!map)
			return -ENODEV;

		ret = set_bus_protection(map, &bp);
		if (ret) {
			scpsys_bus_protect_disable(scpd, i);
			return ret;
		}
	}

	return 0;
}

static void scpsys_extb_iso_down(struct scp_domain *scpd)
{
	u32 val;
	struct scp *scp;
	void __iomem *ctl_addr;

	if (!scpd->data->extb_iso_offs)
		return;

	scp = scpd->scp;
	ctl_addr = scp->base + scpd->data->extb_iso_offs;
	val = readl(ctl_addr) & ~scpd->data->extb_iso_bits;
	writel(val, ctl_addr);
}

static void scpsys_extb_iso_up(struct scp_domain *scpd)
{
	u32 val;
	struct scp *scp;
	void __iomem *ctl_addr;

	if (!scpd->data->extb_iso_offs)
		return;

	scp = scpd->scp;
	ctl_addr = scp->base + scpd->data->extb_iso_offs;
	val = readl(ctl_addr) | scpd->data->extb_iso_bits;
	writel(val, ctl_addr);
}

static int scpsys_power_on(struct generic_pm_domain *genpd)
{
	struct scp_domain *scpd = container_of(genpd, struct scp_domain, genpd);
	struct scp *scp = scpd->scp;
	void __iomem *ctl_addr = scp->base + scpd->data->ctl_offs;
	u32 val;
	int ret, tmp;

	ret = scpsys_regulator_enable(scpd);
	if (ret < 0)
		goto err_regulator;

	ret = scpsys_clk_enable(scpd->clk, MAX_CLKS);
	if (ret)
		goto err_clk;

	ret = scpsys_clk_enable(scpd->lp_clk, MAX_CLKS);
	if (ret)
		goto err_lp_clk;

	/* subsys power on */
	val = readl(ctl_addr);
	val |= PWR_ON_BIT;
	writel(val, ctl_addr);
	/* wait until PWR_ACK = 1 */
	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_IS_PWR_CON_ON))
		ret = readx_poll_timeout_atomic(scpsys_pwr_ack_is_on, scpd, tmp, tmp > 0,
				MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
	else
		ret = readx_poll_timeout_atomic(scpsys_sta_is_on, scpd, tmp, tmp > 0,
				MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
	if (ret < 0)
		goto err_pwr_ack;

	udelay(50);

	/* subsys 2nd power on */
	val |= PWR_ON_2ND_BIT;
	writel(val, ctl_addr);
	/* wait until PWR_ACK = 1 */
	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_IS_PWR_CON_ON))
		ret = readx_poll_timeout_atomic(scpsys_pwr_ack_2nd_is_on, scpd, tmp, tmp > 0,
				MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
	else
		ret = readx_poll_timeout_atomic(scpsys_sta_2nd_is_on, scpd, tmp, tmp > 0,
				MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
	if (ret < 0)
		goto err_pwr_ack;

	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_PEXTP_PHY_RTFF) && scpd->rtff_flag) {
		val |= PWR_RTFF_CLK_DIS;
		writel(val, ctl_addr);
	}

	val &= ~PWR_CLK_DIS_BIT;
	writel(val, ctl_addr);

	val &= ~PWR_ISO_BIT;
	writel(val, ctl_addr);

	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_PEXTP_PHY_RTFF) && scpd->rtff_flag)
		udelay(10);

	val |= PWR_RST_B_BIT;
	writel(val, ctl_addr);

	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_NON_CPU_RTFF) && scpd->rtff_flag) {
		val |= PWR_RTFF_CLK_DIS;
		writel(val, ctl_addr);

		val &= ~PWR_RTFF_NRESTORE;
		writel(val, ctl_addr);

		val |= PWR_RTFF_NRESTORE;
		writel(val, ctl_addr);

		val &= ~PWR_RTFF_CLK_DIS;
		writel(val, ctl_addr);
	} else if (MTK_SCPD_CAPS(scpd, MTK_SCPD_PEXTP_PHY_RTFF) && scpd->rtff_flag) {
		val |= PWR_RTFF_SAVE;
		writel(val, ctl_addr);

		val &= ~PWR_RTFF_SAVE;
		writel(val, ctl_addr);

		val &= ~PWR_RTFF_CLK_DIS;
		writel(val, ctl_addr);
	}

	if (!MTK_SCPD_CAPS(scpd, MTK_SCPD_BYPASS_CLK) || scpsys_init_flag) {
		ret = scpsys_clk_enable(scpd->subsys_clk, MAX_SUBSYS_CLKS);
		if (ret < 0)
			goto err_pwr_ack;

		ret = scpsys_clk_enable(scpd->subsys_lp_clk, MAX_SUBSYS_CLKS);
		if (ret < 0)
			goto err_pwr_ack;
	}

	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_L2TCM_SRAM)) {
		ret = scpsys_sram_table_enable(scpd);
		if (ret < 0)
			goto err_sram;
	}

	ret = scpsys_sram_enable(scpd, ctl_addr);
	if (ret < 0)
		goto err_sram;

	ret = scpsys_bus_protect_disable(scpd, MAX_STEPS - 1);
	if (ret < 0)
		goto err_sram;

	scpsys_clk_disable(scpd->subsys_lp_clk, MAX_SUBSYS_CLKS);

	scpsys_clk_disable(scpd->lp_clk, MAX_CLKS);

	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_NON_CPU_RTFF) ||
			MTK_SCPD_CAPS(scpd, MTK_SCPD_PEXTP_PHY_RTFF)) {
		scpd->rtff_flag = false;
	}

	return 0;

err_sram:
	dev_err(scp->dev, "Failed to power on sram/bus %s(%d)\n", genpd->name, ret);
err_pwr_ack:
	dev_err(scp->dev, "Failed to power on mtcmos %s(%d)\n", genpd->name, ret);
err_lp_clk:
	dev_err(scp->dev, "Failed to enable lp_clk %s(%d)\n", genpd->name, ret);
err_clk:
	val = scpsys_regulator_is_enabled(scpd);
	dev_err(scp->dev, "Failed to enable clk %s(%d %d)\n", genpd->name, ret, val);
err_regulator:
	dev_err(scp->dev, "Failed to power on regulator %s(%d)\n", genpd->name, ret);

	return ret;
}

static int scpsys_power_off(struct generic_pm_domain *genpd)
{
	struct scp_domain *scpd = container_of(genpd, struct scp_domain, genpd);
	struct scp *scp = scpd->scp;
	void __iomem *ctl_addr = scp->base + scpd->data->ctl_offs;
	u32 val;
	int ret, tmp;

	ret = scpsys_clk_enable(scpd->lp_clk, MAX_CLKS);
	if (ret)
		goto err_lp_clk;

	ret = scpsys_clk_enable(scpd->subsys_lp_clk, MAX_SUBSYS_CLKS);
	if (ret < 0)
		goto err_subsys_lp_clk;

	ret = scpsys_bus_protect_enable(scpd);
	if (ret < 0)
		goto out;

	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_L2TCM_SRAM)) {
		ret = scpsys_sram_table_disable(scpd);
		if (ret < 0)
			goto out;
	}

	ret = scpsys_sram_disable(scpd, ctl_addr);
	if (ret < 0)
		goto out;

	if (!MTK_SCPD_CAPS(scpd, MTK_SCPD_BYPASS_CLK)) {
		scpsys_clk_disable(scpd->subsys_clk, MAX_SUBSYS_CLKS);
		scpsys_clk_disable(scpd->subsys_lp_clk, MAX_SUBSYS_CLKS);
	}

	val = readl(ctl_addr);

	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_NON_CPU_RTFF)) {
		val |= PWR_RTFF_CLK_DIS;
		writel(val, ctl_addr);

		val |= PWR_RTFF_SAVE;
		writel(val, ctl_addr);

		val &= ~PWR_RTFF_SAVE;
		writel(val, ctl_addr);

		val &= ~PWR_RTFF_CLK_DIS;
		writel(val, ctl_addr);
	}

	/* subsys power off */
	val |= PWR_ISO_BIT;
	writel(val, ctl_addr);

	val &= ~PWR_RST_B_BIT;
	writel(val, ctl_addr);

	val |= PWR_CLK_DIS_BIT;
	writel(val, ctl_addr);

	/* subsys power off */
	val &= ~PWR_ON_BIT;
	writel(val, ctl_addr);
	/* wait until PWR_ACK = 0 */
	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_IS_PWR_CON_ON))
		ret = readx_poll_timeout_atomic(scpsys_pwr_ack_is_on, scpd, tmp, tmp == 0,
				MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
	else
		ret = readx_poll_timeout_atomic(scpsys_sta_is_on, scpd, tmp, tmp == 0,
				MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
	if (ret < 0)
		goto out;

	/* subsys 2nd power off */
	val &= ~PWR_ON_2ND_BIT;
	writel(val, ctl_addr);

	/* wait until PWR_ACK = 0 */
	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_IS_PWR_CON_ON))
		ret = readx_poll_timeout_atomic(scpsys_pwr_ack_2nd_is_on, scpd, tmp, tmp == 0,
				MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
	else
		ret = readx_poll_timeout_atomic(scpsys_sta_2nd_is_on, scpd, tmp, tmp == 0,
				MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
	if (ret < 0)
		goto out;

	scpsys_clk_disable(scpd->clk, MAX_CLKS);

	scpsys_clk_disable(scpd->lp_clk, MAX_CLKS);

	ret = scpsys_regulator_disable(scpd);
	if (ret < 0)
		goto out;

	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_NON_CPU_RTFF) ||
			MTK_SCPD_CAPS(scpd, MTK_SCPD_PEXTP_PHY_RTFF)) {
		scpd->rtff_flag = true;
	}

	return 0;

err_subsys_lp_clk:
	scpsys_clk_disable(scpd->subsys_lp_clk, MAX_SUBSYS_CLKS);
err_lp_clk:
	scpsys_clk_disable(scpd->lp_clk, MAX_CLKS);
out:
	val = scpsys_regulator_is_enabled(scpd);
	dev_err(scp->dev, "Failed to power off domain %s(%d %d)\n", genpd->name, ret, val);

	return ret;
}

static int scpsys_md_power_on(struct generic_pm_domain *genpd)
{
	struct scp_domain *scpd = container_of(genpd, struct scp_domain, genpd);
	struct scp *scp = scpd->scp;
	void __iomem *ctl_addr = scp->base + scpd->data->ctl_offs;
	u32 val;
	int ret, tmp;

	ret = scpsys_regulator_enable(scpd);
	if (ret < 0)
		return ret;

	scpsys_extb_iso_down(scpd);

	ret = scpsys_clk_enable(scpd->clk, MAX_CLKS);
	if (ret)
		goto err_clk;

	/* for md subsys, reset_b is prior to power_on bit */
	val = readl(ctl_addr);
	val |= PWR_RST_B_BIT;
	writel(val, ctl_addr);

	/* subsys power on */
	val |= PWR_ON_BIT;
	writel(val, ctl_addr);

	/* wait until PWR_ACK = 1 */
	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_IS_PWR_CON_ON))
		ret = readx_poll_timeout_atomic(scpsys_pwr_ack_is_on, scpd, tmp, tmp > 0,
				MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
	else
		ret = readx_poll_timeout_atomic(scpsys_sta_is_on, scpd, tmp, tmp > 0,
				 MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
	if (ret < 0)
		goto err_pwr_ack;

	ret = scpsys_clk_enable(scpd->subsys_clk, MAX_SUBSYS_CLKS);
	if (ret < 0)
		goto err_pwr_ack;

	ret = scpsys_sram_enable(scpd, ctl_addr);
	if (ret < 0)
		goto err_sram;

	ret = scpsys_bus_protect_disable(scpd, MAX_STEPS - 1);
	if (ret < 0)
		goto err_sram;

	return 0;

err_sram:
	scpsys_clk_disable(scpd->subsys_clk, MAX_SUBSYS_CLKS);
err_pwr_ack:
	scpsys_clk_disable(scpd->clk, MAX_CLKS);
err_clk:
	scpsys_extb_iso_up(scpd);
	scpsys_regulator_disable(scpd);

	dev_notice(scp->dev, "Failed to power on domain %s\n", genpd->name);

	return ret;
}

static int scpsys_md_power_off(struct generic_pm_domain *genpd)
{
	struct scp_domain *scpd = container_of(genpd, struct scp_domain, genpd);
	struct scp *scp = scpd->scp;
	void __iomem *ctl_addr = scp->base + scpd->data->ctl_offs;
	u32 val;
	int ret, tmp;

	ret = scpsys_bus_protect_enable(scpd);
	if (ret < 0)
		goto out;

	ret = scpsys_sram_disable(scpd, ctl_addr);
	if (ret < 0)
		goto out;

	scpsys_clk_disable(scpd->subsys_clk, MAX_SUBSYS_CLKS);

	/* subsys power off */
	val = readl(ctl_addr) & ~PWR_ON_BIT;
	writel(val, ctl_addr);

	/* wait until PWR_ACK = 0 */
	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_IS_PWR_CON_ON))
		ret = readx_poll_timeout_atomic(scpsys_pwr_ack_is_on, scpd, tmp, tmp == 0,
				MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
	else
		ret = readx_poll_timeout_atomic(scpsys_sta_is_on, scpd, tmp, tmp == 0,
				 MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
	if (ret < 0)
		goto out;

	/* for md subsys, the isolation is prior to RST_B operation */
	scpsys_extb_iso_up(scpd);

	val &= ~PWR_RST_B_BIT;
	writel(val, ctl_addr);

	scpsys_clk_disable(scpd->clk, MAX_CLKS);

	ret = scpsys_regulator_disable(scpd);
	if (ret < 0)
		goto out;

	return 0;

out:
	dev_notice(scp->dev, "Failed to power off domain %s\n", genpd->name);

	return ret;
}

static int scpsys_apu_power_on(struct generic_pm_domain *genpd)
{
	struct scp_domain *scpd = container_of(genpd, struct scp_domain, genpd);
	struct scp *scp = scpd->scp;
	int ret = 0;

	if (!g_apucb || !g_apucb->apu_power_on)
		return -EPROBE_DEFER;

	if (g_apucb && g_apucb->apu_power_on) {
		ret = g_apucb->apu_power_on();
		if (ret) {
			dev_notice(scp->dev,
				"Failed to power on domain %s\n", genpd->name);
		}
	}
	return ret;
}

static int scpsys_apu_power_off(struct generic_pm_domain *genpd)
{
	struct scp_domain *scpd = container_of(genpd, struct scp_domain, genpd);
	struct scp *scp = scpd->scp;
	int ret = 0;

	if (!g_apucb || !g_apucb->apu_power_off)
		return -EPROBE_DEFER;

	if (g_apucb && g_apucb->apu_power_off) {
		ret = g_apucb->apu_power_off();
		if (ret) {
			dev_notice(scp->dev,
				"Failed to power off domain %s\n", genpd->name);
		}
	}
	return ret;
}

static int mtk_hwv_is_done(struct scp_domain *scpd)
{
	struct scp *scp = scpd->scp;
	u32 val;
	int ret;

	ret = regmap_read(scp->hwv_regmap, scpd->data->hwv_done_ofs, &val);
	if (ret)
		return ret;

	return !!(val & BIT(scpd->data->hwv_shift));
}

static int mtk_hwv_is_enable_done(struct scp_domain *scpd)
{
	struct scp *scp = scpd->scp;
	u32 val, val2, val3;
	int ret;

	ret = regmap_read(scp->hwv_regmap, scpd->data->hwv_done_ofs, &val);
	if (ret)
		return ret;
	ret = regmap_read(scp->hwv_regmap, scpd->data->hwv_en_ofs, &val2);
	if (ret)
		return ret;
	ret = regmap_read(scp->hwv_regmap, scpd->data->hwv_set_sta_ofs, &val3);
	if (ret)
		return ret;

	return (val & BIT(scpd->data->hwv_shift)) &&
	       (val2 & BIT(scpd->data->hwv_shift)) &&
	       !(val3 & BIT(scpd->data->hwv_shift));
}

static int mtk_hwv_is_disable_done(struct scp_domain *scpd)
{
	struct scp *scp = scpd->scp;
	u32 val, val2;
	int ret;

	ret = regmap_read(scp->hwv_regmap, scpd->data->hwv_done_ofs, &val);
	if (ret)
		return ret;
	ret = regmap_read(scp->hwv_regmap, scpd->data->hwv_clr_sta_ofs, &val2);
	if (ret)
		return ret;

	return (val & BIT(scpd->data->hwv_shift)) &&
	       !(val2 & BIT(scpd->data->hwv_shift));
}

static int scpsys_hwv_wait(struct scp_domain *scpd,
			  int (*is_done)(struct scp_domain *),
			  unsigned int timeout)
{
	int status, ret;

	ret = readx_poll_timeout_atomic(is_done, scpd, status, status != 0,
				       MTK_POLL_DELAY_US, timeout);
	if (ret)
		return ret;

	return status < 0 ? status : 0;
}

static int scpsys_hwv_power_on(struct generic_pm_domain *genpd)
{
	struct scp_domain *scpd = container_of(genpd, struct scp_domain, genpd);
	struct scp *scp = scpd->scp;
	u32 val = 0;
	int ret = 0;
	int i = 0;

	ret = scpsys_regulator_enable(scpd);
	if (ret < 0)
		goto err_regulator;

	ret = scpsys_clk_enable(scpd->clk, MAX_CLKS);
	if (ret)
		goto err_clk;

	ret = scpsys_clk_enable(scpd->lp_clk, MAX_CLKS);
	if (ret)
		goto err_lp_clk;

	/* wait for irq status idle */
	ret = scpsys_hwv_wait(scpd, mtk_hwv_is_done, MTK_POLL_IRQ_TIMEOUT);
	if (ret < 0)
		goto err_hwv_prepare;

	val = BIT(scpd->data->hwv_shift);
	ret = regmap_write(scp->hwv_regmap, scpd->data->hwv_set_ofs, val);
	if (ret)
		goto err_hwv_vote;
	do {
		ret = regmap_read(scp->hwv_regmap, scpd->data->hwv_set_ofs, &val);
		if (ret)
			goto err_hwv_vote;
		if ((val & BIT(scpd->data->hwv_shift)) != 0)
			break;

		if (i > MTK_POLL_HWV_PREPARE_CNT) {
			ret = -ETIMEDOUT;
			goto err_hwv_vote;
		}

		udelay(MTK_POLL_HWV_PREPARE_US);
		i++;
	} while (1);

	/* wait until VOTER_ACK = 1 */
	ret = scpsys_hwv_wait(scpd, mtk_hwv_is_enable_done, MTK_POLL_TIMEOUT);
	if (ret < 0)
		goto err_hwv_done;

	scpsys_clk_disable(scpd->lp_clk, MAX_CLKS);

	return 0;

err_hwv_done:
	dev_err(scp->dev, "Failed to hwv done timeout %s(%d)\n", genpd->name, ret);
err_hwv_vote:
	dev_err(scp->dev, "Failed to hwv vote timeout %s(%d %x)\n", genpd->name, ret, val);
err_hwv_prepare:
	regmap_read(scp->hwv_regmap, scpd->data->hwv_done_ofs, &val);
	dev_err(scp->dev, "Failed to hwv prepare timeout %s(%d %x)\n", genpd->name, ret, val);
err_lp_clk:
	dev_err(scp->dev, "Failed to enable lp clk %s(%d)\n", genpd->name, ret);
err_clk:
	val = scpsys_regulator_is_enabled(scpd);
	dev_err(scp->dev, "Failed to enable clk %s(%d %d)\n", genpd->name, ret, val);
err_regulator:
	dev_err(scp->dev, "Failed to power on domain %s(%d)\n", genpd->name, ret);

	return ret;
}

static int scpsys_hwv_power_off(struct generic_pm_domain *genpd)
{
	struct scp_domain *scpd = container_of(genpd, struct scp_domain, genpd);
	struct scp *scp = scpd->scp;
	u32 val = 0;
	int ret = 0;
	int i = 0;

	ret = scpsys_clk_enable(scpd->lp_clk, MAX_CLKS);
	if (ret)
		goto err_lp_clk;

	/* wait for irq status idle */
	ret = scpsys_hwv_wait(scpd, mtk_hwv_is_done, MTK_POLL_IRQ_TIMEOUT);
	if (ret < 0)
		goto err_hwv_prepare;

	val = BIT(scpd->data->hwv_shift);
	ret = regmap_write(scp->hwv_regmap, scpd->data->hwv_clr_ofs, val);
	if (ret)
		goto err_hwv_vote;
	do {
		ret = regmap_read(scp->hwv_regmap, scpd->data->hwv_clr_ofs, &val);
		if (ret)
			goto err_hwv_vote;
		if ((val & BIT(scpd->data->hwv_shift)) == 0)
			break;

		if (i > MTK_POLL_HWV_PREPARE_CNT) {
			ret = -ETIMEDOUT;
			goto err_hwv_vote;
		}
		i++;
		udelay(MTK_POLL_HWV_PREPARE_US);
	} while (1);

	/* delay 100us for stable status */
	udelay(100);

	/* wait until VOTER_ACK = 0 */
	ret = scpsys_hwv_wait(scpd, mtk_hwv_is_disable_done, MTK_POLL_TIMEOUT);
	if (ret < 0)
		goto err_hwv_done;

	scpsys_clk_disable(scpd->clk, MAX_CLKS);

	scpsys_clk_disable(scpd->lp_clk, MAX_CLKS);

	ret = scpsys_regulator_disable(scpd);
	if (ret < 0)
		goto err_regulator;

	return 0;

err_regulator:
	dev_err(scp->dev, "Failed to regulator disable %s(%d)\n", genpd->name, ret);
	return ret;
err_hwv_done:
	dev_err(scp->dev, "Failed to hwv done timeout %s(%d)\n", genpd->name, ret);
err_hwv_vote:
	dev_err(scp->dev, "Failed to hwv vote timeout %s(%d %x)\n", genpd->name, ret, val);
err_hwv_prepare:
	regmap_read(scp->hwv_regmap, scpd->data->hwv_done_ofs, &val);
	dev_err(scp->dev, "Failed to hwv prepare timeout %s(%d %x)\n", genpd->name, ret, val);
	scpsys_clk_disable(scpd->lp_clk, MAX_CLKS);
err_lp_clk:
	dev_err(scp->dev, "Failed to power off domain %s(%d)\n", genpd->name, ret);

	return ret;
}

static int init_subsys_clks(struct platform_device *pdev,
		const char *prefix, struct clk **clk)
{
	struct device_node *node = pdev->dev.of_node;
	u32 prefix_len, sub_clk_cnt = 0;
	struct property *prop;
	const char *clk_name;

	if (!node) {
		dev_notice(&pdev->dev, "Cannot find scpsys node: %ld\n",
			PTR_ERR(node));
		return -ENODEV;
	}

	prefix_len = strlen(prefix);

	of_property_for_each_string(node, "clock-names", prop, clk_name) {
		if (!strncmp(clk_name, prefix, prefix_len) &&
				(clk_name[prefix_len] == '-')) {
			if (sub_clk_cnt >= MAX_SUBSYS_CLKS) {
				dev_notice(&pdev->dev,
					"subsys clk out of range %d\n",
					sub_clk_cnt);
				return -EINVAL;
			}

			clk[sub_clk_cnt] = devm_clk_get(&pdev->dev,
						clk_name);

			if (IS_ERR(clk[sub_clk_cnt])) {
				dev_notice(&pdev->dev,
					"Subsys clk get fail %ld\n",
					PTR_ERR(clk[sub_clk_cnt]));
				return PTR_ERR(clk[sub_clk_cnt]);
			}
			sub_clk_cnt++;
		}
	}

	return sub_clk_cnt;
}

static int init_basic_clks(struct platform_device *pdev, struct clk **clk,
			const char * const *name)
{
	int i;

	for (i = 0; i < MAX_CLKS && name[i]; i++) {
		clk[i] = devm_clk_get(&pdev->dev, name[i]);

		if (IS_ERR(clk[i])) {
			dev_notice(&pdev->dev,
				"get basic clk %s fail %ld\n",
				name[i], PTR_ERR(clk[i]));
			return PTR_ERR(clk[i]);
		}
	}

	return 0;
}

static int mtk_pd_set_performance(struct generic_pm_domain *genpd,
				  unsigned int state)
{
	int i;
	struct scp_domain *scpd =
		container_of(genpd, struct scp_domain, genpd);
	struct scp_event_data scpe;
	struct scp *scp = scpd->scp;
	struct genpd_onecell_data *pd_data = &scp->pd_data;

	for (i = 0; i < pd_data->num_domains; i++) {
		if (genpd == pd_data->domains[i]) {
			dev_dbg(scp->dev, "%d. %s = %d\n",
				i, genpd->name, state);
			break;
		}
	}

	if (i == pd_data->num_domains)
		return 0;

	scpe.event_type = MTK_SCPSYS_PSTATE;
	scpe.genpd = genpd;
	scpe.domain_id = i;
	blocking_notifier_call_chain(&scpsys_notifier_list, state, &scpe);

	return 0;
}

static int mtk_pd_get_regmap(struct platform_device *pdev, struct regmap **map,
			     const char *name)
{
	if (!of_find_property(pdev->dev.of_node, name, NULL)) {
		*map = NULL;
		return 0;
	}

	*map = syscon_regmap_lookup_by_phandle(pdev->dev.of_node, name);
	if (IS_ERR(*map))
		return dev_err_probe(&pdev->dev, PTR_ERR(*map),
				     "Cannot find %s controller\n", name);

	return 0;
}

struct scp *init_scp_with_bus_list(struct platform_device *pdev,
			const struct scp_domain_data *scp_domain_data, int num,
			const struct scp_ctrl_reg *scp_ctrl_reg,
			const char * const bus_list[],
			unsigned int num_bus_type)
{
	struct genpd_onecell_data *pd_data;
	struct resource *res;
	int i, ret;
	struct scp *scp;

	scp = devm_kzalloc(&pdev->dev, sizeof(*scp), GFP_KERNEL);
	if (!scp)
		return ERR_PTR(-ENOMEM);

	scp->ctrl_reg.pwr_sta_offs = scp_ctrl_reg->pwr_sta_offs;
	scp->ctrl_reg.pwr_sta2nd_offs = scp_ctrl_reg->pwr_sta2nd_offs;

	scp->dev = &pdev->dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	scp->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(scp->base))
		return ERR_CAST(scp->base);

	scp->domains = devm_kcalloc(&pdev->dev,
				num, sizeof(*scp->domains), GFP_KERNEL);
	if (!scp->domains)
		return ERR_PTR(-ENOMEM);

	pd_data = &scp->pd_data;

	pd_data->domains = devm_kcalloc(&pdev->dev,
			num, sizeof(*pd_data->domains), GFP_KERNEL);
	if (!pd_data->domains)
		return ERR_PTR(-ENOMEM);

	scp->num_bus_type = num_bus_type;
	scp->bus_regmap = devm_kcalloc(&pdev->dev, num_bus_type,
				      sizeof(*scp->bus_regmap), GFP_KERNEL);
	if (!scp->bus_regmap)
		return ERR_PTR(-ENOMEM);

	/* Index zero is the terminator in every SoC's protection table. */
	for (i = 1; i < num_bus_type; i++) {
		if (!bus_list[i])
			continue;
		ret = mtk_pd_get_regmap(pdev, &scp->bus_regmap[i], bus_list[i]);
		if (ret)
			return ERR_PTR(ret);
	}

	ret = mtk_pd_get_regmap(pdev, &scp->hwv_regmap, "hw-voter-regmap");
	if (ret)
		return ERR_PTR(ret);

	/* A missing controller is optional only when no domain uses it. */
	for (i = 0; i < num; i++) {
		const struct scp_domain_data *data = &scp_domain_data[i];
		int step;

		for (step = 0; step < MAX_STEPS; step++) {
			u32 type = data->bp_table[step].type;

			if (!type)
				continue;
			if (type >= num_bus_type)
				return ERR_PTR(dev_err_probe(&pdev->dev, -EINVAL,
					"%s: invalid bus protection type %u\n",
					data->name, type));
			if (!scp->bus_regmap[type])
				return ERR_PTR(dev_err_probe(&pdev->dev, -ENODEV,
					"%s: missing bus controller %s\n",
					data->name, bus_list[type]));
		}

		if ((data->caps & MTK_SCPD_HWV_OPS) && !scp->hwv_regmap)
			return ERR_PTR(dev_err_probe(&pdev->dev, -ENODEV,
				"%s: missing hw-voter-regmap\n", data->name));
	}

	for (i = 0; i < num; i++) {
		struct scp_domain *scpd = &scp->domains[i];
		const struct scp_domain_data *data = &scp_domain_data[i];

		scpd->supply = devm_regulator_get_optional(&pdev->dev, data->name);
		if (IS_ERR(scpd->supply)) {
			if (PTR_ERR(scpd->supply) == -ENODEV)
				scpd->supply = NULL;
			else
				return ERR_CAST(scpd->supply);
		}
	}

	pd_data->num_domains = num;

	for (i = 0; i < num; i++) {
		struct scp_domain *scpd = &scp->domains[i];
		struct generic_pm_domain *genpd = &scpd->genpd;
		const struct scp_domain_data *data = &scp_domain_data[i];

		pd_data->domains[i] = genpd;
		scpd->scp = scp;

		scpd->data = data;

		ret = init_basic_clks(pdev, scpd->clk, data->basic_clk_name);
		if (ret)
			return ERR_PTR(ret);
		ret = init_basic_clks(pdev, scpd->lp_clk, data->basic_lp_clk_name);
		if (ret)
			return ERR_PTR(ret);

		if (data->subsys_clk_prefix) {
			ret = init_subsys_clks(pdev,
					data->subsys_clk_prefix,
					scpd->subsys_clk);
			if (ret < 0) {
				dev_notice(&pdev->dev,
					"%s: subsys clk unavailable\n",
					data->name);
				return ERR_PTR(ret);
			}
		}

		if (data->subsys_lp_clk_prefix) {
			ret = init_subsys_clks(pdev,
					data->subsys_lp_clk_prefix,
					scpd->subsys_lp_clk);
			if (ret < 0) {
				dev_notice(&pdev->dev,
					"%s: subsys clk unavailable\n",
					data->name);
				return ERR_PTR(ret);
			}
		}

		genpd->name = data->name;
		if (MTK_SCPD_CAPS(scpd, MTK_SCPD_MD_OPS)) {
			genpd->power_off = scpsys_md_power_off;
			genpd->power_on = scpsys_md_power_on;
		} else if (MTK_SCPD_CAPS(scpd, MTK_SCPD_APU_OPS)) {
			genpd->power_on = scpsys_apu_power_on;
			genpd->power_off = scpsys_apu_power_off;
		} else if (MTK_SCPD_CAPS(scpd, MTK_SCPD_HWV_OPS)) {
			genpd->power_on = scpsys_hwv_power_on;
			genpd->power_off = scpsys_hwv_power_off;
		} else {
			genpd->power_off = scpsys_power_off;
			genpd->power_on = scpsys_power_on;
		}

		if (MTK_SCPD_CAPS(scpd, MTK_SCPD_ACTIVE_WAKEUP))
			genpd->flags |= GENPD_FLAG_ACTIVE_WAKEUP;
		if (MTK_SCPD_CAPS(scpd, MTK_SCPD_ALWAYS_ON))
			genpd->flags |= GENPD_FLAG_ALWAYS_ON;

		/* Add opp table check first to avoid OF runtime parse failed */
		if (of_count_phandle_with_args(pdev->dev.of_node,
		    "operating-points-v2", NULL) > 0) {
			genpd->set_performance_state = mtk_pd_set_performance;
		}
	}

	return scp;
}
EXPORT_SYMBOL(init_scp_with_bus_list);

struct scp *init_scp(struct platform_device *pdev,
			const struct scp_domain_data *scp_domain_data, int num,
			const struct scp_ctrl_reg *scp_ctrl_reg)
{
	return init_scp_with_bus_list(pdev, scp_domain_data, num, scp_ctrl_reg,
				     scpsys_bus_list, ARRAY_SIZE(scpsys_bus_list));
}
EXPORT_SYMBOL(init_scp);

int mtk_register_power_domains_with_subdomains(struct platform_device *pdev,
			struct scp *scp, int num,
			const struct scp_subdomain *subdomains,
			int num_subdomains)
{
	struct genpd_onecell_data *pd_data = &scp->pd_data;
	int i, j, ret;

	for (j = 0; j < num_subdomains; j++) {
		if (subdomains[j].origin < 0 || subdomains[j].origin >= num ||
		    subdomains[j].subdomain < 0 || subdomains[j].subdomain >= num)
			return -EINVAL;
	}

	scpsys_init_flag = true;
	for (i = 0; i < num; i++) {
		struct scp_domain *scpd = &scp->domains[i];
		struct generic_pm_domain *genpd = &scpd->genpd;
		bool on = false;

		/*
		 * BYPASS_INIT_ON leaves acquisition to the domain's owner.
		 * Other domains must complete the hardware power-on sequence
		 * before genpd records them as on.
		 */
		if (!MTK_SCPD_CAPS(scpd, MTK_SCPD_BYPASS_INIT_ON)) {
			ret = genpd->power_on(genpd);
			if (ret) {
				dev_err(&pdev->dev, "Failed to initialize domain %s: %d\n",
					genpd->name, ret);
				goto remove_domains;
			}
			on = true;
		}

		ret = pm_genpd_init(genpd, NULL, !on);
		if (ret) {
			dev_err(&pdev->dev, "Failed to register domain %s: %d\n",
				genpd->name, ret);
			goto remove_domains;
		}
	}
	scpsys_init_flag = false;

	/* Publish only after the entire domain topology is usable. */
	for (j = 0; j < num_subdomains; j++) {
		ret = pm_genpd_add_subdomain(pd_data->domains[subdomains[j].origin],
					    pd_data->domains[subdomains[j].subdomain]);
		if (ret) {
			dev_err(&pdev->dev, "Failed to add subdomain %d -> %d: %d\n",
				subdomains[j].origin, subdomains[j].subdomain, ret);
			goto remove_subdomains;
		}
	}

	ret = of_genpd_add_provider_onecell(pdev->dev.of_node, pd_data);
	if (ret) {
		dev_err(&pdev->dev, "Failed to add OF provider: %d\n", ret);
		goto remove_subdomains;
	}

	return 0;

remove_subdomains:
	while (--j >= 0)
		pm_genpd_remove_subdomain(pd_data->domains[subdomains[j].origin],
					 pd_data->domains[subdomains[j].subdomain]);
remove_domains:
	scpsys_init_flag = false;
	/* Keep LK's hardware state; remove only unpublished genpd objects. */
	while (--i >= 0)
		pm_genpd_remove(pd_data->domains[i]);

	return ret;
}
EXPORT_SYMBOL(mtk_register_power_domains_with_subdomains);

int mtk_register_power_domains(struct platform_device *pdev,
			      struct scp *scp, int num)
{
	return mtk_register_power_domains_with_subdomains(pdev, scp, num, NULL, 0);
}
EXPORT_SYMBOL(mtk_register_power_domains);
