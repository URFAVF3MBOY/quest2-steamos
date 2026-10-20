#!/bin/bash
# q2vrpaneltest (desk-253d) - Steam's VR panel with Steam running inside headless gamescope (real DRI3 X server, q2gpu GL).
# 253c: SteamVR's client inside Steam needs OpenGL's shared-semaphore functions ("pGenSemaphoresEXT != nullptr" asserts,
# since desk-251); Steam's GL on Xvfb (drisw zink, /opt/q2gl) does not list GL_EXT_semaphore_fd, so every panel
# texture failed.  Under gamescope Steam gets the q2gpu Mesa through DRI3 - the normal SteamOS setup.
# Variants: E = Steam in gamescope + q2gpu GL; F = E + Q2VR_EXTLINEAR=0.  (A-D from 253c kept below, not run.)
# --- 253c notes:
# 253b result: Steam's SteamVR client creates the panel texture in Vulkan, exports it fine (dma-buf), then the GL side
# (Steam's web UI, desktop GL 4.6 on zink) answers GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT - and zink never even tried
# to import the memory.  This run keeps SteamVR up and restarts Steam once per variant, 100 s each:
#   A  as 253b (Mesa /opt/q2gl, zink on the drisw path), GL calls traced
#   B  A + shared-image layout normalisation off (Q2VR_EXTLINEAR=0)
#   C  Steam's GL from /opt/q2gpu (zink through kopper, not drisw), GL calls traced
#   D  C + Q2VR_EXTLINEAR=0
# Per variant: does Steam's GL have GL_EXT_memory_object_fd, the traced GL calls with their GL errors, and the count of
# "SetOverlayTexture failed".  Runs once per image ~3 min after boot when Steam and SteamVR are not running.
# Report: /var/log/q2/q2vrpanel.txt (Ctrl+Alt+U).   Manual: q2vrpaneltest now     Never: touch /var/lib/q2/vrpanel_off
OUT=/var/log/q2/q2vrpanel.txt; DONE=/var/lib/q2/vrpaneltest.253d
P=/opt/q2gpu; ICDG=$P/share/vulkan/icd.d/freedreno_icd.aarch64.json
U=steamos; id $U >/dev/null 2>&1 || U=deck
HOMED=$(getent passwd $U | cut -d: -f6); SD=$HOMED/.local/share/Steam; LOGD=/var/log/q2/vrlogs; UIDN=$(id -u $U)
K() { echo "q2-vrpanel: $*" > /dev/kmsg; }
mkdir -p /var/log/q2 /var/lib/q2
if [ "$1" != now ]; then
  [ -e /var/lib/q2/vrpanel_off ] && exit 0
  [ -e $DONE ] && exit 0
  sleep 180
fi
exec >"$OUT" 2>&1
echo "=== q2vrpaneltest desk-253d  $(date '+%F %T')"
res() { echo "Q2VRPANEL $*"; K "$*"; }
[ -f $ICDG ] || { res "SKIP no /opt/q2gpu"; exit 0; }
if [ "$(cat /tmp/q2vr/state 2>/dev/null)" = running ] || pgrep -f q2steam-run.sh >/dev/null; then
  res "SKIP SteamVR or Steam already running (you are using it) - run 'q2vrpaneltest now' later"; exit 0; fi
touch $DONE

GLA=$(cat /tmp/q2glenv 2>/dev/null)   # Steam's normal GL env (desktop, /opt/q2gl)
GLQ="LD_LIBRARY_PATH=$P/lib LIBGL_DRIVERS_PATH=$P/lib/dri __EGL_VENDOR_LIBRARY_FILENAMES=$P/share/glvnd/egl_vendor.d/50_mesa.json __GLX_VENDOR_LIBRARY_NAME=mesa VK_ICD_FILENAMES=$ICDG MESA_LOADER_DRIVER_OVERRIDE=zink MESA_VK_WSI_DEBUG=sw"
echo "Steam's normal GL env: $GLA"

