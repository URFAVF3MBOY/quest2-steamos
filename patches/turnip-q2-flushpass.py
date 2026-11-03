#!/usr/bin/env python3
# turnip-q2-flushpass.py - TU_DEBUG=flushpass: clean + invalidate every GPU cache once at the start of each render pass.
#
# The SteamVR compositor needs TU_DEBUG=flushall on this headset: without it the second eye's pass reads stale data
# (black blocks / a "ghost laser" in the right eye).  flushall does that clean + invalidate before EVERY draw, which is
# cheap for the old renderer but costs the Facet renderer (many more draws) ~3.5 ms a frame - Facet then misses 90 Hz.
# The stale data is between passes (one pass reads what the previous one wrote), so flushing at each pass start is the
# same fix at a fraction of the cost.  Use it instead of flushall: TU_DEBUG=nobin,hiprio,flushpass.
#
# Usage: python3 turnip-q2-flushpass.py <mesa source dir>     (fails loudly if an anchor is not found)
import os, sys

root = sys.argv[1] if len(sys.argv) > 1 else "."
V = os.path.join(root, "src/freedreno/vulkan")

def edit(name, pairs):
    p = os.path.join(V, name)
    s = open(p).read()
    for old, new in pairs:
        if s.count(old) != 1:
            sys.exit(f"turnip-q2-flushpass: {name}: anchor found {s.count(old)}x, expected once: {old[:60]!r}")
        s = s.replace(old, new)
    open(p, "w").write(s)
    print(f"turnip-q2-flushpass: patched {name}")

edit("tu_util.h", [(
    "   TU_DEBUG_GMEM_WARMUP              = BITFIELD64_BIT(38),\n",
    "   TU_DEBUG_GMEM_WARMUP              = BITFIELD64_BIT(38),\n"
    "   TU_DEBUG_FLUSHPASS                = BITFIELD64_BIT(39),\n")])

edit("tu_util.cc", [(
    '   { "flushall", TU_DEBUG_FLUSHALL },\n',
    '   { "flushall", TU_DEBUG_FLUSHALL },\n'
    '   { "flushpass", TU_DEBUG_FLUSHPASS },\n')])

FLUSH = (
    "   if (TU_DEBUG(FLUSHPASS)) {   /* q2: clean + invalidate all caches before the pass (turnip-q2-flushpass.py) */\n"
    "      cmd->state.cache.flush_bits |= TU_CMD_FLAG_ALL_CLEAN | TU_CMD_FLAG_ALL_INVALIDATE;\n"
    "      tu_emit_cache_flush<CHIP>(cmd);\n"
    "   }\n")

edit("tu_cmd_buffer.cc", [
    # classic render passes (outside the pass, so this lands in the primary command stream before it)
    ("   VK_FROM_HANDLE(tu_render_pass, pass, pRenderPassBegin->renderPass);\n"
     "   VK_FROM_HANDLE(tu_framebuffer, fb, pRenderPassBegin->framebuffer);\n",
     FLUSH +
     "   VK_FROM_HANDLE(tu_render_pass, pass, pRenderPassBegin->renderPass);\n"
     "   VK_FROM_HANDLE(tu_framebuffer, fb, pRenderPassBegin->framebuffer);\n"),
    # dynamic rendering (Facet); not for a resumed pass, which continues one already started
    ("   VK_FROM_HANDLE(tu_cmd_buffer, cmd, commandBuffer);\n"
     "\n"
     "   tu_setup_dynamic_render_pass(cmd, pRenderingInfo);\n",
     "   VK_FROM_HANDLE(tu_cmd_buffer, cmd, commandBuffer);\n"
     "\n"
     "   if (!(pRenderingInfo->flags & VK_RENDERING_RESUMING_BIT)) {\n" +
     "".join("   " + l + "\n" for l in FLUSH.rstrip("\n").split("\n")) +
     "   }\n"
     "   tu_setup_dynamic_render_pass(cmd, pRenderingInfo);\n"),
])
