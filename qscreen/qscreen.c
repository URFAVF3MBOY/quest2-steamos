/*
 * qscreen - put text on the Quest 2 display from userspace (no fbdev, no kernel rebuild).
 *
 * Talks to the downstream msm_drm/SDE driver through /dev/dri/card0 with raw DRM ioctls:
 * dumb buffer -> framebuffer -> legacy SETCRTC on the DSI connector, then renders text
 * (ANSI escapes stripped) into the buffer, once for each eye half of the panel.
 *
 *   qscreen -l            list connectors/modes and exit
 *   qscreen -t            test card (coloured edge bands + eye labels), stays up
 *   some_cmd | qscreen    scrolling text from stdin, stays up after EOF
 *
 * Options: -d dev  -m modeindex  -r 0|90|180|270  -s scale  -f xrgb|argb|xbgr|abgr
 *          -c cols -n rows  -b (edge bands)  -q (exit at EOF)  -o file.ppm (write PPM at EOF)
 *          -k  show the kernel log (/dev/kmsg, follows new messages) instead of stdin
 *
 * Build for the headset: aarch64-linux-gnu-gcc -O2 -static -o qscreen qscreen.c
 * Host check build:      gcc -O2 -DQS_FAKE -o qscreen_fake qscreen.c   (writes a PPM, no DRM)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>

#include "qfont.h"

static int fd = -1;
static uint32_t W, H, pitch; /* framebuffer size in pixels, pitch in bytes */
static uint8_t *fb;
static int swap_rb; /* 1 for XBGR/ABGR formats */
static int rot = 0, scale = 1, cols = 80, rows = 40, borders = 0, quit_at_eof = 0;
static const char *ppm_out;
static int kmsg_mode;
static int splash_mode;
static unsigned long long splash_addr = 0x9c000000ULL; /* cont_splash_region in the Quest 2 DT */
static size_t splash_size = 36700160;
static unsigned user_pitch, gw, gh;
static const char *memdev = "/dev/mem";

static int xioctl(int req_fd, unsigned long req, void *arg)
{
	int r;
	do { r = ioctl(req_fd, req, arg); } while (r < 0 && (errno == EINTR || errno == EAGAIN));
	return r;
}

/* ---------------------------------------------------------------- drawing */

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
	return swap_rb ? ((uint32_t)b << 16 | g << 8 | r) : ((uint32_t)r << 16 | g << 8 | b);
}

/* eye halves: eye 0 = top half of the (portrait) panel, eye 1 = bottom half */
static uint32_t EW, EH;

static inline void put_phys(int eye, int px, int py, uint32_t c)
{
	if (px < 0 || py < 0 || (uint32_t)px >= EW || (uint32_t)py >= EH) return;
	uint32_t y = (uint32_t)py + (eye ? EH : 0);
	if (y >= H) return;
	*(uint32_t *)(fb + (size_t)y * pitch + (size_t)px * 4) = 0xff000000u | c;
}

/* logical (rotated) canvas of one eye: size depends on rotation */
static inline void canvas_size(int *cw, int *ch)
{
	if (rot == 90 || rot == 270) { *cw = EH; *ch = EW; } else { *cw = EW; *ch = EH; }
}

static inline void put_logical(int eye, int lx, int ly, uint32_t c)
{
	int px, py;
	switch (rot) {
	case 90:  px = (int)EW - 1 - ly; py = lx; break;
	case 180: px = (int)EW - 1 - lx; py = (int)EH - 1 - ly; break;
	case 270: px = ly; py = (int)EH - 1 - lx; break;
	default:  px = lx; py = ly; break;
	}
	put_phys(eye, px, py, c);
}

static void fill_logical(int eye, int x, int y, int w, int h, uint32_t c)
{
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++)
			put_logical(eye, x + i, y + j, c);
}

static void draw_glyph(int eye, int x, int y, unsigned char ch, int sc, uint32_t fg, int opaque_bg, uint32_t bg)
{
	if (ch < 32 || ch > 126) ch = '?';
	const unsigned short *g = &qfont[(ch - 32) * QF_H];
	for (int gy = 0; gy < QF_H; gy++) {
		unsigned short row = g[gy];
		for (int gx = 0; gx < QF_W; gx++) {
			int on = (row >> (QF_W - 1 - gx)) & 1;
			if (!on && !opaque_bg) continue;
			uint32_t c = on ? fg : bg;
			for (int dy = 0; dy < sc; dy++)
				for (int dx = 0; dx < sc; dx++)
					put_logical(eye, x + gx * sc + dx, y + gy * sc + dy, c);
		}
	}
}

