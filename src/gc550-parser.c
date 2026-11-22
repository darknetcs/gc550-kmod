// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * AVerMedia Live Gamer Extreme (GC550) capture stream parser.
 *
 * Stream format and checksums from lgx2userspace's FrameBuilder (MIT,
 * Copyright 2021 Chris Sawczuk and contributors). Unlike FrameBuilder this
 * version keeps the marker match state across chunks, so it never copies a
 * chunk to stitch a marker that straddles a boundary.
 *
 * One deliberate difference: an audio packet that arrives inside a frame's
 * SAV->EAV span returns to the frame afterwards instead of abandoning it. In
 * captures so far audio only arrives between frames, so this changes nothing
 * there.
 */
#include <linux/kernel.h>
#include <linux/string.h>

#include "gc550-parser.h"

#define GC550_MARK_SAV		0xC0
#define GC550_MARK_EAV		0xC1
#define GC550_MARK_AUDIO	0x58

static const u8 gc550_prefix[3] = { 0x00, 0xFF, 0xFF };

/* SAV header: valid iff b3 == b0 + b1 + b2 - 0x40 (mod 256). */
static bool gc550_sav_valid(const u8 *b)
{
	return b[3] == (u8)(b[0] + b[1] + b[2] - 0x40);
}

/* EAV header: valid iff c4 == c0 + c1 + c2 + c3 - 0x3F (mod 256). */
static bool gc550_eav_valid(const u8 *c)
{
	return c[4] == (u8)(c[0] + c[1] + c[2] + c[3] - 0x3F);
}

void gc550_parser_init(struct gc550_parser *p,
		       const struct gc550_parser_ops *ops, void *priv)
{
	memset(p, 0, sizeof(*p));
	p->ops = ops;
	p->priv = priv;
	p->mode = GC550_PARSER_SCAN;
}

void gc550_parser_reset(struct gc550_parser *p)
{
	/* A held buffer stays held and is reused for the next frame. */
	p->mode = GC550_PARSER_SCAN;
	p->in_frame = false;
	p->match = 0;
	p->hdr_len = 0;
	p->audio_left = 0;
	p->frame_len = 0;
}

void gc550_parser_release_buffer(struct gc550_parser *p)
{
	p->dst = NULL;
	p->dst_cap = 0;
	p->in_frame = false;
	p->frame_len = 0;
}

/* Bytes that belong to the current frame, if one is open. */
static void gc550_video(struct gc550_parser *p, const u8 *data, size_t len)
{
	size_t room, take;

	if (!p->in_frame || !len)
		return;
	if (p->dst) {
		room = p->frame_len < p->dst_cap ? p->dst_cap - p->frame_len : 0;
		take = min(len, room);
		if (take)
			memcpy(p->dst + p->frame_len, data, take);
		if (take < len && room)
			p->stats.desyncs++;	/* overflowed the buffer */
	}
	p->frame_len += len;
}

static void gc550_sav(struct gc550_parser *p)
{
	p->in_frame = true;
	p->frame_len = 0;
	p->sav_b0 = p->hdr[0];
	if (!p->dst) {
		p->dst_cap = 0;
		p->dst = p->ops->frame_buffer(p->priv, &p->dst_cap);
		if (!p->dst)
			p->dst_cap = 0;
	}
}

static void gc550_eav(struct gc550_parser *p)
{
	size_t len = p->frame_len;

	p->in_frame = false;
	if (!gc550_eav_valid(p->hdr)) {
		p->stats.desyncs++;
		return;
	}
	if (p->hdr[0] != p->sav_b0)
		p->stats.desyncs++;	/* soft mismatch: still a frame */

	if (len < GC550_FRAME_BYTES - GC550_FRAME_TOLERANCE ||
	    len > GC550_FRAME_BYTES + GC550_FRAME_TOLERANCE) {
		p->stats.bad_spans++;
		return;
	}
	if (!p->dst) {
		p->stats.frames_dropped++;
		if (p->ops->frame_dropped)
			p->ops->frame_dropped(p->priv);
		return;
	}
	p->stats.frames++;
	p->ops->frame_done(p->priv, min(len, p->dst_cap));
	p->dst = NULL;
	p->dst_cap = 0;
}

