// q2ctl_pnp.h - controller constellation pose math for q2ctltrack (desk-220).  Pure functions, no libc.
//   bearings: unit rays in a camera frame (x right, y down, z forward)
//   model:    the controller's LED positions + normals (Meta factory calibration, controller model frame)
//   pose:     R (3x3 row-major), t: x_cam = R * p_model + t
#pragma once

static inline double fsqrt(double x)
{
	if (x <= 0) return 0;
#ifdef __aarch64__
	double r; __asm__("fsqrt %d0, %d1" : "=w"(r) : "w"(x)); return r;
#else
	return __builtin_sqrt(x);
#endif
}
static inline double fabs_(double x) { return x < 0 ? -x : x; }
static double fsin(double x) { double x2 = x * x; return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110))))); }
static double fcos(double x) { double x2 = x * x; return 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56 * (1 - x2 / 90)))); }
static inline double dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline void cross3(const double a[3], const double b[3], double o[3]) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static inline double norm3(const double a[3]) { return fsqrt(dot3(a, a)); }
static inline void unit3(double a[3]) { double n = norm3(a); if (n > 1e-12) { a[0] /= n; a[1] /= n; a[2] /= n; } }
static inline void mv3(const double R[9], const double v[3], double o[3]) { for (int i = 0; i < 3; i++) o[i] = R[i * 3] * v[0] + R[i * 3 + 1] * v[1] + R[i * 3 + 2] * v[2]; }
static inline void mtv3(const double R[9], const double v[3], double o[3]) { for (int i = 0; i < 3; i++) o[i] = R[i] * v[0] + R[3 + i] * v[1] + R[6 + i] * v[2]; }
static void mm3(const double A[9], const double B[9], double O[9]) { double T[9]; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) T[i * 3 + j] = A[i * 3] * B[j] + A[i * 3 + 1] * B[3 + j] + A[i * 3 + 2] * B[6 + j]; for (int i = 0; i < 9; i++) O[i] = T[i]; }
static void mtm3(const double A[9], const double B[9], double O[9]) { double T[9]; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) T[i * 3 + j] = A[i] * B[j] + A[3 + i] * B[3 + j] + A[6 + i] * B[6 + j]; for (int i = 0; i < 9; i++) O[i] = T[i]; }
// quaternion (w x y z) <-> matrix
static void q2m(const double q[4], double R[9])
{
	double w = q[0], x = q[1], y = q[2], z = q[3];
	R[0] = 1 - 2 * (y * y + z * z); R[1] = 2 * (x * y - w * z); R[2] = 2 * (x * z + w * y);
	R[3] = 2 * (x * y + w * z); R[4] = 1 - 2 * (x * x + z * z); R[5] = 2 * (y * z - w * x);
	R[6] = 2 * (x * z - w * y); R[7] = 2 * (y * z + w * x); R[8] = 1 - 2 * (x * x + y * y);
}
static void m2q(const double R[9], double q[4])
{
	double tr = R[0] + R[4] + R[8];
	if (tr > 0) { double s = fsqrt(tr + 1) * 2; q[0] = s / 4; q[1] = (R[7] - R[5]) / s; q[2] = (R[2] - R[6]) / s; q[3] = (R[3] - R[1]) / s; }
	else if (R[0] > R[4] && R[0] > R[8]) { double s = fsqrt(1 + R[0] - R[4] - R[8]) * 2; q[0] = (R[7] - R[5]) / s; q[1] = s / 4; q[2] = (R[1] + R[3]) / s; q[3] = (R[2] + R[6]) / s; }
	else if (R[4] > R[8]) { double s = fsqrt(1 + R[4] - R[0] - R[8]) * 2; q[0] = (R[2] - R[6]) / s; q[1] = (R[1] + R[3]) / s; q[2] = s / 4; q[3] = (R[5] + R[7]) / s; }
	else { double s = fsqrt(1 + R[8] - R[0] - R[4]) * 2; q[0] = (R[3] - R[1]) / s; q[1] = (R[2] + R[6]) / s; q[2] = (R[5] + R[7]) / s; q[3] = s / 4; }
	double n = fsqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]); for (int i = 0; i < 4; i++) q[i] /= n;
	if (q[0] < 0) for (int i = 0; i < 4; i++) q[i] = -q[i];
}
// R <- exp(w) * R (small rotation vector w)
static void rot_update(double R[9], const double w[3])
{
	double q[4] = { 1, w[0] / 2, w[1] / 2, w[2] / 2 }, n = fsqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	for (int i = 0; i < 4; i++) q[i] /= n;
	double D[9]; q2m(q, D); mm3(D, R, R);
}
// re-orthonormalise (drift from many small updates)
static void rot_fix(double R[9]) { double q[4]; m2q(R, q); q2m(q, R); }

