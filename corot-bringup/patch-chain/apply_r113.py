#!/usr/bin/env python3
# Round 113: make room in the ring, then read the r112 measurement properly.
#
# r112 ran 70 s instead of 40 s, so something changed - but none of the new
# COROT-INITPWR lines survived, and the COROT-SNAP lines in the ring come from
# several different boots, so they could not be attributed.  The cause is ours:
# the instrumentation writes far more than the 56 KB ring holds.  Per boot this
# round emits, on top of everything else:
#
#   COROT-CLK r94 : 8 muxes x (1 header + 16 parents) = ~136 lines
#   corot_dump_disp("lk-handoff")    ~4.5 KB
#   corot_dump_disp("kms_init_done") ~4.5 KB
#
# i.e. more than half the ring is our own debug output, and it is emitted before
# the interesting part (the end of kms_init).
#
# This round silences exactly those three and changes no behaviour:
#   - the per-parent clock enumeration (the summary and the set_parent result
#     still print, which is all that was ever read);
#   - the two big register dumps, superseded by COROT-SNAP's five registers.
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
if "COROT r113" in s:
    print("mtk_drm_drv.c: already patched")
    print("OK")
    sys.exit(0)

# 1. the 16-lines-per-mux parent enumeration
old = """		pr_err("COROT-CLK r94 %-9s:   [%u] %-22s %lu Hz\\n",
		       name, i, clk_hw_get_name(p), r);"""
if s.count(old) != 1:
    die("clock parent print anchor %d" % s.count(old))
s = s.replace(old, """		/* COROT r113: the per-parent listing was 128 lines a boot and
		 * pushed the interesting output out of the 56 KB ring.  The
		 * best-parent line below still prints. */
		(void)r;""", 1)
print("silenced the COROT-CLK parent enumeration (128 lines/boot)")

# 2. the two big register dumps
for stage in ("lk-handoff", "kms_init_done"):
    o = '\tcorot_dump_disp("%s");\n' % stage
    if s.count(o) != 1:
        die("corot_dump_disp(%s) anchor %d" % (stage, s.count(o)))
    s = s.replace(o, "\t/* COROT r113: %s dump silenced - ~4.5 KB of ring for\n"
                     "\t * data COROT-SNAP already reports in five registers. */\n" % stage, 1)
    print("silenced corot_dump_disp(\"%s\")" % stage)

wr(DRV, s)
print("OK")
