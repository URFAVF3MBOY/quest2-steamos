/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2017, 2019 The Linux Foundation. All rights reserved. */

#ifndef __A6XX_GPU_H__
#define __A6XX_GPU_H__


#include "adreno_gpu.h"
#include "a6xx.xml.h"

#include "a6xx_gmu.h"

extern bool hang_debug;

struct a6xx_gpu {
	struct adreno_gpu base;

	struct drm_gem_object *sqe_bo;
	uint64_t sqe_iova;

	struct msm_ringbuffer *cur_ring;
	struct msm_file_private *cur_ctx;

	struct a6xx_gmu gmu;

	struct drm_gem_object *shadow_bo;
	uint64_t shadow_iova;
	uint32_t *shadow;

	bool has_whereami;

	/* q2msm: ring preemption (a6xx_preempt.c) */
	struct msm_ringbuffer *next_ring;
	struct drm_gem_object *preempt_bo[MSM_GPU_MAX_RINGS];
	struct a6xx_preempt_record *preempt[MSM_GPU_MAX_RINGS];
	uint64_t preempt_iova[MSM_GPU_MAX_RINGS];
	struct drm_gem_object *preempt_secure_bo[MSM_GPU_MAX_RINGS];
	uint64_t preempt_secure_iova[MSM_GPU_MAX_RINGS];
	struct drm_gem_object *preempt_counter_bo[MSM_GPU_MAX_RINGS];
	uint64_t preempt_counter_iova[MSM_GPU_MAX_RINGS];
	/* draw-call level: user state record per ring + scratch (per ring: the record of the submission in flight) */
	struct drm_gem_object *preempt_user_bo[MSM_GPU_MAX_RINGS];
	uint64_t preempt_user_iova[MSM_GPU_MAX_RINGS];
	struct drm_gem_object *preempt_scratch_bo;
	uint64_t *preempt_scratch;
	uint64_t preempt_scratch_iova;
	uint32_t last_seqno[MSM_GPU_MAX_RINGS];
	atomic_t preempt_state;
	spinlock_t eval_lock;
	struct timer_list preempt_timer;
	unsigned int preempt_level;
	bool uses_gmem;
	bool skip_save_restore;
	unsigned long preempt_count;
};

#define to_a6xx_gpu(x) container_of(x, struct a6xx_gpu, base)

/*
 * q2msm: preemption.  State machine as in mainline (lockless):
 * NONE -> START (trigger evaluating) -> TRIGGERED (CP told to switch) -> PENDING (irq) -> FINISH -> NONE,
 * FAULTED on a timeout or a failed switch (recovery).
 */
enum a6xx_preempt_state {
	PREEMPT_NONE = 0,
	PREEMPT_START,
	PREEMPT_FINISH,
	PREEMPT_TRIGGERED,
	PREEMPT_FAULTED,
	PREEMPT_PENDING,
};

/* CP context record (KGSL struct a6xx_cp_preemption_record) */
struct a6xx_preempt_record {
	u32 magic;
	u32 info;
	u32 errno;
	u32 data;
	u32 cntl;
	u32 rptr;
	u32 wptr;
	u32 _pad;
	u64 rptr_addr;
	u64 rbase;
	u64 counter;
};

#define A6XX_PREEMPT_RECORD_MAGIC 0xAE399D6EUL

/* CP_CONTEXT_SWITCH_CNTL bits (KGSL a6xx_preemption_trigger) */
#define A6XX_CP_CONTEXT_SWITCH_CNTL_STOP		BIT(0)
#define A6XX_CP_CONTEXT_SWITCH_CNTL_LEVEL(x)		(((x) & 0x3) << 6)
#define A6XX_CP_CONTEXT_SWITCH_CNTL_USES_GMEM		BIT(8)
#define A6XX_CP_CONTEXT_SWITCH_CNTL_SKIP_SAVE_RESTORE	BIT(9)

/* CP_SET_PSEUDO_REG register ids */
#define Q2_PSEUDO_SMMU_INFO		0
#define Q2_PSEUDO_NON_SECURE_SAVE_ADDR	1
#define Q2_PSEUDO_SECURE_SAVE_ADDR	2
#define Q2_PSEUDO_NON_PRIV_SAVE_ADDR	3
#define Q2_PSEUDO_COUNTER		4

static inline bool a6xx_in_preempt(struct a6xx_gpu *a6xx_gpu)
{
	int preempt_state;

	/* order the state read against the ring reads that follow */
	smp_rmb();
	preempt_state = atomic_read(&a6xx_gpu->preempt_state);
	return !(preempt_state == PREEMPT_NONE || preempt_state == PREEMPT_FINISH);
}

void a6xx_preempt_init(struct msm_gpu *gpu);
void a6xx_preempt_hw_init(struct msm_gpu *gpu);
int a6xx_preempt_start(struct msm_gpu *gpu);
void a6xx_flush(struct msm_gpu *gpu, struct msm_ringbuffer *ring);
void a6xx_preempt_trigger(struct msm_gpu *gpu);
void a6xx_preempt_irq(struct msm_gpu *gpu);
void a6xx_preempt_fini(struct msm_gpu *gpu);
void a6xx_preempt_emit_pseudo_regs(struct msm_gpu *gpu, struct msm_ringbuffer *ring);
void a6xx_preempt_emit_yield(struct msm_gpu *gpu, struct msm_ringbuffer *ring);
extern int q2_preempt;

/*
 * Given a register and a count, return a value to program into
 * REG_CP_PROTECT_REG(n) - this will block both reads and writes for _len
 * registers starting at _reg.
 */
#define A6XX_PROTECT_RW(_reg, _len) \
	((1 << 31) | \
	(((_len) & 0x3FFF) << 18) | ((_reg) & 0x3FFFF))

/*
 * Same as above, but allow reads over the range. For areas of mixed use (such
 * as performance counters) this allows us to protect a much larger range with a
 * single register
 */
#define A6XX_PROTECT_RDONLY(_reg, _len) \
	((((_len) & 0x3FFF) << 18) | ((_reg) & 0x3FFFF))

static inline bool a6xx_has_gbif(struct adreno_gpu *gpu)
{
	if(adreno_is_a630(gpu))
		return false;

	return true;
}

#define shadowptr(_a6xx_gpu, _ring) ((_a6xx_gpu)->shadow_iova + \
		((_ring)->id * sizeof(uint32_t)))

int a6xx_gmu_resume(struct a6xx_gpu *gpu);
int a6xx_gmu_stop(struct a6xx_gpu *gpu);

int a6xx_gmu_wait_for_idle(struct a6xx_gmu *gmu);

bool a6xx_gmu_isidle(struct a6xx_gmu *gmu);

int a6xx_gmu_set_oob(struct a6xx_gmu *gmu, enum a6xx_gmu_oob_state state);
void a6xx_gmu_clear_oob(struct a6xx_gmu *gmu, enum a6xx_gmu_oob_state state);

int a6xx_gmu_init(struct a6xx_gpu *a6xx_gpu, struct device_node *node);
void a6xx_gmu_remove(struct a6xx_gpu *a6xx_gpu);

void a6xx_gmu_set_freq(struct msm_gpu *gpu, struct dev_pm_opp *opp);
unsigned long a6xx_gmu_get_freq(struct msm_gpu *gpu);

void a6xx_show(struct msm_gpu *gpu, struct msm_gpu_state *state,
		struct drm_printer *p);

struct msm_gpu_state *a6xx_gpu_state_get(struct msm_gpu *gpu);
int a6xx_gpu_state_put(struct msm_gpu_state *state);

#endif /* __A6XX_GPU_H__ */
