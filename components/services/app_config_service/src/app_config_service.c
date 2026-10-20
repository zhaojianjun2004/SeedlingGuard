/**
 ******************************************************************************
 * @file    app_config_service.c
 * @brief   网关上报配置服务实现。
 * @details 只管理 ESP32 上报周期；先持久化成功，再发布活动配置。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：业务服务层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "app_config_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <string.h>

/*--------------------------- 模块配置、类型与状态 ---------------------------*/

struct app_config_service
{
    app_config_service_config_t config;
    app_runtime_config_t value;
    SemaphoreHandle_t lock;
    bool ready;
};

typedef struct
{
    uint32_t magic, version, period;
} app_config_record_t;

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 检查上报周期是否位于服务配置的范围内。
 */
static bool app_config_period_is_valid(app_config_service_handle_t handle,
                                       uint32_t report_period_ms)
{
    return report_period_ms >= handle->config.minimum_report_period_ms &&
           report_period_ms <= handle->config.maximum_report_period_ms;
}

/**
 * @brief 创建网关上报配置服务实例并准备所需资源。
 * @param[in] config 模块配置及其取值约束。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t app_config_service_create(const app_config_service_config_t *config,
                                    app_config_service_handle_t *out_handle)
{
    if (!config || !out_handle || !config->store || !config->minimum_report_period_ms ||
        config->defaults.report_period_ms < config->minimum_report_period_ms ||
        config->defaults.report_period_ms > config->maximum_report_period_ms)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    app_config_service_handle_t handle = calloc(1, sizeof(*handle));
    if (!handle)
    {
        return ESP_ERR_NO_MEM;
    }
    handle->config = *config;
    handle->value = config->defaults;
    handle->lock = xSemaphoreCreateMutex();
    if (!handle->lock)
    {
        free(handle);
        return ESP_ERR_NO_MEM;
    }
    *out_handle = handle;
    return ESP_OK;
}

/**
 * @brief 加载网关配置，兼容旧记录中的上报周期。
 * @param[in] handle 已创建的模块实例句柄。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t app_config_service_load_and_apply(app_config_service_handle_t handle)
{
    if (!handle)
    {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t bytes[20];
    size_t stored_size = 0;
    esp_err_t error = config_store_load(handle->config.store, bytes, sizeof(bytes), &stored_size);
    if (error == ESP_OK && stored_size == sizeof(app_config_record_t))
    {
        app_config_record_t record;
        memcpy(&record, bytes, sizeof(record));
        if (record.magic == 0x4e555243 && record.version == 2 &&
            app_config_period_is_valid(handle, record.period))
        {
            handle->value.report_period_ms = record.period;
        }
    }
    else if (error == ESP_OK && stored_size == 20)
    {
        uint32_t stored_magic, report_period_ms;
        memcpy(&stored_magic, bytes, 4);
        memcpy(&report_period_ms, bytes + 12, 4);
        if (stored_magic == 0x454e5643 && bytes[4] == 1 && bytes[5] == 0 && bytes[6] == 20 &&
            bytes[7] == 0 && app_config_period_is_valid(handle, report_period_ms))
        {
            handle->value.report_period_ms = report_period_ms;
        }
    }
    else if (error != ESP_OK && error != ESP_ERR_NOT_FOUND && error != ESP_ERR_INVALID_SIZE)
    {
        return error;
    }
    handle->ready = true;
    return ESP_OK;
}

/**
 * @brief 复制当前活动的网关上报配置。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_config 接收复制出的完整状态或配置。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t app_config_service_get(app_config_service_handle_t handle,
                                 app_runtime_config_t *out_config)
{
    if (!handle || !out_config)
    {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    *out_config = handle->value;
    xSemaphoreGive(handle->lock);
    return ESP_OK;
}

/**
 * @brief 校验候选配置，保存成功后再发布新的上报周期。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[in] update 候选配置及字段存在标志，未置位字段保持原值。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t app_config_service_update(app_config_service_handle_t handle,
                                    const app_runtime_config_update_t *update)
{
    if (!handle || !update || !update->has_report_period ||
        !app_config_period_is_valid(handle, update->report_period_ms))
    {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    app_config_record_t record = {0x4e555243, 2, update->report_period_ms};
    esp_err_t error = ESP_OK;
    if (handle->value.report_period_ms != update->report_period_ms)
    {
        error = config_store_save(handle->config.store, &record, sizeof(record));
    }
    if (error == ESP_OK)
    {
        handle->value.report_period_ms = update->report_period_ms;
    }
    xSemaphoreGive(handle->lock);
    return error;
}

/**
 * @brief 复制网关上报配置服务的当前状态。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_status 接收复制出的完整状态或配置。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t app_config_service_get_status(app_config_service_handle_t handle,
                                        app_config_service_status_t *out_status)
{
    if (!handle || !out_status)
    {
        return ESP_ERR_INVALID_ARG;
    }
    out_status->initialized = handle->ready;
    return ESP_OK;
}
