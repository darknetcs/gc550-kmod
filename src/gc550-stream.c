// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * AVerMedia Live Gamer Extreme (GC550) capture stream (bulk IN 0x83).
 *
 * GC550_URBS transfers stay in flight while anyone (video or audio) streams.
 * Completions are handed to an ordered work item that runs the parser over
 * the scatter-gather chunks in place and resubmits, so no data is copied
 * except into the destination video buffer and the ALSA ring.
 */
#include <linux/gfp.h>
#include <linux/slab.h>

#include "gc550.h"

static u8 *gc550_cb_frame_buffer(void *priv, size_t *capacity)
{
	return gc550_video_frame_buffer(priv, capacity);
}

static void gc550_cb_frame_done(void *priv, size_t bytes)
{
	gc550_video_frame_done(priv, bytes);
}

static void gc550_cb_audio(void *priv, const u8 *data, size_t len)
{
	gc550_audio_data(priv, data, len);
}

static const struct gc550_parser_ops gc550_parser_ops = {
	.frame_buffer = gc550_cb_frame_buffer,
	.frame_done = gc550_cb_frame_done,
	.audio = gc550_cb_audio,
};

static void gc550_urb_complete(struct urb *urb)
{
	struct gc550_urb *gu = urb->context;
	struct gc550_dev *dev = gu->dev;
	unsigned long flags;

	switch (urb->status) {
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
		return;		/* killed or unplugged */
	default:
		break;
	}
	spin_lock_irqsave(&dev->rx_lock, flags);
	if (dev->streaming) {
		list_add_tail(&gu->done, &dev->rx_done);
		queue_work(dev->rx_wq, &dev->rx_work);
	}
	spin_unlock_irqrestore(&dev->rx_lock, flags);
}

static void gc550_parse_urb(struct gc550_dev *dev, struct gc550_urb *gu)
{
	struct urb *urb = gu->urb;
	size_t left, n;
	unsigned int i;

	if (urb->status) {
		/* transient error: keep going, but bytes were lost */
		dev_dbg(&dev->intf->dev, "stream URB status %d\n", urb->status);
		dev->gap = true;
		dev->rx_errors++;
		return;
	}
	dev->rx_errors = 0;
	/* a short transfer means bytes were lost before it, as in lgx2userspace */
	if (dev->gap || urb->actual_length < urb->transfer_buffer_length)
		gc550_parser_reset(&dev->parser);
	dev->gap = false;

	left = urb->actual_length;
	for (i = 0; i < GC550_URB_CHUNKS && left; i++) {
		n = min_t(size_t, left, gu->sg[i].length);
		gc550_parser_feed(&dev->parser, page_address(gu->chunks[i]), n);
		left -= n;
	}
}

static void gc550_rx_work(struct work_struct *work)
{
	struct gc550_dev *dev = container_of(work, struct gc550_dev, rx_work);
	struct gc550_urb *gu;
	unsigned long flags;
	int ret;

	for (;;) {
		spin_lock_irqsave(&dev->rx_lock, flags);
		gu = list_first_entry_or_null(&dev->rx_done, struct gc550_urb, done);
		if (gu)
			list_del_init(&gu->done);
		spin_unlock_irqrestore(&dev->rx_lock, flags);
		if (!gu)
			break;

		mutex_lock(&dev->parse_lock);
		gc550_parse_urb(dev, gu);
		mutex_unlock(&dev->parse_lock);
		/* outside parse_lock: may take the PCM stream lock */
		gc550_audio_flush_periods(dev);

		spin_lock_irqsave(&dev->rx_lock, flags);
		if (dev->streaming && dev->rx_errors >= GC550_RX_MAX_ERRORS) {
			/* e.g. a stalled endpoint: stop instead of spinning */
			if (dev->rx_errors == GC550_RX_MAX_ERRORS)
				dev_err(&dev->intf->dev,
					"stream failed %u times in a row, stopped; restart capture to retry\n",
					dev->rx_errors);
			dev->rx_errors++;
		} else if (dev->streaming) {
			usb_anchor_urb(gu->urb, &dev->anchor);
			ret = usb_submit_urb(gu->urb, GFP_ATOMIC);
			if (ret) {
				usb_unanchor_urb(gu->urb);
				dev->gap = true;
				dev_err_ratelimited(&dev->intf->dev,
						    "stream resubmit failed: %d\n", ret);
			}
		}
		spin_unlock_irqrestore(&dev->rx_lock, flags);
	}
}

static void gc550_urbs_free(struct gc550_dev *dev)
{
	unsigned int i, j;

	for (i = 0; i < GC550_URBS; i++) {
		struct gc550_urb *gu = &dev->urbs[i];

		usb_free_urb(gu->urb);
		gu->urb = NULL;
		for (j = 0; j < GC550_URB_CHUNKS; j++) {
			if (gu->chunks[j])
				__free_pages(gu->chunks[j], GC550_CHUNK_ORDER);
			gu->chunks[j] = NULL;
		}
	}
	dev->urbs_allocated = false;
}

