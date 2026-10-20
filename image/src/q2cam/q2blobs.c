// q2blobs - step 1 of controller tracking: find bright blobs (controller IR LEDs) in the tracking-camera frames that
// q2camd publishes in /dev/shm/q2cam (layout: see q2camlive_main.c).  Read-only consumer; it never touches hardware.
//
// For every frame: threshold, 8-connected components, intensity-weighted centroid, area, peak.  The short-exposure
// ("LED") frames are where Meta's constellation tracking looks for the controller LEDs; long-exposure ("scene")
// frames are counted too, for comparison.
// Runs for N seconds (default 60), then writes:
//   /var/log/q2/blobs.txt               per-second blob counts per camera + the blob list of the best frames
//   /var/log/q2/blobs_cam<N>_pgm.bin    the LED frame with the most blobs per camera, contrast-stretched, blobs boxed
// Usage: q2blobs [seconds] [threshold]
#define TAG "q2blobs: "
#define LOGF "/var/log/q2/blobs.txt"
#include "q2cam_rt.h"

#define W 640
#define H 480
#define MAXB 256
struct blob { float cx, cy; int area, peak, sum, x0, y0, x1, y1; };


static uint32_t stack32[W * H];
static uint8_t seen[W * H];

static int find_blobs(const uint8_t *img, int thr, struct blob *b, int maxb)
{
	int n = 0;
	memset(seen, 0, sizeof seen);
	for (int y = 1; y < H - 1; y++)
		for (int x = 1; x < W - 1; x++) {
			int i = y * W + x;
			if (img[i] < thr || seen[i]) continue;
			int sp = 0; stack32[sp++] = i; seen[i] = 1;
			long sx = 0, sy = 0, s = 0; int area = 0, peak = 0, x0 = x, x1 = x, y0 = y, y1 = y;
			while (sp) {
				int p = stack32[--sp], px = p % W, py = p / W, v = img[p];
				area++; s += v; sx += (long)v * px; sy += (long)v * py; if (v > peak) peak = v;
				if (px < x0) x0 = px; if (px > x1) x1 = px; if (py < y0) y0 = py; if (py > y1) y1 = py;
				for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
					int qx = px + dx, qy = py + dy; if (qx < 0 || qy < 0 || qx >= W || qy >= H) continue;
					int q = qy * W + qx; if (seen[q] || img[q] < thr) continue;
					seen[q] = 1; if (sp < W * H) stack32[sp++] = q;
				}
			}
			if (area > 400) continue;          // big bright regions (lamps, windows) are not LEDs
			if (n < maxb) { struct blob *o = &b[n++]; o->cx = (float)sx / s; o->cy = (float)sy / s; o->area = area; o->peak = peak; o->sum = (int)s;
				o->x0 = x0; o->y0 = y0; o->x1 = x1; o->y1 = y1; }
		}
	return n;
}

static void putf(char **o, float v) { if (v < 0) { *(*o)++ = '-'; v = -v; } unsigned i = (unsigned)v; unsigned f = (unsigned)((v - i) * 10); char t[12]; int k = 0; do { t[k++] = '0' + i % 10; i /= 10; } while (i); while (k) *(*o)++ = t[--k]; *(*o)++ = '.'; *(*o)++ = '0' + f; }
static void putu(char **o, unsigned v) { char t[12]; int k = 0; do { t[k++] = '0' + v % 10; v /= 10; } while (v); while (k) *(*o)++ = t[--k]; }
static void puts_(char **o, const char *s) { while (*s) *(*o)++ = *s++; }

static uint8_t frame[W * H], best[4][W * H], lastled[4][W * H], mos[960 * 1280];
static struct blob lastbl[4][MAXB]; static int lastn[4];
static float pmean[4][2];   // desk-215: running brightness per frame-number parity -> which parity is the LED frame
static struct blob bl[MAXB], bestbl[4][MAXB];
static int bestn[4];
static uint64_t bestfn[4];

