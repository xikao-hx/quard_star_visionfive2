# OTA Server

本目录承载远程 OTA 服务器最小闭环实现，技术栈与 `memory-bank/tech-stack.md` 保持一致：

- API：`FastAPI`
- 数据存储：`SQLite`
- 静态包发布：`Nginx HTTP` 暴露 manifest 和 OTA 包

当前目录结构：

- `app/`
  - `main.py`：服务入口与路由
  - `config.py`：`key=value` 配置加载
  - `storage.py`：`sqlite3` 表结构与持久化
  - `service.py`：版本匹配、`release_id` 规则等业务逻辑
  - `schemas.py`：接口请求响应模型
- `config/ota_server.conf`
  - 服务器默认配置样例
- `scripts/run_server.sh`
  - 本地启动脚本
- `scripts/deploy_nginx_http.sh`
  - 生成 Nginx HTTP 静态发布配置，并把 `/admin`、`/api` 代理到 FastAPI
- `scripts/publish_static_release.sh`
  - 发布 OTA 包到 Nginx 静态目录并原子更新 manifest
- `tests/`
  - 服务端最小单测
- `requirements.txt`
  - Python 依赖清单
- `deploy/`
  - 单机部署打样文件与说明

职责边界：

- 只负责 `register`、`check`、`report` 三类服务端接口
- 只负责设备元数据、发布记录、升级上报记录的持久化
- 只负责服务器配置加载、版本匹配和最小鉴权
- Nginx 只负责静态下载、Range、缓存策略和日志
- 静态发布脚本只负责包校验、打包、`release.json` 和 manifest 更新

禁止放入本目录的内容：

- QEMU 设备侧 OTA 执行逻辑
- 宿主机下载、解压、传包、SSH 编排逻辑
- 设备侧 `ota_package`、`ota_info` 本地升级基础库代码

## 配置说明

默认配置文件位置：`ota_server/config/ota_server.conf`

最小配置项：

- `server_host`
- `server_port`
- `database_path`
- `package_root`
- `log_dir`
- `public_base_url`
- `package_url_prefix`
- `api_token`
- `require_https_proxy`
- `https_proxy_header`

说明：

- `database_path`、`package_root`、`log_dir` 支持相对路径，相对配置文件目录解析
- `require_https_proxy=1` 时，接口要求请求带上 `https_proxy_header` 指定的值，默认 `X-Forwarded-Proto: https`
- 发布包目录约定为 `package_root/<release_id>/<release_id>.tar.gz`
- `release_id` 固定规则：`日期 + 板型 + 序号`，示例 `20260529-quard-star-r1`

## 本地运行

```bash
python3 -m pip install --user -r ota_server/requirements.txt
OTA_SERVER_CONFIG=$PWD/ota_server/config/ota_server.conf python3 -m ota_server.app.main
```

或直接执行。默认 FastAPI 后台监听 `18081`，Nginx 对外监听 `18080`：

```bash
./ota_server/scripts/run_server.sh
```

启动后可直接打开网页后台：

- 通过 Nginx 统一入口：首页 `http://127.0.0.1:18080/admin`
- 直接访问 FastAPI：首页 `http://127.0.0.1:18081/admin`
- 发布页：`/admin/releases`
- 设备页：`/admin/devices`
- 升级记录页：`/admin/reports`

网页后台提供：

- 概览：看设备数、发布数、升级记录数
- 发布管理：浏览器上传 OTA 压缩包并发布
- 设备列表：查看设备登记信息
- 升级记录：查看成功/失败和原因

## FastAPI 发布 OTA 包

发布脚本输入现有 `ota_package/` 目录，输出固定压缩包和审计文件：

```bash
python3 ota_server/scripts/publish_release.py \
  --config ota_server/config/ota_server.conf \
  --source-dir output/ota_package \
  --release-id 20260601-quard-star-r1 \
  --board-type quard-star \
  --release-note "demo release"
```

发布结果：

- 压缩包：`package_root/<release_id>/<release_id>.tar.gz`
- 审计文件：`package_root/<release_id>/release.json`
- 数据库记录：`releases.release_id = <release_id>`

发布前会校验：

- `data.json`
- `manifest.hash`
- `signature.sig`
- `data.json.payloads[*].file` 对应的所有镜像文件

## Nginx HTTP 静态发布

场景说明：生成 Nginx HTTP 静态发布配置，不写入系统目录。

```bash
cd /home/xikao/quard_star_tutorial
bash ota_server/scripts/deploy_nginx_http.sh --dry-run
```

预期日志关键字：

- `server_name`
- `/ota/manifests/`
- `/ota/packages/`
- `/admin`
- `/api/`
- `Accept-Ranges`
- `Cache-Control "no-store"`

场景说明：将已有 OTA 包目录发布到本地静态目录，并生成 manifest。

```bash
cd /home/xikao/quard_star_tutorial
bash ota_server/scripts/publish_static_release.sh \
  --source-dir output/ota_package \
  --release-id 20260629-quard-star-r1 \
  --board-type quard-star \
  --public-base-url http://127.0.0.1:18080
```

预期日志关键字：

- `publish package`
- `sha256`
- `release.json`
- `manifest updated`

静态发布目录结构：

```text
<ota-root>/
  manifests/
    quard-star.json
  packages/
    20260629-quard-star-r1/
      20260629-quard-star-r1.tar.gz
      release.json
```

板端只需要固定 manifest URL，例如：

```text
manifest_url=http://192.168.31.2:18080/ota/manifests/quard-star.json
```

`ota_client check/download` 会优先读取 `manifest_url`；如果 manifest 流程失败且 `server_url` 已配置，会回落到旧 FastAPI `/api/v1/ota/check` 流程。

## 单机部署打样

生成 `nginx` 和 `systemd` 样板：

```bash
python3 ota_server/scripts/render_deploy.py \
  --config ota_server/config/ota_server.conf
```

生成结果：

- `ota_server/deploy/generated/nginx/ota.conf`
- `ota_server/deploy/generated/systemd/ota-server.service`
