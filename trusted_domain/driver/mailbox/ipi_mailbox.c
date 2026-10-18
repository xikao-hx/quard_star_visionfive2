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
#include "quard_ipi_mailbox_protocol.h"

static struct mbox_controller mbox[SOC_MAX_MBOX];

static uint32_t ipi_mbox_get_state(volatile uint32_t *ack)
{
	uint32_t state = *ack;

	__asm volatile("fence r, rw" ::: "memory");
	return state;
}

static void ipi_mbox_set_state(volatile uint32_t *ack, uint32_t state)
{
	__asm volatile("fence rw, w" ::: "memory");
	*ack = state;
	__asm volatile("fence rw, rw" ::: "memory");
}

/* Send a 28-byte payload to Linux. */
static int ipi_mbox_send_data(struct mbox_chan *chan, const void *message)
{
	struct mbox_chan_intr *mb = chan->mb;
	uint32_t state = ipi_mbox_get_state(mb->ack);

	if (state != IPI_MBOX_STATE_IDLE)
		return -EBUSY;

	memcpy((void *)mb->data, message, IPI_MBOX_PAYLOAD_SIZE);
	ipi_mbox_set_state(mb->ack, IPI_MBOX_STATE_DATA_READY);
	sbi_send_ipi_amp(IPI_MBOX_LINUX_HART, IPI_MBOX_TYPE_RX);
	return 0;
}

static void ipi_mbox_process_rx(void)
{
	struct mbox_chan *chan =
		&mbox[EXTER0_MBOX].chan[IPI_MBOX_TYPE_TX];
	struct mbox_chan_intr *mb = chan->mb;
	uint8_t message[IPI_MBOX_PAYLOAD_SIZE];

	if (!mb || chan->cl.dir != RECV_TYPE ||
	    ipi_mbox_get_state(mb->ack) != IPI_MBOX_STATE_DATA_READY)
		return;

	memcpy(message, (const void *)mb->data, sizeof(message));
	memset((void *)mb->data, 0, sizeof(message));
	if (chan->cl.rx_callback)
		chan->cl.rx_callback(&chan->cl, message);
	ipi_mbox_set_state(mb->ack, IPI_MBOX_STATE_ACK);
	sbi_send_ipi_amp(IPI_MBOX_LINUX_HART, IPI_MBOX_TYPE_TX);
}

static void ipi_mbox_process_tx_done(void)
{
	struct mbox_chan *chan =
		&mbox[EXTER0_MBOX].chan[IPI_MBOX_TYPE_RX];
	struct mbox_chan_intr *mb = chan->mb;
	BaseType_t higher_priority_task_woken = pdFALSE;

	if (!mb || chan->cl.dir != SEND_TYPE ||
	    ipi_mbox_get_state(mb->ack) != IPI_MBOX_STATE_ACK)
		return;

	ipi_mbox_set_state(mb->ack, IPI_MBOX_STATE_IDLE);
	if (chan->cl.tx_done)
		chan->cl.tx_done(&chan->cl, NULL, 0);
	if (chan->xCompletionSemaphore)
		xSemaphoreGiveFromISR(chan->xCompletionSemaphore,
				      &higher_priority_task_woken);
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
	ret = ipi_mbox_send_data(chan, msg);
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
	int ret;

	ret = sbi_clear_ipi_amp(&msg_type);
	if (ret != 0) {
		sbi_clear_ipi();
		return;
	}

	if (msg_type & (1UL << IPI_MBOX_TYPE_TX))
		ipi_mbox_process_rx();
	if (msg_type & (1UL << IPI_MBOX_TYPE_RX))
		ipi_mbox_process_tx_done();
}

void quard_mbox_ipi_controller_init(enum mbox_type type)
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
