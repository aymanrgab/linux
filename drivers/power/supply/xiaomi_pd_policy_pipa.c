// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xiaomi Pad 6 (pipa) USB PD/PPS charge-pump policy manager.
 *
 * Drives the PM8150B TCPM power supply into PPS mode and enables the
 * BQ2597x 2:1 charge pump only while battery and pump telemetry are sane.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define PIPA_PD_POWER_MAX_UW		25000000
#define PIPA_PD_INPUT_CURRENT_MAX_UA	2500000
#define PIPA_PD_PUMP_CURRENT_MAX_UA	5000000
#define PIPA_PD_PPS_VOLTAGE_MIN_UV	10000000
#define PIPA_PD_PPS_VOLTAGE_MAX_UV	11000000
#define PIPA_PD_VBAT_MIN_UV		3500000
#define PIPA_PD_VBAT_MAX_UV		4480000
#define PIPA_PD_BATT_TEMP_MAX		450
#define PIPA_PD_BATT_TEMP_RESUME	420
#define PIPA_PD_PUMP_TEMP_MAX		800
#define PIPA_PD_PUMP_TEMP_RESUME	700
#define PIPA_PD_INTERNAL_FALLBACK_UA	1000000
#define PIPA_PD_WORK_FAST_MS		2000
#define PIPA_PD_WORK_SLOW_MS		10000

enum pipa_pd_mode {
	PIPA_PD_MODE_INTERNAL,
	PIPA_PD_MODE_PUMP,
};

struct pipa_pdpm {
	struct device *dev;
	struct power_supply *usbpd;
	struct power_supply *pump;
	struct power_supply *battery;
	struct power_supply *main_charger;
	struct delayed_work work;
	struct notifier_block nb;
	enum pipa_pd_mode mode;
	bool thermal_limited;
	bool pump_enabled;
	u32 power_max_uw;
	u32 bus_curr_max_ua;
	u32 bus_volt_min_uv;
	u32 bus_volt_max_uv;
	u32 bat_curr_max_ua;
};

