#include "FreeRTOS.h"
#include "task.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include "mailbox_ipi.h"
#include "shell.h"

extern void serial_print(const char *fmt, ...);

static struct mbox_chan *ipi_test_tx_chan;
static struct mbox_chan *ipi_test_rx_chan;
static StaticSemaphore_t ipi_test_rx_semaphore_buf;
static SemaphoreHandle_t ipi_test_rx_semaphore;
static volatile char ipi_test_rx_message[29];

static void ipi_mbox_test_rx_callback(struct mbox_client *client, void *msg)
{
	(void)client;
	BaseType_t higher_priority_task_woken = pdFALSE;

	memcpy((void *)ipi_test_rx_message, msg,
	       sizeof(ipi_test_rx_message) - 1);
	ipi_test_rx_message[sizeof(ipi_test_rx_message) - 1] = '\0';
	xSemaphoreGiveFromISR(ipi_test_rx_semaphore,
			      &higher_priority_task_woken);
}

static int ipi_mbox_send_cmd(int argc, char *argv[])
{
	char value[28] = { 0 };
	int ret;

	if (argc != 2) {
		serial_print("usage: ipi_mbox_send <value>\r\n");
		return -EINVAL;
	}

	strncpy(value, argv[1], sizeof(value) - 1);
	if (!ipi_test_tx_chan) {
		serial_print("ipi mailbox TX channel is not ready\r\n");
		return -ENODEV;
	}

	serial_print("IPI TX: payload=\"%s\", waiting ACK...\r\n", value);
	ret = mbox_send_message(ipi_test_tx_chan, &value);
	serial_print("IPI TX: %s (ret=%d)\r\n",
		     ret ? "ACK timeout/error" : "ACK received", ret);
	return ret;
}

static int ipi_mbox_recv_cmd(int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	serial_print("IPI RX: waiting for message...\r\n");
	if (xSemaphoreTake(ipi_test_rx_semaphore, portMAX_DELAY) != pdTRUE)
		return -EIO;
	serial_print("IPI RX: message=\"%s\"\r\n",
		     (char *)ipi_test_rx_message);
	return 0;
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0)|SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN)|SHELL_CMD_DISABLE_RETURN,
			 ipi_mbox_send, ipi_mbox_send_cmd, send IPI mailbox payload and wait ACK);
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0)|SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN)|SHELL_CMD_DISABLE_RETURN,
			 ipi_mbox_recv, ipi_mbox_recv_cmd, wait for IPI mailbox RX payload);

void mailbox_test_init(void)
{
	struct mbox_client tx_client = { 0 };
	struct mbox_client rx_client = { 0 };

	quard_mailbox_ipi_controller_init(EXTER0_MBOX);
	ipi_test_rx_semaphore =
		xSemaphoreCreateBinaryStatic(&ipi_test_rx_semaphore_buf);
	tx_client.dir = SEND_TYPE;
	tx_client.idx = 0;
	ipi_test_tx_chan = mbox_request_channel(EXTER0_MBOX, &tx_client);

	rx_client.dir = RECV_TYPE;
	rx_client.idx = 1;
	rx_client.rx_callback = ipi_mbox_test_rx_callback;
	ipi_test_rx_chan = mbox_request_channel(EXTER0_MBOX, &rx_client);
	if (!ipi_test_tx_chan || !ipi_test_rx_chan)
		serial_print("IPI mailbox test channel init failed\r\n");
}
