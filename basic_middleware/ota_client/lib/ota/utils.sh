trim_value()
{
    printf '%s' "$1" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//'
}

read_kv()
{
    file=$1
    key=$2

    if [ ! -f "$file" ]; then
        error "config file missing: $file"
        return 1
    fi

    awk -F= -v wanted="$key" '
        /^[[:space:]]*#/ { next }
        $0 ~ "^[[:space:]]*" wanted "[[:space:]]*=" {
            sub(/^[^=]*=/, "", $0)
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", $0)
            print
            exit
        }
    ' "$file"
}

require_value()
{
    name=$1
    value=$2
    if [ -z "$value" ]; then
        error "config invalid: missing $name"
        return 1
    fi
}

validate_uint()
{
    value=$1
    case "$value" in
        ''|*[!0-9]*)
            return 1
            ;;
        *)
            return 0
            ;;
    esac
}

is_truthy()
{
    case "$1" in
        1|true|TRUE|yes|YES|on|ON)
            return 0
            ;;
    esac
    return 1
}

should_print_check_response()
{
    is_truthy "$PRINT_CHECK_RESPONSE"
}

normalize_positive_uint()
{
    value=$1
    fallback=$2

    if ! validate_uint "$value"; then
        printf '%s' "$fallback"
        return 0
    fi

    if [ "$value" -eq 0 ]; then
        printf '%s' "$fallback"
        return 0
    fi

    printf '%s' "$value"
}

json_escape()
{
    printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

extract_json_string()
{
    key=$1
    file=$2
    sed -n "s/.*\"$key\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" "$file" | sed -n '1p'
}

extract_json_bool()
{
    key=$1
    file=$2
    sed -n "s/.*\"$key\"[[:space:]]*:[[:space:]]*\(true\|false\).*/\1/p" "$file" | sed -n '1p'
}

extract_json_number()
{
    key=$1
    file=$2
    sed -n "s/.*\"$key\"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p" "$file" | sed -n '1p'
}

extract_json_payload_files()
{
    file=$1
    grep -o '"file"[[:space:]]*:[[:space:]]*"[^"]*"' "$file" | \
        sed 's/.*"file"[[:space:]]*:[[:space:]]*"\([^"]*\)"/\1/'
}

strip_file_basename()
{
    file_name=$1
    file_name=${file_name##*/}
    printf '%s' "$file_name"
}
