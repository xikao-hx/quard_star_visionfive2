#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <linux/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include "bh_ota_hal.h"
#include "partition.h"
#include "ota_info_log.h"

#define MAX_PARTITIONS 100
#define MAX_LINE_LENGTH 256
#define BANK_A 0
#define BANK_B 1
#define MAX_PARTITION_NAME_LEN 32

#define BUF_SIZE 4096

#define RECOVERY_PART "recovery"

#define MEMERASE		_IOW('M', 2, struct erase_info_user)

#define VERSION_USER 0


struct erase_info_user {
	uint32_t start;
	uint32_t length;
};

static int32_t convert_ab(const char *bank)
{
	int32_t val = -1; 

    if (strcmp(bank, "ab") == 0) {
		val = 0x3;
	} else if (strcmp(bank, "a") == 0) {
		val = 0x1;
	} else if (strcmp(bank, "b") == 0) {
		val = 0x2;
	}

	return val;
}

ssize_t get_file_size_stat(const char *path) 
{
    struct stat st;

    if (stat(path, &st) == -1) {
        OTA_INFO_LOG_ERROR("cmd=file_size result=failed path=%s", path);
        return -1;
    }
    return st.st_size;
}

#if 1
typedef enum {
		OPT_READ,
		OPT_DUMP_STATE,
		OPT_WRITE,
		OPT_RECOVER_BANK,
		OPT_RESET,
		OPT_SYNC,
		OPT_REPAIR,
		OPT_FLASH,
		OPT_VALID,
		OPT_READ_PARTITION,
		OPT_ERASE_PARTITION,
		OPT_WRITE_PARTITION,
} option_t;

typedef enum {
	BANK_UNKNOWN,
    BANK_CURRENT,
    BANK_TARGET,
    BANK_USABLE,
    BANK_OTA_STATE,
    BANK_SESSION_ID,
    BANK_ROLLBACK_INDEX,
    BANK_PENDING_ROLLBACK_INDEX,
    BANK_CURRENT_SYS_VERSION,
    BANK_PENDING_SYS_VERSION,
    BANK_CURRENT_IMAGE_VERSION,
    BANK_PENDING_IMAGE_VERSION,
    BANK_BOOT_SUCCESS,
    BANK_SUCCESSFUL_BANK_MASK,
} bank_type_t;
#endif

#define PROGRAM_NAME "ota_info" 

static void showusage(void)
{
	fprintf(stderr, "usage:\n"
				"       %s read <current_bank|target_bank|usable_bank>\n"
				"       %s dump-state\n"
				"       %s write <current_bank|target_bank> <a|b>\n"
				"       %s write <usable_bank|successful_bank_mask> <a|b|ab>\n"
				"       %s write <ota_state> <state_name|state_value>\n"
				"       %s recover-bank <a|b>\n"
				"       %s reset \n",
				PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME);

	fprintf(stderr,
				"       %s sync - sync from bl1_a to bl1_b\n"
				"       %s repair - copy from bl1_b to bl1_a\n",
			PROGRAM_NAME, PROGRAM_NAME);
	exit(EXIT_FAILURE);
}

static bank_type_t get_bank_type(const char* obj) {
    if (strcmp(obj, "current_bank") == 0) return BANK_CURRENT;
    else if (strcmp(obj, "target_bank") == 0) return BANK_TARGET;
    else if (strcmp(obj, "usable_bank") == 0) return BANK_USABLE;
    else if (strcmp(obj, "ota_state") == 0) return BANK_OTA_STATE;
    else if (strcmp(obj, "session_id") == 0) return BANK_SESSION_ID;
    else if (strcmp(obj, "rollback_index") == 0) return BANK_ROLLBACK_INDEX;
    else if (strcmp(obj, "pending_rollback_index") == 0) return BANK_PENDING_ROLLBACK_INDEX;
    else if (strcmp(obj, "current_sys_version") == 0) return BANK_CURRENT_SYS_VERSION;
    else if (strcmp(obj, "pending_sys_version") == 0) return BANK_PENDING_SYS_VERSION;
    else if (strcmp(obj, "current_image_version") == 0) return BANK_CURRENT_IMAGE_VERSION;
    else if (strcmp(obj, "pending_image_version") == 0) return BANK_PENDING_IMAGE_VERSION;
    else if (strcmp(obj, "boot_success") == 0) return BANK_BOOT_SUCCESS;
    else if (strcmp(obj, "successful_bank_mask") == 0) return BANK_SUCCESSFUL_BANK_MASK;
    else return BANK_UNKNOWN;
}

