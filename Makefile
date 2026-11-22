# SPDX-License-Identifier: GPL-2.0-only OR MIT
#
# Out-of-tree build of the GC550 driver. Building never loads anything.
#
#   make              check, then build gc550.ko against the running kernel
#   make KDIR=...     build against another kernel tree
#   make test         userspace unit tests (no hardware)
#   make checkpatch   kernel coding-style check

KDIR ?= /lib/modules/$(shell uname -r)/build

all: check-ep
	$(MAKE) -C $(KDIR) M=$(CURDIR) W=1 modules

check-ep:
	tools/check-ep-cmd.sh

test:
	$(MAKE) -C tests

checkpatch:
	perl $(KDIR)/scripts/checkpatch.pl --no-tree --strict --terse -f src/*.[ch]

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	$(MAKE) -C tests clean

.PHONY: all check-ep test checkpatch clean
