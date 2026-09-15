/**
 * @file mqtt_service.c
 * @brief MQTT 数据发布、订阅恢复和远程配置实现。
 */

#include "mqtt_service.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"

#define MQTT_SERVICE_BROKER_URI_LENGTH       (256U)
#define MQTT_SERVICE_USERNAME_LENGTH         (128U)
#define MQTT_SERVICE_PASSWORD_LENGTH         (128U)
#define MQTT_SERVICE_TOPIC_LENGTH            (96U)
#define MQTT_SERVICE_CLIENT_ID_LENGTH        (40U)
#define MQTT_SERVICE_COMMAND_MAX_LENGTH      (256U)
#define MQTT_SERVICE_COMMAND_QUEUE_LENGTH    (4U)
#define MQTT_SERVICE_TASK_STACK_SIZE         (6144U)
#define MQTT_SERVICE_TASK_PRIORITY           (5U)
#define MQTT_SERVICE_LOOP_DELAY_MS           (100U)
#define MQTT_SERVICE_NETWORK_POLL_MS         (500U)
#define MQTT_SERVICE_LOCK_TIMEOUT_MS         (100U)
#define MQTT_SERVICE_CONNECTED_BIT           BIT0
#define MQTT_SERVICE_SESSION_SETUP_BIT       BIT1

static const char *TAG = "mqtt_service";
static const char *MQTT_ONLINE_PAYLOAD = "{\"online\":true}";
static const char *MQTT_OFFLINE_PAYLOAD = "{\"online\":false}";

typedef struct {
    char payload[MQTT_SERVICE_COMMAND_MAX_LENGTH + 1U];
    size_t length;
} mqtt_service_command_t;

struct mqtt_service {
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
    EventGroupHandle_t event_group;
    SemaphoreHandle_t status_mutex;
    mqtt_service_status_t status;

    char incoming_command[MQTT_SERVICE_COMMAND_MAX_LENGTH + 1U];
    size_t incoming_expected_length;
    size_t incoming_received_length;
    bool incoming_command_active;
    bool started;
};

static TickType_t mqtt_service_ms_to_ticks(uint32_t milliseconds)
{
    const TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    return (ticks > 0U) ? ticks : 1U;
}

static bool mqtt_service_string_fits(const char *value, size_t capacity)
{
    return (value != NULL) && (strlen(value) < capacity);
}

static bool mqtt_service_config_is_valid(const mqtt_service_config_t *config)
{
    return (config != NULL) &&
           mqtt_service_string_fits(config->broker_uri,
                                    MQTT_SERVICE_BROKER_URI_LENGTH) &&
           (config->broker_uri[0] != '\0') &&
           mqtt_service_string_fits(config->username,
                                    MQTT_SERVICE_USERNAME_LENGTH) &&
           mqtt_service_string_fits(config->password,
                                    MQTT_SERVICE_PASSWORD_LENGTH) &&
           (config->snapshot_context != NULL) &&
           (config->get_snapshot != NULL) &&
           (config->runtime_config_context != NULL) &&
           (config->get_runtime_config != NULL) &&
           (config->apply_runtime_config != NULL) &&
           (config->network_context != NULL) &&
           (config->is_network_ready != NULL);
}

static void mqtt_service_set_state(mqtt_service_handle_t handle,
                                   mqtt_service_state_t state,
                                   bool connected,
                                   esp_err_t last_error)
{
    if (xSemaphoreTake(handle->status_mutex,
                       mqtt_service_ms_to_ticks(MQTT_SERVICE_LOCK_TIMEOUT_MS)) == pdTRUE) {
        handle->status.state = state;
        handle->status.connected = connected;
        handle->status.last_error = last_error;
        (void)xSemaphoreGive(handle->status_mutex);
    }
}

static bool mqtt_service_runtime_config_is_valid(
    const mqtt_runtime_config_t *config)
{
    return (config != NULL) &&
           (config->sample_period_ms >= 1000U) &&
           (config->sample_period_ms <= 600000U) &&
           (config->report_period_ms >= MQTT_SERVICE_MIN_REPORT_PERIOD_MS) &&
           (config->report_period_ms <= MQTT_SERVICE_MAX_REPORT_PERIOD_MS);
}

