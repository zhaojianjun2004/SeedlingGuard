# ESP32-S3 环境监测节点（学习项目规划）

## 1. 项目目标

本项目以现有 `0_helloworld` 工程为起点，使用 **ESP32-S3 N16R8** 和 **AHT20 温湿度传感器**，逐步完成一个可以实际使用的小型环境监测节点。

最终设备能够：

1. 通过 I²C 定时读取 AHT20 的温度和湿度；
2. 通过 BLE 近距离查看实时数据、设备状态并修改简单配置；
3. 连接 Wi-Fi，断网后能自动重连；
4. 通过 MQTT 定时上报温湿度和设备状态；
5. 通过 MQTT 接收命令，例如修改采样周期和上报周期；
6. 把必要配置保存到 NVS，重启后继续使用；
7. 在串口日志中清楚显示传感器、BLE、Wi-Fi 和 MQTT 状态。

这个项目会把要学的内容组成一条完整数据链，而不是四个彼此无关的 Demo：

```text
AHT20
  | I²C
  v
传感器采集任务 ---> 最新数据/消息队列 ---> BLE GATT
                         |
                         +-----------------> MQTT ---> 电脑或服务器
                                                ^
                                                | 配置命令
```

## 2. 学习内容

| 功能 | 对应知识点 |
| --- | --- |
| AHT20 采集 | GPIO、I²C、数据手册、驱动封装、错误处理 |
| 周期采样 | FreeRTOS 任务、队列、互斥、任务解耦 |
| BLE | 广播、GATT Service、Characteristic、Read/Write/Notify |
| Wi-Fi | Station 模式、事件循环、获取 IP、自动重连 |
| MQTT | Broker、Topic、发布/订阅、QoS、Last Will、重连 |
| 参数保存 | NVS 键值存储、默认值、配置版本 |
| 工程化 | 组件拆分、日志、状态机、异常恢复、测试 |

项目只需要当前已有的开发板、AHT20、面包板和杜邦线即可开始。电脑上准备 MQTT Broker 或 MQTT 客户端，即可做端到端验证，暂时不依赖屏幕、按键或其他模块。

## 3. 当前工程状态

- 目标芯片已设为 `esp32s3`；
- 当前本地 ESP-IDF 环境为 **v5.1.2**；
- 阶段 1、2 已完成并通过实物采集验证：包含 AHT20 驱动、CRC-8、采样服务和异常恢复；
- 阶段 3 已通过手机端实物验收：可以读取并订阅温湿度，也可以写入采样周期；
- 阶段 4 已通过实物验收：可以连接热点，断网时按指数退避重试，热点恢复后自动重新获取 IP；
- 阶段 5 已通过本地 Mosquitto 和 MQTTX 实物验收：MQTT 上报、订阅、Last Will、运行时配置及 Broker 心跳均正常；
- 阶段 6 的版本化 NVS 配置存储、统一配置服务和启动恢复代码已完成并通过编译，等待断电重启实物验收；
- `main/main.c` 只负责 NVS 初始化、板级配置、依赖适配和模块装配；
- N16R8 已按 **16 MB Flash + 8 MB Octal PSRAM** 配置；
- 已启用 BLE-only NimBLE，设备名默认为 `esp32s3-env`；
- 全部项目源码和注释已统一为 UTF-8。

### 已确认的开发板特征

根据开发板图片和实物差异说明，当前硬件可按以下特征理解：

- ESP32-S3-WROOM 系列 N16R8 模组，即 16 MB Flash + 8 MB PSRAM；
- 板上同时具备 PCB 天线和可选的外接天线座，默认使用 PCB 天线；本项目不需要购买或连接外接天线；
- 板上有 RST 和 BOOT 按键，并有一颗 WS2812 RGB LED；
- 左侧 Type-C 直连 ESP32-S3 的 USB/OTG；
- 右侧 Type-C 经 USB 转串口芯片连接 UART0，图中是 CH343P，实物是 **FT232**；
- FT232 的差异主要在电脑端驱动和 COM 口识别，不改变 ESP-IDF 中 UART0 的日志与下载逻辑；
- 当前 `sdkconfig` 使用 UART0 作为主控制台、USB Serial/JTAG 作为辅助控制台，波特率为 115200。

开始时建议用右侧 FT232 Type-C 口进行下载和串口监视。该接口的一根 USB 线即可同时完成板卡供电、程序下载和串口日志查看。如果 Windows 不出现 COM 口，先确认 USB 线支持数据传输，再检查对应的 FTDI VCP 驱动。左侧原生 USB 后续可用于 USB Serial/JTAG 或 OTG 学习。

### USB 供电与天线说明

开发板不需要单独购买电源才能开始学习。USB 接口输入的 5 V 会经过板载稳压电路转换为 ESP32-S3 使用的 3.3 V；因此，将开发板连接到电脑或合格的 5 V USB 电源即可开机。接好 USB 后，也可以使用开发板的 `3V3` 和 `GND` 引脚给 AHT20 模块供电。

需要注意：

- 下载程序和查看串口日志时，应使用支持数据传输的 USB 线；仅充电线虽然可能可以供电，但电脑不会出现串口；
- 初次调试建议只连接右侧 FT232 Type-C 接口，接线最简单；
- 暂时不要同时从 USB 和外部 `5V/VIN` 引脚给开发板供电，避免不同电源之间相互倒灌；
- AHT20 应连接 `3V3`，不要直接连接 USB 的 5 V 电源；
- 默认 PCB 天线足以完成 BLE、Wi-Fi 和 MQTT 学习，不连接外接天线不会影响本项目；只有明确按开发板说明切换到外接天线路径时，才需要安装匹配的 2.4 GHz 天线。

### N16R8 工程配置

`sdkconfig.defaults` 已保存 N16R8、NimBLE、回滚和自定义分区表等关键配置，当前生成的 `sdkconfig` 也已同步为 16 MB Flash 和 Octal PSRAM。第二阶段使用项目根目录下的 `partitions.csv`：包含 `otadata`、2 MiB 的 `ota_0` 和 2 MiB 的 `ota_1`，为后续下载新固件、切换启动槽和失败回滚提供基础。其余 Flash 暂不分配，等日志、离线数据或文件系统确有需求时再规划，避免为了“用满容量”而增加复杂度。

N16R8 模组中的八线 PSRAM 可能会占用 GPIO35、GPIO36 和 GPIO37。虽然图上开发板将它们引出，本项目不使用这三个引脚。

## 4. 硬件连接计划

AHT20 常见模块有 `VIN/VCC`、`GND`、`SCL`、`SDA` 四个引脚。

