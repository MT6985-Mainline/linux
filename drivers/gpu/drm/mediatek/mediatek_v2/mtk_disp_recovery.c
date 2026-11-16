// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/kthread.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/sched/clock.h>
#include <linux/delay.h>
#include <uapi/linux/sched/types.h>
#include <linux/pinctrl/consumer.h>

#ifndef DRM_CMDQ_DISABLE
#include <linux/soc/mediatek/mtk-cmdq-ext.h>
#else
#include "mtk-cmdq-ext.h"
#endif

#include "mtk_drm_drv.h"
#include "mtk_drm_ddp_comp.h"
#include "mtk_drm_crtc.h"
#include "mtk_drm_helper.h"
#include "mtk_drm_assert.h"
#include "mtk_drm_mmp.h"
#include "mtk_drm_trace.h"
#include "mtk_dump.h"
#include "mtk_disp_bdg.h"
#include "mtk_dsi.h"
#if defined(CONFIG_PXLW_IRIS)
#include "dsi_iris_mtk_api.h"
#include "dsi_iris_api.h"
#include "mi_disp/mi_disp_feature.h"
#endif

#define ESD_TRY_CNT 5
#define ESD_CHECK_PERIOD 2000 /* ms */
#define esd_timer_to_mtk_crtc(x) container_of(x, struct mtk_drm_crtc, esd_timer)

static DEFINE_MUTEX(pinctrl_lock);

/* pinctrl implementation */
long _set_state(struct drm_crtc *crtc, const char *name)
{
#ifndef CONFIG_FPGA_EARLY_PORTING
	struct mtk_drm_private *priv = crtc->dev->dev_private;
	struct pinctrl_state *pState = 0;
	long ret = 0;

	mutex_lock(&pinctrl_lock);
	if (!priv->pctrl) {
		DDPPR_ERR("this pctrl is null\n");
		ret = -1;
		goto exit;
	}

	pState = pinctrl_lookup_state(priv->pctrl, name);
	if (IS_ERR(pState)) {
		DDPPR_ERR("lookup state '%s' failed\n", name);
		ret = PTR_ERR(pState);
		goto exit;
	}

	/* Report an application failure as well as a missing state. */
	ret = pinctrl_select_state(priv->pctrl, pState);

exit:
	mutex_unlock(&pinctrl_lock);
	return ret; /* Good! */
#else
	return 0; /* Good! */
#endif
}

long disp_dts_gpio_init(struct device *dev, struct mtk_drm_private *private)
{
#ifndef CONFIG_FPGA_EARLY_PORTING
	long ret = 0;
	struct pinctrl *pctrl;

	/* retrieve */
	pctrl = devm_pinctrl_get(dev);
	if (IS_ERR(pctrl)) {
		DDPPR_ERR("Cannot find disp pinctrl!\n");
		ret = PTR_ERR(pctrl);
		goto exit;
	}

	private->pctrl = pctrl;

exit:
	return ret;
#else
	return 0;
#endif
}

static inline int _can_switch_check_mode(struct drm_crtc *crtc,
					 struct mtk_panel_ext *panel_ext)
{
	struct mtk_drm_private *priv = crtc->dev->dev_private;
	int ret = 0;

	if (panel_ext->params->cust_esd_check == 0 &&
	    panel_ext->params->lcm_esd_check_table[0].cmd != 0 &&
	    mtk_drm_helper_get_opt(priv->helper_opt,
				   MTK_DRM_OPT_ESD_CHECK_SWITCH))
		ret = 1;

	return ret;
}

static inline int _lcm_need_esd_check(struct mtk_panel_ext *panel_ext)
{
	int ret = 0;

	if (panel_ext->params->esd_check_enable == 1)
		ret = 1;

	return ret;
}

static inline int need_wait_esd_eof(struct drm_crtc *crtc,
				    struct mtk_panel_ext *panel_ext)
{
	int ret = 1;

	/*
	 * 1.vdo mode
	 * 2.cmd mode te
	 */
	if (!mtk_crtc_is_frame_trigger_mode(crtc))
		ret = 0;

	if (panel_ext->params->cust_esd_check == 0)
		ret = 0;

	return ret;
}

static void esd_cmdq_timeout_cb(struct cmdq_cb_data data)
{
	struct drm_crtc *crtc = data.data;
	struct mtk_drm_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_drm_esd_ctx *esd_ctx = mtk_crtc->esd_ctx;
	struct mtk_ddp_comp *output_comp = NULL;

	if (!crtc) {
		DDPMSG("%s find crtc fail\n", __func__);
		return;
	}

	DDPMSG("read flush fail\n");
	esd_ctx->chk_sta = 0xff;

	if (is_bdg_supported()) {
		if (mtk_crtc) {
			output_comp = mtk_ddp_comp_request_output(mtk_crtc);
			if (output_comp) {
				mtk_dump_analysis(output_comp);
				mtk_dump_reg(output_comp);
			}
		}
		bdg_dsi_dump_reg(DISP_BDG_DSI0);
	} else {
		mtk_drm_crtc_analysis(crtc);
		mtk_drm_crtc_dump(crtc);
	}
}


