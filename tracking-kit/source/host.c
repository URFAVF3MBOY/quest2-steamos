// q2islandhost (desk-249, controller list fixed desk-251d, late connect desk-251e) - stand-ins for the two Android-framework services Meta's 2023 trackingservice needs:
//  * "vrpowermanager"  (oculus.internal.power.IVrPowerManager): registerClient(client) -> tells the tracker
//    STANDBY (1) then RUNNING (2) through client.onStateChange.  Other calls just succeed.
//  * "OVRRemoteService" (oculus.internal.remote.IRemoteService): the controller list.  Answers
//    getSupportedDeviceTypes / getPairedDeviceStatus / getDeviceEventFd and accepts the callbacks.
//    It has NO firmware, OTA, pairing or unpairing code: every other call is answered "ok" and ignored.
// Usage: q2islandhost [left_idhex:serial:fw] [right_idhex:serial:fw]
typedef unsigned long size_t; typedef unsigned long u64; typedef long i64;
void *dlopen(const char *, int); void *dlsym(void *, const char *); char *dlerror(void);
int dprintf(int, const char *, ...); int socketpair(int, int, int, int *); long read(int, void *, size_t); long recv(int, void *, size_t, int); int open(const char *, int, ...); int close(int); int usleep(unsigned); void exit(int);
typedef struct AIBinder AIBinder; typedef struct AParcel AParcel; typedef struct AIBinder_Class AIBinder_Class; typedef struct AStatus AStatus;
typedef int st_t;
static AIBinder_Class *(*Cdef)(const char *, void *(*)(void *), void (*)(void *), st_t (*)(AIBinder *, unsigned, const AParcel *, AParcel *));
static AIBinder *(*Bnew)(const AIBinder_Class *, void *);
static st_t (*SMadd)(AIBinder *, const char *);
static void (*Pmax)(unsigned); static void (*Pstart)(void); static void (*Pjoin)(void);
static st_t (*Wstat)(AParcel *, const AStatus *); static AStatus *(*Sok)(void); static void (*Sdel)(AStatus *);
static st_t (*Wi32)(AParcel *, int); static st_t (*Wi64)(AParcel *, i64); static st_t (*Wdbl)(AParcel *, double);
static st_t (*Wstr)(AParcel *, const char *, int); static st_t (*Wpfd)(AParcel *, int);
static st_t (*Ri32)(const AParcel *, int *); static st_t (*Ri64)(const AParcel *, i64 *); static st_t (*Rbinder)(const AParcel *, AIBinder **);
static int (*Bassoc)(AIBinder *, const AIBinder_Class *); static st_t (*Bprep)(AIBinder *, AParcel **);
static st_t (*Btrans)(AIBinder *, unsigned, AParcel **, AParcel **, unsigned); static void (*Pdel)(AParcel *); static void (*Binc)(AIBinder *);
static AIBinder_Class *pmClientCls, *statusCbCls;
struct ctl { u64 id; char sid[20], serial[24], fw[16]; int type; } C[2]; static int NC;
static int evfd[2][2] = { { -1, -1 }, { -1, -1 } };
static void *noc(void *a) { return a; } static void nod(void *a) { (void)a; }
static st_t dummyTx(AIBinder *b, unsigned c, const AParcel *i, AParcel *o) { (void)b; (void)c; (void)i; (void)o; return 0; }
static void ok(AParcel *o) { AStatus *s = Sok(); Wstat(o, s); Sdel(s); }
static int call_state(AIBinder *cli, int state, unsigned flags)
{
	// desk-239: Meta's own caller sends onStateChange one-way (flags 1); desk-238's two-way call got -32
	if (!Bassoc(cli, pmClientCls)) { dprintf(1, "power client: associateClass failed\n"); return -1; }
	AParcel *in = 0, *out = 0;
	if (Bprep(cli, &in)) { dprintf(1, "power client: prepareTransaction failed\n"); return -1; }
	Wi32(in, state);
	st_t r = Btrans(cli, 1, &in, &out, flags);
	dprintf(1, "vrpowermanager -> client.onStateChange(%d) %s: %d\n", state, flags ? "one-way" : "two-way", r);
	if (out) Pdel(out);
	return r;
}
static AIBinder *powerClient; static AIBinder *statusCb;   // desk-251e: the tracker's IRemoteServiceStatusCallback
static st_t pmTx(AIBinder *b, unsigned code, const AParcel *in, AParcel *out)
{
	(void)b; dprintf(1, "vrpowermanager: call %u\n", code);
	if (code == 1) { AIBinder *cli = 0; Rbinder(in, &cli); ok(out); if (cli) { powerClient = cli; } return 0; }
	ok(out); return 0;
}
static void write_status(AParcel *o, struct ctl *c)
{
	Wi32(o, 1);                                    // optional<RemoteStatus> present
	Wstr(o, c->sid, (int)__builtin_strlen(c->sid));  // deviceId
	Wstr(o, c->fw, (int)__builtin_strlen(c->fw));                       // firmware
	Wstr(o, "0x08", 4);                                                 // hardware revision
	Wstr(o, c->serial, (int)__builtin_strlen(c->serial));               // serial
	Wstr(o, "", 0);                                                     // IMU model
	Wdbl(o, 0.0);
	Wi32(o, 100);       // battery %
	Wi32(o, 3);         // CONNECTED_ACTIVE
	Wi32(o, 0);         // no errors
	Wi32(o, 1);         // model JEDI (Quest 2 Touch)
	Wi32(o, 1);         // tracking NONE (the tracker reports its own)
	Wi32(o, 0);         // brightness
}
static st_t rsTx(AIBinder *b, unsigned code, const AParcel *in, AParcel *out)
{
	(void)b;
	if (code == 1) { ok(out); Wi32(out, NC); for (int i = 0; i < NC; i++) Wi32(out, C[i].type); dprintf(1, "OVRRemoteService: getSupportedDeviceTypes -> %d\n", NC); return 0; }
	if (code == 2) { int t = 0; Ri32(in, &t); ok(out); Wstr(out, t == 0 ? "Left" : t == 1 ? "Right" : "Unknown", t == 0 ? 4 : t == 1 ? 5 : 7); return 0; }
	if (code == 3) {
		int t = 0; Ri32(in, &t); ok(out);
		struct ctl *c = 0; for (int i = 0; i < NC; i++) if (C[i].type == t) c = &C[i];
		if (c) write_status(out, c); else Wi32(out, 0);
		dprintf(1, "OVRRemoteService: getPairedDeviceStatus(%d) -> %s\n", t, c ? c->sid : "none");
		return 0;
	}
	if (code == 26) {
		i64 id = 0; Ri64(in, &id); ok(out);
		int k = -1; for (int i = 0; i < NC; i++) if ((i64)C[i].id == id) k = i;
		dprintf(1, "OVRRemoteService: getDeviceEventFd(%016lx) -> %s\n", (u64)id, k >= 0 ? "socket" : "none");
		Wpfd(out, k >= 0 ? evfd[k][1] : -1); return 0;
	}
	if (code == 19 || code == 21) { AIBinder *cb = 0; Rbinder(in, &cb); if (cb) { Binc(cb); if (code == 19) statusCb = cb; } ok(out); dprintf(1, "OVRRemoteService: callback registered (call %u)\n", code); return 0; }
	dprintf(1, "OVRRemoteService: call %u answered ok (ignored)\n", code);
	ok(out); return 0;
}

