/* audio_trace.c — the audio co-dev trace tooling.
 *
 * The ring, before/after snapshot pool, passive ISR-table sampler and drain-to-file writer for the
 * audio-veneer retirement. Linked ONLY into roth-host (host lane); the imgfree audio slice and
 * the oracle never pull this TU. audio_c2_bridge.c taps every br()/br_stk() call through the two thin
 * entry points au_trace_enter/exit; the per-VA snapshot windows + branch predicates live HERE (the
 * bridge stays image-free-clean and only knows the canon VA + args).
 *
 * Discipline (mirrors audio.c's g_prof_* accumulate-only rule):
 *   - No malloc, no stdio, no I/O in the tap/sampler hot paths — fixed-size struct stores + small
 *     fixed memcpys of mapped game memory only.
 *   - The drain (file I/O) runs ONLY from the MAGIC_POLL safe point (audio.c), the same non-nesting
 *     spot audio_profile_dump emits from, using write(2) (async-signal-safe). It flushes only
 *     COMMITTED records (au_trace_exit bumps the committed head last, so an in-flight veneer call —
 *     entered but not yet exited, e.g. mid-call_orig when the timer IRQ preempts and reaches
 *     MAGIC_POLL — is never flushed with a torn record).
 *   - The taps run on the main game thread (as safe as the call_orig they bracket). The ISR sampler
 *     runs on the SIGALRM/shm_tick beat and does a read-only memcpy only (torn reads there ARE the
 *     interleave witness we want; they cannot corrupt anything).
 *
 * All snapshot/predicate addresses are canon; runtime = canon + OBJ_DELTA (audio.c's CANON()).
 */
#include "audio_trace.h"
#include "roth_host.h"   /* OBJ_DELTA, OBJ1_BASE, STACK_TOP, g_irq_eip */
#include "audio.h"       /* haudio_voice_struct_base — the runtime-resolved voice-struct window */

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CANON(x) ((uint32_t)((x) + OBJ_DELTA))

/* ---- switches, read once at startup ------------------------------------------------- */
static int         g_au_on;          /* ROTH_AU_TRACE master (the single hot-path check) */
static int         g_au_isr_on;      /* ROTH_AU_ISR_SAMPLE passive timer-table sampler */
static int         g_au_ab;          /* ROTH_AU_AB=native/1: run the STAGED retired host-C bodies */
/* ROTH_AU_AB_VA=0xVA[,0xVA...] — per-VA native bisect. When this list is
 * non-empty AND ROTH_AU_AB is unset, ONLY the listed canon VAs run their native host-C body; every
 * other bridge site (including the RETIRED-DEFAULT dispatch_simple/voice_start) falls back to the
 * call_orig veneer. Lets you attribute a staged-native regression in 2-3 short runs with no
 * rebuild. NOTE: distinct from ROTH_AU_TRACE_VA (g_au_va_filter), which only filters what is traced. */
#define AU_AB_VA_MAX 24
static uint32_t    g_au_ab_va[AU_AB_VA_MAX];
static unsigned    g_au_ab_va_n;
static int         g_au_ab_va_on;    /* the list is active (non-empty AND ROTH_AU_AB unset) */
static uint32_t    g_au_va_filter;   /* ROTH_AU_TRACE_VA=0x.. (canon; 0 = trace all) */
static const char *g_au_path = "/tmp/roth_au_trace.txt"; /* ROTH_AU_TRACE_FILE */

static void au_snap_pool_init(void);   /* one-time snapshot-pool allocation; defined with the pool below */

static void __attribute__((constructor)) au_trace_ctor(void)
{
    const char *s = getenv("ROTH_AU_TRACE");
    g_au_on = (s && s[0] && s[0] != '0');
    s = getenv("ROTH_AU_ISR_SAMPLE");
    g_au_isr_on = (s && s[0] && s[0] != '0');
    /* ROTH_AU_AB — the A/B binding selector, tri-state:
     *   unset      -> RETIRED-DEFAULT fns (dispatch_simple 0x45d28, voice_start 0x4a641) run their
     *                 native host-C bodies; STAGED-pending fns (voice_load 0x4ad03) run the veneer.
     *   =native/1  -> ALL staged candidates run native (incl. the pending ones) — the A/B side.
     *   =veneer/0  -> everything back on the call_orig veneers (the escape hatch / regression probe).
     * Flip evidence: dispatch_simple exact 10/10; voice_start 189-call
     * slot-containment invariant, both sides; ears-clean. */
    s = getenv("ROTH_AU_AB");
    g_au_ab = (s && (s[0] == 'n' || s[0] == 'N' || s[0] == '1')) ? 1
            : (s && (s[0] == 'v' || s[0] == 'V' || s[0] == '0')) ? 2 : 0;
    /* ROTH_AU_AB_VA: comma/space-separated canon VAs. Honored ONLY when ROTH_AU_AB is unset (a set
     * ROTH_AU_AB wins — that is the whole-batch A/B). Parsed once here. */
    s = getenv("ROTH_AU_AB_VA");
    if (s && s[0] && g_au_ab == 0) {
        while (*s && g_au_ab_va_n < AU_AB_VA_MAX) {
            while (*s == ',' || *s == ' ' || *s == '\t')
                s++;
            if (!*s)
                break;
            char *end = NULL;
            uint32_t v = (uint32_t)strtoul(s, &end, 0);
            if (end == s)                     /* malformed token: stop parsing */
                break;
            g_au_ab_va[g_au_ab_va_n++] = v;
            s = end;
        }
        g_au_ab_va_on = (g_au_ab_va_n > 0);
    }
    s = getenv("ROTH_AU_TRACE_VA");
    g_au_va_filter = s ? (uint32_t)strtoul(s, NULL, 0) : 0;
    s = getenv("ROTH_AU_TRACE_FILE");
    if (s && s[0])
        g_au_path = s;

    /* Allocate the (large) before/after snapshot pool ONCE, here at init, and only when tracing is
     * on — never lazily on the capture hot path, which can run in tick/ISR-like context. */
    if (g_au_on)
        au_snap_pool_init();
}