int _mtk_esd_check_read(struct drm_crtc *crtc)
{
	struct mtk_drm_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_ddp_comp *output_comp;
	struct mtk_panel_ext *panel_ext;
	struct cmdq_pkt *cmdq_handle, *cmdq_handle2;
	struct mtk_drm_esd_ctx *esd_ctx;
	int ret = 0;

	DDPINFO("[ESD%u]ESD read panel\n", drm_crtc_index(crtc));


	output_comp = mtk_ddp_comp_request_output(mtk_crtc);
	if (unlikely(!output_comp)) {
		DDPPR_ERR("%s:invalid output comp\n", __func__);
		return -EINVAL;
	}

	if (mtk_drm_is_idle(crtc) && mtk_dsi_is_cmd_mode(output_comp))
		return -ECANCELED;

	mtk_ddp_comp_io_cmd(output_comp, NULL, REQ_PANEL_EXT, &panel_ext);
	if (unlikely(!(panel_ext && panel_ext->params))) {
		DDPPR_ERR("%s:can't find panel_ext handle\n", __func__);
		return -EINVAL;
	}

	cmdq_handle = cmdq_pkt_create(mtk_crtc->gce_obj.client[CLIENT_CFG]);
	if (!cmdq_handle)
		return -ENOMEM;
	cmdq_handle->err_cb.cb = esd_cmdq_timeout_cb;
	cmdq_handle->err_cb.data = crtc;

	CRTC_MMP_MARK(drm_crtc_index(crtc), esd_check, 2, 1);

	if (mtk_dsi_is_cmd_mode(output_comp)) {
		if (mtk_crtc_with_sub_path(crtc, mtk_crtc->ddp_mode))
			mtk_crtc_wait_frame_done(mtk_crtc, cmdq_handle,
						 DDP_SECOND_PATH, 0);
		else
			mtk_crtc_wait_frame_done(mtk_crtc, cmdq_handle,
						 DDP_FIRST_PATH, 0);

		cmdq_pkt_wfe(cmdq_handle,
				     mtk_crtc->gce_obj.event[EVENT_CABC_EOF]);

		mtk_ddp_comp_io_cmd(output_comp, cmdq_handle, ESD_CHECK_READ,
				    (void *)mtk_crtc);

		cmdq_pkt_set_event(cmdq_handle,
				   mtk_crtc->gce_obj.event[EVENT_CABC_EOF]);
	} else { /* VDO mode */
		if (mtk_crtc_with_sub_path(crtc, mtk_crtc->ddp_mode))
			mtk_crtc_wait_frame_done(mtk_crtc, cmdq_handle,
						 DDP_SECOND_PATH, mtk_crtc->is_mml?0:1);
		else
			mtk_crtc_wait_frame_done(mtk_crtc, cmdq_handle,
						 DDP_FIRST_PATH, mtk_crtc->is_mml?0:1);

		if (mtk_crtc->msync2.msync_on) {
			u32 vfp_early_stop = 1;

			mtk_ddp_comp_io_cmd(output_comp, cmdq_handle, DSI_VFP_EARLYSTOP,
							&vfp_early_stop);
		}

		CRTC_MMP_MARK(drm_crtc_index(crtc), esd_check, 2, 2);

		mtk_ddp_comp_io_cmd(output_comp, cmdq_handle, DSI_STOP_VDO_MODE,
				    NULL);

		CRTC_MMP_MARK(drm_crtc_index(crtc), esd_check, 2, 3);

		mtk_ddp_comp_io_cmd(output_comp, cmdq_handle, ESD_CHECK_READ,
				    (void *)mtk_crtc);

		mtk_ddp_comp_io_cmd(output_comp, cmdq_handle,
				    DSI_START_VDO_MODE, NULL);

		mtk_disp_mutex_trigger(mtk_crtc->mutex[0], cmdq_handle);
		mtk_ddp_comp_io_cmd(output_comp, cmdq_handle, COMP_REG_START,
				    NULL);
	}
	esd_ctx = mtk_crtc->esd_ctx;
	esd_ctx->chk_sta = 0;
	CRTC_MMP_MARK(drm_crtc_index(crtc), esd_check, 2, 4);
	ret = cmdq_pkt_flush(cmdq_handle);

	CRTC_MMP_MARK(drm_crtc_index(crtc), esd_check, 2, 5);


	mtk_ddp_comp_io_cmd(output_comp, NULL, CONNECTOR_READ_EPILOG,
				    NULL);
	if (ret || esd_ctx->chk_sta == 0xff) {
		if (!ret)
			ret = -ETIMEDOUT;
		if (need_wait_esd_eof(crtc, panel_ext)) {
			/* TODO: set ESD_EOF event through CPU is better */
			mtk_crtc_pkt_create(&cmdq_handle2, crtc,
				mtk_crtc->gce_obj.client[CLIENT_CFG]);
			if (!cmdq_handle2) {
				DDPPR_ERR("failed to allocate ESD read epilog packet\n");
				goto done;
			}

			cmdq_pkt_set_event(
				cmdq_handle2,
				mtk_crtc->gce_obj.event[EVENT_CABC_EOF]);
			cmdq_pkt_flush(cmdq_handle2);
			cmdq_pkt_destroy(cmdq_handle2);
		}
		goto done;
	}

	ret = mtk_ddp_comp_io_cmd(output_comp, NULL, ESD_CHECK_CMP,
				  (void *)mtk_crtc);
done:
	cmdq_pkt_destroy(cmdq_handle);
	return ret;
}

