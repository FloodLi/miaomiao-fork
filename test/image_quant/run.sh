#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/msp-image-quant.XXXXXX")
trap 'rm -rf -- "$build"' EXIT HUP INT TERM
cmake -S "$root" -B "$build" -G Ninja -DIMAGE_QUANT_SANITIZE=ON
cmake --build "$build" --parallel 2
ctest --test-dir "$build" --output-on-failure
