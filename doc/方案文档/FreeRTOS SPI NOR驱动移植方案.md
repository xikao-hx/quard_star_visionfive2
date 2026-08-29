# FreeRTOS Cadence QSPI Indirect 读写移植方案

## 1. 文档目的

本文档规划将仓库 U-Boot Cadence QSPI 驱动中的 indirect read/write 数据通道移植到 FreeRTOS SPI NOR 驱动，解决当前通过 STIG 通道每次最多传输 8 Bytes 导致的读写性能问题。

本方案的核心目标是：

- 将 4 KiB 写入从 512 次 8 Bytes Page Program 降为 16 次 256 Bytes Page Program。
- 将大块读取从重复 8 Bytes STIG 命令改为 Cadence indirect read。
- 保留当前 `spi_nor_*()` 对外 API、FreeRTOS mutex 和 NOR agent 协议。
- 不引入 U-Boot driver model、`spi-mem`、MTD、设备树解析和动态内存依赖。
- 物理 QSPI 仍只归 hart4 FreeRTOS 所有，Linux 和 U-Boot proper 继续通过 mailbox NOR agent 访问。

本文档是实施设计，不代表代码已经移植或实板验证。

## 2. 现状与根因

当前 FreeRTOS 驱动位于：

```text
trusted_domain/driver/spi_nor/spi_nor.c
trusted_domain/driver/spi_nor/spi_nor.h
```

`qspi_exec()` 仅使用 Cadence QSPI STIG（Software Triggered Instruction Generator）命令通道。STIG 的命令数据寄存器只能承载 8 Bytes，因此当前写入流程为：

```text
4 KiB
  -> 512 x 8 Bytes
  -> 每次 WREN
  -> 每次 PAGE PROGRAM
  -> 每次轮询 WIP
  -> 首次轮询仍 busy 时可能等待 1 ms tick
```

GD25LQ128 的 page size 为 256 Bytes。NOR 芯片并不要求每 8 Bytes 发起一次 Page Program，8 Bytes 限制来自当前所选的 STIG 控制器通道。

## 3. 移植来源与边界

### 3.1 U-Boot 来源

以当前仓库中与 JH7110 实际构建匹配的源码为移植基线：

```text
u-boot/drivers/spi/cadence_qspi_apb.c
u-boot/drivers/spi/cadence_qspi.c
u-boot/drivers/spi/cadence_qspi.h
```

主要参考函数：

```text
cadence_qspi_apb_controller_init()
cadence_qspi_apb_read_setup()
cadence_qspi_apb_indirect_read_execute()
cadence_qspi_apb_write_setup()
cadence_qspi_apb_indirect_write_execute()
cadence_qspi_wait_idle()
```

移植时以当前仓库版本为唯一基线，不同时混用其他 U-Boot 版本或 Linux 驱动中的寄存器定义。

### 3.2 只移植的能力

- indirect read 的 setup、FIFO 取数、DONE 检查和超时 cancel。
- indirect write 的 setup、FIFO 填充、DONE 检查和超时 cancel。
- 支撑 indirect 通道的控制器 size、SRAM partition、trigger 和 instruction 寄存器配置。
- 32-bit 对齐快速路径和非对齐/尾部数据处理。

### 3.3 明确不移植的能力

- U-Boot driver model 及 `struct udevice`。
- `struct spi_mem_op` 和 SPI NOR core。
- U-Boot MTD 层。
- device tree 动态解析。
- DMA、DAC/direct access 和 XIP。
- DTR、双线/四线 opcode、4-byte address 和 bank switching。
- U-Boot `malloc()` bounce buffer。

本期仍固定为 GD25LQ128、3-byte address、single-SPI SDR、Mode 0、4 KiB erase 和 256 Bytes page program。

### 3.4 许可证要求

U-Boot 源码使用 GPL 许可。实施前必须确认 FreeRTOS 固件的发布方式和源码义务，并在移植文件中保留原始 copyright、SPDX 和来源说明。若产品的许可策略不允许直接复用 GPL 代码，必须停止直接移植，改用许可兼容的实现来源。

## 4. 目标架构

```text
Linux/U-Boot proper NOR client
              |
              v
       mailbox NOR agent
              |
              v
spi_nor_read/write/erase/get_id       对外 API 保持不变
              |
      +-------+--------+
      |                |
      v                v
 STIG command      Indirect data
 RDID/RDSR/WREN    READ/256B PP
 4K ERASE          FIFO via AHB window
      |                |
      +-------+--------+
              v
       Cadence QSPI controller
              |
              v
          GD25LQ128
```

建议将当前单文件内部逻辑划分为两个职责，但首次移植可先保持在同一 `.c` 文件中，避免无关重构：

