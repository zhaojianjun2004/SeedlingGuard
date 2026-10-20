/**
 ******************************************************************************
 * @file    ota_button_service.c
 * @brief   升级按键入口实现。
 * @details 独立任务完成消抖和长按检测，只提交升级请求，不在按键任务下载镜像。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：应用层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "ota_button_service.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/task.h"

/*--------------------------- 模块配置、类型与状态 ---------------------------*/

#define OTA_BUTTON_URI_MAX_LENGTH       FIRMWARE_UPDATE_URI_MAX_LENGTH
#define OTA_BUTTON_MIN_HOLD_TIME_MS     (500U)
#define OTA_BUTTON_MIN_POLL_INTERVAL_MS (20U)
#define OTA_BUTTON_MAX_POLL_INTERVAL_MS (200U)
#define OTA_BUTTON_MIN_TASK_STACK_SIZE  (2048U)

static const char *TAG = "ota_button";

struct ota_button_service
{
    ota_button_service_config_t config;
    char manifest_uri[OTA_BUTTON_URI_MAX_LENGTH + 1U];
    TaskHandle_t task_handle;
    bool started;
};

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 将按键采样毫秒间隔转换为至少一个 RTOS tick。
 */
static TickType_t ota_button_ms_to_ticks(uint32_t milliseconds)
{
    const TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    return (ticks > 0U) ? ticks : 1U;
}

/**
 * @brief 检查按键 GPIO、长按阈值和任务参数。
 */
static bool ota_button_config_is_valid(const ota_button_service_config_t *config)
{
    return (config != NULL) && GPIO_IS_VALID_GPIO(config->gpio_num) &&
           (config->hold_time_ms >= OTA_BUTTON_MIN_HOLD_TIME_MS) &&
           (config->poll_interval_ms >= OTA_BUTTON_MIN_POLL_INTERVAL_MS) &&
           (config->poll_interval_ms <= OTA_BUTTON_MAX_POLL_INTERVAL_MS) &&
           (config->manifest_uri != NULL) && (config->manifest_uri[0] != '\0') &&
           (strnlen(config->manifest_uri, OTA_BUTTON_URI_MAX_LENGTH + 1U) <=
            OTA_BUTTON_URI_MAX_LENGTH) &&
           (config->update_service != NULL) &&
           (config->task_stack_size >= OTA_BUTTON_MIN_TASK_STACK_SIZE) &&
           (config->task_priority < configMAX_PRIORITIES);
}

/**
 * @brief 检测消抖后的长按事件并提交一次升级请求。
 * @note 任务入口，只在任务上下文运行；正常情况下不返回。
 */
static void ota_button_task(void *context)
{
    ota_button_service_handle_t handle = (ota_button_service_handle_t)context;
    TickType_t pressed_at = 0U;
    bool press_active = false;
    bool request_latched = false;

    while (true)
    {
        const bool pressed = (gpio_get_level(handle->config.gpio_num) == 0);
        const TickType_t now = xTaskGetTickCount();

        if (pressed && !press_active)
        {
            pressed_at = now;
            press_active = true;
            request_latched = false;
            ESP_LOGI(TAG, "BOOT button pressed; keep holding for %u ms",
                     (unsigned int)handle->config.hold_time_ms);
        }
        else if (!pressed && press_active)
        {
            press_active = false;
            request_latched = false;
        }

        if (pressed && press_active && !request_latched)
        {
            const TickType_t held_ticks = now - pressed_at;
            if (held_ticks >= ota_button_ms_to_ticks(handle->config.hold_time_ms))
            {
                request_latched = true;
                const esp_err_t result = firmware_update_service_request(
                    handle->config.update_service, handle->manifest_uri);
                if (result == ESP_OK)
                {
                    ESP_LOGI(TAG, "OTA request submitted; release BOOT button");
                }
                else
                {
                    ESP_LOGE(TAG, "OTA request rejected: %s", esp_err_to_name(result));
                }
            }
        }

        vTaskDelay(ota_button_ms_to_ticks(handle->config.poll_interval_ms));
    }
}

/**
 * @brief 创建升级按键入口实例并准备所需资源。
 * @param[in] config 模块配置及其取值约束。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t ota_button_service_create(const ota_button_service_config_t *config,
                                    ota_button_service_handle_t *out_handle)
{
    if (!ota_button_config_is_valid(config) || (out_handle == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_handle = NULL;
    ota_button_service_handle_t handle = calloc(1U, sizeof(*handle));
    if (handle == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    handle->config = *config;
    (void)strncpy(handle->manifest_uri, config->manifest_uri, sizeof(handle->manifest_uri) - 1U);
    handle->config.manifest_uri = handle->manifest_uri;

    const gpio_config_t button_gpio_config = {
        .pin_bit_mask = 1ULL << config->gpio_num,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const esp_err_t result = gpio_config(&button_gpio_config);
    if (result != ESP_OK)
    {
        free(handle);
        return result;
    }

    *out_handle = handle;
    return ESP_OK;
}

/**
 * @brief 启动升级按键入口的运行入口。
 * @param[in] handle 已创建的模块实例句柄。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t ota_button_service_start(ota_button_service_handle_t handle)
{
    if (handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->started)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const BaseType_t created =
        xTaskCreate(ota_button_task, "ota_button", handle->config.task_stack_size, handle,
                    handle->config.task_priority, &handle->task_handle);
    if (created != pdPASS)
    {
        handle->task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    handle->started = true;
    ESP_LOGI(TAG, "Hold BOOT button for %u ms to request OTA",
             (unsigned int)handle->config.hold_time_ms);
    return ESP_OK;
}
