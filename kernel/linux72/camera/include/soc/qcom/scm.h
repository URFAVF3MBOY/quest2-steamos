/* SPDX-License-Identifier: GPL-2.0-only */
/* The downstream SCM calls the camera drivers make: secure register access through qcom_scm; secure modes refused. */
#ifndef _Q2CAM_SCM_H
#define _Q2CAM_SCM_H
#include <linux/firmware/qcom/qcom_scm.h>

#define SCM_VAL			0
#define SCM_ARGS(n, ...)	(n)
#define SCM_SIP_FNID(s, c)	(((s) << 8) | (c))
#define TZ_SVC_SMMU_PROGRAM	0x15

struct scm_desc {
	u32 arginfo;
	u64 args[10];
	u64 ret[3];
};

static inline int scm_call2(u32 fn_id, struct scm_desc *desc)
{
	return -EOPNOTSUPP;
}

static inline u32 scm_io_read(phys_addr_t address)
{
	unsigned int val = 0;

	qcom_scm_io_readl(address, &val);
	return val;
}

static inline int scm_io_write(phys_addr_t address, u32 val)
{
	return qcom_scm_io_writel(address, val);
}
#endif
