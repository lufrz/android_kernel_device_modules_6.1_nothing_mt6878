// SPDX-License-Identifier: GPL-2.0
/* Deterministic hardware fixture for the production input-only policy. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

typedef uint32_t u32;
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define chr_err(...) ((void)0)
#define lockdep_assert_held(m) assert(*(m))
#define CHG1_SETTING 0
#define CHG2_SETTING 1
#define TYPEC_RP_LEVEL 0
#define DUAL_CHARGERS_IN_SERIES 1
enum { POWER_SUPPLY_TYPE_UNKNOWN, POWER_SUPPLY_TYPE_USB,
       POWER_SUPPLY_TYPE_USB_CDP, POWER_SUPPLY_TYPE_USB_DCP };
enum { POWER_SUPPLY_USB_TYPE_SDP, POWER_SUPPLY_USB_TYPE_DCP };
enum { MTK_PD_CONNECT_NONE, MTK_PD_CONNECT_TYPEC_ONLY_SNK,
       MTK_PD_CONNECT_PE_READY_SNK, MTK_PD_CONNECT_PE_READY_SNK_PD30,
       MTK_PD_CONNECT_PE_READY_SNK_APDO };

struct charger_data {
    int input_current_limit, charging_current_limit;
    int thermal_input_current_limit, input_current_limit_by_aicl;
};
struct charger_device {
    u32 input, minimum;
    int read_error, write_error, minimum_error, power_error, writes;
    bool power_path, ignore_write;
};
struct nt_chg_info { int pre_aicl_limmit; };
struct mtk_charger {
    struct charger_data chg_data[2];
    struct charger_device *chg1_dev;
    void *pd_adapter;
    bool charger_lock, charge_bypass_requested, charge_bypass_input_blocked;
    bool usb_unlimited, water_detected, aging_mode, usb_disable_pp, pd_disable_pp;
    bool atm_enabled, cmd_en_pp, disable_aicl, charger_unlimited, enable_dynamic_mivr;
    bool force_disable_pp[2];
    int bootmode, enable_meta_current_limit, chr_type, usb_type, pd_type, config, sink_ua;
    struct { int input_current_limit1; } setting;
    struct {
        int ac_charger_input_current, ac_charger_current, usb_charger_current;
        int charging_host_charger_current, non_std_charger_input_current;
        int non_std_charger_current, max_dmivr_charger_current;
    } data;
};
static int vbus, pd_connection, rp;
static struct nt_chg_info nt;
static int get_vbus(struct mtk_charger *info) { (void)info; return vbus; }
static int adapter_dev_get_property(void *adapter, int property) {
    (void)adapter; (void)property; return rp;
}
static int get_pd_usb_connected(void) { return pd_connection; }
static struct nt_chg_info *get_nt_chg_entry(void) { return &nt; }
static int charger_dev_is_powerpath_active(struct charger_device *d, bool *active) {
    *active = d->power_path; return d->power_error;
}
static int charger_dev_get_min_input_current(struct charger_device *d, u32 *minimum) {
    *minimum = d->minimum; return d->minimum_error;
}
static int charger_dev_get_input_current(struct charger_device *d, u32 *current) {
    *current = d->input; return d->read_error;
}
static int charger_dev_set_input_current(struct charger_device *d, u32 current) {
    d->writes++;
    if (!d->write_error && !d->ignore_write) d->input = current / 25000 * 25000;
    return d->write_error;
}

/* PRODUCTION_METHODS */

static struct mtk_charger info;
static struct charger_device chip;
static void fixture(void) {
    memset(&info, 0, sizeof(info)); memset(&chip, 0, sizeof(chip));
    info.chg1_dev = &chip; info.charger_lock = true;
    info.charge_bypass_requested = true; info.cmd_en_pp = true;
    info.chr_type = POWER_SUPPLY_TYPE_USB_DCP;
    info.usb_type = POWER_SUPPLY_USB_TYPE_DCP;
    info.data.ac_charger_input_current = 2000000;
    info.data.ac_charger_current = 3000000;
    info.data.usb_charger_current = 500000;
    info.data.charging_host_charger_current = 1500000;
    info.data.non_std_charger_input_current = 500000;
    info.data.non_std_charger_current = 500000;
    info.data.max_dmivr_charger_current = 1800000;
    for (int i = 0; i < 2; ++i) {
        info.chg_data[i].thermal_input_current_limit = -1;
        info.chg_data[i].input_current_limit_by_aicl = -1;
    }
    chip.input = chip.minimum = 100000; chip.power_path = true;
    vbus = 5000; pd_connection = 0; rp = 500; nt.pre_aicl_limmit = -1;
}

