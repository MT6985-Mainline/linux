// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 MediaTek Inc.
 */
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/regulator/consumer.h>
#include <linux/soc/mediatek/mtk_mmdvfs.h>
#if IS_ENABLED(CONFIG_MTK_SMI_DBG)
#include "../../misc/mediatek/smi/mtk-smi-dbg.h"
#endif

#define MAX_OPP_NUM (6)
#define MAX_MUX_NUM (10)
#define MAX_HOPPING_CLK_NUM (2)

#if IS_ENABLED(CONFIG_MMPROFILE)
#include "../../misc/mediatek/mmp/mmprofile.h"

struct mmdvfs_mmp_events_t {
	mmp_event mmdvfs;
	mmp_event freq_change;
};
static struct mmdvfs_mmp_events_t mmdvfs_mmp_events;
#endif

enum {
	ACTION_DEFAULT,
	ACTION_IHDM, /* Voltage Increase: Hopping First, Decrease: MUX First*/
};

enum {
	AOV_MUX, AOV_LP_PARENT, AOV_PARENT, AOV_CLK_NUM
};

struct mmdvfs_mux_data {
	const char *mux_name;
	struct clk *mux;
	struct clk *clk_src[MAX_OPP_NUM];
	struct clk *clk_src_lp;
	struct clk *clk_src_normal;
	u32 num_sources;
};

struct mmdvfs_hopping_data {
	const char *hopping_name;
	struct clk *hopping_clk;
	u32 hopping_rate[MAX_OPP_NUM];
};

struct mmdvfs_drv_data {
	bool need_change_voltage;
	u32 request_voltage;
	u32 num_muxes;
	struct mmdvfs_mux_data muxes[MAX_MUX_NUM];
	u32 num_hoppings;
	struct mmdvfs_hopping_data hoppings[MAX_HOPPING_CLK_NUM];
	u32 action;
	struct notifier_block nb;
	u32 voltages[MAX_OPP_NUM];
	u32 num_voltages;
	struct mutex lp_mutex;
	bool lp_mode;
	u32 last_opp_level;
	struct mmdvfs_dbg_data *dbg;
};

static struct regulator *vcore_reg_id;

#define MMDVFS_RECORD_NUM (10)
struct mmdvfs_opp_record {
	struct notifier_block nb;
	u8 idx;
	ktime_t time[MMDVFS_RECORD_NUM];
	u8 opp_level[MMDVFS_RECORD_NUM];
};

static struct mmdvfs_opp_record *mmdvfs_dbg;

struct mmdvfs_dbg_data {
	struct mmdvfs_drv_data *drv_data;
	struct regulator *reg;
	s32 force_step;
	s32 vote_step;
	bool is_notifier_registered;
	bool is_aov_enable;
	bool is_aov_mux_enable;
	struct clk *aov_clk[AOV_CLK_NUM];
};

static struct mmdvfs_dbg_data *dbg_data;
static bool mmdvfs_ready;

bool mmdvfs_is_ready(void)
{
	return smp_load_acquire(&mmdvfs_ready);
}
EXPORT_SYMBOL_GPL(mmdvfs_is_ready);

static u32 log_level;
enum mmdvfs_log_level {
	log_freq = 0,
};

static u32 trace_ena;
enum mmdvfs_trace_ena {
	trace_systrace_ena = 0,
};


static BLOCKING_NOTIFIER_HEAD(mmdvfs_notifier_list);

static record_opp record_opp_fp;

void mmdvfs_debug_record_opp_set_fp(record_opp fp)
{
	record_opp_fp = fp;
}
EXPORT_SYMBOL_GPL(mmdvfs_debug_record_opp_set_fp);

/**
 * register_mmdvfs_notifier - register multimedia clk changing notifier
 * @nb: notifier block
 *
 * Register notifier block to receive clk changing  notification.
 */
int register_mmdvfs_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&mmdvfs_notifier_list, nb);
}
EXPORT_SYMBOL_GPL(register_mmdvfs_notifier);

/**
 * unregister_mmdvfs_notifier - unregister multimedia clk changing notifier
 * @nb: notifier block
 *
 * Unregister clk changing notifier block.
 */
int unregister_mmdvfs_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&mmdvfs_notifier_list, nb);
}
EXPORT_SYMBOL_GPL(unregister_mmdvfs_notifier);

