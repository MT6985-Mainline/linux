#!/usr/bin/env bash
# r126: r125 configuration (clock-num + real power-up path) but with the power-up
# deferred to userspace, so the log channel finally sees whatever happens.
set -u
cd /home/mytiantian/linux-corot || exit 1
export PATH=/usr/lib/llvm-22/bin:/usr/bin:/bin
K=drivers/gpu/drm/mediatek/mediatek_v2

run() {
  python3 "$2" > "/tmp/$1.log" 2>&1; rc=$?
  [ $rc -ne 0 ] && { echo "  $2 FAILED"; cat "/tmp/$1.log"; exit 1; }
  return 0
}

git checkout HEAD -- drivers/ arch/ include/ .config
rm -f include/dt-bindings/memory/mt6985-larb-port.h

for a in r92 r102 r103 r104 r107 r108 r109 r110 r112 r113; do
  printf '  %-6s ' "$a"
  run "$a" "/mnt/e/corot/.zcode/apply_$a.py"
  echo ok
done
printf '  %-6s ' "r115(clk1)"
R115_MAX=1 run r115 /mnt/e/corot/.zcode/apply_r115.py
echo ok
for a in r120 r121 r126; do
  printf '  %-6s ' "$a"
  run "$a" "/mnt/e/corot/.zcode/apply_$a.py"
  echo ok
done

sed -i -E 's/COROT-MARKER r[0-9a-z-]+/COROT-MARKER r126-defer/g' "$K/mtk_dsi.c"
echo "  marker     : $(grep -c 'COROT-MARKER r126-defer' $K/mtk_dsi.c)"
echo "  clock-num  : $(grep -oE 'clock-num = <[0-9]+>' arch/arm64/boot/dts/mediatek/mt6985-xiaomi-corot.dts)"
echo "  deferrals  : $(grep -c 'COROT-TOPCLK' $K/mtk_drm_drv.c) (want 3)"
echo "  shorts off : $(grep -c 'skip top-clock prepare until clocks are wired' $K/mtk_drm_drv.c) (want 0 = removed)"

echo "=== build ==="
rm -f arch/arm64/boot/dts/mediatek/mt6985-xiaomi-corot.dtb
rm -f "$K"/*.o drivers/memory/*.o drivers/base/dd.o drivers/clk/mediatek/clk-mt6985-mmsys.o
make -j16 ARCH=arm64 LLVM=1 LLVM_IAS=1 Image mediatek/mt6985-xiaomi-corot.dtb > /tmp/r126-build.log 2>&1
rc=$?
echo "  BUILD_EXIT=$rc"
[ $rc -ne 0 ] && { grep -iE 'error:|Error:|undefined|No rule|dtc' /tmp/r126-build.log | head -20; exit 1; }

echo "  Image marker: $(strings arch/arm64/boot/Image | grep -c 'COROT-MARKER r126-defer')"
python3 /mnt/e/corot/pack_corot_images.py > /tmp/r126-pack.log 2>&1 && tail -3 /tmp/r126-pack.log
echo "  staged ok"
