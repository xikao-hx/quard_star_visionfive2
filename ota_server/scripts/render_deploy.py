#!/usr/bin/env python3
import argparse
from pathlib import Path
import sys

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from ota_server.app.config import load_config
from ota_server.app.deploy import render_nginx_config, render_systemd_service


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Render nginx/systemd deploy sample files")
    parser.add_argument("--config", default=None, help="ota server config path")
    parser.add_argument("--output-dir", default="ota_server/deploy/generated", help="deploy output dir")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo_root = REPO_ROOT
    config = load_config(args.config)
    output_dir = (repo_root / args.output_dir).resolve()
    nginx_dir = output_dir / "nginx"
    systemd_dir = output_dir / "systemd"
    nginx_dir.mkdir(parents=True, exist_ok=True)
    systemd_dir.mkdir(parents=True, exist_ok=True)

    (nginx_dir / "ota.conf").write_text(render_nginx_config(config), encoding="utf-8")
    (systemd_dir / "ota-server.service").write_text(
        render_systemd_service(config, repo_root),
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
