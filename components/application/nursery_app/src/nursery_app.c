/**
 ******************************************************************************
 * @file    nursery_app.c
 * @brief   育苗室网关应用装配实现。
 * @details 在应用启动阶段创建资源、注入平台接口并启动服务，外部仪表掉线不作为启动自检失败条件。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：应用层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "app_config_service.h"
#include "boot_health_service.h"
#include "config_store.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "firmware_update_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gateway_http.h"
#include "gateway_link.h"
#include "uart_port.h"
#include "nursery_app.h"
#include "gateway_outbox.h"
#include "mqtt_service.h"
#include "nvs_flash.h"
#include "ota_button_service.h"
#include "ota_console_service.h"
#include "sdkconfig.h"
#include "update_platform_esp32.h"
#include "update_integrity_sha256.h"
#include "update_manifest_json.h"
#include "update_transport_http.h"
#include "wifi_manager.h"

/*--------------------------- 模块配置、类型与状态 ---------------------------*/

#define APP_BOOT_HEALTH_TASK_STACK_SIZE     (3072U)
#define APP_BOOT_HEALTH_TASK_PRIORITY       (3U)
#define APP_FIRMWARE_UPDATE_TASK_STACK_SIZE (6144U)
#define APP_FIRMWARE_UPDATE_TASK_PRIORITY   (4U)
#define APP_OTA_BUTTON_TASK_STACK_SIZE      (2048U)
#define APP_OTA_BUTTON_TASK_PRIORITY        (3U)
#define APP_OTA_BUTTON_POLL_INTERVAL_MS     (50U)
#define APP_GATEWAY_SYNC_TASK_STACK_SIZE    (3072U)
#define APP_GATEWAY_SYNC_TASK_PRIORITY      (4U)
#define APP_CONFIG_NVS_NAMESPACE            "env_config"
#define APP_CONFIG_NVS_KEY                  "runtime"

static const char *TAG = "app_main"; // TAG 用于 ESP_LOGx 日志前缀

static wifi_manager_handle_t s_wifi_manager = NULL;
static mqtt_service_handle_t s_mqtt_service = NULL;
static config_store_handle_t s_config_store = NULL;
static app_config_service_handle_t s_config_service = NULL;
static boot_health_service_handle_t s_boot_health_service = NULL;
static update_transport_http_handle_t s_update_transport = NULL;
static update_manifest_json_handle_t s_update_manifest_parser = NULL;
static update_integrity_sha256_handle_t s_update_integrity = NULL;
static update_platform_esp32_handle_t s_update_platform = NULL;
static firmware_update_service_handle_t s_firmware_update_service = NULL;
static gateway_link_handle_t s_gateway_link = NULL;
static nursery_uart_handle_t s_gateway_uart = NULL;
static gateway_outbox_handle_t s_gateway_outbox = NULL;
#if CONFIG_APP_GATEWAY_HTTP_ENABLED
static gateway_http_handle_t s_gateway_http = NULL;
#endif
#if CONFIG_APP_OTA_CONSOLE_ENABLED
static ota_console_service_handle_t s_ota_console_service = NULL;
#endif
static ota_button_service_handle_t s_ota_button_service = NULL;

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 初始化网关 NVS，处理分区不兼容或无可用页的启动情况。
 */
