#!/usr/bin/env bash
# Fetch the prebuilt third-party libraries Folio links against.
#
# They land in ~/.cache/folio-deps, which is deliberately outside the repo:
# together they are several hundred megabytes unpacked and nothing here is
# source we edit. Re-run this after a clean machine; it is idempotent.
#
#   skia    - skia-pack m144, Linux Release x64 (Ganesh; no Graphite symbols)
#   pdfium  - pdfium-binaries, V8-free / XFA-free, ships PDFiumConfig.cmake
set -euo pipefail

DEPS="${FOLIO_DEPS_DIR:-$HOME/.cache/folio-deps}"
mkdir -p "$DEPS"

# Cross-target selection. Defaults reproduce the historic behaviour exactly
# (host Linux x64); FOLIO_TARGET=android switches both archives to the NDK
# builds. Verified to exist on the tags we already pin:
#   skia-pack m144-e2e6623374-4 -> android-Release-{arm64,x64}  (no armeabi-v7a)
#   pdfium-binaries             -> pdfium-android-{arm64,x64,arm,x86}.tgz
FOLIO_TARGET="${FOLIO_TARGET:-linux}"
FOLIO_ABI="${FOLIO_ABI:-x64}"
case "$FOLIO_TARGET" in
    linux)
        SKIA_MATCH="linux-Release-x64"
        PDFIUM_ASSET="pdfium-linux-x64.tgz"
        ;;
    android)
        # skia-pack names the 64-bit ARM slice "arm64"; the NDK and Gradle
        # call the same thing "arm64-v8a". Accept either spelling.
        case "$FOLIO_ABI" in
            arm64|arm64-v8a) _sk_abi=arm64; _pd_abi=arm64 ;;
            x64|x86_64)      _sk_abi=x64;   _pd_abi=x64   ;;
            *) echo "unsupported FOLIO_ABI=$FOLIO_ABI (arm64 or x64)" >&2; exit 1 ;;
        esac
        SKIA_MATCH="android-Release-${_sk_abi}"
        PDFIUM_ASSET="pdfium-android-${_pd_abi}.tgz"
        ;;
    *)
        echo "unsupported FOLIO_TARGET=$FOLIO_TARGET (linux or android)" >&2
        exit 1
        ;;
esac

have() { [ -e "$1" ]; }

# The unauthenticated GitHub API rate-limits hard and answers 403 rather
# than 429, so a single failed curl here looks like a missing asset.
api() {
    for i in 1 2 3 4 5; do
        if out=$(curl -sSL "$1") && [ -n "$out" ] &&
           ! printf '%s' "$out" | grep -q '"message": *"API rate limit'; then
            printf '%s' "$out"; return 0
        fi
        sleep $((i * 8))
    done
    echo "giving up on $1" >&2; return 1
}

# --- Skia --------------------------------------------------------------
if have "$DEPS/skia/include/core/SkCanvas.h"; then
    echo "skia: present"
