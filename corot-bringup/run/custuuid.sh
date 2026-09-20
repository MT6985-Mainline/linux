#!/usr/bin/env bash
# 1) read cust's ext4 UUID (to bake into the initramfs), 2) check build tools
set -u
A=/mnt/d/platform-tools/adb.exe
S=UGEEOVYX4TZ9EE45
for i in $(seq 1 30); do "$A" devices 2>/dev/null | grep -q "$S" && break; sleep 5; done
echo "=== adb: $("$A" devices | tr -d '\r' | tail -1)"
echo "=== cust size + superblock (offset 1024, 1KB):"
"$A" -s "$S" exec-out su -c "blockdev --getsize64 /dev/block/by-name/cust; dd if=/dev/block/by-name/cust bs=1024 skip=1 count=1 2>/dev/null | od -A d -t x1 | head -12" 2>&1 | tr -d '\r'
echo
echo "=== tools:"
for t in aarch64-linux-gnu-gcc cpio lz4 python3; do
  printf '  %-24s %s\n' "$t" "$(command -v $t || echo MISSING)"
done
