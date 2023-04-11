#include <stddef.h>
#include <linux/slab.h>
#include <linux/delay.h>

#include "shim.h"
#include "bal_api.h"

#include "osal_ops.h"
#if defined(CONFIG_NRF700X_ON_QSPI)
#include "qspi_if.h"
#include "rpu_hw_if.h"
#elif defined(CONFIG_NRF700X_ON_USB_ADAPTER)
#include "usb_request.h"
#else
#include "spi_if.h"
#endif

#include <linux/semaphore.h>

#define MAX_BULK_PACKET_SIZE 64

#if defined(CONFIG_NRF700X_ON_USB_ADAPTER)
static struct semaphore usb_lock;
#endif

static void *lnx_shim_mem_alloc(size_t size)
{
	size = (size + 4) & 0xfffffffc;
	return kmalloc(size, GFP_KERNEL);
}

static void *lnx_shim_mem_zalloc(size_t size)
{
	size = (size + 4) & 0xfffffffc;
	return kcalloc(size, sizeof(char), GFP_KERNEL);
}

static void lnx_shim_mem_free(void *buf)
{
	return kfree(buf);
}

#if defined(CONFIG_NRF700X_ON_USB_ADAPTER)
static unsigned int lnx_shim_usb_read_reg32(void *priv, unsigned long addr)
{
	struct lnx_shim_bus_qspi_priv *qspi_priv = priv;
	int ret, retry_cnt;
	struct rpu_request *req;
	u8 *buf = NULL;
	uint32_t val;

	down(&usb_lock);

	req = kcalloc(sizeof(*req), sizeof(char), GFP_KERNEL);
	if (!req) {
		printk("%s: Unable to allocate memory\n", __func__);
		up(&usb_lock);
		return 0xFFFFFFFF;
	}
	req->cmd = REGISTER_READ;
	req->read_reg.addr = (uint32_t)addr;
	retry_cnt = 0;
retry_tx:
	retry_cnt++;
	ret = usb_control_msg(qspi_priv->usbdev,
			      usb_sndctrlpipe(qspi_priv->usbdev, 0),
			      REGISTER_READ,
			      USB_TYPE_VENDOR | USB_DIR_OUT | USB_RECIP_DEVICE, 0, 0, req,
			      sizeof(*req), 1000);
	if (ret < 0) {
		if (retry_cnt > 100) {
			printk("%s: Unable to send usb control msg: %u %d received\n", __func__, REGISTER_READ, ret);
			val = 0xFFFFFFFF;
			goto out;
		}
		msleep(10);
		goto retry_tx;
	}
	kfree(req);
	buf = kcalloc(sizeof(val), sizeof(char), GFP_KERNEL);
	retry_cnt = 0;
retry_rx:
	retry_cnt++;
	ret = usb_control_msg(qspi_priv->usbdev,
			      usb_rcvctrlpipe(qspi_priv->usbdev, 0),
			      REGISTER_READ,
			      USB_TYPE_VENDOR | USB_DIR_IN | USB_RECIP_DEVICE, 0, 0, buf,
			      sizeof(val), 1000);
	if (ret != sizeof(val)) {
		if (retry_cnt > 10000) {
			printk("%s: Unable to receive usb control msg: %u %d received\n", __func__, REGISTER_READ, ret);
			val = 0xFFFFFFFF;
			goto out;
		}
		usleep_range(100, 200);
		goto retry_rx;
	}
	val = *(uint32_t *)buf;
	//printk("rr: %lu %u", addr, val);
out:
	kfree(buf);
	up(&usb_lock);
	return val;
}

