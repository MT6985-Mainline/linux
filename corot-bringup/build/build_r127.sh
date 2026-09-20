#!/usr/bin/env bash
# r127: keep the display power state exactly as LK hands it over.
#
# The ring (r126's dump) shows the sequence that kills option B:
#     COROT-SNAP[before-first-enable]  ovl_en=0x00000006 dsc_con=0x00010089 dsi_start=0x1  <- LK handoff, ALIVE
#     COROT-SNAP[after-kms-init]       ovl_en=0x00000000 dsc_con=0x00000000 dsi_start=0x0  <- switched OFF
#     COROT-STAGE crtc_enable: is_dual_pipe=1                       <- r107's helper table works
#     COROT[crtc_enable_start] MUTEX/OVL0/DSC/DSI/DSIBUF = 0x00000000
#     -> async SError on the now-unpowered MIPI TX
#
# The release at the end of kms_init comes from r108, which restored the vendor's
# line verbatim.  The vendor's comment above that line is the whole point:
#     "we power off mtcmos at the end of the display initialization.
#      Here we only decrease ref count, THE POWER WILL HOLD ON."
# In the vendor tree another reference keeps the domain up; in this port the
# refcount reaches 0, pm_runtime_put_sync() runs, and the DIS0 domain really does
# power off - then the next crtc_enable pokes an unpowered block.
#
# So this round is r126 (deferred power-up: leave the hardware as LK left it)
# WITHOUT r108's release.  That is also exactly the state the known-good
# CPU-direct builds ran in for 279 s.
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

# NOTE: r108 is deliberately absent - it is the patch that added the release.
for a in r92 r102 r103 r104 r107 r109 r110 r112 r113; do
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

sed -i -E 's/COROT-MARKER r[0-9a-z-]+/COROT-MARKER r127-keepon/g' "$K/mtk_dsi.c"
echo "  marker          : $(grep -c 'COROT-MARKER r127-keepon' $K/mtk_dsi.c)"
echo "  clock-num       : $(grep -oE 'clock-num = <[0-9]+>' arch/arm64/boot/dts/mediatek/mt6985-xiaomi-corot.dts)"
echo "  r108 release    : $(grep -c 'COROT r108' $K/mtk_drm_drv.c) (want 0 = not applied)"
echo "  kms_init release: $(grep -c 'mtk_drm_top_clk_disable_unprepare(drm);' $K/mtk_drm_drv.c) (occurrences in file)"
echo "  deferrals       : $(grep -c 'COROT r126' $K/mtk_drm_drv.c) (want 3)"

echo "=== build ==="
rm -f arch/arm64/boot/dts/mediatek/mt6985-xiaomi-corot.dtb
rm -f "$K"/*.o drivers/memory/*.o drivers/base/dd.o drivers/clk/mediatek/clk-mt6985-mmsys.o
make -j16 ARCH=arm64 LLVM=1 LLVM_IAS=1 Image mediatek/mt6985-xiaomi-corot.dtb > /tmp/r127-build.log 2>&1
rc=$?
echo "  BUILD_EXIT=$rc"
[ $rc -ne 0 ] && { grep -iE 'error:|Error:|undefined|No rule|dtc' /tmp/r127-build.log | head -20; exit 1; }

echo "  Image marker: $(strings arch/arm64/boot/Image | grep -c 'COROT-MARKER r127-keepon')"
python3 /mnt/e/corot/pack_corot_images.py > /tmp/r127-pack.log 2>&1 && tail -3 /tmp/r127-pack.log
echo "  staged ok"
