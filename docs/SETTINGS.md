# Settings and switches

Most behaviour can be changed without rebuilding anything. The settings are plain files in `/var/lib/q2` on the headset. That folder survives restarts, while the rest of the system is a fresh RAM overlay on every boot.

They're written as root, from a terminal (Ctrl+Alt+T in the desktop view):

```
sudo touch /var/lib/q2/noautostart          # turn a switch on
sudo rm /var/lib/q2/noautostart             # turn it off again
echo 2 | sudo tee /var/lib/q2/ipdpos        # settings that take a value
```

Most of them apply at the next boot. The Steam ones apply at the next Steam restart.

## Common ones

| File                               | What it does                                                      |
| ---------------------------------- | ----------------------------------------------------------------- |
| `noautostart`                    | Don't start SteamVR and Steam at boot (you get the flat desktop)  |
| `ipdpos` (`1`, `2` or `3`) | Matches the IPD slider position on the headset (default 2)        |
| `oldlens`                        | Use the lens model of the very first Quest 2 batches              |
| `allowsuspend`                   | Allow sleep (for testing: the headset does not wake up again yet) |
| `nobuttons`                      | The handling of the power and volume buttons off                  |
| `ownpower`                       | Use the power-button handler even if Valve's daemon is available  |

## Steam and SteamVR

| File                   | What it does                                                                                         |
| ---------------------- | ---------------------------------------------------------------------------------------------------- |
| `headlessvr`         | Run Steam in headless gamescope instead of the OpenVR backend (flat games then have nowhere to show) |
| `nosteamframeid`     | Steam sees the Quest's real hardware. **This also unblocks firmware updaters. Don't use it.**  |
| `noframeargs`        | Start Steam without `-deckard -steamdeck -steamos3`                                                |
| `novkui`             | Steam's interface without Vulkan                                                                     |
| `vroldcef`           | Steam's interface with the old, pre-Vulkan settings                                                  |
| `nobinsem`           | Turn off the synchronisation emulation for Steam's VR panels                                         |
| `dashmatch` (`0`)  | Steam's VR panels don't follow the headset frame rate                                                |
| `steamwin` (`WxH`) | Size of Steam's own window in headless mode (default `960x540`)                                    |
| `steamuihz`          | Steam interface frame rate in headless mode (default 45)                                             |
| `compositor_tu`      | `TU_DEBUG` value for the SteamVR compositor (`-` means none)                                     |
| `tuprio`             | GPU priority mapping, `TU_KGSL_PRIO` (default `1,1,5,9`)                                         |
| `kms` (`0`)        | Old display path (splash framebuffer) instead of `q2kmsd`                                          |

## Games and apps

| File                | What it does                                                  |
| ------------------- | ------------------------------------------------------------- |
| `nolaunchwrap`    | Start games without `q2launchwrap` (no game logs, no fixes) |
| `norubyfix`       | Don't patch Proton's `ucrtbase.dll` for Ruby games          |
| `noucrtfix`       | Skip the prefix repair for Ruby games                         |
| `nonullsink`      | Don't try to add a silent sound output                        |
| `nokwinwrap`      | Desktop ("+") without the kwin wrapper                        |
| `android_gpu_off` | Lepton keeps Valve's Android GPU files                        |
| `lepton_nopatch`  | Don't patch Lepton's start scripts                            |

## Debugging

| File        | What it does                                          |
| ----------- | ----------------------------------------------------- |
| `vrprof`  | Turn on the small CPU profiler for the VR render path |
| `gltrace` | Trace Steam's OpenGL calls (slow)                     |
| `nobench` | Never run the one-time Steam interface benchmark      |

## Kernel command line

These are set in the boot image (`image/build.sh` changes them with `--cmdline-set`):

| Token                   | What it does                                                   |
| ----------------------- | -------------------------------------------------------------- |
| `q2_rootdev=userdata` | SteamOS from internal storage (without it: from the USB drive) |
| `q2_wait=120`         | How long to wait for the USB drive, in seconds                 |
| `q2_ovsize=2g`        | Size of the RAM overlay on top of SteamOS                      |
| `qs_rot=270`          | Rotation of the boot log on the panels                         |
| `q2_diag=1`           | Extra diagnostics (Steam's web interface debug port and more)  |
| `loglevel=4`          | Kernel log level                                               |
