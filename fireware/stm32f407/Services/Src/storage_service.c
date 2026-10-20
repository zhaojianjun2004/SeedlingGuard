/**
 ******************************************************************************
 * @file    storage_service.c
 * @brief   W25Q 配置与事件服务实现。
 * @details StorageTask 独占 Flash；配置使用 A/B 副本，日志先正文校验再写提交标记。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：业务服务层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "field_internal.h"
#include "w25q_flash.h"
#include <stddef.h>
#include <string.h>

/*--------------------------- 模块配置、类型与状态 ---------------------------*/

#define STORE_MAGIC 0x4e555253UL
#define COMMITTED   0x434f4d54UL
#define LOG_BEGIN   8192UL
#define LOG_END     (LOG_BEGIN + 256UL * 1024UL)

typedef struct
{
    uint32_t magic, sequence, boot;
    nursery_config_t config;
    uint16_t crc;
    uint16_t reserved;
    uint32_t commit;
} config_record_t;

typedef struct
{
    uint32_t magic;
    field_log_t event;
    uint8_t padding[32];
    uint16_t crc, reserved;
    uint32_t commit;
} log_record_t;

_Static_assert(sizeof(log_record_t) == 64, "log slot must be 64 bytes");

typedef struct
{
    bool replay;
    uint32_t cursor;
    bool configuration;
    nursery_config_t config;
    field_log_t event;
} storage_job_t;

static QueueHandle_t s_storage_jobs;
static bool s_storage_ready;
static uint32_t s_config_sequence, s_boot_count, s_active_config_address,
    s_next_log_address = LOG_BEGIN, s_latest_log_id;
static uint32_t s_log_ids[(LOG_END - LOG_BEGIN) / 64];
static bool s_replay_active;
static uint32_t s_replay_cursor, s_last_event_sent_ms;

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 检查配置记录的魔数、提交标记、业务约束和 CRC。
 */
static bool storage_config_record_is_valid(const config_record_t *record)
{
    return record->magic == STORE_MAGIC && record->commit == COMMITTED &&
           nursery_config_valid(&record->config) &&
           record->crc == gw_crc16((const uint8_t *)record, offsetof(config_record_t, crc));
}

/**
 * @brief 写入并校验非活动配置槽，最后提交后切换活动索引。
 * @note 配置正文与 CRC 校验通过后，最后写提交标记；两个副本位于独立擦除扇区。
 */
static bool storage_commit_config(const nursery_config_t *config)
{
    /* 准备完整候选记录，记录序号与启动计数一起受 CRC 保护。 */
    config_record_t record;
    memset(&record, 0xff, sizeof(record));
    record.magic = STORE_MAGIC;
    record.sequence = s_config_sequence + 1;
    record.boot = s_boot_count;
    record.config = *config;
    record.crc = gw_crc16((const uint8_t *)&record, offsetof(config_record_t, crc));
    /* 选择非活动擦除扇区，避免擦除当前有效配置。 */
    uint32_t target_address = s_active_config_address ^ 4096UL;
    if (!w25q_erase(target_address) ||
        !w25q_program(target_address, &record, offsetof(config_record_t, commit)))
    {
        return false;
    }
    /* 先回读校验正文，确认写入正确后再提交。 */
    config_record_t readback_record;
    if (!w25q_read(target_address, &readback_record, sizeof(readback_record)) ||
        memcmp(&record, &readback_record, offsetof(config_record_t, commit)))
    {
        return false;
    }
    /* 提交标记最后写入；掉电时只接受完整提交的记录。 */
    uint32_t commit_marker = COMMITTED;
    if (!w25q_program(target_address + offsetof(config_record_t, commit), &commit_marker, 4) ||
        !w25q_read(target_address, &readback_record, sizeof(readback_record)) ||
        !storage_config_record_is_valid(&readback_record))
    {
        return false;
    }
    /* 只有持久提交成功，才更新 RAM 中的活动副本索引。 */
    s_config_sequence = record.sequence;
    s_active_config_address = target_address;
    return true;
}

