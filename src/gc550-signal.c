// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * AVerMedia Live Gamer Extreme (GC550) HDMI input status decoding.
 *
 * Ported from lgx2userspace's SignalStatus (MIT, Copyright 2021 Chris Sawczuk
 * and contributors), without floating point. No USB dependencies, so it can
 * be unit-tested in userspace.
 */
#include <linux/math64.h>
#include <linux/string.h>

#include "gc550-signal.h"

static u16 gc550_be16(const u8 *p, u16 mask)
{
	return (u16)(((p[0] << 8) | p[1]) & mask);
}

void gc550_signal_decode(const struct gc550_signal_regs *r, struct gc550_signal *s)
{
	static const enum gc550_colour formats[4] = {
		GC550_COLOUR_RGB, GC550_COLOUR_YCBCR422,
		GC550_COLOUR_YCBCR444, GC550_COLOUR_YCBCR420,
	};
	const u8 *h = r->hdmi05;	/* h[n] is HDMI register 0x05 + n */
	const u8 *hp = r->hdmi20;
	const u8 *vp = r->hdmi2a;
	u16 field0_height;
	u32 tmds_hz, bpc, rep;

	memset(s, 0, sizeof(*s));
	s->cable = r->io6f & 0x10;
	s->tmds_clock = r->io6a & 0x10;
	s->tmds_locked = (r->io6a & 0xE0) == 0xE0;
	s->stdi_valid = r->cpb1 & 0x80;
	s->powered_down = r->io0c & 0x24;
	s->hdmi = h[0] & 0x80;
	s->interlaced = r->io12 & 0x10;
	s->hdcp = s->hdmi && (h[0] & 0x40);
	s->input_colour_space = r->io02 >> 4;
	s->colour = GC550_COLOUR_UNKNOWN;
	if (s->hdmi && (r->io60 & 0x01))
		s->colour = formats[(r->avi01 >> 5) & 0x03];

	s->width = gc550_be16(&h[2], 0x0FFF);
	field0_height = gc550_be16(&h[4], 0x0FFF);
	s->height = field0_height;
	if (s->interlaced)
		s->height += gc550_be16(&h[6], 0x0FFF);

	s->total_width = gc550_be16(&hp[0], 0x3FF) + gc550_be16(&hp[2], 0x3FF) +
			 gc550_be16(&hp[4], 0x3FF) + s->width;
	/* vertical porches are in half-lines: 0x2A vfp0, 0x2E vsync0, 0x32 vbp0 */
	s->total_height = gc550_be16(&vp[0], 0x1FFF) / 2 + gc550_be16(&vp[4], 0x1FFF) / 2 +
			  gc550_be16(&vp[8], 0x1FFF) / 2 + field0_height;

	tmds_hz = h[1] * 1000000u + ((r->hdmi3b & 0x30) >> 4) * 250000u;
	if (s->hdmi) {
		/* deep colour and pixel repetition clock TMDS above the pixel clock */
		bpc = ((h[6] & 0x60) >> 4) + 8;
		rep = (h[0] & 0x0F) + 1;
		tmds_hz = (u32)div_u64((u64)tmds_hz * 8, bpc * rep);
	}
	s->pixel_clock_hz = tmds_hz;
}

bool gc550_signal_present(const struct gc550_signal *s)
{
	/* SSPD is not checked, as adv7604.c skips it on AFE-equipped parts. */
	return !s->powered_down && s->stdi_valid && s->tmds_locked && s->tmds_clock;
}

u32 gc550_signal_refresh_mhz(const struct gc550_signal *s)
{
	u64 pixels = (u64)s->total_width * s->total_height;

	if (!pixels)
		return 0;
	return (u32)div64_u64((u64)s->pixel_clock_hz * 1000 + pixels / 2, pixels);
}

bool gc550_signal_changed(const struct gc550_signal *a, const struct gc550_signal *b)
{
	bool present = gc550_signal_present(a);

	if (a->cable != b->cable || present != gc550_signal_present(b) || a->hdcp != b->hdcp)
		return true;
	if (!present)
		return false;
	/* the pixel clock register resolves 0.25 MHz: ignore noise below that */
	return a->hdmi != b->hdmi || a->interlaced != b->interlaced ||
	       a->colour != b->colour || a->input_colour_space != b->input_colour_space ||
	       a->width != b->width || a->height != b->height ||
	       a->total_width != b->total_width || a->total_height != b->total_height ||
	       a->pixel_clock_hz / 250000 != b->pixel_clock_hz / 250000;
}
