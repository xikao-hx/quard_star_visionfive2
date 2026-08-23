// SPDX-License-Identifier: GPL-2.0+
/* Adapted from bsp/uboot/quard_nor_client.c. */

#include <common.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <mapmem.h>
#include <mtd.h>
#include <quard_mbox_router.h>
#include <linux/errno.h>
#include <linux/mtd/partitions.h>

#include "../../../common_inc/bsp/quard_nor_agent_protocol.h"
#include "../../../common_inc/bsp/quard_router_protocol.h"
#include "../../../common_inc/bsp/quard_router_server_id.h"

struct quard_nor_msg_req {
	struct quard_consumer_msg req_msg;
	struct quard_consumer_msg rsp_msg;
};

struct quard_nor_client_dev {
	struct udevice *dev;
	void *shram_vaddr;
	size_t shram_size;
	phys_addr_t shram_paddr;
	struct mtd_info mtd;
	struct quard_mbox_consumer consumer;
	bool msg_in_progress;
	struct quard_nor_msg_req *cur_req;
	struct quard_nor_info nor_info;
	struct mtd_partition partitions[QUARD_NOR_MAX_PARTS];
};

static void quard_mbox_recv_func(struct udevice *dev,
				 struct quard_consumer_msg *msg)
{
	struct quard_nor_client_dev *nor = dev_get_priv(dev);
	struct quard_nor_msg_req *cur_req = nor->cur_req;

	if (nor->msg_in_progress && cur_req &&
	    msg->cmd == cur_req->req_msg.cmd)
		nor->msg_in_progress = false;
	else
		dev_err(dev, "unbalanced NOR response\n");
}

static int quard_nor_msg_send(struct quard_nor_client_dev *nor,
			      struct quard_nor_msg_req *req)
{
	struct quard_mbox_msg *response =
		(struct quard_mbox_msg *)&req->rsp_msg;
	int ret;

	nor->msg_in_progress = true;
	nor->cur_req = req;
	ret = quard_mbox_send_msg(&nor->consumer, &req->req_msg);
	if (ret)
		goto out;
	ret = quard_mbox_receive(&nor->consumer, &req->rsp_msg);
	if (ret)
		goto out;
	if (nor->msg_in_progress ||
	    response->header.fields.quard_server_id != nor->consumer.server_id ||
	    response->header.fields.quard_server_cmd != req->req_msg.cmd)
		ret = -EPROTO;
out:
	nor->msg_in_progress = false;
	nor->cur_req = NULL;
	return ret;
}

static u32 sum_32(const u8 *buf, u32 len)
{
	u32 sum = 0;

	while (len >= sizeof(u32)) {
		u32 word;

		memcpy(&word, buf, sizeof(word));
		sum += word;
		buf += sizeof(word);
		len -= sizeof(word);
	}
	while (len--)
		sum += *buf++;
	return sum;
}

static int __quard_nor_read(struct quard_nor_client_dev *nor, loff_t from,
			    size_t len, u_char *buf)
{
	struct quard_nor_msg_req request = { 0 };
	struct quard_consumer_msg *tx = &request.req_msg;
	struct quard_consumer_msg *rx = &request.rsp_msg;
	struct quard_nor_common_param *tx_params =
		(struct quard_nor_common_param *)tx->params;
	struct quard_nor_common_param *rx_params =
		(struct quard_nor_common_param *)rx->params;
	u32 sum;
	int ret;

	tx->cmd = NOR_READ;
	tx_params->op_offset = from;
	tx_params->op_len = len;
	tx_params->shram_phy_addr = nor->shram_paddr;
	ret = quard_nor_msg_send(nor, &request);
	if (ret)
		return ret;
	if (rx_params->status != NOR_OP_OK)
		return -EIO;

	asm volatile("fence rw, rw" ::: "memory");
	memcpy(buf, nor->shram_vaddr, len);
	sum = sum_32(buf, len);
	if (sum != rx_params->check_sum) {
		dev_err(nor->dev, "checksum mismatch: expected 0x%x got 0x%x\n",
			rx_params->check_sum, sum);
		return -EBADMSG;
	}
	return 0;
}