static void app_initialize_nvs(void)
{
    esp_err_t result = nvs_flash_init();
    if ((result == ESP_ERR_NVS_NO_FREE_PAGES) || (result == ESP_ERR_NVS_NEW_VERSION_FOUND))
    {
        /** 只有在 NVS 分区不兼容或空间耗尽时才擦除。正常启动会保留所有将来的用户配置 */
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
}

/**
 * @brief 按产品配置启动 Wi-Fi 配网与重连服务。
 *
 * @details 连接过程由 wifi_manager 异步执行，因此这里不会等待路由器响应。
 */
static void app_start_wifi_if_configured(void)
{
    if (CONFIG_APP_WIFI_SSID[0] == '\0')
    {
        ESP_LOGI(TAG, "No compiled Wi-Fi SSID; NVS credentials or AP provisioning will be used");
    }

    const wifi_manager_config_t wifi_config = {
        .ssid = CONFIG_APP_WIFI_SSID,
        .password = CONFIG_APP_WIFI_PASSWORD,
        .reconnect_initial_delay_ms = CONFIG_APP_WIFI_RECONNECT_INITIAL_DELAY_MS,
        .reconnect_max_delay_ms = CONFIG_APP_WIFI_RECONNECT_MAX_DELAY_MS,
    };
    ESP_ERROR_CHECK(wifi_manager_create(&wifi_config, &s_wifi_manager));
    ESP_ERROR_CHECK(wifi_manager_start(s_wifi_manager));
}

#if CONFIG_APP_GATEWAY_LINK_ENABLED
/**
 * @brief 在现场会话变化后查询实际状态并请求遗漏事件。
 * @note 任务入口，只在任务上下文运行；正常情况下不返回。
 */
static void app_gateway_sync_task(void *context)
{
    gateway_link_handle_t link = (gateway_link_handle_t)context;
    uint32_t confirmed_generation = 0U;
    for (;;)
    {
        gateway_link_snapshot_t snapshot;
        if (gateway_link_get_snapshot(link, &snapshot) == ESP_OK && snapshot.field_online &&
            snapshot.session_generation != 0U &&
            snapshot.session_generation != confirmed_generation)
        {
            gw_result_t result;
            const esp_err_t error = gateway_link_command(
                link, GW_CMD_QUERY_STATUS, 0U, ((esp_random() & 0x1ffffffeU) + 1U), 1500U, &result);
            if (error == ESP_OK)
            {
                confirmed_generation = snapshot.session_generation;
                gw_result_t replay_result;
                (void)gateway_link_command(
                    link, GW_CMD_QUERY_EVENTS, gateway_outbox_event_cursor(s_gateway_outbox),
                    ((esp_random() & 0x1ffffffeU) + 1U), 5000, &replay_result);
                ESP_LOGI(TAG, "F407 command path confirmed: collect=%u period=%lu ms",
                         result.collect_enabled ? 1U : 0U, (unsigned long)result.sample_period_ms);
            }
            else
            {
                ESP_LOGW(TAG, "F407 status query failed: %s", esp_err_to_name(error));
                vTaskDelay(pdMS_TO_TICKS(3000U));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500U));
    }
}
#endif

/**
 * @brief 创建 UART 字节端口并注入现场链路服务。
 */
static void app_start_gateway_link(void)
{
#if CONFIG_APP_GATEWAY_LINK_ENABLED
    const nursery_uart_config_t uart_config = {
        .port_number = 1,
        .tx_gpio = CONFIG_APP_GATEWAY_UART_TX_GPIO,
        .rx_gpio = CONFIG_APP_GATEWAY_UART_RX_GPIO,
        .baud_rate = CONFIG_APP_GATEWAY_UART_BAUDRATE,
    };
    ESP_ERROR_CHECK(nursery_uart_open(&uart_config, &s_gateway_uart));
    const gateway_link_config_t config = {
        .uart = s_gateway_uart,
        .task_stack_size = 4096U,
        .task_priority = 6U,
    };
    ESP_ERROR_CHECK(gateway_link_create(&config, &s_gateway_link));
    ESP_ERROR_CHECK(gateway_link_start(s_gateway_link));
    const BaseType_t created =
        xTaskCreate(app_gateway_sync_task, "gateway_sync", APP_GATEWAY_SYNC_TASK_STACK_SIZE,
                    s_gateway_link, APP_GATEWAY_SYNC_TASK_PRIORITY, NULL);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
#endif
}

/**
 * @brief 装配并启动局域网页面和控制 API。
 */
static void app_start_gateway_http_if_configured(void)
{
#if CONFIG_APP_GATEWAY_HTTP_ENABLED
    if (s_gateway_link == NULL || s_wifi_manager == NULL)
    {
        ESP_LOGW(TAG, "Local dashboard requires gateway link and configured Wi-Fi");
        return;
    }
    const gateway_http_config_t config = {
        .link = s_gateway_link,
        .outbox = s_gateway_outbox,
        .mqtt = s_mqtt_service,
        .wifi = s_wifi_manager,
        .control_token = CONFIG_APP_GATEWAY_HTTP_CONTROL_TOKEN,
        .port = CONFIG_APP_GATEWAY_HTTP_PORT,
    };
    const esp_err_t error = gateway_http_start(&config, &s_gateway_http);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "Local dashboard unavailable: %s", esp_err_to_name(error));
    }