| AHT20 | ESP32-S3 | 说明 |
| --- | --- | --- |
| VIN/VCC | 3V3 | 优先使用 3.3 V，先确认所购模块规格 |
| GND | GND | 必须共地 |
| SCL | GPIO9 | 本项目的建议默认值 |
| SDA | GPIO8 | 本项目的建议默认值 |

注意：

- 数据手册给出 2.0-5.5 V 供电范围，推荐值为 3.3 V；与 ESP32-S3 直连时统一使用 3.3 V；
- 裸传感器需要 1 uF 去耦电容，SDA/SCL 需要 2-10 kΩ 上拉（手册典型值 4.7 kΩ）；四针成品模块通常已包含这些器件；
- AHT20 的 7 位 I²C 地址为 `0x38`，不支持可选地址；
- 第一版将 I²C 设为 100 kHz，传感器最高支持 400 kHz；
- 手册建议采样间隔大于 1 秒，避免传感器自热影响精度；本项目使用 2 秒默认周期；
- 接线或改线前先断电；
- GPIO8/GPIO9 在该板上已引出，也避开了启动配置脚 GPIO0/3/45/46、原生 USB 的 GPIO19/20、UART0 以及通常用于板载 RGB LED 的 GPIO48；
- 引脚仍放在项目配置中，不写死在 AHT20 驱动内。

第一轮硬件验收只做三件事：设备能启动、I²C 能发现 `0x38`、能连续输出合理的温湿度。

### AHT20 手册确认的读取流程

1. 上电后等待 40 ms；
2. 读取状态，Bit 3 为校准使能位；
3. 如果未校准，发送 `BE 08 00`，然后等待 10 ms；
4. 发送测量命令 `AC 33 00`；
5. 等待至少 80 ms，再检查状态 Bit 7：`0` 表示测量完成，`1` 表示仍在忙；
6. 读取 6 字节测量数据，建议再读取第 7 字节做 CRC-8 校验；
7. 湿度和温度各使用 20 位原始数据：`RH = raw_h / 2^20 * 100%`，`T = raw_t / 2^20 * 200 - 50`。

CRC 初值为 `0xFF`，多项式为 `0x31`（`x^8 + x^5 + x^4 + 1`）。这些时序和校验会在驱动层实现，并为所有等待设置超时。

### 已有资料

- [AHT20 产品手册 V1.2](../../aht20产品手册%20a2%202020-8-11.pdf)
- [SparkFun AHT20 Arduino 参考库](../../SparkFun_Qwiic_Humidity_AHT20.zip)

SparkFun 压缩包是 Arduino C++ 库，不会直接复制到 ESP-IDF 项目中，但可参考地址、Busy 判断和 20 位数据拼接方式。特别注意：该老版库的初始化参数写成了 `BE 80 00`，而当前产品手册 V1.2 明确给出 `BE 08 00`。实现时以产品手册为主，不直接照搬该处代码。

## 5. 推荐的软件结构

不建议把传感器、BLE、Wi-Fi 和 MQTT 都塞进 `main.c`。随着阶段推进，工程可以逐步整理为：

```text
0_helloworld/
|-- CMakeLists.txt
|-- partitions.csv             # A/B OTA 分区表
|-- README.md
|-- sdkconfig
|-- sdkconfig.defaults          # 已保存 N16R8 和 NimBLE 的关键默认配置
|-- main/
|   |-- CMakeLists.txt
|   |-- main.c                  # 初始化和模块装配
|   `-- Kconfig.projbuild       # GPIO、I²C 频率和采样周期
`-- components/
    |-- aht20/                  # 已实现：器件协议、时序、CRC 和换算
    |-- sensor_service/         # 已实现：总线、采样任务、快照和恢复
    |-- ble_service/            # 已实现：广播、GATT、Notify、BLE 配置入口
    |-- wifi_manager/           # 已实现：Station、状态快照和退避重连
    |-- mqtt_service/           # 已实现：MQTT 上报、订阅、重连和命令队列
    |-- config_store/           # 已实现：与业务无关的 NVS Blob 读写
    |-- app_config_service/     # 已实现：配置校验、应用、版本和持久化
    |-- boot_health_service/    # 已实现：新镜像确认和失败回滚
    |-- firmware_update_service/# 已实现：平台中立的升级事务和状态机
    |-- update_transport_http/  # 已实现：实验室 HTTP 字节流适配器
    |-- update_manifest_json/   # 已实现：Manifest 严格解析和字段边界
    |-- update_integrity_sha256/# 已实现：固件流式 SHA-256 校验
    |-- update_platform_esp32/  # 已实现：ESP-IDF OTA 分区适配器
    |-- ota_button_service/     # 已实现：BOOT 键长按触发 OTA
    `-- ota_console_service/    # 已实现：可选串口 ota 命令入口
