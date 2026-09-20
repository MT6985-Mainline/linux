#!/usr/bin/env python3
# Round 131: stop the dual-pipe path from dereferencing a NULL component, and
# make it say which component is missing.
#
# r130's log finally contained the whole story:
#
#   Internal error: Oops: 96000006 [#1] SMP
#   pc : mtk_crtc_dual_layer_config+0xb4/0x16c      x0 = 0
#   Call trace:
#     mtk_crtc_dual_layer_config
#     mtk_crtc_restore_plane_setting
#     mtk_drm_crtc_enable
#     ...
#     drm_fb_helper_set_par -> fbcon_init -> do_take_over_console
#
# and the cause is this line in mtk_crtc_dual_layer_config():
#
#   p_comp = priv->ddp_comp[dual_pipe_comp_mapping(priv->data->mmsys_id, comp->id)];
#
# dual_pipe_comp_mapping() in this tree has cases for MMSYS_MT6983/MT6895/MT6885
# only - MT6985 falls into `default:` which logs "unknown mmsys" and returns 0,
# so p_comp becomes priv->ddp_comp[0], and id 0 (DDP_COMPONENT_OVL0) has no DT
# node in the corot tree -> NULL -> oops.
#
# That is the half-screen mystery in full: with is_dual_pipe=0 (before r107's
# helper table) this function was never called, so the panel showed the primary
# pipe only and nothing crashed; the moment the dual pipe is enabled, the path
# runs and dies on the missing counterpart.
#
# This round: port the vendor's MT6985 mapping for the components this tree
# actually has, and guard the dereference.  Missing counterparts are then
# reported once and skipped (left pipe only) instead of oopsing, so the port can
# grow the ovlsys-side components one at a time.
import io
import os
import re
import sys

K = "/home/mytiantian/linux-corot"
CRTC = K + "/drivers/gpu/drm/mediatek/mediatek_v2/mtk_drm_crtc.c"
HDR = K + "/drivers/gpu/drm/mediatek/mediatek_v2/mtk_drm_ddp_comp.h"


def die(m):
    print("FAIL: " + m)
    sys.exit(1)


def rd(p):
    return io.open(p, encoding="utf-8").read()


def wr(p, s):
    io.open(p, "w", encoding="utf-8").write(s)
    os.utime(p, None)


s = rd(CRTC)
hdr = rd(HDR)

if "COROT r131" in s:
    print("mtk_drm_crtc.c: already patched")
    sys.exit(0)

# ---- which of the vendor's MT6985 mapping targets exist in this tree? -------
VENDOR_MAP = [
    ("DDP_COMPONENT_OVL0", "DDP_COMPONENT_OVL1"),
    ("DDP_COMPONENT_OVL0_2L", "DDP_COMPONENT_OVL4_2L"),
    ("DDP_COMPONENT_OVL1_2L", "DDP_COMPONENT_OVL5_2L"),
    ("DDP_COMPONENT_OVL2_2L", "DDP_COMPONENT_OVL6_2L"),
    ("DDP_COMPONENT_OVL3_2L", "DDP_COMPONENT_OVL7_2L"),
    ("DDP_COMPONENT_OVL7_2L", "DDP_COMPONENT_OVL3_2L"),
    ("DDP_COMPONENT_OVL0_2L_NWCG", "DDP_COMPONENT_OVL2_2L_NWCG"),
    ("DDP_COMPONENT_OVL1_2L_NWCG", "DDP_COMPONENT_OVL3_2L_NWCG"),
    ("DDP_COMPONENT_OVL2_2L_NWCG", "DDP_COMPONENT_OVL0_2L_NWCG"),
    ("DDP_COMPONENT_OVL3_2L_NWCG", "DDP_COMPONENT_OVL1_2L_NWCG"),
    ("DDP_COMPONENT_AAL0", "DDP_COMPONENT_AAL1"),
    ("DDP_COMPONENT_WDMA0", "DDP_COMPONENT_WDMA1"),
    ("DDP_COMPONENT_MDP_RDMA1", "DDP_COMPONENT_MDP_RDMA0"),
    ("DDP_COMPONENT_OVLSYS_WDMA0", "DDP_COMPONENT_OVLSYS_WDMA2"),
    ("DDP_COMPONENT_OVLSYS_WDMA1", "DDP_COMPONENT_OVLSYS_WDMA3"),
]

have = lambda name: re.search(r"\b%s\b" % name, hdr) is not None
usable = [(a, b) for a, b in VENDOR_MAP if have(a) and have(b)]
missing = [(a, b) for a, b in VENDOR_MAP if not (have(a) and have(b))]
print("mapping entries usable in this tree : %d" % len(usable))
for a, b in usable:
    print("    %-30s -> %s" % (a, b))
