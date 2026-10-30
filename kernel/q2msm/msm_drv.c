// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#include <linux/dma-mapping.h>
#include <linux/kthread.h>
#include <linux/uaccess.h>
#include <uapi/linux/sched/types.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_irq.h>
#include <drm/drm_prime.h>
#include <drm/drm_of.h>
#include <drm/drm_vblank.h>

#include "msm_drv.h"
#include "msm_debugfs.h"
#include "msm_fence.h"
#include "msm_gem.h"
#include "msm_gpu.h"
#include "msm_kms.h"
#include "adreno/adreno_gpu.h"

/*
 * MSM driver version:
 * - 1.0.0 - initial interface
 * - 1.1.0 - adds madvise, and support for submits with > 4 cmd buffers
 * - 1.2.0 - adds explicit fence support for submit ioctl
 * - 1.3.0 - adds GMEM_BASE + NR_RINGS params, SUBMITQUEUE_NEW +
 *           SUBMITQUEUE_CLOSE ioctls, and MSM_INFO_IOVA flag for
 *           MSM_GEM_INFO ioctl.
 * - 1.4.0 - softpin, MSM_RELOC_BO_DUMP, and GEM_INFO support to set/get
 *           GEM object's debug name
 * - 1.5.0 - Add SUBMITQUERY_QUERY ioctl
 * - 1.6.0 - Syncobj support
 */
#define MSM_VERSION_MAJOR	1
#define MSM_VERSION_MINOR	6
#define MSM_VERSION_PATCHLEVEL	0



#ifdef CONFIG_DRM_MSM_REGISTER_LOGGING
static bool reglog = false;
MODULE_PARM_DESC(reglog, "Enable register read/write logging");
module_param(reglog, bool, 0600);
#else
#define reglog 0
#endif

#ifdef CONFIG_DRM_FBDEV_EMULATION
static bool fbdev = true;
MODULE_PARM_DESC(fbdev, "Enable fbdev compat layer");
module_param(fbdev, bool, 0600);
#endif

static char *vram = "16m";
MODULE_PARM_DESC(vram, "Configure VRAM size (for devices without IOMMU/GPUMMU)");
module_param(vram, charp, 0);

bool dumpstate = false;
MODULE_PARM_DESC(dumpstate, "Dump KMS state on errors");
module_param(dumpstate, bool, 0600);

static bool modeset = true;
MODULE_PARM_DESC(modeset, "Use kernel modesetting [KMS] (1=on (default), 0=disable)");
module_param(modeset, bool, 0600);

/*
 * Util/helpers:
 */

struct clk *msm_clk_bulk_get_clock(struct clk_bulk_data *bulk, int count,
		const char *name)
{
	int i;
	char n[32];

	snprintf(n, sizeof(n), "%s_clk", name);

	for (i = 0; bulk && i < count; i++) {
		if (!strcmp(bulk[i].id, name) || !strcmp(bulk[i].id, n))
			return bulk[i].clk;
	}


	return NULL;
}

struct clk *msm_clk_get(struct platform_device *pdev, const char *name)
{
	struct clk *clk;
	char name2[32];

	clk = devm_clk_get(&pdev->dev, name);
	if (!IS_ERR(clk) || PTR_ERR(clk) == -EPROBE_DEFER)
		return clk;

	snprintf(name2, sizeof(name2), "%s_clk", name);

	clk = devm_clk_get(&pdev->dev, name2);
	if (!IS_ERR(clk))
		dev_warn(&pdev->dev, "Using legacy clk name binding.  Use "
				"\"%s\" instead of \"%s\"\n", name, name2);

	return clk;
}

void __iomem *_msm_ioremap(struct platform_device *pdev, const char *name,
			   const char *dbgname, bool quiet)
{
	struct resource *res;
	unsigned long size;
	void __iomem *ptr;

	if (name)
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	else
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	if (!res) {
		if (!quiet)
			DRM_DEV_ERROR(&pdev->dev, "failed to get memory resource: %s\n", name);
		return ERR_PTR(-EINVAL);
	}

	size = resource_size(res);

	ptr = devm_ioremap(&pdev->dev, res->start, size);
	if (!ptr) {
		if (!quiet)
			DRM_DEV_ERROR(&pdev->dev, "failed to ioremap: %s\n", name);
		return ERR_PTR(-ENOMEM);
	}

	if (reglog)
		printk(KERN_DEBUG "IO:region %s %p %08lx\n", dbgname, ptr, size);

	return ptr;
}

