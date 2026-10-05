#include <fcntl.h>
#include <linux/fs.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bh_ota_package_internal.h"

#define OTA_PROGRESS_BAR_WIDTH 30U
#define OTA_PROGRESS_PERCENT_STEP 5U

typedef enum {
    BH_OTA_PAYLOAD_CLASS_FLASH = 0,
    BH_OTA_PAYLOAD_CLASS_SD_BLOCK = 1,
} bh_ota_payload_class_t;

typedef struct {
    const char *partition;
    const char *override_env;
    const char *label_device;
    const char *legacy_device;
} ota_sd_partition_t;

typedef struct {
    const char *partition;
    const char *phase;
    uint64_t total;
    uint32_t last_percent;
} ota_progress_t;

static const ota_sd_partition_t g_sd_partitions[] = {
    {"boot_a", "BH_OTA_BOOT_A_DEV", "/dev/disk/by-partlabel/boot_a", "/dev/mmcblk1p3"},
    {"rootfs_a", "BH_OTA_ROOTFS_A_DEV", "/dev/disk/by-partlabel/rootfs_a", "/dev/mmcblk1p4"},
    {"boot_b", "BH_OTA_BOOT_B_DEV", "/dev/disk/by-partlabel/boot_b", "/dev/mmcblk1p5"},
    {"rootfs_b", "BH_OTA_ROOTFS_B_DEV", "/dev/disk/by-partlabel/rootfs_b", "/dev/mmcblk1p6"},
};

static void ota_progress_init(ota_progress_t *progress,
                              const char *partition,
                              const char *phase,
                              uint64_t total)
{
    if (progress == NULL) {
        return;
    }

    progress->partition = partition;
    progress->phase = phase;
    progress->total = total;
    progress->last_percent = 101U;
}

static void ota_progress_update(ota_progress_t *progress, uint64_t completed)
{
    char bar[OTA_PROGRESS_BAR_WIDTH + 1U];
    uint32_t percent;
    uint32_t filled;
    uint32_t index;

    if (progress == NULL || progress->total == 0U ||
        bh_ota_log_level_current() < BH_OTA_LOG_LEVEL_INFO) {
        return;
    }

    if (completed > progress->total) {
        completed = progress->total;
    }
    percent = (uint32_t)((completed * 100U) / progress->total);

    if (percent != 100U && progress->last_percent <= 100U &&
        percent / OTA_PROGRESS_PERCENT_STEP ==
            progress->last_percent / OTA_PROGRESS_PERCENT_STEP) {
        return;
    }

    filled = percent * OTA_PROGRESS_BAR_WIDTH / 100U;
    for (index = 0U; index < OTA_PROGRESS_BAR_WIDTH; ++index) {
        bar[index] = index < filled ? '#' : '-';
    }
    bar[OTA_PROGRESS_BAR_WIDTH] = '\0';

    fprintf(stdout,
            "[OTA_Package][PROGRESS] partition='%s' phase='%s' [%s] %3u%% (%llu/%llu bytes)\n",
            progress->partition,
            progress->phase,
            bar,
            percent,
            (unsigned long long)completed,
            (unsigned long long)progress->total);
    fflush(stdout);
    progress->last_percent = percent;
}

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

static const ota_sd_partition_t *ota_find_sd_partition(const char *partition)
{
    size_t i;

    for (i = 0; i < sizeof(g_sd_partitions) / sizeof(g_sd_partitions[0]); ++i) {
        if (strcmp(partition, g_sd_partitions[i].partition) == 0) {
            return &g_sd_partitions[i];
        }
    }

    return NULL;
}

