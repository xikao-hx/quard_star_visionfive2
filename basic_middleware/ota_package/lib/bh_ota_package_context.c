#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "bh_ota_package_context.h"
#include "bh_ota_package_recovery.h"

typedef struct ota_partition_order_rule {
    const char *prefix;
    size_t prefix_len;
    int32_t order;
} ota_partition_order_rule_t;

static const ota_partition_order_rule_t g_partition_order_rules[] = {
    {"sbi_dtb_", 8U, 10},
    {"uboot_dtb_", 10U, 20},
    {"bl1_", 4U, 30},
    {"opensbi_", 8U, 40},
    {"trusted_fw_", 11U, 50},
    {"u-boot_", 7U, 60},
    {"fw_payload_", 11U, 70},
    {"boot_", 5U, 80},
    {"rootfs_", 7U, 90},
    {"spl_", 4U, 100},
};

static int32_t ota_path_join(char *out, size_t out_len, const char *dir, const char *file)
{
    int written;

    if (out == NULL || dir == NULL || file == NULL || out_len == 0U) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    written = snprintf(out, out_len, "%s/%s", dir, file);
    if (written <= 0 || (size_t)written >= out_len) {
        PACKAGE_LOG_ERROR("Path is too long: dir='%s' file='%s'", dir, file);
        return BH_OTA_ERROR;
    }

    return BH_OTA_OK;
}

static int32_t ota_partition_get_bank(const char *partition, int32_t *bank_out)
{
    size_t len;

    if (partition == NULL || bank_out == NULL) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    len = strlen(partition);
    if (len < 2U || partition[len - 2] != '_') {
        PACKAGE_LOG_ERROR("Partition '%s' does not follow *_a/*_b naming", partition);
        return BH_OTA_ERROR;
    }

    if (partition[len - 1] == 'a') {
        *bank_out = 0;
        return BH_OTA_OK;
    }

    if (partition[len - 1] == 'b') {
        *bank_out = 1;
        return BH_OTA_OK;
    }

    PACKAGE_LOG_ERROR("Partition '%s' does not follow *_a/*_b naming", partition);
    return BH_OTA_ERROR;
}

static int32_t ota_partition_get_order(const char *partition)
{
    size_t i;

    if (partition == NULL) {
        return -1;
    }

    for (i = 0; i < sizeof(g_partition_order_rules) / sizeof(g_partition_order_rules[0]); ++i) {
        if (strncmp(partition,
                    g_partition_order_rules[i].prefix,
                    g_partition_order_rules[i].prefix_len) == 0) {
            return g_partition_order_rules[i].order;
        }
    }

    return -1;
}

static void ota_package_context_reset(ota_package_context_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->target_bank = -1;
    ctx->current_bank = -1;
}

static const char *ota_version_path(void)
{
    const char *override = getenv("BH_OTA_VERSION_FILE");

    if (override != NULL && override[0] != '\0') {
        return override;
    }

    return BH_OTA_SYSTEM_VERSION_PATH;
}

static int32_t ota_payload_is_rootfs(const char *partition)
{
    return partition != NULL && strncmp(partition, "rootfs_", 7) == 0;
}

