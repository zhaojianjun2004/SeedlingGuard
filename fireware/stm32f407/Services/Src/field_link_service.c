/**
 ******************************************************************************
 * @file    field_link_service.c
 * @brief   现场消息路由服务实现。
 * @details LinkTask 串行发送 UART/CAN 数据；协议校验与硬件收发分别由下层模块负责。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：业务服务层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "field_internal.h"
#include "field_link_port.h"
#include "can_diag_protocol.h"

/*--------------------------- 模块配置、类型与状态 ---------------------------*/

static gw_parser_t s_uart_parser;
static uint16_t s_tx_sequence;

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 按目的链路发送一条已组装的业务帧。
 */
static void field_link_transmit(gw_frame_t *frame, uint8_t destination)
{
    if (frame->type != GW_MSG_RESULT)
    {
        frame->seq = ++s_tx_sequence;
    }
    if (destination & FIELD_UART)
    {
        uint8_t data[GW_FRAME_MAX_SIZE];
        size_t encoded_length = gw_frame_encode(frame, data, sizeof(data));
        if (encoded_length)
        {
            (void)field_link_port_write(data, encoded_length);
        }
    }
    if (destination & FIELD_CAN)
    {
        uint16_t id;
        uint8_t data[8];
        if (gw_can_diag_encode(frame, &id, data))
        {
            (void)field_link_port_can_write(id, data);
        }
    }
}

/**
 * @brief 将命令投递到现场任务，队列满时返回 BUSY。
 */
static void field_link_accept_command(command_event_t *event)
{
    if (xQueueSend(g_commands, event, 0) == pdTRUE)
    {
        return;
    }
    field_snapshot_t snapshot;
    if (!field_snapshot(&snapshot))
    {
        return;
    }
    gw_result_t result = {.op = event->command.op,
                          .result = GW_RESULT_BUSY,
                          .request_id = event->command.request_id,
                          .collect_enabled = snapshot.config.enabled,
                          .sample_period_ms = snapshot.config.period_ms,
                          .config_version = snapshot.config.version};
    gw_frame_t frame = {.seq = event->seq, .session_id = snapshot.session};
    gw_result_pack(&result, &frame);
    field_link_transmit(&frame, event->source);
}

/**
 * @brief 处理串口/诊断命令并周期发送现场快照。
 * @note 任务入口，只在任务上下文运行；正常情况下不返回。
 */
void field_link_service_run(void)
{
    extern volatile bool g_startup_ready;
    while (!g_startup_ready)
    {
        field_health(HEALTH_LINK, 10000);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    configASSERT(field_link_port_open(GW_CAN_COMMAND_ID));
    gw_parser_init(&s_uart_parser);
    uint32_t last_status = field_now() - 1000;
    for (;;)
    {
        field_health(HEALTH_LINK, 1500);
        for (unsigned batch = 0; batch < 4; batch++)
        {
            uint8_t bytes[128];
            bool receiver_restarted = false;
            size_t received_length =
                field_link_port_read(bytes, sizeof(bytes), &receiver_restarted);
            if (receiver_restarted)
            {
                gw_parser_init(&s_uart_parser);
            }
            for (size_t index = 0; index < received_length; index++)
            {
                gw_frame_t frame;
                if (gw_parser_push(&s_uart_parser, bytes[index], &frame) &&
                    frame.session_id == g_field.session)
                {
                    command_event_t command_event = {
                        .seq = frame.seq, .session = frame.session_id, .source = FIELD_UART};
                    if (gw_command_unpack(&frame, &command_event.command))
                    {
                        field_link_accept_command(&command_event);
                    }
                }
            }
            if (!received_length)
            {
                break;
            }
        }
        board_can_frame_t input;
        for (unsigned index = 0; index < 4 && field_link_port_can_read(&input); index++)
        {
            command_event_t command_event = {.session = g_field.session, .source = FIELD_CAN};
            if (gw_can_diag_decode(input.id, input.data, &command_event.command,
                                   &command_event.seq))
            {
                field_link_accept_command(&command_event);
            }
        }
        link_output_t output;
        for (unsigned index = 0; index < 12 && xQueueReceive(g_output, &output, 0) == pdTRUE;
             index++)
        {
            field_link_transmit(&output.frame, output.destination);
        }
        if (field_now() - last_status >= 1000)
        {
            field_snapshot_t snapshot;
            if (field_snapshot(&snapshot))
            {
                gw_status_t status = {.boot_id = snapshot.session,
                                      .collect_enabled = snapshot.config.enabled,
                                      .sample_period_ms = snapshot.config.period_ms,
                                      .config_version = snapshot.config.version,
                                      .storage_ready = snapshot.storage_ready,
                                      .sample_drops = snapshot.sample_drops,
                                      .log_drops = snapshot.log_drops,
                                      .latest_event_id = snapshot.event_id,
                                      .log_pending = snapshot.log_pending};
                gw_frame_t frame = {.session_id = snapshot.session};
                gw_status_pack(&status, &frame);
                field_link_transmit(&frame, FIELD_UART | FIELD_CAN);
                field_publish_points();
            }
            last_status = field_now();
        }
        field_link_port_poll();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/**
 * @brief 读取 CAN 端口错误与接收溢出计数。
 * @param[out] out 容量至少为 2，依次接收 CAN 错误数和接收队列溢出数。
 */
void field_can_counters(uint32_t out[2])
{
    field_link_port_counters(out);
}
