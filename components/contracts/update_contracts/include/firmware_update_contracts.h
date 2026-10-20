/**
 ******************************************************************************
 * @file    firmware_update_contracts.h
 * @brief   OTA 公共接口契约接口定义。
 * @details 服务和底层适配器共同依赖，接口契约不包含升级服务实现。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：公共接口契约
 ******************************************************************************
 */

#pragma once
/*--------------------------- 头文件 ---------------------------*/
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C"
{
#endif
#define FIRMWARE_UPDATE_URI_MAX_LENGTH       (256U)
#define FIRMWARE_UPDATE_NAME_MAX_LENGTH      (32U)
#define FIRMWARE_UPDATE_HARDWARE_MAX_LENGTH  (32U)
#define FIRMWARE_UPDATE_PARTITION_MAX_LENGTH (16U)
#define FIRMWARE_UPDATE_SHA256_LENGTH        (32U)

/** 升级状态；每个状态都对应一个可观察、可诊断的事务阶段。 */
typedef enum
{
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
typedef struct
{
    char project_name[FIRMWARE_UPDATE_NAME_MAX_LENGTH + 1U];
    char version[FIRMWARE_UPDATE_NAME_MAX_LENGTH + 1U];
} firmware_update_image_info_t;

/** Manifest 解析后的平台中立升级约束。 */
typedef struct
{
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
typedef struct
{
    void *context;
    esp_err_t (*open)(void *context, const char *uri, size_t *out_content_length);
    esp_err_t (*read)(void *context, uint8_t *buffer, size_t capacity, size_t *out_length,
                      bool *out_complete);
    void (*close)(void *context);
} firmware_update_transport_t;

/** Manifest 文档解析接口；Core 不依赖 JSON 等具体编码格式。 */
typedef struct
{
    void *context;
    size_t maximum_document_size;
    esp_err_t (*parse)(void *context, const uint8_t *document, size_t document_length,
                       firmware_update_manifest_t *out_manifest);
} firmware_update_manifest_parser_t;

/**
     * @brief 固件流完整性验证接口。
     *
     * 实现可以使用 SHA-256，也可替换为带签名验证的实现。
     */
typedef struct
{
    void *context;
    esp_err_t (*begin)(void *context, const uint8_t *expected_digest, size_t digest_length);
    esp_err_t (*update)(void *context, const uint8_t *data, size_t length);
    esp_err_t (*finish)(void *context);
    void (*abort)(void *context);
} firmware_update_integrity_t;

/**
     * @brief 固件存储与激活接口。
     *
     * Core 不直接调用 ESP-IDF OTA API，更换芯片或启动程序时可替换本接口。
     */
typedef struct
{
    void *context;
    size_t minimum_header_size;
    esp_err_t (*inspect_header)(void *context, const uint8_t *data, size_t length,
                                firmware_update_image_info_t *out_info);
    esp_err_t (*begin)(void *context, size_t image_size, char *out_target, size_t target_capacity);
    esp_err_t (*write)(void *context, const uint8_t *data, size_t length);
    esp_err_t (*finish)(void *context);
    esp_err_t (*activate)(void *context);
    void (*abort)(void *context);
} firmware_update_platform_t;

#ifdef __cplusplus
}
#endif
