#!/bin/sh

ota_read_sys_version()
{
    version_file=$1

    if [ ! -r "$version_file" ]; then
        printf 'OTA system version file is missing: %s\n' "$version_file" >&2
        return 1
    fi

    version=$(sed -n '1p' "$version_file")
    if ! printf '%s\n' "$version" | awk '
        BEGIN { valid = 0 }
        /^[0-9]+\.[0-9]+\.[0-9]+$/ { valid = 1 }
        END { exit valid ? 0 : 1 }
    '; then
        printf 'OTA system version must use MAJOR.MINOR.PATCH: %s\n' \
            "$version" >&2
        return 1
    fi

    if [ -n "$(sed -n '2,$p' "$version_file" | sed -n '/[^[:space:]]/p')" ]; then
        printf 'OTA system version file must contain exactly one value: %s\n' \
            "$version_file" >&2
        return 1
    fi

    printf '%s\n' "$version"
}
