/**
 ******************************************************************************
 * @file    mqtt_service.c
 * @brief   MQTT 上报与远程控制服务实现。
 * @details 协议回调只重组/投递；业务任务负责遥测、事件、会话和确认处理。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：业务服务层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "mqtt_service.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"

/*--------------------------- 模块配置、类型与状态 ---------------------------*/

#define MQTT_SERVICE_BROKER_URI_LENGTH    (256U)
#define MQTT_SERVICE_USERNAME_LENGTH      (128U)
#define MQTT_SERVICE_PASSWORD_LENGTH      (128U)
#define MQTT_SERVICE_TOPIC_LENGTH         (96U)
#define MQTT_SERVICE_CLIENT_ID_LENGTH     (40U)
#define MQTT_SERVICE_COMMAND_MAX_LENGTH   (256U)
#define MQTT_SERVICE_COMMAND_QUEUE_LENGTH (4U)
#define MQTT_SERVICE_ACK_QUEUE_LENGTH     (16U)
#define MQTT_SERVICE_PUBACK_TIMEOUT_MS    (120000U)
#define MQTT_SERVICE_FAULT_HEARTBEAT_MS   (60000U)
#define MQTT_SERVICE_TASK_STACK_SIZE      (6144U)
#define MQTT_SERVICE_TASK_PRIORITY        (5U)
#define MQTT_SERVICE_LOOP_DELAY_MS        (100U)
#define MQTT_SERVICE_NETWORK_POLL_MS      (500U)
#define MQTT_SERVICE_LOCK_TIMEOUT_MS      (100U)
#define MQTT_SERVICE_CONNECTED_BIT        BIT0
#define MQTT_SERVICE_SESSION_SETUP_BIT    BIT1

static const char *TAG = "mqtt_service";
static const char *MQTT_ONLINE_PAYLOAD = "{\"online\":true}";
static const char *MQTT_OFFLINE_PAYLOAD = "{\"online\":false}";

typedef struct
{
    char payload[MQTT_SERVICE_COMMAND_MAX_LENGTH + 1U];
    size_t length;
} mqtt_service_command_t;

typedef struct
{
    int msg_id;
    uint32_t session_generation;
} mqtt_service_puback_t;

struct mqtt_service
{
    mqtt_service_config_t config;
    char broker_uri[MQTT_SERVICE_BROKER_URI_LENGTH];
    char username[MQTT_SERVICE_USERNAME_LENGTH];
    char password[MQTT_SERVICE_PASSWORD_LENGTH];

    char device_id[13U];
    char client_id[MQTT_SERVICE_CLIENT_ID_LENGTH];
    char telemetry_topic[MQTT_SERVICE_TOPIC_LENGTH];
    char status_topic[MQTT_SERVICE_TOPIC_LENGTH];
    char config_set_topic[MQTT_SERVICE_TOPIC_LENGTH];
    char config_state_topic[MQTT_SERVICE_TOPIC_LENGTH];

    esp_mqtt_client_handle_t client;
    TaskHandle_t task;
    QueueHandle_t command_queue;
    QueueHandle_t publish_ack_queue;
    EventGroupHandle_t event_group;
    SemaphoreHandle_t status_mutex;
    mqtt_service_status_t status;

    char incoming_command[MQTT_SERVICE_COMMAND_MAX_LENGTH + 1U];
    uint32_t boot_nonce;
    size_t incoming_expected_length;
    size_t incoming_received_length;
    bool incoming_command_active;
    bool telemetry_inflight;
    int telemetry_msg_id;
    uint32_t telemetry_session_generation;
    uint32_t telemetry_record_id;
    TickType_t telemetry_sent_tick;
    TickType_t last_publish_error_tick;
    bool publish_backoff_active;
    bool last_offline_fault_valid;
    gateway_outbox_record_t last_offline_fault;
    TickType_t last_offline_fault_tick;
    volatile uint32_t mqtt_session_generation;
    bool started;
};

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 将 MQTT 业务等待转换为至少一个 RTOS tick。
 */
static TickType_t mqtt_service_ms_to_ticks(uint32_t milliseconds)
{
    const TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    return (ticks > 0U) ? ticks : 1U;
}

/**
 * @brief 检查 MQTT 参数字符串是否能放入内部缓冲。
 */
static bool mqtt_service_string_fits(const char *value, size_t capacity)
{
    return (value != NULL) && (strlen(value) < capacity);
}

/**
 * @brief 校验 Broker、回调和持久队列依赖。
 */
static bool mqtt_service_config_is_valid(const mqtt_service_config_t *config)
{
    return (config != NULL) &&
           mqtt_service_string_fits(config->broker_uri, MQTT_SERVICE_BROKER_URI_LENGTH) &&
           (config->broker_uri[0] != '\0') &&
           mqtt_service_string_fits(config->username, MQTT_SERVICE_USERNAME_LENGTH) &&
           mqtt_service_string_fits(config->password, MQTT_SERVICE_PASSWORD_LENGTH) &&
           (config->outbox != NULL) && (config->snapshot_context != NULL) &&
           (config->get_snapshot != NULL) && (config->runtime_config_context != NULL) &&
           (config->get_runtime_config != NULL) && (config->apply_runtime_config != NULL) &&
           (config->network_context != NULL) && (config->is_network_ready != NULL);
}

/**
 * @brief 更新 MQTT 连接阶段和最近错误。
 */
static void mqtt_service_set_state(mqtt_service_handle_t handle, mqtt_service_state_t state,
                                   bool connected, esp_err_t last_error)
{
    if (xSemaphoreTake(handle->status_mutex,
                       mqtt_service_ms_to_ticks(MQTT_SERVICE_LOCK_TIMEOUT_MS)) == pdTRUE)
    {
        handle->status.state = state;
        handle->status.connected = connected;
        handle->status.last_error = last_error;
        (void)xSemaphoreGive(handle->status_mutex);
    }
}

/**
 * @brief 检查采样与上报周期是否位于合法范围。
 */
static bool mqtt_service_runtime_config_is_valid(const mqtt_runtime_config_t *config)
{
    return (config != NULL) && (config->sample_period_ms >= 1000U) &&
           (config->sample_period_ms <= 600000U) &&
           (config->report_period_ms >= MQTT_SERVICE_MIN_REPORT_PERIOD_MS) &&
           (config->report_period_ms <= MQTT_SERVICE_MAX_REPORT_PERIOD_MS);
}

