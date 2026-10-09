#!/usr/bin/env bash
# One-command recovery after a sandbox reset.
#
# build/ and ~/.cache/folio-deps are excluded from workspace snapshots, and
# cmake/ninja are pip-installed rather than system packages, so all three
# vanish when the sandbox is recycled. This puts them back.
set -euo pipefail
cd "$(dirname "$0")/.."

command -v cmake  >/dev/null 2>&1 || pip install --quiet cmake
command -v ninja  >/dev/null 2>&1 || pip install --quiet ninja
export PATH="$HOME/.local/bin:/usr/local/bin:$PATH"

# Always run it: every step is idempotent and guarded by its own
# "already present?" check, and skipping on the strength of one directory
# meant a partially-restored cache (Skia kept, GL headers gone) configured
# itself into a broken build.
bash tools/bootstrap-deps.sh

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DFOLIO_BUILD_UI=ON \
      -DFOLIO_GLDEV_ROOT="$HOME/.cache/folio-deps/gldev/root" "$@"
cmake --build build
echo
echo "ready: ./build/folio-ui/folio-stream --port 8080 --size 1440x900"
