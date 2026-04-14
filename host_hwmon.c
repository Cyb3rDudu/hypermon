// SPDX-License-Identifier: GPL-2.0
/*
 * host_hwmon - Expose host CPU/GPU temps, fans, power, voltages, freqs as hwmon sensors in a VM
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

/* Power channels: CPU Package */
#define NUM_POWER_CH 1

/* Voltage channels: VCore, DRAM, +12V, +5V, +3.3V */
#define NUM_IN_CH 5

/* CPU frequency: per-core MHz via custom sysfs (no hwmon freq type) */
#define NUM_FREQ_CORES 14

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

static long power[NUM_POWER_CH]; /* microwatts */
static const char *power_labels[NUM_POWER_CH] = {
    "Host CPU Package",
};

static long voltages[NUM_IN_CH]; /* millivolts */
static const char *in_labels[NUM_IN_CH] = {
    "Host VCore",
    "Host DRAM",
    "Host +12V",
    "Host +5V",
    "Host +3.3V",
};

static long freqs[NUM_FREQ_CORES]; /* MHz */

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
    if (type == hwmon_power && channel < NUM_POWER_CH) {
        if (attr == hwmon_power_input || attr == hwmon_power_label)
            return 0444;
    }
    if (type == hwmon_in && channel < NUM_IN_CH) {
        if (attr == hwmon_in_input || attr == hwmon_in_label)
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
    if (type == hwmon_power && attr == hwmon_power_input && channel < NUM_POWER_CH) {
        *val = power[channel];
        mutex_unlock(&data_lock);
        return 0;
    }
    if (type == hwmon_in && attr == hwmon_in_input && channel < NUM_IN_CH) {
        *val = voltages[channel];
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
    if (type == hwmon_power && attr == hwmon_power_label && channel < NUM_POWER_CH) {
        *str = power_labels[channel];
        return 0;
    }
    if (type == hwmon_in && attr == hwmon_in_label && channel < NUM_IN_CH) {
        *str = in_labels[channel];
        return 0;
    }
    return -EOPNOTSUPP;
}

static ssize_t update_temps_store(struct device *dev,
    struct device_attribute *attr, const char *buf, size_t count)
{
    char tmp[1024];
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
        else if (sscanf(token, "pw=%ld", &val) == 1)
            power[0] = val;
        else if (sscanf(token, "vcore=%ld", &val) == 1)
            voltages[0] = val;
        else if (sscanf(token, "vdram=%ld", &val) == 1)
            voltages[1] = val;
        else if (sscanf(token, "v12=%ld", &val) == 1)
            voltages[2] = val;
        else if (sscanf(token, "v5=%ld", &val) == 1)
            voltages[3] = val;
        else if (sscanf(token, "v33=%ld", &val) == 1)
            voltages[4] = val;
        else if (sscanf(token, "hz%d=%ld", &cn, &val) == 2) {
            int fi;
            for (fi = 0; fi < NUM_FREQ_CORES; fi++) {
                if (core_ids[fi] == cn) {
                    freqs[fi] = val;
                    break;
                }
            }
        }
    }
    mutex_unlock(&data_lock);
    return count;
}
static DEVICE_ATTR_WO(update_temps);

/* Custom sysfs: show all host CPU frequencies as "coreID: MHz\n" */
static ssize_t host_freqs_show(struct device *dev,
    struct device_attribute *attr, char *buf)
{
    int i;
    ssize_t len = 0;

    mutex_lock(&data_lock);
    for (i = 0; i < NUM_FREQ_CORES; i++)
        len += sysfs_emit_at(buf, len, "Core %d: %ld MHz\n", core_ids[i], freqs[i]);
    mutex_unlock(&data_lock);
    return len;
}
static DEVICE_ATTR_RO(host_freqs);

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

/* Power channel config */
static const u32 power_config[NUM_POWER_CH + 1] = {
    [0 ... NUM_POWER_CH - 1] = HWMON_P_INPUT | HWMON_P_LABEL,
    0
};

/* Voltage (in) channel config */
static const u32 in_config[NUM_IN_CH + 1] = {
    [0 ... NUM_IN_CH - 1] = HWMON_I_INPUT | HWMON_I_LABEL,
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

static const struct hwmon_channel_info power_info = {
    .type = hwmon_power,
    .config = power_config,
};

static const struct hwmon_channel_info in_info = {
    .type = hwmon_in,
    .config = in_config,
};

static const struct hwmon_channel_info * const info[] = {
    &temp_info,
    &fan_info,
    &power_info,
    &in_info,
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

    ret = device_create_file(&pdev->dev, &dev_attr_host_freqs);
    if (ret) {
        device_remove_file(&pdev->dev, &dev_attr_update_temps);
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

    pr_info("host_hwmon: registered with %d temp + %d fan + %d power + %d voltage channels\n",
        NUM_TEMP_CH, NUM_FAN_CH, NUM_POWER_CH, NUM_IN_CH);
    return 0;
}

static void __exit host_hwmon_exit(void)
{
    device_remove_file(&pdev->dev, &dev_attr_host_freqs);
    device_remove_file(&pdev->dev, &dev_attr_update_temps);
    platform_device_unregister(pdev);
    pr_info("host_hwmon: unregistered\n");
}

module_init(host_hwmon_init);
module_exit(host_hwmon_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("carrier-admin");
MODULE_DESCRIPTION("Host sensors via virtio-serial, mimics coretemp for btop");
MODULE_VERSION("3.4");
