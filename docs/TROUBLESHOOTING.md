# Troubleshooting

## Getting the logs off the headset

1. **Hold both volume buttons for 2 seconds.** With a keyboard plugged in, **Ctrl+Alt+U** works too. From a terminal, run `q2usbpc`; that also works without root.
2. The headset collects its logs and switches its USB-C port to device mode. **Unplug the hub or keyboard and connect the PC cable.**
3. The PC sees a small USB drive called **`Q2LOGS`**. Copy everything from it.
4. Unplug the PC cable, or wait 3 minutes. The port goes back to host mode by itself.

In device mode, the PC also sees a **serial port** with a root shell. Use PuTTY or any terminal program at 115200 baud.

Files bigger than 2 MB are left off the drive and listed in `skipped-files.txt`.

## What's in the logs

| File                                           | What it's for                                                                                     |
| ---------------------------------------------- | ------------------------------------------------------------------------------------------------- |
| `kmsg-current.log`, `kmsg-prev1.log`       | Kernel log of this boot and of the last one. Start here for boot problems, crashes and GPU faults |
| `q2steam.out.txt`, `q2steam-exits.log.txt` | Steam's and gamescope's output; when and why Steam restarted                                      |
| `steam-*.txt`                                | Steam's own logs (console, web interface, game processes, compatibility tools)                    |
| `vr-*.txt`, `vrlive-*.txt`                 | SteamVR's logs, including frame timing                                                            |
| `proton-log.txt`, `proton-fix.txt`         | The last game's shortened Proton log; what the Proton fixes did                                   |
| `kwin-nested.txt`                            | The "+" desktop                                                                                   |
| `audio.txt`                                  | Sound setup                                                                                       |
| `buttons.txt`                                | Power and volume button presses                                                                   |
| `steam-frameid.txt`                          | What Steam's namespace replaced (Frame identity, blocked firmware updaters)                       |
| `journal-warnings.txt`                       | Warnings and errors from the system journal                                                       |
| `metapose.txt`, `island-test.txt`          | Meta tracker status                                                                               |
| `state-now.txt`                              | Running processes and memory when the logs were collected                                         |

## Common problems

**Black screen, or the boot log never shows.** You may have flashed the wrong image. Go back to the boot menu (Volume Down + Power) and flash `boot_steamos.img` again. If SteamOS was never copied to internal storage, use the USB image ([INSTALL.md](INSTALL.md)).

**The boot log stops at "waiting for the SteamOS drive".** The boot image is looking for SteamOS on a USB drive. That's either because it's the USB image, or because the internal copy is missing. Plug the USB drive in, or flash the normal image.

**Something breaks during boot.** If the boot script hits a problem before systemd starts, it switches the USB port to device mode and opens a serial shell. Connect the PC and open the serial port to read the error.

**Steam doesn't come up in VR.** Wait two minutes; the first start after boot is slow. Then get the logs and look at `q2steam-exits.log.txt`. If gamescope keeps crashing, the launcher falls back to headless mode for that boot.

**A game closes right away.** Look at `proton-log.txt`. If the game is RPG Maker or another Ruby-based game, check `proton-fix.txt` to see whether the Ruby fix was applied. If the game says it can't open sound ("OpenAL", "DirectSound", "XAudio"), that's the missing sound support. Try again a few minutes after boot.

**Steam's interface is slow.** That's known. It renders at about 10 to 20 frames per second. The big dashboard is the slowest part.

**Black blocks in the right eye.** Fixed in desk-303. If it comes back, send the logs.

**Volume buttons do nothing.** Check `buttons.txt`, and that `/var/lib/q2/nobuttons` doesn't exist.

## Going back to Quest OS

Flash your stock boot image ([INSTALL.md](INSTALL.md#going-back-to-quest-os)). Nothing outside `boot` and `userdata` was ever changed.
