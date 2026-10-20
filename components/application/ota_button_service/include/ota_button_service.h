/**
 ******************************************************************************
 * @file    ota_button_service.h
 * @brief   升级按键入口接口定义。
 * @details 独立任务完成消抖和长按检测，只提交升级请求，不在按键任务下载镜像。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：应用层
 ******************************************************************************
 */

#pragma once

/*--------------------------- 头文件 ---------------------------*/
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "firmware_update_service.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
    gpio_num_t gpio_num;
    uint32_t hold_time_ms;
    uint32_t poll_interval_ms;
    const char *manifest_uri;
    firmware_update_service_handle_t update_service;
    uint32_t task_stack_size;
    UBaseType_t task_priority;
} ota_button_service_config_t;

typedef struct ota_button_service *ota_button_service_handle_t;

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 创建升级按键入口实例并准备所需资源。
 * @param[in] config 模块配置及其取值约束。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t ota_button_service_create(const ota_button_service_config_t *config,
                                    ota_button_service_handle_t *out_handle);

/**
 * @brief 启动升级按键入口的运行入口。
 * @param[in] handle 已创建的模块实例句柄。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t ota_button_service_start(ota_button_service_handle_t handle);

#ifdef __cplusplus
}
#endif
