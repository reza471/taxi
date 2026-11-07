#!/bin/bash
# Build and run the native_sim host tests. Needs a Zephyr checkout (west workspace).
# Usage: ZEPHYR_BASE=<zephyr> tests/native/run.sh
set -e
here="$(cd "$(dirname "$0")" && pwd)"
build=${BUILD_DIR:-/tmp/taxi_native_build}
export ZEPHYR_TOOLCHAIN_VARIANT=host
west build -p auto -b native_sim/native/64 "$here" -d "$build"
cd "$build"
rm -f flash.bin
# run the same binary several times on the same (simulated) flash = several power cycles
for i in 1 2 3 4 5; do
	echo "=== run $i ==="
	./zephyr/zephyr.exe -flash=flash.bin | tail -n 22
done
