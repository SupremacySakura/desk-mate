#!/usr/bin/env bash
# Builds an installable debug APK without Gradle.
#
# Gradle would normally drive AGP, which needs network access to Maven for AGP
# and the AndroidX plugins. This script instead uses the SDK build-tools
# (aapt2/d8/zipalign/apksigner) and the NDK's clang directly, so a machine with
# only an installed SDK + NDK can produce a signed APK offline.
#
# Usage:
#   ANDROID_HOME=~/Library/Android/sdk NDK=~/Library/Android/sdk/ndk/27.2.12479018 \
#     bash apps/android-client/package_apk.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$ROOT/../.." && pwd)"

ANDROID_HOME="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
BUILD_TOOLS="${BUILD_TOOLS:-$(ls -d "$ANDROID_HOME"/build-tools/* 2>/dev/null | sort -V | tail -1)}"
PLATFORM_JAR="${PLATFORM_JAR:-$(ls -d "$ANDROID_HOME"/platforms/*/android.jar 2>/dev/null | sort -V | tail -1)}"
NDK="${NDK:-$(ls -d "$ANDROID_HOME"/ndk/* 2>/dev/null | sort -V | tail -1)}"
JAVA_HOME="${JAVA_HOME:-/Applications/Android Studio.app/Contents/jbr/Contents/Home}"
export JAVA_HOME
# d8/apksigner are JVM launchers that need java on PATH.
export PATH="$JAVA_HOME/bin:$PATH"

# core/lifecycle.cpp uses __int128, which 32-bit ARM lacks; 64-bit ABIs only.
ABIS="${ABIS:-arm64-v8a x86_64}"
API="${API:-26}"
OUT="$ROOT/app/build/outputs/apk/debug"
WORK="$ROOT/app/build/manual"

for required in "$BUILD_TOOLS/aapt2" "$BUILD_TOOLS/d8" "$BUILD_TOOLS/zipalign" \
                "$BUILD_TOOLS/apksigner" "$PLATFORM_JAR" "$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/clang++"; do
  if [[ ! -e "$required" ]]; then
    echo "missing build dependency: $required" >&2
    echo "set ANDROID_HOME / NDK / PLATFORM_JAR or install the SDK NDK" >&2
    exit 1
  fi
done

TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/darwin-x86_64"
CXX="$TOOLCHAIN/bin/clang++"

abi_target() {
  case "$1" in
    arm64-v8a)   echo aarch64-linux-android ;;
    armeabi-v7a) echo armv7a-linux-androideabi ;;
    x86_64)      echo x86_64-linux-android ;;
    *) echo "unsupported ABI: $1" >&2; return 1 ;;
  esac
}

rm -rf "$WORK"
mkdir -p "$WORK/res" "$WORK/classes" "$WORK/dex" "$WORK/apk" "$OUT"

echo "==> aapt2 compile"
"$BUILD_TOOLS/aapt2" compile --dir "$ROOT/app/src/main/res" -o "$WORK/res.zip"

echo "==> aapt2 link"
# AGP supplies the package attribute; aapt2 link needs it on the manifest.
sed 's|<manifest xmlns:android="http://schemas.android.com/apk/res/android">|&|' \
  "$ROOT/app/src/main/AndroidManifest.xml" \
  | sed 's|<manifest |<manifest package="dev.deskmate.client" |' \
  > "$WORK/AndroidManifest.xml"
"$BUILD_TOOLS/aapt2" link \
  -o "$WORK/base.apk" \
  --manifest "$WORK/AndroidManifest.xml" \
  -I "$PLATFORM_JAR" \
  --java "$WORK/gen" \
  --min-sdk-version "$API" \
  --target-sdk-version 35 \
  --version-code "${VERSION_CODE:-6}" --version-name "${VERSION_NAME:-1.5.0}" \
  "$WORK/res.zip"

echo "==> javac"
"$JAVA_HOME/bin/javac" -source 17 -target 17 -Xlint:-options \
  -classpath "$PLATFORM_JAR" \
  -d "$WORK/classes" \
  $(find "$WORK/gen" "$ROOT/app/src/main/java" -name '*.java')

echo "==> d8"
find "$WORK/classes" -name '*.class' > "$WORK/classes.txt"
"$BUILD_TOOLS/d8" --min-api "$API" --output "$WORK/dex" @"$WORK/classes.txt"

CORE="$REPO/core/src"
COMMON="$REPO/apps/common/src"
CPP="$ROOT/app/src/main/cpp"
SOURCES=()
for source in "$CORE"/*.cpp "$COMMON"/*.cpp "$CPP"/*.cpp; do
  case "$source" in
    *_test.cpp) continue ;;
  esac
  SOURCES+=("$source")
done
for ABI in $ABIS; do
  TARGET="$(abi_target "$ABI")"
  echo "==> native ($ABI)"
  mkdir -p "$WORK/lib/$ABI"
  "$CXX" --target="$TARGET$API" -std=c++20 -O3 -fPIC -shared -fexceptions \
    -DANDROID \
    -I"$REPO/core/include" -I"$REPO/apps/common/include" -I"$CPP" \
    "${SOURCES[@]}" \
    -landroid -llog -lmediandk -lEGL -lGLESv2 \
    -Wl,-z,max-page-size=16384 \
    -static-libstdc++ \
    -o "$WORK/lib/$ABI/libdeskmate_client.so"
  "$TOOLCHAIN/bin/llvm-strip" --strip-unneeded "$WORK/lib/$ABI/libdeskmate_client.so"
done

echo "==> package"
cp "$WORK/base.apk" "$WORK/unsigned.apk"
cd "$WORK"
zip -q -j unsigned.apk dex/classes.dex
zip -q -r unsigned.apk lib
cd - >/dev/null

"$BUILD_TOOLS/zipalign" -P 16 -f 4 "$WORK/unsigned.apk" "$WORK/aligned.apk"

# Keep the key outside WORK: WORK is wiped every run, and a fresh key would
# make the new APK fail to install over a previously installed build.
KEYSTORE="$ROOT/app/build/debug.keystore"
if [[ ! -f "$KEYSTORE" ]]; then
  "$JAVA_HOME/bin/keytool" -genkeypair -keystore "$KEYSTORE" \
    -storepass android -keypass android -alias androiddebugkey \
    -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
fi

"$BUILD_TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android \
  --key-pass pass:android --ks-key-alias androiddebugkey \
  --out "$OUT/app-debug.apk" "$WORK/aligned.apk"

DIST_ANDROID="$REPO/dist/android"
DIST_APK="$DIST_ANDROID/DeskMate-v${VERSION_NAME:-1.5.0}-android-debug.apk"
mkdir -p "$DIST_ANDROID"
cp -f "$OUT/app-debug.apk" "$DIST_APK"

echo
echo "APK: $OUT/app-debug.apk"
echo "Dist APK: $DIST_APK"
"$BUILD_TOOLS/aapt2" dump badging "$OUT/app-debug.apk" | head -3
