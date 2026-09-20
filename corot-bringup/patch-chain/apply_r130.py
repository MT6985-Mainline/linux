#!/usr/bin/env python3
# Round 130: make the display bring-up OBSERVABLE by probing it after userspace.
#
# The whole evening's measurement problem in one sentence: mtk_drm_probe() and
# mtk_drm_kms_init() run in device_initcall time, ~0.35 s in, before /init
# exists - so a failure there is invisible to every user-space log channel
# (cust boot-log.txt), and the kernel-ring channel only reaches us when LK
# happens to dump it (it did for r126/r127's panics, it did not for r128/r129's
# hangs, which is why those two rounds produced nothing at all).
#
# Fix the channel, not the symptom: return -EPROBE_DEFER from mtk_drm_probe
# until system_state >= SYSTEM_RUNNING, which kernel_init() sets immediately
# before exec'ing /init.  The driver core then retries the probe from its
# deferred-probe workqueue, which runs concurrently with the initramfs - so the
# log-catcher is already streaming to cust before the display code does anything,
# and every breadcrumb (including the last one before a hang) is on flash.
#
# Nothing about the display logic changes: same DT, same power path, same
# breadcrumbs.  Only WHEN the probe runs.
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
if "COROT r130" in s:
    print("mtk_drm_drv.c: already patched")
    sys.exit(0)

# find the display probe entry point and put the gate at its very top
anchors = [
    "static int mtk_drm_probe(struct platform_device *pdev)\n{\n",
    "static int mtk_drm_probe(struct platform_device *pdev)\r\n{\r\n",
]
hit = None
for a in anchors:
    if s.count(a) == 1:
        hit = a
        break
if hit is None:
    die("mtk_drm_probe anchor not found (counts: %s)"
        % [s.count(a) for a in anchors])

gate = hit + """	/*
	 * COROT r130: run the whole display bring-up with a live log channel.
	 *
	 * In device_initcall time (~0.35 s) nothing user-space exists yet, so a
	 * hang here is invisible and the round produces no data at all.  Defer
	 * until kernel_init() has set SYSTEM_RUNNING and started /init: the
	 * initramfs log-catcher is then already writing cust/boot-log.txt, and
	 * every breadcrumb below - including the last one before a hang - lands
	 * on flash where Android can read it.
	 */
	if (system_state < SYSTEM_RUNNING) {
		pr_err("COROT r130 probe deferred: state=%d (waiting for userspace)\\n",
		       system_state);
		return -EPROBE_DEFER;
	}
	pr_err("COROT r130 probe running with userspace up (state=%d)\\n", system_state);

"""
s = s.replace(hit, gate, 1)
print("mtk_drm_probe: deferred until SYSTEM_RUNNING")

if "#include <linux/kernel.h>" not in s:
    a2 = "#include <linux/sched.h>\n"
    if a2 not in s:
        die("no include anchor for kernel.h")
    s = s.replace(a2, "#include <linux/kernel.h>\n" + a2, 1)
    print("added #include <linux/kernel.h>")

wr(DRV, s)
print("OK")