static int set_all_muxes(struct mmdvfs_drv_data *drv_data, u32 opp_level)
{
	u32 num_muxes = drv_data->num_muxes;
	u32 i;
	struct clk *mux, *clk_src;
	s32 err;

	MMDVFS_SYSTRACE_BEGIN("%s opp:%d\n", __func__, opp_level);
	for (i = 0; i < num_muxes; i++) {
		mux = drv_data->muxes[i].mux;
		clk_src = drv_data->muxes[i].clk_src[opp_level];
		err = clk_prepare_enable(mux);
		if (err) {
			pr_notice("prepare mux(%s) fail:%d opp_level:%d\n",
				  drv_data->muxes[i].mux_name, err, opp_level);
			MMDVFS_SYSTRACE_END();
			return err;
		}
		err = clk_set_parent(mux, clk_src);
		if (err)
			pr_notice("set parent(%s) fail:%d opp_level:%d\n",
				  drv_data->muxes[i].mux_name, err, opp_level);
		clk_disable_unprepare(mux);
		if (err) {
			MMDVFS_SYSTRACE_END();
			return err;
		}
	}
	MMDVFS_SYSTRACE_END();
	return 0;
}

static int set_all_hoppings(struct mmdvfs_drv_data *drv_data, u32 opp_level)
{
	u32 num_hoppings = drv_data->num_hoppings;
	u32 i, hopping_rate;
	struct clk *hopping;
	s32 err;

	MMDVFS_SYSTRACE_BEGIN("%s opp:%d\n", __func__, opp_level);
	for (i = 0; i < num_hoppings; i++) {
		hopping = drv_data->hoppings[i].hopping_clk;
		hopping_rate = drv_data->hoppings[i].hopping_rate[opp_level];
		err = clk_prepare_enable(hopping);
		if (err) {
			pr_notice("prepare hopping(%s) fail:%d opp_level:%d\n",
				  drv_data->hoppings[i].hopping_name,
				  err, opp_level);
			MMDVFS_SYSTRACE_END();
			return err;
		}
		err = clk_set_rate(hopping, hopping_rate);
		if (err)
			pr_notice("set %s rate(%u) fail:%d opp_level:%d\n",
				  drv_data->hoppings[i].hopping_name,
				  hopping_rate, err, opp_level);
		clk_disable_unprepare(hopping);
		if (err) {
			MMDVFS_SYSTRACE_END();
			return err;
		}
	}
	MMDVFS_SYSTRACE_END();
	return 0;
}

static int __maybe_unused mmdvfs_dbg_log_cb(struct notifier_block *nb,
		unsigned long value, void *v)
{
	int i;

	pr_notice("[smi] mmdvfs dump opp level start\n");
	for (i = mmdvfs_dbg->idx; i < MMDVFS_RECORD_NUM; i++) {
		pr_notice("[smi] (time, opp_level) = (%18llu, %d))\n",
				mmdvfs_dbg->time[i], mmdvfs_dbg->opp_level[i]);
	}
	for (i = 0; i < mmdvfs_dbg->idx; i++) {
		pr_notice("[smi] (time, opp_level) = (%18llu, %d))\n",
				mmdvfs_dbg->time[i], mmdvfs_dbg->opp_level[i]);
	}
	pr_notice("[smi] mmdvfs opp level end\n");

	return 0;
}

