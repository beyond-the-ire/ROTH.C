/* boot.c — the native image-free boot.
 *
 * Linked ONLY into the moddable engine binary (roth) (not the trap host). Replaces main.c's obj1-mapping + int3
 * live-swap + jmp-ENTRY_VA boot: it maps the arena + DOS/VGA/VESA host memory (NO original CODE), fills
 * obj3 DATA from datac (GADDR==CANON at the pinned 0x470000), stages the one CRT-min byte, binds the C2
 * providers + host hooks (a function-pointer boundary, never call_orig), arms the always-interactive ISR
 * host-mode + the timer/audio/shm services, then calls the lifted entry roth_main() DIRECTLY on the
 * native stack. This TU also carries the image-free-only host hooks (roth_unreachable / roth_sprintf
 * / the 2 parked audio-residual dispatchers) so standalone_hooks.o stays link-safe in the normal host.
 *
 * Provider disposition (nm-clean by construction — none of these reaches original CODE bytes):
 *   g_os_sel_base       -> dpmi_sel_base        (flat base; the ~19 dead selector hatches gc away)
 *   g_os_soft_int       -> host_soft_int        (inline int21/int10/int31/int33 -> host services)
 *   g_os_port_out       -> host_dac_port_out    (GDV fmt-1 DAC fade)
 *   g_os_publish_frame  -> host_gdv_publish_frame
 *   the DOS/DPMI/lowmem c2 contract  -> host_c2.o (pure host C)
 *   the os_audio contract            -> audio_c2_host.o natives + audio_c2_imgfree.o always-native shim
 */
#include "roth_host.h"
#include "standalone_hooks.h"
#include "audio.h"
#include "os_api.h"
#include "obj1data.h"             /* datac: the staged obj1-resident DATA constants */
#include "engine.h"     /* roth_main + the g_os_* fn-ptr hooks */

/* datac's obj3 loader (forward-declared to avoid obj3_data.h's OBJ3_SIZE clash with roth_host.h). */
void obj3_build(unsigned char *out, const uint32_t base[4]);
/* the runtime plugin platform (task #103; src/platform/plugin_loader.c) — MODDABLE flavor only. In the
 * vanilla flavor (-DROTH_VANILLA) none of it is linked: no loader, no override registry, no pads — mods
 * impossible by construction (the two-flavor ruling, docs/MODS_PLATFORM.md §12). */
#ifndef ROTH_VANILLA
#include "plugin_loader.h"
extern const int roth_ovr_target_count;   /* override_registry.c: # of eligible pad entries (diagnostics) */
#endif

#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <execinfo.h>   /* ROTH_CRASH_DIAG backtrace */

extern uint32_t (*g_os_sel_base)(uint16_t);
extern uint32_t (*g_os_soft_int)(uint8_t, regs_t *);
extern void     (*g_os_port_out)(uint16_t, uint8_t);
extern void     (*g_os_publish_frame)(void);
void host_gdv_publish_frame(void);        /* traps.c */

/* ---- the ROTH_OBJ1_GUARD fail-loud audit mode ----
 * Default: the obj1 canon range is a zero-filled RW arena + the staged DATA constants —
 * every enumerated access Just Works, but an UN-enumerated obj1-data read silently returns 0.
 * Guard mode (ROTH_OBJ1_GUARD=1): after staging, the range is flipped PROT_NONE; the first touch
 * of each page SIGSEGVs into this handler, which logs `canon:PC` (async-signal-safe write only),
 * remaps that ONE page RW (contents survive mprotect — the staged bytes are intact) and retries.
 * A clean boot-to-title must fault ONLY at the enumerated boot-subset pages — anything else is a
 * new un-enumerated obj1-data access, caught loud with a precise address instead of a silent 0. */