static int32_t ota_sd_partition_path(const char *partition, char *path, size_t path_len)
{
    const ota_sd_partition_t *mapping;
    const char *override;
    const char *resolved_path;
    int written;

    mapping = ota_find_sd_partition(partition);
    if (mapping == NULL) {
        PACKAGE_LOG_ERROR("Unsupported SD partition '%s'", partition);
        return BH_OTA_ERROR;
    }

    override = getenv(mapping->override_env);
    if (override != NULL && override[0] != '\0') {
        resolved_path = override;
    } else if (access(mapping->label_device, F_OK) == 0) {
        resolved_path = mapping->label_device;
    } else {
        resolved_path = mapping->legacy_device;
        PACKAGE_LOG_INFO("PARTLABEL device unavailable for '%s', using legacy path '%s'",
                         partition, resolved_path);
    }
    written = snprintf(path, path_len, "%s", resolved_path);
    if (written <= 0 || (size_t)written >= path_len) {
        return BH_OTA_ERROR;
    }

    return BH_OTA_OK;
}

static int32_t ota_get_file_size(int32_t fd, uint64_t *size_out)
{
    struct stat st;

    if (fd < 0 || size_out == NULL) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    if (ioctl(fd, BLKGETSIZE64, size_out) == 0) {
        return BH_OTA_OK;
    }

    if (fstat(fd, &st) != 0) {
        PACKAGE_LOG_ERROR("Failed to query file size");
        return BH_OTA_ERROR;
    }

    *size_out = (uint64_t)st.st_size;
    return BH_OTA_OK;
}

static int32_t ota_compare_streams(int32_t fd_a,
                                   int32_t fd_b,
                                   uint64_t bytes,
                                   const char *partition)
{
    uint8_t *buf_a;
    uint8_t *buf_b;
    uint64_t completed = 0U;
    ota_progress_t progress;
    int32_t ret = BH_OTA_OK;

    buf_a = (uint8_t *)malloc(COPY_CHUNK_SIZE);
    buf_b = (uint8_t *)malloc(COPY_CHUNK_SIZE);
    if (buf_a == NULL || buf_b == NULL) {
        free(buf_a);
        free(buf_b);
        PACKAGE_LOG_ERROR("Failed to allocate compare buffers");
        return BH_OTA_ERROR;
    }

    ota_progress_init(&progress, partition, "verify", bytes);
    ota_progress_update(&progress, 0U);

    while (bytes > 0U) {
        size_t chunk_size = (bytes > COPY_CHUNK_SIZE) ? COPY_CHUNK_SIZE : (size_t)bytes;
        ssize_t read_a = read(fd_a, buf_a, chunk_size);
        ssize_t read_b = read(fd_b, buf_b, chunk_size);

        if (read_a != (ssize_t)chunk_size || read_b != (ssize_t)chunk_size) {
            PACKAGE_LOG_ERROR("Read-back verification failed during compare");
            ret = BH_OTA_ERROR;
            break;
        }

        if (memcmp(buf_a, buf_b, chunk_size) != 0) {
            PACKAGE_LOG_ERROR("Read-back verification mismatch detected");
            ret = BH_OTA_ERROR;
            break;
        }

        bytes -= chunk_size;
        completed += chunk_size;
        ota_progress_update(&progress, completed);
    }

    free(buf_a);
    free(buf_b);
    return ret;
}

