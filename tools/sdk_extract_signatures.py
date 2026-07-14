#!/usr/bin/env python3
"""sdk_extract_signatures.py — extract sdk/schema/functions.json from engine.h.

Mechanically parses EVERY function prototype in roth_c/src/engine/engine.h (the lifted
engine surface: ordinary i386 C ABI, register convention surviving only as parameter
ORDER) and records its typed signature keyed by the permanent function ID (canonical VA;
resolution via tools/sdk_common). One row per resolvable prototype:

    {"id": <int>, "va": "0x..", "name": <engine.h symbol>, "ret": <type>,
     "params": [{"name":..,"type":..}, ...], "subsystem":.., "klass":..,
     "resolve": "name"|"trailing-hex", "curated": bool, "exposed": bool}

Prototypes with no canonical corpus VA (host-harness helpers like call_orig, ABI-register
wrappers, mid-entries, standalone-only shims) are written to sdk/schema/functions_skipped.json
with a reason — reported honestly, never guessed onto a VA.

`curated`/`exposed` are set true for the v0.1 STARTER SET: the engine calls the two shipped
mods (item_grabber, doc_viewer) already reuse. Growth is APPEND-ONLY per the versioning
policy — exposing more functions is a minor bump; never un-expose, never change a signature.
"""
import json
import os
import sys

import sdk_common as C

OUT = os.path.join(C.REPO, "sdk", "schema", "functions.json")
OUT_SKIP = os.path.join(C.REPO, "sdk", "schema", "functions_skipped.json")

# THE EXPOSED-ORDER LAW: the ORDER of this
# list IS the engine-call table slot order — a frozen ABI. New exposures are APPENDED AT THE END,
# never inserted/reordered/removed, so existing slot offsets NEVER move and every future exposure is
# a true append-only minor bump. (Through v0.4 the table was id-sorted; slots 0-21 below are that
# v0.4 baseline order, frozen forever; v0.5 appended slots 22-23.) CI enforces prefix-stability vs
# the released baseline (tools/sdk_check.py exposure-order gate). Every entry is a proven reuse
# point in a shipped example plugin (sdk/examples/item_grabber and/or sdk/examples/doc_viewer).
# Keyed by canonical engine.h name; the extractor verifies each resolves.
STARTER_SET = [
    # --- v0.1 starter set, in the frozen v0.4 table order (slots 0-21; happened to be id-sorted) ---
    "clear_framebuffer_rect",       # slot  0  0x12cea  clear a framebuffer rectangle
    "save_framebuffer_region",      # slot  1  0x13062  save a framebuffer rectangle (returns handle)
    "blit_item_icon",               # slot  2  0x13544  icon blit primitive
    "draw_text_to_buffer",          # slot  3  0x14d04  native UI text into an arbitrary buffer
    "register_dirty_rect",          # slot  4  0x15b5b  mark a screen rectangle dirty
    "screen_xy_to_framebuffer_ptr", # slot  5  0x18040  screen x,y -> framebuffer pointer (hires-aware)
    "load_item_icon_resource",      # slot  6  0x1816a  resolve+load an item icon into the DAS pool
    "copy_record_block_op7",        # slot  7  0x1854b  copy a record block (op 7)
    "load_das_cache_resource",      # slot  8  0x1869b  load a DAS resource into the cache
    "blit_das_image_auto_scale",    # slot  9  0x18e48  auto-scale DAS image blit
    "blit_reloc_das_image",         # slot 10  0x18e68  blit an ICONS.ALL reloc tile
    "draw_text_at_screen_xy",       # slot 11  0x1a079  native UI text at screen x,y
    "blit_das_image_at_xy",         # slot 12  0x1a10a  DAS image blit at x,y
    "resolve_reloc_record_fields",  # slot 13  0x1c06b  resolve a reloc record's fields
    "give_item",                    # slot 14  0x1cedc  grant one item by entry index (item_grabber)
    "read_next_dialogue_line",      # slot 15  0x1e8cc  fetch a DBASE400 text line by id
    "measure_control_text_width",   # slot 16  0x1f91f  measure a control string's pixel width
    "build_available_choice_menu",  # slot 17  0x1f950  build the inspect choice menu
    "resolve_reloc_ptr",            # slot 18  0x226c6  resolve a UI reloc-pool slot -> descriptor
    "pool_free_handle",             # slot 19  0x360b3  free a DAS pool handle
    "dos_open_file",                # slot 20  0x41ae5  open a game file
    "dos_close_handle",             # slot 21  0x41b41  close a file handle
    # --- v0.5 (doc_viewer runtime-plugin port) — appended at the tail ---
    "blit_das_image_to_buffer",     # slot 22  0x1325b  blit a DAS descriptor 1:1 into a buffer
    "draw_popup_shadow_border_smc", # slot 23  0x12dde  draw the popup 9-slice shadow border
    # APPEND new exposures HERE — never above, never reorder (slot order is ABI).
]


