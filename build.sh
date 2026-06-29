#!/usr/bin/env bash
#
# Build the metal-cpp program with strict warnings on *your* code.
#
# Overridable via env, e.g.:  WERROR=1 OPT=-O2 SRC=main.cpp ./build.sh
#
set -euo pipefail
cd "$(dirname "$0")"

# --- config ---------------------------------------------------------------
CXX="${CXX:-clang++}"
SRC="${SRC:-main.cpp}"
OUT_DIR="build"
OUT="${OUT:-$OUT_DIR/metalstorm}"
METAL_CPP_DIR="${METAL_CPP_DIR:-metal-cpp}"  # vendored Apple metal-cpp headers
STD="${STD:-c++17}"                          # metal-cpp's documented minimum
OPT="${OPT:--O0}"                            # -O0 for lldb stepping; -O2 for timing milestones

# Strict on your code. metal-cpp is pulled in via -isystem below so its own
# header warnings don't drown yours. Ratchet up later: add -Wconversion
# -Wsign-conversion once your index math is clean.
WARNINGS=(-Wall -Wextra -Wpedantic -Wshadow)
[[ "${WERROR:-0}" == "1" ]] && WARNINGS+=(-Werror)

FRAMEWORKS=(-framework Metal -framework Foundation -framework QuartzCore)

# --- preflight ------------------------------------------------------------
if [[ ! -f "$METAL_CPP_DIR/Metal/Metal.hpp" ]]; then
  cat >&2 <<EOF
error: metal-cpp headers not found at '$METAL_CPP_DIR/'.

Download from https://developer.apple.com/metal/cpp/ , unzip, and place so that
  $METAL_CPP_DIR/Metal/Metal.hpp
exists. Or point the script elsewhere:  METAL_CPP_DIR=/path/to/metal-cpp ./build.sh
EOF
  exit 1
fi

if [[ ! -s "$SRC" ]]; then
  echo "error: source '$SRC' is missing or empty. Nothing to build yet." >&2
  exit 1
fi

# --- build ----------------------------------------------------------------
mkdir -p "$OUT_DIR"

CMD=("$CXX"
  -std="$STD" "$OPT" -g
  "${WARNINGS[@]}"
  -isysroot "$(xcrun --show-sdk-path)"
  -isystem "$METAL_CPP_DIR"
  "$SRC" -o "$OUT"
  "${FRAMEWORKS[@]}")

echo "+ ${CMD[*]}"
"${CMD[@]}"
echo "built: $OUT"
