/**
 ******************************************************************************
 * @file    modbus_rtu.c
 * @brief   Modbus RTU 03 编解码实现。
 * @details 校验地址、功能码、字节数与 CRC；接收时序和重试由端口及采集服务负责。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：协议层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "modbus_rtu.h"

#include "gateway_protocol.h"

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 生成一个保持寄存器读取请求，地址与数量使用高字节在前。
 * @param[in] address Modbus 从站地址，范围 1～247。
 * @param[in] start_register 仪表起始寄存器地址，在线路上按高字节在前编码。
 * @param[in] register_count 连续寄存器数量，本实现允许 1～16。
 * @param[out] output 输出字节缓冲区，容量由配套参数或数组长度约束。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool modbus_rtu_build_read03(uint8_t address, uint16_t start_register, uint16_t register_count,
                             uint8_t output[8])
{
    if (output == NULL || address == 0U || address > 247U || register_count == 0U ||
        register_count > 16U)
    {
        return false;
    }
    output[0] = address;
    output[1] = 0x03U;
    output[2] = (uint8_t)(start_register >> 8);
    output[3] = (uint8_t)start_register;
    output[4] = (uint8_t)(register_count >> 8);
    output[5] = (uint8_t)register_count;
    const uint16_t crc = gw_crc16(output, 6U);
    output[6] = (uint8_t)crc;
    output[7] = (uint8_t)(crc >> 8);
    return true;
}

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
                                            uint16_t *registers)
{
    if (response == NULL || registers == NULL || register_count == 0U || register_count > 16U)
    {
        return MODBUS_RTU_BAD_ARGUMENT;
    }
    if (response_length < 5U)
    {
        return MODBUS_RTU_BAD_LENGTH;
    }
    if (response[0] != address)
    {
        return MODBUS_RTU_BAD_ADDRESS;
    }
    const uint16_t expected_crc = gw_crc16(response, response_length - 2U);
    const uint16_t received_crc =
        (uint16_t)response[response_length - 2U] | ((uint16_t)response[response_length - 1U] << 8);
    if (expected_crc != received_crc)
    {
        return MODBUS_RTU_BAD_CRC;
    }
    if (response[1] == 0x83U)
    {
        return response_length == 5U ? MODBUS_RTU_EXCEPTION : MODBUS_RTU_BAD_LENGTH;
    }
    if (response[1] != 0x03U)
    {
        return MODBUS_RTU_BAD_FUNCTION;
    }
    const size_t expected_length = 5U + 2U * register_count;
    if (response_length != expected_length || response[2] != 2U * register_count)
    {
        return MODBUS_RTU_BAD_LENGTH;
    }
    for (uint16_t i = 0; i < register_count; ++i)
    {
        registers[i] = ((uint16_t)response[3U + 2U * i] << 8) | response[4U + 2U * i];
    }
    return MODBUS_RTU_OK;
}
