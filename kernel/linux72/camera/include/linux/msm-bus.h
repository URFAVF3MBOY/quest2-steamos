/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The downstream bus-scaling client API for the camera drivers: the vote tables are read from the device
 * tree as before (qcom,msm-bus,*), and each client votes on the interconnect path of the same name
 * (interconnect-names of the device, as mainline's CAMSS names them: cam_ahb, cam_hf_0_mnoc, ...).
 */
#ifndef _Q2CAM_MSM_BUS_H
#define _Q2CAM_MSM_BUS_H

#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/interconnect.h>

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
	struct device *dev;
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
	pd->dev = dev;
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

#define Q2CAM_BUS_CLIENTS 16
static struct {
	struct icc_path *path;
	struct msm_bus_scale_pdata *pdata;
} q2cam_bus[Q2CAM_BUS_CLIENTS];

static inline u32 msm_bus_scale_register_client(struct msm_bus_scale_pdata *pdata)
{
	struct icc_path *path;
	u32 i;

	if (!pdata || !pdata->name || !pdata->dev)
		return 0;
	path = of_icc_get(pdata->dev, pdata->name);
	if (IS_ERR_OR_NULL(path)) {
		dev_err(pdata->dev, "no interconnect path %s: %ld", pdata->name, PTR_ERR(path));
		return 0;
	}
	for (i = 0; i < Q2CAM_BUS_CLIENTS; i++) {
		if (!q2cam_bus[i].path) {
			q2cam_bus[i].path = path;
			q2cam_bus[i].pdata = pdata;
			return i + 1;
		}
	}
	icc_put(path);
	return 0;
}

static inline int msm_bus_scale_client_update_request(u32 cl, unsigned int index)
{
	struct msm_bus_scale_pdata *pd;
	struct msm_bus_vectors *v;

	if (!cl || cl > Q2CAM_BUS_CLIENTS || !q2cam_bus[cl - 1].path)
		return -EINVAL;
	pd = q2cam_bus[cl - 1].pdata;
	if (index >= pd->num_usecases)
		return -EINVAL;
	v = &pd->usecase[index].vectors[0];
	/* vectors are bytes/s; the interconnect takes kB/s */
	return icc_set_bw(q2cam_bus[cl - 1].path, div_u64(v->ab, 1000), div_u64(v->ib, 1000));
}

static inline void msm_bus_scale_unregister_client(u32 cl)
{
	if (!cl || cl > Q2CAM_BUS_CLIENTS || !q2cam_bus[cl - 1].path)
		return;
	icc_set_bw(q2cam_bus[cl - 1].path, 0, 0);
	icc_put(q2cam_bus[cl - 1].path);
	q2cam_bus[cl - 1].path = NULL;
}

static inline void msm_bus_cl_clear_pdata(struct msm_bus_scale_pdata *pdata)
{
}
#endif
