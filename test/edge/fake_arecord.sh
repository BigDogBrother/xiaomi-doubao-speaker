#!/bin/sh
printf '%s\n' "$@" > "$EDGE_RECORDER_TEST_DIR/argv"
/bin/dd if=/dev/zero bs=1280 count=3 2>/dev/null
