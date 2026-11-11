#!/usr/bin/env python3
# Measure the panel's real refresh rate from the kernel's vblank counter (DRM_IOCTL_WAIT_VBLANK, no DRM master
# needed): two vblanks N seconds apart, (sequence difference) / (timestamp difference).
#   q2vblank.py [seconds=30] [crtc=0]
import fcntl, os, struct, sys, time

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 30
crtc = int(sys.argv[2]) if len(sys.argv) > 2 else 0
fd = os.open('/dev/dri/card0', os.O_RDWR)
IOWR = (3 << 30) | (24 << 16) | (0x64 << 8) | 0x3A
RELATIVE = 0x1
HIGH_CRTC_SHIFT = 1

def wait_next():
    t = RELATIVE
    if crtc == 1:
        t |= 0x2          # _DRM_VBLANK_SECONDARY
    elif crtc > 1:
        t |= (crtc << HIGH_CRTC_SHIFT) & 0x3e
    b = bytearray(struct.pack('<IIQ8x', t, 1, 0))             # request {type, sequence, signal}, union of 24 bytes
    fcntl.ioctl(fd, IOWR, b)
    _, seq, sec, usec = struct.unpack('<IIqq', b)              # reply {type, sequence, tval_sec, tval_usec}
    return seq, sec + usec / 1e6

s0, t0 = wait_next()
time.sleep(secs)
s1, t1 = wait_next()
print('%d vblanks in %.4f s = %.4f Hz (period %.4f ms)' % (s1 - s0, t1 - t0, (s1 - s0) / (t1 - t0), (t1 - t0) / (s1 - s0) * 1000))