/* Bridge-side A/B selectors: read once at startup; a plain int load on the hot path.
 * au_ab_native() = "run even the PENDING staged candidates"; au_ab_veneer() = "force everything
 * back on the call_orig veneers" (the RETIRED-DEFAULT fns consult this one). */
int au_ab_native(void) { return g_au_ab == 1; }
int au_ab_veneer(void) { return g_au_ab == 2; }

/* The RETIRED-DEFAULT natives (run native when no flag says otherwise):
 *   - dispatch_simple 0x45d28 + voice_start 0x4a641: exact A/B + ears.
 *   - The batch EXONERATION: the "run-B clipping" turned out to be a PRE-EXISTING host
 *     mixer bug — reproduced with sword-swing+clank overlapping SFX under NO LIFTS
 *     at all (and equally under the run-1/run-2 ROTH_AU_AB_VA bisect subsets). With the confound
 *     removed, the batch's A/B evidence stands un-contradicted (byte-identical write-set windows;
 *     runs 1/2 ears = identical-to-original including the bug), so the full staged batch flips:
 *     detection trio 0x48b21/0x48f79/0x48c6b (run-1 ears + boot write-sets), close/enable/disable
 *     0x48666/0x47cf5/0x47d6e (byte-identical [0x7420c]/teardown windows), the 3 voice_field leaves
 *     0x49fe9/0x4a28c/0x4a54a (run-2 ears + the 5,756-call chain) + deactivate 0x4ac55 (2 identical
 *     A/B records from the batch dialogue-skip).
 *   - open_voices 0x47dae: veneer baseline /
 *     AB_VA=0x47dae isolate (native populated the 0x97440 table for the VENEER consumers' hardware
 *     lgs; no fn-0xa trap in the log = the service path drove the boot) / =native batch (native
 *     consumers over the software dpmi_sel_base path, teardown included) — all ears-clean. The one
 *     fade sighting appeared in the VENEER BASELINE too, so it does not implicate the native
 *     (tracked separately as the governor-tuning residual).
 *   - MIDI-router cluster 0x44e0d/0x4594d/0x45dc5/0x45f1d:
 *     (veneer / AB_VA 4-VA isolate / =native batch) over a music-flavored session — new game, a
 *     track change, settings volume change, load-triggered second track change — all ears-clean.
 *   - gdv_setup_voices 0x55440: ROTH_LIFT=gdv gate (veneer /
 *     AB_VA=0x55440 / =native) over the intro movies — ears-clean, and the =native trace shows the
 *     native ran 3x in the cut phase with the exact expected write-set ([0x91dc2]=+0x10 streaming
 *     arm, [0x91df0]=1, [0x97b6c]=1, [0x9187c]=0x118, [0x97cd4] handle++ per movie, ret 0). The
 *     OPEN arm (R staging -> open_voices -> per-buffer timer) is transcribed but in-game UNOBSERVED
 *     (the game always opens main audio first, so GDV always streams) — same unobserved-arm class
 *     as the MIDI alloc arm / open_voices jae arm.
 *   - alloc/free driver-slot 0x44553/0x44a81: (veneer /
 *     AB_VA isolate / =native) over boot→music→quit — ears-clean. Config signatures in the boot
 *     logs: the isolate run LOSES the MIDI fn0/fn1 dispatch-computer traps (the native stamps the
 *     vtable directly, no driver-load far-calls) while music still plays = ORIGINAL code far-called
 *     the native-stamped vtable successfully (the cross-lane proof); =native is fully trap-silent.
 *   Still PENDING (veneer default): voice_load_to_slot 0x4ad03 — "zero traced calls" is a TRACER
 *     BLIND SPOT, not dead code: its callers are the
 *     ISR-context callback chain (0x27501 -> 0x1e487 -> 0x15a92, deliberately un-swapped original
 *     bytes — lift_registry.c refuses int3 in ISR context), so the c2 dispatcher is never on the
 *     path; DBASE500.DAT sampling shows 98% of voice clips exceed one 32KB buffer, so it likely
 *     fires on virtually every voiced line. Same in-game-unreachable-bridge class as 0x4e066:
 *     its gate is the imgfree context, not an in-game A/B.
 *   driver_call_m4 0x44cad — OPL-only, zero traced calls (the genuinely-never-observed bucket). */
