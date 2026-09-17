/**
 * @file boot_health_service.h
 * @brief OTA 新固件首次启动的健康检查、确认和回滚服务。
 *
 * 本组件只负责 ESP-IDF OTA 镜像状态和启动确认。具体业务是否健康由应用层
 * 通过回调判断，因此组件不直接依赖传感器、Wi-Fi、MQTT 或未来的 BMS 驱动。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Boot health 服务的业务状态。 */
typedef enum {
    BOOT_HEALTH_STATE_CREATED = 0,
    BOOT_HEALTH_STATE_NOT_REQUIRED,
    BOOT_HEALTH_STATE_WAITING_CONFIRMATION,
    BOOT_HEALTH_STATE_CHECKING,
    BOOT_HEALTH_STATE_CONFIRMED,
    BOOT_HEALTH_STATE_ROLLBACK_REQUESTED,
    BOOT_HEALTH_STATE_FAILED,
} boot_health_state_t;

/**
 * @brief 由应用实现的启动自检回调。
 *
 * 返回 ESP_OK 表示当前固件已经满足确认条件；其他错误表示新固件不应继续
 * 作为有效镜像运行。回调不能把临时断网或外部传感器未连接误判为固件损坏。
 */
typedef esp_err_t (*boot_health_self_test_fn)(void *context);

typedef struct {
    void *self_test_context;
    boot_health_self_test_fn run_self_test;
    uint32_t confirmation_delay_ms;
    uint32_t task_stack_size;
    UBaseType_t task_priority;
} boot_health_service_config_t;

typedef struct {
    boot_health_state_t state;
    esp_ota_img_states_t initial_ota_state;
    esp_err_t last_error;
    bool pending_verification;
} boot_health_status_t;

typedef struct boot_health_service *boot_health_service_handle_t;

/** 创建服务对象，但不创建任务或修改 OTA 镜像状态。 */
esp_err_t boot_health_service_create(
    const boot_health_service_config_t *config,
    boot_health_service_handle_t *out_handle);

/**
 * @brief 输出固件、分区和复位信息，并在需要时启动延迟确认任务。
 *
 * 只有当前 OTA 镜像处于 ESP_OTA_IMG_PENDING_VERIFY 时才创建任务。普通启动
 * 会记录“不需要确认”并立即返回。
 */
esp_err_t boot_health_service_start(boot_health_service_handle_t handle);

/** 线程安全地复制当前诊断状态。 */
esp_err_t boot_health_service_get_status(
    boot_health_service_handle_t handle,
    boot_health_status_t *out_status);

/** 将业务状态转换为日志可读文本。 */
const char *boot_health_state_to_string(boot_health_state_t state);

#ifdef __cplusplus
}
#endif
