#ifndef OTA_INFO_LOG_H
#define OTA_INFO_LOG_H

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    OTA_INFO_LOG_LEVEL_ERROR = 0,
    OTA_INFO_LOG_LEVEL_WARN = 1,
    OTA_INFO_LOG_LEVEL_INFO = 2,
    OTA_INFO_LOG_LEVEL_DEBUG = 3,
} ota_info_log_level_t;

static inline ota_info_log_level_t ota_info_log_level_current(void)
{
    const char *level = getenv("BH_OTA_LOG_LEVEL");

    if (level == NULL || level[0] == '\0') {
        return OTA_INFO_LOG_LEVEL_INFO;
    }
    if (strcmp(level, "ERROR") == 0) {
        return OTA_INFO_LOG_LEVEL_ERROR;
    }
    if (strcmp(level, "WARN") == 0) {
        return OTA_INFO_LOG_LEVEL_WARN;
    }
    if (strcmp(level, "DEBUG") == 0) {
        return OTA_INFO_LOG_LEVEL_DEBUG;
    }
    return OTA_INFO_LOG_LEVEL_INFO;
}

static inline void ota_info_log_emit(FILE *stream,
                                     const char *level,
                                     const char *fmt,
                                     ...)
{
    va_list args;

    fprintf(stream, "[OTA_Info][%s] ", level);
    va_start(args, fmt);
    vfprintf(stream, fmt, args);
    va_end(args);
    fputc('\n', stream);
}

#define OTA_INFO_LOG_INFO(fmt, ...) \
    do { \
        if (ota_info_log_level_current() >= OTA_INFO_LOG_LEVEL_INFO) { \
            ota_info_log_emit(stdout, "INFO", fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#define OTA_INFO_LOG_WARN(fmt, ...) \
    do { \
        if (ota_info_log_level_current() >= OTA_INFO_LOG_LEVEL_WARN) { \
            ota_info_log_emit(stdout, "WARN", fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#define OTA_INFO_LOG_DEBUG(fmt, ...) \
    do { \
        if (ota_info_log_level_current() >= OTA_INFO_LOG_LEVEL_DEBUG) { \
            ota_info_log_emit(stdout, "DEBUG", fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#define OTA_INFO_LOG_ERROR(fmt, ...) \
    do { \
        if (ota_info_log_level_current() >= OTA_INFO_LOG_LEVEL_ERROR) { \
            int saved_errno__ = errno; \
            ota_info_log_emit(stderr, "ERROR", fmt, ##__VA_ARGS__); \
            if (saved_errno__ != 0) { \
                ota_info_log_emit(stderr, "ERROR", "event=errno detail=%s", strerror(saved_errno__)); \
            } \
            errno = 0; \
        } \
    } while (0)

#endif
