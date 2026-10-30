/*
 * q2blit - mirror an X virtual framebuffer (Xvfb -fbdir, an XWD file) into the Quest 2's splash framebuffer.
 *
 * The bootloader leaves a 1920x3664 (portrait, pitch 7680) 32-bit framebuffer at phys 0x9c000000 scanning out; the
 * panel is two stacked "eyes" of 1920x1832.  This copies the X screen (default 1832x1920 landscape-ish canvas) into
 * that memory rotated by 270 degrees (same mapping as qscreen -r 270), once per eye (mode 1) or across the whole panel
 * (mode 0, canvas 3664x1920).  Only the changed rectangle is rewritten.  Libc-free on aarch64 (raw syscalls).
 *
 *   q2blit [-r 270] [-m 0|1] [-f rgb|bgr] [-a phys] [-M memfile] [-1] XWDFILE
 *     -f bgr : swap red/blue (use if the splash framebuffer is ABGR)
 *     -1     : copy one frame and exit
 */
#ifdef HOSTED
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
typedef unsigned char u8; typedef unsigned int u32; typedef unsigned long long u64;
static long q_open(const char *p, int rw) { return open(p, rw ? O_RDWR : O_RDONLY); }
static void *q_mmap(long fd, u64 len, int rw, u64 off) { void *p = mmap(0, len, rw ? PROT_READ|PROT_WRITE : PROT_READ, MAP_SHARED, (int)fd, (off_t)off); return p == MAP_FAILED ? (void*)-1 : p; }
static long q_size(long fd) { return lseek((int)fd, 0, SEEK_END); }
static void *q_amap(u64 len) { void *p = mmap(0, len, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0); return p == MAP_FAILED ? (void*)-1 : p; }
static void q_sleep_ms(long ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, 0); }
static void say(const char *s) { fputs(s, stderr); }
__attribute__((noreturn)) static void quit(int c) { exit(c); }
#define MAIN int main(int argc, char **argv)
#define ARGS_INIT
#else
typedef unsigned char u8; typedef unsigned int u32; typedef unsigned long u64;
#define SYS_openat 56
#define SYS_lseek 62
#define SYS_write 64
#define SYS_exit 93
#define SYS_nanosleep 101
#define SYS_mmap 222
#define AT_FDCWD (-100)
static inline long sc6(long n, long a, long b, long c, long d, long e, long f) {
	register long x8 __asm__("x8") = n; register long x0 __asm__("x0") = a; register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c; register long x3 __asm__("x3") = d; register long x4 __asm__("x4") = e; register long x5 __asm__("x5") = f;
	__asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory", "cc");
	return x0;
}
static long q_open(const char *p, int rw) { return sc6(SYS_openat, AT_FDCWD, (long)p, rw ? (02 | 04010000) : 0, 0, 0, 0); } /* O_RDWR|O_SYNC */
static void *q_mmap(long fd, u64 len, int rw, u64 off) { long r = sc6(SYS_mmap, 0, (long)len, rw ? 3 : 1, 1 /*MAP_SHARED*/, fd, (long)off); return (r < 0 && r > -4096) ? (void *)-1 : (void *)r; }
static long q_size(long fd) { return sc6(SYS_lseek, fd, 0, 2, 0, 0, 0); }
static void *q_amap(u64 len) { long r = sc6(SYS_mmap, 0, (long)len, 3, 0x22 /*MAP_PRIVATE|MAP_ANONYMOUS*/, -1, 0); return (r < 0 && r > -4096) ? (void *)-1 : (void *)r; }
static void q_sleep_ms(long ms) { long ts[2] = { ms / 1000, (ms % 1000) * 1000000L }; sc6(SYS_nanosleep, (long)ts, 0, 0, 0, 0, 0); }
static void say(const char *s) { long n = 0; while (s[n]) n++; sc6(SYS_write, 2, (long)s, n, 0, 0, 0); }
__attribute__((noreturn)) static void quit(int c) { for (;;) sc6(SYS_exit, c, 0, 0, 0, 0, 0); }
void *memcpy(void *d, const void *s, unsigned long n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { u8 *a = d; while (n--) *a++ = (u8)c; return d; }
#define MAIN void c_main(long *sp)
#endif

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static long atoi_(const char *s) { long v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v; }
static u64 hex_(const char *s) { u64 v = 0; if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2; for (;; s++) { int d; if (*s >= '0' && *s <= '9') d = *s - '0'; else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10; else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10; else break; v = v * 16 + d; } return v; }

#define PW 1920u
#define PH 3664u
#define PPITCH 7680u
#define EHALF (PH / 2)

static u8 *dst;           /* panel framebuffer */
static int swaprb;
static u32 cv(u32 p) { return swaprb ? 0xff000000u | (p & 0xff) << 16 | (p & 0xff00) | (p >> 16 & 0xff) : 0xff000000u | (p & 0xffffff); }

/* write logical rect [x0,x1] x [y0,y1] (inclusive) of canvas `src` (stride in bytes) into one eye starting at row `rowbase` of the panel.
 * rotation 270: panel px = ly, py = (EH-1) - lx   (canvas is EH wide x PW tall) */
static u32 OX, OY;        /* where the canvas sits inside one eye (landscape eye coords, before the 270 rotation) */
static void blit_rect_eye(const u8 *src, u32 sstride, u32 rowbase, u32 x0, u32 x1, u32 y0, u32 y1)
{
	for (u32 cx = x0; cx <= x1; cx++) {
		u32 py = (EHALF - 1) - (cx + OX);
		u32 *drow = (u32 *)(dst + (u64)(rowbase + py) * PPITCH);
		for (u32 cy = y0; cy <= y1; cy++)
			drow[cy + OY] = cv(*(const u32 *)(src + (u64)cy * sstride + (u64)cx * 4));
	}
}
/* whole-panel mode: canvas is PH wide x PW tall; panel px = ly, py = (PH-1) - lx */
static void blit_rect_full(const u8 *src, u32 sstride, u32 x0, u32 x1, u32 y0, u32 y1)
{
	for (u32 py = (PH - 1) - x1; py <= (PH - 1) - x0; py++) {
		u32 lx = (PH - 1) - py;
		u32 *drow = (u32 *)(dst + (u64)py * PPITCH);
		for (u32 px = y0; px <= y1; px++)
			drow[px] = cv(*(const u32 *)(src + (u64)px * sstride + (u64)lx * 4));
	}
}

/* ---- virtual-monitor lens pre-warp (optional, -v) ----
 * Each eye pixel -> radius from the lens centre -> view-space tangent via the Quest lens curve (CatmullRom10 table) ->
 * flat virtual screen at unit distance spanning +-VH tangent (horizontal) -> source pixel of the X canvas.  Table is
 * the Oculus LensConfig scale (screen radius = tan radius * S) so we invert it numerically. */
static const float LA[11] = {1.0f,1.0374f,1.0810f,1.1330f,1.1970f,1.2754f,1.3771f,1.5133f,1.7018f,1.9732f,2.3f};
#ifdef HOSTED
#define SQRTF(x) __builtin_sqrtf(x)
#else
static float q_sqrtf(float x) { float r; __asm__("fsqrt %s0, %s1" : "=w"(r) : "w"(x)); return r; }
#define SQRTF(x) q_sqrtf(x)
#endif
static float crom(float t)
{
	if (t < 0) t = 0; if (t > 1) t = 1;
	float f = t * 10.0f; int i = (int)f; if (i > 9) i = 9; float u = f - (float)i;
	float p1 = LA[i], p2 = LA[i + 1];
	float p0 = i > 0 ? LA[i - 1] : 2 * p1 - p2, p3 = i < 9 ? LA[i + 2] : 2 * p2 - p1;
	return 0.5f * ((2 * p1) + (-p0 + p2) * u + (2 * p0 - 5 * p1 + 4 * p2 - p3) * u * u + (-p0 + 3 * p1 - 3 * p2 + p3) * u * u * u);
}
static u32 *build_lut(u32 EW, u32 EH, u32 SW, u32 SH, float cxs, float cys, float tmax, float hh, float aspect)
{
	u32 *lut = q_amap((u64)EW * EH * 4);
	if (lut == (void *)-1) return 0;
	float rmax = 0.5f * (float)EH;                 /* screen radius (px) that maps to tmax */
	for (u32 y = 0; y < EH; y++)
		for (u32 x = 0; x < EW; x++) {
			float dx = ((float)x - cxs) / rmax, dy = ((float)y - cys) / rmax;   /* normalised screen radius units */
			float rs = SQRTF(dx * dx + dy * dy);
			u32 v = 0xffffffffu;
			if (rs < 1.0f) {
				/* screen radius rs = (tan/tmax) * S(tan/tmax); invert by fixed-point iteration */
				float q = rs;
				for (int k = 0; k < 12; k++) q = rs / crom(q);
				float sc = rs > 1e-6f ? (q / rs) : 1.0f;
				float tx = dx * sc * tmax, ty = dy * sc * tmax;
				float sx = (tx / hh * 0.5f + 0.5f) * (float)SW;
				float sy = (ty / (hh / aspect) * 0.5f + 0.5f) * (float)SH;
				if (sx >= 0 && sy >= 0 && sx < (float)SW && sy < (float)SH) v = (u32)sy << 16 | (u32)sx;
			}
			lut[(u64)y * EW + x] = v;
		}
	return lut;
}
static void blit_warp(const u32 *lut, const u8 *src, u32 sstride, u32 rowbase, u32 EW, u32 EH, u32 bgv, u32 x0, u32 x1, u32 y0, u32 y1, int all)
{
	for (u32 cx = 0; cx < EW; cx++) {
		u32 py = (EHALF - 1) - cx;
		u32 *drow = (u32 *)(dst + (u64)(rowbase + py) * PPITCH);
		for (u32 cy = 0; cy < EH; cy++) {
			u32 l = lut[(u64)cy * EW + cx];
			if (l == 0xffffffffu) { if (all) drow[cy] = bgv; continue; }
			u32 sx = l & 0xffff, sy = l >> 16;
			if (!all && (sx < x0 || sx > x1 || sy < y0 || sy > y1)) continue;
			drow[cy] = cv(*(const u32 *)(src + (u64)sy * sstride + (u64)sx * 4));
		}
	}
}

MAIN
{
#ifndef HOSTED
	long argc = sp[0]; char **argv = (char **)(sp + 1);
#endif
	int mode = 1, once = 0, warp = 0; long dctr = 0, tmx = 240, mon = 45;
	long vw = 0, vh = 0, ox = -1, oy = -1; u32 bg = 0;
	const char *memf = "/dev/mem", *xwd = 0;
	u64 phys = 0x9c000000ULL;
	for (int i = 1; i < argc; i++) {
		if (streq(argv[i], "-m") && i + 1 < argc) mode = (int)atoi_(argv[++i]);
		else if (streq(argv[i], "-r") && i + 1 < argc) { if (atoi_(argv[++i]) != 270) { say("q2blit: only -r 270 is implemented\n"); quit(2); } }
		else if (streq(argv[i], "-f") && i + 1 < argc) swaprb = streq(argv[++i], "bgr");
		else if (streq(argv[i], "-a") && i + 1 < argc) phys = hex_(argv[++i]);
		else if (streq(argv[i], "-M") && i + 1 < argc) memf = argv[++i];
		else if (streq(argv[i], "-w") && i + 1 < argc) vw = atoi_(argv[++i]);
		else if (streq(argv[i], "-h") && i + 1 < argc) vh = atoi_(argv[++i]);
		else if (streq(argv[i], "-x") && i + 1 < argc) ox = atoi_(argv[++i]);
		else if (streq(argv[i], "-y") && i + 1 < argc) oy = atoi_(argv[++i]);
		else if (streq(argv[i], "-b") && i + 1 < argc) bg = (u32)hex_(argv[++i]);
		else if (streq(argv[i], "-1")) once = 1;
		else if (streq(argv[i], "-v")) warp = 1;
		else if (streq(argv[i], "-d") && i + 1 < argc) { const char *a = argv[++i]; int neg = *a == '-'; dctr = atoi_(neg ? a + 1 : a); if (neg) dctr = -dctr; }
		else if (streq(argv[i], "-t") && i + 1 < argc) tmx = atoi_(argv[++i]);   /* edge tangent x100 */
		else if (streq(argv[i], "-s") && i + 1 < argc) mon = atoi_(argv[++i]);   /* monitor half-width tangent x100 */
		else xwd = argv[i];
	}
	if (!xwd) { say("usage: q2blit [-m 0|1] [-f rgb|bgr] [-a phys] [-M memfile] [-1] XWDFILE\n"); quit(2); }

	long xf = q_open(xwd, 0);
	if (xf < 0) { say("q2blit: cannot open the X framebuffer file\n"); quit(1); }
	long xsz = q_size(xf);
	const u8 *x = q_mmap(xf, (u64)xsz, 0, 0);
	if (x == (void *)-1 || xsz < 100) { say("q2blit: cannot map the X framebuffer file\n"); quit(1); }
	u32 hsz = be32(x), W = be32(x + 16), H = be32(x + 20), bpp = be32(x + 44), bpl = be32(x + 48), ncol = be32(x + 76);
	u64 off = hsz + (u64)ncol * 12;
	if (bpp != 32 || off + (u64)bpl * H > (u64)xsz) { say("q2blit: unexpected XWD layout (need 32 bpp)\n"); quit(1); }
	const u8 *src = x + off;
	u32 needw = mode ? EHALF : PH;
	if (mode && vw > 0 && vh > 0) {   /* smaller canvas centred in each eye (unless -x/-y given) */
		if (vw > (long)EHALF) vw = EHALF;
		if (vh > (long)PW) vh = PW;
		needw = (u32)vw;
		if (ox < 0) ox = (EHALF - vw) / 2;
		if (oy < 0) oy = (PW - vh) / 2;
		OX = (u32)ox; OY = (u32)oy;
		if (W < (u32)vw || H < (u32)vh) { say("q2blit: X screen smaller than -w/-h\n"); quit(1); }
	} else if (W < needw || H < PW) { say("q2blit: X screen too small (need 1832x1920 for -m 1, 3664x1920 for -m 0)\n"); quit(1); }

	long mf = q_open(memf, 1);
	if (mf < 0) { say("q2blit: cannot open the memory device\n"); quit(1); }
	dst = q_mmap(mf, 36700160ULL, 1, phys);
	if (dst == (void *)-1) { say("q2blit: cannot map the splash framebuffer\n"); quit(1); }

	/* shadow copy of the canvas to find the changed rectangle */
	u32 CW = mode ? EHALF : PH, CH = PW;
	if (mode && vw > 0 && vh > 0) { CW = (u32)vw; CH = (u32)vh; }
	if (mode && vw > 0) for (u32 i = 0; i < 36700160u / 4; i++) ((u32 *)dst)[i] = cv(bg);   /* black margins */
	u8 *prev = q_amap((u64)bpl * CH);
	if (prev == (void *)-1) { say("q2blit: out of memory\n"); quit(1); }
	int first = 1;
	u32 *lutL = 0, *lutR = 0;
	if (warp) {
		float c = (float)EHALF / 2.0f;   /* eye canvas is EHALF wide x PW tall; lens centre mid-eye +- decenter */
		lutL = build_lut(EHALF, PW, CW, CH, c + (float)dctr, (float)PW / 2, (float)tmx / 100.0f, (float)mon / 100.0f, (float)CW / (float)CH);
		lutR = build_lut(EHALF, PW, CW, CH, c - (float)dctr, (float)PW / 2, (float)tmx / 100.0f, (float)mon / 100.0f, (float)CW / (float)CH);
		if (!lutL || !lutR) { say("q2blit: out of memory (lut)\n"); quit(1); }
	}
	say("q2blit: running\n");
	for (;;) {
		u32 minx = CW, maxx = 0, miny = CH, maxy = 0;
		int changed = 0;
		for (u32 y = 0; y < CH; y++) {
			const u32 *a = (const u32 *)(src + (u64)y * bpl);
			u32 *b = (u32 *)(prev + (u64)y * bpl);
			u32 fx = CW, lx = 0; int rowdiff = 0;
			for (u32 xx = 0; xx < CW; xx++) {
				if (a[xx] != b[xx]) { if (!rowdiff) fx = xx; lx = xx; rowdiff = 1; b[xx] = a[xx]; }
			}
			if (rowdiff) {
				changed = 1;
				if (fx < minx) minx = fx;
				if (lx > maxx) maxx = lx;
				if (y < miny) miny = y;
				if (y > maxy) maxy = y;
			}
		}
		if (first) { minx = 0; maxx = CW - 1; miny = 0; maxy = CH - 1; changed = 1; first = 0; }
		if (changed) {
			if (warp) {
				blit_warp(lutL, src, bpl, 0, EHALF, PW, cv(bg), minx, maxx, miny, maxy, 0);
				blit_warp(lutR, src, bpl, EHALF, EHALF, PW, cv(bg), minx, maxx, miny, maxy, 0);
			} else if (mode) {
				blit_rect_eye(src, bpl, 0, minx, maxx, miny, maxy);
				blit_rect_eye(src, bpl, EHALF, minx, maxx, miny, maxy);
			} else {
				blit_rect_full(src, bpl, minx, maxx, miny, maxy);
			}
		}
		if (once) break;
		q_sleep_ms(changed ? 30 : 60);
	}
	quit(0);
}
#ifndef HOSTED
__asm__(".global _start\n_start:\n mov x0, sp\n and sp, x0, #-16\n bl c_main\n");
#endif
