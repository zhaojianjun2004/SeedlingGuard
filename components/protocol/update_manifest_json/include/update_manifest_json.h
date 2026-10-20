/**
 ******************************************************************************
 * @file    update_manifest_json.h
 * @brief   升级清单 JSON 编解码接口定义。
 * @details 将受限 JSON 文档转换为公共清单结构，产品/硬件/版本准入由升级服务决定。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：协议层
 ******************************************************************************
 */

#pragma once

/*--------------------------- 头文件 ---------------------------*/
#include <stddef.h>

#include "esp_err.h"
#include "firmware_update_contracts.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
    size_t maximum_document_size;
} update_manifest_json_config_t;

typedef struct update_manifest_json *update_manifest_json_handle_t;

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 创建升级清单 JSON 编解码实例并准备所需资源。
 * @param[in] config 模块配置及其取值约束。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t update_manifest_json_create(const update_manifest_json_config_t *config,
                                      update_manifest_json_handle_t *out_handle);

/**
 * @brief 导出升级清单 JSON 编解码的函数接口供升级事务调用。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_parser 接收本次操作的输出数据。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t update_manifest_json_get_interface(update_manifest_json_handle_t handle,
                                             firmware_update_manifest_parser_t *out_parser);

#ifdef __cplusplus
}
#endif
