// libq2island.so - LD_PRELOADed into every Meta program the island test starts (desk-226).
//  1. logging: Android's log daemon is not running, so liblog's entry points are replaced and every log line goes to
//     the program's stderr (the launcher saves one file per program).
//  2. SELinux: SteamOS has no SELinux policy loaded; the service managers would refuse to start.  The few libselinux
//     calls they make are answered with "allowed".
//  3. FIRMWARE BLOCK: libsyncboss's controller firmware-update entry points are replaced by functions that refuse,
//     print Q2ISLAND-BLOCKED-FW (the watchdog stops everything when it sees it) and return an error.
typedef unsigned long size_t; typedef __builtin_va_list va_list;
#define va_start __builtin_va_start
#define va_end __builtin_va_end
int vsnprintf(char *, size_t, const char *, va_list); long write(int, const void *, size_t); char *strdup(const char *);
size_t strlen(const char *); void free(void *); int getpid(void);
static const char P[] = "VDIWEFS";
static void emit(int prio, const char *tag, const char *msg)
{
	char b[1200]; size_t n = 0;
	char c = prio >= 2 && prio <= 8 ? P[prio - 2] : '?';
	b[n++] = c; b[n++] = ' ';
	if (tag) for (const char *t = tag; *t && n < 80; t++) b[n++] = *t;
	b[n++] = ':'; b[n++] = ' ';
	if (msg) for (const char *m = msg; *m && n < sizeof b - 2; m++) b[n++] = *m;
	if (n && b[n - 1] != '\n') b[n++] = '\n';
	write(2, b, n);
}
int __android_log_write(int prio, const char *tag, const char *msg) { emit(prio, tag, msg); return 1; }
int __android_log_buf_write(int buf, int prio, const char *tag, const char *msg) { (void)buf; emit(prio, tag, msg); return 1; }
int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) { char m[1024]; vsnprintf(m, sizeof m, fmt, ap); emit(prio, tag, m); return 1; }
int __android_log_print(int prio, const char *tag, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = __android_log_vprint(prio, tag, fmt, ap); va_end(ap); return r; }
int __android_log_buf_print(int buf, int prio, const char *tag, const char *fmt, ...) { (void)buf; va_list ap; va_start(ap, fmt); int r = __android_log_vprint(prio, tag, fmt, ap); va_end(ap); return r; }
struct lmsg { size_t struct_size; int buffer_id; int priority; const char *tag; const char *file; unsigned line; const char *message; };
void __android_log_write_log_message(struct lmsg *m) { if (m) emit(m->priority, m->tag, m->message); }
void __android_log_logd_logger(const struct lmsg *m) { if (m) emit(m->priority, m->tag, m->message); }
void __android_log_stderr_logger(const struct lmsg *m) { if (m) emit(m->priority, m->tag, m->message); }
int __android_log_is_loggable(int prio, const char *tag, int def) { (void)tag; (void)def; return prio >= 3; }
int __android_log_is_loggable_len(int prio, const char *tag, size_t len, int def) { (void)tag; (void)len; (void)def; return prio >= 3; }

// --- SELinux: answer "allowed" ---
static char CTX[] = "u:r:su:s0";
int is_selinux_enabled(void) { return 0; }
int security_getenforce(void) { return 0; }
int getcon(char **c) { *c = strdup(CTX); return 0; }
int getprevcon(char **c) { *c = strdup(CTX); return 0; }
int getpidcon(int pid, char **c) { (void)pid; *c = strdup(CTX); return 0; }
int getpeercon(int fd, char **c) { (void)fd; *c = strdup(CTX); return 0; }
void freecon(char *c) { free(c); }
int selinux_check_access(const char *a, const char *b, const char *c, const char *d, void *e) { (void)a; (void)b; (void)c; (void)d; (void)e; return 0; }
int selinux_status_open(int fallback) { (void)fallback; return 0; }
int selinux_status_updated(void) { return 0; }
void selinux_status_close(void) {}
static int dummyhandle;
void *selinux_android_service_context_handle(void) { return &dummyhandle; }
void *selinux_android_hw_service_context_handle(void) { return &dummyhandle; }
void *selinux_android_vendor_service_context_handle(void) { return &dummyhandle; }
void *selinux_android_file_context_handle(void) { return &dummyhandle; }
int selabel_lookup(void *h, char **c, const char *key, int type) { (void)h; (void)key; (void)type; *c = strdup("u:object_r:default_android_service:s0"); return 0; }
void selabel_close(void *h) { (void)h; }
void selinux_set_callback(int type, void *cb) { (void)type; (void)cb; }
int selinux_android_restorecon(const char *p, unsigned f) { (void)p; (void)f; return 0; }
int setexeccon(const char *c) { (void)c; return 0; }
int setfscreatecon(const char *c) { (void)c; return 0; }
int setsockcreatecon(const char *c) { (void)c; return 0; }

