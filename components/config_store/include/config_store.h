/**
 * @file config_store.h
 * @brief 基于 NVS 的小型二进制配置存储接口。
 *
 * 本组件只负责可靠地读取、写入和删除一个 Blob，不理解其中的业务字段。
 * 配置版本、取值范围和默认值由上层配置服务负责。
 */

#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** NVS 命名空间和键名最多包含 15 个可见字符。 */
#define CONFIG_STORE_NAME_MAX_LENGTH (15U)

typedef struct config_store *config_store_handle_t;

/** 创建存储实例。namespace_name 和 key 会复制到组件内部。 */
esp_err_t config_store_create(const char *namespace_name,
                              const char *key,
                              config_store_handle_t *out_handle);

/**
 * @brief 读取完整 Blob。
 *
 * 找不到记录时返回 ESP_ERR_NOT_FOUND；缓冲区容量不足时返回
 * ESP_ERR_INVALID_SIZE。无论成功与否，只要能获得记录长度，就会写入
 * out_stored_size。
 */
esp_err_t config_store_load(config_store_handle_t handle,
                            void *out_data,
                            size_t capacity,
                            size_t *out_stored_size);

/** 写入 Blob 并立即提交到 Flash。 */
esp_err_t config_store_save(config_store_handle_t handle,
                            const void *data,
                            size_t data_size);

/** 删除当前键并立即提交；记录不存在也视为成功。 */
esp_err_t config_store_erase(config_store_handle_t handle);

/** 释放存储实例；不会删除 Flash 中的数据。 */
void config_store_delete(config_store_handle_t handle);

#ifdef __cplusplus
}
#endif
