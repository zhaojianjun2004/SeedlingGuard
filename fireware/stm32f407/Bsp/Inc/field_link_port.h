/**
 ******************************************************************************
 * @file    field_link_port.h
 * @brief   UART3 与 CAN2 板级端口接口定义。
 * @details 任务消费 DMA/队列；CAN 中断只投递原始帧，驱动错误按有界间隔恢复。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：板级支持层
 ******************************************************************************
 */

#pragma once
/*--------------------------- 头文件 ---------------------------*/
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint16_t id;
    uint8_t data[8];
} board_can_frame_t;

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 创建 CAN 接收队列并启动 UART3 DMA/CAN2 端口。
 * @param[in] receive_can_id CAN 接收过滤器允许的标准标识符。
 * @return true 表示运行资源创建成功；UART/CAN 故障可由后续轮询恢复。
 */
bool field_link_port_open(uint16_t receive_can_id);

/**
 * @brief 读取 UART3 DMA 新字节，必要时重建接收并报告重启。
 * @param[out] data 输出字节缓冲区，容量由配套参数或数组长度约束。
 * @param[in] capacity 输出缓冲区容量，单位字节。
 * @param[out] out_restarted 接收是否重建 UART DMA 的标记，允许传入 NULL。
 * @return 实际处理的字节数；没有数据或容量不足时可返回 0。
 */
size_t field_link_port_read(uint8_t *data, size_t capacity, bool *out_restarted);

/**
 * @brief 阻塞发送一段 UART3 字节。
 * @param[in] data 输入字节缓冲区。
 * @param[in] length 本次输入内容长度，单位字节。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool field_link_port_write(const uint8_t *data, size_t length);

/**
 * @brief 非阻塞取出一帧标准 CAN 数据。
 * @param[in,out] frame 本次处理的消息、记录或事件对象。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool field_link_port_can_read(board_can_frame_t *frame);

/**
 * @brief 在可用邮箱中提交一帧标准 CAN 数据。
 * @param[in] standard_id 11 位标准 CAN 标识符。
 * @param[in] data 输入字节缓冲区。
 * @return true 表示已提交硬件邮箱；false 表示不可用或提交失败，不代表应用端已处理。
 */
bool field_link_port_can_write(uint16_t standard_id, const uint8_t data[8]);

/**
 * @brief 按恢复间隔尝试重启故障 CAN 控制器。
 */
void field_link_port_poll(void);

/**
 * @brief 原子复制 CAN 错误和接收队列溢出计数。
 * @param[out] out_counters 接收函数产生的结果；仅在成功条件成立时使用。
 */
void field_link_port_counters(uint32_t out_counters[2]);
