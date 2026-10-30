// q2camlive - Quest 2 camera pipeline, phase B.6d (desk-212): all four OV7251 tracking cameras at 60 fps.
//
// What we know (desk-207 stock files + the Quest 2 kernel source):
//  * The cameras are OV7251 (640x480 global shutter mono, 1 MIPI lane, RAW10, 640x481 incl. one extra line).
//  * The kernel does NOT talk to them over I2C: hollywood-camera.dtsi has no "oculus,cam-i2c-ap-controlled", so
//    cam_sensor_io turns every sensor read/write into a no-op and the probe skips power-up and ID check.  The SyncBoss
//    MCU (nRF) configures and starts the sensors (libsyncboss: camera_start_streaming = MCU command 0x2c).
//  * The AP still supplies the rails and the clock: cam-sensor ACQUIRE_DEV runs the power-up sequence
//    (VANA 3.0 V L10C, VIO 1.8 V S4A, MCLK; voltages come from the device tree, not from us).
//  * Data path: sensor -> csiphy N (D-PHY, 1 lane) -> CSID (in-port PHY_N) -> IFE RDI0 -> memory, via Meta's
//    fastpath exactly like the B.5b test-pattern capture.
//
// Steps:
//   1. session, ISP IOMMU handles, buffers
//   2. every 'cam-sensor-driver' subdev: QUERY_CAP (slot + csiphy), PROBE (power sequence from Meta's
//      com.qti.sensormodule.liteon_ov7251_*.bin), ACQUIRE_DEV (= power up)
//   3. ask the head tracker (the SyncBoss owner) to send camera stop + start to the MCU (/dev/shm/q2ht-sbcam)
//   4. for each camera: csiphy ACQUIRE/CONFIG/START, fastpath ACQUIRE_HW (PHY in, RDI0 out, RAW10), INIT, START,
//      dequeue/re-enqueue frames for ~3 s, save the first and last frame, tear down
//   5. camera stop to the MCU, release the sensors (power down), destroy the session
// Output: /var/log/q2/camlive.txt, camlive_camN_{first,last}_pgm.bin (8-bit PGM preview; .bin so the log export
// copies it), camlive_camN_{first,last}_raw10.bin (raw MIPI RAW10 bytes, 800 per line)
// Arguments: [settle_count] [camera cell index or -1 for all]

#define TAG "q2cam-live: "
#define LOGF "/var/log/q2/camlive.txt"
#include "q2cam_rt.h"
#include <media/cam_sensor.h>

#define W 640
#define H 481
#define STRIDE 800                    // RAW10: 640 * 10 / 8
#define OUTSZ (STRIDE * 512)          // a little more than 800 * 481

struct sensor { int fd; int slot; int csiphy; int32_t dev; int powered; char path[32]; };
static struct sensor sens[8]; static int nsens;
static int crm = -1, isp = -1;
static int32_t session, img_mmu, cdm_mmu;

static long cam_ctl_mem(int fd, uint32_t op, int32_t mem_hdl)
{
	struct cam_control c; memset(&c, 0, sizeof c);
	c.op_code = op; c.size = 0; c.handle_type = CAM_HANDLE_MEM_HANDLE; c.handle = (uint64_t)(uint32_t)mem_hdl;
	return sys_ioctl(fd, VIDIOC_CAM_CONTROL, &c);
}

// ---------------------------------------------------------------- 2. sensors: probe + power up
// Meta's power sequence for the OV7251 modules (powerSetting in the sensor module .bin):
//   up:   RESET 0 +1 ms, VANA +1 ms, VIO, MCLK 19.2 MHz +1 ms, RESET 1 +18 ms
//   down: MCLK +1 ms, RESET 0 +1 ms, VIO, VANA
// RESET has no GPIO in the device tree (the MCU owns it), so those steps are no-ops in the kernel.
struct pstep { uint16_t type; uint32_t val; int16_t delay_ms; };
static const struct pstep pup[] = { { 8, 0, 1 }, { 1, 0, 1 }, { 3, 0, 0 }, { 0, 19200000, 1 }, { 8, 1, 18 } };
static const struct pstep pdn[] = { { 0, 0, 1 }, { 8, 0, 1 }, { 3, 0, 0 }, { 1, 0, 0 } };
static uint8_t *put_power(uint8_t *p, int up, const struct pstep *s, int n)
{
	for (int i = 0; i < n; i++) {
		struct cam_cmd_power *c = (void *)p; memset(c, 0, sizeof *c);
		c->count = 1; c->cmd_type = up ? 2 /*PWR_UP*/ : 3 /*PWR_DOWN*/;
		c->power_settings[0].power_seq_type = s[i].type; c->power_settings[0].config_val_low = s[i].val;
		p += sizeof *c;
		if (s[i].delay_ms) {
			struct cam_cmd_unconditional_wait *w = (void *)p; memset(w, 0, sizeof *w);
			w->delay = s[i].delay_ms; w->op_code = 3 /*SW_UCND*/; w->cmd_type = 9 /*WAIT*/;
			p += sizeof *w;
		}
	}
	return p;
}

