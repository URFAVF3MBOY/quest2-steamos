/* SPDX-License-Identifier: GPL-2.0-only */
/* Secure (content-protected) camera buffers are not supported on this kernel. */
#ifndef _Q2CAM_SECURE_BUFFER_H
#define _Q2CAM_SECURE_BUFFER_H
#define VMID_HLOS	0x3
#define VMID_CP_CAMERA	0xD
#define PERM_READ	0x4
#define PERM_WRITE	0x2
#define PERM_EXEC	0x1
static inline int hyp_assign_phys(u64 addr, u64 size, u32 *src, int nsrc, int *dst, int *perm, int ndst)
{
	return -EOPNOTSUPP;
}
#endif
