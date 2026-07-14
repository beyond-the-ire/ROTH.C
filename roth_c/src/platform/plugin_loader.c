/* plugin_loader.c — the image-free plugin loader / lifecycle state machine (task #103).
 *
 * MODS_PLATFORM.md §10.4 (SDK law) + review findings 8/16/18:
 *   - Discovery: <game-dir>/mods/<name>/plugin.so, load order = lexicographic <name>.
 *   - Transactional: dlopen(RTLD_NOW|RTLD_LOCAL) -> resolve the single versioned export
 *     roth_plugin_query_v1 -> validate EVERY candidate (struct_size sanity, ABI load rule, non-empty
 *     reverse-DNS id, duplicate-id) BEFORE any lifecycle callback runs. (dlopen itself runs the
 *     shared object's static constructors — unavoidable; the "no plugin code before validation"
 *     promise covers OUR lifecycle dispatch, not the loader's dlopen.)
 *   - Fail closed: a bad plugin is rejected LOUDLY and the others continue; --strict-mods aborts.
 *     Duplicate reverse-DNS id: first wins, later duplicate rejected.
 *   - No dlclose of an ACCEPTED plugin during the session; handles retained to process exit; on_unload
 *     runs only after roth_main() returns, in reverse load order.
 *   - Kill switches: ROTH_MODS=0 or --no-mods -> the loader is completely inert.
 *
 * This TU also OWNS the two host-side symbols the generated SDK host-adapter table
 * (sdk/generated/host_adapters.c) leaves extern: roth_game_ram_base / roth_game_ram_size — the
 * game_ram window the bounded accessors read/write. Image-free only (never the trap host).
 */
#include "plugin_loader.h"
#include "roth_sdk.h"           /* the plugin-facing contract: roth_plugin_info_v1, roth_api_v1, ... */
#include "override_registry.h"  /* the boot-time function-override registry (task #103) */
#include "roth_host.h"          /* OBJ_DELTA, STACK_TOP, LOGE */
#include "engine.h"             /* regs_t, g_os_soft_int (the int33 seam we chain for on_frame_game) */
#include "standalone_hooks.h"   /* host_soft_int (the real body g_os_soft_int points at) */

#include <dlfcn.h>
#include <dirent.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- game_ram window: canonical VAs index from OBJ_DELTA (canon 0). The host_adapters.c gr_*
 * accessors bound-check off<size. The mapped engine-global regions inside [0,size) are obj1
 * [0x10000,0x60000) and obj3 [0x70000,0xa8170); the sub-0x10000 and obj2 [0x60000,0x70000) sub-ranges
 * carry no named globals and are unmapped image-free (a plugin read there faults — fail-process-wide,
 * §10.6). Reads past the top return 0, writes past drop, block ops -1 (schema contract). ------------ */
uint8_t  *roth_game_ram_base;
uint32_t  roth_game_ram_size;

extern const struct roth_api_v1 roth_host_api_v1;   /* the immutable host API table (host_adapters.c) */

#define PL_MAX     64
#define PL_ID_MAX  160
#define PL_DIR_MAX 2048   /* holds dirname(<game-dir>/mods/<name>/plugin.so); matches discover()'s buf */

typedef struct {
    void *handle;                              /* dlopen handle (retained to process exit) */
    const struct roth_plugin_info_v1 *info;    /* the plugin's static, immutable info struct */
    char id[PL_ID_MAX];                        /* copied reverse-DNS id (duplicate detection) */
    char dir[PL_DIR_MAX];                      /* the plugin's own folder (mods/<name>/) for cfg/assets */
} pl_slot_t;

static pl_slot_t g_pl[PL_MAX];
static int       g_npl;

/* The directory of the plugin whose MAIN lifecycle callback is currently running — what
 * api->plugin_dir() returns (task #103, SDK 0.4). Set around each single-threaded MAIN dispatch
 * (on_load / on_game_ram_ready / on_register_overrides / on_unload) and cleared after; "" otherwise.
 * host_adapters.c's roth_host_api_v1.plugin_dir points at roth_sdk_current_plugin_dir below. */
static const char *g_cur_dispatch_dir;
const char *roth_sdk_current_plugin_dir(void) { return g_cur_dispatch_dir ? g_cur_dispatch_dir : ""; }