```

`config_store` 只封装 NVS Blob 操作，不理解业务字段；`app_config_service` 是运行时配置的唯一权威入口，负责版本、范围校验、运行时应用、持久化以及失败回滚。BLE 和 MQTT 都通过回调访问该服务，不直接调用 NVS。OTA 同样按接口拆分：Core 不知道 HTTP 和 ESP32 Flash API，BOOT 键和可选串口命令都只提交请求，不执行耗时下载。这样以后把 HTTP 换成 CAN 时，升级事务和状态模型仍可复用。

## 6. 核心数据与模块边界

### 6.1 传感器数据

应用层使用统一的数据记录，至少包含：

- 温度（摄氏度）；
- 相对湿度（百分比）；
- 本次采样的运行时间或时间戳；
- 数据是否有效；
- 连续读取失败次数。

AHT20 驱动只负责读取原始值并转换。BLE 和 MQTT 不直接操作 I²C，而是读取 `sensor_service` 保存的最新结果。这样可避免多个模块同时访问传感器，也便于以后替换或增加传感器。

### 6.2 配置项

| 配置 | 建议默认值 | 用途 |
| --- | ---: | --- |
| 采样周期 | 2 秒 | 多久读取一次 AHT20 |
| MQTT 上报周期 | 10 秒 | 多久发布一次数据 |
| 设备名称 | `esp32s3-env` | BLE 广播和设备显示 |
| Wi-Fi 参数 | 不写死在仓库 | 网络名称和密码 |
| MQTT Broker 参数 | 不写死在仓库 | 地址、端口、用户名等 |

收到配置后先检查范围，配置验证通过后再写入 NVS。例如采样周期不能短到持续占用总线，上报周期也不应小于采样周期。

### 6.3 设备状态

应用层统一维护以下状态，供日志、BLE 和 MQTT 使用：

- 传感器：未初始化、正常、故障；
- Wi-Fi：未配置、连接中、已连接、断开；
- MQTT：未连接、连接中、已连接；
- BLE：广播中、已连接；
- 最近一次错误和累计错误次数。

## 7. BLE 功能设计

第一版采用一个自定义 GATT Service，先用通用 BLE 调试 App 验证，不马上开发手机 App。

| Characteristic | 权限 | 内容 |
| --- | --- | --- |
| Temperature | Read + Notify | 当前温度 |
| Humidity | Read + Notify | 当前湿度 |
| Device Status | Read + Notify | 传感器、Wi-Fi、MQTT 状态 |
| Sample Interval | Read + Write | 查看或修改采样周期 |
| Report Interval | Read + Write | 查看或修改 MQTT 上报周期 |

连接后应能读取温湿度，订阅 Notify 后能收到变化，写入周期后设备会校验并应用新值。

### BLE 与 Wi-Fi 配网

建议分两步学习：

1. 先完成自定义 GATT，理解广播、连接、读写和 Notify；
2. 基础稳定后，再决定是否加入 ESP-IDF 的 BLE 配网功能。

第一版可以先通过 `menuconfig` 或本地私有配置提供 Wi-Fi 参数。不要把真实 Wi-Fi 密码提交到 Git。

## 8. Wi-Fi 功能设计

设备使用 Wi-Fi Station 模式连接路由器。`wifi_manager` 负责：

- 初始化网络接口和事件循环；
- 发起连接；
- 处理连接中、获取 IP、断开等事件；
- 使用有最大等待上限的指数退避策略自动重连；
- 通过线程安全的状态快照向其他模块提供连接状态、IP 和错误信息。

MQTT 只在已获得 IP 后启动。Wi-Fi 断开时传感器仍继续更新本地数据，网络恢复后再恢复上报。

## 9. MQTT 功能设计

开发阶段可先使用电脑上的本地 Broker（例如 Mosquitto），稳定后再考虑局域网服务器或云端 Broker。

### Topic 约定

```text
env/<device_id>/telemetry       设备发布温湿度
env/<device_id>/status          设备发布在线和模块状态
env/<device_id>/config/set      设备订阅配置命令
env/<device_id>/config/state    设备发布当前配置
```

`<device_id>` 由芯片唯一标识生成，避免多块板的 Topic 和 MQTT Client ID 冲突。

遥测消息示例：

```json
{
  "temperature_c": 25.4,
  "humidity_rh": 58.2,
  "uptime_ms": 123456,
  "sensor_ok": true
}
```

配置命令示例：

```json
{
  "sample_interval_s": 5,
  "report_interval_s": 30
}
```

第一版策略：

- 遥测使用 QoS 0，优先简单和低开销；
- 配置命令可使用 QoS 1；
- `status` 使用保留消息，并配置 Last Will 表示意外离线；
- 收到配置后校验、原子应用并保存到 NVS，再发布 `config/state` 作为确认；
- MQTT 回调中不做耗时操作，只把消息交给应用任务处理；
- Broker 密码、证书私钥不写入公开仓库。

可信局域网内可先用未加密 MQTT 学习流程；如果 Broker 跨越不可信网络，则必须增加 TLS 和服务端身份校验。

## 10. FreeRTOS 任务与数据流

| 执行单元 | 职责 |
| --- | --- |
| `sensor_task` | 按周期读取 AHT20，更新最新数据，发送数据事件 |
| `app_task` | 处理配置变更和跨模块协调 |
| Wi-Fi/BLE/MQTT 系统任务 | 由 ESP-IDF 组件创建，不重复造轮子 |

模块间优先使用队列、事件组或受保护的快照，不使用无保护的全局变量。网络回调只解析必要信息并投递事件，实际存储、日志和配置应用放到应用任务执行。

## 11. 分阶段实施路线

### 阶段 0：确认开发环境和板卡

- 记录开发板完整型号和引脚图；
- 确认 ESP-IDF 版本和目标芯片；
- 核对 16 MB Flash、8 MB PSRAM 的实际参数；
- 编译、烧录并监视当前 Hello World；
- 记录实际串口和下载方式。

验收：重启、下载和日志输出稳定，配置与实际硬件一致。

### 阶段 1：AHT20 与 I²C

- 完成接线，初始化 I²C Master；
- 扫描并确认地址 `0x38`；
- 按手册时序完成上电等待、校准检查、触发测量和 Busy 轮询；
- 检查状态位，解析 20 位温度和湿度原始数据；
- 验证 CRC-8，丢弃校验失败的数据；
- 为超时、无应答和异常数据打印明确日志。

验收：连续运行至少 10 分钟，读数合理；拔掉传感器后不会死机，重新连接并重启后可恢复。

### 阶段 2：整理驱动与 FreeRTOS 数据流

- 把 AHT20 器件操作封装为独立组件；
- 建立 `sensor_service` 和统一数据结构；
- 使用任务周期采样，线程安全地提供最新快照；
- 区分驱动错误、数据无效和应用状态。

验收：业务模块不直接调用底层 I²C，修改采样周期不会造成并发访问。

### 阶段 3：BLE 本地查看与配置

- 使用标准 Environmental Sensing Service，并增加自定义控制 Service；
- 实现温度、湿度和状态的 Read/Notify；
- 实现周期配置的 Write；
- 处理连接、断开和重新广播；
- 后续再增加配对、加密或 BLE 配网。

验收：BLE 调试 App 能发现设备、读取数据、订阅通知并修改合法配置；非法值会被拒绝且设备不崩溃。

### 阶段 4：Wi-Fi 联网

- 完成 Station 模式和事件处理；
- 获取 IP 后向应用层报告联网；
- 断开后按退避策略重连；
- 将 Wi-Fi 状态写入线程安全快照，并通过串口日志观察。

验收：关闭再打开路由器后，设备无需重启即恢复连接；网络故障不影响本地传感器采样。

### 阶段 5：MQTT 上报与命令

- 连接 Broker；
- 发布 telemetry、status 和 config/state；
- 订阅 config/set；
- 处理断线重连、订阅恢复、遗嘱和保留消息；
- 通过电脑端 MQTT 客户端观察和控制设备。

验收：电脑能持续看到温湿度；发送配置后设备应用新周期并回报；Broker 重启后设备自动恢复。

### 阶段 6：持久化和健壮性

- 使用 NVS 保存有效配置；
- 定义默认值和恢复默认配置的方法；
- 增加配置版本，便于未来迁移；
- 统计传感器、Wi-Fi、MQTT 错误；
- 进行长时间运行和反复断网测试；
- 评估是否需要看门狗、TLS、低功耗或 OTA。

验收：断电重启后配置仍在；配置损坏时能回退到安全默认值；运行 24 小时无明显异常。

## 12. 第一阶段暂不做

- 自制手机 App；
- 显示屏和网页管理后台；
- 公网云平台接入；
- OTA 固件升级（现已进入第二阶段实现）；
- 深度睡眠和电池供电优化；
- Home Assistant 自动发现；
- 复杂的历史数据存储。

第一阶段基础闭环已经完成。第二阶段选择 OTA 作为主线，先建立 A/B 分区、待验证固件的启动自检以及确认/回滚机制，再加入实际固件下载通道。TLS、Home Assistant、网页仪表盘和低功耗暂不并行扩展。

## 13. 调试与安全原则

- 每个阶段先单独验证，再与下一模块集成；
- 使用 ESP-IDF 统一日志接口，为模块设置清楚的日志标签；
- 日志可显示 SSID、IP、Broker 地址和状态，但不打印密码；
- 所有外部输入都要校验，包括 BLE 写入和 MQTT JSON；
- 所有等待都设置超时，不因传感器或网络故障永久卡死；
- 断网、Broker 不可用、AHT20 拔出都应是可恢复故障；
- 查资料时优先匹配当前 ESP-IDF v5.1.2，避免混用其他版本示例的 API。

## 14. 后续阶段需确认

1. 完成本地 Broker 学习后，是否继续增加云端 Broker、TLS 和证书校验；
2. 第一版 Wi-Fi 参数继续通过本地配置写入，还是扩展为 BLE 配网。

当前实验状态：**`1.1.1 -> 1.1.2` 正常升级、`1.1.3 -> 1.1.2` 故障回滚、`1.1.2 -> 1.2.0` 协议过渡升级以及 `1.2.0 -> 1.2.1` Manifest 升级均已实机验证。开发板当前从 `ota_1` 运行已确认有效的 `1.2.1 / release_sequence=121`。**

## 15. 当前代码说明

### 15.1 分层职责

| 层次 | 代码 | 职责 |
| --- | --- | --- |
| 设备驱动层 | `components/aht20` | AHT20 命令、时序、Busy 检查、CRC-8、20 位数据换算 |
| 业务服务层 | `components/sensor_service` | I²C 总线、FreeRTOS 采样任务、最新数据快照、错误计数和恢复 |
| 存储适配层 | `components/config_store` | NVS Blob 读取、提交、删除和底层错误转换 |
| 配置业务层 | `components/app_config_service` | 默认值、版本化记录、范围校验、原子更新、持久化和失败回滚 |
| BLE 表现层 | `components/ble_service` | NimBLE、广播、GATT、通知和采样周期写入 |
| 网络管理层 | `components/wifi_manager` | Wi-Fi Station、事件处理、连接状态快照和自动重连 |
| 消息服务层 | `components/mqtt_service` | MQTT 连接、Topic、遥测发布、命令队列、JSON 校验和会话恢复 |
| 升级事务层 | `components/firmware_update_service` | 单会话状态机、镜像前置检查、进度、失败清理和重启调度 |
| 升级传输层 | `components/update_transport_http` | 打开 HTTP URI、顺序读取字节流、超时和连接清理 |
| 清单解析层 | `components/update_manifest_json` | JSON 字段唯一性、类型、长度、Schema 和 SHA-256 文本解析 |
| 完整性层 | `components/update_integrity_sha256` | 对下载字节流计算 SHA-256，并与 Manifest 摘要比较 |
| 升级平台层 | `components/update_platform_esp32` | 选择备用槽、写入、镜像校验、激活和中止 |
| 启动确认层 | `components/boot_health_service` | 检查 pending verify、新固件确认和主动回滚 |
| 按键入口层 | `components/ota_button_service` | 检测 BOOT 键长按并提交一次升级请求，不执行下载 |
| 可选调试入口层 | `components/ota_console_service` | `ota start`、`ota status`；默认关闭，不在命令回调内下载 |
| 应用装配层 | `main/main.c` | 初始化 NVS、注入数据回调、创建并启动各服务 |

驱动层不知道 SDA/SCL 是哪个 GPIO，也不创建采样任务。业务层不自行解析 AHT20 原始字节。BLE 组件甚至不依赖 `sensor_service`：`main.c` 通过回调适配两边的数据结构。这样后续增加 MQTT 时也能复用相同快照，而不会让无线模块直接访问 I²C。

### 15.2 已实现的异常处理

- 所有公开接口检查空指针和参数范围；
- AHT20 完整测量序列受互斥锁保护；
- 每次 I²C 访问、Busy 等待和服务停止都有超时；
- CRC 错误不会更新有效测量；
- AHT20 未接入或中途断开时，设备不会重启，业务任务会继续重试；
- 每累计 3 次失败尝试软复位，然后重新初始化；
- 最新数据通过互斥锁整体更新和复制，避免其他任务读到一半新、一半旧的数据。

### 15.3 可配置项

在 VS Code 中执行 `ESP-IDF: SDK Configuration editor (menuconfig)`，打开 `Environment monitor configuration`，可调整：

- AHT20 SDA GPIO，默认 GPIO8；
- AHT20 SCL GPIO，默认 GPIO9；
- I²C 频率，默认 100000 Hz；
- 采样周期，默认 2000 ms；
- BLE 设备名，默认 `esp32s3-env`；
- Wi-Fi SSID 和密码，默认留空；
- Wi-Fi 首次重连等待时间，默认 1000 ms；
- Wi-Fi 最大重连等待时间，默认 30000 ms；
- MQTT Broker URI，默认留空；
- MQTT 用户名和密码，默认留空；
- MQTT 遥测上报周期，默认 10000 ms；
- OTA 首次启动确认等待时间，默认 5000 ms；
- HTTP OTA 读取超时，默认 10000 ms；
- 当前固件发布序号，`1.2.1` 为 121；
- OTA 硬件兼容标识，默认 `esp32s3-n16r8`；
- Manifest 最大长度，默认 2048 字节；
- 允许降级开关，默认关闭，仅用于受控实验；
- OTA 下载缓冲区，默认 4096 字节；
- OTA 成功后的重启等待时间，默认 2000 ms；
- BOOT 键 OTA 触发开关，默认开启；
- OTA 触发 GPIO，默认 GPIO0；
- BOOT 键长按时间，默认 2000 ms；
- 实验固件 URI，默认留空，需要填写电脑局域网地址；
- 交互式 OTA 串口控制台，默认关闭；
- OTA 启动自检强制失败开关，仅用于回滚实验。

SSID 留空时不会启动 Wi-Fi，但 AHT20 采样和 BLE 会继续正常工作。Wi-Fi 密码会写入本机生成的 `sdkconfig`，不要把包含真实密码的文件提交到公开仓库或发给他人。

### 15.4 编译、烧录和查看日志

当前工程已在 ESP-IDF v5.1.2 下完成全量编译。开发板当前从 `ota_1` 运行并确认了 `1.2.1 / release_sequence=121`；固件大小为 `0x113bd0` 字节（1129424 字节，约 1.08 MiB），每个 OTA 槽还剩约 46%。实验使用的 `manifest.json` 已与该二进制的大小和 SHA-256 对齐，并通过实机升级验证。

1. 按本 README 完成 AHT20 接线；
2. 将电脑连到右侧 FT232 Type-C 接口；
3. 在 VS Code 状态栏确认 FT232 对应的 COM 口；
4. 只有第一次部署分区表和基线固件时才执行 `ESP-IDF: Build, Flash and Start a Monitor on Your Device`；
5. 当前 OTA 实验只保持 HTTP Server 与串口 Monitor 运行，通过长按 BOOT 触发，串口波特率为 115200。

正常时日志会显示 AHT20 初始化成功、NimBLE Host 启动、BLE 地址以及 `Advertising as 'esp32s3-env'`。后台仍按配置周期采样，但成功采样和成功上报日志已降为 DEBUG，INFO 级别主要保留初始化、状态变化和错误，便于观察 OTA。如果看到 `ESP_FAIL`，优先检查 3V3、GND、SDA/SCL 是否接反、面包板是否接触良好。

### 15.5 BLE GATT 接口

| Service / Characteristic | UUID | 权限 | 数据格式 |
| --- | --- | --- | --- |
| Environmental Sensing | `0x181A` | Primary Service | Bluetooth SIG 标准环境服务 |
| Temperature | `0x2A6E` | Read + Notify | little-endian `int16`，单位 0.01 ℃ |
| Humidity | `0x2A6F` | Read + Notify | little-endian `uint16`，单位 0.01 %RH |
| Environment Control | `7f6a0001-6c7b-4e7d-9f7d-4bf3a9b91000` | Primary Service | 本项目控制服务 |
| Device Status | `7f6a0002-6c7b-4e7d-9f7d-4bf3a9b91000` | Read + Notify | 14 字节版本化状态帧 |
| Sample Interval | `7f6a0003-6c7b-4e7d-9f7d-4bf3a9b91000` | Read + Write | little-endian `uint32`，单位 ms |

状态帧格式：字节 0 是版本（当前为 1）；字节 1 的 bit0 表示测量有效、bit1 表示传感器错误；字节 2～5 是成功采样序号；字节 6～9 是连续失败次数；字节 10～13 是最后一个 `esp_err_t`。多字节字段均为 little-endian。

采样周期允许范围是 1000～600000 ms。比如要写入 5000 ms，十六进制是 `0x00001388`，手机 BLE 工具中应按 little-endian 写入四个字节：`88 13 00 00`。非法长度或超出范围的值会被拒绝。

### 15.6 手机端验收步骤

1. 烧录并打开串口监视器，确认日志出现 `Advertising as 'esp32s3-env'`；
2. 使用支持 GATT 的 BLE 调试 App 扫描并连接 `esp32s3-env`；
3. 在 Environmental Sensing Service 中读取温度和湿度；
4. 打开两个特征值的 Notify，确认每次 AHT20 成功采样后都会收到更新；
5. 读取 Sample Interval，默认应得到 `D0 07 00 00`，即 2000 ms；
6. 写入 `88 13 00 00`，确认串口日志变成每 5 秒采样一次；
7. 尝试写入小于 1000 ms 的值，确认手机端提示写入失败且设备继续运行；
8. 主动断开连接，确认设备重新广播并可再次连接。

### 15.7 Wi-Fi 联网验收步骤

本阶段先使用 `menuconfig` 提供网络参数，不增加 BLE 配网，也不改变现有 BLE 数据格式。

1. 手机热点或路由器启用 **2.4 GHz** Wi-Fi；ESP32-S3 不能连接仅有 5 GHz 的热点；
2. 在 VS Code 中执行 `ESP-IDF: SDK Configuration editor (menuconfig)`；
3. 打开 `Environment monitor configuration`，填写 `Wi-Fi SSID` 和 `Wi-Fi password` 并保存；
4. 执行 `ESP-IDF: Build, Flash and Start a Monitor on Your Device`；
5. 正常时先看到 `Wi-Fi Station started; connecting to SSID '...'`，成功获取地址后看到 `Wi-Fi connected; IPv4=...`；
6. 保持设备运行，关闭热点，确认日志出现断开原因以及 1000、2000、4000 毫秒等逐步增加的重连等待时间，最大不超过 30000 毫秒；
7. 重新打开同名热点，确认设备无需重启即可重新获得 IPv4 地址；
8. 在整个断网和重连过程中，确认 AHT20 日志仍持续输出，并且 BLE 仍可使用。

若一直无法连接，先核对 SSID 和密码的大小写，再确认热点是 2.4 GHz 或已开启兼容模式。手机热点重启后如果改变了名称或密码，也需要重新执行配置和烧录。

### 15.8 MQTT 联调验收步骤

本阶段使用局域网内的普通 MQTT TCP 连接。Broker 和 ESP32 必须能够互相访问；Broker URI 中不能填写 `localhost` 或 `127.0.0.1`，因为这两个地址在 ESP32 看来代表 ESP32 自己。

1. 在电脑或局域网设备上启动 MQTT Broker，监听局域网接口的 TCP 1883 端口；
2. 确认电脑防火墙允许 Broker 接收入站连接，并查询电脑在当前 Wi-Fi 下的 IPv4 地址；
3. 打开 `Environment monitor configuration`，将 `MQTT Broker URI` 填为类似 `mqtt://192.168.45.100:1883`；Broker 允许匿名连接时，用户名和密码留空；
4. MQTT 客户端先订阅 `env/+/telemetry`、`env/+/status` 和 `env/+/config/state`；
5. 编译并烧录后，串口会打印本板的 12 位 `MQTT device ID` 和完整的 `Telemetry topic`；
6. Wi-Fi 获得 IP 后，应看到 `MQTT broker connected`、`Subscribed to .../config/set`，随后每 10 秒打印一次 `Telemetry published`；
7. MQTT 客户端应收到类似以下遥测消息：

