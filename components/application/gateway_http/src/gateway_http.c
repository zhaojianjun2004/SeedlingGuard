/**
 ******************************************************************************
 * @file    gateway_http.c
 * @brief   局域网监控与控制入口实现。
 * @details HTTP Server 任务中处理请求；现场控制只投递命令，结果由查询接口返回。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：应用层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "gateway_http.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

/*--------------------------- 模块配置、类型与状态 ---------------------------*/

#define GATEWAY_HTTP_TOKEN_MAX     64U
#define GATEWAY_HTTP_BODY_MAX      128U
#define GATEWAY_HTTP_WIFI_BODY_MAX 256U

static const char *TAG = "gateway_http";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

struct gateway_http
{
    gateway_link_handle_t link;
    gateway_outbox_handle_t outbox;
    mqtt_service_handle_t mqtt;
    wifi_manager_handle_t wifi;
    httpd_handle_t server;
    char token[GATEWAY_HTTP_TOKEN_MAX + 1U];
};

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 返回测点数据质量的可读名称。
 */
static const char *quality_name(uint8_t quality)
{
    switch (quality)
    {
        case GW_QUALITY_VALID:
            return "VALID";
        case GW_QUALITY_STALE:
            return "STALE";
        case GW_QUALITY_FAULT:
            return "FAULT";
        default:
            return "INVALID";
    }
}

/**
 * @brief 向 HTTP 请求返回结构化错误，不提前确认现场执行。
 */
static void send_json_error(httpd_req_t *request, const char *status, const char *message)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    char body[96];
    const int length = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);
    if (length > 0 && (size_t)length < sizeof(body))
    {
        (void)httpd_resp_send(request, body, length);
    }
    else
    {
        (void)httpd_resp_send_500(request);
    }
}

/**
 * @brief 返回嵌入式监控页面。
 */
static esp_err_t index_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

/**
 * @brief 序列化当前设备、测点、规则与链路状态。
 */
