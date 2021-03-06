/*
 * Copyright (C) 2012 The Android Open Source Project
 * Copyright (c) 2014, NVIDIA CORPORATION.  All rights reserved.
 * Copyright (C) 2015 The CyanogenMod Project
 * Copyright (C) 2017 The LineageOS Project
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "powerhal.h"
#include <pthread.h>
#include <sys/prctl.h>
#ifdef ENABLE_SATA_STANDBY_MODE
#include "tegra_sata_hal.h"
#endif

#define FOSTER_E_HDD    "/dev/block/sda"
#define HDD_STANDBY_TIMEOUT     60
#define BRICK_STATE_PROP "persist.power.brick"
#define CPU_CC_STATE_NODE "/sys/kernel/debug/cpuidle_t210/fast_cluster_states_enable"
#define CPU_CC_IDLE 0x1
#define CPU_CC_ON 0xcf
#define CPU_CEILING_NODE "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq"
#define CPU_CEILING_IDLE 1836000
#define CPU_CEILING_ON 2014500
#define CPU_FLOOR_ON 1132800
#define CPU_FLOOR_IDLE 1734000
#define CPU_FLOOR_WHITELIST 1100000
#define CPU_FLOOR_NODE "/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq"
#define ETHERNET_POWER_SAVER_NODE "/sys/kernel/rt8168_power/mode"
#define GPU_DELAY_MAX 20
#define GPU_FLOOR_ON 768000000
#define GPU_FLOOR_IDLE 844800000
#define GPU_FLOOR_WHITELIST 384000000
#define GPU_MIN_FREQ "/sys/devices/platform/host1x/gpu.0/devfreq/gpu.0/min_freq"
#define GPU_PMU_STATE "/sys/devices/platform/gpu.0/pmu_state"
#define GPU_RAIL_GATE_NODE "/sys/devices/platform/host1x/gpu.0/railgate_enable"
#define GPU_STATE "/dev/nvhost-gpu"
#define GPU_AELPG_NODE "/sys/devices/platform/gpu.0/aelpg_enable"
#define GPU_BLCG_NODE "/sys/devices/platform/gpu.0/blcg_enable"
#define GPU_ELCG_NODE "/sys/devices/platform/gpu.0/elcg_enable"
#define GPU_ELPG_NODE "/sys/devices/platform/gpu.0/elpg_enable"
#define GPU_SLCG_NODE "/sys/devices/platform/gpu.0/slcg_enable"
#define SOC_DISABLE_DVFS_NODE "/sys/module/tegra21_dvfs/parameters/disable_core"
#define WIFI_PM_NODE "/sys/class/net/wlan0/device/rf_test/pm"
#define WIFI_PM_ENABLE "pm_enable"
#define WIFI_PM_DISABLE "pm_disable"

static struct powerhal_info *pInfo;
static struct input_dev_map input_devs[] = {
        {-1, "raydium_ts\n"},
        {-1, "touch\n"},
       };

/*
 * The order in camera_cap array should match with
 * use case order in camera_usecase_t.
 * if min_online_cpus or max_online_cpus is zero, then
 * it won't be applied.
 * Freq is in KHz.
 */
static camera_cap_t camera_cap[] = {
    /* still preview
     * {min_online_cpus, max_online_cpus, freq, minFreq,
     *  minCpuHint, maxCpuHint, minGpuHint, maxGpuHint, fpsHint}
    */
    {0, 0, 0, 0, 0, 0, 0, 0, 0},
    /* video preview
     * {min_online_cpus, max_online_cpus, freq, minFreq,
     *  minCpuHint, maxCpuHint, minGpuHint, maxGpuHint, fpsHint}
    */
    {0, 0, 0, 0, 0, 0, 0, 0, 0},
    /* video record
     * {min_online_cpus, max_online_cpus, freq, minFreq,
     *  minCpuHint, maxCpuHint, minGpuHint, maxGpuHint, fpsHint}
    */
    {0, 0, 0, 0, 0, 0, 0, 0, 0},
    /* high fps video record
     * {min_online_cpus, max_online_cpus, freq, minFreq,
     *  minCpuHint, maxCpuHint, minGpuHint, maxGpuHint, fpsHint}
    */
    {0, 0, 0, 0, 0, 0, 0, 0, 0},
};

static bool booting;
/* Array of range for bricks, except first value */
static char brick_whitelist[][7] = {
    "PBTEST",
    "PB1706",
    "PB9999",
};

static bool wait_for_gpu_sysfs()
{
    int cnt = GPU_DELAY_MAX >> 2;

    while (--cnt > 0 && !sysfs_exists(GPU_STATE))
        sleep(1);

    if (0 == cnt)
        return false;

    return true;
}

