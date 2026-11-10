#!/usr/bin/env python3
# turnip-q2-ssem.py - cross-process OPAQUE_FD semaphores for Turnip on KGSL, ANDROID build (Lepton apps).
#
# SteamVR shares timeline semaphores between its processes as OPAQUE_FD.  KGSL has no cross-process sync objects, so
# Turnip's KGSL backend cannot import or export them: VRChat crashed in Lepton with a jump to address 0 inside
# vulkan.freedreno.so, called from SteamVR's Android client (vrclient.so CSharedSemaphoreLinux::ImportVulkanSemaphore,
# creating its first swapchain; 2026-10-20 log).
# On the Linux side our display layer already emulates these semaphores (q2vrdisplay.c "cross-process timeline
# semaphores"): the OPAQUE_FD is a 4 KiB memfd holding one 64-bit counter; every process keeps an ordinary local
# timeline semaphore and copies progress both ways about every millisecond.  Android apps don't load that layer, so
# this patch puts the same scheme into the Android Turnip itself (it talks to the compositor's layer through the
# shared counter):
#   - vkImportSemaphoreFdKHR(OPAQUE_FD) on a timeline semaphore: map the memfd, keep the semaphore in step with it;
#   - vkGetSemaphoreFdKHR(OPAQUE_FD): make such a memfd for the semaphore;
#   - vkGetPhysicalDeviceExternalSemaphoreProperties: report OPAQUE_FD as importable/exportable for timelines;
#   - vkDestroySemaphore forgets it.  Everything else goes to Mesa's common code as before.
# Usage: python3 turnip-q2-ssem.py <mesa source dir>      (Android Turnip build only; fails loudly on a missing anchor)
import os, sys

root = sys.argv[1] if len(sys.argv) > 1 else "."
V = os.path.join(root, "src/freedreno/vulkan")

