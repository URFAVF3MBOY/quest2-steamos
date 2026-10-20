// VK_LAYER_Q2_vrdisplay - a direct-mode display for SteamVR's compositor on the Quest 2 (SteamOS on hollywood).
//
// vrcompositor always presents through VK_KHR_display ("direct mode"). Turnip on KGSL advertises VK_KHR_display but
// refuses to enumerate its GPU when an instance enables it ("I can't KHR_display" -> vkEnumeratePhysicalDevices fails,
// vrcompositor then crashes on a NULL physical device). This layer:
//   - removes the display extensions before the instance reaches Turnip, and implements them itself: one display
//     "Valve Corporation Quest 2", one 1920x3664 mode at 72 Hz (the panel as scanned out: portrait, eyes stacked),
//     one plane, display-plane surfaces and swapchains on them;
//   - swapchain images are ordinary device images; vkQueuePresentKHR copies the image into a host buffer on the GPU,
//     then a worker thread copies it into /dev/shm/q2warp (the panel-layout frame q2blit -G already scans out);
//   - VK_EXT_display_control: vblank fences/counters from a 72 Hz timer thread;
//   - writes /dev/shm/q2vr.active (pid) while it presents, so the desktop's GPU warp leaves the panel alone.
// Queue submissions of a device are serialized with one mutex (acquire/vblank signal from other threads).
// Written for this port; freestanding (no libc headers) so it cross-builds without an aarch64 sysroot.
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

// ---- libc / pthread (declared by hand; opaque storage big enough for aarch64 and x86-64) ----
typedef struct { _Alignas(16) unsigned char b[64]; } qmutex;
typedef struct { _Alignas(16) unsigned char b[64]; } qcond;
typedef unsigned long qthread;
struct qtimespec { long tv_sec; long tv_nsec; };
extern void *malloc(size_t); extern void *calloc(size_t, size_t); extern void free(void *);
extern void *memcpy(void *, const void *, size_t); extern void *memset(void *, int, size_t);
extern int strcmp(const char *, const char *); extern size_t strlen(const char *); extern char *getenv(const char *);
extern int vsnprintf(char *, size_t, const char *, va_list); extern int snprintf(char *, size_t, const char *, ...);
extern long write(int, const void *, size_t); extern int open(const char *, int, ...); extern int close(int);
extern void *mmap(void *, size_t, int, int, int, long); extern int munmap(void *, size_t); extern int getpid(void);
extern int unlink(const char *); extern int clock_gettime(int, struct qtimespec *);
extern int memfd_create(const char *, unsigned int); extern int ftruncate(int, long); extern int dup(int);
extern long readlink(const char *, char *, size_t); extern char *strstr(const char *, const char *);
extern void *opendir(const char *); extern void *readdir(void *); extern int closedir(void *);
extern int nanosleep(const struct qtimespec *, struct qtimespec *); extern long syscall(long, ...); extern long read(int, void *, size_t);
extern int socket(int, int, int); extern int connect(int, const void *, unsigned); extern long recvmsg(int, void *, int);
extern long send(int, const void *, size_t, int);
struct q_iov { void *base; size_t len; };
struct q_msghdr { void *name; unsigned namelen; struct q_iov *iov; size_t iovlen; void *control; size_t controllen; int flags; };
extern int pthread_create(qthread *, const void *, void *(*)(void *), void *);
extern int pthread_mutex_init(qmutex *, const void *); extern int pthread_mutex_lock(qmutex *); extern int pthread_mutex_unlock(qmutex *);
extern int pthread_cond_init(qcond *, const void *); extern int pthread_cond_wait(qcond *, qmutex *);
extern int pthread_cond_timedwait(qcond *, qmutex *, const struct qtimespec *); extern int pthread_cond_broadcast(qcond *);
#define Q_O_RDWR 2
#define Q_O_CREAT 0100
#define Q_O_TRUNC 01000
#define Q_O_WRONLY 1
#define Q_O_CLOEXEC 02000000
#define Q_CLOCK_MONOTONIC 1
#define Q_CLOCK_REALTIME 0

#define EXPORT __attribute__((visibility("default")))
#define LAYER_NAME "VK_LAYER_Q2_vrdisplay"

static void qlog(const char *fmt, ...)
{
	char b[600]; va_list ap; va_start(ap, fmt);
	int n = vsnprintf(b + 13, sizeof b - 14, fmt, ap); va_end(ap);
	memcpy(b, "q2vrdisplay: ", 13); n += 13; if (n > (int)sizeof b - 2) n = sizeof b - 2;
	b[n++] = '\n'; write(2, b, n);
	// desk-294: Q2VR_LOGDIR=1 (Steam) also keeps its own log, /tmp/q2vrdisplay-<pid>.log (up to 2 MB) - Steam's web UI
	// processes do not pass stderr on, so the layer's messages from them were never seen
	static int lf = -2; static long lw;
	if (lf == -2) { lf = -1; { const char *ev = getenv("Q2VR_LOGDIR"); if (ev && *ev == '1') { char p[64]; snprintf(p, sizeof p, "/tmp/q2vrdisplay-%d.log", getpid()); lf = open(p, 01 | 0100 | 02000 | 02000000, 0644); } } }
	if (lf >= 0 && lw < 2000000) { write(lf, b, n); lw += n; }
}
static uint64_t now_ns(void) { struct qtimespec t; clock_gettime(Q_CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
// desk-190: one lock per real VkQueue (Vulkan's external-sync rule), not one per device.  Before, every submit on any
// queue of the device took the same lock and vkQueueWaitIdle held it for the whole wait, so a wait on one queue blocked
// submits to every other queue - with cross-process timeline semaphores (Steam's UI overlays) that can deadlock the
// compositor.  Owner tid / since / what are kept for the stall report.
struct qlk_e { VkQueue q; qmutex m; volatile long owner; volatile uint64_t since; const char *volatile what; };
static struct qlk_e g_qlk[32]; static int g_nqlk; static qmutex g_qlk_tab; static volatile int g_qlk_init;
static long qtid(void) { return syscall(178 /*gettid*/); }
static struct qlk_e *qlk_of(VkQueue q)
{
	if (!g_qlk_init) { pthread_mutex_init(&g_qlk_tab, 0); g_qlk_init = 1; }
	for (int i = 0; i < g_nqlk; i++) if (g_qlk[i].q == q) return &g_qlk[i];
	pthread_mutex_lock(&g_qlk_tab);
	struct qlk_e *e = 0;
	for (int i = 0; i < g_nqlk; i++) if (g_qlk[i].q == q) e = &g_qlk[i];
	if (!e && g_nqlk < 32) { e = &g_qlk[g_nqlk]; e->q = q; pthread_mutex_init(&e->m, 0); __atomic_store_n(&g_nqlk, g_nqlk + 1, __ATOMIC_RELEASE); }
	if (!e) e = &g_qlk[31];   // more than 32 queues: share the last lock (still correct, just coarser)
	pthread_mutex_unlock(&g_qlk_tab);
	return e;
}
static struct qlk_e *qlk(VkQueue q, const char *what)
{ struct qlk_e *e = qlk_of(q); pthread_mutex_lock(&e->m); e->owner = qtid(); e->since = now_ns(); e->what = what; return e; }
static void qulk(struct qlk_e *e) { e->owner = 0; e->what = 0; pthread_mutex_unlock(&e->m); }
static void sleep_ns(uint64_t ns) { struct qtimespec t = { (long)(ns / 1000000000ull), (long)(ns % 1000000000ull) }; nanosleep(&t, 0); }
static int envi(const char *n, int d) { const char *v = getenv(n); if (!v || !*v) return d; int r = 0; while (*v >= '0' && *v <= '9') r = r * 10 + (*v++ - '0'); return r; }

// ---- display description ----
static uint32_t DW = 1920, DH = 3664, DHZ = 72;
#define Q_DISPLAY ((VkDisplayKHR)(uintptr_t)0x51d2d15f0001ull)
#define Q_MODE ((VkDisplayModeKHR)(uintptr_t)0x51d2d15f0002ull)
static const char *Q_NAME = "Valve Corporation Quest 2";

// ---- object tracking ----
#define MAXI 8
struct inst {
	void *key; VkInstance inst; PFN_vkGetInstanceProcAddr gipa;
	PFN_vkDestroyInstance DestroyInstance;
	PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
	PFN_vkGetPhysicalDeviceSurfaceSupportKHR SurfaceSupport;
	PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR SurfaceCaps;
	PFN_vkGetPhysicalDeviceSurfaceFormatsKHR SurfaceFormats;
	PFN_vkGetPhysicalDeviceSurfacePresentModesKHR SurfaceModes;
	PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR SurfaceCaps2;
	PFN_vkGetPhysicalDeviceSurfaceFormats2KHR SurfaceFormats2;
	PFN_vkDestroySurfaceKHR DestroySurface;
	PFN_vkCreateDevice CreateDevice;
	PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties QFP;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties2 QFP2;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties2KHR QFP2KHR;
	PFN_vkGetPhysicalDeviceExternalSemaphoreProperties ExtSemProps;
	PFN_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR ExtSemPropsKHR;
	int fam_known; uint32_t realNF, baseF; int addFake;   // fake compute-only family = index realNF, backed by baseF
};
static struct inst insts[MAXI];
static qmutex g_lock;
static int g_inited;
static void ginit(void) { if (!g_inited) { pthread_mutex_init(&g_lock, 0); g_inited = 1; } }
static void *dkey(const void *h) { return *(void **)h; }
static struct inst *inst_of(const void *dispatchable)
{
	void *k = dkey(dispatchable);
	for (int i = 0; i < MAXI; i++) if (insts[i].key == k) return &insts[i];
	for (int i = MAXI - 1; i >= 0; i--) if (insts[i].key) return &insts[i];   // physical devices share the instance's key; fallback: latest
	return 0;
}

struct swapchain;
struct dev {
	void *key; VkDevice dev; VkPhysicalDevice pd; struct inst *in; PFN_vkGetDeviceProcAddr gdpa;
	PFN_vkSetDeviceLoaderData setLoaderData;
	qmutex qlock; VkQueue queue;
	VkPhysicalDeviceMemoryProperties memprops;
#define F(n) PFN_vk##n n;
	F(DestroyDevice) F(GetDeviceQueue) F(GetDeviceQueue2) F(QueueSubmit) F(QueueSubmit2) F(QueueSubmit2KHR) F(QueueWaitIdle)
	F(QueueBindSparse) F(QueuePresentKHR) F(CreateSwapchainKHR) F(DestroySwapchainKHR) F(GetSwapchainImagesKHR)
	F(AcquireNextImageKHR) F(AcquireNextImage2KHR) F(CreateImage) F(DestroyImage) F(GetImageMemoryRequirements)
	F(AllocateMemory) F(FreeMemory) F(BindImageMemory) F(CreateBuffer) F(DestroyBuffer) F(GetBufferMemoryRequirements)
	F(BindBufferMemory) F(MapMemory) F(UnmapMemory) F(InvalidateMappedMemoryRanges) F(CreateCommandPool)
	F(DestroyCommandPool) F(AllocateCommandBuffers) F(BeginCommandBuffer) F(EndCommandBuffer) F(CmdPipelineBarrier)
	F(CmdCopyImageToBuffer) F(CreateFence) F(DestroyFence) F(WaitForFences) F(ResetFences) F(GetFenceStatus) F(DeviceWaitIdle)
	F(CmdPipelineBarrier2) F(CmdPipelineBarrier2KHR) F(CmdWaitEvents) F(CmdWaitEvents2) F(CmdWaitEvents2KHR)
	F(CreateSemaphore) F(DestroySemaphore) F(GetSemaphoreFdKHR) F(ImportSemaphoreFdKHR) F(GetSemaphoreCounterValue)
	F(GetSemaphoreCounterValueKHR) F(SignalSemaphore) F(SignalSemaphoreKHR)
	F(WaitForPresentKHR) F(GetSwapchainStatusKHR) F(GetRefreshCycleDurationGOOGLE) F(GetPastPresentationTimingGOOGLE)
	F(ReleaseSwapchainImagesEXT) F(ImportFenceFdKHR) F(BindImageMemory2) F(BindImageMemory2KHR) F(GetMemoryFdKHR)
#undef F
	// display_control
	qmutex evlock; VkFence pending[32]; int npending;
	uint32_t realq[16];                    // real queue count per family (requests beyond it alias queue 0)
	int hostimp;                           // VK_EXT_external_memory_host enabled: frames are copied by the GPU straight into the shm
	PFN_vkGetMemoryHostPointerPropertiesEXT GetMemoryHostPointerPropertiesEXT;
	int dmabuf;                            // VK_EXT_external_memory_dma_buf + _fd enabled: zero-copy into the display's scanout buffers
	int cpufence;                          // desk-168: vblank / acquire fences signalled from the CPU (sync_fd -1 import), not behind GPU work
	PFN_vkGetMemoryFdPropertiesKHR GetMemoryFdPropertiesKHR;
};
static struct dev devs[MAXI];
static uint32_t mapf_in(struct inst *in, uint32_t f);
static uint32_t mapf(struct dev *d, uint32_t f) { return mapf_in(d->in, f); }
static struct dev *dev_of(const void *dispatchable)
{
	void *k = dkey(dispatchable);
	for (int i = 0; i < MAXI; i++) if (devs[i].key == k) return &devs[i];
	return 0;
}

// surfaces we created (display-plane surfaces)
#define MAXS 16
static uint64_t surfaces[MAXS];
static int is_our_surface(VkSurfaceKHR s) { for (int i = 0; i < MAXS; i++) if (surfaces[i] && surfaces[i] == (uint64_t)s) return 1; return 0; }

// ---- vblank timer ----
static volatile uint64_t g_tick;          // vblank counter
static volatile uint64_t g_tick_ns;       // time of the last vblank tick
// desk-175: real scanout times from q2kmsd's flip events (CLOCK_MONOTONIC).  vrcompositor (PostVsyncEvent in
// hmdwindowvulkanwsi.cpp) reads them through vkGetSwapchainCounterEXT with Valve's private counter 0x7FFFFFFD, which
// returns { u64 vblank sequence, u64 vblank time ns }; we used to return only the tick count (time 0) -> its vsync
// phase was garbage and frames paired up (one short interval, one ~50 ms).
static volatile uint64_t g_flip_seq, g_flip_ns;
static int g_pwflip = -1;
// desk-175: vrcompositor's VulkanVblankThread registers a FIRST_PIXEL_OUT display event, sleeps until ~0.5 ms before the
// expected vsync, then vkWaitForFences(fence, 50 ms).  A 50 ms timeout there would explain the ~50 ms frame gaps.
static volatile uint64_t g_ev_reg, g_ev_sig, g_evw_n, g_evw_to, g_evw_ready, g_evw_hist[6];
static volatile uint64_t g_evf[8]; static volatile unsigned g_evf_i;
// desk-177: CONFIRMED by desk-176 - every compositor wait on a display-event fence timed out (139/139 per 10 s, all 48+ ms)
// although the tick had "signalled" it.  The CPU signal is a temporary SYNC_FD import; a vkWaitForFences that is already
// blocked keeps waiting on the fence's OLD payload and never sees it.  So for these fences we answer the wait ourselves:
// the tick marks them signalled here and wakes the waiters (the import still happens, for later vkGetFenceStatus calls).
static struct { volatile uint64_t f; volatile int sig; } g_evt[16];
static volatile unsigned g_evt_i;
static qmutex g_evm; static qcond g_evc;   // zero-initialised = valid glibc defaults
static int ev_slot(uint64_t f) { for (int j = 0; j < 16; j++) if (g_evt[j].f && g_evt[j].f == f) return j; return -1; }                  // present-wait completes at the real flip (1) or at GPU-copy done (0)
static volatile uint64_t g_pi_hist[6], g_pi_last, g_phase_sum, g_phase_n;   // desk-171: present intervals (in panel refreshes) and phase after the tick
static uint64_t g_t0;
static qthread g_vbthr;
static int g_vbstarted;
static uint8_t *shm_map(void);
// The panel's real refresh (q2blit -V publishes it in the frame header: [8..9] last refresh start ns, [10] period ns).
// Our vblank ticks land on real refresh starts, every Nth one for DHZ = panel rate / N (90 Hz panel: 45 Hz -> N = 2),
// so each frame stays on the panel for exactly N refreshes.  Without that info: a free-running DHZ timer.
static int cpu_signal(struct dev *d, VkFence f);
static void *vblank_thread(void *arg)
{
	(void)arg;
	uint64_t period = 1000000000ull / DHZ;
	g_t0 = now_ns();
	uint64_t last = 0; int src = -1;
	// desk-176: ONE clock.  The ticks (display-event fences, VBLANK counter, the 0x7FFFFFFD timestamps) sit on the real
	// panel refresh grid.  Best reference: the last real page-flip time from q2kmsd (KMS event, CLOCK_MONOTONIC); then
	// q2blit's scan timing in the shm header; else a free-running timer.  The source is logged whenever it changes
	// (desk-175 and before only logged the first lock, so a silent fall-back to the free-running timer went unnoticed).
	for (;;) {
		uint64_t t = now_ns(), next = g_t0 + (g_tick + 1) * period, vs = 0, P = 0; int s2 = 2;
		volatile uint32_t *hd = 0; uint8_t *shm = shm_map(); if (shm) hd = (volatile uint32_t *)shm;
		uint64_t sP = hd ? hd[10] : 0; if (!(sP > 5000000 && sP < 20000000)) sP = 0;
		uint64_t fn = g_flip_ns;
		if (fn && t >= fn && t - fn < 1000000000ull && envi("Q2VR_FLIPCLOCK", 1)) { vs = fn; P = sP ? sP : 11111111ull; s2 = 0; }
		else if (hd && sP) { uint64_t v2 = (uint64_t)hd[8] | (uint64_t)hd[9] << 32; if (v2 && t >= v2 && t - v2 < 200000000ull) { vs = v2; P = sP; s2 = 1; } }
		if (vs) {
			uint64_t mult = (period + P / 2) / P; if (mult < 1) mult = 1;
			uint64_t k = (t + 500000 - vs) / P + 1, n = vs + k * P;          // first refresh start > t + 0.5 ms
			while (last && n < last + mult * P - P / 2) n += P;             // keep N refreshes between ticks
			next = n;
			if (s2 != src) qlog("vblank ticks now follow %s: refresh %.3f ms, tick every %llu refresh(es) = %.2f Hz",
			                    s2 == 0 ? "the real KMS flip times" : "q2blit's scan timing", (double)P / 1e6, (unsigned long long)mult, 1e9 / (double)(P * mult));
		} else if (s2 != src) qlog("vblank ticks: free-running %u Hz timer (no flip times, no scan timing)", DHZ);
		src = s2;
		if (next > t) sleep_ns(next - t);
		last = next;
		g_tick_ns = vs ? next : now_ns();   // the grid time of this vblank (not our wake-up time)
		__atomic_add_fetch(&g_tick, 1, __ATOMIC_RELEASE);
		for (int i = 0; i < MAXI; i++) {
			struct dev *d = &devs[i];
			if (!d->key || !d->queue) continue;
			// desk-192: the fences are signalled while d->evlock is held, and BEFORE the waiters are woken.  Until now the
			// waiters were woken first; vrcompositor's VulkanVblankThread then destroyed the fence while this thread was
			// still importing a signal into it -> use-after-free, SIGSEGV in a thread named "VulkanVblankThr" (this one
			// inherits that name).  It crashed the compositor about once per session since desk-177.  vkDestroyFence is
			// hooked too and takes d->evlock, so a fence can no longer vanish while it is being signalled.
			pthread_mutex_lock(&d->evlock);
			int n = d->npending;
			VkFence f[32]; memcpy(f, d->pending, sizeof(VkFence) * n); d->npending = 0;
			for (int k = 0; k < n; k++) {
				g_ev_sig++;
				if (!cpu_signal(d, f[k])) continue;
				struct qlk_e *L = qlk(d->queue, "vblank fence");
				d->QueueSubmit(d->queue, 0, 0, f[k]);
				qulk(L);
			}
			if (n) {   // desk-177: wake waiters in our vkWaitForFences
				pthread_mutex_lock(&g_evm);
				for (int k = 0; k < n; k++) { int j = ev_slot((uint64_t)f[k]); if (j >= 0) g_evt[j].sig = 1; }
				pthread_cond_broadcast(&g_evc); pthread_mutex_unlock(&g_evm);
			}
			pthread_mutex_unlock(&d->evlock);
		}
	}
	return 0;
}
static void start_vblank(void) { if (!g_vbstarted) { g_vbstarted = 1; pthread_create(&g_vbthr, 0, vblank_thread, 0); } }
// desk-168: signal a fence NOW, from the CPU.  An empty vkQueueSubmit(fence) on the shared queue only signals after all
// GPU work queued before it - so "vsync" and "image acquired" reached the compositor late whenever the GPU was busy,
// and its frames lost their deadline (capped well below the panel rate).  A temporary import of the "already
// signalled" sync file (fd -1) signals at once.  Returns 0 on success; on any doubt the caller falls back to the submit.
static int cpu_signal(struct dev *d, VkFence f)
{
	if (!d->cpufence || !f) return -1;
	VkImportFenceFdInfoKHR im = { VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR, 0, f, VK_FENCE_IMPORT_TEMPORARY_BIT,
		VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT, -1 };
	VkResult r = d->ImportFenceFdKHR(d->dev, &im);
	if (r == VK_SUCCESS && d->GetFenceStatus(d->dev, f) == VK_SUCCESS) return 0;
	d->cpufence = 0;
	qlog("CPU-signalled fences do not work here (import %d) - using queue submits", (int)r);
	return -1;
}

// ---- the panel frame (shared with q2blit -G) ----
#define SHM_PATH "/dev/shm/q2warp"
#define SHM_SIZE (4096u + 3664u * 7680u)
static uint8_t *g_shm;
static int g_shm_tried;
static void vr_inactive(void) { unlink("/dev/shm/q2vr.active"); }
__attribute__((destructor)) static void q2vr_fini(void) { if (g_shm) vr_inactive(); }
static uint8_t *shm_map(void)
{
	if (g_shm || g_shm_tried) return g_shm;
	g_shm_tried = 1;
	int fd = open(SHM_PATH, Q_O_RDWR | Q_O_CLOEXEC);
	if (fd < 0) { qlog("cannot open %s - frames are dropped (is the GPU desktop warp running?)", SHM_PATH); return 0; }
	void *p = mmap(0, SHM_SIZE, 3, 1, fd, 0); close(fd);
	if (p == (void *)-1) { qlog("cannot map %s", SHM_PATH); return 0; }
	g_shm = p;
	int af = open("/dev/shm/q2vr.active", Q_O_WRONLY | Q_O_CREAT | Q_O_TRUNC | Q_O_CLOEXEC, 0666);
	if (af >= 0) { char b[32]; int n = snprintf(b, sizeof b, "%d\n", getpid()); write(af, b, n); close(af); }
	qlog("presenting into %s (panel 1920x3664)", SHM_PATH);
	return g_shm;
}

// ---- swapchains ----
#define MAXIMG 4
struct swapchain {
	uint64_t magic; struct dev *d; VkExtent2D ext; VkFormat fmt; uint32_t n;
	VkImage img[MAXIMG]; VkDeviceMemory mem[MAXIMG];
	VkBuffer buf[MAXIMG]; VkDeviceMemory bmem[MAXIMG]; void *map[MAXIMG]; int coherent[MAXIMG];
	VkCommandPool pool; VkCommandBuffer cmd[MAXIMG]; VkFence fence[MAXIMG];
	int state[MAXIMG];                     // 0 free, 1 acquired by the app, 2 copy in flight
	uint32_t next;
	qmutex lock; qcond cond;
	int q[MAXIMG * 2]; int qh, qt;         // presented indices for the worker
	qthread worker; int dead;
	uint64_t frames, t_last_log;
	uint64_t qid[MAXIMG * 2];              // present id of each queued frame (VK_KHR_present_id)
	volatile uint64_t done_id;             // highest present id whose GPU copy finished
	volatile uint64_t want_id, wait_timeouts, acquires, acquire_waits, presents, copy_ns;
	qthread status;
	VkBuffer hbuf; VkDeviceMemory hmem; void *hmap; int himp, hcoh;   // GPU-direct: buffer over /dev/shm/q2warp's frame
	// desk-160 zero-copy: the display's own scanout buffers (dma-bufs from q2kmsd), page-flipped by q2kmsd
	int ksock, kn; VkBuffer kbuf[4]; VkDeviceMemory kmem[4]; VkCommandBuffer kcmd[MAXIMG][4];
	volatile int kstate[4];                // 0 free, 1 GPU writing, 2 sent to q2kmsd, 3 on the panel, 4 was on the panel (desk-303)
	int qk[MAXIMG * 2];                    // scanout buffer of each queued frame (-1: old copy path)
	qthread kreader; volatile uint64_t kflips, kskips, knofree, ksent, klat_ns; uint64_t ksent_t[4], kpid[4];
};
#define SC_MAGIC 0x51325643484149ull
static struct swapchain *sc_of(VkSwapchainKHR s) { struct swapchain *c = (struct swapchain *)(uintptr_t)s; return (c && c->magic == SC_MAGIC) ? c : 0; }

static uint32_t find_mem(struct dev *d, uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags nice)
{
	int best = -1;
	for (uint32_t i = 0; i < d->memprops.memoryTypeCount; i++) {
		if (!(bits & (1u << i))) continue;
		VkMemoryPropertyFlags f = d->memprops.memoryTypes[i].propertyFlags;
		if ((f & want) != want) continue;
		if ((f & nice) == nice) return i;
		if (best < 0) best = (int)i;
	}
	return best < 0 ? 0xffffffffu : (uint32_t)best;
}

static void *worker_thread(void *arg)
{
	struct swapchain *c = arg; struct dev *d = c->d;
	for (;;) {
		pthread_mutex_lock(&c->lock);
		while (c->qh == c->qt && !c->dead) pthread_cond_wait(&c->cond, &c->lock);
		if (c->qh == c->qt && c->dead) { pthread_mutex_unlock(&c->lock); break; }
		int i = c->q[c->qh % (MAXIMG * 2)]; uint64_t pid = c->qid[c->qh % (MAXIMG * 2)]; int kk = c->qk[c->qh % (MAXIMG * 2)]; c->qh++;
		pthread_mutex_unlock(&c->lock);
		VkResult fr = d->WaitForFences(d->dev, 1, &c->fence[i], VK_TRUE, 2000000000ull);
		if (fr) {   // desk-177 (review): never hand an unfinished copy to the display - wait once more, else drop the frame
			qlog("frame copy fence wait failed (%d)%s", (int)fr, kk >= 0 ? " - waiting again before the flip" : "");
			if (kk >= 0 && d->WaitForFences(d->dev, 1, &c->fence[i], VK_TRUE, 2000000000ull) != VK_SUCCESS) {
				qlog("frame copy still not finished - frame dropped (scanout buffer %d not flipped)", kk);
				pthread_mutex_lock(&c->lock); c->kstate[kk] = 0; if (pid > c->done_id) c->done_id = pid; pthread_cond_broadcast(&c->cond); pthread_mutex_unlock(&c->lock);
				kk = -2;   // skip the flip and the CPU paths below
			}
		}
		d->ResetFences(d->dev, 1, &c->fence[i]);
		if (g_pwflip < 0) g_pwflip = envi("Q2VR_PWFLIP", 0);   // desk-177: default back to GPU-copy-done (flip-time completion brought 15 ms present-wait timeouts)
		pthread_mutex_lock(&c->lock); if (!(kk >= 0 && g_pwflip) && pid > c->done_id) c->done_id = pid; pthread_cond_broadcast(&c->cond); pthread_mutex_unlock(&c->lock);
		uint64_t tc0 = now_ns();
		uint8_t *shm = kk != -1 ? 0 : shm_map();
		if (kk == -2) { /* dropped (see above) */ }
		else if (kk >= 0) {   // zero-copy: the frame is in scanout buffer kk; ask q2kmsd to show it
			char m[16]; int ml = snprintf(m, sizeof m, "F %d\n", kk);
			pthread_mutex_lock(&c->lock); c->kstate[kk] = 2; c->ksent_t[kk] = now_ns(); c->kpid[kk] = pid; c->ksent++; pthread_mutex_unlock(&c->lock);
			if (c->ksock < 0 || send(c->ksock, m, (size_t)ml, 0x4000 /*MSG_NOSIGNAL*/) != ml) {
				pthread_mutex_lock(&c->lock); c->kstate[kk] = 0; if (pid > c->done_id) c->done_id = pid; pthread_cond_broadcast(&c->cond); pthread_mutex_unlock(&c->lock);
			}
		} else if (c->himp && shm) {   // the GPU copy already wrote the frame into the shm: make it visible to the CPU, tell q2blit
			if (!c->hcoh && c->hmap) {
				VkMappedMemoryRange r = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, 0, c->hmem, 0, VK_WHOLE_SIZE };
				d->InvalidateMappedMemoryRanges(d->dev, 1, &r);
			}
			uint32_t h = c->ext.height < 3664 ? c->ext.height : 3664;
			volatile uint32_t *hd = (volatile uint32_t *)shm;
			hd[2] = 0; hd[3] = h < 1832 ? h : 1832; hd[6] = 1832; hd[7] = h > 1832 ? h : 1832;
			__atomic_add_fetch(&hd[1], 1, __ATOMIC_RELEASE);
			syscall(98 /*futex*/, &hd[1], 1 /*FUTEX_WAKE*/, 0x7fffffff, 0, 0, 0);
		} else {
		if (!c->coherent[i]) {
			VkMappedMemoryRange r = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, 0, c->bmem[i], 0, VK_WHOLE_SIZE };
			d->InvalidateMappedMemoryRanges(d->dev, 1, &r);
		}
		if (shm && c->map[i]) {
			uint32_t w = c->ext.width < 1920 ? c->ext.width : 1920, h = c->ext.height < 3664 ? c->ext.height : 3664;
			const uint8_t *src = c->map[i]; uint8_t *dst = shm + 4096;
			if (w == 1920) memcpy(dst, src, (size_t)h * 7680);
			else for (uint32_t y = 0; y < h; y++) memcpy(dst + (size_t)y * 7680, src + (size_t)y * c->ext.width * 4, (size_t)w * 4);
			volatile uint32_t *hd = (volatile uint32_t *)shm;
			hd[2] = 0; hd[3] = h < 1832 ? h : 1832; hd[6] = 1832; hd[7] = h > 1832 ? h : 1832;
			__atomic_add_fetch(&hd[1], 1, __ATOMIC_RELEASE);
			syscall(98 /*futex*/, &hd[1], 1 /*FUTEX_WAKE*/, 0x7fffffff, 0, 0, 0);   // q2blit sleeps on it
		}
		}
		c->frames++;
		uint64_t t = now_ns();
		c->copy_ns += t - tc0;
		if (t - c->t_last_log > 10000000000ull) { qlog("%llu frames presented", (unsigned long long)c->frames); c->t_last_log = t; }
		pthread_mutex_lock(&c->lock);
		c->state[i] = 0;
		if (!(kk >= 0 && g_pwflip) && pid > c->done_id) c->done_id = pid;
		pthread_cond_broadcast(&c->cond);
		pthread_mutex_unlock(&c->lock);
	}
	return 0;
}

