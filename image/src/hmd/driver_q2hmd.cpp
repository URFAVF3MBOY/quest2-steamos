// driver_q2hmd - minimal SteamVR HMD driver for the Quest 2 running SteamOS (first-light test).
// One HMD, fixed head pose, display = the Quest 2 panel (two 1832x1920 eyes), identity distortion for now.
// Built freestanding with clang (no libstdc++); libc symbols resolve from vrserver at load time.
#include <stddef.h>
#include <stdint.h>
#include "openvr_driver.h"

extern "C" {
int strcmp(const char *, const char *);
void *malloc(size_t);
void free(void *);
int snprintf(char *, size_t, const char *, ...);
char *getenv(const char *);
int open(const char *, int, ...);
int close(int);
long read(int, void *, unsigned long);
void *mmap(void *, size_t, int, int, int, long);
struct q2ts { long tv_sec, tv_nsec; };
int nanosleep(const q2ts *, q2ts *);
int clock_gettime(int, q2ts *);
typedef unsigned long q2thread;
int pthread_create(q2thread *, const void *, void *(*)(void *), void *);
}
void *operator new(size_t n) { return malloc(n); }
void operator delete(void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
extern "C" void __cxa_pure_virtual() {}

using namespace vr;

static IVRDriverLog *g_log;
static IVRServerDriverHost *g_host;
static IVRProperties *g_props;
static IVRDriverInput *g_input;

static void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern "C" int vsnprintf(char *, size_t, const char *, __builtin_va_list);
static void Log(const char *fmt, ...)
{
	char b[512];
	__builtin_va_list ap; __builtin_va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); __builtin_va_end(ap);
	if (g_log) g_log->Log(b);
}

static int envi(const char *n, int d) { const char *v = getenv(n); if (!v || !*v) return d; int r = 0; while (*v >= '0' && *v <= '9') r = r * 10 + (*v++ - '0'); return r; }

// The display is the panel as scanned out: 1920 x 3664 portrait, eyes stacked (left eye = rows 0..1831).
// The 270-degree rotation and the lens pre-warp are both done by vrcompositor's distortion mesh (ComputeDistortion),
// with the same lens model as the desktop warp (q2blit / q2gpuwarp): Quest lens curve, edge tangent TMAX.
static uint32_t DW = 1920, DH = 3664;
static const uint32_t EW = 1832, EH = 1920;           // one eye in "eye canvas" coords (x across, y down), as q2blit
static float TMAX = 2.4f, TPROJ = 1.3f, DCTR = 0.0f;  // edge tangent of the lens model, render half-FOV tangent, lens decenter px
static const float LA[11] = {1.0f,1.0374f,1.0810f,1.1330f,1.1970f,1.2754f,1.3771f,1.5133f,1.7018f,1.9732f,2.3f};
static float crom(float t)
{
	if (t < 0) t = 0; if (t > 1) t = 1;
	float f = t * 10.0f; int i = (int)f; if (i > 9) i = 9; float u = f - (float)i;
	float p1 = LA[i], p2 = LA[i + 1];
	float p0 = i > 0 ? LA[i - 1] : 2 * p1 - p2, p3 = i < 9 ? LA[i + 2] : 2 * p2 - p1;
	return 0.5f * ((2 * p1) + (-p0 + p2) * u + (2 * p0 - 5 * p1 + 4 * p2 - p3) * u * u + (-p0 + 3 * p1 - 3 * p2 + p3) * u * u * u);
}
// ---- Meta's Quest 2 lens model, read from the headset's own VR runtime (libvrapiimpl.so, Quest 2 branch of the HMD
// config function @0x1e3f00; method and Quest 1 numbers: nicolas-riera/quest1-steamos docs/quest1-lens.md, credit):
//   Oculus SDK LensConfig CatmullRom10, K = LA[] above (same table), MaxR 1, MetersPerTanAngleAtCenter 0.0389 m,
//   eye panel 1832 x 1920 px = 0.06018 x 0.06307 m (32.85 um pixels), lens 1.25 mm above the eye-panel centre and,
//   per IPD slider position, 2.6 / 1.1 / -2.0 mm toward the nose (fitted to the runtime's per-IPD FOV table:
//   inner/outer 42/52, 45/49, 45/43 deg; up 48, down 50 deg).  Chromatic: red 0.996-0.007 r2, blue 1.006+0.020 r2.
static int IPDPOS = 2;
static float T_IN = 1.0f, T_OUT = 1.1504f, T_UP = 1.1106f, T_DN = 1.1918f, LENS_IN = 0.0011f;
static const float LENS_UP = 0.00125f, PITCH = 0.06018085f / 1832.0f, MPT = 0.0389f;
static void set_ipdpos(int p)
{
	IPDPOS = p < 1 ? 1 : p > 3 ? 3 : p;
	static const float tin[3] = { 0.9004f, 1.0f, 1.0f }, tout[3] = { 1.2799f, 1.1504f, 0.9325f }, lin[3] = { 0.0026f, 0.0011f, -0.0020f };
	T_IN = tin[IPDPOS - 1]; T_OUT = tout[IPDPOS - 1]; LENS_IN = lin[IPDPOS - 1];
}
static float cr10(float rsq)
{
	const float *K = LA;
	float f = 10.0f * rsq; int k = (int)f; if (k < 0) k = 0; if (k > 10) k = 10; float t = f - (float)k;
	float p0, m0, p1, m1;
	if (k == 0) { p0 = 1; m0 = K[1] - K[0]; p1 = K[1]; m1 = 0.5f * (K[2] - K[0]); }
	else if (k < 9) { p0 = K[k]; m0 = 0.5f * (K[k + 1] - K[k - 1]); p1 = K[k + 1]; m1 = 0.5f * (K[k + 2] - K[k]); }
	else if (k == 9) { p0 = K[9]; m0 = 0.5f * (K[10] - K[8]); p1 = K[10]; m1 = K[10] - K[9]; }
	else { p0 = K[10]; m0 = K[10] - K[9]; p1 = p0 + m0; m1 = m0; }
	float omt = 1 - t;
	return (p0 * (1 + 2 * t) + m0 * t) * omt * omt + (p1 * (1 + 2 * omt) - m1 * omt) * t * t;
}
// panel position (eye canvas px: x right, y down in the view) -> render UV for each colour (c: 0 red, 1 green, 2 blue)
static void meta_distort(int eye, float cx, float cy, float uv[3][2])
{
	float px = (cx + 0.5f - (float)EW * 0.5f) * PITCH, py = ((float)EH * 0.5f - (cy + 0.5f)) * PITCH;   // metres, y up
	float lx = eye == 0 ? LENS_IN : -LENS_IN;                    // left eye: the nose is to the right (+x)
	float x = (px - lx) / MPT, y = (py - LENS_UP) / MPT, rsq = x * x + y * y, s = cr10(rsq);
	float cs[3] = { 0.996f - 0.007f * rsq, 1.0f, 1.006f + 0.020f * rsq };
	float l = eye == 0 ? -T_OUT : -T_IN, r = eye == 0 ? T_IN : T_OUT;
	for (int c = 0; c < 3; c++) {
		float tx = x * s * cs[c], ty = y * s * cs[c];            // tangents, x right, y up
		uv[c][0] = (tx - l) / (r - l);
		uv[c][1] = (T_UP - ty) / (T_UP + T_DN);               // v = 0 at the top of the eye render
	}
}
static int OLDLENS = 0;

// viewport (u across the panel = 0..1920, v down the eye's 1832 panel rows) -> render-target UV of that eye
static void distort(int eye, float u, float v, float *ou, float *ov)
{
	float cx = (float)(EW - 1) - v * (float)EW, cy = u * (float)EH;        // undo the 270-degree rotation (as q2blit)
	float cxs = (float)EW * 0.5f + (eye == 0 ? DCTR : -DCTR), cys = (float)EH * 0.5f, rmax = 0.5f * (float)EH;
	float dx = (cx - cxs) / rmax, dy = (cy - cys) / rmax;
	float rs = __builtin_sqrtf(dx * dx + dy * dy);
	float q = rs;
	for (int k = 0; k < 12; k++) q = rs / crom(q);
	float sc = rs > 1e-6f ? q / rs : 1.0f;
	float tx = dx * sc * TMAX, ty = dy * sc * TMAX;                         // view tangents (x right, y down)
	*ou = (tx + TPROJ) / (2 * TPROJ); *ov = (ty + TPROJ) / (2 * TPROJ);
}

