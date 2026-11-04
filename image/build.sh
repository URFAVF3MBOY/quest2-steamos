#!/bin/sh
# build.sh - builds the Quest 2 SteamOS boot image from this folder.
#
#   sh build.sh              uses the compiled helpers in prebuilt/
#   REBUILD=1 sh build.sh    also recompiles the tracking tools, the SteamVR driver and the pose reader from src/
#                            (needs clang + lld; a different clang version gives different bytes, same behaviour)
#   USB=1 sh build.sh        image that boots SteamOS from a USB drive instead of the internal storage (first install)
#
# Needs these files first (see docs/BUILDING.md):
#   base/boot_steamos_desk204_phaseb4_caps_mmu.img   base boot image (from the GitHub release): boot header, Meta's
#                               device tree, small Alpine ramdisk.  Frozen - its SHA-256 is in inputs.sha256.
#   art/boot_steamos.img        kernel           (workflow build-quest2-steamos, artifact quest2-steamos)
#   art/q2modules.tar.gz        Wi-Fi driver     (same artifact)
#   art/q2audio.tar.gz          sound modules    (same workflow, artifact quest2-audio; optional - no sound without it)
#   art/q2gpu.tar.gz            GPU userspace    (workflow build-q2gpu, artifact q2gpu-aarch64)
#   art/q2msm.ko                GPU kernel driver (workflow build-q2msm, artifact q2msm: kernel/q2msm/q2msm.ko)
#   art/fuse-overlayfs          for Android apps (workflow build-fuse-overlayfs)
#   art/q2gpu-android.tar.gz    optional: GPU driver for Android apps (workflow build-q2gpu-android)
#
# Output: out/boot_steamos.img (or out/boot_steamos_usb.img with USB=1) - flash it to the boot partition only.
# Never flash abl or xbl.
set -e
cd "$(dirname "$0")"
BASE=${1:-base/boot_steamos_desk204_phaseb4_caps_mmu.img}
for f in "$BASE" art/boot_steamos.img art/q2modules.tar.gz art/q2gpu.tar.gz art/q2msm.ko art/fuse-overlayfs; do
  [ -f "$f" ] || { echo "missing $f - see docs/BUILDING.md"; exit 1; }
done
echo "== base image check"
if [ "$(sha256sum "$BASE" | cut -c1-64)" = "$(cut -c1-64 inputs.sha256)" ]; then echo "base: the released base image"
else echo "base: NOT the released base image ($BASE) - the device-tree patch below may refuse it"; fi