static void lnx_shim_usb_write_reg32(void *priv, unsigned long addr, unsigned int val)
{
	struct lnx_shim_bus_qspi_priv *qspi_priv = priv;
	int ret, retry_cnt = 0;
	struct rpu_request *req;

	down(&usb_lock);

	usleep_range(100, 200);
	req = kcalloc(sizeof(*req), sizeof(char), GFP_KERNEL);
	if (!req) {
		printk("%s: Unable to allocate memory\n", __func__);
		up(&usb_lock);
		return;
	}
	req->cmd = REGISTER_WRITE;
	req->write_reg.addr = (uint32_t)addr;
	req->write_reg.val = val;
	retry_cnt = 0;
retry:
	ret = usb_control_msg(qspi_priv->usbdev,
			      usb_sndctrlpipe(qspi_priv->usbdev, 0),
			      REGISTER_WRITE,
			      USB_TYPE_VENDOR | USB_DIR_OUT | USB_RECIP_DEVICE, 0, 0, req,
			      sizeof(*req), 1000);
	if (ret < 0) {
		printk("%s: Unable to send usb control msg: %u ret: %d cnt: %d\n", __func__, REGISTER_WRITE, ret, retry_cnt);
		if (retry_cnt < 100) {
			retry_cnt++;
			msleep(10);
			goto retry;
		}
	}
	kfree(req);
	up(&usb_lock);
	return;
}

static void lnx_shim_usb_cpy_from(void *priv, void *dest, unsigned long addr, size_t count)
{
	struct lnx_shim_bus_qspi_priv *qspi_priv = priv;
	struct rpu_request *req;
	int ret, actual_length, offset, retry_cnt;
	void *buf;

	if (count % 4 != 0) {
		count = (count + 4) & 0xfffffffc;
	}

	down(&usb_lock);

	usleep_range(100, 200);
	//TO check: is destination allocated as aligned count?
	req = kcalloc(sizeof(*req), sizeof(char), GFP_KERNEL);
	if (!req) {
		printk("%s: Unable to allocate memory\n", __func__);
		up(&usb_lock);
		return;
	}
	req->cmd = BLOCK_READ;
	req->read_block.addr = (uint32_t)addr;
	req->read_block.count = (int32_t)count;
	ret = usb_bulk_msg(qspi_priv->usbdev, usb_sndbulkpipe(qspi_priv->usbdev, 1), req, sizeof(*req), &actual_length, 1000);
	if (ret || (actual_length != sizeof(*req))) {
		printk("%s: Unable to send usb bulk msg: %d\n", __func__, ret);
	}
	kfree(req);
	buf = kcalloc((int32_t)count, sizeof(char), GFP_KERNEL);
	if (!buf) {
		printk("%s: Unable to allocate memory\n", __func__);
		up(&usb_lock);
		return;
	}
	offset = 0;
	retry_cnt = 0;
	while (count - offset > 0) {
		ret = usb_bulk_msg(qspi_priv->usbdev, usb_rcvbulkpipe(qspi_priv->usbdev, 1), buf + offset, MAX_BULK_PACKET_SIZE, &actual_length, 1000);
		if (ret) {
			retry_cnt++;
			if(retry_cnt > 100) {
				printk("%s %u %d actual_length: %u\n", __func__, __LINE__, ret, actual_length);
				goto out;
			}
		}
		offset += actual_length;
	}
	memcpy(dest, buf, count);
out:
	kfree(buf);
	up(&usb_lock);
}

