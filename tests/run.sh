#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
test_bin=$(mktemp "${TMPDIR:-/tmp}/tpkbd-scroll-test.XXXXXX")
trap 'rm -f "$test_bin"' EXIT
clang -O1 -g -Wall -Wextra -fsanitize=address,undefined \
    -framework CoreFoundation -framework IOKit \
    -framework CoreGraphics -framework ApplicationServices -framework CoreVideo \
    tests/scroll_test.c -o "$test_bin"
UBSAN_OPTIONS=halt_on_error=1 "$test_bin"
