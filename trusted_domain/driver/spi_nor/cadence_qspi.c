/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Cadence indirect transfer support adapted from
 * u-boot/drivers/spi/cadence_qspi_apb.c at commit
 * dfc776216facec308ec16998c33b17e02542ab52.
 *
 * Copyright (C) 2012 Altera Corporation <www.altera.com>
 */
#include "cadence_qspi.h"

#include <errno.h>
#include <string.h>

/* JH7110 Cadence QSPI register block and indirect FIFO aperture. */
#define CQSPI_REG_BASE                 0x13010000UL
#define CQSPI_AHB_BASE                 0x21000000UL
#define CQSPI_REG_CONFIG               0x00U
#define CQSPI_REG_RD_INSTR             0x04U
#define CQSPI_REG_WR_INSTR             0x08U
#define CQSPI_REG_SIZE                 0x14U
#define CQSPI_REG_SRAMPARTITION        0x18U
#define CQSPI_REG_INDIRECTTRIGGER      0x1cU
#define CQSPI_REG_REMAP                0x24U
#define CQSPI_REG_SRAMLEVEL            0x2cU
#define CQSPI_REG_IRQSTATUS            0x40U
#define CQSPI_REG_IRQMASK              0x44U
#define CQSPI_REG_INDIRECTRD           0x60U
#define CQSPI_REG_INDIRECTRDSTARTADDR  0x68U
#define CQSPI_REG_INDIRECTRDBYTES      0x6cU
#define CQSPI_REG_INDIRECTWR           0x70U
#define CQSPI_REG_INDIRECTWRSTARTADDR  0x78U
#define CQSPI_REG_INDIRECTWRBYTES      0x7cU
#define CQSPI_REG_CMDCTRL              0x90U
#define CQSPI_REG_CMDADDRESS           0x94U
#define CQSPI_REG_CMDREADDATALOWER     0xa0U
#define CQSPI_REG_CMDREADDATAUPPER     0xa4U
#define CQSPI_REG_CMDWRITEDATALOWER    0xa8U
#define CQSPI_REG_CMDWRITEDATAUPPER    0xacU

#define CQSPI_CONFIG_ENABLE            (1U << 0)
#define CQSPI_CONFIG_CLK_POL           (1U << 1)
#define CQSPI_CONFIG_CLK_PHA           (1U << 2)
#define CQSPI_CONFIG_DECODE            (1U << 9)
#define CQSPI_CONFIG_CS_LSB             10U
#define CQSPI_CONFIG_CS_MASK            0xfU
#define CQSPI_CONFIG_IDLE               (1U << 31)
#define CQSPI_CONFIG_DTR_PROTO          (1U << 24)
#define CQSPI_CONFIG_DUAL_OPCODE        (1U << 30)

#define CQSPI_INSTR_OPCODE_LSB           0U

#define CQSPI_SIZE_ADDRESS_LSB           0U
#define CQSPI_SIZE_PAGE_LSB              4U
#define CQSPI_SIZE_BLOCK_LSB            16U
#define CQSPI_SIZE_ADDRESS_MASK          0xfU
#define CQSPI_SIZE_PAGE_MASK             0xfffU
#define CQSPI_SIZE_BLOCK_MASK            0x3fU

#define CQSPI_SRAMLEVEL_RD_LSB            0U
#define CQSPI_SRAMLEVEL_WR_LSB           16U
#define CQSPI_SRAMLEVEL_MASK             0xffffU

#define CQSPI_INDIRECT_START             (1U << 0)
#define CQSPI_INDIRECT_CANCEL            (1U << 1)
#define CQSPI_INDIRECT_INPROGRESS        (1U << 2)
#define CQSPI_INDIRECT_DONE              (1U << 5)