static void draw_text(int eye, int x, int y, const char *s, int sc, uint32_t fg)
{
	for (; *s; s++, x += QF_W * sc)
		draw_glyph(eye, x, y, (unsigned char)*s, sc, fg, 0, 0);
}

/* ------------------------------------------------------------ text grid */

static char *grid; /* rows * cols */
static int cur_c, cur_r;
static int dirty = 1;

static void grid_clear(void)
{
	memset(grid, ' ', (size_t)rows * cols);
	cur_c = cur_r = 0;
}

static void grid_scroll(void)
{
	memmove(grid, grid + cols, (size_t)(rows - 1) * cols);
	memset(grid + (size_t)(rows - 1) * cols, ' ', cols);
}

static void grid_newline(void)
{
	cur_c = 0;
	if (++cur_r >= rows) { grid_scroll(); cur_r = rows - 1; }
}

static void grid_putc(unsigned char ch)
{
	if (cur_c >= cols) grid_newline();
	grid[(size_t)cur_r * cols + cur_c++] = (char)ch;
}

/* stream parser: strips ANSI escapes, handles \n \r \b \t, collapses UTF-8 to '?' */
static int esc_state; /* 0 none, 1 saw ESC, 2 in CSI, 3 in OSC, 4 ESC-in-OSC */
static int utf_skip;

static void feed(const unsigned char *buf, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned char ch = buf[i];
		dirty = 1;
		switch (esc_state) {
		case 1:
			if (ch == '[') esc_state = 2;
			else if (ch == ']') esc_state = 3;
			else if (ch >= 0x40 && ch <= 0x5f && ch != '[') esc_state = 0; /* 2-byte seq */
			else esc_state = 0;
			continue;
		case 2:
			if (ch >= 0x40 && ch <= 0x7e) esc_state = 0;
			continue;
		case 3:
			if (ch == 7) esc_state = 0;
			else if (ch == 0x1b) esc_state = 4;
			continue;
		case 4:
			esc_state = 0;
			continue;
		}
		if (utf_skip) { if ((ch & 0xc0) == 0x80) { utf_skip--; continue; } utf_skip = 0; }
		if (ch == 0x1b) { esc_state = 1; continue; }
		if (ch == '\n') { grid_newline(); continue; }
		if (ch == '\r') { cur_c = 0; continue; }
		if (ch == '\b') { if (cur_c > 0) cur_c--; continue; }
		if (ch == '\t') { do grid_putc(' '); while (cur_c % 8); continue; }
		if (ch >= 0xc0) { utf_skip = ch >= 0xf0 ? 3 : ch >= 0xe0 ? 2 : 1; grid_putc('?'); continue; }
		if (ch < 32 || ch == 127) continue;
		grid_putc(ch);
	}
}

static void feeds(const char *s) { feed((const unsigned char *)s, strlen(s)); }

/* ------------------------------------------------------------- painting */

