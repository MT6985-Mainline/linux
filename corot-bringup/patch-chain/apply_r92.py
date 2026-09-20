#!/usr/bin/env python3
# Round 92: port the SMI / LARB / IOMMU tree and hand the display back to the
# vendor's real CMDQ stack.
#
# The GCE failure in r86 was not a bandwidth problem.  It was:
#
#   XAGA-GCE: thr0 EN=0x1 STATUS=0x00000002 PC=0x68801000 IRQ=0x00000010
#   [cmdq][err] pc:0x68801000 end:0x688011a8 err:-22 thread:0      <- error IRQ
#   [CMDQ] error irq buffer 0: pa:0x68801000 iova:0x0000000000000000
#   [cmdq][err] cannot get dev domain @cmdq_thread_irq_handler,1438
#   [cmdq] failed to get mediatek,smi / failed to find smi node
#
# i.e. a DMA engine with no IOMMU domain and no SMI, and an SMI hang check that
# had nothing to check.  This round supplies all of it:
#
#   1. the SMI/LARB/IOMMU device tree (607 lines, generated from the vendor DTS
#      by .zcode/gen_smi.py - 3 commons, 14 sub-commons, 36 larbs, 2 IOMMUs);
#   2. mediatek,mt6985-smi-larb / -smi-common / -smi-sub-common in mtk-smi.c;
#   3. include/dt-bindings/memory/mt6985-larb-port.h, where M4U_PORT_L39_GCE_DM
#      lives;
#   4. mediatek,smi + iommus on the GCE node - and its clocks switched from the
#      clk26m stubs to the real CLK_MMINFRA_GCE_D / CLK_MMINFRA_GCE_26M, because
#      devm_clk_get(dev,"gce") was enabling a 26 MHz stub, so the GCE's own gate
#      was never opened;
#   5. the build flipped back to the vendor CMDQ stack (MBOX_EXT=y, no
#      -DDRM_CMDQ_DISABLE, no mtk_cmdq_dummy.o, mboxes restored) - without it
#      nothing ever calls into the GCE at all.
import io, os, re, shutil, sys

K = "/home/mytiantian/linux-corot"
V = "/home/mytiantian/linux-corot-t-oss"
DTS = K + "/arch/arm64/boot/dts/mediatek/mt6985-xiaomi-corot.dts"
BLOCK = "/home/mytiantian/corot-work/smi-block.dtsi"
SMI = K + "/drivers/memory/mtk-smi.c"
MK = K + "/drivers/gpu/drm/mediatek/mediatek_v2/Makefile"
CFG = K + "/.config"
HDR_SRC = V + "/include/dt-bindings/memory/mt6985-larb-port.h"
HDR_DST = K + "/include/dt-bindings/memory/mt6985-larb-port.h"


def die(m):
    print("FAIL: " + m)
    sys.exit(1)


def rd(p):
    return io.open(p, encoding="utf-8").read()


def wr(p, s):
    io.open(p, "w", encoding="utf-8").write(s)
    os.utime(p, None)


# ------------------------------------------------- 1. the larb port binding header
if not os.path.exists(HDR_SRC):
    die("vendor mt6985-larb-port.h not found")
shutil.copy2(HDR_SRC, HDR_DST)
os.utime(HDR_DST, None)
print("installed include/dt-bindings/memory/mt6985-larb-port.h (%d bytes)"
      % os.path.getsize(HDR_DST))
for want in ("M4U_PORT_L39_GCE_DM", "M4U_PORT_L0_DISP_OVL0_2L_RDMA0"):
    ok = want in rd(HDR_DST)
    print("   %-30s %s" % (want, "present" if ok else "MISSING"))
    if not ok and want.startswith("M4U_PORT_L39"):
        die("the GCE port id is missing from the header")

