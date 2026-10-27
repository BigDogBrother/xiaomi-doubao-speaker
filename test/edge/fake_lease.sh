#!/bin/sh
set -eu
case "$2" in ''|*[!a-f0-9]*) exit 30 ;; esac
[ "${#2}" -eq 32 ] || exit 30
printf '%s\n' "$1" >> "$EDGE_TEST_DIR/events"
case "$1" in
  begin|end) exit 0 ;;
  heartbeat) [ ! -f "$EDGE_TEST_DIR/fail-heartbeat" ] ;;
  *) exit 1 ;;
esac
