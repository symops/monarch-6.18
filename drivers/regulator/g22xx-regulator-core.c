#include <linux/delay.h>
#include <linux/module.h>
// SPDX-License-Identifier: GPL-2.0-only
/*
 * GMT-G22XX serise PMIC regulator core functions
 *
 * Copyright (C) 2018-2019 Realtek Semiconductor Corporation
 * Author: Cheng-Yu Lee <cylee12@realtek.com>
 */

#include <linux/bitops.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/suspend.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include "g22xx-regulator.h"

/* of_parse_cb */
int g22xx_regulator_of_parse_cb(struct device_node *np,
	const struct regulator_desc *desc, struct regulator_config *config)
{
	struct g22xx_regulator_data *data = config->driver_data;
	struct device_node *child;
	unsigned int val;

	child = of_get_child_by_name(np, "regulator-state-coldboot");
	if (child) {
		struct regulator_state *state = &data->state_coldboot;

		if (of_property_read_bool(child, "regulator-on-in-suspend"))
			state->enabled = ENABLE_IN_SUSPEND;

		if (of_property_read_bool(child, "regulator-off-in-suspend"))
			state->enabled = DISABLE_IN_SUSPEND;

		if (!of_property_read_u32(child, "regulator-suspend-microvolt",
			&val))
			state->uV = val;
	} else
		child = of_get_child_by_name(np, "regulator-state-mem");
	if (child) {
		struct regulator_state *state = &data->state_mem;

		if (of_property_read_bool(child, "regulator-on-in-suspend"))
			state->enabled = ENABLE_IN_SUSPEND;

		if (of_property_read_bool(child, "regulator-off-in-suspend"))
			state->enabled = DISABLE_IN_SUSPEND;

		if (!of_property_read_u32(child, "regulator-suspend-microvolt",
			&val))
			state->uV = val;
	}

	if (desc->n_voltages == 1 && desc->fixed_uV == 0) {
		u32 min = 0, max = 0;

		of_property_read_u32(np, "regulator-min-microvolt", &min);
		of_property_read_u32(np, "regulator-max-microvolt", &max);
		WARN_ON(min != max);
		data->fixed_uV = max;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(g22xx_regulator_of_parse_cb);

unsigned int g22xx_regulator_dc_of_map_mode(unsigned int mode)
{
	switch (mode) {
	case G22XX_DC_MODE_FORCE_PWM:
		return REGULATOR_MODE_FAST;
	default:
		break;
	}
	return REGULATOR_MODE_NORMAL;
}
EXPORT_SYMBOL_GPL(g22xx_regulator_dc_of_map_mode);

unsigned int g22xx_regulator_ldo_of_map_mode(unsigned int mode)
{
	switch (mode) {
	case G22XX_LDO_MODE_ECO:
		return REGULATOR_MODE_IDLE;
	default:
		break;
	}
	return REGULATOR_MODE_NORMAL;
}
EXPORT_SYMBOL_GPL(g22xx_regulator_ldo_of_map_mode);

/* regulator_ops */
static int g22xx_regulator_set_mode_regmap(struct regulator_dev *rdev,
	unsigned int mode)
{
	struct g22xx_regulator_data *data = rdev_get_drvdata(rdev);
	struct g22xx_regulator_desc *gd = data->gd;
	unsigned int val = 0;

	dev_dbg(rdev_get_dev(rdev), "%s\n", __func__);
	if (!data->nmode)
		return -EINVAL;

	if (g22xx_regulator_type_is_ldo(gd))
		val = (mode & REGULATOR_MODE_IDLE) ? 2 : 0;
	else
		val =  (mode & REGULATOR_MODE_FAST) ? 2 : 0;

	dev_dbg(rdev_get_dev(rdev), "%s: val=%02x\n", __func__, val);
	return regmap_field_write(data->nmode, val);
}

static unsigned int g22xx_regulator_get_mode_regmap(struct regulator_dev *rdev)
{
	struct g22xx_regulator_data *data = rdev_get_drvdata(rdev);
	struct g22xx_regulator_desc *gd = data->gd;
	unsigned int val;
	int ret;

	dev_dbg(rdev_get_dev(rdev), "%s\n", __func__);
	if (!data->nmode)
		return -EINVAL;

	ret = regmap_field_read(data->nmode, &val);
	if (ret)
		return 0;

	dev_dbg(rdev_get_dev(rdev), "%s: val=%02x\n", __func__, val);

	if (g22xx_regulator_type_is_ldo(gd) && val == 2)
		return REGULATOR_MODE_IDLE;
	else if (val == 2)
		return REGULATOR_MODE_FAST;
	return REGULATOR_MODE_NORMAL;
}


static int g22xx_set_suspend_enable(struct regulator_dev *rdev)
{
	struct g22xx_regulator_data *data = rdev_get_drvdata(rdev);

	dev_dbg(rdev_get_dev(rdev), "%s\n", __func__);
	regmap_field_write(data->smode, 0x1);
	return 0;
}

static int g22xx_set_suspend_disable(struct regulator_dev *rdev)
{
	struct g22xx_regulator_data *data = rdev_get_drvdata(rdev);

	dev_dbg(rdev_get_dev(rdev), "%s\n", __func__);
	if (!data->smode)
		return -EINVAL;
	regmap_field_write(data->smode, 0x3);
	return 0;
}

static int g22xx_set_suspend_voltage(struct regulator_dev *rdev,
	int uV)
{
	struct g22xx_regulator_data *data = rdev_get_drvdata(rdev);
	int vsel;

	dev_dbg(rdev_get_dev(rdev), "%s\n", __func__);
	vsel = regulator_map_voltage_iterate(rdev, uV, uV);
	if (vsel < 0)
		return -EINVAL;

	if (!data->smode || !data->svsel)
		return -EINVAL;

	regmap_field_write(data->smode, 0x2);
	regmap_field_write(data->svsel, vsel);
	return 0;
}

static int g22xx_regulator_get_voltage_fixed(struct regulator_dev *rdev)
{
	struct g22xx_regulator_data *data = rdev_get_drvdata(rdev);

	if (data->fixed_uV)
		return data->fixed_uV;
	return -EINVAL;
}

const struct regulator_ops g22xx_regulator_ops = {
	.list_voltage         = regulator_list_voltage_table,
	.map_voltage          = regulator_map_voltage_iterate,
	.set_voltage_sel      = regulator_set_voltage_sel_regmap,
	.get_voltage_sel      = regulator_get_voltage_sel_regmap,
	.enable               = regulator_enable_regmap,
	.disable              = regulator_disable_regmap,
	.is_enabled           = regulator_is_enabled_regmap,
	.get_mode             = g22xx_regulator_get_mode_regmap,
	.set_mode             = g22xx_regulator_set_mode_regmap,
	.set_suspend_voltage  = g22xx_set_suspend_voltage,
	.set_suspend_enable   = g22xx_set_suspend_enable,
	.set_suspend_disable  = g22xx_set_suspend_disable,
};
EXPORT_SYMBOL_GPL(g22xx_regulator_ops);

const struct regulator_ops g22xx_regulator_fixed_uV_ops = {
	.enable               = regulator_enable_regmap,
	.disable              = regulator_disable_regmap,
	.is_enabled           = regulator_is_enabled_regmap,
	.get_mode             = g22xx_regulator_get_mode_regmap,
	.set_mode             = g22xx_regulator_set_mode_regmap,
	.set_suspend_voltage  = g22xx_set_suspend_voltage,
	.set_suspend_enable   = g22xx_set_suspend_enable,
	.set_suspend_disable  = g22xx_set_suspend_disable,
	.get_voltage          = g22xx_regulator_get_voltage_fixed,
};
EXPORT_SYMBOL_GPL(g22xx_regulator_fixed_uV_ops);

/*
 * The sleep-mode fields are what actually switch a rail off when the audio
 * CPU puts the PMIC to sleep at power-off. An I2C transfer that fails (or is
 * not applied) leaves that rail powered, and the board then keeps drawing
 * power after "shutdown -P" -- seen intermittently on real hardware -- so
 * write, read back, retry, and say so if it never sticks.
 */
static int g22xx_sleep_field_write(struct device *dev, struct regmap_field *field,
				   unsigned int val, const char *what)
{
	unsigned int rb = 0;
	int ret = -EIO, i;

	for (i = 0; i < 3; i++) {
		ret = regmap_field_write(field, val);
		if (!ret)
			ret = regmap_field_read(field, &rb);
		if (!ret && rb == val)
			return 0;
		if (!ret)
			ret = -EIO;
		msleep(5);
	}
	dev_warn(dev, "cannot set %s to %u after 3 tries (last error %d, read back %u)\n",
		 what, val, ret, rb);
	return ret;
}

int g22xx_regulator_set_state(struct regulator_dev *rdev, int is_coldboot)
{
	struct g22xx_regulator_data *data = rdev_get_drvdata(rdev);
	struct device *dev = rdev_get_dev(rdev);
	struct regulator_state *rstate;

	rstate = &rdev->constraints->state_mem;
	*rstate = is_coldboot ? data->state_coldboot : data->state_mem;

	if (!data->smode)
		return -EINVAL;

	if (rstate->enabled == ENABLE_IN_SUSPEND && rstate->uV) {
		int vsel;

		vsel = regulator_map_voltage_iterate(rdev, rstate->uV,
						     rstate->uV);
		if (vsel < 0)
			return -EINVAL;

		if (!data->svsel)
			return -EINVAL;

		dev_dbg(dev, "set sleep_mode to volt-%d\n",
			rstate->uV);
		if (g22xx_sleep_field_write(dev, data->smode, 0x2, "sleep mode") ||
		    g22xx_sleep_field_write(dev, data->svsel, vsel, "sleep voltage select"))
			return -EIO;
		return 0;
	}

	if (rstate->enabled == DISABLE_IN_SUSPEND) {
		dev_dbg(dev, "set sleep_mode to disabled\n");
		return g22xx_sleep_field_write(dev, data->smode, 0x3, "sleep mode (off)");
	}

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(g22xx_regulator_set_state);

static struct regmap_field *create_regmap_field(
		struct g22xx_regulator_device *grdev,
		u32 reg,
		u32 mask)
{
	u32 msb = fls(mask) - 1;
	u32 lsb = ffs(mask) - 1;
	struct reg_field map = REG_FIELD(reg, lsb, msb);
	struct regmap_field *rmap;

	if (reg == 0 && mask == 0)
		return NULL;

	dev_dbg(grdev->dev, "reg=%02x, mask=%02x, lsb=%d, msb=%d\n",
		reg, mask, lsb, msb);
	rmap = devm_regmap_field_alloc(grdev->dev, grdev->regmap, map);
	if (IS_ERR(rmap)) {
		dev_err(grdev->dev, "regmap_field_alloc() for (reg=%02x, mask=%02x) returns %ld\n",
			reg, mask, PTR_ERR(rmap));
	}
	return rmap;
}

struct regulator_dev *g22xx_regulator_register(
		struct g22xx_regulator_device *grdev,
		struct g22xx_regulator_desc *gd)
{
	struct device *dev = grdev->dev;
	struct g22xx_regulator_data *data;
	struct regulator_config config = {};
	struct regulation_constraints *c;


	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return ERR_PTR(-ENOMEM);

	data->gd = gd;

	data->nmode = create_regmap_field(grdev, gd->nmode_reg, gd->nmode_mask);
	if (IS_ERR(data->nmode))
		return ERR_CAST(data->nmode);
	data->smode = create_regmap_field(grdev, gd->smode_reg, gd->smode_mask);
	if (IS_ERR(data->smode))
		return ERR_CAST(data->smode);
	data->svsel = create_regmap_field(grdev, gd->svsel_reg, gd->svsel_mask);
	if (IS_ERR(data->svsel))
		return ERR_CAST(data->svsel);

	config.dev         = grdev->dev;
	config.regmap      = grdev->regmap;
	config.driver_data = data;

	data->rdev = devm_regulator_register(dev, &gd->desc, &config);
	if (IS_ERR(data->rdev))
		return data->rdev;

	c = data->rdev->constraints;

	/*
	 * copy state_mem to data for mem/coldboot switching. if there is no
	 * sleep uV and mode, set the state to enable
	 */
	data->state_mem = c->state_mem;
	if (data->state_mem.uV == 0 &&
	    data->state_mem.enabled == DO_NOTHING_IN_SUSPEND) {
		data->state_mem.enabled = ENABLE_IN_SUSPEND;
		c->state_mem = data->state_mem;
	}
	if (data->state_coldboot.uV == 0 &&
	    data->state_coldboot.enabled == DO_NOTHING_IN_SUSPEND)
		data->state_coldboot.enabled = ENABLE_IN_SUSPEND;

	/* enable change mode */
	c->valid_modes_mask |= REGULATOR_MODE_NORMAL;
	if (g22xx_regulator_type_is_ldo(data->gd))
		c->valid_modes_mask |= REGULATOR_MODE_IDLE;
	else
		c->valid_modes_mask |= REGULATOR_MODE_FAST;
	c->valid_ops_mask |= REGULATOR_CHANGE_MODE;

	list_add_tail(&data->list, &grdev->list);

	return data->rdev;
}
EXPORT_SYMBOL_GPL(g22xx_regulator_register);



MODULE_DESCRIPTION("GMT G22xx regulator helpers");
MODULE_LICENSE("GPL");
