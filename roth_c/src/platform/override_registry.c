/* override_registry.c — the boot-time function-override registry (task #103, milestone 3).
 *
 * Image-free ONLY. See override_registry.h for the model. The generated build/override_gen.c
 * provides roth_ovr_targets[] (id -> {pad entry, dispatch thunk, runtime slot}); this file collects
 * plugin registrations, builds one immutable priority-sorted chain per id, and patches the pad
 * entries (jmp rel32 -> dispatch thunk) — single-threaded at boot, before any timer/game/audio
 * thread starts, transactional (validate all, then patch all; reverse-unwind on failure).
 */
#include "override_registry.h"
#include "roth_sdk.h"      /* struct roth_registrar_v1 / roth_api_v1 / roth_register_override_fn */
#include "roth_host.h"     /* LOGE */
#include "sys/sys.h"       /* per-OS seam: executable-page protection for the patch window */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PENDING_MAX 1024   /* total override registrations across all plugins */
#define CHAIN_MAX   64     /* links per function id */

typedef struct {
    uint32_t id;
    void    *cb;
    int      priority;
    int      plugin_idx;   /* stable tiebreak (load order) */
} pending_t;

static pending_t g_pending[PENDING_MAX];
static int       g_npending;
static int       g_reg_errors;          /* registrations that named a non-eligible/absent id */
static int       g_cur_plugin = -1;
static const char *g_cur_plugin_id = "?";

static int       g_patched[PENDING_MAX];   /* target indices actually patched (for unwind + dump) */
static int       g_npatched;

