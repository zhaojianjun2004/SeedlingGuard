/**
 ******************************************************************************
 * @file    update_transport_http.c
 * @brief   HTTP 升级字节流适配实现。
 * @details 下载过程在升级任务执行；返回字节流及长度，不负责清单和镜像业务校验。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：设备驱动 / 平台适配层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "update_transport_http.h"

#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"

/*--------------------------- 模块配置、类型与状态 ---------------------------*/

#define UPDATE_HTTP_MIN_TIMEOUT_MS     (1000U)
#define UPDATE_HTTP_MIN_RX_BUFFER_SIZE (512U)

static const char *TAG = "update_http";

struct update_transport_http
{
    update_transport_http_config_t config;
    esp_http_client_handle_t client;
    bool opened;
};

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 校验 HTTP 超时与接收缓冲大小。
 */
static bool update_transport_http_config_is_valid(const update_transport_http_config_t *config)
{
    return (config != NULL) && (config->timeout_ms >= UPDATE_HTTP_MIN_TIMEOUT_MS) &&
           (config->receive_buffer_size >= UPDATE_HTTP_MIN_RX_BUFFER_SIZE);
}

/**
 * @brief 检查升级 URI 是否使用允许的 HTTP/HTTPS 格式。
 */
static bool update_transport_http_uri_is_allowed(const char *uri)
{
    /*
     * 本阶段使用电脑局域网 HTTP Server，故明确只接受 http://。
     * 量产方案必须增加服务器身份校验、签名验证和密钥生命周期设计，不能
     * 简单地把这里改成跳过 HTTPS 主机名或证书检查。
     */
    return (uri != NULL) && (strncmp(uri, "http://", 7U) == 0);
}

/**
 * @brief 关闭并释放当前 HTTP 连接。
 */
static void update_transport_http_close_internal(update_transport_http_handle_t handle)
{
    if (handle->client != NULL)
    {
        if (handle->opened)
        {
            (void)esp_http_client_close(handle->client);
        }
        (void)esp_http_client_cleanup(handle->client);
    }
    handle->client = NULL;
    handle->opened = false;
}

/**
 * @brief 打开升级下载连接并取得内容长度。
 */
static esp_err_t update_transport_http_open(void *context, const char *uri,
                                            size_t *out_content_length)
{
    update_transport_http_handle_t handle = (update_transport_http_handle_t)context;
    if ((handle == NULL) || (out_content_length == NULL) ||
        !update_transport_http_uri_is_allowed(uri))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->client != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_http_client_config_t http_config = {
        .url = uri,
        .timeout_ms = (int)handle->config.timeout_ms,
        .buffer_size = (int)handle->config.receive_buffer_size,
        .keep_alive_enable = true,
        .disable_auto_redirect = false,
        .max_redirection_count = 3,
    };
    handle->client = esp_http_client_init(&http_config);
    if (handle->client == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = esp_http_client_open(handle->client, 0);
    if (result != ESP_OK)
    {
        update_transport_http_close_internal(handle);
        return result;
    }
    handle->opened = true;

    const int64_t content_length = esp_http_client_fetch_headers(handle->client);
    if ((content_length <= 0) || ((uint64_t)content_length > (uint64_t)SIZE_MAX))
    {
        ESP_LOGE(TAG, "Server did not provide a valid Content-Length");
        update_transport_http_close_internal(handle);
        return ESP_ERR_INVALID_SIZE;
    }

    const int status_code = esp_http_client_get_status_code(handle->client);
    if (status_code != 200)
    {
        ESP_LOGE(TAG, "HTTP server returned status %d", status_code);
        update_transport_http_close_internal(handle);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_content_length = (size_t)content_length;
    ESP_LOGI(TAG, "HTTP stream opened: %u bytes", (unsigned int)*out_content_length);
    return ESP_OK;
}

/**
 * @brief 读取一块 HTTP 内容并报告流是否结束。
 */
static esp_err_t update_transport_http_read(void *context, uint8_t *buffer, size_t capacity,
                                            size_t *out_length, bool *out_complete)
{
    update_transport_http_handle_t handle = (update_transport_http_handle_t)context;
    if ((handle == NULL) || (buffer == NULL) || (capacity == 0U) || (out_length == NULL) ||
        (out_complete == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->opened || (handle->client == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    const int read_result = esp_http_client_read(handle->client, (char *)buffer, (int)capacity);
    if (read_result < 0)
    {
        return (read_result == -ESP_ERR_HTTP_EAGAIN) ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }

    *out_length = (size_t)read_result;
    *out_complete = esp_http_client_is_complete_data_received(handle->client);
    return ESP_OK;
}

/**
 * @brief 关闭升级字节流。
 */
static void update_transport_http_close(void *context)
{
    update_transport_http_handle_t handle = (update_transport_http_handle_t)context;
    if (handle != NULL)
    {
        update_transport_http_close_internal(handle);
    }
}

/**
 * @brief 创建HTTP 升级字节流适配实例并准备所需资源。
 * @param[in] config 模块配置及其取值约束。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t update_transport_http_create(const update_transport_http_config_t *config,
                                       update_transport_http_handle_t *out_handle)
{
    if (!update_transport_http_config_is_valid(config) || (out_handle == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_handle = NULL;
    update_transport_http_handle_t handle = calloc(1U, sizeof(*handle));
    if (handle == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    handle->config = *config;
    *out_handle = handle;
    return ESP_OK;
}

/**
 * @brief 导出HTTP 升级字节流适配的函数接口供升级事务调用。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_transport 接收本次操作的输出数据。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t update_transport_http_get_interface(update_transport_http_handle_t handle,
                                              firmware_update_transport_t *out_transport)
{
    if ((handle == NULL) || (out_transport == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_transport = (firmware_update_transport_t){
        .context = handle,
        .open = update_transport_http_open,
        .read = update_transport_http_read,
        .close = update_transport_http_close,
    };
    return ESP_OK;
}
