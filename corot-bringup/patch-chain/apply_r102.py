#!/usr/bin/env python3
# Round 102: stop the blanket interrupt mask from killing the DSI's own frame
# pacing.  Mask only the two underrun bits; leave TE_RDY alive.
#
# What r101 proved, and why this is the next single variable
# ---------------------------------------------------------
# r101 was supposed to be "r92 + one change".  It was not: the build script
# checked HEAD out first, and HEAD (b6e2d74) already carries r89..r98, so r101
# was really "option B + the whole r89..r98 delta + r100".  It crash-looped:
# four boots, all ending in
#     Kernel panic - not syncing: Asynchronous SError Interrupt
# with COROT-DSI[poweron+] reading all-zero DSI registers and
# COROT-MIPITX[poweron+] never printed, so the abort is taken on the DSI/MIPI-TX
# access itself.  It also exonerates the TE pinmux: r101 has DO_TE = False and
# dies in exactly the same place as r100, which had it on.  COROT-B100 never
# printed - no boot lived 6 s past master_add.
#
# Meanwhile the same expdb ring holds the *previous* round, and r100 got much
# further than r101: its last lines are
#     COROT-STAGE kms_init done, first_enable done (irqs masked)
#     COROT[kms_init_done] ... DSI START=0x00000001 ... DSIBUF CON1=0x0ac90320
# and then the boot stops - before "[drm] Initialized mediatek".  So in option B
# the first real commit never finishes.
#
# The one thing in this tree that is demonstrably wrong about interrupts
# ----------------------------------------------------------------
# mtk_dsi_set_interrupt_enable() arms INTEN = 0x00005004:
#
#     bit 12 BUFFER_UNDERRUN, bit 14 INP_UNFINISH, bit 2 TE_RDY
#
# and corot_mask_display_irqs_early() then writes INTEN = 0 - all three, plus
# the MUTEX enables.  That mask exists for a good reason (the underrun IRQ storm
# wedges the SoC in IRQ context), but it also silences TE_RDY, and TE_RDY is
# exactly what mtk_dsi_irq() turns into mtk_crtc_vblank_irq():
#
#     if ((status & TE_RDY_INT_FLAG) && mtk_crtc && ...)
#                     mtk_crtc_vblank_irq(&mtk_crtc->base);
#
# On a command-mode panel that is the frame pacing.  With bit 2 masked the DRM
# never sees a frame boundary: commits never complete, the MUTEX/CMDQ trigger
# never advances (which is the shape of "option B binds everything, zero cmdq
# errors, does not paint"), and on the CPU-direct side the frame has to be
# re-armed by hand every 16 ms - which is the emulator, not a display driver.
#
# So: keep masking the two underrun bits, which is the whole point of the mask,
# and leave TE_RDY alone.  One variable, one round, nothing else touched - the
# 60 Hz FCON request stays switched off behind r91's "if (0)".
import io, os, re, sys

K = "/home/mytiantian/linux-corot"
K2 = K + "/drivers/gpu/drm/mediatek/mediatek_v2"
DRV = K2 + "/mtk_drm_drv.c"
CRTC = K2 + "/mtk_drm_crtc.c"


def die(m):
    print("FAIL: " + m)
    sys.exit(1)


def rd(p):
    return io.open(p, encoding="utf-8").read()


def wr(p, s):
    io.open(p, "w", encoding="utf-8").write(s)
    os.utime(p, None)


NOTE = """	/*
	 * COROT r102: mask the underrun pair, NOT the whole register.
	 *
	 * DSI_INTEN bits: 2 = TE_RDY, 12 = BUFFER_UNDERRUN, 14 = INP_UNFINISH.
	 * mtk_dsi_set_interrupt_enable() arms all three (0x00005004).  Writing
	 * zero here also killed TE_RDY, and TE_RDY is what mtk_dsi_irq() turns
	 * into mtk_crtc_vblank_irq() - i.e. the frame pacing of a command-mode
	 * panel.  Without it the DRM never completes a frame.
	 *
	 * The storm this function exists to stop is the underrun pair, and those
	 * two are still masked.
	 */
"""

# ------------------------------------------------ 1. mtk_drm_drv.c (the early one)
s = rd(DRV)
if "COROT r102" in s:
    print("mtk_drm_drv.c: already patched")
else:
    old = """	if (d) {
		writel(0, d + 0x08);
		writel(0xffffffff, d + 0x0c);
	}"""
    if s.count(old) != 1:
        die("drv mask body anchor %d" % s.count(old))
    s = s.replace(old, NOTE + """	if (d) {
		writel(0x00000004, d + 0x08);	/* keep TE_RDY */
		writel(0x00005000, d + 0x0c);	/* clear only underrun + inp-unfin */
	}""", 1)
    wr(DRV, s)
    print("mtk_drm_drv.c: corot_mask_display_irqs_early keeps TE_RDY (INTEN=0x4)")

# ------------------------------------- 2. mtk_drm_crtc.c (the one inside the #ifdef)
c = rd(CRTC)
if "COROT r102" in c:
    print("mtk_drm_crtc.c: already patched")
else:
    old = """	if (d) {
		writel(0, d + 0x08);		/* DSI_INTEN = 0 */
		writel(0xffffffff, d + 0x0c);	/* clear latched */
	}"""
    if c.count(old) != 1:
        die("crtc mask body anchor %d" % c.count(old))
    c = c.replace(old, NOTE + """	if (d) {
		writel(0x00000004, d + 0x08);	/* keep TE_RDY, mask the underrun pair */
		writel(0x00005000, d + 0x0c);	/* clear only the latched underruns */
	}""", 1)
    wr(CRTC, c)
    print("mtk_drm_crtc.c: corot_mask_display_irqs keeps TE_RDY (INTEN=0x4)")

# ------------------------------------- 3. the DSI dump reads the wrong block
s = rd(DRV)
old = """void corot_dump_dsi(void)
{
	void __iomem *dsi = ioremap(0x14017000, 0x1000);"""
if s.count(old) == 1:
    s = s.replace(old, """void corot_dump_dsi(void)
{
	/* COROT r102: the DSI is at 0x1400d000 (see dsi0 in the dts).  0x14017000
	 * is not the DSI, so every COROT-DSI line this printed was meaningless -
	 * and reading a block that may not be clocked is one of the ways to take
	 * an asynchronous SError. */
	void __iomem *dsi = ioremap(0x1400d000, 0x1000);""", 1)
    wr(DRV, s)
    print("mtk_drm_drv.c: corot_dump_dsi now reads 0x1400d000")
else:
    print("mtk_drm_drv.c: corot_dump_dsi already fixed")

print("OK (CPU-direct build; only DSI_INTEN policy changed)")