static int      g_opt_no_mods, g_opt_strict, g_opt_list, g_opt_dump;
static int      g_discovered;                  /* discovery ran (guards a double discover) */
static int      g_frame_seam_installed;
static uint16_t g_last_frame_tick;             /* on_frame_game dedup on the 0x90bcc engine tick */
static uint32_t (*g_inner_soft_int)(uint8_t, regs_t *);   /* the real g_os_soft_int, chained by the seam */

/* A callback field is present iff the plugin's struct_size reaches past it (forward/backward compat:
 * a plugin built against an older, shorter info struct simply lacks the newer callbacks). */
#define PL_HAS(info, field) \
    ((info)->struct_size >= (uint32_t)(offsetof(struct roth_plugin_info_v1, field) + sizeof(void *)))

/* ------------------------------------------------------------------ options -- */
void plugins_configure(int no_mods, int strict, int list, int dump)
{
    g_opt_no_mods = no_mods;
    g_opt_strict  = strict;
    g_opt_list    = list;
    g_opt_dump    = dump;
}
int plugins_list_mode(void)   { return g_opt_list; }
int plugins_nomods_flag(void) { return g_opt_no_mods; }

static int plugins_disabled(void)
{
    if (g_opt_no_mods)
        return 1;
    const char *sw = getenv("ROTH_MODS");   /* existing kill-switch: ROTH_MODS=0 */
    return (sw && sw[0] == '0');
}

/* ---------------------------------------------------------------- validation -- */
static int id_valid(const char *id)
{
    if (!id)
        return 0;
    size_t n = strnlen(id, PL_ID_MAX);
    if (n == 0 || n >= PL_ID_MAX)
        return 0;
    if (!strchr(id, '.'))                   /* reverse-DNS: at least one dot */
        return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)id[i];
        if (c < 0x20 || c >= 0x7f)          /* printable ASCII only */
            return 0;
    }
    return 1;
}

static int id_taken(const char *id)
{
    for (int i = 0; i < g_npl; i++)
        if (strcmp(g_pl[i].id, id) == 0)
            return 1;
    return 0;
}

static int validate(const struct roth_plugin_info_v1 *info, const char *path)
{
    if (!info) {
        LOGE("[plugins] %s: roth_plugin_query_v1 returned NULL — rejected\n", path);
        return 0;
    }
    if (info->struct_size < (uint32_t)(offsetof(struct roth_plugin_info_v1, id) + sizeof(void *)) ||
        info->struct_size > 4096) {
        LOGE("[plugins] %s: implausible struct_size %u — rejected\n", path, info->struct_size);
        return 0;
    }
    if (info->abi_major != ROTH_ABI_MAJOR || info->abi_minor > ROTH_ABI_MINOR) {
        LOGE("[plugins] %s: ABI %u.%u incompatible with host %u.%u "
             "(load rule: major== && minor<=) — rejected\n",
             path, info->abi_major, info->abi_minor, ROTH_ABI_MAJOR, ROTH_ABI_MINOR);
        return 0;
    }
    if (!id_valid(info->id)) {
        LOGE("[plugins] %s: missing/invalid reverse-DNS id — rejected\n", path);
        return 0;
    }
    if (id_taken(info->id)) {
        LOGE("[plugins] %s: duplicate plugin id \"%s\" (first wins) — rejected\n", path, info->id);
        return 0;
    }
    return 1;
}

