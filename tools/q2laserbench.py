#!/usr/bin/env python3
# q2laserbench - the "Aurora floor + laser on Steam's UI" frame-drop test without anyone in the headset.
#
# Drives the head and a virtual right controller from a script and reads SteamVR's own frame timing per scene:
#   * head: pauses q2metapose (Meta's tracker feed) and writes poses into /dev/shm/q2pose at 1 kHz (as q2fakehead does),
#     then resumes the real feed.  The right-hand Meta controller record is switched off for the run, so the q2hmd
#     driver takes the right hand from /dev/shm/q2ptr (its mouse/keyboard pointer path: yaw/pitch relative to the head).
#   * dashboard: opened at the start, straight ahead, through q2vrdash (/tmp/q2dashreq).
#   * scenes: head straight ahead or pitched down (Aurora's floor), laser away from the panel or sweeping across it
#     (hover repaints in Steam's UI).  Each scene runs SECONDS; its frame timing comes from the "SteamVR frame timing"
#     lines q2vrdash writes every ~10 s (/var/log/q2/vrlogs/vrdash.txt), the first one of each scene skipped.
# The right hand follows the script even while the real controller is awake (magic 'FRCE' at /dev/shm/q2ptr + 48, q2hmd).
# Usage: q2laserbench.py [seconds per scene, default 30] [label]     Result: /var/log/q2/laserbench.txt
import math, mmap, os, re, signal, struct, subprocess, sys, time

SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 30
LABEL = sys.argv[2] if len(sys.argv) > 2 else ''
DASH = '/var/log/q2/vrlogs/vrdash.txt'
OUT = '/var/log/q2/laserbench.txt'
# name, head pitch (deg, - = down), laser: None = pointed away, else the pitch offset that lands it on the panel
SCENES = [('ahead, laser away', 0, None), ('ahead, laser on panel', 0, 0.0),
          ('floor, laser away', -40, None), ('floor, laser on panel', -40, 40.0)]

def log(s):
    print(s, flush=True)
    with open(OUT, 'a') as f: f.write(s + '\n')

pid = int(subprocess.run(['pgrep', '-f', 'island/q2metapose'], capture_output=True, text=True).stdout.split()[0])
f = open('/dev/shm/q2pose', 'r+b'); m = mmap.mmap(f.fileno(), 4096)
pf = os.open('/dev/shm/q2ptr', os.O_RDWR | os.O_CREAT, 0o666); os.ftruncate(pf, 4096); pm = mmap.mmap(pf, 4096)
x0, y0, z0 = struct.unpack_from('<3d', m, 72)

def qmul(a, b):
    return (a[0]*b[0]-a[1]*b[1]-a[2]*b[2]-a[3]*b[3], a[0]*b[1]+a[1]*b[0]+a[2]*b[3]-a[3]*b[2],
            a[0]*b[2]-a[1]*b[3]+a[2]*b[0]+a[3]*b[1], a[0]*b[3]+a[1]*b[2]-a[2]*b[1]+a[3]*b[0])

def put_head(t, pitch):
    q = (math.cos(pitch / 2), math.sin(pitch / 2), 0.0, 0.0)          # about +X: + = up
    s = struct.unpack_from('<I', m, 0)[0]
    struct.pack_into('<I', m, 0, (s | 1) & 0xffffffff)
    struct.pack_into('<8d', m, 8, q[0], q[1], q[2], q[3], t, 0.0, 0.0, 0.0)
    struct.pack_into('<3d', m, 72, x0, y0, z0)
    struct.pack_into('<I', m, 100, (struct.unpack_from('<I', m, 100)[0] + 1) & 0xffffffff)
    struct.pack_into('<d', m, 104, t)
    struct.pack_into('<3d', m, 112, 0.0, 0.0, 0.0)
    struct.pack_into('<I', m, 384, 0)                                  # right Meta controller record off
    struct.pack_into('<I', m, 0, ((s | 1) + 1) & 0xffffffff)

def put_ptr(yaw, pitch):
    s = struct.unpack_from('<I', pm, 0)[0]
    struct.pack_into('<I', pm, 0, (s | 1) & 0xffffffff)
    struct.pack_into('<5d', pm, 8, yaw, pitch, 0.0, 0.0, 0.0)
    struct.pack_into('<I', pm, 48, 0x45435246)                         # 'FRCE': q2hmd uses this even with a real controller awake
    struct.pack_into('<I', pm, 0, (((s | 1) + 1) or 2) & 0xffffffff)

def timing_lines():
    try: return open(DASH, errors='replace').read().splitlines()
    except OSError: return []

def parse(lines):
    g, mx, dr, fi = [], 0.0, 0, []
    for l in lines:
        r = re.search(r'compositor GPU ([0-9.]+) ms \(max ([0-9.]+)\).*dropped ([0-9]+),.*frame interval avg ([0-9.]+) ms min [0-9.]+ max ([0-9.]+)', l)
        if r:
            g.append(float(r.group(1))); mx = max(mx, float(r.group(2))); dr += int(r.group(3)); fi.append(float(r.group(5)))
    return g, mx, dr, fi

os.kill(pid, signal.SIGSTOP)
try:
    log('=== %s q2laserbench %s (%.0f s per scene)' % (time.strftime('%F %T'), LABEL, SECS))
    t0 = time.monotonic()
    while time.monotonic() - t0 < 2.0:                                  # face ahead, then open the dashboard there
        put_head(time.monotonic(), 0.0); put_ptr(1.2, 0.0); time.sleep(0.001)
    with open('/tmp/q2dashreq.n', 'w') as rq: rq.write('%d open' % int(time.time()))
    os.chmod('/tmp/q2dashreq.n', 0o644); os.replace('/tmp/q2dashreq.n', '/tmp/q2dashreq')
    t0 = time.monotonic()
    while time.monotonic() - t0 < 4.0:
        put_head(time.monotonic(), 0.0); put_ptr(1.2, 0.0); time.sleep(0.001)
    for name, hp, lp in SCENES:
        n0 = len(timing_lines()); ts = time.monotonic()
        while True:
            t = time.monotonic(); u = t - ts
            if u >= SECS: break
            put_head(t, math.radians(hp))
            if lp is None: put_ptr(1.2, 0.0)                             # 70 deg to the side: off the panel
            else: put_ptr(0.18 * math.sin(2 * math.pi * u / 3.0), math.radians(lp) + 0.08 * math.sin(2 * math.pi * u / 1.3))
            time.sleep(0.001)
        g, mx, dr, fi = parse(timing_lines()[n0 + 1:])
        if g: log('%-24s GPU avg %5.1f ms, max %5.1f | dropped %3d | worst frame %5.1f ms   (%d windows)' % (name, sum(g) / len(g), mx, dr, max(fi), len(g)))
        else: log('%-24s no frame timing lines' % name)
finally:
    os.kill(pid, signal.SIGCONT)
    put_ptr(0.0, 0.0); struct.pack_into('<I', pm, 48, 0)
    with open('/tmp/q2dashreq.n', 'w') as rq: rq.write('%d close' % int(time.time()))
    os.chmod('/tmp/q2dashreq.n', 0o644); os.replace('/tmp/q2dashreq.n', '/tmp/q2dashreq')
