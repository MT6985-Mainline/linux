#!/usr/bin/env python3
# Round 103: get ioremap() out of the DSI interrupt handler.
#
# The r102 ring finally named the fault that has been shaping this whole
# bring-up since it was introduced:
#
#   [DISP][E][IRQ] DDP_COMPONENT_DSI0: buffer underrun
#   ------------[ cut here ]------------
#   kernel BUG at mm/vmalloc.c:3228!            <- BUG_ON(in_interrupt())
#    pc : __get_vm_area_node+0x14c/0x150
#    lr : __get_vm_area_caller+0x38/0x48
#    Call trace:
#     __get_vm_area_node
#     generic_ioremap_prot
#     __ioremap_prot
#     mtk_dsi_irq_status+0x588/0xbcc
#     __handle_irq_event_percpu -> handle_irq_event -> handle_fasteoi_irq
#
# and the only ioremap() reachable there is the COROT-UNDERRUN register dump we
# added to the underrun branch of mtk_dsi_irq_status() (mtk_dsi.c ~2278):
#
#     void __iomem *m0 = ioremap(0x14001000, 0x1000);
#     void __iomem *m1 = ioremap(0x14401000, 0x1000);
#     void __iomem *o  = ioremap(0x14402000, 0x1000);
#     void __iomem *dc = ioremap(0x1400c000, 0x1000);
#
# The vendor's own heavy work above it - mtk_drm_crtc_analysis(),
# mtk_drm_crtc_dump(), mtk_smi_dbg_hang_detect() - ran to completion first (the
# ratelimited DDPPR_ERR "buffer underrun" printed after them), so this block is
# where the abort lands.
#
# Why that mattered so much
# -------------------------
# A single buffer underrun therefore killed the kernel.  The response was
# corot_mask_display_irqs_early(): INTEN = 0, everywhere, forever - which also
# removed TE_RDY, and TE_RDY is the only thing that produces a frame boundary on
# a command-mode panel (mtk_dsi_irq_status -> mtk_crtc_vblank_irq).  Everything
# downstream of that follows: the frame had to be re-armed by hand every 16 ms
# by corot_ftrig_tick(), option B never completed its first commit, and the
# analysis kept landing on "the pipeline is too slow" when the real problem was
# that the interrupt path that paces it was switched off to avoid a bug we put
# there ourselves.
#
# The fix: map once, outside interrupt context, at probe time - ioremap() only
# builds a virtual mapping, it does not touch the device, so probe is a perfectly
# good place for it - and have the ISR read through the cached pointers.  If the
# mappings are not there yet the dump is skipped rather than attempted.
import io
import os
import re
import sys

K = "/home/mytiantian/linux-corot"
DSI = K + "/drivers/gpu/drm/mediatek/mediatek_v2/mtk_dsi.c"


def die(m):
    print("FAIL: " + m)
    sys.exit(1)


def rd(p):
    return io.open(p, encoding="utf-8").read()


def wr(p, s):
    io.open(p, "w", encoding="utf-8").write(s)
    os.utime(p, None)


s = rd(DSI)
if "COROT r103" in s:
    print("mtk_dsi.c: already patched")
    print("OK")
    sys.exit(0)

# ---------------------------------------------------- 1. the cached mappings
anchor = "static irqreturn_t mtk_dsi_irq_status(int irq, void *dev_id)\n{"
if s.count(anchor) != 1:
    die("mtk_dsi_irq_status anchor %d" % s.count(anchor))
s = s.replace(anchor, """/*
 * COROT r103: the register blocks this file's interrupt handler wants to read
 * must be mapped BEFORE the handler runs.
 *
 * ioremap() cannot be called from interrupt context - __get_vm_area_node() has
 * BUG_ON(in_interrupt()) - and calling it from the DSI underrun branch is
 * exactly what turned "the panel underran once" into
 *     kernel BUG at mm/vmalloc.c:3228
 * and, in turn, into the blanket INTEN = 0 mask that killed TE_RDY.
 *
 * ioremap only creates a virtual mapping; it does not touch the hardware, so
 * doing it at probe time is safe and the handler just uses the pointers.
 */
static void __iomem *corot_irq_mtx0;
static void __iomem *corot_irq_mtx1;
static void __iomem *corot_irq_ovl;
static void __iomem *corot_irq_dsc;

static void corot_irq_maps_init(void)
{
	if (!corot_irq_mtx0)
		corot_irq_mtx0 = ioremap(0x14001000, 0x1000);
	if (!corot_irq_mtx1)
		corot_irq_mtx1 = ioremap(0x14401000, 0x1000);
	if (!corot_irq_ovl)
		corot_irq_ovl = ioremap(0x14402000, 0x1000);
	if (!corot_irq_dsc)
		corot_irq_dsc = ioremap(0x1400c000, 0x1000);

	pr_err("COROT-IRQMAP mtx0=%p mtx1=%p ovl=%p dsc=%p\\n",
	       corot_irq_mtx0, corot_irq_mtx1, corot_irq_ovl, corot_irq_dsc);
}

""" + anchor, 1)

# ---------------------------------------------------- 2. the ISR uses them
old = """			{
				static int corot_underrun_n;

				if (corot_underrun_n < 4) {
					void __iomem *m0 = ioremap(0x14001000, 0x1000);
					void __iomem *m1 = ioremap(0x14401000, 0x1000);
					void __iomem *o = ioremap(0x14402000, 0x1000);
					void __iomem *dc = ioremap(0x1400c000, 0x1000);

					corot_underrun_n++;"""
if s.count(old) != 1:
    die("underrun ioremap block anchor %d" % s.count(old))
s = s.replace(old, """			{
				static int corot_underrun_n;

				/*
				 * COROT r103: NO ioremap here.  This runs in hard
				 * interrupt context and ioremap() BUGs there; the
				 * mappings are made once by corot_irq_maps_init()
				 * at probe time.  If they are missing the dump is
				 * skipped - a missing line is cheap, a BUG is not.
				 */
				if (corot_underrun_n < 4 && corot_irq_mtx0) {
					void __iomem *m0 = corot_irq_mtx0;
					void __iomem *m1 = corot_irq_mtx1;
					void __iomem *o = corot_irq_ovl;
					void __iomem *dc = corot_irq_dsc;

					corot_underrun_n++;""", 1)

old2 = """						dc ? readl(dc + 0x00) : 0);
					if (m0)
						iounmap(m0);
					if (m1)
						iounmap(m1);
					if (o)
						iounmap(o);
					if (dc)
						iounmap(dc);
				}"""
if s.count(old2) != 1:
    die("underrun iounmap block anchor %d" % s.count(old2))
s = s.replace(old2, """						dc ? readl(dc + 0x00) : 0);
					/* COROT r103: no iounmap either - the
					 * mappings live for the life of the driver. */
				}""", 1)

# ---------------------------------------------------- 3. arm them at probe
# plain substring search: the marker line is rewritten every round, so match on
# the part of it that never changes.
i = s.find('pr_err("COROT-MARKER')
if i < 0:
    die("probe marker anchor not found")
j = s.rfind("\n", 0, i) + 1
s = s[:j] + """\t/*
\t * COROT r103: the mappings the DSI interrupt handler needs, made here
\t * because ioremap() is not legal in interrupt context.
\t */
\tcorot_irq_maps_init();
""" + s[j:]

wr(DSI, s)
print("mtk_dsi.c: IRQ-time ioremap/iounmap removed; mappings made once at probe")
print("OK")
