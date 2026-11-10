# How the port got here

Every boot image has a build number (`desk-NNN`). The boot script's comments are tagged with the build that added each part, so `grep desk-301 image/src/init/init` finds it. These are the main steps.

| Builds     | What happened                                                                                                                                                                       |
| ---------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| early      | RAM-only Linux on the Quest 2 (`build-quest2-ramonly`), a serial console over USB, text on the panels (`qscreen`)                                                               |
| up to ~204 | SteamOS from a USB drive, then from internal storage; the flat desktop with lens correction; first GPU acceleration. desk-204 is the base image the current builds still start from |
| 204–222   | The tracking cameras: test pattern, live capture, LED blobs, controller pose from the cameras                                                                                       |
| 224–242   | Meta's own tracker run from the stock partitions ("island") for head and controllers                                                                                                |
| 253–255   | One patched GPU driver for SteamVR and Steam (`/opt/q2gpu`); Steam and SteamVR start by themselves; the camera trackers retired                                                   |
| 256–282   | Lepton (Android apps): rootless podman, fuse-overlayfs, ashmem, the GPU inside the container, the Android build of the port's GPU driver                                                   |
| 263–269   | GPU priorities, so the SteamVR compositor stopped dropping frames; SteamVR starts first, like on the Frame                                                                          |
| 283–302   | Steam's VR interface: profiling, CPU placement, Vulkan for the web interface (2–5 → 10–20 frames per second), sync emulation for the VR panels, power and volume buttons         |
| 301        | Steam sees Steam Frame hardware; firmware updaters blocked                                                                                                                          |
| 303        | Right-eye black blocks fixed                                                                                                                                                        |
| 305–306   | gamescope with its OpenVR backend, like the Frame: flat games show up in VR                                                                                                         |
| 307–309   | The "+" desktop works; logs without a keyboard (hold both volume buttons)                                                                                                           |
| 310–316   | Windows games: the Ruby problem in ARM64 Proton found and fixed (OneShot runs)                                                                                                      |
| 317        | Silent-output attempt for games that need sound (see the roadmap)                                                                                                                   |
| 318–331   | The Kona sound modules built with the kernel; the audio firmware copied in at boot                                                                                                  |
| 333–336   | Meta's VR GPU power profile; the GPU at the stock 587 MHz level; `q2kmsd` real-time                                                                                                 |
| 343–348   | Bluetooth works (QCA6390, patched kernel driver)                                                                                                                                    |
| 350–353   | Right-eye blocks traced to a GPU cache problem and flushed away; overlay quality back to low                                                                                        |
| 354–362   | The GPU on `q2msm` (mainline drm/msm ported to the 4.19 kernel) instead of KGSL; GPU preemption; a fence deadlock fixed                                                              |
| 363–366   | vsync lead (`vblead`), so frames make their refresh; Steam UI GPU-process restarts reduced                                                                                          |
| 367–370   | Experimental timewarp in the layer (compute shader, render pose per vsync tick, late re-warp); found that SteamVR's Facet renderer caused blur and colour fringes, back to classic   |
| 371        | vsync-tick-to-panel timing measured (frames landed 26–37 ms after the tick against 20 ms predicted); `vblead` 13 ms + `photonms` 25 ms                                             |
| 372–376   | SSH over Wi-Fi (opt-in); direct scanout (no copy per frame); one battery in Steam; `udisksd` off the internal storage; Quest 1 controller models; `q2msm` submits no longer fail on fragmented memory |
| 377        | 72 Hz panel mode by frame length (kernel switch `/sys/kernel/q2display/vtotal`), for heavy 2D-game theater scenes                                                                    |
| 378–379   | 72 Hz with the DSI controllers' frame length too: still garbage on the panel. Turnip query waits sleep instead of spinning (compositor render thread 65 % → 20 % of a core); `vblead` 15 ms + `photonms` 27 ms (90 fps with Steam's panels open) |
| 380–383   | 72 Hz through Meta's own JDI panel mode (DRM modes in `q2kmsd`, the timing the bootloader runs) instead of hand-written frame lengths; 72 Hz by default; `photonms` 32 at 72 Hz; controller model override |
| 384–391   | Ideas from nicolas-riera's Quest1-SteamOS and Reframed VR tried: render pass merge (now off), angular acceleration and an orientation-derived turn rate (both off: more wobble), 75 % head prediction, measured refresh rate to SteamVR |
| 392–398   | GPU back on KGSL by default (draw-level preemption: Aurora with the laser on Steam's UI dropped 0–8 frames per 128 instead of 20–181); anisotropic filtering cap; lens mesh back to 49×49; laser benchmark; Steam restart and settings fixes; log submitter masked |
| 399–401   | Lepton: Android Turnip shares SteamVR's semaphores, apps re-bake after an update, Steam-launched apps install without adb; VRChat reaches its VR session, hello_xr and Open Brush run |
| 402–403   | Controller `/pose/grip` for OpenXR bindings; Android containers boot in normal mode after a USB power-on |
