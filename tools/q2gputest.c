/*
 * q2gputest - checks the Quest 2 "KGSL + msm_drm" GPU stack (/opt/q2gpu, patches/turnip-q2-drm.py and the kernel's
 * patches/q2-dmabuf-sync-file.patch) without touching the display.  Each step prints "PASS"/"FAIL"/"INFO"; the exit
 * code is the number of FAILed steps.
 *   1. Vulkan: Turnip-on-KGSL reports the msm_drm render node (VK_EXT_physical_device_drm)
 *   2. GBM on /dev/dri/renderD128: backend + a scanout-capable buffer, exported as a dma-buf
 *   3. EGL/GLES on that GBM device: renderer must be zink on Turnip (the GPU), draw a colour, read it back
 *   4. the same pixels through the dma-buf mapping (GPU wrote into memory another process could use = zero copy)
 *   5. dma-buf sync_file export/import ioctls (kernel patch)
 *   6. msm_drm (card0) accepts the buffer as a framebuffer (PRIME import + ADDFB2) - only a test FB, nothing is shown
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <vulkan/vulkan.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <linux/dma-buf.h>

#ifndef DMA_BUF_IOCTL_EXPORT_SYNC_FILE
struct dma_buf_export_sync_file { __u32 flags; __s32 fd; };
struct dma_buf_import_sync_file { __u32 flags; __s32 fd; };
#define DMA_BUF_IOCTL_EXPORT_SYNC_FILE _IOWR(DMA_BUF_BASE, 2, struct dma_buf_export_sync_file)
#define DMA_BUF_IOCTL_IMPORT_SYNC_FILE _IOW(DMA_BUF_BASE, 3, struct dma_buf_import_sync_file)
#endif

static int fails;
#define PASS(...) do { printf("PASS  "); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define FAIL(...) do { printf("FAIL  "); printf(__VA_ARGS__); printf("\n"); fflush(stdout); fails++; } while (0)
#define INFO(...) do { printf("INFO  "); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static void test_vulkan(void)
{
   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
   VkInstance inst;
   if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { FAIL("vulkan: vkCreateInstance"); return; }
   uint32_t n = 8; VkPhysicalDevice pd[8];
   vkEnumeratePhysicalDevices(inst, &n, pd);
   if (!n) { FAIL("vulkan: no physical device"); vkDestroyInstance(inst, NULL); return; }
   for (uint32_t i = 0; i < n; i++) {
      uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(pd[i], NULL, &ne, NULL);
      VkExtensionProperties *ep = calloc(ne, sizeof(*ep));
      vkEnumerateDeviceExtensionProperties(pd[i], NULL, &ne, ep);
      int has_drm = 0, has_dmabuf = 0, has_mod = 0, has_syncfd = 0;
      for (uint32_t e = 0; e < ne; e++) {
         if (!strcmp(ep[e].extensionName, "VK_EXT_physical_device_drm")) has_drm = 1;
         if (!strcmp(ep[e].extensionName, "VK_EXT_external_memory_dma_buf")) has_dmabuf = 1;
         if (!strcmp(ep[e].extensionName, "VK_EXT_image_drm_format_modifier")) has_mod = 1;
         if (!strcmp(ep[e].extensionName, "VK_KHR_external_semaphore_fd")) has_syncfd = 1;
      }
      free(ep);
      VkPhysicalDeviceDrmPropertiesEXT drm = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT };
      VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = has_drm ? &drm : NULL };
      vkGetPhysicalDeviceProperties2(pd[i], &p2);
      INFO("vulkan: device %u '%s' api %u.%u.%u, dma_buf %d, modifiers %d, semaphore_fd %d", i, p2.properties.deviceName,
           VK_VERSION_MAJOR(p2.properties.apiVersion), VK_VERSION_MINOR(p2.properties.apiVersion),
           VK_VERSION_PATCH(p2.properties.apiVersion), has_dmabuf, has_mod, has_syncfd);
      if (has_drm && drm.hasRender)
         PASS("vulkan: VK_EXT_physical_device_drm render %lld:%lld primary %s%lld:%lld", (long long) drm.renderMajor,
              (long long) drm.renderMinor, drm.hasPrimary ? "" : "(none) ", (long long) drm.primaryMajor, (long long) drm.primaryMinor);
      else
         FAIL("vulkan: no DRM node reported (VK_EXT_physical_device_drm %s)", has_drm ? "present, no render node" : "missing");
   }
   vkDestroyInstance(inst, NULL);
}

static int sync_file_test(int dmabuf)
{
   struct dma_buf_export_sync_file ex = { .flags = DMA_BUF_SYNC_WRITE, .fd = -1 };
   if (ioctl(dmabuf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &ex)) {
      FAIL("dma-buf: EXPORT_SYNC_FILE: %s%s", strerror(errno), errno == ENOTTY ? " (kernel without q2-dmabuf-sync-file.patch)" : "");
      return -1;
   }
   PASS("dma-buf: EXPORT_SYNC_FILE -> sync_file fd %d", ex.fd);
   struct dma_buf_import_sync_file im = { .flags = DMA_BUF_SYNC_WRITE, .fd = ex.fd };
   if (ioctl(dmabuf, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &im))
      FAIL("dma-buf: IMPORT_SYNC_FILE (write): %s", strerror(errno));
   else
      PASS("dma-buf: IMPORT_SYNC_FILE (write)");
   im.flags = DMA_BUF_SYNC_READ;
   if (ioctl(dmabuf, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &im))
      FAIL("dma-buf: IMPORT_SYNC_FILE (read): %s", strerror(errno));
   else
      PASS("dma-buf: IMPORT_SYNC_FILE (read)");
   close(ex.fd);
   return 0;
}

int main(int argc, char **argv)
{
   const char *node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
   const int W = 256, H = 256;
   setvbuf(stdout, NULL, _IOLBF, 0);

   test_vulkan();

   int fd = open(node, O_RDWR | O_CLOEXEC);
   if (fd < 0) { FAIL("open %s: %s", node, strerror(errno)); return fails; }
   drmVersionPtr v = drmGetVersion(fd);
   INFO("drm: %s is '%s' %d.%d.%d", node, v ? v->name : "?", v ? v->version_major : 0, v ? v->version_minor : 0, v ? v->version_patchlevel : 0);
   if (v) drmFreeVersion(v);

   struct gbm_device *gbm = gbm_create_device(fd);
   if (!gbm) { FAIL("gbm: gbm_create_device failed"); return fails; }
   PASS("gbm: device on %s, backend '%s'", node, gbm_device_get_backend_name(gbm));

   struct gbm_surface *gs = gbm_surface_create(gbm, W, H, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
   if (!gs) gs = gbm_surface_create(gbm, W, H, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING);
   if (!gs) { FAIL("gbm: gbm_surface_create failed"); return fails; }

   PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *) eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay dpy = gpd ? gpd(EGL_PLATFORM_GBM_KHR, gbm, NULL) : eglGetDisplay((EGLNativeDisplayType) gbm);
   EGLint maj, min;
   if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) { FAIL("egl: initialize on GBM: 0x%x", eglGetError()); return fails; }
   INFO("egl: %d.%d vendor '%s'", maj, min, eglQueryString(dpy, EGL_VENDOR));
   eglBindAPI(EGL_OPENGL_ES_API);
   EGLint ca[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                   EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
   EGLConfig cfgs[64]; EGLint nc = 0, pick = -1;
   eglChooseConfig(dpy, ca, cfgs, 64, &nc);
   for (int i = 0; i < nc; i++) { EGLint id; eglGetConfigAttrib(dpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &id); if (id == GBM_FORMAT_XRGB8888) { pick = i; break; } }
   if (pick < 0) { FAIL("egl: no XRGB8888 config (%d configs)", nc); return fails; }
   EGLint cta[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext ctx = eglCreateContext(dpy, cfgs[pick], EGL_NO_CONTEXT, cta);
   EGLSurface es = eglCreateWindowSurface(dpy, cfgs[pick], (EGLNativeWindowType) gs, NULL);
   if (ctx == EGL_NO_CONTEXT || es == EGL_NO_SURFACE || !eglMakeCurrent(dpy, es, es, ctx)) { FAIL("egl: context/surface: 0x%x", eglGetError()); return fails; }
   const char *rend = (const char *) glGetString(GL_RENDERER);
   if (rend && (strstr(rend, "Adreno") || strstr(rend, "urnip")))
      PASS("gl: renderer '%s' (%s)", rend, glGetString(GL_VERSION));
   else
      FAIL("gl: renderer '%s' is not the Adreno", rend ? rend : "?");

   glClearColor(0.0f, 1.0f, 0.0f, 1.0f); /* green */
   glClear(GL_COLOR_BUFFER_BIT);
   uint8_t px[4] = { 0 };
   glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
   if (px[0] < 10 && px[1] > 245 && px[2] < 10) PASS("gl: drew green, read back %d,%d,%d", px[0], px[1], px[2]);
   else FAIL("gl: read back %d,%d,%d instead of green", px[0], px[1], px[2]);
   if (!eglSwapBuffers(dpy, es)) { FAIL("egl: swap 0x%x", eglGetError()); return fails; }

   struct gbm_bo *bo = gbm_surface_lock_front_buffer(gs);
   if (!bo) { FAIL("gbm: lock_front_buffer"); return fails; }
   uint64_t mod = gbm_bo_get_modifier(bo);
   uint32_t stride = gbm_bo_get_stride(bo);
   int dmabuf = gbm_bo_get_fd(bo);
   if (dmabuf < 0) { FAIL("gbm: bo -> dma-buf export failed"); return fails; }
   PASS("gbm: front buffer %dx%d stride %u modifier 0x%llx -> dma-buf fd %d", W, H, stride, (unsigned long long) mod, dmabuf);

   if (mod == DRM_FORMAT_MOD_LINEAR || mod == DRM_FORMAT_MOD_INVALID) {
      sync_file_test(dmabuf); /* also waits nothing; the export below makes the CPU read wait for the GPU */
      struct dma_buf_sync s = { DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
      ioctl(dmabuf, DMA_BUF_IOCTL_SYNC, &s);
      uint8_t *m = mmap(NULL, (size_t) stride * H, PROT_READ, MAP_SHARED, dmabuf, 0);
      if (m == MAP_FAILED) FAIL("dma-buf: mmap: %s", strerror(errno));
      else {
         uint8_t *p = m + (size_t) (H / 2) * stride + (W / 2) * 4; /* XRGB8888 little endian: B G R X */
         if (p[1] > 245 && p[0] < 10 && p[2] < 10) PASS("dma-buf: CPU sees the GPU's green through the shared buffer (zero copy)");
         else FAIL("dma-buf: CPU sees B%d G%d R%d instead of green", p[0], p[1], p[2]);
         munmap(m, (size_t) stride * H);
      }
      s.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
      ioctl(dmabuf, DMA_BUF_IOCTL_SYNC, &s);
   } else {
      INFO("dma-buf: tiled/compressed modifier - CPU readback skipped");
      sync_file_test(dmabuf);
   }

   /* scanout side: does the display driver take the GPU's buffer as a framebuffer? (no modeset, nothing shown) */
   int card = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
   if (card < 0) INFO("kms: /dev/dri/card0: %s - skipped", strerror(errno));
   else {
      uint32_t h = 0, fb = 0;
      if (drmPrimeFDToHandle(card, dmabuf, &h)) {
         if (errno == EACCES) INFO("kms: card0 refuses PRIME import from a non-master, unauthenticated client (normal for apps; the compositor owns card0)");
         else FAIL("kms: PRIME import into card0: %s", strerror(errno));
      }
      else {
         uint32_t hs[4] = { h }, ps[4] = { stride }, os[4] = { 0 };
         uint64_t ms[4] = { mod };
         int r = mod == DRM_FORMAT_MOD_INVALID ? drmModeAddFB2(card, W, H, DRM_FORMAT_XRGB8888, hs, ps, os, &fb, 0)
                 : drmModeAddFB2WithModifiers(card, W, H, DRM_FORMAT_XRGB8888, hs, ps, os, ms, &fb, DRM_MODE_FB_MODIFIERS);
         if (r) INFO("kms: ADDFB2 refused (%s) - expected while another process is DRM master", strerror(-r > 0 ? -r : errno));
         else { PASS("kms: card0 accepted it as framebuffer %u", fb); drmModeRmFB(card, fb); }
         struct drm_gem_close gc = { .handle = h };
         drmIoctl(card, DRM_IOCTL_GEM_CLOSE, &gc);
         PASS("kms: PRIME import into card0 (handle %u)", h);
      }
      close(card);
   }

   close(dmabuf);
   gbm_surface_release_buffer(gs, bo);
   eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglTerminate(dpy);
   gbm_surface_destroy(gs);
   gbm_device_destroy(gbm);
   close(fd);
   printf("RESULT %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
   return fails;
}
