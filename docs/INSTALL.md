# Installing SteamOS on a Quest 2

This guide takes a Quest 2 with an unlocked bootloader to a working SteamOS setup. Plan on an hour or two. Most of that is waiting for downloads and copies.

**Read the [safety rules](../README.md#safety-rules) first.** In short: only ever flash `boot`, never `abl` or `xbl`, and never let firmware updates run.

## What you need

- A **Meta Quest 2** with an **unlocked bootloader**. This guide does not cover unlocking.
- A PC running Windows, Linux or macOS with `adb` and `fastboot` ([Android platform tools](https://developer.android.com/tools/releases/platform-tools)).
- A **USB drive of 16 GB or more**, and a way to plug it into the headset (a USB-C drive, or a USB-C hub or OTG adapter).
- A **USB keyboard** helps a lot (on the same hub). You need one for a few steps.
- **Python 3.8+** on the PC, for the download script.
- About **25 GB of free space** on the PC.
- The files from the [latest release](../../releases/latest):
  - `boot_steamos.img`: the normal boot image (SteamOS from internal storage)
  - `boot_steamos_usb.img`: the first-install image (SteamOS from the USB drive)
  - `rootfs.img.caibx`: the index file for Valve's SteamOS image

Back up anything you want to keep from Quest OS. **Installing SteamOS erases all Quest OS apps, games and saves.** Quest OS itself stays, so you can go back to it later.

## Step 1: Save your stock boot image

This is your way back to Quest OS, so keep it somewhere safe.

If the headset is rooted, run this in a command window on the PC:

```
adb shell su -c "dd if=/dev/block/by-name/boot_a of=/sdcard/boot_stock.img"
adb pull /sdcard/boot_stock.img
```

Use `boot_b` instead if your active slot is B (`adb shell getprop ro.boot.slot_suffix` tells you). If the headset isn't rooted, use the stock boot image for your exact firmware version.

## Step 2: Put SteamOS on the USB drive

The download script rebuilds Valve's Steam Frame SteamOS image (about 10 GB) from Valve's own servers. It also marks the drive, so the boot image can find it.

```
pip install zstandard
python steamos-install/steamos_fetch.py --caibx rootfs.img.caibx --out disk.img
```

This takes a while. When it finishes, it checks the SHA-256 of the result against Valve's published value.

Then write `disk.img` to the USB drive with [balenaEtcher](https://etcher.balena.io), or with Rufus in **DD image mode**. On Linux you can use `dd`, but double-check the device name first.

## Step 3: Boot SteamOS from the USB drive

1. Turn the headset off. Hold **Volume Down + Power** until the boot menu appears.
2. Connect it to the PC and flash the first-install image to the boot partition:
   ```
   fastboot flash boot boot_steamos_usb.img
   fastboot reboot
   ```
3. Unplug the PC cable and plug in the USB drive (and the keyboard through the hub).

The headset shows a kernel log on the panels while it starts, then looks for the USB drive for up to two minutes. The first boot takes a few minutes.

## Step 4: Copy SteamOS onto the headset

Once SteamOS has started, open a terminal with **Ctrl+Alt+T** on the keyboard and run:

```
sudo q2install
```

(A terminal inside Steam's "+" desktop won't work for this, because programs started from Steam aren't allowed to use `sudo`.)

It shows what it will erase and asks you to type `WIPE`. It only ever writes to the partition named `userdata`, the internal storage. Every other partition is only read. Copying takes 10 to 20 minutes.

## Step 5: Switch to the normal boot image

1. Turn the headset off and unplug the USB drive.
2. Boot to the boot menu again (Volume Down + Power) and run:
   ```
   fastboot flash boot boot_steamos.img
   fastboot reboot
   ```

From now on every restart comes back to SteamOS from internal storage.

## Using it

- **First start:** SteamVR starts first, then Steam opens straight into its VR interface. This takes a minute or two after boot.
- **Wi-Fi:** set it up in Steam's settings. Saved networks are kept.
- **Controllers:** they work like in any SteamVR setup. The menu button opens Steam's dashboard.
- **Volume buttons:** change the volume, but there is no sound yet. **Hold both for 2 seconds** to collect the logs (see below).
- **Power button:** a short press dims Steam, because it tries to sleep, which is turned off. Holding it for about 10 seconds forces a restart.
- **Desktop:** the "+" button in Steam's VR interface opens a KDE Plasma desktop inside VR.
- **Windows games:** install and start them from Steam as usual. See [PROTON.md](PROTON.md).

## Getting logs

If something goes wrong, **hold both volume buttons for 2 seconds** (or press **Ctrl+Alt+U** on the keyboard). Then connect the headset to your PC with a USB cable. A small USB drive named `Q2LOGS` appears on the PC with all the logs. After a few minutes, or when you unplug the PC cable, the port goes back to normal. [TROUBLESHOOTING.md](TROUBLESHOOTING.md) explains what's in there.

## Going back to Quest OS

Boot to the boot menu (Volume Down + Power) and flash the stock boot image you saved in step 1:

```
fastboot flash boot boot_stock.img
fastboot reboot
```

Quest OS needs a factory reset after this (Volume Down + Power, then "Factory reset" in the boot menu), because its storage now holds SteamOS. To come back to SteamOS later, start again from step 3.
