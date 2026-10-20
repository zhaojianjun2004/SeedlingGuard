/**
 ******************************************************************************
 * @file    update_transport_http.h
 * @brief   HTTP 升级字节流适配接口定义。
 * @details 下载过程在升级任务执行；返回字节流及长度，不负责清单和镜像业务校验。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：设备驱动 / 平台适配层
 ******************************************************************************
 */

#pragma once

/*--------------------------- 头文件 ---------------------------*/
#include <stdint.h>

#include "esp_err.h"
#include "firmware_update_contracts.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
    uint32_t timeout_ms;
    uint32_t receive_buffer_size;
} update_transport_http_config_t;

typedef struct update_transport_http *update_transport_http_handle_t;

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 创建HTTP 升级字节流适配实例并准备所需资源。
 * @param[in] config 模块配置及其取值约束。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t update_transport_http_create(const update_transport_http_config_t *config,
                                       update_transport_http_handle_t *out_handle);

/**
 * @brief 导出HTTP 升级字节流适配的函数接口供升级事务调用。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_transport 接收本次操作的输出数据。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t update_transport_http_get_interface(update_transport_http_handle_t handle,
                                              firmware_update_transport_t *out_transport);

#ifdef __cplusplus
}
#endif
