// q2ctltrack - Touch controller 6DoF tracking from the four tracking cameras (desk-220: LED constellation version).
//
// desk-216..219 only estimated a position from the size of the LED cluster.  desk-220 uses the controllers' real LED
// models (positions + facing directions of the 15 IR LEDs, Meta's factory calibration as stored by Horizon OS in
// /data/vendor/misc/sensors/controllercal; read from a stock Quest 2 - every Touch controller of a side has the same
// layout to within about a millimetre):
//   1. LED frames from /dev/shm/q2cam (q2camd), bright dots -> rays through Meta's fisheye62 camera calibration
//   2. tracking: last pose (+ velocity) -> where each LED should be in each camera -> nearest dots -> least-squares pose
//      over all cameras at once (Gauss-Newton on the ray errors)
//   3. acquisition (lost / first time): dots close together in one camera -> P3P on 3 neighbouring dots against every
//      triangle of nearby LEDs of the left and the right model -> the pose that explains most dots -> refine as above.
//      Which model fits tells which hand it is.
//   4. published in /dev/shm/q2ctlpos (headset frame: x right, y up, z back) for driver_q2hmd
// /dev/shm/q2ctlpos (version 3; desk-222 adds per hand at +768 + 64*h: count, position rel. to head in world axes, world quaternion, valid): u32 seq (seqlock, odd while writing), u32 version, then
//   per hand at +8 + 64*h (h 0 left, 1 right): f64 count, x, y, z (m, origin of the LED model), dots, camera mask, rms (rad), 0
//   per hand at +256 + 64*h: f64 count, qw, qx, qy, qz (rotation model frame -> headset frame)
// desk-221: IMU axes fixed to #5 (= Meta's acc_m); the controller IMU predicts the rotation between frames and its tilt is
// part of every fit; poses out of arm's reach are dropped; a new lock far from the last shown pose needs 3 frames.
// Files: /usr/share/q2vr/q2camcalib.txt (camera calibration), optional /var/lib/q2/ctlmodel_left.txt / _right.txt
// (15 lines "x y z nx ny nz" to use this headset's own controllers' models), /var/lib/q2/cam_mirror (1 = camera slots
// map to calibration Id 3,2,1,0).  Log: /var/log/q2/ctltrack.txt
#define TAG "q2ctltrack: "
#define LOGF "/var/log/q2/ctltrack.txt"
#ifndef Q2CTL_HOST
#include "q2cam_rt.h"
#endif
#include "q2ctl_pnp.h"
#include "q2ctlmodel.h"

#define W 640
#define H 480
#define MAXB 96

// ---------------------------------------------------------------- number parsing (calibration / model files)
static int parse_line(const char **ps, double *v, int maxv)
{
	const char *s = *ps; int nv = 0;
	while (*s && *s != '\n' && nv < maxv) {
		while (*s == ' ' || *s == '\t' || *s == ',') s++;
		if (*s == '\n' || !*s) break;
		double sign = 1, val = 0, scale = 1; int ex = 0, dig = 0;
		if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
		while (*s >= '0' && *s <= '9') { val = val * 10 + (*s++ - '0'); dig = 1; }
		if (*s == '.') { s++; while (*s >= '0' && *s <= '9') { val = val * 10 + (*s++ - '0'); scale *= 10; dig = 1; } }
		if (dig && (*s == 'e' || *s == 'E')) { s++; int es = 1; if (*s == '-') { es = -1; s++; } else if (*s == '+') s++; while (*s >= '0' && *s <= '9') ex = ex * 10 + (*s++ - '0'); ex *= es; }
		val = sign * val / scale; while (ex > 0) { val *= 10; ex--; } while (ex < 0) { val /= 10; ex++; }
		if (dig) v[nv++] = val;
		while (*s && *s != ' ' && *s != '\t' && *s != ',' && *s != '\n') s++;
	}
	while (*s && *s != '\n') s++;
	if (*s) s++;
	*ps = s; return nv;
}

// ---------------------------------------------------------------- cameras
struct calib { int ok; double f, cx, cy, k[6], p[2], T[12]; };
static struct calib cal[4];
static int camid[4] = { 0, 1, 2, 3 };
static struct camx cx[4];                                    // head -> camera, per kernel camera slot
static int load_calib_text(const char *buf)
{
	const char *s = buf; int got = 0;
	while (*s) {
		double v[24]; int nv = parse_line(&s, v, 24);
		if (nv == 24) { int id = (int)v[0]; if (id >= 0 && id < 4) { struct calib *c = &cal[id]; c->f = v[1]; c->cx = v[2]; c->cy = v[3];
			for (int i = 0; i < 6; i++) c->k[i] = v[4 + i]; c->p[0] = v[10]; c->p[1] = v[11]; for (int i = 0; i < 12; i++) c->T[i] = v[12 + i]; c->ok = 1; got++; } }
	}
	return got;
}
static void setup_extrinsics(void)
{
	for (int c = 0; c < 4; c++) {
		const double *T = cal[camid[c]].T;                   // DeviceFromCamera: x_head = Rdc x_cam + tdc
		double Rdc[9] = { T[0], T[1], T[2], T[4], T[5], T[6], T[8], T[9], T[10] }, tdc[3] = { T[3], T[7], T[11] };
		for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) cx[c].Rc[i * 3 + j] = Rdc[j * 3 + i];
		double r[3]; mv3(cx[c].Rc, tdc, r); for (int k = 0; k < 3; k++) cx[c].tc[k] = -r[k];
	}
}
// pixel -> unit ray in camera frame (x right, y down, z forward).  Fisheye62 radial part (Newton on theta)
static void unproject(const struct calib *c, double u, double v, double ray[3])
{
	double mx = (u - c->cx) / c->f, my = (v - c->cy) / c->f, rd = fsqrt(mx * mx + my * my);
	double th = rd;
	for (int i = 0; i < 10; i++) {
		double t2 = th * th, t4 = t2 * t2, t6 = t4 * t2, t8 = t4 * t4, t10 = t8 * t2, t12 = t6 * t6;
		double r = th * (1 + c->k[0] * t2 + c->k[1] * t4 + c->k[2] * t6 + c->k[3] * t8 + c->k[4] * t10 + c->k[5] * t12);
		double dr = 1 + 3 * c->k[0] * t2 + 5 * c->k[1] * t4 + 7 * c->k[2] * t6 + 9 * c->k[3] * t8 + 11 * c->k[4] * t10 + 13 * c->k[5] * t12;
		th -= (r - rd) / dr;
		if (th < 0) th = 0; if (th > 1.75) th = 1.75;
	}
	double s = rd > 1e-9 ? fsin(th) / rd : 1;
	ray[0] = mx * s; ray[1] = my * s; ray[2] = fcos(th);
}
// camera ray -> pixel (for the host test and the log)
static void project(const struct calib *c, const double x[3], double *u, double *v)
{
	double r = fsqrt(x[0] * x[0] + x[1] * x[1]);
	// theta = atan2(r, z) via a few Newton steps on tan
	double th = 0; if (r > 1e-12) { double a = r / (x[2] > 1e-9 ? x[2] : 1e-9); th = a > 1 ? 0.8 : a; for (int i = 0; i < 30; i++) { double s = fsin(th), co = fcos(th); th -= (s * x[2] - co * r) / (co * x[2] + s * r); if (th < 0) th = 0; } }
	double t2 = th * th, t4 = t2 * t2, t6 = t4 * t2, t8 = t4 * t4, t10 = t8 * t2, t12 = t6 * t6;
	double rd = th * (1 + c->k[0] * t2 + c->k[1] * t4 + c->k[2] * t6 + c->k[3] * t8 + c->k[4] * t10 + c->k[5] * t12);
	double mx = r > 1e-12 ? x[0] / r * rd : 0, my = r > 1e-12 ? x[1] / r * rd : 0;
	*u = c->cx + c->f * mx; *v = c->cy + c->f * my;
}

