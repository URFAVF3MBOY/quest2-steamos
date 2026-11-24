/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Included before every file of the Spectra camera drivers: what they used from the Quest 2's 4.19 kernel,
 * in terms of this kernel's interfaces.
 */
#ifndef _Q2CAM_COMPAT_H
#define _Q2CAM_COMPAT_H

#include <linux/types.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/time64.h>
#include <linux/timekeeping.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/pinctrl/consumer.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/dma-buf.h>
#include <linux/iosys-map.h>
#include <linux/vmalloc.h>
#include <linux/debugfs.h>
#include <linux/file.h>

/* Downstream Kconfig values */
#define CONFIG_NUM_ATTACHED_CAMERAS 8

/* Renamed or removed helpers */
#define strlcpy(dst, src, n)		strscpy(dst, src, n)
#define kzfree(p)			kfree_sensitive(p)
#define in_irq()			in_hardirq()
#define VFL_TYPE_GRABBER		VFL_TYPE_VIDEO
#define writel_relaxed_no_log(v, a)	writel_relaxed(v, a)
#define readl_relaxed_no_log(a)		readl_relaxed(a)
#define writel_no_log(v, a)		writel(v, a)

#define del_timer_sync(t)		timer_delete_sync(t)
#define del_timer(t)			timer_delete(t)
#define V4L2_SUBDEV_NAME_SIZE		32

/* debugfs_create_<int> return nothing now; the drivers test the result */
#define debugfs_create_bool(...)	({ debugfs_create_bool(__VA_ARGS__); 1; })
#define debugfs_create_u8(...)		({ debugfs_create_u8(__VA_ARGS__); 1; })
#define debugfs_create_u16(...)		({ debugfs_create_u16(__VA_ARGS__); 1; })
#define debugfs_create_u32(...)		({ debugfs_create_u32(__VA_ARGS__); 1; })
#define debugfs_create_u64(...)		({ debugfs_create_u64(__VA_ARGS__); 1; })
#define debugfs_create_x32(...)		({ debugfs_create_x32(__VA_ARGS__); 1; })
#define debugfs_create_x64(...)		({ debugfs_create_x64(__VA_ARGS__); 1; })
#define debugfs_create_ulong(...)	({ debugfs_create_ulong(__VA_ARGS__); 1; })

/* struct timespec and struct timeval are gone: timespec64 and the old timeval layout (tv_sec, tv_usec) */
#define timeval				__kernel_old_timeval
#define get_monotonic_boottime64(ts)	ktime_get_boottime_ts64(ts)
#define getnstimeofday64(ts)		ktime_get_real_ts64(ts)
#define timespec			timespec64
#define get_monotonic_boottime(ts)	ktime_get_boottime_ts64(ts)
#define ktime_get_ts(ts)		ktime_get_ts64(ts)
#define getnstimeofday(ts)		ktime_get_real_ts64(ts)
#define timespec_to_ns(ts)		timespec64_to_ns(ts)

/* DDR type from the downstream FDT helper: the Quest 2 has LPDDR4X (downstream code 7) */
#define DDR_TYPE_LPDDR4X		7
#define DDR_TYPE_LPDDR5			8
#define DDR_TYPE_LPDDR5X		9
static inline int of_fdt_get_ddrtype(void)
{
	return DDR_TYPE_LPDDR4X;
}

/* iommu_map and iommu_map_sg take an allocation flag now */
#define iommu_map(d, i, p, s, pr)	iommu_map(d, i, p, s, pr, GFP_KERNEL)
#define iommu_map_sg(d, i, sg, n, pr)	iommu_map_sg(d, i, sg, n, pr, GFP_KERNEL)

/* Legacy GPIO arrays (struct gpio and its helpers were removed) */
struct gpio {
	unsigned int gpio;
	unsigned long flags;
	const char *label;
};

static inline int gpio_request_array(const struct gpio *array, size_t num)
{
	size_t i;
	int err;

	for (i = 0; i < num; i++) {
		err = gpio_request_one(array[i].gpio, array[i].flags, array[i].label);
		if (err) {
			while (i--)
				gpio_free(array[i].gpio);
			return err;
		}
	}
	return 0;
}

static inline void gpio_free_array(const struct gpio *array, size_t num)
{
	while (num--)
		gpio_free(array[num].gpio);
}

/* Downstream IOVA helpers of the camera SMMU: no equivalent; IOVA ranges are kept by the driver itself */
static inline void iommu_dma_enable_best_fit_algo(struct device *dev)
{
}

static inline int iommu_dma_reserve_iova(struct device *dev, dma_addr_t base, u64 size)
{
	return 0;
}

#endif