static int set_all_clk(struct mmdvfs_drv_data *drv_data,
			u32 voltage, bool vol_inc)
{
	s32 i;
	int ret = 0;
	struct mmdvfs_dbg_data *dbg = drv_data->dbg;
	record_opp recorder;
	u32 opp_level;
	MMDVFS_SYSTRACE_BEGIN("%s voltage:%u\n", __func__, voltage);
	for (i = drv_data->num_voltages - 1; i >= 0; i--) {
		if (voltage >= drv_data->voltages[i]) {
			opp_level = i;
			break;
		}
	}
	if (i < 0)
		opp_level = 0;

	mutex_lock(&drv_data->lp_mutex);
	do {
		if (opp_level == drv_data->last_opp_level)
			break;

		if (dbg && dbg->is_aov_enable && dbg->aov_clk[AOV_MUX]) {
			if (opp_level == 0 && drv_data->last_opp_level != MAX_OPP_NUM) {
				if (dbg->is_aov_mux_enable) {
					pr_notice("mmdvfs disable AOV MUX\n");
					clk_disable_unprepare(dbg->aov_clk[AOV_MUX]);
					dbg->is_aov_mux_enable = false;
				}
			} else if (drv_data->last_opp_level == 0 ||
				(drv_data->last_opp_level == MAX_OPP_NUM && opp_level != 0)) {
				pr_notice("mmdvfs enable AOV MUX\n");
				clk_prepare_enable(dbg->aov_clk[AOV_MUX]);
				dbg->is_aov_mux_enable = true;
			}
		}

		switch (drv_data->action) {
		/* Voltage Increase: Hopping First, Decrease: MUX First*/
		case ACTION_IHDM:
			if (vol_inc) {
				ret = set_all_hoppings(drv_data, opp_level);
				if (!ret)
					ret = set_all_muxes(drv_data, opp_level);
			} else {
				ret = set_all_muxes(drv_data, opp_level);
				if (!ret)
					ret = set_all_hoppings(drv_data, opp_level);
			}
			break;
		default:
			ret = set_all_muxes(drv_data, opp_level);
			break;
		}
		if (ret)
			break;
		drv_data->last_opp_level = opp_level;
	} while (0);
	mutex_unlock(&drv_data->lp_mutex);
	if (ret) {
		MMDVFS_SYSTRACE_END();
		return ret;
	}
	blocking_notifier_call_chain(&mmdvfs_notifier_list, opp_level, NULL);
	if (log_level & 1 << log_freq)
		pr_notice("set clk to opp level:%d\n", opp_level);

	/* Record mmdvfs opp log*/
	if (mmdvfs_dbg) {
		mmdvfs_dbg->time[mmdvfs_dbg->idx] = ktime_get();
		mmdvfs_dbg->opp_level[mmdvfs_dbg->idx] = opp_level;
		mmdvfs_dbg->idx = (mmdvfs_dbg->idx + 1) % MMDVFS_RECORD_NUM;
	}
	recorder = READ_ONCE(record_opp_fp);
	if (recorder)
		recorder(opp_level);
	MMDVFS_SYSTRACE_END();
#if IS_ENABLED(CONFIG_MMPROFILE)
	mmprofile_log_ex(
		mmdvfs_mmp_events.freq_change,
		MMPROFILE_FLAG_PULSE, vol_inc, opp_level);
#endif
	return 0;
}

static int regulator_event_notify(struct notifier_block *nb,
				  unsigned long event, void *data)
{
	unsigned long uV;
	int ret;
	struct mmdvfs_drv_data *drv_data;
	struct pre_voltage_change_data *pvc_data;

	drv_data = container_of(nb, struct mmdvfs_drv_data, nb);

	MMDVFS_SYSTRACE_BEGIN("%s event:0x%lx\n", __func__, event);
	if (event == REGULATOR_EVENT_PRE_VOLTAGE_CHANGE) {
		pvc_data = data;
		uV = pvc_data->min_uV;
		MMDVFS_SYSTRACE_BEGIN("%s event=PRE_VOLTAGE_CHANGE old=%lu new=%lu\n",
			__func__, pvc_data->old_uV, pvc_data->min_uV);
		if (uV < pvc_data->old_uV) {
			ret = set_all_clk(drv_data, uV, false);
			if (ret)
				return notifier_from_errno(ret);
			drv_data->request_voltage = uV;
		} else if (uV > pvc_data->old_uV || unlikely(drv_data->request_voltage == 0)) {
			drv_data->need_change_voltage = true;
		}
		if (log_level & 1 << log_freq)
			pr_notice("regulator event=PRE_VOLTAGE_CHANGE old=%lu new=%lu\n",
				pvc_data->old_uV, pvc_data->min_uV);
		MMDVFS_SYSTRACE_END();
	} else if (event == REGULATOR_EVENT_VOLTAGE_CHANGE) {
		uV = (unsigned long)data;
		MMDVFS_SYSTRACE_BEGIN("%s event=VOLTAGE_CHANGE voltage=%lu\n",
			__func__, uV);
		if (drv_data->need_change_voltage) {
			ret = set_all_clk(drv_data, uV, true);
			if (ret)
				return notifier_from_errno(ret);
			drv_data->need_change_voltage = false;
			drv_data->request_voltage = uV;
		}
		if (log_level & 1 << log_freq)
			pr_notice("regulator event=VOLTAGE_CHANGE voltage=%lu\n", uV);
		MMDVFS_SYSTRACE_END();
	} else if (event == REGULATOR_EVENT_ABORT_VOLTAGE_CHANGE) {
		uV = (unsigned long)data;
		MMDVFS_SYSTRACE_BEGIN("%s event=ABORT_VOLTAGE_CHANGE voltage=%lu\n",
			__func__, uV);
		/* If clk was changed, restore to previous setting */
		if (uV != drv_data->request_voltage) {
			ret = set_all_clk(drv_data, uV,
				    uV > drv_data->request_voltage);
			if (ret)
				return notifier_from_errno(ret);
			drv_data->need_change_voltage = false;
			drv_data->request_voltage = uV;
		}
		pr_info("regulator event=ABORT_VOLTAGE_CHANGE voltage=%lu\n",
			uV);
		MMDVFS_SYSTRACE_END();
	}
	MMDVFS_SYSTRACE_END();
	return 0;
}

