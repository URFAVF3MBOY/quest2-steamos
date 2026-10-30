#!/bin/bash
# start-tracking.sh - head + controller tracking on a Quest running Linux/SteamOS, using Meta's own tracking
# programs from the headset's own Android partitions (read-only).  From the Quest 2 SteamOS port.
#
#   sudo ./start-tracking.sh          start (keeps running in the background)
#   sudo ./start-tracking.sh stop     stop
#   sudo ./start-tracking.sh status   show whether poses are coming in
#
# Output for your SteamVR driver (see README.txt):
#   /dev/shm/q2pose   head pose (+ left/right controller poses)
#   /dev/shm/q2ctl    controller buttons, trigger, grip, stick, battery
#
# Settings (only if your headset differs - see README.txt):
#   Q2_HW=hollywood     Android hardware name   (default: androidboot.hardware from the kernel command line)
#   Q2_SLOT=a           Android slot to read    (default: androidboot.slot_suffix, else a)
#   Q2_DEVICEID=3006    hardware id, only used when the vendor files do not have one
set -u
K=$(cd "$(dirname "$0")" && pwd)
Q=/usr/share/q2vr/island; O=/var/log/q2/island; MP=/var/log/q2/metapose.txt
cmd=${1:-start}
[ "$(id -u)" = 0 ] || { echo "run it as root: sudo $0 $cmd"; exit 1; }

case "$cmd" in
stop)
  mkdir -p /run/q2island; touch /run/q2island/stop
  for i in $(seq 1 10); do pgrep -f q2island-inner >/dev/null || break; sleep 1; done
  pkill -f q2island-inner 2>/dev/null; echo "tracking stopped"; exit 0 ;;
status)
  if pgrep -f q2island-inner >/dev/null; then echo "running"; else echo "not running"; fi
  tail -n 6 $MP 2>/dev/null; echo "--- last log lines ($O/run.txt)"; tail -n 8 $O/run.txt 2>/dev/null; exit 0 ;;
start) ;;
*) echo "usage: sudo $0 [start|stop|status]"; exit 2 ;;
esac

pgrep -f q2island-inner >/dev/null && { echo "already running (sudo $0 stop first)"; exit 0; }

# --- checks: what the kernel and system need
bad=0
need() { echo "MISSING: $*"; bad=1; }
for d in /dev/syncboss0 /dev/syncboss_stream0; do [ -e $d ] || need "$d (the kernel needs Meta's SyncBoss driver)"; done
grep -q binder /proc/filesystems || need "binderfs in the kernel (CONFIG_ANDROID_BINDERFS)"
[ -e /dev/disk/by-partlabel/super ] || need "the Android 'super' partition (/dev/disk/by-partlabel/super)"
for t in python3 unshare chroot losetup awk; do command -v $t >/dev/null || need "the program '$t'"; done
[ $bad = 0 ] || exit 1
if command -v fuser >/dev/null && fuser -s /dev/syncboss0 /dev/syncboss_stream0 2>/dev/null; then
  echo "something else is using the tracking hardware - stop it first:"; fuser -v /dev/syncboss0 /dev/syncboss_stream0; exit 1
fi

# --- settings
CL=$(cat /proc/cmdline)
arg() { echo "$CL" | tr ' ' '\n' | sed -n "s/^androidboot\.$1=//p" | head -n 1; }
export Q2_HW=${Q2_HW:-$(arg hardware)}; Q2_HW=${Q2_HW:-hollywood}
S=${Q2_SLOT:-$(arg slot_suffix)}; export Q2_SLOT=${S#_}; Q2_SLOT=${Q2_SLOT:-a}
export Q2_DEVICEID=${Q2_DEVICEID:-3006}
echo "hardware=$Q2_HW slot=$Q2_SLOT"

# --- install the files where the scripts expect them
mkdir -p $Q /var/lib/q2 /var/log/q2; rm -rf $O; mkdir -p $O
cp "$K"/island/* $Q/ && chmod 755 $Q/*
cp $Q/q2island-inner.sh /usr/bin/q2island-inner && chmod 755 /usr/bin/q2island-inner
rm -f /run/q2island-noctl /run/q2island/stop $MP

# --- start (private mount namespace: nothing it mounts is visible outside, all of it goes away on stop)
setsid unshare -m --propagation private /bin/bash /usr/bin/q2island-inner live > /var/log/q2/island-console.txt 2>&1 < /dev/null &
echo "starting Meta's tracking (log: $O/run.txt) ..."

# --- wait for the first head pose (up to 90 s)
for i in $(seq 1 90); do
  if grep -q "newest state at" $MP 2>/dev/null; then
    echo "OK: head tracking is running -> /dev/shm/q2pose"
    echo "controllers: press a button on each one; they show up within ~20 s ('sudo $0 status')"
    exit 0
  fi
  pgrep -f q2island-inner >/dev/null || { echo "FAILED - it stopped. Last log lines:"; tail -n 15 $O/run.txt; exit 1; }
  sleep 1
done
echo "no head pose after 90 s - check $O/run.txt and $O/tracking.log"; exit 1
