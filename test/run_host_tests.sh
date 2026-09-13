#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/msp-host.XXXXXX")
trap 'rm -rf -- "$build"' EXIT HUP INT TERM
sh "$root/test/lunar/host/run.sh"
sh "$root/test/startup/host/run.sh"
python3 "$root/test/http_receive/run_tests.py" --sanitize
cmake -S "$root/test/epd" -B "$build/epd" -G Ninja
cmake --build "$build/epd" --parallel 1
ctest --test-dir "$build/epd" --output-on-failure
cmake -S "$root/test/fb_render" -B "$build/fb-render" -G Ninja
cmake --build "$build/fb-render" --parallel 1
ctest --test-dir "$build/fb-render" --output-on-failure
cmake -S "$root/test/image_quant" -B "$build/image-quant" -G Ninja
cmake --build "$build/image-quant" --parallel 1
ctest --test-dir "$build/image-quant" --output-on-failure