#define CQSPI_CMD_EXECUTE               (1U << 0)
#define CQSPI_CMD_INPROGRESS            (1U << 1)
#define CQSPI_CMD_WR_BYTES_LSB          12U
#define CQSPI_CMD_WR_ENABLE             (1U << 15)
#define CQSPI_CMD_ADDR_BYTES_LSB        16U
#define CQSPI_CMD_ADDR_ENABLE           (1U << 19)
#define CQSPI_CMD_RD_BYTES_LSB          20U
#define CQSPI_CMD_RD_ENABLE             (1U << 23)
#define CQSPI_CMD_OPCODE_LSB            24U

#define CQSPI_STIG_DATA_MAX             8U
#define CQSPI_COMMAND_RETRIES            1000000U
#define CQSPI_INDIRECT_RETRIES           1000000U
#define CQSPI_IDLE_SAMPLES               3U
#define CQSPI_FIFO_DEPTH                 256U
#define CQSPI_FIFO_WIDTH                 4U
#define CQSPI_SRAM_PARTITION             (CQSPI_FIFO_DEPTH / 2U)

static uint32_t qspi_address_bytes;
static uint32_t qspi_page_size;

static inline uint32_t qspi_readl(uint32_t offset)
{
	return *(volatile uint32_t *)(CQSPI_REG_BASE + offset);
}

static inline void qspi_writel(uint32_t value, uint32_t offset)
{
	*(volatile uint32_t *)(CQSPI_REG_BASE + offset) = value;
	__asm volatile("fence iorw, iorw" ::: "memory");
}

static inline uint32_t qspi_fifo_readl(void)
{
	return *(volatile uint32_t *)CQSPI_AHB_BASE;
}

static inline uint8_t qspi_fifo_readb(void)
{
	return *(volatile uint8_t *)CQSPI_AHB_BASE;
}

static inline void qspi_fifo_writel(uint32_t value)
{
	*(volatile uint32_t *)CQSPI_AHB_BASE = value;
}

static inline void qspi_fifo_writeb(uint8_t value)
{
	*(volatile uint8_t *)CQSPI_AHB_BASE = value;
}

static inline void qspi_io_fence(void)
{
	__asm volatile("fence iorw, iorw" ::: "memory");
}

static int qspi_wait_idle(void)
{
	uint32_t retry = CQSPI_COMMAND_RETRIES;
	uint32_t idle_samples = 0;

	while (retry-- != 0U) {
		if ((qspi_readl(CQSPI_REG_CONFIG) & CQSPI_CONFIG_IDLE) != 0U) {
			if (++idle_samples == CQSPI_IDLE_SAMPLES)
				return 0;
		} else {
			idle_samples = 0;
		}
	}

	return -ETIMEDOUT;
}

static int qspi_wait_indirect_done(uint32_t reg)
{
	uint32_t retry = CQSPI_INDIRECT_RETRIES;

	while (retry-- != 0U) {
		if ((qspi_readl(reg) & CQSPI_INDIRECT_DONE) != 0U)
			return 0;
	}

	return -ETIMEDOUT;
}

static void qspi_cancel_indirect(uint32_t reg)
{
	uint32_t retry = CQSPI_INDIRECT_RETRIES;

	qspi_writel(CQSPI_INDIRECT_CANCEL, reg);
	while (retry-- != 0U) {
		if ((qspi_readl(reg) & CQSPI_INDIRECT_INPROGRESS) == 0U)
			break;
	}
	if ((qspi_readl(reg) & CQSPI_INDIRECT_DONE) != 0U)
		qspi_writel(CQSPI_INDIRECT_DONE, reg);
	(void)qspi_wait_idle();
}

static uint32_t qspi_read_sram_level(void)
{
	return (qspi_readl(CQSPI_REG_SRAMLEVEL) >>
		CQSPI_SRAMLEVEL_RD_LSB) & CQSPI_SRAMLEVEL_MASK;
}

static uint32_t qspi_write_sram_level(void)
{
	return (qspi_readl(CQSPI_REG_SRAMLEVEL) >>
		CQSPI_SRAMLEVEL_WR_LSB) & CQSPI_SRAMLEVEL_MASK;
}

