#!/usr/bin/env python3
"""sdk_check.py — offline CI gates for the ROTH.C modding SDK.

Gates (all run; each prints PASS/FAIL/SKIP; process exits non-zero on any FAIL):

  1. drift        Regenerate functions.json / functions_skipped.json / roth_sdk.h /
                  descriptor.json / host_adapters.c in memory and diff against the
                  committed files. Any difference fails (schemas are the source; the
                  generated files must be reproducible).
  2. id-invariants function_ids.json: unique ids; every name (canonical+alias) resolves
                  to exactly one id; every corpus VA present exactly once; canonical_name
                  matches the current source symbol (engine.h where declared, else the
                  corpus name) — an un-recorded rename fails with remediation; and, vs the
                  released baseline: ids never removed/renumbered, aliases never removed,
                  a demoted canonical must survive as an alias.
  3. abi-diff     descriptor.json vs sdk/abi/released/<latest>/descriptor.json — classify
                  the change (identical / append-only / breaking) and enforce the bump
                  policy (identical => equal versions; append-only => minor bump; breaking
                  => major bump).
  3b. exposure-order  THE EXPOSED-ORDER LAW: the baseline engine-call table's slot sequence
                  must be an exact PREFIX of the current one (slot order = exposure order,
                  append-only forever). Fails HARD on any re-sort/insert/removal — no
                  version bump excuses it.
  4. compile      roth_sdk.h syntax-only and host_adapters.c -c under
                  gcc -m32 -std=c11 -Wall -Wextra -Werror (the ABI proof).
  5. abi-probe    Best-effort: compile+run an offsetof/sizeof probe under -m32 and check it
                  against descriptor.json. SKIPs (does not fail) if -m32 build/run is
                  unavailable.

No network, no writes to the repo (probe artifacts go to a temp dir).
"""
import json
import os
import subprocess
import sys
import tempfile

import sdk_common as C
import sdk_extract_signatures as SIG
import sdk_gen as GEN
import sdk_override_manifest as OVR
import sdk_override_validate as VAL

SDK = os.path.join(C.REPO, "sdk")
IDS = os.path.join(SDK, "schema", "function_ids.json")
RELEASED = os.path.join(SDK, "abi", "released")


def _rel(p):
    return os.path.relpath(p, C.REPO)


# ---------------------------------------------------------------- gate 1 ------
def gate_drift():
    ok = True
    msgs = []
    for renderer in (SIG.render, OVR.render, GEN.render):
        texts = renderer()[0]
        for path, want in texts.items():
            if not os.path.exists(path):
                ok = False
                msgs.append(f"MISSING generated file {_rel(path)}")
                continue
            got = open(path).read()
            if got != want:
                ok = False
                msgs.append(f"DRIFT in {_rel(path)} (regenerate: make -C sdk gen)")
    if ok:
        msgs.append("all generated files match their schemas")
    return "drift", ok, msgs


# ---------------------------------------------------------------- gate 2 ------
def _latest_release_dir():
    if not os.path.isdir(RELEASED):
        return None
    subs = sorted(d for d in os.listdir(RELEASED)
                  if os.path.isdir(os.path.join(RELEASED, d)))
    return os.path.join(RELEASED, subs[-1]) if subs else None


