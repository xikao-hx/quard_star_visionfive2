#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <net/sock.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include "quard_mbox_router.h"
#include <quard_log.h>

#define IOCTL_SET_BUFFER _IOW('a', 'a', int32_t*)

#define	DEVICE_NAME	    "log_device"
#define	CLASS_NAME		"log_class"

/* sysfs */
enum {
	BOOTROM	= 0,
	BL1,
	SBI,
	UBOOT,
};
static int boot_id = (int)BOOTROM;

/* mmap */
typedef enum fw_index {
	MCU_LOG = 0,
	BL1_SBI_LOG,
	UBOOT_LOG,
	FW_MAX_INDEX,
} fw_enum;

static const char * const fw_memory_region_names[FW_MAX_INDEX] = {
	[MCU_LOG] = "mcu",
	[BL1_SBI_LOG] = "bl1-sbi",
	[UBOOT_LOG] = "uboot",
};

struct fw_log {
	void *shared_buf;
	char *log_buf;
	u32 log_buf_size;
};

struct log_device {
	struct cdev cdev;
	struct fw_log fw[FW_MAX_INDEX];
	struct platform_device *pdev;
	struct device *dev;
};

struct log_device *log_dev;

static struct class *quard_log_class = NULL;
static dev_t quard_log_devt;
static struct task_struct *soc_klogd_task;
static int buffer_select = 0;
static struct kobject *soc_log_kobj;
static struct quard_mbox_consumer *mcu_log_mbox;

/* netlink */
#define NETLINK_SOC_LOG		31
struct sock *nl_sock = NULL;
u32 soc_logd_pid = 0;

struct mcu_log_rb {
	uint8_t * buffer_ptr;
	u32 read_wrap : 1;
	u32 read_idx : 31;
	u32 write_wrap : 1;
	u32 write_idx : 31;
	u32 buffer_size;
};

struct mcu_log_buf {
	u32 head_magic;
	u32 state;
	struct mcu_log_rb log_rb;
	u32 body_magic;
	u32 log_buffer;
};

static ssize_t log_flush(struct kobject *kobj, struct kobj_attribute *attr,
						 const char *buf, size_t count)
{
	int ret;

	if (!sysfs_streq(buf, "flush")) {
		pr_warn("%s: Invalid input: %s\n", attr->attr.name, buf);
		return count;
	}

	if (sysfs_streq(attr->attr.name, "mcu_log")) {
		struct quard_consumer_msg pdata;
		pr_info("Handling flush for mcu_log.\n");
		memset((void *)&pdata, 0, sizeof(pdata));
		pdata.cmd = 4;
		pdata.params[0] = 0x1234ABCD;
		ret = quard_mbox_send_msg(mcu_log_mbox, &pdata);
	}

	return count;
}
#define CREATE_TRIGGER_ATTR(name) \
	static struct kobj_attribute name##_attr = __ATTR(name, 0220, NULL, log_flush)
CREATE_TRIGGER_ATTR(mcu_log);

int display_boot_log(int fw_idx, int boot_id)
{
	struct ramlog_buffer *rb = NULL;
	u32 head, tail, size, valid_len;
	void *boot_ramlog_page_addr = log_dev->fw[fw_idx].shared_buf;

	char *byte_ptr;
	size_t i, log_offset = 0, region_size = 0;

	switch(boot_id) {
	case BL1:
		log_offset = BL1_LOG_BUF_OFFSET;
		region_size = BL1_LOG_BUF_SIZE;
		break;
	case SBI:
		log_offset = SBI_LOG_BUF_OFFSET;
		region_size = SBI_LOG_BUF_SIZE;
		break;
	case UBOOT:
		log_offset = 0;
		region_size = UBOOT_LOG_BUF_SIZE;
		break;
	default:
		return -EINVAL;
	}

	if (!boot_ramlog_page_addr) {
		pr_err("%s: invalid memory mapping %d.\n", __func__, fw_idx);
		return -EIO;
	}
	if (log_offset + region_size > log_dev->fw[fw_idx].log_buf_size ||
	    region_size <= sizeof(*rb)) {
		pr_err("%s: invalid region for firmware %d.\n", __func__, fw_idx);
		return -EINVAL;
	}

	rb = (struct ramlog_buffer *)((uintptr_t)boot_ramlog_page_addr + log_offset);
	byte_ptr = (char *)((uintptr_t)rb + sizeof(struct ramlog_buffer));

	/* The boot firmware publishes this reserved-RAM buffer before Linux boots. */
	rmb();
	if (READ_ONCE(rb->magic) != RAMLOG_MAGIC) {
		pr_err("%s: invalid ramlog magic for boot stage %d.\n",
		       __func__, boot_id);
		return -EINVAL;
	}
	size = READ_ONCE(rb->size);
	head = READ_ONCE(rb->head);
	tail = READ_ONCE(rb->tail);
	if (size != region_size - sizeof(*rb) || size <= 1 ||
	    head >= size || tail >= size) {
		pr_err("%s: invalid ramlog metadata: size=0x%x head=0x%x tail=0x%x.\n",
		       __func__, size, head, tail);
		return -EINVAL;
	}
	valid_len = tail >= head ? tail - head : size - head + tail;

	pr_info("ramlog capacity=0x%x head=0x%x tail=0x%x valid=0x%x.\n",
		 size, head, tail, valid_len);

	for (i = 0; i < valid_len; i++) {
		char ch = byte_ptr[(head + i) % size];

		if (ch == '\n') {
			printk(KERN_CONT "\n");
		} else {
			printk(KERN_CONT "%c", ch);
		}
	}

	printk(KERN_CONT "\n");

	return 0;
}

