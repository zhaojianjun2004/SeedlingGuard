/**
 ******************************************************************************
 * @file    wifi_manager.c
 * @brief   Wi-Fi 配网与重连服务实现。
 * @details 事件与定时器管理 AP/STA；候选凭据成功获得 IP 后再保存。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：业务服务层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "wifi_manager.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

/*--------------------------- 模块配置、类型与状态 ---------------------------*/

#define WIFI_MANAGER_MIN_RECONNECT_DELAY_MS (100U)
#define WIFI_MANAGER_MAX_RECONNECT_DELAY_MS (300000U)
#define WIFI_MANAGER_LOCK_TIMEOUT_MS        (100U)
#define WIFI_MANAGER_AP_FALLBACK_ATTEMPTS   (6U)
#define WIFI_MANAGER_AP_SHUTDOWN_DELAY_US   (3000000ULL)
#define WIFI_MANAGER_NVS_NAMESPACE          "wifi_cred"

static const char *TAG = "wifi_manager";

struct wifi_manager
{
    wifi_manager_config_t config;
    char ssid[sizeof(((wifi_sta_config_t *)0)->ssid)];
    char password[sizeof(((wifi_sta_config_t *)0)->password)];

    SemaphoreHandle_t status_mutex;
    esp_timer_handle_t reconnect_timer;
    esp_timer_handle_t ap_shutdown_timer;
    esp_netif_t *station_netif;
    esp_netif_t *ap_netif;
    esp_event_handler_instance_t wifi_event_handler;
    esp_event_handler_instance_t ip_event_handler;

    wifi_manager_status_t status;
    uint32_t next_reconnect_delay_ms;
    char ap_ssid[33];
    char ap_password[65];
    bool owns_default_event_loop;
    bool owns_netif_stack;
    bool wifi_initialized;
    bool wifi_handler_registered;
    bool ip_handler_registered;
    bool started;
};

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 将 Wi-Fi 等待转换为至少一个 RTOS tick。
 */
static TickType_t wifi_manager_ms_to_ticks(uint32_t milliseconds)
{
    const TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    return (ticks > 0U) ? ticks : 1U;
}

/**
 * @brief 校验 Wi-Fi 参数与退避重连范围。
 */
static bool wifi_manager_config_is_valid(const wifi_manager_config_t *config)
{
    if ((config == NULL) || (config->ssid == NULL) || (config->password == NULL))
    {
        return false;
    }

    const size_t ssid_length = strlen(config->ssid);
    const size_t password_length = strlen(config->password);
    return (ssid_length < sizeof(((wifi_sta_config_t *)0)->ssid)) &&
           (password_length < sizeof(((wifi_sta_config_t *)0)->password)) &&
           (password_length == 0U || password_length >= 8U) &&
           (config->reconnect_initial_delay_ms >= WIFI_MANAGER_MIN_RECONNECT_DELAY_MS) &&
           (config->reconnect_initial_delay_ms <= config->reconnect_max_delay_ms) &&
           (config->reconnect_max_delay_ms <= WIFI_MANAGER_MAX_RECONNECT_DELAY_MS);
}

/**
 * @brief 生成当前候选凭据对应的 STA 配置。
 */
static wifi_config_t wifi_manager_station_config(const wifi_manager_handle_t handle)
{
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, handle->ssid, strlen(handle->ssid) + 1U);
    memcpy(config.sta.password, handle->password, strlen(handle->password) + 1U);
    config.sta.threshold.authmode =
        handle->password[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    return config;
}

/**
 * @brief 从 NVS 恢复已经确认有效的 Wi-Fi 凭据。
 */