static int au_va_retired_default(uint32_t va)
{
    switch (va) {
    case 0x45d28u: case 0x4a641u:                               /* dispatch_simple / voice_start */
    case 0x48b21u: case 0x48f79u: case 0x48c6bu:                /* detection trio */
    case 0x48666u: case 0x47cf5u: case 0x47d6eu:                /* close/enable/disable */
    case 0x49fe9u: case 0x4a28cu: case 0x4a54au: case 0x4ac55u: /* voice_field leaves */
    case 0x47daeu:                                              /* open_voices */
    case 0x44e0du: case 0x4594du: case 0x45dc5u: case 0x45f1du: /* MIDI-router cluster */
    case 0x55440u:                                              /* gdv_setup_voices */
    case 0x44553u: case 0x44a81u:                               /* alloc/free driver-slot */
    case 0x49923u: case 0x49ca4u: case 0x4980du: case 0x498e9u: /* timer cluster */
        return 1;
    default:
        return 0;
    }
}

/* The single per-VA native decision consulted by every bridge dispatch site (see audio_trace.h).
 * Precedence: ROTH_AU_AB_VA bisect (when ROTH_AU_AB unset) > ROTH_AU_AB=native/veneer > the
 * RETIRED-DEFAULT hold default. Flags-unset behaviour is byte-identical to the prior hold. */
int au_ab_va(uint32_t va)
{
    if (g_au_ab_va_on) {                       /* per-VA bisect: ONLY listed VAs native */
        for (unsigned i = 0; i < g_au_ab_va_n; i++)
            if (g_au_ab_va[i] == va)
                return 1;
        return 0;
    }
    if (g_au_ab == 1)                          /* ROTH_AU_AB=native: all staged natives live */
        return 1;
    if (g_au_ab == 2)                          /* ROTH_AU_AB=veneer: force every veneer */
        return 0;
    return au_va_retired_default(va);          /* default hold: only the RETIRED-DEFAULT set */
}

/* ---- the record ring ---------------------------------------------------------------- */
struct au_trace_rec {
    uint32_t va;        /* SOS canon VA (the key) */
    uint32_t seq;       /* monotonic call counter */
    uint32_t mtick;     /* audio.c poll-tick timestamp (informational) */
    uint32_t irq_eip;   /* g_irq_eip at call: ISR-interleave witness (runtime linear) */
    uint32_t a[4];      /* the 4 marshalled Watcom arg regs (eax/edx/ebx/ecx) */
    uint32_t ret;       /* call_orig EAX (the veneer's return) */
    uint8_t  phase;     /* boot/map/sfx/cut/... tag (see au_phase) */
    uint8_t  branch;    /* predicate arm for this VA (see au_branch) */
    uint16_t snap_id;   /* index into g_au_snap (1:1 with the ring slot) */
};
#define AU_TR_N   8192u                    /* ~512 KB (40 B x 8192) */
#define AU_TR_MASK (AU_TR_N - 1u)
static struct au_trace_rec g_au_tr[AU_TR_N];
static volatile uint32_t   g_tr_alloc;     /* next slot to fill (enter) */
static volatile uint32_t   g_tr_commit;    /* fully-written watermark (exit bumps last) */
static uint32_t            g_tr_drained;    /* flushed watermark (drain only) */
static uint32_t            g_seq;           /* monotonic call counter */
static uint32_t            g_cached_mtick;  /* last audio.c poll tick (au_trace_tick) */

/* ---- the before/after snapshot pool ------------------------------------------------- */
/* Sized to the largest window-set: voice-start (0x4a641) and the 4 voice-field leaves
 * (0x4a54a/0x49fe9/0x4a28c/0x4ac55) each = 0x40 + 0xc0 + 0xd80 = 0xe80 (3712) with the runtime-resolved
 * 32-voice-struct window included. Every other VA is <= 0x220 (close_voices' gap set). At
 * 2*AU_SNAP_BYTES*AU_TR_N the pool is 64 MB, so it is NOT carried as BSS (that would dominate the
 * image's memory size). It is allocated ONCE from the trace-init path (au_trace_ctor) only when
 * ROTH_AU_TRACE is set, and stays NULL — with every access guarded — when tracing is off or the
 * one-time allocation fails. Indexed 1:1 with the ring (snap_id == ring slot). */
#define AU_SNAP_BYTES 4096u
struct au_snap { uint8_t b[AU_SNAP_BYTES], a[AU_SNAP_BYTES]; };
static struct au_snap *g_au_snap;  /* [AU_TR_N]; NULL until trace-init allocates it (see au_trace_ctor) */

/* One-time allocation of the before/after snapshot pool, called from the trace-init constructor when
 * ROTH_AU_TRACE is set. On failure the pool stays NULL and snapshotting is disabled (every access is
 * guarded); the ring and the rest of the trace still run. */
