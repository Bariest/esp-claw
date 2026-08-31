/*
 * SPDX-FileCopyrightText: 2026 MangDang
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * IMU for the MangDang MP4 ESP32 CORE: a Bosch BMI270 on the shared I2C bus.
 *
 * This replaces the QMI8658C-over-SPI driver the earlier MangDang board used.
 * That part shared SPI2 with the four servo driver boards, which is why
 * driver_board.h still carries a long note about a bus lock; on this board the
 * IMU has moved to I2C entirely and that hazard is gone.
 *
 * Ownership: this file is the single owner of the BMI270. ESP-Claw ships its
 * own lua_module_imu with a BMI270 backend, and the two must not both call
 * bmi270_sensor_create() on the same chip, so the board defaults leave
 * CONFIG_APP_CLAW_LUA_MODULE_IMU off and Lua reaches the IMU through
 * lua_module_mpx_robot instead.
 *
 * The chip is configured the same way ESP-Claw's backend configures it --
 * +/-16 g, +/-2000 dps, 200 Hz -- so readings are comparable between the two.
 */

#include "imu.h"
#include "robot.h"

#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

extern "C" {
#include "bmi270_api.h"
#include "esp_board_device.h"
#include "esp_board_periph.h"
#include "i2c_bus.h"
}

/* The board manager generates this table as C, and only declares it in a
 * private header (private_inc/esp_board_find_utils.h) that components outside
 * it cannot include. So declare it here.
 *
 * The extern "C" is load-bearing: without it the C++ compiler mangles the name
 * and the failure lands at link time rather than compile time. It also has to
 * sit at namespace scope -- a linkage specification inside a function body is
 * not legal C++, which is what the first attempt got wrong.
 *
 * Walking the table rather than calling esp_board_manager_get_device_config()
 * is deliberate: the walk yields cfg_size, and that is the only thing standing
 * between a YAML/struct mismatch and an IMU that silently returns nonsense.
 * ESP-Claw's own lua_module_imu.c does the same for the same reason. */
extern "C" const esp_board_device_desc_t g_esp_board_devices[];

static const char *TAG = "mpx_imu";

namespace robot {
namespace {

/* Mirror of the `imu_sensor` entry in board_devices.yaml.
 *
 * The board-manager generator emits struct fields in the order they appear in
 * the YAML and silently omits any key that is missing, so this struct and that
 * YAML block have to agree field for field. The cfg_size check below is what
 * turns a disagreement into a clear error instead of garbage readings. Same
 * contract as lua_imu_board_cfg_t in ESP-Claw's lua_module_imu.        */
struct mpx_imu_board_cfg_t {
    const char *name;
    const char *type;
    const char *chip;
    int8_t      i2c_addr;
    int32_t     frequency;
    int8_t      int_gpio_num;
    int8_t      sdo_gpio_num;
    uint8_t     peripheral_count;
    const char *peripheral_name;
};

constexpr const char *kDeviceName = "imu_sensor";

/* Full-scale ranges applied in configure_sensor(). Keep these two constants
 * and that function in step -- they are the only reason the raw LSB counts
 * the chip returns can be turned into g and dps.                          */
constexpr float kAccelRangeG   = 16.0f;
constexpr float kGyroRangeDps  = 2000.0f;
constexpr float kInt16FullScale = 32768.0f;

constexpr int  kPollPeriodMs   = 50;    /* 20 Hz; the gait loop wants no more */
constexpr int  kTaskStackBytes = 4096;
constexpr int  kTaskPriority   = 3;

ImuData          s_latest;
SemaphoreHandle_t s_mutex        = nullptr;
i2c_bus_handle_t s_i2c_bus       = nullptr;
bmi270_handle_t  s_sensor        = nullptr;
bool             s_periph_ref    = false;
const char      *s_periph_name   = nullptr;
TaskHandle_t     s_task          = nullptr;
bool             s_ready         = false;

esp_err_t resolve_board_cfg(const mpx_imu_board_cfg_t **out)
{
    const esp_board_device_desc_t *desc = g_esp_board_devices;

    while (desc != nullptr && desc->name != nullptr) {
        if (strcmp(desc->name, kDeviceName) == 0) {
            if (desc->cfg == nullptr) {
                return ESP_ERR_NOT_FOUND;
            }
            if (desc->cfg_size != sizeof(mpx_imu_board_cfg_t)) {
                ESP_LOGE(TAG,
                         "Board device '%s' cfg_size=%u, expected %u. "
                         "board_devices.yaml is out of sync with "
                         "mpx_imu_board_cfg_t: every field must be present, "
                         "in order, using -1 for unused GPIOs.",
                         kDeviceName, (unsigned)desc->cfg_size,
                         (unsigned)sizeof(mpx_imu_board_cfg_t));
                return ESP_ERR_INVALID_SIZE;
            }
            *out = (const mpx_imu_board_cfg_t *)desc->cfg;
            return ESP_OK;
        }
        desc = desc->next;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t open_i2c_bus(const char *peripheral_name, int frequency)
{
    i2c_master_bus_handle_t master_handle = nullptr;
    i2c_master_bus_config_t *master_cfg = nullptr;

    esp_err_t err = esp_board_periph_ref_handle(peripheral_name, (void **)&master_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot reference board I2C bus '%s': %s",
                 peripheral_name, esp_err_to_name(err));
        return err;
    }
    s_periph_ref  = true;
    s_periph_name = peripheral_name;

    err = esp_board_periph_get_config(peripheral_name, (void **)&master_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot read board I2C config '%s': %s",
                 peripheral_name, esp_err_to_name(err));
        return err;
    }

    /* The BMI270 driver takes an i2c_bus_handle_t from the i2c_bus wrapper
     * rather than the raw master handle, so build one over the same port and
     * pins the board manager already opened. ESP-Claw's lua_module_imu does
     * exactly this.                                                       */
    const i2c_config_t bus_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = master_cfg->sda_io_num,
        .scl_io_num = master_cfg->scl_io_num,
        .sda_pullup_en = master_cfg->flags.enable_internal_pullup != 0,
        .scl_pullup_en = master_cfg->flags.enable_internal_pullup != 0,
        .master = { .clk_speed = (uint32_t)frequency },
        .clk_flags = 0,
    };
    (void)master_handle;

