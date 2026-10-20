/**
 ******************************************************************************
 * @file    config_store.h
 * @brief   NVS 配置字节存储接口定义。
 * @details 只管理单个命名空间/键的二进制配置，不解释采集和告警语义。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：设备驱动 / 平台适配层
 ******************************************************************************
 */

#pragma once

/*--------------------------- 头文件 ---------------------------*/
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** NVS 命名空间和键名最多包含 15 个可见字符。 */
#define CONFIG_STORE_NAME_MAX_LENGTH (15U)

typedef struct config_store *config_store_handle_t;

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 创建NVS 配置字节存储实例并准备所需资源。
 * @param[in] namespace_name 接口使用的名称、文本或配置内容，长度受对应模块约束。
 * @param[in] key 接口使用的名称、文本或配置内容，长度受对应模块约束。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t config_store_create(const char *namespace_name, const char *key,
                              config_store_handle_t *out_handle);

/**
 * @brief 读取配置二进制内容，并报告实际存储长度。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_data 接收本次操作的输出数据。
 * @param[in] capacity 输出缓冲区容量，单位字节。
 * @param[out] out_stored_size 接收本次操作的输出数据。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 *
 * @details 找不到记录时返回 ESP_ERR_NOT_FOUND；缓冲区容量不足时返回 ESP_ERR_INVALID_SIZE。无论成功与否，只要能获得记录长度，就会写入 out_stored_size。
 */
esp_err_t config_store_load(config_store_handle_t handle, void *out_data, size_t capacity,
                            size_t *out_stored_size);

/**
 * @brief 写入配置 Blob 并提交 NVS。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[in] data 输入字节缓冲区。
 * @param[in] data_size 待保存二进制内容的长度，单位字节。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t config_store_save(config_store_handle_t handle, const void *data, size_t data_size);

/**
 * @brief 删除配置键并提交，不存在的键视为已清除。
 * @param[in] handle 已创建的模块实例句柄。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t config_store_erase(config_store_handle_t handle);

/**
 * @brief 释放配置存储实例，保留 Flash 中的数据。
 * @param[in] handle 已创建的模块实例句柄。
 */
void config_store_delete(config_store_handle_t handle);

#ifdef __cplusplus
}
#endif
