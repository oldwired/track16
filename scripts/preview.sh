#!/bin/sh
# Renders the screen to PNG files on the build machine (tools/preview.cpp), inside the toolchain container, natively:
#   scripts/preview.sh [modules-folder] [module file name …]   → out/preview/*.png
# Default modules folder: ../modules next to this repository.
set -eu
cd "$(dirname "$0")/.."
base=${TOOLCHAIN_IMAGE:-sp16-toolchain:gcc11-egl}
image=sp16-toolchain:preview
docker image inspect "$base" >/dev/null 2>&1 || docker build -t "$base" toolchain
docker image inspect "$image" >/dev/null 2>&1 || docker build -t "$image" --build-arg BASE="$base" -f toolchain/preview.Dockerfile toolchain
modules=$(cd "${1:-../modules}" && pwd)
[ $# -gt 0 ] && shift
mkdir -p out/preview
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/work -v "$modules":/modules:ro -w /work "$image" sh -c '
  set -e
  [ -f build/preview/CMakeCache.txt ] || cmake -S . -B build/preview -G Ninja -DTRACK16_PREVIEW=ON
  cmake --build build/preview --target track16_preview -j$(nproc)
  rm -rf out/preview/data
  build/preview/track16_preview /modules out/preview "$@"
' sh "$@"
