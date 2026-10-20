/**
 ******************************************************************************
 * @file    update_manifest_json.c
 * @brief   升级清单 JSON 编解码实现。
 * @details 将受限 JSON 文档转换为公共清单结构，产品/硬件/版本准入由升级服务决定。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：协议层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "update_manifest_json.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

/*--------------------------- 模块配置、类型与状态 ---------------------------*/

#define UPDATE_MANIFEST_MIN_DOCUMENT_SIZE (128U)
#define UPDATE_MANIFEST_SCHEMA_VERSION    (1U)

static const char *TAG = "manifest_json";

struct update_manifest_json
{
    update_manifest_json_config_t config;
};

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 统计对象中的同名字段以识别重复键。
 */
static size_t update_manifest_count_key(const cJSON *object, const char *key)
{
    size_t count = 0U;
    for (const cJSON *item = object->child; item != NULL; item = item->next)
    {
        if ((item->string != NULL) && (strcmp(item->string, key) == 0))
        {
            ++count;
        }
    }
    return count;
}

/**
 * @brief 取得唯一的清单字段，拒绝缺失或重复的键。
 */
static const cJSON *update_manifest_get_unique(const cJSON *object, const char *key)
{
    if (update_manifest_count_key(object, key) != 1U)
    {
        ESP_LOGE(TAG, "Manifest field '%s' is missing or duplicated", key);
        return NULL;
    }
    return cJSON_GetObjectItemCaseSensitive(object, key);
}

/**
 * @brief 校验并复制长度受限的清单字符串。
 */
static bool update_manifest_copy_string(const cJSON *object, const char *key, char *destination,
                                        size_t capacity)
{
    const cJSON *item = update_manifest_get_unique(object, key);
    if (!cJSON_IsString(item) || (item->valuestring == NULL) || (item->valuestring[0] == '\0'))
    {
        ESP_LOGE(TAG, "Manifest field '%s' must be a non-empty string", key);
        return false;
    }

    const size_t length = strnlen(item->valuestring, capacity);
    if (length >= capacity)
    {
        ESP_LOGE(TAG, "Manifest field '%s' is too long", key);
        return false;
    }
    (void)memcpy(destination, item->valuestring, length + 1U);
    return true;
}

/**
 * @brief 解析范围合法的无符号整数清单字段。
 */
static bool update_manifest_read_u32(const cJSON *object, const char *key, uint32_t *out_value)
{
    const cJSON *item = update_manifest_get_unique(object, key);
    if (!cJSON_IsNumber(item) || (item->valuedouble < 0.0) ||
        (item->valuedouble > (double)UINT32_MAX))
    {
        ESP_LOGE(TAG, "Manifest field '%s' must be uint32", key);
        return false;
    }

    const uint32_t value = (uint32_t)item->valuedouble;
    if ((double)value != item->valuedouble)
    {
        ESP_LOGE(TAG, "Manifest field '%s' must be an integer", key);
        return false;
    }
    *out_value = value;
    return true;
}

/**
 * @brief 将一个十六进制字符转换为四位数值。
 */
static int update_manifest_hex_nibble(char value)
{
    if ((value >= '0') && (value <= '9'))
    {
        return value - '0';
    }
    if ((value >= 'a') && (value <= 'f'))
    {
        return value - 'a' + 10;
    }
    if ((value >= 'A') && (value <= 'F'))
    {
        return value - 'A' + 10;
    }
    return -1;
}

/**
 * @brief 将清单中的十六进制 SHA-256 字符串解析为字节数组。
 */
static bool update_manifest_read_sha256(const cJSON *object, uint8_t *out_digest)
{
    const cJSON *item = update_manifest_get_unique(object, "sha256");
    const size_t expected_hex_length = FIRMWARE_UPDATE_SHA256_LENGTH * 2U;
    if (!cJSON_IsString(item) || (item->valuestring == NULL) ||
        (strlen(item->valuestring) != expected_hex_length))
    {
        ESP_LOGE(TAG, "Manifest sha256 must contain exactly 64 hex digits");
        return false;
    }

    for (size_t index = 0U; index < FIRMWARE_UPDATE_SHA256_LENGTH; ++index)
    {
        const int high = update_manifest_hex_nibble(item->valuestring[index * 2U]);
        const int low = update_manifest_hex_nibble(item->valuestring[index * 2U + 1U]);
        if ((high < 0) || (low < 0))
        {
            ESP_LOGE(TAG, "Manifest sha256 contains a non-hex character");
            return false;
        }
        out_digest[index] = (uint8_t)((high << 4) | low);
    }
    return true;
}

