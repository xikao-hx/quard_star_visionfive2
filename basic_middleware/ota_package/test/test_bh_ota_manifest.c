#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bh_ota_package.h"

static void write_file_or_die(const char *path, const char *content)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    assert(fd >= 0);
    assert(write(fd, content, strlen(content)) == (ssize_t)strlen(content));
    close(fd);
}

static void create_temp_dir_or_die(char *path_template)
{
    assert(mkdtemp(path_template) != NULL);
}

static void remove_dir_or_die(const char *path)
{
    assert(rmdir(path) == 0);
}

static void run_cmd_or_die(const char *cmd)
{
    assert(system(cmd) == 0);
}

static void build_manifest_json(char *out,
                                size_t out_len,
                                const char *sys_version,
                                const char *session_id,
                                uint32_t rollback_index,
                                const char *partition,
                                const char *file,
                                const char *sha256,
                                uint32_t size,
                                uint32_t stage)
{
    int written = snprintf(out,
                           out_len,
                           "{"
                           "\"format_version\":1,"
                           "\"sys_version\":\"%s\","
                           "\"session_id\":\"%s\","
                           "\"rollback_index\":%u,"
                           "\"payloads\":[{"
                           "\"partition\":\"%s\","
                           "\"file\":\"%s\","
                           "\"sha256\":\"%s\","
                           "\"size\":%u,"
                           "\"stage\":%u"
                           "}]"
                           "}",
                           sys_version,
                           session_id,
                           rollback_index,
                           partition,
                           file,
                           sha256,
                           size,
                           stage);
    assert(written > 0 && (size_t)written < out_len);
}

