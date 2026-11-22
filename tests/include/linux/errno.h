/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Userspace stand-in for the kernel's <linux/errno.h>, for the unit tests
 * only. Defined here rather than via <errno.h>, which itself includes
 * <linux/errno.h> and would find this file again.
 */
#ifndef GC550_TEST_LINUX_ERRNO_H
#define GC550_TEST_LINUX_ERRNO_H
#ifndef EPERM
#define EPERM	1
#endif
#ifndef EINVAL
#define EINVAL	22
#endif
#endif
