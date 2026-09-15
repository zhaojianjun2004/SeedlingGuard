/**
 * @file aht20.h
 * @brief AHT20 temperature and humidity sensor driver.
 *
 * This component implements only the AHT20 device protocol. It does not own
 * the I2C bus, create application tasks, print measurements, or contain any
 * board-specific GPIO configuration.
 */

#pragma once

#include <stdint.h>

#include "driver/i2c.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default 7-bit I2C address defined by the AHT20 data sheet. */
#define AHT20_DEFAULT_I2C_ADDRESS (0x38U)

/** Opaque AHT20 device handle. */
typedef struct aht20_device *aht20_handle_t;

/** Driver construction parameters. */
typedef struct {
    i2c_port_t i2c_port;       /**< 调用者已安装的 I2C 主机端口 */
    uint8_t i2c_address;       /**< 7 位设备地址，通常 0x38 */
    uint32_t io_timeout_ms;    /**< 单次 I2C 事务和驱动锁的超时 */
} aht20_config_t;

/** One decoded sensor measurement. */
typedef struct {
    float temperature_c;       /**< 摄氏温度 */
    float humidity_percent;    /**< 相对湿度百分比 RH */
} aht20_measurement_t;

/**
 * @brief Create an AHT20 driver instance. 创建实例
 *
 * The I2C master driver must already be configured and installed. This
 * function allocates the device context, but does not communicate with the
 * sensor; call aht20_initialize() before the first measurement.
 *
 * @param[in] config Driver configuration. The data is copied by the driver.
 * @param[out] out_handle Receives the newly created handle.
 * @return ESP_OK on success, otherwise an ESP-IDF error code.
 */
esp_err_t aht20_create(const aht20_config_t *config, aht20_handle_t *out_handle);

/**
 * @brief Destroy a driver instance created by aht20_create(). 销毁实例
 *
 * The caller must stop every task using this handle before deleting it.
 * Deleting the AHT20 handle does not uninstall the shared I2C bus.
 *
 * @param[in] handle Driver handle. NULL is accepted as a no-op.
 */
void aht20_delete(aht20_handle_t handle);

/**
 * @brief Wait for power-up and initialize the sensor when required.  初始化传感器
 *
 * The routine checks the calibration-enable status bit. If it is not set,
 * the data-sheet initialization sequence BE 08 00 is sent.
 *
 * @param[in] handle Driver handle.
 * @return ESP_OK when the sensor is ready for measurements.
 */
esp_err_t aht20_initialize(aht20_handle_t handle);

/**
 * @brief Trigger and read one temperature/humidity measurement.  读取一次测量
 *
 * This call sends AC 33 00, waits for conversion, polls the busy bit, reads
 * all six data bytes plus CRC, validates CRC-8, and converts both 20-bit raw
 * values to engineering units.
 *
 * @param[in] handle Initialized driver handle.
 * @param[out] out_measurement Receives the decoded result only on success.
 * @return ESP_OK, ESP_ERR_TIMEOUT, ESP_ERR_INVALID_CRC, or an I2C error.
 */
esp_err_t aht20_read(aht20_handle_t handle, aht20_measurement_t *out_measurement);

/**
 * @brief Perform an AHT20 software reset.  软复位传感器
 *
 * A successful reset clears the driver's initialized state. The caller must
 * invoke aht20_initialize() again before reading another measurement.
 *
 * @param[in] handle Driver handle.
 * @return ESP_OK on success, otherwise an I2C error.
 */
esp_err_t aht20_soft_reset(aht20_handle_t handle);

#ifdef __cplusplus
}
#endif
