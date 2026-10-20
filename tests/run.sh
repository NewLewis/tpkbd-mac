#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
test_bin=$(mktemp "${TMPDIR:-/tmp}/tpkbd-scroll-test.XXXXXX")
native_test_bin=$(mktemp "${TMPDIR:-/tmp}/tpkbd-magnify-test.XXXXXX")
trap 'rm -f "$test_bin" "$native_test_bin"' EXIT
clang -O1 -g -Wall -Wextra -fsanitize=address,undefined \
    -framework CoreFoundation -framework IOKit \
    -framework CoreGraphics -framework ApplicationServices -framework CoreVideo \
    tests/scroll_test.c -o "$test_bin"
UBSAN_OPTIONS=halt_on_error=1 "$test_bin"

clang -O1 -g -Wall -Wextra -fsanitize=address,undefined \
    -framework AppKit tests/magnify_native_test.m -o "$native_test_bin"
UBSAN_OPTIONS=halt_on_error=1 "$native_test_bin"