static void lnx_shim_usb_cpy_to(void *priv, unsigned long addr, const void *src, size_t count)
{
	struct lnx_shim_bus_qspi_priv *qspi_priv = priv;
	struct rpu_request req;
	int ret, actual_length, retry_cnt;
	void *buf;
	uint32_t offset = 0;

	if (count % 4 != 0) {
		count = (count + 4) & 0xfffffffc;
	}
	//printk(KERN_DEBUG "%s: %lu count: %zu\n", __func__, addr, count);

	down(&usb_lock);

	// Add some delay to not flood the MCU while patching
	usleep_range(100, 200);
	buf = kcalloc(MAX_BULK_PACKET_SIZE, sizeof(char), GFP_KERNEL);
	if (!buf) {
		printk("%s: Unable to allocate memory\n", __func__);
		up(&usb_lock);
		return;
	}
	req.cmd = BLOCK_WRITE;

	retry_cnt = 0;
	while ((int32_t)count - offset > 0) {
		req.write_block.addr = (uint32_t)addr + offset;
		req.write_block.count = (((int32_t)count - offset) > (MAX_BULK_PACKET_SIZE - sizeof(req)))?(MAX_BULK_PACKET_SIZE - sizeof(req)):((int32_t)count - offset);
		memcpy(buf, &req, sizeof(req));
		memcpy(buf + sizeof(req), src + offset, req.write_block.count);
		ret = usb_bulk_msg(qspi_priv->usbdev, usb_sndbulkpipe(qspi_priv->usbdev, 1), buf, sizeof(req) + req.write_block.count, &actual_length, 1000);
		if (ret || (actual_length != sizeof(req) + req.write_block.count)) {
			retry_cnt++;
			if(retry_cnt > 100) {
				printk("%s: Unable to send usb bulk msg: %d\n", __func__, ret);
				goto out;
			}
		}
		offset += req.write_block.count;
	}
out:
	kfree(buf);
	up(&usb_lock);
}
#endif
static void *lnx_shim_spinlock_alloc(void)
{
	struct semaphore *lock;

	lock = kmalloc(sizeof(*lock), GFP_KERNEL);
	if (!lock) {
		printk("%s: Unable to allocate memory for spinlock\n", __func__);
	}

	return lock;
}

static void lnx_shim_spinlock_free(void *lock)
{
	kfree(lock);
}

static void lnx_shim_spinlock_init(void *lock)
{
	sema_init((struct semaphore *)lock, 1);
}

static void lnx_shim_spinlock_take(void *lock)
{
	down((struct semaphore *)lock);
}

static void lnx_shim_spinlock_rel(void *lock)
{
	up((struct semaphore *)lock);
}

static void lnx_shim_spinlock_irq_take(void *lock, unsigned long *flags)
{
	down((struct semaphore *)lock);
}

static void lnx_shim_spinlock_irq_rel(void *lock, unsigned long *flags)
{
	up((struct semaphore *)lock);
}

static int lnx_shim_pr_dbg(const char *fmt, va_list args)
{
	char buf[80];

	vsnprintf(buf, sizeof(buf), fmt, args);

	printk(KERN_DEBUG "%s\n", buf);

	return 0;
}

static int lnx_shim_pr_info(const char *fmt, va_list args)
{
	char buf[80];

	vsnprintf(buf, sizeof(buf), fmt, args);

	printk(KERN_INFO "%s\n", buf);

	return 0;
}

static int lnx_shim_pr_err(const char *fmt, va_list args)
{
	char buf[256];

	vsnprintf(buf, sizeof(buf), fmt, args);

	printk(KERN_ERR "%s\n", buf);

	return 0;
}

static void *lnx_shim_nbuf_alloc(unsigned int size)
{
	struct nwb *nwb;

	nwb = (struct nwb *)kcalloc(sizeof(struct nwb), sizeof(char), GFP_KERNEL);

	if (!nwb)
		return NULL;

	nwb->priv = kcalloc(size, sizeof(char), GFP_KERNEL);

	if (!nwb->priv) {
		kfree(nwb);
		return NULL;
	}

	nwb->data = (unsigned char *)nwb->priv;
	nwb->tail = nwb->data;
	nwb->len = 0;
	nwb->headroom = 0;
	nwb->next = NULL;

	return nwb;
}

static void lnx_shim_nbuf_free(void *nbuf)
{
	struct nwb *nwb;

	nwb = nbuf;

	kfree(((struct nwb *)nbuf)->priv);

	kfree(nbuf);
}

static void lnx_shim_nbuf_headroom_res(void *nbuf, unsigned int size)
{
	struct nwb *nwb = (struct nwb *)nbuf;

	nwb->data += size;
	nwb->tail += size;
	nwb->headroom += size;
}

static unsigned int lnx_shim_nbuf_headroom_get(void *nbuf)
{
	return ((struct nwb *)nbuf)->headroom;
}

static unsigned int lnx_shim_nbuf_data_size(void *nbuf)
{
	return ((struct nwb *)nbuf)->len;
}

static void *lnx_shim_nbuf_data_get(void *nbuf)
{
	return ((struct nwb *)nbuf)->data;
}

