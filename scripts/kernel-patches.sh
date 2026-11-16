#!/bin/bash
# Apply this port's patches to Meta's Quest 2 kernel source. Run inside the kernel source tree:
#   bash <repo>/scripts/kernel-patches.sh <repo>
# Shared by build-steamos.yml and the kexec kernel build, so both kernels carry the same changes.
set -euo pipefail
P="$1/patches"
# the fixes patch is per kernel generation: try the newer-commit patch, then the older-commit one
if git apply --check "$P/oculus-kernel-fixes.patch" 2>/dev/null; then
  git apply "$P/oculus-kernel-fixes.patch"; echo "applied oculus-kernel-fixes.patch"
else
  git apply --check "$P/oculus-kernel-fixes-old.patch"
  git apply "$P/oculus-kernel-fixes-old.patch"; echo "applied oculus-kernel-fixes-old.patch"
fi
# newer kernel only: swap in the older SyncBoss driver, whose DT bindings (oculus,syncboss-*) match the headset's device tree
if git apply --check "$P/oculus-syncboss-old-driver.patch" 2>/dev/null; then
  git apply "$P/oculus-syncboss-old-driver.patch"; echo "applied oculus-syncboss-old-driver.patch"
else
  echo "syncboss patch not applicable to this kernel commit, skipping"
fi
# GPU stack (custom KGSL route): dma-buf sync_file import/export ioctls (v6.0 backport) for implicit sync
#   between Turnip/zink, compositors and the display (pairs with build-q2gpu / patches/turnip-q2-drm.py)
# audio: give techpack/audio's LPI pinctrl driver the kernel pinctrl include path (core.h / pinctrl-utils.h)
# audio: no PDR (Android's pd-mapper locator, absent on SteamOS: audio waited ~5 min for its timeout) -> the audio
#   notifier uses the plain ADSP subsystem-up event right away
# Bluetooth over the UART line discipline (q2bt / q2btattach): hu->serdev is NULL there - treat the chip as ROME
#   instead of oopsing in qca_setup (desk-330: NULL dereference at qca_setup+0x3c, so hci0 never came up)
# panel refresh by frame length (/sys/kernel/q2display/vtotal: 4916 lines = 90 Hz, 6145 = 72 Hz) - desk-377
# the boot image picks the panel entry the bootloader really runs (msm_drm.q2_dsi_display0=) - desk-382
# lower dynamic-fps rates corrected for the panel clock (msm_drm.q2_dfps_ppm: 72 Hz mode = 72.000, not 72.507) - desk-407
for p in q2-dmabuf-sync-file q2-audio-pinctrl-lpi-include q2-audio-no-pdr q2-bt-hci-qca-ldisc \
         q2-display-vtotal q2-dsi-panel-entry q2-dfps-trim; do
  git apply --check "$P/$p.patch"
  git apply "$P/$p.patch"; echo "applied $p.patch"
done