static void paint(void)
{
	int cw, ch;
	canvas_size(&cw, &ch);
	int gw = cols * QF_W * scale, gh = rows * QF_H * scale;
	int ox = (cw - gw) / 2, oy = (ch - gh) / 2;
	if (ox < 0) ox = 0;
	if (oy < 0) oy = 0;
	uint32_t white = rgb(255, 255, 255), black = rgb(0, 0, 0);

	for (int eye = 0; eye < 2; eye++) {
		/* clear the whole eye half */
		for (uint32_t y = 0; y < EH; y++) {
			uint32_t *row = (uint32_t *)(fb + (size_t)(y + (eye ? EH : 0)) * pitch);
			for (uint32_t x = 0; x < EW; x++) row[x] = 0xff000000u | black;
		}
		for (int r = 0; r < rows; r++)
			for (int c = 0; c < cols; c++) {
				char g = grid[(size_t)r * cols + c];
				if (g != ' ')
					draw_glyph(eye, ox + c * QF_W * scale, oy + r * QF_H * scale,
						   (unsigned char)g, scale, white, 0, 0);
			}
		if (borders) {
			/* bands are drawn in PHYSICAL panel coordinates, so they show the real orientation */
			int inset = 100, band = 30;
			uint32_t red = rgb(255, 0, 0), grn = rgb(0, 255, 0), blu = rgb(0, 0, 255), wht = white;
			for (int j = 0; j < band; j++)
				for (uint32_t i = inset; i + inset < EW; i++) {
					put_phys(eye, i, inset + j, red);                    /* top    = red   */
					put_phys(eye, i, EH - 1 - inset - j, blu);           /* bottom = blue  */
				}
			for (int j = 0; j < band; j++)
				for (uint32_t i = inset; i + inset < EH; i++) {
					put_phys(eye, inset + j, i, wht);                    /* left   = white */
					put_phys(eye, EW - 1 - inset - j, i, grn);           /* right  = green */
				}
			draw_text(eye, 160, 160, eye ? "E1" : "E0", 4, rgb(255, 255, 0));
		}
	}
	dirty = 0;
}

/* ---------------------------------------------------------------- output */

static void write_ppm(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f) { perror(path); return; }
	/* scale down 4x for a small preview */
	int s = 4;
	fprintf(f, "P6\n%u %u\n255\n", W / s, H / s);
	for (uint32_t y = 0; y < H / s; y++)
		for (uint32_t x = 0; x < W / s; x++) {
			uint32_t p = *(uint32_t *)(fb + (size_t)(y * s) * pitch + (size_t)(x * s) * 4);
			uint8_t r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
			if (swap_rb) { uint8_t t = r; r = b; b = t; }
			fputc(r, f); fputc(g, f); fputc(b, f);
		}
	fclose(f);
}

#ifndef QS_FAKE
static void dump_modes(const struct drm_mode_modeinfo *m, int n)
{
	for (int i = 0; i < n; i++)
		fprintf(stderr, "  mode %2d: %ux%u @%uHz clock=%u flags=0x%x type=0x%x '%s'\n", i,
			m[i].hdisplay, m[i].vdisplay, m[i].vrefresh, m[i].clock, m[i].flags, m[i].type, m[i].name);
}

static void *xcalloc(size_t n, size_t sz) { void *p = calloc(n ? n : 1, sz); if (!p) { perror("calloc"); exit(1); } return p; }

struct fmt_try { const char *name; uint32_t fourcc; int swap; };
static const struct fmt_try fmts[] = {
	{ "xrgb", DRM_FORMAT_XRGB8888, 0 }, { "argb", DRM_FORMAT_ARGB8888, 0 },
	{ "xbgr", DRM_FORMAT_XBGR8888, 1 }, { "abgr", DRM_FORMAT_ABGR8888, 1 },
};
#endif

static void on_term(int sig) { (void)sig; _exit(0); }