echo; echo "----- GL as $U on the desktop (:0) per variant: renderer + memory-object extensions"
glcheck() {   # glcheck <env...>
  runuser -u $U -- env DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/$UIDN $(echo "$GLA" | sed 's#VK_ICD_FILENAMES=[^ ]*##g') "$@" timeout -s KILL 25 glxinfo 2>&1 |
    grep -a -E "OpenGL renderer|OpenGL core profile version|direct rendering" | head -n 3 | tr '\n' '|'
  echo -n " ext: "
  runuser -u $U -- env DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/$UIDN $(echo "$GLA" | sed 's#VK_ICD_FILENAMES=[^ ]*##g') "$@" timeout -s KILL 25 glxinfo 2>&1 |
    grep -a -o -E "GL_EXT_memory_object(_fd)?|GL_EXT_semaphore(_fd)?" | sort -u | tr '\n' ' '; echo
}
echo "A/B (/opt/q2gl, drisw zink, q2gpu Vulkan): $(glcheck VK_ICD_FILENAMES=$ICDG)"
QOK=$(runuser -u $U -- env DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/$UIDN $GLQ timeout -s KILL 25 glxinfo -B 2>&1 | grep -a -c 'OpenGL renderer.*\(Adreno\|urnip\)')
echo "C/D (/opt/q2gpu, kopper zink):        $(runuser -u $U -- env DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/$UIDN $GLQ timeout -s KILL 25 glxinfo 2>&1 | grep -a -E 'OpenGL renderer|core profile version|direct rendering|rror' | head -n 4 | tr '\n' '|') ext: $(runuser -u $U -- env DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/$UIDN $GLQ timeout -s KILL 25 glxinfo 2>&1 | grep -a -o -E 'GL_EXT_memory_object(_fd)?|GL_EXT_semaphore(_fd)?' | sort -u | tr '\n' ' ')"

touch /tmp/q2vrgpu-on /var/lib/q2/gltrace_dlsym; rm -f /var/lib/q2/nogltrace
echo; echo "----- SteamVR (q2gpu)"
q2vr start; n=0
while [ $n -lt 120 ] && [ "$(cat /tmp/q2vr/state 2>/dev/null)" != running ]; do sleep 2; n=$((n+2)); done
echo "state after ${n}s: $(cat /tmp/q2vr/state 2>/dev/null)"
dmesg | grep -a 'q2-vr:' | grep -a -E 'gpu driver' | tail -n 2
if [ "$(cat /tmp/q2vr/state 2>/dev/null)" != running ]; then
  res "FAIL SteamVR did not reach running on q2gpu"; q2vr stop; rm -f /tmp/q2vrgpu-on /var/lib/q2/gltrace_dlsym
  [ -e /var/lib/q2/vrpanel_nopc ] || systemd-run --no-block --collect --unit=q2-vrpanel-pc /usr/bin/q2usbpc --gpu >/dev/null 2>&1; exit 0; fi
sleep 15

