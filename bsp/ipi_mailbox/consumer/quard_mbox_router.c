#include <linux/types.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/uaccess.h>
#include <linux/mailbox_client.h>
#include <linux/kallsyms.h>
#include "quard_mbox_router.h"
#include <quard_router_protocol.h>

#define TX_TIMEOUT		10000

/* Data type for mailbox client and channel details */
struct quard_mb_chan {
	struct mbox_client client;
	struct mbox_chan *chan;
};

struct quard_mailbox {
	struct quard_mb_chan rx;
	struct quard_mb_chan tx;
};

struct quard_mbox_work_data {
	struct quard_mbox_consumer *consumer;
	struct work_struct work;
	struct quard_consumer_msg msg;
};

#define CONSUMER_NAME_MAX_LEN		20

struct quard_mbox_router {
	struct device		*dev;
	struct mutex		tx_lock;
	struct quard_mailbox	mailbox;
	struct list_head	router_node;
	struct list_head	consumer_list;
	spinlock_t		consumer_list_lock;
};

struct quard_mbox_consumer {
	struct device			*dev;
	char				name[CONSUMER_NAME_MAX_LEN];
	uint32_t			server_id;
	struct list_head		consumer_node;
	struct workqueue_struct 	*msg_wq;
	quard_mbox_recv_func_t		recv_cb;
	struct quard_mbox_router		*router;
};

static LIST_HEAD(router_list);

static struct quard_mbox_consumer *find_consumer_by_id(struct quard_mbox_router *router,
						    uint32_t server_id)
{
	unsigned long flags;
	int find = 0;
	struct quard_mbox_consumer *consumer = NULL;

	spin_lock_irqsave(&router->consumer_list_lock, flags);
	list_for_each_entry(consumer, &router->consumer_list, consumer_node) {
		if (consumer->server_id == server_id) {
			find = 1;
			break;
		}
	}
	spin_unlock_irqrestore(&router->consumer_list_lock, flags);

	return (find ? consumer : NULL);
}

int quard_mbox_send_msg(struct quard_mbox_consumer *consumer, struct quard_consumer_msg *msg)
{
	int ret;
	struct quard_mbox_msg *quard_msg = (struct quard_mbox_msg *)msg;
	struct quard_mbox_router *router = consumer->router;
	struct quard_mailbox *mailbox = &router->mailbox;

	/* TODO: add crc16 calculate */
	quard_msg->header.fields.crc16 = 0;
	quard_msg->header.fields.quard_server_id = (uint8_t)consumer->server_id;
	mutex_lock(&router->tx_lock);
	ret = mbox_send_message(mailbox->tx.chan, (void *)quard_msg);
	mutex_unlock(&router->tx_lock);

	return ((ret >= 0) ? 0 : ret);
}
EXPORT_SYMBOL_GPL(quard_mbox_send_msg);

static struct quard_mbox_router *of_parse_consumer_server(struct device *dev,
							uint32_t *server_id)
{
	int ret;
	int find = 0;
	struct device_node *server_node;
	struct quard_mbox_router *router = NULL;

	server_node = of_parse_phandle(dev->of_node, "mbox-router-server", 0);
	if (!server_node) {
		dev_err(dev, "no mbox-router-server specified in dts!\n");
		return NULL;
	}

	ret = of_property_read_u32(server_node, "server-id", server_id);
	if (ret) {
		dev_err(dev, "no reg specified!\n");
		return NULL;
	}

	list_for_each_entry(router, &router_list, router_node) {
		if (router->dev->of_node == server_node->parent) {
			find = 1;
			break;
		}
	}

	if (!find) {
		dev_err(dev, "router might not registered!\n");
		return NULL;
	}

	return router;
}

