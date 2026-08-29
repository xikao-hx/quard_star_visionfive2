# Quard-Star 项目真机移植方案（VisionFive 2）

# 1. 文档说明
* 本文档总结将本仓库（QEMU RISC-V 模拟 SoC）的 **mailbox + OTA + trusted domain** 三大功能迁移到真实开发板上的完整方案。
* 内容来源：前期调研结论汇总，覆盖板卡选型、移植内容拆解、注意项与分阶段行动计划。
* 结论先行：**推荐目标板为 StarFive VisionFive 2（JH7110）**，理由见 §3。
* 本文档不包含具体代码，只做决策记录与实施路线。

# 2. 移植目标
## 2.1 要迁移的三大功能
1. **trusted domain 隔离**：OpenSBI PMP 域划分，一个核跑 FreeRTOS（可信域），其余核跑 Linux（不可信域），硬件隔离。
2. **mailbox 通信**：Linux ↔ FreeRTOS 的 IPC，承载 remote shell、日志落盘、nor agent 等服务。
3. **OTA A/B 升级**：双分区 + 回滚 + recovery v2 协议。

## 2.2 本项目的核心资产
* 项目的 trusted/untrusted 域隔离基于 **OpenSBI PMP**（`fdt_domains_populate`），这是**真实硬件技术**，不是 QEMU 模拟——移植时隔离逻辑可平移，只需改 dts。
* FreeRTOS 固件、Linux 侧 OTA 中间件（`basic_middleware/ota_package/` 纯 C）、recovery v2 协议均为平台无关代码。

# 3. 目标板卡选型
## 3.1 结论
* **首选：StarFive VisionFive 2（JH7110）**——同构多核 + OpenSBI 域，与 QEMU 项目玩法一致，成本最低（¥500-900，8GB 版）。
* 备选：Microchip PolarFire SoC（有独立小核 E51，最接近"小核先启动"范式，但贵、HSS 定制工作量大）。

## 3.2 为何排除其他方案
| 方案 | 排除原因 |
|---|---|
| STM32MP257F-EV1 | 官方 M33-TD 唯一 MP257 板，但 ¥3000+，且 ARM 需重写隔离层 |
| STM32MP257F-DK | **官方不支持 M33-TD**（仅 A35-TD）|
| 正点原子 ATK-DLMP257B | 不在官方 M33-TD 验证列表，无 NOR 启动模式 |
| STM32MP215F-DK | 单 A35，性能不足 |
| LicheePi 4A / TH1520 | C906 小核生态薄 |
| RK3506 | 实时隔离而非安全隔离 |

## 3.3 Vision Five 2 板级要点
* **存储**：外置 SD 卡 / eMMC（**插座式**，eMMC 模块为选配件，购买时确认是否包含）；板载 16MiB SPI NOR（GD25LQ128，只放 bootloader）。
* **启动模式**（拨码开关）：00=QSPI NOR，01=SD，10=eMMC，11=UART 恢复。官方建议从 SPI NOR 间接引导 SD/eMMC/NVMe。
* **显示**：仅 HDMI（4K@30）与 MIPI-DSI（4-lane，1080P@30）；**并行 RGB 未引出**，韦东山 i.MX6ULL 屏、Luckfox RGB 屏均不可直连。DSI 屏需在官方兼容列表内（如微雪 7inch DSI LCD B/C、LUCKFOX 7 寸 DSI，800×480）。
* **选型建议**：买原版（非 Lite）8GB 版；Lite 版 SD 与 eMMC 共用 SDIO 总线、二选一，不适合反复 OTA 测试。

# 4. 目标板启动流程（JH7110）
```
┌─────────────┐   ┌──────────────┐   ┌──────────────────────────┐   ┌──────────────┐
│ JH7110 ROM  │ → │ U-Boot SPL   │ → │ OpenSBI (fw_payload,     │ → │ U-Boot proper │ → Linux
│ (固化,不可改)│   │ (M-mode)     │   │  M-mode + 内嵌U-Boot)     │   │  (S-mode)     │
└─────────────┘   └──────────────┘   └──────────────────────────┘   └──────────────┘
   内部固化           从 SPI NOR 加载      加载到 0x40000000            跳到 0x40200000
                   "Trying to boot from SPI"                         从 SD/eMMC 加载 kernel
```
* 存储布局（SPI NOR 16MiB）：`0x0` = U-Boot SPL（`u-boot-spl.bin.normal.out`）；`0x100000` = OpenSBI+U-Boot（`visionfive2_fw_payload.img`）。
* 官方 SDK 日志：`Platform HART Count: 5`（4x U74 + 1x S7），`Boot HART ID: 3`，`Boot HART PMP Count: 8`，`Domain0 Next Address: 0x40200000 (S-mode)`。
* OpenSBI 从 SPL 接过 FDT，解析 `opensbi-domains` 节点——**域配置注入点与此项目 QEMU 相同**。

