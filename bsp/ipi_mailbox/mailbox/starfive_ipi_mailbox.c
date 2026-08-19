// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 StarFive Technology Co., Ltd.
 */

#include <asm/irq.h>
#include <asm/sbi.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/bitfield.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/suspend.h>
#include <linux/slab.h>
#include <linux/atomic.h>
#include <linux/ratelimit.h>
#include <linux/workqueue.h>

#include "mailbox.h"

#define IPI_MB_CHANS		2
#define IPI_MB_PAYLOAD_SIZE	28
#define IPI_MB_STATE_OFFSET	28
#define IPI_MB_CHAN_STRIDE	32

#define IPI_MB_STATE_IDLE	0
#define IPI_MB_STATE_DATA_READY	1
#define IPI_MB_STATE_ACK	2
#define IPI_MB_RECOVERY_MS	20

/* Please not change TX & RX */
enum ipi_mb_chan_type {
	IPI_MB_TYPE_RX		= 0, /* FreeRTOS TX -> Linux RX */
	IPI_MB_TYPE_TX		= 1, /* Linux TX -> FreeRTOS RX */
};

struct ipi_mb_priv;

struct ipi_mb_con_priv {
	struct ipi_mb_priv	*priv;
	unsigned int		idx;
	enum ipi_mb_chan_type	type;
	bool			configured;
	struct mbox_chan	*chan;
	void			*data;
	void			*ack;
	int rtos_hart_id;
};

struct ipi_mb_priv {
	struct device		*dev;

	struct mbox_controller	mbox;
	struct mbox_chan	chans[IPI_MB_CHANS];
	struct ipi_mb_con_priv  con_priv[IPI_MB_CHANS];

	void *mbase;
	int mem_size;
	atomic64_t tx_submit;
	atomic64_t tx_busy;
	atomic64_t tx_ipi_error;
	atomic64_t tx_ack;
	atomic64_t tx_stale_ack;
	atomic64_t recovery_kicks;
	struct delayed_work recovery_work;
	spinlock_t rx_lock;
	bool rx_delivering;
};

struct ipi_mb_priv *mb_priv;

static void *ipi_mb_channel(void *base, unsigned int idx)
{
	return base + idx * IPI_MB_CHAN_STRIDE;
}

static u32 ipi_mb_state(void *ack)
{
	u32 state = READ_ONCE(*(u32 *)ack);

	smp_rmb();
	return state;
}

static void ipi_mb_set_state(void *ack, u32 state)
{
	smp_wmb();
	WRITE_ONCE(*(u32 *)ack, state);
	smp_wmb();
}

static void ipi_mb_recovery_kick(struct ipi_mb_priv *priv,
				 struct ipi_mb_con_priv *cp,
				 unsigned int ipi_type)
{
	long long kicks = atomic64_inc_return(&priv->recovery_kicks);
	int ret;

	ret = sbi_send_ipi_amp(cp->rtos_hart_id, ipi_type);
	if (ret) {
		dev_err_ratelimited(priv->dev,
			"ipi recovery kick failed: type=%u state=%u ret=%d kicks=%lld\n",
			ipi_type, ipi_mb_state(cp->ack), ret, kicks);
	}
}