// ---------------------------------------------------------------- blobs + static mask
struct blob { double cx, cy; int area; };
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
			long sx = 0, sy = 0, s = 0; int area = 0;
			while (sp) {
				int p = stack32[--sp], px = p % W, py = p / W, v = img[p];
				area++; s += v; sx += (long)v * px; sy += (long)v * py;
				for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
					int qx = px + dx, qy = py + dy; if (qx < 0 || qy < 0 || qx >= W || qy >= H) continue;
					int q = qy * W + qx; if (seen[q] || img[q] < thr) continue;
					seen[q] = 1; if (sp < W * H) stack32[sp++] = q;
				}
			}
			if (area < 2 || area > 2500) continue;   // desk-222: up to 2500 px (LEDs close to the cameras are big)
			if (n < maxb) { b[n].cx = (double)sx / s; b[n].cy = (double)sy / s; b[n].area = area; n++; }
		}
	return n;
}
// cells of 32x32 px that hold a dot in almost every LED frame for several seconds (lamps, reflections) are skipped when
// looking for a NEW controller (dots already explained by a tracked controller do not count towards it)
#define GX 20
#define GY 15
static float occ[4][GY][GX];
static void mask_update(int cam, const struct blob *b, const unsigned char *taken, int n)
{
	static uint8_t hit[GY][GX];
	memset(hit, 0, sizeof hit);
	for (int i = 0; i < n; i++) { if (taken[i]) continue; int gx = (int)b[i].cx / 32, gy = (int)b[i].cy / 32; for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) { int x = gx + dx, y = gy + dy; if (x >= 0 && x < GX && y >= 0 && y < GY) hit[y][x] = 1; } }
	for (int y = 0; y < GY; y++) for (int x = 0; x < GX; x++) occ[cam][y][x] = occ[cam][y][x] * 0.995f + (hit[y][x] ? 0.005f : 0);
}
static int masked(int cam, const struct blob *b) { int gx = (int)b->cx / 32, gy = (int)b->cy / 32; return gx >= 0 && gx < GX && gy >= 0 && gy < GY && occ[cam][gy][gx] > 0.6f; }

// per camera: the newest LED frame's dots
struct camframe { int fresh, nb; double t; struct blob b[MAXB]; double bear[MAXB][3]; unsigned char taken[MAXB]; };
static struct camframe cf[4];

// ---------------------------------------------------------------- hands
static struct cmodel mdl[2];
static double rots[24][9];
static int imuaxis[2];
static int imu_get2(int h, double ub[3], double uh[3], double bq[4], double hq[4]);
struct hand { int valid; double R[9], t[3], v[3], tlast; double count; int nobs, cams; double rms; double pub_t[3], pub_q[4]; int pubvalid;
	int streak, imuok; double bq[4], hq[4]; };   // desk-221: confirmation streak, IMU/head orientation at the last fit