else
    echo "skia: fetching m144 $SKIA_MATCH ..."
    url=$(api https://api.github.com/repos/JetBrains/skia-pack/releases \
        | grep -o "https://[^\"]*Skia-m144[^\"]*${SKIA_MATCH}[^\"]*\.zip" \
        | head -1)
    [ -n "$url" ] || { echo "skia: no matching asset"; exit 1; }
    curl -sSfL -o "$DEPS/skia.zip" "$url"
    rm -rf "$DEPS/skia"
    # The archive has no top-level directory: it unpacks include/, out/,
    # modules/, src/ and third_party/ straight into the cwd.
    tmp="$DEPS/.skia-unpack"; rm -rf "$tmp"; mkdir -p "$tmp"
    (cd "$tmp" && unzip -q "$DEPS/skia.zip")
    rm "$DEPS/skia.zip"
    mkdir -p "$DEPS/skia"
    mv "$tmp"/* "$DEPS/skia/"
    rmdir "$tmp"
    echo "skia: ok"
fi

# --- PDFium ------------------------------------------------------------
if have "$DEPS/pdfium/include/fpdfview.h"; then
    echo "pdfium: present"
else
    echo "pdfium: fetching latest $PDFIUM_ASSET ..."
    url=$(api https://api.github.com/repos/bblanchon/pdfium-binaries/releases/latest \
        | grep -o "https://[^\"]*${PDFIUM_ASSET}" | head -1)
    [ -n "$url" ] || { echo "pdfium: no matching asset"; exit 1; }
    curl -sSfL -o "$DEPS/pdfium.tgz" "$url"
    rm -rf "$DEPS/pdfium"; mkdir -p "$DEPS/pdfium"
    tar -xzf "$DEPS/pdfium.tgz" -C "$DEPS/pdfium"
    rm "$DEPS/pdfium.tgz"
    echo "pdfium: ok"
fi

# ---------------------------------------------------------------------------
# PoDoFo - the document/editing engine.
#
# Debian trixie ships 0.9.8, not the 1.x the plan was written against. There
# is no upstream binary release, so the prebuilt Debian package is the
# "already compiled" option; it is extracted into the dep cache rather than
# installed, which needs no root.
#
# Two fixups, both deliberate:
#   * podofo_config.h advertises PODOFO_HAVE_BOOST, which makes PdfDefines.h
#     include <boost/config.hpp>. No installed header uses a single BOOST_
#     macro or boost:: symbol, so the flag is turned off rather than dragging
#     in ~100MB of Boost headers for a vestigial include.
#   * libidn ships no .so development symlink in the runtime package; the
#     link line needs one.
# ---------------------------------------------------------------------------
if [ -f "$DEPS/podofo/usr/lib/libpodofo.so" ]; then
    echo "podofo: cached"
else
    echo "podofo: fetching Debian prebuilt 0.9.8 ..."
    tmp=$(mktemp -d)
    ( cd "$tmp" && for pkg in libpodofo-dev libpodofo0.9.8t64 libidn12; do
          apt-get download "$pkg" >/dev/null 2>&1 || {
              echo "podofo: apt-get download $pkg failed"; exit 1; }
      done ) || exit 1
    rm -rf "$DEPS/podofo"; mkdir -p "$DEPS/podofo"
    for d in "$tmp"/*.deb; do dpkg-deb -x "$d" "$DEPS/podofo"; done
    rm -rf "$tmp"

    cfg="$DEPS/podofo/usr/include/podofo/base/podofo_config.h"
    sed -i 's|^#define PODOFO_HAVE_BOOST|/* PODOFO_HAVE_BOOST disabled by Folio: vestigial in 0.9.8 */|' "$cfg"
    ln -sf libidn.so.12 "$DEPS/podofo/usr/lib/x86_64-linux-gnu/libidn.so"
    echo "podofo: ok"
fi

echo
echo "deps in $DEPS:"
du -sh "$DEPS"/* 2>/dev/null || true

# ---------------------------------------------------------------------------
# X11 + EGL development headers (Linux desktop shell)
#
# The runtime libraries are present on any desktop, but a container usually
# has no -dev packages. These are fetched the same way as everything else:
# apt-get download + dpkg-deb -x, no root required.
#
# The .so symlinks in the extracted tree point at runtime libraries that are
# NOT in the tree, so they dangle and find_library rejects them. Repointing
# them at the system runtime is what makes the link work.
# ---------------------------------------------------------------------------
GLDEV="$DEPS/gldev"
if [ ! -f "$GLDEV/root/usr/include/EGL/egl.h" ]; then
    echo "==> fetching X11 + EGL headers"
    mkdir -p "$GLDEV" && cd "$GLDEV"
    apt-get download libgl-dev libegl-dev libglvnd-dev mesa-common-dev \
                     libx11-dev libxext-dev >/dev/null 2>&1 || true
    for d in *.deb; do [ -f "$d" ] && dpkg-deb -x "$d" root; done
    LIBDIR="$GLDEV/root/usr/lib/x86_64-linux-gnu"
    [ -e /usr/lib/x86_64-linux-gnu/libEGL.so.1 ] && \
        ln -sf /usr/lib/x86_64-linux-gnu/libEGL.so.1 "$LIBDIR/libEGL.so"
    [ -e /usr/lib/x86_64-linux-gnu/libGL.so.1 ] && \
        ln -sf /usr/lib/x86_64-linux-gnu/libGL.so.1 "$LIBDIR/libGL.so"
    cd - >/dev/null
fi
echo "    configure with -DFOLIO_GLDEV_ROOT=$GLDEV/root"