```json
{
  "temperature_c": 24.41,
  "humidity_rh": 70.89,
  "uptime_ms": 123456,
  "sensor_ok": true,
  "sequence": 62
}
```

8. 向串口打印出的设备 ID 对应主题 `env/<device_id>/config/set` 发布：

```json
{
  "sample_interval_s": 5,
  "report_interval_s": 30
}
```

9. 确认串口采样改为每 5 秒一次、遥测改为每 30 秒一次，并从 `env/<device_id>/config/state` 收到保留的当前配置；
10. 尝试发送小于 1 秒、超过允许范围或格式错误的 JSON，确认命令被拒绝且设备继续运行；
11. 重启 Broker，确认 ESP32 自动重新连接、恢复订阅并继续上报；异常掉线时，订阅端会从保留的 `status` Topic 收到 `{"online":false}`，恢复后收到 `{"online":true}`。

阶段 6 固件会将成功修改的采样周期和上报周期保存到 NVS，普通重启和断电不会丢失。执行整片 Flash 擦除会删除用户配置，下一次启动将重新使用 `menuconfig` 中的安全默认值。如果使用手机热点组网，还要注意部分热点会隔离不同客户端；这种情况下电脑上的 Broker 可能无法被 ESP32 访问。

### 15.9 MQTT 联调常见问题与解决方案