static irqreturn_t _esd_check_ext_te_irq_handler(int irq, void *data)
{
	struct mtk_drm_esd_ctx *esd_ctx = data;

	if (READ_ONCE(esd_ctx->stopping) || !READ_ONCE(esd_ctx->chk_active))
		return IRQ_HANDLED;
	atomic_set(&esd_ctx->ext_te_event, 1);
	wake_up_interruptible(&esd_ctx->ext_te_wq);

	return IRQ_HANDLED;
}

static int _mtk_esd_check_eint(struct drm_crtc *crtc)
{
	struct mtk_drm_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_drm_esd_ctx *esd_ctx = mtk_crtc->esd_ctx;
	struct mtk_drm_private *priv = crtc->dev->dev_private;
	bool use_te1;
	long waited;
	int ret;

	DDPINFO("[ESD%u]ESD check eint\n", drm_crtc_index(crtc));

	if (unlikely(!esd_ctx)) {
		DDPPR_ERR("%s:invalid ESD context\n", __func__);
		return -EINVAL;
	}
	if (READ_ONCE(esd_ctx->stopping) || !READ_ONCE(esd_ctx->chk_active) ||
	    !mtk_crtc_esd_check_ready(crtc, MTK_ESD_CHECK_GENERIC))
		return -ECANCELED;

	use_te1 = mtk_drm_helper_get_opt(priv->helper_opt,
				       MTK_DRM_OPT_DUAL_TE) &&
		   atomic_read(&mtk_crtc->d_te.te_switched) == 1;
	if (!use_te1 && !esd_ctx->eint_requested)
		return -ENODEV;

	/* Each health check must see a new edge from its selected TE source. */
	atomic_set(&esd_ctx->ext_te_event, 0);
	if (use_te1)
		atomic_set(&mtk_crtc->d_te.esd_te1_en, 1);
	else
		enable_irq(esd_ctx->eint_irq);

	waited = wait_event_interruptible_timeout(esd_ctx->ext_te_wq,
		READ_ONCE(esd_ctx->stopping) ||
		!READ_ONCE(esd_ctx->chk_active) ||
		!mtk_crtc_esd_check_ready(crtc, MTK_ESD_CHECK_GENERIC) ||
		atomic_read(&esd_ctx->ext_te_event), HZ / 2);

	if (use_te1)
		atomic_set(&mtk_crtc->d_te.esd_te1_en, 0);
	else
		disable_irq(esd_ctx->eint_irq);

	if (READ_ONCE(esd_ctx->stopping) || !READ_ONCE(esd_ctx->chk_active) ||
	    !mtk_crtc_esd_check_ready(crtc, MTK_ESD_CHECK_GENERIC))
		ret = -ECANCELED;
	else if (waited < 0)
		ret = waited;
	else
		ret = waited > 0 && atomic_read(&esd_ctx->ext_te_event) ? 0 : 1;
	atomic_set(&esd_ctx->ext_te_event, 0);
	return ret;
}

