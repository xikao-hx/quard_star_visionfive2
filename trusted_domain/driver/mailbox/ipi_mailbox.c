#define LOG_TAG "IPI_MAILBOX"
#include "elog.h"
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "sbi.h"
#include "ipi_mailbox.h"

/* Layout used by Linux starfive_ipi_mailbox.c. */
#define IPI_MBOX_BASE          0x6e402000UL
#define IPI_MBOX_PAYLOAD_SIZE  28U
#define IPI_MBOX_STATE_OFFSET   IPI_MBOX_PAYLOAD_SIZE
#define IPI_MBOX_CHAN_STRIDE    32U
#define IPI_MB_TYPE_RX         0U
#define IPI_MB_TYPE_TX         1U
#define IPI_MBOX_LINUX_HART    1U
#define IPI_MBOX_CHANS         2U

#define IPI_MBOX_STATE_IDLE       0U
#define IPI_MBOX_STATE_DATA_READY 1U
#define IPI_MBOX_STATE_ACK        2U

static struct mbox_controller mbox[SOC_MAX_MBOX];

extern void serial_print(const char *fmt, ...);

static uint32_t ipi_mailbox_state(volatile uint32_t *ack)
{
	uint32_t state = *ack;

	__asm volatile("fence r, rw" ::: "memory");
	return state;
}

static void ipi_mailbox_set_state(volatile uint32_t *ack, uint32_t state)
{
	__asm volatile("fence rw, w" ::: "memory");
	*ack = state;
	__asm volatile("fence rw, rw" ::: "memory");
}

/* Send a 28-byte payload to Linux. */
static int ipi_mailbox_send_data(struct mbox_chan *chan, const void *message)
{
	struct mbox_chan_intr *mb = chan->mb;
	uint32_t state = ipi_mailbox_state(mb->ack);

	if (state != IPI_MBOX_STATE_IDLE)
		return -EBUSY;

	memcpy((void *)mb->data, message, IPI_MBOX_PAYLOAD_SIZE);
	ipi_mailbox_set_state(mb->ack, IPI_MBOX_STATE_DATA_READY);
	sbi_send_ipi_amp(IPI_MBOX_LINUX_HART, IPI_MB_TYPE_RX);
	return 0;
}

static void ipi_mailbox_send_ack(struct mbox_chan_intr *mb,
					unsigned int ipi_type)
{
	ipi_mailbox_set_state(mb->ack, IPI_MBOX_STATE_ACK);
	sbi_send_ipi_amp(IPI_MBOX_LINUX_HART, ipi_type);
}

static void ipi_mailbox_process(unsigned int ipi_type)
{
	unsigned int idx;

	for (idx = 0; idx < IPI_MBOX_CHANS; idx++) {
		struct mbox_chan *chan = &mbox[EXTER0_MBOX].chan[idx];
		struct mbox_chan_intr *mb;
		uint32_t state;

		if (!chan->mb)
			continue;
		if (ipi_type == IPI_MB_TYPE_TX && chan->cl.dir != RECV_TYPE)
			continue;
		if (ipi_type == IPI_MB_TYPE_RX && chan->cl.dir != SEND_TYPE)
			continue;

		mb = chan->mb;
		state = ipi_mailbox_state(mb->ack);

		if (chan->cl.dir == RECV_TYPE &&
		    state == IPI_MBOX_STATE_DATA_READY) {
			uint8_t message[IPI_MBOX_PAYLOAD_SIZE];

			memcpy(message, (const void *)mb->data,
			       IPI_MBOX_PAYLOAD_SIZE);
			memset((void *)mb->data, 0, IPI_MBOX_PAYLOAD_SIZE);
			if (chan->cl.rx_callback)
				chan->cl.rx_callback(&chan->cl, message);
			ipi_mailbox_send_ack(mb, ipi_type);
		} else if (chan->cl.dir == SEND_TYPE &&
			   state == IPI_MBOX_STATE_ACK) {
			BaseType_t higher_priority_task_woken = pdFALSE;

			ipi_mailbox_set_state(mb->ack, IPI_MBOX_STATE_IDLE);
			if (chan->cl.tx_done)
				chan->cl.tx_done(&chan->cl, NULL, 0);
			if (chan->xCompletionSemaphore)
				xSemaphoreGiveFromISR(chan->xCompletionSemaphore,
						      &higher_priority_task_woken);
		}
	}
}

