#!/usr/bin/env python3
# q2logd (desk-239) - a tiny stand-in for Android's log daemon, only for the island test.
# Meta's trackingservice has its own built-in copy of Android's logging code, which sends every log line to the
# socket /dev/socket/logdw.  Without a log daemon those lines were lost (that is why the tracker's log was nearly
# empty).  This listens on that socket (inside the island only) and writes the lines to a text file.
#   * first 8 MB of lines go to <out>; the most recent 6000 lines are always kept in <out>.tail (rewritten every 2 s)
#   * it only receives and writes text; it never sends anything to any program
import os, socket, struct, sys, time, collections
path, out = sys.argv[1], sys.argv[2]
CAP = 8 * 1024 * 1024
PRIO = "??VDIWEFS"
NAMES = {}
try: os.unlink(path)
except FileNotFoundError: pass
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
s.setsockopt(socket.SOL_SOCKET, 16, 1)          # SO_PASSCRED: learn the sending process id
s.bind(path); os.chmod(path, 0o666)
f = open(out, "w", buffering=1); written = 0; capped = False
tail = collections.deque(maxlen=6000); last_tail = 0.0; s.settimeout(1.0)
def pname(pid):
    n = NAMES.get(pid)
    if n is None:
        try: n = open("/proc/%d/comm" % pid).read().strip()
        except Exception: n = "?"
        NAMES[pid] = n
    return n
def flush_tail():
    try:
        with open(out + ".tail", "w") as t: t.write("".join(tail))
    except Exception: pass
while True:
    try:
        data, anc, _, _ = s.recvmsg(65536, socket.CMSG_SPACE(12))
    except socket.timeout:
        data = None
    except Exception:
        time.sleep(0.2); continue
    if data:
        pid = 0
        for lvl, typ, cd in anc:
            if lvl == socket.SOL_SOCKET and typ == 2 and len(cd) >= 4: pid = struct.unpack("i", cd[:4])[0]   # SCM_CREDENTIALS
        if len(data) >= 11:
            lid, tid, sec, nsec = struct.unpack("<BHII", data[:11]); body = data[11:]
            if lid in (2, 5, 6) or not body:      # binary event / stats / security logs: not text
                continue
            prio = body[0]; rest = body[1:]
            z = rest.find(b"\0"); tag = rest[:z] if z >= 0 else rest; msg = rest[z + 1:] if z >= 0 else b""
            msg = msg.rstrip(b"\0").decode("utf-8", "replace")
            ts = time.strftime("%H:%M:%S", time.localtime(sec)) + ".%03d" % (nsec // 1000000)
            p = PRIO[prio] if prio < len(PRIO) else "?"
            tg = tag.decode("utf-8", "replace")
            for line in (msg.split("\n") or [""]):
                if not line: continue
                l = "%s %5d %5d %-14s %s %s: %s\n" % (ts, pid, tid, pname(pid)[:14], p, tg, line)
                tail.append(l)
                if not capped:
                    f.write(l); written += len(l)
                    if written > CAP: f.write("[q2logd: 8 MB reached - only the .tail file keeps the newest lines from here]\n"); capped = True
    now = time.time()
    if now - last_tail > 2.0: flush_tail(); last_tail = now
