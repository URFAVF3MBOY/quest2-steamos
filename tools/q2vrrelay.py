#!/usr/bin/env python3
"""q2vrrelay - make SteamVR reachable from an Android container on its own network (Lepton's pasta, as on the Steam Frame).

    q2vrrelay.py CONTAINER_PID          (as root; exits when that process is gone)

SteamVR's IPC uses abstract Unix sockets, and abstract names belong to a network namespace, so an app in pasta's
private network can't reach them.  Two kinds are mirrored between the host and the container's namespace:

  1. Stream pipes @/steamvr/SteamVR_Namespace, VR_ServerPipe_<pid>, VR_CompositorPipe_<pid> (listening on the host).
     A twin listens inside the container; each connection is piped to the real socket, bytes and SCM_RIGHTS
     descriptors alike.
  2. Descriptor handovers (IPC::LinuxSharedFd).  The owner of a shared texture or semaphore binds a datagram socket
     @fd-srv-<id> (sun_path padded with NULs); the receiver learns <id> over a pipe, sendto()s a request there and
     gets the descriptor back in the reply.  Owners exist on both sides (compositor: semaphores, app: eye textures),
     and there are thousands of these sockets, so they are found as they are bound: a kprobe on unix_bind in a private
     tracing instance.  Before forwarding any pipe message the relay drains the pending bind events, so the twin of a
     handover socket exists before the message naming it reaches the receiver.  A request arriving at a twin is sent
     on from a proxy bound to the requester's own address on the owner's side, so the owner's reply (with the
     descriptor) comes back to the proxy and is passed on.

setns(CLONE_NEWNET) applies to the calling thread and a socket stays in the namespace it was created in: one thread
lives in the container's namespace and only creates sockets there; everything else runs in host-namespace threads.
"""
import array, os, queue, re, select, socket, sys, threading, time

CLONE_NEWNET = 0x40000000
pid = int(sys.argv[1])
me = os.getpid()
nsfd = os.open('/proc/%d/ns/net' % pid, os.O_RDONLY)
verbose = os.environ.get('Q2VRRELAY_VERBOSE') == '1'

def log(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)

# ---------------------------------------------------------------- the container-namespace socket factory
req = queue.Queue()
entered = threading.Event()

def ns_thread():
    os.setns(nsfd, CLONE_NEWNET)
    entered.set()
    while True:
        kind, name, box, done = req.get()
        try:
            if kind == 'listen':
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.bind(name); s.listen(64)
            elif kind == 'stream':
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            else:                                   # 'dgram': bound to name (None = unbound)
                s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
                if name is not None: s.bind(name)
            box.append(s)
        except OSError as e:
            box.append(e)
        done.set()

def make(side, kind, name=None):
    """A socket in the host's or the container's network namespace."""
    if side == 'container':
        box, done = [], threading.Event()
        req.put((kind, name, box, done)); done.wait()
        if isinstance(box[0], Exception): raise box[0]
        return box[0]
    if kind == 'listen':
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.bind(name); s.listen(64)
    elif kind == 'stream':
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    else:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        if name is not None: s.bind(name)
    return s

threading.Thread(target=ns_thread, daemon=True).start()
entered.wait()

# ---------------------------------------------------------------- bind events (kprobe on unix_bind, own instance)
T = '/sys/kernel/tracing'
INST = '%s/instances/q2vrrelay_%d' % (T, me)
trace = None
try:
    with open(T + '/kprobe_events') as f: defined = 'q2vrbind ' in f.read()
    if not defined:     # one raw write, as echo does (a buffered Python write got EINVAL)
        kfd = os.open(T + '/kprobe_events', os.O_WRONLY | os.O_APPEND)
        try: os.write(kfd, b'p:q2vrbind unix_bind name=+3(%x1):string len=%x2\n')
        finally: os.close(kfd)
    os.makedirs(INST, exist_ok=True)
    with open(INST + '/events/kprobes/q2vrbind/enable', 'w') as f: f.write('1')
    with open(INST + '/tracing_on', 'w') as f: f.write('1')
    trace = os.open(INST + '/trace_pipe', os.O_RDONLY | os.O_NONBLOCK)
except OSError as e:
    log('bind events unavailable (%s): handover sockets only found by periodic scans' % e)

def cleanup_trace():
    try:
        with open(INST + '/events/kprobes/q2vrbind/enable', 'w') as f: f.write('0')
        os.rmdir(INST)
    except OSError:
        pass

