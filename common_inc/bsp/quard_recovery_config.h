#ifndef __QUARD_RECOVERY_CONFIG_H__
#define __QUARD_RECOVERY_CONFIG_H__

#include <stdint.h>

#define RECOVERY_MAGIC 0x424150A5U
#define RECOVERY_STRUCT_VERSION 2U
#define RECOVERY_SYS_VERSION_MAX_LEN 32U
#define RECOVERY_SESSION_ID_MAX_LEN 64U

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
    uint32_t magic;
    uint32_t struct_version;

    uint32_t usable_bank;
    uint32_t current_bank;
    uint32_t target_bank;

    uint32_t ota_reboot_cnt;
    uint32_t ota_update;

    uint32_t ota_state;
    uint32_t session_id_crc32;

    uint32_t successful_bank_mask;
    uint32_t boot_success;

    uint32_t rollback_index;
    uint32_t pending_rollback_index;

    uint32_t current_image_version;
    uint32_t pending_image_version;

    uint32_t reserved0;
    uint32_t reserved1;

    char session_id[RECOVERY_SESSION_ID_MAX_LEN];
    char current_sys_version[RECOVERY_SYS_VERSION_MAX_LEN];
    char pending_sys_version[RECOVERY_SYS_VERSION_MAX_LEN];

    uint32_t header_crc32;
} recovery_config_t __attribute__((aligned(8)));

#endif
