/**
 * @file update_manifest_json.c
 * @brief OTA JSON Manifest 的严格字段、长度和数值校验实现。
 */

#include "update_manifest_json.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#define UPDATE_MANIFEST_MIN_DOCUMENT_SIZE (128U)
#define UPDATE_MANIFEST_SCHEMA_VERSION    (1U)

static const char *TAG = "manifest_json";

struct update_manifest_json {
    update_manifest_json_config_t config;
};

static size_t update_manifest_count_key(const cJSON *object,
                                        const char *key)
{
    size_t count = 0U;
    for (const cJSON *item = object->child;
         item != NULL;
         item = item->next) {
        if ((item->string != NULL) && (strcmp(item->string, key) == 0)) {
            ++count;
        }
    }
    return count;
}

static const cJSON *update_manifest_get_unique(const cJSON *object,
                                                const char *key)
{
    if (update_manifest_count_key(object, key) != 1U) {
        ESP_LOGE(TAG, "Manifest field '%s' is missing or duplicated", key);
        return NULL;
    }
    return cJSON_GetObjectItemCaseSensitive(object, key);
}

static bool update_manifest_copy_string(const cJSON *object,
                                        const char *key,
                                        char *destination,
                                        size_t capacity)
{
    const cJSON *item = update_manifest_get_unique(object, key);
    if (!cJSON_IsString(item) || (item->valuestring == NULL) ||
        (item->valuestring[0] == '\0')) {
        ESP_LOGE(TAG, "Manifest field '%s' must be a non-empty string", key);
        return false;
    }

    const size_t length = strnlen(item->valuestring, capacity);
    if (length >= capacity) {
        ESP_LOGE(TAG, "Manifest field '%s' is too long", key);
        return false;
    }
    (void)memcpy(destination, item->valuestring, length + 1U);
    return true;
}

static bool update_manifest_read_u32(const cJSON *object,
                                     const char *key,
                                     uint32_t *out_value)
{
    const cJSON *item = update_manifest_get_unique(object, key);
    if (!cJSON_IsNumber(item) || (item->valuedouble < 0.0) ||
        (item->valuedouble > (double)UINT32_MAX)) {
        ESP_LOGE(TAG, "Manifest field '%s' must be uint32", key);
        return false;
    }

    const uint32_t value = (uint32_t)item->valuedouble;
    if ((double)value != item->valuedouble) {
        ESP_LOGE(TAG, "Manifest field '%s' must be an integer", key);
        return false;
    }
    *out_value = value;
    return true;
}

static int update_manifest_hex_nibble(char value)
{
    if ((value >= '0') && (value <= '9')) {
        return value - '0';
    }
    if ((value >= 'a') && (value <= 'f')) {
        return value - 'a' + 10;
    }
    if ((value >= 'A') && (value <= 'F')) {
        return value - 'A' + 10;
    }
    return -1;
}

static bool update_manifest_read_sha256(const cJSON *object,
                                        uint8_t *out_digest)
{
    const cJSON *item = update_manifest_get_unique(object, "sha256");
    const size_t expected_hex_length = FIRMWARE_UPDATE_SHA256_LENGTH * 2U;
    if (!cJSON_IsString(item) || (item->valuestring == NULL) ||
        (strlen(item->valuestring) != expected_hex_length)) {
        ESP_LOGE(TAG, "Manifest sha256 must contain exactly 64 hex digits");
        return false;
    }

    for (size_t index = 0U;
         index < FIRMWARE_UPDATE_SHA256_LENGTH;
         ++index) {
        const int high = update_manifest_hex_nibble(
            item->valuestring[index * 2U]);
        const int low = update_manifest_hex_nibble(
            item->valuestring[index * 2U + 1U]);
        if ((high < 0) || (low < 0)) {
            ESP_LOGE(TAG, "Manifest sha256 contains a non-hex character");
            return false;
        }
        out_digest[index] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static bool update_manifest_has_only_trailing_space(const char *cursor,
                                                     const char *end)
{
    while (cursor < end) {
        if (!isspace((unsigned char)*cursor)) {
            return false;
        }
        ++cursor;
    }
    return true;
}

static esp_err_t update_manifest_json_parse(
    void *context,
    const uint8_t *document,
    size_t document_length,
    firmware_update_manifest_t *out_manifest)
{
    update_manifest_json_handle_t handle =
        (update_manifest_json_handle_t)context;
    if ((handle == NULL) || (document == NULL) ||
        (document_length == 0U) || (out_manifest == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (document_length > handle->config.maximum_document_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts((const char *)document,
                                            document_length,
                                            &parse_end,
                                            false);
    const char *document_end = (const char *)document + document_length;
    if ((root == NULL) || !cJSON_IsObject(root) ||
        (parse_end == NULL) ||
        !update_manifest_has_only_trailing_space(parse_end, document_end)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Manifest is not one complete JSON object");
        return ESP_ERR_INVALID_RESPONSE;
    }

    firmware_update_manifest_t manifest = {0};
    uint32_t image_size_u32 = 0U;
    const bool valid =
        update_manifest_read_u32(root, "schema_version",
                                 &manifest.schema_version) &&
        (manifest.schema_version == UPDATE_MANIFEST_SCHEMA_VERSION) &&
        update_manifest_copy_string(root, "product",
                                    manifest.product,
                                    sizeof(manifest.product)) &&
        update_manifest_copy_string(root, "hardware",
                                    manifest.hardware,
                                    sizeof(manifest.hardware)) &&
        update_manifest_copy_string(root, "version",
                                    manifest.version,
                                    sizeof(manifest.version)) &&
        update_manifest_read_u32(root, "release_sequence",
                                 &manifest.release_sequence) &&
        (manifest.release_sequence > 0U) &&
        update_manifest_copy_string(root, "image_url",
                                    manifest.image_uri,
                                    sizeof(manifest.image_uri)) &&
        update_manifest_read_u32(root, "image_size", &image_size_u32) &&
        (image_size_u32 > 0U) &&
        update_manifest_read_sha256(root, manifest.sha256);

    cJSON_Delete(root);
    if (!valid) {
        if (manifest.schema_version != UPDATE_MANIFEST_SCHEMA_VERSION) {
            ESP_LOGE(TAG, "Unsupported Manifest schema version: %u",
                     (unsigned int)manifest.schema_version);
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    manifest.image_size = (size_t)image_size_u32;
    *out_manifest = manifest;
    return ESP_OK;
}

esp_err_t update_manifest_json_create(
    const update_manifest_json_config_t *config,
    update_manifest_json_handle_t *out_handle)
{
    if ((config == NULL) ||
        (config->maximum_document_size < UPDATE_MANIFEST_MIN_DOCUMENT_SIZE) ||
        (out_handle == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_handle = calloc(1U, sizeof(**out_handle));
    if (*out_handle == NULL) {
        return ESP_ERR_NO_MEM;
    }
    (*out_handle)->config = *config;
    return ESP_OK;
}

esp_err_t update_manifest_json_get_interface(
    update_manifest_json_handle_t handle,
    firmware_update_manifest_parser_t *out_parser)
{
    if ((handle == NULL) || (out_parser == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_parser = (firmware_update_manifest_parser_t) {
        .context = handle,
        .maximum_document_size = handle->config.maximum_document_size,
        .parse = update_manifest_json_parse,
    };
    return ESP_OK;
}