// ---- desk-242: "RuntimeIpcBroker" (oculus.internal.IRuntimeIpcBroker) --------------------------------------------
// On a stock Quest this lives in Meta's Android framework.  The tracker's inside-out engine connects to it while
// waking up; when it is missing, Meta's cleanup code dereferences the missing broker and the tracker crashes.  This is
// a plain key -> binder / file-descriptor store: connect/disconnect are accepted, set* store, get* hand back what was
// stored (or "not found"), clear* forget.  Every call is logged.  No firmware, settings or hardware involvement.
static st_t (*Rbytes)(const AParcel *, void *, int (*)(void *, int, signed char **));
static st_t (*Rstr)(const AParcel *, void *, int (*)(void *, int, char **));
static st_t (*Rpfd)(const AParcel *, int *); static st_t (*Wbinder)(AParcel *, AIBinder *);
static AStatus *(*Sexc)(int); static int (*Sdup)(int);
static signed char kb[256]; static int kl; static char sb[256];
static int kalloc(void *d, int n, signed char **out) { (void)d; kl = n < 0 ? -1 : n; if (n > (int)sizeof kb) return 0; *out = n > 0 ? kb : (signed char *)kb; return 1; }
static int salloc(void *d, int n, char **out) { (void)d; if (n < 0) { sb[0] = 0; *out = 0; return 1; } if (n > (int)sizeof sb) return 0; *out = sb; return 1; }
struct kv { int used, kl, fd; signed char k[256]; AIBinder *b; } KV[48];
static int keq(const signed char *a, const signed char *b, int n) { for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }
static struct kv *kfind(int make)
{
	for (int i = 0; i < 48; i++) if (KV[i].used && KV[i].kl == kl && keq(KV[i].k, kb, kl > 0 ? kl : 0)) return &KV[i];
	if (!make) return 0;
	for (int i = 0; i < 48; i++) if (!KV[i].used) { KV[i].used = 1; KV[i].kl = kl; KV[i].fd = -1; KV[i].b = 0; for (int j = 0; j < kl; j++) KV[i].k[j] = kb[j]; return &KV[i]; }
	return 0;
}
static const char *kstr(void) { static char o[160]; int j = 0; for (int i = 0; i < kl && j < 150; i++) { unsigned char c = (unsigned char)kb[i]; if (c >= 32 && c < 127) o[j++] = c; else { const char *h = "0123456789abcdef"; o[j++] = '\\'; o[j++] = h[c >> 4]; o[j++] = h[c & 15]; } } o[j] = 0; return o; }
static void notfound(AParcel *o) { AStatus *s = Sexc(-5 /* EX_ILLEGAL_STATE */); Wstat(o, s); Sdel(s); }
static st_t ribTx(AIBinder *b, unsigned code, const AParcel *in, AParcel *out)
{
	(void)b; kl = 0;
	if (code == 1) { AIBinder *t = 0; Rstr(in, 0, salloc); Rbinder(in, &t); ok(out); dprintf(1, "RuntimeIpcBroker: connect(\"%s\")\n", sb); return 0; }
	if (code == 2) { ok(out); dprintf(1, "RuntimeIpcBroker: disconnect\n"); return 0; }
	if (code >= 3 && code <= 8) Rbytes(in, 0, kalloc);
	struct kv *e;
	switch (code) {
	case 3: { int fd = -1; Rpfd(in, &fd); e = kfind(1); if (e) { e->fd = fd; } ok(out); Wi32(out, 0); dprintf(1, "RuntimeIpcBroker: setFileDescriptor(%s) fd %d\n", kstr(), fd); return 0; }
	case 4: e = kfind(0); dprintf(1, "RuntimeIpcBroker: getFileDescriptor(%s) -> %s\n", kstr(), e && e->fd >= 0 ? "found" : "not found");
		if (e && e->fd >= 0) { ok(out); Wpfd(out, e->fd); } else notfound(out); return 0;
	case 5: e = kfind(0); if (e) e->fd = -1; ok(out); Wi32(out, 0); dprintf(1, "RuntimeIpcBroker: clearFileDescriptor(%s)\n", kstr()); return 0;
	case 6: { AIBinder *x = 0; Rbinder(in, &x); e = kfind(1); if (e) e->b = x; ok(out); Wi32(out, 0); dprintf(1, "RuntimeIpcBroker: setBinder(%s)\n", kstr()); return 0; }
	case 7: e = kfind(0); dprintf(1, "RuntimeIpcBroker: getBinder(%s) -> %s\n", kstr(), e && e->b ? "found" : "none"); ok(out); Wbinder(out, e ? e->b : 0); return 0;
	case 8: e = kfind(0); if (e) e->b = 0; ok(out); Wi32(out, 0); dprintf(1, "RuntimeIpcBroker: clearBinder(%s)\n", kstr()); return 0;
	}
	dprintf(1, "RuntimeIpcBroker: unknown call %u\n", code); ok(out); return 0;
}

