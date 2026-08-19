#include <linux/string.h>
#include <linux/module.h>
#include <linux/mailbox_client.h>
#include <linux/console.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/serial.h>
#include <linux/serial_core.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/mailbox_client.h>
#include <linux/hwspinlock.h>
#include <linux/atomic.h>
#include <linux/ratelimit.h>
#include <linux/workqueue.h>
#include <quard_mbox_router.h>

#define REMOTE_CONSOLE_PAYLOAD_SIZE \
	(sizeof(((struct quard_consumer_msg *)0)->params))
#define REMOTE_CONSOLE_RETRY_MS	10

struct remote_console {
	struct uart_driver driver;
	struct uart_port port;
	struct quard_mbox_consumer *mbox;
	struct delayed_work tx_work;
	bool rx_pending_cr;
	atomic64_t tx_attempted;
	atomic64_t tx_succeeded;
	atomic64_t tx_failed;
};

static void remote_console_receive(struct device *dev, struct quard_consumer_msg *msg)
{
	struct remote_console *console = dev_get_drvdata(dev);
	struct tty_port *port;
	char *data_char;
	unsigned int num_bytes, i;

	if (!console || !console->port.state)
		return;
	port = &console->port.state->port;

	data_char = (char *)msg->params;
	num_bytes = msg->cmd;
	if (num_bytes > REMOTE_CONSOLE_PAYLOAD_SIZE)
		num_bytes = REMOTE_CONSOLE_PAYLOAD_SIZE;
	for (i = 0; i < num_bytes; i++) {
		/* Normalize line endings before handing bytes to the TTY.  CR and
		 * LF are often split across mailbox payloads; holding CR until the
		 * next byte guarantees that userspace receives one atomic CRLF and
		 * cannot advance a line while retaining the previous column.
		 */
		if (data_char[i] == '\r') {
			if (console->rx_pending_cr)
				tty_insert_flip_char(port, '\r', TTY_NORMAL);
			console->rx_pending_cr = true;
			continue;
		}
		if (data_char[i] == '\n') {
			tty_insert_flip_char(port, '\r', TTY_NORMAL);
			tty_insert_flip_char(port, '\n', TTY_NORMAL);
			console->rx_pending_cr = false;
			continue;
		}
		if (console->rx_pending_cr) {
			tty_insert_flip_char(port, '\r', TTY_NORMAL);
			console->rx_pending_cr = false;
		}
		tty_insert_flip_char(port, data_char[i], TTY_NORMAL);
	}
	tty_flip_buffer_push(port);
}

static unsigned int remote_console_uart_tx_empty(struct uart_port *port)
{
	return TIOCSER_TEMT;  
}

static void remote_console_uart_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
}

static unsigned int remote_console_uart_get_mctrl(struct uart_port *port)
{
	/* The remote endpoint is permanently connected. */
	return TIOCM_CTS | TIOCM_DSR | TIOCM_CAR;
}

static void remote_console_uart_stop_tx(struct uart_port *port)
{
}

static int remote_console_write_chunk(struct remote_console *console,
				      const char *data, unsigned int count)
{
	int ret;
	struct quard_consumer_msg pdata;

	if (!console->mbox)
		return -ENODEV;
	if (!count || count > REMOTE_CONSOLE_PAYLOAD_SIZE)
		return -EINVAL;

	memset((void *)&pdata, 0, sizeof(pdata));

	pdata.cmd = count;
	memcpy(pdata.params, data, count);
	atomic64_inc(&console->tx_attempted);
	ret = quard_mbox_send_msg(console->mbox, &pdata);
	if (ret) {
		atomic64_inc(&console->tx_failed);
		dev_err_ratelimited(console->port.dev,
			"rshell TX failed: ret=%d bytes=%u attempted=%lld ok=%lld failed=%lld; retrying\n",
			ret, count, atomic64_read(&console->tx_attempted),
			atomic64_read(&console->tx_succeeded),
			atomic64_read(&console->tx_failed));
	} else {
		atomic64_inc(&console->tx_succeeded);
	}

	return ret;
}

static void remote_console_tx_work(struct work_struct *work)
{
	struct remote_console *console =
		container_of(to_delayed_work(work), struct remote_console, tx_work);
	struct uart_port *port = &console->port;
	struct circ_buf *xmit = &port->state->xmit;
	unsigned long count;
	unsigned int chunk;
	int ret;

	count = CIRC_CNT(xmit->head, xmit->tail, UART_XMIT_SIZE);
	while ((count = CIRC_CNT_TO_END(READ_ONCE(xmit->head), xmit->tail,
					UART_XMIT_SIZE)) != 0) {
		chunk = min_t(unsigned int, count, REMOTE_CONSOLE_PAYLOAD_SIZE);
		ret = remote_console_write_chunk(console, &xmit->buf[xmit->tail],
						 chunk);
		if (ret) {
			mod_delayed_work(system_wq, &console->tx_work,
					 msecs_to_jiffies(REMOTE_CONSOLE_RETRY_MS));
			return;
		}
		xmit->tail = (xmit->tail + chunk) & (UART_XMIT_SIZE - 1);
		port->icount.tx += chunk;
		uart_write_wakeup(port);
	}
}

static void remote_console_uart_start_tx(struct uart_port *port)
{
	struct remote_console *console = port->private_data;

	mod_delayed_work(system_wq, &console->tx_work, 0);
}