int main(void) {
    fixture();
    struct charger_data before[2]; memcpy(before, info.chg_data, sizeof(before));
    assert(!update_power_path_current(&info) && chip.input == 2000000);
    assert(!memcmp(before, info.chg_data, sizeof(before)));
    assert(!update_power_path_current(&info) && chip.writes == 1);

    fixture(); info.chr_type = POWER_SUPPLY_TYPE_USB;
    info.usb_type = POWER_SUPPLY_USB_TYPE_SDP;
    assert(!update_power_path_current(&info) && chip.input == 500000);
    fixture(); info.chr_type = POWER_SUPPLY_TYPE_USB_CDP;
    assert(!update_power_path_current(&info) && chip.input == 1500000);

    fixture(); info.pd_type = MTK_PD_CONNECT_PE_READY_SNK_PD30; pd_connection = 1;
    assert(update_power_path_current(&info) == -EAGAIN && !chip.writes);
    info.sink_ua = 900000;
    assert(!update_power_path_current(&info) && chip.input == 900000);
    fixture(); pd_connection = 2;
    assert(update_power_path_current(&info) == -EBUSY && !chip.writes);
    fixture(); info.pd_type = MTK_PD_CONNECT_TYPEC_ONLY_SNK; rp = 1500; vbus = 9000;
    assert(!update_power_path_current(&info) && chip.input == 1500000);

    fixture(); info.chg_data[0].thermal_input_current_limit = 1200000;
    info.chg_data[1].thermal_input_current_limit = 300000;
    assert(!update_power_path_current(&info) && chip.input == 300000);
    fixture(); info.chg_data[0].input_current_limit_by_aicl = 800000;
    nt.pre_aicl_limmit = 650000;
    assert(!update_power_path_current(&info) && chip.input == 650000);
    fixture(); info.enable_dynamic_mivr = true;
    assert(!update_power_path_current(&info) && chip.input == 1800000);
    fixture(); info.water_detected = true;
    assert(!update_power_path_current(&info) && chip.input == 500000);
    fixture(); info.chr_type = POWER_SUPPLY_TYPE_USB; info.atm_enabled = true;
    assert(!update_power_path_current(&info) && chip.input == 100000);
    fixture(); info.bootmode = 1; info.enable_meta_current_limit = 1;
    assert(!update_power_path_current(&info) && chip.input == 200000);
    fixture(); info.usb_unlimited = true;
    assert(update_power_path_current(&info) == -EBUSY && !chip.writes);

    fixture(); chip.input = 2000000;
    info.chg_data[0].thermal_input_current_limit = 0;
    assert(update_power_path_current(&info) == -ERANGE && !chip.writes);
    assert(info.charge_bypass_input_blocked);
    chip.power_path = false; info.chg_data[0].thermal_input_current_limit = 650001;
    assert(!update_power_path_current(&info) && chip.input == 650000);
    assert(!info.charge_bypass_input_blocked && !chip.power_path);
    /* Clearing our vote cannot itself enable the input path. */
    fixture(); info.cmd_en_pp = false;
    assert(update_power_path_current(&info) == -EBUSY && !chip.writes);
    fixture(); info.force_disable_pp[0] = true;
    assert(update_power_path_current(&info) == -EBUSY && !chip.writes);
    fixture(); chip.power_path = false;
    assert(update_power_path_current(&info) == -EBUSY && !chip.writes);

    fixture(); chip.read_error = -EIO;
    assert(update_power_path_current(&info) == -EIO && !chip.writes);
    fixture(); chip.write_error = -EIO;
    assert(update_power_path_current(&info) == -EIO);
    fixture(); chip.ignore_write = true;
    assert(update_power_path_current(&info) == -EIO && chip.input == 100000);
    puts("input-only policy: source, thermal, AICL, ownership and hardware errors passed");
    return 0;
}
