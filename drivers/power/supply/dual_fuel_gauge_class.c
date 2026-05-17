// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xiaomi dual BQ27z561 fuel-gauge aggregator.
 *
 * The pipa battery pack is monitored by two BQ27z561 gauges.  Expose a
 * single battery power supply to charging policy while keeping the two gauges
 * on the standard bq27xxx driver.
 */

#include <linux/module.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/slab.h>

struct dual_fg {
	struct device *dev;
	struct power_supply *master;
	struct power_supply *slave;
	struct power_supply *psy;
	struct notifier_block nb;
};

static int dual_fg_get_int(struct power_supply *psy,
			   enum power_supply_property prop, int *val)
{
	union power_supply_propval pval;
	int ret;

	ret = power_supply_get_property(psy, prop, &pval);
	if (ret)
		return ret;

	*val = pval.intval;
	return 0;
}

static int dual_fg_get_both(struct dual_fg *dfg,
			    enum power_supply_property prop,
			    int *master, int *slave)
{
	int ret;

	ret = dual_fg_get_int(dfg->master, prop, master);
	if (ret)
		return ret;

	return dual_fg_get_int(dfg->slave, prop, slave);
}

static int dual_fg_get_sum(struct dual_fg *dfg,
			   enum power_supply_property prop,
			   union power_supply_propval *val)
{
	int master, slave, ret;

	ret = dual_fg_get_both(dfg, prop, &master, &slave);
	if (ret)
		return ret;

	val->intval = master + slave;
	return 0;
}

static int dual_fg_get_present(struct dual_fg *dfg,
			       union power_supply_propval *val)
{
	int master, slave, ret;

	ret = dual_fg_get_both(dfg, POWER_SUPPLY_PROP_PRESENT, &master, &slave);
	if (ret)
		return ret;

	val->intval = master && slave;
	return 0;
}

static int dual_fg_get_voltage(struct dual_fg *dfg,
			       union power_supply_propval *val)
{
	int master, slave, ret;

	ret = dual_fg_get_both(dfg, POWER_SUPPLY_PROP_VOLTAGE_NOW,
			       &master, &slave);
	if (ret)
		return ret;

	/*
	 * The two gauges monitor parallel cell groups in the same pack.  Keep
	 * voltage in single-cell units like the stock dual-gauge class does.
	 */
	val->intval = max(master, slave);
	return 0;
}

static int dual_fg_get_temp(struct dual_fg *dfg,
			    union power_supply_propval *val)
{
	int master, slave, ret;

	ret = dual_fg_get_both(dfg, POWER_SUPPLY_PROP_TEMP, &master, &slave);
	if (ret)
		return ret;

	val->intval = max(master, slave);
	return 0;
}

static int dual_fg_get_status(struct dual_fg *dfg,
			      union power_supply_propval *val)
{
	int master, slave, ret;

	ret = dual_fg_get_both(dfg, POWER_SUPPLY_PROP_STATUS, &master, &slave);
	if (ret)
		return ret;

	if (master == POWER_SUPPLY_STATUS_CHARGING ||
	    slave == POWER_SUPPLY_STATUS_CHARGING)
		val->intval = POWER_SUPPLY_STATUS_CHARGING;
	else if (master == POWER_SUPPLY_STATUS_FULL &&
		 slave == POWER_SUPPLY_STATUS_FULL)
		val->intval = POWER_SUPPLY_STATUS_FULL;
	else if (master == POWER_SUPPLY_STATUS_DISCHARGING ||
		 slave == POWER_SUPPLY_STATUS_DISCHARGING)
		val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
	else if (master == POWER_SUPPLY_STATUS_NOT_CHARGING ||
		 slave == POWER_SUPPLY_STATUS_NOT_CHARGING)
		val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
	else
		val->intval = POWER_SUPPLY_STATUS_UNKNOWN;

	return 0;
}

static int dual_fg_get_capacity(struct dual_fg *dfg,
				union power_supply_propval *val)
{
	int cap_m, cap_s, full_m, full_s, ret;
	u64 weighted;

	ret = dual_fg_get_both(dfg, POWER_SUPPLY_PROP_CAPACITY,
			       &cap_m, &cap_s);
	if (ret)
		return ret;

	ret = dual_fg_get_both(dfg, POWER_SUPPLY_PROP_CHARGE_FULL,
			       &full_m, &full_s);
	if (!ret && full_m > 0 && full_s > 0) {
		weighted = (u64)cap_m * full_m + (u64)cap_s * full_s;
		val->intval = DIV_ROUND_CLOSEST_ULL(weighted, full_m + full_s);
	} else {
		val->intval = DIV_ROUND_CLOSEST(cap_m + cap_s, 2);
	}

	return 0;
}

static int dual_fg_get_capacity_level(struct dual_fg *dfg,
				      union power_supply_propval *val)
{
	int cap, ret;

	ret = dual_fg_get_capacity(dfg, val);
	if (ret)
		return ret;

	cap = val->intval;
	if (cap >= 100)
		val->intval = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
	else if (cap >= 80)
		val->intval = POWER_SUPPLY_CAPACITY_LEVEL_HIGH;
	else if (cap > 15)
		val->intval = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
	else if (cap > 5)
		val->intval = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
	else
		val->intval = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;

