/**
 * @file wifi_manager.h
 * @brief Wi-Fi Station 连接与自动重连服务。
 *
 * 本组件只负责网络连接生命周期，不读取传感器，也不启动 MQTT。
 * 上层模块通过状态快照判断网络是否可用，避免直接依赖 Wi-Fi 事件细节。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_MANAGER_IPV4_TEXT_LENGTH (16U)

/** Wi-Fi Station 的业务状态。 */
typedef enum {
    WIFI_MANAGER_STATE_CREATED = 0,
    WIFI_MANAGER_STATE_STARTING,
    WIFI_MANAGER_STATE_CONNECTING,
    WIFI_MANAGER_STATE_WAITING_RETRY,
    WIFI_MANAGER_STATE_CONNECTED,
} wifi_manager_state_t;

/** 创建服务所需的静态配置。凭据会复制到组件内部。 */
typedef struct {
    const char *ssid;                         /**< 路由器 SSID，不能为空。 */
    const char *password;                     /**< 路由器密码；开放网络可为空。 */
    uint32_t reconnect_initial_delay_ms;      /**< 首次重连等待时间。 */
    uint32_t reconnect_max_delay_ms;          /**< 指数退避的最大等待时间。 */
} wifi_manager_config_t;

/** 供 BLE、MQTT 和诊断日志读取的线程安全状态快照。 */
typedef struct {
    wifi_manager_state_t state;
    char ipv4_address[WIFI_MANAGER_IPV4_TEXT_LENGTH];
    uint32_t reconnect_attempts;
    uint8_t last_disconnect_reason;
    esp_err_t last_error;
    bool has_ipv4;
} wifi_manager_status_t;

/** 不透明句柄，隐藏 ESP-IDF Wi-Fi 和事件处理细节。 */
typedef struct wifi_manager *wifi_manager_handle_t;

/** 初始化网络栈、事件处理器和 Station 配置，但暂不启动连接。 */
esp_err_t wifi_manager_create(const wifi_manager_config_t *config,
                              wifi_manager_handle_t *out_handle);

/** 启动 Wi-Fi；函数立即返回，连接结果通过事件异步更新。 */
esp_err_t wifi_manager_start(wifi_manager_handle_t handle);

/** 线程安全地复制当前 Wi-Fi 状态。 */
esp_err_t wifi_manager_get_status(wifi_manager_handle_t handle,
                                  wifi_manager_status_t *out_status);

/** 将状态枚举转换成只读文本，便于日志和后续 MQTT 上报。 */
const char *wifi_manager_state_to_string(wifi_manager_state_t state);

#ifdef __cplusplus
}
#endif
