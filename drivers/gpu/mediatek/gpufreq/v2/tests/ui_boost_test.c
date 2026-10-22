// SPDX-License-Identifier: GPL-2.0
/* Host kernel-API shims and behavioral tests; production bodies are inserted. */
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define __user
#define DEFINE_MUTEX(name) pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
#define mutex_lock(lock) assert(pthread_mutex_lock(lock) == 0)
#define mutex_unlock(lock) assert(pthread_mutex_unlock(lock) == 0)
#define READ_ONCE(value) __atomic_load_n(&(value), __ATOMIC_RELAXED)
#define WRITE_ONCE(target, value) __atomic_store_n(&(target), value, __ATOMIC_RELAXED)
#define EXPORT_SYMBOL(name)
#define GPUFREQ_LOGE(...) do { } while (0)
#define GPUFREQ_TRACE_START(...) do { } while (0)
#define GPUFREQ_TRACE_END(...) do { } while (0)
#define unlikely(value) (value)
#define min(a, b) ((a) < (b) ? (a) : (b))
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

/* PRODUCTION_TYPES */

/* Only shared-memory fields used by the extracted production methods. */
struct gpufreq_shared_status {
    int opp_num_gpu;
    struct gpufreq_opp_info working_table_gpu[GPUFREQ_MAX_OPP_NUM];
    struct gpuppm_limit_info limit_table[LIMIT_NUM];
};
struct file { int unused; };
static unsigned int g_dual_buck;
static struct gpufreq_shared_status shared;
static struct gpufreq_shared_status *g_shared_status = &shared;
static DEFINE_MUTEX(gpufreq_debug_lock);

/* PRODUCTION_STATE */

static int gpufreq_set_limit_raw(enum gpufreq_target target,
    enum gpuppm_limiter limiter, int ceiling_info, int floor_info);
static int gpufreq_switch_limit(enum gpufreq_target target,
    enum gpuppm_limiter limiter, int c_enable, int f_enable);

/* PRODUCTION_METHODS */

/* Kernel string and copy shims used by the actual proc write function. */
static int copy_failure;
static unsigned long copy_from_user(void *to, const void *from, size_t n)
{
    if (copy_failure)
        return n ? n : 1;
    memcpy(to, from, n);
    return 0;
}

