#include <linux/types.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/uaccess.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/of_reserved_mem.h>
#include <linux/of_address.h>
#include "quard_mbox_router.h"
#include <quard_nor_agent_protocol.h>
#include <quard_nor_layout.h>

#define KB(x)			((x) * 1024UL)
#define MB(x)			(KB(x) * 1024UL)

struct quard_nor_msg_req {
	struct quard_consumer_msg	req_msg;
	struct quard_consumer_msg	rsp_msg;
};

struct quard_nor_client_dev {
	struct device		*dev;
	void __iomem		*shram_vaddr;
	size_t			shram_size;
	dma_addr_t		shram_paddr;
	struct mutex		nor_op_lock;
	struct mtd_info		mtd;
	struct quard_mbox_consumer *mbox;
	bool			msg_in_progress;
	struct quard_nor_msg_req	*cur_req;
	struct completion	done;
	struct quard_nor_info	nor_info;
	struct mtd_partition	gpt;
};

static inline struct quard_nor_client_dev *mtd_to_quard_nor(struct mtd_info *mtd)
{
	return container_of(mtd, struct quard_nor_client_dev, mtd);
}

static void quard_mbox_recv_func(struct device *dev, struct quard_consumer_msg *msg)
{
	struct quard_nor_client_dev *nor = dev_get_drvdata(dev);
	struct quard_nor_msg_req *cur_req = nor->cur_req;

	dev_info(dev, "rx msg comes, cmd 0x%x\n", msg->cmd);
	if (nor->msg_in_progress && (msg->cmd == cur_req->req_msg.cmd)) {
		memcpy(&(nor->cur_req->rsp_msg), msg,
		       sizeof(struct quard_consumer_msg));
		complete(&nor->done);
	} else {
		dev_err(dev, "unbalanced rx msg comes\n");
	}
}

#define MAX_RX_TIMEOUT		(msecs_to_jiffies(200000))

static int quard_nor_msg_send(struct quard_nor_client_dev *nor,
			   struct quard_nor_msg_req *req)
{
	int ret;

	reinit_completion(&nor->done);

	nor->msg_in_progress = 1;
	nor->cur_req = req;
	ret = quard_mbox_send_msg(nor->mbox, &req->req_msg);
	if (ret) {
		dev_err(nor->dev, "send mbox msg failed\n");
		goto send_failed;
	}

	if (!wait_for_completion_timeout(&nor->done, MAX_RX_TIMEOUT)) {
		dev_err(nor->dev, "wait msg response timed-out\n");
		ret = -ETIMEDOUT;
	}

send_failed:
	nor->msg_in_progress = 0;
	nor->cur_req = NULL;

	return ret;
}

static uint32_t sum_32(const uint8_t *buf, uint32_t len)
{
	int i;
	uint32_t sum = 0;
	uint32_t rem = len % 4;
	uint32_t size = len / 4;
	const uint32_t *buf_32 = (uint32_t *)buf;

	/* Sum32 for 4bytes group */
	for (i = 0; i < size; i++) {
		sum += buf_32[i];
		buf += 4;
	}

	/* Sum8 for last few bytes */
	for (i = rem; i > 0; i--) {
		sum += buf[rem - i];
	}

	return sum;
}

static int __quard_nor_read(struct quard_nor_client_dev *nor, loff_t from,
			 size_t len, u_char *buf)
{
	int ret = 0;
	struct quard_nor_msg_req request = { 0 };
	struct quard_consumer_msg *tx = &(request.req_msg);
	struct quard_consumer_msg *rx = &(request.rsp_msg);
	struct quard_nor_common_param *tx_params = (struct quard_nor_common_param *)tx->params;
	struct quard_nor_common_param *rx_params = (struct quard_nor_common_param *)rx->params;
	uint32_t sum;

	tx->cmd = NOR_READ;
	tx_params->op_offset = from;
	tx_params->op_len = len;
	tx_params->shram_phy_addr = nor->shram_paddr;
	ret = quard_nor_msg_send(nor, &request);
	if (ret) {
		dev_err(nor->dev, "quard_nor_msg_send failed %s\n", __func__);
		return ret;
	}

	if (rx_params->status != NOR_OP_OK) {
		dev_err(nor->dev, "quard nor read response not ok\n");
		return -EIO;
	} else {
		/* Copy data from share mem */
		rmb();
		memcpy(buf, nor->shram_vaddr, len);
		sum = sum_32(buf, len);
		if (sum != rx_params->check_sum) {
			dev_err(nor->dev, "Checksum verify failed!"
				"target sum 0x%x, cal sum 0x%x\n",
				rx_params->check_sum, sum);
			return -EIO;
		} else {
			dev_info(nor->dev, "Rx checksum pass 0x%x\n", sum);
		}
	}

	return 0;
}