int main(int argc, char **argv)
{
	const char *dev = "/dev/dri/card0";
	int list_only = 0, test = 0, mode_idx = -1;
	const char *fmt_want = NULL;
	int opt;
	extern char *optarg;
	while ((opt = getopt(argc, argv, "d:m:r:s:f:c:n:o:Sa:z:p:g:M:lbtqkh")) != -1) {
		switch (opt) {
		case 'd': dev = optarg; break;
		case 'm': mode_idx = atoi(optarg); break;
		case 'r': rot = atoi(optarg); break;
		case 's': scale = atoi(optarg); break;
		case 'f': fmt_want = optarg; break;
		case 'c': cols = atoi(optarg); break;
		case 'n': rows = atoi(optarg); break;
		case 'o': ppm_out = optarg; break;
		case 'l': list_only = 1; break;
		case 'b': borders = 1; break;
		case 't': test = 1; borders = 1; break;
		case 'q': quit_at_eof = 1; break;
		case 'k': kmsg_mode = 1; break;
		case 'S': splash_mode = 1; break;
		case 'a': splash_addr = strtoull(optarg, NULL, 0); break;
		case 'z': splash_size = strtoul(optarg, NULL, 0); break;
		case 'p': user_pitch = (unsigned)atoi(optarg); break;
		case 'g': sscanf(optarg, "%ux%u", &gw, &gh); break;
		case 'M': memdev = optarg; break;
		default:
			fprintf(stderr, "usage: qscreen [-l] [-t] [-d dev] [-m mode] [-r 0|90|180|270] [-s scale] [-f xrgb|argb|xbgr|abgr] [-c cols] [-n rows] [-b] [-q] [-k] [-S [-a phys] [-z size] [-p pitch] [-g WxH] [-M memdev]] [-o out.ppm]\n");
			return 2;
		}
	}
	if (rot % 90 || rot < 0 || rot > 270 || scale < 1 || cols < 1 || rows < 1) { fprintf(stderr, "bad option value\n"); return 2; }
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	signal(SIGPIPE, SIG_IGN);

#ifdef QS_FAKE
	(void)dev; (void)list_only; (void)mode_idx; (void)fmt_want;
	W = 1920; H = 3664; pitch = W * 4; EW = W; EH = H / 2;
	fb = calloc(1, (size_t)pitch * H);
#else
	if (splash_mode) {
		/* no DRM: draw into the framebuffer the bootloader left scanning out (Meta logo) */
		int mfd = open(memdev, O_RDWR | O_SYNC | O_CLOEXEC);
		if (mfd < 0) { perror(memdev); fprintf(stderr, "(the kernel needs CONFIG_DEVMEM for /dev/mem)\n"); return 1; }
		W = gw ? gw : 1920; H = gh ? gh : 3664;
		pitch = user_pitch ? user_pitch : W * 4;
		if (splash_size < (size_t)pitch * H)
			fprintf(stderr, "qscreen: warning: %zu bytes is smaller than pitch*height = %zu\n", splash_size, (size_t)pitch * H);
		fb = mmap(NULL, splash_size, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, (off_t)splash_addr);
		if (fb == MAP_FAILED) { perror("mmap framebuffer"); return 1; }
		fprintf(stderr, "qscreen: splash framebuffer mode: %s @ 0x%llx, %zu bytes, %ux%u pitch %u\n",
			memdev, splash_addr, splash_size, W, H, pitch);
		if (fmt_want)
			for (size_t i = 0; i < sizeof fmts / sizeof fmts[0]; i++)
				if (!strcmp(fmt_want, fmts[i].name)) swap_rb = fmts[i].swap;
		EW = W; EH = H / 2;
		goto have_fb;
	}
	fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror(dev); return 1; }
	xioctl(fd, DRM_IOCTL_SET_MASTER, NULL); /* may fail if not needed */

	struct drm_mode_card_res res; memset(&res, 0, sizeof res);
	if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) { perror("GETRESOURCES"); return 1; }
	uint32_t *crtcs = xcalloc(res.count_crtcs, 4), *conns = xcalloc(res.count_connectors, 4), *encs = xcalloc(res.count_encoders, 4);
	uint32_t *fbs = xcalloc(res.count_fbs, 4);
	res.crtc_id_ptr = (uintptr_t)crtcs; res.connector_id_ptr = (uintptr_t)conns;
	res.encoder_id_ptr = (uintptr_t)encs; res.fb_id_ptr = (uintptr_t)fbs;
	if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) { perror("GETRESOURCES(2)"); return 1; }
	fprintf(stderr, "qscreen: %u crtcs, %u connectors, %u encoders, max %ux%u\n",
		res.count_crtcs, res.count_connectors, res.count_encoders, res.max_width, res.max_height);

	struct drm_mode_modeinfo *modes = NULL; int nmodes = 0;
	uint32_t conn_id = 0, enc_id = 0, crtc_id = 0, crtc_mask = 0;
	for (uint32_t i = 0; i < res.count_connectors; i++) {
		struct drm_mode_get_connector c; memset(&c, 0, sizeof c);
		c.connector_id = conns[i];
		if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0) { perror("GETCONNECTOR"); continue; }
		uint32_t *ce = xcalloc(c.count_encoders, 4), *cp = xcalloc(c.count_props, 4);
		uint64_t *cv = xcalloc(c.count_props, 8);
		struct drm_mode_modeinfo *cm = xcalloc(c.count_modes, sizeof *cm);
		uint32_t want_modes = c.count_modes;
		c.encoders_ptr = (uintptr_t)ce; c.props_ptr = (uintptr_t)cp; c.prop_values_ptr = (uintptr_t)cv;
		c.modes_ptr = (uintptr_t)cm;
		if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0) { perror("GETCONNECTOR(2)"); continue; }
		fprintf(stderr, "connector %u: type=%u(%u) connection=%u modes=%u encoder=%u size=%ux%umm\n",
			c.connector_id, c.connector_type, c.connector_type_id, c.connection, c.count_modes,
			c.encoder_id, c.mm_width, c.mm_height);
		if (list_only) dump_modes(cm, c.count_modes < want_modes ? c.count_modes : want_modes);
		/* DSI connector (type 16), connected, with modes */
		if (!conn_id && c.connector_type == DRM_MODE_CONNECTOR_DSI && c.connection == 1 && c.count_modes > 0) {
			conn_id = c.connector_id;
			enc_id = c.encoder_id ? c.encoder_id : (c.count_encoders ? ce[0] : 0);
			nmodes = c.count_modes < want_modes ? c.count_modes : want_modes;
			modes = cm;
		}
	}
	if (list_only) return 0;
	if (!conn_id || !modes) { fprintf(stderr, "qscreen: no connected DSI connector with modes\n"); return 1; }
	if (!enc_id) { fprintf(stderr, "qscreen: connector has no encoder\n"); return 1; }

	struct drm_mode_get_encoder e; memset(&e, 0, sizeof e); e.encoder_id = enc_id;
	if (xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e) < 0) { perror("GETENCODER"); return 1; }
	crtc_mask = e.possible_crtcs;
	crtc_id = e.crtc_id;
	if (!crtc_id)
		for (uint32_t i = 0; i < res.count_crtcs; i++)
			if (crtc_mask & (1u << i)) { crtc_id = crtcs[i]; break; }
	fprintf(stderr, "qscreen: connector %u encoder %u crtc %u (possible mask 0x%x)\n", conn_id, enc_id, crtc_id, crtc_mask);
	if (!crtc_id) { fprintf(stderr, "qscreen: no usable crtc\n"); return 1; }

	if (mode_idx < 0) {
		mode_idx = 0;
		for (int i = 0; i < nmodes; i++) if (modes[i].type & DRM_MODE_TYPE_PREFERRED) { mode_idx = i; break; }
	}
	if (mode_idx >= nmodes) { fprintf(stderr, "qscreen: mode %d out of range (have %d)\n", mode_idx, nmodes); return 1; }
	struct drm_mode_modeinfo mode = modes[mode_idx];
	fprintf(stderr, "qscreen: using mode %d: %ux%u@%u '%s'\n", mode_idx, mode.hdisplay, mode.vdisplay, mode.vrefresh, mode.name);

	W = mode.hdisplay; H = mode.vdisplay;
	struct drm_mode_create_dumb cd; memset(&cd, 0, sizeof cd);
	cd.width = W; cd.height = H; cd.bpp = 32;
	if (xioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) { perror("CREATE_DUMB"); return 1; }
	pitch = cd.pitch;
	struct drm_mode_map_dumb md; memset(&md, 0, sizeof md); md.handle = cd.handle;
	if (xioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) { perror("MAP_DUMB"); return 1; }
	fb = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
	if (fb == MAP_FAILED) { perror("mmap dumb"); return 1; }
	fprintf(stderr, "qscreen: dumb buffer handle %u pitch %u size %llu\n", cd.handle, cd.pitch, (unsigned long long)cd.size);
	EW = W; EH = H / 2;
