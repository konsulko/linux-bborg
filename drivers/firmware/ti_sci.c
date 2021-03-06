/*
 * Texas Instruments System Control Interface Protocol Driver
 *
 * Copyright (C) 2015 Texas Instruments Incorporated - http://www.ti.com/
 *	Nishanth Menon
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/bitmap.h>
#include <linux/debugfs.h>
#include <linux/export.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/ti-msgmgr.h>
#include <linux/ti_sci_protocol.h>

#include "ti_sci.h"

/* List of all TI SCI devices active in system */
static LIST_HEAD(ti_sci_list);
/* Protection for the entire list */
static DEFINE_MUTEX(ti_sci_list_mutex);

/**
 * struct ti_sci_xfer - Structure representing a message flow
 * @tx_message:	Transmit message
 * @rx_len:	Receive message length
 * @xfer_buf:	Preallocated buffer to store receive message
 *		Since we work with request-ACK protocol, we can
 *		reuse the same buffer for the rx path as we
 *		use for the tx path.
 * @done:	completion event
 */
struct ti_sci_xfer {
	struct ti_msgmgr_message tx_message;
	u8 rx_len;
	u8 *xfer_buf;
	struct completion done;
};

/**
 * struct ti_sci_xfers_info - Structure to manage transfer information
 * @sem_xfer_count:	Counting Semaphore for managing max simultaneous
 *			Messages.
 * @xfer_block:		Preallocated Message array
 * @xfer_alloc_table:	Bitmap table for allocated messages.
 *			Index of this bitmap table is also used for message
 *			sequence identifier.
 * @xfer_lock:		Protection for message allocation
 */
struct ti_sci_xfers_info {
	struct semaphore sem_xfer_count;
	struct ti_sci_xfer *xfer_block;
	unsigned long *xfer_alloc_table;
	/* protect transfer allocation */
	spinlock_t xfer_lock;
};

/**
 * struct ti_sci_desc - Description of SoC integration
 * @host_id:		Host identifier representing the compute entity
 * @max_rx_timeout_ms:	Timeout for communication with SoC (in Milliseconds)
 * @max_msgs: Maximum number of messages that can be pending
 *		  simultaneously in the system
 * @max_msg_size: Maximum size of data per message that can be handled.
 */
struct ti_sci_desc {
	u8 host_id;
	int max_rx_timeout_ms;
	int max_msgs;
	int max_msg_size;
};

/**
 * struct ti_sci_info - Structure representing a TI SCI instance
 * @dev:	Device pointer
 * @desc:	SoC description for this instance
 * @d:		Debugfs file entry
 * @debug_region: Memory region where the debug message are available
 * @debug_region_size: Debug region size
 * @debug_buffer: Buffer allocated to copy debug messages.
 * @handle:	Instance of TI SCI handle to send to clients.
 * @cl:		Mailbox Client
 * @chan_tx:	Transmit mailbox channel
 * @chan_rx:	Receive mailbox channel
 * @minfo:	Message info
 * @node:	list head
 * @users:	Number of users of this instance
 */
struct ti_sci_info {
	struct device *dev;
	const struct ti_sci_desc *desc;
	struct dentry *d;
	void __iomem *debug_region;
	char *debug_buffer;
	size_t debug_region_size;
	struct ti_sci_handle handle;
	struct mbox_client cl;
	struct mbox_chan *chan_tx;
	struct mbox_chan *chan_rx;
	struct ti_sci_xfers_info minfo;
	struct list_head node;
	/* protected by ti_sci_list_mutex */
	int users;
};

#define cl_to_ti_sci_info(cl)	container_of(cl, struct ti_sci_info, cl)
#define handle_to_ti_sci_info(handle) container_of(handle, struct ti_sci_info,\
						   handle)
#ifdef CONFIG_DEBUG_FS

/**
 * ti_sci_debug_show() - Helper to dump the debug log
 * @s:	sequence file pointer
 * @unused:	unused.
 *
 * Return: 0
 */
static int ti_sci_debug_show(struct seq_file *s, void *unused)
{
	struct ti_sci_info *info = s->private;

	memcpy_fromio(info->debug_buffer, info->debug_region,
		      info->debug_region_size);
	/*
	 * XXX:
	 * 1. Can we trust firmware to leave NULL terminated last byte??
	 * 2. What do we do when log rolls over - how do we detect that and
	 *    provide messages in the right order??
	 *    TOBEFIXED: rewrite code as per final debug strategy.
	 */
	seq_puts(s, info->debug_buffer);
	return 0;
}

