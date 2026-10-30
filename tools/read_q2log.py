#!/usr/bin/env python3
"""
Read the boot log that the Quest 2 init saved onto the SteamOS USB drive (raw region, see qlog.c).

Windows (run PowerShell/CMD as Administrator):
    py read_q2log.py \\\\.\\PhysicalDrive3
Linux:
    sudo python3 read_q2log.py /dev/sdX
The drive number is the "Disk N" shown in Disk Management. Do NOT format / initialize the disk.
Writes quest2-boot.log (and prints the last lines).
"""
import argparse
import sys

BLK = 4096
CHUNK = 1 << 20          # 1 MiB aligned reads (raw Windows disks need sector-aligned I/O)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("device")
    ap.add_argument("--offset", type=lambda x: int(x, 0), default=11 * 1024 ** 3, help="region offset in bytes (default 11 GiB)")
    ap.add_argument("--size", type=lambda x: int(x, 0), default=64 * 1024 ** 2, help="region size in bytes (default 64 MiB)")
    ap.add_argument("-o", "--out", default="quest2-boot.log")
    ap.add_argument("--tail", type=int, default=40, help="print the last N lines")
    ap.add_argument("--grep", default=None, help="print only lines containing this text (e.g. q2-fetch) instead of the tail")
    a = ap.parse_args()

    try:
        f = open(a.device, "rb", buffering=0)
    except PermissionError:
        sys.exit("Permission denied: run as Administrator (Windows) / with sudo (Linux).")
    except OSError as e:
        sys.exit(f"Cannot open {a.device}: {e}")

    with f:
        f.seek(a.offset)
        first = f.read(BLK)
        if not first.startswith(b"Q2LOG1"):
            sys.exit("No Q2LOG1 header at that offset. Wrong disk, or the boot never reached the drive.\n"
                     f"(first bytes: {first[:16]!r})")
        out = bytearray()
        remaining = a.size - BLK
        pending = b""
        done = False
        while remaining > 0 and not done:
            n = min(CHUNK, remaining)
            buf = pending + f.read(n)
            remaining -= n
            pending = b""
            for i in range(0, len(buf) - len(buf) % BLK, BLK):
                blk = buf[i:i + BLK]
                if not blk.strip(b"\0"):
                    done = True
                    break
                out += blk
            else:
                pending = buf[len(buf) - len(buf) % BLK:]

    text = bytes(out).rstrip(b"\0").decode("utf-8", "replace")
    with open(a.out, "w", encoding="utf-8", newline="\n") as o:
        o.write(text if text.endswith("\n") else text + "\n")
    lines = text.splitlines()
    if a.grep:
        hits = [l for l in lines if a.grep in l]
        print(f"saved {len(lines)} lines ({len(text)} bytes) to {a.out}\n--- {len(hits)} lines matching {a.grep!r}:")
        print("\n".join(hits))
    else:
        print(f"saved {len(lines)} lines ({len(text)} bytes) to {a.out}\n--- last {min(a.tail, len(lines))} lines:")
        print("\n".join(lines[-a.tail:]))


if __name__ == "__main__":
    main()