# 5. 移植内容拆解
## 5.1 可直接平移（零/低改动）
| 内容 | 位置 | 说明 |
|---|---|---|
| OTA 中间件 | `basic_middleware/ota_package/` | 纯 C，平台无关 |
| recovery v2 结构体 | `common_inc/bsp/quard_recovery_config.h` | 协议不变 |
| mailbox 协议/服务层 | `common_inc/bsp/quard_router_protocol.h`、`quard_router_server_id.h` | 28 字节消息 + 路由，与传输无关 |
| OpenSBI 域隔离逻辑 | `opensbi-0.9/platform/quard_star/platform.c` | 真板 JH7110 用 generic platform，`fdt_domains_populate` 为默认行为，**无需重写 C**，只需把 `bsp/dts/quard_star_sbi.dts` 的 `opensbi-domains` 节点平移进 JH7110 的 dts |

## 5.2 必须重写/移植
| 内容 | 位置 | 改动 |
|---|---|---|
| FreeRTOS 驱动（UART/定时器/中断/内存布局） | `trusted_domain/driver/` | JH7110 外设全不同，重写寄存器层 |
| mailbox 传输层 | `trusted_domain/driver/mailbox/mailbox.c` 的 `__ipc_send`/`__ipc_rcv` | PL320 寄存器 → 共享内存 + SBI IPI doorbell |
| Linux mailbox controller | `bsp/mailbox/mailbox/quard_mailbox.c` | 新写一个共享内存 + SBI IPI 的 mbox_controller，client/router 不动 |
| 存储 HAL | `basic_middleware/ota_info/lib/bh_ota_hal.c` | 适配 SD/eMMC 块设备 |
| BL1 → U-Boot SPL | `BL1/ota/quard_bootctrl.c` | A/B 选择逻辑落到 SPL 或 U-Boot proper（见 §6.4）|
| U-Boot bootctrl | `u-boot-2021.07/board/emulation/qemu-quard-star/quard_bootctrl.c` | 移植到 JH7110 U-Boot |
| nor agent | `trusted_domain/driver/nor_agent/quard_nor_agent.c` | 若 FreeRTOS 固件放 SPI NOR，Linux 需借 mailbox 让 FreeRTOS 自写 flash——**该机制真板重生** |

## 5.3 FreeRTOS 在 VisionFive 2 上的可行性（已证实）
* **结论：可行，且已有官方 AMP 先例 + 社区 FreeRTOS 移植两条现成路径。**
* **官方 AMP 应用笔记（VisionFive 2-ANCH-020）**：StarFive 官方演示了完全相同的架构——**3 个 U74 跑 Linux + 1 个 U74 跑 RTOS**，证明"划一个核跑实时系统"在 JH7110 上是官方走通的路。只需将 RT-Thread 换成 FreeRTOS、官方 RPMsg 换成项目自研的 28 字节 mailbox 协议。
* **官方 AMP 方案的核间通信**：virtio-based RPMsg + IPI 中断 + 共享内存；串口 Linux 用 UART0、RTOS 用 UART2；Linux 侧驱动 `drivers/rpmsg/virtio_rpmsg_starfive.c` + `drivers/mailbox/starfive_ipi_mailbox.c`；RT-Thread 侧 `rtthread/bsp/starfive/jh7110/`（基于 RT-Thread 5.0.2）；发布在 StarFive Linux 6.6 SDK。
* **Linux 侧 mailbox controller 有官方现成实现**（`starfive_ipi_mailbox.c`，共享内存 + IPI）——对应本项目 Linux 侧要新写的 mbox_controller，无需从零造。
* **社区 FreeRTOS 先例**：`strangerover2002/visionfive2` 仓库（SiFive freedom-tools 编译，FreeRTOS 跑在全部 4 个 U74 上，OpenOCD 调试），FreeRTOS 移植有参照、非从零。
* **差异化工作**：官方 AMP 方案**未做 PMP 域隔离**（仅是简单内存划分 + 君子协定），本项目要在其基础上加 OpenSBI domain——这是项目的价值点与难点，但底层可行性已被官方证明。

