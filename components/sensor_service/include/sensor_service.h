/**
 * @file sensor_service.h
 * @brief Periodic environmental sensor acquisition service.
 *
 * This is the business layer above the AHT20 protocol driver. It owns the I2C
 * bus configuration, sampling task, retry policy, and latest data snapshot.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/** AHT20 采样过快会产生自热，因此业务层不允许小于 1 秒 */
#define SENSOR_SERVICE_MIN_SAMPLE_PERIOD_MS (1000U)

/** 限制远程配置的最大，避免错误参数导致设备长时间不更新 */
#define SENSOR_SERVICE_MAX_SAMPLE_PERIOD_MS (600000U)

/** Opaque sensor service handle. */
typedef struct sensor_service *sensor_service_handle_t;

/** I2C and task configuration owned by the sensor service. */
typedef struct {
    i2c_port_t i2c_port;             /**< I2C controller used by the AHT20. */
    gpio_num_t sda_gpio;             /**< SDA GPIO selected by the application. */
    gpio_num_t scl_gpio;             /**< SCL GPIO selected by the application. */
    uint32_t i2c_clock_hz;           /**< I2C bus frequency; 100 kHz is a safe default. */
    bool enable_internal_pullups;    /**< Supplemental internal pull-ups; external ones are preferred. */
    uint32_t sample_period_ms;       /**< Time from one acquisition cycle to the next. */
    uint32_t task_stack_size;        /**< FreeRTOS task stack size in bytes. */
    UBaseType_t task_priority;       /**< FreeRTOS sampling task priority. */
} sensor_service_config_t;

/** Thread-safe snapshot consumed later by BLE and MQTT modules. */
typedef struct {
    float temperature_c;             /**< Last decoded temperature. */
    float humidity_percent;          /**< Last decoded relative humidity. */
    int64_t last_attempt_at_us;      /**< Time of the latest sampling attempt. */
    int64_t last_success_at_us;      /**< Time of the latest successful sample. */
    uint32_t sequence_number;        /**< Incremented for every successful sample. */
    uint32_t consecutive_failures;   /**< Reset to zero after a successful sample. */
    esp_err_t last_error;            /**< ESP_OK for a valid sample, otherwise latest failure. */
    bool valid;                      /**< True only when this snapshot contains valid data. */
} sensor_reading_t;

/**
 * @brief Create the service and install its I2C master bus.
 *
 * Sensor communication starts only after sensor_service_start().
 */
esp_err_t sensor_service_create(const sensor_service_config_t *config,
                                sensor_service_handle_t *out_handle);

/** Start the periodic acquisition task. */
esp_err_t sensor_service_start(sensor_service_handle_t handle);

/**
 * @brief Copy the latest sensor snapshot.
 *
 * The call is thread-safe and never exposes a partially updated structure.
 * Check snapshot.valid before using the temperature and humidity values.
 */
esp_err_t sensor_service_get_latest(sensor_service_handle_t handle,
                                    sensor_reading_t *out_snapshot);

/** Thread-safely read the active sample period. */
esp_err_t sensor_service_get_sample_period(sensor_service_handle_t handle,
                                           uint32_t *out_period_ms);

/**
 * @brief Update the sample period at runtime.
 *
 * The value is validated against SENSOR_SERVICE_MIN_SAMPLE_PERIOD_MS and
 * SENSOR_SERVICE_MAX_SAMPLE_PERIOD_MS. The sampling task is woken immediately
 * so the new period takes effect without waiting for the previous delay.
 */
esp_err_t sensor_service_set_sample_period(sensor_service_handle_t handle,
                                           uint32_t period_ms);

/** Stop the acquisition task, waiting up to timeout_ms for clean shutdown. */
esp_err_t sensor_service_stop(sensor_service_handle_t handle, uint32_t timeout_ms);

/** Stop and destroy the service, then uninstall its I2C driver. */
esp_err_t sensor_service_delete(sensor_service_handle_t handle);

#ifdef __cplusplus
}
#endif
