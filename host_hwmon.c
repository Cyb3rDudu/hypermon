// SPDX-License-Identifier: GPL-2.0
/*
 * host_hwmon - Expose host CPU/GPU temperatures, fan RPMs as hwmon sensors in a VM
 * Mimics coretemp format so btop shows per-core temperatures
 * Receives data via sysfs from userspace reader connected to virtio-serial
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/hwmon.h>
#include <linux/mutex.h>
#include <linux/string.h>

/* 245K: Package + 14 cores + A3000 GPU = 16 temp channels */
#define NUM_TEMP_CH 16
/* Channel map: 0=Package, 1-14=Cores, 15=GPU */
#define CH_PKG 0
#define CH_GPU 15
#define NUM_CORES 14

/* Fan channels: CPU Fan, Noctua (pump header), System Fan */
#define NUM_FAN_CH 3

static struct platform_device *pdev;
static struct device *hwmon_dev;
static DEFINE_MUTEX(data_lock);

static long temps[NUM_TEMP_CH];
static char temp_labels[NUM_TEMP_CH][20];

static long fans[NUM_FAN_CH];
static const char *fan_labels[NUM_FAN_CH] = {
    "Host CPU Fan",
    "Host Noctua",
    "Host System Fan",
};

/* Core IDs matching the 245K topology */
static const int core_ids[NUM_CORES] = {0,1,2,3,4,5,6,7,8,12,16,20,24,28};

static umode_t host_is_visible(const void *data,
    enum hwmon_sensor_types type, u32 attr, int channel)
{
    if (type == hwmon_temp && channel < NUM_TEMP_CH) {
        if (attr == hwmon_temp_input || attr == hwmon_temp_label)
            return 0444;
    }
    if (type == hwmon_fan && channel < NUM_FAN_CH) {
        if (attr == hwmon_fan_input || attr == hwmon_fan_label)
            return 0444;
    }
    return 0;
}

static int host_read(struct device *dev, enum hwmon_sensor_types type,
    u32 attr, int channel, long *val)
{
    mutex_lock(&data_lock);
    if (type == hwmon_temp && attr == hwmon_temp_input && channel < NUM_TEMP_CH) {
        *val = temps[channel];
        mutex_unlock(&data_lock);
        return 0;
    }
    if (type == hwmon_fan && attr == hwmon_fan_input && channel < NUM_FAN_CH) {
        *val = fans[channel];
        mutex_unlock(&data_lock);
        return 0;
    }
    mutex_unlock(&data_lock);
    return -EOPNOTSUPP;
}

static int host_read_string(struct device *dev,
    enum hwmon_sensor_types type, u32 attr, int channel, const char **str)
{
    if (type == hwmon_temp && attr == hwmon_temp_label && channel < NUM_TEMP_CH) {
        *str = temp_labels[channel];
        return 0;
    }
    if (type == hwmon_fan && attr == hwmon_fan_label && channel < NUM_FAN_CH) {
        *str = fan_labels[channel];
        return 0;
    }
    return -EOPNOTSUPP;
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

    mutex_lock(&data_lock);
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
        else if (sscanf(token, "f1=%ld", &val) == 1)
            fans[0] = val;
        else if (sscanf(token, "f2=%ld", &val) == 1)
            fans[1] = val;
        else if (sscanf(token, "f3=%ld", &val) == 1)
            fans[2] = val;
    }
    mutex_unlock(&data_lock);
    return count;
}
static DEVICE_ATTR_WO(update_temps);

/* Temperature channel config */
static const u32 temp_config[NUM_TEMP_CH + 1] = {
    [0 ... NUM_TEMP_CH - 1] = HWMON_T_INPUT | HWMON_T_LABEL,
    0
};

/* Fan channel config */
static const u32 fan_config[NUM_FAN_CH + 1] = {
    [0 ... NUM_FAN_CH - 1] = HWMON_F_INPUT | HWMON_F_LABEL,
    0
};

static const struct hwmon_channel_info temp_info = {
    .type = hwmon_temp,
    .config = temp_config,
};

static const struct hwmon_channel_info fan_info = {
    .type = hwmon_fan,
    .config = fan_config,
};

static const struct hwmon_channel_info * const info[] = {
    &temp_info,
    &fan_info,
    NULL
};

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
    snprintf(temp_labels[CH_PKG], sizeof(temp_labels[0]), "Package id 0");
    for (i = 0; i < NUM_CORES; i++)
        snprintf(temp_labels[1 + i], sizeof(temp_labels[0]), "Core %d", core_ids[i]);
    snprintf(temp_labels[CH_GPU], sizeof(temp_labels[0]), "A3000 GPU");

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

    pr_info("host_hwmon: registered with %d temp + %d fan channels\n",
        NUM_TEMP_CH, NUM_FAN_CH);
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
MODULE_DESCRIPTION("Host sensors via virtio-serial, mimics coretemp for btop");
MODULE_VERSION("3.1");
