#!/bin/sh

set -eu

MTD_BY_NAME_DIR=${MTD_BY_NAME_DIR:-/dev/mtd/by-name}
OTA_INFO_BIN=${OTA_INFO_BIN:-ota_info}

if [ ! -r /proc/mtd ]; then
    echo "Error: /proc/mtd is not available" >&2
    exit 1
fi

mkdir -p "${MTD_BY_NAME_DIR}"

# Create the by-name links required by ota_info from the registered MTD list.
while IFS= read -r line; do
    case "${line}" in
        mtd[0-9]*:*)
            mtd_dev=${line%%:*}
            part_name=$(printf '%s\n' "${line}" |
                sed -n 's/^[^\"]*"\([^\"]*\)".*$/\1/p')
            if [ -n "${part_name}" ] && [ -e "/dev/${mtd_dev}" ]; then
                ln -sf "../../${mtd_dev}" "${MTD_BY_NAME_DIR}/${part_name}"
            fi
            ;;
    esac
done < /proc/mtd

for required_part in spl_a spl_b gpt recovery fw_payload_a fw_payload_b; do
    if [ ! -e "${MTD_BY_NAME_DIR}/${required_part}" ]; then
        echo "Error: MTD partition '${required_part}' is unavailable" >&2
        exit 1
    fi
done

if ! command -v "${OTA_INFO_BIN}" >/dev/null 2>&1; then
    echo "Error: ota_info command not found: ${OTA_INFO_BIN}" >&2
    exit 1
fi

output=$("${OTA_INFO_BIN}" read current_bank)
active_bank=$(printf '%s\n' "${output}" |
    sed -n 's/.*current bank:[[:space:]]*\([aAbB]\).*/\1/p' |
    tail -n 1 |
    tr '[:upper:]' '[:lower:]')

case "${active_bank}" in
    a)
        inactive_bank=b
        ;;
    b)
        inactive_bank=a
        ;;
    *)
        echo "Error: failed to read current bank from ota_info output" >&2
        printf '%s\n' "${output}" >&2
        exit 1
        ;;
esac

for part in spl fw_payload; do
    ln -sf "${part}_${active_bank}" "${MTD_BY_NAME_DIR}/${part}_active"
    ln -sf "${part}_${inactive_bank}" "${MTD_BY_NAME_DIR}/${part}_inactive"
done

echo "Current active bank: ${active_bank}"
echo "MTD links created in ${MTD_BY_NAME_DIR}"
