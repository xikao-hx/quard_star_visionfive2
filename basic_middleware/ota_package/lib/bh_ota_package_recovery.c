#include <string.h>

#include "bh_ota_package_recovery.h"

uint32_t ota_state_normalize(uint32_t raw_state)
{
    switch (raw_state) {
    case BH_OTA_STATE_IDLE:
    case BH_OTA_STATE_FAILED:
    case BH_OTA_STATE_STAGE1_START:
    case BH_OTA_STATE_STAGE1_WROTE:
    case BH_OTA_STATE_STAGE1_END:
    case BH_OTA_STATE_STAGE2_START:
    case BH_OTA_STATE_STAGE2_WROTE:
    case BH_OTA_STATE_STAGE2_END:
    case BH_OTA_STATE_COMPLETE:
        return raw_state;
    default:
        return BH_OTA_STATE_FAILED;
    }
}

const char *ota_state_to_string(uint32_t state)
{
    switch (state) {
    case BH_OTA_STATE_IDLE:
        return "idle";
    case BH_OTA_STATE_STAGE1_START:
        return "stage1_start";
    case BH_OTA_STATE_STAGE1_WROTE:
        return "stage1_wrote";
    case BH_OTA_STATE_STAGE1_END:
        return "stage1_end";
    case BH_OTA_STATE_STAGE2_START:
        return "stage2_start";
    case BH_OTA_STATE_STAGE2_WROTE:
        return "stage2_wrote";
    case BH_OTA_STATE_STAGE2_END:
        return "stage2_end";
    case BH_OTA_STATE_COMPLETE:
        return "complete";
    case BH_OTA_STATE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

const char *ota_bank_to_string(int32_t bank)
{
    if (bank == 0) {
        return "a";
    }
    if (bank == 1) {
        return "b";
    }
    return "unknown";
}

uint32_t ota_recovery_state(const recovery_config_t *recovery)
{
    if (recovery == NULL) {
        return BH_OTA_STATE_FAILED;
    }

    return ota_state_normalize(recovery->ota_state);
}

int32_t ota_recovery_set_state(recovery_config_t *recovery, uint32_t new_state)
{
    uint32_t old_state;

    if (recovery == NULL) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    old_state = recovery->ota_state;
    recovery->ota_state = new_state;
    PACKAGE_LOG_INFO("state_transition from=%s(%u) to=%s(%u)",
                     ota_state_to_string(old_state),
                     old_state,
                     ota_state_to_string(new_state),
                     new_state);
    return BH_OTA_OK;
}

int32_t ota_recovery_load(recovery_config_t *recovery_out)
{
    if (recovery_out == NULL) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    if (bh_hal_ota_recovery_read(recovery_out) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    return BH_OTA_OK;
}

int32_t ota_recovery_store(const recovery_config_t *recovery)
{
    if (recovery == NULL) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    if (bh_hal_ota_recovery_write(recovery) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    return BH_OTA_OK;
}

int32_t ota_recovery_has_valid_session(const recovery_config_t *recovery,
                                       const bh_ota_manifest_t *manifest)
{
    uint32_t session_crc32;

    if (recovery == NULL || manifest == NULL) {
        return 0;
    }

    /* Session identity must match manifest metadata before any resume/commit is trusted. */
    if (recovery->session_id[0] == '\0' || recovery->pending_sys_version[0] == '\0') {
        return 0;
    }

    if (strcmp(recovery->session_id, manifest->session_id) != 0) {
        return 0;
    }

    if (strcmp(recovery->pending_sys_version, manifest->sys_version) != 0) {
        return 0;
    }

    if (recovery->pending_rollback_index != manifest->rollback_index ||
        recovery->pending_image_version != manifest->sys_version_code) {
        return 0;
    }

    session_crc32 = bh_hal_ota_crc32((const unsigned char *)recovery->session_id,
                                     (uint32_t)strlen(recovery->session_id));
    if (recovery->session_id_crc32 != session_crc32) {
        return 0;
    }

    return 1;
}

int32_t ota_write_stage1_session(const bh_ota_manifest_t *manifest)
{
    recovery_config_t recovery;

    if (manifest == NULL) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    if (ota_recovery_load(&recovery) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    ota_recovery_set_state(&recovery, BH_OTA_STATE_STAGE1_START);
    recovery.ota_update = 0;
    recovery.ota_reboot_cnt = 0U;
    recovery.boot_success = 0;
    recovery.pending_rollback_index = manifest->rollback_index;
    recovery.pending_image_version = manifest->sys_version_code;

    memset(recovery.session_id, 0, sizeof(recovery.session_id));
    memset(recovery.pending_sys_version, 0, sizeof(recovery.pending_sys_version));

    snprintf(recovery.session_id, sizeof(recovery.session_id), "%s", manifest->session_id);
    snprintf(recovery.pending_sys_version, sizeof(recovery.pending_sys_version), "%s", manifest->sys_version);
    recovery.session_id_crc32 = bh_hal_ota_crc32((const unsigned char *)recovery.session_id,
                                                 (uint32_t)strlen(recovery.session_id));

    if (ota_recovery_store(&recovery) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    PACKAGE_LOG_INFO("OTA session initialized: session_id='%s' pending_sys_version='%s' pending_rollback_index=%u",
                     recovery.session_id, recovery.pending_sys_version, recovery.pending_rollback_index);
    return BH_OTA_OK;
}

int32_t ota_validate_stage1_commit_ready(const bh_ota_manifest_t *manifest,
                                         int32_t target_bank,
                                         size_t total_stage1_payloads,
                                         recovery_config_t *recovery_out)
{
    recovery_config_t recovery;
    uint32_t session_crc32;

    if (manifest == NULL || recovery_out == NULL) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    if (total_stage1_payloads == 0U) {
        PACKAGE_LOG_ERROR("Stage-1 commit requires at least one stage-1 payload");
        return BH_OTA_ERROR;
    }

    if (ota_recovery_load(&recovery) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    /* Stage-1 commit is idempotent, but it must only run after all stage-1 payloads are written. */
    switch (ota_recovery_state(&recovery)) {
    case BH_OTA_STATE_STAGE1_WROTE:
    case BH_OTA_STATE_STAGE1_END:
        break;
    default:
        PACKAGE_LOG_ERROR("Stage-1 commit rejected: expected ota_state=stage1_wrote or stage1_end, actual=%s(%u)",
                          ota_state_to_string(ota_recovery_state(&recovery)),
                          recovery.ota_state);
        return BH_OTA_ERROR;
    }

    if (strcmp(recovery.session_id, manifest->session_id) != 0) {
        PACKAGE_LOG_ERROR("Stage-1 commit rejected: expected session_id='%s', actual='%s'",
                          manifest->session_id,
                          recovery.session_id);
        return BH_OTA_ERROR;
    }

    if (strcmp(recovery.pending_sys_version, manifest->sys_version) != 0) {
        PACKAGE_LOG_ERROR("Stage-1 commit rejected: expected pending_sys_version='%s', actual='%s'",
                          manifest->sys_version,
                          recovery.pending_sys_version);
        return BH_OTA_ERROR;
    }

    if (recovery.pending_rollback_index != manifest->rollback_index ||
        recovery.pending_image_version != manifest->sys_version_code) {
        PACKAGE_LOG_ERROR("Stage-1 commit rejected: expected pending_rollback_index=%u and pending_image_version=%u, actual pending_rollback_index=%u pending_image_version=%u",
                          manifest->rollback_index,
                          manifest->sys_version_code,
                          recovery.pending_rollback_index,
                          recovery.pending_image_version);
        return BH_OTA_ERROR;
    }

    session_crc32 = bh_hal_ota_crc32((const unsigned char *)recovery.session_id,
                                     (uint32_t)strlen(recovery.session_id));
    if (recovery.session_id_crc32 != session_crc32) {
        PACKAGE_LOG_ERROR("Stage-1 commit rejected: expected session_id_crc32=0x%08x, actual=0x%08x",
                          session_crc32, recovery.session_id_crc32);
        return BH_OTA_ERROR;
    }

    if (recovery.boot_success != 0U) {
        PACKAGE_LOG_ERROR("Stage-1 commit rejected: expected boot_success=0, actual=%u",
                          recovery.boot_success);
        return BH_OTA_ERROR;
    }

    if (recovery.ota_update != (uint32_t)total_stage1_payloads) {
        PACKAGE_LOG_ERROR("Stage-1 commit rejected: expected ota_update=%zu, actual=%u",
                          total_stage1_payloads,
                          recovery.ota_update);
        return BH_OTA_ERROR;
    }

    if (ota_recovery_state(&recovery) == BH_OTA_STATE_STAGE1_END &&
        recovery.target_bank != (uint32_t)(target_bank & 0x1)) {
        PACKAGE_LOG_ERROR("Stage-1 commit rejected: ota_state is stage1_end but expected target_bank=%s(%d), actual=%s(%u)",
                          ota_bank_to_string(target_bank),
                          target_bank,
                          ota_bank_to_string((int32_t)recovery.target_bank),
                          recovery.target_bank);
        return BH_OTA_ERROR;
    }

    *recovery_out = recovery;
    return BH_OTA_OK;
}

int32_t ota_commit_stage1_recovery(recovery_config_t *recovery,
                                   int32_t target_bank,
                                   size_t total_stage1_payloads)
{
    uint32_t current_bank_mask;
    uint32_t target_bank_mask;

    if (recovery == NULL || (target_bank != 0 && target_bank != 1)) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    current_bank_mask = (1U << (recovery->current_bank & 0x1));
    target_bank_mask = (1U << (target_bank & 0x1));

    recovery->usable_bank |= current_bank_mask | target_bank_mask;
    recovery->target_bank = (uint32_t)(target_bank & 0x1);
    ota_recovery_set_state(recovery, BH_OTA_STATE_STAGE1_END);
    recovery->ota_update = (uint32_t)total_stage1_payloads;
    recovery->ota_reboot_cnt = 0U;
    recovery->boot_success = 0U;

    if (ota_recovery_store(recovery) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    PACKAGE_LOG_INFO("Stage-1 commit completed: target_bank=%c usable_bank=0x%x ota_state=stage1_end",
                     target_bank == 1 ? 'b' : 'a',
                     recovery->usable_bank);
    return BH_OTA_OK;
}

int32_t ota_validate_stage2_resume_ready(const bh_ota_manifest_t *manifest,
                                         int32_t target_bank,
                                         size_t total_stage1_payloads,
                                         size_t total_stage2_payloads,
                                         recovery_config_t *recovery_out,
                                         size_t *completed_stage2_payloads_out)
{
    recovery_config_t recovery;
    int32_t current_bank = -1;

    if (manifest == NULL || recovery_out == NULL || completed_stage2_payloads_out == NULL) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    if (total_stage1_payloads == 0U || total_stage2_payloads == 0U) {
        PACKAGE_LOG_ERROR("Stage-2 resume requires both stage-1 and stage-2 payloads");
        return BH_OTA_ERROR;
    }

    if (bh_hal_ota_get_current_bank(&current_bank) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    if (current_bank != target_bank) {
        PACKAGE_LOG_ERROR("Stage-2 resume rejected: current bank=%s(%d), expected target bank=%s(%d)",
                          ota_bank_to_string(current_bank),
                          current_bank,
                          ota_bank_to_string(target_bank),
                          target_bank);
        return BH_OTA_ERROR;
    }

    if (ota_recovery_load(&recovery) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    if (recovery.target_bank != (uint32_t)(target_bank & 0x1)) {
        PACKAGE_LOG_ERROR("Stage-2 resume rejected: recovery target_bank=%s(%u), expected=%s(%d)",
                          ota_bank_to_string((int32_t)recovery.target_bank),
                          recovery.target_bank,
                          ota_bank_to_string(target_bank),
                          target_bank);
        return BH_OTA_ERROR;
    }

    /* Stage-2 may start fresh from stage1_end, or continue from a partially written stage2_start. */
    switch (ota_recovery_state(&recovery)) {
    case BH_OTA_STATE_STAGE1_END:
        if (recovery.ota_update != (uint32_t)total_stage1_payloads) {
            PACKAGE_LOG_ERROR("Stage-2 resume rejected: expected stage-1 ota_update=%zu, actual=%u",
                              total_stage1_payloads,
                              recovery.ota_update);
            return BH_OTA_ERROR;
        }
        *completed_stage2_payloads_out = 0U;
        break;
    case BH_OTA_STATE_STAGE2_START:
        if (recovery.ota_update > (uint32_t)total_stage2_payloads) {
            PACKAGE_LOG_ERROR("Stage-2 resume rejected: stage-2 ota_update=%u exceeds total=%zu",
                              recovery.ota_update,
                              total_stage2_payloads);
            return BH_OTA_ERROR;
        }
        *completed_stage2_payloads_out = (size_t)recovery.ota_update;
        break;
    default:
        PACKAGE_LOG_ERROR("Stage-2 resume rejected: unexpected ota_state=%s(%u)",
                          ota_state_to_string(ota_recovery_state(&recovery)),
                          recovery.ota_state);
        return BH_OTA_ERROR;
    }

    if (!ota_recovery_has_valid_session(&recovery, manifest)) {
        PACKAGE_LOG_ERROR("Stage-2 resume rejected: session is incomplete or session_id CRC is invalid");
        return BH_OTA_ERROR;
    }

    if (recovery.boot_success != 0U) {
        PACKAGE_LOG_ERROR("Stage-2 resume rejected: expected boot_success=0, actual=%u",
                          recovery.boot_success);
        return BH_OTA_ERROR;
    }

    *recovery_out = recovery;
    return BH_OTA_OK;
}

int32_t ota_validate_stage2_commit_ready(const bh_ota_manifest_t *manifest,
                                         int32_t current_bank,
                                         size_t total_stage2_payloads,
                                         recovery_config_t *recovery_out)
{
    recovery_config_t recovery;

    if (manifest == NULL || recovery_out == NULL) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    if (total_stage2_payloads == 0U) {
        PACKAGE_LOG_ERROR("Stage-2 commit requires at least one stage-2 payload");
        return BH_OTA_ERROR;
    }

    if (ota_recovery_load(&recovery) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    if (ota_recovery_state(&recovery) != BH_OTA_STATE_STAGE2_WROTE &&
        ota_recovery_state(&recovery) != BH_OTA_STATE_STAGE2_END) {
        PACKAGE_LOG_ERROR("Stage-2 commit rejected: expected ota_state=stage2_wrote or stage2_end, actual=%s(%u)",
                          ota_state_to_string(ota_recovery_state(&recovery)),
                          recovery.ota_state);
        return BH_OTA_ERROR;
    }

    if (!ota_recovery_has_valid_session(&recovery, manifest)) {
        PACKAGE_LOG_ERROR("Stage-2 commit rejected: session is incomplete or session_id CRC is invalid");
        return BH_OTA_ERROR;
    }

    if (recovery.current_bank != (uint32_t)(current_bank & 0x1) ||
        recovery.target_bank != (uint32_t)(current_bank & 0x1)) {
        PACKAGE_LOG_ERROR("Stage-2 commit rejected: recovery current/target bank mismatch");
        return BH_OTA_ERROR;
    }

    if (recovery.ota_update != (uint32_t)total_stage2_payloads) {
        PACKAGE_LOG_ERROR("Stage-2 commit rejected: expected ota_update=%zu, actual=%u",
                          total_stage2_payloads,
                          recovery.ota_update);
        return BH_OTA_ERROR;
    }

    *recovery_out = recovery;
    return BH_OTA_OK;
}

int32_t ota_prepare_stage2_recovery(recovery_config_t *recovery,
                                    int32_t current_bank,
                                    size_t completed_stage2_payloads)
{
    uint32_t current_bank_mask;
    uint32_t stage2_bank_mask;
    int32_t stage2_bank;

    if (recovery == NULL || (current_bank != 0 && current_bank != 1)) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    stage2_bank = current_bank == 0 ? 1 : 0;
    current_bank_mask = (1U << (current_bank & 0x1));
    stage2_bank_mask = (1U << (stage2_bank & 0x1));
    recovery->target_bank = (uint32_t)(current_bank & 0x1);
    recovery->usable_bank |= current_bank_mask;
    recovery->successful_bank_mask |= current_bank_mask;
    recovery->successful_bank_mask &= ~stage2_bank_mask;
    ota_recovery_set_state(recovery, BH_OTA_STATE_STAGE2_START);
    recovery->ota_update = (uint32_t)completed_stage2_payloads;
    recovery->ota_reboot_cnt = 0U;

    if (ota_recovery_store(recovery) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    PACKAGE_LOG_INFO("Stage-2 resume started: current bank=%c completed=%zu successful_bank_mask=0x%x",
                     current_bank == 1 ? 'b' : 'a',
                     completed_stage2_payloads,
                     recovery->successful_bank_mask);
    return BH_OTA_OK;
}

int32_t ota_complete_stage2_recovery(recovery_config_t *recovery,
                                     int32_t current_bank,
                                     size_t total_stage2_payloads)
{
    uint32_t current_bank_mask;
    uint32_t old_bank_mask;
    int32_t old_bank;

    if (recovery == NULL || (current_bank != 0 && current_bank != 1)) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    old_bank = current_bank == 0 ? 1 : 0;
    current_bank_mask = (1U << (current_bank & 0x1));
    old_bank_mask = (1U << (old_bank & 0x1));

    recovery->target_bank = (uint32_t)(current_bank & 0x1);
    recovery->usable_bank |= current_bank_mask | old_bank_mask;
    recovery->successful_bank_mask &= ~old_bank_mask;
    ota_recovery_set_state(recovery, BH_OTA_STATE_STAGE2_WROTE);
    recovery->ota_update = (uint32_t)total_stage2_payloads;
    recovery->boot_success = 0U;

    if (ota_recovery_store(recovery) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    PACKAGE_LOG_INFO("Stage-2 payload write completed: current bank=%c next boot remains bank=%c ota_state=stage2_wrote usable_bank=0x%x",
                     current_bank == 1 ? 'b' : 'a',
                     current_bank == 1 ? 'b' : 'a',
                     recovery->usable_bank);
    return BH_OTA_OK;
}

int32_t ota_commit_stage2_recovery(recovery_config_t *recovery,
                                   int32_t current_bank,
                                   size_t total_stage2_payloads)
{
    int32_t final_bank;

    if (recovery == NULL || (current_bank != 0 && current_bank != 1)) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    final_bank = current_bank == 0 ? 1 : 0;
    recovery->target_bank = (uint32_t)(final_bank & 0x1);
    recovery->ota_update = (uint32_t)total_stage2_payloads;
    recovery->ota_reboot_cnt = 0U;
    recovery->boot_success = 0U;
    ota_recovery_set_state(recovery, BH_OTA_STATE_STAGE2_END);

    if (ota_recovery_store(recovery) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    PACKAGE_LOG_INFO("Stage-2 commit completed: current bank=%c next boot bank=%c ota_state=stage2_end",
                     current_bank == 1 ? 'b' : 'a',
                     final_bank == 1 ? 'b' : 'a');
    return BH_OTA_OK;
}

int32_t ota_validate_boot_success_ready(const bh_ota_manifest_t *manifest,
                                        int32_t target_bank,
                                        size_t total_stage2_payloads,
                                        recovery_config_t *recovery_out,
                                        int32_t *current_bank_out)
{
    recovery_config_t recovery;
    int32_t current_bank = -1;

    if (manifest == NULL || recovery_out == NULL || current_bank_out == NULL) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }
    (void)target_bank;

    if (total_stage2_payloads == 0U) {
        PACKAGE_LOG_ERROR("Boot success confirmation requires stage-2 payloads");
        return BH_OTA_ERROR;
    }

    if (bh_hal_ota_get_current_bank(&current_bank) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    if (ota_recovery_load(&recovery) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    if (current_bank != (int32_t)(recovery.target_bank & 0x1)) {
        PACKAGE_LOG_INFO("Boot success confirmation not required: current bank=%c recovery target bank=%c",
                         current_bank == 1 ? 'b' : 'a',
                         recovery.target_bank == 1 ? 'b' : 'a');
        return BH_OTA_OK;
    }

    if (ota_recovery_state(&recovery) != BH_OTA_STATE_STAGE2_END) {
        PACKAGE_LOG_INFO("Boot success confirmation not required: ota_state=%s(%u)",
                         ota_state_to_string(ota_recovery_state(&recovery)),
                         recovery.ota_state);
        return BH_OTA_OK;
    }

    if (recovery.ota_update != (uint32_t)total_stage2_payloads) {
        PACKAGE_LOG_ERROR("Boot success confirmation skipped: expected stage-2 ota_update=%zu, actual=%u",
                          total_stage2_payloads,
                          recovery.ota_update);
        return BH_OTA_OK;
    }

    if (!ota_recovery_has_valid_session(&recovery, manifest)) {
        PACKAGE_LOG_ERROR("Boot success confirmation skipped: session is incomplete or session_id CRC is invalid");
        return BH_OTA_OK;
    }

    if (recovery.boot_success != 0U) {
        PACKAGE_LOG_INFO("Boot success confirmation not required: boot_success=%u", recovery.boot_success);
        return BH_OTA_OK;
    }

    *recovery_out = recovery;
    *current_bank_out = current_bank;
    return BH_OTA_OK;
}

int32_t ota_commit_boot_success(recovery_config_t *recovery, int32_t current_bank)
{
    uint32_t current_bank_mask;

    if (recovery == NULL || (current_bank != 0 && current_bank != 1)) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    current_bank_mask = (1U << (current_bank & 0x1));
    recovery->successful_bank_mask |= current_bank_mask;
    recovery->boot_success = 1U;
    recovery->rollback_index = recovery->pending_rollback_index;
    recovery->current_image_version = recovery->pending_image_version;
    ota_recovery_set_state(recovery, BH_OTA_STATE_COMPLETE);
    recovery->ota_update = 0U;
    recovery->ota_reboot_cnt = 0U;

    memset(recovery->current_sys_version, 0, sizeof(recovery->current_sys_version));
    snprintf(recovery->current_sys_version,
             sizeof(recovery->current_sys_version),
             "%s",
             recovery->pending_sys_version);

    memset(recovery->pending_sys_version, 0, sizeof(recovery->pending_sys_version));
    memset(recovery->session_id, 0, sizeof(recovery->session_id));
    recovery->pending_rollback_index = 0U;
    recovery->pending_image_version = 0U;
    recovery->session_id_crc32 = 0U;

    if (ota_recovery_store(recovery) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    PACKAGE_LOG_INFO("Boot success committed: current bank=%c next boot bank=%c rollback_index=%u current_image_version=%u successful_bank_mask=0x%x",
                     current_bank == 1 ? 'b' : 'a',
                     recovery->target_bank == 1 ? 'b' : 'a',
                     recovery->rollback_index,
                     recovery->current_image_version,
                     recovery->successful_bank_mask);
    return BH_OTA_OK;
}
