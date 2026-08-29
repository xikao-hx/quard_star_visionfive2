# VisionFive 2 OTA A/B 分区实施方案

## 1. 文档目的

本文档用于固定 VisionFive 2 实机的 SPI NOR 分区布局，并规划从分区镜像生成、U-Boot TFTP 烧写、板端读取验证，到最终 A/B 启动选择的分步实施过程。

本方案的核心原则是：

> 从第一步开始就使用最终分区地址。GPT、recovery、SPL 和 fw_payload 的生成、打包、烧写和读取都按最终地址完成。后续实现 A/B 启动时，只增加 bank 选择逻辑，不再调整任何分区地址。

## 2. 已确认的范围

- 目标硬件：StarFive VisionFive 2 实机。
- 启动 Flash：16 MiB SPI NOR。
- NOR 型号与几何参数：GD25LQ128，page size 256 Bytes，erase size 4 KiB。
- NOR 包含：`spl_a`、`spl_b`、`gpt`、`recovery`、`fw_payload_a`、`fw_payload_b`。
- SPL 分区大小：256 KiB，构建时强制 SPL 镜像不得超过 `0x40000` 字节。
- GPT 方案：采用重定位 GPT，不占用 NOR `0x0` 的 BootROM SPL 入口。
- SPL 启动策略：BootROM 始终从 `spl_a@0x0` 启动，`spl_b` 作为备份和恢复源，不作为 BootROM 自动回退入口。
- 固件启动策略：SPL 读取 recovery 的 `target_bank`，选择 `fw_payload_a` 或 `fw_payload_b`，并将实际选择写入 `current_bank`。
- 烧写方式：在 U-Boot 中通过 TFTP 下载镜像，使用 `sf probe/read/update` 操作 SPI NOR。
- Linux 阶段：本轮不实现 Linux kernel/rootfs A/B 升级，只使用当前单份 Linux FIT 验证完整启动链。
- 开发方式：每一步独立生成、烧写、回读和启动验证；当前步骤没有通过前，不进入下一步。

## 3. 地址概念和边界

### 3.1 QSPI 存储偏移

QSPI 存储偏移表示镜像在 16 MiB SPI NOR 中的位置，是本方案需要固定的地址，例如：

- `spl_a` 的存储偏移为 `0x000000`。
- `fw_payload_a` 的存储偏移为 `0x100000`。
- `fw_payload_b` 的存储偏移为 `0x700000`。

GPT 生成器、工厂镜像打包脚本、U-Boot 烧写脚本和 SPL 加载代码必须共享同一份分区定义，禁止各自维护常量。

### 3.2 DDR 加载和运行地址

OpenSBI、U-Boot 和 FreeRTOS 在 DDR 中的 load/entry 地址与 QSPI A/B 分区偏移是两组不同的地址。

当前已有内存地址原则上保持不变：

| 内容 | 当前内存地址 | 本方案处理 |
| --- | ---: | --- |
| OpenSBI `fw_payload` load/entry | `0x40000000` | 保持不变 |
| U-Boot proper next/load address | `0x40200000` | 保持不变 |
| FreeRTOS 运行地址 | `0x6E800000` | 保持不变 |

只有在后续发现 DDR 布局本身冲突时，才单独调整运行地址；不得因为增加 QSPI A/B 分区而修改它们。

### 3.3 `fw_payload` 的组成

当前 OpenSBI、U-Boot 和 FreeRTOS 不再拆分为独立 NOR 分区，而是组成一个 SPL 可加载的 FIT 镜像：

```text
FreeRTOS
   ↓ 追加到 U-Boot 固定内部偏移
U-Boot + FreeRTOS
   ↓ 作为 OpenSBI FW_PAYLOAD_PATH
OpenSBI + U-Boot + FreeRTOS
   ↓ mkimage 生成 SPL 可读取的 FIT
visionfive2_fw_payload_amp.img
```

在分区和烧写阶段，同一份合法产物可以分别写入 `fw_payload_a` 和 `fw_payload_b`。后续为两个 bank 生成不同版本时，其内部内存地址仍保持相同。

