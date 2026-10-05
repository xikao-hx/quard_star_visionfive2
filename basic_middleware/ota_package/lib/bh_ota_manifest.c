#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bh_ota_log.h"
#include "bh_ota_package.h"
#include "jsmn.h"

#define BH_OTA_MANIFEST_MAX_FILE_SIZE 65536U
#define BH_OTA_MANIFEST_MAX_TOKENS 256U

#define MANIFEST_LOG_ERROR(fmt, ...) BH_OTA_LOG_ERROR(fmt, ##__VA_ARGS__)

static int manifest_skip_token(const jsmntok_t *tokens, int index)
{
    int end;
    int i;

    if (index < 0) {
        return -1;
    }

    end = index + 1;
    if (tokens[index].type == JSMN_OBJECT) {
        for (i = 0; i < tokens[index].size; ++i) {
            end = manifest_skip_token(tokens, end);
            end = manifest_skip_token(tokens, end);
        }
        return end;
    }

    if (tokens[index].type == JSMN_ARRAY) {
        for (i = 0; i < tokens[index].size; ++i) {
            end = manifest_skip_token(tokens, end);
        }
        return end;
    }

    return end;
}

static int manifest_token_streq(const char *json,
                                const jsmntok_t *token,
                                const char *literal)
{
    size_t token_len;
    size_t literal_len;

    if (token->type != JSMN_STRING) {
        return 0;
    }

    token_len = (size_t)(token->end - token->start);
    literal_len = strlen(literal);

    return token_len == literal_len &&
           strncmp(json + token->start, literal, token_len) == 0;
}

static int32_t manifest_copy_string(const char *json,
                                    const jsmntok_t *token,
                                    char *dst,
                                    size_t dst_len,
                                    const char *field_name)
{
    size_t token_len;

    if (token->type != JSMN_STRING) {
        MANIFEST_LOG_ERROR("Manifest field '%s' must be a string", field_name);
        return BH_OTA_ERROR;
    }

    token_len = (size_t)(token->end - token->start);
    if (token_len == 0U) {
        MANIFEST_LOG_ERROR("Manifest field '%s' must not be empty", field_name);
        return BH_OTA_ERROR;
    }

    if (token_len >= dst_len) {
        MANIFEST_LOG_ERROR("Manifest field '%s' is too long", field_name);
        return BH_OTA_ERROR;
    }

    memcpy(dst, json + token->start, token_len);
    dst[token_len] = '\0';
    return BH_OTA_OK;
}

static int32_t manifest_copy_sha256_string(const char *json,
                                           const jsmntok_t *token,
                                           char *dst,
                                           size_t dst_len,
                                           const char *field_name)
{
    size_t token_len;
    size_t i;

    if (token->type != JSMN_STRING) {
        MANIFEST_LOG_ERROR("Manifest field '%s' must be a string", field_name);
        return BH_OTA_ERROR;
    }

    token_len = (size_t)(token->end - token->start);
    if (token_len != BH_OTA_SHA256_HEX_LEN || dst_len < (BH_OTA_SHA256_HEX_LEN + 1U)) {
        MANIFEST_LOG_ERROR("Manifest field '%s' must be a %u-character SHA-256 hex string",
                           field_name, BH_OTA_SHA256_HEX_LEN);
        return BH_OTA_ERROR;
    }

    for (i = 0; i < token_len; ++i) {
        char ch = json[token->start + (int)i];
        if (!((ch >= '0' && ch <= '9') ||
              (ch >= 'a' && ch <= 'f') ||
              (ch >= 'A' && ch <= 'F'))) {
            MANIFEST_LOG_ERROR("Manifest field '%s' contains non-hex character", field_name);
            return BH_OTA_ERROR;
        }

        if (ch >= 'A' && ch <= 'F') {
            ch = (char)(ch - 'A' + 'a');
        }
        dst[i] = ch;
    }

    dst[token_len] = '\0';
    return BH_OTA_OK;
}

