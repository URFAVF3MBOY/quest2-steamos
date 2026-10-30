# drm/msm GPU port (KGSL → mainline msm) — plan and findings

Goal: run the Quest 2's Adreno 650 on the mainline `drm/msm` GPU driver instead of Qualcomm's KGSL, so stock Mesa
(Turnip's `msm` backend, real DRM sync objects) works without the KGSL workarounds (emulated cross-process semaphores,
sync-file patch, glFinish fallbacks). The display stays on Meta's SDE driver + q2kmsd + the q2vrdisplay layer.

## What the Meta kernel already has (6e428607392d)

- `drivers/gpu/drm/msm` = mainline **4.19** drm/msm, unused (`# CONFIG_DRM_MSM is not set`; the display is Qualcomm's
  SDE driver, built as `msm_drm`, the GPU is KGSL).
- Its adreno code stops at **A630** (`adreno_device.c`: a3xx/a4xx/a5xx/A630). The A650 arrived in mainline ~5.9.
- `drivers/gpu/drm/drm_syncobj.c` exists — the sync-object core Turnip needs is there; only msm's submit path lacks it.

## Gap to mainline 5.10 (first LTS with A650 + MSM_SUBMIT_SYNCOBJ)

| file | 4.19 (Meta) | 5.10 | what changed |
|---|---|---|---|
| adreno/a6xx_gpu.c | 818 | 1235 | A650 hwcg/registers, UBWC config, zap shader, per-GPU tables |
| adreno/a6xx_gmu.c | 1217 | 1557 | A650 GMU bring-up, RSCC, power sequencing |
| adreno/a6xx_hfi.c | 435 | 598 | HFI v2 (A640/A650 firmware interface) |
| adreno/adreno_gpu.c | 741 | 969 | firmware loading, OPP/bus tables |
| msm_gem_submit.c | 596 | 831 | MSM_SUBMIT_SYNCOBJ_IN/OUT (Turnip) |

## Risks, highest first

1. **GPU-only operation beside SDE.** 4.19/5.10 msm wants to own the display (component master = MDP). Needs a
   headless/render-node-only build, and renamed symbols so it cannot clash with SDE's `msm_drm`.
2. **Interconnect.** 5.10 uses the interconnect (icc) framework; 4.19 has none (Meta has msm_bus). Stub/shim needed;
   the GMU firmware does most bus voting itself (HFI bw table).
3. **Hand-over from KGSL.** KGSL stays built in as the fallback; the port is a module bound at runtime after unbinding
   kgsl-3d0. GMU state left by KGSL is the likely trouble spot. A failed bring-up can hang the headset (watchdog →
   fastboot), so every test must be recoverable by reflashing boot.
4. **Preemption.** Our VR smoothness relies on KGSL ring priorities + preemption (tuprio 0,4,8,12). Mainline a6xx
   preemption is very recent (~6.13); without it VR may stutter under load.

## Findings (step 1)

- **GPU-only is already supported in 5.10.** `adreno_device.c` has `adreno_device_register_headless()` (made for i.MX5,
  triggered by the `amd,imageon` compatible): it registers a dummy `msm` platform device so the driver binds with no
  display (`kms = NULL`). For the Quest: always take that path. No display code is needed, nothing touches SDE.
- **5.10 already matches the KGSL node:** adreno's `dt_match` includes `qcom,kgsl-3d0`.
- 5.10's A650 entry: `ADRENO_REV(6,5,0,ANY_ID)`, `a650_sqe.fw`, `a650_gmu.bin`, `a650_zap.mdt`, GMEM 1 MiB + 128 KiB,
  `a650_hwcg`. All three firmware files are already staged to `/lib/firmware` at boot.

Headset device tree → what mainline expects:

| | headset (KGSL) | mainline 5.10 | work |
|---|---|---|---|
| GPU regs / irq | `kgsl_3d0_reg_memory`, `kgsl_3d0_irq` | same names | none |
| RSCC (A650) | `rscc` on the GPU node | `rscc` region | small |
| GMU regs | `kgsl_gmu_reg`, `kgsl_gmu_pdc_cfg`, `kgsl_gmu_pdc_seq` (node `qcom,gpu-gmu`) | `gmu`, `gmu_pdc`, `gmu_pdc_seq` | name table |
| GMU irqs | `kgsl_hfi_irq`, `kgsl_gmu_irq` | `hfi`, `gmu` | name table |
| power | regulators `vddcx` / `vdd` (GDSCs) | genpd `cx` / `gx` | **shim** (largest piece) |
| clocks | GMU: `gmu_clk cxo_clk axi_clk memnoc_clk gpu_cc_ahb apb_pclk`; GPU: `rbbmtimer_clk mem_clk mem_iface_clk gmu_clk gpu_cc_ahb l3_vote` | bulk get, any names | mostly fine |
| IOMMU | `qcom,kgsl-smmu-v2` @3da0000, context bank device `gfx3d_user` on `3da0000.kgsl-smmu` (iommu API) | an iommu domain for the GPU | attach to `gfx3d_user` |
| GPU speeds | `operating-points-v2` | OPP table | present |

## Steps

1. Gap list per file (A650 commits between 4.19 and 5.10).  ← structural part done (above); next: can KGSL be unbound
   at runtime (`/sys/bus/platform/drivers/kgsl-3d/unbind`), and which 5.10 core APIs are missing in 4.19.
   - KGSL has a remove handler (`drivers/gpu/msm/adreno.c`: `adreno_remove`, `.remove = adreno_remove`), so a runtime
     unbind is supported in principle.  Test it with SteamVR, Steam and the desktop's GPU warp stopped first
     (anything holding /dev/kgsl-3d0 open).  A hang ends in the watchdog → fastboot; `fastboot reboot` recovers.
   - Known 4.19 vs 5.10 API gaps to shim: `dma_resv` (4.19: `reservation_object`), interconnect `icc_*` (absent),
     `mmap_lock` (4.19: `mmap_sem`), `pm_runtime_resume_and_get`, `sysfs_emit`, `drm_gem_object_put` naming,
     genpd `cx`/`gx` → GDSC regulators.
2. Build the 5.10 GPU part as a separate GPU-only module against the 4.19 tree (compat shims), in GitHub Actions.
3. Runtime test: unbind KGSL, load the module, check `/dev/dri/renderD*`, run a Vulkan test with Turnip `msm`.
4. SteamVR on it; compare right-eye panel captures (q2vrshot) and SteamVR frame timing against KGSL.

Sources used locally: Meta kernel at 6e428607392d, torvalds/linux v5.10 (2c85ebc57b3e).

## Status (2026-10-28, ~14:00): preemption works (ring level)

Headset: `boot_steamos_desk359msm_test_flat.img`, `/var/lib/q2/q2msmargs` = `preempt=1`, `/var/lib/q2/deskres` = `1920x1080`.
Plain `reboot bootloader` works (desk-358).  Driver tests without reflashing: copy a module to `/var/lib/q2/q2msm_dev.ko`,
write options to `/var/lib/q2/q2msm_try` (one boot only), reboot.  Don't rmmod/insmod q2msm: the unload oopses in the
IOMMU/IRQ teardown (`arm_smmu_destroy_domain_context` → `free_irq`) and the testguard reboots.

