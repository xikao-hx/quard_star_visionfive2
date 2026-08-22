# VisionFive 2 Linux 6.6 AMP SDK

本项目基于 StarFive VisionFive 2 Linux 6.6 SDK，面向 JH7110 开发板扩展了 Linux + RTOS 异构多核（AMP）运行环境。当前默认 RTOS 为 FreeRTOS，也保留 RT-Thread 构建支持，并在 AMP 通信之上实现了远程 Shell、启动日志采集、SPI-NOR 代理访问和 OTA A/B 升级相关组件。

> 本仓库包含正在持续开发和验证的板级功能。普通 Linux SDK 构建沿用 StarFive 原始流程；AMP、日志和 OTA 功能请优先参考 `doc/` 中与当前实现对应的文档。

## 项目特性

- Linux 6.6、OpenSBI、U-Boot、Buildroot 完整启动链
- FreeRTOS / RT-Thread 可选 AMP 运行时
- OpenSBI domain 与设备树内存隔离配置
- Linux 与 RTOS 间的共享内存 + IPI Mailbox 通信
- Mailbox Router、远程 Console、日志和 NOR Agent 服务
- SPL、OpenSBI、U-Boot 与 RTOS 日志采集及落盘
- OTA 包、设备升级客户端、OTA 服务端及 A/B 状态管理
- Buildroot initramfs、根文件系统和 SD 卡镜像构建

## 系统架构

```text
JH7110 BootROM
    |
    v
U-Boot SPL (M-mode, SPI NOR)
    |
    +-- 加载 AMP RTOS 固件
    v
OpenSBI (domain / PMP 隔离)
    |
    +-- FreeRTOS 或 RT-Thread：独占 U74 hart 与保留内存
    v
U-Boot proper (S-mode)
    |
    v
Linux 6.6 + Buildroot
    |
    +-- IPI Mailbox Controller
        +-- Message Router
            +-- Remote Console
            +-- Log Service
            +-- NOR Client / Agent
```

AMP 默认内存规划如下，最终配置以设备树和链接脚本为准：

| 用途 | 地址范围 | 大小 |
| --- | --- | ---: |
| OpenSBI | `0x40000000` - `0x401fffff` | 2 MiB |
| U-Boot | `0x40200000` - `0x4032ffff` | 约 1.19 MiB |
| Linux / RTOS 共享内存 | `0x6e400000` - `0x6e7fffff` | 4 MiB |
| RTOS 代码和栈 | `0x6e800000` - `0x6effffff` | 8 MiB |
| RTOS 堆 | `0x6f000000` - `0x6fffffff` | 16 MiB |

## 目录说明

| 目录 | 内容 |
| --- | --- |
| `linux/` | Linux 6.6 内核及 JH7110 AMP 设备树 |
| `u-boot/` | SPL、U-Boot proper 与 AMP 固件装载逻辑 |
| `opensbi/` | OpenSBI 固件及 domain 支持 |
| `buildroot/`、`conf/` | initramfs、rootfs 和 FIT 镜像配置 |
| `trusted_domain/` | JH7110 FreeRTOS 固件、驱动和可信域服务 |
| `rtthread/` | RT-Thread AMP 运行时 |
| `bsp/ipi_mailbox/` | Linux IPI Mailbox、Router 与 Consumer 驱动 |
| `common_inc/` | Linux / RTOS 共用的消息和日志协议头文件 |
| `app/soc_log/` | SoC 日志落盘守护进程 |
| `basic_middleware/` | OTA 包、OTA 信息和板端 OTA Client |
| `ota_server/` | OTA 发布、查询和管理服务端 |
| `nfs_rootfs/` | AMP 联调时通过 NFS 挂载的程序和模块 |
| `script/` | SDK 构建、镜像生成和后处理脚本 |
| `doc/` | 启动、AMP、通信、日志和 OTA 文档 |

## 环境准备

推荐使用 Ubuntu 20.04 或 22.04 x86_64 主机。首次完整构建需要下载较多依赖，建议至少预留 25 GiB 可用空间。

```bash
sudo apt update
sudo apt install -y \
  build-essential automake libtool texinfo bison flex gawk g++ git git-lfs \
  xxd curl wget gdisk gperf cpio bc screen unzip libgmp-dev libmpfr-dev \
  libmpc-dev libssl-dev libncurses-dev libglib2.0-dev libpixman-1-dev \
  libyaml-dev patchutils python3-pip zlib1g-dev device-tree-compiler \
  dosfstools mtools kpartx rsync scons
```

工具链分为两套：