#### `Kconfig.projbuild` 中修改了 URI 默认值，但设备仍提示未配置

ESP-IDF 中这几个文件的职责不同：

- `main/Kconfig.projbuild` 定义项目配置菜单、配置项类型、帮助文字和初始默认值；
- `sdkconfig` 保存当前工程实际选择的配置值，编译时以这里为准；
- `build/config/sdkconfig.h` 由构建系统生成，是 C 代码最终读取的宏定义；
- `sdkconfig.defaults` 用于新建或重新生成 `sdkconfig` 时提供可复现的工程默认配置。

如果 `sdkconfig` 已经存在 `CONFIG_APP_MQTT_BROKER_URI=""`，后来只修改 `Kconfig.projbuild` 中的 `default`，旧的空值不会被覆盖。这是 Kconfig 的正常行为，不是 MQTT 组件故障。当前连接地址应通过 `menuconfig` 修改并保存到 `sdkconfig`，不要通过修改配置项的默认值来切换实际 Broker。

若 VS Code 的 `Environment monitor configuration` 没有显示新加入的 MQTT 配置项，可以关闭旧的 SDK Configuration Editor 标签页，执行 `ESP-IDF: Reconfigure Project`，再重新打开配置编辑器。也可以在 ESP-IDF 终端执行：

