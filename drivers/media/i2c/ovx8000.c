// SPDX-License-Identifier: GPL-2.0-only
/* OmniVision OVX8000, as fitted to the Xiaomi Vermeer AAC main camera. */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define OVX8000_XCLK		19200000
#define OVX8000_LINK_FREQ	1348800000LL
/* These binned modes use 16 pixel-array clocks per HTS register unit.
 * 400 * 16 * 6250 at 1.2 GHz gives 30 fps, as in the Android tables.
 * This timing clock is distinct from the CSI transport pixel rate.
 */
#define OVX8000_PIXEL_RATE	1200000000
#define OVX8000_HTS		6400
#define OVX8000_VTS_DEFAULT	6250
#define OVX8000_VTS_MAX		65535
#define OVX8000_EXPOSURE_MARGIN	32
#define OVX8000_CODE		MEDIA_BUS_FMT_SRGGB10_1X10

#include "ovx8000-regs.h"

struct ovx8000_mode {
	u32 width;
	u32 height;
	u32 binning;
	struct v4l2_rect crop;
	const struct reg_sequence *regs;
	unsigned int num_regs;
};

static const struct ovx8000_mode ovx8000_modes[] = {
	{
		.width = 4096, .height = 3072, .binning = 2,
		.crop = { .width = 8224, .height = 6176 },
		.regs = ovx8000_mode_4096x3072,
		.num_regs = ARRAY_SIZE(ovx8000_mode_4096x3072),
	}, {
		.width = 4096, .height = 2304, .binning = 2,
		.crop = { .top = 768, .width = 8224, .height = 4640 },
		.regs = ovx8000_mode_4096x2304,
		.num_regs = ARRAY_SIZE(ovx8000_mode_4096x2304),
	}, {
		.width = 1920, .height = 1080, .binning = 4,
		.crop = { .left = 256, .top = 912, .width = 7712, .height = 4352 },
		.regs = ovx8000_mode_1920x1080,
		.num_regs = ARRAY_SIZE(ovx8000_mode_1920x1080),
	},
};

struct ovx8000 {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct regmap *regmap;
	struct clk *xclk;
	struct gpio_desc *reset;
	/* Android order: VAF, VANA, VIO, VDIG. VAF is shared with the lens. */
	struct regulator_bulk_data supplies[4];
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *gain;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
	const struct ovx8000_mode *mode;
};

static const s64 ovx8000_link_freqs[] = { OVX8000_LINK_FREQ };

static inline struct ovx8000 *to_ovx8000(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ovx8000, sd);
}

static int ovx8000_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ovx8000 *sensor = container_of(ctrl->handler, struct ovx8000, ctrls);
	struct device *dev = sensor->sd.dev;
	u32 max_exposure;
	int ret;

	if (ctrl->id == V4L2_CID_VBLANK) {
		max_exposure = sensor->mode->height + ctrl->val - OVX8000_EXPOSURE_MARGIN;
		__v4l2_ctrl_modify_range(sensor->exposure, 4, max_exposure, 1,
					min_t(u32, 1000, max_exposure));
	}
	ret = pm_runtime_get_if_in_use(dev);
	if (ret <= 0)
		return ret;

	ret = 0;
	cci_write(sensor->regmap, CCI_REG8(0x3208), 0x00, &ret);
	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		cci_write(sensor->regmap, CCI_REG24(0x3500), ctrl->val, &ret);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(sensor->regmap, CCI_REG16(0x3508), ctrl->val, &ret);
		break;
	case V4L2_CID_VBLANK:
		cci_write(sensor->regmap, CCI_REG16(0x380e),
			  sensor->mode->height + ctrl->val, &ret);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	/* End and launch group zero even when one payload write failed. */
	{
		int release_ret = 0;

		cci_write(sensor->regmap, CCI_REG8(0x3208), 0x10, &release_ret);
		cci_write(sensor->regmap, CCI_REG8(0x3208), 0xa0, &release_ret);
		if (!ret)
			ret = release_ret;
	}
	pm_runtime_put(dev);
	return ret;
}

static const struct v4l2_ctrl_ops ovx8000_ctrl_ops = {
	.s_ctrl = ovx8000_set_ctrl,
};

