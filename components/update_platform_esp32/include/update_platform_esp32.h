/**
 * @file update_platform_esp32.h
 * @brief firmware_update_service 到 ESP-IDF OTA 分区 API 的适配器。
 */

#pragma once

#include "esp_err.h"
#include "firmware_update_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct update_platform_esp32 *update_platform_esp32_handle_t;

/** 创建 ESP32 OTA 分区适配器。 */
esp_err_t update_platform_esp32_create(
    update_platform_esp32_handle_t *out_handle);

/** 获取供 firmware_update_service 使用的平台接口。 */
esp_err_t update_platform_esp32_get_interface(
    update_platform_esp32_handle_t handle,
    firmware_update_platform_t *out_platform);

#ifdef __cplusplus
}
#endif