static int sensors_up(void)
{
	struct buf pk = { 0 }, cb = { 0 };
	if (alloc_buf(crm, &pk, 4096, CAM_MEM_FLAG_KMD_ACCESS | CAM_MEM_FLAG_UMD_ACCESS | CAM_MEM_FLAG_PACKET_BUF_TYPE, 0, 0, "sensor packet")) return -1;
	if (alloc_buf(crm, &cb, 4096, CAM_MEM_FLAG_KMD_ACCESS | CAM_MEM_FLAG_UMD_ACCESS | CAM_MEM_FLAG_PACKET_BUF_TYPE, 0, 0, "sensor commands")) { free_buf(crm, &pk, "sensor packet"); return -1; }
	char path[32];
	for (int i = 0; i < 40 && nsens < 8; i++) {
		if (find_node_n("video4linux", "v4l-subdev", "cam-sensor-driver", path, i)) continue;
		struct sensor *s = &sens[nsens]; memset(s, 0, sizeof *s);
		s->fd = sys_open(path, 2); if (s->fd < 0) { P("open %s failed: %d", path, s->fd); continue; }
		memcpy(s->path, path, 32);
		struct cam_sensor_query_cap qc; memset(&qc, 0, sizeof qc);
		long r = cam_ctl(s->fd, CAM_QUERY_CAP, &qc, sizeof qc);
		if (r) { P("sensor %s QUERY_CAP failed: %s", path, errname(r)); sys_close(s->fd); continue; }
		s->slot = qc.slot_info; s->csiphy = qc.csiphy_slot_id;
		P("sensor %s: slot %d, csiphy %d, roll %d pitch %d yaw %d", path, s->slot, s->csiphy, qc.pos_roll, qc.pos_pitch, qc.pos_yaw);
		// probe packet: cmd 0 = i2c info + probe (slave 0xE0, as in the module file; the ID check is skipped by the
		// kernel for MCU-controlled sensors), cmd 1 = power up/down sequence
		uint8_t *c0 = cb.map; memset(c0, 0, 4096);
		struct cam_cmd_i2c_info *ii = (void *)c0; ii->slave_addr = 0xE0; ii->i2c_freq_mode = 1 /*FAST*/; ii->cmd_type = 4 /*I2C_INFO*/;
		struct cam_cmd_probe *pr = (void *)(c0 + sizeof *ii);
		pr->data_type = 2 /*WORD*/; pr->addr_type = 2 /*WORD*/; pr->cmd_type = 1 /*PROBE*/;
		pr->reg_addr = 0x300A; pr->expected_data = 0x7750; pr->data_mask = 0; pr->camera_id = s->slot;
		uint32_t len0 = sizeof *ii + sizeof *pr;
		uint8_t *c1 = c0 + 256, *e = put_power(c1, 1, pup, 5); e = put_power(e, 0, pdn, 4);
		uint32_t len1 = (uint32_t)(e - c1);
		uint8_t *pb = pk.map; memset(pb, 0, 4096);
		struct cam_packet *p = (void *)pb; uint32_t pay = offsetof(struct cam_packet, payload);
		struct cam_cmd_buf_desc *cd = (void *)(pb + pay);
		p->header.op_code = 0x0 /* PROBE packet */; p->cmd_buf_offset = 0; p->num_cmd_buf = 2;
		p->io_configs_offset = 2 * sizeof(*cd); p->patch_offset = 2 * sizeof(*cd); p->header.size = pay + 2 * sizeof(*cd);
		cd[0].mem_handle = cb.hdl; cd[0].offset = 0; cd[0].size = 256; cd[0].length = len0; cd[0].type = CAM_CMD_BUF_LEGACY;
		cd[1].mem_handle = cb.hdl; cd[1].offset = 256; cd[1].size = 4096 - 256; cd[1].length = len1; cd[1].type = CAM_CMD_BUF_LEGACY;
		r = cam_ctl_mem(s->fd, CAM_SENSOR_PROBE_CMD, pk.hdl);
		P("sensor slot %d PROBE: %s", s->slot, r ? errname(r) : "ok (kernel stored the power sequence)");
		struct cam_sensor_acquire_dev ad; memset(&ad, 0, sizeof ad);
		ad.session_handle = session; ad.handle_type = CAM_HANDLE_USER_POINTER;
		r = cam_ctl(s->fd, CAM_ACQUIRE_DEV, &ad, sizeof ad);
		if (r) { P("sensor slot %d ACQUIRE_DEV (power up) failed: %s", s->slot, errname(r)); sys_close(s->fd); continue; }
		s->dev = ad.device_handle; s->powered = 1;
		P("sensor slot %d ACQUIRE_DEV ok: powered up (dev %x)", s->slot, (unsigned)s->dev);
		nsens++;
	}
	free_buf(crm, &cb, "sensor commands"); free_buf(crm, &pk, "sensor packet");
	return nsens ? 0 : -1;
}
static void sensors_down(void)
{
	for (int i = 0; i < nsens; i++) {
		struct sensor *s = &sens[i];
		if (s->powered) { struct cam_release_dev_cmd rd = { session, s->dev }; long r = cam_ctl(s->fd, CAM_RELEASE_DEV, &rd, sizeof rd);
			P("sensor slot %d RELEASE_DEV (power down): %s", s->slot, r ? errname(r) : "ok"); }
		sys_close(s->fd);
	}
	nsens = 0;
}

// ---------------------------------------------------------------- 3. SyncBoss camera commands via the head tracker
static int sbcam(const char *lines)
{
	int fd = sys_open("/run/q2headtrack.pid", 0);
	if (fd < 0) { P("head tracker not running (/run/q2headtrack.pid missing): cannot send SyncBoss camera commands"); return -1; }
	sys_close(fd);
	fd = sys_open("/dev/shm/q2ht-sbcam.tmp", 01 | 0100 | 01000);
	if (fd < 0) { P("cannot write /dev/shm/q2ht-sbcam.tmp: %d", fd); return -1; }
	sys_write(fd, lines, slen(lines)); sys_close(fd);
	sc6(38 /*renameat*/, -100, (long)"/dev/shm/q2ht-sbcam.tmp", -100, (long)"/dev/shm/q2ht-sbcam", 0, 0);
	double t0 = now();
	for (int i = 0; i < 160; i++) {            // the tracker polls every ~50 IMU samples (desk-209: once took 2.6 s)
		sleep_ms(25);
		fd = sys_open("/dev/shm/q2ht-sbcam", 0); if (fd < 0) { P("SyncBoss camera commands picked up by the head tracker after %u ms", (unsigned)((now() - t0) * 1000)); return 0; }
		sys_close(fd);
	}
	P("head tracker has not picked up the SyncBoss commands after 4 s (they stay queued)");
	return 0;
}

// ---------------------------------------------------------------- 4. one camera
static long csiphy_open(int want, int *fdo)
{
	char path[32]; struct cam_csiphy_query_cap qc;
	for (int i = 0; i < 40; i++) {
		if (find_node_n("video4linux", "v4l-subdev", "cam-csiphy-driver", path, i)) continue;
		int fd = sys_open(path, 2); if (fd < 0) continue;
		memset(&qc, 0, sizeof qc);
		if (cam_ctl(fd, CAM_QUERY_CAP, &qc, sizeof qc) == 0 && (int)qc.slot_info == want) { *fdo = fd; P("csiphy %d = %s (version %x, clk lane %x)", want, path, qc.version, qc.clk_lane); return 0; }
		sys_close(fd);
	}
	P("csiphy %d not found", want); return -1;
}

