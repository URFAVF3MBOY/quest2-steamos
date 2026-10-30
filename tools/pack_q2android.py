#!/usr/bin/env python3
r"""
Pack the Quest 2's Android GPU stack (Adreno 650 EGL/GLES/Vulkan + the libraries they need) into one small tar.gz and,
optionally, write it to the SteamOS USB drive (raw region at 12 GiB) where the Quest 2 init unpacks it into /opt/q2android.

  py pack_q2android.py q2android                       # just builds q2android.tar.gz and prints what it contains
  py pack_q2android.py q2android --write \\\\.\\PhysicalDrive1      (PowerShell/CMD as Administrator)

Input folder = what you pulled with adb:  vendor_lib64\ (incl. hw\ and egl\), system_lib64\, runtime_lib64\, linker64
Nothing on the headset is modified; this only reads the pulled copies.
"""
import argparse, gzip, hashlib, io, os, struct, sys, tarfile

BLK = 4096
OFFSET = 12 * 1024 ** 3
MAX = 400 * 1024 ** 2
SEEDS = ["libgsl.so", "libadreno_utils.so", "libEGL_adreno.so", "libGLESv2_adreno.so", "libGLESv1_CM_adreno.so",
         "libq3dtools_adreno.so", "vulkan.adreno.so", "gralloc.kona.so", "gralloc.default.so", "libllvm-glnext.so",
         "libllvm-qgl.so", "libgralloc.qti.so", "libgralloccore.so", "libgrallocutils.so", "libqdMetaData.so",
         "libqdutils.so", "libc.so", "libm.so", "libdl.so", "liblog.so", "libcutils.so", "libutils.so", "libbase.so",
         "libhardware.so", "libsync.so", "libnativewindow.so", "libui.so", "libhidlbase.so", "libvndksupport.so",
         "libnativeloader.so", "libandroid.so", "libc++.so", "libz.so"]


def needed(path):
    """DT_NEEDED names of a 64-bit little-endian ELF."""
    try:
        d = open(path, "rb").read()
        if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 1:
            return []
        shoff, = struct.unpack_from("<Q", d, 0x28)
        shentsize, shnum = struct.unpack_from("<HH", d, 0x3A)
        secs = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize) for i in range(shnum)]
        out = []
        for s in secs:
            if s[1] == 6:  # SHT_DYNAMIC
                strtab = secs[s[6]]
                for o in range(s[4], s[4] + s[5], 16):
                    tag, val = struct.unpack_from("<qQ", d, o)
                    if tag == 1:
                        a = strtab[4] + val
                        out.append(d[a:d.index(b"\0", a)].decode())
                    if tag == 0:
                        break
        return out
    except Exception:
        return []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--write", help=r"raw disk, e.g. \\.\PhysicalDrive1")
    ap.add_argument("-o", "--out", default="q2android.tar.gz")
    a = ap.parse_args()

    where = {"vendor": os.path.join(a.root, "vendor_lib64"), "system": os.path.join(a.root, "system_lib64"),
             "runtime": os.path.join(a.root, "runtime_lib64")}
    index = {}           # name -> (category, path); vendor wins over system over runtime
    for cat in ("runtime", "system", "vendor"):
        for dp, _, fs in os.walk(where[cat]):
            for f in fs:
                index[f] = (cat, os.path.join(dp, f))
    if not index:
        sys.exit("no files found under " + a.root)

    pick, todo, missing = {}, [n for n in SEEDS if n in index], set()
    # every adreno/egl/hw file we managed to pull is a seed too
    for dp, _, fs in os.walk(where["vendor"]):
        for f in fs:
            if os.path.basename(dp) in ("hw", "egl") or "adreno" in f or f.startswith("libgsl"):
                todo.append(f)
    while todo:
        n = todo.pop()
        if n in pick:
            continue
        if n not in index:
            missing.add(n)
            continue
        pick[n] = index[n]
        todo += [x for x in needed(index[n][1]) if x not in pick]

    buf = io.BytesIO()
    total = 0
    with tarfile.open(fileobj=buf, mode="w") as t:
        def add(arc, p):
            nonlocal total
            ti = t.gettarinfo(p, arc); ti.uid = ti.gid = 0; ti.uname = ti.gname = ""; ti.mode = 0o755
            with open(p, "rb") as fh:
                t.addfile(ti, fh)
            total += ti.size
        for n, (cat, p) in sorted(pick.items()):
            rel = os.path.relpath(p, where[cat]).replace("\\", "/")
            base = {"vendor": "vendor/lib64", "system": "system/lib64", "runtime": "apex/com.android.runtime/lib64"}[cat]
            add(base + "/" + rel, p)
        lk = os.path.join(a.root, "linker64")
        if os.path.exists(lk):
            add("system/bin/linker64", lk)
        fw = os.path.join(a.root, "vendor_firmware")
        if os.path.isdir(fw):
            for dp, _, fs in os.walk(fw):
                for f in fs:
                    add("firmware/" + os.path.relpath(os.path.join(dp, f), fw).replace("\\", "/"), os.path.join(dp, f))
        etc = os.path.join(a.root, "vendor_etc")
        if os.path.isdir(etc):
            for dp, _, fs in os.walk(etc):
                for f in fs:
                    if f.endswith((".xml", ".conf", ".cfg", ".json", ".rc")):
                        add("vendor/etc/" + os.path.relpath(os.path.join(dp, f), etc).replace("\\", "/"), os.path.join(dp, f))
    raw = gzip.compress(buf.getvalue(), 6)
    open(a.out, "wb").write(raw)
    print(f"{len(pick)} libraries, {total/1e6:.1f} MB -> {a.out} ({len(raw)/1e6:.1f} MB gz)")
    print("not found (usually harmless Android-framework libs):", ", ".join(sorted(missing)) or "none")
    for must in ("libgsl.so", "libEGL_adreno.so", "libGLESv2_adreno.so", "vulkan.adreno.so", "gralloc.kona.so", "libc.so", "libcutils.so"):
        print(f"  {must:24s}", "OK" if must in pick else "MISSING")
    if len(raw) > MAX:
        sys.exit("archive too large")
    if not a.write:
        print("\nNot written to a drive (add --write \\\\.\\PhysicalDriveN to do that).")
        return
    hdr = b"Q2BLOB1\0" + struct.pack("<Q", len(raw)) + hashlib.sha256(raw).digest()
    data = hdr.ljust(BLK, b"\0") + raw
    data += b"\0" * (-len(data) % BLK)
    try:
        f = open(a.write, "r+b", buffering=0)
    except PermissionError:
        sys.exit("Permission denied: run as Administrator.")
    with f:
        f.seek(OFFSET)
        step = 1 << 20
        for i in range(0, len(data), step):
            f.write(data[i:i + step])
    print(f"written {len(data)/1e6:.1f} MB to {a.write} at 12 GiB. Boot the next image to unpack it.")


if __name__ == "__main__":
    main()