static int32_t ota_precheck_verify_payloads(bh_ota_manifest_t *manifest,
                                            const char *package_dir)
{
    char payload_path[BH_OTA_PATH_MAX];
    char actual_sha256[BH_OTA_SHA256_HEX_LEN + 1U];
    struct stat st;
    size_t i;
    int written;
    int32_t ret;

    if (manifest == NULL || package_dir == NULL || package_dir[0] == '\0') {
        PACKAGE_LOG_ERROR("Payload verification received invalid parameter");
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    for (i = 0; i < manifest->payload_count; ++i) {
        const bh_ota_manifest_payload_t *payload = &manifest->payloads[i];

        written = snprintf(payload_path, sizeof(payload_path), "%s/%s", package_dir, payload->file);
        if (written < 0 || (size_t)written >= sizeof(payload_path)) {
            PACKAGE_LOG_ERROR("Payload path is too long for file '%s'", payload->file);
            return BH_OTA_ERROR;
        }

        if (stat(payload_path, &st) != 0) {
            PACKAGE_LOG_ERROR("Payload file '%s' is missing", payload_path);
            return BH_OTA_ERROR_READ;
        }

        if (!S_ISREG(st.st_mode)) {
            PACKAGE_LOG_ERROR("Payload path '%s' is not a regular file", payload_path);
            return BH_OTA_ERROR;
        }

        if ((uint64_t)st.st_size != (uint64_t)payload->size) {
            PACKAGE_LOG_ERROR("Payload size mismatch for '%s': manifest=%u actual=%lld",
                              payload->file,
                              payload->size,
                              (long long)st.st_size);
            return BH_OTA_ERROR;
        }

        if (ota_payload_is_rootfs(payload->partition)) {
            PACKAGE_LOG_INFO("Payload SHA-256 precheck skipped for rootfs payload: partition='%s' file='%s' size=%u",
                             payload->partition,
                             payload->file,
                             payload->size);
            continue;
        }

        ret = bh_ota_sha256_file_hex(payload_path, actual_sha256, sizeof(actual_sha256));
        if (ret != BH_OTA_OK) {
            return ret;
        }

        if (strcmp(actual_sha256, payload->sha256) != 0) {
            PACKAGE_LOG_ERROR("Payload SHA-256 mismatch for '%s': manifest=%s actual=%s",
                              payload->file,
                              payload->sha256,
                              actual_sha256);
            return BH_OTA_ERROR;
        }

        PACKAGE_LOG_INFO("Payload verified: partition='%s' file='%s' size=%u partition_size=%u sha256=%s",
                         payload->partition,
                         payload->file,
                         payload->size,
                         payload->partition_size,
                         payload->sha256);
    }

    manifest->payload_hashes_verified = 1U;
    return BH_OTA_OK;
}

static int32_t ota_manifest_target_bank(const bh_ota_manifest_t *manifest, int32_t *target_bank_out)
{
    int32_t target_bank = -1;
    size_t i;

    if (manifest == NULL || target_bank_out == NULL) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    for (i = 0; i < manifest->payload_count; ++i) {
        int32_t payload_bank;

        if (manifest->payloads[i].stage != 1U) {
            continue;
        }

        if (ota_partition_get_bank(manifest->payloads[i].partition, &payload_bank) != BH_OTA_OK) {
            return BH_OTA_ERROR;
        }

        if (ota_partition_get_order(manifest->payloads[i].partition) < 0) {
            PACKAGE_LOG_ERROR("Unsupported payload partition '%s'", manifest->payloads[i].partition);
            return BH_OTA_ERROR;
        }

        if (target_bank < 0) {
            target_bank = payload_bank;
        } else if (target_bank != payload_bank) {
            PACKAGE_LOG_ERROR("Mixed-bank OTA package is not supported in current apply flow");
            return BH_OTA_ERROR;
        }
    }

    if (target_bank < 0) {
        PACKAGE_LOG_ERROR("Failed to infer target bank from stage-1 OTA payloads");
        return BH_OTA_ERROR;
    }

    *target_bank_out = target_bank;
    return BH_OTA_OK;
}

static int32_t ota_manifest_validate_stage_layout(const bh_ota_manifest_t *manifest,
                                                  int32_t stage1_bank)
{
    int32_t stage2_bank = -1;
    size_t i;

    if (manifest == NULL) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    for (i = 0; i < manifest->payload_count; ++i) {
        int32_t payload_bank;

        if (ota_partition_get_bank(manifest->payloads[i].partition, &payload_bank) != BH_OTA_OK) {
            return BH_OTA_ERROR;
        }

        if (manifest->payloads[i].stage == 1U) {
            if (payload_bank != stage1_bank) {
                PACKAGE_LOG_ERROR("Stage-1 payload '%s' does not belong to inferred target bank",
                                  manifest->payloads[i].partition);
                return BH_OTA_ERROR;
            }
            continue;
        }

        if (manifest->payloads[i].stage != 2U) {
            PACKAGE_LOG_ERROR("Unsupported payload stage=%u for partition '%s'",
                              manifest->payloads[i].stage,
                              manifest->payloads[i].partition);
            return BH_OTA_ERROR;
        }

        if (stage2_bank < 0) {
            stage2_bank = payload_bank;
        } else if (stage2_bank != payload_bank) {
            PACKAGE_LOG_ERROR("Mixed-bank stage-2 OTA payloads are not supported");
            return BH_OTA_ERROR;
        }
    }

    if (stage2_bank >= 0 && stage2_bank == stage1_bank) {
        PACKAGE_LOG_ERROR("Stage-2 OTA payloads must target the opposite bank");
        return BH_OTA_ERROR;
    }

    return BH_OTA_OK;
}

static int32_t ota_record_progress(uint32_t state,
                                   size_t completed_count,
                                   size_t total_count,
                                   const char *partition)
{
    recovery_config_t recovery;

    if (partition == NULL) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    if (ota_recovery_load(&recovery) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    ota_recovery_set_state(&recovery, state);
    recovery.ota_update = (uint32_t)completed_count;

    if (ota_recovery_store(&recovery) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    PACKAGE_LOG_INFO("%s progress %zu/%zu completed: partition='%s'",
                     state == BH_OTA_STATE_STAGE2_START ? "Stage-2" : "Stage-1",
                     completed_count,
                     total_count,
                     partition);
    return BH_OTA_OK;
}

int32_t ota_env_limit(const char *env_name, size_t *limit_out)
{
    const char *env_value;
    char *endptr;
    unsigned long parsed;

    if (env_name == NULL || limit_out == NULL) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    *limit_out = 0U;
    env_value = getenv(env_name);
    if (env_value == NULL || env_value[0] == '\0') {
        return BH_OTA_OK;
    }

    errno = 0;
    parsed = strtoul(env_value, &endptr, 10);
    if (errno != 0 || endptr == env_value || *endptr != '\0') {
        PACKAGE_LOG_ERROR("Invalid %s='%s'", env_name, env_value);
        return BH_OTA_ERROR;
    }

    *limit_out = (size_t)parsed;
    return BH_OTA_OK;
}

size_t ota_count_stage_payloads(const bh_ota_manifest_t *manifest, uint32_t stage)
{
    size_t count = 0U;
    size_t i;

    if (manifest == NULL) {
        return 0U;
    }

    for (i = 0; i < manifest->payload_count; ++i) {
        if (manifest->payloads[i].stage == stage) {
            ++count;
        }
    }

    return count;
}

int32_t ota_package_load_context(const char *package_dir,
                                 const char *public_key_path,
                                 ota_package_context_t *ctx,
                                 int32_t verify_version,
                                 int32_t verify_payloads)
{
    int32_t compare_result = 0;
    char manifest_path[BH_OTA_PATH_MAX];
    char signature_path[BH_OTA_PATH_MAX];
    int32_t ret;

    if (package_dir == NULL || public_key_path == NULL || ctx == NULL) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    /* Build a reusable execution context: manifest, signature, target bank and stage counts. */
    ota_package_context_reset(ctx);

    if (ota_path_join(manifest_path, sizeof(manifest_path), package_dir, BH_OTA_MANIFEST_FILE_NAME) != BH_OTA_OK ||
        ota_path_join(signature_path, sizeof(signature_path), package_dir, BH_OTA_SIGNATURE_FILE_NAME) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    ret = bh_ota_manifest_parse_file(manifest_path, &ctx->manifest);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    if (verify_version != 0) {
        ret = bh_ota_manifest_check_version_file(&ctx->manifest, ota_version_path(), &compare_result);
        if (ret != BH_OTA_OK || compare_result < 0) {
            return BH_OTA_ERROR;
        }
    }

    if (verify_payloads != 0) {
        ret = ota_precheck_verify_payloads(&ctx->manifest, package_dir);
        if (ret != BH_OTA_OK) {
            return ret;
        }
    } else {
        ctx->manifest.payload_hashes_verified = 1U;
    }

    ret = bh_ota_manifest_verify_signature(&ctx->manifest, package_dir, signature_path, public_key_path);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    ret = ota_manifest_target_bank(&ctx->manifest, &ctx->target_bank);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    ret = ota_manifest_validate_stage_layout(&ctx->manifest, ctx->target_bank);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    ctx->total_stage1_payloads = ota_count_stage_payloads(&ctx->manifest, 1U);
    ctx->total_stage2_payloads = ota_count_stage_payloads(&ctx->manifest, 2U);
    return BH_OTA_OK;
}

int32_t ota_package_load_current_bank(ota_package_context_t *ctx)
{
    if (ctx == NULL) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    if (bh_hal_ota_get_current_bank(&ctx->current_bank) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    return BH_OTA_OK;
}

int32_t ota_package_validate_target_not_current(const ota_package_context_t *ctx)
{
    if (ctx == NULL) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    if (ctx->current_bank == ctx->target_bank) {
        PACKAGE_LOG_ERROR("Refusing to apply OTA package onto current running bank %c",
                          ctx->target_bank == 1 ? 'b' : 'a');
        return BH_OTA_ERROR;
    }

    return BH_OTA_OK;
}

int32_t ota_apply_stage_payloads(const char *package_dir,
                                 const bh_ota_manifest_t *manifest,
                                 uint32_t stage,
                                 size_t completed_payloads,
                                 size_t total_payloads,
                                 size_t abort_after,
                                 size_t *completed_out)
{
    size_t completed_count;
    size_t remaining_skip;
    size_t i;
    size_t rule_index;
    int32_t ret;

    if (package_dir == NULL || manifest == NULL || completed_out == NULL) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    /* Apply payloads in fixed partition order so resume can continue deterministically. */
    completed_count = completed_payloads;
    remaining_skip = completed_payloads;
    for (rule_index = 0; rule_index < sizeof(g_partition_order_rules) / sizeof(g_partition_order_rules[0]); ++rule_index) {
        for (i = 0; i < manifest->payload_count; ++i) {
            if (manifest->payloads[i].stage != stage) {
                continue;
            }

            if (ota_partition_get_order(manifest->payloads[i].partition) != g_partition_order_rules[rule_index].order) {
                continue;
            }

            if (remaining_skip > 0U) {
                --remaining_skip;
                continue;
            }

            ret = bh_ota_package_apply_payload(package_dir, &manifest->payloads[i]);
            if (ret != BH_OTA_OK) {
                return ret;
            }

            ++completed_count;
            ret = ota_record_progress(stage == 2U ? BH_OTA_STATE_STAGE2_START
                                                  : BH_OTA_STATE_STAGE1_START,
                                      completed_count,
                                      total_payloads,
                                      manifest->payloads[i].partition);
            if (ret != BH_OTA_OK) {
                return ret;
            }

            if (abort_after > 0U && completed_count >= abort_after) {
                PACKAGE_LOG_ERROR("%s apply aborted intentionally after %zu payload(s) for recovery testing",
                                  stage == 2U ? "Stage-2" : "Stage-1",
                                  completed_count);
                return BH_OTA_ERROR;
            }
        }
    }

    *completed_out = completed_count;
    return BH_OTA_OK;
}