/** 从配置业务层读取权威值，并同步诊断快照中的上报周期。 */
static esp_err_t mqtt_service_read_runtime_config(
    mqtt_service_handle_t handle,
    mqtt_runtime_config_t *out_config)
{
    const esp_err_t result = handle->config.get_runtime_config(
        handle->config.runtime_config_context,
        out_config);
    if ((result != ESP_OK) || !mqtt_service_runtime_config_is_valid(out_config)) {
        return (result != ESP_OK) ? result : ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(handle->status_mutex,
                       mqtt_service_ms_to_ticks(MQTT_SERVICE_LOCK_TIMEOUT_MS)) == pdTRUE) {
        handle->status.report_period_ms = out_config->report_period_ms;
        (void)xSemaphoreGive(handle->status_mutex);
    }
    return ESP_OK;
}

static uint32_t mqtt_service_get_report_period(mqtt_service_handle_t handle)
{
    mqtt_runtime_config_t runtime_config;
    if (mqtt_service_read_runtime_config(handle, &runtime_config) == ESP_OK) {
        return runtime_config.report_period_ms;
    }

    uint32_t period_ms = MQTT_SERVICE_MIN_REPORT_PERIOD_MS;
    if (xSemaphoreTake(handle->status_mutex,
                       mqtt_service_ms_to_ticks(MQTT_SERVICE_LOCK_TIMEOUT_MS)) == pdTRUE) {
        period_ms = handle->status.report_period_ms;
        (void)xSemaphoreGive(handle->status_mutex);
    }
    return period_ms;
}

static void mqtt_service_record_publish(mqtt_service_handle_t handle,
                                        esp_err_t result)
{
    if (xSemaphoreTake(handle->status_mutex,
                       mqtt_service_ms_to_ticks(MQTT_SERVICE_LOCK_TIMEOUT_MS)) == pdTRUE) {
        if (result == ESP_OK) {
            ++handle->status.published_telemetry_count;
        } else {
            handle->status.last_error = result;
        }
        (void)xSemaphoreGive(handle->status_mutex);
    }
}

static void mqtt_service_record_rejected_command(mqtt_service_handle_t handle)
{
    if (xSemaphoreTake(handle->status_mutex,
                       mqtt_service_ms_to_ticks(MQTT_SERVICE_LOCK_TIMEOUT_MS)) == pdTRUE) {
        ++handle->status.rejected_command_count;
        handle->status.last_error = ESP_ERR_INVALID_ARG;
        (void)xSemaphoreGive(handle->status_mutex);
    }
}

static bool mqtt_service_topic_matches(const esp_mqtt_event_handle_t event,
                                       const char *expected_topic)
{
    const size_t expected_length = strlen(expected_topic);
    return (event->topic != NULL) &&
           (event->topic_len == (int)expected_length) &&
           (memcmp(event->topic, expected_topic, expected_length) == 0);
}

/** MQTT 数据可能分片到达，此函数只负责重组并投递完整命令。 */
static void mqtt_service_receive_command_fragment(
    mqtt_service_handle_t handle,
    const esp_mqtt_event_handle_t event)
{
    if (event->current_data_offset == 0) {
        handle->incoming_command_active = false;
        handle->incoming_expected_length = 0U;
        handle->incoming_received_length = 0U;

        if (!mqtt_service_topic_matches(event, handle->config_set_topic)) {
            return;
        }

        /*
         * config/set 表示一次性控制命令，不能把 Broker 历史值当成新命令。
         * Broker 在客户端订阅后回放保留消息时会设置 retain 标志；拒绝该
         * 消息可避免旧配置在设备重启后覆盖已经从 NVS 恢复的有效配置。
         */
        if (event->retain) {
            ESP_LOGW(TAG,
                     "Rejected retained MQTT command on %s; clear the retained message",
                     handle->config_set_topic);
            mqtt_service_record_rejected_command(handle);
            return;
        }

        if ((event->total_data_len <= 0) ||
            (event->total_data_len > (int)MQTT_SERVICE_COMMAND_MAX_LENGTH)) {
            ESP_LOGW(TAG, "Rejected MQTT command: payload length=%d",
                     event->total_data_len);
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
         handle->incoming_expected_length - handle->incoming_received_length)) {
        handle->incoming_command_active = false;
        return;
    }

    (void)memcpy(handle->incoming_command + handle->incoming_received_length,
                 event->data,
                 (size_t)event->data_len);
    handle->incoming_received_length += (size_t)event->data_len;

    if (handle->incoming_received_length == handle->incoming_expected_length) {
        mqtt_service_command_t command = {
            .length = handle->incoming_received_length,
        };
        (void)memcpy(command.payload,
                     handle->incoming_command,
                     command.length);
        command.payload[command.length] = '\0';
        handle->incoming_command_active = false;

        if (xQueueSend(handle->command_queue, &command, 0U) != pdTRUE) {
            ESP_LOGW(TAG, "Rejected MQTT command: command queue is full");
            mqtt_service_record_rejected_command(handle);
        }
    }
}

