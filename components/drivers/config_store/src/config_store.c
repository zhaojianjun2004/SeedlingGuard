/**
 ******************************************************************************
 * @file    config_store.c
 * @brief   NVS 配置字节存储实现。
 * @details 只管理单个命名空间/键的二进制配置，不解释采集和告警语义。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：设备驱动 / 平台适配层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "config_store.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "nvs.h"

/*--------------------------- 模块配置、类型与状态 ---------------------------*/

struct config_store
{
    char namespace_name[CONFIG_STORE_NAME_MAX_LENGTH + 1U];
    char key[CONFIG_STORE_NAME_MAX_LENGTH + 1U];
};

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 检查 NVS 命名空间或键名的长度约束。
 */
static bool config_store_name_is_valid(const char *name)
{
    if (name == NULL)
    {
        return false;
    }
    const size_t length = strlen(name);
    return (length > 0U) && (length <= CONFIG_STORE_NAME_MAX_LENGTH);
}

/**
 * @brief 创建NVS 配置字节存储实例并准备所需资源。
 * @param[in] namespace_name 接口使用的名称、文本或配置内容，长度受对应模块约束。
 * @param[in] key 接口使用的名称、文本或配置内容，长度受对应模块约束。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t config_store_create(const char *namespace_name, const char *key,
                              config_store_handle_t *out_handle)
{
    if (!config_store_name_is_valid(namespace_name) || !config_store_name_is_valid(key) ||
        (out_handle == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_handle = NULL;
    config_store_handle_t handle = calloc(1U, sizeof(*handle));
    if (handle == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    (void)strcpy(handle->namespace_name, namespace_name);
    (void)strcpy(handle->key, key);
    *out_handle = handle;
    return ESP_OK;
}

/**
 * @brief 读取配置二进制内容，并报告实际存储长度。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_data 接收本次操作的输出数据。
 * @param[in] capacity 输出缓冲区容量，单位字节。
 * @param[out] out_stored_size 接收本次操作的输出数据。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t config_store_load(config_store_handle_t handle, void *out_data, size_t capacity,
                            size_t *out_stored_size)
{
    if ((handle == NULL) || (out_data == NULL) || (capacity == 0U) || (out_stored_size == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_stored_size = 0U;
    nvs_handle_t nvs_handle;
    esp_err_t result = nvs_open(handle->namespace_name, NVS_READONLY, &nvs_handle);
    if (result != ESP_OK)
    {
        return (result == ESP_ERR_NVS_NOT_FOUND) ? ESP_ERR_NOT_FOUND : result;
    }

    size_t stored_size = 0U;
    result = nvs_get_blob(nvs_handle, handle->key, NULL, &stored_size);
    if (result == ESP_OK)
    {
        *out_stored_size = stored_size;
        if (stored_size > capacity)
        {
            result = ESP_ERR_INVALID_SIZE;
        }
        else
        {
            size_t read_size = capacity;
            result = nvs_get_blob(nvs_handle, handle->key, out_data, &read_size);
            if (result == ESP_OK)
            {
                *out_stored_size = read_size;
            }
        }
    }

    nvs_close(nvs_handle);
    return (result == ESP_ERR_NVS_NOT_FOUND) ? ESP_ERR_NOT_FOUND : result;
}

/**
 * @brief 写入配置 Blob 并提交 NVS。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[in] data 输入字节缓冲区。
 * @param[in] data_size 待保存二进制内容的长度，单位字节。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t config_store_save(config_store_handle_t handle, const void *data, size_t data_size)
{
    if ((handle == NULL) || (data == NULL) || (data_size == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t result = nvs_open(handle->namespace_name, NVS_READWRITE, &nvs_handle);
    if (result != ESP_OK)
    {
        return result;
    }

    result = nvs_set_blob(nvs_handle, handle->key, data, data_size);
    if (result == ESP_OK)
    {
        result = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    return result;
}

/**
 * @brief 删除配置键并提交，不存在的键视为已清除。
 * @param[in] handle 已创建的模块实例句柄。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t config_store_erase(config_store_handle_t handle)
{
    if (handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t result = nvs_open(handle->namespace_name, NVS_READWRITE, &nvs_handle);
    if (result != ESP_OK)
    {
        return result;
    }

    result = nvs_erase_key(nvs_handle, handle->key);
    if (result == ESP_ERR_NVS_NOT_FOUND)
    {
        result = ESP_OK;
    }
    if (result == ESP_OK)
    {
        result = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    return result;
}

/**
 * @brief 释放配置存储实例，保留 Flash 中的数据。
 * @param[in] handle 已创建的模块实例句柄。
 */
void config_store_delete(config_store_handle_t handle)
{
    free(handle);
}