rm -rf out; mkdir -p out/island
if [ -n "$REBUILD" ]; then
  CF="--target=aarch64-linux-gnu -O2 -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -fuse-ld=lld -nostdinc -isystem $(clang -print-resource-dir)/include -Isrc/q2cam/inc -Isrc/q2cam -Wno-#warnings"
  for p in q2cam_tpg q2camlive_main:q2camlive q2blobs q2ctltrack; do s=${p%%:*}; d=${p##*:}; [ "$d" = "$p" ] && d=$s
    echo "== $d"; clang $CF -o out/$d src/q2cam/$s.c; done
  echo "== driver_q2hmd.so"
  clang --target=aarch64-linux-gnu -shared -nostdlib -fuse-ld=lld -Wl,-soname,libc.so.6 -o out/libc.so.6 src/hmd/libcstub.c
  clang++ --target=aarch64-linux-gnu -O2 -fPIC -shared -nostdlib -ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-math-errno \
    -fno-stack-protector -fvisibility=hidden -std=c++17 -isystem src/hmd/stub -Isrc/hmd -fuse-ld=lld -Wl,--version-script=src/hmd/exports.map \
    -Wl,-soname,driver_q2hmd.so -o out/driver_q2hmd.so src/hmd/driver_q2hmd.cpp out/libc.so.6
  echo "== q2metapose"
  clang --target=aarch64-linux-gnu -O2 -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -fuse-ld=lld -fno-math-errno \
    -o out/island/q2metapose src/island/metapose.c
  # Android-side helpers (sources in src/island: shim.c = libq2island.so, host.c = q2islandhost) stay prebuilt here
  cp prebuilt/island/libq2island.so prebuilt/island/q2islandhost prebuilt/island/q2tsfind out/island/
else
  cp prebuilt/q2cam_tpg prebuilt/q2camlive prebuilt/q2blobs prebuilt/q2ctltrack prebuilt/driver_q2hmd.so out/
  cp prebuilt/island/* out/island/
fi
sh -n src/init/init

# USB=1: SteamOS from the USB drive instead of the internal "userdata" partition (first install, docs/INSTALL.md)
OUT=out/boot_steamos.img; USBROOT=""
[ -n "$USB" ] && { OUT=out/boot_steamos_usb.img; USBROOT="--cmdline-drop q2_rootdev"; echo "== USB=1: SteamOS from the USB drive"; }
OPT=""
if [ -f art/q2gpu-android.tar.gz ]; then
  OPT="$OPT --file art/q2gpu-android.tar.gz:usr/share/q2gpu-android.tar.gz:644"
  echo "== including art/q2gpu-android.tar.gz (GPU driver for Android apps)"
fi
if [ -f art/q2audio.tar.gz ]; then
  OPT="$OPT --file art/q2audio.tar.gz:usr/share/q2audio.tar.gz:644"
  echo "== including art/q2audio.tar.gz (sound)"
else echo "== art/q2audio.tar.gz missing - the image will have no sound"; fi
echo "== image"
python3 tools/inject_boot.py --in "$BASE" --out out/layered.img --kernel art/boot_steamos.img \
  --file art/q2msm.ko:usr/lib/q2/q2msm.ko:644 --file prebuilt/q2btattach:usr/share/q2vr/q2btattach:755 \
  --file src/q2gpucheck.sh:usr/share/q2vr/q2gpucheck:755 --file src/q2vrpaneltest.sh:usr/share/q2vr/q2vrpaneltest:755 \
  --file art/q2gpu.tar.gz:usr/share/q2gpu.tar.gz:644 --file art/q2modules.tar.gz:usr/share/q2modules.tar.gz:644 --file art/fuse-overlayfs:usr/share/q2fuse/fuse-overlayfs:755 \
  --file src/init/init:init:755 --file prebuilt/q2pcsample:usr/share/q2vr/q2pcsample:755 --file prebuilt/libq2vrdisplay.so:usr/share/q2vr/libq2vrdisplay.so:755 --file prebuilt/cabextract:usr/share/q2vr/cabextract:755 --file src/prof/q2ucrt:usr/share/q2vr/q2ucrt:755 --file src/prof/q2rubyfix:usr/share/q2vr/q2rubyfix:755 \
  --file out/q2cam_tpg:usr/share/q2vr/q2camtpg:755 --file src/q2cam/q2stockcam.py:usr/share/q2vr/q2stockcam.py:755 \
  --file out/q2camlive:usr/share/q2vr/q2camlive:755 --file out/q2blobs:usr/share/q2vr/q2blobs:755 \
  --file out/q2ctltrack:usr/share/q2vr/q2ctltrack:755 --file src/q2cam/q2camcalib.txt:usr/share/q2vr/q2camcalib.txt:644 \
  --file out/driver_q2hmd.so:usr/share/q2vr/driver_q2hmd.so:755 \
  --file src/q2cam/q2islandprobe.py:usr/share/q2vr/q2islandprobe.py:755 \
  --file out/island/libq2island.so:usr/share/q2vr/island/libq2island.so:755 --file out/island/q2islandhost:usr/share/q2vr/island/q2islandhost:755 \
  --file out/island/q2tsfind:usr/share/q2vr/island/q2tsfind:755 --file src/island/q2islandmount.py:usr/share/q2vr/island/q2islandmount.py:755 \
  --file src/island/q2island.sh:usr/share/q2vr/island/q2island.sh:755 --file src/island/q2island-inner.sh:usr/share/q2vr/island/q2island-inner.sh:755 \
  --file src/island/q2island-wait.sh:usr/share/q2vr/island/q2island-wait.sh:755 --file src/island/q2logd.py:usr/share/q2vr/island/q2logd.py:755 \
  --file src/island/q2vintf.py:usr/share/q2vr/island/q2vintf.py:755 --file out/island/q2metapose:usr/share/q2vr/island/q2metapose:755 \
  --file src/island/q2ctlinput.py:usr/share/q2vr/island/q2ctlinput:755 \
  $OPT
# one ramdisk layer instead of the base's seven + ours (smaller, and newest file wins)
python3 tools/flatten_boot.py --in out/layered.img --out out/flat.img
# desk-334: stock 587 MHz GPU level in the device tree
python3 tools/patch_dtb.py --in out/flat.img --out out/dtb.img
# kernel command line: q2msm instead of KGSL (KGSL never probes), quiet console, VR starts by itself; desk-382: the
# display driver uses Meta's JDI 90 Hz panel entry (the timing the bootloader really runs) instead of the dtbo's 144 Hz one
python3 tools/rebase_boot.py --base out/dtb.img --ramdisk-from out/dtb.img --out $OUT \
  --cmdline-drop q2_vrauto --cmdline-drop kgsl.force_lazy_gpumem --cmdline-set loglevel=4 \
  --cmdline-add initcall_blacklist=kgsl_3d_init --cmdline-add msm_drm.q2_dsi_display0=qcom,mdss_dsi_jdi_4k_90Hz_dsc_video: $USBROOT
rm -f out/layered.img out/flat.img out/dtb.img
SIZE=$(stat -c %s $OUT)
[ "$SIZE" -le 83886080 ] || { echo "image is $SIZE bytes - larger than the 80 MiB boot partition"; exit 1; }
echo "== result"
sha256sum $OUT
echo "done: $OUT - flash it to the boot partition only (never abl/xbl)"
