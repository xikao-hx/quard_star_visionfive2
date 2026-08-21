#!/usr/bin/env bash

set -euo pipefail

usage()
{
    cat <<'EOF'
Usage: ./tools/image/build.sh TARGET

Generate and package VisionFive 2 OTA A/B images.

Available image targets:
  gpt       Generate work/gpt.img

Future image targets:
  Step 4: recovery
  Step 5: factory / all

Host tests belong under autotest/ and are not run by this script.
EOF
}

case "${1:-help}" in
    -h|--help|help)
        usage
        ;;
    gpt)
        script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
        repo_dir=$(cd -- "$script_dir/../.." && pwd)
        python_bin=${PYTHON3:-python3}
        "$python_bin" "$script_dir/gpt_image.py" \
            --config "$repo_dir/conf/visionfive2-nor-layout.json" \
            --generate "$repo_dir/work/gpt.img"
        ;;
    *)
        printf 'Unknown image target: %s\n\n' "$1" >&2
        usage >&2
        exit 2
        ;;
esac
