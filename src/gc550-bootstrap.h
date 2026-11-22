/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * AVerMedia Live Gamer Extreme (GC550) bootstrap table.
 *
 * The table is a flat stream of records: one length byte, then that many
 * command bytes for bulk OUT 0x01. Reply lengths follow from the command
 * (gc550_reply_len()).
 */
#ifndef GC550_BOOTSTRAP_H
#define GC550_BOOTSTRAP_H

#include <linux/types.h>

#include "gc550-guard.h"

extern const u8 gc550_bootstrap[];
extern const size_t gc550_bootstrap_size;
extern const unsigned int gc550_bootstrap_count;

/*
 * Fetch the record at *pos and advance *pos past it.
 * Returns 1 for a record, 0 at the end of the stream, -EINVAL if the record
 * runs past the end or has length 0.
 */
int gc550_bootstrap_next(const u8 *stream, size_t size, size_t *pos,
			 const u8 **cmd, size_t *len);

/*
 * Check the framing of the whole stream and run every command through the
 * guard, without sending anything. Returns 0 if every record is allowed;
 * otherwise -EINVAL (framing) or -EPERM (guard) with the failing record's
 * index in *bad_index and, for -EPERM, the verdict in *verdict.
 */
int gc550_bootstrap_validate(const u8 *stream, size_t size,
			     unsigned int *bad_index,
			     enum gc550_guard_verdict *verdict);

#endif /* GC550_BOOTSTRAP_H */