static int quard_nor_read(struct mtd_info *mtd, loff_t from, size_t len,
			 size_t *retlen, u_char *buf)
{
	int ret = 0;
	struct quard_nor_client_dev *nor = mtd_to_quard_nor(mtd);
	uint32_t remain = len;
	uint32_t cur_xfer_len;
	*retlen = 0;

	dev_info(nor->dev, "from 0x%08x, len %zd\n", (u32)from, len);
	mutex_lock(&nor->nor_op_lock);
	while (remain) {
		cur_xfer_len = (remain > nor->shram_size) ? nor->shram_size : remain;
		ret = __quard_nor_read(nor, from, cur_xfer_len, buf);
		if (ret) {
			dev_err(nor->dev, "read failed\n");
			mutex_unlock(&nor->nor_op_lock);
			return ret;
		}
		from += cur_xfer_len;
		*retlen += cur_xfer_len;
		buf += cur_xfer_len;
		remain -= cur_xfer_len;
	}
	mutex_unlock(&nor->nor_op_lock);

	return ret;
}

static int __quard_nor_write(struct quard_nor_client_dev *nor , loff_t to,
			  size_t len, const u_char *buf)
{
	int ret;
	uint32_t sum;
	struct quard_nor_msg_req request = { 0 };
	struct quard_consumer_msg *tx = &(request.req_msg);
	struct quard_consumer_msg *rx = &(request.rsp_msg);
	struct quard_nor_common_param *tx_params = (struct quard_nor_common_param *)tx->params;
	struct quard_nor_common_param *rx_params = (struct quard_nor_common_param *)rx->params;

	tx->cmd = NOR_WRITE;
	tx_params->op_offset = to;
	tx_params->op_len = len;

	sum = sum_32(buf, len);
	/* Copy data to share mem */
	memcpy(nor->shram_vaddr, buf, len);
	tx_params->shram_phy_addr = (uint32_t)nor->shram_paddr;
	tx_params->check_sum = sum;
	wmb();

	dev_info(nor->dev, "tx sum32 0x%x\n", sum);
	ret = quard_nor_msg_send(nor, &request);
	if (ret) {
		dev_err(nor->dev, "quard_nor_msg_send failed %s\n", __func__);
		return ret;
	}

	if (rx_params->status != NOR_OP_OK)
		return -EIO;

	return 0;
}

static int quard_nor_get_mtd_info(struct quard_nor_client_dev *nor);
static void quard_register_partitions(struct quard_nor_client_dev *nor);
static int quard_nor_write(struct mtd_info *mtd, loff_t to, size_t len,
			size_t *retlen, const u_char *buf)
{
	int ret;
	struct quard_nor_client_dev *nor = mtd_to_quard_nor(mtd);
	uint32_t remain = len;
	uint32_t cur_xfer_len;
	uint32_t offset;

	*retlen = 0;

	offset = to;
	dev_info(nor->dev, "to 0x%08x, len %zd\n", (u32)to, len);
	mutex_lock(&nor->nor_op_lock);
	while (remain) {
		cur_xfer_len = (remain > nor->shram_size) ? nor->shram_size : remain;
		ret = __quard_nor_write(nor, to, cur_xfer_len, buf);
		if (ret) {
			dev_err(nor->dev, "write failed\n");
			mutex_unlock(&nor->nor_op_lock);
			return ret;
		}
		to += cur_xfer_len;
		*retlen += cur_xfer_len;
		buf += cur_xfer_len;
		remain -= cur_xfer_len;
	}

	mutex_unlock(&nor->nor_op_lock);
	/* If gpt is wrritten, update paritions */
	if (ret == 0) {
		if ((offset == nor->gpt.offset) && len) {
			ret = quard_nor_get_mtd_info(nor);
			if (ret < 0) {
				dev_info(nor->dev, "get nor info failed\n");
				return ret;
			}
			quard_register_partitions(nor);
		}
	}

	return ret;
}

