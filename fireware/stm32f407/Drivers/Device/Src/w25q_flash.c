/**
 ******************************************************************************
 * @file    w25q_flash.c
 * @brief   W25Q 设备指令驱动实现。
 * @details 完成 JEDEC、跨页编程、扇区擦除和 BUSY 超时；不决定配置和日志地址。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：设备驱动 / 平台适配层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "w25q_flash.h"
#include "board_flash_port.h"
#include "board_system.h"

/*--------------------------- 模块配置、类型与状态 ---------------------------*/

static uint32_t s_flash_capacity;

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 通过 BSP 控制 Flash 片选。
 */
static void w25q_select(bool selected)
{
    board_flash_select(selected);
}

/**
 * @brief 通过 BSP 发送 Flash 指令或数据字节。
 */
static bool w25q_tx(const void *data, size_t length)
{
    return board_flash_tx(data, length);
}

/**
 * @brief 有界轮询 Flash BUSY 位，忙时让出任务执行权。
 */
static bool w25q_wait_ready(uint32_t timeout_ms)
{
    uint32_t started_ms = board_millis();
    do
    {
        uint8_t command = 5, status_register = 0xff;
        w25q_select(true);
        bool transfer_ok = w25q_tx(&command, 1) && board_flash_rx(&status_register, 1);
        w25q_select(false);
        if (!transfer_ok)
        {
            return false;
        }
        if (!(status_register & 1))
        {
            return true;
        }
        board_delay_ms(1);
    } while (board_millis() - started_ms < timeout_ms);
    return false;
}

/**
 * @brief 发送 WREN 写使能命令。
 */
static bool w25q_write_enable(void)
{
    uint8_t command = 6;
    w25q_select(true);
    bool transfer_ok = w25q_tx(&command, 1);
    w25q_select(false);
    return transfer_ok;
}

/**
 * @brief 发送操作码与三字节地址。
 */
static bool w25q_send_address(uint8_t opcode, uint32_t address)
{
    uint8_t command_bytes[4] = {opcode, (uint8_t)(address >> 16), (uint8_t)(address >> 8),
                                (uint8_t)address};
    return w25q_tx(command_bytes, 4);
}

/**
 * @brief 读取 JEDEC 并识别受支持的 Flash 容量。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool w25q_init(void)
{
    uint8_t command = 0x9f, jedec_id[3] = {0};
    w25q_select(true);
    bool transfer_ok = w25q_tx(&command, 1) && board_flash_rx(jedec_id, 3);
    w25q_select(false);
    if (!transfer_ok || jedec_id[0] != 0xef || jedec_id[2] < 0x16 || jedec_id[2] > 0x18)
    {
        return false;
    }
    s_flash_capacity = 1UL << jedec_id[2];
    return w25q_wait_ready(3000);
}

/**
 * @brief 检查容量边界并分块读取 Flash 内容。
 * @param[in] address Flash 字节地址；擦除接口要求 4 KiB 对齐。
 * @param[out] data 输出字节缓冲区，容量由配套参数或数组长度约束。
 * @param[in] length 本次输入内容长度，单位字节。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool w25q_read(uint32_t address, void *data, size_t length)
{
    if (!s_flash_capacity || address >= s_flash_capacity || length > s_flash_capacity - address)
    {
        return false;
    }
    w25q_select(true);
    bool transfer_ok = w25q_send_address(3, address);
    while (transfer_ok && length)
    {
        uint16_t chunk = length > 4096 ? 4096 : (uint16_t)length;
        transfer_ok = board_flash_rx(data, chunk);
        data = (uint8_t *)data + chunk;
        length -= chunk;
    }
    w25q_select(false);
    return transfer_ok;
}

/**
 * @brief 按 256 字节页边界拆分编程并等待完成。
 * @param[in] address Flash 字节地址；擦除接口要求 4 KiB 对齐。
 * @param[in] data 输入字节缓冲区。
 * @param[in] length 本次输入内容长度，单位字节。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 * @note 调用方负责目标擦除状态；失败可能已写入部分数据，不表示整笔原子提交。
 */
bool w25q_program(uint32_t address, const void *data, size_t length)
{
    if (!s_flash_capacity || address >= s_flash_capacity || length > s_flash_capacity - address)
    {
        return false;
    }
    while (length)
    {
        size_t page_length = 256 - (address & 255);
        if (page_length > length)
        {
            page_length = length;
        }
        if (!w25q_wait_ready(3000) || !w25q_write_enable())
        {
            return false;
        }
        w25q_select(true);
        bool transfer_ok = w25q_send_address(2, address) && w25q_tx(data, page_length);
        w25q_select(false);
        if (!transfer_ok || !w25q_wait_ready(3000))
        {
            return false;
        }
        address += (uint32_t)page_length;
        data = (const uint8_t *)data + page_length;
        length -= page_length;
    }
    return true;
}

/**
 * @brief 检查 4 KiB 对齐并擦除一个扇区。
 * @param[in] address Flash 字节地址；擦除接口要求 4 KiB 对齐。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 * @note 擦除粒度为 4096 字节，地址必须对齐；操作含有界 BUSY 等待。
 */
bool w25q_erase(uint32_t address)
{
    if (!s_flash_capacity || address >= s_flash_capacity || (address & 4095) ||
        !w25q_wait_ready(3000) || !w25q_write_enable())
    {
        return false;
    }
    w25q_select(true);
    bool transfer_ok = w25q_send_address(0x20, address);
    w25q_select(false);
    return transfer_ok && w25q_wait_ready(3000);
}
