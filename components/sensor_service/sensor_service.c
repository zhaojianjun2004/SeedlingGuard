/**
 * @file sensor_service.c
 * @brief Business logic for periodic AHT20 acquisition and recovery.
 */

#include "sensor_service.h"

#include <inttypes.h>
#include <stdlib.h>

#include "aht20.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define SENSOR_SERVICE_STOP_REQUESTED_BIT    BIT0
#define SENSOR_SERVICE_STOPPED_BIT           BIT1

#define SENSOR_SERVICE_AHT20_IO_TIMEOUT_MS   (100U)
#define SENSOR_SERVICE_RECOVERY_THRESHOLD    (3U)
#define SENSOR_SERVICE_MIN_STACK_SIZE        (3072U)

static const char *TAG = "sensor_service";

struct sensor_service {
    sensor_service_config_t config;
    aht20_handle_t sensor;
    SemaphoreHandle_t snapshot_mutex;
    SemaphoreHandle_t config_mutex;
    EventGroupHandle_t control_events;
    TaskHandle_t task_handle;
    sensor_reading_t latest;
    bool i2c_driver_installed;
};

static TickType_t sensor_service_ms_to_ticks(uint32_t milliseconds)
{
    const TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    return (ticks > 0U) ? ticks : 1U;
}

static bool sensor_service_config_is_valid(const sensor_service_config_t *config)
{
    return (config != NULL) &&
           (config->i2c_port >= I2C_NUM_0) &&
           (config->i2c_port < I2C_NUM_MAX) &&
           GPIO_IS_VALID_OUTPUT_GPIO(config->sda_gpio) &&
           GPIO_IS_VALID_OUTPUT_GPIO(config->scl_gpio) &&
           (config->sda_gpio != config->scl_gpio) &&
           (config->i2c_clock_hz > 0U) &&
           (config->i2c_clock_hz <= 400000U) &&
           (config->sample_period_ms >= SENSOR_SERVICE_MIN_SAMPLE_PERIOD_MS) &&
           (config->sample_period_ms <= SENSOR_SERVICE_MAX_SAMPLE_PERIOD_MS) &&
           (config->task_stack_size >= SENSOR_SERVICE_MIN_STACK_SIZE) &&
           (config->task_priority < configMAX_PRIORITIES);
}

static uint32_t sensor_service_read_sample_period(sensor_service_handle_t handle)
{
    (void)xSemaphoreTake(handle->config_mutex, portMAX_DELAY);
    const uint32_t period_ms = handle->config.sample_period_ms;
    (void)xSemaphoreGive(handle->config_mutex);
    return period_ms;
}

static esp_err_t sensor_service_install_i2c_driver(sensor_service_handle_t handle)
{
    const i2c_config_t i2c_config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = handle->config.sda_gpio,
        .scl_io_num = handle->config.scl_gpio,
        .sda_pullup_en = handle->config.enable_internal_pullups,
        .scl_pullup_en = handle->config.enable_internal_pullups,
        .master.clk_speed = handle->config.i2c_clock_hz,
        .clk_flags = 0U,
    };

    esp_err_t result = i2c_param_config(handle->config.i2c_port, &i2c_config);
    if (result != ESP_OK) {
        return result;
    }

    result = i2c_driver_install(handle->config.i2c_port,
                                I2C_MODE_MASTER,
                                0U,
                                0U,
                                0);
    if (result == ESP_OK) {
        handle->i2c_driver_installed = true;
    }
    return result;
}

static void sensor_service_record_success(sensor_service_handle_t handle,
                                          const aht20_measurement_t *measurement)
{
    (void)xSemaphoreTake(handle->snapshot_mutex, portMAX_DELAY);
    handle->latest.temperature_c = measurement->temperature_c;
    handle->latest.humidity_percent = measurement->humidity_percent;
    const int64_t now_us = esp_timer_get_time();
    handle->latest.last_attempt_at_us = now_us;
    handle->latest.last_success_at_us = now_us;
    ++handle->latest.sequence_number;
    handle->latest.consecutive_failures = 0U;
    handle->latest.last_error = ESP_OK;
    handle->latest.valid = true;
    (void)xSemaphoreGive(handle->snapshot_mutex);
}

static uint32_t sensor_service_record_failure(sensor_service_handle_t handle, esp_err_t error)
{
    (void)xSemaphoreTake(handle->snapshot_mutex, portMAX_DELAY);
    handle->latest.last_attempt_at_us = esp_timer_get_time();
    ++handle->latest.consecutive_failures;
    handle->latest.last_error = error;
    handle->latest.valid = false;
    const uint32_t failure_count = handle->latest.consecutive_failures;
    (void)xSemaphoreGive(handle->snapshot_mutex);

    return failure_count;
}

