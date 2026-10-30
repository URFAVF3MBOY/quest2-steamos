// shared freestanding runtime + camera helpers (from q2cam_tpg.c)
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <media/cam_req_mgr.h>
#include <media/cam_defs.h>
#include <media/cam_isp.h>
#include <media/cam_isp_ife.h>
#include <media/cam_cpas.h>
#include <media/cam_fastpath_uapi.h>

// ---------------------------------------------------------------- tiny runtime (aarch64 Linux syscalls)
static long sc6(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c,
		x3 __asm__("x3") = d, x4 __asm__("x4") = e, x5 __asm__("x5") = f;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
	return x0;
}
#define SYS_openat 56
#define SYS_close 57
#define SYS_ioctl 29
#define SYS_write 64
#define SYS_mmap 222
#define SYS_munmap 215
#define SYS_ppoll 73
#define SYS_exit_group 94
#define SYS_clock_gettime 113
#define SYS_getdents64 61
#define SYS_nanosleep 101
static int sys_open(const char *p, int fl) { return (int)sc6(SYS_openat, -100, (long)p, fl, 0644, 0, 0); }
static int sys_close(int fd) { return (int)sc6(SYS_close, fd, 0, 0, 0, 0, 0); }
static long sys_ioctl(int fd, unsigned long req, void *arg) { return sc6(SYS_ioctl, fd, (long)req, (long)arg, 0, 0, 0); }
static long sys_write(int fd, const void *b, size_t n) { return sc6(SYS_write, fd, (long)b, (long)n, 0, 0, 0); }
static void *sys_mmap(size_t len, int prot, int fl, int fd) { return (void *)sc6(SYS_mmap, 0, (long)len, prot, fl, fd, 0); }
static void sys_munmap(void *p, size_t len) { sc6(SYS_munmap, (long)p, (long)len, 0, 0, 0, 0); }
static void sys_exit(int c) { sc6(SYS_exit_group, c, 0, 0, 0, 0, 0); for (;;) {} }
struct ts { long s, ns; };
static double now(void) { struct ts t; sc6(SYS_clock_gettime, 0 /*REALTIME*/, (long)&t, 0, 0, 0, 0); return t.s + t.ns / 1e9; }
static void sleep_ms(long ms) { struct ts t = { ms / 1000, (ms % 1000) * 1000000 }; sc6(SYS_nanosleep, (long)&t, 0, 0, 0, 0, 0); }
void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static char obuf[512];
static int logfd = -1;
static void put(char **o, char *e, char c) { if (*o < e) *(*o)++ = c; }
static void putnum(char **o, char *e, unsigned long long v, int base, int neg)
{
	char t[24]; int n = 0;
	if (neg) put(o, e, '-');
	do { t[n++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
	while (n) put(o, e, t[--n]);
}
static void P(const char *fmt, ...)
{
	char *o = obuf, *e = obuf + sizeof obuf - 2; va_list ap; va_start(ap, fmt);
	double t = now(); long ip = (long)t; long fr = (long)((t - ip) * 1000);
	const char *pre = TAG; while (*pre) put(&o, e, *pre++);
	putnum(&o, e, (unsigned long long)ip, 10, 0); put(&o, e, '.');
	put(&o, e, '0' + fr / 100); put(&o, e, '0' + fr / 10 % 10); put(&o, e, '0' + fr % 10); put(&o, e, ' ');
	for (; *fmt; fmt++) {
		if (*fmt != '%') { put(&o, e, *fmt); continue; }
		fmt++; int ll = 0; while (*fmt == 'l') { ll++; fmt++; }
		switch (*fmt) {
		case 'd': { long long v = ll ? va_arg(ap, long long) : va_arg(ap, int); putnum(&o, e, v < 0 ? -v : v, 10, v < 0); break; }
		case 'u': { unsigned long long v = ll ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned); putnum(&o, e, v, 10, 0); break; }
		case 'x': { unsigned long long v = ll ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned); put(&o, e, '0'); put(&o, e, 'x'); putnum(&o, e, v, 16, 0); break; }
		case 's': { const char *s = va_arg(ap, const char *); if (!s) s = "(null)"; while (*s) put(&o, e, *s++); break; }
		default: put(&o, e, *fmt);
		}
	}
	va_end(ap);
	*o++ = '\n';
	sys_write(1, obuf, o - obuf);
	if (logfd >= 0) sys_write(logfd, obuf, o - obuf);
}
static const char *errname(long r)
{
	switch (-r) { case 1: return "EPERM"; case 2: return "ENOENT"; case 11: return "EAGAIN"; case 12: return "ENOMEM";
	case 14: return "EFAULT"; case 16: return "EBUSY"; case 19: return "ENODEV"; case 22: return "EINVAL"; case 25: return "ENOTTY";
	case 110: return "ETIMEDOUT"; case 114: return "EALREADY"; default: return "error"; }
}