static int ipi_mb_generic_tx(struct ipi_mb_priv *priv,
				     struct ipi_mb_con_priv *cp,
				     void *data)
{
	u32 state;
	int ret;

	if (cp->type != IPI_MB_TYPE_TX)
		return -EINVAL;

	atomic64_inc(&priv->tx_submit);
	state = ipi_mb_state(cp->ack);
	/* A timed-out request may have been consumed after the mailbox core
	 * dropped it.  A new send is only submitted when no request is active,
	 * so an ACK observed here is stale and can safely be retired.
	 */
	if (state == IPI_MB_STATE_ACK) {
		ipi_mb_set_state(cp->ack, IPI_MB_STATE_IDLE);
		atomic64_inc(&priv->tx_stale_ack);
		dev_warn_ratelimited(priv->dev,
			"ipi recovered stale TX ACK: idx=%u count=%lld\n",
			cp->idx, atomic64_read(&priv->tx_stale_ack));
		state = IPI_MB_STATE_IDLE;
	}
	if (state != IPI_MB_STATE_IDLE) {
		atomic64_inc(&priv->tx_busy);
		dev_err_ratelimited(priv->dev,
			"ipi TX busy: idx=%u state=%u submit=%lld busy=%lld ack=%lld\n",
			cp->idx, state, atomic64_read(&priv->tx_submit),
			atomic64_read(&priv->tx_busy),
			atomic64_read(&priv->tx_ack));
		return -EBUSY;
	}

	memcpy(cp->data, data, IPI_MB_PAYLOAD_SIZE);
	ipi_mb_set_state(cp->ack, IPI_MB_STATE_DATA_READY);
	ret = sbi_send_ipi_amp(cp->rtos_hart_id, IPI_MB_TYPE_TX);
	if (ret) {
		atomic64_inc(&priv->tx_ipi_error);
		if (ipi_mb_state(cp->ack) == IPI_MB_STATE_DATA_READY)
			ipi_mb_set_state(cp->ack, IPI_MB_STATE_IDLE);
		dev_err_ratelimited(priv->dev,
			"ipi TX IPI failed: idx=%u state=%u ret=%d errors=%lld\n",
			cp->idx, ipi_mb_state(cp->ack), ret,
			atomic64_read(&priv->tx_ipi_error));
	}
	return ret;
}

static void ipi_mb_process_rx(void)
{
	unsigned int i;

	for (i = 0; i < IPI_MB_CHANS; i++) {
		struct ipi_mb_con_priv *cp = &mb_priv->con_priv[i];
		u8 message[IPI_MB_PAYLOAD_SIZE];
		unsigned long flags;
		bool received = false;
		u32 state;

		if (!cp->configured || cp->type != IPI_MB_TYPE_RX)
			continue;

		spin_lock_irqsave(&mb_priv->rx_lock, flags);
		if (mb_priv->rx_delivering) {
			spin_unlock_irqrestore(&mb_priv->rx_lock, flags);
			continue;
		}
		state = ipi_mb_state(cp->ack);
		if (state == IPI_MB_STATE_DATA_READY) {
			memcpy(message, cp->data, sizeof(message));
			memset(cp->data, 0, sizeof(message));
			ipi_mb_set_state(cp->ack, IPI_MB_STATE_ACK);
			mb_priv->rx_delivering = true;
			received = true;
		}
		spin_unlock_irqrestore(&mb_priv->rx_lock, flags);

		if (received) {
			mbox_chan_received_data(cp->chan, message);
			spin_lock_irqsave(&mb_priv->rx_lock, flags);
			mb_priv->rx_delivering = false;
			spin_unlock_irqrestore(&mb_priv->rx_lock, flags);
			sbi_send_ipi_amp(cp->rtos_hart_id,
					 IPI_MB_TYPE_RX);
		}
	}
}

static bool ipi_mb_last_tx_done(struct mbox_chan *chan)
{
	struct ipi_mb_con_priv *cp = chan->con_priv;

	if (!cp || cp->type != IPI_MB_TYPE_TX)
		return false;
	if (ipi_mb_state(cp->ack) != IPI_MB_STATE_ACK)
		return false;

	ipi_mb_set_state(cp->ack, IPI_MB_STATE_IDLE);
	atomic64_inc(&cp->priv->tx_ack);
	return true;
}

/*
 * AMP IPIs are notifications, while the shared state is authoritative.  If
 * either side coalesces or loses a notification, relying solely on the IPI
 * leaves a channel permanently in DATA_READY/ACK.  Poll the state as a
 * fallback, complete ACKed Linux transmissions, and re-kick notifications
 * whose state is still pending.
 */