void __iomem *msm_ioremap(struct platform_device *pdev, const char *name,
			  const char *dbgname)
{
	return _msm_ioremap(pdev, name, dbgname, false);
}

void __iomem *msm_ioremap_quiet(struct platform_device *pdev, const char *name,
				const char *dbgname)
{
	return _msm_ioremap(pdev, name, dbgname, true);
}

void msm_writel(u32 data, void __iomem *addr)
{
	if (reglog)
		printk(KERN_DEBUG "IO:W %p %08x\n", addr, data);
	writel(data, addr);
}

u32 msm_readl(const void __iomem *addr)
{
	u32 val = readl(addr);
	if (reglog)
		pr_err("IO:R %p %08x\n", addr, val);
	return val;
}

struct msm_vblank_work {
	struct work_struct work;
	int crtc_id;
	bool enable;
	struct msm_drm_private *priv;
};

static void vblank_ctrl_worker(struct work_struct *work)
{
	struct msm_vblank_work *vbl_work = container_of(work,
						struct msm_vblank_work, work);
	struct msm_drm_private *priv = vbl_work->priv;
	struct msm_kms *kms = priv->kms;

	if (vbl_work->enable)
		kms->funcs->enable_vblank(kms, priv->crtcs[vbl_work->crtc_id]);
	else
		kms->funcs->disable_vblank(kms,	priv->crtcs[vbl_work->crtc_id]);

	kfree(vbl_work);
}

static int vblank_ctrl_queue_work(struct msm_drm_private *priv,
					int crtc_id, bool enable)
{
	struct msm_vblank_work *vbl_work;

	vbl_work = kzalloc(sizeof(*vbl_work), GFP_ATOMIC);
	if (!vbl_work)
		return -ENOMEM;

	INIT_WORK(&vbl_work->work, vblank_ctrl_worker);

	vbl_work->crtc_id = crtc_id;
	vbl_work->enable = enable;
	vbl_work->priv = priv;

	queue_work(priv->wq, &vbl_work->work);

	return 0;
}

static int msm_drm_uninit(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct drm_device *ddev = platform_get_drvdata(pdev);
	struct msm_drm_private *priv = ddev->dev_private;
	int i;

	/*
	 * Shutdown the hw if we're far enough along where things might be on.
	 * If we run this too early, we'll end up panicking in any variety of
	 * places. Since we don't register the drm device until late in
	 * msm_drm_init, drm_dev->registered is used as an indicator that the
	 * shutdown will be successful.
	 */
	if (ddev->registered) {
		drm_dev_unregister(ddev);
	}

	/* We must cancel and cleanup any pending vblank enable/disable
	 * work before drm_irq_uninstall() to avoid work re-enabling an
	 * irq after uninstall has disabled it.
	 */

	flush_workqueue(priv->wq);

	/* clean up event worker threads */
	for (i = 0; i < priv->num_crtcs; i++) {
		if (priv->event_thread[i].worker)
			kthread_destroy_worker(priv->event_thread[i].worker);
	}

	msm_gem_shrinker_cleanup(ddev);

	drm_mode_config_cleanup(ddev);

	if (priv->vram.paddr) {
		unsigned long attrs = DMA_ATTR_NO_KERNEL_MAPPING;
		drm_mm_takedown(&priv->vram.mm);
		dma_free_attrs(dev, priv->vram.size, NULL,
			       priv->vram.paddr, attrs);
	}

	component_unbind_all(dev, ddev);

	/*
	 * q2msm: tearing the GPU down queues its buffers on the free worker; free them now, while the drm device
	 * (its mmap offset manager) and the SMMU context devices still exist - not in destroy_workqueue() below.
	 */
	flush_workqueue(priv->wq);

	ddev->dev_private = NULL;
	drm_dev_put(ddev);

	destroy_workqueue(priv->wq);
	kfree(priv);

	return 0;
}


#include <linux/of_address.h>

bool msm_use_mmu(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;

	/* a2xx comes with its own MMU */
	return priv->is_a2xx || iommu_present(&platform_bus_type);
}