```powershell
idf.py reconfigure
idf.py menuconfig
```

保存后，可检查 `sdkconfig` 和 `build/config/sdkconfig.h` 中的 `CONFIG_APP_MQTT_BROKER_URI` 是否为预期值，然后重新编译和烧录。

#### 固件可以生成，但最后提示 App 分区太小

同时启用 AHT20、NimBLE、Wi-Fi 和 MQTT 后，完整固件大小超过了默认 1 MiB App 分区。典型错误为：

```text
Error: app partition is too small for binary
Part 'factory' ... size 0x100000
```

当前工程已经改为自定义 `partitions.csv`，提供 `otadata`、`ota_0` 和 `ota_1`。修改分区方案后应执行完整的 Build + Flash，让新分区表、初始 OTA 数据和 App 一起写入 Flash；只写入 `0_helloworld.bin` 会使设备继续使用旧分区表。若构建仍显示旧的 `factory` 分区，先执行 `ESP-IDF: Full Clean`，再重新 Build。

#### 第二阶段：A/B 分区、首次启动确认与回滚

`boot_health_service` 负责读取并记录当前运行分区、下一个更新分区、固件版本、复位原因和镜像状态。只有当 bootloader 将新镜像标记为 `ESP_OTA_IMG_PENDING_VERIFY` 时，服务才会启动延时自检：

1. 等待 `APP_OTA_HEALTH_CONFIRMATION_DELAY_MS`，让必要服务完成初始化；
2. 调用应用层自检回调，检查进程内部的必要服务是否已创建；
3. 自检通过后调用 `esp_ota_mark_app_valid_cancel_rollback()` 确认新固件；
4. 自检失败时调用 `esp_ota_mark_app_invalid_rollback_and_reboot()`，使 bootloader 回到上一槽固件。

自检刻意不要求 AHT20、Wi-Fi 或 MQTT Broker 此刻在线。它们属于可能暂时不可用的外部依赖，不能因为传感器未接、路由器断电或服务器维护就把结构正确的新固件判为坏版本。

`APP_OTA_FORCE_BOOT_HEALTH_FAILURE` 是回滚实验使用的故障注入开关。它只会影响处于 `PENDING_VERIFY` 状态的新 OTA 镜像；普通串口全量烧录后的常规启动不会无条件重启。

### 15.10 HTTP OTA 实验步骤

当前实现把职责分成四段：`ota_button_service` 只把长按事件转换为升级请求；`firmware_update_service` 负责单会话事务和状态；`update_transport_http` 只提供顺序字节流；`update_platform_esp32` 只包装 ESP-IDF 分区操作。`ota_console_service` 保留为可选调试入口，但默认关闭，因为持续输出日志的串口监视器并不适合交互式输入。一次升级的主路径是：

```text
BOOT 键持续按下 2 秒
  -> 按键服务提交预配置 URI
  -> 检查 Wi-Fi 和会话状态
  -> HTTP 获取 Content-Length 和镜像字节
  -> 检查 ESP 镜像头、项目名和版本
  -> 写入当前未运行的 OTA 槽
  -> esp_ota_end() 完整性校验
  -> 将新槽设为启动分区并重启
  -> 新固件处于 pending verify
  -> 延时健康检查
  -> 确认有效，或标记无效并回滚
```

第一次实验按以下顺序进行：