/** MQTT 回调仅更新状态或投递数据，不在协议任务中解析 JSON。 */
static void mqtt_service_event_handler(void *handler_args,
                                       esp_event_base_t event_base,
                                       int32_t event_id,
                                       void *event_data)
{
    (void)event_base;
    mqtt_service_handle_t handle = (mqtt_service_handle_t)handler_args;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            mqtt_service_set_state(handle,
                                   MQTT_SERVICE_STATE_CONNECTED,
                                   true,
                                   ESP_OK);
            (void)xEventGroupSetBits(handle->event_group,
                                     MQTT_SERVICE_CONNECTED_BIT |
                                         MQTT_SERVICE_SESSION_SETUP_BIT);
            ESP_LOGI(TAG, "MQTT broker connected");
            break;

        case MQTT_EVENT_DISCONNECTED:
            (void)xEventGroupClearBits(handle->event_group,
                                       MQTT_SERVICE_CONNECTED_BIT |
                                           MQTT_SERVICE_SESSION_SETUP_BIT);
            mqtt_service_set_state(handle,
                                   MQTT_SERVICE_STATE_DISCONNECTED,
                                   false,
                                   ESP_ERR_INVALID_STATE);
            ESP_LOGW(TAG, "MQTT broker disconnected; waiting for reconnect");
            break;

        case MQTT_EVENT_DATA:
            mqtt_service_receive_command_fragment(handle, event);
            break;

        case MQTT_EVENT_ERROR:
            (void)xEventGroupClearBits(handle->event_group,
                                       MQTT_SERVICE_CONNECTED_BIT |
                                           MQTT_SERVICE_SESSION_SETUP_BIT);
            mqtt_service_set_state(handle,
                                   MQTT_SERVICE_STATE_DISCONNECTED,
                                   false,
                                   ESP_FAIL);
            if ((event->error_handle != NULL) &&
                (event->error_handle->esp_transport_sock_errno != 0)) {
                ESP_LOGE(TAG, "MQTT transport error; socket errno=%d",
                         event->error_handle->esp_transport_sock_errno);
            } else {
                ESP_LOGE(TAG, "MQTT client reported an error");
            }
            break;

        default:
            break;
    }
}

static int mqtt_service_enqueue(mqtt_service_handle_t handle,
                                const char *topic,
                                const char *payload,
                                int qos,
                                bool retain)
{
    return esp_mqtt_client_enqueue(handle->client,
                                   topic,
                                   payload,
                                   0,
                                   qos,
                                   retain ? 1 : 0,
                                   qos > 0);
}

static esp_err_t mqtt_service_publish_config_state(
    mqtt_service_handle_t handle)
{
    mqtt_runtime_config_t runtime_config;
    const esp_err_t result = mqtt_service_read_runtime_config(handle,
                                                              &runtime_config);
    if (result != ESP_OK) {
        return result;
    }

    char payload[160U];
    const int length = snprintf(payload,
                                sizeof(payload),
                                "{\"sample_interval_s\":%.3f,"
                                "\"report_interval_s\":%.3f}",
                                (double)runtime_config.sample_period_ms / 1000.0,
                                (double)runtime_config.report_period_ms / 1000.0);
    if ((length < 0) || ((size_t)length >= sizeof(payload))) {
        return ESP_ERR_INVALID_SIZE;
    }

    return (mqtt_service_enqueue(handle,
                                 handle->config_state_topic,
                                 payload,
                                 1,
                                 true) >= 0)
               ? ESP_OK
               : ESP_FAIL;
}