static int msm_init_vram(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct device_node *node;
	unsigned long size = 0;
	int ret = 0;

	/* In the device-tree world, we could have a 'memory-region'
	 * phandle, which gives us a link to our "vram".  Allocating
	 * is all nicely abstracted behind the dma api, but we need
	 * to know the entire size to allocate it all in one go. There
	 * are two cases:
	 *  1) device with no IOMMU, in which case we need exclusive
	 *     access to a VRAM carveout big enough for all gpu
	 *     buffers
	 *  2) device with IOMMU, but where the bootloader puts up
	 *     a splash screen.  In this case, the VRAM carveout
	 *     need only be large enough for fbdev fb.  But we need
	 *     exclusive access to the buffer to avoid the kernel
	 *     using those pages for other purposes (which appears
	 *     as corruption on screen before we have a chance to
	 *     load and do initial modeset)
	 */

	node = of_parse_phandle(dev->dev->of_node, "memory-region", 0);
	if (node) {
		struct resource r;
		ret = of_address_to_resource(node, 0, &r);
		of_node_put(node);
		if (ret)
			return ret;
		size = r.end - r.start;
		DRM_INFO("using VRAM carveout: %lx@%pa\n", size, &r.start);

		/* if we have no IOMMU, then we need to use carveout allocator.
		 * Grab the entire CMA chunk carved out in early startup in
		 * mach-msm:
		 */
	} else if (!msm_use_mmu(dev)) {
		DRM_INFO("using %s VRAM carveout\n", vram);
		size = memparse(vram, NULL);
	}

	if (size) {
		unsigned long attrs = 0;
		void *p;

		priv->vram.size = size;

		drm_mm_init(&priv->vram.mm, 0, (size >> PAGE_SHIFT) - 1);
		spin_lock_init(&priv->vram.lock);

		attrs |= DMA_ATTR_NO_KERNEL_MAPPING;
		attrs |= DMA_ATTR_WRITE_COMBINE;

		/* note that for no-kernel-mapping, the vaddr returned
		 * is bogus, but non-null if allocation succeeded:
		 */
		p = dma_alloc_attrs(dev->dev, size,
				&priv->vram.paddr, GFP_KERNEL, attrs);
		if (!p) {
			DRM_DEV_ERROR(dev->dev, "failed to allocate VRAM\n");
			priv->vram.paddr = 0;
			return -ENOMEM;
		}

		DRM_DEV_INFO(dev->dev, "VRAM: %08x->%08x\n",
				(uint32_t)priv->vram.paddr,
				(uint32_t)(priv->vram.paddr + size));
	}

	return ret;
}

static int msm_drm_init(struct device *dev, struct drm_driver *drv)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct drm_device *ddev;
	struct msm_drm_private *priv;
	int ret;

	ddev = drm_dev_alloc(drv, dev);
	if (IS_ERR(ddev)) {
		DRM_DEV_ERROR(dev, "failed to allocate drm_device\n");
		return PTR_ERR(ddev);
	}

	platform_set_drvdata(pdev, ddev);

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto err_put_drm_dev;
	}

	ddev->dev_private = priv;
	priv->dev = ddev;


	priv->wq = alloc_ordered_workqueue("msm", 0);

	INIT_WORK(&priv->free_work, msm_gem_free_work);
	init_llist_head(&priv->free_list);

	INIT_LIST_HEAD(&priv->inactive_list);
	q2_gemstat_dev = ddev;

	drm_mode_config_init(ddev);

	/* Bind all our sub-components: */
	ret = component_bind_all(dev, ddev);
	if (ret)
		goto err_free_priv;

	ret = msm_init_vram(ddev);
	if (ret)
		goto err_msm_uninit;

	dma_set_max_seg_size(dev, UINT_MAX);

	msm_gem_shrinker_init(ddev);

	/* q2msm: GPU only (render node) - the display stays with Meta's SDE driver */
	ret = drm_dev_register(ddev, 0);
	if (ret)
		goto err_msm_uninit;

	return 0;

err_msm_uninit:
	msm_drm_uninit(dev);
	return ret;
err_free_priv:
	/* q2msm: a failed GPU bind frees GEM objects through the free worker; finish it while the drm device lives */
	flush_workqueue(priv->wq);
	destroy_workqueue(priv->wq);
	kfree(priv);
err_put_drm_dev:
	drm_dev_put(ddev);
	return ret;
}

/*
 * DRM operations:
 */

static void load_gpu(struct drm_device *dev)
{
	static DEFINE_MUTEX(init_lock);
	struct msm_drm_private *priv = dev->dev_private;

	mutex_lock(&init_lock);

	if (!priv->gpu)
		priv->gpu = adreno_load_gpu(dev);

	mutex_unlock(&init_lock);
}

