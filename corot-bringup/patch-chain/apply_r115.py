#!/usr/bin/env python3
# Round 115: the port short-circuited the display power-on, and the reason it had
# to is that the clock list was never written.
#
# Two independent blocks, both in our tree, both fatal:
#
# 1. drivers/gpu/drm/mediatek/mediatek_v2/mtk_drm_drv.c
#
#       void mtk_drm_top_clk_prepare_enable(struct drm_device *drm)
#       {
#               ...
#               if (priv && priv->data && priv->data->mmsys_id == MMSYS_MT6985) {
#                       DDPMSG("MT6985 bring-up: skip top-clock prepare until clocks are wired\n");
#                       return;                      <-- the whole display power-on
#               }
#
#       void mtk_drm_top_clk_disable_unprepare(struct drm_device *drm)
#       {
#               ...
#               if (priv && priv->data && priv->data->mmsys_id == MMSYS_MT6985)
#                       return;                      <-- and the matching release
#
#    So on MT6985 the display domain is never taken out of runtime suspend.  That
#    is exactly what COROT-SNAP has been reporting all along:
#        COROT-SNAP[bind-in] ... suspended=1        (and every checkpoint after it)
#    and it is why the second crtc_enable reads the whole dispsys back as zeros
#    and takes the asynchronous SError on the first access to the MIPI TX block.
#
# 2. The comment says "until clocks are wired", and it is true: dispsys_config
#    lists ONE clock
#        clocks = <&mmsys0_clk CLK_MM_CONFIG>;
#        clock-names = "mmsys0";
#    while the vendor lists FORTY-EIGHT, and mtk_drm_kms_init() reads them by
#    index:
#        of_property_read_u32(node, "clock-num", &priv->top_clk_num);   <- absent here
#        clk = of_clk_get(node, i);
#    Without `clock-num` the driver sets top_clk_num = -1 and returns, so even
#    without the short-circuit there would be no clocks to enable.
#
# The vendor's list is 48 clocks across the four syscons that all belong to
# clk-mt6985-mmsys.c in this tree already (93 CLK_MM ids, CONFIG_COMMON_CLK_MT6985_MMSYS
# is on):
#     dispsys_config_clk -> mmsys0_clk      (12)
#     dispsys1_config_clk -> mmsys1_clk     (12)
#     ovlsys_config_clk -> ovlsys_clk       (12)
#     ovlsys1_config_clk -> ovlsys1_clk     (12)
# and the ids are all present in include/dt-bindings/clock/mt6985-clk.h.
#
# This round copies that list verbatim (remapping only the phandle names) and
# removes the two short-circuits.  Nothing else changes.
import io
import os
import re
import sys

K = "/home/mytiantian/linux-corot"
DTS = K + "/arch/arm64/boot/dts/mediatek/mt6985-xiaomi-corot.dts"
DRV = K + "/drivers/gpu/drm/mediatek/mediatek_v2/mtk_drm_drv.c"
VDTS = "/home/mytiantian/linux-corot-t-oss/arch/arm64/boot/dts/mediatek/mt6985.dts"

REMAP = {
    "dispsys_config_clk": "mmsys0_clk",
    "dispsys1_config_clk": "mmsys1_clk",
    "ovlsys_config_clk": "ovlsys_clk",
    "ovlsys1_config_clk": "ovlsys1_clk",
}


def die(m):
    print("FAIL: " + m)
    sys.exit(1)


def rd(p):
    return io.open(p, encoding="utf-8").read()


def wr(p, s):
    io.open(p, "w", encoding="utf-8").write(s)
    os.utime(p, None)


# ---------------------------------------------- 1. build the 48-clock list
vs = rd(VDTS)
m = re.search(r"^\s*clocks\s*=\s*(.*?);\s*$", vs[vs.index("dispsys_config:"):], re.M | re.S)
if not m:
    die("vendor dispsys_config clocks not found")
pairs = re.findall(r"<&([a-z0-9_]+)\s+(CLK_[A-Z0-9_]+)>", m.group(1))
if len(pairs) != 48:
    die("expected 48 vendor clocks, parsed %d" % len(pairs))
