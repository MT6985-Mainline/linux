#!/usr/bin/env python3
# Round 104: back to option B (the vendor's real CMDQ stack) - and this time with
# the two defects that made every previous option-B build look dead.
#
# Why option B is worth another boot
# ---------------------------------
# r103 finally made the CPU-direct build stable (no reboot; 279 s of telemetry,
# 11664 frames).  What it also made visible is that the CPU path underruns on
# *every single frame*:
#
#     COROT-FT phase=4 ... pushed=11664 ur=11660 inp=11660 frm=11659
#     dsi_us avg=7055 max=7100 | ovl_us avg=7054
#
# 7055 us per frame is the *DSI link* time for one full 1220x2712 DSC frame at
# 1152 Mbps/lane, and frm == pushed means the frames do complete - so the frame
# is not being cut short, it is being starved: DSIBUF runs dry mid-frame
# (ur == pushed) and the panel shows the top of the frame and rubbish below.
# That is a memory-path problem, and the memory path is exactly what the
# CPU-direct build does not have: it has no SMI/LARB tree at all, by design.
# Option B is the build where the vendor's own SMI + CMDQ-driven trigger
# sequence runs.
#
# The two defects it has been carrying
# -----------------------------------
#   1. corot_mask_display_irqs_early() wrote DSI INTEN = 0, which also removed
#      TE_RDY - the only source of a frame boundary on a command-mode panel
#      (mtk_dsi_irq_status -> mtk_crtc_vblank_irq).  Fixed in r102.
#   2. mtk_dsi_irq_status() called ioremap() in interrupt context for the
#      COROT-UNDERRUN dump, so the first buffer underrun was
#         kernel BUG at mm/vmalloc.c:3228
#      and the kernel died.  That is *why* (1) was written the way it was.
#      Fixed in r103.
#
# Both are in the shared code, so this round gets them from apply_r102/r103; the
# option-B specifics come from apply_r92 (SMI/LARB/IOMMU tree, GCE clocks, smi,
# iommus, mboxes, no -DDRM_CMDQ_DISABLE).
#
# What this round adds: a GCE audit
# --------------------------------
# The question is now "is the GCE actually running the display's packets?", and
# that needs to be answerable from the log rather than inferred from the picture:
#
#   * frame/TE/underrun counters incremented in mtk_dsi_irq_status(), so
#     "are frames being triggered at all" has a number;
#   * a bounded audit work (6 shots, 5 s apart, then it stops) that prints those
#     counters next to DSI/MUTEX/OVL/DSC state - the same register set the
#     CPU-direct telemetry uses, so the two builds are directly comparable;
#   * a bounded GCE-side dump (3 shots) of the environment and of the threads the
#     dispsys node actually uses (0-7, 22, 24, 25), which reports each thread's
#     CURR_ADDR / END_ADDR / CNT / IRQ_ENABLE.  A thread whose CNT advances is a
#     GCE that is fetching and executing; a thread stuck at PC == start with IRQ
#     status 0x10 is the GCE-D DDR-access failure that the vendor's
#     GCE_GCTL_VALUE workaround exists for.
import io
import os
import re
import sys

K = "/home/mytiantian/linux-corot"
K2 = K + "/drivers/gpu/drm/mediatek/mediatek_v2"
DSI = K2 + "/mtk_dsi.c"
DRV = K2 + "/mtk_drm_drv.c"
MBOX = K + "/drivers/misc/mediatek/cmdq/mailbox/mtk-cmdq-mailbox-ext.c"


def die(m):
    print("FAIL: " + m)
    sys.exit(1)


def rd(p):
    return io.open(p, encoding="utf-8").read()


def wr(p, s):
    io.open(p, "w", encoding="utf-8").write(s)
    os.utime(p, None)


# ===================================================== A. re-mask after INTEN
s = rd(DSI)
if "corot_mask_display_irqs_early" in s:
    print("mtk_dsi.c: mask already wired")
