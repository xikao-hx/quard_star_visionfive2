#define LOG_TAG "PFLASH_TEST"
#include "elog.h"
#include "shell.h"
#include "pflash.h"
#include "gpt.h"
#include <stdlib.h>
#include "../../../common_inc/bsp/quard_nor_agent_protocol.h"
#include <stdbool.h>

/**
 * @brief pflash擦除测试：按扇区擦除
 * 格式: pflash_erase 0x100000 1
 */
int pflash_erase(int argc, char *argv[]) {

    // 将字符串转换为 16 进制数值
    uint32_t addr = (uint32_t)strtoul(argv[1], NULL, 16);
    uint32_t len = (uint32_t)strtoul(argv[2], NULL, 16);
    if (addr < 0 || addr >= PFLASH_SIZE) {
        LOG_E("Error: Address 0x%08x is out of PFlash range!", addr);
        return -1;
    }

    if (argc < 3) {
        LOG_E("Usage: pflash_erase <addr> <len>");
        return -1;
    }
    
    LOG_I("pflash_erase: start addr 0x%08x", addr);
    pflash_erase_sector(addr, len);
    LOG_I("pflash_erase: done");

    return 0;
}

/**
 * @brief pflash写测试：写一个字
 * 格式: pflash_write 0 0x12345678
 */
int pflash_write(int argc, char *argv[]) {

    uint32_t addr = (uint32_t)strtoul(argv[1], NULL, 16);
    uint32_t data = (uint32_t)strtoul(argv[2], NULL, 16);

    if (addr < 0 || addr >= PFLASH_SIZE) {
        LOG_E("Error: Address 0x%08x is out of PFlash range!", addr);
        return -1;
    }

    if (argc < 3) {
        LOG_E("Usage: pflash_write <addr> <data>");
        return -1;
    }

    LOG_I("pflash_write: addr 0x%08x, data 0x%08x", addr, data);
    pflash_write_data(addr, &data, sizeof(data));
    LOG_I("pflash_write: done");

    return 0;
}

/**
 * @brief pflash读测试：支持随机读，但是最好按4字节对齐以保证兼容性
 * 格式: pflash_read 0 4
 */
int pflash_read(int argc, char *argv[]) {

    uint32_t addr = (uint32_t)strtoul(argv[1], NULL, 16);
    uint32_t len = (uint32_t)strtoul(argv[2], NULL, 16);
    flash_t data = 0;

    if (addr < 0 || addr >= PFLASH_SIZE) {
        LOG_E("Error: Address 0x%08x is out of PFlash range!", addr);
        return -1;
    }

    if (argc < 3) {
        LOG_E("Usage: pflash_read <addr> <len>");
        return -1;
    }
    
    pflash_read_data(addr, &data, len);
    LOG_I("pflash_read: addr 0x%08x, data 0x%08x", addr, data);

    return 0;
}

#define SECTOR_SIZE 512

/**
 * @brief 读取gpt分区内存
 *
 */
void read_gpt_partitions(void) {
    gpt_header_t header;

    // 1. 读取 GPT 头
    if (ota_get_header(&header) != 0) {
        LOG_E("ota_get_header failed!");
        return;
    }

    // 校验签名 "EFI PART"
    if (memcmp(header.signature, GPT_SIGNATURE, 8) != 0) {
        LOG_E("Invalid GPT Signature");
        return;
    }

    // 2. 遍历分区条目
    uint32_t entry_start_offset = (uint32_t)(header.part_lba * 512); 
    uint32_t max_entries = (header.list_num > 128) ? 128 : header.list_num;

    LOG_I("Starting GPT Partition scan (Total entries: %d)...", header.list_num);

    gpt_entry_t entry;
    for (uint32_t i = 0; i < max_entries; i++) {
        uint32_t current_entry_offset = entry_start_offset + (i * header.part_size);
        bool is_empty = true;
        
        if (ota_get_entry(&entry, current_entry_offset, &is_empty) != 0) {
            LOG_W("ota_get_entry %d failed!", i);
            continue;
        }
        
        if (is_empty) {
            break;
        }

        /* 计算分区信息 */ 
        uint32_t start_addr = (uint32_t)(entry.first_lba * 512);
        uint64_t sector_count = entry.last_lba - entry.first_lba + 1;
        uint32_t size_kb = (uint32_t)(sector_count * 512 / 1024);

        char name_ansi[EFI_NAMELEN];
        unicode_to_ascii(entry.name, (uint8_t *)name_ansi);

        LOG_I("Partition %d: [%s] | Start: 0x%08X | Size: %u KB", 
              i, name_ansi, start_addr, size_kb);
    }

    LOG_I("GPT Partition scan complete.");
}

// 导出命令
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0)|SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN)|SHELL_CMD_DISABLE_RETURN, 
                pflash_erase, pflash_erase, erase pflash sector);

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0)|SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN)|SHELL_CMD_DISABLE_RETURN, 
                pflash_write, pflash_write, write byte to pflash);

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0)|SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN)|SHELL_CMD_DISABLE_RETURN, 
                pflash_read, pflash_read, read byte from pflash);

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0)|SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN)|SHELL_CMD_DISABLE_RETURN, 
                read_gpt_partitions, read_gpt_partitions, read byte from pflash);