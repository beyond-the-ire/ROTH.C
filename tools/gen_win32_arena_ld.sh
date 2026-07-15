#!/usr/bin/env bash
# Generate the Windows (MinGW) arena linker script.
#
# The native engine needs real committed memory at a set of fixed low virtual
# addresses (a 1996 memory layout it stores absolute pointers into). On Windows
# the process heap, thread stacks, and locale (.nls) mappings colonize the low
# address space during process startup, before any application code runs, so
# those addresses cannot be reserved after the fact.
#
# The fix is to let the loader itself pre-claim the region: base the image low
# (0x10000) and place a zero-fill ".arena" section covering the fixed window, so
# the loader's contiguous image reservation owns those addresses before the CRT
# can place anything there. The real code and data are pushed above the arena.
#
# This script derives the layout from the linker's OWN default PE script (so it
# tracks the installed toolchain) and applies exactly two edits:
#   1. insert the ".arena" zero-fill section right after the header page, sized
#      to reach the top of the fixed window;
#   2. let ".text" (and everything after it) follow the location counter instead
#      of being pinned just above the header, so it lands above the arena.
#
# Usage: gen_win32_arena_ld.sh <i686-w64-mingw32-ld> <out.ld>
set -euo pipefail

LD="${1:?usage: gen_win32_arena_ld.sh <ld> <out.ld>}"
OUT="${2:?usage: gen_win32_arena_ld.sh <ld> <out.ld>}"

# The arena spans from the first page after the header (image base + 0x1000, i.e.
# virtual address 0x11000) up to the top of the fixed window (0x4a9000, one page
# past the highest fixed arena address). Size = 0x4a9000 - 0x11000.
ARENA_FILL=0x498000

# Extract the default PE linker script (the block the linker prints between its
# "====" banners under --verbose).
DEFAULT="$("$LD" --verbose | sed -n '/^====.*====$/,/^====.*====$/p' | sed '1d;$d')"

printf '%s\n' "$DEFAULT" | awk -v fill="$ARENA_FILL" '
  # Insert the .arena section after the first post-header ALIGN, just before .text.
  # The .text placement line is rewritten so it follows the location counter.
  /^  \.text  __image_base__ \+ / && !done {
    print "  .arena  __image_base__ + ( __section_alignment__ < 0x1000 ? . : __section_alignment__ ) :"
    print "  {"
    print "    __arena_start__ = . ;"
    print "    . = . + " fill " ;"
    print "    __arena_end__ = . ;"
    print "  }"
    print "  .text  BLOCK(__section_alignment__) :"
    done = 1
    next
  }
  { print }
' > "$OUT"

# Fail loudly if the edits did not take (a future toolchain could rename things).
grep -q '\.arena' "$OUT" || { echo "gen_win32_arena_ld: .arena insert failed" >&2; exit 1; }
grep -q '\.text  BLOCK' "$OUT" || { echo "gen_win32_arena_ld: .text relocation failed" >&2; exit 1; }
