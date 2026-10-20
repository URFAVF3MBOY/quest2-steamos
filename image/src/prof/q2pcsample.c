// q2pcsample <tid> <samples> <interval_us> - samples one thread's program counter and return address (x30) with
// ptrace: SEIZE once, then INTERRUPT / read registers / CONT per sample (the thread stops for tens of microseconds),
// DETACH at the end.  Prints "pc lr" in hex per sample.  desk-287 (VR render path profiler).  Freestanding aarch64.
typedef unsigned long u64; typedef long i64;
static i64 sc(i64 n, i64 a, i64 b, i64 c, i64 d)
{
	register i64 x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c, x3 __asm__("x3") = d;
	__asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
	return x0;
}
#define SYS_write 64
#define SYS_openat 56
#define SYS_pread64 67
#define SYS_exit 93
#define SYS_nanosleep 101
#define SYS_ptrace 117
#define SYS_wait4 260
#define PTRACE_CONT 7
#define PTRACE_DETACH 17
#define PTRACE_GETREGSET 0x4204
#define PTRACE_SEIZE 0x4206
#define PTRACE_INTERRUPT 0x4207
#define WALL 0x40000000
struct regs { u64 x[31], sp, pc, pstate; };
struct iov { void *b; u64 l; };
struct ts { i64 s, ns; };
static i64 num(const char *s) { i64 v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v; }
static char ob[8192]; static int on;
static void flush(void) { if (on) sc(SYS_write, 1, (i64)ob, on, 0); on = 0; }
static void hex(u64 v) { char t[17]; int i = 16; t[i] = 0; do { t[--i] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v); ob[on++] = '0'; ob[on++] = 'x'; while (t[i]) ob[on++] = t[i++]; }
static void run(i64 argc, char **argv)
{
	if (argc < 4) sc(SYS_exit, 2, 0, 0, 0);
	i64 tid = num(argv[1]), n = num(argv[2]), us = num(argv[3]);
	if (sc(SYS_ptrace, PTRACE_SEIZE, tid, 0, 0) < 0) sc(SYS_exit, 1, 0, 0, 0);
	char mp[48] = "/proc/"; int k = 6; { char t[20]; int m = 0; i64 v = tid; do { t[m++] = '0' + v % 10; v /= 10; } while (v); while (m) mp[k++] = t[--m]; }
	const char *sfx = "/mem"; while (*sfx) mp[k++] = *sfx++; mp[k] = 0;
	i64 memfd = sc(SYS_openat, -100, (i64)mp, 0, 0);
	for (i64 i = 0; i < n; i++) {
		int st = 0;
		if (sc(SYS_ptrace, PTRACE_INTERRUPT, tid, 0, 0) < 0) break;
		if (sc(SYS_wait4, tid, (i64)&st, WALL, 0) != tid) break;
		if ((st & 0x7f) != 0x7f) break;   // exited / killed
		struct regs r; struct iov v = { &r, sizeof r };
		if (sc(SYS_ptrace, PTRACE_GETREGSET, tid, 1, (i64)&v) == 0) {
			hex(r.pc); ob[on++] = ' '; hex(r.x[30]);
			// desk-289: stopped in (or right at) ppoll: its fds, what the LAST call reported ready (revents), its timeout
			if (r.x[8] == 73 && memfd >= 0 && r.x[1] > 0 && r.x[1] <= 32) {
				short pf[4 * 32]; long ts[2];
				ob[on++] = ' '; ob[on++] = 'P';
				if (r.x[2] && sc(SYS_pread64, memfd, (i64)ts, 16, (i64)r.x[2]) == 16) { ob[on++] = ' '; ob[on++] = 't'; hex((u64)(ts[0] * 1000 + ts[1] / 1000000)); }
				else { ob[on++] = ' '; ob[on++] = 't'; ob[on++] = '-'; }
				if (sc(SYS_pread64, memfd, (i64)pf, 8 * r.x[1], (i64)r.x[0]) == (i64)(8 * r.x[1]))
					for (u64 j = 0; j < r.x[1]; j++) { ob[on++] = ' '; hex((u64)*(int *)&pf[4 * j]); ob[on++] = ':'; hex((u64)(unsigned short)pf[4 * j + 3]); }
			}
			ob[on++] = '\n';
		}
		// a signal-delivery stop must pass its signal on; an INTERRUPT (event) stop passes none
		i64 sig = (st >> 16) ? 0 : ((st >> 8) & 0x7f);
		sc(SYS_ptrace, PTRACE_CONT, tid, 0, sig);
		if (on > 7000) flush();
		struct ts t = { us / 1000000, (us % 1000000) * 1000 }; sc(SYS_nanosleep, (i64)&t, 0, 0, 0);
	}
	flush();
	sc(SYS_ptrace, PTRACE_DETACH, tid, 0, 0);
	sc(SYS_exit, 0, 0, 0, 0);
}
__attribute__((naked, noreturn)) void _start(void)
{
	__asm__ volatile("ldr x0, [sp]\n add x1, sp, #8\n b run_entry\n");
}
void run_entry(i64 argc, char **argv) { run(argc, argv); }
