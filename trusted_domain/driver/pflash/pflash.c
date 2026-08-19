#include <pflash.h>
#include <stdbool.h>
#include "gpt.h"

static inline void flash_write(uint32_t offset, flash_t data) {

    /* 先计算出 64 位的地址，再转为指针 */
    uintptr_t addr = (uintptr_t)(PFLASH_BASE + offset);    
    *(volatile flash_t *)addr = data;
}

static inline flash_t flash_read(uint32_t offset) {

    uintptr_t addr = (uintptr_t)(PFLASH_BASE + offset);
    return *(volatile flash_t *)addr;
}

/**
 * @brief 擦除函数：擦除一个扇区
 * @param addr 地址
 * @param len 长度: 单位为字节
 * @return int 0 成功，-1 失败
 */
int pflash_erase_sector(uint32_t addr, uint32_t len) {

    for (uint32_t curr_addr = addr; curr_addr < addr + len; curr_addr += PFLASH_SECTOR_SIZE) {
        flash_write(curr_addr, CMD_ERASE_SETUP);
        flash_write(curr_addr, CMD_ERASE_CONFIRM);

        /* 等待状态寄存器第 7 位（Ready）变为 1 */ 
        while (!(flash_read(curr_addr) & 0x80));

        flash_write(curr_addr, CMD_READ_ARRAY);
    }

    return 0;
}

/**
 * @brief 写入函数：支持连续写入多个字节
 * @param addr 地址
 * @param len 长度: 单位为字节
 * @return int 0 成功，-1 失败
 */
int pflash_write_data(uint32_t addr, flash_t *data, uint32_t len) {
    const uint8_t *src = (const uint8_t *)data;
    uint32_t words = len / sizeof(flash_t);
    uint32_t rem = len % sizeof(flash_t);

    for (uint32_t i = 0; i < words; i++) {
        flash_write(addr + (i * sizeof(flash_t)), CMD_PROGRAM);
        flash_write(addr + (i * sizeof(flash_t)), data[i]);

        while (!(flash_read(addr + (i * sizeof(flash_t))) & 0x80));
        flash_write(addr + (i * sizeof(flash_t)), CMD_READ_ARRAY);
    }

    if (rem > 0U) {
        flash_t last_word = 0xFFFFFFFFU;
        uint32_t tail_addr = addr + (words * sizeof(flash_t));

        memcpy(&last_word, src + (words * sizeof(flash_t)), rem);
        flash_write(tail_addr, CMD_PROGRAM);
        flash_write(tail_addr, last_word);

        while (!(flash_read(tail_addr) & 0x80));
        flash_write(tail_addr, CMD_READ_ARRAY);
    }
    
    return 0;
}

/**
 * @brief 读取函数：支持连续读取多个字节
 * @param addr 地址
 * @param len 长度: 单位为字节
 * @return int 0 成功，-1 失败
 */
int pflash_read_data(uint32_t addr, flash_t *data, uint32_t len) {

    /* 强制先切回读取模式，防止 Flash 还停留在之前的命令状态 */ 
    flash_write(0, CMD_READ_ARRAY); 

    uint32_t words = len / sizeof(flash_t);
    uint32_t rem = len % sizeof(flash_t);
    uint8_t *dst = (uint8_t *)data;
    
    for (uint32_t i = 0; i < words; i++) {
        data[i] = flash_read(addr + (i * sizeof(flash_t)));
    }

    if (rem > 0U) {
        flash_t last_word = flash_read(addr + (words * sizeof(flash_t)));

        memcpy(dst + (words * sizeof(flash_t)), &last_word, rem);
    }
    
    return 0;
}

/**
 * @brief 读取GPT头
 * 
 */
int ota_get_header(gpt_header_t *header) {

    uint32_t buf[sizeof(gpt_header_t) / 4 + 1];
    
    if (pflash_read_data(512, buf, sizeof(gpt_header_t)) != 0) {
        return -1;
    }
    memcpy(header, buf, sizeof(gpt_header_t));

    return 0;
}

/**
 * @brief 读取entry 分区条目
 *
 */
int ota_get_entry(gpt_entry_t *entry, uint32_t current_entry_offset, bool *is_empty) {
    uint32_t entry_buf[sizeof(gpt_entry_t) / 4 + 1];

    if (pflash_read_data(current_entry_offset, entry_buf, sizeof(gpt_entry_t)) != 0) {
        return -1;
    }
    memcpy(entry, entry_buf, sizeof(gpt_entry_t));

    /* 检查 type_uuid 是否全 0 (表示空条目) */ 
    uint8_t *guid_ptr = (uint8_t *)&entry->type_uuid;
    for (int g = 0; g < 16; g++) {
        if (guid_ptr[g] != 0) {
            *is_empty = false;
            break;
        }
    }

    return 0;
}
