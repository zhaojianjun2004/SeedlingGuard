/**
 ******************************************************************************
 * @file    rs485_timing.c
 * @brief   RTU 静默间隔观察实现。
 * @details TIM7 每 50us 观察 DMA，DWT 记录时间；中断不做协议解析或 Flash 操作。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：板级支持层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "rs485_timing.h"
#include "usart.h"
#include "main.h"

/*--------------------------- 模块配置、类型与状态 ---------------------------*/

static volatile uint32_t s_previous_dma_remaining, s_last_rx_cycle;
static volatile bool s_attempt_tracking, s_frame_started, s_gap_error;
static uint32_t s_cycles_per_us;

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 配置 DWT 与 TIM7，为 DMA 进度提供细粒度时间观察。
 */
void rs485_timing_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    s_cycles_per_us = HAL_RCC_GetHCLKFreq() / 1000000U;
    __HAL_RCC_TIM7_CLK_ENABLE();
    uint32_t timer_clock_hz = HAL_RCC_GetPCLK1Freq();
    if (RCC->CFGR & RCC_CFGR_PPRE1)
    {
        timer_clock_hz *= 2;
    }
    TIM7->PSC = timer_clock_hz / 1000000U - 1;
    TIM7->ARR = 49;
    TIM7->EGR = TIM_EGR_UG;
    TIM7->SR = 0;
    TIM7->DIER = TIM_DIER_UIE;
    HAL_NVIC_SetPriority(TIM7_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(TIM7_IRQn);
    s_previous_dma_remaining = __HAL_DMA_GET_COUNTER(huart2.hdmarx);
    s_last_rx_cycle = DWT->CYCCNT;
    TIM7->CR1 = TIM_CR1_CEN;
}

/**
 * @brief 重置本次响应的静默间隔观察上下文。
 */
void rs485_timing_begin(void)
{
    HAL_NVIC_DisableIRQ(TIM7_IRQn);
    s_previous_dma_remaining = __HAL_DMA_GET_COUNTER(huart2.hdmarx);
    s_last_rx_cycle = DWT->CYCCNT;
    s_gap_error = false;
    s_frame_started = false;
    s_attempt_tracking = true;
    HAL_NVIC_EnableIRQ(TIM7_IRQn);
}

/**
 * @brief 结束本次尝试的帧内间隔检查。
 */
void rs485_timing_end(void)
{
    s_attempt_tracking = false;
}

/**
 * @brief 读取本次响应是否出现非法字符间隔。
 * @return true 表示检测到非法帧内间隔；false 表示未检测到。
 */
bool rs485_timing_gap_error(void)
{
    return s_gap_error;
}

/**
 * @brief 判断收到字节后是否已经达到 RTU 帧间静默。
 * @return true 表示已经达到帧间静默；false 表示仍需等待。
 */
bool rs485_timing_frame_quiet(void)
{
    return s_frame_started && (uint32_t)(DWT->CYCCNT - s_last_rx_cycle) >= 3646U * s_cycles_per_us;
}

/**
 * @brief 周期观察 DMA 字节进度，记录接收时刻和帧内间隔错误。
 * @note 中断上下文，仅观察索引和时间，不解析协议、写 Flash 或调用阻塞接口。
 */
void TIM7_IRQHandler(void)
{
    if (!(TIM7->SR & TIM_SR_UIF))
    {
        return;
    }
    TIM7->SR = 0;
    uint32_t dma_remaining = __HAL_DMA_GET_COUNTER(huart2.hdmarx);
    if (dma_remaining == s_previous_dma_remaining)
    {
        return;
    }
    uint32_t current_cycle = DWT->CYCCNT;
    /* DMA advances at character completion. Subtract the next character's
     * 1042us transmission time before comparing the silent interval to t1.5. */
    if (s_attempt_tracking && s_frame_started &&
        (uint32_t)(current_cycle - s_last_rx_cycle) > (1563U + 1042U) * s_cycles_per_us)
    {
        s_gap_error = true;
    }
    s_previous_dma_remaining = dma_remaining;
    s_last_rx_cycle = current_cycle;
    s_frame_started = true;
}