static void *lnx_shim_nbuf_data_put(void *nbuf, unsigned int size)
{
	struct nwb *nwb = (struct nwb *)nbuf;
	unsigned char *data = nwb->tail;

	nwb->tail += size;
	nwb->len += size;

	return data;
}

static void *lnx_shim_nbuf_data_push(void *nbuf, unsigned int size)
{
	struct nwb *nwb = (struct nwb *)nbuf;

	nwb->data -= size;
	nwb->headroom -= size;
	nwb->len += size;

	return nwb->data;
}

static void *lnx_shim_nbuf_data_pull(void *nbuf, unsigned int size)
{
	struct nwb *nwb = (struct nwb *)nbuf;

	nwb->data += size;
	nwb->headroom += size;
	nwb->len -= size;

	return nwb->data;
}

void *net_pkt_to_nbuf(struct sk_buff *skb)
{
	struct nwb *nwb;

	unsigned char *data;
	unsigned int len;

	len = skb->len;

	nwb = lnx_shim_nbuf_alloc(len + 100);

	if (!nwb) {
		printk("%s: Fail to allocate nwb\n", __func__);
		return NULL;
	}

	lnx_shim_nbuf_headroom_res(nwb, 100);

	data = lnx_shim_nbuf_data_put(nwb, len);
	memcpy(data, skb->data, len);

	return nwb;
}

static void *lnx_shim_llist_node_alloc(void)
{
	struct lnx_shim_llist_node *llist_node = NULL;

	llist_node = kcalloc(sizeof(*llist_node), sizeof(char), GFP_KERNEL);

	if (!llist_node) {
		printk("%s: Unable to allocate memory for linked list node\n", __func__);
		return NULL;
	}

	INIT_LIST_HEAD(&llist_node->list);

	return llist_node;
}

static void lnx_shim_llist_node_free(void *llist_node)
{
	kfree(llist_node);
}

static void *lnx_shim_llist_node_data_get(void *llist_node)
{
	struct lnx_shim_llist_node *lnx_llist_node = NULL;

	lnx_llist_node = (struct lnx_shim_llist_node *)llist_node;

	return lnx_llist_node->data;
}

static void lnx_shim_llist_node_data_set(void *llist_node, void *data)
{
	struct lnx_shim_llist_node *lnx_llist_node = NULL;

	lnx_llist_node = (struct lnx_shim_llist_node *)llist_node;

	lnx_llist_node->data = data;
}

static void *lnx_shim_llist_alloc(void)
{
	struct lnx_shim_llist *llist = NULL;

	llist = kcalloc(sizeof(*llist), sizeof(char), GFP_KERNEL);
	if (!llist) {
		printk("%s: Unable to allocate memory for linked list\n", __func__);
	}

	return llist;
}

static void lnx_shim_llist_free(void *llist)
{
	kfree(llist);
}

static void lnx_shim_llist_init(void *llist)
{
	struct lnx_shim_llist *lnx_llist = NULL;

	lnx_llist = (struct lnx_shim_llist *)llist;
	INIT_LIST_HEAD(&lnx_llist->list);
	lnx_llist->len = 0;
}

static void lnx_shim_llist_add_node_tail(void *llist, void *llist_node)
{
	struct lnx_shim_llist *lnx_llist = NULL;
	struct lnx_shim_llist_node *lnx_node = NULL;

	lnx_llist = (struct lnx_shim_llist *)llist;
	lnx_node = (struct lnx_shim_llist_node *)llist_node;

	list_add_tail(&lnx_node->list, &lnx_llist->list);
	lnx_llist->len += 1;
}

static void *lnx_shim_llist_get_node_head(void *llist)
{
	struct lnx_shim_llist_node *lnx_head_node = NULL;
	struct lnx_shim_llist *lnx_llist = NULL;

	lnx_llist = (struct lnx_shim_llist *)llist;

	if (!lnx_llist->len) {
		return NULL;
	}

	lnx_head_node = (struct lnx_shim_llist_node *)container_of(lnx_llist->list.next,
								       struct lnx_shim_llist_node,
								       list);

	return lnx_head_node;
}

