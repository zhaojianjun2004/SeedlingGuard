/**
 * @file ble_service.h
 * @brief BLE presentation layer for the environment monitor.
 *
 * The component knows nothing about AHT20 or I2C. Application callbacks provide
 * a snapshot and read or update the sampling period. This keeps the wireless
 * protocol independent from the sensor implementation.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum BLE GAP device-name length accepted by this component. */
#define BLE_SERVICE_MAX_DEVICE_NAME_LENGTH (20U)

/** Snapshot exposed through the BLE GATT database. */
typedef struct {
    float temperature_c;             /**< Temperature in degrees Celsius. */
    float humidity_percent;          /**< Relative humidity in percent. */
    uint32_t sequence_number;        /**< Increments after each valid measurement. */
    uint32_t consecutive_failures;   /**< Consecutive acquisition failures. */
    esp_err_t last_error;            /**< Latest sensor-layer result. */
    bool valid;                      /**< True when temperature and humidity are usable. */
} ble_environment_snapshot_t;

/** Copy the current environment snapshot into out_snapshot. */
typedef esp_err_t (*ble_service_get_snapshot_fn)(void *context,
                                                 ble_environment_snapshot_t *out_snapshot);

/** Read the active sensor sampling period in milliseconds. */
typedef esp_err_t (*ble_service_get_sample_period_fn)(void *context,
                                                      uint32_t *out_period_ms);

/** Validate and apply a new sensor sampling period. */
typedef esp_err_t (*ble_service_set_sample_period_fn)(void *context,
                                                      uint32_t period_ms);

/** Dependencies injected by the application composition root. */
typedef struct {
    const char *device_name;
    void *snapshot_context;
    ble_service_get_snapshot_fn get_snapshot;
    void *config_context;
    ble_service_get_sample_period_fn get_sample_period;
    ble_service_set_sample_period_fn set_sample_period;
} ble_service_config_t;

/** Opaque BLE service handle. ESP-IDF supports one active controller instance. */
typedef struct ble_service *ble_service_handle_t;

/** Allocate and validate a BLE service instance without starting the radio. */
esp_err_t ble_service_create(const ble_service_config_t *config,
                             ble_service_handle_t *out_handle);

/** Initialize NimBLE, register GATT services, and begin connectable advertising. */
esp_err_t ble_service_start(ble_service_handle_t handle);

#ifdef __cplusplus
}
#endif