/* ----------------------------------------------------------------- discovery -- */
static void load_one(const char *path)
{
    if (g_npl >= PL_MAX) {
        LOGE("[plugins] too many plugins (max %d) — %s skipped\n", PL_MAX, path);
        return;
    }
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);   /* local scope: no symbol leakage (finding 8) */
    if (!h) {
        LOGE("[plugins] dlopen %s failed: %s — rejected\n", path, dlerror());
        if (g_opt_strict) exit(2);
        return;
    }
    dlerror();
    /* The single versioned export is authoritative; a mod.toml beside the .so is packaging metadata
     * only and v1 ignores it entirely. */
    union { void *p; const struct roth_plugin_info_v1 *(*fn)(void); } q;
    q.p = dlsym(h, "roth_plugin_query_v1");
    const char *err = dlerror();
    if (err || !q.p) {
        LOGE("[plugins] %s: no roth_plugin_query_v1 export (%s) — rejected\n",
             path, err ? err : "null");
        dlclose(h);
        if (g_opt_strict) exit(2);
        return;
    }
    const struct roth_plugin_info_v1 *info = q.fn();
    if (!validate(info, path)) {
        dlclose(h);                          /* a rejected candidate was never live -> safe to close */
        if (g_opt_strict) exit(2);
        return;
    }
    pl_slot_t *s = &g_pl[g_npl++];
    s->handle = h;
    s->info   = info;
    snprintf(s->id, sizeof s->id, "%s", info->id);
    /* the plugin's own folder = dirname(path) (mods/<name>/) — api->plugin_dir() during MAIN. */
    snprintf(s->dir, sizeof s->dir, "%s", path);
    char *sl = strrchr(s->dir, '/');
    if (sl) *sl = 0; else s->dir[0] = 0;
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void discover(const char *game_dir)
{
    if (g_discovered)
        return;
    g_discovered = 1;

    char moddir[1024];
    snprintf(moddir, sizeof moddir, "%s/mods", game_dir ? game_dir : ".");
    DIR *d = opendir(moddir);
    if (!d) {
        LOGE("[plugins] no mods directory at %s — no plugins loaded\n", moddir);
        return;
    }
    static char names[PL_MAX][256];
    const char *nameptr[PL_MAX];
    int nn = 0;
    struct dirent *e;
    while ((e = readdir(d)) && nn < PL_MAX) {
        if (e->d_name[0] == '.')             /* skip ".", "..", dotfiles */
            continue;
        snprintf(names[nn], sizeof names[nn], "%s", e->d_name);
        nameptr[nn] = names[nn];
        nn++;
    }
    closedir(d);
    qsort(nameptr, (size_t)nn, sizeof nameptr[0], name_cmp);   /* deterministic load order */

    for (int i = 0; i < nn; i++) {
        char so[2048];
        snprintf(so, sizeof so, "%s/%s/plugin.so", moddir, nameptr[i]);
        if (access(so, R_OK) != 0)           /* a subdir without a plugin.so is not a plugin */
            continue;
        load_one(so);
    }
}

/* ------------------------------------------------------------------- report --- */
static void report(void)
{
    LOGE("[plugins] resolved %d plugin(s) from mods/ (load order = lexicographic):\n", g_npl);
    for (int i = 0; i < g_npl; i++) {
        const struct roth_plugin_info_v1 *in = g_pl[i].info;
        char cb[160];
        cb[0] = 0;
        #define ADD(field, label) do {                                       \
            if (PL_HAS(in, field) && in->field) {                            \
                if (cb[0]) strncat(cb, ",", sizeof cb - strlen(cb) - 1);     \
                strncat(cb, label, sizeof cb - strlen(cb) - 1);              \
            } } while (0)
        ADD(on_load, "load");
        ADD(on_game_ram_ready, "game_ram_ready");
        ADD(on_frame_game, "frame");
        ADD(on_compose_tick, "compose");
        ADD(on_audio, "audio");
        ADD(on_unload, "unload");
        ADD(on_register_overrides, "overrides");
        ADD(on_scancode, "scancode");
        ADD(on_mouse_poll, "mouse_poll");
        #undef ADD
        LOGE("[plugins]   #%d %s v%s (%s) abi=%u.%u api_use=0x%x callbacks=[%s]\n", i,
             in->id, in->version ? in->version : "?", in->name ? in->name : "?",
             in->abi_major, in->abi_minor, in->api_use, cb[0] ? cb : "none");
    }
}

/* ------------------------------------------------------- on_frame_game seam --- */
/* Dispatched on the GAME thread once per engine tick. The engine's main loop is pristine (no edit
 * possible), so we chain the int33 mouse-poll host path — g_os_soft_int, which the engine drives from
 * the game thread 2..15x per rendered frame — and dedup on the engine's own 70Hz tick counter
 * (0x90bcc, the project's timing law) so a plugin's on_frame_game fires exactly once per tick. This
 * re-point lives ENTIRELY in the image-free lane (boot.c set g_os_soft_int=host_soft_int; we chain it):
 * the shared standalone_hooks.c / host_soft_int stay byte-identical, so the trap host is untouched. */
