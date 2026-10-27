// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 FIXME
// Generated with linux-mdss-dsi-panel-driver-generator from vendor device tree:
//   Copyright (c) 2013, The Linux Foundation. All rights reserved. (FIXME)

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_connector.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_probe_helper.h>

struct n11_42_02_0a_dsc {
	struct drm_bridge bridge;
	struct backlight_device *backlight;
	struct mipi_dsi_device *dsi;
	struct drm_dsc_config dsc;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data *supplies;
	struct mutex lock; /* Serialize backlight writes with power and mode changes. */
	unsigned int refresh_rate;
	bool prepared;
	bool enabled;
};

enum n11_42_02_0a_dsc_supply {
	N11_VDDIO,
	N11_VDDD,
	N11_VDD,
	N11_VCI,
};

static const struct regulator_bulk_data n11_42_02_0a_dsc_supplies[] = {
	[N11_VDDIO] = { .supply = "vddio" },
	[N11_VDDD] = { .supply = "vddd" },
	[N11_VDD] = { .supply = "vdd" },
	[N11_VCI] = { .supply = "vci" },
};

static inline
struct n11_42_02_0a_dsc *to_n11_42_02_0a_dsc(struct drm_bridge *bridge)
{
	return container_of(bridge, struct n11_42_02_0a_dsc, bridge);
}

static void n11_42_02_0a_dsc_reset(struct n11_42_02_0a_dsc *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(11000, 11100);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(11000, 11100);
}

static int n11_42_02_0a_dsc_power_on(struct n11_42_02_0a_dsc *ctx)
{
	int ret;

	/* Vermeer: L4B -> GPIO42 VDDD -> L11B -> L13B. */
	ret = regulator_enable(ctx->supplies[N11_VDDIO].consumer);
	if (ret)
		return ret;
	usleep_range(1000, 2000);

	ret = regulator_enable(ctx->supplies[N11_VDDD].consumer);
	if (ret)
		goto err_disable_vddio;
	usleep_range(1000, 2000);

	ret = regulator_enable(ctx->supplies[N11_VDD].consumer);
	if (ret)
		goto err_disable_vddd;

	ret = regulator_enable(ctx->supplies[N11_VCI].consumer);
	if (ret)
		goto err_disable_vdd;
	usleep_range(10000, 11000);

	return 0;

err_disable_vdd:
	regulator_disable(ctx->supplies[N11_VDD].consumer);
	usleep_range(3000, 4000);
err_disable_vddd:
	usleep_range(15000, 16000);
	regulator_disable(ctx->supplies[N11_VDDD].consumer);
err_disable_vddio:
	msleep(25);
	regulator_disable(ctx->supplies[N11_VDDIO].consumer);

	return ret;
}

static void n11_42_02_0a_dsc_power_off(struct n11_42_02_0a_dsc *ctx)
{
	regulator_disable(ctx->supplies[N11_VCI].consumer);
	regulator_disable(ctx->supplies[N11_VDD].consumer);
	/* L11B post-off delay (3 ms) plus VDDD pre-off delay (15 ms). */
	usleep_range(18000, 19000);
	regulator_disable(ctx->supplies[N11_VDDD].consumer);
	msleep(25);
	regulator_disable(ctx->supplies[N11_VDDIO].consumer);
}

static void n11_42_02_0a_dsc_write_reg_multi(struct mipi_dsi_multi_context *dsi_ctx,
					     u8 reg, u8 value)
{
	const u8 data[] = { reg, value };

	mipi_dsi_dcs_write_buffer_multi(dsi_ctx, data, sizeof(data));
}

