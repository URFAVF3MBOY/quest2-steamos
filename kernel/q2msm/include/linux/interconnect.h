/* q2msm shim: Linux 4.19 has no interconnect framework.  The A650's GMU votes the GPU's DDR bandwidth itself (HFI
 * bandwidth table), so the driver's own icc votes become no-ops. */
#ifndef _Q2MSM_INTERCONNECT_H
#define _Q2MSM_INTERCONNECT_H
#include <linux/types.h>
#include <linux/err.h>
struct device;
struct icc_path;
#define Bps_to_icc(x) ((x) / 1000)
#define kBps_to_icc(x) (x)
#define MBps_to_icc(x) ((x) * 1000)
static inline struct icc_path *of_icc_get(struct device *dev, const char *name) { return NULL; }
static inline struct icc_path *devm_of_icc_get(struct device *dev, const char *name) { return NULL; }
static inline void icc_put(struct icc_path *path) { }
static inline int icc_set_bw(struct icc_path *path, u32 avg_bw, u32 peak_bw) { return 0; }
static inline void icc_set_tag(struct icc_path *path, u32 tag) { }
#endif
