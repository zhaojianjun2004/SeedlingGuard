/**
 * @file update_manifest_json.h
 * @brief 有边界约束的 OTA JSON Manifest 解析适配器。
 */

#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "firmware_update_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t maximum_document_size;
} update_manifest_json_config_t;

typedef struct update_manifest_json *update_manifest_json_handle_t;

/** 创建 JSON Manifest 解析器。 */
esp_err_t update_manifest_json_create(
    const update_manifest_json_config_t *config,
    update_manifest_json_handle_t *out_handle);

/** 获取供升级 Core 注入使用的解析接口。 */
esp_err_t update_manifest_json_get_interface(
    update_manifest_json_handle_t handle,
    firmware_update_manifest_parser_t *out_parser);

#ifdef __cplusplus
}
#endif