static bool wait_for_gpu_ready(bool on)
{
    char buf[4];

    if (!wait_for_gpu_sysfs())
        return false;

    for (int cnt = 0; cnt < GPU_DELAY_MAX; ++cnt) {
        sleep(1);
        memset(buf, 0, sizeof(buf));
        sysfs_read(GPU_PMU_STATE, buf, sizeof(buf));

        if (atol(buf))
            return true;
    }

    return false;
}

static void* set_gpu_power_knobs_off(void*)
{
    if (!wait_for_gpu_sysfs()) {
        ALOGE("PowerHal: GPU sysfs is missing, skipping policy");
        return NULL;
    }

    sysfs_write_int(GPU_RAIL_GATE_NODE, 0);

    if (!wait_for_gpu_ready(false)) {
        ALOGE("PowerHal: GPU is not ready, skipping power policy");
        return NULL;
    }

    sleep(1);
    sysfs_write_int(GPU_ELPG_NODE, 0);
    sysfs_write_int(GPU_SLCG_NODE, 0);
    sysfs_write_int(GPU_BLCG_NODE, 0);
    sysfs_write_int(GPU_ELCG_NODE, 0);
    booting = 0;
    ALOGI("PowerHal: gpu power knobs are off");

    return NULL;
}

static void* set_gpu_power_knobs_on(void*)
{
    if (!wait_for_gpu_ready(true)) {
        ALOGE("PowerHal: GPU is not ready, skipping power policy");
        return NULL;
    }

    sleep(1);
    sysfs_write_int(GPU_ELCG_NODE, 1);
    sysfs_write_int(GPU_BLCG_NODE, 1);
    sysfs_write_int(GPU_SLCG_NODE, 1);
    sysfs_write_int(GPU_ELPG_NODE, 1);
    booting = 0;
    ALOGI("PowerHal: gpu power knobs are on");

    return NULL;
}

static void set_gpu_knobs(bool on)
{
    char buf[4] = {0};

    sysfs_read(GPU_ELPG_NODE, buf, sizeof(buf));

    if (atol(buf) == on) {
        ALOGI("PowerHal: GPU power knobs are already set");
        return;
    }

    if (booting) {
        pthread_t gpuk_t;
        int err;

        if (on)
            err = pthread_create(&gpuk_t, NULL, &set_gpu_power_knobs_on, NULL);
        else
            err = pthread_create(&gpuk_t, NULL, &set_gpu_power_knobs_off, NULL);

        if (err)
            ALOGE("PowerHal: Failed to create thread %s", __func__);
    } else {
        if (on)
            set_gpu_power_knobs_on(NULL);
        else
            set_gpu_power_knobs_off(NULL);
    }
}

/* Expected format PBXXXX where X is a number */
static bool is_brick_whitelisted(char * brick)
{
    unsigned int i = 0;

    if (strlen(brick) != 6 || strncmp(brick_whitelist[i], brick, 2))
        return false;

    if (!strncmp(brick_whitelist[i], brick, sizeof(brick_whitelist[0])))
        return true;

    /* Each following elements in array are in pair with low and high range */
    for ( i = 1; i < sizeof(brick_whitelist)/sizeof(brick_whitelist[0]); i += 2) {
        long int low = -1, high = -1, brick_num = -1;
        char *p_start;

        p_start = &brick_whitelist[i][2];
        low = strtol(p_start, NULL, 10);
        p_start = &brick_whitelist[i+1][2];
        high = strtol(p_start, NULL, 10);
        p_start = &brick[2];
        brick_num = strtol(p_start, NULL, 10);

        if (brick_num <= high && brick_num >= low)
            return true;
    }

    return false;
}