static int n11_42_02_0a_dsc_on(struct n11_42_02_0a_dsc *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	/* Like the reference, enter 90 Hz from the 60 Hz initialization table. */
	bool init60 = ctx->refresh_rate != 120;

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x1a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb5, 0x16);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb5, 0x15);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x23);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb5, 0x15);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd2,
				     0x00, 0x00, 0x00, 0x00, 0x15);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x05);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd2, 0x01, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd2,
				     0x00, 0x00, 0x10, 0x13, 0x32);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe4,
				     0x80, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x33);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe4,
				     0x80, 0x03, 0x00, 0x10, 0x00, 0x53);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd8, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x09);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd8, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd8, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd2, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x1a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc2, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb4, 0x1f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf5, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc4, 0xe3);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x18);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf8, 0x19);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd2, 0x2a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc5, 0x0b, 0x0b, 0x0b);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc6,
				     0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
				     0x55);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x1b);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf4, 0x55);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x19);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf2, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x81);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x18);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb, 0x04);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x81);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x07);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfc, 0x08);
	mipi_dsi_dcs_set_column_address_multi(&dsi_ctx, 0x0000, 0x059f);
	mipi_dsi_dcs_set_page_address_multi(&dsi_ctx, 0x0000, 0x0c7f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x90, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x91,
				     0xab, 0xa8, 0x00, 0x20, 0xd2, 0x00, 0x02,
				     0x86, 0x03, 0x5e, 0x00, 0x0a, 0x03, 0x5b,
				     0x02, 0x63, 0x10, 0xf0);
	mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x5a, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x3b, 0x00, 0x18, 0x00, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY,
				     0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x51, 0x0f, 0xff);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x9c, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_MEMORY_START,
				     0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x11);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb2,
				     0x0f, 0x2f, 0x4f, 0x0b, 0x03, 0x00, 0x00);
	n11_42_02_0a_dsc_write_reg_multi(&dsi_ctx, 0x2f, init60 ? 0x02 : 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x5f, 0x00);
	n11_42_02_0a_dsc_write_reg_multi(&dsi_ctx, MIPI_DCS_SET_GAMMA_CURVE,
					 init60 ? 0x02 : 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x8b, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc0, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x31);
	n11_42_02_0a_dsc_write_reg_multi(&dsi_ctx, 0xdf,
					 init60 ? 0x21 : 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xcc, 0x30);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xce, 0x01);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xcc, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3,
				     0x90, 0x04, 0x20, 0x13, 0xff, 0x00, 0x04,
				     0x20, 0x0e, 0xff, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0b);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3,
				     0x06, 0x38, 0x1d, 0x06, 0x58, 0x17);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x11);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3,
				     0x06, 0x45, 0x1d, 0x06, 0x41, 0x16);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x17);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3,
				     0x06, 0x38, 0x1d, 0x06, 0x58, 0x17);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x1d);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3,
				     0x06, 0x38, 0x1d, 0x06, 0x41, 0x16);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x23);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3,
				     0x06, 0x38, 0x1d, 0x06, 0x58, 0x17);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x07);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc0, 0xb1);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbe, 0x45);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x05);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbe, 0x88);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x1a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb5, 0x16);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb5, 0x15);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x23);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb5, 0x15);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd2,
				     0x00, 0x00, 0x00, 0x01, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x05);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd2, 0x01, 0x01, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd2,
				     0x03, 0x00, 0x10, 0x03, 0xe4);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe4,
				     0x80, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x33);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe4,
				     0x80, 0x03, 0x00, 0x10, 0x00, 0x53);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd8, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x09);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd8, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd8, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc7, 0x37);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x18);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb4,
				     0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00,
				     0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
				     0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00,
				     0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x46);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb4,
				     0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00,
				     0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
				     0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00,
				     0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe4, 0x00, 0x00);

	return dsi_ctx.accum_err;
}

static int n11_42_02_0a_dsc_prepare(struct n11_42_02_0a_dsc *ctx)
{
	struct device *dev = &ctx->dsi->dev;
	struct drm_dsc_picture_parameter_set pps;
	int ret;

	ret = n11_42_02_0a_dsc_power_on(ctx);
	if (ret) {
		dev_err(dev, "Failed to power on panel: %d\n", ret);
		return ret;
	}

	n11_42_02_0a_dsc_reset(ctx);

	ret = n11_42_02_0a_dsc_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		goto err_power_off;
	}

	drm_dsc_pps_payload_pack(&pps, &ctx->dsc);

	ret = mipi_dsi_picture_parameter_set(ctx->dsi, &pps);
	if (ret < 0) {
		dev_err(dev, "failed to transmit PPS: %d\n", ret);
		goto err_power_off;
	}

	ret = mipi_dsi_compression_mode(ctx->dsi, true);
	if (ret < 0) {
		dev_err(dev, "failed to enable compression mode: %d\n", ret);
		goto err_power_off;
	}

	ctx->prepared = true;
	return 0;

err_power_off:
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	n11_42_02_0a_dsc_power_off(ctx);

	return ret;
}