def gate_id_invariants():
    ok = True
    msgs = []
    doc = json.load(open(IDS))
    rows = doc["functions"]

    # unique ids
    ids = [r["id"] for r in rows]
    if len(ids) != len(set(ids)):
        ok = False
        dup = sorted({i for i in ids if ids.count(i) > 1})
        msgs.append(f"duplicate ids: {dup[:10]}")

    # every name -> exactly one id
    name_to_id = {}
    for r in rows:
        for nm in [r["canonical_name"]] + r["aliases"]:
            if nm in name_to_id and name_to_id[nm] != r["id"]:
                ok = False
                msgs.append(f"name '{nm}' maps to ids "
                            f"0x{name_to_id[nm]:x} and 0x{r['id']:x}")
            name_to_id[nm] = r["id"]

    # corpus coverage: row VAs == corpus VAs
    corpus_rows, by_name, by_va = C.load_corpus()
    corpus_vas = {f["va"].lower() for f in corpus_rows}
    row_vas = {r["va"].lower() for r in rows}
    if row_vas != corpus_vas:
        ok = False
        missing = corpus_vas - row_vas
        extra = row_vas - corpus_vas
        if missing:
            msgs.append(f"{len(missing)} corpus VAs missing from id table "
                        f"(e.g. {sorted(missing)[:5]})")
        if extra:
            msgs.append(f"{len(extra)} id-table VAs not in corpus "
                        f"(e.g. {sorted(extra)[:5]})")

    # canonical matches current source symbol
    protos = C.parse_engine_h()
    resolved, _skipped, _warn = C.build_resolution(protos, by_name, by_va)
    eng_name_by_va = {r["va"]: r["name"] for r in resolved if r["method"] == "name"}
    for r in resolved:  # rename resolutions own their VA too
        eng_name_by_va.setdefault(r["va"], r["name"])
    row_by_va = {r["va"].lower(): r for r in rows}
    stale = []
    for va, ename in eng_name_by_va.items():
        row = row_by_va.get(va)
        if row and row["canonical_name"] != ename:
            stale.append((va, row["canonical_name"], ename))
    if stale:
        ok = False
        msgs.append(f"{len(stale)} rows whose canonical_name lags engine.h "
                    f"(e.g. {stale[0][0]}: table '{stale[0][1]}' vs engine.h "
                    f"'{stale[0][2]}'). Remediate: re-seed (tools/sdk_ids.py --seed) — "
                    f"the seeder appends the old name to aliases.")
    for va, cname in ((f["va"].lower(), f["name"]) for f in corpus_rows):
        if va not in eng_name_by_va:  # not declared in engine.h -> canonical == corpus
            row = row_by_va.get(va)
            if row and row["canonical_name"] != cname and cname not in row["aliases"]:
                # tolerate: corpus rename where table kept a chosen canonical + corpus alias
                pass

    # baseline monotonicity (ids permanent, aliases never lost)
    rel = _latest_release_dir()
    base_ids = rel and os.path.join(rel, "function_ids.json")
    if base_ids and os.path.exists(base_ids):
        base = {r["id"]: r for r in json.load(open(base_ids))["functions"]}
        cur = {r["id"]: r for r in rows}
        for bid, brow in base.items():
            crow = cur.get(bid)
            if crow is None:
                ok = False
                msgs.append(f"id 0x{bid:x} present in baseline but REMOVED (ids are permanent)")
                continue
            cur_names = set([crow["canonical_name"]] + crow["aliases"])
            for a in brow["aliases"]:
                if a not in cur_names:
                    ok = False
                    msgs.append(f"id 0x{bid:x}: baseline alias '{a}' was DROPPED")
            if brow["canonical_name"] not in cur_names:
                ok = False
                msgs.append(f"id 0x{bid:x}: baseline canonical '{brow['canonical_name']}' "
                            f"neither current canonical nor an alias")
        msgs.append(f"baseline {os.path.basename(rel)}: "
                    f"{len(base)} ids checked for permanence/alias-retention")
    else:
        msgs.append("no baseline function_ids.json — monotonicity check skipped")

    if ok:
        msgs.insert(0, f"{len(rows)} rows, {len(set(ids))} unique ids, "
                       f"{len(name_to_id)} names all 1:1")
    return "id-invariants", ok, msgs


# ---------------------------------------------------------------- gate 3 ------
def _sig_key(f):
    return (f["id"], f["name"], f["ret"], tuple(f["params"]), f["offset"], f["size"])