static void report_sem_waits(const char *tag);
// desk-190: when the compositor stops presenting, say where it is stuck: who holds a queue lock (and for how long),
// which shared semaphore waits are unmet, and what every thread is blocked in (/proc/self/task/*: state + wchan).
static void stall_report(struct swapchain *c)
{
	static int nrep; if (++nrep > 6) return;
	uint64_t t = now_ns();
	qlog("STALL: no presents for 10 s (present id done %llu, waited for %llu)", (unsigned long long)c->done_id, (unsigned long long)c->want_id);
	for (int i = 0; i < g_nqlk; i++) if (g_qlk[i].owner)
		qlog("STALL: queue %p lock held by thread %ld for %.1f s (%s)", (void *)g_qlk[i].q, g_qlk[i].owner, (double)(t - g_qlk[i].since) / 1e9, g_qlk[i].what ? g_qlk[i].what : "?");
	report_sem_waits("STALL");
	void *dir = opendir("/proc/self/task"); if (!dir) return;
	char line[560]; int o = 0; void *de;
	while ((de = readdir(dir))) {
		const char *nm = (const char *)de + 19; if (nm[0] < '0' || nm[0] > '9') continue;
		char pth[80], comm[32] = "?", wch[48] = "?", st[300]; char state = '?';
		snprintf(pth, sizeof pth, "/proc/self/task/%s/comm", nm); int fd = open(pth, 0); if (fd >= 0) { long r = read(fd, comm, sizeof comm - 1); comm[r > 0 ? r : 0] = 0; if (r > 0 && comm[r - 1] == '\n') comm[r - 1] = 0; close(fd); }
		snprintf(pth, sizeof pth, "/proc/self/task/%s/wchan", nm); fd = open(pth, 0); if (fd >= 0) { long r = read(fd, wch, sizeof wch - 1); wch[r > 0 ? r : 0] = 0; close(fd); }
		snprintf(pth, sizeof pth, "/proc/self/task/%s/stat", nm); fd = open(pth, 0); if (fd >= 0) { long r = read(fd, st, sizeof st - 1); st[r > 0 ? r : 0] = 0; close(fd); char *q = 0; for (char *x = st; *x; x++) if (*x == ')') q = x; if (q && q[1] && q[2]) state = q[2]; }
		if (state == 'R' || state == 'S' || state == 'D' || state == '?') {
			char ent[100]; int m = snprintf(ent, sizeof ent, " %s:%s:%c:%s", nm, comm, state, wch);
			if (o + m > 500) { line[o] = 0; qlog("STALL threads:%s", line); o = 0; }
			memcpy(line + o, ent, m); o += m;
		}
	}
	closedir(dir);
	if (o) { line[o] = 0; qlog("STALL threads:%s", line); }
}
static void *status_thread(void *arg)
{
	struct swapchain *c = arg;
	uint64_t f0 = 0, p0 = 0, a0 = 0, w0 = 0, cn0 = 0;
	for (int k = 0; c->magic == SC_MAGIC && k < 100000; k++) {
		sleep_ns(10000000000ull);
		if (c->magic != SC_MAGIC) break;
		uint64_t f = c->frames, pr = c->presents, ac = c->acquires, wt = c->acquire_waits, cn = c->copy_ns;
		qlog("status: %llu frames/10s on panel, %llu presents, %llu acquires (%llu waited), copy %.1f ms/frame, states %d%d%d, present id done %llu / waited for %llu (%llu timeouts), vblank %llu",
		     (unsigned long long)(f - f0), (unsigned long long)(pr - p0), (unsigned long long)(ac - a0), (unsigned long long)(wt - w0),
		     f > f0 ? (double)(cn - cn0) / 1e6 / (double)(f - f0) : 0.0, c->state[0], c->state[1], c->n > 2 ? c->state[2] : 9,
		     (unsigned long long)c->done_id, (unsigned long long)c->want_id, (unsigned long long)c->wait_timeouts, (unsigned long long)g_tick);
		{   // GPU load over the last interval (kgsl gpubusy: "busy total" counters since the previous read)
			int gf = open("/sys/class/kgsl/kgsl-3d0/gpubusy", 0); char gb[64]; long gn = gf >= 0 ? read(gf, gb, 63) : -1; if (gf >= 0) close(gf);
			if (gn > 0) { gb[gn] = 0; long b0 = 0, t0 = 0; char *q = gb; while (*q == ' ') q++; while (*q >= '0' && *q <= '9') b0 = b0 * 10 + (*q++ - '0');
				while (*q == ' ') q++; while (*q >= '0' && *q <= '9') t0 = t0 * 10 + (*q++ - '0');
				qlog("gpu busy %ld%% (gpubusy %ld/%ld)", t0 > 0 ? b0 * 100 / t0 : -1, b0, t0); }
		}
		{ static uint64_t kf0, ks0, kn0, kl0, kt0;
		  if (c->kn || c->kflips) {
			uint64_t df = c->kflips - kf0;
			qlog("zero-copy: %llu flips/10s (%.1f/s), %llu replaced, %llu presents found no free buffer, frame-ready-to-on-panel %.1f ms",
			     (unsigned long long)df, df / 10.0, (unsigned long long)(c->kskips - ks0), (unsigned long long)(c->knofree - kn0),
			     df ? (double)(c->klat_ns - kl0) / 1e6 / (double)df : 0.0);
		  }
		  kf0 = c->kflips; ks0 = c->kskips; kn0 = c->knofree; kl0 = c->klat_ns; (void)kt0; }
		{ static uint64_t r0, s0, n0, t0, h0[6];
		  qlog("vblank events (compositor's vsync thread): %llu registered, %llu signalled, %llu waits (%llu timed out); wait ms <2:%llu 2-8:%llu 8-16:%llu 16-30:%llu 30-48:%llu 48+:%llu ; flips seen %llu",
		       (unsigned long long)(g_ev_reg - r0), (unsigned long long)(g_ev_sig - s0), (unsigned long long)(g_evw_n - n0), (unsigned long long)(g_evw_to - t0),
		       (unsigned long long)(g_evw_hist[0] - h0[0]), (unsigned long long)(g_evw_hist[1] - h0[1]), (unsigned long long)(g_evw_hist[2] - h0[2]),
		       (unsigned long long)(g_evw_hist[3] - h0[3]), (unsigned long long)(g_evw_hist[4] - h0[4]), (unsigned long long)(g_evw_hist[5] - h0[5]),
		       (unsigned long long)g_flip_seq);
		  r0 = g_ev_reg; s0 = g_ev_sig; n0 = g_evw_n; t0 = g_evw_to; for (int q = 0; q < 6; q++) h0[q] = g_evw_hist[q]; }
		qlog("present intervals in panel refreshes: 0:%llu 1:%llu 2:%llu 3:%llu 4:%llu 5+:%llu ; present comes %.1f ms after the vsync tick",
		     (unsigned long long)g_pi_hist[0], (unsigned long long)g_pi_hist[1], (unsigned long long)g_pi_hist[2], (unsigned long long)g_pi_hist[3],
		     (unsigned long long)g_pi_hist[4], (unsigned long long)g_pi_hist[5], g_phase_n ? (double)g_phase_sum / 1e6 / (double)g_phase_n : 0.0);
		memset((void *)g_pi_hist, 0, sizeof g_pi_hist); g_phase_sum = g_phase_n = 0;
		if (g_shm) { volatile uint32_t *hd = (volatile uint32_t *)g_shm;
			qlog("panel copy (q2blit): %u us, %u rows/ms (panel scans 442 rows/ms), refresh %u us", hd[12], hd[13] / 1000, hd[10] / 1000); }
		if (pr == p0 && pr > 0) stall_report(c);
		f0 = f; p0 = pr; a0 = ac; w0 = wt; cn0 = cn;
	}
	return 0;
}
// GPU-direct (desk-141): a buffer whose memory IS /dev/shm/q2warp's frame area (VK_EXT_external_memory_host), so the
// present copy writes the panel frame straight into the memory q2blit scans out - no 28 MB CPU memcpy per frame.
static void hostimport_setup(struct dev *d, struct swapchain *c)
{
	uint8_t *shm = shm_map();
	if (!shm || !d->hostimp || !d->GetMemoryHostPointerPropertiesEXT) return;
	const VkDeviceSize fsz = 3664ull * 7680ull;
	VkExternalMemoryBufferCreateInfo eb = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, 0, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
	VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb, 0, fsz, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0 };
	VkResult r = d->CreateBuffer(d->dev, &bi, 0, &c->hbuf);
	if (r) { qlog("GPU-direct: buffer creation failed (%d) - using the CPU copy", (int)r); c->hbuf = 0; return; }
	VkMemoryRequirements mr; d->GetBufferMemoryRequirements(d->dev, c->hbuf, &mr);
	VkMemoryHostPointerPropertiesEXT hp = { VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT, 0, 0 };
	r = d->GetMemoryHostPointerPropertiesEXT(d->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, shm + 4096, &hp);
	uint32_t bits = hp.memoryTypeBits & mr.memoryTypeBits, mt = 0xffffffffu;
	if (!r && !bits && hp.memoryTypeBits && envi("Q2VR_HOSTIMPORT_ANYTYPE", 1)) bits = hp.memoryTypeBits;   // some drivers report disjoint sets
	for (uint32_t k = 0; k < d->memprops.memoryTypeCount; k++) if (bits & (1u << k)) { mt = k; break; }
	if (r || mt == 0xffffffffu || mr.size > fsz) {
		qlog("GPU-direct: shm not importable (r %d, type bits 0x%x/0x%x, %u types, size %llu) - using the CPU copy", (int)r, hp.memoryTypeBits,
		     mr.memoryTypeBits, d->memprops.memoryTypeCount, (unsigned long long)mr.size);
		d->DestroyBuffer(d->dev, c->hbuf, 0); c->hbuf = 0; return;
	}
	VkImportMemoryHostPointerInfoEXT im = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, 0, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, shm + 4096 };
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &im, fsz, mt };
	if ((r = d->AllocateMemory(d->dev, &ai, 0, &c->hmem)) || (r = d->BindBufferMemory(d->dev, c->hbuf, c->hmem, 0))) {
		qlog("GPU-direct: importing the shm failed (%d) - using the CPU copy", (int)r);
		if (c->hmem) d->FreeMemory(d->dev, c->hmem, 0);
		d->DestroyBuffer(d->dev, c->hbuf, 0); c->hbuf = 0; c->hmem = 0; return;
	}
	c->hcoh = (d->memprops.memoryTypes[mt].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
	if (!c->hcoh && d->MapMemory(d->dev, c->hmem, 0, VK_WHOLE_SIZE, 0, &c->hmap)) c->hmap = 0;
	c->himp = 1;
	qlog("GPU-direct: frames go straight into /dev/shm/q2warp (memory type %u, %s) - no CPU copy", mt, c->hcoh ? "coherent" : "invalidate per frame");
}

static void *kms_reader(void *arg)
{
	struct swapchain *c = arg;
	char buf[256]; int have = 0;
	for (;;) {
		long n = read(c->ksock, buf + have, sizeof buf - 1 - have);
		if (n <= 0) break;
		have += (int)n; buf[have] = 0;
		char *line = buf, *nl;
		while ((nl = strstr(line, "\n"))) {
			*nl = 0;
			int k = (line[0] == 'D' || line[0] == 'S') && line[1] == ' ' ? line[2] - '0' : -1;
			if (k >= 0 && k < c->kn) {
				pthread_mutex_lock(&c->lock);
				if (line[0] == 'D') {
					// desk-303: the buffer that WAS on the panel is not reused at once: it stays reserved (state 4) for one more
					// flip.  Freed right at the flip event, the next frame's GPU copy started into it while the display engine
					// was still reading it for the lower (right-eye) half of the panel -> right eye only: black/jagged blocks
					// of the next frame on moving content (laser, controllers, Steam panels), worse while turning the head.
					// Q2VR_KEEPPREV=0 = old behaviour.
					static int keep = -1; if (keep < 0) keep = envi("Q2VR_KEEPPREV", 1);
					for (int j = 0; j < c->kn; j++) {
						if (c->kstate[j] == 4) c->kstate[j] = 0;
						else if (c->kstate[j] == 3 && j != k) c->kstate[j] = keep ? 4 : 0;
					}
					c->kstate[k] = 3; c->kflips++;
					if (c->ksent_t[k]) c->klat_ns += now_ns() - c->ksent_t[k];
					uint64_t ft = 0; const char *q = line + 3; while (*q == ' ') q++;
					while (*q >= '0' && *q <= '9') ft = ft * 10 + (uint64_t)(*q++ - '0');
					uint64_t nw = now_ns(); if (!ft || ft > nw || nw - ft > 100000000ull) ft = nw;   // sanity: must be a recent MONOTONIC time
					g_flip_ns = ft; __atomic_add_fetch(&g_flip_seq, 1, __ATOMIC_RELEASE);
				} else { c->kstate[k] = 0; c->kskips++; }
				if (c->kpid[k] > c->done_id) c->done_id = c->kpid[k];   // present-wait: done when it reached (or was replaced on) the panel
				pthread_cond_broadcast(&c->cond);
				pthread_mutex_unlock(&c->lock);
			}
			line = nl + 1;
		}
		have = (int)(buf + have - line); memcpy(buf, line, have);
		if (have > 200) have = 0;
	}
	qlog("zero-copy: q2kmsd connection closed");
	pthread_mutex_lock(&c->lock); c->kn = 0; pthread_cond_broadcast(&c->cond); pthread_mutex_unlock(&c->lock);   // back to the copy path
	return 0;
}