static int context_init(struct drm_device *dev, struct drm_file *file)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_file_private *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	kref_init(&ctx->ref);
	msm_submitqueue_init(dev, ctx);

	ctx->aspace = msm_gpu_create_private_address_space(priv->gpu, current);
	file->driver_priv = ctx;

	return 0;
}

static int msm_open(struct drm_device *dev, struct drm_file *file)
{
	/* For now, load gpu on open.. to avoid the requirement of having
	 * firmware in the initrd.
	 */
	load_gpu(dev);

	return context_init(dev, file);
}

static void context_close(struct msm_file_private *ctx)
{
	msm_submitqueue_close(ctx);
	msm_file_private_put(ctx);
}

static void msm_postclose(struct drm_device *dev, struct drm_file *file)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_file_private *ctx = file->driver_priv;

	mutex_lock(&dev->struct_mutex);
	if (ctx == priv->lastctx)
		priv->lastctx = NULL;
	mutex_unlock(&dev->struct_mutex);

	context_close(ctx);
}

static irqreturn_t msm_irq(int irq, void *arg)
{
	struct drm_device *dev = arg;
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;
	BUG_ON(!kms);
	return kms->funcs->irq(kms);
}

static void msm_irq_preinstall(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;
	BUG_ON(!kms);
	kms->funcs->irq_preinstall(kms);
}

static int msm_irq_postinstall(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;
	BUG_ON(!kms);

	if (kms->funcs->irq_postinstall)
		return kms->funcs->irq_postinstall(kms);

	return 0;
}

static void msm_irq_uninstall(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;
	BUG_ON(!kms);
	kms->funcs->irq_uninstall(kms);
}

int msm_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	unsigned int pipe = crtc->index;
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;
	if (!kms)
		return -ENXIO;
	DBG("dev=%p, crtc=%u", dev, pipe);
	return vblank_ctrl_queue_work(priv, pipe, true);
}

void msm_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	unsigned int pipe = crtc->index;
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;
	if (!kms)
		return;
	DBG("dev=%p, crtc=%u", dev, pipe);
	vblank_ctrl_queue_work(priv, pipe, false);
}

/*
 * DRM ioctls:
 */

static int msm_ioctl_get_param(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct drm_msm_param *args = data;
	struct msm_gpu *gpu;

	/* for now, we just have 3d pipe.. eventually this would need to
	 * be more clever to dispatch to appropriate gpu module:
	 */
	if (args->pipe != MSM_PIPE_3D0)
		return -EINVAL;

	gpu = priv->gpu;

	if (!gpu)
		return -ENXIO;

	return gpu->funcs->get_param(gpu, args->param, &args->value);
}

static int msm_ioctl_gem_new(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	struct drm_msm_gem_new *args = data;

	if (args->flags & ~MSM_BO_FLAGS) {
		DRM_ERROR("invalid flags: %08x\n", args->flags);
		return -EINVAL;
	}

	return msm_gem_new_handle(dev, file, args->size,
			args->flags, &args->handle, NULL);
}

static inline ktime_t to_ktime(struct drm_msm_timespec timeout)
{
	return ktime_set(timeout.tv_sec, timeout.tv_nsec);
}

static int msm_ioctl_gem_cpu_prep(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	struct drm_msm_gem_cpu_prep *args = data;
	struct drm_gem_object *obj;
	ktime_t timeout = to_ktime(args->timeout);
	int ret;

	if (args->op & ~MSM_PREP_FLAGS) {
		DRM_ERROR("invalid op: %08x\n", args->op);
		return -EINVAL;
	}

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	ret = msm_gem_cpu_prep(obj, args->op, &timeout);

	drm_gem_object_put_unlocked(obj);

	return ret;
}

static int msm_ioctl_gem_cpu_fini(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	struct drm_msm_gem_cpu_fini *args = data;
	struct drm_gem_object *obj;
	int ret;

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	ret = msm_gem_cpu_fini(obj);

	drm_gem_object_put_unlocked(obj);

	return ret;
}

static int msm_ioctl_gem_info_iova(struct drm_device *dev,
		struct drm_file *file, struct drm_gem_object *obj,
		uint64_t *iova)
{
	struct msm_file_private *ctx = file->driver_priv;

	if (!ctx->aspace)
		return -EINVAL;

	/*
	 * Don't pin the memory here - just get an address so that userspace can
	 * be productive
	 */
	return msm_gem_get_iova(obj, ctx->aspace, iova);
}

