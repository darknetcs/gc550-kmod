// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * AVerMedia Live Gamer Extreme (GC550) bootstrap table walking and checking.
 * No USB dependencies, so it can be unit-tested in userspace.
 */
#include <linux/errno.h>

#include "gc550-bootstrap.h"

int gc550_bootstrap_next(const u8 *stream, size_t size, size_t *pos,
			 const u8 **cmd, size_t *len)
{
	size_t n;

	if (*pos >= size)
		return 0;
	n = stream[*pos];
	if (!n || n > size - *pos - 1)
		return -EINVAL;
	*cmd = stream + *pos + 1;
	*len = n;
	*pos += 1 + n;
	return 1;
}

int gc550_bootstrap_validate(const u8 *stream, size_t size,
			     unsigned int *bad_index,
			     enum gc550_guard_verdict *verdict)
{
	enum gc550_guard_verdict v;
	unsigned int index = 0;
	size_t pos = 0, len;
	const u8 *cmd;
	int ret;

	*verdict = GC550_GUARD_OK;
	while ((ret = gc550_bootstrap_next(stream, size, &pos, &cmd, &len)) > 0) {
		v = gc550_guard_check(cmd, len);
		if (v != GC550_GUARD_OK) {
			*bad_index = index;
			*verdict = v;
			return -EPERM;
		}
		index++;
	}
	if (ret < 0) {
		*bad_index = index;
		return ret;
	}
	return 0;
}