# 6. 关键技术注意项
## 6.1 OpenSBI 域划分
* 核心：dts 加 `opensbi-domains` 节点，划一个核 + 一块 DDR 出来。机制与 QEMU 相同。
* **难点在启动链共存**：OpenSBI 起来后要先交棒给 U-Boot proper（S-mode），U-Boot 再加载 Linux。必须保证：
  * U-Boot 不踩划给 FreeRTOS 的 DDR 区域；
  * Linux dts 中该区域不出现在内存可用范围。
* **版本差异**：JH7110 需 OpenSBI ≥ 1.2（官方 SDK 为 v1.0 fw_payload 模式）；本项目是 0.9。域 API/FDT 绑定有小差异，需核对迁移，不是 copy-paste。建议以 **StarFive SDK fork 为基线**（U-Boot 2021.10、内核 5.15、OpenSBI v1.0），与项目时代（0.9 / 2021.07）最接近，改动最少。
* **特权级决策**：FreeRTOS 跑 M 态还是 S 态，决定 IPI doorbell 路径（M 态直接收 MSIP；S 态要绕 OpenSBI）。移植早期必须定死。

## 6.2 mailbox 传输层
* **VisionFive 2 无 PL320/硬件 mailbox**，标准做法 = 共享内存环形缓冲 + doorbell 中断。
* 数据通道：共享一块 DDR（PMP 两侧放行），写入 28 字节消息。
* doorbell：Linux 侧调 SBI `sbi_send_ipi(hartid=FreeRTOS核)` → 目标核 MSIP 置位 → FreeRTOS trap handler 处理。
* **服务层/协议层完全不动**，只换传输实现（改动量约单文件几十行）。

## 6.3 存储与 OTA
* eMMC 分区 A/B（boot_a/rootfs_a/boot_b/rootfs_b/userdata）需在 SD/eMMC 上重建。
* **FreeRTOS 固件存放位置**是核心决策：
  * 放 SPI NOR → Linux 受 domain/防火墙限制写不了 → 必须走 mailbox 让 FreeRTOS 自写（nor agent 重生）；
  * 放 eMMC 分区 → A/B 一起做，更新路径简单，但"固件独立 flash"初衷打折。
* recovery v2 协议、两阶段 OTA 状态机直接平移。

## 6.4 BL1 / SPL 的 A/B 职责
* U-Boot SPL 通常**不支持文件系统/分区表**，只能从固定偏移 raw 读——在 SPL 里做 A/B 判断绕弯多。
* **主流做法**：SPL 无条件加载 U-Boot proper，A/B 决策放到 U-Boot proper（读 boot 控制块，如 Android A/B 的 BCB）。
* 本项目 BL1 与 U-Boot 两层 bootctrl 的职责在真板上需**重新分配**——设计决策，不是平移。

## 6.5 不可修改的二进制
* JH7110 内部 BootROM（mask ROM，只做按拨码读 SPL，不影响本移植）。
* DDR 训练固件（blob，日志中 `DDR version: dc2e84f0`）——启动胶水，与业务逻辑无关，必须用 StarFive 的。
* 注意：控制权从 SPL 才开始（QEMU 里从 reset 即可控），是心态上需调整的点。

## 6.6 FreeRTOS 移植注意项
* **mtvt CSR（0x307）**：SiFive 的裸机/FreeRTOS 示例使用 `mtvt`（机器级中断向量表基址，CLIC 规范）。该 CSR **不在 RISC-V 特权规范中，上游 GNU binutils 不支持**——需用 SiFive 下游工具链，或用数字形式 `csrr %0, 0x307` 绕过。项目标准 riscv 工具链编译时可能踩此坑。
* **FDT 的 hart 0 是 S7 不是 U74**：JH7110 的 hart 0 实际是 **S7 监控核**（无 MMU/S 态），部分固件 FDT 错误标成 U74。**FreeRTOS 应放 U74 核（hart 1-4），避免放 hart 0。**
* **官方 AMP 无隔离**：官方方案 Linux 与 RTOS 共享内存是"君子协定"无 PMP 强制，本项目要在此基础上加 OpenSBI 域隔离，不能照抄官方 AMP。