static ssize_t hunter_boot_log_show(struct kobject *kobj, struct kobj_attribute *attr,
						   char *buf)
{
	return sprintf(buf, "%d\n", boot_id);
}

static ssize_t hunter_boot_log_store(struct kobject *kobj, struct kobj_attribute *attr,
							const char *buf, size_t count)
{
	int ret;

	ret = kstrtoint(buf, 10, &boot_id);
	if (ret < 0) {
		return ret;
	}

	if (BL1 <= boot_id && boot_id <= SBI)
		display_boot_log(BL1_SBI_LOG, boot_id);
	else if (boot_id == UBOOT)
		display_boot_log(UBOOT_LOG, boot_id);
	else
		pr_err("Invalid option %d, prefered range(%d - %d)\n",
				boot_id, BOOTROM, UBOOT);

	return count;
}

static struct kobj_attribute hunter_bootlog_attribute =
	__ATTR(hunter_boot_log, 0664, hunter_boot_log_show, hunter_boot_log_store);

/* sysfs: hunter_boot_log、log_flush */
static struct attribute *attrs[] = {
	&mcu_log_attr.attr,
	&hunter_bootlog_attribute.attr,
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = attrs,
};

static int quard_log_open(struct inode *inode, struct file *file)
{
	struct log_device *quard_log_device;

	quard_log_device = container_of(inode->i_cdev, struct log_device, cdev);

	file->private_data = quard_log_device;

	pr_info("quard_log_device opened.\n");

	return 0;
}

static int quard_log_release(struct inode *inode, struct file *file)
{
	return 0;
}

static int quard_log_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct log_device *quard_log_device = file->private_data;

	unsigned long pfn;
	unsigned long size = vma->vm_end - vma->vm_start;

	if (size > MCU_LOG_BUF_SIZE) {
		dev_err(&quard_log_device->pdev->dev, "mmap size (0x%lx) exceeds limit.\n", size);
		return -EINVAL;
	}
	switch (buffer_select) {
		case MCU_LOG_BUF_FLAG:
			pfn = virt_to_phys(quard_log_device->fw[MCU_LOG].log_buf) >> PAGE_SHIFT;
			break;
		case BL1_SBI_LOG_BUF_FLAG:
			pfn = virt_to_phys(quard_log_device->fw[BL1_SBI_LOG].log_buf) >> PAGE_SHIFT;
			break;
		case UBOOT_LOG_BUF_FLAG:
			pfn = virt_to_phys(quard_log_device->fw[UBOOT_LOG].log_buf) >> PAGE_SHIFT;
			break;
		case BUF_INIT_VALUE:
		case BUF_MAX_VALUE:
		default:
			pr_warn("Invalid buffer selection.\n");
			return -EINVAL;
	}
	vm_flags_set(vma, VM_IO);

	if (remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot)) {
		dev_err(&quard_log_device->pdev->dev, "remap_pfn_range failed.\n");
		return -EAGAIN;
	}

	return 0;
}