static int32_t manifest_parse_u32(const char *json,
                                  const jsmntok_t *token,
                                  uint32_t *value,
                                  const char *field_name)
{
    char number_buf[32];
    char *endptr = NULL;
    unsigned long parsed;
    size_t len;

    if (token->type != JSMN_PRIMITIVE) {
        MANIFEST_LOG_ERROR("Manifest field '%s' must be an integer", field_name);
        return BH_OTA_ERROR;
    }

    len = (size_t)(token->end - token->start);
    if (len == 0U || len >= sizeof(number_buf)) {
        MANIFEST_LOG_ERROR("Manifest field '%s' has invalid length", field_name);
        return BH_OTA_ERROR;
    }

    memcpy(number_buf, json + token->start, len);
    number_buf[len] = '\0';

    if (number_buf[0] == '-') {
        MANIFEST_LOG_ERROR("Manifest field '%s' must not be negative", field_name);
        return BH_OTA_ERROR;
    }

    errno = 0;
    parsed = strtoul(number_buf, &endptr, 10);
    if (errno != 0 || endptr == number_buf || *endptr != '\0' || parsed > 0xffffffffUL) {
        MANIFEST_LOG_ERROR("Manifest field '%s' must be a valid uint32 integer", field_name);
        return BH_OTA_ERROR;
    }

    *value = (uint32_t)parsed;
    return BH_OTA_OK;
}

static int manifest_partition_is_block(const char *partition)
{
    return strncmp(partition, "rootfs_", 7) == 0;
}

static int manifest_find_object_value(const char *json,
                                      const jsmntok_t *tokens,
                                      int object_index,
                                      const char *field_name)
{
    int index;
    int i;

    if (tokens[object_index].type != JSMN_OBJECT) {
        return -1;
    }

    index = object_index + 1;
    for (i = 0; i < tokens[object_index].size; ++i) {
        int key_index = index;
        int value_index = index + 1;

        if (manifest_token_streq(json, &tokens[key_index], field_name)) {
            return value_index;
        }

        index = manifest_skip_token(tokens, value_index);
    }

    return -1;
}

