#define LOG_TAG "SPI_NOR_TEST"
#include "elog.h"
#include "shell.h"
#include "spi_nor.h"
#include "gpt.h"
#include <stdlib.h>
#include "../../../common_inc/bsp/quard_nor_agent_protocol.h"

/**
 * @brief spi_nor擦除测试：按扇区擦除
 * 格式: spi_nor_erase 0x100000 0x1000
 */
int spi_nor_erase(int argc, char *argv[]) {
	int ret;
	uint32_t addr;
	uint32_t len;

    if (argc < 3) {
        LOG_E("Usage: spi_nor_erase <addr> <len>");
        return -1;
    }
	addr = (uint32_t)strtoul(argv[1], NULL, 16);
	len = (uint32_t)strtoul(argv[2], NULL, 16);
    if (addr >= SPI_NOR_SIZE || len > SPI_NOR_SIZE - addr ||
	(addr % SPI_NOR_SECTOR_SIZE) != 0U ||
	(len % SPI_NOR_SECTOR_SIZE) != 0U) {
	LOG_E("erase range must be 4 KiB aligned");
	return -1;
    }
    
    LOG_I("spi_nor_erase: start addr 0x%08x", addr);
	ret = spi_nor_erase_sector(addr, len);
	if (ret != 0) {
		LOG_E("spi_nor_erase failed: %d", ret);
		return ret;
	}
    LOG_I("spi_nor_erase: done");

    return 0;
}

/**
 * @brief spi_nor写测试：写一个字
 * 格式: spi_nor_write 0 0x12345678
 */
int spi_nor_write(int argc, char *argv[]) {
	int ret;
	uint32_t addr;
	uint32_t data;

    if (argc < 3) {
        LOG_E("Usage: spi_nor_write <addr> <data>");
        return -1;
    }
	addr = (uint32_t)strtoul(argv[1], NULL, 16);
	data = (uint32_t)strtoul(argv[2], NULL, 16);
    if (addr >= SPI_NOR_SIZE || sizeof(data) > SPI_NOR_SIZE - addr) {
        LOG_E("Error: Address 0x%08x is out of SPI NOR range!", addr);
        return -1;
    }

    LOG_I("spi_nor_write: addr 0x%08x, data 0x%08x", addr, data);
	ret = spi_nor_write_data(addr, &data, sizeof(data));
	if (ret != 0) {
		LOG_E("spi_nor_write failed: %d", ret);
		return ret;
	}
    LOG_I("spi_nor_write: done");

    return 0;
}

/**
 * @brief spi_nor读测试：支持随机读，但是最好按4字节对齐以保证兼容性
 * 格式: spi_nor_read 0 4
 */
int spi_nor_read(int argc, char *argv[]) {
	int ret;
	uint32_t addr;
	uint32_t len;
	uint32_t data = 0;

    if (argc < 3) {
        LOG_E("Usage: spi_nor_read <addr> <len>");
        return -1;
    }
	addr = (uint32_t)strtoul(argv[1], NULL, 16);
	len = (uint32_t)strtoul(argv[2], NULL, 16);
    if (len == 0U || len > sizeof(data) || addr >= SPI_NOR_SIZE ||
	len > SPI_NOR_SIZE - addr) {
	LOG_E("read length must be between 1 and 4 bytes");
	return -1;
    }
    
	ret = spi_nor_read_data(addr, &data, len);
	if (ret != 0) {
		LOG_E("spi_nor_read failed: %d", ret);
		return ret;
	}
    LOG_I("spi_nor_read: addr 0x%08x, data 0x%08x", addr, data);

    return 0;
}

static int spi_nor_gpt_read(void *context, uint32_t offset, void *buffer,
			    uint32_t length)
{
	(void)context;
	return spi_nor_read_data(offset, buffer, length);
}

/**
 * @brief 读取gpt分区内存
 *
 */
void read_gpt_partitions(void) {
	static struct quard_nor_info info;
	uint32_t i;

	memset(&info, 0, sizeof(info));
	if (gpt_read_partitions(spi_nor_gpt_read, NULL, &info) != 0) {
		LOG_E("GPT validation failed");
		return;
	}
	LOG_I("GPT Partition scan (Total entries: %u)", info.nparts);
	for (i = 0; i < info.nparts; i++)
		LOG_I("Partition %u: [%s] | Start: 0x%08X | Size: %u KB",
		      i, info.parts[i].name, info.parts[i].offset,
		      info.parts[i].length / 1024U);
}

// 导出命令
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0)|SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN)|SHELL_CMD_DISABLE_RETURN, 
                spi_nor_erase, spi_nor_erase, erase spi_nor sector);

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0)|SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN)|SHELL_CMD_DISABLE_RETURN, 
                spi_nor_write, spi_nor_write, write byte to spi_nor);

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0)|SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN)|SHELL_CMD_DISABLE_RETURN, 
                spi_nor_read, spi_nor_read, read byte from spi_nor);

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0)|SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN)|SHELL_CMD_DISABLE_RETURN, 
                read_gpt_partitions, read_gpt_partitions, read byte from spi_nor);