/**
 * ti_sci_debug_open() - debug file open
 * @inode:	inode pointer
 * @file:	file pointer
 *
 * Return: result of single_open
 */
static int ti_sci_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, ti_sci_debug_show, inode->i_private);
}

/* log file operations */
static const struct file_operations ti_sci_debug_fops = {
	.open = ti_sci_debug_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/**
 * ti_sci_debugfs_create() - Create log debug file
 * @pdev:	platform device pointer
 * @info:	Pointer to SCI entity information
 *
 * Return: 0 if all went fine, else corresponding error.
 */
static int ti_sci_debugfs_create(struct platform_device *pdev,
				 struct ti_sci_info *info)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	char debug_name[50] = "ti_sci_debug@";

	/* Debug region is optional */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   "debug_messages");
	info->debug_region = devm_ioremap_resource(dev, res);
	if (IS_ERR(info->debug_region))
		return 0;
	info->debug_region_size = res->end - res->start;

	info->debug_buffer = devm_kcalloc(dev, info->debug_region_size + 1,
					  sizeof(char), GFP_KERNEL);
	if (!info->debug_buffer)
		return -ENOMEM;
	/* Setup NULL termination */
	info->debug_buffer[info->debug_region_size] = 0;

	info->d = debugfs_create_file(strncat(debug_name, dev_name(dev),
					      sizeof(debug_name)),
				      S_IRUGO, NULL, info, &ti_sci_debug_fops);
	if (IS_ERR(info->d))
		return PTR_ERR(info->d);

	dev_dbg(dev, "Debug region => %p, size = %zu bytes, resource: %pr\n",
		info->debug_region, info->debug_region_size, res);
	return 0;
}

/**
 * ti_sci_debugfs_destroy() - clean up log debug file
 * @pdev:	platform device pointer
 * @info:	Pointer to SCI entity information
 */
static void ti_sci_debugfs_destroy(struct platform_device *pdev,
				   struct ti_sci_info *info)
{
	if (IS_ERR(info->debug_region))
		return;

	debugfs_remove(info->d);
}
#else /* CONFIG_DEBUG_FS */
static inline int ti_sci_debugfs_create(struct platform_device *dev,
					struct ti_sci_info *info)
{
	return 0;
}

static inline void ti_sci_debugfs_destroy(struct platform_device *dev,
					  struct ti_sci_info *info)
{
}
#endif /* CONFIG_DEBUG_FS */

/**
 * ti_sci_dump_header_dbg() - Helper to dump a message header.
 * @dev:	Device pointer corresponding to the SCI entity
 * @hdr:	pointer to header.
 */
static inline void ti_sci_dump_header_dbg(struct device *dev,
					  struct ti_sci_msg_hdr *hdr)
{
	dev_dbg(dev, "MSGHDR:type=0x%04x host=0x%02x seq=0x%02x flags=0x%08x\n",
		hdr->type, hdr->host, hdr->seq, hdr->flags);
}

/**
 * ti_sci_rx_callback() - mailbox client callback for receive messages
 * @cl:	client pointer
 * @m:	mailbox message
 *
 * Processes one received message to appropriate transfer information and
 * signals completion of the transfer.
 *
 * NOTE: This function will be invoked in IRQ context, hence should be
 * as optimal as possible.
 */
static void ti_sci_rx_callback(struct mbox_client *cl, void *m)
{
	struct ti_sci_info *info = cl_to_ti_sci_info(cl);
	struct device *dev = info->dev;
	struct ti_sci_xfers_info *minfo = &info->minfo;
	struct ti_msgmgr_message *mbox_msg = m;
	struct ti_sci_msg_hdr *hdr = (struct ti_sci_msg_hdr *)mbox_msg->buf;
	struct ti_sci_xfer *xfer;
	u8 xfer_id;

	xfer_id = hdr->seq;

	/*
	 * Are we even expecting this?
	 * NOTE: barriers were implicit in locks used for modifying the bitmap
	 */
	if (!test_bit(xfer_id, minfo->xfer_alloc_table)) {
		dev_err(dev, "Message for %d is not expected!\n", xfer_id);
		return;
	}

	xfer = &minfo->xfer_block[xfer_id];

	/* Is the message of valid length? */
	if (mbox_msg->len > info->desc->max_msg_size) {
		dev_err(dev, "Unable to handle %d xfer(max %d)\n",
			mbox_msg->len, info->desc->max_msg_size);
		ti_sci_dump_header_dbg(dev, hdr);
		return;
	}
	if (mbox_msg->len < xfer->rx_len) {
		dev_err(dev, "Recv xfer %d < expected %d length\n",
			mbox_msg->len, xfer->rx_len);
		ti_sci_dump_header_dbg(dev, hdr);
		return;
	}

	ti_sci_dump_header_dbg(dev, hdr);
	/* Take a copy to the rx buffer.. */
	memcpy(xfer->xfer_buf, mbox_msg->buf, xfer->rx_len);
	complete(&xfer->done);
}

