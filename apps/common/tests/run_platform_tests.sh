#!/usr/bin/env bash
# Builds and runs every Windows/Android platform contract test with the host
# toolchain. No NDK, MSVC or USB hardware is required: the tests exercise the
# shared core, the AOA link/session adapter, the Android fd semantics and the
# 12 KiB fragmentation policy through in-memory transports.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD="$ROOT/build/platform-tests"
CXX="${CXX:-clang++}"
FLAGS=(-std=c++20 -Wall -Wextra -Wpedantic -Werror -O1 -g)

mkdir -p "$BUILD"

COMMON_SOURCES=("$ROOT"/apps/common/src/*.cpp)
CORE_SOURCES=("$ROOT"/core/src/*.cpp)
INCLUDES=(-I"$ROOT/core/include" -I"$ROOT/apps/common/include")

build() {
  local output="$1"
  shift
  echo "==> $(basename "$output")"
  "$CXX" "${FLAGS[@]}" "${INCLUDES[@]}" "$@" -o "$output"
}

build "$BUILD/platform-tests" \
  "$ROOT/apps/common/tests/platform_tests.cpp" \
  "${COMMON_SOURCES[@]}" "${CORE_SOURCES[@]}"

build "$BUILD/wire-compat-tests" \
  "$ROOT/apps/common/tests/wire_compat_tests.cpp" \
  "${COMMON_SOURCES[@]}" "${CORE_SOURCES[@]}"

build "$BUILD/aoa-session-tests" \
  -I"$ROOT/apps/windows-host/include" \
  "$ROOT/apps/common/tests/aoa_session_tests.cpp" \
  "$ROOT/apps/windows-host/src/aoa_links.cpp" \
  "${COMMON_SOURCES[@]}" "${CORE_SOURCES[@]}"

build "$BUILD/windows-contract-tests" \
  -I"$ROOT/apps/windows-host/include" \
  "$ROOT/apps/windows-host/tests/contract_tests.cpp" \
  "$ROOT/apps/windows-host/src/aoa_links.cpp" \
  "$ROOT/apps/windows-host/src/host_state.cpp" \
  "$ROOT/apps/windows-host/src/input_injector.cpp" \
  "$ROOT/apps/windows-host/src/virtual_display.cpp" \
  "${COMMON_SOURCES[@]}" "${CORE_SOURCES[@]}"

# The AOA link/session library and the Windows entry point must also compile as
# one unit; only the platform graphics/input backends are Windows-only.
build "$BUILD/windows-host-smoke" \
  -I"$ROOT/apps/windows-host/include" \
  "$ROOT/apps/windows-host/src/main.cpp" \
  "$ROOT/apps/windows-host/src/aoa_links.cpp" \
  "$ROOT/apps/windows-host/src/capture_encoder.cpp" \
  "$ROOT/apps/windows-host/src/host_controller.cpp" \
  "$ROOT/apps/windows-host/src/host_state.cpp" \
  "$ROOT/apps/windows-host/src/input_injector.cpp" \
  "$ROOT/apps/windows-host/src/libusb_aoa.cpp" \
  "$ROOT/apps/windows-host/src/studio_ui.cpp" \
  "$ROOT/apps/windows-host/src/virtual_display.cpp" \
  "${COMMON_SOURCES[@]}" "${CORE_SOURCES[@]}"

echo "==> Android accessory fd contract"
bash "$ROOT/apps/common/tests/run_android_io_test.sh"

"$BUILD/platform-tests"
"$BUILD/wire-compat-tests"
"$BUILD/aoa-session-tests"
"$BUILD/windows-contract-tests"

echo "all platform test suites passed"
