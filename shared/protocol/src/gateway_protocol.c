/**
 ******************************************************************************
 * @file    gateway_protocol.c
 * @brief   UART V2 应用协议实现。
 * @details 纯 C 的固定字节布局、小端编解码与 CRC16；不访问 UART 或创建任务。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：协议层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "gateway_protocol.h"

#include <string.h>

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 按小端顺序写入 16 位整数。
 */
static void protocol_write_u16_le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

/**
 * @brief 按小端顺序读取 16 位整数。
 */
static uint16_t protocol_read_u16_le(const uint8_t *src)
{
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

/**
 * @brief 按小端顺序写入 32 位整数。
 */
static void protocol_write_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

/**
 * @brief 按小端顺序读取 32 位整数。
 */
static uint32_t protocol_read_u32_le(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

/**
 * @brief 计算 CRC-16/MODBUS，初值 0xFFFF、多项式 0xA001。
 * @param[in] data 输入字节缓冲区。
 * @param[in] length 本次输入内容长度，单位字节。
 * @return CRC-16/MODBUS 校验值；空数据指针保持初值 0xFFFF。
 */
uint16_t gw_crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFFU;
    if (data == NULL)
    {
        return crc;
    }
    for (size_t i = 0; i < length; ++i)
    {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8U; ++bit)
        {
            crc = (crc & 1U) ? (uint16_t)((crc >> 1) ^ 0xA001U) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

/**
 * @brief 编码完整 UART V2 帧并追加 CRC16。
 * @param[in] frame 已组装的业务帧，包括类型、序号、会话和载荷。
 * @param[out] output 输出字节缓冲区，容量由配套参数或数组长度约束。
 * @param[in] capacity 输出缓冲区容量，单位字节。
 * @return 编码后的总字节数；参数或容量无效时返回 0。
 * @note 多字节字段和 CRC 均按小端编码，CRC 覆盖 version 至 payload。
 */
size_t gw_frame_encode(const gw_frame_t *frame, uint8_t *output, size_t capacity)
{
    if (frame == NULL || output == NULL || frame->length > GW_FRAME_MAX_PAYLOAD ||
        capacity < (size_t)GW_FRAME_OVERHEAD + frame->length)
    {
        return 0U;
    }
    const size_t total = GW_FRAME_OVERHEAD + frame->length;
    output[0] = GW_FRAME_SOF0;
    output[1] = GW_FRAME_SOF1;
    output[2] = GW_PROTOCOL_VERSION;
    output[3] = frame->type;
    protocol_write_u16_le(&output[4], frame->seq);
    protocol_write_u16_le(&output[6], frame->session_id);
    output[8] = frame->length;
    memcpy(&output[9], frame->payload, frame->length);
    protocol_write_u16_le(&output[9U + frame->length], gw_crc16(&output[2], 7U + frame->length));
    return total;
}

/**
 * @brief 清零字节流解析器和错误统计。
 * @param[out] parser 当前字节流解析状态，由单一执行流拥有。
 */
void gw_parser_init(gw_parser_t *parser)
{
    if (parser != NULL)
    {
        memset(parser, 0, sizeof(*parser));
    }
}

/**
 * @brief 输入一个字节，仅在得到完整合法帧时输出消息。
 * @param[in,out] parser 当前字节流解析状态，由单一执行流拥有。
 * @param[in] byte 本次新输入的一个字节。
 * @param[out] output 接收完整帧，仅在函数返回 true 时有效。
 * @return true 表示得到完整且通过校验的帧；false 表示尚未完成或已丢弃坏帧。
 * @note 同一个解析器由单一接收执行流拥有；输出对象仅在返回 true 时有效。
 */
bool gw_parser_push(gw_parser_t *parser, uint8_t byte, gw_frame_t *output)
{
    if (parser == NULL || output == NULL)
    {
        return false;
    }
    if (parser->used == 0U)
    {
        if (byte == GW_FRAME_SOF0)
        {
            parser->bytes[parser->used++] = byte;
        }
        return false;
    }
    if (parser->used == 1U)
    {
        if (byte == GW_FRAME_SOF1)
        {
            parser->bytes[parser->used++] = byte;
        }
        else if (byte != GW_FRAME_SOF0)
        {
            parser->used = 0U;
            ++parser->framing_errors;
        }
        return false;
    }

    parser->bytes[parser->used++] = byte;
    if (parser->used == 9U)
    {
        if (parser->bytes[2] != GW_PROTOCOL_VERSION || parser->bytes[8] > GW_FRAME_MAX_PAYLOAD)
        {
            parser->used = (byte == GW_FRAME_SOF0) ? 1U : 0U;
            ++parser->framing_errors;
            return false;
        }
    }
    if (parser->used < 9U)
    {
        return false;
    }
    const uint8_t length = parser->bytes[8];
    const size_t total = GW_FRAME_OVERHEAD + length;
    if (parser->used < total)
    {
        return false;
    }
    const uint16_t expected = gw_crc16(&parser->bytes[2], 7U + length);
    const uint16_t received = protocol_read_u16_le(&parser->bytes[9U + length]);
    if (expected != received)
    {
        ++parser->crc_errors;
        parser->used = (byte == GW_FRAME_SOF0) ? 1U : 0U;
        return false;
    }
    output->type = parser->bytes[3];
    output->seq = protocol_read_u16_le(&parser->bytes[4]);
    output->session_id = protocol_read_u16_le(&parser->bytes[6]);
    output->length = length;
    memcpy(output->payload, &parser->bytes[9], length);
    parser->used = 0U;
    return true;
}

/**
 * @brief 将现场状态编码为 STATUS 载荷。
 * @param[in] value 本次处理的消息、记录或事件对象。
 * @param[in,out] frame 写入类型和载荷；保留调用方设置的序号与会话。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool gw_status_pack(const gw_status_t *value, gw_frame_t *frame)
{
    if (value == NULL || frame == NULL)
    {
        return false;
    }
    frame->type = GW_MSG_STATUS;
    frame->length = 29U;
    protocol_write_u16_le(&frame->payload[0], value->boot_id);
    frame->payload[2] = value->collect_enabled ? 1U : 0U;
    frame->payload[3] = 0U;
    protocol_write_u32_le(&frame->payload[4], value->sample_period_ms);
    protocol_write_u32_le(&frame->payload[8], value->config_version);
    frame->payload[12] = value->storage_ready;
    protocol_write_u32_le(&frame->payload[13], value->sample_drops);
    protocol_write_u32_le(&frame->payload[17], value->log_drops);
    protocol_write_u32_le(&frame->payload[21], value->latest_event_id);
    protocol_write_u32_le(frame->payload + 25, value->log_pending);
    return true;
}

/**
 * @brief 检查并还原 STATUS 载荷。
 * @param[in] frame 已组装的业务帧，包括类型、序号、会话和载荷。
 * @param[out] value 本次处理的消息、记录或事件对象。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool gw_status_unpack(const gw_frame_t *frame, gw_status_t *value)
{
    if (frame == NULL || value == NULL || frame->type != GW_MSG_STATUS || frame->length != 29U)
    {
        return false;
    }
    value->boot_id = protocol_read_u16_le(&frame->payload[0]);
    value->collect_enabled = frame->payload[2] != 0U;
    value->sample_period_ms = protocol_read_u32_le(&frame->payload[4]);
    value->config_version = protocol_read_u32_le(&frame->payload[8]);
    value->storage_ready = frame->payload[12] != 0;
    value->sample_drops = protocol_read_u32_le(&frame->payload[13]);
    value->log_drops = protocol_read_u32_le(&frame->payload[17]);
    value->latest_event_id = protocol_read_u32_le(&frame->payload[21]);
    value->log_pending = protocol_read_u32_le(frame->payload + 25);
    return true;
}

/**
 * @brief 将测点值、质量、年龄与配置版本编码为 SAMPLE 载荷。
 * @param[in] value 本次处理的消息、记录或事件对象。
 * @param[in,out] frame 写入类型和载荷；保留调用方设置的序号与会话。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool gw_sample_pack(const gw_sample_t *value, gw_frame_t *frame)
{
    if (value == NULL || frame == NULL || value->point_id < GW_POINT_TEMPERATURE ||
        value->point_id > NURSERY_POINT_COUNT || value->quality > GW_QUALITY_FAULT)
    {
        return false;
    }
    frame->type = GW_MSG_SAMPLE;
    frame->length = 15U;
    frame->payload[0] = value->point_id;
    frame->payload[1] = value->quality;
    protocol_write_u32_le(&frame->payload[2], (uint32_t)value->scaled_value);
    protocol_write_u32_le(&frame->payload[6], value->sample_age_ms);
    frame->payload[10] = value->alarm_state;
    protocol_write_u32_le(frame->payload + 11, value->config_version);
    return true;
}

/**
 * @brief 检查并还原 SAMPLE 载荷。
 * @param[in] frame 已组装的业务帧，包括类型、序号、会话和载荷。
 * @param[out] value 本次处理的消息、记录或事件对象。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool gw_sample_unpack(const gw_frame_t *frame, gw_sample_t *value)
{
    if (frame == NULL || value == NULL || frame->type != GW_MSG_SAMPLE || frame->length != 15U ||
        frame->payload[0] < GW_POINT_TEMPERATURE || frame->payload[0] > NURSERY_POINT_COUNT ||
        frame->payload[1] > GW_QUALITY_FAULT || frame->payload[10] > NURSERY_ALARM_UNKNOWN)
    {
        return false;
    }
    value->point_id = frame->payload[0];
    value->quality = frame->payload[1];
    value->scaled_value = (int32_t)protocol_read_u32_le(&frame->payload[2]);
    value->sample_age_ms = protocol_read_u32_le(&frame->payload[6]);
    value->alarm_state = frame->payload[10];
    value->config_version = protocol_read_u32_le(frame->payload + 11);
    return true;
}

/**
 * @brief 将操作、参数、请求号和测点编码为 COMMAND 载荷。
 * @param[in] value 本次处理的消息、记录或事件对象。
 * @param[in,out] frame 写入类型和载荷；保留调用方设置的序号与会话。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool gw_command_pack(const gw_command_t *value, gw_frame_t *frame)
{
    if (value == NULL || frame == NULL || value->op < GW_CMD_SET_ENABLE ||
        value->op > GW_CMD_QUERY_EVENTS)
    {
        return false;
    }
    frame->type = GW_MSG_COMMAND;
    frame->length = 10U;
    frame->payload[0] = value->op;
    protocol_write_u32_le(&frame->payload[1], value->value);
    protocol_write_u32_le(&frame->payload[5], value->request_id);
    frame->payload[9] = value->point_id;
    return true;
}

/**
 * @brief 检查并还原 COMMAND 载荷。
 * @param[in] frame 已组装的业务帧，包括类型、序号、会话和载荷。
 * @param[out] value 本次处理的消息、记录或事件对象。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool gw_command_unpack(const gw_frame_t *frame, gw_command_t *value)
{
    if (frame == NULL || value == NULL || frame->type != GW_MSG_COMMAND || frame->length != 10U ||
        frame->payload[0] < GW_CMD_SET_ENABLE || frame->payload[0] > GW_CMD_QUERY_EVENTS)
    {
        return false;
    }
    value->op = frame->payload[0];
    value->value = protocol_read_u32_le(&frame->payload[1]);
    value->request_id = protocol_read_u32_le(&frame->payload[5]);
    value->point_id = frame->payload[9];
    return true;
}

/**
 * @brief 将执行结果和实际配置编码为 RESULT 载荷。
 * @param[in] value 本次处理的消息、记录或事件对象。
 * @param[in,out] frame 写入类型和载荷；保留调用方设置的序号与会话。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool gw_result_pack(const gw_result_t *value, gw_frame_t *frame)
{
    if (value == NULL || frame == NULL || value->op < GW_CMD_SET_ENABLE ||
        value->op > GW_CMD_QUERY_EVENTS || value->result > GW_RESULT_ERROR)
    {
        return false;
    }
    frame->type = GW_MSG_RESULT;
    frame->length = 15U;
    frame->payload[0] = value->op;
    frame->payload[1] = value->result;
    frame->payload[2] = value->collect_enabled ? 1U : 0U;
    protocol_write_u32_le(&frame->payload[3], value->sample_period_ms);
    protocol_write_u32_le(&frame->payload[7], value->request_id);
    protocol_write_u32_le(&frame->payload[11], value->config_version);
    return true;
}

/**
 * @brief 检查并还原 RESULT 载荷。
 * @param[in] frame 已组装的业务帧，包括类型、序号、会话和载荷。
 * @param[out] value 本次处理的消息、记录或事件对象。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool gw_result_unpack(const gw_frame_t *frame, gw_result_t *value)
{
    if (frame == NULL || value == NULL || frame->type != GW_MSG_RESULT || frame->length != 15U ||
        frame->payload[0] < GW_CMD_SET_ENABLE || frame->payload[0] > GW_CMD_QUERY_EVENTS ||
        frame->payload[1] > GW_RESULT_ERROR)
    {
        return false;
    }
    value->op = frame->payload[0];
    value->result = frame->payload[1];
    value->collect_enabled = frame->payload[2] != 0U;
    value->sample_period_ms = protocol_read_u32_le(&frame->payload[3]);
    value->request_id = protocol_read_u32_le(&frame->payload[7]);
    value->config_version = protocol_read_u32_le(&frame->payload[11]);
    return true;
}
