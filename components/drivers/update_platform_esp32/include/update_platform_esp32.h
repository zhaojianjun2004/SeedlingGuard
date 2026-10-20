/**
 ******************************************************************************
 * @file    update_platform_esp32.h
 * @brief   ESP32 升级分区适配接口定义。
 * @details 实现备用分区写入、镜像校验和启动槽切换，不决定版本准入策略。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：设备驱动 / 平台适配层
 ******************************************************************************
 */

#pragma once

/*--------------------------- 头文件 ---------------------------*/
#include "esp_err.h"
#include "firmware_update_contracts.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct update_platform_esp32 *update_platform_esp32_handle_t;

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 创建ESP32 升级分区适配实例并准备所需资源。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t update_platform_esp32_create(update_platform_esp32_handle_t *out_handle);

/**
 * @brief 导出ESP32 升级分区适配的函数接口供升级事务调用。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_platform 接收本次操作的输出数据。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t update_platform_esp32_get_interface(update_platform_esp32_handle_t handle,
                                              firmware_update_platform_t *out_platform);

#ifdef __cplusplus
}
#endif