What it took (all in `adreno/a6xx_preempt.c` / `a6xx_gpu.c`):
- ring 0's save addresses + a yield at GPU start (`a6xx_preempt_start`, KGSL `post_start`) - without them the CP
  ignores the switch request (stop bit stays set, ring 0 idle).  `pstart=2` adds the completion-irq bit (untested since
  the NULL `funcs->flush` crash was fixed; 1 is mainline's choice).
- the RPTR shadow zeroed at every hw_init (mainline does this): rings restart at 0 after a resume/recover, a stale
  shadow made an empty ring look busy, the CP switched onto it at position 0 and could never leave.
- `plevel=0` (switch only at the yield between submissions).  Level 1 (KGSL default) hangs rb2 mid-frame with SteamVR,
  GMU watchdog ~2 min in.  Tried (17:20–18:00):
  - a 192 KiB user state record per ring as NON_PRIV_SAVE_ADDR, KGSL's scratch slot scheme for the restore address
    (`CP_MEM_WRITE` record address before the IBs, 0 after, trigger reads it): **same hang** (still in the code,
    only active with `plevel=1`).
  - mainline a7xx's `CP_PREEMPT_ENABLE_LOCAL` (0x6a) and a5xx/KGSL's `CP_YIELD_ENABLE` (0x1c) before the IBs:
    both **CP opcode errors** on the a650 SQE firmware - removed.
  Left: no CP state at the hang (needs a crashdump / CP_SQE stat read), Meta's KGSL may set something we don't
  (the record's info / SMMU magic, CP_CHICKEN bits).  Parked - ring level already gives most of the gain.

Measured (SteamVR + Steam UI, 4 min windows): dropped 2 per ~80 s without → 2 per ~4 min with; compositor GPU max
11.7–12.4 ms either way; 1920x1080 Steam desktop costs nothing measurable.

Fence stall (retire_worker + crtc_commit in D for 90 s): fixed in desk-362 (2026-10-29), see CHANGES.md. It was a
deadlock between a struct_mutex holder waiting for another ring's fence and fence signalling that needed struct_mutex
(5.10 signals dma_fences only via retire_submits' dma_fence_is_signaled).  Compositor `flushall` is still required on
q2msm (right-eye glitches without it, checked by eye 2026-10-29).

A/B slot (fixed 17:14, user approved): slot A retries ran out after ~6 boots because nothing marked it successful →
fastboot.  boot_a (`/dev/sde7`) now has GPT attribute bit 54 (`sfdisk --part-attrs /dev/sde 7 GUID:48,49,50,52,53,54`,
backup of the table in `/var/lib/q2/sde-gpt-backup.txt` + first 24 KiB in `sde-gpt-head.bin`); the bootloader keeps
it across reboots.  `fastboot set_active a` and flashing boot_a clear it again - redo after either.

## Handoff (2026-10-28, ~12:10) - where the work stopped

Headset: `boot_steamos_desk357msm_test_flat.img` on boot_a (q2msm at boot, KGSL blacklisted, freedreno GPU stack, Steam UI
90 fps, pose-age HMD driver, q2-testguard).  To fastboot: `systemctl reboot --force bootloader` (plain `reboot bootloader`
hangs: ADB gadget teardown oops - fixed in init as desk-358, not in an image yet).  Build images with
`image/tools/inject_boot.py` then `image/tools/flatten_boot.py` (the boot partition is ~80 MB).

In init, not yet flashed: desk-358 ADB units masked; testguard fix (counts a task as stuck only if it isn't scheduled -
the first version rebooted on the display's crtc_commit thread).

Measured: SteamVR with Steam's UI running at steady state = 2 dropped frames per ~11 s, compositor GPU max ~12.4 ms,
worst frame interval 22 ms (a missed vsync); the Steam-stopped comparison was not finished.  Steam crashed once with
exit 139 on the desk357 boot (not investigated).

Preemption (head-tracking smoothness, "Meta's trick"): `adreno/a6xx_preempt.c` + hooks in `a6xx_gpu.c`, module param
`q2msm.preempt=1` (default 0 = unchanged single ring).  Built (run 40722305797, q2msm.ko 266656 bytes), NOT tested yet.
Test: load with `Q2ARGS=preempt=1` (q2msm-retry flow) or add `options` to the boot load, check
"q2msm: preemption on, 4 rings", q2vktest PASS, then SteamVR timing vs preempt=0.

Still to do (user's list): smoother head tracking (preemption test), desktop/Steam VR desktop at higher resolution
(`/var/lib/q2/deskres` 1920x1080 vs 1280x720, measure) while keeping native render resolution, faster overall.

## Status update (2026-10-28, evening): q2msm from boot

Image `boot_steamos_desk354msm_test.img` = desk-352 test + `initcall_blacklist=kgsl_3d_init` + `/usr/lib/q2/q2msm.ko`,
loaded by q2desktop right after q2fw (before the GL probe).  On the headset: q2msm comes up at boot (renderD129),
q2vktest PASS, runtime suspend/resume PASS, desktop GL = zink on Turnip/msm, fastfetch shows
"Qualcomm Turnip Adreno (TM) 650".  Zero-copy desktop worked on q2msm (dma-buf import from SDE) once
`DRIVER_PRIME` + `.gem_prime_import/export` were set (4.19 needs both).

Also fixed today: free-worker teardown order (flush before drm_dev_put), GPU runtime-PM double disable, GPU RSC woken
on a fresh driver instance when a previous one left it asleep (`GMU_RSCC_CONTROL_ACK` bit 0) - this was the real cause
of "GmuPwrGxBwVote" after KGSL's unbind.

Open:
- zero-copy at boot: q2kmsd answers BUSY (blitter still holds the scanout buffers) - q2display ordering, not q2msm.
- Steam UI runs software GL (CEF "runtime detect" / cefmode) - unchanged by q2msm.
- SteamVR on q2msm: untested (compositor TU_DEBUG/sync paths were tuned for KGSL).
- module reload (dev only): resume after a reload can fail; fences on q2kmsd buffers outlive the module.
- MSM_PARAM_HIGHEST_BANK_BIT / UBWC_SWIZZLE / MACROTILE_MODE and GEM_INFO set-metadata (newer uapi) not provided.

## Status update (2026-10-28, later): the GPU renders on q2msm

With KGSL kept from probing (`initcall_blacklist=kgsl_3d_init`, test image `boot_steamos_desk352_test_nokgsl.img`),
q2msm brings the A650 up and `kernel/q2msm/test/q2vktest` (offscreen fill / compute / triangle, read back and checked)
passes bit-exact through Turnip's msm backend.  GPU power collapse + resume works.

What made it work:
- **No runtime hand-over from KGSL.** After `unbind`, KGSL leaves the GPU power path in a state where the GMU rejects
  every GX vote (`GMU ERROR 115 184 "GmuPwrGxBwVote"` in the HFI debug queue).  On a GPU KGSL never touched it works.
  So q2msm has to own the GPU from boot.
- HFI as KGSL does it for this firmware: debug queue (idx 2, prio 40) present, `START` after the first
  `GX_BW_PERF_VOTE` under the GPU OOB, ACD table + ACD/LM feature control + `LM_CS0` slope before `CORE_FW_START`,
  perf table with KGSL's extra 0 Hz entry, `bw = 0xff` on the vote, bw table from msm-bus (`qcom,gpu-bus-table-ddr7`
  + the GMU node's CNOC table), PDC left to the AOP (`pdc_in_aop`).
- SMMU context-bank devices (`gfx3d_user`, `gmu_kernel`) created by q2msm when KGSL has not populated them, wired to
  the SMMU with `of_dma_configure`.
- Zap shader loaded through PIL (`subsystem_get("a650_zap")`), like KGSL.
- `MSM_SUBMIT_BO_NO_IMPLICIT` (5.17 uapi) accepted - Mesa 26's Turnip sets it.

Next: boot integration (blacklist KGSL + load q2msm from init), then q2blit / desktop / q2kmsd / SteamVR on msm
(render on q2msm, scanout on SDE via dma-buf), then performance (devfreq, 90 Hz frame times, no preemption).

## Status (2026-10-28, branch q2msm)

`kernel/q2msm` builds as `q2msm.ko` (build-q2msm workflow) and loads on the running kernel (vermagic matches; all
imports exported).  Hand-over on the headset (`q2msm-retry.sh` flow): stop q2-vr/q2-desktop, `fuser -k /dev/kgsl-3d0`,
unbind kgsl-3d, insmod, bind `q2msm-adreno`.

Works:
- `[drm] Initialized msm 1.6.0 for q2msm on minor 1` → `/dev/dri/renderD129` beside SDE's card0/renderD128.
- Turnip enumerates "Turnip Adreno (TM) 650" on it.
- GPU iommu domain on KGSL's `gfx3d_user` context bank (KGSL depopulates it on unbind; q2msm recreates it and runs
  `of_dma_configure` by hand), GMU domain on `gmu_kernel`.
- KGSL leaks `kgsl_hfi_irq`/`kgsl_gmu_irq` on unbind; q2msm frees them (`q2_free_kgsl_irq`).
- GMU firmware boots, HFI init/perf table/bw table/core fw start/start are acked.

Fails: the first `HFI_H2F_MSG_GX_BW_PERF_VOTE` times out, then `OOB GPU_SET` → the GMU never powers GX.
Ruled out: register windows (identical to mainline sm8250), RPMh levels (opp-microvolt = RPMh corners, matches
gfx.lvl), CN0 BCM address (XR2 has CN0 at 0x50074, not 0x500a4 — fixed via cmd-db, did not change the result).
Next suspects: the GMU firmware version/HFI layout Meta ships vs mainline's (compare with KGSL's hfi.c/dcvs table),
GX GDSC handling, ACD/feature-ctrl messages KGSL sends before the first vote.

Known teardown bug: after a failed hw init the msm workqueue never drains (5.10 runs GPU recovery on priv->wq), so
unbind/rmmod hangs → reboot between tests (sysrq b).
