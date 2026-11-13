#!/bin/sh
set -u

if [ "$(id -u)" -ne 0 ]; then
    echo "precisa de root (sudo)" >&2
    exit 2
fi

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT=${1:-"$ROOT/results/kimi-kind-$(date +%Y%m%d-%H%M%S)"}
mkdir -p "$OUT"
SUMMARY="$OUT/summary.txt"
: > "$SUMMARY"
overall=0

run_one() {
    name=$1
    shift
    echo "===== $name ====="
    "$@" > "$OUT/$name.log" 2>&1
    rc=$?
    echo "$name rc=$rc" | tee -a "$SUMMARY"
    if [ "$rc" -ne 0 ]; then overall=1; fi
    if [ -f "$OUT/$name.log" ]; then
        grep -v 'nvrm387:' "$OUT/$name.log" | tail -n 22
    fi
    echo
}

run_one test_hw env NVRM387_LOG=1 "$ROOT/test_hw"
run_one device env NVRM387_LOG=1 "$ROOT/nvk/vk_enum" "$ROOT/nvk/libvulkan_nouveau.dylib" device
run_one compute env NVRM387_LOG=1 "$ROOT/nvk/vk_compute" "$ROOT/nvk/libvulkan_nouveau.dylib"
run_one triangle_optimal env NVRM387_LOG=1 "$ROOT/nvk/vk_triangle" "$ROOT/nvk/libvulkan_nouveau.dylib"
run_one triangle_linear env NVRM387_LOG=1 "$ROOT/nvk/vk_triangle" "$ROOT/nvk/libvulkan_nouveau.dylib" linear

echo "LOGS: $OUT"
cat "$SUMMARY"
exit "$overall"
