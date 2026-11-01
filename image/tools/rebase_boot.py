#!/usr/bin/env python3
"""Build a header-v2 boot image from the kernel + dtb of one image and the ramdisk of another, with cmdline edits.

Used for releases: an older image keeps its own kernel, device tree and cmdline options, and gets the current
ramdisk (init, q2msm, layer, drivers) - e.g. the USB-root image or the desk204 kernel with today's fixes.
The cmdline is edited as a whole (main 512-byte field + the 1024-byte extra field) and split back.

usage: rebase_boot.py --base old.img --ramdisk-from new_flat.img --out out.img \
          [--cmdline-drop TOKEN_OR_KEY] [--cmdline-set key=value] [--cmdline-add token]
"""
import argparse, hashlib, struct, sys

ap = argparse.ArgumentParser()
ap.add_argument("--base", required=True, help="image whose kernel, dtb and cmdline are kept")
ap.add_argument("--ramdisk-from", required=True, help="image whose ramdisk is used")
ap.add_argument("--out", required=True)
ap.add_argument("--cmdline-drop", action="append", default=[], help="remove a token: exact match, or every key=... for a bare key")
ap.add_argument("--cmdline-set", action="append", default=[], help="key=value: replace that key's token (or add it)")
ap.add_argument("--cmdline-add", action="append", default=[], help="append this token if it is not there yet")
a = ap.parse_args()


def parts(path):
    img = open(path, "rb").read()
    assert img[:8] == b"ANDROID!", f"{path}: not an Android boot image"
    page, ver = struct.unpack_from("<II", img, 36)
    assert ver == 2, f"{path}: expected header v2, got v{ver}"
    ksz, rsz, ssz = struct.unpack_from("<I", img, 8)[0], struct.unpack_from("<I", img, 16)[0], struct.unpack_from("<I", img, 24)[0]
    assert ssz == 0 and struct.unpack_from("<I", img, 1632)[0] == 0, f"{path}: second stage / recovery dtbo not supported"
    dsz = struct.unpack_from("<I", img, 1648)[0]
    up = lambda n: (n + page - 1) // page * page
    roff = page + up(ksz); doff = roff + up(rsz)
    k, r, d = img[page:page + ksz], img[roff:roff + rsz], img[doff:doff + dsz]
    assert len(k) == ksz and len(r) == rsz and len(d) == dsz, f"{path}: truncated?"
    return img, page, k, r, d


base, page, kernel, _, dtb = parts(a.base)
_, _, _, ramdisk, _ = parts(a.ramdisk_from)
assert ramdisk[:2] == b"\x1f\x8b", "ramdisk is not gzip"

hdr = bytearray(base[:page])
toks = (bytes(hdr[64:576]).split(b"\0")[0] + b" " + bytes(hdr[608:1632]).split(b"\0")[0]).decode().split()
for t in a.cmdline_drop:
    toks = [x for x in toks if x != t and not ("=" not in t and x.split("=", 1)[0] == t)]
for kv in a.cmdline_set:
    k = kv.split("=", 1)[0]
    toks = [x for x in toks if x.split("=", 1)[0] != k] + [kv]
for t in a.cmdline_add:
    if t not in toks:
        toks.append(t)
# split back: as many whole tokens as fit into the main field, the rest into the extra field
main, extra = [], []
for t in toks:
    if not extra and len(" ".join(main + [t])) < 512:
        main.append(t)
    else:
        extra.append(t)
m, e = " ".join(main).encode(), " ".join(extra).encode()
if len(e) >= 1024:
    sys.exit("cmdline too long")
hdr[64:576] = m.ljust(512, b"\0")
hdr[608:1632] = e.ljust(1024, b"\0")
struct.pack_into("<I", hdr, 8, len(kernel))
struct.pack_into("<I", hdr, 16, len(ramdisk))
struct.pack_into("<I", hdr, 1648, len(dtb))
h = hashlib.sha1()
for blob in (kernel, ramdisk, b"", b"", dtb):
    h.update(blob)
    h.update(struct.pack("<I", len(blob)))
hdr[576:608] = h.digest().ljust(32, b"\0")
pad = lambda b: b + b"\0" * (-len(b) % page)
with open(a.out, "wb") as f:
    f.write(bytes(hdr)); f.write(pad(kernel)); f.write(pad(ramdisk)); f.write(pad(dtb))
print(f"wrote {a.out}: kernel+dtb from {a.base}, ramdisk from {a.ramdisk_from} ({len(ramdisk)} bytes)")
print("cmdline:", " ".join(toks))
