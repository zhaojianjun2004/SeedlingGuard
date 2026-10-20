/**
 ******************************************************************************
 * @file    can_diag_protocol.c
 * @brief   CAN 诊断载荷映射实现。
 * @details 纯 C 地转换标准诊断帧，保持 UART 业务模型与硬件邮箱实现分离。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：协议层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "can_diag_protocol.h"
#include <string.h>

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 将一个整数按小端写入 CAN 载荷。
 */
static void can_diag_write_u32_le(uint8_t *data, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++)
    {
        data[i] = (uint8_t)(value >> (8 * i));
    }
}

/**
 * @brief 将支持的业务消息映射为 8 字节 CAN 诊断数据。
 * @param[in] frame 已组装的业务帧，包括类型、序号、会话和载荷。
 * @param[out] id 接收该消息对应的 11 位标准 CAN 标识符。
 * @param[out] data 输出字节缓冲区，容量由配套参数或数组长度约束。
 * @return true 表示消息可映射且载荷有效；false 表示类型不支持或参数无效。
 */
bool gw_can_diag_encode(const gw_frame_t *frame, uint16_t *id, uint8_t data[8])
{
    if (!frame || !id || !data)
    {
        return false;
    }
    memset(data, 0, 8);
    data[0] = GW_PROTOCOL_VERSION;
    if (frame->type == GW_MSG_STATUS)
    {
        gw_status_t sample_or_status;
        if (!gw_status_unpack(frame, &sample_or_status))
        {
            return false;
        }
        *id = GW_CAN_STATUS_ID;
        data[1] = (uint8_t)sample_or_status.boot_id;
        data[2] = (uint8_t)(sample_or_status.boot_id >> 8);
        data[3] = sample_or_status.collect_enabled;
        can_diag_write_u32_le(data + 4, sample_or_status.sample_period_ms);
    }
    else if (frame->type == GW_MSG_SAMPLE)
    {
        gw_sample_t sample_or_status;
        if (!gw_sample_unpack(frame, &sample_or_status))
        {
            return false;
        }
        *id = GW_CAN_SAMPLE_ID;
        data[1] = sample_or_status.point_id;
        data[2] = sample_or_status.quality;
        data[3] = sample_or_status.alarm_state;
        can_diag_write_u32_le(data + 4, (uint32_t)sample_or_status.scaled_value);
    }
    else if (frame->type == GW_MSG_RESULT)
    {
        gw_result_t result;
        if (!gw_result_unpack(frame, &result))
        {
            return false;
        }
        *id = GW_CAN_RESULT_ID;
        data[1] = (uint8_t)frame->seq;
        data[2] = result.result;
        data[3] = result.collect_enabled;
        can_diag_write_u32_le(data + 4, result.sample_period_ms);
    }
    else
    {
        return false;
    }
    return true;
}

/**
 * @brief 校验 CAN 诊断命令并还原请求参数。
 * @param[in] id 11 位标准 CAN 标识符。
 * @param[in] data 输入字节缓冲区。
 * @param[out] command 本次处理的消息、记录或事件对象。
 * @param[out] out_sequence 接收函数产生的结果；仅在成功条件成立时使用。
 * @return true 表示命令格式与操作合法；false 表示拒绝该帧。
 */
bool gw_can_diag_decode(uint16_t id, const uint8_t data[8], gw_command_t *command,
                        uint16_t *out_sequence)
{
    if (!data || !command || !out_sequence || id != GW_CAN_COMMAND_ID ||
        data[0] != GW_PROTOCOL_VERSION || data[2] < GW_CMD_SET_ENABLE ||
        data[2] > GW_CMD_SET_HYSTERESIS)
    {
        return false;
    }
    *out_sequence = data[1];
    *command = (gw_command_t){.op = data[2],
                              .point_id = data[3],
                              .request_id = data[1],
                              .value = (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
                                       ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24)};
    return true;
}