/**
 * @brief 识别 Flash，恢复最新配置与日志位置，并记录本次启动。
 * @param[in,out] config 输入安全默认配置，成功恢复时回填持久配置。
 * @param[out] out_session_id 接收函数产生的结果；仅在成功条件成立时使用。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 * @note 仅存储任务启动阶段调用；恢复配置对象和本次启动标识后才发布就绪。
 */
bool field_storage_init(nursery_config_t *config, uint16_t *out_session_id)
{
    s_storage_ready = false;
    s_config_sequence = s_boot_count = s_active_config_address = s_latest_log_id = 0;
    s_next_log_address = LOG_BEGIN;
    s_replay_cursor = s_last_event_sent_ms = 0;
    s_replay_active = false;
    memset(s_log_ids, 0, sizeof(s_log_ids));
    s_storage_jobs = xQueueCreate(16, sizeof(storage_job_t));
    if (!s_storage_jobs || !w25q_init())
    {
        return false;
    }
    /* 读取两个独立副本，按校验结果和序号恢复最新有效配置。 */
    config_record_t config_slot_a, config_slot_b;
    if (!w25q_read(0, &config_slot_a, sizeof(config_slot_a)) ||
        !w25q_read(4096, &config_slot_b, sizeof(config_slot_b)))
    {
        return false;
    }
    bool slot_a_valid = storage_config_record_is_valid(&config_slot_a),
         slot_b_valid = storage_config_record_is_valid(&config_slot_b);
    config_record_t *latest_config =
        slot_a_valid &&
                (!slot_b_valid || (int32_t)(config_slot_a.sequence - config_slot_b.sequence) > 0)
            ? &config_slot_a
        : slot_b_valid ? &config_slot_b
                       : NULL;
    if (latest_config)
    {
        *config = latest_config->config;
        s_config_sequence = latest_config->sequence;
        s_boot_count = latest_config->boot;
        s_active_config_address = latest_config == &config_slot_a ? 0 : 4096;
    }
    ++s_boot_count;
    s_storage_ready = true;
    if (!storage_commit_config(config))
    {
        s_storage_ready = false;
        return false;
    }
    *out_session_id = (uint16_t)s_boot_count;
    if (!*out_session_id)
    {
        *out_session_id = 1;
    }
    /* 扫描定长日志槽，忽略未提交或 CRC 无效的尾记录。 */
    for (uint32_t address = LOG_BEGIN; address < LOG_END; address += 64)
    {
        if ((address & 4095) == 0)
        {
            field_health(HEALTH_STORAGE, 5000);
        }
        log_record_t log_record;
        if (!w25q_read(address, &log_record, sizeof(log_record)))
        {
            s_storage_ready = false;
            return false;
        }
        if (log_record.magic == STORE_MAGIC && log_record.commit == COMMITTED &&
            log_record.crc == gw_crc16((const uint8_t *)&log_record, offsetof(log_record_t, crc)))
        {
            s_log_ids[(address - LOG_BEGIN) / 64] = log_record.event.id;
            if (log_record.event.id > s_latest_log_id)
            {
                s_latest_log_id = log_record.event.id;
                s_next_log_address = address + 64;
            }
        }
    }
    if (s_next_log_address >= LOG_END)
    {
        s_next_log_address = LOG_BEGIN;
    }
    /* Skip a partly programmed tail after power loss; never overwrite NOR bits. */
    log_record_t tail;
    if (w25q_read(s_next_log_address, &tail, sizeof(tail)))
    {
        const uint8_t *tail_bytes = (const uint8_t *)&tail;
        bool tail_erased = true;
        for (unsigned byte_index = 0; byte_index < sizeof(tail); byte_index++)
        {
            if (tail_bytes[byte_index] != 0xff)
            {
                tail_erased = false;
            }
        }
        if (!tail_erased)
        {
            s_next_log_address = (s_next_log_address + 4095) & ~4095UL;
            if (s_next_log_address >= LOG_END)
            {
                s_next_log_address = LOG_BEGIN;
            }
        }
    }
    g_field.event_id = s_latest_log_id;
    return true;
}

