#!/usr/bin/env python3
"""sdk_ids.py — seed / verify sdk/schema/function_ids.json (the identity registry).

Function identity in the modding SDK is a PERMANENT numeric ID == the canonical 1996
VA (verified 1:1 with the corpus: every VA unique, every name unique). Names are
diagnostics, not identity: they resolve through the {id, canonical_name, aliases[]}
history this file carries.

One row per corpus function (docs/reference/function_classification.json), reconciled
against roth_c/src/engine/engine.h:

    {"id": <int VA>, "va": "0x..", "canonical_name": <str>, "aliases": [<str>...],
     "subsystem": <str|null>, "klass": <str>}

canonical_name = the engine.h declared name where that VA is declared (that is the
LINKABLE symbol a mod author compiles against); otherwise the corpus name. When the two
differ (a rename), the corpus name is recorded as an alias so lookups by the old name
still resolve.

MAINTENANCE RULES (CI-enforced by tools/sdk_check.py against the committed baseline):
  * IDs are permanent — never reused, never renumbered.
  * A rename APPENDS the old name to aliases; aliases are never removed.
  * Every corpus VA must have exactly one row; ids and names (canonical+alias) unique.

USAGE:
  tools/sdk_ids.py --seed     # (re)generate the seed baseline (first commit only)
  tools/sdk_ids.py            # print what the seed WOULD be (diff aid); writes nothing
"""
import json
import os
import sys

import sdk_common as C

OUT = os.path.join(C.REPO, "sdk", "schema", "function_ids.json")

# Reserved synthetic-id space for future entries that have no unique 1996 VA
# (multi-entry shared code that must be addressed separately, aliased-without-VA
# helpers). v0.1 needs ZERO of these — all 1471 corpus functions have a unique VA.
SYNTHETIC_ID_BASE = 0x80000000


def build():
    rows, by_name, by_va = C.load_corpus()
    protos = C.parse_engine_h()
    resolved, skipped, warnings = C.build_resolution(protos, by_name, by_va)

    # engine.h name per VA (the current linkable symbol). Detect >1 proto per VA.
    eng_name_by_va = {}
    collisions = []
    for r in resolved:
        va = r["va"]
        if va in eng_name_by_va and eng_name_by_va[va] != r["name"]:
            # keep the exact-name match; report the other as an alias-worthy collision
            collisions.append((va, eng_name_by_va[va], r["name"], r["method"]))
            if r["method"] == "name":
                eng_name_by_va[va] = r["name"]
        else:
            eng_name_by_va[va] = r["name"]

    out = []
    for f in rows:
        va = f["va"].lower()
        corpus_name = f["name"]
        eng_name = eng_name_by_va.get(va)
        if eng_name and eng_name != corpus_name:
            canonical = eng_name
            aliases = [corpus_name]
        else:
            canonical = eng_name or corpus_name
            aliases = []
        # fold any extra colliding engine.h names for this VA into aliases
        for cva, keep, other, _m in collisions:
            if cva == va and other != canonical and other not in aliases:
                aliases.append(other)
        out.append({
            "id": int(va, 16),
            "va": va,
            "canonical_name": canonical,
            "aliases": aliases,
            "subsystem": f.get("subsystem"),
            "klass": f.get("klass"),
        })
    out.sort(key=lambda r: r["id"])
    meta = {
        "schema": "roth-sdk/function_ids/v1",
        "note": ("Permanent numeric function IDs (id == canonical 1996 VA). Names are "
                 "diagnostics; identity is the id. See tools/sdk_ids.py for maintenance "
                 "rules; tools/sdk_check.py enforces them in CI."),
        "synthetic_id_base": SYNTHETIC_ID_BASE,
        "corpus_source": "docs/reference/function_classification.json",
        "name_source": "roth_c/src/engine/engine.h",
        "count": len(out),
    }
    return meta, out, collisions, skipped, warnings


def dumps(meta, out):
    return json.dumps({"meta": meta, "functions": out}, indent=1) + "\n"


def main():
    meta, out, collisions, skipped, warnings = build()
    seed = "--seed" in sys.argv
    text = dumps(meta, out)
    if seed:
        open(OUT, "w").write(text)
        print(f"wrote {OUT}  ({len(out)} rows)")
    else:
        print(f"[dry-run] would write {OUT}  ({len(out)} rows) — pass --seed to write")
    aliased = sum(1 for r in out if r["aliases"])
    print(f"  rows with aliases (renames) : {aliased}")
    print(f"  engine.h protos skipped     : {len(skipped)} (no corpus VA)")
    print(f"  VA collisions (>1 eng name) : {len(collisions)}")
    for cva, keep, other, m in collisions:
        print(f"      {cva}: kept '{keep}', alias '{other}' (via {m})")
    if warnings:
        print(f"  resolver warnings           : {len(warnings)}")
        for w in warnings:
            print(f"      {w}")


if __name__ == "__main__":
    main()
