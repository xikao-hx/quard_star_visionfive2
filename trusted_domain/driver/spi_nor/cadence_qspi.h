#ifndef TRUSTED_DOMAIN_CADENCE_QSPI_H
#define TRUSTED_DOMAIN_CADENCE_QSPI_H

#include <stdbool.h>
#include <stdint.h>

#define CADENCE_QSPI_INDIRECT_READ_MAX 4096U

int cadence_qspi_init(uint32_t address_bytes, uint32_t page_size,
		      uint32_t block_size_log2);
int cadence_qspi_command(uint8_t opcode, bool has_addr, uint32_t addr,
			 const void *tx_buf, uint32_t tx_len,
			 void *rx_buf, uint32_t rx_len);
int cadence_qspi_indirect_read(uint8_t opcode, uint32_t addr,
			       void *buffer, uint32_t len);
int cadence_qspi_indirect_write(uint8_t opcode, uint32_t addr,
				const void *buffer, uint32_t len);

#endif
