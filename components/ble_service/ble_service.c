/**
 * @file ble_service.c
 * @brief NimBLE GATT server for environmental data and local configuration.
 */

#include "ble_service.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define BLE_SERVICE_ENVIRONMENTAL_SENSING_UUID (0x181AU)
#define BLE_SERVICE_TEMPERATURE_UUID           (0x2A6EU)
#define BLE_SERVICE_HUMIDITY_UUID              (0x2A6FU)

#define BLE_SERVICE_NOTIFY_TASK_STACK_SIZE     (4096U)  
#define BLE_SERVICE_NOTIFY_TASK_PRIORITY       (4U)    
#define BLE_SERVICE_NOTIFY_POLL_PERIOD_MS      (250U)

#define BLE_SERVICE_STATUS_PAYLOAD_VERSION     (1U)
#define BLE_SERVICE_STATUS_PAYLOAD_LENGTH      (14U)
#define BLE_SERVICE_SAMPLE_PERIOD_LENGTH       (4U)

static const char *TAG = "ble_service";

/* Bluetooth SIG standard UUIDs let generic applications label temperature and
 * humidity automatically. Values use the standard 0.01-unit resolution. */
static const ble_uuid16_t s_environment_service_uuid =
    BLE_UUID16_INIT(BLE_SERVICE_ENVIRONMENTAL_SENSING_UUID);
static const ble_uuid16_t s_temperature_uuid =
    BLE_UUID16_INIT(BLE_SERVICE_TEMPERATURE_UUID);
static const ble_uuid16_t s_humidity_uuid =
    BLE_UUID16_INIT(BLE_SERVICE_HUMIDITY_UUID);

/* Project-specific service UUIDs in canonical form:
 *   service:         7f6a0001-6c7b-4e7d-9f7d-4bf3a9b91000
 *   device status:   7f6a0002-6c7b-4e7d-9f7d-4bf3a9b91000
 *   sample interval: 7f6a0003-6c7b-4e7d-9f7d-4bf3a9b91000
 * BLE_UUID128_INIT receives bytes in little-endian transmission order. */
static const ble_uuid128_t s_control_service_uuid = BLE_UUID128_INIT(
    0x00, 0x10, 0xB9, 0xA9, 0xF3, 0x4B, 0x7D, 0x9F,
    0x7D, 0x4E, 0x7B, 0x6C, 0x01, 0x00, 0x6A, 0x7F);
static const ble_uuid128_t s_status_uuid = BLE_UUID128_INIT(
    0x00, 0x10, 0xB9, 0xA9, 0xF3, 0x4B, 0x7D, 0x9F,
    0x7D, 0x4E, 0x7B, 0x6C, 0x02, 0x00, 0x6A, 0x7F);
static const ble_uuid128_t s_sample_period_uuid = BLE_UUID128_INIT(
    0x00, 0x10, 0xB9, 0xA9, 0xF3, 0x4B, 0x7D, 0x9F,
    0x7D, 0x4E, 0x7B, 0x6C, 0x03, 0x00, 0x6A, 0x7F);

struct ble_service {
    ble_service_config_t config;
    char device_name[BLE_SERVICE_MAX_DEVICE_NAME_LENGTH + 1U];

    struct ble_gatt_chr_def environment_characteristics[3];
    struct ble_gatt_chr_def control_characteristics[3];
    struct ble_gatt_svc_def services[3];

    uint16_t temperature_value_handle;
    uint16_t humidity_value_handle;
    uint16_t status_value_handle;
    uint16_t sample_period_value_handle;

    TaskHandle_t notification_task;
    uint8_t own_address_type;
    volatile bool host_synced;
    bool started;
};

/* NimBLE owns one controller and host stack, so the application intentionally
 * permits only one active ble_service instance. The GATT callbacks themselves
 * receive their instance through the characteristic arg field. */
static ble_service_handle_t s_active_service = NULL;

static int ble_service_gap_event(struct ble_gap_event *event, void *argument);