static const char *ota_state_to_string(uint32_t state)
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

static int32_t parse_ota_state_value(const char *value, uint32_t *state_out)
{
    char *end = NULL;
    unsigned long parsed;

    if (value == NULL || state_out == NULL) {
        return -1;
    }

    if (strcmp(value, "idle") == 0) {
        *state_out = BH_OTA_STATE_IDLE;
        return 0;
    } else if (strcmp(value, "failed") == 0) {
        *state_out = BH_OTA_STATE_FAILED;
        return 0;
    } else if (strcmp(value, "stage1_start") == 0) {
        *state_out = BH_OTA_STATE_STAGE1_START;
        return 0;
    } else if (strcmp(value, "stage1_wrote") == 0) {
        *state_out = BH_OTA_STATE_STAGE1_WROTE;
        return 0;
    } else if (strcmp(value, "stage1_end") == 0) {
        *state_out = BH_OTA_STATE_STAGE1_END;
        return 0;
    } else if (strcmp(value, "stage2_start") == 0) {
        *state_out = BH_OTA_STATE_STAGE2_START;
        return 0;
    } else if (strcmp(value, "stage2_wrote") == 0) {
        *state_out = BH_OTA_STATE_STAGE2_WROTE;
        return 0;
    } else if (strcmp(value, "stage2_end") == 0) {
        *state_out = BH_OTA_STATE_STAGE2_END;
        return 0;
    } else if (strcmp(value, "complete") == 0) {
        *state_out = BH_OTA_STATE_COMPLETE;
        return 0;
    }

    errno = 0;
    parsed = strtoul(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0' || parsed > UINT32_MAX) {
        return -1;
    }

    *state_out = (uint32_t)parsed;
    return 0;
}

static void print_bank_mask_value(uint32_t bank_mask)
{
    if (bank_mask == 0x1) {
        printf("a\n");
    } else if (bank_mask == 0x2) {
        printf("b\n");
    } else if (bank_mask == 0x3) {
        printf("ab\n");
    } else {
        printf("0x%x\n", bank_mask);
    }
}

static int32_t ota_dump_state(void)
{
    recovery_config_t recovery;
    uint32_t target_mask;
    uint32_t recoverable_mask;
    int pending_boot_confirmation;
    int32_t ret;

    ret = bh_hal_ota_recovery_read(&recovery);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    printf("magic=0x%08x\n", recovery.magic);
    printf("struct_version=%u\n", recovery.struct_version);
    printf("usable_bank=0x%x\n", recovery.usable_bank);
    printf("current_bank=%u\n", recovery.current_bank);
    printf("target_bank=%u\n", recovery.target_bank);
    printf("ota_reboot_cnt=%u\n", recovery.ota_reboot_cnt);
    printf("ota_update=%u\n", recovery.ota_update);
    printf("ota_state=%u (%s)\n", recovery.ota_state, ota_state_to_string(recovery.ota_state));
    printf("session_id_crc32=0x%08x\n", recovery.session_id_crc32);
    printf("successful_bank_mask=0x%x\n", recovery.successful_bank_mask);
    printf("boot_success=%u\n", recovery.boot_success);
    printf("rollback_index=%u\n", recovery.rollback_index);
    printf("pending_rollback_index=%u\n", recovery.pending_rollback_index);
    printf("current_image_version=%u\n", recovery.current_image_version);
    printf("pending_image_version=%u\n", recovery.pending_image_version);
    printf("session_id=%s\n", recovery.session_id[0] ? recovery.session_id : "");
    printf("current_sys_version=%s\n", recovery.current_sys_version[0] ? recovery.current_sys_version : "");
    printf("pending_sys_version=%s\n", recovery.pending_sys_version[0] ? recovery.pending_sys_version : "");
    printf("header_crc32=0x%08x\n", recovery.header_crc32);

    target_mask = 1U << (recovery.target_bank & 0x1U);
    recoverable_mask = recovery.successful_bank_mask & recovery.usable_bank & 0x3U;
    pending_boot_confirmation = recovery.ota_state == BH_OTA_STATE_STAGE1_END &&
                                (recovery.successful_bank_mask & target_mask) == 0U;

    if (recovery.ota_state == BH_OTA_STATE_FAILED) {
        printf("recovery_status=failed_manual_recovery_required\n");
        printf("recoverable_bank_mask=0x%x\n", recoverable_mask);
    } else if (pending_boot_confirmation) {
        printf("recovery_status=pending_boot_confirmation\n");
        printf("pending_boot_attempts=%u/%u\n", recovery.ota_reboot_cnt, OTA_BOOT_RETRY_LIMIT);
        printf("pending_boot_rollback_ready=%u\n",
               recovery.ota_reboot_cnt >= OTA_BOOT_RETRY_LIMIT && recoverable_mask != 0U ? 1U : 0U);
    } else {
        printf("recovery_status=normal\n");
    }

    return BH_OTA_OK;
}