static void au_snap_pool_init(void)
{
    g_au_snap = calloc(AU_TR_N, sizeof *g_au_snap);
    if (!g_au_snap)
        LOGE("audio_trace: snapshot pool alloc (%u MB) failed — snapshotting disabled, ring still active\n",
             (unsigned)(((uint64_t)AU_TR_N * sizeof(struct au_snap)) >> 20));
}

/* Runtime-resolved window sentinel: a win.canon of AU_FARG_VOICES marks the 32 fn-0xa voice
 * structs (voice_start/voice_load write-set) — the base is not a fixed canon address but is resolved
 * at capture time from haudio_voice_struct_base() (mmap'd at driver-open). Its diff LABEL is
 * AU_FARG_LABEL (= VOICE_OFF, the far-args-segment offset of the voice-struct region), so entries read
 * as struct offsets. The sentinel is well above any real canon window base (all >= 0x74590) and below
 * OBJ space wrap; au_capture/emit_diff special-case it, so CANON() is never applied to it. */
#define AU_FARG_VOICES 0xf0000000u
#define AU_FARG_LABEL  0x40u

/* Per-VA snapshot windows + how the drain re-derives the (canon,size) of each diff. All canon except
 * a win.canon == AU_FARG_VOICES (runtime-resolved; see above). */
struct au_win  { uint32_t canon; uint16_t len; };
struct au_desc { uint32_t va; uint8_t nwin; struct au_win win[10]; };  /* max = close_voices' 10 */

/* windows table (canon). VAs not listed are still recorded (va/args/ret/branch/phase) with an
 * empty snapshot. Every window length is a multiple of 4 so the drain can diff in dwords. */
