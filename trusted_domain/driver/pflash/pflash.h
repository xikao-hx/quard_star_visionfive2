#include <stdint.h>
#include "gpt.h"
#include <stdbool.h>

/* 由于 width 为 4，必须使用 uint32_t 进行操作 */ 
typedef uint32_t flash_t;

#define PFLASH_SIZE          (32 * 1024 * 1024)     /* 32MB大小 */ 
#define PFLASH_BASE          0x20000000ULL          /* 强制为 unsigned long long (64位) */ 
#define PFLASH_END_ADDR      (PFLASH_BASE + PFLASH_SIZE)
#define PFLASH_SECTOR_SIZE   (256 * 1024)           /* 对应配置的扇区大小 */ 

/* 针对 32位并联模式（x16 + x16）的 Intel 命令集 */ 
#define CMD_READ_ARRAY       0x00FF00FF
#define CMD_READ_ID          0x00900090
#define CMD_ERASE_SETUP      0x00200020
#define CMD_ERASE_CONFIRM    0x00D000D0
#define CMD_PROGRAM          0x00400040
#define CMD_READ_STATUS      0x00700070
#define CMD_CLEAR_STATUS     0x00500050

/* 状态位：第 7 位是 Ready */ 
#define STATUS_READY_BIT     0x00800080

int pflash_erase_sector(uint32_t addr, uint32_t len);
int pflash_write_data(uint32_t addr, flash_t *data, uint32_t len);
int pflash_read_data(uint32_t addr, flash_t *data, uint32_t len);
int ota_get_header(gpt_header_t *header);
int ota_get_entry(gpt_entry_t *entry, uint32_t current_entry_offset, bool *is_empty);