static char *strim(char *s)
{
    char *end;

    while (isspace((unsigned char)*s))
        s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static char *skip_spaces(const char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    return (char *)s;
}

static bool sysfs_streq(const char *a, const char *b)
{
    size_t len_a = strlen(a), len_b = strlen(b);

    if (len_a && a[len_a - 1] == '\n')
        len_a--;
    if (len_b && b[len_b - 1] == '\n')
        len_b--;
    return len_a == len_b && !memcmp(a, b, len_a);
}

static int kstrtouint(const char *s, unsigned int base, unsigned int *result)
{
    char *end;
    unsigned long value;

    if (!s[0] || s[0] == '-' || isspace((unsigned char)s[0]))
        return -EINVAL;
    errno = 0;
    value = strtoul(s, &end, base);
    if (end == s || (*end && strcmp(end, "\n")))
        return -EINVAL;
    if (errno == ERANGE || value > UINT_MAX)
        return -ERANGE;
    *result = (unsigned int)value;
    return 0;
}

/* PRODUCTION_PARSERS */

struct call {
    enum gpufreq_target target;
    enum gpuppm_limiter limiter;
    int ceiling;
    int floor;
};
static struct call calls[256];
static unsigned int call_count, fail_call;
static int hardware_failure;
static bool require_boost_lock;
static int switch_count;
static pthread_mutex_t raw_block_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t raw_block_changed = PTHREAD_COND_INITIALIZER;
static bool block_raw, raw_entered, release_raw, release_thread_started;

static int apply_idx(int old, int requested)
{
    if (requested == GPUPPM_KEEP_IDX)
        return old;
    if (requested < 0)
        return GPUPPM_DEFAULT_IDX;
    return requested;
}

/* Fake GPU PPM boundary: record requests, optionally fail, publish success. */
static int gpufreq_set_limit_raw(enum gpufreq_target target,
    enum gpuppm_limiter limiter, int ceiling_info, int floor_info)
{
    int ret = gpufreq_validate_target(&target);

    if (ret)
        return ret;
    if (require_boost_lock)
        assert(pthread_mutex_trylock(&gpufreq_ui_boost_lock) == EBUSY);
    assert(call_count < ARRAY_SIZE(calls));
    calls[call_count++] = (struct call) { target, limiter, ceiling_info, floor_info };
    mutex_lock(&raw_block_lock);
    if (block_raw) {
        raw_entered = true;
        assert(pthread_cond_broadcast(&raw_block_changed) == 0);
        while (!release_raw)
            assert(pthread_cond_wait(&raw_block_changed, &raw_block_lock) == 0);
        block_raw = false;
    }
    mutex_unlock(&raw_block_lock);
    if (hardware_failure && (!fail_call || call_count == fail_call))
        return hardware_failure;
    if (g_shared_status && target == TARGET_GPU) {
        struct gpuppm_limit_info *limit = &g_shared_status->limit_table[limiter];

        limit->ceiling = apply_idx(limit->ceiling, ceiling_info);
        limit->floor = apply_idx(limit->floor, floor_info);
    }
    return 0;
}

static int gpufreq_switch_limit(enum gpufreq_target target,
    enum gpuppm_limiter limiter, int c_enable, int f_enable)
{
    (void)target;
    (void)limiter;
    (void)c_enable;
    (void)f_enable;
    switch_count++;
    return 0;
}

static void reset(void)
{
    static const unsigned int freqs[] = { 1000000, 800000, 700000, 600000, 400000 };
    unsigned int i;

    memset(&shared, 0, sizeof(shared));
    g_shared_status = &shared;
    g_dual_buck = 0;
    shared.opp_num_gpu = ARRAY_SIZE(freqs);
    for (i = 0; i < ARRAY_SIZE(freqs); i++)
        shared.working_table_gpu[i].freq = freqs[i];
    for (i = 0; i < LIMIT_NUM; i++) {
        shared.limit_table[i].ceiling = GPUPPM_DEFAULT_IDX;
        shared.limit_table[i].floor = GPUPPM_DEFAULT_IDX;
        shared.limit_table[i].c_enable = 1;
        shared.limit_table[i].f_enable = 1;
    }
    g_ui_boost_initialized = false;
    g_ui_boost_suspended = false;
    g_ui_boost_freq = 0;
    g_api_boost_floor = GPUPPM_DEFAULT_IDX;
    call_count = 0;
    hardware_failure = 0;
    fail_call = 0;
    copy_failure = 0;
    switch_count = 0;
    require_boost_lock = true;
    block_raw = raw_entered = release_raw = release_thread_started = false;
}

static void expect_api_floor(int floor)
{
    assert(shared.limit_table[LIMIT_APIBOOST].floor == floor);
}

static void test_frequency_mapping(void)
{
    static const struct { unsigned int freq; int index; } cases[] = {
        { 1000000, 0 }, { 999999, 0 }, { 800000, 1 }, { 750000, 1 },
        { 700000, 2 }, { 600001, 2 }, { 600000, 3 }, { 400000, 4 }, { 1, 4 },
    };
    unsigned int i, previous;

    reset();
    for (i = 0; i < ARRAY_SIZE(cases); i++) {
        assert(gpufreq_set_ui_boost_khz(cases[i].freq) == 0);
        expect_api_floor(cases[i].index);
        assert(calls[call_count - 1].limiter == LIMIT_APIBOOST);
        assert(calls[call_count - 1].ceiling == GPUPPM_KEEP_IDX);
    }
    previous = call_count;
    assert(gpufreq_set_ui_boost_khz(1000001) < 0);
    assert(gpufreq_set_ui_boost_khz(UINT_MAX) < 0);
    assert(call_count == previous);
    expect_api_floor(4);
    puts("PASS: frequency mapping uses exact, next-higher and lowest available OPPs");
}

static void test_malformed_tables(void)
{
    static const int bad_counts[] = { -1, 0, GPUFREQ_MAX_OPP_NUM + 1, INT_MAX };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(bad_counts); i++) {
        reset();
        shared.opp_num_gpu = bad_counts[i];
        assert(gpufreq_set_ui_boost_khz(700000) < 0);
        assert(call_count == 0);
    }
    reset();
    shared.working_table_gpu[4].freq = 0;
    assert(gpufreq_set_ui_boost_khz(1000000) < 0);
    assert(call_count == 0);
    reset();
    shared.working_table_gpu[3].freq = 800000;
    assert(gpufreq_set_ui_boost_khz(700000) < 0);
    assert(call_count == 0);
    reset();
    shared.working_table_gpu[3].freq = 700000;
    assert(gpufreq_set_ui_boost_khz(700000) < 0);
    assert(call_count == 0);
    reset();
    g_shared_status = NULL;
    assert(gpufreq_set_ui_boost_khz(700000) < 0);
    assert(call_count == 0);
    reset();
    shared.opp_num_gpu = GPUFREQ_MAX_OPP_NUM;
    for (i = 0; i < GPUFREQ_MAX_OPP_NUM; i++)
        shared.working_table_gpu[i].freq = 1000000 - i * 10000;
    assert(gpufreq_set_ui_boost_khz(700000) == 0);
    expect_api_floor(30);
    puts("PASS: malformed and full-size runtime tables are bounded and validated");
}

