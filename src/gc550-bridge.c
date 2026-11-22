// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * AVerMedia Live Gamer Extreme (GC550) command bridge.
 *
 * gc550_bridge_send() is the ONLY function in this driver that submits
 * anything to the card's command endpoint (bulk OUT 0x01), and it refuses
 * every command gc550_guard_check() does not allow. tools/check-ep-cmd.sh
 * verifies at build time that nothing else references that endpoint.
 */
#include <linux/usb.h>

#include "gc550.h"
#include "gc550-guard.h"

static int gc550_bridge_send_locked(struct gc550_dev *dev, const u8 *cmd, size_t len,
				    u8 *reply)
{
	enum gc550_guard_verdict verdict;
	size_t reply_len;
	int actual = 0;
	int ret;

	lockdep_assert_held(&dev->bridge_lock);

	verdict = gc550_guard_check(cmd, len);
	if (verdict != GC550_GUARD_OK) {
		dev_err(&dev->intf->dev, "command guard refused %*ph: %s\n",
			(int)min_t(size_t, len, 8), cmd, gc550_guard_reason(verdict));
		return -EPERM;
	}
	if (dev->disconnected)
		return -ENODEV;
	if (len > GC550_BRIDGE_BUF)
		return -EINVAL;

	/* usb_bulk_msg() needs a DMA-able buffer, so never the caller's. */
	memcpy(dev->bridge_buf, cmd, len);
	ret = usb_bulk_msg(dev->udev, usb_sndbulkpipe(dev->udev, GC550_EP_CMD_OUT),
			   dev->bridge_buf, len, &actual, GC550_BRIDGE_TIMEOUT_MS);
	if (ret)
		return ret;
	if (actual != len)
		return -EIO;

	reply_len = gc550_reply_len(cmd, len);
	if (!reply_len)
		return 0;

	/*
	 * Receive into the whole buffer: asking for exactly reply_len bytes
	 * would turn a stale, longer reply into -EOVERFLOW.
	 */
	ret = usb_bulk_msg(dev->udev, usb_rcvbulkpipe(dev->udev, GC550_EP_CMD_IN),
			   dev->bridge_buf, GC550_BRIDGE_BUF, &actual,
			   GC550_BRIDGE_TIMEOUT_MS);
	if (ret == -EPIPE)
		usb_clear_halt(dev->udev, usb_rcvbulkpipe(dev->udev, GC550_EP_CMD_IN));
	if (ret)
		return ret;
	if (actual != reply_len)
		return -EIO;
	if (reply)
		memcpy(reply, dev->bridge_buf, reply_len);
	return 0;
}

/*
 * Send one guarded command and, if it has a reply, read the reply into
 * @reply (gc550_reply_len() bytes; @reply may be NULL to discard it).
 */
int gc550_bridge_send(struct gc550_dev *dev, const u8 *cmd, size_t len, u8 *reply)
{
	int ret;

	mutex_lock(&dev->bridge_lock);
	ret = gc550_bridge_send_locked(dev, cmd, len, reply);
	mutex_unlock(&dev->bridge_lock);
	return ret;
}

int gc550_i2c_read(struct gc550_dev *dev, u8 addr, u8 reg, u8 *out, u8 count)
{
	const u8 cmd[] = { 0x02, addr, 0x01, count, reg };

	return gc550_bridge_send(dev, cmd, sizeof(cmd), out);
}

int gc550_i2c_write(struct gc550_dev *dev, u8 addr, u8 reg, u8 val)
{
	const u8 cmd[] = { 0x01, addr, 0x01, reg, val };

	return gc550_bridge_send(dev, cmd, sizeof(cmd), NULL);
}

/*
 * Discard replies left in IN 0x81 by an interrupted earlier session, so they
 * are not taken as answers to our requests. Reads only; sends nothing.
 */
void gc550_bridge_drain(struct gc550_dev *dev)
{
	int actual, i;

	mutex_lock(&dev->bridge_lock);
	for (i = 0; i < 16 && !dev->disconnected; i++) {
		if (usb_bulk_msg(dev->udev, usb_rcvbulkpipe(dev->udev, GC550_EP_CMD_IN),
				 dev->bridge_buf, GC550_BRIDGE_BUF, &actual, 10))
			break;
	}
	mutex_unlock(&dev->bridge_lock);
}
