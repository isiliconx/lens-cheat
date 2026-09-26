#!/usr/bin/env bash
# lens build script.
#
#   ./build.sh                 configure + release build
#   ./build.sh debug           debug build
#   ./build.sh clean           wipe build/
#
# Linux / macOS: needs cmake >= 3.16 and a C++20 compiler (gcc 10+, clang 12+).
# Windows:       needs MSVC 2019+ (Developer Prompt) or MinGW-w64 g++.
set -euo pipefail

cd "$(dirname "$0")"

CFG=Release
case "${1:-}" in
  debug) CFG=Debug ;;
  clean) rm -rf build; echo "wiped build/"; exit 0 ;;
  "") ;;
  *) echo "usage: $0 [debug|clean]"; exit 2 ;;
esac

JOBS="$( (command -v nproc >/dev/null && nproc) || echo 4)"

# Pick a compiler that can actually build. On this box /usr/bin/c++ is a Clang
# that fails CMake's own test program, while g++ works, so probe a candidate
# list rather than trusting the default.
if [ -z "${CXX:-}" ]; then
  for c in g++ clang++ c++; do
    if command -v "$c" >/dev/null 2>&1 &&
       echo 'int main(){return 0;}' | "$c" -x c++ -std=c++20 - -o /dev/null 2>/dev/null; then
      CXX="$c"
      break
    fi
  done
  [ -n "${CXX:-}" ] || { echo "no working C++20 compiler found (tried g++ clang++ c++)" >&2; exit 1; }
  export CXX
  echo "using CXX=$CXX"
fi

cmake -S . -B build -DCMAKE_BUILD_TYPE="$CFG" -DCMAKE_CXX_COMPILER="$CXX"
cmake --build build -j "$JOBS"

echo
echo "built:"
[ -f build/lens ]     && echo "  build/lens"
[ -f build/lens_sim ] && echo "  build/lens_sim"
echo
echo "quick check:  ./build/lens selftest"