/* The header of the marker in p->type is complete. */
static void gc550_header_done(struct gc550_parser *p)
{
	u8 marker[4] = { 0x00, 0xFF, 0xFF, p->type };
	u8 hdr[5];
	u32 len;

	p->mode = GC550_PARSER_SCAN;
	switch (p->type) {
	case GC550_MARK_SAV:
		if (gc550_sav_valid(p->hdr)) {
			gc550_sav(p);
			return;
		}
		break;
	case GC550_MARK_EAV:
		gc550_eav(p);
		return;
	case GC550_MARK_AUDIO:
		len = ((u32)p->hdr[0] << 24) | ((u32)p->hdr[1] << 16) |
		      ((u32)p->hdr[2] << 8) | p->hdr[3];
		if (len <= GC550_AUDIO_MAX_PACKET) {
			p->stats.audio_packets++;
			p->audio_left = len;
			if (len)
				p->mode = GC550_PARSER_AUDIO;
			return;
		}
		break;
	}

	/*
	 * Not a real marker after all: its bytes are frame data, and the
	 * header bytes are scanned again since a marker may start inside them.
	 * They cannot complete one on their own, so this recurses only once.
	 */
	memcpy(hdr, p->hdr, p->hdr_len);
	gc550_video(p, marker, sizeof(marker));
	gc550_parser_feed(p, hdr, p->hdr_len);
}

void gc550_parser_feed(struct gc550_parser *p, const u8 *buf, size_t len)
{
	size_t pos = 0, take;
	const u8 *zero;
	u8 b;

	while (pos < len) {
		switch (p->mode) {
		case GC550_PARSER_AUDIO:
			take = min_t(size_t, len - pos, p->audio_left);
			p->ops->audio(p->priv, buf + pos, take);
			p->stats.audio_bytes += take;
			p->audio_left -= take;
			pos += take;
			if (!p->audio_left)
				p->mode = GC550_PARSER_SCAN;
			break;

		case GC550_PARSER_HEADER:
			take = min_t(size_t, len - pos, p->hdr_need - p->hdr_len);
			memcpy(p->hdr + p->hdr_len, buf + pos, take);
			p->hdr_len += take;
			pos += take;
			if (p->hdr_len == p->hdr_need)
				gc550_header_done(p);
			break;

		case GC550_PARSER_SCAN:
			if (!p->match) {
				/* Fast path: YUYV video rarely contains 0x00. */
				zero = memchr(buf + pos, 0x00, len - pos);
				if (!zero) {
					gc550_video(p, buf + pos, len - pos);
					pos = len;
					break;
				}
				gc550_video(p, buf + pos, zero - (buf + pos));
				pos = zero - buf + 1;
				p->match = 1;
				break;
			}

			b = buf[pos];
			if (p->match < 3) {
				if (b == 0xFF) {
					p->match++;
					pos++;
				} else {
					/* Prefix broken: it was data. Rescan b. */
					gc550_video(p, gc550_prefix, p->match);
					p->match = 0;
				}
				break;
			}

			/* 00 FF FF matched; b is the marker type. */
			p->match = 0;
			if (b == GC550_MARK_SAV || b == GC550_MARK_AUDIO ||
			    (b == GC550_MARK_EAV && p->in_frame)) {
				p->type = b;
				p->hdr_len = 0;
				p->hdr_need = b == GC550_MARK_EAV ? 5 : 4;
				p->mode = GC550_PARSER_HEADER;
				pos++;
			} else if (b == GC550_MARK_EAV) {
				/* Stray EAV outside a frame: ignore it. */
				pos++;
			} else {
				/* Not a marker: the prefix was data. Rescan b. */
				gc550_video(p, gc550_prefix, sizeof(gc550_prefix));
			}
			break;
		}
	}
}