static void guard_hex(char *p, uint32_t v)
{
    for (int i = 7; i >= 0; i--) { p[i] = "0123456789abcdef"[v & 0xf]; v >>= 4; }
}
static void obj1_guard_fault(int sig, siginfo_t *si, void *uctx)
{
    uintptr_t a = (uintptr_t)si->si_addr;
    (void)sig;
    if (a >= OBJ1_BASE && a < OBJ2_BASE) {
        ucontext_t *uc = (ucontext_t *)uctx;
        char msg[] = "[host] obj1-guard: first touch @canon 0x???????? pc 0x????????\n";
        guard_hex(strchr(msg, '?'), (uint32_t)(a - OBJ_DELTA));
        guard_hex(strchr(msg, '?'), (uint32_t)uc->uc_mcontext.gregs[REG_EIP]);
        ssize_t r = write(2, msg, sizeof msg - 1); (void)r;
        mprotect((void *)(a & ~0xfffu), 0x1000, PROT_READ | PROT_WRITE);
        return;                               /* retry the faulting instruction */
    }
    signal(SIGSEGV, SIG_DFL);                 /* not an obj1-range fault: re-crash normally */
    raise(SIGSEGV);
}

/* ---- ROTH_CRASH_DIAG: in-game crash reporter for the secondary-surface render SIGSEGV ----
 * Arms a SIGSEGV handler that, at the fault, dumps (async-signal-safe: write()+hex only) the fault
 * address, the render-target/framebuffer state, the secondary-surface record list, and a backtrace,
 * then re-raises so a core is still produced. One repro captures everything needed to pin the bad
 * projection. Env-gated; imgfree-only (this TU links only into the moddable engine binary (roth)). */
static void diag_kv(const char *label, uint32_t v)
{
    char buf[72];
    int n = 0;
    while (label[n] && n < 48) { buf[n] = label[n]; n++; }
    buf[n++] = '0'; buf[n++] = 'x';
    for (int i = 28; i >= 0; i -= 4) buf[n++] = "0123456789abcdef"[(v >> i) & 0xf];
    buf[n++] = '\n';
    ssize_t r = write(2, buf, (size_t)n); (void)r;
}
static uint32_t diag_r32(uint32_t canon) { return *(volatile uint32_t *)(uintptr_t)(canon + OBJ_DELTA); }
static uint16_t diag_r16(uint32_t canon) { return *(volatile uint16_t *)(uintptr_t)(canon + OBJ_DELTA); }
static uint8_t  diag_r8 (uint32_t canon) { return *(volatile uint8_t  *)(uintptr_t)(canon + OBJ_DELTA); }

static void crash_diag_fault(int sig, siginfo_t *si, void *uctx)
{
    (void)sig;
    ucontext_t *uc = (ucontext_t *)uctx;
    uint32_t fault = (uint32_t)(uintptr_t)si->si_addr;
    static const char hdr[] = "\n===== ROTH_CRASH_DIAG: SIGSEGV =====\n";
    ssize_t r = write(2, hdr, sizeof hdr - 1); (void)r;
    diag_kv("fault_addr      = ", fault);
    diag_kv("eip             = ", (uint32_t)uc->uc_mcontext.gregs[REG_EIP]);

    uint16_t selp = diag_r16(0x90c06), sels = diag_r16(0x89f28);
    uint32_t bp = g_os_sel_base ? g_os_sel_base(selp) : 0;
    uint32_t bs = g_os_sel_base ? g_os_sel_base(sels) : 0;
    diag_kv("framebuffer_ptr = ", diag_r32(0x90a98));
    diag_kv("render_tgt_buf  = ", diag_r32(0x85414));
    diag_kv("rt_sel_primary  = ", selp);
    diag_kv("rt_base_primary = ", bp);
    diag_kv("rt_sel_second   = ", sels);
    diag_kv("rt_base_second  = ", bs);
    diag_kv("fault-minus-pri = ", fault - bp);   /* the wild span offset if primary is the target */
    diag_kv("fault-minus-sec = ", fault - bs);   /* ... or if secondary is the target */

    diag_kv("has_secondary   = ", diag_r8(0x853d0));
    diag_kv("secondary_count = ", diag_r16(0x85318));
    diag_kv("subpass_kind    = ", diag_r8(0x90a48));
    int cnt = (int)(int16_t)diag_r16(0x85318);
    if (cnt < 0 || cnt > 8) cnt = 8;
    for (int i = 0; i < cnt; i++) {              /* the secondary-surface records (0x10 B / 8 words) */
        diag_kv("-- record --    = ", (uint32_t)i);
        for (int w = 0; w < 8; w++)
            diag_kv("   word        = ", diag_r16(0x84b18 + (uint32_t)i * 0x10 + (uint32_t)w * 2));
    }

    static const char btl[] = "----- backtrace -----\n";
    r = write(2, btl, sizeof btl - 1); (void)r;
    void *bt[32];
    int nb = backtrace(bt, 32);
    backtrace_symbols_fd(bt, nb, 2);

    signal(SIGSEGV, SIG_DFL);                    /* re-raise so a core is still produced */
    raise(SIGSEGV);
}

