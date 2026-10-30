// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013-2014 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 *
 * Copyright (c) 2014,2017 The Linux Foundation. All rights reserved.
 */

#include <linux/of_device.h>
#include "adreno_gpu.h"

#define ANY_ID 0xff

bool hang_debug = false;
MODULE_PARM_DESC(hang_debug, "Dump registers when hang is detected (can be slow!)");
module_param_named(hang_debug, hang_debug, bool, 0600);

bool snapshot_debugbus = false;
MODULE_PARM_DESC(snapshot_debugbus, "Include debugbus sections in GPU devcoredump (if not fused off)");
module_param_named(snapshot_debugbus, snapshot_debugbus, bool, 0600);

static const struct adreno_info gpulist[] = {
	{
		.rev = ADRENO_REV(6, 1, 8, ANY_ID),
		.revn = 618,
		.name = "A618",
		.fw = {
			[ADRENO_FW_SQE] = "a630_sqe.fw",
			[ADRENO_FW_GMU] = "a630_gmu.bin",
		},
		.gmem = SZ_512K,
		.inactive_period = DRM_MSM_INACTIVE_PERIOD,
		.init = a6xx_gpu_init,
	}, {
		.rev = ADRENO_REV(6, 3, 0, ANY_ID),
		.revn = 630,
		.name = "A630",
		.fw = {
			[ADRENO_FW_SQE] = "a630_sqe.fw",
			[ADRENO_FW_GMU] = "a630_gmu.bin",
		},
		.gmem = SZ_1M,
		.inactive_period = DRM_MSM_INACTIVE_PERIOD,
		.init = a6xx_gpu_init,
		.zapfw = "a630_zap.mdt",
		.hwcg = a630_hwcg,
	}, {
		.rev = ADRENO_REV(6, 4, 0, ANY_ID),
		.revn = 640,
		.name = "A640",
		.fw = {
			[ADRENO_FW_SQE] = "a630_sqe.fw",
			[ADRENO_FW_GMU] = "a640_gmu.bin",
		},
		.gmem = SZ_1M,
		.inactive_period = DRM_MSM_INACTIVE_PERIOD,
		.init = a6xx_gpu_init,
		.zapfw = "a640_zap.mdt",
		.hwcg = a640_hwcg,
	}, {
		.rev = ADRENO_REV(6, 5, 0, ANY_ID),
		.revn = 650,
		.name = "A650",
		.fw = {
			[ADRENO_FW_SQE] = "a650_sqe.fw",
			[ADRENO_FW_GMU] = "a650_gmu.bin",
		},
		.gmem = SZ_1M + SZ_128K,
		.inactive_period = DRM_MSM_INACTIVE_PERIOD,
		.init = a6xx_gpu_init,
		.zapfw = "a650_zap.mdt",
		.hwcg = a650_hwcg,
	},
};

MODULE_FIRMWARE("qcom/a630_sqe.fw");
MODULE_FIRMWARE("qcom/a630_gmu.bin");
MODULE_FIRMWARE("qcom/a630_zap.mbn");

static inline bool _rev_match(uint8_t entry, uint8_t id)
{
	return (entry == ANY_ID) || (entry == id);
}

const struct adreno_info *adreno_info(struct adreno_rev rev)
{
	int i;

	/* identify gpu: */
	for (i = 0; i < ARRAY_SIZE(gpulist); i++) {
		const struct adreno_info *info = &gpulist[i];
		if (_rev_match(info->rev.core, rev.core) &&
				_rev_match(info->rev.major, rev.major) &&
				_rev_match(info->rev.minor, rev.minor) &&
				_rev_match(info->rev.patchid, rev.patchid))
			return info;
	}

	return NULL;
}

struct msm_gpu *adreno_load_gpu(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct platform_device *pdev = priv->gpu_pdev;
	struct msm_gpu *gpu = NULL;
	struct adreno_gpu *adreno_gpu;
	int ret;

	if (pdev)
		gpu = dev_to_gpu(&pdev->dev);

	if (!gpu) {
		dev_err_once(dev->dev, "no GPU device was found\n");
		return NULL;
	}

	adreno_gpu = to_adreno_gpu(gpu);

	/*
	 * The number one reason for HW init to fail is if the firmware isn't
	 * loaded yet. Try that first and don't bother continuing on
	 * otherwise
	 */

	ret = adreno_load_fw(adreno_gpu);
	if (ret)
		return NULL;

	/* Make sure pm runtime is active and reset any previous errors */
	pm_runtime_set_active(&pdev->dev);

	ret = pm_runtime_get_sync(&pdev->dev);
	if (ret < 0) {
		pm_runtime_put_sync(&pdev->dev);
		DRM_DEV_ERROR(dev->dev, "Couldn't power up the GPU: %d\n", ret);
		return NULL;
	}

	mutex_lock(&dev->struct_mutex);
	ret = msm_gpu_hw_init(gpu);
	mutex_unlock(&dev->struct_mutex);
	pm_runtime_put_autosuspend(&pdev->dev);
	if (ret) {
		DRM_DEV_ERROR(dev->dev, "gpu hw init failed: %d\n", ret);
		return NULL;
	}

#ifdef CONFIG_DEBUG_FS
	if (gpu->funcs->debugfs_init) {
		gpu->funcs->debugfs_init(gpu, dev->primary);
		gpu->funcs->debugfs_init(gpu, dev->render);
	}
#endif

	return gpu;
}

static void set_gpu_pdev(struct drm_device *dev,
		struct platform_device *pdev)
{
	struct msm_drm_private *priv = dev->dev_private;
	priv->gpu_pdev = pdev;
}

