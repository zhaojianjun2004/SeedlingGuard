/**
 ******************************************************************************
 * @file    gateway_http.h
 * @brief   局域网监控与控制入口接口定义。
 * @details HTTP Server 任务中处理请求；现场控制只投递命令，结果由查询接口返回。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：应用层
 ******************************************************************************
 */

#pragma once

/*--------------------------- 头文件 ---------------------------*/
#include <stdint.h>

#include "esp_err.h"
#include "gateway_link.h"
#include "gateway_outbox.h"
#include "mqtt_service.h"
#include "wifi_manager.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct gateway_http *gateway_http_handle_t;

typedef struct
{
    gateway_link_handle_t link;
    gateway_outbox_handle_t outbox;
    mqtt_service_handle_t mqtt;
    wifi_manager_handle_t wifi;
    const char *control_token;
    uint16_t port;
} gateway_http_config_t;

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 启动局域网监控与控制入口的运行入口。
 * @param[in] config 模块配置及其取值约束。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t gateway_http_start(const gateway_http_config_t *config,
                             gateway_http_handle_t *out_handle);

#ifdef __cplusplus
}
#endif
