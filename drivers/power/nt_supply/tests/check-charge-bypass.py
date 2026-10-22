#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Run the actual charge-bypass manager against deterministic hardware failures."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "mtk_charge_bypass.c").read_text()
source = "\n".join(line for line in source.splitlines() if not line.startswith("#include"))

stubs = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>
#include <errno.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define IS_ERR_OR_NULL(p) (!(p))
#define WRITE_ONCE(x, v) ((x) = (v))
#define MAX_ALG_NO 3
#define POWER_SUPPLY_PROP_ONLINE 0
#define POWER_SUPPLY_TYPE_UNKNOWN 0
struct mutex { bool held; };
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held = true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held = false; }
#define lockdep_assert_held(m) assert((m)->held)
struct device { void *data; };
struct platform_device { struct device dev; };
struct attribute { int unused; };
struct device_attribute { struct attribute attr; };
#define DEVICE_ATTR_RW(n) struct device_attribute dev_attr_##n
#define DEVICE_ATTR_RO(n) struct device_attribute dev_attr_##n
#define ATTRIBUTE_GROUPS(n) static const void *n##_groups = n##_attrs
static void *dev_get_drvdata(struct device *d) { return d->data; }
static int devm_device_add_groups(struct device *d, const void *g) { return 0; }
static int sysfs_emit(char *buf, const char *format, ...) {
    va_list ap; va_start(ap, format); int n = vsnprintf(buf, 4096, format, ap);
    va_end(ap); return n;
}
static bool sysfs_streq(const char *a, const char *b) {
    size_t n = strlen(a); if (n && a[n - 1] == '\n') n--;
    return n == strlen(b) && !strncmp(a, b, n);
}
struct power_supply { bool online; int error; };
union power_supply_propval { int intval; };
static int power_supply_get_property(struct power_supply *p, int prop,
        union power_supply_propval *v) { v->intval = p->online; return p->error; }