# 7. 行动计划
## Phase 0：板卡到货探针（第 1-2 周，最高优先级）
* [ ] 决策：FreeRTOS 固件放 SPI NOR 还是 eMMC
* [ ] 决策：A/B 决策放 SPL 还是 U-Boot proper
* [ ] 决策：FreeRTOS 跑 M 态还是 S 态
* [ ] 决策：FreeRTOS 用哪个核（U74 之一 or 验证 S7 监控核可用性）
* [ ] 在 JH7110 上配置 OpenSBI 域：一个核划成 PMP 隔离域
* [ ] FreeRTOS "hello world + LED" 在隔离域核上跑通，同时 Linux 正常启动
* **通过标准**：两件都成 → 后续全为标准工程；卡住 → 暴露真难点，考虑回退 PolarFire

## Phase 1：FreeRTOS 移植（第 3-4 周）
* [ ] UART / 定时器（aclint-mtimer）/ 中断（PLIC）/ 内存布局驱动
* [ ] 在隔离域核上跑通完整 FreeRTOS 调度

## Phase 2：mailbox 传输层（第 5 周）
* [ ] 共享内存 + SBI IPI doorbell 实现（Linux 与 FreeRTOS 两侧）
* [ ] 移植 mbox_router + remote shell + 日志落盘服务，验证端到端 IPC

## Phase 3：存储与 OTA（第 6-8 周）
* [ ] eMMC/SD 分区布局（A/B + userdata）
* [ ] `bh_ota_hal.c` 存储适配
* [ ] U-Boot bootctrl 移植（A/B 选择 + bootcount 回滚）
* [ ] 两阶段 OTA 全流程 + 回滚验证

## Phase 4：完整启动链整合（第 9-10 周）
* [ ] ROM → SPL → OpenSBI → U-Boot → Linux 全链验证
* [ ] FreeRTOS 固件 OTA 更新路径（含 nor agent 重生场景）
* [ ] 整体回滚 + 异常恢复演练

# 8. 风险清单
| 风险 | 等级 | 缓解 |
|---|---|---|
| JH7110 启动链为纯 Linux 设计，域隔离无官方支持 | 高 | Phase 0 探针 2 周内暴露 |
| OpenSBI 0.9 → 1.x 域 API 差异 | 中 | 以 StarFive SDK fork 为基线，核对迁移 |
| S7 核可编程性未验证（且 FDT 将 hart0 误标为 U74）| 中 | 默认用 U74 核（官方 AMP 即如此），S7 仅作备选验证 |
| SPL 级 A/B 复杂 | 中 | 采用 U-Boot proper 做 A/B 的主流做法 |
| 文档缺失处需逆向（SPL→OpenSBI FDT 传递等） | 中 | 黑盒实验，预留时间 |

# 9. 参考来源
* StarFive VisionFive2 SDK（含实机启动日志）：https://github.com/starfive-tech/VisionFive2
* U-Boot VisionFive2 文档：https://docs.u-boot.org/en/v2025.04/board/starfive/visionfive2.html
* StarFive JH7110 OpenSBI domain_support.md：https://glab.starfivetech.com/jh7110/opensbi/-/blob/.../docs/domain_support.md
* OpenSBI Domain Context Switching：https://deepwiki.com/riscv-software-src/opensbi/5.3-domain-context-switching
* StarFive 官方 AMP 应用笔记（RT-Thread + Linux）：https://doc-en.rvspace.org/VisionFive2/AN_RT-Thread/RT_Thread/introduction.html
* 论坛帖：JH-7110 现已支持 AMP 双系统：https://support1.starfivetech.com/t/jh-7110-amp-linux-rt-thread/4067/2
* 社区仓库：FreeRTOS on VisionFive2 4 核：https://github.com/strangerover2002/visionfive2-
* GitHub Issue：FDT 错误将 hart0 标为 U74：https://github.com/starfive-tech/VisionFive2/issues/33
* PolarFire SoC AMP 文档：https://github.com/polarfire-soc/polarfire-soc-documentation/blob/master/applications-and-demos/asymmetric-multiprocessing/amp.md
* VisionFive 2 Camera and Display Interfaces：https://doc-en.rvspace.org/VisionFive2/Datasheet/VisionFive_2/camera_and_display_interfaces.html
* VisionFive 2 40-Pin GPIO Header 用户手册：https://doc.rvspace.org/VisionFive2/40_Pin_GPIO_Header_UG/index.html
* RVspace 论坛：Compatible MIPI DSI display recommendations：https://china-risc.starfivetech.com/t/compatible-mipi-dsi-display-recommendations/526/2
* 微雪 7inch DSI LCD：https://www.waveshare.net/shop/7inch-DSI-LCD-B-with-case-A.htm