int32_t ota_read(const char* obj)
{
    bank_type_t bank_type = get_bank_type(obj);
	int32_t bank = -1;
	int32_t ret  = -1;
    recovery_config_t recovery;

    switch(bank_type) {
        case BANK_CURRENT:
            ret = bh_hal_ota_get_current_bank(&bank);
			break;
        case BANK_TARGET:
			ret = bh_hal_ota_get_target_bank(&bank);
			break;
        case BANK_USABLE:
			ret = bh_hal_ota_get_usable_bank(&bank);
			break;
        case BANK_OTA_STATE:
        case BANK_SESSION_ID:
        case BANK_ROLLBACK_INDEX:
        case BANK_PENDING_ROLLBACK_INDEX:
        case BANK_CURRENT_SYS_VERSION:
        case BANK_PENDING_SYS_VERSION:
        case BANK_CURRENT_IMAGE_VERSION:
        case BANK_PENDING_IMAGE_VERSION:
        case BANK_BOOT_SUCCESS:
        case BANK_SUCCESSFUL_BANK_MASK:
            ret = bh_hal_ota_recovery_read(&recovery);
            if (ret != BH_OTA_OK) {
                return ret;
            }

            switch (bank_type) {
                case BANK_OTA_STATE:
                    printf("%u (%s)\n", recovery.ota_state, ota_state_to_string(recovery.ota_state));
                    break;
                case BANK_SESSION_ID:
                    printf("%s\n", recovery.session_id[0] ? recovery.session_id : "");
                    break;
                case BANK_ROLLBACK_INDEX:
                    printf("%u\n", recovery.rollback_index);
                    break;
                case BANK_PENDING_ROLLBACK_INDEX:
                    printf("%u\n", recovery.pending_rollback_index);
                    break;
                case BANK_CURRENT_SYS_VERSION:
                    printf("%s\n", recovery.current_sys_version[0] ? recovery.current_sys_version : "");
                    break;
                case BANK_PENDING_SYS_VERSION:
                    printf("%s\n", recovery.pending_sys_version[0] ? recovery.pending_sys_version : "");
                    break;
                case BANK_CURRENT_IMAGE_VERSION:
                    printf("%u\n", recovery.current_image_version);
                    break;
                case BANK_PENDING_IMAGE_VERSION:
                    printf("%u\n", recovery.pending_image_version);
                    break;
                case BANK_BOOT_SUCCESS:
                    printf("%u\n", recovery.boot_success);
                    break;
                case BANK_SUCCESSFUL_BANK_MASK:
                    print_bank_mask_value(recovery.successful_bank_mask);
                    break;
                default:
                    break;
            }
            ret = BH_OTA_OK;
            break;
        default:
            OTA_INFO_LOG_ERROR("cmd=read result=failed reason=unknown_bank_type object=%s", obj);
			break;
    }

	return ret;
}


