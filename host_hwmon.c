// SPDX-License-Identifier: GPL-2.0
/*
 * host_hwmon - Expose host CPU/GPU temperatures as hwmon sensors in a VM
 * Mimics coretemp format so btop shows per-core temperatures
 * Receives data via sysfs from userspace reader connected to virtio-serial
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/hwmon.h>
#include <linux/mutex.h>
#include <linux/string.h>

/* 245K: Package + 14 cores + A3000 GPU = 16 channels */
#define NUM_CHANNELS 16
/* Channel map: 0=Package, 1-14=Cores, 15=GPU */
#define CH_PKG 0
#define CH_GPU 15
#define NUM_CORES 14

static struct platform_device *pdev;
static struct device *hwmon_dev;
static DEFINE_MUTEX(temp_lock);

static long temps[NUM_CHANNELS];
static char labels[NUM_CHANNELS][20];

/* Core IDs matching the 245K topology */
static const int core_ids[NUM_CORES] = {0,1,2,3,4,5,6,7,8,12,16,20,24,28};

static umode_t host_is_visible(const void *data,
    enum hwmon_sensor_types type, u32 attr, int channel)
{
    if (type != hwmon_temp || channel >= NUM_CHANNELS)
        return 0;
    if (attr == hwmon_temp_input || attr == hwmon_temp_label)
        return 0444;
    return 0;
}

static int host_read(struct device *dev, enum hwmon_sensor_types type,
    u32 attr, int channel, long *val)
{
    if (type != hwmon_temp || attr != hwmon_temp_input || channel >= NUM_CHANNELS)
        return -EOPNOTSUPP;
    mutex_lock(&temp_lock);
    *val = temps[channel];
    mutex_unlock(&temp_lock);
    return 0;
}

static int host_read_string(struct device *dev,
    enum hwmon_sensor_types type, u32 attr, int channel, const char **str)
{
    if (type != hwmon_temp || attr != hwmon_temp_label || channel >= NUM_CHANNELS)
        return -EOPNOTSUPP;
    *str = labels[channel];
    return 0;
}

static ssize_t update_temps_store(struct device *dev,
    struct device_attribute *attr, const char *buf, size_t count)
{
    char tmp[512];
    char *p, *token;
    int core_idx = 0;

    if (count >= sizeof(tmp))
        return -EINVAL;
    memcpy(tmp, buf, count);
    tmp[count] = '\0';

    mutex_lock(&temp_lock);
    p = tmp;
    while ((token = strsep(&p, " \t\n")) != NULL) {
        long val;
        int cn;
        if (*token == '\0')
            continue;
        if (sscanf(token, "cpu=%ld", &val) == 1)
            temps[CH_PKG] = val;
        else if (sscanf(token, "gpu=%ld", &val) == 1)
            temps[CH_GPU] = val;
        else if (sscanf(token, "c%d=%ld", &cn, &val) == 2 && core_idx < NUM_CORES)
            temps[1 + core_idx++] = val;
    }
    mutex_unlock(&temp_lock);
    return count;
}
static DEVICE_ATTR_WO(update_temps);

static const u32 temp_config[NUM_CHANNELS + 1] = {
    [0 ... NUM_CHANNELS - 1] = HWMON_T_INPUT | HWMON_T_LABEL,
    0
};

static const struct hwmon_channel_info temp_info = {
    .type = hwmon_temp,
    .config = temp_config,
};

static const struct hwmon_channel_info * const info[] = { &temp_info, NULL };
static const struct hwmon_ops ops = {
    .is_visible = host_is_visible,
    .read = host_read,
    .read_string = host_read_string,
};
static const struct hwmon_chip_info chip = { .ops = &ops, .info = info };

static int __init host_hwmon_init(void)
{
    int i, ret;

    /* Label format matching coretemp exactly */
    snprintf(labels[CH_PKG], sizeof(labels[0]), "Package id 0");
    for (i = 0; i < NUM_CORES; i++)
        snprintf(labels[1 + i], sizeof(labels[0]), "Core %d", core_ids[i]);
    snprintf(labels[CH_GPU], sizeof(labels[0]), "A3000 GPU");

    /* Register as coretemp.0 to match btop's expectations */
    pdev = platform_device_register_simple("coretemp", 0, NULL, 0);
    if (IS_ERR(pdev))
        return PTR_ERR(pdev);

    ret = device_create_file(&pdev->dev, &dev_attr_update_temps);
    if (ret) {
        platform_device_unregister(pdev);
        return ret;
    }

    /* Register hwmon as "coretemp" */
    hwmon_dev = devm_hwmon_device_register_with_info(&pdev->dev,
        "coretemp", NULL, &chip, NULL);
    if (IS_ERR(hwmon_dev)) {
        device_remove_file(&pdev->dev, &dev_attr_update_temps);
        platform_device_unregister(pdev);
        return PTR_ERR(hwmon_dev);
    }

    pr_info("host_hwmon: registered as coretemp with %d channels\n", NUM_CHANNELS);
    return 0;
}

static void __exit host_hwmon_exit(void)
{
    device_remove_file(&pdev->dev, &dev_attr_update_temps);
    platform_device_unregister(pdev);
    pr_info("host_hwmon: unregistered\n");
}

module_init(host_hwmon_init);
module_exit(host_hwmon_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("carrier-admin");
MODULE_DESCRIPTION("Host temps via virtio-serial, mimics coretemp for btop");
MODULE_VERSION("3.0");
