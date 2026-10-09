#!/usr/bin/env bash
# tools/fetch-deps.sh  --  work package A2 (vendored binary dependencies)
#
# Provisions the prebuilt third-party binaries Folio links against into a
# single cache directory, so a clean machine can go from `git clone` to a
# green `ctest` with one command and no package manager.
#
#   ./tools/fetch-deps.sh [--dir <cache>] [--skip-skia] [--skip-podofo]
#
# Default cache:  $HOME/.cache/folio-deps
# Override with:  FOLIO_DEPS_DIR=/some/path ./tools/fetch-deps.sh
#
# WHY VENDORED PREBUILTS RATHER THAN A PACKAGE MANAGER
# ----------------------------------------------------
# PDFium is not in any distro. Building it from source needs depot_tools, GN,
# a ~20 GB checkout and roughly an hour per platform. bblanchon/pdfium-binaries
# publishes the same GN build for all six of our targets from one CI matrix,
# which is exactly the reproducibility property we want. Skia is the same story
# and worse (no stable ABI at all - see the pin discipline below).
#
# PIN DISCIPLINE
# --------------
# Every version below is an exact, immutable tag. Never use `latest` in CI:
# PDFium cuts a release most weekdays and Skia has no API stability guarantee
# whatsoever. Uplift is a deliberate, reviewed act (plan Appendix C.8).

set -euo pipefail

# --- pinned versions -------------------------------------------------------
PDFIUM_RELEASE="chromium/7568"          # PDFium 157.0.8086.0
SKIA_RELEASE="m144-e2e6623374-4"        # Skia milestone 144
PODOFO_TAG="1.1.0"

DEPS_DIR="${FOLIO_DEPS_DIR:-$HOME/.cache/folio-deps}"
SKIP_SKIA=0
SKIP_PODOFO=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dir)         DEPS_DIR="$2"; shift 2 ;;
        --skip-skia)   SKIP_SKIA=1; shift ;;
        --skip-podofo) SKIP_PODOFO=1; shift ;;
        -h|--help)     sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

mkdir -p "$DEPS_DIR"
cd "$DEPS_DIR"

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
have() { [[ -e "$1" ]]; }

# --- host triple -----------------------------------------------------------
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64)   PDFIUM_PLAT="linux-x64";   SKIA_PLAT="linux-Release-x64" ;;
    Linux-aarch64)  PDFIUM_PLAT="linux-arm64"; SKIA_PLAT="linux-Release-arm64" ;;
    Darwin-arm64)   PDFIUM_PLAT="mac-arm64";   SKIA_PLAT="macos-Release-arm64" ;;
    Darwin-x86_64)  PDFIUM_PLAT="mac-x64";     SKIA_PLAT="macos-Release-x64" ;;
    *) echo "unsupported host: $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac

# ---------------------------------------------------------------------------
# PDFium  --  BSD-3-Clause. Rendering, text extraction, forms. Required.
# ---------------------------------------------------------------------------
if have pdfium/include/fpdfview.h; then
    say "PDFium already present ($(sed -n 's/^MAJOR=//p' pdfium/VERSION 2>/dev/null || echo '?'))"
else
    say "Fetching PDFium $PDFIUM_RELEASE ($PDFIUM_PLAT)"
    curl -fsSL -o pdfium.tgz \
        "https://github.com/bblanchon/pdfium-binaries/releases/download/${PDFIUM_RELEASE}/pdfium-${PDFIUM_PLAT}.tgz"
    mkdir -p pdfium && tar xzf pdfium.tgz -C pdfium && rm -f pdfium.tgz
fi

# ---------------------------------------------------------------------------
# Skia  --  BSD-3-Clause. Optional: only the Skia render backend needs it.
#
# NOTE, MEASURED 2026-10-08: the JetBrains prebuilt is built WITHOUT Graphite.
#   nm -C libskia.a | grep -c skgpu::graphite   ->  0
#   nm -C libskia.a | grep -c GrDirectContext   ->  229
# So this archive gives you Ganesh (GL/Vulkan) and CPU raster only. Graphite +
# Dawn - the thing that makes "one backend on six platforms" true - requires
# building Skia yourself from GN with skia_enable_graphite=true. Budget that;
# do not assume a prebuilt will hand it to you. (Plan Appendix C.3.)
# ---------------------------------------------------------------------------
if [[ $SKIP_SKIA -eq 0 ]]; then
    if have skia/include/core/SkCanvas.h; then
        say "Skia already present (milestone $(sed -n 's/#define SK_MILESTONE //p' skia/include/core/SkMilestone.h))"
    else
        say "Fetching Skia $SKIA_RELEASE ($SKIA_PLAT)"
        curl -fsSL -o skia.zip \
            "https://github.com/JetBrains/skia-pack/releases/download/${SKIA_RELEASE}/Skia-${SKIA_RELEASE}-${SKIA_PLAT}.zip"
        mkdir -p skia && unzip -q -o skia.zip -d skia && rm -f skia.zip
    fi
fi

# ---------------------------------------------------------------------------
# PoDoFo  --  LGPL-2.0-or-later OR MPL-2.0. We elect MPL-2.0 (plan §2.1):
# file-level copyleft, safe to static-link, which is what iOS forces on us.
#
# Must be >= 1.0.0. Debian trixie ships 0.9.8, which predates both the MPL
# dual-licence option and the PdfMemDocument API we use - so we build it.
# ---------------------------------------------------------------------------
if [[ $SKIP_PODOFO -eq 0 ]]; then
    if have podofo/install/include/podofo/podofo.h; then
        say "PoDoFo already present"
    else
        say "Fetching + building PoDoFo $PODOFO_TAG (MPL-2.0 election)"
        curl -fsSL -o podofo-src.tgz \
            "https://github.com/podofo/podofo/archive/refs/tags/${PODOFO_TAG}.tar.gz"
        mkdir -p podofo && tar xzf podofo-src.tgz -C podofo --strip-components=1 && rm -f podofo-src.tgz
        cmake -S podofo -B podofo/build -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="$DEPS_DIR/podofo/install" \
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
            -DPODOFO_BUILD_STATIC=ON \
            -DPODOFO_BUILD_TEST=OFF \
            -DPODOFO_BUILD_EXAMPLES=OFF \
            -DPODOFO_BUILD_TOOLS=OFF
        # PODOFO_BUILD_TOOLS=OFF is not cosmetic: the command-line tools are
        # GPL-licensed while the library is LGPL/MPL. Shipping them would
        # relicense the product. Keep this OFF forever. (Plan Appendix A.)
        cmake --build podofo/build --parallel "$(nproc 2>/dev/null || echo 4)"
        cmake --install podofo/build
    fi
fi

say "Dependencies ready in $DEPS_DIR"
printf '\nConfigure with:\n  cmake --preset linux-debug -DFOLIO_DEPS_DIR=%s\n\n' "$DEPS_DIR"
