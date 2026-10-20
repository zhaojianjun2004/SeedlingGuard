/**
 ******************************************************************************
 * @file    board_flash_port.h
 * @brief   SPI Flash 板级字节接口接口定义。
 * @details 绑定 SPI1 与 PG6 片选，器件指令和分区策略由上层决定。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：板级支持层
 ******************************************************************************
 */

#pragma once
/*--------------------------- 头文件 ---------------------------*/
#include <stdbool.h>
#include <stddef.h>

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 控制板载 SPI Flash 片选电平。
 * @param[in] selected true 选中器件，false 释放片选。
 */
void board_flash_select(bool selected);

/**
 * @brief 通过 SPI1 发送一段字节。
 * @param[in] data 输入字节缓冲区。
 * @param[in] length 本次输入内容长度，单位字节。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool board_flash_tx(const void *data, size_t length);

/**
 * @brief 通过 SPI1 接收一段字节。
 * @param[out] data 输出字节缓冲区，容量由配套参数或数组长度约束。
 * @param[in] length 本次输入内容长度，单位字节。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool board_flash_rx(void *data, size_t length);
