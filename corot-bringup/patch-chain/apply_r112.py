#!/usr/bin/env python3
# Round 112: finish the init-power-on mechanism the port left half-done.
#
# The vendor's contract (mtk-smi.c):
#
#   probe (larb / common / smi-pd):
#       if (of_property_read_bool(dev->of_node, "init-power-on")) {
#               ret = pm_runtime_get_sync(dev);
#               init_power_on_dev[init_power_on_num++] = ...;    <- register it
#       }
#   end of mtk_drm_kms_init:
#       mtk_smi_init_power_off();                                <- release them all
#
# Our port kept the *get* and lost both the registration and the release:
#
#   - mtk_smi_larb_probe() still has the `init-power-on` block and calls
#     pm_runtime_get_sync(dev) - visible in drivers/memory/mtk-smi.c;
#   - but mtk-smi.c contains no `init_power_on_dev' / `init_power_on_num'
#     anywhere, so nothing is ever registered;
#   - and mtk_smi_init_power_off() is an empty function ("No-op in mainline").
#
# So every larb/common that carries `init-power-on' in the device tree takes a
# runtime-PM reference that nobody ever releases.  That is not just a leak: the
# resume path runs the larb's own port configuration
# (mtk_smi_larb_config_port_gen2_general), which is exactly the thing r96 showed
# destroys the bootloader's working display path - and the block that is
# supposed to hand the hardware back over at the end of DRM init never runs.
#
# This round restores the missing half, faithfully but using this tree's own
# structures (struct device * rather than the vendor's struct mtk_smi):
#
#   1. init_power_on_dev[] / init_power_on_num, and registration in the
#      `init-power-on' block that is already there;
#   2. the real mtk_smi_init_power_off() that puts them all back;
#   3. the call at the end of mtk_drm_kms_init that r111 commented out - and
#      r111 was an invalid experiment anyway (that function was a no-op, so
#      commenting it out changed nothing; the ring shows the r111 kernel never
#      even printed a marker, i.e. it never booted).
import io
import os
import sys

K = "/home/mytiantian/linux-corot"
SMI = K + "/drivers/memory/mtk-smi.c"
DRV = K + "/drivers/gpu/drm/mediatek/mediatek_v2/mtk_drm_drv.c"


def die(m):
    print("FAIL: " + m)
    sys.exit(1)


def rd(p):
    return io.open(p, encoding="utf-8").read()


def wr(p, s):
    io.open(p, "w", encoding="utf-8").write(s)
    os.utime(p, None)


# ------------------------------------------------------- 1. the real mechanism
s = rd(SMI)
if "COROT r112" in s:
    print("mtk-smi.c: already patched")
else:
    stub = """void mtk_smi_init_power_off(void)
{
	/* No-op in mainline: larbs are runtime-PM managed by the component
	 * framework, so there is nothing to release at init time.
	 */
}"""
    if s.count(stub) != 1:
        die("no-op stub anchor %d" % s.count(stub))
    s = s.replace(stub, """/*
 * COROT r112: the init-power-on mechanism, restored.
 *
 * The vendor keeps MTCMOS up from SMI probe until DRM init hands the hardware
 * over: every device whose node carries `init-power-on' takes one runtime-PM
 * reference at probe time, and mtk_smi_init_power_off() (called at the end of
 * mtk_drm_kms_init) gives them all back.  The port kept the get and replaced
 * this release with an empty function, so the references were never returned.
 *
 * The vendor stores struct mtk_smi *; this tree stores the device itself, which
 * is all pm_runtime_put_sync() needs.
 */
#define MAX_INIT_POWER_ON_DEV	32

static struct device *init_power_on_dev[MAX_INIT_POWER_ON_DEV];
static unsigned int init_power_on_num;

static void corot_init_power_on_hold(struct device *dev)
{
	if (!of_property_read_bool(dev->of_node, "init-power-on"))
		return;

	if (init_power_on_num >= MAX_INIT_POWER_ON_DEV) {
		dev_warn(dev, "init-power-on: table full\\n");
		return;
	}
	init_power_on_dev[init_power_on_num++] = dev;
	pr_err("COROT-INITPWR: hold on %pOF (%u)\\n", dev->of_node,
	       init_power_on_num);
}

void mtk_smi_init_power_off(void)
{
	unsigned int i;

	pr_err("COROT-INITPWR: releasing %u init-power-on reference(s)\\n",
	       init_power_on_num);
	for (i = 0; i < init_power_on_num; i++)
		pm_runtime_put_sync(init_power_on_dev[i]);
	init_power_on_num = 0;
}""", 1)
    print("mtk-smi.c: init_power_on_dev[] + real mtk_smi_init_power_off()")

    # register at the larb probe, in the block that already takes the reference
    anchor = """	if (of_property_read_bool(dev->of_node, "init-power-on")) {
		ret = pm_runtime_get_sync(dev);
		if (ret < 0) {
			dev_notice(dev, "Unable to enable SMI LARB%d. ret:%d\\n",
				larb->larbid, ret);
			pm_runtime_put_sync(dev);
		}
	}"""
    if s.count(anchor) != 1:
        die("larb init-power-on block anchor %d" % s.count(anchor))
    s = s.replace(anchor, """	if (of_property_read_bool(dev->of_node, "init-power-on")) {
		ret = pm_runtime_get_sync(dev);
		if (ret < 0) {
			dev_notice(dev, "Unable to enable SMI LARB%d. ret:%d\\n",
				larb->larbid, ret);
			pm_runtime_put_sync(dev);
		} else {
			/* COROT r112: register it so it can be released */
			corot_init_power_on_hold(dev);
		}
	}""", 1)
    print("mtk-smi.c: larb probe now registers the init-power-on hold")
    wr(SMI, s)

# --------------------------------------------------------- 2. restore the call
d = rd(DRV)
if "COROT r111" in d:
    old = """	/*
	 * COROT r111: TEST - this is the line that powers the display off.
	 *
	 * The snapshots say the block is alive on the line above and dead on the
	 * line after this call.  The comment above it states the contract this
	 * port does not satisfy: the SMI larb holds MTCMOS up during init and the
	 * hold is released "after display keeps MTCMOS by itself".  Nothing here
	 * ever took that second hold, so this release is the last one.
	 *
	 * NOT a fix - a one-line test of the identification.  The real fix is to
	 * give the display its own reference first, then release this one.
	 */
	/* mtk_smi_init_power_off(); */
"""
    if d.count(old) != 1:
        die("r111 block anchor %d" % d.count(old))
    d = d.replace(old, """	/*
	 * COROT r112: restored.  r111's test was invalid - the function was an
	 * empty stub at the time, so commenting it out could not change
	 * anything (and the ring shows that build never even printed a marker).
	 * Now that mtk_smi_init_power_off() really releases the init-power-on
	 * holds, this is the vendor's hand-off point again.
	 */
	mtk_smi_init_power_off();
""", 1)
    wr(DRV, d)
    print("mtk_drm_drv.c: mtk_smi_init_power_off() restored at the end of kms_init")
else:
    print("mtk_drm_drv.c: r111 block not present (call already live)")

print("OK")
