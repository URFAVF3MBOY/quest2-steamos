#!/bin/bash
# Make this port's kernel .config (needs $MAKEVARS and clang on PATH). Run inside the kernel source tree:
#   bash <repo>/scripts/kernel-config.sh <repo> [extra fragment ...]
# Extra fragments (same format) are applied after the port's own, e.g. by the kexec kernel build.
set -euo pipefail
R="$1"; shift
FRAGS="$R/config/ramonly.fragment $R/config/steamos.fragment $*"
cp "$R/config/kernel.config" .config
sed -i 's/^CONFIG_SYSTEM_TRUSTED_KEYS=.*/CONFIG_SYSTEM_TRUSTED_KEYS=""/' .config
# apply fragments: "CONFIG_X=y" -> enable, "# CONFIG_X is not set" -> disable
for frag in $FRAGS; do
  while IFS= read -r line; do
    case "$line" in
      CONFIG_*=y)          ./scripts/config --file .config -e "$(echo "$line" | sed -E 's/^CONFIG_([A-Z0-9_]+)=y$/\1/')" ;;
      "# CONFIG_"*" is not set") ./scripts/config --file .config -d "$(echo "$line" | sed -E 's/^# CONFIG_([A-Z0-9_]+) is not set$/\1/')" ;;
    esac
  done < "$frag"
done
make $MAKEVARS olddefconfig
echo "--- requested options after olddefconfig:"
for frag in $FRAGS; do
  grep -E '^CONFIG_[A-Z0-9_]+=y$' "$frag" | while read -r l; do
    opt="${l%%=*}"
    if grep -q "^${opt}=y" .config; then echo "ok       $opt"; else echo "MISSING  $opt"; fi
  done
done
# options that must be OFF (Lepton / Android apps in rootless podman need normal Linux network rules)
for off in CONFIG_ANDROID_PARANOID_NETWORK CONFIG_SDCARD_FS; do
  grep -q "^${off}=y" .config && { echo "required-off option $off is still on"; exit 1; }; echo "off      $off"
done
for must in CONFIG_DEVTMPFS CONFIG_USB_CONFIGFS_ACM CONFIG_BTRFS_FS CONFIG_USER_NS CONFIG_OVERLAY_FS CONFIG_USB_STORAGE CONFIG_USB_XHCI_HCD; do
  grep -q "^${must}=y" .config || { echo "required option $must did not stick (unmet dependency?)"; exit 1; }
done
