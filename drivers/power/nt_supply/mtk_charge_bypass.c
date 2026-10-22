// SPDX-License-Identifier: GPL-2.0
/* A separate, connection-scoped charge inhibition vote for Tetris Parts. */
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include "mtk_charger.h"

#define BYPASS_CHARGERS 7

struct charge_bypass_status {
	bool online;
	bool online_known;
	bool active;
	bool power_path;
	bool charging;
	int error;
};

static void charge_bypass_devices(struct mtk_charger *info,
		struct charger_device **devices)
{
	devices[0] = info->chg1_dev;
	devices[1] = info->chg2_dev;
	devices[2] = info->dvchg1_dev;
	devices[3] = info->dvchg2_dev;
	devices[4] = info->hvdvchg1_dev;
	devices[5] = info->hvdvchg2_dev;
	devices[6] = info->bkbstchg_dev;
}

static int charge_bypass_online(struct mtk_charger *info, bool *online)
{
	union power_supply_propval val = { 0 };
	int ret;

	*online = false;
	if (IS_ERR_OR_NULL(info->bc12_psy))
		return -ENODEV;
	ret = power_supply_get_property(info->bc12_psy,
			POWER_SUPPLY_PROP_ONLINE, &val);
	if (!ret)
		*online = !!val.intval;
	return ret;
}

/* All callers hold charger_lock. Never infer success from a saved request. */
static void charge_bypass_observe(struct mtk_charger *info,
		struct charge_bypass_status *status)
{
	struct charger_device *devices[BYPASS_CHARGERS];
	bool enabled;
	int i, ret;

	memset(status, 0, sizeof(*status));
	if (!info->charge_bypass_ready || !info->chg1_dev) {
		status->charging = true;
		status->error = -EAGAIN;
		return;
	}
	status->error = info->charge_bypass_error ?: info->charge_bypass_input_error;
	ret = charge_bypass_online(info, &status->online);
	status->online_known = !ret;
	if (ret < 0)
		status->error = ret;
	ret = charger_dev_is_powerpath_active(info->chg1_dev,
			&status->power_path);
	if (ret < 0) {
		status->power_path = false;
		status->error = ret;
	}
	charge_bypass_devices(info, devices);
	for (i = 0; i < ARRAY_SIZE(devices); i++) {
		if (!devices[i])
			continue;
		enabled = true;
		ret = charger_dev_is_charge_enabled(devices[i], &enabled);
		if (ret < 0) {
			status->error = ret;
			enabled = true;
		}
		status->charging |= enabled;
	}
	status->active = info->charge_bypass_requested && status->online &&
		status->power_path && !status->charging && !status->error;
}

static int charge_bypass_inhibit(struct mtk_charger *info, bool inhibit)
{
	struct charger_device *devices[BYPASS_CHARGERS];
	int i, ret, error = 0;

	charge_bypass_devices(info, devices);
	for (i = 0; i < ARRAY_SIZE(devices); i++) {
		if (!devices[i])
			continue;
		ret = charger_dev_set_charge_inhibited(devices[i], inhibit);
		if (ret < 0 && !error)
			error = ret;
	}
	return error;
}

void mtk_charge_bypass_reset_locked(struct mtk_charger *info)
{
	lockdep_assert_held(&info->charger_lock);
	if (info->charge_bypass_requested)
		charge_bypass_inhibit(info, false);
	WRITE_ONCE(info->charge_bypass_requested, false);
	info->charge_bypass_error = 0;
	info->charge_bypass_input_error = 0;
	WRITE_ONCE(info->charge_bypass_input_blocked, false);
}

int mtk_charge_bypass_update_input_locked(struct mtk_charger *info)
{
	struct charge_bypass_status status;
	bool online;
	int ret, power_path_ret;

	lockdep_assert_held(&info->charger_lock);
	if (!info->charge_bypass_requested)
		return 0;
	ret = charge_bypass_online(info, &online);
	if (!ret && !online)
		ret = -ENODEV;
	if (!ret && is_disable_charger(info))
		ret = -EBUSY;
	if (!ret && info->charge_bypass_error)
		ret = info->charge_bypass_error;
	if (!ret) {
		/* A successful disable write alone does not prove all pumps are off. */
		charge_bypass_observe(info, &status);
		if (status.charging)
			ret = status.error ?: -EIO;
	}
	if (!ret)
		ret = info->algo.update_power_path_current ?
			info->algo.update_power_path_current(info) : -EOPNOTSUPP;
	/* An unverified budget must not leave an older, higher input limit active. */
	if (ret)
		WRITE_ONCE(info->charge_bypass_input_blocked, true);
	power_path_ret = mtk_charger_refresh_power_path(info);
	if (!ret)
		ret = power_path_ret;
	if (power_path_ret)
		WRITE_ONCE(info->charge_bypass_input_blocked, true);
	info->charge_bypass_input_error = ret;
	return ret;
}