	return 0;
}

static int dual_fg_get_health(struct dual_fg *dfg,
			      union power_supply_propval *val)
{
	int master, slave, ret;

	ret = dual_fg_get_both(dfg, POWER_SUPPLY_PROP_HEALTH, &master, &slave);
	if (ret)
		return ret;

	if (master != POWER_SUPPLY_HEALTH_GOOD)
		val->intval = master;
	else
		val->intval = slave;

	return 0;
}

static int dual_fg_get_property(struct power_supply *psy,
				enum power_supply_property prop,
				union power_supply_propval *val)
{
	struct dual_fg *dfg = power_supply_get_drvdata(psy);

	switch (prop) {
	case POWER_SUPPLY_PROP_PRESENT:
		return dual_fg_get_present(dfg, val);
	case POWER_SUPPLY_PROP_STATUS:
		return dual_fg_get_status(dfg, val);
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		return dual_fg_get_voltage(dfg, val);
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		return dual_fg_get_sum(dfg, prop, val);
	case POWER_SUPPLY_PROP_CAPACITY:
		return dual_fg_get_capacity(dfg, val);
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		return dual_fg_get_capacity_level(dfg, val);
	case POWER_SUPPLY_PROP_TEMP:
		return dual_fg_get_temp(dfg, val);
	case POWER_SUPPLY_PROP_CHARGE_NOW:
	case POWER_SUPPLY_PROP_CHARGE_FULL:
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		return dual_fg_get_sum(dfg, prop, val);
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		return dual_fg_get_sum(dfg, prop, val);
	case POWER_SUPPLY_PROP_HEALTH:
		return dual_fg_get_health(dfg, val);
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		return 0;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "dual-bq27z561";
		return 0;
	default:
		return -EINVAL;
	}
}

static enum power_supply_property dual_fg_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

static const struct power_supply_desc dual_fg_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = dual_fg_props,
	.num_properties = ARRAY_SIZE(dual_fg_props),
	.get_property = dual_fg_get_property,
};

static int dual_fg_notifier_call(struct notifier_block *nb,
				 unsigned long event, void *data)
{
	struct dual_fg *dfg = container_of(nb, struct dual_fg, nb);
	struct power_supply *psy = data;

	if (event != PSY_EVENT_PROP_CHANGED)
		return NOTIFY_OK;

	if (psy == dfg->master || psy == dfg->slave)
		power_supply_changed(dfg->psy);

	return NOTIFY_OK;
}

static void dual_fg_unregister_notifier(void *data)
{
	struct dual_fg *dfg = data;

	power_supply_unreg_notifier(&dfg->nb);
}

static int dual_fg_probe(struct platform_device *pdev)
{
	struct power_supply_config psy_cfg = {};
	struct dual_fg *dfg;
	int ret;

	dfg = devm_kzalloc(&pdev->dev, sizeof(*dfg), GFP_KERNEL);
	if (!dfg)
		return -ENOMEM;

	dfg->dev = &pdev->dev;
	platform_set_drvdata(pdev, dfg);

	dfg->master = devm_power_supply_get_by_reference(&pdev->dev,
							 "master-fuel-gauge");
	if (IS_ERR(dfg->master))
		return dev_err_probe(&pdev->dev, PTR_ERR(dfg->master),
				     "failed to get master fuel gauge\n");
	if (!dfg->master)
		return dev_err_probe(&pdev->dev, -EPROBE_DEFER,
				     "master fuel gauge not ready\n");

	dfg->slave = devm_power_supply_get_by_reference(&pdev->dev,
							"slave-fuel-gauge");
	if (IS_ERR(dfg->slave))
		return dev_err_probe(&pdev->dev, PTR_ERR(dfg->slave),
				     "failed to get slave fuel gauge\n");
	if (!dfg->slave)
		return dev_err_probe(&pdev->dev, -EPROBE_DEFER,
				     "slave fuel gauge not ready\n");

	psy_cfg.drv_data = dfg;
	psy_cfg.fwnode = dev_fwnode(&pdev->dev);

	dfg->psy = devm_power_supply_register(&pdev->dev, &dual_fg_desc,
					      &psy_cfg);
	if (IS_ERR(dfg->psy))
		return dev_err_probe(&pdev->dev, PTR_ERR(dfg->psy),
				     "failed to register battery\n");

	dfg->nb.notifier_call = dual_fg_notifier_call;
	ret = power_supply_reg_notifier(&dfg->nb);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register notifier\n");

	return devm_add_action_or_reset(&pdev->dev,
					dual_fg_unregister_notifier, dfg);
}

static const struct of_device_id dual_fg_of_match[] = {
	{ .compatible = "xiaomi,dual-fuelgauge" },
	{ }
};
MODULE_DEVICE_TABLE(of, dual_fg_of_match);

static struct platform_driver dual_fg_driver = {
	.driver = {
		.name = "xiaomi-dual-fuelgauge",
		.of_match_table = dual_fg_of_match,
	},
	.probe = dual_fg_probe,
};
module_platform_driver(dual_fg_driver);

MODULE_DESCRIPTION("Xiaomi dual fuel gauge aggregator");
MODULE_LICENSE("GPL");