else:
    inc = "#include <linux/clk.h>"
    if s.count(inc) == 1:
        s = s.replace(inc, inc + """
/* COROT r104: clears the display interrupt enables; defined in mtk_drm_drv.c
 * outside every #ifdef, so it exists in both build modes. */
extern void corot_mask_display_irqs_early(void);""", 1)
    else:
        m = re.search(r"^#include [^\n]*\n", s, re.M)
        if not m:
            die("no include in mtk_dsi.c")
        s = s[:m.end()] + """
/* COROT r104: defined in mtk_drm_drv.c, outside every #ifdef. */
extern void corot_mask_display_irqs_early(void);
""" + s[m.end():]

    old = "\tmtk_dsi_set_interrupt_enable(dsi);\n"
    i = s.find(old)
    if i < 0:
        die("mtk_dsi_set_interrupt_enable call not found")
    s = s[:i + len(old)] + """\t/*
\t * COROT r104: mtk_dsi_set_interrupt_enable() has just re-armed
\t * INTEN = 0x00005004 (BUFFER_UNDERRUN | INP_UNFINISH | TE_RDY).  Put the
\t * mask back immediately, but a mask that keeps TE_RDY - see r102.  Without
\t * this the underrun interrupt fires on every frame; with the r103 fix it no
\t * longer kills the kernel, but it is still pure noise.
\t */
\tcorot_mask_display_irqs_early();
""" + s[i + len(old):]
    print("mtk_dsi.c: corot_mask_display_irqs_early() now runs after INTEN is set")

# ===================================================== B. frame counters
if "COROT r104: frame accounting" in s:
    print("mtk_dsi.c: counters already present")
else:
    anchor = "static irqreturn_t mtk_dsi_irq_status(int irq, void *dev_id)\n{"
    if s.count(anchor) != 1:
        die("mtk_dsi_irq_status anchor %d" % s.count(anchor))
    s = s.replace(anchor, """/*
 * COROT r104: frame accounting.
 *
 * "Does the display get triggered at all" needs a number, not an inference from
 * the picture - especially in the option-B build, where none of the CPU-direct
 * telemetry is compiled in.  These are bumped in the DSI interrupt handler from
 * the raw INTSTA value, before the handler's own masking of the status word.
 */
static unsigned int corot_dsi_frm_n;
static unsigned int corot_dsi_te_n;
static unsigned int corot_dsi_ur_n;

void corot_dsi_counts(unsigned int *frm, unsigned int *te, unsigned int *ur)
{
	*frm = corot_dsi_frm_n;
	*te = corot_dsi_te_n;
	*ur = corot_dsi_ur_n;
}

""" + anchor, 1)

    old = """	status &= 0xffde;
	if (status) {"""
    if s.count(old) != 1:
        die("status mask anchor %d" % s.count(old))
    s = s.replace(old, """	/* COROT r104: frame accounting, from the raw status word */
	if (status & FRAME_DONE_INT_FLAG)
		corot_dsi_frm_n++;
	if (status & TE_RDY_INT_FLAG)
		corot_dsi_te_n++;
	if (status & BUFFER_UNDERRUN_INT_FLAG)
		corot_dsi_ur_n++;

	status &= 0xffde;
	if (status) {""", 1)
    print("mtk_dsi.c: frame/TE/underrun counters added to the DSI ISR")

wr(DSI, s)

# ===================================================== C. the audit work
r = rd(DRV)
if "COROT-GCEAUDIT" in r:
    print("mtk_drm_drv.c: audit already present")