static struct hand hands[2];
static uint8_t *pub;
static double sigma = 0.003;                                // typical fit error of a tracked controller (rad)
static unsigned st_noimu, st_unconf, st_implaus, st_multi, st_tilt, st_ambig, st_track[2], st_acq[2], st_acqtry, st_lostrms[2], st_lostn[2];
static double st_rms[2]; static unsigned st_nrms[2];
static void publish_hand(int h)
{
	struct hand *hd = &hands[h];
	double q[4]; m2q(hd->R, q);
	if (!hd->pubvalid) { for (int k = 0; k < 3; k++) hd->pub_t[k] = hd->t[k]; for (int k = 0; k < 4; k++) hd->pub_q[k] = q[k]; hd->pubvalid = 1; }
	else {
		// light smoothing (camera noise); the driver blends with the IMU on top of this
		for (int k = 0; k < 3; k++) hd->pub_t[k] = hd->t[k];   // desk-222: no smoothing here (the driver filters with velocity)
		double d = q[0] * hd->pub_q[0] + q[1] * hd->pub_q[1] + q[2] * hd->pub_q[2] + q[3] * hd->pub_q[3]; double sg = d < 0 ? -1 : 1;
		for (int k = 0; k < 4; k++) hd->pub_q[k] += 0.8 * (sg * q[k] - hd->pub_q[k]);
		double n = fsqrt(hd->pub_q[0] * hd->pub_q[0] + hd->pub_q[1] * hd->pub_q[1] + hd->pub_q[2] * hd->pub_q[2] + hd->pub_q[3] * hd->pub_q[3]);
		for (int k = 0; k < 4; k++) hd->pub_q[k] /= n;
	}
	hd->count += 1;
	if (!pub) return;
	volatile uint32_t *seq = (volatile uint32_t *)pub;
	uint32_t s0 = *seq; *seq = s0 + 1; __asm__ volatile("" ::: "memory");
#ifdef __aarch64__
	__asm__ volatile("dmb ish" ::: "memory");
#endif
	double *d = (double *)(pub + 8 + h * 64);
	d[0] = hd->count; d[1] = hd->pub_t[0]; d[2] = hd->pub_t[1]; d[3] = hd->pub_t[2]; d[4] = hd->nobs; d[5] = hd->cams; d[6] = hd->rms; d[7] = 0;
	double *e = (double *)(pub + 256 + h * 64);
	e[0] = hd->count; e[1] = hd->pub_q[0]; e[2] = hd->pub_q[1]; e[3] = hd->pub_q[2]; e[4] = hd->pub_q[3];
#ifdef __aarch64__
	__asm__ volatile("dmb ish" ::: "memory");
#endif
	// desk-222 (v3): the same pose turned into the world with the head orientation at the time of the fit
	double *wv = (double *)(pub + 768 + h * 64);
	if (hd->imuok && hd->hq[0] * hd->hq[0] + hd->hq[1] * hd->hq[1] + hd->hq[2] * hd->hq[2] + hd->hq[3] * hd->hq[3] > 0.5) {
		double Hm[9], pw[3], qw[4]; q2m(hd->hq, Hm); mv3(Hm, hd->pub_t, pw);
		double hq[4] = { hd->hq[0], hd->hq[1], hd->hq[2], hd->hq[3] }, pq[4] = { hd->pub_q[0], hd->pub_q[1], hd->pub_q[2], hd->pub_q[3] };
		qw[0] = hq[0] * pq[0] - hq[1] * pq[1] - hq[2] * pq[2] - hq[3] * pq[3]; qw[1] = hq[0] * pq[1] + hq[1] * pq[0] + hq[2] * pq[3] - hq[3] * pq[2];
		qw[2] = hq[0] * pq[2] - hq[1] * pq[3] + hq[2] * pq[0] + hq[3] * pq[1]; qw[3] = hq[0] * pq[3] + hq[1] * pq[2] - hq[2] * pq[1] + hq[3] * pq[0];
		wv[0] = hd->count; wv[1] = pw[0]; wv[2] = pw[1]; wv[3] = pw[2]; wv[4] = qw[0]; wv[5] = qw[1]; wv[6] = qw[2]; wv[7] = qw[3]; wv[8] = 1;
	} else { wv[0] = hd->count; wv[8] = 0; }
	double *a = (double *)(pub + 512 + h * 128);
	a[0] = imuaxis[h]; for (int k = 0; k < 9; k++) a[1 + k] = imuaxis[h] >= 0 ? rots[imuaxis[h]][k] : 0;
#ifdef __aarch64__
	__asm__ volatile("dmb ish" ::: "memory");
#endif
	*seq = s0 + 2;
}

// collect matches for pose (R,t) (head frame) in every fresh camera, then refine.  Returns obs count, rms in *rms
#define MAXOBS 64
static int fit_all(int h, double R[9], double t[3], double thr, int iters, double *rms, int *cams, struct obs *o)
{
	int n = 0; *cams = 0;
	for (int c = 0; c < 4; c++) {
		if (!cf[c].fresh) continue;
		int k = match_cam(&mdl[h], cx, c, R, t, (const double (*)[3])cf[c].bear, cf[c].nb, cf[c].taken, thr, o + n, MAXOBS - n, 0);
		if (k) *cams |= 1 << c;
		n += k;
	}
	// desk-221: the controller IMU's tilt as an extra constraint (weight: 2 deg of tilt ~ 1 px)
	double pr[7], ub[3], hq[4], bq[4]; const double *prp = 0;
	if (imuaxis[h] >= 0 && imu_get2(h, ub, pr, bq, hq)) { mv3(rots[imuaxis[h]], ub, pr + 3); pr[6] = 0.12; prp = pr; }
	*rms = n >= 3 ? refine_p(&mdl[h], cx, o, n, R, t, iters, prp) : 1;
	return n;
}
static void mark_taken(const struct obs *o, int n)
{
	for (int i = 0; i < n; i++) { struct camframe *f = &cf[o[i].cam]; int b = blob_of((const double (*)[3])f->bear, f->nb, o[i].f); if (b >= 0) f->taken[b] = 1; }
}
// head rotation between two head orientations as a headset-frame rotation: Hd = Qh_new^-1 * Qh_old
static void head_delta(const double qo[4], const double qn[4], double Hd[9])
{
	double Ro[9], Rn[9]; q2m(qo, Ro); q2m(qn, Rn); mtm3(Rn, Ro, Hd);
}
static void accept(int h, const double R[9], const double t[3], double tnow, int n, int cams, double rms, int continuing)
{
	struct hand *hd = &hands[h];
	double ub[3], uh[3], bq[4], hq[4]; int imuok = imu_get2(h, ub, uh, bq, hq);
	if (!imuok) st_noimu++;
	if (continuing && hd->valid && tnow - hd->tlast < 0.3 && tnow > hd->tlast) {
		double dt = tnow - hd->tlast, to[3];
		if (imuok && hd->imuok) { double Hd[9]; head_delta(hd->hq, hq, Hd); mv3(Hd, hd->t, to); } else for (int k = 0; k < 3; k++) to[k] = hd->t[k];
		for (int k = 0; k < 3; k++) { double vv = (t[k] - to[k]) / dt; hd->v[k] += 0.5 * (vv - hd->v[k]); }
		hd->streak++;
	} else {
		for (int k = 0; k < 3; k++) hd->v[k] = 0;
		// a new lock: published at once only if it is where the hand was last shown; else after 3 frames in a row
		double e[3] = { t[0] - hd->pub_t[0], t[1] - hd->pub_t[1], t[2] - hd->pub_t[2] };
		hd->streak = (hd->pubvalid && dot3(e, e) < 0.12 * 0.12 && tnow - hd->tlast < 1.0) ? 3 : 1;
	}
	for (int k = 0; k < 9; k++) hd->R[k] = R[k]; for (int k = 0; k < 3; k++) hd->t[k] = t[k];
	hd->imuok = imuok; if (imuok) for (int k = 0; k < 4; k++) { hd->bq[k] = bq[k]; hd->hq[k] = hq[k]; }
	hd->valid = 1; hd->tlast = tnow; hd->nobs = n; hd->cams = cams; hd->rms = rms;
	st_rms[h] += rms; st_nrms[h]++; if (cams & (cams - 1)) st_multi++;
	if (hd->streak == 3 && !continuing) {}                    // close to the last shown pose: keep smoothing
	else if (hd->streak == 3) hd->pubvalid = 0;                 // confirmed new lock: jump there (no smoothing across)
	if (hd->streak >= 3) publish_hand(h); else st_unconf++;
}
// the controller must be within arm's reach of the headset: not above the head, not far behind it
static int plausible(const double t[3])
{
	double d = norm3(t);
	return d > 0.08 && d < 1.0 && t[1] < 0.35 && t[2] < 0.30;
}
static int missing(int h, const double R[9], const double t[3])
{
	int miss = 0;
	for (int c = 0; c < 4; c++) {
		if (!cf[c].fresh) continue;
		double Rc[9], tc[3]; mm3(cx[c].Rc, R, Rc); mv3(cx[c].Rc, t, tc); for (int k = 0; k < 3; k++) tc[k] += cx[c].tc[k];
		for (int i = 0; i < mdl[h].n; i++) {
			double xc[3]; if (!led_visible(&mdl[h], i, Rc, tc, xc) || xc[2] < 0.05) continue;
			double nn = norm3(xc), d[3] = { xc[0] / nn, xc[1] / nn, xc[2] / nn };
			if (d[2] < 0.3) continue;                                    // > ~72 deg off-axis: may be outside the image
			double u, v; project(&cal[camid[c]], xc, &u, &v);
			if (u < 8 || v < 8 || u > W - 8 || v > H - 8) continue;
			int near = 0; for (int b = 0; b < cf[c].nb && !near; b++) if (dot3(d, cf[c].bear[b]) > 0.99955) near = 1;   // 0.03 rad
			if (!near) miss++;
		}
	}
	return miss;
}

