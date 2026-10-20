/**
 ******************************************************************************
 * @file    uart_port.c
 * @brief   ESP32 UART 字节端口实现。
 * @details 只封装外设资源和字节收发；端口生命周期由应用拥有，帧解析由链路服务完成。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：设备驱动 / 平台适配层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "uart_port.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include <stdlib.h>

/*--------------------------- 模块配置、类型与状态 ---------------------------*/

struct nursery_uart
{
    uart_port_t port;
};

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 校验端口参数，安装 UART 驱动并配置引脚。
 * @param[in] config 模块配置及其取值约束。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t nursery_uart_open(const nursery_uart_config_t *config, nursery_uart_handle_t *out_handle)
{
    /* 校验端口和引脚，再分配本模块拥有的运行资源。 */
    if (!config || !out_handle || config->port_number < 0 || config->port_number >= UART_NUM_MAX ||
        !config->baud_rate || !GPIO_IS_VALID_OUTPUT_GPIO(config->tx_gpio) ||
        !GPIO_IS_VALID_GPIO(config->rx_gpio))
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    nursery_uart_handle_t handle = calloc(1, sizeof(*handle));
    if (!handle)
    {
        return ESP_ERR_NO_MEM;
    }
    handle->port = (uart_port_t)config->port_number;
    /* 按应用注入的接线建立固定 8N1 字节端口。 */
    uart_config_t uart_config = {.baud_rate = (int)config->baud_rate,
                                 .data_bits = UART_DATA_8_BITS,
                                 .parity = UART_PARITY_DISABLE,
                                 .stop_bits = UART_STOP_BITS_1,
                                 .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                                 .source_clk = UART_SCLK_DEFAULT};
    esp_err_t error = uart_driver_install(handle->port, 2048, 512, 0, NULL, 0);
    if (error != ESP_OK)
    {
        free(handle);
        return error;
    }
    error = uart_param_config(handle->port, &uart_config);
    if (error == ESP_OK)
    {
        error = uart_set_pin(handle->port, config->tx_gpio, config->rx_gpio, UART_PIN_NO_CHANGE,
                             UART_PIN_NO_CHANGE);
    }
    /* 配置失败只释放本次已安装的驱动，避免泄漏端口资源。 */
    if (error != ESP_OK)
    {
        uart_driver_delete(handle->port);
        free(handle);
        return error;
    }
    *out_handle = handle;
    return ESP_OK;
}

/**
 * @brief 从 UART 接收缓冲读取字节。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] data 输出字节缓冲区，容量由配套参数或数组长度约束。
 * @param[in] capacity 输出缓冲区容量，单位字节。
 * @param[in] timeout_ms 时间长度，单位 ms；等待接口可能阻塞到该期限。
 * @return 实际读取字节数；超时可返回 0，无效句柄或驱动错误返回负值。
 */
int nursery_uart_read(nursery_uart_handle_t handle, uint8_t *data, size_t capacity,
                      uint32_t timeout_ms)
{
    if (!handle || !data)
    {
        return -1;
    }
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms && !ticks)
    {
        ticks = 1;
    }
    return uart_read_bytes(handle->port, data, capacity, ticks);
}

/**
 * @brief 将待发送字节交给 UART 驱动。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[in] data 输入字节缓冲区。
 * @param[in] length 本次输入内容长度，单位字节。
 * @return 交给驱动的字节数；负值表示失败，不代表现场设备已经接收。
 */
int nursery_uart_write(nursery_uart_handle_t handle, const uint8_t *data, size_t length)
{
    return handle && data ? uart_write_bytes(handle->port, data, length) : -1;
}

/**
 * @brief 关闭 UART 驱动并释放端口句柄。
 * @param[in] handle 已创建的模块实例句柄。
 * @note 释放前必须先停止所有使用该端口的任务；关闭后句柄不可继续使用。
 */
void nursery_uart_close(nursery_uart_handle_t handle)
{
    if (handle)
    {
        uart_driver_delete(handle->port);
        free(handle);
    }
}