// ---------------------------------------------------------------- model
#define MAXLED 24
#define MAXTRI 400
struct cmodel { int n; double p[MAXLED][3], nrm[MAXLED][3]; int ntri; unsigned char tri[MAXTRI][3]; };
static void model_init(struct cmodel *m, const double (*led)[6], int n)
{
	m->n = n > MAXLED ? MAXLED : n; m->ntri = 0;
	for (int i = 0; i < m->n; i++) { for (int k = 0; k < 3; k++) { m->p[i][k] = led[i][k]; m->nrm[i][k] = led[i][3 + k]; } unit3(m->nrm[i]); }
	// seed triangles: LEDs that can be seen together (normals within ~100 deg of each other) and are near each other
	for (int i = 0; i < m->n; i++) for (int j = i + 1; j < m->n; j++) for (int k = j + 1; k < m->n; k++) {
		const int id[3] = { i, j, k }; int ok = 1;
		for (int a = 0; a < 3 && ok; a++) { int u = id[a], v = id[(a + 1) % 3];
			double d[3] = { m->p[u][0] - m->p[v][0], m->p[u][1] - m->p[v][1], m->p[u][2] - m->p[v][2] };
			if (dot3(d, d) > 0.060 * 0.060 || dot3(m->nrm[u], m->nrm[v]) < -0.15) ok = 0; }
		if (ok && m->ntri < MAXTRI) { m->tri[m->ntri][0] = i; m->tri[m->ntri][1] = j; m->tri[m->ntri][2] = k; m->ntri++; }
	}
}
// is LED i facing the camera for pose (R,t)?  cos limit ~ 0.2 (78 deg off-axis)
static inline int led_visible(const struct cmodel *m, int i, const double R[9], const double t[3], double xc[3])
{
	double n[3]; mv3(R, m->p[i], xc); xc[0] += t[0]; xc[1] += t[1]; xc[2] += t[2];
	mv3(R, m->nrm[i], n);
	return -dot3(n, xc) > 0.2 * norm3(xc);
}

