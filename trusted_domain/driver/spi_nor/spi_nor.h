#ifndef TRUSTED_DOMAIN_SPI_NOR_H
#define TRUSTED_DOMAIN_SPI_NOR_H

#include <stdbool.h>
#include <stdint.h>

#include "../../../common_inc/bsp/quard_nor_layout.h"

/* VisionFive 2 onboard GD25LQ128 SPI NOR. */
#define SPI_NOR_SIZE          QUARD_NOR_FLASH_SIZE
#define SPI_NOR_SECTOR_SIZE   QUARD_NOR_ERASE_SIZE
#define SPI_NOR_PAGE_SIZE     QUARD_NOR_PAGE_SIZE
#define SPI_NOR_ADDR_BYTES    3U

#define SPI_NOR_END_ADDR      SPI_NOR_SIZE

int spi_nor_init(void);
int spi_nor_get_id(uint32_t *id);
int spi_nor_erase_sector(uint32_t addr, uint32_t len);
int spi_nor_write_data(uint32_t addr, const void *data, uint32_t len);
int spi_nor_read_data(uint32_t addr, void *data, uint32_t len);
#endif