static void ipi_mb_recovery_work(struct work_struct *work)
{
	struct ipi_mb_priv *priv = container_of(to_delayed_work(work),
						struct ipi_mb_priv, recovery_work);
	struct ipi_mb_con_priv *rx = &priv->con_priv[IPI_MB_TYPE_RX];
	struct ipi_mb_con_priv *tx = &priv->con_priv[IPI_MB_TYPE_TX];
	unsigned long flags;
	bool kick_rx;

	if (READ_ONCE(mb_priv) != priv)
		return;

	ipi_mb_process_rx();

	if (ipi_mb_state(tx->ack) == IPI_MB_STATE_DATA_READY) {
		ipi_mb_recovery_kick(priv, tx, IPI_MB_TYPE_TX);
	}
	spin_lock_irqsave(&priv->rx_lock, flags);
	kick_rx = !priv->rx_delivering &&
		  ipi_mb_state(rx->ack) == IPI_MB_STATE_ACK;
	spin_unlock_irqrestore(&priv->rx_lock, flags);
	if (kick_rx) {
		ipi_mb_recovery_kick(priv, rx, IPI_MB_TYPE_RX);
	}

	mod_delayed_work(system_wq, &priv->recovery_work,
			 msecs_to_jiffies(IPI_MB_RECOVERY_MS));
}

static void ipi_mb_isr(unsigned long msg_type)
{
	if (!msg_type || !mb_priv)
		return;

	if (msg_type & BIT(IPI_MB_TYPE_RX)) {
		ipi_mb_process_rx();
	}
	if (msg_type & BIT(IPI_MB_TYPE_TX)) {
		/* TX completion is state-polled, so a lost ACK IPI cannot wedge it. */
	}
}

static int ipi_mb_send_data(struct mbox_chan *chan, void *data)
{
	struct ipi_mb_con_priv *cp = chan->con_priv;
	struct ipi_mb_priv *priv;

	if (!cp || !cp->priv)
		return -ENODEV;
	priv = cp->priv;
	return ipi_mb_generic_tx(priv, cp, data);
}

static const struct mbox_chan_ops ipi_mb_ops = {
	.send_data = ipi_mb_send_data,
	.last_tx_done = ipi_mb_last_tx_done,
};

static struct mbox_chan *ipi_mb_xlate(struct mbox_controller *mbox,
				      const struct of_phandle_args *sp)
{
	struct ipi_mb_priv *priv = container_of(mbox, struct ipi_mb_priv, mbox);
	struct ipi_mb_con_priv *cp;
	struct mbox_chan *p_chan;
	u32 type, idx;

	if (sp->args_count != 2) {
		dev_err(mbox->dev, "Invalid argument count %d\n", sp->args_count);
		return ERR_PTR(-EINVAL);
	}

	type = sp->args[0]; /* channel type */
	idx = sp->args[1]; /* index */

	if (type > IPI_MB_TYPE_TX || idx >= mbox->num_chans) {
		dev_err(mbox->dev,
			"Not supported channel number: %d. (type: %d, idx: %d)\n",
			idx, type, idx);
		return ERR_PTR(-EINVAL);
	}

	p_chan = &mbox->chans[idx];
	cp = &priv->con_priv[idx];
	if (cp->configured && cp->type != type) {
		dev_err(mbox->dev,
			"channel %u already configured for type %u\n",
			idx, cp->type);
		return ERR_PTR(-EBUSY);
	}
	cp->type = type;
	cp->configured = true;

	return p_chan;
}

static void ipi_mb_init_generic(struct ipi_mb_priv *priv, int rtos_hart_id)
{
	unsigned int i;

	for (i = 0; i < IPI_MB_CHANS; i++) {
		struct ipi_mb_con_priv *cp = &priv->con_priv[i];

		cp->priv = priv;
		cp->idx = i;
		cp->chan = &priv->chans[i];
		cp->rtos_hart_id = rtos_hart_id;
		cp->chan->con_priv = cp;
	}

	priv->mbox.num_chans = IPI_MB_CHANS;
	priv->mbox.of_xlate = ipi_mb_xlate;

}