/* Apply the vendor's HS timing switch after sleep-out and display-on. */
static int n11_42_02_0a_dsc_timing_switch(struct n11_42_02_0a_dsc *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	unsigned long flags = ctx->dsi->mode_flags;
	unsigned int frame_us = DIV_ROUND_UP(1000000, ctx->refresh_rate);
	bool hz60 = ctx->refresh_rate == 60;
	bool hz90 = ctx->refresh_rate == 90;

	ctx->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
	n11_42_02_0a_dsc_write_reg_multi(&dsi_ctx, 0x2f,
					 hz60 ? 0x02 : hz90 ? 0x01 : 0x00);
	/* The reference's 90 Hz sequence omits the gamma selector (0x26). */
	if (!hz90)
		n11_42_02_0a_dsc_write_reg_multi(&dsi_ctx, 0x26, hz60 ? 0x02 : 0x00);
	n11_42_02_0a_dsc_write_reg_multi(&dsi_ctx, 0x8b, hz90 ? 0x10 : 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc0, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x31);
	n11_42_02_0a_dsc_write_reg_multi(&dsi_ctx, 0xdf, hz60 ? 0x21 : 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xcc, 0x30);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xce, 0x01);
	/* Latch the new gamma/timing after one frame, as in the vendor switch. */
	mipi_dsi_usleep_range(&dsi_ctx, frame_us, frame_us + 1000);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xcc, 0x00);
	ctx->dsi->mode_flags = flags;

	return dsi_ctx.accum_err;
}

static int n11_42_02_0a_dsc_enable(struct n11_42_02_0a_dsc *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	int ret;

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	if (dsi_ctx.accum_err)
		return dsi_ctx.accum_err;

	ret = n11_42_02_0a_dsc_timing_switch(ctx);
	if (!ret)
		ctx->enabled = true;

	return ret;
}

static int n11_42_02_0a_dsc_disable(struct n11_42_02_0a_dsc *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 100);
	ctx->enabled = false;

	return dsi_ctx.accum_err;
}

static void n11_42_02_0a_dsc_unprepare(struct n11_42_02_0a_dsc *ctx)
{
	ctx->prepared = false;
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	n11_42_02_0a_dsc_power_off(ctx);
}

static const struct drm_display_mode n11_42_02_0a_dsc_modes[] = {
	{
		/* 120 Hz Mode (Primary / Preferred) - Exact vendor DT timings */
		.clock = (1440 + 8 + 12 + 8) * (3200 + 16 + 14 + 6) * 120 / 1000,
		.hdisplay = 1440,
		.hsync_start = 1440 + 8,
		.hsync_end = 1440 + 8 + 12,
		.htotal = 1440 + 8 + 12 + 8,
		.vdisplay = 3200,
		.vsync_start = 3200 + 16,
		.vsync_end = 3200 + 16 + 14,
		.vtotal = 3200 + 16 + 14 + 6,
		.width_mm = 70,
		.height_mm = 155,
		.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
	},
	{
		/* 90 Hz uses the same geometry and HS lane rate. */
		.clock = (1440 + 8 + 12 + 8) * (3200 + 16 + 14 + 6) * 90 / 1000,
		.hdisplay = 1440,
		.hsync_start = 1440 + 8,
		.hsync_end = 1440 + 8 + 12,
		.htotal = 1440 + 8 + 12 + 8,
		.vdisplay = 3200,
		.vsync_start = 3200 + 16,
		.vsync_end = 3200 + 16 + 14,
		.vtotal = 3200 + 16 + 14 + 6,
		.width_mm = 70,
		.height_mm = 155,
		.type = DRM_MODE_TYPE_DRIVER,
	},
	{
		/* 60 Hz Mode (Alternative) */
		.clock = (1440 + 8 + 12 + 8) * (3200 + 16 + 14 + 6) * 60 / 1000,
		.hdisplay = 1440,
		.hsync_start = 1440 + 8,
		.hsync_end = 1440 + 8 + 12,
		.htotal = 1440 + 8 + 12 + 8,
		.vdisplay = 3200,
		.vsync_start = 3200 + 16,
		.vsync_end = 3200 + 16 + 14,
		.vtotal = 3200 + 16 + 14 + 6,
		.width_mm = 70,
		.height_mm = 155,
		.type = DRM_MODE_TYPE_DRIVER,
	},
};

static int n11_42_02_0a_dsc_get_modes(struct drm_bridge *bridge,
				      struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(n11_42_02_0a_dsc_modes); i++) {
		mode = drm_mode_duplicate(connector->dev, &n11_42_02_0a_dsc_modes[i]);
		if (!mode)
			return -ENOMEM;

		drm_mode_set_name(mode);
		drm_mode_probed_add(connector, mode);
	}

	connector->display_info.width_mm = 70;
	connector->display_info.height_mm = 155;
	connector->display_info.bpc = 10;

	return ARRAY_SIZE(n11_42_02_0a_dsc_modes);
}