static int ovx8000_power_on(struct device *dev)
{
	struct ovx8000 *sensor = to_ovx8000(dev_get_drvdata(dev));
	unsigned int i;
	int ret;

	gpiod_set_value_cansleep(sensor->reset, 1);
	for (i = 0; i < ARRAY_SIZE(sensor->supplies); i++) {
		ret = regulator_enable(sensor->supplies[i].consumer);
		if (ret)
			goto disable_supplies;
		usleep_range(1000, 1500);
	}
	ret = clk_prepare_enable(sensor->xclk);
	if (ret)
		goto disable_supplies;
	usleep_range(1000, 1500);
	gpiod_set_value_cansleep(sensor->reset, 0);
	usleep_range(5000, 6000);
	return 0;

disable_supplies:
	while (i--)
		regulator_disable(sensor->supplies[i].consumer);
	return ret;
}

static int ovx8000_power_off(struct device *dev)
{
	struct ovx8000 *sensor = to_ovx8000(dev_get_drvdata(dev));
	int i;

	gpiod_set_value_cansleep(sensor->reset, 1);
	usleep_range(1000, 1500);
	clk_disable_unprepare(sensor->xclk);
	for (i = ARRAY_SIZE(sensor->supplies) - 1; i >= 0; i--)
		regulator_disable(sensor->supplies[i].consumer);
	return 0;
}

static int ovx8000_enable_streams(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				u32 pad, u64 streams_mask)
{
	struct ovx8000 *sensor = to_ovx8000(sd);
	int ret;

	ret = pm_runtime_resume_and_get(sd->dev);
	if (ret)
		return ret;
	ret = regmap_multi_reg_write(sensor->regmap, ovx8000_init_regs,
				     ARRAY_SIZE(ovx8000_init_regs));
	if (ret)
		goto put_pm;
	ret = regmap_multi_reg_write(sensor->regmap, sensor->mode->regs,
				     sensor->mode->num_regs);
	if (ret)
		goto put_pm;
	ret = __v4l2_ctrl_handler_setup(&sensor->ctrls);
	if (ret)
		goto put_pm;
	ret = cci_write(sensor->regmap, CCI_REG8(0x0100), 1, NULL);
	if (!ret)
		return 0;
put_pm:
	cci_write(sensor->regmap, CCI_REG8(0x0100), 0, NULL);
	pm_runtime_put(sd->dev);
	return ret;
}

static int ovx8000_disable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 u32 pad, u64 streams_mask)
{
	struct ovx8000 *sensor = to_ovx8000(sd);
	int ret;

	ret = 0;
	cci_write(sensor->regmap, CCI_REG8(0x3208), 0x11, &ret);
	cci_write(sensor->regmap, CCI_REG8(0x0100), 0, &ret);
	pm_runtime_mark_last_busy(sd->dev);
	pm_runtime_put_autosuspend(sd->dev);
	return ret;
}

static void ovx8000_format(struct v4l2_mbus_framefmt *fmt,
			   const struct ovx8000_mode *mode)
{
	*fmt = (struct v4l2_mbus_framefmt) {
		.width = mode->width, .height = mode->height,
		.code = OVX8000_CODE, .field = V4L2_FIELD_NONE,
		.colorspace = V4L2_COLORSPACE_RAW,
		.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT,
		.quantization = V4L2_QUANTIZATION_FULL_RANGE,
		.xfer_func = V4L2_XFER_FUNC_NONE,
	};
}

static int ovx8000_init_state(struct v4l2_subdev *sd, struct v4l2_subdev_state *state)
{
	ovx8000_format(v4l2_subdev_state_get_format(state, 0), &ovx8000_modes[0]);
	return 0;
}

static int ovx8000_enum_code(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			    struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad || code->stream || code->index)
		return -EINVAL;
	code->code = OVX8000_CODE;
	return 0;
}

static int ovx8000_enum_size(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			    struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->pad || fse->stream || fse->code != OVX8000_CODE ||
	    fse->index >= ARRAY_SIZE(ovx8000_modes))
		return -EINVAL;
	fse->min_width = fse->max_width = ovx8000_modes[fse->index].width;
	fse->min_height = fse->max_height = ovx8000_modes[fse->index].height;
	return 0;
}

static int ovx8000_set_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	struct ovx8000 *sensor = to_ovx8000(sd);
	const struct ovx8000_mode *mode;
	u32 vblank;

	if (fmt->pad || fmt->stream)
		return -EINVAL;
	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE && v4l2_subdev_is_streaming(sd))
		return -EBUSY;
	mode = v4l2_find_nearest_size(ovx8000_modes, ARRAY_SIZE(ovx8000_modes),
				     width, height, fmt->format.width, fmt->format.height);
	ovx8000_format(&fmt->format, mode);
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;
	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		sensor->mode = mode;
		__v4l2_ctrl_modify_range(sensor->hblank, OVX8000_HTS - mode->width,
			OVX8000_HTS - mode->width, 1, OVX8000_HTS - mode->width);
		vblank = OVX8000_VTS_DEFAULT - mode->height;
		__v4l2_ctrl_modify_range(sensor->vblank, vblank,
			OVX8000_VTS_MAX - mode->height, 1, vblank);
		__v4l2_ctrl_s_ctrl(sensor->vblank, vblank);
	}
	return 0;
}

