/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * AVerMedia Live Gamer Extreme (GC550) command guard.
 *
 * Every command the driver sends to the card's command bridge (bulk OUT 0x01)
 * must pass gc550_guard_check() first. The allow-list is exactly what the
 * Windows driver's own bootstrap sends to this card: I2C transactions to the
 * board's volatile chips, one board-info read and the single-byte 0x08.
 * Anything else - in particular the AT88SC CryptoMemory transport (0x05,
 * 0x06, 0x07, 0x09), config-memory writes (0x0D, 0x15) and any flash or
 * unknown opcode - is refused, because a locked AT88SC makes the card
 * permanently unusable with the official driver.
 */
#ifndef GC550_GUARD_H
#define GC550_GUARD_H

#include <linux/types.h>

/* Largest I2C payload or read in the recording (an EDID block). */
#define GC550_I2C_MAX_DATA	32

enum gc550_guard_verdict {
	GC550_GUARD_OK = 0,
	GC550_GUARD_EMPTY,
	GC550_GUARD_BAD_WRITE,		/* malformed 0x01 I2C write */
	GC550_GUARD_WRITE_ADDR,		/* I2C write to an address never written */
	GC550_GUARD_BAD_READ,		/* malformed 0x02 I2C read */
	GC550_GUARD_READ_ADDR,		/* I2C read from an address never read */
	GC550_GUARD_MEMORY_READ,	/* 0x0E other than the board-info read */
	GC550_GUARD_OPCODE,		/* CryptoMemory, config write, flash, unknown */
};

enum gc550_guard_verdict gc550_guard_check(const u8 *cmd, size_t len);
const char *gc550_guard_reason(enum gc550_guard_verdict verdict);

/*
 * Number of reply bytes the bridge returns on bulk IN 0x81 for an allowed
 * command: <count> for an I2C read, the big-endian length in the board-info
 * read, 0 otherwise. Only meaningful for commands that passed the guard.
 */
size_t gc550_reply_len(const u8 *cmd, size_t len);

#endif /* GC550_GUARD_H */
