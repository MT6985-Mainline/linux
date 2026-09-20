#!/usr/bin/env python3
# Round 109: find the exact step that turns the display block off.
#
# What we know from the ring: the block is ALIVE at the end of mtk_drm_kms_init
#     COROT[kms_init_done] OVL0 EN=0x00e00001 ... DSIBUF CON1=0x0ac90320
# and DARK at the next crtc_enable
#     COROT[crtc_enable_start] ... every register 0x00000000 ...
#     COROT-DSI[poweron+] CON=0x00000000 ... -> asynchronous SError
#
# Between those two points, in OUR mtk_drm_bind, there are exactly three calls:
#
#     ret = mtk_drm_kms_init(drm);
#     ret = drm_dev_register(drm, 0);
#     drm_client_setup(drm, NULL);        <- we have this, the vendor does NOT
#     mtk_layering_rule_init(drm);
#
# drm_client_setup() is the mainline way to bring up fbdev/simpledrm, and it
# performs an immediate atomic modeset - i.e. a crtc_enable at bind time.  The
# vendor's mtk_drm_bind has no such call: on Android the first commit comes from
# userspace, long after the vendor's own power hand-off has settled.  So the
# mode set our port performs at bind time is one the vendor never performs, and
# it lands inside the window the vendor's comment describes:
#
#     "we power on mtcmos at the beginning of the display initialization.  We
#      power off mtcmos at the end of the display initialization.  Here we only
#      decrease ref count, the power will hold on."
#
# This round does not change behaviour.  It prints one line at each of those
# points - four display registers plus the runtime-PM state - so the next round
# can be aimed at the call that actually does it instead of at a guess.
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
if "COROT-SNAP" in s:
    print("mtk_drm_drv.c: already patched")
    print("OK")
    sys.exit(0)

HELPER = """/*
 * COROT r109: is the display block alive right now?
 *
 * One line per checkpoint: the mutex, an OVL, the DSC and the DSI, plus the
 * runtime-PM state of the display's own device.  Process context only - the
 * mappings are made once, on the first call, and never from an interrupt (see
 * r103: ioremap() in an IRQ handler is a kernel BUG on this tree).
 */
static void corot_snap(struct device *dev, const char *tag)
{
	static void __iomem *mtx, *ovl, *dsc, *dsi;

	if (!mtx) {
		mtx = ioremap(0x14001000, 0x1000);
		ovl = ioremap(0x14402000, 0x1000);
		dsc = ioremap(0x1400c000, 0x1000);
		dsi = ioremap(0x1400d000, 0x1000);
	}

	pr_err("COROT-SNAP[%-22s] mtx_en=0x%08x ovl_en=0x%08x dsc_con=0x%08x dsi_start=0x%08x dsi_sta=0x%08x suspended=%d\\n",
	       tag,
	       mtx ? readl(mtx + 0x00) : 0,
	       ovl ? readl(ovl + 0x00) : 0,
	       dsc ? readl(dsc + 0x00) : 0,
	       dsi ? readl(dsi + 0x00) : 0,
	       dsi ? readl(dsi + 0x0c) : 0,
	       dev ? pm_runtime_status_suspended(dev) : -1);
}

static int mtk_drm_bind(struct device *dev)
{"""
old = "static int mtk_drm_bind(struct device *dev)\n{"
if s.count(old) != 1:
    die("mtk_drm_bind anchor %d" % s.count(old))
s = s.replace(old, HELPER, 1)
print("mtk_drm_drv.c: added corot_snap()")

# ---- the four checkpoints in mtk_drm_bind
pairs = [
    ("\tdrm->dev_private = private;\n",
     "\tcorot_snap(private->mmsys_dev ? private->mmsys_dev : dev, \"bind-in\");\n"),
    ("\tret = drm_dev_register(drm, 0);\n",
     "\tcorot_snap(private->mmsys_dev ? private->mmsys_dev : dev, \"after-kms-init\");\n"),
    ("\tdrm_client_setup(drm, NULL);\n",
     "\tcorot_snap(private->mmsys_dev ? private->mmsys_dev : dev, \"after-drm-dev-register\");\n"),
    ("\tmtk_layering_rule_init(drm);\n",
     "\tcorot_snap(private->mmsys_dev ? private->mmsys_dev : dev, \"after-client-setup\");\n"),
]
for anchor, before in pairs:
    if s.count(anchor) != 1:
        die("anchor %r count %d" % (anchor.strip(), s.count(anchor)))
    s = s.replace(anchor, before + anchor, 1)
    print("  checkpoint before: %s" % anchor.strip()[:50])

# (no bind-out checkpoint: "after-client-setup" already sits at the end of the
#  window that matters, and the DDPINFO tail anchor is not unique in this file)

wr(DRV, s)
print("OK")
