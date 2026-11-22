/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * AVerMedia Live Gamer Extreme (GC550) capture stream parser.
 *
 * Bulk IN 0x83 carries BT.1120-style markers:
 *
 *   00 FF FF C0 <4-byte header>   SAV: a 1920x1080 YUYV frame starts
 *   00 FF FF C1 <5-byte header>   EAV: the frame ends
 *   00 FF FF 58 <BE u32 length>   audio packet: <length> bytes of 48 kHz
 *                                 S16_LE stereo follow, never scanned
 *
 * One checksum-valid SAV->EAV pair is one frame. The parser accepts the stream
 * in arbitrarily split chunks and copies video straight into the buffer the
 * owner provides; it never allocates.
 */
#ifndef GC550_PARSER_H
#define GC550_PARSER_H

#include <linux/types.h>

#define GC550_FRAME_WIDTH	1920
#define GC550_FRAME_HEIGHT	1080
#define GC550_FRAME_BYTES	(GC550_FRAME_WIDTH * GC550_FRAME_HEIGHT * 2)
/* A SAV->EAV span within this of GC550_FRAME_BYTES is a frame. */
#define GC550_FRAME_TOLERANCE	0x4000
/* Buffers should hold this much so a frame at the tolerance edge fits. */
#define GC550_FRAME_BUFFER_BYTES (GC550_FRAME_BYTES + GC550_FRAME_TOLERANCE)
#define GC550_AUDIO_MAX_PACKET	(256 * 1024)

struct gc550_parser_ops {
	/*
	 * Called at each SAV while no buffer is held: return the buffer for the
	 * next frame and its capacity, or NULL to drop that frame. The parser
	 * keeps the buffer until frame_done(), so a span with the wrong size
	 * reuses it for the next frame.
	 */
	u8 *(*frame_buffer)(void *priv, size_t *capacity);
	/* The held buffer now contains a complete frame of @bytes. */
	void (*frame_done)(void *priv, size_t bytes);
	/* Optional: a complete frame arrived while no buffer was held. */
	void (*frame_dropped)(void *priv);
	/* Audio payload bytes, in order; packet boundaries are not reported. */
	void (*audio)(void *priv, const u8 *data, size_t len);
};

struct gc550_parser_stats {
	u64 frames;		/* delivered through frame_done() */
	u64 frames_dropped;	/* complete, but no buffer was available */
	u64 audio_packets;
	u64 audio_bytes;
	u32 desyncs;		/* bad EAV checksum, SAV/EAV mismatch, overflow */
	u32 bad_spans;		/* SAV->EAV span of the wrong size */
};

enum gc550_parser_mode {
	GC550_PARSER_SCAN,	/* looking for markers */
	GC550_PARSER_HEADER,	/* collecting a marker's header bytes */
	GC550_PARSER_AUDIO,	/* inside an audio payload */
};

struct gc550_parser {
	const struct gc550_parser_ops *ops;
	void *priv;

	enum gc550_parser_mode mode;
	bool in_frame;		/* between a valid SAV and its EAV */
	u8 match;		/* bytes of the 00 FF FF prefix matched */
	u8 type;		/* marker type whose header is being collected */
	u8 hdr[5];
	u8 hdr_len;
	u8 hdr_need;
	u8 sav_b0;
	u32 audio_left;

	u8 *dst;
	size_t dst_cap;
	size_t frame_len;	/* bytes seen since SAV (may exceed dst_cap) */

	struct gc550_parser_stats stats;
};

void gc550_parser_init(struct gc550_parser *p,
		       const struct gc550_parser_ops *ops, void *priv);
/* Bytes were lost: drop the partial frame and re-sync on the next SAV. */
void gc550_parser_reset(struct gc550_parser *p);
void gc550_parser_feed(struct gc550_parser *p, const u8 *buf, size_t len);
/*
 * Forget the held buffer without frame_done() (the owner is taking it back,
 * e.g. on STREAMOFF). The current frame, if any, is dropped.
 */
void gc550_parser_release_buffer(struct gc550_parser *p);

#endif /* GC550_PARSER_H */
