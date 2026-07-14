#!/usr/bin/env python3
"""sdk_override_manifest.py — the ID-level override-ELIGIBILITY manifest.

Emits the CHECKED-IN, drift-gated artifact:

    sdk/abi/override_targets.json   { id, canonical_name, signature_id, eligible, reason,
                                      context_flags[] }   — NO addresses (they churn per build)

Eligibility is reproducible from checked-in SOURCES ONLY (sdk/schema/functions.json +
the hard-exclude/SMC/opaque-type policy baked here) so the drift gate never needs the
binary. The PER-BUILD address descriptor (which of these are actually present, and where)
is a separate concern: tools/gen_override_dispatch.py emits build/override_gen.c as a table
of symbol references (&fn) that the linker resolves — see that tool.

Policy:
  * eligible  = a typed (resolve=="name") engine function that is NOT hard-excluded and whose
                signature uses only plugin-expressible (stdint/plain-C) types.
  * NOT static TICK_ISR reachability — the standalone host drives the gameplay heartbeat inside
    SIGALRM and the engine call graph is one dense SCC (1020/1109 typed
    functions reachable from the 4 tick roots). So instead of a reachability FILTER, every
    eligible target carries a `tick_isr_reachable` context flag: a documented author contract
    ("an override runs in whatever context the original ran, including SIGALRM preemption; the
    platform does not make it worse"), conservatively asserted for all eligible.
  * HARD-EXCLUDE (the true ISR machinery, ~11): the 4 tick-ISR bodies + update_software_cursor
    + the 6 audio-callback-only functions (the closure of the audio pump seam;
    reproduced by tools/closure over roth and baked below).
  * OPAQUE-TYPE exclude: functions whose signature threads the engine-internal `regs_t`
    (ABI-register shims / int handlers). Keeping them out preserves a pure-stdint plugin ABI
    (cross-compiler/Windows portability). A future SDK may expose an opaque
    register handle.
  * `self_modifying` context flag (advisory): the true-SMC texture mappers that
    pack values into their OWN code bytes — an override author copying such a function must
    preserve the SMC handling. Derived here by scanning renderer.c for the "SMC write-back"
    self-patch marker (reproducible from source).
"""
import json
import os
import re

import sdk_common as C

SDK = os.path.join(C.REPO, "sdk")
FUNCTIONS = os.path.join(SDK, "schema", "functions.json")
OUT = os.path.join(SDK, "abi", "override_targets.json")
RENDERER_C = os.path.join(C.REPO, "roth_c", "src", "engine", "renderer.c")

# Toolchain of record: pad geometry + section semantics are gcc/binutils-version-sensitive.
TOOLCHAIN = {"gcc": "16.1.1", "binutils": "2.46", "pad": "-fpatchable-function-entry=5,0"}

# --- the ~11 ISR-machinery hard-excludes. Keyed by canonical name;
# the builder verifies each resolves to a typed row. The audio-callback set is the closure of the
# audio pump seam (closure(audio_standalone_tick) ∩ typed-engine == exactly these 6),
# reproduced against roth and baked here so the manifest stays source-reproducible. ----
HARD_EXCLUDE = {
    # the 4 tick-ISR bodies (the gameplay + GDV heartbeats driven from SIGALRM / shm_tick)
    "vsync_timer_tick":              "ISR body: gameplay heartbeat, driven from the SIGALRM tick",
    "gdv_decode_timer_isr":          "ISR body: GDV decode timer",
    "gdv_decode_timer_isr_noaudio":  "ISR body: GDV decode timer (no-audio variant)",
    "gdv_tick_timer_isr":            "ISR body: GDV tick timer",
    # the software cursor, redrawn every tick
    "update_software_cursor":        "ISR machinery: software cursor redrawn every tick",
    # the audio-callback-only set (closure of the audio pump seam)
    "sos_user_callback_trampoline":  "audio-callback-only: SOS user callback (audio pump closure)",
    "sos_voice_set_callback":        "audio-callback-only: SOS voice-set callback",
    "voice_stream_sos_callback":     "audio-callback-only: SOS voice-stream callback",
    "decode_midi_varlen":            "audio-callback-only: MIDI varlen decode (audio pump closure)",
    "emit_audio_sequence_event":     "audio-callback-only: audio sequence event emit",
    "parse_music_sequence_tracks":   "audio-callback-only: music sequence track parse",
}

# engine-internal parameter types a plugin header cannot express in pure stdint/plain-C.
OPAQUE_TYPES = {"regs_t"}


def _base_type(t):
    return t.replace("const", "").replace("volatile", "").replace("*", "").strip()


def _uses_opaque(row):
    for p in row["params"]:
        if _base_type(p["type"]) in OPAQUE_TYPES:
            return _base_type(p["type"])
    if _base_type(row["ret"]) in OPAQUE_TYPES:
        return _base_type(row["ret"])
    return None


