#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "bh_ota_package.h"

static void write_text_file(const char *path, const char *content)
{
    FILE *fp = fopen(path, "wb");
    assert(fp != NULL);
    assert(fwrite(content, 1, strlen(content), fp) == strlen(content));
    fclose(fp);
}

static void truncate_file(const char *path, size_t size)
{
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);
    assert(ftruncate(fd, (off_t)size) == 0);
    close(fd);
}

static void write_padded_file(const char *path, const char *content, size_t total_size)
{
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    size_t content_len = strlen(content);

    assert(fd >= 0);
    assert(content_len <= total_size);
    assert(write(fd, content, content_len) == (ssize_t)content_len);
    assert(ftruncate(fd, (off_t)total_size) == 0);
    close(fd);
}

static void create_dir(const char *path)
{
    assert(mkdir(path, 0755) == 0);
}

static void run_cmd_or_die(const char *cmd)
{
    int ret = system(cmd);
    assert(ret == 0);
}

static void join_path(char *out, size_t out_len, const char *dir, const char *file)
{
    int written = snprintf(out, out_len, "%s/%s", dir, file);
    assert(written > 0 && (size_t)written < out_len);
}

static void assert_file_equals(const char *lhs, const char *rhs)
{
    FILE *fp_lhs = fopen(lhs, "rb");
    FILE *fp_rhs = fopen(rhs, "rb");
    int ch_lhs;
    int ch_rhs;

    assert(fp_lhs != NULL);
    assert(fp_rhs != NULL);

    while (1) {
        ch_lhs = fgetc(fp_lhs);
        ch_rhs = fgetc(fp_rhs);
        assert(ch_lhs == ch_rhs);
        if (ch_lhs == EOF) {
            break;
        }
    }

    fclose(fp_lhs);
    fclose(fp_rhs);
}

static void assert_file_not_equals(const char *lhs, const char *rhs)
{
    FILE *fp_lhs = fopen(lhs, "rb");
    FILE *fp_rhs = fopen(rhs, "rb");
    int ch_lhs;
    int ch_rhs;
    int different = 0;

    assert(fp_lhs != NULL);
    assert(fp_rhs != NULL);

    while (1) {
        ch_lhs = fgetc(fp_lhs);
        ch_rhs = fgetc(fp_rhs);
        if (ch_lhs != ch_rhs) {
            different = 1;
            break;
        }
        if (ch_lhs == EOF) {
            break;
        }
    }

    fclose(fp_lhs);
    fclose(fp_rhs);
    assert(different);
}

