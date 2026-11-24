/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The downstream bus-scaling client API for the camera drivers: the vote tables are read from the device
 * tree as before (qcom,msm-bus,*); votes are kept but not yet applied to this kernel's interconnects.
 */
#ifndef _Q2CAM_MSM_BUS_H
#define _Q2CAM_MSM_BUS_H

#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

struct msm_bus_vectors {
	int src;
	int dst;
	u64 ab;
	u64 ib;
};

struct msm_bus_paths {
	int num_paths;
	struct msm_bus_vectors *vectors;
};

struct msm_bus_scale_pdata {
	struct msm_bus_paths *usecase;
	int num_usecases;
	const char *name;
	unsigned int active_only;
};

static inline struct msm_bus_scale_pdata *msm_bus_pdata_from_node(struct platform_device *pdev,
								  struct device_node *of_node)
{
	struct device *dev = &pdev->dev;
	struct msm_bus_scale_pdata *pd;
	u32 cases = 0, paths = 0, *v;
	int i, j, n;

	if (of_property_read_u32(of_node, "qcom,msm-bus,num-cases", &cases) ||
	    of_property_read_u32(of_node, "qcom,msm-bus,num-paths", &paths) || !cases || !paths)
		return NULL;
	n = of_property_count_u32_elems(of_node, "qcom,msm-bus,vectors-KBps");
	if (n != cases * paths * 4)
		return NULL;
	pd = devm_kzalloc(dev, sizeof(*pd), GFP_KERNEL);
	v = devm_kcalloc(dev, n, sizeof(u32), GFP_KERNEL);
	if (!pd || !v)
		return NULL;
	pd->usecase = devm_kcalloc(dev, cases, sizeof(*pd->usecase), GFP_KERNEL);
	if (!pd->usecase || of_property_read_u32_array(of_node, "qcom,msm-bus,vectors-KBps", v, n))
		return NULL;
	of_property_read_string(of_node, "qcom,msm-bus,name", &pd->name);
	pd->num_usecases = cases;
	for (i = 0; i < cases; i++) {
		struct msm_bus_paths *p = &pd->usecase[i];

		p->num_paths = paths;
		p->vectors = devm_kcalloc(dev, paths, sizeof(*p->vectors), GFP_KERNEL);
		if (!p->vectors)
			return NULL;
		for (j = 0; j < paths; j++) {
			u32 *e = v + (i * paths + j) * 4;

			p->vectors[j].src = e[0];
			p->vectors[j].dst = e[1];
			p->vectors[j].ab = (u64)e[2] * 1000;
			p->vectors[j].ib = (u64)e[3] * 1000;
		}
	}
	return pd;
}

static inline u32 msm_bus_scale_register_client(struct msm_bus_scale_pdata *pdata)
{
	return 1;
}

static inline int msm_bus_scale_client_update_request(u32 cl, unsigned int index)
{
	return 0;
}

static inline void msm_bus_scale_unregister_client(u32 cl)
{
}

static inline void msm_bus_cl_clear_pdata(struct msm_bus_scale_pdata *pdata)
{
}
#endif
