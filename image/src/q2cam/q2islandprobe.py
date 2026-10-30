#!/usr/bin/env python3
# q2islandprobe v2 (desk-225; v1 was desk-224) - READ-ONLY survey of this headset's own Meta (Horizon OS) partitions, to plan running
# Meta's own head/controller tracking (trackingservice + sensors HAL) next to SteamOS.  Nothing is started or executed:
# it only mounts system/system_ext/vendor read-only, reads versions, lists the tracking pieces, copies the small init
# scripts and the tracking binaries into /var/log/q2/island_files.bin (a .tar.gz) and checks kernel features.
# Derived from q2stockcam (same LP parser, -r loop devices, ro,noload mounts, full cleanup).
#
# The kernel only carries the camera *plumbing*; the sensor model, power-up sequence and register tables live in
# Meta's userspace (vendor partition) and the per-device calibration in /persist. This tool:
#   1. finds the stock "super" partition, parses its Android dynamic-partition (LP) metadata
#   2. attaches vendor/odm/product (slot a and b) as READ-ONLY loop devices (no copy) and mounts them -o ro
#   3. mounts /persist -o ro
#   4. lists every camera / sensor / tracking / syncboss related file, and copies the small ones
#      (sensor modules, camera configs, calibration JSON, the camera HAL libs) into /var/log/q2/stockcam/
#   5. packs them as /var/log/q2/stockcam_files.bin (a .tar.gz - rename to view) + /var/log/q2/stockcam.txt
#   6. unmounts and detaches everything again
# Nothing is ever written to any stock partition: loop devices are created with -r, mounts are ro(,noload).
import os, struct, subprocess, sys, tarfile, time

OUT = "/var/log/q2/island2"; REP = "/var/log/q2/island-probe2.txt"; TARF = "/var/log/q2/island2_files.bin"
# desk-208: "--only NAME[,NAME...]" copies just the files whose name contains one of NAMEs (e.g. the 4.4 MB camera HAL
# that did not fit last time) into stockcam2.txt / stockcam2_files.bin
ONLY = None
if len(sys.argv) > 2 and sys.argv[1] == "--only":
    ONLY = [x for x in sys.argv[2].split(",") if x]
    OUT = "/var/log/q2/stockcam2"; REP = "/var/log/q2/stockcam2.txt"; TARF = "/var/log/q2/stockcam2_files.bin"
log_lines = []
def L(*a):
    s = " ".join(str(x) for x in a); print(s); log_lines.append(s)
def sh(cmd):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr).strip()

def find_part(names):
    d = "/dev/disk/by-partlabel"
    for n in names:
        p = os.path.join(d, n)
        if os.path.exists(p): return os.path.realpath(p)
    return None

def parse_lp(dev):
    f = open(dev, "rb")
    f.seek(4096); geo = f.read(4096)
    magic, = struct.unpack_from("<I", geo, 0)
    if magic != 0x616C4467: raise Exception("no LP geometry magic (got %#x)" % magic)
    struct_size, = struct.unpack_from("<I", geo, 4)
    meta_max, slots, lbs = struct.unpack_from("<III", geo, 4 + 4 + 32)
    f.seek(4096 + 4096 * 2)                 # primary metadata, slot 0
    hdr = f.read(256)
    hmagic, major, minor, hsize = struct.unpack_from("<IHHI", hdr, 0)
    if hmagic != 0x414C5030: raise Exception("no LP metadata magic (got %#x)" % hmagic)
    o = 4 + 2 + 2 + 4 + 32
    tables_size, = struct.unpack_from("<I", hdr, o); o += 4 + 32
    desc = []
    for i in range(4):
        desc.append(struct.unpack_from("<III", hdr, o)); o += 12
    f.seek(4096 + 4096 * 2 + hsize); tables = f.read(tables_size)
    (po, pn, ps), (eo, en, es), (go, gn, gs), (bo, bn, bs) = desc
    exts = [struct.unpack_from("<QIQI", tables, eo + i * es) for i in range(en)]
    parts = {}
    for i in range(pn):
        b = tables[po + i * ps: po + (i + 1) * ps]
        name = b[:36].split(b"\0")[0].decode()
        attrs, first, num, grp = struct.unpack_from("<IIII", b, 36)
        parts[name] = [exts[first + k] for k in range(num)]
    L("LP metadata v%d.%d, %d partitions: %s" % (major, minor, pn, " ".join(sorted(parts))))
    return parts