static void quard_del_partitions(struct quard_nor_client_dev *nor);
static int quard_nor_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	int ret;
	struct quard_nor_client_dev *nor = mtd_to_quard_nor(mtd);
	struct quard_nor_msg_req request = { 0 };
	struct quard_consumer_msg *tx = &(request.req_msg);
	struct quard_consumer_msg *rx = &(request.rsp_msg);
	struct quard_nor_common_param *tx_params = (struct quard_nor_common_param *)tx->params;
	struct quard_nor_common_param *rx_params = (struct quard_nor_common_param *)rx->params;
	struct quard_nor_info *info = &nor->nor_info;

	dev_info(nor->dev, "erase at 0x%llx, len %lld\n", (long long)instr->addr,
			(long long)instr->len);

	tx->cmd = NOR_ERASE;
	tx_params->op_offset = instr->addr;
	tx_params->op_len = instr->len;
	mutex_lock(&nor->nor_op_lock);
	ret = quard_nor_msg_send(nor, &request);
	if (ret) {
		dev_err(nor->dev, "quard_nor_msg_send failed %s\n", __func__);
		goto erase_failed;
	}

	if (rx_params->status != NOR_OP_OK)
		ret = -EIO;

	/* If gpt is erased, delete paritions */
	if (ret == 0) {
		if ((instr->addr == nor->gpt.offset) && instr->len) {
			quard_del_partitions(nor);
			info->nparts = 1;
		}
	}
erase_failed:
	mutex_unlock(&nor->nor_op_lock);

	return ret;
}

static void quard_register_partitions(struct quard_nor_client_dev *nor)
{
	int i, ret;
	struct quard_nor_info *info = &nor->nor_info;
	nor_part_t *part;
	struct mtd_info *mtd = &nor->mtd;

	for (i = 0; i < info->nparts; i++) {
		part = &info->parts[i];
		ret = mtd_add_partition(mtd, part->name,
					part->offset, part->length);
		if (ret < 0)
			dev_err(nor->dev, "register part %s failed\n", part->name);
	}
}

static void quard_del_partitions(struct quard_nor_client_dev *nor)
{
	int i, ret;
	struct quard_nor_info *info = &nor->nor_info;
	nor_part_t *part;
	struct mtd_info *mtd = &nor->mtd;

	for (i = 0; i < info->nparts; i++) {
		part = &info->parts[i];
		dev_info(nor->dev, "del part %s ...\n", part->name);
		ret = mtd_del_partition(mtd, i);
		if (ret < 0)
			dev_err(nor->dev, "del part %s failed\n", part->name);
	}
}

