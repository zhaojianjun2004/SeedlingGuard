/**
 ******************************************************************************
 * @file    gateway_app.c
 * @brief   F407 应用入口门面实现。
 * @details 供 CubeMX 调用；入口负责转入服务，任务通常不返回。
 *
 * 项目：智能育苗室环境监测与远程告警系统
 * 层次：应用层
 ******************************************************************************
 */

/*--------------------------- 头文件 ---------------------------*/
#include "gateway_app.h"
#include "field_services.h"

/*--------------------------- 函数实现 ---------------------------*/

/**
 * @brief 创建现场服务运行前需要的 IPC 与默认状态。
 * @return true 表示操作完成或检查通过；false 表示失败、未就绪或条件不满足。
 */
bool GatewayApp_Init(void)
{
    return field_services_init();
}

/**
 * @brief 进入现场链路服务任务。
 * @note 任务入口，只在任务上下文运行；正常情况下不返回。
 */
void GatewayApp_LinkTask(void)
{
    field_link_service_run();
}

/**
 * @brief 进入现场状态服务任务。
 * @note 任务入口，只在任务上下文运行；正常情况下不返回。
 */
void GatewayApp_FieldTask(void)
{
    field_state_service_run();
}

/**
 * @brief 进入采集调度服务任务。
 * @note 任务入口，只在任务上下文运行；正常情况下不返回。
 */
void GatewayApp_ModbusTask(void)
{
    acquisition_service_run();
}

/**
 * @brief 进入配置与事件存储任务。
 * @note 任务入口，只在任务上下文运行；正常情况下不返回。
 */
void GatewayApp_StorageTask(void)
{
    storage_service_run();
}

/**
 * @brief 进入健康监督服务任务。
 * @note 任务入口，只在任务上下文运行；正常情况下不返回。
 */
void GatewayApp_SupervisorTask(void)
{
    supervisor_service_run();
}
