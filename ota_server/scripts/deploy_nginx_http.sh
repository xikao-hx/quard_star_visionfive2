#!/usr/bin/env bash
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)

OTA_ROOT=$REPO_ROOT/ota_server/static_ota
LOG_DIR=$REPO_ROOT/ota_server/logs/quard-ota
LISTEN=18080
SERVER_NAME=_
URL_PREFIX=/ota
API_UPSTREAM=http://127.0.0.1:18081
CONF_PATH=/etc/nginx/conf.d/quard-ota-static.conf
DRY_RUN=0

usage() {
    cat <<'EOF'
Usage: deploy_nginx_http.sh [options]

Options:
  --ota-root <path>       OTA static root, default: ota_server/static_ota
  --log-dir <path>        Nginx OTA log dir, default: ota_server/logs/quard-ota
  --listen <port>         Nginx listen port, default: 18080
  --server-name <name>    Nginx server_name, default: _
  --url-prefix <prefix>   Public URL prefix, default: /ota
  --api-upstream <url>    FastAPI upstream for admin/api, default: http://127.0.0.1:18081
  --conf-path <path>      Nginx config install path
  --dry-run               Print config without writing system files
  -h, --help              Show this help
EOF
}

die() {
    echo "error: $*" >&2
    exit 1
}

need_value() {
    [ "$#" -ge 2 ] || die "$1 requires a value"
    [ -n "$2" ] || die "$1 requires a non-empty value"
}

normalize_prefix() {
    case "$1" in
        /*) printf '%s' "${1%/}" ;;
        *) die "--url-prefix must start with /" ;;
    esac
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --ota-root)
            need_value "$@"
            OTA_ROOT=$2
            shift 2
            ;;
        --log-dir)
            need_value "$@"
            LOG_DIR=$2
            shift 2
            ;;
        --listen)
            need_value "$@"
            LISTEN=$2
            shift 2
            ;;
        --server-name)
            need_value "$@"
            SERVER_NAME=$2
            shift 2
            ;;
        --url-prefix)
            need_value "$@"
            URL_PREFIX=$(normalize_prefix "$2")
            shift 2
            ;;
        --api-upstream)
            need_value "$@"
            API_UPSTREAM=$2
            shift 2
            ;;
        --conf-path)
            need_value "$@"
            CONF_PATH=$2
            shift 2
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown option: $1"
            ;;
    esac
done

case "$LISTEN" in
    ''|*[!0-9]*) die "--listen must be a numeric port" ;;
esac

MANIFESTS_URI=$URL_PREFIX/manifests/
PACKAGES_URI=$URL_PREFIX/packages/

render_config() {
    cat <<EOF
server {
    listen $LISTEN;
    server_name $SERVER_NAME;

    access_log $LOG_DIR/nginx-access.log;
    error_log $LOG_DIR/nginx-error.log;

    location = $URL_PREFIX {
        return 301 $URL_PREFIX/;
    }

    location $MANIFESTS_URI {
        alias $OTA_ROOT/manifests/;
        autoindex off;
        add_header Cache-Control "no-store" always;
        add_header X-Content-Type-Options "nosniff" always;
        types { application/json json; }
        default_type application/json;
    }

    location $PACKAGES_URI {
        alias $OTA_ROOT/packages/;
        autoindex off;
        add_header Accept-Ranges bytes always;
        add_header Cache-Control "public, max-age=300" always;
        default_type application/octet-stream;
    }

    location = / {
        return 302 /admin;
    }

    location /admin {
        proxy_pass $API_UPSTREAM;
        proxy_set_header Host \$host;
        proxy_set_header X-Forwarded-For \$proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto \$scheme;
        proxy_http_version 1.1;
    }

    location /api/ {
        proxy_pass $API_UPSTREAM;
        proxy_set_header Host \$host;
        proxy_set_header X-Forwarded-For \$proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto \$scheme;
        proxy_http_version 1.1;
    }

    location /static/ {
        proxy_pass $API_UPSTREAM;
        proxy_set_header Host \$host;
        proxy_set_header X-Forwarded-For \$proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto \$scheme;
        proxy_http_version 1.1;
    }

    location /packages/ {
        proxy_pass $API_UPSTREAM;
        proxy_set_header Host \$host;
        proxy_set_header X-Forwarded-For \$proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto \$scheme;
        proxy_http_version 1.1;
    }
}
EOF
}

if [ "$DRY_RUN" -eq 1 ]; then
    echo "# dry-run: nginx config would be written to $CONF_PATH"
    echo "# dry-run: ota root directories: $OTA_ROOT/manifests $OTA_ROOT/packages"
    echo "# dry-run: log directory: $LOG_DIR"
    echo "# dry-run: api upstream: $API_UPSTREAM"
    render_config
    exit 0
fi

[ "$(id -u)" -eq 0 ] || die "install mode needs root; rerun with sudo or use --dry-run"
command -v nginx >/dev/null 2>&1 || die "nginx not found; install nginx first"

mkdir -p "$OTA_ROOT/manifests" "$OTA_ROOT/packages" "$LOG_DIR" "$(dirname "$CONF_PATH")"
tmp_conf=$CONF_PATH.tmp.$$
render_config > "$tmp_conf"
mv "$tmp_conf" "$CONF_PATH"

nginx -t

echo "nginx static OTA config installed: $CONF_PATH"
echo "ota root: $OTA_ROOT"
echo "logs: $LOG_DIR/nginx-access.log $LOG_DIR/nginx-error.log"
