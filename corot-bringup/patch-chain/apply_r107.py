#!/usr/bin/env python3
# Round 107: give the ported DRM the device-tree description it never had.
#
# The whole symptom chain comes back to dispsys_config.  Comparing our node with
# the vendor's (arch/arm64/boot/dts/mediatek/mt6985.dts in linux-corot-t-oss)
# shows 17 missing properties, and two of them are decisive:
#
#   1. helper-name / helper-value.
#      mtk_drm_helper_init() (mtk_drm_helper.c:179) walks the DRIVER's option
#      table and looks each name up in the device tree by name; anything it does
#      not find is left at 0.  We have neither property, so *every* option is 0 -
#      including MTK_DRM_OPT_PRIM_DUAL_PIPE, which mtk_crtc_is_dual_pipe()
#      (mtk_drm_crtc.c:1243) requires:
#
#          if (crtc index == 0 &&
#              mtk_drm_helper_get_opt(priv->helper_opt, MTK_DRM_OPT_PRIM_DUAL_PIPE) &&
#              panel_ext->output_mode == MTK_PANEL_DSC_SINGLE_PORT &&
#              panel_ext->dsc_params.slice_mode == 1)
#                  return true;
#
#      Our panel driver already sets output_mode = MTK_PANEL_DSC_SINGLE_PORT, so
#      the only thing missing was the option.  With it 0, mtk_crtc_prepare_dual_pipe()
#      always takes the else branch, the log prints is_dual_pipe=0, and the panel's
#      *second* DSC pipe is never created - which is exactly the half-screen: the
#      part appears as a grey field with a blue dashed divider between the two
#      DSC pipes.  The vendor sets PRIM_DUAL_PIPE = 1.
#
#      The vendor's list is copied verbatim, except that the features whose nodes
#      this tree does not have yet are forced to 0 so nothing is enabled that
#      cannot work: MML_* (no mmlsys_config), MMQOS_SUPPORT (no mmqos),
#      MMDVFS_SUPPORT (no opp_table_disp / dvfsrc_vcore).
#
#   2. power-domains.  The vendor points dispsys_config at
#      MT6985_POWER_DOMAIN_DIS0_SHUTDOWN and lists the ovlsys / side-dispsys
#      domains in pd-others.  We have none, so nothing brings the display block up
#      and its registers read back as zeros - which is the option-B crash:
#          COROT-DSI[poweron+] CON=0x00000000 START=0x00000000 ...
#          Kernel panic - not syncing: Asynchronous SError Interrupt
#
# Also added, all of them plain data or phandles we already have:
#   mediatek,mailbox-gce = <&gce>   - the GCE the DRM is supposed to drive
#   iommus                          - the display IOMMU port of the master
#
# Deliberately NOT added yet, because they need nodes or clock IDs that are not
# in the tree: the 48 dispsys clocks (clock-num/condition-num must match the real
# list), interconnects/pre-define-bw (no mmqos node), mediatek,mml (no MML), and
# pd-others (names the side-dispsys components we have not ported).
import io
import os
import re
import sys

K = "/home/mytiantian/linux-corot"
DTS = K + "/arch/arm64/boot/dts/mediatek/mt6985-xiaomi-corot.dts"
VDTS = "/home/mytiantian/linux-corot-t-oss/arch/arm64/boot/dts/mediatek/mt6985.dts"

# options whose backing node this tree does not have yet
FORCE_ZERO = {
    "MTK_DRM_OPT_MMQOS_SUPPORT",
    "MTK_DRM_OPT_MMDVFS_SUPPORT",
    "MTK_DRM_OPT_MML_PRIMARY",
    "MTK_DRM_OPT_MML_SUPPORT_CMD_MODE",
    "MTK_DRM_OPT_MML_PQ",
    "MTK_DRM_OPT_MML_IR",
    "MTK_DRM_OPT_MML_IR",
    "MTK_DRM_OPT_VIRTUAL_DISP",
}


def die(m):
    print("FAIL: " + m)
    sys.exit(1)


def rd(p):
    return io.open(p, encoding="utf-8").read()


def wr(p, s):
    io.open(p, "w", encoding="utf-8").write(s)
    os.utime(p, None)


# ---------------------------------------------------------------- vendor table
vs = rd(VDTS)


