// SPDX-License-Identifier: GPL-2.0-only
/* Will Semiconductor WL2868C seven-channel camera PMIC. */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>

#define WL2868C_DEVICE_ID		0x00
#define WL2868C_DEVICE_ID_VALUE		0x82
#define WL2868C_LDO1_VOUT			0x03
#define WL2868C_LDO_ENABLE		0x0e
#define WL2868C_SYS_ENABLE		BIT(7)
#define WL2868C_MAX_REGISTER		0x25

static const struct regulator_ops wl2868c_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
};

#define WL2868C_LDO(_num, _supply, _min_uv, _mask) \
	{ \
		.name = "ldo" #_num, \
		.of_match = "ldo" #_num, \
		.regulators_node = "regulators", \
		.id = (_num) - 1, \
		.ops = &wl2868c_ops, \
		.type = REGULATOR_VOLTAGE, \
		.owner = THIS_MODULE, \
		.supply_name = _supply, \
		.min_uV = _min_uv, \
		.uV_step = 8000, \
		.n_voltages = (_mask) + 1, \
		.vsel_reg = WL2868C_LDO1_VOUT + (_num) - 1, \
		.vsel_mask = _mask, \
		.enable_reg = WL2868C_LDO_ENABLE, \
		.enable_mask = BIT((_num) - 1), \
		.enable_time = 150, \
	}

static const struct regulator_desc wl2868c_regulators[] = {
	WL2868C_LDO(1, "vin12", 496000, GENMASK(6, 0)),
	WL2868C_LDO(2, "vin12", 496000, GENMASK(6, 0)),
	WL2868C_LDO(3, "vin34", 1504000, GENMASK(7, 0)),
	WL2868C_LDO(4, "vin34", 1504000, GENMASK(7, 0)),
	WL2868C_LDO(5, "vin5", 1504000, GENMASK(7, 0)),
	WL2868C_LDO(6, "vin6", 1504000, GENMASK(7, 0)),
	WL2868C_LDO(7, "vin7", 1504000, GENMASK(7, 0)),
};

static const struct regmap_config wl2868c_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = WL2868C_MAX_REGISTER,
};

static void wl2868c_assert_reset(void *data)
{
	gpiod_set_value_cansleep(data, 1);
}

static int wl2868c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regulator_config config = { .dev = dev };
	struct regulator_dev *rdev;
	struct gpio_desc *reset;
	struct regmap *regmap;
	unsigned int id;
	int ret, i;

	/* RST_N must remain high while any output is in use. */
	reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset), "failed to get reset GPIO\n");

	if (reset) {
		ret = devm_add_action_or_reset(dev, wl2868c_assert_reset, reset);
		if (ret)
			return ret;

		msleep(25);
	}

	regmap = devm_regmap_init_i2c(client, &wl2868c_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap), "failed to create regmap\n");

	ret = regmap_read(regmap, WL2868C_DEVICE_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read device ID\n");
	if (id != WL2868C_DEVICE_ID_VALUE)
		return dev_err_probe(dev, -ENODEV, "unexpected device ID 0x%02x\n", id);

	/* Keep SYS_EN set when changing the individual LDO enable bits. */
	ret = regmap_update_bits(regmap, WL2868C_LDO_ENABLE,
				 WL2868C_SYS_ENABLE, WL2868C_SYS_ENABLE);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable PMIC\n");

	config.regmap = regmap;
	for (i = 0; i < ARRAY_SIZE(wl2868c_regulators); i++) {
		rdev = devm_regulator_register(dev, &wl2868c_regulators[i], &config);
		if (IS_ERR(rdev))
			return dev_err_probe(dev, PTR_ERR(rdev), "failed to register LDO%d\n",
					     i + 1);
	}

	return 0;
}

static const struct of_device_id wl2868c_of_match[] = {
	{ .compatible = "willsemi,wl2868c" },
	{ }
};
MODULE_DEVICE_TABLE(of, wl2868c_of_match);

static struct i2c_driver wl2868c_driver = {
	.driver = {
		.name = "wl2868c-regulator",
		.of_match_table = wl2868c_of_match,
	},
	.probe = wl2868c_probe,
};
module_i2c_driver(wl2868c_driver);

MODULE_DESCRIPTION("Will Semiconductor WL2868C regulator driver");
MODULE_LICENSE("GPL");
