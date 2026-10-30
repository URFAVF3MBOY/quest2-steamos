/* SPDX-License-Identifier: GPL-2.0-only */
/* q2msm: force-included into every file.  Small 5.x APIs the 5.10 driver uses that the 4.19 kernel lacks. */
#ifndef __Q2COMPAT_H__
#define __Q2COMPAT_H__

#include <linux/types.h>
#include <linux/err.h>

struct device;
struct dev_pm_opp;
struct clk_bulk_data;
struct drm_gem_object;

/* 5.10 drm_gem_object_put_locked() is 4.19's drm_gem_object_put() (struct_mutex held) */
void drm_gem_object_put(struct drm_gem_object *obj);
#define q2_gem_put_locked(obj) drm_gem_object_put(obj)

/* no timeline syncobjs on 4.19: points are rejected with -EOPNOTSUPP, so the chain path is never taken */
#define DRIVER_SYNCOBJ_TIMELINE 0
struct dma_fence_chain { int unused; };
#define drm_syncobj_add_point(syncobj, chain, fence, point) do { } while (0)

/* no interconnect: the GMU firmware does the bus votes */
static inline int dev_pm_opp_set_bw(struct device *dev, struct dev_pm_opp *opp) { return 0; }

/* the KGSL device tree puts the RPMh corner in opp-microvolt (no opp-level on 4.19) */
unsigned long dev_pm_opp_get_voltage(struct dev_pm_opp *opp);
#define dev_pm_opp_get_level(opp) ((unsigned int)dev_pm_opp_get_voltage(opp))

int devm_clk_bulk_get_all(struct device *dev, struct clk_bulk_data **clks);

/* KGSL device tree glue (q2shim.c) */
struct device_node;
const char *q2_kgsl_name(const char *name, bool irq);
struct device *q2_of_child_device(struct device_node *np, const char *name);
struct device *q2_of_device_by_name(const char *name);
void q2_cap_opps(struct device *dev);
void q2_release_devices(void);
void q2_free_kgsl_irq(int irq);
void q2_aop_acd_off(struct device *gmu_dev);
struct q2_gpu_cfg {
	bool acd, lm;
	u32 acd_enable_by_level, acd_num, acd_data[6];
	u32 throttle_mask, lm_slope;
};
extern struct q2_gpu_cfg q2_cfg;
void q2_gpu_cfg_init(struct device *gpu_dev);
void *subsystem_get(const char *name);
struct platform_device;
struct msm_bus_tcs_handle;
int q2_bus_init(struct platform_device *gpu_pdev, struct platform_device *gmu_pdev);
void q2_bus_exit(void);
int q2_bus_levels(bool cnoc);
int q2_bus_query(bool cnoc, struct msm_bus_tcs_handle *h);
void q2_bus_vote(bool on);

#endif
