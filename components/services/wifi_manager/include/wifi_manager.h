/**
 ******************************************************************************
 * @file    wifi_manager.h
 * @brief   Wi-Fi 配网与重连服务接口定义。
 * @details 事件与定时器管理 AP/STA；候选凭据成功获得 IP 后再保存。
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

#ifdef __cplusplus
extern "C"
{
#endif

#define WIFI_MANAGER_IPV4_TEXT_LENGTH (16U)

/** Wi-Fi Station 的业务状态。 */
typedef enum
{
    WIFI_MANAGER_STATE_CREATED = 0,
    WIFI_MANAGER_STATE_STARTING,
    WIFI_MANAGER_STATE_CONNECTING,
    WIFI_MANAGER_STATE_WAITING_RETRY,
    WIFI_MANAGER_STATE_CONNECTED,
    WIFI_MANAGER_STATE_PROVISIONING_AP,
} wifi_manager_state_t;

/** 创建服务所需的静态配置。凭据会复制到组件内部。 */
typedef struct
{
    const char *ssid;                    /**< 路由器 SSID；为空则从 NVS 恢复或启动配网 AP。 */
    const char *password;                /**< 路由器密码；开放网络可为空。 */
    uint32_t reconnect_initial_delay_ms; /**< 首次重连等待时间。 */
    uint32_t reconnect_max_delay_ms;     /**< 指数退避的最大等待时间。 */
} wifi_manager_config_t;

/** 供 BLE、MQTT 和诊断日志读取的线程安全状态快照。 */
typedef struct
{
    wifi_manager_state_t state;
    char ipv4_address[WIFI_MANAGER_IPV4_TEXT_LENGTH];
    uint32_t reconnect_attempts;
    uint8_t last_disconnect_reason;
    esp_err_t last_error;
    bool has_ipv4;
    bool ap_active;
    bool provisioning_pending;
} wifi_manager_status_t;

/** 不透明句柄，隐藏 ESP-IDF Wi-Fi 和事件处理细节。 */
typedef struct wifi_manager *wifi_manager_handle_t;

/* 仅在配网 AP 活动时接受新凭据；拿到 STA IP 后写入 NVS。 */

/* 本地维护入口：清除保存的 STA 凭据并切回配网 AP。 */

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 创建Wi-Fi 配网与重连服务实例并准备所需资源。
 * @param[in] config 模块配置及其取值约束。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t wifi_manager_create(const wifi_manager_config_t *config,
                              wifi_manager_handle_t *out_handle);

/**
 * @brief 启动Wi-Fi 配网与重连服务的运行入口。
 * @param[in] handle 已创建的模块实例句柄。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t wifi_manager_start(wifi_manager_handle_t handle);

/**
 * @brief 复制Wi-Fi 配网与重连服务的当前状态。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_status 接收复制出的完整状态或配置。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t wifi_manager_get_status(wifi_manager_handle_t handle, wifi_manager_status_t *out_status);

/**
 * @brief 接收候选凭据并发起连接，暂不提前保存为有效配置。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[in] ssid 接口使用的名称、文本或配置内容，长度受对应模块约束。
 * @param[in] password 接口使用的名称、文本或配置内容，长度受对应模块约束。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t wifi_manager_submit_credentials(wifi_manager_handle_t handle, const char *ssid,
                                          const char *password);

/**
 * @brief 清除已保存凭据并返回 AP 配网状态。
 * @param[in] handle 已创建的模块实例句柄。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t wifi_manager_clear_credentials(wifi_manager_handle_t handle);

/**
 * @brief 返回 Wi-Fi 连接阶段的可读名称。
 * @param[in] state 将要发布的连接或事务阶段。
 * @return 只读文本指针，调用方不释放。
 */
const char *wifi_manager_state_to_string(wifi_manager_state_t state);

#ifdef __cplusplus
}
#endif