static esp_err_t status_handler(httpd_req_t *request)
{
    const gateway_http_handle_t handle = request->user_ctx;
    gateway_link_snapshot_t snapshot;
    if (gateway_link_get_snapshot(handle->link, &snapshot) != ESP_OK)
    {
        send_json_error(request, "503 Service Unavailable", "snapshot_unavailable");
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        send_json_error(request, "500 Internal Server Error", "no_memory");
        return ESP_OK;
    }
    cJSON *points = cJSON_AddArrayToObject(root, "points");
    if (points == NULL)
    {
        cJSON_Delete(root);
        send_json_error(request, "500 Internal Server Error", "no_memory");
        return ESP_OK;
    }
    cJSON_AddBoolToObject(root, "field_online", snapshot.field_online);
    cJSON_AddBoolToObject(root, "collect_enabled", snapshot.collect_enabled);
    cJSON_AddBoolToObject(root, "control_enabled", handle->token[0] != '\0');
    cJSON_AddNumberToObject(root, "sample_period_ms", snapshot.sample_period_ms);
    cJSON_AddNumberToObject(root, "boot_id", snapshot.boot_id);
    cJSON *devices = cJSON_AddArrayToObject(root, "devices");
    if (!devices)
    {
        cJSON_Delete(root);
        send_json_error(request, "500 Internal Server Error", "no_memory");
        return ESP_OK;
    }
    for (unsigned i = 0; i < NURSERY_DEVICE_COUNT; i++)
    {
        cJSON_AddItemToArray(devices,
                             cJSON_CreateNumber(snapshot.field_online ? snapshot.devices[i] : 0));
    }
    cJSON_AddNumberToObject(root, "config_version", snapshot.config_version);
    cJSON_AddBoolToObject(root, "field_storage_ready", snapshot.storage_ready);
    cJSON_AddNumberToObject(root, "field_sample_drops", snapshot.sample_drops);
    cJSON_AddNumberToObject(root, "field_log_drops", snapshot.log_drops);
    cJSON_AddNumberToObject(root, "field_log_pending", snapshot.log_pending);
    cJSON_AddBoolToObject(root, "synchronized", snapshot.synchronized);
    cJSON_AddNumberToObject(root, "last_event_id", snapshot.last_event_id);
    cJSON_AddNumberToObject(root, "session_generation", snapshot.session_generation);
    cJSON_AddNumberToObject(root, "uart_rx_bytes", snapshot.uart_rx_bytes);
    cJSON_AddNumberToObject(root, "received_frames", snapshot.received_frames);
    cJSON_AddNumberToObject(root, "crc_errors", snapshot.crc_errors);
    cJSON_AddNumberToObject(root, "framing_errors", snapshot.framing_errors);
    wifi_manager_status_t wifi_status;
    if (wifi_manager_get_status(handle->wifi, &wifi_status) == ESP_OK)
    {
        cJSON_AddBoolToObject(root, "provisioning_ap_active", wifi_status.ap_active);
        cJSON_AddBoolToObject(root, "wifi_connected", wifi_status.has_ipv4);
        cJSON_AddStringToObject(root, "wifi_ip", wifi_status.ipv4_address);
    }
    gateway_outbox_status_t outbox_status;
    if (handle->outbox != NULL &&
        gateway_outbox_get_status(handle->outbox, &outbox_status) == ESP_OK)
    {
        cJSON_AddNumberToObject(root, "pending_records", outbox_status.pending_count);
        cJSON_AddNumberToObject(root, "dropped_records", outbox_status.dropped_count);
    }
    mqtt_service_status_t mqtt_status;
    if (handle->mqtt != NULL && mqtt_service_get_status(handle->mqtt, &mqtt_status) == ESP_OK)
    {
        cJSON_AddBoolToObject(root, "mqtt_connected", mqtt_status.connected);
        cJSON_AddNumberToObject(root, "telemetry_sent", mqtt_status.published_telemetry_count);
        cJSON_AddNumberToObject(root, "broker_acked", mqtt_status.broker_ack_count);
        cJSON_AddNumberToObject(root, "field_applied_commands",
                                mqtt_status.field_applied_command_count);
    }

    const int64_t now = esp_timer_get_time();
    const uint32_t fresh_limit_ms = snapshot.sample_period_ms * 2U + 2000U;
    for (unsigned i = 0U; i < NURSERY_POINT_COUNT; ++i)
    {
        const gateway_point_snapshot_t *point = &snapshot.points[i];
        cJSON *item = cJSON_CreateObject();
        if (item == NULL)
        {
            cJSON_Delete(root);
            send_json_error(request, "500 Internal Server Error", "no_memory");
            return ESP_OK;
        }
        cJSON_AddItemToArray(points, item);
        cJSON_AddNumberToObject(item, "point_id", i + 1U);
        cJSON_AddNumberToObject(item, "alarm_state", point->alarm_state);
        cJSON_AddNumberToObject(item, "low", point->rule.low);
        cJSON_AddNumberToObject(item, "high", point->rule.high);
        cJSON_AddNumberToObject(item, "hysteresis", point->rule.hysteresis);
        uint32_t age_ms = gateway_point_age_ms(point, now);
        uint8_t quality = point->received ? point->quality : GW_QUALITY_INVALID;
        if (quality == GW_QUALITY_VALID && (!snapshot.field_online || age_ms > fresh_limit_ms))
        {
            quality = GW_QUALITY_STALE;
        }
        cJSON_AddStringToObject(item, "quality", quality_name(quality));
        cJSON_AddBoolToObject(item, "valid", quality == GW_QUALITY_VALID);
        if (age_ms != UINT32_MAX)
        {
            cJSON_AddNumberToObject(item, "age_ms", age_ms);
        }
        if (quality == GW_QUALITY_VALID)
        {
            cJSON_AddNumberToObject(item, "value_scaled", point->scaled_value);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL)
    {
        send_json_error(request, "500 Internal Server Error", "no_memory");
        return ESP_OK;
    }
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const esp_err_t result = httpd_resp_send(request, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return result;
}

/**
 * @brief 校验控制请求携带的令牌。
 */
static bool authorized(httpd_req_t *request, const char *token)
{
    const size_t expected = strlen(token);
    const size_t actual = httpd_req_get_hdr_value_len(request, "X-Gateway-Token");
    if (expected == 0U || actual != expected || actual > GATEWAY_HTTP_TOKEN_MAX)
    {
        return false;
    }
    char supplied[GATEWAY_HTTP_TOKEN_MAX + 1U];
    if (httpd_req_get_hdr_value_str(request, "X-Gateway-Token", supplied, sizeof(supplied)) !=
        ESP_OK)
    {
        return false;
    }
    unsigned diff = 0U;
    for (size_t i = 0U; i < expected; ++i)
    {
        diff |= (unsigned)(supplied[i] ^ token[i]);
    }
    return diff == 0U;
}

/**
 * @brief 仅在配网模式接收并提交候选 Wi-Fi 凭据。
 */
static esp_err_t wifi_provision_handler(httpd_req_t *request)
{
    const gateway_http_handle_t handle = request->user_ctx;
    wifi_manager_status_t status;
    if (wifi_manager_get_status(handle->wifi, &status) != ESP_OK || !status.ap_active ||
        status.has_ipv4)
    {
        send_json_error(request, "409 Conflict", "provisioning_ap_inactive");
        return ESP_OK;
    }
    if (request->content_len <= 0 || request->content_len > GATEWAY_HTTP_WIFI_BODY_MAX)
    {
        send_json_error(request, "413 Payload Too Large", "invalid_body_length");
        return ESP_OK;
    }
    char body[GATEWAY_HTTP_WIFI_BODY_MAX + 1U];
    size_t received = 0U;
    while (received < (size_t)request->content_len)
    {
        const int count =
            httpd_req_recv(request, body + received, (size_t)request->content_len - received);
        if (count <= 0)
        {
            send_json_error(request, "400 Bad Request", "body_incomplete");
            return ESP_OK;
        }
        received += (size_t)count;
    }
    body[received] = '\0';
    cJSON *root = cJSON_ParseWithLength(body, received);
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        send_json_error(request, "400 Bad Request", "invalid_credentials");
        return ESP_OK;
    }
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (!cJSON_IsString(ssid) || !cJSON_IsString(password))
    {
        cJSON_Delete(root);
        send_json_error(request, "400 Bad Request", "invalid_credentials");
        return ESP_OK;
    }
    const esp_err_t error =
        wifi_manager_submit_credentials(handle->wifi, ssid->valuestring, password->valuestring);
    cJSON_Delete(root);
    if (error != ESP_OK)
    {
        send_json_error(request, error == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "409 Conflict",
                        "credentials_rejected");
        return ESP_OK;
    }
    httpd_resp_set_status(request, "202 Accepted");
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    return httpd_resp_send(request, "{\"status\":\"connecting\"}", HTTPD_RESP_USE_STRLEN);
}

/**
 * @brief 校验控制参数，将请求投递到统一命令服务并返回受理编号。
 */
static esp_err_t control_handler(httpd_req_t *request)
{
    const gateway_http_handle_t handle = request->user_ctx;
    if (!authorized(request, handle->token))
    {
        send_json_error(request, "403 Forbidden", "control_token_required");
        return ESP_OK;
    }
    if (request->content_len <= 0 || request->content_len > GATEWAY_HTTP_BODY_MAX)
    {
        send_json_error(request, "413 Payload Too Large", "invalid_body_length");
        return ESP_OK;
    }
    char body[GATEWAY_HTTP_BODY_MAX + 1U];
    size_t received = 0U;
    while (received < (size_t)request->content_len)
    {
        const int count =
            httpd_req_recv(request, body + received, (size_t)request->content_len - received);
        if (count <= 0)
        {
            send_json_error(request, "400 Bad Request", "body_incomplete");
            return ESP_OK;
        }
        received += (size_t)count;
    }
    body[received] = '\0';
    cJSON *root = cJSON_ParseWithLength(body, received);
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        send_json_error(request, "400 Bad Request", "invalid_command");
        return ESP_OK;
    }
    const cJSON *op_item = cJSON_GetObjectItemCaseSensitive(root, "op");
    const cJSON *period_item = cJSON_GetObjectItemCaseSensitive(root, "period_ms");
    const cJSON *point = cJSON_GetObjectItemCaseSensitive(root, "point_id");
    const cJSON *threshold = cJSON_GetObjectItemCaseSensitive(root, "value_scaled");
    const cJSON *request_item = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    uint8_t point_id = 0;
    uint32_t request_id = esp_random() & 0x1fffffffU;
    if (!request_id)
    {
        request_id = 1;
    }
    if (request_item)
    {
        if (!cJSON_IsNumber(request_item) || request_item->valuedouble < 1 ||
            request_item->valuedouble > 536870911 ||
            request_item->valuedouble != (double)(uint32_t)request_item->valuedouble)
        {
            cJSON_Delete(root);
            send_json_error(request, "400 Bad Request", "invalid_request_id");
            return ESP_OK;
        }
        request_id = (uint32_t)request_item->valuedouble;
    }
    gw_command_op_t op;
    uint32_t value = 0U;
    if (!cJSON_IsString(op_item))
    {
        cJSON_Delete(root);
        send_json_error(request, "400 Bad Request", "invalid_command");
        return ESP_OK;
    }
    if (strcmp(op_item->valuestring, "start") == 0)
    {
        op = GW_CMD_SET_ENABLE;
        value = 1U;
    }
    else if (strcmp(op_item->valuestring, "stop") == 0)
    {
        op = GW_CMD_SET_ENABLE;
    }
    else if (strcmp(op_item->valuestring, "period") == 0 && cJSON_IsNumber(period_item) &&
             period_item->valuedouble >= NURSERY_PERIOD_MIN_MS &&
             period_item->valuedouble <= 600000.0 &&
             (double)(uint32_t)period_item->valuedouble == period_item->valuedouble)
    {
        op = GW_CMD_SET_PERIOD;
        value = (uint32_t)period_item->valuedouble;
    }
    else if ((!strcmp(op_item->valuestring, "low") || !strcmp(op_item->valuestring, "high") ||
              !strcmp(op_item->valuestring, "hysteresis")) &&
             cJSON_IsNumber(point) && point->valuedouble >= 1 && point->valuedouble <= 9 &&
             point->valuedouble == (double)(unsigned)point->valuedouble &&
             cJSON_IsNumber(threshold) && threshold->valuedouble >= -400 &&
             threshold->valuedouble <= 200000 &&
             threshold->valuedouble == (double)(int32_t)threshold->valuedouble)
    {
        op = !strcmp(op_item->valuestring, "low")    ? GW_CMD_SET_LOW
             : !strcmp(op_item->valuestring, "high") ? GW_CMD_SET_HIGH
                                                     : GW_CMD_SET_HYSTERESIS;
        value = (uint32_t)(int32_t)threshold->valuedouble;
        point_id = (uint8_t)point->valueint;
    }
    else
    {
        cJSON_Delete(root);
        send_json_error(request, "400 Bad Request", "invalid_command");
        return ESP_OK;
    }
    cJSON_Delete(root);

    esp_err_t error = gateway_link_submit(handle->link, op, value, point_id, request_id);
    if (error != ESP_OK)
    {
        send_json_error(request,
                        error == ESP_ERR_NO_MEM ? "503 Service Unavailable" : "409 Conflict",
                        "command_rejected");
        return ESP_OK;
    }
    char response[96];
    snprintf(response, sizeof(response), "{\"request_id\":%lu,\"status\":\"PENDING\"}",
             (unsigned long)request_id);
    httpd_resp_set_status(request, "202 Accepted");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, response);
}