```text
SPI NOR protocol
  - page boundary
  - WREN/WEL
  - WIP timeout
  - erase/range validation

Cadence controller transport
  - STIG
  - indirect setup
  - FIFO transfer
  - controller completion/cancel
```

## 5. 硬件参数

移植以 JH7110 DTS 和现有 U-Boot 配置为依据：

| 参数 | 值 | 用途 |
| --- | ---: | --- |
| QSPI register base | `0x13010000` | APB 寄存器 |
| QSPI AHB data aperture | `0x21000000` | indirect FIFO 数据窗口 |
| AHB aperture size | `0x400000` | 仅作控制器数据窗口，不是 NOR 容量 |
| FIFO depth | `256` | Cadence FIFO entries |
| FIFO width | `4` | 每 entry 4 Bytes |
| Trigger address | `0` | indirect trigger |
| NOR size | `0x1000000` | 16 MiB |
| Page size | `256` | Page Program 上限 |
| Erase size | `4096` | 4 KiB subsector |
| Address bytes | `3` | 24-bit address |

不得把 AHB aperture size 当作 NOR 可访问容量，NOR 边界仍由 `SPI_NOR_SIZE` 校验。

## 6. 内部接口设计

对外头文件 `spi_nor.h` 不变。在 `spi_nor.c` 内新增或拆分以下静态接口：

```c
static int qspi_indirect_read_setup(uint8_t opcode, uint32_t addr);
static int qspi_indirect_read_execute(void *buffer, uint32_t len);
static int qspi_indirect_read(uint8_t opcode, uint32_t addr,
                              void *buffer, uint32_t len);

static int qspi_indirect_write_setup(uint8_t opcode, uint32_t addr);
static int qspi_indirect_write_execute(const void *buffer, uint32_t len);
static int qspi_indirect_write(uint8_t opcode, uint32_t addr,
                               const void *buffer, uint32_t len);

static int qspi_wait_indirect_done(uint32_t reg, uint32_t done_mask,
                                   uint32_t timeout);
static void qspi_cancel_indirect(uint32_t reg, uint32_t cancel_mask);
```

`qspi_exec()` 继续限制在 8 Bytes，且只用于 STIG 命令。不通过扩大 `CQSPI_STIG_DATA_MAX` 伪装支持大块传输。

## 7. 控制器初始化调整

`spi_nor_hw_init()` 在现有 Mode 0、single-SPI 配置基础上增加 indirect 通道所需的一次性配置：

1. 禁用 controller，保留现有 CS、CPOL、CPHA 和 SDR 设置。
2. 配置 `CQSPI_REG_SIZE`：3-byte address、256-byte page、4 KiB block。
3. 配置 `CQSPI_REG_REMAP = 0`。
4. 配置 `CQSPI_REG_SRAMPARTITION = fifo_depth / 2`。
5. 配置 `CQSPI_REG_INDIRECTTRIGGER = 0`。
6. 禁用本期不使用的 QSPI controller IRQ，indirect 使用轮询完成。
7. 恢复 controller enable，等待连续 idle sample。
8. 使用 STIG RDID 确认 NOR 仍可访问。

当前依赖 U-Boot 已打开 JH7110 QSPI clocks/reset 的前提保持不变。后续若要支持 FreeRTOS 独立启动，再扩展 clock/reset 所有权，不与本次性能移植混合。

## 8. Indirect write 设计

### 8.1 顶层分页

`spi_nor_write_data()` 仍是唯一的 Page Program 分页层：

```c
page_left = SPI_NOR_PAGE_SIZE - (addr % SPI_NOR_PAGE_SIZE);
chunk = min(len, page_left);
```

`chunk` 最大为 256 Bytes，不再受 `CQSPI_STIG_DATA_MAX` 限制。每个 chunk 的顺序必须保持：

```text
WREN through STIG
  -> verify WEL
  -> configure WR_INSTR = PAGE PROGRAM 0x02
  -> set INDIRECTWRSTARTADDR
  -> set INDIRECTWRBYTES
  -> clear stale DONE
  -> START
  -> feed complete page/chunk through AHB FIFO window
  -> wait FIFO drained
  -> wait INDIRECTWR DONE
  -> clear DONE
  -> poll NOR WIP through STIG
```

禁止将跨越 256 Bytes page boundary 的数据交给一次 indirect write。

### 8.2 FIFO 写入

