// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 StarFive Technology Co., Ltd.
 */

#include <asm/irq.h>
#include <asm/sbi.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <quard_ipi_mailbox_protocol.h>

#define IPI_MBOX_EVENT_MASK	(BIT(IPI_MBOX_TYPE_RX) | \
				 BIT(IPI_MBOX_TYPE_TX))

struct quard_ipi_mbox;

struct quard_ipi_mbox_chan {
	struct quard_ipi_mbox	*ipi_mbox;
	unsigned int		type;
	uint8_t			*data;
	uint32_t		*state;
	uint32_t		rtos_hart_id;
};

struct quard_ipi_mbox {
	struct device		*dev;
	struct mbox_controller	mbox;
	struct mbox_chan	chans[IPI_MBOX_CHANS];
	struct quard_ipi_mbox_chan mb[IPI_MBOX_CHANS];

	void *base;
	spinlock_t tx_lock;
};

static uint8_t *ipi_mbox_base_addr(void *base, unsigned int idx)
{
	return (uint8_t *)base + idx * IPI_MBOX_CHAN_STRIDE;
}

static uint32_t ipi_mbox_get_state(uint32_t *state)
{
	/* Read the state before consuming payload written by the remote hart. */
	return smp_load_acquire(state);
}

static void ipi_mbox_set_state(uint32_t *state, uint32_t value)
{
	/* Publish payload updates before making the new state visible. */
	smp_store_release(state, value);
}

static void ipi_mbox_process_rx(struct quard_ipi_mbox *ipi_mbox)
{
	struct quard_ipi_mbox_chan *rx = &ipi_mbox->mb[IPI_MBOX_TYPE_RX];
	struct mbox_chan *chan = &ipi_mbox->chans[IPI_MBOX_TYPE_RX];
	uint8_t message[IPI_MBOX_PAYLOAD_SIZE];

	if (!READ_ONCE(chan->cl) ||
	    ipi_mbox_get_state(rx->state) != IPI_MBOX_STATE_DATA_READY)
		return;

	memcpy(message, rx->data, sizeof(message));
	memset(rx->data, 0, sizeof(message));

	mbox_chan_received_data(chan, message);
	ipi_mbox_set_state(rx->state, IPI_MBOX_STATE_ACK);
	(void)sbi_send_ipi_amp(rx->rtos_hart_id, IPI_MBOX_TYPE_RX);
}

static void ipi_mbox_process_tx_done(struct quard_ipi_mbox *ipi_mbox)
{
	struct quard_ipi_mbox_chan *tx = &ipi_mbox->mb[IPI_MBOX_TYPE_TX];
	struct mbox_chan *chan = &ipi_mbox->chans[IPI_MBOX_TYPE_TX];
	unsigned long flags;
	bool completed = false;

	spin_lock_irqsave(&ipi_mbox->tx_lock, flags);
	if (READ_ONCE(chan->cl) &&
	    ipi_mbox_get_state(tx->state) == IPI_MBOX_STATE_ACK) {
		ipi_mbox_set_state(tx->state, IPI_MBOX_STATE_IDLE);
		completed = true;
	}
	spin_unlock_irqrestore(&ipi_mbox->tx_lock, flags);

	if (completed)
		mbox_chan_txdone(chan, 0);
}

static void ipi_mbox_isr(unsigned long msg_type, void *data)
{
	struct quard_ipi_mbox *ipi_mbox = data;

	msg_type &= IPI_MBOX_EVENT_MASK;
	if (msg_type & BIT(IPI_MBOX_TYPE_RX))
		ipi_mbox_process_rx(ipi_mbox);
	if (msg_type & BIT(IPI_MBOX_TYPE_TX))
		ipi_mbox_process_tx_done(ipi_mbox);
}

static int ipi_mbox_send_data(struct mbox_chan *chan, void *data)
{
	struct quard_ipi_mbox_chan *mb = chan->con_priv;
	struct quard_ipi_mbox *ipi_mbox;
	unsigned long flags;
	uint32_t state;
	int ret;

	if (!mb || !mb->ipi_mbox)
		return -ENODEV;
	if (!data)
		return -EINVAL;
	ipi_mbox = mb->ipi_mbox;

	if (mb->type != IPI_MBOX_TYPE_TX)
		return -EINVAL;

	spin_lock_irqsave(&ipi_mbox->tx_lock, flags);
	state = ipi_mbox_get_state(mb->state);
	/* A timed-out request may have been consumed after the mailbox core
	 * dropped it.  A new send is only submitted when no request is active,
	 * so an ACK observed here is stale and can safely be retired.
	 */
	if (state == IPI_MBOX_STATE_ACK) {
		ipi_mbox_set_state(mb->state, IPI_MBOX_STATE_IDLE);
		state = IPI_MBOX_STATE_IDLE;
	}
	if (state != IPI_MBOX_STATE_IDLE) {
		spin_unlock_irqrestore(&ipi_mbox->tx_lock, flags);
		return -EBUSY;
	}

	memcpy(mb->data, data, IPI_MBOX_PAYLOAD_SIZE);
	ipi_mbox_set_state(mb->state, IPI_MBOX_STATE_DATA_READY);
	spin_unlock_irqrestore(&ipi_mbox->tx_lock, flags);

	ret = sbi_send_ipi_amp(mb->rtos_hart_id, IPI_MBOX_TYPE_TX);
	if (ret) {
		spin_lock_irqsave(&ipi_mbox->tx_lock, flags);
		if (ipi_mbox_get_state(mb->state) == IPI_MBOX_STATE_DATA_READY)
			ipi_mbox_set_state(mb->state, IPI_MBOX_STATE_IDLE);
		spin_unlock_irqrestore(&ipi_mbox->tx_lock, flags);
	}
	return ret;
}