static int save_frame(const uint8_t *raw, int cam, const char *suffix)
{
	static uint8_t px[W * H];
	unsigned long sum = 0; unsigned mn = 255, mx = 0, n5a = 0;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			uint8_t v = raw[y * STRIDE + (x / 4) * 5 + (x & 3)];   // MIPI RAW10: 4 pixels' high bytes, then 1 byte of low bits
			px[y * W + x] = v; sum += v; if (v < mn) mn = v; if (v > mx) mx = v; if (v == 0x5a) n5a++;
		}
	P("camera %d %s: mean %u, min %u, max %u, 0x5a (untouched) %u of %u", cam, suffix, (unsigned)(sum / (W * H)), mn, mx, n5a, W * H);
	char fn[64]; char *o = fn; const char *a = "/var/log/q2/camlive_cam"; while (*a) *o++ = *a++;
	*o++ = '0' + cam; *o++ = '_'; a = suffix; while (*a) *o++ = *a++; char *ext = o;
	a = "_pgm.bin"; while (*a) *o++ = *a++; *o = 0;
	int f = sys_open(fn, 01 | 0100 | 01000);
	if (f >= 0) { const char *hdr = "P5\n640 481\n255\n"; sys_write(f, hdr, slen(hdr)); sys_write(f, px, W * H); sys_close(f); }
	o = ext; a = "_raw10.bin"; while (*a) *o++ = *a++; *o = 0;
	f = sys_open(fn, 01 | 0100 | 01000);
	if (f >= 0) { sys_write(f, raw, STRIDE * H); sys_close(f); }
	return n5a == (unsigned)(W * H) ? -1 : 0;
}


// ---------------------------------------------------------------- 4. one camera, Meta's way: cam-isp STREAM MODE
// desk-208 showed the fastpath context only advances to the next buffer on an EPOCH interrupt, and the IFE hw
// manager drops EPOCH for RDI paths -> exactly one frame, then "Active list is empty" at every SOF (60 Hz).
// Meta's HAL (libfastpathcamerahal: "IfeStreamModePacket") instead uses the regular cam-isp node in stream mode:
//   ACQUIRE_DEV + ACQUIRE_HW (same as before), SET_STREAM_MODE with N images (each = its own UPDATE packet that
//   points RDI0 at one buffer), CRM LINK (needed to reach READY), CONFIG_DEV with a blobs-only INIT packet, START.
//   The kernel applies a free image at every SOF (cam_isp_ctx_rdi_stream_*), and STREAM_MODE_CMD GET_IMAGE /
//   RETURN_IMAGE hand finished images to us and back.
#define NIMG 4

static int32_t crm_link(int32_t dev)
{
	static struct cam_req_mgr_link_info li; memset(&li, 0, sizeof li);
	li.session_hdl = session; li.num_devices = 1; li.dev_hdls[0] = dev;
	long r = cam_ctl(crm, CAM_REQ_MGR_LINK, &li, sizeof li);
	if (r) { P("CRM LINK failed: %s", errname(r)); return 0; }
	return li.link_hdl;
}


// desk-210: all four cameras at once (one cam-isp context + CSID/IFE each), ~15 s, and if every camera goes quiet the
// camera start is sent to SyncBoss again (desk-209: cameras stopped ~5 s after the start command).
struct cam {
	struct sensor *s; int settle;
	int phyfd, phystart, hw_acq, started, ok;
	int32_t phydev, dev, link;
	struct buf out[NIMG], cmd, pkt, pcmd;
	int64_t held; int got, firstsaved, midsaved; uint64_t ts0, tsl; int64_t fn0, fnl; double tlast;
	unsigned means[16]; int nmeans; uint64_t ts16[16]; int64_t fn16[16]; int bad;
};
static struct cam *cams; static int ncams;   // in shared memory: one child process per camera fills it in (desk-212)

