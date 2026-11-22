// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Unit tests for the capture stream parser, on synthetic streams fed in every
 * small split size (so every marker and header straddles a chunk boundary at
 * every offset) and in random splits.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc550-parser.h"
#include "check.h"
#include "streamgen.h"

#define MAX_FRAMES 16

struct sink {
	u8 *bufs[MAX_FRAMES];
	size_t lens[MAX_FRAMES];
	int nframes;
	int handed_out;		/* buffers handed out */
	int refuse;		/* return NULL from frame_buffer */
	u8 *audio;
	size_t audio_len, audio_cap;
};

static u8 *sink_frame_buffer(void *priv, size_t *cap)
{
	struct sink *s = priv;

	if (s->refuse || s->nframes >= MAX_FRAMES)
		return NULL;
	s->handed_out++;
	*cap = GC550_FRAME_BUFFER_BYTES;
	memset(s->bufs[s->nframes], 0xEE, GC550_FRAME_BUFFER_BYTES);
	return s->bufs[s->nframes];
}

static void sink_frame_done(void *priv, size_t bytes)
{
	struct sink *s = priv;

	s->lens[s->nframes++] = bytes;
}

static void sink_audio(void *priv, const u8 *data, size_t len)
{
	struct sink *s = priv;

	if (s->audio_len + len > s->audio_cap) {
		s->audio_cap = (s->audio_len + len) * 2;
		s->audio = realloc(s->audio, s->audio_cap);
	}
	memcpy(s->audio + s->audio_len, data, len);
	s->audio_len += len;
}

static const struct gc550_parser_ops sink_ops = {
	.frame_buffer = sink_frame_buffer,
	.frame_done = sink_frame_done,
	.audio = sink_audio,
};

static u8 *frame_store[MAX_FRAMES];

static void sink_init(struct sink *s)
{
	int i;

	memset(s, 0, sizeof(*s));
	for (i = 0; i < MAX_FRAMES; i++)
		s->bufs[i] = frame_store[i];
}

/* Feed @len bytes in chunks of @step (0 = random sizes 1..3 MiB). */
static void feed_split(struct gc550_parser *p, const u8 *buf, size_t len, size_t step,
		       uint32_t seed)
{
	size_t pos = 0;

	while (pos < len) {
		size_t n = step;

		if (!n) {
			seed = seed * 1103515245u + 12345u;
			n = 1 + (seed >> 8) % (3 << 20);
		}
		if (n > len - pos)
			n = len - pos;
		gc550_parser_feed(p, buf + pos, n);
		pos += n;
	}
}

/* The basic stream: junk, 4 tricky frames with audio between them. */
#define BASIC_FRAMES 4
static u8 *basic_expect[BASIC_FRAMES];
static u8 basic_audio[3][4000];
static const uint32_t basic_audio_len[3] = { 3840, 1, 0 };
static struct sg basic;

static void build_basic(void)
{
	int i;

	sg_init(&basic, 0xC0FFEE);
	sg_junk(&basic, 1000);
	for (i = 0; i < BASIC_FRAMES; i++) {
		basic_expect[i] = malloc(GC550_FRAME_BYTES);
		sg_frame(&basic, GC550_FRAME_BYTES, 1, basic_expect[i]);
		if (i < 3)
			sg_audio(&basic, basic_audio_len[i], basic_audio[i]);
		sg_junk(&basic, 17);
	}
}

static void check_basic(struct sink *s, struct gc550_parser *p, const char *how)
{
	size_t off = 0;
	int i;

	CHECKF(s->nframes == BASIC_FRAMES, "%s: %d frames", how, s->nframes);
	for (i = 0; i < s->nframes && i < BASIC_FRAMES; i++) {
		CHECKF(s->lens[i] == GC550_FRAME_BYTES, "%s: frame %d length", how, i);
		CHECKF(!memcmp(s->bufs[i], basic_expect[i], GC550_FRAME_BYTES),
		       "%s: frame %d content", how, i);
	}
	CHECKF(s->audio_len == 3841, "%s: %zu audio bytes", how, s->audio_len);
	for (i = 0; i < 3; i++) {
		if (off + basic_audio_len[i] <= s->audio_len)
			CHECKF(!memcmp(s->audio + off, basic_audio[i], basic_audio_len[i]),
			       "%s: audio packet %d", how, i);
		off += basic_audio_len[i];
	}
	CHECKF(p->stats.audio_packets == 3, "%s: audio packets", how);
	CHECKF(p->stats.desyncs == 0, "%s: %u desyncs", how, p->stats.desyncs);
	CHECKF(p->stats.bad_spans == 0, "%s: bad spans", how);
}

