// SPDX-License-Identifier: GPL-2.0+
/* VisionFive 2 U-Boot proper shared-memory IPI mailbox provider. */

#include <common.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <mailbox-uclass.h>
#include <asm/sbi.h>
#include <linux/delay.h>
#include <linux/errno.h>

#define IPI_MB_CHANS             2U
#define IPI_MB_PAYLOAD_SIZE      28U
#define IPI_MB_STATE_OFFSET      28U
#define IPI_MB_CHAN_STRIDE       32U
#define IPI_MB_REQUIRED_SIZE     (IPI_MB_CHANS * IPI_MB_CHAN_STRIDE)
#define IPI_MB_TYPE_RX           0U
#define IPI_MB_TYPE_TX           1U
#define IPI_MB_STATE_IDLE        0U
#define IPI_MB_STATE_DATA_READY  1U
#define IPI_MB_STATE_ACK         2U
#define IPI_MB_TIMEOUT_MS        5000UL
#define IPI_MB_RECOVERY_MS       20UL

#define SBI_EXT_IPI_SEND_EXT_DOMAIN 0x100U
#define SBI_EXT_IPI_SET_AMP_DATA    0x101U
#define SBI_EXT_IPI_CLEAR_AMP       0x102U

struct starfive_ipi_mbox {
	uintptr_t base;
	u32 remote_hart;
};

static ulong starfive_amp_bits[CONFIG_NR_CPUS] __aligned(64);

static void ipi_mb_fence(void)
{
	asm volatile("fence rw, rw" ::: "memory");
}

static volatile u8 *ipi_mb_data(struct starfive_ipi_mbox *priv, u32 channel)
{
	return (volatile u8 *)(priv->base + channel * IPI_MB_CHAN_STRIDE);
}

static volatile u32 *ipi_mb_state_ptr(struct starfive_ipi_mbox *priv,
				      u32 channel)
{
	return (volatile u32 *)(priv->base + channel * IPI_MB_CHAN_STRIDE +
				IPI_MB_STATE_OFFSET);
}

static u32 ipi_mb_get_state(volatile u32 *state)
{
	u32 value = *state;

	ipi_mb_fence();
	return value;
}

static void ipi_mb_set_state(volatile u32 *state, u32 value)
{
	ipi_mb_fence();
	*state = value;
	ipi_mb_fence();
}

static int ipi_mb_sbi_call(u32 fid, ulong arg0, ulong arg1, ulong arg2)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_IPI, fid, arg0, arg1, arg2, 0, 0, 0);
	return ret.error ? (int)ret.error : 0;
}

static int ipi_mb_notify(struct starfive_ipi_mbox *priv, u32 type)
{
	return ipi_mb_sbi_call(SBI_EXT_IPI_SEND_EXT_DOMAIN, 0,
			       priv->remote_hart, type);
}

static void ipi_mb_clear_local(void)
{
	ulong pending = 0;

	(void)ipi_mb_sbi_call(SBI_EXT_IPI_CLEAR_AMP,
			      (ulong)&pending, 0, 0);
}

static int starfive_ipi_xlate(struct mbox_chan *chan,
			      struct ofnode_phandle_args *args)
{
	if (args->args_count != 1 || args->args[0] >= IPI_MB_CHANS)
		return -EINVAL;
	chan->id = args->args[0];
	return 0;
}