def build():
    rows, by_name, by_va = C.load_corpus()
    protos = C.parse_engine_h()
    resolved, skipped, warnings = C.build_resolution(protos, by_name, by_va)

    starter = set(STARTER_SET)
    if len(starter) != len(STARTER_SET):
        raise SystemExit("STARTER_SET contains a duplicate name — slot order is ABI, fix the list")
    slot_of = {name: i for i, name in enumerate(STARTER_SET)}   # exposure order = table slot order
    seen_starter = set()
    out = []
    for r in resolved:
        corpus = by_va[r["va"]]
        curated = r["name"] in starter
        if curated:
            seen_starter.add(r["name"])
        row = {
            "id": r["id"],
            "va": r["va"],
            "name": r["name"],
            "ret": r["ret"],
            "params": r["params"],
            "subsystem": corpus.get("subsystem"),
            "klass": corpus.get("klass"),
            "resolve": r["method"],
            "curated": curated,
            "exposed": curated,
        }
        if curated:
            row["exposed_slot"] = slot_of[r["name"]]   # THE frozen engine-call-table slot (ABI)
        out.append(row)
    out.sort(key=lambda r: r["id"])

    missing_starter = sorted(starter - seen_starter)
    meta = {
        "schema": "roth-sdk/functions/v1",
        "source": "roth_c/src/engine/engine.h",
        "note": ("Mechanically extracted typed signatures for every engine.h prototype "
                 "that resolves to a canonical VA/id. exposed/curated = the exposed set; "
                 "exposed_slot = the engine-call-table slot (THE EXPOSED-ORDER LAW: slot "
                 "order = exposure order, append-only forever — see STARTER_SET in "
                 "tools/sdk_extract_signatures.py)."),
        "count": len(out),
        "exposed_count": sum(1 for r in out if r["exposed"]),
        "prototypes_parsed": len(protos),
        "skipped_count": len(skipped),
    }
    return meta, out, skipped, warnings, missing_starter


def render():
    """Return {path: text} for the generated schema files (single source used by both
    the writer and the CI drift check). Raises on unresolvable starter-set names."""
    meta, out, skipped, warnings, missing = build()
    if missing:
        raise SystemExit("starter-set names not found / not resolvable in engine.h: "
                         + ", ".join(missing))
    functions_text = json.dumps({"meta": meta, "functions": out}, indent=1) + "\n"
    skip_text = json.dumps({
        "schema": "roth-sdk/functions_skipped/v1",
        "note": ("engine.h prototypes with NO canonical corpus VA: host-harness helpers, "
                 "ABI-register wrappers, mid-entries, standalone-only shims. Recorded for "
                 "honesty and CI stability; never assigned an id."),
        "count": len(skipped),
        "skipped": skipped,
    }, indent=1) + "\n"
    return {OUT: functions_text, OUT_SKIP: skip_text}, meta, skipped, warnings


def main():
    meta, out, skipped, warnings, missing = build()
    if missing:
        print("ERROR: starter-set names not found / not resolvable in engine.h:",
              missing, file=sys.stderr)
        return 2
    write = "--write" in sys.argv or "--seed" in sys.argv
    texts, meta, skipped, warnings = render()
    functions_text = texts[OUT]
    skip_text = texts[OUT_SKIP]
    if write:
        open(OUT, "w").write(functions_text)
        open(OUT_SKIP, "w").write(skip_text)
        print(f"wrote {OUT}  ({meta['count']} functions, {meta['exposed_count']} exposed)")
        print(f"wrote {OUT_SKIP}  ({len(skipped)} skipped)")
    else:
        print(f"[dry-run] {meta['count']} functions, {meta['exposed_count']} exposed, "
              f"{len(skipped)} skipped — pass --write to emit")
    parsed = meta["prototypes_parsed"]
    print(f"  coverage: {meta['count']}/{parsed} prototypes resolved "
          f"({100.0*meta['count']/parsed:.1f}%)")
    if warnings:
        print(f"  resolver warnings: {len(warnings)}")
        for w in warnings:
            print("    WARN", w)
    return 0


if __name__ == "__main__":
    sys.exit(main())