void roth_boot(void)
{
    LOGE("roth_boot: native standalone boot — no obj1/obj2 CODE mapped, no call_orig, no trampoline\n");

    /* 1. Host memory maps — arena + DOS/VGA/VESA, but NO obj1/obj2 CODE image and no load_blob. */
    map_fixed(OBJ3_BASE, STACK_TOP - OBJ3_BASE + 0xe90, PROT_READ | PROT_WRITE); /* pinned obj3 arena */
    map_fixed(DOSMEM_LIN, DOSMEM_SIZE, PROT_READ | PROT_WRITE);                  /* DPMI/DOS pool      */
    map_fixed(VGA_LIN, VGA_SIZE, PROT_READ | PROT_WRITE);                        /* 0xA0000 window     */
    map_fixed(VESA_LFB_LIN, VESA_LFB_SIZE, PROT_READ | PROT_WRITE);              /* VESA LFB sink       */
    /* 1b. the obj1-resident DATA arena — zero-filled RW over the
     *    obj1 canon range (NO original code bytes; the arena is never executed). The ~46 boot-touched
     *    obj1-data slots (G16(0x2ef54) DS slot, framebuffer globals, SMC round-trip words, code-ptr
     *    token stores) then Just Work: write-first slots get correct zero-init, and obj1data_build
     *    stages the few read-before-write DATA constants (config strings + the VESA mode table,
     *    235 bytes, generator-asserted reloc-free). */
    map_fixed(OBJ1_BASE, OBJ2_BASE - OBJ1_BASE, PROT_READ | PROT_WRITE);

    /* 2. Materialize obj3 DATA into the pinned arena (datac; Q4: GADDR==CANON at 0x470000). obj3 is
     *    DATA (legal image-free); the 159 obj1 code relocs land on OBJ1_BASE tokens (write-only). */
    static const uint32_t base[4] = { 0, OBJ1_BASE, OBJ2_BASE, OBJ3_BASE };
    obj3_build((unsigned char *)(uintptr_t)OBJ3_BASE, base);
    obj1data_build((unsigned char *)(uintptr_t)OBJ1_BASE);

    /* 2a. the light-pattern pointer table (canon 0x32310, 5 entries) carries LE fixups, so it
     *     can't ride obj1data (raw bytes). Re-express it symbolically: each entry points at a sub-block
     *     inside the DATA block obj1data just staged (0x322ce..0x3230b). GADDR==CANON at OBJ1_BASE, so
     *     the pointer value IS the runtime address. Without this the count reads 5 but the pointers read
     *     0 -> light/texture pattern animation silently dies. */
    {
        static const uint32_t lp_tbl[5] = { 0x322ceu, 0x322dfu, 0x322eeu, 0x322fbu, 0x32304u };
        for (unsigned i = 0; i < 5; i++)
            *(volatile uint32_t *)(uintptr_t)(0x32310u + OBJ_DELTA + i * 4u) =
                (uint32_t)(lp_tbl[i] + OBJ_DELTA);
    }

    /* 2a'. the default-texture-record pointer cell [0x29e58] -> 0x29e5c (fixup-carrying,
     *      so symbolic like 2a; the surrounding record bytes ride obj1data). Consumed by the faceres
     *      cur==0xffff (0x2c7d0) and rwss type-1 id==0xffff (0x2b7a6) default-record paths. */
    *(volatile uint32_t *)(uintptr_t)(0x29e58u + OBJ_DELTA) = (uint32_t)(0x29e5cu + OBJ_DELTA);

    /* 2a''. the 0x30780 RAW-command handler table (128 fixup-carrying code ptrs, obj1-resident) —
     *       staged as canon+delta TOKENS (write-only per the code-ptr policy; consumed by
     *       lift_raw_commands.c rawcmd_dispatch_30780's enumerated switch, fail-loud on any other value). */
    {
        static const uint32_t rawcmd_tbl[128] = {
        0x30ab0u, 0x30ab0u, 0x33b94u, 0x312a1u, 0x30ab0u, 0x30ab0u, 0x30f51u, 0x3121cu,
        0x30ab0u, 0x32ac5u, 0x32626u, 0x32626u, 0x32645u, 0x327f8u, 0x32473u, 0x324a7u,
        0x31339u, 0x32269u, 0x32195u, 0x30ab0u, 0x315c4u, 0x318fdu, 0x313b4u, 0x31563u,
        0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x3179cu, 0x33ac4u, 0x31c31u, 0x3198eu,
        0x31700u, 0x31b1au, 0x31a62u, 0x31107u, 0x3146du, 0x30ab0u, 0x35617u, 0x35544u,
        0x355a7u, 0x35437u, 0x354d3u, 0x3540bu, 0x30ab0u, 0x311adu, 0x31676u, 0x33a69u,
        0x30ab0u, 0x30ab0u, 0x30ab0u, 0x320e6u, 0x32738u, 0x30f83u, 0x31326u, 0x30ab0u,
        0x30f55u, 0x30ab0u, 0x31000u, 0x3104au, 0x30d10u, 0x30ab0u, 0x30b23u, 0x30acbu,
        0x304b8u, 0x30ab3u, 0x30f63u, 0x30ab0u, 0x30ab0u, 0x33be2u, 0x335ecu, 0x30ab0u,
        0x30ab0u, 0x30ab0u, 0x32d7du, 0x30ab0u, 0x32c05u, 0x33229u, 0x333c0u, 0x3354au,
        0x3286bu, 0x324d2u, 0x32592u, 0x30ab0u, 0x32324u, 0x32221u, 0x30ab0u, 0x30ab0u,
        0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u,
        0x33b3bu, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x33091u, 0x33188u,
        0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u,
        0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u,
        0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u,
        };
        for (unsigned i = 0; i < 128; i++)
            *(volatile uint32_t *)(uintptr_t)(0x30780u + OBJ_DELTA + i * 4u) =
                (uint32_t)(rawcmd_tbl[i] + OBJ_DELTA);
    }

    /* 2a'''. the 0x3088c ACTIVE-EFFECT tick table (64 entries, a subset
     *        of the 0x30780 handler set) — same token staging; registrants overwrite live. */
    {
        static const uint32_t effect_tbl[64] = {
            0x30ab0u, 0x30ab0u, 0x33be2u, 0x335ecu, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x32d7du,
            0x30ab0u, 0x32c05u, 0x33229u, 0x333c0u, 0x3354au, 0x3286bu, 0x324d2u, 0x32592u,
            0x30ab0u, 0x32324u, 0x32221u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u,
            0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x33b3bu, 0x30ab0u, 0x30ab0u,
            0x30ab0u, 0x30ab0u, 0x30ab0u, 0x33091u, 0x33188u, 0x30ab0u, 0x30ab0u, 0x30ab0u,
            0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u,
            0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u,
            0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u, 0x30ab0u,
        };
        for (unsigned i = 0; i < 64; i++)
            *(volatile uint32_t *)(uintptr_t)(0x3088cu + OBJ_DELTA + i * 4u) =
                (uint32_t)(effect_tbl[i] + OBJ_DELTA);
    }

    /* 2b. the fail-loud obj1 audit mode (see obj1_guard_fault above). */
    if (getenv("ROTH_OBJ1_GUARD")) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = obj1_guard_fault;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL);
        mprotect((void *)(uintptr_t)OBJ1_BASE, OBJ2_BASE - OBJ1_BASE, PROT_NONE);
        LOGE("ROTH_OBJ1_GUARD=1: obj1 arena PROT_NONE — per-page first-touch audit armed\n");
    }

    /* 3. CRT-min (Q1): the one byte the game reads — g_dos_major (the CRT's int21 AH=30 store). errno /
     *    _doserrno are arena BSS (free). No _cstart_ runs. */
    *(volatile uint8_t *)(uintptr_t)(0x7256bu + OBJ_DELTA) = 5;
    /* 3b. CRT arg0 (exe path) [0x7253c] — boot finding: parse_config_keywords' <exe-dir>\
     *    fallback walks this pointer whenever the primary open fails, and CONFIG.INI is absent in
     *    every known install, so the fallback RUNS on every boot (trap host included — there the
     *    CRT parses arg0 from the env block main.c stages). Stage the same string; a host static
     *    is a valid flat pointer, and the built "C:\CONFIG.INI" open fails gracefully like the
     *    trap host's does (return 0 -> the template-default config path). */
    static const char arg0[] = "C:\\ROTH.EXE";
    *(volatile uint32_t *)(uintptr_t)(0x7253cu + OBJ_DELTA) = (uint32_t)(uintptr_t)arg0;

    /* 4. Install the C2 providers + host hooks (function-ptr boundary, NOT call_orig). */
    g_os_sel_base = dpmi_sel_base;          /* flat: kills the dead selector hatches at runtime */
    g_os_soft_int = host_soft_int;          /* int21/int10/int31/int33 -> host services */
    g_os_port_out = host_dac_port_out;      /* GDV fmt-1 DAC fade */
    g_os_publish_frame = host_gdv_publish_frame;

    /* 5. Always-interactive ISR host-mode (Q4): SIGALRM -> the C ISR bodies (shm_tick), no inject_irq. */
    g_standalone_boot = 1;
    g_os_interactive = 1;

    /* 6. Host memory-mapped framebuffer (spawns NO thread — safe before the plugin/mod platform). */
    shm_setup();

    /* 6a2. ROTH_CRASH_DIAG: arm the in-game crash reporter (g_os_sel_base is set by step 4). */
    if (getenv("ROTH_CRASH_DIAG")) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = crash_diag_fault;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL);
        LOGE("ROTH_CRASH_DIAG=1: SIGSEGV crash reporter armed (dumps render-target + secondary-surface state)\n");
    }

    /* 6b. THE PLUGIN PLATFORM (task #103, the two-flavor ruling) — boot-order LAW (MODS_PLATFORM.md
     *     §10.3 / §12): ALL plugin discovery/validation/load + the MAIN lifecycle callbacks +
     *     on_register_overrides + entry patching complete HERE, BEFORE irq_timer_start() (the SIGALRM
     *     tick / TICK_ISR source, and the compose seam plugins_dispatch_compose_tick feeds) and before
     *     audio_init() (the audio pump/thread) — so no plugin callback can race a live tick. game_ram is
     *     fully staged pristine by steps 1-3, so on_game_ram_ready sees it clean. MODDABLE flavor only:
     *     the vanilla artifact links none of this (mods impossible by construction) and the trap
     *     host/oracle links none of it either. */