// ---------------------------------------------------------------- camera control helpers
static long cam_ctl(int fd, uint32_t op, void *payload, uint32_t size)
{
	struct cam_control c; memset(&c, 0, sizeof c);
	c.op_code = op; c.size = size; c.handle_type = CAM_HANDLE_USER_POINTER; c.handle = (uint64_t)(uintptr_t)payload;
	return sys_ioctl(fd, VIDIOC_CAM_CONTROL, &c);
}
// find /dev/v4l-subdevN (or /dev/videoN) whose sysfs name matches
static int find_node(const char *cls, const char *prefix, const char *want, char *path)
{
	char p[96], nm[64];
	for (int i = 0; i < 40; i++) {
		char *o = p; const char *a = "/sys/class/"; while (*a) *o++ = *a++; a = cls; while (*a) *o++ = *a++;
		*o++ = '/'; a = prefix; while (*a) *o++ = *a++;
		if (i >= 10) *o++ = '0' + i / 10; *o++ = '0' + i % 10; a = "/name"; while (*a) *o++ = *a++; *o = 0;
		int fd = sys_open(p, 0); if (fd < 0) continue;
		long n = sc6(63 /*read*/, fd, (long)nm, sizeof nm - 1, 0, 0, 0); sys_close(fd);
		if (n <= 0) continue; nm[n] = 0; if (nm[n - 1] == '\n') nm[n - 1] = 0;
		if (seq(nm, want)) {
			o = path; a = "/dev/"; while (*a) *o++ = *a++; a = prefix; while (*a) *o++ = *a++;
			if (i >= 10) *o++ = '0' + i / 10; *o++ = '0' + i % 10; *o = 0;
			return 0;
		}
	}
	return -1;
}

// like find_node, but only checks node number i
static int find_node_n(const char *cls, const char *prefix, const char *want, char *path, int i)
{
	char p[96], nm[64]; char *o = p; const char *a = "/sys/class/"; while (*a) *o++ = *a++; a = cls; while (*a) *o++ = *a++;
	*o++ = '/'; a = prefix; while (*a) *o++ = *a++;
	if (i >= 10) *o++ = '0' + i / 10; *o++ = '0' + i % 10; a = "/name"; while (*a) *o++ = *a++; *o = 0;
	int fd = sys_open(p, 0); if (fd < 0) return -1;
	long n = sc6(63, fd, (long)nm, sizeof nm - 1, 0, 0, 0); sys_close(fd);
	if (n <= 0) return -1; nm[n] = 0; if (nm[n - 1] == '\n') nm[n - 1] = 0;
	if (!seq(nm, want)) return -1;
	o = path; a = "/dev/"; while (*a) *o++ = *a++; a = prefix; while (*a) *o++ = *a++;
	if (i >= 10) *o++ = '0' + i / 10; *o++ = '0' + i % 10; *o = 0;
	return 0;
}

struct buf { int32_t hdl; int fd; uint64_t len; void *map; };
static long alloc_buf(int crm, struct buf *b, uint64_t len, uint32_t flags, const int32_t *mmu, int nmmu, const char *what)
{
	struct cam_mem_mgr_alloc_cmd a; memset(&a, 0, sizeof a);
	a.len = len; a.align = 4096; a.flags = flags; a.num_hdl = nmmu;
	for (int i = 0; i < nmmu; i++) a.mmu_hdls[i] = mmu[i];
	long r = cam_ctl(crm, CAM_REQ_MGR_ALLOC_BUF, &a, sizeof a);
	if (r) { P("ALLOC_BUF %s (%llu bytes, flags %x, %d mmu) failed: %d %s", what, (unsigned long long)len, flags, nmmu, (int)r, errname(r)); return r; }
	b->hdl = a.out.buf_handle; b->fd = a.out.fd; b->len = len;
	b->map = sys_mmap(len, 3 /*RW*/, 1 /*SHARED*/, b->fd);
	if ((long)b->map < 0 && (long)b->map > -4096) { P("mmap of %s failed: %d", what, (int)(long)b->map); b->map = 0; }
	if (what || !b->map) P("ALLOC_BUF %s ok: handle %x fd %d, %llu bytes, cpu %s", what ? what : "", (unsigned)b->hdl, b->fd, (unsigned long long)len, b->map ? "mapped" : "NOT mapped");
	return 0;
}
static void free_buf(int crm, struct buf *b, const char *what)
{
	if (!b->hdl) return;
	if (b->map) sys_munmap(b->map, b->len);
	if (b->fd > 0) sys_close(b->fd);
	struct cam_mem_mgr_release_cmd rc; memset(&rc, 0, sizeof rc); rc.buf_handle = b->hdl;
	long r = cam_ctl(crm, CAM_REQ_MGR_RELEASE_BUF, &rc, sizeof rc);
	if (what || r) P("RELEASE_BUF %s: %s", what ? what : "", r ? errname(r) : "ok");
	b->hdl = 0;
}

// blob header: type in bits 7:0, size in bits 31:8; payload padded to 4 bytes
static uint32_t *add_blob(uint32_t *p, uint32_t type, const void *data, uint32_t size)
{
	*p++ = (type & 0xFF) | (size << 8);
	memcpy(p, data, size);
	return p + (size + 3) / 4;
}
