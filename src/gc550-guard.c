// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * AVerMedia Live Gamer Extreme (GC550) command guard.
 *
 * Ported from lgx2userspace's CommandGuard (MIT, Copyright 2021 Chris Sawczuk
 * and contributors). This file has no USB or kernel-object dependencies so it
 * can be unit-tested in userspace.
 */
#include <linux/kernel.h>
#include <linux/string.h>

#include "gc550-guard.h"

/*
 * 7-bit I2C addresses the recorded Windows bootstrap writes / reads. Runtime
 * register access (board-state detection, colour-space fix, status poll)
 * stays inside these sets.
 *
 *   0x10 FPGA          0x18 TLV320AIC3x codec   0x20 ADV7604 IO
 *   0x22 ADV7604 CP    0x26 ADV7604 AFE         0x32 ADV7604 repeater
 *   0x34 ADV7604 HDMI  0x36 ADV7604 EDID        0x38 ADV7604 ESDP
 *   0x3D ADV7511       0x3F (read only)         0x46 ADV7604 InfoFrame
 *   0x4A audio mixer   0x65 (unknown)           0x74 PCA9539A GPIO
 *
 * Nothing in 0x50-0x5F (EEPROMs; also AT88SC command bytes B0-BA seen as
 * 7-bit addresses) is in either set.
 */
static const u8 gc550_writable[] = {
	0x10, 0x18, 0x20, 0x22, 0x26, 0x32, 0x34, 0x36, 0x38, 0x3D, 0x4A, 0x65, 0x74,
};

static const u8 gc550_readable[] = {
	0x10, 0x20, 0x22, 0x26, 0x32, 0x34, 0x3D, 0x3F, 0x46, 0x4A, 0x74,
};

static const u8 gc550_board_info_read[] = { 0x0E, 0x02, 0x07, 0xFE, 0x02, 0x00 };

#define GC550_SINGLE_BYTE_CMD	0x08

static bool gc550_in_set(const u8 *set, size_t n, u8 addr)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (set[i] == addr)
			return true;
	return false;
}

enum gc550_guard_verdict gc550_guard_check(const u8 *cmd, size_t len)
{
	if (!cmd || !len)
		return GC550_GUARD_EMPTY;

	switch (cmd[0]) {
	case 0x01:	/* 01 <addr> 01 <reg> <data...> */
		if (len < 5 || cmd[2] != 0x01 || len - 4 > GC550_I2C_MAX_DATA)
			return GC550_GUARD_BAD_WRITE;
		if (!gc550_in_set(gc550_writable, ARRAY_SIZE(gc550_writable), cmd[1]))
			return GC550_GUARD_WRITE_ADDR;
		return GC550_GUARD_OK;
	case 0x02:	/* 02 <addr> 01 <count> <reg> */
		if (len != 5 || cmd[2] != 0x01 || cmd[3] == 0 ||
		    cmd[3] > GC550_I2C_MAX_DATA)
			return GC550_GUARD_BAD_READ;
		if (!gc550_in_set(gc550_readable, ARRAY_SIZE(gc550_readable), cmd[1]))
			return GC550_GUARD_READ_ADDR;
		return GC550_GUARD_OK;
	case 0x0E:
		if (len == sizeof(gc550_board_info_read) &&
		    !memcmp(cmd, gc550_board_info_read, len))
			return GC550_GUARD_OK;
		return GC550_GUARD_MEMORY_READ;
	default:
		if (len == 1 && cmd[0] == GC550_SINGLE_BYTE_CMD)
			return GC550_GUARD_OK;
		return GC550_GUARD_OPCODE;
	}
}

const char *gc550_guard_reason(enum gc550_guard_verdict verdict)
{
	switch (verdict) {
	case GC550_GUARD_OK:
		return "allowed";
	case GC550_GUARD_EMPTY:
		return "empty command";
	case GC550_GUARD_BAD_WRITE:
		return "malformed I2C write";
	case GC550_GUARD_WRITE_ADDR:
		return "I2C write to an address the bootstrap never writes";
	case GC550_GUARD_BAD_READ:
		return "malformed I2C read";
	case GC550_GUARD_READ_ADDR:
		return "I2C read from an address the bootstrap never reads";
	case GC550_GUARD_MEMORY_READ:
		return "memory read other than the board-info read";
	case GC550_GUARD_OPCODE:
		return "opcode not allowed (CryptoMemory, config write or unknown)";
	}
	return "unknown verdict";
}

size_t gc550_reply_len(const u8 *cmd, size_t len)
{
	if (len == 5 && cmd[0] == 0x02)
		return cmd[3];
	if (len == sizeof(gc550_board_info_read) && cmd[0] == 0x0E)
		return ((size_t)cmd[4] << 8) | cmd[5];
	return 0;
}
