#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
mkdir -p "$root/output/host-tests"
for name in workspace theme message gallery config; do
    mkdir -p "$root/output/web-tests/$name"
done
sh "$root/test/editor/probe.sh" > "$root/output/host-tests/image-profiles.json"