# drop to the container owner's uid (the user SteamVR runs as); the trace fd stays readable
uid = os.stat('/proc/%d' % pid).st_uid
if os.getuid() == 0 and uid != 0:
    import pwd
    pw = pwd.getpwuid(uid)
    os.initgroups(pw.pw_name, pw.pw_gid); os.setresgid(pw.pw_gid, pw.pw_gid, 0); os.setresuid(uid, uid, 0)   # saved 0: cleanup at exit

def side_of(tid):
    """'container' for a task in a nested pid namespace (the container), 'host' otherwise; None for our own threads."""
    try:
        with open('/proc/%d/status' % tid) as f:
            st = f.read()
    except OSError:
        return 'gone'
    m = re.search(r'^Tgid:\s*(\d+)', st, re.M)
    if m and int(m.group(1)) == me:
        return None
    m = re.search(r'^NSpid:\s*(.*)$', st, re.M)
    return 'container' if m and len(m.group(1).split()) > 1 else 'host'

BIND_RE = re.compile(r'-(\d+)\s+\[\d+\].*?q2vrbind: \([^)]*\) name="([^"]*)" len=0x([0-9a-f]+)')
tbuf = b''
tlock = threading.Lock()

def drain_binds():
    """Handle every bind announced so far."""
    global tbuf
    if trace is None: return
    with tlock:
        while True:
            try:
                chunk = os.read(trace, 65536)
            except BlockingIOError:
                break
            except OSError:
                break
            if not chunk: break
            tbuf += chunk
        lines = tbuf.split(b'\n'); tbuf = lines.pop()
    for line in lines:
        m = BIND_RE.search(line.decode('utf-8', 'replace'))
        if not m: continue
        tid, nm, ln = int(m.group(1)), m.group(2), int(m.group(3), 16)
        if not (nm.startswith('fd-srv-') or nm.startswith('/steamvr/')): continue
        side = side_of(tid)
        if side in (None, 'gone'): continue
        raw = b'\0' + nm.encode() ; raw = raw + b'\0' * max(0, (ln - 2) - len(raw))
        if nm.startswith('fd-srv-'): dgram_mirror(side, raw)
        else: stream_sync()

# ---------------------------------------------------------------- 1. stream pipes
def listeners(path):
    names = set()
    try:
        with open(path) as f:
            next(f)
            for line in f:
                p = line.split()
                if len(p) >= 8 and p[4] == '0001' and p[3] == '00010000' and p[7].startswith('@/steamvr/'):
                    names.add(p[7])
    except OSError:
        pass
    return names

HOST_TABLE = '/proc/self/net/unix'
CONT_TABLE = '/proc/%d/net/unix' % pid
smirrors = {}
sowned = set()
slock = threading.Lock()

def serve(name, lst):
    while True:
        try:
            c, _ = lst.accept()
        except OSError:
            return
        r = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            r.connect(name)
        except OSError as e:
            log('connect', name, 'failed:', e); c.close(); r.close(); continue
        threading.Thread(target=pump, args=(c, r), daemon=True).start()
        threading.Thread(target=pump, args=(r, c), daemon=True).start()

def stream_sync():
    with slock:
        want = {'\0' + n[1:] for n in listeners(HOST_TABLE)}     # our twins live in the container: not in this table
        for name in want - set(smirrors):
            try:
                s = make('container', 'listen', name)
            except OSError as e:
                smirrors[name] = None; log('mirror', name.replace('\0', '@'), 'failed:', e); continue
            smirrors[name] = s; sowned.add(name)
            threading.Thread(target=serve, args=(name, s), daemon=True).start()
            log('mirrored', name.replace('\0', '@'))
        for name in set(smirrors) - want:
            s = smirrors.pop(name); sowned.discard(name)
            if s:
                try: s.shutdown(socket.SHUT_RDWR)
                except OSError: pass
                s.close()

FDSPACE = socket.CMSG_SPACE(64 * array.array('i').itemsize)

def fds_of(anc):
    fds = []
    for lvl, typ, cdata in anc:
        if lvl == socket.SOL_SOCKET and typ == socket.SCM_RIGHTS:
            a = array.array('i'); a.frombytes(cdata[:len(cdata) - len(cdata) % a.itemsize]); fds += list(a)
    return fds