    s_i2c_bus = i2c_bus_create((i2c_port_t)master_cfg->i2c_port, &bus_cfg);
    if (s_i2c_bus == nullptr) {
        ESP_LOGE(TAG, "i2c_bus_create failed on port %d", (int)master_cfg->i2c_port);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t configure_sensor()
{
    uint8_t sens_list[2] = { BMI2_ACCEL, BMI2_GYRO };
    struct bmi2_sens_config config[2] = {};

    config[BMI2_ACCEL].type = BMI2_ACCEL;
    config[BMI2_GYRO].type  = BMI2_GYRO;

    int8_t rslt = bmi2_set_adv_power_save(BMI2_DISABLE, s_sensor);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "bmi2_set_adv_power_save failed: %d", rslt);
        return ESP_FAIL;
    }

    rslt = bmi2_get_sensor_config(config, 2, s_sensor);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "bmi2_get_sensor_config failed: %d", rslt);
        return ESP_FAIL;
    }

    config[BMI2_ACCEL].cfg.acc.odr         = BMI2_ACC_ODR_200HZ;
    config[BMI2_ACCEL].cfg.acc.range       = BMI2_ACC_RANGE_16G;
    config[BMI2_ACCEL].cfg.acc.bwp         = BMI2_ACC_NORMAL_AVG4;
    config[BMI2_ACCEL].cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;

    config[BMI2_GYRO].cfg.gyr.odr          = BMI2_GYR_ODR_200HZ;
    config[BMI2_GYRO].cfg.gyr.range        = BMI2_GYR_RANGE_2000;
    config[BMI2_GYRO].cfg.gyr.bwp          = BMI2_GYR_NORMAL_MODE;
    config[BMI2_GYRO].cfg.gyr.noise_perf   = BMI2_PERF_OPT_MODE;
    config[BMI2_GYRO].cfg.gyr.filter_perf  = BMI2_PERF_OPT_MODE;

    rslt = bmi2_set_sensor_config(config, 2, s_sensor);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "bmi2_set_sensor_config failed: %d", rslt);
        return ESP_FAIL;
    }

    rslt = bmi270_sensor_enable(sens_list, 2, s_sensor);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "bmi270_sensor_enable failed: %d", rslt);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void poll_task(void *)
{
    for (;;) {
        struct bmi2_sens_data data = {};
        if (bmi2_get_sensor_data(&data, s_sensor) == BMI2_OK) {
            ImuData sample;
            sample.ax = (float)data.acc.x / kInt16FullScale * kAccelRangeG;
            sample.ay = (float)data.acc.y / kInt16FullScale * kAccelRangeG;
            sample.az = (float)data.acc.z / kInt16FullScale * kAccelRangeG;
            sample.gx = (float)data.gyr.x / kInt16FullScale * kGyroRangeDps;
            sample.gy = (float)data.gyr.y / kInt16FullScale * kGyroRangeDps;
            sample.gz = (float)data.gyr.z / kInt16FullScale * kGyroRangeDps;

            if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
                s_latest = sample;
                xSemaphoreGive(s_mutex);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(kPollPeriodMs));
    }
}