static int ovx8000_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	struct v4l2_mbus_framefmt *fmt = v4l2_subdev_state_get_format(state, 0);
	const struct ovx8000_mode *mode;

	if (sel->pad || sel->stream)
		return -EINVAL;
	mode = v4l2_find_nearest_size(ovx8000_modes, ARRAY_SIZE(ovx8000_modes),
				     width, height, fmt->width, fmt->height);
	switch (sel->target) {
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r = ovx8000_modes[0].crop;
		return 0;
	case V4L2_SEL_TGT_CROP:
		sel->r = mode->crop;
		return 0;
	default:
		return -EINVAL;
	}
}

static int ovx8000_get_mbus_config(struct v4l2_subdev *sd, unsigned int pad,
				  struct v4l2_mbus_config *config)
{
	if (pad)
		return -EINVAL;
	config->type = V4L2_MBUS_CSI2_CPHY;
	config->bus.mipi_csi2.num_data_lanes = 3;
	config->bus.mipi_csi2.data_lanes[0] = 0;
	config->bus.mipi_csi2.data_lanes[1] = 1;
	config->bus.mipi_csi2.data_lanes[2] = 2;
	return 0;
}

static const struct v4l2_subdev_pad_ops ovx8000_pad_ops = {
	.enum_mbus_code = ovx8000_enum_code,
	.enum_frame_size = ovx8000_enum_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = ovx8000_set_fmt,
	.get_selection = ovx8000_get_selection,
	.get_mbus_config = ovx8000_get_mbus_config,
	.enable_streams = ovx8000_enable_streams,
	.disable_streams = ovx8000_disable_streams,
};

static const struct v4l2_subdev_video_ops ovx8000_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_ops ovx8000_ops = {
	.video = &ovx8000_video_ops,
	.pad = &ovx8000_pad_ops,
};

static const struct v4l2_subdev_internal_ops ovx8000_internal_ops = {
	.init_state = ovx8000_init_state,
};

static int ovx8000_init_controls(struct ovx8000 *sensor)
{
	struct v4l2_ctrl_handler *hdl = &sensor->ctrls;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *ctrl;
	u32 vblank = OVX8000_VTS_DEFAULT - sensor->mode->height;
	int ret;

	v4l2_ctrl_handler_init(hdl, 8);
	sensor->sd.ctrl_handler = hdl;
	sensor->vblank = v4l2_ctrl_new_std(hdl, &ovx8000_ctrl_ops, V4L2_CID_VBLANK,
		vblank, OVX8000_VTS_MAX - sensor->mode->height, 1, vblank);
	sensor->exposure = v4l2_ctrl_new_std(hdl, &ovx8000_ctrl_ops, V4L2_CID_EXPOSURE,
		4, OVX8000_VTS_DEFAULT - OVX8000_EXPOSURE_MARGIN, 1, 1000);
	sensor->gain = v4l2_ctrl_new_std(hdl, &ovx8000_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
		256, 0x3fc0, 4, 256);
	sensor->hblank = v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_HBLANK,
		OVX8000_HTS - sensor->mode->width, OVX8000_HTS - sensor->mode->width,
		1, OVX8000_HTS - sensor->mode->width);
	if (sensor->hblank)
		sensor->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	ctrl = v4l2_ctrl_new_int_menu(hdl, NULL, V4L2_CID_LINK_FREQ, 0, 0, ovx8000_link_freqs);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_PIXEL_RATE, OVX8000_PIXEL_RATE,
			  OVX8000_PIXEL_RATE, 1, OVX8000_PIXEL_RATE);
	if (hdl->error)
		return hdl->error;
	ret = v4l2_fwnode_device_parse(sensor->sd.dev, &props);
	if (ret)
		return ret;
	return v4l2_ctrl_new_fwnode_properties(hdl, &ovx8000_ctrl_ops, &props);
}

