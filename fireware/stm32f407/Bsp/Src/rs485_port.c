/**
 ******************************************************************************
 * @file    rs485_port.c
 * @brief   USART2 半双工字节端口实现。
 * @details 拥有循环 DMA 缓冲和 PC0 方向；成功的阻塞发送已等待最后停止位离线。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：板级支持层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "rs485_port.h"
#include "rs485_timing.h"
#include "main.h"
#include "usart.h"

/*--------------------------- 模块配置、类型与状态 ---------------------------*/

static uint8_t s_rx_buffer[128];
static unsigned s_rx_read_index;
static bool s_rx_started;

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 取得 USART2 循环 DMA 的当前写位置。
 */
static unsigned rs485_dma_write_index(void)
{
    unsigned write_index = sizeof(s_rx_buffer) - __HAL_DMA_GET_COUNTER(huart2.hdmarx);
    return write_index == sizeof(s_rx_buffer) ? 0 : write_index;
}

/**
 * @brief 启动 USART2 循环 DMA 并初始化 RTU 时序观察。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool rs485_port_open(void)
{
    s_rx_started = HAL_UART_Receive_DMA(&huart2, s_rx_buffer, sizeof(s_rx_buffer)) == HAL_OK;
    rs485_timing_init();
    return s_rx_started;
}

/**
 * @brief 检查接收状态并在需要时重建 USART2 DMA。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool rs485_port_ready(void)
{
    if (!s_rx_started || huart2.ErrorCode != HAL_UART_ERROR_NONE)
    {
        HAL_UART_AbortReceive(&huart2);
        s_rx_read_index = 0;
        s_rx_started = HAL_UART_Receive_DMA(&huart2, s_rx_buffer, sizeof(s_rx_buffer)) == HAL_OK;
    }
    return s_rx_started;
}

/**
 * @brief 丢弃当前尚未消费的接收字节。
 * @return true 表示丢弃了尚未消费的字节；false 表示当前没有残留字节。
 */
bool rs485_port_discard(void)
{
    unsigned write_index = rs485_dma_write_index();
    bool received_pending_bytes = write_index != s_rx_read_index;
    s_rx_read_index = write_index;
    return received_pending_bytes;
}

/**
 * @brief 从循环 DMA 缓冲复制尚未消费的字节。
 * @param[out] data 输出字节缓冲区，容量由配套参数或数组长度约束。
 * @param[in] capacity 输出缓冲区容量，单位字节。
 * @return 实际处理的字节数；没有数据或容量不足时可返回 0。
 */
size_t rs485_port_read(uint8_t *data, size_t capacity)
{
    unsigned write_index = rs485_dma_write_index();
    size_t received_length = 0;
    while (s_rx_read_index != write_index && received_length < capacity)
    {
        data[received_length++] = s_rx_buffer[s_rx_read_index];
        s_rx_read_index = (s_rx_read_index + 1) % sizeof(s_rx_buffer);
    }
    return received_length;
}

/**
 * @brief 切换发送方向，阻塞等待 TC 后再切回接收。
 * @param[in] data 输入字节缓冲区。
 * @param[in] length 本次输入内容长度，单位字节。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 * @note 在任务上下文调用；HAL 阻塞发送成功包含 TC，不能用 TXE 提前切换方向。
 */
bool rs485_port_send(const uint8_t *data, size_t length)
{
    if (length > UINT16_MAX)
    {
        return false;
    }
    HAL_GPIO_WritePin(RS485_DE_RE_GPIO_Port, RS485_DE_RE_Pin, GPIO_PIN_SET);
    /* Blocking HAL success includes TC: the last stop bit has left the wire. */
    HAL_StatusTypeDef tx_result = HAL_UART_Transmit(&huart2, (uint8_t *)data, (uint16_t)length, 30);
    HAL_GPIO_WritePin(RS485_DE_RE_GPIO_Port, RS485_DE_RE_Pin, GPIO_PIN_RESET);
    return tx_result == HAL_OK;
}
