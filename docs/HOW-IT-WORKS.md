# How it works

This page is for anyone who wants to work on the port, or bring the same approach to another headset.

The short version: a small boot script prepares the hardware, starts Valve's unmodified SteamOS, and adds a few pieces that stand in for the Steam Frame hardware the Quest 2 doesn't have.

```
 Steam (VR mode) ──► gamescope (OpenVR backend) ──► SteamVR compositor + our Vulkan layer ──► q2kmsd ──► panels
       games ──────────┘    windows become overlays                │ frames as dma-bufs, no copying
                                                                   │
 Meta tracker (stock partitions) ──► /dev/shm/q2pose, q2ctl ──► SteamVR driver (driver_q2hmd)

 Everything that draws runs on the Adreno 650 through patched Mesa (Turnip on Qualcomm's KGSL driver).
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

Uses Meta's own Quest 2 kernel (4.19), with Meta's display driver (SDE `msm_drm`) and Qualcomm's GPU driver (KGSL) as they are. Nothing is ported to the upstream `drm/msm` driver. changes:

| Patch                                | Why                                                                                                          |
| ------------------------------------ | ------------------------------------------------------------------------------------------------------------ |
| `oculus-kernel-fixes.patch`        | Build fixes so the tree builds with AOSP clang outside Meta's build system                                   |
| `oculus-syncboss-old-driver.patch` | An older SyncBoss driver (the microcontroller behind the controllers and the IMU) that binds on firmware v59 |
| `q2-dmabuf-sync-file.patch`        | Backport of Linux 6.0's dma-buf sync-file ioctls, needed by gamescope and by Vulkan window-system code       |

`config/steamos.fragment` turns on what systemd, btrfs and containers need. `CONFIG_ANDROID_PARANOID_NETWORK` must be off, or Android apps in Lepton get no network. `CONFIG_SDCARD_FS` must be off, or Android's storage daemon crashes in Lepton.

## GPU

Everything draws on the Adreno 650 through a patched Mesa in `/opt/q2gpu`. That's the same split stock Quest OS uses: KGSL renders, and Meta's display driver shows the result.

- `patches/turnip-q2-drm.py` pairs Turnip-on-KGSL with the display's DRM device (`renderD128`, `card0`). With that, the normal Linux graphics stack works: GBM, EGL and GLX run through zink on Turnip, and gamescope and Xwayland can share buffers with the GPU.
- `patches/turnip-q2-async.py` adds a separate compute queue and four GPU priority levels mapped to KGSL ring buffers. SteamVR's compositor gets the high-priority ring and can interrupt everything else. Before this, it waited behind Steam's interface and dropped about a third of its frames.

Programs get the driver through `VK_ICD_FILENAMES` and `LD_LIBRARY_PATH`. A program started without them finds no working GPU. That's worth checking first whenever something falls back to software drawing.

## Display

`q2kmsd` (`q2kms serve`) owns the display. It keeps the panel timing the bootloader set up and flips the frames SteamVR finishes straight onto the two panel planes. The frames are never copied: both eyes sit stacked in one 1248×2380 buffer, and the hardware scales it. Outside VR, `q2display` draws a lens-corrected flat desktop the same way.

## SteamVR

SteamVR runs unmodified from the SteamOS image. Three pieces make it work on the Quest 2:

- **`driver_q2hmd.so`** (`image/src/hmd/`) is the SteamVR driver. It provides the headset (display, lens distortion, IPD) and the two Touch controllers. It reads the poses at 500 Hz from shared memory and smooths them between tracker updates.
- **`libq2vrdisplay.so`** (`image/src/layer/`) is a Vulkan layer loaded by the compositor and by Steam:
  - The compositor wants a real display (`VK_KHR_display`, "direct mode"). The layer provides one and hands each frame to `q2kmsd`, along with the vblank timing.
  - Turnip on KGSL can't share synchronisation objects between processes. The layer emulates that for the compositor, and for Steam's VR panels, which pass from OpenGL to Vulkan inside Steam.
  - It keeps one extra buffer in rotation, which fixed black blocks in the right eye.
- **Compositor tuning:** sysmem rendering (as on the Frame), its own high-priority GPU ring, the two fastest CPU cores (6 and 7), and four display buffers.

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

Every game goes through `q2launchwrap`. It removes our layer's settings from the game's environment, applies the Proton fixes and saves the game's Proton log ([PROTON.md](PROTON.md)).

## Buttons, sound, power

- `q2buttons`: the volume keys change PipeWire's volume, holding both for 2 seconds starts the log export, and the power key behaves like Valve's `deckard-powerbuttond` (short press and long press to Steam). systemd-logind no longer acts on the power key, and suspend is refused.
- There is no sound card yet. The kernel registers no ALSA device.

## Android apps (Lepton)

Lepton is Valve's Android container (rootless podman). For it, the port adds `fuse-overlayfs` 1.18 (the 2.x Rust version needs `openat2`, which kernel 4.19 doesn't have), user access to `/dev/ashmem`, and the GPU as `/dev/kgsl-3d0` inside the container. It also swaps in a GPU driver built for Android (the originals are kept as `*.valve`). Android starts; VRChat doesn't run yet.
