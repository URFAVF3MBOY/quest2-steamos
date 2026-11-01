#!/usr/bin/env python3
"""desk-334: the stock 587 MHz GPU level in the boot image's device tree.

Meta's kona v2.1 DTB gives speed bin 0 the GPU levels 805 and 670 MHz; this headset's bin uses bin 1's stock 670 and
587 MHz instead (the boot script pins the GPU at the highest level <= 587 MHz).  Two big-endian u32 values in the
base image's DTB, checked before they are changed - any other DTB is refused.

usage: patch_dtb.py --in base.img --out patched.img
"""
import argparse, hashlib, struct, sys

# offset in the DTB, expected value, new value (Hz)
PATCH = [(0xfd204, 805000000, 670000000), (0xfd2b0, 670000000, 587000000)]

ap = argparse.ArgumentParser()
ap.add_argument("--in", dest="inp", required=True)
ap.add_argument("--out", required=True)
a = ap.parse_args()

img = bytearray(open(a.inp, "rb").read())
assert img[:8] == b"ANDROID!" and struct.unpack_from("<I", img, 40)[0] == 2, "expected a header-v2 boot image"
page = struct.unpack_from("<I", img, 36)[0]
ksz, rsz, dsz = struct.unpack_from("<I", img, 8)[0], struct.unpack_from("<I", img, 16)[0], struct.unpack_from("<I", img, 1648)[0]
up = lambda n: (n + page - 1) // page * page
doff = page + up(ksz) + up(rsz)
for off, old, new in PATCH:
    cur = struct.unpack_from(">I", img, doff + off)[0]
    if cur == new:
        continue
    if cur != old:
        sys.exit(f"DTB offset {off:#x}: {cur}, expected {old} - not the base image's device tree")
    struct.pack_into(">I", img, doff + off, new)
h = hashlib.sha1()
for o, n in ((page, ksz), (page + up(ksz), rsz), (0, 0), (0, 0), (doff, dsz)):
    h.update(bytes(img[o:o + n]))
    h.update(struct.pack("<I", n))
img[576:608] = h.digest().ljust(32, b"\0")
open(a.out, "wb").write(img)
print(f"wrote {a.out}: GPU levels " + ", ".join(f"{o // 1000000}->{n // 1000000} MHz" for _, o, n in PATCH))
