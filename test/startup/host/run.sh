#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
idf=${IDF_PATH:-/mnt/d/espidf/Espressif/frameworks/esp-idf-v5.5.4}
unity=${UNITY_ROOT:-$idf/components/unity/unity/src}
cjson=${CJSON_ROOT:-$idf/components/json/cJSON}
for source in "$unity/unity.c" "$cjson/cJSON.c"; do
    if [ ! -f "$source" ]; then
        printf 'Missing %s; set IDF_PATH or UNITY_ROOT and CJSON_ROOT.\n' "$source" >&2
        exit 1
    fi
done
build=$(mktemp -d "${TMPDIR:-/tmp}/msp-startup.XXXXXX")
trap 'rm -rf -- "$build"' EXIT HUP INT TERM
cc=${CC:-cc}
compile() {
    "$cc" -std=c11 -D_POSIX_C_SOURCE=200809L -DMSP_HOST_TEST=1 \
        -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections \
        -I"$root/test/startup/host/stubs" -I"$root/test/startup/host" \
        -I"$root/main" -I"$unity" -I"$cjson" -include stddef.h -pthread "$@"
}
compile -c "$root/main/display_policy.c" -o "$build/policy.o"
compile -c "$root/main/display_mode.c" -o "$build/mode.o"
compile -c "$root/main/message_board.c" -o "$build/message.o"
compile -Dfopen=host_canvas_fopen -Dmalloc=host_canvas_malloc \
    -c "$root/main/canvas_board.c" -o "$build/canvas.o"
compile -c "$root/test/startup/main/test_startup_state.c" -o "$build/tests.o"
compile -c "$root/test/startup/host/host_support.c" -o "$build/support.o"
compile -c "$root/test/startup/host/test_boards.c" -o "$build/boards.o"
compile -c "$unity/unity.c" -o "$build/unity.o"
compile -c "$cjson/cJSON.c" -o "$build/cjson.o"
compile "$build"/*.o -Wl,--gc-sections -Wl,--wrap=mkdir -Wl,--wrap=stat \
    -Wl,--wrap=access -Wl,--wrap=rename -lm -o "$build/startup-tests"
"$build/startup-tests"