// desk-160: zero-copy.  q2kmsd (the process that owns the display) hands us its VR scanout buffers as dma-bufs; we
// import them, and each present copies the compositor's image into a free one ON THE GPU, then asks q2kmsd to flip the
// display to it.  No CPU copy and no q2blit in the VR path.  Any failure -> the old copy path, nothing else changes.
static void kms_setup(struct dev *d, struct swapchain *c)
{
	c->ksock = -1; c->kn = 0;
	if (!envi("Q2VR_ZEROCOPY", 1)) return;
	if (!d->dmabuf || !d->GetMemoryFdPropertiesKHR) { qlog("zero-copy: no dma-buf import in the GPU driver - copy path"); return; }
	const uint32_t SW = c->ext.width, SH = c->ext.height;   // desk-161: may be smaller than the panel (the display engine scales up)
	if (SW < 64 || SH < 64 || SW > 1920 || SH > 3664 || (SW & 3) || (SH & 1)) { qlog("zero-copy: swapchain %ux%u unusable - copy path", SW, SH); return; }
	int s = socket(1 /*AF_UNIX*/, 1 /*SOCK_STREAM*/ | 02000000 /*CLOEXEC*/, 0);
	if (s < 0) return;
	struct { unsigned short fam; char path[108]; } sa; memset(&sa, 0, sizeof sa); sa.fam = 1;
	memcpy(sa.path, "/run/q2kms-vr.sock", 19);
	if (connect(s, &sa, sizeof sa) < 0) { close(s); qlog("zero-copy: q2kmsd not running (display not on KMS) - copy path"); return; }
	{ char rq[40]; int rl = snprintf(rq, sizeof rq, "SIZE %u %u\n", SW, SH); send(s, rq, (size_t)rl, 0x4000); }
	char txt[128]; uint64_t cb[16];
	struct q_iov io = { txt, sizeof txt - 1 };
	struct q_msghdr m = { 0, 0, &io, 1, cb, sizeof cb, 0 };
	long n = recvmsg(s, &m, 0x40000000 /*MSG_CMSG_CLOEXEC*/);
	if (n <= 0) { close(s); qlog("zero-copy: no answer from q2kmsd - copy path"); return; }
	txt[n] = 0;
	int fds[4] = { -1, -1, -1, -1 }, nf = 0;
	if (m.controllen >= 20) {
		uint8_t *cm = (uint8_t *)cb; size_t clen = *(size_t *)cm; int lvl = *(int *)(cm + 8), typ = *(int *)(cm + 12);
		if (lvl == 1 && typ == 1) { nf = (int)((clen - 16) / 4); if (nf > 4) nf = 4; for (int k = 0; k < nf; k++) fds[k] = *(int *)(cm + 16 + 4 * k); }
	}
	int nb = 0, W = 0, H = 0, pitch = 0; long long sz = 0;
	{ const char *q = txt; if (strstr(q, "VRBUF ") == q) { q += 6; long v[5] = { 0 }; for (int f = 0; f < 5; f++) { while (*q == ' ') q++; while (*q >= '0' && *q <= '9') v[f] = v[f] * 10 + (*q++ - '0'); }
		nb = (int)v[0]; W = (int)v[1]; H = (int)v[2]; pitch = (int)v[3]; sz = v[4]; } }
	if (nb < 2 || nb > 4 || nf != nb || W != (int)SW || H != (int)SH || pitch < (int)SW * 4 || (pitch & 3) || sz < (long long)pitch * SH) {
		qlog("zero-copy: unexpected offer from q2kmsd (%s, %d fds) - copy path", txt, nf);
		for (int k = 0; k < nf; k++) close(fds[k]); close(s); return;
	}
	VkResult r = 0; int ok = 0;
	for (int k = 0; k < nb; k++) {
		VkExternalMemoryBufferCreateInfo eb = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, 0, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
		VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb, 0, (VkDeviceSize)pitch * SH, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0 };
		if ((r = d->CreateBuffer(d->dev, &bi, 0, &c->kbuf[k]))) break;
		VkMemoryRequirements mr; d->GetBufferMemoryRequirements(d->dev, c->kbuf[k], &mr);
		VkMemoryFdPropertiesKHR fp = { VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR, 0, 0 };
		if ((r = d->GetMemoryFdPropertiesKHR(d->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fds[k], &fp))) break;
		uint32_t bits = fp.memoryTypeBits & mr.memoryTypeBits, mt = 0xffffffffu;
		if (!bits) {   // desk-177 (review): disjoint type masks are outside the spec - say so; Q2VR_DMABUF_STRICT=1 refuses them
			qlog("zero-copy: buffer %d: dma-buf type bits 0x%x and buffer type bits 0x%x share no type%s", k, fp.memoryTypeBits, mr.memoryTypeBits,
			     envi("Q2VR_DMABUF_STRICT", 0) ? " - refused" : " - importing with the dma-buf's types anyway");
			bits = envi("Q2VR_DMABUF_STRICT", 0) ? 0 : (fp.memoryTypeBits ? fp.memoryTypeBits : mr.memoryTypeBits);
		}
		for (uint32_t t = 0; t < d->memprops.memoryTypeCount; t++) if (bits & (1u << t)) { mt = t; break; }
		if (mt == 0xffffffffu || mr.size > (VkDeviceSize)sz) { r = VK_ERROR_FORMAT_NOT_SUPPORTED; qlog("zero-copy: buffer %d: memory type bits 0x%x/0x%x size %llu", k, fp.memoryTypeBits, mr.memoryTypeBits, (unsigned long long)mr.size); break; }
		VkMemoryDedicatedAllocateInfo ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, 0, VK_NULL_HANDLE, c->kbuf[k] };
		VkImportMemoryFdInfoKHR im = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &ded, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fds[k] };
		VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &im, mr.size, mt };
		if ((r = d->AllocateMemory(d->dev, &ai, 0, &c->kmem[k]))) break;
		fds[k] = -1;   // the driver owns the fd now
		if ((r = d->BindBufferMemory(d->dev, c->kbuf[k], c->kmem[k], 0))) break;
		ok++;
	}
	for (int k = 0; k < nb; k++) if (fds[k] >= 0) close(fds[k]);
	if (ok != nb) {
		qlog("zero-copy: importing the scanout buffers failed (%d, %d of %d) - copy path", (int)r, ok, nb);
		for (int k = 0; k < nb; k++) { if (c->kbuf[k]) d->DestroyBuffer(d->dev, c->kbuf[k], 0); if (c->kmem[k]) d->FreeMemory(d->dev, c->kmem[k], 0); c->kbuf[k] = 0; c->kmem[k] = 0; }
		close(s); return;
	}
	int eshift = 0;
	{ int ef = open("/var/lib/q2/eyeshift", 0); if (ef >= 0) { char eb2[16]; long en = read(ef, eb2, 15); close(ef);
		if (en > 0) { eb2[en] = 0; int sg = 1, v = 0; char *q = eb2; while (*q == ' ') q++; if (*q == '-') { sg = -1; q++; } else if (*q == '+') q++;
			while (*q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0'); eshift = sg * v; } } }
	if (eshift) qlog("zero-copy: eye shift %d px (from /var/lib/q2/eyeshift; changes apply when VR restarts)", eshift);
	VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, c->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
	for (uint32_t i = 0; i < c->n; i++) for (int k = 0; k < nb; k++) {
		if ((r = d->AllocateCommandBuffers(d->dev, &cai, &c->kcmd[i][k]))) { qlog("zero-copy: command buffers (%d) - copy path", (int)r); close(s); return; }
		if (d->setLoaderData) d->setLoaderData(d->dev, c->kcmd[i][k]);
		VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, 0, 0 };
		d->BeginCommandBuffer(c->kcmd[i][k], &cbi);
		VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VkImageMemoryBarrier b1 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
			VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, c->img[i], rng };
		d->CmdPipelineBarrier(c->kcmd[i][k], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &b1);
		// eye alignment (/var/lib/q2/eyeshift, as q2blit applies it): upper eye half dst row r <- src r+e, lower half r <- r-e
		VkBufferImageCopy reg[2];
		const int EH2 = (int)SH / 2; const uint32_t RL = (uint32_t)pitch / 4;
		int e = (int)((long)eshift * (long)SH / 3664); int lim = EH2 / 4; if (e > lim) e = lim; if (e < -lim) e = -lim;
		{ int s0 = e > 0 ? e : 0, d0 = e > 0 ? 0 : -e, L = EH2 - (e > 0 ? e : -e);
		  VkBufferImageCopy r0 = { (VkDeviceSize)d0 * pitch, RL, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, s0, 0 }, { SW, (uint32_t)L, 1 } }; reg[0] = r0; }
		{ int s0 = EH2 + (e < 0 ? -e : 0), d0 = EH2 + (e > 0 ? e : 0), L = EH2 - (e > 0 ? e : -e);
		  VkBufferImageCopy r1 = { (VkDeviceSize)d0 * pitch, RL, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, s0, 0 }, { SW, (uint32_t)L, 1 } }; reg[1] = r1; }
		d->CmdCopyImageToBuffer(c->kcmd[i][k], c->img[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->kbuf[k], 2, reg);
		VkImageMemoryBarrier b2 = b1; b2.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b2.dstAccessMask = 0;
		b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		VkBufferMemoryBarrier b3 = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
			VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, c->kbuf[k], 0, VK_WHOLE_SIZE };
		d->CmdPipelineBarrier(c->kcmd[i][k], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, 0, 1, &b3, 1, &b2);
		if ((r = d->EndCommandBuffer(c->kcmd[i][k]))) { qlog("zero-copy: recording (%d) - copy path", (int)r); close(s); return; }
	}
	c->ksock = s;
	for (int k = 0; k < nb; k++) c->kstate[k] = 0;
	c->kn = nb;
	pthread_create(&c->kreader, 0, kms_reader, c);
	qlog("zero-copy: ON - %ux%u frames are copied on the GPU into %d display scanout buffers and page-flipped by q2kmsd (no CPU copy, no q2blit)%s",
	     SW, SH, nb, (SW != 1920 || SH != 3664) ? "; the display engine scales them up to the 1920x3664 panel" : "");
}

static VkResult create_swapchain(struct dev *d, const VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *out)
{
	struct swapchain *c = calloc(1, sizeof *c);
	if (!c) return VK_ERROR_OUT_OF_HOST_MEMORY;
	c->magic = SC_MAGIC; c->d = d; c->ext = ci->imageExtent; c->fmt = ci->imageFormat;
	c->n = ci->minImageCount < 2 ? 2 : ci->minImageCount; if (c->n > 3) c->n = 3;
	pthread_mutex_init(&c->lock, 0); pthread_cond_init(&c->cond, 0);
	VkResult r;
	VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, 0, 0 };
	if ((r = d->CreateCommandPool(d->dev, &pci, 0, &c->pool))) goto fail;
	VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, c->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, c->n };
	if ((r = d->AllocateCommandBuffers(d->dev, &cai, c->cmd))) goto fail;
	VkDeviceSize bsz = (VkDeviceSize)c->ext.width * c->ext.height * 4;
	hostimport_setup(d, c);
	for (uint32_t i = 0; i < c->n; i++) {
		if (d->setLoaderData) d->setLoaderData(d->dev, c->cmd[i]);
		VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0,
			(ci->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
			VK_IMAGE_TYPE_2D, ci->imageFormat, { c->ext.width, c->ext.height, 1 }, 1, ci->imageArrayLayers ? ci->imageArrayLayers : 1,
			VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			VK_SHARING_MODE_EXCLUSIVE, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED };
		if ((r = d->CreateImage(d->dev, &ii, 0, &c->img[i]))) goto fail;
		VkMemoryRequirements mr; d->GetImageMemoryRequirements(d->dev, c->img[i], &mr);
		VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size,
			find_mem(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
		if (ai.memoryTypeIndex == 0xffffffffu) ai.memoryTypeIndex = find_mem(d, mr.memoryTypeBits, 0, 0);
		if ((r = d->AllocateMemory(d->dev, &ai, 0, &c->mem[i]))) goto fail;
		if ((r = d->BindImageMemory(d->dev, c->img[i], c->mem[i], 0))) goto fail;
		VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, bsz, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, 0 };
		if ((r = d->CreateBuffer(d->dev, &bi, 0, &c->buf[i]))) goto fail;
		d->GetBufferMemoryRequirements(d->dev, c->buf[i], &mr);
		uint32_t mt = find_mem(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
		                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
		if (mt == 0xffffffffu) { r = VK_ERROR_OUT_OF_DEVICE_MEMORY; goto fail; }
		c->coherent[i] = (d->memprops.memoryTypes[mt].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
		VkMemoryAllocateInfo bai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mt };
		if ((r = d->AllocateMemory(d->dev, &bai, 0, &c->bmem[i]))) goto fail;
		if ((r = d->BindBufferMemory(d->dev, c->buf[i], c->bmem[i], 0))) goto fail;
		if ((r = d->MapMemory(d->dev, c->bmem[i], 0, VK_WHOLE_SIZE, 0, &c->map[i]))) goto fail;
		VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0, 0 };
		if ((r = d->CreateFence(d->dev, &fi, 0, &c->fence[i]))) goto fail;
		// pre-recorded copy: PRESENT_SRC -> TRANSFER_SRC, image -> buffer, back to PRESENT_SRC, buffer -> host
		VkCommandBufferBeginInfo cb = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, 0, 0 };
		d->BeginCommandBuffer(c->cmd[i], &cb);
		VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VkImageMemoryBarrier b1 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
			VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, c->img[i], rng };
		d->CmdPipelineBarrier(c->cmd[i], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &b1);
		VkBufferImageCopy reg = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { c->ext.width, c->ext.height, 1 } };
		VkBuffer dstb = c->buf[i];
		if (c->himp) {   // panel layout: 1920 px rows (7680 bytes), at most 1920 x 3664
			reg.bufferRowLength = 1920; reg.bufferImageHeight = 3664;
			if (reg.imageExtent.width > 1920) reg.imageExtent.width = 1920;
			if (reg.imageExtent.height > 3664) reg.imageExtent.height = 3664;
			dstb = c->hbuf;
		}
		d->CmdCopyImageToBuffer(c->cmd[i], c->img[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstb, 1, &reg);
		VkImageMemoryBarrier b2 = b1; b2.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b2.dstAccessMask = 0;
		b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		VkBufferMemoryBarrier b3 = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
			VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, dstb, 0, VK_WHOLE_SIZE };
		d->CmdPipelineBarrier(c->cmd[i], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_HOST_BIT,
			0, 0, 0, 1, &b3, 1, &b2);
		if ((r = d->EndCommandBuffer(c->cmd[i]))) goto fail;
	}
	kms_setup(d, c);
	pthread_create(&c->worker, 0, worker_thread, c);
	if (envi("Q2VR_STATUS", 1)) pthread_create(&c->status, 0, status_thread, c);
	qlog("swapchain %ux%u format %d, %u images (usage 0x%x)", c->ext.width, c->ext.height, (int)c->fmt, c->n, (unsigned)ci->imageUsage);
	*out = (VkSwapchainKHR)(uintptr_t)c;
	return VK_SUCCESS;
fail:
	qlog("swapchain creation failed: %d", (int)r);
	return r;   // (leaks the partial swapchain; creation failure is fatal for the compositor anyway)
}

static void destroy_swapchain(struct dev *d, struct swapchain *c)
{
	pthread_mutex_lock(&c->lock); c->dead = 1; pthread_cond_broadcast(&c->cond); pthread_mutex_unlock(&c->lock);
	d->DeviceWaitIdle(d->dev);
	sleep_ns(50000000);
	for (uint32_t i = 0; i < c->n; i++) {
		d->DestroyFence(d->dev, c->fence[i], 0);
		d->DestroyBuffer(d->dev, c->buf[i], 0); d->FreeMemory(d->dev, c->bmem[i], 0);
		d->DestroyImage(d->dev, c->img[i], 0); d->FreeMemory(d->dev, c->mem[i], 0);
	}
	d->DestroyCommandPool(d->dev, c->pool, 0);
	if (c->ksock >= 0 && c->kn) { int ks = c->ksock; c->ksock = -1; close(ks); }   // q2kmsd shows the desktop again
	for (int k = 0; k < 4; k++) { if (c->kbuf[k]) d->DestroyBuffer(d->dev, c->kbuf[k], 0); if (c->kmem[k]) d->FreeMemory(d->dev, c->kmem[k], 0); c->kbuf[k] = 0; c->kmem[k] = 0; }
	c->kn = 0;
	if (c->hbuf) { d->DestroyBuffer(d->dev, c->hbuf, 0); c->hbuf = 0; }
	if (c->hmem) { d->FreeMemory(d->dev, c->hmem, 0); c->hmem = 0; }
	c->himp = 0;
	c->magic = 0;   // the worker may still hold the pointer briefly; the struct is not freed
	vr_inactive();
	qlog("swapchain destroyed after %llu frames", (unsigned long long)c->frames);
}

// ---- instance-level: display extension ----
#define DISPLAY_EXTS_N 6
static const char *display_exts[DISPLAY_EXTS_N] = { "VK_KHR_display", "VK_EXT_direct_mode_display", "VK_EXT_display_surface_counter",
	"VK_EXT_acquire_xlib_display", "VK_EXT_acquire_drm_display", "VK_KHR_get_display_properties2" };
static const VkExtensionProperties layer_inst_exts[] = {
	{ "VK_KHR_display", 23 }, { "VK_EXT_direct_mode_display", 1 }, { "VK_EXT_display_surface_counter", 1 },
	{ "VK_EXT_acquire_xlib_display", 1 }, { "VK_EXT_acquire_drm_display", 1 }, { "VK_KHR_get_display_properties2", 1 } };

static void fill_display_props(VkDisplayPropertiesKHR *p)
{
	memset(p, 0, sizeof *p);
	p->display = Q_DISPLAY; p->displayName = Q_NAME;
	p->physicalDimensions.width = 51; p->physicalDimensions.height = 97;   // mm, the LCD
	p->physicalResolution.width = DW; p->physicalResolution.height = DH;
	p->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	p->planeReorderPossible = VK_FALSE; p->persistentContent = VK_FALSE;
}
static void fill_mode_props(VkDisplayModePropertiesKHR *p)
{
	p->displayMode = Q_MODE; p->parameters.visibleRegion.width = DW; p->parameters.visibleRegion.height = DH;
	p->parameters.refreshRate = DHZ * 1000;
}
static void fill_plane_caps(VkDisplayPlaneCapabilitiesKHR *c)
{
	memset(c, 0, sizeof *c);
	c->supportedAlpha = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
	c->maxSrcPosition.x = 0; c->maxSrcPosition.y = 0;
	c->minSrcExtent.width = 1; c->minSrcExtent.height = 1; c->maxSrcExtent.width = DW; c->maxSrcExtent.height = DH;
	c->minDstExtent.width = DW; c->minDstExtent.height = DH; c->maxDstExtent.width = DW; c->maxDstExtent.height = DH;
}
#define ENUM1(count, arr, fill) do { if (!(arr)) { *(count) = 1; return VK_SUCCESS; } if (*(count) < 1) return VK_INCOMPLETE; *(count) = 1; fill; return VK_SUCCESS; } while (0)