static int ovx8000_check_endpoint(struct device *dev)
{
	struct v4l2_fwnode_endpoint ep = { .bus_type = V4L2_MBUS_CSI2_CPHY };
	struct fwnode_handle *endpoint;
	u64 frequencies;
	unsigned int i;
	int ret;

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!endpoint)
		return -EINVAL;
	ret = v4l2_fwnode_endpoint_parse(endpoint, &ep);
	if (!ret)
		ret = fwnode_property_read_u64(endpoint, "link-frequencies", &frequencies);
	fwnode_handle_put(endpoint);
	if (ret)
		return ret;
	if (ep.bus_type != V4L2_MBUS_CSI2_CPHY || ep.bus.mipi_csi2.num_data_lanes != 3 ||
	    frequencies != OVX8000_LINK_FREQ)
		return -EINVAL;
	for (i = 0; i < 3; i++)
		if (ep.bus.mipi_csi2.data_lanes[i] != i)
			return -EINVAL;
	return 0;
}

static int ovx8000_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ovx8000 *sensor;
	u64 id;
	int ret;

	ret = ovx8000_check_endpoint(dev);
	if (ret)
		return dev_err_probe(dev, ret, "requires three C-PHY trios and the supported link rate\n");
	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;
	sensor->mode = &ovx8000_modes[0];
	sensor->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(sensor->regmap))
		return PTR_ERR(sensor->regmap);
	sensor->xclk = devm_clk_get(dev, NULL);
	if (IS_ERR(sensor->xclk))
		return dev_err_probe(dev, PTR_ERR(sensor->xclk), "cannot get clock\n");
	if (clk_get_rate(sensor->xclk) != OVX8000_XCLK)
		return dev_err_probe(dev, -EINVAL, "requires a 19.2 MHz clock\n");
	sensor->supplies[0].supply = "afvdd";
	sensor->supplies[1].supply = "avdd";
	sensor->supplies[2].supply = "dovdd";
	sensor->supplies[3].supply = "dvdd";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(sensor->supplies), sensor->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "cannot get supplies\n");
	sensor->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset))
		return dev_err_probe(dev, PTR_ERR(sensor->reset), "cannot get reset\n");
	v4l2_i2c_subdev_init(&sensor->sd, client, &ovx8000_ops);
	sensor->sd.internal_ops = &ovx8000_internal_ops;
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = ovx8000_power_on(dev);
	if (ret)
		return ret;
	ret = cci_read(sensor->regmap, CCI_REG16(0x300a), &id, NULL);
	if (ret)
		goto power_off;
	if (id != 0x5650) {
		ret = dev_err_probe(dev, -ENODEV, "unexpected sensor ID %#llx\n", id);
		goto power_off;
	}
	ret = ovx8000_init_controls(sensor);
	if (ret)
		goto free_controls;
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret)
		goto free_controls;
	sensor->sd.state_lock = sensor->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&sensor->sd);
	if (ret)
		goto entity_cleanup;
	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if (ret)
		goto subdev_cleanup;
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
	return 0;

subdev_cleanup:
	pm_runtime_disable(dev);
	pm_runtime_put_noidle(dev);
	v4l2_subdev_cleanup(&sensor->sd);
entity_cleanup:
	media_entity_cleanup(&sensor->sd.entity);
free_controls:
	v4l2_ctrl_handler_free(&sensor->ctrls);
power_off:
	ovx8000_power_off(dev);
	pm_runtime_set_suspended(dev);
	return ret;
}

static void ovx8000_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ovx8000 *sensor = to_ovx8000(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&sensor->ctrls);
	pm_runtime_disable(sd->dev);
	if (!pm_runtime_status_suspended(sd->dev))
		ovx8000_power_off(sd->dev);
	pm_runtime_set_suspended(sd->dev);
}

static const struct dev_pm_ops ovx8000_pm_ops = {
	SET_RUNTIME_PM_OPS(ovx8000_power_off, ovx8000_power_on, NULL)
};

static const struct of_device_id ovx8000_of_match[] = {
	{ .compatible = "ovti,ovx8000" },
	{ }
};
MODULE_DEVICE_TABLE(of, ovx8000_of_match);

static struct i2c_driver ovx8000_driver = {
	.driver = {
		.name = "ovx8000",
		.of_match_table = ovx8000_of_match,
		.pm = &ovx8000_pm_ops,
	},
	.probe = ovx8000_probe,
	.remove = ovx8000_remove,
};
module_i2c_driver(ovx8000_driver);

MODULE_DESCRIPTION("OmniVision OVX8000 camera sensor");
MODULE_LICENSE("GPL");
