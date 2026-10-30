# How the port got here

Every boot image has a build number (`desk-NNN`). The boot script's comments are tagged with the build that added each part, so `grep desk-301 image/src/init/init` finds it. These are the main steps.

| Builds     | What happened                                                                                                                                                                       |
| ---------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| early      | RAM-only Linux on the Quest 2 (`build-quest2-ramonly`), a serial console over USB, text on the panels (`qscreen`)                                                               |
| up to ~204 | SteamOS from a USB drive, then from internal storage; the flat desktop with lens correction; first GPU acceleration. desk-204 is the base image the current builds still start from |
| 204–222   | The tracking cameras: test pattern, live capture, LED blobs, controller pose from the cameras                                                                                       |
| 224–242   | Meta's own tracker run from the stock partitions ("island") for head and controllers                                                                                                |
| 253–255   | One patched GPU driver for SteamVR and Steam (`/opt/q2gpu`); Steam and SteamVR start by themselves; the camera trackers retired                                                   |
| 256–282   | Lepton (Android apps): rootless podman, fuse-overlayfs, ashmem, the GPU inside the container, the Android build of our GPU driver                                                   |
| 263–269   | GPU priorities, so the SteamVR compositor stopped dropping frames; SteamVR starts first, like on the Frame                                                                          |
| 283–302   | Steam's VR interface: profiling, CPU placement, Vulkan for the web interface (2–5 → 10–20 frames per second), sync emulation for the VR panels, power and volume buttons         |
| 301        | Steam sees Steam Frame hardware; firmware updaters blocked                                                                                                                          |
| 303        | Right-eye black blocks fixed                                                                                                                                                        |
| 305–306   | gamescope with its OpenVR backend, like the Frame: flat games show up in VR                                                                                                         |
| 307–309   | The "+" desktop works; logs without a keyboard (hold both volume buttons)                                                                                                           |
| 310–316   | Windows games: the Ruby problem in ARM64 Proton found and fixed (OneShot runs)                                                                                                      |
| 317        | Silent-output attempt for games that need sound (see the roadmap)                                                                                                                   |