/**
 * @brief 非阻塞提交候选配置的持久化请求。
 * @param[in] config 模块配置及其取值约束。
 * @return true 表示请求已入队；false 表示存储不可用或队列满，不表示配置已落盘。
 */
bool field_storage_config(const nursery_config_t *config)
{
    storage_job_t job = {.configuration = true, .config = *config};
    return s_storage_ready && xQueueSend(s_storage_jobs, &job, 0) == pdTRUE;
}

/**
 * @brief 非阻塞提交一条事件记录。
 * @param[in] event 本次处理的消息、记录或事件对象。
 * @return true 表示请求已入队；false 表示存储不可用或队列满。
 */
bool field_storage_log(const field_log_t *event)
{
    storage_job_t job = {.event = *event};
    return s_storage_ready && xQueueSend(s_storage_jobs, &job, 0) == pdTRUE;
}

/**
 * @brief 将已提交日志转换为可追补的现场事件消息。
 */
static bool storage_publish_event(const field_log_t *event)
{
    gw_frame_t frame = {.type = GW_MSG_EVENT, .length = 18};
    for (unsigned byte_index = 0; byte_index < 4; byte_index++)
    {
        frame.payload[byte_index] = (uint8_t)(event->id >> (8 * byte_index));
        frame.payload[4 + byte_index] = (uint8_t)(event->uptime >> (8 * byte_index));
        frame.payload[8 + byte_index] = (uint8_t)((uint32_t)event->value >> (8 * byte_index));
    }
    frame.payload[12] = event->point_id;
    frame.payload[13] = event->state;
    frame.payload[14] = event->kind;
    frame.payload[15] = event->quality;
    frame.payload[16] = (uint8_t)event->boot_id;
    frame.payload[17] = (uint8_t)(event->boot_id >> 8);
    return field_send(&frame, FIELD_UART);
}

/**
 * @brief 提交确认位置并请求继续发送遗漏事件。
 * @param[in] after_event_id 接收方已持久确认的事件位置。
 * @return true 表示请求已入队；false 表示存储不可用或队列满。
 */
bool field_storage_replay(uint32_t after_event_id)
{
    storage_job_t job = {.replay = true, .cursor = after_event_id};
    return s_storage_ready && xQueueSend(s_storage_jobs, &job, 0) == pdTRUE;
}

/**
 * @brief 发送最老未确认日志，等待上层持久确认后才推进。
 * @note UART 入队不推进持久确认位置，等待 ESP32 已保存事件后的应用确认。
 */
static void storage_replay_step(void)
{
    if (!s_replay_active || field_now() - s_last_event_sent_ms < 500)
    {
        return;
    }
    unsigned next_slot = (LOG_END - LOG_BEGIN) / 64;
    uint32_t next_event_id = UINT32_MAX;
    for (unsigned slot_index = 0; slot_index < (LOG_END - LOG_BEGIN) / 64; slot_index++)
    {
        if (s_log_ids[slot_index] > s_replay_cursor && s_log_ids[slot_index] < next_event_id)
        {
            next_slot = slot_index;
            next_event_id = s_log_ids[slot_index];
        }
    }
    if (next_slot == (LOG_END - LOG_BEGIN) / 64)
    {
        s_replay_active = false;
        return;
    }
    log_record_t record;
    if (!w25q_read(LOG_BEGIN + next_slot * 64, &record, sizeof(record)) ||
        record.magic != STORE_MAGIC || record.commit != COMMITTED ||
        record.crc != gw_crc16((const uint8_t *)&record, offsetof(log_record_t, crc)))
    {
        s_log_ids[next_slot] = 0;
        return;
    }
    if (storage_publish_event(&record.event))
    {
        s_last_event_sent_ms = field_now();
    }
}

