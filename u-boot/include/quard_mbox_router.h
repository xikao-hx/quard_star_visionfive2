/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __QUARD_MBOX_ROUTER_H__
#define __QUARD_MBOX_ROUTER_H__

#include <linux/list.h>
#include <linux/types.h>

struct udevice;

#define USER_PARAM_NUM 6
#define CONSUMER_NAME_MAX_LEN 20

struct quard_consumer_msg {
	u8 mbox_router_reserved[3];
	u8 cmd;
	u32 params[USER_PARAM_NUM];
};

typedef void (*quard_mbox_recv_func_t)(struct udevice *dev,
				       struct quard_consumer_msg *msg);

struct quard_mbox_router;

struct quard_mbox_consumer {
	struct udevice *dev;
	char name[CONSUMER_NAME_MAX_LEN];
	u32 server_id;
	quard_mbox_recv_func_t recv_cb;
	struct list_head consumer_node;
	struct quard_mbox_router *router;
};

int quard_mbox_consumer_register(struct quard_mbox_consumer *consumer,
				 quard_mbox_recv_func_t recv_cb);
void quard_mbox_consumer_unregister(struct quard_mbox_consumer *consumer);
int quard_mbox_send_msg(struct quard_mbox_consumer *consumer,
			struct quard_consumer_msg *msg);
int quard_mbox_receive(struct quard_mbox_consumer *consumer,
		       struct quard_consumer_msg *msg);

#endif
