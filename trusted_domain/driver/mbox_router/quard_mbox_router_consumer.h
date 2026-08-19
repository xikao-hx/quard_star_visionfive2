#ifndef _QUARD_MBOX_ROUTER_CONSUMER_H_
#define _QUARD_MBOX_ROUTER_CONSUMER_H_

#include <stdint.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "queue.h"
#include "../../../common_inc/bsp/quard_router_protocol.h"

#define CONSUMER_NAME_MAX_LEN 8

#define USER_PARAM_NUM 6
struct quard_consumer_msg {
	uint8_t mbox_router_reserved[3];
	uint8_t cmd;
	uint32_t params[USER_PARAM_NUM];
};

#define MSG_TIME_SIZE  (sizeof(struct quard_consumer_msg))
typedef void (*quard_mbox_recv_func_t)(struct quard_consumer_msg *msg);

struct quard_mbox_consumer_cfg {
	uint8_t 		         server_id;
	quard_mbox_recv_func_t      recv_cb;
	StackType_t              *thread_stack;
	uint32_t                 stack_depth;
	UBaseType_t              thread_priority;
	uint32_t                 rx_queue_item_num;
	struct quard_consumer_msg   *rx_queue_buf;
};

struct quard_mbox_router;
struct quard_mbox_consumer {
	struct quard_mbox_consumer_cfg         cfg;
	char                                name[CONSUMER_NAME_MAX_LEN];
	StaticTask_t                        rx_static_task;
	StaticQueue_t                       rx_static_queue;
	QueueHandle_t                       rx_queue;
	struct quard_mbox_router               *router;
};

int quard_mbox_consumer_register(struct quard_mbox_consumer *consumer);
int quard_router_send_msg(struct quard_mbox_consumer * consumer, 
						struct quard_consumer_msg * user_msg);
int quard_router_send_msg_shell(struct quard_mbox_consumer * consumer, 
						struct quard_consumer_msg * user_msg);

#endif


