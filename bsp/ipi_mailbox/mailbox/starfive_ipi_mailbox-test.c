// SPDX-License-Identifier: GPL-2.0
/* Direct StarFive AMP IPI mailbox test client (without RPMsg). */

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/mailbox_client.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define IPI_TEST_NAME "starfive_ipi_mailbox"
#define IPI_TEST_PAYLOAD_SIZE 28

struct starfive_ipi_test {
	struct device *dev;
	struct mbox_client tx_client;
	struct mbox_client rx_client;
	struct mbox_chan *tx_chan;
	struct mbox_chan *rx_chan;
	struct miscdevice misc;
	wait_queue_head_t waitq;
	spinlock_t lock;
	char rx_value[IPI_TEST_PAYLOAD_SIZE + 1];
	bool rx_ready;
};

static void starfive_ipi_test_rx(struct mbox_client *client, void *message)
{
	struct starfive_ipi_test *test =
		container_of(client, struct starfive_ipi_test, rx_client);
	unsigned long flags;

	if (!message)
		return;

	spin_lock_irqsave(&test->lock, flags);
	memcpy(test->rx_value, message, IPI_TEST_PAYLOAD_SIZE);
	test->rx_value[IPI_TEST_PAYLOAD_SIZE] = '\0';
	test->rx_ready = true;
	spin_unlock_irqrestore(&test->lock, flags);
	wake_up_interruptible(&test->waitq);
}

static ssize_t starfive_ipi_test_read(struct file *file, char __user *buf,
					      size_t count, loff_t *ppos)
{
	struct starfive_ipi_test *test = file->private_data;
	char output[IPI_TEST_PAYLOAD_SIZE + 2];
	unsigned long flags;
	int len;
	int ret;

	ret = wait_event_interruptible(test->waitq, READ_ONCE(test->rx_ready));
	if (ret)
		return ret;

	spin_lock_irqsave(&test->lock, flags);
	memcpy(output, test->rx_value, IPI_TEST_PAYLOAD_SIZE);
	output[IPI_TEST_PAYLOAD_SIZE] = '\0';
	test->rx_ready = false;
	spin_unlock_irqrestore(&test->lock, flags);

	len = strnlen(output, IPI_TEST_PAYLOAD_SIZE);
	if (len < IPI_TEST_PAYLOAD_SIZE)
		output[len++] = '\n';
	ret = simple_read_from_buffer(buf, count, ppos, output, len);
	if (ret > 0)
		*ppos = 0;
	return ret;
}

static ssize_t starfive_ipi_test_write(struct file *file,
					       const char __user *buf,
					       size_t count, loff_t *ppos)
{
	struct starfive_ipi_test *test = file->private_data;
	char input[IPI_TEST_PAYLOAD_SIZE + 1] = { 0 };
	int ret;

	if (!count || count > sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, buf, count))
		return -EFAULT;
	if (input[count - 1] == '\n')
		input[count - 1] = '\0';
	ret = mbox_send_message(test->tx_chan, input);
	if (ret < 0) {
		dev_err(test->dev, "send string failed: %d\n", ret);
		return ret;
	}

	return count;
}

static int starfive_ipi_test_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;

	file->private_data = container_of(misc, struct starfive_ipi_test, misc);
	return 0;
}

static const struct file_operations starfive_ipi_test_fops = {
	.owner = THIS_MODULE,
	.open = starfive_ipi_test_open,
	.read = starfive_ipi_test_read,
	.write = starfive_ipi_test_write,
	.llseek = no_llseek,
};

static int starfive_ipi_test_probe(struct platform_device *pdev)
{
	struct starfive_ipi_test *test;
	int ret;

	test = devm_kzalloc(&pdev->dev, sizeof(*test), GFP_KERNEL);
	if (!test)
		return -ENOMEM;

	test->dev = &pdev->dev;
	init_waitqueue_head(&test->waitq);
	spin_lock_init(&test->lock);
	test->tx_client.dev = &pdev->dev;
	test->tx_client.tx_block = false;
	test->tx_client.knows_txdone = false;
	test->tx_client.tx_tout = 1000;
	test->rx_client.dev = &pdev->dev;
	test->rx_client.rx_callback = starfive_ipi_test_rx;

	test->tx_chan = mbox_request_channel_byname(&test->tx_client, "tx");
	if (IS_ERR(test->tx_chan))
		return dev_err_probe(&pdev->dev, PTR_ERR(test->tx_chan),
				     "failed to request tx channel\n");

	test->rx_chan = mbox_request_channel_byname(&test->rx_client, "rx");
	if (IS_ERR(test->rx_chan)) {
		ret = PTR_ERR(test->rx_chan);
		mbox_free_channel(test->tx_chan);
		return dev_err_probe(&pdev->dev, ret,
				     "failed to request rx channel\n");
	}

	test->misc.minor = MISC_DYNAMIC_MINOR;
	test->misc.name = IPI_TEST_NAME;
	test->misc.fops = &starfive_ipi_test_fops;
	test->misc.parent = &pdev->dev;
	platform_set_drvdata(pdev, test);

	ret = misc_register(&test->misc);
	if (ret) {
		mbox_free_channel(test->rx_chan);
		mbox_free_channel(test->tx_chan);
		return ret;
	}

	dev_info(&pdev->dev, "direct IPI mailbox test ready: /dev/%s\n",
		 test->misc.name);
	return 0;
}

static int starfive_ipi_test_remove(struct platform_device *pdev)
{
	struct starfive_ipi_test *test = platform_get_drvdata(pdev);

	misc_deregister(&test->misc);
	mbox_free_channel(test->rx_chan);
	mbox_free_channel(test->tx_chan);
	return 0;
}

static const struct of_device_id starfive_ipi_test_of_match[] = {
	{ .compatible = "starfive,ipi-mailbox-test" },
	{ }
};
MODULE_DEVICE_TABLE(of, starfive_ipi_test_of_match);

static struct platform_driver starfive_ipi_test_driver = {
	.probe = starfive_ipi_test_probe,
	.remove = starfive_ipi_test_remove,
	.driver = {
		.name = IPI_TEST_NAME,
		.of_match_table = starfive_ipi_test_of_match,
	},
};
module_platform_driver(starfive_ipi_test_driver);

MODULE_DESCRIPTION("StarFive direct AMP IPI mailbox test client");
MODULE_LICENSE("GPL");
