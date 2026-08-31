#ifndef __BH_OTA_HAL_H__
#define __BH_OTA_HAL_H__

#include <stddef.h>
#include <stdint.h>
#include "../../../common_inc/bsp/quard_recovery_config.h"

#define BH_OTA_OK 0
#define BH_OTA_ERROR -1
#define BH_OTA_ERROR_READ -2
#define BH_OTA_ERROR_WRITE -3
#define BH_OTA_ERROR_ERASE -4
#define BH_OTA_ERROR_INVALID_PARAM -5

#define BH_OTA_RECOVERY_MAGIC RECOVERY_MAGIC
#define BH_OTA_RECOVERY_STRUCT_VERSION RECOVERY_STRUCT_VERSION
#define BH_OTA_SYSTEM_VERSION_PATH "/etc/version"
#define BH_OTA_SYS_VERSION_MAX_LEN RECOVERY_SYS_VERSION_MAX_LEN
#define BH_OTA_SESSION_ID_MAX_LEN RECOVERY_SESSION_ID_MAX_LEN
typedef enum {
    BH_OTA_STATE_IDLE = 0,
    BH_OTA_STATE_FAILED = OTA_STATE_FAILED,
    BH_OTA_STATE_STAGE1_START = OTA_STATE_STAGE1_START,
    BH_OTA_STATE_STAGE1_WROTE = OTA_STATE_STAGE1_WROTE,
    BH_OTA_STATE_STAGE1_END = OTA_STATE_STAGE1_END,
    BH_OTA_STATE_STAGE2_START = OTA_STATE_STAGE2_START,
    BH_OTA_STATE_STAGE2_WROTE = OTA_STATE_STAGE2_WROTE,
    BH_OTA_STATE_STAGE2_END = OTA_STATE_STAGE2_END,
    BH_OTA_STATE_COMPLETE = OTA_STATE_COMPLETE,
} bh_ota_state_t;

/**
 * @brief Initializes the OTA library, mainly collects nor-flash and emmc/ufs gpt info.
 *
 * bh_hal_ota_init is required before calling any other OTA functions 
 *
 * @return int32_t Returns 0 on success, negative value on failure.
 */
int32_t bh_hal_ota_init(void);

int32_t bh_hal_ota_recovery_read(recovery_config_t *cfg);
int32_t bh_hal_ota_recovery_write(const recovery_config_t *cfg);

/**
 * @brief Calculates the CRC32 checksum of the specified buffer.
 *
 * @param buf Pointer to the data buffer.
 * @param size Size of the data buffer in bytes.
 * @return uint32_t The calculated CRC32 checksum.
 */
uint32_t bh_hal_ota_crc32(const unsigned char *buf, uint32_t size);

/**
 * @brief Writes an OTA image into the specified Flash partition.
 *
 * @param buf_addr Pointer to the image data buffer.
 * @param buf_len Length of the image data.
 * @param partition Name of the target partition to write to, example bl2_a, kernel_b, etc.
 * @return int32_t Returns 0 on success, negative value on failure.
 */
int32_t bh_hal_ota_flash_fsi_image(uint8_t *buf_addr, uint32_t buf_len, const char *partition);

/**
 * @brief Switches to the target bank for booting
 *
 * @param target_bank Target bank number, 0 - bank a, 1 - bank b .
 * @return int32_t Returns 0 on success, negative value on failure.
 */
int32_t bh_hal_ota_switch_bank(int32_t target_bank);

/**
 * @brief Retrieves the currently set target bank.
 *
 * @param bank Pointer to output the target bank, 0 or 1.
 * @return int32_t Returns 0 on success, negative value on failure.
 */
int32_t bh_hal_ota_get_target_bank(int32_t *bank);

/**
 * @brief Retrieves the current running bank.
 *
 * @param bank Pointer to output the current running bank.
 * @return int32_t Returns 0 on success, negative value on failure.
 */
int32_t bh_hal_ota_get_current_bank(int32_t *bank);

/**
 * @brief Sets the specified bank as usable.
 *
 * @param bank_mask Bank mask to set
 * @return int32_t Returns 0 on success, negative value on failure.
 */
int32_t bh_hal_ota_set_usable_bank(uint32_t bank_mask);

/**
 * @brief Retrieves the current usable bank mask.
 *
 * @param usable_bank Pointer to output the usable bank mask.
 * @return int32_t Returns 0 on success, negative value on failure.
 */
int32_t bh_hal_ota_get_usable_bank(int32_t *bank_mask);

/**
 * @brief Sets the current running bank.
 *
 * @param bank Bank to set, 0 or 1.
 * @return int32_t Returns 0 on success, negative value on failure.
 */
int32_t bh_hal_ota_set_current_bank(int32_t bank);

/**
 * @brief Triggers system reset.
 *
 * @return int32_t Returns 0 on success, negative value on failure.
 */
int32_t bh_hal_ota_reset(void);

/**
 * @brief Synchronizes BL1 images between two banks
 *
 * @param direction Synchronization direction,
 * 0 - copy from bl1_a to bl1_b 
 * 1 - copy from bl1_b to bl1_a 
 * @return int32_t Returns 0 on success, negative value on failure.
 */
int32_t bh_hal_ota_sync_bl1_image(int32_t direction);

/**
 * @brief Retrieves the bank number where the current BL1 image resides.
 *
 * @param bank Pointer to output the BL1 residing bank.
 * @return int32_t Returns 0 on success, negative value on failure.
 */
int32_t bh_hal_ota_get_bl1_bank(int32_t *bank);

/**
 * @brief   Verify the integrity and validity of an OTA image file.
 *
 * This function checks the binary image located at the specified file path by
 * validating its header checksum and payload CRC to ensure the image is not
 * corrupted or malformed. It is typically used during OTA update preparation.
 *
 * @param[in] filename  Path to the image file to be verified (null-terminated string)
 *
 * @return  int32_t
 *          - 0         if verification succeeds
 *          - Non-zero  if verification fails (e.g., invalid checksum or file I/O error)
 */
int32_t bh_hal_ota_verify_image(const char *filename);

/**
 * @brief Parses a semantic version string into a comparable integer.
 *
 * The accepted format is strictly `major.minor.patch`, where each field is a
 * decimal number in the range `[0, 99]`. The internal comparable integer is
 * encoded as `major * 10000 + minor * 100 + patch`.
 *
 * @param version_str Input version string.
 * @param version_code Optional output for the comparable integer.
 * @param normalized_version Optional output for the canonical `x.y.z` string.
 * @param normalized_len Size of @p normalized_version.
 * @return int32_t Returns 0 on success, negative value on failure.
 */
#endif
