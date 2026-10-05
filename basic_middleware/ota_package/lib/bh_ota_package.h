#ifndef __BH_OTA_PACKAGE_H__
#define __BH_OTA_PACKAGE_H__

#include <stddef.h>
#include <stdint.h>

#include "bh_ota_hal.h"

#define BH_OTA_MANIFEST_MAX_PAYLOADS 32U
#define BH_OTA_MANIFEST_FILE_NAME_MAX_LEN 128U
#define BH_OTA_SHA256_HEX_LEN 64U
#define BH_OTA_MANIFEST_FILE_NAME "data.json"
#define BH_OTA_SIGNATURE_FILE_NAME "signature.sig"

typedef struct bh_ota_manifest_payload {
    char partition[32];
    char file[BH_OTA_MANIFEST_FILE_NAME_MAX_LEN];
    char sha256[BH_OTA_SHA256_HEX_LEN + 1U];
    uint32_t size;
    uint32_t partition_size;
    uint32_t stage;
} bh_ota_manifest_payload_t;

typedef struct bh_ota_manifest {
    uint32_t format_version;
    char sys_version[BH_OTA_SYS_VERSION_MAX_LEN];
    uint32_t sys_version_code;
    char session_id[BH_OTA_SESSION_ID_MAX_LEN];
    uint32_t rollback_index;
    size_t payload_count;
    uint8_t payload_hashes_verified;
    bh_ota_manifest_payload_t payloads[BH_OTA_MANIFEST_MAX_PAYLOADS];
} bh_ota_manifest_t;

int32_t bh_ota_version_parse(const char *version_str,
                             uint32_t *version_code,
                             char *normalized_version,
                             size_t normalized_len);

int32_t bh_ota_version_compare(const char *lhs,
                               const char *rhs,
                               int32_t *result);

int32_t bh_ota_version_read_file(const char *path,
                                 char *normalized_version,
                                 size_t normalized_len,
                                 uint32_t *version_code);

int32_t bh_ota_version_read_system(char *normalized_version,
                                   size_t normalized_len,
                                   uint32_t *version_code);

int32_t bh_ota_manifest_parse(const char *json, bh_ota_manifest_t *manifest);
int32_t bh_ota_manifest_parse_file(const char *path, bh_ota_manifest_t *manifest);

int32_t bh_ota_manifest_check_version(const bh_ota_manifest_t *manifest,
                                      const char *device_version,
                                      int32_t *comparison_result);

int32_t bh_ota_manifest_check_version_file(const bh_ota_manifest_t *manifest,
                                           const char *version_path,
                                           int32_t *comparison_result);

int32_t bh_ota_manifest_check_system_version(const bh_ota_manifest_t *manifest,
                                             int32_t *comparison_result);

int32_t bh_ota_sha256_file_hex(const char *path, char *out_hex, size_t out_len);

int32_t bh_ota_manifest_verify_payloads(const bh_ota_manifest_t *manifest,
                                        const char *package_dir);

int32_t bh_ota_manifest_build_signature_payload(const bh_ota_manifest_t *manifest,
                                                const char *package_dir,
                                                uint8_t **out_buf,
                                                size_t *out_len);

int32_t bh_ota_manifest_verify_signature(const bh_ota_manifest_t *manifest,
                                         const char *package_dir,
                                         const char *signature_path,
                                         const char *public_key_path);

int32_t bh_ota_package_verify_stage1(const char *package_dir,
                                     const char *public_key_path,
                                     bh_ota_manifest_t *manifest_out,
                                     int32_t *target_bank_out);

int32_t bh_ota_package_apply_stage1(const char *package_dir,
                                    const char *public_key_path,
                                    int32_t *target_bank_out);

int32_t bh_ota_package_commit_stage1(const char *package_dir,
                                     const char *public_key_path,
                                     int32_t *target_bank_out);

int32_t bh_ota_package_verify_stage2(const char *package_dir,
                                     const char *public_key_path,
                                     int32_t *current_bank_out);

int32_t bh_ota_package_apply_stage2(const char *package_dir,
                                    const char *public_key_path,
                                    int32_t *current_bank_out);

int32_t bh_ota_package_commit_stage2(const char *package_dir,
                                     const char *public_key_path,
                                     int32_t *current_bank_out);

int32_t bh_ota_package_boot_success(const char *package_dir,
                                    const char *public_key_path,
                                    int32_t *current_bank_out);

#endif
