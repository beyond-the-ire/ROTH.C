#!/usr/bin/env bash
# pe_arena_gate.sh — post-link geometry gate for the Windows (PE) build.
#
# The engine needs committed memory at a set of fixed low virtual addresses (a 1996 layout it stores
# absolute pointers into). On Windows the image itself pre-claims them: it is based at 0x10000 with a
# zero-fill .arena section covering the fixed window, so the loader's own image reservation owns those
# addresses before the process heap / thread stacks / locale mappings can. This gate reads the BUILT
# executable back and asserts that geometry actually landed. If a toolchain change, a stray link flag,
# or a linker-script regression ever moves the base, drops the section, shrinks it, lets .text overlap
# the window, or turns ASLR back on, the arena is silently broken at load time — catching it here
# fails the build instead of shipping an exe that cannot map its arena.
#
# Usage: pe_arena_gate.sh <objdump> <exe>
set -euo pipefail

OBJDUMP="${1:?usage: pe_arena_gate.sh <objdump> <exe>}"
EXE="${2:?usage: pe_arena_gate.sh <objdump> <exe>}"

# Expected geometry — must match tools/gen_win32_arena_ld.sh (ARENA_FILL, the .arena placement) and
# the Makefile's CROSS=mingw LDFLAGS (--image-base, --disable-dynamicbase).
EXP_IMAGE_BASE=0x10000
EXP_ARENA_RVA=0x1000       # .arena starts one page past the header page (VA 0x11000)
EXP_ARENA_VSZ=0x498000     # 0x4a9000 - 0x11000: the fixed window
EXP_TEXT_MIN=0x4a9000      # .text (and everything after) must land above the arena
DYNAMIC_BASE_BIT=0x40      # IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE (ASLR)

echo "== PE arena geometry ($EXE) =="

hdr="$("$OBJDUMP" -p "$EXE")"
sec="$("$OBJDUMP" -h "$EXE")"

fail=0
note() { printf '  %-18s %s\n' "$1" "$2"; }
bad()  { printf '  FAIL: %s\n' "$1"; fail=1; }

# --- ImageBase == 0x10000 (objdump -p) ---
image_base="0x$(printf '%s\n' "$hdr" | awk '/^ImageBase/ {print $2}')"
note "ImageBase" "$image_base (want $EXP_IMAGE_BASE)"
[ "$((image_base))" -eq "$((EXP_IMAGE_BASE))" ] || bad "ImageBase is $image_base, not $EXP_IMAGE_BASE"

# --- DllCharacteristics has no dynamic-base (ASLR) bit (objdump -p) ---
dllchar="0x$(printf '%s\n' "$hdr" | awk '/^DllCharacteristics/ {print $2}')"
if [ "$(( dllchar & DYNAMIC_BASE_BIT ))" -ne 0 ]; then
    note "DllCharacteristics" "$dllchar (dynamic-base bit $DYNAMIC_BASE_BIT SET)"
    bad "DllCharacteristics $dllchar has the dynamic-base bit $DYNAMIC_BASE_BIT set — ASLR would relocate the arena"
else
    note "DllCharacteristics" "$dllchar (no dynamic-base bit — good)"
fi

# --- .arena: RVA (VMA - ImageBase) == 0x1000, virtual size == 0x498000, no file space (objdump -h) ---
# objdump -h columns:  Idx Name Size VMA LMA "File off" Algn.  For a PE the Size column is the section's
# VIRTUAL size; a zero-fill section has "File off" 0 (no bytes on disk).
arena_line="$(printf '%s\n' "$sec" | awk '$2 == ".arena" {print}')"
if [ -z "$arena_line" ]; then
    bad ".arena section is missing"
else
    arena_vsz="0x$(printf '%s\n' "$arena_line" | awk '{print $3}')"
    arena_vma="0x$(printf '%s\n' "$arena_line" | awk '{print $4}')"
    arena_foff="0x$(printf '%s\n' "$arena_line" | awk '{print $6}')"
    arena_rva=$(( arena_vma - image_base ))
    note ".arena VMA" "$arena_vma (RVA $(printf '0x%x' "$arena_rva"), want $EXP_ARENA_RVA)"
    note ".arena vsize" "$arena_vsz (want $EXP_ARENA_VSZ)"
    note ".arena file off" "$arena_foff (want 0x0 — zero-fill, no file space)"
    [ "$arena_rva" -eq "$((EXP_ARENA_RVA))" ] || bad ".arena RVA is $(printf '0x%x' "$arena_rva"), not $EXP_ARENA_RVA"
    [ "$((arena_vsz))" -eq "$((EXP_ARENA_VSZ))" ] || bad ".arena virtual size is $arena_vsz, not $EXP_ARENA_VSZ"
    [ "$((arena_foff))" -eq 0 ] || bad ".arena occupies file space (File off $arena_foff != 0)"
fi

# --- .text starts above the arena (objdump -h) ---
text_vma="0x$(printf '%s\n' "$sec" | awk '$2 == ".text" {print $4}')"
if [ "$text_vma" = "0x" ]; then
    bad ".text section is missing"
else
    note ".text VMA" "$text_vma (want >= $EXP_TEXT_MIN)"
    [ "$((text_vma))" -ge "$((EXP_TEXT_MIN))" ] || bad ".text VMA $text_vma is below the arena top $EXP_TEXT_MIN"
fi

if [ "$fail" -ne 0 ]; then
    echo "== PE arena geometry: FAIL =="
    exit 1
fi
echo "== PE arena geometry: PASS =="
