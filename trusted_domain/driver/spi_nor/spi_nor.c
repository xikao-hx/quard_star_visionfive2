#include "spi_nor.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

/* JH7110 Cadence QSPI register block. */
#define CQSPI_REG_BASE                 0x13010000UL
#define CQSPI_REG_CONFIG               0x00U
#define CQSPI_REG_RD_INSTR             0x04U
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

#define CQSPI_CMD_EXECUTE               (1U << 0)
#define CQSPI_CMD_INPROGRESS            (1U << 1)
#define CQSPI_CMD_WR_BYTES_LSB          12U
#define CQSPI_CMD_WR_ENABLE             (1U << 15)
#define CQSPI_CMD_ADDR_BYTES_LSB        16U
#define CQSPI_CMD_ADDR_ENABLE           (1U << 19)
#define CQSPI_CMD_RD_BYTES_LSB          20U
#define CQSPI_CMD_RD_ENABLE             (1U << 23)
#define CQSPI_CMD_OPCODE_LSB            24U

#define SPI_NOR_CMD_READ                0x03U
#define SPI_NOR_CMD_PAGE_PROGRAM        0x02U
#define SPI_NOR_CMD_WRITE_ENABLE        0x06U
#define SPI_NOR_CMD_READ_STATUS         0x05U
#define SPI_NOR_CMD_READ_ID             0x9fU
#define SPI_NOR_CMD_ERASE_4K            0x20U
#define SPI_NOR_STATUS_WIP              (1U << 0)
#define SPI_NOR_STATUS_WEL              (1U << 1)

#define CQSPI_STIG_DATA_MAX             8U
#define CQSPI_COMMAND_RETRIES            1000000U
#define CQSPI_IDLE_SAMPLES               3U
#define SPI_NOR_PROGRAM_TIMEOUT_MS       2000U
#define SPI_NOR_ERASE_TIMEOUT_MS         10000U

static StaticSemaphore_t spi_nor_mutex_storage;
static SemaphoreHandle_t spi_nor_mutex;
static bool spi_nor_ready;
static uint32_t spi_nor_jedec_id;

static inline uint32_t qspi_readl(uint32_t offset)
{
	return *(volatile uint32_t *)(CQSPI_REG_BASE + offset);
}

static inline void qspi_writel(uint32_t value, uint32_t offset)
{
	*(volatile uint32_t *)(CQSPI_REG_BASE + offset) = value;
	__asm volatile("fence iorw, iorw" ::: "memory");
}

static int spi_nor_lock(void)
{
	if (spi_nor_mutex == NULL) {
		taskENTER_CRITICAL();
		if (spi_nor_mutex == NULL)
			spi_nor_mutex = xSemaphoreCreateMutexStatic(
				&spi_nor_mutex_storage);
		taskEXIT_CRITICAL();
	}

	if (spi_nor_mutex == NULL ||
	    xSemaphoreTake(spi_nor_mutex, portMAX_DELAY) != pdTRUE)
		return -EBUSY;

	return 0;
}

static void spi_nor_unlock(void)
{
	xSemaphoreGive(spi_nor_mutex);
}

