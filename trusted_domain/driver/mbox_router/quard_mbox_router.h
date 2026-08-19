#ifndef _QUARD_MBOX_ROUTER_H_
#define _QUARD_MBOX_ROUTER_H_

#include <stdint.h>
#include <string.h>
#include "task.h"

#include "quard_mbox_router_consumer.h"

struct router_cfg {
	uint8_t mbox_type;
	uint8_t tx_mbox_id;
	uint8_t rx_mbox_id;
	uint8_t local_chan_id;
	uint8_t remote_chan_id;
};

struct quard_mbox_router {
	const struct router_cfg cfg;
	struct mbox_chan *tx_chan;
	struct mbox_chan *rx_chan;
	struct quard_mbox_consumer *consumer[QUARD_SERVER_ID_MAX];
};

void init_quard_mbox_router(void);

#endif
