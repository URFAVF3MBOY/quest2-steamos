// q2cam_tpg - Quest 2 camera pipeline test, phase B.5: one frame from the CSID TEST PATTERN through Meta's
// IFE "fastpath" (no sensor, no csiphy, no camera power rails, no SyncBoss).
//
// What it does (all from the kernel's own UAPI and drivers, techpack/camera in the Quest 2 kernel source):
//   1. /dev/video0 (cam-req-mgr): CREATE_SESSION
//   2. /dev/v4l-subdev* named 'cam-isp': CAM_QUERY_CAP -> IOMMU handles (image + CDM)
//   3. buffers: output image (640x480, IFE-mapped, CPU-readable), command buffer (CDM-mapped, kernel+user
//      access: generic blobs + room for the kernel's own commands), packet buffer (kernel+user access)
//   4. 'cam-isp-fastpath' subdev: CAM_ACQUIRE_DEV (num_resources = CAM_API_COMPAT_CONSTANT) -> dev handle;
//      its bits 31:24 = fastpath context index -> /dev/cam_fp_ife<idx>_q
//   5. CAM_ACQUIRE_HW v2: input = CSID test pattern generator (0x4000), output = RDI0 (0x3006), PLAIN8 640x480
//   6. CAM_FP_ENQUEUE_BUFSET (RDI0 = bit 8) BEFORE the INIT packet (cam_isp_fpc_prepare_hw_config takes the
//      first buffer set from the queue for request 1)
//   7. CAM_CONFIG_DEV with the INIT packet (op_code 0 = INIT: the kernel adds 1), blobs: IFE clock, CSID
//      clock, bandwidth v2 (RDI0 write path)
//   8. CAM_START_DEV, poll() the queue, CAM_FP_DEQUEUE_BUFSET, save the frame as /tmp/q2cam_tpg.pgm
//   9. STOP, RELEASE_HW, RELEASE_DEV, RELEASE_BUF x3, DESTROY_SESSION (also on every error path)
//
// After every step the result is printed.  If a step fails, run `dmesg | grep CAM_ | tail -40` - the
// kernel names the exact reason.  Built freestanding (raw syscalls, no libc), see build.sh.
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
	const char *pre = "q2cam-tpg: "; while (*pre) put(&o, e, *pre++);
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
	case 110: return "ETIMEDOUT"; default: return "error"; }
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
	P("ALLOC_BUF %s ok: handle %x fd %d, %llu bytes, cpu %s", what, (unsigned)b->hdl, b->fd, (unsigned long long)len, b->map ? "mapped" : "NOT mapped");
	return 0;
}
static void free_buf(int crm, struct buf *b, const char *what)
{
	if (!b->hdl) return;
	if (b->map) sys_munmap(b->map, b->len);
	if (b->fd > 0) sys_close(b->fd);
	struct cam_mem_mgr_release_cmd rc; memset(&rc, 0, sizeof rc); rc.buf_handle = b->hdl;
	long r = cam_ctl(crm, CAM_REQ_MGR_RELEASE_BUF, &rc, sizeof rc);
	P("RELEASE_BUF %s: %s", what, r ? errname(r) : "ok");
	b->hdl = 0;
}

// blob header: type in bits 7:0, size in bits 31:8; payload padded to 4 bytes
static uint32_t *add_blob(uint32_t *p, uint32_t type, const void *data, uint32_t size)
{
	*p++ = (type & 0xFF) | (size << 8);
	memcpy(p, data, size);
	return p + (size + 3) / 4;
}

#define W 640
#define H 480