struct quard_mbox_consumer *quard_mbox_consumer_register(struct device *dev,
						   quard_mbox_recv_func_t recv_cb)
{
	unsigned long flags;
	struct quard_mbox_router *router = NULL;
	struct quard_mbox_consumer *consumer = NULL;
	uint32_t server_id;

	router = of_parse_consumer_server(dev, &server_id);
	if (!router) {
		dev_err(dev, "cannot find server router!\n");
		return ERR_PTR(-EPROBE_DEFER);
	}

	if (server_id >= (1 << 8)) {
		dev_err(dev, "server id max is 255!\n");
		return ERR_PTR(-EINVAL);
	}

	if (find_consumer_by_id(router, server_id)) {
		dev_err(dev, "server already registered!\n");
		return ERR_PTR(-EBUSY);
	}

	consumer = devm_kzalloc(router->dev, sizeof(struct quard_mbox_consumer),
				GFP_KERNEL);
	if (!consumer) {
		dev_err(dev, "alloc consumer failed!\n");
		return ERR_PTR(-ENOMEM);
	}

	consumer->server_id = server_id;
	consumer->dev = dev;
	consumer->router = router;
	snprintf(consumer->name, CONSUMER_NAME_MAX_LEN, "quard_router_%d", server_id);
	consumer->recv_cb = recv_cb;
	consumer->msg_wq = create_singlethread_workqueue(consumer->name);
	if (!consumer->msg_wq) {
		dev_err(dev, "create_singlethread_workqueue failed!\n");
		devm_kfree(router->dev, consumer);
		return ERR_PTR(-ENOMEM);
	}

	spin_lock_irqsave(&router->consumer_list_lock, flags);
	list_add_tail(&consumer->consumer_node, &router->consumer_list);
	spin_unlock_irqrestore(&router->consumer_list_lock, flags);

	return consumer;
}
EXPORT_SYMBOL_GPL(quard_mbox_consumer_register);

void quard_mbox_consumer_unregister(struct quard_mbox_consumer *consumer)
{
	unsigned long flags;
	struct quard_mbox_router *router;

	router = consumer->router;
	destroy_workqueue(consumer->msg_wq);
	spin_lock_irqsave(&router->consumer_list_lock, flags);
	list_del(&consumer->consumer_node);
	spin_unlock_irqrestore(&router->consumer_list_lock, flags);
	devm_kfree(router->dev, consumer);
}
EXPORT_SYMBOL_GPL(quard_mbox_consumer_unregister);

static void consumer_work_handle(struct work_struct *work)
{
	struct quard_mbox_work_data *quard_mbox_work;
	struct quard_mbox_consumer *consumer;

	quard_mbox_work = container_of(work, struct quard_mbox_work_data, work);
	consumer = quard_mbox_work->consumer;
	consumer->recv_cb(consumer->dev, &quard_mbox_work->msg);
	devm_kfree(consumer->dev, quard_mbox_work);
}

static void quard_mbox_receive(struct mbox_client *cl, void *msg)
{
	struct device *dev = cl->dev;
	struct quard_mbox_router *router = dev_get_drvdata(dev);
	struct quard_mbox_msg *quard_msg = msg;
	struct quard_mbox_consumer *consumer = NULL;
	struct quard_mbox_work_data *quard_mbox_work;
	struct quard_consumer_msg *user_msg;

	/* TODO: add crc verify */
	consumer = find_consumer_by_id(router, quard_msg->header.fields.quard_server_id);
	if (!consumer) {
		dev_err_ratelimited(dev,
			"unknown mailbox route: server=%u cmd=%u crc=0x%x payload=%*phN\n",
			quard_msg->header.fields.quard_server_id,
			quard_msg->header.fields.quard_server_cmd,
			quard_msg->header.fields.crc16,
			(int)sizeof(quard_msg->params), quard_msg->params);
		return;
	}

	quard_mbox_work = devm_kzalloc(consumer->dev, sizeof(struct quard_mbox_work_data),
				    GFP_ATOMIC);
	if (!quard_mbox_work) {
		dev_err_ratelimited(dev, "mailbox RX work allocation failed\n");
		return;
	}
	quard_mbox_work->consumer = consumer;
	user_msg = &quard_mbox_work->msg;
	user_msg->cmd = quard_msg->header.fields.quard_server_cmd;
	memcpy(user_msg->params, quard_msg->params, sizeof(uint32_t) * USER_PARAM_NUM);
	INIT_WORK(&quard_mbox_work->work, consumer_work_handle);
	queue_work(consumer->msg_wq, &quard_mbox_work->work);
}