/**
 * @brief 查询有限请求槽中保存的命令执行状态。
 */
static esp_err_t command_status_handler(httpd_req_t *request)
{
    gateway_http_handle_t handle = request->user_ctx;
    if (!authorized(request, handle->token))
    {
        send_json_error(request, "403 Forbidden", "control_token_required");
        return ESP_OK;
    }
    char *end;
    const char *number = request->uri + strlen("/api/commands/");
    unsigned long id = strtoul(number, &end, 10);
    if (!*number || *end || !id || id > UINT32_MAX)
    {
        send_json_error(request, "400 Bad Request", "invalid_request_id");
        return ESP_OK;
    }
    gateway_command_status_t status;
    if (gateway_link_command_status(handle->link, (uint32_t)id, &status) != ESP_OK)
    {
        send_json_error(request, "404 Not Found", "command_not_found");
        return ESP_OK;
    }
    static const char *names[] = {"PENDING", "APPLIED", "FAILED", "UNCONFIRMED"};
    char response[256];
    snprintf(response, sizeof(response),
             "{\"request_id\":%lu,\"status\":\"%s\",\"error\":%d,\"collect_enabled\":%s,\"sample_"
             "period_ms\":%lu,\"config_version\":%lu}",
             id, names[status.state], (int)status.error,
             status.result.collect_enabled ? "true" : "false",
             (unsigned long)status.result.sample_period_ms,
             (unsigned long)status.result.config_version);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, response);
}

