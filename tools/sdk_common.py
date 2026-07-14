#!/usr/bin/env python3
"""sdk_common.py — shared machinery for the ROTH.C modding SDK generators.

This module is the single, mechanical bridge between two upstream sources of truth:

  * docs/reference/function_classification.json  — the CANONICAL-VA corpus: every
    named 1996-binary function with its unique canonical VA, subsystem and klass.
    This is the identity source: id == canonical VA (verified 1:1 here).
  * roth_c/src/engine/engine.h                   — the lifted engine surface the SDK
    wraps: ordinary i386 C prototypes (zero register attributes; original register
    convention survives only as parameter ORDER). This is the signature source.

Nothing here writes files; the generators (sdk_ids / sdk_extract_signatures / sdk_gen /
sdk_check) import these helpers so the parse/resolve logic lives in exactly one place.

Resolver (engine.h prototype -> canonical VA), priority order, each step SAFE:
  1. exact name match in the corpus            (names are unique in the corpus)
  2. trailing same-line `/* ... 0xVA ... */` first hex that IS a corpus VA
  3. name hex-suffix `_<hex>` that IS a corpus VA (project naming convention)
A prototype that resolves by (2) or (3) is a RENAME: the engine.h name is the current
canonical name, the corpus name at that VA becomes an alias. Prototypes that resolve by
none of these are host-harness helpers or standalone-only alt-entries with no corpus VA;
they are reported, never guessed.
"""
import json
import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLASSIFICATION = os.path.join(REPO, "docs", "reference", "function_classification.json")
ENGINE_H = os.path.join(REPO, "roth_c", "src", "engine", "engine.h")


# ------------------------------------------------------------------ corpus ----
def load_corpus(path=CLASSIFICATION):
    """Return (rows, by_name, by_va). Each row is the raw classification dict."""
    doc = json.load(open(path))
    rows = doc["functions"]
    by_name = {}
    by_va = {}
    for f in rows:
        by_name[f["name"]] = f
        by_va[f["va"].lower()] = f
    return rows, by_name, by_va


# ------------------------------------------------------------- engine.h parse -
def _tokenize(raw):
    """Split into (kind, start, end) tokens: 'comment' (/* */) and 'code'."""
    toks = []
    i, n = 0, len(raw)
    while i < n:
        if raw[i:i + 2] == "/*":
            j = raw.find("*/", i + 2)
            j = n if j < 0 else j + 2
            toks.append(("comment", i, j))
            i = j
        else:
            j = raw.find("/*", i)
            j = n if j < 0 else j
            toks.append(("code", i, j))
            i = j
    return toks


def _split_params(param_str):
    """Split a parameter list on top-level commas; return [{type,name}, ...].
    'void'/'' -> [] (no params)."""
    s = " ".join(param_str.split())
    if s == "" or s == "void":
        return []
    parts = []
    depth = 0
    cur = ""
    for ch in s:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur)
    out = []
    for p in parts:
        p = p.strip()
        # trailing identifier that follows a type token (space or '*') is the name
        m = re.match(r"^(?P<type>.*[\s\*])(?P<name>[A-Za-z_]\w*)$", p)
        if m and m.group("type").strip():
            out.append({"type": " ".join(m.group("type").split()),
                        "name": m.group("name")})
        else:
            out.append({"type": " ".join(p.split()), "name": ""})
    return out


_FUNC_RE = re.compile(
    r"^(?P<ret>[A-Za-z_][\w\s\*]*?)\b(?P<name>[A-Za-z_]\w*)\s*\((?P<params>.*)\)\s*$",
    re.S)
_CANON_HEX_RE = re.compile(r"0x[0-9a-fA-F]+")
_SUFFIX_RE = re.compile(r"_(?P<hex>[0-9a-fA-F]{4,6})$")


def parse_engine_h(path=ENGINE_H):
    """Parse every function prototype. Return list of dicts:
       {name, ret, params:[{type,name}], line, trailing_hex:[...] }.
    Non-prototype statements (typedefs, extern vars, fn-ptr vars) are skipped."""
    raw = open(path).read()
    # blank out preprocessor lines (keep newlines so line numbers stay stable)
    raw = re.sub(r"(?m)^[ \t]*#.*$", "", raw)
    toks = _tokenize(raw)
    comments = [(s, e, raw.count("\n", 0, s) + 1, raw[s:e])
                for (k, s, e) in toks if k == "comment"]

    # locate depth-0 ';' offsets among code tokens (braces => struct bodies)
    semis = []
    depth = 0
    for k, s, e in toks:
        if k != "code":
            continue
        for p in range(s, e):
            c = raw[p]
            if c == "{":
                depth += 1
            elif c == "}":
                depth = max(0, depth - 1)
            elif c == ";" and depth == 0:
                semis.append(p)

    protos = []
    prev = 0
    for off in semis:
        seg = raw[prev:off]
        seg_start = prev
        prev = off + 1
        code = " ".join(re.sub(r"/\*.*?\*/", " ", seg, flags=re.S).split())
        if not code or code.startswith("typedef"):
            continue
        if code.startswith("extern") and "(*" in code:
            continue
        if "(" not in code:
            continue
        if re.search(r"\(\s*\*\s*[A-Za-z_]\w*\s*\)\s*\(", code):
            continue  # function-pointer variable
        m = _FUNC_RE.match(code)
        if not m:
            continue
        line = raw.count("\n", 0, seg_start) + 1
        # trailing comment = comment starting on the same source line as this ';'
        semi_line = raw.count("\n", 0, off) + 1
        trailing = [t for (s, e, cl, t) in comments if s >= off and cl == semi_line]
        trailing_hex = []
        for t in trailing:
            trailing_hex += [h.lower() for h in _CANON_HEX_RE.findall(t)]
        protos.append({
            "name": m.group("name"),
            "ret": " ".join(m.group("ret").split()),
            "params": _split_params(m.group("params")),
            "line": line,
            "trailing_hex": trailing_hex,
        })
    return protos