static void set_power_level_floor(int on)
{
    char brick[PROPERTY_VALUE_MAX+1];
    char platform[PROPERTY_VALUE_MAX+1];

    property_get("ro.hardware", platform, "");
    property_get(BRICK_STATE_PROP, brick, "");

    if (strncmp(platform, "darcy", 5))
        return;

    if (is_brick_whitelisted(brick)) {
            ALOGI("PowerHal: Whitelisted power supply");
            set_gpu_knobs(1);
            sysfs_write_int(ETHERNET_POWER_SAVER_NODE, 1);
            sysfs_write_int(SOC_DISABLE_DVFS_NODE, 0);
            sysfs_write_int(CPU_CC_STATE_NODE, CPU_CC_ON);
            sysfs_write_int(CPU_FLOOR_NODE, CPU_FLOOR_WHITELIST);
            sysfs_write_int(CPU_CEILING_NODE, CPU_CEILING_ON);
            sysfs_write_int(GPU_MIN_FREQ, GPU_FLOOR_WHITELIST);
            return;
    }

    ALOGI("PowerHal: Blacklisted power supply");
    if (on) {
        sysfs_write_int(CPU_CC_STATE_NODE, CPU_CC_ON);
        sysfs_write_int(CPU_FLOOR_NODE, CPU_FLOOR_ON);
        sysfs_write_int(CPU_CEILING_NODE, CPU_CEILING_ON);
        sysfs_write_int(GPU_MIN_FREQ, GPU_FLOOR_ON);
    } else {
        sysfs_write_int(CPU_CC_STATE_NODE, CPU_CC_IDLE);
        sysfs_write_int(CPU_FLOOR_NODE, CPU_FLOOR_IDLE);
        sysfs_write_int(CPU_CEILING_NODE, CPU_CEILING_IDLE);
        sysfs_write_int(GPU_MIN_FREQ, GPU_FLOOR_IDLE);
    }

    sysfs_write_int(ETHERNET_POWER_SAVER_NODE, 0);
    sysfs_write_int(SOC_DISABLE_DVFS_NODE, 1);
    set_gpu_knobs(0);
}

static void shield_power_init(struct power_module *module)
{
    if (!pInfo)
        pInfo = (powerhal_info*)malloc(sizeof(powerhal_info));
    pInfo->input_devs = input_devs;
    pInfo->input_cnt = sizeof(input_devs)/sizeof(struct input_dev_map);

    booting = 1;
    common_power_init(module, pInfo);
}

static void shield_power_set_interactive(struct power_module *module, int on)
{
    int error = 0;

    common_power_set_interactive(module, pInfo, on);
    set_power_level_floor(on);

#ifdef ENABLE_SATA_STANDBY_MODE
    if (!access(FOSTER_E_HDD, F_OK)) {
        /*
        * Turn-off Foster HDD at display off
        */
        ALOGI("HAL: Display is %s, set HDD to %s standby mode.", on?"on":"off", on?"disable":"enter");
        if (on) {
            error = hdd_disable_standby_timer();
            if (error)
                ALOGE("HAL: Failed to set standby timer, error: %d", error);
        }
        else {
            error = hdd_set_standby_timer(HDD_STANDBY_TIMEOUT);
            if (error)
                ALOGE("HAL: Failed to set standby timer, error: %d", error);
        }
    }
#endif
}

static void shield_power_hint(struct power_module *module, power_hint_t hint,
                            void *data)
{
    common_power_hint(module, pInfo, hint, data);
}

static void shield_set_feature(__attribute__ ((unused)) struct power_module *module, feature_t feature, __attribute__ ((unused)) int state)
{
    switch (feature) {
    case POWER_FEATURE_DOUBLE_TAP_TO_WAKE:
        ALOGW("Double tap to wake is not supported\n");
        break;
    default:
        ALOGW("Error setting the feature, it doesn't exist %d\n", feature);
        break;
    }
}

static int shield_power_open(__attribute__ ((unused)) const hw_module_t *module, const char *name,
                          hw_device_t **device)
{
    if (strcmp(name, POWER_HARDWARE_MODULE_ID))
        return -EINVAL;

    if (!pInfo) {
        pInfo = (powerhal_info*)calloc(1, sizeof(powerhal_info));

        common_power_open(pInfo);
        common_power_camera_init(pInfo, camera_cap);
    }

    power_module_t *dev = (power_module_t *)calloc(1,
            sizeof(power_module_t));

    if (!dev) {
        ALOGD("%s: failed to allocate memory", __FUNCTION__);
        return -ENOMEM;
    }

    dev->common.tag = HARDWARE_MODULE_TAG;
    dev->common.module_api_version = POWER_MODULE_API_VERSION_0_2;
    dev->common.hal_api_version = HARDWARE_HAL_API_VERSION;

    dev->init = shield_power_init;
    dev->powerHint = shield_power_hint;
    dev->setInteractive = shield_power_set_interactive;
    dev->setFeature = shield_set_feature;

    *device = (hw_device_t*)dev;

    return 0;
}

static struct hw_module_methods_t power_module_methods = {
    .open = shield_power_open,
};

struct power_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = POWER_MODULE_API_VERSION_0_3,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = POWER_HARDWARE_MODULE_ID,
        .name = "Shield Power HAL",
        .author = "The LineageOS Project",
        .methods = &power_module_methods,
        .dso = NULL,
        .reserved = {0},
    },

    .init = shield_power_init,
    .setInteractive = shield_power_set_interactive,
    .powerHint = shield_power_hint,
    .setFeature = shield_set_feature,
};