static enum drm_mode_status
n11_42_02_0a_dsc_mode_valid(struct drm_bridge *bridge,
			    const struct drm_display_info *info,
			    const struct drm_display_mode *mode)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(n11_42_02_0a_dsc_modes); i++)
		if (drm_mode_match(mode, &n11_42_02_0a_dsc_modes[i],
				   DRM_MODE_MATCH_TIMINGS | DRM_MODE_MATCH_CLOCK |
				   DRM_MODE_MATCH_FLAGS))
			return MODE_OK;

	return MODE_BAD;
}

static void n11_42_02_0a_dsc_atomic_pre_enable(struct drm_bridge *bridge,
					       struct drm_atomic_commit *state)
{
	struct n11_42_02_0a_dsc *ctx = to_n11_42_02_0a_dsc(bridge);
	struct drm_crtc_state *crtc_state;
	struct drm_crtc *crtc;

	crtc = drm_atomic_get_new_crtc_for_encoder(state, bridge->encoder);
	if (!crtc)
		return;
	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);

	mutex_lock(&ctx->lock);
	ctx->refresh_rate = drm_mode_vrefresh(&crtc_state->adjusted_mode);
	n11_42_02_0a_dsc_prepare(ctx);
	mutex_unlock(&ctx->lock);
}

static void n11_42_02_0a_dsc_atomic_enable(struct drm_bridge *bridge,
					   struct drm_atomic_commit *state)
{
	struct n11_42_02_0a_dsc *ctx = to_n11_42_02_0a_dsc(bridge);
	int ret = -EIO;

	mutex_lock(&ctx->lock);
	if (ctx->prepared)
		ret = n11_42_02_0a_dsc_enable(ctx);
	mutex_unlock(&ctx->lock);
	if (ret) {
		dev_err(&ctx->dsi->dev, "Failed to enable %u Hz panel: %d\n",
			ctx->refresh_rate, ret);
		return;
	}

	backlight_enable(ctx->backlight);
}

static void n11_42_02_0a_dsc_atomic_disable(struct drm_bridge *bridge,
					    struct drm_atomic_commit *state)
{
	struct n11_42_02_0a_dsc *ctx = to_n11_42_02_0a_dsc(bridge);
	int ret = 0;

	backlight_disable(ctx->backlight);
	mutex_lock(&ctx->lock);
	if (ctx->prepared)
		ret = n11_42_02_0a_dsc_disable(ctx);
	mutex_unlock(&ctx->lock);
	if (ret)
		dev_err(&ctx->dsi->dev, "Failed to disable panel: %d\n", ret);
}

static void n11_42_02_0a_dsc_atomic_post_disable(struct drm_bridge *bridge,
						 struct drm_atomic_commit *state)
{
	struct n11_42_02_0a_dsc *ctx = to_n11_42_02_0a_dsc(bridge);

	mutex_lock(&ctx->lock);
	if (ctx->prepared)
		n11_42_02_0a_dsc_unprepare(ctx);
	mutex_unlock(&ctx->lock);
}

static const struct drm_bridge_funcs n11_42_02_0a_dsc_bridge_funcs = {
	.atomic_pre_enable = n11_42_02_0a_dsc_atomic_pre_enable,
	.atomic_enable = n11_42_02_0a_dsc_atomic_enable,
	.atomic_disable = n11_42_02_0a_dsc_atomic_disable,
	.atomic_post_disable = n11_42_02_0a_dsc_atomic_post_disable,
	.mode_valid = n11_42_02_0a_dsc_mode_valid,
	.get_modes = n11_42_02_0a_dsc_get_modes,
	.atomic_reset = drm_atomic_helper_bridge_reset,
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_get_input_bus_fmts = drm_atomic_helper_bridge_propagate_bus_fmt,
};

static int n11_42_02_0a_dsc_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	struct n11_42_02_0a_dsc *ctx = mipi_dsi_get_drvdata(dsi);
	u16 brightness = backlight_get_brightness(bl);
	unsigned long flags;
	int ret = 0;

	if (brightness > 0 && brightness < 25)
		brightness = 25;

	mutex_lock(&ctx->lock);
	if (ctx->enabled) {
		flags = dsi->mode_flags;
		dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
		ret = mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
		dsi->mode_flags = flags;
	}
	mutex_unlock(&ctx->lock);

	return ret;
}