/**
 * ti_sci_get_one_xfer() - Allocate one message
 * @info:	Pointer to SCI entity information
 * @msg_type:	Message type
 * @msg_flags:	Flag to set for the message
 * @tx_message_size: transmit message size
 * @rx_message_size: receive message size
 *
 * Helper function which is used by various command functions that are
 * exposed to clients of this driver for allocating a message traffic event.
 *
 * This function can sleep depending on pending requests already in the system
 * for the SCI entity. Further, this also holds a spinlock to maintain integrity
 * of internal data structures.
 *
 * Return: 0 if all went fine, else corresponding error.
 */
static struct ti_sci_xfer *ti_sci_get_one_xfer(struct ti_sci_info *info,
					       u16 msg_type, u32 msg_flags,
					       size_t tx_message_size,
					       size_t rx_message_size)
{
	struct ti_sci_xfers_info *minfo = &info->minfo;
	struct ti_sci_xfer *xfer;
	struct ti_sci_msg_hdr *hdr;
	unsigned long flags;
	unsigned long bit_pos;
	u8 xfer_id;
	int ret;
	int timeout;

	/* Ensure we have sane transfer sizes */
	if (rx_message_size > info->desc->max_msg_size ||
	    tx_message_size > info->desc->max_msg_size ||
	    rx_message_size < sizeof(*hdr) || rx_message_size < sizeof(*hdr))
		return ERR_PTR(-ERANGE);

	/*
	 * Ensure we have only controlled number of pending messages.
	 * Ideally, we might just have to wait a single message, be
	 * conservative and wait 5 times that..
	 */
	timeout = msecs_to_jiffies(info->desc->max_rx_timeout_ms) * 5;
	ret = down_timeout(&minfo->sem_xfer_count, timeout);
	if (ret < 0)
		return ERR_PTR(ret);

	/* Keep the locked section as small as possible */
	spin_lock_irqsave(&minfo->xfer_lock, flags);
	bit_pos = find_first_zero_bit(minfo->xfer_alloc_table,
				      info->desc->max_msgs);
	set_bit(bit_pos, minfo->xfer_alloc_table);
	spin_unlock_irqrestore(&minfo->xfer_lock, flags);

	/*
	 * We already ensured in probe that we can have max messages that can
	 * fit in  hdr.seq - NOTE: this improves access latencies
	 * to predictable O(1) access, BUT, it opens us to risk if
	 * remote misbehaves with corrupted message sequence responses.
	 * If that happens, we are going to be messed up anyways..
	 */
	xfer_id = (u8)bit_pos;

	xfer = &minfo->xfer_block[xfer_id];

	hdr = (struct ti_sci_msg_hdr *)xfer->tx_message.buf;
	xfer->tx_message.len = tx_message_size;
	xfer->rx_len = (u8)rx_message_size;

	reinit_completion(&xfer->done);

	hdr->seq = xfer_id;
	hdr->type = msg_type;
	hdr->host = info->desc->host_id;
	hdr->flags = msg_flags;

	return xfer;
}

/**
 * ti_sci_put_one_xfer() - Release a message
 * @minfo:	transfer info pointer
 * @xfer:	message that was reserved by ti_sci_get_one_xfer
 *
 * This holds a spinlock to maintain integrity of internal data structures.
 */
static void ti_sci_put_one_xfer(struct ti_sci_xfers_info *minfo,
				struct ti_sci_xfer *xfer)
{
	unsigned long flags;
	struct ti_sci_msg_hdr *hdr;
	u8 xfer_id;

