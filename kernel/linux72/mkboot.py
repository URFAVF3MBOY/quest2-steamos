#!/usr/bin/env python3
# Android boot image, header version 2 (kernel, ramdisk, dtb), with the Quest 2's load addresses.
#   mkboot.py KERNEL RAMDISK DTB CMDLINE OUT
import hashlib
import struct
import sys

PAGE = 4096
BASE = 0x0
KERNEL_OFF, RAMDISK_OFF, TAGS_OFF, DTB_OFF = 0x8000, 0x1000000, 0x100, 0x1F00000
OS_VERSION = (12 << 25) | (0 << 18) | (0 << 11) | ((2023 - 2000) << 4) | 8


def pad(b):
    return b + bytes(-len(b) % PAGE)


def main():
    kernel, ramdisk, dtb = (open(p, "rb").read() for p in sys.argv[1:4])
    cmdline = sys.argv[4].encode()
    if len(cmdline) > 512 + 1024 - 2:
        sys.exit("command line too long")
    sha = hashlib.sha1()
    for part in (kernel, ramdisk, b"", b"", dtb):
        sha.update(part)
        sha.update(struct.pack("<I", len(part)))
    hdr = bytearray(1660)
    hdr[0:8] = b"ANDROID!"
    struct.pack_into("<10I", hdr, 8, len(kernel), BASE + KERNEL_OFF, len(ramdisk), BASE + RAMDISK_OFF,
                     0, 0, BASE + TAGS_OFF, PAGE, 2, OS_VERSION)
    hdr[64:64 + min(len(cmdline), 511)] = cmdline[:511]
    hdr[576:576 + 20] = sha.digest()
    if len(cmdline) > 511:
        rest = cmdline[511:]
        hdr[608:608 + len(rest)] = rest
    struct.pack_into("<IQI", hdr, 1632, 0, 0, 1660)
    struct.pack_into("<IQ", hdr, 1648, len(dtb), BASE + DTB_OFF)
    with open(sys.argv[5], "wb") as f:
        for part in (bytes(hdr), kernel, ramdisk, dtb):
            f.write(pad(part))


if __name__ == "__main__":
    main()
