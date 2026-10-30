// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2018, The Linux Foundation. All rights reserved. */
/* Copyright (c) 2023 Collabora, Ltd. */
/* Copyright (c) 2024 Valve Corporation */
/*
 * q2msm: ring preemption for the A650 (Quest 2), so SteamVR's late warp (a high-priority queue) can interrupt other
 * GPU work instead of waiting behind it.  The state machine is mainline's (v6.13 a6xx_preempt.c, written for a7xx);
 * the per-ring records and the packets around each submission follow what KGSL does on this GPU and firmware
 * (adreno_a6xx_preempt.c): a privileged context record of GMEM + 0x110000 bytes, a "secure" record of the same size
 * (never used - we stay in non-secure mode - but the CP gets a real address), a 4 KiB perfcounter save area, and
 * CP_CONTEXT_SWITCH_YIELD after every submission.  One pagetable for everything, so no SMMU switching (info = 0).
 * Off unless q2msm.preempt=1.
 */

#include "msm_gem.h"
#include "a6xx_gpu.h"
#include "a6xx_gmu.xml.h"
#include "msm_mmu.h"

/*
 * q2msm.plevel: 0 = switch only at the yield after a submission (no user state to save), 1 = KGSL's draw-call level:
 * the CP also saves the user state of the submission it stops in, to the ring's user record (NON_PRIV_SAVE_ADDR).
 * As in KGSL, each submission writes its record address to the ring's scratch slot before its IBs and clears it after,
 * and the trigger hands the slot to the CP as the restore address (0 = stopped at a yield, nothing to restore).
 * A ring always resumes its own stopped submission first, so one record per ring does for KGSL's one per context.
 * Read at every trigger / submission.
 */
static int q2_plevel;
module_param_named(plevel, q2_plevel, int, 0644);

/* Try to move the preemption state from old to new; false if it wasn't 'old' */
static inline bool try_preempt_state(struct a6xx_gpu *a6xx_gpu,
		enum a6xx_preempt_state old, enum a6xx_preempt_state new)
{
	enum a6xx_preempt_state cur = atomic_cmpxchg(&a6xx_gpu->preempt_state,
		old, new);

	return (cur == old);
}

/* Force the state (used where the current state is known and can't change) */
static inline void set_preempt_state(struct a6xx_gpu *gpu,
		enum a6xx_preempt_state new)
{
	smp_mb__before_atomic();
	atomic_set(&gpu->preempt_state, new);
	smp_mb__after_atomic();
}

/* Write the most recent wptr for the given ring into the hardware */
static inline void update_wptr(struct msm_gpu *gpu, struct msm_ringbuffer *ring)
{
	unsigned long flags;
	uint32_t wptr;

	spin_lock_irqsave(&ring->lock, flags);

	if (ring->restore_wptr) {
		wptr = get_wptr(ring);

		gpu_write(gpu, REG_A6XX_CP_RB_WPTR, wptr);

		ring->restore_wptr = false;
	}

	spin_unlock_irqrestore(&ring->lock, flags);
}

/* Return the highest priority ringbuffer with something in it */
static struct msm_ringbuffer *get_next_ring(struct msm_gpu *gpu)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);
	unsigned long flags;
	int i;

	for (i = 0; i < gpu->nr_rings; i++) {
		bool empty;
		struct msm_ringbuffer *ring = gpu->rb[i];

		spin_lock_irqsave(&ring->lock, flags);
		empty = (get_wptr(ring) == gpu->funcs->get_rptr(gpu, ring));
		if (!empty && ring == a6xx_gpu->cur_ring)
			empty = ring->memptrs->fence == a6xx_gpu->last_seqno[i];
		spin_unlock_irqrestore(&ring->lock, flags);

		if (!empty)
			return ring;
	}

	return NULL;
}