static int cam_setup(struct cam *c)
{
	struct sensor *s = c->s; int settle = c->settle; long r;
#define phy c->phyfd
#define phydev c->phydev
#define phystart c->phystart
#define dev c->dev
#define hw_acq c->hw_acq
#define link c->link
#define out c->out
#define cmd c->cmd
#define pkt c->pkt
#define pcmd c->pcmd
	phy = -1; c->held = -1;
	P("---- camera slot %d -> csiphy %d (PHY_%d), settle count %d, stream mode with %d buffers", s->slot, s->csiphy, s->csiphy, settle, NIMG);
	{ int32_t m1[1] = { img_mmu };
	  for (int i = 0; i < NIMG; i++) if (alloc_buf(crm, &out[i], OUTSZ, CAM_MEM_FLAG_HW_READ_WRITE | CAM_MEM_FLAG_UMD_ACCESS | CAM_MEM_FLAG_PIXEL_BUF_TYPE, m1, 1, 0)) return -1; }
	{ int32_t m2[2] = { cdm_mmu, img_mmu }; if (alloc_buf(crm, &cmd, 65536, CAM_MEM_FLAG_HW_READ_WRITE | CAM_MEM_FLAG_KMD_ACCESS | CAM_MEM_FLAG_UMD_ACCESS | CAM_MEM_FLAG_CMD_BUF_TYPE, m2, 2, 0)) return -1; }
	if (alloc_buf(crm, &pkt, 16384, CAM_MEM_FLAG_KMD_ACCESS | CAM_MEM_FLAG_UMD_ACCESS | CAM_MEM_FLAG_PACKET_BUF_TYPE, 0, 0, 0)) return -1;
	if (alloc_buf(crm, &pcmd, 4096, CAM_MEM_FLAG_KMD_ACCESS | CAM_MEM_FLAG_UMD_ACCESS | CAM_MEM_FLAG_PACKET_BUF_TYPE, 0, 0, 0)) return -1;
	for (int i = 0; i < NIMG; i++) { if (!out[i].map) { P("output buffer not mapped"); return -1; } memset(out[i].map, 0x5a, OUTSZ); }
	if (!cmd.map || !pkt.map || !pcmd.map) { P("a buffer could not be mapped"); return -1; }
	if (csiphy_open(s->csiphy, &phy)) return -1;
	{
		struct cam_csiphy_acquire_dev_info ai = { 0, 0 };
		struct cam_sensor_acquire_dev ad; memset(&ad, 0, sizeof ad);
		ad.session_handle = session; ad.handle_type = CAM_HANDLE_USER_POINTER; ad.info_handle = (uint64_t)(uintptr_t)&ai;
		r = cam_ctl(phy, CAM_ACQUIRE_DEV, &ad, sizeof ad);
		if (r) { P("csiphy ACQUIRE_DEV failed: %s", errname(r)); return -1; }
		phydev = ad.device_handle;
		struct cam_csiphy_info *ci = pcmd.map; memset(ci, 0, sizeof *ci);
		ci->lane_mask = 0x1; ci->lane_assign = 0x0; ci->lane_cnt = 1;
		ci->settle_time = (uint64_t)settle * 200000000ull; ci->data_rate = 640000000ull;
		uint8_t *pb = pkt.map; memset(pb, 0, 16384);
		struct cam_packet *p = (void *)pb; uint32_t pay = offsetof(struct cam_packet, payload);
		struct cam_cmd_buf_desc *cd = (void *)(pb + pay);
		p->num_cmd_buf = 1; p->io_configs_offset = sizeof(*cd); p->patch_offset = sizeof(*cd); p->header.size = pay + sizeof(*cd);
		cd[0].mem_handle = pcmd.hdl; cd[0].size = 4096; cd[0].length = sizeof *ci; cd[0].type = CAM_CMD_BUF_GENERIC;
		struct cam_config_dev_cmd cf; memset(&cf, 0, sizeof cf);
		cf.session_handle = session; cf.dev_handle = phydev; cf.packet_handle = (uint64_t)(uint32_t)pkt.hdl;
		r = cam_ctl(phy, CAM_CONFIG_DEV, &cf, sizeof cf);
		if (r) { P("csiphy CONFIG_DEV failed: %s", errname(r)); return -1; }
		struct cam_start_stop_dev_cmd st = { session, phydev };
		r = cam_ctl(phy, CAM_START_DEV, &st, sizeof st);
		if (r) { P("csiphy START_DEV failed: %s", errname(r)); return -1; }
		phystart = 1;
	}

	// cam-isp context
	{
		struct cam_acquire_dev_cmd ad; memset(&ad, 0, sizeof ad);
		ad.session_handle = session; ad.handle_type = CAM_HANDLE_USER_POINTER; ad.num_resources = CAM_API_COMPAT_CONSTANT;
		r = cam_ctl(isp, CAM_ACQUIRE_DEV, &ad, sizeof ad);
		if (r) { P("cam-isp ACQUIRE_DEV failed: %s", errname(r)); return -1; }
		dev = ad.dev_handle;
	}
	{
		static uint64_t hwbuf[64];
		struct cam_isp_acquire_hw_info *hi = (void *)hwbuf;
		struct cam_isp_in_port_info_v2 *in = (void *)&hi->data;
		memset(hwbuf, 0, sizeof hwbuf);
		hi->common_info_version = 0x2000; hi->num_inputs = 1; hi->input_info_version = 0x00020000;
		hi->input_info_size = sizeof(*in); hi->input_info_offset = 0;
		in->res_type = CAM_ISP_IFE_IN_RES_PHY_0 + s->csiphy;
		in->lane_type = CAM_ISP_LANE_TYPE_DPHY; in->lane_num = 1; in->lane_cfg = 0x0;
		in->vc[0] = 0; in->dt[0] = 0x2B; in->num_valid_vc_dt = 1;
		in->format = CAM_FORMAT_MIPI_RAW_10;
		in->left_start = 0; in->left_stop = W - 1; in->left_width = W;
		in->line_start = 0; in->line_stop = H - 1; in->height = H;
		in->num_out_res = 1;
		in->data[0].res_type = CAM_ISP_IFE_OUT_RES_RDI_0; in->data[0].format = CAM_FORMAT_MIPI_RAW_10;
		in->data[0].width = W; in->data[0].height = H;
		uint32_t size = (uint32_t)(offsetof(struct cam_isp_acquire_hw_info, data) + sizeof(*in));
		struct cam_acquire_hw_cmd_v2 ah; memset(&ah, 0, sizeof ah);
		ah.struct_version = CAM_ACQUIRE_HW_STRUCT_VERSION_2; ah.session_handle = session; ah.dev_handle = dev;
		ah.handle_type = CAM_HANDLE_USER_POINTER; ah.data_size = size; ah.resource_hdl = (uint64_t)(uintptr_t)hwbuf;
		r = cam_ctl(isp, CAM_ACQUIRE_HW, &ah, sizeof ah);
		if (r) { P("cam-isp ACQUIRE_HW (PHY_%d -> RDI0) failed: %s", s->csiphy, errname(r)); return -1; }
		hw_acq = 1;
		P("cam-isp context: dev %x, ACQUIRE_HW ok (hw id %u)", (unsigned)dev, ah.hw_info.acquired_hw_id[0]);
	}

	// packets: INIT (blobs only) at offset 0, one UPDATE packet per stream image at 2048 * (i + 1)
	{
		uint8_t *pb = pkt.map; memset(pb, 0, 16384);
		uint32_t pay = offsetof(struct cam_packet, payload);
		uint32_t *bp = cmd.map, *b0 = bp;
		struct cam_isp_clock_config ck; memset(&ck, 0, sizeof ck);
		ck.num_rdi = 1; ck.rdi_hz[0] = 400000000ull;
		bp = add_blob(bp, CAM_ISP_GENERIC_BLOB_TYPE_CLOCK_CONFIG, &ck, sizeof ck);
		struct cam_isp_csid_clock_config cc = { 400000000ull };
		bp = add_blob(bp, CAM_ISP_GENERIC_BLOB_TYPE_CSID_CLOCK_CONFIG, &cc, sizeof cc);
		struct cam_isp_bw_config_v2 bw; memset(&bw, 0, sizeof bw);
		bw.num_paths = 1; bw.axi_path[0].usage_data = CAM_ISP_USAGE_RDI; bw.axi_path[0].transac_type = CAM_AXI_TRANSACTION_WRITE;
		bw.axi_path[0].path_data_type = CAM_AXI_PATH_DATA_IFE_RDI0;
		bw.axi_path[0].camnoc_bw = bw.axi_path[0].mnoc_ab_bw = bw.axi_path[0].mnoc_ib_bw = 200000000ull;
		bw.axi_path[0].ddr_ab_bw = bw.axi_path[0].ddr_ib_bw = 200000000ull;
		bp = add_blob(bp, CAM_ISP_GENERIC_BLOB_TYPE_BW_CONFIG_V2, &bw, sizeof bw);
		uint32_t blob_len = (uint32_t)((uint8_t *)bp - (uint8_t *)b0);
		struct cam_packet *p = (void *)pb;
		struct cam_cmd_buf_desc *cd = (void *)(pb + pay);
		p->header.op_code = 0; p->header.request_id = 1;              // INIT
		p->num_cmd_buf = 2; p->io_configs_offset = 2 * sizeof(*cd); p->num_io_configs = 0;
		p->patch_offset = 2 * sizeof(*cd); p->kmd_cmd_buf_index = 1; p->header.size = pay + 2 * sizeof(*cd);
		cd[0].mem_handle = cmd.hdl; cd[0].offset = 0; cd[0].size = 1024; cd[0].length = blob_len;
		cd[0].type = CAM_CMD_BUF_GENERIC; cd[0].meta_data = CAM_ISP_PACKET_META_GENERIC_BLOB_COMMON;
		cd[1].mem_handle = cmd.hdl; cd[1].offset = 1024; cd[1].size = 8192 - 1024;
		cd[1].type = CAM_CMD_BUF_DIRECT; cd[1].meta_data = CAM_ISP_PACKET_META_COMMON;
		for (int i = 0; i < NIMG; i++) {
			uint8_t *q = pb + 2048 * (i + 1);
			struct cam_packet *u = (void *)q;
			struct cam_cmd_buf_desc *ud = (void *)(q + pay);
			struct cam_buf_io_cfg *io = (void *)(q + pay + sizeof(*ud));
			u->header.op_code = 1; u->header.request_id = 2 + i;         // UPDATE
			u->num_cmd_buf = 1; u->io_configs_offset = sizeof(*ud); u->num_io_configs = 1;
			u->patch_offset = sizeof(*ud) + sizeof(*io); u->kmd_cmd_buf_index = 0;
			u->header.size = pay + sizeof(*ud) + sizeof(*io);
			ud->mem_handle = cmd.hdl; ud->offset = 8192 * (i + 1); ud->size = 8192;
			ud->type = CAM_CMD_BUF_DIRECT; ud->meta_data = CAM_ISP_PACKET_META_COMMON;
			io->mem_handle[0] = out[i].hdl; io->planes[0].width = W; io->planes[0].height = H;
			io->planes[0].plane_stride = STRIDE; io->planes[0].slice_height = H;
			io->format = CAM_FORMAT_MIPI_RAW_10; io->resource_type = CAM_ISP_IFE_OUT_RES_RDI_0;
			io->fence = -1; io->early_fence = -1; io->direction = CAM_BUF_OUTPUT;
		}
		static struct cam_set_stream_mode sm; memset(&sm, 0, sizeof sm);
		sm.session_handle = session; sm.dev_handle = dev; sm.num_images = NIMG;
		for (int i = 0; i < NIMG; i++) {
			sm.stream_images[i].mem_handles[0] = (uint32_t)out[i].hdl; sm.stream_images[i].image_id = i;
			sm.stream_images[i].packet_handle = (uint32_t)pkt.hdl; sm.stream_images[i].packet_offset = 2048 * (i + 1);
		}
		r = cam_ctl(isp, CAM_SET_STREAM_MODE, &sm, sizeof sm);
		if (r) { P("SET_STREAM_MODE (%d images) failed: %s", NIMG, errname(r)); return -1; }
		link = crm_link(dev);
		if (!link) return -1;
		struct cam_config_dev_cmd cf; memset(&cf, 0, sizeof cf);
		cf.session_handle = session; cf.dev_handle = dev; cf.offset = 0; cf.packet_handle = (uint64_t)(uint32_t)pkt.hdl;
		r = cam_ctl(isp, CAM_CONFIG_DEV, &cf, sizeof cf);
		if (r) { P("CONFIG_DEV (INIT) failed: %s", errname(r)); return -1; }
		P("stream mode set (%d images), CRM link %x, INIT packet accepted", NIMG, (unsigned)link);
	}

	{
		struct cam_start_stop_dev_cmd st = { session, dev };
		r = cam_ctl(isp, CAM_START_DEV, &st, sizeof st);
		if (r) { P("START_DEV failed: %s", errname(r)); return -1; }
		c->started = 1;
	}
	return 0;
}