int32_t ota_write(const char* obj, const char *ab)
{
	    bank_type_t bank_type = get_bank_type(obj);
		int32_t bank_mask = -1;
		int32_t ret = -1;
		uint32_t state;
		recovery_config_t recovery;

	    switch(bank_type) {
	        case BANK_CURRENT:
				bank_mask = convert_ab(ab);
				if (bank_mask < 0) {
					OTA_INFO_LOG_ERROR("cmd=write result=failed reason=invalid_bank bank=%s", ab);
					return -1;
				}
				if (bank_mask != 3) {
					ret = bh_hal_ota_set_current_bank(bank_mask - 1);
				} else {
				OTA_INFO_LOG_ERROR("cmd=write result=failed reason=invalid_bank bank=%s", ab);
				}
				break;
	        case BANK_TARGET:
				bank_mask = convert_ab(ab);
				if (bank_mask < 0) {
					OTA_INFO_LOG_ERROR("cmd=write result=failed reason=invalid_bank bank=%s", ab);
					return -1;
				}
				if (bank_mask != 3) {
					ret = bh_hal_ota_switch_bank(bank_mask - 1);
				} else {
				OTA_INFO_LOG_ERROR("cmd=write result=failed reason=invalid_bank bank=%s", ab);
				}
				break;
	        case BANK_USABLE:
				bank_mask = convert_ab(ab);
				if (bank_mask < 0) {
					OTA_INFO_LOG_ERROR("cmd=write result=failed reason=invalid_bank bank=%s", ab);
					return -1;
				}
				ret = bh_hal_ota_set_usable_bank(bank_mask);
				break;
	        case BANK_SUCCESSFUL_BANK_MASK:
				bank_mask = convert_ab(ab);
				if (bank_mask < 0) {
					OTA_INFO_LOG_ERROR("cmd=write result=failed reason=invalid_bank bank=%s", ab);
					return -1;
				}
				ret = bh_hal_ota_recovery_read(&recovery);
				if (ret == BH_OTA_OK) {
					recovery.successful_bank_mask = (uint32_t)bank_mask;
					ret = bh_hal_ota_recovery_write(&recovery);
				}
				break;
	        case BANK_OTA_STATE:
				if (parse_ota_state_value(ab, &state) != 0) {
					OTA_INFO_LOG_ERROR("cmd=write result=failed reason=invalid_ota_state state=%s", ab);
					return -1;
				}
				ret = bh_hal_ota_recovery_read(&recovery);
				if (ret == BH_OTA_OK) {
					recovery.ota_state = state;
					ret = bh_hal_ota_recovery_write(&recovery);
				}
				break;
	        default:
	            OTA_INFO_LOG_ERROR("cmd=write result=failed reason=unknown_bank_type object=%s", obj);
				break;
    }

    if (ret == BH_OTA_OK) {
        OTA_INFO_LOG_INFO("cmd=write result=success object=%s bank=%s", obj, ab);
    }

	return ret;
}