static void a6xx_preempt_timer(struct timer_list *t)
{
	struct a6xx_gpu *a6xx_gpu = from_timer(a6xx_gpu, t, preempt_timer);
	struct msm_gpu *gpu = &a6xx_gpu->base.base;
	struct msm_drm_private *priv = gpu->dev->dev_private;

	if (!try_preempt_state(a6xx_gpu, PREEMPT_TRIGGERED, PREEMPT_FAULTED))
		return;

	dev_err(gpu->dev->dev, "%s: preemption timed out after %lu switches: ring %d -> %d, cntl %08x rptr %x wptr %x status %08x irq %08x fault %08x\n",
		gpu->name, a6xx_gpu->preempt_count, a6xx_gpu->cur_ring ? a6xx_gpu->cur_ring->id : -1,
		a6xx_gpu->next_ring ? a6xx_gpu->next_ring->id : -1,
		gpu_read(gpu, REG_A6XX_CP_CONTEXT_SWITCH_CNTL), gpu_read(gpu, REG_A6XX_CP_RB_RPTR),
		gpu_read(gpu, REG_A6XX_CP_RB_WPTR), gpu_read(gpu, REG_A6XX_RBBM_STATUS),
		gpu_read(gpu, REG_A6XX_RBBM_INT_0_STATUS), gpu_read(gpu, REG_A6XX_CP_HW_FAULT));
	queue_work(priv->wq, &gpu->recover_work);
}

void a6xx_preempt_irq(struct msm_gpu *gpu)
{
	uint32_t status;
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);
	struct msm_drm_private *priv = gpu->dev->dev_private;

	if (!try_preempt_state(a6xx_gpu, PREEMPT_TRIGGERED, PREEMPT_PENDING))
		return;

	/* Delete the preemption watchdog timer */
	del_timer(&a6xx_gpu->preempt_timer);

	/*
	 * The hardware clears the stop bit of CP_CONTEXT_SWITCH_CNTL before firing the interrupt; if it is still set,
	 * something went wrong - recover.
	 */
	status = gpu_read(gpu, REG_A6XX_CP_CONTEXT_SWITCH_CNTL);
	if (unlikely(status & A6XX_CP_CONTEXT_SWITCH_CNTL_STOP)) {
		set_preempt_state(a6xx_gpu, PREEMPT_FAULTED);
		dev_err(gpu->dev->dev, "%s: preemption failed to complete (cntl %08x)\n", gpu->name, status);
		queue_work(priv->wq, &gpu->recover_work);
		return;
	}

	a6xx_gpu->cur_ring = a6xx_gpu->next_ring;
	a6xx_gpu->next_ring = NULL;
	a6xx_gpu->preempt_count++;

	set_preempt_state(a6xx_gpu, PREEMPT_FINISH);

	update_wptr(gpu, a6xx_gpu->cur_ring);

	set_preempt_state(a6xx_gpu, PREEMPT_NONE);

	/* Retrigger, in case a preemption was skipped because one was already in flight */
	a6xx_preempt_trigger(gpu);
}

void a6xx_preempt_hw_init(struct msm_gpu *gpu)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);
	int i;

	/* No preemption if we only have one ring */
	if (gpu->nr_rings == 1)
		return;

	for (i = 0; i < gpu->nr_rings; i++) {
		struct a6xx_preempt_record *record_ptr = a6xx_gpu->preempt[i];

		record_ptr->wptr = 0;
		record_ptr->rptr = 0;
		record_ptr->rptr_addr = shadowptr(a6xx_gpu, gpu->rb[i]);
		record_ptr->info = 0;
		record_ptr->data = 0;
		record_ptr->rbase = gpu->rb[i]->iova;
		gpu->rb[i]->restore_wptr = false;
		/* rings start empty: no stopped submission to restore */
		a6xx_gpu->preempt_scratch[i] = 0;
	}

	/* 0 = we don't switch pagetables (one address space for everything) */
	gpu_write64(gpu, REG_A6XX_CP_CONTEXT_SWITCH_SMMU_INFO_LO,
		REG_A6XX_CP_CONTEXT_SWITCH_SMMU_INFO_HI, 0);

	/* Enable the GMEM save/restore feature for preemption */
	gpu_write(gpu, REG_A6XX_RB_CONTEXT_SWITCH_GMEM_SAVE_RESTORE, 0x1);

	/* Reset the preemption state */
	set_preempt_state(a6xx_gpu, PREEMPT_NONE);

	spin_lock_init(&a6xx_gpu->eval_lock);

	/* Always come up on rb 0 */
	a6xx_gpu->cur_ring = gpu->rb[0];
	a6xx_gpu->next_ring = NULL;
}