static const struct of_device_id of_mmdvfs_match_tbl[] = {
	{
		.compatible = "mediatek,mmdvfs",
	},
	{}
};

/**
 * unregister_mmdvfs_notifier - unregister multimedia clk changing notifier
 * @nb: notifier block
 *
 * Unregister clk changing notifier block.
 */
void mmdvfs_set_lp_mode(bool lp_mode)
{
	struct mmdvfs_dbg_data *dbg = READ_ONCE(dbg_data);
	struct mmdvfs_drv_data *drv_data;
	struct mmdvfs_mux_data *mux;
	int i;

	if (!mmdvfs_is_ready() || !dbg)
		return;
	drv_data = dbg->drv_data;
	mutex_lock(&drv_data->lp_mutex);
	do {
		if (lp_mode == drv_data->lp_mode)
			break;

		drv_data->lp_mode = lp_mode;
		for (i = 0; i < drv_data->num_muxes; i++) {
			mux = &drv_data->muxes[i];
			if (!mux->clk_src_lp)
				continue;

			mux->clk_src[0] = lp_mode ? mux->clk_src_lp
				: mux->clk_src_normal;
		}
		if (drv_data->last_opp_level == 0)
			set_all_muxes(drv_data, 0);
	} while (0);
	mutex_unlock(&drv_data->lp_mutex);
}
EXPORT_SYMBOL_GPL(mmdvfs_set_lp_mode);

static int set_lp_mode(const char *val, const struct kernel_param *kp)
{
	int result;
	int new_lp_mode;

	result = kstrtoint(val, 0, &new_lp_mode);
	if (result) {
		pr_notice("mmdvfs lp mode failed: %d\n", result);
		return result;
	}
	mmdvfs_set_lp_mode(new_lp_mode);
	return 0;
}

static struct kernel_param_ops lp_mode_ops = {
	.set = set_lp_mode,
};
module_param_cb(lp_mode, &lp_mode_ops, NULL, 0644);
MODULE_PARM_DESC(lp_mode, "set low power mode");

int mmdvfs_dbg_clk_set(int step, bool is_force)
{
	struct mmdvfs_drv_data *drv_data;
	s32 ret = 0;
	u32 v_real = 0;
	int volt = 0;
	s32 last_force_step;

	if (!dbg_data) {
		pr_notice("%s: dbg_data is not ready!\n", __func__);
		return -EINVAL;
	}
	drv_data = dbg_data->drv_data;

	if (!drv_data) {
		pr_notice("%s: drv_data is not ready!\n", __func__);
		return -EINVAL;
	}

	if (step >= (int)drv_data->num_voltages) {
		pr_notice("%s: invalid force_step(%d)\n", __func__, step);
		return -EINVAL;
	}

	if (is_force) {
		last_force_step = dbg_data->force_step;
		dbg_data->force_step = step;
	} else {
		dbg_data->vote_step = step;
	}

	if (step < 0) {
		if (is_force && !dbg_data->is_notifier_registered) {
			ret = devm_regulator_register_notifier(
					dbg_data->reg, &drv_data->nb);
			dbg_data->is_notifier_registered = true;
			if (ret)
				pr_notice("%s: failed to register notifier(%d)\n",
					__func__, ret);
		}

		regulator_set_voltage(dbg_data->reg, 0, INT_MAX);
	} else {
		volt = drv_data->voltages[drv_data->num_voltages-1-step];
		if (is_force) {
			if (dbg_data->is_notifier_registered) {
				devm_regulator_unregister_notifier(
						dbg_data->reg, &drv_data->nb);
				dbg_data->is_notifier_registered = false;
			}
			if ((last_force_step < 0 && volt > drv_data->request_voltage)
				|| (last_force_step >= 0 && step < last_force_step)) {
				regulator_set_voltage(
					dbg_data->reg, volt, INT_MAX);
				if (!IS_ERR_OR_NULL(vcore_reg_id)) {
					v_real = regulator_get_voltage(vcore_reg_id);
					pr_notice("%s: step=%d volt=%d r_volt=%d is_force=%d\n",
							__func__, step, volt, v_real, is_force);
				}
				set_all_clk(drv_data, volt, true);
			} else {
				set_all_clk(drv_data, volt, false);
				regulator_set_voltage(
					dbg_data->reg, volt, INT_MAX);
				if (!IS_ERR_OR_NULL(vcore_reg_id)) {
					v_real = regulator_get_voltage(vcore_reg_id);
					pr_notice("%s: step=%d volt=%d r_volt=%d is_force=%d\n",
							__func__, step, volt, v_real, is_force);
				}
			}
		} else {
			regulator_set_voltage(dbg_data->reg, volt, INT_MAX);
		}
	}
	pr_notice("%s: step=%d volt=%d is_force=%d\n", __func__, step, volt, is_force);
	return ret;
}

