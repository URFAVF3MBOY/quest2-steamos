// SPDX-License-Identifier: GPL-2.0-only
/*
 * Meta Quest 2 (hollywood) JDI LCD: one 1920x3664 panel (both eyes) on two bonded DSI links,
 * video mode with DSC (480x16 slices, 8 bpc, 8 bpp), 90 Hz.
 *
 * The bootloader powers the panel and shows its splash; the first prepare keeps it as it is.
 * Supplies and the backlight stay as the bootloader set them.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>

#include <drm/display/drm_dsc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct hollywood_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi[2];
	struct drm_dsc_config dsc;
	struct gpio_desc *enable;
	struct gpio_desc *reset;
	bool boot_on;
};

static inline struct hollywood_panel *to_hollywood(struct drm_panel *panel)
{
	return container_of(panel, struct hollywood_panel, panel);
}

static const struct drm_display_mode hollywood_mode = {
	.clock = (1920 + 112 + 24 + 88) * (3664 + 1231 + 1 + 20) * 90 / 1000,
	.hdisplay = 1920,
	.hsync_start = 1920 + 112,
	.hsync_end = 1920 + 112 + 24,
	.htotal = 1920 + 112 + 24 + 88,
	.vdisplay = 3664,
	.vsync_start = 3664 + 1231,
	.vsync_end = 3664 + 1231 + 1,
	.vtotal = 3664 + 1231 + 1 + 20,
	.width_mm = 76,
	.height_mm = 145,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

struct hollywood_cmd {
	bool dcs;
	u16 wait_ms;
	u8 len;
	u8 data[160];
};

#define G(w, ...) { false, w, sizeof((u8[]){ __VA_ARGS__ }), { __VA_ARGS__ } }
#define D(w, ...) { true, w, sizeof((u8[]){ __VA_ARGS__ }), { __VA_ARGS__ } }

static const struct hollywood_cmd hollywood_on[] = {
	G(0, 0xb0, 0x04),
	G(0, 0xd6, 0x00),
	G(0, 0xb6, 0x30, 0x6b, 0x00, 0x06, 0x33, 0x8a, 0x00, 0x1a, 0x7a),
	G(0, 0xb7, 0x52, 0x00, 0x00, 0x00),
	G(0, 0xb9, 0x11, 0x5a, 0x01, 0xec, 0x00, 0x00, 0x00, 0x00, 0x03, 0x21, 0x01, 0xec),
	G(0, 0xc0, 0x47, 0x86, 0x1a, 0x00, 0x0e, 0x50, 0x04, 0xca),
	G(0, 0xf1, 0x12),
	G(0, 0xc6, 0x40),
	G(0, 0xf1, 0x1e),
	G(0, 0xc6, 0x80, 0x07, 0x80, 0x02, 0x1d, 0x4f, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	  0x00, 0x00),
	G(0, 0xcd, 0x00),
	G(0, 0xcf, 0x8b, 0x00, 0x80, 0x46, 0x61, 0x00, 0x8b),
	G(0, 0xec, 0x00, 0x00, 0x00, 0x00, 0x00),
	G(0, 0xe6, 0x11, 0x00, 0x00, 0x89, 0x30, 0x80, 0x0e, 0x50, 0x07, 0x80, 0x00, 0x10, 0x01, 0xe0,
	  0x01, 0xe0, 0x02, 0x00, 0x01, 0xf0, 0x00, 0x20, 0x01, 0x6d, 0x00, 0x06, 0x00, 0x0c, 0x06,
	  0x67, 0x07, 0x27, 0x18, 0x00, 0x10, 0xf0, 0x03, 0x0c, 0x20, 0x00, 0x06, 0x0b, 0x0b, 0x33,
	  0x0e, 0x1c, 0x2a, 0x38, 0x46, 0x54, 0x62, 0x69, 0x70, 0x77, 0x79, 0x7b, 0x7d, 0x7e, 0x01,
	  0x02, 0x01, 0x00, 0x09, 0x40, 0x09, 0xbe, 0x19, 0xfc, 0x19, 0xfa, 0x19, 0xf8, 0x1a, 0x38,
	  0x1a, 0x78, 0x1a, 0xb6, 0x2a, 0xf6, 0x2b, 0x34, 0x2b, 0x74, 0x3b, 0x74, 0x63, 0xf4, 0x00,
	  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	D(0, 0x03, 0x01),
	D(0, 0x44, 0x00, 0x00),
	D(0, 0x35, 0x00),
	D(0, 0x36, 0x00),
	D(32, 0x3a, 0x77),
	D(120, 0x29),
	D(120, 0x11),
	G(0, 0xd6, 0x80),
	G(0, 0xb0, 0x03),
};

static const struct hollywood_cmd hollywood_off[] = {
	D(120, 0x28),
	D(0, 0x34),
	D(120, 0x10),
};

static int hollywood_send(struct hollywood_panel *ctx, const struct hollywood_cmd *cmds, int n, bool lpm)
{
	int i, l;
	ssize_t ret;

	for (l = 0; l < 2; l++) {
		if (lpm)
			ctx->dsi[l]->mode_flags |= MIPI_DSI_MODE_LPM;
		else
			ctx->dsi[l]->mode_flags &= ~MIPI_DSI_MODE_LPM;
	}
	for (i = 0; i < n; i++) {
		for (l = 0; l < 2; l++) {
			if (cmds[i].dcs)
				ret = mipi_dsi_dcs_write_buffer(ctx->dsi[l], cmds[i].data, cmds[i].len);
			else
				ret = mipi_dsi_generic_write(ctx->dsi[l], cmds[i].data, cmds[i].len);
			if (ret < 0) {
				dev_err(&ctx->dsi[0]->dev, "command %02x on link %d: %zd\n", cmds[i].data[0], l, ret);
				return ret;
			}
		}
		if (cmds[i].wait_ms)
			msleep(cmds[i].wait_ms);
	}
	return 0;
}

static int hollywood_prepare(struct drm_panel *panel)
{
	struct hollywood_panel *ctx = to_hollywood(panel);
	int ret;

	if (ctx->boot_on) {
		ctx->boot_on = false;
		dev_info(panel->dev, "keeping the panel as the bootloader left it\n");
		return 0;
	}
	gpiod_set_value_cansleep(ctx->enable, 1);
	msleep(20);
	gpiod_set_value_cansleep(ctx->reset, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset, 0);
	msleep(25);
	ret = hollywood_send(ctx, hollywood_on, ARRAY_SIZE(hollywood_on), true);
	if (ret)
		gpiod_set_value_cansleep(ctx->reset, 1);
	return ret;
}

static int hollywood_unprepare(struct drm_panel *panel)
{
	struct hollywood_panel *ctx = to_hollywood(panel);

	hollywood_send(ctx, hollywood_off, ARRAY_SIZE(hollywood_off), false);
	gpiod_set_value_cansleep(ctx->reset, 1);
	msleep(30);
	gpiod_set_value_cansleep(ctx->enable, 0);
	return 0;
}

static int hollywood_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &hollywood_mode);
}

static const struct drm_panel_funcs hollywood_panel_funcs = {
	.prepare = hollywood_prepare,
	.unprepare = hollywood_unprepare,
	.get_modes = hollywood_get_modes,
};

static int hollywood_probe(struct mipi_dsi_device *dsi)
{
	const struct mipi_dsi_device_info info = { .type = "hollywood-jdi", .channel = 0 };
	struct device *dev = &dsi->dev;
	struct hollywood_panel *ctx;
	struct device_node *np1;
	struct mipi_dsi_host *host1;
	int i, ret;

	ctx = devm_drm_panel_alloc(dev, struct hollywood_panel, panel, &hollywood_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->enable = devm_gpiod_get(dev, "enable", GPIOD_ASIS);
	if (IS_ERR(ctx->enable))
		return dev_err_probe(dev, PTR_ERR(ctx->enable), "enable gpio\n");
	ctx->reset = devm_gpiod_get(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(ctx->reset))
		return dev_err_probe(dev, PTR_ERR(ctx->reset), "reset gpio\n");
	ctx->boot_on = gpiod_get_value_cansleep(ctx->enable) == 1 && gpiod_get_value_cansleep(ctx->reset) == 0;
	gpiod_direction_output(ctx->enable, ctx->boot_on);
	gpiod_direction_output(ctx->reset, !ctx->boot_on);
	dev_info(dev, "panel %s at probe\n", ctx->boot_on ? "on" : "off");

	np1 = of_graph_get_remote_node(dev->of_node, 1, -1);
	if (!np1)
		return dev_err_probe(dev, -ENODEV, "no second DSI link\n");
	host1 = of_find_mipi_dsi_host_by_node(np1);
	of_node_put(np1);
	if (!host1)
		return dev_err_probe(dev, -EPROBE_DEFER, "second DSI host not ready\n");

	ctx->dsi[0] = dsi;
	ctx->dsi[1] = devm_mipi_dsi_device_register_full(dev, host1, &info);
	if (IS_ERR(ctx->dsi[1]))
		return dev_err_probe(dev, PTR_ERR(ctx->dsi[1]), "second DSI device\n");

	ctx->dsc = (struct drm_dsc_config) {
		.dsc_version_major = 1,
		.dsc_version_minor = 1,
		.slice_height = 16,
		.slice_width = 480,
		.slice_count = 2,
		.bits_per_component = 8,
		.bits_per_pixel = 8 << 4,
		.block_pred_enable = true,
	};

	ctx->panel.prepare_prev_first = true;
	mipi_dsi_set_drvdata(dsi, ctx);
	drm_panel_add(&ctx->panel);

	for (i = 0; i < 2; i++) {
		struct mipi_dsi_device *link = ctx->dsi[i];

		link->lanes = 4;
		link->format = MIPI_DSI_FMT_RGB888;
		link->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_CLOCK_NON_CONTINUOUS;
		link->dsc = &ctx->dsc;
		ret = devm_mipi_dsi_attach(dev, link);
		if (ret) {
			drm_panel_remove(&ctx->panel);
			return dev_err_probe(dev, ret, "attach link %d\n", i);
		}
	}
	return 0;
}

static void hollywood_remove(struct mipi_dsi_device *dsi)
{
	struct hollywood_panel *ctx = mipi_dsi_get_drvdata(dsi);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id hollywood_of_match[] = {
	{ .compatible = "oculus,hollywood-jdi" },
	{ }
};
MODULE_DEVICE_TABLE(of, hollywood_of_match);

static struct mipi_dsi_driver hollywood_panel_driver = {
	.probe = hollywood_probe,
	.remove = hollywood_remove,
	.driver = {
		.name = "panel-oculus-hollywood-jdi",
		.of_match_table = hollywood_of_match,
	},
};
module_mipi_dsi_driver(hollywood_panel_driver);

MODULE_DESCRIPTION("Meta Quest 2 JDI LCD on bonded DSI with DSC");
MODULE_LICENSE("GPL");