static bool spi_nor_range_valid(uint32_t addr, uint32_t len)
{
	return len == 0U || (addr < SPI_NOR_SIZE && len <= SPI_NOR_SIZE - addr);
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

static int qspi_exec(uint8_t opcode, bool has_addr, uint32_t addr,
		     const void *tx_buf, uint32_t tx_len,
		     void *rx_buf, uint32_t rx_len)
{
	uint32_t cmd = (uint32_t)opcode << CQSPI_CMD_OPCODE_LSB;
	uint32_t value = 0;
	uint32_t retry = CQSPI_COMMAND_RETRIES;
	bool complete = false;

	if (tx_len > CQSPI_STIG_DATA_MAX || rx_len > CQSPI_STIG_DATA_MAX ||
	    (tx_len != 0U && tx_buf == NULL) ||
	    (rx_len != 0U && rx_buf == NULL) ||
	    (tx_len != 0U && rx_len != 0U))
		return -EINVAL;

	if (has_addr) {
		qspi_writel(addr, CQSPI_REG_CMDADDRESS);
		cmd |= CQSPI_CMD_ADDR_ENABLE;
		cmd |= (SPI_NOR_ADDR_BYTES - 1U) << CQSPI_CMD_ADDR_BYTES_LSB;
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
	if (!complete)
		return -ETIMEDOUT;
	if (qspi_wait_idle() != 0)
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

static int spi_nor_read_status(uint8_t *status)
{
	return qspi_exec(SPI_NOR_CMD_READ_STATUS, false, 0, NULL, 0,
			 status, sizeof(*status));
}

static int spi_nor_write_enable(void)
{
	uint8_t status;
	int ret;

	ret = qspi_exec(SPI_NOR_CMD_WRITE_ENABLE, false, 0, NULL, 0,
			NULL, 0);
	if (ret != 0)
		return ret;
	ret = spi_nor_read_status(&status);
	if (ret != 0)
		return ret;

	return (status & SPI_NOR_STATUS_WEL) != 0U ? 0 : -EIO;
}

static int spi_nor_wait_ready(uint32_t timeout_ms)
{
	TickType_t start = xTaskGetTickCount();
	TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
	uint8_t status;
	int ret;

	do {
		ret = spi_nor_read_status(&status);
		if (ret != 0)
			return ret;
		if ((status & SPI_NOR_STATUS_WIP) == 0U)
			return 0;
		vTaskDelay(pdMS_TO_TICKS(1));
	} while ((xTaskGetTickCount() - start) < timeout);

	return -ETIMEDOUT;
}

static int spi_nor_hw_init(void)
{
	uint32_t config;
	uint8_t id[3];
	int ret;

	if (spi_nor_ready)
		return 0;

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
	/* Instruction, address and data all use single-SPI SDR transfers. */
	qspi_writel(0, CQSPI_REG_RD_INSTR);
	qspi_writel(config | CQSPI_CONFIG_ENABLE, CQSPI_REG_CONFIG);

	ret = qspi_wait_idle();
	if (ret != 0)
		return ret;
	ret = qspi_exec(SPI_NOR_CMD_READ_ID, false, 0, NULL, 0,
			id, sizeof(id));
	if (ret != 0)
		return ret;
	if ((id[0] == 0U && id[1] == 0U && id[2] == 0U) ||
	    (id[0] == 0xffU && id[1] == 0xffU && id[2] == 0xffU))
		return -ENODEV;

	spi_nor_jedec_id = ((uint32_t)id[0] << 16) |
			   ((uint32_t)id[1] << 8) | id[2];
	spi_nor_ready = true;
	return 0;
}

int spi_nor_init(void)
{
	int ret = spi_nor_lock();

	if (ret == 0) {
		ret = spi_nor_hw_init();
		spi_nor_unlock();
	}
	return ret;
}

int spi_nor_get_id(uint32_t *id)
{
	int ret;

	if (id == NULL)
		return -EINVAL;
	ret = spi_nor_lock();
	if (ret == 0) {
		ret = spi_nor_hw_init();
		if (ret == 0)
			*id = spi_nor_jedec_id;
		spi_nor_unlock();
	}
	return ret;
}

int spi_nor_erase_sector(uint32_t addr, uint32_t len)
{
	uint32_t current;
	int ret;

	if (len == 0U || !spi_nor_range_valid(addr, len) ||
	    (addr % SPI_NOR_SECTOR_SIZE) != 0U ||
	    (len % SPI_NOR_SECTOR_SIZE) != 0U)
		return -EINVAL;

	ret = spi_nor_lock();
	if (ret != 0)
		return ret;
	ret = spi_nor_hw_init();
	for (current = addr; ret == 0 && current < addr + len;
	     current += SPI_NOR_SECTOR_SIZE) {
		ret = spi_nor_write_enable();
		if (ret == 0)
			ret = qspi_exec(SPI_NOR_CMD_ERASE_4K, true, current,
					NULL, 0, NULL, 0);
		if (ret == 0)
			ret = spi_nor_wait_ready(SPI_NOR_ERASE_TIMEOUT_MS);
	}
	spi_nor_unlock();
	return ret;
}

int spi_nor_write_data(uint32_t addr, const void *data, uint32_t len)
{
	const uint8_t *src = (const uint8_t *)data;
	uint32_t chunk;
	uint32_t page_left;
	int ret;

	if ((len != 0U && data == NULL) || !spi_nor_range_valid(addr, len))
		return -EINVAL;
	if (len == 0U)
		return 0;

	ret = spi_nor_lock();
	if (ret != 0)
		return ret;
	ret = spi_nor_hw_init();
	while (ret == 0 && len != 0U) {
		page_left = SPI_NOR_PAGE_SIZE - (addr % SPI_NOR_PAGE_SIZE);
		chunk = len > CQSPI_STIG_DATA_MAX ? CQSPI_STIG_DATA_MAX : len;
		if (chunk > page_left)
			chunk = page_left;

		ret = spi_nor_write_enable();
		if (ret == 0)
			ret = qspi_exec(SPI_NOR_CMD_PAGE_PROGRAM, true, addr,
					src, chunk, NULL, 0);
		if (ret == 0)
			ret = spi_nor_wait_ready(SPI_NOR_PROGRAM_TIMEOUT_MS);
		addr += chunk;
		src += chunk;
		len -= chunk;
	}
	spi_nor_unlock();
	return ret;
}

int spi_nor_read_data(uint32_t addr, void *data, uint32_t len)
{
	uint8_t *dst = (uint8_t *)data;
	uint32_t chunk;
	int ret;

	if ((len != 0U && data == NULL) || !spi_nor_range_valid(addr, len))
		return -EINVAL;
	if (len == 0U)
		return 0;

	ret = spi_nor_lock();
	if (ret != 0)
		return ret;
	ret = spi_nor_hw_init();
	while (ret == 0 && len != 0U) {
		chunk = len > CQSPI_STIG_DATA_MAX ? CQSPI_STIG_DATA_MAX : len;
		ret = qspi_exec(SPI_NOR_CMD_READ, true, addr, NULL, 0,
				dst, chunk);
		addr += chunk;
		dst += chunk;
		len -= chunk;
	}
	spi_nor_unlock();
	return ret;
}
