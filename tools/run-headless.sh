#!/usr/bin/env bash
# tools/run-headless.sh - run the native desktop shell with no real display.
#
# Starts Xvfb with Mesa's llvmpipe behind it, so folio-desktop gets a genuine
# X11 window and a genuine EGL/Ganesh GL context, just with a software
# rasteriser underneath. That is enough to exercise every part of the native
# path that CI and this sandbox otherwise cannot reach: window creation,
# context creation, the event loop, the clipboard's selection protocol, and
# the frame pipeline end to end.
#
# It is NOT a performance test. llvmpipe is a CPU rasteriser; timings taken
# here say nothing about a real GPU.
#
#   tools/run-headless.sh                 # run until killed
#   tools/run-headless.sh --shot out.png  # run, screenshot, exit
#   tools/run-headless.sh --software      # force the CPU paint path
#
# Everything is fetched by tools/bootstrap-headless.sh into ~/.cache and
# needs no root, except one symlink: Xvfb execs "/usr/bin/xkbcomp" by
# absolute path, so that one has to exist.

set -euo pipefail

CACHE="${FOLIO_DEPS_CACHE:-$HOME/.cache/folio-deps}"
R="$CACHE/xvfb/root"
DISP="${FOLIO_HEADLESS_DISPLAY:-:99}"
SIZE="${FOLIO_HEADLESS_SIZE:-1440x900}"
BIN="${FOLIO_DESKTOP_BIN:-build/folio-ui/folio-desktop}"

SHOT=""
EXTRA=()
while [ $# -gt 0 ]; do
    case "$1" in
        --shot) SHOT="$2"; shift 2 ;;
        *) EXTRA+=("$1"); shift ;;
    esac
done

if [ ! -x "$R/usr/bin/Xvfb" ]; then
    echo "no Xvfb in $R - run tools/bootstrap-headless.sh first" >&2
    exit 1
fi
if [ ! -x "$BIN" ]; then
    echo "no $BIN - build folio-desktop first" >&2
    exit 1
fi

export LD_LIBRARY_PATH="$R/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
export LIBGL_DRIVERS_PATH="$R/usr/lib/x86_64-linux-gnu/dri"
export LIBGL_ALWAYS_SOFTWARE=1
export DISPLAY="$DISP"

cleanup() {
    [ -n "${APP_PID:-}" ] && kill "$APP_PID" 2>/dev/null || true
    [ -n "${X_PID:-}" ] && kill "$X_PID" 2>/dev/null || true
}
trap cleanup EXIT

mkdir -p /tmp/.X11-unix
"$R/usr/bin/Xvfb" "$DISP" -screen 0 "${SIZE}x24" -nolisten tcp \
    -xkbdir "$R/usr/share/X11/xkb" >/tmp/folio-xvfb.log 2>&1 &
X_PID=$!

for _ in $(seq 1 40); do
    [ -S "/tmp/.X11-unix/X${DISP#:}" ] && break
    sleep 0.25
done
if [ ! -S "/tmp/.X11-unix/X${DISP#:}" ]; then
    echo "Xvfb did not come up:" >&2
    tail -5 /tmp/folio-xvfb.log >&2
    exit 1
fi

"$BIN" --size "${SIZE/x/x}" "${EXTRA[@]+"${EXTRA[@]}"}" &
APP_PID=$!

if [ -n "$SHOT" ]; then
    sleep 8
    if ! kill -0 "$APP_PID" 2>/dev/null; then
        echo "folio-desktop exited early" >&2
        exit 1
    fi
    "$R/usr/bin/xwd" -root -silent > /tmp/folio-shot.xwd
    python3 "$(dirname "$0")/xwd2png.py" /tmp/folio-shot.xwd "$SHOT"
    echo "wrote $SHOT"
    exit 0
fi

wait "$APP_PID"