int main(int argc, char **argv)
{
	int secs = 60, thr = 64;
	if (argc > 1) { int v = 0; for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) v = v * 10 + *p - '0'; if (v >= 5 && v <= 3600) secs = v; }
	if (argc > 2) { int v = 0; for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++) v = v * 10 + *p - '0'; if (v >= 8 && v <= 250) thr = v; }
	logfd = sys_open(LOGF, 01 | 0100 | 01000);
	P("start: %d s, threshold %d (8-bit), reading /dev/shm/q2cam", secs, thr);
	int fd = -1;
	for (int i = 0; i < 120 && fd < 0; i++) { fd = sys_open("/dev/shm/q2cam", 0); if (fd < 0) sleep_ms(500); }
	if (fd < 0) { P("/dev/shm/q2cam not found - is q2-camd running?"); return 1; }
	uint32_t hdr[8]; sc6(63, fd, (long)hdr, sizeof hdr, 0, 0, 0);
	if (hdr[0] != 0x4d433251 || hdr[3] != W || hdr[4] != H) { P("unexpected shm header %x %u %u", hdr[0], hdr[3], hdr[4]); return 1; }
	uint32_t nslots = hdr[5], hsz = hdr[6], stride = hdr[7];
	size_t sz = hsz + (size_t)4 * nslots * stride;
	const uint8_t *shm = sys_mmap(sz, 1 /*READ*/, 1, fd); sys_close(fd);
	if ((long)shm < 0 && (long)shm > -4096) { P("mmap failed"); return 1; }
	uint64_t last[4] = { 0 };
	for (int c = 0; c < 4; c++) last[c] = *(volatile const uint64_t *)(shm + 0x40 + c * 64);
	double t0 = now(), tsec = t0, tsnap = t0; unsigned nsnap = 0;
	unsigned frames[4][2] = { { 0 } }, withblobs[4][2] = { { 0 } }, blobsum[4][2] = { { 0 } }, dropped = 0;
	while (now() - t0 < secs) {
		int any = 0;
		for (int c = 0; c < 4; c++) {
			uint64_t pub = *(volatile const uint64_t *)(shm + 0x40 + c * 64);
			if (pub == last[c]) continue;
			if (pub - last[c] > 1) dropped += (unsigned)(pub - last[c] - 1);
			last[c] = pub; any = 1;
			const uint8_t *slot = shm + hsz + (size_t)(c * nslots + (uint32_t)((pub - 1) % nslots)) * stride;
			uint64_t s1 = *(volatile const uint64_t *)slot;
			if (s1 & 1) continue;
			uint64_t fn = ((const uint64_t *)slot)[1]; unsigned mean = ((const uint32_t *)slot)[8];
			// desk-214's per-frame "brighter than the previous frame" flag broke when both exposures looked alike
			// (headset face-down): decide by frame-number parity, comparing running averages of the two parities
			pmean[c][fn & 1] = pmean[c][fn & 1] * 0.95f + mean * 0.05f;
			int bright = pmean[c][fn & 1] > pmean[c][(fn & 1) ^ 1];
			memcpy(frame, slot + 64, W * H);
			if (*(volatile const uint64_t *)slot != s1) continue;     // overwritten while copying
			int n = find_blobs(frame, thr, bl, MAXB);
			int k = bright ? 1 : 0;
			frames[c][k]++; if (n) { withblobs[c][k]++; blobsum[c][k] += n; }
			if (!bright) { memcpy(lastled[c], frame, W * H); memcpy(lastbl[c], bl, sizeof(struct blob) * n); lastn[c] = n; }
			if (!bright && n > bestn[c]) { bestn[c] = n; bestfn[c] = fn; memcpy(best[c], frame, W * H); memcpy(bestbl[c], bl, sizeof(struct blob) * n); }
		}
		double t = now();
		if (t - tsnap >= 12.0) {    // every 12 s: 2x2 mosaic of the latest LED frames (x4 brighter, blobs boxed) + blob list
			tsnap = t;
			for (int c = 0; c < 4; c++) {
				int ox = (c & 1) * 640, oy = (c / 2) * 480;
				for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { int v = lastled[c][y * W + x] * 4; mos[(oy + y) * 1280 + ox + x] = v > 255 ? 255 : v; }
				for (int i = 0; i < lastn[c]; i++) {
					struct blob *q = &lastbl[c][i];
					int x0 = q->x0 - 3, x1 = q->x1 + 3, y0 = q->y0 - 3, y1 = q->y1 + 3;
					if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 >= W) x1 = W - 1; if (y1 >= H) y1 = H - 1;
					for (int x = x0; x <= x1; x++) { mos[(oy + y0) * 1280 + ox + x] = 255; mos[(oy + y1) * 1280 + ox + x] = 255; }
					for (int y = y0; y <= y1; y++) { mos[(oy + y) * 1280 + ox + x0] = 255; mos[(oy + y) * 1280 + ox + x1] = 255; }
				}
				char b[400]; char *o = b; puts_(&o, "snap "); putu(&o, nsnap); puts_(&o, " cam"); putu(&o, c); puts_(&o, ": "); putu(&o, lastn[c]); puts_(&o, " blobs");
				for (int i = 0; i < lastn[c] && i < 14; i++) { struct blob *q = &lastbl[c][i]; puts_(&o, " ("); putu(&o, (unsigned)q->cx); *o++ = ','; putu(&o, (unsigned)q->cy); *o++ = ' '; putu(&o, q->area); *o++ = 'p'; *o++ = 'x'; *o++ = ')'; }
				*o = 0; P("%s", b);
			}
			char fn[48]; char *o = fn; puts_(&o, "/var/log/q2/blobs_snap"); *o++ = '0' + nsnap % 10; puts_(&o, "_pgm.bin"); *o = 0;
			int f = sys_open(fn, 01 | 0100 | 01000);
			if (f >= 0) { const char *hh = "P5\n1280 960\n255\n"; sys_write(f, hh, slen(hh)); sys_write(f, mos, sizeof mos); sys_close(f); }
			nsnap++;
		}
		if (t - tsec >= 1.0) {
			char b[400]; char *o = b;
			for (int c = 0; c < 4; c++) {
				puts_(&o, "cam"); putu(&o, c); puts_(&o, " LED frames "); putu(&o, frames[c][0]); puts_(&o, " (with blobs "); putu(&o, withblobs[c][0]);
				puts_(&o, ", avg "); putf(&o, withblobs[c][0] ? (float)blobsum[c][0] / withblobs[c][0] : 0); puts_(&o, ") scene "); putu(&o, frames[c][1]);
				puts_(&o, " ("); putu(&o, withblobs[c][1]); puts_(&o, ") | ");
				for (int k = 0; k < 2; k++) frames[c][k] = withblobs[c][k] = blobsum[c][k] = 0;
			}
			*o = 0; P("%s missed %u", b, dropped); dropped = 0; tsec = t;
		}
		if (!any) sleep_ms(2);
	}
	for (int c = 0; c < 4; c++) {
		P("camera %d: best LED frame %llu with %d blobs", c, (unsigned long long)bestfn[c], bestn[c]);
		for (int i = 0; i < bestn[c] && i < 40; i++) {
			char b[160]; char *o = b; struct blob *q = &bestbl[c][i];
			puts_(&o, "  blob x "); putf(&o, q->cx); puts_(&o, " y "); putf(&o, q->cy); puts_(&o, " area "); putu(&o, q->area); puts_(&o, " peak "); putu(&o, q->peak); *o = 0;
			P("%s", b);
		}
		if (!bestn[c]) continue;
		// contrast stretch x4 and draw boxes
		static uint8_t img[W * H];
		for (int i = 0; i < W * H; i++) { int v = best[c][i] * 4; img[i] = v > 255 ? 255 : v; }
		for (int i = 0; i < bestn[c]; i++) {
			struct blob *q = &bestbl[c][i];
			int x0 = q->x0 - 3, x1 = q->x1 + 3, y0 = q->y0 - 3, y1 = q->y1 + 3;
			if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 >= W) x1 = W - 1; if (y1 >= H) y1 = H - 1;
			for (int x = x0; x <= x1; x++) { img[y0 * W + x] = 255; img[y1 * W + x] = 255; }
			for (int y = y0; y <= y1; y++) { img[y * W + x0] = 255; img[y * W + x1] = 255; }
		}
		char fn[48]; char *o = fn; puts_(&o, "/var/log/q2/blobs_cam"); *o++ = '0' + c; puts_(&o, "_pgm.bin"); *o = 0;
		int f = sys_open(fn, 01 | 0100 | 01000);
		if (f >= 0) { const char *hh = "P5\n640 480\n255\n"; sys_write(f, hh, slen(hh)); sys_write(f, img, W * H); sys_close(f); }
	}
	P("done");

	return 0;
}

__attribute__((naked, noreturn)) void _start(void)
{
	__asm__ volatile("ldr x0, [sp]\n" "add x1, sp, #8\n" "and sp, x1, #~15\n" "bl c_start\n");
}
__attribute__((used)) void c_start(long argc, char **argv) { sys_exit(main((int)argc, argv)); }
