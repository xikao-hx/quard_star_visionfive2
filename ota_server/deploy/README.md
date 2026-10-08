# OTA Server Deployment Sample

本目录承载 OTA Server 部署打样文件，当前包含两类入口：

- FastAPI 管理后台反向代理样板。
- Nginx HTTP 静态 OTA 发布样板。

## Nginx HTTP 静态发布

`ota_server/scripts/deploy_nginx_http.sh` 只负责生成 Nginx 静态文件服务配置和初始化目录，不发布 OTA 包，不启动 FastAPI。
同一个 Nginx server 还会把 `/admin`、`/api`、`/static`、`/packages` 代理到 FastAPI，默认 upstream 是 `http://127.0.0.1:18081`。

默认目录：

- 发布根目录：`ota_server/static_ota`
- manifest：`ota_server/static_ota/manifests`
- OTA 包：`ota_server/static_ota/packages`
- 日志目录：`ota_server/logs/quard-ota`
- URL 前缀：`/ota`
- 监听端口：`18080`
- FastAPI 后台端口：`18081`

场景说明：生成 Nginx HTTP 配置并检查配置内容，不实际写入系统目录。

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

场景说明：以 root 安装 Nginx HTTP 静态发布配置，并执行 `nginx -t`。

```bash
cd /home/xikao/quard_star_tutorial
sudo bash ota_server/scripts/deploy_nginx_http.sh \
  --listen 18080 \
  --server-name _ \
  --api-upstream http://127.0.0.1:18081
```

预期日志关键字：

- `nginx: configuration file`
- `test is successful`
- `nginx static OTA config installed`

静态 URL 约定：

- `http://<server>:18080/ota/manifests/<board_type>.json`
- `http://<server>:18080/ota/packages/<release_id>/<release_id>.tar.gz`

`/ota/manifests/` 使用 `Cache-Control "no-store"`，避免板端拿到旧 manifest；`/ota/packages/` 显式输出 `Accept-Ranges`，用于 OTA 包断点续传和 Range 验证。

场景说明：发布已有 OTA 包目录到本地静态发布目录。

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

发布脚本也支持 `--archive <path>`，要求压缩包内包含顶层 `ota_package/` 目录。发布流程会先生成包和 `release.json`，最后原子替换 `manifests/<board_type>.json`。

板端配置入口：

```text
manifest_url=http://<server>:18080/ota/manifests/quard-star.json
```

`server_url` 可继续保留，用于旧 API fallback 和升级阶段上报。

网页后台入口：

- `http://<server>:18080/admin`
- `http://<server>:18080/admin/releases`
- `http://<server>:18080/admin/devices`
- `http://<server>:18080/admin/reports`

后台进程启动命令：

```bash
cd /home/xikao/quard_star_tutorial
./ota_server/scripts/run_server.sh
```

## FastAPI 管理后台样板

原有部署样板目标是固定：

- `Nginx` 统一 HTTPS 入口
- `FastAPI + SQLite` 应用路径
- `systemd` 启停方式
- 日志和故障排查入口

生成部署样板：

```bash
cd /home/xikao/quard_star_tutorial
python3 ota_server/scripts/render_deploy.py --config ota_server/config/ota_server.conf
```

生成后会得到：

- `ota_server/deploy/generated/nginx/ota.conf`
- `ota_server/deploy/generated/systemd/ota-server.service`

网页后台入口：

- `https://<server>/admin`
- `https://<server>/admin/releases`
- `https://<server>/admin/devices`
- `https://<server>/admin/reports`

最小排查入口：

- `journalctl -u ota-server -n 100 --no-pager`
- `tail -n 100 ota_server/logs/ota-server.log`
- `tail -n 100 ota_server/logs/nginx-error.log`

说明：

- `Nginx` 直接映射 `package_root`
- `FastAPI` 只监听本地 HTTP
- 统一 HTTPS 域名由 `Nginx` 暴露
