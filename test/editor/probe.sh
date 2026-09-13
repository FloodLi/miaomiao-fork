#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/msp-profile.XXXXXX")
trap 'rm -rf -- "$build"' EXIT HUP INT TERM
gcc -std=gnu11 -I"$root/test/epd/include" -I"$root/main" -Wno-unused-parameter \
    -Wno-missing-field-initializers "$root/test/editor/panels_probe.c" -o "$build/probe"
"$build/probe"
