/**
 ******************************************************************************
 * @file    rs485_port.h
 * @brief   USART2 半双工字节端口接口定义。
 * @details 拥有循环 DMA 缓冲和 PC0 方向；成功的阻塞发送已等待最后停止位离线。
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

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 启动 USART2 循环 DMA 并初始化 RTU 时序观察。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool rs485_port_open(void);

/**
 * @brief 检查接收状态并在需要时重建 USART2 DMA。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool rs485_port_ready(void);

/**
 * @brief 丢弃当前尚未消费的接收字节。
 * @return true 表示丢弃了尚未消费的字节；false 表示当前没有残留字节。
 */
bool rs485_port_discard(void);

/**
 * @brief 从循环 DMA 缓冲复制尚未消费的字节。
 * @param[out] data 输出字节缓冲区，容量由配套参数或数组长度约束。
 * @param[in] capacity 输出缓冲区容量，单位字节。
 * @return 实际处理的字节数；没有数据或容量不足时可返回 0。
 */
size_t rs485_port_read(uint8_t *data, size_t capacity);

/**
 * @brief 切换发送方向，阻塞等待 TC 后再切回接收。
 * @param[in] data 输入字节缓冲区。
 * @param[in] length 本次输入内容长度，单位字节。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 * @note 在任务上下文调用；HAL 阻塞发送成功包含 TC，不能用 TXE 提前切换方向。
 */
bool rs485_port_send(const uint8_t *data, size_t length);