	hdr = (struct ti_sci_msg_hdr *)xfer->tx_message.buf;
	xfer_id = hdr->seq;

	/*
	 * Keep the locked section as small as possible
	 * NOTE: we might escape with smp_mb and no lock here..
	 * but just be conservative and symmetric.
	 * */
	spin_lock_irqsave(&minfo->xfer_lock, flags);
	clear_bit(xfer_id, minfo->xfer_alloc_table);
	spin_unlock_irqrestore(&minfo->xfer_lock, flags);

	/* Increment the count for the next user to get through */
	up(&minfo->sem_xfer_count);
}

/**
 * ti_sci_do_xfer() - Do one transfer
 * @info:	Pointer to SCI entity information
 * @xfer:	Transfer to initiate and wait for response
 *
 * Return: -ETIMEDOUT in case of no response, if transmit error,
 *	   return corresponding error, else if all goes well,
 *	   return 0.
 */
static inline int ti_sci_do_xfer(struct ti_sci_info *info,
				 struct ti_sci_xfer *xfer)
{
	int ret;
	int timeout;
	struct device *dev = info->dev;

	ret = mbox_send_message(info->chan_tx, &xfer->tx_message);
	if (ret < 0)
		return ret;

	/*
	 * NOTE: we don't need the mailbox ticker to manage the transfer
	 * queueing since the protocol layer queues things by itself. So,
	 * kick it once we are done with current transmit. This forces
	 * the mailbox framework to submit next message allowing for
	 * transmission of next message to occur in parallel to processing
	 * in TISCI entity and subsequent response of the current message.
	 */
	mbox_client_txdone(info->chan_tx, 0);
	ret = 0;

	/* And we wait for the response. */
	timeout = msecs_to_jiffies(info->desc->max_rx_timeout_ms);
	if (!wait_for_completion_timeout(&xfer->done, timeout)) {
		dev_err(dev, "Mbox timedout in resp(caller: %pF)\n",
			(void *)_RET_IP_);
		ret = -ETIMEDOUT;
	}

	return ret;
}

/**
 * ti_sci_cmd_get_revision() - command to get the revision of the SCI entity
 * @info:	Pointer to SCI entity information
 *
 * Updates the SCI information in the internal data structure.
 *
 * Return: 0 if all went fine, else return appropriate error.
 */
static int ti_sci_cmd_get_revision(struct ti_sci_info *info)
{
	struct device *dev = info->dev;
	struct ti_sci_handle *handle = &info->handle;
	struct ti_sci_version_info *ver = &handle->version;
	struct ti_sci_msg_resp_version *rev_info;
	struct ti_sci_xfer *xfer;
	int ret;

	/* No need to setup flags since it is expected to respond */
	xfer = ti_sci_get_one_xfer(info, TI_SCI_MSG_VERSION,
				   0x0, sizeof(struct ti_sci_msg_hdr),
				   sizeof(*rev_info));
	if (IS_ERR(xfer)) {
		ret = PTR_ERR(xfer);
		dev_err(dev, "Message alloc failed(%d)\n", ret);
		return ret;
	}

	rev_info = (struct ti_sci_msg_resp_version *)xfer->xfer_buf;

	ret = ti_sci_do_xfer(info, xfer);
	if (ret) {
		dev_err(dev, "Mbox send fail %d\n", ret);
		goto fail;
	}

	ver->abi_major = rev_info->abi_major;
	ver->abi_minor = rev_info->abi_minor;
	ver->firmware_revision = rev_info->firmware_revision;
	strncpy(ver->firmware_description, rev_info->firmware_description,
		sizeof(ver->firmware_description));

fail:
	ti_sci_put_one_xfer(&info->minfo, xfer);
	return ret;
}

/**
 * ti_sci_get_handle() - Get the TI SCI handle for a device
 * @dev:	Pointer to device for which we want SCI handle
 *
 * NOTE: The function does not track individual clients of the framework
 * and is expected to be maintained by caller of TI SCI protocol library.
 * ti_sci_put_handle must be balanced with successful ti_sci_get_handle
 * Return: pointer to handle if successful, else:
 * -EPROBE_DEFER if the instance is not ready
 * -ENODEV if the required node handler is missing
 * -EINVAL if invalid conditions are encountered.
 */