struct mbox_chan *mbox_request_channel(enum mbox_type type,
					       struct mbox_client *client)
{
	struct mbox_controller *controller;
	struct mbox_chan *chan;
	struct mbox_chan_intr *mb;

	if (type >= SOC_MAX_MBOX || !client || client->idx >= IPI_MBOX_CHANS)
		return NULL;
	if (client->dir != SEND_TYPE && client->dir != RECV_TYPE)
		return NULL;

	controller = &mbox[type];
	chan = &controller->chan[client->idx];
	mb = &controller->mb[client->idx];

	mb->master = client->dir == SEND_TYPE;
	chan->cl = *client;
	chan->mb = mb;

	if (!chan->xMutex)
		chan->xMutex = xSemaphoreCreateMutexStatic(&chan->xMutex_buf);
	if (!chan->xCompletionSemaphore)
		chan->xCompletionSemaphore =
			xSemaphoreCreateBinaryStatic(&chan->xCompletionSemaphore_buf);

	return chan;
}

int mbox_send_message(struct mbox_chan *chan, void *msg)
{
	int ret;

	if (!chan || !chan->mb || !chan->mb->master || !msg)
		return -EINVAL;
	if (xSemaphoreTake(chan->xMutex, portMAX_DELAY) != pdTRUE)
		return -EBUSY;

	(void)xSemaphoreTake(chan->xCompletionSemaphore, 0);
	ret = ipi_mailbox_send_data(chan, msg);
	if (ret < 0)
		goto out;
	if (xSemaphoreTake(chan->xCompletionSemaphore,
			   pdMS_TO_TICKS(1000)) != pdTRUE) {
		ret = -ETIMEDOUT;
		goto out;
	}
	ret = 0;

out:
	xSemaphoreGive(chan->xMutex);
	return ret;
}

void quard_ipi_mailbox_handle(void)
{
	unsigned long msg_type = 0;
	unsigned long pending_type = 0;
	int ret;

	/*
	 * The platform can reassert SSIP after the AMP ecall returns, so retain
	 * the architectural clear required by the FreeRTOS scheduler.  Read the
	 * AMP bits once more afterwards: if that clear raced with a newly arrived
	 * mailbox IPI, the second atomic exchange consumes its message instead of
	 * leaving it stranded without SSIP.
	 */
	ret = sbi_clear_ipi_amp(&msg_type);
	sbi_clear_ipi();
	if (ret != 0) {
		/* The standard clear above handles a normal FreeRTOS yield. */
		return;
	}

	if (sbi_clear_ipi_amp(&pending_type) == 0)
		msg_type |= pending_type;

	if (msg_type & (1UL << IPI_MB_TYPE_TX))
		ipi_mailbox_process(IPI_MB_TYPE_TX);
	if (msg_type & (1UL << IPI_MB_TYPE_RX))
		ipi_mailbox_process(IPI_MB_TYPE_RX);
}

void quard_mailbox_ipi_controller_init(enum mbox_type type)
{
	struct mbox_controller *controller;
	unsigned int i;
	struct mbox_chan_intr *mb;
	uintptr_t channel_base;

	if (type != EXTER0_MBOX) {
		LOG_E("mbox_type not support");
		return;
	}

	controller = &mbox[type];
	if (controller->base == IPI_MBOX_BASE)
		return;
	controller->base = IPI_MBOX_BASE;

	for (i = 0; i < IPI_MBOX_CHANS; i++) {
		memset((void *)((uintptr_t)controller->base +
			       i * IPI_MBOX_CHAN_STRIDE),
		       0, IPI_MBOX_CHAN_STRIDE);
		
		mb = &controller->mb[i];
		channel_base = controller->base + i * IPI_MBOX_CHAN_STRIDE;

		mb->idx = i;
		mb->mbox = controller;
		mb->data = (volatile uint8_t *)channel_base;
		mb->ack = (volatile uint32_t *)(channel_base + IPI_MBOX_STATE_OFFSET);
	}

	__asm volatile("fence rw, rw" ::: "memory");
	LOG_I("FreeRTOS IPI mailbox init successful: base=0x%lx\r\n", (unsigned long)IPI_MBOX_BASE);
}
