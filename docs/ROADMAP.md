# Roadmap

Roughly in the order they are planned.

## 1. Locked headsets through kexec

**Goal:** run SteamOS on a Quest 2 **without unlocking the bootloader**.

Kexec lets a running Linux kernel load and start another kernel directly, without going through the bootloader: a kernel module loaded into the running Quest OS kernel boots a custom kernel and initramfs from there.

The plan:

1. Build the kexec modules for the exact Quest 2 kernel version of the supported firmware.
2. Capture the Quest 2's runtime device tree and work out what the target kernel needs to take over the hardware without a full reset: display, GPU, USB and SyncBoss.
3. Boot the SteamOS kernel and boot script through kexec instead of `fastboot flash boot`. The rest of the port stays the same.
4. Add a "return to Quest OS" path, which is just a normal reboot.

**What it still needs:** root access on Quest OS, to load a kernel module. On a locked headset that means a root method for the firmware version on the headset. Kexec removes the need to unlock the bootloader, not the need for root.

## 2. Sound

The Quest 2 uses Qualcomm's audio DSP (ADSP) for its speakers and microphones. In the kernel, no sound card registers ("ALSA device list:" is empty). The parts are Meta's audio drivers in the kernel tree, the DSP firmware from the vendor partition, and a mixer setup taken from Android's `mixer_paths.xml`. Until then, PipeWire has no output, and some games refuse to start. Adding a silent output (the `nonullsink` switch) didn't work yet, because PipeWire's pulse layer refused it ("Not supported").

## 3. A 72 Hz panel mode, switched automatically

The theater screen of a 2D game makes SteamVR's compositor need about 12 ms of GPU time per frame, more than the 11.1 ms of a 90 Hz frame, so it runs at about 67 fps with judder. The kernel switch `/sys/kernel/q2display/vtotal` runs the panel at 72 Hz (13.9 ms per frame) by lengthening the vertical blanking; `vrhz` = `72` uses it for a whole SteamVR session. Next: switch to 72 Hz only while a 2D game is on the theater screen.

## 4. A smoother Steam interface

Steam's VR panels still stutter when Steam is busy. The next things to look at are the web interface's GPU process, and whole-panel redraws when only part of the panel changes.

## 5. Power menu, sleep and wake

The power button's short and long presses reach Steam (`steam://shortpowerpress`), but Steam's power menu doesn't open. Steam probably expects something from Valve's session manager that isn't running here. Sleep is turned off, because the panel and the GPU don't come back yet.

## 6. VRChat and Android apps

Lepton (Valve's Android container) starts Android, with networking and the GPU driver. VRChat doesn't run yet.

## Also on the list

- Testing x86 Linux games through FEX-Emu.
- KernelSU for the Quest 2 kernel.
- A GitHub workflow that builds the final boot image as well, so a fork can produce everything without a Linux PC.
- Reporting the Ruby problem to Valve, so the Proton fix becomes unnecessary.
