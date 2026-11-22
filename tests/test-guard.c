// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Unit tests for the command guard and the bootstrap table. Ported from
 * lgx2userspace's CommandGuard.t.cpp, plus checks that the generated table
 * is byte-for-byte the recording.
 *
 * Usage: test-guard <recording tokens> <pre-f150158 head tokens>
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc550-bootstrap.h"
#include "gc550-guard.h"
#include "check.h"

#define CMD(...) ((const u8[]){ __VA_ARGS__ }), sizeof((const u8[]){ __VA_ARGS__ })

static bool allowed(const u8 *cmd, size_t len)
{
	return gc550_guard_check(cmd, len) == GC550_GUARD_OK;
}

static void test_single_byte_opcodes(void)
{
	int op;

	CHECK(gc550_guard_check(NULL, 0) == GC550_GUARD_EMPTY);
	CHECK(gc550_guard_check((const u8 *)"\x08", 0) == GC550_GUARD_EMPTY);
	for (op = 0; op < 256; op++) {
		u8 cmd = (u8)op;

		CHECKF(allowed(&cmd, 1) == (op == 0x08), "single-byte opcode %02X", op);
	}
	/* the GC551's single-byte command is not the GC550's */
	CHECK(!allowed(CMD(0x14)));
}

static void test_crypto_and_memory(void)
{
	/* From the GC550 recording before the CryptoMemory section was removed. */
	CHECK(!allowed(CMD(0x05, 0xB8, 0x00, 0x00, 0x0A)));		/* security reset */
	CHECK(!allowed(CMD(0x07, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)));
	CHECK(!allowed(CMD(0x05, 0xB6, 0x00, 0x00, 0x08)));		/* read config zone */
	CHECK(!allowed(CMD(0x06, 0x08)));
	CHECK(!allowed(CMD(0x05, 0xB6, 0x00, 0x50, 0x01)));		/* attempt counters */
	CHECK(!allowed(CMD(0x06, 0x01)));
	CHECK(!allowed(CMD(0x05, 0xB8, 0x01, 0x00, 0x10)));		/* verify crypto */
	CHECK(!allowed(CMD(0x05, 0xBA, 0x02, 0x00, 0x03)));		/* verify password */
	CHECK(!allowed(CMD(0x05, 0xB4, 0x03, 0x00, 0x00)));		/* select user zone */
	CHECK(!allowed(CMD(0x09, 0x0A)));
	/* config-memory writes and any other memory read */
	CHECK(!allowed(CMD(0x0D, 0x02, 0x07, 0xFE, 0x01, 0x00, 0xAA)));
	CHECK(!allowed(CMD(0x15, 0x02, 0x07, 0xFE, 0xAA)));
	CHECK(!allowed(CMD(0x0E, 0x02, 0x07, 0x00, 0x02, 0x00)));
	CHECK(!allowed(CMD(0x0E, 0x02, 0x07, 0xFE, 0x02, 0x00, 0x00)));
	CHECK(allowed(CMD(0x0E, 0x02, 0x07, 0xFE, 0x02, 0x00)));
	CHECK(gc550_reply_len(CMD(0x0E, 0x02, 0x07, 0xFE, 0x02, 0x00)) == 512);
}

static void test_malformed_i2c(void)
{
	u8 too_long[4 + 33] = { 0x01, 0x36, 0x01, 0x00 };
	u8 max_len[4 + 32] = { 0x01, 0x36, 0x01, 0x00 };

	CHECK(!allowed(CMD(0x01, 0x20, 0x01, 0x02)));			/* no data */
	CHECK(!allowed(CMD(0x01, 0x20, 0x02, 0x00, 0x02, 0x00)));	/* 2-byte register */
	CHECK(!allowed(CMD(0x02, 0x20, 0x01, 0x00, 0x02)));		/* zero-length read */
	CHECK(!allowed(CMD(0x02, 0x20, 0x01, 0x21, 0x02)));		/* too long */
	CHECK(!allowed(CMD(0x02, 0x20, 0x01, 0x01, 0x02, 0x00)));	/* trailing byte */
	CHECK(!allowed(CMD(0x02, 0x20, 0x02, 0x01, 0x02)));		/* 2-byte register */
	CHECK(!allowed(too_long, sizeof(too_long)));
	CHECK(allowed(max_len, sizeof(max_len)));
	CHECK(gc550_reply_len(CMD(0x02, 0x22, 0x01, 0x02, 0xB1)) == 2);
	CHECK(gc550_reply_len(CMD(0x01, 0x22, 0x01, 0x3C, 0x00)) == 0);
}

