#!/usr/bin/env bash
# =============================================================================
# build_sdl3_win32.sh — reproducible i686 Windows (PE) SDL3 build for the Windows host
# =============================================================================
# Sibling of tools/build_sdl3_i386.sh. Build enablement for the Windows port of the
# in-process SDL3 window/audio layer (rothc.exe, the CROSS=mingw Makefile axis).
#
# WHY: rothc.exe is an immovably 32-bit (i686) PE (the engine C uses fixed 1996 linear
# addresses). Folding SDL3 in-process therefore needs an i686 Windows SDL3 to link
# against. There is no prebuilt DLL in this tree by policy (policy: SDL3-win32 is
# BUILT FROM SOURCE, version-pinned to the SAME SDL3 release as the Linux build), so
# we cross-compile our own here with the MinGW i686 toolchain.
#
# WHAT IT PRODUCES (all git-ignored — see the top-level .gitignore: third_party/):
#   roth_c/third_party/SDL-src/            pinned SDL source checkout (SHARED with the
#                                          i386 script — one checkout, two build trees)
#   roth_c/third_party/SDL-src/build-win32 CMake build tree (i686 PE)
#   roth_c/third_party/sdl3-win32/include/ SDL3 headers  (SDL3_INC on the CROSS axis)
#   roth_c/third_party/sdl3-win32/lib/     libSDL3.dll.a (import lib; -lSDL3 resolves it)
#                                          + libSDL3.a (static; keeps packaging reversible)
#   roth_c/third_party/sdl3-win32/bin/     SDL3.dll (the runtime DLL dist-win bundles)
#
# Unlike the Linux i386 build, the Windows SDL backends that matter are all IN-TREE:
# video = Win32, audio = WASAPI. There is no dlopen'd system client lib to satisfy, so
# no -dev:i386 client packages and no pkg-config are needed (a genuine simplification).
# CMAKE_SYSTEM_NAME=Windows makes CMake emit PE and auto-select the Win32/WASAPI backends.
#
# BUNDLE PLAN: the shipped rothc.exe links the import lib libSDL3.dll.a; at runtime the
# Windows loader finds SDL3.dll beside the exe automatically (the default search order —
# no rpath/$ORIGIN analog is needed on Windows). `make dist-win` copies bin/SDL3.dll next
# to rothc.exe (the portable-zip dist).
#
# REPRODUCIBLE + RE-RUNNABLE: the SDL release tag is pinned (SDL_TAG below, MUST equal
# build_sdl3_i386.sh's pin — same SDL3 version on both OSes). Re-running fetches/checks
# out the pinned tag and rebuilds. Pass --clean to wipe the build tree + install first.
#
# USAGE:
#   tools/build_sdl3_win32.sh              # fetch (if needed) + configure + build + install
#   tools/build_sdl3_win32.sh --clean      # from-scratch rebuild
#   SDL_TAG=release-3.4.10 tools/build_sdl3_win32.sh   # different pin (keep in lockstep!)
#
# PREREQS (the script checks): the MinGW i686 cross toolchain (i686-w64-mingw32-gcc,
# i686-w64-mingw32-windres), cmake, make (or ninja), git. NO -m32 multilib, NO i386
# client libs — Windows SDL links Win32 APIs directly.
# =============================================================================
set -euo pipefail

# MUST equal build_sdl3_i386.sh's SDL_TAG (same SDL3 version on both OSes).
SDL_TAG="${SDL_TAG:-release-3.4.12}"
SDL_REPO="${SDL_REPO:-https://github.com/libsdl-org/SDL}"

# repo-root-relative layout (script lives in <repo>/tools/)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TP="$REPO_ROOT/roth_c/third_party"
SRC="$TP/SDL-src"          # SHARED with build_sdl3_i386.sh (one checkout, two build trees)
BUILD="$SRC/build-win32"
PREFIX="$TP/sdl3-win32"

JOBS="$(nproc 2>/dev/null || echo 4)"

XPFX="i686-w64-mingw32-"

