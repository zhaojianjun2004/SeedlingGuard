/**
 ******************************************************************************
 * @file    gateway_protocol.h
 * @brief   UART V2 应用协议接口定义。
 * @details 纯 C 的固定字节布局、小端编解码与 CRC16；不访问 UART 或创建任务。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：协议层
 ******************************************************************************
 */

#pragma once

/*--------------------------- 头文件 ---------------------------*/
#include "nursery_model.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define GW_PROTOCOL_VERSION  2U
#define GW_FRAME_SOF0        0xA5U
#define GW_FRAME_SOF1        0x5AU
#define GW_FRAME_MAX_PAYLOAD 32U
#define GW_FRAME_OVERHEAD    11U
#define GW_FRAME_MAX_SIZE    (GW_FRAME_OVERHEAD + GW_FRAME_MAX_PAYLOAD)

typedef enum
{
    GW_MSG_STATUS = 0x01,
    GW_MSG_SAMPLE = 0x02,
    GW_MSG_RULE = 0x03,
    GW_MSG_DEVICE = 0x04,
    GW_MSG_COMMAND = 0x10,
    GW_MSG_RESULT = 0x11,
    GW_MSG_EVENT = 0x12,
} gw_message_type_t;

typedef enum
{
    GW_POINT_TEMPERATURE = 1,
    GW_POINT_HUMIDITY = 2,
    GW_POINT_LIGHT = 3,
} gw_point_id_t;

typedef enum
{
    GW_QUALITY_INVALID = 0,
    GW_QUALITY_VALID = 1,
    GW_QUALITY_STALE = 2,
    GW_QUALITY_FAULT = 3,
} gw_quality_t;

typedef enum
{
    GW_CMD_SET_ENABLE = 1,
    GW_CMD_SET_PERIOD = 2,
    GW_CMD_QUERY_STATUS = 3,
    GW_CMD_SET_LOW = 4,
    GW_CMD_SET_HIGH = 5,
    GW_CMD_SET_HYSTERESIS = 6,
    GW_CMD_QUERY_EVENTS = 7,
} gw_command_op_t;

typedef enum
{
    GW_RESULT_OK = 0,
    GW_RESULT_INVALID = 1,
    GW_RESULT_BUSY = 2,
    GW_RESULT_ERROR = 3,
} gw_result_code_t;

typedef struct
{
    uint8_t type;
    uint16_t seq;
    uint16_t session_id;
    uint8_t length;
    uint8_t payload[GW_FRAME_MAX_PAYLOAD];
} gw_frame_t;

typedef struct
{
    uint8_t bytes[GW_FRAME_MAX_SIZE];
    uint8_t used;
    uint32_t framing_errors;
    uint32_t crc_errors;
} gw_parser_t;

typedef struct
{
    uint16_t boot_id;
    bool collect_enabled;
    uint32_t sample_period_ms;
    uint32_t config_version;
    bool storage_ready;
    uint32_t sample_drops, log_drops, latest_event_id, log_pending;
} gw_status_t;

typedef struct
{
    uint8_t point_id;
    uint8_t quality;
    int32_t scaled_value;
    uint32_t sample_age_ms;
    uint8_t alarm_state;
    uint32_t config_version;
} gw_sample_t;

typedef struct
{
    uint8_t op;
    uint32_t value;
    uint32_t request_id;
    uint8_t point_id;
} gw_command_t;

typedef struct
{
    uint8_t op;
    uint8_t result;
    bool collect_enabled;
    uint32_t sample_period_ms;
    uint32_t request_id;
    uint32_t config_version;
} gw_result_t;

/* CRC-16/MODBUS: init=0xFFFF, reflected polynomial=0xA001. */

/* 线格式：A5 5A | version | type | seq_le16 | session_le16 | len | payload | crc_le16。 */

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 计算 CRC-16/MODBUS，初值 0xFFFF、多项式 0xA001。
 * @param[in] data 输入字节缓冲区。
 * @param[in] length 本次输入内容长度，单位字节。
 * @return CRC-16/MODBUS 校验值；空数据指针保持初值 0xFFFF。
 */
