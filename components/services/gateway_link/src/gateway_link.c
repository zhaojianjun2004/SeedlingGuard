/**
 ******************************************************************************
 * @file    gateway_link.c
 * @brief   现场链路与命令服务实现。
 * @details 接收任务更新快照并匹配回执，命令任务串行发送；共享快照由互斥量保护。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：业务服务层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "gateway_link.h"

#include <stdlib.h>
#include <string.h>

#include "uart_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"

/*--------------------------- 模块配置、类型与状态 ---------------------------*/

#define GATEWAY_LINK_OFFLINE_TIMEOUT_US 3000000LL

static const char *TAG = "gateway_link";

struct gateway_link
{
    gateway_link_config_t config;
    SemaphoreHandle_t snapshot_mutex;
    SemaphoreHandle_t command_mutex;
    SemaphoreHandle_t pending_mutex;
    SemaphoreHandle_t tx_mutex;
    SemaphoreHandle_t command_done;
    TaskHandle_t task;
    gw_parser_t parser;
    gateway_link_snapshot_t snapshot;
    gw_result_t pending_result;
    uint16_t next_sequence;
    uint16_t last_status_sequence;
    bool status_seen;
    volatile uint16_t pending_sequence;
    uint8_t pending_op;
    volatile uint32_t pending_request_id;
    volatile bool pending_command;
    bool started;
    QueueHandle_t requests;
    QueueHandle_t events;
    SemaphoreHandle_t requests_mutex;
    gateway_command_status_t slots[8];
    bool used[8];
    unsigned next_slot;
    uint16_t pending_session;
    TaskHandle_t command_task;
};

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 将链路等待时间转换为至少一个 RTOS tick。
 */
static TickType_t gateway_link_ms_to_ticks(uint32_t milliseconds)
{
    const TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    return ticks == 0 ? 1 : ticks;
}

/**
 * @brief 按半区间规则判断 16 位帧序号是否更新。
 */
static bool gateway_sequence_is_newer(uint16_t incoming, uint16_t previous)
{
    const uint16_t difference = (uint16_t)(incoming - previous);
    return difference != 0U && difference < 0x8000U;
}

/**
 * @brief 从四个小端字节还原无符号整数。
 */