static void mb_tx_empty_notify(struct mbox_client *cl,
			       void *data, int empty_value)
{
}

static int router_mb_init(struct device *dev)
{
	int err;
	struct quard_mbox_router *router = dev_get_drvdata(dev);
	struct quard_mailbox *mailbox = &router->mailbox;

	mailbox->tx.client.dev = dev;
	mailbox->rx.client.dev = dev;
	mailbox->tx.client.tx_block = true;
	mailbox->tx.client.tx_tout = TX_TIMEOUT;
	mailbox->rx.client.rx_callback = quard_mbox_receive;
	mailbox->tx.client.tx_done = mb_tx_empty_notify;

	mailbox->tx.chan = mbox_request_channel_byname(&mailbox->tx.client,
						       "router-tx");
	if (IS_ERR(mailbox->tx.chan)) {
		err = PTR_ERR(mailbox->tx.chan);
		dev_err(dev, "failed to get tx mailbox: %d\n", err);
		return err;
	}

	mailbox->rx.chan = mbox_request_channel_byname(&mailbox->rx.client,
						       "router-rx");
	if (IS_ERR(mailbox->rx.chan)) {
		err = PTR_ERR(mailbox->rx.chan);
		dev_err(dev, "failed to get rx mailbox: %d\n", err);
		return err;
	}

	return 0;
}

static int quard_mbox_router_probe(struct platform_device *pdev)
{
	int ret;
	struct device *dev = &pdev->dev;
	struct quard_mbox_router *router;

	router = devm_kzalloc(dev, sizeof(struct quard_mbox_router), GFP_KERNEL);
	if (!router) {
		dev_err(dev, "failed to allocate memory for %s\n", dev_name(dev));
		return -ENOMEM;
	}

	router->dev = dev;
	mutex_init(&router->tx_lock);
	dev_set_drvdata(dev, router);
	INIT_LIST_HEAD(&router->consumer_list);
	spin_lock_init(&router->consumer_list_lock);
	ret = router_mb_init(dev);
	if (ret)
		return ret;
	list_add_tail(&router->router_node, &router_list);
	dev_info(dev, "mailbox router ready: tx_timeout=%dms\n", TX_TIMEOUT);

	return 0;
}

static int quard_mbox_router_remove(struct platform_device *pdev)
{
	struct quard_mbox_router *router;
	struct quard_mailbox *mailbox;

	router = dev_get_drvdata(&pdev->dev);
	mailbox = &router->mailbox;
	mutex_destroy(&router->tx_lock);
	mbox_free_channel(mailbox->tx.chan);
	mbox_free_channel(mailbox->rx.chan);
	list_del(&router->router_node);

	return 0;
}

static const struct of_device_id quard_mbox_router_of_match[] = {
	{.compatible = "quard,mbox-router", },
	{},
};
MODULE_DEVICE_TABLE(of, quard_mbox_router_of_match);

static struct platform_driver quard_mbox_router_driver = {
	.probe     = quard_mbox_router_probe,
	.remove    = quard_mbox_router_remove,
	.driver    = {
			.name = "quard-mbox-router",
			.owner = THIS_MODULE,
			.of_match_table = of_match_ptr(quard_mbox_router_of_match),
		},
};

static int __init quard_mbox_router_init(void)
{
	int ret;

	ret = platform_driver_register(&quard_mbox_router_driver);
	if (ret)
		pr_warn("quard_mbox_router driver not registered\n");

	return ret;
}

static void __exit quard_mbox_router_exit(void)
{
	platform_driver_unregister(&quard_mbox_router_driver);
}

module_init(quard_mbox_router_init);
module_exit(quard_mbox_router_exit);

MODULE_AUTHOR("Quard Star Team");
MODULE_DESCRIPTION("mbox router driver");
MODULE_VERSION("V0.1");
MODULE_LICENSE("GPL v2");
