#include "spi_nor.h"

#include <errno.h>
#include <stdbool.h>

#include "FreeRTOS.h"
#include "cadence_qspi.h"
#include "semphr.h"
#include "task.h"

#define SPI_NOR_CMD_READ                0x03U
#define SPI_NOR_CMD_PAGE_PROGRAM        0x02U
#define SPI_NOR_CMD_WRITE_ENABLE        0x06U
#define SPI_NOR_CMD_READ_STATUS         0x05U
#define SPI_NOR_CMD_READ_ID             0x9fU
#define SPI_NOR_CMD_ERASE_4K            0x20U
#define SPI_NOR_STATUS_WIP              (1U << 0)
#define SPI_NOR_STATUS_WEL              (1U << 1)

#define SPI_NOR_BLOCK_SIZE_LOG2         12U
#define SPI_NOR_PROGRAM_TIMEOUT_MS      2000U
#define SPI_NOR_ERASE_TIMEOUT_MS        10000U

static StaticSemaphore_t spi_nor_mutex_storage;
static SemaphoreHandle_t spi_nor_mutex;
static bool spi_nor_ready;
static uint32_t spi_nor_jedec_id;

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

static int spi_nor_read_status(uint8_t *status)
{
	return cadence_qspi_command(SPI_NOR_CMD_READ_STATUS, false, 0,
				    NULL, 0, status, sizeof(*status));
}

static int spi_nor_write_enable(void)
{
	uint8_t status;
	int ret;

	ret = cadence_qspi_command(SPI_NOR_CMD_WRITE_ENABLE, false, 0,
				   NULL, 0, NULL, 0);
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
	uint8_t id[3];
	int ret;

	if (spi_nor_ready)
		return 0;

	ret = cadence_qspi_init(SPI_NOR_ADDR_BYTES, SPI_NOR_PAGE_SIZE,
				SPI_NOR_BLOCK_SIZE_LOG2);
	if (ret != 0)
		return ret;
	ret = cadence_qspi_command(SPI_NOR_CMD_READ_ID, false, 0,
				   NULL, 0, id, sizeof(id));
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
			ret = cadence_qspi_command(SPI_NOR_CMD_ERASE_4K, true,
						   current, NULL, 0, NULL, 0);
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
		chunk = len > page_left ? page_left : len;

		ret = spi_nor_write_enable();
		if (ret == 0)
			ret = cadence_qspi_indirect_write(
				SPI_NOR_CMD_PAGE_PROGRAM, addr, src, chunk);
		if (ret == 0)
			ret = spi_nor_wait_ready(SPI_NOR_PROGRAM_TIMEOUT_MS);
		if (ret != 0)
			break;
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
		chunk = len > CADENCE_QSPI_INDIRECT_READ_MAX ?
			CADENCE_QSPI_INDIRECT_READ_MAX : len;
		ret = cadence_qspi_indirect_read(SPI_NOR_CMD_READ, addr,
						  dst, chunk);
		if (ret != 0)
			break;
		addr += chunk;
		dst += chunk;
		len -= chunk;
	}
	spi_nor_unlock();
	return ret;
}
