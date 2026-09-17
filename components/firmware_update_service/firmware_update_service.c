/**
 * @file firmware_update_service.c
 * @brief 固件升级事务、镜像前置检查、进度和错误收敛实现。
 */

#include "firmware_update_service.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define FIRMWARE_UPDATE_MIN_BUFFER_SIZE     (512U)
#define FIRMWARE_UPDATE_MIN_TASK_STACK_SIZE (4096U)
#define FIRMWARE_UPDATE_LOCK_TIMEOUT_MS     (100U)

static const char *TAG = "fw_update";

typedef struct {
    char manifest_uri[FIRMWARE_UPDATE_URI_MAX_LENGTH + 1U];
} firmware_update_request_t;

struct firmware_update_service {
    firmware_update_service_config_t config;
    SemaphoreHandle_t status_mutex;
    QueueHandle_t request_queue;
    TaskHandle_t task_handle;
    firmware_update_status_t status;
    bool started;
};

static TickType_t firmware_update_ms_to_ticks(uint32_t milliseconds)
{
    const TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    return (ticks > 0U) ? ticks : 1U;
}

static bool firmware_update_string_fits(const char *value, size_t capacity)
{
    return (value != NULL) && (strnlen(value, capacity) < capacity);
}

static bool firmware_update_config_is_valid(
    const firmware_update_service_config_t *config)
{
    return (config != NULL) &&
           (config->transport.context != NULL) &&
           (config->transport.open != NULL) &&
           (config->transport.read != NULL) &&
           (config->transport.close != NULL) &&
           (config->manifest_parser.context != NULL) &&
           (config->manifest_parser.maximum_document_size > 0U) &&
           (config->manifest_parser.parse != NULL) &&
           (config->integrity.context != NULL) &&
           (config->integrity.begin != NULL) &&
           (config->integrity.update != NULL) &&
           (config->integrity.finish != NULL) &&
           (config->integrity.abort != NULL) &&
           (config->platform.context != NULL) &&
           (config->platform.minimum_header_size > 0U) &&
           (config->platform.inspect_header != NULL) &&
           (config->platform.begin != NULL) &&
           (config->platform.write != NULL) &&
           (config->platform.finish != NULL) &&
           (config->platform.activate != NULL) &&
           (config->platform.abort != NULL) &&
           firmware_update_string_fits(config->expected_project_name,
                                       FIRMWARE_UPDATE_NAME_MAX_LENGTH + 1U) &&
           firmware_update_string_fits(config->expected_hardware_id,
                                       FIRMWARE_UPDATE_HARDWARE_MAX_LENGTH + 1U) &&
           firmware_update_string_fits(config->current_version,
                                       FIRMWARE_UPDATE_NAME_MAX_LENGTH + 1U) &&
           (config->current_release_sequence > 0U) &&
           (config->is_network_ready != NULL) &&
           (config->restart != NULL) &&
           (config->download_buffer_size >= FIRMWARE_UPDATE_MIN_BUFFER_SIZE) &&
           (config->download_buffer_size >=
            config->platform.minimum_header_size) &&
           (config->task_stack_size >= FIRMWARE_UPDATE_MIN_TASK_STACK_SIZE) &&
           (config->task_priority < configMAX_PRIORITIES);
}

static void firmware_update_set_status(
    firmware_update_service_handle_t handle,
    firmware_update_state_t state,
    esp_err_t error)
{
    if (xSemaphoreTake(handle->status_mutex,
                       firmware_update_ms_to_ticks(
                           FIRMWARE_UPDATE_LOCK_TIMEOUT_MS)) == pdTRUE) {
        handle->status.state = state;
        handle->status.last_error = error;
        (void)xSemaphoreGive(handle->status_mutex);
    }
}