def grab(prop, path):
    m = re.search(r"^\s*%s\s*=\s*(.*?);\s*$" % prop, path, re.M | re.S)
    if not m:
        die("vendor %s not found" % prop)
    return m.group(0)


names_src = grab("helper-name", vs)
values_src = grab("helper-value", vs)

names = re.findall(r'"([A-Z0-9_]+)"', names_src)
vals = [int(x) for x in re.findall(r"<\s*(\d+)\s*>", values_src)]
if len(names) != len(vals):
    die("vendor helper-name(%d) != helper-value(%d)" % (len(names), len(vals)))
print("vendor helper table: %d options, PRIM_DUAL_PIPE=%d USE_CMDQ=%d"
      % (len(names),
         vals[names.index("MTK_DRM_OPT_PRIM_DUAL_PIPE")],
         vals[names.index("MTK_DRM_OPT_USE_CMDQ")]))

zeroed = []
out_vals = []
for n, v in zip(names, vals):
    if n in FORCE_ZERO and v:
        out_vals.append(0)
        zeroed.append(n)
    else:
        out_vals.append(v)
print("  forced to 0 (node not ported yet): %s" % (", ".join(sorted(set(zeroed))) or "none"))

name_lines = "\n".join('\t\t\t"%s",' % n for n in names[:-1]) + '\n\t\t\t"%s";' % names[-1]
# the driver maps by NAME, the <n> comments are only for readability
val_lines = "\n".join('\t\t\t<%d>, /*%s*/' % (v, n) for n, v in zip(names, out_vals))
val_lines = val_lines.rsplit("\n", 1)[0] + "\n\t\t\t<%d>; /*%s*/" % (out_vals[-1], names[-1])

HELPER = """\t\t/*
\t\t * COROT r107: the vendor's DRM option table.  mtk_drm_helper_init()
\t\t * looks each name up here by name; anything missing stays 0, and
\t\t * MTK_DRM_OPT_PRIM_DUAL_PIPE = 0 means mtk_crtc_is_dual_pipe() is
\t\t * false, which means the panel's second DSC pipe is never created.
\t\t * That is the half-screen.
\t\t */
\t\thelper-name = %s
\t\thelper-value = %s
""" % (name_lines, val_lines)

# ------------------------------------------------------------------ our dts
s = rd(DTS)
if "COROT r107" in s:
    print("dts: already patched")
    print("OK")
    sys.exit(0)

anchor = '\t\tcompatible = "mediatek,mt6985-disp";\n'
if s.count(anchor) != 1:
    die("dispsys_config compatible anchor %d" % s.count(anchor))

# the scpsys label in our tree
m = re.search(r"^\s*([a-z0-9_]+):\s*power-controller@1c001000", s, re.M)
if not m:
    die("scpsys node not found")
scpsys = m.group(1)
print("scpsys label: %s" % scpsys)

# does the ported driver know the option names we care about?
hh = rd(K + "/drivers/gpu/drm/mediatek/mediatek_v2/mtk_drm_helper.h")
for opt in ("MTK_DRM_OPT_PRIM_DUAL_PIPE", "MTK_DRM_OPT_USE_CMDQ"):
    print("  driver knows %-32s %s" % (opt, "yes" if opt in hh else "NO"))

add = anchor + HELPER + """\t\t/*
\t\t * COROT r107: the display power domain.  Without it nothing powers the
\t\t * block and every register reads back 0 - the option-B SError.
\t\t */
\t\tpower-domains = <&%s MT6985_POWER_DOMAIN_DIS0_SHUTDOWN>;
\t\t/* COROT r107: the GCE the DRM is meant to drive, and the master's
\t\t * display IOMMU port. */
\t\tmediatek,mailbox-gce = <&gce>;
\t\tiommus = <&disp_iommu M4U_PORT_L0_DISP_OVL1_2L_RDMA1>;
""" % scpsys

s = s.replace(anchor, add, 1)

# make sure the port macro header is included
if "mt6985-larb-port.h" not in s:
    m2 = re.search(r"^#include <dt-bindings/[^\n]*\n", s, re.M)
    if not m2:
        die("no include line")
    s = s[:m2.end()] + "#include <dt-bindings/memory/mt6985-larb-port.h>\n" + s[m2.end():]
    print("added the larb-port include")

wr(DTS, s)
print("dts: helper table + power-domains + mailbox-gce + iommus added to dispsys_config")
print("OK")
