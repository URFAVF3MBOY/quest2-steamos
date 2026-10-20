#!/usr/bin/env python3
r"""
Pack any folder (e.g. the Mesa source) into a tar.gz and write it to the SteamOS USB drive (raw region at 13 GiB);
the Quest 2 image unpacks it into /opt/q2src on boot.

  py pack_dir.py mesa-src                                   # builds q2src.tar.gz only
  py pack_dir.py mesa-src --write \\.\PhysicalDrive1         (Administrator; check the disk number first!)

.git folders are skipped.  Nothing on the headset itself is touched.
"""
import argparse, gzip, hashlib, io, os, struct, sys, tarfile

BLK = 4096
OFFSET = 13 * 1024 ** 3
MAX = 900 * 1024 ** 2

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("--write")
    ap.add_argument("-o", "--out", default="q2src.tar.gz")
    a = ap.parse_args()
    top = os.path.basename(os.path.abspath(a.folder))
    buf = io.BytesIO()
    n = 0
    with tarfile.open(fileobj=buf, mode="w") as t:
        for dp, dns, fs in os.walk(a.folder):
            dns[:] = [d for d in dns if d != ".git"]
            for f in fs:
                p = os.path.join(dp, f)
                if os.path.islink(p):
                    continue
                arc = top + "/" + os.path.relpath(p, a.folder).replace("\\", "/")
                ti = t.gettarinfo(p, arc); ti.uid = ti.gid = 0; ti.uname = ti.gname = ""
                with open(p, "rb") as fh:
                    t.addfile(ti, fh)
                n += 1
    raw = gzip.compress(buf.getvalue(), 6)
    open(a.out, "wb").write(raw)
    print(f"{n} files, {len(buf.getvalue())/1e6:.0f} MB -> {a.out} ({len(raw)/1e6:.0f} MB gz)")
    if len(raw) > MAX:
        sys.exit("archive too large")
    if not a.write:
        print("Not written to a drive (add --write \\\\.\\PhysicalDriveN).")
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
        for i in range(0, len(data), 1 << 20):
            f.write(data[i:i + (1 << 20)])
    print(f"written {len(data)/1e6:.0f} MB to {a.write} at 13 GiB. Boot the next image to unpack it.")

if __name__ == "__main__":
    main()
