#ifndef __QUARD_MBOX_ROUTER_PROTOCOL_H__
#define __QUARD_MBOX_ROUTER_PROTOCOL_H__

#include "quard_router_server_id.h"

#define MSG_ITEM_SIZE    sizeof(struct quard_mbox_msg)
struct quard_mbox_msg {
	union {
		uint32_t value;
		struct {
			/* crc16 of all msg (16 bits) */
			uint32_t crc16  : 16;
			/* server type (8 bits) */
			uint32_t quard_server_id  : 8;
			 /* Command type (8 bits) */
			uint32_t quard_server_cmd  : 8;
		} fields;
	} header;
	uint32_t params[6];
};

#endif