// SPDX-License-Identifier: GPL-2.0
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>

#define mutex_lock pthread_mutex_lock
#define mutex_unlock pthread_mutex_unlock
#define WRITE_ONCE(target, value) __atomic_store_n(&(target), (value), __ATOMIC_RELEASE)
#define READ_ONCE(target) __atomic_load_n(&(target), __ATOMIC_ACQUIRE)
#define EXPORT_SYMBOL(name)

struct charger_device;
struct charger_ops {
    int (*enable)(struct charger_device *, bool);
    int (*is_powerpath_active)(struct charger_device *, bool *);
    int (*is_charge_enabled)(struct charger_device *, bool *);
};
struct charger_device {
    const struct charger_ops *ops;
    pthread_mutex_t enable_lock;
    bool charge_inhibited;
};

/* PRODUCTION_METHODS */

static bool enabled;
static int calls, failure;
static pthread_mutex_t barrier = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool hold_enable, inside_enable, release_enable;

static int hardware_enable(struct charger_device *dev, bool en)
{
    (void)dev;
    pthread_mutex_lock(&barrier);
    ++calls;
    if (en && hold_enable) {
        inside_enable = true;
        pthread_cond_broadcast(&changed);
        while (!release_enable)
            pthread_cond_wait(&changed, &barrier);
    }
    if (!failure)
        enabled = en;
    pthread_mutex_unlock(&barrier);
    return failure;
}

static void *enable_thread(void *arg)
{
    assert(charger_dev_enable(arg, true) == 0);
    return NULL;
}

static void *inhibit_thread(void *arg)
{
    assert(charger_dev_set_charge_inhibited(arg, true) == 0);
    return NULL;
}

int main(void)
{
    const struct charger_ops ops = { .enable = hardware_enable };
    struct charger_device dev = { .ops = &ops, .enable_lock = PTHREAD_MUTEX_INITIALIZER };
    pthread_t enable_worker, inhibit_worker;
    int previous;

    assert(charger_dev_set_charge_inhibited(NULL, true) == -ENODEV);
    assert(charger_dev_enable(NULL, true) == -EOPNOTSUPP);
    assert(!charger_dev_is_charge_inhibited(NULL));
    assert(charger_dev_enable(&dev, true) == 0 && enabled);
    assert(charger_dev_set_charge_inhibited(&dev, true) == 0 && !enabled);
    previous = calls;
    assert(charger_dev_enable(&dev, true) == -EBUSY && calls == previous);
    assert(charger_dev_enable(&dev, false) == 0 && !enabled);
    previous = calls;
    assert(charger_dev_set_charge_inhibited(&dev, false) == 0);
    assert(calls == previous && !enabled); /* Policy, not release, may resume charging. */

    assert(charger_dev_enable(&dev, true) == 0 && enabled);
    failure = -EIO;
    assert(charger_dev_set_charge_inhibited(&dev, true) == -EIO);
    assert(charger_dev_is_charge_inhibited(&dev));
    assert(charger_dev_enable(&dev, true) == -EBUSY);
    failure = 0;
    assert(charger_dev_set_charge_inhibited(&dev, true) == 0 && !enabled);
    assert(charger_dev_set_charge_inhibited(&dev, false) == 0);

    /* A vote must wait for an already-running enable and disable it afterwards. */
    hold_enable = true;
    assert(pthread_create(&enable_worker, NULL, enable_thread, &dev) == 0);
    pthread_mutex_lock(&barrier);
    while (!inside_enable)
        pthread_cond_wait(&changed, &barrier);
    assert(pthread_mutex_trylock(&dev.enable_lock) == EBUSY);
    assert(pthread_create(&inhibit_worker, NULL, inhibit_thread, &dev) == 0);
    release_enable = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&barrier);
    pthread_join(enable_worker, NULL);
    pthread_join(inhibit_worker, NULL);
    assert(charger_dev_is_charge_inhibited(&dev) && !enabled);
    previous = calls;
    assert(charger_dev_enable(&dev, true) == -EBUSY && calls == previous);

    dev.ops = NULL;
    assert(charger_dev_set_charge_inhibited(&dev, true) == -EOPNOTSUPP);
    assert(charger_dev_is_charge_inhibited(&dev));
    assert(charger_dev_set_charge_inhibited(&dev, false) == 0);
    assert(!charger_dev_is_charge_inhibited(&dev));
    puts("PASS: charge inhibit, release, failed hardware and concurrent enable");
    return 0;
}