/**
 * @brief 确认 JSON 文档后只有允许的空白字符。
 */
static bool update_manifest_has_only_trailing_space(const char *cursor, const char *end)
{
    while (cursor < end)
    {
        if (!isspace((unsigned char)*cursor))
        {
            return false;
        }
        ++cursor;
    }
    return true;
}

/**
 * @brief 解析完整清单，检查字段类型、重复键和摘要格式。
 */
static esp_err_t update_manifest_json_parse(void *context, const uint8_t *document,
                                            size_t document_length,
                                            firmware_update_manifest_t *out_manifest)
{
    update_manifest_json_handle_t handle = (update_manifest_json_handle_t)context;
    if ((handle == NULL) || (document == NULL) || (document_length == 0U) || (out_manifest == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (document_length > handle->config.maximum_document_size)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    const char *parse_end = NULL;
    cJSON *root =
        cJSON_ParseWithLengthOpts((const char *)document, document_length, &parse_end, false);
    const char *document_end = (const char *)document + document_length;
    if ((root == NULL) || !cJSON_IsObject(root) || (parse_end == NULL) ||
        !update_manifest_has_only_trailing_space(parse_end, document_end))
    {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Manifest is not one complete JSON object");
        return ESP_ERR_INVALID_RESPONSE;
    }

    firmware_update_manifest_t manifest = {0};
    uint32_t image_size_u32 = 0U;
    const bool valid =
        update_manifest_read_u32(root, "schema_version", &manifest.schema_version) &&
        (manifest.schema_version == UPDATE_MANIFEST_SCHEMA_VERSION) &&
        update_manifest_copy_string(root, "product", manifest.product, sizeof(manifest.product)) &&
        update_manifest_copy_string(root, "hardware", manifest.hardware,
                                    sizeof(manifest.hardware)) &&
        update_manifest_copy_string(root, "version", manifest.version, sizeof(manifest.version)) &&
        update_manifest_read_u32(root, "release_sequence", &manifest.release_sequence) &&
        (manifest.release_sequence > 0U) &&
        update_manifest_copy_string(root, "image_url", manifest.image_uri,
                                    sizeof(manifest.image_uri)) &&
        update_manifest_read_u32(root, "image_size", &image_size_u32) && (image_size_u32 > 0U) &&
        update_manifest_read_sha256(root, manifest.sha256);

    cJSON_Delete(root);
    if (!valid)
    {
        if (manifest.schema_version != UPDATE_MANIFEST_SCHEMA_VERSION)
        {
            ESP_LOGE(TAG, "Unsupported Manifest schema version: %u",
                     (unsigned int)manifest.schema_version);
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    manifest.image_size = (size_t)image_size_u32;
    *out_manifest = manifest;
    return ESP_OK;
}

/**
 * @brief 创建升级清单 JSON 编解码实例并准备所需资源。
 * @param[in] config 模块配置及其取值约束。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t update_manifest_json_create(const update_manifest_json_config_t *config,
                                      update_manifest_json_handle_t *out_handle)
{
    if ((config == NULL) || (config->maximum_document_size < UPDATE_MANIFEST_MIN_DOCUMENT_SIZE) ||
        (out_handle == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_handle = calloc(1U, sizeof(**out_handle));
    if (*out_handle == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    (*out_handle)->config = *config;
    return ESP_OK;
}

/**
 * @brief 导出升级清单 JSON 编解码的函数接口供升级事务调用。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_parser 接收本次操作的输出数据。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t update_manifest_json_get_interface(update_manifest_json_handle_t handle,
                                             firmware_update_manifest_parser_t *out_parser)
{
    if ((handle == NULL) || (out_parser == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_parser = (firmware_update_manifest_parser_t){
        .context = handle,
        .maximum_document_size = handle->config.maximum_document_size,
        .parse = update_manifest_json_parse,
    };
    return ESP_OK;
}