void a6xx_preempt_trigger(struct msm_gpu *gpu)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);
	unsigned long flags;
	struct msm_ringbuffer *ring;
	unsigned int cntl;

	if (gpu->nr_rings == 1)
		return;

	/*
	 * Lock so that another thread attempting preemption doesn't skip it while we are still evaluating the next
	 * ring: the other thread then starts preemption if we abort it.
	 */
	spin_lock_irqsave(&a6xx_gpu->eval_lock, flags);

	/* NONE -> START, or a preemption is already in flight */
	if (!try_preempt_state(a6xx_gpu, PREEMPT_NONE, PREEMPT_START)) {
		spin_unlock_irqrestore(&a6xx_gpu->eval_lock, flags);
		return;
	}

	/* KGSL: (level << 6) | stop, + skip save/restore (bit 9), + GMEM save/restore (bit 8) */
	cntl = A6XX_CP_CONTEXT_SWITCH_CNTL_LEVEL(q2_plevel);
	if (a6xx_gpu->skip_save_restore)
		cntl |= A6XX_CP_CONTEXT_SWITCH_CNTL_SKIP_SAVE_RESTORE;
	if (a6xx_gpu->uses_gmem)
		cntl |= A6XX_CP_CONTEXT_SWITCH_CNTL_USES_GMEM;
	cntl |= A6XX_CP_CONTEXT_SWITCH_CNTL_STOP;

	ring = get_next_ring(gpu);

	/* Nothing to switch to: just bring the current ring's wptr up to date */
	if (!ring || (a6xx_gpu->cur_ring == ring)) {
		set_preempt_state(a6xx_gpu, PREEMPT_FINISH);
		update_wptr(gpu, a6xx_gpu->cur_ring);
		set_preempt_state(a6xx_gpu, PREEMPT_NONE);
		spin_unlock_irqrestore(&a6xx_gpu->eval_lock, flags);
		return;
	}

	spin_unlock_irqrestore(&a6xx_gpu->eval_lock, flags);

	spin_lock_irqsave(&ring->lock, flags);
	a6xx_gpu->preempt[ring->id]->wptr = get_wptr(ring);
	/* the CP writes the wptr we set above when it switches in: don't write it a second time */
	ring->restore_wptr = false;
	spin_unlock_irqrestore(&ring->lock, flags);

	/* make the record writes visible before the CP reads them */
	wmb();

	gpu_write64(gpu, REG_A6XX_CP_CONTEXT_SWITCH_PRIV_NON_SECURE_RESTORE_ADDR_LO,
		REG_A6XX_CP_CONTEXT_SWITCH_PRIV_NON_SECURE_RESTORE_ADDR_HI,
		a6xx_gpu->preempt_iova[ring->id]);
	gpu_write64(gpu, REG_A6XX_CP_CONTEXT_SWITCH_PRIV_SECURE_RESTORE_ADDR_LO,
		REG_A6XX_CP_CONTEXT_SWITCH_PRIV_SECURE_RESTORE_ADDR_HI,
		a6xx_gpu->preempt_secure_iova[ring->id]);
	gpu_write64(gpu, REG_A6XX_CP_CONTEXT_SWITCH_NON_PRIV_RESTORE_ADDR_LO,
		REG_A6XX_CP_CONTEXT_SWITCH_NON_PRIV_RESTORE_ADDR_HI,
		q2_plevel ? READ_ONCE(a6xx_gpu->preempt_scratch[ring->id]) : 0);

	a6xx_gpu->next_ring = ring;

	if (a6xx_gpu->preempt_count < 3)
		dev_info(gpu->dev->dev, "q2msm: preempt ring %d -> %d, cntl %08x\n", a6xx_gpu->cur_ring->id, ring->id, cntl);

	/* Catch a stuck preemption */
	mod_timer(&a6xx_gpu->preempt_timer, jiffies + msecs_to_jiffies(10000));

	set_preempt_state(a6xx_gpu, PREEMPT_TRIGGERED);

	/* Trigger the preemption */
	gpu_write(gpu, REG_A6XX_CP_CONTEXT_SWITCH_CNTL, cntl);
}

