/**
 ******************************************************************************
 * @file    boot_health_service.c
 * @brief   OTA 首启确认服务实现。
 * @details 仅待确认镜像启动一次性健康任务，成功确认有效，失败请求回滚。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：业务服务层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "boot_health_service.h"

#include <inttypes.h>
#include <stdlib.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/*--------------------------- 模块配置、类型与状态 ---------------------------*/

#define BOOT_HEALTH_MIN_TASK_STACK_SIZE (3072U)
#define BOOT_HEALTH_LOCK_TIMEOUT_MS     (100U)

static const char *TAG = "boot_health";

struct boot_health_service
{
    boot_health_service_config_t config;
    SemaphoreHandle_t status_mutex;
    TaskHandle_t task_handle;
    boot_health_status_t status;
    bool started;
};

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 将启动确认延迟转换为至少一个 RTOS tick。
 */
static TickType_t boot_health_ms_to_ticks(uint32_t milliseconds)
{
    const TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    return (ticks > 0U) ? ticks : 1U;
}

/**
 * @brief 检查首启自检回调、确认延迟和任务参数。
 */
static bool boot_health_config_is_valid(const boot_health_service_config_t *config)
{
    return (config != NULL) && (config->run_self_test != NULL) &&
           (config->confirmation_delay_ms > 0U) &&
           (config->task_stack_size >= BOOT_HEALTH_MIN_TASK_STACK_SIZE) &&
           (config->task_priority < configMAX_PRIORITIES);
}

/**
 * @brief 返回 OTA 分区状态的可读名称。
 */
static const char *boot_health_ota_state_to_string(esp_ota_img_states_t state)
{
    switch (state)
    {
        case ESP_OTA_IMG_NEW:
            return "new";
        case ESP_OTA_IMG_PENDING_VERIFY:
            return "pending_verify";
        case ESP_OTA_IMG_VALID:
            return "valid";
        case ESP_OTA_IMG_INVALID:
            return "invalid";
        case ESP_OTA_IMG_ABORTED:
            return "aborted";
        case ESP_OTA_IMG_UNDEFINED:
        default:
            return "undefined";
    }
}

/**
 * @brief 在互斥保护下更新启动确认阶段和最近错误。
 */
static void boot_health_set_status(boot_health_service_handle_t handle, boot_health_state_t state,
                                   esp_err_t last_error)
{
    if (xSemaphoreTake(handle->status_mutex,
                       boot_health_ms_to_ticks(BOOT_HEALTH_LOCK_TIMEOUT_MS)) == pdTRUE)
    {
        handle->status.state = state;
        handle->status.last_error = last_error;
        (void)xSemaphoreGive(handle->status_mutex);
    }
}

/**
 * @brief 延迟执行自检，成功确认镜像，失败请求回滚。
 * @note 任务入口，只在任务上下文运行；正常情况下不返回。
 */
