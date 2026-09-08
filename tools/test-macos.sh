#!/bin/sh
# Run on Apple Silicon using an already installed CMake/Ninja/Apple toolchain.
# Optional graphics mode requires an already approved/installed Vulkan SDK.
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
if [ "$(uname -s)" != Darwin ] || [ "$(uname -m)" != arm64 ]; then
    echo 'This acceptance gate requires an Apple Silicon Mac.' >&2
    exit 2
fi
graphics=OFF
if [ "${1:-}" = '--graphics' ]; then graphics=ON; fi
build="out/build/macos-arm64-$graphics"
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
    -DPCSX5_BUILD_VULKAN="$graphics"
cmake --build "$build" --parallel 2
# Local ad-hoc development signature only; not notarization or distribution.
codesign --force --sign - --options runtime --entitlements tools/macos-jit.entitlements \
    "$build/tests/pcsx5_arm64_execution"
ctest --test-dir "$build" --output-on-failure --no-tests=error \
    --output-junit junit.xml --parallel 2
