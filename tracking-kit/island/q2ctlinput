#!/usr/bin/python3
# q2ctlinput (desk-251) - Touch controller buttons for SteamVR while Meta's tracker owns SyncBoss.
# LISTEN ONLY: opens /dev/syncboss_stream0 read-only (the SyncBoss driver copies every packet to each reader of the
# stream) and never opens /dev/syncboss0, writes no SyncBoss settings and sends nothing to the MCU or the controllers.
# Meta's sensors service keeps doing all of that.  Positions and rotations come from Meta's tracker (q2metapose); this
# only adds trigger, grip, stick, buttons, touch and battery.
# Output: /dev/shm/q2ctl in q2headtrack's layout (q2hmd reads it): seq u32 (seqlock), per hand (left +8, right +136)
# 16 doubles: counter, aim w x y z (identity here), angular velocity xyz (0), trigger, grip, stick x, stick y,
# buttons (bit0 A/X, bit1 B/Y, bit2 stick, bit3 menu/Oculus), touch bits, battery %, 0.
# Packet layout (Quest 2 "Jedi" controllers, same parser as q2headtrack; protocol from nicolas-riera/quest1-steamos,
# docs/controllers-native.md, used with the author's permission): packet 0x8f = {u64 id; u32 pulsar_type; ...;
# u8 register @19; side channel @20..23; chunks @24}, chunk = u16 header (type bits 0-4, length bits 5-11, fragment
# bits 12-14, bit 15 last) + data: 0 battery, 2 stick, 3 trigger/grip (2 x 12 bit), 4 buttons, 5 touch.
import mmap, os, select, struct, sys, time

def log(m):
    print('%s q2ctlinput: %s' % (time.strftime('%H:%M:%S'), m), flush=True)

SHM = '/dev/shm/q2ctl'
fd = os.open(SHM, os.O_RDWR | os.O_CREAT, 0o644)
os.ftruncate(fd, 4096); os.fchmod(fd, 0o644)
cm = mmap.mmap(fd, 4096)
cm[0:4096] = bytes(4096)
seq = [0]
BTN = ['A/X', 'B/Y', 'stick', 'menu/Oculus']

class Ctl:
    def __init__(s, slot, cid, ptype):
        s.slot = slot; s.id = cid; s.name = 'left' if slot == 0 else 'right'
        s.trig = s.grip = s.sx = s.sy = 0.0; s.btn = 0; s.touch = 0; s.batt = -1; s.inv = None
        s.cnt = 0; s.npk = 0; s.frag = None
    def chunk(s, t, b):
        if t == 0 and len(b) >= 1:
            if b[0] != s.batt: log('%s battery %d%%' % (s.name, b[0]))
            s.batt = b[0]
        elif t == 2 and len(b) >= 4:
            x, y = struct.unpack_from('<2h', b, 0)
            s.sx = x / 32767.0 if x > 0 else x / 32768.0; s.sy = y / 32767.0 if y > 0 else y / 32768.0
        elif t == 3 and len(b) >= 3:
            v = b[0] | b[1] << 8 | b[2] << 16; tr = (v & 0xfff) / 4095.0; gr = (v >> 12 & 0xfff) / 4095.0
            if s.inv is None:
                s.inv = tr > 0.5 and gr > 0.5; log('%s trigger/grip at rest %.2f/%.2f -> %s' % (s.name, tr, gr, 'inverted' if s.inv else 'direct'))
            s.trig, s.grip = ((1 - tr, 1 - gr) if s.inv else (tr, gr))
        elif t == 4 and len(b) >= 1:
            nb = b[0] & 0x0f
            if nb != s.btn: log('%s buttons %s' % (s.name, '+'.join(BTN[i] for i in range(4) if nb >> i & 1) or 'released'))
            s.btn = nb
        elif t == 5 and len(b) >= 1:
            s.touch = b[0]
    def publish(s):
        s.cnt += 1; o = 8 + s.slot * 128
        seq[0] += 1; struct.pack_into('<I', cm, 0, seq[0])
        struct.pack_into('<16d', cm, o, s.cnt, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, s.trig, s.grip, s.sx, s.sy, s.btn, s.touch, s.batt, 0.0)
        seq[0] += 1; struct.pack_into('<I', cm, 0, seq[0])

CTLS = {}
def ctl_get(cid, ptype):
    c = CTLS.get(cid)
    if c: return c
    side = ptype >> 16 & 0xff
    taken = set(x.slot for x in CTLS.values())
    slot = 0 if side == 0 else 1 if side == 1 else (1 if 1 not in taken else 0)
    if slot in taken: slot = 1 - slot
    c = Ctl(slot, cid, ptype); CTLS[cid] = c
    log('%s controller: id %016x, pulsar type 0x%08x' % (c.name, cid, ptype))
    return c

def packet(d):
    if len(d) < 24: return
    cid, ptype = struct.unpack_from('<QI', d, 0)
    if d[19] >= 2: return
    c = ctl_get(cid, ptype); c.npk += 1
    p = d[24:]; i = 0
    while i + 2 <= len(p):
        h = p[i] | p[i + 1] << 8
        t = h & 0x1f; n = h >> 5 & 0x7f; idx = h >> 12 & 7; last = h & 0x8000
        if i + 2 + n > len(p): break
        b = bytes(p[i + 2:i + 2 + n]); i += 2 + n
        if idx == 0 and last: c.frag = None; c.chunk(t, b)
        elif idx == 0: c.frag = [t, 0, b]
        elif c.frag and c.frag[0] == t and c.frag[1] + 1 == idx and len(c.frag[2]) + n <= 128:
            c.frag[1] = idx; c.frag[2] += b
            if last: t2, _, b2 = c.frag; c.frag = None; c.chunk(t2, b2)
        else: c.frag = None
    c.publish()

try:
    st = os.open('/dev/syncboss_stream0', os.O_RDONLY)
except Exception as e:
    log('cannot open the SyncBoss stream: %s' % e); sys.exit(1)
log('listening on the SyncBoss stream (read only, nothing is sent)')
buf = b''; tlog = time.time(); n8f = 0; seen = {}
while True:
    r, _, _ = select.select([st], [], [], 1.0)
    if r:
        buf += os.read(st, 65536)
        while len(buf) >= 3:
            ty, l = buf[0], buf[2]
            if len(buf) < 3 + l: break
            d = buf[3:3 + l]; buf = buf[3 + l:]
            seen[ty] = seen.get(ty, 0) + 1
            if ty == 0x8f:
                n8f += 1
                try: packet(d)
                except Exception as e: log('packet error %r: %s' % (e, d.hex()))
    if time.time() - tlog > 10:
        tlog = time.time()
        log('10 s: controller packets %d, stream types %s; %s' % (n8f, ' '.join('%02x:%d' % kv for kv in sorted(seen.items())),
            '; '.join('%s %d pkt, battery %s%%, trigger %.2f grip %.2f stick %+.2f %+.2f buttons 0x%x touch 0x%x'
                      % (c.name, c.npk, c.batt, c.trig, c.grip, c.sx, c.sy, c.btn, c.touch) for c in sorted(CTLS.values(), key=lambda x: x.slot)) or 'no controllers yet'))
        n8f = 0; seen = {}
        for c in CTLS.values(): c.npk = 0
