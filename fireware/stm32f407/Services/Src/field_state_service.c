/**
 ******************************************************************************
 * @file    field_state_service.c
 * @brief   现场状态与配置事务实现。
 * @details FieldTask 统一修改测点和配置；质量有效后再判断告警，存储完成后再确认配置。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：业务服务层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "field_internal.h"
#include <string.h>

/*--------------------------- 模块配置、类型与状态 ---------------------------*/

QueueHandle_t g_samples, g_commands, g_output, g_storage_done;
TaskHandle_t g_modbus_task;
SemaphoreHandle_t g_state_mutex;
field_snapshot_t g_field;
volatile bool g_startup_ready;
static volatile uint32_t s_health_deadlines[HEALTH_COUNT];
static volatile bool s_health_reported[HEALTH_COUNT];

static struct
{
    bool valid;
    command_event_t request;
    uint8_t result;
} s_command_cache[8];

static unsigned s_command_cache_next;
static field_log_t s_pending_logs[32];
static unsigned s_log_head, s_log_count;
static bool s_config_transaction_active;
static command_event_t s_pending_command;
static uint32_t s_config_transaction_started_ms;

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 取得当前 RTOS 单调运行毫秒数。
 * @return 单调运行时间，单位 ms；不表示 UTC 时间。
 */
