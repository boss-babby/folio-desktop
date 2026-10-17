#!/usr/bin/env bash
# tools/build-apk.sh - produce an installable Folio APK.
#
# Verified working: this is the sequence that produced the first Folio APK
# in round 23, containing libfolio-android.so for arm64-v8a and x86_64 plus
# the font assets.
#
# What it needs, and why the fetch is here rather than in
# tools/bootstrap-deps.sh: the Android SDK and a JDK 17 are gigabytes, and
# the native library build (tools/build-android.sh) does not need any of
# them. Fetching them unconditionally would triple the setup time for
# everyone who is only compiling C++. So they are fetched on demand, into
# the same snapshot-excluded cache, and reused.
set -euo pipefail
cd "$(dirname "$0")/.."

CACHE="${FOLIO_DEPS_CACHE:-$HOME/.cache/folio-deps}"
D="$CACHE/android"
SDK="$D/sdk"
GRADLE_VER="8.9"

say() { printf '==> %s\n' "$*"; }

# --- JDK 17 --------------------------------------------------------------
# Android Gradle Plugin 8.x requires 17. A system JDK 11 is common enough
# (and is what this sandbox has) that we do not trust whatever `java`
# resolves to.
if [ -x "$D/jdk17/bin/java" ]; then
    export JAVA_HOME="$D/jdk17"
elif [ -n "${JAVA_HOME:-}" ] && "$JAVA_HOME/bin/java" -version 2>&1 |
        grep -qE '"(1[7-9]|2[0-9])'; then
    say "using existing JAVA_HOME"
else
    say "fetching JDK 17"
    mkdir -p "$D"
    curl -sSL -o "$D/jdk.tgz" \
        "https://api.adoptium.net/v3/binary/latest/17/ga/linux/x64/jdk/hotspot/normal/eclipse"
    mkdir -p "$D/jdk17"
    tar -xzf "$D/jdk.tgz" -C "$D/jdk17" --strip-components=1
    rm -f "$D/jdk.tgz"
    export JAVA_HOME="$D/jdk17"
fi

# --- Android SDK ---------------------------------------------------------
# build-tools, the platform and the NDK. `ndk;27.2.12479018` matches
# ndkVersion in android/app/build.gradle.kts, and installing it through
# sdkmanager rather than pointing local.properties at a hand-unzipped NDK
# matters: AGP rejects an NDK that is not under the SDK root, and the
# error ("did not contain a valid NDK") does not mention where it looked.
if [ ! -d "$SDK/build-tools" ]; then
    say "fetching Android SDK (build-tools 34, platform 34, NDK r27c)"
    mkdir -p "$SDK/cmdline-tools"
    if [ ! -x "$SDK/cmdline-tools/latest/bin/sdkmanager" ]; then
        curl -sSL -o "$D/clt.zip" \
            "https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip"
        unzip -q -o "$D/clt.zip" -d "$D/clt-tmp"
        rm -rf "$SDK/cmdline-tools/latest"
        mv "$D/clt-tmp/cmdline-tools" "$SDK/cmdline-tools/latest"
        rm -rf "$D/clt-tmp" "$D/clt.zip"
    fi
    export ANDROID_HOME="$SDK"
    yes 2>/dev/null | "$SDK/cmdline-tools/latest/bin/sdkmanager" \
        --sdk_root="$SDK" --licenses >/dev/null 2>&1 || true
    "$SDK/cmdline-tools/latest/bin/sdkmanager" --sdk_root="$SDK" \
        "platform-tools" "platforms;android-34" "build-tools;34.0.0" \
        "ndk;27.2.12479018" >/dev/null
fi
export ANDROID_HOME="$SDK"
export ANDROID_SDK_ROOT="$SDK"

# Install the exact Android CMake version required by CMakeLists.txt *before* starting Gradle.
# Otherwise AGP starts sdkmanager inside its already memory-limited daemon;
# the two Java heaps can OOM-kill that daemon on a 2 GiB build machine.
if [ ! -x "$SDK/cmake/3.31.6/bin/cmake" ]; then
    say "fetching Android CMake 3.31.6"
    rm -rf "$SDK/cmake/3.31.6"   # remove an interrupted package install
    JAVA_OPTS="-Xmx384m -XX:MaxMetaspaceSize=192m" \
        "$SDK/cmdline-tools/latest/bin/sdkmanager" --sdk_root="$SDK" \
        "cmake;3.31.6" >/dev/null
    test -x "$SDK/cmake/3.31.6/bin/cmake" || {
        echo "Android CMake 3.31.6 was not installed" >&2; exit 1;
    }
