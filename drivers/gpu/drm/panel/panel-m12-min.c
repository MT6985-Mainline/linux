// SPDX-License-Identifier: GPL-2.0
/*
 * m12-min: minimal probe driver for the corot M12 OLED (APA2600 DDIC).
 *
 * P1 bring-up: power, reset, attempt a DCS ID read, log everything.
 * Proves the DSI host + MIPI TX + panel link without the full DRM stack.
 * The full m12 panel port (init sequences, 60/120Hz, HBM...) lands later.
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/list.h>
#include <linux/regulator/consumer.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>
#include <drm/drm_modes.h>

#include "../mediatek/mediatek_v2/mtk_panel_ext.h"
#include "../mediatek/mediatek_v2/mtk_drm_graphics_base.h"

struct m12_min {
	struct mipi_dsi_device *dsi;
	struct drm_panel panel;
	struct gpio_desc *reset;
	struct gpio_desc *pm_en;
	struct gpio_desc *dvdd;
	struct gpio_desc *cam;
};

static inline struct m12_min *panel_to_m12_min(struct drm_panel *panel)
{
	return container_of(panel, struct m12_min, panel);
}

struct m12_cmd {
	u8 cmd;
	u8 len;
	u8 data[18];
};

static int m12_write_cmd(struct mipi_dsi_device *dsi,
			 const struct m12_cmd *c)
{
	u8 buf[19];
	int ret = -1, try;

	buf[0] = c->cmd;
	memcpy(&buf[1], c->data, c->len);
	for (try = 0; try < 3; try++) {
		if (c->cmd >= 0xb0)
			ret = mipi_dsi_generic_write(dsi, buf, c->len + 1);
		else
			ret = mipi_dsi_dcs_write_buffer(dsi, buf, c->len + 1);
		if (ret >= 0)
			break;
		msleep(20);
	}
	if (ret < 0)
		dev_err(&dsi->dev, "m12: cmd %02x failed %d\n", c->cmd, ret);
	return ret;
}

#define M12C(_cmd, ...) { .cmd = (_cmd), .len = sizeof((u8[]){__VA_ARGS__}), .data = {__VA_ARGS__} }
static const struct m12_cmd m12_init_cmds[] = {
	M12C(0xff, 0xaa,0x55,0xa5,0x81), M12C(0x6f,0x0f), M12C(0xfd,0x01),
	M12C(0x6f,0x10), M12C(0xfd,0xaf), M12C(0x6f,0x23), M12C(0xfd,0x01),
	M12C(0x6f,0x24), M12C(0xfd,0xbb), M12C(0xff,0xaa,0x55,0xa5,0x80),
	M12C(0x6f,0x1d), M12C(0xf2,0x05),
	M12C(0xf0,0x55,0xaa,0x52,0x08,0x01), M12C(0xc3,0x9c,0x01,0xaf,0xd0,0x22,0x02,0x00),
	M12C(0xf0,0x55,0xaa,0x52,0x08,0x00), M12C(0x6f,0x1c),
	M12C(0xc0,0x00,0x33,0x00,0x00,0x11), M12C(0x26,0x08),
	M12C(0xf0,0x55,0xaa,0x52,0x08,0x00), M12C(0x03,0x01),
	M12C(0x3b,0x00,0x14,0x00,0x14), M12C(0x90,0x11),
	M12C(0x91,0xab,0x28,0x00,0x0c,0xc2,0x00,0x02,0x32,0x01,0x31,0x00,0x08,0x08,0xbb,0x07,0x7b,0x10,0xf0),
	M12C(0x2c,0x00), M12C(0x51,0x00,0x00,0x00,0x00), M12C(0x53,0x20),
	M12C(0x35,0x00), M12C(0x2a,0x00,0x00,0x04,0xc3), M12C(0x2b,0x00,0x00,0x0a,0x97),
	M12C(0xf0,0x55,0xaa,0x52,0x08,0x07), M12C(0xc0,0x01), M12C(0x44,0x0a,0x70),
	M12C(0xf0,0x55,0xaa,0x52,0x08,0x00), M12C(0x6f,0x05), M12C(0xbe,0x88),
	M12C(0xff,0xaa,0x55,0xa5,0x80), M12C(0x6f,0x61), M12C(0xf3,0x80),
	M12C(0xf0,0x55,0xaa,0x52,0x08,0x08), M12C(0xc1,0x22,0x80,0x3b,0x01,0x81),
	M12C(0xc6,0x11),
	M12C(0xca,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0),
	M12C(0xcb,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0),
	M12C(0xcd,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0),
	M12C(0xd2,0x80,0x01,0x10,0x00,0x98,0x00,0x52), M12C(0x67,0x32,0x24,0x38,0x38),
};

/*
 * COROT r51: refresh-rate selection.
 *
 * The M12A does not follow the DSI timing - it runs at the rate its own FCON
 * register says.  The vendor driver's tables (mode_*hz_setting_gir_off) show
 * the sequence; the key writes are
 *     0x2F (FCON)    0x08 = 60 Hz, 0x04 = 90 Hz, 0x02 = 120 Hz, 0x03 = 144 Hz
 *     0xB3 (EM duty) 0x14 at 60 Hz, 0x38 at the others
 * Each table below re-selects Page0 first, because the sequence ends on Page8.
 */