steam_stop() { touch /tmp/q2steam-stopping; pkill -f q2steam-run.sh; pkill -u $U -x steam; pkill -u $U -f steamwebhelper; sleep 4; pkill -9 -u $U -f steamwebhelper; pkill -9 -u $U -x steam; pkill -9 -u $U -f q2gamescope; pkill -9 -u $U -x Xwayland; sleep 1; }
WV="$SD/logs/webhelper_vr.txt"; VC="$LOGD/vrclient_steamwebhelper.txt"; WH="$SD/logs/steamwebhelper.log"
SUM=""
variant() {   # variant <name> <description> <steamenv file content>
  name=$1; desc=$2
  echo; echo "=================== variant $name: $desc"
  steam_stop; rm -f /tmp/q2gltrace-*.log
  printf '%s' "$3" > /tmp/q2vrgpu-steamenv
  F0=$(grep -a -c 'SetOverlayTexture failed' "$WV" 2>/dev/null); F0=${F0:-0}
  C0=$(grep -a -c 'Failed to create OpenGL framebuffer' "$VC" 2>/dev/null); C0=${C0:-0}
  R0=$(grep -a -c 'Created remote GL texture' "$VC" 2>/dev/null); R0=${R0:-0}
  H0=$(wc -l < "$WH" 2>/dev/null); H0=${H0:-0}
  rm -f /tmp/q2steam-stopping; /usr/bin/q2steam start >/dev/null 2>&1
  sleep 120
  SP=$(pgrep -u $U -x steam | head -n 1)
  echo "steam: $([ -n "$SP" ] && echo "running, VR mode $(tr '\0' ' ' < /proc/$SP/cmdline | grep -c vrgamepadui)" || echo NOT running)"
  [ -n "$SP" ] && echo "steam env: $(tr '\0' '\n' < /proc/$SP/environ 2>/dev/null | grep -a -E '^(VK_ICD_FILENAMES|LD_LIBRARY_PATH|GALLIUM_DRIVER|MESA_LOADER_DRIVER_OVERRIDE|Q2VR_EXTLINEAR|LD_PRELOAD)=' | tr '\n' ' ')"
  tail -n 3 /tmp/q2steam-exits.log 2>/dev/null
  F1=$(grep -a -c 'SetOverlayTexture failed' "$WV" 2>/dev/null); F1=${F1:-0}
  C1=$(grep -a -c 'Failed to create OpenGL framebuffer' "$VC" 2>/dev/null); C1=${C1:-0}
  R1=$(grep -a -c 'Created remote GL texture' "$VC" 2>/dev/null); R1=${R1:-0}
  echo "remote GL textures $((R1 - R0)), framebuffer failures $((C1 - C0)), SetOverlayTexture failures $((F1 - F0))"
  echo "-- GL trace (texture sharing calls + GL error after each):"
  cat /tmp/q2gltrace-*.log 2>/dev/null | sed 's/q2gltrace\[[0-9]*\]: //' | sort | uniq -c | sort -rn | head -n 25
  echo "-- layer, new lines (imports/exports):"
  tail -n +$((H0 + 1)) "$WH" 2>/dev/null | grep -a -E 'q2vrdisplay: (ext-memory|ext-image:)' | sed 's/dmabuf[0-9]*/dmabuf/' | sort | uniq -c | sort -rn | head -n 8
  tail -n +$((H0 + 1)) "$WH" 2>/dev/null | grep -a -i -E 'zink|mesa|glx|memory.?object|error' | grep -a -v -E 'ssl_client|q2vrdisplay: vkGetDeviceProcAddr' | sort | uniq -c | sort -rn | head -n 10
  if [ -z "$SP" ]; then v="steam not running"
  elif [ $((R1 - R0)) -eq 0 ]; then v="no panel texture made"
  elif [ $((C1 - C0)) -eq 0 ] && [ $((F1 - F0)) -eq 0 ]; then v="PASS"
  else v="fail ($((C1 - C0)) framebuffer, $((F1 - F0)) SetOverlayTexture)"; fi
  res "variant $name: $v"; SUM="$SUM | $name: $v"
}
# gamescope without file capabilities (with them the loader ignores VK_ICD_FILENAMES / LD_LIBRARY_PATH)
GS=$(command -v gamescope); cp "$GS" /tmp/q2gamescope && chmod 755 /tmp/q2gamescope
if "$GS" --help 2>&1 | grep -q -- '--backend'; then HL="--backend headless"; else HL="--headless"; fi
GLG="LD_LIBRARY_PATH=$P/lib LIBGL_DRIVERS_PATH=$P/lib/dri __EGL_VENDOR_LIBRARY_FILENAMES=$P/share/glvnd/egl_vendor.d/50_mesa.json VK_ICD_FILENAMES=$ICDG"
echo "GL inside gamescope: $(runuser -u $U -- env XDG_RUNTIME_DIR=/run/user/$UIDN $GLG timeout -s KILL 40 /tmp/q2gamescope $HL -W 640 -H 480 -- sh -c 'glxinfo 2>/dev/null | grep -a -E "OpenGL renderer|core profile version" | tr "\n" "|"; glxinfo 2>/dev/null | grep -a -o -E "GL_EXT_memory_object(_fd)?|GL_EXT_semaphore(_fd)?" | sort -u | tr "\n" " "' 2>/dev/null | grep -a -E 'renderer|GL_EXT' | tail -n 2 | tr '\n' ' ')"
pkill -9 -u $U -f q2gamescope 2>/dev/null; pkill -9 -u $U -x Xwayland 2>/dev/null
echo "/tmp/q2gamescope $HL -W 1920 -H 1080 -r 60 --" > /tmp/q2steam-wrap
variant E "Steam inside headless gamescope, q2gpu GL (DRI3)" "replace $GLG Q2_GLTRACE=1"
variant F "E + no shared-image layout change" "replace $GLG Q2_GLTRACE=1 Q2VR_EXTLINEAR=0"
rm -f /tmp/q2steam-wrap

echo; echo "----- stopping (normal desktop again; Steam off as after boot)"
steam_stop; pkill -9 -u $U -f q2gamescope; pkill -9 -u $U -x Xwayland; rm -f /tmp/q2vrgpu-steamenv /tmp/q2steam-wrap /var/lib/q2/gltrace_dlsym
q2vr stop; n=0; while [ $n -lt 40 ] && [ "$(cat /tmp/q2vr/state 2>/dev/null)" = running ]; do sleep 2; n=$((n+2)); done
rm -f /tmp/q2vrgpu-on
res "summary$SUM"
echo "SteamVR $(cat /tmp/q2vr/state 2>/dev/null); done $(date '+%T')"
# hand only this test's files to the PC by itself (like Ctrl+Alt+U, but just what is needed).  Off: /var/lib/q2/vrpanel_nopc
if [ ! -e /var/lib/q2/vrpanel_nopc ] && [ -x /usr/bin/q2usbpc ]; then
  K "test done - log drive with only the test's files (Q2LOGS) coming up for the PC now"
  systemd-run --no-block --collect --unit=q2-vrpanel-pc /usr/bin/q2usbpc --gpu >/dev/null 2>&1 || { setsid /usr/bin/q2usbpc --gpu >/dev/null 2>&1 < /dev/null & }
fi
