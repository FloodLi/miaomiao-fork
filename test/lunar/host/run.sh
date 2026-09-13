#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
idf=${IDF_PATH:-/mnt/d/espidf/Espressif/frameworks/esp-idf-v5.5.4}
unity=${UNITY_ROOT:-$idf/components/unity/unity/src}
build=$(mktemp -d "${TMPDIR:-/tmp}/msp-lunar.XXXXXX")
trap 'rm -rf -- "$build"' EXIT HUP INT TERM
${CC:-cc} -std=c11 -Wall -Wextra -Werror -I"$root/test/lunar/host" \
    -I"$root/main" -I"$unity" "$root/main/lunar.c" \
    "$root/test/lunar/main/test_lunar.c" "$root/test/lunar/host/entry.c" \
    "$unity/unity.c" -o "$build/lunar-tests"
"$build/lunar-tests"
