/**
 * @file ota_console_service.h
 * @brief 用于实验室手动触发和观察 OTA 的串口命令服务。
 */

#pragma once

#include "esp_err.h"
#include "firmware_update_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    firmware_update_service_handle_t update_service;
    const char *prompt;
} ota_console_service_config_t;

typedef struct ota_console_service *ota_console_service_handle_t;

/** 注册 ota 命令并创建 UART REPL。 */
esp_err_t ota_console_service_create(
    const ota_console_service_config_t *config,
    ota_console_service_handle_t *out_handle);

/** 启动串口 REPL；命令执行与固件下载均为异步。 */
esp_err_t ota_console_service_start(ota_console_service_handle_t handle);

#ifdef __cplusplus
}
#endif
