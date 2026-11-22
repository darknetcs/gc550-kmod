// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Unit tests for the HDMI input status decoder, ported from lgx2userspace's
 * SignalStatus.t.cpp (GC550 cases; the GC551 front-end is not supported).
 */
#include <string.h>

#include "gc550-signal.h"
#include "check.h"

static void put16(u8 *p, u16 v)
{
	p[0] = (u8)(v >> 8);
	p[1] = (u8)v;
}

/* Locked CEA 1080p60 HDMI source: 148.5 MHz, 2200x1125 total. */
static struct gc550_signal_regs locked1080p60(void)
{
	struct gc550_signal_regs r;

	memset(&r, 0, sizeof(r));
	r.io6f = 0x10;
	r.io6a = 0xF0;
	r.cpb1 = 0x80;
	r.io02 = 0xFC;
	r.io60 = 0x01;
	r.avi01 = 0x00;
	r.hdmi05[0] = 0x80;
	r.hdmi05[1] = 148;
	put16(&r.hdmi05[2], 1920);
	put16(&r.hdmi05[4], 1080);
	r.hdmi3b = 0x20;
	put16(&r.hdmi20[0], 88);
	put16(&r.hdmi20[2], 44);
	put16(&r.hdmi20[4], 148);
	put16(&r.hdmi2a[0], 4 * 2);
	put16(&r.hdmi2a[4], 5 * 2);
	put16(&r.hdmi2a[8], 36 * 2);
	return r;
}

static struct gc550_signal decode(struct gc550_signal_regs r)
{
	struct gc550_signal s;

	gc550_signal_decode(&r, &s);
	return s;
}

int main(void)
{
	struct gc550_signal_regs r, a, b;
	struct gc550_signal s, t;

	s = decode(locked1080p60());
	CHECK(s.cable);
	CHECK(gc550_signal_present(&s));
	CHECK(s.hdmi && !s.interlaced && !s.hdcp);
	CHECK(s.width == 1920 && s.height == 1080);
	CHECK(s.total_width == 2200 && s.total_height == 1125);
	CHECKF(s.pixel_clock_hz == 148500000, "%u", s.pixel_clock_hz);
	CHECKF(gc550_signal_refresh_mhz(&s) == 60000, "%u", gc550_signal_refresh_mhz(&s));
	CHECK(s.colour == GC550_COLOUR_RGB);
	CHECK(s.input_colour_space == 0xF);

	r = locked1080p60();
	r.avi01 = 0x20;
	r.io02 = 0x1C;
	s = decode(r);
	CHECK(s.colour == GC550_COLOUR_YCBCR422);
	CHECK(s.input_colour_space == 0x1);
	r.io60 = 0;
	CHECK(decode(r).colour == GC550_COLOUR_UNKNOWN);
	r = locked1080p60();
	r.avi01 = 0x40;
	s = decode(r);
	t = decode(locked1080p60());
	CHECK(gc550_signal_changed(&s, &t));
	CHECK(!gc550_signal_changed(&t, &t));

	memset(&r, 0, sizeof(r));
	s = decode(r);
	CHECK(!s.cable && !gc550_signal_present(&s));
	CHECK(gc550_signal_refresh_mhz(&s) == 0);

	/* cable without lock */
	r = locked1080p60(); r.io6a = 0x10; s = decode(r);
	CHECK(s.cable && !gc550_signal_present(&s));
	r = locked1080p60(); r.io6a = 0xE0; s = decode(r);
	CHECK(!gc550_signal_present(&s));
	r = locked1080p60(); r.cpb1 = 0; s = decode(r);
	CHECK(!gc550_signal_present(&s));
	r = locked1080p60(); r.io0c = 0x24; s = decode(r);
	CHECK(!gc550_signal_present(&s));

	/* HDCP */
	r = locked1080p60(); r.hdmi05[0] |= 0x40; s = decode(r);
	CHECK(s.hdcp);
	t = decode(locked1080p60());
	CHECK(gc550_signal_changed(&s, &t));
	r = locked1080p60(); r.hdmi05[0] = 0x40; s = decode(r);
	CHECK(!s.hdcp);		/* bit 6 without HDMI mode is not HDCP */

	/* deep colour and pixel repetition */
	r = locked1080p60(); r.hdmi05[1] = 185; r.hdmi3b = 0x20; r.hdmi05[6] = 0x20;
	s = decode(r);
	CHECKF(s.pixel_clock_hz == 148400000, "%u", s.pixel_clock_hz);
	r = locked1080p60(); r.hdmi05[0] = 0x81; s = decode(r);
	CHECKF(s.pixel_clock_hz == 74250000, "%u", s.pixel_clock_hz);

	/* interlaced */
	r = locked1080p60(); r.io12 = 0x10;
	put16(&r.hdmi05[4], 540); put16(&r.hdmi05[6], 540); r.hdmi05[6] |= 0x20;
	s = decode(r);
	CHECK(s.interlaced && s.height == 1080);

	/* equality */
	memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b));
	a.io6f = b.io6f = 0x10;
	put16(&b.hdmi05[2], 1234);
	s = decode(a); t = decode(b);
	CHECK(!gc550_signal_changed(&s, &t));
	t = decode(locked1080p60());
	CHECK(gc550_signal_changed(&s, &t));
	r = locked1080p60(); put16(&r.hdmi05[2], 1280); s = decode(r);
	CHECK(gc550_signal_changed(&s, &t));

	return check_report("test-signal");
}
