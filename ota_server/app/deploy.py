from pathlib import Path
from urllib.parse import urlparse

from .config import ServerConfig


def render_nginx_config(config: ServerConfig) -> str:
    public_host = urlparse(config.public_base_url).hostname or "192.168.31.2"
    return f"""server {{
    listen 443 ssl http2;
    server_name {public_host};

    ssl_certificate /etc/nginx/certs/ota.crt;
    ssl_certificate_key /etc/nginx/certs/ota.key;

    access_log {config.log_dir}/nginx-access.log;
    error_log {config.log_dir}/nginx-error.log;

    client_max_body_size 2g;

    location {config.package_url_prefix}/ {{
        alias {config.package_root}/;
        autoindex off;
        add_header Cache-Control "public, max-age=300";
    }}

    location /api/ {{
        proxy_pass http://{config.server_host}:{config.server_port};
        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto https;
        proxy_http_version 1.1;
    }}
}}
"""


def render_systemd_service(config: ServerConfig, repo_root: Path) -> str:
    return f"""[Unit]
Description=OTA Server
After=network.target

[Service]
Type=simple
WorkingDirectory={repo_root}
Environment=OTA_SERVER_CONFIG={config.config_path}
ExecStart=/usr/bin/python3 -m ota_server.app.main
Restart=always
RestartSec=2
StandardOutput=append:{config.log_dir}/ota-server.log
StandardError=append:{config.log_dir}/ota-server-error.log

[Install]
WantedBy=multi-user.target
"""
