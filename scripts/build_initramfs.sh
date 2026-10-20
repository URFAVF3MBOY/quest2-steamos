#!/bin/bash
# Build the RAM-only root filesystem: Alpine Linux (aarch64) minirootfs +
# bash + neofetch, packed as a gzip cpio initramfs with our own /init.
# Runs on an x86_64 Ubuntu host (GitHub Actions runner); aarch64 packages are
# installed through qemu-user-static + binfmt.
#
# usage: [INIT_FILE=path/to/init] build_initramfs.sh <out.cpio.gz>
set -euo pipefail

OUT="$(readlink -f "${1:?usage: build_initramfs.sh <out.cpio.gz>}")"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
ROOT="$WORK/rootfs"
mkdir -p "$ROOT"

ALPINE_BRANCH="${ALPINE_BRANCH:-v3.20}"
BASE="https://dl-cdn.alpinelinux.org/alpine/${ALPINE_BRANCH}/releases/aarch64"

echo "== resolving latest minirootfs"
FILE="$(curl -fsSL "$BASE/latest-releases.yaml" | grep -o 'alpine-minirootfs-[0-9.]*-aarch64\.tar\.gz' | head -n 1)"
[ -n "$FILE" ] || { echo "could not find minirootfs name" >&2; exit 1; }
echo "   $FILE"
curl -fsSL "$BASE/$FILE" | sudo tar -xz -C "$ROOT"

echo "== installing packages in an aarch64 chroot"
sudo cp /usr/bin/qemu-aarch64-static "$ROOT/usr/bin/"
echo "nameserver 1.1.1.1" | sudo tee "$ROOT/etc/resolv.conf" >/dev/null
sudo chroot "$ROOT" /sbin/apk add --no-cache bash ncurses-terminfo-base util-linux coreutils
if ! sudo chroot "$ROOT" /sbin/apk add --no-cache neofetch; then
  echo "neofetch not available on ${ALPINE_BRANCH}; falling back to fastfetch"
  sudo chroot "$ROOT" /sbin/apk add --no-cache fastfetch
  printf '#!/bin/sh\nexec fastfetch "$@"\n' | sudo tee "$ROOT/usr/bin/neofetch" >/dev/null
  sudo chmod +x "$ROOT/usr/bin/neofetch"
fi
sudo rm -f "$ROOT/usr/bin/qemu-aarch64-static" "$ROOT/etc/resolv.conf"

echo "== building qscreen (display tool) and helper scripts"
if [ -f "$HERE/qscreen/qscreen.c" ]; then
  aarch64-linux-gnu-gcc -O2 -static -Wall -o "$WORK/qscreen" "$HERE/qscreen/qscreen.c"
  aarch64-linux-gnu-strip "$WORK/qscreen"
  sudo install -m 0755 "$WORK/qscreen" "$ROOT/usr/bin/qscreen"
  if [ -f "$HERE/qlog/qlog.c" ]; then
    aarch64-linux-gnu-gcc -O2 -static -Wall -o "$WORK/qlog" "$HERE/qlog/qlog.c"
    aarch64-linux-gnu-strip "$WORK/qlog"
    sudo install -m 0755 "$WORK/qlog" "$ROOT/usr/bin/qlog"
  fi
  if [ -f "$HERE/qrw/qrw.c" ]; then
    aarch64-linux-gnu-gcc -O2 -static -Wall -o "$WORK/qrw" "$HERE/qrw/qrw.c"
    aarch64-linux-gnu-strip "$WORK/qrw"
    sudo install -m 0755 "$WORK/qrw" "$ROOT/usr/bin/qrw"
  fi
  for f in "$HERE"/initramfs-extra/*; do sudo install -m 0755 "$f" "$ROOT/usr/bin/$(basename "$f")"; done
fi

echo "== installing /init and device nodes"
sudo install -m 0755 "${INIT_FILE:-$HERE/initramfs/init}" "$ROOT/init"
sudo mkdir -p "$ROOT/dev" "$ROOT/proc" "$ROOT/sys" "$ROOT/run" "$ROOT/tmp" "$ROOT/root"
sudo mknod -m 600 "$ROOT/dev/console" c 5 1 2>/dev/null || true
sudo mknod -m 666 "$ROOT/dev/null" c 1 3 2>/dev/null || true
printf 'quest2\n' | sudo tee "$ROOT/etc/hostname" >/dev/null
# root shell without a password prompt (RAM-only, USB serial only)
sudo sed -i 's#^root:.*#root:x:0:0:root:/root:/bin/bash#' "$ROOT/etc/passwd"

echo "== packing initramfs"
( cd "$ROOT" && sudo find . | sudo cpio -o -H newc --quiet ) | gzip -9 > "$OUT"
ls -l "$OUT"
sudo rm -rf "$WORK"
