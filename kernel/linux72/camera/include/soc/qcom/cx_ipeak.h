/* SPDX-License-Identifier: GPL-2.0-only */
/* CX peak-current limiting is not used on this kernel. */
#ifndef _Q2CAM_CX_IPEAK_H
#define _Q2CAM_CX_IPEAK_H
#include <linux/err.h>
struct cx_ipeak_client;
static inline struct cx_ipeak_client *cx_ipeak_register(struct device_node *np, const char *name)
{
	return NULL;
}
static inline void cx_ipeak_unregister(struct cx_ipeak_client *c)
{
}
static inline int cx_ipeak_update(struct cx_ipeak_client *c, bool vote)
{
	return 0;
}
#endif