static int quard_nor_get_mtd_info(struct quard_nor_client_dev *nor)
{
	int ret = 0;
	struct quard_nor_msg_req request = { 0 };
	struct quard_consumer_msg *tx = &(request.req_msg);
	struct quard_consumer_msg *rx = &(request.rsp_msg);
	struct quard_nor_common_param *tx_params = (struct quard_nor_common_param *)tx->params;
	struct quard_nor_common_param *rx_params = (struct quard_nor_common_param *)rx->params;
	uint32_t sum;
	uint32_t i;
	struct mtd_partition *gpt = &nor->gpt;
	struct quard_nor_info *nor_info = &nor->nor_info;

	tx->cmd = NOR_GET_INFO;
	tx_params->op_len = sizeof(struct quard_nor_info);
	tx_params->shram_phy_addr = nor->shram_paddr;
	mutex_lock(&nor->nor_op_lock);
	ret = quard_nor_msg_send(nor, &request);
	if (ret) {
		dev_err(nor->dev, "quard_nor_msg_send failed %s\n", __func__);
		goto info_out;
	}

	if (rx_params->status != NOR_OP_OK) {
		dev_err(nor->dev, "quard nor get info cmd not ok\n");
		ret = -EIO;
		goto info_out;
	} else {
		/* Copy data from share mem */
		rmb();
		memcpy(nor_info, nor->shram_vaddr, sizeof(struct quard_nor_info));
		sum = sum_32((void *)nor_info, sizeof(struct quard_nor_info));
		if (sum != rx_params->check_sum) {
			dev_err(nor->dev, "Checksum verify failed!"
				"target sum 0x%x, cal sum 0x%x\n",
				rx_params->check_sum, sum);
			ret = -EIO;
			goto info_out;
		} else {
			if (nor_info->abi_version != QUARD_NOR_ABI_VERSION ||
			    nor_info->capacity != QUARD_NOR_FLASH_SIZE ||
			    nor_info->sector_size != QUARD_NOR_SECTOR_SIZE ||
			    nor_info->page_size != QUARD_NOR_PAGE_SIZE ||
			    nor_info->erase_size != QUARD_NOR_ERASE_SIZE ||
			    nor_info->nparts != QUARD_NOR_EXPECTED_PARTS) {
				dev_err(nor->dev, "invalid NOR information\n");
				ret = -EINVAL;
				goto info_out;
			}
			dev_info(nor->dev, "Rx checksum pass 0x%x\n", sum);
			for (i = 0; i < nor_info->nparts; i++) {
				if (!strcmp(nor_info->parts[i].name,
					    QUARD_NOR_GPT_NAME)) {
					gpt->name = nor_info->parts[i].name;
					gpt->offset = nor_info->parts[i].offset;
					gpt->size = nor_info->parts[i].length;
					break;
				}
			}
			if (i == nor_info->nparts) {
				dev_err(nor->dev, "GPT partition not reported\n");
				ret = -EINVAL;
				goto info_out;
			}
		}
	}
info_out:
	mutex_unlock(&nor->nor_op_lock);

	return ret;
}

static int quard_nor_set_mtd_info(struct quard_nor_client_dev *client_dev)
{
	int ret;
	struct device *dev = client_dev->dev;
	struct device_node *np = dev->of_node;
	struct mtd_info *mtd = &client_dev->mtd;

	ret = quard_nor_get_mtd_info(client_dev);
	if (ret < 0) {
		dev_info(client_dev->dev, "get nor info failed\n");
		return ret;
	}
	mtd_set_of_node(mtd, np);
	mtd->name = "quard-nor";
	mtd->owner = THIS_MODULE;
	mtd->dev.parent = dev;
	mtd->type = MTD_NORFLASH;
	mtd->flags = MTD_CAP_NORFLASH;
	/* Minimal writable flash unit size. In case of NOR flash it is 1 */
	mtd->writesize = 1;
	/* Geometry comes from the FreeRTOS NOR agent. */
	mtd->writebufsize = client_dev->nor_info.page_size;
	mtd->size = client_dev->nor_info.capacity;
	mtd->_read = quard_nor_read;
	mtd->_write = quard_nor_write;
	mtd->_erase = quard_nor_erase;
	mtd->erasesize = client_dev->nor_info.erase_size;

	dev_info(dev, "(%lld Kbytes)\n", (long long)mtd->size >> 10);

	dev_info(dev, ".size = 0x%llx (%lldMiB), "
		 ".erasesize = 0x%.8x (%uKiB)\n",
		 (long long)mtd->size, (long long)(mtd->size >> 20),
		 mtd->erasesize, mtd->erasesize / 1024);

	return 0;
}

static int quard_nor_reserved_mem_init(struct quard_nor_client_dev *client_dev)
{
	int ret;
	struct device_node *resv_node;
	struct device *dev = client_dev->dev;
	struct resource res;

	resv_node = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!resv_node) {
		dev_err(dev, "failed to find memory-region in dts\n");
		return -ENODEV;
	}

	ret = of_address_to_resource(resv_node, 0, &res);
    of_node_put(resv_node);
    if (ret) {
        dev_err(dev, "Failed to get resource from DTS\n");
        return ret;
    }

	client_dev->shram_paddr = res.start;
	client_dev->shram_size = resource_size(&res);
	if (res.start != QUARD_NOR_SHRAM_BASE ||
	    client_dev->shram_size != QUARD_NOR_SHRAM_SIZE ||
	    res.end > U32_MAX ||
	    client_dev->shram_size < sizeof(client_dev->nor_info)) {
		dev_err(dev, "invalid NOR shared memory resource\n");
		return -EINVAL;
	}
    
    dev_info(dev, "Reserved mem found: pa: %pa, size: %zu\n", &res.start, client_dev->shram_size);

	client_dev->shram_vaddr = memremap(res.start, resource_size(&res), MEMREMAP_WT);
    if (!client_dev->shram_vaddr) {
        dev_err(dev, "memremap failed!\n");
        return -ENOMEM;
    }
    
    dev_info(dev, "memremap success at vaddr: %px\n", client_dev->shram_vaddr);

	return 0;
}