static int quard_nor_read(struct mtd_info *mtd, loff_t from, size_t len,
			  size_t *retlen, u_char *buf)
{
	struct quard_nor_client_dev *nor = dev_get_priv(mtd->dev);
	size_t remain = len;
	int ret = 0;

	*retlen = 0;
	if (from < 0 || from >= mtd->size || len > mtd->size - from)
		return -EINVAL;
	while (remain) {
		size_t cur_xfer_len = min_t(size_t, remain, nor->shram_size);

		ret = __quard_nor_read(nor, from, cur_xfer_len, buf);
		if (ret)
			return ret;
		from += cur_xfer_len;
		*retlen += cur_xfer_len;
		buf += cur_xfer_len;
		remain -= cur_xfer_len;
	}
	return 0;
}

static int __quard_nor_write(struct quard_nor_client_dev *nor, loff_t to,
			     size_t len, const u_char *buf)
{
	struct quard_nor_msg_req request = { 0 };
	struct quard_consumer_msg *tx = &request.req_msg;
	struct quard_consumer_msg *rx = &request.rsp_msg;
	struct quard_nor_common_param *tx_params =
		(struct quard_nor_common_param *)tx->params;
	struct quard_nor_common_param *rx_params =
		(struct quard_nor_common_param *)rx->params;
	u32 sum = sum_32(buf, len);
	int ret;

	memcpy(nor->shram_vaddr, buf, len);
	asm volatile("fence rw, rw" ::: "memory");
	tx->cmd = NOR_WRITE;
	tx_params->op_offset = to;
	tx_params->op_len = len;
	tx_params->shram_phy_addr = nor->shram_paddr;
	tx_params->check_sum = sum;
	ret = quard_nor_msg_send(nor, &request);
	if (ret)
		return ret;
	if (rx_params->status != NOR_OP_OK)
		return -EIO;
	return 0;
}

static int quard_nor_write(struct mtd_info *mtd, loff_t to, size_t len,
			   size_t *retlen, const u_char *buf)
{
	struct quard_nor_client_dev *nor = dev_get_priv(mtd->dev);
	size_t remain = len;
	int ret;

	*retlen = 0;
	if (to < 0 || to >= mtd->size || len > mtd->size - to)
		return -EINVAL;
	while (remain) {
		size_t cur_xfer_len = min_t(size_t, remain, mtd->erasesize);

		cur_xfer_len = min_t(size_t, cur_xfer_len, nor->shram_size);

		ret = __quard_nor_write(nor, to, cur_xfer_len, buf);
		if (ret)
			return ret;
		to += cur_xfer_len;
		*retlen += cur_xfer_len;
		buf += cur_xfer_len;
		remain -= cur_xfer_len;
	}
	return 0;
}

static int quard_nor_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	struct quard_nor_client_dev *nor = dev_get_priv(mtd->dev);
	u64 offset;
	u64 remain;
	int ret;

	if ((instr->addr % mtd->erasesize) ||
	    (instr->len % mtd->erasesize))
		return -EINVAL;
	instr->state = MTD_ERASING;
	offset = instr->addr;
	remain = instr->len;
	while (remain) {
		struct quard_nor_msg_req request = { 0 };
		struct quard_consumer_msg *tx = &request.req_msg;
		struct quard_consumer_msg *rx = &request.rsp_msg;
		struct quard_nor_common_param *tx_params =
			(struct quard_nor_common_param *)tx->params;
		struct quard_nor_common_param *rx_params =
			(struct quard_nor_common_param *)rx->params;

		tx->cmd = NOR_ERASE;
		tx_params->op_offset = offset;
		tx_params->op_len = mtd->erasesize;
		ret = quard_nor_msg_send(nor, &request);
		if (!ret && rx_params->status != NOR_OP_OK)
			ret = -EIO;
		if (ret) {
			instr->state = MTD_ERASE_FAILED;
			instr->fail_addr = offset;
			return ret;
		}
		offset += mtd->erasesize;
		remain -= mtd->erasesize;
	}
	instr->state = MTD_ERASE_DONE;
	mtd_erase_callback(instr);
	return 0;
}