## 4. 最终 SPI NOR 分区布局

### 4.1 地址表

| 物理区域 | 起始地址 | 结束地址 | 大小 | 用途 |
| --- | ---: | ---: | ---: | --- |
| `spl_a` | `0x000000` | `0x03FFFF` | 256 KiB | BootROM 固定加载的主 SPL |
| `spl_b` | `0x040000` | `0x07FFFF` | 256 KiB | SPL 备份和恢复源 |
| `gpt` | `0x080000` | `0x0BFFFF` | 256 KiB | 重定位 GPT 元数据 |
| `recovery` | `0x0C0000` | `0x0FFFFF` | 256 KiB | OTA 状态、bank 选择和掉电恢复信息 |
| `fw_payload_a` | `0x100000` | `0x6FFFFF` | 6 MiB | A bank OpenSBI + U-Boot + FreeRTOS FIT |
| `fw_payload_b` | `0x700000` | `0xCFFFFF` | 6 MiB | B bank OpenSBI + U-Boot + FreeRTOS FIT |
| 保留区域 | `0xD00000` | `0xFFFFFF` | 3 MiB | 未来扩容，当前不承载业务语义 |

### 4.2 线性布局

```text
0x000000  +----------------------+  spl_a          256 KiB
          | BootROM entry        |
0x040000  +----------------------+  spl_b          256 KiB
          | SPL backup           |
0x080000  +----------------------+  gpt            256 KiB
          | relocated GPT        |
0x0C0000  +----------------------+  recovery       256 KiB
          | recovery records     |
0x100000  +----------------------+  fw_payload_a     6 MiB
          | OpenSBI/U-Boot/RTOS  |
0x700000  +----------------------+  fw_payload_b     6 MiB
          | OpenSBI/U-Boot/RTOS  |
0xD00000  +----------------------+  reserved         3 MiB
0x1000000 +----------------------+  NOR end
```

### 4.3 强制约束

- NOR 总容量固定为 `0x1000000` 字节。
- 所有一级分区都按 `0x40000` 字节边界对齐。
- SPL 镜像必须满足 `image_size <= 0x40000`。
- `fw_payload` 镜像必须满足 `image_size <= 0x600000`。
- 分区不得重叠，不得越过 `0xFFFFFF`。
- 工厂镜像中未写入区域必须填充 `0xFF`，不使用全零填充。
- 保留区域不得在当前阶段被隐式分配。
- 分区地址修改必须先修改唯一布局配置，再重新生成其他产物。

## 5. 重定位 GPT 设计

### 5.1 定位

GPT 元数据位于 `gpt@0x080000`，而不是整个 NOR 的 LBA0。原因是 NOR `0x0` 必须保留给 BootROM 加载 `spl_a`。

因此，这是一份使用 GPT Header/Entry/CRC 格式的项目内嵌式分区元数据，不是标准 PC 磁盘 LBA0 GPT。`fdisk` 不是它的板端验收工具，应使用项目内 host 检查器和 SPL/U-Boot GPT 解析器验证。

### 5.2 LBA 规则

- Sector/LBA 大小固定为 512 字节。
- GPT 读取基址固定为 `0x080000`。
- GPT Entry 中的 `first_lba/last_lba` 表示相对整个 NOR 起点的绝对 LBA，不是相对 `gpt` 分区的 LBA。
- 解析器读取 GPT Header/Entry 时使用 `GPT_FLASH_OFFSET`；读取具体分区时直接使用 Entry 中的绝对 LBA。
- 本阶段 GPT 为静态工厂元数据，不参与 OTA 时的频繁更新。

### 5.3 GPT 中的分区条目

为了使物理布局和对外分区名一致，项目自定义 GPT 中保留以下六个 Entry：