static int pipa_pd_get_int(struct power_supply *psy,
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

static int pipa_pd_set_int(struct power_supply *psy,
			   enum power_supply_property prop, int val)
{
	union power_supply_propval pval = { .intval = val };

	return power_supply_set_property(psy, prop, &pval);
}

static void pipa_pd_disable_pump(struct pipa_pdpm *pm)
{
	if (pm->pump_enabled) {
		pipa_pd_set_int(pm->pump, POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT, 0);
		pm->pump_enabled = false;
	}

	pm->mode = PIPA_PD_MODE_INTERNAL;
}

static void pipa_pd_set_internal_fallback(struct pipa_pdpm *pm)
{
	pipa_pd_disable_pump(pm);

	if (pm->usbpd)
		pipa_pd_set_int(pm->usbpd, POWER_SUPPLY_PROP_ONLINE, 1);

	if (pm->main_charger)
		pipa_pd_set_int(pm->main_charger, POWER_SUPPLY_PROP_CURRENT_MAX,
				PIPA_PD_INTERNAL_FALLBACK_UA);
}

static bool pipa_pd_online_is_pps_capable(struct pipa_pdpm *pm, int online)
{
	int usb_type;

	if (online <= 0)
		return false;

	if (pipa_pd_get_int(pm->usbpd, POWER_SUPPLY_PROP_USB_TYPE, &usb_type))
		return false;

	return usb_type == POWER_SUPPLY_USB_TYPE_PD_PPS;
}

static int pipa_pd_read_sensors(struct pipa_pdpm *pm, int *vbat_uv,
				int *batt_temp, int *pump_temp)
{
	int health, ret;

	ret = pipa_pd_get_int(pm->battery, POWER_SUPPLY_PROP_PRESENT, &health);
	if (ret || !health)
		return -ENODEV;

	ret = pipa_pd_get_int(pm->battery, POWER_SUPPLY_PROP_VOLTAGE_NOW,
			      vbat_uv);
	if (ret || *vbat_uv < PIPA_PD_VBAT_MIN_UV ||
	    *vbat_uv > PIPA_PD_VBAT_MAX_UV)
		return ret ?: -ERANGE;

	ret = pipa_pd_get_int(pm->battery, POWER_SUPPLY_PROP_TEMP, batt_temp);
	if (ret)
		return ret;

	ret = pipa_pd_get_int(pm->pump, POWER_SUPPLY_PROP_TEMP, pump_temp);
	if (ret)
		return ret;

	ret = pipa_pd_get_int(pm->pump, POWER_SUPPLY_PROP_HEALTH, &health);
	if (ret)
		return ret;
	if (health != POWER_SUPPLY_HEALTH_GOOD)
		return -EIO;

	return 0;
}

static bool pipa_pd_thermal_ok(struct pipa_pdpm *pm, int batt_temp,
			       int pump_temp)
{
	if (batt_temp >= PIPA_PD_BATT_TEMP_MAX ||
	    pump_temp >= PIPA_PD_PUMP_TEMP_MAX) {
		pm->thermal_limited = true;
		return false;
	}

	if (pm->thermal_limited) {
		if (batt_temp > PIPA_PD_BATT_TEMP_RESUME ||
		    pump_temp > PIPA_PD_PUMP_TEMP_RESUME)
			return false;
		pm->thermal_limited = false;
	}

	return true;
}

static int pipa_pd_set_pps(struct pipa_pdpm *pm, int voltage_uv,
			   int current_ua)
{
	u64 power_uw;
	int ret;

	voltage_uv = clamp(voltage_uv, (int)pm->bus_volt_min_uv,
			   (int)pm->bus_volt_max_uv);
	current_ua = min3(current_ua, (int)pm->bus_curr_max_ua,
			  PIPA_PD_INPUT_CURRENT_MAX_UA);

	power_uw = div64_u64((u64)voltage_uv * current_ua, 1000000);
	if (power_uw > pm->power_max_uw)
		current_ua = div64_u64((u64)pm->power_max_uw * 1000000,
				       voltage_uv);

	current_ua = min(current_ua, PIPA_PD_INPUT_CURRENT_MAX_UA);
	current_ua -= current_ua % 50000;
	voltage_uv -= voltage_uv % 20000;

	ret = pipa_pd_set_int(pm->usbpd, POWER_SUPPLY_PROP_ONLINE, 2);
	if (ret)
		return ret;

	msleep(100);

	ret = pipa_pd_set_int(pm->usbpd, POWER_SUPPLY_PROP_VOLTAGE_NOW,
			      voltage_uv);
	if (ret)
		return ret;

	ret = pipa_pd_set_int(pm->usbpd, POWER_SUPPLY_PROP_CURRENT_NOW,
			      current_ua);
	return ret;
}

static int pipa_pd_enable_pump(struct pipa_pdpm *pm)
{
	int ret;

	ret = pipa_pd_set_int(pm->pump,
			      POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
			      min_t(u32, pm->bat_curr_max_ua,
				    PIPA_PD_PUMP_CURRENT_MAX_UA));
	if (ret)
		return ret;

	ret = pipa_pd_set_int(pm->pump, POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT,
			      1);
	if (ret)
		return ret;

	pm->pump_enabled = true;
	pm->mode = PIPA_PD_MODE_PUMP;

	return 0;
}

static void pipa_pd_work(struct work_struct *work)
{
	struct pipa_pdpm *pm = container_of(to_delayed_work(work),
					   struct pipa_pdpm, work);
	int online, vbat_uv, batt_temp, pump_temp, voltage_uv;
	u32 delay = PIPA_PD_WORK_FAST_MS;
	int ret;

	ret = pipa_pd_get_int(pm->usbpd, POWER_SUPPLY_PROP_ONLINE, &online);
	if (ret || !online) {
		pipa_pd_set_internal_fallback(pm);
		delay = PIPA_PD_WORK_SLOW_MS;
		goto reschedule;
	}

	if (!pipa_pd_online_is_pps_capable(pm, online)) {
		pipa_pd_set_internal_fallback(pm);
		goto reschedule;
	}

	ret = pipa_pd_read_sensors(pm, &vbat_uv, &batt_temp, &pump_temp);
	if (ret) {
		dev_dbg(pm->dev, "sensor read failed: %d\n", ret);
		pipa_pd_set_internal_fallback(pm);
		goto reschedule;
	}

	if (!pipa_pd_thermal_ok(pm, batt_temp, pump_temp)) {
		dev_dbg(pm->dev, "thermal fallback batt=%d pump=%d\n",
			batt_temp, pump_temp);
		pipa_pd_set_internal_fallback(pm);
		goto reschedule;
	}

	voltage_uv = PIPA_PD_PPS_VOLTAGE_MIN_UV;

	ret = pipa_pd_set_pps(pm, voltage_uv, pm->bus_curr_max_ua);
	if (ret) {
		dev_dbg(pm->dev, "PPS request failed: %d\n", ret);
		pipa_pd_set_internal_fallback(pm);
		goto reschedule;
	}

	if (!pm->pump_enabled) {
		ret = pipa_pd_enable_pump(pm);
		if (ret) {
			dev_dbg(pm->dev, "pump enable failed: %d\n", ret);
			pipa_pd_set_internal_fallback(pm);
		}
	}

reschedule:
	schedule_delayed_work(&pm->work, msecs_to_jiffies(delay));
}

static int pipa_pd_notifier_call(struct notifier_block *nb,
				 unsigned long event, void *data)
{
	struct pipa_pdpm *pm = container_of(nb, struct pipa_pdpm, nb);
	struct power_supply *psy = data;

	if (event != PSY_EVENT_PROP_CHANGED)
		return NOTIFY_OK;

	if (psy == pm->usbpd || psy == pm->battery || psy == pm->pump)
		mod_delayed_work(system_wq, &pm->work, 0);

	return NOTIFY_OK;
}

static ssize_t charge_mode_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct pipa_pdpm *pm = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n",
			  pm->mode == PIPA_PD_MODE_PUMP ? "pump" : "internal");
}
static DEVICE_ATTR_RO(charge_mode);

