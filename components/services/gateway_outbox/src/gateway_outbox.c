/**
 ******************************************************************************
 * @file    gateway_outbox.c
 * @brief   持久待发队列服务实现。
 * @details 存储任务独占运行期 NVS 操作；仅业务匹配的 PUBACK 才允许删除记录。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：业务服务层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "gateway_outbox.h"

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

/*--------------------------- 模块配置、类型与状态 ---------------------------*/

#define GATEWAY_OUTBOX_SCHEMA_VERSION 4U
#define GATEWAY_OUTBOX_NAMESPACE      "records"

static const char *TAG = "gateway_outbox";
_Static_assert(offsetof(gateway_outbox_record_t, points) == 20, "legacy metadata layout changed");
_Static_assert(sizeof(gateway_outbox_point_t) == 12, "legacy point layout changed");

typedef struct
{
    uint32_t version;
    gateway_outbox_record_t record;
} stored_record_t;

struct gateway_outbox
{
    uint32_t event_cursor;
    bool faulted;
    QueueHandle_t jobs;
    nvs_handle_t nvs;
    SemaphoreHandle_t mutex;
    uint32_t slot_id[GATEWAY_OUTBOX_CAPACITY];
    gateway_outbox_status_t status;
};

typedef struct
{
    unsigned op;
    gateway_outbox_record_t record;
    uint32_t id;
    esp_err_t result;
    SemaphoreHandle_t done;
} outbox_job_t;

static esp_err_t outbox_append_record(gateway_outbox_handle_t, const gateway_outbox_record_t *,
                                      uint32_t *);
static esp_err_t outbox_read_oldest(gateway_outbox_handle_t, gateway_outbox_record_t *);
static esp_err_t outbox_remove_confirmed(gateway_outbox_handle_t, uint32_t);

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 独占 NVS，串行执行待发队列的追加、读取和确认删除。
 * @note 任务入口，只在任务上下文运行；正常情况下不返回。
 */
static void outbox_storage_task(void *context)
{
    gateway_outbox_handle_t handle = context;
    outbox_job_t *job;
    for (;;)
    {
        if (xQueueReceive(handle->jobs, &job, portMAX_DELAY) == pdTRUE)
        {
            job->result = job->op == 1   ? outbox_append_record(handle, &job->record, &job->id)
                          : job->op == 2 ? outbox_read_oldest(handle, &job->record)
                                         : outbox_remove_confirmed(handle, job->id);
            xSemaphoreGive(job->done);
        }
    }
}

/**
 * @brief 生成待发槽位对应的 NVS 键名。
 */
static void outbox_slot_key(unsigned slot_index, char key[8])
{
    (void)snprintf(key, 8U, "r%03u", slot_index);
}

/**
 * @brief 查找记录号最小的有效待发槽。
 */
static int outbox_oldest_slot(const gateway_outbox_handle_t handle)
{
    int oldest = -1;
    for (unsigned slot_index = 0U; slot_index < GATEWAY_OUTBOX_CAPACITY; ++slot_index)
    {
        if (handle->slot_id[slot_index] != 0U &&
            (oldest < 0 || handle->slot_id[slot_index] < handle->slot_id[oldest]))
        {
            oldest = (int)slot_index;
        }
    }
    return oldest;
}