#ifdef ROTH_VANILLA
    LOGE("build flavor: vanilla (no mod surface — pads and loader absent by construction)\n");
#else
    LOGE("build flavor: moddable (runtime plugin platform armed; NOP pads at %d entries)\n",
         roth_ovr_target_count);
    plugins_load(g_game_dir);           /* discover+dlopen+validate+build api+report+on_load(MAIN)+seam */
    plugins_dispatch_game_ram_ready();  /* on_game_ram_ready(MAIN) — game_ram staged pristine, pre-threads */
    /* 6b'. THE OVERRIDE REGISTRY (task #103): on_register_overrides(MAIN) for every plugin, then build
     *      the immutable priority chains and patch the pad entries — jmp rel32 -> per-fn dispatch thunk.
     *      Single-threaded, AFTER game_ram is staged pristine and BEFORE audio_init()/irq_timer_start()
     *      so no patched entry executes mid-write (§11.2). With no plugins this patches nothing (pads
     *      stay NOP = pristine call targets). ROTH_MODS=0 / --no-mods make the whole platform inert. */
    plugins_apply_overrides();
#endif

    /* 6c. NOW the thread sources: the audio pump + the SIGALRM ISR-surrogate tick. */
    LOGE("roth_boot: plugin/mod MAIN phase complete — starting audio + the SIGALRM timer tick\n");
    audio_init();
    irq_timer_start();

    /* 7. Enter the game as verified C, directly on the native process stack. */
    roth_main();                            /* 0x15110 */

    /* 7b. Teardown: on_unload(MAIN), reverse load order, after the game has stopped. (The SDL
     *     window-close quit path _exit()s from shm_tick, so on_unload runs only on a clean roth_main
     *     return — the same hard-exit teardown the host already uses elsewhere.) MODDABLE flavor only. */