- Linux / U-Boot / OpenSBI 使用 Buildroot 在 `work/buildroot_initramfs/host/` 生成的 `riscv64-buildroot-linux-gnu-` 工具链。
- FreeRTOS / RT-Thread 默认使用 `/opt/riscv/bin/riscv64-unknown-elf-` 裸机工具链。

可用 `TRUSTED_CROSS_COMPILE` 覆盖 FreeRTOS 工具链，RT-Thread 则使用 `RTTHREAD_EXEC_PATH` 和 `RTTHREAD_CC_PREFIX`：

```bash
make ampuboot_fit AMP_RTOS=freertos \
  TRUSTED_CROSS_COMPILE=/path/to/riscv64-unknown-elf- -j"$(nproc)"
```

## 获取代码

克隆后必须初始化所有子模块：

```bash
git clone <repository-url> VisionFive2_6.6
cd VisionFive2_6.6
git submodule update --init --recursive
```

如需从 StarFive 原始仓库重新搭建基线，请参考 [SDK 下载与编译说明](doc/软件文档/SDK下载与编译说明.md)，其中记录了各子模块分支和浅克隆注意事项。

## 快速构建

### 普通 Linux 镜像

```bash
make -j"$(nproc)"
```

主要产物：

```text
work/
├── image.fit
├── initramfs.cpio.gz
├── u-boot-spl.bin.normal.out
├── visionfive2_fw_payload.img
└── linux/arch/riscv/boot/
    ├── Image.gz
    └── dts/starfive/*.dtb
```

### AMP 镜像（默认 FreeRTOS）

```bash
make ampuboot_fit AMP_RTOS=freertos -j"$(nproc)"
make ampfit AMP_RTOS=freertos -j"$(nproc)"
```

切换到 RT-Thread：

```bash
make amp-clean
make ampuboot_fit AMP_RTOS=rtthread -j"$(nproc)"
make ampfit AMP_RTOS=rtthread -j"$(nproc)"
```

切换 RTOS 前建议执行 `make amp-clean`，避免复用另一运行时的 AMP 中间产物。

AMP 主要产物：

| 文件 | 用途 |
| --- | --- |
| `work/u-boot-amp-spl.bin.normal.out` | AMP SPL，写入 SPI NOR `0x0` |
| `work/visionfive2_fw_payload_amp.img` | OpenSBI + U-Boot + RTOS，写入 SPI NOR `0x100000` |
| `work/amp/image.fit` | AMP Linux 内核、DTB 和 initramfs |
| `work/amp/amp_rtos.bin` | 本次构建选中的 RTOS 原始固件 |

也可以使用一键脚本完成工具链检查、普通镜像、AMP 镜像和 SD 卡镜像构建：

```bash
# 默认 FreeRTOS
./script/build-rtthread-amp-sdk.sh

# 选择 RT-Thread
AMP_RTOS=rtthread ./script/build-rtthread-amp-sdk.sh
```

> 脚本名称保留了历史命名，但当前同时支持 FreeRTOS 和 RT-Thread。

### SD 卡镜像

```bash
# 普通 Linux
make buildroot_rootfs -j"$(nproc)"
make img

# AMP；AMP_RTOS 默认 freertos
make amp_img AMP_RTOS=freertos -j"$(nproc)"
```

产物分别为 `work/sdcard.img` 和 `work/sdcard_amp.img`。

## 烧录与启动

### AMP SPI NOR 布局

| SPI NOR 偏移 | 镜像 |
| ---: | --- |
| `0x0` | `u-boot-amp-spl.bin.normal.out` |
| `0x100000` | `visionfive2_fw_payload_amp.img` |

通过 TFTP 在 U-Boot 中更新固件的示例：

```bash
setenv ipaddr 192.168.5.9
setenv serverip 192.168.5.11

sf probe
tftpboot ${loadaddr} u-boot-amp-spl.bin.normal.out
sf update ${loadaddr} 0x0 ${filesize}

tftpboot ${loadaddr} visionfive2_fw_payload_amp.img
sf update ${loadaddr} 0x100000 ${filesize}
```

加载 AMP Linux FIT：

```bash
tftpboot ${loadaddr} image.fit
bootm ${loadaddr}
```

完整的拨码、TFTP、NOR 校验、SD 卡启动和 SSH 操作见 [AMP 启动说明](doc/软件文档/2.AMP启动.md)。

> **警告：** `sf erase`、`sf update`、`dd` 和 `make DISK=/dev/... format-boot-loader` 会改写 Flash 或磁盘。执行前必须核对目标设备和镜像，错误的设备名可能导致数据不可恢复。

