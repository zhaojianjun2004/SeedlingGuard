/**
 * @file main.c
 * @brief Application composition root for the environment monitor.
 *
 * app_main() intentionally contains no sensor protocol or sampling business
 * logic. Its responsibility is to construct modules with board-specific
 * configuration and start them in a well-defined order.
 */

#include "app_config_service.h"
#include "ble_service.h"
#include "boot_health_service.h"
#include "config_store.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "firmware_update_service.h"
#include "mqtt_service.h"
#include "nvs_flash.h"
#include "ota_button_service.h"
#include "ota_console_service.h"
#include "sensor_service.h"
#include "sdkconfig.h"
#include "update_platform_esp32.h"
#include "update_integrity_sha256.h"
#include "update_manifest_json.h"
#include "update_transport_http.h"
#include "wifi_manager.h"

#define APP_SENSOR_TASK_STACK_SIZE    (4096U)  // 传感器服务任务的堆栈大小4096字节。足够大以容纳 I2C 驱动和 AHT20 协议事务。
#define APP_SENSOR_TASK_PRIORITY      (5U)  // 传感器服务任务的优先级。高于 BLE 通知任务（4），低于 Wi-Fi 连接管理（6）。
#define APP_BOOT_HEALTH_TASK_STACK_SIZE (3072U)
#define APP_BOOT_HEALTH_TASK_PRIORITY   (3U)
#define APP_FIRMWARE_UPDATE_TASK_STACK_SIZE (6144U)
#define APP_FIRMWARE_UPDATE_TASK_PRIORITY   (4U)
#define APP_OTA_BUTTON_TASK_STACK_SIZE      (2048U)
#define APP_OTA_BUTTON_TASK_PRIORITY        (3U)
#define APP_OTA_BUTTON_POLL_INTERVAL_MS     (50U)
#define APP_CONFIG_NVS_NAMESPACE      "env_config"
#define APP_CONFIG_NVS_KEY            "runtime"

static const char *TAG = "app_main";  // TAG 用于 ESP_LOGx 日志前缀

/**  
 * 这些服务在应用整个生命周期内都存在。
 * 把句柄保存在这里，也为将来 BLE 和 MQTT 模块通过 sensor_service_get_latest() 集成提供了明确的接入点。 
 * 用 static 限制在本文件可见，避免外部乱改；用 s_ 前缀表示“静态全局”。
*/
static sensor_service_handle_t s_sensor_service = NULL;
static ble_service_handle_t s_ble_service = NULL;
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
#if CONFIG_APP_OTA_CONSOLE_ENABLED
static ota_console_service_handle_t s_ota_console_service = NULL;
#endif
static ota_button_service_handle_t s_ota_button_service = NULL;

