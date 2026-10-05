#ifndef __BH_OTA_PACKAGE_RECOVERY_H__
#define __BH_OTA_PACKAGE_RECOVERY_H__

#include "bh_ota_package_internal.h"

const char *ota_state_to_string(uint32_t state);
const char *ota_bank_to_string(int32_t bank);
uint32_t ota_state_normalize(uint32_t raw_state);
uint32_t ota_recovery_state(const recovery_config_t *recovery);
int32_t ota_recovery_set_state(recovery_config_t *recovery, uint32_t new_state);

int32_t ota_recovery_load(recovery_config_t *recovery_out);
int32_t ota_recovery_store(const recovery_config_t *recovery);

int32_t ota_recovery_has_valid_session(const recovery_config_t *recovery,
                                       const bh_ota_manifest_t *manifest);

int32_t ota_write_stage1_session(const bh_ota_manifest_t *manifest);

int32_t ota_validate_stage1_commit_ready(const bh_ota_manifest_t *manifest,
                                         int32_t target_bank,
                                         size_t total_stage1_payloads,
                                         recovery_config_t *recovery_out);

int32_t ota_commit_stage1_recovery(recovery_config_t *recovery,
                                   int32_t target_bank,
                                   size_t total_stage1_payloads);

int32_t ota_validate_stage2_resume_ready(const bh_ota_manifest_t *manifest,
                                         int32_t target_bank,
                                         size_t total_stage1_payloads,
                                         size_t total_stage2_payloads,
                                         recovery_config_t *recovery_out,
                                         size_t *completed_stage2_payloads_out);

int32_t ota_validate_stage2_commit_ready(const bh_ota_manifest_t *manifest,
                                         int32_t current_bank,
                                         size_t total_stage2_payloads,
                                         recovery_config_t *recovery_out);

int32_t ota_prepare_stage2_recovery(recovery_config_t *recovery,
                                    int32_t current_bank,
                                    size_t completed_stage2_payloads);

int32_t ota_complete_stage2_recovery(recovery_config_t *recovery,
                                     int32_t current_bank,
                                     size_t total_stage2_payloads);

int32_t ota_commit_stage2_recovery(recovery_config_t *recovery,
                                   int32_t current_bank,
                                   size_t total_stage2_payloads);

int32_t ota_validate_boot_success_ready(const bh_ota_manifest_t *manifest,
                                        int32_t target_bank,
                                        size_t total_stage2_payloads,
                                        recovery_config_t *recovery_out,
                                        int32_t *current_bank_out);

int32_t ota_commit_boot_success(recovery_config_t *recovery, int32_t current_bank);

#endif
