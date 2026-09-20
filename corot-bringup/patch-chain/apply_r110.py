#!/usr/bin/env python3
# Round 110: name the call inside mtk_drm_kms_init that powers the display down.
#
# r109's checkpoints narrowed it to one function:
#     COROT-SNAP[bind-in        ] ovl_en=0x00000006 dsc_con=0x00010089 dsi_start=0x00000001 ...  <- LK, alive
#     COROT-SNAP[after-kms-init ] ovl_en=0x00000000 dsc_con=0x00000000 dsi_start=0x00000000 ... <- dark
# and pm_runtime_status_suspended(mmsys_dev) is already 1 at bind-in, i.e. the
# kernel's PM state never matched the hardware the bootloader handed over - the
# very thing the vendor's comment in mtk_drm_kms_init is about.
#
# This round puts the same one-line snapshot at every step inside kms_init:
# entry, after component_bind_all, after each of the three crtc_create calls,
# after drm_vblank_init, before and after mtk_drm_first_enable, after the
# top-clock release r108 restored, and just before returning.  Whichever line
# shows the first transition to 0x00000000 is the call to fix.
import io
import os
import sys

K = "/home/mytiantian/linux-corot"
DRV = K + "/drivers/gpu/drm/mediatek/mediatek_v2/mtk_drm_drv.c"


def die(m):
    print("FAIL: " + m)
    sys.exit(1)


def rd(p):
    return io.open(p, encoding="utf-8").read()


def wr(p, s):
    io.open(p, "w", encoding="utf-8").write(s)
    os.utime(p, None)


s = rd(DRV)
if "COROT r110" in s:
    print("mtk_drm_drv.c: already patched")
    print("OK")
    sys.exit(0)

if "corot_snap" not in s:
    die("r109's corot_snap() is not there")

# forward declaration: corot_snap() is defined further down the file, next to
# mtk_drm_bind()
fwd = "static int mtk_drm_kms_init(struct drm_device *drm)\n{"
if s.count(fwd) != 1:
    die("mtk_drm_kms_init anchor %d" % s.count(fwd))
s = s.replace(fwd, """/* COROT r110: defined next to mtk_drm_bind() below */
static void corot_snap(struct device *dev, const char *tag);

""" + fwd, 1)
print("mtk_drm_drv.c: forward declaration added")

SNAP = '\tcorot_snap(drm->dev, "%s");\n'

# each anchor appears exactly once; the snapshot goes immediately after it
STEPS = [
    ("\tdrm = drm_dev_alloc(&mtk_drm_driver, drm->dev);", None),          # not in kms_init
    ("\tret = component_bind_all(drm->dev, drm);", "after-component-bind-all"),
    ("\tret = mtk_drm_crtc_create(drm, private->data->main_path_data);", "after-crtc0-create"),
    # NOTE: the ext_alter_path_data / ext_path_data creates sit in an
    # if/else WITHOUT braces, so a statement cannot be inserted after them
    # without changing the code.  They are not our display path anyway.
    ("\tret = drm_vblank_init(drm, MAX_CRTC);", "after-vblank-init"),
    ('\tpr_err("COROT-STAGE kms_init: BEFORE first_enable (LK handoff state)\\n");', "before-first-enable"),
    ("\tmtk_drm_first_enable(drm);", "after-first-enable"),
    ("\t\tmtk_drm_top_clk_disable_unprepare(drm);", "after-topclk-release"),
    ('\tpr_err("COROT-STAGE kms_init done, first_enable done (irqs masked)\\n");', "kms-init-return"),
]

placed = 0
for anchor, tag in STEPS:
    if tag is None:
        continue
    if s.count(anchor) != 1:
        print("  skip (count %d): %s" % (s.count(anchor), anchor.strip()[:56]))
        continue
    s = s.replace(anchor, anchor + "\n" + (SNAP % tag), 1)
    placed += 1
    print("  snapshot: %-24s after  %s" % (tag, anchor.strip()[:52]))

if placed < 6:
    die("only %d snapshots placed" % placed)

wr(DRV, s)
print("OK (%d snapshots inside kms_init)" % placed)