// ---------------------------------------------------------------- P3P (Grunert, Haralick et al. 1994 formulation)
// real roots of a4 x^4 + ... + a0 (Durand-Kerner + Newton polish); returns count
static int quartic_roots(double a4, double a3, double a2, double a1, double a0, double out[4])
{
	if (fabs_(a4) < 1e-14) return 0;
	double c[4] = { a3 / a4, a2 / a4, a1 / a4, a0 / a4 };
	double zr[4], zi[4], br = 0.4, bi = 0.9;
	{ double r = 1, i = 0; for (int k = 0; k < 4; k++) { zr[k] = r; zi[k] = i; double nr = r * br - i * bi, ni = r * bi + i * br; r = nr; i = ni; } }
	double sc = 1 + fabs_(c[0]) + fabs_(c[1]) + fabs_(c[2]) + fabs_(c[3]); for (int k = 0; k < 4; k++) { zr[k] *= sc * 0.5; zi[k] *= sc * 0.5; }
	for (int it = 0; it < 200; it++) {
		double mx = 0;
		for (int k = 0; k < 4; k++) {
			// p(z) (monic)
			double pr = 1, pi = 0;
			for (int j = 0; j < 4; j++) { double nr = pr * zr[k] - pi * zi[k] + c[j], ni = pr * zi[k] + pi * zr[k]; pr = nr; pi = ni; }
			double dr = 1, di = 0;
			for (int j = 0; j < 4; j++) if (j != k) { double er = zr[k] - zr[j], ei = zi[k] - zi[j]; double nr = dr * er - di * ei, ni = dr * ei + di * er; dr = nr; di = ni; }
			double den = dr * dr + di * di; if (den < 1e-300) den = 1e-300;
			double qr = (pr * dr + pi * di) / den, qi = (pi * dr - pr * di) / den;
			zr[k] -= qr; zi[k] -= qi;
			double m = fabs_(qr) + fabs_(qi); if (m > mx) mx = m;
		}
		if (mx < 1e-13 * sc) break;
	}
	int n = 0;
	for (int k = 0; k < 4; k++) {
		if (fabs_(zi[k]) > 1e-4 * (1 + fabs_(zr[k]))) continue;
		double x = zr[k];
		for (int it = 0; it < 4; it++) {
			double p = (((x + c[0]) * x + c[1]) * x + c[2]) * x + c[3], d = ((4 * x + 3 * c[0]) * x + 2 * c[1]) * x + c[2];
			if (fabs_(d) < 1e-300) break; x -= p / d;
		}
		out[n++] = x;
	}
	return n;
}
// rotation + translation mapping 3 model points P onto 3 camera points Q (frames built from the triangles; exact if
// the triangles are congruent, which they are up to the P3P solution's accuracy)
static int tri_align(const double P[3][3], const double Q[3][3], double R[9], double t[3])
{
	double ex[2][3], ey[2][3], ez[2][3];
	for (int s = 0; s < 2; s++) {
		const double (*X)[3] = s ? Q : P;
		double a[3] = { X[1][0] - X[0][0], X[1][1] - X[0][1], X[1][2] - X[0][2] }, b[3] = { X[2][0] - X[0][0], X[2][1] - X[0][1], X[2][2] - X[0][2] };
		double z[3]; cross3(a, b, z); if (norm3(z) < 1e-12) return 0;
		unit3(a); unit3(z); double y[3]; cross3(z, a, y);
		for (int k = 0; k < 3; k++) { ex[s][k] = a[k]; ey[s][k] = y[k]; ez[s][k] = z[k]; }
	}
	// R = Fq * Fp^T, with F = [ex ey ez] as columns
	for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) R[i * 3 + j] = ex[1][i] * ex[0][j] + ey[1][i] * ey[0][j] + ez[1][i] * ez[0][j];
	double rp[3]; mv3(R, P[0], rp); for (int k = 0; k < 3; k++) t[k] = Q[0][k] - rp[k];
	return 1;
}
// f: 3 unit bearings, P: 3 model points.  Up to 4 poses.
static int p3p(const double f[3][3], const double P[3][3], double Rs[4][9], double ts[4][3])
{
	double d12[3], d13[3], d23[3];
	for (int k = 0; k < 3; k++) { d12[k] = P[0][k] - P[1][k]; d13[k] = P[0][k] - P[2][k]; d23[k] = P[1][k] - P[2][k]; }
	double a2 = dot3(d23, d23), b2 = dot3(d13, d13), c2 = dot3(d12, d12);
	if (a2 < 1e-8 || b2 < 1e-8 || c2 < 1e-8) return 0;
	double ca = dot3(f[1], f[2]), cb = dot3(f[0], f[2]), cg = dot3(f[0], f[1]);
	double amc = (a2 - c2) / b2, apc = (a2 + c2) / b2, bmc = (b2 - c2) / b2, bma = (b2 - a2) / b2;
	double A4 = (amc - 1) * (amc - 1) - 4 * c2 / b2 * ca * ca;
	double A3 = 4 * (amc * (1 - amc) * cb - (1 - apc) * ca * cg + 2 * c2 / b2 * ca * ca * cb);
	double A2 = 2 * (amc * amc - 1 + 2 * amc * amc * cb * cb + 2 * bmc * ca * ca - 4 * apc * ca * cb * cg + 2 * bma * cg * cg);
	double A1 = 4 * (-amc * (1 + amc) * cb + 2 * a2 / b2 * cg * cg * cb - (1 - apc) * ca * cg);
	double A0 = (1 + amc) * (1 + amc) - 4 * a2 / b2 * cg * cg;
	double vs[4]; int nv = quartic_roots(A4, A3, A2, A1, A0, vs), n = 0;
	for (int r = 0; r < nv; r++) {
		double v = vs[r]; if (v <= 0) continue;
		double den = 2 * (cg - v * ca); if (fabs_(den) < 1e-12) continue;
		double u = ((-1 + amc) * v * v - 2 * amc * cb * v + 1 + amc) / den; if (u <= 0) continue;
		double q = 1 + v * v - 2 * v * cb; if (q <= 1e-12) continue;
		double s1 = fsqrt(b2 / q), s2 = u * s1, s3 = v * s1;
		double Q[3][3]; for (int k = 0; k < 3; k++) { Q[0][k] = f[0][k] * s1; Q[1][k] = f[1][k] * s2; Q[2][k] = f[2][k] * s3; }
		// reject solutions whose side lengths do not match (numerical junk)
		double e12[3], e13[3], e23[3]; for (int k = 0; k < 3; k++) { e12[k] = Q[0][k] - Q[1][k]; e13[k] = Q[0][k] - Q[2][k]; e23[k] = Q[1][k] - Q[2][k]; }
		if (fabs_(dot3(e12, e12) - c2) > 0.05 * c2 || fabs_(dot3(e13, e13) - b2) > 0.05 * b2 || fabs_(dot3(e23, e23) - a2) > 0.05 * a2) continue;
		if (tri_align(P, Q, Rs[n], ts[n])) n++;
	}
	return n;
}