void cleanup()
{
    if (s_sensor != nullptr) {
        bmi270_sensor_del(&s_sensor);
        s_sensor = nullptr;
    }
    if (s_i2c_bus != nullptr) {
        i2c_bus_delete(&s_i2c_bus);
        s_i2c_bus = nullptr;
    }
    if (s_periph_ref && s_periph_name != nullptr) {
        esp_board_periph_unref_handle(s_periph_name);
        s_periph_ref = false;
    }
}

/* Say which address the chip is actually on, before trying to talk to it.
 *
 * bmi270_sensor_create() takes no address -- the driver picks one, and its own
 * header calls 0x68 "typical". This board straps SDO high, so the chip is on
 * 0x69. If those disagree, create() fails with a bare ESP_FAIL and no hint
 * that addressing is the problem, which is a genuinely horrible thing to debug
 * on a bench with a scope.
 *
 * A bus scan costs a few milliseconds once at boot and turns that into a
 * sentence. It also prints every other device on the bus, which is the first
 * thing anyone wants when a board is new.
 */
void probe_i2c(int8_t declared_addr)
{
    uint8_t found[16] = {0};
    char list[96];
    size_t at = 0;
    bool at_declared = false;
    uint8_t n = i2c_bus_scan(s_i2c_bus, found, sizeof(found));

    for (uint8_t i = 0; i < n && at + 6 < sizeof(list); i++) {
        at += (size_t)snprintf(list + at, sizeof(list) - at, "%s0x%02X",
                               i ? " " : "", found[i]);
        if (found[i] == (uint8_t)declared_addr) {
            at_declared = true;
        }
    }
    ESP_LOGI(TAG, "I2C scan: %u device(s) [%s]", (unsigned)n, n ? list : "none");

    if (!at_declared) {
        ESP_LOGW(TAG, "Nothing answered at 0x%02X, the address board_devices.yaml "
                      "declares for the BMI270 -- check the SDO strap",
                 (unsigned)(uint8_t)declared_addr);
    }
}

}  // namespace

bool imu_init()
{
    if (s_ready) {
        ESP_LOGW(TAG, "imu_init() called twice — ignoring");
        return true;
    }

    const mpx_imu_board_cfg_t *cfg = nullptr;
    esp_err_t err = resolve_board_cfg(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Board device '%s' not usable: %s",
                 kDeviceName, esp_err_to_name(err));
        return false;
    }
    if (cfg->peripheral_count == 0 || cfg->peripheral_name == nullptr) {
        ESP_LOGE(TAG, "Board device '%s' declares no I2C peripheral", kDeviceName);
        return false;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == nullptr) {
        ESP_LOGE(TAG, "mutex alloc failed");
        return false;
    }

    if (open_i2c_bus(cfg->peripheral_name, cfg->frequency) != ESP_OK) {
        cleanup();
        return false;
    }

    probe_i2c(cfg->i2c_addr);

    /* bmi270_sensor_create() uploads the ~8 KB configuration blob, which is
     * why board_devices.yaml marks this device init_skip: true and we do it
     * here rather than on the board manager's boot path.                  */
    err = bmi270_sensor_create(s_i2c_bus, &s_sensor, bmi270_config_file,
                               BMI2_GYRO_CROSS_SENS_ENABLE | BMI2_CRT_RTOSK_ENABLE);
    if (err != ESP_OK || s_sensor == nullptr) {
        ESP_LOGE(TAG, "bmi270_sensor_create failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "If the scan above found the chip at an address other "
                      "than the one this driver uses, that is the reason: "
                      "bmi270_sensor_create() takes no address argument.");
        cleanup();
        return false;
    }

    if (configure_sensor() != ESP_OK) {
        cleanup();
        return false;
    }

    if (xTaskCreate(poll_task, "mpx_imu", kTaskStackBytes, nullptr,
                    kTaskPriority, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "poll task create failed");
        cleanup();
        return false;
    }

    s_ready = true;
    ESP_LOGI(TAG, "BMI270 ready on I2C '%s' addr 0x%02X (+/-%.0fg, +/-%.0fdps, %d Hz poll)",
             cfg->peripheral_name, (unsigned)(uint8_t)cfg->i2c_addr,
             (double)kAccelRangeG, (double)kGyroRangeDps, 1000 / kPollPeriodMs);
    return true;
}

ImuData imu_read()
{
    ImuData copy;
    if (s_mutex != nullptr && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        copy = s_latest;
        xSemaphoreGive(s_mutex);
    }
    return copy;
}

void imu_print()
{
    const ImuData d = imu_read();
    ESP_LOGI(TAG, "accel %.3f %.3f %.3f g   gyro %.2f %.2f %.2f dps",
             (double)d.ax, (double)d.ay, (double)d.az,
             (double)d.gx, (double)d.gy, (double)d.gz);
}

}  // namespace robot