static int gc550_urbs_alloc(struct gc550_dev *dev)
{
	unsigned int i, j;
	size_t left;

	for (i = 0; i < GC550_URBS; i++) {
		struct gc550_urb *gu = &dev->urbs[i];

		gu->dev = dev;
		INIT_LIST_HEAD(&gu->done);
		gu->urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!gu->urb)
			goto fail;
		sg_init_table(gu->sg, GC550_URB_CHUNKS);
		left = GC550_URB_BYTES;
		for (j = 0; j < GC550_URB_CHUNKS; j++) {
			gu->chunks[j] = alloc_pages(GFP_KERNEL | __GFP_NOWARN, GC550_CHUNK_ORDER);
			if (!gu->chunks[j])
				goto fail;
			sg_set_page(&gu->sg[j], gu->chunks[j],
				    min_t(size_t, left, GC550_CHUNK_BYTES), 0);
			left -= gu->sg[j].length;
		}
		usb_fill_bulk_urb(gu->urb, dev->udev,
				  usb_rcvbulkpipe(dev->udev, GC550_EP_STREAM_IN),
				  NULL, GC550_URB_BYTES, gc550_urb_complete, gu);
		gu->urb->sg = gu->sg;
		gu->urb->num_sgs = GC550_URB_CHUNKS;
	}
	dev->urbs_allocated = true;
	return 0;
fail:
	gc550_urbs_free(dev);
	return -ENOMEM;
}

static void gc550_stream_stop_locked(struct gc550_dev *dev)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->rx_lock, flags);
	dev->streaming = false;
	spin_unlock_irqrestore(&dev->rx_lock, flags);

	usb_kill_anchored_urbs(&dev->anchor);
	cancel_work_sync(&dev->rx_work);
	/* a resubmit that raced with the kill above */
	usb_kill_anchored_urbs(&dev->anchor);

	spin_lock_irqsave(&dev->rx_lock, flags);
	while (!list_empty(&dev->rx_done))
		list_del_init(dev->rx_done.next);
	spin_unlock_irqrestore(&dev->rx_lock, flags);
}

static int gc550_stream_start_locked(struct gc550_dev *dev)
{
	unsigned long flags;
	unsigned int i;
	int ret;

	if (!dev->urbs_allocated) {
		ret = gc550_urbs_alloc(dev);
		if (ret)
			return ret;
	}

	mutex_lock(&dev->parse_lock);
	gc550_parser_reset(&dev->parser);
	mutex_unlock(&dev->parse_lock);
	dev->gap = false;
	dev->rx_errors = 0;

	spin_lock_irqsave(&dev->rx_lock, flags);
	dev->streaming = true;
	spin_unlock_irqrestore(&dev->rx_lock, flags);

	for (i = 0; i < GC550_URBS; i++) {
		usb_anchor_urb(dev->urbs[i].urb, &dev->anchor);
		ret = usb_submit_urb(dev->urbs[i].urb, GFP_KERNEL);
		if (ret) {
			usb_unanchor_urb(dev->urbs[i].urb);
			dev_err(&dev->intf->dev, "stream start failed: %d\n", ret);
			gc550_stream_stop_locked(dev);
			return ret;
		}
	}
	return 0;
}

int gc550_stream_get(struct gc550_dev *dev, enum gc550_user user)
{
	int ret = 0;

	mutex_lock(&dev->stream_lock);
	if (READ_ONCE(dev->disconnected)) {
		ret = -ENODEV;
	} else if (!(dev->users & user)) {
		if (!dev->users)
			ret = gc550_stream_start_locked(dev);
		if (!ret)
			dev->users |= user;
	}
	mutex_unlock(&dev->stream_lock);
	return ret;
}

void gc550_stream_put(struct gc550_dev *dev, enum gc550_user user)
{
	mutex_lock(&dev->stream_lock);
	if (dev->users & user) {
		dev->users &= ~user;
		if (!dev->users)
			gc550_stream_stop_locked(dev);
	}
	mutex_unlock(&dev->stream_lock);
}

/* Unplug: stop for everyone; later puts are no-ops apart from bookkeeping. */
void gc550_stream_disconnect(struct gc550_dev *dev)
{
	mutex_lock(&dev->stream_lock);
	if (dev->users)
		gc550_stream_stop_locked(dev);
	dev->users = 0;
	mutex_unlock(&dev->stream_lock);
}

int gc550_stream_init(struct gc550_dev *dev)
{
	if (dev->udev->bus->sg_tablesize < GC550_URB_CHUNKS) {
		dev_err(&dev->intf->dev,
			"USB host controller lacks scatter-gather support (%u entries)\n",
			dev->udev->bus->sg_tablesize);
		return -ENODEV;
	}
	mutex_init(&dev->stream_lock);
	mutex_init(&dev->parse_lock);
	spin_lock_init(&dev->rx_lock);
	INIT_LIST_HEAD(&dev->rx_done);
	init_usb_anchor(&dev->anchor);
	INIT_WORK(&dev->rx_work, gc550_rx_work);
	gc550_parser_init(&dev->parser, &gc550_parser_ops, dev);
	dev->rx_wq = alloc_ordered_workqueue("gc550-rx", WQ_HIGHPRI);
	return dev->rx_wq ? 0 : -ENOMEM;
}

void gc550_stream_cleanup(struct gc550_dev *dev)
{
	if (dev->rx_wq)
		destroy_workqueue(dev->rx_wq);
	dev->rx_wq = NULL;
	gc550_urbs_free(dev);
}