#endif

#ifndef QS_FAKE
have_fb:
#endif
	grid = malloc((size_t)rows * cols);
	if (!grid) return 1;
	grid_clear();
	if (test) {
		char line[128];
		feeds("QUEST 2 DISPLAY TEST CARD\n");
		snprintf(line, sizeof line, "panel %ux%u  text rotation %d  scale %d\n", W, H, rot, scale);
		feeds(line);
		feeds("Physical edge bands: TOP red, BOTTOM blue,\n");
		feeds("LEFT white, RIGHT green (panel coordinates).\n");
		feeds("E0/E1 = top/bottom half of the panel.\n");
		feeds("If this text is sideways or mirrored, rerun\nwith -r 90, -r 180 or -r 270.\n\n");
		feeds("0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ\nabcdefghijklmnopqrstuvwxyz !\"#$%&'()*+,-./:;<=>?@\n");
	}
	paint();

#ifndef QS_FAKE
	/* scan out: try each pixel format until the CRTC accepts it */
	int ok = splash_mode; /* nothing to scan out in splash mode */
	for (size_t i = 0; i < sizeof fmts / sizeof fmts[0] && !ok; i++) {
		if (fmt_want && strcmp(fmt_want, fmts[i].name)) continue;
		struct drm_mode_fb_cmd2 f2; memset(&f2, 0, sizeof f2);
		f2.width = W; f2.height = H; f2.pixel_format = fmts[i].fourcc;
		f2.handles[0] = cd.handle; f2.pitches[0] = cd.pitch;
		if (xioctl(fd, DRM_IOCTL_MODE_ADDFB2, &f2) < 0) {
			fprintf(stderr, "qscreen: ADDFB2 %s: %s\n", fmts[i].name, strerror(errno));
			continue;
		}
		struct drm_mode_crtc sc; memset(&sc, 0, sizeof sc);
		uint32_t cl = conn_id;
		sc.set_connectors_ptr = (uintptr_t)&cl; sc.count_connectors = 1;
		sc.crtc_id = crtc_id; sc.fb_id = f2.fb_id; sc.mode_valid = 1; sc.mode = mode;
		swap_rb = fmts[i].swap;
		paint(); /* repaint with this format's channel order */
		if (xioctl(fd, DRM_IOCTL_MODE_SETCRTC, &sc) < 0) {
			fprintf(stderr, "qscreen: SETCRTC with %s failed: %s\n", fmts[i].name, strerror(errno));
			xioctl(fd, DRM_IOCTL_MODE_RMFB, &f2.fb_id);
			continue;
		}
		fprintf(stderr, "qscreen: SETCRTC ok, format %s, fb %u\n", fmts[i].name, f2.fb_id);
		ok = 1;
	}
	if (!ok) { fprintf(stderr, "qscreen: could not set the mode (see dmesg for sde/dsi messages)\n"); return 1; }
