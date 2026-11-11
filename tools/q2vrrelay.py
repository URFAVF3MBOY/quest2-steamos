#!/usr/bin/env python3
"""q2vrrelay - make SteamVR reachable from an Android container on its own network (Lepton's pasta, as on the Steam Frame).

    q2vrrelay.py CONTAINER_PID          (as root; exits when that process is gone)

SteamVR's IPC endpoints are abstract Unix sockets (@/steamvr/SteamVR_Namespace, @/steamvr/VR_ServerPipe_<pid>,
@/steamvr/VR_CompositorPipe_<pid>).  Abstract names belong to a network namespace, so an app in pasta's private network
gets "connection refused".  This relay watches the host's listening @/steamvr/ sockets (/proc/thread-self/net/unix),
binds a twin of each inside the container's network namespace, and pipes every connection through to the real one -
bytes and passed file descriptors (SCM_RIGHTS: SteamVR hands shared memory and semaphores over these sockets; the
kernel lets descriptors cross namespaces).

Namespaces: setns(CLONE_NEWNET) applies to the calling thread, and a socket stays in the namespace it was created in.
One thread lives in the container's namespace and only creates the twin listeners; everything else (accepting,
connecting to the real socket, pumping) runs in host-namespace threads.
"""
import array, os, queue, socket, sys, threading, time

PREFIX = '@/steamvr/'
CLONE_NEWNET = 0x40000000
pid = int(sys.argv[1])
nsfd = os.open('/proc/%d/ns/net' % pid, os.O_RDONLY)
verbose = os.environ.get('Q2VRRELAY_VERBOSE') == '1'

def log(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)

# ---- the container-namespace thread: creates listeners on request ----
req, rep = queue.Queue(), queue.Queue()
entered = threading.Event()

def ns_thread():
    os.setns(nsfd, CLONE_NEWNET)
    entered.set()
    while True:
        name = req.get()
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.bind('\0' + name[1:])
            s.listen(64)
            rep.put((name, s))
        except OSError as e:
            rep.put((name, e))

threading.Thread(target=ns_thread, daemon=True).start()
entered.wait()
# setns needs root; the rest runs as the container's owner (the same user SteamVR runs as), so SteamVR sees its usual
# uid on every connection.  glibc applies setuid to all threads.
uid = os.stat('/proc/%d' % pid).st_uid
if os.getuid() == 0 and uid != 0:
    import pwd
    pw = pwd.getpwuid(uid)
    os.initgroups(pw.pw_name, pw.pw_gid); os.setgid(pw.pw_gid); os.setuid(uid)

# ---- pumping ----
FDSPACE = socket.CMSG_SPACE(64 * array.array('i').itemsize)

def pump(src, dst, tag):
    n = 0
    try:
        while True:
            data, anc, flags, _ = src.recvmsg(1 << 18, FDSPACE)
            fds = []
            for lvl, typ, cdata in anc:
                if lvl == socket.SOL_SOCKET and typ == socket.SCM_RIGHTS:
                    a = array.array('i'); a.frombytes(cdata[:len(cdata) - len(cdata) % a.itemsize]); fds += list(a)
            if not data and not fds:
                break
            try:
                if fds:
                    sent = dst.sendmsg([data], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', fds))])
                    if sent < len(data):
                        dst.sendall(data[sent:])
                else:
                    dst.sendall(data)
            finally:
                for f in fds:
                    os.close(f)
            n += 1
    except OSError:
        pass
    finally:
        for s in (src, dst):
            try: s.shutdown(socket.SHUT_RDWR)
            except OSError: pass
        if verbose: log('closed', tag, n, 'messages')

def serve(name, lst):
    while True:
        try:
            c, _ = lst.accept()
        except OSError:
            return                                   # listener closed (the real socket went away)
        h = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)   # created in this (host) thread's namespace
        try:
            h.connect('\0' + name[1:])
        except OSError as e:
            log('connect', name, 'failed:', e); c.close(); h.close(); continue
        if verbose: log('connection', name)
        threading.Thread(target=pump, args=(c, h, name + ' ->'), daemon=True).start()
        threading.Thread(target=pump, args=(h, c, name + ' <-'), daemon=True).start()

# ---- watch the host's listening SteamVR sockets ----
def host_listeners():
    names = set()
    try:
        with open('/proc/thread-self/net/unix') as f:
            next(f)
            for line in f:
                p = line.split()
                # Num RefCount Protocol Flags Type St Inode Path ; flags 00010000 = listening
                if len(p) >= 8 and p[4] == '0001' and p[3] == '00010000' and p[7].startswith(PREFIX):
                    names.add(p[7])
    except OSError:
        pass
    return names

mirrors = {}
log('relaying SteamVR sockets into the network namespace of pid %d' % pid)
while os.path.exists('/proc/%d' % pid):
    want = host_listeners()
    for name in want - set(mirrors):
        req.put(name)
        n, s = rep.get()
        if isinstance(s, Exception):
            log('mirror', name, 'failed:', s); mirrors[name] = None; continue
        mirrors[name] = s
        threading.Thread(target=serve, args=(name, s), daemon=True).start()
        log('mirrored', name)
    for name in set(mirrors) - want:
        s = mirrors.pop(name)
        if s:
            try: s.shutdown(socket.SHUT_RDWR)
            except OSError: pass
            s.close()
        log('removed', name)
    time.sleep(0.2)
log('container gone, exiting')