static void test_legacy_overlap_and_release(void)
{
    reset();
    shared.limit_table[LIMIT_APIBOOST].floor = 3;
    assert(gpufreq_set_ui_boost_khz(700000) == 0);
    expect_api_floor(2);
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_APIBOOST, GPUPPM_KEEP_IDX, 1) == 0);
    expect_api_floor(1);
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_APIBOOST, GPUPPM_KEEP_IDX, 4) == 0);
    expect_api_floor(2);
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_APIBOOST, 0, GPUPPM_KEEP_IDX) == 0);
    expect_api_floor(2);
    assert(shared.limit_table[LIMIT_APIBOOST].ceiling == 0);
    shared.opp_num_gpu = 0;
    assert(gpufreq_set_ui_boost_khz(0) == 0);
    expect_api_floor(4);
    assert(shared.limit_table[LIMIT_APIBOOST].ceiling == 0);
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_APIBOOST,
        GPUPPM_KEEP_IDX, GPUPPM_RESET_IDX) == 0);
    expect_api_floor(GPUPPM_DEFAULT_IDX);

    reset();
    assert(gpufreq_set_ui_boost_khz(700000) == 0);
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_APIBOOST,
        GPUPPM_KEEP_IDX, GPUPPM_RESET_IDX) == 0);
    expect_api_floor(2);
    assert(gpufreq_set_ui_boost_khz(0) == 0);
    expect_api_floor(GPUPPM_DEFAULT_IDX);
    puts("PASS: legacy API floor and ceiling survive UI updates and invalid-table release");
}

static void test_shared_table_update(void)
{
    reset();
    assert(gpufreq_set_ui_boost_khz(700000) == 0);
    expect_api_floor(2);
    shared.working_table_gpu[1].freq = 700000;
    shared.working_table_gpu[2].freq = 650000;
    assert(gpufreq_set_ui_boost_khz(700000) == 0);
    expect_api_floor(1);
    shared.working_table_gpu[1].freq = 680000;
    assert(gpufreq_set_ui_boost_khz(700000) == 0);
    expect_api_floor(0);
    shared.working_table_gpu[1].freq = 800000;
    shared.working_table_gpu[2].freq = 700000;
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_APIBOOST, GPUPPM_KEEP_IDX, 4) == 0);
    expect_api_floor(2);
    puts("PASS: repeated requests re-read the published working OPP table");
}