static int32_t ota_recover_bank(const char *bank_arg)
{
    recovery_config_t recovery;
    int32_t bank_mask;
    uint32_t bank;
    int32_t ret;

    bank_mask = convert_ab(bank_arg);
    if (bank_mask != 0x1 && bank_mask != 0x2) {
        OTA_INFO_LOG_ERROR("cmd=recover-bank result=failed reason=invalid_bank bank=%s", bank_arg);
        return BH_OTA_ERROR_INVALID_PARAM;
    }

    ret = bh_hal_ota_recovery_read(&recovery);
    if (ret != BH_OTA_OK) {
        return ret;
    }

    if (recovery.ota_state != BH_OTA_STATE_FAILED) {
        OTA_INFO_LOG_ERROR("cmd=recover-bank result=failed reason=state_not_failed ota_state=%u",
                           recovery.ota_state);
        return BH_OTA_ERROR;
    }

    if ((recovery.successful_bank_mask & (uint32_t)bank_mask) == 0U) {
        OTA_INFO_LOG_ERROR("cmd=recover-bank result=failed reason=bank_not_confirmed bank=%s successful_bank_mask=0x%x",
                           bank_arg, recovery.successful_bank_mask);
        return BH_OTA_ERROR;
    }

    if ((recovery.usable_bank & (uint32_t)bank_mask) == 0U) {
        OTA_INFO_LOG_ERROR("cmd=recover-bank result=failed reason=bank_not_usable bank=%s usable_bank=0x%x",
                           bank_arg, recovery.usable_bank);
        return BH_OTA_ERROR;
    }

    bank = bank_mask == 0x1 ? BANK_A : BANK_B;
    recovery.current_bank = bank;
    recovery.target_bank = bank;
    recovery.usable_bank &= recovery.successful_bank_mask & 0x3U;
    recovery.usable_bank |= (uint32_t)bank_mask;
    recovery.ota_reboot_cnt = 0U;
    recovery.ota_state = BH_OTA_STATE_IDLE;

    ret = bh_hal_ota_recovery_write(&recovery);
    if (ret == BH_OTA_OK) {
        OTA_INFO_LOG_INFO("cmd=recover-bank result=success bank=%s usable_bank=0x%x ota_state=idle",
                          bank_arg, recovery.usable_bank);
    }

    return ret;
}

ssize_t get_file_size_fstat(int32_t fd)
{
    struct stat st;

    if (fstat(fd, &st) == 0) {
        return st.st_size;
    }

    return -1;
}


int32_t ota_flash(const char* filename, const char *partition)
{
    int32_t fd;
    ssize_t size, size_read;
	uint8_t *buf;
    int32_t ret;

	fd = open(filename, O_RDONLY);
    if (fd < 0) {
        OTA_INFO_LOG_ERROR("cmd=flash result=failed reason=open_source_failed file=%s", filename);
		return BH_OTA_ERROR;
    }

    size = get_file_size_fstat(fd);
	if (size < 0) {
        OTA_INFO_LOG_ERROR("cmd=flash result=failed reason=get_file_size_failed file=%s", filename);
        close(fd);
		return BH_OTA_ERROR;
	}

	buf = (uint8_t *)malloc(size);
    if (buf == NULL) {
		ret = BH_OTA_ERROR;
		goto close_fd;
    }

    size_read = read(fd, buf, size);
	if (size_read != size) {
		ret = BH_OTA_ERROR;
		goto error;
	}

	ret = bh_hal_ota_flash_fsi_image(buf, size_read, partition);
	if (ret) {
		OTA_INFO_LOG_ERROR("cmd=flash result=failed partition=%s file=%s", partition, filename);
	} else {
		OTA_INFO_LOG_INFO("cmd=flash result=success partition=%s file=%s bytes=%zd",
                          partition, filename, size_read);
	}

error:
	free(buf);
close_fd:
	close(fd);
	return ret;
}

/* 读取分区内容并保存到文件 */
int32_t ota_read_partition(const char* filename, const char *partition)
{
    int32_t fd_file, fd_part;
    uint32_t part_size;
    uint8_t *buf;
    char mtd_path[MAX_PARTITION_NAME_LEN];
    int32_t ret = 0;

    // 获取分区大小 (利用 HAL 提供的接口)
    if (get_partition_size_by_name(partition, TYPE_FLASH, &part_size) != 0) {
        OTA_INFO_LOG_ERROR("cmd=read_partition result=failed reason=partition_not_found partition=%s", partition);
        return -1;
    }

    snprintf(mtd_path, sizeof(mtd_path), "/dev/mtd/by-name/%s", partition);
    fd_part = open(mtd_path, O_RDONLY);
    if (fd_part < 0) {
        OTA_INFO_LOG_ERROR("cmd=read_partition result=failed reason=open_partition_failed partition=%s", partition);
        return -1;
    }

    fd_file = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_file < 0) {
        OTA_INFO_LOG_ERROR("cmd=read_partition result=failed reason=open_output_failed file=%s", filename);
        close(fd_part);
        return -1;
    }

    buf = malloc(part_size);
    if (!buf) {
        OTA_INFO_LOG_ERROR("cmd=read_partition result=failed reason=memory_allocation_failed partition=%s", partition);
        close(fd_part);
        close(fd_file);
        return -1;
    }

    if (read(fd_part, buf, part_size) != part_size) {
        OTA_INFO_LOG_ERROR("cmd=read_partition result=failed reason=read_partition_failed partition=%s", partition);
        ret = -1;
    } else if (write(fd_file, buf, part_size) != part_size) {
        OTA_INFO_LOG_ERROR("cmd=read_partition result=failed reason=write_file_failed file=%s", filename);
        ret = -1;
    } else {
        OTA_INFO_LOG_INFO("cmd=read_partition result=success partition=%s file=%s bytes=%u",
                          partition, filename, part_size);
    }

    free(buf);
    close(fd_part);
    close(fd_file);
    return ret;
}