// ---------------------------------------------------------------- matching + refinement
struct obs { int cam, led; double f[3]; };            // one LED seen as one blob: bearing f in camera cam's frame
// per-camera extrinsics for joint refinement: x_cam = Rc * x_head + tc  (inverse of DeviceFromCamera)
struct camx { double Rc[9], tc[3]; };
// pose in "head" frame (R, t): x_head = R p + t.  Residual of one observation: tangent-plane error of the predicted ray
static inline void obs_res(const struct cmodel *m, const struct camx *cx, const struct obs *o, const double R[9], const double t[3], double r[2])
{
	double xh[3], xc[3]; mv3(R, m->p[o->led], xh); for (int k = 0; k < 3; k++) xh[k] += t[k];
	mv3(cx[o->cam].Rc, xh, xc); for (int k = 0; k < 3; k++) xc[k] += cx[o->cam].tc[k];
	// basis orthogonal to f
	const double *f = o->f; double e1[3], e2[3], h[3] = { 1, 0, 0 };
	if (fabs_(f[0]) > 0.8) { h[0] = 0; h[1] = 1; }
	cross3(f, h, e1); unit3(e1); cross3(f, e1, e2);
	double z = dot3(xc, f); if (z < 1e-3) z = 1e-3;
	r[0] = dot3(xc, e1) / z; r[1] = dot3(xc, e2) / z;
}
// Gauss-Newton over (rotation, translation) with numeric Jacobian; returns RMS residual of the observations (radians).
// prior (optional, desk-221): { up_head[3], up_model_from_imu[3], weight } adds the residual weight * (R^T up_head -
// up_model_from_imu): the controller IMU's tilt, which pins the orientation (and with it the distance) when few LEDs show
static void prior_res(const double *pr, const double R[9], double r[3])
{
	double um[3]; mtv3(R, pr, um); for (int k = 0; k < 3; k++) r[k] = pr[6] * (um[k] - pr[3 + k]);
}
static double refine_p(const struct cmodel *m, const struct camx *cx, const struct obs *o, int n, double R[9], double t[3], int iters, const double *pr)
{
	if (n < 3) return 1;
	for (int it = 0; it < iters; it++) {
		double JtJ[36] = { 0 }, Jtr[6] = { 0 };
		for (int i = 0; i <= n; i++) {
			if (i == n && !pr) break;
			int nr = i < n ? 2 : 3;
			double r0[3]; if (i < n) obs_res(m, cx, &o[i], R, t, r0); else prior_res(pr, R, r0);
			double J[3][6];
			for (int p = 0; p < 6; p++) {
				double R2[9], t2[3]; for (int k = 0; k < 9; k++) R2[k] = R[k]; for (int k = 0; k < 3; k++) t2[k] = t[k];
				const double hstep = p < 3 ? 1e-5 : 1e-6;
				if (p < 3) { double w[3] = { 0, 0, 0 }; w[p] = hstep; rot_update(R2, w); }
				else t2[p - 3] += hstep;
				double r1[3]; if (i < n) obs_res(m, cx, &o[i], R2, t2, r1); else prior_res(pr, R2, r1);
				for (int a = 0; a < nr; a++) J[a][p] = (r1[a] - r0[a]) / hstep;
			}
			for (int a = 0; a < nr; a++) for (int p = 0; p < 6; p++) { Jtr[p] += J[a][p] * r0[a]; for (int q = 0; q < 6; q++) JtJ[p * 6 + q] += J[a][p] * J[a][q]; }
		}
		for (int p = 0; p < 6; p++) { JtJ[p * 7] *= 1.001; JtJ[p * 7] += 1e-9; }
		// solve JtJ dx = -Jtr (Gaussian elimination with partial pivoting)
		double A[6][7]; for (int p = 0; p < 6; p++) { for (int q = 0; q < 6; q++) A[p][q] = JtJ[p * 6 + q]; A[p][6] = -Jtr[p]; }
		int bad = 0;
		for (int c = 0; c < 6 && !bad; c++) {
			int pv = c; for (int r = c + 1; r < 6; r++) if (fabs_(A[r][c]) > fabs_(A[pv][c])) pv = r;
			if (fabs_(A[pv][c]) < 1e-18) { bad = 1; break; }
			if (pv != c) for (int k = 0; k < 7; k++) { double tmp = A[c][k]; A[c][k] = A[pv][k]; A[pv][k] = tmp; }
			for (int r = 0; r < 6; r++) if (r != c) { double fct = A[r][c] / A[c][c]; for (int k = c; k < 7; k++) A[r][k] -= fct * A[c][k]; }
		}
		if (bad) break;
		double dx[6]; for (int p = 0; p < 6; p++) dx[p] = A[p][6] / A[p][p];
		double step = 0; for (int p = 0; p < 6; p++) step += dx[p] * dx[p];
		if (step > 0.25) { double s = 0.5 / fsqrt(step); for (int p = 0; p < 6; p++) dx[p] *= s; }   // clamp wild steps
		rot_update(R, dx); for (int k = 0; k < 3; k++) t[k] += dx[3 + k];
		if (step < 1e-14) break;
	}
	rot_fix(R);
	double ss = 0; for (int i = 0; i < n; i++) { double r[2]; obs_res(m, cx, &o[i], R, t, r); ss += r[0] * r[0] + r[1] * r[1]; }
	return fsqrt(ss / n);
}
static double refine(const struct cmodel *m, const struct camx *cx, const struct obs *o, int n, double R[9], double t[3], int iters) { return refine_p(m, cx, o, n, R, t, iters, 0); }
// project the model with pose (head frame) into camera c and pair visible LEDs with bearings (greedy, closest first)
// within angle thr (rad).  bear[]: unit bearings of the camera's blobs; taken[]: blobs already used (skipped).
static int match_cam(const struct cmodel *m, const struct camx *cx, int c, const double R[9], const double t[3],
                     const double (*bear)[3], int nb, const unsigned char *taken, double thr, struct obs *out, int maxo, double *esum)
{
	double Rc[9], tc[3]; mm3(cx[c].Rc, R, Rc); mv3(cx[c].Rc, t, tc); for (int k = 0; k < 3; k++) tc[k] += cx[c].tc[k];
	struct { int led, b; double d; } cand[MAXLED * 4]; int nc = 0;
	double ct = fcos(thr);
	for (int i = 0; i < m->n; i++) {
		double xc[3]; if (!led_visible(m, i, Rc, tc, xc)) continue;
		if (xc[2] < 0.05) continue;
		double nn = norm3(xc); double d[3] = { xc[0] / nn, xc[1] / nn, xc[2] / nn };
		if (d[2] < 0.05) continue;                                 // behind the lens / outside the ~170 deg field
		// best two blobs for this LED
		int b1 = -1, b2 = -1; double c1 = ct, c2 = ct;
		for (int b = 0; b < nb; b++) { if (taken && taken[b]) continue; double cs = dot3(d, bear[b]);
			if (cs > c1) { c2 = c1; b2 = b1; c1 = cs; b1 = b; } else if (cs > c2) { c2 = cs; b2 = b; } }
		if (b1 >= 0 && nc < MAXLED * 4) { cand[nc].led = i; cand[nc].b = b1; cand[nc].d = 1 - c1; nc++; }
		if (b2 >= 0 && nc < MAXLED * 4) { cand[nc].led = i; cand[nc].b = b2; cand[nc].d = 1 - c2; nc++; }
	}
	// sort by distance (insertion), then greedy unique assignment
	for (int i = 1; i < nc; i++) { __typeof__(cand[0]) x = cand[i]; int j = i - 1; while (j >= 0 && cand[j].d > x.d) { cand[j + 1] = cand[j]; j--; } cand[j + 1] = x; }
	unsigned char ul[MAXLED] = { 0 }; static unsigned char ub[256]; for (int b = 0; b < nb && b < 256; b++) ub[b] = 0;
	int n = 0; double es = 0;
	for (int i = 0; i < nc && n < maxo; i++) {
		if (ul[cand[i].led] || cand[i].b >= 256 || ub[cand[i].b]) continue;
		ul[cand[i].led] = 1; ub[cand[i].b] = 1;
		out[n].cam = c; out[n].led = cand[i].led; for (int k = 0; k < 3; k++) out[n].f[k] = bear[cand[i].b][k];
		es += cand[i].d; n++;
	}
	if (esum) *esum = es;
	return n;
}
// which blob each observation came from (by identical bearing), so they can be marked as used
static int blob_of(const double (*bear)[3], int nb, const double f[3]) { for (int b = 0; b < nb; b++) if (bear[b][0] == f[0] && bear[b][1] == f[1] && bear[b][2] == f[2]) return b; return -1; }