static void test_debug_suspension(void)
{
    unsigned int previous;

    reset();
    shared.limit_table[LIMIT_APIBOOST].floor = 4;
    assert(gpufreq_set_ui_boost_khz(700000) == 0);
    previous = call_count;
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_DEBUG, 3, 3) == 0);
    assert(call_count == previous + 2);
    assert(calls[previous].limiter == LIMIT_APIBOOST);
    assert(calls[previous].floor == 4);
    assert(calls[previous + 1].limiter == LIMIT_DEBUG);
    expect_api_floor(4);
    assert(shared.limit_table[LIMIT_DEBUG].ceiling == 3);
    assert(shared.limit_table[LIMIT_DEBUG].floor == 3);
    assert(gpufreq_set_ui_boost_khz(800000) == 0);
    expect_api_floor(4);
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_APIBOOST, GPUPPM_KEEP_IDX, 3) == 0);
    expect_api_floor(3);
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_DEBUG,
        GPUPPM_RESET_IDX, GPUPPM_KEEP_IDX) == 0);
    expect_api_floor(3); /* A remaining DEBUG floor keeps UI suspended. */
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_DEBUG,
        GPUPPM_KEEP_IDX, GPUPPM_RESET_IDX) == 0);
    expect_api_floor(1);
    assert(gpufreq_set_ui_boost_khz(0) == 0);
    expect_api_floor(3);

    reset();
    shared.limit_table[LIMIT_DEBUG].ceiling = 3;
    assert(gpufreq_set_ui_boost_khz(700000) == 0);
    expect_api_floor(GPUPPM_DEFAULT_IDX);
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_DEBUG,
        GPUPPM_RESET_IDX, GPUPPM_RESET_IDX) == 0);
    expect_api_floor(2);
    puts("PASS: manual DEBUG ceilings/floors suspend UI and neutral DEBUG restores it");
}

static void test_transport_failures(void)
{
    unsigned int previous;

    reset();
    hardware_failure = -EIO;
    assert(gpufreq_set_ui_boost_khz(700000) == -EIO);
    expect_api_floor(GPUPPM_DEFAULT_IDX);
    hardware_failure = 0;
    assert(gpufreq_set_ui_boost_khz(700000) == 0);
    expect_api_floor(2);
    hardware_failure = -EIO;
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_APIBOOST, GPUPPM_KEEP_IDX, 1) == -EIO);
    expect_api_floor(2);
    hardware_failure = 0;
    assert(gpufreq_set_ui_boost_khz(0) == 0);
    expect_api_floor(1); /* The failed legacy write's latest intent is retained. */

    reset();
    assert(gpufreq_set_ui_boost_khz(700000) == 0);
    hardware_failure = -EIO;
    assert(gpufreq_set_ui_boost_khz(0) == -EIO);
    expect_api_floor(2);
    hardware_failure = 0;
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_APIBOOST,
        GPUPPM_KEEP_IDX, GPUPPM_KEEP_IDX) == 0);
    expect_api_floor(GPUPPM_DEFAULT_IDX); /* A failed release is not resurrected. */

    reset();
    shared.limit_table[LIMIT_APIBOOST].floor = 4;
    assert(gpufreq_set_ui_boost_khz(700000) == 0);
    previous = call_count;
    hardware_failure = -EIO;
    fail_call = previous + 1;
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_DEBUG, 3, 3) == -EIO);
    assert(call_count == previous + 1); /* Do not set DEBUG if UI removal failed. */
    assert(shared.limit_table[LIMIT_DEBUG].ceiling == GPUPPM_DEFAULT_IDX);
    expect_api_floor(2);
    hardware_failure = 0;
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_APIBOOST,
        GPUPPM_KEEP_IDX, GPUPPM_KEEP_IDX) == 0);
    expect_api_floor(4);
    previous = call_count;
    hardware_failure = -EIO;
    fail_call = previous + 2;
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_DEBUG, 3, 3) == -EIO);
    assert(call_count == previous + 2);
    expect_api_floor(4);
    assert(shared.limit_table[LIMIT_DEBUG].ceiling == GPUPPM_DEFAULT_IDX);
    hardware_failure = 0;
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_APIBOOST,
        GPUPPM_KEEP_IDX, GPUPPM_KEEP_IDX) == 0);
    expect_api_floor(4); /* A failed DEBUG write leaves UI suspended. */
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_DEBUG, 3, 3) == 0);
    previous = call_count;
    hardware_failure = -EIO;
    fail_call = previous + 3;
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_DEBUG,
        GPUPPM_RESET_IDX, GPUPPM_RESET_IDX) == -EIO);
    assert(call_count == previous + 3);
    expect_api_floor(4);
    assert(shared.limit_table[LIMIT_DEBUG].ceiling == GPUPPM_DEFAULT_IDX);
    hardware_failure = 0;
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_DEBUG,
        GPUPPM_RESET_IDX, GPUPPM_RESET_IDX) == 0);
    expect_api_floor(2);
    puts("PASS: failed apply, release and each DEBUG phase retain retryable intent");
}