static const struct au_desc g_desc[] = {
    /* MIDI-router (1a): channel-volume state + the [0x951cc] device-remap gate. Plus the
     * raw-volume table 0x7372c (dev*0x10+chan, the CC7 store) + the 0x951b4/0x951c0 work buffers so a
     * paired A/B sees the native store-set of the alloc-mode==0 arm (the router-output message + the
     * scaled/mute volume) — the {0x951b4,0x10} window spans both 0x951b4..7 and restore/mute's 0x951c0..2. */
    { 0x44e0d, 6, { {0x93104,0x40},{0x93124,0x40},{0x970f4,0x40},{0x951cc,0x10},{0x7372c,0x40},{0x951b4,0x10} } },
    { 0x4594d, 6, { {0x93104,0x40},{0x93124,0x40},{0x970f4,0x40},{0x951cc,0x10},{0x7372c,0x40},{0x951b4,0x10} } },
    { 0x45dc5, 6, { {0x93104,0x40},{0x93124,0x40},{0x970f4,0x40},{0x951cc,0x10},{0x7372c,0x40},{0x951b4,0x10} } },
    { 0x45f1d, 6, { {0x93104,0x40},{0x93124,0x40},{0x970f4,0x40},{0x951cc,0x10},{0x7372c,0x40},{0x951b4,0x10} } },
    /* driver-load (1b): desc block 0x97b7c.., 16 selectors, buffer slot, play-pos, voice slot 0.
     * close_voices/enable/disable carry ADDED gap windows so the
     * paired A/B sees their full native write-set: close = the per-slot bookkeeping it zeroes; enable/
     * disable = the [0x97b30] armed-flag + the [0x7420c] service-descriptor strcpy target. */
    { 0x47dae, 6, { {0x97b7c,0x70},{0x9740c,0x40},{0x97420,0x08},{0x97800,0x08},{0x97440,0xc0},
                    {AU_FARG_VOICES,0xd80} } }, /* +the fn-0xa voice-struct zeroing (native A/B) */
    { 0x48666,10, { {0x97b7c,0x70},{0x9740c,0x40},{0x97420,0x08},{0x97800,0x08},{0x97440,0xc0},
                    {0x972a4,0x30},{0x9730c,0x30},{0x97374,0x30},{0x9739c,0x08},{0x97b1c,0x08} } },
    { 0x47cf5, 7, { {0x97b7c,0x70},{0x9740c,0x40},{0x97420,0x08},{0x97800,0x08},{0x97440,0xc0},
                    {0x97b30,0x04},{0x7420c,0x40} } },
    { 0x47d6e, 7, { {0x97b7c,0x70},{0x9740c,0x40},{0x97420,0x08},{0x97800,0x08},{0x97440,0xc0},
                    {0x97b30,0x04},{0x7420c,0x40} } },
    { 0x48b21, 5, { {0x97b7c,0x70},{0x9740c,0x40},{0x97420,0x08},{0x97800,0x08},{0x97440,0xc0} } },
    { 0x48c6b, 5, { {0x97b7c,0x70},{0x9740c,0x40},{0x97420,0x08},{0x97800,0x08},{0x97440,0xc0} } },
    { 0x48f79, 5, { {0x97b7c,0x70},{0x9740c,0x40},{0x97420,0x08},{0x97800,0x08},{0x97440,0xc0} } },
    /* timer (1c): the full 16-entry table columns + the [0x74590] service slot */
    { 0x49923, 3, { {0x979c4,0x80},{0x97a64,0x50},{0x74590,0x04} } },
    { 0x49ca4, 3, { {0x979c4,0x80},{0x97a64,0x50},{0x74590,0x04} } },
    { 0x4980d, 3, { {0x979c4,0x80},{0x97a64,0x50},{0x74590,0x04} } },
    { 0x498e9, 3, { {0x979c4,0x80},{0x97a64,0x50},{0x74590,0x04} } },
    /* voice-start (1d): selector table + the far-ptr table + the RUNTIME-RESOLVED 32 voice
     * structs (the real write-set). 0xd80 = VOICE_N(32)*VOICE_SZ(0x6c) — must match the span
     * haudio_voice_struct_base() reports; AU_FARG_VOICES = resolve the base at capture time. */
    { 0x4a641, 3, { {0x9740c,0x40},{0x97440,0xc0},{AU_FARG_VOICES,0xd80} } },
    { 0x4ad03, 3, { {0x9740c,0x40},{0x97440,0xc0},{AU_FARG_VOICES,0xd80} } },
    /* voice-field leaves (1e): the far-ptr field ops read/write the SAME 32 voice structs, so
     * they share voice-start's window set — the AU_FARG_VOICES window captures the xchg/deactivate RMW
     * write-set (get_w34 is a pure read). 0x4a54a get_w34 / 0x49fe9 xchg_w32 / 0x4a28c xchg_w54 /
     * 0x4ac55 deactivate. */
    { 0x4a54a, 3, { {0x9740c,0x40},{0x97440,0xc0},{AU_FARG_VOICES,0xd80} } },
    { 0x49fe9, 3, { {0x9740c,0x40},{0x97440,0xc0},{AU_FARG_VOICES,0xd80} } },
    { 0x4a28c, 3, { {0x9740c,0x40},{0x97440,0xc0},{AU_FARG_VOICES,0xd80} } },
    { 0x4ac55, 3, { {0x9740c,0x40},{0x97440,0xc0},{AU_FARG_VOICES,0xd80} } },
    /* driver-slot (1d): slot flags + the vtable (4 slots x stride 0x48). The trio (STOPPED) is widened
     * to the full disasm write-set for a future paired A/B if these are ever attempted: the slot
     * descriptor tables 0x9204c/0x92050/0x9206c/0x92080/0x92094/0x92098 (+slot*6) + the DPMI/type/active
     * block 0x920b4.. (incl. the NON-DETERMINISTIC handle 0x920dc + the type 0x920f0) + the 12-method
     * vtable 0x92f9c... alloc additionally writes the info descriptor 0x7f37c fields + the slot_out 0x7f3b0;
     * m4's a003 method-4 writes g_extmidi_out_callback 0x931b8. */
    { 0x44553, 6, { {0x9204c,0x58},{0x920b4,0x40},{0x92f9c,0x120},{0x7f37c,0x40},
                    {0x736b8,0x50},{0x951d4,0x08} } }, /* +chan-busy copy +[0x951d4/d8] zeroing (native) */
    { 0x44a81, 3, { {0x9204c,0x58},{0x920b4,0x40},{0x92f9c,0x120} } },
    { 0x44cad, 3, { {0x920b4,0x40},{0x92f9c,0x120},{0x931b8,0x08} } },
    { 0x45d28, 2, { {0x920b4,0x40},{0x92f9c,0x120} } },
    /* GDV-audio (1d): each window-set covers the VA's native/veneer write-set for the paired
     * ROTH_LIFT=gdv A/B. setup_voices (0x55440, the retired native): error 0x91d10 + the g_gdv init-flag
     * mask/enabled/df0 (0x91dbe..) + the [0x9187c] budget + [0x97b6c] + the handle/timer-handle block
     * 0x97cd0.. . load_drivers (0x55360, STOPPED/veneer): its error + the device descriptor block
     * 0x97b70..0x97bec + the dispatch far-ptr 0x97ce4. begin_playback (0x4e066, native): the
     * |0x40 flag bit + the GDV stream-flags region incl. the (dead) [0x91df2] store {0x91dc2,0x40} + the
     * ADPCM decoder accumulators 0x9189c/0x918a0 + the streaming-voice descriptor 0x97c5c.. + the voice
     * handle 0x97ce0. (The decode output buffer [0x91d4c] contents are a scratch area, not a fixed
     * global — no window; the native's audio correctness rides the ears.) */
    { 0x4e066, 4, { {0x91dc2,0x40},{0x9189c,0x08},{0x97c5c,0x40},{0x97cd4,0x20} } },
    { 0x55360, 3, { {0x91d10,0x08},{0x97b70,0x7c},{0x97ce4,0x08} } },
    { 0x55440, 5, { {0x91d08,0x10},{0x91dbe,0x40},{0x9187c,0x04},{0x97b6c,0x04},{0x97cd0,0x28} } },
};

