# SPDX-License-Identifier: GPL-2.0-only OR MIT
#
# Userspace unit tests for the hardware-independent parts of the driver.
# These never touch a device.
#
#   make -C tests            build and run all tests
#   make -C tests LGX_SRC=   skip the differential test against lgx2userspace

SRC	:= ../src
LGX_SRC	?= $(HOME)/Projects/gc550/lgx2userspace/src/liblgx/src
SAN	:= -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS	:= -std=gnu11 -O2 -g -Wall -Wextra -Werror $(SAN) -Iinclude -I$(SRC)
CXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra $(SAN) -Iinclude -I$(SRC) -I$(LGX_SRC)

TESTS := test-guard test-parser
ifneq ($(LGX_SRC),)
TESTS += diff-framebuilder
endif

all: run

test-guard: test-guard.c $(SRC)/gc550-guard.c $(SRC)/gc550-bootstrap.c $(SRC)/gc550-bootstrap-data.c
	$(CC) $(CFLAGS) -o $@ $^

test-parser: test-parser.c $(SRC)/gc550-parser.c
	$(CC) $(CFLAGS) -o $@ $^

gc550-parser.o: $(SRC)/gc550-parser.c
	$(CC) $(CFLAGS) -c -o $@ $<

# lgx2userspace's FrameBuilder (as of e5184dd) sets _carryLen for a chunk that
# ends in 00 / 00 FF / 00 FF FF but never copies those bytes into _carry, so a
# marker split there is lost and stale bytes enter the frame. Compare against a
# copy with that one-line fix; the kernel parser does not have the bug.
FrameBuilder-fixed.cpp: $(LGX_SRC)/FrameBuilder.cpp
	sed '/_carryLen = 3;$$/a\            if (_carryLen > 0) memcpy(_carry, buf + n - _carryLen, _carryLen); // test fix' $< > $@
	grep -q 'test fix' $@

diff-framebuilder: diff-framebuilder.cpp gc550-parser.o FrameBuilder-fixed.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

run: $(TESTS)
	./test-guard ../tools/recordings/gc550-f150158.txt pre-f150158-head.txt
	./test-parser
	$(if $(filter diff-framebuilder,$(TESTS)),./diff-framebuilder)

clean:
	rm -f test-guard test-parser diff-framebuilder FrameBuilder-fixed.cpp *.o

.PHONY: all run clean
