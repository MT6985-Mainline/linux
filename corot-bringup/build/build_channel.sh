#!/usr/bin/env bash
# r125 step 1: rebuild the log-catcher initramfs with UUID-based cust detection
# (the measurement channel fix), and stage it into the pack directory.
set -u
export PATH=/usr/bin:/bin
OUT=/home/mytiantian/corot-work/corot-initramfs
MASTER=/mnt/e/corot/corot-initramfs/init-log.c
ROOT=$OUT/root

echo "=== 1. sanity-check the master source ==="
grep -c 'find_cust' "$MASTER" | sed 's/^/  find_cust refs: /'
grep -c 'CUST_UUID' "$MASTER" | sed 's/^/  CUST_UUID refs: /'
grep -o 'hb / 2 >= [0-9]*' "$MASTER" | head -1 | sed 's/^/  teardown window: /'
grep -c '/dev/sdc80' "$MASTER" | sed 's/^/  hard-coded sdc80 refs: /'

echo
echo "=== 2. compile (proven flags) ==="
mkdir -p "$ROOT/dev" "$ROOT/proc" "$ROOT/sys" "$ROOT/newroot"
cp -f "$MASTER" "$OUT/init-log.c"
aarch64-linux-gnu-gcc -nostdlib -static -fno-stack-protector -fno-builtin \
  -ffreestanding -Os -Wl,-e,_start -o "$ROOT/init" "$OUT/init-log.c" 2>&1 | head -20
if [ ! -s "$ROOT/init" ]; then echo "  COMPILE FAILED"; exit 1; fi
echo "  root/init: $(stat -c %s "$ROOT/init") bytes"
strings "$ROOT/init" | grep -c 'corot-log: initramfs log-catcher started' | sed 's/^/  catcher string present: /'
strings "$ROOT/init" | grep -m2 'cust found at' | sed 's/^/  /'

echo
echo "=== 3. pack cpio + lz4 ==="
( cd "$ROOT" && find . -print | cpio -o -H newc ) > "$OUT/initramfs.cpio" 2>/dev/null || exit 1
lz4 -l -9 -f "$OUT/initramfs.cpio" "$OUT/initramfs.cpio.lz4" >/dev/null || exit 1
echo "  cpio.lz4: $(stat -c %s "$OUT/initramfs.cpio.lz4") bytes"
rm -rf /tmp/irverify && mkdir -p /tmp/irverify
lz4 -d "$OUT/initramfs.cpio.lz4" /tmp/irverify/ir.cpio >/dev/null 2>&1
strings -a /tmp/irverify/ir.cpio | grep -m1 'cust found at' | sed 's/^/  verify in packed ramdisk: /'
echo "  staged ok (run build_r125.sh next to pack it with the kernel)"