static int charge_bypass_set_locked(struct mtk_charger *info, bool requested)
{
	struct charge_bypass_status status;
	bool online;
	int i, ret, error;

	lockdep_assert_held(&info->charger_lock);
	if (!requested) {
		mtk_charge_bypass_reset_locked(info);
		return 0;
	}
	if (!info->charge_bypass_ready || !info->chg1_dev)
		return -EAGAIN;
	ret = charge_bypass_online(info, &online);
	if (ret < 0)
		return ret;
	if (!online)
		return -ENODEV;
	/* Wait for the manager's first plug-in transition before taking a vote. */
	if (info->chr_type == POWER_SUPPLY_TYPE_UNKNOWN)
		return -EAGAIN;

	WRITE_ONCE(info->charge_bypass_requested, true);
	info->can_charging = false;
	error = charge_bypass_inhibit(info, true);
	/* No class enable_lock is held while algorithms finish their workers. */
	for (i = 0; i < MAX_ALG_NO; i++) {
		if (!info->alg[i])
			continue;
		ret = chg_alg_stop_algo(info->alg[i]);
		if (ret < 0 && !error)
			error = ret;
	}
	/* Also verify disable after any algorithm's shutdown hardware sequence. */
	ret = charge_bypass_inhibit(info, true);
	if (ret < 0 && !error)
		error = ret;
	info->charge_bypass_error = error;
	mtk_charge_bypass_update_input_locked(info);
	charge_bypass_observe(info, &status);
	if (!error && status.charging)
		error = status.error ?: -EIO;
	if (status.online_known && !status.online) {
		/* The cable may disappear during a sleeping algorithm stop. */
		mtk_charge_bypass_reset_locked(info);
		return -ENODEV;
	}
	info->charge_bypass_error = error;
	return error ?: status.error;
}

static ssize_t charge_bypass_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mtk_charger *info = dev_get_drvdata(dev);
	bool requested;

	mutex_lock(&info->charger_lock);
	requested = info->charge_bypass_requested;
	mutex_unlock(&info->charger_lock);
	return sysfs_emit(buf, "%u\n", requested);
}

static ssize_t charge_bypass_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mtk_charger *info = dev_get_drvdata(dev);
	bool requested;
	int ret;

	if (sysfs_streq(buf, "1"))
		requested = true;
	else if (sysfs_streq(buf, "0"))
		requested = false;
	else
		return -EINVAL;
	mutex_lock(&info->charger_lock);
	ret = charge_bypass_set_locked(info, requested);
	mutex_unlock(&info->charger_lock);
	_wake_up_charger(info);
	return ret < 0 ? ret : count;
}
static DEVICE_ATTR_RW(charge_bypass);

static ssize_t charge_bypass_status_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mtk_charger *info = dev_get_drvdata(dev);
	struct charge_bypass_status status;
	ssize_t count;

	mutex_lock(&info->charger_lock);
	charge_bypass_observe(info, &status);
	count = sysfs_emit(buf,
		"version=1 requested=%u online=%u active=%u power_path=%u charging=%u error=%d\n",
		info->charge_bypass_requested, status.online, status.active,
		status.power_path, status.charging, status.error);
	mutex_unlock(&info->charger_lock);
	return count;
}
static DEVICE_ATTR_RO(charge_bypass_status);

static struct attribute *charge_bypass_attrs[] = {
	&dev_attr_charge_bypass.attr,
	&dev_attr_charge_bypass_status.attr,
	NULL,
};
ATTRIBUTE_GROUPS(charge_bypass);

int mtk_charge_bypass_init(struct mtk_charger *info)
{
	return devm_device_add_groups(&info->pdev->dev, charge_bypass_groups);
}