static void frame_pump(void)
{
    /* Equality compare = wrap-safe on the 16-bit counter (once-per-tick gate; item_grabber idiom). */
    uint16_t now = *(volatile uint16_t *)(uintptr_t)(0x90bccu + OBJ_DELTA);
    if (now == g_last_frame_tick)
        return;
    g_last_frame_tick = now;
    for (int i = 0; i < g_npl; i++)
        if (PL_HAS(g_pl[i].info, on_frame_game) && g_pl[i].info->on_frame_game)
            g_pl[i].info->on_frame_game(&roth_host_api_v1);
}

/* on_mouse_poll(GAME): the int33 mouse seam (SDK 0.4). Fires on the game thread after the real
 * int33 (so it sees the poll result), with the SDK-neutral roth_mouse_poll_v1 view. Plugins may
 * modify ebx/ecx/edx to hide the mouse from the game (overlay capture); we write the result back.
 * Runs inside g_inner_soft_int's relay (the plugin platform is the only int33 seam consumer now; the
 * legacy mods layer is retired, §12). `ax_in` is the subfunction captured BEFORE the call (io->eax by
 * then holds the result). */
static void dispatch_mouse_poll(uint16_t ax_in, regs_t *io)
{
    struct roth_mouse_poll_v1 mp;
    mp.ax = ax_in; mp.ebx = io->ebx; mp.ecx = io->ecx; mp.edx = io->edx;
    int any = 0;
    for (int i = 0; i < g_npl; i++)
        if (PL_HAS(g_pl[i].info, on_mouse_poll) && g_pl[i].info->on_mouse_poll) {
            g_pl[i].info->on_mouse_poll(&roth_host_api_v1, &mp);
            any = 1;
        }
    if (any) { io->ebx = mp.ebx; io->ecx = mp.ecx; io->edx = mp.edx; }
}

static uint32_t frame_relay(uint8_t vec, regs_t *io)
{
    uint16_t ax_in = (uint16_t)io->eax;         /* int33 subfunction, captured before the call runs */
    uint32_t r = g_inner_soft_int(vec, io);     /* the real int21/int10/int31/int33 host body first */
    if (vec == 0x33) {                          /* mouse poll: game-thread, per-frame chokepoint */
        dispatch_mouse_poll(ax_in, io);         /* on_mouse_poll(GAME): per-poll, engine calls legal */
        frame_pump();                           /* on_frame_game(GAME): deduped on the 0x90bcc tick */
    }
    return r;
}

/* Both int33-riding seams (on_frame_game + on_mouse_poll) install the SAME g_os_soft_int chain. */
static int any_int33_seam_wanted(void)
{
    for (int i = 0; i < g_npl; i++)
        if ((PL_HAS(g_pl[i].info, on_frame_game) && g_pl[i].info->on_frame_game) ||
            (PL_HAS(g_pl[i].info, on_mouse_poll) && g_pl[i].info->on_mouse_poll))
            return 1;
    return 0;
}

static void install_frame_seam(void)
{
    if (g_frame_seam_installed)
        return;
    if (!any_int33_seam_wanted())
        return;                                 /* no int33-riding plugin -> zero overhead, no re-point */
    g_inner_soft_int = g_os_soft_int;           /* host_soft_int, bound by boot.c step 4 */
    g_os_soft_int    = frame_relay;
    g_frame_seam_installed = 1;
    g_last_frame_tick = *(volatile uint16_t *)(uintptr_t)(0x90bccu + OBJ_DELTA);   /* prime */
    LOGE("[plugins] int33 seam installed (on_frame_game deduped on the 0x90bcc engine tick; on_mouse_poll per poll)\n");
}

/* ------------------------------------------------------------ public lifecycle -- */
static void set_game_ram_window(void)
{
    roth_game_ram_base = (uint8_t *)(uintptr_t)OBJ_DELTA;      /* canonical VA 0 */
    roth_game_ram_size = (uint32_t)(STACK_TOP - OBJ_DELTA);    /* through the obj3 arena top (0xa8170) */
}