static void wifi_manager_load_saved_credentials(wifi_manager_handle_t handle)
{
    nvs_handle_t nvs;
    if (nvs_open(WIFI_MANAGER_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
    {
        return;
    }
    uint8_t force_ap = 0U;
    if (nvs_get_u8(nvs, "force_ap", &force_ap) == ESP_OK && force_ap != 0U)
    {
        handle->ssid[0] = '\0';
        handle->password[0] = '\0';
        nvs_close(nvs);
        return;
    }
    char saved_ssid[sizeof(handle->ssid)] = {0};
    char saved_password[sizeof(handle->password)] = {0};
    size_t ssid_length = sizeof(saved_ssid);
    size_t password_length = sizeof(saved_password);
    if (nvs_get_str(nvs, "ssid", saved_ssid, &ssid_length) == ESP_OK &&
        nvs_get_str(nvs, "password", saved_password, &password_length) == ESP_OK &&
        saved_ssid[0] != '\0' && ssid_length <= sizeof(((wifi_sta_config_t *)0)->ssid) &&
        (password_length == 1U || password_length >= 9U))
    {
        memcpy(handle->ssid, saved_ssid, ssid_length);
        memcpy(handle->password, saved_password, password_length);
        ESP_LOGI(TAG, "Loaded Wi-Fi credentials from NVS for SSID '%s'", handle->ssid);
    }
    nvs_close(nvs);
}

/**
 * @brief 生成或恢复本机配网 AP 的名称与密码。
 */
static void wifi_manager_prepare_ap(wifi_manager_handle_t handle)
{
    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    (void)snprintf(handle->ap_ssid, sizeof(handle->ap_ssid), "ESP32-Gateway-%02X%02X%02X", mac[3],
                   mac[4], mac[5]);
    nvs_handle_t nvs;
    bool saved = false;
    if (nvs_open(WIFI_MANAGER_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK)
    {
        size_t length = sizeof(handle->ap_password);
        if (nvs_get_str(nvs, "ap_password", handle->ap_password, &length) == ESP_OK &&
            length >= 9U && length <= sizeof(((wifi_ap_config_t *)0)->password))
        {
            saved = true;
        }
        if (!saved)
        {
            (void)snprintf(handle->ap_password, sizeof(handle->ap_password), "GW%08lX",
                           (unsigned long)esp_random());
            if (nvs_set_str(nvs, "ap_password", handle->ap_password) == ESP_OK)
            {
                (void)nvs_commit(nvs);
            }
        }
        nvs_close(nvs);
    }
    if (handle->ap_password[0] == '\0')
    {
        (void)snprintf(handle->ap_password, sizeof(handle->ap_password), "GW%08lX",
                       (unsigned long)esp_random());
    }
}

/**
 * @brief 生成配网 AP 的 SDK 配置。
 */
static wifi_config_t wifi_manager_ap_config(const wifi_manager_handle_t handle)
{
    wifi_config_t config = {0};
    memcpy(config.ap.ssid, handle->ap_ssid, strlen(handle->ap_ssid) + 1U);
    config.ap.ssid_len = (uint8_t)strlen(handle->ap_ssid);
    memcpy(config.ap.password, handle->ap_password, strlen(handle->ap_password) + 1U);
    config.ap.channel = 1U;
    config.ap.max_connection = 4U;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.pmf_cfg.required = false;
    return config;
}

/**
 * @brief 提交已成功获得 IP 的候选凭据。
 */
static esp_err_t wifi_manager_save_credentials(const wifi_manager_handle_t handle)
{
    nvs_handle_t nvs;
    esp_err_t error = nvs_open(WIFI_MANAGER_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (error != ESP_OK)
    {
        return error;
    }
    error = nvs_set_str(nvs, "ssid", handle->ssid);
    if (error == ESP_OK)
    {
        error = nvs_set_str(nvs, "password", handle->password);
    }
    if (error == ESP_OK)
    {
        error = nvs_set_u8(nvs, "force_ap", 0U);
    }
    if (error == ESP_OK)
    {
        error = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "Wi-Fi credentials connected but NVS save failed: %s",
                 esp_err_to_name(error));
    }
    return error;
}

/**
 * @brief 更新受互斥保护的 Wi-Fi 状态。
 */
static void wifi_manager_set_connection_state(wifi_manager_handle_t handle,
                                              wifi_manager_state_t state, esp_err_t last_error)
{
    if (xSemaphoreTake(handle->status_mutex,
                       wifi_manager_ms_to_ticks(WIFI_MANAGER_LOCK_TIMEOUT_MS)) == pdTRUE)
    {
        handle->status.state = state;
        handle->status.last_error = last_error;
        (void)xSemaphoreGive(handle->status_mutex);
    }
}

/**
 * @brief 向 SDK 发起非阻塞 STA 连接。
 */
static esp_err_t wifi_manager_connect(wifi_manager_handle_t handle)
{
    if (handle->ssid[0] == '\0')
    {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_manager_set_connection_state(handle, WIFI_MANAGER_STATE_CONNECTING, ESP_OK);
    const esp_err_t result = esp_wifi_connect();
    if (result != ESP_OK)
    {
        wifi_manager_set_connection_state(handle, WIFI_MANAGER_STATE_WAITING_RETRY, result);
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(result));
    }
    return result;
}

/**
 * @brief 按有上限的退避策略安排下一次重连。
 */
static void wifi_manager_schedule_reconnect(wifi_manager_handle_t handle)
{
    uint32_t delay_ms = handle->config.reconnect_max_delay_ms;
    bool enable_ap = false;

    if (xSemaphoreTake(handle->status_mutex,
                       wifi_manager_ms_to_ticks(WIFI_MANAGER_LOCK_TIMEOUT_MS)) == pdTRUE)
    {
        delay_ms = handle->next_reconnect_delay_ms;
        handle->status.state = WIFI_MANAGER_STATE_WAITING_RETRY;
        ++handle->status.reconnect_attempts;
        enable_ap = handle->status.reconnect_attempts >= WIFI_MANAGER_AP_FALLBACK_ATTEMPTS &&
                    !handle->status.ap_active;
        if (handle->status.reconnect_attempts >= WIFI_MANAGER_AP_FALLBACK_ATTEMPTS)
        {
            handle->status.provisioning_pending = false;
        }

        if (handle->next_reconnect_delay_ms < handle->config.reconnect_max_delay_ms)
        {
            const uint64_t doubled_delay = (uint64_t)handle->next_reconnect_delay_ms * 2U;
            handle->next_reconnect_delay_ms =
                (doubled_delay < handle->config.reconnect_max_delay_ms)
                    ? (uint32_t)doubled_delay
                    : handle->config.reconnect_max_delay_ms;
        }
        (void)xSemaphoreGive(handle->status_mutex);
    }

    if (enable_ap)
    {
        const esp_err_t mode_result = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (mode_result == ESP_OK)
        {
            ESP_LOGW(TAG, "STA retries exceeded; provisioning AP %s enabled", handle->ap_ssid);
        }
    }

    if (esp_timer_is_active(handle->reconnect_timer))
    {
        (void)esp_timer_stop(handle->reconnect_timer);
    }

    const esp_err_t result =
        esp_timer_start_once(handle->reconnect_timer, (uint64_t)delay_ms * 1000U);
    if (result == ESP_OK)
    {
        ESP_LOGW(TAG, "Wi-Fi reconnect scheduled in %" PRIu32 " ms", delay_ms);
    }
    else
    {
        wifi_manager_set_connection_state(handle, WIFI_MANAGER_STATE_WAITING_RETRY, result);
        ESP_LOGE(TAG, "Failed to schedule Wi-Fi reconnect: %s", esp_err_to_name(result));
    }
}

/**
 * @brief 在定时器回调中发起下一次非阻塞连接。
 */
static void wifi_manager_reconnect_timer_callback(void *argument)
{
    wifi_manager_handle_t handle = (wifi_manager_handle_t)argument;
    if (wifi_manager_connect(handle) != ESP_OK)
    {
        wifi_manager_schedule_reconnect(handle);
    }
}

/**
 * @brief 成功联网后按策略关闭配网 AP。
 */
static void wifi_manager_ap_shutdown_timer_callback(void *argument)
{
    wifi_manager_handle_t handle = (wifi_manager_handle_t)argument;
    if (xSemaphoreTake(handle->status_mutex, 0U) != pdTRUE)
    {
        return;
    }
    const bool connected = handle->status.has_ipv4;
    xSemaphoreGive(handle->status_mutex);
    if (!connected)
    {
        return;
    }
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK)
    {
        ESP_LOGW(TAG, "Could not close provisioning AP after STA connection");
    }
}

/**
 * @brief 记录断线原因并清理 IPv4 就绪状态。
 */
static void wifi_manager_record_disconnection(wifi_manager_handle_t handle,
                                              const wifi_event_sta_disconnected_t *event)
{
    if (xSemaphoreTake(handle->status_mutex,
                       wifi_manager_ms_to_ticks(WIFI_MANAGER_LOCK_TIMEOUT_MS)) == pdTRUE)
    {
        handle->status.has_ipv4 = false;
        handle->status.ipv4_address[0] = '\0';
        handle->status.last_disconnect_reason = event->reason;
        handle->status.last_error = ESP_ERR_WIFI_NOT_CONNECT;
        (void)xSemaphoreGive(handle->status_mutex);
    }
}

/**
 * @brief 记录新获得的 IPv4 和连接状态。
 */
static void wifi_manager_record_got_ip(wifi_manager_handle_t handle, const ip_event_got_ip_t *event)
{
    if (xSemaphoreTake(handle->status_mutex,
                       wifi_manager_ms_to_ticks(WIFI_MANAGER_LOCK_TIMEOUT_MS)) == pdTRUE)
    {
        handle->status.state = WIFI_MANAGER_STATE_CONNECTED;
        handle->status.has_ipv4 = true;
        handle->status.reconnect_attempts = 0U;
        handle->status.last_disconnect_reason = 0U;
        handle->status.last_error = ESP_OK;
        (void)snprintf(handle->status.ipv4_address, sizeof(handle->status.ipv4_address), IPSTR,
                       IP2STR(&event->ip_info.ip));
        handle->next_reconnect_delay_ms = handle->config.reconnect_initial_delay_ms;
        (void)xSemaphoreGive(handle->status_mutex);
    }
}

/**
 * @brief 处理 AP/STA/IP 事件，安排重连和凭据提交。
 */
static void wifi_manager_event_handler(void *argument, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data)
{
    wifi_manager_handle_t handle = (wifi_manager_handle_t)argument;

    if ((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_STA_START))
    {
        if (handle->ssid[0] == '\0')
        {
            return;
        }
        wifi_manager_status_t status;
        if (wifi_manager_get_status(handle, &status) == ESP_OK && status.provisioning_pending)
        {
            if (xSemaphoreTake(handle->status_mutex,
                               wifi_manager_ms_to_ticks(WIFI_MANAGER_LOCK_TIMEOUT_MS)) != pdTRUE)
            {
                return;
            }
            wifi_config_t station_config = wifi_manager_station_config(handle);
            xSemaphoreGive(handle->status_mutex);
            const esp_err_t configured = esp_wifi_set_config(WIFI_IF_STA, &station_config);
            if (configured != ESP_OK)
            {
                ESP_LOGE(TAG, "Provisioned STA config failed: %s", esp_err_to_name(configured));
                return;
            }
        }
        ESP_LOGI(TAG, "Wi-Fi Station started; connecting to SSID '%s'", handle->ssid);
        if (wifi_manager_connect(handle) != ESP_OK)
        {
            wifi_manager_schedule_reconnect(handle);
        }
        return;
    }

    if ((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_STA_DISCONNECTED))
    {
        const wifi_event_sta_disconnected_t *event =
            (const wifi_event_sta_disconnected_t *)event_data;
        wifi_manager_record_disconnection(handle, event);
        ESP_LOGW(TAG, "Wi-Fi disconnected; reason=%u", event->reason);
        if (handle->ssid[0] != '\0')
        {
            wifi_manager_schedule_reconnect(handle);
        }
        return;
    }

    if ((event_base == WIFI_EVENT) &&
        (event_id == WIFI_EVENT_AP_START || event_id == WIFI_EVENT_AP_STOP))
    {
        if (xSemaphoreTake(handle->status_mutex,
                           wifi_manager_ms_to_ticks(WIFI_MANAGER_LOCK_TIMEOUT_MS)) == pdTRUE)
        {
            handle->status.ap_active = event_id == WIFI_EVENT_AP_START;
            if (event_id == WIFI_EVENT_AP_START && !handle->status.has_ipv4)
            {
                handle->status.state = WIFI_MANAGER_STATE_PROVISIONING_AP;
            }
            xSemaphoreGive(handle->status_mutex);
        }
        if (event_id == WIFI_EVENT_AP_START)
        {
            ESP_LOGI(TAG, "Provisioning AP: SSID=%s password=%s IP=192.168.4.1", handle->ap_ssid,
                     handle->ap_password);
        }
        return;
    }

    if ((event_base == IP_EVENT) && (event_id == IP_EVENT_STA_GOT_IP))
    {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        if (esp_timer_is_active(handle->reconnect_timer))
        {
            (void)esp_timer_stop(handle->reconnect_timer);
        }
        wifi_manager_record_got_ip(handle, event);
        wifi_manager_status_t status;
        if (wifi_manager_get_status(handle, &status) == ESP_OK)
        {
            bool close_ap = true;
            if (status.provisioning_pending)
            {
                close_ap = wifi_manager_save_credentials(handle) == ESP_OK;
                if (close_ap)
                {
                    if (xSemaphoreTake(handle->status_mutex,
                                       wifi_manager_ms_to_ticks(WIFI_MANAGER_LOCK_TIMEOUT_MS)) ==
                        pdTRUE)
                    {
                        handle->status.provisioning_pending = false;
                        xSemaphoreGive(handle->status_mutex);
                    }
                }
            }
            if (close_ap && status.ap_active && !esp_timer_is_active(handle->ap_shutdown_timer))
            {
                (void)esp_timer_start_once(handle->ap_shutdown_timer,
                                           WIFI_MANAGER_AP_SHUTDOWN_DELAY_US);
            }
        }
        ESP_LOGI(TAG, "Wi-Fi connected; IPv4=" IPSTR, IP2STR(&event->ip_info.ip));
    }
}

/**
 * @brief 释放未成功创建的 Wi-Fi 实例与事件资源。
 */
static void wifi_manager_release_partial_instance(wifi_manager_handle_t handle)
{
    if (handle == NULL)
    {
        return;
    }

    if (handle->ip_handler_registered)
    {
        (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                    handle->ip_event_handler);
    }
    if (handle->wifi_handler_registered)
    {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                    handle->wifi_event_handler);
    }
    if (handle->wifi_initialized)
    {
        (void)esp_wifi_deinit();
    }
    if (handle->station_netif != NULL)
    {
        esp_netif_destroy_default_wifi(handle->station_netif);
    }
    if (handle->ap_netif != NULL)
    {
        esp_netif_destroy_default_wifi(handle->ap_netif);
    }
    if (handle->owns_default_event_loop)
    {
        (void)esp_event_loop_delete_default();
    }
    if (handle->owns_netif_stack)
    {
        (void)esp_netif_deinit();
    }
    if (handle->reconnect_timer != NULL)
    {
        (void)esp_timer_delete(handle->reconnect_timer);
    }
    if (handle->ap_shutdown_timer != NULL)
    {
        (void)esp_timer_delete(handle->ap_shutdown_timer);
    }
    if (handle->status_mutex != NULL)
    {
        vSemaphoreDelete(handle->status_mutex);
    }
    free(handle);
}

/**
 * @brief 创建Wi-Fi 配网与重连服务实例并准备所需资源。
 * @param[in] config 模块配置及其取值约束。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t wifi_manager_create(const wifi_manager_config_t *config,
                              wifi_manager_handle_t *out_handle)
{
    if (!wifi_manager_config_is_valid(config) || (out_handle == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;

    wifi_manager_handle_t handle = calloc(1U, sizeof(*handle));
    if (handle == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    handle->config = *config;
    (void)memcpy(handle->ssid, config->ssid, strlen(config->ssid) + 1U);
    (void)memcpy(handle->password, config->password, strlen(config->password) + 1U);
    handle->config.ssid = handle->ssid;
    handle->config.password = handle->password;
    wifi_manager_load_saved_credentials(handle);
    handle->status.state = WIFI_MANAGER_STATE_CREATED;
    handle->status.last_error = ESP_OK;
    handle->next_reconnect_delay_ms = config->reconnect_initial_delay_ms;

    handle->status_mutex = xSemaphoreCreateMutex();
    if (handle->status_mutex == NULL)
    {
        wifi_manager_release_partial_instance(handle);
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t timer_config = {
        .callback = wifi_manager_reconnect_timer_callback,
        .arg = handle,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_reconnect",
        .skip_unhandled_events = true,
    };
    esp_err_t result = esp_timer_create(&timer_config, &handle->reconnect_timer);
    if (result != ESP_OK)
    {
        wifi_manager_release_partial_instance(handle);
        return result;
    }
    const esp_timer_create_args_t ap_timer_config = {
        .callback = wifi_manager_ap_shutdown_timer_callback,
        .arg = handle,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_ap_close",
        .skip_unhandled_events = true,
    };
    result = esp_timer_create(&ap_timer_config, &handle->ap_shutdown_timer);
    if (result != ESP_OK)
    {
        wifi_manager_release_partial_instance(handle);
        return result;
    }

    result = esp_netif_init();
    if (result == ESP_OK)
    {
        handle->owns_netif_stack = true;
    }
    else if (result != ESP_ERR_INVALID_STATE)
    {
        wifi_manager_release_partial_instance(handle);
        return result;
    }

    result = esp_event_loop_create_default();
    if (result == ESP_OK)
    {
        handle->owns_default_event_loop = true;
    }
    else if (result != ESP_ERR_INVALID_STATE)
    {
        wifi_manager_release_partial_instance(handle);
        return result;
    }

    handle->station_netif = esp_netif_create_default_wifi_sta();
    if (handle->station_netif == NULL)
    {
        wifi_manager_release_partial_instance(handle);
        return ESP_ERR_NO_MEM;
    }
    handle->ap_netif = esp_netif_create_default_wifi_ap();
    if (handle->ap_netif == NULL)
    {
        wifi_manager_release_partial_instance(handle);
        return ESP_ERR_NO_MEM;
    }

    const wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&wifi_init_config);
    if (result != ESP_OK)
    {
        wifi_manager_release_partial_instance(handle);
        return result;
    }
    handle->wifi_initialized = true;
    wifi_manager_prepare_ap(handle);

    result = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                 wifi_manager_event_handler, handle,
                                                 &handle->wifi_event_handler);
    if (result != ESP_OK)
    {
        wifi_manager_release_partial_instance(handle);
        return result;
    }
    handle->wifi_handler_registered = true;

    result = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                 wifi_manager_event_handler, handle,
                                                 &handle->ip_event_handler);
    if (result != ESP_OK)
    {
        wifi_manager_release_partial_instance(handle);
        return result;
    }
    handle->ip_handler_registered = true;

    wifi_config_t station_config = wifi_manager_station_config(handle);
    wifi_config_t ap_config = wifi_manager_ap_config(handle);

    result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result == ESP_OK)
    {
        result = esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
    if (result == ESP_OK && handle->ssid[0] != '\0')
    {
        result = esp_wifi_set_config(WIFI_IF_STA, &station_config);
    }
    if (result == ESP_OK)
    {
        result = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    }
    if (result == ESP_OK)
    {
        result = esp_wifi_set_mode(handle->ssid[0] == '\0' ? WIFI_MODE_AP : WIFI_MODE_STA);
    }
    if (result != ESP_OK)
    {
        wifi_manager_release_partial_instance(handle);
        return result;
    }

    *out_handle = handle;
    return ESP_OK;
}

/**
 * @brief 启动Wi-Fi 配网与重连服务的运行入口。
 * @param[in] handle 已创建的模块实例句柄。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t wifi_manager_start(wifi_manager_handle_t handle)
{
    if (handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->started)
    {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_manager_set_connection_state(handle, WIFI_MANAGER_STATE_STARTING, ESP_OK);
    const esp_err_t result = esp_wifi_start();
    if (result != ESP_OK)
    {
        wifi_manager_set_connection_state(handle, WIFI_MANAGER_STATE_CREATED, result);
        return result;
    }

    handle->started = true;
    return ESP_OK;
}

/**
 * @brief 复制Wi-Fi 配网与重连服务的当前状态。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_status 接收复制出的完整状态或配置。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t wifi_manager_get_status(wifi_manager_handle_t handle, wifi_manager_status_t *out_status)
{
    if ((handle == NULL) || (out_status == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(handle->status_mutex,
                       wifi_manager_ms_to_ticks(WIFI_MANAGER_LOCK_TIMEOUT_MS)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    *out_status = handle->status;
    (void)xSemaphoreGive(handle->status_mutex);
    return ESP_OK;
}

/**
 * @brief 接收候选凭据并发起连接，暂不提前保存为有效配置。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[in] ssid 接口使用的名称、文本或配置内容，长度受对应模块约束。
 * @param[in] password 接口使用的名称、文本或配置内容，长度受对应模块约束。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t wifi_manager_submit_credentials(wifi_manager_handle_t handle, const char *ssid,
                                          const char *password)
{
    if (handle == NULL || ssid == NULL || password == NULL || strlen(ssid) == 0U ||
        strlen(ssid) >= sizeof(((wifi_sta_config_t *)0)->ssid) ||
        strlen(password) >= sizeof(handle->password) ||
        (strlen(password) > 0U && strlen(password) < 8U))
    {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_manager_status_t status;
    if (wifi_manager_get_status(handle, &status) != ESP_OK || !status.ap_active ||
        status.has_ipv4 || status.provisioning_pending)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(handle->status_mutex,
                       wifi_manager_ms_to_ticks(WIFI_MANAGER_LOCK_TIMEOUT_MS)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    memcpy(handle->ssid, ssid, strlen(ssid) + 1U);
    memcpy(handle->password, password, strlen(password) + 1U);
    handle->status.provisioning_pending = true;
    handle->status.reconnect_attempts = 0U;
    handle->next_reconnect_delay_ms = handle->config.reconnect_initial_delay_ms;
    xSemaphoreGive(handle->status_mutex);
    if (esp_timer_is_active(handle->reconnect_timer))
    {
        (void)esp_timer_stop(handle->reconnect_timer);
    }
    wifi_mode_t mode;
    esp_err_t error = esp_wifi_get_mode(&mode);
    if (error != ESP_OK)
    {
        return error;
    }
    if (mode == WIFI_MODE_AP)
    {
        /* STA_START 回调将在 APSTA 模式生效后配置新凭据并连接。 */
        return esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
    wifi_config_t station = wifi_manager_station_config(handle);
    error = esp_wifi_set_config(WIFI_IF_STA, &station);
    if (error == ESP_OK)
    {
        error = wifi_manager_connect(handle);
    }
    return error;
}

