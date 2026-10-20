/**
 ******************************************************************************
 * @file    uart_port.h
 * @brief   ESP32 UART 字节端口接口定义。
 * @details 只封装外设资源和字节收发；端口生命周期由应用拥有，帧解析由链路服务完成。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：设备驱动 / 平台适配层
 ******************************************************************************
 */

#pragma once
/*--------------------------- 头文件 ---------------------------*/
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
typedef struct nursery_uart *nursery_uart_handle_t;

typedef struct
{
    int port_number, tx_gpio, rx_gpio;
    uint32_t baud_rate;
} nursery_uart_config_t;

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 校验端口参数，安装 UART 驱动并配置引脚。
 * @param[in] config 模块配置及其取值约束。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t nursery_uart_open(const nursery_uart_config_t *config, nursery_uart_handle_t *out_handle);

/**
 * @brief 从 UART 接收缓冲读取字节。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] data 输出字节缓冲区，容量由配套参数或数组长度约束。
 * @param[in] capacity 输出缓冲区容量，单位字节。
 * @param[in] timeout_ms 时间长度，单位 ms；等待接口可能阻塞到该期限。
 * @return 实际读取字节数；超时可返回 0，无效句柄或驱动错误返回负值。
 */
int nursery_uart_read(nursery_uart_handle_t handle, uint8_t *data, size_t capacity,
                      uint32_t timeout_ms);

/**
 * @brief 将待发送字节交给 UART 驱动。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[in] data 输入字节缓冲区。
 * @param[in] length 本次输入内容长度，单位字节。
 * @return 交给驱动的字节数；负值表示失败，不代表现场设备已经接收。
 */
int nursery_uart_write(nursery_uart_handle_t handle, const uint8_t *data, size_t length);

/**
 * @brief 关闭 UART 驱动并释放端口句柄。
 * @param[in] handle 已创建的模块实例句柄。
 * @note 释放前必须先停止所有使用该端口的任务；关闭后句柄不可继续使用。
 */
void nursery_uart_close(nursery_uart_handle_t handle);
