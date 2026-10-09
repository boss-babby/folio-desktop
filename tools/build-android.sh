#!/usr/bin/env bash
# tools/build-android.sh - cross-compile Folio for Android.
#
# Mirrors tools/dev-setup.sh for the Android target. Needs no Android SDK
# and no Gradle: this builds the native library only, which is the part
# that can be checked without a device.
set -euo pipefail
cd "$(dirname "$0")/.."

ABI="${1:-arm64-v8a}"
case "$ABI" in
    arm64-v8a) DEPABI=arm64 ;;
    x86_64)    DEPABI=x64 ;;
    *) echo "usage: $0 [arm64-v8a|x86_64]" >&2; exit 1 ;;
esac

NDK_VER="android-ndk-r27c"
NDK="${ANDROID_NDK_HOME:-$HOME/.cache/folio-deps/android/$NDK_VER}"
if [ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
    echo "==> fetching $NDK_VER"
    mkdir -p "$HOME/.cache/folio-deps/android"
    (cd "$HOME/.cache/folio-deps/android" &&
     curl -L --retry 3 -o ndk.zip \
        "https://dl.google.com/android/repository/$NDK_VER-linux.zip" &&
     unzip -q ndk.zip && rm -f ndk.zip)
    NDK="$HOME/.cache/folio-deps/android/$NDK_VER"
fi

FOLIO_TARGET=android FOLIO_ABI="$DEPABI" bash tools/bootstrap-deps.sh

# Nested under build/ on purpose: that directory name is excluded from
# workspace snapshots, so a 49 MB cross-build does not get committed.
BUILD="build/android-$DEPABI"
cmake -S . -B "$BUILD" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI="$ABI" -DANDROID_PLATFORM=android-26 \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DFOLIO_BUILD_UI=ON -DFOLIO_BUILD_TESTS=OFF
cmake --build "$BUILD"

SO="$BUILD/folio-ui/libfolio-android.so"
NM="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm"

# An Android library that links but exports no entry point is the failure
# this check exists for: NativeActivity finds ANativeActivity_onCreate by
# dlsym, so a linker that garbage-collects it produces a .so that builds,
# installs, and cannot start. Verify rather than assume.
echo
# Note: the symbol list is captured once rather than piped into `grep -q`
# per symbol. Under `set -o pipefail`, `grep -q` exits on its first match,
# nm takes SIGPIPE, and the pipeline reports failure *because* the symbol
# was found - which inverts the whole check.
syms="$("$NM" -D --defined-only "$SO")"
for sym in ANativeActivity_onCreate android_main; do
    if printf '%s\n' "$syms" | grep -E " T $sym\$" >/dev/null; then
        echo "  ok: $sym exported"
    else
        echo "  FAIL: $SO does not export $sym" >&2
        exit 1
    fi
done
echo
echo "ready: $SO ($(du -h "$SO" | cut -f1), $ABI)"
echo "an APK additionally needs the Android SDK and Gradle: cd android && ./gradlew assembleDebug"
