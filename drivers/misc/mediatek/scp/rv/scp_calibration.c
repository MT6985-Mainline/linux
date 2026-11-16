// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 * VLP ULPOSC two-phase calibration and SCP handoff from corot-t-oss
 * scp_dvfs.c. The measurement transaction follows clk-fmeter-mt6985.c;
 * native CCF does not expose the downstream mt_get_fmeter_freq() API.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/soc/mediatek/mtk_sip_svc.h>
#include "scp_native.h"

#define SCP_ULPOSC_CON0		0x210
#define SCP_ULPOSC_CON1		0x214
#define SCP_ULPOSC_CON2		0x218
#define SCP_FQMTR_CON0		0x230
#define SCP_FQMTR_CON1		0x234
#define SCP_FQMTR_ULPOSC2_ID	22
#define SCP_CALIBRATION_BITS	7
#define SCP_CALIBRATION_DEFAULT	0x40
#define SCP_CALIBRATION_MAX	0x7f
#define SCP_CALIBRATION_EXT_MAX	2
#define SCP_CALIBRATION_DIVISOR	512
#define SCP_CALIBRATION_TOLERANCE 40 /* per thousand, as in vendor firmware */

static void scp_clk_put(void *data)
{
	clk_put(data);
}

static struct clk *scp_get_dt_clock(struct scp_native *scp,
				   struct device_node *node, const char *name)
{
	struct clk *clk = of_clk_get_by_name(node, name);
	int ret;

	if (IS_ERR(clk))
		return clk;
	ret = devm_add_action_or_reset(scp->dev, scp_clk_put, clk);
	return ret ? ERR_PTR(ret) : clk;
}

int scp_clock_release(struct scp_native *scp)
{
	int ret;

	if (scp->resource_held) {
		ret = scp_smc(SCP_SIP_DVFS_CONTROL, SCP_SIP_RESOURCE_REQ,
			      SCP_RESOURCE_RELEASE);
		if (ret) {
			dev_err(scp->dev, "SCP 26 MHz resource release rejected: %d\n", ret);
			return ret;
		}
		scp->resource_held = false;
	}
	if (scp->clock_held) {
		clk_disable_unprepare(scp->clk_mux);
		scp->clock_held = false;
	}
	return 0;
}

static int scp_ulposc_switch(bool on)
{
	int ret = scp_smc(SCP_SIP_DVFS_CONTROL,
			  on ? SCP_SIP_ULPOSC_ON : SCP_SIP_ULPOSC_OFF, 0);

	udelay(50);
	return ret;
}

/* Return counts in the 512-cycle units used by scp_dvfs.c's search.
 * The real MT6985 VLP meter runs for 1024 cycles with a DIV4 input,
 * so one measured count represents two of these calibration units.
 */
static int scp_measure_ulposc(struct scp_native *scp, u32 *count)
{
	u32 original0, original1, result;
	int ret, restore0, restore1;

	ret = regmap_read(scp->fmeter, SCP_FQMTR_CON0, &original0);
	if (ret)
		return ret;
	ret = regmap_read(scp->fmeter, SCP_FQMTR_CON1, &original1);
	if (ret)
		return ret;
	/* Reset 1 -> 0 -> 1, select OSC_2, 1024 load cycles and DIV4. */
	ret = regmap_update_bits(scp->fmeter, SCP_FQMTR_CON0, BIT(15), 0);
	if (ret)
		goto restore;
	ret = regmap_update_bits(scp->fmeter, SCP_FQMTR_CON0, BIT(15), BIT(15));
	if (ret)
		goto restore;
	ret = regmap_update_bits(scp->fmeter, SCP_FQMTR_CON0, GENMASK(20, 16),
				 SCP_FQMTR_ULPOSC2_ID << 16);
	if (ret)
		goto restore;
	ret = regmap_update_bits(scp->fmeter, SCP_FQMTR_CON1, GENMASK(25, 16),
				 0x3ff << 16);
	if (ret)
		goto restore;
	ret = regmap_update_bits(scp->fmeter, SCP_FQMTR_CON0, GENMASK(31, 24),
				 3 << 24);
	if (ret)
		goto restore;
	ret = regmap_update_bits(scp->fmeter, SCP_FQMTR_CON0, GENMASK(14, 3), BIT(12));
	if (ret)
		goto restore;
	ret = regmap_update_bits(scp->fmeter, SCP_FQMTR_CON0, BIT(4), BIT(4));
	if (ret)
		goto restore;
	/* MT6985's VLP meter requires the fixed 40 us capture interval. */
	udelay(40);
	ret = regmap_read(scp->fmeter, SCP_FQMTR_CON1, &result);
	if (!ret) {
		result &= 0xffff;
		if (!result)
			ret = -EIO;
		else
			*count = result * 2;
	}
restore:
	restore0 = regmap_write(scp->fmeter, SCP_FQMTR_CON0, original0);
	restore1 = regmap_write(scp->fmeter, SCP_FQMTR_CON1, original1);
	return ret ?: restore0 ?: restore1;
}

