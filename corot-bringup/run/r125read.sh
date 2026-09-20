#!/usr/bin/env bash
# r125 read-out: both channels, after stock is back
set -u
A=/mnt/d/platform-tools/adb.exe
S=UGEEOVYX4TZ9EE45
T=/home/mytiantian/corot-work/.flash2
LOG=/home/mytiantian/corot-work/r125-cust-log.txt
EBL=/home/mytiantian/corot-work/r125-expdb.txt
mkdir -p "$T"

for i in $(seq 1 40); do "$A" devices 2>/dev/null | grep -q "$S" && break; sleep 10; done
for i in $(seq 1 36); do
  bc=$("$A" -s "$S" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')
  [ "$bc" = "1" ] && { echo "Android up"; break; }
  sleep 10
done

echo "=== channel A: cust boot-log.txt ==="
rm -f "$LOG"
for attempt in 1 2 3 4; do
  "$A" -s "$S" exec-out su -c "mkdir -p /data/local/tmp/cust_ro; mount -t ext4 -o ro,noload /dev/block/by-name/cust /data/local/tmp/cust_ro 2>/dev/null; cat /data/local/tmp/cust_ro/boot-log.txt; umount /data/local/tmp/cust_ro 2>/dev/null" > "$LOG" 2>/dev/null
  sz=$(stat -c %s "$LOG" 2>/dev/null || echo 0)
  echo "  attempt $attempt: $sz bytes"
  [ "$sz" -gt 20000 ] && break
  sleep 8
done

echo "=== channel B: expdb tail ==="
rm -f "$T/e125.bin"
for attempt in 1 2 3; do
  "$A" -s "$S" exec-out su -c "dd if=/dev/block/by-name/expdb bs=1M skip=124 2>/dev/null" > "$T/e125.bin" 2>/dev/null
  sz=$(stat -c %s "$T/e125.bin" 2>/dev/null || echo 0)
  nz=$(python3 -c "
raw=open('$T/e125.bin','rb').read() if $sz else b''
print(0 if not raw else 100*raw.count(0)//len(raw))")
  echo "  attempt $attempt: $sz bytes zero=$nz%"
  [ "$sz" -gt 100000 ] && [ "$nz" -lt 99 ] && break
  sleep 8
done

python3 - "$LOG" "$EBL" "$T/e125.bin" <<'PY'
import re, sys
cust, ebl, edb = sys.argv[1], sys.argv[2], sys.argv[3]
def load(p):
    try: return open(p, 'rb').read()
    except Exception: return b''
c = load(cust)
print()
print('################ CHANNEL A (cust boot-log.txt): %d bytes' % len(c))
if c:
    s = c.decode('latin-1', 'replace')
    for pat in ('corot-log: initramfs log-catcher started', 'cust found at', 'cust mounted rw',
                'COROT-MARKER', 'COROT-PROBE', 'COROT-DEFER', 'COROT-CLKPROBE', 'COROT-TOPCLK',
                'COROT-SNAP', 'COROT-STAGE', 'COROT-BC probe', 'Kernel panic', 'SError',
                'suspended=1', 'suspended=0', 'is_dual_pipe=1', 'is_dual_pipe=0',
                'BUG:', 'Unable to handle', 'Internal error', 'reboot to bootloader'):
        print('  %-50s %d' % (pat, s.count(pat)))
    print('  markers: %s' % sorted(set(re.findall(r'COROT-MARKER [a-z0-9-]*', s))))
    for key in ('cust found at', 'cust mounted', 'NOT FOUND'):
        for line in [x for x in s.split('\n') if key in x][:3]:
            print('  > %s' % line.strip()[:160])
    for key in ('COROT-TOPCLK', 'COROT-CLKPROBE', 'COROT-SNAP', 'COROT-STAGE', 'COROT-PROBE'):
        hits = [x for x in s.split('\n') if key in x]
        print('  --- %s (%d):' % (key, len(hits)))
        for h in hits[:8]:
            print('      %s' % h.strip()[:170])
    tail = [x for x in s.split('\n') if x.strip()][-12:]
    print('  --- last lines:')
    for l in tail:
        print('      %s' % l.strip()[:170])
e = load(edb)
print()
print('################ CHANNEL B (expdb tail): %d bytes zero=%d%%' % (len(e), 0 if not e else 100*e.count(0)//len(e)))
if e:
    print('  jump-to-kernel: %d' % e.count(b'jump to linux kernel'))
    print('  kernel_sz: %s' % sorted(set(x.decode() for x in re.findall(rb'kernel_sz=0x([0-9a-f]+)', e))))
    print('  COROT lines: %d  markers: %s' % (len(re.findall(rb'COROT-', e)),
          sorted(set(x.decode() for x in re.findall(rb'COROT-MARKER ([a-z0-9-]+)', e)))))
    hits = re.findall(rb'COROT[^\r\n\x01]{0,150}', e)
    for h in hits[:15]:
        print('      %s' % h.decode('latin-1', 'replace'))
PY