static void cam_teardown(struct cam *c)
{
	long r; struct sensor *s = c->s;
	if (c->started) { struct cam_start_stop_dev_cmd st = { session, dev }; r = cam_ctl(isp, CAM_STOP_DEV, &st, sizeof st); if (r) P("slot %d STOP_DEV: %s", s->slot, errname(r)); }
	if (link) { struct cam_req_mgr_unlink_info ul = { session, link }; r = cam_ctl(crm, CAM_REQ_MGR_UNLINK, &ul, sizeof ul); if (r) P("CRM UNLINK: %s", errname(r)); }
	if (hw_acq) { struct cam_release_hw_cmd_v1 rh; memset(&rh, 0, sizeof rh); rh.struct_version = 1; rh.session_handle = session; rh.dev_handle = dev;
		r = cam_ctl(isp, CAM_RELEASE_HW, &rh, sizeof rh); if (r) P("RELEASE_HW: %s", errname(r)); }
	if (dev) { struct cam_release_dev_cmd rd = { session, dev }; r = cam_ctl(isp, CAM_RELEASE_DEV, &rd, sizeof rd); if (r) P("RELEASE_DEV: %s", errname(r)); }
	if (phystart) { struct cam_start_stop_dev_cmd st = { session, phydev }; r = cam_ctl(phy, CAM_STOP_DEV, &st, sizeof st); if (r) P("csiphy STOP_DEV: %s", errname(r)); }
	if (phydev) { struct cam_release_dev_cmd rd = { session, phydev }; r = cam_ctl(phy, CAM_RELEASE_DEV, &rd, sizeof rd); if (r) P("csiphy RELEASE_DEV: %s", errname(r)); }
	if (phy >= 0) sys_close(phy);
	free_buf(crm, &pcmd, 0); free_buf(crm, &pkt, 0); free_buf(crm, &cmd, 0);
	for (int i = NIMG - 1; i >= 0; i--) free_buf(crm, &out[i], 0);
	memset(c, 0, sizeof *c); phy = -1;
}
#undef phy
#undef phydev
#undef phystart
#undef dev
#undef hw_acq
#undef link
#undef out
#undef cmd
#undef pkt
#undef pcmd

