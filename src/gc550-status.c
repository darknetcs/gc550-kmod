// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * AVerMedia Live Gamer Extreme (GC550) input status poll.
 *
 * Reads the same ADV7604 registers as lgx2userspace's StatusMonitor about
 * once a second, through the guarded bridge, and reports changes.
 */
#include "gc550.h"

#define GC550_STATUS_INTERVAL	msecs_to_jiffies(1000)
#define GC550_STATUS_MAX_FAILS	5

int gc550_status_read(struct gc550_dev *dev, struct gc550_signal *s)
{
	struct gc550_signal_regs r = {};
	int ret;

	ret = gc550_i2c_read(dev, GC550_ADV_IO, 0x0C, &r.io0c, 1) ?:
	      gc550_i2c_read(dev, GC550_ADV_IO, 0x12, &r.io12, 1) ?:
	      gc550_i2c_read(dev, GC550_ADV_IO, 0x6A, &r.io6a, 1) ?:
	      gc550_i2c_read(dev, GC550_ADV_IO, 0x6F, &r.io6f, 1) ?:
	      gc550_i2c_read(dev, GC550_ADV_CP, 0xB1, &r.cpb1, 1) ?:
	      gc550_i2c_read(dev, GC550_ADV_IO, 0x02, &r.io02, 1) ?:
	      gc550_i2c_read(dev, GC550_ADV_IO, 0x60, &r.io60, 1) ?:
	      gc550_i2c_read(dev, GC550_ADV_INFOFRAME, 0x01, &r.avi01, 1) ?:
	      gc550_i2c_read(dev, GC550_ADV_HDMI, 0x05, r.hdmi05, sizeof(r.hdmi05)) ?:
	      gc550_i2c_read(dev, GC550_ADV_HDMI, 0x20, r.hdmi20, sizeof(r.hdmi20)) ?:
	      gc550_i2c_read(dev, GC550_ADV_HDMI, 0x2A, r.hdmi2a, sizeof(r.hdmi2a)) ?:
	      gc550_i2c_read(dev, GC550_ADV_HDMI, 0x3B, &r.hdmi3b, 1);
	if (ret)
		return ret;
	gc550_signal_decode(&r, s);
	return 0;
}

static void gc550_status_log(struct gc550_dev *dev, const struct gc550_signal *s)
{
	struct device *d = &dev->intf->dev;
	u32 mhz;

	if (!s->cable) {
		dev_info(d, "HDMI input: no cable\n");
	} else if (s->hdcp) {
		dev_info(d, "HDMI input: HDCP-protected source, not captured\n");
	} else if (!gc550_signal_present(s)) {
		dev_info(d, "HDMI input: cable connected, no signal\n");
	} else {
		mhz = gc550_signal_refresh_mhz(s);
		dev_info(d, "HDMI input: %ux%u%c @ %u.%02u Hz (%s)\n", s->width, s->height,
			 s->interlaced ? 'i' : 'p', mhz / 1000, (mhz % 1000) / 10,
			 s->hdmi ? "HDMI" : "DVI");
	}
}

static void gc550_status_work(struct work_struct *work)
{
	struct gc550_dev *dev = container_of(work, struct gc550_dev, status_work.work);
	struct gc550_signal s;
	bool changed;
	int ret;

	ret = gc550_status_read(dev, &s);
	if (ret == -ENODEV)
		return;
	if (ret) {
		/* a late reply would be taken as the next answer: flush it */
		gc550_bridge_drain(dev);
		if (++dev->status_failures == GC550_STATUS_MAX_FAILS) {
			dev_err(&dev->intf->dev, "status poll failed %u times (%d), stopping it\n",
				dev->status_failures, ret);
			mutex_lock(&dev->status_lock);
			dev->signal_valid = false;
			mutex_unlock(&dev->status_lock);
			return;
		}
	} else {
		dev->status_failures = 0;
		mutex_lock(&dev->status_lock);
		changed = !dev->signal_valid || gc550_signal_changed(&s, &dev->signal);
		dev->signal = s;
		dev->signal_valid = true;
		mutex_unlock(&dev->status_lock);
		if (changed) {
			gc550_status_log(dev, &s);
			gc550_v4l2_source_change(dev);
		}
	}
	schedule_delayed_work(&dev->status_work, GC550_STATUS_INTERVAL);
}

void gc550_status_start(struct gc550_dev *dev)
{
	dev->status_failures = 0;
	schedule_delayed_work(&dev->status_work, 0);
}

void gc550_status_stop(struct gc550_dev *dev)
{
	cancel_delayed_work_sync(&dev->status_work);
}

/* HDCP-protected content is never delivered: such frames are blanked. */
bool gc550_input_protected(struct gc550_dev *dev)
{
	bool protected;

	mutex_lock(&dev->status_lock);
	protected = dev->signal_valid && dev->signal.cable && dev->signal.hdcp;
	mutex_unlock(&dev->status_lock);
	return protected;
}

void gc550_status_init(struct gc550_dev *dev)
{
	INIT_DELAYED_WORK(&dev->status_work, gc550_status_work);
	mutex_init(&dev->status_lock);
}
