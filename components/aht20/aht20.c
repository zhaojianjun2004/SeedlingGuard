/**
 * @file aht20.c
 * @brief Protocol implementation for the ASAIR AHT20 sensor.
 */

#include "aht20.h"

#include <stdbool.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* Commands and timing values come from AHT20 data sheet V1.2. */
#define AHT20_CMD_INITIALIZE              (0xBEU)
#define AHT20_CMD_TRIGGER_MEASUREMENT     (0xACU)
#define AHT20_CMD_SOFT_RESET              (0xBAU)
#define AHT20_INITIALIZE_PARAMETER        (0x08U)
#define AHT20_MEASURE_PARAMETER           (0x33U)

// 状态字节的位定义
#define AHT20_STATUS_BUSY_MASK            (1U << 7)
#define AHT20_STATUS_CALIBRATED_MASK      (1U << 3)

// 各种延时/超时
#define AHT20_POWER_ON_DELAY_MS           (40U)  // 上电后等待 40 ms，确保传感器进入空闲状态
#define AHT20_INITIALIZE_DELAY_MS         (10U)  // 初始化命令后等待 10 ms，确保传感器完成初始化
#define AHT20_MEASUREMENT_DELAY_MS        (80U)  // 测量命令后等待 80 ms，确保传感器完成测量
#define AHT20_BUSY_POLL_INTERVAL_MS       (5U)  // 每次轮询忙状态的间隔时间为 5 ms
#define AHT20_BUSY_TIMEOUT_MS             (120U) // 轮询忙状态的最大超时时间为 120 ms
#define AHT20_RESET_DELAY_MS              (20U) // 软复位命令后等待 20 ms，确保传感器完成复位
#define AHT20_CRC_DELAY_MS                (10U) // CRC 计算后等待 10 ms

#define AHT20_DATA_LENGTH                 (6U) // 数据帧长度为 6 字节（不包括CRC 字节）
#define AHT20_FRAME_LENGTH                (7U) // 总帧长度为 7 字节（包括CRC 字节）
#define AHT20_RAW_VALUE_DENOMINATOR       (1048576.0F) /* 2^20  用于将原始归1化 */

#define AHT20_CRC_INITIAL_VALUE           (0xFFU)
#define AHT20_CRC_POLYNOMIAL              (0x31U)

struct aht20_device {
    i2c_port_t i2c_port; // 调用者已安装的 I2C 主机端口
    uint8_t i2c_address;  // 设备 I2C 地址
    TickType_t io_timeout_ticks;
    SemaphoreHandle_t mutex;  // 互斥锁
    bool initialized;  // 是否初始化
};

/** 把毫秒转换成至少 1 个 RTOS tick */
static TickType_t aht20_ms_to_ticks(uint32_t milliseconds)
{
    const TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    return (ticks > 0U) ? ticks : 1U;
}

/** 锁定整个传感器事务，而不仅仅是一次单独的 I2C 调用.
 *  互斥锁保证 read、initialize、soft_reset 三个操作不会交叉执行，从而让“检查 initialized → 发命令 → 读数据”成为一个原子事务。
 * 
 */