static long quard_log_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
	int new_buffer_select;

	switch (cmd) {
		case IOCTL_SET_BUFFER:
			if (copy_from_user(&new_buffer_select, (int32_t*)arg, sizeof(new_buffer_select))) {
				return -EFAULT;
			}
			if (new_buffer_select < BUF_INIT_VALUE || new_buffer_select > BUF_MAX_VALUE) {
				pr_warn("Invalid buffer selection: %d\n", new_buffer_select);
				return -EINVAL;
			}
			buffer_select = new_buffer_select;
			pr_info("Buffer selection set to %d\n", buffer_select);
			break;
		default:
			return -ENOTTY;
	}

	return 0;
}

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = quard_log_open,
	.release = quard_log_release,
	.mmap = quard_log_mmap,
	.unlocked_ioctl = quard_log_ioctl,
};

/* netlink: user space */
static void send_netlink_event(const char *message)
{
	struct sk_buff *skb_out;
	struct nlmsghdr *nlh;
	int msg_size = strlen(message);
	int res;

	if (soc_logd_pid == 0) {
		pr_info("soc_logd_pid = 0, soc_logd is not running.\n");
		return;
	}

	skb_out = nlmsg_new(msg_size, GFP_KERNEL);
	if (!skb_out) {
		pr_err("failed to allocate new skb\n");
		return;
	}

	nlh = nlmsg_put(skb_out, 0, 0, NLMSG_DONE, msg_size, 0);
	strncpy(nlmsg_data(nlh), message, msg_size);

	res = nlmsg_unicast(nl_sock, skb_out, soc_logd_pid);
	if (res < 0)
		pr_err("error while sending back to user pid: %d.\n", soc_logd_pid);
}

static void netlink_recv_msg(struct sk_buff *skb)
{
	struct nlmsghdr *nlh;
	char *msg;

	nlh = (struct nlmsghdr *)skb->data;
	msg = (char *)nlmsg_data(nlh);

	pr_debug("received message from user: %s\n", msg);

	soc_logd_pid = nlh->nlmsg_pid;	/* record user process pid */
	if (soc_logd_pid == 0) {
		pr_debug("received message from kernel, return.\n");
		return;
	}

	/* send ack to user process */
	send_netlink_event("initial ack from kernel");
}

static int netlink_init(void)
{
	struct netlink_kernel_cfg cfg = {
		.input = netlink_recv_msg,
	};

	nl_sock = netlink_kernel_create(&init_net, NETLINK_SOC_LOG, &cfg);
	if (!nl_sock) {
		pr_err("error creating socket.\n");
		return -ENOLINK;
	}

	pr_info("soc log netlink socket created successfully.\n");

	return 0;
}

/* ringbuffer state check */
static void buf_check_process(fw_enum index, void *data)
{
	struct mcu_log_buf *fw_log_buf = NULL;
	struct log_device *quard_log_device = (struct log_device *)data;
	char *fw = "default fw";
	char *full_msg = "default full";
	int state;

	switch (index) {
		case MCU_LOG:
			fw = "mcu";
			full_msg = MCU_LOG_FULL_MSG;
			break;
		default:
			pr_err("Wrong index(%d) in %s\n", index, __func__);
			break;
	}

	fw_log_buf = (struct mcu_log_buf *)quard_log_device->fw[index].shared_buf;

	if (soc_logd_pid == 0) {
		return ;
	}

	state = READ_ONCE(fw_log_buf->state);
	if (state != 1)
		return;
	rmb();

	memcpy(quard_log_device->fw[index].log_buf,
				fw_log_buf,
				quard_log_device->fw[index].log_buf_size);

	/* Release the shared buffer only after the local snapshot is complete. */
	wmb();
	WRITE_ONCE(fw_log_buf->state, 0);

	send_netlink_event(full_msg);
}

static int soc_klogd_fn(void *data)
{
	while (!kthread_should_stop()) {
		buf_check_process(MCU_LOG, data);
		msleep(100);
	}

	return 0;
}

void prepare_acpu_boot_log(void *log_device, fw_enum index)
{
    struct log_device *log_dev = (struct log_device *)log_device;

	rmb();
	memcpy((void *)log_dev->fw[index].log_buf,
		(void *)log_dev->fw[index].shared_buf,
        log_dev->fw[index].log_buf_size);
}

