/**
 * @file firmware_update_service.h
 * @brief 与具体网络协议和芯片 Flash API 解耦的固件升级事务服务。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FIRMWARE_UPDATE_URI_MAX_LENGTH       (256U)
#define FIRMWARE_UPDATE_NAME_MAX_LENGTH      (32U)
#define FIRMWARE_UPDATE_HARDWARE_MAX_LENGTH  (32U)
#define FIRMWARE_UPDATE_PARTITION_MAX_LENGTH (16U)
#define FIRMWARE_UPDATE_SHA256_LENGTH        (32U)

/** 升级状态；每个状态都对应一个可观察、可诊断的事务阶段。 */
typedef enum {
    FIRMWARE_UPDATE_STATE_CREATED = 0,
    FIRMWARE_UPDATE_STATE_IDLE,
    FIRMWARE_UPDATE_STATE_QUEUED,
    FIRMWARE_UPDATE_STATE_FETCHING_MANIFEST,
    FIRMWARE_UPDATE_STATE_CHECKING_MANIFEST,
    FIRMWARE_UPDATE_STATE_OPENING_TRANSPORT,
    FIRMWARE_UPDATE_STATE_CHECKING_IMAGE,
    FIRMWARE_UPDATE_STATE_WRITING,
    FIRMWARE_UPDATE_STATE_VALIDATING,
    FIRMWARE_UPDATE_STATE_ACTIVATING,
    FIRMWARE_UPDATE_STATE_REBOOT_PENDING,
    FIRMWARE_UPDATE_STATE_FAILED,
} firmware_update_state_t;

/** 从固件镜像头部提取出的平台中立信息。 */
typedef struct {
    char project_name[FIRMWARE_UPDATE_NAME_MAX_LENGTH + 1U];
    char version[FIRMWARE_UPDATE_NAME_MAX_LENGTH + 1U];
} firmware_update_image_info_t;

/** Manifest 解析后的平台中立升级约束。 */
typedef struct {
    uint32_t schema_version;
    char product[FIRMWARE_UPDATE_NAME_MAX_LENGTH + 1U];
    char hardware[FIRMWARE_UPDATE_HARDWARE_MAX_LENGTH + 1U];
    char version[FIRMWARE_UPDATE_NAME_MAX_LENGTH + 1U];
    uint32_t release_sequence;
    char image_uri[FIRMWARE_UPDATE_URI_MAX_LENGTH + 1U];
    size_t image_size;
    uint8_t sha256[FIRMWARE_UPDATE_SHA256_LENGTH];
} firmware_update_manifest_t;

/**
 * @brief 固件字节流接口。
 *
 * Core 只认识 open/read/close，不知道字节来自 HTTP、CAN 还是其他介质。
 */
typedef struct {
    void *context;
    esp_err_t (*open)(void *context, const char *uri,
                      size_t *out_content_length);
    esp_err_t (*read)(void *context, uint8_t *buffer, size_t capacity,
                      size_t *out_length, bool *out_complete);
    void (*close)(void *context);
} firmware_update_transport_t;

/** Manifest 文档解析接口；Core 不依赖 JSON 等具体编码格式。 */
typedef struct {
    void *context;
    size_t maximum_document_size;
    esp_err_t (*parse)(void *context, const uint8_t *document,
                       size_t document_length,
                       firmware_update_manifest_t *out_manifest);
} firmware_update_manifest_parser_t;

/**
 * @brief 固件流完整性验证接口。
 *
 * 实现可以使用 SHA-256，也可以在目标 BMS 中替换为带签名验证的实现。
 */
typedef struct {
    void *context;
    esp_err_t (*begin)(void *context, const uint8_t *expected_digest,
                       size_t digest_length);
    esp_err_t (*update)(void *context, const uint8_t *data, size_t length);
    esp_err_t (*finish)(void *context);
    void (*abort)(void *context);
} firmware_update_integrity_t;

/**
 * @brief 固件存储与激活接口。
 *
 * Core 不直接调用 ESP-IDF OTA API，后续映射到 F103 Bootloader 时可替换本接口。
 */
typedef struct {
    void *context;
    size_t minimum_header_size;
    esp_err_t (*inspect_header)(void *context, const uint8_t *data,
                                size_t length,
                                firmware_update_image_info_t *out_info);
    esp_err_t (*begin)(void *context, size_t image_size,
                       char *out_target, size_t target_capacity);
    esp_err_t (*write)(void *context, const uint8_t *data, size_t length);
    esp_err_t (*finish)(void *context);
    esp_err_t (*activate)(void *context);
    void (*abort)(void *context);
} firmware_update_platform_t;

typedef bool (*firmware_update_network_ready_fn)(void *context);
typedef void (*firmware_update_restart_fn)(void *context);

typedef struct {
    firmware_update_transport_t transport;
    firmware_update_manifest_parser_t manifest_parser;
    firmware_update_integrity_t integrity;
    firmware_update_platform_t platform;
    const char *expected_project_name;
    const char *expected_hardware_id;
    const char *current_version;
    uint32_t current_release_sequence;
    void *network_context;
    firmware_update_network_ready_fn is_network_ready;
    void *restart_context;
    firmware_update_restart_fn restart;
    size_t download_buffer_size;
    uint32_t restart_delay_ms;
    uint32_t task_stack_size;
    UBaseType_t task_priority;
    bool allow_same_version;
    bool allow_downgrade;
} firmware_update_service_config_t;

typedef struct {
    firmware_update_state_t state;
    esp_err_t last_error;
    size_t bytes_received;
    size_t total_bytes;
    uint8_t progress_percent;
    uint32_t release_sequence;
    char image_version[FIRMWARE_UPDATE_NAME_MAX_LENGTH + 1U];
    char target_partition[FIRMWARE_UPDATE_PARTITION_MAX_LENGTH + 1U];
} firmware_update_status_t;

typedef struct firmware_update_service *firmware_update_service_handle_t;

/** 创建升级服务和内部同步对象，但暂不创建工作任务。 */
esp_err_t firmware_update_service_create(
    const firmware_update_service_config_t *config,
    firmware_update_service_handle_t *out_handle);

/** 启动单一升级工作任务。 */
esp_err_t firmware_update_service_start(
    firmware_update_service_handle_t handle);

/**
 * @brief 异步提交一次 Manifest 升级请求。
 *
 * 同一时刻只允许一个事务；忙碌时返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t firmware_update_service_request(
    firmware_update_service_handle_t handle,
    const char *manifest_uri);

/** 线程安全地获取升级进度和最近错误。 */
esp_err_t firmware_update_service_get_status(
    firmware_update_service_handle_t handle,
    firmware_update_status_t *out_status);

/** 将状态枚举转换为日志可读文本。 */
const char *firmware_update_state_to_string(firmware_update_state_t state);

#ifdef __cplusplus
}
#endif