static const struct backlight_ops n11_42_02_0a_dsc_bl_ops = {
	.update_status = n11_42_02_0a_dsc_bl_update_status,
};

static struct backlight_device *
n11_42_02_0a_dsc_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 2048,
		.max_brightness = 4095,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &n11_42_02_0a_dsc_bl_ops, &props);
}

static int n11_42_02_0a_dsc_init_dsc(struct n11_42_02_0a_dsc *ctx)
{
	struct drm_dsc_config *dsc = &ctx->dsc;
	int ret;

	dsc->dsc_version_major = 1;
	dsc->dsc_version_minor = 2;
	dsc->slice_height = 32;
	dsc->slice_width = 720;
	dsc->slice_count = 2;
	dsc->bits_per_component = 10;
	dsc->bits_per_pixel = 8 << 4;
	dsc->block_pred_enable = true;
	dsc->convert_rgb = true;
	dsc->line_buf_depth = 11;
	drm_dsc_set_const_params(dsc);
	drm_dsc_set_rc_buf_thresh(dsc);
	ret = drm_dsc_setup_rc_params(dsc, DRM_DSC_1_2_444);
	if (ret)
		return ret;

	/* N11 uses the vendor's NVT DSC table, not the generic 1.2 table. */
	dsc->first_line_bpg_offset = 13;
	dsc->rc_range_params[9].range_min_qp = 8;
	dsc->rc_range_params[11].range_bpg_offset = -10 & DSC_RANGE_BPG_OFFSET_MASK;
	dsc->rc_range_params[13].range_min_qp = 12;
	dsc->initial_scale_value = drm_dsc_initial_scale_value(dsc);

	return 0;
}

static int n11_42_02_0a_dsc_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct n11_42_02_0a_dsc *ctx;
	int ret;

	ctx = devm_drm_bridge_alloc(dev, struct n11_42_02_0a_dsc, bridge,
				    &n11_42_02_0a_dsc_bridge_funcs);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	/* Hold reset low during DSI host setup, as after unprepare(). */
	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ret = devm_regulator_bulk_get_const(dev,
					  ARRAY_SIZE(n11_42_02_0a_dsc_supplies),
					  n11_42_02_0a_dsc_supplies,
					  &ctx->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get panel supplies\n");

	ctx->dsi = dsi;
	mutex_init(&ctx->lock);
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM |
			  MIPI_DSI_MODE_DSC_ALL_SLICES_IN_PKT;
	/* The vendor uses the same 1.2 Gbit/s lane rate for both refresh modes. */
	dsi->hs_rate = 1200000000;
	dsi->dsc_slice_per_pkt = 2;
	dsi->dsc = &ctx->dsc;
	ret = n11_42_02_0a_dsc_init_dsc(ctx);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to configure DSC\n");

	ctx->backlight = n11_42_02_0a_dsc_create_backlight(dsi);
	if (IS_ERR(ctx->backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->backlight),
				     "Failed to create backlight\n");

	/* A bridge receives the selected atomic mode before powering the panel. */
	ctx->bridge.of_node = dev->of_node;
	ctx->bridge.type = DRM_MODE_CONNECTOR_DSI;
	ctx->bridge.ops = DRM_BRIDGE_OP_MODES;
	ctx->bridge.pre_enable_prev_first = true;
	ret = devm_drm_bridge_add(dev, &ctx->bridge);
	if (ret)
		return ret;

	return devm_mipi_dsi_attach(dev, dsi);
}

static const struct of_device_id n11_42_02_0a_dsc_of_match[] = {
	{ .compatible = "mdss,n11-42-02-0a-dsc" }, // FIXME
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, n11_42_02_0a_dsc_of_match);

static struct mipi_dsi_driver n11_42_02_0a_dsc_driver = {
	.probe = n11_42_02_0a_dsc_probe,
	.driver = {
		.name = "panel-n11-42-02-0a-dsc",
		.of_match_table = n11_42_02_0a_dsc_of_match,
	},
};
module_mipi_dsi_driver(n11_42_02_0a_dsc_driver);

MODULE_AUTHOR("linux-mdss-dsi-panel-driver-generator <fix@me>"); // FIXME
MODULE_DESCRIPTION("DRM driver for xiaomi n11 42 02 0a cmd mode dsc dsi panel");
MODULE_LICENSE("GPL");
