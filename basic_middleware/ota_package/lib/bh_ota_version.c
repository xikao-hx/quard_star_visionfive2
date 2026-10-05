#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bh_ota_log.h"
#include "bh_ota_package.h"

#define BH_OTA_VERSION_COMPONENTS 3U
#define BH_OTA_VERSION_COMPONENT_MAX 99U
#define BH_OTA_VERSION_FILE_BUF_LEN 128U

#define VERSION_LOG_ERROR(fmt, ...) BH_OTA_LOG_ERROR(fmt, ##__VA_ARGS__)

static int32_t bh_ota_version_validate_outputs(uint32_t *version_code,
                                               char *normalized_version,
                                               size_t normalized_len)
{
    if (version_code == NULL && normalized_version == NULL) {
        VERSION_LOG_ERROR("Version parse requires at least one output target");
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    if (normalized_version != NULL && normalized_len == 0U) {
        VERSION_LOG_ERROR("Normalized version buffer length must be greater than zero");
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    return BH_OTA_OK;
}

static int32_t bh_ota_version_trim_copy(const char *src,
                                        char *dst,
                                        size_t dst_len)
{
    const char *start;
    const char *end;
    size_t len;

    if (src == NULL || dst == NULL || dst_len == 0U) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    start = src;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }

    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }

    len = (size_t)(end - start);
    if (len == 0U) {
        VERSION_LOG_ERROR("Version string is empty after trimming");
        return BH_OTA_ERROR;
    }

    if (len >= dst_len) {
        VERSION_LOG_ERROR("Version string length %zu exceeds buffer size %zu", len, dst_len);
        return BH_OTA_ERROR;
    }

    memcpy(dst, start, len);
    dst[len] = '\0';
    return BH_OTA_OK;
}

int32_t bh_ota_version_parse(const char *version_str,
                             uint32_t *version_code,
                             char *normalized_version,
                             size_t normalized_len)
{
    uint32_t components[BH_OTA_VERSION_COMPONENTS];
    uint32_t component_value;
    size_t component_index;
    size_t digit_count;
    size_t i;
    int32_t ret;

    ret = bh_ota_version_validate_outputs(version_code, normalized_version, normalized_len);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    if (version_str == NULL || version_str[0] == '\0') {
        VERSION_LOG_ERROR("Version string is empty");
        return BH_OTA_ERROR;
    }

    memset(components, 0, sizeof(components));
    component_value = 0U;
    component_index = 0U;
    digit_count = 0U;

    for (i = 0U; ; ++i) {
        unsigned char ch = (unsigned char)version_str[i];

        if (isdigit(ch)) {
            component_value = (component_value * 10U) + (uint32_t)(ch - '0');
            digit_count++;
            if (component_value > BH_OTA_VERSION_COMPONENT_MAX) {
                VERSION_LOG_ERROR("Version component exceeds maximum value %u", BH_OTA_VERSION_COMPONENT_MAX);
                return BH_OTA_ERROR;
            }
            continue;
        }

        if (ch == '.' || ch == '\0') {
            if (digit_count == 0U) {
                VERSION_LOG_ERROR("Version string contains an empty component");
                return BH_OTA_ERROR;
            }

            if (component_index >= BH_OTA_VERSION_COMPONENTS) {
                VERSION_LOG_ERROR("Version string contains too many components");
                return BH_OTA_ERROR;
            }

            components[component_index++] = component_value;
            component_value = 0U;
            digit_count = 0U;

            if (ch == '\0') {
                break;
            }
            continue;
        }

        VERSION_LOG_ERROR("Version string contains invalid character '%c'", ch);
        return BH_OTA_ERROR;
    }

    if (component_index != BH_OTA_VERSION_COMPONENTS) {
        VERSION_LOG_ERROR("Version string must contain exactly %u components", BH_OTA_VERSION_COMPONENTS);
        return BH_OTA_ERROR;
    }

    if (version_code != NULL) {
        *version_code = (components[0] * 10000U) + (components[1] * 100U) + components[2];
    }

    if (normalized_version != NULL) {
        int written = snprintf(normalized_version,
                               normalized_len,
                               "%u.%u.%u",
                               components[0],
                               components[1],
                               components[2]);
        if (written < 0 || (size_t)written >= normalized_len) {
            VERSION_LOG_ERROR("Normalized version buffer is too small");
            return BH_OTA_ERROR_INVALID_PARAM;
        }
    }

    return BH_OTA_OK;
}

int32_t bh_ota_version_compare(const char *lhs,
                               const char *rhs,
                               int32_t *result)
{
    uint32_t lhs_code;
    uint32_t rhs_code;

    if (result == NULL) {
        VERSION_LOG_ERROR("Version compare result pointer is NULL");
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    if (bh_ota_version_parse(lhs, &lhs_code, NULL, 0U) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    if (bh_ota_version_parse(rhs, &rhs_code, NULL, 0U) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    if (lhs_code < rhs_code) {
        *result = -1;
    } else if (lhs_code > rhs_code) {
        *result = 1;
    } else {
        *result = 0;
    }

    return BH_OTA_OK;
}

int32_t bh_ota_version_read_file(const char *path,
                                 char *normalized_version,
                                 size_t normalized_len,
                                 uint32_t *version_code)
{
    char file_buf[BH_OTA_VERSION_FILE_BUF_LEN];
    char trimmed_buf[BH_OTA_SYS_VERSION_MAX_LEN];
    ssize_t bytes_read;
    int32_t fd;
    int32_t ret;

    ret = bh_ota_version_validate_outputs(version_code, normalized_version, normalized_len);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    if (path == NULL || path[0] == '\0') {
        VERSION_LOG_ERROR("Version file path is empty");
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        VERSION_LOG_ERROR("Failed to open version file '%s'", path);
        return BH_OTA_ERROR_READ;
    }

    memset(file_buf, 0, sizeof(file_buf));
    bytes_read = read(fd, file_buf, sizeof(file_buf) - 1U);
    close(fd);

    if (bytes_read < 0) {
        VERSION_LOG_ERROR("Failed to read version file '%s'", path);
        return BH_OTA_ERROR_READ;
    }

    if (bytes_read == 0) {
        VERSION_LOG_ERROR("Version file '%s' is empty", path);
        return BH_OTA_ERROR;
    }

    file_buf[bytes_read] = '\0';

    ret = bh_ota_version_trim_copy(file_buf, trimmed_buf, sizeof(trimmed_buf));
    if (ret != BH_OTA_OK) {
        return ret;
    }

    return bh_ota_version_parse(trimmed_buf,
                                version_code,
                                normalized_version,
                                normalized_len);
}

int32_t bh_ota_version_read_system(char *normalized_version,
                                   size_t normalized_len,
                                   uint32_t *version_code)
{
    return bh_ota_version_read_file(BH_OTA_SYSTEM_VERSION_PATH,
                                    normalized_version,
                                    normalized_len,
                                    version_code);
}
