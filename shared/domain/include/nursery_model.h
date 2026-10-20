/**
 ******************************************************************************
 * @file    nursery_model.h
 * @brief   育苗室领域规则接口定义。
 * @details 纯 C 的配置边界、样本范围、告警确认和回差；不依赖 HAL、SDK 或 RTOS。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：共享领域层
 ******************************************************************************
 */

#pragma once
/*--------------------------- 头文件 ---------------------------*/
#include <stdbool.h>
#include <stdint.h>
#define NURSERY_ZONE_COUNT    3U
#define NURSERY_DEVICE_COUNT  6U
#define NURSERY_POINT_COUNT   9U
#define NURSERY_PERIOD_MIN_MS 5000U
#define NURSERY_PERIOD_MAX_MS 600000U

typedef struct
{
    int32_t low, high, hysteresis;
} nursery_rule_t;

typedef struct
{
    uint32_t version, period_ms;
    uint8_t enabled;
    nursery_rule_t rules[NURSERY_POINT_COUNT];
} nursery_config_t;

typedef enum
{
    NURSERY_ALARM_NORMAL,
    NURSERY_ALARM_PENDING,
    NURSERY_ALARM_ACTIVE,
    NURSERY_ALARM_UNKNOWN
} nursery_alarm_state_t;

typedef struct
{
    uint8_t state, enter_count, recover_count;
    bool latched;
} nursery_alarm_t;

/* 每个新样本调用一次；质量失效不能恢复告警，也不能跨失效样本累计确认。 */

/* 完整快照必须同时收到九个测点、九条规则及六个设备状态。 */

/*--------------------------- 对外接口 ---------------------------*/

/**
 * @brief 建立三区域九测点的默认采集与告警配置。
 * @param[out] config 接收默认配置，调用方提供有效对象。
 * @note 调用方提供有效配置对象；默认阈值仅作台架初值，部署时按种苗与仪表调整。
 */
void nursery_config_defaults(nursery_config_t *config);

/**
 * @brief 检查周期、使能、上下限和回差的组合约束。
 * @param[in] config 模块配置及其取值约束。
 * @return true 表示满足本函数检查条件；false 表示未满足。
 */
bool nursery_config_valid(const nursery_config_t *config);

/**
 * @brief 按测点类型检查定点测量值的物理量范围。
 * @param[in] point_index 从 0 开始的测点下标，范围 0～8。
 * @param[in] value_scaled 定点测量值：温湿度为十分之一单位，光照为 lx。
 * @return true 表示满足本函数检查条件；false 表示未满足。
 * @note 温度/湿度使用十分之一单位，光照使用整数 lx；测点索引从 0 开始。
 */
bool nursery_value_valid(unsigned point_index, int32_t value_scaled);

/**
 * @brief 按一个新样本推进告警确认或恢复状态。
 * @param[in,out] alarm 保存连续确认计数和已锁存告警的状态对象。
 * @param[in] rule 当前测点的上下限与恢复回差，单位与样本一致。
 * @param[in] value_scaled 定点测量值：温湿度为十分之一单位，光照为 lx。
 * @param[in] valid 本次样本是否可信且可参与规则判断。
 * @return true 表示告警状态发生变化；false 表示状态未变化，不代表采样失败。
 * @note 每个新样本调用一次；无效样本清空连续计数，但不能解除已锁存告警。
 */
bool nursery_alarm_update(nursery_alarm_t *alarm, const nursery_rule_t *rule, int32_t value_scaled,
                          bool valid);

/**
 * @brief 检查测点、规则及设备同步掩码是否全部到齐。
 * @param[in] point_mask 九个测点的同步位图，低 9 位应全部置位。
 * @param[in] rule_mask 九条阈值规则的同步位图，低 9 位应全部置位。
 * @param[in] device_mask 六个设备的同步位图，低 6 位应全部置位。
 * @return true 表示满足本函数检查条件；false 表示未满足。
 */
bool nursery_snapshot_complete(uint16_t point_mask, uint16_t rule_mask, uint8_t device_mask);
