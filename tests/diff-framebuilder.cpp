// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Differential test: feed the same random streams, in the same random splits,
 * to lgx2userspace's FrameBuilder (proven on real hardware) and to the kernel
 * driver's parser, and require identical frames and audio.
 *
 * Streams contain only cases where the two are meant to agree: audio between
 * frames (not inside a SAV->EAV span), spans of exactly one frame or far from
 * it, junk, bad SAV/EAV checksums, stray markers and marker-like video data.
 *
 * Build: needs LGX_SRC pointing at lgx2userspace/src/liblgx/src.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "FrameBuilder.h"

extern "C" {
#include "gc550-parser.h"
#include "streamgen.h"
}

namespace {

struct Capture {
	std::vector<std::vector<uint8_t>> frames;
	std::vector<uint8_t> audio;
	std::vector<uint8_t> buffer = std::vector<uint8_t>(GC550_FRAME_BUFFER_BYTES);
};

u8 *frameBuffer(void *priv, size_t *cap)
{
	auto *c = static_cast<Capture *>(priv);
	*cap = c->buffer.size();
	return c->buffer.data();
}

void frameDone(void *priv, size_t bytes)
{
	auto *c = static_cast<Capture *>(priv);
	c->frames.emplace_back(c->buffer.begin(), c->buffer.begin() + bytes);
}

void audio(void *priv, const u8 *data, size_t len)
{
	auto *c = static_cast<Capture *>(priv);
	c->audio.insert(c->audio.end(), data, data + len);
}

const gc550_parser_ops ops = { frameBuffer, frameDone, audio };

void build(struct sg *g)
{
	int frames = 12 + sg_rand(g) % 8;

	sg_junk(g, sg_rand(g) % 5000);
	for (int i = 0; i < frames; i++) {
		uint32_t kind = sg_rand(g) % 10;
		uint8_t b0 = sg_pixel(g);

		if (kind == 0) {
			sg_frame(g, 1000 + sg_rand(g) % 100000, 1, nullptr);	/* short span */
		} else if (kind == 1) {
			sg_sav(g, b0, 1);					/* bad EAV */
			sg_payload(g, GC550_FRAME_BYTES, 1, nullptr);
			sg_eav(g, b0, 0);
		} else if (kind == 2) {
			sg_sav(g, b0, 0);					/* bad SAV */
			sg_payload(g, 5000, 1, nullptr);
			sg_eav(g, b0, 1);					/* -> stray EAV */
		} else if (kind == 3) {
			sg_sav(g, b0, 1);					/* SAV/EAV mismatch */
			sg_payload(g, GC550_FRAME_BYTES, 1, nullptr);
			sg_eav(g, (uint8_t)(b0 + 1), 1);
		} else {
			sg_frame(g, GC550_FRAME_BYTES, 1, nullptr);
		}
		if (sg_rand(g) % 2)
			sg_audio(g, sg_rand(g) % 8000, nullptr);
		sg_junk(g, sg_rand(g) % 64);
	}
}

}  // namespace

int main()
{
	int failures = 0;

	for (uint32_t seed = 1; seed <= 12; seed++) {
		struct sg g;
		sg_init(&g, seed * 2654435761u);
		build(&g);

		Capture ref, ours;
		utils::FrameBuilder fb;
		fb.setVideoFrameCallback([&](const uint8_t *data, uint32_t len) {
			ref.frames.emplace_back(data, data + len);
		});
		fb.setAudioFrameCallback([&](const uint8_t *data, uint32_t len) {
			ref.audio.insert(ref.audio.end(), data, data + len);
		});
		struct gc550_parser p;
		gc550_parser_init(&p, &ops, &ours);

		uint32_t split = seed;
		std::vector<uint8_t> chunk;
		for (size_t pos = 0; pos < g.len;) {
			split = split * 1103515245u + 12345u;
			size_t n = (seed % 3 == 0) ? 1 + (split >> 8) % 64 : 1 + (split >> 8) % (3 << 20);
			if (n > g.len - pos)
				n = g.len - pos;
			chunk.assign(g.buf + pos, g.buf + pos + n);
			fb.feed(chunk.data(), static_cast<uint32_t>(n), false);
			gc550_parser_feed(&p, g.buf + pos, n);
			pos += n;
		}

		bool ok = ref.frames == ours.frames && ref.audio == ours.audio;
		printf("seed %2u: %zu bytes, FrameBuilder %zu frames / %zu audio B, gc550 %zu frames / %zu audio B: %s\n",
		       seed, g.len, ref.frames.size(), ref.audio.size(), ours.frames.size(),
		       ours.audio.size(), ok ? "same" : "DIFFERENT");
		if (!ok)
			failures++;
		sg_free(&g);
	}
	printf("diff-framebuilder: %d of 12 streams differ\n", failures);
	return failures ? 1 : 0;
}