else:
    anchor = "static int mtk_drm_probe(struct platform_device *pdev)\n{"
    if r.count(anchor) != 1:
        die("mtk_drm_probe anchor %d" % r.count(anchor))
    r = r.replace(anchor, """/*
 * COROT r104: bounded display/GCE audit.
 *
 * Six shots, five seconds apart, then it stops.  A periodic reader of these
 * registers is what hung the SoC in r69/r80/r81, so the count is fixed and the
 * work is never re-armed after the last shot.
 *
 * The point is to answer one question with evidence: in the option-B build, is
 * anything triggering frames?  The register set is deliberately the same one the
 * CPU-direct telemetry prints, so the two builds can be compared line by line.
 */
extern void corot_dsi_counts(unsigned int *frm, unsigned int *te, unsigned int *ur);

#define COROT_AUDIT_SHOTS	6
#define COROT_AUDIT_MS		5000

/* the work re-arms itself: declare the object, then the callback, then INIT it
 * at the arming site */
static struct delayed_work corot_audit_dw;
static void corot_audit_dump(struct work_struct *w);

static void corot_audit_dump(struct work_struct *w)
{
	static int shot;
	void __iomem *dsi = ioremap(0x1400d000, 0x1000);
	void __iomem *mtx0 = ioremap(0x14001000, 0x1000);
	void __iomem *mtx1 = ioremap(0x14401000, 0x1000);
	void __iomem *ovl = ioremap(0x14402000, 0x1000);
	void __iomem *dsc = ioremap(0x1400c000, 0x1000);
	unsigned int frm = 0, te = 0, ur = 0;

	corot_dsi_counts(&frm, &te, &ur);
	shot++;

	pr_err("COROT-GCEAUDIT[%d] frames=%u te=%u underrun=%u\\n", shot, frm, te, ur);
	pr_err("COROT-GCEAUDIT[%d] dsi   START=0x%08x INTSTA=0x%08x INTEN=0x%08x CON=0x%08x MODE=0x%08x PSCTRL=0x%08x SIZE=0x%08x\\n",
	       shot,
	       dsi ? readl(dsi + 0x00) : 0, dsi ? readl(dsi + 0x0c) : 0,
	       dsi ? readl(dsi + 0x08) : 0, dsi ? readl(dsi + 0x10) : 0,
	       dsi ? readl(dsi + 0x14) : 0, dsi ? readl(dsi + 0x1c) : 0,
	       dsi ? readl(dsi + 0x38) : 0);
	pr_err("COROT-GCEAUDIT[%d] mutex mtx0 EN=0x%08x SOF=0x%08x | mtx1 EN=0x%08x SOF=0x%08x\\n",
	       shot,
	       mtx0 ? readl(mtx0 + 0x00) : 0, mtx0 ? readl(mtx0 + 0x04) : 0,
	       mtx1 ? readl(mtx1 + 0x00) : 0, mtx1 ? readl(mtx1 + 0x04) : 0);
	pr_err("COROT-GCEAUDIT[%d] ovl   EN=0x%08x INTSTA=0x%08x ROI=0x%08x SRC_CON=0x%08x DATAPATH=0x%08x\\n",
	       shot,
	       ovl ? readl(ovl + 0x00) : 0, ovl ? readl(ovl + 0x0c) : 0,
	       ovl ? readl(ovl + 0x20) : 0, ovl ? readl(ovl + 0x2c) : 0,
	       ovl ? readl(ovl + 0x24) : 0);
	pr_err("COROT-GCEAUDIT[%d] dsc   CON=0x%08x INTSTA=0x%08x MODE=0x%08x ENC_W=0x%08x BUF=0x%08x\\n",
	       shot,
	       dsc ? readl(dsc + 0x00) : 0, dsc ? readl(dsc + 0x08) : 0,
	       dsc ? readl(dsc + 0x10) : 0, dsc ? readl(dsc + 0x14) : 0,
	       dsc ? readl(dsc + 0x2c) : 0);

	if (dsi)
		iounmap(dsi);
	if (mtx0)
		iounmap(mtx0);
	if (mtx1)
		iounmap(mtx1);
	if (ovl)
		iounmap(ovl);
	if (dsc)
		iounmap(dsc);

	if (shot < COROT_AUDIT_SHOTS)
		schedule_delayed_work(&corot_audit_dw,
				      msecs_to_jiffies(COROT_AUDIT_MS));
	else
		pr_err("COROT-GCEAUDIT: done after %d shots\\n", shot);
}

""" + anchor, 1)

    old2 = '\tpr_err("COROT-STAGE probe: comps collected, master_add next\\n");'
    if r.count(old2) != 1:
        die("master_add breadcrumb anchor %d" % r.count(old2))
    r = r.replace(old2, """\tpr_err("COROT-STAGE probe: comps collected, master_add next\\n");
\t/* COROT r104: first audit shot five seconds from now */
\tINIT_DELAYED_WORK(&corot_audit_dw, corot_audit_dump);
\tschedule_delayed_work(&corot_audit_dw, msecs_to_jiffies(COROT_AUDIT_MS));""", 1)
    wr(DRV, r)
    print("mtk_drm_drv.c: bounded GCE/display audit armed (6 shots, 5 s apart)")

