/**
 * @file wifi_manager.c
 * @brief Wi-Fi Station 状态机与有上限的指数退避重连实现。
 */

#include "wifi_manager.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define WIFI_MANAGER_MIN_RECONNECT_DELAY_MS (100U)
#define WIFI_MANAGER_MAX_RECONNECT_DELAY_MS (300000U)
#define WIFI_MANAGER_LOCK_TIMEOUT_MS         (100U)

static const char *TAG = "wifi_manager";

struct wifi_manager {
    wifi_manager_config_t config;
    char ssid[sizeof(((wifi_sta_config_t *)0)->ssid)];
    char password[sizeof(((wifi_sta_config_t *)0)->password)];

    SemaphoreHandle_t status_mutex;
    esp_timer_handle_t reconnect_timer;
    esp_netif_t *station_netif;
    esp_event_handler_instance_t wifi_event_handler;
    esp_event_handler_instance_t ip_event_handler;

    wifi_manager_status_t status;
    uint32_t next_reconnect_delay_ms;
    bool owns_default_event_loop;
    bool owns_netif_stack;
    bool wifi_initialized;
    bool wifi_handler_registered;
    bool ip_handler_registered;
    bool started;
};

static TickType_t wifi_manager_ms_to_ticks(uint32_t milliseconds)
{
    const TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    return (ticks > 0U) ? ticks : 1U;
}

static bool wifi_manager_config_is_valid(const wifi_manager_config_t *config)
{
    if ((config == NULL) || (config->ssid == NULL) || (config->password == NULL)) {
        return false;
    }

    const size_t ssid_length = strlen(config->ssid);
    const size_t password_length = strlen(config->password);
    return (ssid_length > 0U) &&
           (ssid_length < sizeof(((wifi_sta_config_t *)0)->ssid)) &&
           (password_length < sizeof(((wifi_sta_config_t *)0)->password)) &&
           (config->reconnect_initial_delay_ms >= WIFI_MANAGER_MIN_RECONNECT_DELAY_MS) &&
           (config->reconnect_initial_delay_ms <= config->reconnect_max_delay_ms) &&
           (config->reconnect_max_delay_ms <= WIFI_MANAGER_MAX_RECONNECT_DELAY_MS);
}

static void wifi_manager_set_connection_state(wifi_manager_handle_t handle,
                                              wifi_manager_state_t state,
                                              esp_err_t last_error)
{
    if (xSemaphoreTake(handle->status_mutex,
                       wifi_manager_ms_to_ticks(WIFI_MANAGER_LOCK_TIMEOUT_MS)) == pdTRUE) {
        handle->status.state = state;
        handle->status.last_error = last_error;
        (void)xSemaphoreGive(handle->status_mutex);
    }
}

static esp_err_t wifi_manager_connect(wifi_manager_handle_t handle)
{
    wifi_manager_set_connection_state(handle,
                                      WIFI_MANAGER_STATE_CONNECTING,
                                      ESP_OK);
    const esp_err_t result = esp_wifi_connect();
    if (result != ESP_OK) {
        wifi_manager_set_connection_state(handle,
                                          WIFI_MANAGER_STATE_WAITING_RETRY,
                                          result);
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(result));
    }
    return result;
}

static void wifi_manager_schedule_reconnect(wifi_manager_handle_t handle)
{
    uint32_t delay_ms = handle->config.reconnect_max_delay_ms;

    if (xSemaphoreTake(handle->status_mutex,
                       wifi_manager_ms_to_ticks(WIFI_MANAGER_LOCK_TIMEOUT_MS)) == pdTRUE) {
        delay_ms = handle->next_reconnect_delay_ms;
        handle->status.state = WIFI_MANAGER_STATE_WAITING_RETRY;
        ++handle->status.reconnect_attempts;

        if (handle->next_reconnect_delay_ms < handle->config.reconnect_max_delay_ms) {
            const uint64_t doubled_delay = (uint64_t)handle->next_reconnect_delay_ms * 2U;
            handle->next_reconnect_delay_ms =
                (doubled_delay < handle->config.reconnect_max_delay_ms)
                    ? (uint32_t)doubled_delay
                    : handle->config.reconnect_max_delay_ms;
        }
        (void)xSemaphoreGive(handle->status_mutex);
    }

    if (esp_timer_is_active(handle->reconnect_timer)) {
        (void)esp_timer_stop(handle->reconnect_timer);
    }

    const esp_err_t result =
        esp_timer_start_once(handle->reconnect_timer, (uint64_t)delay_ms * 1000U);
    if (result == ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi reconnect scheduled in %" PRIu32 " ms", delay_ms);
    } else {
        wifi_manager_set_connection_state(handle,
                                          WIFI_MANAGER_STATE_WAITING_RETRY,
                                          result);
        ESP_LOGE(TAG, "Failed to schedule Wi-Fi reconnect: %s",
                 esp_err_to_name(result));
    }
}