// ---------------------------------------------------------------- acquisition (one camera, one cluster of blobs)
// tries seed blob triples against every co-visible LED triangle of the model (all 6 orderings), P3P, then counts how
// many of the cluster's blobs the pose explains.  Result: best pose in the CAMERA frame and its inlier count.
#define NACQ 4
struct acq { int inl; double err, R[9], t[3]; };
// keep the NACQ best distinct candidates (most inliers, then smallest error)
static void acq_add(struct acq *best, const double R[9], const double t[3], int inl, double err)
{
	int worst = 0;
	for (int i = 0; i < NACQ; i++) {
		if (best[i].inl) {
			double d[3] = { best[i].t[0] - t[0], best[i].t[1] - t[1], best[i].t[2] - t[2] };
			double tr = 0; for (int k = 0; k < 9; k++) tr += best[i].R[k] * R[k];   // trace(Ri^T R) = 1 + 2 cos(angle)
			if (dot3(d, d) < 0.015 * 0.015 && tr > 1 + 2 * 0.97) {                // same pose: keep the better one
				if (inl > best[i].inl || (inl == best[i].inl && err < best[i].err)) { best[i].inl = inl; best[i].err = err; for (int k = 0; k < 9; k++) best[i].R[k] = R[k]; for (int k = 0; k < 3; k++) best[i].t[k] = t[k]; }
				return;
			}
		}
		if (best[i].inl < best[worst].inl || (best[i].inl == best[worst].inl && best[i].err > best[worst].err)) worst = i;
	}
	struct acq *w = &best[worst];
	if (inl > w->inl || (inl == w->inl && err < w->err)) { w->inl = inl; w->err = err; for (int k = 0; k < 9; k++) w->R[k] = R[k]; for (int k = 0; k < 3; k++) w->t[k] = t[k]; }
}
static void acquire(const struct cmodel *m, const double (*bear)[3], const int *idx, int nidx, struct acq *best, double thr)
{
	for (int i = 0; i < NACQ; i++) { best[i].inl = 0; best[i].err = 1e9; }
	if (nidx < 4) return;
	double ct = fcos(thr);
	// seeds: up to 5 blobs, each with its two nearest neighbours in the cluster (angularly)
	int nseed = nidx < 4 ? nidx : 4;
	for (int s = 0; s < nseed; s++) {
		int a = idx[s], n1 = -1, n2 = -1; double c1 = -2, c2 = -2;
		for (int j = 0; j < nidx; j++) { int b = idx[j]; if (b == a) continue; double cs = dot3(bear[a], bear[b]);
			if (cs > c1) { c2 = c1; n2 = n1; c1 = cs; n1 = b; } else if (cs > c2) { c2 = cs; n2 = b; } }
		if (n1 < 0 || n2 < 0) continue;
		double f[3][3]; for (int k = 0; k < 3; k++) { f[0][k] = bear[a][k]; f[1][k] = bear[n1][k]; f[2][k] = bear[n2][k]; }
		static const int perm[6][3] = { { 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 }, { 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 } };
		for (int tr = 0; tr < m->ntri; tr++) for (int pm = 0; pm < 6; pm++) {
			double P[3][3]; int li[3];
			for (int q = 0; q < 3; q++) { li[q] = m->tri[tr][perm[pm][q]]; for (int k = 0; k < 3; k++) P[q][k] = m->p[li[q]][k]; }
			double Rs[4][9], ts[4][3]; int ns = p3p(f, P, Rs, ts);
			for (int sI = 0; sI < ns; sI++) {
				double *R = Rs[sI], *t = ts[sI];
				double dist = norm3(t); if (dist < 0.08 || dist > 2.0) continue;
				int vis = 1; for (int q = 0; q < 3 && vis; q++) { double xc[3]; if (!led_visible(m, li[q], R, t, xc)) vis = 0; }
				if (!vis) continue;
				// score: each visible LED -> nearest blob of the cluster within thr
				int inl = 0; double err = 0;
				for (int i = 0; i < m->n; i++) {
					double xc[3]; if (!led_visible(m, i, R, t, xc)) continue;
					double nn = norm3(xc); double d[3] = { xc[0] / nn, xc[1] / nn, xc[2] / nn }, bc = ct;
					for (int j = 0; j < nidx; j++) { double cs = dot3(d, bear[idx[j]]); if (cs > bc) bc = cs; }
					if (bc > ct) { inl++; err += 1 - bc; }
				}
				if (inl >= 4) acq_add(best, R, t, inl, err);
			}
		}
	}
}