static int32_t manifest_parse_payloads(const char *json,
                                       const jsmntok_t *tokens,
                                       int payloads_index,
                                       bh_ota_manifest_t *manifest)
{
    int index;
    int i;
    int j;

    if (tokens[payloads_index].type != JSMN_ARRAY) {
        MANIFEST_LOG_ERROR("Manifest field 'payloads' must be an array");
        return BH_OTA_ERROR;
    }

    if (tokens[payloads_index].size == 0) {
        MANIFEST_LOG_ERROR("Manifest field 'payloads' must not be empty");
        return BH_OTA_ERROR;
    }

    if ((size_t)tokens[payloads_index].size > BH_OTA_MANIFEST_MAX_PAYLOADS) {
        MANIFEST_LOG_ERROR("Manifest payload count exceeds limit %u", BH_OTA_MANIFEST_MAX_PAYLOADS);
        return BH_OTA_ERROR;
    }

    manifest->payload_count = (size_t)tokens[payloads_index].size;
    index = payloads_index + 1;

    for (i = 0; i < tokens[payloads_index].size; ++i) {
        int partition_index;
        int file_index;
        int sha256_index;
        int size_index;
        int stage_index;
        int32_t ret;

        if (tokens[index].type != JSMN_OBJECT) {
            MANIFEST_LOG_ERROR("Manifest payload[%d] must be an object", i);
            return BH_OTA_ERROR;
        }

        partition_index = manifest_find_object_value(json, tokens, index, "partition");
        if (partition_index < 0) {
            MANIFEST_LOG_ERROR("Manifest payload[%d] is missing required field 'partition'", i);
            return BH_OTA_ERROR;
        }

        ret = manifest_copy_string(json,
                                   &tokens[partition_index],
                                   manifest->payloads[i].partition,
                                   sizeof(manifest->payloads[i].partition),
                                   "payloads[].partition");
        if (ret != BH_OTA_OK) {
            return ret;
        }

        file_index = manifest_find_object_value(json, tokens, index, "file");
        if (file_index < 0) {
            MANIFEST_LOG_ERROR("Manifest payload[%d] is missing required field 'file'", i);
            return BH_OTA_ERROR;
        }

        ret = manifest_copy_string(json,
                                   &tokens[file_index],
                                   manifest->payloads[i].file,
                                   sizeof(manifest->payloads[i].file),
                                   "payloads[].file");
        if (ret != BH_OTA_OK) {
            return ret;
        }

        sha256_index = manifest_find_object_value(json, tokens, index, "sha256");
        if (sha256_index < 0) {
            MANIFEST_LOG_ERROR("Manifest payload[%d] is missing required field 'sha256'", i);
            return BH_OTA_ERROR;
        }

        ret = manifest_copy_sha256_string(json,
                                          &tokens[sha256_index],
                                          manifest->payloads[i].sha256,
                                          sizeof(manifest->payloads[i].sha256),
                                          "payloads[].sha256");
        if (ret != BH_OTA_OK) {
            return ret;
        }

        size_index = manifest_find_object_value(json, tokens, index, "size");
        if (size_index < 0) {
            MANIFEST_LOG_ERROR("Manifest payload[%d] is missing required field 'size'", i);
            return BH_OTA_ERROR;
        }

        ret = manifest_parse_u32(json,
                                 &tokens[size_index],
                                 &manifest->payloads[i].size,
                                 "payloads[].size");
        if (ret != BH_OTA_OK) {
            return ret;
        }

        manifest->payloads[i].partition_size = manifest->payloads[i].size;

        size_index = manifest_find_object_value(json, tokens, index, "partition_size");
        if (size_index >= 0) {
            ret = manifest_parse_u32(json,
                                     &tokens[size_index],
                                     &manifest->payloads[i].partition_size,
                                     "payloads[].partition_size");
            if (ret != BH_OTA_OK) {
                return ret;
            }
        }

        if (manifest_partition_is_block(manifest->payloads[i].partition)) {
            if (manifest->payloads[i].partition_size < manifest->payloads[i].size) {
                MANIFEST_LOG_ERROR("Manifest payload[%d] partition_size must be >= size for block payload '%s'",
                                   i, manifest->payloads[i].partition);
                return BH_OTA_ERROR;
            }
        } else if (manifest->payloads[i].partition_size != manifest->payloads[i].size) {
            MANIFEST_LOG_ERROR("Manifest payload[%d] partition_size must equal size for non-block payload '%s'",
                               i, manifest->payloads[i].partition);
            return BH_OTA_ERROR;
        }

        stage_index = manifest_find_object_value(json, tokens, index, "stage");
        if (stage_index < 0) {
            MANIFEST_LOG_ERROR("Manifest payload[%d] is missing required field 'stage'", i);
            return BH_OTA_ERROR;
        }

        ret = manifest_parse_u32(json,
                                 &tokens[stage_index],
                                 &manifest->payloads[i].stage,
                                 "payloads[].stage");
        if (ret != BH_OTA_OK) {
            return ret;
        }

        if (manifest->payloads[i].stage != 1U && manifest->payloads[i].stage != 2U) {
            MANIFEST_LOG_ERROR("Manifest payload[%d] has unsupported stage=%u",
                               i, manifest->payloads[i].stage);
            return BH_OTA_ERROR;
        }

        for (j = 0; j < i; ++j) {
            if (strcmp(manifest->payloads[j].partition,
                       manifest->payloads[i].partition) == 0) {
                MANIFEST_LOG_ERROR("Manifest contains duplicate partition '%s'",
                                   manifest->payloads[i].partition);
                return BH_OTA_ERROR;
            }
        }

        index = manifest_skip_token(tokens, index);
    }

    return BH_OTA_OK;
}