def pump(src, dst):
    try:
        while True:
            data, anc, flags, _ = src.recvmsg(1 << 18, FDSPACE)
            fds = fds_of(anc)
            if not data and not fds:
                break
            drain_binds()       # a handover socket named in this message gets its twin before the message arrives
            try:
                if fds:
                    sent = dst.sendmsg([data], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', fds))])
                    if sent < len(data): dst.sendall(data[sent:])
                else:
                    dst.sendall(data)
            finally:
                for f in fds: os.close(f)
    except OSError:
        pass
    finally:
        for s in (src, dst):
            try: s.shutdown(socket.SHUT_RDWR)
            except OSError: pass

# ---------------------------------------------------------------- 2. descriptor handovers (datagram)
ep = select.epoll()
dlock = threading.Lock()
twins = {}       # raw name -> (owner side, twin socket on the other side)
by_fd = {}       # fileno -> ('twin', raw) | ('proxy', raw, client address)
proxies = {}     # (raw, client address) -> proxy socket on the owner's side
dg_count = [0, 0]

def dgram_mirror(owner, raw):
    other = 'container' if owner == 'host' else 'host'
    with dlock:
        if raw in twins: return
        try:
            t = make(other, 'dgram', raw)
        except OSError as e:
            if verbose: log('handover twin', raw.rstrip(b'\0')[1:], 'failed:', e)
            return
        twins[raw] = (owner, t); by_fd[t.fileno()] = ('twin', raw)
        ep.register(t.fileno(), select.EPOLLIN)
        dg_count[0] += 1
        if verbose: log('handover', owner, '->', other, raw.rstrip(b'\0')[1:].decode())

def dgram_loop():
    while True:
        for fd, _ in ep.poll(1.0):
            ent = by_fd.get(fd)
            if not ent: continue
            try:
                if ent[0] == 'twin':
                    raw = ent[1]; owner, t = twins[raw]
                    data, anc, flags, addr = t.recvmsg(65536, FDSPACE)
                    fds = fds_of(anc)
                    key = (raw, addr)
                    with dlock:
                        p = proxies.get(key)
                        if p is None:
                            p = make(owner, 'dgram', addr if addr else None)
                            proxies[key] = p; by_fd[p.fileno()] = ('proxy', raw, addr)
                            ep.register(p.fileno(), select.EPOLLIN)
                    try:
                        p.sendmsg([data], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', fds))] if fds else [], 0, raw)
                    finally:
                        for f in fds: os.close(f)
                else:
                    _, raw, addr = ent; owner, t = twins[raw]
                    p = proxies[(raw, addr)]
                    data, anc, flags, _ = p.recvmsg(65536, FDSPACE)
                    fds = fds_of(anc)
                    try:
                        if addr:
                            t.sendmsg([data], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', fds))] if fds else [], 0, addr)
                            dg_count[1] += 1
                    finally:
                        for f in fds: os.close(f)
            except Exception as e:      # never let one bad handover stop the loop
                if verbose: log('handover error', e)

threading.Thread(target=dgram_loop, daemon=True).start()

def dgram_prune():
    """Drop twins (and their proxies) whose real socket is gone."""
    live = set()
    for path in (HOST_TABLE, CONT_TABLE):
        try:
            with open(path) as f:
                for line in f:
                    i = line.find('@fd-srv-')
                    if i >= 0:
                        field = line[i:].split()[0]
                        live.add(('\0' + field[1:].replace('@', '\0')).encode())
        except OSError:
            pass
    with dlock:
        for raw in [r for r in twins if r not in live]:
            owner, t = twins.pop(raw)
            for key in [k for k in proxies if k[0] == raw]:
                p = proxies.pop(key); by_fd.pop(p.fileno(), None)
                try: ep.unregister(p.fileno())
                except OSError: pass
                p.close()
            by_fd.pop(t.fileno(), None)
            try: ep.unregister(t.fileno())
            except OSError: pass
            t.close()

# ---------------------------------------------------------------- main loop
log('relaying SteamVR between the host and the network namespace of pid %d (bind events: %s)' % (pid, 'yes' if trace is not None else 'no'))
n = 0
try:
    while os.path.exists('/proc/%d' % pid):
        drain_binds()
        if n % 5 == 0: stream_sync()
        if n % 50 == 0:
            dgram_prune()
            if n % 300 == 0: log('handover sockets mirrored %d, descriptors passed %d, live twins %d' % (dg_count[0], dg_count[1], len(twins)))
        n += 1
        time.sleep(0.02)
finally:
    log('container gone, exiting')
    if trace is not None:
        try:
            os.seteuid(0)
        except OSError:
            pass
        cleanup_trace()
