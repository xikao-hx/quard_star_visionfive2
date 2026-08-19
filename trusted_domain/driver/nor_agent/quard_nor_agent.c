#define LOG_TAG "NOR_AGENT"
#include "elog.h"
#include <stdint.h>
#include "ipi_mailbox.h"
#include "pflash.h"
#include "gpt.h"
#include "quard_mbox_router_consumer.h"
#include "../../../common_inc/bsp/quard_nor_agent_protocol.h"

#define NOR_CLIENT_STACK_SIZE 0x300
#define NOR_QUEUE_MSG_NUM     10

struct quard_nor_agent{
	StackType_t stack_buf[NOR_CLIENT_STACK_SIZE];
	struct quard_consumer_msg rx_queue_buf[NOR_QUEUE_MSG_NUM];
};

static struct quard_nor_agent nor_agent;
static struct quard_mbox_consumer nor_mbox;

static uint32_t sum_32(void *buf, uint32_t len)
{
	uint32_t i;
	uint32_t sum = 0;
	uint32_t rem = len % 4UL;
	uint32_t size = len / 4UL;
	uint32_t *buf_32 = (uint32_t *)buf;
	uint8_t *buf_8 = (uint8_t *)buf;

	for (i = 0; i < size; i ++) {
		sum += buf_32[i];
		buf_8 += 4;
	}

	for (i = rem; i >0UL; i --) {
		sum += buf_8[rem - i];
	}

	return sum;
}

static void get_flash_info(struct quard_nor_info *pinfo) {

	gpt_header_t header;
	memset(pinfo, 0, sizeof(struct quard_nor_info));

	/* 1. 读取 GPT 头 */ 
	if (ota_get_header(&header) != 0) {
		LOG_D("ota_get_header failed!");
        return;
    }

    pinfo->id = 0x8918; 
    pinfo->capacity = PFLASH_SIZE;
    pinfo->otp_size = 0;
	
	/* 2. 校验 GPT 签名 */ 
    if (memcmp(header.signature, "EFI PART", 8) != 0) {
		LOG_D("GPT signature check failed!");
        return;
    }
	
	/* 3. 遍历分区条目 */
	if (parse_gpt_entry(&header, pinfo) != 0) {
		LOG_D("parse_gpt_entry failed!");
        return;
    }
}

static void nor_agent_recv_cb(struct quard_consumer_msg *msg)
{
	int ret = 0;
	struct quard_nor_common_param *req_params;
	struct quard_nor_common_param *rsp_params;
	struct quard_consumer_msg response;
	void *buf;
	uint32_t offset, len, phy_addr, sum;
	struct quard_mbox_consumer *mbox;
	mbox = &nor_mbox;

	uint8_t server_id = msg->mbox_router_reserved[2];

	if (server_id == QUARD_SERVER_ID_NOR) {
		mbox = &nor_mbox;
	} else {
		LOG_E("Unkown ServerID(%d)!\n", server_id);
	}

	req_params = (struct quard_nor_common_param *)msg->params;
	rsp_params = (struct quard_nor_common_param *)response.params;

	memcpy(&response, msg, sizeof(struct quard_consumer_msg));
	rsp_params->status = NOR_OP_OK;
	offset = req_params->op_offset;
	len = req_params->op_len;
	phy_addr = req_params->shram_phy_addr;

	LOG_D("cmd 0x%x, offset 0x%x, len 0x%x, addr_ap 0x%x",
		msg->cmd, offset, len, phy_addr);

	buf = (void *)(uintptr_t)phy_addr;
	switch (msg->cmd) {
	case NOR_GET_INFO:
		
		get_flash_info((struct quard_nor_info *)buf);
		sum = sum_32(buf, sizeof(struct quard_nor_info));
		rsp_params->check_sum = sum;
		LOG_D("Nor data Target sum 0x%x", sum);
		break;
	case NOR_WRITE:
	case NOR_SEC_WRITE:
		sum = sum_32(buf, len);
		if (req_params->check_sum != sum) {
			rsp_params->status = NOR_OP_FAILED;
			LOG_E("Checksum not match!! host_sum 0x%x, cal sum 0x%x", 
					req_params->check_sum, sum);
			break;
		} else {
			LOG_I("Checksum verify pass");
		} 

		if (msg->cmd == (uint8_t)NOR_WRITE) { 
			ret = pflash_write_data(offset, (flash_t *)buf, len);
		}

		if (ret < 0) {
			LOG_E("write flash failed");
			rsp_params->status = NOR_OP_FAILED;
		} else {
			LOG_I("write flash done!");
		}
		break;
	case NOR_READ:
	case NOR_SEC_READ:
		if (msg->cmd == (uint8_t)NOR_READ) {
			ret = pflash_read_data(offset, (flash_t *)buf, len);
		}

		if (ret < 0) {
			LOG_E("read flash failed");
			rsp_params->status = NOR_OP_FAILED;
		} else {
			sum = sum_32(buf, len);
			rsp_params->check_sum = sum;
			LOG_D("Nor data Target sum 0x%x", sum);
		}
		break;
	case NOR_ERASE:

		ret = pflash_erase_sector(offset, len);
		if (ret < 0) {
			LOG_E("erase flash failed");
			rsp_params->status = NOR_OP_FAILED;
		} else {
			LOG_I("erase flash done!");
		}
		break;
	default:
		rsp_params->status = NOR_OP_FAILED;
		LOG_E("unsupported cmd 0x%x", msg->cmd);
		break;
	}

	quard_router_send_msg(mbox, &response);
}

// TODO: 后面优化任务优先安排，并从配置文件中读取
#define MBOX_CONS_TASK_PRI 4   
void init_quard_nor_agent(void)
{
	struct quard_mbox_consumer *consumer = &nor_mbox;
	struct quard_mbox_consumer_cfg *cfg = &consumer->cfg;

	cfg->server_id = QUARD_SERVER_ID_NOR;
	cfg->thread_stack = nor_agent.stack_buf;
	cfg->stack_depth = NOR_CLIENT_STACK_SIZE;
	cfg->thread_priority = MBOX_CONS_TASK_PRI;
	cfg->recv_cb = nor_agent_recv_cb;
	cfg->rx_queue_item_num = NOR_QUEUE_MSG_NUM;
	cfg->rx_queue_buf = nor_agent.rx_queue_buf;

	quard_mbox_consumer_register(consumer);
}
