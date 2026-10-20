/**
 ******************************************************************************
 * @file    ota_console_service.c
 * @brief   串口运维入口实现。
 * @details 在控制台任务解析维护命令；配置回显以现场回执为准。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：应用层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "ota_console_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"

/*--------------------------- 模块配置、类型与状态 ---------------------------*/

static const char *TAG = "ota_console";

struct ota_console_service
{
    ota_console_service_config_t config;
    esp_console_repl_t *repl;
    bool started;
};

static ota_console_service_handle_t s_command_context = NULL;

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 返回现场数据质量的可读名称。
 */
static const char *field_quality_name(uint8_t quality)
{
    switch (quality)
    {
        case GW_QUALITY_VALID:
            return "VALID";
        case GW_QUALITY_STALE:
            return "STALE";
        case GW_QUALITY_FAULT:
            return "FAULT";
        default:
            return "INVALID";
    }
}

/**
 * @brief 在维护串口输出现场快照和样本年龄。
 */
static void field_console_print_snapshot(gateway_link_handle_t link)
{
    gateway_link_snapshot_t snapshot;
    const esp_err_t error = gateway_link_get_snapshot(link, &snapshot);
    if (error != ESP_OK)
    {
        printf("无法读取 F407 状态：%s\n", esp_err_to_name(error));
        return;
    }
    printf("F407 online=%u boot=%u session=%lu collect=%u period=%lu ms\n",
           snapshot.field_online ? 1U : 0U, (unsigned)snapshot.boot_id,
           (unsigned long)snapshot.session_generation, snapshot.collect_enabled ? 1U : 0U,
           (unsigned long)snapshot.sample_period_ms);
    printf("UART rx_bytes=%lu frames=%lu crc=%lu framing=%lu\n",
           (unsigned long)snapshot.uart_rx_bytes, (unsigned long)snapshot.received_frames,
           (unsigned long)snapshot.crc_errors, (unsigned long)snapshot.framing_errors);
    static const char *names[9] = {"zone1_temperature", "zone1_humidity", "zone1_light",
                                   "zone2_temperature", "zone2_humidity", "zone2_light",
                                   "zone3_temperature", "zone3_humidity", "zone3_light"};
    const int64_t now = esp_timer_get_time();
    for (unsigned i = 0U; i < NURSERY_POINT_COUNT; ++i)
    {
        const gateway_point_snapshot_t *point = &snapshot.points[i];
        if (!point->received)
        {
            printf("%s: INVALID (no frame)\n", names[i]);
            continue;
        }
        uint32_t age = gateway_point_age_ms(point, now);
        const long long age_ms = age == UINT32_MAX ? -1LL : (long long)age;
        printf("%s: value=%ld quality=%s age_ms=%lld seq=%u\n", names[i], (long)point->scaled_value,
               field_quality_name(point->quality), age_ms, (unsigned)point->sequence);
    }
}

/**
 * @brief 提交一条现场命令并回显实际执行结果。
 */
static int field_console_send(gateway_link_handle_t link, gw_command_op_t op, uint32_t value)
{
    gw_result_t result = {0};
    const esp_err_t error =
        gateway_link_command(link, op, value, ((esp_random() & 0x1ffffffeU) + 1U), 5000U, &result);
    if (error == ESP_OK || error == ESP_ERR_INVALID_RESPONSE)
    {
        printf("F407 result=%u collect=%u period=%lu ms\n", (unsigned)result.result,
               result.collect_enabled ? 1U : 0U, (unsigned long)result.sample_period_ms);
        return error == ESP_OK ? 0 : 1;
    }
    printf("F407 命令未确认：%s\n", esp_err_to_name(error));
    return 1;
}

/**
 * @brief 解析现场状态、启停和采样周期维护命令。
 */
