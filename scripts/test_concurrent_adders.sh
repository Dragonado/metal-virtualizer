#!/usr/bin/env bash

set -uo pipefail

readonly CLIENT_COUNT=10
readonly PORT=50051

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

bazel build //src:adder

log_dir="$(mktemp -d "${TMPDIR:-/tmp}/metal-virtualizer-concurrency.XXXXXX")"
server_pid=""
test_passed=false

cleanup() {
    if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
        kill "$server_pid"
        wait "$server_pid" 2>/dev/null || true
    fi

    if "$test_passed"; then
        rm -rf "$log_dir"
    else
        echo "Logs kept in $log_dir"
    fi
}
trap cleanup EXIT INT TERM

declare -a client_pids
for ((i = 1; i <= CLIENT_COUNT; ++i)); do
    bazel-bin/src/adder >"$log_dir/adder-$i.log" 2>&1 &
    client_pids[i]=$!
done

failures=0
for ((i = 1; i <= CLIENT_COUNT; ++i)); do
    if ! wait "${client_pids[i]}"; then
        echo "FAIL: adder $i exited unsuccessfully"
        failures=$((failures + 1))
        continue
    fi

    if ! grep -qF "OK: Computation is correct" "$log_dir/adder-$i.log"; then
        echo "FAIL: adder $i did not verify its result"
        failures=$((failures + 1))
    fi
done

if ((failures > 0)); then
    echo "FAIL: $failures of $CLIENT_COUNT adders failed"
    exit 1
fi

test_passed=true
echo "PASS: all $CLIENT_COUNT adders computed the correct result"
