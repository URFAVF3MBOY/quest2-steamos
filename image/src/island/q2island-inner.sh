#!/bin/bash
# q2island-inner (desk-249) - runs inside a PRIVATE mount namespace (started by q2island via unshare -m).
# Builds an Android root from this headset's own Meta partitions (read-only), blocks every firmware path, starts
# Meta's service managers + sensors service + tracking service + our stand-ins, watches them for ~75 s, records
# everything, then stops them all.  Never writes to a stock partition; never runs Meta's init scripts or fw_init.
set -u
MODE=${1:-test}   # desk-247: "live" = keep Meta's tracker running and feed SteamVR (no time limit); "test" = 75 s test
R=/run/q2island/root; O=/var/log/q2/island; Q=/usr/share/q2vr/island; RUN=/run/q2island
log() { echo "$(date +%T.%3N) $*" | tee -a $O/run.txt; }
PIDS=""
stop_all() {
  for p in $PIDS; do kill -TERM $p 2>/dev/null; done; sleep 2
  for p in $PIDS; do kill -KILL $p 2>/dev/null; done
  for d in /proc/[0-9]*; do [ "$(readlink $d/root 2>/dev/null)" = "$R" ] && kill -KILL ${d#/proc/} 2>/dev/null; done
}
cleanup() {
  log "stopping Meta's programs"; stop_all; [ -n "${FWL:-}" ] && kill $FWL 2>/dev/null; [ -n "${FWT:-}" ] && echo $FWT > /sys/class/firmware/timeout; sleep 1
  umount -R -l $R 2>/dev/null
  python3 /usr/bin/q2islandmount down >> $O/run.txt 2>&1
  for n in binder hwbinder vndbinder; do [ -L /dev/$n ] && [ "$(readlink /dev/$n)" = "binderfs/$n" ] && rm -f /dev/$n; done
  [ -L /dev/powerstate_mgr0 ] && [ "$(readlink /dev/powerstate_mgr0)" = "null" ] && rm -f /dev/powerstate_mgr0
  rmdir /dev/cpuset 2>/dev/null
  [ -n "${LOGD:-}" ] && kill $LOGD 2>/dev/null; [ "${MADESOCK:-0}" = 1 ] && rmdir /dev/socket 2>/dev/null
  dmesg | tail -n +$((DM0+1)) | tail -n 3000 > $O/dmesg-island.txt 2>/dev/null
  log "cleanup done"
}
abort() { log "ABORT: $*"; cleanup; exit 1; }
trap cleanup EXIT
trap "exit 1" TERM INT HUP
DM0=$(dmesg | wc -l)
mkdir -p $RUN; rm -f $RUN/*.dead $RUN/*.pid $RUN/crashed $RUN/stop   # desk-247: leftovers of an earlier run this boot
mount --make-rprivate / || abort "cannot make mounts private"
mkdir -p $R && mount -t tmpfs -o size=320m,mode=755 tmpfs $R || abort "tmpfs"
log "== mounting this headset's Meta partitions read-only"
python3 /usr/bin/q2islandmount up $RUN/parts >> $O/run.txt 2>&1 || abort "partition mount failed"
P=$RUN/parts
mkdir -p $R/system $R/system_ext $R/vendor $R/odm $R/product $R/apex $R/dev $R/proc $R/sys $R/data $R/persist $R/mnt/vendor/persist $R/q2 $R/tmp $R/cache $R/metadata
mount --bind $P/system_a/system $R/system || abort "bind system"
mount --bind $P/system_ext_a $R/system_ext; mount --bind $P/vendor_a $R/vendor || abort "bind vendor"
mount --bind $P/vendor_a/odm $R/odm; [ -d $P/product_a ] && mount --bind $P/product_a $R/product
[ -d $P/persist ] && { mount --bind $P/persist $R/persist; mount --bind $P/persist $R/mnt/vendor/persist; }
for d in $R/system/apex/* $R/vendor/apex/*; do
  n=$(basename "$d"); if [ -d "$d" ]; then mkdir -p $R/apex/$n && mount --bind "$d" $R/apex/$n; else log "apex $n is a packed file - skipped"; fi
done
VNDK=$(grep -h '^ro.vndk.version=' $R/vendor/build.prop 2>/dev/null | head -1 | cut -d= -f2)
if [ -d $R/apex/com.android.vndk.current ] && [ -n "$VNDK" ]; then mkdir -p $R/apex/com.android.vndk.v$VNDK; mount --bind $R/apex/com.android.vndk.current $R/apex/com.android.vndk.v$VNDK; fi
log "apex: $(ls $R/apex | tr '\n' ' ') (vndk $VNDK)"
mount --rbind /dev $R/dev; mount --rbind /proc $R/proc; mount --rbind /sys $R/sys
mkdir -p $R/dev/binderfs && mount -t binder binder $R/dev/binderfs || abort "binderfs"
for n in binder hwbinder vndbinder; do [ -e /dev/$n ] || ln -s binderfs/$n /dev/$n; done
# Android's /dev/cpuset groups: Meta's thread helper writes thread ids into /dev/cpuset/<group>/tasks.  SteamOS has
# no such tree, so the test gets an empty in-memory one (writes land in RAM files and change nothing).
mkdir -p /dev/cpuset 2>/dev/null; mount -t tmpfs -o mode=755 tmpfs $R/dev/cpuset && \
  for g in tracking object_tracking foreground background top-app system-background restricted camera-daemon audio-app display vr system; do mkdir -p $R/dev/cpuset/$g; : > $R/dev/cpuset/$g/tasks; done; : > $R/dev/cpuset/tasks; chmod -R 777 $R/dev/cpuset
# desk-239: Android's log socket.  Meta's tracker has Android's logging code built in and sends every line to
# /dev/socket/logdw; there is no log daemon here, so q2logd (receive-only) catches them into island/logcat.log.
MADESOCK=0; [ -d /dev/socket ] || { mkdir -p /dev/socket && MADESOCK=1; }
mount -t tmpfs -o mode=755 tmpfs $R/dev/socket && { python3 $Q/q2logd.py $R/dev/socket/logdw $O/logcat.log >> $O/run.txt 2>&1 & LOGD=$!; }
# Meta's kernel has a "powerstate_mgr" device that the sensors service only opens and holds (a power vote; no reads,
# writes or ioctls - checked in its code).  Our kernel has no such driver, so it gets /dev/null to hold instead.
[ -e /dev/powerstate_mgr0 ] || { ln -s null /dev/powerstate_mgr0; log "  /dev/powerstate_mgr0 -> /dev/null (held open only)"; }
mount -t tmpfs -o mode=777 tmpfs $R/data
mkdir -p $R/data/vendor/misc/sensors/controllercal $R/data/misc/tracking $R/data/local/tmp $R/data/vendor/camera $R/data/system $R/data/tombstones
chmod -R 777 $R/data; mount -t tmpfs tmpfs $R/tmp
# /vision = Meta's scratch area for the tracking engine (maps etc.); a fresh empty RAM folder each test
mkdir -p $R/vision && mount -t tmpfs -o mode=777 tmpfs $R/vision && mkdir -p $R/vision/insideout && chmod 777 $R/vision/insideout

log "== FIRMWARE BLOCKS"
BLOCK='syncboss|jedi|lcon|ruby|archive|pacific|starlet|coulee|barista|cobalt|captouch|nrf|spl'
hide() {
  local d=$1 t=$RUN/fw$(echo $1 | tr / _); [ -d $R$d ] || { log "  $d: not present"; return; }
  mkdir -p $t && mount -t tmpfs tmpfs $t || abort "tmpfs for $d"
  for f in $R$d/*; do [ -e "$f" ] || continue; b=$(basename "$f")
    if echo "$b" | grep -qiE "$BLOCK"; then log "  hidden: $d/$b"; else cp -a "$f" $t/; fi; done
  mount --bind $t $R$d && mount -o remount,bind,ro $R$d || abort "cannot cover $d"
}
hide /vendor/firmware; hide /odm/firmware; hide /system/etc/firmware
# DSP firmware (cdsp.*) lives on the "modem" partition (Android mounts it at /vendor/firmware_mnt): attach it read-only
MP=$(readlink -f /dev/disk/by-partlabel/modem_a 2>/dev/null || readlink -f /dev/disk/by-partlabel/modem 2>/dev/null)
if [ -n "$MP" ] && ML=$(losetup -r -f --show "$MP"); then
  echo "loop $ML" >> /run/q2island.loops; mkdir -p $RUN/parts/modem
  if mount -t vfat -o ro,shortname=lower $ML $RUN/parts/modem 2>/dev/null || mount -o ro $ML $RUN/parts/modem; then
    mount --bind $RUN/parts/modem $R/vendor/firmware_mnt && log "  DSP firmware partition attached read-only ($MP)"
  else log "  could not mount the modem/DSP firmware partition"; fi
else log "  no modem/DSP firmware partition found"; fi
LEFT=$(find $R/vendor/firmware $R/odm/firmware $R/system/etc/firmware 2>/dev/null | grep -iE "$BLOCK")
[ -z "$LEFT" ] || abort "firmware files still visible: $LEFT"
log "  firmware folders checked: no SyncBoss / controller images visible"
SW=$R/sys/devices/virtual/misc/syncboss0/swd
if [ -d $SW ]; then
  for f in $SW/*; do [ -f "$f" ] || continue; mount --bind /dev/null "$f" || abort "cannot cover $f"
    [ "$(stat -L -c %t:%T "$f")" = "1:3" ] || abort "cover check failed for $f"; log "  covered (writes go nowhere): ${f#$R}"; done
else log "  no swd control folder at ${SW#$R}"; fi
[ -e $SW/update_firmware ] && { [ "$(stat -L -c %t:%T $SW/update_firmware)" = "1:3" ] || abort "update_firmware not covered"; }
cp $Q/libq2island.so $Q/q2islandhost $R/q2/ && chmod 755 $R/q2/* || abort "copy tools"
grep -q "syncboss_input_update_firmware" $R/q2/libq2island.so || abort "firmware stub missing from preload library"
log "  preload library with firmware-update refusal: in place"
log "  fw_init and Meta's init scripts: never started by this test"

APX=$(for a in $R/apex/*/lib64; do echo -n ":${a#$R}"; done)
SYSLP=/system/lib64:/system_ext/lib64:/apex/com.android.runtime/lib64/bionic$APX
VENLP=/odm/lib64:/vendor/lib64:/apex/com.android.vndk.v$VNDK/lib64:/odm/lib64/hw:/vendor/lib64/hw:/apex/com.android.runtime/lib64/bionic:/system/lib64$APX
start() {
  local name=$1 lp=$2; shift 2
  /usr/bin/env -i PATH=/system/bin:/vendor/bin:/odm/bin LD_LIBRARY_PATH=$lp LD_PRELOAD=/q2/libq2island.so ANDROID_ROOT=/system \
    ANDROID_DATA=/data ANDROID_ART_ROOT=/apex/com.android.art ANDROID_I18N_ROOT=/apex/com.android.i18n ANDROID_TZDATA_ROOT=/apex/com.android.tzdata \
    /usr/bin/chroot $R "$@" > $O/$name.log 2>&1 &
  local p=$!; PIDS="$PIDS $p"; echo $p > $RUN/$name.pid; log "started $name (pid $p): $*"
}
alive() { kill -0 $(cat $RUN/$1.pid 2>/dev/null) 2>/dev/null; }
# services Android's init would start on request (ctl.start / ctl.interface_start).  ONLY these are allowed; every
# other request is logged and refused.  None of them touches firmware.
CTLN=0
serve_ctl() {
  [ -f $R/q2/ctl.queue ] || return 0
  local total; total=$(wc -l < $R/q2/ctl.queue)
  while [ $CTLN -lt $total ]; do
    CTLN=$((CTLN+1)); local req; req=$(sed -n "${CTLN}p" $R/q2/ctl.queue); local v=${req#*=}
    case "$v" in
      *IDeviceTelemetry*|oculus-hardware-telemetry-1-0) alive telemetryhal || start telemetryhal "$SYSLP" /system/bin/hw/vendor.oculus.hardware.telemetry@1.0-service ;;
      *IIad*|vendor.oculus.iad-hal-1-0) alive iad || start iad "$VENLP" /odm/bin/hw/vendor.oculus.hardware.sensors@1.0-iad ;;
      *graphics.allocator*IAllocator*) alive allocator || start allocator "$VENLP" /vendor/bin/hw/vendor.qti.hardware.display.allocator-service ;;
      mrsystemservice) alive mrsystem || start mrsystem "$SYSLP" /system/bin/mrsystemservice ;;
      cameramuxmodeservice) alive cameramux || start cameramux "$SYSLP" /system/bin/cameramuxmodeservice ;;
      trackingfidelityservice) alive fidelity || start fidelity "$SYSLP" /system/bin/trackingfidelityservice ;;
      telemetry) alive telemetryd || start telemetryd "$SYSLP" /system/bin/telemetry ;;
      *) grep -qxF "$req" $RUN/ctl.refused 2>/dev/null || { echo "$req" >> $RUN/ctl.refused; log "  start request NOT allowed (not on the list): $req"; } ;;
    esac
  done
}
FWPAT='Q2ISLAND-BLOCKED-FW|update firmware|updatefirmware|fw update|firmware update|flashing|swd update|starting syncboss swd|spl update|captouch update|version mismatch|disable_fw_version_check'
watchdog() {
  local hit; hit=$(grep -h -iE "$FWPAT" $O/*.log 2>/dev/null | head -3)
  [ -n "$hit" ] && { log "WATCHDOG: firmware-related message -> stopping everything: $hit"; return 1; }
  hit=$(dmesg | tail -n +$((DM0+1)) | grep -iE "swd|update_firmware|syncboss\.bin|jedi|_archive|q2fw-refused" | head -3)
  [ -n "$hit" ] && { log "WATCHDOG: kernel firmware activity -> stopping everything: $hit"; return 1; }
  return 0
}
waitfor() { local n=0; while [ $n -lt $2 ]; do watchdog || return 2; serve_ctl; grep -qE "$3" $O/$1.log 2>/dev/null && return 0; sleep 0.5; n=$((n+1)); done; return 1; }

log "== Android properties (shared file /q2/props, from this headset's build.prop files)"
{ for f in $R/system/build.prop $R/system_ext/etc/build.prop $R/product/etc/build.prop $R/vendor/build.prop $R/vendor/default.prop $R/odm/etc/build.prop; do
    [ -f $f ] && grep -E '^[a-zA-Z0-9_.-]+=' $f; done
  for a in $(cat /proc/cmdline); do case $a in androidboot.*=*) k=${a%%=*}; echo "ro.boot.${k#androidboot.}=${a#*=}";; esac; done
  echo "ro.hardware=hollywood"; echo "ro.boot.hardware=hollywood"; echo "sys.boot_completed=1"; echo "dev.bootcomplete=1"
  echo "vendor.syncbosshal.fw_update_complete=true"        # = "no SyncBoss update pending"; fw_init is never run
  echo "persist.vendor.syncbosshal.disable_fw_version_check=false"
  echo "vendor.syncbosshal.wait_for_service=false"
  # desk-244: stock Android sets this hardware id at startup; the sensors service needs it when the cameras start.
  # Value read from the user's own stock Quest 2 (3006).  Meta's camera code only checks ">= 3005" with it.
  grep -qh '^ro.vendor.hw.deviceid=' $R/vendor/build.prop $R/odm/etc/build.prop 2>/dev/null || echo "ro.vendor.hw.deviceid=3006"
  echo "log.tag=D"; echo "persist.log.tag=D"                # desk-239: let the tracker's built-in logging through (to q2logd)
} > $R/q2/props; chmod 666 $R/q2/props; log "  $(wc -l < $R/q2/props) properties"
grep -q "disable_fw_version_check=false" $R/q2/props || abort "property file check"

log "== firmware loader (only serves files from this headset's own partitions; refuses SyncBoss/controller images)"
FWT=$(cat /sys/class/firmware/timeout 2>/dev/null); echo 30 > /sys/class/firmware/timeout 2>/dev/null
fwloader() {
  while :; do
    for d in /sys/class/firmware/*/; do
      [ -f "$d/loading" ] || continue; n=$(basename "$d" | tr '!' '/'); b=$(basename "$n")
      if echo "$b" | grep -qiE "$BLOCK"; then echo -1 > "$d/loading"; echo "q2fw-refused $n" > /dev/kmsg; log "  FIRMWARE REFUSED: $n"; continue; fi
      f=""; for c in $R/vendor/firmware_mnt/image/$n $R/vendor/firmware/$n $R/odm/firmware/$n $R/vendor/firmware_mnt/$n; do [ -f "$c" ] && { f=$c; break; }; done
      if [ -n "$f" ]; then echo 1 > "$d/loading"; cat "$f" > "$d/data"; echo 0 > "$d/loading"; log "  firmware served: $n"
      else echo -1 > "$d/loading"; log "  firmware not found: $n"; fi
    done
    sleep 0.1
  done
}
fwloader & FWL=$!
if [ -e /sys/kernel/boot_cdsp/boot ]; then
  log "  booting the tracking DSP (cdsp) from this headset's own DSP firmware"
  echo 1 > /sys/kernel/boot_cdsp/boot 2>>$O/run.txt; sleep 4
  log "  dsp: $(dmesg | tail -n +$((DM0+1)) | grep -iE 'cdsp|subsys|pil' | tail -n 4 | tr '\n' '|')"
else log "  no /sys/kernel/boot_cdsp/boot - DSP boot skipped"; fi

# desk-241/243: Android's HAL lookup waits FOREVER for a listed-but-not-running HIDL HAL (desk-240: the power HAL ->
# the tracker missed its deadline and shut down).  Every HIDL HAL this test does not run (power, firmware, display,
# audio, ...) is taken out of copies of the manifests (RAM, bind-mounted in this namespace only), so lookups answer
# "not available" at once.  The firmware HALs can't even be looked up this way.
mkdir -p $RUN/vintf && python3 $Q/q2vintf.py $R $RUN/vintf > $RUN/vintf.list 2>>$O/run.txt
while read -r orig copy; do
  case "$orig" in HIDDEN) log "  HALs hidden from lookups (not run by this test): $copy";; *) mount --bind "$copy" "$R/$orig" && log "  manifest copy in use: /$orig";; esac