static void setStr(PropertyContainerHandle_t c, ETrackedDeviceProperty p, const char *s)
{
	PropertyWrite_t w = {}; w.prop = p; w.writeType = PropertyWrite_Set; w.pvBuffer = (void *)s;
	uint32_t n = 0; while (s[n]) n++; w.unBufferSize = n + 1; w.unTag = k_unStringPropertyTag;
	g_props->WritePropertyBatch(c, &w, 1);
}
template <class T> static void setVal(PropertyContainerHandle_t c, ETrackedDeviceProperty p, T v, PropertyTypeTag_t tag)
{
	PropertyWrite_t w = {}; w.prop = p; w.writeType = PropertyWrite_Set; w.pvBuffer = &v; w.unBufferSize = sizeof v; w.unTag = tag;
	g_props->WritePropertyBatch(c, &w, 1);
}

class Q2Display : public IVRDisplayComponent {
public:
	void GetWindowBounds(int32_t *x, int32_t *y, uint32_t *w, uint32_t *h) override { *x = 0; *y = 0; *w = DW; *h = DH; }
	bool IsDisplayOnDesktop() override { return envi("Q2VR_ONDESKTOP", 0) != 0; }
	bool IsDisplayRealDisplay() override { return true; }
	void GetRecommendedRenderTargetSize(uint32_t *w, uint32_t *h) override
	{
		*w = (uint32_t)envi("Q2VR_RT", 1440);
		*h = OLDLENS ? *w : (uint32_t)((float)*w * (T_UP + T_DN) / (T_IN + T_OUT) + 0.5f) & ~1u;
	}
	void GetEyeOutputViewport(EVREye e, uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h) override
	{   // Q2VR_EYESWAP=1: left eye on the lower panel half (which half each eye looks at is not verified yet)
		static int sw = envi("Q2VR_EYESWAP", 0);
		*x = 0; *y = ((e == Eye_Left) != (sw != 0)) ? 0 : DH / 2; *w = DW; *h = DH / 2; }
	void GetProjectionRaw(EVREye e, float *l, float *r, float *t, float *b) override
	{
		if (OLDLENS) { *l = -TPROJ; *r = TPROJ; *t = -TPROJ; *b = TPROJ; return; }
		*l = e == Eye_Left ? -T_OUT : -T_IN; *r = e == Eye_Left ? T_IN : T_OUT; *t = -T_UP; *b = T_DN;
	}
	DistortionCoordinates_t ComputeDistortion(EVREye e, float u, float v) override
	{
		DistortionCoordinates_t d;
		if (OLDLENS) {
			float ou, ov; distort(e == Eye_Left ? 0 : 1, u, v, &ou, &ov);
			d.rfRed[0] = d.rfGreen[0] = d.rfBlue[0] = ou; d.rfRed[1] = d.rfGreen[1] = d.rfBlue[1] = ov; return d;
		}
		float cx = (float)(EW - 1) - v * (float)EW, cy = u * (float)EH;    // undo the 270-degree rotation (as q2blit)
		float uv[3][2]; meta_distort(e == Eye_Left ? 0 : 1, cx, cy, uv);
		d.rfRed[0] = uv[0][0]; d.rfRed[1] = uv[0][1]; d.rfGreen[0] = uv[1][0]; d.rfGreen[1] = uv[1][1];
		d.rfBlue[0] = uv[2][0]; d.rfBlue[1] = uv[2][1];
		return d;
	}
	bool ComputeInverseDistortion(HmdVector2_t *res, EVREye e, uint32_t ch, float u, float v) override
	{ (void)e; (void)ch; (void)res; (void)u; (void)v; return false; }
};

// ---- head rotation from q2headtrack: /dev/shm/q2pose = seq u32 (seqlock), then at +8: qw qx qy qz t wx wy wz (doubles)
static volatile uint32_t *g_pose;
static double g_meta_pos[3], g_meta_vel[3], g_meta_age; static bool g_meta_ok;   // desk-247: position from Meta's tracker (fresh this sample)
// desk-251: Meta's controller poses (q2metapose), copied inside the same seqlock: per hand at +256 / +384:
// u32 'MCTL', u32 counter, f64 time, qw qx qy qz, px py pz, vx vy vz, wx wy wz (world)
static double g_mctl[2][14]; static bool g_mctl_ok[2];
static bool read_pose(double o[8])
{
	if (!g_pose) {
		static int tries;
		if (tries++ % 500) return false;               // retry the open about once a second
		int fd = open("/dev/shm/q2pose", 0 /*O_RDONLY*/);
		if (fd < 0) return false;
		void *m = mmap(0, 4096, 1 /*PROT_READ*/, 1 /*MAP_SHARED*/, fd, 0);
		close(fd);
		if (m == (void *)-1) return false;
		g_pose = (volatile uint32_t *)m;
		Log("q2hmd: head tracking pose found (/dev/shm/q2pose)\n");
	}
	// seqlock: the writer (q2headtrack, Python) can sit between its two counter updates for several microseconds, so
	// retry for a while and otherwise reuse the last good pose (desk-115 and earlier returned "no pose" = looking
	// straight ahead for that sample: a flash of a wrongly oriented frame now and then)
	// desk-247: q2metapose (Meta's own tracker) also writes position: +72 px py pz, +96 'META', +104 update time
	static double last[8]; static bool have;
	for (int k = 0; k < 20000; k++) {
		uint32_t s0 = __atomic_load_n(g_pose, __ATOMIC_ACQUIRE);
		if (s0 & 1) continue;
		volatile double *d = (volatile double *)((volatile char *)g_pose + 8);
		double t[8], mp[3], mv[3], mt; uint32_t mg;
		for (int i = 0; i < 8; i++) t[i] = d[i];
		for (int i = 0; i < 3; i++) { mp[i] = d[8 + i]; mv[i] = d[13 + i]; }   // +72 position, +112 velocity
		mg = *(volatile uint32_t *)((volatile char *)g_pose + 96); mt = *(volatile double *)((volatile char *)g_pose + 104);
		uint32_t cm[2]; double cv[2][14];
		for (int h = 0; h < 2; h++) {
			volatile char *b = (volatile char *)g_pose + 256 + 128 * h;
			cm[h] = *(volatile uint32_t *)b;
			for (int i = 0; i < 14; i++) cv[h][i] = ((volatile double *)(b + 8))[i];
		}
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(g_pose, __ATOMIC_ACQUIRE) == s0) {
			if (!s0) break;
			for (int i = 0; i < 8; i++) o[i] = last[i] = t[i];
			g_meta_ok = false; g_mctl_ok[0] = g_mctl_ok[1] = false;
			if (mg == 0x4154454d) {
				q2ts n; clock_gettime(1 /*CLOCK_MONOTONIC*/, &n);
				double age = (n.tv_sec + n.tv_nsec * 1e-9) - mt;
				if (age > -0.1 && age < 0.3) {
					for (int i = 0; i < 3; i++) { g_meta_pos[i] = mp[i]; g_meta_vel[i] = mv[i]; } g_meta_age = age; g_meta_ok = true;
					for (int h = 0; h < 2; h++) if (cm[h] == 0x4c54434d) { for (int i = 0; i < 14; i++) g_mctl[h][i] = cv[h][i]; g_mctl_ok[h] = true; }
				}
			}
			have = true; return true;
		}
	}
	if (have) { for (int i = 0; i < 8; i++) o[i] = last[i]; return true; }
	return false;
}

// ---- mouse/keyboard pointer from q2input: /dev/shm/q2ptr = seq u32, then at +8: yaw pitch (rad), buttons, stick x, stick y
static volatile uint32_t *g_ptr;
static bool read_ptr(double o[5])
{
	if (!g_ptr) {
		static int tries;
		if (tries++ % 500) return false;
		int fd = open("/dev/shm/q2ptr", 0);
		if (fd < 0) return false;
		void *m = mmap(0, 4096, 1, 1, fd, 0);
		close(fd);
		if (m == (void *)-1) return false;
		g_ptr = (volatile uint32_t *)m;
		Log("q2hmd: mouse/keyboard pointer found (/dev/shm/q2ptr)\n");
	}
	for (int k = 0; k < 20000; k++) {
		uint32_t s0 = __atomic_load_n(g_ptr, __ATOMIC_ACQUIRE);
		if (s0 & 1) continue;
		volatile double *d = (volatile double *)((volatile char *)g_ptr + 8);
		for (int i = 0; i < 5; i++) o[i] = d[i];
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(g_ptr, __ATOMIC_ACQUIRE) == s0) return s0 != 0;
	}
	return false;
}
static void qmul(const double a[4], const double b[4], double o[4])
{
	o[0] = a[0]*b[0] - a[1]*b[1] - a[2]*b[2] - a[3]*b[3]; o[1] = a[0]*b[1] + a[1]*b[0] + a[2]*b[3] - a[3]*b[2];
	o[2] = a[0]*b[2] - a[1]*b[3] + a[2]*b[0] + a[3]*b[1]; o[3] = a[0]*b[3] + a[1]*b[2] - a[2]*b[1] + a[3]*b[0];
}
static void qrot(const double q[4], const double v[3], double o[3])
{
	double x = q[1], y = q[2], z = q[3], w = q[0];
	double tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
	o[0] = v[0] + w * tx + (y * tz - z * ty); o[1] = v[1] + w * ty + (z * tx - x * tz); o[2] = v[2] + w * tz + (x * ty - y * tx);
}
static double q_sin(double x) { double x2 = x * x; return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72)))); }
static double q_cos(double x) { double x2 = x * x; return 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56))); }