static void test_unrelated_paths(void)
{
    unsigned int previous;

    reset();
    require_boost_lock = false;
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_THERMAL_AP, 3, GPUPPM_RESET_IDX) == 0);
    assert(call_count == 1 && calls[0].ceiling == 3);
    assert(calls[0].limiter == LIMIT_THERMAL_AP && calls[0].floor == GPUPPM_RESET_IDX);
    assert(!g_ui_boost_initialized);
    g_dual_buck = 1;
    previous = call_count;
    assert(gpufreq_set_ui_boost_khz(700000) < 0 && call_count == previous);
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_APIBOOST, 2, 3) == 0);
    assert(calls[call_count - 1].target == TARGET_STACK);
    assert(calls[call_count - 1].floor == 3);
    assert(!g_ui_boost_initialized);
    previous = call_count;
    assert(gpufreq_set_limit(TARGET_INVALID, LIMIT_APIBOOST, 2, 3) < 0);
    assert(call_count == previous);
    puts("PASS: unrelated limiters and dual-buck requests retain their original path");
}

static void test_startup_legacy_behavior(void)
{
    reset();
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_APIBOOST, 0, 4) == 0);
    assert(call_count == 1 && !g_ui_boost_initialized);
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_DEBUG, 3, 3) == 0);
    assert(call_count == 2 && calls[1].limiter == LIMIT_DEBUG);
    assert(!g_ui_boost_initialized);
    assert(gpufreq_set_ui_boost_khz(700000) == 0);
    expect_api_floor(4);
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_DEBUG,
        GPUPPM_RESET_IDX, GPUPPM_RESET_IDX) == 0);
    expect_api_floor(2);
    assert(gpufreq_set_ui_boost_khz(0) == 0);
    expect_api_floor(4);
    assert(shared.limit_table[LIMIT_APIBOOST].ceiling == 0);
    reset();
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_APIBOOST, GPUPPM_KEEP_IDX, 3) == 0);
    assert(gpufreq_set_ui_boost_khz(0) == 0);
    expect_api_floor(3); /* A release may be the first UI command after boot. */
    puts("PASS: startup legacy calls stay direct and first UI command captures their state");
}

static void *legacy_thread(void *unused)
{
    (void)unused;
    assert(gpufreq_set_limit(TARGET_DEFAULT, LIMIT_APIBOOST, GPUPPM_KEEP_IDX, 1) == 0);
    return NULL;
}

static void *ui_release_thread(void *unused)
{
    (void)unused;
    mutex_lock(&raw_block_lock);
    release_thread_started = true;
    assert(pthread_cond_broadcast(&raw_block_changed) == 0);
    mutex_unlock(&raw_block_lock);
    assert(gpufreq_set_ui_boost_khz(0) == 0);
    return NULL;
}