done < $RUN/vintf.list
# desk-246: Android gives every service an open-file limit of 32768; a systemd service on SteamOS starts with 1024.
# The tracker reached RUNNING in desk-245 and then failed to duplicate a camera buffer's file descriptors.
O0=$(ulimit -n); ulimit -n 32768 2>/dev/null || ulimit -n $(ulimit -Hn) 2>/dev/null
log "  open-file limit for Meta's programs: $(ulimit -n) (was $O0)"
log "== starting Meta's programs (logs: island/*.log)"
start servicemanager "$SYSLP" /system/bin/servicemanager; sleep 0.7
alive servicemanager || abort "servicemanager exited: $(tail -n 5 $O/servicemanager.log | tr '\n' ' ')"
start hwservicemanager "$SYSLP" /system/bin/hwservicemanager; sleep 0.7
alive hwservicemanager || abort "hwservicemanager exited: $(tail -n 5 $O/hwservicemanager.log | tr '\n' ' ')"
start vndservicemanager "$VENLP" /vendor/bin/vndservicemanager /dev/vndbinder; sleep 0.5
start cdsprpcd "$VENLP" /vendor/bin/cdsprpcd; sleep 0.5
start telemetryhal "$SYSLP" /system/bin/hw/vendor.oculus.hardware.telemetry@1.0-service
# graphics buffer allocator (camera buffers only; the display composer / surfaceflinger are NEVER started - SteamOS owns the display)
start allocator "$VENLP" /vendor/bin/hw/vendor.qti.hardware.display.allocator-service; sleep 0.5
# desk-251k: each controller's factory calibration (LED positions, IMU) - Meta's sensors service keeps it in
# /data/vendor/misc/sensors/controllercal/<id> once it has read it from the controller.  Our /data is a fresh RAM
# folder every start, so the file was always missing: if the tracker asked before that controller had fully connected,
# it got "Could not load factory calibration" and never tracked it (desk-251j: the left hand).  Kept across starts in
# /var/lib/q2/metacal (this headset's own controllers' data, written by Meta's service itself).
CALD=$R/data/vendor/misc/sensors/controllercal; MCAL=/var/lib/q2/metacal; mkdir -p $MCAL
n=0; for f in $MCAL/*; do [ -f "$f" ] || continue; cp "$f" $CALD/ && chmod 666 $CALD/$(basename "$f") && n=$((n+1)); done
log "controller calibration files restored: $n ($(ls $MCAL 2>/dev/null | tr '\n' ' '))"
start sensors "$VENLP" /odm/bin/hw/vendor.oculus.hardware.sensors@1.0-service
waitfor sensors 50 "paired devices|Syncboss is ready|ready" ; r=$?
[ $r = 2 ] && abort "watchdog"
alive sensors || abort "sensors service exited: $(tail -n 8 $O/sensors.log | tr '\n' ' ')"
log "sensors service: $(grep -m1 -iE 'Detected firmware version' $O/sensors.log)"
# desk-249: wait for the HAL to finish registering (was a fixed 20 s wait for a message that never comes)
waitfor sensors 40 "sensor HAL ready|Finished getting cache data|Cache populated" ; [ $? = 2 ] && abort "watchdog"
serve_ctl; sleep 2
# desk-251: the controller list for Meta's tracker.  The controllers only connect AFTER the tracker starts (it is what
# switches the controller radio on), so their ids are never in the log yet at this point - desk-247..250 handed the
# tracker an EMPTY list ("controllers: left= right="), and Meta never tracked them.  The ids now come from
# /var/lib/q2/meta_controllers (saved from the sensors service's log whenever the controllers connect, seeded at boot
# with this headset's paired pair), and the log is still read in case they are already connected.
# Log format: the cache block has the id masked ("0d6afd2bxxxxxxxx") with serial / fw / handedness; the full id is in
# "Syncboss input 0d6afd2b603545b0 hardware type 4, subtype N".  Both are joined on the first 8 hex digits.
CTLDB=/var/lib/q2/meta_controllers
ctl_from_log() {
  awk '/Syncboss input [0-9a-f]+ hardware/{for(i=1;i<=NF;i++) if($i=="input"){full[substr($(i+1),1,8)]=$(i+1)}}
    /^ *id +:/{id=substr($NF,1,8)} /^ *serial +:/{sn[id]=$NF} /^ *fw ver +:/{fw[id]=$NF} /^ *handedness +:/{h[id]=$NF}
    END{for(i in h) if(full[i]!="" && length(full[i])==16) printf "%s %s:%s:%s\n", h[i], full[i], (sn[i]?sn[i]:"unknown"), (fw[i]?fw[i]:"0.0.0")}' $O/sensors.log 2>/dev/null
}
CTL=$(ctl_from_log)
LS=$(echo "$CTL" | awk '$1=="left"{print $2}' | head -1); RS=$(echo "$CTL" | awk '$1=="right"{print $2}' | head -1)
CSRC="sensors log"
if [ -z "$LS" ] || [ -z "$RS" ]; then
  [ -z "$LS" ] && LS=$(awk '$1=="left"{print $2}' $CTLDB 2>/dev/null | head -1)
  [ -z "$RS" ] && RS=$(awk '$1=="right"{print $2}' $CTLDB 2>/dev/null | head -1)
  CSRC="$CTLDB"
fi
# desk-251d: one retry without controllers when the tracker failed with them listed (see q2island) - head tracking first
if [ -e /run/q2island-noctl ]; then LS=""; RS=""; CSRC="none: retry without controllers after a failed start (until reboot)"; fi
# desk-251e: the tracker always starts with an EMPTY controller list (listing them at start stalled it - desk-251d's
# first try: no head pose in 40 s).  Once both controllers have connected to Meta's sensors service, they are announced
# to the tracker the normal way (the stand-in's status callback), through /q2/ctl-late (see the loop below).
rm -f /run/q2island-ctl-listed $R/q2/ctl-late $R/q2/ctl-again /run/q2metactl.state
log "controllers known ($CSRC): left=$LS right=$RS - announced to Meta's tracker once they connect"
LS=""; RS=""
[ -z "$LS$RS" ] && log "  WARNING: no controller ids known - Meta will not track the controllers this time (they are saved once they connect)"
CTLSAVED=""; CTLLOGGED=""; CTLNEXT=0   # desk-251f: set -u - desk-251e stopped the island on the unset CTLLOGGED the moment the controllers connected
# desk-245: a HAL this test runs must stay listed in the manifest copy - stop at once (with the name) if one is refused
v=$(grep -h "must be in VINTF manifest" $O/*.log 2>/dev/null | head -2 | tr '\n' ' ')
[ -n "$v" ] && abort "a HAL this test runs was hidden from the manifest by mistake: $v"
# the tracker's camera-mode monitor waits for these two Meta system services (no firmware, no display)
# desk-249: stand-ins first - the camera-mode service waits ~10 s for "permission" if it is not there yet
start standins "$SYSLP" /q2/q2islandhost "$LS" "$RS"; sleep 0.8
alive standins || abort "stand-in services exited: $(tail -n 5 $O/standins.log | tr '\n' ' ')"
start cameramux "$SYSLP" /system/bin/cameramuxmodeservice
start mrsystem "$SYSLP" /system/bin/mrsystemservice; sleep 0.8
start tracking "$SYSLP" /system/bin/trackingservice
take_snap() {   # take_snap <name>: Meta's own status dump, the tracker's memory map / threads / shared memory
    S=$O/$1; mkdir -p $S
    timeout 10 /usr/bin/env -i LD_LIBRARY_PATH=$SYSLP LD_PRELOAD=/q2/libq2island.so /usr/bin/chroot $R /system/bin/dumpsys -l > $S/services.txt 2>&1
    for svc in tracking TrackingService; do
      timeout 10 /usr/bin/env -i LD_LIBRARY_PATH=$SYSLP LD_PRELOAD=/q2/libq2island.so /usr/bin/chroot $R /system/bin/dumpsys $svc > $S/dumpsys_$svc.txt 2>&1
    done
    TP=$(cat $RUN/tracking.pid 2>/dev/null); cat /proc/$TP/maps > $S/tracking_maps.txt 2>/dev/null
    # what each tracker thread is doing (kernel side): name, state, wait channel, current syscall, kernel stack
    for t in /proc/$TP/task/*; do
      echo "== thread ${t##*/} $(cat $t/comm 2>/dev/null) state=$(awk '/^State/{print $2}' $t/status 2>/dev/null) wchan=$(cat $t/wchan 2>/dev/null) syscall=$(cat $t/syscall 2>/dev/null)"
      cat $t/stack 2>/dev/null | head -12
    done > $S/tracking_threads.txt 2>&1
    ls -l /proc/$TP/fd > $S/tracking_fds.txt 2>&1
    V=$(grep -h "Pose:" $S/dumpsys_*.txt 2>/dev/null | sed -E 's/.*trans=\( *([-0-9.]+), *([-0-9.]+), *([-0-9.]+)\).*/\1 \2 \3/' | head -6 | tr '\n' ' ')
    [ -n "$V" ] && timeout 60 $Q/q2tsfind $TP $S $V > $S/find.txt 2>&1
    # desk-251g: copy of every small shared-memory area of the tracker (to find where it keeps the controller poses)
    timeout 30 python3 - $TP $S <<'PYSHM' >> $O/run.txt 2>&1
