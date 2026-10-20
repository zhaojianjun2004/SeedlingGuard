/**
 ******************************************************************************
 * @file    gateway_link.h
 * @brief   现场链路与命令服务接口定义。
 * @details 接收任务更新快照并匹配回执，命令任务串行发送；共享快照由互斥量保护。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：业务服务层
 ******************************************************************************
 */

#pragma once

/*--------------------------- 头文件 ---------------------------*/
#include <stdbool.h>
#include <stdint.h>

#include "uart_port.h"
#include "esp_err.h"
#include "gateway_protocol.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct gateway_link *gateway_link_handle_t;

typedef struct
{
    nursery_uart_handle_t uart;
    uint32_t task_stack_size;
    uint32_t task_priority;
} gateway_link_config_t;

typedef struct
{
    int32_t scaled_value;
    uint16_t sequence;
    uint8_t quality;
    bool received;
    uint8_t alarm_state;
    uint32_t sample_age_ms;
    int64_t received_at_us;
    nursery_rule_t rule;
} gateway_point_snapshot_t;

typedef struct
{
    uint16_t boot_id;
    uint32_t session_generation;
    uint32_t config_version;
    bool storage_ready;
    uint32_t sample_drops, log_drops, latest_event_id, log_pending;
    uint32_t last_event_id;
    uint8_t last_event_point, last_event_state, last_event_kind;
    uint32_t event_drops;
    uint16_t points_mask, rules_mask;
    uint8_t devices_mask;
    bool synchronized;
    uint32_t sample_period_ms;
    uint32_t uart_rx_bytes;
    uint32_t received_frames;
    uint32_t crc_errors;
    uint32_t framing_errors;
    int64_t last_frame_at_us;
    gateway_point_snapshot_t points[NURSERY_POINT_COUNT];
    uint8_t devices[NURSERY_DEVICE_COUNT];
    bool collect_enabled;
    bool field_online;
} gateway_link_snapshot_t;

/* 同一时刻只允许一笔等待 F407 执行结果的控制命令。 */

/* 请求槽位属于服务；HTTP/MQTT/串口只投递，不直接写 UART。 */
typedef enum
{
    GW_COMMAND_PENDING,
    GW_COMMAND_APPLIED,
    GW_COMMAND_FAILED,
    GW_COMMAND_UNCONFIRMED
} gateway_command_state_t;

typedef struct
{
    gw_command_t command;
    gw_result_t result;
    gateway_command_state_t state;
    esp_err_t error;
    uint32_t session_generation;
} gateway_command_status_t;

typedef struct
{
    uint32_t id, uptime;
    int32_t value;
    uint16_t field_boot_id;
    uint8_t point, state, kind, quality;
} gateway_event_t;

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 创建现场链路与命令服务实例并准备所需资源。
 * @param[in] config 模块配置及其取值约束。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t gateway_link_create(const gateway_link_config_t *config,
                              gateway_link_handle_t *out_handle);

/**
 * @brief 启动现场链路与命令服务的运行入口。
 * @param[in] handle 已创建的模块实例句柄。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t gateway_link_start(gateway_link_handle_t handle);

/**
 * @brief 在互斥保护下复制完整现场快照。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_snapshot 接收复制出的完整状态或配置。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 * @note 只短时间持锁复制；调用方在锁外完成格式化或网络处理。
 */
esp_err_t gateway_link_get_snapshot(gateway_link_handle_t handle,
                                    gateway_link_snapshot_t *out_snapshot);

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
                               gw_result_t *out_result);

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
                              uint32_t value, uint8_t point_id, uint32_t request_id);

/**
 * @brief 按请求号复制命令槽的当前结果。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[in] request_id 业务请求号，同一请求号的操作与参数必须保持一致。
 * @param[out] out_status 接收复制出的完整状态或配置。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t gateway_link_command_status(gateway_link_handle_t handle, uint32_t request_id,
                                      gateway_command_status_t *out_status);

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
                                     uint32_t timeout_ms, gw_result_t *out_result);

/**
 * @brief 计算样本原始年龄与网关接收后时间之和。
 * @param[in] point 现场测点快照，包含原始年龄和网关接收时刻。
 * @param[in] now 当前 ESP32 单调运行微秒数，不是 UTC。
 * @return 样本年龄，单位 ms；未收样本或年龄未知时返回 UINT32_MAX。
 */
uint32_t gateway_point_age_ms(const gateway_point_snapshot_t *point, int64_t now);

/**
 * @brief 非阻塞取出一条已接收的现场事件。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_event 接收函数产生的结果；仅在成功条件成立时使用。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t gateway_link_take_event(gateway_link_handle_t handle, gateway_event_t *out_event);

#ifdef __cplusplus
}
#endif
