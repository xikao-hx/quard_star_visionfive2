#include <errno.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bh_ota_log.h"
#include "bh_ota_package.h"

#define SIGNATURE_LOG_ERROR(fmt, ...) BH_OTA_LOG_ERROR(fmt, ##__VA_ARGS__)

typedef struct bh_ota_signature_entry {
    char file[BH_OTA_MANIFEST_FILE_NAME_MAX_LEN];
} bh_ota_signature_entry_t;

extern int32_t bh_ota_sha256_file_raw(const char *path, uint8_t out_digest[32]);

static int compare_signature_entries(const void *lhs, const void *rhs)
{
    const bh_ota_signature_entry_t *a = (const bh_ota_signature_entry_t *)lhs;
    const bh_ota_signature_entry_t *b = (const bh_ota_signature_entry_t *)rhs;
    return strcmp(a->file, b->file);
}

static int32_t build_package_path(const char *package_dir,
                                  const char *file_name,
                                  char *out_path,
                                  size_t out_len)
{
    int written;

    written = snprintf(out_path, out_len, "%s/%s", package_dir, file_name);
    if (written < 0 || (size_t)written >= out_len) {
        SIGNATURE_LOG_ERROR("Package path is too long for file '%s'", file_name);
        return BH_OTA_ERROR;
    }

    return BH_OTA_OK;
}

