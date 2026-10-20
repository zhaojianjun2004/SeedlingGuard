/**
 ******************************************************************************
 * @file    rs485_timing.h
 * @brief   RTU 静默间隔观察接口定义。
 * @details TIM7 每 50us 观察 DMA，DWT 记录时间；中断不做协议解析或 Flash 操作。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：板级支持层
 ******************************************************************************
 */

#pragma once
/*--------------------------- 头文件 ---------------------------*/
#include <stdbool.h>

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 配置 DWT 与 TIM7，为 DMA 进度提供细粒度时间观察。
 */
void rs485_timing_init(void);

/**
 * @brief 重置本次响应的静默间隔观察上下文。
 */
void rs485_timing_begin(void);

/**
 * @brief 结束本次尝试的帧内间隔检查。
 */
void rs485_timing_end(void);

/**
 * @brief 读取本次响应是否出现非法字符间隔。
 * @return true 表示检测到非法帧内间隔；false 表示未检测到。
 */
bool rs485_timing_gap_error(void);

/**
 * @brief 判断收到字节后是否已经达到 RTU 帧间静默。
 * @return true 表示已经达到帧间静默；false 表示仍需等待。
 */
bool rs485_timing_frame_quiet(void);
