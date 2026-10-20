/**
 ******************************************************************************
 * @file    update_integrity_sha256.h
 * @brief   SHA-256 镜像完整性适配接口定义。
 * @details 摘要对象由升级事务串行使用，校验完整性不等于验证发布者身份。
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

typedef struct update_integrity_sha256 *update_integrity_sha256_handle_t;

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 创建SHA-256 镜像完整性适配实例并准备所需资源。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t update_integrity_sha256_create(update_integrity_sha256_handle_t *out_handle);

/**
 * @brief 导出SHA-256 镜像完整性适配的函数接口供升级事务调用。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_integrity 接收本次操作的输出数据。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t update_integrity_sha256_get_interface(update_integrity_sha256_handle_t handle,
                                                firmware_update_integrity_t *out_integrity);

#ifdef __cplusplus
}
#endif
