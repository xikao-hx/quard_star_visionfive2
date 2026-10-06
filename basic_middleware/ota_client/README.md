# OTA Client

本目录预留给设备侧远程 OTA 客户端核心模块，定位是“远程 OTA 编排入口”，不是替代已有本地 OTA 基础库。

职责边界：

- 负责设备身份采集、版本读取、远程 OTA 请求/响应数据整理
- 负责串联已有 `ota_info`、`ota_package` 等本地升级能力
- 负责未来真机场景下的直连 OTA 服务器接口预留

允许扩展的现有依赖：

- `basic_middleware/ota_info`
- `basic_middleware/ota_package`
- `target_root_script/` 下的启动脚本和配置落盘入口

禁止放入本目录的内容：

- 宿主机 SSH/SCP 编排逻辑
- 服务端 API、数据库和发布目录管理逻辑
- BL1/U-Boot 中与远程网络交互无关的启动期代码

实现约束：

- 远程 OTA 客户端只做用户态编排，不直接改写 BL1/U-Boot 职责
- recovery 状态推进仍复用现有设备侧 OTA 执行链路

代码框架：

- `ota_client.sh`：唯一公开入口，只负责按业务域 `exec` 分发命令
- `libexec/ota-client/ota-device`：`identity`、`config`、`state` 查询进程和实现
- `libexec/ota-client/ota-remote`：`register`、`check`、`download` 远程进程和实现
- `libexec/ota-client/ota-package`：`unpack`、`install`、`service_start` 包编排进程和实现
- `lib/ota/command.sh`：远程命令和包命令共用的命令生命周期
- `lib/ota/api.sh`、`config.sh`、`state.sh`、`network.sh`、`locking.sh`、`logging.sh`、`utils.sh`：单一职责基础能力

三个内部入口是独立进程，包含各自业务实现，只声明并加载本业务域需要的变量和公共基础能力。远程请求状态不会进入包安装进程，包编排状态也不会污染设备查询进程。

命令：

  - `identity`：输出 `device_id`、`board_type`、`hardware_version`、`mac`、`current_version`
  - `config`：输出远程 OTA 配置读取结果
  - `register`：向服务器发起设备登记
  - `check`：向服务器发起升级检查
  - `download`：下载 OTA 压缩包、校验 SHA-256，并带重试清理
  - `unpack`：解压压缩包为固定 `ota_package/` 目录并校验关键文件
  - `install`：按 `verify_stage1 -> apply_stage1 -> commit_stage1` 顺序触发本地 OTA
  - `service_start`：按 OTA 状态幂等恢复 stage1/stage2，在 bank、rootfs、
    PARTUUID、版本和健康钩子检查通过后执行 stage2 或 `boot_success`
  - `state`：输出当前缓存的发布元数据

- `build.sh`
  - 默认安装到 `nfs_rootfs`，也可通过 `OTA_ROOTFS_DIR` 指定目标 rootfs
  - 安装 `ota_client` 到目标 rootfs 的 `/bin/ota_client`
  - 安装内部命令到目标 rootfs 的 `/usr/libexec/ota-client/`
  - 安装模块到目标 rootfs 的 `/lib/ota/`
  - 安装 `/etc/init.d/S95ota-stage2.sh`，在主要 SysV 服务启动后触发自动恢复

可观测性约定：

- 默认日志统一采用 `stage=<阶段> event=<事件>` 结构，优先输出阶段开始、进度、成功和失败
- `ota_client` 不重写 `ota_package` / `ota_info` 原始输出
- `ota_package`、`ota_info` 以及启动脚本的日志等级由各自实现通过 `BH_OTA_LOG_LEVEL` 控制
- `download`、`unpack`、`install`、`service_start` 会创建最小互斥锁，重复触发时明确输出 `reason=already_running`
- 包目录相关命令共用 `package` 锁，init 自动恢复与手工安装不能并发执行
- 运行期会把阶段耗时基线追加到 `download_dir/ota_metrics.log`

配套配置文件：

- `target_root_script/etc/ota/device_identity.conf`
- `target_root_script/etc/ota/client.conf`
- `target_root_script/etc/ota/keys/ota_public.pem`

持久 rootfs 由 `conf/amp_rootfs_post_build.sh` 安装上述配置、客户端脚本、
`ota_info`、`ota_package` 和运行库。设备身份优先读取 EEPROM，仅在字段缺失时
使用 `device_identity.conf` 补齐；镜像不得预置多台设备共用的唯一 `device_id`。

配套运行约定：

- OTA 压缩包下载到 `download_dir`
- 解压目录固定为 `OTA_CLIENT_PACKAGE_DIR`，默认 `/userdata/ota/ota_package`
- 默认公钥路径固定为 `/etc/ota/keys/ota_public.pem`，与 `build.sh` 生成的 rootfs 内置公钥一致
- 解压后的发布元数据会同步写入 `download_dir/current_release.env` 和 `ota_package/.ota_remote_release.env`
- 阶段观测基线会写入 `download_dir/ota_metrics.log`
- `target_root_script/etc/init.d/S95ota-stage2.sh` 会直接调用内部
  `ota-package service_start`；`/etc/fstab` 对 `userdata` 的挂载发生在 SysV
  `rcS` 之前，服务仍会再次确认它不是 rootfs 上的同名目录
- stage2 和 `boot_success` 前必须同时满足：recovery 的 current/target bank
  一致、内核 `active_bank` 一致、根挂载设备属于对应 `rootfs_a/b`、内核
  `root=PARTUUID` 与实际设备一致、`/etc/version` 与 manifest/发布状态一致
- 迁移设备若缺少 rootfs GPT PARTLABEL，只允许通过实际根块设备的 sysfs
  分区号按 A=p4、B=p6 回退；PARTLABEL 存在但错误时拒绝回退
- `health_check_dir` 默认是 `/etc/ota/health-check.d`；其中所有可执行文件均
  作为只读关键服务健康检查钩子运行，任一非零退出都会阻止 OTA 状态推进
- stage1 或 stage2 提交成功后，自动恢复服务调用 `reboot -f` 切换到 recovery
  指定 bank；`complete(17)` 只保留状态并停止推进，成功包清理由后续治理步骤处理