log() { printf '\n\033[1;36m[sdl3-win32]\033[0m %s\n' "$*"; }
die() { printf '\n\033[1;31m[sdl3-win32] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# ---- preflight ------------------------------------------------------------
command -v git                     >/dev/null || die "git not found"
command -v cmake                   >/dev/null || die "cmake not found"
command -v make                    >/dev/null || die "make not found (needed for the Unix Makefiles generator fallback)"
command -v "${XPFX}gcc"            >/dev/null || die "${XPFX}gcc not found (install gcc-mingw-w64-i686)"
command -v "${XPFX}windres"        >/dev/null || die "${XPFX}windres not found (install binutils-mingw-w64-i686)"
# ninja is OPTIONAL — same policy as build_sdl3_i386.sh: prefer it, fall back to make.
GEN="Unix Makefiles"; command -v ninja >/dev/null && GEN="Ninja"

if [ "${1:-}" = "--clean" ]; then
  log "--clean: removing $BUILD and $PREFIX"
  rm -rf "$BUILD" "$PREFIX"
fi

mkdir -p "$TP"

# ---- fetch (pinned, shallow) — SHARED source checkout with the i386 script ------------
if [ ! -d "$SRC/.git" ]; then
  log "cloning $SDL_REPO @ $SDL_TAG (shallow)"
  git clone --depth 1 --branch "$SDL_TAG" "$SDL_REPO" "$SRC"
else
  log "SDL-src present; fetching + checking out $SDL_TAG"
  git -C "$SRC" fetch --depth 1 origin "refs/tags/$SDL_TAG:refs/tags/$SDL_TAG" 2>/dev/null || true
  git -C "$SRC" checkout -q "$SDL_TAG"
fi
ACTUAL_REV="$(git -C "$SRC" rev-parse --short HEAD)"
log "SDL source at $SDL_TAG ($ACTUAL_REV)"

# ---- i686 MinGW (Windows PE) cross toolchain file -------------------------
mkdir -p "$BUILD"
TC="$BUILD/toolchain-win32.cmake"
cat > "$TC" <<EOF
# generated by tools/build_sdl3_win32.sh — i686 Windows (PE) MinGW cross toolchain
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)
set(CMAKE_C_COMPILER   ${XPFX}gcc)
set(CMAKE_CXX_COMPILER ${XPFX}g++)
set(CMAKE_RC_COMPILER  ${XPFX}windres)
set(CMAKE_FIND_ROOT_PATH /usr/i686-w64-mingw32)
# host tools (cmake/ninja/make) come from the host PATH; libs/headers only from the mingw sysroot
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF

# ---- configure ------------------------------------------------------------
# Same flag set as build_sdl3_i386.sh; CMAKE_SYSTEM_NAME=Windows picks the Win32/WASAPI backends.
log "configuring (generator: $GEN, jobs: $JOBS) -> $PREFIX"
cmake -S "$SRC" -B "$BUILD" -G "$GEN" \
  -DCMAKE_TOOLCHAIN_FILE="$TC" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DSDL_SHARED=ON \
  -DSDL_STATIC=ON \
  -DSDL_TESTS=OFF \
  -DSDL_TEST_LIBRARY=OFF \
  -DSDL_EXAMPLES=OFF \
  -DSDL_INSTALL_TESTS=OFF \
  -DSDL_CAMERA=OFF \
  -DSDL_HAPTIC=OFF \
  -DSDL_DISABLE_INSTALL_DOCS=ON

# ---- build + install ------------------------------------------------------
log "building"
cmake --build "$BUILD" --parallel "$JOBS"
log "installing"
cmake --install "$BUILD"

# ---- report ---------------------------------------------------------------
log "artifacts:"
ls -l "$PREFIX/bin"/SDL3.dll "$PREFIX/lib"/libSDL3.dll.a "$PREFIX/lib"/libSDL3.a 2>/dev/null || true
DLL="$PREFIX/bin/SDL3.dll"
[ -f "$DLL" ] && { echo; file "$DLL"; }
echo
log "Makefile glue (CROSS=mingw):  SDL3_INC=$PREFIX/include   SDL3_LIB=$PREFIX/lib"
log "                              dist-win bundles $DLL beside rothc.exe"
log "done ($SDL_TAG / $ACTUAL_REV)"
