// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * AVerMedia Live Gamer Extreme (GC550) USB capture driver.
 *
 * Probe only checks the interface and allocates; it sends nothing. The
 * bootstrap (every command checked by the guard first) and device
 * registration run afterwards from a work item, unless bootstrap=0.
 *
 * Based on the protocol work of lgx2userspace (MIT, Copyright 2021 Chris
 * Sawczuk and contributors).
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>

#include "gc550.h"

static bool bootstrap = true;
module_param(bootstrap, bool, 0444);
MODULE_PARM_DESC(bootstrap,
		 "Talk to the card after probe (default: true). With 0 the driver binds and checks the endpoints but sends nothing.");

#define GC550_VID	0x07CA
#define GC550_PID	0x4710

static void gc550_dev_free(struct kref *kref)
{
	struct gc550_dev *dev = container_of(kref, struct gc550_dev, kref);

	gc550_stream_cleanup(dev);
	usb_put_dev(dev->udev);
	kfree(dev->bridge_buf);
	kfree(dev);
}

void gc550_dev_get(struct gc550_dev *dev)
{
	kref_get(&dev->kref);
}

void gc550_dev_put(struct gc550_dev *dev)
{
	kref_put(&dev->kref, gc550_dev_free);
}

static void gc550_boot_work(struct work_struct *work)
{
	struct gc550_dev *dev = container_of(work, struct gc550_dev, boot_work);
	int ret;

	ret = gc550_boot(dev);
	if (ret) {
		dev_err(&dev->intf->dev, "bootstrap failed (%d); replug the card to retry\n", ret);
		return;
	}
	dev->booted = true;

	ret = gc550_v4l2_register(dev);
	if (ret)
		dev_err(&dev->intf->dev, "video device registration failed: %d\n", ret);
	ret = gc550_alsa_register(dev);
	if (ret)
		dev_err(&dev->intf->dev, "sound card registration failed: %d\n", ret);
	gc550_status_start(dev);
}

/* Exactly the card's layout: bulk 01/81 and 83 on interface 0. */
static int gc550_check_endpoints(struct usb_interface *intf)
{
	struct usb_host_interface *alt = intf->cur_altsetting;
	bool cmd_out = false, cmd_in = false, stream_in = false;
	unsigned int i;

	if (alt->desc.bInterfaceNumber != 0 ||
	    alt->desc.bInterfaceClass != USB_CLASS_VENDOR_SPEC)
		return -ENODEV;
	for (i = 0; i < alt->desc.bNumEndpoints; i++) {
		const struct usb_endpoint_descriptor *ep = &alt->endpoint[i].desc;

		if (!usb_endpoint_xfer_bulk(ep))
			continue;
		cmd_out |= ep->bEndpointAddress == GC550_EP_CMD_OUT;
		cmd_in |= ep->bEndpointAddress == GC550_EP_CMD_IN;
		stream_in |= ep->bEndpointAddress == GC550_EP_STREAM_IN;
	}
	return cmd_out && cmd_in && stream_in ? 0 : -ENODEV;
}

static int gc550_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct gc550_dev *dev;
	int ret;

	ret = gc550_check_endpoints(intf);
	if (ret) {
		dev_err(&intf->dev, "unexpected interface layout, not binding\n");
		return ret;
	}

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	kref_init(&dev->kref);
	dev->udev = usb_get_dev(interface_to_usbdev(intf));
	dev->intf = intf;
	mutex_init(&dev->bridge_lock);
	INIT_WORK(&dev->boot_work, gc550_boot_work);
	gc550_status_init(dev);
	dev->bridge_buf = kmalloc(GC550_BRIDGE_BUF, GFP_KERNEL);
	if (!dev->bridge_buf) {
		ret = -ENOMEM;
		goto fail;
	}
	ret = gc550_stream_init(dev);
	if (ret)
		goto fail;
	usb_set_intfdata(intf, dev);

	dev_info(&intf->dev, "AVerMedia GC550 (bcdDevice %04x) bound\n",
		 le16_to_cpu(dev->udev->descriptor.bcdDevice));
	if (bootstrap)
		queue_work(system_long_wq, &dev->boot_work);	/* takes seconds */
	else
		dev_info(&intf->dev, "bootstrap=0: nothing will be sent to the card\n");
	return 0;
fail:
	gc550_dev_put(dev);
	return ret;
}

static void gc550_disconnect(struct usb_interface *intf)
{
	struct gc550_dev *dev = usb_get_intfdata(intf);

	cancel_work_sync(&dev->boot_work);
	gc550_status_stop(dev);

	mutex_lock(&dev->bridge_lock);
	WRITE_ONCE(dev->disconnected, true);
	mutex_unlock(&dev->bridge_lock);

	gc550_stream_disconnect(dev);
	gc550_v4l2_unregister(dev);
	gc550_alsa_unregister(dev);
	usb_set_intfdata(intf, NULL);
	gc550_dev_put(dev);
}

static const struct usb_device_id gc550_ids[] = {
	{ USB_DEVICE_INTERFACE_NUMBER(GC550_VID, GC550_PID, 0) },
	{ }
};
MODULE_DEVICE_TABLE(usb, gc550_ids);

static struct usb_driver gc550_driver = {
	.name = GC550_DRIVER_NAME,
	.probe = gc550_probe,
	.disconnect = gc550_disconnect,
	.id_table = gc550_ids,
};
module_usb_driver(gc550_driver);

MODULE_DESCRIPTION("AVerMedia Live Gamer Extreme (GC550) HDMI capture");
MODULE_LICENSE("Dual MIT/GPL");
