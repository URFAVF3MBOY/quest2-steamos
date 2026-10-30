#!/usr/bin/env python3
"""Flatten the ramdisk of a header-v2 Android boot image built up by inject_boot.py layers.

inject_boot.py appends a gzip'd cpio per change; the kernel unpacks them in order, later files replacing earlier
ones.  After many layers the image carries every superseded copy and outgrows the boot partition.  This merges
all layers into one archive (last entry for a path wins, original order otherwise kept) - same files the kernel
would end up with, minus the dead copies.  Kernel, DTB and cmdline are copied unchanged.

usage: flatten_boot.py --in boot_layered.img --out boot_flat.img
"""
import argparse, gzip, hashlib, struct, sys, zlib

ap = argparse.ArgumentParser()
ap.add_argument("--in", dest="inp", required=True)
ap.add_argument("--out", required=True)
a = ap.parse_args()

img = open(a.inp, "rb").read()
assert img[:8] == b"ANDROID!", "not an Android boot image"
page = struct.unpack_from("<I", img, 36)[0]
assert struct.unpack_from("<I", img, 40)[0] == 2, "expected header v2"
ksz, rsz = struct.unpack_from("<I", img, 8)[0], struct.unpack_from("<I", img, 16)[0]
dsz = struct.unpack_from("<I", img, 1648)[0]
up = lambda n: (n + page - 1) // page * page
koff = page; roff = koff + up(ksz); doff = roff + up(rsz)
kernel, ramdisk, dtb = img[koff:koff + ksz], img[roff:roff + rsz], img[doff:doff + dsz]

# all gzip members, concatenated (trailing zero padding between members is skipped)
raw, pos, members = b"", 0, 0
while pos < len(ramdisk):
    if ramdisk[pos:pos + 2] != b"\x1f\x8b":
        pos += 1
        continue
    d = zlib.decompressobj(16 + zlib.MAX_WBITS)
    raw += d.decompress(ramdisk[pos:]) + d.flush()
    pos = len(ramdisk) - len(d.unused_data)
    members += 1

entries, order, hardlinks = {}, [], 0
layer = []           # entries of the current archive: hard links are resolved per archive (inode numbers are per archive)


def end_layer():
    # newc hard links: every name of the inode has nlink > 1, only the last one carries the data
    global hardlinks
    data_of = {}
    for name, ino, nlink, rec in layer:
        if nlink > 1 and rec[-1]:
            data_of[ino] = rec[-1]
    for name, ino, nlink, rec in layer:
        if nlink > 1 and (rec[0] & 0o170000) == 0o100000:
            hardlinks += 1
            rec = rec[:-1] + (data_of.get(ino, rec[-1]),)    # each name becomes a plain copy
        if name not in entries:
            order.append(name)
        entries[name] = rec
    layer.clear()


p = 0
while p + 110 <= len(raw):
    if raw[p:p + 6] != b"070701":
        p += 1
        continue
    f = [int(raw[p + 6 + 8 * i:p + 14 + 8 * i], 16) for i in range(13)]
    ino, mode, uid, gid, nlink, mtime, size, dmaj, dmin, rmaj, rmin, nsz, chk = f
    name = raw[p + 110:p + 110 + nsz - 1].decode()
    q = p + 110 + nsz; q += -q % 4
    data = raw[q:q + size]; q += size; q += -q % 4
    p = q
    if name == "TRAILER!!!":
        end_layer()
        continue
    layer.append((name, ino, nlink, (mode, uid, gid, mtime, rmaj, rmin, data)))
end_layer()


def newc(name, ino, mode, uid, gid, mtime, rmaj, rmin, data):
    nm = name.encode() + b"\0"
    out = ("070701" + "".join("%08X" % v for v in (
        ino, mode, uid, gid, 1, mtime, len(data), 0, 0, rmaj, rmin, len(nm), 0))).encode() + nm
    out += b"\0" * (-len(out) % 4) + data
    return out + b"\0" * (-len(data) % 4)


cpio = b"".join(newc(n, i + 1, *entries[n]) for i, n in enumerate(order)) + newc("TRAILER!!!", 0, 0, 0, 0, 0, 0, 0, b"")
new_ramdisk = gzip.compress(cpio, 9, mtime=0)

hdr = bytearray(img[:page])
struct.pack_into("<I", hdr, 16, len(new_ramdisk))
h = hashlib.sha1()
for blob in (kernel, new_ramdisk, b"", b"", dtb):
    h.update(blob); h.update(struct.pack("<I", len(blob)))
hdr[576:608] = h.digest().ljust(32, b"\0")
pad = lambda b: b + b"\0" * (-len(b) % page)
with open(a.out, "wb") as fo:
    fo.write(bytes(hdr)); fo.write(pad(kernel)); fo.write(pad(new_ramdisk)); fo.write(pad(dtb))
print(f"{members} ramdisk layers, {len(order)} paths ({hardlinks} hard links made plain copies): "
      f"ramdisk {rsz} -> {len(new_ramdisk)} bytes, wrote {a.out}")
