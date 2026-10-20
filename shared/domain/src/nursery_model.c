/**
 ******************************************************************************
 * @file    nursery_model.c
 * @brief   育苗室领域规则实现。
 * @details 纯 C 的配置边界、样本范围、告警确认和回差；不依赖 HAL、SDK 或 RTOS。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：共享领域层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "nursery_model.h"
#include <string.h>

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 建立三区域九测点的默认采集与告警配置。
 * @param[out] config 接收默认配置，调用方提供有效对象。
 * @note 调用方提供有效配置对象；默认阈值仅作台架初值，部署时按种苗与仪表调整。
 */
void nursery_config_defaults(nursery_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->version = 1;
    config->period_ms = 5000;
    config->enabled = 1;
    /* 每区依次为温度、湿度、光照；表中三列为下限、上限和回差。 */
    for (unsigned point_index = 0; point_index < NURSERY_POINT_COUNT; point_index++)
    {
        static const nursery_rule_t defaults[3] = {
            {100, 350, 10}, {300, 900, 30}, {100, 50000, 100}};
        config->rules[point_index] = defaults[point_index % 3];
    }
}

/**
 * @brief 按测点类型检查定点测量值的物理量范围。
 * @param[in] point_index 从 0 开始的测点下标，范围 0～8。
 * @param[in] value_scaled 定点测量值：温湿度为十分之一单位，光照为 lx。
 * @return true 表示满足本函数检查条件；false 表示未满足。
 * @note 温度/湿度使用十分之一单位，光照使用整数 lx；测点索引从 0 开始。
 */
bool nursery_value_valid(unsigned point_index, int32_t value_scaled)
{
    if (point_index >= NURSERY_POINT_COUNT)
    {
        return false;
    }
    return point_index % 3 == 0   ? value_scaled >= -400 && value_scaled <= 800
           : point_index % 3 == 1 ? value_scaled >= 0 && value_scaled <= 1000
                                  : value_scaled >= 0 && value_scaled <= 200000;
}

/**
 * @brief 检查周期、使能、上下限和回差的组合约束。
 * @param[in] config 模块配置及其取值约束。
 * @return true 表示满足本函数检查条件；false 表示未满足。
 */
bool nursery_config_valid(const nursery_config_t *config)
{
    if (!config || !config->version || config->enabled > 1 ||
        config->period_ms < NURSERY_PERIOD_MIN_MS || config->period_ms > NURSERY_PERIOD_MAX_MS)
    {
        return false;
    }
    for (unsigned point_index = 0; point_index < NURSERY_POINT_COUNT; point_index++)
    {
        const nursery_rule_t *rule = &config->rules[point_index];
        if (!nursery_value_valid(point_index, rule->low) ||
            !nursery_value_valid(point_index, rule->high) || rule->low >= rule->high ||
            rule->hysteresis < 0 || rule->hysteresis > (rule->high - rule->low) / 2)
        {
            return false;
        }
    }
    return true;
}

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
                          bool valid)
{
    uint8_t previous_state = alarm->state;
    /* 无效样本不累计确认或恢复，也不能解除已锁存的超限。 */
    if (!valid)
    {
        alarm->state = NURSERY_ALARM_UNKNOWN;
        alarm->enter_count = alarm->recover_count = 0;
    }
    /* 告警已成立后，需要连续有效样本回到带回差的范围。 */
    else if (alarm->latched)
    {
        if (value_scaled >= rule->low + rule->hysteresis &&
            value_scaled <= rule->high - rule->hysteresis)
        {
            if (++alarm->recover_count >= 3)
            {
                alarm->latched = false;
                alarm->recover_count = 0;
            }
        }
        else
        {
            alarm->recover_count = 0;
        }
        alarm->state = alarm->latched ? NURSERY_ALARM_ACTIVE : NURSERY_ALARM_NORMAL;
    }
    else
    {
        if (value_scaled < rule->low || value_scaled > rule->high)
        {
            if (++alarm->enter_count >= 3)
            {
                alarm->latched = true;
                alarm->enter_count = 0;
            }
            alarm->state = alarm->latched ? NURSERY_ALARM_ACTIVE : NURSERY_ALARM_PENDING;
        }
        else
        {
            alarm->enter_count = 0;
            alarm->state = NURSERY_ALARM_NORMAL;
        }
    }
    return previous_state != alarm->state;
}

/**
 * @brief 检查测点、规则及设备同步掩码是否全部到齐。
 * @param[in] point_mask 九个测点的同步位图，低 9 位应全部置位。
 * @param[in] rule_mask 九条阈值规则的同步位图，低 9 位应全部置位。
 * @param[in] device_mask 六个设备的同步位图，低 6 位应全部置位。
 * @return true 表示满足本函数检查条件；false 表示未满足。
 */
bool nursery_snapshot_complete(uint16_t point_mask, uint16_t rule_mask, uint8_t device_mask)
{
    return point_mask == ((1U << NURSERY_POINT_COUNT) - 1) && rule_mask == point_mask &&
           device_mask == ((1U << NURSERY_DEVICE_COUNT) - 1);
}