/* 擦除指定分区 */
int32_t ota_erase_partition(const char *partition)
{
    int32_t fd;
    uint32_t part_size;
    char mtd_path[MAX_PARTITION_NAME_LEN];
    int32_t ret;

    if (get_partition_size_by_name(partition, TYPE_FLASH, &part_size) != 0) {
        OTA_INFO_LOG_ERROR("cmd=erase_partition result=failed reason=partition_not_found partition=%s", partition);
        return -1;
    }

    snprintf(mtd_path, sizeof(mtd_path), "/dev/mtd/by-name/%s", partition);
    fd = open(mtd_path, O_RDWR | O_SYNC);
    if (fd < 0) {
        OTA_INFO_LOG_ERROR("cmd=erase_partition result=failed reason=open_partition_failed partition=%s", partition);
        return -1;
    }

    // 调用 bh_ota_hal.c 中的逻辑（此处需确保 bh_ota_hal.h 暴露了 erase 接口或直接实现）
    // 为了保持一致性，我们模拟 HAL 的擦除调用
    struct erase_info_user erase;
    erase.start = 0;
    erase.length = part_size; 

    ret = ioctl(fd, _IOW('M', 2, struct erase_info_user), &erase);
    if (ret < 0) {
        OTA_INFO_LOG_ERROR("cmd=erase_partition result=failed partition=%s", partition);
    } else {
        OTA_INFO_LOG_INFO("cmd=erase_partition result=success partition=%s bytes=%u", partition, part_size);
    }

    close(fd);
    return ret;
}

/**
 * @brief 单独的写功能：将本地文件直接写入指定分区
 * @param filename 源文件名
 * @param partition 目标分区名（如 "kernel", "rootfs"）
 */