static int mtk_drm_request_eint(struct drm_crtc *crtc)
{
	struct mtk_drm_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_drm_esd_ctx *esd_ctx = mtk_crtc->esd_ctx;
	struct mtk_ddp_comp *output_comp;
	struct device_node *node;
	char *compat_str = NULL;
	int irq, ret;

	if (unlikely(!esd_ctx)) {
		DDPPR_ERR("%s:invalid ESD context\n", __func__);
		return -EINVAL;
	}
	if (esd_ctx->eint_requested) {
		DDPPR_ERR("%s: reentry with inited eint_irq %d\n",
			  __func__, esd_ctx->eint_irq);
		return -EINVAL;
	}

	output_comp = mtk_ddp_comp_request_output(mtk_crtc);
	if (unlikely(!output_comp)) {
		DDPPR_ERR("%s:invalid output comp\n", __func__);
		return -EINVAL;
	}

	mtk_ddp_comp_io_cmd(output_comp, NULL, REQ_ESD_EINT_COMPAT,
			    &compat_str);
	if (unlikely(!compat_str || !*compat_str)) {
		DDPPR_ERR("%s: invalid compat string\n", __func__);
		return -EINVAL;
	}
	node = of_find_compatible_node(NULL, NULL, compat_str);
	if (unlikely(!node)) {
		DDPPR_ERR("can't find ESD TE eint compatible node %s\n", compat_str);
		return -ENODEV;
	}

	irq = of_irq_get(node, 0);
	of_node_put(node);
	if (irq <= 0)
		return irq ? irq : -ENXIO;

	/* Leave EINT disabled until a ready display begins a health check. */
	ret = request_irq(irq, _esd_check_ext_te_irq_handler,
			  IRQF_TRIGGER_RISING | IRQF_NO_AUTOEN,
			  "ESD_TE-eint", esd_ctx);
	if (ret) {
		DDPPR_ERR("eint irq line %u not available! %d\n", irq, ret);
		return ret;
	}
	esd_ctx->eint_irq = irq;
	esd_ctx->eint_requested = true;

	/* EINT setup selects GPIO mode; restore the authentic DSI TE mux. */
	ret = _set_state(crtc, output_comp->id == DDP_COMPONENT_DSI0 ?
			 "mode_te_te" : "mode_te_te1");
	if (ret) {
		DDPPR_ERR("failed to restore DSI TE pin state: %d\n", ret);
		free_irq(esd_ctx->eint_irq, esd_ctx);
		esd_ctx->eint_requested = false;
		esd_ctx->eint_irq = -1;
	}
	return ret;
}

static int mtk_drm_esd_check(struct drm_crtc *crtc)
{
	struct mtk_drm_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_panel_ext *panel_ext;
	struct mtk_drm_esd_ctx *esd_ctx = mtk_crtc->esd_ctx;
	int ret = 0;

	if (!esd_ctx || READ_ONCE(esd_ctx->stopping) ||
	    !READ_ONCE(esd_ctx->chk_active) ||
	    !mtk_crtc_esd_check_ready(crtc, MTK_ESD_CHECK_GENERIC) ||
	    !crtc->state || !crtc->state->active)
		return -ECANCELED;

#if defined(CONFIG_PXLW_IRIS)
	if (is_mi_dev_support_iris() && iris_is_chip_supported()) {
		if (iris_recovery_is_needed()) {
			return -1;
		} else if (!iris_is_pt_mode()) {
			DDPINFO("[ESD] continue\n");
			//return 0;
		}
	}
#endif
	CRTC_MMP_EVENT_START(drm_crtc_index(crtc), esd_check, 0, 0);

	if (mtk_crtc->enabled == 0) {
		CRTC_MMP_MARK(drm_crtc_index(crtc), esd_check, 0, 99);
		DDPINFO("[ESD] CRTC %d disable. skip esd check\n",
			drm_crtc_index(crtc));
		ret = -ECANCELED;
		goto done;
	}

	panel_ext = mtk_crtc->panel_ext;
	if (unlikely(!(panel_ext && panel_ext->params))) {
		DDPPR_ERR("can't find panel_ext handle\n");
		ret = -EINVAL;
		goto done;
	}

	/* Check panel EINT */
	if (panel_ext->params->cust_esd_check == 0 &&
	    esd_ctx->chk_mode == READ_EINT) {
		CRTC_MMP_MARK(drm_crtc_index(crtc), esd_check, 1, 0);
		ret = _mtk_esd_check_eint(crtc);
	} else { /* READ LCM CMD  */
		CRTC_MMP_MARK(drm_crtc_index(crtc), esd_check, 2, 0);

#if defined(CONFIG_PXLW_IRIS)
		ret = _mtk_esd_check_eint(crtc);
		if (ret) {
			DDPINFO("No TE signal triggers ESD\n");
		} else if (iris_is_pt_mode()) {
			ret = _mtk_esd_check_read(crtc);
		}
#else
		ret = _mtk_esd_check_read(crtc);
#endif
	}

	/* switch ESD check mode */
	if (_can_switch_check_mode(crtc, panel_ext) &&
	    !mtk_crtc_is_frame_trigger_mode(crtc))
		esd_ctx->chk_mode =
			(esd_ctx->chk_mode == READ_EINT) ? READ_LCM : READ_EINT;

done:
	CRTC_MMP_EVENT_END(drm_crtc_index(crtc), esd_check, 0, ret);
	return ret;
}

static atomic_t panel_dead;
int get_panel_dead_flag(void) {
	return atomic_read(&panel_dead);
}
EXPORT_SYMBOL(get_panel_dead_flag);

void set_panel_dead_flag(int value) {
	atomic_set(&panel_dead, value);
}
EXPORT_SYMBOL(set_panel_dead_flag);

