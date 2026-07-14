#!/usr/bin/env bash
# =============================================================================
# build_sdl3_i386.sh — reproducible 32-bit (i386) SDL3 build for the standalone game host
# =============================================================================
# Build enablement for the in-process SDL3 window/audio layer.
#
# WHY: roth (the game binary) is immovably 32-bit (-m32; the engine C uses fixed 1996
# linear addresses). Folding SDL3 in-process therefore needs a 32-bit SDL3
# to link against. This box ships SDL3 **64-bit only** (/usr/lib/libSDL3.so),
# and no distro i386 SDL3 package exists, so we build our own i386 SDL3 here.
#
# WHAT IT PRODUCES (all git-ignored — see the top-level .gitignore):
#   roth_c/third_party/SDL-src/           pinned SDL source checkout
#   roth_c/third_party/SDL-src/build-i386 CMake build tree
#   roth_c/third_party/sdl3-i386/include/ SDL3 headers  (SDL3_I386_INC in the Makefile)
#   roth_c/third_party/sdl3-i386/lib/     libSDL3.so.0 (shared) + libSDL3.a (static)
#                                         (SDL3_I386_LIB in the Makefile)
#
# The build enables the backends whose *32-bit* client libs/headers are present
# (video: X11; audio: ALSA + PipeWire; + the dlopen'd GL/EGL/Vulkan loaders) and
# lets the rest auto-disable. SDL3 dlopens windowing/audio backends at RUNTIME,
# so the headers gate what compiles and the .so is loaded lazily; a missing 32-bit
# backend lib is a graceful runtime no-op, never a link failure. Backends observed
# to auto-disable on this box (no i386 lib): native Wayland (no i386 libxkbcommon),
# PulseAudio (no i386 libpulse). X11 covers Wayland sessions via XWayland.
#
# BOTH libs are built on purpose: the shared .so is the bundle default for the
# product ("$ORIGIN-rpath a beside-the-exe libSDL3.so.0"), the static .a keeps
# the bundle-vs-static packaging decision reversible.
#
# RPATH PLAN: the shipped roth links the shared lib and finds
# it beside the exe via `-Wl,-rpath,'$ORIGIN'` + libSDL3.so.0 copied next to the
# binary. For dev builds, -L/-rpath point at this install's lib/ dir.
#
# REPRODUCIBLE + RE-RUNNABLE: the SDL release tag is pinned (SDL_TAG below,
# overridable via env). Re-running fetches/checks out the pinned tag and rebuilds.
# Pass --clean to wipe the build tree + install first.
#
# USAGE:
#   tools/build_sdl3_i386.sh              # fetch (if needed) + configure + build + install
#   tools/build_sdl3_i386.sh --clean      # from-scratch rebuild
#   SDL_TAG=release-3.4.10 tools/build_sdl3_i386.sh   # different pin
#
# PREREQS (all present on this box; the script checks): gcc with -m32 multilib,
# cmake, make, git, pkg-config, and the i386 client libs in /usr/lib32 (Arch:
# lib32-libx11 lib32-alsa-lib lib32-libpipewire lib32-mesa lib32-glibc ...).
# =============================================================================
set -euo pipefail

SDL_TAG="${SDL_TAG:-release-3.4.12}"
SDL_REPO="${SDL_REPO:-https://github.com/libsdl-org/SDL}"

# repo-root-relative layout (script lives in <repo>/tools/)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TP="$REPO_ROOT/roth_c/third_party"
SRC="$TP/SDL-src"
BUILD="$SRC/build-i386"
PREFIX="$TP/sdl3-i386"

JOBS="$(nproc 2>/dev/null || echo 4)"

# 32-bit pkg-config: search ONLY the i386 .pc tree (+ arch-independent) so SDL's
# auto-detection never latches a 64-bit lib into the -m32 link.
export PKG_CONFIG_LIBDIR="/usr/lib32/pkgconfig:/usr/share/pkgconfig"

log() { printf '\n\033[1;36m[sdl3-i386]\033[0m %s\n' "$*"; }
die() { printf '\n\033[1;31m[sdl3-i386] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# ---- preflight ------------------------------------------------------------
command -v git   >/dev/null || die "git not found"
command -v cmake >/dev/null || die "cmake not found"
command -v make  >/dev/null || die "make not found"
command -v gcc   >/dev/null || die "gcc not found"
printf 'int main(void){return 0;}\n' | gcc -m32 -x c - -o /tmp/.sdl3_m32_probe 2>/dev/null \
  || die "gcc cannot build -m32 objects (install multilib: lib32-glibc / gcc-multilib)"
rm -f /tmp/.sdl3_m32_probe
GEN="Unix Makefiles"; command -v ninja >/dev/null && GEN="Ninja"

if [ "${1:-}" = "--clean" ]; then
  log "--clean: removing $BUILD and $PREFIX"
  rm -rf "$BUILD" "$PREFIX"
fi

mkdir -p "$TP"

# ---- fetch (pinned, shallow) ---------------------------------------------
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

# ---- i386 cross toolchain file -------------------------------------------
mkdir -p "$BUILD"
TC="$BUILD/toolchain-i386.cmake"
cat > "$TC" <<EOF
# generated by tools/build_sdl3_i386.sh — 32-bit (i386) toolchain
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)
set(CMAKE_C_FLAGS_INIT             "-m32")
set(CMAKE_CXX_FLAGS_INIT           "-m32")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-m32")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-m32")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-m32")
# find_library must prefer the multilib dir over /usr/lib (which is 64-bit here)
set(CMAKE_LIBRARY_PATH /usr/lib32)
set(CMAKE_SIZEOF_VOID_P 4)
# restrict pkg-config to the i386 .pc tree (belt-and-suspenders with the env var)
set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib32/pkgconfig:/usr/share/pkgconfig")
EOF

# ---- configure ------------------------------------------------------------
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
ls -l "$PREFIX/lib"/libSDL3.so* "$PREFIX/lib"/libSDL3.a 2>/dev/null || true
SO="$(ls "$PREFIX/lib"/libSDL3.so.*.* 2>/dev/null | head -1 || true)"
[ -n "$SO" ] && { echo; file "$SO"; }
echo
log "Makefile glue:  SDL3_I386_INC=$PREFIX/include   SDL3_I386_LIB=$PREFIX/lib"
log "done ($SDL_TAG / $ACTUAL_REV)"
