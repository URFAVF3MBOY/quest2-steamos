# Building it yourself

You can rebuild everything in this repository. There are two ways:

| Way                                    | You need                                                | You get                                                                                                                 |
| -------------------------------------- | ------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------- |
| **A. Rebuild the release image** | Linux or WSL with `python3`, `sh` and `sha256sum` | The exact same bytes as the released `boot_steamos.img` (about 2 minutes)                                             |
| **B. From source**               | A GitHub account (for Actions), then way A              | A working image built from fresh source. It won't be byte-identical, because the CI builds stamp times into their files |

Either way you finish with `image/out/boot_steamos.img`, which you flash to the **boot partition only** ([INSTALL.md](INSTALL.md)).

## A. Rebuild the release image

1. Download `build-inputs.zip` and `build-inputs.z01` from the [latest release](../../releases/latest) and unzip them (7-Zip, or `zip -s 0 build-inputs.zip --out all.zip && unzip all.zip` on Linux). Put the files here:

   | File                                                                                 | Put it in       |
   | ------------------------------------------------------------------------------------ | --------------- |
   | `boot_steamos_desk204_phaseb4_caps_mmu.img`                                        | `image/base/` |
   | `boot_steamos.img` (the kernel build, not the image you flash)                     | `image/art/`  |
   | `q2modules.tar.gz`, `q2gpu.tar.gz`, `fuse-overlayfs`, `q2gpu-android.tar.gz` | `image/art/`  |

   `SHA256SUMS` in the zip lists the checksums.
2. Run:

   ```
   cd image
   sh build.sh
   ```

The script checks every input against `image/inputs.sha256`. It then adds the kernel, the boot script and the port's programs to the base image, refuses anything bigger than the 80 MiB boot partition, and prints the SHA-256 of the result next to the one in `image/output.sha256`. If they match, you have exactly the released image.

Other options:

- `USB=1 sh build.sh` builds the first-install image (SteamOS from the USB drive).
- `REBUILD=1 sh build.sh` also recompiles the tracking tools, the SteamVR driver and the pose reader with `clang` + `lld`. It behaves the same; the bytes differ if your clang version does.

## B. From source with GitHub Actions

1. Fork this repository (or push it to a repository of your own) and enable Actions.
2. In the **Actions** tab, run these workflows. Each one ends with an artifact to download:| Workflow                 | Artifact                   | Files for `image/art/`                   | What it is                                                                             |
   | ------------------------ | -------------------------- | ------------------------------------------ | -------------------------------------------------------------------------------------- |
   | `build-quest2-steamos` | `quest2-steamos`         | `boot_steamos.img`, `q2modules.tar.gz` | Meta's Quest 2 kernel 4.19 with patches and config, plus the Wi-Fi driver built for it |
   | `build-q2gpu`          | `q2gpu-aarch64`          | `q2gpu.tar.gz`                           | Mesa with patches: Turnip (Vulkan) on Qualcomm's KGSL driver, zink for OpenGL      |
   | `build-fuse-overlayfs` | `fuse-overlayfs-aarch64` | `fuse-overlayfs`                         | fuse-overlayfs 1.18 (the C version), needed by Lepton on kernel 4.19                   |
   | `build-q2gpu-android`  | `q2gpu-android`          | `q2gpu-android.tar.gz`                   | The same Mesa built for Android, for Android apps in Lepton (optional)                 |
3. Unzip each artifact into `image/art/`. Get the base image from the release (see below), then run `sh build.sh` in `image/`. It will say the inputs differ from the release; that's expected.

### The base image

`image/base/boot_steamos_desk204_phaseb4_caps_mmu.img` is a boot image from an earlier stage of the project, and we keep it frozen because the release hashes depend on it. It provides:

- the Android boot header and the kernel command line (the build changes `loglevel` and, for `USB=1`, the root device),
- Meta's device tree (the same one as in `stock/`),
- the small Alpine Linux ramdisk with busybox, bash, util-linux and  `qscreen`, `qlog`, `qrw` and `q2blit` tools,
- the Adreno 650 GPU firmware (`a650_sqe.fw`, `a650_gmu.bin`, from linux-firmware). The boot script also reads the GPU firmware from the headset's own vendor partition.

The kernel and `init` from the base image are replaced during the build. The ramdisk comes from `initramfs/` and `scripts/build_initramfs.sh`, which the `build-quest2-steamos` workflow also packs.

## What gets built where

| Part                        | Source                                                                                                   | Build                                                                             |
| --------------------------- | -------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------- |
| Kernel                      | `facebookincubator/oculus-linux-kernel` at commit `6e428607392d` + `patches/*.patch` + `config/` | `build-quest2-steamos` with AOSP clang r450784e                                 |
| Wi-Fi driver                | `qcacld-3.0` in the same kernel tree                                                                   | same workflow                                                                     |
| GPU driver (`/opt/q2gpu`) | Mesa `4e1e46921e` + `patches/turnip-q2-drm.py` + `patches/turnip-q2-async.py`                      | `build-q2gpu`                                                                   |
| Boot script                 | `image/src/init/init`                                                                                  | copied into the boot image by `build.sh`                                        |
| SteamVR driver              | `image/src/hmd/driver_q2hmd.cpp`                                                                       | `REBUILD=1 sh build.sh`                                                         |
| SteamVR display layer       | `image/src/layer/q2vrdisplay.c`                                                                        | by hand, see below                                                                |
| Tracking helpers            | `image/src/island/`                                                                                    | `metapose.c` with `REBUILD=1`; `shim.c` and `host.c` need the Android NDK |
| Ruby/Proton fix             | `image/src/prof/q2rubyfix` (Python)                                                                    | nothing to build                                                                  |
| cabextract                  | [libmspack](https://github.com/kyz/libmspack) + musl, static aarch64                                      | prebuilt; see below                                                               |

### Rebuilding the Vulkan layer

```
cd image/src/layer
clang --target=aarch64-linux-gnu -shared -nostdlib -fuse-ld=lld -Wl,-soname,libc.so.6 -o libc.so.6 libcstub.c
clang --target=aarch64-linux-gnu -O2 -fPIC -shared -nostdlib -ffreestanding -fno-stack-protector \
  -fvisibility=hidden -I<vulkan headers> -fuse-ld=lld -Wl,-soname,libq2vrdisplay.so \
  -o libq2vrdisplay.so q2vrdisplay.c libc.so.6
cp libq2vrdisplay.so ../../prebuilt/
```

`<vulkan headers>` is any folder with `vulkan/vulkan.h` (for example from the [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) repository).

### Rebuilding cabextract

It's a static aarch64 build of cabextract 1.11 (from libmspack) against musl libc, built with clang. Any static aarch64 cabextract works the same. Put it at `image/prebuilt/cabextract`.

## Legacy workflows

`build-quest2-ramonly`, `add-qscreen`, `repack-quest2-steamos`, `build-turnip`, `build-turnip-q2async`, `build-mesa-gl`, `fetch-quest2-kernel-parts` and `probe-steamos-rootfs` come from earlier stages of the project: the RAM-only test system, the first display tests and older GPU driver builds. The current image doesn't use them, but they still work and help when bringing up a new headset.