/**
 * @brief 创建持久待发队列服务实例并准备所需资源。
 * @param[in] partition 专用 NVS 分区名称，本项目使用 outbox。
 * @param[out] out_handle 接收创建成功的实例句柄，由调用方保存。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t gateway_outbox_create(const char *partition, gateway_outbox_handle_t *out_handle)
{
    if (partition == NULL || partition[0] == '\0' || out_handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    esp_err_t error = nvs_flash_init_partition(partition);
    if (error != ESP_OK)
    {
        return error;
    }
    gateway_outbox_handle_t handle = calloc(1U, sizeof(*handle));
    if (handle == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    handle->mutex = xSemaphoreCreateMutex();
    if (handle->mutex == NULL)
    {
        free(handle);
        return ESP_ERR_NO_MEM;
    }
    error =
        nvs_open_from_partition(partition, GATEWAY_OUTBOX_NAMESPACE, NVS_READWRITE, &handle->nvs);
    if (error != ESP_OK)
    {
        vSemaphoreDelete(handle->mutex);
        free(handle);
        return error;
    }

    uint32_t largest_id = 0U;
    for (unsigned i = 0U; i < GATEWAY_OUTBOX_CAPACITY; ++i)
    {
        char key[8];
        outbox_slot_key(i, key);
        stored_record_t stored;
        size_t length = sizeof(stored);
        error = nvs_get_blob(handle->nvs, key, &stored, &length);
        if (error == ESP_ERR_NVS_NOT_FOUND)
        {
            continue;
        }
        if (error == ESP_ERR_NVS_INVALID_LENGTH)
        {
            error = ESP_ERR_INVALID_VERSION;
            break;
        }
        if (error != ESP_OK)
        {
            break;
        }
        if (length == 60U && stored.version == 2U)
        {
            struct
            {
                uint32_t version;
                uint8_t record[56];
            } legacy;

            memcpy(&legacy, &stored, sizeof(legacy));
            memset(&stored, 0, sizeof(stored));
            stored.version = GATEWAY_OUTBOX_SCHEMA_VERSION;
            memcpy(&stored.record, legacy.record, 20);
            for (unsigned n = 0; n < 3; n++)
            {
                memcpy(&stored.record.points[n], legacy.record + 20 + n * 12, 9);
                stored.record.points[n].alarm_state = 3;
            }
            for (unsigned n = 3; n < GATEWAY_OUTBOX_POINT_COUNT; n++)
            {
                stored.record.points[n].sample_age_ms = UINT32_MAX;
            }
            error = nvs_set_blob(handle->nvs, key, &stored, sizeof(stored));
            if (error == ESP_OK)
            {
                error = nvs_commit(handle->nvs);
            }
            if (error != ESP_OK)
            {
                break;
            }
            length = sizeof(stored);
        }
        if (length != sizeof(stored) || stored.version != GATEWAY_OUTBOX_SCHEMA_VERSION)
        {
            ESP_LOGE(TAG, "Unsupported outbox schema in slot %u; preserve data for recovery", i);
            error = ESP_ERR_INVALID_VERSION;
            break;
        }
        if (stored.version != GATEWAY_OUTBOX_SCHEMA_VERSION || stored.record.record_id == 0U)
        {
            ESP_LOGW(TAG, "Ignoring incompatible record in slot %u", i);
            continue;
        }
        handle->slot_id[i] = stored.record.record_id;
        ++handle->status.pending_count;
        if (stored.record.record_id > largest_id)
        {
            largest_id = stored.record.record_id;
        }
    }
    if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND && error != ESP_ERR_NVS_INVALID_LENGTH)
    {
        nvs_close(handle->nvs);
        vSemaphoreDelete(handle->mutex);
        free(handle);
        return error;
    }
    uint32_t saved_next_id = 0U;
    if (nvs_get_u32(handle->nvs, "next_id", &saved_next_id) != ESP_OK)
    {
        saved_next_id = 0U;
    }
    const uint32_t scanned_next_id = largest_id == UINT32_MAX ? UINT32_MAX : largest_id + 1U;
    handle->status.next_record_id =
        saved_next_id > scanned_next_id ? saved_next_id : scanned_next_id;
    (void)nvs_get_u32(handle->nvs, "dropped", &handle->status.dropped_count);
    (void)nvs_get_u32(handle->nvs, "event_cursor", &handle->event_cursor);
    handle->jobs = xQueueCreate(8, sizeof(outbox_job_t *));
    if (!handle->jobs ||
        xTaskCreate(outbox_storage_task, "gateway_storage", 4096, handle, 4, NULL) != pdPASS)
    {
        if (handle->jobs)
        {
            vQueueDelete(handle->jobs);
        }
        nvs_close(handle->nvs);
        vSemaphoreDelete(handle->mutex);
        free(handle);
        return ESP_ERR_NO_MEM;
    }
    *out_handle = handle;
    ESP_LOGI(TAG, "Recovered %lu pending telemetry records",
             (unsigned long)handle->status.pending_count);
    return ESP_OK;
}

/**
 * @brief 持久保存一条记录，成功后更新槽位索引与事件位置。
 */