#endif
}

/**
 * @brief 将现场快照转换为遥测记录，冻结采样年龄并降级过期质量。
 */
static esp_err_t app_mqtt_get_snapshot(void *context, mqtt_environment_snapshot_t *out_snapshot)
{
    if ((context == NULL) || (out_snapshot == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    gateway_link_snapshot_t field;
    const esp_err_t error = gateway_link_get_snapshot((gateway_link_handle_t)context, &field);
    if (error != ESP_OK)
    {
        return error;
    }
    const int64_t now_us = esp_timer_get_time();
    *out_snapshot = (mqtt_environment_snapshot_t){
        .capture_uptime_ms = (uint32_t)(now_us / 1000LL),
        .sample_period_ms = field.sample_period_ms,
        .field_boot_id = field.boot_id,
        .field_online = field.field_online,
        .collect_enabled = field.collect_enabled,
    };
    const uint32_t fresh_limit_ms = field.sample_period_ms * 2U + 2000U;
    for (unsigned i = 0U; i < GATEWAY_OUTBOX_POINT_COUNT; ++i)
    {
        const gateway_point_snapshot_t *point = &field.points[i];
        gateway_outbox_point_t *stored = &out_snapshot->points[i];
        stored->value_scaled = point->scaled_value;
        stored->quality = point->received ? point->quality : GW_QUALITY_INVALID;
        stored->sample_age_ms = gateway_point_age_ms(point, now_us);
        stored->alarm_state = point->alarm_state;
        if (stored->quality == GW_QUALITY_VALID &&
            (!field.field_online || stored->sample_age_ms > fresh_limit_ms))
        {
            stored->quality = GW_QUALITY_STALE;
        }
    }
    return ESP_OK;
}

/**
 * @brief 查询 MQTT 所需的 Wi-Fi IPv4 就绪状态。
 */
static bool app_mqtt_is_network_ready(void *context)
{
    wifi_manager_status_t status;
    return (wifi_manager_get_status((wifi_manager_handle_t)context, &status) == ESP_OK) &&
           status.has_ipv4;
}

/**
 * @brief 汇总网关上报配置和现场实际采集配置。
 */
static esp_err_t app_mqtt_get_runtime_config(void *context, mqtt_runtime_config_t *out_config)
{
    if ((context == NULL) || (out_config == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    app_runtime_config_t config;
    esp_err_t result = app_config_service_get((app_config_service_handle_t)context, &config);
    if (result == ESP_OK)
    {
        out_config->report_period_ms = config.report_period_ms;
        gateway_link_snapshot_t field;
        result = gateway_link_get_snapshot(s_gateway_link, &field);
        if (result == ESP_OK)
        {
            out_config->collect_enabled = field.collect_enabled;
            out_config->config_version = field.config_version;
            out_config->field_online = field.field_online;
            out_config->synchronized = field.synchronized;
            for (unsigned i = 0; i < NURSERY_POINT_COUNT; i++)
            {
                out_config->rules[i] = field.points[i].rule;
            }
            out_config->sample_period_ms =
                field.sample_period_ms >= NURSERY_PERIOD_MIN_MS ? field.sample_period_ms : 5000U;
        }
    }
    return result;
}

/**
 * @brief 转发现场配置命令，并单独保存网关上报周期。
 */
static esp_err_t app_mqtt_apply_runtime_config(void *context,
                                               const mqtt_runtime_config_update_t *mqtt_update)
{
    if ((context == NULL) || (mqtt_update == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (mqtt_update->has_collect_enabled || mqtt_update->has_sample_period)
    {
        gateway_link_snapshot_t field;
        if (gateway_link_get_snapshot(s_gateway_link, &field) != ESP_OK || !field.field_online)
        {
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (mqtt_update->has_collect_enabled)
    {
        gw_result_t result = {0};
        const esp_err_t error = gateway_link_command(
            s_gateway_link, GW_CMD_SET_ENABLE, mqtt_update->collect_enabled ? 1U : 0U,
            mqtt_update->request_id ? mqtt_update->request_id ^ 0x80000000U : esp_random(), 5000U,
            &result);
        if (error != ESP_OK)
        {
            return error;
        }
        if (result.collect_enabled != mqtt_update->collect_enabled)
        {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (mqtt_update->has_sample_period)
    {
        gw_result_t result = {0};
        const esp_err_t error = gateway_link_command(
            s_gateway_link, GW_CMD_SET_PERIOD, mqtt_update->sample_period_ms,
            mqtt_update->request_id ? mqtt_update->request_id : esp_random(), 5000U, &result);
        if (error != ESP_OK)
        {
            return error;
        }
        if (result.sample_period_ms != mqtt_update->sample_period_ms)
        {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (mqtt_update->has_threshold)
    {
        gw_result_t result;
        esp_err_t e = gateway_link_command_point(
            s_gateway_link, mqtt_update->threshold_op, (uint32_t)mqtt_update->threshold_value,
            mqtt_update->point_id,
            mqtt_update->request_id ? mqtt_update->request_id ^ 0x40000000U : esp_random(), 5000,
            &result);
        if (e != ESP_OK)
        {
            return e;
        }
    }
    if (!mqtt_update->has_report_period)
    {
        return ESP_OK;
    }
    const app_runtime_config_update_t update = {
        .has_report_period = true,
        .report_period_ms = mqtt_update->report_period_ms,
    };
    return app_config_service_update((app_config_service_handle_t)context, &update);
}

/**
 * @brief 在 Broker 已配置时创建待发队列和 MQTT 服务。
 */
static void app_start_mqtt_if_configured(void)
{
    if (CONFIG_APP_MQTT_BROKER_URI[0] == '\0')
    {
        ESP_LOGW(TAG, "MQTT is not configured; set APP_MQTT_BROKER_URI in menuconfig");
        return;
    }
    if (s_wifi_manager == NULL)
    {
        ESP_LOGW(TAG, "MQTT requires a configured Wi-Fi connection");
        return;
    }
    if (s_gateway_link == NULL)
    {
        ESP_LOGW(TAG, "F407 telemetry requires the gateway link");
        return;
    }
    const esp_err_t outbox_result = gateway_outbox_create("outbox", &s_gateway_outbox);
    if (outbox_result != ESP_OK)
    {
        ESP_LOGE(TAG, "Persistent MQTT outbox unavailable: %s", esp_err_to_name(outbox_result));
        return;
    }

    const mqtt_service_config_t mqtt_config = {
        .broker_uri = CONFIG_APP_MQTT_BROKER_URI,
        .username = CONFIG_APP_MQTT_USERNAME,
        .password = CONFIG_APP_MQTT_PASSWORD,
        .outbox = s_gateway_outbox,
        .field_link = s_gateway_link,
        .snapshot_context = s_gateway_link,
        .get_snapshot = app_mqtt_get_snapshot,
        .runtime_config_context = s_config_service,
        .get_runtime_config = app_mqtt_get_runtime_config,
        .apply_runtime_config = app_mqtt_apply_runtime_config,
        .network_context = s_wifi_manager,
        .is_network_ready = app_mqtt_is_network_ready,
    };
    ESP_ERROR_CHECK(mqtt_service_create(&mqtt_config, &s_mqtt_service));
    ESP_ERROR_CHECK(mqtt_service_start(s_mqtt_service));
}

/**
 * @brief 查询升级下载所需的网络就绪状态。
 */
static bool app_ota_is_network_ready(void *context)
{
    if (context == NULL)
    {
        return false;
    }

    wifi_manager_status_t status;
    return (wifi_manager_get_status((wifi_manager_handle_t)context, &status) == ESP_OK) &&
           status.has_ipv4;
}

/**
 * @brief 执行升级成功后的芯片重启。
 */
static void app_restart_after_update(void *context)
{
    (void)context;
    esp_restart();
}

/**
 * @brief 注入 HTTP、清单、摘要和分区适配器，创建升级服务及入口。
 *
 * @details 按键或可选串口入口只提交升级意图，真正的网络读取和 Flash 写入始终 在独立任务执行。
 */
static void app_start_firmware_update_service(void)
{
    const update_transport_http_config_t transport_config = {
        .timeout_ms = CONFIG_APP_OTA_HTTP_TIMEOUT_MS,
        .receive_buffer_size = CONFIG_APP_OTA_DOWNLOAD_BUFFER_SIZE,
    };
    ESP_ERROR_CHECK(update_transport_http_create(&transport_config, &s_update_transport));

    firmware_update_transport_t transport;
    ESP_ERROR_CHECK(update_transport_http_get_interface(s_update_transport, &transport));

    const update_manifest_json_config_t manifest_config = {
        .maximum_document_size = CONFIG_APP_OTA_MAX_MANIFEST_SIZE,
    };
    ESP_ERROR_CHECK(update_manifest_json_create(&manifest_config, &s_update_manifest_parser));
    firmware_update_manifest_parser_t manifest_parser;
    ESP_ERROR_CHECK(update_manifest_json_get_interface(s_update_manifest_parser, &manifest_parser));

    ESP_ERROR_CHECK(update_integrity_sha256_create(&s_update_integrity));
    firmware_update_integrity_t integrity;
    ESP_ERROR_CHECK(update_integrity_sha256_get_interface(s_update_integrity, &integrity));

    ESP_ERROR_CHECK(update_platform_esp32_create(&s_update_platform));
    firmware_update_platform_t platform;
    ESP_ERROR_CHECK(update_platform_esp32_get_interface(s_update_platform, &platform));

    const esp_app_desc_t *app_description = esp_app_get_description();
    const firmware_update_service_config_t update_config = {
        .transport = transport,
        .manifest_parser = manifest_parser,
        .integrity = integrity,
        .platform = platform,
        .expected_project_name = app_description->project_name,
        .expected_hardware_id = CONFIG_APP_OTA_HARDWARE_ID,
        .current_version = app_description->version,
        .current_release_sequence = CONFIG_APP_FIRMWARE_RELEASE_SEQUENCE,
        .network_context = s_wifi_manager,
        .is_network_ready = app_ota_is_network_ready,
        .restart_context = NULL,
        .restart = app_restart_after_update,
        .download_buffer_size = CONFIG_APP_OTA_DOWNLOAD_BUFFER_SIZE,
        .restart_delay_ms = CONFIG_APP_OTA_RESTART_DELAY_MS,
        .task_stack_size = APP_FIRMWARE_UPDATE_TASK_STACK_SIZE,
        .task_priority = APP_FIRMWARE_UPDATE_TASK_PRIORITY,
        .allow_same_version = false,
#if CONFIG_APP_OTA_ALLOW_DOWNGRADE
        .allow_downgrade = true,
#else
        .allow_downgrade = false,
#endif
    };
    ESP_ERROR_CHECK(firmware_update_service_create(&update_config, &s_firmware_update_service));
    ESP_ERROR_CHECK(firmware_update_service_start(s_firmware_update_service));

#if CONFIG_APP_OTA_CONSOLE_ENABLED
    const ota_console_service_config_t console_config = {
        .update_service = s_firmware_update_service,
        .gateway_link = s_gateway_link,
        .wifi = s_wifi_manager,
        .prompt = "env> ",
    };
    ESP_ERROR_CHECK(ota_console_service_create(&console_config, &s_ota_console_service));
    ESP_ERROR_CHECK(ota_console_service_start(s_ota_console_service));
#endif

#if CONFIG_APP_OTA_BUTTON_TRIGGER_ENABLED
    if (CONFIG_APP_OTA_TEST_URI[0] == '\0')
    {
        ESP_LOGW(TAG, "BOOT-button OTA is enabled but APP_OTA_TEST_URI is empty");
    }
    else
    {
        const ota_button_service_config_t button_config = {
            .gpio_num = (gpio_num_t)CONFIG_APP_OTA_BUTTON_GPIO,
            .hold_time_ms = CONFIG_APP_OTA_BUTTON_HOLD_MS,
            .poll_interval_ms = APP_OTA_BUTTON_POLL_INTERVAL_MS,
            .manifest_uri = CONFIG_APP_OTA_TEST_URI,
            .update_service = s_firmware_update_service,
            .task_stack_size = APP_OTA_BUTTON_TASK_STACK_SIZE,
            .task_priority = APP_OTA_BUTTON_TASK_PRIORITY,
        };
        ESP_ERROR_CHECK(ota_button_service_create(&button_config, &s_ota_button_service));
        ESP_ERROR_CHECK(ota_button_service_start(s_ota_button_service));
    }
#endif
}

/**
 * @brief 检查必须的内部服务与配置已初始化，不依赖外部设备在线。
 * @note 只检查软件内部必要条件，Wi-Fi/Broker/仪表暂时离线不直接触发回滚。
 *
 * @details 外部传感器掉线、Wi-Fi 暂时无法连接或 Broker 离线都不代表新固件损坏， 因此这里只检查软件内部必须成立的条件，不把外部环境作为回滚条件。
 */
static esp_err_t app_run_boot_self_test(void *context)
{
    (void)context;

#if CONFIG_APP_OTA_FORCE_BOOT_HEALTH_FAILURE
    ESP_LOGW(TAG, "Boot health failure is forced by test configuration");
    return ESP_FAIL;
#endif

    if ((s_config_store == NULL) || (s_config_service == NULL) || (s_update_transport == NULL) ||
        (s_update_manifest_parser == NULL) || (s_update_integrity == NULL) ||
        (s_update_platform == NULL) || (s_firmware_update_service == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_APP_GATEWAY_LINK_ENABLED
    if (s_gateway_link == NULL || s_gateway_uart == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
#endif

#if CONFIG_APP_OTA_CONSOLE_ENABLED
    if (s_ota_console_service == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
#endif

#if CONFIG_APP_OTA_BUTTON_TRIGGER_ENABLED
    if ((CONFIG_APP_OTA_TEST_URI[0] != '\0') && (s_ota_button_service == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }
#endif

    app_config_service_status_t config_status;
    const esp_err_t config_result = app_config_service_get_status(s_config_service, &config_status);
    if ((config_result != ESP_OK) || !config_status.initialized)
    {
        return (config_result != ESP_OK) ? config_result : ESP_ERR_INVALID_STATE;
    }

#if CONFIG_APP_GATEWAY_HTTP_ENABLED
    if (s_gateway_http == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
#endif
    if (s_wifi_manager == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if ((CONFIG_APP_MQTT_BROKER_URI[0] != '\0') && (s_mqtt_service == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

/**
 * @brief 创建新镜像的首启健康确认服务。
 */
static void app_start_boot_health_service(void)
{
    const boot_health_service_config_t config = {
        .self_test_context = NULL,
        .run_self_test = app_run_boot_self_test,
        .confirmation_delay_ms = CONFIG_APP_OTA_HEALTH_CONFIRMATION_DELAY_MS,
        .task_stack_size = APP_BOOT_HEALTH_TASK_STACK_SIZE,
        .task_priority = APP_BOOT_HEALTH_TASK_PRIORITY,
    };

    ESP_ERROR_CHECK(boot_health_service_create(&config, &s_boot_health_service));
    ESP_ERROR_CHECK(boot_health_service_start(s_boot_health_service));
}

/**
 * @brief 按依赖顺序创建并启动育苗室网关服务。
 */
void nursery_app_start(void)
{
    ESP_LOGI(TAG, "Starting nursery monitoring and alarm gateway");
    app_initialize_nvs();
    ESP_ERROR_CHECK(
        config_store_create(APP_CONFIG_NVS_NAMESPACE, APP_CONFIG_NVS_KEY, &s_config_store));
    const app_config_service_config_t config = {
        .defaults = {.report_period_ms = CONFIG_APP_MQTT_REPORT_PERIOD_MS},
        .minimum_report_period_ms = MQTT_SERVICE_MIN_REPORT_PERIOD_MS,
        .maximum_report_period_ms = MQTT_SERVICE_MAX_REPORT_PERIOD_MS,
        .store = s_config_store,
    };
    ESP_ERROR_CHECK(app_config_service_create(&config, &s_config_service));
    ESP_ERROR_CHECK(app_config_service_load_and_apply(s_config_service));
    app_start_gateway_link();
    app_start_wifi_if_configured();
    app_start_mqtt_if_configured();
    app_start_gateway_http_if_configured();
    app_start_firmware_update_service();
    app_start_boot_health_service();
    ESP_LOGI(TAG, "Nursery gateway ready");
}