- 32-bit 对齐 buffer 和 4-byte 整数长度使用 word 写快速路径。
- 非 32-bit 对齐 buffer 通过 `memcpy()` 装入局部 `uint32_t` 后写入 FIFO，不使用未对齐指针解引用。
- 尾部 1–3 Bytes 使用明确的 byte 路径，不读取 buffer 边界外数据。
- 每次写 FIFO 前根据 SRAM fill level 或 U-Boot 现有等待条件确认可用空间。
- MMIO 写入和控制寄存器检查之间保留 `fence iorw, iorw`。

不移植 U-Boot 的 `malloc()` bounce buffer。最大一页只有 256 Bytes，若硬件实测表明必须使用对齐 bounce buffer，应使用固定大小且在 mutex 保护下的静态 buffer。

### 8.3 两类完成状态

indirect write 必须区分：

1. **Controller DONE**：表示 FIFO 数据已经由 Cadence controller 送往 NOR。
2. **NOR WIP clear**：表示 NOR 内部 Page Program 实际完成。

两者都成功才能返回。Controller DONE 不能替代现有 `spi_nor_wait_ready()`。

## 9. Indirect read 设计

`spi_nor_read_data()` 不再将数据拆成 8 Bytes STIG read。读取流程为：

```text
wait controller idle
  -> configure RD_INSTR = READ 0x03, single-SPI, no dummy
  -> set INDIRECTRDSTARTADDR
  -> set INDIRECTRDBYTES
  -> clear stale DONE
  -> START
  -> poll SRAM read level
  -> drain FIFO through AHB aperture
  -> wait INDIRECTRD DONE
  -> clear DONE
```

读路径要求：

- 接口继续支持任意起始地址和任意长度。
- 对齐数据使用 word 读，非对齐目标使用局部 word + `memcpy()`。
- 只复制请求长度，不因 FIFO word 宽度越过目标 buffer 边界。
- 每一次等待 FIFO 数据和 DONE 都必须有超时。
- 若 FIFO level 长时间为 0，不得无限循环。

本期可将单次 indirect read 上限设为 NOR agent 共享 buffer 上限（4 KiB），避免设计无限大的同步请求。

## 10. 超时、调度与错误恢复

### 10.1 Controller 超时

Controller FIFO/DONE 属于短时硬件传输，不在每次 FIFO 等待中调用 `vTaskDelay(1)`，否则会再次引入毫秒级放大。首期保留有界轮询，后续接入可靠的微秒时基。

错误返回约定：

| 场景 | 返回值 | 清理 |
| --- | ---: | --- |
| FIFO 长时间无进展 | `-ETIMEDOUT` | 发送 CANCEL |
| indirect DONE 超时 | `-ETIMEDOUT` | 发送 CANCEL |
| controller 未回到 idle | `-ETIMEDOUT` | CANCEL 后重新检查 |
| 参数/边界/跨 page | `-EINVAL` | 不启动传输 |
| WEL 未置位 | `-EIO` | 不启动传输 |
| NOR WIP 超时 | `-ETIMEDOUT` | 保留 controller idle 检查 |

DONE 位按 Cadence 寄存器语义使用 write-one-to-clear，禁止用普通 read-modify-write 方式清除。

### 10.2 NOR WIP 等待

`spi_nor_wait_ready()` 保持任务上下文执行：

```text
立即读取一次 status
  -> 已 ready：立即返回
  -> 仍 busy：vTaskDelay(1 tick)
  -> 再读 status，直到 ready 或超时
```

当 Page Program 降为每 256 Bytes 一次后，4 KiB 写入最多只有 16 个这样的等待点。本次不同时修改 WIP 调度策略，便于单独评估 indirect 改动。

## 11. 并发、上下文与内存顺序

- 继续使用现有 `spi_nor_mutex`串行化 init/read/write/erase/get-id。
- 一次请求从 WREN 到 WIP clear 全程持有 mutex，禁止另一任务插入 STIG 命令。
- indirect 读写只能在 NOR agent 任务或其他任务上下文中调用。
- ISR 不得调用 SPI NOR，不得等待 FIFO、DONE 或 WIP。
- QSPI MMIO 访问使用 `volatile` 并保留 I/O fence。
- AHB data aperture 必须被当作设备窗口访问，不得依赖 CPU cache 自动刷新。

## 12. 文件改动边界

首期预期只修改：

```text
trusted_domain/driver/spi_nor/spi_nor.c
```

若为清晰分层需要拆文件，允许新增：

```text
trusted_domain/driver/spi_nor/cadence_qspi.c
trusted_domain/driver/spi_nor/cadence_qspi.h
```

但对外 `spi_nor.h`、NOR agent、GPT parser、mailbox 协议和 Linux/U-Boot NOR client 均不应因本次移植改变。

## 13. 分阶段实施计划

### Phase 0：固定基线