fi

# --- per-ABI prebuilts ---------------------------------------------------
# Gradle builds every ABI in abiFilters, and CMake skips Skia-dependent
# targets when the prebuilts are missing, which surfaces as
# "Unexpected native build target folio-android" - opaque. So fetch all of
# them here. The native library is what takes a while; these are cached.
for pair in "arm64:arm64-v8a" "x64:x86_64"; do
    dep="${pair%%:*}"
    if [ ! -d "$CACHE/skia-android-$dep" ]; then
        say "fetching skia/pdfium prebuilts for $dep"
        FOLIO_TARGET=android FOLIO_ABI="$dep" bash tools/bootstrap-deps.sh >/dev/null
    fi
done

# --- Gradle --------------------------------------------------------------
if [ ! -d "$D/gradle-$GRADLE_VER" ]; then
    say "fetching Gradle $GRADLE_VER"
    curl -sSL -o "$D/gradle.zip" \
        "https://services.gradle.org/distributions/gradle-$GRADLE_VER-bin.zip"
    unzip -q "$D/gradle.zip" -d "$D"
    rm -f "$D/gradle.zip"
fi
export PATH="$D/gradle-$GRADLE_VER/bin:$JAVA_HOME/bin:$PATH"

if [ "${FOLIO_PREPARE_ONLY:-0}" = 1 ]; then
    say "Android toolchain and prebuilts ready (build skipped)"
    exit 0
fi

# local.properties is generated, never committed: sdk.dir is an absolute
# path to a machine-specific cache.
printf 'sdk.dir=%s\n' "$SDK" > android/local.properties

say "building"
# Native compilation and the Gradle daemon run in the same memory budget.
# Unbounded Gradle workers OOM-killed the daemon on a 2 GiB builder. Keep
# these overridable for larger CI machines, but make the default reproducible
# on modest phones/VMs as well.
GRADLE_WORKERS="${FOLIO_GRADLE_WORKERS:-1}"
GRADLE_JVMARGS="${FOLIO_GRADLE_JVMARGS:--Xmx384m -XX:MaxMetaspaceSize=256m}"
( cd android && gradle --no-daemon --max-workers="$GRADLE_WORKERS" \
    "-Dorg.gradle.jvmargs=$GRADLE_JVMARGS" ":app:assemble${1:-Debug}" \
    ":app:lint${1:-Debug}" )

APK=$(find android/app/build/outputs/apk -name "*.apk" | head -1)
[ -n "$APK" ] || { echo "no APK produced" >&2; exit 1; }

echo
echo "ready: $APK ($(du -h "$APK" | cut -f1))"
# The CMake scratch tree is ~400 MB and is named .cxx, which - unlike
# "build" - is not on the workspace snapshot exclusion list, so leaving it
# behind is what blows a size-capped workspace. It is pure build output and
# regenerating it costs about a minute. Set KEEP_CXX=1 to keep incremental
# native rebuilds.
if [ -d android/app/.cxx ] && [ -z "${KEEP_CXX:-}" ]; then
    _sz=$(du -sh android/app/.cxx | cut -f1)
    rm -rf android/app/.cxx android/.gradle
    echo "(removed android/app/.cxx, $_sz, and android/.gradle - set KEEP_CXX=1 to keep them)"
fi

echo
echo "install:   $ANDROID_HOME/platform-tools/adb install -r $APK"
echo "run:       $ANDROID_HOME/platform-tools/adb shell am start \\"
echo "               -n ai.folio.reader/ai.folio.reader.FolioActivity"
echo "logs:      $ANDROID_HOME/platform-tools/adb logcat -s folio"
echo
echo "This sandbox has no /dev/kvm, so no emulator could be started here:"
echo "the APK is verified by inspection (contents, manifest, entry points),"
echo "never by running."