mounted = []; loops = []
def attach(dev, name, extents):
    # only linear extents on this device (target_type 0 = LINEAR, target_source 0 = super itself)
    if not extents: L(name, ": empty"); return None
    if len(extents) != 1:
        # several extents: use device-mapper (read-only table) if dmsetup exists
        rc, _ = sh("command -v dmsetup")
        if rc: L(name, ": %d extents and no dmsetup - skipped" % len(extents)); return None
        table, start = [], 0
        for (nsec, ttype, tdata, tsrc) in extents:
            if ttype != 0 or tsrc != 0: L(name, ": non-linear extent - skipped"); return None
            table.append("%d %d linear %s %d" % (start, nsec, dev, tdata)); start += nsec
        dmn = "q2ro_" + name
        rc, out = sh("printf '%s\\n' | dmsetup create --readonly %s" % ("\\n".join(table), dmn))
        if rc: L(name, ": dmsetup failed:", out); return None
        loops.append(("dm", dmn)); return "/dev/mapper/" + dmn
    nsec, ttype, tdata, tsrc = extents[0]
    if ttype != 0 or tsrc != 0: L(name, ": not a plain extent - skipped"); return None
    rc, out = sh("losetup -r -f --show -o %d --sizelimit %d %s" % (tdata * 512, nsec * 512, dev))
    if rc: L(name, ": losetup failed:", out); return None
    loops.append(("loop", out)); return out

def mount_ro(blk, name):
    mp = "/run/q2island/" + name; os.makedirs(mp, exist_ok=True)
    for opts in ("ro,noload", "ro"):
        for fs in ("ext4", "erofs", "auto"):
            rc, out = sh("mount -t %s -o %s %s %s" % (fs, opts, blk, mp))
            if rc == 0: mounted.append(mp); L(name, ": mounted read-only (%s) at %s" % (fs, mp)); return mp
    L(name, ": could not mount:", out); return None


NAMES = ("trackingservice", "trackingclient", "trackinghost", "trackingserviceclients", "trackingengines", "libtrackingvendorutils",
         "memorybroker", "servicemanager", "hwservicemanager", "vndservicemanager", "cdsprpcd", "adsprpcd", "linker64",
         "libbinder_ndk", "sensors@1.0-service", "libsyncboss", "syncboss", "ossdk", "vrdriver", "vrruntime", "headtracking",
         "constellation", "libvrsensors", "libovr", "insideout", "iot")
RCKEYS = (b"sensors", b"tracking", b"syncboss", b"swd", b"fastrpc", b"cdsp", b"adsp", b"camera", b"firmware", b"oculus")
def props(path):
    out = {}
    try:
        for l in open(path, errors="replace"):
            if "=" in l and not l.startswith("#"):
                k, v = l.strip().split("=", 1)
                if k.endswith(("fingerprint", "build.id", "version.incremental", "build.date", "release", "security_patch", "product.device")): out[k] = v
    except OSError: pass
    return out

