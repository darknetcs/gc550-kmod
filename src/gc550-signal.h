/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * AVerMedia Live Gamer Extreme (GC550) HDMI input status.
 *
 * The ADV7604 registers the driver polls through the command bridge, and their
 * decoding. Register meanings follow lgx2userspace's SignalStatus and the
 * in-tree adv7604 driver.
 */
#ifndef GC550_SIGNAL_H
#define GC550_SIGNAL_H

#include <linux/types.h>

/* 7-bit I2C addresses of the ADV7604 maps as the bootstrap programs them. */
#define GC550_ADV_IO		0x20
#define GC550_ADV_CP		0x22
#define GC550_ADV_HDMI		0x34
#define GC550_ADV_INFOFRAME	0x46
#define GC550_PCA9539A		0x74

struct gc550_signal_regs {
	u8 io0c;		/* bits 5,2: power-down */
	u8 io12;		/* bit 4: STDI interlaced */
	u8 io6a;		/* bit 4: TMDS clock (port A); 7:5 TMDS PLL lock */
	u8 io6f;		/* bit 4: +5V cable detect (port A) */
	u8 cpb1;		/* bit 7: STDI data valid */
	u8 io02;		/* 7:4: input colour space (0xF = automatic) */
	u8 io60;		/* bit 0: AVI InfoFrame received */
	u8 avi01;		/* AVI InfoFrame byte 1 */
	u8 hdmi05[8];		/* HDMI 0x05-0x0C: mode, TMDS MHz, width, height */
	u8 hdmi20[6];		/* HDMI 0x20-0x25: H front porch, sync, back porch */
	u8 hdmi2a[12];		/* HDMI 0x2A-0x35: V porches in half-lines */
	u8 hdmi3b;		/* bits 5:4: TMDS frequency fraction (1/4 MHz) */
};

enum gc550_colour {
	GC550_COLOUR_UNKNOWN,
	GC550_COLOUR_RGB,
	GC550_COLOUR_YCBCR422,
	GC550_COLOUR_YCBCR444,
	GC550_COLOUR_YCBCR420,
};

struct gc550_signal {
	bool cable;
	bool tmds_clock;
	bool tmds_locked;
	bool stdi_valid;
	bool powered_down;
	bool hdmi;		/* false: DVI */
	bool interlaced;
	bool hdcp;		/* encrypted source: never captured */
	enum gc550_colour colour;
	u8 input_colour_space;
	u16 width, height;	/* height covers both fields when interlaced */
	u16 total_width, total_height;
	u32 pixel_clock_hz;
};

void gc550_signal_decode(const struct gc550_signal_regs *r, struct gc550_signal *s);
bool gc550_signal_present(const struct gc550_signal *s);
/* Refresh rate in mHz (60000 = 60 Hz); 0 if unknown. */
u32 gc550_signal_refresh_mhz(const struct gc550_signal *s);
/* True if the change would be reported (timings only matter with a signal). */
bool gc550_signal_changed(const struct gc550_signal *a, const struct gc550_signal *b);

#endif /* GC550_SIGNAL_H */