#ifndef ROTH_VANILLA
    plugins_dispatch_unload();
#endif

    LOGE("roth_boot: roth_main returned (game exited)\n");
}

/* ---- image-free-only host hooks (referenced only by the -DROTH_STANDALONE lifted TUs) ---- */

/* fail-loud: an in-game-only bridge target reached on the boot-to-title path with no original
 * CODE bytes mapped. The title path MUST never hit one; a hit is a precise, reportable stop. */
void roth_unreachable(uint32_t canon)
{
    LOGE("roth_unreachable: in-game-only bridge target 0x%x reached on the standalone boot-to-title "
         "path (no original CODE bytes mapped) — aborting (fail-loud)\n", canon);
    abort();
}

/* the Watcom CRT sprintf shim (0x27c53). CDECL/variadic. The boot+title callers use integer + string
 * conversions only (SAVE%D.SAV, "%D: %s", "%s%s", "\x9e%D", "Vesa %dx%d", write_roth_ini's settings
 * template @0x75f21); %D is Watcom long-decimal (== %ld, args are 32-bit). Handles %d/%D/%i, %u/%x/%X,
 * %s, %c, %% with an optional 0-flag + width; fail-loud (abort) on anything else so a wrong glyph never
 * slips through. Returns chars written (the sprintf contract — write_roth_ini uses it as the write
 * length). */