// --- FIRMWARE BLOCK (controllers): refuse, shout, fail ---
static long blocked(const char *what)
{
	const char *a = "Q2ISLAND-BLOCKED-FW: refused "; write(2, a, strlen(a)); write(2, what, strlen(what)); write(2, "\n", 1);
	return -1;
}
long syncboss_input_update_firmware(void) { return blocked("syncboss_input_update_firmware"); }
long syncboss_internal_input_force_spl_update(void) { return blocked("syncboss_internal_input_force_spl_update"); }
long syncboss_internal_input_force_peripheral_spl_update(void) { return blocked("syncboss_internal_input_force_peripheral_spl_update"); }
long syncboss_internal_input_force_peripheral_app_update(void) { return blocked("syncboss_internal_input_force_peripheral_app_update"); }

// --- 4. Android system properties (desk-227) -------------------------------------------------------------------
// Android's property service (init) is not running.  Every program in the island shares one plain text file,
// /q2/props ("name=value" lines, prepared by the launcher from Meta's build.prop files), through these replacements
// of bionic's property functions.  Set = rewrite the file (locked, atomic rename); wait = poll it.
int open(const char *, int, ...); long read(int, void *, size_t); int close(int); int rename(const char *, const char *);
int flock(int, int); int nanosleep(const void *, void *); int clock_gettime(int, void *);
struct ts { long s, ns; };
typedef struct prop_info { char name[100]; char value[92]; unsigned serial; } prop_info;
#define NP 4096
static prop_info E[NP]; static int NE; static unsigned gserial = 2; static long lastload = -1000000000L;
static const char PF[] = "/q2/props", PT[] = "/q2/props.tmp";
static long now_ms(void) { struct ts t; clock_gettime(1, &t); return t.s * 1000 + t.ns / 1000000; }
static int scmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
static void scpy(char *d, const char *s, int n) { int i = 0; for (; s[i] && i < n - 1; i++) d[i] = s[i]; d[i] = 0; }
static prop_info *lookup(const char *n) { for (int i = 0; i < NE; i++) if (!scmp(E[i].name, n)) return &E[i]; return 0; }
static char FB[1 << 18];
static void put(const char *n, const char *v)
{
	prop_info *p = lookup(n);
	if (!p) { if (NE >= NP) return; p = &E[NE++]; scpy(p->name, n, sizeof p->name); p->value[0] = 0; p->serial = 0; }
	if (scmp(p->value, v)) { scpy(p->value, v, sizeof p->value); p->serial += 2; gserial += 2; }
}
static void load(int force)
{
	long t = now_ms(); if (!force && t - lastload < 50) return; lastload = t;
	int fd = open(PF, 0); if (fd < 0) return;
	long n = 0, r; while (n < (long)sizeof FB - 1 && (r = read(fd, FB + n, sizeof FB - 1 - n)) > 0) n += r; close(fd); FB[n] = 0;
	char *s = FB;
	while (*s) {
		char *e = s; while (*e && *e != '\n') e++; char c = *e; *e = 0;
		if (*s && *s != '#') { char *q = s; while (*q && *q != '=') q++; if (*q == '=') { *q = 0; put(s, q + 1); } }
		if (!c) break; s = e + 1;
	}
}
static char seen[64][100]; static int nseen;
static void note(const char *what, const char *name)
{
	for (int i = 0; i < nseen; i++) if (!scmp(seen[i], name)) return;
	if (nseen < 64) scpy(seen[nseen++], name, 100);
	char m[160]; int k = 0; for (const char *a = what; *a; a++) m[k++] = *a; for (const char *a = name; *a && k < 158; a++) m[k++] = *a; m[k] = 0;
	emit(4, "q2props", m);
}
static void te_patch(void);
const prop_info *__system_property_find(const char *name) { te_patch(); load(0); prop_info *p = lookup(name); if (!p) note("missing ", name); return p; }
int __system_property_get(const char *name, char *value) { te_patch(); load(0); prop_info *p = lookup(name); if (!p) { note("missing ", name); value[0] = 0; return 0; } scpy(value, p->value, 92); return (int)strlen(value); }
void __system_property_read_callback(const prop_info *p, void (*cb)(void *, const char *, const char *, unsigned), void *ck) { if (p && cb) cb(ck, p->name, p->value, p->serial); }
int __system_property_read(const prop_info *p, char *name, char *value) { if (!p) return 0; if (name) scpy(name, p->name, 32); scpy(value, p->value, 92); return (int)strlen(value); }
unsigned __system_property_serial(const prop_info *p) { load(0); return p ? p->serial : 0; }
unsigned __system_property_area_serial(void) { load(0); return gserial; }
int __system_property_foreach(void (*fn)(const prop_info *, void *), void *ck) { load(0); for (int i = 0; i < NE; i++) fn(&E[i], ck); return 0; }
int __system_property_wait(const prop_info *p, unsigned old, unsigned *newp, const struct ts *rel)
{
	note("waiting for ", p ? p->name : "(any property)");
	long end = rel ? now_ms() + rel->s * 1000 + rel->ns / 1000000 : -1;
	for (;;) {
		load(1);
		unsigned cur = p ? p->serial : gserial;
		if (cur != old) { if (newp) *newp = cur; return 1; }
		if (end >= 0 && now_ms() >= end) return 0;
		struct ts d = { 0, 50000000 }; nanosleep(&d, 0);
	}
}
unsigned __system_property_wait_any(unsigned old) { unsigned n = old; __system_property_wait(0, old, &n, 0); return n; }
int __system_property_set(const char *name, const char *value)
{
	if (!name || !value) return -1;
	if (name[0] == 'c' && name[1] == 't' && name[2] == 'l' && name[3] == '.') {
		// init control request (start a service): queued for the launcher, which only starts allow-listed services
		int q = open("/q2/ctl.queue", 01 | 0100 | 02000, 0666);
		if (q >= 0) { write(q, name, strlen(name)); write(q, "=", 1); write(q, value, strlen(value)); write(q, "\n", 1); close(q); }
		return 0;
	}
	int lk = open("/q2/props.lock", 0102, 0644); if (lk >= 0) flock(lk, 2);
	load(1); put(name, value);
	int fd = open(PT, 01 | 0100 | 01000, 0644);
	if (fd >= 0) {
		for (int i = 0; i < NE; i++) { write(fd, E[i].name, strlen(E[i].name)); write(fd, "=", 1); write(fd, E[i].value, strlen(E[i].value)); write(fd, "\n", 1); }
		close(fd); rename(PT, PF);
	}
	if (lk >= 0) { flock(lk, 8); close(lk); }
	char m[200]; int k = 0; for (const char *a = "set "; *a; a++) m[k++] = *a; for (const char *a = name; *a && k < 120; a++) m[k++] = *a; m[k++] = '='; for (const char *a = value; *a && k < 198; a++) m[k++] = *a; m[k] = 0;
	emit(4, "q2props", m);
	return 0;
}

