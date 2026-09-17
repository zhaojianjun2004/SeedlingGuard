/**
 * @file update_platform_esp32.c
 * @brief ESP32 OTA 目标分区选择、写入、校验和激活实现。
 */

#include "update_platform_esp32.h"

#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

#define UPDATE_PLATFORM_IMAGE_INFO_OFFSET \
    (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))
#define UPDATE_PLATFORM_MINIMUM_HEADER_SIZE \
    (UPDATE_PLATFORM_IMAGE_INFO_OFFSET + sizeof(esp_app_desc_t))

static const char *TAG = "update_platform";

struct update_platform_esp32 {
    const esp_partition_t *target_partition;
    esp_ota_handle_t ota_handle;
    bool transaction_open;
    bool image_finished;
};

static void update_platform_copy_field(char *destination,
                                       size_t destination_size,
                                       const char *source,
                                       size_t source_size)
{
    const size_t copy_length = strnlen(source, source_size);
    const size_t bounded_length =
        (copy_length < (destination_size - 1U))
            ? copy_length
            : (destination_size - 1U);
    (void)memcpy(destination, source, bounded_length);
    destination[bounded_length] = '\0';
}

static esp_err_t update_platform_esp32_inspect_header(
    void *context,
    const uint8_t *data,
    size_t length,
    firmware_update_image_info_t *out_info)
{
    (void)context;
    if ((data == NULL) || (out_info == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (length < UPDATE_PLATFORM_MINIMUM_HEADER_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_image_header_t *image_header =
        (const esp_image_header_t *)data;
    if (image_header->magic != ESP_IMAGE_HEADER_MAGIC) {
        ESP_LOGE(TAG, "Invalid ESP image magic: 0x%02x",
                 image_header->magic);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const esp_app_desc_t *app_description =
        (const esp_app_desc_t *)&data[UPDATE_PLATFORM_IMAGE_INFO_OFFSET];
    if (app_description->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        ESP_LOGE(TAG, "Invalid application description magic");
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_info = (firmware_update_image_info_t) {0};
    update_platform_copy_field(out_info->project_name,
                               sizeof(out_info->project_name),
                               app_description->project_name,
                               sizeof(app_description->project_name));
    update_platform_copy_field(out_info->version,
                               sizeof(out_info->version),
                               app_description->version,
                               sizeof(app_description->version));
    return ESP_OK;
}

static esp_err_t update_platform_esp32_begin(void *context,
                                             size_t image_size,
                                             char *out_target,
                                             size_t target_capacity)
{
    update_platform_esp32_handle_t handle =
        (update_platform_esp32_handle_t)context;
    if ((handle == NULL) || (image_size == 0U) || (out_target == NULL) ||
        (target_capacity == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->transaction_open) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * 待确认镜像必须先完成健康检查，不能在试运行窗口中继续覆盖另一槽。
     * 这条约束避免“还没有确认当前版本，又开始下一次升级”的事务嵌套。
     */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t running_state = ESP_OTA_IMG_UNDEFINED;
    if ((running != NULL) &&
        (esp_ota_get_state_partition(running, &running_state) == ESP_OK) &&
        (running_state == ESP_OTA_IMG_PENDING_VERIFY)) {
        ESP_LOGW(TAG, "Current firmware is still pending verification");
        return ESP_ERR_INVALID_STATE;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (image_size > target->size) {
        ESP_LOGE(TAG, "Image does not fit target partition: image=%u, slot=%u",
                 (unsigned int)image_size,
                 (unsigned int)target->size);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_ota_handle_t ota_handle = 0U;
    const esp_err_t result = esp_ota_begin(target,
                                           image_size,
                                           &ota_handle);
    if (result != ESP_OK) {
        return result;
    }

    handle->target_partition = target;
    handle->ota_handle = ota_handle;
    handle->transaction_open = true;
    handle->image_finished = false;
    (void)strncpy(out_target, target->label, target_capacity - 1U);
    out_target[target_capacity - 1U] = '\0';
    ESP_LOGI(TAG, "OTA transaction opened on %s at 0x%08x",
             target->label,
             (unsigned int)target->address);
    return ESP_OK;
}

static esp_err_t update_platform_esp32_write(void *context,
                                             const uint8_t *data,
                                             size_t length)
{
    update_platform_esp32_handle_t handle =
        (update_platform_esp32_handle_t)context;
    if ((handle == NULL) || (data == NULL) || (length == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->transaction_open) {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_ota_write(handle->ota_handle, data, length);
}

static esp_err_t update_platform_esp32_finish(void *context)
{
    update_platform_esp32_handle_t handle =
        (update_platform_esp32_handle_t)context;
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->transaction_open) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t result = esp_ota_end(handle->ota_handle);
    handle->ota_handle = 0U;
    handle->transaction_open = false;
    handle->image_finished = (result == ESP_OK);
    return result;
}

static esp_err_t update_platform_esp32_activate(void *context)
{
    update_platform_esp32_handle_t handle =
        (update_platform_esp32_handle_t)context;
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->image_finished || (handle->target_partition == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t result = esp_ota_set_boot_partition(
        handle->target_partition);
    if (result == ESP_OK) {
        handle->image_finished = false;
        ESP_LOGI(TAG, "Boot partition changed to %s",
                 handle->target_partition->label);
    }
    return result;
}

static void update_platform_esp32_abort(void *context)
{
    update_platform_esp32_handle_t handle =
        (update_platform_esp32_handle_t)context;
    if ((handle != NULL) && handle->transaction_open) {
        (void)esp_ota_abort(handle->ota_handle);
        handle->ota_handle = 0U;
        handle->transaction_open = false;
        handle->image_finished = false;
        ESP_LOGW(TAG, "OTA transaction aborted");
    }
}

esp_err_t update_platform_esp32_create(
    update_platform_esp32_handle_t *out_handle)
{
    if (out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_handle = calloc(1U, sizeof(**out_handle));
    return (*out_handle != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t update_platform_esp32_get_interface(
    update_platform_esp32_handle_t handle,
    firmware_update_platform_t *out_platform)
{
    if ((handle == NULL) || (out_platform == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_platform = (firmware_update_platform_t) {
        .context = handle,
        .minimum_header_size = UPDATE_PLATFORM_MINIMUM_HEADER_SIZE,
        .inspect_header = update_platform_esp32_inspect_header,
        .begin = update_platform_esp32_begin,
        .write = update_platform_esp32_write,
        .finish = update_platform_esp32_finish,
        .activate = update_platform_esp32_activate,
        .abort = update_platform_esp32_abort,
    };
    return ESP_OK;
}
