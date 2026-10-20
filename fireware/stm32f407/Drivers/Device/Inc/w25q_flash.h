/**
 ******************************************************************************
 * @file    w25q_flash.h
 * @brief   W25Q 设备指令驱动接口定义。
 * @details 完成 JEDEC、跨页编程、扇区擦除和 BUSY 超时；不决定配置和日志地址。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：设备驱动 / 平台适配层
 ******************************************************************************
 */

#pragma once
/*--------------------------- 头文件 ---------------------------*/
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 读取 JEDEC 并识别受支持的 Flash 容量。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool w25q_init(void);

/**
 * @brief 检查容量边界并分块读取 Flash 内容。
 * @param[in] address Flash 字节地址；擦除接口要求 4 KiB 对齐。
 * @param[out] data 输出字节缓冲区，容量由配套参数或数组长度约束。
 * @param[in] length 本次输入内容长度，单位字节。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool w25q_read(uint32_t address, void *data, size_t length);

/**
 * @brief 按 256 字节页边界拆分编程并等待完成。
 * @param[in] address Flash 字节地址；擦除接口要求 4 KiB 对齐。
 * @param[in] data 输入字节缓冲区。
 * @param[in] length 本次输入内容长度，单位字节。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 * @note 调用方负责目标擦除状态；失败可能已写入部分数据，不表示整笔原子提交。
 */
bool w25q_program(uint32_t address, const void *data, size_t length);

/**
 * @brief 检查 4 KiB 对齐并擦除一个扇区。
 * @param[in] address Flash 字节地址；擦除接口要求 4 KiB 对齐。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 * @note 擦除粒度为 4096 字节，地址必须对齐；操作含有界 BUSY 等待。
 */
bool w25q_erase(uint32_t address);
