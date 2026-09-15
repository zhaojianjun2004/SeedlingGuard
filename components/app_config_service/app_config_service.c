/**
 * @file app_config_service.c
 * @brief 版本化配置、运行时应用和 NVS 持久化实现。
 */

#include "app_config_service.h"

#include <inttypes.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define APP_CONFIG_RECORD_MAGIC        (0x454E5643UL)
#define APP_CONFIG_RECORD_VERSION      (1U)
#define APP_CONFIG_LOCK_TIMEOUT_MS     (200U)

static const char *TAG = "app_config";

/**
 * Flash 中的版本 1 记录。固定宽度字段和 size 便于未来迁移到新结构。
 * NVS 已为 Blob 提供 CRC 校验，此处额外校验业务语义，不重复实现 CRC。
 */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t sample_period_ms;
    uint32_t report_period_ms;
    uint32_t reserved;
} app_config_record_v1_t;

_Static_assert(sizeof(app_config_record_v1_t) == 20U,
               "Unexpected persisted configuration layout");

struct app_config_service {
    app_config_service_config_t config;
    SemaphoreHandle_t mutex;
    app_config_service_status_t status;
};

static TickType_t app_config_ms_to_ticks(uint32_t milliseconds)
{
    const TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    return (ticks > 0U) ? ticks : 1U;
}

static bool app_config_values_are_valid(
    const app_config_service_config_t *limits,
    const app_runtime_config_t *values)
{
    return (values->sample_period_ms >= limits->minimum_sample_period_ms) &&
           (values->sample_period_ms <= limits->maximum_sample_period_ms) &&
           (values->report_period_ms >= limits->minimum_report_period_ms) &&
           (values->report_period_ms <= limits->maximum_report_period_ms);
}

static bool app_config_service_config_is_valid(
    const app_config_service_config_t *config)
{
    return (config != NULL) &&
           (config->minimum_sample_period_ms > 0U) &&
           (config->minimum_sample_period_ms <= config->maximum_sample_period_ms) &&
           (config->minimum_report_period_ms > 0U) &&
           (config->minimum_report_period_ms <= config->maximum_report_period_ms) &&
           (config->store != NULL) &&
           (config->sample_context != NULL) &&
           (config->apply_sample_period != NULL) &&
           app_config_values_are_valid(config, &config->defaults);
}

static app_config_record_v1_t app_config_make_record(
    const app_runtime_config_t *values)
{
    return (app_config_record_v1_t) {
        .magic = APP_CONFIG_RECORD_MAGIC,
        .version = APP_CONFIG_RECORD_VERSION,
        .size = sizeof(app_config_record_v1_t),
        .sample_period_ms = values->sample_period_ms,
        .report_period_ms = values->report_period_ms,
        .reserved = 0U,
    };
}

static bool app_config_record_is_valid(
    const app_config_service_config_t *limits,
    const app_config_record_v1_t *record,
    size_t stored_size)
{
    const app_runtime_config_t values = {
        .sample_period_ms = record->sample_period_ms,
        .report_period_ms = record->report_period_ms,
    };
    return (stored_size == sizeof(*record)) &&
           (record->magic == APP_CONFIG_RECORD_MAGIC) &&
           (record->version == APP_CONFIG_RECORD_VERSION) &&
           (record->size == sizeof(*record)) &&
           app_config_values_are_valid(limits, &values);
}

static esp_err_t app_config_save_locked(
    app_config_service_handle_t handle,
    const app_runtime_config_t *values)
{
    const app_config_record_v1_t record = app_config_make_record(values);
    return config_store_save(handle->config.store, &record, sizeof(record));
}

static void app_config_record_persistence_failure_locked(
    app_config_service_handle_t handle,
    esp_err_t error)
{
    ++handle->status.persistence_failure_count;
    handle->status.last_error = error;
}