// ---- Touch controllers (desk-114) --------------------------------------------------------------------------------------
// q2headtrack decodes the controllers from SyncBoss and publishes /dev/shm/q2ctl: seq u32 (seqlock), then per hand
// (left at +8, right at +136) 16 doubles: counter, aim quaternion w x y z (world), angular velocity xyz (world, rad/s),
// trigger, grip, stick x, stick y, buttons (bit0 A/X, bit1 B/Y, bit2 stick, bit3 menu/Oculus), touch bits, battery %.
// Each hand is one SteamVR controller of the type Steam's dashboard has bindings for (q2vrtest picks it and lists its
// input components in Q2VR_CTLCOMP as "path=source;..."), drawn with the Quest render models.  The right hand falls
// back to the mouse/keyboard pointer (q2input, /dev/shm/q2ptr) while the real right controller sends nothing.
// Pose (as in nicolas-riera/quest1-steamos, controllers.cpp, used with the author's permission): position from an arm
// model (elbow 17 cm beside, 45 cm below, 5 cm in front of the head, turning with its heading; 30 cm forearm along the
// aim); SteamVR's device frame is the Oculus "raw" frame, raw = aim * rotX(60) * inverse(openxr_grip of the model).
static volatile uint32_t *g_ctl;
static bool read_ctl(int slot, double o[16])
{
	if (!g_ctl) {
		static int tries;
		if (tries++ % 500) return false;
		int fd = open("/dev/shm/q2ctl", 0);
		if (fd < 0) return false;
		void *m = mmap(0, 4096, 1, 1, fd, 0);
		close(fd);
		if (m == (void *)-1) return false;
		g_ctl = (volatile uint32_t *)m;
		Log("q2hmd: controller data found (/dev/shm/q2ctl)\n");
	}
	static double last[2][16]; static bool have[2];
	for (int k = 0; k < 20000; k++) {
		uint32_t s0 = __atomic_load_n(g_ctl, __ATOMIC_ACQUIRE);
		if (s0 & 1) continue;
		volatile double *d = (volatile double *)((volatile char *)g_ctl + 8 + slot * 128);
		double t[16];
		for (int i = 0; i < 16; i++) t[i] = d[i];
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(g_ctl, __ATOMIC_ACQUIRE) == s0) {
			if (t[0] <= 0) break;
			for (int i = 0; i < 16; i++) o[i] = last[slot][i] = t[i];
			have[slot] = true; return true;
		}
	}
	if (have[slot]) { for (int i = 0; i < 16; i++) o[i] = last[slot][i]; return true; }   // same data: counts as stale
	return false;
}
// desk-216: controller POSITION from the tracking cameras (q2ctltrack): /dev/shm/q2ctlpos = seq u32 (seqlock),
// u32 version, then per hand (left +8, right +72) f64 count, x, y, z (metres, headset frame: x right, y up, z back),
// dots, cameras.  Used while it keeps updating; otherwise the arm model below takes over again.
static volatile uint32_t *g_cpos;
static bool read_ctlpos(int slot, double p[3], double *count)
{
	if (!g_cpos) {
		static int tries;
		if (tries++ % 500) return false;
		int fd = open("/dev/shm/q2ctlpos", 0);
		if (fd < 0) return false;
		void *m = mmap(0, 4096, 1, 1, fd, 0);
		close(fd);
		if (m == (void *)-1) return false;
		g_cpos = (volatile uint32_t *)m;
		Log("q2hmd: camera controller positions found (/dev/shm/q2ctlpos)\n");
	}
	for (int k = 0; k < 20000; k++) {
		uint32_t s0 = __atomic_load_n(g_cpos, __ATOMIC_ACQUIRE);
		if (s0 & 1) continue;
		volatile double *d = (volatile double *)((volatile char *)g_cpos + 8 + slot * 64);
		double c = d[0], x = d[1], y = d[2], z = d[3];
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(g_cpos, __ATOMIC_ACQUIRE) == s0) { *count = c; p[0] = x; p[1] = y; p[2] = z; return c > 0; }
	}
	return false;
}
// desk-220 (q2ctltrack v2): per hand at +256 + 64*h: count, qw qx qy qz = rotation LED-model frame -> headset frame;
// at +512 + 128*h: IMU axis index (-1 = not learned yet), A[9] (v_model = A v_imu)
static bool read_ctlrot(int slot, double q[4], double A[9], int *axis)
{
	if (!g_cpos) return false;
	for (int k = 0; k < 20000; k++) {
		uint32_t s0 = __atomic_load_n(g_cpos, __ATOMIC_ACQUIRE);
		if (s0 & 1) continue;
		uint32_t ver = g_cpos[1];
		volatile double *d = (volatile double *)((volatile char *)g_cpos + 256 + slot * 64);
		volatile double *a = (volatile double *)((volatile char *)g_cpos + 512 + slot * 128);
		double c = d[0], t[4] = { d[1], d[2], d[3], d[4] }, ax = a[0], m[9]; for (int i = 0; i < 9; i++) m[i] = a[1 + i];
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(g_cpos, __ATOMIC_ACQUIRE) != s0) continue;
		if (ver < 2 || c <= 0) return false;
		for (int i = 0; i < 4; i++) q[i] = t[i]; for (int i = 0; i < 9; i++) A[i] = m[i]; *axis = (int)ax;
		return true;
	}
	return false;
}
// desk-222 (q2ctltrack v3): at +768 + 64*h: count, position relative to the head (world axes), rotation (world), valid
static bool read_ctlworld(int slot, double p[3], double q[4])
{
	if (!g_cpos) return false;
	for (int k = 0; k < 20000; k++) {
		uint32_t s0 = __atomic_load_n(g_cpos, __ATOMIC_ACQUIRE);
		if (s0 & 1) continue;
		uint32_t ver = g_cpos[1];
		volatile double *d = (volatile double *)((volatile char *)g_cpos + 768 + slot * 64);
		double v[9]; for (int i = 0; i < 9; i++) v[i] = d[i];
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(g_cpos, __ATOMIC_ACQUIRE) != s0) continue;
		if (ver < 3 || v[0] <= 0 || v[8] < 0.5) return false;
		for (int i = 0; i < 3; i++) p[i] = v[1 + i]; for (int i = 0; i < 4; i++) q[i] = v[4 + i];
		return true;
	}
	return false;
}
// q2headtrack (desk-220): at 2048 + 128*slot of /dev/shm/q2ctl: counter, IMU-frame up, IMU-frame orientation (world)
static bool read_ctlbody(int slot, double q[4])
{
	if (!g_ctl) return false;
	for (int k = 0; k < 20000; k++) {
		uint32_t s0 = __atomic_load_n(g_ctl, __ATOMIC_ACQUIRE);
		if (s0 & 1) continue;
		volatile double *d = (volatile double *)((volatile char *)g_ctl + 2048 + slot * 128);
		double c = d[0], t[4] = { d[4], d[5], d[6], d[7] };
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(g_ctl, __ATOMIC_ACQUIRE) != s0) continue;
		if (c <= 0 || t[0] * t[0] + t[1] * t[1] + t[2] * t[2] + t[3] * t[3] < 0.5) return false;
		for (int i = 0; i < 4; i++) q[i] = t[i];
		return true;
	}
	return false;
}
static void m2q(const double R[9], double q[4])
{
	double tr = R[0] + R[4] + R[8];
	if (tr > 0) { double s = __builtin_sqrt(tr + 1) * 2; q[0] = s / 4; q[1] = (R[7] - R[5]) / s; q[2] = (R[2] - R[6]) / s; q[3] = (R[3] - R[1]) / s; }
	else if (R[0] > R[4] && R[0] > R[8]) { double s = __builtin_sqrt(1 + R[0] - R[4] - R[8]) * 2; q[0] = (R[7] - R[5]) / s; q[1] = s / 4; q[2] = (R[1] + R[3]) / s; q[3] = (R[2] + R[6]) / s; }
	else if (R[4] > R[8]) { double s = __builtin_sqrt(1 + R[4] - R[0] - R[8]) * 2; q[0] = (R[2] - R[6]) / s; q[1] = (R[1] + R[3]) / s; q[2] = s / 4; q[3] = (R[5] + R[7]) / s; }
	else { double s = __builtin_sqrt(1 + R[8] - R[0] - R[4]) * 2; q[0] = (R[3] - R[1]) / s; q[1] = (R[2] + R[6]) / s; q[2] = (R[5] + R[7]) / s; q[3] = s / 4; }
}
static void qnorm4(double q[4]) { double n = __builtin_sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]); if (n < 1e-12) { q[0] = 1; q[1] = q[2] = q[3] = 0; return; } for (int i = 0; i < 4; i++) q[i] /= n; }
// a += k * (b - a) on the short arc, normalised (nlerp)
static void qblend(double a[4], const double b[4], double k)
{
	double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], s = d < 0 ? -1 : 1;
	for (int i = 0; i < 4; i++) a[i] += k * (s * b[i] - a[i]);
	qnorm4(a);
}
static void q_yaw(const double q[4], double o[4])        // heading of q (rotation about +y)
{
	double f[3]; const double fw[3] = { 0, 0, -1 }; qrot(q, fw, f);
	double n = __builtin_sqrt(f[0] * f[0] + f[2] * f[2]);
	if (n < 1e-9) { o[0] = 1; o[1] = o[2] = o[3] = 0; return; }
	double c = -f[2] / n, s = -f[0] / n;                  // cos, sin of the yaw angle
	double ch = __builtin_sqrt((1 + c) / 2 > 0 ? (1 + c) / 2 : 0), sh;
	if (ch > 0.1) sh = s / (2 * ch);
	else { sh = __builtin_sqrt((1 - c) / 2 > 0 ? (1 - c) / 2 : 0); if (s < 0) sh = -sh; }
	o[0] = ch; o[1] = 0; o[2] = sh; o[3] = 0;
}
struct CtlIn { double trig, grip, sx, sy; uint32_t btn, touch; };
static float ctl_val(const char *c, const CtlIn &d, bool *scalar)
{
	*scalar = false;
	auto is = [&](const char *s) { return !strcmp(c, s); };
	float sm = (float)(d.sx * d.sx + d.sy * d.sy);
	if (is("ac") || is("av")) return d.btn & 1;
	if (is("at")) return (d.touch & 1) || (d.btn & 1);
	if (is("bc") || is("bv")) return (d.btn >> 1) & 1;
	if (is("bt")) return (d.touch & 2) || (d.btn & 2);
	if (is("sysc") || is("syst") || is("sysv")) return (d.btn >> 3) & 1;
	if (is("tv")) { *scalar = true; return (float)d.trig; }
	if (is("tc")) return d.trig > 0.7;
	if (is("tt")) return (d.touch & 8) || d.trig > 0.05;
	if (is("gv")) { *scalar = true; return (float)d.grip; }
	if (is("gc")) return d.grip > 0.7;
	if (is("gt")) return d.grip > 0.05;
	if (is("sx")) { *scalar = true; return (float)d.sx; }
	if (is("sy")) { *scalar = true; return (float)d.sy; }
	if (is("kc")) return (d.btn >> 2) & 1;
	if (is("kt")) return (d.touch & 4) || (d.btn & 4) || sm > 0.0025f;
	if (is("rt")) return (d.touch >> 4) & 1;
	return 0;
}
static const char *k_defcomp = "/input/system/click=sysc;/input/a/click=ac;/input/a/touch=at;/input/b/click=bc;/input/b/touch=bt;"
	"/input/x/click=ac;/input/x/touch=at;/input/y/click=bc;/input/y/touch=bt;/input/trigger/value=tv;/input/trigger/touch=tt;"
	"/input/grip/value=gv;/input/grip/touch=gt;/input/joystick/x=sx;/input/joystick/y=sy;/input/joystick/click=kc;"
	"/input/joystick/touch=kt;/input/thumbrest/touch=rt";