static esp_err_t aht20_lock(aht20_handle_t handle)
{
    if (xSemaphoreTake(handle->mutex, handle->io_timeout_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void aht20_unlock(aht20_handle_t handle)
{
    (void)xSemaphoreGive(handle->mutex);
}

/** 以读方向寻址设备，读取状态字节。
 *  函数名带 _unlocked，表示调用者必须已经持有锁。
 */
static esp_err_t aht20_read_status_unlocked(aht20_handle_t handle, uint8_t *out_status)
{
    return i2c_master_read_from_device(handle->i2c_port,
                                       handle->i2c_address,
                                       out_status,
                                       sizeof(*out_status),
                                       handle->io_timeout_ticks);
}

/** 发送由操作码和两个参数组成的三字节命令。
 *  AHT20 的命令都是 3 字节，如 BE 08 00、AC 33 00。
 */
static esp_err_t aht20_write_command_unlocked(aht20_handle_t handle,
                                               uint8_t command,
                                               uint8_t parameter_1,
                                               uint8_t parameter_2)
{
    const uint8_t command_frame[] = {command, parameter_1, parameter_2};

    return i2c_master_write_to_device(handle->i2c_port,
                                      handle->i2c_address,
                                      command_frame,
                                      sizeof(command_frame),
                                      handle->io_timeout_ticks);
}

/** 使用初始值 0xFF 和多项式 0x31 计算 CRC-8。 */
static uint8_t aht20_calculate_crc8(const uint8_t *data, size_t length)
{
    uint8_t crc = AHT20_CRC_INITIAL_VALUE;

    for (size_t byte_index = 0U; byte_index < length; ++byte_index) {
        crc ^= data[byte_index];

        for (uint8_t bit_index = 0U; bit_index < 8U; ++bit_index) {
            if ((crc & 0x80U) != 0U) {
                crc = (uint8_t)((crc << 1U) ^ AHT20_CRC_POLYNOMIAL);
            } else {
                crc <<= 1U;
            }
        }
    }

    return crc;
}

esp_err_t aht20_create(const aht20_config_t *config, aht20_handle_t *out_handle)
{
    if ((config == NULL) || (out_handle == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((config->i2c_port < I2C_NUM_0) || (config->i2c_port >= I2C_NUM_MAX) ||
        (config->i2c_address == 0U) || (config->i2c_address > 0x7FU) ||
        (config->io_timeout_ms == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_handle = NULL;

    aht20_handle_t handle = calloc(1U, sizeof(*handle));
    if (handle == NULL) {
        return ESP_ERR_NO_MEM;
    }

    handle->mutex = xSemaphoreCreateMutex();
    if (handle->mutex == NULL) {
        free(handle);
        return ESP_ERR_NO_MEM;
    }

    handle->i2c_port = config->i2c_port;
    handle->i2c_address = config->i2c_address;
    handle->io_timeout_ticks = aht20_ms_to_ticks(config->io_timeout_ms);
    handle->initialized = false;

    *out_handle = handle;
    return ESP_OK;
}

void aht20_delete(aht20_handle_t handle)
{
    if (handle == NULL) {
        return;
    }

    vSemaphoreDelete(handle->mutex);
    free(handle);
}

esp_err_t aht20_initialize(aht20_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = aht20_lock(handle);
    if (result != ESP_OK) {
        return result;
    }

    handle->initialized = false;

    /* A fixed 40 ms delay is harmless if the sensor was powered earlier and
     * guarantees that a freshly powered sensor has reached its idle state. */
    vTaskDelay(aht20_ms_to_ticks(AHT20_POWER_ON_DELAY_MS));

    uint8_t status = 0U;
    result = aht20_read_status_unlocked(handle, &status);
    if (result != ESP_OK) {
        goto exit;
    }

    if ((status & AHT20_STATUS_CALIBRATED_MASK) == 0U) {
        result = aht20_write_command_unlocked(handle,
                                              AHT20_CMD_INITIALIZE,
                                              AHT20_INITIALIZE_PARAMETER,
                                              0x00U);
        if (result != ESP_OK) {
            goto exit;
        }

        vTaskDelay(aht20_ms_to_ticks(AHT20_INITIALIZE_DELAY_MS));

        result = aht20_read_status_unlocked(handle, &status);
        if (result != ESP_OK) {
            goto exit;
        }
        if ((status & AHT20_STATUS_CALIBRATED_MASK) == 0U) {
            result = ESP_ERR_INVALID_STATE;
            goto exit;
        }
    }

    handle->initialized = true;

exit:
    aht20_unlock(handle);
    return result;
}

esp_err_t aht20_read(aht20_handle_t handle, aht20_measurement_t *out_measurement)
{
    if ((handle == NULL) || (out_measurement == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = aht20_lock(handle);
    if (result != ESP_OK) {
        return result;
    }

    /* The initialized flag belongs to the protected device state. Checking
     * it after taking the mutex keeps read/reset/initialize thread-safe as a
     * complete protocol transaction. */
    if (!handle->initialized) {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    result = aht20_write_command_unlocked(handle,
                                          AHT20_CMD_TRIGGER_MEASUREMENT,
                                          AHT20_MEASURE_PARAMETER,
                                          0x00U);
    if (result != ESP_OK) {
        goto exit;
    }

    vTaskDelay(aht20_ms_to_ticks(AHT20_MEASUREMENT_DELAY_MS));

    /* Measurement normally finishes in 80 ms. Polling has a bounded timeout
     * so a damaged or disconnected sensor cannot block the application. */
    uint32_t elapsed_poll_ms = 0U;
    uint8_t status = AHT20_STATUS_BUSY_MASK;
    while ((status & AHT20_STATUS_BUSY_MASK) != 0U) {
        result = aht20_read_status_unlocked(handle, &status);
        if (result != ESP_OK) {
            goto exit;
        }
        if ((status & AHT20_STATUS_BUSY_MASK) == 0U) {
            break;
        }
        if (elapsed_poll_ms >= AHT20_BUSY_TIMEOUT_MS) {
            result = ESP_ERR_TIMEOUT;
            goto exit;
        }

        vTaskDelay(aht20_ms_to_ticks(AHT20_BUSY_POLL_INTERVAL_MS));
        elapsed_poll_ms += AHT20_BUSY_POLL_INTERVAL_MS;
    }

    uint8_t frame[AHT20_FRAME_LENGTH] = {0U};
    result = i2c_master_read_from_device(handle->i2c_port,
                                         handle->i2c_address,
                                         frame,
                                         sizeof(frame),
                                         handle->io_timeout_ticks);
    if (result != ESP_OK) {
        goto exit;
    }

    if ((frame[0] & AHT20_STATUS_BUSY_MASK) != 0U) {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    if (aht20_calculate_crc8(frame, AHT20_DATA_LENGTH) != frame[6]) {
        result = ESP_ERR_INVALID_CRC;
        goto exit;
    }

    const uint32_t raw_humidity = ((uint32_t)frame[1] << 12U) |
                                  ((uint32_t)frame[2] << 4U) |
                                  ((uint32_t)frame[3] >> 4U);
    const uint32_t raw_temperature = (((uint32_t)frame[3] & 0x0FU) << 16U) |
                                     ((uint32_t)frame[4] << 8U) |
                                     (uint32_t)frame[5];

    /* Use a local value so the caller never observes a partially written
     * measurement if a future validation step returns an error. */
    const aht20_measurement_t measurement = {
        .temperature_c = ((float)raw_temperature / AHT20_RAW_VALUE_DENOMINATOR) * 200.0F - 50.0F,
        .humidity_percent = ((float)raw_humidity / AHT20_RAW_VALUE_DENOMINATOR) * 100.0F,
    };

    *out_measurement = measurement;

exit:
    aht20_unlock(handle);
    return result;
}

esp_err_t aht20_soft_reset(aht20_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = aht20_lock(handle);
    if (result != ESP_OK) {
        return result;
    }

    const uint8_t command = AHT20_CMD_SOFT_RESET;
    result = i2c_master_write_to_device(handle->i2c_port,
                                        handle->i2c_address,
                                        &command,
                                        sizeof(command),
                                        handle->io_timeout_ticks);
    if (result == ESP_OK) {
        handle->initialized = false;
        vTaskDelay(aht20_ms_to_ticks(AHT20_RESET_DELAY_MS));
    }

    aht20_unlock(handle);
    return result;
}
