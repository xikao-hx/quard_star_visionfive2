#include "bh_ota_package_context.h"
#include "bh_ota_package_recovery.h"

#define BH_OTA_STAGE1 1U
#define BH_OTA_STAGE2 2U

static int32_t ota_package_stage1_precheck(const char *package_dir,
                                           const char *public_key_path,
                                           bh_ota_manifest_t *manifest,
                                           int32_t *target_bank_out)
{
    ota_package_context_t ctx;
    int32_t ret;

    ret = ota_package_load_context(package_dir, public_key_path, &ctx, 1, 1);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    ret = ota_package_load_current_bank(&ctx);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    ret = ota_package_validate_target_not_current(&ctx);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    *manifest = ctx.manifest;
    *target_bank_out = ctx.target_bank;
    return BH_OTA_OK;
}

static int32_t ota_package_stage1_commit_precheck(const char *package_dir,
                                                  const char *public_key_path,
                                                  bh_ota_manifest_t *manifest,
                                                  int32_t *target_bank_out)
{
    ota_package_context_t ctx;
    int32_t ret;

    ret = ota_package_load_context(package_dir, public_key_path, &ctx, 0, 0);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    ret = ota_package_load_current_bank(&ctx);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    ret = ota_package_validate_target_not_current(&ctx);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    *manifest = ctx.manifest;
    *target_bank_out = ctx.target_bank;
    return BH_OTA_OK;
}

static int32_t ota_assess_stage2_resume(const char *package_dir,
                                        const char *public_key_path,
                                        int32_t verify_payloads,
                                        ota_package_context_t *ctx,
                                        int32_t *should_resume_out)
{
    uint32_t normalized_state;
    int32_t ret;

    if (ctx == NULL || should_resume_out == NULL) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    /*
     * Cheap gate for stage2: answer "should we even try" before running the stricter
     * stage2 validation path. This keeps resume idempotent when the boot bank or state
     * is not ready yet.
     */
    *should_resume_out = 0;

    ret = ota_package_load_context(package_dir, public_key_path, ctx, 0, verify_payloads);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    ret = ota_package_load_current_bank(ctx);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    if (ctx->total_stage1_payloads == 0U || ctx->total_stage2_payloads == 0U) {
        PACKAGE_LOG_INFO("Stage-2 resume not required: OTA package does not contain both stage-1 and stage-2 payloads");
        return BH_OTA_OK;
    }

    ret = ota_recovery_load(&ctx->recovery);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    if (ctx->current_bank != ctx->target_bank) {
        PACKAGE_LOG_INFO("Stage-2 resume not required: current bank=%c target bank=%c",
                         ctx->current_bank == 1 ? 'b' : 'a',
                         ctx->target_bank == 1 ? 'b' : 'a');
        return BH_OTA_OK;
    }

    if (ctx->recovery.target_bank != (uint32_t)(ctx->target_bank & 0x1)) {
        PACKAGE_LOG_INFO("Stage-2 resume not required: recovery target_bank=%s(%u) does not match manifest target bank=%s(%d)",
                         ota_bank_to_string((int32_t)ctx->recovery.target_bank),
                         ctx->recovery.target_bank,
                         ota_bank_to_string(ctx->target_bank),
                         ctx->target_bank);
        return BH_OTA_OK;
    }

    normalized_state = ota_recovery_state(&ctx->recovery);
    if (normalized_state != BH_OTA_STATE_STAGE1_END &&
        normalized_state != BH_OTA_STATE_STAGE2_START) {
        PACKAGE_LOG_INFO("Stage-2 resume not required: ota_state=%s(%u)",
                         ota_state_to_string(normalized_state),
                         ctx->recovery.ota_state);
        return BH_OTA_OK;
    }

    if (!ota_recovery_has_valid_session(&ctx->recovery, &ctx->manifest)) {
        PACKAGE_LOG_ERROR("Stage-2 resume skipped: session is incomplete or session_id CRC is invalid");
        return BH_OTA_OK;
    }

    if (ctx->recovery.boot_success != 0U) {
        PACKAGE_LOG_INFO("Stage-2 resume not required: boot_success=%u", ctx->recovery.boot_success);
        return BH_OTA_OK;
    }

    if (normalized_state == BH_OTA_STATE_STAGE1_END &&
        ctx->recovery.ota_update != (uint32_t)ctx->total_stage1_payloads) {
        PACKAGE_LOG_ERROR("Stage-2 resume skipped: expected stage-1 ota_update=%zu, actual=%u",
                          ctx->total_stage1_payloads,
                          ctx->recovery.ota_update);
        return BH_OTA_OK;
    }

    *should_resume_out = 1;
    PACKAGE_LOG_INFO("Stage-2 resume recognized: current bank=%c session_id='%s' ota_state=%s",
                     ctx->current_bank == 1 ? 'b' : 'a',
                     ctx->recovery.session_id,
                     ota_state_to_string(normalized_state));
    return BH_OTA_OK;
}

