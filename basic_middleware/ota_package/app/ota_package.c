#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bh_ota_package.h"
#include "bh_ota_log.h"

typedef enum {
    OPT_VERIFY_STAGE1,
    OPT_APPLY_STAGE1,
    OPT_COMMIT_STAGE1,
    OPT_VERIFY_STAGE2,
    OPT_APPLY_STAGE2,
    OPT_COMMIT_STAGE2,
    OPT_BOOT_SUCCESS,
} option_t;

typedef struct {
    const char *name;
    option_t option;
} ota_command_entry_t;

#define PROGRAM_NAME "ota_package"

static const ota_command_entry_t g_ota_commands[] = {
    {"verify_stage1", OPT_VERIFY_STAGE1},
    {"apply_stage1", OPT_APPLY_STAGE1},
    {"commit_stage1", OPT_COMMIT_STAGE1},
    {"verify_stage2", OPT_VERIFY_STAGE2},
    {"apply_stage2", OPT_APPLY_STAGE2},
    {"commit_stage2", OPT_COMMIT_STAGE2},
    {"boot_success", OPT_BOOT_SUCCESS},
};

static void showusage(void)
{
    fprintf(stderr, "usage:\n"
            "       %s verify_stage1 <package_dir> <public_key.pem>\n"
            "       %s apply_stage1 <package_dir> <public_key.pem>\n"
            "       %s commit_stage1 <package_dir> <public_key.pem>\n"
            "       %s verify_stage2 <package_dir> <public_key.pem>\n"
            "       %s apply_stage2 <package_dir> <public_key.pem>\n"
            "       %s commit_stage2 <package_dir> <public_key.pem>\n"
            "       %s boot_success <package_dir> <public_key.pem>\n",
            PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME,
            PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME);
}

static const char *ota_bank_cli_name(int32_t bank)
{
    if (bank == 0) {
        return "a";
    }
    if (bank == 1) {
        return "b";
    }
    return "unknown";
}

static int32_t ota_package_verify_stage1_cmd(const char *package_dir, const char *public_key_path)
{
    int32_t target_bank = -1;
    int32_t ret;

    ret = bh_ota_package_verify_stage1(package_dir, public_key_path, NULL, &target_bank);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    BH_OTA_LOG_INFO("cmd=verify_stage1 result=success target_bank=%c",
                    target_bank == 1 ? 'b' : 'a');
    return BH_OTA_OK;
}

static int32_t ota_package_apply_stage1_cmd(const char *package_dir,
                                            const char *public_key_path)
{
    int32_t target_bank = -1;
    int32_t ret;

    ret = bh_ota_package_apply_stage1(package_dir, public_key_path, &target_bank);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    BH_OTA_LOG_INFO("cmd=apply_stage1 result=success prepared_target_bank=%c",
                    target_bank == 1 ? 'b' : 'a');

    return BH_OTA_OK;
}

static int32_t ota_package_commit_stage1_cmd(const char *package_dir,
                                             const char *public_key_path)
{
    int32_t target_bank = -1;
    int32_t ret;

    ret = bh_ota_package_commit_stage1(package_dir, public_key_path, &target_bank);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    BH_OTA_LOG_INFO("cmd=commit_stage1 result=success next_boot_bank=%c",
                    target_bank == 1 ? 'b' : 'a');
    return BH_OTA_OK;
}

static int32_t ota_package_verify_stage2_cmd(const char *package_dir,
                                             const char *public_key_path)
{
    int32_t current_bank = -1;
    int32_t ret;

    ret = bh_ota_package_verify_stage2(package_dir, public_key_path, &current_bank);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    BH_OTA_LOG_INFO("cmd=verify_stage2 result=success current_bank=%s",
                    ota_bank_cli_name(current_bank));
    return BH_OTA_OK;
}

static int32_t ota_package_apply_stage2_cmd(const char *package_dir,
                                            const char *public_key_path)
{
    int32_t current_bank = -1;
    int32_t ret;

    ret = bh_ota_package_apply_stage2(package_dir, public_key_path, &current_bank);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    BH_OTA_LOG_INFO("cmd=apply_stage2 result=success current_bank=%s",
                    ota_bank_cli_name(current_bank));
    return BH_OTA_OK;
}

static int32_t ota_package_commit_stage2_cmd(const char *package_dir,
                                             const char *public_key_path)
{
    int32_t current_bank = -1;
    int32_t ret;

    ret = bh_ota_package_commit_stage2(package_dir, public_key_path, &current_bank);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    BH_OTA_LOG_INFO("cmd=commit_stage2 result=success current_bank=%s",
                    ota_bank_cli_name(current_bank));
    return BH_OTA_OK;
}

static int32_t ota_package_boot_success_cmd(const char *package_dir,
                                            const char *public_key_path)
{
    int32_t current_bank = -1;
    int32_t ret;

    ret = bh_ota_package_boot_success(package_dir, public_key_path, &current_bank);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    BH_OTA_LOG_INFO("cmd=boot_success result=success current_bank=%s",
                    ota_bank_cli_name(current_bank));
    return BH_OTA_OK;
}

static int32_t ota_package_parse_option(int argc, char *argv[], option_t *option_out)
{
    size_t i;

    if (argc != 4 || argv == NULL || option_out == NULL) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    for (i = 0; i < sizeof(g_ota_commands) / sizeof(g_ota_commands[0]); ++i) {
        if (strcmp(argv[1], g_ota_commands[i].name) == 0) {
            *option_out = g_ota_commands[i].option;
            return BH_OTA_OK;
        }
    }

    return BH_OTA_ERROR_INVALID_PARAM;
}

int main(int argc, char *argv[])
{
    option_t option;
    int32_t err;

    err = ota_package_parse_option(argc, argv, &option);
    if (err != BH_OTA_OK) {
        showusage();
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    err = bh_hal_ota_init();
    if (err != BH_OTA_OK) {
        return err;
    }

    switch (option) {
    case OPT_VERIFY_STAGE1:
        err = ota_package_verify_stage1_cmd(argv[2], argv[3]);
        break;
    case OPT_APPLY_STAGE1:
        err = ota_package_apply_stage1_cmd(argv[2], argv[3]);
        break;
    case OPT_COMMIT_STAGE1:
        err = ota_package_commit_stage1_cmd(argv[2], argv[3]);
        break;
    case OPT_VERIFY_STAGE2:
        err = ota_package_verify_stage2_cmd(argv[2], argv[3]);
        break;
    case OPT_APPLY_STAGE2:
        err = ota_package_apply_stage2_cmd(argv[2], argv[3]);
        break;
    case OPT_COMMIT_STAGE2:
        err = ota_package_commit_stage2_cmd(argv[2], argv[3]);
        break;
    case OPT_BOOT_SUCCESS:
        err = ota_package_boot_success_cmd(argv[2], argv[3]);
        break;
    default:
        err = BH_OTA_ERROR_INVALID_PARAM;
        break;
    }

    return err;
}
