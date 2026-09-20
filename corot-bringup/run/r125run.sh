#!/usr/bin/env bash
# r125run.sh - hardened single round: flash the staged test build, watch, restore
# stock, then read BOTH log channels (cust boot-log.txt + expdb tail).
#
# Differences from flash2.sh, all learned the hard way tonight:
#   * every fastboot/adb call is wrapped in `timeout` and retried, so a dropped
#     USB enumeration can never hang the round for 40 minutes;
#   * it waits for fastboot to actually enumerate and retries the reboot once;
#   * it reads the cust boot-log.txt (the channel that survives a watchdog reset)
#     as well as the expdb tail, and reports which one had data.
#
# Usage: r125run.sh [tag]      (tag defaults to r125)
set -u
A=/mnt/d/platform-tools/adb.exe
F=/mnt/d/platform-tools/fastboot.exe
S=UGEEOVYX4TZ9EE45
R='E:\corot\stock-restore-current'
D=/mnt/c/Users/mytiantian/Desktop
T=/home/mytiantian/corot-work/.flash2
TAG=${1:-r125}
LOG=/home/mytiantian/corot-work/${TAG}-cust-log.txt
EBL=/home/mytiantian/corot-work/${TAG}-expdb.txt
mkdir -p "$T"

adb_() { timeout 40 "$A" -s "$S" "$@" 2>/dev/null; }
fb_()  { timeout 90 "$F" -s "$S" "$@" 2>/dev/null; }

wait_adb() {   # wait_adb <tries>
  for i in $(seq 1 "${1:-30}"); do
    adb_ devices | grep -q "$S" && return 0
    sleep 5
  done
  return 1
}
wait_fastboot() {
  for i in $(seq 1 "${1:-24}"); do
    timeout 30 "$F" devices 2>/dev/null | grep -q "$S" && return 0
    sleep 5
  done
  return 1
}
wait_android() {
  wait_adb 30 || return 1
  for i in $(seq 1 36); do
    bc=$(adb_ shell getprop sys.boot_completed | tr -d '\r')
    [ "$bc" = "1" ] && return 0
    sleep 10
  done
  return 1
}

echo "=== 0. state ==="
if timeout 30 "$F" devices 2>/dev/null | grep -q "$S"; then
  echo "  device is in fastboot; rebooting to Android first"
  fb_ reboot
  sleep 25
fi
if wait_android; then echo "  Android up (clean start)"; else echo "  WARNING: Android not up"; fi

echo
echo "=== 1. flash the staged test build ==="
cp -f /home/mytiantian/corot-work/images/corot-boot.img      "$D/corot-test-boot.img"      || exit 1
cp -f /home/mytiantian/corot-work/images/corot-init_boot.img "$D/corot-test-init_boot.img" || exit 1
echo "  boot.img $(stat -c %s "$D/corot-test-boot.img")  init_boot.img $(stat -c %s "$D/corot-test-init_boot.img")"
echo "  kernel marker in image: $(strings "$D/corot-test-boot.img" | grep -m1 -o 'COROT-MARKER [a-z0-9-]*')"

adb_ reboot bootloader >/dev/null
if ! wait_fastboot 24; then
  echo "  fastboot did not enumerate after adb reboot - retrying once"
  sleep 10
  wait_fastboot 12 || { echo "  *** no fastboot: aborting before touching flash ***"; exit 1; }
fi
echo "  fastboot: $(timeout 30 "$F" devices | tr -d '\r' | tail -1)"

flash_part() {   # flash_part <part> <file>   (fastboot writes progress to stderr,
                 #  so success is judged by exit status, not by grepping OKAY)
  for i in 1 2 3 4; do
    if timeout 120 "$F" -s "$S" flash "$1" "$2" > "$T/fb.log" 2>&1; then
      echo "  $1 flashed (try $i)"
      return 0
    fi
    echo "  $1 try $i failed: $(tail -2 "$T/fb.log" | tr '\n' ' ' | cut -c1-120)"
    sleep 6
  done
  echo "  *** $1 FLASH FAILED ***"; return 1
}
flash_part boot_ab      'C:\Users\mytiantian\Desktop\corot-test-boot.img'      || exit 1
flash_part init_boot_a  'C:\Users\mytiantian\Desktop\corot-test-init_boot.img' || exit 1
flash_part init_boot_b  'C:\Users\mytiantian\Desktop\corot-test-init_boot.img' || exit 1
fb_ set_active a >/dev/null
fb_ reboot >/dev/null
echo "  flashed; watching (the log-catcher hands back to fastboot at ~180 s)"

echo
echo "=== 2. watch ==="
t=0
while [ $t -lt 260 ]; do
  sleep 10; t=$((t+10))
  timeout 30 "$F" devices 2>/dev/null | grep -q "$S" && { echo "  t=${t}s fastboot (kernel handed back)"; break; }
  adb_ devices | grep -q "$S" && { echo "  t=${t}s adb (Android came up)"; break; }
  [ $((t % 60)) -eq 0 ] && echo "  t=${t}s still running"
done
[ $t -ge 260 ] && echo "  t=260s window over (still running)"