| 分区名 | `first_lba` | `last_lba` | 字节范围 |
| --- | ---: | ---: | --- |
| `spl_a` | `0` | `511` | `0x000000-0x03FFFF` |
| `spl_b` | `512` | `1023` | `0x040000-0x07FFFF` |
| `gpt` | `1024` | `1535` | `0x080000-0x0BFFFF` |
| `recovery` | `1536` | `2047` | `0x0C0000-0x0FFFFF` |
| `fw_payload_a` | `2048` | `14335` | `0x100000-0x6FFFFF` |
| `fw_payload_b` | `14336` | `26623` | `0x700000-0xCFFFFF` |

`gpt` Entry 描述 GPT 自身是项目自定义行为，用于让 host、U-Boot、FreeRTOS nor agent 和 Linux MTD 注册结果对六个物理区域使用相同名称。普通业务烧写接口不应开放 `gpt` 更新；它只由布局/工厂镜像命令修改。

## 6. recovery 区域设计

### 6.1 外部地址

- 固定起始地址：`0x0C0000`。
- 固定大小：`0x40000`。
- recovery 镜像生成后直接烧写到该区域，后续 A/B 实现不改地址。

### 6.2 内部记录

GD25LQ128 的 page size 为 256 Bytes，最小擦除粒度为 4 KiB。recovery 物理分区仍保持 256 KiB，当前只使用第一个 4 KiB 擦除单元，不保存备份副本：

| 内部区域 | 绝对地址 | 大小 | 用途 |
| --- | ---: | ---: | --- |
| recovery 有效记录 | `0x0C0000-0x0C0FFF` | 4 KiB | 保存一份 recovery 结构体 |
| recovery 内部保留区 | `0x0C1000-0x0FFFFF` | 252 KiB | 当前填充 `0xFF` |

有效记录包含 recovery 结构体和 Header CRC32，结构体之后到 4 KiB 结尾填充 `0xFF`。

SPL 修改 `current_bank` 时只操作 `0x0C0000-0x0C0FFF`：

1. 将首个 4 KiB 完整读入 RAM。
2. 校验 recovery 结构体，并在 RAM 副本中修改 `current_bank`。
3. 重新计算 Header CRC32。
4. 擦除 `0x0C0000-0x0C0FFF` 这一个 4 KiB 单元。
5. 按 256 Bytes page 边界写回并回读校验。

本阶段明确接受单副本更新的掉电风险。如果擦除/写入中掉电导致 recovery 无效，SPL 使用默认 A bank 启动，并由后续恢复流程重建 recovery。

初始镜像状态固定为：

```text
usable_bank          = A | B
current_bank         = A
target_bank          = A
successful_bank_mask = A
boot_success         = 1
ota_state            = idle
ota_reboot_cnt       = 0
```

GPT 和 recovery 地址分开：GPT 只描述布局，recovery 只管理可变启动状态。禁止为了切换 bank 修改 GPT。

## 7. 统一分区定义和产物

### 7.1 唯一数据源

在实施时建立一份可机读的 NOR 布局配置，至少包含：

```text
flash_size
sector_size
alignment
partition.name
partition.offset
partition.size
partition.update_policy
```

以该配置生成或校验：

1. `gpt.img`。
2. `recovery.img`。
3. 16 MiB 工厂 NOR 镜像。
4. U-Boot TFTP 烧写参数或脚本。
5. SPL 使用的分区地址头文件。
6. Host 侧分区布局检查报告。

不允许在 GPT 生成器、Shell/Makefile、U-Boot 命令和 SPL 代码中手工复制六套 offset。

### 7.2 预期产物

| 产物 | 用途 |
| --- | --- |
| `spl_a.bin` | 写入 `0x000000`的主 SPL |
| `spl_b.bin` | 写入 `0x040000`的 SPL 备份，初期可与 A 相同 |
| `gpt.img` | 写入 `0x080000` 的重定位 GPT |
| `recovery.img` | 写入 `0x0C0000` 的初始 recovery |
| `fw_payload_a.img` | 写入 `0x100000` 的 A bank FIT |
| `fw_payload_b.img` | 写入 `0x700000` 的 B bank FIT |
| `nor_factory.img` | 按最终地址合成的 16 MiB 完整 NOR 镜像 |
| `nor-layout.txt` | 包含 offset、size、实际镜像大小和校验值的检查报告 |

