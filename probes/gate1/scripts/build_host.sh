#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
PROBE_ROOT="$(cd "$SCRIPT_DIR/.." && pwd -P)"
source "$SCRIPT_DIR/common.sh"

MODE="real"
if [[ "${1:-}" == "--syntax-only" ]]; then
  MODE="syntax"
elif [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  cat <<'EOF'
Usage: build_host.sh [--syntax-only]

Default: build and test the real libusb host probe at host/build-real/.
--syntax-only: build a non-runnable libusb API shim when the real dependency is absent.

Environment overrides:
  CMAKE          CMake executable
  CTEST          CTest executable
  BUILD_DIR      build directory
  LIBUSB_PREFIX  libusb installation prefix
EOF
  exit 0
elif [[ -n "${1:-}" ]]; then
  echo "unknown argument: $1" >&2
  exit 2
fi

find_cmake() {
  if [[ -n "${CMAKE:-}" && -x "$CMAKE" ]]; then
    printf '%s\n' "$CMAKE"
    return 0
  fi
  if command -v cmake >/dev/null 2>&1; then
    command -v cmake
    return 0
  fi
  local sdk_root
  sdk_root="$(gate1_native_sdk_root)" || return 1
  local candidate="$sdk_root/openharmony/native/build-tools/cmake/bin/cmake"
  if [[ -x "$candidate" ]]; then
    printf '%s\n' "$candidate"
    return 0
  fi
  return 1
}

CMAKE_BIN="$(find_cmake)" || {
  echo "CMake 3.16+ not found." >&2
  echo "Install CMake or set DEVECO_SDK_HOME/CMAKE." >&2
  exit 2
}

if [[ "$MODE" == "syntax" ]]; then
  BUILD_DIR="${BUILD_DIR:-$PROBE_ROOT/host/build-syntax}"
  CONFIGURE_ARGS=(
    -DCMAKE_BUILD_TYPE=Debug
    -DGATE1_LIBUSB_SYNTAX_SHIM=ON
    -DGATE1_FORCE_LIBUSB_SYNTAX_SHIM=ON
  )
  EXPECTED_TARGET="gate1_host_syntax"
else
  BUILD_DIR="${BUILD_DIR:-$PROBE_ROOT/host/build-real}"
  CONFIGURE_ARGS=(-DCMAKE_BUILD_TYPE=Debug)
  if [[ -n "${LIBUSB_PREFIX:-}" ]]; then
    CONFIGURE_ARGS+=(
      "-DLIBUSB_INCLUDE_DIR=$LIBUSB_PREFIX/include"
      "-DLIBUSB_LIBRARY=$LIBUSB_PREFIX/lib/libusb-1.0.dylib"
    )
  fi
  EXPECTED_TARGET="gate1_host"
fi

echo "CMake:       $CMAKE_BIN"
echo "Build dir:   $BUILD_DIR"
echo "Mode:        $MODE"

"$CMAKE_BIN" -S "$PROBE_ROOT/host" -B "$BUILD_DIR" "${CONFIGURE_ARGS[@]}"
"$CMAKE_BIN" --build "$BUILD_DIR" --parallel

if [[ -n "${CTEST:-}" && -x "$CTEST" ]]; then
  CTEST_BIN="$CTEST"
elif [[ -x "$(dirname "$CMAKE_BIN")/ctest" ]]; then
  CTEST_BIN="$(dirname "$CMAKE_BIN")/ctest"
elif command -v ctest >/dev/null 2>&1; then
  CTEST_BIN="$(command -v ctest)"
else
  echo "CTest not found; run $BUILD_DIR/gate1_core_tests directly." >&2
  CTEST_BIN=""
fi

if [[ -n "$CTEST_BIN" ]]; then
  "$CTEST_BIN" --test-dir "$BUILD_DIR" --output-on-failure
else
  "$BUILD_DIR/gate1_core_tests"
fi

if [[ ! -x "$BUILD_DIR/$EXPECTED_TARGET" ]]; then
  if [[ "$MODE" == "real" ]]; then
    cat >&2 <<'EOF'
Real libusb host probe was NOT built. Core tests may have passed, but that is
not runtime evidence.

Install libusb 1.0, then rerun:
  brew install libusb
  probes/gate1/scripts/build_host.sh

Or provide a prefix:
  LIBUSB_PREFIX=/path/to/libusb probes/gate1/scripts/build_host.sh

For compile-only checking without runtime:
  probes/gate1/scripts/build_host.sh --syntax-only
EOF
    exit 3
  fi
  echo "expected syntax target not found: $BUILD_DIR/$EXPECTED_TARGET" >&2
  exit 3
fi

if [[ "$MODE" == "syntax" ]]; then
  echo "SYNTAX ONLY: $BUILD_DIR/$EXPECTED_TARGET is deliberately non-runnable."
else
  echo "RUNTIME PROBE: $BUILD_DIR/$EXPECTED_TARGET"
fi