static int msm_ioctl_gem_info(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	struct drm_msm_gem_info *args = data;
	struct drm_gem_object *obj;
	struct msm_gem_object *msm_obj;
	int i, ret = 0;

	if (args->pad)
		return -EINVAL;

	switch (args->info) {
	case MSM_INFO_GET_OFFSET:
	case MSM_INFO_GET_IOVA:
		/* value returned as immediate, not pointer, so len==0: */
		if (args->len)
			return -EINVAL;
		break;
	case MSM_INFO_SET_NAME:
	case MSM_INFO_GET_NAME:
	case MSM_INFO_SET_METADATA:
	case MSM_INFO_GET_METADATA:
		break;
	default:
		return -EINVAL;
	}

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	msm_obj = to_msm_bo(obj);

	switch (args->info) {
	case MSM_INFO_GET_OFFSET:
		args->value = msm_gem_mmap_offset(obj);
		break;
	case MSM_INFO_GET_IOVA:
		ret = msm_ioctl_gem_info_iova(dev, file, obj, &args->value);
		break;
	case MSM_INFO_SET_NAME:
		/* length check should leave room for terminating null: */
		if (args->len >= sizeof(msm_obj->name)) {
			ret = -EINVAL;
			break;
		}
		if (copy_from_user(msm_obj->name, u64_to_user_ptr(args->value),
				   args->len)) {
			msm_obj->name[0] = '\0';
			ret = -EFAULT;
			break;
		}
		msm_obj->name[args->len] = '\0';
		for (i = 0; i < args->len; i++) {
			if (!isprint(msm_obj->name[i])) {
				msm_obj->name[i] = '\0';
				break;
			}
		}
		break;
	case MSM_INFO_GET_NAME:
		if (args->value && (args->len < strlen(msm_obj->name))) {
			ret = -EINVAL;
			break;
		}
		args->len = strlen(msm_obj->name);
		if (args->value) {
			if (copy_to_user(u64_to_user_ptr(args->value),
					 msm_obj->name, args->len))
				ret = -EFAULT;
		}
		break;
	case MSM_INFO_SET_METADATA: {   /* q2msm: as mainline */
		void *md;

		if (args->len > 128) {
			ret = -EOVERFLOW;
			break;
		}
		md = kmalloc(args->len ?: 1, GFP_KERNEL);
		if (!md) {
			ret = -ENOMEM;
			break;
		}
		if (copy_from_user(md, u64_to_user_ptr(args->value), args->len)) {
			kfree(md);
			ret = -EFAULT;
			break;
		}
		mutex_lock(&msm_obj->lock);
		kfree(msm_obj->metadata);
		msm_obj->metadata = md;
		msm_obj->metadata_size = args->len;
		mutex_unlock(&msm_obj->lock);
		break;
	}
	case MSM_INFO_GET_METADATA: {
		void *md = NULL;
		u32 len;

		mutex_lock(&msm_obj->lock);
		len = msm_obj->metadata_size;
		if (args->value && len && args->len >= len)
			md = kmemdup(msm_obj->metadata, len, GFP_KERNEL);
		mutex_unlock(&msm_obj->lock);
		if (!args->value) {          /* size query */
			args->len = len;
			break;
		}
		if (args->len < len) {
			ret = -EINVAL;
			break;
		}
		if (len && !md) {
			ret = -ENOMEM;
			break;
		}
		if (len && copy_to_user(u64_to_user_ptr(args->value), md, len))
			ret = -EFAULT;
		args->len = len;
		kfree(md);
		break;
	}
	}

	drm_gem_object_put_unlocked(obj);

	return ret;
}

static int msm_ioctl_wait_fence(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct drm_msm_wait_fence *args = data;
	ktime_t timeout = to_ktime(args->timeout);
	struct msm_gpu_submitqueue *queue;
	struct msm_gpu *gpu = priv->gpu;
	int ret;

	if (args->pad) {
		DRM_ERROR("invalid pad: %08x\n", args->pad);
		return -EINVAL;
	}

	if (!gpu)
		return 0;

	queue = msm_submitqueue_get(file->driver_priv, args->queueid);
	if (!queue)
		return -ENOENT;

	ret = msm_wait_fence(gpu->rb[queue->prio]->fctx, args->fence, &timeout,
		true);

	msm_submitqueue_put(queue);
	return ret;
}

