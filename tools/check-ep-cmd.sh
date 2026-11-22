#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only OR MIT
#
# Static safety check: only src/gc550-bridge.c may send anything to the card.
# Fails if any other source file creates an OUT or control pipe, sends a
# control message, or names the command OUT endpoint. The only exceptions are
# the endpoint's #define and comment in gc550.h and the descriptor check in
# gc550_check_endpoints(), which only reads the interface descriptor.
set -eu
cd "$(dirname "$0")/.."

pattern='usb_sndbulkpipe|usb_sndctrlpipe|usb_sndintpipe|usb_sndisocpipe|usb_rcvctrlpipe|usb_control_msg|usb_interrupt_msg|GC550_EP_CMD_OUT|USB_DIR_OUT'
bad=$(grep -nE "$pattern" src/*.c src/*.h \
	| grep -vE '^src/gc550-bridge\.c:' \
	| grep -vE '^src/gc550\.h:[0-9]+:#define GC550_EP_CMD_OUT' \
	| grep -vE '^src/gc550\.h:[0-9]+:[[:space:]]*/\* command bridge: the only path to GC550_EP_CMD_OUT \*/$' \
	| grep -vE '^src/gc550-core\.c:[0-9]+:[[:space:]]*cmd_out \|= ep->bEndpointAddress == GC550_EP_CMD_OUT;$' \
	|| true)
if [ -n "$bad" ]; then
	echo "check-ep-cmd: only src/gc550-bridge.c may send to the card:" >&2
	echo "$bad" >&2
	exit 1
fi

# The bridge must run every command through the guard before submitting.
grep -q 'gc550_guard_check(cmd, len)' src/gc550-bridge.c || {
	echo "check-ep-cmd: gc550-bridge.c no longer calls gc550_guard_check()" >&2
	exit 1
}
n=$(grep -c 'usb_sndbulkpipe' src/gc550-bridge.c)
[ "$n" -eq 1 ] || {
	echo "check-ep-cmd: expected exactly one OUT pipe in gc550-bridge.c, found $n" >&2
	exit 1
}
echo "check-ep-cmd: OK (one guarded OUT path, in gc550-bridge.c)"