/* The ring's scratch slot = the user record to restore if it gets stopped from here on */
static void preempt_emit_scratch(struct a6xx_gpu *a6xx_gpu, struct msm_ringbuffer *ring, u64 val)
{
	u64 dest = a6xx_gpu->preempt_scratch_iova + ring->id * sizeof(u64);

	OUT_PKT7(ring, CP_MEM_WRITE, 4);
	OUT_RING(ring, CP_MEM_WRITE_0_ADDR_LO(lower_32_bits(dest)));
	OUT_RING(ring, CP_MEM_WRITE_1_ADDR_HI(upper_32_bits(dest)));
	OUT_RING(ring, lower_32_bits(val));
	OUT_RING(ring, upper_32_bits(val));
}

/* CP_SET_PSEUDO_REG before a submission: where the CP saves this ring's state when it gets preempted */
void a6xx_preempt_emit_pseudo_regs(struct msm_gpu *gpu, struct msm_ringbuffer *ring)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);

	OUT_PKT7(ring, CP_SET_PSEUDO_REG, 12);
	/* SMMU info: none, the kernel tracks it (one pagetable) */
	OUT_RING(ring, Q2_PSEUDO_SMMU_INFO);
	OUT_RING(ring, 0);
	OUT_RING(ring, 0);
	OUT_RING(ring, Q2_PSEUDO_NON_SECURE_SAVE_ADDR);
	OUT_RING(ring, lower_32_bits(a6xx_gpu->preempt_iova[ring->id]));
	OUT_RING(ring, upper_32_bits(a6xx_gpu->preempt_iova[ring->id]));
	OUT_RING(ring, Q2_PSEUDO_SECURE_SAVE_ADDR);
	OUT_RING(ring, lower_32_bits(a6xx_gpu->preempt_secure_iova[ring->id]));
	OUT_RING(ring, upper_32_bits(a6xx_gpu->preempt_secure_iova[ring->id]));
	OUT_RING(ring, Q2_PSEUDO_COUNTER);
	OUT_RING(ring, lower_32_bits(a6xx_gpu->preempt_counter_iova[ring->id]));
	OUT_RING(ring, upper_32_bits(a6xx_gpu->preempt_counter_iova[ring->id]));

	if (!q2_plevel)
		return;

	OUT_PKT7(ring, CP_SET_PSEUDO_REG, 3);
	OUT_RING(ring, Q2_PSEUDO_NON_PRIV_SAVE_ADDR);
	OUT_RING(ring, lower_32_bits(a6xx_gpu->preempt_user_iova[ring->id]));
	OUT_RING(ring, upper_32_bits(a6xx_gpu->preempt_user_iova[ring->id]));

	preempt_emit_scratch(a6xx_gpu, ring, a6xx_gpu->preempt_user_iova[ring->id]);
}


/*
 * Once after CP_ME_INIT (KGSL a6xx_post_start, mainline a7xx_preempt_start): ring 0's save addresses and a first
 * yield point, so the CP has a context to save from the first switch on.
 */
/* q2msm.pstart: 0 = no start packets, 1 = pseudo regs + yield (mainline), 2 = yield with the completion irq bit (KGSL) */
static int q2_pstart = 1;
module_param_named(pstart, q2_pstart, int, 0644);

