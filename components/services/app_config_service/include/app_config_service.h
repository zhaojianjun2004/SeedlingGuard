/**
 ******************************************************************************
 * @file    app_config_service.h
 * @brief   网关上报配置服务接口定义。
 * @details 只管理 ESP32 上报周期；先持久化成功，再发布活动配置。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：业务服务层
 ******************************************************************************
 */

#pragma once
/*--------------------------- 头文件 ---------------------------*/
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "config_store.h"
typedef struct app_config_service *app_config_service_handle_t;

typedef struct
{
    uint32_t report_period_ms;
} app_runtime_config_t;

typedef struct
{
    bool has_report_period;
    uint32_t report_period_ms;
} app_runtime_config_update_t;

typedef struct
{
    app_runtime_config_t defaults;
    uint32_t minimum_report_period_ms, maximum_report_period_ms;
    config_store_handle_t store;
} app_config_service_config_t;

typedef struct
{
    bool initialized;
} app_config_service_status_t;

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 创建网关上报配置服务实例并准备所需资源。
 * @param[in] config 模块配置及其取值约束。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t app_config_service_create(const app_config_service_config_t *config,
                                    app_config_service_handle_t *out_handle);

/**
 * @brief 加载网关配置，兼容旧记录中的上报周期。
 * @param[in] handle 已创建的模块实例句柄。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t app_config_service_load_and_apply(app_config_service_handle_t handle);

/**
 * @brief 复制当前活动的网关上报配置。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_config 接收复制出的完整状态或配置。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t app_config_service_get(app_config_service_handle_t handle,
                                 app_runtime_config_t *out_config);

/**
 * @brief 校验候选配置，保存成功后再发布新的上报周期。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[in] update 候选配置及字段存在标志，未置位字段保持原值。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t app_config_service_update(app_config_service_handle_t handle,
                                    const app_runtime_config_update_t *update);

/**
 * @brief 复制网关上报配置服务的当前状态。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_status 接收复制出的完整状态或配置。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t app_config_service_get_status(app_config_service_handle_t handle,
                                        app_config_service_status_t *out_status);