def smc_self_modifying_names():
    """Renderer functions that self-modify (pack values into their OWN code): the definitive
    'SMC write-back' texture-mapper cluster. Attributed by nearest preceding typed
    function definition in renderer.c. Advisory `self_modifying` flag only."""
    try:
        src = open(RENDERER_C).read().splitlines()
    except OSError:
        return set()
    fdoc = json.load(open(FUNCTIONS))
    typed = {r["name"] for r in fdoc["functions"] if r["resolve"] == "name"}
    deflines = []
    for i, ln in enumerate(src):
        if not ln or ln[0] in " \t/*}#":
            continue
        m = re.match(r"^[A-Za-z_][\w \*]*?\b([a-z_]\w+)\s*\(", ln)
        if m and m.group(1) in typed:
            deflines.append((i, m.group(1)))
    deflines.sort()

    def enclosing(lineno):
        best = None
        for i, nm in deflines:
            if i <= lineno:
                best = nm
            else:
                break
        return best

    smc = set()
    for i, ln in enumerate(src):
        if "SMC write-back" in ln:            # the function writes into its own code bytes
            e = enclosing(i)
            if e:
                smc.add(e)
    return smc


def build():
    fdoc = json.load(open(FUNCTIONS))
    rows = fdoc["functions"]
    by_name = {r["name"]: r for r in rows}
    smc = smc_self_modifying_names()

    # sanity: every hard-exclude name must resolve to a typed row (fail loud if a rename slipped)
    missing = [n for n in HARD_EXCLUDE if n not in by_name or by_name[n]["resolve"] != "name"]
    if missing:
        raise SystemExit("hard-exclude names not present as typed engine functions "
                         "(rename? re-derive the closure): " + ", ".join(missing))

    targets = []
    for r in rows:
        name = r["name"]
        flags = []
        if name in smc:
            flags.append("self_modifying")
        if r["resolve"] != "name":
            eligible, reason = False, f"resolved by {r['resolve']} (not a name-keyed typed target)"
        elif name in HARD_EXCLUDE:
            eligible, reason = False, HARD_EXCLUDE[name]
        else:
            opq = _uses_opaque(r)
            if opq:
                eligible = False
                reason = (f"signature threads engine-internal type '{opq}' (ABI-register shim) "
                          f"— not a stable typed override target in v1")
            else:
                eligible, reason = True, ""
        if eligible:
            flags.insert(0, "tick_isr_reachable")   # conservative author contract (dense SCC)
        targets.append({
            "id": r["id"],
            "canonical_name": name,
            "signature_id": r["id"],   # the permanent id keys the signature (descriptor.json)
            "eligible": eligible,
            "reason": reason,
            "context_flags": flags,
        })
    targets.sort(key=lambda t: t["id"])

    elig = [t for t in targets if t["eligible"]]
    meta = {
        "schema": "roth-sdk/override_targets/v1",
        "note": ("ID-level override-eligibility manifest. Reproducible "
                 "from sdk/schema/functions.json + the baked policy; NO addresses. tick_isr_reachable "
                 "is conservatively true for all eligible (the engine call graph is one dense SCC — "
                 "1020/1109 typed functions are tick-reachable). The per-build address "
                 "descriptor (which are present + where) is build/override_gen.c."),
        "toolchain": TOOLCHAIN,
        "count": len(targets),
        "eligible_count": len(elig),
        "hard_excluded_count": sum(1 for t in targets if not t["eligible"]
                                   and t["canonical_name"] in HARD_EXCLUDE),
        "opaque_type_excluded_count": sum(1 for t in targets if not t["eligible"]
                                          and "engine-internal type" in t["reason"]),
        "untyped_count": sum(1 for t in targets if not t["eligible"]
                             and "not a name-keyed" in t["reason"]),
        "self_modifying_count": sum(1 for t in targets if "self_modifying" in t["context_flags"]),
    }
    return meta, targets


def render():
    """Return ({path: text}, targets) — the single source for the writer and the CI drift check."""
    meta, targets = build()
    text = json.dumps({"meta": meta, "targets": targets}, indent=1) + "\n"
    return {OUT: text}, targets


def eligible_targets():
    """The eligible rows (for tools/sdk_gen.py: it generates typedefs + roth_next_<fn> for these)."""
    _meta, targets = build()
    return [t for t in targets if t["eligible"]]


def main():
    import sys
    texts, _targets = render()
    meta, _ = build()
    if "--write" in sys.argv or "--seed" in sys.argv:
        open(OUT, "w").write(texts[OUT])
        print(f"wrote {OUT}")
        print(f"  {meta['count']} targets, {meta['eligible_count']} eligible, "
              f"{meta['hard_excluded_count']} hard-excluded, "
              f"{meta['opaque_type_excluded_count']} opaque-type-excluded, "
              f"{meta['self_modifying_count']} self-modifying")
    else:
        print(f"[dry-run] {meta['count']} targets, {meta['eligible_count']} eligible "
              f"— pass --write to emit")


if __name__ == "__main__":
    main()
