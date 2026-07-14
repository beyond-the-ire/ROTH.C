/* override_registry.h — the boot-time function-override registry (task #103, milestone 3).
 *
 * Image-free ONLY (linked into the moddable engine binary (roth); the trap host never compiles or links this).
 * MODS_PLATFORM.md §11 (director ruling D1/D3) + recon OVERRIDE_REGISTRY_RECON.md.
 *
 * MODEL. The imgfree engine objects are compiled -fpatchable-function-entry=5,0: five NOP bytes at
 * every function entry. A plugin's on_register_overrides(MAIN) calls roth_override(reg, ROTH_FN_<fn>,
 * cb, prio); the host collects those, builds ONE immutable priority-sorted chain per function id
 * (higher priority = outer), and — for ids with a NON-EMPTY chain only — overwrites the 5 pad bytes
 * with `jmp rel32` to a generated per-function dispatch thunk (roth_ovr_dispatch_<fn>, in the
 * generated build/override_gen.c). The thunk builds an invocation-local chain context and enters
 * roth_next_<fn>; call-next re-enters the recorded original body at entry+5. Unpatched functions keep
 * their NOP pads = the §10.5 purity promise ("no registered plugin => no semantic call-target change").
 * All of this runs single-threaded at boot, BEFORE the timer/game/audio threads start.
 */
#ifndef OVERRIDE_REGISTRY_H
#define OVERRIDE_REGISTRY_H

#include <stdint.h>

struct roth_api_v1;         /* the host API table (roth_sdk.h) */
struct roth_registrar_v1;   /* the registration handle handed to on_register_overrides (roth_sdk.h) */

/* One per-id runtime chain slot. The generated dispatch thunk for a patched id reads its own slot;
 * the registry fills slots only for ids that end up with a non-empty chain. */
struct roth_ovr_slot {
    const void *const *slots;          /* [n] typed roth_override_<fn>_fn callbacks, as void* */
    int n;
    void *body;                        /* recorded original body = winner entry + 5 */
    const struct roth_api_v1 *api;     /* the immutable host API table passed to each callback */
};

/* One per eligible engine function — emitted by the generated build/override_gen.c. `entry` and
 * `dispatch` are symbol references the linker resolves (strip-safe; no ELF self-inspection). */
struct roth_ovr_target {
    uint32_t id;                       /* canonical VA == ROTH_FN_<fn> */
    const char *name;                  /* canonical name (diagnostics) */
    void *entry;                       /* &<fn> == the 5-NOP pad entry to patch (the link winner) */
    void *dispatch;                    /* &roth_ovr_dispatch_<fn> == the jmp target */
    struct roth_ovr_slot *slot;        /* the runtime slot the dispatch thunk reads */
};

/* Provided by the generated build/override_gen.c. */
extern const struct roth_ovr_target roth_ovr_targets[];
extern const int roth_ovr_target_count;

/* --- registration surface (called by plugin_loader.c during on_register_overrides dispatch) --- */
/* The registrar the loader hands each plugin's on_register_overrides (a stable host object whose
 * `override` fn ptr records into the pending list, tagged with the current plugin). */
struct roth_registrar_v1 *overrides_registrar(void);
/* Tag subsequent roth_override() calls with this plugin (index = stable-tiebreak, id = diagnostics). */
void overrides_begin_plugin(int plugin_idx, const char *plugin_id);

/* --- resolve + patch (called by plugin_loader.c after ALL on_register_overrides run) --- */
/* Build the immutable chains and patch the pad entries. strict != 0 (--strict-mods) aborts the
 * process on any conflict/validation failure (fail closed, transactional reverse-unwind); non-strict
 * logs loudly and drops the offending override. `api` is the immutable host API table. Returns the
 * number of patched functions (0 if no overrides registered). */
int overrides_apply(int strict, const struct roth_api_v1 *api);

/* --dump-mod-chains: print every resolved chain (base + priority-ordered links). */
void overrides_dump_chains(void);
/* Number of pad entries actually patched (for the no-plugin regression: must be 0 with no plugins). */
int overrides_patched_count(void);

#endif /* OVERRIDE_REGISTRY_H */
