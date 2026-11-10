#!/usr/bin/env python3
"""set_cmdline_arg.py - add or replace one key=value in a boot image's main command line field (header v2).

    python3 tools/set_cmdline_arg.py IMG msm_drm.q2_dfps_ppm=7042 [OUT]

The bootloader reads only the main 512-byte field, so arguments that must reach the kernel go there. The header
checksum covers the kernel, ramdisk and other blobs, not the command line, so nothing else changes.
"""
import sys

src, arg = sys.argv[1], sys.argv[2]
out = sys.argv[3] if len(sys.argv) > 3 else src
d = bytearray(open(src, "rb").read())
if d[:8] != b"ANDROID!":
    sys.exit("not an Android boot image")
key = arg.split("=", 1)[0] + "="
words = [w for w in bytes(d[64:576]).split(b"\0")[0].decode().split(" ") if w and not w.startswith(key)]
words.append(arg)
c = " ".join(words).encode()
if len(c) > 511:
    sys.exit("command line would be %d bytes, the field holds 511" % len(c))
d[64:576] = c.ljust(512, b"\0")
open(out, "wb").write(bytes(d))
print("%s: command line %d bytes, ...%s" % (out, len(c), c[-90:].decode()))