int a6xx_preempt_start(struct msm_gpu *gpu)
{
	struct msm_ringbuffer *ring = gpu->rb[0];
	bool idle;

	if (gpu->nr_rings <= 1 || !q2_pstart)
		return 0;

	dev_info(gpu->dev->dev, "q2msm: preempt start %d\n", q2_pstart);

	a6xx_preempt_emit_pseudo_regs(gpu, ring);

	OUT_PKT7(ring, CP_CONTEXT_SWITCH_YIELD, 4);
	OUT_RING(ring, 0x00);
	OUT_RING(ring, 0x00);
	OUT_RING(ring, 0x00);
	/* generate an interrupt on preemption completion */
	OUT_RING(ring, q2_pstart == 2 ? 0x01 : 0x00);

	a6xx_flush(gpu, ring);
	idle = adreno_idle(gpu, ring);
	dev_info(gpu->dev->dev, "q2msm: preempt start %s, rptr %x wptr %x status %08x\n", idle ? "done" : "TIMED OUT",
		gpu_read(gpu, REG_A6XX_CP_RB_RPTR), gpu_read(gpu, REG_A6XX_CP_RB_WPTR),
		gpu_read(gpu, REG_A6XX_RBBM_STATUS));
	return idle ? 0 : -EINVAL;
}

/* After a submission: let a higher priority ring in here */
void a6xx_preempt_emit_yield(struct msm_gpu *gpu, struct msm_ringbuffer *ring)
{
	/* submission done: nothing of it left to restore */
	if (q2_plevel)
		preempt_emit_scratch(to_a6xx_gpu(to_adreno_gpu(gpu)), ring, 0);

	OUT_PKT7(ring, CP_CONTEXT_SWITCH_YIELD, 4);
	/* dword 1-2: an address the CP writes dword 3 to on preemption - 0 = none */
	OUT_RING(ring, 0x00);
	OUT_RING(ring, 0x00);
	OUT_RING(ring, 0x01);
	/* KGSL: 0 */
	OUT_RING(ring, 0x00);
}

static void *preempt_bo_flags(struct msm_gpu *gpu, u32 size, u32 flags, struct drm_gem_object **bo, u64 *iova,
		const char *what, int id)
{
	void *ptr = msm_gem_kernel_new(gpu->dev, size, flags, gpu->aspace, bo, iova);

	if (IS_ERR(ptr))
		return ptr;
	msm_gem_object_set_name(*bo, "%s ring%d", what, id);
	return ptr;
}

static void *preempt_bo(struct msm_gpu *gpu, u32 size, struct drm_gem_object **bo, u64 *iova,
		const char *what, int id)
{
	return preempt_bo_flags(gpu, size, MSM_BO_WC | MSM_BO_MAP_PRIV, bo, iova, what, id);
}

/* KGSL A6XX_CP_CTXRECORD_USER_RESTORE_SIZE */
#define Q2_PREEMPT_USER_SIZE	(192 * 1024)

