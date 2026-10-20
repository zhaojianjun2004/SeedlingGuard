# ESP32 组件分层

先按职责找目录，再进入具体组件的 `include/` 和 `src/`。

| 目录 | 放什么 | 不放什么 |
| --- | --- | --- |
| `application/` | 服务装配、网页/按键/串口入口 | 传感器协议和 Flash 指令 |
| `services/` | 命令、网络策略、缓存、升级等运行流程 | GPIO、UART 安装和外设寄存器 |
| `domain/` | 共享领域规则的构建封装 | RTOS 任务和芯片 API |
| `protocol/` | 帧和消息格式转换 | 外设收发、业务执行 |
| `drivers/` | UART、NVS、HTTP 下载、Flash 与摘要平台适配 | 告警策略与设备离线策略 |
| `contracts/` | 服务和适配器共同遵守的数据/函数接口 | 服务实现 |

`domain/nursery_domain` 和 `protocol/gateway_protocol` 引入项目 `shared/` 的同一份源码。ESP-IDF 原生系统/网络组件按需复用；项目组件的层级由根 CMake 注册。

完整目录、调用链和讲述方式见[架构说明](../ARCHITECTURE.md)。