static void firmware_update_reset_progress(
    firmware_update_service_handle_t handle,
    size_t total_bytes)
{
    if (xSemaphoreTake(handle->status_mutex,
                       firmware_update_ms_to_ticks(
                           FIRMWARE_UPDATE_LOCK_TIMEOUT_MS)) == pdTRUE) {
        handle->status.last_error = ESP_OK;
        handle->status.bytes_received = 0U;
        handle->status.total_bytes = total_bytes;
        handle->status.progress_percent = 0U;
        handle->status.release_sequence = 0U;
        handle->status.image_version[0] = '\0';
        handle->status.target_partition[0] = '\0';
        (void)xSemaphoreGive(handle->status_mutex);
    }
}

static void firmware_update_record_image(
    firmware_update_service_handle_t handle,
    const firmware_update_manifest_t *manifest,
    const char *target_partition)
{
    if (xSemaphoreTake(handle->status_mutex,
                       firmware_update_ms_to_ticks(
                           FIRMWARE_UPDATE_LOCK_TIMEOUT_MS)) == pdTRUE) {
        (void)strncpy(handle->status.image_version,
                      manifest->version,
                      sizeof(handle->status.image_version) - 1U);
        handle->status.image_version[
            sizeof(handle->status.image_version) - 1U] = '\0';
        (void)strncpy(handle->status.target_partition,
                      target_partition,
                      sizeof(handle->status.target_partition) - 1U);
        handle->status.target_partition[
            sizeof(handle->status.target_partition) - 1U] = '\0';
        handle->status.release_sequence = manifest->release_sequence;
        (void)xSemaphoreGive(handle->status_mutex);
    }
}

static void firmware_update_record_progress(
    firmware_update_service_handle_t handle,
    size_t bytes_received,
    size_t total_bytes)
{
    uint8_t percent = 0U;
    if (total_bytes > 0U) {
        const size_t calculated = (bytes_received * 100U) / total_bytes;
        percent = (uint8_t)((calculated > 100U) ? 100U : calculated);
    }

    bool should_log = false;
    if (xSemaphoreTake(handle->status_mutex,
                       firmware_update_ms_to_ticks(
                           FIRMWARE_UPDATE_LOCK_TIMEOUT_MS)) == pdTRUE) {
        should_log = (percent >= (uint8_t)(handle->status.progress_percent + 10U)) ||
                     (bytes_received == total_bytes);
        handle->status.bytes_received = bytes_received;
        handle->status.progress_percent = percent;
        (void)xSemaphoreGive(handle->status_mutex);
    }

    if (should_log) {
        ESP_LOGI(TAG, "Download progress: %u%% (%u/%u bytes)",
                 (unsigned int)percent,
                 (unsigned int)bytes_received,
                 (unsigned int)total_bytes);
    }
}

