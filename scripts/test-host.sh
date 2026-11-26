#!/bin/sh
# Builds and runs the player engine's host tests (tests/) inside the toolchain container, natively for the build
# machine. Module files: the folder given as $1 (default: ../modules next to this repository).
#   scripts/test-host.sh [modules-folder]
set -eu
cd "$(dirname "$0")/.."
image=${TOOLCHAIN_IMAGE:-sp16-toolchain:gcc11-egl}
modules=$(cd "${1:-../modules}" && pwd)
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/work -v "$modules":/modules:ro -w /work "$image" sh -c "
  set -e
  [ -f build/host-tests/CMakeCache.txt ] || cmake -S tests -B build/host-tests -G Ninja
  cmake --build build/host-tests -j\$(nproc)
  build/host-tests/player_test /modules
"
