// q2metapose (desk-248, controllers desk-251) - hands Meta's own head and controller tracking to SteamVR.
// Meta's trackingservice (running in the island) keeps its latest head state in a small shared-memory area
// ("TrackingServiceHeadTracker").  Each entry is Meta's "HeadTrackingStateV5" (layout read from this headset's own
// libposeprediction.so: checkHeadTrackingStateValidV5 / extrapolateHeadTrackingStateToTimeV5):
//   +0x00 u32 valid   +0x10 orientation quat (x,y,z,w)   +0x20 position (m)   +0x2c angular velocity (rad/s, world)
//   +0x38 linear velocity   +0x44 angular accel   +0x50 linear accel   +0x60 i64 time (ns, CLOCK_MONOTONIC)
//   +0x80 second quat (reference from odometry)   size 0xa0
// This program READS that area (read-only, through /proc/<pid>/mem), picks the newest valid entry, predicts it to
// "now" and writes it into /dev/shm/q2pose - the file the q2hmd SteamVR driver already reads:
//   +0 u32 seqlock, +8 doubles: qw qx qy qz t wx wy wz (angular velocity in HEAD frame)   (as q2headtrack writes)
//   +72 doubles: px py pz (metres, standing height added)   +96 u32 'META' magic   +100 u32 update counter
//   +104 double: monotonic time of this update (the driver ignores the position when it is older than 0.3 s)
//   +112 doubles: linear velocity vx vy vz (m/s, world) - desk-248: SteamVR predicts position too
// Nothing is ever written to Meta's process.  Status every 5 s: /var/log/q2/metapose.txt (Ctrl+Alt+U exports it while it runs)
// Usage: q2metapose <tracker pid>
typedef unsigned long size_t; typedef unsigned int u32; typedef long i64;
static long sc(long n, long a, long b, long c, long d, long e, long f) { register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c, x3 __asm__("x3") = d, x4 __asm__("x4") = e, x5 __asm__("x5") = f; __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory"); return x0; }
#define openat(p, f, m) sc(56, -100, (long)(p), f, m, 0, 0)
#define rd(fd, b, n) sc(63, fd, (long)(b), n, 0, 0, 0)
#define wr(fd, b, n) sc(64, fd, (long)(b), n, 0, 0, 0)
#define pread(fd, b, n, o) sc(67, fd, (long)(b), n, o, 0, 0)
#define cls(fd) sc(57, fd, 0, 0, 0, 0, 0)
#define ftrunc(fd, n) sc(46, fd, n, 0, 0, 0, 0)
#define mmap6(a, l, p, f, fd, o) sc(222, (long)(a), l, p, f, fd, o)
#define lseek(fd, o, w) sc(62, fd, o, w, 0, 0, 0)
#define kill0(p) sc(129, p, 0, 0, 0, 0, 0)
struct ts_ { long s, ns; };
static double now(void) { struct ts_ t; sc(113, 1 /*CLOCK_MONOTONIC*/, (long)&t, 0, 0, 0, 0); return t.s + t.ns * 1e-9; }
static void msleep(long ns) { struct ts_ t = { 0, ns }; sc(101, (long)&t, 0, 0, 0, 0, 0); }
static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static char lb[4096]; static int ll;
static void L(const char *s) { while (*s && ll < 4000) lb[ll++] = *s++; }
static void Ln(double v, int dec) { char t[40]; int k = 0; if (v != v) { L("nan"); return; } if (v < 0) { L("-"); v = -v; } if (v > 1e12) { L("big"); return; } double m = 1; for (int i = 0; i < dec; i++) m *= 10; long w = (long)v; long f = (long)((v - w) * m + 0.5); if (f >= (long)m) { w++; f -= (long)m; } do { t[k++] = '0' + w % 10; w /= 10; } while (w); while (k) { char c[2] = { t[--k], 0 }; L(c); } if (dec) { L("."); char d[16]; for (int i = dec - 1; i >= 0; i--) { d[i] = '0' + f % 10; f /= 10; } d[dec] = 0; L(d); } }
static void Lx(unsigned long v) { char t[20]; int k = 0; do { int d = v & 15; t[k++] = d < 10 ? '0' + d : 'a' + d - 10; v >>= 4; } while (v); while (k) { char c[2] = { t[--k], 0 }; L(c); } }
static void flushlog(int append)
{
	int fd = openat("/var/log/q2/metapose.txt", 01 | 0100 | (append ? 02000 : 01000), 0644);   // O_WRONLY|O_CREAT|(O_APPEND|O_TRUNC)
	if (fd >= 0) { wr(fd, lb, ll); cls(fd); }
	ll = 0;
}
static unsigned long hex(const char **p) { unsigned long v = 0; for (;;) { char c = **p; int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; if (d < 0) break; v = v * 16 + d; (*p)++; } return v; }
static int contains(const char *s, const char *e, const char *w) { size_t n = slen(w); for (const char *p = s; p + n <= e; p++) { size_t i = 0; while (i < n && p[i] == w[i]) i++; if (i == n) return 1; } return 0; }

#define MAXR 64
static unsigned long ra[MAXR], rl[MAXR]; static int nr;
static char maps[1 << 20];
static int find_regions_to(int pid, const char *name, unsigned long *oa, unsigned long *ol, int max);
static int find_regions(int pid, const char *name) { nr = 0; int n = find_regions_to(pid, name, ra, rl, MAXR); nr = n < 0 ? 0 : n; return n; }
static int find_regions_to(int pid, const char *name, unsigned long *oa, unsigned long *ol, int max)
{
	char path[64] = "/proc/"; int k = 6; char t[12]; int j = 0; int v = pid; do { t[j++] = '0' + v % 10; v /= 10; } while (v); while (j) path[k++] = t[--j];
	const char *m = "/maps"; while (*m) path[k++] = *m++; path[k] = 0;
	int fd = openat(path, 0, 0); if (fd < 0) return -1;
	long n = 0, r; while ((r = rd(fd, maps + n, sizeof maps - 1 - n)) > 0) n += r; cls(fd); maps[n] = 0;
	int cnt = 0;
	for (const char *p = maps; *p && cnt < max; ) {
		const char *s = p; unsigned long a = hex(&p); p++; unsigned long b = hex(&p);
		const char *e = p; while (*e && *e != '\n') e++;
		if (contains(s, e, name) && b - a <= (1 << 20)) { oa[cnt] = a; ol[cnt] = b - a; cnt++; }
		p = *e ? e + 1 : e;
	}
	return cnt;
}
static float F(const unsigned char *p) { union { u32 u; float f; } x; x.u = p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; return x.f; }
static i64 I64(const unsigned char *p) { unsigned long v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return (i64)v; }
static u32 U32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static int unitq(const unsigned char *p) { double s = 0; for (int i = 0; i < 4; i++) { double v = F(p + 4 * i); if (v != v) return 0; s += v * v; } return s > 0.98 && s < 1.02; }
static int valid_rec(const unsigned char *r)
{
	if (U32(r) != 1) return 0;
	i64 t = I64(r + 0x60); if (t <= 0 || t > (i64)1e16) return 0;
	if (!unitq(r + 0x10) || !unitq(r + 0x80)) return 0;
	for (int i = 0x20; i < 0x5c; i += 4) { float v = F(r + i); if (v != v || v > 1e4f || v < -1e4f) return 0; }
	return 1;
}
static unsigned char buf[1 << 20], buf2[0xa0];

// ---- desk-251h: Meta's controller poses --------------------------------------------------------------------------
// Found in this headset's own memory (desk-251g snapshots, controller moving, compared with Meta's dumpsys): the
// tracker's "TrackingServiceController" shared memory (16 KB) holds, per tracked controller,
//  * an info entry with the controller's 64-bit id, then model/firmware, then handedness (1 left, 2 right) at id+0x10;
//  * a LATEST-STATE record updated ~500 times a second (IMU rate): u32 seqA, u32 seqB (equal = stable), u32 valid (1),
//    u32 flags (0x8f), ..., +0x30 orientation quat (x, y, z, w), +0x40 position (m), +0x4c angular velocity (rad/s,
//    world), +0x58 linear velocity (m/s), +0x64 angular accel, +0x70 linear accel, +0x80 f64 time (s, monotonic);
//  * a 31-entry history ring at camera rate (same layout, 0xff.. marker instead of the counters) - not used.
// The ids come from /var/lib/q2/meta_controllers (re-read every 5 s), so each record is matched to its hand by id:
// the n-th info entry belongs to the n-th latest-state record.  /var/lib/q2/meta_ctl_swap swaps the hands by hand.
// Output: /dev/shm/q2pose +256 (left) / +384 (right), inside the same seqlock: u32 'MCTL', u32 counter, f64 time,
// f64 qw qx qy qz, px py pz (same origin/offset as the head), vx vy vz (m/s, world), wx wy wz (rad/s, world).
#define CST_LEN 0x88
static unsigned long cra[2], crl[2]; static int cnr; static double cscan_t, cfind_t, cids_t;
static unsigned char cbuf[1 << 16], cst[CST_LEN];
static unsigned long cid[2];                         // known ids: [0] left, [1] right
static int nlat; static long latoff[4]; static int lathand[4];   // latest-state records and their hands (-1 unknown)
static int ninfo; static long infooff[4]; static int infohand[4];
static double lastt[4], newt[4]; static long nupd[4];
static int cswapfile; static double cswap_t;
static unsigned long cdump_n; static double cdump_t;
struct cout { int ok; double q[4], p[3], v[3], w[3], age; unsigned char flags; };
static struct cout CO[2];
static double D64(const unsigned char *p) { union { unsigned long u; double d; } x; x.u = 0; for (int i = 7; i >= 0; i--) x.u = x.u << 8 | p[i]; return x.d; }
static unsigned long U64(const unsigned char *p) { unsigned long v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }
static void read_ids(void)
{
	// /var/lib/q2/meta_controllers: "left <16 hex>:serial:fw" / "right <16 hex>:..."
	int fd = openat("/var/lib/q2/meta_controllers", 0, 0); if (fd < 0) return;
	char b[512]; long n = rd(fd, b, sizeof b - 1); cls(fd); if (n <= 0) return; b[n] = 0;
	for (const char *p = b; *p; ) {
		int h = (p[0] == 'l') ? 0 : (p[0] == 'r') ? 1 : -1;
		while (*p && *p != ' ' && *p != '\n') p++;
		if (*p == ' ') p++;
		unsigned long v = hex(&p); if (h >= 0 && v) cid[h] = v;
		while (*p && *p != '\n') p++; if (*p) p++;
	}
}
static int latest_ok(const unsigned char *r, double headt)
{
	u32 a = U32(r), b = U32(r + 4);
	if (a != b || a == 0 || a == 0xffffffffu || U32(r + 8) != 1) return 0;
	if (!unitq(r + 0x30)) return 0;
	for (int i = 0x40; i < 0x7c; i += 4) { float v = F(r + i); if (v != v || v > 1e4f || v < -1e4f) return 0; }
	double t = D64(r + 0x80); if (t != t || t <= 0 || (headt > 0 && (t - headt > 10 || headt - t > 10))) return 0;
	return 1;
}
static void scan_region(int mem, double headt)
{
	long got = pread(mem, cbuf, crl[0] > sizeof cbuf ? sizeof cbuf : crl[0], cra[0]); if (got < 0x100) return;
	int ni = 0, nl = 0; long io[4]; int ih[4]; long lo[4];
	// desk-251j: the info entries are 0x114 apart, so the second id is only 4-byte aligned (251h looked at 8-byte steps,
	// missed the left id, and wrote the left controller's record into the RIGHT hand).  The hand comes from Meta's own
	// handedness field (id + 0x10: 1 left, 2 right); the id decides only when that field is something else.
	for (long o = 0; o + 0x18 <= got && ni < 4; o += 4) {
		unsigned long v = U64(cbuf + o); if (!v) continue;
		for (int h = 0; h < 2; h++) if (cid[h] && v == cid[h]) {
			u32 hd = U32(cbuf + o + 0x10); io[ni] = o; ih[ni] = hd == 1 ? 0 : hd == 2 ? 1 : h; ni++; o += 4;
		}
	}
	for (long o = 0; o + CST_LEN <= got && nl < 4; o += 8) if (latest_ok(cbuf + o, headt)) { lo[nl++] = o; o += CST_LEN - 8; }
	int changed = ni != ninfo || nl != nlat;
	for (int i = 0; !changed && i < ni; i++) changed = io[i] != infooff[i] || ih[i] != infohand[i];
	for (int i = 0; !changed && i < nl; i++) changed = lo[i] != latoff[i];
	if (!changed) return;
	ninfo = ni; nlat = nl;
	for (int i = 0; i < ni; i++) { infooff[i] = io[i]; infohand[i] = ih[i]; }
	for (int i = 0; i < nl; i++) { latoff[i] = lo[i]; lathand[i] = i < ni ? ih[i] : -1; lastt[i] = 0; newt[i] = 0; nupd[i] = 0; }
	L("controllers in Meta's memory: ");
	for (int i = 0; i < ni; i++) { L(ih[i] ? "right" : "left"); L(" id at +"); Lx(io[i]); L(" (handedness "); Ln(U32(cbuf + io[i] + 0x10), 0); L(") "); }
	if (!ni) L("no known id yet ");
	L("| live records: "); for (int i = 0; i < nl; i++) { L("+"); Lx(lo[i]); L(i < ni ? (ih[i] ? "=right " : "=left ") : "=? "); } if (!nl) L("none");
	L("\n"); flushlog(1);
}
static void dump_cregion(int mem)
{
	long got = pread(mem, cbuf, crl[0] > sizeof cbuf ? sizeof cbuf : crl[0], cra[0]); if (got <= 0) return;
	char path[64] = "/var/log/q2/metactl_"; int k = 20; path[k++] = '0' + (char)(cdump_n % 10);
	const char *e = ".bin"; while (*e) path[k++] = *e++; path[k] = 0;
	int fd = openat(path, 01 | 0100 | 01000, 0644); if (fd >= 0) { wr(fd, cbuf, got); cls(fd); }
	L("controller memory dumped to "); L(path); L("\n"); cdump_n++;
}
// one consistent copy of a latest-state record (the counters before and after must match)
static int read_latest(int mem, long off, double headt)
{
	for (int t = 0; t < 4; t++) {
		if (pread(mem, cst, CST_LEN, cra[0] + off) != CST_LEN) return 0;
		unsigned char h2[8]; if (pread(mem, h2, 8, cra[0] + off) != 8) return 0;
		if (U32(h2) == U32(cst) && U32(h2 + 4) == U32(cst + 4) && latest_ok(cst, headt)) return 1;
	}
	return 0;
}
static void ctl_update(int mem, int pid, double tn, i64 headts, const double hq[4], const double hp[3], const double off[3])
{
	(void)hq; (void)hp;
	CO[0].ok = CO[1].ok = 0;
	double headt = headts * 1e-9;
	if (tn - cids_t > 5) { cids_t = tn; read_ids(); int sf = openat("/var/lib/q2/meta_ctl_swap", 0, 0); cswapfile = sf >= 0; if (sf >= 0) cls(sf); }
	(void)cswap_t;
	if (!cnr) {
		if (tn - cfind_t < 2) return;
		cfind_t = tn;
		int n = find_regions_to(pid, "TrackingServiceController", cra, crl, 2);
		if (n <= 0) return;
		cnr = 1; L("controller memory: "); Lx(cra[0]); L(" ("); Ln((double)crl[0], 0); L(" bytes)\n"); flushlog(1);
		cdump_t = tn;
	}
	if (tn - cscan_t > (nlat < 2 ? 1.0 : 5.0)) { cscan_t = tn; scan_region(mem, headt); }
	if (nlat && cdump_n < 2 && tn - cdump_t > (cdump_n == 0 ? 15 : 60)) dump_cregion(mem);
	for (int i = 0; i < nlat; i++) {
		if (!read_latest(mem, latoff[i], headt)) continue;
		double t = D64(cst + 0x80);
		if (t != lastt[i]) { lastt[i] = t; newt[i] = tn; nupd[i]++; }
		double age = tn - t; if (age < -0.05 || age > 0.3) continue;    // stale = controller gone / not tracked
		int hand = lathand[i]; if (hand < 0) continue;                    // desk-251j: unknown hand -> not used (251h guessed and overwrote the right hand)
		if (cswapfile) hand = 1 - hand;
		if (hand < 0 || hand > 1) continue;
		double dt = age; if (dt < 0) dt = 0; if (dt > 0.05) dt = 0.05;
		double qx = F(cst + 0x30), qy = F(cst + 0x34), qz = F(cst + 0x38), qw = F(cst + 0x3c);
		double wx = F(cst + 0x4c), wy = F(cst + 0x50), wz = F(cst + 0x54), vx = F(cst + 0x58), vy = F(cst + 0x5c), vz = F(cst + 0x60);
		double hx = wx * dt * 0.5, hy = wy * dt * 0.5, hz = wz * dt * 0.5;
		double nw = qw - hx * qx - hy * qy - hz * qz, nx = qx + hx * qw + hy * qz - hz * qy;
		double ny = qy - hx * qz + hy * qw + hz * qx, nz = qz + hx * qy - hy * qx + hz * qw;
		double nn = __builtin_sqrt(nw * nw + nx * nx + ny * ny + nz * nz);
		struct cout *c = &CO[hand];
		c->q[0] = nw / nn; c->q[1] = nx / nn; c->q[2] = ny / nn; c->q[3] = nz / nn;
		c->p[0] = F(cst + 0x40) + vx * dt + off[0]; c->p[1] = F(cst + 0x44) + vy * dt + off[1]; c->p[2] = F(cst + 0x48) + vz * dt + off[2];
		c->v[0] = vx; c->v[1] = vy; c->v[2] = vz; c->w[0] = wx; c->w[1] = wy; c->w[2] = wz;
		c->age = age * 1000; c->flags = cst[0xc]; c->ok = 1;
	}
	// for q2island: which hands Meta tracks right now (it re-announces a missing one)
	static double st_t; static int lastmask = -1;
	if (tn - st_t > 2) {
		st_t = tn; int mask = 0; for (int i = 0; i < ninfo; i++) mask |= 1 << infohand[i];
		if (mask != lastmask) {
			lastmask = mask;
			int fd = openat("/run/q2metactl.state", 01 | 0100 | 01000, 0644);
			if (fd >= 0) { char b[32] = "left=0 right=0\n"; b[5] = '0' + (mask & 1); b[13] = '0' + ((mask >> 1) & 1); wr(fd, b, 15); cls(fd); }
		}
	}
}
static void ctl_log(double tn)
{
	if (!cnr) { L("  controllers: memory not found yet\n"); return; }
	L("  controllers: "); Ln((double)ninfo, 0); L(" known in Meta's memory, "); Ln((double)nlat, 0); L(" live record(s)"); if (cswapfile) L(", SWAPPED by meta_ctl_swap"); L("\n");
	for (int i = 0; i < nlat; i++) {
		L("    record +"); Lx(latoff[i]); L(lathand[i] == 0 ? " (left)" : lathand[i] == 1 ? " (right)" : " (?)");
		L(": new states/5s "); Ln((double)nupd[i], 0); nupd[i] = 0;
		L(", age "); Ln(lastt[i] > 0 ? (tn - lastt[i]) * 1000 : -1, 1); L(" ms\n");
	}
	for (int h = 0; h < 2; h++) {
		L(h ? "    right: " : "    left: ");
		if (CO[h].ok) { L("tracked, age "); Ln(CO[h].age, 1); L(" ms, p=("); Ln(CO[h].p[0], 3); L(" "); Ln(CO[h].p[1], 3); L(" "); Ln(CO[h].p[2], 3); L(") q=("); Ln(CO[h].q[0], 3); L(" "); Ln(CO[h].q[1], 3); L(" "); Ln(CO[h].q[2], 3); L(" "); Ln(CO[h].q[3], 3); L(")"); }
		else L("not tracked");
		L("\n");
	}
}

__attribute__((used)) int q2main(int argc, char **argv)
{
	if (argc < 2) return 1;
	int pid = 0; for (const char *s = argv[1]; *s >= '0' && *s <= '9'; s++) pid = pid * 10 + *s - '0';
	L("q2metapose: reading Meta's head tracking from pid "); Ln(pid, 0); L("\n"); flushlog(0);
	// the pose file the SteamVR driver reads
	int sfd = openat("/dev/shm/q2pose", 02 | 0100, 0644); if (sfd < 0) { L("cannot open /dev/shm/q2pose\n"); flushlog(1); return 1; }
	struct { long a[16]; } st; sc(80, sfd, (long)&st, 0, 0, 0, 0);
	if (st.a[6] < 4096) ftrunc(sfd, 4096);
	volatile unsigned char *shm = (volatile unsigned char *)mmap6(0, 4096, 3, 1, sfd, 0);
	if ((long)shm < 0 && (long)shm > -4096) { L("cannot map /dev/shm/q2pose\n"); flushlog(1); return 1; }
	volatile u32 *seq = (volatile u32 *)shm; volatile double *D = (volatile double *)(shm + 8);
	// find Meta's head-tracker area (it appears once the tracker has started its head tracker)
	char mp[64] = "/proc/"; { int k = 6; char t[12]; int j = 0; int v = pid; do { t[j++] = '0' + v % 10; v /= 10; } while (v); while (j) mp[k++] = t[--j]; const char *m = "/mem"; while (*m) mp[k++] = *m++; mp[k] = 0; }
	int mem = -1; double t0 = now(), lastlog = 0; long frames = 0, upd = 0; i64 lastts = 0; double maxgap = 0, agesum = 0, agemax = 0; long agen = 0; double off[3] = { 0, 0, 0 }; int haveoff = 0;
	unsigned long best_addr = 0; u32 counter = 0;
	for (;;) {
		if (kill0(pid) < 0) { L("tracker process gone - stopping, the driver falls back to its own tracking\n"); break; }
		if (!nr) {
			int fb = 0;
			if (find_regions(pid, "TrackingServiceHeadTracker") <= 0) {
				if (now() - t0 > 600) { L("head tracker memory not found in 10 min - giving up\n"); break; }
				// fallback after 40 s: any small shared-memory area of the tracker that holds valid head states
				if (now() - t0 < 40 || find_regions(pid, "/dev/ashmem") <= 0) { msleep(500000000); continue; }
				fb = 1;
			}
			mem = openat(mp, 0, 0); if (mem < 0) { L("cannot open tracker memory\n"); break; }
			if (fb) {
				int k = 0;
				for (int i = 0; i < nr; i++) {
					long got = pread(mem, buf, rl[i] > sizeof buf ? sizeof buf : rl[i], ra[i]); int ok = 0;
					for (long o = 0; o + 0xa0 <= got && !ok; o += 8) ok = valid_rec(buf + o);
					if (ok) { ra[k] = ra[i]; rl[k] = rl[i]; k++; }
				}
				nr = k; L("(name not found - using shared memory that holds head states) ");
				if (!nr) { cls(mem); mem = -1; t0 = now() - 30; msleep(1000000000); continue; }
			}
			L("head tracker memory: "); for (int i = 0; i < nr; i++) { Lx(ra[i]); L(" ("); Ln((double)rl[i], 0); L(" bytes) "); } L("\n"); flushlog(1);
		}
		// newest valid head state in the area(s)
		const unsigned char *bp = 0; i64 bts = 0; unsigned long baddr = 0;
		for (int i = 0; i < nr; i++) {
			long got = pread(mem, buf, rl[i] > sizeof buf ? sizeof buf : rl[i], ra[i]);
			if (got < 0xa0) continue;
			for (long o = 0; o + 0xa0 <= got; o += 8) if (valid_rec(buf + o)) {
				i64 t = I64(buf + o + 0x60); if (t > bts) { bts = t; bp = buf + o; baddr = ra[i] + o; }
			}
		}
		if (bp) {
			// guard against reading an entry while Meta writes it: read it again, use it only if unchanged
			if (pread(mem, buf2, 0xa0, baddr) == 0xa0) { int same = 1; for (int i = 0; i < 0xa0; i++) if (buf2[i] != bp[i]) { same = 0; break; } if (!same) bp = valid_rec(buf2) ? buf2 : 0; }
		}
		if (bp) {
			double tsec = bts * 1e-9, tn = now(), dt = tn - tsec; if (dt < 0) dt = 0; if (dt > 0.05) dt = 0.05;
			double qx = F(bp + 0x10), qy = F(bp + 0x14), qz = F(bp + 0x18), qw = F(bp + 0x1c);
			double px = F(bp + 0x20), py = F(bp + 0x24), pz = F(bp + 0x28);
			double wx = F(bp + 0x2c), wy = F(bp + 0x30), wz = F(bp + 0x34);           // world frame
			double vx = F(bp + 0x38), vy = F(bp + 0x3c), vz = F(bp + 0x40);
			// predict to now: q' = exp(w*dt/2) * q (world-frame angular velocity), p' = p + v*dt
			double hx = wx * dt * 0.5, hy = wy * dt * 0.5, hz = wz * dt * 0.5, hw = 1;
			double nw = hw * qw - hx * qx - hy * qy - hz * qz, nx = hw * qx + hx * qw + hy * qz - hz * qy;
			double ny = hw * qy - hx * qz + hy * qw + hz * qx, nz = hw * qz + hx * qy - hy * qx + hz * qw;
			double nn = __builtin_sqrt(nw * nw + nx * nx + ny * ny + nz * nz); nw /= nn; nx /= nn; ny /= nn; nz /= nn;
			px += vx * dt; py += vy * dt; pz += vz * dt;
			// angular velocity into the head frame (the driver turns it back into world): v_head = q^-1 v q
			double cx = -nx, cy = -ny, cz = -nz, cw = nw;
			double tx = 2 * (cy * wz - cz * wy), ty = 2 * (cz * wx - cx * wz), tz = 2 * (cx * wy - cy * wx);
			double lx = wx + cw * tx + (cy * tz - cz * ty), ly = wy + cw * ty + (cz * tx - cx * tz), lz = wz + cw * tz + (cx * ty - cy * tx);
			// standing height: the first good pose is put at 1.6 m, straight below/above the SteamVR origin
			if (!haveoff) { off[0] = -px; off[1] = 1.6 - py; off[2] = -pz; haveoff = 1; }
			{ double hq[4] = { nw, nx, ny, nz }, hp[3] = { px, py, pz }; ctl_update(mem, pid, tn, bts, hq, hp, off); }   // desk-251
			u32 s = *seq; *seq = s | 1; __atomic_thread_fence(__ATOMIC_RELEASE);
			D[0] = nw; D[1] = nx; D[2] = ny; D[3] = nz; D[4] = tn; D[5] = lx; D[6] = ly; D[7] = lz;
			D[8] = px + off[0]; D[9] = py + off[1]; D[10] = pz + off[2];
			*(volatile u32 *)(shm + 96) = 0x4154454d;   /* 'META' */ *(volatile u32 *)(shm + 100) = ++counter;
			*(volatile double *)(shm + 104) = tn;
			*(volatile double *)(shm + 112) = vx; *(volatile double *)(shm + 120) = vy; *(volatile double *)(shm + 128) = vz;
			for (int h = 0; h < 2; h++) {                // desk-251: controllers at +256 / +384
				volatile unsigned char *b = shm + 256 + 128 * h; volatile double *cd = (volatile double *)(b + 8);
				if (!CO[h].ok) { *(volatile u32 *)b = 0; continue; }
				cd[0] = tn; for (int i = 0; i < 4; i++) cd[1 + i] = CO[h].q[i];
				for (int i = 0; i < 3; i++) { cd[5 + i] = CO[h].p[i]; cd[8 + i] = CO[h].v[i]; cd[11 + i] = CO[h].w[i]; }
				*(volatile u32 *)(b + 4) = *(volatile u32 *)(b + 4) + 1; *(volatile u32 *)b = 0x4c54434d;   /* 'MCTL' */
			}
			__atomic_thread_fence(__ATOMIC_RELEASE); *seq = (s | 1) + 1;
			frames++;
			if (bts != lastts) { if (lastts) { double g = (bts - lastts) * 1e-6; if (g > maxgap) maxgap = g; } upd++; lastts = bts; }
			{ double age = (tn - tsec) * 1000; agesum += age; agen++; if (age > agemax) agemax = age; }
			best_addr = baddr;
		}
		double tn = now();
		if (tn - lastlog > 5) {
			lastlog = tn;
			L("t+"); Ln(tn - t0, 1); L("s: ");
			if (bp) { L("newest state at "); Lx(best_addr); L(", age "); Ln((tn - bts * 1e-9) * 1000, 1); L(" ms, new states/5s "); Ln((double)upd, 0); L(" (biggest gap "); Ln(maxgap, 1); L(" ms; state age avg "); Ln(agen ? agesum / agen : 0, 1); L(" max "); Ln(agemax, 1); L(" ms)");
				L(", q=("); Ln(F(bp + 0x1c), 3); L(" "); Ln(F(bp + 0x10), 3); L(" "); Ln(F(bp + 0x14), 3); L(" "); Ln(F(bp + 0x18), 3);
				L(") p=("); Ln(F(bp + 0x20), 3); L(" "); Ln(F(bp + 0x24), 3); L(" "); Ln(F(bp + 0x28), 3); L(") flags "); Lx(U32(bp + 4)); }
			else L("no valid head state yet");
			L("\n"); ctl_log(tn); flushlog(1); upd = 0; maxgap = 0; agesum = 0; agemax = 0; agen = 0;
		}
		msleep(1000000);   // 1 kHz
	}
	// tell the driver the Meta position is gone (it then uses its own tracking again)
	*(volatile u32 *)(shm + 96) = 0; *(volatile u32 *)(shm + 256) = 0; *(volatile u32 *)(shm + 384) = 0;
	flushlog(1);
	return 0;
}
__attribute__((naked, noreturn)) void _start(void) { __asm__ volatile("ldr x0, [sp]\n" "add x1, sp, #8\n" "bl q2main\n" "mov x8, #93\n" "svc 0\n"); }
