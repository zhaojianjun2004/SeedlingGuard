/**
 ******************************************************************************
 * @file    field_internal.h
 * @brief   现场服务内部协作契约接口定义。
 * @details 仅服务层可见；队列、快照、配置事务和健康期限不向应用或驱动公开。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：业务服务层
 ******************************************************************************
 */

#pragma once
/*--------------------------- 头文件 ---------------------------*/
#include "field_services.h"
#include "gateway_protocol.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"
#define FIELD_UART 1U
#define FIELD_CAN  2U

typedef struct
{
    int32_t value;
    uint32_t sampled_ms, failures;
    uint8_t quality;
    bool has_value;
    nursery_alarm_t alarm;
} field_point_t;

typedef struct
{
    nursery_config_t config;
    uint16_t session;
    field_point_t points[NURSERY_POINT_COUNT];
    uint8_t devices[NURSERY_DEVICE_COUNT];
    uint32_t event_id, sample_drops, log_drops, log_pending;
    bool storage_ready;
} field_snapshot_t;

typedef struct
{
    uint8_t device, count, quality, device_state;
    int32_t values[2];
    uint32_t sampled_ms, config_version;
} sample_event_t;

typedef struct
{
    gw_command_t command;
    uint16_t seq, session;
    uint8_t source;
} command_event_t;

typedef struct
{
    gw_frame_t frame;
    uint8_t destination;
} link_output_t;

typedef struct
{
    bool ok;
    nursery_config_t config;
} storage_completion_t;

typedef struct
{
    uint8_t point_id, state, kind, quality;
    uint32_t id, uptime;
    int32_t value;
    uint16_t boot_id, reserved;
} field_log_t;

typedef enum
{
    HEALTH_LINK,
    HEALTH_FIELD,
    HEALTH_MODBUS,
    HEALTH_STORAGE,
    HEALTH_COUNT
} health_id_t;

extern QueueHandle_t g_samples, g_commands, g_output, g_storage_done;
extern TaskHandle_t g_modbus_task;
extern SemaphoreHandle_t g_state_mutex;
extern field_snapshot_t g_field;

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 取得当前 RTOS 单调运行毫秒数。
 * @return 单调运行时间，单位 ms；不表示 UTC 时间。
 */
uint32_t field_now(void);

/**
 * @brief 在短时间互斥保护下复制现场共享状态。
 * @param[out] out 接收受互斥保护的现场状态副本。
 * @return true 表示复制成功；false 表示未能在 20ms 内取得状态锁。
 */
bool field_snapshot(field_snapshot_t *out);

/**
 * @brief 报告一个任务当前阶段允许的最晚进展时间。
 * @param[in] id 需要报告进展的任务编号，取 HEALTH_* 枚举。
 * @param[in] allowance 当前阶段允许的剩余时间，单位 ms。
 */
void field_health(health_id_t id, uint32_t allowance);

/**
 * @brief 检查全部必需任务是否已经报告且未超出阶段期限。
 * @return true 表示满足本函数检查条件；false 表示未满足。
 */
bool field_health_ok(void);

/**
 * @brief 给帧绑定当前会话并非阻塞投递发送队列。
 * @param[out] frame 接收完整帧，仅在函数返回 true 时有效。
 * @param[in] destination 目的链路位图，由 FIELD_UART/FIELD_CAN 组合。
 * @return true 表示进入发送队列；false 表示当前队列满，不表示对端已经接收。
 */
bool field_send(gw_frame_t *frame, uint8_t destination);

/**
 * @brief 从同一版本快照生成设备、样本与规则消息。
 */
void field_publish_points(void);

/**
 * @brief 识别 Flash，恢复最新配置与日志位置，并记录本次启动。
 * @param[in,out] config 输入安全默认配置，成功恢复时回填持久配置。
 * @param[out] out_session_id 接收函数产生的结果；仅在成功条件成立时使用。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 * @note 仅存储任务启动阶段调用；恢复配置对象和本次启动标识后才发布就绪。
 */
bool field_storage_init(nursery_config_t *config, uint16_t *out_session_id);

/**
 * @brief 非阻塞提交候选配置的持久化请求。
 * @param[in] config 模块配置及其取值约束。
 * @return true 表示请求已入队；false 表示存储不可用或队列满，不表示配置已落盘。
 */
bool field_storage_config(const nursery_config_t *config);

/**
 * @brief 非阻塞提交一条事件记录。
 * @param[in] event 本次处理的消息、记录或事件对象。
 * @return true 表示请求已入队；false 表示存储不可用或队列满。
 */
bool field_storage_log(const field_log_t *event);

/**
 * @brief 执行一条配置/日志请求或推进一次事件追补。
 * @note 仅由存储拥有者任务调用，不从采集、链路或中断直接执行。
 */
void field_storage_process(void);

/**
 * @brief 提交确认位置并请求继续发送遗漏事件。
 * @param[in] after_event_id 接收方已持久确认的事件位置。
 * @return true 表示请求已入队；false 表示存储不可用或队列满。
 */
bool field_storage_replay(uint32_t after_event_id);

/**
 * @brief 复制指定设备的每次尝试分类计数。
 * @param[in] device_index 从 0 开始的仪表下标，范围 0～5。
 * @param[out] out_counters 容量至少为 6，依次接收成功、超时、CRC、异常、协议和发送错误计数。
 */
void field_modbus_counters(unsigned device_index, uint32_t out_counters[6]);

/**
 * @brief 读取 CAN 端口错误与接收溢出计数。
 * @param[out] out 容量至少为 2，依次接收 CAN 错误数和接收队列溢出数。
 */
void field_can_counters(uint32_t out[2]);