static int32_t ota_assess_boot_success_pending(const char *package_dir,
                                               const char *public_key_path,
                                               ota_package_context_t *ctx,
                                               int32_t *should_mark_out)
{
    int32_t ret;

    if (ctx == NULL || should_mark_out == NULL) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    /* Detect the final "stage2_end and rebooted back" window before writing complete. */
    *should_mark_out = 0;

    ret = ota_package_load_context(package_dir, public_key_path, ctx, 0, 0);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    ret = ota_validate_boot_success_ready(&ctx->manifest,
                                          ctx->target_bank,
                                          ctx->total_stage2_payloads,
                                          &ctx->recovery,
                                          &ctx->current_bank);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    if (ota_recovery_state(&ctx->recovery) != BH_OTA_STATE_STAGE2_END) {
        return BH_OTA_OK;
    }

    *should_mark_out = 1;
    PACKAGE_LOG_INFO("Boot success confirmation recognized: current bank=%c session_id='%s'",
                     ctx->current_bank == 1 ? 'b' : 'a',
                     ctx->recovery.session_id);
    return BH_OTA_OK;
}

int32_t bh_ota_package_verify_stage1(const char *package_dir,
                                     const char *public_key_path,
                                     bh_ota_manifest_t *manifest_out,
                                     int32_t *target_bank_out)
{
    bh_ota_manifest_t manifest;
    int32_t target_bank = -1;
    int32_t ret;

    ret = ota_package_stage1_precheck(package_dir, public_key_path, &manifest, &target_bank);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    if (manifest_out != NULL) {
        *manifest_out = manifest;
    }

    if (target_bank_out != NULL) {
        *target_bank_out = target_bank;
    }

    PACKAGE_LOG_INFO("Stage-1 verify success: target bank=%c", target_bank == 1 ? 'b' : 'a');
    return BH_OTA_OK;
}

int32_t bh_ota_package_commit_stage1(const char *package_dir,
                                     const char *public_key_path,
                                     int32_t *target_bank_out)
{
    bh_ota_manifest_t manifest;
    recovery_config_t recovery;
    int32_t target_bank = -1;
    int32_t ret;
    size_t total_stage1_payloads;

    ret = ota_package_stage1_commit_precheck(package_dir, public_key_path, &manifest, &target_bank);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    total_stage1_payloads = ota_count_stage_payloads(&manifest, BH_OTA_STAGE1);
    ret = ota_validate_stage1_commit_ready(&manifest, target_bank, total_stage1_payloads, &recovery);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    ret = ota_commit_stage1_recovery(&recovery, target_bank, total_stage1_payloads);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    if (target_bank_out != NULL) {
        *target_bank_out = target_bank;
    }

    return BH_OTA_OK;
}

int32_t bh_ota_package_verify_stage2(const char *package_dir,
                                     const char *public_key_path,
                                     int32_t *current_bank_out)
{
    ota_package_context_t ctx;
    uint32_t normalized_state;
    int32_t should_resume = 0;
    int32_t ret;

    if (current_bank_out != NULL) {
        *current_bank_out = -1;
    }

    ret = ota_assess_stage2_resume(package_dir, public_key_path, 1, &ctx, &should_resume);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    if (current_bank_out != NULL) {
        *current_bank_out = ctx.current_bank;
    }

    normalized_state = ota_recovery_state(&ctx.recovery);
    if (normalized_state == BH_OTA_STATE_STAGE1_END) {
        if (ctx.current_bank != ctx.target_bank) {
            PACKAGE_LOG_INFO("stage=stage2_verify result=waiting current_bank=%c target_bank=%c",
                             ctx.current_bank == 1 ? 'b' : 'a',
                             ctx.target_bank == 1 ? 'b' : 'a');
            return BH_OTA_OK;
        }

        ota_recovery_set_state(&ctx.recovery, BH_OTA_STATE_STAGE2_START);
        ctx.recovery.ota_update = 0U;
        ctx.recovery.ota_reboot_cnt = 0U;
        ctx.recovery.successful_bank_mask |= 1U << (ctx.current_bank & 0x1);
        ret = ota_recovery_store(&ctx.recovery);
        if (ret != BH_OTA_OK) {
            return ret;
        }
    }

    if (should_resume == 0) {
        return BH_OTA_OK;
    }

    ret = ota_validate_stage2_resume_ready(&ctx.manifest,
                                           ctx.target_bank,
                                           ctx.total_stage1_payloads,
                                           ctx.total_stage2_payloads,
                                           &ctx.recovery,
                                           &ctx.completed_stage2_payloads);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    PACKAGE_LOG_INFO("Stage-2 verify success: current bank=%c completed=%zu/%zu",
                     ctx.current_bank == 1 ? 'b' : 'a',
                     ctx.completed_stage2_payloads,
                     ctx.total_stage2_payloads);
    return BH_OTA_OK;
}

