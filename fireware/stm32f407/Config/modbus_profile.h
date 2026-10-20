/**
 ******************************************************************************
 * @file    modbus_profile.h
 * @brief   现场仪表产品参数接口定义。
 * @details 地址、寄存器和倍率沿用参考映射，部署前必须按实际仪表手册核对。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：产品配置
 ******************************************************************************
 */

#pragma once
#define NURSERY_TH_REGISTER       0x0001U
#define NURSERY_LIGHT_REGISTER    0x0002U
#define NURSERY_LIGHT_DIVISOR     1000U
#define NURSERY_MODBUS_TIMEOUT_MS 200U