static const struct au_desc *au_desc_for(uint32_t va)
{
    for (unsigned i = 0; i < sizeof g_desc / sizeof g_desc[0]; i++)
        if (g_desc[i].va == va)
            return &g_desc[i];
    return NULL;
}

/* memcpy the VA's snapshot windows (concatenated) into dst; returns the bytes used. Read-only on
 * mapped game memory — safe on the main thread inside the veneer (as safe as call_orig). */
static uint32_t au_capture(const struct au_desc *d, uint8_t *dst)
{
    uint32_t off = 0;
    if (!d)
        return 0;
    for (unsigned i = 0; i < d->nwin; i++) {
        uint16_t len = d->win[i].len;
        if (off + len > AU_SNAP_BYTES)
            break;
        if (d->win[i].canon == AU_FARG_VOICES) {
            /* Runtime-resolved: the 32 fn-0xa voice structs (voice_start/voice_load write-set). The
             * base is mmap'd (0xf3xxxxxx) and bound at driver-open, well before any voice call; if it
             * is not yet staged (base 0) or the window would exceed the staged span, snapshot zeros so
             * the before/after diff stays empty (no spurious change). */
            uint32_t span = 0;
            uint32_t base = haudio_voice_struct_base(&span);
            if (base && len <= span)
                memcpy(dst + off, (const void *)(uintptr_t)base, len);
            else
                memset(dst + off, 0, len);
        } else {
            memcpy(dst + off, (const void *)(uintptr_t)CANON(d->win[i].canon), len);
        }
        off += len;
    }
    return off;
}

/* ---- helpers: game-memory reads for the branch/phase bytes (canon) ------------------------- */
static inline uint32_t GP32(uint32_t canon) { return *(volatile uint32_t *)(uintptr_t)CANON(canon); }
static inline uint16_t GP16(uint32_t canon) { return *(volatile uint16_t *)(uintptr_t)CANON(canon); }
static inline uint8_t  GP8 (uint32_t canon) { return *(volatile uint8_t  *)(uintptr_t)CANON(canon); }

/* branch arm per disasm tell. bit set = the "hard"/non-trivial arm (documented per VA):
 *  - 0x44e0d : [0x951cc]==0 device-remap gate taken
 *  - voice/close (0x4a641/0x4ad03/0x48666) : [handle*4+0x9740c] >= 0xe106 (the jae/non-empty-slot
 *      arm — a SET bit on 0x4a641/0x4ad03 is the hard blocker on a trivial retire; flag loudly)
 *  - 0x44a81 : [slot*4+0x920b4]==0 early-out taken
 *  - timer VAs : byte[0x755b4]!=0 (the PIC-mask-guarded table-edit path active) */
static uint8_t au_branch(uint32_t va, uint32_t a0)
{
    switch (va) {
    case 0x44e0d:
        return GP32(0x951cc) == 0 ? 1 : 0;
    case 0x4a641:
    case 0x4ad03:
    case 0x48666:
        return GP32(0x9740c + (a0 & 0xff) * 4) >= 0xe106 ? 1 : 0;
    case 0x44a81:
        return GP32(0x920b4 + (a0 & 0xf) * 4) == 0 ? 1 : 0;
    case 0x49923:
    case 0x49ca4:
    case 0x4980d:
    case 0x498e9:
        return GP8(0x755b4) != 0 ? 1 : 0;
    default:
        return 0;
    }
}

/* phase byte. Cheap, best-effort — a log-slice convenience, not required to be perfect:
 *   boot  until the first midi_dispatch (0x44e0d); cut while a GDV cutscene's stream voice is up
 *   ([0x91dc2]&0x40) or the call itself is a GDV VA; sfx on the voice-start VAs; else map (the
 *   running default). save/quit are not auto-derived (documented). */
enum { PH_BOOT = 0, PH_MAP = 1, PH_SFX = 2, PH_CUT = 3, PH_SAVE = 4, PH_QUIT = 5 };
static const char *const g_ph_name[] = { "boot", "map", "sfx", "cut", "save", "quit" };
static int g_seen_midi;

static uint8_t au_phase(uint32_t va)
{
    if (va == 0x44e0d)
        g_seen_midi = 1;
    if (va == 0x4e066 || va == 0x55360 || va == 0x55440 || (GP16(0x91dc2) & 0x40))
        return PH_CUT;
    if (va == 0x4a641 || va == 0x4ad03)
        return PH_SFX;
    if (!g_seen_midi)
        return PH_BOOT;
    return PH_MAP;
}