print("vendor dispsys clocks: %d entries" % len(pairs))

# R115_ONLY_PH=dispsys_config_clk keeps just that group - the bisection knob for
# "which group of clocks makes the SoC stop before it can print anything".
only = os.environ.get("R115_ONLY_PH")
if only:
    keep = set(only.split(","))
    before = len(pairs)
    pairs = [p for p in pairs if p[0] in keep]
    print("R115_ONLY_PH=%s: keeping %d of %d clocks" % (only, len(pairs), before))

# R115_MAX=<n> keeps only the first n entries - used to test the clock-num path
# itself with n = 1, which is functionally identical to the node before r115.
maxn = os.environ.get("R115_MAX")
if maxn:
    pairs = pairs[:int(maxn)]
    print("R115_MAX=%s: keeping the first %d clock(s)" % (maxn, len(pairs)))

HEADER = K + "/include/dt-bindings/clock/mt6985-clk.h"
hdr = rd(HEADER)
entries = []
missing = []
for ph, cid in pairs:
    if ph not in REMAP:
        die("unmapped phandle %s" % ph)
    if not re.search(r"#define\s+%s\b" % cid, hdr):
        missing.append(cid)
    entries.append("<&%s %s>" % (REMAP[ph], cid))
if missing:
    die("ids missing from mt6985-clk.h: %s" % sorted(set(missing)))
print("all %d ids present in mt6985-clk.h; providers remapped to %s"
      % (len(entries), ", ".join(sorted(set(REMAP.values())))))

CLKBLOCK = "\t\t/*\n" \
           "\t\t * COROT r115: the vendor's full dispsys clock list.\n" \
           "\t\t *\n" \
           "\t\t * mtk_drm_kms_init() reads these by INDEX with of_clk_get(node, i)\n" \
           "\t\t * and sizes its array from the `clock-num' property, so the list has\n" \
           "\t\t * to match the vendor's exactly - it is what pulls the display domain\n" \
           "\t\t * out of runtime suspend.  This node used to list only\n" \
           "\t\t * <&mmsys0_clk CLK_MM_CONFIG>, which is why MT6985's top-clock\n" \
           "\t\t * prepare was short-circuited in the driver.\n" \
           "\t\t */\n" \
           "\t\tclock-num = <%d>;\n" % len(entries)
for i in range(0, len(entries), 3):
    CLKBLOCK += "\t\tclocks = " if i == 0 else "\t\t\t "
    CLKBLOCK += ",\n".join("" if i == 0 and False else "")  # placeholder, replaced below
    CLKBLOCK = CLKBLOCK  # noqa
# build the list cleanly
lines = []
for i in range(0, len(entries), 3):
    chunk = ", ".join(entries[i:i + 3])
    if i == 0:
        lines.append("\t\tclocks = " + chunk + ",")
    else:
        lines.append("\t\t\t " + chunk + ",")
lines[-1] = lines[-1].rstrip(",") + ";"
CLKBLOCK = ("\t\t/*\n"
            "\t\t * COROT r115: the vendor's full dispsys clock list.\n"
            "\t\t *\n"
            "\t\t * mtk_drm_kms_init() reads these by INDEX with of_clk_get(node, i)\n"
            "\t\t * and sizes its array from the `clock-num' property, so the list has\n"
            "\t\t * to match the vendor's exactly - this is what takes the display\n"
            "\t\t * domain out of runtime suspend.  The node used to list only\n"
            "\t\t * <&mmsys0_clk CLK_MM_CONFIG>, which is why MT6985's top-clock\n"
            "\t\t * prepare was short-circuited in the driver.\n"
            "\t\t */\n"
            "\t\tclock-num = <%d>;\n" % len(entries)) + "\n".join(lines) + "\n"

d = rd(DTS)
if "COROT r115" in d:
    print("dts: already patched")