static esp_err_t mqtt_service_setup_session(mqtt_service_handle_t handle)
{
    const int subscribe_id = esp_mqtt_client_subscribe(
        handle->client,
        handle->config_set_topic,
        1);
    if (subscribe_id < 0) {
        return ESP_FAIL;
    }

    if (mqtt_service_enqueue(handle,
                             handle->status_topic,
                             MQTT_ONLINE_PAYLOAD,
                             1,
                             true) < 0) {
        return ESP_FAIL;
    }

    const esp_err_t result = mqtt_service_publish_config_state(handle);
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Subscribed to %s", handle->config_set_topic);
    }
    return result;
}

static esp_err_t mqtt_service_publish_telemetry(mqtt_service_handle_t handle)
{
    mqtt_environment_snapshot_t snapshot;
    const esp_err_t result =
        handle->config.get_snapshot(handle->config.snapshot_context, &snapshot);
    if (result != ESP_OK) {
        return result;
    }

    char payload[256U];
    const int length = snprintf(
        payload,
        sizeof(payload),
        "{\"temperature_c\":%.2f,\"humidity_rh\":%.2f,"
        "\"uptime_ms\":%" PRIi64 ",\"sensor_ok\":%s,"
        "\"sequence\":%" PRIu32 "}",
        (double)snapshot.temperature_c,
        (double)snapshot.humidity_percent,
        esp_timer_get_time() / 1000,
        snapshot.valid ? "true" : "false",
        snapshot.sequence_number);
    if ((length < 0) || ((size_t)length >= sizeof(payload))) {
        return ESP_ERR_INVALID_SIZE;
    }

    const int message_id = esp_mqtt_client_publish(handle->client,
                                                    handle->telemetry_topic,
                                                    payload,
                                                    length,
                                                    0,
                                                    0);
    if (message_id < 0) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Telemetry published: %s", payload);
    return ESP_OK;
}

static bool mqtt_service_json_seconds_to_ms(const cJSON *item,
                                            uint32_t minimum_ms,
                                            uint32_t maximum_ms,
                                            uint32_t *out_value_ms)
{
    if (!cJSON_IsNumber(item) || (out_value_ms == NULL)) {
        return false;
    }

    const double milliseconds = item->valuedouble * 1000.0;
    if ((milliseconds < (double)minimum_ms) ||
        (milliseconds > (double)maximum_ms)) {
        return false;
    }

    const uint32_t rounded_ms = (uint32_t)(milliseconds + 0.5);
    double difference = milliseconds - (double)rounded_ms;
    if (difference < 0.0) {
        difference = -difference;
    }
    if (difference > 0.001) {
        return false;
    }

    *out_value_ms = rounded_ms;
    return true;
}

static void mqtt_service_process_command(mqtt_service_handle_t handle,
                                         const mqtt_service_command_t *command)
{
    cJSON *root = cJSON_ParseWithLength(command->payload, command->length);
    if (!cJSON_IsObject(root)) {
        ESP_LOGW(TAG, "Rejected MQTT command: invalid JSON object");
        mqtt_service_record_rejected_command(handle);
        cJSON_Delete(root);
        return;
    }

    const cJSON *sample_item =
        cJSON_GetObjectItemCaseSensitive(root, "sample_interval_s");
    const cJSON *report_item =
        cJSON_GetObjectItemCaseSensitive(root, "report_interval_s");
    const bool has_sample_period = sample_item != NULL;
    const bool has_report_period = report_item != NULL;
    uint32_t sample_period_ms = 0U;
    uint32_t report_period_ms = 0U;

    const bool sample_valid =
        !has_sample_period ||
        mqtt_service_json_seconds_to_ms(sample_item,
                                        1000U,
                                        600000U,
                                        &sample_period_ms);
    const bool report_valid =
        !has_report_period ||
        mqtt_service_json_seconds_to_ms(report_item,
                                        MQTT_SERVICE_MIN_REPORT_PERIOD_MS,
                                        MQTT_SERVICE_MAX_REPORT_PERIOD_MS,
                                        &report_period_ms);

    if ((!has_sample_period && !has_report_period) ||
        !sample_valid || !report_valid) {
        ESP_LOGW(TAG, "Rejected MQTT command: invalid interval value");
        mqtt_service_record_rejected_command(handle);
        cJSON_Delete(root);
        return;
    }

    const mqtt_runtime_config_update_t update = {
        .has_sample_period = has_sample_period,
        .sample_period_ms = sample_period_ms,
        .has_report_period = has_report_period,
        .report_period_ms = report_period_ms,
    };
    esp_err_t result = handle->config.apply_runtime_config(
        handle->config.runtime_config_context,
        &update);
    if (result == ESP_OK) {
        result = mqtt_service_publish_config_state(handle);
    }

    if (result == ESP_OK) {
        ESP_LOGI(TAG,
                 "MQTT configuration applied: sample=%" PRIu32
                 " ms, report=%" PRIu32 " ms",
                 has_sample_period ? sample_period_ms : 0U,
                 has_report_period ? report_period_ms : 0U);
    } else {
        ESP_LOGW(TAG, "Failed to apply MQTT configuration: %s",
                 esp_err_to_name(result));
        mqtt_service_record_rejected_command(handle);
    }

    cJSON_Delete(root);
}

