#!/usr/bin/env bash

set -uo pipefail

readonly CLIENT_COUNT=100

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

mode="remote"
bazel_args=(--//src:shim=true)

case "${1:-}" in
    "")
        ;;
    --local)
        mode="local"
        bazel_args=(--//src:shim=false)
        ;;
    *)
        echo "Usage: $0 [--local]"
        exit 2
        ;;
esac

if [[ "$mode" == "remote" ]]; then
    echo "Running adders through the remote Metal shim. Start the server first."
else
    echo "Running adders directly on the local Metal GPU."
fi

bazel build "${bazel_args[@]}" //src:adder
adder_binary="$(bazel cquery "${bazel_args[@]}" --output=files //src:adder)"

log_dir="$(mktemp -d "${TMPDIR:-/tmp}/metal-virtualizer-concurrency.XXXXXX")"
test_passed=false

cleanup() {
    if "$test_passed"; then
        rm -rf "$log_dir"
    else
        echo "Logs kept in $log_dir"
    fi
}
trap cleanup EXIT INT TERM

start_time="$(perl -MTime::HiRes=time -e 'printf "%.6f", time')"

declare -a client_pids
for ((i = 1; i <= CLIENT_COUNT; ++i)); do
    "$adder_binary" >"$log_dir/adder-$i.log" 2>&1 &
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

end_time="$(perl -MTime::HiRes=time -e 'printf "%.6f", time')"
elapsed="$(perl -e 'printf "%.3f", $ARGV[1] - $ARGV[0]' "$start_time" "$end_time")"

if ((failures > 0)); then
    echo "FAIL: $failures of $CLIENT_COUNT adders failed"
    exit 1
fi

test_passed=true
echo "PASS: all $CLIENT_COUNT adders computed the correct result in $elapsed seconds"
