/**
 ******************************************************************************
 * @file    modbus_rtu.h
 * @brief   Modbus RTU 03 编解码接口定义。
 * @details 校验地址、功能码、字节数与 CRC；接收时序和重试由端口及采集服务负责。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：协议层
 ******************************************************************************
 */

#pragma once

/*--------------------------- 头文件 ---------------------------*/
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    MODBUS_RTU_OK = 0,
    MODBUS_RTU_BAD_ARGUMENT,
    MODBUS_RTU_BAD_LENGTH,
    MODBUS_RTU_BAD_ADDRESS,
    MODBUS_RTU_BAD_FUNCTION,
    MODBUS_RTU_EXCEPTION,
    MODBUS_RTU_BAD_CRC,
    MODBUS_RTU_TIMEOUT,
    MODBUS_RTU_TX_ERROR,
} modbus_rtu_result_t;

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 生成一个保持寄存器读取请求，地址与数量使用高字节在前。
 * @param[in] address Modbus 从站地址，范围 1～247。
 * @param[in] start_register 仪表起始寄存器地址，在线路上按高字节在前编码。
 * @param[in] register_count 连续寄存器数量，本实现允许 1～16。
 * @param[out] output 输出字节缓冲区，容量由配套参数或数组长度约束。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool modbus_rtu_build_read03(uint8_t address, uint16_t start_register, uint16_t register_count,
                             uint8_t output[8]);

/**
 * @brief 检查 03 响应并解析寄存器值，单独返回协议异常。
 * @param[in] address Modbus 从站地址，范围 1～247。
 * @param[in] register_count 连续寄存器数量，本实现允许 1～16。
 * @param[in] response 输入字节缓冲区。
 * @param[in] response_length 本次输入内容长度，单位字节。
 * @param[out] registers 接收解析后的寄存器数组，容量不少于请求数量。
 * @return MODBUS_RTU_OK 表示成功；异常响应、CRC、地址和长度错误分别返回对应分类。
 */
modbus_rtu_result_t modbus_rtu_parse_read03(uint8_t address, uint16_t register_count,
                                            const uint8_t *response, size_t response_length,
                                            uint16_t *registers);
