#ifndef __BH_OTA_LOG_H__
#define __BH_OTA_LOG_H__

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    BH_OTA_LOG_LEVEL_ERROR = 0,
    BH_OTA_LOG_LEVEL_WARN = 1,
    BH_OTA_LOG_LEVEL_INFO = 2,
    BH_OTA_LOG_LEVEL_DEBUG = 3,
} bh_ota_log_level_t;

static inline bh_ota_log_level_t bh_ota_log_level_current(void)
{
    const char *level = getenv("BH_OTA_LOG_LEVEL");

    if (level == NULL || level[0] == '\0') {
        return BH_OTA_LOG_LEVEL_INFO;
    }
    if (strcmp(level, "ERROR") == 0) {
        return BH_OTA_LOG_LEVEL_ERROR;
    }
    if (strcmp(level, "WARN") == 0) {
        return BH_OTA_LOG_LEVEL_WARN;
    }
    if (strcmp(level, "DEBUG") == 0) {
        return BH_OTA_LOG_LEVEL_DEBUG;
    }
    return BH_OTA_LOG_LEVEL_INFO;
}

#define BH_OTA_LOG_INFO(fmt, ...) \
    do { \
        if (bh_ota_log_level_current() >= BH_OTA_LOG_LEVEL_INFO) { \
            fprintf(stdout, "[OTA_Package][INFO] " fmt "\n", ##__VA_ARGS__); \
        } \
    } while (0)

#define BH_OTA_LOG_WARN(fmt, ...) \
    do { \
        if (bh_ota_log_level_current() >= BH_OTA_LOG_LEVEL_WARN) { \
            fprintf(stdout, "[OTA_Package][WARN] " fmt "\n", ##__VA_ARGS__); \
        } \
    } while (0)

#define BH_OTA_LOG_DEBUG(fmt, ...) \
    do { \
        if (bh_ota_log_level_current() >= BH_OTA_LOG_LEVEL_DEBUG) { \
            fprintf(stdout, "[OTA_Package][DEBUG] " fmt "\n", ##__VA_ARGS__); \
        } \
    } while (0)

#define BH_OTA_LOG_ERROR(fmt, ...) \
    do { \
        if (bh_ota_log_level_current() >= BH_OTA_LOG_LEVEL_ERROR) { \
            int saved_errno = errno; \
            fprintf(stderr, "[OTA_Package][ERROR] " fmt, ##__VA_ARGS__); \
            if (saved_errno != 0) { \
                fprintf(stderr, ": %s", strerror(saved_errno)); \
            } \
            fprintf(stderr, "\n"); \
            errno = 0; \
        } \
    } while (0)

#endif