static int field_console_command(int argc, char **argv)
{
    gateway_link_handle_t link =
        s_command_context != NULL ? s_command_context->config.gateway_link : NULL;
    if (link == NULL)
    {
        printf("F407 板间链路未启用\n");
        return 1;
    }
    if (argc == 2 && strcmp(argv[1], "status") == 0)
    {
        field_console_print_snapshot(link);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "query") == 0)
    {
        return field_console_send(link, GW_CMD_QUERY_STATUS, 0U);
    }
    if (argc == 2 && strcmp(argv[1], "start") == 0)
    {
        return field_console_send(link, GW_CMD_SET_ENABLE, 1U);
    }
    if (argc == 2 && strcmp(argv[1], "stop") == 0)
    {
        return field_console_send(link, GW_CMD_SET_ENABLE, 0U);
    }
    if (argc == 3 && strcmp(argv[1], "period") == 0)
    {
        char *end = NULL;
        errno = 0;
        const unsigned long period = strtoul(argv[2], &end, 10);
        if (errno == 0 && end != argv[2] && *end == '\0' && period >= NURSERY_PERIOD_MIN_MS &&
            period <= 600000UL)
        {
            return field_console_send(link, GW_CMD_SET_PERIOD, (uint32_t)period);
        }
        printf("采集周期须为 5000..600000 ms\n");
        return 1;
    }
    printf("用法：field status|query|start|stop|period <ms>\n");
    return 1;
}

/**
 * @brief 解析 Wi-Fi 状态与凭据复位维护命令。
 */
static int wifi_console_command(int argc, char **argv)
{
    wifi_manager_handle_t wifi = s_command_context != NULL ? s_command_context->config.wifi : NULL;
    if (wifi == NULL)
    {
        printf("Wi-Fi 服务未初始化\n");
        return 1;
    }
    if (argc == 2 && strcmp(argv[1], "status") == 0)
    {
        wifi_manager_status_t status;
        const esp_err_t error = wifi_manager_get_status(wifi, &status);
        if (error != ESP_OK)
        {
            printf("无法读取 Wi-Fi 状态：%s\n", esp_err_to_name(error));
            return 1;
        }
        printf("Wi-Fi=%s, IP=%s, AP=%u, 重连=%lu, 最近原因=%u\n",
               wifi_manager_state_to_string(status.state),
               status.has_ipv4 ? status.ipv4_address : "none", status.ap_active ? 1U : 0U,
               (unsigned long)status.reconnect_attempts, (unsigned)status.last_disconnect_reason);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "reset") == 0)
    {
        const esp_err_t error = wifi_manager_clear_credentials(wifi);
        if (error != ESP_OK)
        {
            printf("清除 Wi-Fi 凭据失败：%s\n", esp_err_to_name(error));
            return 1;
        }
        printf("Wi-Fi 凭据已清除，配网 AP 正在启动；请查看串口中的 AP 名称和密码。\n");
        return 0;
    }
    printf("用法：wifi status|reset\n");
    return 1;
}

/**
 * @brief 输出升级事务和启动确认状态。
 */
static void ota_console_print_status(firmware_update_service_handle_t update_service)
{
    firmware_update_status_t status;
    const esp_err_t result = firmware_update_service_get_status(update_service, &status);
    if (result != ESP_OK)
    {
        printf("无法读取 OTA 状态：%s\n", esp_err_to_name(result));
        return;
    }

    printf("OTA 状态=%s, 错误=%s, 进度=%u%% (%u/%u 字节)",
           firmware_update_state_to_string(status.state), esp_err_to_name(status.last_error),
           (unsigned int)status.progress_percent, (unsigned int)status.bytes_received,
           (unsigned int)status.total_bytes);
    if (status.image_version[0] != '\0')
    {
        printf(", 版本=%s, 发布序号=%u", status.image_version,
               (unsigned int)status.release_sequence);
    }
    if (status.target_partition[0] != '\0')
    {
        printf(", 目标分区=%s", status.target_partition);
    }
    printf("\n");
}

/**
 * @brief 解析升级提交与进度查询命令。
 */