static const struct m12_cmd m12_fps_60[] = {
	M12C(0xf0,0x55,0xaa,0x52,0x08,0x00), M12C(0x6f,0x44),
	M12C(0xb3,0x00,0x14,0x00,0x14,0x00,0x14,0x00,0x14,0x00,0x14,0x00,0x14,0x00,0x14),
	M12C(0x2f,0x08),
	M12C(0xf0,0x55,0xaa,0x52,0x08,0x08), M12C(0xe9,0x00,0x00,0x00,0x00), M12C(0x5f,0x01),
};

static const struct m12_cmd m12_fps_90[] = {
	M12C(0xf0,0x55,0xaa,0x52,0x08,0x00), M12C(0x6f,0x44),
	M12C(0xb3,0x00,0x38,0x00,0x38,0x00,0x38,0x00,0x38,0x00,0x38,0x00,0x38,0x00,0x38),
	M12C(0x2f,0x04),
	M12C(0xf0,0x55,0xaa,0x52,0x08,0x08), M12C(0xe9,0x00,0x00,0x00,0x00), M12C(0x5f,0x01),
};

static const struct m12_cmd m12_fps_120[] = {
	M12C(0xf0,0x55,0xaa,0x52,0x08,0x00), M12C(0x6f,0x44),
	M12C(0xb3,0x00,0x38,0x00,0x38,0x00,0x38,0x00,0x38,0x00,0x38,0x00,0x38,0x00,0x38),
	M12C(0x2f,0x02),
	M12C(0xf0,0x55,0xaa,0x52,0x08,0x08), M12C(0xe9,0x00,0x00,0x00,0x00), M12C(0x5f,0x01),
};

static const struct m12_cmd m12_fps_144[] = {
	M12C(0xf0,0x55,0xaa,0x52,0x08,0x00), M12C(0x6f,0x44),
	M12C(0xb3,0x00,0x38,0x00,0x38,0x00,0x38,0x00,0x38,0x00,0x38,0x00,0x38,0x00,0x38),
	M12C(0x2f,0x03),
	M12C(0xf0,0x55,0xaa,0x52,0x08,0x08), M12C(0xe9,0x00,0x00,0x00,0x00), M12C(0x5f,0x01),
};

static struct mipi_dsi_device *g_m12_dsi;

