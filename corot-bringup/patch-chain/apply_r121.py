#!/usr/bin/env python3
# Round 121: make the driver core say who probes, who defers and who is last.
#
# r115..r120 all stop the machine with no error-level output - not even the
# COROT-CLKPROBE breadcrumb I put at the top of clk_mt6985_mmsys_grp_probe(),
# which normally runs inside the first second.  The r119 control (same build
# without the clock list) logs normally.  So the hang is very early and silent.
#
# Silence is the problem: the ring only keeps pr_err, and everything the deferred
# probe machinery says is pr_debug.  A cycle in the probe graph does not crash
# anything - deferred_probe_work_func() simply keeps retrying, the boot never
# reaches userspace, and nothing is logged at a level we can see.
#
# So put pr_err where the decisions are made:
#   - really_probe(): device name on entry, and the return value - which turns
#     "-517 / -EPROBE_DEFER" from an invisible debug message into a line we can
#     read, and names the device that asked for the deferral;
#   - device_add()'s deferred trigger is left alone on purpose: if the workqueue
#     is spinning, printing from inside it would flood the 56 KB ring.
#
# The last COROT-PROBE line in the ring is the device that probed immediately
# before the machine stops; the COROT-DEFER lines name the cycle.
import io
import os
import sys

K = "/home/mytiantian/linux-corot"
DD = K + "/drivers/base/dd.c"


def die(m):
    print("FAIL: " + m)
    sys.exit(1)


def rd(p):
    return io.open(p, encoding="utf-8").read()


def wr(p, s):
    io.open(p, "w", encoding="utf-8").write(s)
    os.utime(p, None)


s = rd(DD)
if "COROT r121" in s:
    print("dd.c: already patched")
    print("OK")
    sys.exit(0)

anchor = "static int really_probe(struct device *dev, const struct device_driver *drv)\n{"
if s.count(anchor) != 1:
    die("really_probe anchor %d" % s.count(anchor))

s = s.replace(anchor, anchor + """
	/* COROT r121: error level on purpose - the ring only keeps pr_err, and
	 * without this the boot stops with no trace at all. */
	pr_err("COROT-PROBE %s <- %s\\n",
	       dev_name(dev), drv ? drv->name : "(none)");
""", 1)
print("dd.c: really_probe() entry breadcrumb added")

# report the deferrals: find the place where a probe returning -EPROBE_DEFER is
# turned into a deferred add.  In this tree it is the switch inside really_probe.
old = """	case -EPROBE_DEFER:"""
if s.count(old) != 1:
    die("EPROBE_DEFER case anchor %d" % s.count(old))
s = s.replace(old, """	case -EPROBE_DEFER:
		/* COROT r121: name the device that asked for the deferral */
		pr_err("COROT-DEFER %s <- %s\\n",
		       dev_name(dev), drv ? drv->name : "(none)");""", 1)
print("dd.c: deferral breadcrumb added")

wr(DD, s)
print("OK")