/**
 * @brief 通过应用回调读取实际运行配置并更新诊断周期。
 */
static esp_err_t mqtt_service_read_runtime_config(mqtt_service_handle_t handle,
                                                  mqtt_runtime_config_t *out_config)
{
    const esp_err_t result =
        handle->config.get_runtime_config(handle->config.runtime_config_context, out_config);
    if ((result != ESP_OK) || !mqtt_service_runtime_config_is_valid(out_config))
    {
        return (result != ESP_OK) ? result : ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(handle->status_mutex,
                       mqtt_service_ms_to_ticks(MQTT_SERVICE_LOCK_TIMEOUT_MS)) == pdTRUE)
    {
        handle->status.report_period_ms = out_config->report_period_ms;
        (void)xSemaphoreGive(handle->status_mutex);
    }
    return ESP_OK;
}

/**
 * @brief 读取上报周期，读取失败时使用已保存的诊断值。
 */
static uint32_t mqtt_service_get_report_period(mqtt_service_handle_t handle)
{
    mqtt_runtime_config_t runtime_config;
    if (mqtt_service_read_runtime_config(handle, &runtime_config) == ESP_OK)
    {
        return runtime_config.report_period_ms;
    }

    uint32_t period_ms = MQTT_SERVICE_MIN_REPORT_PERIOD_MS;
    if (xSemaphoreTake(handle->status_mutex,
                       mqtt_service_ms_to_ticks(MQTT_SERVICE_LOCK_TIMEOUT_MS)) == pdTRUE)
    {
        period_ms = handle->status.report_period_ms;
        (void)xSemaphoreGive(handle->status_mutex);
    }
    return period_ms;
}

/**
 * @brief 累计发送尝试及最近发布错误。
 */
static void mqtt_service_record_publish(mqtt_service_handle_t handle, esp_err_t result)
{
    if (xSemaphoreTake(handle->status_mutex,
                       mqtt_service_ms_to_ticks(MQTT_SERVICE_LOCK_TIMEOUT_MS)) == pdTRUE)
    {
        if (result == ESP_OK)
        {
            ++handle->status.published_telemetry_count;
        }
        else
        {
            handle->status.last_error = result;
        }
        (void)xSemaphoreGive(handle->status_mutex);
    }
}

/**
 * @brief 累计遥测持久化结果。
 */
static void mqtt_service_record_capture(mqtt_service_handle_t handle, esp_err_t result)
{
    if (xSemaphoreTake(handle->status_mutex,
                       mqtt_service_ms_to_ticks(MQTT_SERVICE_LOCK_TIMEOUT_MS)) == pdTRUE)
    {
        if (result == ESP_OK)
        {
            ++handle->status.stored_telemetry_count;
        }
        else
        {
            handle->status.last_error = result;
        }
        xSemaphoreGive(handle->status_mutex);
    }
}

/**
 * @brief 累计被拒绝的 MQTT 控制请求。
 */
static void mqtt_service_record_rejected_command(mqtt_service_handle_t handle)
{
    if (xSemaphoreTake(handle->status_mutex,
                       mqtt_service_ms_to_ticks(MQTT_SERVICE_LOCK_TIMEOUT_MS)) == pdTRUE)
    {
        ++handle->status.rejected_command_count;
        handle->status.last_error = ESP_ERR_INVALID_ARG;
        (void)xSemaphoreGive(handle->status_mutex);
    }
}

/**
 * @brief 累计获得现场执行确认的控制请求。
 */
static void mqtt_service_record_field_applied(mqtt_service_handle_t handle)
{
    if (xSemaphoreTake(handle->status_mutex,
                       mqtt_service_ms_to_ticks(MQTT_SERVICE_LOCK_TIMEOUT_MS)) == pdTRUE)
    {
        ++handle->status.field_applied_command_count;
        xSemaphoreGive(handle->status_mutex);
    }
}

/**
 * @brief 累计匹配的 Broker 发布确认。
 */
static void mqtt_service_record_broker_ack(mqtt_service_handle_t handle)
{
    if (xSemaphoreTake(handle->status_mutex,
                       mqtt_service_ms_to_ticks(MQTT_SERVICE_LOCK_TIMEOUT_MS)) == pdTRUE)
    {
        ++handle->status.broker_ack_count;
        (void)xSemaphoreGive(handle->status_mutex);
    }
}

/**
 * @brief 按显式长度匹配收到的 MQTT 主题。
 */
static bool mqtt_service_topic_matches(const esp_mqtt_event_handle_t event,
                                       const char *expected_topic)
{
    const size_t expected_length = strlen(expected_topic);
    return (event->topic != NULL) && (event->topic_len == (int)expected_length) &&
           (memcmp(event->topic, expected_topic, expected_length) == 0);
}

/**
 * @brief 重组有界 MQTT 分片，只投递完整且非 retain 的命令。
 */
static void mqtt_service_receive_command_fragment(mqtt_service_handle_t handle,
                                                  const esp_mqtt_event_handle_t event)
{
    if (event->current_data_offset == 0)
    {
        handle->incoming_command_active = false;
        handle->incoming_expected_length = 0U;
        handle->incoming_received_length = 0U;

        if (!mqtt_service_topic_matches(event, handle->config_set_topic))
        {
            return;
        }

        /*
         * config/set 表示一次性控制命令，不能把 Broker 历史值当成新命令。
         * Broker 在客户端订阅后回放保留消息时会设置 retain 标志；拒绝该
         * 消息可避免旧配置在设备重启后覆盖已经从 NVS 恢复的有效配置。
         */
        if (event->retain)
        {
            ESP_LOGW(TAG, "Rejected retained MQTT command on %s; clear the retained message",
                     handle->config_set_topic);
            mqtt_service_record_rejected_command(handle);
            return;
        }

        if ((event->total_data_len <= 0) ||
            (event->total_data_len > (int)MQTT_SERVICE_COMMAND_MAX_LENGTH))
        {
            ESP_LOGW(TAG, "Rejected MQTT command: payload length=%d", event->total_data_len);
            mqtt_service_record_rejected_command(handle);
            return;
        }

        handle->incoming_expected_length = (size_t)event->total_data_len;
        handle->incoming_command_active = true;
    }

    if (!handle->incoming_command_active || (event->data_len < 0) ||
        (event->current_data_offset < 0) ||
        ((size_t)event->current_data_offset != handle->incoming_received_length) ||
        ((size_t)event->data_len >
         handle->incoming_expected_length - handle->incoming_received_length))
    {
        handle->incoming_command_active = false;
        return;
    }

    (void)memcpy(handle->incoming_command + handle->incoming_received_length, event->data,
                 (size_t)event->data_len);
    handle->incoming_received_length += (size_t)event->data_len;

    if (handle->incoming_received_length == handle->incoming_expected_length)
    {
        mqtt_service_command_t command = {
            .length = handle->incoming_received_length,
        };
        (void)memcpy(command.payload, handle->incoming_command, command.length);
        command.payload[command.length] = '\0';
        handle->incoming_command_active = false;

        if (xQueueSend(handle->command_queue, &command, 0U) != pdTRUE)
        {
            ESP_LOGW(TAG, "Rejected MQTT command: command queue is full");
            mqtt_service_record_rejected_command(handle);
        }
    }
}