int corot_m12_apply_fps(unsigned int fps)
{
	const struct m12_cmd *tbl = NULL;
	unsigned int nr = 0, i;

	switch (fps) {
	case 60:
		tbl = m12_fps_60;
		nr = sizeof(m12_fps_60) / sizeof(m12_fps_60[0]);
		break;
	case 90:
		tbl = m12_fps_90;
		nr = sizeof(m12_fps_90) / sizeof(m12_fps_90[0]);
		break;
	case 120:
		tbl = m12_fps_120;
		nr = sizeof(m12_fps_120) / sizeof(m12_fps_120[0]);
		break;
	case 144:
		tbl = m12_fps_144;
		nr = sizeof(m12_fps_144) / sizeof(m12_fps_144[0]);
		break;
	default:
		return -22;
	}
	pr_err("COROT-FPS r80: request %u Hz, dsi=%p tbl=%p n=%u\n",
	       fps, g_m12_dsi, tbl, nr);
	if (!g_m12_dsi) {
		pr_err("COROT-FPS r80: FAILED - panel dsi not known yet\n");
		return -19;
	}

	for (i = 0; i < nr; i++)
		m12_write_cmd(g_m12_dsi, &tbl[i]);
	pr_err("COROT-FPS: panel FCON set to %u Hz\n", fps);
	return 0;
}
EXPORT_SYMBOL_GPL(corot_m12_apply_fps);

static int m12_min_unprepare(struct drm_panel *panel)
{
	/* Bring-up: leaving the panel powered keeps the LK handoff state
	 * alive across DRM disable/enable cycles. */
	return 0;
}

static int m12_min_prepare(struct drm_panel *panel)
{
	struct m12_min *ctx = panel_to_m12_min(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;
	u8 id[3] = {0, 0, 0};
	int rd;

	/* LK already ran the full M12 init; the command-mode OLED is
	 * self-refreshing the logo. Do not reset or re-init here — that
	 * loses the working state. Only verify liveness. */
	rd = mipi_dsi_dcs_read(dsi, 0x04, id, sizeof(id));
	dev_info(&dsi->dev, "m12: keep LK state; DCS 0x04 = %02x %02x %02x (ret=%d)\n",
		 id[0], id[1], id[2], rd);

	/* COROT r51: our mode says 60 Hz, so make the panel actually run at
	 * 60 Hz instead of inheriting whatever rate LK left. */
	corot_m12_apply_fps(60);
	return 0;
}
static int m12_min_enable(struct drm_panel *panel)
{
	struct m12_min *ctx = panel_to_m12_min(panel);
	return mipi_dsi_dcs_set_display_on(ctx->dsi);
}

static int m12_min_disable(struct drm_panel *panel)
{
	struct m12_min *ctx = panel_to_m12_min(panel);
	return mipi_dsi_dcs_set_display_off(ctx->dsi);
}

static int m12_min_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &(struct drm_display_mode){
		.clock = 273139, .hdisplay = 1220, .hsync_start = 1628,
		.hsync_end = 1630, .htotal = 1634, .vdisplay = 2712,
		.vsync_start = 2766, .vsync_end = 2776, .vtotal = 2786,
	});
	if (!mode)
		return -ENOMEM;
	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);
	connector->display_info.width_mm = 70;
	connector->display_info.height_mm = 155;
	return 1;
}

static const struct drm_panel_funcs m12_min_panel_funcs = {
	.prepare = m12_min_prepare,
	.unprepare = m12_min_unprepare,
	.enable = m12_min_enable,
	.disable = m12_min_disable,
	.get_modes = m12_min_get_modes,
};

/*
 * Panel extension required by the mediatek_v2 stack (dsi->ext->params is
 * dereferenced by the trigger/MMCLK/HRT paths). Values copied from the
 * 5.15 corot panel-m12-42-02-0a-dsc-cmd driver: 1220x2712, DSC v17,
 * slice 610x12, 10bpc, 8bpp compressed, data rate 1152 Mbps.
 */