/**
 * @brief 启动局域网监控与控制入口的运行入口。
 * @param[in] config 模块配置及其取值约束。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t gateway_http_start(const gateway_http_config_t *config, gateway_http_handle_t *out_handle)
{
    if (config == NULL || out_handle == NULL || config->link == NULL || config->wifi == NULL ||
        config->control_token == NULL || config->port == 0U ||
        strlen(config->control_token) > GATEWAY_HTTP_TOKEN_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    gateway_http_handle_t handle = calloc(1U, sizeof(*handle));
    if (handle == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    handle->link = config->link;
    handle->outbox = config->outbox;
    handle->mqtt = config->mqtt;
    handle->wifi = config->wifi;
    memcpy(handle->token, config->control_token, strlen(config->control_token) + 1U);
    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.server_port = config->port;
    server_config.stack_size = 8192U;
    server_config.max_uri_handlers = 5U;
    server_config.uri_match_fn = httpd_uri_match_wildcard;
    server_config.lru_purge_enable = true;
    esp_err_t error = httpd_start(&handle->server, &server_config);
    if (error == ESP_OK)
    {
        const httpd_uri_t routes[] = {
            {.uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = handle},
            {.uri = "/api/status",
             .method = HTTP_GET,
             .handler = status_handler,
             .user_ctx = handle},
            {.uri = "/api/control",
             .method = HTTP_POST,
             .handler = control_handler,
             .user_ctx = handle},
            {.uri = "/api/commands/*",
             .method = HTTP_GET,
             .handler = command_status_handler,
             .user_ctx = handle},
            {.uri = "/api/wifi",
             .method = HTTP_POST,
             .handler = wifi_provision_handler,
             .user_ctx = handle},
        };
        for (size_t i = 0U; i < sizeof(routes) / sizeof(routes[0]); ++i)
        {
            error = httpd_register_uri_handler(handle->server, &routes[i]);
            if (error != ESP_OK)
            {
                break;
            }
        }
    }
    if (error != ESP_OK)
    {
        if (handle->server != NULL)
        {
            httpd_stop(handle->server);
        }
        free(handle);
        return error;
    }
    *out_handle = handle;
    ESP_LOGI(TAG, "Local dashboard listening on port %u; controls %s", (unsigned)config->port,
             handle->token[0] ? "enabled" : "read-only");
    return ESP_OK;
}