print("mapping entries NOT portable yet    : %d" % len(missing))
for a, b in missing:
    print("    %-30s -> %-30s (id absent)" % (a, b))

# ---------------------------------------------------- 1. the MT6985 map itself
FUNC = "static unsigned int dual_comp_map_mt6985(unsigned int comp_id)\n{\n"
FUNC += "	/* COROT r131: ported from the vendor's mt6985 table, keeping only the\n"
FUNC += "	 * entries whose target component id exists in this tree.  The rest of\n"
FUNC += "	 * the ovlsys-side ids (OVL4_2L..OVL7_2L, OVLSYS_WDMA2/3) are not\n"
FUNC += "	 * declared here yet, so those stay unmapped (0) and the caller falls\n"
FUNC += "	 * back to the primary pipe instead of dereferencing a NULL. */\n"
FUNC += "	switch (comp_id) {\n"
for a, b in usable:
    FUNC += "	case %s:\n		return %s;\n" % (a, b)
FUNC += "	default:\n"
FUNC += "		DDPMSG(\"COROT r131: comp %u has no MT6985 dual-pipe counterpart in this port\\n\",\n"
FUNC += "		       comp_id);\n"
FUNC += "		return 0;\n"
FUNC += "	}\n}\n\n"

anchor = "static unsigned int dual_comp_map_mt6895(unsigned int comp_id)\n{"
if s.count(anchor) != 1:
    die("dual_comp_map_mt6895 anchor count %d" % s.count(anchor))
s = s.replace(anchor, FUNC + anchor, 1)
print("added dual_comp_map_mt6985() (%d entries)" % len(usable))

# ------------------------------------------------- 2. wire it into the switch
old_sw = """	case MMSYS_MT6983:
		ret = dual_comp_map_mt6983(comp_id);
		break;
	case MMSYS_MT6895:"""
if s.count(old_sw) != 1:
    die("mapping switch anchor count %d" % s.count(old_sw))
s = s.replace(old_sw, """	case MMSYS_MT6983:
		ret = dual_comp_map_mt6983(comp_id);
		break;
	case MMSYS_MT6985:
		ret = dual_comp_map_mt6985(comp_id);
		break;
	case MMSYS_MT6895:""", 1)
print("dual_pipe_comp_mapping(): MMSYS_MT6985 case added")

# ------------------------------------------------- 3. the NULL guard (crash fix)
old_call = """	p_comp = priv->ddp_comp[dual_pipe_comp_mapping(priv->data->mmsys_id, comp->id)];
	mtk_ddp_comp_layer_config(p_comp, idx,
				&plane_state_r, cmdq_handle);
"""
if s.count(old_call) != 1:
    die("dual_layer_config deref anchor count %d" % s.count(old_call))
new_call = """	/*
	 * COROT r131: this dereference killed the kernel.
	 *
	 * dual_pipe_comp_mapping() returned 0 for MT6985 (no case for this SoC),
	 * so p_comp became priv->ddp_comp[0] - DDP_COMPONENT_OVL0, which has no
	 * DT node on corot - and the very next call oopsed:
	 *     pc : mtk_crtc_dual_layer_config+0xb4/0x16c   x0 = 0
	 *     mtk_crtc_restore_plane_setting <- mtk_drm_crtc_enable <- fbcon
	 * The right-hand pipe components (the ovlsys OVLs, the second DSC and
	 * MUTEX) are not ported yet, so until they are, configure the primary
	 * pipe only and say exactly which counterpart was missing.
	 */
	comp_id_r = dual_pipe_comp_mapping(priv->data->mmsys_id, comp->id);
	if (comp_id_r == 0 || priv->ddp_comp[comp_id_r] == NULL) {
		pr_err_ratelimited("COROT r131: no dual-pipe counterpart for comp %u (mapped %u) - primary pipe only\\n",
				   comp->id, comp_id_r);
		mtk_ddp_comp_layer_config(comp, idx, &plane_state_l, cmdq_handle);
		return;
	}
	p_comp = priv->ddp_comp[comp_id_r];
	mtk_ddp_comp_layer_config(p_comp, idx,
				&plane_state_r, cmdq_handle);
"""
s = s.replace(old_call, new_call, 1)
print("mtk_crtc_dual_layer_config(): NULL guard added")

# declare the new local
old_decl = """	struct mtk_plane_state plane_state_l;
	struct mtk_plane_state plane_state_r;
	struct mtk_ddp_comp *p_comp;
"""
if s.count(old_decl) != 1:
    die("local-decl anchor count %d" % s.count(old_decl))
s = s.replace(old_decl, """	struct mtk_plane_state plane_state_l;
	struct mtk_plane_state plane_state_r;
	struct mtk_ddp_comp *p_comp;
	unsigned int comp_id_r;
""", 1)
print("mtk_crtc_dual_layer_config(): comp_id_r declared")

wr(CRTC, s)
print("OK")
