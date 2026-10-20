# 智能育苗室环境监测与远程告警系统

STM32F407 负责三区域现场采集、数据质量、阈值告警和配置/事件存储；ESP32-S3 负责配网、网页、MQTT 持久补传与自身 A/B OTA。网络异常不停止现场采集。

当前应用版本 `1.3.0`，发布序号 `130`，板间协议 **UART V2**。AHT20/BLE 学习组件已清理，工程只装配育苗室服务。

## 从这里复盘

- [代码书写规范](CODE_STYLE.md)：文件头、函数注释、命名、步骤说明与统一排版。
- **[先看架构分层与代码导航](ARCHITECTURE.md)**：每层的职责、真实文件、采集调用链与面试讲述。

- [当前工程结构、接口、看门狗与联调说明](docs/独立项目_物联网边缘网关/工程结构与联调_智能育苗室_2026-10-17.md)
- [原方案与改造前代码盘点](docs/独立项目_物联网边缘网关/方案与现有代码盘点_2026-10-17.md)：历史基线，不能当作改造后的完成清单。
- [最终简历场景](docs/独立项目_物联网边缘网关/简历最终版_智能育苗室环境监测与远程告警系统.md)

## 目录与架构

| 层次 | ESP32 | F407 |
| --- | --- | --- |
| 应用层 | `main/`、`components/application/` | `Application/` |
| 业务服务层 | `components/services/` | `Services/` |
| 协议层 | `components/protocol/` | `Protocol/` |
| 驱动/平台适配层 | `components/drivers/` | `Drivers/Device/` |
| 板级支持层 | UART 引脚由应用配置，ESP-IDF 执行硬件操作 | `Bsp/`、CubeMX `Core/` |
| 共享基础 | `shared/domain/`、`shared/protocol/`；OTA 契约在 `components/contracts/` | 复用共享纯 C 领域与 UART 协议 |

F407 路径均相对 `fireware/stm32f407/`。各实现目录继续分头文件和源文件；服务私有状态不向应用或驱动公开。两端已有独立构建依赖，硬件操作已从现场业务服务中下沉。

## 默认行为

- 六从站，温湿度地址 1/3/5、光照地址 2/4/6，九个测点。
- 9600 8N1，默认采集周期 5 s，单次响应等待 200 ms，首次失败最多重试一次。
- 连续三轮通信失败降级，离线后 10 s 单次探测，两次成功恢复。
- 告警按连续三个有效样本和恢复回差确认；失效数据不清除已确认的告警历史。
- W25Q 配置 A/B、256 KiB 循环事件日志；配置保存完成后才发布实际状态。
- HTTP 异步命令与结果查询；MQTT 命令要求 request_id，每次修改一个字段。
- ESP32 outbox 为 1 MiB NVS，1024 条上限，普通遥测保留 128 条事件余量；PUBACK 后出队。
- IWDG 由监督任务统一门控；TIM7 负责 RTU 静默检测，无需先操作 CubeMX。

## 构建与更新

ESP-IDF 5.1.2 环境下执行 `idf.py -B build_nursery build`。

F407 使用 Arm GNU + CMake/Ninja，从根目录执行：

```powershell
cmake -S fireware/stm32f407 -B fireware/stm32f407/build/Nursery -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build fireware/stm32f407/build/Nursery
```

五项主机测试已通过。按用户要求，测试源码、测试替身及测试构建目录已清理，只保留验证结论。

**首次更新必须配套刷两板，并更新 ESP32 分区表。** 此次 UART 版本和 outbox 分区大小有变化，不能只给一侧升级，也不能只通过应用 OTA 更新分区表。详细步骤、参数和验收顺序见当前工程说明。

## 验证边界

两端分层后已重新编译；领域/UART 基线一致性、CAN/Modbus 金样例与依赖边界检查通过。此前主机验证覆盖协议随机载荷、质量/回差、180 个存储写入中断位置及页面异步命令模拟。尚未自动烧录或代为连接 Broker。实际传感器寄存器/倍率、RS485 时序、看门狗、真实断电和新版本 OTA 回滚仍须按联调清单在板上验收。

原 OTA 产品标识保留 `0_helloworld` 以兼容既有清单检查；这不代表仍装配旧学习模块。历史文档中的旧目录、接口和完成度以对应日期为准。