# 1b. the vendor port bindings need MTK_M4U_PORT_ID(), which upstream's shared
# helper header does not define.  Add it in a guarded, purely additive way:
# several upstream boards (mt8183/8186/8188/8195) already include that file, so
# it must not be overwritten and no existing macro may be redefined.
MP = K + "/include/dt-bindings/memory/mtk-memory-port.h"
mp = rd(MP)
if "MTK_M4U_PORT_ID" not in mp:
    shutil.copy2(MP, MP + ".pre-r92")
    mp = mp.replace("#define MTK_IFAIOMMU_PERI_ID(port)",
        """/*
 * COROT r92: the MT6985 port bindings express an id as
 * tab[21:20] | dom[19:16] | larb[10:5] | port[4:0].  Upstream only provides
 * MTK_M4U_ID(); add the tab/dom form alongside it.  Additive on purpose - this
 * header is shared with the mt8183/8186/8188/8195 device trees, so nothing
 * existing is redefined or removed.
 */
#define MTK_M4U_DOM_NR_MAX		16
#define MTK_M4U_TAB_NR_MAX		3
#define TAB_ID				0

#define MTK_M4U_PORT_ID(tab, dom, larb, port)	(((tab & 0x3) << 20) | ((dom & 0xf) << 16) |\\
						 ((larb & 0x3f) << 5) | (port & 0x1f))
#define MTK_M4U_DOM_ID(dom, larb, port)		MTK_M4U_PORT_ID(TAB_ID, dom, larb, port)

#define MTK_IFAIOMMU_PERI_ID(port)""", 1)
    if "MTK_M4U_PORT_ID" not in mp:
        die("could not add MTK_M4U_PORT_ID")
    wr(MP, mp)
    print("mtk-memory-port.h: added MTK_M4U_PORT_ID / TAB_ID / MTK_M4U_DOM_ID")
else:
    print("mtk-memory-port.h: MTK_M4U_PORT_ID already present")

# ------------------------------------------------------------------- 2. the DTS
s = rd(DTS)
shutil.copy2(DTS, DTS + ".pre-r92")

# 2a. include the port bindings
if "mt6985-larb-port.h" not in s:
    m = re.search(r"^#include <dt-bindings/[^\n]*\n", s, re.M)
    if not m:
        die("no #include line found in the dts")
    s = s[:m.end()] + "#include <dt-bindings/memory/mt6985-larb-port.h>\n" + s[m.end():]
    print("added #include <dt-bindings/memory/mt6985-larb-port.h>")

# 2b. GCE clock fix + smi + iommus
old = """		clocks = <&clk26m>, <&clk26m>;
		clock-names = "gce", "gce-timer";"""
if s.count(old) != 1:
    die("gce clocks anchor %d" % s.count(old))
s = s.replace(old, """		/*
		 * COROT r92: the vendor's real GCE clocks, not clk26m stubs.
		 * devm_clk_get(dev, "gce") was enabling a 26 MHz stub, so the
		 * GCE's own clock gate was never opened - which is consistent with
		 * it accepting a packet and never executing it.
		 */
		clocks = <&mminfra_clk CLK_MMINFRA_GCE_D>,
			 <&mminfra_clk CLK_MMINFRA_GCE_26M>;
		clock-names = "gce", "gce-timer";
		/* COROT r92: the SMI the GCE fetches its command buffer through,
		 * and the IOMMU port it is mapped on. */
		mediatek,smi = <&smi_mdp_2x1_subcommon>;
		iommus = <&mdp_iommu M4U_PORT_L39_GCE_DM>;""", 1)
print("gce node: real mminfra clocks + mediatek,smi + iommus")

# 2c. mboxes back (option B needs the GCE as the mailbox supplier)
if "mboxes = <&gce" not in s:
    anchor = '\t\tgce-client-names = "CLIENT_CFG0",'
    if s.count(anchor) != 1:
        die("gce-client-names anchor %d" % s.count(anchor))
    s = s.replace(anchor, """\t\t/* COROT r92: back with the real CMDQ stack, which does use it. */
\t\tmboxes = <&gce 0 0 CMDQ_THR_PRIO_4>,
\t\t\t <&gce 1 0 CMDQ_THR_PRIO_4>,
\t\t\t <&gce 2 0 CMDQ_THR_PRIO_4>,
\t\t\t <&gce 24 0 CMDQ_THR_PRIO_4>,
\t\t\t <&gce 3 CMDQ_NO_TIMEOUT CMDQ_THR_PRIO_2>,
\t\t\t <&gce 5 CMDQ_NO_TIMEOUT CMDQ_THR_PRIO_2>,
\t\t\t <&gce 25 CMDQ_NO_TIMEOUT CMDQ_THR_PRIO_2>,
\t\t\t <&gce 7 CMDQ_NO_TIMEOUT CMDQ_THR_PRIO_2>,
\t\t\t <&gce 4 0 CMDQ_THR_PRIO_4>,
\t\t\t <&gce 6 0 CMDQ_THR_PRIO_3>,
\t\t\t <&gce 22 0 CMDQ_THR_PRIO_1>;
""" + anchor, 1)
    print("dispsys: mboxes restored")
else:
    print("dispsys: mboxes already present")

# 2d. insert the SMI block before the closing brace of the root node
block = rd(BLOCK)
if "smi_disp_common" in s:
    die("the dts already contains the smi block")