int main(void)
{
    char temp_dir[] = "/tmp/test_bh_ota_package_applyXXXXXX";
    char src_dir[PATH_MAX];
    char pkg_dir[PATH_MAX];
    char fake_mtd_dir[PATH_MAX];
    char private_key[PATH_MAX];
    char public_key[PATH_MAX];
    char version_file[PATH_MAX];
    char manifest_path[PATH_MAX];
    char boot_payload[PATH_MAX];
    char rootfs_payload[PATH_MAX];
    char boot_stage2_payload[PATH_MAX];
    char rootfs_stage2_payload[PATH_MAX];
    char boot_a_dev[PATH_MAX];
    char boot_dev[PATH_MAX];
    char rootfs_a_dev[PATH_MAX];
    char rootfs_dev[PATH_MAX];
    char cmd[32768];
    recovery_config_t recovery;
    bh_ota_manifest_t manifest;
    int32_t current_bank = -1;
    int32_t target_bank = -1;

    assert(mkdtemp(temp_dir) != NULL);

    join_path(src_dir, sizeof(src_dir), temp_dir, "src");
    join_path(pkg_dir, sizeof(pkg_dir), temp_dir, "pkg");
    join_path(fake_mtd_dir, sizeof(fake_mtd_dir), temp_dir, "mtd");
    join_path(private_key, sizeof(private_key), temp_dir, "ota_private.pem");
    join_path(public_key, sizeof(public_key), temp_dir, "ota_public.pem");
    join_path(version_file, sizeof(version_file), temp_dir, "version");
    join_path(manifest_path, sizeof(manifest_path), pkg_dir, "data.json");
    join_path(boot_payload, sizeof(boot_payload), src_dir, "boot_b.img");
    join_path(rootfs_payload, sizeof(rootfs_payload), src_dir, "rootfs_b.img");
    join_path(boot_stage2_payload, sizeof(boot_stage2_payload), src_dir, "boot_a.img");
    join_path(rootfs_stage2_payload, sizeof(rootfs_stage2_payload), src_dir, "rootfs_a.img");
    join_path(boot_a_dev, sizeof(boot_a_dev), temp_dir, "boot_a.dev");
    join_path(boot_dev, sizeof(boot_dev), temp_dir, "boot_b.dev");
    join_path(rootfs_a_dev, sizeof(rootfs_a_dev), temp_dir, "rootfs_a.dev");
    join_path(rootfs_dev, sizeof(rootfs_dev), temp_dir, "rootfs_b.dev");

    create_dir(src_dir);
    create_dir(fake_mtd_dir);

    write_padded_file(boot_payload, "boot-b-payload\n", 4096U);
    write_padded_file(rootfs_payload, "rootfs-b-payload\n", 8192U);
    write_padded_file(boot_stage2_payload, "boot-a-stage2\n", 4096U);
    write_padded_file(rootfs_stage2_payload, "rootfs-a-stage2\n", 8192U);
    write_text_file(version_file, "1.2.3\n");

    truncate_file(boot_a_dev, 4096U);
    truncate_file(boot_dev, 4096U);
    truncate_file(rootfs_a_dev, 8192U);
    truncate_file(rootfs_dev, 8192U);

    assert(snprintf(cmd, sizeof(cmd),
                    "openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out %s >/dev/null 2>&1",
                    private_key) > 0);
    run_cmd_or_die(cmd);

    assert(snprintf(cmd, sizeof(cmd),
                    "openssl rsa -pubout -in %s -out %s >/dev/null 2>&1",
                    private_key, public_key) > 0);
    run_cmd_or_die(cmd);

    memset(&recovery, 0, sizeof(recovery));
    recovery.magic = BH_OTA_RECOVERY_MAGIC;
    recovery.struct_version = BH_OTA_RECOVERY_STRUCT_VERSION;
    recovery.usable_bank = 0x2;
    recovery.current_bank = 0;
    recovery.target_bank = 0;
    recovery.ota_state = BH_OTA_STATE_IDLE;
    recovery.successful_bank_mask = 0x1;
    recovery.boot_success = 1;

    assert(setenv("BH_OTA_SKIP_PARTITION_INIT", "1", 1) == 0);
    assert(setenv("BH_OTA_VERSION_FILE", version_file, 1) == 0);
    assert(setenv("BH_OTA_MTD_BY_NAME_DIR", fake_mtd_dir, 1) == 0);
    assert(setenv("BH_OTA_BOOT_A_DEV", boot_a_dev, 1) == 0);
    assert(setenv("BH_OTA_BOOT_B_DEV", boot_dev, 1) == 0);
    assert(setenv("BH_OTA_ROOTFS_A_DEV", rootfs_a_dev, 1) == 0);
    assert(setenv("BH_OTA_ROOTFS_B_DEV", rootfs_dev, 1) == 0);

    assert(bh_hal_ota_recovery_write(&recovery) == BH_OTA_OK);

    assert(snprintf(cmd, sizeof(cmd),
                    "python3 tools/script/build_ota_package.py "
                    "--output-dir %s "
                    "--private-key %s "
                    "--sys-version 1.2.4 "
                    "--rollback-index 10204 "
                    "--session-id ota-session-fixed "
                    "--payload rootfs_b:1=%s "
                    "--payload boot_a:2=%s "
                    "--payload boot_b:1=%s "
                    "--payload rootfs_a:2=%s",
                    pkg_dir, private_key, rootfs_payload, boot_stage2_payload,
                    boot_payload, rootfs_stage2_payload) > 0);
    run_cmd_or_die(cmd);

    assert(bh_ota_manifest_parse_file(manifest_path, &manifest) == BH_OTA_OK);
    assert(manifest.payload_count == 4U);
    assert(manifest.payloads[0].stage == 1U || manifest.payloads[0].stage == 2U);

    assert(bh_ota_package_commit_stage1(pkg_dir, public_key, &target_bank) != BH_OTA_OK);
    assert(bh_hal_ota_recovery_read(&recovery) == BH_OTA_OK);
    assert(recovery.target_bank == 0);
    assert(recovery.ota_state == BH_OTA_STATE_IDLE);

    assert(setenv("BH_OTA_MTD_BY_NAME_DIR", "/tmp/test_bh_ota_package_apply_missing_dir", 1) == 0);
    assert(bh_ota_package_apply_stage1(pkg_dir, public_key, &target_bank) != BH_OTA_OK);
    assert_file_not_equals(boot_payload, boot_dev);
    assert_file_not_equals(rootfs_payload, rootfs_dev);
    assert(setenv("BH_OTA_MTD_BY_NAME_DIR", fake_mtd_dir, 1) == 0);
    assert(bh_hal_ota_recovery_read(&recovery) == BH_OTA_OK);
    assert(recovery.target_bank == 0);

    assert(setenv("BH_OTA_STAGE1_ABORT_AFTER", "1", 1) == 0);
    assert(bh_ota_package_apply_stage1(pkg_dir, public_key, &target_bank) != BH_OTA_OK);
    assert(unsetenv("BH_OTA_STAGE1_ABORT_AFTER") == 0);
    assert(bh_hal_ota_recovery_read(&recovery) == BH_OTA_OK);
    assert(recovery.current_bank == 0);
    assert(recovery.target_bank == 0);
    assert(recovery.ota_state == BH_OTA_STATE_STAGE1_START);
    assert(recovery.ota_update == 1U);
    assert(recovery.usable_bank == 0x2U);
    assert_file_not_equals(boot_stage2_payload, boot_a_dev);
    assert_file_not_equals(rootfs_stage2_payload, rootfs_a_dev);

    assert(bh_ota_package_apply_stage1(pkg_dir, public_key, &target_bank) == BH_OTA_OK);
    assert(target_bank == 1);

    assert(bh_hal_ota_recovery_read(&recovery) == BH_OTA_OK);
    assert(recovery.current_bank == 0);
    assert(recovery.target_bank == 0);
    assert(recovery.ota_state == BH_OTA_STATE_STAGE1_WROTE);
    assert(recovery.ota_update == 2U);
    assert(recovery.usable_bank == 0x2U);

    assert_file_not_equals(boot_stage2_payload, boot_a_dev);
    assert_file_equals(boot_payload, boot_dev);
    assert_file_not_equals(rootfs_stage2_payload, rootfs_a_dev);
    assert_file_equals(rootfs_payload, rootfs_dev);

    assert(bh_ota_package_commit_stage1(pkg_dir, public_key, &target_bank) == BH_OTA_OK);
    assert(target_bank == 1);

    assert(bh_hal_ota_recovery_read(&recovery) == BH_OTA_OK);
    assert(recovery.current_bank == 0);
    assert(recovery.target_bank == 1);
    assert(recovery.ota_state == BH_OTA_STATE_STAGE1_END);
    assert(recovery.ota_update == 2U);
    assert(recovery.boot_success == 0U);
    assert(recovery.usable_bank == 0x3U);

    assert(bh_ota_package_commit_stage1(pkg_dir, public_key, &target_bank) == BH_OTA_OK);
    assert(target_bank == 1);
    assert(bh_hal_ota_recovery_read(&recovery) == BH_OTA_OK);
    assert(recovery.target_bank == 1);
    assert(recovery.ota_state == BH_OTA_STATE_STAGE1_END);
    assert(recovery.usable_bank == 0x3U);

    assert(bh_ota_package_verify_stage2(pkg_dir, public_key, &current_bank) == BH_OTA_OK);
    assert(current_bank == 0);

    recovery.current_bank = 1;
    recovery.target_bank = 1;
    recovery.ota_state = BH_OTA_STATE_STAGE1_END;
    assert(bh_hal_ota_recovery_write(&recovery) == BH_OTA_OK);

    assert(bh_ota_package_verify_stage2(pkg_dir, public_key, &current_bank) == BH_OTA_OK);
    assert(current_bank == 1);
    assert(bh_hal_ota_recovery_read(&recovery) == BH_OTA_OK);
    assert(recovery.ota_state == BH_OTA_STATE_STAGE2_START);
    assert(recovery.successful_bank_mask == 0x3U);
    assert(recovery.boot_success == 0U);

    assert(setenv("BH_OTA_VERSION_FILE", "/tmp/test_bh_ota_package_apply_missing_version", 1) == 0);
    assert(setenv("BH_OTA_STAGE2_ABORT_AFTER", "1", 1) == 0);
    assert(bh_ota_package_apply_stage2(pkg_dir, public_key, &current_bank) != BH_OTA_OK);
    assert(unsetenv("BH_OTA_STAGE2_ABORT_AFTER") == 0);
    assert(current_bank == 1);
    assert(bh_hal_ota_recovery_read(&recovery) == BH_OTA_OK);
    assert(recovery.current_bank == 1);
    assert(recovery.target_bank == 1);
    assert(recovery.ota_state == BH_OTA_STATE_STAGE2_START);
    assert(recovery.ota_update == 1U);
    assert_file_equals(boot_stage2_payload, boot_a_dev);
    assert_file_not_equals(rootfs_stage2_payload, rootfs_a_dev);

    assert(bh_ota_package_apply_stage2(pkg_dir, public_key, &current_bank) == BH_OTA_OK);
    assert(setenv("BH_OTA_VERSION_FILE", version_file, 1) == 0);
    assert(current_bank == 1);
    assert(bh_hal_ota_recovery_read(&recovery) == BH_OTA_OK);
    assert(recovery.current_bank == 1);
    assert(recovery.target_bank == 1);
    assert(recovery.ota_state == BH_OTA_STATE_STAGE2_WROTE);
    assert(recovery.ota_update == 2U);
    assert(recovery.boot_success == 0U);
    assert_file_equals(boot_stage2_payload, boot_a_dev);
    assert_file_equals(rootfs_stage2_payload, rootfs_a_dev);

    assert(bh_ota_package_commit_stage2(pkg_dir, public_key, &current_bank) == BH_OTA_OK);
    assert(current_bank == 1);
    assert(bh_hal_ota_recovery_read(&recovery) == BH_OTA_OK);
    assert(recovery.target_bank == 0);
    assert(recovery.ota_state == BH_OTA_STATE_STAGE2_END);

    recovery.current_bank = 0;
    assert(bh_hal_ota_recovery_write(&recovery) == BH_OTA_OK);
    assert(bh_ota_package_boot_success(pkg_dir, public_key, &current_bank) == BH_OTA_OK);
    assert(current_bank == 0);
    assert(bh_hal_ota_recovery_read(&recovery) == BH_OTA_OK);
    assert(recovery.current_bank == 0);
    assert(recovery.target_bank == 0);
    assert(recovery.ota_state == BH_OTA_STATE_COMPLETE);
    assert(recovery.ota_update == 0U);
    assert(recovery.ota_reboot_cnt == 0U);
    assert(recovery.boot_success == 1U);
    assert(recovery.successful_bank_mask == 0x3U);
    assert(recovery.rollback_index == 10204U);
    assert(recovery.pending_rollback_index == 0U);
    assert(recovery.current_image_version == 10204U);
    assert(recovery.pending_image_version == 0U);
    assert(strcmp(recovery.current_sys_version, "1.2.4") == 0);
    assert(strcmp(recovery.pending_sys_version, "") == 0);
    assert(strcmp(recovery.session_id, "") == 0);
    assert(recovery.session_id_crc32 == 0U);

    current_bank = -1;
    assert(bh_ota_package_boot_success(pkg_dir, public_key, &current_bank) == BH_OTA_OK);
    assert(current_bank == -1);

    recovery.ota_state = BH_OTA_STATE_IDLE;
    assert(bh_hal_ota_recovery_write(&recovery) == BH_OTA_OK);
    current_bank = -1;
    assert(bh_ota_package_verify_stage2(pkg_dir, public_key, &current_bank) == BH_OTA_OK);
    assert(current_bank == 0);

    recovery.ota_state = BH_OTA_STATE_STAGE1_END;
    recovery.session_id_crc32 ^= 0xffffffffU;
    assert(bh_hal_ota_recovery_write(&recovery) == BH_OTA_OK);
    current_bank = -1;
    assert(bh_ota_package_verify_stage2(pkg_dir, public_key, &current_bank) == BH_OTA_OK);
    assert(current_bank == 0);

    puts("test_bh_ota_package_apply: all tests passed");
    return 0;
}
