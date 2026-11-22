/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Userspace stand-in for <linux/kernel.h>, for the unit tests only. */
#ifndef GC550_TEST_LINUX_KERNEL_H
#define GC550_TEST_LINUX_KERNEL_H
#include <linux/types.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#endif