int mmdvfs_set_force_step(int force_step)
{
	return mmdvfs_dbg_clk_set(force_step, true);
}
EXPORT_SYMBOL_GPL(mmdvfs_set_force_step);

static int set_force_step(const char *val, const struct kernel_param *kp)
{
	int result;
	int new_force_step;

	result = kstrtoint(val, 0, &new_force_step);
	if (result) {
		pr_notice("mmdvfs set force step failed: %d\n", result);
		return result;
	}
	return mmdvfs_set_force_step(new_force_step);
}

static struct kernel_param_ops set_force_step_ops = {
	.set = set_force_step,
};
module_param_cb(force_step, &set_force_step_ops, NULL, 0644);
MODULE_PARM_DESC(force_step, "force mmdvfs to specified step");

int mmdvfs_set_vote_step(int vote_step)
{
	return mmdvfs_dbg_clk_set(vote_step, false);
}
EXPORT_SYMBOL_GPL(mmdvfs_set_vote_step);

static int set_vote_step(const char *val, const struct kernel_param *kp)
{
	int result;
	int vote_step;

	result = kstrtoint(val, 0, &vote_step);
	if (result) {
		pr_notice("mmdvfs set vote step failed: %d\n", result);
		return result;
	}

	return mmdvfs_set_vote_step(vote_step);
}

static struct kernel_param_ops set_vote_step_ops = {
	.set = set_vote_step,
};
module_param_cb(vote_step, &set_vote_step_ops, NULL, 0644);
MODULE_PARM_DESC(vote_step, "vote mmdvfs to specified step");

module_param(log_level, uint, 0644);
MODULE_PARM_DESC(log_level, "mmdvfs log level");

#define MAX_DUMP (PAGE_SIZE - 1)
static struct mmdvfs_drv_data *mmdvfs_data;
int dump_setting(char *buf, const struct kernel_param *kp)
{
	struct mmdvfs_drv_data *drv_data = READ_ONCE(mmdvfs_data);
	u32 i, j;
	int length = 0;

	if (!mmdvfs_is_ready() || !drv_data)
		return -EAGAIN;
	length += snprintf(buf + length, MAX_DUMP  - length,
		"mux number:%d\n", drv_data->num_muxes);
	length += snprintf(buf + length, MAX_DUMP  - length,
		"mux:");
	for (i = 0; i < drv_data->num_muxes; i++) {
		length += snprintf(buf + length, MAX_DUMP  - length,
		"%s ", drv_data->muxes[i].mux_name);
	}
	length += snprintf(buf + length, MAX_DUMP  - length,
		"\n");
	length += snprintf(buf + length, MAX_DUMP  - length,
		"hopping number:%d\n", drv_data->num_hoppings);
	for (i = 0; i < drv_data->num_hoppings; i++) {
		length += snprintf(buf + length, MAX_DUMP  - length,
		"%s: ", drv_data->hoppings[i].hopping_name);
		for (j = 0; j < MAX_OPP_NUM; j++) {
			if (!drv_data->hoppings[i].hopping_rate[j])
				break;
			length += snprintf(buf + length, MAX_DUMP  - length,
				"%d ", drv_data->hoppings[i].hopping_rate[j]);
		}
		length += snprintf(buf + length, MAX_DUMP  - length,
		"\n");
	}
	length += snprintf(buf + length, MAX_DUMP  - length,
		"action: %d\n", drv_data->action);
	length += snprintf(buf + length, MAX_DUMP  - length,
		"voltage level:");
	for (i = 0; i < MAX_OPP_NUM; i++) {
		if (!drv_data->voltages[i])
			break;
		length += snprintf(buf + length, MAX_DUMP  - length,
		"%d ", drv_data->voltages[i]);
	}
	length += snprintf(buf + length, MAX_DUMP  - length,
		"\n");
	length += snprintf(buf + length, MAX_DUMP  - length,
		"request voltage:%d\n", drv_data->request_voltage);
#ifdef MMDVFS_DBG
	length += snprintf(buf + length, MAX_DUMP  - length,
		"force_step:%d\n", dbg_data->force_step);
	length += snprintf(buf + length, MAX_DUMP  - length,
		"vote_step:%d\n", dbg_data->vote_step);
#endif
	if (length >= MAX_DUMP)
		length = MAX_DUMP - 1;

	return length;
}

