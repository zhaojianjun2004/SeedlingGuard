/**
 ******************************************************************************
 * @file    gateway_outbox.h
 * @brief   持久待发队列服务接口定义。
 * @details 存储任务独占运行期 NVS 操作；仅业务匹配的 PUBACK 才允许删除记录。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：业务服务层
 ******************************************************************************
 */

#pragma once

/*--------------------------- 头文件 ---------------------------*/
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define GATEWAY_OUTBOX_CAPACITY    1024U
#define GATEWAY_OUTBOX_POINT_COUNT 9U

typedef struct gateway_outbox *gateway_outbox_handle_t;

typedef struct
{
    int32_t value_scaled;
    uint32_t sample_age_ms;
    uint8_t quality;
    uint8_t alarm_state;
} gateway_outbox_point_t;

typedef struct
{
    uint32_t record_id;
    uint32_t capture_uptime_ms;
    uint32_t gateway_boot_nonce;
    uint32_t sample_period_ms;
    uint16_t field_boot_id;
    bool field_online;
    bool collect_enabled;
    gateway_outbox_point_t points[GATEWAY_OUTBOX_POINT_COUNT];
    uint32_t event_id, event_uptime;
    int32_t event_value;
    uint8_t event_point, event_state, event_kind, event_quality;
    uint8_t record_kind; /* 0=telemetry, 1=committed field event */
} gateway_outbox_record_t;

typedef struct
{
    uint32_t pending_count;
    uint32_t dropped_count;
    uint32_t next_record_id;
} gateway_outbox_status_t;

/* 专用 NVS 分区中的定长记录槽；重启时扫描记录恢复队列。 */

/* 只在对应 MQTT QoS 1 PUBACK 到达后调用。 */

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 创建持久待发队列服务实例并准备所需资源。
 * @param[in] partition 专用 NVS 分区名称，本项目使用 outbox。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t gateway_outbox_create(const char *partition, gateway_outbox_handle_t *out_handle);

/**
 * @brief 请求追加一条持久待发记录。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[in] record 本次处理的消息、记录或事件对象。
 * @param[out] out_record_id 接收成功分配的记录号；允许传入 NULL。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 * @note 任务接口，可能阻塞等待存储完成；记录 ID 仅在成功时回填。
 */
esp_err_t gateway_outbox_append(gateway_outbox_handle_t handle,
                                const gateway_outbox_record_t *record, uint32_t *out_record_id);

/**
 * @brief 请求读取最老记录，保留其待发状态。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_record 接收函数产生的结果；仅在成功条件成立时使用。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t gateway_outbox_peek_oldest(gateway_outbox_handle_t handle,
                                     gateway_outbox_record_t *out_record);

/**
 * @brief 请求删除匹配 PUBACK 的持久记录。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[in] record_id 待确认或删除的持久记录号。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 * @note 只能在对应消息 ID 与连接代数的 PUBACK 匹配后调用。
 */
esp_err_t gateway_outbox_ack(gateway_outbox_handle_t handle, uint32_t record_id);

/**
 * @brief 复制持久待发队列服务的当前状态。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_status 接收复制出的完整状态或配置。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t gateway_outbox_get_status(gateway_outbox_handle_t handle,
                                    gateway_outbox_status_t *out_status);

/**
 * @brief 读取已经持久保存的现场事件位置。
 * @param[in] handle 已创建的模块实例句柄。
 * @return 已持久保存的现场事件号；空句柄返回 0。
 */
uint32_t gateway_outbox_event_cursor(gateway_outbox_handle_t handle);

#ifdef __cplusplus
}
#endif