static int mtk_drm_esd_recover(struct drm_crtc *crtc)
{
	struct mtk_drm_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_ddp_comp *output_comp;
	struct mtk_drm_private *priv = mtk_crtc->base.dev->dev_private;
	struct mtk_dsi *dsi;
	struct cmdq_pkt *cmdq_handle = NULL;
	struct cmdq_pkt *restart_handle = NULL;
	bool output_was_enabled;
	int ret = 0;

	atomic_set(&panel_dead, 1);
	CRTC_MMP_EVENT_START(drm_crtc_index(crtc), esd_recovery, 0, 0);
	if (crtc->state && !crtc->state->active) {
		DDPMSG("%s: crtc is inactive\n", __func__);
		goto done;
	}
	CRTC_MMP_MARK(drm_crtc_index(crtc), esd_recovery, 0, 1);
	output_comp = mtk_ddp_comp_request_output(mtk_crtc);

	if (unlikely(!output_comp)) {
		DDPPR_ERR("%s: invalid output comp\n", __func__);
		ret = -EINVAL;
		goto done;
	}
	dsi = container_of(output_comp, struct mtk_dsi, ddp_comp);
	mtk_drm_idlemgr_kick(__func__, &mtk_crtc->base, 0);

	if (mtk_crtc->is_mml) {
		mtk_crtc_pkt_create(&cmdq_handle, crtc, mtk_crtc->gce_obj.client[CLIENT_CFG]);
		if (!cmdq_handle) {
			ret = -ENOMEM;
			goto done;
		}
		mtk_crtc_mml_racing_stop_sync(crtc, cmdq_handle,
					      mtk_crtc_is_frame_trigger_mode(crtc) ? true : false);
		/* VDO stop is added to this packet by the DSI disable path. */
	}

	cmdq_mbox_stop(mtk_crtc->gce_obj.client[CLIENT_CFG]);

	/* The CMD disable path does not consume an incoming MML packet. */
	if (cmdq_handle && mtk_dsi_is_cmd_mode(output_comp)) {
		ret = cmdq_pkt_flush(cmdq_handle);
		cmdq_pkt_destroy(cmdq_handle);
		cmdq_handle = NULL;
		if (ret < 0)
			goto done;
	}
	output_was_enabled = READ_ONCE(dsi->output_en);
	ret = mtk_ddp_comp_io_cmd(output_comp, cmdq_handle,
				  CONNECTOR_PANEL_DISABLE, NULL);
	if (cmdq_handle) {
		/* Active VDO/OPENING consumes it; early returns leave it owned. */
		if (!output_was_enabled || READ_ONCE(dsi->output_en))
			cmdq_pkt_destroy(cmdq_handle);
		cmdq_handle = NULL;
	}
	if (ret < 0)
		goto done;
	if (is_bdg_supported())
		bdg_common_deinit(DISP_BDG_DSI0, NULL, dsi);

	mtk_drm_crtc_disable(crtc, true);
	CRTC_MMP_MARK(drm_crtc_index(crtc), esd_recovery, 0, 2);

	if (mtk_drm_helper_get_opt(priv->helper_opt,
		MTK_DRM_OPT_MMQOS_SUPPORT)) {
		if (drm_crtc_index(crtc) == 0)
			mtk_disp_set_hrt_bw(mtk_crtc,
				mtk_crtc->qos_ctx->last_hrt_req);
	}

	mtk_drm_crtc_enable(crtc);
	CRTC_MMP_MARK(drm_crtc_index(crtc), esd_recovery, 0, 3);

	if (is_bdg_supported())
		mtk_output_bdg_enable(dsi, false);
	ret = mtk_ddp_comp_io_cmd(output_comp, NULL, CONNECTOR_PANEL_ENABLE, NULL);
	if (ret < 0)
		goto done;

	if (mtk_crtc->is_mml)
		mtk_crtc_mml_racing_resubmit(crtc, NULL);
	CRTC_MMP_MARK(drm_crtc_index(crtc), esd_recovery, 0, 4);

#ifdef CONFIG_MI_DISP_ESD_CHECK
	ret = mtk_ddp_comp_io_cmd(output_comp, NULL, ESD_RESTORE_BACKLIGHT, NULL);
	if (ret < 0)
		goto done;
#endif

	mtk_crtc_hw_block_ready(crtc);
	if (mtk_crtc_is_frame_trigger_mode(crtc)) {
		mtk_crtc_pkt_create(&restart_handle, &mtk_crtc->base,
			mtk_crtc->gce_obj.client[CLIENT_CFG]);
		if (!restart_handle) {
			ret = -ENOMEM;
			goto done;
		}

		ret = cmdq_pkt_set_event(restart_handle,
			mtk_crtc->gce_obj.event[EVENT_STREAM_DIRTY]);
		if (ret < 0)
			goto done;
		ret = cmdq_pkt_set_event(restart_handle,
			mtk_crtc->gce_obj.event[EVENT_CABC_EOF]);
		if (ret < 0)
			goto done;
		ret = cmdq_pkt_set_event(restart_handle,
			mtk_crtc->gce_obj.event[EVENT_ESD_EOF]);
		if (ret < 0)
			goto done;

		ret = cmdq_pkt_flush(restart_handle);
		if (ret < 0)
			goto done;
	}
	mtk_drm_idlemgr_kick(__func__, &mtk_crtc->base, 0);
	CRTC_MMP_MARK(drm_crtc_index(crtc), esd_recovery, 0, 5);

done:
	if (restart_handle)
		cmdq_pkt_destroy(restart_handle);
	if (cmdq_handle)
		cmdq_pkt_destroy(cmdq_handle);
	if (ret >= 0)
		atomic_set(&panel_dead, 0);
	CRTC_MMP_EVENT_END(drm_crtc_index(crtc), esd_recovery, 0, ret);

	return ret;
}

