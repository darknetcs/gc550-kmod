/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Userspace stand-in for <linux/math64.h>, for the unit tests only. */
#ifndef GC550_TEST_LINUX_MATH64_H
#define GC550_TEST_LINUX_MATH64_H
#include <linux/types.h>
static inline u64 div_u64(u64 a, u32 b) { return a / b; }
static inline u64 div64_u64(u64 a, u64 b) { return a / b; }
#endif
