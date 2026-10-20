# SteamOS on the Meta Quest 2

This project runs the Steam Frame's ARM version of SteamOS (Valve's 0.3.0 image) natively on a Meta Quest 2. It is not an emulator and not a streaming setup. The headset boots straight into SteamOS, starts SteamVR and opens Steam's VR interface. Head and controller tracking come from Meta's own tracking software, which is run from the headset's stock partitions.

Everything added lives in a single Android boot image that goes on the **boot partition only**. SteamOS itself is installed on the internal storage. Flash the stock boot image back and the headset is a normal Quest again.

> **Right now you need an unlocked bootloader.** Planned to support locked headsets later through kexec (see [Roadmap](#roadmap)).

## What works and what doesn't

Last tested release: **desk-317** (October 2026).

| Area                                                    | Status                       | Notes                                                                               |
| ------------------------------------------------------- | ---------------------------- | ----------------------------------------------------------------------------------- |
| Booting SteamOS from internal storage                   | Works                        | Normal restarts come back to SteamOS                                                |
| Wi-Fi                                                   | Works                        | Uses Meta's Wi-Fi driver, rebuilt for the kernel                                    |
| Display (both panels, lens correction)                  | Works                        | SteamVR draws straight to the panels; 0 dropped frames in tests                     |
| Head tracking                                           | Works                        | Meta's own tracker                                                                  |
| Touch controllers (tracking, buttons, sticks, triggers) | Works                        | Meta's own tracker                                                                  |
| SteamVR                                                 | Works                        | Compositor takes about 6 to 8 ms per frame                                          |
| Steam's VR interface (library, store, settings)         | Works                        | GPU-accelerated, but only 10 to 20 frames per second (sorta, sometimes more)        |
| Desktop mode (the "+" button in Steam)                  | Works                        | Nested KDE Plasma desktop inside VR                                                 |
| Windows games through Proton                            | Works                        | Tested with OneShot. Includes a fix for Ruby/RPG Maker games (see below)           |
| Volume buttons                                          | Work                         | Change the volume level, but there is no sound yet                                  |
| **Sound**                                         | **Doesn't work**       | The Quest 2's sound hardware has no Linux driver in the kernel yet                  |
| Power button menu                                       | Doesn't work                 | The press reaches Steam, but the power menu does not open                           |
| Sleep                                                   | Doesn't work                 | Turned off on purpose: the headset does not wake up again                           |
| Android apps (VRChat through Lepton)                    | Partly                       | Android starts in the container; VRChat (only tested app) itself is not working yet |
| Linux x86 games (through FEX)                           | Not tested                   | Windows x86 games do run through FEX inside Proton                                  |
| Firmware updates                                        | **Blocked on purpose** | Headset and controller firmware updates would brick the hardware                    |

Games that need sound to start may still refuse to run (OneShot only started once the audio system had come up).

### The Proton fix for Ruby games

Games built on Ruby for Windows (RPG Maker XP/VX/VX Ace, mkxp games such as OneShot) close right away on the ARM64 version of Proton, with no error. This also affects the Steam Frame. Ruby looks for a specific piece of x86 code inside Wine's C runtime (`_isatty` in `ucrtbase.dll`). Wine includes that code for Ruby, but leaves it out of ARM64 builds. We add those 17 bytes back into Proton's own `ucrtbase.dll` the first time a game starts. Nothing else in the file changes, and the original is kept as `ucrtbase.dll.q2orig`. Details are in [docs/PROTON.md](docs/PROTON.md).

## Safety rules

Please read these before you flash anything.

- **Flash only the `boot` partition.** Never flash `abl` or `xbl`. Those are the bootloader; getting them wrong can permanently brick the headset.
- **Never let firmware updates run.** The port blocks Steam's firmware updaters. Don't try to get around that. A headset or controller firmware update cannot be undone.
- **GPU clock.** The stock 525 to 587 MHz is what is used.
- Installing SteamOS **erases the Quest OS apps and data** on the headset.

## Getting started

- **Just want to try it:** follow [docs/INSTALL.md](docs/INSTALL.md). It uses the image attached to the latest release.
- **Want to build it yourself:** [docs/BUILDING.md](docs/BUILDING.md). Every part can be rebuilt with GitHub Actions, and `image/build.sh` puts the released image together byte for byte.
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
| [docs/ROADMAP.md](docs/ROADMAP.md)                 | What to work on next, including kexec for locked headsets               |
| [docs/HISTORY.md](docs/HISTORY.md)                 | How the port got here                                                     |

## What's in this repository

| Folder                                         | Contents                                                                                                                           |
| ---------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------- |
| `.github/workflows/`                         | GitHub Actions that build the kernel, the GPU driver and the other binary parts                                                    |
| `config/`, `patches/`                      | Kernel configuration and patches; Mesa (GPU driver) patches                                                                        |
| `image/`                                     | Everything that goes into the boot image: `build.sh`, the boot script (`src/init/init`), all sources, and the compiled helpers |
| `steamos-install/`                           | Tools to put SteamOS on a USB drive and then copy it onto the headset                                                              |
| `tracking-kit/`                              | Meta's tracking as a standalone kit, for people porting other Linux systems to Quest headsets                                      |
| `initramfs/`, `scripts/`, `stock/`       | The small RAM-only system used to bring the headset up, and the boot image tools                                                   |
| `qscreen/`, `qlog/`, `qrw/`, `q2blit/` | Small tools for the display and for logs                                                                                           |
| `tools/`                                     | Helper scripts                                                                                                                     |

## Roadmap

The short version:

1. **Locked headsets through kexec.** Kexec lets a running kernel start another kernel without the bootloader.
2. **Sound**, using the Quest 2's Qualcomm audio hardware.
3. **A faster Steam interface**, aiming for 60 frames per second.
4. **The power menu, and sleep and wake.**
5. **VRChat** and other Android apps through Lepton.

More detail is in [docs/ROADMAP.md](docs/ROADMAP.md).

## Credits

- Meta's [Quest 2 kernel source](https://github.com/facebookincubator/oculus-linux-kernel) (GPL-2.0).
- [Mesa](https://mesa3d.org) (Turnip and zink), [Wine and Proton](https://github.com/ValveSoftware/Proton), [FEX-Emu](https://fex-emu.com), [gamescope](https://github.com/ValveSoftware/gamescope).
- [Quest-KeXec](https://github.com/machachu56/quest-kexec-q2) and [kexec-at-home](https://github.com/provide-salad/kexec-at-home), which our kexec plans build on.
- cabextract and libmspack (LGPL-2.1), musl libc (MIT), fuse-overlayfs (GPL-3.0).

SteamOS, SteamVR, Steam and Meta's tracking software belong to Valve and Meta. **None of them are in this repository.** SteamOS is downloaded from Valve's servers, and the tracking software is read from your own headset's partitions.

## Licenses

Our own code (scripts, tools, the SteamVR driver, the Vulkan layer, the boot script) is in this repository, and each third-party part keeps its own license. Kernel patches are GPL-2.0 like the kernel.