static int scp_set_trim(struct scp_native *scp, u32 value, bool extension)
{
	u32 mask = extension ? GENMASK(8, 7) : GENMASK(6, 0);
	int ret = regmap_update_bits(scp->ulposc, SCP_ULPOSC_CON0, mask,
				     extension ? value << 7 : value);

	udelay(50);
	return ret;
}

static int scp_find_trim(struct scp_native *scp, u32 target, bool extension,
			 u16 *result)
{
	u32 low = 0, high = extension ? SCP_CALIBRATION_EXT_MAX : SCP_CALIBRATION_MAX;
	u32 middle, measured, low_diff, high_diff;
	int ret;

	while (low < high) {
		middle = (low + high) / 2;
		if (middle == low)
			break;
		ret = scp_set_trim(scp, middle, extension);
		if (ret)
			return ret;
		ret = scp_measure_ulposc(scp, &measured);
		if (ret)
			return ret;
		if (measured > target)
			high = middle;
		else
			low = middle;
	}
	ret = scp_set_trim(scp, low, extension);
	if (ret)
		return ret;
	ret = scp_measure_ulposc(scp, &measured);
	if (ret)
		return ret;
	low_diff = measured > target ? measured - target : target - measured;
	ret = scp_set_trim(scp, high, extension);
	if (ret)
		return ret;
	ret = scp_measure_ulposc(scp, &measured);
	if (ret)
		return ret;
	high_diff = measured > target ? measured - target : target - measured;
	*result = low_diff < high_diff ? low : high;
	return scp_set_trim(scp, *result, extension);
}

static int scp_calibrate_ulposc(struct scp_native *scp)
{
	const u32 registers[] = { SCP_ULPOSC_CON0, SCP_ULPOSC_CON1, SCP_ULPOSC_CON2 };
	u32 target, measured, i, j;
	int ret, off_ret;

	for (i = 0; i < scp->calibration_count; i++) {
		struct scp_ulposc_cali *cali = &scp->calibration[i];

		ret = scp_ulposc_switch(false);
		if (ret)
			return ret;
		for (j = 0; j < ARRAY_SIZE(registers); j++) {
			ret = regmap_write(scp->ulposc, registers[j], cali->config[j]);
			if (ret)
				goto off;
		}
		ret = scp_ulposc_switch(true);
		if (ret)
			goto off;
		target = cali->target * SCP_CALIBRATION_DIVISOR / 26;
		if (!scp->bypass_phase1) {
			ret = scp_set_trim(scp, SCP_CALIBRATION_DEFAULT, false);
			if (ret)
				goto off;
			ret = scp_find_trim(scp, target, true, &cali->extension);
			if (ret)
				goto off;
		}
		ret = scp_find_trim(scp, target, false, &cali->value);
		if (ret)
			goto off;
		ret = scp_measure_ulposc(scp, &measured);
		if (ret)
			goto off;
		if (measured <= target * (1000 - SCP_CALIBRATION_TOLERANCE) / 1000 ||
		    measured >= target * (1000 + SCP_CALIBRATION_TOLERANCE) / 1000) {
			dev_err(scp->dev, "ULPOSC calibration %u MHz failed, measured %u MHz\n",
				cali->target, measured * 26 / SCP_CALIBRATION_DIVISOR);
			ret = -ERANGE;
			goto off;
		}
		dev_info(scp->dev, "ULPOSC target %u MHz, measured %u MHz, trim %#x/%#x\n",
			 cali->target, measured * 26 / SCP_CALIBRATION_DIVISOR,
			 cali->extension, cali->value);
	}
	ret = 0;
off:
	off_ret = scp_ulposc_switch(false);
	return ret ?: off_ret;
}