static bool g_sysmirror;
static double g_rightsys;                                 // right menu/Oculus state, mirrored to the left (oculus_touch)
class Q2Ctl : public ITrackedDeviceServerDriver {
public:
	explicit Q2Ctl(int s) : slot(s) {}
	int slot;                                              // 0 left, 1 right
	double camcnt = -1, campos[3] = { 0, 0, 0 }, camw = 0; int camstale = 100000; bool camuse = false; int camdbg = 0, camrej = 0;   // desk-216/219
	double fpos[3] = { 0, 0, 0 }, fvel[3] = { 0, 0, 0 }; bool fvalid = false; int sincecam = 100000; unsigned nsnap = 0;   // desk-222 filter
	// desk-220 orientation: IMU (model frame via the learned axes) with its heading corrected by the cameras
	double yawc[4] = { 1, 0, 0, 0 }; bool yawset = false; double visq[4] = { 1, 0, 0, 0 }; double visrot = -1; int visstale = 100000;
	int rotmode = -1; unsigned nyaw = 0, ntiltrej = 0; double tiltsum = 0;
	// desk-221: last good reads, held for a moment (a single failed read no longer flips the orientation source)
	double lastA[9] = { 0 }, lastbq[4] = { 1, 0, 0, 0 }, lastcq[4] = { 1, 0, 0, 0 }; int lastaxis = -1, ageA = 100000, agebq = 100000, misrot = 0, misbody = 0; double m2r_q[4] = { 1, 0, 0, 0 }, m2r_t[3] = { 0, 0, 0 };
	uint32_t id = k_unTrackedDeviceIndexInvalid;
	PropertyContainerHandle_t cont = 0;
	enum { NC = 40 };
	int nc = 0; VRInputComponentHandle_t ch[NC]; char code[NC][6]; float last[NC];
	double dfh_q[4] = { 1, 0, 0, 0 }, dfh_t[3] = { 0, 0, 0 };
	double lastcnt = -1; int stale = 100000; int lastbatt = -1; int src = -1;
	double mo_q[4] = { 1, 0, 0, 0 }, mo_t[3] = { 0, 0, 0 }; int mdbg = 0;   // desk-251: Meta pose -> SteamVR controller frame
	DriverPose_t pose = {};
	EVRInitError Activate(uint32_t unObjectId) override
	{
		id = unObjectId;
		PropertyContainerHandle_t c = cont = g_props->TrackedDeviceToPropertyContainer(id);
		const char *type = getenv("Q2VR_CTLTYPE"); if (!type || !*type) type = "oculus_touch";
		const char *prof = getenv("Q2VR_CTLPROFILE");
		const char *model = getenv("Q2VR_CTLMODEL"); if (!model || !*model) model = "oculus_quest2_controller_";
		char rm[160]; snprintf(rm, sizeof rm, "%s%s", model, slot ? "right" : "left");
		setStr(c, Prop_ModelNumber_String, slot ? "Quest 2 Touch (Right)" : "Quest 2 Touch (Left)");
		setStr(c, Prop_ManufacturerName_String, "Oculus");
		setStr(c, Prop_TrackingSystemName_String, "q2hmd");
		setStr(c, Prop_SerialNumber_String, slot ? "Q2CTL-RIGHT" : "Q2CTL-LEFT");
		setStr(c, Prop_ControllerType_String, type);
		setStr(c, Prop_InputProfilePath_String, prof && *prof ? prof : "{oculus}/input/touch_profile.json");
		setStr(c, Prop_RenderModelName_String, rm);
		setStr(c, Prop_RegisteredDeviceType_String, slot ? "q2hmd/Q2CTL-RIGHT" : "q2hmd/Q2CTL-LEFT");
		setVal<int32_t>(c, Prop_ControllerRoleHint_Int32, slot ? TrackedControllerRole_RightHand : TrackedControllerRole_LeftHand, k_unInt32PropertyTag);
		setVal<int32_t>(c, Prop_DeviceClass_Int32, TrackedDeviceClass_Controller, k_unInt32PropertyTag);
		setVal<bool>(c, Prop_DeviceProvidesBatteryStatus_Bool, true, k_unBoolPropertyTag);
		setVal<bool>(c, Prop_WillDriftInYaw_Bool, true, k_unBoolPropertyTag);
		g_sysmirror = !strcmp(type, "oculus_touch");
		// input components: "path=source;..." (q2vrtest builds it from the type's input profile)
		const char *cl = getenv("Q2VR_CTLCOMP"); if (!cl || !*cl) cl = k_defcomp;
		char path[128];
		while (*cl && nc < NC && g_input) {
			int i = 0; while (*cl && *cl != '=' && *cl != ';' && *cl != ',' && i < 127) path[i++] = *cl++;
			path[i] = 0;
			char cd[6]; int j = 0;
			if (*cl == '=') { cl++; while (*cl && *cl != ';' && *cl != ',') { if (j < 5) cd[j++] = *cl; cl++; } }
			cd[j] = 0;
			if (*cl == ';' || *cl == ',') cl++;
			if (!path[0] || !cd[0]) continue;
			CtlIn z = {}; bool sc; ctl_val(cd, z, &sc);
			EVRInputError e = sc ? g_input->CreateScalarComponent(c, path, &ch[nc], VRScalarType_Absolute,
			                                                      (!strcmp(cd, "sx") || !strcmp(cd, "sy")) ? VRScalarUnits_NormalizedTwoSided : VRScalarUnits_NormalizedOneSided)
			                     : g_input->CreateBooleanComponent(c, path, &ch[nc]);
			if (e == VRInputError_None) { for (int k = 0; k < 6; k++) code[nc][k] = cd[k]; last[nc] = -9; nc++; }
		}
		// driver-from-head: rotX(60) * inverse(openxr_grip) for the Quest models; openxr_grip = rotX(20.6), (+-0.007, -0.0018, 0.102)
		bool questModel = true; { const char *a = model, *b = "oculus_quest"; while (*b) if (*a++ != *b++) { questModel = false; break; } }
		if (questModel) {
			double a = 60 * 3.14159265358979 / 360, g = 20.6 * 3.14159265358979 / 360;
			double r60[4] = { q_cos(a), q_sin(a), 0, 0 }, gi[4] = { q_cos(g), -q_sin(g), 0, 0 };
			double gt[3] = { slot ? -0.007 : 0.007, -0.00182941, 0.1019482 }, it[3];
			qrot(gi, gt, it); it[0] = -it[0]; it[1] = -it[1]; it[2] = -it[2];   // inverse(grip).t
			qmul(r60, gi, dfh_q); qrot(r60, it, dfh_t);
		}
		{   // desk-220: /var/lib/q2/ctl_model2raw "rx ry rz tx ty tz" (deg, m): LED-model frame -> SteamVR controller frame
			int fd = open("/var/lib/q2/ctl_model2raw", 0); char b[160]; long n = fd >= 0 ? read(fd, b, sizeof b - 1) : 0; if (fd >= 0) close(fd);
			double v[6] = { 0, 0, 0, 0, 0, 0 }; int nv = 0;
			if (n > 0) { b[n] = 0; const char *s = b; while (*s && nv < 6) { while (*s == ' ' || *s == '\n' || *s == '\t') s++; if (!*s) break;
				double sg = 1, x = 0, sc = 1; if (*s == '-') { sg = -1; s++; } while (*s >= '0' && *s <= '9') x = x * 10 + (*s++ - '0'); if (*s == '.') { s++; while (*s >= '0' && *s <= '9') { x = x * 10 + (*s++ - '0'); sc *= 10; } }
				v[nv++] = sg * x / sc; while (*s && *s != ' ' && *s != '\n' && *s != '\t') s++; } }
			const double D = 3.14159265358979 / 360;
			double qx[4] = { q_cos(v[0] * D), q_sin(v[0] * D), 0, 0 }, qy[4] = { q_cos(v[1] * D), 0, q_sin(v[1] * D), 0 }, qz[4] = { q_cos(v[2] * D), 0, 0, q_sin(v[2] * D) }, t1[4];
			qmul(qz, qy, t1); qmul(t1, qx, m2r_q); m2r_t[0] = v[3]; m2r_t[1] = v[4]; m2r_t[2] = v[5];
			if (nv) Log("q2hmd: %s controller: model->controller offset %.1f %.1f %.1f deg, %.3f %.3f %.3f m (ctl_model2raw)\n", slot ? "right" : "left", v[0], v[1], v[2], v[3], v[4], v[5]);
		}
		{   // desk-251: /var/lib/q2/meta_ctl_offset "rx ry rz tx ty tz" (deg, m, given for the LEFT hand; the right hand
			// gets the mirror image: ry, rz and tx flipped): Meta's controller pose -> SteamVR controller frame.  Default none.
			int fd = open("/var/lib/q2/meta_ctl_offset", 0); char b[160]; long n = fd >= 0 ? read(fd, b, sizeof b - 1) : 0; if (fd >= 0) close(fd);
			double v[6] = { 0, 0, 0, 0, 0, 0 }; int nv = 0;
			if (n > 0) { b[n] = 0; const char *s = b; while (*s && nv < 6) { while (*s == ' ' || *s == '\n' || *s == '\t') s++; if (!*s) break;
				double sg = 1, x = 0, sc = 1; if (*s == '-') { sg = -1; s++; } while (*s >= '0' && *s <= '9') x = x * 10 + (*s++ - '0'); if (*s == '.') { s++; while (*s >= '0' && *s <= '9') { x = x * 10 + (*s++ - '0'); sc *= 10; } }
				v[nv++] = sg * x / sc; while (*s && *s != ' ' && *s != '\n' && *s != '\t') s++; } }
			if (slot) { v[1] = -v[1]; v[2] = -v[2]; v[3] = -v[3]; }
			const double D = 3.14159265358979 / 360;
			double qx[4] = { q_cos(v[0] * D), q_sin(v[0] * D), 0, 0 }, qy[4] = { q_cos(v[1] * D), 0, q_sin(v[1] * D), 0 }, qz[4] = { q_cos(v[2] * D), 0, 0, q_sin(v[2] * D) }, t1[4];
			qmul(qz, qy, t1); qmul(t1, qx, mo_q); mo_t[0] = v[3]; mo_t[1] = v[4]; mo_t[2] = v[5];
			Log("q2hmd: %s controller: Meta pose offset %.1f %.1f %.1f deg, %.3f %.3f %.3f m%s\n", slot ? "right" : "left", v[0], v[1], v[2], v[3], v[4], v[5], nv ? " (meta_ctl_offset)" : " (none)");
		}
		Log("q2hmd: %s controller (%s, %s, model %s) activated id %u, %d input components\n", slot ? "right" : "left", type,
		    prof && *prof ? prof : "{oculus}/input/touch_profile.json", rm, id, nc);
		return VRInitError_None;
	}
	void Deactivate() override { id = k_unTrackedDeviceIndexInvalid; }
	void EnterStandby() override {}
	void *GetComponent(const char *) override { return nullptr; }
	void DebugRequest(const char *, char *buf, uint32_t n) override { if (n) buf[0] = 0; }
	DriverPose_t GetPose() override { return pose; }
	// pose thread, 500 Hz
	void Update(const double qh[4], const double hp[3])
	{
		if (id == k_unTrackedDeviceIndexInvalid) return;
		double o[16]; bool have = read_ctl(slot, o);
		if (have && o[0] != lastcnt) { lastcnt = o[0]; stale = 0; } else if (stale < 100000) stale++;
		bool btn = have && stale < 1000;                   // buttons fresh within 2 s (a failed read reuses the last data)
		bool meta = g_mctl_ok[slot];                       // desk-251: pose from Meta's tracker (q2metapose)
		bool real = btn || meta;
		double pt[5] = { 0, 0, 0, 0, 0 };
		bool mouse = !real && slot == 1 && read_ptr(pt);
		int nsrc = meta ? (btn ? 3 : 4) : btn ? 1 : mouse ? 2 : 0;
		if (nsrc != src) { Log("q2hmd: %s hand now %s\n", slot ? "right" : "left", nsrc == 3 ? "Meta's tracking + Touch buttons" : nsrc == 4 ? "Meta's tracking (no button data)" :
		    nsrc == 1 ? "the Touch controller" : nsrc == 2 ? "the mouse pointer" : "not tracking"); src = nsrc; }
		DriverPose_t p = {};
		p.qWorldFromDriverRotation.w = 1; p.qRotation.w = 1; p.willDriftInYaw = true;
		p.qDriverFromHeadRotation.w = dfh_q[0]; p.qDriverFromHeadRotation.x = dfh_q[1];
		p.qDriverFromHeadRotation.y = dfh_q[2]; p.qDriverFromHeadRotation.z = dfh_q[3];
		for (int i = 0; i < 3; i++) p.vecDriverFromHeadTranslation[i] = dfh_t[i];
		CtlIn in = {};
		double aim[4] = { 1, 0, 0, 0 }, pos[3];
		if (meta) {
			// desk-251: Meta's own controller tracking - orientation, position and velocities as Meta has them (already
			// predicted to now and in the head's world/origin); buttons from q2ctlinput when it has them
			const double *m = g_mctl[slot];
			for (int i = 0; i < 4; i++) aim[i] = m[1 + i];
			for (int i = 0; i < 3; i++) {
				pos[i] = m[5 + i];
				double v = m[8 + i], w = m[11 + i];
				p.vecVelocity[i] = (v == v && v > -20 && v < 20) ? v : 0; p.vecAngularVelocity[i] = (w == w && w > -60 && w < 60) ? w : 0;
			}
			p.willDriftInYaw = false;
			p.qDriverFromHeadRotation.w = mo_q[0]; p.qDriverFromHeadRotation.x = mo_q[1]; p.qDriverFromHeadRotation.y = mo_q[2]; p.qDriverFromHeadRotation.z = mo_q[3];
			for (int i = 0; i < 3; i++) p.vecDriverFromHeadTranslation[i] = mo_t[i];
			if (btn) {
				in.trig = o[8]; in.grip = o[9]; in.sx = o[10]; in.sy = o[11]; in.btn = (uint32_t)o[12]; in.touch = (uint32_t)o[13];
				int b = (int)o[14];
				if (b != lastbatt && b >= 0) { lastbatt = b; setVal<float>(cont, Prop_DeviceBatteryPercentage_Float, b / 100.0f, k_unFloatPropertyTag); }
			}
			if (++mdbg % 5000 == 1) {
				double rel[3], hq[4] = { qh[0], -qh[1], -qh[2], -qh[3] }, d[3]; for (int i = 0; i < 3; i++) d[i] = pos[i] - hp[i]; qrot(hq, d, rel);
				Log("q2hmd: %s (Meta) head frame %+.2f %+.2f %+.2f m, q %.3f %.3f %.3f %.3f, buttons %s\n", slot ? "right" : "left", rel[0], rel[1], rel[2],
				    aim[0], aim[1], aim[2], aim[3], btn ? "yes" : "no");
			}
		} else if (btn) {
			aim[0] = o[1]; aim[1] = o[2]; aim[2] = o[3]; aim[3] = o[4];
			in.trig = o[8]; in.grip = o[9]; in.sx = o[10]; in.sy = o[11]; in.btn = (uint32_t)o[12]; in.touch = (uint32_t)o[13];
			for (int i = 0; i < 3; i++) p.vecAngularVelocity[i] = o[5 + i];
			double yq[4], e[3], f[3]; q_yaw(qh, yq);
			const double el[3] = { slot ? 0.17 : -0.17, -0.45, -0.05 }, fa[3] = { 0, 0, -0.30 };
			qrot(yq, el, e); qrot(aim, fa, f);
			for (int i = 0; i < 3; i++) pos[i] = hp[i] + e[i] + f[i];
			// desk-222 camera position.  q2ctltrack (v3) also gives the position and rotation already turned into the world
			// with the head orientation AT THE MOMENT OF THE FIT (so turning the head does not drag the controllers).
			// A position+velocity filter runs here at 500 Hz: between camera updates (30/s) the controller keeps moving
			// at its measured speed instead of stepping, and each update is treated as ~35 ms old (camera + processing).
			// Lost: it holds where it was (velocity fading out) for 1.5 s, then fades to the arm model over ~1 s.
			double cp[3], cc, cw[3], cwq[4]; bool haveworld = false;
			if (read_ctlpos(slot, cp, &cc)) {
				haveworld = read_ctlworld(slot, cw, cwq);
				bool fresh = cc != camcnt;
				if (fresh) { camcnt = cc; camstale = 0; } else if (camstale < 100000) camstale++;
				double wp[3]; if (haveworld) for (int i = 0; i < 3; i++) wp[i] = cw[i]; else qrot(qh, cp, wp);
				const double dt = 0.002, L = 0.035, T = 1.0 / 30;
				for (int i = 0; i < 3; i++) fpos[i] += fvel[i] * dt;            // predict to now
				if (fresh) {
					double m[3]; for (int i = 0; i < 3; i++) m[i] = hp[i] + wp[i];
					double e2 = 0; for (int i = 0; i < 3; i++) { double d = m[i] - fpos[i]; e2 += d * d; }
					if (!fvalid || sincecam > 150 || e2 > 0.4 * 0.4) {           // (re)start: jump there
						for (int i = 0; i < 3; i++) { fpos[i] = m[i]; fvel[i] = 0; } fvalid = true; nsnap++;
					} else {
						for (int i = 0; i < 3; i++) {
							double r = m[i] - (fpos[i] - fvel[i] * L);           // measurement vs the estimate at its own time
							fpos[i] += 0.55 * r; fvel[i] += 0.35 * r / T;
							if (fvel[i] > 4) fvel[i] = 4; if (fvel[i] < -4) fvel[i] = -4;
						}
					}
					sincecam = 0;
					if (!camuse) { Log("q2hmd: %s hand: position from the cameras\n", slot ? "right" : "left"); camuse = true; }
				} else if (sincecam < 100000) sincecam++;
				if (sincecam > 50) for (int i = 0; i < 3; i++) fvel[i] *= 0.985;     // no update for 0.1 s: stop extrapolating
				if (camuse && sincecam > 750) { Log("q2hmd: %s hand: cameras lost it for 1.5 s, arm model\n", slot ? "right" : "left"); camuse = false; }
				double target = fvalid && sincecam <= 750 ? 1.0 : 0.0;
				camw += (target - camw) * (target > camw ? 0.05 : 0.002);
				if (fvalid && camw > 0.001) for (int i = 0; i < 3; i++) pos[i] = pos[i] * (1 - camw) + fpos[i] * camw;
				if (++camdbg % 2500 == 0) {
					double rel[3], hq[4] = { qh[0], -qh[1], -qh[2], -qh[3] }, d[3]; for (int i = 0; i < 3; i++) d[i] = fpos[i] - hp[i]; qrot(hq, d, rel);
					Log("q2hmd: %s position: %s, head frame %+.2f %+.2f %+.2f, speed %.2f m/s, restarts %u, world-frame data %s\n", slot ? "right" : "left",
					    sincecam < 125 ? "tracking" : sincecam <= 750 ? "holding" : "arm model", rel[0], rel[1], rel[2],
					    __builtin_sqrt(fvel[0] * fvel[0] + fvel[1] * fvel[1] + fvel[2] * fvel[2]), nsnap, haveworld ? "yes" : "no");
				}
			}
			// desk-220 orientation.  Modes: 2 = controller IMU in the LED-model frame (axes learned by q2ctltrack) with its
			// heading pulled toward the cameras' heading; 1 = camera orientation alone (IMU axes not learned yet);
			// 0 = the old IMU aim (no camera orientation yet).
			{
				double cq[4], A[9]; int axis = -1; bool vfresh = false;
				if (read_ctlrot(slot, cq, A, &axis)) {
					for (int i = 0; i < 9; i++) lastA[i] = A[i]; for (int i = 0; i < 4; i++) lastcq[i] = cq[i]; lastaxis = axis; ageA = 0;
					if (camcnt != visrot) { visrot = camcnt; visstale = 0; vfresh = true; } else if (visstale < 100000) visstale++;
				} else { misrot++; if (ageA < 100000) ageA++; if (visstale < 100000) visstale++; }
				if (ageA < 250) { for (int i = 0; i < 9; i++) A[i] = lastA[i]; for (int i = 0; i < 4; i++) cq[i] = lastcq[i]; axis = lastaxis; }
				bool vok = visstale < 125 && camstale < 125;        // camera pose fresh (0.25 s)
				double vw[4]; if (vok || vfresh) { if (haveworld) for (int i = 0; i < 4; i++) vw[i] = cwq[i]; else qmul(qh, cq, vw); }
				double bq[4]; int mode = 0;
				if (read_ctlbody(slot, bq)) { for (int i = 0; i < 4; i++) lastbq[i] = bq[i]; agebq = 0; }
				else { misbody++; if (agebq < 100000) agebq++; for (int i = 0; i < 4; i++) bq[i] = lastbq[i]; }
				if (axis >= 0 && agebq < 250) {
					double At[9] = { A[0], A[3], A[6], A[1], A[4], A[7], A[2], A[5], A[8] }, qa[4]; m2q(At, qa); qnorm4(qa);
					double qi[4]; qmul(bq, qa, qi);                  // IMU orientation of the model frame (heading not yet right)
					if (vfresh && camstale < 125) {
						double cur[4]; qmul(yawc, qi, cur);
						double inv[4] = { cur[0], -cur[1], -cur[2], -cur[3] }, D[4]; qmul(vw, inv, D);   // camera * inverse(imu)
						double tw[4] = { D[0], 0, D[2], 0 }; qnorm4(tw);                      // heading part (about up)
						double tinv[4] = { tw[0], -tw[1], -tw[2], -tw[3] }, sw[4]; qmul(tinv, D, sw);   // tilt part
						if (sw[0] * sw[0] > 0.883) {                    // tilt agrees within ~40 deg: correct the heading
							double ny[4]; qmul(tw, yawc, ny);
							if (!yawset) { for (int i = 0; i < 4; i++) yawc[i] = ny[i]; yawset = true; } else qblend(yawc, ny, 0.08);
							nyaw++; tiltsum += 2 * __builtin_sqrt(1 - (sw[0] * sw[0] > 1 ? 1 : sw[0] * sw[0])) * 57.3;
						} else ntiltrej++;
					}
					qmul(yawc, qi, aim); mode = 2;
				} else if (vok) {
					if (rotmode != 1) { for (int i = 0; i < 4; i++) visq[i] = vw[i]; }
					else if (vfresh) qblend(visq, vw, 0.5);
					for (int i = 0; i < 4; i++) aim[i] = visq[i]; mode = 1;
				}
				if (mode != rotmode) {
					Log("q2hmd: %s controller orientation: %s\n", slot ? "right" : "left", mode == 2 ? "controller IMU, heading from the cameras" :
					    mode == 1 ? "cameras only (IMU axes not learned yet - move the controller around in view)" : "controller IMU only (old aim)");
					rotmode = mode;
				}
				if (mode) {                                      // aim now holds the LED-model frame: no aim->raw offset
					p.qDriverFromHeadRotation.w = m2r_q[0]; p.qDriverFromHeadRotation.x = m2r_q[1];
					p.qDriverFromHeadRotation.y = m2r_q[2]; p.qDriverFromHeadRotation.z = m2r_q[3];
					for (int i = 0; i < 3; i++) p.vecDriverFromHeadTranslation[i] = m2r_t[i];
				}
				if (camdbg % 2500 == 1)
					Log("q2hmd: %s orientation mode %d, IMU axis %d, heading updates %u (tilt diff avg %.1f deg), rejected %u, read misses camera %d imu %d\n", slot ? "right" : "left",
					    mode, axis, nyaw, nyaw ? tiltsum / nyaw : 0.0, ntiltrej, misrot, misbody);
			}
			int b = (int)o[14];
			if (b != lastbatt && b >= 0) { lastbatt = b; setVal<float>(cont, Prop_DeviceBatteryPercentage_Float, b / 100.0f, k_unFloatPropertyTag); }
		} else if (mouse) {
			// laser direction: head orientation, then the mouse's yaw (about up) and pitch (about right)
			double yq[4] = { q_cos(pt[0] / 2), 0, q_sin(pt[0] / 2), 0 }, pq[4] = { q_cos(pt[1] / 2), q_sin(pt[1] / 2), 0, 0 }, t[4];
			qmul(qh, yq, t); qmul(t, pq, aim);
			const double off[3] = { 0.12, -0.18, -0.25 }; double of[3]; qrot(qh, off, of);
			for (int i = 0; i < 3; i++) pos[i] = hp[i] + of[i];
			uint32_t bt = (uint32_t)pt[2];                 // bit0 trigger, bit1 B, bit2 A, bit3 system, bit4 stick click
			in.trig = bt & 1; in.btn = ((bt >> 2) & 1) | (((bt >> 1) & 1) << 1) | (((bt >> 4) & 1) << 2) | (((bt >> 3) & 1) << 3);
			in.sx = pt[3]; in.sy = pt[4];
		} else {
			pos[0] = hp[0]; pos[1] = hp[1] - 0.5; pos[2] = hp[2];
		}
		if (slot == 1) g_rightsys = (in.btn >> 3) & 1;
		else if (g_sysmirror && g_rightsys) in.btn |= 8;    // Touch dashboard bindings only bind the left system button
		p.qRotation.w = aim[0]; p.qRotation.x = aim[1]; p.qRotation.y = aim[2]; p.qRotation.z = aim[3];
		for (int i = 0; i < 3; i++) p.vecPosition[i] = pos[i];
		p.poseIsValid = real || mouse; p.deviceIsConnected = real || mouse;
		p.result = (real || mouse) ? TrackingResult_Running_OK : TrackingResult_Running_OutOfRange;
		pose = p;
		g_host->TrackedDevicePoseUpdated(id, p, sizeof p);
		for (int k = 0; k < nc; k++) {
			bool sc; float v = ctl_val(code[k], in, &sc);
			if (v == last[k]) continue;
			last[k] = v;
			if (sc) g_input->UpdateScalarComponent(ch[k], v, 0); else g_input->UpdateBooleanComponent(ch[k], v != 0, 0);
		}
	}
};
static Q2Ctl *g_ctldev[2];