static void test_serialization(void)
{
    pthread_t legacy_worker, release_worker;

    reset();
    assert(gpufreq_set_ui_boost_khz(700000) == 0);
    block_raw = true;
    assert(pthread_create(&legacy_worker, NULL, legacy_thread, NULL) == 0);
    mutex_lock(&raw_block_lock);
    while (!raw_entered)
        assert(pthread_cond_wait(&raw_block_changed, &raw_block_lock) == 0);
    /* The real production mutex is held throughout the pending hardware call. */
    assert(pthread_mutex_trylock(&gpufreq_ui_boost_lock) == EBUSY);
    assert(pthread_create(&release_worker, NULL, ui_release_thread, NULL) == 0);
    while (!release_thread_started)
        assert(pthread_cond_wait(&raw_block_changed, &raw_block_lock) == 0);
    release_raw = true;
    assert(pthread_cond_broadcast(&raw_block_changed) == 0);
    mutex_unlock(&raw_block_lock);
    assert(pthread_join(legacy_worker, NULL) == 0);
    assert(pthread_join(release_worker, NULL) == 0);
    assert(call_count == 3);
    assert(calls[1].floor == 1 && calls[2].floor == 1);
    expect_api_floor(1);
    assert(g_ui_boost_freq == 0);
    puts("PASS: overlapping legacy and UI release calls serialize through hardware completion");
}

static ssize_t write_command(const char *command)
{
    return limit_table_proc_write(NULL, command, strlen(command), NULL);
}

static void test_parser(void)
{
    static const char *invalid[] = {
        "boost", "boost ", "boost -1", "boost -700000", "boost 0x10",
        "boost 700000junk", "boost 700000 1", "boost 700000\nset 1 -1 -1",
        "boost 4294967296", "boost 9999999999999999999999999999999999999999",
        "boost banana", "boost 700.0", "boost 1e6", "boost +700000", "boosting 700000",
    };
    char long_command[160], embedded_nul[] = "boost 700000\0unexpected";
    unsigned int i, previous;

    reset();
    assert(write_command("boost 700000\n") == (ssize_t)strlen("boost 700000\n"));
    expect_api_floor(2);
    previous = call_count;
    assert(limit_table_proc_write(NULL, "", 0, NULL) < 0);
    for (i = 0; i < ARRAY_SIZE(invalid); i++) {
        assert(write_command(invalid[i]) < 0);
        assert(call_count == previous);
    }
    memset(long_command, ' ', sizeof(long_command));
    memcpy(long_command, "boost 700000", 12);
    memcpy(long_command + 130, "1", 1);
    assert(limit_table_proc_write(NULL, long_command, sizeof(long_command), NULL) < 0);
    assert(call_count == previous);
    assert(limit_table_proc_write(NULL, embedded_nul, sizeof(embedded_nul) - 1, NULL) < 0);
    assert(call_count == previous);
    copy_failure = 1;
    assert(write_command("boost 0") < 0);
    assert(call_count == previous);
    copy_failure = 0;
    assert(write_command(" \tboost\t  700000 \n") == (ssize_t)strlen(" \tboost\t  700000 \n"));
    expect_api_floor(2);
    assert(write_command("boost 0") == (ssize_t)strlen("boost 0"));
    expect_api_floor(GPUPPM_DEFAULT_IDX);
    require_boost_lock = false;
    assert(write_command("set 04 02 -1") == (ssize_t)strlen("set 04 02 -1"));
    assert(calls[call_count - 1].limiter == LIMIT_THERMAL_AP);
    assert(calls[call_count - 1].ceiling == 2);
    assert(write_command("switch 04 01 00") == (ssize_t)strlen("switch 04 01 00"));
    assert(switch_count == 1);
    puts("PASS: proc input rejects malformed, negative, overflowing and truncated boosts");
}

int main(void)
{
    test_frequency_mapping();
    test_malformed_tables();
    test_legacy_overlap_and_release();
    test_shared_table_update();
    test_debug_suspension();
    test_transport_failures();
    test_unrelated_paths();
    test_startup_legacy_behavior();
    test_serialization();
    test_parser();
    puts("PASS: all production UI boost host tests");
    return 0;
}
