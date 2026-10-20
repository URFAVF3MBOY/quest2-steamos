#!/usr/bin/env python3
"""Build a header-v2 Android boot image from stock header + stock DTB bundle
and a new kernel + ramdisk. Keeps every header field of the stock image
(addresses, page size, os_version, cmdline) except the sizes, the optional
cmdline append, and the id hash.

usage: repack_boot.py --header stock/header_page.bin --dtb stock/dtb.bin \
         --kernel Image --ramdisk initramfs.cpio.gz \
         [--cmdline-append "tok1 tok2"] --out boot.img
"""
import argparse, hashlib, struct, sys

ap = argparse.ArgumentParser()
ap.add_argument("--header", required=True)
ap.add_argument("--dtb", required=True)
ap.add_argument("--kernel", required=True)
ap.add_argument("--ramdisk", required=True)
ap.add_argument("--cmdline-append", default="")
ap.add_argument("--out", required=True)
a = ap.parse_args()

hdr = bytearray(open(a.header, "rb").read())
kernel = open(a.kernel, "rb").read()
ramdisk = open(a.ramdisk, "rb").read()
dtb = open(a.dtb, "rb").read()

assert hdr[:8] == b"ANDROID!", "header page is not an Android boot header"
page = struct.unpack_from("<I", hdr, 36)[0]
version = struct.unpack_from("<I", hdr, 40)[0]
assert version == 2, f"expected header v2, got v{version}"
assert struct.unpack_from("<I", hdr, 24)[0] == 0, "second-stage not supported"
assert struct.unpack_from("<I", hdr, 1632)[0] == 0, "recovery_dtbo not supported"

pad = lambda b: b + b"\0" * (-len(b) % page)

struct.pack_into("<I", hdr, 8, len(kernel))
struct.pack_into("<I", hdr, 16, len(ramdisk))
struct.pack_into("<I", hdr, 1648, len(dtb))

if a.cmdline_append:
    cur = bytes(hdr[64:576]).split(b"\0")[0]
    new = cur + b" " + a.cmdline_append.encode()
    if len(new) >= 512:
        sys.exit(f"cmdline too long: {len(new)} bytes (max 511)")
    hdr[64:576] = new.ljust(512, b"\0")

h = hashlib.sha1()
for blob in (kernel, ramdisk, b"", b"", dtb):
    h.update(blob)
    h.update(struct.pack("<I", len(blob)))
hdr[576:608] = h.digest().ljust(32, b"\0")

with open(a.out, "wb") as f:
    f.write(bytes(hdr))
    f.write(pad(kernel))
    f.write(pad(ramdisk))
    f.write(pad(dtb))
print(f"wrote {a.out}: kernel {len(kernel)} ramdisk {len(ramdisk)} dtb {len(dtb)}")