/** 业务任务负责 JSON 解析和发布调度，MQTT 协议任务只处理网络。 */
static void mqtt_service_task(void *context)
{
    mqtt_service_handle_t handle = (mqtt_service_handle_t)context;
    mqtt_service_set_state(handle,
                           MQTT_SERVICE_STATE_WAITING_NETWORK,
                           false,
                           ESP_OK);

    while (!handle->config.is_network_ready(handle->config.network_context)) {
        vTaskDelay(mqtt_service_ms_to_ticks(MQTT_SERVICE_NETWORK_POLL_MS));
    }

    mqtt_service_set_state(handle,
                           MQTT_SERVICE_STATE_CONNECTING,
                           false,
                           ESP_OK);
    const esp_err_t start_result = esp_mqtt_client_start(handle->client);
    if (start_result != ESP_OK) {
        mqtt_service_set_state(handle,
                               MQTT_SERVICE_STATE_DISCONNECTED,
                               false,
                               start_result);
        ESP_LOGE(TAG, "Failed to start MQTT client: %s",
                 esp_err_to_name(start_result));
        handle->task = NULL;
        vTaskDelete(NULL);
        return;
    }

    TickType_t last_report_at = xTaskGetTickCount();
    while (true) {
        const EventBits_t bits = xEventGroupGetBits(handle->event_group);

        if ((bits & MQTT_SERVICE_SESSION_SETUP_BIT) != 0U) {
            const esp_err_t setup_result = mqtt_service_setup_session(handle);
            if (setup_result == ESP_OK) {
                (void)xEventGroupClearBits(handle->event_group,
                                           MQTT_SERVICE_SESSION_SETUP_BIT);
                last_report_at = xTaskGetTickCount() -
                                 mqtt_service_ms_to_ticks(
                                     mqtt_service_get_report_period(handle));
            } else {
                ESP_LOGW(TAG, "MQTT session setup will be retried");
            }
        }

        mqtt_service_command_t command;
        while (xQueueReceive(handle->command_queue, &command, 0U) == pdTRUE) {
            mqtt_service_process_command(handle, &command);
        }

        if ((bits & MQTT_SERVICE_CONNECTED_BIT) != 0U) {
            const TickType_t now = xTaskGetTickCount();
            const TickType_t report_ticks = mqtt_service_ms_to_ticks(
                mqtt_service_get_report_period(handle));
            if ((TickType_t)(now - last_report_at) >= report_ticks) {
                const esp_err_t publish_result =
                    mqtt_service_publish_telemetry(handle);
                mqtt_service_record_publish(handle, publish_result);
                if (publish_result != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to publish telemetry: %s",
                             esp_err_to_name(publish_result));
                }
                last_report_at = now;
            }
        }

        vTaskDelay(mqtt_service_ms_to_ticks(MQTT_SERVICE_LOOP_DELAY_MS));
    }
}

static void mqtt_service_release_partial_instance(
    mqtt_service_handle_t handle)
{
    if (handle == NULL) {
        return;
    }
    if (handle->client != NULL) {
        (void)esp_mqtt_client_destroy(handle->client);
    }
    if (handle->command_queue != NULL) {
        vQueueDelete(handle->command_queue);
    }
    if (handle->event_group != NULL) {
        vEventGroupDelete(handle->event_group);
    }
    if (handle->status_mutex != NULL) {
        vSemaphoreDelete(handle->status_mutex);
    }
    free(handle);
}

