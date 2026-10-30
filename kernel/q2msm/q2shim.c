// SPDX-License-Identifier: GPL-2.0-only
/* q2msm: pieces of 5.10 drm/msm that are left out of the GPU-only port (GPU crash-state dumps, debugfs perf/rd). */
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/nvmem-consumer.h>
#include <linux/slab.h>
#include <linux/msm-bus.h>
#include <linux/mailbox_client.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/of_platform.h>
#include <linux/of_device.h>
#include <linux/pm_opp.h>
#include <linux/module.h>
#include "msm_drv.h"
#include "msm_gpu.h"
#include "adreno/a6xx_gpu.h"
#include "adreno/a6xx_gpu_state.h"

struct msm_gpu_state *a6xx_gpu_state_get(struct msm_gpu *gpu) { return ERR_PTR(-EOPNOTSUPP); }
int a6xx_gpu_state_put(struct msm_gpu_state *state) { return 0; }
#if defined(CONFIG_DEBUG_FS) || defined(CONFIG_DEV_COREDUMP)
void a6xx_show(struct msm_gpu *gpu, struct msm_gpu_state *state, struct drm_printer *p) { }
#endif

/* 4.20 added clk_bulk_get_all: every clock named in clock-names */
int devm_clk_bulk_get_all(struct device *dev, struct clk_bulk_data **clks)
{
	struct device_node *np = dev->of_node;
	struct clk_bulk_data *c;
	int i, ret, n = of_property_count_strings(np, "clock-names");

	if (n <= 0)
		return 0;
	c = devm_kcalloc(dev, n, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	for (i = 0; i < n; i++)
		of_property_read_string_index(np, "clock-names", i, &c[i].id);
	ret = devm_clk_bulk_get(dev, n, c);
	if (ret)
		return ret;
	*clks = c;
	return n;
}

#ifdef CONFIG_DEBUG_FS
/* the rd (cmdstream dump) debugfs file is left out */
void msm_rd_dump_submit(struct msm_rd_state *rd, struct msm_gem_submit *submit,
		const char *fmt, ...)
{
}
#endif

/* mainline reg / irq names -> the KGSL device tree's */
const char *q2_kgsl_name(const char *name, bool irq)
{
	if (irq)
		return !strcmp(name, "hfi") ? "kgsl_hfi_irq" : !strcmp(name, "gmu") ? "kgsl_gmu_irq" : name;
	return !strcmp(name, "gmu") ? "kgsl_gmu_reg" :
		!strcmp(name, "gmu_pdc") ? "kgsl_gmu_pdc_cfg" :
		!strcmp(name, "gmu_pdc_seq") ? "kgsl_gmu_pdc_seq" : name;
}

/* devices q2msm created itself (KGSL populates its context-bank devices only while it is bound) */
static struct platform_device *q2_created[4];

/*
 * The platform device for an SMMU context-bank node; created (and wired to its SMMU) when KGSL has not populated
 * it - after KGSL was unbound, or when KGSL never probed.  Consumes the np reference.
 */
static struct device *q2_dev_of(struct device_node *np)
{
	struct platform_device *pdev, *parent;
	int i;

	if (!np)
		return NULL;
	pdev = of_find_device_by_node(np);
	if (!pdev) {
		parent = of_find_device_by_node(np->parent);
		pdev = of_platform_device_create(np, NULL, parent ? &parent->dev : NULL);
		if (parent)
			put_device(&parent->dev);
		/* 4.19 only wires a device to its SMMU (iommu fwspec + group) when a driver binds; do it by hand */
		if (pdev)
			of_dma_configure(&pdev->dev, np, true);
		for (i = 0; pdev && i < ARRAY_SIZE(q2_created); i++)
			if (!q2_created[i]) {
				q2_created[i] = pdev;
				break;
			}
		pr_info("q2msm: created %s: %s\n", np->name, pdev ? dev_name(&pdev->dev) : "failed");
	}
	of_node_put(np);
	return pdev ? &pdev->dev : NULL;
}

/* KGSL's SMMU context banks are child devices (gmu_kernel under the GMU, gfx3d_user under kgsl-iommu) */
struct device *q2_of_child_device(struct device_node *np, const char *name)
{
	return q2_dev_of(of_get_child_by_name(np, name));
}

struct device *q2_of_device_by_name(const char *name)
{
	return q2_dev_of(of_find_node_by_name(NULL, name));
}

void q2_release_devices(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(q2_created); i++) {
		if (!q2_created[i])
			continue;
		of_node_clear_flag(q2_created[i]->dev.of_node, OF_POPULATED);
		platform_device_unregister(q2_created[i]);
		q2_created[i] = NULL;
	}
}

