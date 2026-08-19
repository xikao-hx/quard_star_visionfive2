#ifndef _ROUTER_CONFIG_H_
#define _ROUTER_CONFIG_H_

#include "ipi_mailbox.h"
#include "quard_mbox_router.h"
#include "router_config.h"

enum {
	MBOX_ROUTER_0 = 0,
	TOTAL_ROUTER_NUM,
};

struct quard_mbox_router mbox_router_0 = {
	.cfg = {
		.mbox_type = EXTER0_MBOX,
		/* mailbox-test: Linux -> RTOS uses channel 1, RTOS -> Linux channel 0. */
		.tx_mbox_id = 0,
		.rx_mbox_id = 1,
		.local_chan_id = 11,
		.remote_chan_id = 4,
	},
};

static const int server_to_router_map[QUARD_SERVER_ID_MAX] = 
{
	[QUARD_SERVER_ID_LOG] = MBOX_ROUTER_0,
	[QUARD_SERVER_ID_VSHELL] = MBOX_ROUTER_0,
	[QUARD_SERVER_ID_NOR] = MBOX_ROUTER_0,
};


static struct quard_mbox_router *router_tab[TOTAL_ROUTER_NUM] = {
	[MBOX_ROUTER_0] = &mbox_router_0,
};

#endif