// ---------------------------------------------------------------- controller IMU (from q2headtrack, /dev/shm/q2ctl)
// at 2048 + 128*h: counter, up vector in the IMU frame (fused tilt), IMU-frame orientation quaternion, head up vector
// in the headset frame.  How the IMU frame sits in the LED model frame (A: v_model = A v_imu) is LEARNED here: while a
// controller is tracked well, the camera pose says where "up" is in the model frame and the IMU says where it is in
// its own frame; of the 24 axis-aligned rotations the one that agrees over many different poses is kept
// (/var/lib/q2/ctl_imuaxis_left / _right).  Once known, it rejects camera fits whose tilt disagrees with the IMU
// (flipped or ring-rotated fits) and lets driver_q2hmd fuse IMU rotation with the camera pose.
static volatile const uint32_t *ctlshm;
static int imuaxis[2] = { 5, 5 };   // desk-221: #5 = rotX(90) = Meta's acc_m (desk-220 learned it on the left; same for the right after the head tracker's mirror)
static double axerr[2][24]; static int axn[2]; static double axsum[2][3];
static void make_rots(void)
{
	int n = 0;
	static const int pm[6][3] = { { 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 }, { 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 } };
	for (int p = 0; p < 6; p++) for (int s = 0; s < 8; s++) {
		double M[9] = { 0 };
		for (int r = 0; r < 3; r++) M[r * 3 + pm[p][r]] = (s >> r & 1) ? -1 : 1;
		double det = M[0] * (M[4] * M[8] - M[5] * M[7]) - M[1] * (M[3] * M[8] - M[5] * M[6]) + M[2] * (M[3] * M[7] - M[4] * M[6]);
		if (det > 0 && n < 24) { for (int k = 0; k < 9; k++) rots[n][k] = M[k]; n++; }
	}
}
// returns 1 with up_imu (IMU frame) and up_head (headset frame) if the controller's IMU data is fresh
static int imu_get(int h, double ub[3], double uh[3])
{
	static double lastc[2]; static int stale[2];
	if (!ctlshm) return 0;
	for (int k = 0; k < 400000; k++) {
		uint32_t s0 = ctlshm[0]; if (s0 & 1) continue;
		__asm__ volatile("" ::: "memory");
		const volatile double *d = (const volatile double *)((const volatile char *)ctlshm + 2048 + h * 128);
		double c = d[0], u0 = d[1], u1 = d[2], u2 = d[3], h0 = d[8], h1 = d[9], h2 = d[10];
		__asm__ volatile("" ::: "memory");
		if (ctlshm[0] != s0) continue;
		if (c <= 0) return 0;
		if (c != lastc[h]) { lastc[h] = c; stale[h] = 0; } else if (++stale[h] > 30) return 0;   // no new IMU data for ~1 s
		ub[0] = u0; ub[1] = u1; ub[2] = u2; uh[0] = h0; uh[1] = h1; uh[2] = h2;
		return dot3(ub, ub) > 0.5 && dot3(uh, uh) > 0.5;
	}
	return 0;
}
// desk-221: also the IMU orientation (world, IMU frame) and the head orientation (world)
static int imu_get2(int h, double ub[3], double uh[3], double bq[4], double hq[4])
{
	static double lastc[2]; static int stale[2];
	if (!ctlshm) return 0;
	for (int k = 0; k < 400000; k++) {
		uint32_t s0 = ctlshm[0]; if (s0 & 1) continue;
		__asm__ volatile("" ::: "memory");
		const volatile double *d = (const volatile double *)((const volatile char *)ctlshm + 2048 + h * 128);
		double v[15]; for (int i = 0; i < 15; i++) v[i] = d[i];
		__asm__ volatile("" ::: "memory");
		if (ctlshm[0] != s0) continue;
		if (v[0] <= 0) return 0;
		if (v[0] != lastc[h]) { lastc[h] = v[0]; stale[h] = 0; } else if (++stale[h] > 30) return 0;
		for (int i = 0; i < 3; i++) { ub[i] = v[1 + i]; uh[i] = v[8 + i]; }
		for (int i = 0; i < 4; i++) { bq[i] = v[4 + i]; hq[i] = v[11 + i]; }
		if (dot3(hq + 1, hq + 1) + hq[0] * hq[0] < 0.5) { hq[0] = 1; hq[1] = hq[2] = hq[3] = 0; }   // desk-220 head tracker: no head quaternion
		return dot3(ub, ub) > 0.5 && dot3(uh, uh) > 0.5;
	}
	return 0;
}
// cosine between the camera pose's up (model frame) and the IMU's up mapped into the model frame; 2 = unknown
static double imu_agree(int h, const double R[9])
{
	double ub[3], uh[3];
	if (imuaxis[h] < 0 || !imu_get(h, ub, uh)) return 2;
	double um[3], ui[3]; mtv3(R, uh, um); mv3(rots[imuaxis[h]], ub, ui);
	return dot3(um, ui);
}
static void imu_learn(int h, const double R[9])
{
	double ub[3], uh[3];
	if (imuaxis[h] >= 0 || !imu_get(h, ub, uh)) return;
	double um[3]; mtv3(R, uh, um);
	for (int k = 0; k < 24; k++) { double ui[3]; mv3(rots[k], ub, ui); double e = 1 - dot3(um, ui); axerr[h][k] += e > 0.5 ? 0.5 : e; }
	for (int k = 0; k < 3; k++) axsum[h][k] += ub[k];
	if (++axn[h] < 150) return;
	int b = 0, b2 = -1; for (int k = 1; k < 24; k++) if (axerr[h][k] < axerr[h][b]) b = k;
	for (int k = 0; k < 24; k++) if (k != b && (b2 < 0 || axerr[h][k] < axerr[h][b2])) b2 = k;
	double mb = axerr[h][b] / axn[h], m2 = axerr[h][b2] / axn[h], spread = norm3(axsum[h]) / axn[h];
	if (mb < 0.03 && m2 > 3 * mb + 0.02 && spread < 0.95) {
		imuaxis[h] = b;
		P("%s controller: IMU axes in the LED model frame learned: rotation #%u (mean error %u deg-ish, next best %u), saved",
		  h ? "right" : "left", (unsigned)b, (unsigned)(fsqrt(2 * mb) * 57.3), (unsigned)(fsqrt(2 * m2) * 57.3));
		int fd = sys_open(h ? "/var/lib/q2/ctl_imuaxis_right" : "/var/lib/q2/ctl_imuaxis_left", 01 | 0100 | 01000);
		if (fd >= 0) { char t[4] = { (char)('0' + b / 10), (char)('0' + b % 10), '\n', 0 }; sc6(64, fd, (long)t, 3, 0, 0, 0); sys_close(fd); }
	} else if (axn[h] >= 3000) {
		P("%s controller: IMU axes still unclear after %u samples (best %u, spread %u%%) - turn it around more", h ? "right" : "left", (unsigned)axn[h], (unsigned)b, (unsigned)(spread * 100));
		for (int k = 0; k < 24; k++) axerr[h][k] = 0; axn[h] = 0; axsum[h][0] = axsum[h][1] = axsum[h][2] = 0;
	}
}
#define RMS_OK 0.012                                          // ~3 px at the cameras' 242 px/rad
static void track_step(double tnow)
{
	static struct obs o[MAXOBS];
	int done[2] = { 0, 0 };
	// 1. tracking from the last pose
	for (int h = 0; h < 2; h++) {
		struct hand *hd = &hands[h];
		if (!hd->valid || tnow - hd->tlast > 0.25) continue;
		double R[9], t[3], dt = tnow - hd->tlast; if (dt > 0.1) dt = 0.1;
		for (int k = 0; k < 9; k++) R[k] = hd->R[k];
		for (int k = 0; k < 3; k++) t[k] = hd->t[k] + hd->v[k] * dt;
		// desk-221: predict the rotation since the last fit from the controller IMU and the head IMU:
		// R_new = Hd * R_old * A * Bold^T * Bnew * A^T  (Hd = head rotation, B = controller IMU orientation)
		{ double ub[3], uh[3], bq[4], hq[4];
		  if (hd->imuok && imuaxis[h] >= 0 && imu_get2(h, ub, uh, bq, hq)) {
			double Hd[9], Bo[9], Bn[9], D[9], M[9], tt[3]; const double *A = rots[imuaxis[h]];
			head_delta(hd->hq, hq, Hd); q2m(hd->bq, Bo); q2m(bq, Bn);
			mtm3(Bo, Bn, D);                                   // Bold^T Bnew (IMU frame)
			mm3(A, D, M); double At[9] = { A[0], A[3], A[6], A[1], A[4], A[7], A[2], A[5], A[8] }; mm3(M, At, M);   // in the model frame
			mm3(R, M, R); mm3(Hd, R, R); rot_fix(R);
			mv3(Hd, t, tt); for (int k = 0; k < 3; k++) t[k] = tt[k];
		  } }
		double rms; int cams;
		int n = fit_all(h, R, t, 0.12, 6, &rms, &cams, o);
		if (n < 4) { st_lostn[h]++; continue; }
		n = fit_all(h, R, t, 0.03, 8, &rms, &cams, o);
		int need = imuaxis[h] >= 0 && hd->imuok ? 4 : 5;            // 4 LEDs only with the IMU tilt helping
		if (n < need || rms > RMS_OK) { if (n < need) st_lostn[h]++; else st_lostrms[h]++; continue; }
		if (!plausible(t)) { st_implaus++; hd->valid = 0; continue; }
		{ static int bad[2]; int miss = missing(h, R, t);
		  if (miss > n / 2 + 1 || imu_agree(h, R) < 0.906) { if (++bad[h] >= 8) { bad[h] = 0; hd->valid = 0; st_lostrms[h]++; continue; } } else bad[h] = 0; }
		mark_taken(o, n);
		accept(h, R, t, tnow, n, cams, rms, 1); st_track[h]++; done[h] = 1;
		if (n >= 7 && rms < 1.5 * sigma) imu_learn(h, R);
		if (n >= 7) { sigma += 0.02 * (rms - sigma); if (sigma < 0.0015) sigma = 0.0015; if (sigma > 0.006) sigma = 0.006; }
	}
	if (done[0] && done[1]) return;
	// 2. acquisition: clusters of unexplained, unmasked dots in each fresh camera
	for (int c = 0; c < 4; c++) {
		struct camframe *f = &cf[c];
		if (!f->fresh) continue;
		unsigned char cl[MAXB]; for (int i = 0; i < f->nb; i++) cl[i] = f->taken[i] || masked(c, &f->b[i]);
		int tries = 0;
		for (int i = 0; i < f->nb && tries < 3; i++) {
			if (cl[i]) continue;
			int idx[MAXB], m = 0; idx[m++] = i; cl[i] = 1;
			for (int a = 0; a < m; a++) for (int j = 0; j < f->nb; j++) {
				if (cl[j]) continue;
				double dx = f->b[j].cx - f->b[idx[a]].cx, dy = f->b[j].cy - f->b[idx[a]].cy;
				if (dx * dx + dy * dy < 160 * 160) { cl[j] = 1; idx[m++] = j; }   // desk-222: 160 px (was 110: a close controller split up)
			}
			if (m < 5) continue;
			// biggest dots first (seeds), at most 14
			for (int a = 1; a < m; a++) { int x = idx[a], b = a - 1; while (b >= 0 && f->b[idx[b]].area < f->b[x].area) { idx[b + 1] = idx[b]; b--; } idx[b + 1] = x; }
			if (m > 14) m = 14;
			tries++; st_acqtry++;
			// candidates from both models; each one refined over all cameras and judged by how well it explains the dots:
			// matched LEDs, minus LEDs it says should be visible in a camera but have no dot, minus fit error
			static struct acq best[2][NACQ];
			int bh = -1; double s2 = -1e9, bscore = -1e9, bR[9] = { 0 }, bt[3] = { 0 }, brms = 1; int bn = 0, bcams = 0;
			for (int h = 0; h < 2; h++) {
				if (done[h]) continue;
				acquire(&mdl[h], (const double (*)[3])f->bear, idx, m, best[h], 0.03);
				for (int k = 0; k < NACQ; k++) {
					if (best[h][k].inl < 5) continue;
					// camera frame -> head frame: R_h = Rc^T R, t_h = Rc^T (t - tc)
					double R[9], t[3], tt[3];
					mtm3(cx[c].Rc, best[h][k].R, R);
					for (int q = 0; q < 3; q++) tt[q] = best[h][k].t[q] - cx[c].tc[q];
					mtv3(cx[c].Rc, tt, t);
					double rms; int cams;
					int n = fit_all(h, R, t, 0.03, 10, &rms, &cams, o);
					if (n >= 5) n = fit_all(h, R, t, 0.025, 8, &rms, &cams, o);
					if (n < 5 || rms > RMS_OK) continue;
					if (!plausible(t)) { st_implaus++; continue; }
					if (imu_agree(h, R) < 0.866) { st_tilt++; continue; }          // > 30 deg from the IMU's tilt
					int miss = missing(h, R, t);
					double score = n - 0.7 * miss - rms * 2500;
					// left and right LED layouts are mirror images and can partly fit each other: prefer the hand that
					// was last seen near here, else the one on its own side of the headset
					struct hand *hd = &hands[h];
					double dd = 1e9; if (hd->valid && tnow - hd->tlast < 1.5) { double e[3] = { t[0] - hd->t[0], t[1] - hd->t[1], t[2] - hd->t[2] }; dd = dot3(e, e); }
					if (dd < 0.15 * 0.15) score += 3;
					else if ((h == 0) == (t[0] < 0)) score += 1.5;
					if (score > bscore) {
						if (bh >= 0) { double e[3] = { t[0] - bt[0], t[1] - bt[1], t[2] - bt[2] }, tr = 0; for (int q = 0; q < 9; q++) tr += R[q] * bR[q];
							if (bh != h || dot3(e, e) > 0.02 * 0.02 || tr < 1 + 2 * 0.985) s2 = bscore; }   // old best is a different pose
						bscore = score; bh = h; bn = n; bcams = cams; brms = rms; for (int q = 0; q < 9; q++) bR[q] = R[q]; for (int q = 0; q < 3; q++) bt[q] = t[q];
					} else if (score > s2) {
						double e[3] = { t[0] - bt[0], t[1] - bt[1], t[2] - bt[2] }, tr = 0; for (int q = 0; q < 9; q++) tr += R[q] * bR[q];
						if (bh != h || dot3(e, e) > 0.02 * 0.02 || tr < 1 + 2 * 0.985) s2 = score;
					}
				}
			}
			// accept only a clear winner that fits about as well as tracked controllers normally do
			if (bh < 0 || bscore - s2 < 1.5 || brms > 1.7 * sigma) { if (bh >= 0) st_ambig++; continue; }
			double R[9], t[3], rms; int cams, n;
			for (int q = 0; q < 9; q++) R[q] = bR[q]; for (int q = 0; q < 3; q++) t[q] = bt[q];
			n = fit_all(bh, R, t, 0.025, 4, &rms, &cams, o); (void)bn; (void)bcams; (void)brms;
			if (n < 5 || rms > RMS_OK || !plausible(t)) continue;
			mark_taken(o, n); for (int a = 0; a < f->nb; a++) if (f->taken[a]) cl[a] = 1;
			if (hands[bh].valid && tnow - hands[bh].tlast < 0.25) hands[bh].valid = 0;   // re-acquired: no velocity across it
			accept(bh, R, t, tnow, n, cams, rms, 0); st_acq[bh]++; done[bh] = 1;
		}
	}
}

