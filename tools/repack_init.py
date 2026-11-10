#!/usr/bin/env python3
"""repack_init.py - put a new /init into an existing boot image (header v2) without touching anything else.

    python3 tools/repack_init.py --in boot_steamos.img --init image/src/init/init --out out/boot_steamos.img

Keeps the kernel, device tree, kernel command line and every other file in the ramdisk byte for byte; only
/init, the ramdisk size and the header checksum change.  Refuses an image bigger than the 80 MiB boot partition.
Takes a few seconds (the ramdisk is recompressed with gzip -9).
"""
import argparse, gzip, hashlib, io, struct, sys

LIMIT = 80 * 1024 * 1024

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="src", required=True)
    ap.add_argument("--init", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--put", action="append", default=[], metavar="RAMDISK_PATH=LOCAL_FILE",
                    help="also replace this ramdisk file (e.g. usr/share/q2vr/driver_q2hmd.so=image/prebuilt/driver_q2hmd.so)")
    a = ap.parse_args()

    d = open(a.src, "rb").read()
    if d[:8] != b"ANDROID!":
        sys.exit("not an Android boot image")
    ks, _, rs, _, ss = struct.unpack_from("<IIIII", d, 8)
    ps, ver = struct.unpack_from("<II", d, 36)
    if ver != 2:
        sys.exit(f"boot header v{ver} not supported (need v2)")
    dtbo_sz, dtbo_off, _, dtb_sz = struct.unpack_from("<IQII", d, 1632)
    al = lambda n: (n + ps - 1) // ps * ps
    ro = ps + al(ks); so = ro + al(rs); do_ = so + al(ss); dto = do_ + al(dtbo_sz)
    if dtbo_sz and dtbo_off != do_:
        sys.exit("unexpected recovery_dtbo offset")
    kern, rd = d[ps:ps + ks], d[ro:ro + rs]
    sec, dtbo, dtb = d[so:so + ss], d[do_:do_ + dtbo_sz], d[dto:dto + dtb_sz]
    tail = d[dto + al(dtb_sz):]

    cpio = gzip.decompress(rd)
    new = open(a.init, "rb").read()
    puts = {}
    for pv in a.put:
        k, _, v = pv.partition("=")
        puts[k.lstrip("/")] = open(v, "rb").read()
    done = set()
    out, o, found = bytearray(), 0, False
    while True:                                   # newc cpio: walk the entries, swap only "init"
        if cpio[o:o + 6] != b"070701":
            sys.exit("ramdisk is not a newc cpio archive")
        fs = int(cpio[o + 54:o + 62], 16); nsz = int(cpio[o + 94:o + 102], 16)
        name = cpio[o + 110:o + 110 + nsz - 1].decode()
        hdr = bytearray(cpio[o:o + 110 + nsz])
        ds = (o + 110 + nsz + 3) & ~3
        pad = cpio[o + 110 + nsz:ds]
        data = cpio[ds:ds + fs]
        if name == "init" and not found:
            data, found = new, True
            hdr[54:62] = b"%08X" % len(new)
        elif name in puts and name not in done:
            data = puts[name]; done.add(name)
            hdr[54:62] = b"%08X" % len(data)
        out += hdr + pad + data + b"\0" * (((len(data) + 3) & ~3) - len(data))
        o = (ds + fs + 3) & ~3
        if name == "TRAILER!!!":
            break
    if not found:
        sys.exit("no /init in the ramdisk")
    if set(puts) - done:
        sys.exit("not in the ramdisk: " + ", ".join(sorted(set(puts) - done)))
    out += cpio[o:]

    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", compresslevel=9, mtime=0) as g:
        g.write(bytes(out))
    nrd = buf.getvalue()

    h = bytearray(d[:ps])
    struct.pack_into("<I", h, 16, len(nrd))
    if dtbo_sz:
        struct.pack_into("<Q", h, 1636, ro + al(len(nrd)) + al(ss))
    sh = hashlib.sha1()
    for blob in (kern, nrd, sec):
        sh.update(blob); sh.update(struct.pack("<I", len(blob)))
    if dtbo_sz: sh.update(dtbo); sh.update(struct.pack("<I", dtbo_sz))
    if dtb_sz:  sh.update(dtb);  sh.update(struct.pack("<I", dtb_sz))
    h[576:608] = sh.digest().ljust(32, b"\0")

    pad = lambda b: bytes(b) + b"\0" * (al(len(b)) - len(b))
    res = bytes(h) + pad(kern) + pad(nrd) + pad(sec) + pad(dtbo) + pad(dtb) + tail
    if len(res) > LIMIT:
        sys.exit(f"image is {len(res)} bytes, the boot partition is {LIMIT}")
    open(a.out, "wb").write(res)
    print(f"{a.out}: {len(res)} bytes, ramdisk {rs} -> {len(nrd)}, sha256 {hashlib.sha256(res).hexdigest()}")

main()