/** 定时器回调只发起非阻塞连接，不在 esp_timer 任务中执行等待。 */
static void wifi_manager_reconnect_timer_callback(void *argument)
{
    wifi_manager_handle_t handle = (wifi_manager_handle_t)argument;
    if (wifi_manager_connect(handle) != ESP_OK) {
        wifi_manager_schedule_reconnect(handle);
    }
}

static void wifi_manager_record_disconnection(
    wifi_manager_handle_t handle,
    const wifi_event_sta_disconnected_t *event)
{
    if (xSemaphoreTake(handle->status_mutex,
                       wifi_manager_ms_to_ticks(WIFI_MANAGER_LOCK_TIMEOUT_MS)) == pdTRUE) {
        handle->status.has_ipv4 = false;
        handle->status.ipv4_address[0] = '\0';
        handle->status.last_disconnect_reason = event->reason;
        handle->status.last_error = ESP_ERR_WIFI_NOT_CONNECT;
        (void)xSemaphoreGive(handle->status_mutex);
    }
}

static void wifi_manager_record_got_ip(wifi_manager_handle_t handle,
                                       const ip_event_got_ip_t *event)
{
    if (xSemaphoreTake(handle->status_mutex,
                       wifi_manager_ms_to_ticks(WIFI_MANAGER_LOCK_TIMEOUT_MS)) == pdTRUE) {
        handle->status.state = WIFI_MANAGER_STATE_CONNECTED;
        handle->status.has_ipv4 = true;
        handle->status.reconnect_attempts = 0U;
        handle->status.last_disconnect_reason = 0U;
        handle->status.last_error = ESP_OK;
        (void)snprintf(handle->status.ipv4_address,
                       sizeof(handle->status.ipv4_address),
                       IPSTR,
                       IP2STR(&event->ip_info.ip));
        handle->next_reconnect_delay_ms = handle->config.reconnect_initial_delay_ms;
        (void)xSemaphoreGive(handle->status_mutex);
    }
}

/** ESP-IDF 系统事件回调只更新状态和安排工作，禁止在这里阻塞。 */
static void wifi_manager_event_handler(void *argument,
                                       esp_event_base_t event_base,
                                       int32_t event_id,
                                       void *event_data)
{
    wifi_manager_handle_t handle = (wifi_manager_handle_t)argument;

    if ((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_STA_START)) {
        ESP_LOGI(TAG, "Wi-Fi Station started; connecting to SSID '%s'",
                 handle->ssid);
        if (wifi_manager_connect(handle) != ESP_OK) {
            wifi_manager_schedule_reconnect(handle);
        }
        return;
    }

    if ((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_STA_DISCONNECTED)) {
        const wifi_event_sta_disconnected_t *event =
            (const wifi_event_sta_disconnected_t *)event_data;
        wifi_manager_record_disconnection(handle, event);
        ESP_LOGW(TAG, "Wi-Fi disconnected; reason=%u", event->reason);
        wifi_manager_schedule_reconnect(handle);
        return;
    }

    if ((event_base == IP_EVENT) && (event_id == IP_EVENT_STA_GOT_IP)) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        if (esp_timer_is_active(handle->reconnect_timer)) {
            (void)esp_timer_stop(handle->reconnect_timer);
        }
        wifi_manager_record_got_ip(handle, event);
        ESP_LOGI(TAG, "Wi-Fi connected; IPv4=" IPSTR,
                 IP2STR(&event->ip_info.ip));
    }
}

static void wifi_manager_release_partial_instance(wifi_manager_handle_t handle)
{
    if (handle == NULL) {
        return;
    }

    if (handle->ip_handler_registered) {
        (void)esp_event_handler_instance_unregister(IP_EVENT,
                                                    IP_EVENT_STA_GOT_IP,
                                                    handle->ip_event_handler);
    }
    if (handle->wifi_handler_registered) {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT,
                                                    ESP_EVENT_ANY_ID,
                                                    handle->wifi_event_handler);
    }
    if (handle->wifi_initialized) {
        (void)esp_wifi_deinit();
    }
    if (handle->station_netif != NULL) {
        esp_netif_destroy_default_wifi(handle->station_netif);
    }
    if (handle->owns_default_event_loop) {
        (void)esp_event_loop_delete_default();
    }
    if (handle->owns_netif_stack) {
        (void)esp_netif_deinit();
    }
    if (handle->reconnect_timer != NULL) {
        (void)esp_timer_delete(handle->reconnect_timer);
    }
    if (handle->status_mutex != NULL) {
        vSemaphoreDelete(handle->status_mutex);
    }
    free(handle);
}

