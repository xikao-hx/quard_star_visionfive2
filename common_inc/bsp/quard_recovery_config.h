#ifndef __QUARD_RECOVERY_CONFIG_H__
#define __QUARD_RECOVERY_CONFIG_H__

#ifdef __KERNEL__
#include <linux/stddef.h>
#include <linux/types.h>
typedef u32 quard_recovery_u32;
#else
#include <stddef.h>
#include <stdint.h>
typedef uint32_t quard_recovery_u32;
#endif

#define RECOVERY_MAGIC 0x424150A5U
#define RECOVERY_STRUCT_VERSION 2U
#define RECOVERY_SYS_VERSION_MAX_LEN 32U
#define RECOVERY_SESSION_ID_MAX_LEN 64U
#define RECOVERY_RECORD_SIZE 200U
#define RECOVERY_HEADER_CRC32_OFFSET 196U

#define RECOVERY_BANK_A 0U
#define RECOVERY_BANK_B 1U
#define RECOVERY_BANK_MASK_A (1U << RECOVERY_BANK_A)
#define RECOVERY_BANK_MASK_B (1U << RECOVERY_BANK_B)
#define RECOVERY_BANK_MASK_ALL (RECOVERY_BANK_MASK_A | RECOVERY_BANK_MASK_B)

#define OTA_STATE_IDLE 0U
#define OTA_STATE_FAILED 6U

#define OTA_STATE_STAGE1_START 11U
#define OTA_STATE_STAGE1_WROTE 12U
#define OTA_STATE_STAGE1_END 13U
#define OTA_STATE_STAGE2_START 14U
#define OTA_STATE_STAGE2_WROTE 15U
#define OTA_STATE_STAGE2_END 16U
#define OTA_STATE_COMPLETE 17U

#define OTA_BOOT_RETRY_LIMIT 3U

typedef struct recovery_config {
    quard_recovery_u32 magic;
    quard_recovery_u32 struct_version;

    quard_recovery_u32 usable_bank;
    quard_recovery_u32 current_bank;
    quard_recovery_u32 target_bank;

    quard_recovery_u32 ota_reboot_cnt;
    quard_recovery_u32 ota_update;

    quard_recovery_u32 ota_state;
    quard_recovery_u32 session_id_crc32;

    quard_recovery_u32 successful_bank_mask;
    quard_recovery_u32 boot_success;

    quard_recovery_u32 rollback_index;
    quard_recovery_u32 pending_rollback_index;

    quard_recovery_u32 current_image_version;
    quard_recovery_u32 pending_image_version;

    quard_recovery_u32 reserved0;
    quard_recovery_u32 reserved1;

    char session_id[RECOVERY_SESSION_ID_MAX_LEN];
    char current_sys_version[RECOVERY_SYS_VERSION_MAX_LEN];
    char pending_sys_version[RECOVERY_SYS_VERSION_MAX_LEN];

    quard_recovery_u32 header_crc32;
} recovery_config_t __attribute__((aligned(8)));

_Static_assert(offsetof(recovery_config_t, current_bank) == 12,
	       "recovery current_bank ABI changed");
_Static_assert(offsetof(recovery_config_t, target_bank) == 16,
	       "recovery target_bank ABI changed");
_Static_assert(offsetof(recovery_config_t, successful_bank_mask) == 36,
	       "recovery successful_bank_mask ABI changed");
_Static_assert(offsetof(recovery_config_t, header_crc32) ==
	       RECOVERY_HEADER_CRC32_OFFSET,
	       "recovery CRC offset ABI changed");
_Static_assert(sizeof(recovery_config_t) == RECOVERY_RECORD_SIZE,
	       "recovery record size ABI changed");

#endif
