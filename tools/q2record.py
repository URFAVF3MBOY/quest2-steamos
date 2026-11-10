#!/usr/bin/env python3
# Record what the Quest 2's panels show (the buffer the display engine scans out) to an MP4, from SSH.
#   q2record.py out.mp4 [seconds=60] [fps=30]
# ffmpeg's kmsgrab can't read framebuffers on this kernel (GETFB2 is missing), so this reads the scanned-out dumb
# buffer itself, like q2panelshot.py, and pipes raw frames to ffmpeg. The buffer holds both eyes stacked and turned
# on their side (lens distortion included); the video shows them upright, side by side.
import fcntl, mmap, os, struct, subprocess, sys, time

out = sys.argv[1] if len(sys.argv) > 1 else '/tmp/q2record.mp4'
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 60
fps = int(sys.argv[3]) if len(sys.argv) > 3 else 30
PLANE = int(os.environ.get('PLANE', '58'))

def IOWR(nr, size): return (3 << 30) | (size << 16) | (0x64 << 8) | nr
fd = os.open('/dev/dri/card0', os.O_RDWR)
fcntl.ioctl(fd, (1 << 30) | (16 << 16) | (0x64 << 8) | 0x0D, struct.pack('<QQ', 2, 1))   # universal planes

def plane_fb(pid):
    b = bytearray(struct.pack('<IIIIIIQ', pid, 0, 0, 0, 0, 0, 0))
    fcntl.ioctl(fd, IOWR(0xB6, len(b)), b)                                                 # GETPLANE
    return struct.unpack_from('<III', b, 0)[2]

maps = {}
def frame():
    fb = plane_fb(PLANE)
    if fb not in maps:
        b = bytearray(struct.pack('<7I', fb, 0, 0, 0, 0, 0, 0)); fcntl.ioctl(fd, IOWR(0xAD, len(b)), b)   # GETFB
        _, w, h, pitch, bpp, depth, handle = struct.unpack('<7I', b)
        b = bytearray(struct.pack('<IIQ', handle, 0, 0)); fcntl.ioctl(fd, IOWR(0xB3, len(b)), b)         # MAP_DUMB
        off = struct.unpack_from('<Q', b, 8)[0]
        maps[fb] = (mmap.mmap(fd, pitch * h, mmap.MAP_SHARED, mmap.PROT_READ, offset=off), w, h, pitch)
    return maps[fb]

m, W, H, PITCH = frame()
if PITCH != W * 4:
    sys.exit('pitch %d != width*4' % PITCH)
sw, sh = W // 2 // 2 * 2, H // 2 // 4 * 4    # half resolution, even sizes; each eye is the top / bottom half
eh = sh // 2
vf = ('scale=%d:%d,split[a][b];[a]crop=%d:%d:0:0,transpose=2[l];[b]crop=%d:%d:0:%d,transpose=2[r];[l][r]hstack'
      % (sw, sh, sw, eh, sw, eh, eh))
ff = subprocess.Popen(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'bgr0',
                       '-s', '%dx%d' % (W, H), '-framerate', str(fps), '-i', '-', '-vf', vf, '-c:v', 'libx264',
                       '-preset', 'veryfast', '-crf', '23', '-pix_fmt', 'yuv420p', '-movflags', '+faststart', out],
                      stdin=subprocess.PIPE)
t0 = time.monotonic(); n = 0
while True:
    t = t0 + n / fps
    if t - t0 >= secs: break
    d = t - time.monotonic()
    if d > 0: time.sleep(d)
    m, _, _, _ = frame()
    ff.stdin.write(m[:])
    n += 1
ff.stdin.close(); ff.wait()
print('%d frames, %.1f s -> %s' % (n, time.monotonic() - t0, out))