static esp_err_t firmware_update_validate_manifest(
    firmware_update_service_handle_t handle,
    const firmware_update_manifest_t *manifest)
{
    if (strcmp(manifest->product,
               handle->config.expected_project_name) != 0) {
        ESP_LOGE(TAG, "Manifest product mismatch: expected=%s, received=%s",
                 handle->config.expected_project_name,
                 manifest->product);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (strcmp(manifest->hardware,
               handle->config.expected_hardware_id) != 0) {
        ESP_LOGE(TAG, "Manifest hardware mismatch: expected=%s, received=%s",
                 handle->config.expected_hardware_id,
                 manifest->hardware);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!handle->config.allow_same_version &&
        (strcmp(manifest->version, handle->config.current_version) == 0)) {
        ESP_LOGW(TAG, "Manifest version %s is already running",
                 manifest->version);
        return ESP_ERR_INVALID_STATE;
    }
    if (!handle->config.allow_downgrade &&
        (manifest->release_sequence <=
         handle->config.current_release_sequence)) {
        ESP_LOGE(TAG,
                 "Release sequence rejected: current=%u, requested=%u",
                 (unsigned int)handle->config.current_release_sequence,
                 (unsigned int)manifest->release_sequence);
        return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}

static esp_err_t firmware_update_validate_image(
    firmware_update_service_handle_t handle,
    const uint8_t *header,
    size_t header_length,
    const firmware_update_manifest_t *manifest,
    firmware_update_image_info_t *out_info)
{
    esp_err_t result = handle->config.platform.inspect_header(
        handle->config.platform.context,
        header,
        header_length,
        out_info);
    if (result != ESP_OK) {
        return result;
    }

    if (strcmp(out_info->project_name,
               handle->config.expected_project_name) != 0) {
        ESP_LOGE(TAG, "Image product mismatch: expected=%s, received=%s",
                 handle->config.expected_project_name,
                 out_info->project_name);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if ((strcmp(out_info->project_name, manifest->product) != 0) ||
        (strcmp(out_info->version, manifest->version) != 0)) {
        ESP_LOGE(TAG,
                 "Image header does not match Manifest: image=%s/%s, manifest=%s/%s",
                 out_info->project_name,
                 out_info->version,
                 manifest->product,
                 manifest->version);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (!handle->config.allow_same_version &&
        (strcmp(out_info->version, handle->config.current_version) == 0)) {
        ESP_LOGW(TAG, "Image version %s is already running", out_info->version);
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

static esp_err_t firmware_update_fetch_manifest(
    firmware_update_service_handle_t handle,
    const char *manifest_uri,
    firmware_update_manifest_t *out_manifest)
{
    esp_err_t result = ESP_FAIL;
    uint8_t *document = NULL;
    bool transport_open = false;
    size_t document_length = 0U;

    firmware_update_set_status(handle,
                               FIRMWARE_UPDATE_STATE_FETCHING_MANIFEST,
                               ESP_OK);
    result = handle->config.transport.open(handle->config.transport.context,
                                           manifest_uri,
                                           &document_length);
    if (result != ESP_OK) {
        goto cleanup;
    }
    transport_open = true;
    if ((document_length == 0U) ||
        (document_length >
         handle->config.manifest_parser.maximum_document_size)) {
        ESP_LOGE(TAG, "Manifest size is invalid: %u bytes",
                 (unsigned int)document_length);
        result = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    document = calloc(document_length + 1U, sizeof(uint8_t));
    if (document == NULL) {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    size_t total_received = 0U;
    while (total_received < document_length) {
        size_t received = 0U;
        bool complete = false;
        result = handle->config.transport.read(
            handle->config.transport.context,
            &document[total_received],
            document_length - total_received,
            &received,
            &complete);
        if (result != ESP_OK) {
            goto cleanup;
        }
        if (received > (document_length - total_received)) {
            result = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }
        total_received += received;
        if (complete && (total_received != document_length)) {
            result = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }
        if ((received == 0U) && !complete) {
            result = ESP_ERR_INVALID_RESPONSE;
            goto cleanup;
        }
    }

    handle->config.transport.close(handle->config.transport.context);
    transport_open = false;
    firmware_update_set_status(handle,
                               FIRMWARE_UPDATE_STATE_CHECKING_MANIFEST,
                               ESP_OK);
    result = handle->config.manifest_parser.parse(
        handle->config.manifest_parser.context,
        document,
        document_length,
        out_manifest);
    if (result != ESP_OK) {
        goto cleanup;
    }
    result = firmware_update_validate_manifest(handle, out_manifest);
    if (result == ESP_OK) {
        ESP_LOGI(TAG,
                 "Accepted Manifest: product=%s, hardware=%s, version=%s, sequence=%u",
                 out_manifest->product,
                 out_manifest->hardware,
                 out_manifest->version,
                 (unsigned int)out_manifest->release_sequence);
    }

cleanup:
    if (transport_open) {
        handle->config.transport.close(handle->config.transport.context);
    }
    free(document);
    return result;
}

static esp_err_t firmware_update_read_header(
    firmware_update_service_handle_t handle,
    uint8_t *buffer,
    size_t *out_length)
{
    size_t accumulated = 0U;
    const size_t required = handle->config.platform.minimum_header_size;

    while (accumulated < required) {
        size_t received = 0U;
        bool complete = false;
        const esp_err_t result = handle->config.transport.read(
            handle->config.transport.context,
            &buffer[accumulated],
            handle->config.download_buffer_size - accumulated,
            &received,
            &complete);
        if (result != ESP_OK) {
            return result;
        }
        accumulated += received;
        if (complete && (accumulated < required)) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (received == 0U) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    *out_length = accumulated;
    return ESP_OK;
}

static esp_err_t firmware_update_execute(
    firmware_update_service_handle_t handle,
    const firmware_update_request_t *request)
{
    esp_err_t result = ESP_FAIL;
    bool transport_open = false;
    bool platform_open = false;
    bool integrity_open = false;
    uint8_t *buffer = NULL;
    size_t content_length = 0U;
    size_t total_received = 0U;
    firmware_update_manifest_t manifest = {0};

    if (!handle->config.is_network_ready(handle->config.network_context)) {
        ESP_LOGW(TAG, "Firmware update rejected: network is not ready");
        return ESP_ERR_INVALID_STATE;
    }

    result = firmware_update_fetch_manifest(handle,
                                            request->manifest_uri,
                                            &manifest);
    if (result != ESP_OK) {
        goto cleanup;
    }

    firmware_update_set_status(handle,
                               FIRMWARE_UPDATE_STATE_OPENING_TRANSPORT,
                               ESP_OK);
    result = handle->config.transport.open(handle->config.transport.context,
                                           manifest.image_uri,
                                           &content_length);
    if (result != ESP_OK) {
        goto cleanup;
    }
    transport_open = true;
    if ((content_length == 0U) ||
        (content_length != manifest.image_size)) {
        ESP_LOGE(TAG,
                 "Firmware length does not match Manifest: http=%u, manifest=%u",
                 (unsigned int)content_length,
                 (unsigned int)manifest.image_size);
        result = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    firmware_update_reset_progress(handle, content_length);

    buffer = malloc(handle->config.download_buffer_size);
    if (buffer == NULL) {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    firmware_update_set_status(handle,
                               FIRMWARE_UPDATE_STATE_CHECKING_IMAGE,
                               ESP_OK);
    size_t initial_length = 0U;
    result = firmware_update_read_header(handle, buffer, &initial_length);
    if (result != ESP_OK) {
        goto cleanup;
    }

    firmware_update_image_info_t image_info = {0};
    result = firmware_update_validate_image(handle,
                                            buffer,
                                            initial_length,
                                            &manifest,
                                            &image_info);
    if (result != ESP_OK) {
        goto cleanup;
    }

    char target_partition[FIRMWARE_UPDATE_PARTITION_MAX_LENGTH + 1U] = {0};
    result = handle->config.platform.begin(handle->config.platform.context,
                                           manifest.image_size,
                                           target_partition,
                                           sizeof(target_partition));
    if (result != ESP_OK) {
        goto cleanup;
    }
    platform_open = true;
    result = handle->config.integrity.begin(
        handle->config.integrity.context,
        manifest.sha256,
        sizeof(manifest.sha256));
    if (result != ESP_OK) {
        goto cleanup;
    }
    integrity_open = true;
    firmware_update_record_image(handle, &manifest, target_partition);
    ESP_LOGI(TAG,
             "Accepted firmware: project=%s, version=%s, sequence=%u, target=%s",
             image_info.project_name,
             image_info.version,
             (unsigned int)manifest.release_sequence,
             target_partition);

    firmware_update_set_status(handle,
                               FIRMWARE_UPDATE_STATE_WRITING,
                               ESP_OK);
    result = handle->config.platform.write(handle->config.platform.context,
                                           buffer,
                                           initial_length);
    if (result != ESP_OK) {
        goto cleanup;
    }
    result = handle->config.integrity.update(handle->config.integrity.context,
                                             buffer,
                                             initial_length);
    if (result != ESP_OK) {
        goto cleanup;
    }
    total_received = initial_length;
    firmware_update_record_progress(handle,
                                    total_received,
                                    content_length);

    while (total_received < content_length) {
        size_t received = 0U;
        bool complete = false;
        result = handle->config.transport.read(
            handle->config.transport.context,
            buffer,
            handle->config.download_buffer_size,
            &received,
            &complete);
        if (result != ESP_OK) {
            goto cleanup;
        }
        if (received > (content_length - total_received)) {
            result = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }
        if (received > 0U) {
            result = handle->config.platform.write(
                handle->config.platform.context,
                buffer,
                received);
            if (result != ESP_OK) {
                goto cleanup;
            }
            result = handle->config.integrity.update(
                handle->config.integrity.context,
                buffer,
                received);
            if (result != ESP_OK) {
                goto cleanup;
            }
            total_received += received;
            firmware_update_record_progress(handle,
                                            total_received,
                                            content_length);
        }
        if (complete && (total_received != content_length)) {
            result = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }
        if ((received == 0U) && !complete) {
            result = ESP_ERR_INVALID_RESPONSE;
            goto cleanup;
        }
    }

    firmware_update_set_status(handle,
                               FIRMWARE_UPDATE_STATE_VALIDATING,
                               ESP_OK);
    result = handle->config.integrity.finish(
        handle->config.integrity.context);
    integrity_open = false;
    if (result != ESP_OK) {
        goto cleanup;
    }

    result = handle->config.platform.finish(handle->config.platform.context);
    platform_open = false;
    if (result != ESP_OK) {
        goto cleanup;
    }

    firmware_update_set_status(handle,
                               FIRMWARE_UPDATE_STATE_ACTIVATING,
                               ESP_OK);
    result = handle->config.platform.activate(handle->config.platform.context);
    if (result != ESP_OK) {
        goto cleanup;
    }

cleanup:
    if (integrity_open) {
        handle->config.integrity.abort(handle->config.integrity.context);
    }
    if (platform_open) {
        handle->config.platform.abort(handle->config.platform.context);
    }
    if (transport_open) {
        handle->config.transport.close(handle->config.transport.context);
    }
    free(buffer);
    return result;
}

static void firmware_update_task(void *context)
{
    firmware_update_service_handle_t handle =
        (firmware_update_service_handle_t)context;
    firmware_update_request_t request;

    firmware_update_set_status(handle, FIRMWARE_UPDATE_STATE_IDLE, ESP_OK);
    while (true) {
        if (xQueueReceive(handle->request_queue,
                          &request,
                          portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(TAG, "Firmware update Manifest requested: %s",
                 request.manifest_uri);
        const esp_err_t result = firmware_update_execute(handle, &request);
        if (result != ESP_OK) {
            firmware_update_set_status(handle,
                                       FIRMWARE_UPDATE_STATE_FAILED,
                                       result);
            ESP_LOGE(TAG, "Firmware update failed: %s",
                     esp_err_to_name(result));
            continue;
        }

        firmware_update_set_status(handle,
                                   FIRMWARE_UPDATE_STATE_REBOOT_PENDING,
                                   ESP_OK);
        ESP_LOGI(TAG, "Firmware activated; rebooting in %u ms",
                 (unsigned int)handle->config.restart_delay_ms);
        vTaskDelay(firmware_update_ms_to_ticks(
            handle->config.restart_delay_ms));
        handle->config.restart(handle->config.restart_context);

        /* restart 回调在正常平台上不会返回；返回则保留明确错误。 */
        firmware_update_set_status(handle,
                                   FIRMWARE_UPDATE_STATE_FAILED,
                                   ESP_ERR_INVALID_STATE);
    }
}

esp_err_t firmware_update_service_create(
    const firmware_update_service_config_t *config,
    firmware_update_service_handle_t *out_handle)
{
    if (!firmware_update_config_is_valid(config) || (out_handle == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_handle = NULL;
    firmware_update_service_handle_t handle = calloc(1U, sizeof(*handle));
    if (handle == NULL) {
        return ESP_ERR_NO_MEM;
    }

    handle->config = *config;
    handle->status.state = FIRMWARE_UPDATE_STATE_CREATED;
    handle->status.last_error = ESP_OK;
    handle->status_mutex = xSemaphoreCreateMutex();
    handle->request_queue = xQueueCreate(1U, sizeof(firmware_update_request_t));
    if ((handle->status_mutex == NULL) || (handle->request_queue == NULL)) {
        if (handle->request_queue != NULL) {
            vQueueDelete(handle->request_queue);
        }
        if (handle->status_mutex != NULL) {
            vSemaphoreDelete(handle->status_mutex);
        }
        free(handle);
        return ESP_ERR_NO_MEM;
    }

    *out_handle = handle;
    return ESP_OK;
}

esp_err_t firmware_update_service_start(
    firmware_update_service_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->started) {
        return ESP_ERR_INVALID_STATE;
    }

    const BaseType_t created = xTaskCreate(firmware_update_task,
                                           "fw_update",
                                           handle->config.task_stack_size,
                                           handle,
                                           handle->config.task_priority,
                                           &handle->task_handle);
    if (created != pdPASS) {
        handle->task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    handle->started = true;
    return ESP_OK;
}

esp_err_t firmware_update_service_request(
    firmware_update_service_handle_t handle,
    const char *manifest_uri)
{
    if ((handle == NULL) ||
        !firmware_update_string_fits(manifest_uri,
                                    FIRMWARE_UPDATE_URI_MAX_LENGTH + 1U) ||
        (manifest_uri[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->started) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(handle->status_mutex,
                       firmware_update_ms_to_ticks(
                           FIRMWARE_UPDATE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const bool accepts_request =
        (handle->status.state == FIRMWARE_UPDATE_STATE_IDLE) ||
        (handle->status.state == FIRMWARE_UPDATE_STATE_FAILED);
    if (accepts_request) {
        handle->status.state = FIRMWARE_UPDATE_STATE_QUEUED;
        handle->status.last_error = ESP_OK;
    }
    (void)xSemaphoreGive(handle->status_mutex);
    if (!accepts_request) {
        return ESP_ERR_INVALID_STATE;
    }

    firmware_update_request_t request = {0};
    (void)strncpy(request.manifest_uri,
                  manifest_uri,
                  sizeof(request.manifest_uri) - 1U);
    if (xQueueSend(handle->request_queue, &request, 0U) != pdTRUE) {
        firmware_update_set_status(handle,
                                   FIRMWARE_UPDATE_STATE_IDLE,
                                   ESP_ERR_TIMEOUT);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t firmware_update_service_get_status(
    firmware_update_service_handle_t handle,
    firmware_update_status_t *out_status)
{
    if ((handle == NULL) || (out_status == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(handle->status_mutex,
                       firmware_update_ms_to_ticks(
                           FIRMWARE_UPDATE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    *out_status = handle->status;
    (void)xSemaphoreGive(handle->status_mutex);
    return ESP_OK;
}

const char *firmware_update_state_to_string(firmware_update_state_t state)
{
    switch (state) {
        case FIRMWARE_UPDATE_STATE_CREATED:
            return "created";
        case FIRMWARE_UPDATE_STATE_IDLE:
            return "idle";
        case FIRMWARE_UPDATE_STATE_QUEUED:
            return "queued";
        case FIRMWARE_UPDATE_STATE_FETCHING_MANIFEST:
            return "fetching_manifest";
        case FIRMWARE_UPDATE_STATE_CHECKING_MANIFEST:
            return "checking_manifest";
        case FIRMWARE_UPDATE_STATE_OPENING_TRANSPORT:
            return "opening_transport";
        case FIRMWARE_UPDATE_STATE_CHECKING_IMAGE:
            return "checking_image";
        case FIRMWARE_UPDATE_STATE_WRITING:
            return "writing";
        case FIRMWARE_UPDATE_STATE_VALIDATING:
            return "validating";
        case FIRMWARE_UPDATE_STATE_ACTIVATING:
            return "activating";
        case FIRMWARE_UPDATE_STATE_REBOOT_PENDING:
            return "reboot_pending";
        case FIRMWARE_UPDATE_STATE_FAILED:
            return "failed";
        default:
            return "unknown";
    }
}