const struct ti_sci_handle *ti_sci_get_handle(struct device *dev)
{
	struct device_node *np;
	struct device_node *ti_sci_np;
	struct list_head *p;
	struct ti_sci_handle *handle = NULL;
	struct ti_sci_info *info;

	if (!dev) {
		pr_err("I need a device pointer\n");
		return ERR_PTR(-EINVAL);
	}
	np = dev->of_node;
	if (!np) {
		dev_err(dev, "No OF information\n");
		return ERR_PTR(-EINVAL);
	}

	ti_sci_np = of_parse_phandle(np, "ti,sci", 0);
	if (!ti_sci_np) {
		dev_err(dev, "Needs a 'ti,sci' phandle\n");
		return ERR_PTR(-ENODEV);
	}

	mutex_lock(&ti_sci_list_mutex);
	list_for_each(p, &ti_sci_list) {
		info = list_entry(p, struct ti_sci_info, node);
		if (ti_sci_np == info->dev->of_node) {
			handle = &info->handle;
			info->users++;
			break;
		}
	}
	mutex_unlock(&ti_sci_list_mutex);
	of_node_put(ti_sci_np);

	if (!handle)
		return ERR_PTR(-EPROBE_DEFER);

	return handle;
}
EXPORT_SYMBOL_GPL(ti_sci_get_handle);

/**
 * ti_sci_put_handle() - Release the handle acquired by ti_sci_get_handle
 * @handle:	Handle acquired by ti_sci_get_handle
 *
 * NOTE: The function does not track individual clients of the framework
 * and is expected to be maintained by caller of TI SCI protocol library.
 * ti_sci_put_handle must be balanced with successful ti_sci_get_handle
 *
 * Return: 0 is successfully released
 * if an error pointer was passed, it returns the error value back,
 * if null was passed, it returns -EINVAL;
 */
