#!/usr/bin/env bash
# r128: hold the display power domain up, the way the vendor's comment says.
#
# r127's ring pinned the mechanism down:
#   [before-first-enable] ovl_en=0x6 dsc_con=0x00010089 dsi_start=0x1   <- LK handoff alive
#   [after-kms-init]      ovl_en=0x0 dsc_con=0x0        dsi_start=0x0   <- cut in that window
#   crtc_enable: is_dual_pipe=1 -> all-zero register dump -> DSI access
#   -> "Kernel panic - not syncing: Asynchronous SError Interrupt"
# and COROT-SNAP reports suspended=1 from bind-in onwards.
#
# That is the genpd: r107 attached `power-domains` to dispsys-config, the driver
# enables runtime PM, and once no reference is held the core suspends the device
# and the DIS0 domain really powers off - the LK scanout dies with it.  The
# vendor's own comment on the power path says what must happen instead:
#   "we power on mtcmos at the beginning of the display initialization.
#    ... Here we only decrease ref count, THE POWER WILL HOLD ON."
#
# So: take the reference (real power-up, no deferral) and do NOT release it at
# the end of kms_init.  r125 was "take + release" (crashes), r127 was
# "neither" (domain dies anyway, crashes), this is "take and hold".
#
# Chain: r92..r113 + r115(clk1) + r120 + r121, WITHOUT r108 (its release) and
# WITHOUT r126 (the deferral).
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
for a in r120 r121; do
  printf '  %-6s ' "$a"
  run "$a" "/mnt/e/corot/.zcode/apply_$a.py"
  echo ok
done

sed -i -E 's/COROT-MARKER r[0-9a-z-]+/COROT-MARKER r128-hold/g' "$K/mtk_dsi.c"
echo "  marker           : $(grep -c 'COROT-MARKER r128-hold' $K/mtk_dsi.c)"
echo "  clock-num        : $(grep -oE 'clock-num = <[0-9]+>' arch/arm64/boot/dts/mediatek/mt6985-xiaomi-corot.dts)"
echo "  r108 release     : $(grep -c 'COROT r108' $K/mtk_drm_drv.c) (want 0)"
echo "  deferral (r126)  : $(grep -c 'COROT r126' $K/mtk_drm_drv.c) (want 0)"
echo "  power-up calls   : $(grep -c 'pm_runtime_get_sync(priv->mmsys_dev);' $K/mtk_drm_drv.c) (want >=2: get_top_clk + prepare_enable)"
echo "  shorts removed   : $(grep -c 'DDPMSG("MT6985 bring-up: skip top-clock' $K/mtk_drm_drv.c) (want 0)"

echo "=== build ==="
rm -f arch/arm64/boot/dts/mediatek/mt6985-xiaomi-corot.dtb
rm -f "$K"/*.o drivers/memory/*.o drivers/base/dd.o drivers/clk/mediatek/clk-mt6985-mmsys.o
make -j16 ARCH=arm64 LLVM=1 LLVM_IAS=1 Image mediatek/mt6985-xiaomi-corot.dtb > /tmp/r128-build.log 2>&1
rc=$?
echo "  BUILD_EXIT=$rc"
[ $rc -ne 0 ] && { grep -iE 'error:|Error:|undefined|No rule|dtc' /tmp/r128-build.log | head -20; exit 1; }

echo "  Image marker: $(strings arch/arm64/boot/Image | grep -c 'COROT-MARKER r128-hold')"
python3 /mnt/e/corot/pack_corot_images.py > /tmp/r128-pack.log 2>&1 && tail -3 /tmp/r128-pack.log
echo "  staged ok"
