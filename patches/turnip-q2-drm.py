#!/usr/bin/env python3
# turnip-q2-drm.py - pair Turnip-on-KGSL with the Quest 2's display DRM node, so the normal Linux graphics stack works
# on the GPU while KGSL stays the GPU driver (the way the stock Quest 2 runs: KGSL renders, Meta's display driver shows).
#
# The Quest 2 kernel has two GPU-related devices:
#   /dev/kgsl-3d0                         Adreno 650 (KGSL) - Turnip renders here
#   /dev/dri/card0 + /dev/dri/renderD128  "msm_drm" = Qualcomm SDE display driver (KMS + GEM/PRIME, no GPU)
# Stock Turnip-on-KGSL reports no DRM node at all (no VK_EXT_physical_device_drm), so everything that needs one fails or
# falls back to CPU copies: GBM, Xwayland/Xorg glamor, DRI3 for Vulkan/GL clients, gamescope, Chromium/CEF native
# pixmaps ("dri3 extension not supported").  With this patch:
#   1. Turnip-on-KGSL finds the msm_drm render node and reports it (and card0) through VK_EXT_physical_device_drm.
#      TU_KGSL_DRM_NODE=/dev/dri/renderDxxx picks a node by hand, TU_KGSL_DRM_NODE=0 turns the pairing off.
#   2. VK_KHR_display instances (gamescope) are no longer refused on KGSL (desk-253 log: gamescope 'failed to find
#      physical device'); no display fd is opened, so the display stays with its current owner.
#   3. Turnip's WSI accepts that node as "its" device (DRI3/Wayland present without the different-GPU blit path).
#   4. Mesa's loader maps the msm_drm kernel driver to zink, so GBM, EGL and GLX on that node run on Turnip (KGSL).
#      Buffers are Turnip's dma-bufs (ION); msm_drm only imports them (PRIME) for scanout and handle bookkeeping.
#      Q2_NO_MSM_DRM_ZINK=1 turns the mapping off.
# Every change is logged to stderr as "q2-turnip-drm: ..." once per process.
#
# Usage: python3 turnip-q2-drm.py <mesa source dir>     (fails loudly if an anchor is not found)
import re, sys, os

root = sys.argv[1] if len(sys.argv) > 1 else "."

def edit(rel, fn):
    p = os.path.join(root, rel)
    s = open(p).read()
    t = fn(s)
    if t == s:
        sys.exit(f"turnip-q2-drm: {rel}: nothing changed (anchor missing?)")
    open(p, "w").write(t)
    print(f"turnip-q2-drm: patched {rel}")

def rep1(s, old, new, what):
    if s.count(old) != 1:
        sys.exit(f"turnip-q2-drm: anchor not found exactly once ({s.count(old)}x): {what}")
    return s.replace(old, new, 1)

# --- tu_device.h: remember the paired node -------------------------------------------------------------------
def dev_h(s):
    return rep1(s, "   int kgsl_dma_fd;\n",
                "   int kgsl_dma_fd;\n"
                "   /* q2: KGSL paired with the display's DRM node (msm_drm) - see turnip-q2-drm.py */\n"
                "   bool q2_has_drm;\n"
                "   int q2_drm_fd;\n", "tu_physical_device.kgsl_dma_fd")
edit("src/freedreno/vulkan/tu_device.h", dev_h)

# --- tu_knl_kgsl.cc: find the node -------------------------------------------------------------------------------
PAIR_FN = r'''
/* q2: pair KGSL with the display's DRM node (Qualcomm SDE "msm_drm"), so VK_EXT_physical_device_drm, GBM (zink), DRI3
 * and Wayland dma-buf presentation work.  TU_KGSL_DRM_NODE=<path> picks the node, TU_KGSL_DRM_NODE=0 disables. */
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include "drm-uapi/drm.h"

static void
q2_kgsl_pair_drm(struct tu_physical_device *device)
{
   const char *want = os_get_option("TU_KGSL_DRM_NODE");
   if (want && strcmp(want, "0") == 0)
      return;

   char path[64] = { 0 }, name[32];
   int fd = -1;
   for (int i = 128; i < 192 && fd < 0; i++) {
      if (want && *want) {
         snprintf(path, sizeof(path), "%s", want);
         i = 192;
      } else {
         snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
      }
      int f = open(path, O_RDWR | O_CLOEXEC);
      if (f < 0)
         continue;
      struct drm_version v;
      memset(&v, 0, sizeof(v));
      memset(name, 0, sizeof(name));
      v.name = name;
      v.name_len = sizeof(name) - 1;
      if (ioctl(f, DRM_IOCTL_VERSION, &v) == 0 &&
          ((want && *want) || strcmp(name, "msm_drm") == 0))
         fd = f;
      else
         close(f);
   }

   struct stat st;
   if (fd < 0 || fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode)) {
      if (fd >= 0)
         close(fd);
      fprintf(stderr, "q2-turnip-drm: no msm_drm render node found - running unpaired (no GBM/DRI3)\n");
      return;
   }

   device->q2_drm_fd = fd;
   device->q2_has_drm = true;
   device->has_local = true;
   device->local_major = major(st.st_rdev);
   device->local_minor = minor(st.st_rdev);

   /* DRM numbering: render node minor = 128 + index, primary node minor = index */
   struct stat pst;
   char ppath[64];
   snprintf(ppath, sizeof(ppath), "/dev/dri/card%u", (unsigned) (minor(st.st_rdev) & 63));
   if (stat(ppath, &pst) == 0 && S_ISCHR(pst.st_mode) && major(pst.st_rdev) == major(st.st_rdev)) {
      device->has_master = true;
      device->master_major = major(pst.st_rdev);
      device->master_minor = minor(pst.st_rdev);
   }

   static bool logged;
   if (!logged) {
      logged = true;
      fprintf(stderr, "q2-turnip-drm: KGSL paired with %s (%s, %d:%d)%s%s\n", path, name,
              (int) device->local_major, (int) device->local_minor,
              device->has_master ? ", primary " : "", device->has_master ? ppath : "");
   }
}
'''
KHR_DISPLAY_OLD = """   if (instance->vk.enabled_extensions.KHR_display) {
      return vk_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                       "I can't KHR_display");
   }
"""
KHR_DISPLAY_NEW = """   /* q2: stock Turnip-on-KGSL refuses every instance that enables VK_KHR_display, which hides the GPU from gamescope
    * (it always enables it: "failed to find physical device").  Allow it; the display fd stays -1 here (never open
    * card0 ourselves: the first opener would become DRM master and take the display from q2kms), so the instance simply
    * sees no displays unless the app hands over its own fd (vkAcquireDrmDisplayEXT). */
"""