/* the OPP table lists 805 MHz but KGSL never goes past 670 MHz on the Quest 2 */
static unsigned long max_freq = 670000000;
module_param(max_freq, ulong, 0444);

void q2_cap_opps(struct device *dev)
{
	unsigned long freq = max_freq + 1;
	struct dev_pm_opp *opp;

	while (!IS_ERR(opp = dev_pm_opp_find_freq_ceil(dev, &freq))) {
		dev_pm_opp_put(opp);
		dev_pm_opp_disable(dev, freq);
		freq++;
	}
}

/* KGSL's unbind leaves its GMU interrupts requested (kgsl_hfi_irq / kgsl_gmu_irq); drop them so we can take the lines */
void q2_free_kgsl_irq(int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);
	struct irqaction *action = desc ? desc->action : NULL;

	if (action && action->name && !strncmp(action->name, "kgsl_", 5)) {
		pr_info("q2msm: freeing KGSL's leftover irq %d (%s)\n", irq, action->name);
		free_irq(irq, action->dev_id);
	}
}

/* KGSL tells the AOP "ACD on" at boot and then gives the GMU an ACD table; mainline sends no ACD table, so put the
 * AOP back to ACD off (same QMP message KGSL uses, over the GMU node's "aop" mailbox) */
struct q2_qmp_msg {
	u32 len;
	void *msg;
};

void q2_aop_acd_off(struct device *gmu_dev)
{
	struct mbox_client cl = {
		.dev = gmu_dev,
		.tx_block = true,
		.tx_tout = 1000,
		.knows_txdone = false,
	};
	struct mbox_chan *chan;
	struct q2_qmp_msg m;
	char buf[33];
	int ret;

	chan = mbox_request_channel(&cl, 0);
	if (IS_ERR(chan)) {
		pr_err("q2msm: no AOP mailbox (%ld)\n", PTR_ERR(chan));
		return;
	}
	m.len = scnprintf(buf, sizeof(buf), "{class: gpu, res: acd, value: %d}", q2_cfg.acd);
	m.msg = buf;
	ret = mbox_send_message(chan, &m);
	pr_info("q2msm: AOP acd %d: %d\n", q2_cfg.acd, ret);
	mbox_free_channel(chan);
}

/*
 * Bus votes the way KGSL does them (there is no interconnect framework on 4.19): the DDR table is the GPU node's
 * qcom,gpu-bus-table-ddr<N> child for this board's DDR type, the CNOC table the GMU node's own msm-bus properties.
 * They give the GMU's HFI bw table (exact RPMh TCS commands) and the CPU-side DDR vote while the GMU is up.
 */
struct msm_bus_scale_pdata *msm_bus_cl_get_pdata(struct platform_device *pdev);
struct msm_bus_scale_pdata *msm_bus_pdata_from_node(struct platform_device *pdev, struct device_node *of_node);
int of_fdt_get_ddrtype(void);

static u32 q2_pcl, q2_ccl;
static int q2_nbw, q2_ncnoc;

int q2_bus_init(struct platform_device *gpu_pdev, struct platform_device *gmu_pdev)
{
	struct device_node *np = NULL, *child;
	struct msm_bus_scale_pdata *t;
	char compat[40];

	if (q2_pcl)
		return 0;
	snprintf(compat, sizeof(compat), "qcom,gpu-bus-table-ddr%d", of_fdt_get_ddrtype());
	for_each_child_of_node(gpu_pdev->dev.of_node, child)
		if (of_device_is_compatible(child, compat)) {
			np = child;
			break;
		}
	if (!np)
		np = of_get_compatible_child(gpu_pdev->dev.of_node, "qcom,gpu-bus-table");
	if (!np)
		return -ENODEV;
	t = msm_bus_pdata_from_node(gpu_pdev, np);
	if (!t)
		return -ENODEV;
	q2_nbw = t->num_usecases;
	q2_pcl = msm_bus_scale_register_client(t);

	t = msm_bus_cl_get_pdata(gmu_pdev);
	if (t) {
		q2_ncnoc = t->num_usecases;
		q2_ccl = msm_bus_scale_register_client(t);
	}
	pr_info("q2msm: bus %s: ddr client %u (%d levels), cnoc client %u (%d levels)\n",
		np->name, q2_pcl, q2_nbw, q2_ccl, q2_ncnoc);
	return q2_pcl ? 0 : -ENODEV;
}

void q2_bus_exit(void)
{
	if (q2_ccl)
		msm_bus_scale_unregister_client(q2_ccl);
	if (q2_pcl)
		msm_bus_scale_unregister_client(q2_pcl);
	q2_ccl = q2_pcl = 0;
}