// --- 5. thread priorities (desk-234) ------------------------------------------------------------------------------
// Meta's thread helper aborts if the kernel refuses a priority/scheduler change (SteamOS runs this test inside a
// systemd cgroup, where real-time scheduling is not granted).  Try the real call; if it is refused, say "done".
void *dlsym(void *, const char *);
struct sched_param_ { int prio; };
static long sys3(long n, long a, long b, long c) { register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c; __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory"); return x0; }
static int warned_prio;
static void used(int i, const char *what);
static void prio_note(const char *what) { if (warned_prio++ < 5) { char m[120]; int k = 0; for (const char *a = "refused by the kernel, reported as done: "; *a; a++) m[k++] = *a; for (const char *a = what; *a && k < 118; a++) m[k++] = *a; m[k] = 0; emit(5, "q2sched", m); } }
int setpriority(int which, unsigned who, int prio) { used(10, "setpriority"); long r = sys3(140, which, who, prio); if (r < 0) prio_note("setpriority"); return 0; }
int sched_setscheduler(int pid, int policy, const struct sched_param_ *p) { used(9, "sched_setscheduler"); long r = sys3(119, pid, policy, (long)p); if (r < 0) prio_note("sched_setscheduler"); return 0; }
int pthread_setschedparam(unsigned long t, int policy, const struct sched_param_ *p)
{
	static int (*real)(unsigned long, int, const struct sched_param_ *);
	used(11, "pthread_setschedparam");
	if (!real) *(void **)&real = dlsym((void *)-1L, "pthread_setschedparam");
	int r = real ? real(t, policy, p) : 0;
	if (r) prio_note("pthread_setschedparam");
	return 0;
}

// desk-235: Android's task-profile / scheduling-group helpers (libprocessgroup, libutils).  They need init's cgroup
// setup (/dev/cgroup_info, /dev/stune, ...), which SteamOS does not have.  Try the real one; if it fails, say "done".
typedef int (*f_iPi)(int, const void *, int); typedef int (*f_uiP)(unsigned, int, const void *); typedef int (*f_ii)(int, int); typedef int (*f_iii)(int, int, int);
static void *nxt(const char *n) { return dlsym((void *)-1L, n); }
int SetTaskProfiles(int tid, const void *profiles, int use_cache)
{ static f_iPi r_; used(4, "SetTaskProfiles"); if (!r_) r_ = (f_iPi)nxt("SetTaskProfiles"); int r = r_ ? r_(tid, profiles, use_cache) : 0; if (!(r & 1)) prio_note("SetTaskProfiles"); return 1; }
int SetProcessProfiles(unsigned uid, int pid, const void *profiles)
{ static f_uiP r_; if (!r_) r_ = (f_uiP)nxt("SetProcessProfiles"); int r = r_ ? r_(uid, pid, profiles) : 0; if (!(r & 1)) prio_note("SetProcessProfiles"); return 1; }
int SetProcessProfilesCached(unsigned uid, int pid, const void *profiles)
{ static f_uiP r_; if (!r_) r_ = (f_uiP)nxt("SetProcessProfilesCached"); int r = r_ ? r_(uid, pid, profiles) : 0; if (!(r & 1)) prio_note("SetProcessProfilesCached"); return 1; }
int set_sched_policy(int tid, int policy)
{ used(6, "set_sched_policy"); static f_ii r_; if (!r_) r_ = (f_ii)nxt("set_sched_policy"); int r = r_ ? r_(tid, policy) : 0; if (r) prio_note("set_sched_policy"); return 0; }
int set_cpuset_policy(int tid, int policy)
{ used(7, "set_cpuset_policy"); static f_ii r_; if (!r_) r_ = (f_ii)nxt("set_cpuset_policy"); int r = r_ ? r_(tid, policy) : 0; if (r) prio_note("set_cpuset_policy"); return 0; }
int androidSetThreadPriority(int tid, int prio, int change_policy)
{ used(8, "androidSetThreadPriority"); static f_iii r_; if (!r_) r_ = (f_iii)nxt("androidSetThreadPriority"); int r = r_ ? r_(tid, prio, change_policy) : 0; if (r) prio_note("androidSetThreadPriority"); return 0; }
int sched_setattr(int pid, const void *attr, unsigned flags) { used(5, "sched_setattr"); long r = sys3(274, pid, (long)attr, flags); if (r < 0) prio_note("sched_setattr"); return 0; }
int sched_setparam(int pid, const struct sched_param_ *p) { long r = sys3(118, pid, (long)p, 0); if (r < 0) prio_note("sched_setparam"); return 0; }

// desk-235: the C++ TaskProfiles entry points, and a one-time note of every priority helper that gets used at all
// (so the next log shows which one the tracking engine relies on).
static int usedn[16];
static void used(int i, const char *what) { if (!usedn[i]++) { char m[100]; int k = 0; for (const char *a = "used: "; *a; a++) m[k++] = *a; for (const char *a = what; *a && k < 98; a++) m[k++] = *a; m[k] = 0; emit(4, "q2sched", m); } }
int tp_task(void *self, int tid, const void *profiles, int cache) __asm__("_ZN12TaskProfiles15SetTaskProfilesEiRKNSt3__16vectorINS0_12basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEENS5_IS7_EEEEb");
int tp_task(void *self, int tid, const void *profiles, int cache) { (void)self; (void)tid; (void)profiles; (void)cache; used(0, "TaskProfiles::SetTaskProfiles"); return 1; }
int tp_proc(void *self, unsigned uid, int pid, const void *profiles, int cache) __asm__("_ZN12TaskProfiles18SetProcessProfilesEjiRKNSt3__16vectorINS0_12basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEENS5_IS7_EEEEb");
int tp_proc(void *self, unsigned uid, int pid, const void *profiles, int cache) { (void)self; (void)uid; (void)pid; (void)profiles; (void)cache; used(1, "TaskProfiles::SetProcessProfiles"); return 1; }
int as_tp3(int tid, int prio, int chg) __asm__("_Z24androidSetThreadPriorityiib");
int as_tp3(int tid, int prio, int chg) { used(2, "androidSetThreadPriority(3)"); long r = sys3(140, 0, tid, prio); (void)r; (void)chg; return 0; }
int nice(int inc) { used(3, "nice"); long r = sys3(140, 0, 0, inc); (void)r; return 0; }
// libc's generic syscall(): the same "refused -> done" rule for the scheduling syscalls only; everything else is
// passed straight to the kernel with normal error handling.
int *__errno(void);
long syscall(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c, x3 __asm__("x3") = d, x4 __asm__("x4") = e, x5 __asm__("x5") = f;
	__asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
	long r = x0;
	if (n == 118 || n == 119 || n == 140 || n == 274 || n == 122) { used(n == 274 ? 5 : 9, "syscall(scheduling)"); if (r < 0) { prio_note("syscall(scheduling)"); return 0; } }
	if (r < 0 && r > -4096) { *__errno() = (int)-r; return -1; }
	return r;
}
// desk-236: CPU core pinning.  The tracking engine pins its threads to particular cores per thread type; if the
// kernel refuses (cores offline/unavailable under SteamOS), keep the thread where it is and report success.
int sched_setaffinity(int pid, unsigned long size, const void *mask)
{
	used(12, "sched_setaffinity");
	long r = sys3(122, pid, (long)size, (long)mask);
	if (r < 0) {
		char m[96]; int k = 0; for (const char *a = "core pinning refused (errno "; *a; a++) m[k++] = *a;
		long e = -r; char t[8]; int j = 0; do { t[j++] = '0' + e % 10; e /= 10; } while (e); while (j) m[k++] = t[--j];
		for (const char *a = ", mask "; *a; a++) m[k++] = *a;
		unsigned long v = size >= 8 ? *(const unsigned long *)mask : *(const unsigned char *)mask;
		for (int b = 15; b >= 0; b--) { int d = (v >> (b * 4)) & 15; if (d || b < 2) m[k++] = d < 10 ? '0' + d : 'a' + d - 10; }
		for (const char *a = ") - left unpinned"; *a; a++) m[k++] = *a; m[k] = 0;
		emit(5, "q2sched", m);
	}
	return 0;
}

// desk-237: the tracking engine (libtrackingengines.so, this headset's build only) asks its own priority callback to
// set each worker thread's priority, and aborts the whole tracker if that callback says "no".  Under SteamOS the
// callback says no for the Mapper thread even though the kernel accepted every priority call.  Priorities only
// affect timing, so the abort is turned into "carry on": one conditional branch right after that callback is
// replaced by a no-op IN THIS PROCESS'S MEMORY ONLY.  Nothing on disk is changed.  The patch is applied only when the
// exact expected instructions and the exact error text are found at the expected places (= the same engine build
// that was analysed); any other build is left untouched and logged.
int open(const char *, int, ...); long read(int, void *, size_t); int close(int);
#define TE_CALL 0x8d5e40UL   /* blr x8          (calls the priority callback)            */
#define TE_TEST 0x8d5e44UL   /* tbz w0,#0,fail  (callback said "no" -> abort path)        */
#define TE_MSG  0xa8111UL    /* "Failed to set thread priority for thread type ..."       */
static int te_state, te_tries;   /* 0 = not found yet, 1 = patched, 2 = different build / gave up */
static unsigned long hexval(const char **p) { unsigned long v = 0; for (;;) { char c = **p; int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; if (d < 0) return v; v = v * 16 + d; (*p)++; } }
static unsigned long te_base, te_xlo[8], te_xhi[8]; static int te_nx;
static void te_line(const char *s, const char *e)
{
	const char *lib = "/libtrackingengines.so"; long L = 22;
	if (e - s <= L) return;
	for (long i = 0; i < L; i++) if (e[-L + i] != lib[i]) return;
	const char *p = s; unsigned long a = hexval(&p); p++; unsigned long b = hexval(&p); p++;
	char x = p[2]; p += 5; unsigned long off = hexval(&p);
	if (off == 0 && !te_base) te_base = a;
	if (x == 'x' && te_nx < 8) { te_xlo[te_nx] = a; te_xhi[te_nx] = b; te_nx++; }
}
static int is_tracker(void)
{
	static char cb[256]; int fd = open("/proc/self/cmdline", 0); if (fd < 0) return 0;
	long n = read(fd, cb, sizeof cb - 1); close(fd); if (n <= 0) return 0; cb[n] = 0;
	const char *w = "trackingservice"; for (long i = 0; cb[i]; i++) { int k = 0; while (w[k] && cb[i + k] == w[k]) k++; if (!w[k]) return 1; }
	return 0;
}
static void te_patch(void)
{
	if (te_state || te_tries++ > 40) return;
	/* /proc/self/maps of the tracker is far longer than one buffer: read it line by line in chunks */
	int fd = open("/proc/self/maps", 0);
	if (fd < 0) { if (te_tries == 1 && is_tracker()) emit(5, "q2engine", "cannot read /proc/self/maps - not patched"); return; }
	static char cb[8192]; long have = 0, r; te_base = 0; te_nx = 0;
	while ((r = read(fd, cb + have, sizeof cb - have)) > 0) {
		have += r; long st = 0;
		for (long i = 0; i < have; i++) if (cb[i] == '\n') { te_line(cb + st, cb + i); st = i + 1; }
		for (long i = st; i < have; i++) cb[i - st] = cb[i];
		have -= st; if (have == (long)sizeof cb) have = 0;      /* absurdly long line: drop it */
	}
	close(fd);
	if (!te_base) { if (te_tries == 40 && is_tracker()) emit(5, "q2engine", "engine not found in this process - not patched"); return; }
	unsigned long base = te_base;
	te_state = 2;
	int inx = 0; for (int i = 0; i < te_nx; i++) if (base + TE_CALL >= te_xlo[i] && base + TE_TEST + 4 <= te_xhi[i]) inx = 1;
	if (!inx) { emit(5, "q2engine", "engine code not where expected - not patched"); return; }
	const char *want = "Failed to set thread priority for thread type";
	const char *got = (const char *)(base + TE_MSG);
	for (int i = 0; want[i]; i++) if (got[i] != want[i]) { emit(5, "q2engine", "engine build differs (message) - not patched"); return; }
	volatile unsigned *ins = (volatile unsigned *)(base + TE_TEST);
	if (*(volatile unsigned *)(base + TE_CALL) != 0xd63f0100u || *ins != 0x36000480u) { emit(5, "q2engine", "engine build differs (code) - not patched"); return; }
	unsigned long pg = (base + TE_TEST) & ~0xfffUL;
	long mr = sys3(226, pg, 0x1000, 7);
	if (mr < 0) { emit(5, "q2engine", "could not make the page writable - not patched"); return; }
	*ins = 0xd503201fu;                                  /* nop */
	__asm__ volatile("dc cvau, %0\n dsb ish\n ic ivau, %0\n dsb ish\n isb" :: "r"(base + TE_TEST) : "memory");
	sys3(226, pg, 0x1000, 5);
	te_state = 1;
	emit(4, "q2engine", "thread-priority refusal made non-fatal (in memory only, engine file untouched)");
}
__attribute__((constructor)) static void te_init(void) { te_patch(); }

// desk-246: Meta's tracker reached RUNNING, then aborted on its first camera buffer: the graphics mapper's
// native_handle_clone() (dup() of the buffer's file descriptors) failed.  Same function as Android's libcutils,
// plus a log line saying WHY a dup failed (errno, fd, the process's open-file limit), so the next test is conclusive.
typedef struct { int version, numFds, numInts; int data[]; } q2nh_t;
static long sys4(long n, long a, long b, long c, long d) { register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c, x3 __asm__("x3") = d; __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory"); return x0; }
static void numout(char *m, int *k, long v) { char t[24]; int j = 0; if (v < 0) { m[(*k)++] = '-'; v = -v; } do { t[j++] = '0' + v % 10; v /= 10; } while (v); while (j) m[(*k)++] = t[--j]; }
q2nh_t *native_handle_clone(const q2nh_t *h)
{
	static q2nh_t *(*cr)(int, int); static int (*cl)(const q2nh_t *); static int (*de)(q2nh_t *);
	if (!cr) { *(void **)&cr = dlsym((void *)-1L, "native_handle_create"); *(void **)&cl = dlsym((void *)-1L, "native_handle_close"); *(void **)&de = dlsym((void *)-1L, "native_handle_delete"); }
	if (!h || !cr) return 0;
	q2nh_t *c = cr(h->numFds, h->numInts);
	if (!c) { char m[120]; int k = 0; for (const char *a = "native_handle_create failed: fds "; *a; a++) m[k++] = *a; numout(m, &k, h->numFds); for (const char *a = " ints "; *a; a++) m[k++] = *a; numout(m, &k, h->numInts); m[k] = 0; emit(6, "q2handle", m); return 0; }
	for (int i = 0; i < h->numFds; i++) {
		long r = sys3(23, h->data[i], 0, 0);   /* dup */
		if (r < 0) {
			unsigned long lim[2] = { 0, 0 }; sys4(261, 0, 7, 0, (long)lim);   /* prlimit64(self, RLIMIT_NOFILE) */
			char m[200]; int k = 0;
			for (const char *a = "dup of buffer fd "; *a; a++) m[k++] = *a; numout(m, &k, h->data[i]);
			for (const char *a = " failed, errno "; *a; a++) m[k++] = *a; numout(m, &k, -r);
			for (const char *a = " (24=too many open files, 9=bad fd); open-file limit "; *a; a++) m[k++] = *a; numout(m, &k, (long)lim[0]);
			m[k] = 0; emit(6, "q2handle", m);
			c->numFds = i; if (cl) cl(c); if (de) de(c); return 0;
		}
		c->data[i] = (int)r;
	}
	for (int i = 0; i < h->numInts; i++) c->data[h->numFds + i] = h->data[h->numFds + i];
	return c;
}
