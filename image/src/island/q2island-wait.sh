#!/bin/sh
# q2island-wait (desk-249): starts Meta's tracking once per boot.
#   default ("live"): ~40 s after boot, Meta's tracker runs from then on and feeds SteamVR's head pose (6DoF).
#       While it runs, our own camera/IMU trackers are paused - including our own controller support.
#   echo test > /var/lib/q2/island_mode   -> old behaviour: one 75 s test ~20 s after SteamVR starts
#   sudo touch /var/lib/q2/island_off     -> never start it
[ -e /var/lib/q2/island_off ] && exit 0
MODE=$(cat /var/lib/q2/island_mode 2>/dev/null | tr -cd a-z); MODE=${MODE:-live}
[ -e /run/q2island.done ] && exit 0
if [ "$MODE" = live ]; then
  # wait until boot has settled and our own tracker is up (the island pauses it and takes over the hardware)
  n=0; while [ $n -lt 90 ] && ! systemctl is-active -q q2-headtrack.service; do sleep 2; n=$((n+1)); done
  sleep 3; touch /run/q2island.done; exec /usr/bin/q2island live   # desk-249: 20 s -> 3 s
fi
while :; do
  if pgrep -f '/vrserver' >/dev/null 2>&1; then sleep 20; touch /run/q2island.done; exec /usr/bin/q2island test; fi
  sleep 2
done
