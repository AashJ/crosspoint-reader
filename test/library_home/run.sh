#!/bin/sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${TMPDIR:-/tmp}/crosspoint-library-home-test"

mkdir -p "$BUILD_DIR"

c++ -std=c++20 -Wall -Wextra -Werror \
  -I"$ROOT_DIR/src" \
  -I"$ROOT_DIR/freeink-sdk/libs/ui/FreeInkUI/include" \
  "$ROOT_DIR/freeink-sdk/libs/ui/FreeInkUI/src/FreeInkUI.cpp" \
  "$SCRIPT_DIR/LibraryHomeScreenTest.cpp" \
  -o "$BUILD_DIR/library_home_test"

"$BUILD_DIR/library_home_test"
