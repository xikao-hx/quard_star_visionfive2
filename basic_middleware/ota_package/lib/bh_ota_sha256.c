#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bh_ota_log.h"
#include "bh_ota_package.h"

#define SHA256_BLOCK_SIZE 64U
#define SHA256_DIGEST_SIZE 32U
#define SHA256_FILE_BUF_SIZE 4096U

#define SHA256_LOG_ERROR(fmt, ...) BH_OTA_LOG_ERROR(fmt, ##__VA_ARGS__)

typedef struct bh_ota_sha256_ctx {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t buffer[SHA256_BLOCK_SIZE];
    size_t buffer_len;
} bh_ota_sha256_ctx_t;

static const uint32_t sha256_k[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static uint32_t sha256_rotr(uint32_t value, uint32_t bits)
{
    return (value >> bits) | (value << (32U - bits));
}

static void sha256_transform(bh_ota_sha256_ctx_t *ctx, const uint8_t block[SHA256_BLOCK_SIZE])
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t s0, s1, temp1, temp2;
    int i;

    for (i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               (uint32_t)block[i * 4 + 3];
    }

    for (i = 16; i < 64; ++i) {
        s0 = sha256_rotr(w[i - 15], 7) ^ sha256_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        s1 = sha256_rotr(w[i - 2], 17) ^ sha256_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64; ++i) {
        s1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
        temp1 = h + s1 + ((e & f) ^ ((~e) & g)) + sha256_k[i] + w[i];
        s0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
        temp2 = s0 + ((a & b) ^ (a & c) ^ (b & c));

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(bh_ota_sha256_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->state[0] = 0x6a09e667U;
    ctx->state[1] = 0xbb67ae85U;
    ctx->state[2] = 0x3c6ef372U;
    ctx->state[3] = 0xa54ff53aU;
    ctx->state[4] = 0x510e527fU;
    ctx->state[5] = 0x9b05688cU;
    ctx->state[6] = 0x1f83d9abU;
    ctx->state[7] = 0x5be0cd19U;
}

static void sha256_update(bh_ota_sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t offset = 0;

    if (len == 0U) {
        return;
    }

    ctx->bit_count += (uint64_t)len * 8U;

    while (offset < len) {
        size_t space = SHA256_BLOCK_SIZE - ctx->buffer_len;
        size_t copy_len = (len - offset < space) ? (len - offset) : space;

        memcpy(ctx->buffer + ctx->buffer_len, data + offset, copy_len);
        ctx->buffer_len += copy_len;
        offset += copy_len;

        if (ctx->buffer_len == SHA256_BLOCK_SIZE) {
            sha256_transform(ctx, ctx->buffer);
            ctx->buffer_len = 0U;
        }
    }
}

static void sha256_final(bh_ota_sha256_ctx_t *ctx, uint8_t digest[SHA256_DIGEST_SIZE])
{
    size_t i;
    uint64_t bit_count = ctx->bit_count;

    ctx->buffer[ctx->buffer_len++] = 0x80U;

    if (ctx->buffer_len > 56U) {
        while (ctx->buffer_len < SHA256_BLOCK_SIZE) {
            ctx->buffer[ctx->buffer_len++] = 0U;
        }
        sha256_transform(ctx, ctx->buffer);
        ctx->buffer_len = 0U;
    }

    while (ctx->buffer_len < 56U) {
        ctx->buffer[ctx->buffer_len++] = 0U;
    }

    for (i = 0; i < 8U; ++i) {
        ctx->buffer[63U - i] = (uint8_t)(bit_count & 0xffU);
        bit_count >>= 8U;
    }

    sha256_transform(ctx, ctx->buffer);

    for (i = 0; i < 8U; ++i) {
        digest[i * 4U] = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4U + 1U] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4U + 2U] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4U + 3U] = (uint8_t)(ctx->state[i]);
    }
}

static void sha256_digest_to_hex(const uint8_t digest[SHA256_DIGEST_SIZE],
                                 char *out_hex,
                                 size_t out_len)
{
    static const char hex_chars[] = "0123456789abcdef";
    size_t i;

    if (out_len < (BH_OTA_SHA256_HEX_LEN + 1U)) {
        return;
    }

    for (i = 0; i < SHA256_DIGEST_SIZE; ++i) {
        out_hex[i * 2U] = hex_chars[(digest[i] >> 4) & 0x0fU];
        out_hex[i * 2U + 1U] = hex_chars[digest[i] & 0x0fU];
    }
    out_hex[BH_OTA_SHA256_HEX_LEN] = '\0';
}

int32_t bh_ota_sha256_file_hex(const char *path, char *out_hex, size_t out_len)
{
    bh_ota_sha256_ctx_t ctx;
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint8_t file_buf[SHA256_FILE_BUF_SIZE];
    ssize_t bytes_read;
    int fd;

    if (path == NULL || out_hex == NULL || out_len < (BH_OTA_SHA256_HEX_LEN + 1U)) {
        SHA256_LOG_ERROR("SHA-256 file helper received invalid parameter");
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        SHA256_LOG_ERROR("Failed to open payload file '%s' for SHA-256", path);
        return BH_OTA_ERROR_READ;
    }

    sha256_init(&ctx);

    for (;;) {
        bytes_read = read(fd, file_buf, sizeof(file_buf));
        if (bytes_read < 0) {
            close(fd);
            SHA256_LOG_ERROR("Failed to read payload file '%s' for SHA-256", path);
            return BH_OTA_ERROR_READ;
        }

        if (bytes_read == 0) {
            break;
        }

        sha256_update(&ctx, file_buf, (size_t)bytes_read);
    }

    close(fd);
    sha256_final(&ctx, digest);
    sha256_digest_to_hex(digest, out_hex, out_len);
    return BH_OTA_OK;
}

int32_t bh_ota_sha256_file_raw(const char *path, uint8_t out_digest[32])
{
    bh_ota_sha256_ctx_t ctx;
    uint8_t file_buf[SHA256_FILE_BUF_SIZE];
    ssize_t bytes_read;
    int fd;

    if (path == NULL || out_digest == NULL) {
        SHA256_LOG_ERROR("SHA-256 raw helper received invalid parameter");
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        SHA256_LOG_ERROR("Failed to open payload file '%s' for SHA-256", path);
        return BH_OTA_ERROR_READ;
    }

    sha256_init(&ctx);

    for (;;) {
        bytes_read = read(fd, file_buf, sizeof(file_buf));
        if (bytes_read < 0) {
            close(fd);
            SHA256_LOG_ERROR("Failed to read payload file '%s' for SHA-256", path);
            return BH_OTA_ERROR_READ;
        }

        if (bytes_read == 0) {
            break;
        }

        sha256_update(&ctx, file_buf, (size_t)bytes_read);
    }

    close(fd);
    sha256_final(&ctx, out_digest);
    return BH_OTA_OK;
}
