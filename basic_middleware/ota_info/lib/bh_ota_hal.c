#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <linux/reboot.h>
#include <time.h>
#include <stddef.h>

#include "tf_crc32.h"
#include "partition.h"
#include "bh_ota_hal.h"
#include "ota_info_log.h"
#include "../../../common_inc/bsp/quard_bap_header.h"

#define MAX_PARTITIONS 100
#define BANK_A 0
#define BANK_B 1
#define MAX_PARTITION_NAME_LEN 256

#define CMDLINE_PATH "/proc/cmdline"
#define MAX_LINE_SIZE 512
#define INITIAL_BUF_SIZE 4096

#define RECOVERY_PART "recovery"
#define DEFAULT_MTD_BY_NAME_DIR "/dev/mtd/by-name"

#define MEMERASE                _IOW('M', 2, struct erase_info_user)

#define VERSION_USER 0

const char* banks_01[] = {"a", "b"};
const char* banks_123[] = {"none", "a", "b", "ab"};

struct erase_info_user {
    uint32_t start;
    uint32_t length;
};

struct image_partition_map {
    char *partition;
    char *image;
};

int32_t parse_value_from_cmdline(const char *key, int32_t *out_value);

static int32_t ota_is_regular_file_fd(int32_t fd)
{
    struct stat st;

    if (fstat(fd, &st) != 0) {
        return 0;
    }

    return S_ISREG(st.st_mode) ? 1 : 0;
}

static int32_t ota_resolve_mtd_path(const char *partition, char *path, size_t path_len)
{
    const char *base_dir;
    int written;

    if (partition == NULL || path == NULL || path_len == 0U) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    base_dir = getenv("BH_OTA_MTD_BY_NAME_DIR");
    if (base_dir == NULL || base_dir[0] == '\0') {
        base_dir = DEFAULT_MTD_BY_NAME_DIR;
    }

    written = snprintf(path, path_len, "%s/%s", base_dir, partition);
    if (written <= 0 || (size_t)written >= path_len) {
        OTA_INFO_LOG_ERROR("MTD path is too long for partition '%s'", partition);
        return BH_OTA_ERROR;
    }

    return BH_OTA_OK;
}

static uint32_t recovery_header_crc32(const recovery_config_t *cfg)
{
    return bh_hal_ota_crc32((const unsigned char *)cfg,
                            offsetof(recovery_config_t, header_crc32));
}

static int32_t recovery_is_valid(const recovery_config_t *cfg)
{
    uint32_t crc;

    if (cfg->magic != BH_OTA_RECOVERY_MAGIC) {
        return 0;
    }

    if (cfg->struct_version != BH_OTA_RECOVERY_STRUCT_VERSION) {
        return 0;
    }

    crc = recovery_header_crc32(cfg);
    if (crc != cfg->header_crc32) {
        return 0;
    }

    return 1;
}

static int32_t ota_verify_image(const char *filename, struct binary_header *bheader)
{
    struct binary_header header;
    ssize_t bytes_read;
    off_t payload_offset;
    uint32_t payload_length;
    uint8_t *buffer;
    uint32_t crc = 0;
    int32_t fd;

	if (!filename || !bheader) {
        OTA_INFO_LOG_ERROR("Invalid parameters (NULL).");
        return BH_OTA_ERROR;
    }

    fd = open(filename, O_RDONLY);
    if (fd == -1) {
        OTA_INFO_LOG_ERROR("Failed to open file '%s'", filename);
        return BH_OTA_ERROR;
    }

    bytes_read = read(fd, &header, sizeof(header));
    if (bytes_read != sizeof(header)) {
        OTA_INFO_LOG_ERROR("Failed to read header from '%s', expected %zu, got %zd", filename, sizeof(header), bytes_read);
        close(fd);
        return BH_OTA_ERROR;
    }

    crc = bh_hal_ota_crc32((uint8_t *)&header + 8, bytes_read - 12);
    if (crc != header.header_checksum) {
        OTA_INFO_LOG_ERROR("Header checksum mismatch for '%s'! Expected: 0x%x, Got: 0x%x",
                  filename, header.header_checksum, crc);
        close(fd);
        return BH_OTA_ERROR;
    }

    OTA_INFO_LOG_INFO("Header checksum for '%s' is valid: 0x%x", filename, crc);

    payload_offset = sizeof(header);
    payload_length = header.image_length;
    if (lseek(fd, payload_offset, SEEK_SET) == (off_t)-1) {
        OTA_INFO_LOG_ERROR("Failed to seek to payload in '%s'", filename);
        close(fd);
        return BH_OTA_ERROR;
    }

    buffer = (uint8_t *)malloc(payload_length);
    if (!buffer) {
        OTA_INFO_LOG_ERROR("Memory allocation failed for payload of size %u", payload_length);
        close(fd);
        return BH_OTA_ERROR;
    }

    bytes_read = read(fd, buffer, payload_length);
    if (bytes_read != payload_length) {
        OTA_INFO_LOG_ERROR("Failed to read payload data from '%s', expected %u, got %zd", filename, payload_length, bytes_read);
        free(buffer);
        close(fd);
        return BH_OTA_ERROR;
    }

    crc = bh_hal_ota_crc32(buffer, payload_length);
    free(buffer);
    close(fd);

    if (crc != header.binary_checksum) {
        OTA_INFO_LOG_ERROR("Payload CRC mismatch for '%s'! Expected: 0x%x, Got: 0x%x",
                  filename, header.binary_checksum, crc);
        return BH_OTA_ERROR;
    }

    OTA_INFO_LOG_INFO("Payload checksum for '%s' is valid: 0x%x", filename, crc);

    if (bheader) {
        memcpy((uint8_t *)bheader, (uint8_t *)&header, sizeof(struct binary_header));
    }

    return BH_OTA_OK;
}