static unsigned frame_mean(const uint8_t *raw)
{
	unsigned long sum = 0;
	for (int y = 1; y < H; y += 4) for (int x = 0; x < W; x += 4) sum += raw[y * STRIDE + (x / 4) * 5];
	return (unsigned)(sum / ((H / 4) * (W / 4)));
}

// one non-blocking-ish poll of a camera: returns 1 if an image was taken
#define SEQN 12
// ---- desk-213: q2camd mode ("q2camlive daemon"): runs until stopped and publishes every frame in /dev/shm/q2cam
// Layout (all little-endian), for consumers (tracking, viewers):
//   0x000 header: u32 magic 0x4d433251 ("Q2CM"), u32 version 1, u32 ncams 4, u32 width 640, u32 height 480,
//                 u32 nslots 8, u32 header_size 4096, u32 slot_stride (= 64 + 640*480 rounded up to 4096)
//   0x040 + cam*64: u64 published (frames published so far for camera cell index cam), u32 last_slot, u32 csiphy
//   header_size + (cam*nslots + i)*slot_stride: slot header (64 bytes):
//        u64 seq (odd while being written, even when stable), u64 frame_num, u64 timestamp_ns (frame done, monotonic),
//        u64 sof_ns, u32 mean brightness, u32 bright (1 = longer "scene" exposure, 0 = short controller-LED exposure)
//     then 640*480 bytes, 8-bit (the upper 8 of the sensor's 10 bits; the extra first sensor line is dropped)
// Frames alternate bright/dark (Meta's head-tracking / controller-constellation pattern), 60 fps per camera.
#define SHM_NSLOTS 8
#define SHM_HDR 4096
#define SHM_STRIDE (((64 + W * 480) + 4095) & ~4095)
static int daemon_mode;
static uint8_t *shm;
static void publish(struct cam *c, const uint8_t *raw, uint64_t fn, uint64_t ts, uint64_t sof, unsigned mean, int bright)
{
	int cam = c->s->slot & 3;
	uint8_t *h = shm + 0x40 + cam * 64;
	uint64_t n = *(volatile uint64_t *)h;
	uint32_t si = (uint32_t)(n % SHM_NSLOTS);
	uint8_t *slot = shm + SHM_HDR + (size_t)(cam * SHM_NSLOTS + si) * SHM_STRIDE;
	volatile uint64_t *seqp = (volatile uint64_t *)slot;
	uint64_t sq = *seqp;
	*seqp = sq + 1; __asm__ volatile("dmb ish" ::: "memory");
	((uint64_t *)slot)[1] = fn; ((uint64_t *)slot)[2] = ts; ((uint64_t *)slot)[3] = sof;
	((uint32_t *)slot)[8] = mean; ((uint32_t *)slot)[9] = (uint32_t)bright;
	uint8_t *d = slot + 64;
	for (int y = 0; y < 480; y++) {
		const uint8_t *r = raw + (y + 1) * STRIDE; uint8_t *o = d + y * W;
		for (int x = 0; x < W; x += 4, r += 5) { o[x] = r[0]; o[x + 1] = r[1]; o[x + 2] = r[2]; o[x + 3] = r[3]; }
	}
	__asm__ volatile("dmb ish" ::: "memory"); *seqp = sq + 2;
	*(volatile uint32_t *)(h + 8) = si; *(volatile uint32_t *)(h + 12) = (uint32_t)c->s->csiphy;
	__asm__ volatile("dmb ish" ::: "memory"); *(volatile uint64_t *)h = n + 1;
}
static uint8_t seqbuf[SEQN][W * 480];
static int nseq;

// return the held image (separately: desk-211 lost track of it when a combined RETURN+GET timed out -> torn frame)
static void cam_return(struct cam *c)
{
	static struct cam_stream_mode_cmd sc;
	if (c->held < 0) return;
	memset(&sc, 0, sizeof sc); sc.session_handle = session; sc.dev_handle = c->dev; sc.cmd_mode = CAM_STREAM_MODE_RETURN_IMAGE;
	sc.cmd_return.num_images = 1; sc.cmd_return.image_ids[0] = c->held;
	long r = cam_ctl(isp, CAM_STREAM_MODE_CMD, &sc, sizeof sc);
	if (r) P("slot %d: RETURN_IMAGE %d failed: %s", c->s->slot, (int)c->held, errname(r));
	c->held = -1;
}