uint32_t field_now(void)
{
    return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

/**
 * @brief 报告一个任务当前阶段允许的最晚进展时间。
 * @param[in] id 需要报告进展的任务编号，取 HEALTH_* 枚举。
 * @param[in] allowance 当前阶段允许的剩余时间，单位 ms。
 */
void field_health(health_id_t id, uint32_t allowance)
{
    s_health_deadlines[id] = field_now() + allowance;
    s_health_reported[id] = true;
}

/**
 * @brief 检查全部必需任务是否已经报告且未超出阶段期限。
 * @return true 表示满足本函数检查条件；false 表示未满足。
 */
bool field_health_ok(void)
{
    uint32_t now = field_now();
    for (unsigned i = 0; i < HEALTH_COUNT; i++)
    {
        if (!s_health_reported[i] || (int32_t)(now - s_health_deadlines[i]) > 0)
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 在短时间互斥保护下复制现场共享状态。
 * @param[out] out 接收受互斥保护的现场状态副本。
 * @return true 表示复制成功；false 表示未能在 20ms 内取得状态锁。
 */
bool field_snapshot(field_snapshot_t *out)
{
    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
    {
        return false;
    }
    *out = g_field;
    xSemaphoreGive(g_state_mutex);
    return true;
}

/**
 * @brief 给帧绑定当前会话并非阻塞投递发送队列。
 * @param[out] frame 接收完整帧，仅在函数返回 true 时有效。
 * @param[in] destination 目的链路位图，由 FIELD_UART/FIELD_CAN 组合。
 * @return true 表示进入发送队列；false 表示当前队列满，不表示对端已经接收。
 */
bool field_send(gw_frame_t *frame, uint8_t destination)
{
    frame->session_id = g_field.session;
    link_output_t output = {.frame = *frame, .destination = destination};
    return xQueueSend(g_output, &output, 0) == pdTRUE;
}

/**
 * @brief 建立现场队列、状态互斥量和默认配置。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool field_services_init(void)
{
    g_state_mutex = xSemaphoreCreateMutex();
    g_samples = xQueueCreate(16, sizeof(sample_event_t));
    g_commands = xQueueCreate(8, sizeof(command_event_t));
    g_output = xQueueCreate(32, sizeof(link_output_t));
    g_storage_done = xQueueCreate(1, sizeof(storage_completion_t));
    nursery_config_defaults(&g_field.config);
    return g_state_mutex && g_samples && g_commands && g_output && g_storage_done;
}

/**
 * @brief 生成含实际配置的命令执行回执。
 */
static void field_reply_command(const command_event_t *request, uint8_t result_code)
{
    gw_result_t result = {.op = request->command.op,
                          .result = result_code,
                          .request_id = request->command.request_id,
                          .collect_enabled = g_field.config.enabled,
                          .sample_period_ms = g_field.config.period_ms,
                          .config_version = g_field.config.version};
    gw_frame_t frame = {.seq = request->seq};
    gw_result_pack(&result, &frame);
    (void)field_send(&frame, request->source);
}

/**
 * @brief 缓存近期请求号与结果以抑制重复执行。
 */
static void field_cache_command_result(const command_event_t *request, uint8_t result)
{
    s_command_cache[s_command_cache_next].valid = true;
    s_command_cache[s_command_cache_next].request = *request;
    s_command_cache[s_command_cache_next].result = result;
    s_command_cache_next = (s_command_cache_next + 1) % 8;
}

/**
 * @brief 生成状态变化事件，存储队列暂满时保留到有界待补记缓冲。
 */
static void field_record_transition(unsigned point_index, uint8_t event_kind)
{
    field_point_t *point = &g_field.points[point_index];
    field_log_t event = {
        .point_id = (uint8_t)(point_index + 1),
        .state = event_kind == 3
                     ? g_field.devices[(point_index / 3) * 2 + (point_index % 3 == 2 ? 1 : 0)]
                     : point->alarm.state,
        .kind = event_kind,
        .quality = point->quality,
        .id = ++g_field.event_id,
        .uptime = field_now(),
        .value = point->value,
        .boot_id = g_field.session};
    if (s_log_count || !field_storage_log(&event))
    {
        if (s_log_count < 32)
        {
            s_pending_logs[(s_log_head + s_log_count) % 32] = event;
            ++s_log_count;
        }
        else
        {
            ++g_field.log_drops;
        }
        g_field.log_pending = s_log_count;
    }
}

/**
 * @brief 校验样本版本，更新质量、设备状态与告警，并记录变化。
 */
static void field_apply_sample(const sample_event_t *sample_event)
{
    /* Results from an old configuration must not overwrite a newly enabled acquisition. */
    /* 旧配置版本的采样结果不能覆盖新配置下的现场状态。 */
    if (!g_field.config.enabled || sample_event->config_version != g_field.config.version ||
        sample_event->device >= NURSERY_DEVICE_COUNT)
    {
        return;
    }
    unsigned zone_index = sample_event->device / 2,
             first_point_index = zone_index * 3 + (sample_event->device % 2 ? 2 : 0);
    uint8_t previous_device_state = g_field.devices[sample_event->device];
    g_field.devices[sample_event->device] = sample_event->device_state;
    bool device_changed = previous_device_state != sample_event->device_state &&
                          (previous_device_state == 3 || sample_event->device_state == 3);
    for (unsigned offset = 0;
         offset < sample_event->count && first_point_index + offset < NURSERY_POINT_COUNT; offset++)
    {
        unsigned point_index = first_point_index + offset;
        field_point_t *point = &g_field.points[point_index];
        uint8_t previous_quality = point->quality;
        bool sample_valid = sample_event->quality == GW_QUALITY_VALID &&
                            nursery_value_valid(point_index, sample_event->values[offset]);
        if (sample_valid)
        {
            point->value = sample_event->values[offset];
            point->sampled_ms = sample_event->sampled_ms;
            point->has_value = true;
            point->failures = 0;
            point->quality = GW_QUALITY_VALID;
        }
        else
        {
            ++point->failures;
            point->quality = sample_event->quality == GW_QUALITY_FAULT || point->failures >= 3
                                 ? GW_QUALITY_FAULT
                             : point->has_value ? GW_QUALITY_STALE
                                                : GW_QUALITY_INVALID;
        }
        /* 仅有效质量参与阈值判断；失效样本保持已确认的告警历史。 */
        bool alarm_changed = nursery_alarm_update(&point->alarm, &g_field.config.rules[point_index],
                                                  point->value, point->quality == GW_QUALITY_VALID);
        if (alarm_changed)
        {
            field_record_transition(point_index, 1);
        }
        if (previous_quality != point->quality &&
            (previous_quality == GW_QUALITY_FAULT || point->quality == GW_QUALITY_FAULT))
        {
            field_record_transition(point_index, 2);
        }
    }
    if (device_changed)
    {
        field_record_transition(first_point_index, 3);
    }
}

/**
 * @brief 校验并去重现场命令，异步提交候选配置或执行只读查询。
 */
static void field_apply_command(const command_event_t *command_event)
{
    if (command_event->source == FIELD_UART && command_event->session != g_field.session)
    {
        return;
    }
    /* 先核对近期请求缓存，同请求号不同参数明确拒绝。 */
    for (unsigned cache_index = 0; cache_index < 8; cache_index++)
    {
        if (s_command_cache[cache_index].valid &&
            s_command_cache[cache_index].request.source == command_event->source &&
            s_command_cache[cache_index].request.command.request_id ==
                command_event->command.request_id)
        {
            const gw_command_t *cached_command = &s_command_cache[cache_index].request.command;
            field_reply_command(command_event,
                                cached_command->op == command_event->command.op &&
                                        cached_command->value == command_event->command.value &&
                                        cached_command->point_id == command_event->command.point_id
                                    ? s_command_cache[cache_index].result
                                    : GW_RESULT_INVALID);
            return;
        }
    }
    if (s_config_transaction_active)
    {
        field_reply_command(command_event, GW_RESULT_BUSY);
        return;
    }
    if (command_event->command.op == GW_CMD_QUERY_STATUS)
    {
        field_reply_command(command_event, GW_RESULT_OK);
        return;
    }
    if (command_event->command.op == GW_CMD_QUERY_EVENTS)
    {
        field_reply_command(command_event, field_storage_replay(command_event->command.value)
                                               ? GW_RESULT_OK
                                               : GW_RESULT_BUSY);
        return;
    }
    /* 构造候选配置，不提前修改当前活动配置。 */
    nursery_config_t candidate_config = g_field.config;
    if (command_event->command.op == GW_CMD_SET_ENABLE && command_event->command.value <= 1)
    {
        candidate_config.enabled = (uint8_t)command_event->command.value;
    }
    else if (command_event->command.op == GW_CMD_SET_PERIOD)
    {
        candidate_config.period_ms = command_event->command.value;
    }
    else if (command_event->command.op >= GW_CMD_SET_LOW &&
             command_event->command.op <= GW_CMD_SET_HYSTERESIS &&
             command_event->command.point_id >= 1 &&
             command_event->command.point_id <= NURSERY_POINT_COUNT)
    {
        nursery_rule_t *rule = &candidate_config.rules[command_event->command.point_id - 1];
        int32_t threshold_value = (int32_t)command_event->command.value;
        if (command_event->command.op == GW_CMD_SET_LOW)
        {
            rule->low = threshold_value;
        }
        else if (command_event->command.op == GW_CMD_SET_HIGH)
        {
            rule->high = threshold_value;
        }
        else
        {
            rule->hysteresis = threshold_value;
        }
    }
    else
    {
        field_reply_command(command_event, GW_RESULT_INVALID);
        return;
    }
    /* 校验全部组合约束，再异步提交存储任务。 */
    if (!nursery_config_valid(&candidate_config))
    {
        field_reply_command(command_event, GW_RESULT_INVALID);
        return;
    }
    if (!memcmp(&candidate_config, &g_field.config, sizeof(candidate_config)))
    {
        field_cache_command_result(command_event, GW_RESULT_OK);
        field_reply_command(command_event, GW_RESULT_OK);
        return;
    }
    ++candidate_config.version;
    if (!field_storage_config(&candidate_config))
    {
        field_reply_command(command_event, GW_RESULT_ERROR);
        return;
    }
    s_pending_command = *command_event;
    s_config_transaction_started_ms = field_now();
    s_config_transaction_active = true;
}

/**
 * @brief 从同一版本快照生成设备、样本与规则消息。
 */
void field_publish_points(void)
{
    field_snapshot_t snapshot;
    if (!field_snapshot(&snapshot))
    {
        return;
    }
    for (unsigned index = 0; index < NURSERY_DEVICE_COUNT; index++)
    {
        gw_frame_t device = {.type = GW_MSG_DEVICE, .length = 6};
        device.payload[0] = (uint8_t)(index + 1);
        device.payload[1] = snapshot.devices[index];
        for (unsigned byte_index = 0; byte_index < 4; byte_index++)
        {
            device.payload[2 + byte_index] = (uint8_t)(snapshot.config.version >> (8 * byte_index));
        }
        (void)field_send(&device, FIELD_UART);
    }
    for (unsigned index = 0; index < NURSERY_POINT_COUNT; index++)
    {
        field_point_t *point = &snapshot.points[index];
        gw_sample_t sample = {.point_id = (uint8_t)(index + 1),
                              .quality = point->quality,
                              .scaled_value = point->value,
                              .sample_age_ms =
                                  point->has_value ? field_now() - point->sampled_ms : UINT32_MAX,
                              .alarm_state = point->alarm.state,
                              .config_version = snapshot.config.version};
        gw_frame_t frame = {0};
        gw_sample_pack(&sample, &frame);
        (void)field_send(&frame, FIELD_UART | FIELD_CAN);
        frame = (gw_frame_t){.type = GW_MSG_RULE, .length = 17};
        frame.payload[0] = (uint8_t)(index + 1);
        int32_t rule_values[3] = {snapshot.config.rules[index].low,
                                  snapshot.config.rules[index].high,
                                  snapshot.config.rules[index].hysteresis};
        for (unsigned rule_index = 0; rule_index < 3; rule_index++)
        {
            for (unsigned byte_index = 0; byte_index < 4; byte_index++)
            {
                frame.payload[1 + 4 * rule_index + byte_index] =
                    (uint8_t)((uint32_t)rule_values[rule_index] >> (8 * byte_index));
            }
        }
        for (unsigned byte_index = 0; byte_index < 4; byte_index++)
        {
            frame.payload[13 + byte_index] = (uint8_t)(snapshot.config.version >> (8 * byte_index));
        }
        (void)field_send(&frame, FIELD_UART);
    }
}

/**
 * @brief 有界处理命令/样本，接收配置提交结果并检查新鲜度。
 * @note 任务入口，只在任务上下文运行；正常情况下不返回。
 */
void field_state_service_run(void)
{
    while (!g_startup_ready)
    {
        field_health(HEALTH_FIELD, 10000);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    for (;;)
    {
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        command_event_t command_event;
        if (xQueueReceive(g_commands, &command_event, 0) == pdTRUE)
        {
            field_apply_command(&command_event);
        }
        sample_event_t sample_event;
        for (unsigned retry_index = 0;
             retry_index < 4 && xQueueReceive(g_samples, &sample_event, 0) == pdTRUE; retry_index++)
        {
            field_apply_sample(&sample_event);
        }
        storage_completion_t storage_result;
        if (xQueueReceive(g_storage_done, &storage_result, 0) == pdTRUE &&
            s_config_transaction_active)
        {
            if (storage_result.ok)
            {
                g_field.config = storage_result.config;
                for (unsigned point_index = 0; point_index < NURSERY_POINT_COUNT; point_index++)
                {
                    if (g_field.points[point_index].quality == GW_QUALITY_VALID)
                    {
                        g_field.points[point_index].quality = GW_QUALITY_STALE;
                        if (nursery_alarm_update(&g_field.points[point_index].alarm,
                                                 &g_field.config.rules[point_index],
                                                 g_field.points[point_index].value, false))
                        {
                            field_record_transition(point_index, 1);
                        }
                    }
                    g_field.points[point_index].alarm.enter_count =
                        g_field.points[point_index].alarm.recover_count = 0;
                }
                if (g_modbus_task)
                {
                    xTaskNotifyGive(g_modbus_task);
                }
            }
            field_cache_command_result(&s_pending_command,
                                       storage_result.ok ? GW_RESULT_OK : GW_RESULT_ERROR);
            field_reply_command(&s_pending_command,
                                storage_result.ok ? GW_RESULT_OK : GW_RESULT_ERROR);
            s_config_transaction_active = false;
        }
        /* Never discard an in-flight persistence transaction: a late commit must be reconciled. */
        if (s_config_transaction_active && field_now() - s_config_transaction_started_ms > 12000)
        {
            field_health(HEALTH_FIELD, 0);
        }
        uint32_t freshness_limit_ms = g_field.config.period_ms * 2 + 2000;
        for (unsigned point_index = 0; point_index < NURSERY_POINT_COUNT; point_index++)
        {
            field_point_t *point = &g_field.points[point_index];
            if (point->quality == GW_QUALITY_VALID &&
                (!g_field.config.enabled || field_now() - point->sampled_ms > freshness_limit_ms))
            {
                point->quality = GW_QUALITY_STALE;
                if (nursery_alarm_update(&point->alarm, &g_field.config.rules[point_index],
                                         point->value, false))
                {
                    field_record_transition(point_index, 1);
                }
            }
        }
        /* Preserve ordering while retrying a temporarily full storage queue. */
        for (unsigned retry_index = 0; retry_index < 4 && s_log_count; retry_index++)
        {
            if (!field_storage_log(&s_pending_logs[s_log_head]))
            {
                break;
            }
            s_log_head = (s_log_head + 1) % 32;
            --s_log_count;
        }
        g_field.log_pending = s_log_count;
        bool transaction_stalled =
            s_config_transaction_active && field_now() - s_config_transaction_started_ms > 12000;
        xSemaphoreGive(g_state_mutex);
        if (!transaction_stalled)
        {
            field_health(HEALTH_FIELD, 1500);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
