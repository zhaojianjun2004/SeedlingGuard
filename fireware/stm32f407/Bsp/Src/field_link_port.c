/**
 ******************************************************************************
 * @file    field_link_port.c
 * @brief   UART3 与 CAN2 板级端口实现。
 * @details 任务消费 DMA/队列；CAN 中断只投递原始帧，驱动错误按有界间隔恢复。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：板级支持层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "field_link_port.h"
#include "board_system.h"
#include "usart.h"
#include "can.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

/*--------------------------- 模块配置、类型与状态 ---------------------------*/

static uint8_t s_uart_rx_buffer[512];
static unsigned s_uart_rx_read_index;
static bool s_uart_rx_started;
static volatile bool s_can_ready, s_can_failed;
static volatile uint32_t s_can_errors, s_can_rx_drops;
static uint16_t s_can_filter_id;
static uint32_t s_last_can_recovery_ms;
static QueueHandle_t s_can_rx_queue;

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 配置 CAN2 过滤器、启动控制器并启用接收和错误通知。
 */
static bool field_link_can_start(void)
{
    CAN_FilterTypeDef filter = {.FilterBank = 14,
                                .FilterMode = CAN_FILTERMODE_IDMASK,
                                .FilterScale = CAN_FILTERSCALE_32BIT,
                                .FilterFIFOAssignment = CAN_FILTER_FIFO0,
                                .FilterActivation = ENABLE,
                                .SlaveStartFilterBank = 14,
                                .FilterIdHigh = (uint32_t)s_can_filter_id << 5,
                                .FilterMaskIdHigh = 0xffe0,
                                .FilterMaskIdLow = 6};
    return HAL_CAN_ConfigFilter(&hcan2, &filter) == HAL_OK && HAL_CAN_Start(&hcan2) == HAL_OK &&
           HAL_CAN_ActivateNotification(&hcan2, CAN_IT_RX_FIFO0_MSG_PENDING | CAN_IT_BUSOFF |
                                                    CAN_IT_ERROR) == HAL_OK;
}

/**
 * @brief 创建 CAN 接收队列并启动 UART3 DMA/CAN2 端口。
 * @param[in] receive_can_id CAN 接收过滤器允许的标准标识符。
 * @return true 表示运行资源创建成功；UART/CAN 故障可由后续轮询恢复。
 */
bool field_link_port_open(uint16_t receive_can_id)
{
    s_can_rx_queue = xQueueCreate(16, sizeof(board_can_frame_t));
    if (!s_can_rx_queue)
    {
        return false;
    }
    s_can_filter_id = receive_can_id;
    s_can_ready = field_link_can_start();
    s_can_failed = !s_can_ready;
    s_uart_rx_started =
        HAL_UART_Receive_DMA(&huart3, s_uart_rx_buffer, sizeof(s_uart_rx_buffer)) == HAL_OK;
    return true; /* Peripheral failure is recoverable; allocation failure is not. */
}

/**
 * @brief 读取 UART3 DMA 新字节，必要时重建接收并报告重启。
 * @param[out] data 输出字节缓冲区，容量由配套参数或数组长度约束。
 * @param[in] capacity 输出缓冲区容量，单位字节。
 * @param[out] out_restarted 接收是否重建 UART DMA 的标记，允许传入 NULL。
 * @return 实际处理的字节数；没有数据或容量不足时可返回 0。
 */
size_t field_link_port_read(uint8_t *data, size_t capacity, bool *out_restarted)
{
    if (out_restarted)
    {
        *out_restarted = false;
    }
    if (!s_uart_rx_started || huart3.ErrorCode != HAL_UART_ERROR_NONE)
    {
        HAL_UART_AbortReceive(&huart3);
        s_uart_rx_read_index = 0;
        s_uart_rx_started =
            HAL_UART_Receive_DMA(&huart3, s_uart_rx_buffer, sizeof(s_uart_rx_buffer)) == HAL_OK;
        if (out_restarted)
        {
            *out_restarted = true;
        }
    }
    if (!s_uart_rx_started)
    {
        return 0;
    }
    unsigned write_index = sizeof(s_uart_rx_buffer) - __HAL_DMA_GET_COUNTER(huart3.hdmarx);
    if (write_index == sizeof(s_uart_rx_buffer))
    {
        write_index = 0;
    }
    size_t received_length = 0;
    while (s_uart_rx_read_index != write_index && received_length < capacity)
    {
        data[received_length++] = s_uart_rx_buffer[s_uart_rx_read_index];
        s_uart_rx_read_index = (s_uart_rx_read_index + 1) % sizeof(s_uart_rx_buffer);
    }
    return received_length;
}

