#!/usr/bin/env python3
# q2islandmount (desk-226) - attach this headset's own Meta partitions READ-ONLY for the island test.
#   up <dir>  : system_a, system_ext_a, vendor_a, product_a (+ persist) -> <dir>/<name>, ro (loop -r / dm --readonly,
#               mount ro,noload); the loop/dm devices are listed in /run/q2island.loops
#   down      : detaches the devices listed there (the mounts themselves live in the test's private mount namespace)
# Nothing is ever written to a stock partition.
import os, struct, subprocess, sys, tarfile, time


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
    mp = os.path.join(BASE, name); os.makedirs(mp, exist_ok=True)
    for opts in ("ro,noload", "ro"):
        for fs in ("ext4", "erofs", "auto"):
            rc, out = sh("mount -t %s -o %s %s %s" % (fs, opts, blk, mp))
            if rc == 0: mounted.append(mp); L(name, ": mounted read-only (%s) at %s" % (fs, mp)); return mp
    L(name, ": could not mount:", out); return None


BASE = "/run/q2island/parts"
def up(base):
    global BASE; BASE = base; os.makedirs(base, exist_ok=True)
    sup = find_part(["super"])
    if not sup: L("no super partition"); return 1
    parts = parse_lp(sup)
    for name in ("system_a", "system_ext_a", "vendor_a", "product_a"):
        if name in parts:
            blk = attach(sup, name, parts[name])
            if blk: mount_ro(blk, name)
    per = find_part(["persist"])
    if per:
        rc, out = sh("losetup -r -f --show %s" % per)
        if rc == 0: loops.append(("loop", out)); mount_ro(out, "persist")
    with open("/run/q2island.loops", "w") as f:
        for kind, x in loops: f.write("%s %s\n" % (kind, x))
    ok = all(os.path.isdir(os.path.join(base, n)) and os.listdir(os.path.join(base, n)) for n in ("system_a", "vendor_a"))
    L("mounted:", " ".join(mounted)); return 0 if ok else 1
def down():
    try: lines = open("/run/q2island.loops").read().split("\n")
    except OSError: return 0
    for l in reversed([x for x in lines if x]):
        kind, x = l.split(" ", 1)
        rc, out = sh("dmsetup remove %s" % x if kind == "dm" else "losetup -d %s" % x)
        L("released", x, "ok" if rc == 0 else out)
    os.remove("/run/q2island.loops"); return 0
if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "up": sys.exit(up(sys.argv[2]))
    if len(sys.argv) > 1 and sys.argv[1] == "down": sys.exit(down())
    print("usage: q2islandmount up <dir> | down"); sys.exit(2)