static void pipa_pdpm_unregister_notifier(void *data)
{
	struct pipa_pdpm *pm = data;

	power_supply_unreg_notifier(&pm->nb);
}

static void pipa_pdpm_remove_charge_mode(void *data)
{
	struct device *dev = data;

	device_remove_file(dev, &dev_attr_charge_mode);
}

static void pipa_pdpm_cancel_work(void *data)
{
	struct pipa_pdpm *pm = data;

	cancel_delayed_work_sync(&pm->work);
	pipa_pd_disable_pump(pm);
}

static int pipa_pdpm_get_supply(struct device *dev, const char *prop,
				struct power_supply **psy)
{
	*psy = devm_power_supply_get_by_reference(dev, prop);
	if (IS_ERR(*psy))
		return dev_err_probe(dev, PTR_ERR(*psy),
				     "failed to get %s\n", prop);
	if (!*psy)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "%s not ready\n", prop);

	return 0;
}

static void pipa_pdpm_read_limits(struct pipa_pdpm *pm)
{
	device_property_read_u32(pm->dev, "mi,pd-power-max-uw",
				 &pm->power_max_uw);
	device_property_read_u32(pm->dev, "mi,pd-bus-curr-max-ua",
				 &pm->bus_curr_max_ua);
	device_property_read_u32(pm->dev, "mi,pd-bus-volt-min-uv",
				 &pm->bus_volt_min_uv);
	device_property_read_u32(pm->dev, "mi,pd-bus-volt-max-uv",
				 &pm->bus_volt_max_uv);
	device_property_read_u32(pm->dev, "mi,pd-bat-curr-max-ua",
				 &pm->bat_curr_max_ua);

	pm->power_max_uw = min_t(u32, pm->power_max_uw ?: PIPA_PD_POWER_MAX_UW,
				 PIPA_PD_POWER_MAX_UW);
	pm->bus_curr_max_ua = min_t(u32,
				    pm->bus_curr_max_ua ?:
				    PIPA_PD_INPUT_CURRENT_MAX_UA,
				    PIPA_PD_INPUT_CURRENT_MAX_UA);
	pm->bus_volt_min_uv = max_t(u32,
				    pm->bus_volt_min_uv ?:
				    PIPA_PD_PPS_VOLTAGE_MIN_UV,
				    PIPA_PD_PPS_VOLTAGE_MIN_UV);
	pm->bus_volt_max_uv = min_t(u32,
				    pm->bus_volt_max_uv ?:
				    PIPA_PD_PPS_VOLTAGE_MAX_UV,
				    PIPA_PD_PPS_VOLTAGE_MAX_UV);
	pm->bat_curr_max_ua = min_t(u32,
				    pm->bat_curr_max_ua ?:
				    PIPA_PD_PUMP_CURRENT_MAX_UA,
				    PIPA_PD_PUMP_CURRENT_MAX_UA);
}