/**
 * @brief 在 MQTT 回调中记录连接状态并投递数据或 PUBACK。
 * @note SDK 回调内不等待现场回执，不执行完整业务 JSON 处理或 Flash 擦写。
 */
static void mqtt_service_event_handler(void *handler_args, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data)
{
    (void)event_base;
    mqtt_service_handle_t handle = (mqtt_service_handle_t)handler_args;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
        case MQTT_EVENT_CONNECTED:
            ++handle->mqtt_session_generation;
            mqtt_service_set_state(handle, MQTT_SERVICE_STATE_CONNECTED, true, ESP_OK);
            (void)xEventGroupSetBits(handle->event_group,
                                     MQTT_SERVICE_CONNECTED_BIT | MQTT_SERVICE_SESSION_SETUP_BIT);
            ESP_LOGI(TAG, "MQTT broker connected");
            break;

        case MQTT_EVENT_DISCONNECTED:
            (void)xEventGroupClearBits(handle->event_group,
                                       MQTT_SERVICE_CONNECTED_BIT | MQTT_SERVICE_SESSION_SETUP_BIT);
            mqtt_service_set_state(handle, MQTT_SERVICE_STATE_DISCONNECTED, false,
                                   ESP_ERR_INVALID_STATE);
            ESP_LOGW(TAG, "MQTT broker disconnected; waiting for reconnect");
            break;

        case MQTT_EVENT_DATA:
            mqtt_service_receive_command_fragment(handle, event);
            break;

        case MQTT_EVENT_PUBLISHED:
            /* 协议任务只交付 msg_id；业务任务匹配后才删除持久记录。 */
            {
                const mqtt_service_puback_t ack = {
                    .msg_id = event->msg_id,
                    .session_generation = handle->mqtt_session_generation,
                };
                if (xQueueSend(handle->publish_ack_queue, &ack, 0U) != pdTRUE)
                {
                    ESP_LOGW(TAG, "PUBACK queue full; pending record will be retried");
                }
            }
            break;

        case MQTT_EVENT_ERROR:
            (void)xEventGroupClearBits(handle->event_group,
                                       MQTT_SERVICE_CONNECTED_BIT | MQTT_SERVICE_SESSION_SETUP_BIT);
            mqtt_service_set_state(handle, MQTT_SERVICE_STATE_DISCONNECTED, false, ESP_FAIL);
            if ((event->error_handle != NULL) &&
                (event->error_handle->esp_transport_sock_errno != 0))
            {
                ESP_LOGE(TAG, "MQTT transport error; socket errno=%d",
                         event->error_handle->esp_transport_sock_errno);
            }
            else
            {
                ESP_LOGE(TAG, "MQTT client reported an error");
            }
            break;

        default:
            break;
    }
}

/**
 * @brief 向 SDK MQTT 待发队列提交指定 QoS/retain 的消息。
 */
static int mqtt_service_enqueue(mqtt_service_handle_t handle, const char *topic,
                                const char *payload, int qos, bool retain)
{
    return esp_mqtt_client_enqueue(handle->client, topic, payload, 0, qos, retain ? 1 : 0, qos > 0);
}

/**
 * @brief 发布已同步的实际配置和规则快照。
 */