static int32_t ota_write_block_partition(const char *image_path,
                                         const char *partition,
                                         uint32_t partition_size)
{
    char device_path[BH_OTA_PATH_MAX];
    int32_t src_fd = -1;
    int32_t dst_fd = -1;
    uint8_t *buf = NULL;
    uint64_t src_size;
    uint64_t dst_size;
    uint64_t completed = 0U;
    ota_progress_t progress;
    int32_t ret = BH_OTA_ERROR;

    if (ota_sd_partition_path(partition, device_path, sizeof(device_path)) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    src_fd = open(image_path, O_RDONLY);
    if (src_fd < 0) {
        PACKAGE_LOG_ERROR("Failed to open payload '%s'", image_path);
        return BH_OTA_ERROR;
    }

    dst_fd = open(device_path, O_RDWR);
    if (dst_fd < 0) {
        PACKAGE_LOG_ERROR("Failed to open block partition '%s'", device_path);
        goto cleanup;
    }

    if (ota_get_file_size(src_fd, &src_size) != BH_OTA_OK ||
        ota_get_file_size(dst_fd, &dst_size) != BH_OTA_OK) {
        goto cleanup;
    }

    if (partition_size == 0U) {
        PACKAGE_LOG_ERROR("Block payload '%s' has invalid partition_size=0", partition);
        goto cleanup;
    }

    if (src_size > (uint64_t)partition_size) {
        PACKAGE_LOG_ERROR("Payload '%s' is larger than declared partition_size for '%s'",
                          image_path, partition);
        goto cleanup;
    }

    if ((uint64_t)partition_size > dst_size) {
        PACKAGE_LOG_ERROR("Declared partition_size for '%s' exceeds destination device size", partition);
        goto cleanup;
    }

    if (src_size > dst_size) {
        PACKAGE_LOG_ERROR("Payload '%s' is larger than block partition '%s'", image_path, partition);
        goto cleanup;
    }

    buf = (uint8_t *)malloc(COPY_CHUNK_SIZE);
    if (buf == NULL) {
        PACKAGE_LOG_ERROR("Failed to allocate copy buffer");
        goto cleanup;
    }

    if (lseek(src_fd, 0, SEEK_SET) == (off_t)-1 || lseek(dst_fd, 0, SEEK_SET) == (off_t)-1) {
        PACKAGE_LOG_ERROR("lseek failed before writing block partition '%s'", partition);
        goto cleanup;
    }

    ota_progress_init(&progress, partition, "write", src_size);
    ota_progress_update(&progress, 0U);

    while (1) {
        ssize_t read_size = read(src_fd, buf, COPY_CHUNK_SIZE);
        ssize_t written = 0;

        if (read_size < 0) {
            PACKAGE_LOG_ERROR("Failed to read payload '%s'", image_path);
            goto cleanup;
        }
        if (read_size == 0) {
            break;
        }

        while (written < read_size) {
            ssize_t write_size = write(dst_fd,
                                       buf + written,
                                       (size_t)(read_size - written));
            if (write_size <= 0) {
                PACKAGE_LOG_ERROR("Failed to write block partition '%s'", partition);
                goto cleanup;
            }
            written += write_size;
            completed += (uint64_t)write_size;
            ota_progress_update(&progress, completed);
        }
    }

    if (completed != src_size) {
        PACKAGE_LOG_ERROR("Block payload copy ended at an unexpected size: partition='%s' expected=%llu actual=%llu",
                          partition,
                          (unsigned long long)src_size,
                          (unsigned long long)completed);
        goto cleanup;
    }

    if (src_size < (uint64_t)partition_size) {
        uint64_t zero_total = (uint64_t)partition_size - src_size;
        uint64_t remaining = zero_total;
        uint64_t zero_completed = 0U;

        memset(buf, 0, COPY_CHUNK_SIZE);
        ota_progress_init(&progress, partition, "zero-fill", zero_total);
        ota_progress_update(&progress, 0U);
        while (remaining > 0U) {
            size_t chunk_size = (remaining > COPY_CHUNK_SIZE) ? COPY_CHUNK_SIZE : (size_t)remaining;
            size_t chunk_written = 0U;

            while (chunk_written < chunk_size) {
                ssize_t write_size = write(dst_fd,
                                           buf + chunk_written,
                                           chunk_size - chunk_written);
                if (write_size <= 0) {
                    PACKAGE_LOG_ERROR("Failed to zero-fill tail of block partition '%s'", partition);
                    goto cleanup;
                }
                chunk_written += (size_t)write_size;
                remaining -= (uint64_t)write_size;
                zero_completed += (uint64_t)write_size;
                ota_progress_update(&progress, zero_completed);
            }
        }
    }

    PACKAGE_LOG_INFO("Syncing block payload after buffered write: partition='%s'", partition);
    if (fsync(dst_fd) != 0) {
        PACKAGE_LOG_ERROR("fsync failed for block partition '%s'", partition);
        goto cleanup;
    }

    if (lseek(src_fd, 0, SEEK_SET) == (off_t)-1 || lseek(dst_fd, 0, SEEK_SET) == (off_t)-1) {
        PACKAGE_LOG_ERROR("lseek failed before read-back verification");
        goto cleanup;
    }

    if (ota_compare_streams(src_fd, dst_fd, src_size, partition) != BH_OTA_OK) {
        goto cleanup;
    }

    PACKAGE_LOG_INFO("Block payload written successfully: partition='%s' device='%s' payload_size=%llu partition_size=%u",
                     partition,
                     device_path,
                     (unsigned long long)src_size,
                     partition_size);
    ret = BH_OTA_OK;

cleanup:
    free(buf);
    if (src_fd >= 0) {
        close(src_fd);
    }
    if (dst_fd >= 0) {
        close(dst_fd);
    }
    return ret;
}

static int32_t ota_flash_partition_from_file(const char *image_path, const char *partition)
{
    int32_t fd = -1;
    uint8_t *buf = NULL;
    off_t file_size;
    int32_t ret = BH_OTA_ERROR;

    fd = open(image_path, O_RDONLY);
    if (fd < 0) {
        PACKAGE_LOG_ERROR("Failed to open payload '%s'", image_path);
        return BH_OTA_ERROR;
    }

    file_size = lseek(fd, 0, SEEK_END);
    if (file_size <= 0 || lseek(fd, 0, SEEK_SET) == (off_t)-1) {
        PACKAGE_LOG_ERROR("Failed to determine payload size for '%s'", image_path);
        goto cleanup;
    }

    buf = (uint8_t *)malloc((size_t)file_size);
    if (buf == NULL) {
        PACKAGE_LOG_ERROR("Failed to allocate flash buffer for '%s'", image_path);
        goto cleanup;
    }

    if (read(fd, buf, (size_t)file_size) != file_size) {
        PACKAGE_LOG_ERROR("Failed to read payload '%s'", image_path);
        goto cleanup;
    }

    ret = bh_hal_ota_flash_fsi_image(buf, (uint32_t)file_size, partition);
    if (ret == BH_OTA_OK) {
        PACKAGE_LOG_INFO("Flash payload written successfully: partition='%s'", partition);
    }

cleanup:
    free(buf);
    if (fd >= 0) {
        close(fd);
    }
    return ret;
}

static int32_t ota_partition_get_class(const char *partition, bh_ota_payload_class_t *payload_class)
{
    if (partition == NULL || payload_class == NULL) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    if (strncmp(partition, "boot_", 5) == 0 ||
        strncmp(partition, "rootfs_", 7) == 0) {
        *payload_class = BH_OTA_PAYLOAD_CLASS_SD_BLOCK;
        return BH_OTA_OK;
    }

    *payload_class = BH_OTA_PAYLOAD_CLASS_FLASH;
    return BH_OTA_OK;
}

int32_t bh_ota_package_apply_payload(const char *package_dir,
                                     const bh_ota_manifest_payload_t *payload)
{
    char payload_path[BH_OTA_PATH_MAX];
    bh_ota_payload_class_t payload_class;

    if (package_dir == NULL || payload == NULL) {
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    if (ota_path_join(payload_path, sizeof(payload_path), package_dir, payload->file) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    if (ota_partition_get_class(payload->partition, &payload_class) != BH_OTA_OK) {
        return BH_OTA_ERROR;
    }

    PACKAGE_LOG_INFO("Applying payload: partition='%s' file='%s'", payload->partition, payload->file);

    if (payload_class == BH_OTA_PAYLOAD_CLASS_SD_BLOCK) {
        return ota_write_block_partition(payload_path, payload->partition, payload->partition_size);
    }

    return ota_flash_partition_from_file(payload_path, payload->partition);
}
