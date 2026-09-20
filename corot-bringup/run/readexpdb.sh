#!/usr/bin/env bash
# Read the kernel's crash-surviving console log out of the expdb partition.
#
# cust:/boot-log.txt is written by the initramfs, so it only exists if the
# kernel reached userspace.  A kernel that hangs earlier leaves NOTHING there.
# Every printk() is also mirrored into the log_store ring at physical
# 0x7ffbf000 (COROT_LOGSTORE_PA), the ring is dcache-cleaned on every write, and
# LK dumps it into the expdb partition on the following boot.
#
# WHERE in expdb: NOT at offset 0.  An earlier version of this script read the
# first 2 MiB and reported "no header" for r101 - the dump was entirely zero
# there.  The ring actually lands ~384 KiB from the END of the 128 MiB
# partition (offset 0x7FAxxxx in expdb-r100.bin).  So read the tail.
set -u
A=/mnt/d/platform-tools/adb.exe
S=UGEEOVYX4TZ9EE45
OUT=${1:-/home/mytiantian/corot-work/expdb.txt}
SKIP=${2:-124}          # MiB to skip; 128 MiB partition -> last 4 MiB

echo "=== waiting for Android ==="
for i in $(seq 1 40); do "$A" devices 2>/dev/null | grep -q "$S" && break; sleep 10; done
for i in $(seq 1 30); do
  bc=$("$A" -s "$S" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')
  [ "$bc" = "1" ] && break
  sleep 10
done

echo "=== expdb tail (skip ${SKIP} MiB) ==="
"$A" -s "$S" shell su -c "dd if=/dev/block/by-name/expdb bs=1M skip=$SKIP 2>/dev/null" > /tmp/expdb.raw 2>/dev/null
echo "  raw bytes: $(stat -c %s /tmp/expdb.raw 2>/dev/null)"

python3 - "$OUT" <<'PY'
import sys, re
raw = open('/tmp/expdb.raw','rb').read()
out = sys.argv[1]
print('  raw len: %d' % len(raw))

# The ring body is plain ASCII text separated by \n; LK copies it verbatim, so
# do not look for the header - look for the text.  Split it at the round markers
# afterwards: the ring is circular and accumulates across boots.
runs = re.findall(rb'[\x20-\x7e\n\t]{24,}', raw)
keep = [r for r in runs if b'COROT' in r or b'Kernel panic' in r or b'log_store' in r]
blob = b'\n'.join(keep)
open(out,'wb').write(blob)
print('  %d printable runs, %d mention COROT -> %d bytes -> %s'
      % (len(runs), len(keep), len(blob), out))

s = blob.decode('latin-1','replace')
print()
print('=== COROT markers seen ===')
for line in s.split('\n'):
    if 'COROT-MARKER' in line:
        print('  ' + line.strip()[:170])
print()
for pat in ('Kernel panic','SError','Unable to handle','Call trace','corot-log','log_store',
            'COROT-B100','COROT-STAGE','COROT-BC probe','Initialized','EPROBE_DEFER','-517'):
    print('  %-20s %d' % (pat, s.count(pat)))
PY