/**
 * @brief 执行一条配置/日志请求或推进一次事件追补。
 * @note 仅由存储拥有者任务调用，不从采集、链路或中断直接执行。
 */
void field_storage_process(void)
{
    storage_job_t job;
    field_health(HEALTH_STORAGE, 12000);
    if (!s_storage_jobs)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
        return;
    }
    if (xQueueReceive(s_storage_jobs, &job, pdMS_TO_TICKS(s_replay_active ? 20 : 100)) != pdTRUE)
    {
        storage_replay_step();
        return;
    }
    if (job.replay)
    {
        s_replay_cursor = job.cursor;
        s_replay_active = true;
        return;
    }
    if (job.configuration)
    {
        storage_completion_t done = {.config = job.config};
        done.ok = s_storage_ready && storage_commit_config(&job.config);
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        g_field.storage_ready = done.ok;
        xSemaphoreGive(g_state_mutex);
        xQueueOverwrite(g_storage_done, &done);
    }
    else
    {
        log_record_t record;
        memset(&record, 0xff, sizeof(record));
        record.magic = STORE_MAGIC;
        record.event = job.event;
        record.crc = gw_crc16((const uint8_t *)&record, offsetof(log_record_t, crc));
        uint32_t commit_marker = COMMITTED;
        bool write_ok = s_storage_ready;
        if ((s_next_log_address & 4095) == 0)
        {
            write_ok = write_ok && w25q_erase(s_next_log_address);
            if (write_ok)
            {
                memset(&s_log_ids[(s_next_log_address - LOG_BEGIN) / 64], 0, 64 * sizeof(uint32_t));
            }
        }
        write_ok =
            write_ok && w25q_program(s_next_log_address, &record, offsetof(log_record_t, commit));
        log_record_t readback_record;
        write_ok = write_ok &&
                   w25q_read(s_next_log_address, &readback_record, sizeof(readback_record)) &&
                   !memcmp(&record, &readback_record, offsetof(log_record_t, commit));
        write_ok = write_ok && w25q_program(s_next_log_address + offsetof(log_record_t, commit),
                                            &commit_marker, 4);
        write_ok = write_ok &&
                   w25q_read(s_next_log_address, &readback_record, sizeof(readback_record)) &&
                   readback_record.commit == COMMITTED &&
                   readback_record.crc ==
                       gw_crc16((const uint8_t *)&readback_record, offsetof(log_record_t, crc));
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        g_field.storage_ready = write_ok;
        xSemaphoreGive(g_state_mutex);
        if (write_ok)
        {
            s_log_ids[(s_next_log_address - LOG_BEGIN) / 64] = job.event.id;
            s_replay_active = true; /* Send only the oldest unacknowledged log via replay_step. */
            s_next_log_address += 64;
            if (s_next_log_address >= LOG_END)
            {
                s_next_log_address = LOG_BEGIN;
            }
        }
        else
        {
            taskENTER_CRITICAL();
            ++g_field.log_drops;
            taskEXIT_CRITICAL();
            s_next_log_address = (s_next_log_address + 4096) & ~4095UL;
            if (s_next_log_address >= LOG_END)
            {
                s_next_log_address = LOG_BEGIN;
            }
        }
    }
}

/**
 * @brief 加载现场持久状态，发布启动就绪后独占存储循环。
 * @note 任务入口，只在任务上下文运行；正常情况下不返回。
 */
void storage_service_run(void)
{
    nursery_config_t config;
    nursery_config_defaults(&config);
    uint16_t session_id = (uint16_t)(field_now() + 1);
    field_health(HEALTH_STORAGE, 10000);
    bool storage_ready = field_storage_init(&config, &session_id);
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_field.config = config;
    g_field.session = session_id;
    g_field.storage_ready = storage_ready;
    xSemaphoreGive(g_state_mutex);
    /* Configuration is published once before the other application tasks run. */
    extern volatile bool g_startup_ready;
    g_startup_ready = true;
    for (;;)
    {
        field_storage_process();
    }
}