static int preempt_init_ring(struct a6xx_gpu *a6xx_gpu, struct msm_ringbuffer *ring)
{
	struct adreno_gpu *adreno_gpu = &a6xx_gpu->base;
	struct msm_gpu *gpu = &adreno_gpu->base;
	/* KGSL: GMEM size + GPU HW state (0x110000) */
	u32 size = adreno_gpu->gmem + 0x110000;
	struct a6xx_preempt_record *record_ptr;
	void *ptr;

	ptr = preempt_bo(gpu, size, &a6xx_gpu->preempt_bo[ring->id], &a6xx_gpu->preempt_iova[ring->id],
		"preempt_record", ring->id);
	if (IS_ERR(ptr))
		return PTR_ERR(ptr);
	memset(ptr, 0, PAGE_SIZE);
	a6xx_gpu->preempt[ring->id] = ptr;
	record_ptr = ptr;

	ptr = preempt_bo(gpu, size, &a6xx_gpu->preempt_secure_bo[ring->id],
		&a6xx_gpu->preempt_secure_iova[ring->id], "preempt_secure", ring->id);
	if (IS_ERR(ptr))
		return PTR_ERR(ptr);

	ptr = preempt_bo(gpu, 4096, &a6xx_gpu->preempt_counter_bo[ring->id],
		&a6xx_gpu->preempt_counter_iova[ring->id], "preempt_counters", ring->id);
	if (IS_ERR(ptr))
		return PTR_ERR(ptr);
	memset(ptr, 0, 4096);

	/* user state: written by the CP in user mode, so not a privileged mapping (KGSL: in the context's pagetable) */
	ptr = preempt_bo_flags(gpu, Q2_PREEMPT_USER_SIZE, MSM_BO_WC, &a6xx_gpu->preempt_user_bo[ring->id],
		&a6xx_gpu->preempt_user_iova[ring->id], "preempt_user", ring->id);
	if (IS_ERR(ptr))
		return PTR_ERR(ptr);
	memset(ptr, 0, PAGE_SIZE);

	/* Defaults for the record (KGSL a6xx_preemption_ringbuffer_init) */
	record_ptr->magic = A6XX_PREEMPT_RECORD_MAGIC;
	record_ptr->info = 0;
	record_ptr->errno = 0;
	record_ptr->data = 0;
	record_ptr->rptr = 0;
	record_ptr->wptr = 0;
	record_ptr->cntl = MSM_GPU_RB_CNTL_DEFAULT;
	record_ptr->rbase = ring->iova;
	record_ptr->counter = 0;

	return 0;
}

void a6xx_preempt_fini(struct msm_gpu *gpu)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);
	int i;

	for (i = 0; i < gpu->nr_rings; i++) {
		msm_gem_kernel_put(a6xx_gpu->preempt_bo[i], gpu->aspace, false);
		msm_gem_kernel_put(a6xx_gpu->preempt_secure_bo[i], gpu->aspace, false);
		msm_gem_kernel_put(a6xx_gpu->preempt_counter_bo[i], gpu->aspace, false);
		msm_gem_kernel_put(a6xx_gpu->preempt_user_bo[i], gpu->aspace, false);
		a6xx_gpu->preempt_bo[i] = a6xx_gpu->preempt_secure_bo[i] = a6xx_gpu->preempt_counter_bo[i] = NULL;
		a6xx_gpu->preempt_user_bo[i] = NULL;
	}
	msm_gem_kernel_put(a6xx_gpu->preempt_scratch_bo, gpu->aspace, false);
	a6xx_gpu->preempt_scratch_bo = NULL;
}

void a6xx_preempt_init(struct msm_gpu *gpu)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);
	int i;

	/* No preemption if we only have one ring */
	if (gpu->nr_rings <= 1)
		return;

	a6xx_gpu->preempt_scratch = preempt_bo(gpu, PAGE_SIZE, &a6xx_gpu->preempt_scratch_bo,
		&a6xx_gpu->preempt_scratch_iova, "preempt_scratch", 0);
	if (IS_ERR(a6xx_gpu->preempt_scratch))
		goto fail;
	memset(a6xx_gpu->preempt_scratch, 0, PAGE_SIZE);

	for (i = 0; i < gpu->nr_rings; i++) {
		if (preempt_init_ring(a6xx_gpu, gpu->rb[i]))
			goto fail;
	}

	/* mainline / KGSL defaults */
	a6xx_gpu->preempt_level = 1;
	a6xx_gpu->uses_gmem = 1;
	a6xx_gpu->skip_save_restore = 1;

	timer_setup(&a6xx_gpu->preempt_timer, a6xx_preempt_timer, 0);

	DRM_DEV_INFO(&gpu->pdev->dev, "q2msm: preemption on, %d rings\n", gpu->nr_rings);
	return;
fail:
	/* On any failure: clean up and fall back to one ring, no preemption */
	a6xx_preempt_fini(gpu);
	gpu->nr_rings = 1;

	DRM_DEV_ERROR(&gpu->pdev->dev, "preemption init failed, disabling preemption\n");
}