static int qspi_wait_read_data(uint32_t *entries)
{
	uint32_t retry = CQSPI_INDIRECT_RETRIES;

	while (retry-- != 0U) {
		*entries = qspi_read_sram_level();
		if (*entries != 0U)
			return 0;
	}

	return -ETIMEDOUT;
}

static int qspi_wait_write_fifo_empty(void)
{
	uint32_t retry = CQSPI_INDIRECT_RETRIES;

	while (retry-- != 0U) {
		if (qspi_write_sram_level() == 0U)
			return 0;
	}

	return -ETIMEDOUT;
}

static void qspi_fifo_read(void *buffer, uint32_t len)
{
	uint8_t *dst = buffer;
	uint32_t value;

	while (len >= sizeof(value)) {
		value = qspi_fifo_readl();
		if (((uintptr_t)dst & (sizeof(value) - 1U)) == 0U)
			*(uint32_t *)(void *)dst = value;
		else
			memcpy(dst, &value, sizeof(value));
		dst += sizeof(value);
		len -= sizeof(value);
	}
	while (len-- != 0U)
		*dst++ = qspi_fifo_readb();
	qspi_io_fence();
}

static void qspi_fifo_write(const void *buffer, uint32_t len)
{
	const uint8_t *src = buffer;
	uint32_t value;

	while (len >= sizeof(value)) {
		if (((uintptr_t)src & (sizeof(value) - 1U)) == 0U)
			value = *(const uint32_t *)(const void *)src;
		else
			memcpy(&value, src, sizeof(value));
		qspi_fifo_writel(value);
		src += sizeof(value);
		len -= sizeof(value);
	}
	while (len-- != 0U)
		qspi_fifo_writeb(*src++);
	qspi_io_fence();
}

static int qspi_indirect_read_setup(uint8_t opcode, uint32_t addr)
{
	int ret = qspi_wait_idle();

	if (ret != 0)
		return ret;
	qspi_writel(addr, CQSPI_REG_INDIRECTRDSTARTADDR);
	qspi_writel((uint32_t)opcode << CQSPI_INSTR_OPCODE_LSB,
		     CQSPI_REG_RD_INSTR);
	return 0;
}

static int qspi_indirect_read_execute(void *buffer, uint32_t len)
{
	uint8_t *dst = buffer;
	uint32_t remaining = len;
	uint32_t entries;
	uint32_t bytes;
	int ret;

	qspi_writel(CQSPI_INDIRECT_DONE, CQSPI_REG_INDIRECTRD);
	qspi_writel(len, CQSPI_REG_INDIRECTRDBYTES);
	qspi_writel(CQSPI_INDIRECT_START, CQSPI_REG_INDIRECTRD);
	while (remaining != 0U) {
		ret = qspi_wait_read_data(&entries);
		if (ret != 0)
			goto fail;
		bytes = entries * CQSPI_FIFO_WIDTH;
		if (bytes > remaining)
			bytes = remaining;
		qspi_fifo_read(dst, bytes);
		dst += bytes;
		remaining -= bytes;
	}

	ret = qspi_wait_indirect_done(CQSPI_REG_INDIRECTRD);
	if (ret != 0)
		goto fail;
	qspi_writel(CQSPI_INDIRECT_DONE, CQSPI_REG_INDIRECTRD);
	ret = qspi_wait_idle();
	if (ret == 0)
		return 0;

fail:
	qspi_cancel_indirect(CQSPI_REG_INDIRECTRD);
	return -ETIMEDOUT;
}

static int qspi_indirect_write_setup(uint8_t opcode, uint32_t addr)
{
	int ret = qspi_wait_idle();

	if (ret != 0)
		return ret;
	qspi_writel((uint32_t)opcode << CQSPI_INSTR_OPCODE_LSB,
		     CQSPI_REG_WR_INSTR);
	qspi_writel(addr, CQSPI_REG_INDIRECTWRSTARTADDR);
	return 0;
}