## 8. 分步实施计划

## Step 0：实机基线与 NOR 恢复能力

### 目标

在改变 NOR 内容前，确保当前 SPL + payload + Linux 启动链可复现，且 NOR 可完整备份和恢复。

### 操作

- 记录 `sf probe` 识别的型号、容量和 erase size。
- 在 U-Boot 中读出完整 16 MiB NOR 并传回宿主机保存。
- 保存当前 SPL、`visionfive2_fw_payload_amp.img`、Linux FIT 和完整启动日志。
- 确认 UART 恢复或外部烧录路径可用。

### 通过标准

- 当前镜像可正常启动。
- NOR 备份长度为 `0x1000000`，并已计算校验值。
- 能够使用已确认方法恢复原 NOR 内容。

## Step 1：固化最终分区布局

### 目标

建立唯一布局配置和容量检查，使后续所有镜像操作都使用本文第 4 节的最终地址。

### 实施内容

- 将现有 32 MiB QEMU pFlash 配置改为 16 MiB VisionFive 2 NOR 配置。
- 分区名改为本文确定的六个名称。
- 增加容量、边界、重叠、对齐和产物大小检查。
- 生成供 C 代码使用的分区常量，不手工在 SPL 中重复地址。

### 通过标准

- 六个区域与保留区完整覆盖但不超过 16 MiB。
- 故意填入超大 SPL、超大 payload、重叠或越界地址时，构建必须失败。
- 布局报告显示的地址与本文一致。

## Step 2：GPT 镜像生成、烧写与读取

### 目标

在不改变当前 A 启动行为的前提下，将按最终地址生成的 `gpt.img` 写入 `0x080000`，并使 host 和板端都能正确解析六个区域。

### 实施内容

- 调整 GPT 生成器支持 `GPT_FLASH_OFFSET=0x080000`。
- 生成保护信息、GPT Header、Entry Array、Header CRC 和 Entry CRC。
- 增加 host 侧检查器，输出六个分区的名称、offset 和 size。
- 优先在 U-Boot 增加只读 GPT dump/校验能力，本步不实现 bank 选择。
- 通过 TFTP 下载并写入最终 GPT 地址。

### U-Boot 验证流程

```bash
tftpboot ${loadaddr} gpt.img
sf probe
sf update ${loadaddr} 0x080000 ${filesize}
sf read ${verifyaddr} 0x080000 ${filesize}
crc32 ${loadaddr} ${filesize}
crc32 ${verifyaddr} ${filesize}
```

`${verifyaddr}` 必须选择与 `${loadaddr}` 不重叠且容量足够的 DDR 区域，实际值在实施前通过 U-Boot `bdinfo` 和当前内存布局确认。

### 通过标准

- TFTP 内存镜像与 NOR 回读镜像 CRC32 一致。
- Host 和 U-Boot 输出的六个分区信息一致。
- 重启后仍按现有 `fw_payload_a@0x100000` 启动。

## Step 3：Linux/FreeRTOS nor agent 链路验证

### 目标

利用 Step 2 已经写入的 GPT 建立 NOR 分区视图，独立验证 Linux 到 FreeRTOS nor agent 的完整读写链路。当前这项验证必须放在 GPT 之后，因为 FreeRTOS 需要先从 NOR 解析 GPT，才能通过 mailbox 向 Linux 返回最终分区信息。Linux 不直接读取或解析 GPT。

### 验证链路

```text
Linux 测试命令/MTD 访问
  -> Linux nor-client
  -> mailbox consumer/router
  -> IPI + 共享内存
  -> FreeRTOS mailbox router
  -> FreeRTOS nor agent
  -> FreeRTOS GPT 解析/分区查询
  -> SPI NOR 读/擦除/写
  -> 回读数据
  -> 原路径返回 Linux
```

### 实施内容

