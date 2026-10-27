#!/bin/sh
set -eu
scratch=$(mktemp -d "${TMPDIR:-/tmp}/doubao-edge-test.XXXXXXXX")
trap 'rm -rf -- "$scratch"' EXIT HUP INT TERM
"$1" "$2" "$scratch"
