#ifndef __BH_OTA_PACKAGE_CONTEXT_H__
#define __BH_OTA_PACKAGE_CONTEXT_H__

#include "bh_ota_package_internal.h"

typedef struct ota_package_context {
    bh_ota_manifest_t manifest;
    recovery_config_t recovery;
    int32_t target_bank;
    int32_t current_bank;
    size_t total_stage1_payloads;
    size_t total_stage2_payloads;
    size_t completed_stage2_payloads;
} ota_package_context_t;

int32_t ota_env_limit(const char *env_name, size_t *limit_out);

size_t ota_count_stage_payloads(const bh_ota_manifest_t *manifest, uint32_t stage);

int32_t ota_package_load_context(const char *package_dir,
                                 const char *public_key_path,
                                 ota_package_context_t *ctx,
                                 int32_t verify_version,
                                 int32_t verify_payloads);

int32_t ota_package_load_current_bank(ota_package_context_t *ctx);

int32_t ota_package_validate_target_not_current(const ota_package_context_t *ctx);

int32_t ota_apply_stage_payloads(const char *package_dir,
                                 const bh_ota_manifest_t *manifest,
                                 uint32_t stage,
                                 size_t completed_payloads,
                                 size_t total_payloads,
                                 size_t abort_after,
                                 size_t *completed_out);

#endif