#define M12_DSC_ENABLE              1
#define M12_DSC_VER                 17
#define M12_DSC_SLICE_MODE          1
#define M12_DSC_RGB_SWAP            0
#define M12_DSC_DSC_CFG             40
#define M12_DSC_RCT_ON              1
#define M12_DSC_BIT_PER_CHANNEL     10
#define M12_DSC_LINE_BUF_DEPTH      11
#define M12_DSC_BP_ENABLE           1
#define M12_DSC_BIT_PER_PIXEL       128
#define M12_DSC_SLICE_HEIGHT        12
#define M12_DSC_SLICE_WIDTH         610
#define M12_DSC_CHUNK_SIZE          610
#define M12_DSC_XMIT_DELAY          512
#define M12_DSC_DEC_DELAY           562
#define M12_DSC_SCALE_VALUE         32
#define M12_DSC_INCREMENT_INTERVAL  305
#define M12_DSC_DECREMENT_INTERVAL  8
#define M12_DSC_LINE_BPG_OFFSET     12
#define M12_DSC_NFL_BPG_OFFSET      2235
#define M12_DSC_SLICE_BPG_OFFSET    1915
#define M12_DSC_INITIAL_OFFSET      6144
#define M12_DSC_FINAL_OFFSET        4336
#define M12_DSC_FLATNESS_MINQP      7
#define M12_DSC_FLATNESS_MAXQP      16
#define M12_DSC_RC_MODEL_SIZE       8192
#define M12_DSC_RC_EDGE_FACTOR      6
#define M12_DSC_RC_QUANT_INCR_LIMIT0 15
#define M12_DSC_RC_QUANT_INCR_LIMIT1 15
#define M12_DSC_RC_TGT_OFFSET_HI    3
#define M12_DSC_RC_TGT_OFFSET_LO    3
#define M12_DATA_RATE               1152

static struct mtk_panel_params m12_ext_params = {
	.pll_clk = M12_DATA_RATE / 2,
	.data_rate = M12_DATA_RATE,
	.lcm_color_mode = MTK_DRM_COLOR_MODE_DISPLAY_P3,
	.physical_width_um = 69540,
	.physical_height_um = 154584,
	.output_mode = MTK_PANEL_DSC_SINGLE_PORT,
	.dsc_params = {
		.enable = M12_DSC_ENABLE,
		.ver = M12_DSC_VER,
		.slice_mode = M12_DSC_SLICE_MODE,
		.rgb_swap = M12_DSC_RGB_SWAP,
		.dsc_cfg = M12_DSC_DSC_CFG,
		.rct_on = M12_DSC_RCT_ON,
		.bit_per_channel = M12_DSC_BIT_PER_CHANNEL,
		.dsc_line_buf_depth = M12_DSC_LINE_BUF_DEPTH,
		.bp_enable = M12_DSC_BP_ENABLE,
		.bit_per_pixel = M12_DSC_BIT_PER_PIXEL,
		.pic_height = 2712,
		.pic_width = 1220,
		.slice_height = M12_DSC_SLICE_HEIGHT,
		.slice_width = M12_DSC_SLICE_WIDTH,
		.chunk_size = M12_DSC_CHUNK_SIZE,
		.xmit_delay = M12_DSC_XMIT_DELAY,
		.dec_delay = M12_DSC_DEC_DELAY,
		.scale_value = M12_DSC_SCALE_VALUE,
		.increment_interval = M12_DSC_INCREMENT_INTERVAL,
		.decrement_interval = M12_DSC_DECREMENT_INTERVAL,
		.line_bpg_offset = M12_DSC_LINE_BPG_OFFSET,
		.nfl_bpg_offset = M12_DSC_NFL_BPG_OFFSET,
		.slice_bpg_offset = M12_DSC_SLICE_BPG_OFFSET,
		.initial_offset = M12_DSC_INITIAL_OFFSET,
		.final_offset = M12_DSC_FINAL_OFFSET,
		.flatness_minqp = M12_DSC_FLATNESS_MINQP,
		.flatness_maxqp = M12_DSC_FLATNESS_MAXQP,
		.rc_model_size = M12_DSC_RC_MODEL_SIZE,
		.rc_edge_factor = M12_DSC_RC_EDGE_FACTOR,
		.rc_quant_incr_limit0 = M12_DSC_RC_QUANT_INCR_LIMIT0,
		.rc_quant_incr_limit1 = M12_DSC_RC_QUANT_INCR_LIMIT1,
		.rc_tgt_offset_hi = M12_DSC_RC_TGT_OFFSET_HI,
		.rc_tgt_offset_lo = M12_DSC_RC_TGT_OFFSET_LO,
		/*
		 * COROT: without this the DSC driver falls into its default PPS
		 * table (PPS12 = 0x01040880) instead of the M12A CSOT one.
		 * LK's known-good registers match 0x6d126102 exactly:
		 *   PPS12 = 0x01040900
		 *   PPS16..19 = 0xd9c7e1a7 0xd209d9e9 0xd22bd229 0x0000d271
		 */
		.dsc_config_panel_name = 0x6d126102,
	},
};

