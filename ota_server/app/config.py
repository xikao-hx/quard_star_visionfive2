import os
from dataclasses import dataclass
from pathlib import Path


DEFAULT_CONFIG_PATH = Path(__file__).resolve().parents[1] / "config" / "ota_server.conf"


def _parse_bool(value: str) -> bool:
    return value.strip().lower() in {"1", "true", "yes", "on"}


@dataclass(frozen=True)
class ServerConfig:
    server_host: str
    server_port: int
    database_path: Path
    package_root: Path
    log_dir: Path
    public_base_url: str
    package_url_prefix: str
    api_token: str
    require_https_proxy: bool
    https_proxy_header: str
    config_path: Path


def _resolve_path(raw_value: str, config_dir: Path) -> Path:
    path = Path(raw_value)
    if not path.is_absolute():
        path = (config_dir / path).resolve()
    return path


def load_config(config_path: str | None = None) -> ServerConfig:
    raw_path = config_path or os.environ.get("OTA_SERVER_CONFIG") or str(DEFAULT_CONFIG_PATH)
    path = Path(raw_path).resolve()
    if not path.exists():
        raise FileNotFoundError(f"config file not found: {path}")

    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if "=" not in stripped:
            raise ValueError(f"invalid config line: {line}")
        key, value = stripped.split("=", 1)
        values[key.strip()] = value.strip()

    required_keys = {
        "server_host",
        "server_port",
        "database_path",
        "package_root",
        "log_dir",
        "public_base_url",
        "package_url_prefix",
        "api_token",
        "require_https_proxy",
        "https_proxy_header",
    }
    missing = sorted(required_keys - values.keys())
    if missing:
        raise ValueError(f"missing config keys: {', '.join(missing)}")

    config_dir = path.parent
    package_url_prefix = values["package_url_prefix"].strip()
    if not package_url_prefix.startswith("/"):
        raise ValueError("package_url_prefix must start with '/'")

    return ServerConfig(
        server_host=values["server_host"].strip(),
        server_port=int(values["server_port"]),
        database_path=_resolve_path(values["database_path"], config_dir),
        package_root=_resolve_path(values["package_root"], config_dir),
        log_dir=_resolve_path(values["log_dir"], config_dir),
        public_base_url=values["public_base_url"].rstrip("/"),
        package_url_prefix=package_url_prefix.rstrip("/"),
        api_token=values["api_token"].strip(),
        require_https_proxy=_parse_bool(values["require_https_proxy"]),
        https_proxy_header=values["https_proxy_header"].strip(),
        config_path=path,
    )
