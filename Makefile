# SPDX-License-Identifier: GPL-2.0-only OR MIT
#
# Out-of-tree build of the GC550 driver. Building never loads anything.
#
#   make              check, then build gc550.ko against the running kernel
#   make KDIR=...     build against another kernel tree
#   make test         userspace unit tests (no hardware)
#   make checkpatch   kernel coding-style check
#   sudo make dkms-install    install as a DKMS package (rebuilt and signed
#                             for every kernel, loaded when the card is plugged in)
#   sudo make dkms-uninstall  remove it again

KDIR ?= /lib/modules/$(shell uname -r)/build
VERSION := $(shell sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' dkms.conf)
DKMS_SRC := /usr/src/gc550-$(VERSION)
# Everything a DKMS build needs, and nothing else.
DKMS_FILES := dkms.conf Kbuild LICENSE README.md DESIGN.md tools/check-ep-cmd.sh \
	$(wildcard src/*.c src/*.h)

all: check-ep
	$(MAKE) -C $(KDIR) M=$(CURDIR) W=1 modules

check-ep:
	tools/check-ep-cmd.sh

test:
	$(MAKE) -C tests

checkpatch:
	perl $(KDIR)/scripts/checkpatch.pl --no-tree --strict --terse -f src/*.[ch]

# Versions of gc550 that DKMS knows about, e.g. "0.1.0 0.1.1".
DKMS_VERSIONS = $$(dkms status -m gc550 2>/dev/null | sed -n 's|^gc550/\([^,]*\),.*|\1|p' | sort -u)

# Copies sources only; the build itself runs under DKMS, never in this tree.
# Any older installed version is removed first, so this also upgrades.
dkms-install:
	@test "$$(id -u)" = 0 || { echo "run as: sudo make dkms-install"; exit 1; }
	tools/check-ep-cmd.sh
	@for v in $(DKMS_VERSIONS); do \
		echo "removing installed gc550/$$v"; \
		dkms remove -m gc550 -v $$v --all && rm -rf /usr/src/gc550-$$v || exit 1; \
	done
	rm -rf $(DKMS_SRC)
	for f in $(DKMS_FILES); do install -D -m 644 $$f $(DKMS_SRC)/$$f; done
	chmod 755 $(DKMS_SRC)/tools/check-ep-cmd.sh
	dkms add -m gc550 -v $(VERSION)
	dkms install -m gc550 -v $(VERSION)
	@echo "Installed gc550/$(VERSION). Unplug and replug the GC550 to load it."

dkms-uninstall:
	@test "$$(id -u)" = 0 || { echo "run as: sudo make dkms-uninstall"; exit 1; }
	-modprobe -r gc550
	@for v in $(DKMS_VERSIONS); do \
		dkms remove -m gc550 -v $$v --all; rm -rf /usr/src/gc550-$$v; \
	done

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	$(MAKE) -C tests clean

.PHONY: all check-ep test checkpatch dkms-install dkms-uninstall clean