1. 通过 `ipconfig` 查电脑与 ESP32 同一网络下的 IPv4 地址。不要填写 `127.0.0.1`，因为它在 ESP32 上表示 ESP32 自己；
2. 在 `menuconfig -> Environment monitor configuration` 中填写 `Lab OTA firmware URI`，格式为 `http://电脑IPv4:8000/0_helloworld.bin`；确认 BOOT 键触发已开启、GPIO 为 0、串口控制台保持关闭；
3. 保持根目录 `CMakeLists.txt` 中 `PROJECT_VER` 为 `1.1.1`，执行完整 Build + Flash + Monitor。这一步会写入 bootloader、A/B 分区表、初始 OTA 数据和具有按键触发能力的基线 App；
4. 确认日志包含 `App version: 1.1.1`、`Running partition: label=ota_0`、`Next update partition: label=ota_1`、`This boot does not require OTA confirmation` 和 `Hold BOOT button for 2000 ms to request OTA`。默认不再出现 `env>`；
5. 将 `PROJECT_VER` 改为 `1.1.2`，只执行 Build，不要执行 Flash，否则就绕过了 OTA 下载过程；
6. 在另一个 PowerShell 中进入项目根目录，启动本地文件服务器：

```powershell
python -m http.server 8000 --directory build
```

7. 开发板已经连上 Wi-Fi 后，按住 BOOT 键至少 2 秒；看到 `OTA request submitted; release BOOT button` 后立即松开；
8. 观察 10% 粒度的下载进度、目标分区 `ota_1`、镜像激活和自动重启；
9. 新版本启动后应先显示 `pending_verify`，约 5 秒后出现 `Boot health check passed; firmware marked valid`，并显示 `App version: 1.1.2`。

GPIO0 同时也是 ESP32-S3 的启动绑带引脚，因此日志提示提交成功后必须松开 BOOT 键，再让 OTA 流程自动重启。如果重启期间仍把 GPIO0 拉低，芯片可能进入 ROM 下载模式；此时松开按键并按一下 RST 即可恢复正常启动。

首次实机升级已验证通过：`1.1.1` 从 `ota_0` 运行，通过 HTTP 下载 `1.1.2` 到 `ota_1`；新固件首次启动状态为 `pending_verify`，5 秒后应用自检通过并标记为 `valid`。升级期间的 AHT20 失败来自当前小模块引脚未焊接造成的接触不良，不属于 OTA 故障，也没有被健康检查错误地当作回滚条件。

若按键提交后提示网络未就绪，等待 Wi-Fi 获得 IPv4 后松开并重新长按。若电脑 HTTP Server 没有访问记录，检查 Windows 防火墙、手机热点的客户端隔离以及 IP/端口。若提示版本已经运行，说明提供的仍是当前版本固件；必须修改 `PROJECT_VER` 并重新 Build。

主动回滚也已实机验证：`1.1.3` 下载到 `ota_0` 并以 `pending_verify` 启动，故障注入使健康检查返回 `ESP_FAIL`，ESP-IDF 随后自动回到 `ota_1` 中状态为 `valid` 的 `1.1.2`。故障注入开关已经关闭。

当前 HTTP 适配器只允许局域网实验用的 `http://`。从 `1.2.0` 起，兼容性、版本和 SHA-256 由独立 Manifest/Integrity 组件处理，HTTP 层仍只负责字节流。

### 15.11 Manifest、发布序号与 SHA-256

从 `1.2.0` 开始，按键提交的是 `manifest.json` 地址，而不是裸 `.bin` 地址。Core 按以下顺序处理：

```text
下载有最大长度限制的 Manifest
  -> 严格解析唯一字段和数据类型
  -> 检查 product、hardware、version、release_sequence
  -> 打开 image_url，并核对 Content-Length == image_size
  -> 检查镜像头的项目名/版本与 Manifest 一致
  -> 一边写备用槽，一边计算 SHA-256
  -> 摘要、ESP 镜像完整性全部通过后才激活新槽
```

Manifest 格式如下：

```json
{
  "schema_version": 1,
  "product": "0_helloworld",
  "hardware": "esp32s3-n16r8",
  "version": "1.2.1",
  "release_sequence": 121,
  "image_url": "http://192.168.45.139:8000/0_helloworld.bin",
  "image_size": 1129424,
  "sha256": "64个十六进制字符"
}
```

版本字符串用于人阅读，`release_sequence` 才用于机器判断新旧。Core 默认要求目标序号严格大于当前序号，因此不会依赖字符串字典序去比较 `1.10.0` 和 `1.9.0`。产品、硬件、长度、镜像头版本或摘要任一不一致，都不会激活新槽。

工程提供 `tools/generate_ota_manifest.py`，构建新版本后可生成清单：

```powershell
python tools/generate_ota_manifest.py `
  --binary build/0_helloworld.bin `
  --output build/manifest.json `
  --product 0_helloworld `
  --hardware esp32s3-n16r8 `
  --version 1.2.1 `
  --release-sequence 121 `
  --image-url http://192.168.45.139:8000/0_helloworld.bin
```

协议过渡分为两步：板上的 `1.1.2` 只认识裸固件 URL，因此先沿用旧入口把 `1.2.0` 写入备用槽；随后由 `1.2.0` 读取 `1.2.1 + manifest.json` 并完成新流程。两步均已实机验证。直接让 `1.1.2` 请求 Manifest 会因为旧固件不认识 JSON 而失败。

SHA-256 可以发现下载损坏或 Manifest 与镜像不一致，但不能证明发布者身份。当前 Manifest 和固件仍通过明文 HTTP 获取，攻击者若能同时替换两者，仍可重新计算摘要。因此这一阶段不能宣称具备量产安全 OTA；后续仍需 HTTPS 身份校验或离线数字签名、可信公钥和密钥生命周期设计。

当前发布序号编译在 Application 内，适合验证“普通升级请求不得降级”的策略，但它会随 Application 一起回滚，不能充当不可逆的安全计数器。量产防回滚必须把可信的单调版本放在 Bootloader 可保护的冗余元数据、一次性熔丝或其他受保护存储中，并由不可随 App 降级的验证链执行检查；F103 BMS 也要按这个原则单独设计。

#### MQTTX 连接成功，但看不到 ESP32 的遥测

MQTTX 显示绿色只代表它连接到了某个 Broker，并不能证明它与 ESP32 连接的是同一个 Broker。联调时应核对：

1. ESP32 使用的 URI 和 MQTTX Host 指向同一台电脑、同一端口；
2. Mosquitto 控制台同时出现 ESP32 和 MQTTX 两个客户端的连接记录；
3. ESP32 发布 telemetry 后，Mosquitto 日志中出现向 MQTTX 转发 PUBLISH 的记录；
4. MQTTX 已订阅正确的 Topic。

如果 ESP32 连接 `192.168.x.x:1883`，而 MQTTX 连接 `127.0.0.1:1883` 后只能看到自己发送的消息，电脑上可能存在分别监听回环地址和局域网地址的两个 Broker 实例。此时应让 MQTTX 也连接 ESP32 所使用的局域网 IP。`127.0.0.1` 只有在能够确认它与局域网地址由同一个 Broker 进程监听时才等价。

推荐在 MQTTX 中分别订阅：

```text
env/+/telemetry
env/+/status
env/+/config/state
```

调试时也可以临时订阅 `env/#`。其中 `+` 只匹配一层 Topic，`#` 匹配后续所有层级。