/** Sampling task: initialize, acquire, publish snapshot, and recover. */
static void sensor_service_task(void *context)
{
    sensor_service_handle_t handle = (sensor_service_handle_t)context;
    bool sensor_initialized = false;

    ESP_LOGI(TAG,
             "Sampling task started: SDA=%d, SCL=%d, I2C=%" PRIu32 " Hz, period=%" PRIu32 " ms",
             (int)handle->config.sda_gpio,
             (int)handle->config.scl_gpio,
             handle->config.i2c_clock_hz,
             sensor_service_read_sample_period(handle));

    while ((xEventGroupGetBits(handle->control_events) & SENSOR_SERVICE_STOP_REQUESTED_BIT) == 0U) {
        const TickType_t cycle_started_at = xTaskGetTickCount();
        esp_err_t result = ESP_OK;

        if (!sensor_initialized) {
            result = aht20_initialize(handle->sensor);
            if (result == ESP_OK) {
                sensor_initialized = true;
                ESP_LOGI(TAG, "AHT20 initialized successfully at I2C address 0x%02X",
                         AHT20_DEFAULT_I2C_ADDRESS);
            }
        }

        aht20_measurement_t measurement = {0};
        if (result == ESP_OK) {
            result = aht20_read(handle->sensor, &measurement);
        }

        if (result == ESP_OK) {
            sensor_service_record_success(handle, &measurement);
            /* 周期性成功数据使用 DEBUG，避免持续覆盖串口交互提示符。 */
            ESP_LOGD(TAG,
                     "Temperature: %.2f C, humidity: %.2f %%RH",
                     (double)measurement.temperature_c,
                     (double)measurement.humidity_percent);
        } else {
            const uint32_t failure_count = sensor_service_record_failure(handle, result);
            ESP_LOGW(TAG,
                     "AHT20 acquisition failed: %s (consecutive failures: %" PRIu32 ")",
                     esp_err_to_name(result),
                     failure_count);

            /* Reinitialize after repeated failures. A successful soft reset
             * is helpful for a stuck sensor; a failed reset is harmless when
             * the sensor is physically disconnected. */
            if ((failure_count % SENSOR_SERVICE_RECOVERY_THRESHOLD) == 0U) {
                const esp_err_t reset_result = aht20_soft_reset(handle->sensor);
                if (reset_result == ESP_OK) {
                    ESP_LOGW(TAG, "AHT20 software reset completed; initialization will be retried");
                } else {
                    ESP_LOGW(TAG, "AHT20 software reset failed: %s", esp_err_to_name(reset_result));
                }
                sensor_initialized = false;
            }
        }

        /* Keep a stable cycle period while allowing sensor_service_stop() to
         * wake the task immediately through a task notification. */
        const TickType_t elapsed_ticks = xTaskGetTickCount() - cycle_started_at;
        const uint32_t period_ms = sensor_service_read_sample_period(handle);
        const TickType_t period_ticks = sensor_service_ms_to_ticks(period_ms);
        const TickType_t wait_ticks = (elapsed_ticks < period_ticks)
                                          ? (period_ticks - elapsed_ticks)
                                          : 1U;
        (void)ulTaskNotifyTake(pdTRUE, wait_ticks);
    }

    ESP_LOGI(TAG, "Sampling task stopped");
    xEventGroupSetBits(handle->control_events, SENSOR_SERVICE_STOPPED_BIT);
    vTaskDelete(NULL);
}