- [ ] 记录所移植 U-Boot 源文件的 commit ID。
- [ ] 确认 GPL 发布策略，保留 SPDX/copyright。
- [ ] 对照 U-Boot DTS 确认 register base、AHB base、FIFO depth/width 和 trigger address。
- [ ] 记录当前 4 KiB read/write 的耗时和 Page Program 次数，作为对照基线。

### Phase 1：控制器寄存与初始化

- [ ] 引入 indirect read/write、SRAM level、size、trigger 和 IRQ mask 寄存定义。
- [ ] 扩展 `spi_nor_hw_init()` 完成 SRAM partition 和 device size 配置。
- [ ] 保持 STIG RDID/RDSR/WREN/ERASE 正常。
- [ ] 确认 Linux DTS 仍禁用 QSPI，不改变 AMP 外设所有权。

### Phase 2：Indirect read

- [ ] 实现 setup/start/FIFO drain/DONE/clear/cancel。
- [ ] 替换 `spi_nor_read_data()` 中的 8 Bytes STIG 循环。
- [ ] 覆盖 1、3、4、7、8、255、256、257 和 4096 Bytes 读取。
- [ ] 覆盖非对齐地址、非对齐 buffer 和尾部数据。

### Phase 3：Indirect page program

- [ ] 实现 setup/start/FIFO feed/FIFO drain/DONE/clear/cancel。
- [ ] 将 `spi_nor_write_data()` 的 chunk 上限从 8 Bytes 改为当前 page 剩余空间。
- [ ] 保留每 page 的 WREN/WEL 确认和 WIP 超时。
- [ ] 确保单次 indirect write 不跨 page。
- [ ] 确保 4 KiB 写入正好产生 16 次 Page Program。

### Phase 4：错误路径

- [ ] FIFO 无进展超时后 CANCEL。
- [ ] DONE 超时后 CANCEL。
- [ ] 清理 stale DONE，避免下一请求误判完成。
- [ ] 失败后释放 mutex，不留下永久 `-EBUSY`。
- [ ] 失败后下一次 RDID/read 可正常执行。

### Phase 5：完整 AMP 验证

仅在用户明确要求构建/部署/测试时执行：

- [ ] 使用顶层 `make ampfit AMP_RTOS=freertos -j$(nproc)` 构建完整 AMP 产物。
- [ ] 部署精确对应的 SPL、payload、FIT 和 NOR modules，对比 SHA-256。
- [ ] 确认运行的 FreeRTOS 时间戳和 FIT 创建时间。
- [ ] 确认 mailbox router 和 6 个 GPT MTD 分区正常。
- [ ] 破坏性测试只使用 `recovery` 分区首个 4 KiB，先备份、后恢复并逐字节比对。

## 14. 验收标准

### 14.1 功能

- JEDEC ID 与移植前一致。
- GPT 信息读取和 6 个分区注册不变。
- 1–4096 Bytes 任意地址读取正确。
- 页内、跨页和 4 KiB 写入后回读一致。
- 擦除后目标 4 KiB 全为 `0xFF`。
- 连续读写不出现 timeout、`-EBUSY` 或 stale DONE。

### 14.2 性能和命令次数

- 4 KiB 写入必须为 16 次 256 Bytes Page Program，不再是 512 次 8 Bytes。
- 每个 page 恰好一次 WREN 和一次 Page Program。
- 不因 FIFO 等待引入每 word/8 Bytes 的 1 ms tick。
- 4 KiB read/write 耗时相比 Phase 0 基线有明显下降。

### 14.3 异常恢复

- 参数错误不启动硬件传输。
- 模拟/注入 controller timeout 后执行 CANCEL 并返回错误。
- 一次失败不会使后续请求永久卡住。
- NOR agent 错误会通过原协议返回 Linux/U-Boot client。

## 15. 回退方案

在 indirect 实现通过完整验证前，保留 STIG 控制命令路径。如 indirect 在实板上出现不可接受的稳定性问题，回退时只需将 `spi_nor_read_data()` 和 `spi_nor_write_data()` 恢复为 STIG 数据路径，RDID/RDSR/WREN/ERASE 无需变更。

不建议在生产版长期保留运行时可切换的双数据路径，避免两套逻辑同时维护。

## 16. 后续扩展建议

### 16.1 优先级 P0：稳定性与可观测性

1. **传输计数器**
   - 统计 STIG、indirect read、indirect write、Page Program、WIP poll、timeout 和 cancel 次数。
   - 默认不打印，仅通过调试命令或一次性摘要读取，避免性能路径日志干扰。

