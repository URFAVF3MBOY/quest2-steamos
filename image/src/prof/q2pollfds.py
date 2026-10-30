#!/usr/bin/python3
# q2pollfds <pid> <tid> <samples> - desk-288: which file descriptors a thread polls (ppoll/epoll), what is ready on them,
# and the timeout it asked for, sampled from /proc/<tid>/syscall + /proc/<pid>/mem; plus its kernel stack.  Root only.
import os, sys, struct, time, collections
pid, tid, n = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
cnt = collections.Counter(); stk = collections.Counter(); calls = 0
def name(fd):
    try: t = os.readlink('/proc/%d/fd/%d' % (pid, fd))
    except OSError: return '?'
    if t.startswith('anon_inode:'):
        try:
            info = open('/proc/%d/fdinfo/%d' % (pid, fd)).read().split('\n')
            extra = [l for l in info if l.startswith(('eventfd-count', 'clockid', 'ticks', 'tfd'))]
            t += ' ' + ' '.join(e.replace('\t', '').replace(' ', '') for e in extra[:2])
        except OSError: pass
    return t
try: mem = open('/proc/%d/mem' % pid, 'rb')
except OSError as e: print('      (no /proc/mem: %s)' % e); sys.exit()
for i in range(n):
    try:
        s = open('/proc/%d/task/%d/syscall' % (pid, tid)).read().split()
    except OSError: break
    try:
        k = open('/proc/%d/task/%d/stack' % (pid, tid)).read().split('\n')
        stk[' < '.join(l.split()[-1].split('+')[0] for l in k[:5] if l)] += 1
    except (OSError, IndexError): pass
    if s[0] == '73':   # ppoll(fds, nfds, timeout*, sigmask)
        calls += 1; p, nf, tp = int(s[1], 16), int(s[2], 16), int(s[3], 16)
        try:
            mem.seek(p); raw = mem.read(8 * min(nf, 64))
            to = 'inf'
            if tp: mem.seek(tp); a, b = struct.unpack('qq', mem.read(16)); to = '%.1fms' % (a * 1e3 + b / 1e6)
            fds = [struct.unpack_from('ihh', raw, 8 * j) for j in range(len(raw) // 8)]
            cnt['timeout ' + to] += 1
            for fd, ev, rev in fds: cnt['fd %d %s ev=%x rev=%x' % (fd, name(fd), ev, rev)] += 1
        except OSError: pass
    time.sleep(0.03)
print('      ppoll seen %d/%d samples' % (calls, n))
for k, v in cnt.most_common(14): print('        %4d  %s' % (v, k))
print('      kernel stacks:')
for k, v in stk.most_common(5): print('        %4d  %s' % (v, k))
