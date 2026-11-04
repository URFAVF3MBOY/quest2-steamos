# Settings and switches

Most behaviour can be changed without rebuilding anything. The settings are plain files in `/var/lib/q2` on the headset. That folder survives restarts. The boot script rewrites its own files elsewhere (its services, `/usr/bin/q2vrtest` and the files in `/etc` it manages) on every boot, so change behaviour through these settings rather than by editing those files.

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
| `authorized_keys`                | SSH public keys, one per line: at boot they become root's keys and `sshd` starts (key login only). Delete the file and reboot to turn SSH off |

## VR display

These apply when SteamVR starts (`sh /var/lib/q2/q2vrrestart.sh` restarts SteamVR and Steam).

| File            | Default | What it does |
| --------------- | ------- | ------------ |
| `vrhz`          | `90`    | SteamVR's frame rate. `72` also switches the panel to 72 Hz by frame length (`q2-display-vtotal` kernel) - **experimental: the panel currently shows garbage at 72 Hz**, so leave it at 90 |
| `vblead`        | `13000` | How far (µs) the compositor's vsync comes before the panel refresh it renders for. Too little and frames miss their refresh under load (stutter); the default leaves about 7 ms of headroom |
| `photonms`      | `25`    | The time (ms) SteamVR predicts the head pose for, from its vsync. Must match the real vsync-to-panel time (24 ms with the default `vblead`), or fast head turns overshoot or lag |
| `direct`        | `1`     | The compositor draws straight into the display's scanout buffers (no copy per frame, ~1.3 ms of GPU time saved). `0` = the copy below |
| `tw`            | `0`     | How a frame gets to the display when `direct` is off or can't be used: `0` = transfer copy, `2` = exact copy by a compute shader, `1` = experimental timewarp (re-aims each frame at the newest head pose; resamples the picture) |
| `twlate`        | on      | With `tw` = `1`: a missed frame is re-aimed and shown again instead of repeated. `0` = off |
| `kmstrim`       | `0`     | Drop one frame after this many in a row had to wait behind an unfinished page flip (shortens a stuck flip queue). `0` = never |
| `panelscale`    | `65`    | The compositor's output size in percent of the panel, scaled up by the display engine for free (`80` = 1536x2928 is sharper and still holds 90 fps; `100` = full size costs about 2 ms of GPU time more) |
| `vrscale`       | `1.0`   | Render resolution scale of the eye images |
| `eyeshift`      | `0`     | Moves both eyes' pictures vertically by this many panel pixels (forces the copy path) |
| `compositor_tu` | `nobin,hiprio,flushpass` | `TU_DEBUG` for the compositor. `flushpass` prevents the right-eye black blocks; `nobin` (no tiled rendering) is faster for the compositor here (`-` means none) |
| `vrfacet` (`1`) | off     | SteamVR's newer "facet" renderer. On the Quest 2 it blurs the picture with colour fringes and makes head tracking glitchy; the classic renderer is the default |
| `vrdirect`      | `1`     | Outside the zero-copy path: the GPU writes frames straight into the panel buffer (`0` = old CPU copy) |

## Steam and SteamVR

| File                   | What it does                                                                                         |
| ---------------------- | ---------------------------------------------------------------------------------------------------- |
| `headlessvr`         | Run Steam in headless gamescope instead of the OpenVR backend (flat games then have nowhere to show) |
| `nosteamframeid`     | Steam sees the Quest's real hardware. **This also unblocks firmware updaters. Don't use it.**  |
| `noframeargs`        | Start Steam without `-deckard -steamdeck -steamos3`                                                |
| `novkui`             | Steam's interface without Vulkan                                                                     |
| `vroldcef`           | Steam's interface with the old, pre-Vulkan settings                                                  |
| `nobinsem`           | Turn off the synchronisation emulation for Steam's VR panels                                         |
| `dashmatch` (`1`)  | Steam's VR panels follow the headset frame rate (off by default since desk-350: slower compositor with a game running) |
| `nocefshm`           | Steam's VR interface draws with `-cef-use-vulkan` instead of ANGLE-Vulkan with a CPU-copy present (`q2_cefshm=0` on the kernel command line does the same) |
| `vrsysdistort` (`1`) | SteamVR `renderSystemLayerInDistortPass` on (the Steam Frame sets it). Restart SteamVR to apply; for testing |
| `vrscanout` (`1`)    | SteamVR `enableScanoutScaling` on (the Steam Frame sets it). Restart SteamVR to apply; for testing |
| `steamsentinel`      | Give Steam the SteamOS shutdown/restart sentinel files; when Steam exits after Shut down or Restart in its power menu, run `systemctl poweroff` / `reboot`. Experimental |
| `steamwin` (`WxH`) | Size of Steam's own window in headless mode (default `960x540`)                                    |
| `steamuihz`          | Steam interface frame rate in headless mode (default 45)                                             |
| `steamvrhz`          | Steam's interface frame rate inside SteamVR (gamescope `-r`, default 90)                             |
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