static int scp_parse_calibration(struct scp_native *scp, struct device_node *node)
{
	const char *version;
	u32 count, i, j;
	int ret;

	scp->do_calibration = of_property_read_bool(node, "do-ulposc-cali");
	if (!scp->do_calibration)
		return 0;
	/* These are the real MT6985 VLP registers, not a guessed cross-SoC ABI. */
	if (!of_property_read_bool(node, "vlp-support") ||
	    !of_property_read_bool(node, "vlpck-support"))
		return -EINVAL;
	ret = of_property_read_string(node, "ulposc-cali-ver", &version);
	if (ret || strcmp(version, "v2"))
		return -EINVAL;
	ret = of_property_read_string(node, "clk-dbg-ver", &version);
	if (ret || strcmp(version, "v2"))
		return -EINVAL;
	scp->bypass_phase1 = of_property_read_bool(node, "vlpck-bypass-phase1");
	scp->fmeter = syscon_regmap_lookup_by_phandle(node, "fmeter-clksys");
	if (IS_ERR(scp->fmeter))
		return PTR_ERR(scp->fmeter);
	scp->ulposc = syscon_regmap_lookup_by_phandle(node, "ulposc-clksys");
	if (IS_ERR(scp->ulposc))
		return PTR_ERR(scp->ulposc);
	ret = of_property_read_u32(node, "ulposc-cali-num", &count);
	if (ret || !count || count > 32 ||
	    of_property_count_u32_elems(node, "ulposc-cali-target") != count ||
	    of_property_count_u32_elems(node, "ulposc-cali-config") != count * 3)
		return -EINVAL;
	scp->calibration_count = count;
	scp->calibration = devm_kcalloc(scp->dev, count,
					sizeof(*scp->calibration), GFP_KERNEL);
	if (!scp->calibration)
		return -ENOMEM;
	for (i = 0; i < count; i++) {
		struct scp_ulposc_cali *cali = &scp->calibration[i];

		ret = of_property_read_u32_index(node, "ulposc-cali-target", i, &cali->target);
		if (ret || !cali->target || cali->target > 3000)
			return -EINVAL;
		for (j = 0; j < ARRAY_SIZE(cali->config); j++) {
			ret = of_property_read_u32_index(node, "ulposc-cali-config", i * 3 + j,
						 &cali->config[j]);
			if (ret)
				return ret;
		}
	}
	return 0;
}

int scp_calibration_prepare(struct scp_native *scp)
{
	struct device_node *node;
	int ret;

	node = of_find_compatible_node(NULL, NULL, "mediatek,scp-dvfs");
	if (!node)
		return -ENODEV;
	scp->clk_mux = scp_get_dt_clock(scp, node, "clk_mux");
	if (IS_ERR(scp->clk_mux)) {
		ret = PTR_ERR(scp->clk_mux);
		goto put_node;
	}
	scp->clk_26m = scp_get_dt_clock(scp, node, "clk_pll_0");
	if (IS_ERR(scp->clk_26m)) {
		ret = PTR_ERR(scp->clk_26m);
		goto put_node;
	}
	ret = scp_parse_calibration(scp, node);
	if (ret)
		goto put_node;
	ret = clk_prepare_enable(scp->clk_mux);
	if (ret)
		goto put_node;
	scp->clock_held = true;
	ret = clk_set_parent(scp->clk_mux, scp->clk_26m);
	if (ret)
		goto release;
	ret = scp_smc(SCP_SIP_DVFS_CONTROL, SCP_SIP_RESOURCE_REQ, SCP_RESOURCE_26M);
	if (ret)
		goto release;
	scp->resource_held = true;
	if (scp->do_calibration) {
		ret = scp_calibrate_ulposc(scp);
		if (ret)
			goto release;
	}
	of_node_put(node);
	return 0;
release:
	scp_clock_release(scp);
put_node:
	of_node_put(node);
	return ret;
}

int scp_calibration_sync(struct scp_native *scp)
{
	u32 message[2], selector, i;
	int ret;

	if (!scp->do_calibration)
		return 0;
	message[0] = SCP_SYNC_ULPOSC_CALI;
	for (i = 0; i < scp->calibration_count; i++) {
		struct scp_ulposc_cali *cali = &scp->calibration[i];
		u16 trim = cali->value;

		if (READ_ONCE(scp->stopping) || !READ_ONCE(scp->firmware_ready))
			return -EHOSTDOWN;
		if (!scp->bypass_phase1)
			trim |= cali->extension << SCP_CALIBRATION_BITS;
		/* Firmware ABI: frequency and trim occupy low/high 16-bit halves. */
		message[1] = cali->target | ((u32)trim << 16);
		ret = mtk_ipi_send_compl(&scp_ipidev, IPI_OUT_C_SLEEP_0,
					 IPI_SEND_WAIT, message, ARRAY_SIZE(message), 500);
		if (ret)
			return ret;
	}
	ret = readl_poll_timeout(scp->clkctrl, selector,
				 ((selector >> 8) & (BIT(2) | BIT(3))) != 0,
				 10, 10000);
	if (ret)
		dev_err(scp->dev, "SCP did not switch to ULPOSC after calibration (selector %#x)\n",
			selector);
	return ret;
}