static void test_valid_manifest(void)
{
    static const char *json =
        "{"
        "\"format_version\":1,"
        "\"sys_version\":\"1.2.3\","
        "\"session_id\":\"20260524-0001\","
        "\"rollback_index\":120,"
        "\"payloads\":["
        "{\"partition\":\"bl1_b\",\"file\":\"bl1_b.img.ota\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"size\":3,\"stage\":1},"
        "{\"partition\":\"rootfs_b\",\"file\":\"rootfs_b.img.ota\",\"sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"size\":5,\"stage\":1}"
        "]"
        "}";
    bh_ota_manifest_t manifest;

    assert(bh_ota_manifest_parse(json, &manifest) == BH_OTA_OK);
    assert(manifest.format_version == 1U);
    assert(strcmp(manifest.sys_version, "1.2.3") == 0);
    assert(manifest.sys_version_code == 10203U);
    assert(strcmp(manifest.session_id, "20260524-0001") == 0);
    assert(manifest.rollback_index == 120U);
    assert(manifest.payload_count == 2U);
    assert(strcmp(manifest.payloads[0].partition, "bl1_b") == 0);
    assert(strcmp(manifest.payloads[1].partition, "rootfs_b") == 0);
    assert(strcmp(manifest.payloads[0].file, "bl1_b.img.ota") == 0);
    assert(strcmp(manifest.payloads[0].sha256, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 0);
    assert(manifest.payloads[0].size == 3U);
    assert(manifest.payloads[0].partition_size == 3U);
    assert(manifest.payloads[0].stage == 1U);
}

static void test_block_payload_partition_size(void)
{
    static const char *json =
        "{"
        "\"format_version\":1,"
        "\"sys_version\":\"1.2.3\","
        "\"session_id\":\"20260524-0001\","
        "\"rollback_index\":120,"
        "\"payloads\":["
        "{\"partition\":\"rootfs_b\",\"file\":\"rootfs_b.img.ota\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"size\":3,\"partition_size\":4096,\"stage\":1}"
        "]"
        "}";
    bh_ota_manifest_t manifest;

    assert(bh_ota_manifest_parse(json, &manifest) == BH_OTA_OK);
    assert(manifest.payloads[0].size == 3U);
    assert(manifest.payloads[0].partition_size == 4096U);
}

static void test_invalid_block_partition_size(void)
{
    static const char *json =
        "{"
        "\"format_version\":1,"
        "\"sys_version\":\"1.2.3\","
        "\"session_id\":\"20260524-0001\","
        "\"rollback_index\":120,"
        "\"payloads\":["
        "{\"partition\":\"rootfs_b\",\"file\":\"rootfs_b.img.ota\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"size\":4096,\"partition_size\":3,\"stage\":1}"
        "]"
        "}";
    bh_ota_manifest_t manifest;

    assert(bh_ota_manifest_parse(json, &manifest) != BH_OTA_OK);
}

static void test_invalid_non_block_partition_size(void)
{
    static const char *json =
        "{"
        "\"format_version\":1,"
        "\"sys_version\":\"1.2.3\","
        "\"session_id\":\"20260524-0001\","
        "\"rollback_index\":120,"
        "\"payloads\":["
        "{\"partition\":\"bl1_b\",\"file\":\"bl1_b.img.ota\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"size\":3,\"partition_size\":4096,\"stage\":1}"
        "]"
        "}";
    bh_ota_manifest_t manifest;

    assert(bh_ota_manifest_parse(json, &manifest) != BH_OTA_OK);
}

static void test_boot_file_partition_size_must_equal_size(void)
{
    static const char *json =
        "{"
        "\"format_version\":1,"
        "\"sys_version\":\"1.2.3\","
        "\"session_id\":\"20260524-0001\","
        "\"rollback_index\":120,"
        "\"payloads\":["
        "{\"partition\":\"boot_b\",\"file\":\"boot_b.vfat.ota\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"size\":3,\"partition_size\":4096,\"stage\":1}"
        "]"
        "}";
    bh_ota_manifest_t manifest;

    assert(bh_ota_manifest_parse(json, &manifest) != BH_OTA_OK);
}

static void test_invalid_stage_value(void)
{
    static const char *json =
        "{"
        "\"format_version\":1,"
        "\"sys_version\":\"1.2.3\","
        "\"session_id\":\"20260524-0001\","
        "\"rollback_index\":120,"
        "\"payloads\":["
        "{\"partition\":\"rootfs_b\",\"file\":\"rootfs_b.img.ota\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"size\":3,\"stage\":3}"
        "]"
        "}";
    bh_ota_manifest_t manifest;

    assert(bh_ota_manifest_parse(json, &manifest) != BH_OTA_OK);
}

static void test_missing_required_field(void)
{
    static const char *json =
        "{"
        "\"format_version\":1,"
        "\"session_id\":\"20260524-0001\","
        "\"rollback_index\":120,"
        "\"payloads\":[{\"partition\":\"bl1_b\",\"file\":\"bl1_b.img.ota\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"size\":3,\"stage\":1}]"
        "}";
    bh_ota_manifest_t manifest;

    assert(bh_ota_manifest_parse(json, &manifest) != BH_OTA_OK);
}

static void test_field_type_error(void)
{
    static const char *json =
        "{"
        "\"format_version\":\"1\","
        "\"sys_version\":\"1.2.3\","
        "\"session_id\":\"20260524-0001\","
        "\"rollback_index\":120,"
        "\"payloads\":[{\"partition\":\"bl1_b\",\"file\":\"bl1_b.img.ota\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"size\":3,\"stage\":1}]"
        "}";
    bh_ota_manifest_t manifest;

    assert(bh_ota_manifest_parse(json, &manifest) != BH_OTA_OK);
}

static void test_empty_payloads(void)
{
    static const char *json =
        "{"
        "\"format_version\":1,"
        "\"sys_version\":\"1.2.3\","
        "\"session_id\":\"20260524-0001\","
        "\"rollback_index\":120,"
        "\"payloads\":[]"
        "}";
    bh_ota_manifest_t manifest;

    assert(bh_ota_manifest_parse(json, &manifest) != BH_OTA_OK);
}

static void test_duplicate_partition(void)
{
    static const char *json =
        "{"
        "\"format_version\":1,"
        "\"sys_version\":\"1.2.3\","
        "\"session_id\":\"20260524-0001\","
        "\"rollback_index\":120,"
        "\"payloads\":["
        "{\"partition\":\"bl1_b\",\"file\":\"bl1_b.img.ota\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"size\":3,\"stage\":1},"
        "{\"partition\":\"bl1_b\",\"file\":\"bl1_b_2.img.ota\",\"sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"size\":5,\"stage\":1}"
        "]"
        "}";
    bh_ota_manifest_t manifest;

    assert(bh_ota_manifest_parse(json, &manifest) != BH_OTA_OK);
}

static void test_parse_manifest_file(void)
{
    const char *path = "/tmp/test_bh_ota_manifest.json";
    static const char *json =
        "{\n"
        "  \"format_version\": 1,\n"
        "  \"sys_version\": \"2.0.1\",\n"
        "  \"session_id\": \"20260524-0002\",\n"
        "  \"rollback_index\": 200,\n"
        "  \"payloads\": [\n"
        "    {\"partition\": \"boot_b\", \"file\": \"boot_b.img.ota\", \"sha256\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\", \"size\": 3, \"stage\": 1}\n"
        "  ]\n"
        "}\n";
    bh_ota_manifest_t manifest;

    write_file_or_die(path, json);
    assert(bh_ota_manifest_parse_file(path, &manifest) == BH_OTA_OK);
    assert(strcmp(manifest.sys_version, "2.0.1") == 0);
    assert(manifest.rollback_index == 200U);
    assert(manifest.payload_count == 1U);
    unlink(path);
}

static void test_version_gate_lower_rejected(void)
{
    const char *path = "/tmp/test_bh_ota_manifest_version_low.txt";
    static const char *json =
        "{"
        "\"format_version\":1,"
        "\"sys_version\":\"1.2.0\","
        "\"session_id\":\"20260524-1001\","
        "\"rollback_index\":120,"
        "\"payloads\":[{\"partition\":\"rootfs_b\",\"file\":\"rootfs_b.img.ota\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"size\":3,\"stage\":1}]"
        "}";
    bh_ota_manifest_t manifest;
    recovery_config_t recovery_before;
    recovery_config_t recovery_after;
    int32_t comparison = 0;

    memset(&recovery_before, 0x5a, sizeof(recovery_before));
    memset(&recovery_after, 0x5a, sizeof(recovery_after));

    assert(bh_ota_manifest_parse(json, &manifest) == BH_OTA_OK);
    write_file_or_die(path, "1.2.1\n");
    assert(bh_ota_manifest_check_version_file(&manifest, path, &comparison) != BH_OTA_OK);
    assert(comparison < 0);
    assert(memcmp(&recovery_before, &recovery_after, sizeof(recovery_before)) == 0);
    unlink(path);
}

static void test_version_gate_equal_allowed(void)
{
    const char *path = "/tmp/test_bh_ota_manifest_version_equal.txt";
    static const char *json =
        "{"
        "\"format_version\":1,"
        "\"sys_version\":\"1.2.1\","
        "\"session_id\":\"20260524-1002\","
        "\"rollback_index\":121,"
        "\"payloads\":[{\"partition\":\"rootfs_b\",\"file\":\"rootfs_b.img.ota\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"size\":3,\"stage\":1}]"
        "}";
    bh_ota_manifest_t manifest;
    int32_t comparison = -2;

    assert(bh_ota_manifest_parse(json, &manifest) == BH_OTA_OK);
    write_file_or_die(path, "1.2.1\n");
    assert(bh_ota_manifest_check_version_file(&manifest, path, &comparison) == BH_OTA_OK);
    assert(comparison == 0);
    unlink(path);
}

static void test_version_gate_higher_allowed(void)
{
    const char *path = "/tmp/test_bh_ota_manifest_version_high.txt";
    static const char *json =
        "{"
        "\"format_version\":1,"
        "\"sys_version\":\"1.2.2\","
        "\"session_id\":\"20260524-1003\","
        "\"rollback_index\":122,"
        "\"payloads\":[{\"partition\":\"rootfs_b\",\"file\":\"rootfs_b.img.ota\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"size\":3,\"stage\":1}]"
        "}";
    bh_ota_manifest_t manifest;
    int32_t comparison = -2;

    assert(bh_ota_manifest_parse(json, &manifest) == BH_OTA_OK);
    write_file_or_die(path, "1.2.1\n");
    assert(bh_ota_manifest_check_version_file(&manifest, path, &comparison) == BH_OTA_OK);
    assert(comparison > 0);
    unlink(path);
}

static void test_verify_payloads_success(void)
{
    char dir[] = "/tmp/test_bh_ota_payload_okXXXXXX";
    const char *file_name = "rootfs_b.img.ota";
    char payload_path[256];
    char sha256[BH_OTA_SHA256_HEX_LEN + 1U];
    char json[1024];
    bh_ota_manifest_t manifest;

    create_temp_dir_or_die(dir);
    snprintf(payload_path, sizeof(payload_path), "%s/%s", dir, file_name);
    write_file_or_die(payload_path, "abc");
    assert(bh_ota_sha256_file_hex(payload_path, sha256, sizeof(sha256)) == BH_OTA_OK);
    build_manifest_json(json, sizeof(json), "1.2.3", "20260524-2001", 200U,
                        "rootfs_b", file_name, sha256, 3U, 1U);

    assert(bh_ota_manifest_parse(json, &manifest) == BH_OTA_OK);
    assert(bh_ota_manifest_verify_payloads(&manifest, dir) == BH_OTA_OK);

    unlink(payload_path);
    remove_dir_or_die(dir);
}

static void test_verify_payloads_tampered_content(void)
{
    char dir[] = "/tmp/test_bh_ota_payload_tamperXXXXXX";
    const char *file_name = "rootfs_b.img.ota";
    char payload_path[256];
    char sha256[BH_OTA_SHA256_HEX_LEN + 1U];
    char json[1024];
    bh_ota_manifest_t manifest;

    create_temp_dir_or_die(dir);
    snprintf(payload_path, sizeof(payload_path), "%s/%s", dir, file_name);
    write_file_or_die(payload_path, "abc");
    assert(bh_ota_sha256_file_hex(payload_path, sha256, sizeof(sha256)) == BH_OTA_OK);
    build_manifest_json(json, sizeof(json), "1.2.3", "20260524-2002", 201U,
                        "rootfs_b", file_name, sha256, 3U, 1U);
    assert(bh_ota_manifest_parse(json, &manifest) == BH_OTA_OK);

    write_file_or_die(payload_path, "abd");
    assert(bh_ota_manifest_verify_payloads(&manifest, dir) != BH_OTA_OK);

    unlink(payload_path);
    remove_dir_or_die(dir);
}

static void test_verify_payloads_size_mismatch(void)
{
    char dir[] = "/tmp/test_bh_ota_payload_sizeXXXXXX";
    const char *file_name = "rootfs_b.img.ota";
    char payload_path[256];
    char sha256[BH_OTA_SHA256_HEX_LEN + 1U];
    char json[1024];
    bh_ota_manifest_t manifest;

    create_temp_dir_or_die(dir);
    snprintf(payload_path, sizeof(payload_path), "%s/%s", dir, file_name);
    write_file_or_die(payload_path, "abc");
    assert(bh_ota_sha256_file_hex(payload_path, sha256, sizeof(sha256)) == BH_OTA_OK);
    build_manifest_json(json, sizeof(json), "1.2.3", "20260524-2003", 202U,
                        "rootfs_b", file_name, sha256, 4U, 1U);

    assert(bh_ota_manifest_parse(json, &manifest) == BH_OTA_OK);
    assert(bh_ota_manifest_verify_payloads(&manifest, dir) != BH_OTA_OK);

    unlink(payload_path);
    remove_dir_or_die(dir);
}

static void test_verify_payloads_filename_mismatch(void)
{
    char dir[] = "/tmp/test_bh_ota_payload_fileXXXXXX";
    const char *real_file = "rootfs_b.img.ota";
    const char *manifest_file = "missing.img.ota";
    char payload_path[256];
    char sha256[BH_OTA_SHA256_HEX_LEN + 1U];
    char json[1024];
    bh_ota_manifest_t manifest;

    create_temp_dir_or_die(dir);
    snprintf(payload_path, sizeof(payload_path), "%s/%s", dir, real_file);
    write_file_or_die(payload_path, "abc");
    assert(bh_ota_sha256_file_hex(payload_path, sha256, sizeof(sha256)) == BH_OTA_OK);
    build_manifest_json(json, sizeof(json), "1.2.3", "20260524-2004", 203U,
                        "rootfs_b", manifest_file, sha256, 3U, 1U);

    assert(bh_ota_manifest_parse(json, &manifest) == BH_OTA_OK);
    assert(bh_ota_manifest_verify_payloads(&manifest, dir) != BH_OTA_OK);

    unlink(payload_path);
    remove_dir_or_die(dir);
}

static void create_signed_package_or_die(char *dir_template,
                                         const char *sys_version,
                                         const char *session_id,
                                         uint32_t rollback_index,
                                         const char *payload_file,
                                         const char *payload_content,
                                         const char *private_key_path,
                                         char *out_public_key_path,
                                         size_t public_key_path_len)
{
    char payload_path[256];
    char manifest_path[256];
    char message_path[256];
    char signature_path[256];
    char manifest_json[1024];
    char payload_sha256[BH_OTA_SHA256_HEX_LEN + 1U];
    char cmd[1024];
    bh_ota_manifest_t manifest;
    uint8_t *payload = NULL;
    size_t payload_len = 0U;
    FILE *fp;

    create_temp_dir_or_die(dir_template);
    snprintf(payload_path, sizeof(payload_path), "%s/%s", dir_template, payload_file);
    snprintf(manifest_path, sizeof(manifest_path), "%s/%s", dir_template, BH_OTA_MANIFEST_FILE_NAME);
    snprintf(message_path, sizeof(message_path), "%s/sign-input.bin", dir_template);
    snprintf(signature_path, sizeof(signature_path), "%s/%s", dir_template, BH_OTA_SIGNATURE_FILE_NAME);
    snprintf(out_public_key_path, public_key_path_len, "%s/public.pem", dir_template);

    write_file_or_die(payload_path, payload_content);
    assert(bh_ota_sha256_file_hex(payload_path, payload_sha256, sizeof(payload_sha256)) == BH_OTA_OK);

    build_manifest_json(manifest_json, sizeof(manifest_json),
                        sys_version, session_id, rollback_index,
                        "rootfs_b", payload_file, payload_sha256, (uint32_t)strlen(payload_content), 1U);
    write_file_or_die(manifest_path, manifest_json);
    assert(bh_ota_manifest_parse_file(manifest_path, &manifest) == BH_OTA_OK);
    assert(bh_ota_manifest_build_signature_payload(&manifest, dir_template, &payload, &payload_len) == BH_OTA_OK);

    fp = fopen(message_path, "wb");
    assert(fp != NULL);
    assert(fwrite(payload, 1U, payload_len, fp) == payload_len);
    fclose(fp);
    free(payload);

    snprintf(cmd, sizeof(cmd),
             "openssl rsa -in %s -pubout -out %s >/dev/null 2>&1",
             private_key_path, out_public_key_path);
    run_cmd_or_die(cmd);

    snprintf(cmd, sizeof(cmd),
             "openssl dgst -sha256 -sign %s -out %s %s >/dev/null 2>&1",
             private_key_path, signature_path, message_path);
    run_cmd_or_die(cmd);

    unlink(message_path);
}

static void cleanup_signed_package_or_die(const char *dir,
                                          const char *payload_file,
                                          int keep_signature)
{
    char path[256];

    snprintf(path, sizeof(path), "%s/%s", dir, payload_file);
    unlink(path);

    snprintf(path, sizeof(path), "%s/%s", dir, BH_OTA_MANIFEST_FILE_NAME);
    unlink(path);

    snprintf(path, sizeof(path), "%s/public.pem", dir);
    unlink(path);

    if (!keep_signature) {
        snprintf(path, sizeof(path), "%s/%s", dir, BH_OTA_SIGNATURE_FILE_NAME);
        unlink(path);
    }

    remove_dir_or_die(dir);
}

static void test_verify_signature_success(void)
{
    char dir[] = "/tmp/test_bh_ota_sig_okXXXXXX";
    const char *key_path = "/tmp/test_bh_ota_sig_ok.key";
    const char *payload_file = "rootfs_b.img.ota";
    char public_key_path[256];
    char manifest_path[256];
    char signature_path[256];
    char cmd[512];
    bh_ota_manifest_t manifest;

    snprintf(cmd, sizeof(cmd),
             "openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out %s >/dev/null 2>&1",
             key_path);
    run_cmd_or_die(cmd);

    create_signed_package_or_die(dir, "1.2.3", "20260524-3001", 301U,
                                 payload_file, "abc", key_path,
                                 public_key_path, sizeof(public_key_path));

    snprintf(manifest_path, sizeof(manifest_path), "%s/%s", dir, BH_OTA_MANIFEST_FILE_NAME);
    snprintf(signature_path, sizeof(signature_path), "%s/%s", dir, BH_OTA_SIGNATURE_FILE_NAME);

    assert(bh_ota_manifest_parse_file(manifest_path, &manifest) == BH_OTA_OK);
    assert(bh_ota_manifest_verify_signature(&manifest, dir, signature_path, public_key_path) == BH_OTA_OK);

    cleanup_signed_package_or_die(dir, payload_file, 0);
    unlink(key_path);
}

static void test_verify_signature_tampered_manifest(void)
{
    char dir[] = "/tmp/test_bh_ota_sig_tamperXXXXXX";
    const char *key_path = "/tmp/test_bh_ota_sig_tamper.key";
    const char *payload_file = "rootfs_b.img.ota";
    char public_key_path[256];
    char manifest_path[256];
    char signature_path[256];
    char cmd[512];
    bh_ota_manifest_t manifest;

    snprintf(cmd, sizeof(cmd),
             "openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out %s >/dev/null 2>&1",
             key_path);
    run_cmd_or_die(cmd);

    create_signed_package_or_die(dir, "1.2.3", "20260524-3002", 302U,
                                 payload_file, "abc", key_path,
                                 public_key_path, sizeof(public_key_path));

    snprintf(manifest_path, sizeof(manifest_path), "%s/%s", dir, BH_OTA_MANIFEST_FILE_NAME);
    snprintf(signature_path, sizeof(signature_path), "%s/%s", dir, BH_OTA_SIGNATURE_FILE_NAME);
    write_file_or_die(manifest_path,
                      "{\"format_version\":1,\"sys_version\":\"1.2.4\",\"session_id\":\"20260524-3002\",\"rollback_index\":302,"
                      "\"payloads\":[{\"partition\":\"rootfs_b\",\"file\":\"rootfs_b.img.ota\","
                      "\"sha256\":\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\",\"size\":3,\"stage\":1}]}");

    assert(bh_ota_manifest_parse_file(manifest_path, &manifest) == BH_OTA_OK);
    assert(bh_ota_manifest_verify_signature(&manifest, dir, signature_path, public_key_path) != BH_OTA_OK);

    cleanup_signed_package_or_die(dir, payload_file, 0);
    unlink(key_path);
}

static void test_verify_signature_wrong_key(void)
{
    char dir[] = "/tmp/test_bh_ota_sig_wrong_keyXXXXXX";
    const char *key_path = "/tmp/test_bh_ota_sig_wrong_key.key";
    const char *wrong_key_path = "/tmp/test_bh_ota_sig_wrong_key_other.key";
    const char *payload_file = "rootfs_b.img.ota";
    char public_key_path[256];
    char wrong_public_key_path[256];
    char manifest_path[256];
    char signature_path[256];
    char cmd[512];
    bh_ota_manifest_t manifest;

    snprintf(cmd, sizeof(cmd),
             "openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out %s >/dev/null 2>&1",
             key_path);
    run_cmd_or_die(cmd);
    snprintf(cmd, sizeof(cmd),
             "openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out %s >/dev/null 2>&1",
             wrong_key_path);
    run_cmd_or_die(cmd);

    create_signed_package_or_die(dir, "1.2.3", "20260524-3003", 303U,
                                 payload_file, "abc", key_path,
                                 public_key_path, sizeof(public_key_path));
    snprintf(wrong_public_key_path, sizeof(wrong_public_key_path), "%s/wrong-public.pem", dir);
    snprintf(cmd, sizeof(cmd),
             "openssl rsa -in %s -pubout -out %s >/dev/null 2>&1",
             wrong_key_path, wrong_public_key_path);
    run_cmd_or_die(cmd);

    snprintf(manifest_path, sizeof(manifest_path), "%s/%s", dir, BH_OTA_MANIFEST_FILE_NAME);
    snprintf(signature_path, sizeof(signature_path), "%s/%s", dir, BH_OTA_SIGNATURE_FILE_NAME);

    assert(bh_ota_manifest_parse_file(manifest_path, &manifest) == BH_OTA_OK);
    assert(bh_ota_manifest_verify_signature(&manifest, dir, signature_path, wrong_public_key_path) != BH_OTA_OK);

    unlink(wrong_public_key_path);
    cleanup_signed_package_or_die(dir, payload_file, 0);
    unlink(key_path);
    unlink(wrong_key_path);
}

int main(void)
{
    test_valid_manifest();
    test_block_payload_partition_size();
    test_invalid_block_partition_size();
    test_invalid_non_block_partition_size();
    test_boot_file_partition_size_must_equal_size();
    test_invalid_stage_value();
    test_missing_required_field();
    test_field_type_error();
    test_empty_payloads();
    test_duplicate_partition();
    test_parse_manifest_file();
    test_version_gate_lower_rejected();
    test_version_gate_equal_allowed();
    test_version_gate_higher_allowed();
    test_verify_payloads_success();
    test_verify_payloads_tampered_content();
    test_verify_payloads_size_mismatch();
    test_verify_payloads_filename_mismatch();
    test_verify_signature_success();
    test_verify_signature_tampered_manifest();
    test_verify_signature_wrong_key();

    puts("test_bh_ota_manifest: all tests passed");
    return 0;
}