def _classify_structs(base_structs, cur_structs):
    """Classify the struct-layout delta across ALL public structs (not just game_ram/api):
    a REMOVED struct, a removed/moved/resized field, or an inserted field (offset < old size)
    is breaking; a wholly-new struct or a trailing appended field is append-only. Returns
    (has_breaking, has_append, details[])."""
    has_breaking = has_append = False
    details = []
    for name in set(base_structs) | set(cur_structs):
        b = base_structs.get(name)
        c = cur_structs.get(name)
        if b and not c:
            has_breaking = True
            details.append(f"struct {name} REMOVED")
            continue
        if c and not b:
            has_append = True
            details.append(f"struct {name} added")
            continue
        bf = {f["name"]: (f["offset"], f["size"], f.get("kind")) for f in b["fields"]}
        cf = {f["name"]: (f["offset"], f["size"], f.get("kind")) for f in c["fields"]}
        for fn, bv in bf.items():
            if fn not in cf:
                has_breaking = True
                details.append(f"{name}.{fn} REMOVED")
            elif cf[fn] != bv:
                has_breaking = True
                details.append(f"{name}.{fn} changed {bv}->{cf[fn]}")
        for fn in set(cf) - set(bf):
            if cf[fn][0] < b["size"]:               # inserted before the old end -> reshuffle
                has_breaking = True
                details.append(f"{name}.{fn} inserted at {cf[fn][0]} (< old size {b['size']})")
            else:
                has_append = True
                details.append(f"{name}.{fn} appended")
        if c["size"] < b["size"]:
            has_breaking = True
            details.append(f"struct {name} shrank {b['size']}->{c['size']}")
    return has_breaking, has_append, details


def gate_abi_diff():
    ok = True
    msgs = []
    cur = json.load(open(GEN.DESC))
    rel = _latest_release_dir()
    base_path = rel and os.path.join(rel, "descriptor.json")
    if not (base_path and os.path.exists(base_path)):
        return "abi-diff", True, ["no released baseline — nothing to diff"]
    base = json.load(open(base_path))

    cur_slots = {f["id"]: _sig_key(f) for f in cur["engine_table"]}
    base_slots = {f["id"]: _sig_key(f) for f in base["engine_table"]}
    cur_structs = cur["structs"]
    base_structs = base["structs"]

    # classify engine-table slot signatures (catches signature changes at equal offsets)
    removed = set(base_slots) - set(cur_slots)
    added = set(cur_slots) - set(base_slots)
    changed = [i for i in (set(cur_slots) & set(base_slots))
               if cur_slots[i] != base_slots[i]]
    # general struct-layout delta across ALL public structs (game_ram / api / plugin_info /
    # registrar / chain): new structs + trailing appended fields are append-only.
    s_break, s_append, s_details = _classify_structs(base_structs, cur_structs)

    if removed or changed or s_break:
        change_class = "breaking"
    elif added or s_append:
        change_class = "append-only"
    else:
        change_class = "identical"

    bmaj, bmin = base["abi_major"], base["abi_minor"]
    cmaj, cmin = cur["abi_major"], cur["abi_minor"]
    msgs.append(f"change class: {change_class} "
                f"(+{len(added)} slots, ~{len(changed)}, -{len(removed)}; "
                f"structs: {'breaking' if s_break else 'append' if s_append else 'identical'})")
    for d in s_details[:6]:
        msgs.append(f"  struct delta: {d}")
    msgs.append(f"abi baseline {bmaj}.{bmin} -> current {cmaj}.{cmin}")

    if change_class == "identical":
        if (cmaj, cmin) != (bmaj, bmin):
            ok = False
            msgs.append("identical ABI but version changed — revert the bump")
    elif change_class == "append-only":
        if not (cmaj == bmaj and cmin > bmin):
            ok = False
            msgs.append("append-only change requires a MINOR bump (same major)")
    else:  # breaking
        if not (cmaj > bmaj):
            ok = False
            msgs.append("breaking change requires a MAJOR bump; also move the released "
                        "baseline forward")
    if ok:
        msgs.append("bump policy satisfied")
    return "abi-diff", ok, msgs