static int find_chipid(struct device *dev, struct adreno_rev *rev)
{
	struct device_node *node = dev->of_node;
	const char *compat;
	int ret;
	u32 chipid;

	/* first search the compat strings for qcom,adreno-XYZ.W: */
	ret = of_property_read_string_index(node, "compatible", 0, &compat);
	if (ret == 0) {
		unsigned int r, patch;

		if (sscanf(compat, "qcom,adreno-%u.%u", &r, &patch) == 2 ||
		    sscanf(compat, "amd,imageon-%u.%u", &r, &patch) == 2) {
			rev->core = r / 100;
			r %= 100;
			rev->major = r / 10;
			r %= 10;
			rev->minor = r;
			rev->patchid = patch;

			return 0;
		}
	}

	/* and if that fails, fall back to legacy "qcom,chipid" property: */
	ret = of_property_read_u32(node, "qcom,chipid", &chipid);
	if (ret) {
		DRM_DEV_ERROR(dev, "could not parse qcom,chipid: %d\n", ret);
		return ret;
	}

	rev->core = (chipid >> 24) & 0xff;
	rev->major = (chipid >> 16) & 0xff;
	rev->minor = (chipid >> 8) & 0xff;
	rev->patchid = (chipid & 0xff);

	dev_warn(dev, "Using legacy qcom,chipid binding!\n");
	dev_warn(dev, "Use compatible qcom,adreno-%u%u%u.%u instead.\n",
		rev->core, rev->major, rev->minor, rev->patchid);

	return 0;
}

static int adreno_bind(struct device *dev, struct device *master, void *data)
{
	static struct adreno_platform_config config = {};
	const struct adreno_info *info;
	struct drm_device *drm = dev_get_drvdata(master);
	struct msm_drm_private *priv = drm->dev_private;
	struct msm_gpu *gpu;
	int ret;

	ret = find_chipid(dev, &config.rev);
	if (ret)
		return ret;

	dev->platform_data = &config;
	set_gpu_pdev(drm, to_platform_device(dev));

	info = adreno_info(config.rev);

	if (!info) {
		dev_warn(drm->dev, "Unknown GPU revision: %u.%u.%u.%u\n",
			config.rev.core, config.rev.major,
			config.rev.minor, config.rev.patchid);
		return -ENXIO;
	}

	DBG("Found GPU: %u.%u.%u.%u", config.rev.core, config.rev.major,
		config.rev.minor, config.rev.patchid);

	priv->is_a2xx = config.rev.core == 2;

	gpu = info->init(drm);
	if (IS_ERR(gpu)) {
		dev_warn(drm->dev, "failed to load adreno gpu\n");
		return PTR_ERR(gpu);
	}

	return 0;
}

static void adreno_unbind(struct device *dev, struct device *master,
		void *data)
{
	struct msm_gpu *gpu = dev_to_gpu(dev);

	pm_runtime_force_suspend(dev);
	gpu->funcs->destroy(gpu);

	set_gpu_pdev(dev_get_drvdata(master), NULL);
}

static const struct component_ops a3xx_ops = {
		.bind   = adreno_bind,
		.unbind = adreno_unbind,
};

static struct platform_device *q2_headless;

static void adreno_device_register_headless(struct device_node *np)
{
	/* the GPU-only drm device ("headless", made for imx5 on mainline) */
	struct platform_device *pdev = platform_device_alloc("q2msm", -1);

	if (!pdev)
		return;
	/* q2msm: GEM buffers are cache-synced through the drm device, so give it the GPU node's
	 * (non-coherent) DMA ops before it is added and probed */
	pdev->dev.coherent_dma_mask = DMA_BIT_MASK(64);
	pdev->dev.dma_mask = &pdev->dev.coherent_dma_mask;
	of_dma_configure(&pdev->dev, np, true);
	if (platform_device_add(pdev)) {
		platform_device_put(pdev);
		return;
	}
	q2_headless = pdev;
}

static int adreno_probe(struct platform_device *pdev)
{

	int ret;

	ret = component_add(&pdev->dev, &a3xx_ops);
	if (ret)
		return ret;

	/* q2msm: always GPU-only - the display is driven by Meta's SDE driver */
	adreno_device_register_headless(pdev->dev.of_node);

	return 0;
}

static int adreno_remove(struct platform_device *pdev)
{
	if (q2_headless)
		platform_device_unregister(q2_headless);
	q2_headless = NULL;
	component_del(&pdev->dev, &a3xx_ops);
	q2_release_devices();
	return 0;
}

static const struct of_device_id dt_match[] = {
	{ .compatible = "qcom,adreno" },
	{ .compatible = "qcom,adreno-3xx" },
	/* for compatibility with imx5 gpu: */
	{ .compatible = "amd,imageon" },
	/* for backwards compat w/ downstream kgsl DT files: */
	{ .compatible = "qcom,kgsl-3d0" },
	{}
};

#ifdef CONFIG_PM
static int adreno_resume(struct device *dev)
{
	struct msm_gpu *gpu = dev_to_gpu(dev);

	return gpu->funcs->pm_resume(gpu);
}

static int adreno_suspend(struct device *dev)
{
	struct msm_gpu *gpu = dev_to_gpu(dev);

	return gpu->funcs->pm_suspend(gpu);
}
#endif

static const struct dev_pm_ops adreno_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(adreno_suspend, adreno_resume, NULL)
};

static struct platform_driver adreno_driver = {
	.probe = adreno_probe,
	.remove = adreno_remove,
	.driver = {
		.name = "q2msm-adreno",
		.of_match_table = dt_match,
		.pm = &adreno_pm_ops,
	},
};

void __init adreno_register(void)
{
	platform_driver_register(&adreno_driver);
}

void __exit adreno_unregister(void)
{
	platform_driver_unregister(&adreno_driver);
}