static int cam_poll(struct cam *c, unsigned timeout_ms, double tstart)
{
	static struct cam_stream_mode_cmd sc; long r;
	cam_return(c);
	memset(&sc, 0, sizeof sc);
	sc.session_handle = session; sc.dev_handle = c->dev; sc.cmd_mode = CAM_STREAM_MODE_GET_IMAGE;
	sc.cmd_get.timeout_ms = timeout_ms;
	r = cam_ctl(isp, CAM_STREAM_MODE_CMD, &sc, sizeof sc);
	if (r || sc.cmd_get.num_images == 0) { c->bad++; return 0; }
	int id = (int)sc.cmd_get.images[0].image_id;
	if (id < 0 || id >= NIMG) { P("slot %d: bad image id %d", c->s->slot, id); return 0; }
	c->held = id; c->got++; c->tlast = now();
	if (c->got == 1) { c->fn0 = sc.cmd_get.images[0].frame_num; c->ts0 = sc.cmd_get.images[0].timestamp;
		P("slot %d: first image after %u ms (frame %lld)", c->s->slot, (unsigned)((c->tlast - tstart) * 1000), (long long)c->fn0); }
	c->fnl = sc.cmd_get.images[0].frame_num; c->tsl = sc.cmd_get.images[0].timestamp;
	const uint8_t *raw = c->out[id].map;
	if (daemon_mode) {
		unsigned m = frame_mean(raw); int bright = m > c->means[15];   // means[15] = previous frame's mean in daemon mode
		c->means[15] = m;
		publish(c, raw, (uint64_t)c->fnl, c->tsl, sc.cmd_get.images[0].sof_timestamp, m, bright);
		return 1;
	}
	if (c->nmeans < 16) { c->ts16[c->nmeans] = c->tsl; c->fn16[c->nmeans] = c->fnl; c->means[c->nmeans++] = frame_mean(raw); }
	if (!c->firstsaved) { save_frame(raw, c->s->slot, "first"); c->firstsaved = 1; }
	if (!c->midsaved && c->tlast - tstart > 8.0) { save_frame(raw, c->s->slot, "t8s"); c->midsaved = 1; }
	if (c->s->slot == 0 && nseq < SEQN && c->got > 30) {     // 12 consecutive camera-0 frames (after 0.5 s)
		for (int y = 0; y < 480; y++) for (int x = 0; x < W; x++) seqbuf[nseq][y * W + x] = raw[(y + 1) * STRIDE + (x / 4) * 5 + (x & 3)];
		nseq++;
	}
	return 1;
}

static void put_u(char **o, unsigned long long v) { char t[24]; int n = 0; do { t[n++] = '0' + v % 10; v /= 10; } while (v); while (n) *(*o)++ = t[--n]; }

// child process: one camera, blocking GET_IMAGE (desk-211 polled all four from one loop and only got every 5th frame)
static void cam_child(struct cam *c, int secs, double t0)
{
	if (daemon_mode) for (;;) cam_poll(c, 1000, t0);
	while (now() - t0 < secs) cam_poll(c, 300, t0);
	if (c->held >= 0) save_frame(c->out[c->held].map, c->s->slot, "last");
	cam_return(c);
	if (nseq) {
		int f = sys_open("/var/log/q2/camlive_seq_pgm.bin", 01 | 0100 | 01000);
		if (f >= 0) { char hdr[40]; char *o = hdr; const char *a = "P5\n640 "; while (*a) *o++ = *a++; put_u(&o, 480ull * nseq);
			a = "\n255\n"; while (*a) *o++ = *a++;
			sys_write(f, hdr, o - hdr); sys_write(f, seqbuf, (size_t)W * 480 * nseq); sys_close(f); }
	}
	sys_exit(0);
}

static int atoi_s(const char *s) { int neg = 0, v = 0; if (*s == '-') { neg = 1; s++; } while (*s >= '0' && *s <= '9') v = v * 10 + *s++ - '0'; return neg ? -v : v; }

