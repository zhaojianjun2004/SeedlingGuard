/**
 * @file update_transport_http.h
 * @brief 固件升级字节流的 HTTP 实验适配器。
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "firmware_update_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t timeout_ms;
    uint32_t receive_buffer_size;
} update_transport_http_config_t;

typedef struct update_transport_http *update_transport_http_handle_t;

/** 创建 HTTP 适配器。当前版本只允许实验室局域网中的明文 http:// URI。 */
esp_err_t update_transport_http_create(
    const update_transport_http_config_t *config,
    update_transport_http_handle_t *out_handle);

/** 获取供 firmware_update_service 使用的平台中立字节流接口。 */
esp_err_t update_transport_http_get_interface(
    update_transport_http_handle_t handle,
    firmware_update_transport_t *out_transport);

#ifdef __cplusplus
}
#endif