static void ble_service_put_u16_le(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value & 0xFFU);
    destination[1] = (uint8_t)((value >> 8U) & 0xFFU);
}

static void ble_service_put_u32_le(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value & 0xFFU);
    destination[1] = (uint8_t)((value >> 8U) & 0xFFU);
    destination[2] = (uint8_t)((value >> 16U) & 0xFFU);
    destination[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

static uint32_t ble_service_get_u32_le(const uint8_t *source)
{
    return (uint32_t)source[0] |
           ((uint32_t)source[1] << 8U) |
           ((uint32_t)source[2] << 16U) |
           ((uint32_t)source[3] << 24U);
}

static int ble_service_append(struct os_mbuf *buffer,
                              const void *data,
                              uint16_t data_length)
{
    return (os_mbuf_append(buffer, data, data_length) == 0)
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static esp_err_t ble_service_read_snapshot(ble_service_handle_t handle,
                                           ble_environment_snapshot_t *out_snapshot)
{
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    out_snapshot->last_error = ESP_ERR_INVALID_STATE;
    return handle->config.get_snapshot(handle->config.snapshot_context, out_snapshot);
}

static int16_t ble_service_temperature_to_centi_degrees(float temperature_c)
{
    float scaled = temperature_c * 100.0F;
    if (scaled >= (float)INT16_MAX) {
        return INT16_MAX;
    }
    if (scaled <= (float)INT16_MIN) {
        return INT16_MIN;
    }
    scaled += (scaled >= 0.0F) ? 0.5F : -0.5F;
    return (int16_t)scaled;
}

static uint16_t ble_service_humidity_to_centi_percent(float humidity_percent)
{
    float bounded = humidity_percent;
    if (bounded < 0.0F) {
        bounded = 0.0F;
    } else if (bounded > 100.0F) {
        bounded = 100.0F;
    }
    return (uint16_t)((bounded * 100.0F) + 0.5F);
}

static int ble_service_environment_access(uint16_t connection_handle,
                                          uint16_t attribute_handle,
                                          struct ble_gatt_access_ctxt *context,
                                          void *argument)
{
    (void)connection_handle;
    ble_service_handle_t handle = (ble_service_handle_t)argument;

    if (context->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }

    ble_environment_snapshot_t snapshot;
    if ((ble_service_read_snapshot(handle, &snapshot) != ESP_OK) || !snapshot.valid) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t payload[2];
    if (attribute_handle == handle->temperature_value_handle) {
        const int16_t value = ble_service_temperature_to_centi_degrees(snapshot.temperature_c);
        ble_service_put_u16_le(payload, (uint16_t)value);
        return ble_service_append(context->om, payload, sizeof(payload));
    }
    if (attribute_handle == handle->humidity_value_handle) {
        const uint16_t value = ble_service_humidity_to_centi_percent(snapshot.humidity_percent);
        ble_service_put_u16_le(payload, value);
        return ble_service_append(context->om, payload, sizeof(payload));
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int ble_service_control_access(uint16_t connection_handle,
                                      uint16_t attribute_handle,
                                      struct ble_gatt_access_ctxt *context,
                                      void *argument)
{
    (void)connection_handle;
    ble_service_handle_t handle = (ble_service_handle_t)argument;

    if ((context->op == BLE_GATT_ACCESS_OP_READ_CHR) &&
        (attribute_handle == handle->status_value_handle)) {
        ble_environment_snapshot_t snapshot;
        const esp_err_t provider_result = ble_service_read_snapshot(handle, &snapshot);
        if (provider_result != ESP_OK) {
            snapshot.valid = false;
            snapshot.last_error = provider_result;
        }

        /* Versioned, fixed-length wire format:
         * [0] version; [1] flags (bit0 valid, bit1 sensor error);
         * [2..5] sequence; [6..9] consecutive failures;
         * [10..13] last esp_err_t, all multi-byte fields little-endian. */
        uint8_t payload[BLE_SERVICE_STATUS_PAYLOAD_LENGTH] = {0U};
        payload[0] = BLE_SERVICE_STATUS_PAYLOAD_VERSION;
        payload[1] = (snapshot.valid ? BIT0 : 0U) |
                     ((snapshot.last_error != ESP_OK) ? BIT1 : 0U);
        ble_service_put_u32_le(&payload[2], snapshot.sequence_number);
        ble_service_put_u32_le(&payload[6], snapshot.consecutive_failures);
        ble_service_put_u32_le(&payload[10], (uint32_t)snapshot.last_error);
        return ble_service_append(context->om, payload, sizeof(payload));
    }

    if ((context->op == BLE_GATT_ACCESS_OP_READ_CHR) &&
        (attribute_handle == handle->sample_period_value_handle)) {
        uint32_t period_ms = 0U;
        if (handle->config.get_sample_period(handle->config.config_context, &period_ms) != ESP_OK) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        uint8_t payload[BLE_SERVICE_SAMPLE_PERIOD_LENGTH];
        ble_service_put_u32_le(payload, period_ms);
        return ble_service_append(context->om, payload, sizeof(payload));
    }

    if ((context->op == BLE_GATT_ACCESS_OP_WRITE_CHR) &&
        (attribute_handle == handle->sample_period_value_handle)) {
        if (OS_MBUF_PKTLEN(context->om) != BLE_SERVICE_SAMPLE_PERIOD_LENGTH) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        uint8_t payload[BLE_SERVICE_SAMPLE_PERIOD_LENGTH];
        uint16_t copied_length = 0U;
        if ((ble_hs_mbuf_to_flat(context->om,
                                 payload,
                                 sizeof(payload),
                                 &copied_length) != 0) ||
            (copied_length != sizeof(payload))) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        const uint32_t period_ms = ble_service_get_u32_le(payload);
        const esp_err_t result =
            handle->config.set_sample_period(handle->config.config_context, period_ms);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Rejected sample period %lu ms: %s",
                     (unsigned long)period_ms,
                     esp_err_to_name(result));
            return BLE_ATT_ERR_UNLIKELY;
        }

        ESP_LOGI(TAG, "BLE client set sample period to %lu ms",
                 (unsigned long)period_ms);
        return 0;
    }

    return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
}

static void ble_service_build_gatt_table(ble_service_handle_t handle)
{
    handle->environment_characteristics[0] = (struct ble_gatt_chr_def) {
        .uuid = &s_temperature_uuid.u,
        .access_cb = ble_service_environment_access,
        .arg = handle,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &handle->temperature_value_handle,
    };
    handle->environment_characteristics[1] = (struct ble_gatt_chr_def) {
        .uuid = &s_humidity_uuid.u,
        .access_cb = ble_service_environment_access,
        .arg = handle,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &handle->humidity_value_handle,
    };

    handle->control_characteristics[0] = (struct ble_gatt_chr_def) {
        .uuid = &s_status_uuid.u,
        .access_cb = ble_service_control_access,
        .arg = handle,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &handle->status_value_handle,
    };
    handle->control_characteristics[1] = (struct ble_gatt_chr_def) {
        .uuid = &s_sample_period_uuid.u,
        .access_cb = ble_service_control_access,
        .arg = handle,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &handle->sample_period_value_handle,
    };

    handle->services[0] = (struct ble_gatt_svc_def) {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_environment_service_uuid.u,
        .characteristics = handle->environment_characteristics,
    };
    handle->services[1] = (struct ble_gatt_svc_def) {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_control_service_uuid.u,
        .characteristics = handle->control_characteristics,
    };
}

static esp_err_t ble_service_start_advertising(ble_service_handle_t handle)
{
    if (ble_gap_adv_active()) {
        return ESP_OK;
    }

    struct ble_hs_adv_fields advertising_fields = {0};
    advertising_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    advertising_fields.uuids16 = (ble_uuid16_t *)&s_environment_service_uuid;
    advertising_fields.num_uuids16 = 1U;
    advertising_fields.uuids16_is_complete = 1U;

    int result = ble_gap_adv_set_fields(&advertising_fields);
    if (result != 0) {
        ESP_LOGE(TAG, "Failed to set advertising fields: rc=%d", result);
        return ESP_FAIL;
    }

    struct ble_hs_adv_fields scan_response_fields = {0};
    scan_response_fields.name = (uint8_t *)handle->device_name;
    scan_response_fields.name_len = (uint8_t)strlen(handle->device_name);
    scan_response_fields.name_is_complete = 1U;
    result = ble_gap_adv_rsp_set_fields(&scan_response_fields);
    if (result != 0) {
        ESP_LOGE(TAG, "Failed to set scan-response fields: rc=%d", result);
        return ESP_FAIL;
    }

    const struct ble_gap_adv_params parameters = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    result = ble_gap_adv_start(handle->own_address_type,
                               NULL,
                               BLE_HS_FOREVER,
                               &parameters,
                               ble_service_gap_event,
                               handle);
    if (result != 0) {
        ESP_LOGE(TAG, "Failed to start advertising: rc=%d", result);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Advertising as '%s'", handle->device_name);
    return ESP_OK;
}

static int ble_service_gap_event(struct ble_gap_event *event, void *argument)
{
    ble_service_handle_t handle = (ble_service_handle_t)argument;

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ESP_LOGI(TAG, "BLE client connected; handle=%u",
                         event->connect.conn_handle);
            } else {
                ESP_LOGW(TAG, "BLE connection failed: status=%d",
                         event->connect.status);
                (void)ble_service_start_advertising(handle);
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "BLE client disconnected; reason=%d",
                     event->disconnect.reason);
            (void)ble_service_start_advertising(handle);
            break;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            ESP_LOGW(TAG, "BLE advertising completed; reason=%d",
                     event->adv_complete.reason);
            (void)ble_service_start_advertising(handle);
            break;

        case BLE_GAP_EVENT_SUBSCRIBE:
            ESP_LOGI(TAG,
                     "Subscription changed: conn=%u attr=%u notify=%u indicate=%u",
                     event->subscribe.conn_handle,
                     event->subscribe.attr_handle,
                     event->subscribe.cur_notify,
                     event->subscribe.cur_indicate);
            break;

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "MTU updated: conn=%u mtu=%u",
                     event->mtu.conn_handle,
                     event->mtu.value);
            break;

        default:
            break;
    }
    return 0;
}

