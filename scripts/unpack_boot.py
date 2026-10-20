#!/usr/bin/env python3
"""Split an Android boot image (header v0-v2) into its parts.

usage: unpack_boot.py boot.img outdir
writes: header_page.bin, kernel, ramdisk.cpio.gz (or ramdisk), second (if any), dtb.bin (v2), info.txt
These are exactly the pieces scripts/repack_boot.py puts back together (v2 images).
"""
import struct, sys, os

if len(sys.argv) != 3:
    sys.exit(__doc__)
img = open(sys.argv[1], "rb").read()
out = sys.argv[2]
os.makedirs(out, exist_ok=True)
if img[:8] != b"ANDROID!":
    sys.exit("not an Android boot image (no ANDROID! magic)")
ksz, kaddr, rsz, raddr, ssz, saddr, taddr, page, ver = struct.unpack_from("<9I", img, 8)
cmd = img[64:576].split(b"\0")[0].decode(errors="replace")
extra = img[608:1632].split(b"\0")[0].decode(errors="replace")
up = lambda n: (n + page - 1) // page * page
off = page
parts = {}
parts["kernel"] = img[off:off + ksz]; off += up(ksz)
parts["ramdisk.cpio.gz" if img[off:off + 2] == b"\x1f\x8b" else "ramdisk"] = img[off:off + rsz]; off += up(rsz)
if ssz:
    parts["second"] = img[off:off + ssz]; off += up(ssz)
if ver >= 1:
    rdsz = struct.unpack_from("<I", img, 1632)[0]
    if rdsz:
        parts["recovery_dtbo"] = img[off:off + rdsz]; off += up(rdsz)
if ver >= 2:
    dsz = struct.unpack_from("<I", img, 1648)[0]
    parts["dtb.bin"] = img[off:off + dsz]; off += up(dsz)
open(os.path.join(out, "header_page.bin"), "wb").write(img[:page])
for n, b in parts.items():
    open(os.path.join(out, n), "wb").write(b)
with open(os.path.join(out, "info.txt"), "w") as f:
    f.write(f"header_version {ver}\npage_size {page}\nkernel {ksz} @0x{kaddr:x}\nramdisk {rsz} @0x{raddr:x}\n"
            f"second {ssz}\ntags 0x{taddr:x}\ncmdline: {cmd}\nextra_cmdline: {extra}\n"
            f"trailing bytes after parsed parts: {len(img) - off}\n")
print(open(os.path.join(out, "info.txt")).read())
