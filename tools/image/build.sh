#!/usr/bin/env bash

set -euo pipefail

usage()
{
    cat <<'EOF'
Usage: ./tools/image/build.sh TARGET

Generate and package VisionFive 2 OTA A/B images.

Available image targets:
  gpt       Generate work/target/amp/gpt.img and gpt-layout.txt
  recovery  Generate work/target/amp/recovery.img and nor-layout.txt
  all       Generate all images and layout reports

Host tests belong under autotest/ and are not run by this script.
EOF
}

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "$script_dir/../.." && pwd)
target_dir="$repo_dir/work/target/amp"
python_bin=${PYTHON3:-python3}
layout_config="$repo_dir/conf/visionfive2-nor-layout.json"

generate_nor_report()
{
    local -a report_args=(
        --config "$layout_config"
        --report "$target_dir/nor-layout.txt"
    )
    local assignment
    local image_path

    # Add images which currently exist in the unified publish directory.
    # nor_layout.py also checks that each image fits its configured partition.
    for assignment in \
        "spl_a=u-boot-amp-spl.bin.normal.out" \
        "gpt=gpt.img" \
        "recovery=recovery.img" \
        "fw_payload_a=visionfive2_fw_payload_amp.img"
    do
        image_path="$target_dir/${assignment#*=}"
        if [[ -f "$image_path" ]]; then
            report_args+=(--image "${assignment%%=*}=$image_path")
        fi
    done

    "$python_bin" "$script_dir/nor_layout.py" "${report_args[@]}"
}

generate_gpt()
{
    mkdir -p "$target_dir"
    "$python_bin" "$script_dir/gpt_image.py" \
        --config "$layout_config" \
        --generate "$target_dir/gpt.img" \
        --report "$target_dir/gpt-layout.txt"
}

generate_recovery()
{
    mkdir -p "$target_dir"
    "$python_bin" "$script_dir/recovery_image.py" \
        --config "$layout_config" \
        --generate "$target_dir/recovery.img"
}

case "${1:-help}" in
    -h|--help|help)
        usage
        ;;
    gpt)
        generate_gpt
        generate_nor_report
        ;;
    recovery)
        generate_recovery
        generate_nor_report
        ;;
    all)
        generate_gpt
        generate_recovery
        generate_nor_report
        ;;
    *)
        printf 'Unknown image target: %s\n\n' "$1" >&2
        usage >&2
        exit 2
        ;;
esac
