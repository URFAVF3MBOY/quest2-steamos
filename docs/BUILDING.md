# Building it yourself

Everything in the release images is built from this repository. You need:

- **GitHub Actions** (a fork of this repository, or the runs of this one) for the compiled parts: kernel, drivers, Mesa.
- **Linux or WSL** with `python3`, `sh` and `sha256sum` to put the boot image together (about 2 minutes).
- **The base image** `boot_steamos_desk204_phaseb4_caps_mmu.img` from the [latest release](../../releases/latest).

The result is `image/out/boot_steamos.img` (SteamOS on the internal storage) or `image/out/boot_steamos_usb.img` (SteamOS on the USB drive, for the first install). Flash it to the **boot partition only** ([INSTALL.md](INSTALL.md)).

The image won't be byte-identical to the released one: the CI builds stamp build times into the kernel and the modules. It behaves the same.

## 1. Get the build inputs

Run these workflows in the **Actions** tab (or download the artifacts of their latest runs here) and put the files in `image/art/`:

| Workflow | Artifact | File(s) for `image/art/` | What it is |
| --- | --- | --- | --- |
| `build-quest2-steamos` | `quest2-steamos` | `boot_steamos.img`, `q2modules.tar.gz` | Meta's Quest 2 kernel 4.19 with patches and config (only the kernel is used), plus the Wi-Fi driver built for it |
| `build-quest2-steamos` | `quest2-audio` | `q2audio.tar.gz` | The Kona sound modules (optional: without them there is no sound) |
| `build-q2msm` | `q2msm` | `q2msm.ko` (in `kernel/q2msm/`) | The GPU kernel driver: mainline Linux drm/msm ported to this kernel |
| `build-q2gpu` | `q2gpu-aarch64` | `q2gpu.tar.gz` | Mesa: Turnip (Vulkan) and freedreno (OpenGL) for the Adreno 650, plus zink. Run it with `debug` ticked only for crash analysis (artifact `q2gpu-aarch64-debug`: symbols kept, Mesa asserts on; not for the image) |
| `build-fuse-overlayfs` | `fuse-overlayfs-aarch64` | `fuse-overlayfs` | fuse-overlayfs 1.18 (the C version), needed by Lepton on kernel 4.19 |
| `build-q2gpu-android` | `q2gpu-android` | `q2gpu-android.tar.gz` | The same Mesa built for Android, for Android apps in Lepton (optional) |

Put the base image in `image/base/`. Its SHA-256 is in `image/inputs.sha256`; `build.sh` checks it.

## 2. Build

```
cd image
sh build.sh            # out/boot_steamos.img      (SteamOS on the internal storage)
USB=1 sh build.sh      # out/boot_steamos_usb.img  (SteamOS on the USB drive)
```

`build.sh` deletes and recreates `image/out/` every time, so copy the image somewhere else before the next run.

What it does:

1. Adds the kernel, the boot script (`image/src/init/init`), the GPU driver and the port's programs to the base image (`tools/inject_boot.py`).
2. Merges all ramdisk layers into one (`tools/flatten_boot.py`); the boot partition is only 80 MiB.
3. Patches Meta's device tree for the stock 587 MHz GPU level (`tools/patch_dtb.py`, two values; it refuses any other device tree).
4. Sets the kernel command line (`tools/rebase_boot.py`): `initcall_blacklist=kgsl_3d_init` (the GPU runs on q2msm, Qualcomm's KGSL driver never starts), `loglevel=4`, SteamVR starts by itself, and for `USB=1` no `q2_rootdev=userdata`.
5. Refuses an image bigger than the 80 MiB boot partition and prints its SHA-256.

`REBUILD=1 sh build.sh` also recompiles the SteamVR driver, the pose reader and the camera tools from `image/src/` with `clang` + `lld` instead of using `image/prebuilt/`. The `build-q2tools` workflow does the same in CI.

## Which init?

`image/src/init/init` is the boot script of the release images, for both variants. Its `find_root()` uses the internal `userdata` partition when the kernel command line has `q2_rootdev=userdata` (`boot_steamos.img`), and otherwise looks for the SteamOS USB drive (`boot_steamos_usb.img`).

`initramfs/init-steamos` and `scripts/build_initramfs.sh` belong to the images that `build-quest2-steamos` packs for itself (USB only, from the early stage of the project). `build.sh` only takes the kernel from that workflow, so they are not part of the release images.

## The base image

`boot_steamos_desk204_phaseb4_caps_mmu.img` is a boot image from an earlier stage of the project. It stays frozen so builds start from the same point. It provides:

- the Android boot header and the kernel command line (which the build then edits),
- Meta's device tree (the same one as in `stock/`; the build patches the GPU levels),
- the small Alpine Linux ramdisk with busybox, bash, util-linux and the `qscreen`, `qlog`, `qrw` and `q2blit` tools,
- the Adreno 650 GPU firmware (`a650_sqe.fw`, `a650_gmu.bin`, from linux-firmware). The boot script also reads the GPU firmware from the headset's own vendor partition.

Its kernel and `init` are replaced during the build.

## What gets built where

| Part | Source | Build |
| --- | --- | --- |
| Kernel | `facebookincubator/oculus-linux-kernel` at commit `6e428607392d` + `patches/*.patch` + `config/` (see below) | `build-quest2-steamos` with AOSP clang r450784e |
| Wi-Fi driver | `qcacld-3.0` in the same kernel tree | same workflow |
| Sound modules | `techpack/audio` in the same kernel tree | same workflow |
| GPU kernel driver (`q2msm.ko`) | `kernel/q2msm/` (mainline drm/msm, adapted) | `build-q2msm` |
| GPU userspace (`/opt/q2gpu`) | Mesa `4e1e46921e` + `patches/turnip-q2-drm.py` + `patches/turnip-q2-async.py` + `patches/turnip-q2-flushpass.py` | `build-q2gpu` |
| Boot script | `image/src/init/init` | copied into the boot image by `build.sh` |
| SteamVR driver | `image/src/hmd/driver_q2hmd.cpp` | `build-q2tools`, or `REBUILD=1 sh build.sh` |
| SteamVR display layer | `image/src/layer/q2vrdisplay.c` + the compute shader `q2warp.comp` | `build-q2layer`; copy the result to `image/prebuilt/` |
| Tracking helpers | `image/src/island/` | `metapose.c` with `build-q2tools` / `REBUILD=1`; `shim.c` and `host.c` need the Android NDK |
| Ruby/Proton fix | `image/src/prof/q2rubyfix` (Python) | nothing to build |
| cabextract | [libmspack](https://github.com/kyz/libmspack) + musl, static aarch64 | prebuilt; see below |
| Bluetooth attach tool (`q2btattach`) | | prebuilt only (`image/prebuilt/q2btattach`) |

### Kernel patches

`build-quest2-steamos` applies these to Meta's tree, in this order:

| Patch | What it does |
| --- | --- |
| `oculus-kernel-fixes.patch` (or `-old`) | Build fixes so the tree builds with AOSP clang outside Meta's build system |
| `oculus-syncboss-old-driver.patch` | An older SyncBoss driver (controllers and IMU) that binds on firmware v59 |
| `q2-dmabuf-sync-file.patch` | Linux 6.0's dma-buf sync-file ioctls, needed by gamescope and Vulkan window-system code |
| `q2-audio-pinctrl-lpi-include.patch`, `q2-audio-no-pdr.patch` | The Kona sound modules build, and start without Android's PD mapper |
| `q2-bt-hci-qca-ldisc.patch` | Bluetooth (QCA6390) over the UART line discipline |
| `q2-display-vtotal.patch` | `/sys/kernel/q2display/vtotal`: the panel's frame length in lines (4916 = 90 Hz, 6145 = 72 Hz), changed without a mode set |

### Rebuilding the Vulkan layer by hand

```
cd image/src/layer
glslangValidator -V --target-env vulkan1.1 --vn q2warp_spv -o q2warp_spv.h q2warp.comp
clang --target=aarch64-linux-gnu -shared -nostdlib -fuse-ld=lld -Wl,-soname,libc.so.6 -o libc.so.6 libcstub.c
clang --target=aarch64-linux-gnu -O2 -fPIC -shared -nostdlib -ffreestanding -fno-stack-protector \
  -fvisibility=hidden -I<vulkan headers> -fuse-ld=lld -Wl,-soname,libq2vrdisplay.so \
  -o libq2vrdisplay.so q2vrdisplay.c libc.so.6
cp libq2vrdisplay.so ../../prebuilt/
```

`<vulkan headers>` is any folder with `vulkan/vulkan.h` (for example from the [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) repository), and `glslangValidator` comes with `glslang-tools`. The `build-q2layer` workflow runs exactly this.

### Rebuilding cabextract

It's a static aarch64 build of cabextract 1.11 (from libmspack) against musl libc, built with clang. Any static aarch64 cabextract works the same. Put it at `image/prebuilt/cabextract`.

## Legacy workflows

`build-quest2-ramonly`, `add-qscreen`, `repack-quest2-steamos`, `build-turnip`, `build-turnip-q2async`, `build-mesa-gl`, `fetch-quest2-kernel-parts` and `probe-steamos-rootfs` come from earlier stages of the project: the RAM-only test system, the first display tests and older GPU driver builds. The current image doesn't use them, but they still work and help when bringing up a new headset.