static int msm_ioctl_gem_madvise(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	struct drm_msm_gem_madvise *args = data;
	struct drm_gem_object *obj;
	int ret;

	switch (args->madv) {
	case MSM_MADV_DONTNEED:
	case MSM_MADV_WILLNEED:
		break;
	default:
		return -EINVAL;
	}

	ret = mutex_lock_interruptible(&dev->struct_mutex);
	if (ret)
		return ret;

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj) {
		ret = -ENOENT;
		goto unlock;
	}

	ret = msm_gem_madvise(obj, args->madv);
	if (ret >= 0) {
		args->retained = ret;
		ret = 0;
	}

	q2_gem_put_locked(obj);

unlock:
	mutex_unlock(&dev->struct_mutex);
	return ret;
}


static int msm_ioctl_submitqueue_new(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	struct drm_msm_submitqueue *args = data;

	if (args->flags & ~MSM_SUBMITQUEUE_FLAGS)
		return -EINVAL;

	return msm_submitqueue_create(dev, file->driver_priv, args->prio,
		args->flags, &args->id);
}

static int msm_ioctl_submitqueue_query(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	return msm_submitqueue_query(dev, file->driver_priv, data);
}

static int msm_ioctl_submitqueue_close(struct drm_device *dev, void *data,
		struct drm_file *file)
{
	u32 id = *(u32 *) data;

	return msm_submitqueue_remove(file->driver_priv, id);
}

/*
 * q2msm: Turnip turns any failed GPU ioctl (or a nonzero fault count) into VK_ERROR_DEVICE_LOST without saying why,
 * and Steam's web UI / the SteamVR compositor die of it with nothing in the kernel log.  Log every unexpected error
 * (and every nonzero fault count handed out), with the process: then the kernel log says what Turnip saw.
 */
