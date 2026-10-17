#!/usr/bin/env bash
# Rebuild the Android debug APK without leaving Gradle/CMake/SDK artifacts
# anywhere in the shared workspace. All work, including downloaded toolchains,
# lives under /var/tmp (disk-backed; /tmp is often too small a tmpfs).
# Usage: bash tools/build-apk-clean.sh [absolute-output-apk-path]
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
temp_base=${FOLIO_TEMP_ROOT:-/var/tmp}
mkdir -p "$temp_base"
temp_base=$(cd "$temp_base" && pwd -P)
repo_parent=$(dirname "$repo")
case "$temp_base/" in "$repo_parent/"*) echo 'FOLIO_TEMP_ROOT must be outside the shared workspace' >&2; exit 1;; esac
scratch=$(mktemp -d "$temp_base/folio-android-build.XXXXXXXX")
cleanup() { rm -rf -- "$scratch"; }
trap cleanup EXIT
trap 'exit 130' INT TERM
mkdir -p "$scratch/source" "$scratch/deps" "$scratch/tmp"

# Include uncommitted/new source (git archive HEAD would omit it), not
# generated outputs. No source path in the checkout is ever modified.
tar -C "$repo" --exclude='./.git' --exclude='./build' \
    --exclude='./android/.gradle' --exclude='./android/app/.cxx' \
    --exclude='./android/app/build' --exclude='./android/local.properties' \
    --exclude='./android/.kotlin' --exclude='*.apk' --exclude='*.zip' \
    -cf - . | tar -C "$scratch/source" -xf -

# The inner build script uses FOLIO_DEPS_CACHE to place SDK/JDK/Gradle and
# bootstrap-deps.sh uses FOLIO_DEPS_DIR to place PDFium/Skia. Both MUST agree.
# Keep SDK/JDK/Gradle caches and java.io.tmpdir out of the checkout. Do NOT
# change java user.home: Android's existing ~/.android/debug.keystore must
# remain stable or the next APK cannot upgrade the previously installed app.
# The tiny signing key is the one intentional persistent build credential.
export FOLIO_DEPS_CACHE="$scratch/deps" FOLIO_DEPS_DIR="$scratch/deps"
export GRADLE_USER_HOME="$scratch/gradle-home" TMPDIR="$scratch/tmp"
# AGP lint requires more than 192 MiB Metaspace on a cold Gradle home.
export FOLIO_GRADLE_JVMARGS="${FOLIO_GRADLE_JVMARGS:--Xmx384m -XX:MaxMetaspaceSize=384m}"
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-1}"
export JAVA_TOOL_OPTIONS="${JAVA_TOOL_OPTIONS:-} -Djava.io.tmpdir=$scratch/tmp"
( cd "$scratch/source" && bash tools/build-apk.sh )

apk="$scratch/source/android/app/build/outputs/apk/debug/app-debug.apk"
[[ -s "$apk" ]] || { echo 'Android build finished without a debug APK' >&2; exit 1; }
unzip -tq "$apk" >/dev/null
output=${1:-"$(dirname "$repo")/folio-android-corrected-debug.apk"}
case "$output" in /*) ;; *) echo 'Output APK path must be absolute' >&2; exit 1;; esac
mkdir -p "$(dirname "$output")"
incoming="$(dirname "$output")/.folio-incoming-$$.apk"
trap 'rm -f -- "$incoming"; cleanup' EXIT
cp -- "$apk" "$incoming"
cmp -- "$apk" "$incoming"
mv -f -- "$incoming" "$output"
printf 'APK: %s\n' "$output"
sha256sum "$output"
# The EXIT trap removes the entire temporary copy, dependencies and build.