def kgsl_cc(s):
    s = rep1(s, KHR_DISPLAY_OLD, KHR_DISPLAY_NEW, "KGSL KHR_display refusal")
    s = rep1(s, "\nVkResult\ntu_knl_kgsl_load(struct tu_instance *instance, int fd)\n",
             PAIR_FN + "\nVkResult\ntu_knl_kgsl_load(struct tu_instance *instance, int fd)\n", "tu_knl_kgsl_load")
    s = rep1(s, "   device->kgsl_dma_fd = dma_fd;\n",
             "   device->kgsl_dma_fd = dma_fd;\n   q2_kgsl_pair_drm(device);\n", "device->kgsl_dma_fd = dma_fd")
    # the pairing fd must not leak if the physical device fails later in tu_knl_kgsl_load
    s = rep1(s, "fail:\n   vk_free(&instance->vk.alloc, device);\n   if (dma_fd >= 0)\n      close(dma_fd);\n",
             "fail:\n   if (device->q2_has_drm)\n      close(device->q2_drm_fd);\n"
             "   vk_free(&instance->vk.alloc, device);\n   if (dma_fd >= 0)\n      close(dma_fd);\n", "kgsl load fail path")
    for inc in ("#include <string.h>\n", "#include <stdio.h>\n"):
        if inc not in s:
            s = inc + s
    return s
edit("src/freedreno/vulkan/tu_knl_kgsl.cc", kgsl_cc)

# --- tu_device.cc: advertise VK_EXT_physical_device_drm when paired; close the fd -------------------------------
def dev_cc(s):
    s = rep1(s, ".EXT_physical_device_drm = !is_kgsl(device->instance),",
             ".EXT_physical_device_drm = !is_kgsl(device->instance) || device->q2_has_drm, /* q2 */",
             "EXT_physical_device_drm")
    s = rep1(s, "   if (device->kgsl_dma_fd != -1)\n      close(device->kgsl_dma_fd);\n",
             "   if (device->kgsl_dma_fd != -1)\n      close(device->kgsl_dma_fd);\n"
             "   if (device->q2_has_drm)\n      close(device->q2_drm_fd);\n", "physical device finish")
    return s
edit("src/freedreno/vulkan/tu_device.cc", dev_cc)

# --- tu_wsi.cc: the paired node counts as this device ----------------------------------------------------------
def wsi_cc(s):
    return rep1(s, "   return wsi_common_drm_devices_equal(fd, pdevice->local_fd);\n",
                "   if (pdevice->q2_has_drm) /* q2: KGSL paired with msm_drm */\n"
                "      return wsi_common_drm_devices_equal(fd, pdevice->q2_drm_fd);\n"
                "   return wsi_common_drm_devices_equal(fd, pdevice->local_fd);\n", "tu_wsi_can_present_on_device")
edit("src/freedreno/vulkan/tu_wsi.cc", wsi_cc)

# --- loader.c: msm_drm -> zink (GBM, EGL, GLX pick the kopper/zink screen for this node) ---------------------------
def loader_c(s):
    return rep1(s, "   driver = loader_get_pci_driver(fd);\n   if (!driver)\n      driver = loader_get_kernel_driver_name(fd);\n",
                "   driver = loader_get_pci_driver(fd);\n   if (!driver)\n      driver = loader_get_kernel_driver_name(fd);\n\n"
                "   /* q2: Qualcomm SDE display node (msm_drm) + KGSL GPU: no Gallium driver; GL/GBM run on zink (Turnip-KGSL) */\n"
                "   if (driver && strcmp(driver, \"msm_drm\") == 0 && !os_get_option(\"Q2_NO_MSM_DRM_ZINK\")) {\n"
                "      free(driver);\n"
                "      driver = strdup(\"zink\");\n"
                "   }\n", "loader_get_driver_for_fd")
edit("src/loader/loader.c", loader_c)

# --- pipe_loader_drm.c: same mapping for paths that probe the fd directly ---------------------------------------
def pipe_c(s):
    return rep1(s, "   /* powervr has no Gallium driver and relies on Zink */\n",
                "   /* q2: msm_drm (Qualcomm SDE display, GPU on KGSL) has no Gallium driver and relies on Zink */\n"
                "   if (strcmp(ddev->base.driver_name, \"msm_drm\") == 0) {\n"
                "      FREE(ddev->base.driver_name);\n"
                "      ddev->base.driver_name = strdup(\"zink\");\n"
                "   }\n\n"
                "   /* powervr has no Gallium driver and relies on Zink */\n", "pipe loader powervr block")
edit("src/gallium/auxiliary/pipe-loader/pipe_loader_drm.c", pipe_c)
print("turnip-q2-drm: done")