static bool q2_ioctl_err_expected(int ret)
{
	return ret == -EINTR || ret == -ERESTARTSYS || ret == -EAGAIN || ret == -ETIMEDOUT || ret == -EBUSY;
}
#define Q2_IOCTL_LOGGED(fn) \
static int q2_##fn(struct drm_device *dev, void *data, struct drm_file *file) \
{ \
	int ret = fn(dev, data, file); \
	if (ret && !q2_ioctl_err_expected(ret)) \
		pr_warn_ratelimited("q2msm: %s failed %d (%s pid %d)\n", #fn, ret, current->comm, task_tgid_nr(current)); \
	return ret; \
}
Q2_IOCTL_LOGGED(msm_ioctl_gem_new)
Q2_IOCTL_LOGGED(msm_ioctl_gem_info)
Q2_IOCTL_LOGGED(msm_ioctl_gem_cpu_prep)
Q2_IOCTL_LOGGED(msm_ioctl_gem_cpu_fini)
Q2_IOCTL_LOGGED(msm_ioctl_gem_submit)
Q2_IOCTL_LOGGED(msm_ioctl_wait_fence)
Q2_IOCTL_LOGGED(msm_ioctl_gem_madvise)
Q2_IOCTL_LOGGED(msm_ioctl_submitqueue_new)

static int q2_msm_ioctl_get_param(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_msm_param *args = data;
	int ret = msm_ioctl_get_param(dev, data, file);

	if (ret && !q2_ioctl_err_expected(ret))
		pr_warn_ratelimited("q2msm: get_param %u failed %d (%s pid %d)\n", args->param, ret, current->comm,
			task_tgid_nr(current));
	else if (!ret && args->param == MSM_PARAM_FAULTS && args->value)
		pr_warn_ratelimited("q2msm: get_param FAULTS = %llu (%s pid %d)\n", args->value, current->comm,
			task_tgid_nr(current));
	return ret;
}

static int q2_msm_ioctl_submitqueue_query(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_msm_submitqueue_query *args = data;
	int ret = msm_ioctl_submitqueue_query(dev, data, file);
	u32 faults = 0;

	if (ret && !q2_ioctl_err_expected(ret))
		pr_warn_ratelimited("q2msm: submitqueue_query %u failed %d (%s pid %d)\n", args->param, ret, current->comm,
			task_tgid_nr(current));
	else if (!ret && args->param == MSM_SUBMITQUEUE_PARAM_FAULTS && args->len >= sizeof(faults) &&
		 !copy_from_user(&faults, u64_to_user_ptr(args->data), sizeof(faults)) && faults)
		pr_warn_ratelimited("q2msm: queue %u FAULTS = %u (%s pid %d)\n", args->id, faults, current->comm,
			task_tgid_nr(current));
	return ret;
}

static const struct drm_ioctl_desc msm_ioctls[] = {
	DRM_IOCTL_DEF_DRV(MSM_GET_PARAM,    q2_msm_ioctl_get_param,    DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_GEM_NEW,      q2_msm_ioctl_gem_new,      DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_GEM_INFO,     q2_msm_ioctl_gem_info,     DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_GEM_CPU_PREP, q2_msm_ioctl_gem_cpu_prep, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_GEM_CPU_FINI, q2_msm_ioctl_gem_cpu_fini, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_GEM_SUBMIT,   q2_msm_ioctl_gem_submit,   DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_WAIT_FENCE,   q2_msm_ioctl_wait_fence,   DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_GEM_MADVISE,  q2_msm_ioctl_gem_madvise,  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_SUBMITQUEUE_NEW,   q2_msm_ioctl_submitqueue_new,   DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_SUBMITQUEUE_CLOSE, msm_ioctl_submitqueue_close, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(MSM_SUBMITQUEUE_QUERY, q2_msm_ioctl_submitqueue_query, DRM_RENDER_ALLOW),
};

static const struct vm_operations_struct vm_ops = {
	.fault = msm_gem_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static const struct file_operations fops = {
	.owner              = THIS_MODULE,
	.open               = drm_open,
	.release            = drm_release,
	.unlocked_ioctl     = drm_ioctl,
	.compat_ioctl       = drm_compat_ioctl,
	.poll               = drm_poll,
	.read               = drm_read,
	.llseek             = no_llseek,
	.mmap               = msm_gem_mmap,
};

static struct drm_driver msm_driver = {
	/* q2msm: 4.19 still gates dma-buf import/export on DRIVER_PRIME (always on since 5.4) */
	.driver_features    = DRIVER_GEM |
				DRIVER_PRIME |
				DRIVER_RENDER |
				DRIVER_SYNCOBJ,
	.open               = msm_open,
	.postclose           = msm_postclose,
	.irq_handler        = msm_irq,
	.irq_preinstall     = msm_irq_preinstall,
	.irq_postinstall    = msm_irq_postinstall,
	.irq_uninstall      = msm_irq_uninstall,
	.gem_free_object_unlocked = msm_gem_free_object,
	.gem_vm_ops         = &vm_ops,
	.dumb_create        = msm_gem_dumb_create,
	.dumb_map_offset    = msm_gem_dumb_map_offset,
	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	/* q2msm: 4.19 calls these unconditionally (the defaults only arrived in 5.x) */
	.gem_prime_import   = drm_gem_prime_import,
	.gem_prime_export   = drm_gem_prime_export,
	.gem_prime_res_obj  = msm_gem_prime_res_obj,
	.gem_prime_pin      = msm_gem_prime_pin,
	.gem_prime_unpin    = msm_gem_prime_unpin,
	.gem_prime_get_sg_table = msm_gem_prime_get_sg_table,
	.gem_prime_import_sg_table = msm_gem_prime_import_sg_table,
	.gem_prime_vmap     = msm_gem_prime_vmap,
	.gem_prime_vunmap   = msm_gem_prime_vunmap,
	.gem_prime_mmap     = msm_gem_prime_mmap,
	.ioctls             = msm_ioctls,
	.num_ioctls         = ARRAY_SIZE(msm_ioctls),
	.fops               = &fops,
	.name               = "msm",
	.desc               = "MSM Snapdragon DRM (q2msm: GPU only, mainline 5.10 port)",
	.date               = "20130625",
	.major              = MSM_VERSION_MAJOR,
	.minor              = MSM_VERSION_MINOR,
	.patchlevel         = MSM_VERSION_PATCHLEVEL,
};

static int __maybe_unused msm_runtime_suspend(struct device *dev)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct msm_drm_private *priv = ddev->dev_private;
	struct msm_mdss *mdss = priv->mdss;

	DBG("");

	if (mdss && mdss->funcs)
		return mdss->funcs->disable(mdss);

	return 0;
}

static int __maybe_unused msm_runtime_resume(struct device *dev)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct msm_drm_private *priv = ddev->dev_private;
	struct msm_mdss *mdss = priv->mdss;

	DBG("");

	if (mdss && mdss->funcs)
		return mdss->funcs->enable(mdss);

	return 0;
}

static int __maybe_unused msm_pm_suspend(struct device *dev)
{

	if (pm_runtime_suspended(dev))
		return 0;

	return msm_runtime_suspend(dev);
}

static int __maybe_unused msm_pm_resume(struct device *dev)
{
	if (pm_runtime_suspended(dev))
		return 0;

	return msm_runtime_resume(dev);
}

static int __maybe_unused msm_pm_prepare(struct device *dev)
{
	struct drm_device *ddev = dev_get_drvdata(dev);

	return drm_mode_config_helper_suspend(ddev);
}

static void __maybe_unused msm_pm_complete(struct device *dev)
{
	struct drm_device *ddev = dev_get_drvdata(dev);

	drm_mode_config_helper_resume(ddev);
}

static const struct dev_pm_ops msm_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(msm_pm_suspend, msm_pm_resume)
	SET_RUNTIME_PM_OPS(msm_runtime_suspend, msm_runtime_resume, NULL)
	.prepare = msm_pm_prepare,
	.complete = msm_pm_complete,
};

