#!/usr/bin/env python3
"""Rebuild Valve's Steam Frame ("deckard") SteamOS root filesystem from its public
casync chunk store and wrap it in a disk image for a USB drive.

  disk.img = [1 MiB: MBR with one Linux partition] + [10 GiB: Valve's rootfs (btrfs)]

Inputs : rootfs.img.caibx  (casync index, from Valve's RAUC bundle; 3.5 MB)
Network: downloads each unique chunk once from steamdeck-images.steamos.cloud
Output : disk.img  (10 GiB + 1 MiB) -> write it to the USB drive with Etcher / Rufus (DD mode)

Needs:  Python 3.8+   and   pip install zstandard
Usage:  python steamos_fetch.py --caibx rootfs.img.caibx --out disk.img
"""
import argparse
import hashlib
import http.client
import os
import struct
import subprocess
import sys
import threading
import time
import urllib.parse
from concurrent.futures import ThreadPoolExecutor, as_completed

_zstd_err = []
try:  # Python 3.14+ ships zstd in the standard library
    from compression import zstd as _stdzstd

    def zstd_decompress(blob):
        return _stdzstd.decompress(blob)
except Exception as e:
    _zstd_err.append(f"compression.zstd: {e!r}")
    try:
        import zstandard

        def zstd_decompress(blob):
            return zstandard.ZstdDecompressor().decompressobj().decompress(blob)
    except Exception as e2:
        _zstd_err.append(f"zstandard: {e2!r}")
        sys.exit("No zstd decoder available:\n  " + "\n  ".join(_zstd_err) +
                 "\nTry:  py -m pip install zstandard")

DEFAULT_STORES = [
    "https://steamdeck-images.steamos.cloud/vr/20260921.6090922/deckard-20260921.6090922-0.5.0.castr",
    "https://steamdeck-images.steamos.cloud/vr/chunks.castr",
]
DEFAULT_SHA256 = "5c53ff2ed7dc78f313a19fc9224aa07e7fb63271b811a4ada295441a0361e6a8"  # from Valve's manifest
MBR_OFFSET = 1 << 20          # rootfs starts 1 MiB into the disk image (LBA 2048)
DISK_SIGNATURE = 0x20262026   # => PARTUUID=20262026-01 for the rootfs partition


def parse_caibx(path):
    """Return (total_size, [(start, length, chunk_id_hex), ...])."""
    d = open(path, "rb").read()
    hsize, htype = struct.unpack_from("<2Q", d, 0)
    if htype != 0x96824D9C7B129FF9:
        sys.exit(f"{path}: not a casync index (bad magic)")
    off = hsize + 16  # skip the table header
    chunks, prev = [], 0
    while off + 40 <= len(d):
        (end,) = struct.unpack_from("<Q", d, off)
        if end <= prev:  # tail marker
            break
        chunks.append((prev, end - prev, d[off + 8:off + 40].hex()))
        prev = end
        off += 40
    return prev, chunks


