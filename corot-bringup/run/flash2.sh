#!/usr/bin/env bash
# flash2.sh - single-shot test with STATE HYGIENE and BOOT VERIFICATION.
#
# Why: several rounds came back with no log at all, including r118, whose kernel
# is functionally identical to r113 (which logged fine).  A round that "did not
# log" is only meaningful if we know the kernel was actually started - and after
# a failed boot this device can sit in fastboot, where LK never jumps to the
# kernel.  So, every round:
#
#   1. recover the device out of fastboot into Android first (clean state);
#   2. count LK's "jump to linux kernel" lines in expdb BEFORE flashing;
#   3. flash, boot, watch;
#   4. count them again AFTER - if the count did not grow, the kernel was never
#      started and the round produced no data at all.
#
# Usage: flash2.sh <logpath> [watch-seconds]
set -u
A=/mnt/d/platform-tools/adb.exe
F=/mnt/d/platform-tools/fastboot.exe
S=UGEEOVYX4TZ9EE45
R='E:\corot\stock-restore-current'
D=/mnt/c/Users/mytiantian/Desktop
IMG=/home/mytiantian/corot-work/images
L=${1:-/home/mytiantian/corot-work/r-current-bootlog.txt}
WATCH=${2:-150}
TMP=/home/mytiantian/corot-work/.flash2

mkdir -p "$TMP"

jumps() {   # count LK kernel jumps currently recorded in expdb
  "$A" -s "$S" shell su -c "dd if=/dev/block/by-name/expdb bs=1M skip=124 2>/dev/null" \
    > "$TMP/e.bin" 2>/dev/null
  if [ ! -s "$TMP/e.bin" ]; then echo "n/a"; return; fi
  c=$(grep -ac 'jump to linux kernel' "$TMP/e.bin" 2>/dev/null)
  echo "${c:-0}"
}