echo
echo "=== 3. restore stock ==="
if ! wait_fastboot 20; then
  echo "  not in fastboot; trying adb reboot bootloader"
  adb_ reboot bootloader >/dev/null 2>&1
  if ! wait_fastboot 24; then
    echo "  still no fastboot - the test kernel may be hung at the boot logo."
    echo "  waiting up to 6 more minutes for a manual force-restart into fastboot"
    echo "  (hold power + volume-down), or for a watchdog reset"
    wait_fastboot 36 || echo "  *** STILL no fastboot: leaving the device as it is ***"
  fi
fi
flash_part boot_ab        "$R\\boot_a.img"        || echo "  (boot restore failed)"
flash_part vendor_boot_ab "$R\\vendor_boot_a.img" || echo "  (vendor_boot restore failed)"
flash_part init_boot_a    "$R\\init_boot_a.img"   || echo "  (init_boot_a restore failed)"
flash_part init_boot_b    "$R\\init_boot_b.img"   || echo "  (init_boot_b restore failed)"
fb_ set_active a >/dev/null
fb_ reboot >/dev/null
echo "  stock restored"
rm -f "$D/corot-test-boot.img" "$D/corot-test-init_boot.img"

echo
echo "=== 4. channel A: cust boot-log.txt ==="
if ! wait_android; then
  echo "  Android did not come up"; exit 1
fi
rm -f "$LOG"
for attempt in 1 2 3 4; do
  adb_ exec-out su -c "mkdir -p /data/local/tmp/cust_ro; mount -t ext4 -o ro,noload /dev/block/by-name/cust /data/local/tmp/cust_ro 2>/dev/null; cat /data/local/tmp/cust_ro/boot-log.txt; umount /data/local/tmp/cust_ro 2>/dev/null" > "$LOG"
  sz=$(stat -c %s "$LOG" 2>/dev/null || echo 0)
  echo "  attempt $attempt: $sz bytes"
  [ "$sz" -gt 20000 ] && break
  sleep 8
done
echo "  cust log bytes: $(stat -c %s "$LOG" 2>/dev/null || echo 0)"

echo
echo "=== 5. channel B: expdb tail ==="
rm -f "$T/e125.bin"
for attempt in 1 2 3; do
  adb_ exec-out su -c "dd if=/dev/block/by-name/expdb bs=1M skip=124 2>/dev/null" > "$T/e125.bin"
  sz=$(stat -c %s "$T/e125.bin" 2>/dev/null || echo 0)
  nz=$(python3 -c "
raw=open('$T/e125.bin','rb').read() if $sz else b''
print('%d' % (0 if not raw else 100*raw.count(0)//len(raw)))")
  echo "  attempt $attempt: $sz bytes, zero=$nz%"
  [ "$sz" -gt 100000 ] && [ "$nz" -lt 99 ] && break
  sleep 8
done

echo
echo "############ CHANNEL A: cust boot-log.txt ############"
python3 - "$LOG" "$EBL" "$T/e125.bin" <<'PY'
import re, sys
cust, ebl, edb = sys.argv[1], sys.argv[2], sys.argv[3]

def load(p):
    try:
        return open(p, 'rb').read()
    except Exception:
        return b''

c = load(cust)
print('cust log bytes: %d' % len(c))
if c:
    s = c.decode('latin-1', 'replace')
    for pat in ('corot-log: initramfs log-catcher started', 'cust found at', 'cust mounted rw',
                'COROT-MARKER', 'COROT-PROBE', 'COROT-DEFER', 'COROT-CLKPROBE', 'COROT-TOPCLK',
                'COROT-SNAP', 'COROT-STAGE', 'COROT-BC probe', 'Kernel panic', 'SError',
                'suspended=1', 'suspended=0', 'is_dual_pipe=1', 'is_dual_pipe=0',
                'tele ', 'corot-hb', 'reboot to bootloader'):
        print('  %-52s %d' % (pat, s.count(pat)))
    print('  --- markers:')
    for m in sorted(set(re.findall(r'COROT-MARKER [a-z0-9-]*', s))):
        print('      %s' % m)
    print('  --- which device mounted:')
    for line in s.split('\n'):
        if 'cust found at' in line or 'cust mounted' in line or 'NOT FOUND' in line:
            print('      %s' % line.strip()[:150])
    print('  --- COROT-TOPCLK / SNAP / STAGE sample:')
    for key in ('COROT-TOPCLK', 'COROT-SNAP', 'COROT-STAGE crtc_enable', 'COROT-CLKPROBE'):
        hits = [l for l in s.split('\n') if key in l][:6]
        for h in hits:
            print('      %s' % h.strip()[:165])
    print('  --- last 6 lines:')
    for l in [x for x in s.split('\n') if x.strip()][-6:]:
        print('      %s' % l.strip()[:165])

e = load(edb)
print()
print('expdb tail bytes: %d  zero=%d%%' % (len(e), 0 if not e else 100*e.count(0)//len(e)))
if e:
    print('  jump-to-kernel: %d  kernel_sz: %s  COROT lines: %d' % (
        e.count(b'jump to linux kernel'),
        sorted(set(x.decode() for x in re.findall(rb'kernel_sz=0x([0-9a-f]+)', e))),
        len(re.findall(rb'COROT-', e))))
PY