- 确认 FreeRTOS nor agent 已注册并能处理 NOR info、partition info、read、erase 和 write 请求。
- FreeRTOS nor agent 从 `gpt@0x080000` 读取并校验 GPT Header/Entry CRC，生成六个分区的 name/offset/size 信息。
- Linux nor-client 通过 mailbox 获取 NOR 容量、page size、erase size 和 FreeRTOS 返回的分区信息。
- Linux nor-client 对返回的分区数量、名称、边界和重叠进行防御性检查，检查通过后注册 master MTD 和六个 MTD 子分区。
- 检查 `/proc/mtd` 或项目的分区查询接口，确认分区名、offset 和 size 与 host/U-Boot 一致。
- 对 `gpt`、`spl_a`、`spl_b` 和 `fw_payload_a` 只执行读测试，禁止擦除和写入。
- 在还没有写入有效 recovery 镜像的 `recovery@0x0C0000` 内执行受控的 erase/write/readback 测试。
- 测试前先回读并保存 `recovery` 整区原始数据；测试结束后恢复原数据或恢复为明确的 `0xFF` 擦除态。
- 分别使用 256 字节、512 字节、4 KiB 和跨 page 数据验证读写，检查长度、page 边界、擦除边界、超时和响应校验。
- 对越过 `recovery` 分区边界的请求执行负向测试，必须在操作 NOR 前被拒绝。

### 操作边界

- 本步只验证已有 nor agent 链路，不实现 recovery 业务语义和 A/B 选择。
- 不修改 GPT 分区布局。
- 不使用 `spl_a`、`spl_b`、`gpt` 或 `fw_payload_a` 作为破坏性测试区域。
- 如果 nor-client/nor agent 存在概率性 CRC、超时或队列溢出，必须在本步定位和修复，不得将规避逻辑放进 recovery 或 A/B 流程。

### 通过标准

- Linux 侧能稳定获取正确的 NOR info。
- FreeRTOS 能正确解析 GPT 并通过 mailbox 返回六个最终分区。
- Linux 能使用 FreeRTOS 返回的信息注册并列出六个 MTD 分区，地址与 host/U-Boot/FreeRTOS 输出完全一致。
- `recovery` 测试区的 erase/write/readback 数据完全一致。
- 连续多轮读写不出现 mailbox 超时、队列溢出、响应 CRC 错误或 FreeRTOS 任务卡死。
- 越界、非法长度和非法对齐请求被明确拒绝，且 NOR 内容不发生变化。
- 测试后重启，系统仍从 `spl_a` 和 `fw_payload_a` 正常启动。

## Step3.1：U-Boot 通过 Mailbox 实现 Flash 镜像刷写
## Step3.2：优化 FreeRTOS 的 Flash 驱动

## Step 4：recovery 镜像生成、烧写与读取

### 目标

在最终 `recovery@0x0C0000` 地址生成和验证 recovery 镜像，本步只读取状态，不影响启动 bank。

### 实施内容

- 统一 Python 生成器与 C 结构体的结构版本、字段偏移、总长和 CRC 范围。
- 对 C 结构增加 `sizeof`/`offsetof` 编译期断言。
- 按第 6.2 节生成默认 A bank 已确认成功的 recovery 状态。
- `recovery.img` 总长保持 256 KiB，只在前 4 KiB 放置有效记录，其余 252 KiB 填充 `0xFF`。
- 增加 U-Boot recovery dump/CRC 只读命令。
- 通过 TFTP 写入 `0x0C0000`，回读后比较 CRC 和字段。

### 通过标准

- Host 生成器、Host 解析器和 U-Boot 解析器对所有字段的理解一致。
- NOR 回读数据的 magic、version、bank 字段和 CRC 正确。
- recovery 无效或未写入时，当前 SPL 仍使用固定 A payload 启动。

## Step 5：按最终地址生成和烧写全部镜像

### 目标

在实现 A/B 选择前，先让六个区域全部按最终地址存在于 NOR 中，并完成逐分区回读校验。

### 实施内容