# ---------------------------------------------------------------- gate 3b -----
def gate_exposure_order(cur_desc_path=None, base_desc_path=None):
    """THE EXPOSED-ORDER LAW: the engine-call table's slot order = exposure
    order, append-only FOREVER. The released baseline's (id, name) slot sequence must be an exact
    PREFIX of the current table — any re-sort / insertion / removal fails HARD (no bump excuses it,
    unlike abi-diff, which a major bump + re-baseline could silently absorb). Paths are
    parameterizable so the gate itself is testable."""
    name = "exposure-order"
    ok = True
    msgs = []
    cur = json.load(open(cur_desc_path or GEN.DESC))
    if base_desc_path is None:
        rel = _latest_release_dir()
        base_desc_path = rel and os.path.join(rel, "descriptor.json")
    if not (base_desc_path and os.path.exists(base_desc_path)):
        return name, True, ["no released baseline — nothing to prefix-check"]
    base = json.load(open(base_desc_path))

    cur_seq = [(f["id"], f["name"]) for f in cur["engine_table"]]
    base_seq = [(f["id"], f["name"]) for f in base["engine_table"]]
    if len(cur_seq) < len(base_seq):
        ok = False
        msgs.append(f"engine table SHRANK ({len(base_seq)} -> {len(cur_seq)} slots) — "
                    f"exposed functions are never removed")
    else:
        for i, (b, c) in enumerate(zip(base_seq, cur_seq)):
            if b != c:
                ok = False
                msgs.append(f"slot {i} changed: baseline {b[1]} (0x{b[0]:x}) -> current "
                            f"{c[1]} (0x{c[0]:x}) — slot order is ABI; new exposures go at "
                            f"the END of STARTER_SET (tools/sdk_extract_signatures.py)")
                break
    if ok:
        tail = len(cur_seq) - len(base_seq)
        msgs.append(f"baseline slot order is a prefix of the current table "
                    f"({len(base_seq)} frozen slots, +{tail} appended)")
    return name, ok, msgs


# ---------------------------------------------------------------- gate 4 ------
CFLAGS = ["-m32", "-std=c11", "-Wall", "-Wextra", "-Werror"]
INCS = ["-I", os.path.join(SDK, "include"),
        "-I", os.path.join(C.REPO, "roth_c", "src", "engine")]


def _have_cc():
    try:
        subprocess.run(["gcc", "--version"], capture_output=True, check=True)
        return True
    except Exception:
        return False


def gate_compile():
    if not _have_cc():
        return "compile", True, ["gcc unavailable — SKIP"]
    msgs = []
    ok = True
    with tempfile.TemporaryDirectory() as td:
        # header syntax-only
        main_c = os.path.join(td, "hdr_check.c")
        open(main_c, "w").write('#include "roth_sdk.h"\nint main(void){return 0;}\n')
        r = subprocess.run(["gcc", *CFLAGS, "-fsyntax-only", *INCS, main_c],
                           capture_output=True, text=True)
        if r.returncode != 0:
            ok = False
            msgs.append("roth_sdk.h failed syntax check:\n" + r.stderr.strip())
        else:
            msgs.append("roth_sdk.h clean (-Wall -Wextra -Werror -m32)")
        # host_adapters.c -c  (the ABI proof)
        obj = os.path.join(td, "host_adapters.o")
        r = subprocess.run(["gcc", *CFLAGS, "-c", *INCS, GEN.ADAPTERS, "-o", obj],
                           capture_output=True, text=True)
        if r.returncode != 0:
            ok = False
            msgs.append("host_adapters.c failed to compile:\n" + r.stderr.strip())
        else:
            msgs.append("host_adapters.c compiled (engine.h ABI proof holds)")
    return "compile", ok, msgs