static void test_splits(void)
{
	struct gc550_parser p;
	struct sink s;
	char how[64];
	size_t step;

	for (step = 1; step <= 19; step++) {
		sink_init(&s);
		gc550_parser_init(&p, &sink_ops, &s);
		feed_split(&p, basic.buf, basic.len, step, 0);
		snprintf(how, sizeof(how), "step %zu", step);
		check_basic(&s, &p, how);
		free(s.audio);
	}
	for (step = 0; step < 8; step++) {
		sink_init(&s);
		gc550_parser_init(&p, &sink_ops, &s);
		feed_split(&p, basic.buf, basic.len, 0, (uint32_t)step + 1);
		snprintf(how, sizeof(how), "random split %zu", step);
		check_basic(&s, &p, how);
		free(s.audio);
	}
	sink_init(&s);
	gc550_parser_init(&p, &sink_ops, &s);
	feed_split(&p, basic.buf, basic.len, basic.len, 0);
	check_basic(&s, &p, "one chunk");
	free(s.audio);
}

static void test_wrong_sizes_and_drops(void)
{
	struct gc550_parser p;
	struct sink s;
	struct sg g;

	sg_init(&g, 7);
	sg_frame(&g, 1000, 0, NULL);					/* too short */
	sg_frame(&g, GC550_FRAME_BYTES + GC550_FRAME_TOLERANCE + 1, 0, NULL); /* too long */
	sg_frame(&g, GC550_FRAME_BYTES - GC550_FRAME_TOLERANCE, 0, NULL);	/* edge: ok */
	sg_frame(&g, GC550_FRAME_BYTES + GC550_FRAME_TOLERANCE, 0, NULL);	/* edge: ok */
	sink_init(&s);
	gc550_parser_init(&p, &sink_ops, &s);
	feed_split(&p, g.buf, g.len, 4096, 0);
	CHECK(s.nframes == 2);
	CHECK(s.lens[0] == GC550_FRAME_BYTES - GC550_FRAME_TOLERANCE);
	CHECK(s.lens[1] == GC550_FRAME_BYTES + GC550_FRAME_TOLERANCE);
	CHECK(p.stats.bad_spans == 2);
	CHECKF(s.handed_out == 2, "wrong-size spans reuse the buffer (%d handed out)",
	       s.handed_out);
	CHECK(p.stats.desyncs == 1);	/* the too-long span overflowed the buffer */

	/* no buffer available: counted, nothing delivered */
	sink_init(&s);
	s.refuse = 1;
	gc550_parser_init(&p, &sink_ops, &s);
	feed_split(&p, basic.buf, basic.len, 65536, 0);
	CHECK(s.nframes == 0);
	CHECK(p.stats.frames_dropped == BASIC_FRAMES);
	CHECK(s.audio_len == 3841);	/* audio unaffected */
	free(s.audio);
	sg_free(&g);
}

static void test_bad_markers(void)
{
	struct gc550_parser p;
	struct sink s;
	u8 *expect = malloc(GC550_FRAME_BYTES);
	u8 big[8] = { 0x00, 0xFF, 0xFF, 0x58, 0x00, 0x04, 0x00, 0x01 }; /* > max */
	struct sg g;
	uint8_t b0;

	sg_init(&g, 99);
	sg_eav(&g, 0x33, 1);		/* stray EAV outside a frame */
	sg_sav(&g, 0x44, 0);		/* bad SAV checksum: not a frame start */
	sg_junk(&g, 100);
	sg_put(&g, big, sizeof(big));	/* implausible audio length: data */
	sg_junk(&g, 100);
	/* frame with a bad EAV checksum: dropped, counted */
	b0 = 0x55;
	sg_sav(&g, b0, 1);
	sg_payload(&g, GC550_FRAME_BYTES, 0, NULL);
	sg_eav(&g, b0, 0);
	/* frame with mismatched SAV/EAV b0: delivered, counted */
	sg_sav(&g, 0x66, 1);
	sg_payload(&g, GC550_FRAME_BYTES, 0, expect);
	sg_eav(&g, 0x67, 1);

	sink_init(&s);
	gc550_parser_init(&p, &sink_ops, &s);
	feed_split(&p, g.buf, g.len, 3, 0);
	CHECK(s.nframes == 1);
	CHECK(s.nframes == 1 && !memcmp(s.bufs[0], expect, GC550_FRAME_BYTES));
	CHECK(p.stats.desyncs == 2);
	CHECK(s.audio_len == 0);
	CHECK(p.stats.audio_packets == 0);
	sg_free(&g);
	free(expect);
}