esp_err_t sensor_service_create(const sensor_service_config_t *config,
                                sensor_service_handle_t *out_handle)
{
    if (!sensor_service_config_is_valid(config) || (out_handle == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_handle = NULL;

    sensor_service_handle_t handle = calloc(1U, sizeof(*handle));
    if (handle == NULL) {
        return ESP_ERR_NO_MEM;
    }
    handle->config = *config;
    handle->latest.last_error = ESP_ERR_INVALID_STATE;

    handle->snapshot_mutex = xSemaphoreCreateMutex();
    handle->config_mutex = xSemaphoreCreateMutex();
    handle->control_events = xEventGroupCreate();
    if ((handle->snapshot_mutex == NULL) ||
        (handle->config_mutex == NULL) ||
        (handle->control_events == NULL)) {
        (void)sensor_service_delete(handle);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = sensor_service_install_i2c_driver(handle);
    if (result != ESP_OK) {
        (void)sensor_service_delete(handle);
        return result;
    }

    const aht20_config_t aht20_config = {
        .i2c_port = config->i2c_port,
        .i2c_address = AHT20_DEFAULT_I2C_ADDRESS,
        .io_timeout_ms = SENSOR_SERVICE_AHT20_IO_TIMEOUT_MS,
    };
    result = aht20_create(&aht20_config, &handle->sensor);
    if (result != ESP_OK) {
        (void)sensor_service_delete(handle);
        return result;
    }

    *out_handle = handle;
    return ESP_OK;
}

esp_err_t sensor_service_start(sensor_service_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xEventGroupClearBits(handle->control_events,
                         SENSOR_SERVICE_STOP_REQUESTED_BIT | SENSOR_SERVICE_STOPPED_BIT);

    const BaseType_t task_created = xTaskCreate(sensor_service_task,
                                                "sensor_service",
                                                handle->config.task_stack_size,
                                                handle,
                                                handle->config.task_priority,
                                                &handle->task_handle);
    if (task_created != pdPASS) {
        handle->task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t sensor_service_get_latest(sensor_service_handle_t handle,
                                    sensor_reading_t *out_snapshot)
{
    if ((handle == NULL) || (out_snapshot == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(handle->snapshot_mutex, sensor_service_ms_to_ticks(100U)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *out_snapshot = handle->latest;
    (void)xSemaphoreGive(handle->snapshot_mutex);

    return ESP_OK;
}

esp_err_t sensor_service_get_sample_period(sensor_service_handle_t handle,
                                           uint32_t *out_period_ms)
{
    if ((handle == NULL) || (out_period_ms == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(handle->config_mutex, sensor_service_ms_to_ticks(100U)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *out_period_ms = handle->config.sample_period_ms;
    (void)xSemaphoreGive(handle->config_mutex);
    return ESP_OK;
}

esp_err_t sensor_service_set_sample_period(sensor_service_handle_t handle,
                                           uint32_t period_ms)
{
    if ((handle == NULL) ||
        (period_ms < SENSOR_SERVICE_MIN_SAMPLE_PERIOD_MS) ||
        (period_ms > SENSOR_SERVICE_MAX_SAMPLE_PERIOD_MS)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(handle->config_mutex, sensor_service_ms_to_ticks(100U)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const bool changed = handle->config.sample_period_ms != period_ms;
    handle->config.sample_period_ms = period_ms;
    (void)xSemaphoreGive(handle->config_mutex);

    if (changed) {
        ESP_LOGI(TAG, "Sample period updated to %" PRIu32 " ms", period_ms);

        /* The same notification channel is used by stop() and configuration
         * updates. The control event bit still determines whether to stop. */
        if (handle->task_handle != NULL) {
            xTaskNotifyGive(handle->task_handle);
        }
    }
    return ESP_OK;
}

esp_err_t sensor_service_stop(sensor_service_handle_t handle, uint32_t timeout_ms)
{
    if ((handle == NULL) || (timeout_ms == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->task_handle == NULL) {
        return ESP_OK;
    }

    xEventGroupSetBits(handle->control_events, SENSOR_SERVICE_STOP_REQUESTED_BIT);
    xTaskNotifyGive(handle->task_handle);

    const EventBits_t bits = xEventGroupWaitBits(handle->control_events,
                                                  SENSOR_SERVICE_STOPPED_BIT,
                                                  pdFALSE,
                                                  pdTRUE,
                                                  sensor_service_ms_to_ticks(timeout_ms));
    if ((bits & SENSOR_SERVICE_STOPPED_BIT) == 0U) {
        return ESP_ERR_TIMEOUT;
    }

    handle->task_handle = NULL;
    return ESP_OK;
}

esp_err_t sensor_service_delete(sensor_service_handle_t handle)
{
    if (handle == NULL) {
        return ESP_OK;
    }

    if (handle->task_handle != NULL) {
        const esp_err_t stop_result = sensor_service_stop(handle, 1000U);
        if (stop_result != ESP_OK) {
            return stop_result;
        }
    }

    aht20_delete(handle->sensor);

    /* Release every owned resource even if the I2C uninstall reports an
     * error. Returning the error still gives the caller diagnostics without
     * leaking the service object and synchronization primitives. */
    esp_err_t result = ESP_OK;
    if (handle->i2c_driver_installed) {
        result = i2c_driver_delete(handle->config.i2c_port);
    }

    if (handle->control_events != NULL) {
        vEventGroupDelete(handle->control_events);
    }
    if (handle->snapshot_mutex != NULL) {
        vSemaphoreDelete(handle->snapshot_mutex);
    }
    if (handle->config_mutex != NULL) {
        vSemaphoreDelete(handle->config_mutex);
    }
    free(handle);

    return result;
}