- 对 SPL 产物做大小检查，初期 `spl_a.bin` 和 `spl_b.bin` 使用同一份已验证 SPL。
- 使用当前 OpenSBI + U-Boot + FreeRTOS 构建流程生成 `fw_payload`，不修改 DDR load/entry 地址。
- 初期 `fw_payload_a.img` 和 `fw_payload_b.img` 可使用同一份已验证产物；切换验证时再为 B 加入可观测版本标识。
- 按以下固定顺序烧写和验证：`spl_b` → `fw_payload_b` → `gpt` → `recovery` → `fw_payload_a` → `spl_a`。
- `spl_a` 最后写入，且只在其他相关区域全部回读验证成功后才允许更新。
- 生成一份 16 MiB `nor_factory.img`，用于恢复和量产，但调试阶段仍以逐分区烧写为主。

### 通过标准

- 六个区域均能通过分区名找到最终 offset/size。
- 每个区域的 TFTP 源文件与 NOR 回读数据校验一致。
- 不启用 A/B 选择时，系统仍从 `spl_a` 和 `fw_payload_a` 完整启动。
- `nor_factory.img` 中每个产物的位置与单独烧写位置一致。

## Step 6：仅增加 A/B payload 选择

### 目标

在不改变任何分区地址、FIT 内部布局和 DDR 运行地址的前提下，让 `spl_a` 根据 recovery 选择 A/B payload。

### 新增逻辑

```text
BootROM
  -> spl_a@0x000000
  -> read GPT@0x080000
  -> read recovery@0x0C0000
  -> validate recovery
  -> read target_bank and select bank
       A -> fw_payload_a@0x100000
       B -> fw_payload_b@0x700000
  -> if current_bank != selected_bank
       update current_bank in RAM
       recalculate recovery CRC
       erase/write/readback recovery first 4 KiB
  -> load selected FIT to 0x40000000
  -> existing OpenSBI/U-Boot/FreeRTOS flow
  -> existing single Linux FIT flow
```

### 选择规则

- GPT/recovery 无效或 `target_bank` 越界：选择 A，不尝试重建或写入无效 recovery。
- `target_bank=A`：选择并加载 A。
- `target_bank=B`：选择并加载 B。
- `current_bank` 已等于选中 bank 时不写 NOR，避免每次启动都擦写 recovery。
- `current_bank` 不等于选中 bank 时，先按第 6.2 节修改和回读校验 recovery，再加载镜像。
- 本阶段不校验 payload 完整性，不根据镜像损坏自动回退。加载/启动失败时保留诊断日志，完整性检查留到真正 OTA 阶段实现。

### 实现边界

- 本步只增加 SPL 的 `target_bank` 识别、分区选择、`current_bank` 更新和对应 payload 加载逻辑。
- 不改 GPT 生成地址。
- 不改 recovery 地址和结构。
- 不改 OpenSBI、U-Boot、FreeRTOS 的 DDR 运行地址。
- 不实现 Linux kernel/rootfs A/B。
- 不实现网络 OTA Client 和 Linux 自动写 NOR。
- 不实现 payload 完整性检查、自动增加启动次数和三次失败回退。

### 通过标准

- recovery 选 A 时，日志明确显示并启动 `fw_payload_a@0x100000`。
- recovery 选 B 时，日志明确显示并启动 `fw_payload_b@0x700000`。
- 从 A 切换到 B 后，回读 recovery 可观察到 `current_bank=B`。
- 从 B 切换到 A 后，回读 recovery 可观察到 `current_bank=A`。
- `current_bank` 已等于 `target_bank` 时，日志表明 SPL 跳过 recovery 擦写。
- 破坏 recovery CRC 后安全选择 A，且不覆盖无效 recovery。
- 冷启动和软重启都能使用同一套地址正常启动。

## Step 7：SPL 备份与恢复

### 目标

使 `spl_b` 成为可验证、可恢复的 SPL 备份，但不虚假声称 BootROM 具备 SPL 自动 A/B 能力。

### 实施内容

- U-Boot 提供 SPL A/B 镜像头、大小和 CRC 检查。
- SPL 更新时先写 `spl_b`，回读校验后才写 `spl_a`。
- 提供显式的 `spl_b -> spl_a` 恢复流程。
- 记录 `spl_a` 完全损坏时必须使用 UART 恢复或外部烧录的硬件边界。

