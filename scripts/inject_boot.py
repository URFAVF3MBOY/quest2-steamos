#!/usr/bin/env python3
"""Append files to the ramdisk of an existing header-v2 Android boot image.

The files are packed as an extra gzip'd cpio (newc) archive and concatenated
after the existing ramdisk; the kernel unpacks concatenated archives into the
same rootfs, so nothing in the original ramdisk has to be touched.

usage: inject_boot.py --in boot_ramonly.img --out boot_new.img \
          --file build/qscreen:usr/bin/qscreen:755 --file extra/screen-test:usr/bin/screen-test:755
"""
import argparse, gzip, hashlib, struct, sys, time

ap = argparse.ArgumentParser()
ap.add_argument("--in", dest="inp", required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--file", action="append", default=[], help="src:dest[:octalmode]")
ap.add_argument("--cmdline-append", default="")
a = ap.parse_args()

img = open(a.inp, "rb").read()
assert img[:8] == b"ANDROID!", "not an Android boot image"
page = struct.unpack_from("<I", img, 36)[0]
ver = struct.unpack_from("<I", img, 40)[0]
assert ver == 2, f"expected header v2, got v{ver}"
ksz, rsz, ssz = struct.unpack_from("<III", img, 8)[0], struct.unpack_from("<I", img, 16)[0], struct.unpack_from("<I", img, 24)[0]
dsz = struct.unpack_from("<I", img, 1648)[0]
assert ssz == 0 and struct.unpack_from("<I", img, 1632)[0] == 0
up = lambda n: (n + page - 1) // page * page
koff = page
roff = koff + up(ksz)
doff = roff + up(rsz)
kernel, ramdisk, dtb = img[koff:koff + ksz], img[roff:roff + rsz], img[doff:doff + dsz]
assert len(dtb) == dsz and len(ramdisk) == rsz, "image truncated?"
assert ramdisk[:2] == b"\x1f\x8b", "existing ramdisk is not gzip"


def newc(name, data, mode, ino):
    nm = name.encode() + b"\0"
    hdr = "070701" + "".join("%08X" % v for v in (
        ino, mode, 0, 0, 1, int(time.time()), len(data), 0, 0, 0, 0, len(nm), 0)).encode().decode()
    out = hdr.encode() + nm
    out += b"\0" * (-len(out) % 4)
    out += data
    out += b"\0" * (-len(data) % 4)
    return out


extra = b""
ino = 0x7000
dirs = set()
for spec in a.file:
    parts = spec.split(":")
    src, dest = parts[0], parts[1]
    mode = int(parts[2], 8) if len(parts) > 2 else 0o755
    d = dest.rsplit("/", 1)[0] if "/" in dest else ""
    # directory entries for every parent (harmless if they already exist)
    cur = ""
    for comp in (d.split("/") if d else []):
        cur = f"{cur}/{comp}" if cur else comp
        if cur not in dirs:
            dirs.add(cur)
            ino += 1
            extra += newc(cur, b"", 0o040755, ino)
    ino += 1
    extra += newc(dest, open(src, "rb").read(), 0o100000 | mode, ino)
extra += newc("TRAILER!!!", b"", 0, 0)
extra_gz = gzip.compress(extra, 9, mtime=0)
new_ramdisk = ramdisk + extra_gz

hdr = bytearray(img[:page])
struct.pack_into("<I", hdr, 16, len(new_ramdisk))
if a.cmdline_append:
    cur = bytes(hdr[64:576]).split(b"\0")[0]
    new = cur + b" " + a.cmdline_append.encode()
    if len(new) >= 512:
        sys.exit("cmdline too long")
    hdr[64:576] = new.ljust(512, b"\0")
h = hashlib.sha1()
for blob in (kernel, new_ramdisk, b"", b"", dtb):
    h.update(blob)
    h.update(struct.pack("<I", len(blob)))
hdr[576:608] = h.digest().ljust(32, b"\0")
pad = lambda b: b + b"\0" * (-len(b) % page)
with open(a.out, "wb") as f:
    f.write(bytes(hdr)); f.write(pad(kernel)); f.write(pad(new_ramdisk)); f.write(pad(dtb))
print(f"wrote {a.out}: ramdisk {rsz} -> {len(new_ramdisk)} (+{len(extra_gz)}), files: {[s.split(':')[1] for s in a.file]}")
