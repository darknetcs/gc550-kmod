/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Synthetic GC550 capture stream generator for the parser tests. Header-only
 * so both the C tests and the C++ differential test can use it.
 */
#ifndef GC550_STREAMGEN_H
#define GC550_STREAMGEN_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct sg {
	uint8_t *buf;
	size_t len, cap;
	uint32_t rng;
};

static inline uint32_t sg_rand(struct sg *s)
{
	/* xorshift32: deterministic across runs and compilers */
	uint32_t x = s->rng;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return s->rng = x;
}

static inline void sg_init(struct sg *s, uint32_t seed)
{
	memset(s, 0, sizeof(*s));
	s->rng = seed ? seed : 1;
}

static inline void sg_free(struct sg *s)
{
	free(s->buf);
	s->buf = NULL;
	s->len = s->cap = 0;
}

static inline void sg_put(struct sg *s, const uint8_t *data, size_t n)
{
	if (s->len + n > s->cap) {
		size_t cap = s->cap ? s->cap : 1 << 20;

		while (cap < s->len + n)
			cap *= 2;
		s->buf = (uint8_t *)realloc(s->buf, cap);
		if (!s->buf)
			abort();
		s->cap = cap;
	}
	memcpy(s->buf + s->len, data, n);
	s->len += n;
}

static inline void sg_byte(struct sg *s, uint8_t b)
{
	sg_put(s, &b, 1);
}

/* A video-like byte that is never 0x00. */
static inline uint8_t sg_pixel(struct sg *s)
{
	return (uint8_t)(1 + sg_rand(s) % 255);
}

/*
 * Frame payload of @size bytes. When @tricky, it contains bare 0x00 bytes,
 * 00 FF, 00 FF FF prefixes and 00 FF FF <non-marker type> sequences, which a
 * parser must pass through as data. The payload is also written to @out
 * (if not NULL) so tests can compare.
 */
static inline void sg_payload(struct sg *s, size_t size, int tricky, uint8_t *out)
{
	size_t start = s->len, i = 0;

	while (i < size) {
		uint32_t r = sg_rand(s);

		if (tricky && size - i >= 4 && r % 997 == 0) {
			static const uint8_t types[] = { 0x00, 0x01, 0xC2, 0x57, 0x59, 0xFF, 0xBF };
			uint8_t seq[4] = { 0x00, 0xFF, 0xFF, types[(r >> 10) % sizeof(types)] };
			size_t n = 1 + (r >> 16) % 4;	/* 00 / 00 FF / 00 FF FF / full */

			if (seq[3] == 0x00 && n == 4)
				n = 3;	/* 00 FF FF 00: keep it a non-marker */
			sg_put(s, seq, n);
			i += n;
			if (n < 4 && i < size) {
				/* break the prefix with a non-FF, non-00 byte */
				sg_byte(s, 0x10);
				i++;
			}
		} else {
			sg_byte(s, sg_pixel(s));
			i++;
		}
	}
	if (out)
		memcpy(out, s->buf + start, size);
}

static inline void sg_sav(struct sg *s, uint8_t b0, int valid)
{
	uint8_t m[8] = { 0x00, 0xFF, 0xFF, 0xC0, b0, sg_pixel(s), sg_pixel(s), 0 };

	m[7] = (uint8_t)(m[4] + m[5] + m[6] - 0x40);
	if (!valid)
		m[7] ^= 0x01;
	sg_put(s, m, sizeof(m));
}

static inline void sg_eav(struct sg *s, uint8_t c0, int valid)
{
	uint8_t m[9] = { 0x00, 0xFF, 0xFF, 0xC1, c0, sg_pixel(s), sg_pixel(s), sg_pixel(s), 0 };

	m[8] = (uint8_t)(m[4] + m[5] + m[6] + m[7] - 0x3F);
	if (!valid)
		m[8] ^= 0x01;
	sg_put(s, m, sizeof(m));
}

/* A full SAV, payload, EAV frame. */
static inline void sg_frame(struct sg *s, size_t size, int tricky, uint8_t *out)
{
	uint8_t b0 = sg_pixel(s);

	sg_sav(s, b0, 1);
	sg_payload(s, size, tricky, out);
	sg_eav(s, b0, 1);
}

/*
 * Audio packet with @n payload bytes. The payload deliberately contains
 * marker-like sequences, which must not be scanned. Payload copied to @out.
 */
static inline void sg_audio(struct sg *s, uint32_t n, uint8_t *out)
{
	uint8_t h[8] = { 0x00, 0xFF, 0xFF, 0x58,
			 (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
	size_t start;
	uint32_t i;

	sg_put(s, h, sizeof(h));
	start = s->len;
	for (i = 0; i < n; i++) {
		uint32_t r = sg_rand(s);

		if (n - i >= 8 && r % 61 == 0) {
			static const uint8_t fake[8] = { 0x00, 0xFF, 0xFF, 0xC1, 1, 2, 3, 4 };

			sg_put(s, fake, sizeof(fake));
			i += 7;
		} else {
			sg_byte(s, (uint8_t)r);
		}
	}
	if (out)
		memcpy(out, s->buf + start, n);
}

/* Junk between frames, without 0x00 so it cannot start a marker. */
static inline void sg_junk(struct sg *s, size_t n)
{
	while (n--)
		sg_byte(s, sg_pixel(s));
}

#endif /* GC550_STREAMGEN_H */
