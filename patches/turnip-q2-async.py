#!/usr/bin/env python3
# turnip-q2-async.py - patch Mesa's Turnip (KGSL backend) so SteamVR's compositor gets a REAL second GPU queue.
#
# Stock Turnip on KGSL: one queue family, one queue, one GPU priority (MEDIUM).  vrcompositor's "async" mode wants a
# separate compute queue (its late lens-warp) at high priority; our display layer could only fake it by aliasing it onto
# the graphics queue, so the two halves of the compositor waited on each other (~31 ms per frame for ~11 ms of GPU work).
#
# This patch (KGSL only; the MSM backend is unchanged):
#   1. adds a COMPUTE|TRANSFER-only queue family.  Each queue = its own KGSL draw context, so its submits no longer wait
#      behind the graphics queue's submits (separate Vulkan queues, separate kernel contexts).
#      TU_Q2_COMPUTE_FAMILY=0 turns it off (stock queue layout).
#   2. exposes 4 global priorities (LOW/MEDIUM/HIGH/REALTIME) and passes them to the kernel as KGSL context priorities.
#      KGSL: priority field 0 = "undefined" -> default 1; ring buffer = priority / 4 (RB0 = 1..3 highest, RB1 = 4..7, ...);
#      ring-buffer preemption (preempt_level 0x1 on the Quest 2) lets a higher ring interrupt a lower one.
#      TU_KGSL_PRIO="rt,high,medium,low" sets the KGSL priority used for each level (default "1,1,1,8": everything the
#      way stock creates it except LOW).  E.g. "1,1,5,9" puts normal work on ring 1 so HIGH/REALTIME can preempt it.
#   3. logs every KGSL context it creates to stderr ("q2-turnip: ...") so the compositor log shows it worked.
#
# Usage: python3 turnip-q2-async.py <mesa source dir>     (fails loudly if an anchor is not found)
import re, sys, os

root = sys.argv[1] if len(sys.argv) > 1 else "."
V = os.path.join(root, "src/freedreno/vulkan")

def edit(name, fn):
    p = os.path.join(V, name)
    s = open(p).read()
    t = fn(s)
    if t == s:
        sys.exit(f"turnip-q2-async: {name}: nothing changed (anchor missing?)")
    open(p, "w").write(t)
    print(f"turnip-q2-async: patched {name}")

def sub1(pat, rep, s, what, flags=0):
    t, n = re.subn(pat, rep, s, count=1, flags=flags)
    if n != 1:
        sys.exit(f"turnip-q2-async: anchor not found: {what}")
    return t

# --- tu_device.h: room for the extra family -------------------------------------------------------------------
def dev_h(s):
    return sub1(r"(#define\s+TU_MAX_QUEUE_FAMILIES\s+)(\d+)", lambda m: m.group(1) + str(int(m.group(2)) + 1), s,
                "TU_MAX_QUEUE_FAMILIES")
edit("tu_device.h", dev_h)

# --- tu_device.cc ---------------------------------------------------------------------------------------------
COMPUTE_FAMILY = """/* q2: compute-only family (KGSL), backed by the same engine; each queue is its own KGSL context */
static const VkQueueFamilyProperties tu_q2_compute_queue_family_properties = {
   .queueFlags = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
   .queueCount = 1,
   .timestampValidBits = 48,
   .minImageTransferGranularity = { 1, 1, 1 },
};

"""
ADD_FAMILY = """
   /* q2: a compute-only queue family on KGSL (vrcompositor's async late-warp queue) */
   if (is_kgsl(instance) && debug_get_bool_option("TU_Q2_COMPUTE_FAMILY", true) &&
       device->num_queue_families < TU_MAX_QUEUE_FAMILIES) {
      device->queue_families[device->num_queue_families++] =
         (struct tu_queue_family) {
            .type = TU_QUEUE_GFX,
            .properties = &tu_q2_compute_queue_family_properties,
         };
   }
"""
def dev_cc(s):
    # 1. the properties struct, before the first queue family properties definition
    s = sub1(r"(static const VkQueueFamilyProperties tu_gfx_queue_family_properties\s*=)",
             lambda m: COMPUTE_FAMILY + m.group(1), s, "tu_gfx_queue_family_properties")
    # 2. register it after the sparse family block (or after the gfx family if the block looks different)
    m = re.search(r"\n   if \(device->has_sparse\) \{\n      device->queue_families\[device->num_queue_families\+\+\] =.*?\n   \}\n", s, re.S)
    if not m:
        m = re.search(r"\n   device->queue_families\[device->num_queue_families\+\+\] =\n      \(struct tu_queue_family\) \{.*?\n      \};\n", s, re.S)
    if not m:
        sys.exit("turnip-q2-async: anchor not found: queue family registration")
    s = s[:m.end()] + ADD_FAMILY + s[m.end():]
    # the registration must sit in tu_physical_device_init(..., struct tu_instance *instance)
    fstart = s.rfind("tu_physical_device_init(", 0, m.start())
    if fstart < 0 or "struct tu_instance *instance" not in s[fstart:fstart + 200]:
        sys.exit("turnip-q2-async: queue families are not registered in tu_physical_device_init(..., instance)")
    # 3. advertise up to 4 global priorities on KGSL
    s = sub1(r"MIN2\(pdevice->submitqueue_priority_count,\s*3\)",
             "MIN2(pdevice->submitqueue_priority_count, is_kgsl(pdevice->instance) ? 4 : 3)", s,
             "priorityCount MIN2")
    if "util/u_debug.h" not in s and "debug_get_bool_option" not in s.replace(ADD_FAMILY, ""):
        s = s.replace('#include "tu_device.h"', '#include "tu_device.h"\n#include "util/u_debug.h"', 1)
    return s