static void ble_service_on_reset(int reason)
{
    if (s_active_service != NULL) {
        s_active_service->host_synced = false;
    }
    ESP_LOGE(TAG, "NimBLE host reset; reason=%d", reason);
}

static void ble_service_on_sync(void)
{
    ble_service_handle_t handle = s_active_service;
    if (handle == NULL) {
        return;
    }

    int result = ble_hs_util_ensure_addr(0);
    if (result == 0) {
        result = ble_hs_id_infer_auto(0, &handle->own_address_type);
    }
    if (result != 0) {
        ESP_LOGE(TAG, "Failed to determine BLE identity address: rc=%d", result);
        return;
    }

    uint8_t address[6] = {0U};
    result = ble_hs_id_copy_addr(handle->own_address_type, address, NULL);
    if (result != 0) {
        ESP_LOGE(TAG, "Failed to read BLE identity address: rc=%d", result);
        return;
    }

    handle->host_synced = true;
    ESP_LOGI(TAG, "BLE address %02X:%02X:%02X:%02X:%02X:%02X",
             address[5], address[4], address[3],
             address[2], address[1], address[0]);
    (void)ble_service_start_advertising(handle);
}

static void ble_service_host_task(void *context)
{
    (void)context;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_service_notification_task(void *context)
{
    ble_service_handle_t handle = (ble_service_handle_t)context;
    ble_environment_snapshot_t previous = {0};
    bool previous_available = false;

    for (;;) {
        ble_environment_snapshot_t current;
        if (handle->host_synced &&
            (ble_service_read_snapshot(handle, &current) == ESP_OK)) {
            const bool new_measurement = current.valid &&
                (!previous_available ||
                 !previous.valid ||
                 (current.sequence_number != previous.sequence_number));
            const bool status_changed = !previous_available ||
                (current.valid != previous.valid) ||
                (current.sequence_number != previous.sequence_number) ||
                (current.consecutive_failures != previous.consecutive_failures) ||
                (current.last_error != previous.last_error);

            /* ble_gatts_chr_updated() sends only to peers that enabled the
             * automatically generated CCCD, so no connection list is needed. */
            if (new_measurement) {
                ble_gatts_chr_updated(handle->temperature_value_handle);
                ble_gatts_chr_updated(handle->humidity_value_handle);
            }
            if (status_changed) {
                ble_gatts_chr_updated(handle->status_value_handle);
            }

            previous = current;
            previous_available = true;
        }

        vTaskDelay(pdMS_TO_TICKS(BLE_SERVICE_NOTIFY_POLL_PERIOD_MS));
    }
}

static bool ble_service_config_is_valid(const ble_service_config_t *config)
{
    if ((config == NULL) ||
        (config->device_name == NULL) ||
        (config->snapshot_context == NULL) ||
        (config->get_snapshot == NULL) ||
        (config->config_context == NULL) ||
        (config->get_sample_period == NULL) ||
        (config->set_sample_period == NULL)) {
        return false;
    }

    const size_t name_length = strlen(config->device_name);
    return (name_length > 0U) &&
           (name_length <= BLE_SERVICE_MAX_DEVICE_NAME_LENGTH);
}

esp_err_t ble_service_create(const ble_service_config_t *config,
                             ble_service_handle_t *out_handle)
{
    if (!ble_service_config_is_valid(config) || (out_handle == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_handle = NULL;
    ble_service_handle_t handle = calloc(1U, sizeof(*handle));
    if (handle == NULL) {
        return ESP_ERR_NO_MEM;
    }

    handle->config = *config;
    (void)strncpy(handle->device_name,
                  config->device_name,
                  sizeof(handle->device_name) - 1U);
    handle->config.device_name = handle->device_name;
    ble_service_build_gatt_table(handle);

    *out_handle = handle;
    return ESP_OK;
}

esp_err_t ble_service_start(ble_service_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->started || (s_active_service != NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = nimble_port_init();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NimBLE: %s", esp_err_to_name(result));
        return result;
    }

    s_active_service = handle;
    ble_hs_cfg.reset_cb = ble_service_on_reset;
    ble_hs_cfg.sync_cb = ble_service_on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int nimble_result = ble_gatts_count_cfg(handle->services);
    if (nimble_result == 0) {
        nimble_result = ble_gatts_add_svcs(handle->services);
    }
    if (nimble_result == 0) {
        nimble_result = ble_svc_gap_device_name_set(handle->device_name);
    }
    if (nimble_result != 0) {
        ESP_LOGE(TAG, "Failed to register NimBLE GATT database: rc=%d", nimble_result);
        s_active_service = NULL;
        (void)nimble_port_deinit();
        return ESP_FAIL;
    }

    const BaseType_t task_created = xTaskCreate(ble_service_notification_task,
                                                "ble_notify",
                                                BLE_SERVICE_NOTIFY_TASK_STACK_SIZE,
                                                handle,
                                                BLE_SERVICE_NOTIFY_TASK_PRIORITY,
                                                &handle->notification_task);
    if (task_created != pdPASS) {
        s_active_service = NULL;
        (void)nimble_port_deinit();
        return ESP_ERR_NO_MEM;
    }

    handle->started = true;
    nimble_port_freertos_init(ble_service_host_task);
    ESP_LOGI(TAG, "BLE service started");
    return ESP_OK;
}