/* ---- the two bridge taps (main-thread) ----------------------------------------------------- */
int au_trace_enter(uint32_t va, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    if (!g_au_on)                                    /* the single hot-path check when off */
        return -1;
    if (g_au_va_filter && va != g_au_va_filter)
        return -1;

    uint32_t slot = g_tr_alloc++ & AU_TR_MASK;       /* main thread is the only allocator */
    struct au_trace_rec *r = &g_au_tr[slot];
    const struct au_desc *d = au_desc_for(va);
    r->va = va;
    r->seq = g_seq++;
    r->mtick = g_cached_mtick;
    r->irq_eip = g_irq_eip;
    r->a[0] = a0; r->a[1] = a1; r->a[2] = a2; r->a[3] = a3;
    r->ret = 0;
    r->branch = au_branch(va, a0);
    r->phase = au_phase(va);
    r->snap_id = (uint16_t)slot;
    if (g_au_snap)
        au_capture(d, g_au_snap[slot].b);            /* before-snapshot (NULL pool => snapshotting off) */
    return (int)slot;
}

void au_trace_exit(int token, uint32_t ret)
{
    if (token < 0)
        return;
    uint32_t slot = (uint32_t)token;
    struct au_trace_rec *r = &g_au_tr[slot];
    r->ret = ret;
    if (g_au_snap)
        au_capture(au_desc_for(r->va), g_au_snap[slot].a); /* after-snapshot (NULL pool => off) */
    /* Publish: bump the committed watermark LAST so a drain preempting an in-flight call (during
     * call_orig) never flushes this record before ret + after-snapshot are stored. Single-core
     * signal preemption => a volatile store ordered last is sufficient (no true concurrency). */
    g_tr_commit = g_tr_alloc;
}

/* ---- the passive ISR-table sampler, on the shm_tick/SIGALRM beat -------------------- */
struct au_isr_sample {
    uint32_t seq, mtick, irq_eip;
    uint8_t  tbl[0xd4];   /* 0x979c4[0x80] + 0x97a64[0x50] + 0x74590[0x04] = the timer table */
};
#define AU_ISR_N   4096u
#define AU_ISR_MASK (AU_ISR_N - 1u)
static struct au_isr_sample g_au_isr[AU_ISR_N];
static volatile uint32_t    g_isr_commit;
static uint32_t             g_isr_drained;
static uint32_t             g_isr_seq;

/* The timer-table windows the sampler captures (same layout the drain decodes). */
static const struct au_win g_isr_win[] = { {0x979c4,0x80}, {0x97a64,0x50}, {0x74590,0x04} };

static void au_isr_sample(void)
{
    uint32_t slot = g_isr_seq & AU_ISR_MASK;
    struct au_isr_sample *s = &g_au_isr[slot];
    s->seq = g_isr_seq;
    s->mtick = g_cached_mtick;
    s->irq_eip = g_irq_eip;
    uint32_t off = 0;
    for (unsigned i = 0; i < sizeof g_isr_win / sizeof g_isr_win[0]; i++) {
        uint16_t len = g_isr_win[i].len;
        if (off + len > sizeof s->tbl)
            break;
        memcpy(s->tbl + off, (const void *)(uintptr_t)CANON(g_isr_win[i].canon), len);
        off += len;
    }
    g_isr_seq++;
    g_isr_commit = g_isr_seq;   /* publish last */
}

void au_trace_tick(uint32_t g_mtick)
{
    if (!g_au_on)
        return;
    g_cached_mtick = g_mtick;   /* record timestamp source (approx, ~70 Hz) */
    if (g_au_isr_on)
        au_isr_sample();
}

/* ---- the drain, MAGIC_POLL safe point ONLY ------------------------------------------ */
static int g_au_fd = -1;   /* -1 = not yet opened; -2 = open failed (don't retry) */

/* tiny async-signal-safe formatters (no stdio in the drain either). */
static char *put_str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *put_hex(char *p, uint32_t v)          /* minimal-width lowercase hex */
{
    char t[8]; int n = 0;
    do { t[n++] = "0123456789abcdef"[v & 0xf]; v >>= 4; } while (v);
    while (n) *p++ = t[--n];
    return p;
}
static char *put_hex8(char *p, uint32_t v)         /* fixed 8-digit hex (arg/ret columns) */
{
    for (int sh = 28; sh >= 0; sh -= 4) *p++ = "0123456789abcdef"[(v >> sh) & 0xf];
    return p;
}
static char *put_dec(char *p, uint32_t v)
{
    char t[10]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    return p;
}
/* irq_eip / any runtime linear -> canon for the log (matches the docs' canon convention). */
static char *put_canon(char *p, uint32_t lin)
{
    if (lin >= OBJ_DELTA)
        return put_hex(p, lin - OBJ_DELTA);
    return put_hex(p, lin);
}

/* One drain line is built in this static buffer (au_trace_drain is single-entry at MAGIC_POLL and
 * never nests — the formatters read only host memory, so they cannot fault). A single dword-diff
 * token is ~28 B and a window-set is at most AU_SNAP_BYTES/4 dwords, so this comfortably bounds the
 * worst case; every append still guards LIM so we can never overrun. */
#define AU_LINE 4096u
#define AU_LIM  (AU_LINE - 40u)   /* leave slack for the trailing token + '\n' */
static char g_line[AU_LINE];

/* Emit the before!=after diff of one record's windows as "canon:old->new" dword entries. A window
 * whose canon is AU_FARG_VOICES (the runtime voice-struct window) is labelled with AU_FARG_LABEL —
 * i.e. its diff entries read as far-args-segment offsets (0x40 + slot*0x6c + field), NOT canon. */