static struct mtk_panel_funcs m12_ext_funcs = {
};

static int m12_min_probe(struct mipi_dsi_device *dsi)
{
	struct m12_min *ctx;
	int ret;

	ctx = devm_kzalloc(&dsi->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dsi = dsi;
	g_m12_dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	/* Match the stock corot M12 command-mode link. Do not change the
	 * clock/EOT policy while taking over LK's initialized panel. */
	dsi->mode_flags = MIPI_DSI_MODE_LPM |
		MIPI_DSI_MODE_NO_EOT_PACKET |
		MIPI_DSI_CLOCK_NON_CONTINUOUS;

	/* GPIOD_ASIS is essential for LK handoff: probe must not pull the
	 * active-high panel rail low before DRM owns the panel. */
	ctx->pm_en = devm_gpiod_get_optional(&dsi->dev, "pm-enable",
					     GPIOD_ASIS);
	if (IS_ERR(ctx->pm_en))
		return PTR_ERR(ctx->pm_en);
	ctx->reset = devm_gpiod_get_optional(&dsi->dev, "reset",
					   GPIOD_ASIS);
	if (IS_ERR(ctx->reset))
		return PTR_ERR(ctx->reset);

	ctx->panel.funcs = &m12_min_panel_funcs;
	ctx->panel.dev = &dsi->dev;
	ctx->panel.connector_type = DRM_MODE_CONNECTOR_DSI;
	INIT_LIST_HEAD(&ctx->panel.list);

	/* LK has already initialized and enabled the M12 panel. Keep the DRM
	 * wrapper in that state so probe does not send a second command. */
	ctx->panel.prepared = true;
	ctx->panel.enabled = true;
	drm_panel_add(&ctx->panel);

	ret = mtk_panel_ext_create(&dsi->dev, &m12_ext_params,
				   &m12_ext_funcs, &ctx->panel);
	if (ret)
		pr_warn("m12-min: mtk_panel_ext_create failed %d\n", ret);

	pr_info("m12-min: probed on %pOF (lanes=4 rgb888 cmd-mode dsc-v17)\n",
		dsi->dev.of_node);

	return mipi_dsi_attach(dsi);
}

static void m12_min_remove(struct mipi_dsi_device *dsi)
{
	struct m12_min *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id m12_min_of_match[] = {
	{ .compatible = "xiaomi,m12-42-02-0a-min" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, m12_min_of_match);

static struct mipi_dsi_driver m12_min_driver = {
	.driver = {
		.name = "panel-m12-min",
		.of_match_table = m12_min_of_match,
	},
	.probe = m12_min_probe,
	.remove = m12_min_remove,
};
module_mipi_dsi_driver(m12_min_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Minimal M12 OLED probe driver for corot P1 bring-up");