esp_err_t app_config_service_create(
    const app_config_service_config_t *config,
    app_config_service_handle_t *out_handle)
{
    if (!app_config_service_config_is_valid(config) || (out_handle == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_handle = NULL;
    app_config_service_handle_t handle = calloc(1U, sizeof(*handle));
    if (handle == NULL) {
        return ESP_ERR_NO_MEM;
    }

    handle->config = *config;
    handle->status.values = config->defaults;
    handle->status.last_error = ESP_OK;
    handle->mutex = xSemaphoreCreateMutex();
    if (handle->mutex == NULL) {
        free(handle);
        return ESP_ERR_NO_MEM;
    }

    *out_handle = handle;
    return ESP_OK;
}

esp_err_t app_config_service_load_and_apply(
    app_config_service_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(handle->mutex,
                       app_config_ms_to_ticks(APP_CONFIG_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (handle->status.initialized) {
        (void)xSemaphoreGive(handle->mutex);
        return ESP_ERR_INVALID_STATE;
    }

    app_config_record_v1_t record = {0};
    size_t stored_size = 0U;
    const esp_err_t load_result = config_store_load(handle->config.store,
                                                    &record,
                                                    sizeof(record),
                                                    &stored_size);
    app_runtime_config_t selected = handle->config.defaults;
    bool loaded_from_nvs = false;
    esp_err_t diagnostic_error = ESP_OK;

    if ((load_result == ESP_OK) &&
        app_config_record_is_valid(&handle->config, &record, stored_size)) {
        selected.sample_period_ms = record.sample_period_ms;
        selected.report_period_ms = record.report_period_ms;
        loaded_from_nvs = true;
    } else if (load_result == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved configuration; using project defaults");
    } else {
        diagnostic_error = (load_result == ESP_OK)
                               ? ESP_ERR_INVALID_VERSION
                               : load_result;
        ESP_LOGW(TAG,
                 "Saved configuration is unavailable or invalid (%s); using defaults",
                 esp_err_to_name(diagnostic_error));
        const esp_err_t erase_result = config_store_erase(handle->config.store);
        if (erase_result != ESP_OK) {
            ESP_LOGW(TAG, "Failed to remove invalid configuration: %s",
                     esp_err_to_name(erase_result));
            diagnostic_error = erase_result;
        }
    }

    const esp_err_t apply_result = handle->config.apply_sample_period(
        handle->config.sample_context,
        selected.sample_period_ms);
    if (apply_result != ESP_OK) {
        handle->status.last_error = apply_result;
        (void)xSemaphoreGive(handle->mutex);
        return apply_result;
    }

    handle->status.values = selected;
    handle->status.initialized = true;
    handle->status.loaded_from_nvs = loaded_from_nvs;
    handle->status.last_error = diagnostic_error;
    ESP_LOGI(TAG,
             "Configuration active: sample=%" PRIu32
             " ms, report=%" PRIu32 " ms, source=%s",
             selected.sample_period_ms,
             selected.report_period_ms,
             loaded_from_nvs ? "nvs" : "defaults");
    (void)xSemaphoreGive(handle->mutex);
    return ESP_OK;
}

esp_err_t app_config_service_get(app_config_service_handle_t handle,
                                 app_runtime_config_t *out_config)
{
    if ((handle == NULL) || (out_config == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(handle->mutex,
                       app_config_ms_to_ticks(APP_CONFIG_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!handle->status.initialized) {
        (void)xSemaphoreGive(handle->mutex);
        return ESP_ERR_INVALID_STATE;
    }

    *out_config = handle->status.values;
    (void)xSemaphoreGive(handle->mutex);
    return ESP_OK;
}

esp_err_t app_config_service_update(
    app_config_service_handle_t handle,
    const app_runtime_config_update_t *update)
{
    if ((handle == NULL) || (update == NULL) ||
        (!update->has_sample_period && !update->has_report_period)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(handle->mutex,
                       app_config_ms_to_ticks(APP_CONFIG_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!handle->status.initialized) {
        (void)xSemaphoreGive(handle->mutex);
        return ESP_ERR_INVALID_STATE;
    }

    const app_runtime_config_t previous = handle->status.values;
    app_runtime_config_t candidate = previous;
    if (update->has_sample_period) {
        candidate.sample_period_ms = update->sample_period_ms;
    }
    if (update->has_report_period) {
        candidate.report_period_ms = update->report_period_ms;
    }
    if (!app_config_values_are_valid(&handle->config, &candidate)) {
        handle->status.last_error = ESP_ERR_INVALID_ARG;
        (void)xSemaphoreGive(handle->mutex);
        return ESP_ERR_INVALID_ARG;
    }

    const bool sample_changed =
        candidate.sample_period_ms != previous.sample_period_ms;
    const bool report_changed =
        candidate.report_period_ms != previous.report_period_ms;
    if (!sample_changed && !report_changed) {
        handle->status.last_error = ESP_OK;
        (void)xSemaphoreGive(handle->mutex);
        return ESP_OK;
    }

    if (sample_changed) {
        const esp_err_t apply_result = handle->config.apply_sample_period(
            handle->config.sample_context,
            candidate.sample_period_ms);
        if (apply_result != ESP_OK) {
            handle->status.last_error = apply_result;
            ESP_LOGW(TAG, "Configuration could not be applied: %s",
                     esp_err_to_name(apply_result));
            (void)xSemaphoreGive(handle->mutex);
            return apply_result;
        }
    }

    const esp_err_t save_result = app_config_save_locked(handle, &candidate);
    if (save_result != ESP_OK) {
        if (sample_changed) {
            const esp_err_t rollback_result = handle->config.apply_sample_period(
                handle->config.sample_context,
                previous.sample_period_ms);
            if (rollback_result != ESP_OK) {
                ESP_LOGE(TAG, "Failed to roll back sample period: %s",
                         esp_err_to_name(rollback_result));
            }
        }
        app_config_record_persistence_failure_locked(handle, save_result);
        ESP_LOGW(TAG, "Configuration update was not committed: %s",
                 esp_err_to_name(save_result));
        (void)xSemaphoreGive(handle->mutex);
        return save_result;
    }

    handle->status.values = candidate;
    handle->status.loaded_from_nvs = true;
    handle->status.last_error = ESP_OK;
    ++handle->status.successful_update_count;
    ESP_LOGI(TAG,
             "Configuration saved: sample=%" PRIu32
             " ms, report=%" PRIu32 " ms",
             candidate.sample_period_ms,
             candidate.report_period_ms);
    (void)xSemaphoreGive(handle->mutex);
    return ESP_OK;
}

esp_err_t app_config_service_reset_defaults(
    app_config_service_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(handle->mutex,
                       app_config_ms_to_ticks(APP_CONFIG_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!handle->status.initialized) {
        (void)xSemaphoreGive(handle->mutex);
        return ESP_ERR_INVALID_STATE;
    }

    const app_runtime_config_t previous = handle->status.values;
    const bool sample_changed = previous.sample_period_ms !=
                                handle->config.defaults.sample_period_ms;
    if (sample_changed) {
        const esp_err_t apply_result = handle->config.apply_sample_period(
            handle->config.sample_context,
            handle->config.defaults.sample_period_ms);
        if (apply_result != ESP_OK) {
            handle->status.last_error = apply_result;
            (void)xSemaphoreGive(handle->mutex);
            return apply_result;
        }
    }

    const esp_err_t erase_result = config_store_erase(handle->config.store);
    if (erase_result != ESP_OK) {
        if (sample_changed) {
            (void)handle->config.apply_sample_period(
                handle->config.sample_context,
                previous.sample_period_ms);
        }
        app_config_record_persistence_failure_locked(handle, erase_result);
        (void)xSemaphoreGive(handle->mutex);
        return erase_result;
    }

    handle->status.values = handle->config.defaults;
    handle->status.loaded_from_nvs = false;
    handle->status.last_error = ESP_OK;
    ++handle->status.successful_update_count;
    ESP_LOGI(TAG, "Configuration reset to project defaults");
    (void)xSemaphoreGive(handle->mutex);
    return ESP_OK;
}

esp_err_t app_config_service_get_status(
    app_config_service_handle_t handle,
    app_config_service_status_t *out_status)
{
    if ((handle == NULL) || (out_status == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(handle->mutex,
                       app_config_ms_to_ticks(APP_CONFIG_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    *out_status = handle->status;
    (void)xSemaphoreGive(handle->mutex);
    return ESP_OK;
}

void app_config_service_delete(app_config_service_handle_t handle)
{
    if (handle == NULL) {
        return;
    }
    if (handle->mutex != NULL) {
        vSemaphoreDelete(handle->mutex);
    }
    free(handle);
}