void plugins_discover_report(const char *game_dir)
{
    set_game_ram_window();
    if (plugins_disabled()) {
        LOGE("[plugins] disabled (%s) — nothing to list\n",
             g_opt_no_mods ? "--no-mods" : "ROTH_MODS=0");
        return;
    }
    discover(game_dir);
    report();
}

void plugins_load(const char *game_dir)
{
    set_game_ram_window();
    if (plugins_disabled()) {
        LOGE("[plugins] disabled (%s) — no plugins loaded, mods layer off\n",
             g_opt_no_mods ? "--no-mods" : "ROTH_MODS=0");
        return;
    }
    discover(game_dir);
    report();
    /* on_load(MAIN): after ALL validation, before any engine thread starts (§10.3). */
    for (int i = 0; i < g_npl; i++)
        if (PL_HAS(g_pl[i].info, on_load) && g_pl[i].info->on_load) {
            g_cur_dispatch_dir = g_pl[i].dir;           /* api->plugin_dir() valid inside the callback */
            g_pl[i].info->on_load(&roth_host_api_v1);
            g_cur_dispatch_dir = NULL;
        }
    install_frame_seam();
}

void plugins_dispatch_game_ram_ready(void)
{
    for (int i = 0; i < g_npl; i++)             /* g_npl==0 when disabled -> no-op */
        if (PL_HAS(g_pl[i].info, on_game_ram_ready) && g_pl[i].info->on_game_ram_ready) {
            g_cur_dispatch_dir = g_pl[i].dir;
            g_pl[i].info->on_game_ram_ready(&roth_host_api_v1);
            g_cur_dispatch_dir = NULL;
        }
}

void plugins_dispatch_compose_tick(uint8_t *pixels, uint32_t width, uint32_t height)
{
    for (int i = 0; i < g_npl; i++)
        if (PL_HAS(g_pl[i].info, on_compose_tick) && g_pl[i].info->on_compose_tick)
            g_pl[i].info->on_compose_tick(&roth_host_api_v1, pixels, width, height);
}

/* on_scancode(TICK_ISR): the input seam, called from iso_apply_scancode (traps.c) — the plugin
 * platform is the only input-seam consumer now (the legacy mods layer is retired, §12). Runs each
 * plugin's filter in LOAD ORDER; a 0 return SWALLOWS the key and stops the chain (overlay input
 * capture). Returns the possibly-rewritten scancode. TICK_ISR context: no engine calls (contract).
 * g_npl==0 when disabled -> pure passthrough. */
uint8_t plugins_dispatch_scancode(uint8_t sc)
{
    for (int i = 0; i < g_npl; i++)
        if (PL_HAS(g_pl[i].info, on_scancode) && g_pl[i].info->on_scancode) {
            sc = g_pl[i].info->on_scancode(&roth_host_api_v1, sc);
            if (sc == 0)
                return 0;                        /* swallowed — no further plugin sees it */
        }
    return sc;
}

/* on_register_overrides(MAIN) for every plugin, then resolve chains + patch pad entries (task #103).
 * Runs pre-thread in boot step 6b; with 0 plugins it is a no-op (0 patches = pristine call targets =
 * the §10.5 purity promise). */
void plugins_apply_overrides(void)
{
    if (plugins_disabled())
        return;
    for (int i = 0; i < g_npl; i++) {
        if (PL_HAS(g_pl[i].info, on_register_overrides) && g_pl[i].info->on_register_overrides) {
            overrides_begin_plugin(i, g_pl[i].id);
            g_cur_dispatch_dir = g_pl[i].dir;
            g_pl[i].info->on_register_overrides(&roth_host_api_v1, overrides_registrar());
            g_cur_dispatch_dir = NULL;
        }
    }
    overrides_apply(g_opt_strict, &roth_host_api_v1);
    if (g_opt_dump)
        overrides_dump_chains();
}

void plugins_dispatch_unload(void)
{
    for (int i = g_npl - 1; i >= 0; i--)        /* reverse load order */
        if (PL_HAS(g_pl[i].info, on_unload) && g_pl[i].info->on_unload) {
            g_cur_dispatch_dir = g_pl[i].dir;
            g_pl[i].info->on_unload(&roth_host_api_v1);
            g_cur_dispatch_dir = NULL;
        }
    /* v1: NO dlclose of accepted plugins — handles retained to process exit (§10.3 / finding 3). */
}