int main(int argc, char **argv)
{
	int pattern = 1;
	if (argc > 1 && argv[1][0] >= '1' && argv[1][0] <= '9') pattern = argv[1][0] - '0';
	logfd = sys_open("/var/log/q2/camtpg.txt", 01 | 0100 | 01000 /*WRONLY|CREAT|TRUNC*/);
	P("Phase B.5b start: CSID test pattern %d -> IFE fastpath RDI0, %dx%d MIPI RAW8 (frame-based write); no sensor, no csiphy, no SyncBoss", pattern, W, H);

	int crm = -1, isp = -1, fp = -1, q = -1, rc_main = 1;
	int32_t session = 0, dev = 0; int hw_acq = 0, started = 0;
	struct buf out = { 0 }, cmd = { 0 }, pkt = { 0 };
	char path[64];

	// 1. session
	if (find_node("video4linux", "video", "cam-req-mgr", path) == 0) crm = sys_open(path, 2 /*RDWR*/);
	if (crm < 0) { crm = sys_open("/dev/video0", 2); memcpy(path, "/dev/video0", 12); }
	if (crm < 0) { P("cannot open cam-req-mgr: %d", crm); return 1; }
	P("CRM %s fd %d", path, crm);
	struct cam_req_mgr_session_info si; memset(&si, 0, sizeof si);
	long r = cam_ctl(crm, CAM_REQ_MGR_CREATE_SESSION, &si, sizeof si);
	if (r) { P("CREATE_SESSION failed: %s", errname(r)); goto out; }
	session = si.session_hdl; P("CREATE_SESSION ok: session %x", (unsigned)session);

	// 2. ISP IOMMU handles
	if (find_node("video4linux", "v4l-subdev", "cam-isp", path) || (isp = sys_open(path, 2)) < 0) { P("cam-isp subdev not found"); goto out; }
	struct cam_isp_query_cap_cmd iq; memset(&iq, 0, sizeof iq);
	struct cam_query_cap_cmd qc; memset(&qc, 0, sizeof qc);
	qc.size = sizeof iq; qc.handle_type = CAM_HANDLE_USER_POINTER; qc.caps_handle = (uint64_t)(uintptr_t)&iq;
	r = cam_ctl(isp, CAM_QUERY_CAP, &qc, sizeof qc);
	if (r) { P("ISP QUERY_CAP failed: %s", errname(r)); goto out; }
	int32_t img_mmu = iq.device_iommu.non_secure, cdm_mmu = iq.cdm_iommu.non_secure;
	P("ISP %s: image iommu %d, cdm iommu %d, %d IFE(s)", path, img_mmu, cdm_mmu, iq.num_dev);

	// 3. buffers
	{ int32_t m1[1] = { img_mmu }; if (alloc_buf(crm, &out, 327680, CAM_MEM_FLAG_HW_READ_WRITE | CAM_MEM_FLAG_UMD_ACCESS | CAM_MEM_FLAG_PIXEL_BUF_TYPE, m1, 1, "output 640x480")) goto out; }
	{ int32_t m2[2] = { cdm_mmu, img_mmu }; if (alloc_buf(crm, &cmd, 8192, CAM_MEM_FLAG_HW_READ_WRITE | CAM_MEM_FLAG_KMD_ACCESS | CAM_MEM_FLAG_UMD_ACCESS | CAM_MEM_FLAG_CMD_BUF_TYPE, m2, 2, "command")) goto out; }
	if (alloc_buf(crm, &pkt, 8192, CAM_MEM_FLAG_KMD_ACCESS | CAM_MEM_FLAG_UMD_ACCESS | CAM_MEM_FLAG_PACKET_BUF_TYPE, 0, 0, "packet")) goto out;
	if (!out.map || !cmd.map || !pkt.map) { P("a buffer could not be mapped for the CPU - stopping"); goto out; }
	memset(out.map, 0x5a, out.len);   // marker: if the frame comes back still 0x5a, nothing was written

	// 4. fastpath device handle
	if (find_node("video4linux", "v4l-subdev", "cam-isp-fastpath", path) || (fp = sys_open(path, 2)) < 0) { P("cam-isp-fastpath subdev not found"); goto out; }
	struct cam_acquire_dev_cmd ad; memset(&ad, 0, sizeof ad);
	ad.session_handle = session; ad.handle_type = CAM_HANDLE_USER_POINTER; ad.num_resources = CAM_API_COMPAT_CONSTANT;
	r = cam_ctl(fp, CAM_ACQUIRE_DEV, &ad, sizeof ad);
	if (r) { P("fastpath %s ACQUIRE_DEV failed: %s", path, errname(r)); goto out; }
	dev = ad.dev_handle;
	unsigned ctxi = FP_DEV_GET_HDL_IDX((unsigned)dev);
	P("fastpath %s ACQUIRE_DEV ok: dev handle %x -> context %u", path, (unsigned)dev, ctxi);

	// 5. hardware: TPG in, RDI0 out
	{
		static uint64_t hwbuf[64];
		struct cam_isp_acquire_hw_info *hi = (void *)hwbuf;
		struct cam_isp_in_port_info_v2 *in = (void *)&hi->data;
		memset(hwbuf, 0, sizeof hwbuf);
		hi->common_info_version = 0x2000;          // major 2 -> cam_isp_in_port_info_v2
		hi->num_inputs = 1;
		hi->input_info_version = 0x00020000;
		hi->input_info_size = sizeof(*in);         // one output port (data[1] is part of the struct)
		hi->input_info_offset = 0;                 // relative to &hi->data
		in->res_type = CAM_ISP_IFE_IN_RES_TPG;
		in->lane_type = CAM_ISP_LANE_TYPE_DPHY; in->lane_num = 1; in->lane_cfg = 0x3210;
		in->vc[0] = 0; in->dt[0] = 0x2A; in->num_valid_vc_dt = 1;
		in->format = CAM_FORMAT_MIPI_RAW_8; in->test_pattern = pattern; in->usage_type = 0;
		in->left_start = 0; in->left_stop = W - 1; in->left_width = W;
		in->line_start = 0; in->line_stop = H - 1; in->height = H;
		in->num_out_res = 1;
		in->data[0].res_type = CAM_ISP_IFE_OUT_RES_RDI_0; in->data[0].format = CAM_FORMAT_MIPI_RAW_8;   // B.5b: frame-based RDI write (PLAIN8 gave an image-size violation on IFE-lite)
		in->data[0].width = W; in->data[0].height = H;
		uint32_t size = (uint32_t)(offsetof(struct cam_isp_acquire_hw_info, data) + sizeof(*in));
		struct cam_acquire_hw_cmd_v2 ah; memset(&ah, 0, sizeof ah);
		ah.struct_version = CAM_ACQUIRE_HW_STRUCT_VERSION_2; ah.session_handle = session; ah.dev_handle = dev;
		ah.handle_type = CAM_HANDLE_USER_POINTER; ah.data_size = size; ah.resource_hdl = (uint64_t)(uintptr_t)hwbuf;
		r = cam_ctl(fp, CAM_ACQUIRE_HW, &ah, sizeof ah);
		if (r) { P("ACQUIRE_HW (TPG -> RDI0) failed: %s", errname(r)); goto out; }
		hw_acq = 1;
		P("ACQUIRE_HW ok (valid_acquired_hw %u, hw id %u)", ah.hw_info.valid_acquired_hw, ah.hw_info.acquired_hw_id[0]);
	}

	// 6. buffer set into the context's queue (before the INIT packet)
	{
		char qp[32]; const char *a = "/dev/cam_fp_ife"; char *o = qp; while (*a) *o++ = *a++;
		if (ctxi >= 10) *o++ = '0' + ctxi / 10; *o++ = '0' + ctxi % 10; a = "_q"; while (*a) *o++ = *a++; *o = 0;
		q = sys_open(qp, 2);
		if (q < 0) { P("open %s failed: %d", qp, q); goto out; }
		static struct cam_fp_buffer_set bs; memset(&bs, 0, sizeof bs);
		bs.request_id = 1; bs.user_data = 0x7e57;
		bs.out_buffer_set_mask = 1ull << 8;        // ife_port_res_lut[8] = 0x3006 = RDI0
		bs.out_bufs[8].num_planes = 1; bs.out_bufs[8].plane[0].handle = out.hdl; bs.out_bufs[8].plane[0].offset = 0;
		r = sys_ioctl(q, CAM_FP_ENQUEUE_BUFSET, &bs);
		if (r) { P("ENQUEUE_BUFSET on %s failed: %s", qp, errname(r)); goto out; }
		P("ENQUEUE_BUFSET on %s ok (request 1, RDI0 -> handle %x)", qp, (unsigned)out.hdl);
	}

	// 7. INIT packet
	{
		// command buffer: [0, 1024) blobs, [1024, 8192) free room for the kernel's own commands
		uint32_t *bp = cmd.map, *b0 = bp;
		struct cam_isp_clock_config ck; memset(&ck, 0, sizeof ck);
		ck.usage_type = 0; ck.num_rdi = 1; ck.left_pix_hz = 0; ck.right_pix_hz = 0; ck.rdi_hz[0] = 400000000ull;
		bp = add_blob(bp, CAM_ISP_GENERIC_BLOB_TYPE_CLOCK_CONFIG, &ck, sizeof ck);
		struct cam_isp_csid_clock_config cc = { 400000000ull };
		bp = add_blob(bp, CAM_ISP_GENERIC_BLOB_TYPE_CSID_CLOCK_CONFIG, &cc, sizeof cc);
		struct cam_isp_bw_config_v2 bw; memset(&bw, 0, sizeof bw);
		bw.usage_type = 0; bw.num_paths = 1;
		bw.axi_path[0].usage_data = CAM_ISP_USAGE_RDI; bw.axi_path[0].transac_type = CAM_AXI_TRANSACTION_WRITE;
		bw.axi_path[0].path_data_type = CAM_AXI_PATH_DATA_IFE_RDI0;
		bw.axi_path[0].camnoc_bw = 200000000ull; bw.axi_path[0].mnoc_ab_bw = 200000000ull; bw.axi_path[0].mnoc_ib_bw = 200000000ull;
		bw.axi_path[0].ddr_ab_bw = 200000000ull; bw.axi_path[0].ddr_ib_bw = 200000000ull;
		bp = add_blob(bp, CAM_ISP_GENERIC_BLOB_TYPE_BW_CONFIG_V2, &bw, sizeof bw);
		uint32_t blob_len = (uint32_t)((uint8_t *)bp - (uint8_t *)b0);

		uint8_t *pb = pkt.map; memset(pb, 0, 8192);
		struct cam_packet *p = (void *)pb;
		uint32_t pay = offsetof(struct cam_packet, payload);
		struct cam_cmd_buf_desc *cd = (void *)(pb + pay);           // cmd_buf_offset 0
		struct cam_buf_io_cfg *io = (void *)(pb + pay + 2 * sizeof(*cd));
		p->header.op_code = 0;                                       // kernel: (0 + 1) & 0xF == CAM_ISP_PACKET_INIT_DEV
		p->header.request_id = 1;
		p->cmd_buf_offset = 0; p->num_cmd_buf = 2;
		p->io_configs_offset = 2 * sizeof(*cd); p->num_io_configs = 1;
		p->patch_offset = 2 * sizeof(*cd) + sizeof(*io); p->num_patches = 0;
		p->kmd_cmd_buf_index = 1; p->kmd_cmd_buf_offset = 0;
		p->header.size = pay + 2 * sizeof(*cd) + sizeof(*io);
		cd[0].mem_handle = cmd.hdl; cd[0].offset = 0; cd[0].size = 1024; cd[0].length = blob_len;
		cd[0].type = CAM_CMD_BUF_GENERIC; cd[0].meta_data = CAM_ISP_PACKET_META_GENERIC_BLOB_COMMON;
		cd[1].mem_handle = cmd.hdl; cd[1].offset = 1024; cd[1].size = 8192 - 1024; cd[1].length = 0;
		cd[1].type = CAM_CMD_BUF_DIRECT; cd[1].meta_data = CAM_ISP_PACKET_META_COMMON;
		io->mem_handle[0] = out.hdl; io->offsets[0] = 0;
		io->planes[0].width = W; io->planes[0].height = H; io->planes[0].plane_stride = W; io->planes[0].slice_height = H;
		io->format = CAM_FORMAT_MIPI_RAW_8; io->resource_type = CAM_ISP_IFE_OUT_RES_RDI_0;
		io->fence = -1; io->early_fence = -1; io->direction = CAM_BUF_OUTPUT;
		struct cam_config_dev_cmd cf; memset(&cf, 0, sizeof cf);
		cf.session_handle = session; cf.dev_handle = dev; cf.offset = 0; cf.packet_handle = (uint64_t)(uint32_t)pkt.hdl;
		r = cam_ctl(fp, CAM_CONFIG_DEV, &cf, sizeof cf);
		if (r) { P("CONFIG_DEV (INIT packet, %u blob bytes) failed: %s", blob_len, errname(r)); goto out; }
		P("CONFIG_DEV ok (INIT packet: clock + CSID clock + bandwidth blobs, RDI0 io config)");
	}

	// 8. start, wait, dequeue
	{
		struct cam_start_stop_dev_cmd st = { session, dev };
		r = cam_ctl(fp, CAM_START_DEV, &st, sizeof st);
		if (r) { P("START_DEV failed: %s", errname(r)); goto out; }
		started = 1; P("START_DEV ok - waiting up to 3 s for the frame");
		struct { int fd; short ev, rev; } pf = { q, 1 /*POLLIN*/, 0 };
		struct ts to = { 3, 0 };
		long n = sc6(SYS_ppoll, (long)&pf, 1, (long)&to, 0, 0, 0);
		static struct cam_fp_buffer_set done; memset(&done, 0, sizeof done);
		if (n <= 0) { P("no frame within 3 s (poll %d) - check dmesg for CSID/IFE errors", (int)n); goto out; }
		r = sys_ioctl(q, CAM_FP_DEQUEUE_BUFSET, &done);
		if (r) { P("DEQUEUE_BUFSET failed: %s", errname(r)); goto out; }
		P("FRAME: request %llu status %u (0 = success) timestamp %llu sof %u", (unsigned long long)done.request_id, done.status,
		  (unsigned long long)done.timestamp, done.sof_index);
		const uint8_t *px = out.map; unsigned long sum = 0, n5a = 0; unsigned mn = 255, mx = 0;
		for (int i = 0; i < W * H; i++) { sum += px[i]; if (px[i] == 0x5a) n5a++; if (px[i] < mn) mn = px[i]; if (px[i] > mx) mx = px[i]; }
		P("pixels: mean %u, min %u, max %u, untouched 0x5a bytes %u of %u; first 8: %u %u %u %u %u %u %u %u",
		  (unsigned)(sum / (W * H)), mn, mx, (unsigned)n5a, W * H, px[0], px[1], px[2], px[3], px[4], px[5], px[6], px[7]);
		int f = sys_open("/var/log/q2/camtpg_frame.bin", 01 | 0100 | 01000);
		if (f >= 0) { const char *hdr = "P5\n640 480\n255\n"; sys_write(f, hdr, slen(hdr)); sys_write(f, px, W * H); sys_close(f); P("saved /var/log/q2/camtpg_frame.bin (a PGM image: rename to .pgm to view)"); }
		rc_main = done.status == 0 ? 0 : 2;
	}

out:
	// 9. clean up in reverse order (each step only if it happened)
	if (started) { struct cam_start_stop_dev_cmd st = { session, dev }; r = cam_ctl(fp, CAM_STOP_DEV, &st, sizeof st); P("STOP_DEV: %s", r ? errname(r) : "ok"); }
	if (hw_acq) { struct cam_release_hw_cmd_v1 rh; memset(&rh, 0, sizeof rh); rh.struct_version = 1; rh.session_handle = session; rh.dev_handle = dev;
		r = cam_ctl(fp, CAM_RELEASE_HW, &rh, sizeof rh); P("RELEASE_HW: %s", r ? errname(r) : "ok"); }
	if (dev) { struct cam_release_dev_cmd rd = { session, dev }; r = cam_ctl(fp, CAM_RELEASE_DEV, &rd, sizeof rd); P("RELEASE_DEV: %s", r ? errname(r) : "ok"); }
	if (q >= 0) sys_close(q);
	if (crm >= 0) { free_buf(crm, &pkt, "packet"); free_buf(crm, &cmd, "command"); free_buf(crm, &out, "output"); }
	if (session) { struct cam_req_mgr_session_info s2 = { session, 0 }; r = cam_ctl(crm, CAM_REQ_MGR_DESTROY_SESSION, &s2, sizeof s2); P("DESTROY_SESSION: %s", r ? errname(r) : "ok"); }
	if (fp >= 0) sys_close(fp);
	if (isp >= 0) sys_close(isp);
	if (crm >= 0) sys_close(crm);
	P("Phase B.5b end: %s", rc_main == 0 ? "TEST-PATTERN FRAME RECEIVED" : "no frame (see the failing step above and dmesg | grep CAM_)");
	if (logfd >= 0) sys_close(logfd);
	(void)sleep_ms;
	return rc_main;
}

__attribute__((naked, noreturn)) void _start(void)
{
	__asm__ volatile(
		"ldr x0, [sp]\n"        // argc
		"add x1, sp, #8\n"      // argv
		"and sp, x1, #~15\n"
		"bl c_start\n");
}
__attribute__((used)) void c_start(long argc, char **argv) { sys_exit(main((int)argc, argv)); }
