#define LOG_TAG "NOR_AGENT"
#include "elog.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "gpt.h"
#include "quard_mbox_router_consumer.h"
#include "spi_nor.h"
#include "../../../common_inc/bsp/quard_nor_agent_protocol.h"
#include "../../../common_inc/bsp/quard_nor_layout.h"

#define NOR_CLIENT_STACK_SIZE 0x300
#define NOR_QUEUE_MSG_NUM     10
#define MBOX_CONS_TASK_PRI    4

struct quard_nor_agent {
	StackType_t stack_buf[NOR_CLIENT_STACK_SIZE];
	struct quard_consumer_msg rx_queue_buf[NOR_QUEUE_MSG_NUM];
	struct quard_nor_info info;
	bool info_valid;
};

static struct quard_nor_agent nor_agent;
static struct quard_mbox_consumer nor_mbox;

static void shared_memory_barrier(void)
{
	__asm volatile("fence rw, rw" ::: "memory");
}

static uint32_t sum_32(const void *buffer, uint32_t length)
{
	const uint8_t *data = buffer;
	uint32_t sum = 0U;
	uint32_t word;

	while (length >= sizeof(word)) {
		memcpy(&word, data, sizeof(word));
		sum += word;
		data += sizeof(word);
		length -= sizeof(word);
	}
	while (length-- != 0U)
		sum += *data++;
	return sum;
}

static bool master_range_valid(uint32_t offset, uint32_t length)
{
	return length != 0U && offset < QUARD_NOR_FLASH_SIZE &&
	       length <= QUARD_NOR_FLASH_SIZE - offset;
}

static bool shared_buffer_valid(uint32_t address, uint32_t length)
{
	return length != 0U && address == QUARD_NOR_SHRAM_BASE &&
	       length <= QUARD_NOR_SHRAM_SIZE;
}

static int spi_nor_gpt_read(void *context, uint32_t offset, void *buffer,
			    uint32_t length)
{
	(void)context;
	return spi_nor_read_data(offset, buffer, length);
}

static int refresh_flash_info(void)
{
	struct quard_nor_info *info = &nor_agent.info;
	int ret;

	memset(info, 0, sizeof(*info));
	ret = spi_nor_get_id(&info->id);
	if (ret != 0)
		goto failed;
	info->abi_version = QUARD_NOR_ABI_VERSION;
	info->capacity = QUARD_NOR_FLASH_SIZE;
	info->sector_size = QUARD_NOR_SECTOR_SIZE;
	info->page_size = QUARD_NOR_PAGE_SIZE;
	info->erase_size = QUARD_NOR_ERASE_SIZE;
	ret = gpt_read_partitions(spi_nor_gpt_read, NULL, info);
	if (ret != 0)
		goto failed;
	nor_agent.info_valid = true;
	return 0;

failed:
	nor_agent.info_valid = false;
	memset(info, 0, sizeof(*info));
	return ret;
}

static bool destructive_range_valid(uint32_t offset, uint32_t length)
{
	uint32_t i;

	if (offset >= QUARD_NOR_RESERVED_OFFSET &&
	    length <= QUARD_NOR_RESERVED_SIZE &&
	    offset - QUARD_NOR_RESERVED_OFFSET <=
		QUARD_NOR_RESERVED_SIZE - length)
		return true;
	if (!nor_agent.info_valid && refresh_flash_info() != 0)
		return false;
	for (i = 0; i < nor_agent.info.nparts; i++) {
		const nor_part_t *part = &nor_agent.info.parts[i];

		if (strcmp(part->name, QUARD_NOR_GPT_NAME) == 0)
			continue;
		if (offset >= part->offset && length <= part->length &&
		    offset - part->offset <= part->length - length)
			return true;
	}
	return false;
}

static void nor_agent_recv_cb(struct quard_consumer_msg *msg)
{
	struct quard_consumer_msg response;
	struct quard_nor_common_param *request;
	struct quard_nor_common_param *reply;
	void *buffer;
	uint32_t offset;
	uint32_t length;
	uint32_t address;
	uint32_t checksum;
	int ret = -EINVAL;

	if (msg->mbox_router_reserved[2] != QUARD_SERVER_ID_NOR) {
		LOG_E("Unknown ServerID(%u)", msg->mbox_router_reserved[2]);
		return;
	}
	memcpy(&response, msg, sizeof(response));
	request = (struct quard_nor_common_param *)msg->params;
	reply = (struct quard_nor_common_param *)response.params;
	reply->status = NOR_OP_FAILED;
	reply->check_sum = 0U;
	offset = request->op_offset;
	length = request->op_len;
	address = request->shram_phy_addr;
	buffer = (void *)(uintptr_t)address;

	LOG_D("cmd 0x%x, offset 0x%x, len 0x%x, addr 0x%x",
	      msg->cmd, offset, length, address);
	switch (msg->cmd) {
	case NOR_GET_INFO:
		if (!shared_buffer_valid(address, sizeof(nor_agent.info)) ||
		    length != sizeof(nor_agent.info))
			break;
		ret = refresh_flash_info();
		if (ret != 0)
			break;
		memcpy(buffer, &nor_agent.info, sizeof(nor_agent.info));
		shared_memory_barrier();
		reply->check_sum = sum_32(buffer, sizeof(nor_agent.info));
		break;
	case NOR_READ:
		if (!master_range_valid(offset, length) ||
		    !shared_buffer_valid(address, length))
			break;
		ret = spi_nor_read_data(offset, buffer, length);
		if (ret == 0) {
			shared_memory_barrier();
			reply->check_sum = sum_32(buffer, length);
		}
		break;
	case NOR_WRITE:
		if (!master_range_valid(offset, length) ||
		    !shared_buffer_valid(address, length) ||
		    !destructive_range_valid(offset, length))
			break;
		shared_memory_barrier();
		checksum = sum_32(buffer, length);
		if (checksum != request->check_sum) {
			ret = -EBADMSG;
			break;
		}
		ret = spi_nor_write_data(offset, buffer, length);
		break;
	case NOR_ERASE:
		if (!master_range_valid(offset, length) ||
		    (offset % QUARD_NOR_ERASE_SIZE) != 0U ||
		    (length % QUARD_NOR_ERASE_SIZE) != 0U ||
		    !destructive_range_valid(offset, length))
			break;
		ret = spi_nor_erase_sector(offset, length);
		break;
	case NOR_SEC_WRITE:
	case NOR_SEC_READ:
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	if (ret == 0)
		reply->status = NOR_OP_OK;
	else
		LOG_E("NOR cmd 0x%x rejected/failed: %d", msg->cmd, ret);
	quard_router_send_msg(&nor_mbox, &response);
}

void init_quard_nor_agent(void)
{
	struct quard_mbox_consumer_cfg *cfg = &nor_mbox.cfg;

	memset(&nor_agent, 0, sizeof(nor_agent));
	cfg->server_id = QUARD_SERVER_ID_NOR;
	cfg->thread_stack = nor_agent.stack_buf;
	cfg->stack_depth = NOR_CLIENT_STACK_SIZE;
	cfg->thread_priority = MBOX_CONS_TASK_PRI;
	cfg->recv_cb = nor_agent_recv_cb;
	cfg->rx_queue_item_num = NOR_QUEUE_MSG_NUM;
	cfg->rx_queue_buf = nor_agent.rx_queue_buf;
	quard_mbox_consumer_register(&nor_mbox);
}