static esp_err_t mqtt_service_publish_config_state(mqtt_service_handle_t handle)
{
    mqtt_runtime_config_t runtime_config;
    const esp_err_t result = mqtt_service_read_runtime_config(handle, &runtime_config);
    if (result != ESP_OK)
    {
        return result;
    }

    if (runtime_config.field_online && !runtime_config.synchronized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root)
    {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(root, "sample_interval_s", runtime_config.sample_period_ms / 1000.0);
    cJSON_AddNumberToObject(root, "report_interval_s", runtime_config.report_period_ms / 1000.0);
    cJSON_AddBoolToObject(root, "collect_enabled", runtime_config.collect_enabled);
    cJSON_AddBoolToObject(root, "field_online", runtime_config.field_online);
    cJSON_AddBoolToObject(root, "synchronized", runtime_config.synchronized);
    cJSON_AddNumberToObject(root, "config_version", runtime_config.config_version);
    cJSON *points = cJSON_AddArrayToObject(root, "thresholds");
    if (!points)
    {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    for (unsigned i = 0; i < NURSERY_POINT_COUNT; i++)
    {
        cJSON *p = cJSON_CreateObject();
        if (!p)
        {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddNumberToObject(p, "point_id", i + 1);
        cJSON_AddNumberToObject(p, "low", runtime_config.rules[i].low);
        cJSON_AddNumberToObject(p, "high", runtime_config.rules[i].high);
        cJSON_AddNumberToObject(p, "hysteresis", runtime_config.rules[i].hysteresis);
        cJSON_AddItemToArray(points, p);
    }
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload)
    {
        return ESP_ERR_NO_MEM;
    }
    int id = mqtt_service_enqueue(handle, handle->config_state_topic, payload, 1, true);
    cJSON_free(payload);
    return id >= 0 ? ESP_OK : ESP_FAIL;
}

/**
 * @brief 建立订阅并发布本次会话在线和配置状态。
 */
static esp_err_t mqtt_service_setup_session(mqtt_service_handle_t handle)
{
    const int subscribe_id = esp_mqtt_client_subscribe(handle->client, handle->config_set_topic, 1);
    if (subscribe_id < 0)
    {
        return ESP_FAIL;
    }

    if (mqtt_service_enqueue(handle, handle->status_topic, MQTT_ONLINE_PAYLOAD, 1, true) < 0)
    {
        return ESP_FAIL;
    }

    const esp_err_t result = mqtt_service_publish_config_state(handle);
    if (result == ESP_OK)
    {
        ESP_LOGI(TAG, "Subscribed to %s", handle->config_set_topic);
    }
    return result;
}

/**
 * @brief 比较离线故障快照，识别无需重复入队的状态。
 */
static bool mqtt_service_same_fault_state(const gateway_outbox_record_t *left,
                                          const gateway_outbox_record_t *right)
{
    if (left->field_online != right->field_online ||
        left->collect_enabled != right->collect_enabled ||
        left->field_boot_id != right->field_boot_id ||
        left->sample_period_ms != right->sample_period_ms)
    {
        return false;
    }
    for (unsigned i = 0U; i < GATEWAY_OUTBOX_POINT_COUNT; ++i)
    {
        if (left->points[i].quality != right->points[i].quality)
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 冻结质量与样本年龄，再保存一条遥测记录。
 */
static esp_err_t mqtt_service_capture_telemetry(mqtt_service_handle_t handle, bool *out_stored)
{
    *out_stored = false;
    mqtt_environment_snapshot_t snapshot;
    const esp_err_t result =
        handle->config.get_snapshot(handle->config.snapshot_context, &snapshot);
    if (result != ESP_OK)
    {
        return result;
    }
    snapshot.gateway_boot_nonce = handle->boot_nonce;
    bool all_points_invalid = true;
    for (unsigned i = 0U; i < GATEWAY_OUTBOX_POINT_COUNT; ++i)
    {
        if (snapshot.points[i].quality == MQTT_SERVICE_QUALITY_VALID)
        {
            all_points_invalid = false;
            break;
        }
    }
    const bool broker_offline =
        (xEventGroupGetBits(handle->event_group) & MQTT_SERVICE_CONNECTED_BIT) == 0U;
    const TickType_t now = xTaskGetTickCount();
    if (broker_offline && all_points_invalid && handle->last_offline_fault_valid &&
        mqtt_service_same_fault_state(&snapshot, &handle->last_offline_fault) &&
        now - handle->last_offline_fault_tick <
            mqtt_service_ms_to_ticks(MQTT_SERVICE_FAULT_HEARTBEAT_MS))
    {
        return ESP_OK;
    }
    uint32_t record_id = 0U;
    const esp_err_t saved = gateway_outbox_append(handle->config.outbox, &snapshot, &record_id);
    if (saved == ESP_OK)
    {
        *out_stored = true;
        ESP_LOGD(TAG, "Telemetry stored as record %" PRIu32, record_id);
    }
    if (broker_offline && all_points_invalid && (saved == ESP_OK || saved == ESP_ERR_NO_MEM))
    {
        handle->last_offline_fault = snapshot;
        handle->last_offline_fault_tick = now;
        handle->last_offline_fault_valid = true;
    }
    else if (saved == ESP_OK)
    {
        handle->last_offline_fault_valid = false;
    }
    return saved;
}

/**
 * @brief 将数据质量枚举转换为 JSON 文本。
 */
static const char *mqtt_quality_name(uint8_t quality)
{
    switch (quality)
    {
        case 1U:
            return "VALID";
        case 2U:
            return "STALE";
        case 3U:
            return "FAULT";
        default:
            return "INVALID";
    }
}

/**
 * @brief 在发送前将持久遥测或事件记录转换为 JSON。
 * @note 返回的 JSON 字符串由调用方使用 cJSON_free 释放。
 */
static char *mqtt_service_record_json(const mqtt_service_handle_t handle,
                                      const gateway_outbox_record_t *record)
{
    static const char *value_keys[3] = {"temperature_c", "humidity_rh", "light_lux"};
    static const char *quality_keys[3] = {"temperature_quality", "humidity_quality",
                                          "light_quality"};
    static const char *age_keys[3] = {"temperature_age_ms", "humidity_age_ms", "light_age_ms"};
    if (record->record_kind)
    {
        char payload[512];
        snprintf(
            payload, sizeof(payload),
            "{\"record_id\":%lu,\"event_id\":%lu,\"field_boot_id\":%u,\"uptime_ms\":%lu,\"point_"
            "id\":%u,\"state\":%u,\"event_kind\":%u,\"quality\":%u,\"value_scaled\":%ld}",
            (unsigned long)record->record_id, (unsigned long)record->event_id,
            record->field_boot_id, (unsigned long)record->event_uptime, record->event_point,
            record->event_state, record->event_kind, record->event_quality,
            (long)record->event_value);
        return strdup(payload);
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        return NULL;
    }
    bool complete = cJSON_AddNumberToObject(root, "record_id", record->record_id) != NULL;
    complete &=
        cJSON_AddNumberToObject(root, "capture_uptime_ms", record->capture_uptime_ms) != NULL;
    complete &= cJSON_AddBoolToObject(root, "field_online", record->field_online) != NULL;
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    const bool same_boot = record->gateway_boot_nonce == handle->boot_nonce;
    const uint32_t delivery_age_ms = now_ms - record->capture_uptime_ms;
    complete &= cJSON_AddBoolToObject(root, "queued_delivery",
                                      !same_boot || delivery_age_ms > 5000U) != NULL;
    complete &= (same_boot ? cJSON_AddNumberToObject(root, "delivery_delay_ms", delivery_age_ms)
                           : cJSON_AddNullToObject(root, "delivery_delay_ms")) != NULL;
    for (unsigned i = 0U; i < GATEWAY_OUTBOX_POINT_COUNT; ++i)
    {
        const gateway_outbox_point_t *point = &record->points[i];
        char value_key[40], quality_key[40], age_key[40], alarm_key[40];
        snprintf(value_key, sizeof(value_key), "zone%u_%s", i / 3 + 1, value_keys[i % 3]);
        snprintf(quality_key, sizeof(quality_key), "zone%u_%s", i / 3 + 1, quality_keys[i % 3]);
        snprintf(age_key, sizeof(age_key), "zone%u_%s", i / 3 + 1, age_keys[i % 3]);
        snprintf(alarm_key, sizeof(alarm_key), "zone%u_point%u_alarm", i / 3 + 1, i % 3 + 1);
        complete &= cJSON_AddNumberToObject(root, alarm_key, point->alarm_state) != NULL;
        complete &=
            cJSON_AddStringToObject(root, quality_key, mqtt_quality_name(point->quality)) != NULL;
        if (point->sample_age_ms == UINT32_MAX)
        {
            complete &= cJSON_AddNullToObject(root, age_key) != NULL;
        }
        else
        {
            complete &= cJSON_AddNumberToObject(root, age_key, point->sample_age_ms) != NULL;
        }
        if (point->quality == MQTT_SERVICE_QUALITY_VALID)
        {
            const double divisor = i % 3 < 2U ? 10.0 : 1.0;
            complete &= cJSON_AddNumberToObject(root, value_key,
                                                (double)point->value_scaled / divisor) != NULL;
        }
        else
        {
            complete &= cJSON_AddNullToObject(root, value_key) != NULL;
        }
    }
    char *json = complete ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    return json;
}

/**
 * @brief 读取最老待发记录并建立唯一业务在途消息。
 */
static esp_err_t mqtt_service_publish_pending(mqtt_service_handle_t handle)
{
    if (handle->telemetry_inflight)
    {
        return ESP_OK;
    }
    gateway_outbox_record_t record;
    const esp_err_t found = gateway_outbox_peek_oldest(handle->config.outbox, &record);
    if (found == ESP_ERR_NOT_FOUND)
    {
        return ESP_OK;
    }
    if (found != ESP_OK)
    {
        return found;
    }
    char *payload = mqtt_service_record_json(handle, &record);
    if (payload == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    char event_topic[128];
    /* Derive events topic from the configured telemetry topic, preserving device identity. */
    snprintf(event_topic, sizeof(event_topic), "%s", handle->telemetry_topic);
    char *suffix = strrchr(event_topic, '/');
    if (suffix)
    {
        snprintf(suffix, (size_t)(event_topic + sizeof(event_topic) - suffix), "/events");
    }
    const int msg_id = esp_mqtt_client_publish(
        handle->client, record.record_kind ? event_topic : handle->telemetry_topic, payload, 0, 1,
        0);
    cJSON_free(payload);
    if (msg_id < 0)
    {
        return ESP_FAIL;
    }
    handle->telemetry_inflight = true;
    handle->telemetry_msg_id = msg_id;
    handle->telemetry_session_generation = handle->mqtt_session_generation;
    handle->telemetry_record_id = record.record_id;
    handle->telemetry_sent_tick = xTaskGetTickCount();
    mqtt_service_record_publish(handle, ESP_OK);
    ESP_LOGI(TAG, "Record %" PRIu32 " sent as MQTT msg_id=%d", record.record_id, msg_id);
    return ESP_OK;
}

/**
 * @brief 将秒制 JSON 数值转换为经过范围校验的整数毫秒。
 */
static bool mqtt_service_json_seconds_to_ms(const cJSON *item, uint32_t minimum_ms,
                                            uint32_t maximum_ms, uint32_t *out_value_ms)
{
    if (!cJSON_IsNumber(item) || (out_value_ms == NULL))
    {
        return false;
    }

    const double milliseconds = item->valuedouble * 1000.0;
    if ((milliseconds < (double)minimum_ms) || (milliseconds > (double)maximum_ms))
    {
        return false;
    }

    const uint32_t rounded_ms = (uint32_t)(milliseconds + 0.5);
    double difference = milliseconds - (double)rounded_ms;
    if (difference < 0.0)
    {
        difference = -difference;
    }
    if (difference > 0.001)
    {
        return false;
    }

    *out_value_ms = rounded_ms;
    return true;
}

/**
 * @brief 校验云端单字段命令，调用配置接口并发布逐条结果。
 */
static void mqtt_service_process_command(mqtt_service_handle_t handle,
                                         const mqtt_service_command_t *command)
{
    cJSON *root = cJSON_ParseWithLength(command->payload, command->length);
    if (!cJSON_IsObject(root))
    {
        ESP_LOGW(TAG, "Rejected MQTT command: invalid JSON object");
        mqtt_service_record_rejected_command(handle);
        cJSON_Delete(root);
        return;
    }

    const cJSON *sample_item = cJSON_GetObjectItemCaseSensitive(root, "sample_interval_s");
    const cJSON *report_item = cJSON_GetObjectItemCaseSensitive(root, "report_interval_s");
    const cJSON *collect_item = cJSON_GetObjectItemCaseSensitive(root, "collect_enabled");
    const cJSON *request_item = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    const cJSON *threshold = cJSON_GetObjectItemCaseSensitive(root, "threshold");
    const cJSON *point = cJSON_GetObjectItemCaseSensitive(root, "point_id");
    const cJSON *parameter = cJSON_GetObjectItemCaseSensitive(root, "parameter");
    bool has_threshold = threshold != NULL;
    uint8_t threshold_op = 0;
    if (cJSON_IsString(parameter))
    {
        if (!strcmp(parameter->valuestring, "low"))
        {
            threshold_op = GW_CMD_SET_LOW;
        }
        else if (!strcmp(parameter->valuestring, "high"))
        {
            threshold_op = GW_CMD_SET_HIGH;
        }
        else if (!strcmp(parameter->valuestring, "hysteresis"))
        {
            threshold_op = GW_CMD_SET_HYSTERESIS;
        }
    }
    bool threshold_valid =
        !has_threshold ||
        (cJSON_IsNumber(threshold) && threshold->valuedouble >= -400 &&
         threshold->valuedouble <= 200000 &&
         threshold->valuedouble == (double)(int32_t)threshold->valuedouble &&
         cJSON_IsNumber(point) && point->valuedouble >= 1 && point->valuedouble <= 9 &&
         point->valuedouble == (double)(unsigned)point->valuedouble && threshold_op);
    bool request_valid = cJSON_IsNumber(request_item) && request_item->valuedouble >= 1 &&
                         request_item->valuedouble <= 536870911 &&
                         request_item->valuedouble == (double)(uint32_t)request_item->valuedouble;
    const bool has_collect = collect_item != NULL;
    const bool has_sample_period = sample_item != NULL;
    const bool has_report_period = report_item != NULL;
    uint32_t sample_period_ms = 0U;
    uint32_t report_period_ms = 0U;

    const bool sample_valid = !has_sample_period || mqtt_service_json_seconds_to_ms(
                                                        sample_item, NURSERY_PERIOD_MIN_MS,
                                                        NURSERY_PERIOD_MAX_MS, &sample_period_ms);
    const bool report_valid =
        !has_report_period ||
        mqtt_service_json_seconds_to_ms(report_item, MQTT_SERVICE_MIN_REPORT_PERIOD_MS,
                                        MQTT_SERVICE_MAX_REPORT_PERIOD_MS, &report_period_ms);
    const bool collect_valid = !has_collect || cJSON_IsBool(collect_item);

    if ((!has_collect && !has_sample_period && !has_report_period && !has_threshold) ||
        !threshold_valid || !request_valid ||
        ((unsigned)has_collect + (unsigned)has_sample_period + (unsigned)has_report_period +
         (unsigned)has_threshold) != 1U ||
        !collect_valid || !sample_valid || !report_valid)
    {
        ESP_LOGW(TAG, "Rejected MQTT command: invalid control fields");
        mqtt_service_record_rejected_command(handle);
        cJSON_Delete(root);
        return;
    }

    const mqtt_runtime_config_update_t update = {
        .request_id = (uint32_t)request_item->valuedouble,
        .has_threshold = has_threshold,
        .threshold_op = threshold_op,
        .point_id = has_threshold ? (uint8_t)point->valueint : 0,
        .threshold_value = has_threshold ? (int32_t)threshold->valuedouble : 0,
        .has_collect_enabled = has_collect,
        .collect_enabled = has_collect && cJSON_IsTrue(collect_item),
        .has_sample_period = has_sample_period,
        .sample_period_ms = sample_period_ms,
        .has_report_period = has_report_period,
        .report_period_ms = report_period_ms,
    };
    const esp_err_t applied =
        handle->config.apply_runtime_config(handle->config.runtime_config_context, &update);
    if (applied == ESP_OK)
    {
        if (has_collect || has_sample_period)
        {
            mqtt_service_record_field_applied(handle);
        }
        ESP_LOGI(
            TAG,
            "MQTT configuration applied: collect=%d sample=%" PRIu32 " ms, report=%" PRIu32 " ms",
            has_collect ? (int)cJSON_IsTrue(collect_item) : -1,
            has_sample_period ? sample_period_ms : 0U, has_report_period ? report_period_ms : 0U);
        const esp_err_t published = mqtt_service_publish_config_state(handle);
        if (published != ESP_OK)
        {
            ESP_LOGW(TAG, "Applied configuration but state publish failed: %s",
                     esp_err_to_name(published));
        }
    }
    else
    {
        ESP_LOGW(TAG, "Failed to apply MQTT configuration: %s", esp_err_to_name(applied));
        mqtt_service_record_rejected_command(handle);
    }

    char topic[128], reply[160];
    snprintf(topic, sizeof(topic), "%s/result", handle->config_set_topic);
    snprintf(reply, sizeof(reply), "{\"request_id\":%lu,\"status\":\"%s\",\"error\":%d}",
             (unsigned long)update.request_id,
             applied == ESP_OK            ? "APPLIED"
             : applied == ESP_ERR_TIMEOUT ? "UNCONFIRMED"
                                          : "FAILED",
             (int)applied);
    (void)mqtt_service_enqueue(handle, topic, reply, 1, false);
    cJSON_Delete(root);
}

/**
 * @brief 在独立任务中解析并处理云端命令。
 * @note 任务入口，只在任务上下文运行；正常情况下不返回。
 */
static void mqtt_command_task(void *context)
{
    mqtt_service_handle_t h = context;
    mqtt_service_command_t command;
    for (;;)
    {
        if (xQueueReceive(h->command_queue, &command, portMAX_DELAY) == pdTRUE)
        {
            mqtt_service_process_command(h, &command);
        }
    }
}

/**
 * @brief 调度遥测、事件追补、PUBACK 和连接会话。
 * @note 任务入口，只在任务上下文运行；正常情况下不返回。
 */
static void mqtt_service_task(void *context)
{
    mqtt_service_handle_t handle = (mqtt_service_handle_t)context;
    mqtt_service_set_state(handle, MQTT_SERVICE_STATE_WAITING_NETWORK, false, ESP_OK);
    bool client_started = false;
    TickType_t last_start_attempt =
        xTaskGetTickCount() - mqtt_service_ms_to_ticks(MQTT_SERVICE_NETWORK_POLL_MS);
    TickType_t last_report_at =
        xTaskGetTickCount() - mqtt_service_ms_to_ticks(mqtt_service_get_report_period(handle));
    TickType_t last_replay = 0, last_config_check = 0;
    uint32_t published_config_version = UINT32_MAX;
    bool published_field_online = false;
    gateway_event_t pending_event = {0};
    bool have_event = false;
    while (true)
    {
        const TickType_t now = xTaskGetTickCount();
        if (handle->config.field_link)
        {
            if (!have_event)
            {
                have_event =
                    gateway_link_take_event(handle->config.field_link, &pending_event) == ESP_OK;
            }
            if (have_event)
            {
                uint32_t cursor = gateway_outbox_event_cursor(handle->config.outbox);
                if (pending_event.id <= cursor)
                {
                    have_event = false;
                }
                else
                {
                    gateway_outbox_record_t r = {.record_kind = 1,
                                                 .event_id = pending_event.id,
                                                 .event_uptime = pending_event.uptime,
                                                 .event_value = pending_event.value,
                                                 .event_point = pending_event.point,
                                                 .event_state = pending_event.state,
                                                 .event_kind = pending_event.kind,
                                                 .event_quality = pending_event.quality,
                                                 .field_boot_id = pending_event.field_boot_id,
                                                 .gateway_boot_nonce = handle->boot_nonce,
                                                 .capture_uptime_ms =
                                                     (uint32_t)(esp_timer_get_time() / 1000)};
                    if (gateway_outbox_append(handle->config.outbox, &r, NULL) == ESP_OK)
                    {
                        have_event = false;
                    }
                }
            }
            if (!have_event && (pending_event.id != 0 || now - last_replay >= pdMS_TO_TICKS(5000)))
            {
                pending_event.id = 0;
                uint32_t id = esp_random() & 0x1fffffffU;
                if (!id)
                {
                    id = 1;
                }
                (void)gateway_link_submit(handle->config.field_link, GW_CMD_QUERY_EVENTS,
                                          gateway_outbox_event_cursor(handle->config.outbox), 0,
                                          id);
                last_replay = now;
            }
        }
        if (!client_started &&
            now - last_start_attempt >= mqtt_service_ms_to_ticks(MQTT_SERVICE_NETWORK_POLL_MS) &&
            handle->config.is_network_ready(handle->config.network_context))
        {
            last_start_attempt = now;
            mqtt_service_set_state(handle, MQTT_SERVICE_STATE_CONNECTING, false, ESP_OK);
            const esp_err_t start_result = esp_mqtt_client_start(handle->client);
            if (start_result == ESP_OK)
            {
                client_started = true;
            }
            else
            {
                mqtt_service_set_state(handle, MQTT_SERVICE_STATE_WAITING_NETWORK, false,
                                       start_result);
                ESP_LOGW(TAG, "MQTT start failed: %s", esp_err_to_name(start_result));
            }
        }

        const TickType_t report_ticks =
            mqtt_service_ms_to_ticks(mqtt_service_get_report_period(handle));
        if (now - last_report_at >= report_ticks)
        {
            bool stored = false;
            const esp_err_t saved = mqtt_service_capture_telemetry(handle, &stored);
            if (stored || saved != ESP_OK)
            {
                mqtt_service_record_capture(handle, saved);
            }
            if (saved != ESP_OK)
            {
                ESP_LOGW(TAG, "Telemetry could not be stored: %s", esp_err_to_name(saved));
            }
            last_report_at = now;
        }

        const EventBits_t bits = xEventGroupGetBits(handle->event_group);
        if ((bits & MQTT_SERVICE_CONNECTED_BIT) && now - last_config_check >= pdMS_TO_TICKS(2000))
        {
            mqtt_runtime_config_t actual;
            if (mqtt_service_read_runtime_config(handle, &actual) == ESP_OK &&
                (actual.config_version != published_config_version ||
                 actual.field_online != published_field_online) &&
                mqtt_service_publish_config_state(handle) == ESP_OK)
            {
                published_config_version = actual.config_version;
                published_field_online = actual.field_online;
            }
            last_config_check = now;
        }

        if ((bits & MQTT_SERVICE_SESSION_SETUP_BIT) != 0U)
        {
            handle->telemetry_inflight = false;
            handle->publish_backoff_active = false;
            mqtt_service_puback_t old_ack;
            while (xQueueReceive(handle->publish_ack_queue, &old_ack, 0U) == pdTRUE)
            {
            }
            const esp_err_t setup_result = mqtt_service_setup_session(handle);
            if (setup_result == ESP_OK)
            {
                (void)xEventGroupClearBits(handle->event_group, MQTT_SERVICE_SESSION_SETUP_BIT);
            }
            else
            {
                ESP_LOGW(TAG, "MQTT session setup will be retried");
            }
        }

        mqtt_service_puback_t ack;
        while (xQueueReceive(handle->publish_ack_queue, &ack, 0U) == pdTRUE)
        {
            if (handle->telemetry_inflight && ack.msg_id == handle->telemetry_msg_id &&
                ack.session_generation == handle->telemetry_session_generation)
            {
                const esp_err_t acked =
                    gateway_outbox_ack(handle->config.outbox, handle->telemetry_record_id);
                if (acked == ESP_OK)
                {
                    mqtt_service_record_broker_ack(handle);
                    ESP_LOGI(TAG, "Broker confirmed record %" PRIu32, handle->telemetry_record_id);
                }
                else
                {
                    ESP_LOGW(TAG, "PUBACK received but record deletion failed: %s",
                             esp_err_to_name(acked));
                }
                handle->telemetry_inflight = false;
            }
        }

        if ((bits & MQTT_SERVICE_CONNECTED_BIT) == 0U)
        {
            handle->telemetry_inflight = false;
        }
        else if ((bits & MQTT_SERVICE_SESSION_SETUP_BIT) == 0U)
        {
            if (handle->telemetry_inflight &&
                now - handle->telemetry_sent_tick >=
                    mqtt_service_ms_to_ticks(MQTT_SERVICE_PUBACK_TIMEOUT_MS))
            {
                /* ESP-MQTT already retransmits its in-flight QoS 1 packet.
                 * Do not create a second PUBLISH for the same NVS record. */
                ESP_LOGW(TAG, "Telemetry PUBACK still missing; reconnecting MQTT");
                const esp_err_t reconnect = esp_mqtt_client_reconnect(handle->client);
                if (reconnect != ESP_OK)
                {
                    ESP_LOGW(TAG, "MQTT reconnect request failed: %s", esp_err_to_name(reconnect));
                }
                handle->telemetry_sent_tick = now;
            }
            if (!handle->publish_backoff_active ||
                now - handle->last_publish_error_tick >= mqtt_service_ms_to_ticks(1000U))
            {
                const esp_err_t published = mqtt_service_publish_pending(handle);
                if (published != ESP_OK)
                {
                    handle->last_publish_error_tick = now;
                    handle->publish_backoff_active = true;
                    mqtt_service_record_publish(handle, published);
                    ESP_LOGW(TAG, "Persistent telemetry send failed: %s",
                             esp_err_to_name(published));
                }
                else
                {
                    handle->publish_backoff_active = false;
                }
            }
        }

        vTaskDelay(mqtt_service_ms_to_ticks(MQTT_SERVICE_LOOP_DELAY_MS));
    }
}

/**
 * @brief 释放尚未启动成功的 MQTT 实例资源。
 */
static void mqtt_service_release_partial_instance(mqtt_service_handle_t handle)
{
    if (handle == NULL)
    {
        return;
    }
    if (handle->client != NULL)
    {
        (void)esp_mqtt_client_destroy(handle->client);
    }
    if (handle->command_queue != NULL)
    {
        vQueueDelete(handle->command_queue);
    }
    if (handle->publish_ack_queue != NULL)
    {
        vQueueDelete(handle->publish_ack_queue);
    }
    if (handle->event_group != NULL)
    {
        vEventGroupDelete(handle->event_group);
    }
    if (handle->status_mutex != NULL)
    {
        vSemaphoreDelete(handle->status_mutex);
    }
    free(handle);
}

/**
 * @brief 创建MQTT 上报与远程控制服务实例并准备所需资源。
 * @param[in] config 模块配置及其取值约束。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t mqtt_service_create(const mqtt_service_config_t *config,
                              mqtt_service_handle_t *out_handle)
{
    if (!mqtt_service_config_is_valid(config) || (out_handle == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;

    mqtt_service_handle_t handle = calloc(1U, sizeof(*handle));
    if (handle == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    (void)strcpy(handle->broker_uri, config->broker_uri);
    (void)strcpy(handle->username, config->username);
    (void)strcpy(handle->password, config->password);
    handle->config = *config;
    handle->boot_nonce = esp_random();
    handle->config.broker_uri = handle->broker_uri;
    handle->config.username = handle->username;
    handle->config.password = handle->password;
    handle->status.state = MQTT_SERVICE_STATE_CREATED;
    handle->status.last_error = ESP_OK;

    handle->status_mutex = xSemaphoreCreateMutex();
    handle->event_group = xEventGroupCreate();
    handle->command_queue =
        xQueueCreate(MQTT_SERVICE_COMMAND_QUEUE_LENGTH, sizeof(mqtt_service_command_t));
    handle->publish_ack_queue =
        xQueueCreate(MQTT_SERVICE_ACK_QUEUE_LENGTH, sizeof(mqtt_service_puback_t));
    if ((handle->status_mutex == NULL) || (handle->event_group == NULL) ||
        (handle->command_queue == NULL) || (handle->publish_ack_queue == NULL))
    {
        mqtt_service_release_partial_instance(handle);
        return ESP_ERR_NO_MEM;
    }

    mqtt_runtime_config_t runtime_config;
    esp_err_t result = mqtt_service_read_runtime_config(handle, &runtime_config);
    if (result != ESP_OK)
    {
        mqtt_service_release_partial_instance(handle);
        return result;
    }

    uint8_t mac_address[6U];
    result = esp_read_mac(mac_address, ESP_MAC_WIFI_STA);
    if (result != ESP_OK)
    {
        mqtt_service_release_partial_instance(handle);
        return result;
    }
    (void)snprintf(handle->device_id, sizeof(handle->device_id), "%02x%02x%02x%02x%02x%02x",
                   mac_address[0], mac_address[1], mac_address[2], mac_address[3], mac_address[4],
                   mac_address[5]);
    (void)snprintf(handle->client_id, sizeof(handle->client_id), "esp32s3-env-%s",
                   handle->device_id);
    (void)snprintf(handle->telemetry_topic, sizeof(handle->telemetry_topic), "env/%s/telemetry",
                   handle->device_id);
    (void)snprintf(handle->status_topic, sizeof(handle->status_topic), "env/%s/status",
                   handle->device_id);
    (void)snprintf(handle->config_set_topic, sizeof(handle->config_set_topic), "env/%s/config/set",
                   handle->device_id);
    (void)snprintf(handle->config_state_topic, sizeof(handle->config_state_topic),
                   "env/%s/config/state", handle->device_id);

    const esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = handle->broker_uri,
        .credentials.username = (handle->username[0] != '\0') ? handle->username : NULL,
        .credentials.client_id = handle->client_id,
        .credentials.authentication.password =
            (handle->password[0] != '\0') ? handle->password : NULL,
        .session.last_will.topic = handle->status_topic,
        .session.last_will.msg = MQTT_OFFLINE_PAYLOAD,
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
        .network.reconnect_timeout_ms = 5000,
    };
    handle->client = esp_mqtt_client_init(&mqtt_config);
    if (handle->client == NULL)
    {
        mqtt_service_release_partial_instance(handle);
        return ESP_FAIL;
    }

    result = esp_mqtt_client_register_event(handle->client, ESP_EVENT_ANY_ID,
                                            mqtt_service_event_handler, handle);
    if (result != ESP_OK)
    {
        mqtt_service_release_partial_instance(handle);
        return result;
    }

    ESP_LOGI(TAG, "MQTT device ID: %s", handle->device_id);
    ESP_LOGI(TAG, "MQTT Broker: %s", handle->broker_uri);
    ESP_LOGI(TAG, "Telemetry topic: %s", handle->telemetry_topic);
    *out_handle = handle;
    return ESP_OK;
}

/**
 * @brief 启动MQTT 上报与远程控制服务的运行入口。
 * @param[in] handle 已创建的模块实例句柄。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t mqtt_service_start(mqtt_service_handle_t handle)
{
    if (handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->started)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xTaskCreate(mqtt_command_task, "mqtt_commands", 4096, handle, 4, NULL) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }
    const BaseType_t task_created =
        xTaskCreate(mqtt_service_task, "mqtt_service", MQTT_SERVICE_TASK_STACK_SIZE, handle,
                    MQTT_SERVICE_TASK_PRIORITY, &handle->task);
    if (task_created != pdPASS)
    {
        handle->task = NULL;
        return ESP_ERR_NO_MEM;
    }

    handle->started = true;
    return ESP_OK;
}

/**
 * @brief 复制MQTT 上报与远程控制服务的当前状态。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_status 接收复制出的完整状态或配置。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t mqtt_service_get_status(mqtt_service_handle_t handle, mqtt_service_status_t *out_status)
{
    if ((handle == NULL) || (out_status == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(handle->status_mutex,
                       mqtt_service_ms_to_ticks(MQTT_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    *out_status = handle->status;
    (void)xSemaphoreGive(handle->status_mutex);
    gateway_outbox_status_t outbox_status;
    if (gateway_outbox_get_status(handle->config.outbox, &outbox_status) == ESP_OK)
    {
        out_status->pending_telemetry_count = outbox_status.pending_count;
        out_status->dropped_telemetry_count = outbox_status.dropped_count;
    }
    return ESP_OK;
}