static void quard_nor_reserved_mem_deinit(struct quard_nor_client_dev *client_dev)
{
	if (client_dev->shram_vaddr) {
        memunmap(client_dev->shram_vaddr);
        client_dev->shram_vaddr = NULL;
    }
}

static int quard_nor_client_probe(struct platform_device *pdev)
{
	int ret;
	struct device *dev = &pdev->dev;
	struct quard_nor_client_dev *client_dev;
	struct quard_mbox_consumer *mbox;

	mbox = quard_mbox_consumer_register(dev, quard_mbox_recv_func);
	if (IS_ERR(mbox)) {
		dev_err(dev, "failed to match mbox router\n");
		return PTR_ERR(mbox);
	}

	client_dev = devm_kzalloc(dev, sizeof(struct quard_nor_client_dev), GFP_KERNEL);
	if (!client_dev) {
		dev_err(dev, "failed to allocate memory for client_dev\n");
		return -ENOMEM;
	}

	client_dev->dev = dev;
	client_dev->mbox = mbox;

	ret = quard_nor_reserved_mem_init(client_dev);
	if (ret) {
		dev_err(dev, "failed to init reserved memory\n");
		return -ENOMEM;
	}

	mutex_init(&client_dev->nor_op_lock);
	init_completion(&client_dev->done);
	dev_set_drvdata(dev, client_dev);

	ret = quard_nor_set_mtd_info(client_dev);
	if (ret) {
		dev_err(dev, "failed to set mtd info, aborting probe\n");
		goto err_set_info; 
	}

	ret = mtd_device_register(&client_dev->mtd, NULL, 0);
	if (ret) {
		dev_err(dev, "mtd register failed\n");
		goto err_set_info; 
	}

	quard_register_partitions(client_dev);
	dev_info(dev, "nor client dev probed, shram addr 0x%x, size 0x%x\n",
		 (uint32_t)client_dev->shram_paddr,
		 (uint32_t)client_dev->shram_size);
	
	return 0;
	
err_set_info:		 
	quard_nor_reserved_mem_deinit(client_dev);
	mutex_destroy(&client_dev->nor_op_lock);
	quard_mbox_consumer_unregister(mbox);

	return ret;
}

static int quard_nor_client_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct quard_nor_client_dev *client_dev;

	client_dev = dev_get_drvdata(&pdev->dev);

	quard_mbox_consumer_unregister(client_dev->mbox);
	quard_nor_reserved_mem_deinit(client_dev);
	mutex_destroy(&client_dev->nor_op_lock);
	mtd_device_unregister(&client_dev->mtd);
	dev_err(dev, "quard_nor_client driver removed\n");

	return 0;
}

static const struct of_device_id quard_nor_client_of_match[] = {
	{.compatible = "quard,nor-client", },
	{},
};
MODULE_DEVICE_TABLE(of, quard_nor_client_of_match);

static struct platform_driver quard_nor_client_driver = {
	.probe     = quard_nor_client_probe,
	.remove    = quard_nor_client_remove,
	.driver    = {
			.name = "quard-nor-client",
			.owner = THIS_MODULE,
			.of_match_table = of_match_ptr(quard_nor_client_of_match),
		},
};

static int __init quard_nor_client_init(void)
{
	int ret;

	ret = platform_driver_register(&quard_nor_client_driver);
	if (ret)
		pr_warn("quard_nor_client driver not registered\n");

	pr_info("quard_nor_client_init enter!\n");

	return ret;
}

static void __exit quard_nor_client_exit(void)
{
	pr_info("quard_nor_client_exit enter!\n");
	platform_driver_unregister(&quard_nor_client_driver);
}

module_init(quard_nor_client_init);
module_exit(quard_nor_client_exit);

MODULE_AUTHOR("quard team");
MODULE_DESCRIPTION("pflash client driver");
MODULE_VERSION("V0.1");
MODULE_LICENSE("GPL v2");