static int quard_nor_get_info(struct quard_nor_client_dev *nor)
{
	struct quard_nor_msg_req request = { 0 };
	struct quard_nor_common_param *tx =
		(struct quard_nor_common_param *)request.req_msg.params;
	struct quard_nor_common_param *rx =
		(struct quard_nor_common_param *)request.rsp_msg.params;
	int ret;

	request.req_msg.cmd = NOR_GET_INFO;
	tx->op_len = sizeof(nor->nor_info);
	tx->shram_phy_addr = nor->shram_paddr;
	ret = quard_nor_msg_send(nor, &request);
	if (ret)
		return ret;
	if (rx->status != NOR_OP_OK)
		return -EIO;
	asm volatile("fence rw, rw" ::: "memory");
	memcpy(&nor->nor_info, nor->shram_vaddr, sizeof(nor->nor_info));
	if (sum_32((const u8 *)&nor->nor_info, sizeof(nor->nor_info)) !=
	    rx->check_sum)
		return -EBADMSG;
	return 0;
}

static int quard_nor_reserved_mem_init(struct quard_nor_client_dev *nor)
{
	struct ofnode_phandle_args args;
	fdt_size_t size;
	fdt_addr_t address;
	int ret;

	ret = dev_read_phandle_with_args(nor->dev, "memory-region", NULL, 0, 0,
					 &args);
	if (ret)
		return ret;
	address = ofnode_get_addr_size_index(args.node, 0, &size);
	if (address != QUARD_NOR_SHRAM_BASE || size != QUARD_NOR_SHRAM_SIZE)
		return -EINVAL;
	nor->shram_paddr = address;
	nor->shram_size = size;
	nor->shram_vaddr = map_sysmem(address, size);
	return nor->shram_vaddr ? 0 : -ENOMEM;
}

static int quard_nor_set_mtd_info(struct quard_nor_client_dev *nor)
{
	struct mtd_info *mtd = &nor->mtd;

	mtd->dev = nor->dev;
	mtd_set_ofnode(mtd, dev_ofnode(nor->dev));
	mtd->name = "quard-nor";
	mtd->type = MTD_NORFLASH;
	mtd->flags = MTD_CAP_NORFLASH;
	mtd->writesize = 1;
	mtd->writebufsize = nor->nor_info.page_size;
	mtd->size = nor->nor_info.capacity;
	mtd->erasesize = nor->nor_info.erase_size;
	mtd->_read = quard_nor_read;
	mtd->_write = quard_nor_write;
	mtd->_erase = quard_nor_erase;
	return 0;
}

static int quard_nor_register_partitions(struct quard_nor_client_dev *nor)
{
	u32 i;

	if (nor->nor_info.nparts > ARRAY_SIZE(nor->partitions))
		return -EOVERFLOW;
	for (i = 0; i < nor->nor_info.nparts; i++) {
		nor->partitions[i].name = nor->nor_info.parts[i].name;
		nor->partitions[i].offset = nor->nor_info.parts[i].offset;
		nor->partitions[i].size = nor->nor_info.parts[i].length;
		nor->partitions[i].mask_flags = 0;
	}
	return add_mtd_partitions(&nor->mtd, nor->partitions,
				  nor->nor_info.nparts);
}

static int quard_nor_client_probe(struct udevice *dev)
{
	struct quard_nor_client_dev *nor = dev_get_priv(dev);
	struct quard_mbox_consumer *consumer = &nor->consumer;
	int ret;

	nor->dev = dev;
	consumer->dev = dev;
	ret = quard_mbox_consumer_register(consumer, quard_mbox_recv_func);
	if (ret)
		return ret;
	ret = quard_nor_reserved_mem_init(nor);
	if (ret)
		goto unregister_consumer;
	ret = quard_nor_get_info(nor);
	if (ret)
		goto unregister_consumer;
	quard_nor_set_mtd_info(nor);
	ret = add_mtd_device(&nor->mtd);
	if (ret) {
		ret = -ENOMEM;
		goto unregister_consumer;
	}
	ret = quard_nor_register_partitions(nor);
	if (ret) {
		del_mtd_device(&nor->mtd);
		goto unregister_consumer;
	}
	dev_info(dev, "writable NOR MTD ready: size=0x%llx parts=%u\n",
		 nor->mtd.size, nor->nor_info.nparts);
	return 0;

unregister_consumer:
	quard_mbox_consumer_unregister(consumer);
	return ret;
}

static const struct udevice_id quard_nor_client_ids[] = {
	{ .compatible = "quard,nor-client" },
	{ }
};

U_BOOT_DRIVER(quard_nor_client) = {
	.name = "quard_nor_client",
	.id = UCLASS_MTD,
	.of_match = quard_nor_client_ids,
	.probe = quard_nor_client_probe,
	.priv_auto = sizeof(struct quard_nor_client_dev),
};
