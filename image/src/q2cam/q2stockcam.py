#!/usr/bin/env python3
# q2stockcam - READ-ONLY look into the Quest 2's stock (Meta) partitions for the tracking-camera files.
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

OUT = "/var/log/q2/stockcam"; REP = "/var/log/q2/stockcam.txt"; TARF = "/var/log/q2/stockcam_files.bin"
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
    mp = "/run/q2stock/" + name; os.makedirs(mp, exist_ok=True)
    for opts in ("ro,noload", "ro"):
        for fs in ("ext4", "erofs", "auto"):
            rc, out = sh("mount -t %s -o %s %s %s" % (fs, opts, blk, mp))
            if rc == 0: mounted.append(mp); L(name, ": mounted read-only (%s) at %s" % (fs, mp)); return mp
    L(name, ": could not mount:", out); return None

KEYS = ("camera", "sensor", "cam", "tracking", "syncboss", "ossdk", "calib", "monterey", "hollywood", "ov7251",
        "ov9282", "og01", "imx", "tof", "chi", "isp", "ife")
def interesting(path):
    if ONLY is not None: return any(x in os.path.basename(path) for x in ONLY)
    p = path.lower()
    return any(k in p for k in KEYS)

def main():
    if os.geteuid() != 0: print("run as root (sudo)"); sys.exit(1)
    os.makedirs(OUT, exist_ok=True)
    L("q2stockcam", time.strftime("%F %T"), "- read-only")
    sup = find_part(["super"])
    if not sup: L("no 'super' partition label found; partlabels:", " ".join(sorted(os.listdir("/dev/disk/by-partlabel"))))
    else:
        L("super =", sup)
        try:
            parts = parse_lp(sup)
            for name in sorted(parts):
                base = name[:-2] if name[-2:] in ("_a", "_b") else name
                if base not in ("vendor", "odm", "product", "system_ext", "vendor_dlkm", "odm_dlkm"): continue
                blk = attach(sup, name, parts[name])
                if blk: mount_ro(blk, name)
        except Exception as e:
            L("LP parse failed:", e)
    per = find_part(["persist"])
    if per:
        blk_rc, out = sh("losetup -r -f --show %s" % per)
        if blk_rc == 0: loops.append(("loop", out)); mount_ro(out, "persist")
    # inventory + copy
    listing, cands, copied, total = [], [], 0, 0
    for mp in mounted:
        for root, dirs, files in os.walk(mp):
            for fn in files:
                full = os.path.join(root, fn)
                if not interesting(full) or os.path.islink(full): continue
                try: sz = os.path.getsize(full)
                except OSError: continue
                rel = os.path.relpath(full, "/run/q2stock")
                listing.append("%10d  %s" % (sz, rel)); cands.append((sz, full, rel))
    # the log USB drive is 64 MB: copy the smallest files first (configs, sensor modules, calibration), up to 40 MB
    for sz, full, rel in sorted(cands):
        if sz > 6 * 1024 * 1024 or total + sz > 40 * 1024 * 1024: continue
        dst = os.path.join(OUT, rel); os.makedirs(os.path.dirname(dst), exist_ok=True)
        try:
            with open(full, "rb") as a, open(dst, "wb") as b: b.write(a.read())
            copied += 1; total += sz
        except OSError as e: listing.append("            (copy failed %s: %s)" % (rel, e))
    L("camera-related files found: %d, copied: %d (%d KB)" % (len(listing), copied, total // 1024))
    with open(REP, "w") as f:
        f.write("\n".join(log_lines) + "\n\n== files (size, path)\n" + "\n".join(sorted(listing, key=lambda s: s[12:])) + "\n")
    with tarfile.open(TARF, "w:gz") as t: t.add(OUT, arcname="stockcam")
    L("report:", REP, " files:", TARF, "(a .tar.gz)")

try:
    main()
finally:
    for mp in reversed(mounted): sh("umount %s" % mp)
    for kind, x in reversed(loops):
        sh("dmsetup remove %s" % x if kind == "dm" else "losetup -d %s" % x)
    with open(REP, "a") as f: f.write("\ncleanup done: %d mounts, %d loop/dm devices released\n" % (len(mounted), len(loops)))