int mtk_drm_esd_testing_process(struct mtk_drm_esd_ctx *esd_ctx, bool need_lock)
{
	struct mtk_drm_private *private;
	struct drm_crtc *crtc;
	struct mtk_drm_crtc *mtk_crtc;
	struct disp_event event;
	unsigned int crtc_idx;
	u8 panel_dead = 0;
	int ret = 0;

	if (!esd_ctx)
		return -EINVAL;
	if (READ_ONCE(esd_ctx->stopping) || !READ_ONCE(esd_ctx->chk_active))
		return 0;
	crtc = READ_ONCE(esd_ctx->crtc);
	if (!crtc)
		return -EINVAL;
	mtk_crtc = to_mtk_crtc(crtc);
	private = crtc->dev->dev_private;

	if (need_lock) {
		mutex_lock(&private->commit.lock);
		DDP_MUTEX_LOCK(&mtk_crtc->lock, __func__, __LINE__);
	}
	/* A wakeup may belong to an output stopped while locks were pending. */
	if (READ_ONCE(esd_ctx->stopping) || !READ_ONCE(esd_ctx->chk_active) ||
	    READ_ONCE(esd_ctx->crtc) != crtc ||
	    !mtk_crtc_esd_check_ready(crtc, MTK_ESD_CHECK_GENERIC))
		goto unlock;
	if (!mtk_drm_is_idle(crtc))
		atomic_set(&esd_ctx->target_time, 0);

	crtc_idx = drm_crtc_index(crtc);
	event.disp_id = MI_DISP_PRIMARY;
	event.type = MI_DISP_EVENT_PANEL_DEAD;
	event.length = sizeof(panel_dead);
	ret = mtk_drm_esd_check(crtc);
	if (ret == -ECANCELED || ret == -ERESTARTSYS ||
	    READ_ONCE(esd_ctx->stopping) || !READ_ONCE(esd_ctx->chk_active) ||
	    !mtk_crtc_esd_check_ready(crtc, MTK_ESD_CHECK_GENERIC)) {
		ret = 0;
		goto unlock;
	}
	if (!ret) {
		if (esd_ctx->recovery_pending)
			DDPPR_ERR("[ESD%u] esd recovery success\n", crtc_idx);
		/* A real health pass also clears a failed MI recovery's flag. */
		if (esd_ctx->recovery_pending || get_panel_dead_flag()) {
			set_panel_dead_flag(0);
			panel_dead = 0;
			mi_disp_feature_event_notify(&event, &panel_dead);
		}
		esd_ctx->recovery_pending = false;
		esd_ctx->recovery_count = 0;
		goto unlock;
	}

	if (esd_ctx->recovery_count >= ESD_TRY_CNT) {
		DDPPR_ERR("[ESD%u]after esd recovery %d times, still fail, disable esd check\n",
			  crtc_idx, ESD_TRY_CNT);
		mtk_disp_esd_check_switch(crtc, false);
		ret = 0;
		goto unlock;
	}

	DDPPR_ERR("[ESD%u]esd check fail, will do esd recovery. try=%u\n",
		  crtc_idx, esd_ctx->recovery_count);
	panel_dead = 1;
	mi_disp_feature_event_notify(&event, &panel_dead);
	esd_ctx->recovery_pending = true;
	ret = mtk_drm_esd_recover(crtc);
	esd_ctx->recovery_count++;
	if (ret < 0) {
		DDPPR_ERR("[ESD%u]esd recovery failed: %d\n", crtc_idx, ret);
		goto unlock;
	}
	panel_dead = 0;
	mi_disp_feature_event_notify(&event, &panel_dead);
#if defined(CONFIG_PXLW_IRIS)
	if (is_mi_dev_support_iris() && iris_is_chip_supported()) {
		atomic_set(&private->idle_need_repaint, 1);
		drm_trigger_repaint(DRM_REPAINT_FOR_ESD, crtc->dev);
		iris_reset_esd_flag(crtc);
	}
#endif
	/* Recovery changed the epoch. A new hardware frame must arm us again. */
unlock:
	if (need_lock) {
		DDP_MUTEX_UNLOCK(&mtk_crtc->lock, __func__, __LINE__);
		mutex_unlock(&private->commit.lock);
	}
	return ret;
}

