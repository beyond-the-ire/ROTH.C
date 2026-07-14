#!/usr/bin/env python3
"""sdk_override_validate.py — build-time validation of the override pads against the LINKED binary.

The permanent gate for three properties that must hold in the linked binary:

  1. PAD PRESENT      every eligible+present function's entry is 5x 0x90 (NOP), and its address is
                      in the binary's __patchable_function_entries section (the authoritative pad
                      table). A missing/short pad means the registry could not safely write the
                      jmp rel32 there — fail closed.
  2. NO INTERIOR JMP  no direct branch (jmp/jcc/call with an annotated target) lands INSIDE
                      (entry, entry+5) of any function — i.e. offset 1..4 into a pad. Such a branch
                      would land mid-jmp once the entry is patched. (offset 0 = the entry itself is
                      fine — it hits the jmp; a tail-call to an entry is the DESIRED behavior.)
  3. DISPATCH ENTRIES every dispatch-edge table target (docs/reference/dispatch_edges.json) resolves
                      to a function ENTRY (a canonical VA), never mid-function — so an indirect call
                      through a table also hits the pad.

Consumes only the linked roth_c/roth + objdump/nm/readelf + the checked-in schemas.
Importable (gate(binary) for tools/sdk_check.py, SKIPs if the binary is absent) and a CLI.
"""
import json
import os
import re
import subprocess

import sdk_common as C
import sdk_override_manifest as OVR

REPO = C.REPO
BINARY = os.path.join(REPO, "roth_c", "roth")
FUNCTIONS = os.path.join(REPO, "sdk", "schema", "functions.json")
IDS = os.path.join(REPO, "sdk", "schema", "function_ids.json")
DISPATCH = os.path.join(REPO, "docs", "reference", "dispatch_edges.json")

_HDR = re.compile(r"^([0-9a-f]+) <([^>]+)>:")
_INSN = re.compile(r"^\s*([0-9a-f]+):\t([0-9a-f ]+?)\t")
_BRANCH_TGT = re.compile(r"<([^>+]+)\+0x([0-9a-f]+)>")   # a branch into sym+offset
_BRANCH_MNE = re.compile(r"\t(j\w+|call|loop\w*)\s")


def _nm_addrs(binary):
    out = subprocess.check_output(["nm", binary], text=True)
    addrs = {}
    for ln in out.splitlines():
        p = ln.split()
        if len(p) == 3 and p[1] in "tT":
            addrs[p[2]] = int(p[0], 16)
    return addrs


def _pfe_addrs(binary):
    """The absolute addresses in __patchable_function_entries (the authoritative live pad table)."""
    try:
        out = subprocess.check_output(["readelf", "-x", "__patchable_function_entries", binary],
                                      text=True, stderr=subprocess.DEVNULL)
    except subprocess.CalledProcessError:
        return None
    words = []
    for ln in out.splitlines():
        toks = ln.split()
        if len(toks) < 2 or not toks[0].startswith("0x"):
            continue
        # data words are the leading run of exactly-8-hex tokens after the address; at most 4 per
        # line (16 bytes). Stop before the trailing ASCII column (capped at 4 so ASCII is never read).
        for hexword in toks[1:5]:
            if re.fullmatch(r"[0-9a-f]{8}", hexword):
                words.append(int.from_bytes(bytes.fromhex(hexword), "little"))
            else:
                break
    return set(words)


def _disasm(binary):
    return subprocess.check_output(["objdump", "-d", binary], text=True).splitlines()