static char *emit_diff(char *p, const struct au_trace_rec *r)
{
    const struct au_desc *d = au_desc_for(r->va);
    if (!d || !g_au_snap)                     /* no descriptor, or the snapshot pool was never allocated */
        return p;
    const uint8_t *b = g_au_snap[r->snap_id].b;
    const uint8_t *a = g_au_snap[r->snap_id].a;
    uint32_t off = 0;
    for (unsigned i = 0; i < d->nwin; i++) {
        uint32_t base = d->win[i].canon == AU_FARG_VOICES ? AU_FARG_LABEL : d->win[i].canon;
        uint16_t len = d->win[i].len;
        if (off + len > AU_SNAP_BYTES)
            break;
        for (uint16_t k = 0; k + 4 <= len; k += 4) {
            uint32_t ov, nv;
            memcpy(&ov, b + off + k, 4);
            memcpy(&nv, a + off + k, 4);
            if (ov != nv) {
                if ((uint32_t)(p - g_line) >= AU_LIM) { *p++ = ' '; *p++ = '.'; *p++ = '.'; return p; }
                *p++ = ' ';
                p = put_hex(p, base + k);
                *p++ = ':';
                p = put_hex8(p, ov);
                *p++ = '-'; *p++ = '>';
                p = put_hex8(p, nv);
            }
        }
        off += len;
    }
    return p;
}

static void au_flush(const char *buf, uint32_t n)
{
    if (g_au_fd == -2 || n == 0)
        return;
    if (g_au_fd == -1) {
        g_au_fd = open(g_au_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (g_au_fd < 0) { g_au_fd = -2; return; }
    }
    (void)!write(g_au_fd, buf, n);
}

void au_trace_drain(void)
{
    if (!g_au_on)
        return;

    /* records: flush [g_tr_drained, committed). Line: va seq mtick irq_eip a0..a3 ret phase branch
     * | canon:old->new ...  (one record per line). Each line fits a fixed buffer comfortably. */
    uint32_t commit = g_tr_commit;
    while (g_tr_drained != commit) {
        uint32_t slot = g_tr_drained & AU_TR_MASK;
        const struct au_trace_rec *r = &g_au_tr[slot];
        char *p = g_line;
        p = put_hex(p, r->va);            *p++ = ' ';
        p = put_dec(p, r->seq);           *p++ = ' ';
        p = put_dec(p, r->mtick);         *p++ = ' ';
        p = put_canon(p, r->irq_eip);     *p++ = ' ';
        for (int i = 0; i < 4; i++) { p = put_hex8(p, r->a[i]); *p++ = ' '; }
        p = put_hex8(p, r->ret);          *p++ = ' ';
        p = put_str(p, g_ph_name[r->phase < 6 ? r->phase : 1]); *p++ = ' ';
        p = put_dec(p, r->branch);
        *p++ = ' '; *p++ = '|';
        p = emit_diff(p, r);
        *p++ = '\n';
        au_flush(g_line, (uint32_t)(p - g_line));
        g_tr_drained++;
    }

    /* ISR samples: emit each new sample as a diff vs the previously-emitted sample (the ISR's
     * accumulator/fire-flag advances). First sample after (re)start emits full. */
    static uint8_t last[sizeof g_au_isr[0].tbl];
    static int have_last;
    uint32_t icommit = g_isr_commit;
    while (g_isr_drained != icommit) {
        uint32_t slot = g_isr_drained & AU_ISR_MASK;
        const struct au_isr_sample *s = &g_au_isr[slot];
        char *p = g_line;
        p = put_str(p, "isr ");
        p = put_dec(p, s->seq);           *p++ = ' ';
        p = put_dec(p, s->mtick);         *p++ = ' ';
        p = put_canon(p, s->irq_eip);
        *p++ = ' '; *p++ = '|';
        uint32_t off = 0;
        for (unsigned i = 0; i < sizeof g_isr_win / sizeof g_isr_win[0]; i++) {
            uint32_t base = g_isr_win[i].canon;
            uint16_t len = g_isr_win[i].len;
            for (uint16_t k = 0; k + 4 <= len && off + k + 4 <= sizeof s->tbl; k += 4) {
                uint32_t nv;
                memcpy(&nv, s->tbl + off + k, 4);
                uint32_t ov = 0;
                if (have_last)
                    memcpy(&ov, last + off + k, 4);
                if (!have_last || ov != nv) {
                    if ((uint32_t)(p - g_line) >= AU_LIM) { *p++ = ' '; *p++ = '.'; *p++ = '.'; break; }
                    *p++ = ' ';
                    p = put_hex(p, base + k);
                    *p++ = ':';
                    p = put_hex8(p, nv);
                }
            }
            off += len;
        }
        *p++ = '\n';
        au_flush(g_line, (uint32_t)(p - g_line));
        memcpy(last, s->tbl, sizeof last);
        have_last = 1;
        g_isr_drained++;
    }
}
