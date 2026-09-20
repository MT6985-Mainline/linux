#!/usr/bin/env bash
# Scan the WHOLE expdb partition in 4MiB chunks for COROT ring remnants.
set -u
A=/mnt/d/platform-tools/adb.exe
S=UGEEOVYX4TZ9EE45
T=/home/mytiantian/corot-work/.flash2/scan
mkdir -p "$T"

for i in $(seq 1 20); do "$A" devices 2>/dev/null | grep -q "$S" && break; sleep 5; done

for off in 0 4 8 12 16 20 24 28 32 36 40 44 48 52 56 60 64 68 72 76 80 84 88 92 96 100 104 108 112 116 120 124; do
  F="$T/chunk_$off.bin"
  [ -s "$F" ] && sz=$(stat -c %s "$F") || sz=0
  if [ "$sz" -lt 4000000 ]; then
    "$A" -s "$S" exec-out su -c "dd if=/dev/block/by-name/expdb bs=1M skip=$off count=4 2>/dev/null" > "$F" 2>/dev/null
    sz=$(stat -c %s "$F" 2>/dev/null || echo 0)
  fi
  info=$(python3 - "$F" <<'PY'
import sys, re
try:
    raw = open(sys.argv[1], 'rb').read()
except Exception:
    print('unreadable'); raise SystemExit
z = 100.0*raw.count(0)/max(1, len(raw))
c = raw.count(b'COROT')
m = sorted(set(re.findall(rb'COROT-MARKER ([a-z0-9-]+)', raw)))
k = raw.count(b'jump to linux kernel')
print('%d B  zero=%4.1f%%  COROT=%d  jumps=%d  markers=%s' % (len(raw), z, c, k, [x.decode() for x in m]))
PY
)
  echo "skip=${off}MiB : $info"
done