# ===================================================== D. GCE-side audit
m = rd(MBOX)
if "COROT-GCEAUDIT" in m:
    print("mtk-cmdq-mailbox-ext.c: audit already present")
else:
    old = "static s32 cmdq_clk_enable(struct cmdq *cmdq)\n{"
    if m.count(old) != 1:
        die("cmdq_clk_enable anchor %d" % m.count(old))
    m = m.replace(old, """/*
 * COROT r104: does the GCE actually fetch and execute?
 *
 * The environment dump already fires on enable and on error, but "no errors"
 * only proves the GCE is quiet.  These three shots read the threads the dispsys
 * node really uses and report each one's CURR_ADDR / END_ADDR / CNT / IRQ_ENABLE:
 * a thread whose CNT advances across shots is a GCE that is running packets,
 * while PC == start with IRQ status 0x10 is the GCE-D DDR-access failure that
 * GCE_GCTL_VALUE exists to avoid.
 */
static const u32 corot_audit_thr[] = { 0, 1, 2, 3, 4, 5, 6, 7, 22, 24, 25 };
static atomic_t corot_thr_audit_left = ATOMIC_INIT(3);
static struct delayed_work corot_thr_audit_dw;
static struct cmdq *corot_thr_audit_cmdq;

static void corot_gce_thread_audit(struct work_struct *w)
{
	struct cmdq *cmdq = corot_thr_audit_cmdq;
	unsigned int i;
	int left;

	if (!cmdq)
		return;

	left = atomic_dec_return(&corot_thr_audit_left);
	corot_gce_env_dump(cmdq, "audit", 0);
	for (i = 0; i < ARRAY_SIZE(corot_audit_thr); i++)
		corot_gce_thread_dump(cmdq, "audit", corot_audit_thr[i]);
	pr_err("COROT-GCEAUDIT: thread shot done, %d left\\n", left);

	if (left > 0)
		schedule_delayed_work(&corot_thr_audit_dw,
				      msecs_to_jiffies(5000));
}

static s32 cmdq_clk_enable(struct cmdq *cmdq)
{""", 1)

    # arm it the first time the GCE is actually clocked on
    old2 = "\tusage = atomic_inc_return(&cmdq->usage);"
    if m.count(old2) != 1:
        die("usage anchor %d" % m.count(old2))
    m = m.replace(old2, old2 + """
		if (usage == 1 && corot_thr_audit_cmdq != cmdq) {
			corot_thr_audit_cmdq = cmdq;
			INIT_DELAYED_WORK(&corot_thr_audit_dw,
					  corot_gce_thread_audit);
			schedule_delayed_work(&corot_thr_audit_dw,
					      msecs_to_jiffies(6000));
			pr_err("COROT-GCEAUDIT: thread audit armed\\n");
		}""", 1)
    wr(MBOX, m)
    print("mtk-cmdq-mailbox-ext.c: bounded thread audit armed (3 shots, 5 s apart)")

print("OK (option B stack + r102 + r103 + r104 audit)")