static int hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static int32_t sha256_hex_to_raw(const char *hex, uint8_t out_digest[32])
{
    size_t i;

    if (hex == NULL || out_digest == NULL) {
        SIGNATURE_LOG_ERROR("SHA-256 hex decode received invalid parameter");
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    if (strlen(hex) != BH_OTA_SHA256_HEX_LEN) {
        SIGNATURE_LOG_ERROR("SHA-256 hex decode received invalid length");
        return BH_OTA_ERROR;
    }

    for (i = 0; i < 32U; ++i) {
        int high = hex_nibble(hex[i * 2U]);
        int low = hex_nibble(hex[i * 2U + 1U]);
        if (high < 0 || low < 0) {
            SIGNATURE_LOG_ERROR("SHA-256 hex decode received invalid character");
            return BH_OTA_ERROR;
        }
        out_digest[i] = (uint8_t)((high << 4) | low);
    }

    return BH_OTA_OK;
}

static int32_t read_file_to_buffer(const char *path, uint8_t **out_buf, size_t *out_len)
{
    FILE *fp;
    long file_len;
    uint8_t *buf;
    size_t read_len;

    if (path == NULL || out_buf == NULL || out_len == NULL) {
        SIGNATURE_LOG_ERROR("Signature file read received invalid parameter");
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    fp = fopen(path, "rb");
    if (fp == NULL) {
        SIGNATURE_LOG_ERROR("Failed to open file '%s'", path);
        return BH_OTA_ERROR_READ;
    }

    if (fseek(fp, 0L, SEEK_END) != 0) {
        fclose(fp);
        SIGNATURE_LOG_ERROR("Failed to seek file '%s'", path);
        return BH_OTA_ERROR_READ;
    }

    file_len = ftell(fp);
    if (file_len < 0) {
        fclose(fp);
        SIGNATURE_LOG_ERROR("Failed to get length of file '%s'", path);
        return BH_OTA_ERROR_READ;
    }

    if (fseek(fp, 0L, SEEK_SET) != 0) {
        fclose(fp);
        SIGNATURE_LOG_ERROR("Failed to rewind file '%s'", path);
        return BH_OTA_ERROR_READ;
    }

    buf = (uint8_t *)malloc((size_t)file_len);
    if (buf == NULL) {
        fclose(fp);
        SIGNATURE_LOG_ERROR("Failed to allocate %ld bytes for file '%s'", file_len, path);
        return BH_OTA_ERROR;
    }

    read_len = fread(buf, 1U, (size_t)file_len, fp);
    fclose(fp);
    if (read_len != (size_t)file_len) {
        free(buf);
        SIGNATURE_LOG_ERROR("Failed to read full file '%s'", path);
        return BH_OTA_ERROR_READ;
    }

    *out_buf = buf;
    *out_len = read_len;
    return BH_OTA_OK;
}

int32_t bh_ota_manifest_build_signature_payload(const bh_ota_manifest_t *manifest,
                                                const char *package_dir,
                                                uint8_t **out_buf,
                                                size_t *out_len)
{
    bh_ota_signature_entry_t entries[BH_OTA_MANIFEST_MAX_PAYLOADS + 1U];
    uint8_t *payload_buf;
    size_t entry_count;
    size_t i;
    int32_t ret;

    if (manifest == NULL || package_dir == NULL || out_buf == NULL || out_len == NULL) {
        SIGNATURE_LOG_ERROR("Signature payload build received invalid parameter");
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    memset(entries, 0, sizeof(entries));
    strncpy(entries[0].file, BH_OTA_MANIFEST_FILE_NAME, sizeof(entries[0].file) - 1U);
    entry_count = 1U;

    for (i = 0; i < manifest->payload_count; ++i) {
        size_t j;

        if (strcmp(manifest->payloads[i].file, BH_OTA_SIGNATURE_FILE_NAME) == 0) {
            SIGNATURE_LOG_ERROR("Payload file name '%s' conflicts with reserved signature file name",
                                manifest->payloads[i].file);
            return BH_OTA_ERROR;
        }

        for (j = 0; j < entry_count; ++j) {
            if (strcmp(entries[j].file, manifest->payloads[i].file) == 0) {
                SIGNATURE_LOG_ERROR("Duplicate signature input file '%s'", manifest->payloads[i].file);
                return BH_OTA_ERROR;
            }
        }

        strncpy(entries[entry_count].file,
                manifest->payloads[i].file,
                sizeof(entries[entry_count].file) - 1U);
        entry_count++;
    }

    qsort(entries, entry_count, sizeof(entries[0]), compare_signature_entries);

    payload_buf = (uint8_t *)calloc(entry_count, 32U);
    if (payload_buf == NULL) {
        SIGNATURE_LOG_ERROR("Failed to allocate signature payload buffer");
        return BH_OTA_ERROR;
    }

    BH_OTA_LOG_INFO("Building OTA package signature input for %zu files%s",
                    entry_count,
                    manifest->payload_hashes_verified ? " using cached payload digests" : "");

    for (i = 0; i < entry_count; ++i) {
        char path[512];
        int found = 0;

        if (strcmp(entries[i].file, BH_OTA_MANIFEST_FILE_NAME) == 0 || !manifest->payload_hashes_verified) {
            ret = build_package_path(package_dir, entries[i].file, path, sizeof(path));
            if (ret != BH_OTA_OK) {
                free(payload_buf);
                return ret;
            }

            ret = bh_ota_sha256_file_raw(path, payload_buf + (i * 32U));
            if (ret != BH_OTA_OK) {
                free(payload_buf);
                return ret;
            }
            continue;
        }

        for (size_t j = 0; j < manifest->payload_count; ++j) {
            if (strcmp(entries[i].file, manifest->payloads[j].file) == 0) {
                ret = sha256_hex_to_raw(manifest->payloads[j].sha256, payload_buf + (i * 32U));
                if (ret != BH_OTA_OK) {
                    free(payload_buf);
                    return ret;
                }
                found = 1;
                break;
            }
        }

        if (!found) {
            SIGNATURE_LOG_ERROR("Failed to find cached digest for payload file '%s'", entries[i].file);
            free(payload_buf);
            return BH_OTA_ERROR;
        }
    }

    BH_OTA_LOG_INFO("OTA package signature input is ready");
    *out_buf = payload_buf;
    *out_len = entry_count * 32U;
    return BH_OTA_OK;
}

int32_t bh_ota_manifest_verify_signature(const bh_ota_manifest_t *manifest,
                                         const char *package_dir,
                                         const char *signature_path,
                                         const char *public_key_path)
{
    EVP_MD_CTX *md_ctx = NULL;
    EVP_PKEY *pubkey = NULL;
    FILE *pub_fp = NULL;
    uint8_t *signature = NULL;
    size_t signature_len = 0U;
    uint8_t *payload = NULL;
    size_t payload_len = 0U;
    int verify_ok;
    int32_t ret = BH_OTA_ERROR;

    if (manifest == NULL || package_dir == NULL ||
        signature_path == NULL || public_key_path == NULL) {
        SIGNATURE_LOG_ERROR("Signature verification received invalid parameter");
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    ret = bh_ota_manifest_build_signature_payload(manifest, package_dir, &payload, &payload_len);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    ret = read_file_to_buffer(signature_path, &signature, &signature_len);
    if (ret != BH_OTA_OK) {
        free(payload);
        return ret;
    }

    pub_fp = fopen(public_key_path, "rb");
    if (pub_fp == NULL) {
        SIGNATURE_LOG_ERROR("Failed to open public key '%s'", public_key_path);
        goto out;
    }

    pubkey = PEM_read_PUBKEY(pub_fp, NULL, NULL, NULL);
    fclose(pub_fp);
    pub_fp = NULL;
    if (pubkey == NULL) {
        SIGNATURE_LOG_ERROR("Failed to parse PEM public key '%s'", public_key_path);
        goto out;
    }

    md_ctx = EVP_MD_CTX_new();
    if (md_ctx == NULL) {
        SIGNATURE_LOG_ERROR("Failed to allocate OpenSSL digest context");
        goto out;
    }

    if (EVP_DigestVerifyInit(md_ctx, NULL, EVP_sha256(), NULL, pubkey) != 1) {
        SIGNATURE_LOG_ERROR("Failed to initialize RSA-SHA256 verification");
        goto out;
    }

    verify_ok = EVP_DigestVerify(md_ctx,
                                 signature,
                                 signature_len,
                                 payload,
                                 payload_len);
    if (verify_ok != 1) {
        SIGNATURE_LOG_ERROR("OTA package signature verification failed");
        ret = BH_OTA_ERROR;
        goto out;
    }

    BH_OTA_LOG_INFO("OTA package signature verified successfully with RSA-2048 + SHA-256");
    ret = BH_OTA_OK;

out:
    if (md_ctx != NULL) {
        EVP_MD_CTX_free(md_ctx);
    }
    if (pubkey != NULL) {
        EVP_PKEY_free(pubkey);
    }
    if (pub_fp != NULL) {
        fclose(pub_fp);
    }
    free(signature);
    free(payload);
    return ret;
}