int32_t bh_ota_package_apply_stage2(const char *package_dir,
                                    const char *public_key_path,
                                    int32_t *current_bank_out)
{
    ota_package_context_t ctx;
    int32_t should_resume = 0;
    int32_t ret;
    size_t stage2_abort_after = 0U;

    if (current_bank_out != NULL) {
        *current_bank_out = -1;
    }

    ret = ota_assess_stage2_resume(package_dir, public_key_path, 1, &ctx, &should_resume);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    if (current_bank_out != NULL) {
        *current_bank_out = ctx.current_bank;
    }

    if (should_resume == 0) {
        return BH_OTA_OK;
    }

    ret = ota_validate_stage2_resume_ready(&ctx.manifest,
                                           ctx.target_bank,
                                           ctx.total_stage1_payloads,
                                           ctx.total_stage2_payloads,
                                           &ctx.recovery,
                                           &ctx.completed_stage2_payloads);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    ret = ota_env_limit("BH_OTA_STAGE2_ABORT_AFTER", &stage2_abort_after);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    ret = ota_prepare_stage2_recovery(&ctx.recovery, ctx.current_bank, ctx.completed_stage2_payloads);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    ret = ota_apply_stage_payloads(package_dir,
                                   &ctx.manifest,
                                   BH_OTA_STAGE2,
                                   ctx.completed_stage2_payloads,
                                   ctx.total_stage2_payloads,
                                   stage2_abort_after,
                                   &ctx.completed_stage2_payloads);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    ret = ota_complete_stage2_recovery(&ctx.recovery, ctx.current_bank, ctx.total_stage2_payloads);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    return BH_OTA_OK;
}

int32_t bh_ota_package_commit_stage2(const char *package_dir,
                                     const char *public_key_path,
                                     int32_t *current_bank_out)
{
    ota_package_context_t ctx;
    int32_t ret;

    if (current_bank_out != NULL) {
        *current_bank_out = -1;
    }

    ret = ota_package_load_context(package_dir, public_key_path, &ctx, 0, 0);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    ret = ota_package_load_current_bank(&ctx);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    if (current_bank_out != NULL) {
        *current_bank_out = ctx.current_bank;
    }

    ret = ota_validate_stage2_commit_ready(&ctx.manifest,
                                           ctx.current_bank,
                                           ctx.total_stage2_payloads,
                                           &ctx.recovery);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    return ota_commit_stage2_recovery(&ctx.recovery, ctx.current_bank, ctx.total_stage2_payloads);
}

int32_t bh_ota_package_boot_success(const char *package_dir,
                                    const char *public_key_path,
                                    int32_t *current_bank_out)
{
    ota_package_context_t ctx;
    int32_t should_mark = 0;
    int32_t ret;

    if (current_bank_out != NULL) {
        *current_bank_out = -1;
    }

    ret = ota_assess_boot_success_pending(package_dir, public_key_path, &ctx, &should_mark);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    if (current_bank_out != NULL) {
        *current_bank_out = ctx.current_bank;
    }

    if (should_mark == 0) {
        return BH_OTA_OK;
    }

    return ota_commit_boot_success(&ctx.recovery, ctx.current_bank);
}

int32_t bh_ota_package_apply_stage1(const char *package_dir,
                                    const char *public_key_path,
                                    int32_t *target_bank_out)
{
    bh_ota_manifest_t manifest;
    recovery_config_t recovery;
    int32_t target_bank = -1;
    int32_t ret;
    size_t total_stage1_payloads;
    size_t completed_stage1_payloads = 0U;
    size_t abort_after = 0U;

    ret = ota_package_stage1_precheck(package_dir, public_key_path, &manifest, &target_bank);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    /* Write-ahead session metadata must be persisted before any stage-1 payload is touched. */
    ret = ota_write_stage1_session(&manifest);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    total_stage1_payloads = ota_count_stage_payloads(&manifest, BH_OTA_STAGE1);
    if (total_stage1_payloads == 0U) {
        PACKAGE_LOG_ERROR("OTA package does not contain any stage-1 payload");
        return BH_OTA_ERROR;
    }

    ret = ota_env_limit("BH_OTA_STAGE1_ABORT_AFTER", &abort_after);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    ret = ota_apply_stage_payloads(package_dir,
                                   &manifest,
                                   BH_OTA_STAGE1,
                                   0U,
                                   total_stage1_payloads,
                                   abort_after,
                                   &completed_stage1_payloads);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    if (ota_recovery_load(&recovery) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }
    ota_recovery_set_state(&recovery, BH_OTA_STATE_STAGE1_WROTE);
    recovery.ota_update = (uint32_t)completed_stage1_payloads;
    if (ota_recovery_store(&recovery) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    if (target_bank_out != NULL) {
        *target_bank_out = target_bank;
    }

    PACKAGE_LOG_INFO("Stage-1 payload write completed. target bank remains unchanged until commit_stage1. prepared bank=%c",
                     target_bank == 1 ? 'b' : 'a');
    return BH_OTA_OK;
}