int32_t ota_write_partition(const char* filename, const char *partition)
{
    int32_t fd_file = -1, fd_part = -1;
    uint32_t part_size = 0;
    ssize_t file_size = 0;
    uint8_t *buf = NULL;
    char mtd_path[MAX_PARTITION_NAME_LEN];
    int32_t ret = 0;

    // 1. 获取分区信息并检查
    if (get_partition_size_by_name(partition, TYPE_FLASH, &part_size) != 0) {
        OTA_INFO_LOG_ERROR("cmd=write_partition result=failed reason=partition_not_found partition=%s", partition);
        return -1;
    }

    // 2. 打开源文件并检查大小
    fd_file = open(filename, O_RDONLY);
    if (fd_file < 0) {
        OTA_INFO_LOG_ERROR("cmd=write_partition result=failed reason=open_source_failed file=%s", filename);
        return -1;
    }
    file_size = lseek(fd_file, 0, SEEK_END);
    lseek(fd_file, 0, SEEK_SET);

    if (file_size > part_size) {
        OTA_INFO_LOG_ERROR("cmd=write_partition result=failed reason=file_too_large file=%s file_size=%zd partition_size=%u",
                           filename, file_size, part_size);
        close(fd_file);
        return -1;
    }

    // 3. 打开目标 MTD 设备
    snprintf(mtd_path, sizeof(mtd_path), "/dev/mtd/by-name/%s", partition);
    fd_part = open(mtd_path, O_RDWR | O_SYNC);
    if (fd_part < 0) {
        OTA_INFO_LOG_ERROR("cmd=write_partition result=failed reason=open_partition_failed partition=%s", partition);
        close(fd_file);
        return -1;
    }

    // 4. 准备缓冲区并读取文件内容
    buf = malloc(file_size);
    if (!buf) {
        OTA_INFO_LOG_ERROR("cmd=write_partition result=failed reason=memory_allocation_failed partition=%s", partition);
        goto cleanup;
    }

    if (read(fd_file, buf, file_size) != file_size) {
        OTA_INFO_LOG_ERROR("cmd=write_partition result=failed reason=read_source_failed file=%s", filename);
        ret = -1;
        goto cleanup;
    }

    // 5. 擦除分区
    // 使用 HAL 中定义的逻辑执行擦除，确保写入前块是干净的
    struct erase_info_user erase;
    erase.start = 0;
    erase.length = (uint32_t)file_size; // 至少擦除文件大小对应的空间
    
    // 这里的 MEMERASE ioctl 是直接对驱动的操作
    if (ioctl(fd_part, _IOW('M', 2, struct erase_info_user), &erase) < 0) {
        OTA_INFO_LOG_ERROR("cmd=write_partition result=failed reason=erase_partition_failed partition=%s", partition);
        ret = -1;
        goto cleanup;
    }

    // 6. 写入数据
    if (write(fd_part, buf, file_size) != file_size) {
        OTA_INFO_LOG_ERROR("cmd=write_partition result=failed reason=write_partition_failed partition=%s", partition);
        ret = -1;
    } else {
        OTA_INFO_LOG_INFO("cmd=write_partition result=success file=%s partition=%s bytes=%zd",
                          filename, partition, file_size);
    }

cleanup:
    if (buf) free(buf);
    if (fd_file >= 0) close(fd_file);
    if (fd_part >= 0) close(fd_part);
    return ret;
}

int32_t ota_valid(const char* filename)
{
	int32_t ret;

	ret = bh_hal_ota_verify_image(filename);

	return ret;
}

#define CMDLINE_PATH "/proc/cmdline"
#define MAX_CMDLINE_SIZE 4096
static char cmdline[MAX_CMDLINE_SIZE];

int parse_active_bl1(const char *cmdline) {
    const char *token = "active_bl1=";
    const char *ptr = strstr(cmdline, token);
    
    if (ptr) {
        ptr += strlen(token);
        if (*ptr == '0' || *ptr == '1') {
            return *ptr - '0'; 
        }
    }
    return -1;
}

int32_t get_active_bl1(void)
{
    int fd;
    ssize_t bytes_read;
    const char *token = "active_bl1=";
	int32_t active_bl1 = -1;

    if ((fd = open(CMDLINE_PATH, O_RDONLY)) < 0) {
        OTA_INFO_LOG_ERROR("cmd=get_active_bl1 result=failed reason=open_cmdline_failed path=%s", CMDLINE_PATH);
        return -1;
    }

    bytes_read = read(fd, cmdline, MAX_CMDLINE_SIZE - 1);
    close(fd);
    
    if (bytes_read <= 0) {
        OTA_INFO_LOG_ERROR("cmd=get_active_bl1 result=failed reason=read_cmdline_failed path=%s", CMDLINE_PATH);
        return -1;
    }

    cmdline[bytes_read] = '\0';

    const char *ptr = strstr(cmdline, token);
    
    if (ptr) {
        ptr += strlen(token);
        if (*ptr == '0' || *ptr == '1') {
            active_bl1 = *ptr - '0'; 
        }
    }

    OTA_INFO_LOG_DEBUG("cmd=get_active_bl1 event=cmdline value=%s", cmdline);
	OTA_INFO_LOG_INFO("cmd=get_active_bl1 result=success active_bl1=%d", active_bl1);

	return active_bl1;
    
}