2. **时基超时**
   - 将循环次数超时改为可校准的微秒时基，避免编译优化和 CPU 频率影响。
   - Controller 等待使用微秒级 timeout，NOR program/erase 使用 tick 级 timeout。

3. **错误分类**
   - 区分 FIFO stall、controller DONE timeout、controller idle timeout、WEL failure 和 NOR WIP timeout。
   - mailbox 协议若后续升级，可传递更精确的诊断码，但不在本次移植中改 ABI。

### 16.2 优先级 P1：性能

1. **批量 indirect read**
   - 允许单次读取覆盖完整 4 KiB 共享 buffer。
   - 根据 FIFO 水位成批 drain，减少寄存器轮询。

2. **WIP 混合等待**
   - Page Program 后可先进行很短的微秒级轮询；仍 busy 再让出一个 tick。
   - 只在实测证明 16 个 tick 仍是主要开销后实施，不应在 indirect 首次移植中同时调优。

3. **快速读/四线读**
   - 先支持 `0x0B` Fast Read，正确配置 dummy cycles。
   - 再评估 dual/quad read、QE bit 和数据采样延迟校准。
   - 写入速度主要受 NOR 内部 program 限制，quad program 不是优先项。

### 16.3 优先级 P2：通用化

1. **拆分 controller 与 NOR protocol**
   - 在 indirect 稳定后将 Cadence controller 移入独立 `cadence_qspi.c/.h`。
   - NOR 层只描述 opcode、address、buffer 和超时，不直接操作 FIFO 寄存器。

2. **SFDP/芯片参数表**
   - 当需要支持更换 NOR 型号或扩容时，再引入 SFDP 或 SFUD。
   - 在只有板载 GD25LQ128 时不引入完整通用 NOR 框架。

3. **4-byte address**
   - NOR 容量超过 16 MiB 后，通过明确芯片参数选择 4-byte opcode 或 enter-4-byte-address mode。
   - 不使用本期不需要的 bank register 逻辑。

### 16.4 优先级 P3：中断与异步化

1. **QSPI IRQ 完成通知**
   - 将 FIFO watermark/DONE 从轮询改为 ISR 仅清状态并唤醒任务。
   - 服务调度、NOR WIP 轮询和错误处理仍在任务上下文。

2. **NOR agent 异步队列**
   - 当 mailbox 需要并行处理其他服务时，将长擦除/写入放入专用 NOR worker。
   - 保持单个 QSPI owner 任务，避免用多个任务并发操作 controller。

3. **请求取消与进度**
   - 只在大镜像写入需要可观测进度时扩展 mailbox ABI。
   - 取消必须保证当前 page/erase 操作收敛后再返回，不在 NOR WIP 期间强制释放 controller。

## 17. 建议实施顺序

综合风险和收益，建议顺序为：

```text
U-Boot indirect read
  -> FreeRTOS indirect read
  -> read-only 实板验证
  -> U-Boot indirect write
  -> FreeRTOS 256B page program
  -> recovery 首 4KiB 可恢复写测试
  -> 连续/异常测试
  -> 完整 AMP 固件更新验证
```

先读后写可以先验证 register setup、AHB FIFO 和 DONE 语义，不引入 NOR 内容破坏风险。写路径验证必须遵守 recovery 首 4 KiB 备份与恢复要求。

## 18. 仓库内参考文件

| 文件 | 参考内容 |
| --- | --- |
| `trusted_domain/driver/spi_nor/spi_nor.c` | FreeRTOS 现有 STIG、WREN、WIP、erase 和对外 API |
| `trusted_domain/driver/nor_agent/quard_nor_agent.c` | NOR 请求上下文、共享 buffer 和破坏性边界 |
| `u-boot/drivers/spi/cadence_qspi_apb.c` | indirect setup/execute、FIFO、DONE、timeout 和 cancel |
| `u-boot/drivers/spi/cadence_qspi.c` | `spi_mem_op` 到 Cadence APB 操作的调用边界，只用于理解调用顺序 |
| `u-boot/drivers/spi/cadence_qspi.h` | controller 平台参数和函数声明 |
| `u-boot/drivers/mtd/spi/spi-nor-core.c` | WREN -> page program -> wait-ready 的上层语义，不直接移植 |
| `u-boot/arch/riscv/dts/jh7110.dtsi` | JH7110 QSPI register/AHB base、FIFO depth/width 和 NOR 参数 |
| `linux/arch/riscv/boot/dts/starfive/jh7110-starfive-visionfive-2-amp.dts` | AMP 中 QSPI 归 FreeRTOS 所有的边界 |
| `common_inc/bsp/quard_nor_layout.h` | NOR 容量、page/erase size 和 recovery 范围的生成常量 |
