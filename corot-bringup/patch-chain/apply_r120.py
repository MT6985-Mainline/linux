#!/usr/bin/env python3
# Round 120: put error-level breadcrumbs exactly where the boot goes dark.
#
# r115..r118 (the rounds that give dispsys_config the vendor's 48-clock list)
# produce no error-level output at all - the ring holds only pr_err, and it is
# empty for those boots, while the r119 control (= r113) logs normally and three
# LK jumps are recorded for it.  So those builds stop the machine somewhere
# before mtk_drm_probe's first breadcrumb, and the messages that would explain it
# are all pr_info, which the ring does not keep.
#
# The mechanism that fits: giving dispsys_config clock references turns the four
# mmsys/ovlsys syscons into *suppliers*, and a dependency cycle in the deferred
# probe graph makes deferred_probe_work spin forever - a hang, not a crash, with
# nothing at error level to show for it.
#
# This round adds pr_err at:
#   - clk_mt6985_mmsys_grp_probe(), entry and after mtk_clk_simple_probe(), so we
#     see whether the four clock providers come up at all;
#   - mtk_drm_get_top_clk(), entry, the clock-num result, every failed
#     of_clk_get(), and the count it ended up with.
# Everything else is r115 unchanged (48 clocks listed, both short-circuits gone).
import io
import os
import sys

K = "/home/mytiantian/linux-corot"
DRV = K + "/drivers/gpu/drm/mediatek/mediatek_v2/mtk_drm_drv.c"
CLK = K + "/drivers/clk/mediatek/clk-mt6985-mmsys.c"


def die(m):
    print("FAIL: " + m)
    sys.exit(1)


def rd(p):
    return io.open(p, encoding="utf-8").read()


def wr(p, s):
    io.open(p, "w", encoding="utf-8").write(s)
    os.utime(p, None)


# ------------------------------------------------- 1. the clock providers
s = rd(CLK)
if "COROT r120" in s:
    print("clk-mt6985-mmsys.c: already patched")
else:
    a = "static int clk_mt6985_mmsys_grp_probe(struct platform_device *pdev)\n{"
    if s.count(a) != 1:
        die("clk probe anchor %d" % s.count(a))
    s = s.replace(a, a + """
	/* COROT r120: error-level, because the ring only keeps pr_err and this is
	 * exactly the point after which the boot goes silent. */
	pr_err("COROT-CLKPROBE enter %pOF\\n", pdev->dev.of_node);
""", 1)
    b = """	r = mtk_clk_simple_probe(pdev);
	if (r)"""
    if s.count(b) != 1:
        die("simple_probe anchor %d" % s.count(b))
    s = s.replace(b, """	r = mtk_clk_simple_probe(pdev);
	pr_err("COROT-CLKPROBE done  %pOF ret=%d\\n", pdev->dev.of_node, r);
	if (r)""", 1)
    wr(CLK, s)
    print("clk-mt6985-mmsys.c: provider probe breadcrumbs added")

# ------------------------------------------------- 2. the clock-num reader
d = rd(DRV)
if "COROT r120" in d:
    print("mtk_drm_drv.c: already patched")
else:
    a = """	struct device *dev = priv->mmsys_dev;
	struct device_node *node = dev->of_node;
	struct clk *clk;
	int ret, i;

	if (disp_helper_get_stage() != DISP_HELPER_STAGE_NORMAL) {"""
    if d.count(a) != 1:
        die("get_top_clk anchor %d" % d.count(a))
    d = d.replace(a, """	struct device *dev = priv->mmsys_dev;
	struct device_node *node = dev->of_node;
	struct clk *clk;
	int ret, i;

	/* COROT r120: breadcrumbs down the path that the 48-clock build dies on */
	pr_err("COROT-TOPCLK enter node=%pOF\\n", node);

	if (disp_helper_get_stage() != DISP_HELPER_STAGE_NORMAL) {""", 1)

    b = """	if (of_property_read_u32(node, "clock-num", &priv->top_clk_num)) {
		priv->top_clk_num = -1;
		priv->top_clk = NULL;
		return;"""
    if d.count(b) != 1:
        die("clock-num anchor %d" % d.count(b))
    d = d.replace(b, """	if (of_property_read_u32(node, "clock-num", &priv->top_clk_num)) {
		pr_err("COROT-TOPCLK no clock-num property\\n");
		priv->top_clk_num = -1;
		priv->top_clk = NULL;
		return;""", 1)

    c = """		if (IS_ERR(clk)) {
			DDPPR_ERR("%s get %d clk failed\\n", __func__, i);
			priv->top_clk_num = -1;
			return;
		}"""
    if d.count(c) != 1:
        die("of_clk_get anchor %d" % d.count(c))
    d = d.replace(c, """		pr_err("COROT-TOPCLK of_clk_get(%d) = %p\\n", i, clk);
		if (IS_ERR(clk)) {
			pr_err("COROT-TOPCLK get %d clk FAILED %ld\\n", i, PTR_ERR(clk));
			DDPPR_ERR("%s get %d clk failed\\n", __func__, i);
			priv->top_clk_num = -1;
			return;
		}""", 1)

    e = """		 * the display initialization.
		 */
		ret = clk_prepare_enable(priv->top_clk[i]);
		if (ret)
			DDPPR_ERR("top clk prepare enable failed:%d\\n", i);"""
    if d.count(e) != 1:
        die("prepare_enable anchor %d" % d.count(e))
    d = d.replace(e, """		 * the display initialization.
		 */
		ret = clk_prepare_enable(priv->top_clk[i]);
		if (ret)
			DDPPR_ERR("top clk prepare enable failed:%d\\n", i);
		if (i < 3 || ret)
			pr_err("COROT-TOPCLK enabled[%d] ret=%d\\n", i, ret);""", 1)
    wr(DRV, d)
    print("mtk_drm_drv.c: mtk_drm_get_top_clk breadcrumbs added")

print("OK")
