#!/usr/bin/env bash
#
# Build, then run the program. Args after `--` (or any args) pass through to it.
#   ./run.sh                 # build + run
#   OPT=-O2 ./run.sh 1024    # release build, pass 1024 to the program
#
set -euo pipefail
cd "$(dirname "$0")"

OUT="${OUT:-build/metalstorm}"  # keep in sync with build.sh's default

OUT="$OUT" ./build.sh           # bails here if the build fails

printf '\n──────────── run ────────────\n'
echo "+ $OUT $*"
exec "$OUT" "$@"
