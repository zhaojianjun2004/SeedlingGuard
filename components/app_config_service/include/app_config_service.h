/**
 * @file app_config_service.h
 * @brief 环境监测节点的运行时配置业务服务。
 *
 * BLE、MQTT 等通信组件通过本服务读写配置。本服务统一完成范围校验、
 * 运行时应用和 NVS 持久化，通信组件不直接依赖 NVS 或传感器实现。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "config_store.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct app_config_service *app_config_service_handle_t;

/** 当前完整的有效配置。 */
typedef struct {
    uint32_t sample_period_ms;
    uint32_t report_period_ms;
} app_runtime_config_t;

/** 部分更新模型；未置位的字段保持原值。 */
typedef struct {
    bool has_sample_period;
    uint32_t sample_period_ms;
    bool has_report_period;
    uint32_t report_period_ms;
} app_runtime_config_update_t;

/** 将采样周期应用到传感器业务层。 */
typedef esp_err_t (*app_config_apply_sample_period_fn)(void *context,
                                                       uint32_t period_ms);

/** 创建配置服务所需的依赖和边界。 */
typedef struct {
    app_runtime_config_t defaults;
    uint32_t minimum_sample_period_ms;
    uint32_t maximum_sample_period_ms;
    uint32_t minimum_report_period_ms;
    uint32_t maximum_report_period_ms;
    config_store_handle_t store;
    void *sample_context;
    app_config_apply_sample_period_fn apply_sample_period;
} app_config_service_config_t;

/** 供诊断使用的线程安全状态快照。 */
typedef struct {
    app_runtime_config_t values;
    uint32_t successful_update_count;
    uint32_t persistence_failure_count;
    esp_err_t last_error;
    bool initialized;
    bool loaded_from_nvs;
} app_config_service_status_t;

/** 创建服务，但尚不读取 NVS 或修改运行时配置。 */
esp_err_t app_config_service_create(
    const app_config_service_config_t *config,
    app_config_service_handle_t *out_handle);

/** 从 NVS 加载配置；不存在或内容无效时应用安全默认值。 */
esp_err_t app_config_service_load_and_apply(
    app_config_service_handle_t handle);

/** 线程安全地读取当前完整配置。 */
esp_err_t app_config_service_get(app_config_service_handle_t handle,
                                 app_runtime_config_t *out_config);

/** 原子地校验、应用并持久化一次部分更新。 */
esp_err_t app_config_service_update(
    app_config_service_handle_t handle,
    const app_runtime_config_update_t *update);

/** 应用默认值并删除 NVS 中的用户配置。 */
esp_err_t app_config_service_reset_defaults(
    app_config_service_handle_t handle);

/** 线程安全地复制诊断状态。 */
esp_err_t app_config_service_get_status(
    app_config_service_handle_t handle,
    app_config_service_status_t *out_status);

/** 释放服务实例；不会删除 NVS 中的数据。 */
void app_config_service_delete(app_config_service_handle_t handle);

#ifdef __cplusplus
}
#endif