static uint32_t gateway_read_u32_le(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

/**
 * @brief 根据测点、规则和设备掩码更新完整同步标记。
 */
static void gateway_update_sync_progress(gateway_link_snapshot_t *snapshot)
{
    snapshot->synchronized = nursery_snapshot_complete(snapshot->points_mask, snapshot->rules_mask,
                                                       snapshot->devices_mask);
}

/**
 * @brief 清空同步进度并将旧有效样本标为过期。
 */
static void gateway_invalidate_snapshot(gateway_link_snapshot_t *snapshot)
{
    snapshot->points_mask = snapshot->rules_mask = 0;
    snapshot->devices_mask = 0;
    snapshot->synchronized = false;
    for (unsigned point_index = 0; point_index < NURSERY_POINT_COUNT; point_index++)
    {
        if (snapshot->points[point_index].quality == GW_QUALITY_VALID)
        {
            snapshot->points[point_index].quality = GW_QUALITY_STALE;
            snapshot->points[point_index].alarm_state = NURSERY_ALARM_UNKNOWN;
        }
    }
}

/**
 * @brief 处理当前启动会话的状态帧，识别会话或配置版本变化。
 */
static void gateway_process_status(gateway_link_handle_t handle, const gw_frame_t *frame,
                                   int64_t received_at_us)
{
    gw_status_t status;
    if (!gw_status_unpack(frame, &status) || frame->session_id != status.boot_id)
    {
        return;
    }
    if (xSemaphoreTake(handle->snapshot_mutex, portMAX_DELAY) != pdTRUE)
    {
        return;
    }
    const bool state_changed =
        !handle->snapshot.field_online || handle->snapshot.boot_id != status.boot_id;
    const bool sequence_reset =
        handle->status_seen && frame->seq != handle->last_status_sequence &&
        !gateway_sequence_is_newer(frame->seq, handle->last_status_sequence);
    if (state_changed || sequence_reset)
    {
        memset(handle->snapshot.points, 0, sizeof(handle->snapshot.points));
        gateway_invalidate_snapshot(&handle->snapshot);
        memset(handle->snapshot.devices, 0, sizeof(handle->snapshot.devices));
        ++handle->snapshot.session_generation;
    }
    handle->last_status_sequence = frame->seq;
    handle->status_seen = true;
    handle->snapshot.boot_id = status.boot_id;
    if (handle->snapshot.config_version != status.config_version)
    {
        gateway_invalidate_snapshot(&handle->snapshot);
    }
    handle->snapshot.config_version = status.config_version;
    handle->snapshot.storage_ready = status.storage_ready;
    handle->snapshot.sample_drops = status.sample_drops;
    handle->snapshot.log_drops = status.log_drops;
    handle->snapshot.latest_event_id = status.latest_event_id;
    handle->snapshot.log_pending = status.log_pending;
    handle->snapshot.collect_enabled = status.collect_enabled;
    handle->snapshot.sample_period_ms = status.sample_period_ms;
    handle->snapshot.field_online = true;
    handle->snapshot.last_frame_at_us = received_at_us;
    ++handle->snapshot.received_frames;
    xSemaphoreGive(handle->snapshot_mutex);
    if (state_changed)
    {
        ESP_LOGI(TAG, "F407 online: boot=%u collect=%u period=%lu ms", status.boot_id,
                 status.collect_enabled, (unsigned long)status.sample_period_ms);
    }
}

/**
 * @brief 接收当前配置版本的样本，维护序号、年龄和质量投影。
 */
static void gateway_process_sample(gateway_link_handle_t handle, const gw_frame_t *frame,
                                   int64_t received_at_us)
{
    gw_sample_t sample;
    if (!gw_sample_unpack(frame, &sample) || frame->session_id != handle->snapshot.boot_id ||
        !handle->status_seen || sample.config_version != handle->snapshot.config_version)
    {
        return;
    }
    if (xSemaphoreTake(handle->snapshot_mutex, portMAX_DELAY) != pdTRUE)
    {
        return;
    }
    gateway_point_snapshot_t *point = &handle->snapshot.points[sample.point_id - 1U];
    if (!point->received || gateway_sequence_is_newer(frame->seq, point->sequence))
    {
        point->scaled_value = sample.scaled_value;
        point->quality = sample.quality;
        point->sequence = frame->seq;
        point->received = true;
        point->sample_age_ms = sample.sample_age_ms;
        point->received_at_us = received_at_us;
        point->alarm_state = sample.alarm_state;
        handle->snapshot.points_mask |= (uint16_t)(1U << (sample.point_id - 1));
        gateway_update_sync_progress(&handle->snapshot);
    }
    handle->snapshot.field_online = true;
    handle->snapshot.last_frame_at_us = received_at_us;
    ++handle->snapshot.received_frames;
    xSemaphoreGive(handle->snapshot_mutex);
}

/**
 * @brief 匹配会话、序号、操作和请求号，再唤醒等待命令。
 */
static void gateway_process_result(gateway_link_handle_t handle, const gw_frame_t *frame,
                                   int64_t received_at_us)
{
    gw_result_t result;
    if (!gw_result_unpack(frame, &result) || frame->session_id != handle->snapshot.boot_id)
    {
        return;
    }
    if (xSemaphoreTake(handle->pending_mutex, portMAX_DELAY) == pdTRUE)
    {
        const bool matched = handle->pending_command && frame->seq == handle->pending_sequence &&
                             frame->session_id == handle->pending_session &&
                             result.op == handle->pending_op &&
                             result.request_id == handle->pending_request_id;
        if (xSemaphoreTake(handle->snapshot_mutex, portMAX_DELAY) == pdTRUE)
        {
            if (matched)
            {
                if (handle->snapshot.config_version != result.config_version)
                {
                    gateway_invalidate_snapshot(&handle->snapshot);
                }
                handle->snapshot.config_version = result.config_version;
                handle->snapshot.collect_enabled = result.collect_enabled;
                handle->snapshot.sample_period_ms = result.sample_period_ms;
            }
            handle->snapshot.field_online = true;
            handle->snapshot.last_frame_at_us = received_at_us;
            ++handle->snapshot.received_frames;
            xSemaphoreGive(handle->snapshot_mutex);
        }
        if (matched)
        {
            handle->pending_result = result;
            handle->pending_command = false;
            xSemaphoreGive(handle->command_done);
        }
        xSemaphoreGive(handle->pending_mutex);
    }
}

/**
 * @brief 按消息类型分发已校验的 UART 帧。
 */
static void gateway_process_frame(gateway_link_handle_t handle, const gw_frame_t *frame)
{
    const int64_t now = esp_timer_get_time();
    switch (frame->type)
    {
        case GW_MSG_STATUS:
            gateway_process_status(handle, frame, now);
            break;
        case GW_MSG_SAMPLE:
            gateway_process_sample(handle, frame, now);
            break;
        case GW_MSG_RESULT:
            gateway_process_result(handle, frame, now);
            break;
        case GW_MSG_DEVICE:
            if (frame->session_id == handle->snapshot.boot_id && frame->length == 6 &&
                gateway_read_u32_le(frame->payload + 2) == handle->snapshot.config_version &&
                frame->payload[0] >= 1 && frame->payload[0] <= NURSERY_DEVICE_COUNT &&
                frame->payload[1] <= 4)
            {
                xSemaphoreTake(handle->snapshot_mutex, portMAX_DELAY);
                handle->snapshot.devices[frame->payload[0] - 1] = frame->payload[1];
                handle->snapshot.devices_mask |= (uint8_t)(1U << (frame->payload[0] - 1));
                gateway_update_sync_progress(&handle->snapshot);
                xSemaphoreGive(handle->snapshot_mutex);
            }
            break;
        case GW_MSG_RULE:
            if (frame->session_id == handle->snapshot.boot_id && frame->length == 17 &&
                gateway_read_u32_le(frame->payload + 13) == handle->snapshot.config_version &&
                frame->payload[0] >= 1 && frame->payload[0] <= NURSERY_POINT_COUNT)
            {
                nursery_rule_t rule;
                int32_t values[3];
                for (unsigned rule_index = 0; rule_index < 3; rule_index++)
                {
                    uint32_t field_value = 0;
                    for (unsigned byte_index = 0; byte_index < 4; byte_index++)
                    {
                        field_value |= (uint32_t)frame->payload[1 + rule_index * 4 + byte_index]
                                       << (8 * byte_index);
                    }
                    values[rule_index] = (int32_t)field_value;
                }
                rule = (nursery_rule_t){values[0], values[1], values[2]};
                xSemaphoreTake(handle->snapshot_mutex, portMAX_DELAY);
                handle->snapshot.points[frame->payload[0] - 1].rule = rule;
                handle->snapshot.rules_mask |= (uint16_t)(1U << (frame->payload[0] - 1));
                gateway_update_sync_progress(&handle->snapshot);
                xSemaphoreGive(handle->snapshot_mutex);
            }
            break;
        case GW_MSG_EVENT:
            if (frame->session_id == handle->snapshot.boot_id && frame->length == 18)
            {
                uint32_t event_id = 0;
                for (unsigned byte_index = 0; byte_index < 4; byte_index++)
                {
                    event_id |= (uint32_t)frame->payload[byte_index] << (8 * byte_index);
                }
                xSemaphoreTake(handle->snapshot_mutex, portMAX_DELAY);
                gateway_event_t event = {.id = event_id,
                                         .field_boot_id = (uint16_t)frame->payload[16] |
                                                          ((uint16_t)frame->payload[17] << 8),
                                         .point = frame->payload[12],
                                         .state = frame->payload[13],
                                         .kind = frame->payload[14],
                                         .quality = frame->payload[15]};
                uint32_t event_value = 0;
                for (unsigned byte_index = 0; byte_index < 4; byte_index++)
                {
                    event.uptime |= (uint32_t)frame->payload[4 + byte_index] << (8 * byte_index);
                    event_value |= (uint32_t)frame->payload[8 + byte_index] << (8 * byte_index);
                }
                event.value = (int32_t)event_value;
                if (xQueueSend(handle->events, &event, 0) != pdTRUE)
                {
                    ++handle->snapshot.event_drops;
                }
                handle->snapshot.last_event_id = event_id;
                handle->snapshot.last_event_point = frame->payload[12];
                handle->snapshot.last_event_state = frame->payload[13];
                handle->snapshot.last_event_kind = frame->payload[14];
                xSemaphoreGive(handle->snapshot_mutex);
            }
            break;
        default:
            break;
    }
}

/**
 * @brief 持续读取 UART 字节、解析帧并检查现场心跳超时。
 * @note 任务入口，只在任务上下文运行；正常情况下不返回。
 */
static void gateway_link_task(void *context)
{
    gateway_link_handle_t handle = (gateway_link_handle_t)context;
    uint8_t bytes[128];
    int64_t last_diagnostic_at_us = esp_timer_get_time();
    bool first_diagnostic = true;
    for (;;)
    {
        const int read = nursery_uart_read(handle->config.uart, bytes, sizeof(bytes), 100U);
        for (int i = 0; i < read; ++i)
        {
            gw_frame_t frame;
            if (gw_parser_push(&handle->parser, bytes[i], &frame))
            {
                gateway_process_frame(handle, &frame);
            }
        }
        bool log_offline = false;
        bool log_diagnostic = false;
        bool logged_online = false;
        uint16_t logged_boot_id = 0U;
        uint32_t logged_bytes = 0U;
        uint32_t logged_frames = 0U;
        uint32_t logged_crc = 0U;
        uint32_t logged_framing = 0U;
        if (xSemaphoreTake(handle->snapshot_mutex, gateway_link_ms_to_ticks(10U)) == pdTRUE)
        {
            const int64_t now = esp_timer_get_time();
            if (read > 0)
            {
                handle->snapshot.uart_rx_bytes += (uint32_t)read;
            }
            if (handle->snapshot.last_frame_at_us == 0 ||
                now - handle->snapshot.last_frame_at_us > GATEWAY_LINK_OFFLINE_TIMEOUT_US)
            {
                const bool was_online = handle->snapshot.field_online;
                handle->snapshot.field_online = false;
                gateway_invalidate_snapshot(&handle->snapshot);
                for (unsigned point = 0; point < NURSERY_POINT_COUNT; ++point)
                {
                    if (handle->snapshot.points[point].received &&
                        handle->snapshot.points[point].quality == GW_QUALITY_VALID)
                    {
                        handle->snapshot.points[point].quality = GW_QUALITY_STALE;
                    }
                }
                log_offline = was_online;
            }
            handle->snapshot.crc_errors = handle->parser.crc_errors;
            handle->snapshot.framing_errors = handle->parser.framing_errors;
            const int64_t diagnostic_interval_us =
                first_diagnostic || !handle->snapshot.field_online ? 5000000LL : 30000000LL;
            if (now - last_diagnostic_at_us >= diagnostic_interval_us)
            {
                logged_online = handle->snapshot.field_online;
                logged_boot_id = handle->snapshot.boot_id;
                logged_bytes = handle->snapshot.uart_rx_bytes;
                logged_frames = handle->snapshot.received_frames;
                logged_crc = handle->snapshot.crc_errors;
                logged_framing = handle->snapshot.framing_errors;
                log_diagnostic = true;
                first_diagnostic = false;
                last_diagnostic_at_us = now;
            }
            xSemaphoreGive(handle->snapshot_mutex);
        }
        if (log_offline)
        {
            ESP_LOGW(TAG, "F407 UART link timed out");
        }
        if (log_diagnostic)
        {
            if (logged_online)
            {
                ESP_LOGI(TAG,
                         "F407 UART: online=1 boot=%u rx_bytes=%lu frames=%lu crc=%lu framing=%lu",
                         (unsigned)logged_boot_id, (unsigned long)logged_bytes,
                         (unsigned long)logged_frames, (unsigned long)logged_crc,
                         (unsigned long)logged_framing);
            }
            else
            {
                ESP_LOGW(TAG, "F407 UART: online=0 rx_bytes=%lu frames=%lu crc=%lu framing=%lu",
                         (unsigned long)logged_bytes, (unsigned long)logged_frames,
                         (unsigned long)logged_crc, (unsigned long)logged_framing);
            }
        }
    }
}

/**
 * @brief 创建现场链路与命令服务实例并准备所需资源。
 * @param[in] config 模块配置及其取值约束。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t gateway_link_create(const gateway_link_config_t *config,
                              gateway_link_handle_t *out_handle)
{
    if (config == NULL || out_handle == NULL || config->uart == NULL ||
        config->task_stack_size < 2048U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    gateway_link_handle_t handle = calloc(1, sizeof(*handle));
    if (handle == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    handle->config = *config;
    handle->snapshot_mutex = xSemaphoreCreateMutex();
    handle->command_mutex = xSemaphoreCreateMutex();
    handle->pending_mutex = xSemaphoreCreateMutex();
    handle->tx_mutex = xSemaphoreCreateMutex();
    handle->command_done = xSemaphoreCreateBinary();
    if (handle->snapshot_mutex == NULL || handle->command_mutex == NULL ||
        handle->pending_mutex == NULL || handle->tx_mutex == NULL || handle->command_done == NULL)
    {
        if (handle->snapshot_mutex != NULL)
        {
            vSemaphoreDelete(handle->snapshot_mutex);
        }
        if (handle->command_mutex != NULL)
        {
            vSemaphoreDelete(handle->command_mutex);
        }
        if (handle->pending_mutex != NULL)
        {
            vSemaphoreDelete(handle->pending_mutex);
        }
        if (handle->tx_mutex != NULL)
        {
            vSemaphoreDelete(handle->tx_mutex);
        }
        if (handle->command_done != NULL)
        {
            vSemaphoreDelete(handle->command_done);
        }
        free(handle);
        return ESP_ERR_NO_MEM;
    }
    gw_parser_init(&handle->parser);
    *out_handle = handle;
    return ESP_OK;
}

static esp_err_t gateway_execute_command(gateway_link_handle_t, gw_command_op_t, uint32_t, uint32_t,
                                         uint32_t, uint8_t, gw_result_t *);

/**
 * @brief 串行执行请求槽中的现场命令，超时后查询实际状态。
 * @note 任务入口，只在任务上下文运行；正常情况下不返回。
 */
static void gateway_command_task(void *context)
{
    gateway_link_handle_t handle = context;
    unsigned slot_index;
    for (;;)
    {
        if (xQueueReceive(handle->requests, &slot_index, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }
        xSemaphoreTake(handle->requests_mutex, portMAX_DELAY);
        gateway_command_status_t current = handle->slots[slot_index];
        xSemaphoreGive(handle->requests_mutex);
        gateway_link_snapshot_t snapshot;
        esp_err_t error = gateway_link_get_snapshot(handle, &snapshot);
        if (error == ESP_OK && snapshot.session_generation != current.session_generation)
        {
            error = ESP_ERR_INVALID_STATE;
        }
        if (error == ESP_OK)
        {
            error = gateway_execute_command(handle, current.command.op, current.command.value,
                                            current.command.request_id, 5000,
                                            current.command.point_id, &current.result);
        }
        current.error = error;
        current.state = error == ESP_OK            ? GW_COMMAND_APPLIED
                        : error == ESP_ERR_TIMEOUT ? GW_COMMAND_UNCONFIRMED
                                                   : GW_COMMAND_FAILED;
        xSemaphoreTake(handle->requests_mutex, portMAX_DELAY);
        handle->slots[slot_index] = current;
        xSemaphoreGive(handle->requests_mutex);
        /* Timeout is uncertain: refresh actual state; do not blindly reissue writes. */
        if (error == ESP_ERR_TIMEOUT)
        {
            gw_result_t actual;
            (void)gateway_execute_command(handle, GW_CMD_QUERY_STATUS, 0,
                                          current.command.request_id ^ 0x20000000U, 1500, 0,
                                          &actual);
        }
    }
}

/**
 * @brief 启动现场链路与命令服务的运行入口。
 * @param[in] handle 已创建的模块实例句柄。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t gateway_link_start(gateway_link_handle_t handle)
{
    if (handle == NULL || handle->started)
    {
        return ESP_ERR_INVALID_STATE;
    }
    handle->events = xQueueCreate(64, sizeof(gateway_event_t));
    handle->requests = xQueueCreate(8, sizeof(unsigned));
    handle->requests_mutex = xSemaphoreCreateMutex();
    if (!handle->requests || !handle->requests_mutex || !handle->events)
    {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(gateway_link_task, "gateway_link", handle->config.task_stack_size, handle,
                    handle->config.task_priority, &handle->task) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }
    handle->started = true;
    if (xTaskCreate(gateway_command_task, "gateway_command", 4096, handle, 5,
                    &handle->command_task) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "F407 link service started with application-supplied UART port");
    return ESP_OK;
}

/**
 * @brief 在互斥保护下复制完整现场快照。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_snapshot 接收复制出的完整状态或配置。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 * @note 只短时间持锁复制；调用方在锁外完成格式化或网络处理。
 */
esp_err_t gateway_link_get_snapshot(gateway_link_handle_t handle,
                                    gateway_link_snapshot_t *out_snapshot)
{
    if (handle == NULL || out_snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(handle->snapshot_mutex, gateway_link_ms_to_ticks(20U)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    *out_snapshot = handle->snapshot;
    xSemaphoreGive(handle->snapshot_mutex);
    return ESP_OK;
}

/**
 * @brief 完成一笔 UART 命令发送及现场回执等待。
 */
static esp_err_t gateway_execute_command(gateway_link_handle_t handle, gw_command_op_t op,
                                         uint32_t value, uint32_t request_id, uint32_t timeout_ms,
                                         uint8_t point_id, gw_result_t *out_result)
{
    if (handle == NULL || out_result == NULL || timeout_ms == 0U || op < GW_CMD_SET_ENABLE ||
        op > GW_CMD_QUERY_EVENTS || !handle->started)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(handle->command_mutex, gateway_link_ms_to_ticks(timeout_ms)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    while (xSemaphoreTake(handle->command_done, 0U) == pdTRUE)
    {
    }
    gw_command_t command = {
        .op = op, .value = value, .request_id = request_id, .point_id = point_id};
    gateway_link_snapshot_t snapshot;
    if (gateway_link_get_snapshot(handle, &snapshot) != ESP_OK || !snapshot.field_online)
    {
        xSemaphoreGive(handle->command_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    gw_frame_t frame = {.seq = ++handle->next_sequence, .session_id = snapshot.boot_id};
    uint8_t bytes[GW_FRAME_MAX_SIZE];
    if (!gw_command_pack(&command, &frame))
    {
        xSemaphoreGive(handle->command_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    const size_t length = gw_frame_encode(&frame, bytes, sizeof(bytes));
    if (length == 0U)
    {
        xSemaphoreGive(handle->command_mutex);
        return ESP_FAIL;
    }
    if (xSemaphoreTake(handle->pending_mutex, portMAX_DELAY) != pdTRUE)
    {
        xSemaphoreGive(handle->command_mutex);
        return ESP_FAIL;
    }
    handle->pending_sequence = frame.seq;
    handle->pending_session = frame.session_id;
    handle->pending_op = op;
    handle->pending_request_id = request_id;
    handle->pending_command = true;
    xSemaphoreGive(handle->pending_mutex);
    esp_err_t result = ESP_OK;
    if (xSemaphoreTake(handle->tx_mutex, gateway_link_ms_to_ticks(timeout_ms)) != pdTRUE)
    {
        result = ESP_ERR_TIMEOUT;
    }
    else
    {
        const int written = nursery_uart_write(handle->config.uart, bytes, length);
        xSemaphoreGive(handle->tx_mutex);
        if (written != (int)length)
        {
            result = ESP_FAIL;
        }
    }
    if (result == ESP_OK &&
        xSemaphoreTake(handle->command_done, gateway_link_ms_to_ticks(timeout_ms)) == pdTRUE)
    {
        *out_result = handle->pending_result;
        result = out_result->result == GW_RESULT_OK ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }
    else if (result == ESP_OK)
    {
        result = ESP_ERR_TIMEOUT;
    }
    if (xSemaphoreTake(handle->pending_mutex, portMAX_DELAY) == pdTRUE)
    {
        handle->pending_command = false;
        xSemaphoreGive(handle->pending_mutex);
    }
    xSemaphoreGive(handle->command_mutex);
    return result;
}

/**
 * @brief 计算样本原始年龄与网关接收后时间之和。
 * @param[in] point 现场测点快照，包含原始年龄和网关接收时刻。
 * @param[in] now 当前 ESP32 单调运行微秒数，不是 UTC。
 * @return 样本年龄，单位 ms；未收样本或年龄未知时返回 UINT32_MAX。
 */
uint32_t gateway_point_age_ms(const gateway_point_snapshot_t *point, int64_t now)
{
    if (!point->received || point->sample_age_ms == UINT32_MAX || now < point->received_at_us)
    {
        return UINT32_MAX;
    }
    uint64_t age = (uint64_t)point->sample_age_ms + (uint64_t)(now - point->received_at_us) / 1000;
    return age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
}

/**
 * @brief 校验并投递命令，在有限请求窗口内识别重复请求。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[in] operation 现场命令操作码，使用 GW_CMD_* 定义。
 * @param[in] value 操作参数；启停为 0/1、周期为 ms，阈值使用测点定点单位，事件查询使用已确认事件号。
 * @param[in] point_id 测点编号 1～9；不关联测点的命令使用 0。
 * @param[in] request_id 业务请求号，同一请求号的操作与参数必须保持一致。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 * @note 只受理请求，不等待现场执行；相同请求号参数必须一致，请求槽为有限窗口。
 */
esp_err_t gateway_link_submit(gateway_link_handle_t handle, gw_command_op_t operation,
                              uint32_t value, uint8_t point_id, uint32_t request_id)
{
    if (!handle || !handle->started || !request_id || operation < GW_CMD_SET_ENABLE ||
        operation > GW_CMD_QUERY_EVENTS)
    {
        return ESP_ERR_INVALID_ARG;
    }
    gateway_link_snapshot_t snapshot;
    if (gateway_link_get_snapshot(handle, &snapshot) != ESP_OK || !snapshot.field_online)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(handle->requests_mutex, portMAX_DELAY);
    for (unsigned slot_index = 0; slot_index < 8; slot_index++)
    {
        if (handle->used[slot_index] && handle->slots[slot_index].command.request_id == request_id)
        {
            gw_command_t *cached_command = &handle->slots[slot_index].command;
            esp_err_t error =
                cached_command->op == operation && cached_command->value == value &&
                        cached_command->point_id == point_id &&
                        handle->slots[slot_index].session_generation == snapshot.session_generation
                    ? ESP_OK
                    : ESP_ERR_INVALID_ARG;
            xSemaphoreGive(handle->requests_mutex);
            return error;
        }
    }
    unsigned selected_slot = 8;
    for (unsigned offset = 0; offset < 8; offset++)
    {
        unsigned slot_index = (handle->next_slot + offset) % 8;
        if (!handle->used[slot_index] || handle->slots[slot_index].state != GW_COMMAND_PENDING)
        {
            selected_slot = slot_index;
            break;
        }
    }
    if (selected_slot == 8)
    {
        xSemaphoreGive(handle->requests_mutex);
        return ESP_ERR_NO_MEM;
    }
    handle->used[selected_slot] = true;
    handle->slots[selected_slot] =
        (gateway_command_status_t){.command = {.op = operation,
                                               .value = value,
                                               .point_id = point_id,
                                               .request_id = request_id},
                                   .state = GW_COMMAND_PENDING,
                                   .session_generation = snapshot.session_generation};
    handle->next_slot = (selected_slot + 1) % 8;
    esp_err_t error =
        xQueueSend(handle->requests, &selected_slot, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
    if (error != ESP_OK)
    {
        handle->used[selected_slot] = false;
    }
    xSemaphoreGive(handle->requests_mutex);
    return error;
}

/**
 * @brief 按请求号复制命令槽的当前结果。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[in] request_id 业务请求号，同一请求号的操作与参数必须保持一致。
 * @param[out] out_status 接收复制出的完整状态或配置。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t gateway_link_command_status(gateway_link_handle_t handle, uint32_t request_id,
                                      gateway_command_status_t *out_status)
{
    if (!handle || !out_status)
    {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(handle->requests_mutex, portMAX_DELAY);
    for (unsigned slot_index = 0; slot_index < 8; slot_index++)
    {
        if (handle->used[slot_index] && handle->slots[slot_index].command.request_id == request_id)
        {
            *out_status = handle->slots[slot_index];
            xSemaphoreGive(handle->requests_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(handle->requests_mutex);
    return ESP_ERR_NOT_FOUND;
}

/**
 * @brief 提交含测点的命令，并等待其最终状态或调用方超时。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[in] operation 现场命令操作码，使用 GW_CMD_* 定义。
 * @param[in] value 操作参数；启停为 0/1、周期为 ms，阈值使用测点定点单位，事件查询使用已确认事件号。
 * @param[in] point_id 测点编号 1～9；不关联测点的命令使用 0。
 * @param[in] request_id 业务请求号，同一请求号的操作与参数必须保持一致。
 * @param[in] timeout_ms 时间长度，单位 ms；等待接口可能阻塞到该期限。
 * @param[out] out_result 接收已匹配的现场回执；超时或未匹配时不可当作执行确认。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 * @note 请求受理后由命令任务拥有；等待超时后应查询实际状态，避免盲目重发写命令。
 */
esp_err_t gateway_link_command_point(gateway_link_handle_t handle, gw_command_op_t operation,
                                     uint32_t value, uint8_t point_id, uint32_t request_id,
                                     uint32_t timeout_ms, gw_result_t *out_result)
{
    if (!out_result || !timeout_ms)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = gateway_link_submit(handle, operation, value, point_id, request_id);
    if (error != ESP_OK)
    {
        return error;
    }
    TickType_t start = xTaskGetTickCount();
    do
    {
        gateway_command_status_t status;
        error = gateway_link_command_status(handle, request_id, &status);
        if (error != ESP_OK)
        {
            return error;
        }
        if (status.state != GW_COMMAND_PENDING)
        {
            *out_result = status.result;
            return status.error;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    } while (xTaskGetTickCount() - start < pdMS_TO_TICKS(timeout_ms));
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief 提交不含测点的现场命令并等待结果。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[in] operation 现场命令操作码，使用 GW_CMD_* 定义。
 * @param[in] value 操作参数；启停为 0/1、周期为 ms，阈值使用测点定点单位，事件查询使用已确认事件号。
 * @param[in] request_id 业务请求号，同一请求号的操作与参数必须保持一致。
 * @param[in] timeout_ms 时间长度，单位 ms；等待接口可能阻塞到该期限。
 * @param[out] out_result 接收已匹配的现场回执；超时或未匹配时不可当作执行确认。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 * @note 仅可在允许阻塞的任务调用；调用方超时不等同于现场未执行。
 */
esp_err_t gateway_link_command(gateway_link_handle_t handle, gw_command_op_t operation,
                               uint32_t value, uint32_t request_id, uint32_t timeout_ms,
                               gw_result_t *out_result)
{
    return gateway_link_command_point(handle, operation, value, 0, request_id, timeout_ms,
                                      out_result);
}

/**
 * @brief 非阻塞取出一条已接收的现场事件。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_event 接收函数产生的结果；仅在成功条件成立时使用。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t gateway_link_take_event(gateway_link_handle_t handle, gateway_event_t *out_event)
{
    if (!handle || !out_event)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return xQueueReceive(handle->events, out_event, 0) == pdTRUE ? ESP_OK : ESP_ERR_NOT_FOUND;
}
