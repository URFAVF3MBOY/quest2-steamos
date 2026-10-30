#!/bin/sh
# q2install - copy this running SteamOS (from the USB drive) onto the Quest 2's internal "userdata" partition.
#
# WRITES ONLY TO: the partition whose GPT name is exactly "userdata" (internal storage), and a raw backup area at 15 GiB
# on the SteamOS USB drive.  Every other internal partition (abl, xbl, xbl_config, tz, hyp, aop, boot, vbmeta, super,
# modem, persist, ...) is only ever READ (boot/dtbo/vbmeta are read once for the backup) - never opened for writing.
# This wipes everything in userdata (Quest OS apps, games, saves).  Usage: q2install   (asks you to type WIPE)
set -u
L() { echo "q2-install: $*"; echo "q2-install: $*" > /dev/kmsg 2>/dev/null; }
die() { L "STOPPED: $*"; exit 1; }
[ "$(id -u)" = 0 ] || die "run as root"

part_by_name() {   # print /dev/sdXN for an exact GPT partition name on an INTERNAL (non-USB) disk
  for u in /sys/class/block/*/uevent; do
    d=$(dirname "$u"); n=$(basename "$d")
    grep -qx "PARTNAME=$1" "$u" 2>/dev/null || continue
    case "$(readlink -f "$d")" in *usb*) continue;; esac
    echo "/dev/$n"; return 0
  done
  return 1
}
name_of() { grep '^PARTNAME=' "/sys/class/block/$(basename "$1")/uevent" 2>/dev/null | cut -d= -f2; }

UD=$(part_by_name userdata) || die "no internal partition named userdata"
NM=$(name_of "$UD")
[ "$NM" = "userdata" ] || die "safety check: $UD is named '$NM', not userdata"
SECT=$(cat "/sys/class/block/$(basename "$UD")/size"); GB=$((SECT / 2 / 1024 / 1024))
[ "$GB" -ge 16 ] || die "safety check: $UD is only ${GB} GiB, expected the big userdata partition"
case "$(readlink -f /sys/class/block/$(basename "$UD"))" in *usb*) die "safety check: $UD is on USB";; esac
grep -q "^$UD " /proc/mounts && die "$UD is mounted"
ROOTSRC=$(findmnt -n -o SOURCE / 2>/dev/null)
L "target: $UD (GPT name '$NM', ${GB} GiB, internal) ; copying from / ($ROOTSRC)"

USB=""; for d in /dev/sd?; do
  case "$(readlink -f /sys/block/$(basename $d))" in *usb*) ;; *) continue;; esac
  [ "$(dd if=$d bs=1 skip=440 count=4 2>/dev/null | od -An -tx1 | tr -d ' \n')" = 26202620 ] && USB=$d
done

if [ "${1:-}" != "--yes" ]; then
  echo
  echo "This ERASES the Quest's internal userdata ($UD, ${GB} GiB): all Quest OS apps, games and saves."
  echo "Nothing else is written. Quest OS itself stays, but must not be booted normally afterwards (it would wipe SteamOS)."
  printf "Type WIPE to continue: "; read -r ans; [ "$ans" = WIPE ] || die "cancelled"
fi

# 1. backup of the stock boot images (read-only) -> USB drive, raw area at 15 GiB (PC: py tools\get_backup.py \\.\PhysicalDriveN)
if [ -n "$USB" ]; then
  B=/tmp/q2backup; rm -rf $B; mkdir -p $B
  for p in boot_a boot_b dtbo_a dtbo_b vbmeta_a vbmeta_b vbmeta_system_a vbmeta_system_b; do
    s=$(part_by_name $p) && dd if="$s" of="$B/$p.img" bs=1M 2>/dev/null && L "backed up $p ($s)"
  done
  ( cd $B && sha256sum *.img > SHA256SUMS ); tar -C $B -czf /tmp/q2backup.tgz .
  LEN=$(stat -c %s /tmp/q2backup.tgz); SHA=$(sha256sum /tmp/q2backup.tgz | cut -d' ' -f1)
  OFF=$((15 * 1024 * 1024 * 1024)); DSZ=$(( $(cat /sys/block/$(basename $USB)/size) * 512 ))
  if [ $((OFF + LEN + 8192)) -lt "$DSZ" ]; then
    python3 - "$USB" "$OFF" /tmp/q2backup.tgz <<'PY'
import hashlib, struct, sys
dev, off, f = sys.argv[1], int(sys.argv[2]), sys.argv[3]
raw = open(f, 'rb').read()
hdr = (b"Q2BLOB1\0" + struct.pack("<Q", len(raw)) + hashlib.sha256(raw).digest()).ljust(4096, b"\0")
with open(dev, 'r+b') as d:
    d.seek(off); d.write(hdr + raw)
PY
    L "boot backup ($LEN bytes) written to $USB at 15 GiB"
  else L "USB drive too small for the backup area; backup left in /tmp only"; fi
else
  L "no SteamOS USB drive found: skipping the boot backup"
fi

# 2. new filesystem on userdata
if command -v mkfs.btrfs >/dev/null; then FS=btrfs; mkfs.btrfs -f -L steamos "$UD" >/tmp/q2mkfs.log 2>&1 || die "mkfs.btrfs failed: $(tail -n 3 /tmp/q2mkfs.log)"
elif command -v mkfs.ext4 >/dev/null; then FS=ext4; mkfs.ext4 -F -L steamos "$UD" >/tmp/q2mkfs.log 2>&1 || die "mkfs.ext4 failed: $(tail -n 3 /tmp/q2mkfs.log)"
else die "no mkfs.btrfs / mkfs.ext4 on this image"; fi
L "formatted $UD as $FS"
M=/mnt/q2ud; mkdir -p $M; mount -t $FS -o noatime "$UD" $M || die "mount of the new filesystem failed"

# 3. copy (pseudo filesystems excluded)
L "copying files (several minutes; progress every 30 s)"
( while sleep 30; do L "copied $(du -sm $M 2>/dev/null | cut -f1) MiB so far"; done ) & W=$!
EX="--exclude=/proc/* --exclude=/sys/* --exclude=/dev/* --exclude=/run/* --exclude=/tmp/* --exclude=/mnt/* --exclude=/media/* --exclude=/var/tmp/q2mesa-build"
if command -v rsync >/dev/null; then rsync -aHAX --numeric-ids $EX / $M/ 2>/tmp/q2copy.err; RC=$?
else
  RC=0; for e in /*; do case "$e" in /proc|/sys|/dev|/run|/tmp|/mnt|/media) mkdir -p "$M$e";; *) cp -a "$e" "$M/" 2>>/tmp/q2copy.err || RC=1;; esac; done
fi
kill $W 2>/dev/null
L "copy finished rc=$RC, $(du -sm $M | cut -f1) MiB; errors: $(grep -v -i 'vanished' /tmp/q2copy.err 2>/dev/null | head -n 3 | tr '\n' '|' | cut -c1-200)"
mkdir -p $M/proc $M/sys $M/dev $M/run $M/tmp $M/mnt; chmod 1777 $M/tmp
date > $M/.q2-userdata-root
sync; umount $M
L "DONE. Boot the userdata image (rootfs on internal storage). The USB drive is then only needed for logs."
