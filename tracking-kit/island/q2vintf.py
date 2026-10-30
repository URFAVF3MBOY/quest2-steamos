#!/usr/bin/env python3
# q2vintf (desk-245) - island test only.  (AIDL HALs listed by vendor/odm are hidden as well: this test runs none.)
#  Android's HAL lookup WAITS FOREVER for any HIDL HAL that the device manifest
# lists but that is not running; Meta's tracker then misses its power-state deadline and shuts itself down (desk-240:
# the power HAL).  This test runs only a few of Meta's HALs, so every other HIDL HAL is taken out of copies of the
# vendor/odm manifests: a lookup then answers "not available" at once, and Meta's code takes its "HAL missing" path.
# Kept (run by this test, or served in-process): sensors, sensors_java, telemetry, graphics allocator/mapper, the
# display allocator, DSP services, android.hidl.*.  The HIDL power HAL is also removed from the system files.
# The firmware HALs (e.g. vendor.oculus.hardware.arfirmware) are hidden too, so nothing can even look them up.
# Writes changed copies into <outdir>; the caller bind-mounts them over the originals inside the private island
# namespace only.  Prints "<original> <copy>" per changed file, then "HIDDEN <names>".  Partitions are never written.
import os, re, sys
root, outdir = sys.argv[1], sys.argv[2]
os.makedirs(outdir, exist_ok=True)
KEEP = ("android.hidl.", "vendor.oculus.hardware.sensors", "android.hardware.sensors",  # desk-245: the sensors service registers android.hardware.sensors@2.0 too
        "vendor.oculus.hardware.telemetry",
        "android.hardware.graphics.allocator", "android.hardware.graphics.mapper", "android.hardware.graphics.common",
        "vendor.qti.hardware.display.allocator", "vendor.qti.hardware.display.mapper", "vendor.qti.hardware.display.mapperextensions",
        "vendor.qti.hardware.dsp", "vendor.qti.hardware.fastrpc", "android.hardware.renderscript")
HAL = re.compile(r'<hal\b([^>]*)>(.*?)</hal>\s*', re.S)
NAME = re.compile(r'<name>\s*([A-Za-z0-9_.]+)\s*</name>')
hidden = set(); n = 0
for d in ("vendor/etc/vintf", "odm/etc/vintf", "system/etc/vintf", "system_ext/etc/vintf", "product/etc/vintf"):
    device = d.startswith(("vendor", "odm"))
    for dp, dn, fn in os.walk(os.path.join(root, d)):
        for f in fn:
            if not f.endswith(".xml"): continue
            p = os.path.join(dp, f)
            try: s = open(p, encoding="utf-8", errors="replace").read()
            except Exception: continue
            matrix = "compatibility_matrix" in f or "<compatibility-matrix" in s[:400]
            def drop(m):
                attrs, body = m.group(1), m.group(2)
                if 'format="native"' in attrs: return m.group(0)
                nm = NAME.search(body); name = nm.group(1) if nm else ""
                if 'format="aidl"' in attrs:
                    # AIDL HALs: Android's lookup also waits forever for a listed one; this test runs none of them
                    if device and not matrix and name: hidden.add(name + " (aidl)"); return ""
                    return m.group(0)
                if name == "android.hardware.power": hidden.add(name); return ""
                if device and not matrix and name and not name.startswith(KEEP): hidden.add(name); return ""
                return m.group(0)
            t = HAL.sub(drop, s)
            if t != s:
                n += 1; cp = os.path.join(outdir, "%d_%s" % (n, f))
                open(cp, "w", encoding="utf-8").write(t)
                print(os.path.relpath(p, root), cp)
print("HIDDEN", " ".join(sorted(hidden)))
