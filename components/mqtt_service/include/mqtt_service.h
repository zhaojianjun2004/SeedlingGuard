/**
 * @file mqtt_service.h
 * @brief MQTT 遥测、状态发布和远程配置服务。
 *
 * 本组件通过回调访问传感器与网络状态，不直接依赖具体驱动或 Wi-Fi 实现。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MQTT_SERVICE_MIN_REPORT_PERIOD_MS (1000U)
#define MQTT_SERVICE_MAX_REPORT_PERIOD_MS (3600000U)

/** MQTT 服务使用的中立环境数据结构。 */
typedef struct {
    float temperature_c;
    float humidity_percent;
    uint32_t sequence_number;
    uint32_t consecutive_failures;
    esp_err_t last_error;
    bool valid;
} mqtt_environment_snapshot_t;

typedef esp_err_t (*mqtt_service_get_snapshot_fn)(
    void *context,
    mqtt_environment_snapshot_t *out_snapshot);

/** MQTT 配置协议使用的中立运行时配置模型。 */
typedef struct {
    uint32_t sample_period_ms;
    uint32_t report_period_ms;
} mqtt_runtime_config_t;

/** MQTT JSON 可以只更新其中一个字段。 */
typedef struct {
    bool has_sample_period;
    uint32_t sample_period_ms;
    bool has_report_period;
    uint32_t report_period_ms;
} mqtt_runtime_config_update_t;

typedef esp_err_t (*mqtt_service_get_runtime_config_fn)(
    void *context,
    mqtt_runtime_config_t *out_config);
typedef esp_err_t (*mqtt_service_apply_runtime_config_fn)(
    void *context,
    const mqtt_runtime_config_update_t *update);
typedef bool (*mqtt_service_network_ready_fn)(void *context);

/** MQTT 服务的运行状态。 */
typedef enum {
    MQTT_SERVICE_STATE_CREATED = 0,
    MQTT_SERVICE_STATE_WAITING_NETWORK,
    MQTT_SERVICE_STATE_CONNECTING,
    MQTT_SERVICE_STATE_CONNECTED,
    MQTT_SERVICE_STATE_DISCONNECTED,
} mqtt_service_state_t;

/** 创建 MQTT 服务所需的静态配置。所有字符串都会复制到组件内部。 */
typedef struct {
    const char *broker_uri;
    const char *username;
    const char *password;

    void *snapshot_context;
    mqtt_service_get_snapshot_fn get_snapshot;

    void *runtime_config_context;
    mqtt_service_get_runtime_config_fn get_runtime_config;
    mqtt_service_apply_runtime_config_fn apply_runtime_config;

    void *network_context;
    mqtt_service_network_ready_fn is_network_ready;
} mqtt_service_config_t;

/** 供诊断和后续状态汇总使用的线程安全快照。 */
typedef struct {
    mqtt_service_state_t state;
    uint32_t report_period_ms;
    uint32_t published_telemetry_count;
    uint32_t rejected_command_count;
    esp_err_t last_error;
    bool connected;
} mqtt_service_status_t;

typedef struct mqtt_service *mqtt_service_handle_t;

/** 创建 MQTT 客户端及其业务资源，但暂不启动网络连接。 */
esp_err_t mqtt_service_create(const mqtt_service_config_t *config,
                              mqtt_service_handle_t *out_handle);

/** 启动 MQTT 业务任务；任务会等待 Wi-Fi 获得 IP 后再连接 Broker。 */
esp_err_t mqtt_service_start(mqtt_service_handle_t handle);

/** 线程安全地复制当前 MQTT 状态。 */
esp_err_t mqtt_service_get_status(mqtt_service_handle_t handle,
                                  mqtt_service_status_t *out_status);

#ifdef __cplusplus
}
#endif
