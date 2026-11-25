#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ANDROID_HOME="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
NDK_VERSION="${NDK_VERSION:-27.2.12479018}"
CMAKE_VERSION="${CMAKE_VERSION:-3.22.1}"

if [[ ! -d "$ANDROID_HOME/ndk/$NDK_VERSION" ]]; then
  echo "Missing NDK $NDK_VERSION under $ANDROID_HOME/ndk" >&2
  echo "Install with: sdkmanager --install 'ndk;$NDK_VERSION' 'cmake;$CMAKE_VERSION'" >&2
  exit 1
fi

if [[ ! -x "$ROOT/gradlew" ]]; then
  echo "Missing Gradle wrapper. Open apps/android-client in Android Studio once," >&2
  echo "or run: gradle wrapper --gradle-version 8.9" >&2
  exit 1
fi

cd "$ROOT"
./gradlew :app:assembleDebug
echo "APK: $ROOT/app/build/outputs/apk/debug/app-debug.apk"
