/**
 ******************************************************************************
 * @file    ota_console_service.h
 * @brief   串口运维入口接口定义。
 * @details 在控制台任务解析维护命令；配置回显以现场回执为准。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：应用层
 ******************************************************************************
 */

#pragma once

/*--------------------------- 头文件 ---------------------------*/
#include "esp_err.h"
#include "firmware_update_service.h"
#include "gateway_link.h"
#include "wifi_manager.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
    firmware_update_service_handle_t update_service;
    gateway_link_handle_t gateway_link;
    wifi_manager_handle_t wifi;
    const char *prompt;
} ota_console_service_config_t;

typedef struct ota_console_service *ota_console_service_handle_t;

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 创建串口运维入口实例并准备所需资源。
 * @param[in] config 模块配置及其取值约束。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t ota_console_service_create(const ota_console_service_config_t *config,
                                     ota_console_service_handle_t *out_handle);

/**
 * @brief 启动串口运维入口的运行入口。
 * @param[in] handle 已创建的模块实例句柄。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t ota_console_service_start(ota_console_service_handle_t handle);

#ifdef __cplusplus
}
#endif
