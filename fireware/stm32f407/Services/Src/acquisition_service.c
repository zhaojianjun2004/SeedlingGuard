/**
 ******************************************************************************
 * @file    acquisition_service.c
 * @brief   Modbus 六设备采集调度实现。
 * @details 采集任务独占事务，使用有界重试和离线探测，结果带配置版本投递到现场任务。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：业务服务层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "field_internal.h"
#include "modbus_profile.h"
#include "modbus_rtu.h"
#include "rs485_timing.h"
#include "rs485_port.h"
#include <string.h>

/*--------------------------- 模块配置、类型与状态 ---------------------------*/

static uint32_t s_last_rx_ms;

static struct
{
    uint32_t due, failures, successes, ok, timeout, crc, exception, protocol, tx;
    bool offline;
} s_devices[NURSERY_DEVICE_COUNT];

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 丢弃上笔事务残留字节并更新最后接收时间。
 */
static void modbus_discard_pending_bytes(void)
{
    if (rs485_port_discard())
    {
        s_last_rx_ms = field_now();
    }
}

/**
 * @brief 有界等待 RS485 达到请求前静默时间。
 */
static bool modbus_wait_bus_quiet(void)
{
    uint32_t start = field_now();
    do
    {
        modbus_discard_pending_bytes();
        if (field_now() - s_last_rx_ms >= 5)
        {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    } while (field_now() - start < 200);
    return false;
}

/**
 * @brief 执行一次请求与完整响应等待，不延长总事务截止时间。
 */
static modbus_rtu_result_t modbus_execute_attempt(unsigned device_index, uint16_t *out_registers)
{
    /* 开始新请求前丢弃迟到字节，并有界等待总线静默。 */
    if (!modbus_wait_bus_quiet())
    {
        return MODBUS_RTU_BAD_LENGTH;
    }
    uint8_t request[8], response[37];
    unsigned received_length = 0, expected_length = 9;
    modbus_rtu_build_read03((uint8_t)(device_index + 1),
                            device_index % 2 ? NURSERY_LIGHT_REGISTER : NURSERY_TH_REGISTER, 2,
                            request);
    /* 从本次尝试重新计时；端口负责 DE 切向与等待 TC。 */
    rs485_timing_begin();
    bool request_sent = rs485_port_send(request, sizeof(request));
    uint32_t response_started_ms = field_now();
    if (!request_sent)
    {
        return MODBUS_RTU_TX_ERROR;
    }
    /* 只接受本次截止期限内的完整响应，部分帧不能延长等待。 */
    for (;;)
    {
        if (rs485_timing_gap_error())
        {
            return MODBUS_RTU_BAD_LENGTH;
        }
        uint8_t received_byte;
        while (rs485_port_read(&received_byte, 1) == 1)
        {
            if (received_length >= sizeof(response))
            {
                return MODBUS_RTU_BAD_LENGTH;
            }
            response[received_length++] = received_byte;
            s_last_rx_ms = field_now();
            if (received_length == 2 && response[1] == 0x83)
            {
                expected_length = 5;
            }
            if (received_length == 3 && response[1] == 3)
            {
                if (response[2] != 4)
                {
                    return MODBUS_RTU_BAD_LENGTH;
                }
                expected_length = 9;
            }
        }
        if (received_length >= expected_length && rs485_timing_frame_quiet())
        {
            return modbus_rtu_parse_read03((uint8_t)(device_index + 1), 2, response,
                                           received_length, out_registers);
        }
        if (field_now() - response_started_ms >= NURSERY_MODBUS_TIMEOUT_MS)
        {
            return MODBUS_RTU_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/**
 * @brief 执行一次尝试，并在所有退出路径关闭时序上下文。
 */
static modbus_rtu_result_t modbus_poll_attempt(unsigned device_index, uint16_t *out_registers)
{
    modbus_rtu_result_t result = modbus_execute_attempt(device_index, out_registers);
    rs485_timing_end();
    return result;
}

/**
 * @brief 轮询到期设备，执行有界重试并更新离线探测调度。
 * @note 任务入口，只在任务上下文运行；正常情况下不返回。
 */
void acquisition_service_run(void)
{
    extern volatile bool g_startup_ready;
    while (!g_startup_ready)
    {
        field_health(HEALTH_MODBUS, 10000);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    g_modbus_task = xTaskGetCurrentTaskHandle();
    (void)rs485_port_open();
    uint32_t active_config_version = 0;
    for (;;)
    {
        field_health(HEALTH_MODBUS, 1500);
        if (!rs485_port_ready())
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        field_snapshot_t snapshot;
        if (!field_snapshot(&snapshot))
        {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (active_config_version != snapshot.config.version)
        {
            for (unsigned device_index = 0; device_index < NURSERY_DEVICE_COUNT; device_index++)
            {
                s_devices[device_index].due = field_now();
            }
            active_config_version = snapshot.config.version;
        }
        if (!snapshot.config.enabled)
        {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }
        bool polled = false;
        for (unsigned device_index = 0; device_index < NURSERY_DEVICE_COUNT; device_index++)
        {
            if ((int32_t)(field_now() - s_devices[device_index].due) < 0)
            {
                continue;
            }
            uint16_t register_values[2] = {0};
            modbus_rtu_result_t poll_result = MODBUS_RTU_BAD_LENGTH;
            bool was_offline = s_devices[device_index].offline;
            for (unsigned attempt_index = 0; attempt_index < (was_offline ? 1U : 2U);
                 attempt_index++)
            {
                poll_result = modbus_poll_attempt(device_index, register_values);
                switch (poll_result)
                {
                    case MODBUS_RTU_OK:
                        ++s_devices[device_index].ok;
                        break;
                    case MODBUS_RTU_TIMEOUT:
                        ++s_devices[device_index].timeout;
                        break;
                    case MODBUS_RTU_BAD_CRC:
                        ++s_devices[device_index].crc;
                        break;
                    case MODBUS_RTU_EXCEPTION:
                        ++s_devices[device_index].exception;
                        break;
                    case MODBUS_RTU_TX_ERROR:
                        ++s_devices[device_index].tx;
                        break;
                    default:
                        ++s_devices[device_index].protocol;
                        break;
                }
                if (poll_result == MODBUS_RTU_OK || poll_result == MODBUS_RTU_EXCEPTION)
                {
                    break;
                }
            }
            bool poll_ok = poll_result == MODBUS_RTU_OK;
            if (poll_ok)
            {
                s_devices[device_index].failures = 0;
                if (++s_devices[device_index].successes >= 2)
                {
                    s_devices[device_index].offline = false;
                }
            }
            else
            {
                s_devices[device_index].successes = 0;
                if (poll_result == MODBUS_RTU_EXCEPTION)
                {
                    /* A CRC-valid exception proves the instrument can communicate. */
                    s_devices[device_index].offline = false;
                    s_devices[device_index].failures = 0;
                }
                else if (++s_devices[device_index].failures >= 3)
                {
                    s_devices[device_index].offline = true;
                }
            }
            sample_event_t event = {.device = (uint8_t)device_index,
                                    .count = (uint8_t)(device_index % 2 ? 1 : 2),
                                    .sampled_ms = field_now(),
                                    .config_version = active_config_version,
                                    .quality = s_devices[device_index].offline ||
                                                       poll_result == MODBUS_RTU_EXCEPTION
                                                   ? GW_QUALITY_FAULT
                                               : poll_ok ? GW_QUALITY_VALID
                                                         : GW_QUALITY_STALE};
            event.device_state = s_devices[device_index].offline       ? 3
                                 : poll_ok                             ? 1
                                 : poll_result == MODBUS_RTU_EXCEPTION ? 4
                                                                       : 2;
            if (device_index % 2)
            {
                event.values[0] =
                    (int32_t)((((uint32_t)register_values[0] << 16) | register_values[1]) /
                              NURSERY_LIGHT_DIVISOR);
            }
            else
            {
                event.values[0] = (int16_t)register_values[0];
                event.values[1] = register_values[1];
            }
            if (xQueueSend(g_samples, &event, 0) != pdTRUE)
            {
                taskENTER_CRITICAL();
                ++g_field.sample_drops;
                taskEXIT_CRITICAL();
            }
            uint32_t next_due_ms = s_devices[device_index].due + snapshot.config.period_ms;
            s_devices[device_index].due = s_devices[device_index].offline ? field_now() + 10000
                                          : was_offline ? field_now() + snapshot.config.period_ms
                                          : (int32_t)(field_now() - next_due_ms) >= 0
                                              ? field_now() + snapshot.config.period_ms
                                              : next_due_ms;
            field_health(HEALTH_MODBUS, 1500);
            polled = true;
            break;
        }
        if (!polled)
        {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        }
    }
}

/**
 * @brief 复制指定设备的每次尝试分类计数。
 * @param[in] device_index 从 0 开始的仪表下标，范围 0～5。
 * @param[out] out_counters 容量至少为 6，依次接收成功、超时、CRC、异常、协议和发送错误计数。
 */
void field_modbus_counters(unsigned device_index, uint32_t out_counters[6])
{
    if (device_index >= NURSERY_DEVICE_COUNT)
    {
        return;
    }
    taskENTER_CRITICAL();
    out_counters[0] = s_devices[device_index].ok;
    out_counters[1] = s_devices[device_index].timeout;
    out_counters[2] = s_devices[device_index].crc;
    out_counters[3] = s_devices[device_index].exception;
    out_counters[4] = s_devices[device_index].protocol;
    out_counters[5] = s_devices[device_index].tx;
    taskEXIT_CRITICAL();
}
