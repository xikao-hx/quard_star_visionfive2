#define LOG_TAG "MAB_ROUTER"
#include "elog.h"
#include <errno.h>
#include <stdio.h>
#include "ipi_mailbox.h"
#include "quard_mbox_router_consumer.h"
#include "quard_mbox_router.h"
#include "router_config.h"
#include "quard_errno.h"

static void consumer_mbox_rx_task(void *param)
{
	struct quard_mbox_consumer *consumer = param;
	struct quard_mbox_consumer_cfg *cfg = &consumer->cfg;
	struct quard_consumer_msg msg = { 0 };
	LOG_I("router server %s started", consumer->name);

	while (1) {
		if (xQueueReceive(consumer->rx_queue, &msg,
				  (portTickType)portMAX_DELAY) != pdPASS)
			continue;

		if (cfg->recv_cb != NULL)
			cfg->recv_cb(&msg);
	}
}

int quard_router_send_msg(struct quard_mbox_consumer *consumer,
						struct quard_consumer_msg *user_msg)
{
	int ret;
	struct quard_mbox_consumer_cfg *cfg = &consumer->cfg;
	struct quard_mbox_msg *msg = (struct quard_mbox_msg *)user_msg;

	msg->header.fields.quard_server_id = cfg->server_id;
	ret = mbox_send_message(consumer->router->tx_chan, (void*)msg);
	return ret;
}

int quard_router_send_msg_shell(struct quard_mbox_consumer *consumer,
							struct quard_consumer_msg *user_msg)
{
	int ret;
	struct quard_mbox_consumer_cfg *cfg = &consumer->cfg;
	struct quard_mbox_msg *msg = (struct quard_mbox_msg *)user_msg;

	msg->header.fields.quard_server_id = cfg->server_id;
	ret = mbox_send_message(consumer->router->tx_chan, (void *)msg);
	return ret;
}

int quard_mbox_consumer_register(struct quard_mbox_consumer *consumer)
{
	int ret = 0;
	int ret_snprintf = 0;
	int router_id;
	struct quard_mbox_consumer_cfg *cfg = &consumer->cfg;
	struct quard_mbox_router *router;

	if (cfg->server_id >= QUARD_SERVER_ID_MAX) {
		LOG_E("invalid server id %d\n", cfg->server_id);
		return -EINVAL;
	}

	router_id = server_to_router_map[cfg->server_id];
	if (router_id >= TOTAL_ROUTER_NUM) {
		LOG_E("invalid router id %d\n", router_id);
		return -EINVAL;
	}

	router = router_tab[router_id];

	ret_snprintf = snprintf(consumer->name, CONSUMER_NAME_MAX_LEN,
			"mbox_c%d", cfg->server_id);
	if (ret_snprintf <= 1) {
		LOG_E("snprintf fail %d\n", ret_snprintf);
	}

	consumer->rx_queue = xQueueCreateStatic(cfg->rx_queue_item_num,
											MSG_ITEM_SIZE,
											(uint8_t *)cfg->rx_queue_buf,
											&consumer->rx_static_queue);

	if (NULL == xTaskCreateStatic(consumer_mbox_rx_task,
									consumer->name,
									cfg->stack_depth,
									(void *)consumer,
									cfg->thread_priority,
									cfg->thread_stack,
									&consumer->rx_static_task)) {
		LOG_E("consumer %s task created failed");
		return -EINVAL;
	}

	/* check repeat register */
	taskENTER_CRITICAL();
	if (router->consumer[cfg->server_id] == NULL) {
		router->consumer[cfg->server_id] = consumer;
	} else {
		LOG_E("server id %d already registered!", cfg->server_id);
		ret = -EBUSY;
	}
	taskEXIT_CRITICAL();
	consumer->router = router;

	return ret;
}

static struct quard_mbox_consumer *find_consumer_by_id(uint8_t server_id)
{
	int i;
	struct quard_mbox_router *router = NULL;
	struct quard_mbox_consumer *consumer = NULL;

	for (i = 0; i < TOTAL_ROUTER_NUM; i ++) {
		router = router_tab[i];
		consumer = router->consumer[server_id];
		if (consumer != NULL)
			return consumer;
	}

	return NULL;
}


static void router_rx_notify(struct mbox_client *cl, void *msg_raw)
{
	struct quard_mbox_consumer *consumer;
	uint8_t server_id;
	BaseType_t ret;
	struct quard_mbox_msg *msg = msg_raw;

	ARG_UNUSED(cl);
	server_id = (uint8_t)(msg->header.fields.quard_server_id);
	if (server_id >= QUARD_SERVER_ID_MAX) {
		LOG_E("invalid msg: server id %d", server_id);
		return ;
	}

	consumer = find_consumer_by_id(server_id);
	if (consumer == NULL) {
		LOG_E("invalid msg: server %d not registered", server_id);
		return ;
	}

	ret = xQueueSendToBackFromISR(consumer->rx_queue, msg, NULL);
	if	(ret != pdPASS) {
		LOG_E("send msg to server queue %d failed", server_id);
		return;
	}

	/*
	 * Do not call portYIELD_FROM_ISR() here.  The RISC-V SSIP trap path
	 * invokes vTaskSwitchContext() after vPortClearIpiInterrupt() returns.
	 * Yielding here would schedule twice during one mailbox interrupt and can
	 * restore an inconsistent task context before the mailbox ACK completes.
	 */
}

static void router_tx_empty_notify(struct mbox_client *cl, void *data,
									int empty_value)
{
	ARG_UNUSED(cl);
	ARG_UNUSED(data);
	ARG_UNUSED(empty_value);
}

static void init_quard_mbox(struct quard_mbox_router *router)
{
	struct mbox_client tx_cl = { 0 };
	struct mbox_client rx_cl = { 0 };
	const struct router_cfg *cfg = &router->cfg;

	quard_mailbox_ipi_controller_init((enum mbox_type)cfg->mbox_type);

	tx_cl.dir = SEND_TYPE;
	tx_cl.idx = cfg->tx_mbox_id;
	tx_cl.src = cfg->local_chan_id;
	tx_cl.dst = cfg->remote_chan_id;
	tx_cl.tx_done = router_tx_empty_notify;
	router->tx_chan = mbox_request_channel((enum mbox_type)cfg->mbox_type, &tx_cl);

	rx_cl.dir = RECV_TYPE;
	rx_cl.idx = cfg->rx_mbox_id;
	rx_cl.src = cfg->remote_chan_id;
	rx_cl.dst = cfg->local_chan_id;
	rx_cl.rx_callback = router_rx_notify;
	router->rx_chan = mbox_request_channel((enum mbox_type)cfg->mbox_type, &rx_cl);

	if ((router->tx_chan == NULL) || (router->rx_chan == NULL)) {
		LOG_E("% fail", __func__);
	} 
}

void init_quard_mbox_router(void)
{
	int i;

	for (i = 0; i < TOTAL_ROUTER_NUM; i ++)
		init_quard_mbox(router_tab[i]);
}