int q2_bus_levels(bool cnoc)
{
	return cnoc ? q2_ncnoc : q2_nbw;
}

/* fills h->usecases[0 .. levels-1] with the RPMh commands for every level of that table */
int q2_bus_query(bool cnoc, struct msm_bus_tcs_handle *h)
{
	u32 cl = cnoc ? q2_ccl : q2_pcl;

	if (!cl)
		return -ENODEV;
	return msm_bus_scale_query_tcs_cmd_all(h, cl);
}

/* CPU-side DDR vote: a middle level while the GMU is up (KGSL votes its default level's bus_min), 0 when down */
void q2_bus_vote(bool on)
{
	if (q2_pcl)
		msm_bus_scale_client_update_request(q2_pcl, on ? q2_nbw / 2 : 0);
}

/*
 * The a650 GMU firmware rejects the first GX vote ("GmuPwrGxBwVote", error 115 = HFI_VALUE_LM_CS0) unless limits
 * management is configured, and KGSL also hands it an ACD table first.  Rebuild exactly what KGSL sends
 * (adreno.c / kgsl_gmu.c / kgsl_hfi.c), from the same sources: the speed bin's qcom,gpu-pwrlevels-N node
 * (qcom,acd-level per level, qcom,throttle-pwrlevel) and the isense_slope / speed_bin fuses.
 * Indices are KGSL's GMU perf table: [0] = off, then the bin's levels lowest first (including its 0 Hz level).
 */
struct q2_gpu_cfg q2_cfg;

static int q2_fuse(struct device *dev, const char *name, u32 *val)
{
	struct nvmem_cell *cell = nvmem_cell_get(dev, name);
	size_t len;
	void *buf;

	if (IS_ERR(cell))
		return PTR_ERR(cell);
	buf = nvmem_cell_read(cell, &len);
	nvmem_cell_put(cell);
	if (IS_ERR(buf))
		return PTR_ERR(buf);
	*val = 0;
	memcpy(val, buf, min_t(size_t, len, sizeof(*val)));
	kfree(buf);
	return 0;
}

void q2_gpu_cfg_init(struct device *gpu_dev)
{
	struct device_node *bins, *bin = NULL, *child, *lvl[10] = { NULL };
	u32 speed_bin = 0, b, throttle, acd, n = 0;
	int i, ret;

	memset(&q2_cfg, 0, sizeof(q2_cfg));
	ret = q2_fuse(gpu_dev, "speed_bin", &speed_bin);
	bins = of_get_child_by_name(gpu_dev->of_node, "qcom,gpu-pwrlevel-bins");
	for_each_child_of_node(bins, child)
		if (!of_property_read_u32(child, "qcom,speed-bin", &b) && b == speed_bin) {
			bin = child;
			break;
		}
	if (!bin) {
		pr_err("q2msm: no pwrlevel bin for speed bin %u (fuse read %d)\n", speed_bin, ret);
		return;
	}
	for_each_child_of_node(bin, child)
		if (!of_property_read_u32(child, "reg", &b) && b < ARRAY_SIZE(lvl)) {
			lvl[b] = child;
			n = max(n, b + 1);
		}

	/* kgsl_gmu.c: acd_level = pwrlevels[numlvl - i] for i = 0 .. numlvl - 1 (pwrlevels[numlvl] is empty) */
	for (i = 0; i < n; i++) {
		struct device_node *l = (n - i) < ARRAY_SIZE(lvl) ? lvl[n - i] : NULL;

		if (l && !of_property_read_u32(l, "qcom,acd-level", &acd) && acd && q2_cfg.acd_num < 6) {
			q2_cfg.acd_enable_by_level |= 1 << i;
			q2_cfg.acd_data[q2_cfg.acd_num++] = acd;
		}
	}
	q2_cfg.acd = q2_cfg.acd_num > 0;

	/* adreno.c adreno_of_get_limits */
	if (!of_property_read_u32(bin, "qcom,throttle-pwrlevel", &throttle) && n) {
		throttle = min(throttle, n - 1);
		q2_cfg.throttle_mask = GENMASK(n - 1, n - 1 - throttle);
		q2_cfg.lm = true;
	}
	q2_fuse(gpu_dev, "isense_slope", &q2_cfg.lm_slope);

	pr_info("q2msm: speed bin %u, %u levels: acd %s (enable %02x, %u levels), lm %s (throttle %02x, slope %u)\n",
		speed_bin, n, q2_cfg.acd ? "on" : "off", q2_cfg.acd_enable_by_level, q2_cfg.acd_num,
		q2_cfg.lm ? "on" : "off", q2_cfg.throttle_mask, q2_cfg.lm_slope);
}