static esp_err_t outbox_append_record(gateway_outbox_handle_t handle,
                                      const gateway_outbox_record_t *record,
                                      uint32_t *out_record_id)
{
    if (handle == NULL || record == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->faulted)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (handle->status.next_record_id == UINT32_MAX)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(1000U)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    int slot = -1;
    for (unsigned i = 0U; i < GATEWAY_OUTBOX_CAPACITY; ++i)
    {
        if (handle->slot_id[i] == 0U)
        {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0 ||
        (!record->record_kind && handle->status.pending_count >= GATEWAY_OUTBOX_CAPACITY - 128U))
    {
        ++handle->status.dropped_count;
        const esp_err_t recorded =
            nvs_set_u32(handle->nvs, "dropped", handle->status.dropped_count);
        if (recorded == ESP_OK)
        {
            (void)nvs_commit(handle->nvs);
        }
        xSemaphoreGive(handle->mutex);
        return ESP_ERR_NO_MEM;
    }
    stored_record_t stored = {.version = GATEWAY_OUTBOX_SCHEMA_VERSION};
    stored.record = *record;
    stored.record.record_id = handle->status.next_record_id;
    const uint32_t next_id =
        stored.record.record_id == UINT32_MAX ? 1U : stored.record.record_id + 1U;
    char key[8];
    outbox_slot_key((unsigned)slot, key);
    esp_err_t error = nvs_set_blob(handle->nvs, key, &stored, sizeof(stored));
    if (error == ESP_OK)
    {
        error = nvs_set_u32(handle->nvs, "next_id", next_id);
    }
    if (error == ESP_OK && record->record_kind && record->event_id > handle->event_cursor)
    {
        error = nvs_set_u32(handle->nvs, "event_cursor", record->event_id);
    }
    if (error == ESP_OK)
    {
        error = nvs_commit(handle->nvs);
    }
    if (error == ESP_OK)
    {
        handle->slot_id[slot] = stored.record.record_id;
        ++handle->status.pending_count;
        handle->status.next_record_id = next_id;
        if (record->record_kind && record->event_id > handle->event_cursor)
        {
            handle->event_cursor = record->event_id;
        }
        if (out_record_id != NULL)
        {
            *out_record_id = stored.record.record_id;
        }
    }
    if (error != ESP_OK)
    {
        handle->faulted = true;
    }
    xSemaphoreGive(handle->mutex);
    return error;
}

/**
 * @brief 读取最老待发记录并核对存储结构与记录号。
 */
static esp_err_t outbox_read_oldest(gateway_outbox_handle_t handle,
                                    gateway_outbox_record_t *out_record)
{
    if (handle == NULL || out_record == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(1000U)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    const int slot = outbox_oldest_slot(handle);
    if (slot < 0)
    {
        xSemaphoreGive(handle->mutex);
        return ESP_ERR_NOT_FOUND;
    }
    char key[8];
    outbox_slot_key((unsigned)slot, key);
    stored_record_t stored;
    size_t length = sizeof(stored);
    esp_err_t error = nvs_get_blob(handle->nvs, key, &stored, &length);
    if (error == ESP_OK &&
        (length != sizeof(stored) || stored.version != GATEWAY_OUTBOX_SCHEMA_VERSION ||
         stored.record.record_id != handle->slot_id[slot]))
    {
        error = ESP_ERR_INVALID_STATE;
    }
    if (error == ESP_OK)
    {
        *out_record = stored.record;
    }
    if (error != ESP_OK)
    {
        handle->faulted = true;
    }
    xSemaphoreGive(handle->mutex);
    return error;
}

/**
 * @brief 按记录号删除已获业务确认的待发记录。
 */
static esp_err_t outbox_remove_confirmed(gateway_outbox_handle_t handle, uint32_t record_id)
{
    if (handle == NULL || record_id == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(1000U)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    int slot = -1;
    for (unsigned i = 0U; i < GATEWAY_OUTBOX_CAPACITY; ++i)
    {
        if (handle->slot_id[i] == record_id)
        {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0)
    {
        xSemaphoreGive(handle->mutex);
        return ESP_ERR_NOT_FOUND;
    }
    char key[8];
    outbox_slot_key((unsigned)slot, key);
    esp_err_t error = nvs_erase_key(handle->nvs, key);
    if (error == ESP_OK)
    {
        error = nvs_commit(handle->nvs);
    }
    if (error == ESP_OK)
    {
        handle->slot_id[slot] = 0U;
        --handle->status.pending_count;
    }
    if (error != ESP_OK)
    {
        handle->faulted = true;
    }
    xSemaphoreGive(handle->mutex);
    return error;
}

/**
 * @brief 复制持久待发队列服务的当前状态。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_status 接收复制出的完整状态或配置。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t gateway_outbox_get_status(gateway_outbox_handle_t handle,
                                    gateway_outbox_status_t *out_status)
{
    if (handle == NULL || out_status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(100U)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    *out_status = handle->status;
    xSemaphoreGive(handle->mutex);
    return ESP_OK;
}

/**
 * @brief 投递存储请求并等待拥有者任务完成。
 * @note 请求入队后必须等拥有者完成，避免把任务栈上的请求对象提前释放。
 */
static esp_err_t outbox_submit_and_wait(gateway_outbox_handle_t handle, outbox_job_t *job)
{
    if (!handle)
    {
        return ESP_ERR_INVALID_ARG;
    }
    job->done = xSemaphoreCreateBinary();
    if (!job->done)
    {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = ESP_ERR_TIMEOUT;
    if (xQueueSend(handle->jobs, &job, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        /* Once accepted, keep request storage alive until the owner completes. */
        xSemaphoreTake(job->done, portMAX_DELAY);
        result = job->result;
    }
    vSemaphoreDelete(job->done);
    return result;
}

/**
 * @brief 请求追加一条持久待发记录。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[in] record 本次处理的消息、记录或事件对象。
 * @param[out] out_record_id 接收成功分配的记录号；允许传入 NULL。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 * @note 任务接口，可能阻塞等待存储完成；记录 ID 仅在成功时回填。
 */
esp_err_t gateway_outbox_append(gateway_outbox_handle_t handle,
                                const gateway_outbox_record_t *record, uint32_t *out_record_id)
{
    if (!record)
    {
        return ESP_ERR_INVALID_ARG;
    }
    outbox_job_t job = {.op = 1, .record = *record};
    esp_err_t error = outbox_submit_and_wait(handle, &job);
    if (error == ESP_OK && out_record_id)
    {
        *out_record_id = job.id;
    }
    return error;
}

/**
 * @brief 请求读取最老记录，保留其待发状态。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[out] out_record 接收函数产生的结果；仅在成功条件成立时使用。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 */
esp_err_t gateway_outbox_peek_oldest(gateway_outbox_handle_t handle,
                                     gateway_outbox_record_t *out_record)
{
    if (!out_record)
    {
        return ESP_ERR_INVALID_ARG;
    }
    outbox_job_t job = {.op = 2};
    esp_err_t error = outbox_submit_and_wait(handle, &job);
    if (error == ESP_OK)
    {
        *out_record = job.record;
    }
    return error;
}

/**
 * @brief 请求删除匹配 PUBACK 的持久记录。
 * @param[in] handle 已创建的模块实例句柄。
 * @param[in] record_id 待确认或删除的持久记录号。
 * @return ESP_OK 表示该接口成功；其他值为参数、资源或运行错误码。
 * @note 只能在对应消息 ID 与连接代数的 PUBACK 匹配后调用。
 */
esp_err_t gateway_outbox_ack(gateway_outbox_handle_t handle, uint32_t record_id)
{
    outbox_job_t job = {.op = 3, .id = record_id};
    return outbox_submit_and_wait(handle, &job);
}

/**
 * @brief 读取已经持久保存的现场事件位置。
 * @param[in] handle 已创建的模块实例句柄。
 * @return 已持久保存的现场事件号；空句柄返回 0。
 */
uint32_t gateway_outbox_event_cursor(gateway_outbox_handle_t handle)
{
    if (!handle)
    {
        return 0;
    }
    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    uint32_t event_cursor = handle->event_cursor;
    xSemaphoreGive(handle->mutex);
    return event_cursor;
}
