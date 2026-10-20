#!/usr/bin/python3
# q2sym <pid> < "count lib+0xoff <- lib+0xoff" lines - desk-290: names the functions behind profiler hot spots, from the
# library's own symbol tables (.symtab if present, else .dynsym).  Libraries are found through /proc/<pid>/maps.
import sys, struct, bisect, re
pid = sys.argv[1]; paths = {}
for l in open('/proc/%s/maps' % pid):
    f = l.split()
    if len(f) >= 6 and f[5].startswith('/'): paths.setdefault(f[5].rsplit('/', 1)[1], f[5])
cache = {}
def table(name):
    if name in cache: return cache[name]
    t = None
    try:
        with open(paths[name], 'rb') as fh:
            h = fh.read(64); shoff, = struct.unpack_from('<Q', h, 0x28); shentsize, shnum, shstrndx = struct.unpack_from('<HHH', h, 0x3a)
            phoff, = struct.unpack_from('<Q', h, 0x20); phentsize, phnum = struct.unpack_from('<HH', h, 0x36)
            fh.seek(phoff); ph = fh.read(phentsize * phnum); segs = []
            for i in range(phnum):
                typ, flg, off, va, pa, fsz = struct.unpack_from('<IIQQQQ', ph, i * phentsize)
                if typ == 1: segs.append((off, va, fsz))
            fh.seek(shoff); sh = fh.read(shentsize * shnum); secs = [struct.unpack_from('<IIQQQQIIQQ', sh, i * shentsize) for i in range(shnum)]
            syms = []
            for want in (2, 11):   # SHT_SYMTAB, then SHT_DYNSYM
                for s in secs:
                    if s[1] != want: continue
                    strs = secs[s[6]]; fh.seek(strs[4]); st = fh.read(strs[5]); fh.seek(s[4]); d = fh.read(s[5])
                    for k in range(0, len(d), 24):
                        nm, info, oth, shn, val, sz = struct.unpack_from('<IBBHQQ', d, k)
                        if info & 15 == 2 and val: syms.append((val, sz, st[nm:st.index(b'\0', nm)].decode('ascii', 'replace')))
                if syms: break
            syms.sort(); t = (segs, [s[0] for s in syms], syms)
    except Exception as e: t = None
    cache[name] = t; return t
def sym(x):
    m = re.match(r'(.+)\+0x([0-9a-f]+)$', x)
    if not m or m.group(1) not in paths: return x
    t = table(m.group(1)); off = int(m.group(2), 16)
    if not t: return x
    va = off
    for so, sv, sz in t[0]:
        if so <= off < so + sz: va = off - so + sv; break
    i = bisect.bisect_right(t[1], va) - 1
    if i < 0: return x
    v, sz, n = t[2][i]
    return '%s(%s+0x%x)' % (m.group(1), n[:70], va - v)
for l in sys.stdin:
    f = l.split()
    if len(f) >= 4 and f[2] == '<-': print('        %s %s <- %s' % (f[0], sym(f[1]), sym(f[3])))