/**
 * This function is used to register fw log to probe func
 * @param pdev: platform device pointer
 * @param index: fw index which is same in dts reg attribute
 * @param data: quard_log_device pointer
 *
*/
static int register_fw_log(struct platform_device *pdev, fw_enum index, struct log_device *log_dev)
{
	struct device_node *mem_node;
	struct resource res;
	int mem_index;
	int ret;

	dev_info(&pdev->dev, "index:%d\n", index);

	mem_index = of_property_match_string(pdev->dev.of_node,
					     "memory-region-names",
					     fw_memory_region_names[index]);
	if (mem_index < 0) {
		dev_err(&pdev->dev, "memory-region '%s' is missing\n",
			fw_memory_region_names[index]);
		return mem_index;
	}

	mem_node = of_parse_phandle(pdev->dev.of_node, "memory-region", mem_index);
	if (!mem_node) {
		dev_err(&pdev->dev, "memory-region '%s' is invalid\n",
			fw_memory_region_names[index]);
		return -ENODEV;
	}

	ret = of_address_to_resource(mem_node, 0, &res);
	of_node_put(mem_node);
	if (ret) {
		dev_err(&pdev->dev, "failed to resolve memory-region '%s'\n",
			fw_memory_region_names[index]);
		return ret;
	}

	log_dev->fw[index].log_buf_size = resource_size(&res);
	if (log_dev->fw[index].log_buf_size > MCU_LOG_BUF_SIZE) {
		dev_err(&pdev->dev, "mcu log buf size exceeds limit.\n");
		return -EINVAL;
	}

	log_dev->fw[index].shared_buf = devm_memremap(&pdev->dev, res.start,
						resource_size(&res), MEMREMAP_WB);
	if (!log_dev->fw[index].shared_buf) {
		dev_err(&pdev->dev, "failed to map shared log memory[%d]\n", index);
		return -ENOMEM;
	}

	dev_info(&pdev->dev, "log shared memory[%d]: pa=%pa va=%px size=%u\n",
		 index, &res.start, log_dev->fw[index].shared_buf,
		 log_dev->fw[index].log_buf_size);

	log_dev->fw[index].log_buf = (char *)__get_free_pages(GFP_KERNEL,
						get_order(log_dev->fw[index].log_buf_size));
	if (!log_dev->fw[index].log_buf) {
		dev_err(&pdev->dev, "alloc mcu log buffer failed.");
		return -ENOMEM;
	}

	/* not need to acquire lock */

	return 0;
}

static void log_router_receive(struct device *dev, struct quard_consumer_msg *msg)
{
	pr_debug("receive log mbox router msg\n");
}

static int quard_log_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct device_node *node = pdev->dev.of_node;
	struct log_device *quard_log_device = NULL;

	if (!node) {
		dev_err(&pdev->dev, "device tree node not found.\n");
		return -ENODEV;
	}

	mcu_log_mbox = quard_mbox_consumer_register(&pdev->dev, log_router_receive);
	if (IS_ERR(mcu_log_mbox)) {
		dev_err(&pdev->dev, "failed to match log mbox router\n");
		return PTR_ERR(mcu_log_mbox);
	}

	/* extend log device */
	quard_log_device = devm_kzalloc(&pdev->dev, sizeof(*quard_log_device), GFP_KERNEL);
	if (!quard_log_device)
		return -ENOMEM;

	quard_log_device->pdev = pdev;

	log_dev = quard_log_device;

	platform_set_drvdata(pdev, quard_log_device);

	/* register char device */
	ret = alloc_chrdev_region(&quard_log_devt, 0, 1, DEVICE_NAME);
	if (ret < 0)
		goto free_log_device;

	cdev_init(&quard_log_device->cdev, &fops);
	quard_log_device->cdev.owner = THIS_MODULE;

	ret = cdev_add(&quard_log_device->cdev, quard_log_devt, 1);
	if (ret)
		goto unregister_chrdev;

	quard_log_class = class_create(CLASS_NAME);
	if (IS_ERR(quard_log_class)) {
		ret = PTR_ERR(quard_log_class);
		goto del_cdev;
	}

	quard_log_device->dev = device_create(quard_log_class, NULL, quard_log_devt, NULL, DEVICE_NAME);
	if (IS_ERR(quard_log_device->dev)) {
		ret = PTR_ERR(quard_log_device->dev);
		goto destroy_class;
	}

	ret = register_fw_log(pdev, MCU_LOG, quard_log_device);
	if(ret != 0)
		goto destroy_device;
	ret = register_fw_log(pdev, BL1_SBI_LOG, quard_log_device);
	if(ret != 0)
		goto free_mcu_log_buf;
	prepare_acpu_boot_log(quard_log_device, BL1_SBI_LOG);

	ret = register_fw_log(pdev, UBOOT_LOG, quard_log_device);
	if(ret != 0)
		goto free_bl1_sbi_log_buf;
	prepare_acpu_boot_log(quard_log_device, UBOOT_LOG);

	/* sysfs */
	soc_log_kobj = kobject_create_and_add("soc_log", kernel_kobj);
	if (!soc_log_kobj) {
		ret = -ENOMEM;
		goto free_uboot_log_buf;
	}

	ret = sysfs_create_group(soc_log_kobj, &attr_group);
	if (ret) {
		kobject_put(soc_log_kobj);
		dev_err(&pdev->dev, "Failed to create sysfs group\n");
		goto free_uboot_log_buf;
	}

	dev_info(&pdev->dev, "Driver initialized and sysfs files created.\n");

	/* create log daemon thread */
	soc_klogd_task = kthread_run(soc_klogd_fn, (void *)quard_log_device, "%s", "soc_klogd");
	if (IS_ERR(soc_klogd_task)) {
		pr_err("Failed to create kernel thread\n");
		ret = PTR_ERR(soc_klogd_task);
		goto free_uboot_log_buf;
	}

	ret = netlink_init();
	if (ret < 0)
		goto free_uboot_log_buf;

	dev_info(&pdev->dev, "log device probed successfully.\n");

	return 0;