def make_mbr(part_bytes):
    mbr = bytearray(512)
    struct.pack_into("<I", mbr, 440, DISK_SIGNATURE)
    sectors = (part_bytes + 511) // 512
    # status, CHS first (dummy), type 0x83, CHS last (dummy), LBA start, sector count
    mbr[446:462] = struct.pack("<B3sB3sII", 0x00, b"\xfe\xff\xff", 0x83, b"\xfe\xff\xff",
                               MBR_OFFSET // 512, sectors)
    mbr[510:512] = b"\x55\xaa"
    return bytes(mbr)


_tls = threading.local()


def _conn(url):
    u = urllib.parse.urlsplit(url)
    key = (u.scheme, u.netloc)
    conns = getattr(_tls, "conns", None)
    if conns is None:
        conns = _tls.conns = {}
    c = conns.get(key)
    if c is None:
        cls = http.client.HTTPSConnection if u.scheme == "https" else http.client.HTTPConnection
        c = conns[key] = cls(u.netloc, timeout=60)
    return c, key


def http_get(url):
    u = urllib.parse.urlsplit(url)
    for attempt in range(2):  # one reconnect if the keep-alive socket died
        c, key = _conn(url)
        try:
            c.request("GET", u.path)
            r = c.getresponse()
            body = r.read()
            if r.status == 200:
                return body
            if r.status == 404:
                return None
            raise IOError(f"HTTP {r.status}")
        except (http.client.HTTPException, OSError):
            try:
                c.close()
            except Exception:
                pass
            _tls.conns.pop(key, None)
            if attempt:
                raise
    return None


def sha512_256(data):
    try:
        return hashlib.new("sha512_256", data).hexdigest()
    except ValueError:
        return None


def fetch_chunk(cid, stores):
    """Download + decompress + verify one chunk. Returns raw bytes."""
    last_err = None
    for tries in range(4):
        for store in stores:
            url = f"{store}/{cid[:4]}/{cid}.cacnk"
            try:
                blob = http_get(url)
            except Exception as e:  # network trouble: try the next store / retry
                last_err = e
                continue
            if blob is None:
                continue
            try:
                raw = zstd_decompress(blob)
            except Exception as e:
                last_err = e
                continue
            h = sha512_256(raw)
            if h is not None and h != cid:
                last_err = ValueError("chunk hash mismatch")
                continue
            return raw
        time.sleep(1 + tries * 2)
    raise RuntimeError(f"chunk {cid} failed: {last_err}")


def set_sparse(path):
    if os.name == "nt":
        try:
            subprocess.run(["fsutil", "sparse", "setflag", path], check=False,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--caibx", required=True, help="rootfs.img.caibx")
    ap.add_argument("--out", default="disk.img")
    ap.add_argument("--store", action="append", help="chunk store URL (repeatable); default: Valve's")
    ap.add_argument("--workers", type=int, default=12)
    ap.add_argument("--sha256", default=DEFAULT_SHA256, help="expected sha256 of the rootfs image")
    ap.add_argument("--raw", action="store_true", help="write the bare rootfs (no MBR, no 1 MiB gap)")
    a = ap.parse_args()
    stores = a.store or DEFAULT_STORES

    total, chunks = parse_caibx(a.caibx)
    base = 0 if a.raw else MBR_OFFSET
    positions = {}
    for start, length, cid in chunks:
        positions.setdefault(cid, []).append((start, length))
    uniq = list(positions)
    print(f"rootfs: {total / 2**30:.2f} GiB, {len(chunks)} chunks, {len(uniq)} unique")
    print(f"output: {a.out} ({(base + total) / 2**30:.2f} GiB) -- needs that much free disk space")

    with open(a.out, "wb") as f:
        f.close()
    set_sparse(a.out)
    with open(a.out, "r+b") as f:
        f.truncate(base + total)
        if not a.raw:
            f.seek(0)
            f.write(make_mbr(total))

        t0 = time.time()
        done = nbytes = zeros = 0
        last = 0
        with ThreadPoolExecutor(max_workers=a.workers) as ex:
            futs = {ex.submit(fetch_chunk, cid, stores): cid for cid in uniq}
            try:
                for fut in as_completed(futs):
                    cid = futs[fut]
                    raw = fut.result()
                    nbytes += len(raw)
                    if raw.count(0) == len(raw):  # all zeros: file is already zero there
                        zeros += 1
                    else:
                        for start, length in positions[cid]:
                            if len(raw) != length:
                                sys.exit(f"size mismatch for chunk {cid}: {len(raw)} != {length}")
                            f.seek(base + start)
                            f.write(raw)
                    done += 1
                    now = time.time()
                    if now - last > 2 or done == len(uniq):
                        last = now
                        rate = nbytes / max(now - t0, 1e-6) / 2**20
                        eta = (len(uniq) - done) * (now - t0) / done
                        print(f"\r{done}/{len(uniq)} chunks  {nbytes / 2**20:7.0f} MiB  "
                              f"{rate:5.1f} MiB/s  ETA {eta / 60:4.1f} min   ", end="", flush=True)
            except KeyboardInterrupt:
                for fu in futs:
                    fu.cancel()
                sys.exit("\ninterrupted")
        print(f"\ndownloaded {len(uniq)} unique chunks ({zeros} all-zero) in {time.time() - t0:.0f}s")

    print("verifying sha256 of the rootfs region ...")
    h = hashlib.sha256()
    with open(a.out, "rb") as f:
        f.seek(base)
        left = total
        while left:
            b = f.read(min(left, 8 << 20))
            if not b:
                break
            h.update(b)
            left -= len(b)
    got = h.hexdigest()
    if got == a.sha256:
        print("OK: sha256 matches Valve's manifest")
    else:
        print(f"WARNING: sha256 mismatch\n  got      {got}\n  expected {a.sha256}")
        sys.exit(1)
    if not a.raw:
        print(f"\nDone. Write {a.out} to the USB drive with Etcher or Rufus (DD Image mode).")
        print("This erases the drive. The rootfs partition will be PARTUUID=20262026-01.")


if __name__ == "__main__":
    main()