static void remote_console_uart_stop_rx(struct uart_port *port)
{
}

static void remote_console_uart_break_ctl(struct uart_port *port, int ctl)
{
}

static int remote_console_uart_startup(struct uart_port *port)
{
	return 0;
}

static void remote_console_uart_shutdown(struct uart_port *port)
{
}

static void remote_console_uart_set_termios(struct uart_port *port,
				       struct ktermios *new,
				       const struct ktermios *old)
{
}

static const struct uart_ops remote_console_uart_ops = {
	.tx_empty = remote_console_uart_tx_empty,       
	.set_mctrl = remote_console_uart_set_mctrl,    
	.get_mctrl = remote_console_uart_get_mctrl,     
	.stop_tx = remote_console_uart_stop_tx,         
	.start_tx = remote_console_uart_start_tx,
	.stop_rx = remote_console_uart_stop_rx,         
	.break_ctl = remote_console_uart_break_ctl,     
	.startup = remote_console_uart_startup,         
	.shutdown = remote_console_uart_shutdown,
	.set_termios = remote_console_uart_set_termios,  
};

static int remote_console_probe(struct platform_device *pdev)
{
	struct uart_port *port;
	struct remote_console *remote_shell;
	struct quard_mbox_consumer *mbox;
	struct device *dev = &pdev->dev;

	int err;
	const char* device_name;
	
	if (of_property_read_string(pdev->dev.of_node, "device-name", &device_name)) {
        dev_err(dev, "Failed to get device-name from DT\n");
        return -EINVAL;
    }
    
	remote_shell = devm_kzalloc(dev, sizeof(*remote_shell), GFP_KERNEL);
	if (!remote_shell)
		return -ENOMEM;
	platform_set_drvdata(pdev, remote_shell);

	INIT_DELAYED_WORK(&remote_shell->tx_work, remote_console_tx_work);
	atomic64_set(&remote_shell->tx_attempted, 0);
	atomic64_set(&remote_shell->tx_succeeded, 0);
	atomic64_set(&remote_shell->tx_failed, 0);

    if (strcmp(device_name, "quard-freertos") == 0) {
        mbox = quard_mbox_consumer_register(dev, remote_console_receive);
        if (IS_ERR(mbox)) {
            dev_err(dev, "failed to match mbox router\n");
            return PTR_ERR(mbox);
        }
        remote_shell->mbox = mbox;
    }
    
	/* setup the driver */
    remote_shell->driver.driver_name = devm_kzalloc(dev, strlen("drv_") + strlen(device_name) + 1, GFP_KERNEL);
    if (!remote_shell->driver.driver_name) {
        dev_err(dev, "Failed to allocate memory for driver name\n");
        return -ENOMEM;
    }
    snprintf((char *)remote_shell->driver.driver_name, strlen("drv_") + strlen(device_name) + 1, "drv_%s", device_name);
    
	remote_shell->driver.owner = THIS_MODULE;
	remote_shell->driver.dev_name = device_name;
	/* This device is a remote shell TTY, not a Linux printk console. */
	remote_shell->driver.cons = NULL;
	remote_shell->driver.nr = 1;

	err = uart_register_driver(&remote_shell->driver);
	if (err) {
		dev_err(dev, "failed to register UART driver: %d\n",
			err);
		goto remote_mb_router;
	}

	/* setup the port */
	port = &remote_shell->port;
	spin_lock_init(&port->lock);    
	port->dev = dev;
	port->type = PORT_TEGRA_TCU;   
	port->ops = &remote_console_uart_ops;
	port->fifosize = 1;             
	port->iotype = UPIO_MEM;
	port->flags = UPF_BOOT_AUTOCONF;    
	port->private_data = remote_shell;
    
	err = uart_add_one_port(&remote_shell->driver, port);
	if (err) {
		dev_err(dev, "failed to add UART port: %d\n", err);
		goto unregister_uart;
	}
	dev_info(dev, "remote shell ready: tty=%s0 payload=%zu retry=%dms\n",
		 device_name, REMOTE_CONSOLE_PAYLOAD_SIZE,
		 REMOTE_CONSOLE_RETRY_MS);

	dev_set_drvdata(dev, remote_shell);
	return 0;

unregister_uart:
	uart_unregister_driver(&remote_shell->driver);
remote_mb_router:
	if (remote_shell->mbox)
		quard_mbox_consumer_unregister(remote_shell->mbox);
	return err;
}

static int remote_console_remove(struct platform_device *pdev)
{
	struct remote_console *remote_shell = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&remote_shell->tx_work);
	if (remote_shell->mbox)
		quard_mbox_consumer_unregister(remote_shell->mbox);

	uart_remove_one_port(&remote_shell->driver, &remote_shell->port);
	uart_unregister_driver(&remote_shell->driver);

	return 0;
}

static const struct of_device_id remote_console_match[] = {
	{ .compatible = "quard,remote_console" },
	{ }
};
MODULE_DEVICE_TABLE(of, remote_console_match);

static struct platform_driver remote_console_driver = {
	.driver = {
		.name = "quard_remote_console",
		.of_match_table = remote_console_match,
	},
	.probe = remote_console_probe,
	.remove = remote_console_remove,
};
module_platform_driver(remote_console_driver);

MODULE_AUTHOR("quard star team");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("remote console driver");