/* ----------------------------------------------------------------- target lookup -- */
static int target_index_by_id(uint32_t id)
{
    /* roth_ovr_targets[] is id-sorted (the generator emits it so) -> binary search. */
    int lo = 0, hi = roth_ovr_target_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t v = roth_ovr_targets[mid].id;
        if (v == id) return mid;
        if (v < id) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

/* ----------------------------------------------------------------- registration --- */
static int registry_record(struct roth_registrar_v1 *reg, uint32_t id, void *cb, int priority)
{
    (void)reg;
    if (!cb) {
        LOGE("[overrides] %s: null callback for id 0x%x — rejected\n", g_cur_plugin_id, id);
        g_reg_errors++;
        return 1;
    }
    if (target_index_by_id(id) < 0) {
        LOGE("[overrides] %s: id 0x%x is not an eligible/present override target — rejected\n",
             g_cur_plugin_id, id);
        g_reg_errors++;
        return 1;
    }
    if (g_npending >= PENDING_MAX) {
        LOGE("[overrides] too many registrations (max %d) — id 0x%x dropped\n", PENDING_MAX, id);
        g_reg_errors++;
        return 1;
    }
    g_pending[g_npending++] = (pending_t){ id, cb, priority, g_cur_plugin };
    return 0;
}

static struct roth_registrar_v1 g_registrar = {
    (uint32_t)sizeof(struct roth_registrar_v1),
    (roth_register_override_fn)registry_record,
};

struct roth_registrar_v1 *overrides_registrar(void) { return &g_registrar; }

void overrides_begin_plugin(int plugin_idx, const char *plugin_id)
{
    g_cur_plugin = plugin_idx;
    g_cur_plugin_id = plugin_id ? plugin_id : "?";
}

/* -------------------------------------------------------------------- patching ---- */
/* Overwrite the 5 NOP pad bytes at `entry` with `jmp rel32 -> target`. Re-verifies the pad first
 * (fail closed). Minimal page-granular W^X window (exactly the 1-2 pages the 5-byte write spans). */
static int patch_entry(uint8_t *entry, void *target)
{
    for (int i = 0; i < 5; i++)
        if (entry[i] != 0x90)
            return -1;                        /* not a clean 5-NOP pad — refuse */
    int32_t rel = (int32_t)((intptr_t)target - (intptr_t)(entry + 5));
    uintptr_t lo = (uintptr_t)entry & ~(uintptr_t)0xfff;
    uintptr_t hi = ((uintptr_t)entry + 5 + 0xfff) & ~(uintptr_t)0xfff;
    if (sys_protect_exec((void *)lo, (size_t)(hi - lo), SYS_PROT_RWX) != 0)
        return -2;
    entry[0] = 0xE9;
    memcpy(entry + 1, &rel, 4);
    sys_protect_exec((void *)lo, (size_t)(hi - lo), SYS_PROT_RX);
    return 0;
}

/* Restore the 5 NOP pad (transactional unwind). */
static void unpatch_entry(uint8_t *entry)
{
    uintptr_t lo = (uintptr_t)entry & ~(uintptr_t)0xfff;
    uintptr_t hi = ((uintptr_t)entry + 5 + 0xfff) & ~(uintptr_t)0xfff;
    if (sys_protect_exec((void *)lo, (size_t)(hi - lo), SYS_PROT_RWX) != 0)
        return;
    memset(entry, 0x90, 5);
    sys_protect_exec((void *)lo, (size_t)(hi - lo), SYS_PROT_RX);
}

static void unwind_all(void)
{
    for (int i = 0; i < g_npatched; i++)
        unpatch_entry((uint8_t *)roth_ovr_targets[g_patched[i]].entry);
    g_npatched = 0;
}

/* ---------------------------------------------------------------------- apply ----- */
static int cmp_pending(const void *a, const void *b)
{
    const pending_t *x = a, *y = b;
    if (x->id != y->id) return (x->id < y->id) ? -1 : 1;
    if (x->priority != y->priority) return (x->priority > y->priority) ? -1 : 1; /* higher = outer */
    return x->plugin_idx - y->plugin_idx;                                        /* stable tiebreak */
}

int overrides_apply(int strict, const struct roth_api_v1 *api)
{
    if (strict && g_reg_errors) {
        LOGE("[overrides] --strict-mods: %d bad registration(s) — aborting\n", g_reg_errors);
        exit(2);
    }
    if (g_npending == 0) {
        /* §10.5 purity regression: with no registered override, no entry is ever written. Prove it
         * at runtime — every eligible entry must still be a clean 5-NOP pad (cmp against 0x90*5). */
        int pristine = 0, dirty = 0;
        for (int t = 0; t < roth_ovr_target_count; t++) {
            const uint8_t *e = (const uint8_t *)roth_ovr_targets[t].entry;
            if (e[0] == 0x90 && e[1] == 0x90 && e[2] == 0x90 && e[3] == 0x90 && e[4] == 0x90)
                pristine++;
            else
                dirty++;
        }
        LOGE("[overrides] 0 overrides registered — pads stay NOP (pristine call targets); "
             "runtime cmp: %d/%d entries still 5-NOP, %d written\n",
             pristine, roth_ovr_target_count, dirty);
        return 0;
    }
    qsort(g_pending, (size_t)g_npending, sizeof g_pending[0], cmp_pending);

    /* Phase 1: per distinct id, build the immutable slots[] chain + validate the pad. Nothing is
     * written yet (transactional). */
    int patched = 0;
    int i = 0;
    while (i < g_npending) {
        int j = i;
        while (j < g_npending && g_pending[j].id == g_pending[i].id) j++;
        int n = j - i;                            /* links for this id */
        uint32_t id = g_pending[i].id;
        int ti = target_index_by_id(id);          /* guaranteed >=0 (checked at record time) */
        const struct roth_ovr_target *t = &roth_ovr_targets[ti];

        /* duplicate-priority conflict (finding 13) */
        for (int k = i + 1; k < j; k++) {
            if (g_pending[k].priority == g_pending[k - 1].priority) {
                if (strict) {
                    LOGE("[overrides] --strict-mods: duplicate priority %d on %s (0x%x) "
                         "(plugins #%d,#%d) — aborting\n", g_pending[k].priority, t->name, id,
                         g_pending[k - 1].plugin_idx, g_pending[k].plugin_idx);
                    unwind_all();
                    exit(2);
                }
                LOGE("[overrides] WARN duplicate priority %d on %s (0x%x); ordering by load index\n",
                     g_pending[k].priority, t->name, id);
            }
        }
        if (n > CHAIN_MAX) {
            LOGE("[overrides] %s (0x%x): %d links exceeds max %d — extra dropped\n",
                 t->name, id, n, CHAIN_MAX);
            n = CHAIN_MAX;
        }
        /* validate the pad before committing this id to the patch set (fail closed) */
        uint8_t *entry = (uint8_t *)t->entry;
        int pad_ok = 1;
        for (int b = 0; b < 5; b++) if (entry[b] != 0x90) pad_ok = 0;
        if (!pad_ok) {
            LOGE("[overrides] %s (0x%x): entry lacks a 5-NOP pad — override dropped%s\n",
                 t->name, id, strict ? " (strict: aborting)" : "");
            if (strict) { unwind_all(); exit(2); }
            i = j;
            continue;
        }
        /* build the immutable slots array (session lifetime; never freed) */
        const void **slots = calloc((size_t)n, sizeof(void *));
        if (!slots) {
            LOGE("[overrides] OOM building chain for %s (0x%x)%s\n", t->name, id,
                 strict ? " (strict: aborting)" : "");
            if (strict) { unwind_all(); exit(2); }
            i = j;
            continue;
        }
        for (int k = 0; k < n; k++) slots[k] = g_pending[i + k].cb;
        t->slot->slots = slots;
        t->slot->n = n;
        t->slot->body = (uint8_t *)t->entry + 5;   /* recorded original body (past the jmp) */
        t->slot->api = api;

        /* Phase 2 (per id, but only reached after this id validated): write the jmp. */
        if (patch_entry(entry, t->dispatch) != 0) {
            LOGE("[overrides] %s (0x%x): patch write failed — unwinding%s\n", t->name, id,
                 strict ? " (strict: aborting)" : "");
            unwind_all();
            if (strict) exit(2);
            return patched;   /* fail closed: stop patching further */
        }
        g_patched[g_npatched++] = ti;
        patched++;
        i = j;
    }
    /* §10.5 purity accounting: prove the UNpatched entries are still pristine 5-NOP pads (only the
     * `patched` functions got a jmp written). Report the breakdown so the moddable boot log states it. */
    int still_nop = 0;
    for (int t = 0; t < roth_ovr_target_count; t++) {
        const uint8_t *e = (const uint8_t *)roth_ovr_targets[t].entry;
        if (e[0] == 0x90 && e[1] == 0x90 && e[2] == 0x90 && e[3] == 0x90 && e[4] == 0x90)
            still_nop++;
    }
    LOGE("[overrides] applied %d function override chain(s) from %d registration(s); "
         "%d/%d eligible entries written, %d still 5-NOP (pristine)\n",
         patched, g_npending, patched, roth_ovr_target_count, still_nop);
    return patched;
}

int overrides_patched_count(void) { return g_npatched; }

/* --------------------------------------------------------------- --dump-mod-chains -- */
void overrides_dump_chains(void)
{
    LOGE("[overrides] === resolved override chains (%d) ===\n", g_npatched);
    for (int p = 0; p < g_npatched; p++) {
        const struct roth_ovr_target *t = &roth_ovr_targets[g_patched[p]];
        LOGE("[overrides]   %s (0x%x): %d link(s), base=recorded-body@%p\n",
             t->name, t->id, t->slot->n, t->slot->body);
        for (int k = 0; k < t->slot->n; k++)
            LOGE("[overrides]     [%d] cb=%p (higher priority = outer, runs first)\n",
                 k, t->slot->slots[k]);
    }
    if (g_npatched == 0)
        LOGE("[overrides]   (none — pads pristine)\n");
}
