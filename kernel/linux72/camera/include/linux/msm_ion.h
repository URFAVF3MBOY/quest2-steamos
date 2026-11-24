/* SPDX-License-Identifier: GPL-2.0-only */
/* The downstream ION kernel API, served by this kernel's /dev/ion exporter (drivers/misc/q2android/ion.c). */
#ifndef _Q2CAM_MSM_ION_H
#define _Q2CAM_MSM_ION_H

#include <linux/dma-buf.h>

#define ION_BIT(nr)			(1U << (nr))
#define ION_HEAP(bit)			ION_BIT(bit)
#define ION_FLAG_CACHED			1
#define ION_FLAG_SECURE			ION_BIT(31)
#define ION_FLAG_CP_CAMERA		ION_BIT(21)
#define ION_FLAG_CP_CDSP		ION_BIT(29)
#define ION_SECURE_DISPLAY_HEAP_ID	10
#define ION_CAMERA_HEAP_ID		20
#define ION_SYSTEM_HEAP_ID		25

struct dma_buf *ion_alloc(size_t len, unsigned int heap_id_mask, unsigned int flags);
int dma_buf_get_flags(struct dma_buf *dmabuf, unsigned long *flags);

#endif
