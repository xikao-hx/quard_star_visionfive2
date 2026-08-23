// SPDX-License-Identifier: GPL-2.0+
/* Adapted from bsp/uboot/quard_mbox_router.c. */

#include <common.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <mailbox.h>
#include <quard_mbox_router.h>
#include <linux/err.h>
#include <linux/list.h>

#include "../../../common_inc/bsp/quard_router_protocol.h"

#define QUARD_MBOX_RX_TIMEOUT_US 15000000UL

struct quard_mbox_router {
	struct udevice *dev;
	struct mbox_chan tx;
	struct mbox_chan rx;
	struct list_head router_node;
	struct list_head consumer_list;
};

static LIST_HEAD(router_list);

static struct quard_mbox_consumer *
find_consumer_by_id(struct quard_mbox_router *router, u32 server_id)
{
	struct quard_mbox_consumer *consumer;

	list_for_each_entry(consumer, &router->consumer_list, consumer_node) {
		if (consumer->server_id == server_id)
			return consumer;
	}
	return NULL;
}

int quard_mbox_send_msg(struct quard_mbox_consumer *consumer,
			struct quard_consumer_msg *msg)
{
	struct quard_mbox_msg *quard_msg = (struct quard_mbox_msg *)msg;
	struct quard_mbox_router *router = consumer->router;
	int ret;

	quard_msg->header.fields.crc16 = 0;
	quard_msg->header.fields.quard_server_id = (u8)consumer->server_id;
	ret = mbox_send(&router->tx, quard_msg);
	return ret < 0 ? ret : 0;
}

static struct quard_mbox_router *
of_parse_consumer_server(struct udevice *dev, u32 *server_id)
{
	struct ofnode_phandle_args args;
	struct udevice *router_dev;
	ofnode server_node;
	ofnode parent_node;
	int ret;

	ret = dev_read_phandle_with_args(dev, "mbox-router-server", NULL, 0,
					 0, &args);
	if (ret) {
		dev_err(dev, "no mbox-router-server in DT: %d\n", ret);
		return NULL;
	}
	server_node = args.node;
	parent_node = ofnode_get_parent(server_node);
	if (!ofnode_valid(parent_node))
		return NULL;
	ret = ofnode_read_u32(server_node, "server-id", server_id);
	if (ret)
		return NULL;
	ret = uclass_get_device_by_ofnode(UCLASS_NOP, parent_node, &router_dev);
	if (ret) {
		dev_err(dev, "mailbox router unavailable: %d\n", ret);
		return NULL;
	}
	return dev_get_priv(router_dev);
}

int quard_mbox_consumer_register(struct quard_mbox_consumer *consumer,
				 quard_mbox_recv_func_t recv_cb)
{
	struct quard_mbox_router *router;
	u32 server_id;

	router = of_parse_consumer_server(consumer->dev, &server_id);
	if (!router)
		return -EPROBE_DEFER;
	if (server_id >= (1U << 8))
		return -EINVAL;
	if (find_consumer_by_id(router, server_id))
		return -EBUSY;

	consumer->server_id = server_id;
	consumer->router = router;
	snprintf(consumer->name, sizeof(consumer->name), "quard_router_%u",
		 server_id);
	consumer->recv_cb = recv_cb;
	INIT_LIST_HEAD(&consumer->consumer_node);
	list_add_tail(&consumer->consumer_node, &router->consumer_list);
	return 0;
}

void quard_mbox_consumer_unregister(struct quard_mbox_consumer *consumer)
{
	list_del(&consumer->consumer_node);
}

int quard_mbox_receive(struct quard_mbox_consumer *consumer,
		       struct quard_consumer_msg *msg)
{
	struct quard_mbox_router *router = consumer->router;
	int ret;

	ret = mbox_recv(&router->rx, msg, QUARD_MBOX_RX_TIMEOUT_US);
	if (!ret && consumer->recv_cb)
		consumer->recv_cb(consumer->dev, msg);
	return ret;
}

static int router_mb_init(struct udevice *dev)
{
	struct quard_mbox_router *router = dev_get_priv(dev);
	int ret;

	ret = mbox_get_by_name(dev, "router-tx", &router->tx);
	if (ret)
		return ret;
	ret = mbox_get_by_name(dev, "router-rx", &router->rx);
	if (ret) {
		mbox_free(&router->tx);
		return ret;
	}
	return 0;
}

static int quard_mbox_router_probe(struct udevice *dev)
{
	struct quard_mbox_router *router = dev_get_priv(dev);
	int ret;

	router->dev = dev;
	INIT_LIST_HEAD(&router->router_node);
	INIT_LIST_HEAD(&router->consumer_list);
	ret = router_mb_init(dev);
	if (ret)
		return ret;
	list_add_tail(&router->router_node, &router_list);
	dev_info(dev, "mailbox router ready\n");
	return 0;
}

static const struct udevice_id quard_mbox_router_of_match[] = {
	{ .compatible = "quard,mbox-router" },
	{ }
};

U_BOOT_DRIVER(quard_mbox_router) = {
	.name = "quard-mbox-router",
	.id = UCLASS_NOP,
	.of_match = quard_mbox_router_of_match,
	.probe = quard_mbox_router_probe,
	.priv_auto = sizeof(struct quard_mbox_router),
};
