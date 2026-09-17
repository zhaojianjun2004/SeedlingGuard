/**
 * @file update_integrity_sha256.h
 * @brief OTA 字节流 SHA-256 完整性验证适配器。
 */

#pragma once

#include "esp_err.h"
#include "firmware_update_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct update_integrity_sha256 *update_integrity_sha256_handle_t;

/** 创建可复用的单会话 SHA-256 验证器。 */
esp_err_t update_integrity_sha256_create(
    update_integrity_sha256_handle_t *out_handle);

/** 获取供升级 Core 注入使用的完整性接口。 */
esp_err_t update_integrity_sha256_get_interface(
    update_integrity_sha256_handle_t handle,
    firmware_update_integrity_t *out_integrity);

#ifdef __cplusplus
}
#endif
