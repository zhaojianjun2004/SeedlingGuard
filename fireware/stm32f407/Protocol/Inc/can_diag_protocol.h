/**
 ******************************************************************************
 * @file    can_diag_protocol.h
 * @brief   CAN 诊断载荷映射接口定义。
 * @details 纯 C 地转换标准诊断帧，保持 UART 业务模型与硬件邮箱实现分离。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：协议层
 ******************************************************************************
 */

#pragma once
/*--------------------------- 头文件 ---------------------------*/
#include "gateway_protocol.h"
#define GW_CAN_STATUS_ID  0x100U
#define GW_CAN_SAMPLE_ID  0x200U
#define GW_CAN_COMMAND_ID 0x300U
#define GW_CAN_RESULT_ID  0x301U

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 将支持的业务消息映射为 8 字节 CAN 诊断数据。
 * @param[in] frame 已组装的业务帧，包括类型、序号、会话和载荷。
 * @param[out] id 接收该消息对应的 11 位标准 CAN 标识符。
 * @param[out] data 输出字节缓冲区，容量由配套参数或数组长度约束。
 * @return true 表示消息可映射且载荷有效；false 表示类型不支持或参数无效。
 */
bool gw_can_diag_encode(const gw_frame_t *frame, uint16_t *id, uint8_t data[8]);

/**
 * @brief 校验 CAN 诊断命令并还原请求参数。
 * @param[in] id 11 位标准 CAN 标识符。
 * @param[in] data 输入字节缓冲区。
 * @param[out] command 本次处理的消息、记录或事件对象。
 * @param[out] out_sequence 接收函数产生的结果；仅在成功条件成立时使用。
 * @return true 表示命令格式与操作合法；false 表示拒绝该帧。
 */
bool gw_can_diag_decode(uint16_t id, const uint8_t data[8], gw_command_t *command,
                        uint16_t *out_sequence);