edit("tu_device.cc", dev_cc)

# --- tu_cmd_buffer.cc: hardware init for primary command buffers of any graphics-engine family -----------------
def cmd_cc(s):
    if "switch (cmd_buffer->queue_family_index) {" not in s:
        print("turnip-q2-async: tu_cmd_buffer.cc has no family switch - leaving it")
        return s + "\n"
    return s.replace("switch (cmd_buffer->queue_family_index) {",
                     "switch (TU_QUEUE_GENERAL) { /* q2: all our families run on the graphics engine */", 1)
edit("tu_cmd_buffer.cc", cmd_cc)

# --- tu_knl_kgsl.cc: priorities -> KGSL context priority --------------------------------------------------------
PRIO_FN = r"""
#ifndef KGSL_CONTEXT_PRIORITY_SHIFT
#define KGSL_CONTEXT_PRIORITY_SHIFT 12
#define KGSL_CONTEXT_PRIORITY_MASK 0x0000F000
#endif
/* q2: Turnip priority level (0 = REALTIME .. 3 = LOW) -> KGSL context priority (1..15; 0 means "default" = 1) */
static int
q2_kgsl_priority(uint32_t level)
{
   static int map[4] = { 1, 1, 1, 8 };
   static int parsed;
   if (!parsed) {
      const char *e = getenv("TU_KGSL_PRIO");
      for (int i = 0; e && *e && i < 4; i++) {
         int v = 0, any = 0;
         while (*e == ' ') e++;
         while (*e >= '0' && *e <= '9') { v = v * 10 + (*e++ - '0'); any = 1; }
         if (any && v >= 1 && v <= 15) map[i] = v;
         while (*e && *e != ',') e++;
         if (*e == ',') e++;
      }
      parsed = 1;
   }
   return map[level < 4 ? level : 3];
}
"""
def kgsl_cc(s):
    s = sub1(r"\nstatic int\nkgsl_submitqueue_new\(", lambda m: PRIO_FN + m.group(0), s, "kgsl_submitqueue_new")
    s = sub1(r"(   int ret = safe_ioctl\(dev->physical_device->local_fd, IOCTL_KGSL_DRAWCTXT_CREATE, &req\);\n"
             r"   if \(ret\)\n      return ret;\n)",
             lambda m: ("   const int q2prio = q2_kgsl_priority(queue->priority);\n"
                        "   req.flags |= ((uint32_t) q2prio << KGSL_CONTEXT_PRIORITY_SHIFT) & KGSL_CONTEXT_PRIORITY_MASK;\n"
                        + m.group(1) +
                        "   fprintf(stderr, \"q2-turnip: kgsl context %u: family %u, level %u -> kgsl priority %d (ring %d)\\n\",\n"
                        "           req.drawctxt_id, queue->vk.queue_family_index, queue->priority, q2prio, q2prio / 4);\n"),
             s, "DRAWCTXT_CREATE ioctl")
    s = sub1(r"device->submitqueue_priority_count\s*=\s*1;", "device->submitqueue_priority_count = 4; /* q2 */", s,
             "submitqueue_priority_count = 1")
    if "<stdio.h>" not in s:
        s = "#include <stdio.h>\n#include <stdlib.h>\n" + s
    return s
edit("tu_knl_kgsl.cc", kgsl_cc)
print("turnip-q2-async: done")