static int pipa_pdpm_probe(struct platform_device *pdev)
{
	struct pipa_pdpm *pm;
	int ret;

	pm = devm_kzalloc(&pdev->dev, sizeof(*pm), GFP_KERNEL);
	if (!pm)
		return -ENOMEM;

	pm->dev = &pdev->dev;
	pm->mode = PIPA_PD_MODE_INTERNAL;
	platform_set_drvdata(pdev, pm);

	pipa_pdpm_read_limits(pm);

	ret = pipa_pdpm_get_supply(&pdev->dev, "usbpd-source", &pm->usbpd);
	if (ret)
		return ret;

	ret = pipa_pdpm_get_supply(&pdev->dev, "charge-pump", &pm->pump);
	if (ret)
		return ret;

	ret = pipa_pdpm_get_supply(&pdev->dev, "fuel-gauge", &pm->battery);
	if (ret)
		return ret;

	pm->main_charger = devm_power_supply_get_by_reference(&pdev->dev,
							      "main-charger");
	if (IS_ERR(pm->main_charger))
		return dev_err_probe(&pdev->dev, PTR_ERR(pm->main_charger),
				     "failed to get main charger\n");

	ret = device_create_file(&pdev->dev, &dev_attr_charge_mode);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(&pdev->dev,
				       pipa_pdpm_remove_charge_mode,
				       &pdev->dev);
	if (ret)
		return ret;

	INIT_DELAYED_WORK(&pm->work, pipa_pd_work);
	ret = devm_add_action_or_reset(&pdev->dev, pipa_pdpm_cancel_work, pm);
	if (ret)
		return ret;

	pm->nb.notifier_call = pipa_pd_notifier_call;
	ret = power_supply_reg_notifier(&pm->nb);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register notifier\n");

	ret = devm_add_action_or_reset(&pdev->dev,
				       pipa_pdpm_unregister_notifier, pm);
	if (ret)
		return ret;

	pipa_pd_disable_pump(pm);
	schedule_delayed_work(&pm->work, 0);

	return 0;
}

static const struct of_device_id pipa_pdpm_of_match[] = {
	{ .compatible = "xiaomi,usbpd-pm-pipa" },
	{ }
};
MODULE_DEVICE_TABLE(of, pipa_pdpm_of_match);

static struct platform_driver pipa_pdpm_driver = {
	.driver = {
		.name = "xiaomi-pipa-pd-policy",
		.of_match_table = pipa_pdpm_of_match,
	},
	.probe = pipa_pdpm_probe,
};
module_platform_driver(pipa_pdpm_driver);

MODULE_DESCRIPTION("Xiaomi Pad 6 PPS charge pump policy manager");
MODULE_LICENSE("GPL");