static void *lnx_shim_llist_get_node_nxt(void *llist, void *llist_node)
{
	struct lnx_shim_llist_node *lnx_node = NULL;
	struct lnx_shim_llist_node *lnx_nxt_node = NULL;
	struct lnx_shim_llist *lnx_llist = NULL;

	lnx_llist = (struct lnx_shim_llist *)llist;
	lnx_node = (struct lnx_shim_llist_node *)llist_node;

	if (lnx_node->list.next == &lnx_llist->list)
		return NULL;

	lnx_nxt_node = (struct lnx_shim_llist_node *)container_of(lnx_node->list.next,
								      struct lnx_shim_llist_node,
								      list);

	return lnx_nxt_node;
}

static void lnx_shim_llist_del_node(void *llist, void *llist_node)
{
	struct lnx_shim_llist_node *lnx_node = NULL;
	struct lnx_shim_llist *lnx_llist = NULL;

	lnx_llist = (struct lnx_shim_llist *)llist;
	lnx_node = (struct lnx_shim_llist_node *)llist_node;

	list_del(&lnx_node->list);
	lnx_llist->len -= 1;
}

static unsigned int lnx_shim_llist_len(void *llist)
{
	struct lnx_shim_llist *lnx_llist = NULL;

	lnx_llist = (struct lnx_shim_llist *)llist;

	return lnx_llist->len;
}

static void *lnx_shim_work_alloc(void)
{
	struct work_item *item = NULL;

	item = kcalloc(sizeof(*item), sizeof(char), GFP_KERNEL);

	if (!item) {
		printk("%s: Unable to allocate memory for work\n", __func__);
		goto out;
	}
out:
	return item;
}

static void lnx_shim_work_free(void *item)
{
	return kfree(item);
}

static void fn_worker(struct work_struct *worker)
{
	struct work_item *item_ctx;

    item_ctx = container_of(worker, struct work_item, work);
	item_ctx->callback(item_ctx->data);
}

static void lnx_shim_work_init(void *item, void (*callback)(unsigned long data),
				  unsigned long data)
{
	struct work_item *item_ctx = NULL;

	item_ctx = item;
	item_ctx->data = data;
	item_ctx->callback = callback;
	INIT_WORK(&item_ctx->work, fn_worker);
}

static void lnx_shim_work_schedule(void *item)
{
	struct work_item *item_ctx = NULL;

	item_ctx = item;
	schedule_work(&item_ctx->work);
}

static void lnx_shim_work_kill(void *item)
{
	struct work_item *item_ctx = NULL;

	item_ctx = item;
	cancel_work_sync(&item_ctx->work);
}

static int lnx_shim_msleep(int msecs)
{
	msleep((unsigned int)msecs);

	return 0;
}

static int lnx_shim_usleep(int usecs)
{
	usleep_range((unsigned int)usecs, (unsigned int)(usecs * 2));

	return 0;
}

static unsigned long lnx_shim_time_get_curr_us(void)
{
	return ktime_to_us(ktime_get_boottime());
}

static unsigned int lnx_shim_time_elapsed_us(unsigned long start_time_us)
{
	unsigned long curr_time_us = 0;

	curr_time_us = lnx_shim_time_get_curr_us();

	return curr_time_us - start_time_us;
}

static enum wifi_nrf_status lnx_shim_bus_qspi_dev_init(void *os_qspi_dev_ctx)
{
	enum wifi_nrf_status status = WIFI_NRF_STATUS_FAIL;
	struct qspi_dev *dev = NULL;

	dev = os_qspi_dev_ctx;

	status = WIFI_NRF_STATUS_SUCCESS;

	return status;
}

static void lnx_shim_bus_qspi_dev_deinit(void *os_qspi_dev_ctx)
{
	struct qspi_dev *dev = NULL;

	dev = os_qspi_dev_ctx;
}