static void boot_health_confirmation_task(void *context)
{
    boot_health_service_handle_t handle = (boot_health_service_handle_t)context;

    ESP_LOGI(TAG, "New firmware is pending verification; self-test starts in %" PRIu32 " ms",
             handle->config.confirmation_delay_ms);
    vTaskDelay(boot_health_ms_to_ticks(handle->config.confirmation_delay_ms));

    boot_health_set_status(handle, BOOT_HEALTH_STATE_CHECKING, ESP_OK);
    const esp_err_t self_test_result =
        handle->config.run_self_test(handle->config.self_test_context);

    if (self_test_result == ESP_OK)
    {
        const esp_err_t confirm_result = esp_ota_mark_app_valid_cancel_rollback();
        if (confirm_result == ESP_OK)
        {
            boot_health_set_status(handle, BOOT_HEALTH_STATE_CONFIRMED, ESP_OK);
            ESP_LOGI(TAG, "Boot health check passed; firmware marked valid");
        }
        else
        {
            boot_health_set_status(handle, BOOT_HEALTH_STATE_FAILED, confirm_result);
            ESP_LOGE(TAG, "Failed to confirm firmware: %s", esp_err_to_name(confirm_result));
        }
    }
    else
    {
        boot_health_set_status(handle, BOOT_HEALTH_STATE_ROLLBACK_REQUESTED, self_test_result);
        ESP_LOGE(TAG, "Boot health check failed (%s); requesting rollback",
                 esp_err_to_name(self_test_result));

        /* 成功时此函数会立即重启；只有回滚不可用或状态异常时才会返回。 */
        const esp_err_t rollback_result = esp_ota_mark_app_invalid_rollback_and_reboot();
        boot_health_set_status(handle, BOOT_HEALTH_STATE_FAILED, rollback_result);
        ESP_LOGE(TAG, "Rollback request failed: %s", esp_err_to_name(rollback_result));
    }

    handle->task_handle = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief 创建OTA 首启确认服务实例并准备所需资源。
 * @param[in] config 模块配置及其取值约束。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t boot_health_service_create(const boot_health_service_config_t *config,
                                     boot_health_service_handle_t *out_handle)
{
    if (!boot_health_config_is_valid(config) || (out_handle == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_handle = NULL;
    boot_health_service_handle_t handle = calloc(1U, sizeof(*handle));
    if (handle == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    handle->config = *config;
    handle->status.state = BOOT_HEALTH_STATE_CREATED;
    handle->status.initial_ota_state = ESP_OTA_IMG_UNDEFINED;
    handle->status.last_error = ESP_OK;
    handle->status_mutex = xSemaphoreCreateMutex();
    if (handle->status_mutex == NULL)
    {
        free(handle);
        return ESP_ERR_NO_MEM;
    }

    *out_handle = handle;
    return ESP_OK;
}

/**
 * @brief 查询当前镜像状态，仅为待确认镜像启动自检任务。
 * @param[in] handle 已创建的模块实例句柄。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t boot_health_service_start(boot_health_service_handle_t handle)
{
    if (handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->started)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    if (running_partition == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t *app_description = esp_app_get_description();
    ESP_LOGI(TAG, "Firmware: project=%s, version=%s, IDF=%s, built=%s %s",
             app_description->project_name, app_description->version, app_description->idf_ver,
             app_description->date, app_description->time);
    ESP_LOGI(TAG,
             "Running partition: label=%s, address=0x%08" PRIx32 ", size=%" PRIu32
             " bytes, reset_reason=%d",
             running_partition->label, running_partition->address, running_partition->size,
             (int)esp_reset_reason());
    if (update_partition != NULL)
    {
        ESP_LOGI(TAG,
                 "Next update partition: label=%s, address=0x%08" PRIx32 ", size=%" PRIu32 " bytes",
                 update_partition->label, update_partition->address, update_partition->size);
    }

    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    const esp_err_t state_result = esp_ota_get_state_partition(running_partition, &ota_state);

    if ((state_result != ESP_OK) && (state_result != ESP_ERR_NOT_FOUND) &&
        (state_result != ESP_ERR_NOT_SUPPORTED))
    {
        boot_health_set_status(handle, BOOT_HEALTH_STATE_FAILED, state_result);
        return state_result;
    }

    if (xSemaphoreTake(handle->status_mutex,
                       boot_health_ms_to_ticks(BOOT_HEALTH_LOCK_TIMEOUT_MS)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    handle->status.initial_ota_state = ota_state;
    handle->status.pending_verification =
        (state_result == ESP_OK) && (ota_state == ESP_OTA_IMG_PENDING_VERIFY);
    (void)xSemaphoreGive(handle->status_mutex);

    ESP_LOGI(TAG, "Initial OTA image state: %s", boot_health_ota_state_to_string(ota_state));

    if ((state_result != ESP_OK) || (ota_state != ESP_OTA_IMG_PENDING_VERIFY))
    {
        boot_health_set_status(handle, BOOT_HEALTH_STATE_NOT_REQUIRED, ESP_OK);
        handle->started = true;
        ESP_LOGI(TAG, "This boot does not require OTA confirmation");
        return ESP_OK;
    }

    boot_health_set_status(handle, BOOT_HEALTH_STATE_WAITING_CONFIRMATION, ESP_OK);
    const BaseType_t task_created =
        xTaskCreate(boot_health_confirmation_task, "boot_health", handle->config.task_stack_size,
                    handle, handle->config.task_priority, &handle->task_handle);
    if (task_created != pdPASS)
    {
        handle->task_handle = NULL;
        boot_health_set_status(handle, BOOT_HEALTH_STATE_FAILED, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    handle->started = true;
    return ESP_OK;
}

/**
 * @brief 复制OTA 首启确认服务的当前状态。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_status 接收复制出的完整状态或配置。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t boot_health_service_get_status(boot_health_service_handle_t handle,
                                         boot_health_status_t *out_status)
{
    if ((handle == NULL) || (out_status == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(handle->status_mutex,
                       boot_health_ms_to_ticks(BOOT_HEALTH_LOCK_TIMEOUT_MS)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    *out_status = handle->status;
    (void)xSemaphoreGive(handle->status_mutex);
    return ESP_OK;
}

/**
 * @brief 返回启动健康确认阶段的可读名称。
 * @param[in] state 将要发布的连接或事务阶段。
 * @return 只读文本指针，调用方不释放。
 */
const char *boot_health_state_to_string(boot_health_state_t state)
{
    switch (state)
    {
        case BOOT_HEALTH_STATE_CREATED:
            return "created";
        case BOOT_HEALTH_STATE_NOT_REQUIRED:
            return "not_required";
        case BOOT_HEALTH_STATE_WAITING_CONFIRMATION:
            return "waiting_confirmation";
        case BOOT_HEALTH_STATE_CHECKING:
            return "checking";
        case BOOT_HEALTH_STATE_CONFIRMED:
            return "confirmed";
        case BOOT_HEALTH_STATE_ROLLBACK_REQUESTED:
            return "rollback_requested";
        case BOOT_HEALTH_STATE_FAILED:
            return "failed";
        default:
            return "unknown";
    }
}