int roth_sprintf(char *dst, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *out = dst;
    for (const char *p = fmt; *p; ) {
        if (*p != '%') { *out++ = *p++; continue; }
        p++;
        if (*p == '%') { *out++ = '%'; p++; continue; }
        int zero = 0, width = 0;
        if (*p == '0') { zero = 1; p++; }
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
        if (*p == 'l' || *p == 'L') p++;
        char conv = *p ? *p++ : 0;
        char tmp[32];
        int n = 0;
        switch (conv) {
        case 'd': case 'D': case 'i': n = snprintf(tmp, sizeof tmp, "%d",  va_arg(ap, int));      break;
        case 'u':                     n = snprintf(tmp, sizeof tmp, "%u",  va_arg(ap, unsigned)); break;
        case 'x':                     n = snprintf(tmp, sizeof tmp, "%x",  va_arg(ap, unsigned)); break;
        case 'X':                     n = snprintf(tmp, sizeof tmp, "%X",  va_arg(ap, unsigned)); break;
        case 'c':                     tmp[0] = (char)va_arg(ap, int); n = 1;                      break;
        case 's': { const char *s = va_arg(ap, const char *); while (*s) *out++ = *s++; continue; }
        default:
            LOGE("roth_sprintf: unhandled conversion %%%c in \"%s\" — specifier-limited shim; "
                 "aborting fail-loud\n", conv ? conv : '?', fmt);
            abort();
        }
        for (int i = n; i < width; i++) *out++ = zero ? '0' : ' ';
        for (int i = 0; i < n; i++)     *out++ = tmp[i];
    }
    *out = 0;
    va_end(ap);
    return (int)(out - dst);
}

/* the two os_audio_* dispatchers with NO retired native (the audio residual): m4 (0x44cad,
 * never observed) + gdv_load_drivers (0x55360, parked link-time decision). os_audio_standalone.c omits
 * them; fail-loud-but-continue stubs so a stray reach LOGs once (gate = the VISUAL title; music may
 * lag) instead of an undefined-symbol link failure. Image-free-only (the normal host's audio_c2_bridge.o
 * owns these symbols). */
uint32_t os_audio_driver_call_m4(uint32_t slot, uint32_t cmd, uint32_t param, uint16_t sel)
{
    static int warned;
    if (!warned) { warned = 1; LOGE("os_audio_driver_call_m4: audio residual (0x44cad) reached "
                                    "image-free — parked, returning 0 (title music may lag)\n"); }
    (void)slot; (void)cmd; (void)param; (void)sel;
    return 0;
}
uint32_t os_audio_gdv_load_drivers(void)
{
    static int warned;
    if (!warned) { warned = 1; LOGE("os_audio_gdv_load_drivers: audio residual (0x55360) reached "
                                    "image-free — parked, returning 0 (title music may lag)\n"); }
    return 0;
}