#if defined(CONFIG_NRF700X_ON_USB_ADAPTER)
static void *lnx_shim_bus_usb_init(void)
{
	struct lnx_shim_bus_qspi_priv *qspi_priv = NULL;

	sema_init(&usb_lock, 1);

	qspi_priv = kcalloc(sizeof(*qspi_priv), sizeof(char), GFP_KERNEL);

	if (!qspi_priv) {
		printk("%s: Unable to allocate memory for qspi_priv\n", __func__);
		goto out;
	}
out:
	return qspi_priv;
}

static void lnx_shim_bus_usb_deinit(void *os_qspi_priv)
{
	struct lnx_shim_bus_qspi_priv *qspi_priv = NULL;

	qspi_priv = os_qspi_priv;

	kfree(qspi_priv);
}

static void *lnx_shim_bus_usb_dev_add(void *os_qspi_priv, void *osal_qspi_dev_ctx)
{
	struct lnx_shim_bus_qspi_priv *lnx_qspi_priv = NULL;
	u32 size;
	int ret;

	lnx_qspi_priv = os_qspi_priv;

	/* Send control msg to add Wi-Fi device */
	ret = usb_control_msg(lnx_qspi_priv->usbdev,
			      usb_sndctrlpipe(lnx_qspi_priv->usbdev, 0),
			      RPU_ENABLE,
			      USB_TYPE_VENDOR | USB_DIR_OUT | USB_RECIP_DEVICE, 0, 0, NULL,
			      0, 1000);
	if (size < 0) {
		printk("%s: Unable to send usb control msg: %u\n", __func__, RPU_ENABLE);
		return NULL;
	}

	ret = usb_control_msg(lnx_qspi_priv->usbdev,
			      usb_sndctrlpipe(lnx_qspi_priv->usbdev, 0),
			      IRQ_ENABLE,
			      USB_TYPE_VENDOR | USB_DIR_OUT | USB_RECIP_DEVICE, 0, 0, NULL,
			      0, 1000);
	if (size < 0) {
		printk("%s: Unable to send usb control msg: %u\n", __func__, IRQ_ENABLE);
		return NULL;
	}

	lnx_qspi_priv->dev_added = true;

	return lnx_qspi_priv;
}

static void lnx_shim_bus_usb_dev_rem(void *os_qspi_dev_ctx)
{
	struct qspi_dev *dev = NULL;

	dev = os_qspi_dev_ctx;

	/* Send control msg to remove Wi-Fi device */

	/* TODO: Make qspi_dev a dynamic instance and remove it here */
}

#endif

static void zep_shim_bus_qspi_dev_host_map_get(void *os_qspi_dev_ctx,
					       struct wifi_nrf_osal_host_map *host_map)
{
	if (!os_qspi_dev_ctx || !host_map) {
		printk("%s: Invalid parameters\n", __func__);
		return;
	}

	host_map->addr = 0;
}

static void irq_work_handler(struct work_struct* work)
{
	struct lnx_shim_intr_priv *intr_priv = NULL;
	int ret = 0;

	intr_priv =
		(struct lnx_shim_intr_priv *)container_of(work, struct lnx_shim_intr_priv, work);

	if(!intr_priv) {
		printk("fail to get back intr priv\n");
	}

	ret = intr_priv->callbk_fn(intr_priv->callbk_data);
	if (ret) {
		printk("%s: Interrupt callback failed\n", __func__);
	}
}

static void int_complete(struct urb *urb)
{
	struct lnx_shim_bus_qspi_priv *lnx_qspi_priv = (struct lnx_shim_bus_qspi_priv *)urb->context;
	int ret;

	if (urb->status != 0 || !urb->actual_length) {
		printk("int urb fail: %d %d\n", ret, urb->actual_length);
		return;
	}

	schedule_work(&lnx_qspi_priv->intr_priv.work);

	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret) {
		printk("Re usb_submit_urb fail: %d\n", ret);
	}
}