static int ota_console_command(int argc, char **argv)
{
    if ((s_command_context == NULL) || (s_command_context->config.update_service == NULL))
    {
        printf("OTA 命令服务尚未初始化\n");
        return 1;
    }

    if ((argc == 2) && (strcmp(argv[1], "status") == 0))
    {
        ota_console_print_status(s_command_context->config.update_service);
        return 0;
    }

    if ((argc == 3) && (strcmp(argv[1], "start") == 0))
    {
        const esp_err_t result =
            firmware_update_service_request(s_command_context->config.update_service, argv[2]);
        if (result != ESP_OK)
        {
            printf("OTA 请求被拒绝：%s\n", esp_err_to_name(result));
            return 1;
        }
        printf("OTA 请求已进入队列，可使用 'ota status' 查看进度\n");
        return 0;
    }

    printf("用法：\n");
    printf("  ota status\n");
    printf("  ota start http://<电脑局域网IP>:<端口>/manifest.json\n");
    return 1;
}

/**
 * @brief 创建串口运维入口实例并准备所需资源。
 * @param[in] config 模块配置及其取值约束。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t ota_console_service_create(const ota_console_service_config_t *config,
                                     ota_console_service_handle_t *out_handle)
{
    if ((config == NULL) || (config->update_service == NULL) || (config->prompt == NULL) ||
        (config->prompt[0] == '\0') || (out_handle == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_command_context != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    *out_handle = NULL;
    ota_console_service_handle_t handle = calloc(1U, sizeof(*handle));
    if (handle == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    handle->config = *config;

    const esp_console_cmd_t command = {
        .command = "ota",
        .help = "通过 Manifest 手动触发 HTTP OTA 或读取升级状态",
        .hint = "<status|start URL>",
        .func = ota_console_command,
        .argtable = NULL,
    };
    esp_err_t result = esp_console_cmd_register(&command);
    if (result != ESP_OK)
    {
        free(handle);
        return result;
    }
    if (config->gateway_link != NULL)
    {
        const esp_console_cmd_t field_command = {
            .command = "field",
            .help = "读取 F407 状态并经板间链路发送采集控制命令",
            .hint = "<status|query|start|stop|period ms>",
            .func = field_console_command,
            .argtable = NULL,
        };
        result = esp_console_cmd_register(&field_command);
        if (result != ESP_OK)
        {
            free(handle);
            return result;
        }
    }
    if (config->wifi != NULL)
    {
        const esp_console_cmd_t wifi_command = {
            .command = "wifi",
            .help = "读取 Wi-Fi 状态或清除凭据进入 AP 配网",
            .hint = "<status|reset>",
            .func = wifi_console_command,
            .argtable = NULL,
        };
        result = esp_console_cmd_register(&wifi_command);
        if (result != ESP_OK)
        {
            free(handle);
            return result;
        }
    }

    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = config->prompt;
    repl_config.max_cmdline_length = FIRMWARE_UPDATE_URI_MAX_LENGTH + 32U;
    repl_config.history_save_path = NULL;

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t device_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    result = esp_console_new_repl_uart(&device_config, &repl_config, &handle->repl);
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t device_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    result = esp_console_new_repl_usb_serial_jtag(&device_config, &repl_config, &handle->repl);
#else
    result = ESP_ERR_NOT_SUPPORTED;
#endif
    if (result != ESP_OK)
    {
        free(handle);
        return result;
    }

    s_command_context = handle;
    *out_handle = handle;
    return ESP_OK;
}

/**
 * @brief 启动串口运维入口的运行入口。
 * @param[in] handle 已创建的模块实例句柄。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t ota_console_service_start(ota_console_service_handle_t handle)
{
    if (handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->started)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t result = esp_console_start_repl(handle->repl);
    if (result == ESP_OK)
    {
        handle->started = true;
        ESP_LOGI(TAG, "Console started; use 'ota status' or 'field status'");
    }
    return result;
}
