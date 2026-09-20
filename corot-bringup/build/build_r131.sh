#!/usr/bin/env bash
# r131: r130 + the MT6985 dual-pipe mapping + the NULL guard that stops the oops.
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

for a in r92 r102 r103 r104 r107 r109 r110 r112 r113; do
  printf '  %-6s ' "$a"
  run "$a" "/mnt/e/corot/.zcode/apply_$a.py"
  echo ok
done
printf '  %-6s ' "r115(clk1)"
R115_MAX=1 run r115 /mnt/e/corot/.zcode/apply_r115.py
echo ok
for a in r120 r121 r129 r130 r131; do
  printf '  %-6s ' "$a"
  run "$a" "/mnt/e/corot/.zcode/apply_$a.py"
  echo ok
done

sed -i -E 's/COROT-MARKER r[0-9a-z-]+/COROT-MARKER r131-dualmap/g' "$K/mtk_dsi.c"
echo "  marker      : $(grep -c 'COROT-MARKER r131-dualmap' $K/mtk_dsi.c)"
echo "  mt6985 map  : $(grep -c 'dual_comp_map_mt6985' $K/mtk_drm_crtc.c) (want 2)"
echo "  NULL guard  : $(grep -c 'no dual-pipe counterpart' $K/mtk_drm_crtc.c) (want 1)"
echo "  MT6985 case : $(grep -c 'ret = dual_comp_map_mt6985(comp_id);' $K/mtk_drm_crtc.c) (want 1)"

echo "=== build ==="
rm -f arch/arm64/boot/dts/mediatek/mt6985-xiaomi-corot.dtb
rm -f "$K"/*.o drivers/memory/*.o drivers/base/dd.o drivers/clk/mediatek/clk-mt6985-mmsys.o
make -j16 ARCH=arm64 LLVM=1 LLVM_IAS=1 Image mediatek/mt6985-xiaomi-corot.dtb > /tmp/r131-build.log 2>&1
rc=$?
echo "  BUILD_EXIT=$rc"
[ $rc -ne 0 ] && { grep -iE 'error:|Error:|undefined|No rule|dtc' /tmp/r131-build.log | head -20; exit 1; }

echo "  Image marker: $(strings arch/arm64/boot/Image | grep -c 'COROT-MARKER r131-dualmap')"
python3 /mnt/e/corot/pack_corot_images.py > /tmp/r131-pack.log 2>&1 && tail -3 /tmp/r131-pack.log
echo "  staged ok"