### 通过标准

- U-Boot 能分别输出 SPL A/B 的版本和校验结果。
- 可以在 U-Boot 中使用 `spl_b` 恢复 `spl_a`。
- 恢复后冷启动成功。

## 9. U-Boot TFTP 烧写规则

### 9.1 建议流程

每个分区的烧写都遵循：

```text
TFTP 下载
  -> 检查 filesize <= partition.size
  -> 校验内存中的镜像
  -> sf update 到分区固定 offset
  -> sf read 到另一块 DDR
  -> 回读 CRC/compare
  -> 只在成功后继续下一分区
```

如果当前 U-Boot 不方便对 `${filesize}` 与分区大小做安全比较，应增加按分区名封装的烧写命令，不把裸 `sf erase/write` 作为最终使用接口。

### 9.2 烧写保护

- `spl_a` 默认只读，需要显式解锁才能更新。
- `gpt` 只能由布局/工厂镜像流程更新。
- `recovery` 更新必须使用状态封装，不允许业务层随意整区覆盖。
- payload 更新优先写非当前 bank。
- 任何烧写超过目标分区大小时必须立即失败。

## 10. 后续 A/B 实现时不得变更的内容

完成 Step 1～Step 5 并通过实机验证后，以下内容视为分区 ABI，后续 A/B 代码不得擅自修改：

- 六个分区的名称。
- 六个分区的 offset 和 size。
- GPT 读取基址 `0x080000`。
- recovery 读取基址 `0x0C0000`。
- A payload 偏移 `0x100000`。
- B payload 偏移 `0x700000`。
- GPT Entry 使用整个 NOR 绝对 LBA 的语义。
- recovery 结构体的字段语义和 CRC 规则。
- OpenSBI `0x40000000`、U-Boot `0x40200000`、FreeRTOS `0x6E800000` 的内存地址。

后续 A/B 启动的本质必须保持为：

```text
selected_bank == A ? 0x100000 : 0x700000
```

而不是重新计算或迁移镜像地址。

## 11. 暂不实现的内容

- Linux kernel/rootfs A/B 分区。
- Linux OTA Client 自动下载和升级。
- Linux 通过 mailbox/nor-agent 自动更新 SPI NOR。
- 网络端 OTA 状态机和 Stage1/Stage2 全流程。
- payload 完整性检查和基于校验失败的自动 bank 回退。
- `ota_reboot_cnt` 自动增加和三次启动失败回退。
- recovery 多副本或掉电安全更新。
- BootROM 自动从 `spl_a` 回退到 `spl_b`。
- 因 A/B 分区而修改 OpenSBI/U-Boot/FreeRTOS DDR 运行地址。

## 12. 主要风险

| 风险 | 处理方式 |
| --- | --- |
| SPL 超过 256 KiB | 构建时强制拒绝，不允许截断 |
| payload 超过 6 MiB | 构建时拒绝；必须重新评审分区 ABI |
| GPT 被标准工具误判 | 文档中明确其为重定位项目 GPT，使用专用检查器 |
| 多处硬编码地址漂移 | 单一布局配置生成 C 常量和烧写参数 |
| TFTP 下载大小超过分区 | 写入前强制比较 `${filesize}` |
| 烧写中掉电 | 先 B 后 A，每次写入后必须回读校验 |
| `spl_a` 完全损坏 | 使用 UART/外部恢复；`spl_b` 不是 BootROM 自动入口 |
| recovery 单副本更新中掉电 | 当前阶段接受该风险；CRC 无效时 SPL 默认启动 A，后续 OTA 阶段再实现掉电安全更新 |

## 13. 实施约束

- 一次只实施一个 Step。
- 开始每个 Step 前，列出当次必须修改的文件和明确禁止修改的范围。
- 每个 Step 必须包含 host 侧静态检查、NOR 回读校验和实机启动验证中的适用项。
- 当前 Step 没有得到“测试通过”确认前，不开始下一个 Step。
- 发现必须修改分区 ABI 时，立即停止实施，先修订本文档并重新审核。
