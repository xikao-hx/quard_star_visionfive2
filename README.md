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
make ampuboot_fit RTOS=freertos \
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

**说明：** 官方 SDK 的增量编译存在一些问题，可能修改文件后，并不会真的重新编译，需要执行 touch xxx 来修改一下文件夹的时间，来触发重新编译

### 普通 Linux 镜像

```bash
make -j"$(nproc)"
```

主要产物：

```text

```

### AMP 镜像（默认 FreeRTOS）

**官方 SDK 镜像产物参考：** [镜像信息](doc/使用手册/1.镜像信息.md)

```bash
# 只构建并发布普通镜像到 work/target/normal
./build.sh sdk build normal

# 只构建并发布 AMP 镜像到 work/target/amp
./build.sh sdk build amp

# 同时构建并发布两组镜像（默认）
./build.sh sdk build all

# 拷贝到 tftp 指定目录
sudo cp work/target/amp/u-boot-amp-spl.bin.normal.out \
        work/target/amp/visionfive2_fw_payload_amp.img \
        work/target/amp/image.fit \
        work/target/amp/starfive-visionfive2-vfat.part \
        work/target/amp/gpt.img \
        work/target/amp/recovery.img /srv/tftp/
```

### SD 卡镜像

```bash
# 普通镜像
make img
# work/sdcard.img

# AMP镜像，默认 FreeRTOS
make amp_img RTOS=freertos -j"$(nproc)"
# work/sdcard_amp.img
```

构建并发布镜像到独立目录：

```bash
# work/target/normal/
make publish_normal_images -j"$(nproc)"

# work/target/amp/；RTOS 可选择 freertos 或 rtthread
make publish_amp_images RTOS=freertos -j"$(nproc)"

# 同时发布普通和 AMP 镜像
make publish_all_images RTOS=freertos -j"$(nproc)"
```

AMP 发布目标生成并复制 SPL、fw_payload、FIT、FAT boot 分区和 rootfs
产物；普通发布目标不包含 AMP FAT 分区镜像。发布目标不生成或发布
`sdcard.img`、`sdcard_amp.img`。需要 SD 卡整盘镜像时请显式执行上面的
`make img` 或 `make amp_img`。

## 烧录与启动

**参考如下文档：** [刷写命令](doc/使用手册/2.刷写命令.md)

## AMP 联调

构建 Linux 侧 Mailbox 模块和日志程序并安装到 `nfs_rootfs/`：

```bash
./build.sh bsp build
./build.sh app build
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
make ampuboot_fit RTOS=freertos # AMP SPL 与 fw_payload
make ampfit RTOS=freertos       # AMP Linux FIT
make buildroot_rootfs                # Buildroot ext4 rootfs
make linux-menuconfig                # Linux 配置
make uboot-menuconfig                # U-Boot 配置
make buildroot_initramfs-menuconfig  # initramfs 配置
make buildroot_rootfs-menuconfig     # rootfs 配置
make amp-clean                       # 仅清理 AMP 产物
make clean                           # 清理主要构建产物
make distclean                       # 删除整个 work/，下次将完整重编
```

## 当前开发注意事项

- AMP 默认运行时是 FreeRTOS；RT-Thread 通过 `RTOS=rtthread` 选择。
- `conf/amp_rootfs_post_build.sh` 内含开发环境默认网络：板端 `192.168.5.9`、NFS 主机 `192.168.5.11`。
- NFS 自动挂载默认导出路径为 `/home/xikao/VisionFive2_6.6/nfs_rootfs`，换主机时需要修改。
- 仓库中可能存在本机构建生成的 `.ko`、目标文件和服务端数据；正式发布前应按需清理并确认 `.gitignore`。
- 首次 Buildroot 构建耗时较长；下载失败、子模块分支和磁盘空间问题可查阅 SDK 编译文档。

## 上游基础

本项目的板级 SDK 基于 [StarFive VisionFive2](https://github.com/starfive-tech/VisionFive2) Linux 6.6 开发分支，并保留其原有 Linux、U-Boot、OpenSBI、Buildroot 和多媒体组件。各上游组件的许可证以对应子目录中的 LICENSE / COPYING 文件为准；本项目新增代码的授权方式请以仓库实际声明为准。