#ifndef Q2CTL_HOST
static void putf(char **o, double v) { if (v < 0) { *(*o)++ = '-'; v = -v; } unsigned i = (unsigned)v; unsigned f = (unsigned)((v - i) * 100); char t[12]; int k = 0; do { t[k++] = '0' + i % 10; i /= 10; } while (i); while (k) *(*o)++ = t[--k]; *(*o)++ = '.'; *(*o)++ = '0' + f / 10; *(*o)++ = '0' + f % 10; }
static void puts_(char **o, const char *s) { while (*s) *(*o)++ = *s++; }
static void putu(char **o, unsigned v) { char t[12]; int k = 0; do { t[k++] = '0' + v % 10; v /= 10; } while (v); while (k) *(*o)++ = t[--k]; }
// direction in words (headset frame: x right, y up, z back)
static void putdir(char **o, const double d[3])
{
	const char *nm[6] = { "right", "left", "up", "down", "back", "ahead" };
	int ax = 0; for (int k = 1; k < 3; k++) if (fabs_(d[k]) > fabs_(d[ax])) ax = k;
	puts_(o, nm[ax * 2 + (d[ax] < 0)]);
	int a2 = -1; for (int k = 0; k < 3; k++) if (k != ax && fabs_(d[k]) > 0.35 && (a2 < 0 || fabs_(d[k]) > fabs_(d[a2]))) a2 = k;
	if (a2 >= 0) { puts_(o, "+"); puts_(o, nm[a2 * 2 + (d[a2] < 0)]); }
}
static int load_model_file(const char *path, struct cmodel *m)
{
	static char buf[4096]; int fd = sys_open(path, 0); if (fd < 0) return 0;
	long n = sc6(63, fd, (long)buf, sizeof buf - 1, 0, 0, 0); sys_close(fd); if (n <= 0) return 0; buf[n] = 0;
	double led[MAXLED][6]; int nl = 0; const char *s = buf;
	while (*s && nl < MAXLED) { double v[6]; if (parse_line(&s, v, 6) == 6) { for (int k = 0; k < 6; k++) led[nl][k] = v[k]; nl++; } }
	if (nl < 8) return 0;
	model_init(m, (const double (*)[6])led, nl); return nl;
}