static void test_bad_sav_inside_frame(void)
{
	/* An invalid SAV inside a frame is frame data, header bytes included. */
	struct gc550_parser p;
	struct sink s;
	u8 *expect = malloc(GC550_FRAME_BYTES);
	size_t half = GC550_FRAME_BYTES / 2;
	struct sg g;

	sg_init(&g, 5);
	sg_sav(&g, 0x21, 1);
	sg_payload(&g, half, 0, NULL);
	sg_sav(&g, 0x22, 0);
	sg_payload(&g, GC550_FRAME_BYTES - half - 8, 0, NULL);
	memcpy(expect, g.buf + 8, GC550_FRAME_BYTES);
	sg_eav(&g, 0x21, 1);

	sink_init(&s);
	gc550_parser_init(&p, &sink_ops, &s);
	feed_split(&p, g.buf, g.len, 5, 0);
	CHECK(s.nframes == 1);
	CHECK(s.nframes == 1 && !memcmp(s.bufs[0], expect, GC550_FRAME_BYTES));
	sg_free(&g);
	free(expect);
}

static void test_gap(void)
{
	struct gc550_parser p;
	struct sink s;
	size_t cut = basic.len / 3;

	/* Lose bytes in the middle of frame 1: it is dropped, the rest survive. */
	sink_init(&s);
	gc550_parser_init(&p, &sink_ops, &s);
	gc550_parser_feed(&p, basic.buf, cut);
	gc550_parser_reset(&p);
	gc550_parser_feed(&p, basic.buf + cut + 12345, basic.len - cut - 12345);
	CHECKF(s.nframes == BASIC_FRAMES - 1, "%d frames after a gap", s.nframes);
	CHECK(s.handed_out == BASIC_FRAMES - 1);	/* the cut frame's buffer is reused */
	free(s.audio);
}

static void test_audio_inside_frame(void)
{
	/* Audio inside a SAV->EAV span is cut out and the frame continues. */
	struct gc550_parser p;
	struct sink s;
	u8 *expect = malloc(GC550_FRAME_BYTES);
	u8 audio[300];
	size_t half = 1000000;
	struct sg g;

	sg_init(&g, 11);
	sg_sav(&g, 0x30, 1);
	sg_payload(&g, half, 1, expect);
	sg_audio(&g, sizeof(audio), audio);
	sg_payload(&g, GC550_FRAME_BYTES - half, 1, expect + half);
	sg_eav(&g, 0x30, 1);

	sink_init(&s);
	gc550_parser_init(&p, &sink_ops, &s);
	feed_split(&p, g.buf, g.len, 7, 0);
	CHECK(s.nframes == 1);
	CHECK(s.nframes == 1 && !memcmp(s.bufs[0], expect, GC550_FRAME_BYTES));
	CHECK(s.audio_len == sizeof(audio) && !memcmp(s.audio, audio, sizeof(audio)));
	free(s.audio);
	sg_free(&g);
	free(expect);
}

int main(void)
{
	int i;

	for (i = 0; i < MAX_FRAMES; i++)
		frame_store[i] = malloc(GC550_FRAME_BUFFER_BYTES);
	build_basic();
	test_splits();
	test_wrong_sizes_and_drops();
	test_bad_markers();
	test_bad_sav_inside_frame();
	test_gap();
	test_audio_inside_frame();
	return check_report("test-parser");
}