// ---- desk-249: "permission" (android.os.IPermissionController) ----------------------------------------------------
// Meta's camera-mode service waits ~10 s for Android's permission service before it answers the tracker (that was
// 10 s of every tracking start-up).  Inside the island everything runs as root and is ours, so every check is granted.
static st_t permTx(AIBinder *b, unsigned code, const AParcel *in, AParcel *out)
{
	(void)b; (void)in;
	static int said;
	if (!said++) dprintf(1, "permission: first call %u (all checks granted)\n", code);
	ok(out);
	if (code == 1 || code == 4) Wi32(out, 1);          // checkPermission / isRuntimePermission -> true
	else if (code == 2) Wi32(out, 0);                  // noteOp -> MODE_ALLOWED
	else if (code == 3) Wi32(out, 0);                  // getPackagesForUid -> empty list
	else if (code == 5) Wi32(out, 0);                  // getPackageUid -> 0
	return 0;
}
static int hx(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static void parse(const char *s, struct ctl *c, int type)
{
	int k = 0; c->id = 0; while (hx(*s) >= 0 && k < 16) { c->sid[k++] = *s; c->id = c->id * 16 + hx(*s); s++; } c->sid[k] = 0;
	if (*s == ':') s++; k = 0; while (*s && *s != ':' && k < 23) c->serial[k++] = *s++; c->serial[k] = 0;
	if (*s == ':') s++; k = 0; while (*s && *s != ':' && k < 15) c->fw[k++] = *s++; c->fw[k] = 0;
	c->type = type;
}
// desk-251e: tell the tracker a controller is now paired + connected - IRemoteServiceStatusCallback::onStatus(int
// deviceType, @nullable RemoteStatus), transaction 1, one-way (as Meta's own OVRRemoteService does when a controller
// connects).  Listing the controllers at start-up instead (desk-251/251d) stalled the tracker before head tracking
// began: they only connect after the tracker switches the controller radio on.
static int notify(struct ctl *c)
{
	if (!statusCb) return -1;
	if (!Bassoc(statusCb, statusCbCls)) { dprintf(1, "status callback: associateClass failed\n"); return -1; }
	AParcel *in = 0, *out = 0;
	if (Bprep(statusCb, &in)) { dprintf(1, "status callback: prepareTransaction failed\n"); return -1; }
	Wi32(in, c->type); write_status(in, c);
	st_t r = Btrans(statusCb, 1, &in, &out, 1);
	dprintf(1, "OVRRemoteService -> onStatus(type %d, %s) one-way: %d\n", c->type, c->sid, r);
	if (out) Pdel(out);
	return r;
}
static int late_done;
static void check_late(void)
{
	// /q2/ctl-late (written by q2island-inner once both controllers have connected): lines "<type> <id:serial:fw>"
	int fd = open("/q2/ctl-late", 0); if (fd < 0) return;
	static char b[512]; long n = read(fd, b, sizeof b - 1); close(fd); if (n <= 0) return; b[n] = 0;
	late_done = 1;
	for (char *p = b; *p; ) {
		char *e = p; while (*e && *e != '\n') e++; char sv = *e; *e = 0;
		if ((p[0] == '0' || p[0] == '1') && p[1] == ' ' && NC < 2) {
			int t = p[0] - '0', have = 0; for (int i = 0; i < NC; i++) if (C[i].type == t) have = 1;
			if (!have) {
				parse(p + 2, &C[NC], t);
				if (socketpair(1, 5 | 02000000, 0, evfd[NC])) { dprintf(1, "socketpair failed\n"); evfd[NC][0] = evfd[NC][1] = -1; }
				dprintf(1, "controller connected, type %d (%s): %s serial %s fw %s\n", t, t ? "right" : "left", C[NC].sid, C[NC].serial, C[NC].fw);
				NC++; notify(&C[NC - 1]);
			}
		}
		*e = sv; p = *e ? e + 1 : e;
	}
}
// desk-251h: /q2/ctl-again (q2island-inner, when Meta's memory still lacks a hand ~8 s after the announcement):
// lines "<n> <type>" - every line with a new n announces that controller again
static int again_n;
static void check_again(void)
{
	int fd = open("/q2/ctl-again", 0); if (fd < 0) return;
	static char b[256]; long n = read(fd, b, sizeof b - 1); close(fd); if (n <= 0) return; b[n] = 0;
	for (char *p = b; *p; ) {
		int k = 0; while (*p >= '0' && *p <= '9') k = k * 10 + (*p++ - '0');
		if (*p == ' ') p++;
		int t = (*p == '0' || *p == '1') ? *p - '0' : -1;
		if (k > again_n && t >= 0) { again_n = k; for (int i = 0; i < NC; i++) if (C[i].type == t) { dprintf(1, "announcing type %d again (request %d)\n", t, k); notify(&C[i]); } }
		while (*p && *p != '\n') p++; if (*p) p++;
	}
}
#define G(v, n) do { *(void **)&v = dlsym(nb, n); if (!v) { dprintf(1, "missing %s\n", n); return 1; } } while (0)
__attribute__((used)) int q2main(int argc, char **argv)
{
	void *nb = dlopen("libbinder_ndk.so", 2);
	if (!nb) { dprintf(1, "dlopen libbinder_ndk: %s\n", dlerror()); return 1; }
	G(Cdef, "AIBinder_Class_define"); G(Bnew, "AIBinder_new"); G(SMadd, "AServiceManager_addService");
	G(Pmax, "ABinderProcess_setThreadPoolMaxThreadCount"); G(Pstart, "ABinderProcess_startThreadPool"); G(Pjoin, "ABinderProcess_joinThreadPool");
	G(Wstat, "AParcel_writeStatusHeader"); G(Sok, "AStatus_newOk"); G(Sdel, "AStatus_delete");
	G(Wi32, "AParcel_writeInt32"); G(Wi64, "AParcel_writeInt64"); G(Wdbl, "AParcel_writeDouble"); G(Wstr, "AParcel_writeString"); G(Wpfd, "AParcel_writeParcelFileDescriptor");
	G(Ri32, "AParcel_readInt32"); G(Ri64, "AParcel_readInt64"); G(Rbinder, "AParcel_readStrongBinder");
	G(Rbytes, "AParcel_readByteArray"); G(Rstr, "AParcel_readString"); G(Rpfd, "AParcel_readParcelFileDescriptor"); G(Wbinder, "AParcel_writeStrongBinder");
	G(Sexc, "AStatus_fromExceptionCode"); (void)Sdup;
	G(Bassoc, "AIBinder_associateClass"); G(Bprep, "AIBinder_prepareTransaction"); G(Btrans, "AIBinder_transact"); G(Pdel, "AParcel_delete"); G(Binc, "AIBinder_incStrong");
	for (int a = 1; a < argc && a <= 2; a++) if (argv[a][0]) { parse(argv[a], &C[NC], a - 1); dprintf(1, "controller type %d (%s): %s serial %s fw %s\n", a - 1, a == 2 ? "right" : "left", C[NC].sid, C[NC].serial, C[NC].fw); NC++; }
	// desk-251d: AF_UNIX + SOCK_SEQPACKET (+CLOEXEC).  desk-251 passed no socket type -> EINVAL, the fds stayed 0 and
	// the tracker got this program's stdin as each controller's event channel (and the loop below could block on it)
	for (int i = 0; i < NC; i++) if (socketpair(1, 5 | 02000000, 0, evfd[i])) { dprintf(1, "socketpair failed\n"); evfd[i][0] = evfd[i][1] = -1; }
	pmClientCls = Cdef("oculus.internal.power.IVrPowerManagerClient", noc, nod, dummyTx);
	statusCbCls = Cdef("oculus.internal.remote.IRemoteServiceStatusCallback", noc, nod, dummyTx);
	AIBinder_Class *pmCls = Cdef("oculus.internal.power.IVrPowerManager", noc, nod, pmTx);
	AIBinder_Class *rsCls = Cdef("oculus.internal.remote.IRemoteService", noc, nod, rsTx);
	Pmax(4); Pstart();
	dprintf(1, "addService vrpowermanager: %d\n", SMadd(Bnew(pmCls, 0), "vrpowermanager"));
	dprintf(1, "addService OVRRemoteService: %d\n", SMadd(Bnew(rsCls, 0), "OVRRemoteService"));
	AIBinder_Class *permCls = Cdef("android.os.IPermissionController", noc, nod, permTx);
	dprintf(1, "addService permission: %d\n", SMadd(Bnew(permCls, 0), "permission"));
	AIBinder_Class *ribCls = Cdef("oculus.internal.IRuntimeIpcBroker", noc, nod, ribTx);
	dprintf(1, "addService RuntimeIpcBroker: %d\n", SMadd(Bnew(ribCls, 0), "RuntimeIpcBroker"));
	static char buf[4096]; long tot = 0; int sent = 0, tries = 0;
	for (int t = 0;; t++) {
		if (powerClient && sent < 2 && tries < 12) {
			int st = sent == 0 ? 1 : 2;
			if (call_state(powerClient, st, 1) == 0 || call_state(powerClient, st, 0) == 0) { sent++; tries = 0; } else tries++;
		}
		for (int i = 0; i < NC; i++) { long r; if (evfd[i][0] < 0) continue; while ((r = recv(evfd[i][0], buf, sizeof buf, 0x40 /*MSG_DONTWAIT*/)) > 0) tot += r; }
		if (!late_done && sent >= 2 && statusCb && t % 4 == 0) check_late();
		if (late_done && t % 4 == 2) check_again();
		if (t % 20 == 0) dprintf(1, "alive; power client %s; event bytes from tracker %ld\n", powerClient ? "registered" : "not yet", tot);
		usleep(250000);
	}
}
__attribute__((naked, noreturn)) void _start(void) { __asm__ volatile("mov x29, #0\n" "mov x30, #0\n" "ldr x0, [sp]\n" "add x1, sp, #8\n" "bl q2main\n" "bl exit\n"); }