static void mtk_esd_timer_do(struct timer_list *esd_timer)
{
	struct mtk_drm_esd_ctx *esd_ctx =
		container_of(esd_timer, struct mtk_drm_esd_ctx, esd_timer);

	if (READ_ONCE(esd_ctx->stopping) || !READ_ONCE(esd_ctx->chk_active))
		return;
	atomic_set(&esd_ctx->target_time, 1);
	wake_up_interruptible(&esd_ctx->check_task_wq);
}

static int mtk_drm_esd_check_worker_kthread(void *data)
{
	struct sched_param param = { .sched_priority = 87 };
	struct mtk_drm_esd_ctx *esd_ctx = data;
	long ret;

	if (!esd_ctx)
		return -EINVAL;
	sched_setscheduler(current, SCHED_RR, &param);

	while (!kthread_should_stop()) {
		/* Preserve the normal check period, but make stopping interruptible. */
		ret = wait_event_interruptible_timeout(esd_ctx->check_task_wq,
			kthread_should_stop() || READ_ONCE(esd_ctx->stopping),
			msecs_to_jiffies(ESD_CHECK_PERIOD));
		if (kthread_should_stop() || READ_ONCE(esd_ctx->stopping))
			break;
		if (ret < 0 || !READ_ONCE(esd_ctx->chk_en))
			continue;

		mod_timer(&esd_ctx->esd_timer, jiffies + HZ);
		ret = wait_event_interruptible(esd_ctx->check_task_wq,
			kthread_should_stop() || READ_ONCE(esd_ctx->stopping) ||
			(atomic_read(&esd_ctx->check_wakeup) &&
			 (atomic_read(&esd_ctx->target_time) ||
			  READ_ONCE(esd_ctx->chk_mode) == READ_EINT)));
		timer_delete_sync(&esd_ctx->esd_timer);
		if (kthread_should_stop() || READ_ONCE(esd_ctx->stopping))
			break;
		if (ret < 0)
			continue;
		ret = mtk_drm_esd_testing_process(esd_ctx, true);
		if (ret < 0)
			DDPPR_ERR("generic ESD check worker failed: %ld\n", ret);
	}
	return 0;
}

void mtk_disp_esd_check_switch(struct drm_crtc *crtc, bool enable)
{
	if (crtc)
		mtk_crtc_esd_check_request(crtc, MTK_ESD_CHECK_GENERIC, enable);
}

/* Only the common first-frame worker may activate the generic checker. */
void mtk_disp_esd_check_arm(struct drm_crtc *crtc, bool enable)
{
	struct mtk_drm_private *priv;
	struct mtk_drm_crtc *mtk_crtc;
	struct mtk_drm_esd_ctx *esd_ctx = NULL;
	struct mtk_ddp_comp *output_comp;

	if (!crtc || !crtc->dev)
		return;
	priv = crtc->dev->dev_private;
	mtk_crtc = to_mtk_crtc(crtc);
	if (enable && !mtk_drm_helper_get_opt(priv->helper_opt,
						 MTK_DRM_OPT_ESD_CHECK_RECOVERY))
		return;

	output_comp = mtk_ddp_comp_request_output(mtk_crtc);
	if (output_comp && mtk_ddp_comp_get_type(output_comp->id) == MTK_DSI) {
		struct mtk_dsi *dsi = container_of(output_comp, struct mtk_dsi, ddp_comp);

		esd_ctx = dsi->esd_ctx;
	}
	if (!esd_ctx)
		esd_ctx = mtk_crtc->esd_ctx;
	if (!esd_ctx || READ_ONCE(esd_ctx->stopping))
		return;

	mtk_crtc->esd_ctx = esd_ctx;
	if (enable)
		WRITE_ONCE(esd_ctx->crtc, crtc);
	WRITE_ONCE(esd_ctx->chk_active, enable);
	atomic_set(&esd_ctx->check_wakeup, enable);
	if (!enable) {
		atomic_set(&esd_ctx->target_time, 0);
		atomic_set(&esd_ctx->ext_te_event, 0);
	}
	wake_up_interruptible(&esd_ctx->check_task_wq);
	wake_up_interruptible(&esd_ctx->ext_te_wq);
}

