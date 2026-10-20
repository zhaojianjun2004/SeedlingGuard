/**
 ******************************************************************************
 * @file    firmware_update_service.h
 * @brief   固件升级事务服务接口定义。
 * @details 通过注入的传输、清单、摘要和分区接口运行升级，事务串行且可查询进度。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：业务服务层
 ******************************************************************************
 */

#pragma once

/*--------------------------- 头文件 ---------------------------*/
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "firmware_update_contracts.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef bool (*firmware_update_network_ready_fn)(void *context);
typedef void (*firmware_update_restart_fn)(void *context);

typedef struct
{
    firmware_update_transport_t transport;
    firmware_update_manifest_parser_t manifest_parser;
    firmware_update_integrity_t integrity;
    firmware_update_platform_t platform;
    const char *expected_project_name;
    const char *expected_hardware_id;
    const char *current_version;
    uint32_t current_release_sequence;
    void *network_context;
    firmware_update_network_ready_fn is_network_ready;
    void *restart_context;
    firmware_update_restart_fn restart;
    size_t download_buffer_size;
    uint32_t restart_delay_ms;
    uint32_t task_stack_size;
    UBaseType_t task_priority;
    bool allow_same_version;
    bool allow_downgrade;
} firmware_update_service_config_t;

typedef struct
{
    firmware_update_state_t state;
    esp_err_t last_error;
    size_t bytes_received;
    size_t total_bytes;
    uint8_t progress_percent;
    uint32_t release_sequence;
    char image_version[FIRMWARE_UPDATE_NAME_MAX_LENGTH + 1U];
    char target_partition[FIRMWARE_UPDATE_PARTITION_MAX_LENGTH + 1U];
} firmware_update_status_t;

typedef struct firmware_update_service *firmware_update_service_handle_t;

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 创建固件升级事务服务实例并准备所需资源。
 * @param[in] config 模块配置及其取值约束。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t firmware_update_service_create(const firmware_update_service_config_t *config,
                                         firmware_update_service_handle_t *out_handle);

/**
 * @brief 启动固件升级事务服务的运行入口。
 * @param[in] handle 已创建的模块实例句柄。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t firmware_update_service_start(firmware_update_service_handle_t handle);

/**
 * @brief 异步提交一个升级清单 URI，忙碌时明确拒绝。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[in] manifest_uri 接口使用的名称、文本或配置内容，长度受对应模块约束。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 *
 * @details 同一时刻只允许一个事务；忙碌时返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t firmware_update_service_request(firmware_update_service_handle_t handle,
                                          const char *manifest_uri);

/**
 * @brief 复制固件升级事务服务的当前状态。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_status 接收复制出的完整状态或配置。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t firmware_update_service_get_status(firmware_update_service_handle_t handle,
                                             firmware_update_status_t *out_status);

/**
 * @brief 返回升级事务阶段的可读名称。
 * @param[in] state 将要发布的连接或事务阶段。
 * @return 只读文本指针，调用方不释放。
 */
const char *firmware_update_state_to_string(firmware_update_state_t state);

#ifdef __cplusplus
}
#endif
