/**
 * @file ota_console_service.c
 * @brief OTA 串口命令解析与状态输出。
 */

#include "ota_console_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"

static const char *TAG = "ota_console";

struct ota_console_service {
    ota_console_service_config_t config;
    esp_console_repl_t *repl;
    bool started;
};

static ota_console_service_handle_t s_command_context = NULL;

static void ota_console_print_status(
    firmware_update_service_handle_t update_service)
{
    firmware_update_status_t status;
    const esp_err_t result = firmware_update_service_get_status(
        update_service,
        &status);
    if (result != ESP_OK) {
        printf("无法读取 OTA 状态：%s\n", esp_err_to_name(result));
        return;
    }

    printf("OTA 状态=%s, 错误=%s, 进度=%u%% (%u/%u 字节)",
           firmware_update_state_to_string(status.state),
           esp_err_to_name(status.last_error),
           (unsigned int)status.progress_percent,
           (unsigned int)status.bytes_received,
           (unsigned int)status.total_bytes);
    if (status.image_version[0] != '\0') {
        printf(", 版本=%s, 发布序号=%u",
               status.image_version,
               (unsigned int)status.release_sequence);
    }
    if (status.target_partition[0] != '\0') {
        printf(", 目标分区=%s", status.target_partition);
    }
    printf("\n");
}

static int ota_console_command(int argc, char **argv)
{
    if ((s_command_context == NULL) ||
        (s_command_context->config.update_service == NULL)) {
        printf("OTA 命令服务尚未初始化\n");
        return 1;
    }

    if ((argc == 2) && (strcmp(argv[1], "status") == 0)) {
        ota_console_print_status(
            s_command_context->config.update_service);
        return 0;
    }

    if ((argc == 3) && (strcmp(argv[1], "start") == 0)) {
        const esp_err_t result = firmware_update_service_request(
            s_command_context->config.update_service,
            argv[2]);
        if (result != ESP_OK) {
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

esp_err_t ota_console_service_create(
    const ota_console_service_config_t *config,
    ota_console_service_handle_t *out_handle)
{
    if ((config == NULL) || (config->update_service == NULL) ||
        (config->prompt == NULL) || (config->prompt[0] == '\0') ||
        (out_handle == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_command_context != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    *out_handle = NULL;
    ota_console_service_handle_t handle = calloc(1U, sizeof(*handle));
    if (handle == NULL) {
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
    if (result != ESP_OK) {
        free(handle);
        return result;
    }

    esp_console_repl_config_t repl_config =
        ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = config->prompt;
    repl_config.max_cmdline_length =
        FIRMWARE_UPDATE_URI_MAX_LENGTH + 32U;
    repl_config.history_save_path = NULL;

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || \
    defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t device_config =
        ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    result = esp_console_new_repl_uart(&device_config,
                                       &repl_config,
                                       &handle->repl);
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t device_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    result = esp_console_new_repl_usb_serial_jtag(&device_config,
                                                  &repl_config,
                                                  &handle->repl);
#else
    result = ESP_ERR_NOT_SUPPORTED;
#endif
    if (result != ESP_OK) {
        free(handle);
        return result;
    }

    s_command_context = handle;
    *out_handle = handle;
    return ESP_OK;
}

esp_err_t ota_console_service_start(ota_console_service_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->started) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t result = esp_console_start_repl(handle->repl);
    if (result == ESP_OK) {
        handle->started = true;
        ESP_LOGI(TAG, "OTA console started; use 'ota status' or 'ota start <URL>'");
    }
    return result;
}