esp_err_t wifi_manager_create(const wifi_manager_config_t *config,
                              wifi_manager_handle_t *out_handle)
{
    if (!wifi_manager_config_is_valid(config) || (out_handle == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;

    wifi_manager_handle_t handle = calloc(1U, sizeof(*handle));
    if (handle == NULL) {
        return ESP_ERR_NO_MEM;
    }

    handle->config = *config;
    (void)memcpy(handle->ssid, config->ssid, strlen(config->ssid) + 1U);
    (void)memcpy(handle->password, config->password, strlen(config->password) + 1U);
    handle->config.ssid = handle->ssid;
    handle->config.password = handle->password;
    handle->status.state = WIFI_MANAGER_STATE_CREATED;
    handle->status.last_error = ESP_OK;
    handle->next_reconnect_delay_ms = config->reconnect_initial_delay_ms;

    handle->status_mutex = xSemaphoreCreateMutex();
    if (handle->status_mutex == NULL) {
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
    if (result != ESP_OK) {
        wifi_manager_release_partial_instance(handle);
        return result;
    }

    result = esp_netif_init();
    if (result == ESP_OK) {
        handle->owns_netif_stack = true;
    } else if (result != ESP_ERR_INVALID_STATE) {
        wifi_manager_release_partial_instance(handle);
        return result;
    }

    result = esp_event_loop_create_default();
    if (result == ESP_OK) {
        handle->owns_default_event_loop = true;
    } else if (result != ESP_ERR_INVALID_STATE) {
        wifi_manager_release_partial_instance(handle);
        return result;
    }

    handle->station_netif = esp_netif_create_default_wifi_sta();
    if (handle->station_netif == NULL) {
        wifi_manager_release_partial_instance(handle);
        return ESP_ERR_NO_MEM;
    }

    const wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&wifi_init_config);
    if (result != ESP_OK) {
        wifi_manager_release_partial_instance(handle);
        return result;
    }
    handle->wifi_initialized = true;

    result = esp_event_handler_instance_register(WIFI_EVENT,
                                                 ESP_EVENT_ANY_ID,
                                                 wifi_manager_event_handler,
                                                 handle,
                                                 &handle->wifi_event_handler);
    if (result != ESP_OK) {
        wifi_manager_release_partial_instance(handle);
        return result;
    }
    handle->wifi_handler_registered = true;

    result = esp_event_handler_instance_register(IP_EVENT,
                                                 IP_EVENT_STA_GOT_IP,
                                                 wifi_manager_event_handler,
                                                 handle,
                                                 &handle->ip_event_handler);
    if (result != ESP_OK) {
        wifi_manager_release_partial_instance(handle);
        return result;
    }
    handle->ip_handler_registered = true;

    wifi_config_t station_config = {0};
    (void)memcpy(station_config.sta.ssid,
                 handle->ssid,
                 strlen(handle->ssid) + 1U);
    (void)memcpy(station_config.sta.password,
                 handle->password,
                 strlen(handle->password) + 1U);
    station_config.sta.threshold.authmode =
        (handle->password[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    station_config.sta.pmf_cfg.capable = true;
    station_config.sta.pmf_cfg.required = false;
    station_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result == ESP_OK) {
        result = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (result == ESP_OK) {
        result = esp_wifi_set_config(WIFI_IF_STA, &station_config);
    }
    if (result != ESP_OK) {
        wifi_manager_release_partial_instance(handle);
        return result;
    }

    *out_handle = handle;
    return ESP_OK;
}

esp_err_t wifi_manager_start(wifi_manager_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->started) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_manager_set_connection_state(handle,
                                      WIFI_MANAGER_STATE_STARTING,
                                      ESP_OK);
    const esp_err_t result = esp_wifi_start();
    if (result != ESP_OK) {
        wifi_manager_set_connection_state(handle,
                                          WIFI_MANAGER_STATE_CREATED,
                                          result);
        return result;
    }

    handle->started = true;
    return ESP_OK;
}

esp_err_t wifi_manager_get_status(wifi_manager_handle_t handle,
                                  wifi_manager_status_t *out_status)
{
    if ((handle == NULL) || (out_status == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(handle->status_mutex,
                       wifi_manager_ms_to_ticks(WIFI_MANAGER_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *out_status = handle->status;
    (void)xSemaphoreGive(handle->status_mutex);
    return ESP_OK;
}

const char *wifi_manager_state_to_string(wifi_manager_state_t state)
{
    switch (state) {
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
        default:
            return "unknown";
    }
}