wait_android() {
  for i in $(seq 1 36); do
    bc=$("$A" -s "$S" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')
    [ "$bc" = "1" ] && return 0
    sleep 10
  done
  return 1
}

echo "=== 0. device state ==="
if "$F" devices 2>/dev/null | grep -q "$S"; then
  echo "  in fastboot - rebooting to Android for a clean state"
  "$F" -s "$S" reboot >/dev/null 2>&1
  sleep 30
fi
for i in $(seq 1 36); do "$A" devices 2>/dev/null | grep -q "$S" && break; sleep 5; done
if wait_android; then echo "  Android is up (clean state)"; else echo "  WARNING: Android never came up"; fi

BEFORE=$(jumps)
echo "  LK kernel jumps seen before the test: $BEFORE"

echo
echo "=== 1. stage + flash the test build ==="
cp -f "$IMG/corot-boot.img"      "$D/corot-test-boot.img"      || exit 1
cp -f "$IMG/corot-init_boot.img" "$D/corot-test-init_boot.img" || exit 1
echo "  boot.img $(stat -c %s "$D/corot-test-boot.img") bytes"
"$A" -s "$S" reboot bootloader >/dev/null 2>&1
for i in $(seq 1 24); do "$F" devices 2>/dev/null | grep -q "$S" && break; sleep 5; done
"$F" devices 2>/dev/null | tr -d '\r' | sed 's/^/  /'
"$F" -s "$S" flash boot_ab    'C:\Users\mytiantian\Desktop\corot-test-boot.img'      >/dev/null 2>&1 || { echo "  boot_ab flash FAILED"; exit 1; }
"$F" -s "$S" flash init_boot_a 'C:\Users\mytiantian\Desktop\corot-test-init_boot.img' >/dev/null 2>&1 || { echo "  init_boot_a FAILED"; exit 1; }
"$F" -s "$S" flash init_boot_b 'C:\Users\mytiantian\Desktop\corot-test-init_boot.img' >/dev/null 2>&1 || { echo "  init_boot_b FAILED"; exit 1; }
"$F" -s "$S" set_active a >/dev/null 2>&1
"$F" -s "$S" reboot >/dev/null 2>&1
echo "  flashed + rebooting; watching ${WATCH}s"

t=0
while [ $t -lt "$WATCH" ]; do
  sleep 10; t=$((t+10))
  "$A" devices 2>/dev/null | grep -q "$S" && { echo "  t=${t}s adb (Android came up!)"; break; }
  "$F" devices 2>/dev/null | grep -q "$S" && { echo "  t=${t}s fastboot (test kernel ended)"; break; }
  [ $((t % 60)) -eq 0 ] && echo "  t=${t}s still running"
done
[ $t -ge "$WATCH" ] && echo "  t=${WATCH}s window over (still running)"

echo
echo "=== 2. did LK actually start the kernel? ==="
AFTER=$(jumps)
echo "  jumps before=$BEFORE after=$AFTER"
if [ "$BEFORE" = "n/a" ] || [ "$AFTER" = "n/a" ]; then
  echo "  --> could not read expdb; boot verification skipped"
elif [ "$AFTER" -gt "$BEFORE" ]; then
  echo "  --> KERNEL WAS STARTED this round (measurement is valid)"
else
  echo "  --> no new LK jump: the kernel was probably NOT started"
  echo "      (this device stops booting after repeated failures and parks in"
  echo "       fastboot; a successful stock boot resets that, which step 0 does)"
fi
grep -a 'kernel_sz=' "$TMP/e.bin" 2>/dev/null | tail -3 | sed 's/^/    /'

echo
echo "=== 3. restore stock ==="
for i in $(seq 1 30); do "$F" devices 2>/dev/null | grep -q "$S" && break; sleep 5; done
if ! "$F" devices 2>/dev/null | grep -q "$S"; then
  echo "  not in fastboot - forcing it"
  "$A" -s "$S" reboot bootloader >/dev/null 2>&1
  for i in $(seq 1 24); do "$F" devices 2>/dev/null | grep -q "$S" && break; sleep 5; done
fi
"$F" -s "$S" flash boot_ab        "$R\\boot_a.img"        >/dev/null 2>&1
"$F" -s "$S" flash vendor_boot_ab "$R\\vendor_boot_a.img" >/dev/null 2>&1
"$F" -s "$S" flash init_boot_a    "$R\\init_boot_a.img"    >/dev/null 2>&1
"$F" -s "$S" flash init_boot_b    "$R\\init_boot_b.img"    >/dev/null 2>&1
"$F" -s "$S" set_active a >/dev/null 2>&1
"$F" -s "$S" reboot >/dev/null 2>&1
rm -f "$D/corot-test-boot.img" "$D/corot-test-init_boot.img"
echo "  stock restored"

echo
echo "=== 4. read the ring ==="
for i in $(seq 1 36); do "$A" devices 2>/dev/null | grep -q "$S" && break; sleep 10; done
if ! wait_android; then
  echo "  Android did not come up - rebooting out of fastboot"
  "$F" devices 2>/dev/null | grep -q "$S" && { "$F" -s "$S" reboot >/dev/null 2>&1; sleep 40; }
  wait_android
fi
# exec-out, because `adb shell` translates \n to \r\n and corrupts the readback.
# Retry, and REFUSE to interpret an empty dump: an empty dump is a read failure,
# not a silent kernel.  That mistake cost several rounds.
rm -f "$TMP/e.bin"
for attempt in 1 2 3 4 5; do
  "$A" -s "$S" exec-out su -c "dd if=/dev/block/by-name/expdb bs=1M skip=124 2>/dev/null" \
    > "$TMP/e.bin" 2>/dev/null
  sz=$(stat -c %s "$TMP/e.bin" 2>/dev/null || echo 0)
  [ "$sz" -gt 100000 ] && break
  echo "  read attempt $attempt gave $sz bytes - retrying"
  sleep 15
done
sz=$(stat -c %s "$TMP/e.bin" 2>/dev/null || echo 0)
echo "  dump bytes: $sz"
if [ "$sz" -lt 100000 ]; then
  echo "  *** EXPDB READ FAILED - this round produced no usable data ***"
  exit 1
fi
python3 - "$TMP/e.bin" "$L" <<'PY'
import re, sys
raw = open(sys.argv[1], 'rb').read()
out = sys.argv[2]
keep = re.findall(rb'COROT[^\r\n\x01]{0,180}', raw)
open(out, 'wb').write(b'\n'.join(keep))
open(out, 'ab').write(b'\n')
print('  jumps to kernel : %d' % raw.count(b'jump to linux kernel'))
for k in sorted(set(re.findall(rb'kernel_sz=0x([0-9a-f]+)', raw))):
    print('  kernel_sz 0x%s = %d' % (k.decode(), int(k, 16)))
print('  COROT lines kept: %d -> %s' % (len(keep), out))
mk = sorted(set(re.findall(rb'COROT-MARKER ([a-z0-9-]+)', raw)))
print('  markers:', [m.decode() for m in mk])
print('  PROBE %d  DEFER %d  CLKPROBE %d  TOPCLK %d  SNAP %d  INITPWR %d  panic %d' % (
    raw.count(b'COROT-PROBE'), raw.count(b'COROT-DEFER'),
    raw.count(b'COROT-CLKPROBE'), raw.count(b'COROT-TOPCLK'),
    raw.count(b'COROT-SNAP'), raw.count(b'COROT-INITPWR'),
    raw.count(b'Kernel panic')))
PY
echo
echo "############ WHICH ROUND ############"
grep -ao 'COROT-MARKER [a-z0-9-]*' "$L" | sort | uniq -c