static int32_t ota_repair(void)
{
	int32_t active_bl1;
	int32_t ret = 0;

	ret = bh_hal_ota_get_bl1_bank(&active_bl1);
	if (ret == 0) {
		OTA_INFO_LOG_INFO("cmd=repair event=active_bl1 value=%d", active_bl1);
		if (active_bl1 == 1) {
			ret = bh_hal_ota_sync_bl1_image(1);
			if (ret == 0)
				OTA_INFO_LOG_INFO("cmd=repair result=success action=sync_bl1");
		} else {
			OTA_INFO_LOG_INFO("cmd=repair result=skip reason=no_need_to_repair");
		}
	} else {
		OTA_INFO_LOG_ERROR("cmd=repair result=failed reason=get_active_bl1_failed");
	}

	return ret;
}

static int32_t ota_sync(void)
{
	int32_t active_bl1;
	int32_t ret = 0;

	ret = bh_hal_ota_get_bl1_bank(&active_bl1);
	if (ret == 0) {
		OTA_INFO_LOG_INFO("cmd=sync event=active_bl1 value=%d", active_bl1);
		if (active_bl1 == 0) {
			ret = bh_hal_ota_sync_bl1_image(0);
			if (ret == 0)
				OTA_INFO_LOG_INFO("cmd=sync result=success action=sync_bl1");
		} else {
			OTA_INFO_LOG_INFO("cmd=sync result=skip reason=no_need_to_sync");
		}
	} else {
		OTA_INFO_LOG_ERROR("cmd=sync result=failed reason=get_active_bl1_failed");
	}

	return ret;
}

int32_t main(int32_t argc, char *argv[])
{
	int32_t err = 0;
	option_t option;

	if (argc == 3 && !strcmp(argv[1], "read"))
		option = OPT_READ;
	else if (argc == 2 && !strcmp(argv[1], "dump-state"))
		option = OPT_DUMP_STATE;
	else if (argc == 4 && !strcmp(argv[1], "write"))
		option = OPT_WRITE;
	else if (argc == 3 && !strcmp(argv[1], "recover-bank"))
		option = OPT_RECOVER_BANK;
	else if (argc == 2 && !strcmp(argv[1], "sync"))
		option = OPT_SYNC;
	else if (argc == 2 && !strcmp(argv[1], "repair"))
		option = OPT_REPAIR;
	else if (argc == 4 && !strcmp(argv[1], "flash"))
		option = OPT_FLASH;
	else if (argc == 3 && !strcmp(argv[1], "valid"))
		option = OPT_VALID;
	else if (argc == 2 && !strcmp(argv[1], "reset"))
		option = OPT_RESET;
	else if (argc == 4 && !strcmp(argv[1], "read_partition"))
		option = OPT_READ_PARTITION;
	else if (argc == 3 && !strcmp(argv[1], "erase_partition"))
		option = OPT_ERASE_PARTITION;
	else if (argc == 4 && !strcmp(argv[1], "write_partition"))
		option = OPT_WRITE_PARTITION;
	else
		showusage();

	err = bh_hal_ota_init();
	if (err) {
		return err;	
	}

	switch (option) {
		case OPT_READ:
			err = ota_read(argv[2]);
			break;
		case OPT_DUMP_STATE:
			err = ota_dump_state();
			break;
		case OPT_WRITE:
			err = ota_write(argv[2], argv[3]);
			break;
		case OPT_RECOVER_BANK:
			err = ota_recover_bank(argv[2]);
			break;
		case OPT_RESET:
			err = bh_hal_ota_reset();
			break;
		case OPT_REPAIR:
			err = ota_repair();
			break;
		case OPT_SYNC:
			err = ota_sync();
			break;
		case OPT_FLASH:
			err = ota_flash(argv[2], argv[3]);
			break;
		case OPT_VALID:
			err = ota_valid(argv[2]);
			break;
		case OPT_READ_PARTITION:
			err = ota_read_partition(argv[2], argv[3]);
			break;
		case OPT_ERASE_PARTITION:
			err = ota_erase_partition(argv[2]);
			break;
		case OPT_WRITE_PARTITION:
			err = ota_write_partition(argv[2], argv[3]);
			break;
	}

	return err;
}