int32_t bh_ota_manifest_parse(const char *json, bh_ota_manifest_t *manifest)
{
    jsmn_parser parser;
    jsmntok_t tokens[BH_OTA_MANIFEST_MAX_TOKENS];
    int token_count;
    int field_index;
    int32_t ret;

    if (json == NULL || manifest == NULL) {
        MANIFEST_LOG_ERROR("Manifest parse received NULL parameter");
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    memset(manifest, 0, sizeof(*manifest));

    jsmn_init(&parser);
    token_count = jsmn_parse(&parser, json, (unsigned int)strlen(json),
                             tokens, BH_OTA_MANIFEST_MAX_TOKENS);
    if (token_count < 0) {
        MANIFEST_LOG_ERROR("Failed to parse manifest JSON");
        return BH_OTA_ERROR;
    }

    if (token_count == 0 || tokens[0].type != JSMN_OBJECT) {
        MANIFEST_LOG_ERROR("Manifest root must be a JSON object");
        return BH_OTA_ERROR;
    }

    field_index = manifest_find_object_value(json, tokens, 0, "format_version");
    if (field_index < 0) {
        MANIFEST_LOG_ERROR("Manifest is missing required field 'format_version'");
        return BH_OTA_ERROR;
    }
    ret = manifest_parse_u32(json, &tokens[field_index], &manifest->format_version, "format_version");
    if (ret != BH_OTA_OK) {
        return ret;
    }
    if (manifest->format_version != 1U) {
        MANIFEST_LOG_ERROR("Unsupported manifest format_version %u", manifest->format_version);
        return BH_OTA_ERROR;
    }

    field_index = manifest_find_object_value(json, tokens, 0, "sys_version");
    if (field_index < 0) {
        MANIFEST_LOG_ERROR("Manifest is missing required field 'sys_version'");
        return BH_OTA_ERROR;
    }
    ret = manifest_copy_string(json,
                               &tokens[field_index],
                               manifest->sys_version,
                               sizeof(manifest->sys_version),
                               "sys_version");
    if (ret != BH_OTA_OK) {
        return ret;
    }
    ret = bh_ota_version_parse(manifest->sys_version,
                               &manifest->sys_version_code,
                               manifest->sys_version,
                               sizeof(manifest->sys_version));
    if (ret != BH_OTA_OK) {
        MANIFEST_LOG_ERROR("Manifest field 'sys_version' is invalid");
        return ret;
    }

    field_index = manifest_find_object_value(json, tokens, 0, "session_id");
    if (field_index < 0) {
        MANIFEST_LOG_ERROR("Manifest is missing required field 'session_id'");
        return BH_OTA_ERROR;
    }
    ret = manifest_copy_string(json,
                               &tokens[field_index],
                               manifest->session_id,
                               sizeof(manifest->session_id),
                               "session_id");
    if (ret != BH_OTA_OK) {
        return ret;
    }

    field_index = manifest_find_object_value(json, tokens, 0, "rollback_index");
    if (field_index < 0) {
        MANIFEST_LOG_ERROR("Manifest is missing required field 'rollback_index'");
        return BH_OTA_ERROR;
    }
    ret = manifest_parse_u32(json, &tokens[field_index], &manifest->rollback_index, "rollback_index");
    if (ret != BH_OTA_OK) {
        return ret;
    }

    field_index = manifest_find_object_value(json, tokens, 0, "payloads");
    if (field_index < 0) {
        MANIFEST_LOG_ERROR("Manifest is missing required field 'payloads'");
        return BH_OTA_ERROR;
    }

    return manifest_parse_payloads(json, tokens, field_index, manifest);
}

int32_t bh_ota_manifest_parse_file(const char *path, bh_ota_manifest_t *manifest)
{
    char *json_buf;
    ssize_t bytes_read;
    int32_t ret;
    int fd;

    if (path == NULL || manifest == NULL) {
        MANIFEST_LOG_ERROR("Manifest file parse received NULL parameter");
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        MANIFEST_LOG_ERROR("Failed to open manifest file '%s'", path);
        return BH_OTA_ERROR_READ;
    }

    json_buf = (char *)calloc(BH_OTA_MANIFEST_MAX_FILE_SIZE + 1U, 1U);
    if (json_buf == NULL) {
        close(fd);
        MANIFEST_LOG_ERROR("Failed to allocate manifest buffer");
        return BH_OTA_ERROR;
    }

    bytes_read = read(fd, json_buf, BH_OTA_MANIFEST_MAX_FILE_SIZE);
    close(fd);

    if (bytes_read < 0) {
        free(json_buf);
        MANIFEST_LOG_ERROR("Failed to read manifest file '%s'", path);
        return BH_OTA_ERROR_READ;
    }

    if (bytes_read == 0) {
        free(json_buf);
        MANIFEST_LOG_ERROR("Manifest file '%s' is empty", path);
        return BH_OTA_ERROR;
    }

    if ((size_t)bytes_read == BH_OTA_MANIFEST_MAX_FILE_SIZE) {
        free(json_buf);
        MANIFEST_LOG_ERROR("Manifest file '%s' exceeds maximum size %u",
                           path, BH_OTA_MANIFEST_MAX_FILE_SIZE);
        return BH_OTA_ERROR;
    }

    json_buf[bytes_read] = '\0';
    ret = bh_ota_manifest_parse(json_buf, manifest);
    free(json_buf);
    return ret;
}

int32_t bh_ota_manifest_check_version(const bh_ota_manifest_t *manifest,
                                      const char *device_version,
                                      int32_t *comparison_result)
{
    int32_t cmp;
    int32_t ret;

    if (manifest == NULL || device_version == NULL) {
        MANIFEST_LOG_ERROR("Version gate received NULL parameter");
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    ret = bh_ota_version_compare(manifest->sys_version, device_version, &cmp);
    if (ret != BH_OTA_OK) {
        MANIFEST_LOG_ERROR("Failed to compare package version '%s' with device version '%s'",
                           manifest->sys_version, device_version);
        return ret;
    }

    if (comparison_result != NULL) {
        *comparison_result = cmp;
    }

    if (cmp < 0) {
        MANIFEST_LOG_ERROR("Rejecting OTA package because package version '%s' is lower than device version '%s'",
                           manifest->sys_version, device_version);
        return BH_OTA_ERROR;
    }

    if (cmp == 0) {
        BH_OTA_LOG_INFO("OTA version gate passed: package version '%s' matches device version '%s'",
                        manifest->sys_version, device_version);
    } else {
        BH_OTA_LOG_INFO("OTA version gate passed: package version '%s' is newer than device version '%s'",
                        manifest->sys_version, device_version);
    }

    return BH_OTA_OK;
}

int32_t bh_ota_manifest_check_version_file(const bh_ota_manifest_t *manifest,
                                           const char *version_path,
                                           int32_t *comparison_result)
{
    char device_version[BH_OTA_SYS_VERSION_MAX_LEN];
    uint32_t device_version_code;
    int32_t ret;

    if (manifest == NULL || version_path == NULL) {
        MANIFEST_LOG_ERROR("Version gate file check received NULL parameter");
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    ret = bh_ota_version_read_file(version_path,
                                   device_version,
                                   sizeof(device_version),
                                   &device_version_code);
    if (ret != BH_OTA_OK) {
        MANIFEST_LOG_ERROR("Failed to read device version from '%s' before OTA version gate",
                           version_path);
        return ret;
    }

    (void)device_version_code;
    return bh_ota_manifest_check_version(manifest,
                                         device_version,
                                         comparison_result);
}

int32_t bh_ota_manifest_check_system_version(const bh_ota_manifest_t *manifest,
                                             int32_t *comparison_result)
{
    return bh_ota_manifest_check_version_file(manifest,
                                              BH_OTA_SYSTEM_VERSION_PATH,
                                              comparison_result);
}

int32_t bh_ota_manifest_verify_payloads(const bh_ota_manifest_t *manifest,
                                        const char *package_dir)
{
    char payload_path[512];
    char actual_sha256[BH_OTA_SHA256_HEX_LEN + 1U];
    struct stat st;
    size_t i;
    int written;
    int32_t ret;

    if (manifest == NULL || package_dir == NULL || package_dir[0] == '\0') {
        MANIFEST_LOG_ERROR("Payload verification received invalid parameter");
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    for (i = 0; i < manifest->payload_count; ++i) {
        const bh_ota_manifest_payload_t *payload = &manifest->payloads[i];

        written = snprintf(payload_path,
                           sizeof(payload_path),
                           "%s/%s",
                           package_dir,
                           payload->file);
        if (written < 0 || (size_t)written >= sizeof(payload_path)) {
            MANIFEST_LOG_ERROR("Payload path is too long for file '%s'", payload->file);
            return BH_OTA_ERROR;
        }

        if (stat(payload_path, &st) != 0) {
            MANIFEST_LOG_ERROR("Payload file '%s' is missing", payload_path);
            return BH_OTA_ERROR_READ;
        }

        if (!S_ISREG(st.st_mode)) {
            MANIFEST_LOG_ERROR("Payload path '%s' is not a regular file", payload_path);
            return BH_OTA_ERROR;
        }

        if ((uint64_t)st.st_size != (uint64_t)payload->size) {
            MANIFEST_LOG_ERROR("Payload size mismatch for '%s': manifest=%u actual=%lld",
                               payload->file,
                               payload->size,
                               (long long)st.st_size);
            return BH_OTA_ERROR;
        }

        ret = bh_ota_sha256_file_hex(payload_path, actual_sha256, sizeof(actual_sha256));
        if (ret != BH_OTA_OK) {
            return ret;
        }

        if (strcmp(actual_sha256, payload->sha256) != 0) {
            MANIFEST_LOG_ERROR("Payload SHA-256 mismatch for '%s': manifest=%s actual=%s",
                               payload->file,
                               payload->sha256,
                               actual_sha256);
            return BH_OTA_ERROR;
        }

        BH_OTA_LOG_INFO("Payload verified: partition='%s' file='%s' size=%u partition_size=%u sha256=%s",
                        payload->partition,
                        payload->file,
                        payload->size,
                        payload->partition_size,
                        payload->sha256);
    }

    ((bh_ota_manifest_t *)manifest)->payload_hashes_verified = 1U;

    return BH_OTA_OK;
}