static void test_addresses(void)
{
	int a;

	/* EEPROMs, and AT88SC command bytes B0-BA as 7-bit addresses */
	for (a = 0x50; a <= 0x5F; a++) {
		CHECKF(!allowed(CMD(0x01, (u8)a, 0x01, 0x00, 0x00)), "write to %02X", a);
		CHECKF(!allowed(CMD(0x02, (u8)a, 0x01, 0x01, 0x00)), "read from %02X", a);
	}
	/* every address outside the recorded sets */
	for (a = 0; a < 256; a++) {
		static const u8 w[] = { 0x10, 0x18, 0x20, 0x22, 0x26, 0x32, 0x34, 0x36,
					0x38, 0x3D, 0x4A, 0x65, 0x74 };
		static const u8 r[] = { 0x10, 0x20, 0x22, 0x26, 0x32, 0x34, 0x3D, 0x3F,
					0x46, 0x4A, 0x74 };
		bool in_w = memchr(w, a, sizeof(w)) != NULL;
		bool in_r = memchr(r, a, sizeof(r)) != NULL;

		CHECKF(allowed(CMD(0x01, (u8)a, 0x01, 0x00, 0x00)) == in_w, "write %02X", a);
		CHECKF(allowed(CMD(0x02, (u8)a, 0x01, 0x01, 0x00)) == in_r, "read %02X", a);
	}
	/* runtime access the driver needs */
	CHECK(allowed(CMD(0x02, 0x20, 0x01, 0x01, 0xFA)));	/* board-state detection */
	CHECK(allowed(CMD(0x02, 0x74, 0x01, 0x02, 0x06)));
	CHECK(allowed(CMD(0x01, 0x20, 0x01, 0x02, 0xF0)));	/* auto input colour space */
	CHECK(allowed(CMD(0x01, 0x22, 0x01, 0x3C, 0x00)));	/* picture adjustment */
	CHECK(allowed(CMD(0x02, 0x46, 0x01, 0x01, 0x01)));	/* InfoFrame status poll */
	CHECK(allowed(CMD(0x02, 0x34, 0x01, 0x0D, 0x2A)));
	CHECK(!allowed(CMD(0x01, 0x46, 0x01, 0x01, 0x00)));	/* never written */
}

/* Parse lgx2userspace tokens into the table format. Returns record count. */
static size_t tokens_to_stream(const char *path, u8 **out, size_t *size)
{
	FILE *f = fopen(path, "r");
	size_t cap = 1 << 16, len = 0, count = 0;
	char tok[1024];
	u8 *buf = malloc(cap);

	if (!f || !buf) {
		fprintf(stderr, "cannot read %s\n", path);
		exit(2);
	}
	while (fscanf(f, "%1023s", tok) == 1) {
		size_t n, i;

		if (tok[0] == '<')
			continue;
		n = (strlen(tok) - 1) / 2;
		if (len + 1 + n > cap)
			buf = realloc(buf, cap *= 2);
		buf[len++] = (u8)n;
		for (i = 0; i < n; i++) {
			unsigned int v;

			sscanf(tok + 1 + 2 * i, "%2x", &v);
			buf[len++] = (u8)v;
		}
		count++;
	}
	fclose(f);
	*out = buf;
	*size = len;
	return count;
}

/* The recorded reply lengths must match gc550_reply_len(). */
static void check_reply_lengths(const char *path)
{
	FILE *f = fopen(path, "r");
	char tok[1024], prev[1024] = "";
	unsigned int index = 0;

	while (f && fscanf(f, "%1023s", tok) == 1) {
		if (tok[0] == '<') {
			u8 cmd[256];
			size_t n = (strlen(prev) - 1) / 2, i;

			for (i = 0; i < n; i++) {
				unsigned int v;

				sscanf(prev + 1 + 2 * i, "%2x", &v);
				cmd[i] = (u8)v;
			}
			CHECKF(gc550_reply_len(cmd, n) == (size_t)atoi(tok + 1),
			       "reply length of transaction %u", index - 1);
		} else {
			strcpy(prev, tok);
			index++;
		}
	}
	if (f)
		fclose(f);
}

static void test_bootstrap_table(const char *recording)
{
	enum gc550_guard_verdict v;
	unsigned int bad = 0;
	size_t size, count;
	u8 *ref;

	count = tokens_to_stream(recording, &ref, &size);
	CHECK(count == 6576);
	CHECK(gc550_bootstrap_count == count);
	/* byte for byte the recording, in the same order */
	CHECK(gc550_bootstrap_size == size);
	CHECK(size == gc550_bootstrap_size && !memcmp(ref, gc550_bootstrap, size));
	CHECK(gc550_bootstrap_validate(gc550_bootstrap, gc550_bootstrap_size, &bad, &v) == 0);
	check_reply_lengths(recording);
	free(ref);
}

static void test_bad_streams(const char *old_head)
{
	enum gc550_guard_verdict v;
	unsigned int bad = 0;
	size_t size;
	u8 *old;
	u8 *mixed;

	/* The pre-f150158 recording is refused at its first CryptoMemory command. */
	tokens_to_stream(old_head, &old, &size);
	CHECK(gc550_bootstrap_validate(old, size, &bad, &v) == -EPERM);
	CHECKF(bad == 20, "refused at %u", bad);
	CHECK(v == GC550_GUARD_OPCODE);
	CHECK(old[0] == 6);	/* sanity: record 0 is the board-info read */
	free(old);

	/* A bad command at the very end is still found before anything is sent. */
	mixed = malloc(gc550_bootstrap_size + 6);
	memcpy(mixed, gc550_bootstrap, gc550_bootstrap_size);
	memcpy(mixed + gc550_bootstrap_size, "\x05\x05\xB6\x00\x50\x01", 6);
	CHECK(gc550_bootstrap_validate(mixed, gc550_bootstrap_size + 6, &bad, &v) == -EPERM);
	CHECK(bad == gc550_bootstrap_count);

	/* Framing: a record running past the end, and a zero-length record. */
	CHECK(gc550_bootstrap_validate(mixed, gc550_bootstrap_size + 3, &bad, &v) == -EINVAL);
	CHECK(gc550_bootstrap_validate((const u8 *)"\x00", 1, &bad, &v) == -EINVAL);
	CHECK(gc550_bootstrap_validate((const u8 *)"", 0, &bad, &v) == 0);
	free(mixed);
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s <recording> <pre-f150158 head>\n", argv[0]);
		return 2;
	}
	test_single_byte_opcodes();
	test_crypto_and_memory();
	test_malformed_i2c();
	test_addresses();
	test_bootstrap_table(argv[1]);
	test_bad_streams(argv[2]);
	return check_report("test-guard");
}