static struct kernel_param_ops dump_param_ops = {.get = dump_setting};
module_param_cb(dump_setting, &dump_param_ops, NULL, 0444);
MODULE_PARM_DESC(dump_setting, "dump mmdvfs current setting");

static void mmdvfs_remove_opp_table(void *data)
{
	dev_pm_opp_of_remove_table(data);
}

static int mmdvfs_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mmdvfs_drv_data *drv_data;
	struct mmdvfs_dbg_data *dbg;
	struct mmdvfs_opp_record *record;
	struct regulator *reg;
	struct property *prop, *sources;
	const char *name, *source;
	char prop_name[64];
	unsigned long freq = 0;
	struct dev_pm_opp *opp;
	u32 num, count, i;
	int ret;

	if (mmdvfs_is_ready())
		return -EBUSY;
	drv_data = devm_kzalloc(dev, sizeof(*drv_data), GFP_KERNEL);
	dbg = devm_kzalloc(dev, sizeof(*dbg), GFP_KERNEL);
	record = devm_kzalloc(dev, sizeof(*record), GFP_KERNEL);
	if (!drv_data || !dbg || !record)
		return -ENOMEM;
	mutex_init(&drv_data->lp_mutex);
	drv_data->last_opp_level = MAX_OPP_NUM;
	drv_data->dbg = dbg;
	dbg->drv_data = drv_data;
	dbg->force_step = -1;
	dbg->vote_step = -1;

	of_property_for_each_string(dev->of_node, "mediatek,support-mux", prop, name) {
		num = drv_data->num_muxes;
		if (num >= MAX_MUX_NUM)
			return -EINVAL;
		drv_data->muxes[num].mux = devm_clk_get(dev, name);
		if (IS_ERR(drv_data->muxes[num].mux))
			return dev_err_probe(dev, PTR_ERR(drv_data->muxes[num].mux),
					     "Mux %s is unavailable\n", name);
		drv_data->muxes[num].mux_name = name;
		snprintf(prop_name, sizeof(prop_name), "mediatek,mux-%s", name);
		count = 0;
		of_property_for_each_string(dev->of_node, prop_name, sources, source) {
			if (count >= MAX_OPP_NUM)
				return -EINVAL;
			drv_data->muxes[num].clk_src[count] = devm_clk_get(dev, source);
			if (IS_ERR(drv_data->muxes[num].clk_src[count]))
				return dev_err_probe(dev, PTR_ERR(drv_data->muxes[num].clk_src[count]),
						     "Clock source %s is unavailable\n", source);
			count++;
		}
		if (!count)
			return dev_err_probe(dev, -EINVAL, "Mux %s has no OPP parents\n", name);
		drv_data->muxes[num].num_sources = count;
		drv_data->num_muxes++;
	}
	if (!drv_data->num_muxes)
		return dev_err_probe(dev, -EINVAL, "No multimedia muxes configured\n");

	of_property_for_each_string(dev->of_node, "mediatek,support-hopping", prop, name) {
		num = drv_data->num_hoppings;
		if (num >= MAX_HOPPING_CLK_NUM)
			return -EINVAL;
		drv_data->hoppings[num].hopping_clk = devm_clk_get(dev, name);
		if (IS_ERR(drv_data->hoppings[num].hopping_clk))
			return dev_err_probe(dev, PTR_ERR(drv_data->hoppings[num].hopping_clk),
					     "Hopping clock %s is unavailable\n", name);
		drv_data->hoppings[num].hopping_name = name;
		snprintf(prop_name, sizeof(prop_name), "mediatek,hopping-%s", name);
		ret = of_property_count_u32_elems(dev->of_node, prop_name);
		if (ret <= 0 || ret > MAX_OPP_NUM)
			return -EINVAL;
		ret = of_property_read_u32_array(dev->of_node, prop_name,
				drv_data->hoppings[num].hopping_rate, ret);
		if (ret)
			return ret;
		drv_data->num_hoppings++;
	}
	of_property_read_u32(dev->of_node, "mediatek,action", &drv_data->action);
	if (drv_data->action > ACTION_IHDM)
		return -EINVAL;

	ret = dev_pm_opp_of_add_table(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot register multimedia OPPs\n");
	ret = devm_add_action_or_reset(dev, mmdvfs_remove_opp_table, dev);
	if (ret)
		return ret;
	ret = dev_pm_opp_get_opp_count(dev);
	if (ret <= 0 || ret > MAX_OPP_NUM)
		return dev_err_probe(dev, -EINVAL, "Invalid multimedia OPP count %d\n", ret);
	while (drv_data->num_voltages < ret) {
		opp = dev_pm_opp_find_freq_ceil(dev, &freq);
		if (IS_ERR(opp))
			return PTR_ERR(opp);
		drv_data->voltages[drv_data->num_voltages++] = dev_pm_opp_get_voltage(opp);
		dev_pm_opp_put(opp);
		freq++;
	}
	for (i = 0; i < drv_data->num_muxes; i++)
		if (drv_data->muxes[i].num_sources != drv_data->num_voltages)
			return dev_err_probe(dev, -EINVAL, "Mux %s parent count differs from OPP count\n",
					     drv_data->muxes[i].mux_name);
	for (i = 0; i < drv_data->num_hoppings; i++)
		for (num = 0; num < drv_data->num_voltages; num++)
			if (!drv_data->hoppings[i].hopping_rate[num])
				return -EINVAL;

	reg = devm_regulator_get(dev, "dvfsrc-vcore");
	if (IS_ERR(reg))
		return dev_err_probe(dev, PTR_ERR(reg), "DVFSRC Vcore is unavailable\n");
	dbg->reg = reg;
	i = 0;
	of_property_for_each_string(dev->of_node, "clock-names", prop, name) {
		if (strncmp(name, "aov", 3))
			continue;
		if (i >= AOV_CLK_NUM)
			return -EINVAL;
		dbg->aov_clk[i] = devm_clk_get(dev, name);
		if (IS_ERR(dbg->aov_clk[i]))
			return dev_err_probe(dev, PTR_ERR(dbg->aov_clk[i]),
					     "AOV clock %s is unavailable\n", name);
		i++;
	}
	if (i && i != AOV_CLK_NUM)
		return -EINVAL;
	i = 0;
	of_property_for_each_string(dev->of_node, "mediatek,mux-lp", prop, source) {
		if (i >= drv_data->num_muxes)
			return -EINVAL;
		if (*source) {
			drv_data->muxes[i].clk_src_lp = devm_clk_get(dev, source);
			if (IS_ERR(drv_data->muxes[i].clk_src_lp))
				return dev_err_probe(dev, PTR_ERR(drv_data->muxes[i].clk_src_lp),
						     "Low power clock %s is unavailable\n", source);
			drv_data->muxes[i].clk_src_normal = drv_data->muxes[i].clk_src[0];
		}
		i++;
	}

	drv_data->nb.notifier_call = regulator_event_notify;
	ret = devm_regulator_register_notifier(reg, &drv_data->nb);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot register voltage transition notifier\n");
	dbg->is_notifier_registered = true;
	vcore_reg_id = devm_regulator_get_optional(dev, "_vcore");
	if (IS_ERR(vcore_reg_id))
		vcore_reg_id = NULL;
	platform_set_drvdata(pdev, drv_data);
	WRITE_ONCE(mmdvfs_data, drv_data);
	WRITE_ONCE(mmdvfs_dbg, record);
	WRITE_ONCE(dbg_data, dbg);
	smp_store_release(&mmdvfs_ready, true);
	dev_info(dev, "Multimedia V2 ready: %u muxes, %u voltage levels\n",
		 drv_data->num_muxes, drv_data->num_voltages);
	return 0;
}

