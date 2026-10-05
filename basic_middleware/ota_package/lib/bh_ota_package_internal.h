#ifndef __BH_OTA_PACKAGE_INTERNAL_H__
#define __BH_OTA_PACKAGE_INTERNAL_H__

#include "bh_ota_log.h"
#include "bh_ota_package.h"

#define PACKAGE_LOG_INFO(fmt, ...) BH_OTA_LOG_INFO(fmt, ##__VA_ARGS__)
#define PACKAGE_LOG_WARN(fmt, ...) BH_OTA_LOG_WARN(fmt, ##__VA_ARGS__)
#define PACKAGE_LOG_ERROR(fmt, ...) BH_OTA_LOG_ERROR(fmt, ##__VA_ARGS__)
#define PACKAGE_LOG_DEBUG(fmt, ...) BH_OTA_LOG_DEBUG(fmt, ##__VA_ARGS__)

#define BH_OTA_PATH_MAX 512U
#define COPY_CHUNK_SIZE 65536U

int32_t bh_ota_package_apply_payload(const char *package_dir,
                                     const bh_ota_manifest_payload_t *payload);

#endif
