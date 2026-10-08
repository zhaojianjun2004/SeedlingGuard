# 霸天虎 V2：STM32CubeMX 建工程与配置清单

> 目标板：野火 F407 霸天虎 V2，STM32F407ZGT6。  
> 已生成位置：本仓库 `fireware/stm32f407/`；早期规划写作 `firmware/`，目前先按实际路径继续，不在 CubeMX 打开期间移动工程。  
> 2026-10-08 修订：按 CubeMX 实际字段填写 SPI、GPIO、DMA、中断与任务参数。  
> 本清单依据板卡硬件规格书、2025 命名版原理图和书籍配套例程交叉核对；动手接线前仍以手中板卡丝印为准。

**已选实机路线：**F407↔ESP32-S3 先用 USART3/UART1 的 3.3 V TTL 串口；F407 板载 CAN2↔现有 USB-CAN/电脑单独验证。ESP32-S3 有 TWAI 控制器但目前没有 CAN 收发器，不能把 CANH/CANL 直接连到 ESP32 GPIO。

**当前完成口径：**下文 `[√]` 表示已在 `.ioc`/生成代码中核对或已完成对应源码修正；`[ ]` 表示还需应用代码、实物接线或联调。2026-10-08 已编译通过生成骨架，尚未烧录验证。CAN1 时钟已由 CubeMX 在 `can.c` 自动启用；多余 `defaultTask` 和任务拼写已在 `.ioc`、`freertos.c` 同步修正。

## 0. 先明确工程怎么管理

这是一套 **双 MCU 系统**：F407 是一颗 Cortex-M4 单核 MCU，ESP32-S3 是另一颗 MCU；“两颗 MCU”与“单颗双核 MCU”是不同概念。两边各有独立固件、构建工具和版本号，用一份 Git 仓库与一版板间 UART 协议协调；F407 的 CAN2 单独与电脑联调，不把 F407 源码塞进 ESP-IDF 的 `components`。

```text
0_helloworld/                      一个 Git 仓库
├── main/ + components/             现有 ESP32-S3 / ESP-IDF 工程
├── fireware/stm32f407/             当前 CubeMX 生成的 F407 工程 + 后续自写 App/
├── shared/protocol/                两端共用的纯 C CAN 编解码与主机测试
└── docs/独立项目_物联网边缘网关/       硬件、协议、联调和发布记录
```

F407 与 ESP32 分别编译、烧录和调试；仓库根目录的 `idf.py build` 只负责 ESP32，F407 子目录的 CMake 构建只负责 F407。每次联调记录 `F407 固件版本 + ESP32 固件版本 + UART 帧协议版本 + 板卡版本`，CAN 台架另记波特率和诊断帧版本。第一版 F407 通过 SWD 烧录，已有 ESP32 A/B OTA 保留；不要将 ESP32 升级成功理解为 F407 也已升级。

两端启动后先交换协议版本与 F407 启动会话号；版本不兼容时保留各自基本功能，并明确报错。F407 持有采集启停与周期的实际状态，ESP32 只保存供 HTTP/MQTT 使用的镜像。建议联调记录采用一个固定模板，逐项记下接线、两个固件的版本、仪表地址与寄存器映射、CAN 波特率、日志以及故障注入结果，避免以后只凭口述判断哪一版组合可用。

## 1. 板卡事实与资料差异