/*
 * Componentized driver support:
 */

/*
 * NOTE: duplication of the same code as exynos or imx (or probably any other).
 * so probably some room for some helpers
 */
static int compare_of(struct device *dev, void *data)
{
	return dev->of_node == data;
}

/*
 * We don't know what's the best binding to link the gpu with the drm device.
 * Fow now, we just hunt for all the possible gpus that we support, and add them
 * as components.
 */
static const struct of_device_id msm_gpu_match[] = {
	{ .compatible = "qcom,adreno" },
	{ .compatible = "qcom,adreno-3xx" },
	{ .compatible = "amd,imageon" },
	{ .compatible = "qcom,kgsl-3d0" },
	{ },
};

static int add_gpu_components(struct device *dev,
			      struct component_match **matchptr)
{
	struct device_node *np;

	np = of_find_matching_node(NULL, msm_gpu_match);
	if (!np)
		return 0;

	if (of_device_is_available(np))
		drm_of_component_match_add(dev, matchptr, compare_of, np);

	of_node_put(np);

	return 0;
}

static int msm_drm_bind(struct device *dev)
{
	return msm_drm_init(dev, &msm_driver);
}

static void msm_drm_unbind(struct device *dev)
{
	msm_drm_uninit(dev);
}

static const struct component_master_ops msm_drm_ops = {
	.bind = msm_drm_bind,
	.unbind = msm_drm_unbind,
};

/*
 * Platform driver:
 */

static int msm_pdev_probe(struct platform_device *pdev)
{
	struct component_match *match = NULL;
	int ret;

	ret = add_gpu_components(&pdev->dev, &match);
	if (ret)
		goto fail;

	/* on all devices that I am aware of, iommu's which can map
	 * any address the cpu can see are used:
	 */
	ret = dma_set_mask_and_coherent(&pdev->dev, ~0);
	if (ret)
		goto fail;

	ret = component_master_add_with_match(&pdev->dev, &msm_drm_ops, match);
	if (ret)
		goto fail;

	return 0;

fail:
	of_platform_depopulate(&pdev->dev);
	return ret;
}

static int msm_pdev_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &msm_drm_ops);
	of_platform_depopulate(&pdev->dev);

	return 0;
}

static void msm_pdev_shutdown(struct platform_device *pdev)
{
	/* q2msm: no display to shut down */
}


static struct platform_driver msm_platform_driver = {
	.probe      = msm_pdev_probe,
	.remove     = msm_pdev_remove,
	.shutdown   = msm_pdev_shutdown,
	.driver     = {
		.name   = "q2msm",   /* the dummy device adreno_device_register_headless() creates */
		.pm     = &msm_pm_ops,
	},
};

static int __init msm_drm_register(void)
{
	if (!modeset)
		return -EINVAL;

	DBG("init");
	adreno_register();
	return platform_driver_register(&msm_platform_driver);
}

static void __exit msm_drm_unregister(void)
{
	DBG("fini");
	platform_driver_unregister(&msm_platform_driver);
	adreno_unregister();
}

module_init(msm_drm_register);
module_exit(msm_drm_unregister);

MODULE_AUTHOR("Rob Clark <robdclark@gmail.com");
MODULE_DESCRIPTION("MSM DRM GPU driver (mainline 5.10, GPU-only port for the Quest 2)");
MODULE_LICENSE("GPL");