static void mtk_disp_esd_chk_deinit(struct drm_crtc *crtc)
{
	struct mtk_drm_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_drm_esd_ctx *esd_ctx = mtk_crtc->esd_ctx;
	struct drm_crtc *owner;

	if (!esd_ctx)
		return;
	mtk_disp_esd_check_switch(crtc, false);
	WRITE_ONCE(esd_ctx->stopping, true);
	WRITE_ONCE(esd_ctx->chk_active, false);
	atomic_set(&esd_ctx->check_wakeup, 0);
	wake_up_interruptible(&esd_ctx->check_task_wq);
	wake_up_interruptible(&esd_ctx->ext_te_wq);
	kthread_stop(esd_ctx->disp_esd_chk_task);
	timer_shutdown_sync(&esd_ctx->esd_timer);
	/* The optional TE1 ISR also dereferences this context during a check. */
	owner = READ_ONCE(esd_ctx->crtc);
	if (owner) {
		struct dual_te *d_te = &to_mtk_crtc(owner)->d_te;

		atomic_set(&d_te->esd_te1_en, 0);
		if (d_te->en && d_te->te1 > 0)
			synchronize_irq(d_te->te1);
	}
	if (esd_ctx->eint_requested)
		free_irq(esd_ctx->eint_irq, esd_ctx);
	if (esd_ctx->dsi && esd_ctx->dsi->esd_ctx == esd_ctx)
		esd_ctx->dsi->esd_ctx = NULL;
	owner = READ_ONCE(esd_ctx->crtc);
	if (owner && to_mtk_crtc(owner)->esd_ctx == esd_ctx)
		to_mtk_crtc(owner)->esd_ctx = NULL;
	mtk_crtc->esd_ctx = NULL;
	WRITE_ONCE(esd_ctx->crtc, NULL);
	kfree(esd_ctx);
}

static void mtk_disp_esd_chk_init(struct drm_crtc *crtc)
{
	struct mtk_drm_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_panel_ext *panel_ext = mtk_crtc->panel_ext;
	struct mtk_drm_esd_ctx *esd_ctx;
	struct mtk_ddp_comp *output_comp;
	struct mtk_dsi *dsi;
	int ret;

	if (!(panel_ext && panel_ext->params) ||
	    !_lcm_need_esd_check(panel_ext) || mtk_crtc->esd_ctx)
		return;
	output_comp = mtk_ddp_comp_request_output(mtk_crtc);
	if (!output_comp || mtk_ddp_comp_get_type(output_comp->id) != MTK_DSI)
		return;
	dsi = container_of(output_comp, struct mtk_dsi, ddp_comp);
	if (dsi->esd_ctx)
		return;

	esd_ctx = kzalloc(sizeof(*esd_ctx), GFP_KERNEL);
	if (!esd_ctx)
		return;
	esd_ctx->eint_irq = -1;
	esd_ctx->chk_en = 1;
	esd_ctx->crtc = crtc;
	esd_ctx->dsi = dsi;
	init_waitqueue_head(&esd_ctx->check_task_wq);
	init_waitqueue_head(&esd_ctx->ext_te_wq);
	atomic_set(&esd_ctx->check_wakeup, 0);
	atomic_set(&esd_ctx->ext_te_event, 0);
	atomic_set(&esd_ctx->target_time, 0);
	timer_setup(&esd_ctx->esd_timer, mtk_esd_timer_do, 0);
	esd_ctx->chk_mode = panel_ext->params->cust_esd_check == 1 ?
			    READ_LCM : READ_EINT;
	mtk_crtc->esd_ctx = esd_ctx;
	dsi->esd_ctx = esd_ctx;

	ret = mtk_drm_request_eint(crtc);
	if (ret)
		goto free_ctx;
	esd_ctx->disp_esd_chk_task = kthread_create(
		mtk_drm_esd_check_worker_kthread, esd_ctx, "disp_echk");
	if (IS_ERR(esd_ctx->disp_esd_chk_task)) {
		ret = PTR_ERR(esd_ctx->disp_esd_chk_task);
		free_irq(esd_ctx->eint_irq, esd_ctx);
		goto free_ctx;
	}
	wake_up_process(esd_ctx->disp_esd_chk_task);
	return;

free_ctx:
	DDPPR_ERR("failed to initialize generic ESD check: %d\n", ret);
	timer_shutdown_sync(&esd_ctx->esd_timer);
	dsi->esd_ctx = NULL;
	mtk_crtc->esd_ctx = NULL;
	kfree(esd_ctx);
}

void mtk_disp_chk_recover_deinit(struct drm_crtc *crtc)
{
	if (crtc)
		mtk_disp_esd_chk_deinit(crtc);
}

void mtk_disp_chk_recover_init(struct drm_crtc *crtc)
{
	struct mtk_drm_private *priv = crtc->dev->dev_private;
	struct mtk_drm_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_ddp_comp *output_comp;

	output_comp = (mtk_crtc) ? mtk_ddp_comp_request_output(mtk_crtc) : NULL;
	/* only support ESD check for DSI output interface */
	if (mtk_drm_helper_get_opt(priv->helper_opt, MTK_DRM_OPT_ESD_CHECK_RECOVERY) &&
			output_comp && mtk_ddp_comp_get_type(output_comp->id) == MTK_DSI)
		mtk_disp_esd_chk_init(crtc);
}