static void mmdvfs_remove(struct platform_device *pdev)
{
	struct mmdvfs_drv_data *drv_data = platform_get_drvdata(pdev);
	struct mmdvfs_dbg_data *dbg = drv_data->dbg;

	smp_store_release(&mmdvfs_ready, false);
	if (dbg->is_notifier_registered)
		devm_regulator_unregister_notifier(dbg->reg, &drv_data->nb);
	if (dbg->is_aov_mux_enable)
		clk_disable_unprepare(dbg->aov_clk[AOV_MUX]);
	WRITE_ONCE(dbg_data, NULL);
	WRITE_ONCE(mmdvfs_data, NULL);
	WRITE_ONCE(mmdvfs_dbg, NULL);
	vcore_reg_id = NULL;
}

void mtk_mmdvfs_aov_enable(const bool enable)
{
	if (!dbg_data)
		return;
	dbg_data->is_aov_enable = enable;
	if (!enable && dbg_data->aov_clk[AOV_MUX] && dbg_data->is_aov_mux_enable) {
		pr_notice("%s disable AOV MUX\n", __func__);
		clk_disable_unprepare(dbg_data->aov_clk[AOV_MUX]);
		dbg_data->is_aov_mux_enable = false;
	}
}
EXPORT_SYMBOL_GPL(mtk_mmdvfs_aov_enable);