static enum wifi_nrf_status lnx_shim_bus_qspi_intr_reg(void *os_dev_ctx, void *callbk_data,
						       int (*callbk_fn)(void *callbk_data))
{
	struct lnx_shim_bus_qspi_priv *lnx_qspi_priv = NULL;
	int ret;

	lnx_qspi_priv = os_dev_ctx;
	lnx_qspi_priv->urb = usb_alloc_urb(0, GFP_KERNEL);

	if (!lnx_qspi_priv->urb)
		return WIFI_NRF_STATUS_FAIL;

	usb_fill_int_urb(lnx_qspi_priv->urb, lnx_qspi_priv->usbdev, usb_rcvintpipe(lnx_qspi_priv->usbdev, 2),
			 lnx_qspi_priv->int_buf, USB_INTR_CONTENT_LENGTH,
			 int_complete, lnx_qspi_priv, 10);
	ret = usb_submit_urb(lnx_qspi_priv->urb, GFP_KERNEL);
	if (ret) {
		printk("usb_submit_urb fail: %d\n", ret);
		goto error;
	}

	lnx_qspi_priv->intr_priv.callbk_data = callbk_data;
	lnx_qspi_priv->intr_priv.callbk_fn = callbk_fn;
	INIT_WORK(&lnx_qspi_priv->intr_priv.work, irq_work_handler);

	return WIFI_NRF_STATUS_SUCCESS;

error:
	usb_free_urb(lnx_qspi_priv->urb);
	return WIFI_NRF_STATUS_FAIL;
}

static void lnx_shim_bus_qspi_intr_unreg(void *os_qspi_dev_ctx)
{
	struct lnx_shim_bus_qspi_priv *lnx_qspi_priv = NULL;

	lnx_qspi_priv = os_qspi_dev_ctx;
	usb_free_urb(lnx_qspi_priv->urb);
}

#ifdef CONFIG_NRF_WIFI_LOW_POWER
static void *zep_shim_timer_alloc(void)
{
	struct timer_list *timer = NULL;

	timer = kmalloc(sizeof(*timer), GFP_KERNEL);

	if (!timer)
		LOG_ERR("%s: Unable to allocate memory for work\n", __func__);

	return timer;
}

static void zep_shim_timer_init(void *timer, void (*callback)(unsigned long), unsigned long data)
{
	((struct timer_list *)timer)->function = callback;
	((struct timer_list *)timer)->data = data;

	init_timer(timer);
}

static void zep_shim_timer_free(void *timer)
{
	kfree(timer);
}

static void zep_shim_timer_schedule(void *timer, unsigned long duration)
{
	mod_timer(timer, duration);
}

static void zep_shim_timer_kill(void *timer)
{
	del_timer_sync(timer);
}
#endif /* CONFIG_NRF_WIFI_LOW_POWER */

