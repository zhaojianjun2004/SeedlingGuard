/**
 ******************************************************************************
 * @file    supervisor_service.c
 * @brief   任务健康监督服务实现。
 * @details 监督器根据阶段期限决定是否喂狗，外部仪表或网络离线不等同于任务停滞。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：业务服务层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "field_internal.h"
#include "board_system.h"

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 向有界诊断行追加文本。
 */
static void append_text(char *line, unsigned *used, const char *text)
{
    while (*text && *used < 158)
    {
        line[(*used)++] = *text++;
    }
}

/**
 * @brief 向诊断行追加十进制无符号整数，避免 printf 栈开销。
 */
static void append_number(char *line, unsigned *used, uint32_t value)
{
    char digits[10];
    unsigned n = 0;
    do
    {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (n && *used < 158)
    {
        line[(*used)++] = digits[--n];
    }
}

/**
 * @brief 输出各从站错误分类和存储/CAN 诊断计数。
 */
static void print_diagnostics(void)
{
    static char line[160];
    uint32_t counters[6];
    static const char *labels[] = {
        " ok=", " timeout=", " crc=", " exception=", " protocol=", " tx="};
    for (unsigned i = 0; i < NURSERY_DEVICE_COUNT; i++)
    {
        unsigned n = 0;
        field_modbus_counters(i, counters);
        append_text(line, &n, "mb");
        append_number(line, &n, i + 1);
        for (unsigned j = 0; j < 6; j++)
        {
            append_text(line, &n, labels[j]);
            append_number(line, &n, counters[j]);
        }
        append_text(line, &n, "\r\n");
        (void)board_console_write(line, n);
    }
    static field_snapshot_t s;
    if (field_snapshot(&s))
    {
        uint32_t can[2];
        field_can_counters(can);
        unsigned n = 0;
        append_text(line, &n, "storage=");
        append_number(line, &n, s.storage_ready);
        append_text(line, &n, " sample_drops=");
        append_number(line, &n, s.sample_drops);
        append_text(line, &n, " log_drops=");
        append_number(line, &n, s.log_drops);
        append_text(line, &n, " can_errors=");
        append_number(line, &n, can[0]);
        append_text(line, &n, " can_rx_drops=");
        append_number(line, &n, can[1]);
        append_text(line, &n, "\r\n");
        (void)board_console_write(line, n);
    }
}

/**
 * @brief 按任务健康期限统一门控 IWDG 并定期输出诊断。
 * @note 任务入口，只在任务上下文运行；正常情况下不返回。
 */
void supervisor_service_run(void)
{
    /* LSI nominal 32kHz: /256, reload 2047 => nominal 16.384s; LSI has tolerance. */
    vTaskDelay(pdMS_TO_TICKS(10000));
    bool watchdog_ready = board_watchdog_start();
    bool previous = false;
    uint32_t last_diagnostic = field_now();
    for (;;)
    {
        bool healthy = field_health_ok();
        if (healthy && watchdog_ready)
        {
            board_watchdog_feed();
        }
        if (healthy != previous)
        {
            const char *text = healthy ? "nursery tasks healthy\r\n"
                                       : "nursery task deadline missed; watchdog gated\r\n";
            unsigned n = 0;
            while (text[n])
            {
                ++n;
            }
            (void)board_console_write(text, n);
            previous = healthy;
        }
        if (field_now() - last_diagnostic >= 10000)
        {
            print_diagnostics();
            last_diagnostic = field_now();
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