static int starfive_ipi_send(struct mbox_chan *chan, const void *message)
{
	struct starfive_ipi_mbox *priv = dev_get_priv(chan->dev);
	volatile u8 *data;
	volatile u32 *state;
	ulong start;
	ulong last_kick;
	int ret;

	if (chan->id != IPI_MB_TYPE_TX || !message)
		return -EINVAL;
	data = ipi_mb_data(priv, chan->id);
	state = ipi_mb_state_ptr(priv, chan->id);
	if (ipi_mb_get_state(state) == IPI_MB_STATE_ACK)
		ipi_mb_set_state(state, IPI_MB_STATE_IDLE);
	if (ipi_mb_get_state(state) != IPI_MB_STATE_IDLE)
		return -EBUSY;

	memcpy((void *)data, message, IPI_MB_PAYLOAD_SIZE);
	ipi_mb_set_state(state, IPI_MB_STATE_DATA_READY);
	ret = ipi_mb_notify(priv, IPI_MB_TYPE_TX);
	if (ret) {
		ipi_mb_set_state(state, IPI_MB_STATE_IDLE);
		return ret;
	}

	start = get_timer(0);
	last_kick = start;
	while (get_timer(start) <= IPI_MB_TIMEOUT_MS) {
		if (ipi_mb_get_state(state) == IPI_MB_STATE_ACK) {
			ipi_mb_set_state(state, IPI_MB_STATE_IDLE);
			ipi_mb_clear_local();
			return 0;
		}
		if (get_timer(last_kick) >= IPI_MB_RECOVERY_MS) {
			ret = ipi_mb_notify(priv, IPI_MB_TYPE_TX);
			if (ret)
				return ret;
			last_kick = get_timer(0);
		}
		udelay(10);
	}
	return -ETIMEDOUT;
}

static int starfive_ipi_recv(struct mbox_chan *chan, void *message)
{
	struct starfive_ipi_mbox *priv = dev_get_priv(chan->dev);
	volatile u8 *data;
	volatile u32 *state;
	int ret;

	if (chan->id != IPI_MB_TYPE_RX || !message)
		return -EINVAL;
	data = ipi_mb_data(priv, chan->id);
	state = ipi_mb_state_ptr(priv, chan->id);
	if (ipi_mb_get_state(state) != IPI_MB_STATE_DATA_READY) {
		udelay(10);
		return -ENODATA;
	}

	memcpy(message, (const void *)data, IPI_MB_PAYLOAD_SIZE);
	memset((void *)data, 0, IPI_MB_PAYLOAD_SIZE);
	ipi_mb_set_state(state, IPI_MB_STATE_ACK);
	ret = ipi_mb_notify(priv, IPI_MB_TYPE_RX);
	ipi_mb_clear_local();
	return ret;
}

static const struct mbox_ops starfive_ipi_ops = {
	.of_xlate = starfive_ipi_xlate,
	.send = starfive_ipi_send,
	.recv = starfive_ipi_recv,
};

static int starfive_ipi_probe(struct udevice *dev)
{
	struct starfive_ipi_mbox *priv = dev_get_priv(dev);
	fdt_size_t size;
	fdt_addr_t address;
	int ret;

	address = dev_read_addr_size(dev, "reg", &size);
	if (address == FDT_ADDR_T_NONE || size < IPI_MB_REQUIRED_SIZE)
		return -EINVAL;
	priv->base = (uintptr_t)address;
	ret = dev_read_u32(dev, "rtos-hart-id", &priv->remote_hart);
	if (ret || priv->remote_hart >= CONFIG_NR_CPUS)
		return -EINVAL;

	memset(starfive_amp_bits, 0, sizeof(starfive_amp_bits));
	ipi_mb_fence();
	ret = ipi_mb_sbi_call(SBI_EXT_IPI_SET_AMP_DATA,
			      (ulong)starfive_amp_bits, 0, 0);
	if (ret)
		return ret;

	dev_info(dev, "IPI mailbox ready: base=0x%lx rtos-hart=%u\n",
		 (ulong)priv->base, priv->remote_hart);
	return 0;
}

static const struct udevice_id starfive_ipi_ids[] = {
	{ .compatible = "starfive,ipi-amp-mailbox" },
	{ }
};

U_BOOT_DRIVER(starfive_ipi_mailbox) = {
	.name = "starfive_ipi_mailbox",
	.id = UCLASS_MAILBOX,
	.of_match = starfive_ipi_ids,
	.probe = starfive_ipi_probe,
	.ops = &starfive_ipi_ops,
	.priv_auto = sizeof(struct starfive_ipi_mbox),
};