struct charger_device {
    bool inhibited, enabled, power_path;
    int disable_error, read_error, power_error, disable_writes, enable_writes;
};
struct chg_alg_device { int stop_error, stops; bool disconnect; };
struct mtk_charger {
    struct platform_device *pdev;
    struct charger_device *chg1_dev, *chg2_dev, *dvchg1_dev, *dvchg2_dev;
    struct charger_device *hvdvchg1_dev, *hvdvchg2_dev, *bkbstchg_dev;
    struct power_supply *bc12_psy;
    struct chg_alg_device *alg[MAX_ALG_NO];
    struct mutex charger_lock;
    bool charge_bypass_requested, charge_bypass_ready, can_charging;
    bool charge_bypass_input_blocked;
    int charge_bypass_error, charge_bypass_input_error, chr_type, wakes;
    int policy_error, policy_calls;
    bool manager_disabled;
    struct { int (*update_power_path_current)(struct mtk_charger *); } algo;
    /* Existing owners must remain completely untouched. */
    bool cmd_discharging, cmd_en_pp, thermal_stop;
    int input_limit;
};
static struct mtk_charger *current;
static int mtk_charger_refresh_power_path(struct mtk_charger *info) {
    assert(info->charger_lock.held);
    info->chg1_dev->power_path = !info->charge_bypass_input_blocked;
    return 0;
}
static bool is_disable_charger(struct mtk_charger *info) { return info->manager_disabled; }
static int update_input(struct mtk_charger *info) {
    assert(info->charger_lock.held);
    info->policy_calls++;
    if (!info->policy_error) info->charge_bypass_input_blocked = false;
    return info->policy_error;
}
static int charger_dev_set_charge_inhibited(struct charger_device *d, bool v) {
    d->inhibited = v;
    if (!v) return 0;
    d->disable_writes++;
    if (d->disable_error) return d->disable_error;
    d->enabled = false; return 0;
}
static int charger_dev_enable(struct charger_device *d, bool v) {
    if (v && d->inhibited) return -EBUSY;
    if (v) d->enable_writes++;
    d->enabled = v; return 0;
}
static int charger_dev_is_charge_enabled(struct charger_device *d, bool *v) {
    *v = d->enabled; return d->read_error;
}
static int charger_dev_is_powerpath_active(struct charger_device *d, bool *v) {
    *v = d->power_path; return d->power_error;
}
static int chg_alg_stop_algo(struct chg_alg_device *a) {
    a->stops++;
    /* Model the old algorithm's attempted charging restoration during stop. */
    assert(charger_dev_enable(current->chg1_dev, true) == -EBUSY);
    assert(charger_dev_enable(current->dvchg1_dev, true) == -EBUSY);
    if (a->disconnect) current->bc12_psy->online = false;
    return a->stop_error;
}
static void _wake_up_charger(struct mtk_charger *info) { info->wakes++; }
'''

tests = r'''
static struct mtk_charger info;
static struct platform_device pdev;
static struct charger_device chips[7];
static struct power_supply supply;
static struct chg_alg_device algorithms[MAX_ALG_NO];
static void fixture(void) {
    memset(&info, 0, sizeof(info)); memset(chips, 0, sizeof(chips));
    memset(algorithms, 0, sizeof(algorithms)); memset(&supply, 0, sizeof(supply));
    pdev.dev.data = &info; info.pdev = &pdev; current = &info;
    info.algo.update_power_path_current = update_input;
    info.chg1_dev = chips; info.chg2_dev = chips + 1;
    info.dvchg1_dev = chips + 2; info.dvchg2_dev = chips + 3;
    info.hvdvchg1_dev = chips + 4; info.hvdvchg2_dev = chips + 5;
    info.bkbstchg_dev = chips + 6; info.bc12_psy = &supply;
    for (int i = 0; i < 7; ++i) chips[i].enabled = true;
    for (int i = 0; i < MAX_ALG_NO; ++i) info.alg[i] = algorithms + i;
    chips[0].power_path = true; info.charge_bypass_ready = true;
    info.chr_type = 1; supply.online = true; info.can_charging = true;
    info.cmd_discharging = true; info.cmd_en_pp = false;
    info.thermal_stop = true; info.input_limit = 325000;
}
static int write_request(const char *request) {
    return charge_bypass_store(&pdev.dev, NULL, request, strlen(request));
}
static struct charge_bypass_status observe(void) {
    struct charge_bypass_status s;
    mutex_lock(&info.charger_lock); charge_bypass_observe(&info, &s);
    mutex_unlock(&info.charger_lock); return s;
}
static void other_owners_unchanged(void) {
    assert(info.cmd_discharging && !info.cmd_en_pp && info.thermal_stop);
    assert(info.input_limit == 325000);
    for (int i = 0; i < 7; ++i) assert(!chips[i].enable_writes);
}
int main(void) {
    struct charge_bypass_status s;
    fixture(); info.charge_bypass_ready = false;
    assert(write_request("1") == -EAGAIN);
    s = observe(); assert(!s.active && s.charging && s.error == -EAGAIN);
    assert(write_request("0") == 1); other_owners_unchanged();

    fixture(); assert(write_request("true") == -EINVAL);
    assert(write_request("01") == -EINVAL); assert(write_request("-1") == -EINVAL);
    supply.online = false; assert(write_request("1") == -ENODEV);
    assert(!info.charge_bypass_requested);
    supply.online = true; supply.error = -EIO;
    assert(write_request("1") == -EIO && !info.charge_bypass_requested);
    supply.error = 0; info.chr_type = 0; assert(write_request("1") == -EAGAIN);

    fixture(); assert(write_request("1\n") == 2);
    s = observe(); assert(s.active && s.online && s.power_path && !s.charging && !s.error);
    for (int i = 0; i < 7; ++i) assert(chips[i].inhibited && !chips[i].enabled);
    for (int i = 0; i < MAX_ALG_NO; ++i) assert(algorithms[i].stops == 1);
    assert(write_request("1") == 1); /* Explicit retry remains supported. */
    other_owners_unchanged();
    char text[4096]; assert(charge_bypass_status_show(&pdev.dev, NULL, text) > 0);
    assert(!strcmp(text, "version=1 requested=1 online=1 active=1 power_path=1 charging=0 error=0\n"));

    chips[0].power_path = false; s = observe();
    assert(!s.active && !s.power_path && !s.error && info.charge_bypass_requested);
    chips[0].power_path = true; chips[3].read_error = -EIO; s = observe();
    assert(!s.active && s.charging && s.error == -EIO);
    chips[3].read_error = 0; chips[0].power_error = -EIO; s = observe();
    assert(!s.active && !s.power_path && s.error == -EIO);
    assert(write_request("0") == 1);
    for (int i = 0; i < 7; ++i) assert(!chips[i].inhibited && !chips[i].enabled);
    other_owners_unchanged();

    fixture(); chips[2].disable_error = -EIO;
    assert(write_request("1") == -EIO && info.charge_bypass_requested);
    s = observe(); assert(!s.active && s.charging && s.error == -EIO);
    for (int i = 0; i < 7; ++i) assert(chips[i].inhibited);
    chips[2].disable_error = 0;
    assert(write_request("1") == 1); assert(observe().active);
    other_owners_unchanged();

    fixture(); algorithms[1].stop_error = -ETIMEDOUT;
    assert(write_request("1") == -ETIMEDOUT);
    s = observe(); assert(!s.active && s.error == -ETIMEDOUT && !s.charging);
    assert(write_request("0") == 1 && !info.charge_bypass_requested);
    other_owners_unchanged();

    fixture(); info.policy_error = -EAGAIN;
    assert(write_request("1") == -EAGAIN && info.charge_bypass_requested);
    assert(!observe().active && !info.charge_bypass_error);
    info.policy_error = 0;
    mutex_lock(&info.charger_lock);
    assert(!mtk_charge_bypass_update_input_locked(&info));
    mutex_unlock(&info.charger_lock);
    assert(observe().active);

    fixture(); algorithms[1].stop_error = -ETIMEDOUT; info.policy_error = -EAGAIN;
    assert(write_request("1") == -ETIMEDOUT);
    info.policy_error = 0;
    mutex_lock(&info.charger_lock);
    assert(mtk_charge_bypass_update_input_locked(&info) == -ETIMEDOUT);
    mutex_unlock(&info.charger_lock);
    assert(!observe().active && observe().error == -ETIMEDOUT);

    fixture(); info.algo.update_power_path_current = NULL;
    assert(write_request("1") == -EOPNOTSUPP && !observe().active);
    fixture(); info.manager_disabled = true;
    assert(write_request("1") == -EBUSY && info.policy_calls == 0);

    fixture(); algorithms[1].disconnect = true;
    assert(write_request("1") == -ENODEV && !info.charge_bypass_requested);
    for (int i = 0; i < 7; ++i) assert(!chips[i].inhibited);
    other_owners_unchanged();

    fixture(); assert(write_request("1") == 1); supply.online = false;
    s = observe(); assert(!s.active && !s.online);
    mutex_lock(&info.charger_lock); mtk_charge_bypass_reset_locked(&info);
    mutex_unlock(&info.charger_lock);
    assert(!info.charge_bypass_requested && !info.charge_bypass_error);
    for (int i = 0; i < 7; ++i) assert(!chips[i].inhibited && !chips[i].enabled);
    other_owners_unchanged();
    puts("charge bypass manager: transition, ownership, error and readback tests passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="charge-bypass-test-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(stubs + source + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Werror",
                    "-Wno-unused-function", "-Wno-unused-variable", str(path / "test.c"),
                    "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)

# Verify integration points which cannot be exercised by the host hardware model.
manager = (root / "mtk_charger.c").read_text()
assert manager.count("mtk_charge_bypass_reset_locked(info);") >= 2
assert "if (info->charge_bypass_requested)\n\t\tcharging = false;" in manager
assert "mtk_charge_bypass.o" in (root / "Makefile").read_text()
for name in ("mt6375-charger.c", "sc8541_charger.c", "upm6720_charger.c"):
    assert ".is_charge_enabled =" in (root / name).read_text(), name
print("charge bypass kernel integration hooks: passed")