free_uboot_log_buf:
	kobject_put(soc_log_kobj);
	free_pages((uintptr_t)quard_log_device->fw[UBOOT_LOG].log_buf,
		get_order(quard_log_device->fw[UBOOT_LOG].log_buf_size));
free_bl1_sbi_log_buf:
	free_pages((uintptr_t)quard_log_device->fw[BL1_SBI_LOG].log_buf,
		get_order(quard_log_device->fw[BL1_SBI_LOG].log_buf_size));
free_mcu_log_buf:
	free_pages((uintptr_t)quard_log_device->fw[MCU_LOG].log_buf,
		get_order(quard_log_device->fw[MCU_LOG].log_buf_size));
destroy_device:
	device_destroy(quard_log_class, quard_log_devt);
destroy_class:
	class_destroy(quard_log_class);
del_cdev:
	cdev_del(&quard_log_device->cdev);
unregister_chrdev:
	unregister_chrdev_region(quard_log_devt, 1);
free_log_device:
	devm_kfree(&pdev->dev, quard_log_device);
	quard_mbox_consumer_unregister(mcu_log_mbox);

	return ret;
}

static int quard_log_remove(struct platform_device *pdev)
{
	struct log_device *quard_log_device = platform_get_drvdata(pdev);
	if (soc_klogd_task) {
		kthread_stop(soc_klogd_task);
		soc_klogd_task = NULL;
	}

	for(fw_enum i = MCU_LOG; i < FW_MAX_INDEX; i++) {
		pr_debug("free_pages %d\n", i);
		free_pages((uintptr_t)quard_log_device->fw[(FW_MAX_INDEX - i - 1)].log_buf,
			get_order(quard_log_device->fw[(FW_MAX_INDEX - i - 1)].log_buf_size));
	}

	netlink_kernel_release(nl_sock);
	device_destroy(quard_log_class, quard_log_devt);
	class_destroy(quard_log_class);
	cdev_del(&quard_log_device->cdev);
	unregister_chrdev_region(quard_log_devt, 1);
	devm_kfree(&pdev->dev, quard_log_device);
	quard_mbox_consumer_unregister(mcu_log_mbox);

	if (soc_log_kobj) {
		kobject_put(soc_log_kobj);
		soc_log_kobj = NULL;
	}

	pr_info("mmap_char_device: unregistered\n");

	return 0;
}

static const struct of_device_id log_of_match[] = {
	{ .compatible = "quard,log_device", },
	{ /* ... */ }
};
MODULE_DEVICE_TABLE(of, log_of_match);

static struct platform_driver quard_log_driver = {
	.driver = {
		.name = DEVICE_NAME,
		.of_match_table = log_of_match,
	},
	.probe = quard_log_probe,
	.remove = quard_log_remove,
};

module_platform_driver(quard_log_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("quard tream");
MODULE_DESCRIPTION("log device driver");
