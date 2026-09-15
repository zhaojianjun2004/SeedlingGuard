/**
 * @file config_store.c
 * @brief NVS Blob 存储实现。
 */

#include "config_store.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "nvs.h"

struct config_store {
    char namespace_name[CONFIG_STORE_NAME_MAX_LENGTH + 1U];
    char key[CONFIG_STORE_NAME_MAX_LENGTH + 1U];
};

static bool config_store_name_is_valid(const char *name)
{
    if (name == NULL) {
        return false;
    }
    const size_t length = strlen(name);
    return (length > 0U) && (length <= CONFIG_STORE_NAME_MAX_LENGTH);
}

esp_err_t config_store_create(const char *namespace_name,
                              const char *key,
                              config_store_handle_t *out_handle)
{
    if (!config_store_name_is_valid(namespace_name) ||
        !config_store_name_is_valid(key) ||
        (out_handle == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_handle = NULL;
    config_store_handle_t handle = calloc(1U, sizeof(*handle));
    if (handle == NULL) {
        return ESP_ERR_NO_MEM;
    }

    (void)strcpy(handle->namespace_name, namespace_name);
    (void)strcpy(handle->key, key);
    *out_handle = handle;
    return ESP_OK;
}

esp_err_t config_store_load(config_store_handle_t handle,
                            void *out_data,
                            size_t capacity,
                            size_t *out_stored_size)
{
    if ((handle == NULL) || (out_data == NULL) ||
        (capacity == 0U) || (out_stored_size == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_stored_size = 0U;
    nvs_handle_t nvs_handle;
    esp_err_t result = nvs_open(handle->namespace_name,
                                NVS_READONLY,
                                &nvs_handle);
    if (result != ESP_OK) {
        return (result == ESP_ERR_NVS_NOT_FOUND) ? ESP_ERR_NOT_FOUND : result;
    }

    size_t stored_size = 0U;
    result = nvs_get_blob(nvs_handle, handle->key, NULL, &stored_size);
    if (result == ESP_OK) {
        *out_stored_size = stored_size;
        if (stored_size > capacity) {
            result = ESP_ERR_INVALID_SIZE;
        } else {
            size_t read_size = capacity;
            result = nvs_get_blob(nvs_handle,
                                  handle->key,
                                  out_data,
                                  &read_size);
            if (result == ESP_OK) {
                *out_stored_size = read_size;
            }
        }
    }

    nvs_close(nvs_handle);
    return (result == ESP_ERR_NVS_NOT_FOUND) ? ESP_ERR_NOT_FOUND : result;
}

esp_err_t config_store_save(config_store_handle_t handle,
                            const void *data,
                            size_t data_size)
{
    if ((handle == NULL) || (data == NULL) || (data_size == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t result = nvs_open(handle->namespace_name,
                                NVS_READWRITE,
                                &nvs_handle);
    if (result != ESP_OK) {
        return result;
    }

    result = nvs_set_blob(nvs_handle, handle->key, data, data_size);
    if (result == ESP_OK) {
        result = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    return result;
}

esp_err_t config_store_erase(config_store_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t result = nvs_open(handle->namespace_name,
                                NVS_READWRITE,
                                &nvs_handle);
    if (result != ESP_OK) {
        return result;
    }

    result = nvs_erase_key(nvs_handle, handle->key);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        result = ESP_OK;
    }
    if (result == ESP_OK) {
        result = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    return result;
}

void config_store_delete(config_store_handle_t handle)
{
    free(handle);
}