| 功能 | 霸天虎 V2 实际连接 | 需要注意 |
| --- | --- | --- |
| MCU 时钟 | 25 MHz HSE 晶振；32.768 kHz LSE 晶振 | 第一版只需 HSE，RTC/LSE 可暂不启用 |
| 板载 CAN | **CAN2 RX `PB12`、TX `PB13`**，板载收发器和 120 Ω 终端 | CAN/485 电源由 J40 的 1–2 控制；ESP32 侧仍需外接收发器与终端 |
| 板载 RS485 | USART2 TX `PA2`、RX `PA3`，DE 与 /RE 共接 `PC0` | J40 选择 485 而非 232：按原理图 3–5、4–6 接通；`PA2` 同时连板载以太网 MDIO，不启用 ETH |
| 板间 UART 候选 | USART3 TX `PD8`、RX `PD9` | 两脚还通向未使用的 LCD/FSMC 连接器；启用串口前确认可从排针接出，且未安装 LCD |
| 板载 SPI Flash | SPI1 `PB3/PB4/PB5`，CS `PG6` | `PB3/PB4` 与 JTAG 复用，所以 SYS 选 Serial Wire；先读 JEDEC ID，再确认 Flash 容量 |
| 调试串口 | USART1 TX `PA9`、RX `PA10` 通过 J80/J81 接 CH340 | 选用时核对跳帽、端口与波特率；不占用 USART2 |
| 用户按键 | 配套例程使用 `PA0`、`PC13` | 可用其中一颗作本地采集控制，需核对按下电平及去抖 |
| 调试接口 | SWD `PA13/PA14` | 保留 SWD，不能把这两脚另作普通 GPIO |

硬件规格书称 Flash 为 **W25Q64 / 8 MB**、CAN 为 TJA1042T/3、RS485 为 MAX485；较新的原理图显示 **W25Q128JV / 16 MB**、SIT1042AQT/3、SIT3088EESA。上表的**连接关系**有原理图与配套例程相互印证，器件具体型号和容量应按实际板卡丝印及运行时 ID 再确认。

这也修正了早期通用 F407 草案：本板不用 CAN1 `PB8/PB9`，因为板载 CAN 收发器接的是 CAN2 `PB12/PB13`；这些脚占用 SPI2，所以板载 Flash 使用 SPI1。

## 2. CubeMX 新建与代码生成

- [√] 新建 MCU 工程，选择 **STM32F407ZGTx / STM32F407ZGT6**，不要选择 F411 或通用 F407VG。
- [√] Project Name 为 `stm32f407`；实际生成目录是 `fireware/stm32f407/`。工程已生成后先保持原路径，后续如统一改为 `firmware/`，应关闭 CubeMX、移动工程并重新配置构建。
- [ ] 使用已安装的 STM32CubeF4 固件包，优先选择 **CMake + GCC** 作为 Toolchain/IDE。当前环境已有 CubeMX 6.14.1、Arm GNU、CMake 和 Ninja；这样 VS Code 可以编辑两边代码，构建仍各自独立。
- [ ] Code Generator 勾选 **Generate peripheral initialization as a pair of .c/.h files**、**Keep User Code when re-generating**，建议同时开启重新生成前备份。
- [ ] 自动生成文件只在 `USER CODE BEGIN/END` 区域放极少量装配代码。协议、Modbus、领域状态和任务逻辑放自建 `App/`，不要把主要实现写进 `main.c` 或 `freertos.c` 的生成区。
- [ ] 第一次生成后保存 `.ioc`、生成的 CMake 文件、启动文件、链接脚本和所需 HAL/FreeRTOS 源码；以后每次重新生成先检查 Git 差异，再编译，不直接覆盖人工维护的应用文件。

