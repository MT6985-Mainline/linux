#!/usr/bin/env bash
# r125: the decisive option-B power-on test, now with a working log channel.
#
# Configuration = the r122 minimum:
#   - the vendor clock list is reduced to its first entry (R115_MAX=1), so
#     dispsys_config gets `clock-num = <1>` plus the SAME single clock the node
#     always had  ->  mtk_drm_kms_init() can finally size top_clk[] and call
#     pm_runtime_get_sync(priv->mmsys_dev)
#   - the two MT6985 short-circuits in mtk_drm_top_clk_prepare_enable() /
#     disable_unprepare() are removed, so that call is actually reached
#   - r120/r121 breadcrumbs stay (COROT-CLKPROBE / COROT-TOPCLK / COROT-PROBE)
#
# What is new this time is the measurement channel: the initramfs log-catcher
# now finds cust by ext4 UUID (build_channel.sh), so this boot's full kernel log
# lands where Android can read it - no expdb ring, no ab_retry ambiguity.
#
# Judgement criteria (all from the cust boot-log.txt):
#   COROT-TOPCLK      -> what clock-num / top_clk_num the driver actually read
#   COROT-SNAP        -> suspended=0 means the display domain came out of suspend
#   COROT-STAGE crtc_enable: is_dual_pipe=?
#   SError / Kernel panic presence
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
for a in r120 r121; do
  printf '  %-6s ' "$a"
  run "$a" "/mnt/e/corot/.zcode/apply_$a.py"
  echo ok
done

sed -i -E 's/COROT-MARKER r[0-9a-z-]+/COROT-MARKER r125-clk1/g' "$K/mtk_dsi.c"
echo "  marker     : $(grep -c 'COROT-MARKER r125-clk1' $K/mtk_dsi.c)"
echo "  clock-num  : $(grep -oE 'clock-num = <[0-9]+>' arch/arm64/boot/dts/mediatek/mt6985-xiaomi-corot.dts)"
echo "  dispsys clk: $(grep -A3 'clock-num = <1>' arch/arm64/boot/dts/mediatek/mt6985-xiaomi-corot.dts | grep -c 'mmsys0_clk CLK_MM_CONFIG') (want 1)"
echo "  shorts     : $(grep -c 'DDPMSG(\"MT6985 bring-up: skip top-clock prepare' $K/mtk_drm_drv.c) (want 1 = comment only, call reached)"
echo "  clkprobe   : $(grep -c 'COROT-CLKPROBE' drivers/clk/mediatek/clk-mt6985-mmsys.c 2>/dev/null || echo 0)"

echo "=== build ==="
rm -f arch/arm64/boot/dts/mediatek/mt6985-xiaomi-corot.dtb
rm -f "$K"/*.o drivers/memory/*.o drivers/base/dd.o drivers/clk/mediatek/clk-mt6985-mmsys.o
make -j16 ARCH=arm64 LLVM=1 LLVM_IAS=1 Image mediatek/mt6985-xiaomi-corot.dtb > /tmp/r125-build.log 2>&1
rc=$?
echo "  BUILD_EXIT=$rc"
[ $rc -ne 0 ] && { grep -iE 'error:|Error:|undefined|No rule|dtc' /tmp/r125-build.log | head -20; exit 1; }

echo "  Image marker check: $(strings arch/arm64/boot/Image | grep -c 'COROT-MARKER r125-clk1')"
python3 /mnt/e/corot/pack_corot_images.py > /tmp/r125-pack.log 2>&1 && tail -4 /tmp/r125-pack.log
cp -f /home/mytiantian/corot-work/images/corot-boot.img      /mnt/c/Users/mytiantian/Desktop/corot-test-boot.img
cp -f /home/mytiantian/corot-work/images/corot-init_boot.img /mnt/c/Users/mytiantian/Desktop/corot-test-init_boot.img
echo "  staged: $(stat -c %s /mnt/c/Users/mytiantian/Desktop/corot-test-boot.img) + $(stat -c %s /mnt/c/Users/mytiantian/Desktop/corot-test-init_boot.img)"
