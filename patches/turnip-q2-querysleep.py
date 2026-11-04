#!/usr/bin/env python3
# turnip-q2-querysleep.py - vkGetQueryPoolResults(WAIT) sleeps instead of busy-polling.
#
# Turnip's wait_for_available() spins on the query's "available" word, calling clock_gettime in a tight loop, until
# the GPU writes it (Mesa's own TODO: no scheduler-friendly wait yet).  The SteamVR compositor reads its GPU
# timestamps with WAIT every frame, so its render thread (real-time priority, on the big cores) spent ~60 % of a core
# in that loop (perf, 2026-11-04), polling memory the GPU is writing.  This spins for 20 us (results that are
# already nearly done stay instant), then sleeps 100 us between checks.
#
# Usage: python3 turnip-q2-querysleep.py <mesa source dir>     (fails loudly if an anchor is not found)
import os, sys

root = sys.argv[1] if len(sys.argv) > 1 else "."
p = os.path.join(root, "src/freedreno/vulkan/tu_query_pool.cc")
s = open(p).read()
old = ("   while(os_time_get_nano() < abs_timeout) {\n"
       "      if (query_is_available(slot))\n"
       "         return VK_SUCCESS;\n"
       "   }\n"
       "   return vk_error(device, VK_TIMEOUT);\n")
new = ("   /* q2: spin 20 us, then sleep 100 us between checks (turnip-q2-querysleep.py) */\n"
       "   uint64_t q2_spin_end = os_time_get_nano() + 20000;\n"
       "   for (;;) {\n"
       "      if (query_is_available(slot))\n"
       "         return VK_SUCCESS;\n"
       "      uint64_t q2_now = os_time_get_nano();\n"
       "      if (q2_now >= abs_timeout)\n"
       "         break;\n"
       "      if (q2_now >= q2_spin_end)\n"
       "         os_time_sleep(100);\n"
       "   }\n"
       "   return vk_error(device, VK_TIMEOUT);\n")
if s.count(old) != 1:
    sys.exit(f"turnip-q2-querysleep: tu_query_pool.cc: anchor found {s.count(old)}x, expected once")
open(p, "w").write(s.replace(old, new))
print("turnip-q2-querysleep: patched tu_query_pool.cc")