#### 在 MQTTX 中发送一条命令，却看到两条相同内容

如果 MQTTX 订阅了 `env/#`，它发布到 `env/<device_id>/config/set` 后，Broker 还会按照订阅规则把这条消息投递回 MQTTX。因此界面会同时显示：

- 一条 MQTTX 本地的 Published 消息；
- 一条 Broker 返回的 Received 消息。

这不是 ESP32 重复执行，也不是 Broker 重复发布。发布 QoS 与订阅 QoS 可以不同，例如命令以 QoS 1 发布，而 `env/#` 以 QoS 0 订阅，回显消息就会显示 QoS 0。若不需要这种回显，使用前面的三个精确订阅，不订阅 `config/set`。

#### 配置命令已发送，但采样和上报周期没有改变

向以下主题发送命令，`<device_id>` 必须与串口打印的 12 位设备 ID 完全一致：

```text
env/<device_id>/config/set
```

Payload 示例：

```json
{
  "sample_interval_s": 5,
  "report_interval_s": 30
}
```

MQTTX 中消息格式选择 JSON，QoS 选择 1，并关闭 `Retain`。控制命令不应保留，否则设备重新连接 Broker、重新订阅 `config/set` 时，Broker 会自动下发旧命令。设备会把该旧命令当作一条正常配置再次应用并保存到 NVS，导致刚刚恢复或新设置的值被覆盖。

典型现象是：设备启动时先从 NVS 打印正确的 `Configuration active: ... source=nvs`，随后 MQTTX 在很短时间内收到两条 `config/state`；第一条是 NVS 恢复值，第二条又变成旧值。此时不是 NVS 失效，而是 retained 命令覆盖了 NVS。

阶段 6 固件增加了第二层防护：设备重新订阅后，如果 Broker 回放的 `config/set` 消息带有 retained 标志，MQTT 服务会拒绝执行、增加拒绝命令计数，并打印 `Rejected retained MQTT command`。这样旧命令不会再次覆盖从 NVS 恢复的配置。

即使设备能够拒绝，仍应清理 Broker 中的错误保留消息，避免每次重连都产生警告。清理时在 MQTTX 中向相同 Topic 发布一个**长度为 0 的空消息**，临时打开 `Retain` 并使用 QoS 1。这会让 Broker 删除该 Topic 保存的旧命令。发送后立即重新关闭 `Retain`，再发送实际 JSON 配置。`config/state` 和 `status` 由设备主动使用 Retain 发布是正常设计；只有 `config/set` 必须保持非保留。

不能仅根据 MQTTX 中出现 Published 或自身回显判断设备已经执行命令。应同时验证：

1. 串口出现 `Sample period updated to 5000 ms`；
2. 串口出现 `MQTT configuration applied: sample=5000 ms, report=30000 ms`；
3. MQTTX 从 `env/<device_id>/config/state` 收到设备回报的新配置；
4. 传感器日志随后约每 5 秒出现一次，遥测日志约每 30 秒出现一次。

上报周期改变后，调度器不会强制从命令到达时重新开始计时，因此第一条遥测与命令之间不一定正好间隔 30 秒，最长等待约为新的上报周期；后续才会稳定按新周期上报。采样周期允许 1～600 秒，上报周期允许 1～3600 秒。阶段 6 固件会保存成功更新后的完整配置，设备重启后从 NVS 恢复。

### 15.12 NVS 配置持久化设计与验收

运行时配置采用单一权威入口，数据流如下：

```text
BLE / MQTT
     |
     v
app_config_service -----> sensor_service / mqtt_service
     |
     v
config_store -----------> NVS
```

`config_store` 只保存和读取二进制 Blob，并将 NVS 专属的“记录不存在”错误转换为通用错误码。`app_config_service` 保存版本 1 配置记录，记录包含魔数、版本号、结构长度、采样周期和 MQTT 上报周期。NVS 自身负责 Blob CRC，业务层额外检查版本、长度和字段范围。

启动顺序为：初始化 NVS、创建传感器对象、创建配置服务、加载并应用配置、启动采样任务，最后启动 BLE、Wi-Fi 和 MQTT。这样第一次传感器采样就会使用恢复后的周期。

一次配置更新遵循以下规则：

1. 在配置互斥锁内基于当前值生成候选配置；
2. 对完整候选配置进行范围校验；
3. 将变化的采样周期应用到传感器服务；
4. 将完整配置作为一个版本化 Blob 提交到 NVS；
5. NVS 提交失败时回滚刚才应用的采样周期；
6. 只有应用和持久化都成功，才更新内存中的权威配置并向调用方返回成功；
7. 新值与当前值完全相同时不重复写 Flash。

建议按以下步骤进行实物验收：

1. 首次烧录后启动，若 NVS 中没有用户配置，应看到：

```text
app_config: No saved configuration; using project defaults
app_config: Configuration active: sample=2000 ms, report=10000 ms, source=defaults
```

2. 通过 MQTTX 向 `env/<device_id>/config/set` 发送：

```json
{
  "sample_interval_s": 5,
  "report_interval_s": 30
}
```

3. 确认串口出现 `Configuration saved: sample=5000 ms, report=30000 ms`，随后 MQTT 服务打印配置已应用；
4. 按 RST 或拔掉 USB 后重新上电，确认启动日志出现：

```text
app_config: Configuration active: sample=5000 ms, report=30000 ms, source=nvs
```

5. 确认重启后传感器仍约每 5 秒采样，MQTT 仍约每 30 秒上报；
6. 再通过 BLE 将采样周期改为其他合法值，重启后确认新采样周期被恢复，同时 MQTT 上报周期仍为 30 秒；
7. 如需测试安全默认值回退，可执行 `idf.py erase-flash` 后重新烧录。该命令会擦除整片 Flash，包括 App 和全部 NVS 数据，执行后必须重新烧录固件；
8. 配置记录版本、长度、魔数或字段范围异常时，服务会删除异常记录、应用 `menuconfig` 默认值并继续启动，不会让设备陷入重启循环。

代码已经提供 `app_config_service_reset_defaults()` 接口，后续可以将它连接到长按 BOOT 键或受保护的 MQTT 恢复命令。目前不额外扩展控制协议，保持本阶段目标集中在可靠持久化和重启恢复。
