/**
 * @file update_integrity_sha256.c
 * @brief 基于 Mbed TLS 的流式 SHA-256 校验实现。
 */

#include "update_integrity_sha256.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "mbedtls/sha256.h"

static const char *TAG = "integrity_sha256";

struct update_integrity_sha256 {
    mbedtls_sha256_context sha256;
    uint8_t expected[FIRMWARE_UPDATE_SHA256_LENGTH];
    bool active;
};

static void update_integrity_sha256_reset(
    update_integrity_sha256_handle_t handle)
{
    mbedtls_sha256_free(&handle->sha256);
    mbedtls_sha256_init(&handle->sha256);
    (void)memset(handle->expected, 0, sizeof(handle->expected));
    handle->active = false;
}

static esp_err_t update_integrity_sha256_begin(void *context,
                                               const uint8_t *expected_digest,
                                               size_t digest_length)
{
    update_integrity_sha256_handle_t handle =
        (update_integrity_sha256_handle_t)context;
    if ((handle == NULL) || (expected_digest == NULL) ||
        (digest_length != FIRMWARE_UPDATE_SHA256_LENGTH)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->active) {
        return ESP_ERR_INVALID_STATE;
    }

    update_integrity_sha256_reset(handle);
    if (mbedtls_sha256_starts(&handle->sha256, 0) != 0) {
        return ESP_FAIL;
    }
    (void)memcpy(handle->expected,
                 expected_digest,
                 sizeof(handle->expected));
    handle->active = true;
    return ESP_OK;
}

static esp_err_t update_integrity_sha256_update(void *context,
                                                const uint8_t *data,
                                                size_t length)
{
    update_integrity_sha256_handle_t handle =
        (update_integrity_sha256_handle_t)context;
    if ((handle == NULL) || (data == NULL) || (length == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->active) {
        return ESP_ERR_INVALID_STATE;
    }

    return (mbedtls_sha256_update(&handle->sha256, data, length) == 0)
        ? ESP_OK
        : ESP_FAIL;
}

static esp_err_t update_integrity_sha256_finish(void *context)
{
    update_integrity_sha256_handle_t handle =
        (update_integrity_sha256_handle_t)context;
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->active) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t calculated[FIRMWARE_UPDATE_SHA256_LENGTH] = {0};
    const int finish_result = mbedtls_sha256_finish(&handle->sha256,
                                                    calculated);
    uint8_t difference = 0U;
    for (size_t index = 0U;
         index < FIRMWARE_UPDATE_SHA256_LENGTH;
         ++index) {
        difference |= (uint8_t)(calculated[index] ^ handle->expected[index]);
    }
    update_integrity_sha256_reset(handle);

    if (finish_result != 0) {
        return ESP_FAIL;
    }
    if (difference != 0U) {
        ESP_LOGE(TAG, "Firmware SHA-256 does not match Manifest");
        return ESP_ERR_INVALID_CRC;
    }

    ESP_LOGI(TAG, "Firmware SHA-256 verified");
    return ESP_OK;
}

static void update_integrity_sha256_abort(void *context)
{
    update_integrity_sha256_handle_t handle =
        (update_integrity_sha256_handle_t)context;
    if (handle != NULL) {
        update_integrity_sha256_reset(handle);
    }
}

esp_err_t update_integrity_sha256_create(
    update_integrity_sha256_handle_t *out_handle)
{
    if (out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_handle = calloc(1U, sizeof(**out_handle));
    if (*out_handle == NULL) {
        return ESP_ERR_NO_MEM;
    }
    mbedtls_sha256_init(&(*out_handle)->sha256);
    return ESP_OK;
}

esp_err_t update_integrity_sha256_get_interface(
    update_integrity_sha256_handle_t handle,
    firmware_update_integrity_t *out_integrity)
{
    if ((handle == NULL) || (out_integrity == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_integrity = (firmware_update_integrity_t) {
        .context = handle,
        .begin = update_integrity_sha256_begin,
        .update = update_integrity_sha256_update,
        .finish = update_integrity_sha256_finish,
        .abort = update_integrity_sha256_abort,
    };
    return ESP_OK;
}
