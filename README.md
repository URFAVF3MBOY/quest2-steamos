# SteamOS on the Meta Quest 2

This project runs the Steam Frame's ARM version of SteamOS (Valve's 0.3.0 image) natively on a Meta Quest 2. It is not an emulator and not a streaming setup. The headset boots straight into SteamOS, starts SteamVR and opens Steam's VR interface. Head and controller tracking come from Meta's own tracking software, which is run from the headset's stock partitions.

Everything added lives in a single Android boot image that goes on the **boot partition only**. SteamOS itself is installed on the internal storage. Flash the stock boot image back and the headset is a normal Quest again.

> **Right now an unlocked bootloader is required.** Support for locked headsets through kexec is planned (see [Roadmap](#roadmap)).

## What works and what doesn't

Last tested build: **desk-403** (November 2026).

| Area                                                    | Status                       | Notes                                                                                                  |
| ------------------------------------------------------- | ---------------------------- | ------------------------------------------------------------------------------------------------------ |
| Booting SteamOS from internal storage                   | Works                        | Normal restarts come back to SteamOS                                                                   |
| Wi-Fi                                                   | Works                        | Meta's Wi-Fi driver, rebuilt for the kernel                                                            |
| Bluetooth                                               | Works                        | The QCA6390 combo chip, through a patched kernel driver                                                |
| Display (both panels, lens correction)                  | Works                        | 72 Hz by default (Meta's own panel mode), 90 Hz available; the compositor draws straight into the display's scanout buffers |
| Head tracking                                           | Works                        | Meta's own tracker, about 860 pose updates per second                                                  |
| Touch controllers (tracking, buttons, sticks, triggers) | Works                        | Meta's own tracker; shown with the Quest 1 controller models                                          |
| SteamVR                                                 | Works                        | Classic renderer; the compositor needs about 6 to 8 ms of GPU time per frame. The GPU runs on Qualcomm's KGSL driver, which can pause other GPU work mid-draw for the compositor |
| Steam's VR interface (library, store, settings)         | Works                        | GPU-accelerated; can stutter while it loads a lot at once                                             |
| Desktop mode (the "+" button in Steam)                  | Works                        | Nested KDE Plasma desktop inside VR                                                                    |
| Windows games through Proton                            | Works                        | Tested with OneShot. Includes a fix for Ruby/RPG Maker games (see below)                               |
| 2D games on SteamVR's theater screen                    | Partly                       | The theater view needs about 12 ms of GPU time per frame: too much for 90 Hz, it mostly fits at 72 Hz |
| Sound                                                   | Works                        | Speakers through the Kona sound modules; the volume buttons change the level                          |
| Power menu                                              | Works                        | Shut down and Restart in Steam's power menu                                                            |
| SSH over Wi-Fi                                          | Works (opt-in)               | Put public keys in `/var/lib/q2/authorized_keys` ([SETTINGS.md](docs/SETTINGS.md))                    |
| Sleep                                                   | Doesn't work                 | Turned off on purpose: the headset does not wake up again                                              |
| Android apps (VRChat through Lepton)                    | Partly                       | Steam-launched Android apps run on their own network as on the Steam Frame: VRChat in VR and online, Open Brush at 72 fps. Memory is tight with VRChat |
| Linux x86 games (through FEX)                           | Not tested                   | Windows x86 games do run through FEX inside Proton                                                     |
| Firmware updates                                        | **Blocked on purpose** | Headset and controller firmware updates would brick the hardware                                       |

### The Proton fix for Ruby games

Games built on Ruby for Windows (RPG Maker XP/VX/VX Ace, mkxp games such as OneShot) close right away on the ARM64 version of Proton, with no error. This also affects the Steam Frame. Ruby looks for a specific piece of x86 code inside Wine's C runtime (`_isatty` in `ucrtbase.dll`). Wine includes that code for Ruby, but leaves it out of ARM64 builds. The port adds those 17 bytes back into Proton's own `ucrtbase.dll` the first time a game starts. Nothing else in the file changes, and the original is kept as `ucrtbase.dll.q2orig`. Details are in [docs/PROTON.md](docs/PROTON.md).

## Safety rules

Read these before flashing anything.

- **Flash only the `boot` partition.** Never flash `abl` or `xbl`. Those are the bootloader; getting them wrong can permanently brick the headset.
- **Never let firmware updates run.** The port blocks Steam's firmware updaters. Don't try to get around that. A headset or controller firmware update cannot be undone.
- **GPU clock.** The port runs the GPU at the stock 587 MHz level, and never above it.
- Installing SteamOS **erases the Quest OS apps and data** on the headset.

## Getting started

- **Just trying it:** follow [docs/INSTALL.md](docs/INSTALL.md). It uses the image attached to the latest release.
- **Building it from source:** [docs/BUILDING.md](docs/BUILDING.md). Every part can be rebuilt with GitHub Actions, and `image/build.sh` puts the boot image together.
- **Something went wrong:** [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) shows how to get the logs off the headset.

## Documentation

| File                                              | What it covers                                                            |
| ------------------------------------------------- | ------------------------------------------------------------------------- |
| [docs/INSTALL.md](docs/INSTALL.md)                 | Installing SteamOS and the boot image, first boot, going back to Quest OS |
| [docs/BUILDING.md](docs/BUILDING.md)               | Rebuilding everything: kernel, GPU driver, boot image                     |
| [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md)       | How the pieces fit together: boot, display, tracking, GPU, Steam          |
| [docs/PROTON.md](docs/PROTON.md)                   | Running Windows games, the Ruby fix, game logs                            |
| [docs/SETTINGS.md](docs/SETTINGS.md)               | Switches that turn features on and off without rebuilding                 |
| [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | Getting logs, common problems, recovery                                   |
| [docs/ROADMAP.md](docs/ROADMAP.md)                 | What comes next, including kexec for locked headsets                      |
| [docs/HISTORY.md](docs/HISTORY.md)                 | How the port got here                                                     |
| [docs/DRM-MSM-PORT.md](docs/DRM-MSM-PORT.md)       | The GPU kernel driver: mainline drm/msm ported to the Quest 2 kernel      |

## What's in this repository

| Folder                                         | Contents                                                                                                                           |
| ---------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------- |
| `.github/workflows/`                         | GitHub Actions that build the kernel, the GPU drivers and the other binary parts                                                   |
| `config/`, `patches/`                      | Kernel configuration and patches; Mesa (GPU driver) patches                                                                        |
| `kernel/q2msm/`                              | An optional GPU kernel driver (mainline drm/msm, adapted to the Quest 2's 4.19 kernel); KGSL is the default                        |
| `image/`                                     | Everything that goes into the boot image: `build.sh`, the boot script (`src/init/init`), all sources, and the compiled helpers |
| `steamos-install/`                           | Tools to put SteamOS on a USB drive and then copy it onto the headset                                                              |
| `tracking-kit/`                              | Meta's tracking as a standalone kit, for porting other Linux systems to Quest headsets                                             |
| `initramfs/`, `scripts/`, `stock/`       | The small RAM-only system used to bring the headset up, and the boot image tools                                                   |
| `qscreen/`, `qlog/`, `qrw/`, `q2blit/` | Small tools for the display and for logs                                                                                           |
| `tools/`                                     | Helper scripts                                                                                                                     |

## Roadmap

The short version:

1. **Locked headsets through kexec.** Kexec lets a running kernel start another kernel without the bootloader.
2. **Less GPU work per frame**, so heavy scenes such as 2D games on the theater screen hold 90 fps.
3. **A smoother Steam interface.**
4. **Sleep and wake.**
5. **VRChat** and other Android apps through Lepton: controller positions, then performance.

More detail is in [docs/ROADMAP.md](docs/ROADMAP.md).

## Credits

- Meta's [Quest 2 kernel source](https://github.com/facebookincubator/oculus-linux-kernel) (GPL-2.0).
- The Linux kernel's [drm/msm](https://gitlab.freedesktop.org/drm/msm) GPU driver (GPL-2.0), the base of `kernel/q2msm`.
- [Mesa](https://mesa3d.org) (Turnip, freedreno and zink; MIT), built for the boot image.
- cabextract and libmspack (LGPL-2.1), musl libc (MIT), fuse-overlayfs (GPL-3.0), shipped as prebuilt binaries.
- nicolas-riera's [Quest1-SteamOS](https://github.com/nicolas-riera/Quest1-SteamOS) (MIT): the Vulkan layer's render pass merge (`image/src/layer/q2rpmerge.c.inc`) is adapted from its `quest1_rpmerge.c`.

SteamOS, SteamVR, Steam and Meta's tracking software belong to Valve and Meta. **None of them are in this repository.** SteamOS is downloaded from Valve's servers, and the tracking software is read from the headset's own partitions.

## Licenses

The project's own code (scripts, tools, the SteamVR driver, the Vulkan layer, the boot script) is in this repository, and each third-party part keeps its own license. Kernel patches and `kernel/q2msm` are GPL-2.0 like the kernel.