esp_err_t mqtt_service_create(const mqtt_service_config_t *config,
                              mqtt_service_handle_t *out_handle)
{
    if (!mqtt_service_config_is_valid(config) || (out_handle == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;

    mqtt_service_handle_t handle = calloc(1U, sizeof(*handle));
    if (handle == NULL) {
        return ESP_ERR_NO_MEM;
    }

    (void)strcpy(handle->broker_uri, config->broker_uri);
    (void)strcpy(handle->username, config->username);
    (void)strcpy(handle->password, config->password);
    handle->config = *config;
    handle->config.broker_uri = handle->broker_uri;
    handle->config.username = handle->username;
    handle->config.password = handle->password;
    handle->status.state = MQTT_SERVICE_STATE_CREATED;
    handle->status.last_error = ESP_OK;

    handle->status_mutex = xSemaphoreCreateMutex();
    handle->event_group = xEventGroupCreate();
    handle->command_queue = xQueueCreate(MQTT_SERVICE_COMMAND_QUEUE_LENGTH,
                                         sizeof(mqtt_service_command_t));
    if ((handle->status_mutex == NULL) ||
        (handle->event_group == NULL) ||
        (handle->command_queue == NULL)) {
        mqtt_service_release_partial_instance(handle);
        return ESP_ERR_NO_MEM;
    }

    mqtt_runtime_config_t runtime_config;
    esp_err_t result = mqtt_service_read_runtime_config(handle, &runtime_config);
    if (result != ESP_OK) {
        mqtt_service_release_partial_instance(handle);
        return result;
    }

    uint8_t mac_address[6U];
    result = esp_read_mac(mac_address, ESP_MAC_WIFI_STA);
    if (result != ESP_OK) {
        mqtt_service_release_partial_instance(handle);
        return result;
    }
    (void)snprintf(handle->device_id,
                   sizeof(handle->device_id),
                   "%02x%02x%02x%02x%02x%02x",
                   mac_address[0], mac_address[1], mac_address[2],
                   mac_address[3], mac_address[4], mac_address[5]);
    (void)snprintf(handle->client_id,
                   sizeof(handle->client_id),
                   "esp32s3-env-%s",
                   handle->device_id);
    (void)snprintf(handle->telemetry_topic,
                   sizeof(handle->telemetry_topic),
                   "env/%s/telemetry",
                   handle->device_id);
    (void)snprintf(handle->status_topic,
                   sizeof(handle->status_topic),
                   "env/%s/status",
                   handle->device_id);
    (void)snprintf(handle->config_set_topic,
                   sizeof(handle->config_set_topic),
                   "env/%s/config/set",
                   handle->device_id);
    (void)snprintf(handle->config_state_topic,
                   sizeof(handle->config_state_topic),
                   "env/%s/config/state",
                   handle->device_id);

    const esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = handle->broker_uri,
        .credentials.username =
            (handle->username[0] != '\0') ? handle->username : NULL,
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
    if (handle->client == NULL) {
        mqtt_service_release_partial_instance(handle);
        return ESP_FAIL;
    }

    result = esp_mqtt_client_register_event(handle->client,
                                            ESP_EVENT_ANY_ID,
                                            mqtt_service_event_handler,
                                            handle);
    if (result != ESP_OK) {
        mqtt_service_release_partial_instance(handle);
        return result;
    }

    ESP_LOGI(TAG, "MQTT device ID: %s", handle->device_id);
    ESP_LOGI(TAG, "Telemetry topic: %s", handle->telemetry_topic);
    *out_handle = handle;
    return ESP_OK;
}

esp_err_t mqtt_service_start(mqtt_service_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->started) {
        return ESP_ERR_INVALID_STATE;
    }

    const BaseType_t task_created = xTaskCreate(mqtt_service_task,
                                                "mqtt_service",
                                                MQTT_SERVICE_TASK_STACK_SIZE,
                                                handle,
                                                MQTT_SERVICE_TASK_PRIORITY,
                                                &handle->task);
    if (task_created != pdPASS) {
        handle->task = NULL;
        return ESP_ERR_NO_MEM;
    }

    handle->started = true;
    return ESP_OK;
}

esp_err_t mqtt_service_get_status(mqtt_service_handle_t handle,
                                  mqtt_service_status_t *out_status)
{
    if ((handle == NULL) || (out_status == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(handle->status_mutex,
                       mqtt_service_ms_to_ticks(MQTT_SERVICE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    *out_status = handle->status;
    (void)xSemaphoreGive(handle->status_mutex);
    return ESP_OK;
}