/** 初始化 NVS 分区，用于射频校准和将来的设置保存。 */
static void app_initialize_nvs(void)
{
    esp_err_t result = nvs_flash_init();
    if ((result == ESP_ERR_NVS_NO_FREE_PAGES) ||
        (result == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        /** 只有在 NVS 分区不兼容或空间耗尽时才擦除。正常启动会保留所有将来的用户配置 */
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
}

/** 配置服务通过此适配器修改传感器服务的采样周期。 */
static esp_err_t app_apply_sensor_sample_period(void *context,
                                                uint32_t period_ms)
{
    return sensor_service_set_sample_period((sensor_service_handle_t)context,
                                            period_ms);
}



/** 
 ****************** BLE适配回调  *******************
 */


/** 把传感器服务的快照适配成 BLE 组件的中性模型 */
static esp_err_t app_ble_get_snapshot(void *context,
                                      ble_environment_snapshot_t *out_snapshot)
{
    if ((context == NULL) || (out_snapshot == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    sensor_reading_t reading;
    const esp_err_t result =
        sensor_service_get_latest((sensor_service_handle_t)context, &reading);
    if (result != ESP_OK) {
        return result;
    }

    *out_snapshot = (ble_environment_snapshot_t) {
        .temperature_c = reading.temperature_c,
        .humidity_percent = reading.humidity_percent,
        .sequence_number = reading.sequence_number,
        .consecutive_failures = reading.consecutive_failures,
        .last_error = reading.last_error,
        .valid = reading.valid,
    };
    return ESP_OK;
}

// 获取采样周期
static esp_err_t app_get_sample_period(void *context,
                                       uint32_t *out_period_ms)
{
    if ((context == NULL) || (out_period_ms == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    app_runtime_config_t config;
    const esp_err_t result = app_config_service_get(
        (app_config_service_handle_t)context,
        &config);
    if (result == ESP_OK) {
        *out_period_ms = config.sample_period_ms;
    }
    return result;
}

// 统一更新采样周期；配置服务会在成功应用后保存到 NVS
static esp_err_t app_set_sample_period(void *context, uint32_t period_ms)
{
    const app_runtime_config_update_t update = {
        .has_sample_period = true,
        .sample_period_ms = period_ms,
        .has_report_period = false,
        .report_period_ms = 0U,
    };
    return app_config_service_update((app_config_service_handle_t)context,
                                     &update);
}

/**
 * @brief 在已配置 SSID 时启动 Wi-Fi，未配置时保留离线功能。
 *
 * 连接过程由 wifi_manager 异步执行，因此这里不会等待路由器响应。
 */
static void app_start_wifi_if_configured(void)
{
    if (CONFIG_APP_WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG,
                 "Wi-Fi is not configured; set APP_WIFI_SSID in menuconfig");
        return;
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

/** 将传感器业务快照适配为 MQTT 组件的中立数据结构。 */
static esp_err_t app_mqtt_get_snapshot(
    void *context,
    mqtt_environment_snapshot_t *out_snapshot)
{
    if ((context == NULL) || (out_snapshot == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    sensor_reading_t reading;
    const esp_err_t result =
        sensor_service_get_latest((sensor_service_handle_t)context, &reading);
    if (result != ESP_OK) {
        return result;
    }

    *out_snapshot = (mqtt_environment_snapshot_t) {
        .temperature_c = reading.temperature_c,
        .humidity_percent = reading.humidity_percent,
        .sequence_number = reading.sequence_number,
        .consecutive_failures = reading.consecutive_failures,
        .last_error = reading.last_error,
        .valid = reading.valid,
    };
    return ESP_OK;
}

/** MQTT 通过此回调查询 Wi-Fi 是否已经获得 IPv4 地址。 */
static bool app_mqtt_is_network_ready(void *context)
{
    wifi_manager_status_t status;
    return (wifi_manager_get_status((wifi_manager_handle_t)context, &status) == ESP_OK) &&
           status.has_ipv4;
}

/** 将应用配置快照适配为 MQTT 组件的中立模型。 */
static esp_err_t app_mqtt_get_runtime_config(
    void *context,
    mqtt_runtime_config_t *out_config)
{
    if ((context == NULL) || (out_config == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    app_runtime_config_t config;
    const esp_err_t result = app_config_service_get(
        (app_config_service_handle_t)context,
        &config);
    if (result == ESP_OK) {
        out_config->sample_period_ms = config.sample_period_ms;
        out_config->report_period_ms = config.report_period_ms;
    }
    return result;
}

/** 将 MQTT 的部分更新一次性提交给配置业务层。 */
static esp_err_t app_mqtt_apply_runtime_config(
    void *context,
    const mqtt_runtime_config_update_t *mqtt_update)
{
    if ((context == NULL) || (mqtt_update == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    const app_runtime_config_update_t update = {
        .has_sample_period = mqtt_update->has_sample_period,
        .sample_period_ms = mqtt_update->sample_period_ms,
        .has_report_period = mqtt_update->has_report_period,
        .report_period_ms = mqtt_update->report_period_ms,
    };
    return app_config_service_update((app_config_service_handle_t)context,
                                     &update);
}

/** 在 Wi-Fi 和 Broker 均已配置时启动 MQTT 服务。 */
static void app_start_mqtt_if_configured(void)
{
    if (CONFIG_APP_MQTT_BROKER_URI[0] == '\0') {
        ESP_LOGW(TAG,
                 "MQTT is not configured; set APP_MQTT_BROKER_URI in menuconfig");
        return;
    }
    if (s_wifi_manager == NULL) {
        ESP_LOGW(TAG, "MQTT requires a configured Wi-Fi connection");
        return;
    }

    const mqtt_service_config_t mqtt_config = {
        .broker_uri = CONFIG_APP_MQTT_BROKER_URI,
        .username = CONFIG_APP_MQTT_USERNAME,
        .password = CONFIG_APP_MQTT_PASSWORD,
        .snapshot_context = s_sensor_service,
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

/** OTA Core 通过此适配器查询 Wi-Fi 是否已经获得 IPv4 地址。 */
static bool app_ota_is_network_ready(void *context)
{
    if (context == NULL) {
        return false;
    }

    wifi_manager_status_t status;
    return (wifi_manager_get_status((wifi_manager_handle_t)context,
                                    &status) == ESP_OK) &&
           status.has_ipv4;
}

/** 隔离芯片重启 API，使升级事务核心不直接依赖 ESP32。 */
static void app_restart_after_update(void *context)
{
    (void)context;
    esp_restart();
}

/**
 * @brief 组装 HTTP 传输、ESP32 Flash 适配器、升级 Core 和触发入口。
 *
 * 按键或可选串口入口只提交升级意图，真正的网络读取和 Flash 写入始终
 * 在独立任务执行。
 */
static void app_start_firmware_update_service(void)
{
    const update_transport_http_config_t transport_config = {
        .timeout_ms = CONFIG_APP_OTA_HTTP_TIMEOUT_MS,
        .receive_buffer_size = CONFIG_APP_OTA_DOWNLOAD_BUFFER_SIZE,
    };
    ESP_ERROR_CHECK(update_transport_http_create(&transport_config,
                                                  &s_update_transport));

    firmware_update_transport_t transport;
    ESP_ERROR_CHECK(update_transport_http_get_interface(s_update_transport,
                                                        &transport));

    const update_manifest_json_config_t manifest_config = {
        .maximum_document_size = CONFIG_APP_OTA_MAX_MANIFEST_SIZE,
    };
    ESP_ERROR_CHECK(update_manifest_json_create(&manifest_config,
                                                 &s_update_manifest_parser));
    firmware_update_manifest_parser_t manifest_parser;
    ESP_ERROR_CHECK(update_manifest_json_get_interface(
        s_update_manifest_parser,
        &manifest_parser));

    ESP_ERROR_CHECK(update_integrity_sha256_create(&s_update_integrity));
    firmware_update_integrity_t integrity;
    ESP_ERROR_CHECK(update_integrity_sha256_get_interface(s_update_integrity,
                                                          &integrity));

    ESP_ERROR_CHECK(update_platform_esp32_create(&s_update_platform));
    firmware_update_platform_t platform;
    ESP_ERROR_CHECK(update_platform_esp32_get_interface(s_update_platform,
                                                        &platform));

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
    ESP_ERROR_CHECK(firmware_update_service_create(
        &update_config,
        &s_firmware_update_service));
    ESP_ERROR_CHECK(firmware_update_service_start(
        s_firmware_update_service));

#if CONFIG_APP_OTA_CONSOLE_ENABLED
    const ota_console_service_config_t console_config = {
        .update_service = s_firmware_update_service,
        .prompt = "env> ",
    };
    ESP_ERROR_CHECK(ota_console_service_create(&console_config,
                                                &s_ota_console_service));
    ESP_ERROR_CHECK(ota_console_service_start(s_ota_console_service));
#endif

#if CONFIG_APP_OTA_BUTTON_TRIGGER_ENABLED
    if (CONFIG_APP_OTA_TEST_URI[0] == '\0') {
        ESP_LOGW(TAG,
                 "BOOT-button OTA is enabled but APP_OTA_TEST_URI is empty");
    } else {
        const ota_button_service_config_t button_config = {
            .gpio_num = (gpio_num_t)CONFIG_APP_OTA_BUTTON_GPIO,
            .hold_time_ms = CONFIG_APP_OTA_BUTTON_HOLD_MS,
            .poll_interval_ms = APP_OTA_BUTTON_POLL_INTERVAL_MS,
            .manifest_uri = CONFIG_APP_OTA_TEST_URI,
            .update_service = s_firmware_update_service,
            .task_stack_size = APP_OTA_BUTTON_TASK_STACK_SIZE,
            .task_priority = APP_OTA_BUTTON_TASK_PRIORITY,
        };
        ESP_ERROR_CHECK(ota_button_service_create(&button_config,
                                                   &s_ota_button_service));
        ESP_ERROR_CHECK(ota_button_service_start(s_ota_button_service));
    }
#endif
}

/**
 * @brief 检查应用核心服务是否完成初始化。
 *
 * 外部传感器掉线、Wi-Fi 暂时无法连接或 Broker 离线都不代表新固件损坏，
 * 因此这里只检查软件内部必须成立的条件，不把外部环境作为回滚条件。
 */
static esp_err_t app_run_boot_self_test(void *context)
{
    (void)context;

#if CONFIG_APP_OTA_FORCE_BOOT_HEALTH_FAILURE
    ESP_LOGW(TAG, "Boot health failure is forced by test configuration");
    return ESP_FAIL;
#endif

    if ((s_sensor_service == NULL) ||
        (s_ble_service == NULL) ||
        (s_config_store == NULL) ||
        (s_config_service == NULL) ||
        (s_update_transport == NULL) ||
        (s_update_manifest_parser == NULL) ||
        (s_update_integrity == NULL) ||
        (s_update_platform == NULL) ||
        (s_firmware_update_service == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_APP_OTA_CONSOLE_ENABLED
    if (s_ota_console_service == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
#endif

#if CONFIG_APP_OTA_BUTTON_TRIGGER_ENABLED
    if ((CONFIG_APP_OTA_TEST_URI[0] != '\0') &&
        (s_ota_button_service == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }
#endif

    app_config_service_status_t config_status;
    const esp_err_t config_result = app_config_service_get_status(
        s_config_service,
        &config_status);
    if ((config_result != ESP_OK) || !config_status.initialized) {
        return (config_result != ESP_OK) ? config_result : ESP_ERR_INVALID_STATE;
    }

    if ((CONFIG_APP_WIFI_SSID[0] != '\0') && (s_wifi_manager == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((CONFIG_APP_MQTT_BROKER_URI[0] != '\0') &&
        (s_mqtt_service == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

/** 创建启动健康服务；仅待确认 OTA 镜像会启动后台自检任务。 */
static void app_start_boot_health_service(void)
{
    const boot_health_service_config_t config = {
        .self_test_context = NULL,
        .run_self_test = app_run_boot_self_test,
        .confirmation_delay_ms = CONFIG_APP_OTA_HEALTH_CONFIRMATION_DELAY_MS,
        .task_stack_size = APP_BOOT_HEALTH_TASK_STACK_SIZE,
        .task_priority = APP_BOOT_HEALTH_TASK_PRIORITY,
    };

    ESP_ERROR_CHECK(boot_health_service_create(&config,
                                                &s_boot_health_service));
    ESP_ERROR_CHECK(boot_health_service_start(s_boot_health_service));
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP32-S3 environment monitor");
    ESP_LOGI(TAG,
             "AHT20 wiring: VCC=3V3, GND=GND, SDA=GPIO%d, SCL=GPIO%d",
             CONFIG_APP_AHT20_SDA_GPIO,
             CONFIG_APP_AHT20_SCL_GPIO);

    app_initialize_nvs();

    const sensor_service_config_t sensor_config = {
        .i2c_port = I2C_NUM_0,
        .sda_gpio = (gpio_num_t)CONFIG_APP_AHT20_SDA_GPIO,
        .scl_gpio = (gpio_num_t)CONFIG_APP_AHT20_SCL_GPIO,
        .i2c_clock_hz = CONFIG_APP_AHT20_I2C_CLOCK_HZ,
        .enable_internal_pullups = true,
        .sample_period_ms = CONFIG_APP_SENSOR_SAMPLE_PERIOD_MS,
        .task_stack_size = APP_SENSOR_TASK_STACK_SIZE,
        .task_priority = APP_SENSOR_TASK_PRIORITY,
    };

    /* Bus or task creation failures are unrecoverable application setup
     * errors, so fail fast. A missing AHT20 is different: the background
     * service reports it and keeps retrying without rebooting the board. */
    ESP_ERROR_CHECK(sensor_service_create(&sensor_config, &s_sensor_service));

    ESP_ERROR_CHECK(config_store_create(APP_CONFIG_NVS_NAMESPACE,
                                        APP_CONFIG_NVS_KEY,
                                        &s_config_store));
    const app_config_service_config_t config_service_config = {
        .defaults = {
            .sample_period_ms = CONFIG_APP_SENSOR_SAMPLE_PERIOD_MS,
            .report_period_ms = CONFIG_APP_MQTT_REPORT_PERIOD_MS,
        },
        .minimum_sample_period_ms = SENSOR_SERVICE_MIN_SAMPLE_PERIOD_MS,
        .maximum_sample_period_ms = SENSOR_SERVICE_MAX_SAMPLE_PERIOD_MS,
        .minimum_report_period_ms = MQTT_SERVICE_MIN_REPORT_PERIOD_MS,
        .maximum_report_period_ms = MQTT_SERVICE_MAX_REPORT_PERIOD_MS,
        .store = s_config_store,
        .sample_context = s_sensor_service,
        .apply_sample_period = app_apply_sensor_sample_period,
    };
    ESP_ERROR_CHECK(app_config_service_create(&config_service_config,
                                              &s_config_service));
    ESP_ERROR_CHECK(app_config_service_load_and_apply(s_config_service));
    ESP_ERROR_CHECK(sensor_service_start(s_sensor_service));

    const ble_service_config_t ble_config = {
        .device_name = CONFIG_APP_BLE_DEVICE_NAME,
        .snapshot_context = s_sensor_service,
        .get_snapshot = app_ble_get_snapshot,
        .config_context = s_config_service,
        .get_sample_period = app_get_sample_period,
        .set_sample_period = app_set_sample_period,
    };
    ESP_ERROR_CHECK(ble_service_create(&ble_config, &s_ble_service));
    ESP_ERROR_CHECK(ble_service_start(s_ble_service));

    app_start_wifi_if_configured();
    app_start_mqtt_if_configured();
    app_start_firmware_update_service();
    app_start_boot_health_service();

    ESP_LOGI(TAG, "Environment monitor started successfully");
}
