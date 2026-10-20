/**
 ******************************************************************************
 * @file    board_system.c
 * @brief   F407 系统板级接口实现。
 * @details 集中封装时基、任务延时、USART1 诊断与 IWDG 寄存器动作。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：板级支持层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "board_system.h"
#include "main.h"
#include "usart.h"
#include "FreeRTOS.h"
#include "task.h"

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 读取 HAL 单调运行毫秒数。
 * @return HAL 单调运行时间，单位 ms。
 */
uint32_t board_millis(void)
{
    return HAL_GetTick();
}

/**
 * @brief 在任务上下文中按毫秒延时。
 * @param[in] delay_ms 时间长度，单位 ms；等待接口可能阻塞到该期限。
 */
void board_delay_ms(uint32_t delay_ms)
{
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

/**
 * @brief 向 USART1 输出一段诊断文本。
 * @param[in] data 输入字节缓冲区。
 * @param[in] length 本次输入内容长度，单位字节。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool board_console_write(const void *data, size_t length)
{
    return length <= UINT16_MAX &&
           HAL_UART_Transmit(&huart1, (uint8_t *)data, (uint16_t)length, 100) == HAL_OK;
}

/**
 * @brief 启动并配置 IWDG，等待硬件参数更新完成。
 * @return true 表示参数已更新；false 表示更新超时，此时硬件可能已经启动。
 * @note IWDG 启动后不能由普通代码关闭；是否刷新由监督服务决定。
 */
bool board_watchdog_start(void)
{
    DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_IWDG_STOP;
    IWDG->KR = 0xCCCC;
    IWDG->KR = 0x5555;
    IWDG->PR = 6;
    IWDG->RLR = 2047;
    uint32_t started_ms = board_millis();
    while (IWDG->SR && board_millis() - started_ms < 1000)
    {
        board_delay_ms(1);
    }
    return IWDG->SR == 0;
}

/**
 * @brief 在硬件配置空闲时刷新 IWDG。
 */
void board_watchdog_feed(void)
{
    if (!IWDG->SR)
    {
        IWDG->KR = 0xAAAA;
    }
}
