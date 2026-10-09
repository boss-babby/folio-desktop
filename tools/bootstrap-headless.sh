#!/usr/bin/env bash
# tools/bootstrap-headless.sh - fetch Xvfb + Mesa llvmpipe + X test tools.
#
# Everything lands in ~/.cache/folio-deps/xvfb and needs no root, with one
# exception noted at the end.
set -euo pipefail
CACHE="${FOLIO_DEPS_CACHE:-$HOME/.cache/folio-deps}"
DIR="$CACHE/xvfb"
mkdir -p "$DIR" && cd "$DIR"

PKGS="xvfb xauth libfontenc1 libxfont2 libpixman-1-0 libxkbfile1 libunwind8
      x11-xkb-utils xkb-data libgl1-mesa-dri libxcb-glx0 libxcb-dri2-0
      libxcb-dri3-0 libxcb-present0 libxcb-sync1 libxshmfence1 libdrm2
      libllvm19 libsensors5 libz3-4 libedit2 libxcb-randr0 libxcb-shm0
      libxcb-xfixes0 libwayland-server0 x11-apps xdotool libxdo3 libxtst6
      libxkbcommon0 xclip"

echo "==> downloading"
for p in $PKGS; do apt-get download "$p" >/dev/null 2>&1 || echo "   skip $p"; done
echo "==> unpacking"
for d in *.deb; do [ -f "$d" ] && dpkg-deb -x "$d" root; done

# Xvfb execs this by absolute path; there is no environment override.
if [ ! -e /usr/bin/xkbcomp ]; then
    echo "==> linking /usr/bin/xkbcomp (needs root, the only such step)"
    sudo ln -sf "$DIR/root/usr/bin/xkbcomp" /usr/bin/xkbcomp || {
        echo "   could not link xkbcomp; Xvfb will fail to start a keyboard" >&2
    }
fi
echo "==> headless harness ready in $DIR/root"
