/**
 ******************************************************************************
 * @file    mqtt_service.h
 * @brief   MQTT 上报与远程控制服务接口定义。
 * @details 协议回调只重组/投递；业务任务负责遥测、事件、会话和确认处理。
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
#include "gateway_outbox.h"
#include "gateway_protocol.h"
#include "gateway_link.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define MQTT_SERVICE_MIN_REPORT_PERIOD_MS (1000U)
#define MQTT_SERVICE_MAX_REPORT_PERIOD_MS (3600000U)
#define MQTT_SERVICE_QUALITY_VALID        (1U)

/** MQTT 采样结果在入队前与持久记录采用同一结构。 */
typedef gateway_outbox_record_t mqtt_environment_snapshot_t;

typedef esp_err_t (*mqtt_service_get_snapshot_fn)(void *context,
                                                  mqtt_environment_snapshot_t *out_snapshot);

/** MQTT 配置协议使用的中立运行时配置模型。 */
typedef struct
{
    uint32_t sample_period_ms;
    uint32_t report_period_ms;
    uint32_t config_version;
    bool collect_enabled, field_online, synchronized;
    nursery_rule_t rules[NURSERY_POINT_COUNT];
} mqtt_runtime_config_t;

/** MQTT JSON 可以只更新其中一个字段。 */
typedef struct
{
    uint32_t request_id;
    bool has_threshold;
    gw_command_op_t threshold_op;
    uint8_t point_id;
    int32_t threshold_value;
    bool has_collect_enabled;
    bool collect_enabled;
    bool has_sample_period;
    uint32_t sample_period_ms;
    bool has_report_period;
    uint32_t report_period_ms;
} mqtt_runtime_config_update_t;

typedef esp_err_t (*mqtt_service_get_runtime_config_fn)(void *context,
                                                        mqtt_runtime_config_t *out_config);
typedef esp_err_t (*mqtt_service_apply_runtime_config_fn)(
    void *context, const mqtt_runtime_config_update_t *update);
typedef bool (*mqtt_service_network_ready_fn)(void *context);

/** MQTT 服务的运行状态。 */
typedef enum
{
    MQTT_SERVICE_STATE_CREATED = 0,
    MQTT_SERVICE_STATE_WAITING_NETWORK,
    MQTT_SERVICE_STATE_CONNECTING,
    MQTT_SERVICE_STATE_CONNECTED,
    MQTT_SERVICE_STATE_DISCONNECTED,
} mqtt_service_state_t;

/** 创建 MQTT 服务所需的静态配置。所有字符串都会复制到组件内部。 */
typedef struct
{
    const char *broker_uri;
    const char *username;
    const char *password;
    gateway_outbox_handle_t outbox;

    void *snapshot_context;
    mqtt_service_get_snapshot_fn get_snapshot;
    gateway_link_handle_t field_link;

    void *runtime_config_context;
    mqtt_service_get_runtime_config_fn get_runtime_config;
    mqtt_service_apply_runtime_config_fn apply_runtime_config;

    void *network_context;
    mqtt_service_network_ready_fn is_network_ready;
} mqtt_service_config_t;

/** 供诊断和后续状态汇总使用的线程安全快照。 */
typedef struct
{
    mqtt_service_state_t state;
    uint32_t report_period_ms;
    uint32_t published_telemetry_count;
    uint32_t stored_telemetry_count;
    uint32_t broker_ack_count;
    uint32_t pending_telemetry_count;
    uint32_t dropped_telemetry_count;
    uint32_t rejected_command_count;
    uint32_t field_applied_command_count;
    esp_err_t last_error;
    bool connected;
} mqtt_service_status_t;

typedef struct mqtt_service *mqtt_service_handle_t;

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 创建MQTT 上报与远程控制服务实例并准备所需资源。
 * @param[in] config 模块配置及其取值约束。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t mqtt_service_create(const mqtt_service_config_t *config,
                              mqtt_service_handle_t *out_handle);

/**
 * @brief 启动MQTT 上报与远程控制服务的运行入口。
 * @param[in] handle 已创建的模块实例句柄。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t mqtt_service_start(mqtt_service_handle_t handle);

/**
 * @brief 复制MQTT 上报与远程控制服务的当前状态。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_status 接收复制出的完整状态或配置。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t mqtt_service_get_status(mqtt_service_handle_t handle, mqtt_service_status_t *out_status);

#ifdef __cplusplus
}
#endif