#endif

	/* main loop: read stdin (or /dev/kmsg with -k), repaint at most every 100 ms */
	int infd = 0;
	int in_open = !isatty(0) && !test;
	if (kmsg_mode) {
		infd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (infd < 0) { perror("/dev/kmsg"); return 1; }
		in_open = 1;
	}
	unsigned char buf[8192];
	for (;;) {
		struct pollfd p = { .fd = infd, .events = POLLIN };
		int to = dirty ? 100 : (kmsg_mode && quit_at_eof ? 300 : -1);
		if (!in_open) {
			if (quit_at_eof) break;
			if (dirty) paint();
			pause();
			continue;
		}
		int r = poll(&p, 1, to);
		if (r > 0 && (p.revents & (POLLIN | POLLHUP))) {
			ssize_t n = read(infd, buf, sizeof buf - 1);
			if (kmsg_mode) {
				if (n > 0) {
					/* one record: "pri,seq,ts,flags;text\n" optionally followed by " KEY=VAL" lines */
					buf[n] = 0;
					unsigned char *semi = memchr(buf, ';', (size_t)n);
					if (semi) {
						unsigned char *nl = memchr(semi, '\n', (size_t)(buf + n - semi));
						feed(semi + 1, (size_t)((nl ? nl : buf + n) - semi - 1));
						feed((const unsigned char *)"\n", 1);
					}
				} else if (n < 0 && errno != EAGAIN && errno != EPIPE) {
					if (errno == EINTR) continue;
					perror("kmsg read");
				}
				continue;
			}
			if (n > 0) feed(buf, (size_t)n);
			else if (n == 0) { in_open = 0; if (dirty) paint(); if (quit_at_eof) break; }
		} else if (r == 0 && dirty) {
			paint();
		} else if (r == 0 && kmsg_mode && quit_at_eof) {
			break; /* log drained (test mode) */
		}
	}
	if (dirty) paint();
	if (ppm_out) write_ppm(ppm_out);
	return 0;
}