int32_t bh_hal_ota_verify_image(const char *filename)
{
    struct binary_header header;
    return ota_verify_image(filename, &header);
}

static const char* convert_01(int32_t bank_mask)
{
    return banks_01[bank_mask & 0x1];
}

static const char* convert_usable(uint32_t usable)
{
    return banks_123[usable & 0x3];
}

#if VERSION_USER
static int32_t is_bank_usable(uint32_t bank, uint32_t usable_bank)
{
    uint32_t bk = bank & 0x1;
    uint32_t mask = usable_bank & 0x3;
    return ((1 << bk) & mask) != 0;
}
#endif

uint32_t bh_hal_ota_crc32(const unsigned char *buf, uint32_t size)
{
    return tf_crc32(0, buf, size);
}

int32_t bh_hal_ota_init(void)
{
    char recovery_path[MAX_PARTITION_NAME_LEN];
    int32_t fd;

    if (ota_resolve_mtd_path(RECOVERY_PART, recovery_path,
                             sizeof(recovery_path)) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    fd = open(recovery_path, O_RDONLY);
    if (fd < 0) {
        OTA_INFO_LOG_ERROR("Failed to open recovery MTD device '%s'",
                           recovery_path);
        return BH_OTA_ERROR_READ;
    }

    close(fd);
    return BH_OTA_OK;
}

static int32_t memerase(int32_t fd, struct erase_info_user *erase)
{
    return ioctl(fd, MEMERASE, erase);
}

static int32_t erase_flash(int32_t fd, uint32_t offset, uint32_t bytes)
{
    int32_t err;
    struct erase_info_user erase;
    const size_t alignment = 0x1000;

    erase.start = offset;
    erase.length = (bytes + alignment - 1) & ~(alignment - 1);

    err = memerase(fd, &erase);
    if (err < 0) {
        OTA_INFO_LOG_ERROR("MEMERASE ioctl failed with return code %d", err);
        return BH_OTA_ERROR;
    }
    OTA_INFO_LOG_INFO("Erased %u bytes from address 0x%08x in flash", erase.length, offset);

    return BH_OTA_OK;
}

static int32_t flash_recovery_partition(const recovery_config_t *recovery)
{
    int32_t fd_out;
    ssize_t size_write;
    char recovery_path[MAX_PARTITION_NAME_LEN];

    if (ota_resolve_mtd_path(RECOVERY_PART, recovery_path, sizeof(recovery_path)) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    fd_out = open(recovery_path, O_SYNC | O_RDWR | O_CREAT, 0644);
    if (fd_out < 0) {
        OTA_INFO_LOG_ERROR("Error opening recovery MTD device");
        return BH_OTA_ERROR;
    }

    if (ota_is_regular_file_fd(fd_out)) {
        if (ftruncate(fd_out, 0) != 0 || lseek(fd_out, 0, SEEK_SET) == (off_t)-1) {
            OTA_INFO_LOG_ERROR("Failed to prepare regular recovery file '%s' for write", recovery_path);
            close(fd_out);
            return BH_OTA_ERROR;
        }
    } else {
        if (erase_flash(fd_out, 0, sizeof(*recovery)) != BH_OTA_OK) {
            close(fd_out);
            return BH_OTA_ERROR;
        }
    }

    size_write = write(fd_out, (const void *)recovery, sizeof(*recovery));
    if (size_write != sizeof(*recovery)) {
        OTA_INFO_LOG_ERROR("Write to recovery partition failed. Wrote %zd bytes, expected %zu",
                  size_write, sizeof(*recovery));
        close(fd_out);
        return BH_OTA_ERROR;
    }

    close(fd_out);
    return BH_OTA_OK;
}

int32_t bh_hal_ota_recovery_read(recovery_config_t *cfg)
{
    int32_t fd_out;
    ssize_t size_read;
    char recovery_path[MAX_PARTITION_NAME_LEN];
    recovery_config_t raw;

    if (cfg == NULL) {
        OTA_INFO_LOG_ERROR("Invalid recovery metadata pointer (NULL).");
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    if (ota_resolve_mtd_path(RECOVERY_PART, recovery_path, sizeof(recovery_path)) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    fd_out = open(recovery_path, O_RDONLY);
    if (fd_out == -1) {
        OTA_INFO_LOG_ERROR("Error opening recovery MTD device for reading");
        return BH_OTA_ERROR;
    }

    memset(&raw, 0, sizeof(raw));
    size_read = read(fd_out, (void *)&raw, sizeof(raw));
    if (size_read < 0) {
        OTA_INFO_LOG_ERROR("Read from recovery partition failed");
        close(fd_out);
        return BH_OTA_ERROR;
    }

    close(fd_out);

    if ((size_t)size_read >= sizeof(recovery_config_t) &&
        recovery_is_valid(&raw)) {
        memcpy(cfg, &raw, sizeof(*cfg));
        return BH_OTA_OK;
    }

    OTA_INFO_LOG_ERROR("Recovery metadata is not a valid block");
    return BH_OTA_ERROR;
}

int32_t bh_hal_ota_recovery_write(const recovery_config_t *cfg)
{
    recovery_config_t disk_cfg;

    if (cfg == NULL) {
        OTA_INFO_LOG_ERROR("Invalid recovery metadata pointer (NULL).");
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    memcpy(&disk_cfg, cfg, sizeof(disk_cfg));
    disk_cfg.magic = BH_OTA_RECOVERY_MAGIC;
    disk_cfg.struct_version = BH_OTA_RECOVERY_STRUCT_VERSION;
    disk_cfg.header_crc32 = 0;
    disk_cfg.header_crc32 = recovery_header_crc32(&disk_cfg);

    return flash_recovery_partition(&disk_cfg);
}

int32_t bh_hal_ota_switch_bank(int32_t target_bank)
{
    recovery_config_t recovery;

    if (target_bank != BANK_A && target_bank != BANK_B) {
        OTA_INFO_LOG_ERROR("Invalid target bank: %d", target_bank);
        return BH_OTA_ERROR;
    }

    if (bh_hal_ota_recovery_read(&recovery) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    if (((1 << target_bank) & recovery.usable_bank) == 0) {
        OTA_INFO_LOG_ERROR("Refusing to switch to unusable bank %d (usable_mask=0x%x)", target_bank, recovery.usable_bank);
        return BH_OTA_ERROR;
    }

    recovery.target_bank = target_bank;

    if (bh_hal_ota_recovery_write(&recovery) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    OTA_INFO_LOG_INFO("Switched target bank to %s successfully.", convert_01(recovery.target_bank));
    return BH_OTA_OK;
}

int32_t bh_hal_ota_set_current_bank(int32_t bank)
{
    recovery_config_t recovery;

    if (bank != BANK_A && bank != BANK_B) {
        OTA_INFO_LOG_ERROR("Invalid bank: %d", bank);
        return BH_OTA_ERROR;
    }

    if (bh_hal_ota_recovery_read(&recovery) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    recovery.current_bank = bank;

    if (bh_hal_ota_recovery_write(&recovery) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    OTA_INFO_LOG_INFO("Bank %s marked as current bank.", convert_01(bank));
    return BH_OTA_OK;
}

int32_t bh_hal_ota_get_current_bank(int32_t *bank)
{
    recovery_config_t recovery;

    if (bank == NULL) {
        OTA_INFO_LOG_ERROR("Invalid bank pointer (NULL).");
        return BH_OTA_ERROR;
    }

    if (bh_hal_ota_recovery_read(&recovery) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    *bank = recovery.current_bank & 0x1;
    OTA_INFO_LOG_INFO("current bank: %s", convert_01(*bank));
    return BH_OTA_OK;
}

int32_t bh_hal_ota_get_target_bank(int32_t *bank)
{
    recovery_config_t recovery;

    if (bank == NULL) {
        OTA_INFO_LOG_ERROR("Invalid bank pointer (NULL).");
        return BH_OTA_ERROR;
    }

    if (bh_hal_ota_recovery_read(&recovery) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    *bank = recovery.target_bank & 0x1;
    OTA_INFO_LOG_INFO("target bank is: %s", convert_01(*bank));
    return BH_OTA_OK;
}

int32_t bh_hal_ota_get_usable_bank(int32_t *bank_mask)
{
    recovery_config_t recovery;

    if (bank_mask == NULL) {
        OTA_INFO_LOG_ERROR("Invalid bank_mask pointer (NULL).");
        return BH_OTA_ERROR;
    }

    if (bh_hal_ota_recovery_read(&recovery) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    *bank_mask = recovery.usable_bank & 0x3;
    OTA_INFO_LOG_INFO("usable bank mask is: %s (0x%x)", convert_usable(*bank_mask), *bank_mask);
    return BH_OTA_OK;
}

int32_t bh_hal_ota_set_usable_bank(uint32_t bank_mask)
{
    recovery_config_t recovery;

    if (bank_mask > 3) {
        OTA_INFO_LOG_ERROR("Invalid bank mask: %u", bank_mask);
        return BH_OTA_ERROR;
    }

    if (bh_hal_ota_recovery_read(&recovery) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    recovery.usable_bank = bank_mask;

#if VERSION_USER
    if (!is_bank_usable(recovery.target_bank, recovery.usable_bank)) {
        OTA_INFO_LOG_ERROR("Operation not allowed, target_bank %d would become invalid with mask %u",
                  recovery.target_bank, recovery.usable_bank);
        return -1;
    }
#endif

    if (bh_hal_ota_recovery_write(&recovery) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    OTA_INFO_LOG_INFO("usable bank mask set to '%s' (0x%x).", convert_usable(bank_mask), bank_mask);
    return BH_OTA_OK;
}

typedef enum {
    BANK_UNKNOWN,
    BANK_CURRENT,
    BANK_TARGET,
    BANK_USABLE,
    BANK_ALL,
} bank_type_t;

bank_type_t get_bank_type(const char* obj)
{
    if (strcmp(obj, "current_bank") == 0) {
        return BANK_CURRENT;
    } else if (strcmp(obj, "target_bank") == 0) {
        return BANK_TARGET;
    } else if (strcmp(obj, "usable_bank") == 0) {
        return BANK_USABLE;
    } else if (strcmp(obj, "all") == 0) {
        return BANK_ALL;
    } else {
        return BANK_UNKNOWN;
    }
}

int32_t bh_hal_ota_reset(void)
{
    OTA_INFO_LOG_INFO("Requesting system reboot...");
    sync();

    if (reboot(LINUX_REBOOT_CMD_RESTART) == -1) {
        OTA_INFO_LOG_ERROR("Reboot command failed");
        return BH_OTA_ERROR;
    }

    // Should not reach here
    return BH_OTA_OK;
}

int32_t bh_hal_ota_sync_bl1_image(int32_t direction)
{
    struct binary_header header_src, header_dst;
    int32_t fd_src = -1, fd_dst = -1;
    ssize_t size_read, size_write;
    uint32_t total_len;
    uint8_t *buf = NULL;
    int32_t ret = BH_OTA_ERROR;
    char src_file[MAX_PARTITION_NAME_LEN];
    char dst_file[MAX_PARTITION_NAME_LEN];

    if (ota_resolve_mtd_path((direction == 0) ? "bl1_a" : "bl1_b", src_file, sizeof(src_file)) != BH_OTA_OK ||
        ota_resolve_mtd_path((direction == 0) ? "bl1_b" : "bl1_a", dst_file, sizeof(dst_file)) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    OTA_INFO_LOG_INFO("Starting BL1 sync from %s to %s", src_file, dst_file);

    if ((ret = ota_verify_image(src_file, &header_src)) != 0) {
        OTA_INFO_LOG_ERROR("Verification of source image '%s' failed", src_file);
        goto exit;
    }

    if ((fd_src = open(src_file, O_RDONLY)) < 0) {
        OTA_INFO_LOG_ERROR("Failed to open source partition '%s'", src_file);
        goto exit;
    }

    if ((fd_dst = open(dst_file, O_SYNC | O_RDWR)) < 0) {
        OTA_INFO_LOG_ERROR("Failed to open destination partition '%s'", dst_file);
        goto close_fd_src;
    }

    total_len = header_src.image_length + sizeof(struct binary_header);

    buf = malloc(total_len);
    if (buf == NULL) {
        OTA_INFO_LOG_ERROR("malloc(%u) failed", total_len);
        goto close_fds;
    }
    memset(buf, 0, total_len);

    OTA_INFO_LOG_INFO("Total length to sync = %u bytes", total_len);

    size_read = read(fd_src, buf, total_len);
    if (size_read != total_len) {
        OTA_INFO_LOG_ERROR("Read error from '%s', expected %u, got %zd", src_file, total_len, size_read);
        goto free_buf;
    }

    if (ota_is_regular_file_fd(fd_dst)) {
        if (ftruncate(fd_dst, 0) != 0) {
            OTA_INFO_LOG_ERROR("Failed to truncate destination image '%s'", dst_file);
            goto free_buf;
        }
    } else {
        if (erase_flash(fd_dst, 0, total_len) != 0) {
            OTA_INFO_LOG_ERROR("Failed to erase flash on '%s'", dst_file);
            goto free_buf;
        }
    }

    if (lseek(fd_dst, 0, SEEK_SET) == -1) {
        OTA_INFO_LOG_ERROR("lseek failed on '%s'", dst_file);
        goto free_buf;
    }

    size_write = write(fd_dst, buf, total_len);
    if (size_write != size_read) {
        OTA_INFO_LOG_ERROR("Write error to '%s', expected %zd, wrote %zd", dst_file, size_read, size_write);
        goto free_buf;
    }

    close(fd_dst);
    fd_dst = -1;

    if ((ret = ota_verify_image(dst_file, &header_dst)) != 0) {
        OTA_INFO_LOG_ERROR("Verification of destination image '%s' failed after sync", dst_file);
        goto free_buf;
    }

    if ((header_dst.binary_checksum == header_src.binary_checksum) &&
        (header_dst.header_checksum == header_src.header_checksum)) {
        OTA_INFO_LOG_INFO("BL1 sync successful. Checksums match.");
        ret = BH_OTA_OK;
    } else {
        OTA_INFO_LOG_ERROR("Destination image verification failed! Checksums do not match.");
        ret = BH_OTA_ERROR;
    }

free_buf:
    free(buf);
close_fds:
    if (fd_dst >= 0) close(fd_dst);
close_fd_src:
    if (fd_src >= 0) close(fd_src);
exit:
    return ret;
}

int32_t bh_hal_ota_flash_fsi_image(uint8_t *buf_addr, uint32_t buf_len, const char *partition)
{
    int32_t fd;
    ssize_t size_write, size_read;
    uint32_t part_size;
    uint8_t *buf;
    char mtd[MAX_PARTITION_NAME_LEN];
    int32_t ret = BH_OTA_OK;
    uint32_t crc32 = 0;

    if (buf_addr == NULL || buf_len == 0 || partition == NULL) {
        OTA_INFO_LOG_ERROR("Invalid parameters: buf_addr, buf_len, or partition is NULL");
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    if (ota_resolve_mtd_path(partition, mtd, sizeof(mtd)) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    ret = get_partition_size_by_name(partition, TYPE_FLASH, &part_size);
    if (ret != BH_OTA_OK) {
        OTA_INFO_LOG_ERROR("Partition '%s' does not exist", partition);
        return ret;
    }

    if (buf_len > part_size) {
        OTA_INFO_LOG_ERROR("Buffer length %u is greater than partition '%s' size %u", buf_len, partition, part_size);
        return BH_OTA_ERROR;
    }

    if ((fd = open(mtd, O_SYNC | O_RDWR )) < 0) {
        OTA_INFO_LOG_ERROR("Failed to open MTD device '%s'", mtd);
        return BH_OTA_ERROR;
    }

    crc32 = bh_hal_ota_crc32((uint8_t *)buf_addr, buf_len);

    if (ota_is_regular_file_fd(fd)) {
        if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) == (off_t)-1) {
            OTA_INFO_LOG_ERROR("Failed to prepare regular MTD image '%s' for write", mtd);
            ret = BH_OTA_ERROR;
            goto close_fd;
        }
    } else {
        if (erase_flash(fd, 0, buf_len) != BH_OTA_OK) {
            ret = BH_OTA_ERROR;
            goto close_fd;
        }
    }

    size_write = write(fd, buf_addr, buf_len);
    if (size_write != buf_len) {
        OTA_INFO_LOG_ERROR("Write to '%s' failed. Expected %u, wrote %zd", mtd, buf_len, size_write);
        ret = BH_OTA_ERROR;
        goto close_fd;
    }

    /* read back */
    buf = (uint8_t *)malloc(buf_len);
    if (buf == NULL) {
        OTA_INFO_LOG_ERROR("Failed to allocate buffer for read-back verification");
        ret = BH_OTA_ERROR;
        goto close_fd;
    }

    memset(buf, 0, buf_len);
    if (lseek(fd, 0, SEEK_SET) == -1) {
        OTA_INFO_LOG_ERROR("lseek failed on '%s'", mtd);
        ret = BH_OTA_ERROR;
        goto error;
    }

    size_read = read(fd, buf, buf_len);
    if (size_read != buf_len) {
        OTA_INFO_LOG_ERROR("Read-back from '%s' failed. Expected %u, read %zd", mtd, buf_len, size_read);
        ret = BH_OTA_ERROR;
        goto error;
    }

    if (crc32 != bh_hal_ota_crc32(buf, buf_len)) {
        OTA_INFO_LOG_ERROR("Read-back verification failed for '%s'. CRC mismatch.", partition);
        ret = BH_OTA_ERROR;
    } else {
        OTA_INFO_LOG_INFO("Flash to partition '%s' successful.", partition);
    }

error:
    free(buf);
close_fd:
    close(fd);

    return ret;
}

int32_t parse_value_from_cmdline(const char *key, int32_t *out_value)
{
    FILE *fp = NULL;
    static char cmdline_buf[INITIAL_BUF_SIZE];
    char *token;
    char *endptr;
    char *saveptr;
    size_t bytes_read;
	int64_t val;
    const char *current_key;
    const char *value_str;
    int32_t result = BH_OTA_ERROR;

    if (!key || !out_value) {
        OTA_INFO_LOG_ERROR("Invalid parameters: key or out_value is NULL.");
        return BH_OTA_ERROR;
    }

    fp = fopen(CMDLINE_PATH, "r");
    if (!fp) {
        OTA_INFO_LOG_ERROR("Error opening " CMDLINE_PATH);
        return BH_OTA_ERROR;
    }

    bytes_read = fread(cmdline_buf, 1, sizeof(cmdline_buf) - 1, fp);

    if (bytes_read == 0 && ferror(fp)) {
        OTA_INFO_LOG_ERROR("Error reading from " CMDLINE_PATH);
        fclose(fp);
        return BH_OTA_ERROR;
    }

    fclose(fp);

    cmdline_buf[bytes_read] = '\0';

    for (size_t i = 0; i < bytes_read; ++i) {
        if (cmdline_buf[i] == '\n') {
            cmdline_buf[i] = ' ';
        }
    }

    token = strtok_r(cmdline_buf, " ", &saveptr);
    while (token != NULL) {
        char *eq_ptr = strchr(token, '=');
        if (eq_ptr != NULL) {
            *eq_ptr = '\0';
            current_key = token;
            value_str = eq_ptr + 1;

            if (strcmp(current_key, key) == 0) {
                if (*value_str == '\0') {
                    OTA_INFO_LOG_ERROR("Key '%s' found but has no value.", key);
                    result = BH_OTA_ERROR;
                    break;
                }

                val = strtol(value_str, &endptr, 10);

                if (*endptr == '\0' || isspace((unsigned char)*endptr)) {
                    *out_value = (int32_t)val;
                    result = BH_OTA_OK;
                } else {
                    OTA_INFO_LOG_ERROR("Value for key '%s' is not a valid integer: '%s'", key, value_str);
                    result = BH_OTA_ERROR;
                }
                break;
            }
        }
        token = strtok_r(NULL, " ", &saveptr);
    }

    return result;
}

int32_t bh_hal_ota_get_bl1_bank(int32_t *bank)
{
    int32_t ret;

	ret	= parse_value_from_cmdline("active_bl1", bank);
    if (ret == BH_OTA_OK) {
        OTA_INFO_LOG_INFO("Found active_bl1 bank from cmdline: %d", *bank);
    } else {
        OTA_INFO_LOG_ERROR("Could not find 'active_bl1' in cmdline.");
    }

    return ret;
}