def needed(path):
    # DT_NEEDED names of a 64-bit little-endian ELF (no external tools needed)
    try: d = open(path, "rb").read()
    except OSError: return []
    if d[:4] != b"\x7fELF" or d[4] != 2: return []
    phoff, = struct.unpack_from("<Q", d, 0x20); phes, phn = struct.unpack_from("<HH", d, 0x36)
    loads, dyn = [], None
    for i in range(phn):
        t, fl, off, va, pa, fs, ms, al = struct.unpack_from("<IIQQQQQQ", d, phoff + i * phes)
        if t == 1: loads.append((va, off, fs))
        if t == 2: dyn = (off, fs)
    if not dyn: return []
    def v2o(v):
        for va, off, fs in loads:
            if va <= v < va + fs: return v - va + off
        return None
    ents, strtab = [], None
    for k in range(dyn[1] // 16):
        tag, val = struct.unpack_from("<qQ", d, dyn[0] + 16 * k)
        if tag == 0: break
        ents.append((tag, val))
        if tag == 5: strtab = v2o(val)
    if strtab is None: return []
    return [d[strtab + v: d.index(b"\0", strtab + v)].decode() for t, v in ents if t == 1]

def closure(roots, libdirs):
    seen, order, missing = {}, [], []
    todo = list(roots)
    while todo:
        f = todo.pop(0)
        for n in needed(f):
            if n in seen or n in missing: continue
            hit = next((os.path.join(d, n) for d in libdirs if os.path.exists(os.path.join(d, n))), None)
            if hit: seen[n] = hit; order.append(n); todo.append(hit)
            else: missing.append(n)
    return seen, order, missing

def phase2():
    R = "/run/q2island"
    libdirs = [R + "/system_a/system/lib64", R + "/system_ext_a/lib64", R + "/vendor_a/lib64", R + "/vendor_a/odm/lib64",
               R + "/system_a/system/apex/com.android.runtime/lib64/bionic", R + "/system_a/system/lib64/bootstrap"]
    total = 0
    BIG = ("libtrackingengines.so",)
    for root in (R + "/system_a/system/bin/trackingservice", R + "/vendor_a/odm/bin/hw/vendor.oculus.hardware.sensors@1.0-service"):
        seen, order, missing = closure([root], libdirs)
        L("== libraries needed by %s: %d found, missing: %s" % (os.path.relpath(root, R), len(order), " ".join(missing) or "none"))
        for n in order:
            full = seen[n]; sz = os.path.getsize(full); rel = os.path.relpath(full, R)
            L("  %9d %s" % (sz, rel))
            if n in BIG or sz > 4 * 1024 * 1024 or total + sz > 28 * 1024 * 1024: continue
            if n in ("libc.so", "libm.so", "libdl.so", "libc++.so", "liblog.so", "libutils.so", "libcutils.so", "libbase.so"): continue
            dst = os.path.join(OUT, rel)
            if os.path.exists(dst): continue
            os.makedirs(os.path.dirname(dst), exist_ok=True); open(dst, "wb").write(open(full, "rb").read()); total += sz
    # the SyncBoss firmware updater (fw_init) and anything that mentions it: listed, its scripts copied, never run
    for mp in mounted:
        for root, dirs, files in os.walk(mp):
            for fn in files:
                full = os.path.join(root, fn)
                if os.path.islink(full): continue
                if "fw_init" in fn or fn in ("libsyncboss.so",):
                    rel = os.path.relpath(full, R); sz = os.path.getsize(full)
                    L("  fw/syncboss piece: %9d %s" % (sz, rel))
                    if sz < 4 * 1024 * 1024:
                        dst = os.path.join(OUT, rel); os.makedirs(os.path.dirname(dst), exist_ok=True); open(dst, "wb").write(open(full, "rb").read())
                elif fn.endswith(".rc"):
                    try: data = open(full, "rb").read()
                    except OSError: continue
                    if b"fw_init" in data:
                        dst = os.path.join(OUT, os.path.relpath(full, R)); os.makedirs(os.path.dirname(dst), exist_ok=True); open(dst, "wb").write(data)
                        L("  rc mentioning fw_init:", os.path.relpath(full, R))
    L("phase 2 copied %d KB" % (total // 1024))

def main():
    if os.geteuid() != 0: print("run as root (sudo)"); sys.exit(1)
    os.makedirs(OUT, exist_ok=True)
    L("q2islandprobe", time.strftime("%F %T"), "- read-only; nothing from the stock partitions is executed")
    L("kernel:", sh("uname -a")[1])
    L("== kernel features")
    fs = open("/proc/filesystems").read()
    for k in ("binder", "erofs", "ext4", "f2fs"): L("  filesystem %-8s %s" % (k, "yes" if k in fs else "NO"))
    for d in ("/dev/binder", "/dev/hwbinder", "/dev/vndbinder", "/dev/binderfs", "/dev/ashmem", "/dev/ion", "/dev/dma_heap",
              "/dev/adsprpc-smd", "/dev/adsprpc-smd-secure", "/dev/syncboss0", "/dev/syncboss_control0", "/dev/syncboss_stream0",
              "/dev/syncboss_powerstate0", "/dev/video0", "/dev/video1", "/dev/v4l-subdev0", "/sys/fs/selinux/enforce"):
        L("  %-28s %s" % (d, "present" if os.path.exists(d) else "-"))
    if os.path.isdir("/dev/dma_heap"): L("  dma heaps:", " ".join(sorted(os.listdir("/dev/dma_heap"))))
    rc, out = sh("zcat /proc/config.gz 2>/dev/null | grep -E 'CONFIG_(ANDROID_BINDER|ANDROID_BINDERFS|ASHMEM|ION|DMABUF_HEAPS|MSM_ADSPRPC|SECURITY_SELINUX|EROFS_FS|MEMFD)'")
    L("  config:", out.replace("\n", " ") if out else "(no /proc/config.gz)")
    os.makedirs("/run/q2island/binderfs", exist_ok=True)
    rc, out = sh("mount -t binder binder /run/q2island/binderfs")
    if rc == 0:
        L("  binderfs: mountable; nodes:", " ".join(sorted(os.listdir("/run/q2island/binderfs")))); sh("umount /run/q2island/binderfs")
    else: L("  binderfs: mount failed:", out)
    L("  who uses the tracking hardware now:", sh("fuser -v /dev/syncboss* /dev/video* 2>&1 | tr -s ' ' | head -20")[1].replace("\n", " | "))
    L("== partitions")
    sup = find_part(["super"])
    if not sup: L("no 'super' partition; partlabels:", " ".join(sorted(os.listdir("/dev/disk/by-partlabel")))); return
    L("super =", sup)
    parts = parse_lp(sup)
    for name in sorted(parts):
        base = name[:-2] if name[-2:] in ("_a", "_b") else name
        if base not in ("system", "system_ext", "vendor", "odm", "product"): continue
        blk = attach(sup, name, parts[name])
        if blk: mount_ro(blk, name)
    L("== versions")
    for mp in mounted:
        for bp in ("build.prop", "system/build.prop", "etc/build.prop", "odm/etc/build.prop", "odm/build.prop"):
            p = props(os.path.join(mp, bp))
            for k in sorted(p): L("  %s/%s: %s=%s" % (os.path.basename(mp), bp, k, p[k]))
    L("== tracking pieces (size, sha256-16, path)")
    import hashlib
    cands, rcs = [], []
    for mp in mounted:
        for root, dirs, files in os.walk(mp):
            for fn in files:
                full = os.path.join(root, fn); rel = os.path.relpath(full, "/run/q2island")
                if os.path.islink(full): continue
                low = fn.lower()
                if low.endswith(".rc"):
                    try:
                        data = open(full, "rb").read()
                        if any(k in data.lower() for k in RCKEYS): rcs.append((full, rel))
                    except OSError: pass
                    continue
                if any(k in low for k in NAMES):
                    try:
                        sz = os.path.getsize(full); h = hashlib.sha256(open(full, "rb").read()).hexdigest()[:16]
                    except OSError: continue
                    L("  %9d %s %s" % (sz, h, rel)); cands.append((sz, full, rel, low))
    L("== init scripts copied: %d" % len(rcs))
    for full, rel in rcs:
        dst = os.path.join(OUT, rel); os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, "wb").write(open(full, "rb").read())
    # copy the tracking service binaries/libraries themselves (for interface comparison with the stock test headset)
    total = 0
    pri = lambda c: (0 if "tracking" in c[3] or "memorybroker" in c[3] else 1, c[0])
    for sz, full, rel, low in sorted(cands, key=pri):
        if "tracking" not in low and "memorybroker" not in low and "sensors@1.0-service" not in low: continue
        if sz > 12 * 1024 * 1024 or total + sz > 20 * 1024 * 1024: L("  (not copied, size)", rel); continue
        dst = os.path.join(OUT, rel); os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, "wb").write(open(full, "rb").read()); total += sz
    L("copied %d KB of tracking binaries" % (total // 1024))
    phase2()
    with open(REP, "w") as f: f.write("\n".join(log_lines) + "\n")
    with tarfile.open(TARF, "w:gz") as t: t.add(OUT, arcname="island")
    L("report:", REP, " files:", TARF, "(a .tar.gz)")

try:
    main()
except Exception as e:
    L("probe error:", e)
finally:
    for mp in reversed(mounted): sh("umount %s" % mp)
    for kind, x in reversed(loops):
        sh("dmsetup remove %s" % x if kind == "dm" else "losetup -d %s" % x)
    with open(REP, "w") as f: f.write("\n".join(log_lines) + "\ncleanup done: %d mounts, %d loop/dm devices released\n" % (len(mounted), len(loops)))