/**
 * @brief 阻塞发送一段 UART3 字节。
 * @param[in] data 输入字节缓冲区。
 * @param[in] length 本次输入内容长度，单位字节。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool field_link_port_write(const uint8_t *data, size_t length)
{
    return length <= UINT16_MAX &&
           HAL_UART_Transmit(&huart3, (uint8_t *)data, (uint16_t)length, 100) == HAL_OK;
}

/**
 * @brief 非阻塞取出一帧标准 CAN 数据。
 * @param[in,out] frame 本次处理的消息、记录或事件对象。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool field_link_port_can_read(board_can_frame_t *frame)
{
    return s_can_rx_queue && xQueueReceive(s_can_rx_queue, frame, 0) == pdTRUE;
}

/**
 * @brief 在可用邮箱中提交一帧标准 CAN 数据。
 * @param[in] standard_id 11 位标准 CAN 标识符。
 * @param[in] data 输入字节缓冲区。
 * @return true 表示已提交硬件邮箱；false 表示不可用或提交失败，不代表应用端已处理。
 */
bool field_link_port_can_write(uint16_t standard_id, const uint8_t data[8])
{
    if (!s_can_ready || !HAL_CAN_GetTxMailboxesFreeLevel(&hcan2))
    {
        return false;
    }
    CAN_TxHeaderTypeDef tx_header = {
        .StdId = standard_id, .IDE = CAN_ID_STD, .RTR = CAN_RTR_DATA, .DLC = 8};
    uint32_t mailbox_index;
    if (HAL_CAN_AddTxMessage(&hcan2, &tx_header, (uint8_t *)data, &mailbox_index) != HAL_OK)
    {
        ++s_can_errors;
        return false;
    }
    return true;
}

/**
 * @brief 按恢复间隔尝试重启故障 CAN 控制器。
 */
void field_link_port_poll(void)
{
    if ((s_can_failed || !s_can_ready) && board_millis() - s_last_can_recovery_ms >= 2000)
    {
        HAL_CAN_Stop(&hcan2);
        s_can_ready = field_link_can_start();
        s_can_failed = !s_can_ready;
        s_last_can_recovery_ms = board_millis();
    }
}

/**
 * @brief 原子复制 CAN 错误和接收队列溢出计数。
 * @param[out] out_counters 接收函数产生的结果；仅在成功条件成立时使用。
 */
void field_link_port_counters(uint32_t out_counters[2])
{
    taskENTER_CRITICAL();
    out_counters[0] = s_can_errors;
    out_counters[1] = s_can_rx_drops;
    taskEXIT_CRITICAL();
}

/**
 * @brief 在接收中断中投递原始 CAN 帧并按需唤醒任务。
 * @param[in] can SDK/HAL 传入的 CAN 控制器句柄。
 * @note 中断上下文，使用 FromISR 接口；队列满只统计丢失，不无限等待。
 */
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *can)
{
    if (can->Instance != CAN2 || !s_can_rx_queue)
    {
        return;
    }
    BaseType_t higher_priority_woken = pdFALSE;
    while (HAL_CAN_GetRxFifoFillLevel(can, CAN_RX_FIFO0))
    {
        CAN_RxHeaderTypeDef header;
        board_can_frame_t frame;
        if (HAL_CAN_GetRxMessage(can, CAN_RX_FIFO0, &header, frame.data) != HAL_OK)
        {
            break;
        }
        if (header.IDE != CAN_ID_STD || header.RTR != CAN_RTR_DATA || header.DLC != 8)
        {
            continue;
        }
        frame.id = (uint16_t)header.StdId;
        if (xQueueSendFromISR(s_can_rx_queue, &frame, &higher_priority_woken) != pdTRUE)
        {
            ++s_can_rx_drops;
        }
    }
    portYIELD_FROM_ISR(higher_priority_woken);
}

/**
 * @brief 在错误回调中记录 CAN 故障并请求后续恢复。
 * @param[in] can SDK/HAL 传入的 CAN 控制器句柄。
 * @note 中断上下文，只设置诊断和恢复标志，实际重启留给任务。
 */
void HAL_CAN_ErrorCallback(CAN_HandleTypeDef *can)
{
    if (can->Instance == CAN2)
    {
        ++s_can_errors;
        if (HAL_CAN_GetError(can) & HAL_CAN_ERROR_BOF)
        {
            s_can_ready = false;
            s_can_failed = true;
        }
    }
}