static VKAPI_ATTR VkResult VKAPI_CALL q_GetPhysicalDeviceDisplayPropertiesKHR(VkPhysicalDevice pd, uint32_t *n, VkDisplayPropertiesKHR *p)
{ (void)pd; ENUM1(n, p, fill_display_props(&p[0])); }
static VKAPI_ATTR VkResult VKAPI_CALL q_GetPhysicalDeviceDisplayProperties2KHR(VkPhysicalDevice pd, uint32_t *n, VkDisplayProperties2KHR *p)
{ (void)pd; ENUM1(n, p, fill_display_props(&p[0].displayProperties)); }
static VKAPI_ATTR VkResult VKAPI_CALL q_GetPhysicalDeviceDisplayPlanePropertiesKHR(VkPhysicalDevice pd, uint32_t *n, VkDisplayPlanePropertiesKHR *p)
{ (void)pd; ENUM1(n, p, (p[0].currentDisplay = Q_DISPLAY, p[0].currentStackIndex = 0)); }
static VKAPI_ATTR VkResult VKAPI_CALL q_GetPhysicalDeviceDisplayPlaneProperties2KHR(VkPhysicalDevice pd, uint32_t *n, VkDisplayPlaneProperties2KHR *p)
{ (void)pd; ENUM1(n, p, (p[0].displayPlaneProperties.currentDisplay = Q_DISPLAY, p[0].displayPlaneProperties.currentStackIndex = 0)); }
static VKAPI_ATTR VkResult VKAPI_CALL q_GetDisplayPlaneSupportedDisplaysKHR(VkPhysicalDevice pd, uint32_t plane, uint32_t *n, VkDisplayKHR *d)
{ (void)pd; if (plane != 0) { *n = 0; return VK_SUCCESS; } ENUM1(n, d, d[0] = Q_DISPLAY); }
static VKAPI_ATTR VkResult VKAPI_CALL q_GetDisplayModePropertiesKHR(VkPhysicalDevice pd, VkDisplayKHR disp, uint32_t *n, VkDisplayModePropertiesKHR *p)
{ (void)pd; (void)disp; ENUM1(n, p, fill_mode_props(&p[0])); }
static VKAPI_ATTR VkResult VKAPI_CALL q_GetDisplayModeProperties2KHR(VkPhysicalDevice pd, VkDisplayKHR disp, uint32_t *n, VkDisplayModeProperties2KHR *p)
{ (void)pd; (void)disp; ENUM1(n, p, fill_mode_props(&p[0].displayModeProperties)); }
static VKAPI_ATTR VkResult VKAPI_CALL q_CreateDisplayModeKHR(VkPhysicalDevice pd, VkDisplayKHR disp, const VkDisplayModeCreateInfoKHR *ci,
	const VkAllocationCallbacks *a, VkDisplayModeKHR *m)
{ (void)pd; (void)disp; (void)ci; (void)a; *m = Q_MODE; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL q_GetDisplayPlaneCapabilitiesKHR(VkPhysicalDevice pd, VkDisplayModeKHR m, uint32_t plane, VkDisplayPlaneCapabilitiesKHR *c)
{ (void)pd; (void)m; (void)plane; fill_plane_caps(c); return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL q_GetDisplayPlaneCapabilities2KHR(VkPhysicalDevice pd, const VkDisplayPlaneInfo2KHR *i, VkDisplayPlaneCapabilities2KHR *c)
{ (void)pd; (void)i; fill_plane_caps(&c->capabilities); return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL q_ReleaseDisplayEXT(VkPhysicalDevice pd, VkDisplayKHR d) { (void)pd; (void)d; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL q_AcquireXlibDisplayEXT(VkPhysicalDevice pd, void *dpy, VkDisplayKHR d) { (void)pd; (void)dpy; (void)d; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL q_GetRandROutputDisplayEXT(VkPhysicalDevice pd, void *dpy, unsigned long out, VkDisplayKHR *d)
{ (void)pd; (void)dpy; (void)out; *d = Q_DISPLAY; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL q_AcquireDrmDisplayEXT(VkPhysicalDevice pd, int32_t fd, VkDisplayKHR d) { (void)pd; (void)fd; (void)d; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL q_GetDrmDisplayEXT(VkPhysicalDevice pd, int32_t fd, uint32_t conn, VkDisplayKHR *d)
{ (void)pd; (void)fd; (void)conn; *d = Q_DISPLAY; return VK_SUCCESS; }

static VKAPI_ATTR VkResult VKAPI_CALL q_CreateDisplayPlaneSurfaceKHR(VkInstance inst, const VkDisplaySurfaceCreateInfoKHR *ci,
	const VkAllocationCallbacks *a, VkSurfaceKHR *s)
{
	(void)inst; (void)a;
	uint64_t *h = calloc(1, 64);
	pthread_mutex_lock(&g_lock);
	for (int i = 0; i < MAXS; i++) if (!surfaces[i]) { surfaces[i] = (uint64_t)(uintptr_t)h; break; }
	pthread_mutex_unlock(&g_lock);
	*s = (VkSurfaceKHR)(uintptr_t)h;
	qlog("display surface created (mode %s, %ux%u)", ci->displayMode == Q_MODE ? "ours" : "?", ci->imageExtent.width, ci->imageExtent.height);
	return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL q_DestroySurfaceKHR(VkInstance inst, VkSurfaceKHR s, const VkAllocationCallbacks *a)
{
	if (is_our_surface(s)) {
		pthread_mutex_lock(&g_lock);
		for (int i = 0; i < MAXS; i++) if (surfaces[i] == (uint64_t)s) surfaces[i] = 0;
		pthread_mutex_unlock(&g_lock);
		free((void *)(uintptr_t)s);
		return;
	}
	struct inst *in = inst_of(inst);
	if (in && in->DestroySurface) in->DestroySurface(inst, s, a);
}
static void fill_caps(VkSurfaceCapabilitiesKHR *c)
{
	memset(c, 0, sizeof *c);
	c->minImageCount = 2; c->maxImageCount = 3;
	c->currentExtent.width = DW; c->currentExtent.height = DH;
	c->minImageExtent = c->currentExtent; c->maxImageExtent = c->currentExtent; c->maxImageArrayLayers = 1;
	c->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR; c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
}
static VKAPI_ATTR VkResult VKAPI_CALL q_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice pd, uint32_t qf, VkSurfaceKHR s, VkBool32 *ok)
{
	if (is_our_surface(s)) { *ok = VK_TRUE; return VK_SUCCESS; }
	struct inst *in = inst_of(pd); return in && in->SurfaceSupport ? in->SurfaceSupport(pd, mapf_in(in, qf), s, ok) : VK_ERROR_SURFACE_LOST_KHR;
}
static VKAPI_ATTR VkResult VKAPI_CALL q_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice pd, VkSurfaceKHR s, VkSurfaceCapabilitiesKHR *c)
{
	if (is_our_surface(s)) { fill_caps(c); return VK_SUCCESS; }
	struct inst *in = inst_of(pd); return in && in->SurfaceCaps ? in->SurfaceCaps(pd, s, c) : VK_ERROR_SURFACE_LOST_KHR;
}
static VKAPI_ATTR VkResult VKAPI_CALL q_GetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR *si,
	VkSurfaceCapabilities2KHR *c)
{
	if (is_our_surface(si->surface)) { fill_caps(&c->surfaceCapabilities); return VK_SUCCESS; }
	struct inst *in = inst_of(pd); return in && in->SurfaceCaps2 ? in->SurfaceCaps2(pd, si, c) : VK_ERROR_SURFACE_LOST_KHR;
}
static VKAPI_ATTR VkResult VKAPI_CALL q_GetPhysicalDeviceSurfaceCapabilities2EXT(VkPhysicalDevice pd, VkSurfaceKHR s, VkSurfaceCapabilities2EXT *c)
{
	(void)pd; (void)s;
	VkSurfaceCapabilitiesKHR k; fill_caps(&k);
	c->minImageCount = k.minImageCount; c->maxImageCount = k.maxImageCount; c->currentExtent = k.currentExtent;
	c->minImageExtent = k.minImageExtent; c->maxImageExtent = k.maxImageExtent; c->maxImageArrayLayers = 1;
	c->supportedTransforms = k.supportedTransforms; c->currentTransform = k.currentTransform;
	c->supportedCompositeAlpha = k.supportedCompositeAlpha; c->supportedUsageFlags = k.supportedUsageFlags;
	c->supportedSurfaceCounters = VK_SURFACE_COUNTER_VBLANK_BIT_EXT;
	return VK_SUCCESS;
}
static const VkFormat our_formats[2] = { VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM };
static VKAPI_ATTR VkResult VKAPI_CALL q_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice pd, VkSurfaceKHR s, uint32_t *n, VkSurfaceFormatKHR *f)
{
	if (!is_our_surface(s)) { struct inst *in = inst_of(pd); return in && in->SurfaceFormats ? in->SurfaceFormats(pd, s, n, f) : VK_ERROR_SURFACE_LOST_KHR; }
	if (!f) { *n = 2; return VK_SUCCESS; }
	uint32_t k = *n < 2 ? *n : 2;
	for (uint32_t i = 0; i < k; i++) { f[i].format = our_formats[i]; f[i].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR; }
	*n = k; return k < 2 ? VK_INCOMPLETE : VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL q_GetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR *si,
	uint32_t *n, VkSurfaceFormat2KHR *f)
{
	if (!is_our_surface(si->surface)) { struct inst *in = inst_of(pd); return in && in->SurfaceFormats2 ? in->SurfaceFormats2(pd, si, n, f) : VK_ERROR_SURFACE_LOST_KHR; }
	if (!f) { *n = 2; return VK_SUCCESS; }
	uint32_t k = *n < 2 ? *n : 2;
	for (uint32_t i = 0; i < k; i++) { f[i].surfaceFormat.format = our_formats[i]; f[i].surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR; }
	*n = k; return k < 2 ? VK_INCOMPLETE : VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL q_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice pd, VkSurfaceKHR s, uint32_t *n, VkPresentModeKHR *m)
{
	if (!is_our_surface(s)) { struct inst *in = inst_of(pd); return in && in->SurfaceModes ? in->SurfaceModes(pd, s, n, m) : VK_ERROR_SURFACE_LOST_KHR; }
	ENUM1(n, m, m[0] = VK_PRESENT_MODE_FIFO_KHR);
}

// ---- queues ----
// vrcompositor asserts on a compute queue from a compute-only queue family; Turnip has one family with one queue.
// We advertise an extra family (COMPUTE|TRANSFER, 1 queue) backed by the real one, and 2 queues per real family;
// every queue request beyond the real ones aliases queue 0 of the real family. Submissions are serialized anyway.
// All places that carry a queue family index are remapped (device/queue creation, command pools, sharing lists,
// ownership-transfer barriers become plain barriers).
#define QWANT 2
static void fam_learn(struct inst *in, VkPhysicalDevice pd)
{
	if (in->fam_known) return;
	uint32_t n = 0; VkQueueFamilyProperties f[16];
	in->QFP(pd, &n, 0); if (n > 16) n = 16;
	in->QFP(pd, &n, f);
	in->realNF = n; in->baseF = 0; in->addFake = 1;
	for (uint32_t i = 0; i < n; i++) {
		if ((f[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(f[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) in->addFake = 0;
	}
	for (uint32_t i = 0; i < n; i++) if (f[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { in->baseF = i; break; }
	in->fam_known = 1;
	qlog("queue families: %u real%s", n, in->addFake ? ", adding a compute-only family backed by the first one" : "");
}
static uint32_t mapf_in(struct inst *in, uint32_t f)
{
	if (!in || !in->fam_known || !in->addFake) return f;
	return f == in->realNF ? in->baseF : f;
}
static void bump(VkQueueFamilyProperties *p)
{ if ((p->queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) && p->queueCount < QWANT) p->queueCount = QWANT; }
static VKAPI_ATTR void VKAPI_CALL q_GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice pd, uint32_t *n, VkQueueFamilyProperties *p)
{
	struct inst *in = inst_of(pd); fam_learn(in, pd);
	uint32_t tot = in->realNF + (in->addFake ? 1 : 0);
	if (!p) { *n = tot; return; }
	uint32_t want = *n, rn = in->realNF;
	VkQueueFamilyProperties t[17];
	in->QFP(pd, &rn, t);
	uint32_t k = 0;
	for (; k < want && k < in->realNF; k++) { p[k] = t[k]; bump(&p[k]); }
	if (in->addFake && k < want) { p[k] = t[in->baseF]; p[k].queueFlags = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT; p[k].queueCount = 1; k++; }
	*n = k;
}
static VKAPI_ATTR void VKAPI_CALL q_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice pd, uint32_t *n, VkQueueFamilyProperties2 *p)
{
	struct inst *in = inst_of(pd); fam_learn(in, pd);
	PFN_vkGetPhysicalDeviceQueueFamilyProperties2 f = in->QFP2 ? in->QFP2 : in->QFP2KHR;
	uint32_t tot = in->realNF + (in->addFake ? 1 : 0);
	if (!p) { *n = tot; return; }
	uint32_t want = *n, rn = in->realNF;
	if (rn > 16) rn = 16;
	VkQueueFamilyProperties2 t[16];
	for (uint32_t i = 0; i < rn; i++) { t[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2; t[i].pNext = i < want ? p[i].pNext : 0; }
	f(pd, &rn, t);
	uint32_t k = 0;
	for (; k < want && k < in->realNF; k++) { p[k].queueFamilyProperties = t[k].queueFamilyProperties; bump(&p[k].queueFamilyProperties); }
	if (in->addFake && k < want) {
		// fill the app's extension structs of the fake entry from the backing family
		uint32_t rn2 = in->realNF;
		for (uint32_t i = 0; i < rn2; i++) { t[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2; t[i].pNext = i == in->baseF ? p[k].pNext : 0; }
		f(pd, &rn2, t);
		p[k].queueFamilyProperties = t[in->baseF].queueFamilyProperties;
		p[k].queueFamilyProperties.queueFlags = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
		p[k].queueFamilyProperties.queueCount = 1;
		k++;
	}
	*n = k;
}
static VKAPI_ATTR void VKAPI_CALL q_GetPhysicalDeviceQueueFamilyProperties2KHR(VkPhysicalDevice pd, uint32_t *n, VkQueueFamilyProperties2 *p)
{ q_GetPhysicalDeviceQueueFamilyProperties2(pd, n, p); }

// ---- device-level ----
static VKAPI_ATTR void VKAPI_CALL q_GetDeviceQueue(VkDevice dv, uint32_t fam, uint32_t idx, VkQueue *q)
{
	struct dev *d = dev_of(dv);
	fam = mapf(d, fam);
	if (fam < 16 && d->realq[fam] && idx >= d->realq[fam]) idx = 0;   // aliased queue
	d->GetDeviceQueue(dv, fam, idx, q);
	if (!d->queue && *q) d->queue = *q;
}
static VKAPI_ATTR void VKAPI_CALL q_GetDeviceQueue2(VkDevice dv, const VkDeviceQueueInfo2 *i, VkQueue *q)
{
	struct dev *d = dev_of(dv);
	VkDeviceQueueInfo2 i2 = *i;
	i2.queueFamilyIndex = mapf(d, i2.queueFamilyIndex);
	if (i2.queueFamilyIndex < 16 && d->realq[i2.queueFamilyIndex] && i2.queueIndex >= d->realq[i2.queueFamilyIndex]) i2.queueIndex = 0;
	d->GetDeviceQueue2(dv, &i2, q);
	if (!d->queue && *q) d->queue = *q;
}
static void note_waits1(uint32_t n, const VkSubmitInfo *s);
static void note_waits2(uint32_t n, const VkSubmitInfo2 *s);
static void bsem_after1(uint32_t n, const VkSubmitInfo *s);
static void bsem_after2(uint32_t n, const VkSubmitInfo2 *s);
static void bsem_before1(uint32_t n, const VkSubmitInfo *s);
static void bsem_before2(uint32_t n, const VkSubmitInfo2 *s);
static VKAPI_ATTR VkResult VKAPI_CALL q_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f)
{ struct dev *d = dev_of(q); note_waits1(n, s); bsem_before1(n, s); struct qlk_e *L = qlk(q, "app submit"); VkResult r = d->QueueSubmit(q, n, s, f); qulk(L); if (!r) bsem_after1(n, s); return r; }
static VKAPI_ATTR VkResult VKAPI_CALL q_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *s, VkFence f)
{ struct dev *d = dev_of(q); note_waits2(n, s); bsem_before2(n, s); struct qlk_e *L = qlk(q, "app submit2"); VkResult r = d->QueueSubmit2(q, n, s, f); qulk(L); if (!r) bsem_after2(n, s); return r; }
static VKAPI_ATTR VkResult VKAPI_CALL q_QueueSubmit2KHR(VkQueue q, uint32_t n, const VkSubmitInfo2 *s, VkFence f)
{ struct dev *d = dev_of(q); note_waits2(n, s); bsem_before2(n, s); struct qlk_e *L = qlk(q, "app submit2"); VkResult r = d->QueueSubmit2KHR(q, n, s, f); qulk(L); if (!r) bsem_after2(n, s); return r; }
// desk-190: wait for the queue to drain WITHOUT holding its lock (an empty submit with a fence, then wait on the fence),
// so other threads can keep submitting - including the work this wait may depend on.
static VKAPI_ATTR VkResult VKAPI_CALL q_QueueWaitIdle(VkQueue q)
{
	struct dev *d = dev_of(q); VkFence f = 0;
	VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0, 0 };
	if (!envi("Q2VR_WAITIDLE_LOCKED", 0) && d->CreateFence && d->WaitForFences && d->DestroyFence && d->CreateFence(d->dev, &fi, 0, &f) == VK_SUCCESS) {
		struct qlk_e *L = qlk(q, "wait-idle marker"); VkResult r = d->QueueSubmit(q, 0, 0, f); qulk(L);
		if (r == VK_SUCCESS) r = d->WaitForFences(d->dev, 1, &f, 1, ~0ull);
		d->DestroyFence(d->dev, f, 0);
		return r;
	}
	struct qlk_e *L = qlk(q, "wait idle"); VkResult r = d->QueueWaitIdle(q); qulk(L); return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL q_QueueBindSparse(VkQueue q, uint32_t n, const VkBindSparseInfo *b, VkFence f)
{ struct dev *d = dev_of(q); struct qlk_e *L = qlk(q, "bind sparse"); VkResult r = d->QueueBindSparse(q, n, b, f); qulk(L); return r; }

static VKAPI_ATTR VkResult VKAPI_CALL q_CreateSwapchainKHR(VkDevice dv, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSwapchainKHR *s)
{
	struct dev *d = dev_of(dv);
	if (is_our_surface(ci->surface)) {
		if (ci->oldSwapchain && sc_of(ci->oldSwapchain)) { /* the old one stays until destroyed */ }
		return create_swapchain(d, ci, s);
	}
	return d->CreateSwapchainKHR(dv, ci, a, s);
}
static VKAPI_ATTR void VKAPI_CALL q_DestroySwapchainKHR(VkDevice dv, VkSwapchainKHR s, const VkAllocationCallbacks *a)
{
	struct dev *d = dev_of(dv); struct swapchain *c = sc_of(s);
	if (c) { destroy_swapchain(d, c); return; }
	if (s) d->DestroySwapchainKHR(dv, s, a);
}
static VKAPI_ATTR VkResult VKAPI_CALL q_GetSwapchainImagesKHR(VkDevice dv, VkSwapchainKHR s, uint32_t *n, VkImage *img)
{
	struct dev *d = dev_of(dv); struct swapchain *c = sc_of(s);
	if (!c) return d->GetSwapchainImagesKHR(dv, s, n, img);
	if (!img) { *n = c->n; return VK_SUCCESS; }
	uint32_t k = *n < c->n ? *n : c->n;
	for (uint32_t i = 0; i < k; i++) img[i] = c->img[i];
	*n = k; return k < c->n ? VK_INCOMPLETE : VK_SUCCESS;
}
static VkResult acquire(struct dev *d, struct swapchain *c, uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t *idx)
{
	pthread_mutex_lock(&c->lock);
	uint64_t deadline = timeout == UINT64_MAX ? UINT64_MAX : now_ns() + timeout;
	int found = -1;
	for (;;) {
		for (uint32_t k = 0; k < c->n; k++) { uint32_t i = (c->next + k) % c->n; if (c->state[i] == 0) { found = (int)i; break; } }
		if (found >= 0) break;
		if (timeout == 0) { pthread_mutex_unlock(&c->lock); return VK_NOT_READY; }
		c->acquire_waits++;
		if (deadline != UINT64_MAX && now_ns() >= deadline) { pthread_mutex_unlock(&c->lock); return VK_TIMEOUT; }
		struct qtimespec ts; clock_gettime(Q_CLOCK_REALTIME, &ts); ts.tv_nsec += 20000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
		pthread_cond_timedwait(&c->cond, &c->lock, &ts);
	}
	c->state[found] = 1; c->next = (uint32_t)(found + 1) % c->n; c->acquires++;
	pthread_mutex_unlock(&c->lock);
	*idx = (uint32_t)found;
	if (fence && !cpu_signal(d, fence)) fence = VK_NULL_HANDLE;   // signalled already, from the CPU
	if (sem || fence) {
		VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 0, 0, sem ? 1u : 0u, sem ? &sem : 0 };
		VkResult r = VK_ERROR_INITIALIZATION_FAILED;
		if (d->queue) { struct qlk_e *L = qlk(d->queue, "acquire signal"); r = d->QueueSubmit(d->queue, 1, &si, fence); qulk(L); }
		if (r) { qlog("acquire: signal submit failed (%d, queue %p)", (int)r, (void *)d->queue); return r; }
	}
	return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL q_AcquireNextImageKHR(VkDevice dv, VkSwapchainKHR s, uint64_t t, VkSemaphore sem, VkFence f, uint32_t *i)
{
	struct dev *d = dev_of(dv); struct swapchain *c = sc_of(s);
	return c ? acquire(d, c, t, sem, f, i) : d->AcquireNextImageKHR(dv, s, t, sem, f, i);
}
static VKAPI_ATTR VkResult VKAPI_CALL q_AcquireNextImage2KHR(VkDevice dv, const VkAcquireNextImageInfoKHR *ai, uint32_t *i)
{
	struct dev *d = dev_of(dv); struct swapchain *c = sc_of(ai->swapchain);
	return c ? acquire(d, c, ai->timeout, ai->semaphore, ai->fence, i) : d->AcquireNextImage2KHR(dv, ai, i);
}
static VKAPI_ATTR VkResult VKAPI_CALL q_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR *pi)
{
	struct dev *d = dev_of(q);
	int ours = 0;
	for (uint32_t k = 0; k < pi->swapchainCount; k++) if (sc_of(pi->pSwapchains[k])) ours++;
	if (!ours) { struct qlk_e *L = qlk(q, "present (not ours)"); VkResult r = d->QueuePresentKHR(q, pi); qulk(L); return r; }
	{   uint64_t t = now_ns();
		if (g_pi_last) { uint64_t r = (t - g_pi_last + 5555555ull) / 11111111ull; g_pi_hist[r > 5 ? 5 : r]++; }
		g_pi_last = t; if (g_tick_ns && t > g_tick_ns) { g_phase_sum += t - g_tick_ns; g_phase_n++; } }
	VkResult res = VK_SUCCESS;
	const VkPresentIdKHR *pid = 0;
	for (const VkBaseInStructure *b = pi->pNext; b; b = b->pNext) if (b->sType == VK_STRUCTURE_TYPE_PRESENT_ID_KHR) pid = (const VkPresentIdKHR *)b;
	for (uint32_t k = 0; k < pi->swapchainCount; k++) {
		struct swapchain *c = sc_of(pi->pSwapchains[k]);
		VkResult r = VK_ERROR_OUT_OF_DATE_KHR;
		if (c) {
			uint32_t i = pi->pImageIndices[k];
			VkPipelineStageFlags st[16];
			uint32_t nw = k == 0 ? pi->waitSemaphoreCount : 0; if (nw > 16) nw = 16;
			for (uint32_t w = 0; w < nw; w++) st[w] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			int kk = -1;
			if (c->kn) {   // zero-copy: a scanout buffer that is neither on the panel nor waiting to be
				pthread_mutex_lock(&c->lock);
				uint64_t until = now_ns() + 40000000ull;
				for (;;) {
					for (int j = 0; j < c->kn && kk < 0; j++) if (c->kstate[j] == 0) kk = j;
					if (kk >= 0 || !c->kn || now_ns() >= until) break;
					uint64_t tw = now_ns() + 2000000ull; struct qtimespec ts;
					{ struct qtimespec rt; clock_gettime(Q_CLOCK_REALTIME, &rt); uint64_t a2 = (uint64_t)rt.tv_sec * 1000000000ull + rt.tv_nsec + (tw - now_ns()); ts.tv_sec = (long)(a2 / 1000000000ull); ts.tv_nsec = (long)(a2 % 1000000000ull); }
					pthread_cond_timedwait(&c->cond, &c->lock, &ts);
				}
				if (kk >= 0) c->kstate[kk] = 1; else c->knofree++;
				pthread_mutex_unlock(&c->lock);
			}
			VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, nw, pi->pWaitSemaphores, st, 1, kk >= 0 ? &c->kcmd[i][kk] : &c->cmd[i], 0, 0 };
			{ struct qlk_e *L = qlk(q, "present copy"); r = d->QueueSubmit(q, 1, &si, c->fence[i]); qulk(L); }
			if (r != VK_SUCCESS && kk >= 0) { pthread_mutex_lock(&c->lock); c->kstate[kk] = 0; pthread_mutex_unlock(&c->lock); }
			if (r == VK_SUCCESS) {
				pthread_mutex_lock(&c->lock);
				c->state[i] = 2; c->q[c->qt % (MAXIMG * 2)] = (int)i; c->qk[c->qt % (MAXIMG * 2)] = kk; c->presents++;
				c->qid[c->qt % (MAXIMG * 2)] = (pid && pid->pPresentIds && k < pid->swapchainCount) ? pid->pPresentIds[k] : 0; c->qt++;
				pthread_cond_broadcast(&c->cond);
				pthread_mutex_unlock(&c->lock);
			}
		}
		if (r) qlog("present: copy submit failed (%d)", (int)r);
		if (pi->pResults) pi->pResults[k] = r;
		if (r && !res) res = r;
	}
	return res;
}

// ---- queue family remapping in device calls ----
static VKAPI_ATTR VkResult VKAPI_CALL q_CreateCommandPool(VkDevice dv, const VkCommandPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkCommandPool *p)
{
	struct dev *d = dev_of(dv); VkCommandPoolCreateInfo c = *ci; c.queueFamilyIndex = mapf(d, c.queueFamilyIndex);
	return d->CreateCommandPool(dv, &c, a, p);
}
static void map_sharing(struct dev *d, VkSharingMode *mode, uint32_t *n, const uint32_t **idx, uint32_t *store)
{
	if (*mode != VK_SHARING_MODE_CONCURRENT || !*idx) return;
	uint32_t m = 0;
	for (uint32_t i = 0; i < *n && m < 16; i++) {
		uint32_t f = mapf(d, (*idx)[i]); int dup = 0;
		for (uint32_t j = 0; j < m; j++) if (store[j] == f) dup = 1;
		if (!dup) store[m++] = f;
	}
	if (m < 2) { *mode = VK_SHARING_MODE_EXCLUSIVE; *n = 0; *idx = 0; } else { *n = m; *idx = store; }
}
// desk-193: trace of shared (external-memory) images.  Steam's web UI hands its VR panels to SteamVR as GL textures
// on shared memory, and every one failed ("Failed to create OpenGL framebuffer ... 0x8cd6" = incomplete attachment,
// "SetOverlayTexture failed") -> the panels stay blank.  These lines show where the sharing breaks: image parameters,
// memory size needed vs. given, import/export results.  Q2VR_EXTTRACE=0 turns it off; at most 120 lines per process.
static int g_xt_n;
static struct { VkImage im; uint32_t w, h, fmt; uint64_t need; } g_xt[64]; static unsigned g_xt_i;
static int xt_on(void) { static int on = -1; if (on < 0) on = envi("Q2VR_EXTTRACE", 1); return on && g_xt_n < 120; }
static int xt_find(VkImage im) { for (int i = 0; i < 64; i++) if (g_xt[i].im && g_xt[i].im == im) return i; return -1; }
static VKAPI_ATTR VkResult VKAPI_CALL q_CreateImage(VkDevice dv, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *im)
{
	struct dev *d = dev_of(dv); VkImageCreateInfo c = *ci; uint32_t st[16];
	map_sharing(d, &c.sharingMode, &c.queueFamilyIndexCount, &c.pQueueFamilyIndices, st);
	// desk-193: shared images must have the SAME memory layout in every process.  Turnip picks UBWC compression /
	// tiling per image from its create flags (mutable format, format list...), and the two sides of a shared texture
	// (Steam's GL via zink, SteamVR's Vulkan) create it with different flags -> different layouts and sizes -> the GL
	// side's framebuffer is incomplete and the panel stays blank.  So every external-memory image (OPAQUE_FD or dma-buf,
	// no explicit DRM modifier, colour format) gets MUTABLE_FORMAT and no format list: Turnip then always uses its
	// plain, uncompressed layout, identical in every process.  Q2VR_EXTLINEAR=0 turns it off.
	int norm = 0;
	if (envi("Q2VR_EXTLINEAR", 1)) {
		uint32_t ht = 0; int mod = 0;
		for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext) {
			if (p->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO) ht = ((const VkExternalMemoryImageCreateInfo *)p)->handleTypes;
			if (p->sType == VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT || p->sType == VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT) mod = 1;
		}
		int depth = ci->format >= VK_FORMAT_D16_UNORM && ci->format <= VK_FORMAT_D32_SFLOAT_S8_UINT;
		int compressed = ci->format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && ci->format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
		if ((ht & (VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT | VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)) && !mod && !depth && !compressed
		    && ci->tiling == VK_IMAGE_TILING_OPTIMAL && ci->samples == VK_SAMPLE_COUNT_1_BIT) {
			norm = 1;
		}
	}
	VkResult r;
	if (norm) {
		// rebuild the chain: duplicate each struct we keep (all are small, fixed-size Vulkan structs we know)
		uint64_t store[8][8]; const VkBaseInStructure *p = ci->pNext; VkBaseOutStructure *last = 0; void *first = 0; int k = 0, ok = 1;
		for (; p && ok; p = p->pNext) {
			if (p->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) continue;
			size_t sz = 0;
			switch (p->sType) {
			case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO: sz = sizeof(VkExternalMemoryImageCreateInfo); break;
			case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_IMAGE_CREATE_INFO_NV: sz = sizeof(VkDedicatedAllocationImageCreateInfoNV); break;
			case VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO: sz = sizeof(VkImageStencilUsageCreateInfo); break;
			case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT: sz = sizeof(VkImageCompressionControlEXT); break;
			default: ok = 0; break;
			}
			if (!ok || k >= 8 || sz > 64) { ok = 0; break; }
			memcpy(store[k], p, sz); ((VkBaseOutStructure *)store[k])->pNext = 0;
			if (last) last->pNext = (VkBaseOutStructure *)store[k]; else first = store[k];
			last = (VkBaseOutStructure *)store[k]; k++;
		}
		if (ok) {
			c.pNext = first; c.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
			r = d->CreateImage(dv, &c, a, im);
			static int said; if (!said++) qlog("shared images: plain uncompressed layout forced (same in every process; Q2VR_EXTLINEAR=0 to turn off)");
		} else {
			static int said2; if (!said2++) qlog("shared images: unknown struct %d in the image chain - layout not normalised", (int)p->sType);
			r = d->CreateImage(dv, &c, a, im);
		}
	} else r = d->CreateImage(dv, &c, a, im);
	if (xt_on()) {
		uint32_t ht = 0; int ext = 0, mod = 0, fl = 0;
		for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext) {
			if (p->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO) { ext = 1; ht = ((const VkExternalMemoryImageCreateInfo *)p)->handleTypes; }
			if (p->sType == VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT || p->sType == VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT) mod = 1;
			if (p->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) fl = (int)((const VkImageFormatListCreateInfo *)p)->viewFormatCount;
		}
		if (ext) {
			VkMemoryRequirements mr = { 0, 0, 0 }; if (r == VK_SUCCESS) d->GetImageMemoryRequirements(dv, *im, &mr);
			g_xt_n++;
			qlog("ext-image: %ux%u format %d tiling %d usage 0x%x flags 0x%x samples %d mips %u layers %u handles 0x%x%s viewformats %d -> %d, needs %llu bytes (align %llu, types 0x%x)",
			     ci->extent.width, ci->extent.height, (int)ci->format, (int)ci->tiling, ci->usage, ci->flags, (int)ci->samples, ci->mipLevels, ci->arrayLayers,
			     ht, mod ? " drm-modifier" : "", fl, (int)r, (unsigned long long)mr.size, (unsigned long long)mr.alignment, mr.memoryTypeBits);
			if (r == VK_SUCCESS) { unsigned k = g_xt_i++ & 63; g_xt[k].im = *im; g_xt[k].w = ci->extent.width; g_xt[k].h = ci->extent.height; g_xt[k].fmt = ci->format; g_xt[k].need = mr.size; }
		}
	}
	return r;
}
// ---- desk-296: GPU->CPU read-back statistics (Steam's web UI only: Q2VR_LOGDIR=1) ----
// Do Steam's VR panel frames travel GPU -> CPU -> GPU?  Counts image->buffer copies and memory maps per 10 s, by
// memory type (type 0 on KGSL = uncached: CPU reads of it are very slow), and lists the device's memory types once.
#define MAXMT 2048
static struct { VkDeviceMemory m; uint32_t type; uint64_t size; } g_mt[MAXMT];
static unsigned g_mtn; static qmutex g_mtl; static int g_mti, g_rb = -1;
static unsigned long g_cp, g_map[8], g_mapsz[8]; static uint64_t g_cpbytes, g_rbt0;
static int rb_on(void) { if (g_rb < 0) { const char *e = getenv("Q2VR_LOGDIR"); g_rb = e && *e == '1'; if (g_rb) { pthread_mutex_init(&g_mtl, 0); g_mti = 1; } } return g_rb; }
static void rb_note_alloc(struct dev *d, VkDeviceMemory m, const VkMemoryAllocateInfo *ai)
{
	if (!rb_on() || !m) return;
	static int listed;
	if (!listed) { listed = 1; char b[400]; int o = 0;
		for (uint32_t i = 0; i < d->memprops.memoryTypeCount && o < 360; i++) { VkMemoryPropertyFlags f = d->memprops.memoryTypes[i].propertyFlags;
			o += snprintf(b + o, sizeof b - o, " [%u:%s%s%s%s]", i, f & 1 ? "DEV " : "", f & 2 ? "VIS " : "", f & 4 ? "COH " : "", f & 8 ? "CACHED" : "UNCACHED"); }
		b[o] = 0; qlog("memory types:%s", b); }
	pthread_mutex_lock(&g_mtl);
	unsigned k = g_mtn++ % MAXMT; g_mt[k].m = m; g_mt[k].type = ai->memoryTypeIndex; g_mt[k].size = ai->allocationSize;
	pthread_mutex_unlock(&g_mtl);
}
static void rb_report(void)
{
	uint64_t t = now_ns();
	if (!g_rbt0) { g_rbt0 = t; return; }
	if (t - g_rbt0 < 10000000000ull) return;
	double sec = (t - g_rbt0) / 1e9;
	qlog("read-back: %lu image->buffer copies (%.1f MB) in %.1f s; memory maps by type: 0:%lu (%.1f MB) 1:%lu (%.1f MB) 2:%lu (%.1f MB)", g_cp, g_cpbytes / 1e6, sec,
	     g_map[0], g_mapsz[0] / 1e6, g_map[1], g_mapsz[1] / 1e6, g_map[2], g_mapsz[2] / 1e6);
	g_rbt0 = t; g_cp = 0; g_cpbytes = 0; for (int i = 0; i < 8; i++) { g_map[i] = 0; g_mapsz[i] = 0; }
}
static VKAPI_ATTR VkResult VKAPI_CALL q_MapMemory(VkDevice dv, VkDeviceMemory m, VkDeviceSize off, VkDeviceSize sz, VkMemoryMapFlags fl, void **pp)
{
	struct dev *d = dev_of(dv);
	if (rb_on()) {
		pthread_mutex_lock(&g_mtl);
		unsigned n = g_mtn < MAXMT ? g_mtn : MAXMT; int ty = 7; uint64_t size = 0;
		for (unsigned i = 0; i < n; i++) if (g_mt[i].m == m) { ty = g_mt[i].type < 7 ? (int)g_mt[i].type : 7; size = g_mt[i].size; break; }
		g_map[ty]++; g_mapsz[ty] += sz == VK_WHOLE_SIZE ? size : sz; rb_report();
		pthread_mutex_unlock(&g_mtl);
	}
	return d->MapMemory(dv, m, off, sz, fl, pp);
}
static VKAPI_ATTR void VKAPI_CALL q_CmdCopyImageToBuffer(VkCommandBuffer cb, VkImage im, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy *r)
{
	struct dev *d = dev_of(cb);
	if (rb_on()) {
		pthread_mutex_lock(&g_mtl);
		g_cp++; for (uint32_t i = 0; i < n; i++) g_cpbytes += 4ull * r[i].imageExtent.width * r[i].imageExtent.height * (r[i].imageExtent.depth ? r[i].imageExtent.depth : 1);
		rb_report();
		pthread_mutex_unlock(&g_mtl);
	}
	d->CmdCopyImageToBuffer(cb, im, l, b, n, r);
}
static VKAPI_ATTR VkResult VKAPI_CALL q_AllocateMemory(VkDevice dv, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *m)
{
	struct dev *d = dev_of(dv);
	VkResult r = d->AllocateMemory(dv, ai, a, m);
	if (!r) rb_note_alloc(d, *m, ai);   // desk-296
	if (xt_on()) {
		int imp = -1, expo = 0; uint32_t iht = 0, eht = 0; VkImage ded = 0; char tgt[96] = "";
		for (const VkBaseInStructure *p = ai->pNext; p; p = p->pNext) {
			if (p->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR) { imp = ((const VkImportMemoryFdInfoKHR *)p)->fd; iht = ((const VkImportMemoryFdInfoKHR *)p)->handleType; }
			if (p->sType == VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO) { expo = 1; eht = ((const VkExportMemoryAllocateInfo *)p)->handleTypes; }
			if (p->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) ded = ((const VkMemoryDedicatedAllocateInfo *)p)->image;
		}
		if (imp >= 0 || expo) {
			if (imp >= 0) { char pth[48]; snprintf(pth, sizeof pth, "/proc/self/fd/%d", imp); long n = readlink(pth, tgt, sizeof tgt - 1); tgt[n > 0 ? n : 0] = 0; }
			int k = ded ? xt_find(ded) : -1;
			g_xt_n++;
			qlog("ext-memory: %s %llu bytes, type %u, handle 0x%x%s%s%s, dedicated %s (needs %llu) -> %d",
			     imp >= 0 ? "IMPORT" : "EXPORTABLE", (unsigned long long)ai->allocationSize, ai->memoryTypeIndex, imp >= 0 ? iht : eht,
			     imp >= 0 ? " fd " : "", imp >= 0 ? tgt : "", "", ded ? (k >= 0 ? "traced image" : "other image") : "no", k >= 0 ? (unsigned long long)g_xt[k].need : 0ull, (int)r);
		}
	}
	return r;
}
static void xt_bind(VkImage im, VkDeviceSize off, VkResult r, const char *how)
{
	int k = xt_find(im);
	if (k < 0 || !xt_on()) return;
	g_xt_n++;
	qlog("ext-image bind (%s): %ux%u format %u at offset %llu -> %d", how, g_xt[k].w, g_xt[k].h, g_xt[k].fmt, (unsigned long long)off, (int)r);
}
static VKAPI_ATTR VkResult VKAPI_CALL q_BindImageMemory(VkDevice dv, VkImage im, VkDeviceMemory m, VkDeviceSize off)
{ struct dev *d = dev_of(dv); VkResult r = d->BindImageMemory(dv, im, m, off); xt_bind(im, off, r, "1"); return r; }
static VKAPI_ATTR VkResult VKAPI_CALL q_BindImageMemory2(VkDevice dv, uint32_t n, const VkBindImageMemoryInfo *b)
{ struct dev *d = dev_of(dv); VkResult r = d->BindImageMemory2(dv, n, b); for (uint32_t i = 0; i < n; i++) xt_bind(b[i].image, b[i].memoryOffset, r, "2"); return r; }
static VKAPI_ATTR VkResult VKAPI_CALL q_BindImageMemory2KHR(VkDevice dv, uint32_t n, const VkBindImageMemoryInfo *b)
{ struct dev *d = dev_of(dv); VkResult r = d->BindImageMemory2KHR(dv, n, b); for (uint32_t i = 0; i < n; i++) xt_bind(b[i].image, b[i].memoryOffset, r, "2KHR"); return r; }
static VKAPI_ATTR VkResult VKAPI_CALL q_GetMemoryFdKHR(VkDevice dv, const VkMemoryGetFdInfoKHR *gi, int *fd)
{
	struct dev *d = dev_of(dv);
	VkResult r = d->GetMemoryFdKHR(dv, gi, fd);
	if (xt_on()) { char pth[48], tgt[96]; long n = -1; if (r == VK_SUCCESS) { snprintf(pth, sizeof pth, "/proc/self/fd/%d", *fd); n = readlink(pth, tgt, sizeof tgt - 1); } tgt[n > 0 ? n : 0] = 0;
		g_xt_n++; qlog("ext-memory EXPORT fd: handle 0x%x -> %d (%s)", gi->handleType, (int)r, tgt); }
	return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL q_CreateBuffer(VkDevice dv, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *b)
{
	struct dev *d = dev_of(dv); VkBufferCreateInfo c = *ci; uint32_t st[16];
	map_sharing(d, &c.sharingMode, &c.queueFamilyIndexCount, &c.pQueueFamilyIndices, st);
	return d->CreateBuffer(dv, &c, a, b);
}
// ownership transfers between the real family and its alias become plain barriers
static void fix_pair(struct dev *d, uint32_t *src, uint32_t *dst)
{
	uint32_t s2 = *src, d2 = *dst;
	if (s2 != VK_QUEUE_FAMILY_IGNORED && s2 < 0xfffffff0u) s2 = mapf(d, s2);
	if (d2 != VK_QUEUE_FAMILY_IGNORED && d2 < 0xfffffff0u) d2 = mapf(d, d2);
	if (s2 == d2) { s2 = VK_QUEUE_FAMILY_IGNORED; d2 = VK_QUEUE_FAMILY_IGNORED; }
	*src = s2; *dst = d2;
}
#define TMPARR(T, name, n, srcp) T name##_s[32]; T *name = (n) <= 32 ? name##_s : malloc(sizeof(T) * (n)); if (n) memcpy(name, srcp, sizeof(T) * (n))
#define TMPFREE(name) do { if (name != name##_s) free(name); } while (0)
static VKAPI_ATTR void VKAPI_CALL q_CmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags ss, VkPipelineStageFlags ds, VkDependencyFlags df,
	uint32_t nm, const VkMemoryBarrier *mb, uint32_t nb, const VkBufferMemoryBarrier *bb, uint32_t ni, const VkImageMemoryBarrier *ib)
{
	struct dev *d = dev_of(cb);
	TMPARR(VkBufferMemoryBarrier, b2, nb, bb); TMPARR(VkImageMemoryBarrier, i2, ni, ib);
	for (uint32_t i = 0; i < nb; i++) fix_pair(d, &b2[i].srcQueueFamilyIndex, &b2[i].dstQueueFamilyIndex);
	for (uint32_t i = 0; i < ni; i++) fix_pair(d, &i2[i].srcQueueFamilyIndex, &i2[i].dstQueueFamilyIndex);
	d->CmdPipelineBarrier(cb, ss, ds, df, nm, mb, nb, b2, ni, i2);
	TMPFREE(b2); TMPFREE(i2);
}
static void fix_dep(struct dev *d, const VkDependencyInfo *in, VkDependencyInfo *out, VkBufferMemoryBarrier2 **bo, VkImageMemoryBarrier2 **io)
{
	*out = *in;
	VkBufferMemoryBarrier2 *b = in->bufferMemoryBarrierCount ? malloc(sizeof *b * in->bufferMemoryBarrierCount) : 0;
	VkImageMemoryBarrier2 *im = in->imageMemoryBarrierCount ? malloc(sizeof *im * in->imageMemoryBarrierCount) : 0;
	for (uint32_t i = 0; i < in->bufferMemoryBarrierCount; i++) { b[i] = in->pBufferMemoryBarriers[i]; fix_pair(d, &b[i].srcQueueFamilyIndex, &b[i].dstQueueFamilyIndex); }
	for (uint32_t i = 0; i < in->imageMemoryBarrierCount; i++) { im[i] = in->pImageMemoryBarriers[i]; fix_pair(d, &im[i].srcQueueFamilyIndex, &im[i].dstQueueFamilyIndex); }
	out->pBufferMemoryBarriers = b; out->pImageMemoryBarriers = im; *bo = b; *io = im;
}
static VKAPI_ATTR void VKAPI_CALL q_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *di)
{ struct dev *d = dev_of(cb); VkDependencyInfo o; VkBufferMemoryBarrier2 *b; VkImageMemoryBarrier2 *i; fix_dep(d, di, &o, &b, &i); d->CmdPipelineBarrier2(cb, &o); free(b); free(i); }
static VKAPI_ATTR void VKAPI_CALL q_CmdPipelineBarrier2KHR(VkCommandBuffer cb, const VkDependencyInfo *di)
{ struct dev *d = dev_of(cb); VkDependencyInfo o; VkBufferMemoryBarrier2 *b; VkImageMemoryBarrier2 *i; fix_dep(d, di, &o, &b, &i); d->CmdPipelineBarrier2KHR(cb, &o); free(b); free(i); }
static VKAPI_ATTR void VKAPI_CALL q_CmdWaitEvents(VkCommandBuffer cb, uint32_t ne, const VkEvent *ev, VkPipelineStageFlags ss, VkPipelineStageFlags ds,
	uint32_t nm, const VkMemoryBarrier *mb, uint32_t nb, const VkBufferMemoryBarrier *bb, uint32_t ni, const VkImageMemoryBarrier *ib)
{
	struct dev *d = dev_of(cb);
	TMPARR(VkBufferMemoryBarrier, b2, nb, bb); TMPARR(VkImageMemoryBarrier, i2, ni, ib);
	for (uint32_t i = 0; i < nb; i++) fix_pair(d, &b2[i].srcQueueFamilyIndex, &b2[i].dstQueueFamilyIndex);
	for (uint32_t i = 0; i < ni; i++) fix_pair(d, &i2[i].srcQueueFamilyIndex, &i2[i].dstQueueFamilyIndex);
	d->CmdWaitEvents(cb, ne, ev, ss, ds, nm, mb, nb, b2, ni, i2);
	TMPFREE(b2); TMPFREE(i2);
}
static void wait2(struct dev *d, PFN_vkCmdWaitEvents2 f, VkCommandBuffer cb, uint32_t ne, const VkEvent *ev, const VkDependencyInfo *di)
{
	VkDependencyInfo o[16]; VkBufferMemoryBarrier2 *b[16]; VkImageMemoryBarrier2 *i[16];
	uint32_t n = ne < 16 ? ne : 16;
	for (uint32_t k = 0; k < n; k++) fix_dep(d, &di[k], &o[k], &b[k], &i[k]);
	f(cb, n, ev, o);
	for (uint32_t k = 0; k < n; k++) { free(b[k]); free(i[k]); }
}
static VKAPI_ATTR void VKAPI_CALL q_CmdWaitEvents2(VkCommandBuffer cb, uint32_t ne, const VkEvent *ev, const VkDependencyInfo *di)
{ struct dev *d = dev_of(cb); wait2(d, d->CmdWaitEvents2, cb, ne, ev, di); }
static VKAPI_ATTR void VKAPI_CALL q_CmdWaitEvents2KHR(VkCommandBuffer cb, uint32_t ne, const VkEvent *ev, const VkDependencyInfo *di)
{ struct dev *d = dev_of(cb); wait2(d, d->CmdWaitEvents2KHR, cb, ne, ev, di); }

// ---- cross-process timeline semaphores (OPAQUE_FD) ----
// SteamVR shares timeline semaphores between its processes as OPAQUE_FD. Turnip on KGSL cannot export semaphores
// that way (only SYNC_FD, binary). Emulation: the "fd" is a memfd holding a 64-bit counter; each process keeps an
// ordinary local timeline semaphore, and one watcher thread per process copies progress both ways every ~1 ms
// (local GPU/host signals -> shared counter; shared counter -> local semaphore via vkSignalSemaphore).
// desk-189: 256 slots overflowed ~20 s after Steam's web UI joined VR ("too many shared semaphores", then vrcompositor
// segfaulted).  Now 4096 slots, loops only up to the highest slot in use, entries of a destroyed device are dropped,
// failures are returned as errors (never a silently unshared semaphore), and the live count is logged every 10 s.
#define MAXSEM 4096
struct ssem { struct dev *d; VkSemaphore sem; volatile uint64_t *ctr; int fd; volatile uint64_t wval, wtime; };
static struct ssem ssems[MAXSEM];
static int g_semhi;   // slots [0, g_semhi) may be in use
static unsigned long g_sem_new, g_sem_imp, g_sem_del, g_sem_fail;
static qmutex g_semlock;
static int g_semthr;
static uint64_t sem_local(struct ssem *e)
{
	uint64_t v = 0;
	PFN_vkGetSemaphoreCounterValue f = e->d->GetSemaphoreCounterValue ? e->d->GetSemaphoreCounterValue : e->d->GetSemaphoreCounterValueKHR;
	if (f) f(e->d->dev, e->sem, &v);
	return v;
}
static void sem_sync(struct ssem *e)
{
	uint64_t l = sem_local(e), sh = __atomic_load_n(e->ctr, __ATOMIC_ACQUIRE);
	while (l > sh) { if (__atomic_compare_exchange_n(e->ctr, &sh, l, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break; }
	if (sh > l) {
		VkSemaphoreSignalInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, 0, e->sem, sh };
		PFN_vkSignalSemaphore f = e->d->SignalSemaphore ? e->d->SignalSemaphore : e->d->SignalSemaphoreKHR;
		if (f) f(e->d->dev, &si);
	}
}
static void ssem_free(struct ssem *e)   // g_semlock held
{
	if (!e || !e->ctr) return;
	munmap((void *)e->ctr, 4096); if (e->fd >= 0) close(e->fd);
	e->ctr = 0; e->sem = 0; e->d = 0; e->fd = -1; e->wval = 0; e->wtime = 0; g_sem_del++;
	while (g_semhi > 0 && !ssems[g_semhi - 1].ctr) g_semhi--;
}
static void *sem_thread(void *arg)
{
	(void)arg; int tick = 0; unsigned long ln = ~0ul, li = ~0ul, ld = ~0ul, lf = ~0ul;
	for (;;) {
		pthread_mutex_lock(&g_semlock);
		int live = 0;
		for (int i = 0; i < g_semhi; i++) if (ssems[i].ctr && ssems[i].d && ssems[i].d->key) { sem_sync(&ssems[i]); live++; }
		unsigned long n = g_sem_new, im = g_sem_imp, de = g_sem_del, fa = g_sem_fail;
		pthread_mutex_unlock(&g_semlock);
		if (++tick >= 10000) {   // ~10 s
			tick = 0;
			if (n != ln || im != li || de != ld || fa != lf)
				qlog("shared semaphores: %d live (slots up to %d of %d); total %lu created, %lu imported, %lu released, %lu failed", live, g_semhi, MAXSEM, n, im, de, fa);
			ln = n; li = im; ld = de; lf = fa;
			{ int late = 0; uint64_t tn = now_ns(); pthread_mutex_lock(&g_semlock);
			  for (int i = 0; i < g_semhi; i++) if (ssems[i].ctr && ssems[i].wval > __atomic_load_n(ssems[i].ctr, __ATOMIC_ACQUIRE) && tn - ssems[i].wtime > 2000000000ull) late++;
			  pthread_mutex_unlock(&g_semlock);
			  if (late) report_sem_waits("semaphore waits unmet for 2 s+"); }
		}
		sleep_ns(1000000);
	}
	return 0;
}
static struct ssem *ssem_find(VkSemaphore sem)
{ for (int i = 0; i < g_semhi; i++) if (ssems[i].ctr && ssems[i].sem == sem) return &ssems[i]; return 0; }
// desk-190: remember the last value each shared semaphore was waited for (submit wait lists), for the stall report
static void note_wait(VkSemaphore sem, uint64_t v)
{
	struct ssem *e = ssem_find(sem);
	if (e && v > e->wval) { e->wval = v; e->wtime = now_ns(); }
}
static void note_waits1(uint32_t n, const VkSubmitInfo *s)
{
	if (!g_semthr) return;
	pthread_mutex_lock(&g_semlock);
	for (uint32_t i = 0; i < n; i++) {
		const VkTimelineSemaphoreSubmitInfo *tl = 0;
		for (const VkBaseInStructure *p = s[i].pNext; p; p = p->pNext) if (p->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) tl = (const void *)p;
		if (tl && tl->pWaitSemaphoreValues) for (uint32_t k = 0; k < s[i].waitSemaphoreCount && k < tl->waitSemaphoreValueCount; k++) note_wait(s[i].pWaitSemaphores[k], tl->pWaitSemaphoreValues[k]);
	}
	pthread_mutex_unlock(&g_semlock);
}
static void note_waits2(uint32_t n, const VkSubmitInfo2 *s)
{
	if (!g_semthr) return;
	pthread_mutex_lock(&g_semlock);
	for (uint32_t i = 0; i < n; i++) for (uint32_t k = 0; k < s[i].waitSemaphoreInfoCount; k++) note_wait(s[i].pWaitSemaphoreInfos[k].semaphore, s[i].pWaitSemaphoreInfos[k].value);
	pthread_mutex_unlock(&g_semlock);
}
// stall report: shared semaphores whose last waited value is still ahead of the shared counter
static void report_sem_waits(const char *tag)
{
	if (!g_semthr) return;
	char b[520]; int o = 0, k = 0, n = 0; uint64_t t = now_ns();
	pthread_mutex_lock(&g_semlock);
	for (int i = 0; i < g_semhi; i++) {
		struct ssem *e = &ssems[i];
		if (!e->ctr || !e->wval) continue;
		uint64_t c = __atomic_load_n(e->ctr, __ATOMIC_ACQUIRE);
		if (e->wval <= c) continue;
		n++;
		if (k < 8 && o < 440) { o += snprintf(b + o, sizeof b - o, " [slot %d waits for %llu, counter %llu, %.1f s ago]", i, (unsigned long long)e->wval, (unsigned long long)c, (double)(t - e->wtime) / 1e9); k++; }
	}
	pthread_mutex_unlock(&g_semlock);
	if (n) { b[o] = 0; qlog("%s: %d shared semaphore(s) waited for a value not reached yet:%s", tag, n, b); }
	else qlog("%s: no shared semaphore is waited for beyond its current value", tag);
}
static void ssem_init(void)
{
	if (!g_semthr) { pthread_mutex_init(&g_semlock, 0); g_semthr = 1; qthread t; pthread_create(&t, 0, sem_thread, 0); }
}
static struct ssem *ssem_add(struct dev *d, VkSemaphore sem, volatile uint64_t *ctr, int fd)   // g_semlock held
{
	struct ssem *e = 0;
	for (int i = 0; i < MAXSEM; i++) if (!ssems[i].ctr) { e = &ssems[i]; if (i >= g_semhi) g_semhi = i + 1; break; }
	if (e) { e->d = d; e->sem = sem; e->ctr = ctr; e->fd = fd; e->wval = 0; e->wtime = 0; }
	else { g_sem_fail++; qlog("too many shared semaphores (%d in use) - refusing", MAXSEM); }
	return e;
}
static void ssem_drop_device(struct dev *d)
{
	if (!g_semthr) return;
	pthread_mutex_lock(&g_semlock);
	int n = 0;
	for (int i = 0; i < g_semhi; i++) if (ssems[i].ctr && ssems[i].d == d) { ssem_free(&ssems[i]); n++; }
	pthread_mutex_unlock(&g_semlock);
	if (n) qlog("device destroyed: released %d shared semaphore(s) it still had", n);
}
// ---- desk-293: BINARY OPAQUE_FD semaphores shared inside one process ----
// Steam's web UI hands each VR panel from OpenGL (zink) to Vulkan (SteamVR's client library) through a binary semaphore
// exported as OPAQUE_FD and imported into GL.  Turnip on KGSL cannot export OPAQUE_FD: the creation failed ("vkCreate-
// Semaphore failed with error -1") and Steam fell back to glFinish() - the CPU stalls until the GPU is done, on every
// panel frame.  (The Steam Frame's GPU driver has real OPAQUE_FD semaphores: DRM syncobjs.)  Emulation, on with
// Q2VR_BINSEM=1: the semaphore is created with SYNC_FD export instead; its "OPAQUE_FD" is a memfd named q2vrbsem holding
// a group number; importing that fd (another VkDevice of the same process, e.g. zink's) joins the group.  After every
// submit that signals a group member, the member's payload is exported as a sync file (which resets it, like a wait)
// and imported as a temporary payload into the other members: the devices see one binary semaphore between them.
// Ordering: zink submits from its own flush thread, so a wait can be submitted before the matching signal was.  Binary
// semaphores pair the n-th wait with the n-th signal, so a submit that waits on a member first waits (CPU, up to 250 ms)
// until the other members have passed on as many signals as that member has waits.
#define MAXB 256
#define MAXBM 4
struct bmem { struct dev *d; VkSemaphore sem; unsigned long sig, wait; };   // signals passed on / waits submitted
struct bgrp { int fd; int nm; struct bmem m[MAXBM]; };
static struct bgrp bgrps[MAXB];
static int g_bhi, g_binit, g_bon = -1;
static qmutex g_blk;
static unsigned long g_bnew, g_bimp, g_bsig, g_bfail, g_bwaitlate, g_bwaitto;
static int bsem_on(void) { if (g_bon < 0) g_bon = envi("Q2VR_BINSEM", 0) ? 1 : 0; return g_bon; }
static void bsem_init(void) { if (!g_binit) { pthread_mutex_init(&g_blk, 0); g_binit = 1; } }
static struct bgrp *bsem_find(VkSemaphore sem, int *mi)   // g_blk held
{
	for (int i = 0; i < g_bhi; i++) for (int k = 0; k < bgrps[i].nm; k++) if (bgrps[i].m[k].sem == sem) { if (mi) *mi = k; return &bgrps[i]; }
	return 0;
}
static void bsem_remove(VkSemaphore sem)   // g_blk held
{
	int k; struct bgrp *g = bsem_find(sem, &k);
	if (!g) return;
	g->m[k] = g->m[--g->nm];
	if (!g->nm) { if (g->fd >= 0) close(g->fd); g->fd = -1; while (g_bhi > 0 && !bgrps[g_bhi - 1].nm) g_bhi--; }
}
static void bsem_drop_device(struct dev *d)
{
	if (!g_binit) return;
	pthread_mutex_lock(&g_blk);
	for (int i = 0; i < g_bhi; i++) for (int k = 0; k < bgrps[i].nm; ) { if (bgrps[i].m[k].d == d) bsem_remove(bgrps[i].m[k].sem); else k++; }
	pthread_mutex_unlock(&g_blk);
}
static long bsem_pread(int fd, void *b, size_t n)
{
#ifdef __aarch64__
	return syscall(67, fd, b, n, 0L);
#else
	return syscall(17, fd, b, n, 0L);
#endif
}
// after a submit: pass the payload of a signalled group member on to the group's other members
static void bsem_signalled(VkSemaphore sem)   // g_blk held
{
	int k; struct bgrp *g = bsem_find(sem, &k);
	if (!g || g->nm < 2) return;
	struct bmem *src = &g->m[k]; int fd = -1;
	VkSemaphoreGetFdInfoKHR gi = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, 0, sem, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT };
	if (!src->d->GetSemaphoreFdKHR || src->d->GetSemaphoreFdKHR(src->d->dev, &gi, &fd) != VK_SUCCESS) { g_bfail++; return; }
	for (int j = 0; j < g->nm; j++) {
		if (j == k || !g->m[j].d->ImportSemaphoreFdKHR) continue;
		int f2 = fd >= 0 ? dup(fd) : -1;
		VkImportSemaphoreFdInfoKHR ii = { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR, 0, g->m[j].sem, VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
		                                  VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, f2 };
		if (g->m[j].d->ImportSemaphoreFdKHR(g->m[j].d->dev, &ii) != VK_SUCCESS) { g_bfail++; if (f2 >= 0) close(f2); }
	}
	if (fd >= 0) close(fd);
	src->sig++; g_bsig++;
	{   // desk-294: panel hand-overs per second (= Steam's VR panel frame rate, all panels together), every ~10 s
		static uint64_t t0; static unsigned long s0, l0, o0; uint64_t t = now_ns();
		if (!t0) { t0 = t; s0 = g_bsig; l0 = g_bwaitlate; o0 = g_bwaitto; }
		else if (t - t0 >= 10000000000ull) {
			qlog("binary sync: %lu panel hand-overs in %.1f s (%.1f/s), %lu waits came before their signal, %lu timed out; groups %d",
			     g_bsig - s0, (t - t0) / 1e9, (g_bsig - s0) * 1e9 / (double)(t - t0), g_bwaitlate - l0, g_bwaitto - o0, g_bhi);
			t0 = t; s0 = g_bsig; l0 = g_bwaitlate; o0 = g_bwaitto;
		}
	}
	if ((g_bsig & (g_bsig - 1)) == 0) qlog("binary shared semaphores: %lu signals passed on (%lu groups made, %lu joined, %lu failures, %lu waits had to wait for their signal, %lu timed out)", g_bsig, g_bnew, g_bimp, g_bfail, g_bwaitlate, g_bwaitto);
}
static void bsem_after1(uint32_t n, const VkSubmitInfo *s)
{
	if (!g_binit || !g_bhi) return;
	pthread_mutex_lock(&g_blk);
	for (uint32_t i = 0; i < n; i++) for (uint32_t k = 0; k < s[i].signalSemaphoreCount; k++) bsem_signalled(s[i].pSignalSemaphores[k]);
	pthread_mutex_unlock(&g_blk);
}
static void bsem_after2(uint32_t n, const VkSubmitInfo2 *s)
{
	if (!g_binit || !g_bhi) return;
	pthread_mutex_lock(&g_blk);
	for (uint32_t i = 0; i < n; i++) for (uint32_t k = 0; k < s[i].signalSemaphoreInfoCount; k++) bsem_signalled(s[i].pSignalSemaphoreInfos[k].semaphore);
	pthread_mutex_unlock(&g_blk);
}
// before a submit: each wait on a group member needs one signal passed on from the other members
static void bsem_need(VkSemaphore sem)   // g_blk held on entry and exit
{
	int k; struct bgrp *g = bsem_find(sem, &k);
	if (!g || g->nm < 2) return;
	unsigned long need = g->m[k].wait + 1;
	for (int t = 0; ; t++) {
		g = bsem_find(sem, &k); if (!g) return;
		unsigned long have = 0; for (int j = 0; j < g->nm; j++) if (j != k) have += g->m[j].sig;
		if (have >= need) break;
		if (t == 0) g_bwaitlate++;
		if (t >= 1250) { g_bwaitto++; qlog("binary shared semaphore: wait #%lu still has no signal after 250 ms - submitting anyway (%lu late, %lu timed out)", need, g_bwaitlate, g_bwaitto); break; }
		pthread_mutex_unlock(&g_blk); sleep_ns(200000); pthread_mutex_lock(&g_blk);
	}
	g = bsem_find(sem, &k); if (g) g->m[k].wait = need;
}
static void bsem_before1(uint32_t n, const VkSubmitInfo *s)
{
	if (!g_binit || !g_bhi) return;
	pthread_mutex_lock(&g_blk);
	for (uint32_t i = 0; i < n; i++) for (uint32_t k = 0; k < s[i].waitSemaphoreCount; k++) bsem_need(s[i].pWaitSemaphores[k]);
	pthread_mutex_unlock(&g_blk);
}
static void bsem_before2(uint32_t n, const VkSubmitInfo2 *s)
{
	if (!g_binit || !g_bhi) return;
	pthread_mutex_lock(&g_blk);
	for (uint32_t i = 0; i < n; i++) for (uint32_t k = 0; k < s[i].waitSemaphoreInfoCount; k++) bsem_need(s[i].pWaitSemaphoreInfos[k].semaphore);
	pthread_mutex_unlock(&g_blk);
}
// a binary semaphore that asked for OPAQUE_FD export: create it SYNC_FD-exportable and start a group
static VkResult bsem_create(struct dev *d, VkDevice dv, const VkSemaphoreCreateInfo *ci, const VkAllocationCallbacks *a, VkSemaphore *out)
{
	VkSemaphoreCreateInfo c = *ci; VkExportSemaphoreCreateInfo ex = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, 0, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT };
	c.pNext = &ex;   // a binary semaphore's chain: only the export request matters here
	VkResult r = d->CreateSemaphore(dv, &c, a, out);
	if (r) return r;
	bsem_init();
	pthread_mutex_lock(&g_blk);
	int gi = -1; for (int i = 0; i < MAXB; i++) if (!bgrps[i].nm) { gi = i; break; }
	int fd = gi >= 0 ? memfd_create("q2vrbsem", 1) : -1;
	if (fd >= 0) { long v = 0x51320000L | gi; if (write(fd, &v, 8) != 8) { close(fd); fd = -1; } }
	if (fd < 0) { pthread_mutex_unlock(&g_blk); d->DestroySemaphore(dv, *out, a); *out = 0; g_bfail++; qlog("binary shared semaphore: no group slot / memfd"); return VK_ERROR_OUT_OF_HOST_MEMORY; }
	bgrps[gi].fd = fd; bgrps[gi].nm = 1; bgrps[gi].m[0].d = d; bgrps[gi].m[0].sem = *out; bgrps[gi].m[0].sig = bgrps[gi].m[0].wait = 0;
	if (gi >= g_bhi) g_bhi = gi + 1;
	g_bnew++;
	pthread_mutex_unlock(&g_blk);
	if (g_bnew == 1) qlog("binary shared semaphores on (Q2VR_BINSEM): OPAQUE_FD export emulated inside this process");
	return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL q_CreateSemaphore(VkDevice dv, const VkSemaphoreCreateInfo *ci, const VkAllocationCallbacks *a, VkSemaphore *out)
{
	struct dev *d = dev_of(dv);
	const VkBaseInStructure *p = ci->pNext; int exp = 0, timeline = 0; uint64_t init = 0;
	for (; p; p = p->pNext) {
		if (p->sType == VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO && (((const VkExportSemaphoreCreateInfo *)p)->handleTypes & VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT)) exp = 1;
		if (p->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO && ((const VkSemaphoreTypeCreateInfo *)p)->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) {
			timeline = 1; init = ((const VkSemaphoreTypeCreateInfo *)p)->initialValue;
		}
	}
	if (!exp) return d->CreateSemaphore(dv, ci, a, out);
	// rebuild the chain without the export struct
	VkSemaphoreCreateInfo c = *ci; VkSemaphoreTypeCreateInfo ty = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, 0, VK_SEMAPHORE_TYPE_TIMELINE, init };
	c.pNext = timeline ? &ty : 0;
	if (!timeline && bsem_on()) return bsem_create(d, dv, ci, a, out);   // desk-293
	if (!timeline) {   // only timeline semaphores are emulated; never hand out a semaphore that only looks shareable
		qlog("create: BINARY semaphore with OPAQUE_FD export requested - not supported, returning an error");
		ssem_init(); pthread_mutex_lock(&g_semlock); g_sem_fail++; pthread_mutex_unlock(&g_semlock);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	VkResult r = d->CreateSemaphore(dv, &c, a, out);
	if (r) return r;
	int fd = memfd_create("q2vrsem", 1 /* MFD_CLOEXEC */);
	if (fd < 0 || ftruncate(fd, 4096) != 0) {
		qlog("create: memfd for a shared timeline semaphore failed - returning an error");
		if (fd >= 0) close(fd);
		d->DestroySemaphore(dv, *out, a); *out = 0; return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	void *m = mmap(0, 4096, 3, 1, fd, 0);
	if (m == (void *)-1) { qlog("create: mmap of a shared timeline semaphore failed - returning an error"); close(fd); d->DestroySemaphore(dv, *out, a); *out = 0; return VK_ERROR_OUT_OF_HOST_MEMORY; }
	*(volatile uint64_t *)m = init;
	ssem_init();
	pthread_mutex_lock(&g_semlock);
	struct ssem *e = ssem_add(d, *out, m, fd);
	if (e) g_sem_new++;
	pthread_mutex_unlock(&g_semlock);
	if (!e) { munmap(m, 4096); close(fd); d->DestroySemaphore(dv, *out, a); *out = 0; return VK_ERROR_OUT_OF_HOST_MEMORY; }
	return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL q_DestroySemaphore(VkDevice dv, VkSemaphore sem, const VkAllocationCallbacks *a)
{
	struct dev *d = dev_of(dv);
	if (sem && g_semthr) {
		pthread_mutex_lock(&g_semlock);
		ssem_free(ssem_find(sem));
		pthread_mutex_unlock(&g_semlock);
	}
	if (sem && g_binit) { pthread_mutex_lock(&g_blk); bsem_remove(sem); pthread_mutex_unlock(&g_blk); }   // desk-293
	d->DestroySemaphore(dv, sem, a);
}
static VKAPI_ATTR VkResult VKAPI_CALL q_GetSemaphoreFdKHR(VkDevice dv, const VkSemaphoreGetFdInfoKHR *gi, int *fd)
{
	struct dev *d = dev_of(dv);
	if (gi->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT && g_semthr) {
		pthread_mutex_lock(&g_semlock); struct ssem *e = ssem_find(gi->semaphore); int f = e && e->fd >= 0 ? dup(e->fd) : -1;
		if (e) sem_sync(e);
		pthread_mutex_unlock(&g_semlock);
		if (f >= 0) { *fd = f; return VK_SUCCESS; }
	}
	if (gi->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT && g_binit) {   // desk-293
		pthread_mutex_lock(&g_blk); struct bgrp *g = bsem_find(gi->semaphore, 0); int f = g && g->fd >= 0 ? dup(g->fd) : -1; pthread_mutex_unlock(&g_blk);
		if (f >= 0) { *fd = f; return VK_SUCCESS; }
	}
	return d->GetSemaphoreFdKHR ? d->GetSemaphoreFdKHR(dv, gi, fd) : VK_ERROR_INVALID_EXTERNAL_HANDLE;
}
static VKAPI_ATTR VkResult VKAPI_CALL q_ImportSemaphoreFdKHR(VkDevice dv, const VkImportSemaphoreFdInfoKHR *ii)
{
	struct dev *d = dev_of(dv);
	if (ii->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT) {
		char path[64], target[128]; snprintf(path, sizeof path, "/proc/self/fd/%d", ii->fd);
		long n = readlink(path, target, sizeof target - 1);
		if (n > 0) { target[n] = 0; }
		if (n > 0 && strstr(target, "q2vrsem")) {
			void *m = mmap(0, 4096, 3, 1, ii->fd, 0);
			if (m == (void *)-1) { qlog("import: mmap of a shared timeline semaphore failed"); return VK_ERROR_INVALID_EXTERNAL_HANDLE; }
			ssem_init();
			pthread_mutex_lock(&g_semlock);
			ssem_free(ssem_find(ii->semaphore));   // importing replaces any earlier sharing of this semaphore
			struct ssem *e = ssem_add(d, ii->semaphore, m, ii->fd);   // on success the fd is ours (import transfers ownership)
			if (e) { g_sem_imp++; sem_sync(e); }
			pthread_mutex_unlock(&g_semlock);
			if (!e) { munmap(m, 4096); return VK_ERROR_OUT_OF_HOST_MEMORY; }   // the caller keeps the fd on failure
			return VK_SUCCESS;
		}
		if (n > 0 && strstr(target, "q2vrbsem")) {   // desk-293: join a binary group of this process
			long v = 0; bsem_init();
			if (bsem_pread(ii->fd, &v, 8) != 8 || (v & ~0xffffL) != 0x51320000L) { qlog("import: bad q2vrbsem fd"); return VK_ERROR_INVALID_EXTERNAL_HANDLE; }
			int gi = (int)(v & 0xffff);
			pthread_mutex_lock(&g_blk);
			bsem_remove(ii->semaphore);
			struct bgrp *g = gi < MAXB ? &bgrps[gi] : 0; int ok = g && g->nm > 0 && g->nm < MAXBM;
			if (ok) { g->m[g->nm].d = d; g->m[g->nm].sem = ii->semaphore; g->m[g->nm].sig = g->m[g->nm].wait = 0; g->nm++; g_bimp++; }
			pthread_mutex_unlock(&g_blk);
			if (!ok) { g_bfail++; qlog("import: q2vrbsem group %d gone or full", gi); return VK_ERROR_INVALID_EXTERNAL_HANDLE; }
			close(ii->fd);   // a successful import owns the fd
			if (g_bimp == 1) qlog("binary shared semaphore joined by another device of this process (GL interop)");
			return VK_SUCCESS;
		}
		qlog("OPAQUE_FD semaphore import of a non-emulated fd (%s)", n > 0 ? target : "?");
	}
	return d->ImportSemaphoreFdKHR ? d->ImportSemaphoreFdKHR(dv, ii) : VK_ERROR_INVALID_EXTERNAL_HANDLE;
}
static VkResult counter(struct dev *d, PFN_vkGetSemaphoreCounterValue f, VkDevice dv, VkSemaphore sem, uint64_t *v)
{
	VkResult r = f(dv, sem, v);
	if (g_semthr) {
		pthread_mutex_lock(&g_semlock); struct ssem *e = ssem_find(sem);
		if (e) { uint64_t sh = __atomic_load_n(e->ctr, __ATOMIC_ACQUIRE); if (sh > *v) *v = sh; }
		pthread_mutex_unlock(&g_semlock);
	}
	(void)d; return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL q_GetSemaphoreCounterValue(VkDevice dv, VkSemaphore sem, uint64_t *v)
{ struct dev *d = dev_of(dv); return counter(d, d->GetSemaphoreCounterValue, dv, sem, v); }
static VKAPI_ATTR VkResult VKAPI_CALL q_GetSemaphoreCounterValueKHR(VkDevice dv, VkSemaphore sem, uint64_t *v)
{ struct dev *d = dev_of(dv); return counter(d, d->GetSemaphoreCounterValueKHR, dv, sem, v); }
static VkResult hsignal(struct dev *d, PFN_vkSignalSemaphore f, VkDevice dv, const VkSemaphoreSignalInfo *si)
{
	VkResult r = f(dv, si);
	if (g_semthr) { pthread_mutex_lock(&g_semlock); struct ssem *e = ssem_find(si->semaphore); if (e) sem_sync(e); pthread_mutex_unlock(&g_semlock); }
	(void)d; return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL q_SignalSemaphore(VkDevice dv, const VkSemaphoreSignalInfo *si)
{ struct dev *d = dev_of(dv); return hsignal(d, d->SignalSemaphore, dv, si); }
static VKAPI_ATTR VkResult VKAPI_CALL q_SignalSemaphoreKHR(VkDevice dv, const VkSemaphoreSignalInfo *si)
{ struct dev *d = dev_of(dv); return hsignal(d, d->SignalSemaphoreKHR, dv, si); }
static void fix_extsem(const VkPhysicalDeviceExternalSemaphoreInfo *i, VkExternalSemaphoreProperties *p)
{
	// desk-189: only TIMELINE semaphores are emulated, so only a query that asks about a timeline semaphore gets
	// "exportable + importable" (no VkSemaphoreTypeCreateInfo in the chain means binary).  For now binary queries keep
	// the old answer (Q2VR_SEM_BINARY_ADV, default 1) because SteamVR's clients got this far with it; the log says which
	// kind of query each process makes, and creating a binary exportable semaphore now fails loudly instead of silently.
	int tl = 0;
	for (const VkBaseInStructure *q = i->pNext; q; q = q->pNext)
		if (q->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO && ((const VkSemaphoreTypeCreateInfo *)q)->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) tl = 1;
	if (i->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT) {
		static int logged_b, logged_t;
		if (!tl && !logged_b) { logged_b = 1; qlog("external-semaphore query: OPAQUE_FD for a BINARY semaphore -> %s", envi("Q2VR_SEM_BINARY_ADV", 1) ? "supported (Q2VR_SEM_BINARY_ADV=1)" : "left to the driver (not emulated)"); }
		if (tl && !logged_t) { logged_t = 1; qlog("external-semaphore query: OPAQUE_FD for a TIMELINE semaphore -> supported (emulated)"); }
		if (!tl && !envi("Q2VR_SEM_BINARY_ADV", 1)) return;
		p->exportFromImportedHandleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
		p->compatibleHandleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
		p->externalSemaphoreFeatures = VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
	}
}
static VKAPI_ATTR void VKAPI_CALL q_GetPhysicalDeviceExternalSemaphoreProperties(VkPhysicalDevice pd, const VkPhysicalDeviceExternalSemaphoreInfo *i, VkExternalSemaphoreProperties *p)
{ struct inst *in = inst_of(pd); (in->ExtSemProps ? in->ExtSemProps : in->ExtSemPropsKHR)(pd, i, p); fix_extsem(i, p); }
static VKAPI_ATTR void VKAPI_CALL q_GetPhysicalDeviceExternalSemaphorePropertiesKHR(VkPhysicalDevice pd, const VkPhysicalDeviceExternalSemaphoreInfo *i, VkExternalSemaphoreProperties *p)
{ q_GetPhysicalDeviceExternalSemaphoreProperties(pd, i, p); }

// ---- present-related queries on our swapchains (vrcompositor enables present_id/present_wait) ----
static VKAPI_ATTR VkResult VKAPI_CALL q_WaitForPresentKHR(VkDevice dv, VkSwapchainKHR s, uint64_t id, uint64_t timeout)
{
	struct dev *d = dev_of(dv); struct swapchain *c = sc_of(s);
	if (!c) return d->WaitForPresentKHR ? d->WaitForPresentKHR(dv, s, id, timeout) : VK_SUCCESS;
	uint64_t deadline = timeout == UINT64_MAX ? UINT64_MAX : now_ns() + timeout;
	pthread_mutex_lock(&c->lock);
	if (id > c->want_id) c->want_id = id;
	while (c->done_id < id && !c->dead) {
		if (deadline != UINT64_MAX && now_ns() >= deadline) { c->wait_timeouts++; pthread_mutex_unlock(&c->lock); return VK_TIMEOUT; }
		struct qtimespec ts; clock_gettime(Q_CLOCK_REALTIME, &ts); ts.tv_nsec += 5000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
		pthread_cond_timedwait(&c->cond, &c->lock, &ts);
	}
	pthread_mutex_unlock(&c->lock);
	return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL q_GetSwapchainStatusKHR(VkDevice dv, VkSwapchainKHR s)
{ struct dev *d = dev_of(dv); if (sc_of(s)) return VK_SUCCESS; return d->GetSwapchainStatusKHR ? d->GetSwapchainStatusKHR(dv, s) : VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL q_GetRefreshCycleDurationGOOGLE(VkDevice dv, VkSwapchainKHR s, VkRefreshCycleDurationGOOGLE *r)
{
	struct dev *d = dev_of(dv);
	if (sc_of(s) || !d->GetRefreshCycleDurationGOOGLE) { r->refreshDuration = 1000000000ull / DHZ; return VK_SUCCESS; }
	return d->GetRefreshCycleDurationGOOGLE(dv, s, r);
}
static VKAPI_ATTR VkResult VKAPI_CALL q_GetPastPresentationTimingGOOGLE(VkDevice dv, VkSwapchainKHR s, uint32_t *n, VkPastPresentationTimingGOOGLE *t)
{
	struct dev *d = dev_of(dv);
	if (sc_of(s) || !d->GetPastPresentationTimingGOOGLE) { (void)t; *n = 0; return VK_SUCCESS; }
	return d->GetPastPresentationTimingGOOGLE(dv, s, n, t);
}
static VKAPI_ATTR VkResult VKAPI_CALL q_ReleaseSwapchainImagesEXT(VkDevice dv, const VkReleaseSwapchainImagesInfoEXT *ri)
{
	struct dev *d = dev_of(dv); struct swapchain *c = sc_of(ri->swapchain);
	if (!c) return d->ReleaseSwapchainImagesEXT ? d->ReleaseSwapchainImagesEXT(dv, ri) : VK_SUCCESS;
	pthread_mutex_lock(&c->lock);
	for (uint32_t k = 0; k < ri->imageIndexCount; k++) if (ri->pImageIndices[k] < c->n && c->state[ri->pImageIndices[k]] == 1) c->state[ri->pImageIndices[k]] = 0;
	pthread_cond_broadcast(&c->cond); pthread_mutex_unlock(&c->lock);
	return VK_SUCCESS;
}
// functions the GPU driver does not have (e.g. VK_EXT_debug_marker): harmless no-ops instead of NULL pointers
static long q_noop(void) { return 0; }

static VKAPI_ATTR VkResult VKAPI_CALL q_WaitForFences(VkDevice dv, uint32_t n, const VkFence *f, VkBool32 all, uint64_t to)
{
	struct dev *d = dev_of(dv);
	int ev = 0;
	for (uint32_t i = 0; i < n && !ev; i++) for (int j = 0; j < 8; j++) if (g_evf[j] && g_evf[j] == (uint64_t)f[i]) { ev = 1; break; }
	if (!ev) return d->WaitForFences(dv, n, f, all, to);
	uint64_t t0 = now_ns();
	VkResult r;
	int own = n > 0 && envi("Q2VR_EVWAIT", 1);
	for (uint32_t i = 0; i < n && own; i++) if (ev_slot((uint64_t)f[i]) < 0) own = 0;   // only display-event fences
	if (own) {   // desk-177: wait on our own "signalled" marks (see g_evt)
		uint64_t dl = to == UINT64_MAX ? UINT64_MAX : t0 + to;
		pthread_mutex_lock(&g_evm);
		for (;;) {
			uint32_t got = 0;
			for (uint32_t i = 0; i < n; i++) { int j = ev_slot((uint64_t)f[i]); if (j >= 0 && g_evt[j].sig) got++; }
			if (all ? got == n : got > 0) { r = VK_SUCCESS; break; }
			uint64_t nw = now_ns();
			if (nw >= dl) { r = VK_TIMEOUT; break; }
			struct qtimespec ts; clock_gettime(Q_CLOCK_REALTIME, &ts); ts.tv_nsec += 2000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
			pthread_cond_timedwait(&g_evc, &g_evm, &ts);
		}
		pthread_mutex_unlock(&g_evm);
	} else r = d->WaitForFences(dv, n, f, all, to);
	uint64_t ms = (now_ns() - t0) / 1000000ull;
	g_evw_n++; if (r == VK_TIMEOUT) g_evw_to++;
	g_evw_hist[ms < 2 ? 0 : ms < 8 ? 1 : ms < 16 ? 2 : ms < 30 ? 3 : ms < 48 ? 4 : 5]++;
	return r;
}
// desk-192: a display-event fence leaves our lists before it is destroyed (see vblank_thread)
static VKAPI_ATTR void VKAPI_CALL q_DestroyFence(VkDevice dv, VkFence f, const VkAllocationCallbacks *a)
{
	struct dev *d = dev_of(dv);
	if (!f) { d->DestroyFence(dv, f, a); return; }
	pthread_mutex_lock(&d->evlock);
	for (int k = 0; k < d->npending; k++) if (d->pending[k] == f) { d->pending[k] = d->pending[--d->npending]; k--; }
	pthread_mutex_lock(&g_evm);
	int j = ev_slot((uint64_t)f); if (j >= 0) { g_evt[j].f = 0; g_evt[j].sig = 0; }
	for (int k = 0; k < 8; k++) if (g_evf[k] == (uint64_t)f) g_evf[k] = 0;
	pthread_mutex_unlock(&g_evm);
	d->DestroyFence(dv, f, a);
	pthread_mutex_unlock(&d->evlock);
}
// VK_EXT_display_control
static VKAPI_ATTR VkResult VKAPI_CALL q_DisplayPowerControlEXT(VkDevice dv, VkDisplayKHR disp, const VkDisplayPowerInfoEXT *p)
{ (void)dv; (void)disp; (void)p; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL q_RegisterDeviceEventEXT(VkDevice dv, const VkDeviceEventInfoEXT *e, const VkAllocationCallbacks *a, VkFence *f)
{
	(void)e; struct dev *d = dev_of(dv);
	VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0, 0 };
	return d->CreateFence(dv, &fi, a, f);   // hotplug never happens
}
static VKAPI_ATTR VkResult VKAPI_CALL q_RegisterDisplayEventEXT(VkDevice dv, VkDisplayKHR disp, const VkDisplayEventInfoEXT *e,
	const VkAllocationCallbacks *a, VkFence *f)
{
	(void)disp; (void)e; struct dev *d = dev_of(dv);
	VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0, 0 };
	VkResult r = d->CreateFence(dv, &fi, a, f);
	if (r) return r;
	start_vblank();
	pthread_mutex_lock(&d->evlock);
	if (d->npending < 32) d->pending[d->npending++] = *f;
	g_ev_reg++; g_evf[g_evf_i++ & 7] = (uint64_t)*f;
	pthread_mutex_lock(&g_evm); { int j = ev_slot((uint64_t)*f); if (j < 0) j = (int)(g_evt_i++ & 15); g_evt[j].f = (uint64_t)*f; g_evt[j].sig = 0; } pthread_mutex_unlock(&g_evm);
	pthread_mutex_unlock(&d->evlock);
	return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL q_GetSwapchainCounterEXT(VkDevice dv, VkSwapchainKHR s, VkSurfaceCounterFlagBitsEXT b, uint64_t *v)
{
	(void)dv; (void)s; start_vblank();
	if ((uint32_t)b == 0x7FFFFFFDu) {   // Valve: { vblank sequence, vblank CLOCK_MONOTONIC ns } (see g_flip_seq)
		// desk-176: same clock as the VBLANK counter and the display-event fences (the ticks, on the real flip grid)
		static int said; v[0] = __atomic_load_n(&g_tick, __ATOMIC_ACQUIRE); v[1] = g_tick_ns;
		if (!said) { said = 1; qlog("vblank timestamps: compositor reads them via counter 0x7FFFFFFD (answered from the vblank ticks)"); }
		return VK_SUCCESS;
	}
	*v = __atomic_load_n(&g_tick, __ATOMIC_ACQUIRE); return VK_SUCCESS;
}

// ---- create / destroy ----
static VkLayerInstanceCreateInfo *inst_chain(const VkInstanceCreateInfo *ci, VkLayerFunction f)
{
	VkLayerInstanceCreateInfo *c = (VkLayerInstanceCreateInfo *)ci->pNext;
	while (c && !(c->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && c->function == f)) c = (VkLayerInstanceCreateInfo *)c->pNext;
	return c;
}
static VkLayerDeviceCreateInfo *dev_chain(const VkDeviceCreateInfo *ci, VkLayerFunction f)
{
	VkLayerDeviceCreateInfo *c = (VkLayerDeviceCreateInfo *)ci->pNext;
	while (c && !(c->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && c->function == f)) c = (VkLayerDeviceCreateInfo *)c->pNext;
	return c;
}

static VKAPI_ATTR VkResult VKAPI_CALL q_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out)
{
	ginit();
	VkLayerInstanceCreateInfo *link = inst_chain(ci, VK_LAYER_LINK_INFO);
	if (!link) return VK_ERROR_INITIALIZATION_FAILED;
	PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	link->u.pLayerInfo = link->u.pLayerInfo->pNext;
	PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(0, "vkCreateInstance");
	// strip the display extensions (Turnip/KGSL refuses its GPU when VK_KHR_display is enabled)
	const char *exts[64]; uint32_t n = 0; int stripped = 0;
	for (uint32_t i = 0; i < ci->enabledExtensionCount && n < 64; i++) {
		int drop = 0;
		for (int k = 0; k < DISPLAY_EXTS_N; k++) if (!strcmp(ci->ppEnabledExtensionNames[i], display_exts[k])) drop = 1;
		if (drop) stripped++; else exts[n++] = ci->ppEnabledExtensionNames[i];
	}
	VkInstanceCreateInfo c2 = *ci; c2.enabledExtensionCount = n; c2.ppEnabledExtensionNames = exts;
	VkResult r = create(&c2, a, out);
	if (r) { qlog("vkCreateInstance failed below the layer: %d", (int)r); return r; }
	pthread_mutex_lock(&g_lock);
	struct inst *in = 0;
	for (int i = 0; i < MAXI; i++) if (!insts[i].key) { in = &insts[i]; break; }
	if (!in) in = &insts[MAXI - 1];
	memset(in, 0, sizeof *in);
	in->key = dkey(*out); in->inst = *out; in->gipa = gipa;
#define G(field, name) in->field = (void *)gipa(*out, name)
	G(DestroyInstance, "vkDestroyInstance"); G(GetPhysicalDeviceMemoryProperties, "vkGetPhysicalDeviceMemoryProperties");
	G(SurfaceSupport, "vkGetPhysicalDeviceSurfaceSupportKHR"); G(SurfaceCaps, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
	G(SurfaceFormats, "vkGetPhysicalDeviceSurfaceFormatsKHR"); G(SurfaceModes, "vkGetPhysicalDeviceSurfacePresentModesKHR");
	G(SurfaceCaps2, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"); G(SurfaceFormats2, "vkGetPhysicalDeviceSurfaceFormats2KHR");
	G(DestroySurface, "vkDestroySurfaceKHR"); G(CreateDevice, "vkCreateDevice");
	G(EnumerateDeviceExtensionProperties, "vkEnumerateDeviceExtensionProperties");
	G(QFP, "vkGetPhysicalDeviceQueueFamilyProperties"); G(QFP2, "vkGetPhysicalDeviceQueueFamilyProperties2");
	G(QFP2KHR, "vkGetPhysicalDeviceQueueFamilyProperties2KHR");
	G(ExtSemProps, "vkGetPhysicalDeviceExternalSemaphoreProperties"); G(ExtSemPropsKHR, "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR");
#undef G
	pthread_mutex_unlock(&g_lock);
	DW = (uint32_t)envi("Q2VR_DISPLAY_W", 1920); DH = (uint32_t)envi("Q2VR_DISPLAY_H", 3664); DHZ = (uint32_t)envi("Q2VR_HZ", 72);
	qlog("instance created (%d display extension(s) handled by the layer, display %ux%u @ %u Hz)", stripped, DW, DH, DHZ);
	return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL q_DestroyInstance(VkInstance i, const VkAllocationCallbacks *a)
{
	struct inst *in = inst_of(i);
	if (!in) return;
	PFN_vkDestroyInstance f = in->DestroyInstance;
	pthread_mutex_lock(&g_lock); in->key = 0; pthread_mutex_unlock(&g_lock);
	f(i, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL q_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *out)
{
	struct inst *in = inst_of(pd);
	VkLayerDeviceCreateInfo *link = dev_chain(ci, VK_LAYER_LINK_INFO);
	VkLayerDeviceCreateInfo *ld = dev_chain(ci, VK_LOADER_DATA_CALLBACK);
	if (!link || !in) return VK_ERROR_INITIALIZATION_FAILED;
	PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
	link->u.pLayerInfo = link->u.pLayerInfo->pNext;
	PFN_vkCreateDevice create = (PFN_vkCreateDevice)gipa(in->inst, "vkCreateDevice");
	// clamp queue requests to what the GPU really has (we advertised more)
	uint32_t real[16] = { 0 }, nf = 0;
	VkQueueFamilyProperties qf[16];
	in->QFP(pd, &nf, 0); if (nf > 16) nf = 16;
	in->QFP(pd, &nf, qf);
	for (uint32_t i = 0; i < nf; i++) real[i] = qf[i].queueCount;
	VkDeviceQueueCreateInfo qci[16];
	uint32_t nq = ci->queueCreateInfoCount < 16 ? ci->queueCreateInfoCount : 16;
	int clamped = 0;
	fam_learn(in, pd);
	uint32_t m = 0;
	for (uint32_t i = 0; i < nq; i++) {
		VkDeviceQueueCreateInfo e = ci->pQueueCreateInfos[i];
		uint32_t f = mapf_in(in, e.queueFamilyIndex);
		if (f != e.queueFamilyIndex) clamped = 1;
		e.queueFamilyIndex = f;
		int dup = 0;
		for (uint32_t j = 0; j < m; j++) if (qci[j].queueFamilyIndex == f) { dup = 1; clamped = 1; }
		if (dup) continue;
		if (f < 16 && real[f] && e.queueCount > real[f]) { e.queueCount = real[f]; clamped = 1; }
		qci[m++] = e;
	}
	VkDeviceCreateInfo c2 = *ci; c2.pQueueCreateInfos = qci; c2.queueCreateInfoCount = m;
	// GPU-direct frames need VK_EXT_external_memory_host (+ external_memory); zero-copy into the display's buffers needs
	// VK_KHR_external_memory_fd + VK_EXT_external_memory_dma_buf.  Add what the GPU has and the app did not enable.
	const char *dexts[72]; int hostimp = 0, dmabuf = 0, added = 0, fencefd = 0;
	if (ci->enabledExtensionCount < 64) {
		uint32_t ne = 0; in->EnumerateDeviceExtensionProperties(pd, 0, &ne, 0);
		VkExtensionProperties *ep = ne ? calloc(ne, sizeof *ep) : 0;
		int has_h = 0, has_m = 0, has_fd = 0, has_db = 0, has_f = 0, has_ffd = 0;
		if (ep && !in->EnumerateDeviceExtensionProperties(pd, 0, &ne, ep))
			for (uint32_t i = 0; i < ne; i++) { const char *x = ep[i].extensionName;
				if (!strcmp(x, "VK_EXT_external_memory_host")) has_h = 1; if (!strcmp(x, "VK_KHR_external_memory")) has_m = 1;
				if (!strcmp(x, "VK_KHR_external_memory_fd")) has_fd = 1; if (!strcmp(x, "VK_EXT_external_memory_dma_buf")) has_db = 1;
				if (!strcmp(x, "VK_KHR_external_fence")) has_f = 1; if (!strcmp(x, "VK_KHR_external_fence_fd")) has_ffd = 1; }
		free(ep);
		if (!envi("Q2VR_HOSTIMPORT", 1)) has_h = 0;
		if (!envi("Q2VR_ZEROCOPY", 1)) has_db = 0;
		if (!has_h) qlog("GPU-direct: the GPU driver has no VK_EXT_external_memory_host - frames use the CPU copy (unless zero-copy works)");
		if (!envi("Q2VR_CPUFENCE", 1)) has_ffd = 0;
		uint32_t k = 0; int seen_h = 0, seen_m = 0, seen_fd = 0, seen_db = 0, seen_f = 0, seen_ffd = 0;
		for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) { const char *x = ci->ppEnabledExtensionNames[i]; dexts[k++] = x;
			if (!strcmp(x, "VK_EXT_external_memory_host")) seen_h = 1; if (!strcmp(x, "VK_KHR_external_memory")) seen_m = 1;
			if (!strcmp(x, "VK_KHR_external_memory_fd")) seen_fd = 1; if (!strcmp(x, "VK_EXT_external_memory_dma_buf")) seen_db = 1;
			if (!strcmp(x, "VK_KHR_external_fence")) seen_f = 1; if (!strcmp(x, "VK_KHR_external_fence_fd")) seen_ffd = 1; }
		if (has_h || (has_db && has_fd)) {
			if (!seen_m && has_m) { dexts[k++] = "VK_KHR_external_memory"; added = 1; }
			if (has_h) { if (!seen_h) { dexts[k++] = "VK_EXT_external_memory_host"; added = 1; } hostimp = 1; }
			if (has_db && has_fd) { if (!seen_fd) { dexts[k++] = "VK_KHR_external_memory_fd"; added = 1; }
				if (!seen_db) { dexts[k++] = "VK_EXT_external_memory_dma_buf"; added = 1; } dmabuf = 1; }
			c2.enabledExtensionCount = k; c2.ppEnabledExtensionNames = dexts;
		}
		if (has_f && has_ffd) {
			if (c2.ppEnabledExtensionNames != dexts) { c2.ppEnabledExtensionNames = dexts; }
			if (!seen_f) { dexts[k++] = "VK_KHR_external_fence"; added = 1; }
			if (!seen_ffd) { dexts[k++] = "VK_KHR_external_fence_fd"; added = 1; }
			c2.enabledExtensionCount = k; fencefd = 1;
		}
	}
	VkResult r = create(pd, &c2, a, out);
	if (r && added) {   // retry without our extensions rather than fail the compositor
		qlog("vkCreateDevice with the added memory extensions failed (%d) - retrying without them", (int)r);
		c2.enabledExtensionCount = ci->enabledExtensionCount; c2.ppEnabledExtensionNames = ci->ppEnabledExtensionNames; hostimp = 0; dmabuf = 0; fencefd = 0;
		r = create(pd, &c2, a, out);
	}
	if (r) { qlog("vkCreateDevice failed below the layer: %d", (int)r); return r; }
	pthread_mutex_lock(&g_lock);
	struct dev *d = 0;
	for (int i = 0; i < MAXI; i++) if (!devs[i].key) { d = &devs[i]; break; }
	if (!d) d = &devs[MAXI - 1];
	memset(d, 0, sizeof *d);
	d->key = dkey(*out); d->dev = *out; d->pd = pd; d->in = in; d->gdpa = gdpa;
	d->setLoaderData = ld ? ld->u.pfnSetDeviceLoaderData : 0;
	memcpy(d->realq, real, sizeof d->realq);
	pthread_mutex_init(&d->qlock, 0); pthread_mutex_init(&d->evlock, 0);
#define F(n) d->n = (PFN_vk##n)gdpa(*out, "vk" #n);
	F(DestroyDevice) F(GetDeviceQueue) F(GetDeviceQueue2) F(QueueSubmit) F(QueueSubmit2) F(QueueSubmit2KHR) F(QueueWaitIdle)
	F(QueueBindSparse) F(QueuePresentKHR) F(CreateSwapchainKHR) F(DestroySwapchainKHR) F(GetSwapchainImagesKHR)
	F(AcquireNextImageKHR) F(AcquireNextImage2KHR) F(CreateImage) F(DestroyImage) F(GetImageMemoryRequirements)
	F(AllocateMemory) F(FreeMemory) F(BindImageMemory) F(CreateBuffer) F(DestroyBuffer) F(GetBufferMemoryRequirements)
	F(BindBufferMemory) F(MapMemory) F(UnmapMemory) F(InvalidateMappedMemoryRanges) F(CreateCommandPool)
	F(DestroyCommandPool) F(AllocateCommandBuffers) F(BeginCommandBuffer) F(EndCommandBuffer) F(CmdPipelineBarrier)
	F(CmdCopyImageToBuffer) F(CreateFence) F(DestroyFence) F(WaitForFences) F(ResetFences) F(GetFenceStatus) F(DeviceWaitIdle)
	F(CmdPipelineBarrier2) F(CmdPipelineBarrier2KHR) F(CmdWaitEvents) F(CmdWaitEvents2) F(CmdWaitEvents2KHR)
	F(CreateSemaphore) F(DestroySemaphore) F(GetSemaphoreFdKHR) F(ImportSemaphoreFdKHR) F(GetSemaphoreCounterValue)
	F(GetSemaphoreCounterValueKHR) F(SignalSemaphore) F(SignalSemaphoreKHR)
	F(WaitForPresentKHR) F(GetSwapchainStatusKHR) F(GetRefreshCycleDurationGOOGLE) F(GetPastPresentationTimingGOOGLE)
	F(ReleaseSwapchainImagesEXT) F(ImportFenceFdKHR) F(BindImageMemory2) F(BindImageMemory2KHR) F(GetMemoryFdKHR)
#undef F
	in->GetPhysicalDeviceMemoryProperties(pd, &d->memprops);
	d->cpufence = fencefd && d->ImportFenceFdKHR && envi("Q2VR_CPUFENCE", 1);
	qlog("vsync / acquire fences: %s", d->cpufence ? "signalled from the CPU (sync_fd import)" : "queue submits");
	d->hostimp = hostimp;
	if (hostimp) d->GetMemoryHostPointerPropertiesEXT = (PFN_vkGetMemoryHostPointerPropertiesEXT)gdpa(*out, "vkGetMemoryHostPointerPropertiesEXT");
	d->dmabuf = dmabuf;
	if (dmabuf) d->GetMemoryFdPropertiesKHR = (PFN_vkGetMemoryFdPropertiesKHR)gdpa(*out, "vkGetMemoryFdPropertiesKHR");
	// a queue for acquire/vblank signalling: the first one the app creates (it fetches it too; same handle)
	if (m) d->GetDeviceQueue(*out, qci[0].queueFamilyIndex, 0, &d->queue);   // real (mapped) family; the app may have asked for the alias first
	pthread_mutex_unlock(&g_lock);
	qlog("device created (%u extension(s), %u queue request(s)%s)", ci->enabledExtensionCount, nq,
	     clamped ? ", extra queues aliased to queue 0" : "");
	return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL q_DestroyDevice(VkDevice dv, const VkAllocationCallbacks *a)
{
	struct dev *d = dev_of(dv);
	if (!d) return;
	PFN_vkDestroyDevice f = d->DestroyDevice;
	ssem_drop_device(d);
	bsem_drop_device(d);   // desk-293
	pthread_mutex_lock(&g_lock); d->key = 0; d->queue = 0; pthread_mutex_unlock(&g_lock);
	f(dv, a);
}

// ---- enumeration ----
static const VkLayerProperties layer_props = { LAYER_NAME, VK_MAKE_API_VERSION(0, 1, 3, 0), 1, "Quest 2 direct-mode display for SteamVR" };
static VKAPI_ATTR VkResult VKAPI_CALL q_EnumerateInstanceLayerProperties(uint32_t *n, VkLayerProperties *p)
{ ENUM1(n, p, p[0] = layer_props); }
static VKAPI_ATTR VkResult VKAPI_CALL q_EnumerateInstanceExtensionProperties(const char *layer, uint32_t *n, VkExtensionProperties *p)
{
	if (!layer || strcmp(layer, LAYER_NAME)) return VK_ERROR_LAYER_NOT_PRESENT;
	uint32_t tot = sizeof layer_inst_exts / sizeof layer_inst_exts[0];
	if (!p) { *n = tot; return VK_SUCCESS; }
	uint32_t k = *n < tot ? *n : tot;
	for (uint32_t i = 0; i < k; i++) p[i] = layer_inst_exts[i];
	*n = k; return k < tot ? VK_INCOMPLETE : VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL q_EnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char *layer, uint32_t *n, VkExtensionProperties *p)
{
	if (layer && !strcmp(layer, LAYER_NAME)) { *n = 0; return VK_SUCCESS; }
	struct inst *in = inst_of(pd);
	return in ? in->EnumerateDeviceExtensionProperties(pd, layer, n, p) : VK_ERROR_INITIALIZATION_FAILED;
}

// ---- proc addr ----
struct fn { const char *name; void *fn; };
#define E(n) { "vk" #n, (void *)q_##n },
static const struct fn device_fns[] = {
	E(GetDeviceQueue) E(GetDeviceQueue2) E(QueueSubmit) E(QueueSubmit2) E(QueueSubmit2KHR) E(QueueWaitIdle) E(QueueBindSparse)
	E(CreateSwapchainKHR) E(DestroySwapchainKHR) E(GetSwapchainImagesKHR) E(AcquireNextImageKHR) E(AcquireNextImage2KHR)
	E(QueuePresentKHR) E(DisplayPowerControlEXT) E(RegisterDeviceEventEXT) E(RegisterDisplayEventEXT) E(GetSwapchainCounterEXT)
	E(DestroyDevice) E(CreateCommandPool) E(CreateImage) E(CreateBuffer) E(CmdPipelineBarrier) E(CmdPipelineBarrier2)
	E(CmdPipelineBarrier2KHR) E(CmdWaitEvents) E(CmdWaitEvents2) E(CmdWaitEvents2KHR)
	E(CreateSemaphore) E(DestroySemaphore) E(GetSemaphoreFdKHR) E(ImportSemaphoreFdKHR) E(GetSemaphoreCounterValue)
	E(GetSemaphoreCounterValueKHR) E(SignalSemaphore) E(SignalSemaphoreKHR)
	E(WaitForPresentKHR) E(GetSwapchainStatusKHR) E(GetRefreshCycleDurationGOOGLE) E(GetPastPresentationTimingGOOGLE)
	E(ReleaseSwapchainImagesEXT) E(WaitForFences) E(DestroyFence)
E(AllocateMemory) E(MapMemory) E(CmdCopyImageToBuffer) E(BindImageMemory) E(BindImageMemory2) E(BindImageMemory2KHR) E(GetMemoryFdKHR)
};
static const struct fn instance_fns[] = {
	E(CreateInstance) E(DestroyInstance) E(CreateDevice) E(EnumerateInstanceLayerProperties) E(EnumerateInstanceExtensionProperties)
	E(EnumerateDeviceExtensionProperties)
	E(GetPhysicalDeviceQueueFamilyProperties) E(GetPhysicalDeviceQueueFamilyProperties2) E(GetPhysicalDeviceQueueFamilyProperties2KHR)
	E(GetPhysicalDeviceExternalSemaphoreProperties) E(GetPhysicalDeviceExternalSemaphorePropertiesKHR)
	E(GetPhysicalDeviceDisplayPropertiesKHR) E(GetPhysicalDeviceDisplayProperties2KHR) E(GetPhysicalDeviceDisplayPlanePropertiesKHR)
	E(GetPhysicalDeviceDisplayPlaneProperties2KHR) E(GetDisplayPlaneSupportedDisplaysKHR) E(GetDisplayModePropertiesKHR)
	E(GetDisplayModeProperties2KHR) E(CreateDisplayModeKHR) E(GetDisplayPlaneCapabilitiesKHR) E(GetDisplayPlaneCapabilities2KHR)
	E(CreateDisplayPlaneSurfaceKHR) E(ReleaseDisplayEXT) E(AcquireXlibDisplayEXT) E(GetRandROutputDisplayEXT) E(AcquireDrmDisplayEXT)
	E(GetDrmDisplayEXT) E(DestroySurfaceKHR) E(GetPhysicalDeviceSurfaceSupportKHR) E(GetPhysicalDeviceSurfaceCapabilitiesKHR)
	E(GetPhysicalDeviceSurfaceCapabilities2KHR) E(GetPhysicalDeviceSurfaceCapabilities2EXT) E(GetPhysicalDeviceSurfaceFormatsKHR)
	E(GetPhysicalDeviceSurfaceFormats2KHR) E(GetPhysicalDeviceSurfacePresentModesKHR)
};
#undef E
static void *lookup(const struct fn *t, size_t n, const char *name)
{ for (size_t i = 0; i < n; i++) if (!strcmp(t[i].name, name)) return t[i].fn; return 0; }

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL q2vr_GetDeviceProcAddr(VkDevice dv, const char *name)
{
	if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)q2vr_GetDeviceProcAddr;
	struct dev *d = dv ? dev_of(dv) : 0;
	if (!d) return 0;
	void *f = lookup(device_fns, sizeof device_fns / sizeof device_fns[0], name);
	if (f) {
		// only intercept what the device below actually has (so feature checks stay honest)
		if (!strcmp(name, "vkQueueSubmit2") && !d->QueueSubmit2) return 0;
		if (!strcmp(name, "vkQueueSubmit2KHR") && !d->QueueSubmit2KHR) return 0;
		if (!strcmp(name, "vkGetDeviceQueue2") && !d->GetDeviceQueue2) return 0;
		if (!strcmp(name, "vkAcquireNextImage2KHR") && !d->AcquireNextImage2KHR) return 0;
		if (!strcmp(name, "vkCmdPipelineBarrier2") && !d->CmdPipelineBarrier2) return 0;
		if (!strcmp(name, "vkCmdPipelineBarrier2KHR") && !d->CmdPipelineBarrier2KHR) return 0;
		if (!strcmp(name, "vkCmdWaitEvents2") && !d->CmdWaitEvents2) return 0;
		if (!strcmp(name, "vkCmdWaitEvents2KHR") && !d->CmdWaitEvents2KHR) return 0;
		if (!strcmp(name, "vkGetSemaphoreFdKHR") && !d->GetSemaphoreFdKHR) return 0;
		if (!strcmp(name, "vkImportSemaphoreFdKHR") && !d->ImportSemaphoreFdKHR) return 0;
		if (!strcmp(name, "vkGetSemaphoreCounterValue") && !d->GetSemaphoreCounterValue) return 0;
		if (!strcmp(name, "vkGetSemaphoreCounterValueKHR") && !d->GetSemaphoreCounterValueKHR) return 0;
		if (!strcmp(name, "vkSignalSemaphore") && !d->SignalSemaphore) return 0;
		if (!strcmp(name, "vkSignalSemaphoreKHR") && !d->SignalSemaphoreKHR) return 0;
		if (!strcmp(name, "vkBindImageMemory2") && !d->BindImageMemory2) return 0;
		if (!strcmp(name, "vkBindImageMemory2KHR") && !d->BindImageMemory2KHR) return 0;
		if (!strcmp(name, "vkGetMemoryFdKHR") && !d->GetMemoryFdKHR) return 0;
		return (PFN_vkVoidFunction)f;
	}
	PFN_vkVoidFunction nf = d->gdpa(dv, name);
	if (!nf && (!strcmp(name, "vkCmdDebugMarkerBeginEXT") || !strcmp(name, "vkCmdDebugMarkerEndEXT") || !strcmp(name, "vkCmdDebugMarkerInsertEXT")
	            || !strcmp(name, "vkDebugMarkerSetObjectNameEXT") || !strcmp(name, "vkDebugMarkerSetObjectTagEXT")
	            || !strcmp(name, "vkSetHdrMetadataEXT"))) {
		qlog("%s: not in the GPU driver, using a no-op", name);
		return (PFN_vkVoidFunction)q_noop;
	}
	if (!nf) qlog("vkGetDeviceProcAddr(%s) = NULL", name);
	return nf;
}
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL q2vr_GetInstanceProcAddr(VkInstance inst, const char *name)
{
	if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)q2vr_GetInstanceProcAddr;
	if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)q2vr_GetDeviceProcAddr;
	void *f = lookup(instance_fns, sizeof instance_fns / sizeof instance_fns[0], name);
	if (f) return (PFN_vkVoidFunction)f;
	f = lookup(device_fns, sizeof device_fns / sizeof device_fns[0], name);
	if (f) return (PFN_vkVoidFunction)f;
	if (!inst) return 0;
	struct inst *in = inst_of(inst);
	return in ? in->gipa(inst, name) : 0;
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v)
{
	if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
	v->pfnGetInstanceProcAddr = q2vr_GetInstanceProcAddr;
	v->pfnGetDeviceProcAddr = q2vr_GetDeviceProcAddr;
	v->pfnGetPhysicalDeviceProcAddr = 0;
	return VK_SUCCESS;
}
