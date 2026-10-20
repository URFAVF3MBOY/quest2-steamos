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
