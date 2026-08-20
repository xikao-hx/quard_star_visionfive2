#ifndef __QUARD_MBOX_ROUTER_H__
#define __QUARD_MBOX_ROUTER_H__

#include <linux/types.h>

#define USER_PARAM_NUM 6

struct quard_consumer_msg {
	uint8_t mbox_router_reserved[3];
	uint8_t cmd;
	uint32_t params[USER_PARAM_NUM];
};

struct quard_mbox_consumer;
typedef void (*quard_mbox_recv_func_t)(struct device *dev,
				    struct quard_consumer_msg *msg);

struct quard_mbox_consumer *quard_mbox_consumer_register(struct device *dev,
						   quard_mbox_recv_func_t recv_cb);

void quard_mbox_consumer_unregister(struct quard_mbox_consumer *consumer);
int quard_mbox_send_msg(struct quard_mbox_consumer *consumer,
		     struct quard_consumer_msg *msg);
			 
#endif