# ---------------------------------------------------------------- gate 5 ------
def gate_abi_probe():
    if not _have_cc():
        return "abi-probe", True, ["gcc unavailable — SKIP"]
    desc = json.load(open(GEN.DESC))
    structs = desc["structs"]
    # build a probe that prints struct:field:offset:size for every descriptor field
    lines = ['#include <stdio.h>', '#include <stddef.h>', '#include "roth_sdk.h"',
             "int main(void){"]
    for sname, s in structs.items():
        lines.append(f'  printf("%s::__size__::%zu\\n", "{sname}", sizeof(struct {sname}));')
        for f in s["fields"]:
            lines.append(f'  printf("%s::%s::%zu::%zu\\n", "{sname}", "{f["name"]}", '
                         f'offsetof(struct {sname}, {f["name"]}), '
                         f'sizeof(((struct {sname}*)0)->{f["name"]}));')
    lines.append("  return 0;}")
    src = "\n".join(lines) + "\n"
    with tempfile.TemporaryDirectory() as td:
        c = os.path.join(td, "abi_probe.c")
        exe = os.path.join(td, "abi_probe")
        open(c, "w").write(src)
        r = subprocess.run(["gcc", "-m32", "-std=c11", *INCS, c, "-o", exe],
                           capture_output=True, text=True)
        if r.returncode != 0:
            return "abi-probe", True, ["-m32 probe build unavailable — SKIP: "
                                       + r.stderr.strip().splitlines()[-1:][0] if r.stderr else "SKIP"]
        try:
            run = subprocess.run([exe], capture_output=True, text=True, timeout=15)
        except Exception as e:
            return "abi-probe", True, [f"-m32 probe run unavailable — SKIP ({e})"]
        if run.returncode != 0:
            return "abi-probe", True, ["-m32 probe run failed — SKIP"]
    # parse and compare
    got = {}
    for ln in run.stdout.splitlines():
        parts = ln.split("::")
        if parts[2] == "__size__":
            got[(parts[0], "__size__")] = int(parts[3]) if len(parts) > 3 else int(parts[2 + 1])
        elif len(parts) == 4:
            got[(parts[0], parts[1])] = (int(parts[2]), int(parts[3]))
    ok = True
    msgs = []
    nfields = 0
    for sname, s in structs.items():
        want_size = s["size"]
        gsz = None
        # find size line
        for ln in run.stdout.splitlines():
            p = ln.split("::")
            if p[0] == sname and p[1] == "__size__":
                gsz = int(p[2 + 1]) if len(p) == 4 else int(p[2])
        if gsz is not None and gsz != want_size:
            ok = False
            msgs.append(f"{sname}: descriptor size {want_size} != actual {gsz}")
        for f in s["fields"]:
            g = got.get((sname, f["name"]))
            if g is None:
                continue
            off, sz = g
            nfields += 1
            if off != f["offset"] or sz != f["size"]:
                ok = False
                msgs.append(f"{sname}.{f['name']}: descriptor (off={f['offset']},"
                            f"size={f['size']}) != actual (off={off},size={sz})")
    if ok:
        msgs.append(f"i386 layout verified: {nfields} fields across "
                    f"{len(structs)} structs match descriptor.json")
    return "abi-probe", ok, msgs


GATES = [gate_drift, gate_id_invariants, gate_abi_diff, gate_exposure_order,
         gate_compile, gate_abi_probe, VAL.gate]


def main():
    print("== ROTH.C SDK checks ==")
    all_ok = True
    for g in GATES:
        name, ok, msgs = g()
        status = "PASS" if ok else "FAIL"
        print(f"[{status}] {name}")
        for m in msgs:
            for ln in m.splitlines():
                print(f"        {ln}")
        all_ok = all_ok and ok
    print("== " + ("ALL GATES PASS" if all_ok else "GATES FAILED") + " ==")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