int main(int argc, char **argv)
{
	daemon_mode = argc > 1 && seq(argv[1], "daemon");
	int settle = (argc > 1 && !daemon_mode) ? atoi_s(argv[1]) : 30, secs = argc > 2 ? atoi_s(argv[2]) : 15, mcu = 0, total = 0;
	if (settle < 4 || settle > 120) settle = 30;
	if (secs < 3 || secs > 120) secs = 15;
	logfd = sys_open(daemon_mode ? "/var/log/q2/camd.txt" : LOGF, 01 | 0100 | 01000);
	if (daemon_mode) {
		int sf = sys_open("/dev/shm/q2cam", 02 | 0100);
		size_t sz = SHM_HDR + (size_t)4 * SHM_NSLOTS * SHM_STRIDE;
		if (sf < 0 || sc6(46 /*ftruncate*/, sf, (long)sz, 0, 0, 0, 0) < 0) { P("cannot create /dev/shm/q2cam"); return 1; }
		sc6(52 /*fchmod*/, sf, 0644, 0, 0, 0, 0);
		shm = sys_mmap(sz, 3, 1, sf); sys_close(sf);
		if ((long)shm < 0 && (long)shm > -4096) { P("cannot map /dev/shm/q2cam"); return 1; }
		memset(shm, 0, SHM_HDR);
		uint32_t *hd = (uint32_t *)shm; hd[0] = 0x4d433251; hd[1] = 1; hd[2] = 4; hd[3] = W; hd[4] = 480; hd[5] = SHM_NSLOTS; hd[6] = SHM_HDR; hd[7] = SHM_STRIDE;
		P("q2camd: publishing to /dev/shm/q2cam (%u bytes, %d slots per camera)", (unsigned)sz, SHM_NSLOTS);
	}
	P("Phase B.6d start: all OV7251 cameras at once (cam-isp stream mode), one reader process per camera, %d s, settle %d", secs, settle);
	char path[64];
	// cam-req-mgr allows only ONE open at a time (-EALREADY); desk-210 collided with the B.4 probe -> retry up to 30 s
	if (find_node("video4linux", "video", "cam-req-mgr", path)) memcpy(path, "/dev/video0", 12);
	for (int i = 0; i < 120 && crm < 0; i++) {
		crm = sys_open(path, 2);
		if (crm < 0) { if (i == 0) P("open %s: %d (%s) - another camera program has it, waiting", path, crm, errname(crm)); sleep_ms(250); }
	}
	if (crm < 0) { P("cannot open cam-req-mgr %s: %d", path, crm); goto end; }
	struct cam_req_mgr_session_info si; memset(&si, 0, sizeof si);
	long r = cam_ctl(crm, CAM_REQ_MGR_CREATE_SESSION, &si, sizeof si);
	if (r) { P("CREATE_SESSION failed: %s", errname(r)); goto end; }
	session = si.session_hdl;
	if (find_node("video4linux", "v4l-subdev", "cam-isp", path) || (isp = sys_open(path, 2)) < 0) { P("cam-isp subdev not found"); goto end; }
	{
		struct cam_isp_query_cap_cmd iq; memset(&iq, 0, sizeof iq);
		struct cam_query_cap_cmd qc; memset(&qc, 0, sizeof qc);
		qc.size = sizeof iq; qc.handle_type = CAM_HANDLE_USER_POINTER; qc.caps_handle = (uint64_t)(uintptr_t)&iq;
		r = cam_ctl(isp, CAM_QUERY_CAP, &qc, sizeof qc);
		if (r) { P("ISP QUERY_CAP failed: %s", errname(r)); goto end; }
		img_mmu = iq.device_iommu.non_secure; cdm_mmu = iq.cdm_iommu.non_secure;
		P("ISP: %d IFE devices", iq.num_dev);
	}
	cams = sys_mmap(4 * sizeof(struct cam), 3, 0x21 /*SHARED|ANONYMOUS*/, -1);
	if ((long)cams < 0 && (long)cams > -4096) { P("shared memory failed"); cams = 0; goto end; }
	memset(cams, 0, 4 * sizeof(struct cam));
	if (sensors_up()) { P("no camera sensor could be powered up"); goto end; }
	// pipelines first (all four), then the MCU start, so no frame is missed
	for (int slot = 0; slot < 4; slot++)
		for (int i = 0; i < nsens; i++) {
			if (sens[i].slot != slot) continue;
			struct cam *c = &cams[ncams]; memset(c, 0, sizeof *c); c->phyfd = -1; c->s = &sens[i]; c->settle = settle;
			if (cam_setup(c)) { P("slot %d: pipeline setup failed - skipping this camera", slot); cam_teardown(c); continue; }
			c->ok = 1; ncams++;
		}
	P("%d camera pipelines running", ncams);
	if (!ncams) goto end;
	mcu = sbcam("2d0400\n280000\n030004dd000101\n030004dd010101\n2c0000\n") == 0;
	double t0 = now(); int nchild = 0;
	for (int k = 0; k < ncams; k++) {
		long pid = sc6(220 /*clone*/, 17 /*SIGCHLD*/, 0, 0, 0, 0, 0);
		if (pid == 0) cam_child(&cams[k], secs, t0);
		if (pid > 0) nchild++; else P("fork for slot %d failed: %d", cams[k].s->slot, (int)pid);
	}
	if (daemon_mode) {   // monitor: stats + a 2x2 snapshot of the latest bright frames; runs until systemd stops us
		static uint8_t mos[960 * 1280]; int got0[4] = { 0 }; double tl = now(); int n = 0;
		for (;;) {
			sleep_ms(n < 6 ? 10000 : 60000); n++;
			double t = now(); char b[200]; char *o = b;
			for (int k = 0; k < ncams; k++) { struct cam *c = &cams[k]; put_u(&o, c->s->slot); *o++ = ':'; put_u(&o, (unsigned long long)((c->got - got0[k]) / (t - tl) + 0.5)); *o++ = ' '; got0[k] = c->got; }
			*o = 0; tl = t;
			P("fps per camera (slot:fps): %s", b);
			for (int cam = 0; cam < 4; cam++) {
				uint8_t *h = shm + 0x40 + cam * 64; uint64_t pub = *(volatile uint64_t *)h; if (!pub) continue;
				for (int back = 0; back < SHM_NSLOTS; back++) {        // newest bright frame
					uint8_t *slot = shm + SHM_HDR + (size_t)(cam * SHM_NSLOTS + (uint32_t)((pub - 1 - back) % SHM_NSLOTS)) * SHM_STRIDE;
					if (!((uint32_t *)slot)[9]) continue;
					for (int y = 0; y < 480; y++) memcpy(mos + (size_t)((cam / 2) * 480 + y) * 1280 + (cam & 1) * 640, slot + 64 + y * W, W);
					break;
				}
			}
			int f = sys_open("/var/log/q2/camd_snapshot_pgm.bin", 01 | 0100 | 01000);
			if (f >= 0) { const char *hh = "P5\n1280 960\n255\n"; sys_write(f, hh, slen(hh)); sys_write(f, mos, sizeof mos); sys_close(f); }
		}
	}
	for (int k = 0; k < nchild; k++) { int st; sc6(260 /*wait4*/, -1, (long)&st, 0, 0, 0, 0); }
	for (int k = 0; k < ncams; k++) {
		struct cam *c = &cams[k];
		if (c->got > 1 && c->tsl > c->ts0)
			P("slot %d: %d images, frames %lld..%lld (%lld frames, %d not delivered), %u.%u fps, %d empty waits", c->s->slot, c->got, (long long)c->fn0, (long long)c->fnl,
			  (long long)(c->fnl - c->fn0 + 1), (int)(c->fnl - c->fn0 + 1 - c->got),
			  (unsigned)((c->got - 1) * 1000000000ull / (c->tsl - c->ts0)), (unsigned)((c->got - 1) * 10000000000ull / (c->tsl - c->ts0) % 10), c->bad);
		else P("slot %d: %d images", c->s->slot, c->got);
		if (c->nmeans) {
			char b[400]; char *o = b; for (int i = 0; i < c->nmeans; i++) { put_u(&o, c->means[i]); *o++ = ' '; } *o = 0;
			P("slot %d: brightness of the first %d frames: %s", c->s->slot, c->nmeans, b);
			o = b; for (int i = 0; i < c->nmeans; i++) { put_u(&o, (unsigned long long)c->fn16[i]); *o++ = '@'; put_u(&o, (c->ts16[i] / 1000ull) % 100000000ull); *o++ = ' '; } *o = 0;
			P("slot %d: frame@timestamp(us): %s", c->s->slot, b);
		}
		total += c->got;
	}
end:
	for (int k = 0; cams && k < ncams; k++) cam_teardown(&cams[k]);
	if (mcu) sbcam("2d0400\n");
	if (nsens) sensors_down();
	if (session) { struct cam_req_mgr_session_info s2 = { session, 0 }; r = cam_ctl(crm, CAM_REQ_MGR_DESTROY_SESSION, &s2, sizeof s2); P("DESTROY_SESSION: %s", r ? errname(r) : "ok"); }
	if (isp >= 0) sys_close(isp);
	if (crm >= 0) sys_close(crm);
	P("Phase B.6d end: %d images in total", total);
	if (logfd >= 0) sys_close(logfd);
	return total ? 0 : 1;
}

__attribute__((naked, noreturn)) void _start(void)
{
	__asm__ volatile("ldr x0, [sp]\n" "add x1, sp, #8\n" "and sp, x1, #~15\n" "bl c_start\n");
}
__attribute__((used)) void c_start(long argc, char **argv) { sys_exit(main((int)argc, argv)); }