// eye separation in mm from /var/lib/q2/ipdmm (Ctrl+Alt+[ / Ctrl+Alt+] change it while VR runs); 0 = none set
static int ipd_file()
{
	int fd = open("/var/lib/q2/ipdmm", 0); if (fd < 0) return 0;
	char b[16]; long n = read(fd, b, sizeof b - 1); close(fd); if (n <= 0) return 0;
	int v = 0; for (long i = 0; i < n && b[i] >= '0' && b[i] <= '9'; i++) v = v * 10 + (b[i] - '0');
	return v >= 50 && v <= 80 ? v : 0;
}
class Q2Hmd : public ITrackedDeviceServerDriver {
public:
	uint32_t id = k_unTrackedDeviceIndexInvalid;
	PropertyContainerHandle_t cont = 0; int ipd_mm = 63;
	void PollIpd()
	{
		int mm = ipd_file();
		if (!mm || mm == ipd_mm || !cont) return;
		ipd_mm = mm; setVal<float>(cont, Prop_UserIpdMeters_Float, mm / 1000.0f, k_unFloatPropertyTag);
		Log("q2hmd: eye separation now %d mm\n", mm);
	}
	Q2Display disp;
	EVRInitError Activate(uint32_t unObjectId) override
	{
		id = unObjectId;
		PropertyContainerHandle_t c = g_props->TrackedDeviceToPropertyContainer(id);
		setStr(c, Prop_ModelNumber_String, "Quest 2 (SteamOS)");
		setStr(c, Prop_ManufacturerName_String, "q2port");
		setStr(c, Prop_TrackingSystemName_String, "q2hmd");
		setStr(c, Prop_RenderModelName_String, "generic_hmd");
		setVal<float>(c, Prop_DisplayFrequency_Float, (float)envi("Q2VR_HZ", 72), k_unFloatPropertyTag);
		// eye separation from the IPD slider position (Meta: 1 = 58 mm, 2 = 63 mm, 3 = 68 mm)
		cont = c;
		float ipd = IPDPOS == 1 ? 0.058f : IPDPOS == 3 ? 0.068f : 0.063f; int mm = ipd_file();
		if (mm) ipd = mm / 1000.0f;
		setVal<float>(c, Prop_UserIpdMeters_Float, ipd, k_unFloatPropertyTag); ipd_mm = (int)(ipd * 1000 + 0.5f);
		Log("q2hmd: eyes: IPD %d mm (slider position %d), left eye on the %s half of the panel\n", ipd_mm, IPDPOS, envi("Q2VR_EYESWAP", 0) ? "lower" : "upper");
		setVal<float>(c, Prop_UserHeadToEyeDepthMeters_Float, 0.0f, k_unFloatPropertyTag);
		setVal<float>(c, Prop_SecondsFromVsyncToPhotons_Float, envi("Q2VR_PHOTON_MS", 16) / 1000.0f, k_unFloatPropertyTag);   // vsync -> copy -> panel scan
		setVal<bool>(c, Prop_IsOnDesktop_Bool, disp.IsDisplayOnDesktop(), k_unBoolPropertyTag);
		setVal<bool>(c, Prop_DisplayDebugMode_Bool, envi("Q2VR_DEBUGMODE", 0) != 0, k_unBoolPropertyTag);
		setVal<int32_t>(c, Prop_EdidVendorID_Int32, 0xD222, k_unInt32PropertyTag);   // "VLV": first entry of SteamVR's direct-mode list
		setVal<int32_t>(c, Prop_EdidProductID_Int32, 0xAA01, k_unInt32PropertyTag);
		static q2thread th; pthread_create(&th, 0, pose_thread, this);
		Log("q2hmd: activated id %u, display %ux%u, on desktop %d\n", id, DW, DH, (int)disp.IsDisplayOnDesktop());
		return VRInitError_None;
	}
	void Deactivate() override { id = k_unTrackedDeviceIndexInvalid; }
	static void *pose_thread(void *a)
	{
		Q2Hmd *h = (Q2Hmd *)a; q2ts ts = { 0, 2000000 };      // 500 Hz
		while (h->id != k_unTrackedDeviceIndexInvalid) {
			DriverPose_t p = h->GetPose();
			g_host->TrackedDevicePoseUpdated(h->id, p, sizeof p);
			if (g_ctldev[0] || g_ctldev[1]) {
				double qh[4] = { p.qRotation.w, p.qRotation.x, p.qRotation.y, p.qRotation.z };
				double hp[3] = { p.vecPosition[0], p.vecPosition[1], p.vecPosition[2] };
				for (int s = 0; s < 2; s++) if (g_ctldev[s]) g_ctldev[s]->Update(qh, hp);
			}
			nanosleep(&ts, 0);
		}
		return 0;
	}
	void EnterStandby() override {}
	void *GetComponent(const char *name) override
	{
		Log("q2hmd: GetComponent(%s)\n", name);
		if (!strcmp(name, IVRDisplayComponent_Version)) return &disp;
		return nullptr;
	}
	void DebugRequest(const char *, char *buf, uint32_t n) override { if (n) buf[0] = 0; }
	DriverPose_t GetPose() override
	{
		DriverPose_t p = {};
		p.poseIsValid = true; p.result = TrackingResult_Running_OK; p.deviceIsConnected = true;
		p.qWorldFromDriverRotation.w = 1; p.qDriverFromHeadRotation.w = 1; p.qRotation.w = 1;
		p.vecPosition[1] = 1.6; p.willDriftInYaw = true;     // 3DoF IMU, no magnetometer
		double o[8];
		if (read_pose(o)) {
			double w = o[0], x = o[1], y = o[2], z = o[3];
			p.qRotation.w = w; p.qRotation.x = x; p.qRotation.y = y; p.qRotation.z = z;
			// angular velocity: head frame -> world (rotate by q) so SteamVR can predict ahead
			double vx = o[5], vy = o[6], vz = o[7];
			double tx = 2 * (y * vz - z * vy), ty = 2 * (z * vx - x * vz), tz = 2 * (x * vy - y * vx);
			p.vecAngularVelocity[0] = vx + w * tx + (y * tz - z * ty);
			p.vecAngularVelocity[1] = vy + w * ty + (z * tx - x * tz);
			p.vecAngularVelocity[2] = vz + w * tz + (x * ty - y * tx);
			// neck model (Oculus SDK defaults, as in nicolas-riera/quest1-steamos): the eyes sit 7.5 cm above and
			// 8.05 cm in front of the neck pivot, so turning the head also moves them a little. Q2VR_NECK=0 turns it off.
			static int neck = envi("Q2VR_NECK", 1);
			static bool said;
			if (g_meta_ok) {
				// Meta's tracker: real 6DoF position (already the head position - no neck model)
				p.vecPosition[0] = g_meta_pos[0]; p.vecPosition[1] = g_meta_pos[1]; p.vecPosition[2] = g_meta_pos[2];
				p.willDriftInYaw = false;
				// desk-248: linear velocity too, so SteamVR predicts position (not only rotation) to the photon time
				for (int i = 0; i < 3; i++) { double v = g_meta_vel[i]; p.vecVelocity[i] = (v == v && v > -20 && v < 20) ? v : 0; }
				if (!said) { said = true; Log("q2hmd: 6DoF head tracking from Meta's tracker\n"); }
				// every 10 s: how old the Meta pose was when SteamVR got it (ms)
				static double amax, asum; static long an; static int cnt;
				double a = g_meta_age * 1000; asum += a; an++; if (a > amax) amax = a;
				if (++cnt >= 5000) { Log("q2hmd: Meta pose age avg %.2f ms, max %.2f ms\n", asum / an, amax); cnt = 0; amax = asum = 0; an = 0; }
			} else if (neck) {
				const double n[3] = { 0, 0.075, -0.0805 };
				double ux = 2 * (y * n[2] - z * n[1]), uy = 2 * (z * n[0] - x * n[2]), uz = 2 * (x * n[1] - y * n[0]);
				p.vecPosition[0] += ux * w + (y * uz - z * uy);
				p.vecPosition[1] += uy * w + (z * ux - x * uz);
				p.vecPosition[2] += uz * w + (x * uy - y * ux);
			}
		}
		return p;
	}
};

