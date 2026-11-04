# How it works

This page is for anyone who wants to work on the port, or bring the same approach to another headset.

The short version: a small boot script prepares the hardware, starts Valve's unmodified SteamOS, and adds a few pieces that stand in for the Steam Frame hardware the Quest 2 doesn't have.

```
 Steam (VR mode) ──► gamescope (OpenVR backend) ──► SteamVR compositor + the port's Vulkan layer ──► q2kmsd ──► panels
       games ──────────┘    windows become overlays                │ draws straight into the scanout buffers
                                                                   │
 Meta tracker (stock partitions) ──► /dev/shm/q2pose, q2ctl ──► SteamVR driver (driver_q2hmd)

 Everything that draws runs on the Adreno 650 through patched Mesa (Turnip) on q2msm, a port of the
 mainline Linux drm/msm GPU driver.
```

## Boot

The boot image holds Meta's kernel, Meta's device tree and a small Alpine Linux ramdisk with the boot script, `image/src/init/init`. The script runs as the first process (PID 1) and:

1. Shows the kernel log on the panels (`qscreen`, drawing into the bootloader's splash framebuffer) and switches the USB-C port to host mode.
2. Finds SteamOS. With `q2_rootdev=userdata` on the kernel command line, that's the internal `userdata` partition. Otherwise it waits up to two minutes (`q2_wait`) for a USB drive whose MBR disk signature is `0x20262026`.
3. Mounts Valve's btrfs root filesystem **read-only** and puts a RAM overlay on top (`q2_ovsize`, default 2 GB). Settings and logs that need to survive a restart live in `/var/lib/q2` and `/var/log/q2`.
4. Writes the port's services and helpers into the new root (display daemon, tracking, SteamVR driver, Steam launcher, buttons, log export), unpacks the GPU driver into `/opt/q2gpu` and loads the Wi-Fi module.
5. Hands over to systemd. SteamVR and then Steam start by themselves.

If anything goes wrong before systemd, the USB-C port switches back to device mode, and a serial console shows up on the PC (115200 baud, any terminal program).

Most of the port lives in that one script, about 7,000 lines. The comments are tagged with the build number that added each part (`desk-NNN`), so `grep desk-301` finds everything from build 301.

## Kernel

Uses Meta's own Quest 2 kernel (4.19) with Meta's display driver (SDE `msm_drm`). Qualcomm's GPU driver (KGSL) is kept from starting (`initcall_blacklist=kgsl_3d_init` on the command line); the GPU runs on `q2msm` instead, the mainline Linux `drm/msm` GPU driver ported to this kernel as a module (`kernel/q2msm/`, details in [DRM-MSM-PORT.md](DRM-MSM-PORT.md)). Kernel changes:

| Patch                                | Why                                                                                                          |
| ------------------------------------ | ------------------------------------------------------------------------------------------------------------ |
| `oculus-kernel-fixes.patch`        | Build fixes so the tree builds with AOSP clang outside Meta's build system                                   |
| `oculus-syncboss-old-driver.patch` | An older SyncBoss driver (the microcontroller behind the controllers and the IMU) that binds on firmware v59 |
| `q2-dmabuf-sync-file.patch`        | Backport of Linux 6.0's dma-buf sync-file ioctls, needed by gamescope and by Vulkan window-system code       |
| `q2-audio-*.patch`                 | The Kona sound modules build, and start without Android's PD mapper                                          |
| `q2-bt-hci-qca-ldisc.patch`        | Bluetooth (QCA6390) over the UART line discipline                                                             |
| `q2-display-vtotal.patch`          | `/sys/kernel/q2display/vtotal`: changes the panel's frame length (90 Hz ↔ 72 Hz) without a mode set           |

`config/steamos.fragment` turns on what systemd, btrfs and containers need. `CONFIG_ANDROID_PARANOID_NETWORK` must be off, or Android apps in Lepton get no network. `CONFIG_SDCARD_FS` must be off, or Android's storage daemon crashes in Lepton.

## GPU

Everything draws on the Adreno 650 through a patched Mesa in `/opt/q2gpu`: Turnip (Vulkan), freedreno (native OpenGL) and zink. The GPU kernel driver is `q2msm`; the GPU is pinned at its stock 587 MHz level.

- `patches/turnip-q2-drm.py` pairs Turnip with the display's DRM device (`card0`). With that, the normal Linux graphics stack works: GBM, EGL and GLX run through zink or freedreno, and gamescope and Xwayland can share buffers with the GPU.
- `patches/turnip-q2-async.py` adds a separate compute queue and four GPU priority levels. SteamVR's compositor gets the high-priority ring. Before this, it waited behind Steam's interface and dropped about a third of its frames.
- `patches/turnip-q2-flushpass.py` adds `TU_DEBUG=flushpass`: a full GPU cache flush at the start of every render pass. That removes the black blocks in the right eye (stale data in the second eye's pass) at no measurable cost.
- `patches/turnip-q2-querysleep.py`: waiting for query results (the compositor reads its GPU timestamps every frame) sleeps in 100 µs steps instead of busy-polling. The compositor's render thread went from about 65 % of a big core to about 20 %.

The compositor renders without tiling (`nobin`): tiled rendering was measured slower for its work (OneShot's theater view 14.3 ms per frame instead of 12.0).

Programs get the driver through `VK_ICD_FILENAMES` and `LD_LIBRARY_PATH`. A program started without them finds no working GPU. That's worth checking first whenever something falls back to software drawing.

## Display

`q2kmsd` (`q2kms serve`) owns the display. It keeps the panel timing the bootloader set up (432 × 4916 lines = 90 Hz) and page-flips four scanout buffers onto the two panel planes. Both eyes sit stacked in one buffer smaller than the panel (1536×2928 at the 80 % setting), and the display engine scales it up for free. Outside VR, `q2display` draws a lens-corrected flat desktop the same way.

`/sys/kernel/q2display/vtotal` (kernel patch) can lengthen the vertical blanking for 72 Hz (6145 lines). The display engine then runs at 72.0 Hz, but the panel shows garbage, so it is experimental and off.

## SteamVR

SteamVR runs unmodified from the SteamOS image. Three pieces make it work on the Quest 2:

- **`driver_q2hmd.so`** (`image/src/hmd/`) is the SteamVR driver. It provides the headset (display, Meta's lens model with chromatic correction, IPD) and the two Touch controllers. It reads the poses at 500 Hz from shared memory and passes the pose age and velocities on, so SteamVR predicts from the pose's real time.
- **`libq2vrdisplay.so`** (`image/src/layer/`) is a Vulkan layer loaded by the compositor and by Steam:
  - The compositor wants a real display (`VK_KHR_display`, "direct mode"). The layer provides one. Its swapchain images *are* `q2kmsd`'s scanout buffers (linear images on the same dma-bufs), so a present is just a cache flush and a page-flip request. If that can't be used, the frame is copied into a scanout buffer on the GPU instead.
  - It produces the compositor's vsync: ticks locked to the real page-flip times, `vblead` ahead of the panel refresh, so the compositor finishes in time for the refresh it aims at. SteamVR then predicts the head pose `photonms` past the tick.
  - The buffer that was on the panel is kept back for one more flip, because the display engine still reads its lower (right-eye) half after the flip.
  - Cross-process timeline semaphores (Steam's VR panels) are emulated through shared memory.
  - An experimental timewarp (`tw` = 1) can re-aim each finished frame at the newest head pose with a compute shader (`q2warp.comp`); it is off by default because it resamples the picture.
- **Compositor tuning:** the classic renderer, no tiling, its own high-priority GPU ring and the two fastest CPU cores (6 and 7).

SteamVR is also set as the OpenXR runtime, like the Frame does it.

## Tracking

The Quest 2 has no Linux tracking stack, so it runs Meta's. `q2island` attaches the headset's Android partitions **read-only** and starts Meta's tracking services in a small sandbox. That needs a preload shim for logging, SELinux and Android properties, plus stand-ins for two Meta system services. `q2metapose` copies the poses into `/dev/shm/q2pose`, and `q2ctlinput` reads buttons, sticks and triggers from the SyncBoss data stream into `/dev/shm/q2ctl`. The byte layouts are in `tracking-kit/README.txt`.

The first tracking attempt used the four tracking cameras directly: `q2camd` for capture, `q2blobs` for the controller LEDs, `q2ctltrack` for pose. It's still in the image but not started, because Meta's tracker works better.

## Steam

Steam is started the way the Steam Frame starts it:

1. **`q2steamns`** gives Steam its own mount namespace. In it, the device tree reports Frame hardware, so Steam takes its Frame code paths, and **every firmware updater is replaced by a program that does nothing**.
2. **gamescope** runs with its OpenVR backend: `--backend openvr --vr-overlay-key valve.steam.gamepadui.fallback --vr-app-overlay-key valve.steam.desktopgame ...`. Steam's interface and every flat game window become SteamVR overlays. Don't add `--allow-deferred-backend`: here gamescope starts after SteamVR, and that option makes it abort.
3. **Steam** runs with `-gamepadui -vrgamepadui -deckard -steamdeck -steamos3 -cef-force-gpu -cef-use-vulkan`. Its web interface draws with Vulkan on the GPU (ANGLE). The log line "Disabling GPU acceleration due to runtime detect" is harmless.

CPU placement: Steam on cores 0–5, its web interface on 2–6, and the SteamVR compositor on 6–7. On the Quest 2, cores 0–3 are the slow ones, 4–6 the big ones, and 7 the fastest.

Every game goes through `q2launchwrap`. It removes the port's layer settings from the game's environment, applies the Proton fixes and saves the game's Proton log ([PROTON.md](PROTON.md)).

## Buttons, sound, power

- `q2buttons`: the volume keys change PipeWire's volume, holding both for 2 seconds starts the log export, and the power key behaves like Valve's `deckard-powerbuttond` (short press and long press to Steam). systemd-logind no longer acts on the power key, and suspend is refused.
- There is no sound card yet. The kernel registers no ALSA device.

## Android apps (Lepton)

Lepton is Valve's Android container (rootless podman). For it, the port adds `fuse-overlayfs` 1.18 (the 2.x Rust version needs `openat2`, which kernel 4.19 doesn't have), user access to `/dev/ashmem`, and the GPU as `/dev/kgsl-3d0` inside the container. It also swaps in a GPU driver built for Android (the originals are kept as `*.valve`). Android starts; VRChat doesn't run yet.