import sys, re
pid, out = sys.argv[1], sys.argv[2]; n = tot = 0
try: mem = open('/proc/%s/mem' % pid, 'rb', 0)
except Exception as e: print('  shm dump: %s' % e); sys.exit(0)
for l in open('/proc/%s/maps' % pid):
    p = l.split()
    if len(p) < 6 or p[1][3] != 's': continue
    a, b = [int(x, 16) for x in p[0].split('-')]; name = ' '.join(p[5:])
    if b - a > (1 << 20) or tot + (b - a) > (48 << 20): continue
    if not re.search(r'MemoryBroker|Tracking|Controller', name): continue   # desk-251h: camera image buffers left out
    try: mem.seek(a); d = mem.read(b - a)
    except Exception: continue
    open('%s/shm_%s_%s.bin' % (out, p[0].split('-')[0], re.sub(r'[^A-Za-z0-9]+', '_', name)[-50:]), 'wb').write(d); n += 1; tot += len(d)
print('  shm dump: %d areas, %d KB' % (n, tot // 1024))
PYSHM
    log "snapshot $1: $(grep -h -E 'Tracking Level' $S/dumpsys_*.txt 2>/dev/null | sed -E 's/^ +//' | cut -c1-80 | tr '\n' '|')"
    ls -la $CALD > $O/controllercal-list.txt 2>&1
    { grep -a -i -E "remote|controller|pulsar|paired|constellation|handed|ctrl" $O/logcat.log 2>/dev/null | grep -v q2props | tail -n 600; } > $O/logcat-controllers.txt
    tar -czf /var/log/q2/island_live.bin -C $O $(cd $O && ls -d snap* run.txt standins.log logcat-controllers.txt sensors.log controllercal-list.txt 2>/dev/null) 2>/dev/null   # exported by Ctrl+Alt+U while live
}
nows() { cut -d. -f1 /proc/uptime; }   # desk-251g: seconds since boot (the wall clock jumps when network time arrives)
T0=$(nows); snap=0; CTLT=""; csnap=0; AGAIN=0; AGAINT=0
# desk-247: hand Meta's head pose to the SteamVR driver (/dev/shm/q2pose) - reads the tracker's memory, writes nothing to it
$Q/q2metapose $(cat $RUN/tracking.pid) > /dev/null 2>&1 & MP=$!; PIDS="$PIDS $MP"; log "started q2metapose (pid $MP): Meta head + controller poses -> SteamVR"
# desk-251: controller buttons / trigger / grip / stick / touch / battery while Meta owns SyncBoss.  q2ctlinput only
# LISTENS on the SyncBoss data stream (the kernel copies every packet to each reader); it never opens the SyncBoss
# command device and sends nothing to the MCU or the controllers.
if [ -x $Q/q2ctlinput ] && [ ! -e /var/lib/q2/meta_ctlinput_off ]; then
  /usr/bin/python3 $Q/q2ctlinput >> /var/log/q2/ctlinput.txt 2>&1 & CI=$!; PIDS="$PIDS $CI"; log "started q2ctlinput (pid $CI): controller buttons -> SteamVR (listen only)"
fi
if [ "$MODE" = live ]; then LIMIT=999999999; log "== LIVE: Meta's tracker feeds SteamVR until it stops (stop: systemctl stop q2-island)"
else LIMIT=75; log "== running ~75 s: move the headset and both controllers in front of it"; fi
while [ $(( $(nows) - T0 )) -lt $LIMIT ]; do
  [ -e $RUN/stop ] && { log "stop requested"; break; }
  # desk-249: higher CPU priority (nice -10) for every thread of Meta's tracker and sensors service, re-applied every
  # 15 s for new threads - SteamVR's dashboard was delaying their head updates by up to 27 ms
  if [ $(( ($(nows) - T0) % 15 )) = 0 ]; then for n in tracking sensors; do P=$(cat $RUN/$n.pid 2>/dev/null); [ -n "$P" ] && for t in /proc/$P/task/*; do renice -n -10 -p ${t##*/} >/dev/null 2>&1; done; done; fi
  [ $(( ($(nows) - T0) % 60 )) = 0 ] && cp $O/run.txt /var/log/q2/island-test.txt 2>/dev/null
  watchdog || exit 1
  serve_ctl
  # desk-251: save the controller ids once they have connected (used for the tracker's list on the next start)
  if [ -z "$CTLSAVED" ] && [ $(nows) -ge ${CTLNEXT:-0} ]; then
    CTLNEXT=$(( $(nows) + 3 ))
    NC=$(ctl_from_log); NL=$(echo "$NC" | awk '$1=="left"{print $2}' | head -1); NR_=$(echo "$NC" | awk '$1=="right"{print $2}' | head -1)
    if [ -n "$NL" ] && [ -n "$NR_" ]; then
      CTLSAVED=1
      # desk-251e: hand them to the tracker now (the stand-in picks this file up within ~1 s and calls onStatus)
      printf '0 %s\n1 %s\n' "$NL" "$NR_" > $R/q2/ctl-late.tmp && mv $R/q2/ctl-late.tmp $R/q2/ctl-late
      log "controllers connected: left=$NL right=$NR_ - announcing them to Meta's tracker"; CTLT=$(nows)
      LS0=$(awk '$1=="left"{print $2}' $CTLDB 2>/dev/null | head -1); RS0=$(awk '$1=="right"{print $2}' $CTLDB 2>/dev/null | head -1)
      if [ "$NL" != "$LS0" ] || [ "$NR_" != "$RS0" ]; then
        printf 'left %s\nright %s\n' "$NL" "$NR_" > $CTLDB.tmp && mv $CTLDB.tmp $CTLDB; log "  (saved to $CTLDB)"; fi
    fi
  fi
  # desk-251h: a hand still missing from Meta's controller memory 8 s after the announcement -> announce it again
  # (desk-251g: only the right controller was taken; the left never appeared).  Up to 4 times, 8 s apart.
  if [ -n "$CTLT" ] && [ $AGAIN -lt 4 ] && [ $(nows) -ge $(( CTLT + 8 + AGAIN * 8 )) ]; then
    ST=$(cat /run/q2metactl.state 2>/dev/null); NEED=""
    case "$ST" in *left=1*) ;; *) NEED="$NEED 0";; esac; case "$ST" in *right=1*) ;; *) NEED="$NEED 1";; esac
    AGAIN=$((AGAIN+1))
    if [ -n "$NEED" ]; then
      for t in $NEED; do AGAINT=$((AGAINT+1)); echo "$AGAINT $t" >> $R/q2/ctl-again; done
      log "Meta's controller memory: ${ST:-nothing yet} - announcing type(s)$NEED again (try $AGAIN)"
    else log "Meta's controller memory: both hands present"; AGAIN=9; fi
  fi
  # desk-251k: keep every controller calibration file Meta's sensors service writes (checked every 10 s)
  if [ $(( ($(nows) - T0) % 10 )) = 0 ]; then
    for f in $CALD/*; do [ -f "$f" ] || continue; b=$(basename "$f")
      if ! cmp -s "$f" $MCAL/$b; then cp "$f" $MCAL/$b.tmp && mv $MCAL/$b.tmp $MCAL/$b && log "controller calibration saved: $b ($(stat -c %s "$f") bytes)"; fi
    done
  fi
  # desk-251e: what the tracker answered when the controllers were announced (once)
  if [ -n "$CTLSAVED" ] && [ -z "$CTLLOGGED" ] && grep -q "onStatus" $O/standins.log 2>/dev/null; then
    CTLLOGGED=1; sleep 1; grep -a -E "controller connected|onStatus|callback" $O/standins.log | tail -n 4 | while IFS= read -r l; do log "  stand-in: $l"; done; fi
  for n in servicemanager hwservicemanager telemetryhal allocator sensors cameramux mrsystem standins tracking fidelity iad; do
    [ -f $RUN/$n.pid ] || continue
    alive $n || { [ -f $RUN/$n.dead ] || { touch $RUN/$n.dead; log "$n EXITED: $(tail -n 6 $O/$n.log | tr '\n' ' ')"; }; }
  done
  v=$(grep -h "must be in VINTF manifest" $O/*.log 2>/dev/null | head -2 | tr '\n' ' ')
  [ -n "$v" ] && { log "a HAL this test runs was hidden from the manifest by mistake - ending the test: $v"; sleep 2; break; }
  [ -f $RUN/sensors.dead ] && { log "sensors service stopped - ending the test early"; touch $RUN/crashed; sleep 3; break; }
  # desk-251d: with controllers listed, Meta's head pose must appear within 40 s, else stop (q2island retries without them)
  if [ -n "$LS$RS" ] && [ $(( $(nows) - T0 )) -ge 40 ] && ! grep -q "newest state at" /var/log/q2/metapose.txt 2>/dev/null; then
    log "no head pose from Meta's tracker after 40 s with the controllers listed - stopping"; touch $RUN/crashed; sleep 1; break; fi
  # desk-240: no point waiting out the full time once Meta's tracker has stopped - end the test now
  if [ -f $RUN/tracking.dead ]; then log "tracker stopped - ending the test early"; touch $RUN/crashed; sleep 3; break; fi
  el=$(( $(nows) - T0 ))
  if [ $el -ge $((10 + snap * 10)) ] && [ $snap -lt 4 ]; then snap=$((snap+1)); take_snap snap$snap; fi
  # desk-251g: two more ~12 s and ~16 s after the controllers were announced (move both controllers in view then)
  if [ -n "$CTLT" ] && [ $csnap -lt 2 ] && [ $(nows) -ge $(( CTLT + 12 + csnap * 4 )) ]; then csnap=$((csnap+1)); take_snap snapctl$csnap; fi
  sleep 1
done
log "== test time over"
[ -e $RUN/crashed ] && exit 3
exit 0