SRC = r'''/* q2: cross-process OPAQUE_FD timeline semaphores on KGSL (patches/turnip-q2-ssem.py) */
#include "tu_device.h"
#include "vk_semaphore.h"
#include "vk_util.h"
#include "vk_common_entrypoints.h"
#include "util/log.h"

#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define Q2S_MAX 1024
struct q2s_entry {
   VkDevice dev;
   VkSemaphore sem;
   volatile uint64_t *ctr;
   int fd;
};
static struct q2s_entry q2s_tab[Q2S_MAX];
static int q2s_hi;
static pthread_mutex_t q2s_lock = PTHREAD_MUTEX_INITIALIZER;
static bool q2s_thread_on;

static bool
q2s_is_timeline(VkSemaphore sem)
{
   VK_FROM_HANDLE(vk_semaphore, s, sem);
   return s && s->type == VK_SEMAPHORE_TYPE_TIMELINE;
}

static void
q2s_sync(struct q2s_entry *e)
{
   uint64_t l = 0;
   if (vk_common_GetSemaphoreCounterValue(e->dev, e->sem, &l) != VK_SUCCESS)
      return;
   uint64_t sh = __atomic_load_n(e->ctr, __ATOMIC_ACQUIRE);
   while (l > sh) {
      if (__atomic_compare_exchange_n(e->ctr, &sh, l, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
         break;
   }
   if (sh > l) {
      VkSemaphoreSignalInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, e->sem, sh };
      vk_common_SignalSemaphore(e->dev, &si);
   }
}

static void *
q2s_thread(void *arg)
{
   (void)arg;
   for (;;) {
      pthread_mutex_lock(&q2s_lock);
      for (int i = 0; i < q2s_hi; i++)
         if (q2s_tab[i].ctr)
            q2s_sync(&q2s_tab[i]);
      pthread_mutex_unlock(&q2s_lock);
      struct timespec ts = { 0, 1000000 };
      nanosleep(&ts, NULL);
   }
   return NULL;
}

/* q2s_lock held */
static VkResult
q2s_add(VkDevice dev, VkSemaphore sem, int fd)
{
   void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   if (m == MAP_FAILED) {
      mesa_loge("q2: shared semaphore: mmap of fd %d failed (%d)", fd, errno);
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }
   int slot = -1;
   for (int i = 0; i < q2s_hi; i++)
      if (!q2s_tab[i].ctr) { slot = i; break; }
   if (slot < 0) {
      if (q2s_hi == Q2S_MAX) {
         munmap(m, 4096);
         mesa_loge("q2: shared semaphore: table full");
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
      slot = q2s_hi++;
   }
   q2s_tab[slot] = (struct q2s_entry) { dev, sem, (volatile uint64_t *) m, fd };
   if (!q2s_thread_on) {
      pthread_t t;
      if (pthread_create(&t, NULL, q2s_thread, NULL) == 0) {
         pthread_detach(t);
         q2s_thread_on = true;
      }
   }
   return VK_SUCCESS;
}

static void
q2s_forget(VkSemaphore sem)
{
   pthread_mutex_lock(&q2s_lock);
   for (int i = 0; i < q2s_hi; i++) {
      if (q2s_tab[i].ctr && q2s_tab[i].sem == sem) {
         munmap((void *) q2s_tab[i].ctr, 4096);
         close(q2s_tab[i].fd);
         q2s_tab[i] = (struct q2s_entry) { 0 };
      }
   }
   while (q2s_hi > 0 && !q2s_tab[q2s_hi - 1].ctr)
      q2s_hi--;
   pthread_mutex_unlock(&q2s_lock);
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_ImportSemaphoreFdKHR(VkDevice device, const VkImportSemaphoreFdInfoKHR *info)
{
   if (info->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT &&
       q2s_is_timeline(info->semaphore)) {
      pthread_mutex_lock(&q2s_lock);
      VkResult r = q2s_add(device, info->semaphore, info->fd);   /* takes the fd */
      if (r == VK_SUCCESS) {
         for (int i = 0; i < q2s_hi; i++)
            if (q2s_tab[i].sem == info->semaphore && q2s_tab[i].ctr)
               q2s_sync(&q2s_tab[i]);
      }
      pthread_mutex_unlock(&q2s_lock);
      mesa_logi("q2: shared semaphore imported (fd %d) -> %d", info->fd, r);
      return r;
   }
   return vk_common_ImportSemaphoreFdKHR(device, info);
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_GetSemaphoreFdKHR(VkDevice device, const VkSemaphoreGetFdInfoKHR *info, int *pFd)
{
   if (info->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT &&
       q2s_is_timeline(info->semaphore)) {
      int fd = (int) syscall(__NR_memfd_create, "q2vrsem", 0u);
      if (fd < 0 || ftruncate(fd, 4096) != 0) {
         if (fd >= 0) close(fd);
         return VK_ERROR_TOO_MANY_OBJECTS;
      }
      int out = dup(fd);
      pthread_mutex_lock(&q2s_lock);
      VkResult r = q2s_add(device, info->semaphore, fd);
      if (r == VK_SUCCESS) {
         for (int i = 0; i < q2s_hi; i++)
            if (q2s_tab[i].sem == info->semaphore && q2s_tab[i].ctr)
               q2s_sync(&q2s_tab[i]);
      }
      pthread_mutex_unlock(&q2s_lock);
      if (r != VK_SUCCESS) { close(out); close(fd); return r; }
      *pFd = out;
      return VK_SUCCESS;
   }
   return vk_common_GetSemaphoreFdKHR(device, info, pFd);
}

VKAPI_ATTR void VKAPI_CALL
tu_DestroySemaphore(VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks *pAllocator)
{
   if (semaphore)
      q2s_forget(semaphore);
   vk_common_DestroySemaphore(device, semaphore, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL
tu_GetPhysicalDeviceExternalSemaphoreProperties(VkPhysicalDevice pd,
                                                const VkPhysicalDeviceExternalSemaphoreInfo *info,
                                                VkExternalSemaphoreProperties *props)
{
   const VkSemaphoreTypeCreateInfo *ti = (const VkSemaphoreTypeCreateInfo *)
      vk_find_struct_const(info->pNext, SEMAPHORE_TYPE_CREATE_INFO);
   if (info->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT &&
       ti && ti->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) {
      props->exportFromImportedHandleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
      props->compatibleHandleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
      props->externalSemaphoreFeatures = VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
                                         VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
      return;
   }
   vk_common_GetPhysicalDeviceExternalSemaphoreProperties(pd, info, props);
}
'''

p = os.path.join(V, "tu_q2sem.cc")
open(p, "w").write(SRC)
print("turnip-q2-ssem: wrote", p)

m = os.path.join(V, "meson.build")
s = open(m).read()
a = "  'tu_queue.cc',\n"
if s.count(a) != 1:
    sys.exit("turnip-q2-ssem: meson.build anchor not found")
s = s.replace(a, a + "  'tu_q2sem.cc',\n")
open(m, "w").write(s)
print("turnip-q2-ssem: added tu_q2sem.cc to", m)
