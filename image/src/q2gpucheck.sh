#!/bin/bash
# q2gpucheck (desk-253) - one-shot check of the KGSL + msm_drm GPU stack (/opt/q2gpu from CI build-q2gpu, kernel with
# q2-dmabuf-sync-file.patch).  Runs once per boot, ~60 s after the desktop is up; touches nothing on the display.
# Report: /var/log/q2/q2gpu.txt (exported with Ctrl+Alt+U).  Off: /var/lib/q2/q2gpu_off.   Manual run: q2gpucheck now
OUT=/var/log/q2/q2gpu.txt; P=/opt/q2gpu
mkdir -p /var/log/q2
U=steamos; id $U >/dev/null 2>&1 || U=deck
UID_=$(id -u $U); RT=/run/user/$UID_
[ "$1" = now ] || sleep 60
exec >"$OUT" 2>&1
echo "=== q2gpucheck desk-253d  $(date '+%F %T')  kernel $(uname -r)"
step() { echo; echo "----- $*"; }
res() { echo "Q2GPU $*"; }

step "nodes and permissions"
ls -l /dev/dri /dev/kgsl-3d0 /dev/ion /dev/dma_heap 2>&1
id $U
if [ ! -x $P/bin/q2gputest ]; then res "FAIL /opt/q2gpu missing (no q2gpu.tar.gz in this image?)"; exit 0; fi
echo "mesa: $(cat $P/mesa-commit.txt 2>/dev/null | cut -c1-12)"
ICD=$(ls $P/share/vulkan/icd.d/freedreno_icd*.json 2>/dev/null | head -n 1)
EGLV=$(ls $P/share/glvnd/egl_vendor.d/*mesa*.json 2>/dev/null | head -n 1)
ENVS="LD_LIBRARY_PATH=$P/lib VK_ICD_FILENAMES=$ICD __EGL_VENDOR_LIBRARY_FILENAMES=$EGLV __GLX_VENDOR_LIBRARY_NAME=mesa LIBGL_DRIVERS_PATH=$P/lib/dri GBM_BACKENDS_PATH=$P/lib/gbm"
echo "env: $ENVS"

step "q2gputest as root"
env $ENVS timeout -s KILL 60 $P/bin/q2gputest; rc=$?
res "root q2gputest exit $rc ($([ $rc = 0 ] && echo all passed || echo "$rc failed / crashed"))"

step "q2gputest as $U (render node permissions for apps)"
runuser -u $U -- env $ENVS XDG_RUNTIME_DIR=$RT timeout -s KILL 60 $P/bin/q2gputest; rc=$?
res "$U q2gputest exit $rc"

step "vulkaninfo --summary (q2gpu Turnip)"
env $ENVS timeout -s KILL 40 vulkaninfo --summary 2>&1 | grep -a -v -E '^\s*$' | head -n 60

step "kernel log (kgsl / msm_drm / dma-buf) after the tests"
dmesg | grep -a -i -E 'kgsl|msm_drm|sde|dma.?buf|smmu|q2-turnip' | tail -n 40

# gamescope (SteamOS' own compositor) headless: own Xwayland whose glamor uses GBM on renderD128 (zink -> Turnip).
# The client test inside it shows whether X apps get DRI3 + the GPU with no CPU copies.
step "gamescope headless + Xwayland (glamor/DRI3) test as $U"
GS=$(command -v gamescope)
# desk-253c: SteamOS' gamescope has file capabilities (cap_sys_nice).  For such programs the Vulkan loader and ld.so
# ignore VK_ICD_FILENAMES / LD_LIBRARY_PATH (secure mode), so 253b's gamescope got the stock Turnip -> "failed to find
# physical device".  A copy has no capabilities (cp does not copy them), so it uses /opt/q2gpu.
if [ -n "$GS" ]; then echo "gamescope capabilities: $(getcap "$GS" 2>/dev/null | cut -d' ' -f2-)"; cp "$GS" /tmp/q2gamescope && chmod 755 /tmp/q2gamescope && GS=/tmp/q2gamescope; fi
if [ -z "$GS" ]; then res "SKIP gamescope not installed"
else
  CLIENT='echo "DISPLAY=$DISPLAY WAYLAND_DISPLAY=$WAYLAND_DISPLAY"; xdpyinfo 2>/dev/null | grep -a -E "DRI3|Present|DRI2" | head; LIBGL_DEBUG=verbose glxinfo -B 2>&1 | grep -a -v drirc | head -n 30; echo "GL sharing extensions: $(glxinfo 2>/dev/null | grep -a -o -E "GL_EXT_memory_object(_fd)?|GL_EXT_semaphore(_fd)?" | sort -u | tr "\n" " ")"; MESA_VK_WSI_DEBUG= timeout 10 vkcube --c 120 2>&1 | tail -n 5; sleep 2'
  if "$GS" --help 2>&1 | grep -q -- '--backend'; then HL="--backend headless"; else HL="--headless"; fi
  echo "gamescope: $("$GS" --version 2>&1 | head -n 1), headless option: $HL"
  runuser -u $U -- env $ENVS XDG_RUNTIME_DIR=$RT ENABLE_GAMESCOPE_WSI=0 timeout -s KILL 60 \
    "$GS" $HL -W 1280 -H 720 -r 60 -- sh -c "$CLIENT" 2>&1 | grep -a -v -E '^\s*$' | tail -n 120
  pkill -9 -u $U -f 'q2gamescope' 2>/dev/null; pkill -9 -u $U -x Xwayland 2>/dev/null
  if grep -a -q -E '(OpenGL renderer string|Device):.*(Adreno|urnip)' "$OUT"; then res "PASS gamescope+Xwayland: X client renders on the Adreno"
  else res "FAIL gamescope+Xwayland: no Adreno GL renderer inside (see above)"; fi
fi

step "summary"
grep -a -E '^(PASS|FAIL|Q2GPU|RESULT)' "$OUT" | sort | uniq -c | sort -rn | head -n 40
echo "=== q2gpucheck done $(date '+%T')"
