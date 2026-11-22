/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Minimal test assertions. */
#ifndef GC550_CHECK_H
#define GC550_CHECK_H

#include <stdio.h>

static int check_failures;
static int check_count;

#define CHECKF(cond, ...)						\
	do {								\
		check_count++;						\
		if (!(cond)) {						\
			check_failures++;				\
			fprintf(stderr, "%s:%d: FAILED: %s (", __FILE__,	\
				__LINE__, #cond);			\
			fprintf(stderr, __VA_ARGS__);			\
			fprintf(stderr, ")\n");				\
		}							\
	} while (0)

#define CHECK(cond) CHECKF(cond, "%s", "")

static inline int check_report(const char *name)
{
	printf("%s: %d checks, %d failed\n", name, check_count, check_failures);
	return check_failures ? 1 : 0;
}

#endif /* GC550_CHECK_H */
