#!/bin/sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="${1:-$ROOT_DIR/build/ui-previews}"
BUILD_DIR="${TMPDIR:-/tmp}/crosspoint-library-home-preview"

mkdir -p "$OUT_DIR" "$BUILD_DIR"

c++ -std=c++20 -Wall -Wextra -Werror \
  -I"$ROOT_DIR/src" \
  -I"$ROOT_DIR/freeink-sdk/libs/ui/FreeInkUI/include" \
  -I"$ROOT_DIR/freeink-sdk/libs/ui/FreeInkUI/tools" \
  "$ROOT_DIR/freeink-sdk/libs/ui/FreeInkUI/src/FreeInkUI.cpp" \
  "$SCRIPT_DIR/render_library_home_preview.cpp" \
  -o "$BUILD_DIR/render_library_home_preview"

"$BUILD_DIR/render_library_home_preview" "$OUT_DIR"
printf 'Rendered library previews to %s\n' "$OUT_DIR"