static int ipi_mbox_startup(struct mbox_chan *chan)
{
	struct quard_ipi_mbox_chan *mb = chan->con_priv;
	struct quard_ipi_mbox *ipi_mbox = mb->ipi_mbox;

	if (mb->type == IPI_MBOX_TYPE_RX)
		ipi_mbox_process_rx(ipi_mbox);
	else
		ipi_mbox_process_tx_done(ipi_mbox);
	return 0;
}

static const struct mbox_chan_ops ipi_mbox_ops = {
	.send_data = ipi_mbox_send_data,
	.startup = ipi_mbox_startup,
};

static struct mbox_chan *ipi_mbox_xlate(struct mbox_controller *mbox,
				      const struct of_phandle_args *args)
{
	uint32_t type, idx;

	if (args->args_count != 2) {
		dev_err(mbox->dev, "invalid argument count %d\n", args->args_count);
		return ERR_PTR(-EINVAL);
	}

	type = args->args[0];
	idx = args->args[1];

	if (type > IPI_MBOX_TYPE_TX || idx >= mbox->num_chans || type != idx) {
		dev_err(mbox->dev, "unsupported mailbox channel: type=%u idx=%u\n", type, idx);
		return ERR_PTR(-EINVAL);
	}

	return &mbox->chans[idx];
}

static int ipi_mbox_init_mem_region(struct quard_ipi_mbox *ipi_mbox,
				  struct platform_device *pdev)
{
	struct resource *res;
	resource_size_t mem_size;
	unsigned int i;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	if (resource_size(res) < IPI_MBOX_REQUIRED_SIZE)
		return -EINVAL;

	mem_size = resource_size(res);
	ipi_mbox->base = devm_memremap(ipi_mbox->dev, res->start,
					 mem_size, MEMREMAP_WB);

	if (IS_ERR(ipi_mbox->base))
		return PTR_ERR(ipi_mbox->base);

	for (i = 0; i < IPI_MBOX_CHANS; i++) {
		uint8_t *channel = ipi_mbox_base_addr(ipi_mbox->base, i);

		ipi_mbox->mb[i].data = channel;
		ipi_mbox->mb[i].state = (uint32_t *)(channel + IPI_MBOX_STATE_OFFSET);
	}
	/* FreeRTOS owns initialisation; preserve messages posted before probe. */

	return 0;
}

static int quard_ipi_mbox_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct quard_ipi_mbox *ipi_mbox;
	unsigned int i;
	uint32_t rtos_hart_id;
	int ret;

	ipi_mbox = devm_kzalloc(dev, sizeof(*ipi_mbox), GFP_KERNEL);
	if (!ipi_mbox)
		return -ENOMEM;

	if (of_property_read_u32(np, "rtos-hart-id", &rtos_hart_id))
		return -EINVAL;

	ipi_mbox->dev = dev;
	spin_lock_init(&ipi_mbox->tx_lock);

	ipi_mbox->mbox.dev = dev;
	ipi_mbox->mbox.ops = &ipi_mbox_ops;
	ipi_mbox->mbox.chans = ipi_mbox->chans;
	ipi_mbox->mbox.num_chans = IPI_MBOX_CHANS;
	ipi_mbox->mbox.of_xlate = ipi_mbox_xlate;
	ipi_mbox->mbox.txdone_irq = true;
	ipi_mbox->mbox.txdone_poll = false;

	for (i = 0; i < IPI_MBOX_CHANS; i++) {
		ipi_mbox->mb[i].ipi_mbox = ipi_mbox;
		ipi_mbox->mb[i].type = i;
		ipi_mbox->mb[i].rtos_hart_id = rtos_hart_id;
		ipi_mbox->chans[i].con_priv = &ipi_mbox->mb[i];
	}

	platform_set_drvdata(pdev, ipi_mbox);

	ret = ipi_mbox_init_mem_region(ipi_mbox, pdev);
	if (ret)
		return ret;

	ret = devm_mbox_controller_register(dev, &ipi_mbox->mbox);
	if (ret)
		return ret;

	ret = register_ipi_mailbox_handler(ipi_mbox_isr, ipi_mbox);
	if (ret)
		return ret;

	dev_info(dev, "IPI mailbox ready: txdone=irq\n");

	return 0;
}

static int quard_ipi_mbox_remove(struct platform_device *pdev)
{
	struct quard_ipi_mbox *ipi_mbox = platform_get_drvdata(pdev);

	unregister_ipi_mailbox_handler(ipi_mbox);
	return 0;
}

static const struct of_device_id ipi_amp_of_match[] = {
	{ .compatible = "starfive,ipi-amp-mailbox", .data = NULL },
	{},
};
MODULE_DEVICE_TABLE(of, ipi_amp_of_match);

static struct platform_driver quard_ipi_mbox_driver = {
	.probe = quard_ipi_mbox_probe,
	.remove = quard_ipi_mbox_remove,
	.driver = {
		.name = "starfive-ipi-mailbox",
		.of_match_table = ipi_amp_of_match,
	},
};
module_platform_driver(quard_ipi_mbox_driver);
MODULE_LICENSE("GPL");
