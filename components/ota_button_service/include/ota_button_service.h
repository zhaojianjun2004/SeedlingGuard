/**
 * @file ota_button_service.h
 * @brief 使用开发板按键提交 OTA 实验请求的输入适配器。
 */

#pragma once

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "firmware_update_service.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gpio_num_t gpio_num;
    uint32_t hold_time_ms;
    uint32_t poll_interval_ms;
    const char *manifest_uri;
    firmware_update_service_handle_t update_service;
    uint32_t task_stack_size;
    UBaseType_t task_priority;
} ota_button_service_config_t;

typedef struct ota_button_service *ota_button_service_handle_t;

/** 配置活动低电平按键并创建服务对象。 */
esp_err_t ota_button_service_create(
    const ota_button_service_config_t *config,
    ota_button_service_handle_t *out_handle);

/** 启动按键轮询任务；长按达到阈值后只提交一次请求，松开后重新解锁。 */
esp_err_t ota_button_service_start(ota_button_service_handle_t handle);

#ifdef __cplusplus
}
#endif
