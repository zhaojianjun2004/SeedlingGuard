# F407 现场控制器的分层

| 目录 | 职责 | 优先读的文件 |
| --- | --- | --- |
| `Application` | CubeMX 到业务服务的应用入口 | `Src/gateway_app.c` |
| `Services` | 采集、现场状态、链路、存储、监督 | `Src/field_state_service.c` |
| `Protocol` | Modbus 和 CAN 纯 C 编解码 | `Src/modbus_rtu.c`、`Src/can_diag_protocol.c` |
| `Drivers/Device` | W25Q 设备操作 | `Src/w25q_flash.c` |
| `Bsp` | 具体外设、DMA、中断、引脚与看门狗硬件 | `Src/rs485_port.c`、`Src/board_system.c` |
| `Config` | 仪表映射和产品参数 | `modbus_profile.h` |
| `Core` | CubeMX 生成的启动与外设/任务配置 | `Src/main.c`、`Src/freertos.c` |

领域与 UART 帧代码引用根目录的 `shared/domain`、`shared/protocol`。`Services/Private` 只在服务目标内开放，外部不直接操作队列或 `g_field`。

各层通过 `cmake/nursery_layers.cmake` 分别构建为库。CubeMX 继续调用 `GatewayApp_*`，无需修改既有任务入口名称。

完整架构图与面试讲述见[项目架构说明](../../ARCHITECTURE.md)。