/**
 * @brief 清除已保存凭据并返回 AP 配网状态。
 * @param[in] handle 已创建的模块实例句柄。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t wifi_manager_clear_credentials(wifi_manager_handle_t handle)
{
    if (handle == NULL || !handle->started)
    {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t nvs;
    esp_err_t error = nvs_open(WIFI_MANAGER_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (error != ESP_OK)
    {
        return error;
    }
    error = nvs_erase_key(nvs, "ssid");
    if (error == ESP_ERR_NVS_NOT_FOUND)
    {
        error = ESP_OK;
    }
    if (error == ESP_OK)
    {
        error = nvs_erase_key(nvs, "password");
        if (error == ESP_ERR_NVS_NOT_FOUND)
        {
            error = ESP_OK;
        }
    }
    if (error == ESP_OK)
    {
        error = nvs_set_u8(nvs, "force_ap", 1U);
    }
    if (error == ESP_OK)
    {
        error = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (error != ESP_OK)
    {
        return error;
    }
    if (xSemaphoreTake(handle->status_mutex,
                       wifi_manager_ms_to_ticks(WIFI_MANAGER_LOCK_TIMEOUT_MS)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    handle->ssid[0] = '\0';
    handle->password[0] = '\0';
    handle->status.provisioning_pending = false;
    xSemaphoreGive(handle->status_mutex);
    if (esp_timer_is_active(handle->reconnect_timer))
    {
        (void)esp_timer_stop(handle->reconnect_timer);
    }
    return esp_wifi_set_mode(WIFI_MODE_AP);
}

/**
 * @brief 返回 Wi-Fi 连接阶段的可读名称。
 * @param[in] state 将要发布的连接或事务阶段。
 * @return 只读文本指针，调用方不释放。
 */
const char *wifi_manager_state_to_string(wifi_manager_state_t state)
{
    switch (state)
    {
        case WIFI_MANAGER_STATE_CREATED:
            return "created";
        case WIFI_MANAGER_STATE_STARTING:
            return "starting";
        case WIFI_MANAGER_STATE_CONNECTING:
            return "connecting";
        case WIFI_MANAGER_STATE_WAITING_RETRY:
            return "waiting_retry";
        case WIFI_MANAGER_STATE_CONNECTED:
            return "connected";
        case WIFI_MANAGER_STATE_PROVISIONING_AP:
            return "provisioning_ap";
        default:
            return "unknown";
    }
}