static int qspi_indirect_write_execute(const void *buffer, uint32_t len)
{
	int ret;

	qspi_writel(CQSPI_INDIRECT_DONE, CQSPI_REG_INDIRECTWR);
	qspi_writel(len, CQSPI_REG_INDIRECTWRBYTES);
	qspi_writel(CQSPI_INDIRECT_START, CQSPI_REG_INDIRECTWR);
	/* Synchronize the APB START write before touching the AHB FIFO. */
	(void)qspi_readl(CQSPI_REG_INDIRECTWR);
	qspi_fifo_write(buffer, len);
	ret = qspi_wait_write_fifo_empty();
	if (ret != 0)
		goto fail;
	ret = qspi_wait_indirect_done(CQSPI_REG_INDIRECTWR);
	if (ret != 0)
		goto fail;
	qspi_writel(CQSPI_INDIRECT_DONE, CQSPI_REG_INDIRECTWR);
	ret = qspi_wait_idle();
	if (ret == 0)
		return 0;

fail:
	qspi_cancel_indirect(CQSPI_REG_INDIRECTWR);
	return -ETIMEDOUT;
}

int cadence_qspi_init(uint32_t address_bytes, uint32_t page_size,
		      uint32_t block_size_log2)
{
	uint32_t config;
	uint32_t size;

	if (address_bytes == 0U || address_bytes > 4U || page_size == 0U ||
	    page_size > CQSPI_SIZE_PAGE_MASK ||
	    block_size_log2 > CQSPI_SIZE_BLOCK_MASK)
		return -EINVAL;

	/* U-Boot enabled the JH7110 QSPI clocks and reset lines. */
	config = qspi_readl(CQSPI_REG_CONFIG);
	config &= ~CQSPI_CONFIG_ENABLE;
	qspi_writel(config, CQSPI_REG_CONFIG);
	config &= ~(CQSPI_CONFIG_CLK_POL | CQSPI_CONFIG_CLK_PHA |
		    CQSPI_CONFIG_DTR_PROTO | CQSPI_CONFIG_DUAL_OPCODE);
	config &= ~CQSPI_CONFIG_DECODE;
	config &= ~(CQSPI_CONFIG_CS_MASK << CQSPI_CONFIG_CS_LSB);
	config |= 0xeU << CQSPI_CONFIG_CS_LSB;
	qspi_writel(config, CQSPI_REG_CONFIG);

	size = qspi_readl(CQSPI_REG_SIZE);
	size &= ~((CQSPI_SIZE_ADDRESS_MASK << CQSPI_SIZE_ADDRESS_LSB) |
		  (CQSPI_SIZE_PAGE_MASK << CQSPI_SIZE_PAGE_LSB) |
		  (CQSPI_SIZE_BLOCK_MASK << CQSPI_SIZE_BLOCK_LSB));
	size |= (address_bytes - 1U) << CQSPI_SIZE_ADDRESS_LSB;
	size |= page_size << CQSPI_SIZE_PAGE_LSB;
	size |= block_size_log2 << CQSPI_SIZE_BLOCK_LSB;
	qspi_writel(size, CQSPI_REG_SIZE);
	qspi_writel(0, CQSPI_REG_REMAP);
	qspi_writel(CQSPI_SRAM_PARTITION, CQSPI_REG_SRAMPARTITION);
	qspi_writel(0, CQSPI_REG_INDIRECTTRIGGER);
	qspi_writel(0, CQSPI_REG_IRQMASK);
	qspi_writel(UINT32_MAX, CQSPI_REG_IRQSTATUS);
	qspi_writel(0, CQSPI_REG_RD_INSTR);
	qspi_writel(0, CQSPI_REG_WR_INSTR);
	qspi_writel(config | CQSPI_CONFIG_ENABLE, CQSPI_REG_CONFIG);

	qspi_address_bytes = address_bytes;
	qspi_page_size = page_size;
	return qspi_wait_idle();
}