end = s.rstrip()
if not end.endswith("};"):
    die("the dts does not end with the root node's closing brace")
s = end[:-2].rstrip("\n") + "\n\n" + block.rstrip("\n") + "\n};\n"
wr(DTS, s)
print("inserted the smi/iommu block (%d lines)" % len(block.split("\n")))

# ------------------------------------------------------------ 3. the smi driver
d = rd(SMI)
shutil.copy2(SMI, SMI + ".pre-r92")

# 3a. larb gen data
old = """static const struct of_device_id mtk_smi_larb_of_ids[] = {"""
if d.count(old) != 1:
    die("larb of_ids anchor %d" % d.count(old))
d = d.replace(old, """/*
 * COROT r92: MT6985.  Only config_port is provided: the vendor's own driver
 * carries per-port OSTD/bw tables we do not have, and the generic gen2 port
 * configuration is what makes a larb usable.  Documented as a deliberate
 * simplification, not an oversight.
 */
static const struct mtk_smi_larb_gen mtk_smi_larb_mt6985 = {
	.config_port = mtk_smi_larb_config_port_gen2_general,
};

static const struct of_device_id mtk_smi_larb_of_ids[] = {
	{.compatible = "mediatek,mt6985-smi-larb", .data = &mtk_smi_larb_mt6985},""", 1)

# 3b. common / sub-common plat data
old = """static const struct of_device_id mtk_smi_common_of_ids[] = {"""
if d.count(old) != 1:
    die("common of_ids anchor %d" % d.count(old))
d = d.replace(old, """/*
 * COROT r92: MT6985 commons.  bus_sel/init left at zero - the vendor programs
 * these for its own MM DVFS port mapping, and a zero bus_sel is a no-op rather
 * than a wrong value.  has_gals is set because the MT6985 nodes supply the
 * gals clocks and the driver sizes its clk_bulk_get by it.
 */
static const struct mtk_smi_common_plat mtk_smi_common_mt6985 = {
	.type     = MTK_SMI_GEN2,
	.has_gals = true,
};

static const struct mtk_smi_common_plat mtk_smi_sub_common_mt6985 = {
	.type     = MTK_SMI_GEN2_SUB_COMM,
	.has_gals = true,
};

static const struct of_device_id mtk_smi_common_of_ids[] = {
	{.compatible = "mediatek,mt6985-smi-common", .data = &mtk_smi_common_mt6985},
	{.compatible = "mediatek,mt6985-smi-sub-common", .data = &mtk_smi_sub_common_mt6985},""", 1)

wr(SMI, d)
print("mtk-smi.c: mt6985 larb gen + common/sub-common plat data + of_ids")

# --------------------------------------------------------- 4. back to option B
mk = rd(MK)
shutil.copy2(MK, MK + ".pre-r92")
mk = re.sub(r"^ccflags-y \+= -DDRM_CMDQ_DISABLE\n", "", mk, flags=re.M)
mk = re.sub(r"^mediatek-drm-y \+= mtk_cmdq_dummy\.o\n", "", mk, flags=re.M)
# only real make directives matter; Makefile.pre-B carries explanatory comments
# that still mention mtk_cmdq_dummy.o
live = [l for l in mk.split("\n")
        if not l.lstrip().startswith("#")
        and ("DRM_CMDQ_DISABLE" in l or "mtk_cmdq_dummy.o" in l)]
if live:
    die("the no-CMDQ directives survived: %s" % live)
wr(MK, mk)
print("Makefile: -DDRM_CMDQ_DISABLE and mtk_cmdq_dummy.o removed (option B)")

cfg = rd(CFG)
shutil.copy2(CFG, CFG + ".pre-r92")
cfg = re.sub(r"^# CONFIG_MTK_CMDQ_MBOX_EXT is not set\s*$",
             "CONFIG_MTK_CMDQ_MBOX_EXT=y", cfg, flags=re.M)
cfg = re.sub(r"^CONFIG_MTK_CMDQ_MBOX=y\s*$",
             "# CONFIG_MTK_CMDQ_MBOX is not set", cfg, flags=re.M)
cfg = re.sub(r"^CONFIG_MTK_CMDQ=m\s*$",
             "# CONFIG_MTK_CMDQ is not set", cfg, flags=re.M)
wr(CFG, cfg)
print(".config CMDQ lines now:")
for line in cfg.split("\n"):
    if "MTK_CMDQ" in line and "TEGRA" not in line:
        print("   " + line)

print("OK")