static const struct wifi_nrf_osal_ops wifi_nrf_os_lnx_ops = {
	.mem_alloc = lnx_shim_mem_alloc,
	.mem_zalloc = lnx_shim_mem_zalloc,
	.mem_free = lnx_shim_mem_free,
	.mem_cpy = memcpy,
	.mem_set = memset,
#if defined(CONFIG_NRF700X_ON_QSPI)
	.qspi_read_reg32 = lnx_shim_qspi_read_reg32,
	.qspi_write_reg32 = lnx_shim_qspi_write_reg32,
	.qspi_cpy_from = lnx_shim_qspi_cpy_from,
	.qspi_cpy_to = lnx_shim_qspi_cpy_to,
#elif defined(CONFIG_NRF700X_ON_USB_ADAPTER)
	.qspi_read_reg32 = lnx_shim_usb_read_reg32,
	.qspi_write_reg32 = lnx_shim_usb_write_reg32,
	.qspi_cpy_from = lnx_shim_usb_cpy_from,
	.qspi_cpy_to = lnx_shim_usb_cpy_to,
#endif
	.spinlock_alloc = lnx_shim_spinlock_alloc,
	.spinlock_free = lnx_shim_spinlock_free,
	.spinlock_init = lnx_shim_spinlock_init,
	.spinlock_take = lnx_shim_spinlock_take,
	.spinlock_rel = lnx_shim_spinlock_rel,

	.spinlock_irq_take = lnx_shim_spinlock_irq_take,
	.spinlock_irq_rel = lnx_shim_spinlock_irq_rel,

	.log_dbg = lnx_shim_pr_dbg,
	.log_info = lnx_shim_pr_info,
	.log_err = lnx_shim_pr_err,

	.llist_node_alloc = lnx_shim_llist_node_alloc,
	.llist_node_free = lnx_shim_llist_node_free,
	.llist_node_data_get = lnx_shim_llist_node_data_get,
	.llist_node_data_set = lnx_shim_llist_node_data_set,
	.llist_alloc = lnx_shim_llist_alloc,
	.llist_free = lnx_shim_llist_free,
	.llist_init = lnx_shim_llist_init,

	.llist_add_node_tail = lnx_shim_llist_add_node_tail,
	.llist_get_node_head = lnx_shim_llist_get_node_head,
	.llist_get_node_nxt = lnx_shim_llist_get_node_nxt,
	.llist_del_node = lnx_shim_llist_del_node,
	.llist_len = lnx_shim_llist_len,

	.nbuf_alloc = lnx_shim_nbuf_alloc,
	.nbuf_free = lnx_shim_nbuf_free,
	.nbuf_headroom_res = lnx_shim_nbuf_headroom_res,
	.nbuf_headroom_get = lnx_shim_nbuf_headroom_get,
	.nbuf_data_size = lnx_shim_nbuf_data_size,
	.nbuf_data_get = lnx_shim_nbuf_data_get,
	.nbuf_data_put = lnx_shim_nbuf_data_put,
	.nbuf_data_push = lnx_shim_nbuf_data_push,
	.nbuf_data_pull = lnx_shim_nbuf_data_pull,

	.tasklet_alloc = lnx_shim_work_alloc,
	.tasklet_free = lnx_shim_work_free,
	.tasklet_init = lnx_shim_work_init,
	.tasklet_schedule = lnx_shim_work_schedule,
	.tasklet_kill = lnx_shim_work_kill,

	.sleep_ms = lnx_shim_msleep,
	.delay_us = lnx_shim_usleep,

	.time_get_curr_us = lnx_shim_time_get_curr_us,
	.time_elapsed_us = lnx_shim_time_elapsed_us,

#if defined(CONFIG_NRF700X_ON_QSPI)
	.bus_qspi_init = lnx_shim_bus_qspi_init,
	.bus_qspi_deinit = lnx_shim_bus_qspi_deinit,
	.bus_qspi_dev_add = lnx_shim_bus_qspi_dev_add,
	.bus_qspi_dev_rem = lnx_shim_bus_qspi_dev_rem,
#elif defined(CONFIG_NRF700X_ON_USB_ADAPTER)
	.bus_qspi_init = lnx_shim_bus_usb_init,
	.bus_qspi_deinit = lnx_shim_bus_usb_deinit,
	.bus_qspi_dev_add = lnx_shim_bus_usb_dev_add,
	.bus_qspi_dev_rem = lnx_shim_bus_usb_dev_rem,
#else
#endif
	.bus_qspi_dev_init = lnx_shim_bus_qspi_dev_init,
	.bus_qspi_dev_deinit = lnx_shim_bus_qspi_dev_deinit,
	.bus_qspi_dev_intr_reg = lnx_shim_bus_qspi_intr_reg,
	.bus_qspi_dev_intr_unreg = lnx_shim_bus_qspi_intr_unreg,
	.bus_qspi_dev_host_map_get = zep_shim_bus_qspi_dev_host_map_get,
#ifdef CONFIG_NRF_WIFI_LOW_POWER
	.timer_alloc = zep_shim_timer_alloc,
	.timer_init = zep_shim_timer_init,
	.timer_free = zep_shim_timer_free,
	.timer_schedule = zep_shim_timer_schedule,
	.timer_kill = zep_shim_timer_kill,

	.bus_qspi_ps_sleep = zep_shim_bus_qspi_ps_sleep,
	.bus_qspi_ps_wake = zep_shim_bus_qspi_ps_wake,
	.bus_qspi_ps_status = zep_shim_bus_qspi_ps_status,
#endif /* CONFIG_NRF_WIFI_LOW_POWER */
};

const struct wifi_nrf_osal_ops *get_os_ops(void)
{
	return &wifi_nrf_os_lnx_ops;
}