class Q2Provider : public IServerTrackedDeviceProvider {
public:
	Q2Hmd *hmd = nullptr;
	int frames = 0; bool ctl_added = false;
	EVRInitError Init(IVRDriverContext *ctx) override
	{
		EVRInitError e = VRInitError_None;
		g_log = (IVRDriverLog *)ctx->GetGenericInterface(IVRDriverLog_Version, &e);
		g_host = (IVRServerDriverHost *)ctx->GetGenericInterface(IVRServerDriverHost_Version, &e);
		g_props = (IVRProperties *)ctx->GetGenericInterface(IVRProperties_Version, &e);
		g_input = (IVRDriverInput *)ctx->GetGenericInterface(IVRDriverInput_Version, &e);
		if (!g_host || !g_props) return VRInitError_Init_InterfaceNotFound;
		TMAX = envi("Q2VR_TMAX", 240) / 100.0f; TPROJ = envi("Q2VR_TPROJ", 130) / 100.0f; DCTR = (float)envi("Q2VR_DCTR", 0);
		OLDLENS = envi("Q2VR_OLDLENS", 0); DW = (uint32_t)envi("Q2VR_DISPLAY_W", 1920); DH = (uint32_t)envi("Q2VR_DISPLAY_H", 3664); set_ipdpos(envi("Q2VR_IPDPOS", 2));   // desk-161: DW/DH smaller than the panel when the display engine scales up
		{   // fine lens-centre adjustment, 0.1 mm steps toward the nose (Ctrl+Alt+; / Ctrl+Alt+'), signed
			const char *v = getenv("Q2VR_LENSADJ"); int sg = 1, n = 0;
			if (v && *v == '-') { sg = -1; v++; }
			while (v && *v >= '0' && *v <= '9') n = n * 10 + (*v++ - '0');
			LENS_IN += sg * n * 0.0001f;
			if (n) Log("q2hmd: lens centre adjusted %+.1f mm toward the nose\n", sg * n * 0.1f);
		}
		Log("q2hmd: lens model %s, IPD slider position %d: FOV inner %.3f outer %.3f up %.3f down %.3f (tan), lens %.1f mm in\n",
		    OLDLENS ? "old" : "Meta Quest 2", IPDPOS, T_IN, T_OUT, T_UP, T_DN, LENS_IN * 1000);
		Log("q2hmd: Init (host %p props %p)\n", (void *)g_host, (void *)g_props);
		hmd = new Q2Hmd();
		bool ok = g_host->TrackedDeviceAdded("Q2HMD-0001", TrackedDeviceClass_HMD, hmd);
		Log("q2hmd: TrackedDeviceAdded -> %d\n", (int)ok);
		// The controllers are added once Steam has set up its VR input (RunFrame): when they already exist while Steam connects, vrserver loads
		// Steam's controller bindings inside Steam's SetActionManifestPath call, which then takes 1.1 s on this CPU - over
		// Steam's 1 s limit, so Steam logs "Failed to initialize SteamVR Input" and its VR dashboard never opens (desk-116..120).
		return VRInitError_None;
	}
	void Cleanup() override { Log("q2hmd: Cleanup\n"); }
	const char *const *GetInterfaceVersions() override { return k_InterfaceVersions; }
	void RunFrame() override
	{
		VREvent_t ev;
		while (g_host->PollNextEvent(&ev, sizeof ev)) {}
		if (++frames % 900 == 1) Log("q2hmd: RunFrame %d\n", frames);
		if (frames % 30 == 0 && hmd) hmd->PollIpd();
		// q2vrtest touches /dev/shm/q2ctl-go once vrserver has answered Steam's SetActionManifestPath (fallback: ~90 s)
		bool go = false;
		if (!ctl_added && frames % 25 == 0) { int fd = open("/dev/shm/q2ctl-go", 0); if (fd >= 0) { close(fd); go = true; } else if (frames > 1200) go = true; /* desk-217: 90 s -> ~15 s */ }
		if (!ctl_added && go && envi("Q2VR_CTL", 1)) {
			ctl_added = true;
			for (int s = 0; s < 2; s++) {
				Q2Ctl *d = new Q2Ctl(s);
				bool ok2 = g_host->TrackedDeviceAdded(s ? "Q2CTL-RIGHT" : "Q2CTL-LEFT", TrackedDeviceClass_Controller, d);
				g_ctldev[s] = d;
				Log("q2hmd: %s controller added -> %d\n", s ? "right" : "left", (int)ok2);
			}
		}
	}
	bool ShouldBlockStandbyMode() override { return false; }
	void EnterStandby() override {}
	void LeaveStandby() override {}
};

static Q2Provider g_provider;

extern "C" __attribute__((visibility("default"))) void *HmdDriverFactory(const char *iface, int *rc)
{
	if (!strcmp(iface, IServerTrackedDeviceProvider_Version)) return &g_provider;
	if (rc) *rc = VRInitError_Init_InterfaceNotFound;
	return nullptr;
}