uint16_t gw_crc16(const uint8_t *data, size_t length);

/**
 * @brief 编码完整 UART V2 帧并追加 CRC16。
 * @param[in] frame 已组装的业务帧，包括类型、序号、会话和载荷。
 * @param[out] output 输出字节缓冲区，容量由配套参数或数组长度约束。
 * @param[in] capacity 输出缓冲区容量，单位字节。
 * @return 编码后的总字节数；参数或容量无效时返回 0。
 * @note 多字节字段和 CRC 均按小端编码，CRC 覆盖 version 至 payload。
 */
size_t gw_frame_encode(const gw_frame_t *frame, uint8_t *output, size_t capacity);

/**
 * @brief 清零字节流解析器和错误统计。
 * @param[out] parser 当前字节流解析状态，由单一执行流拥有。
 */
void gw_parser_init(gw_parser_t *parser);

/**
 * @brief 输入一个字节，仅在得到完整合法帧时输出消息。
 * @param[in,out] parser 当前字节流解析状态，由单一执行流拥有。
 * @param[in] byte 本次新输入的一个字节。
 * @param[out] output 接收完整帧，仅在函数返回 true 时有效。
 * @return true 表示得到完整且通过校验的帧；false 表示尚未完成或已丢弃坏帧。
 * @note 同一个解析器由单一接收执行流拥有；输出对象仅在返回 true 时有效。
 */
bool gw_parser_push(gw_parser_t *parser, uint8_t byte, gw_frame_t *output);

/**
 * @brief 将现场状态编码为 STATUS 载荷。
 * @param[in] value 本次处理的消息、记录或事件对象。
 * @param[in,out] frame 写入类型和载荷；保留调用方设置的序号与会话。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool gw_status_pack(const gw_status_t *value, gw_frame_t *frame);

/**
 * @brief 检查并还原 STATUS 载荷。
 * @param[in] frame 已组装的业务帧，包括类型、序号、会话和载荷。
 * @param[out] value 本次处理的消息、记录或事件对象。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool gw_status_unpack(const gw_frame_t *frame, gw_status_t *value);

/**
 * @brief 将测点值、质量、年龄与配置版本编码为 SAMPLE 载荷。
 * @param[in] value 本次处理的消息、记录或事件对象。
 * @param[in,out] frame 写入类型和载荷；保留调用方设置的序号与会话。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool gw_sample_pack(const gw_sample_t *value, gw_frame_t *frame);

/**
 * @brief 检查并还原 SAMPLE 载荷。
 * @param[in] frame 已组装的业务帧，包括类型、序号、会话和载荷。
 * @param[out] value 本次处理的消息、记录或事件对象。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool gw_sample_unpack(const gw_frame_t *frame, gw_sample_t *value);

/**
 * @brief 将操作、参数、请求号和测点编码为 COMMAND 载荷。
 * @param[in] value 本次处理的消息、记录或事件对象。
 * @param[in,out] frame 写入类型和载荷；保留调用方设置的序号与会话。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool gw_command_pack(const gw_command_t *value, gw_frame_t *frame);

/**
 * @brief 检查并还原 COMMAND 载荷。
 * @param[in] frame 已组装的业务帧，包括类型、序号、会话和载荷。
 * @param[out] value 本次处理的消息、记录或事件对象。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool gw_command_unpack(const gw_frame_t *frame, gw_command_t *value);

/**
 * @brief 将执行结果和实际配置编码为 RESULT 载荷。
 * @param[in] value 本次处理的消息、记录或事件对象。
 * @param[in,out] frame 写入类型和载荷；保留调用方设置的序号与会话。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool gw_result_pack(const gw_result_t *value, gw_frame_t *frame);

/**
 * @brief 检查并还原 RESULT 载荷。
 * @param[in] frame 已组装的业务帧，包括类型、序号、会话和载荷。
 * @param[out] value 本次处理的消息、记录或事件对象。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool gw_result_unpack(const gw_frame_t *frame, gw_result_t *value);

#ifdef __cplusplus
}
#endif
