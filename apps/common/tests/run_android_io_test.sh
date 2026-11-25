#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD="$ROOT/build/platform-tests"
mkdir -p "$BUILD"

cat > "$BUILD/android_log_stub.h" <<'EOF'
#pragma once
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_ERROR 6
static inline int __android_log_print(int, const char*, const char*, ...) {
  return 0;
}
EOF

mkdir -p "$BUILD/android_stub/android"
cp "$BUILD/android_log_stub.h" "$BUILD/android_stub/android/log.h"

clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror \
  -I"$ROOT/core/include" \
  -I"$ROOT/apps/common/include" \
  -I"$ROOT/apps/android-client/app/src/main/cpp" \
  -I"$BUILD/android_stub" \
  "$ROOT/apps/android-client/app/src/main/cpp/accessory_io_test.cpp" \
  "$ROOT/apps/android-client/app/src/main/cpp/accessory_io.cpp" \
  -o "$BUILD/android-io-tests"

"$BUILD/android-io-tests"