int ti_sci_put_handle(const struct ti_sci_handle *handle)
{
	struct ti_sci_info *info;

	if (IS_ERR(handle))
		return PTR_ERR(handle);
	if (!handle)
		return -EINVAL;

	info = handle_to_ti_sci_info(handle);
	mutex_lock(&ti_sci_list_mutex);
	if (!WARN_ON(!info->users))
		info->users--;
	mutex_unlock(&ti_sci_list_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(ti_sci_put_handle);

static void devm_ti_sci_release(struct device *dev, void *res)
{
	const struct ti_sci_handle **ptr = res;
	const struct ti_sci_handle *handle = *ptr;
	int ret;

	ret = ti_sci_put_handle(handle);
	if (ret)
		dev_err(dev, "failed to put handle %d\n", ret);
}

/**
 * devm_ti_sci_get_handle() - Managed get handle
 * @dev:	device for which we want SCI handle for.
 *
 * NOTE: This releases the handle once the device resources are
 * no longer needed. MUST NOT BE released with ti_sci_put_handle.
 * The function does not track individual clients of the framework
 * and is expected to be maintained by caller of TI SCI protocol library.
 *
 * Return: 0 if all went fine, else corresponding error.
 */
const struct ti_sci_handle *devm_ti_sci_get_handle(struct device *dev)
{
	const struct ti_sci_handle **ptr;
	const struct ti_sci_handle *handle;

	ptr = devres_alloc(devm_ti_sci_release, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return ERR_PTR(-ENOMEM);
	handle = ti_sci_get_handle(dev);

	if (!IS_ERR(handle)) {
		*ptr = handle;
		devres_add(dev, ptr);
	} else {
		devres_free(ptr);
	}

	return handle;
}
EXPORT_SYMBOL_GPL(devm_ti_sci_get_handle);

/* Description for K2G */
static const struct ti_sci_desc ti_sci_pmmc_k2g_desc = {
	.host_id = 2,
	.max_rx_timeout_ms = 200,
	.max_msgs = 128,
	.max_msg_size = 64,
};

static const struct of_device_id ti_sci_of_match[] = {
	{.compatible = "ti,k2g-sci", .data = &ti_sci_pmmc_k2g_desc},
	{ /* Sentinel */ },
};
MODULE_DEVICE_TABLE(of, ti_sci_of_match);

static int ti_sci_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct of_device_id *of_id;
	const struct ti_sci_desc *desc;
	struct ti_sci_xfer *xfer;
	struct ti_sci_info *info = NULL;
	struct ti_sci_xfers_info *minfo;
	struct mbox_client *cl;
	int ret = -EINVAL;
	int i;

	of_id = of_match_device(ti_sci_of_match, dev);
	if (!of_id) {
		dev_err(dev, "OF data missing\n");
		return -EINVAL;
	}
	desc = of_id->data;

	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->dev = dev;
	info->desc = desc;
	INIT_LIST_HEAD(&info->node);
	minfo = &info->minfo;

	/*
	 * Pre-allocate messages
	 * NEVER allocate more than what we can indicate in hdr.seq
	 * if we have data description bug, force a fix..
	 */
	if (WARN_ON(desc->max_msgs >=
		    1 << 8 * sizeof(((struct ti_sci_msg_hdr *)0)->seq)))
		return -EINVAL;

	minfo->xfer_block = devm_kcalloc(dev,
					 desc->max_msgs,
					 sizeof(*minfo->xfer_block),
					 GFP_KERNEL);
	if (!minfo->xfer_block)
		return -ENOMEM;

	minfo->xfer_alloc_table = devm_kzalloc(dev,
					       BITS_TO_LONGS(desc->max_msgs)
					       * sizeof(unsigned long),
					       GFP_KERNEL);
	if (!minfo->xfer_alloc_table)
		return -ENOMEM;
	bitmap_zero(minfo->xfer_alloc_table, desc->max_msgs);

	/* Pre-initialize the buffer pointer to pre-allocated buffers */
	for (i = 0, xfer = minfo->xfer_block; i < desc->max_msgs; i++, xfer++) {
		xfer->xfer_buf = devm_kcalloc(dev, 1, desc->max_msg_size,
					      GFP_KERNEL);
		if (!xfer->xfer_buf)
			return -ENOMEM;

		xfer->tx_message.buf = xfer->xfer_buf;
		init_completion(&xfer->done);
	}

	ret = ti_sci_debugfs_create(pdev, info);
	if (ret)
		dev_warn(dev, "Failed to create debug file\n");

	platform_set_drvdata(pdev, info);

	cl = &info->cl;
	cl->dev = dev;
	cl->tx_block = false;
	cl->rx_callback = ti_sci_rx_callback;
	cl->knows_txdone = true;

	spin_lock_init(&minfo->xfer_lock);
	sema_init(&minfo->sem_xfer_count, desc->max_msgs);

	info->chan_rx = mbox_request_channel_byname(cl, "rx");
	if (IS_ERR(info->chan_rx)) {
		ret = PTR_ERR(info->chan_rx);
		goto out;
	}

	info->chan_tx = mbox_request_channel_byname(cl, "tx");
	if (IS_ERR(info->chan_tx)) {
		ret = PTR_ERR(info->chan_tx);
		goto out;
	}
	ret = ti_sci_cmd_get_revision(info);
	if (ret) {
		dev_err(dev, "Unable to communicate with TISCI(%d)\n", ret);
		goto out;
	}

	dev_info(dev, "ABI: %d.%d (firmware rev 0x%04x '%s')\n",
		 info->handle.version.abi_major, info->handle.version.abi_minor,
		 info->handle.version.firmware_revision,
		 info->handle.version.firmware_description);

	mutex_lock(&ti_sci_list_mutex);
	list_add_tail(&info->node, &ti_sci_list);
	mutex_unlock(&ti_sci_list_mutex);

	return 0;
out:
	if (!IS_ERR(info->chan_tx))
		mbox_free_channel(info->chan_tx);
	if (!IS_ERR(info->chan_rx))
		mbox_free_channel(info->chan_rx);
	debugfs_remove(info->d);
	return ret;
}

static int ti_sci_remove(struct platform_device *pdev)
{
	struct ti_sci_info *info;
	int ret = 0;

	info = platform_get_drvdata(pdev);

	mutex_lock(&ti_sci_list_mutex);
	if (info->users)
		ret = -EBUSY;
	else
		list_del(&info->node);
	mutex_unlock(&ti_sci_list_mutex);

	if (!ret)
		ti_sci_debugfs_destroy(pdev, info);

	return ret;
}

static struct platform_driver ti_sci_driver = {
	.probe = ti_sci_probe,
	.remove = ti_sci_remove,
	.driver = {
		   .name = "ti-sci",
		   .of_match_table = of_match_ptr(ti_sci_of_match),
	},
};
module_platform_driver(ti_sci_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("TI System Control Interface(SCI) driver");
MODULE_AUTHOR("Nishanth Menon");
MODULE_ALIAS("platform:ti-sci");