else:
    if os.environ.get("R115_NUM_ONLY"):
        # absolute minimum: add `clock-num' and nothing else.  The clocks property
        # keeps the single <&mmsys0_clk CLK_MM_CONFIG> it always had, so the only
        # difference from the untouched tree is this one property.
        old = "\t\tclocks = <&mmsys0_clk CLK_MM_CONFIG>;\n"
        if d.count(old) != 1:
            die("dispsys clocks anchor %d" % d.count(old))
        d = d.replace(old, "\t\t/* COROT r115 (NUM_ONLY): only clock-num is added. */\n"
                           "\t\tclock-num = <1>;\n" + old, 1)
        wr(DTS, d)
        print("dts: ONLY clock-num = <1> added (clocks line untouched)")
        print("OK")
        sys.exit(0)
    old = "\t\tclocks = <&mmsys0_clk CLK_MM_CONFIG>;\n"
    if d.count(old) != 1:
        die("dispsys clocks anchor %d" % d.count(old))
    d = d.replace(old, CLKBLOCK, 1)
    wr(DTS, d)
    print("dts: dispsys_config now lists %d clocks + clock-num" % len(entries))

# ------------------------------------------- 2. remove the two short-circuits
# R115_DT_ONLY=1 makes this a bisection round: the device tree gets the vendor's
# 48-clock list and the driver keeps both short-circuits, so the only new thing
# the kernel does at boot is enable those clocks in mtk_drm_kms_init().
# R115_DT_ONLY=0 with the short-circuits removed is the full r115.
BISECT_HOLD = os.environ.get("R115_KEEP_SHORT", "")
if os.environ.get("R115_DT_ONLY"):
    print("R115_DT_ONLY: device tree only - driver short-circuits left in place")
    print("OK")
    sys.exit(0)

s = rd(DRV)
if "COROT r115" in s:
    print("mtk_drm_drv.c: already patched")
else:
    a = """	if (priv && priv->data && priv->data->mmsys_id == MMSYS_MT6985) {
		DDPMSG("MT6985 bring-up: skip top-clock prepare until clocks are wired\\n");
		return;
	}
"""
    if s.count(a) != 1:
        die("prepare_enable short-circuit anchor %d" % s.count(a))
    s = s.replace(a, """	/*
	 * COROT r115: the MT6985 short-circuit is gone.
	 *
	 * It said "skip top-clock prepare until clocks are wired", and it did
	 * exactly that - so on this SoC the display domain was never taken out of
	 * runtime suspend (COROT-SNAP reports suspended=1 at every checkpoint),
	 * the next crtc_enable read the whole dispsys back as zeros, and the first
	 * access to the MIPI TX block raised the asynchronous SError.
	 *
	 * The clocks are wired now: dispsys_config carries the vendor's full
	 * 48-entry list and clock-num = 48.
	 */
""", 1)
    print("mtk_drm_drv.c: removed the MT6985 short-circuit in prepare_enable")

    b = """	if (priv && priv->data && priv->data->mmsys_id == MMSYS_MT6985)
		return;
"""
    if s.count(b) != 1:
        die("disable_unprepare short-circuit anchor %d" % s.count(b))
    s = s.replace(b, """	/* COROT r115: the matching short-circuit is gone too - the release
	 * has to balance the get, or the domain is never dropped again. */
""", 1)
    print("mtk_drm_drv.c: removed the MT6985 short-circuit in disable_unprepare")

    # the vendor also resumes ovlsys / side ovlsys here; this tree's private
    # struct has no such members, so say so rather than pretend.
    note = """	pm_runtime_get_sync(priv->mmsys_dev);
	if (priv->side_mmsys_dev)
		pm_runtime_get_sync(priv->side_mmsys_dev);"""
    if s.count(note) >= 1:
        s = s.replace(note, note + """
	/* COROT r115 NOTE: the vendor also resumes priv->ovlsys_dev and
	 * priv->side_ovlsys_dev here.  Neither member exists in this tree's
	 * struct mtk_drm_private, so the ovlsys power domains are still not
	 * managed - worth porting next if the display comes up but stays dark. */""", 1)
        print("mtk_drm_drv.c: noted the missing ovlsys resume")

    wr(DRV, s)

print("OK")