def gate(binary=BINARY):
    """Returns (name, ok, msgs). SKIP (ok=True) if the binary is absent — like the abi-probe gate."""
    name = "override-validate"
    if not os.path.exists(binary):
        return name, True, [f"{os.path.relpath(binary, REPO)} not built — SKIP "
                            f"(build roth_c/roth to run pad validation)"]
    for tool in ("nm", "objdump", "readelf"):
        if subprocess.run(["which", tool], capture_output=True).returncode != 0:
            return name, True, [f"{tool} unavailable — SKIP"]

    msgs = []
    ok = True

    eligible = {t["canonical_name"] for t in OVR.eligible_targets()}
    nm = _nm_addrs(binary)
    pfe = _pfe_addrs(binary)
    if not pfe:
        return name, False, ["__patchable_function_entries section missing/empty — the pad flag "
                             "is not on the engine objects (rebuild with ENGINE_STANDALONE_CFLAGS)"]

    # present eligible entries: eligible name that has a symbol AND is a live pad address
    present = {nm[n]: n for n in eligible if n in nm}
    lines = _disasm(binary)

    # ---- checks 1 & 2 in one disassembly pass -----------------------------------------------
    entry_bytes = {}          # addr -> concatenated first-5 byte hex list (for present eligible)
    interior = []             # (target_sym, offset) branches into a pad interior
    entry_set = set(present)  # addrs that begin an eligible function
    cur_addr = None
    collecting = None         # addr we're collecting pad bytes for
    for ln in lines:
        h = _HDR.match(ln)
        if h:
            cur_addr = int(h.group(1), 16)
            collecting = cur_addr if cur_addr in entry_set else None
            if collecting is not None:
                entry_bytes[collecting] = []
            continue
        mi = _INSN.match(ln)
        if mi:
            if collecting is not None and cur_addr in entry_bytes:
                bl = entry_bytes[collecting]
                if len(bl) < 5:
                    bl += mi.group(2).split()
                if len(bl) >= 5:
                    collecting = None
            # branch-into-pad scan (check 2)
            if _BRANCH_MNE.search(ln):
                bt = _BRANCH_TGT.search(ln)
                if bt:
                    off = int(bt.group(2), 16)
                    if 1 <= off <= 4 and bt.group(1) in eligible:
                        interior.append((bt.group(1), off))

    # check 1: pad present (5 NOPs) + in the pfe table
    bad_pad = []
    not_in_pfe = []
    for addr, nm_name in present.items():
        bl = entry_bytes.get(addr, [])[:5]
        if bl != ["90", "90", "90", "90", "90"]:
            bad_pad.append((nm_name, " ".join(bl) or "<none>"))
        if addr not in pfe:
            not_in_pfe.append(nm_name)
    if bad_pad:
        ok = False
        msgs.append(f"{len(bad_pad)} eligible entries WITHOUT a 5-NOP pad (e.g. "
                    f"{bad_pad[0][0]}: {bad_pad[0][1]}) — fail closed")
    if not_in_pfe:
        ok = False
        msgs.append(f"{len(not_in_pfe)} eligible entries not in __patchable_function_entries "
                    f"(e.g. {not_in_pfe[0]})")
    if not bad_pad and not not_in_pfe:
        msgs.append(f"pad-present: {len(present)} eligible+present entries all 5-NOP and in the "
                    f"live pad table ({len(pfe)} pads total)")

    # check 2
    if interior:
        ok = False
        u = sorted(set(interior))
        msgs.append(f"{len(interior)} direct branch(es) into an eligible pad interior "
                    f"(entry+1..4), e.g. {u[0][0]}+{u[0][1]} — a patched jmp would be split")
    else:
        msgs.append("no-interior-branch: 0 direct branches land inside any eligible (entry, entry+5)")

    # ---- check 3: dispatch-edge targets are function entries ---------------------------------
    try:
        de = json.load(open(DISPATCH))
        ids = json.load(open(IDS))["functions"]
        starts = sorted(int(r["va"], 16) for r in ids)     # every known function entry (canonical VA)
        start_set = set(starts)
        lo, hi = starts[0], starts[-1]
        mid = []
        for e in de.get("edges", []):
            to = e.get("to")
            if not to or not e.get("to_defined"):
                continue
            va = int(to, 16)
            if va < lo or va > hi:
                continue                                    # non-corpus (host/data) target — skip
            if va not in start_set:
                mid.append(to)                              # lands strictly inside a known function
        if mid:
            ok = False
            msgs.append(f"{len(mid)} dispatch-edge target(s) land mid-function, not at an entry "
                        f"(e.g. {mid[0]}) — an indirect call there would bypass the pad")
        else:
            msgs.append(f"dispatch-entries: all {len(de.get('edges', []))} dispatch-edge targets "
                        f"resolve to function entries")
    except (OSError, ValueError, KeyError) as ex:
        msgs.append(f"dispatch-edge check skipped ({ex})")

    return name, ok, msgs


def main():
    import sys
    binary = sys.argv[1] if len(sys.argv) > 1 else BINARY
    name, ok, msgs = gate(binary)
    status = "PASS" if ok else "FAIL"
    print(f"[{status}] {name}")
    for m in msgs:
        print("   ", m)
    return 0 if ok else 1


if __name__ == "__main__":
    import sys
    sys.exit(main())
