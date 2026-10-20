/**
 ******************************************************************************
 * @file    boot_health_service.h
 * @brief   OTA 首启确认服务接口定义。
 * @details 仅待确认镜像启动一次性健康任务，成功确认有效，失败请求回滚。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：业务服务层
 ******************************************************************************
 */

#pragma once

/*--------------------------- 头文件 ---------------------------*/
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Boot health 服务的业务状态。 */
typedef enum
{
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

typedef struct
{
    void *self_test_context;
    boot_health_self_test_fn run_self_test;
    uint32_t confirmation_delay_ms;
    uint32_t task_stack_size;
    UBaseType_t task_priority;
} boot_health_service_config_t;

typedef struct
{
    boot_health_state_t state;
    esp_ota_img_states_t initial_ota_state;
    esp_err_t last_error;
    bool pending_verification;
} boot_health_status_t;

typedef struct boot_health_service *boot_health_service_handle_t;

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 创建OTA 首启确认服务实例并准备所需资源。
 * @param[in] config 模块配置及其取值约束。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t boot_health_service_create(const boot_health_service_config_t *config,
                                     boot_health_service_handle_t *out_handle);

/**
 * @brief 查询当前镜像状态，仅为待确认镜像启动自检任务。
 * @param[in] handle 已创建的模块实例句柄。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 *
 * @details 只有当前 OTA 镜像处于 ESP_OTA_IMG_PENDING_VERIFY 时才创建任务。普通启动 会记录“不需要确认”并立即返回。
 */
esp_err_t boot_health_service_start(boot_health_service_handle_t handle);

/**
 * @brief 复制OTA 首启确认服务的当前状态。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_status 接收复制出的完整状态或配置。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t boot_health_service_get_status(boot_health_service_handle_t handle,
                                         boot_health_status_t *out_status);

/**
 * @brief 返回启动健康确认阶段的可读名称。
 * @param[in] state 将要发布的连接或事务阶段。
 * @return 只读文本指针，调用方不释放。
 */
const char *boot_health_state_to_string(boot_health_state_t state);

#ifdef __cplusplus
}
#endif