STM32CubeMX 的“Keep User Code”仅保护生成文件内的标记区，不自动保护所有手写内容；这是把业务代码放在 `App/` 的原因。见 [ST 的 CubeMX 手册](https://www.st.com/resource/en/user_manual/um1718-stm32cubemx-for-stm32-configuration-and-initialization-c-code-generation-stmicroelectronics.pdf)。

## 3. Pinout & Clock Configuration

### 3.1 System Core 与时钟

- [√] `SYS → Debug` 选 **Serial Wire**，保留 `PA13/PA14`；不要选全 JTAG，因为 SPI1 用 `PB3/PB4`。
- [√] `RCC → HSE` 选 **Crystal/Ceramic Resonator**，HSE 输入频率设 **25 MHz**；第一版 `LSE` 可不启用。
- [√] 已生成：SYSCLK/HCLK **168 MHz**、APB1 **42 MHz**（分频 4）、APB2 **84 MHz**（分频 2），`PLLM=25、PLLN=336、PLLP=2`。当前 `PLLQ=4`，所以 48 MHz 域为 **84 MHz**；本版不用 USB/SDIO，不阻塞 CAN/UART/SPI。若将来启用需要 48 MHz 的外设，再设 `PLLQ=7` 并复核整个时钟树。
- [√] `SYS → Timebase Source` 选 **TIM6**，把 SysTick 留给 FreeRTOS。不要在任务中依赖 `HAL_Delay()` 作长期阻塞；任务延时用 RTOS API。ST 的 [FreeRTOS 配置说明](https://dev.st.com/stm32cube-docs/mw-freertos/2.0.0/en/docs/markup/how_to_use.html)也采用独立 HAL timebase。
- [√] USART1 已生成 `PA9 TX / PA10 RX`、115200、8N1；板载 CH340 的 J80/J81 跳帽接法和实际日志输出留待实物验收。

### 3.2 CAN2：先与现有 USB-CAN/电脑联调

- [√] 在 **Pinout & Configuration → Connectivity → CAN2** 将模式选为 **Activated / Normal**（具体下拉名称随 CubeMX 版本略有差异），确认图上是 `PB12 = CAN2_RX`、`PB13 = CAN2_TX`。不要点击 CAN1 的 `PB8/PB9`。
- [√] CAN2 **Parameter Settings** 已生成；CAN2 在 APB1 上，PCLK1 为 42 MHz。下表按本次生成值记录。

  | CubeMX 字段 | 填写值 | 意义 |
  | --- | --- | --- |
  | Prescaler | `21` | 把 42 MHz 分到 CAN 时间量子 |
  | Time Quanta in Bit Segment 1 / BS1 | `12 TQ` | 传播与相位段 1 |
  | Time Quanta in Bit Segment 2 / BS2 | `3 TQ` | 相位段 2 |
  | Resynchronization Jump Width / SJW | `1 TQ` | 初始重同步幅度 |
  | Time Triggered Mode | Disabled | 不用时间触发 CAN |
  | Auto Bus-Off | Disabled | 应用层记录 Bus-Off 并控制恢复时机 |
  | Auto Wake-Up | Disabled | 不靠自动唤醒改变链路状态 |
  | Auto Retransmission | Disabled | 本版由应用层记录失败并做有上限重试；避免对端离线时硬件无限重传 |
  | Receive FIFO Locked | Disabled | 新帧可以覆盖 FIFO 满时的旧帧，同时记录溢出 |
  | Transmit FIFO Priority | Disabled | 按报文 ID 决定发送优先级 |

  这样 `42 MHz ÷ [21 × (1+12+3)] = 125 kbps`，采样点为 81.25%。现阶段先让 USB-CAN 设为同一速率并核对错误计数。ESP32-S3 有 TWAI 控制器，但当前没有 ESP32 侧 CAN 收发器；购入兼容 3.3 V 逻辑的收发器之前，不能将 F407 的 CANH/CANL 直接接到 ESP32 GPIO。
- [√] CAN2 的 `RX0 interrupt` 和 `SCE interrupt` 已生成，抢占优先级分别为 **5**、**6**，子优先级均为 **0**；`CAN2 TX`、`CAN2 RX1` 未启用。
- [ ] 应用层仍需由 `linkTask` 检查发送邮箱/超时，并处理 SCE 报告的错误和 Bus-Off；仅勾选 NVIC 不会自动完成这些业务逻辑。
- [√] **CAN2 所需的 CAN1 外设时钟已由生成代码启用**：`Core/Src/can.c` 的 `HAL_CAN_MspInit()` 同时调用 `__HAL_RCC_CAN2_CLK_ENABLE()` 和 `__HAL_RCC_CAN1_CLK_ENABLE()`，无需重复插入。未占用 CAN1 引脚。
- [ ] 应用层接收滤波尚未配置：先用 CAN2 bank 14 接收全部帧打通 USB-CAN 链路，再收窄为协议 ID；代码中设置 `SlaveStartFilterBank=14`、`FilterBank>=14`。参见 ST 的 [双 CAN 滤波器说明](https://community.st.com/stm32-mcus-60/stm32-in-dual-can-configuration-bxcan-filter-bank-explanation-and-relation-with-can2-start-bank-parameter-136737)。
- [ ] CubeMX 只生成 `MX_CAN2_Init()` 与 IRQ 入口，**不会替应用完成收发启动**。接入代码时依次执行 `HAL_CAN_ConfigFilter()`、`HAL_CAN_Start()`、`HAL_CAN_ActivateNotification(CAN_IT_RX_FIFO0_MSG_PENDING | CAN_IT_ERROR | CAN_IT_BUSOFF)`，失败要写入诊断状态。CAN RX 回调只拷贝帧到队列，不在中断里解释业务命令。
- [ ] 上电前确认 J40 1–2 给 CAN/485 供电，F407 与 USB-CAN 两端波特率相同，CANH/CANL 未反接；板端已有 120 Ω，USB-CAN 端按模块说明启用或外接另一只 120 Ω，断电测得总线等效阻值约 60 Ω 再进入正常模式联调。

### 3.3 USART2 与板载 RS485

- [√] 在 **Connectivity → USART2 → Mode** 选 `Asynchronous`，确认 `PA2 = USART2_TX`、`PA3 = USART2_RX`。在 **Parameter Settings** 填 `Baud Rate = 9600`、`Word Length = 8 Bits`、`Parity = None`、`Stop Bits = 1`、`Hardware Flow Control = None`、`Mode = TX and RX`、`Oversampling = 16`。这是首轮默认值，若仪表手册另有要求，两端按实际仪表一起调整。
- [√] PC0 已生成 `GPIO_Output`、初始 Low、Output Push Pull、无上下拉、低速，标签 `RS485_DE_RE`。板载收发器的 DE 与 /RE 同接 PC0，**低=接收，高=发送**。
- [ ] Modbus 发送代码仍需发送前拉高 PC0，确认最后一个停止位真正发完后再拉低；当前只有 GPIO 初始配置。
- [ ] J40 选择 485 路径：按原理图将 3–5、4–6 接通；不要同时连到 RS232 路径。断电先核对跳帽编号和丝印。
- [√] 首版确定为**阻塞 TX + DMA 环形 RX**。在 USART2 的 **DMA Settings → Add → USART2_RX** 选择 `DMA1 Stream5 / Channel 4`，`Direction = Peripheral to Memory`、`Mode = Circular`、`Peripheral Increment = Disable`、`Memory Increment = Enable`、两侧数据宽度均 `Byte`、`Priority = High`、`FIFO Mode = Disable`。不添加 USART2_TX DMA：8 字节 Modbus 请求可由独占任务使用有界的 `HAL_UART_Transmit()` 发送。
- [√] USART2 global interrupt 与 DMA1 Stream5 global interrupt 已生成，均为抢占/子优先级 **6/0**。
- [ ] 应用层仍需启动 `HAL_UARTEx_ReceiveToIdle_DMA()`，并用 DMA 写入位置和 IDLE 事件通知任务。**IDLE 不是 Modbus RTU 3.5 字符间隔的完整判据**；仍按预期长度、绝对超时、帧间隔与 CRC 判定响应。DMA 缓冲区放普通 SRAM，不放入 F407 CCM。
- [ ] DMA RX 缓冲区先用 **256 字节**并由 Modbus 任务独占；`HAL_UARTEx_RxEventCallback()` 中的 `Size` 按环形写入位置处理，不能当成“本次新增字节数”直接重复拷贝。STM32CubeF4 V1.28.0 已提供 `HAL_UARTEx_ReceiveToIdle_DMA()`；回调可能由 IDLE、半传输或传输完成触发，所以必须保存上次处理位置。
- [√] 未启用以太网 ETH：`PA2` 在板上也接 RMII MDIO，启用 ETH 会与 RS485 TX 冲突。
- [ ] 板上 RS485 端已有 120 Ω；多从机总线只在物理两端保留终端，另一端按仪表/外部电阻配置，不能每个从机都再加 120 Ω。

### 3.4 USART3 与 ESP32-S3 的板间 UART

- [√] USART3 已生成 `Asynchronous`、`PD8 → USART3_TX`、`PD9 → USART3_RX`，`PB10/PB11` 未占 USART3。`PD8/PD9` 同时接未使用的 LCD 接口，所以本项目不安装 LCD。
- [√] USART3 参数已生成：`Baud Rate = 115200`、`Word Length = 8 Bits`、`Parity = None`、`Stop Bits = 1`、`Hardware Flow Control = None`、`Mode = TX and RX`、`Oversampling = 16`。
- [√] USART3_RX DMA 已生成：`DMA1 Stream1 / Channel 4`，外设到内存、Circular、仅内存地址递增、Byte 对齐、High、FIFO 禁用；未配置 TX DMA。
- [√] USART3 global interrupt 与 DMA1 Stream1 global interrupt 已启用，均为 **6/0**。
- [ ] 应用层仍需启动 `HAL_UARTEx_ReceiveToIdle_DMA()`、管理 256 字节 RX 环形缓冲；`linkTask` 从 ISR 通知中取得新增区间，校验帧头、长度、序号和 CRC16，再提交领域消息。
- [ ] 接线：`F407 PD8/TX → ESP32 GPIO18/RX`、`F407 PD9/RX ← ESP32 GPIO17/TX` 已接；**F407 GND ↔ ESP32 GND 尚需补上**。两边都是 3.3 V TTL，可分别由 USB 供电，不接 RS232 电平，也不把两板 3V3 电源脚相连。先断电补 GND，再核对 ESP32 GPIO17/GPIO18 和 F407 PD8/PD9 的实际排针。

### 3.5 板载 SPI Flash、按键与 IWDG

- [√] SPI1 已生成 `Full-Duplex Master`，引脚为 `PB3 → SPI1_SCK`、`PB4 → SPI1_MISO`、`PB5 → SPI1_MOSI`；`PA5/PA6/PA7` 未占用 SPI1。`SYS → Debug` 是 Serial Wire，释放了与 JTAG 复用的 PB3/PB4。
- [√] SPI1 参数已生成，采用霸天虎配套 W25Q128 例程的 Mode 3：`CPOL=High + CPHA=2 Edge`；上一版的 Mode 0 对应 `CPOL=Low + CPHA=1 Edge`，两者都不能交叉组合。

  | CubeMX 字段 | 首轮填写值 | 说明 |
  | --- | --- | --- |
  | Frame Format | Motorola | 普通 SPI，不选 TI |
  | Data Size | 8 Bits | Flash 命令与地址逐字节发送 |
  | First Bit | MSB First | 高位先发 |
  | Clock Polarity (CPOL) | High | 空闲时 SCK 为高 |
  | Clock Phase (CPHA) | 2 Edge | 在第二个时钟边沿采样，即 Mode 3 |
  | NSS Signal Type | Software | 片选不用硬件 NSS 引脚，由 PG6 软件控制 |
  | Baud Rate Prescaler | 32 | SPI1 时钟来自 APB2=84 MHz，初始 SCK 约 **2.625 MHz** |
  | CRC Calculation | Disabled | Flash 命令不使用 STM32 SPI 硬件 CRC |
  | TI Mode / NSS Pulse（若界面有） | Disabled | 保持普通 Motorola SPI |

  首轮不加 SPI1 DMA、不启用 SPI1 global interrupt；用任务上下文里的有界 HAL SPI 收发先读 JEDEC ID。可核对本地 `24-SPI—读写串行FLASH（W25Q128）` 例程中 `SPI_POLARITY_HIGH`、`SPI_PHASE_2EDGE` 的组合，以及 [Winbond 官方 W25Q128JV 数据手册入口](https://winbond.com/hq/support/documentation/?__locale=en&line=/product/code-storage-flash/index.html&family=/product/code-storage-flash/qspi-nor/index.html&category=/.categories/resources/datasheet/&pno=W25Q128JV)。
- [√] PG6 已生成 `GPIO_Output`，初始 High、Output Push Pull、无上下拉、低速，标签 `Flash_CS`；Flash 片选低有效，上电保持高电平。
- [ ] 尚需通过 SPI1 读 JEDEC ID，区分实物 W25Q64/W25Q128，再决定容量和存储分区。
- [√] 首轮未配置 PA0/PC13，也未创建按键中断。`Reset_State` 表示当前未分配功能，不等于硬件不可用；按键留到主链路稳定后再选引脚、核对极性并去抖。
- [√] 当前工程未启用 IWDG，便于先检查任务是否正常推进。
- [ ] 监督器完成后，再到 **System Core → IWDG → Activated**，可从 `Prescaler = 256`、`Reload = 1249`（按 LSI 标称 32 kHz 约 10 s）开始，实测后调整；只由 supervisor 根据关键任务进展统一刷新。IWDG 不需要 NVIC 中断。
- [√] 未启用 FSMC/LCD、触摸屏、板载 ESP8266 或 LVGL；它们不属于这版网关。

### 3.6 NVIC：这一轮逐项核对

在 **System Core → NVIC** 将优先级分组设为 `NVIC_PRIORITYGROUP_4`（4 位抢占优先级、0 位子优先级）。下表的数字是 **NVIC 中断抢占优先级，数值越小越紧急**；它与下一节的 FreeRTOS 任务优先级是两套刻度。若 CubeMX 画面里某条中断由外设自动勾选，也仍要核对优先级。

| NVIC 条目 | 本轮设置 | 抢占/子优先级 | 用途 |
| --- | --- | --- | --- |
| `CAN2 RX0 interrupt` | Enable | `5 / 0` | 把接收帧投到 CAN 队列 |
| `CAN2 SCE interrupt` | Enable | `6 / 0` | 错误、Bus-Off 状态通知 |
| `USART2 global interrupt` | Enable | `6 / 0` | IDLE 事件和 UART 错误 |
| `DMA1 Stream5 global interrupt` | Enable | `6 / 0` | USART2_RX DMA 半满/满/错误通知 |
| `USART3 global interrupt` | Enable | `6 / 0` | 两板 UART 的 IDLE 事件和错误 |
| `DMA1 Stream1 global interrupt` | Enable | `6 / 0` | USART3_RX DMA 半满/满/错误通知 |
| `TIM6_DAC interrupt` | Enable，由 HAL timebase 生成 | `15 / 0`，保持你当前 CubeMX 的生成默认值 | 更新 HAL 毫秒时基；中断回调中不调用 RTOS API，也不在其他高优先级 ISR 中等待 HAL 时基 |
| `SysTick`、`PendSV`、`SVC` | FreeRTOS/CubeMX 管理 | 保持生成值；通常 SysTick/PendSV 为最低 `15` | 不手工改写 RTOS 核心异常 |
| `CAN2 TX`、`CAN2 RX1`、`SPI1`、`USART1`、`GPIO EXTI` | Disable | 不适用 | 本版分别用邮箱检查、FIFO0、轮询 SPI、仅输出调试日志、轮询按键 |

在 **FREERTOS → Config parameters** 若有 `LIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` 字段，设为 **5**；`LIBRARY_LOWEST_INTERRUPT_PRIORITY` 设为 **15**。生成后必须在 `FreeRTOSConfig.h` 核对这两个宏及 `configMAX_SYSCALL_INTERRUPT_PRIORITY` 的左移结果；若当前 GUI 不显示该项，先查看生成值，再调整 NVIC 表，不能假定默认一定是 5。调用 `xQueueSendFromISR()` 等 FreeRTOS API 的 ISR 必须使用数值 **≥5** 的抢占优先级。TIM6 保持当前生成的 **15/0**；它是 HAL 时基，低优先级意味着不能在其他中断里调用等待 HAL tick 推进的阻塞函数。还应检查 CAN2 依赖的 CAN1 外设时钟是否在 MSP 初始化中启用，CAN2 的滤波 bank 14 则由应用代码配置。

## 4. FreeRTOS 与生成后的应用装配

- [√] FreeRTOS 已生成 CMSIS_V2、heap_4、32 KiB heap、1 kHz tick、Dynamic 任务和互斥锁支持。`configMAX_PRIORITIES = 56` 与 CMSIS_V2 映射配套；`.ioc` 的 `MAX_CO_ROUTINE_PRIORITIES = 2` 不影响任务，因为 `configUSE_CO_ROUTINES = 0`。
- [√] `.ioc` 与 `freertos.c` 已同步为下表四个任务；多余 `defaultTask` 已删除，`StartFieldTask` 拼写已修正。任务的优先级名称属于 CMSIS_V2，不是 NVIC 数字。

  任务调度次序为 `AboveNormal > Normal > BelowNormal`；同为 Normal 的 `fieldTask` 与 `modbusTask` 靠阻塞等待和时间片公平运行。ISR 的数字优先级方向正好相反，不要互相套用。

  | Task Name / Entry Function | CubeMX Priority | Stack Size (Words) | 运行职责 |
  | --- | --- | ---: | --- |
  | `linkTask` / `StartLinkTask` | `osPriorityAboveNormal` | `512` | 处理 USART3 板间帧和 CAN2 诊断帧，单次只处理有限批量 |
  | `fieldTask` / `StartFieldTask` | `osPriorityNormal` | `512` | 采集启停和周期的唯一写者，处理来自 ESP32 的命令 |
  | `modbusTask` / `StartModbusTask` | `osPriorityNormal` | `768` | 独占 USART2，轮询两台仪表并提交带质量的样本 |
  | `supervisorTask` / `StartSupervisorTask` | `osPriorityBelowNormal` | `256` | 每 1 s 检查关键任务进展，后期统一喂 IWDG |

  **栈单位要看 CubeMX 当前列名。** 若 UI 显示 `Stack Size (Words)`，填表中 512/768/256，每 word 为 4 字节；若它显示 Bytes，分别填 2048/3072/1024。生成后检查 `freertos.c` 中的 `.stack_size` 最终约为这些字节数。上述是保守起点，用 `uxTaskGetStackHighWaterMark()` 再调整。任务的 Entry Function 先保留 CubeMX 生成的 `osDelay()` 空循环也能编译；接入业务时只在其 `USER CODE` 区域调用 `App/` 的实现。
- [ ] 队列和互斥量首轮**不在 CubeMX Tasks and Queues 页面创建**，由 `App_Init()` 统一创建并检查返回值；计划 UART RX 事件队列 8 条、CAN RX 队列 16 帧、控制命令队列 8 条。协议编解码、CRC、点位转换是普通模块，不各自创建任务。
- [ ] CAN RX 中断、UART RX/IDLE 回调只做有限拷贝和投队列；Modbus 解析、状态更新和 Flash 操作在任务上下文执行。
- [ ] 首次生成时可先保持 stack overflow / malloc failed hook 默认关闭，保证空工程可编译；接入 `App/` 时提供 `vApplicationStackOverflowHook()`、`vApplicationMallocFailedHook()` 后，再设 `CHECK_FOR_STACK_OVERFLOW = 2`、`USE_MALLOC_FAILED_HOOK = 1`。同时记录任务进展计数、栈余量、CAN 错误计数与 Modbus 超时/CRC 计数。
- [ ] CubeMX 生成后，将自写模块加入 CMake 构建并确认重新生成仍会保留该引用；每次重新生成后检查 `CMakeLists.txt` 和链接 map。应用对外只暴露 `App_Init()`/`App_Start()` 等装配入口，生成区尽量不承载业务逻辑。

### 4.1 生成文件核对表

生成代码后无需猜测 CubeMX 是否真的应用了设置，直接检查这些字段：

| 文件/函数 | 应看到的关键内容 |
| --- | --- |
| `gpio.c` / `MX_GPIO_Init()` | `PC0` 输出初始 RESET、`PG6` 初始 SET；两脚 `GPIO_MODE_OUTPUT_PP`、无上下拉 |
| `main.c` / 初始化顺序 | 先初始化 GPIO、DMA 和外设，再由应用启动 UART RX DMA、CAN 滤波/通知；不在外设未初始化时创建收发事务 |
| `spi.c` / `MX_SPI1_Init()` | `SPI_MODE_MASTER`、`SPI_DIRECTION_2LINES`、`SPI_DATASIZE_8BIT`、`SPI_POLARITY_HIGH`、`SPI_PHASE_2EDGE`、`SPI_NSS_SOFT`、`SPI_BAUDRATEPRESCALER_32` |
| `usart.c` / `MX_USART2_UART_Init()` | 9600、8 位、无校验、1 停止位、TX/RX；MSP 中 DMA1 Stream5 Channel4 Circular |
| `usart.c` / `MX_USART3_UART_Init()` | 115200、8 位、无校验、1 停止位、TX/RX；MSP 中 PD8/PD9 AF7、DMA1 Stream1 Channel4 Circular |
| `can.c` / `MX_CAN2_Init()` | CAN2、Prescaler 21、BS1 12TQ、BS2 3TQ、SJW 1TQ；MSP 中 CAN1/CAN2 时钟与 PB12/PB13 AF9 |
| `freertos.c`、`FreeRTOSConfig.h` | 四任务及栈大小、FreeRTOS tick、最大中断优先级；TIM6 时基与 SysTick 没有重复占用 |

**GPIO 初始电平应先于对应 GPIO 模式初始化写入**，这样 PC0 不会在配置成输出瞬间误进入发送，PG6 不会误选中 Flash。若生成代码与表不符，先回 `.ioc` 改设置并重新生成，再考虑手工补丁。

## 5. 生成后按这个顺序验收

1. **编译与下载**：CMake/GCC 构建产生 ELF/BIN，SWD 下载后可读启动日志；核对芯片 ID、HSE 25 MHz、系统时钟和异常复位原因。
2. **CAN 单功能**：先 CAN2 内部回环，再连接现有 USB-CAN 与电脑；确认 125 kbps、滤波器 bank 14、接收中断、错误计数和 Bus-Off 恢复。板载收发器供电跳帽必须盖好。
3. **RS485 单功能**：先与 USB-RS485 或模拟从机收发固定帧，再轮询实际 Modbus 仪表；分别注入超时、CRC 错、异常码和单设备掉线。
4. **存储**：Flash JEDEC ID 与读写边界单独验收；本地按键留到确认引脚之后。
5. **双板闭环**：F407 USART3 ↔ ESP32 UART1 连通后，再做测点 → ESP32 网页/MQTT 与反向命令/执行回执；拔线测试针对 UART。CAN Bus-Off 在 F407↔USB-CAN 台架单独测试。两边固件和协议版本一同记录。
6. **监督与升级回归**：最后启用 IWDG，故障注入验证任务停滞能导致受控复位；ESP32 既有 OTA 确认/回滚流程仍正常。

## 6. 可直接参考的本地资料

- `F:\tools\learn\stm32\文档\stm32f407_霸天虎相关资料\F407_霸天虎V2\原理图\野火STM32F407_霸天虎V2开发板原理图2025.pdf`：第 7 页 CAN/RS485/J40，第 8 页 SPI Flash，第 4 页 HSE/LSE，第 10 页按键。
- `F:\tools\learn\stm32\文档\stm32f407_霸天虎相关资料\F407_霸天虎V2\[野火]STM32F407_霸天虎V2开发板硬件规格书.pdf`：第 12 页资源，第 21 页跳帽说明。
- `F:\tools\learn\stm32\文档\stm32f407_霸天虎相关资料\F407_霸天虎V2\野火F407霸天虎V2开发板IO分配表.xlsx`：芯片可选复用功能；它不是底板实际连线的唯一依据。
- `D:\stm32\1-书籍配套例程-F407ZG_20240311_1\40-CAN通信实验\CAN—双机通讯\User\can\bsp_can.h`：板载 CAN2 `PB12/PB13` 参考。
- `D:\stm32\1-书籍配套例程-F407ZG_20240311_1\41-RS485通信实验\User\485\bsp_485.h`：USART2 `PA2/PA3`、方向脚 `PC0` 参考。
- `D:\stm32\1-书籍配套例程-F407ZG_20240311_1\24-SPI—读写串行FLASH（W25Q128）\User\flash\bsp_spi_flash.h`：板载 SPI1 `PB3/PB4/PB5`、CS `PG6` 参考。

配套例程用来核对板级连线和最小收发流程。项目实现采用 CubeMX 生成的 HAL/FreeRTOS 框架及自有协议和业务模块，不逐文件复制示例工程。