static int mmdvfs_set_aov_clk(bool is_sys_resume)
{
	int err = 0, i;
	struct clk *parent;

	if (!dbg_data)
		return -EINVAL;

	if (dbg_data->is_aov_enable) {
		for (i = 0; i < AOV_CLK_NUM; i++) {
			if (!dbg_data->aov_clk[i]) {
				pr_notice("%s aov_clk[%d] is NULL\n", __func__, i);
				return -EINVAL;
			}
		}
		err = clk_prepare_enable(dbg_data->aov_clk[AOV_MUX]);
		if (err) {
			pr_notice("%s prepare aov_mux fail:%d\n", __func__, err);
			return -EINVAL;
		}
		parent = (is_sys_resume) ? dbg_data->aov_clk[AOV_PARENT] :
				dbg_data->aov_clk[AOV_LP_PARENT];
		err = clk_set_parent(dbg_data->aov_clk[AOV_MUX], parent);
		if (err)
			pr_notice("%s set parent fail:%d is_sys_resume:%d\n",
				__func__, err, is_sys_resume);
		clk_disable_unprepare(dbg_data->aov_clk[AOV_MUX]);
		pr_notice("%s: is_sys_resume=%d\n", __func__, is_sys_resume);
	}

	return err;
}

static int mmdvfs_pm_suspend(struct device *dev)
{
	mmdvfs_set_aov_clk(false);
	return 0;
}

static int mmdvfs_pm_resume(struct device *dev)
{
	mmdvfs_set_aov_clk(true);
	return 0;
}

bool mmdvfs_systrace_enabled(void)
{
	return trace_ena & (1 << trace_systrace_ena);
}

noinline int tracing_mark_write(char *fmt, ...)
{
#if IS_ENABLED(CONFIG_MTK_FTRACER)
	char buf[TRACE_MSG_LEN];
	va_list args;
	int len;

	va_start(args, fmt);
	len = vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	if (len >= TRACE_MSG_LEN) {
		pr_notice("%s trace size %u exceed limit\n", __func__, len);
		return -1;
	}
	trace_puts(buf);
#endif
	return 0;
}

module_param(trace_ena, uint, 0644);
MODULE_PARM_DESC(trace_ena, "mmdvfs trace enable");

static const struct dev_pm_ops mmdvfs_pm_ops = {
	.suspend = mmdvfs_pm_suspend,
	.resume  = mmdvfs_pm_resume,
};

static struct platform_driver mmdvfs_drv = {
	.probe = mmdvfs_probe,
	.remove = mmdvfs_remove,
	.driver = {
		.name = "mtk-mmdvfs",
		.of_match_table = of_mmdvfs_match_tbl,
		.pm = &mmdvfs_pm_ops,
	},
};

static int __init mtk_mmdvfs_init(void)
{
	s32 status;

	status = platform_driver_register(&mmdvfs_drv);
	if (status) {
		pr_notice("Failed to register MMDVFS driver(%d)\n", status);
		return -ENODEV;
	}
	return 0;
}

static void __exit mtk_mmdvfs_exit(void)
{
	platform_driver_unregister(&mmdvfs_drv);
}

module_init(mtk_mmdvfs_init);
module_exit(mtk_mmdvfs_exit);
MODULE_DESCRIPTION("MTK MMDVFS driver");
MODULE_AUTHOR("Anthony Huang<anthony.huang@mediatek.com>");
MODULE_LICENSE("GPL");