static uint8_t frame[W * H];
static float pmean[4][2];

int main(int argc, char **argv)
{
	logfd = sys_open(LOGF, 01 | 0100 | 01000);
	{ static char buf[8192]; int fd = sys_open("/usr/share/q2vr/q2camcalib.txt", 0); long n = -1; if (fd >= 0) { n = sc6(63, fd, (long)buf, sizeof buf - 1, 0, 0, 0); sys_close(fd); }
	  if (n <= 0 || load_calib_text((buf[n] = 0, buf)) < 4) { P("calibration missing (/usr/share/q2vr/q2camcalib.txt)"); return 1; } }
	{ char b[8]; int fd = sys_open("/var/lib/q2/cam_mirror", 0); if (fd >= 0) { long n = sc6(63, fd, (long)b, 7, 0, 0, 0); sys_close(fd);
	  if (n > 0 && b[0] == '1') { camid[0] = 3; camid[1] = 2; camid[2] = 1; camid[3] = 0; P("cam_mirror=1: camera slots mapped to calibration Id 3,2,1,0"); } } }
	setup_extrinsics(); make_rots();
	for (int h = 0; h < 2; h++) { char b[8]; int fd = sys_open(h ? "/var/lib/q2/ctl_imuaxis_right" : "/var/lib/q2/ctl_imuaxis_left", 0);
	  if (fd >= 0) { long n = sc6(63, fd, (long)b, 7, 0, 0, 0); sys_close(fd); if (n >= 1 && b[0] >= '0' && b[0] <= '9') { int v = b[0] - '0'; if (n >= 2 && b[1] >= '0' && b[1] <= '9') v = v * 10 + b[1] - '0'; if (v < 24) { imuaxis[h] = v; P("%s controller: IMU axes from the saved file: rotation #%u", h ? "right" : "left", (unsigned)v); } } } }
	{ int fd = sys_open("/dev/shm/q2ctl", 0); if (fd >= 0) { ctlshm = (volatile const uint32_t *)sys_mmap(4096, 1, 1, fd); sys_close(fd); P("controller IMU data: /dev/shm/q2ctl"); } else P("no /dev/shm/q2ctl yet (head tracker not running?) - retrying later"); }
	int nl0 = load_model_file("/var/lib/q2/ctlmodel_left.txt", &mdl[0]), nl1 = load_model_file("/var/lib/q2/ctlmodel_right.txt", &mdl[1]);
	if (!nl0) model_init(&mdl[0], k_led_left, 15);
	if (!nl1) model_init(&mdl[1], k_led_right, 15);
	P("start (desk-220 constellation): LED models left %u LEDs (%s), right %u LEDs (%s), %u/%u seed triangles", (unsigned)mdl[0].n, nl0 ? "this headset's file" : "built in", (unsigned)mdl[1].n, nl1 ? "this headset's file" : "built in", (unsigned)mdl[0].ntri, (unsigned)mdl[1].ntri);
	int pf = sys_open("/dev/shm/q2ctlpos", 02 | 0100);
	if (pf < 0 || sc6(46, pf, 4096, 0, 0, 0, 0) < 0) { P("cannot create /dev/shm/q2ctlpos"); return 1; }
	sc6(52, pf, 0644, 0, 0, 0, 0);
	pub = sys_mmap(4096, 3, 1, pf); sys_close(pf);
	memset(pub, 0, 4096); ((uint32_t *)pub)[1] = 3;
	int fd = -1;
	for (int i = 0; i < 240 && fd < 0; i++) { fd = sys_open("/dev/shm/q2cam", 0); if (fd < 0) sleep_ms(500); }
	if (fd < 0) { P("/dev/shm/q2cam not found"); return 1; }
	uint32_t hdr[8]; sc6(63, fd, (long)hdr, sizeof hdr, 0, 0, 0);
	if (hdr[0] != 0x4d433251) { P("bad shm header"); return 1; }
	uint32_t nslots = hdr[5], hsz = hdr[6], stride = hdr[7];
	const uint8_t *shm = sys_mmap(hsz + (size_t)4 * nslots * stride, 1, 1, fd); sys_close(fd);
	uint64_t last[4]; for (int c = 0; c < 4; c++) last[c] = *(volatile const uint64_t *)(shm + 0x40 + c * 64);
	double t0 = now(), tlog = t0; unsigned nled = 0, nsteps = 0;
	for (;;) {
		int any = 0;
		for (int c = 0; c < 4; c++) {
			uint64_t pubn = *(volatile const uint64_t *)(shm + 0x40 + c * 64);
			if (pubn == last[c]) continue;
			last[c] = pubn;
			const uint8_t *slot = shm + hsz + (size_t)(c * nslots + (uint32_t)((pubn - 1) % nslots)) * stride;
			uint64_t s1 = *(volatile const uint64_t *)slot; if (s1 & 1) continue;
			uint64_t fn = ((const uint64_t *)slot)[1]; unsigned mean = ((const uint32_t *)slot)[8];
			pmean[c][fn & 1] = pmean[c][fn & 1] * 0.95f + mean * 0.05f;
			if (pmean[c][fn & 1] > pmean[c][(fn & 1) ^ 1]) continue;          // scene frame
			memcpy(frame, slot + 64, W * H);
			if (*(volatile const uint64_t *)slot != s1) continue;
			nled++; any = 1;
			struct camframe *f = &cf[c];
			f->nb = find_blobs(frame, 64, f->b, MAXB); f->fresh = 1; f->t = now();
			for (int i = 0; i < f->nb; i++) { unproject(&cal[camid[c]], f->b[i].cx, f->b[i].cy, f->bear[i]); f->taken[i] = 0; }
		}
		// the four cameras are exposed together: wait up to 12 ms after the first new LED frame for the others (desk-222; was 4 ms)
		static double tfirst = 0;
		if (any && !tfirst) tfirst = now();
		int nf = 0; for (int c = 0; c < 4; c++) nf += cf[c].fresh;
		if (nf && (nf == 4 || now() - tfirst > 0.012)) {
			track_step(now()); nsteps++;
			for (int c = 0; c < 4; c++) if (cf[c].fresh) { mask_update(c, cf[c].b, cf[c].taken, cf[c].nb); cf[c].fresh = 0; }
			tfirst = 0;
		}
		double t = now();
		if (!ctlshm && t - tlog >= 0.9) { int fd = sys_open("/dev/shm/q2ctl", 0); if (fd >= 0) { ctlshm = (volatile const uint32_t *)sys_mmap(4096, 1, 1, fd); sys_close(fd); P("controller IMU data found: /dev/shm/q2ctl"); } }
		if (t - tlog >= (t - t0 < 600 ? 1.0 : 10.0)) {
			char b[600]; char *o = b;
			puts_(&o, "LED frames "); putu(&o, nled); puts_(&o, ", steps "); putu(&o, nsteps); puts_(&o, ", search tries "); putu(&o, st_acqtry); puts_(&o, " (unclear "); putu(&o, st_ambig); puts_(&o, ", tilt mismatch "); putu(&o, st_tilt); puts_(&o, ", out of reach "); putu(&o, st_implaus); puts_(&o, ", unconfirmed "); putu(&o, st_unconf); puts_(&o, ", fits without IMU "); putu(&o, st_noimu); puts_(&o, "), seen by 2+ cameras "); putu(&o, st_multi); puts_(&o, ", typical error "); putf(&o, sigma * 242); puts_(&o, " px");
			for (int h = 0; h < 2; h++) {
				struct hand *hd = &hands[h];
				puts_(&o, h ? " | RIGHT " : " | LEFT ");
				puts_(&o, "tracked "); putu(&o, st_track[h]); puts_(&o, " found "); putu(&o, st_acq[h]);
				puts_(&o, " lost(few dots "); putu(&o, st_lostn[h]); puts_(&o, ", bad fit "); putu(&o, st_lostrms[h]); puts_(&o, ")");
				if (!hd->valid || t - hd->tlast > 0.5) { puts_(&o, " - not seen"); continue; }
				puts_(&o, " at x "); putf(&o, hd->t[0]); puts_(&o, " y "); putf(&o, hd->t[1]); puts_(&o, " z "); putf(&o, hd->t[2]);
				puts_(&o, " ("); putu(&o, hd->nobs); puts_(&o, " LEDs, cams 0x"); putu(&o, hd->cams); puts_(&o, ", err ");
				putf(&o, st_nrms[h] ? st_rms[h] / st_nrms[h] * 242 : 0); puts_(&o, " px), ring top points ");
				double y[3] = { hd->R[1], hd->R[4], hd->R[7] }, z[3] = { -hd->R[2], -hd->R[5], -hd->R[8] };
				putdir(&o, y); puts_(&o, ", model -z points "); putdir(&o, z);
			}
			*o = 0; P("%s", b);
			for (int h = 0; h < 2; h++) { st_track[h] = st_acq[h] = st_lostn[h] = st_lostrms[h] = 0; st_rms[h] = 0; st_nrms[h] = 0; }
			nled = nsteps = st_acqtry = st_ambig = st_tilt = st_multi = st_implaus = st_unconf = st_noimu = 0; tlog = t;
		}
		if (!any) sleep_ms(1);
	}
	return 0;
}

__attribute__((naked, noreturn)) void _start(void)
{
	__asm__ volatile("ldr x0, [sp]\n" "add x1, sp, #8\n" "and sp, x1, #~15\n" "bl c_start\n");
}
__attribute__((used)) void c_start(long argc, char **argv) { sys_exit(main((int)argc, argv)); }
#endif
