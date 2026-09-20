#!/usr/bin/env python3
# Round 129: isolate WHERE the power-up hangs, and keep the reference held.
#
# Facts so far (all from the expdb ring, with kernel_sz proving the boot):
#   r125  take+release the top clock  -> "Asynchronous SError Interrupt" panic loop
#   r126  defer both, r108 release    -> same SError
#   r127  defer both, no release      -> same SError, and the ring shows the
#                                        dispsys registers going 0x6/0x10089/0x1
#                                        -> 0x0 in the kms_init-return window
#   r128  take (probe) + hold         -> NO SError any more, but the box hangs at
#                                        the Redmi logo (no auto-reboot)
#
# So holding the domain up fixes the SError, and the new failure is a hang -
# which points at the power-up call itself.  r128 took the reference in
# mtk_drm_get_top_clk(), i.e. during the DRM probe, ~0.35 s in.  This round moves
# ONLY that one: the probe-phase power-up is deferred again, while
# mtk_drm_top_clk_prepare_enable() (called from the first crtc_enable inside
# kms_init) powers the domain up for real and - because r108's release stays out
# - holds it.  Breadcrumbs bracket every step, so whichever side hangs leaves its
# last word in the ring.
#
# If this boots: the hang was the probe-phase power-up, and the display domain is
# now up with the reference held (which is what the vendor's comment describes).
# If it still hangs: the hang is in prepare_enable itself, and the breadcrumbs
# say whether it is pm_runtime_get_sync() or the clock gate.
import io
import os
import sys

DRV = "/home/mytiantian/linux-corot/drivers/gpu/drm/mediatek/mediatek_v2/mtk_drm_drv.c"


def die(m):
    print("FAIL: " + m)
    sys.exit(1)


def rd(p):
    return io.open(p, encoding="utf-8").read()


def wr(p, s):
    io.open(p, "w", encoding="utf-8").write(s)
    os.utime(p, None)


s = rd(DRV)
if "COROT r129" in s:
    print("mtk_drm_drv.c: already patched")
    sys.exit(0)

# ------------------------------------- 1. defer ONLY the probe-phase power-up
A1 = """	pm_runtime_get_sync(dev);
	if (priv->side_mmsys_dev)
		pm_runtime_get_sync(priv->side_mmsys_dev);
	for (i = 0; i < priv->top_clk_num; i++) {
"""
if s.count(A1) != 1:
    die("anchor A1 count %d" % s.count(A1))
B1 = """	/*
	 * COROT r129: the probe-phase power-up is the one that hung in r128
	 * (no SError, stuck at the boot logo).  Defer it to userspace; the real
	 * power-up now happens in mtk_drm_top_clk_prepare_enable() below, from
	 * the first crtc_enable, where the breadcrumbs can see it.
	 */
	if (system_state >= SYSTEM_RUNNING) {
		pm_runtime_get_sync(dev);
		if (priv->side_mmsys_dev)
			pm_runtime_get_sync(priv->side_mmsys_dev);
	} else {
		pr_err("COROT-TOPCLK get_top_clk power deferred: state=%d num=%d\\n",
		       system_state, priv->top_clk_num);
	}
	for (i = 0; i < priv->top_clk_num; i++) {
"""
s = s.replace(A1, B1, 1)
print("get_top_clk: probe-phase power-up deferred")

A2 = """		/* TODO: check display enable from lk */
		/* Because of align lk hw power status,
		 * we power on mtcmos at the beginning of
		 * the display initialization.
		 * We will power off mtcmos at the end of
		 * the display initialization.
		 */
		ret = clk_prepare_enable(priv->top_clk[i]);
"""
if s.count(A2) != 1:
    die("anchor A2 count %d" % s.count(A2))
B2 = """		/* COROT r129: the gate write needs the domain up; do it later. */
		if (system_state < SYSTEM_RUNNING) {
			pr_err("COROT-TOPCLK clock %d recorded, enable deferred\\n", i);
			continue;
		}
		ret = clk_prepare_enable(priv->top_clk[i]);
"""
s = s.replace(A2, B2, 1)
print("get_top_clk: gate enable deferred")

# ------------------------- 2. breadcrumbs around the real power-up (kms_init)
A3 = """	//set_swpm_disp_active(true);
	pm_runtime_get_sync(priv->mmsys_dev);
	if (priv->side_mmsys_dev)
		pm_runtime_get_sync(priv->side_mmsys_dev);
"""
if s.count(A3) != 1:
    die("anchor A3 count %d" % s.count(A3))
B3 = """	pr_err("COROT r129 prepare: pm_runtime_get_sync(mmsys) begin state=%d num=%d\\n",
	       system_state, priv->top_clk_num);
	//set_swpm_disp_active(true);
	pm_runtime_get_sync(priv->mmsys_dev);
	pr_err("COROT r129 prepare: mmsys runtime-resumed\\n");
	if (priv->side_mmsys_dev)
		pm_runtime_get_sync(priv->side_mmsys_dev);
"""
s = s.replace(A3, B3, 1)
print("prepare_enable: breadcrumbs around pm_runtime_get_sync")

A4 = """		if (IS_ERR(priv->top_clk[i])) {
			DDPPR_ERR("%s invalid %d clk\\n", __func__, i);
			return;
		}
		ret = clk_prepare_enable(priv->top_clk[i]);
"""
if s.count(A4) != 1:
    die("anchor A4 count %d" % s.count(A4))
B4 = """		if (IS_ERR(priv->top_clk[i])) {
			DDPPR_ERR("%s invalid %d clk\\n", __func__, i);
			return;
		}
		pr_err("COROT r129 prepare: enabling top clk %d\\n", i);
		ret = clk_prepare_enable(priv->top_clk[i]);
		pr_err("COROT r129 prepare: top clk %d ret=%d\\n", i, ret);
"""
s = s.replace(A4, B4, 1)
print("prepare_enable: breadcrumbs around the clock gate")

if "#include <linux/kernel.h>" not in s:
    anchor = "#include <linux/sched.h>\n"
    if anchor not in s:
        die("no include anchor for kernel.h")
    s = s.replace(anchor, "#include <linux/kernel.h>\n" + anchor, 1)
    print("added #include <linux/kernel.h>")

wr(DRV, s)
print("OK")