## AMP 联调

构建 Linux 侧 Mailbox 模块和日志程序并安装到 `nfs_rootfs/`：

```bash
./bsp/ipi_mailbox/build.sh
./app/build.sh
```

板端通过 NFS 挂载该目录后，按依赖顺序加载：

```bash
/mnt/install.sh
```

该脚本依次加载 IPI Mailbox、Router、Remote Console、NOR Client 和 Log 驱动，并启动 `soc_logd`。NFS、SSH、Minicom 和远程 Shell 的配置方法见 [Buildroot 联调说明](doc/软件文档/3.buildroot使用.md)。

## 常用构建命令

```bash
make vmlinux                         # Linux 内核、模块和 DTB
make uboot                           # 普通 U-Boot
make fit                             # 普通 Linux FIT
make ampuboot_fit AMP_RTOS=freertos # AMP SPL 与 fw_payload
make ampfit AMP_RTOS=freertos       # AMP Linux FIT
make buildroot_rootfs                # Buildroot ext4 rootfs
make linux-menuconfig                # Linux 配置
make uboot-menuconfig                # U-Boot 配置
make buildroot_initramfs-menuconfig  # initramfs 配置
make buildroot_rootfs-menuconfig     # rootfs 配置
make amp-clean                       # 仅清理 AMP 产物
make clean                           # 清理主要构建产物
make distclean                       # 删除整个 work/，下次将完整重编
```

## OTA 子系统

OTA 代码当前分为以下部分：

- `basic_middleware/ota_package/`：OTA 包解析、校验和写入流程。
- `basic_middleware/ota_info/`：设备分区、版本与 recovery 状态访问。
- `basic_middleware/ota_client/`：板端查询、下载、解包、安装和状态命令。
- `ota_server/`：发布版本、静态文件服务和管理接口。

入口文档：

- [OTA 总体说明](doc/OTA.md)
- [OTA Client](basic_middleware/ota_client/README.md)
- [OTA Server](ota_server/README.md)
- [A/B 启动实现记录](doc/OTA%20ab分区启动实现记录.md)

OTA 会修改启动分区和 SPI-NOR 状态区，进行真机测试前请先确认分区表、当前 bank 和回滚路径。

## 文档索引

- [启动流程与镜像说明](doc/软件文档/1.启动流程.md)
- [AMP 启动与刷写](doc/软件文档/2.AMP启动.md)
- [Buildroot、NFS、SSH 与 Minicom](doc/软件文档/3.buildroot使用.md)
- [SDK 下载与编译](doc/软件文档/SDK下载与编译说明.md)
- [VisionFive 2 移植方案](doc/方案文档/移植方案-VisionFive2.md)
- [日志收集方案](doc/方案文档/日志收集方案.md)
- [FreeRTOS 启动调试记录](doc/调试文档/1.FreeRTOS%20启动调试记录.md)
- [IPI Mailbox 调试记录](doc/调试文档/2.IPI_Mailbox%20通信调试记录.md)
- [FreeRTOS SSIP / IPI 调试记录](doc/调试文档/3.FreeRTOS-SSIP与IPI-Mailbox调试记录.md)
- [Remote Shell 调试记录](doc/调试文档/4.Remote-Shell回显问题调试记录.md)
- [日志落盘实现记录](doc/日志落盘实现记录.md)

## 当前开发注意事项

- AMP 默认运行时是 FreeRTOS；RT-Thread 通过 `AMP_RTOS=rtthread` 选择。
- `conf/amp_rootfs_post_build.sh` 内含开发环境默认网络：板端 `192.168.5.9`、NFS 主机 `192.168.5.11`。
- NFS 自动挂载默认导出路径为 `/home/xikao/VisionFive2_6.6/nfs_rootfs`，换主机时需要修改。
- 仓库中可能存在本机构建生成的 `.ko`、目标文件和服务端数据；正式发布前应按需清理并确认 `.gitignore`。
- 首次 Buildroot 构建耗时较长；下载失败、子模块分支和磁盘空间问题可查阅 SDK 编译文档。

## 上游基础

本项目的板级 SDK 基于 [StarFive VisionFive2](https://github.com/starfive-tech/VisionFive2) Linux 6.6 开发分支，并保留其原有 Linux、U-Boot、OpenSBI、Buildroot 和多媒体组件。各上游组件的许可证以对应子目录中的 LICENSE / COPYING 文件为准；本项目新增代码的授权方式请以仓库实际声明为准。