# ------------------------------------------------------------------ resolver --
def _suffix_va(name, by_va):
    m = _SUFFIX_RE.search(name)
    if m:
        cand = "0x" + m.group("hex").lower()
        if cand in by_va:
            return cand
    return None


def build_resolution(protos, by_name, by_va):
    """Resolve every engine.h prototype to a canonical VA/id in TWO passes.

    Pass 1 — exact name matches (names are unique in the corpus). These OWN their VA.
    Pass 2 — residual prototypes resolve by (a) a trailing same-line `/* 0xVA */` hex
             that is a corpus VA, else (b) a `_<hex>` name suffix that is a corpus VA.
             A pass-2 match is REJECTED if the target VA is already owned by a pass-1
             (exact-name) prototype: that means this prototype is a DIFFERENT symbol
             (an ABI-register wrapper, a mid-entry, a dispatch shim) whose comment
             merely REFERENCES the corpus function it wraps — it is not another name
             for that VA, so it must not be aliased onto it.

    Returns (resolved, skipped, warnings):
       resolved: proto fields + {va, id, method, corpus_name}
       skipped:  {name, ret, reason}
       warnings: strings (e.g. a stale/mismatched trailing hex on a name-matched proto)
    """
    resolved, skipped, warnings = [], [], []
    owned = set()  # VAs claimed by an exact-name match

    # pass 1
    residual = []
    for p in protos:
        if p["name"] in by_name:
            va = by_name[p["name"]]["va"].lower()
            owned.add(va)
            resolved.append({**p, "va": va, "id": int(va, 16),
                             "method": "name", "corpus_name": by_va[va]["name"]})
        else:
            residual.append(p)

    # consistency warnings for pass-1 protos whose trailing hex points elsewhere
    for r in resolved:
        if r["trailing_hex"]:
            mism = [h for h in r["trailing_hex"] if h in by_va and h != r["va"]]
            if mism and r["va"] not in r["trailing_hex"]:
                warnings.append(
                    f"{r['name']} (line {r['line']}): resolved to {r['va']} but trailing "
                    f"comment cites corpus VA {mism[0]} ({by_va[mism[0]]['name']})")

    # pass 2
    for p in residual:
        va = None
        method = None
        for h in p["trailing_hex"]:
            if h in by_va:
                va, method = h, "trailing-hex"
                break
        if va is None:
            s = _suffix_va(p["name"], by_va)
            if s:
                va, method = s, "name-suffix"
        if va is None:
            skipped.append({"name": p["name"], "ret": p["ret"],
                            "reason": "no corpus VA (host-harness or standalone-only alt-entry)"})
            continue
        if va in owned:
            skipped.append({"name": p["name"], "ret": p["ret"],
                            "reason": f"references corpus VA {va} ({by_va[va]['name']}) "
                                      f"already owned by an exact-name match — wrapper/"
                                      f"mid-entry/shim, not another name for that VA"})
            continue
        resolved.append({**p, "va": va, "id": int(va, 16),
                         "method": method, "corpus_name": by_va[va]["name"]})
        owned.add(va)  # a pass-2 rename now owns its VA too (no double-claim)

    return resolved, skipped, warnings


if __name__ == "__main__":
    rows, by_name, by_va = load_corpus()
    protos = parse_engine_h()
    resolved, skipped, warnings = build_resolution(protos, by_name, by_va)
    print(f"corpus functions      : {len(rows)}")
    print(f"engine.h prototypes   : {len(protos)}")
    print(f"resolved to a VA/id   : {len(resolved)}")
    bym = {}
    for r in resolved:
        bym[r["method"]] = bym.get(r["method"], 0) + 1
    print(f"  by method           : {bym}")
    renames = [r for r in resolved if r["method"] != "name" and r["name"] != r["corpus_name"]]
    print(f"  renames (alias adds) : {len(renames)}")
    print(f"skipped (no corpus VA): {len(skipped)}")
    for s in skipped:
        print(f"    SKIP {s['name']}  ({s['reason']})")
    print(f"warnings              : {len(warnings)}")
    for w in warnings:
        print(f"    WARN {w}")
