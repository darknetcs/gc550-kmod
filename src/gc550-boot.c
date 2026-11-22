// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * AVerMedia Live Gamer Extreme (GC550) bootstrap.
 *
 * Replays the Windows driver's recorded setup (gc550-bootstrap-data.c) after
 * checking every command in it against the guard, then applies the two
 * corrections lgx2userspace makes for sources other than the one recorded.
 */
#include <linux/slab.h>

#include "gc550.h"
#include "gc550-bootstrap.h"

enum gc550_board_state {
	GC550_BOARD_FRESH,
	GC550_BOARD_CONFIGURED,
	GC550_BOARD_UNKNOWN,
};

/*
 * The bootstrap sets the ADV7604 EDID map address (IO 0xFA, reset value 0x00)
 * to 0x6C and leaves the PCA9539A configuration registers at 00 09. Both are
 * bus powered and survive a USB reset, so together they identify a board
 * still configured by an earlier session. Re-running the bootstrap on such a
 * board gives garbage output, so it is reused instead.
 */
static enum gc550_board_state gc550_board_state(struct gc550_dev *dev)
{
	u8 edid_map = 0, pca[2] = { 0, 0 };

	if (gc550_i2c_read(dev, GC550_ADV_IO, 0xFA, &edid_map, 1) ||
	    gc550_i2c_read(dev, GC550_PCA9539A, 0x06, pca, sizeof(pca)))
		return GC550_BOARD_UNKNOWN;
	if (edid_map != 0x6C)
		return GC550_BOARD_FRESH;
	if (pca[0] == 0x00 && pca[1] == 0x09)
		return GC550_BOARD_CONFIGURED;
	dev_warn(&dev->intf->dev,
		 "unrecognised board state (EDID map %02x, PCA9539A %02x %02x)\n",
		 edid_map, pca[0], pca[1]);
	return GC550_BOARD_UNKNOWN;
}

static int gc550_replay(struct gc550_dev *dev)
{
	unsigned int index = 0;
	size_t pos = 0, len;
	const u8 *cmd;
	u8 *reply;
	int ret;

	reply = kmalloc(GC550_BRIDGE_BUF, GFP_KERNEL);
	if (!reply)
		return -ENOMEM;

	while ((ret = gc550_bootstrap_next(gc550_bootstrap, gc550_bootstrap_size,
					   &pos, &cmd, &len)) > 0) {
		ret = gc550_bridge_send(dev, cmd, len, reply);
		if (ret) {
			dev_err(&dev->intf->dev, "bootstrap command %u failed: %d\n",
				index, ret);
			break;
		}
		index++;
	}
	kfree(reply);
	return ret;
}

/*
 * The recording fixes the ADV7604 input colour space (IO 0x02 bits 7:4) to
 * full-range RGB, as Windows set it for the recorded source, which ruins a
 * YCbCr source. Use automatic selection from the AVI InfoFrame instead, as
 * adv7604.c does for HDMI.
 */
static void gc550_auto_colour_space(struct gc550_dev *dev)
{
	u8 io02, want;

	if (gc550_i2c_read(dev, GC550_ADV_IO, 0x02, &io02, 1)) {
		dev_warn(&dev->intf->dev, "could not read the input colour space\n");
		return;
	}
	want = (io02 & 0x0F) | 0xF0;
	if (want != io02 && gc550_i2c_write(dev, GC550_ADV_IO, 0x02, want))
		dev_warn(&dev->intf->dev, "could not set the input colour space\n");
}

/*
 * Windows also raised the CP brightness for the recorded source; put the CP
 * picture adjustment back to the neutral values the bootstrap itself writes.
 */
static void gc550_neutral_picture(struct gc550_dev *dev)
{
	static const struct { u8 reg, neutral; } adj[] = {
		{ 0x3A, 0x80 },	/* contrast */
		{ 0x3B, 0x80 },	/* saturation */
		{ 0x3C, 0x00 },	/* brightness */
		{ 0x3D, 0x00 },	/* hue */
	};
	unsigned int i;
	u8 val;

	for (i = 0; i < ARRAY_SIZE(adj); i++) {
		if (gc550_i2c_read(dev, GC550_ADV_CP, adj[i].reg, &val, 1))
			continue;
		if (val != adj[i].neutral)
			gc550_i2c_write(dev, GC550_ADV_CP, adj[i].reg, adj[i].neutral);
	}
}

int gc550_boot(struct gc550_dev *dev)
{
	struct device *d = &dev->intf->dev;
	enum gc550_guard_verdict verdict;
	unsigned int bad;
	int ret;

	/* Nothing is sent unless the whole table passes the guard. */
	ret = gc550_bootstrap_validate(gc550_bootstrap, gc550_bootstrap_size, &bad, &verdict);
	if (ret) {
		dev_err(d, "bootstrap table refused at command %u (%s); nothing sent\n",
			bad, ret == -EPERM ? gc550_guard_reason(verdict) : "malformed");
		return ret;
	}

	gc550_bridge_drain(dev);
	switch (gc550_board_state(dev)) {
	case GC550_BOARD_CONFIGURED:
		dev_info(d, "already configured, reusing its configuration\n");
		break;
	case GC550_BOARD_FRESH:
		dev_info(d, "bootstrapping (%u commands)\n", gc550_bootstrap_count);
		ret = gc550_replay(dev);
		if (ret)
			return ret;
		gc550_bridge_drain(dev);
		dev_info(d, "bootstrap complete\n");
		break;
	default:
		dev_err(d, "board state unknown; unplug and replug the card\n");
		return -EIO;
	}

	gc550_auto_colour_space(dev);
	gc550_neutral_picture(dev);
	return 0;
}