int cadence_qspi_command(uint8_t opcode, bool has_addr, uint32_t addr,
			 const void *tx_buf, uint32_t tx_len,
			 void *rx_buf, uint32_t rx_len)
{
	uint32_t cmd = (uint32_t)opcode << CQSPI_CMD_OPCODE_LSB;
	uint32_t value = 0;
	uint32_t retry = CQSPI_COMMAND_RETRIES;
	bool complete = false;

	if (qspi_address_bytes == 0U ||
	    tx_len > CQSPI_STIG_DATA_MAX || rx_len > CQSPI_STIG_DATA_MAX ||
	    (tx_len != 0U && tx_buf == NULL) ||
	    (rx_len != 0U && rx_buf == NULL) ||
	    (tx_len != 0U && rx_len != 0U))
		return -EINVAL;

	/* STIG needs only the single-SPI protocol fields in RD_INSTR. */
	qspi_writel(0, CQSPI_REG_RD_INSTR);
	if (has_addr) {
		qspi_writel(addr, CQSPI_REG_CMDADDRESS);
		cmd |= CQSPI_CMD_ADDR_ENABLE;
		cmd |= (qspi_address_bytes - 1U) << CQSPI_CMD_ADDR_BYTES_LSB;
	}

	if (tx_len != 0U) {
		memcpy(&value, tx_buf, tx_len > sizeof(value) ?
		       sizeof(value) : tx_len);
		qspi_writel(value, CQSPI_REG_CMDWRITEDATALOWER);
		if (tx_len > sizeof(value)) {
			value = 0;
			memcpy(&value, (const uint8_t *)tx_buf + sizeof(value),
			       tx_len - sizeof(value));
			qspi_writel(value, CQSPI_REG_CMDWRITEDATAUPPER);
		}
		cmd |= CQSPI_CMD_WR_ENABLE;
		cmd |= (tx_len - 1U) << CQSPI_CMD_WR_BYTES_LSB;
	}
	if (rx_len != 0U) {
		cmd |= CQSPI_CMD_RD_ENABLE;
		cmd |= (rx_len - 1U) << CQSPI_CMD_RD_BYTES_LSB;
	}

	qspi_writel(cmd, CQSPI_REG_CMDCTRL);
	qspi_writel(cmd | CQSPI_CMD_EXECUTE, CQSPI_REG_CMDCTRL);
	while (retry-- != 0U) {
		if ((qspi_readl(CQSPI_REG_CMDCTRL) &
		     CQSPI_CMD_INPROGRESS) == 0U) {
			complete = true;
			break;
		}
	}
	if (!complete || qspi_wait_idle() != 0)
		return -ETIMEDOUT;

	if (rx_len != 0U) {
		value = qspi_readl(CQSPI_REG_CMDREADDATALOWER);
		memcpy(rx_buf, &value, rx_len > sizeof(value) ?
		       sizeof(value) : rx_len);
		if (rx_len > sizeof(value)) {
			value = qspi_readl(CQSPI_REG_CMDREADDATAUPPER);
			memcpy((uint8_t *)rx_buf + sizeof(value), &value,
			       rx_len - sizeof(value));
		}
	}
	qspi_writel(0, CQSPI_REG_CMDCTRL);
	return 0;
}

int cadence_qspi_indirect_read(uint8_t opcode, uint32_t addr,
			       void *buffer, uint32_t len)
{
	int ret;

	if (buffer == NULL || len == 0U ||
	    len > CADENCE_QSPI_INDIRECT_READ_MAX)
		return -EINVAL;
	ret = qspi_indirect_read_setup(opcode, addr);
	if (ret != 0)
		return ret;
	return qspi_indirect_read_execute(buffer, len);
}

int cadence_qspi_indirect_write(uint8_t opcode, uint32_t addr,
				const void *buffer, uint32_t len)
{
	int ret;

	if (qspi_page_size == 0U || buffer == NULL || len == 0U ||
	    len > qspi_page_size ||
	    len > qspi_page_size - (addr % qspi_page_size))
		return -EINVAL;
	ret = qspi_indirect_write_setup(opcode, addr);
	if (ret != 0)
		return ret;
	return qspi_indirect_write_execute(buffer, len);
}