static int ipi_mb_init_mem_region(struct ipi_mb_priv *priv, struct platform_device *pdev)
{
	phys_addr_t phy_addr;
	struct resource *r;
	unsigned int i;

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	phy_addr = r->start;
	priv->mem_size = resource_size(r);
	priv->mbase = devm_memremap(priv->dev, phy_addr,
				   priv->mem_size,
				   MEMREMAP_WB);

	if (IS_ERR(priv->mbase)) {
		dev_err(priv->dev, "unable to map memory region: %llx %d\n",
			(u64)r->start, priv->mem_size);
		return -EBUSY;
	}

	for (i = 0; i < IPI_MB_CHANS; i++) {
		void *channel = ipi_mb_channel(priv->mbase, i);

		priv->con_priv[i].data = channel;
		priv->con_priv[i].ack = priv->con_priv[i].data +
			IPI_MB_STATE_OFFSET;
	}

	memset(priv->mbase, 0, priv->mem_size);

	return 0;
}

static int starfive_ipi_mb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct ipi_mb_priv *priv;
	u32 rtos_hart_id;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	if (of_property_read_u32(np, "rtos-hart-id",
				 &rtos_hart_id))
		return -EINVAL;

	priv->dev = dev;
	atomic64_set(&priv->tx_submit, 0);
	atomic64_set(&priv->tx_busy, 0);
	atomic64_set(&priv->tx_ipi_error, 0);
	atomic64_set(&priv->tx_ack, 0);
	atomic64_set(&priv->tx_stale_ack, 0);
	atomic64_set(&priv->recovery_kicks, 0);
	INIT_DELAYED_WORK(&priv->recovery_work, ipi_mb_recovery_work);
	spin_lock_init(&priv->rx_lock);

	priv->mbox.dev = dev;
	priv->mbox.ops = &ipi_mb_ops;
	priv->mbox.chans = priv->chans;
	priv->mbox.txdone_poll = true;
	priv->mbox.txpoll_period = 2;
	ipi_mb_init_generic(priv, rtos_hart_id);

	platform_set_drvdata(pdev, priv);

	ret = ipi_mb_init_mem_region(priv, pdev);
	if (ret)
		return ret;

	register_ipi_mailbox_handler(ipi_mb_isr);
	mb_priv = priv;

	ret = devm_mbox_controller_register(priv->dev, &priv->mbox);
	if (!ret) {
		dev_info(dev,
			 "IPI mailbox ready: base=%px size=%d tx_poll=2ms recovery=%dms\n",
			 priv->mbase, priv->mem_size, IPI_MB_RECOVERY_MS);
		mod_delayed_work(system_wq, &priv->recovery_work,
				 msecs_to_jiffies(IPI_MB_RECOVERY_MS));
	}

	return ret;
}

static int starfive_ipi_mb_remove(struct platform_device *pdev)
{
	struct ipi_mb_priv *priv = platform_get_drvdata(pdev);

	register_ipi_mailbox_handler(NULL);
	WRITE_ONCE(mb_priv, NULL);
	cancel_delayed_work_sync(&priv->recovery_work);
	return 0;
}

static const struct of_device_id ipi_amp_of_match[] = {
	{ .compatible = "starfive,ipi-amp-mailbox", .data = NULL },
	{},
};
MODULE_DEVICE_TABLE(of, ipi_amp_of_match);

static struct platform_driver starfive_ipi_mb_driver = {
	.probe = starfive_ipi_mb_probe,
	.remove = starfive_ipi_mb_remove,
	.driver = {
		.name = "starfive-ipi-mailbox",
		.of_match_table = ipi_amp_of_match,
	},
};
module_platform_driver(starfive_ipi_mb_driver);
MODULE_LICENSE("GPL");
