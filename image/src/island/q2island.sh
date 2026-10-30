#!/bin/sh
# q2island (desk-251k) - Meta's own tracking on this headset, next to SteamOS.  "q2island live" (default at boot since
# desk-247) keeps it running and feeds SteamVR; "q2island test" = one 75 s test.
# Pauses our own camera/IMU trackers (they hold the same hardware), runs the island (q2island-inner) in a private
# mount namespace, then ALWAYS restarts our trackers.  Results: /var/log/q2/island-test.txt + island_test.bin
O=/var/log/q2/island; mkdir -p /var/log/q2; rm -rf $O; mkdir -p $O
log() { echo "$(date +%T.%3N) $*" | tee -a $O/run.txt; }
MODE=${1:-test}
log "q2island $MODE (desk-251k) - Meta's tracking from this headset's own partitions, firmware paths blocked"
SV="q2-ctltrack q2-blobs q2-camframe q2-camframe2 q2-camlive q2-camd q2-headtrack"
# desk-250: also the ones that have not started YET (desk-249: Meta's tracker started early at boot, q2-camd and
# q2-ctltrack came up after it and took the cameras -> Meta's camera service failed).  They are masked until the island
# ends (runtime only - gone after a reboot) and the ones that were running or due to start are started again after.
ACTIVE=""; for s in $SV; do
  if systemctl is-active -q $s.service 2>/dev/null || systemctl is-enabled -q $s.service 2>/dev/null; then ACTIVE="$ACTIVE $s"; fi
done
log "pausing our trackers:$ACTIVE"
for s in $SV; do systemctl mask --runtime $s.service >/dev/null 2>&1; systemctl stop $s.service 2>/dev/null; done
for t in $(systemctl list-units --all --plain --no-legend 'q2-blobs*.timer' 'q2-cam*.timer' 'q2-ctl*.timer' 2>/dev/null | awk '{print $1}'); do systemctl stop "$t" 2>/dev/null; done
n=0; while [ $n -lt 20 ] && fuser -s /dev/syncboss0 /dev/syncboss_stream0 /dev/video0 /dev/video1 2>/dev/null; do sleep 0.5; n=$((n+1)); done
if fuser -s /dev/syncboss0 /dev/syncboss_stream0 /dev/video0 /dev/video1 2>/dev/null; then
  log "tracking hardware still in use ($(fuser -v /dev/syncboss0 /dev/video0 2>&1 | tr -s ' ' | tr '\n' '|')) - test skipped"
else
  if [ "$MODE" = live ]; then unshare -m --propagation private /bin/bash /usr/bin/q2island-inner live; RC=$?
    # desk-251d: Meta's tracker failed while the controllers were listed -> once more without them (head tracking only)
    if [ "$RC" != 0 ] && [ -e /run/q2island-ctl-listed ] && [ ! -e /run/q2island-noctl ]; then
      touch /run/q2island-noctl
      log "Meta's tracker failed with the controllers listed (rc=$RC) - starting it again WITHOUT controllers (head only, until reboot)"
      mkdir -p $O/try1; cp $O/*.log $O/*.txt /var/log/q2/metapose.txt $O/try1/ 2>/dev/null; cp -r $O/snap* $O/try1/ 2>/dev/null; tar -czf /var/log/q2/island_try1.bin -C $O try1 2>/dev/null
      sleep 2; unshare -m --propagation private /bin/bash /usr/bin/q2island-inner live; RC=$?
    fi
  else timeout 240 unshare -m --propagation private /bin/bash /usr/bin/q2island-inner test; RC=$?; fi
  log "island finished (rc=$RC)"
fi
sleep 1
for s in $SV; do systemctl unmask --runtime $s.service >/dev/null 2>&1; done
for s in q2-headtrack q2-camd q2-ctltrack; do
  case " $ACTIVE " in *" $s "*) systemctl start --no-block $s.service && log "restarted $s";; esac
done
cp $O/run.txt /var/log/q2/island-test.txt
tar -czf /var/log/q2/island_test.bin -C /var/log/q2 island 2>/dev/null
log "done: /var/log/q2/island-test.txt + island_test.bin (Ctrl+Alt+U exports them)"
# desk-240: hand the results to the PC by itself - same as pressing Ctrl+Alt+U (Q2LOGS drive over the USB-C port).
# Off: touch /var/lib/q2/island_nopc
# desk-247: in live mode only when something went wrong (a clean stop = shutdown or "systemctl stop q2-island")
PCMODE=1; [ "$MODE" = live ] && [ "${RC:-0}" = 0 ] && PCMODE=0
if [ $PCMODE = 1 ] && [ ! -e /var/lib/q2/island_nopc ] && [ -x /usr/bin/q2usbpc ]; then
  log "starting PC mode by itself: the Q2LOGS drive appears on the PC (port waits 3 min for a PC, then goes back to host mode)"
  cp $O/run.txt /var/log/q2/island-test.txt
  # its own systemd unit, so it keeps running after this test's service ends
  if ! systemd-run --no-block --collect --unit=q2-island-pc /usr/bin/q2usbpc --island > /dev/null 2>&1; then
    setsid /usr/bin/q2usbpc --island > /dev/null 2>&1 < /dev/null & sleep 1
  fi
fi
cp $O/run.txt /var/log/q2/island-test.txt
