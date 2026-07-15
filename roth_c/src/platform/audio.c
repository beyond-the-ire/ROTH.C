/* Virtual HMI digital-audio driver — see audio.h for the overall design.
 *
 * Phase 1 (this file, initial): intercept the SOS "dispatch-computer" (canon
 * 0x4fcd3, which normally returns {offset, code-selector} for the loaded
 * .386 driver) and hand back a MAGIC far-pointer instead. Every later
 * `call far [dispatch]` (EAX = function number) then faults at MAGIC_OFF; we
 * log it here so the runtime reveals the exact init/playback call sequence,
 * then implement each function. No real .386 is executed.
 */
#include "audio.h"
#include "audio_trace.h"   /* au_trace_tick (ISR sampler) + au_trace_drain hooks */
#include "shared_audio.h"
#include "shared_midi.h"
#include "shared_fb.h"
#include "g_names.h"   /* VA_<global> canon-VA constants for readable G-macro sites (generated) */
#include "sys/sys.h"   /* per-OS seam: low (32-bit-addressable) allocation */
#include <time.h>
#include <sys/time.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CANON(x) ((uint32_t)((x) + OBJ_DELTA))

/* The SOS routine that builds a driver-dispatch far-pointer from a loaded
 * driver segment. We int3 its entry and return MAGIC instead. */
#define HMI_DISPATCH_COMPUTER CANON(0x4fcd3)

/* Music (MIDI) init entry. Returns an error code in EAX; the caller (0x448a0)
 * skips music entirely when EAX != 0. MIDI/OPL is a later phase, so we stub
 * this to "no card" unless ROTH_MIDI is set. Avoids the null MIDI-driver
 * far-calls (per-tick player 0x49f60, track-start 0x44996). */
#define HMI_MUSIC_INIT CANON(0x51681)

/* Driver dispatch calls land here. Unmapped linear address; the selector is a
 * flat code selector (base 0), so `call far {MAGIC_OFF, CS}` page-faults
 * fetching at MAGIC_OFF and we catch it. */
#define MAGIC_OFF 0xe0d10000u

/* Digital "poll" callbacks land here. fn 0xa returns these function pointers;
 * the unified audio service (0x49eaf, run by the HMI timer) far-calls them each
 * tick. Distinct page from MAGIC_OFF so we can tell a poll from a fn-dispatch.
 * A small range so the 3 callbacks fn 0xa returns are individually identifiable. */
#define MAGIC_POLL 0xe0d20000u

/* Return trampoline for a host-initiated far-call into game code (the streaming
 * buffer-complete handler). After the callee's retf lands here we pop its cdecl
 * args and resume the original driver-callback far-return. */
#define MAGIC_AFTER 0xe0d30000u

/* MIDI driver fns. The MIDI driver (HMIMDRV.386, card 0xa004) is virtualized
 * like the digital one: its fn=1 returns a 12-entry function table which the SOS
 * installs into the per-channel handler table (0x92fa2) and far-calls per event.
 * We hand back 12 pointers into this page (MAGIC_MIDI + i*4) so every MIDI event
 * faults here and we capture it (handler index = (eip - MAGIC_MIDI)/4).
 * Distinct page so it's unambiguous vs the digital dispatch. */
#define MAGIC_MIDI 0xe0d40000u
#define MIDI_NFN   12u

/* SB16 "16 ST" card descriptor (HMI card id 0xe018 = CONFIG.INI SoundCard),
 * lifted verbatim from DIGI/HMIDET.386 @ file offset 0x652a (106 bytes). The
 * SOS detect (driver fn 2) returns this; the thunk copies it and patches the 4
 * driver-fnptr selectors at +0x44/+0x4c/+0x54/+0x5c. Fields: +0x00 name
 * "SB16 16 ST", +0x24 bits=16, +0x2c min_rate=4000, +0x30 max_rate=44000,
 * +0x40..+0x5f four driver fnptrs (offs 0xf1/0xfb/0x109/0x113), +0x64 id 0xe018. */
static const uint8_t SB16_DESC[0x6a] = {
    0x53,0x42,0x31,0x36,0x20,0x31,0x36,0x20,0x53,0x54,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0xa0,0x0f,0x00,0x00,
    0xe0,0xab,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x00,0x00,0x00,
    0xf1,0x00,0x00,0x00,0x00,0xc0,0x00,0x00,0xfb,0x00,0x00,0x00,0x00,0xc0,0x00,0x00,
    0x09,0x01,0x00,0x00,0x00,0xc0,0x00,0x00,0x13,0x01,0x00,0x00,0x00,0xc0,0x00,0x00,
    0x01,0x00,0x00,0x00,0x18,0xe0,0x00,0x00,0x03,0x10,
};
#define DESC_OFF 0x200u /* where we stage the descriptor in the work buffer
                         * (must fit the smallest far-args seg; fn8's is 1 KB) */
/* MIDI fn=0/fn=1 stage their result (descriptor / 12-entry table) at the work
 * offset the thunk already put in EDI — that's the caller's in-limit area, so we
 * don't have to know segment 0xa004's size. */

/* DMA output buffer the SOS mixer renders PCM into (returned by driver fn 0xa).
 * The mixer writes via a flat data selector at the linear offset we return, so
 * a host-resident buffer works: the game writes here, we read it for SDL.
 * 3 sub-buffers of 0x2000 (the requested size) for the SOS's multi-buffering. */
#define DMA_SUB   0x2000u
#define DMA_NSUB  3
static uint8_t g_dma_buf[DMA_SUB * DMA_NSUB];
static uint32_t g_dma_lin;   /* flat linear base of g_dma_buf */
static uint32_t g_dma_reqsz; /* size the SOS asked for (fn 0xa ECX) */

/* PCM output ring to the viewer (SDL audio). The HMI mixer renders at the timer
 * rate (70 Hz); each digital poll we copy one tick's PCM (rate/70 frames) from
 * the mixer buffer into the ring. Format defaults match the observed 8-bit
 * unsigned mono mix; override by ear with ROTH_AUDIO_{RATE,BITS,CH,CHUNK,SRC}. */
static struct roth_audio *g_au;
static struct roth_midi *g_midi; /* MIDI-event ring -> viewer SoundFont synth */
static uint32_t g_au_rate = 22050, g_au_bits = 16, g_au_ch = 2, g_au_chunk;
static int g_au_src_edi; /* 0 = tap ESI (mixed PCM), 1 = tap EDI */

/* fn 0xa hands the SOS three values inside the caller's far-args segment
 * (fargSel): EDI = output callback (code), ESI = base of 32 voice structs
 * (0x6c each; 0x4fd30 spreads them into the voice table 0x97440), ECX = a
 * play-position dword (stored to 0x97800, read by the SOS at 0x49fc2 and the
 * mixer at 0x4a5f9). We carve those out of fargSel at fixed offsets. Skip a
 * small low header so we never tread on anything the caller left below us. */
#define VOICE_OFF 0x40u
#define VOICE_N   32u
#define VOICE_SZ  0x6cu
#define POS_OFF   (VOICE_OFF + VOICE_N * VOICE_SZ) /* 0x40 + 0xd80 = 0xdc0 */
static uint32_t g_farg_base; /* linear base of the fn 0xa far-args segment */
static uint32_t g_pos_lin;   /* linear address of the play-position dword */

/* Streaming-completion driver (experimental, env-gated). The intro movie streams
 * digital audio and waits on a buffer-complete event that a real SB16 IRQ5 would
 * raise; we have no card, so nothing fires it and the movie hangs. When
 * ROTH_STREAM_FN is set we far-call that canon handler (default the IRQ-mode
 * buffer-complete 0x4e394) every ROTH_STREAM_DIV-th driver tick. */
static uint32_t g_drive_fn;        /* runtime addr of the handler, 0 = disabled */
static uint32_t g_drive_div;       /* fixed ticks/fire override; 0 = auto-pace */
static int g_in_drive;             /* re-entrancy guard while the handler runs */
static int g_after_stream;         /* MAGIC_AFTER: 1=from stream drive (do the
                                    * decode-buffer copy), 0=from an SFX done-cb */

/* Clean-C SFX mixer state. The .386 driver used to mix the voice table into the
 * DMA buffer; we replaced the driver, so in-game sounds (voices in 0x97440) are
 * loaded but never played. We now mix them ourselves: 16-bit mono samples
 * (voice +0x00 ptr, +0x18 byte length, +0x32 volume 0..0x7fff) -> the 16-bit
 * stereo ring. g_vcur = our per-voice play cursor (sample index); g_vact = was
 * the voice active last tick (to reset the cursor on a fresh start). */
static uint32_t g_vcur[VOICE_N];
static uint32_t g_vstart[VOICE_N];
static uint8_t g_vact[VOICE_N];
static uint8_t g_vended[VOICE_N];   /* sample hit its end; done-cb queued, awaiting resolve */
static int      g_vsmooth = 1;      /* ROTH_VOL_SMOOTH: read-side +0x32 decrease governor (default on) */
static uint32_t g_vsmooth_fps = 10; /* the DOS-cadence model (slew calls/sec). Tunable:
                                     * ROTH_VOL_SMOOTH=<fps> (>=2). DOSBox comparison showed no
                                     * perceptible fade on the ~0.7s clank -> period render fps was
                                     * low; 10 gives ~12%% fade over 0.7s (inaudible), 20 gave ~50%%. */
static uint16_t g_veff[VOICE_N];    /* governor: rate-limited effective mix volume (host-only shadow) */
static uint8_t  g_veff_valid[VOICE_N]; /* governor: shadow seeded for this voice instance */
static uint32_t g_vgen[VOICE_N];    /* per-slot generation: ++ on every detected (re)start.
                                     * A queued done-cb records the gen it was queued at; a
                                     * bump means the slot was recycled -> the cb is stale
                                     * (see the delivery-site guard). Mirrors the .386 driver's
                                     * synchronous ISR delivery, which could never target a
                                     * slot the game had already re-owned. */
static uint32_t g_vlogptr[VOICE_N]; /* last sample ptr logged per voice (debug) */
static int g_cb_voice = -1;         /* voice whose done-cb is the in-flight MAGIC_AFTER */
static uint32_t g_cb_action;        /* the type (0/2) we fired -> MAGIC_AFTER resolution */
static int g_stream_fed; /* ticks since a movie block was shipped (SFX yield to it) */

/* When an SFX sample ends, the game expects its per-voice done-callback (+0x3c)
 * to fire: callback(device, action=0, voice). Without it the game never frees
 * its sound handles (SFX stop after a few) and never advances speech / clears
 * subtitles. We queue finished voices and fire one callback per tick (via the
 * MAGIC_AFTER trampoline). g_sfx_dev = the SOS device index of our voices,
 * found by the VOICE_OFF (0x40) signature we returned from fn 0xa. */
static int g_sfx_dev = -1;
static unsigned long g_mtick; /* MAGIC_POLL tick counter (debug timestamps) */
/* Live game code selector, captured on any audio dispatch/callback fault (audio_trap). The
 * open-driver service (haudio_open_voices_service) stamps it into the poll-cb far-ptr the timer ISR
 * far-calls, so the CS load succeeds; 0x23 (the DOS/4GW flat ring-3 code selector, the r1-observed
 * cb-sel + dpmi.c's convention) is the pre-first-fault fallback. */
static uint16_t g_game_cs;
static int g_pcm_fd = -1;     /* ROTH_PCM_DUMP: raw 16-bit-stereo capture of ring writes */
#define SFX_CUSHION 4096u /* ring bytes to keep buffered for SFX (~46ms @22k st) */
/* Read-side volume-slew governor. The original slews an SFX's applied
 * volume by <=0x200 per update_active_sounds call (0x27b05, clamp_diff_200); on DOS that runs once
 * per render frame (~15-30 fps). SFX_DOS_FPS models that per-frame cadence so the host can rate-limit
 * the DECREASE of the volume it READS from voice+0x32 to the DOS-equivalent slope. 20 is a mid-range
 * DOS render pace; the mixer tick rate is g_au_rate/70 samples per tick (70 ticks/sec). */
#define SFX_DOS_FPS 20u
#define SFX_CBQ 16
static struct {
    uint32_t off;
    uint16_t voice;
    uint32_t gen;   /* g_vgen[voice] at queue time — identity of the ended sound */
    uint32_t start; /* [voice+0x00] at queue time — secondary identity check */
    uint16_t qtag;  /* word[voice+0x34] at queue time = the handle-record index. The game cb
                     * (0x27501) reads THIS field LIVE off the slot and acts on
                     * rec = 0x83ed4 + tag*0x9a. Queue entries carry no other identity, so a
                     * SAME-SAMPLE reuse (same start, gen unbumped on a cushion tick) slips the
                     * (bit15/gen/start) guard while the LIVE tag has already flipped to the
                     * new occupant's record — the residual clank kill. Captured here so
                     * delivery can compare and refuse to run the cb against a re-owned slot. */
    uint8_t  qfree; /* the ENDED sound's cb would run the action-2 arm (0x275a6: st32(rec,0),
                     * freeing its record). Captured at queue time (the OLD node-header
                     * heuristic, while the slot still holds the ended sound); used to reclaim
                     * the old record host-side when the cb can't be delivered. 0 = loop/speech
                     * (the cb requeues / forwards; it never frees the record). */
} g_cbq[SFX_CBQ];
static unsigned g_cbq_head, g_cbq_tail;

static int sfx_device(void)
{
    if (g_sfx_dev < 0)
        for (int d = 0; d < 16; d++)
            if (*(uint32_t *)(uintptr_t)CANON(0x97440 + d * 0xc0) == VOICE_OFF) {
                g_sfx_dev = d;
                break;
            }
    return g_sfx_dev;
}

static int g_alog = 1;   /* init/setup audio logs (one-time milestones; on by default) */
static int g_alog_v = 0; /* ROTH_LOG_AUDIO (alias ROTH_AUDIO_LOG): per-tick/per-event audio
                          * trace (voicedump / voice on-off / poll / sfx done-cb / driver-call
                          * counter / DMA-changed). SPAMMY -> off by default; opt in to debug. */

#define ALOG(...)                                                              \
    do {                                                                       \
        if (g_alog)                                                            \
            fprintf(stderr, "[audio] " __VA_ARGS__);                           \
    } while (0)

/* verbose (spammy) per-tick/per-event audio trace; only when ROTH_LOG_AUDIO=1. */
#define ALOGV(...)                                                             \
    do {                                                                       \
        if (g_alog && g_alog_v)                                                \
            fprintf(stderr, "[audio] " __VA_ARGS__);                           \
    } while (0)

/* ===== VOLUME-ZERO TRANSITION TRACER (ROTH_VOL_TRACE=1) ============================================
 * Pure OBSERVATION of the clank-clip bug. The host mixer scales each voice's
 * PCM by word[voice+0x32]; F2 shows that word driven to 0 within ~20 ticks of a clank ON while the
 * voice stays active -> silent mix. The prior investigator theorized about the zero's ORIGIN but never
 * OBSERVED it. This tracer captures the ground truth: it shadows word[voice+0x32] every mixer entry,
 * rings the per-voice change history (so a ~0x200-step slew is distinguishable from a single jump =
 * a different writer), and on the nonzero->zero (or >0x400 one-step drop) transition emits a full
 * evidence dump of the voice, its owning sound record, the compute_sound_volume_pan ingredients, and a
 * read-only HOST recomputation of compute's three zero paths.
 *
 * Record<->voice linkage (verified in the lifted C, offsets cited):
 *   voice->record: word[voice+0x34] = the record tag/index the game done-cb reads LIVE (vs_fill
 *       os_audio.c:238 `SW(0x34, VW(0x18))`; cb 0x27501 -> rec = 0x83ed4 + tag*0x9a).
 *   record->voice: dword[rec+0x18] = the mixer voice SLOT INDEX. update_active_sounds
 *       (lift_audio.c:1000) calls sos_voice_set_w32(ld32u(rec+0x18), applied); the native xchg_w32
 *       (os_audio.c:377-385) resolves voice_field_slot(bank, rec+0x18) -> the SAME struct at
 *       g_farg_base+VOICE_OFF+(rec+0x18)*VOICE_SZ and writes word[+0x32]. The deactivate range-guard
 *       `voice >= 0x20 -> ret 0xa` (os_audio.c:406) confirms rec+0x18 is a 0..31 slot index.
 * So for mixer voice v: primary owner = record[word[vp+0x34]]; cross-checked by scanning the 16
 * records for rec+0x00!=0 && dword[rec+0x18]==v.
 *
 * Record layout (compute_sound_volume_pan lift_audio.c:65-105 + update_active_sounds:974-1010):
 *   rec+0x00 dword node/sample ptr · +0x04 byte flags (0x80 own-coords, 0x20 static-skip) ·
 *   +0x06 byte zone side (1-based; 0 = no zone attenuation) · +0x08 dword dist^2 in · +0x0c dword
 *   dist out (compute writes it) · +0x10 word applied vol · +0x12 word target vol · +0x14/+0x16 own
 *   int16 coords · +0x18 dword voice-slot handle. Sample struct (*(rec+0)): +0xa u16 range,
 *   +0x10 u8 sample-vol(/64). Globals: master [0x71d84]; player pos [0x90a8c]/[0x90a94] (>>16);
 *   zone list [0x85c48]; wall block = [0x85c48] + side*0x20 - 0x20, count int16 @blk, rects @blk+2
 *   stride 0xa (byte0 attenuation<<7, byte1&1 inverted-rect flag, 4 int16 bounds @+2/+4/+6/+8). */
static int g_vtrace; /* ROTH_VOL_TRACE: volume-zero transition tracer (pure observation) */
#define VOL_RING 32u
struct vol_change {
    unsigned long tick; /* g_mtick at the observing mixer scan */
    uint16_t oldv;      /* word[vp+0x32] at the previous scan */
    uint16_t newv;      /* word[vp+0x32] now */
    uint16_t wc;        /* inferred min game-side writes this interval = ceil(|delta|/0x200) */
};
static uint16_t g_vshadow_vol[VOICE_N];        /* last word[vp+0x32] seen per voice */
static uint8_t  g_vshadow_valid[VOICE_N];      /* shadow established (voice active & tracked) */
static uint32_t g_vt_ptr[VOICE_N];             /* last dword[vp+0x00] seen (restart/reuse signal) */
static uint16_t g_von_vol[VOICE_N];            /* word[vp+0x32] snapshot at VOICE ON */
static struct vol_change g_vring[VOICE_N][VOL_RING];
static uint8_t  g_vring_head[VOICE_N];          /* next write slot in the ring */
static uint8_t  g_vring_cnt[VOICE_N];           /* valid entries (<= VOL_RING) */
static unsigned g_vt_changes[VOICE_N];          /* total observed changes since ON */

/* ===== §MIX-TRACE: per-tick governor-efficacy tracer (ROTH_MIX_TRACE=1) =============================
 * The ROTH_VOL_TRACE ring above shadows only the RAW word[+0x32]; it runs before the mixer and knows
 * nothing about the governor. This tracer sits INSIDE audio_mix_sfx and logs, per tick for each
 * ACTIVELY-MIXED sfx voice, the raw word[+0x32], the governor output `eff`, and g_veff_valid — so a
 * single run reveals whether eff is CLAMPED as designed (eff > raw, descending at the DOS
 * slope, => fps-sensitive) or TRACKS raw (eff == raw: the governor is being re-seeded/bypassed, OR the
 * raw word already descends slower than the clamp => fps-INSENSITIVE, which is exactly the
 * "6/10/15 identical" report). It also stamps every g_veff_valid reset with its reason and whether it
 * fired MID-PLAY (a reset misfire on a voice we were already mixing makes every ROTH_VOL_SMOOTH fps
 * sound identical). Pure observation: reads voice state + the host governor shadow, writes NO game
 * memory. Emits a line only when raw or eff CHANGES for a voice (the descent shape, compactly), plus
 * every reset edge, under a global line cap. */
static int      g_mixtrace;                     /* ROTH_MIX_TRACE: per-tick governor-efficacy tracer */
static uint16_t g_mt_lastraw[VOICE_N];          /* last raw word[+0x32] logged per voice */
static int32_t  g_mt_lasteff[VOICE_N];          /* last governed eff logged per voice */
static uint8_t  g_mt_seen[VOICE_N];             /* voice was actively mixed+logged last tick (edge) */
static unsigned g_mt_lines;                     /* global emitted-line counter (cap) */
#define MIX_TRACE_MAX 6000u
/* Edge-triggered reset logger: called at each g_veff_valid[v]=0 site. mid = the voice was actively
 * being mixed (a mid-play misfire, the fps-insensitivity smoking gun) vs a benign end-of-life drop. */
static void mixtrace_reset(unsigned v, const char *why, int mid)
{
    if (g_mixtrace && (mid || g_mt_seen[v]) && g_mt_lines < MIX_TRACE_MAX) {
        g_mt_lines++;
        fprintf(stderr, "[audio] MIXTRACE t%lu v%u RESET veff_valid=0 (%s)%s\n",
                g_mtick, v, why, mid ? "  <== MID-PLAY" : "");
    }
    if (!mid)
        g_mt_seen[v] = 0; /* end-of-life: forget so the next onset re-logs; restart keeps mixing */
}

static int g_defer_midi;
static int g_midi_log_en; /* ROTH_MIDI_LOG: per-message MIDI log (debug; lags) */

/* ===== §SPEECH-SKIP TRACER (ROTH_SPEECH_TRACE=1) — dialogue-voice interrupt/skip observation ========
 * The reported P3: queue two dialogue lines, click to skip line 1, and line 2 starts "from ~the second
 * word" — present WITHOUT lifts (host + original bytes). This tracer is the OBSERVATION arm
 * (pin the mechanism with
 * evidence before touching anything). The dialogue voice is a fixed two-buffer ping-pong: bufA
 * [0x8201c] / bufB [0x82020] (allocated once at [0x8548c]/[0x85490], STABLE across every clip), remain
 * [0x82018], counts [0x82024]/[0x82028], refill-pending [0x820b9], file [0x82010], stream-state
 * [0x8200c] (0 idle / 1 playing / 2 end), line-active [0x81e3e]. The skip runs (game side, original
 * bytes) try_interrupt_dialogue_voice 0x18a2a -> dialogue_voice_force_end 0x1f671, which sos_stop_voice's
 * the slot (deactivate 0x4ac55: clears bit15 + zeros +0x34) and sets state=2; the next pump resets
 * state 2->0 and advance_dialogue_action_queue -> prime_voice_clip 0x1e54d re-submits line 2 into the
 * SAME slot with +0x00 = bufA start. This tracer prints, SPEECH-VOICE-ONLY (tag +0x34 == 0xedXX) and
 * read-only: every stream-state transition, every host mixer restart (WITH which condition fired —
 * !g_vact / start-changed / rewound — and the g_vcur it reset), deactivate edges, buffer-end done-cb
 * queues, the done-cb action decision (+ the eof/ended statics + remain), any stale/retagged drop, and
 * the MAGIC_AFTER resolution. One skip -> the exact host-side ordering of line 2's onset.
 * Zero-cost when off (every site guards on g_sptrace); writes NO game memory. */
static int      g_sptrace;              /* ROTH_SPEECH_TRACE: dialogue-voice skip tracer (observation) */
static uint8_t  g_vspeech[VOICE_N];     /* voice was a speech (tag 0xedXX) voice when last seen active */
static unsigned g_sp_lines;             /* emitted-line cap (runaway guard) */
#define SP_TRACE_MAX 8000u
#define SPLOG(...)                                                             \
    do {                                                                       \
        if (g_sptrace && g_sp_lines < SP_TRACE_MAX) {                          \
            g_sp_lines++;                                                       \
            fprintf(stderr, "[audio] SPEECH t%lu ", g_mtick);                  \
            fprintf(stderr, __VA_ARGS__);                                      \
        }                                                                      \
    } while (0)

/* Read-only snapshot of the dialogue-voice stream globals (canon addrs from lift_audio.c's speech
 * client). Emitted alongside each SPLOG event so a single skip capture shows the full stream state at
 * every transition. Reads game globals only (mapped MAP_FIXED); no writes. */
static void sptrace_globals(const char *ev)
{
    if (!g_sptrace)
        return;
#define SPG32(a) (int)*(const volatile uint32_t *)(uintptr_t)CANON(a)
    fprintf(stderr,
            "[audio] SPEECH t%lu   %-12s state[8200c]=%d rem[82018]=%d refill[820b9]=%d "
            "file[82010]=%08x bufA[8201c]=%08x bufB[82020]=%08x cntA[82024]=%d cntB[82028]=%d "
            "busy[83aea]=%d lineActive[81e3e]=%d\n",
            g_mtick, ev, SPG32(0x8200c), SPG32(0x82018), SPG32(0x820b9),
            (uint32_t)SPG32(0x82010), (uint32_t)SPG32(0x8201c), (uint32_t)SPG32(0x82020),
            SPG32(0x82024), SPG32(0x82028), SPG32(0x83aea), SPG32(0x81e3e));
#undef SPG32
}

static void env_u(const char *name, uint32_t *v)
{
    const char *s = getenv(name);
    if (s) {
        long x = strtol(s, NULL, 0);
        if (x > 0)
            *v = (uint32_t)x;
    }
}

static void audio_shm_setup(void)
{
    env_u("ROTH_AUDIO_RATE", &g_au_rate);
    env_u("ROTH_AUDIO_BITS", &g_au_bits);
    env_u("ROTH_AUDIO_CH", &g_au_ch);
    const char *src = getenv("ROTH_AUDIO_SRC");
    if (src && src[0] == 'e' && src[1] == 'd')
        g_au_src_edi = 1;
    g_au_chunk = (g_au_rate / 70u) * g_au_ch * (g_au_bits / 8u);
    env_u("ROTH_AUDIO_CHUNK", &g_au_chunk);

    /* Streaming buffer-complete handler: default to the IRQ-mode handler
     * (canon 0x4e394) — intro/movies stall without something firing it, since
     * we emulate no SB16 IRQ5. ROTH_STREAM_FN overrides (0 disables). */
    uint32_t sfn = 0x4e394;
    const char *sf = getenv("ROTH_STREAM_FN");
    if (sf)
        sfn = (uint32_t)strtoul(sf, NULL, 0);
    g_drive_fn = sfn ? CANON(sfn) : 0;
    env_u("ROTH_STREAM_DIV", &g_drive_div); /* 0 = auto-pace to sample rate */
    ALOG("stream driver: %s canon %#x pacing=%s\n",
         g_drive_fn ? "on" : "disabled", sfn,
         g_drive_div ? "fixed(ROTH_STREAM_DIV)" : "auto");

    int fd = shm_open(ROTH_AUDIO_SHM_NAME, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        ALOG("audio shm_open failed: no sound output\n");
        return;
    }
    if (ftruncate(fd, sizeof(struct roth_audio)) != 0) {
        close(fd);
        return;
    }
    g_au = mmap(NULL, sizeof(struct roth_audio), PROT_READ | PROT_WRITE,
                MAP_SHARED, fd, 0);
    close(fd);
    if (g_au == MAP_FAILED) {
        g_au = NULL;
        return;
    }
    memset(g_au, 0, sizeof *g_au);
    g_au->magic = ROTH_AUDIO_MAGIC;
    g_au->rate = g_au_rate;
    g_au->channels = g_au_ch;
    g_au->bits = g_au_bits;
    g_au->ready = 1;
    ALOG("audio out ready: %u Hz, %u-bit, %u ch, %u B/tick from %s "
         "(run viewer to hear)\n", g_au_rate, g_au_bits, g_au_ch, g_au_chunk,
         g_au_src_edi ? "edi" : "esi");
}

/* ---- CPU profiler (ROTH_PROFILE) ---------------------------------------- *
 * SIGPROF samples the interrupted EIP so we can see where time goes: game code
 * (a busy-wait) vs the host trap handler, plus which game page is hot. The host
 * timer IRQ uses SIGALRM, so SIGPROF/ITIMER_PROF is free. Diagnostic only. */
static unsigned g_prof_pg[0x600];   /* 256-byte buckets (canon 0..0x60000) so the hot bucket pins the exact fn */
static unsigned long g_prof_game, g_prof_host, g_prof_tot;
static void prof_handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig;
    (void)si;
    ucontext_t *u = (ucontext_t *)uc;
    uint32_t eip = (uint32_t)u->uc_mcontext.gregs[REG_EIP];
    g_prof_tot++;
    if (eip >= OBJ1_BASE && eip < STACK_TOP) {
        g_prof_game++;
        unsigned pg = (eip - OBJ_DELTA) >> 8;
        if (pg < 0x600)
            g_prof_pg[pg]++;
    } else {
        g_prof_host++;
    }
    /* Accumulate ONLY — no I/O. fprintf in a SIGPROF handler can re-enter libc
     * stdio that the interrupted code (the SIGSEGV handler's own logging) holds,
     * corrupting it -> crash. The dump runs from MAGIC_POLL (a non-nesting spot;
     * SIGPROF never does I/O so it can't nest on the dump). */
}

static unsigned long g_prof_next = 1000;
static void audio_profile_dump(void)
{
    if (!g_prof_tot || g_prof_tot < g_prof_next)
        return;
    g_prof_next = g_prof_tot + 1000;
    static unsigned long lf;
    static double lt;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;
    unsigned long fr = g_shm ? g_shm->frame : 0;
    double fps = (lt > 0.0 && now > lt) ? (double)(fr - lf) / (now - lt) : 0.0;
    lf = fr;
    lt = now;
    unsigned ti[6] = {0}, tv[6] = {0};
    for (unsigned p = 0; p < 0x600; p++) {
        unsigned c = g_prof_pg[p];
        for (int k = 0; k < 6; k++)
            if (c > tv[k]) {
                for (int j = 5; j > k; j--) {
                    tv[j] = tv[j - 1];
                    ti[j] = ti[j - 1];
                }
                tv[k] = c;
                ti[k] = p;
                break;
            }
    }
    fprintf(stderr, "[prof] tot=%lu game=%lu host=%lu fps=%.0f  hot(canon):",
            g_prof_tot, g_prof_game, g_prof_host, fps);
    for (int k = 0; k < 6; k++)
        if (tv[k])
            fprintf(stderr, " %x=%u", ti[k] << 8, tv[k]);
    fprintf(stderr, "\n");
}
static void audio_profile_init(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = prof_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    struct itimerval it = {{0, 5000}, {0, 5000}}; /* 5 ms CPU-time ticks */
    setitimer(ITIMER_PROF, &it, NULL);
    ALOG("CPU profiler on (SIGPROF 5ms): [prof] = host vs game CPU + hot pages\n");
}

static void sfx_trace_init(void);   /* §SFX-DROPOUT STANDING TRACE — defined below (near audio_mix_sfx) */

void audio_init(void)
{
    /* obj1 is mapped RWX; plant the dispatch-computer int3. The image-free boot maps NO obj1 code
     * (image-free audio dispatches through the os_audio_* natives, not a trap hook), so skip it there. */
    extern volatile int g_standalone_boot;   /* roth_host.h (traps.c); 0 in the trap host, 1 image-free */
    if (!g_standalone_boot)
        *(uint8_t *)(uintptr_t)HMI_DISPATCH_COMPUTER = 0xcc;
    ALOG("init: %s at canon 0x4fcd3 (dispatch computer); MAGIC_OFF=%#x\n",
         g_standalone_boot ? "NO int3 (image-free: os_audio natives dispatch)" : "int3",
         MAGIC_OFF);
    audio_shm_setup();
    if (getenv("ROTH_PROFILE"))
        audio_profile_init();

    if (getenv("ROTH_PCM_DUMP")) {
        g_pcm_fd = open("/tmp/roth_pcm.raw", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        ALOG("PCM dump -> /tmp/roth_pcm.raw (raw %u Hz %u-bit %u-ch)\n", g_au_rate,
             g_au_bits, g_au_ch);
    }

    { const char *m = getenv("ROTH_MIDI");  /* MUSIC ON BY DEFAULT;
                                             * ROTH_MIDI=0 = the explicit opt-out (the old deferred mode). Both
                                             * lanes: this is the exact config validated (the trap
                                             * w=367 / imgfree event-identical MIDI-ring captures). */
      g_defer_midi = (m != NULL && m[0] == '0'); }
    g_midi_log_en = (getenv("ROTH_MIDI_LOG") != NULL); /* off: per-msg log lags */
    g_alog_v = (getenv("ROTH_LOG_AUDIO") != NULL) || (getenv("ROTH_AUDIO_LOG") != NULL); /* per-tick/event spam */
    g_vtrace = (getenv("ROTH_VOL_TRACE") != NULL); /* volume-zero transition tracer (observation only) */
    if (g_vtrace)
        fprintf(stderr, "[audio] ROTH_VOL_TRACE=1: volume-zero transition tracer armed "
                        "(voice+0x32 shadow, per-voice change ring, zero-path evidence dumps)\n");
    g_mixtrace = (getenv("ROTH_MIX_TRACE") != NULL); /* per-tick governor-efficacy tracer (observation only) */
    if (g_mixtrace)
        fprintf(stderr, "[audio] ROTH_MIX_TRACE=1: governor-efficacy tracer armed (per-tick raw +0x32 "
                        "vs governed eff vs g_veff_valid, + reset-point reasons/mid-play)\n");
    g_sptrace = (getenv("ROTH_SPEECH_TRACE") != NULL); /* dialogue-voice skip tracer (observation only) */
    if (g_sptrace)
        fprintf(stderr, "[audio] ROTH_SPEECH_TRACE=1: dialogue-voice skip tracer armed (speech-voice "
                        "restart/deactivate/buffer-end/done-cb + stream-state transitions)\n");
    sfx_trace_init();  /* §SFX-DROPOUT STANDING TRACE (ROTH_SFX_TRACE=1): env + manual-dump signal */
    { const char *vs = getenv("ROTH_VOL_SMOOTH"); /* unset/1 = on (fps 10); 0 = off; N>=2 = on, fps N */
      if (vs == NULL) { g_vsmooth = 1; }
      else {
          unsigned long fv = strtoul(vs, NULL, 0);
          if (fv == 0)      g_vsmooth = 0;
          else if (fv >= 2) { g_vsmooth = 1; g_vsmooth_fps = (uint32_t)fv; }
          else              g_vsmooth = 1;   /* "1" = on, default fps */
      } }
    ALOG("SFX volume governor: %s, dos-fps=%u (ROTH_VOL_SMOOTH=0 off | 1 on | N>=2 = fps)\n",
         g_vsmooth ? "on" : "off", g_vsmooth_fps);
    if (g_defer_midi) {
        if (!g_standalone_boot)   /* obj1 unmapped image-free — no music-init int3 hook */
            *(uint8_t *)(uintptr_t)HMI_MUSIC_INIT = 0xcc;
        ALOG("init: MIDI deferred (%s at music-init 0x51681; set ROTH_MIDI=1 "
             "to enable). Null MIDI callbacks are skipped via the eip=0 guard.\n",
             g_standalone_boot ? "NO int3, image-free" : "int3");
    } else {
        /* MIDI enabled: set up the host->viewer MIDI-event ring for the synth. */
        int mfd = shm_open(ROTH_MIDI_SHM_NAME, O_CREAT | O_RDWR, 0600);
        if (mfd >= 0 && ftruncate(mfd, sizeof(struct roth_midi)) == 0) {
            g_midi = mmap(NULL, sizeof(struct roth_midi), PROT_READ | PROT_WRITE,
                          MAP_SHARED, mfd, 0);
            if (g_midi == MAP_FAILED)
                g_midi = NULL;
        }
        if (mfd >= 0)
            close(mfd);
        if (g_midi) {
            memset(g_midi, 0, sizeof *g_midi);
            g_midi->magic = ROTH_MIDI_MAGIC;
            g_midi->ready = 1;
            ALOG("MIDI ring ready (%s) -> viewer SoundFont synth\n",
                 ROTH_MIDI_SHM_NAME);
        }
    }
}

/* Emulate a 32-bit far RET: pop EIP and CS (8 bytes) off the game stack. */
static void far_ret(cpu_t *c)
{
    uint32_t esp = R_ESP(c);
    const uint32_t *st = (const uint32_t *)(uintptr_t)esp;
    R_EIP(c) = st[0];
    c->uc->uc_mcontext.gregs[REG_CS] = (greg_t)(st[1] & 0xffff);
    R_ESP(c) = esp + 8;
}

static unsigned long g_call_count;
static unsigned long g_midi_evlog; /* MIDI capture log throttle (Step B) */

/* Throttled dump of the play-position + any non-empty voice structs, so a
 * headless run reveals (a) whether the position is advancing and (b) the live
 * 0x6c voice layout the game writes (sample ptr/len/pos/rate/vol/flags). */
static void audio_voice_dump(cpu_t *c)
{
    g_mtick++;
    /* §SPEECH-SKIP: stream-state transition watcher. Runs every MAGIC_POLL tick (before the mixer's
     * cushion early-return can hide it), so the skip's force_end (state 1->2), the pump's 2->0, and
     * prime's ->1 are all timestamped against the mixer restart lines below. Read-only. */
    if (g_sptrace) {
        static int last_state = -2, last_active = -2;
        int st = (int)*(const volatile uint32_t *)(uintptr_t)CANON(0x8200c);
        int ac = (int)*(const volatile uint32_t *)(uintptr_t)CANON(0x81e3e);
        if (st != last_state || ac != last_active) {
            SPLOG("STREAM-STATE state[8200c] %d->%d  lineActive[81e3e] %d->%d\n",
                  last_state, st, last_active, ac);
            sptrace_globals("state-change");
            last_state = st;
            last_active = ac;
        }
    }
    /* SFX probe: log each voice the moment its "active" flag (word[+0x30] bit15)
     * goes set, with key struct fields, so triggering an in-game sound reveals
     * which voices the SOS lights up and the sample layout. Edge-triggered +
     * capped so it doesn't spam. (Voice 0 is the movie stream; SFX = others.) */
    if (g_farg_base) {
        static uint32_t seen;
        static int logs;
        for (unsigned v = 0; v < VOICE_N; v++) {
            const uint8_t *vp =
                (const uint8_t *)(uintptr_t)(g_farg_base + VOICE_OFF + v * VOICE_SZ);
            uint16_t fl = *(const uint16_t *)(vp + 0x30);
            if (fl & 0x8000) {
                const uint32_t *w = (const uint32_t *)vp;
                if ((!(seen & (1u << v)) || g_vlogptr[v] != w[0]) &&
                    logs++ < 40) {
                    g_vlogptr[v] = w[0];
                    int stream = (w[15] == CANON(0x4e394));
                    uint8_t hdr8 = 0;
                    int16_t s0 = 0, s1 = 0;
                    if (w[0] > 0x10000) {
                        const uint8_t *sp = (const uint8_t *)(uintptr_t)w[0];
                        hdr8 = sp[8];
                        s0 = ((const int16_t *)sp)[0];
                        s1 = ((const int16_t *)sp)[1];
                    }
                    ALOGV("t%lu VOICE %u ON %s ptr=%08x len=%u +10=%08x +31=%02x "
                         "+34=%08x vol=%04x cb=%08x +54rate=%u hdr8=%02x s16=%d,%d\n",
                         g_mtick, v, stream ? "stream" : "sfx", w[0], w[6], w[4],
                         *(vp + 0x31), w[13], *(const uint16_t *)(vp + 0x32),
                         w[15], w[21], hdr8, s0, s1);
                    /* PROBE (kept as a tool, ROTH_SAMPLESCAN=1): scan the sample bytes at ptr to
                     * decide silence vs real audio — max-abs over up to 8192 s16 + a hex head. */
                    static int ss_on = -1;
                    if (ss_on < 0) ss_on = getenv("ROTH_SAMPLESCAN") ? 1 : 0;
                    if (ss_on && w[0] > 0x10000 && w[6] >= 2) {
                        const int16_t *ss = (const int16_t *)(uintptr_t)w[0];
                        uint32_t ns = w[6] / 2u, lim = ns > 8192u ? 8192u : ns;
                        int32_t mx = 0; uint64_t energy = 0; uint32_t nzero = 0;
                        for (uint32_t k = 0; k < lim; k++) {
                            int32_t a = ss[k] < 0 ? -ss[k] : ss[k];
                            if (a > mx) mx = a;
                            energy += (uint64_t)a;
                            if (ss[k] != 0) nzero++;
                        }
                        const uint8_t *sb = (const uint8_t *)(uintptr_t)w[0];
                        /* the resource-pool descriptor header sits at data-0xe:
                         * +0 size, +4 word rate, +6 flags(bit1=upsampled), +9 group */
                        const uint8_t *dc = sb - 0xe;
                        uint32_t dsize = *(const uint32_t *)dc;
                        uint16_t drate = *(const uint16_t *)(dc + 4);
                        uint8_t  dflag = dc[6], dgrp = dc[9];
                        fprintf(stderr, "[audio] SAMPLESCAN v%u ptr=%08x nsamp=%u scanned=%u "
                                "maxabs=%d meanabs=%llu nonzero=%u desc{sz=%u rate=%u fl=%02x grp=%02x}  head:",
                                v, w[0], ns, lim,
                                mx, (unsigned long long)(lim ? energy / lim : 0), nzero,
                                dsize, drate, dflag, dgrp);
                        for (int b = 0; b < 16; b++) fprintf(stderr, " %02x", sb[b]);
                        fprintf(stderr, "\n");
                    }
                }
                seen |= (1u << v);
            } else {
                seen &= ~(1u << v);
            }
        }
    }
    static unsigned long tick, dumps;
    tick++;
    if ((tick % 70) != 1)
        return; /* ~once per second of HMI ticks */
    if (dumps++ >= 12 || !g_farg_base)
        return;
    uint32_t pos = g_pos_lin ? *(uint32_t *)(uintptr_t)g_pos_lin : 0;
    ALOGV("voicedump t=%lu pos=%#x  (cb esi=%#x edi=%#x ecx=%#x)\n", tick, pos,
         c ? R_ESI(c) : 0, c ? R_EDI(c) : 0, c ? R_ECX(c) : 0);
    /* streaming-audio engine state (canon 0x91dxx): pending-buffer counter
     * db4 (the intro spins on db4>=2), play cursors d60/d68/d70, refill count
     * cb8, active flag 7b6c, and the two service callbacks d00/1898. */
    ALOGV("  stream db4=%u d50=%#x d5c=%#x d60=%#x d68=%#x d70=%#x cb8=%u "
         "flag7b6c=%u\n",
         *(uint16_t *)(uintptr_t)CANON(0x91db4),
         *(uint32_t *)(uintptr_t)CANON(0x91d50),
         *(uint32_t *)(uintptr_t)CANON(0x91d5c),
         *(uint32_t *)(uintptr_t)CANON(0x91d60),
         *(uint32_t *)(uintptr_t)CANON(0x91d68),
         *(uint32_t *)(uintptr_t)CANON(0x91d70),
         *(uint32_t *)(uintptr_t)CANON(0x91cb8),
         *(uint8_t *)(uintptr_t)CANON(0x97b6c));
    ALOGV("  timing mode(d0c)=%#x dbe=%d db0=%d db8=%d dba=%d acc(dbc)=%#x "
         "884=%#x ds=%#x int8vec=canon%#x\n",
         *(uint32_t *)(uintptr_t)CANON(0x91d0c),
         *(int16_t *)(uintptr_t)CANON(0x91dbe),
         *(int16_t *)(uintptr_t)CANON(0x91db0),
         *(int16_t *)(uintptr_t)CANON(0x91db8),
         *(int16_t *)(uintptr_t)CANON(0x91dba),
         *(uint16_t *)(uintptr_t)CANON(0x91dbc),
         *(uint32_t *)(uintptr_t)CANON(0x91884),
         c ? (unsigned)c->uc->uc_mcontext.gregs[REG_DS] : 0,
         g_pm_vec_int21[8] ? g_pm_vec_int21[8] - OBJ_DELTA : 0);
    /* decoded-PCM buffer (0x4e460 writes 16-bit interleaved stereo here) */
    uint32_t decbuf = *(uint32_t *)(uintptr_t)CANON(0x91d4c);
    uint32_t d28 = *(uint32_t *)(uintptr_t)CANON(0x91d28);
    if (decbuf > 0x10000) {
        const int16_t *p = (const int16_t *)(uintptr_t)decbuf;
        ALOGV("  decbuf=%#x d28=%u(frames~%u) samp: %d %d %d %d %d %d\n", decbuf,
             d28, d28 >> 1, p[0], p[1], p[2], p[3], p[4], p[5]);
    }
    for (unsigned v = 0; v < VOICE_N; v++) {
        const uint32_t *w =
            (const uint32_t *)(uintptr_t)(g_farg_base + VOICE_OFF + v * VOICE_SZ);
        int nz = 0;
        for (unsigned k = 0; k < VOICE_SZ / 4; k++)
            if (w[k]) {
                nz = 1;
                break;
            }
        if (!nz)
            continue;
        ALOGV("  v%02u %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x "
             "%08x %08x %08x %08x %08x %08x\n", v, w[0], w[1], w[2], w[3], w[4],
             w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13], w[14],
             w[15], w[16], w[17]);
    }
}

/* Would a NON-speech done-cb on this voice run the action-2 arm — i.e. free its handle
 * record? This is the OLD node-header heuristic (the one restored by the revert; it mirrors
 * how the game cb 0x27501 classifies the sound): a one-shot ends with action 2 (0x275a6:
 * st32(rec,0)); a loop-flagged sample node ends with action 0 (the cb requeues, frees
 * nothing). Evaluated at QUEUE time — while the voice struct still holds the ENDED sound —
 * so a later same-slot reuse can't hide the ended sound's class. Speech (tag 0xedXX) takes
 * the g_voice_sos_callback branch and never touches the record this way -> 0. */
static int sfx_end_frees_record(const uint8_t *vp)
{
    uint16_t sid = *(const uint16_t *)(vp + 0x34);
    if ((sid & 0xff00u) == 0xed00u)
        return 0; /* speech: forwarded to g_voice_sos_callback; no record free */
    if (sid < 256u) {
        uint32_t se = CANON(0x83ed4) + (uint32_t)sid * 0x9au;
        uint32_t sp = *(uint32_t *)(uintptr_t)se;
        if (sp > 0x10000u) {
            uint8_t lf = *(uint8_t *)(uintptr_t)(sp + 8);
            if ((lf & 0x80) && (lf & 7) == 1 &&
                *(const uint16_t *)(uintptr_t)(se + 0x10) != 0)   /* rec+0x10 (cb's 0x27564 test); a FADED
                                                                   * loop must be reclaimed, not requeued */
                return 0; /* audible looping node: cb requeues (action 0) — record stays live */
        }
    }
    return 1; /* one-shot: cb action 2 -> st32(0x83ed4 + sid*0x9a, 0) */
}

/* ---- VOLUME-ZERO TRANSITION TRACER helpers (read-only; all no-ops unless g_vtrace) -------------- */

/* raw read-only game-memory accessors (linear = canon + OBJ_DELTA; the region is MAP_FIXED) */
static inline uint8_t  vt_r8 (uint32_t a) { return *(const volatile uint8_t  *)(uintptr_t)a; }
static inline uint16_t vt_r16(uint32_t a) { return *(const volatile uint16_t *)(uintptr_t)a; }
static inline uint32_t vt_r32(uint32_t a) { return *(const volatile uint32_t *)(uintptr_t)a; }

/* Plain integer floor-sqrt for the HOST recomputation of compute's out-of-range path. The game uses
 * a table-based fixed-point isqrt (0x3bfe5); this floor-sqrt is within a few units — ample for the
 * dist-vs-range zero decision (labelled a host recomputation, not the game's own result). */
static uint32_t vt_isqrt(uint32_t v)
{
    if (v == 0) return 0;
    uint32_t x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return x;
}

/* Read-only replica of compute_sound_volume_pan (lift_audio.c:65-105) with the SAME globals/record
 * fields, exposing WHICH of compute's three zero paths (if any) currently fires. Writes nothing
 * (the game's compute stores dist into rec+0xc; this replica does NOT). *why labels the outcome. */
static int32_t vt_compute_vol(uint32_t rec, const char **why)
{
    *why = "NONE";
    uint32_t samp = vt_r32(rec);
    if (samp <= 0x10000u) { *why = "BAD_NODE_PTR"; return -1; }
    int32_t range = (int16_t)vt_r16(samp + 0xa);
    uint32_t dist = vt_isqrt(vt_r32(rec + 8));
    if (range == 0) { *why = "RANGE==0 (game #DE)"; return 0; }
    int32_t edx = 0x7fff - (int32_t)(dist * 0x7fffu) / range;
    if (edx < 0) { *why = "OUT_OF_RANGE (dist>range)"; return 0; }
    uint8_t sv = vt_r8(samp + 0x10);
    if (sv != 0x40) edx = (edx * (int32_t)sv) >> 6;
    uint8_t side = vt_r8(rec + 6);
    if (side != 0) {
        uint32_t zbase = vt_r32(CANON(0x85c48));
        if (zbase > 0x10000u) {
            uint32_t blk  = zbase + ((uint32_t)side << 5) - 0x20u;
            int32_t count = (int16_t)vt_r16(blk);
            int32_t ex, ey;
            if (vt_r8(rec + 4) & 0x80) { ex = (int16_t)vt_r16(rec + 0x14); ey = (int16_t)vt_r16(rec + 0x16); }
            else                       { ex = (int16_t)vt_r16(samp);       ey = (int16_t)vt_r16(samp + 2); }
            int32_t rel_a = ((int32_t)vt_r32(CANON(0x90a94)) >> 16) - ey;
            int32_t rel_b = ((int32_t)vt_r32(CANON(0x90a8c)) >> 16) - ex;
            uint32_t w = blk + 2;
            for (int32_t i = 0; i < count && i < 64; i++, w += 0xau) {
                int hit;
                if (vt_r8(w + 1) & 1)
                    hit = ((int16_t)vt_r16(w + 2) < rel_b) && (rel_b < (int16_t)vt_r16(w + 6))
                       && ((int16_t)vt_r16(w + 4) < rel_a) && (rel_a < (int16_t)vt_r16(w + 8));
                else
                    hit = (rel_b < (int16_t)vt_r16(w + 2)) || (rel_b > (int16_t)vt_r16(w + 6))
                       || (rel_a < (int16_t)vt_r16(w + 4)) || (rel_a > (int16_t)vt_r16(w + 8));
                if (hit) {
                    edx -= (int32_t)((uint32_t)vt_r8(w) << 7);
                    if (edx < 0) { *why = "ZONE_WALL (wall scan subtract)"; return 0; }
                    break;
                }
            }
        }
    }
    uint32_t master = vt_r32(CANON(0x71d84));
    if (master == 0) { *why = "MASTER_ZERO [0x71d84]"; return 0; }
    edx = (int32_t)(((uint32_t)(edx * (int32_t)master)) >> 15);
    if (edx > 0x7fff) edx = 0x7fff;
    return edx;
}

/* Owning-record lookup for a mixer voice (both linkage directions). Extracted from vt_dump_record so
 * the governor SEED path can resolve a voice's record OUTSIDE the flag-gated trace code. Prefer the
 * rec+0x18==v back-link scan (the live pointer the game maintains); fall back to the voice+0x34 tag
 * when no scan hit. .rec = the chosen linear (canon+DELTA) record addr, 0 when none. .scan_hits>1
 * means multiple live records claim this voice (ambiguous — the seed must fall back to raw). The tag
 * read is bounded 0..15; the scan is 16 slots. Read-only. */
struct rec_link {
    uint32_t rec;         /* chosen owning record (linear), or 0 */
    uint32_t rec_by_tag;  /* tag-derived record (linear), 0 if tag>=16 */
    uint32_t rec_by_scan; /* first rec+0x18==v hit (linear), or 0 */
    int      scan_hits;   /* # live records whose rec+0x18==v */
    int      tag_ok;      /* tag record is live AND back-links to v */
    uint16_t tag;         /* word[vp+0x34] */
};
static struct rec_link vt_link_record(unsigned v, const uint8_t *vp)
{
    struct rec_link L;
    L.tag        = *(const uint16_t *)(vp + 0x34);
    L.rec_by_tag = (L.tag < 16u) ? (uint32_t)(CANON(0x83ed4) + (uint32_t)L.tag * 0x9au) : 0;
    L.rec_by_scan = 0; L.scan_hits = 0;
    for (unsigned r = 0; r < 16u; r++) {
        uint32_t rp = CANON(0x83ed4) + r * 0x9au;
        if (vt_r32(rp) == 0) continue;               /* dead slot (rec+0==0) */
        if (vt_r32(rp + 0x18) == v) { if (!L.rec_by_scan) L.rec_by_scan = rp; L.scan_hits++; }
    }
    L.tag_ok = (L.rec_by_tag && vt_r32(L.rec_by_tag) != 0 && vt_r32(L.rec_by_tag + 0x18) == v);
    L.rec    = L.rec_by_scan ? L.rec_by_scan : (L.tag_ok ? L.rec_by_tag : 0);
    return L;
}

/* The governor's SEED decision for a voice at its FIRST sighting after start/reuse. The clank (play_object_sound path) computes the CORRECT onset volume
 * at submit (voice+0x32 == compute_sound_volume_pan's result), but the game's own update_active_sounds
 * slews the APPLIED volume toward a STALE record target (rec+0x12, never initialized on this path and
 * not cleared by find_free_slot -> 0 on a fresh slot). That slew runs ~30x per mixer tick under the
 * host, so the mixer's first scan of the new voice already reads a DEGRADED raw = expected - m*0x200
 * (m = the submit-to-scan latency lottery). Seed rule: when the owning record's target word (+0x12)
 * is the 0 stale signature AND the raw is BELOW the game's own compute (expected), reconstruct the
 * true onset (seed = expected, capped 0x7fff) — DOS's mixer sampled within ~1 frame of submit, so
 * this reproduces DOS timing. Otherwise seed at raw as before.
 *   EXCLUSIONS (their volume semantics differ from the positional-SFX compute):
 *     - speech voices (tag 0xedXX -> g_voice_sos_callback) — never reconstruct.
 *     - movie-stream voices (+0x3c==0x4e394) already `continue` before the mixer seed, so they can
 *       never reach here; the tag exclusion is the belt-and-braces for the trace-print call site.
 *   CAN'T MISFIRE ON AN ENDING VOICE: deactivation is via the active bit (word[+0x30] bit15) and
 *   +0x34, not rec+0x12; the mixer clears g_veff_valid and `continue`s the moment bit15 drops, so an
 *   ending voice never reaches the seed. A seed only ever happens on the first tick after a (re)start,
 *   where the voice is active and freshly filled — a target==0 there IS the un-initialized artifact,
 *   not a computed-zero. And a legitimately quiet sound (out-of-range / master-muted) has expected==0,
 *   so `expected > raw` (raw>=0) can never fire — the swing (range==0 -> expected 0) is likewise never
 *   reconstructed. `raw` = the clamped voice+0x32 at first sighting. Read-only; game state untouched. */
static uint16_t governor_seed(unsigned v, const uint8_t *vp, uint16_t raw, int *reconstructed)
{
    if (reconstructed) *reconstructed = 0;
    uint16_t tag = *(const uint16_t *)(vp + 0x34);
    if ((tag & 0xff00u) == 0xed00u)                  /* speech: excluded */
        return raw;
    struct rec_link L = vt_link_record(v, vp);
    if (!L.rec || L.scan_hits > 1)                   /* no record / ambiguous: fall back to raw */
        return raw;
    if (vt_r16(L.rec + 0x12) != 0)                   /* target initialized: not the stale artifact */
        return raw;
    const char *why;
    int32_t expected = vt_compute_vol(L.rec, &why);  /* the game's own formula, read-only replica */
    if (expected > (int32_t)raw) {                   /* stale target + raw dragged below the true onset */
        if (expected > 0x7fff) expected = 0x7fff;    /* cap: never seed above full scale */
        if (reconstructed) *reconstructed = 1;
        return (uint16_t)expected;
    }
    return raw;
}

/* Dump the per-voice volume-change ring (oldest -> newest) for voice v. A descent that arrives as
 * many ~0x200-step entries = the game's slew (update_active_sounds/clamp_diff_200); a single big
 * entry (large wc) = a non-slew direct write => a DIFFERENT writer. */
static void vt_dump_ring(unsigned v)
{
    unsigned cnt = g_vring_cnt[v];
    fprintf(stderr, "[audio]   vol-ring v%u (%u change(s) since ON, oldest->newest):\n", v, cnt);
    for (unsigned k = 0; k < cnt; k++) {
        unsigned idx = (g_vring_head[v] + VOL_RING - cnt + k) % VOL_RING;
        struct vol_change *e = &g_vring[v][idx];
        int32_t d = (int32_t)e->newv - (int32_t)e->oldv;
        fprintf(stderr, "[audio]     t%lu  %04x -> %04x  (d=%+d, inferred %u write(s) @<=0x200/step)\n",
                e->tick, e->oldv, e->newv, d, e->wc);
    }
}

/* Find + dump the owning sound record for voice v (both linkage directions), the compute ingredients,
 * and a host-side recomputation of compute's zero paths. Read-only. When is_onset, also emit a single
 * glanceable ONSET line (observed voice+0x32 vs host-recomputed-expected + every compute input) so a
 * per-attack sweep shows AT ONCE which input carries the loudness variance. */
static void vt_dump_record(unsigned v, const uint8_t *vp, int is_onset)
{
    struct rec_link L = vt_link_record(v, vp);       /* shared owning-record lookup (both directions) */
    fprintf(stderr, "[audio]   linkage: voice+0x34 tag=%u (rec_by_tag %s), scan rec+0x18==v -> %d hit(s)%s\n",
            L.tag, L.tag_ok ? "MATCHES rec+0x18==v" : "does NOT match rec+0x18==v",
            L.scan_hits, (L.scan_hits > 1) ? " [!! multiple records reference this voice]" : "");

    uint32_t rec = L.rec;
    if (rec == 0) {
        fprintf(stderr, "[audio]   NO owning record found for voice %u (tag=%u) — orphaned voice or stale +0x34\n", v, L.tag);
        return;
    }
    int recidx = (int)((rec - CANON(0x83ed4)) / 0x9au);
    fprintf(stderr, "[audio]   record[%d] @canon %#x  (source: %s%s)\n", recidx, rec - OBJ_DELTA,
            L.rec_by_scan ? "rec+0x18 scan" : "voice+0x34 tag",
            (L.rec_by_scan && L.tag_ok && L.rec_by_tag != L.rec_by_scan) ? " ; DISAGREES with tag" : "");

    uint32_t node = vt_r32(rec + 0x00);
    fprintf(stderr, "[audio]     +00 node=%#x +04 flags=%02x +06 side=%u +08 d2in=%u +0c distout=%u "
                    "+10 applied=%04x +12 target=%04x +18 vhandle=%u +14/16 own=(%d,%d)\n",
            node, vt_r8(rec + 4), vt_r8(rec + 6), vt_r32(rec + 8), vt_r32(rec + 0xc),
            vt_r16(rec + 0x10), vt_r16(rec + 0x12), vt_r32(rec + 0x18),
            (int)(int16_t)vt_r16(rec + 0x14), (int)(int16_t)vt_r16(rec + 0x16));

    /* full 0x9a raw bytes of the record (the whole slot, incl. the embedded +0x26 SOS voice struct) */
    {
        char hex[3 * 0x9a + 1]; int p = 0;
        for (unsigned b = 0; b < 0x9a; b++)
            p += snprintf(hex + p, sizeof hex - (size_t)p, "%02x ", vt_r8(rec + b));
        fprintf(stderr, "[audio]     raw[0x00..0x9a]: %s\n", hex);
    }

    /* compute ingredients: same globals compute_sound_volume_pan reads */
    uint32_t master = vt_r32(CANON(0x71d84));
    int32_t  py16   = (int32_t)vt_r32(CANON(0x90a8c)) >> 16;
    int32_t  px16   = (int32_t)vt_r32(CANON(0x90a94)) >> 16;
    fprintf(stderr, "[audio]     master[0x71d84]=%#x  player[0x90a8c>>16]=%d [0x90a94>>16]=%d\n",
            master, py16, px16);
    if (node > 0x10000u) {
        fprintf(stderr, "[audio]     sample: node.coord=(%d,%d) range[+0xa]=%d sampvol[+0x10]=%u\n",
                (int)(int16_t)vt_r16(node), (int)(int16_t)vt_r16(node + 2),
                (int)(int16_t)vt_r16(node + 0xa), vt_r8(node + 0x10));
    } else {
        fprintf(stderr, "[audio]     sample: node ptr %#x looks invalid (<=0x10000) — skipped\n", node);
    }
    uint8_t side = vt_r8(rec + 6);
    if (side != 0) {
        uint32_t zbase = vt_r32(CANON(0x85c48));
        if (zbase > 0x10000u) {
            uint32_t blk = zbase + ((uint32_t)side << 5) - 0x20u;
            int32_t count = (int16_t)vt_r16(blk);
            fprintf(stderr, "[audio]     zone side=%u blk=@canon %#x count=%d\n", side, blk - OBJ_DELTA, count);
            uint32_t w = blk + 2;
            for (int32_t i = 0; i < count && i < 3; i++, w += 0xau)
                fprintf(stderr, "[audio]       wall[%d] atten<<7=%u inv=%u bounds=(%d,%d,%d,%d)\n", i,
                        (unsigned)vt_r8(w) << 7, vt_r8(w + 1) & 1,
                        (int)(int16_t)vt_r16(w + 2), (int)(int16_t)vt_r16(w + 6),
                        (int)(int16_t)vt_r16(w + 4), (int)(int16_t)vt_r16(w + 8));
        } else {
            fprintf(stderr, "[audio]     zone side=%u but zone-list [0x85c48]=%#x invalid — skipped\n", side, zbase);
        }
    } else {
        fprintf(stderr, "[audio]     zone side=0 (no zone-wall attenuation)\n");
    }

    /* host-side zero-path diagnosis (recomputation — NOT the game's own result) */
    const char *why = "NONE";
    int32_t hv = vt_compute_vol(rec, &why);
    fprintf(stderr, "[audio]     HOST-RECOMPUTE compute_sound_volume_pan => %s (vol=%d) "
                    "[read-only; the game's own compute may differ]\n", why, hv);

    /* ONSET one-liner: observed submit volume (voice+0x32 == game's start_sound_voice_vol -> v+0x14 ==
     * compute result, mixer-scales PCM by it) vs the host recomputation, with EVERY compute input on one
     * line. For the entity/clank path (emitter 0x83eb0: range=0x7d0, sampvol=0x40 -> sv-scale skipped,
     * side=0 -> no wall, master=[0x71d84] a user setting) the ONLY per-attack input is d2in=rec+8, the
     * squared player->coords distance. Sweep this line across N stationary attacks: if d2in/coords vary
     * while player(90a8c/90a94) stays fixed, the loudness lottery is the submit COORDS (rec+0x14/0x16),
     * not any host volume transform and not record-slot residue (none of +0x0c/+0x10/+0x12 is a compute
     * input on this path). */
    if (is_onset) {
        uint16_t observed = *(const uint16_t *)(vp + 0x32);
        uint32_t d2in     = vt_r32(rec + 8);
        int      erange   = (node > 0x10000u) ? (int)(int16_t)vt_r16(node + 0xa) : 0;
        unsigned esv      = (node > 0x10000u) ? vt_r8(node + 0x10) : 0;
        fprintf(stderr,
            "[audio]   >>> ONSET v%u: observed(voice+0x32)=%04x  host-expected=%d [%s]  delta=%+d\n"
            "[audio]       d2in(rec+8)=%u dist=%u  coords(rec+14/16)=(%d,%d)  player(90a8c/90a94>>16)=(%d,%d)\n"
            "[audio]       emitter=%#x range(+0xa)=%d sampvol(+0x10)=%u  master[71d84]=%04x  side=%u\n",
            v, observed, hv, why, (int)hv - (int)observed,
            d2in, vt_isqrt(d2in),
            (int)(int16_t)vt_r16(rec + 0x14), (int)(int16_t)vt_r16(rec + 0x16),
            py16, px16, node, erange, esv, master, side);
        /* Seed decision the governor will make for this voice:
         * reconstructed(expected) when rec+0x12 is the 0 stale signature AND raw < expected (the DOS
         * onset is rebuilt), else raw. Computed via the same governor_seed() the mixer uses, so the
         * trace shows exactly what the seed path did. */
        int recon = 0;
        uint16_t rawc  = (uint16_t)((int16_t)observed > 0 ? (int16_t)observed : 0);
        uint16_t seedv = governor_seed(v, vp, rawc, &recon);
        fprintf(stderr,
            "[audio]       governor seed=%s vol=%04x  (raw=%04x target(rec+0x12)=%04x; %s)\n",
            recon ? "reconstructed(expected)" : "raw", seedv, rawc, vt_r16(rec + 0x12),
            g_vsmooth ? "ROTH_VOL_SMOOTH on" : "ROTH_VOL_SMOOTH off (seed unused)");
    }
}

/* Full evidence dump for voice v at a volume-zero (or big-drop) transition. */
static void vt_dump(unsigned v, const uint8_t *vp, const char *reason)
{
    fprintf(stderr, "[audio] === ROTH_VOL_TRACE %s: voice %u t%lu ===\n", reason, v, g_mtick);
    fprintf(stderr, "[audio]   voice: +30dword=%08x +34tag=%04x +00ptr=%08x +08cur=%08x +10end=%08x "
                    "on_vol=%04x now_vol=%04x\n",
            *(const uint32_t *)(vp + 0x30), *(const uint16_t *)(vp + 0x34),
            *(const uint32_t *)(vp + 0x00), *(const uint32_t *)(vp + 0x08),
            *(const uint32_t *)(vp + 0x10), g_von_vol[v], *(const uint16_t *)(vp + 0x32));
    vt_dump_ring(v);
    vt_dump_record(v, vp, 0);
}

/* Per-mixer-entry volume shadow scan. Runs every audio_mix_sfx call (before its cushion early-returns
 * — so a slew during a cushioned window is still sampled tick-by-tick). Pure observation. */
static void vol_trace_scan(void)
{
    if (!g_vtrace || !g_farg_base)
        return;
    for (unsigned v = 0; v < VOICE_N; v++) {
        const uint8_t *vp = (const uint8_t *)(uintptr_t)(g_farg_base + VOICE_OFF + v * VOICE_SZ);
        uint16_t fl = *(const uint16_t *)(vp + 0x30);
        if (!(fl & 0x8000u)) { g_vshadow_valid[v] = 0; continue; } /* inactive: drop the shadow */
        uint32_t curptr = *(const uint32_t *)(vp + 0x00);
        uint16_t curvol = *(const uint16_t *)(vp + 0x32);
        if (!g_vshadow_valid[v] || g_vt_ptr[v] != curptr) {       /* ON / restart / slot reuse */
            g_vshadow_vol[v]   = curvol;
            g_von_vol[v]       = curvol;
            g_vt_ptr[v]        = curptr;
            g_vshadow_valid[v] = 1;
            g_vring_head[v]    = 0;
            g_vring_cnt[v]     = 0;
            g_vt_changes[v]    = 0;
            fprintf(stderr, "[audio] === ROTH_VOL_TRACE VOICE ON: voice %u t%lu on_vol=%04x tag=%04x ptr=%08x ===\n",
                    v, g_mtick, curvol, *(const uint16_t *)(vp + 0x34), curptr);
            vt_dump_record(v, vp, 1);                              /* start-state record snapshot + ONSET line (item 4) */
            continue;
        }
        if (curvol != g_vshadow_vol[v]) {
            int32_t delta = (int32_t)curvol - (int32_t)g_vshadow_vol[v];
            uint32_t mag  = (uint32_t)(delta < 0 ? -delta : delta);
            uint16_t wc   = (uint16_t)((mag + 0x1ffu) / 0x200u);   /* min ~0x200-step writes to span it */
            struct vol_change *e = &g_vring[v][g_vring_head[v]];
            e->tick = g_mtick; e->oldv = g_vshadow_vol[v]; e->newv = curvol; e->wc = wc ? wc : 1;
            g_vring_head[v] = (uint8_t)((g_vring_head[v] + 1) % VOL_RING);
            if (g_vring_cnt[v] < VOL_RING) g_vring_cnt[v]++;
            g_vt_changes[v]++;
            int kill = (g_vshadow_vol[v] != 0 && curvol == 0) || (delta <= -0x400);
            g_vshadow_vol[v] = curvol;
            if (kill)
                vt_dump(v, vp, (curvol == 0) ? "VOLUME->ZERO" : "VOLUME BIG-DROP");
        }
    }
}

/* ===== §SFX-DROPOUT STANDING TRACE (ROTH_SFX_TRACE=1) — v2 ========================================
 * A whole-session, LOW-OVERHEAD capture for the intermittent "all SFX die permanently mid-session"
 * bug (restart-only recovery). Two lock-free in-memory rings (NO I/O during play), written from the
 * audio pump (game thread):
 *   - an EVENT ring: voice lifecycle edges (START / END / DEACTIVATE / CB-QUEUE / CB-DELIVER /
 *     CB-DROP) PLUS voice-START results (VSTART / VSTARTFAIL) — the WHAT-the-voices-were-doing history.
 *   - a once/second HEALTH log: active/wanted/stuck/free voice counts, done-cb queue depth, PCM ring
 *     level + underruns, sfx PCM frames mixed, and the voice-start attempt/success deltas that second —
 *     the long-term trend (wrap-resistant), whose free-slot column is the smoking-gun slot-pool curve.
 *
 * TWO death-mode auto-detectors, each fires ONCE per episode + auto-dumps:
 *   (1) TOTAL-SILENCE (the v1 signature): the game holds SFX voices ACTIVE (bit15 set, real PCM,
 *       not a movie stream) but the mixer produced ZERO sfx PCM and the ring is not merely cushioned,
 *       for ROTH_SFX_DEAD_SECS (default 4) wall-clock seconds -> "*** SFX-DEAD DETECTED ***".
 *   (2) START-FAIL (v2, the mode the director actually hits): the host voice-start native returns
 *       0xffffffff (no free slot) ROTH_SFX_STARTFAIL_N times in a row (default 4) — i.e. every NEW
 *       voice fails to allocate because the 32 slots are all held (the g_vended done-cb latch wedges
 *       them one by one), while established loops keep mixing (so detector 1 never trips) ->
 *       "*** SFX START-FAIL DETECTED ***". This is invisible to the mixer scan (a failed start
 *       populates no voice struct), so it MUST be observed at the voice-start veneer itself
 *       (sfx_trace_voice_start, called from os_audio_standalone.c).
 *
 * DUMP-ON-EXIT (v2): armed, the trace file writes itself when the process leaves — sfx_trace_exit_dump
 * is called from the host clean-return points (main.c) AND before the SDL-window-close hard _exit
 * (traps.c shm_tick). So the director's flow is simply: play, quit, send ROTH_SFX_TRACE_FILE.
 * Manual dump anytime: `kill -USR2 <pid>`.
 * Zero cost when ROTH_SFX_TRACE is unset (one cached-int test per hook; no scans, no syscalls). */
static int g_sfxtrace;                    /* ROTH_SFX_TRACE: the standing SFX-dropout trace */
static double g_sfx_dead_secs = 4.0;      /* ROTH_SFX_DEAD_SECS: dead-hold before the auto-marker */
static const char *g_sfx_trace_file;      /* ROTH_SFX_TRACE_FILE (default /tmp/roth_sfx_trace.txt) */
volatile sig_atomic_t g_sfx_dump_req;     /* set by the SIGUSR2 handler; polled by the pump */

/* v2 voice-start observability (the real dropout signature). Session totals + the consecutive-failure
 * run that drives the START-FAIL detector. Written only from sfx_trace_voice_start (game/pump thread). */
static unsigned long g_sfx_vs_att;        /* voice-start attempts this session */
static unsigned long g_sfx_vs_ok;         /* ... that got a free slot */
static unsigned long g_sfx_vs_fail;       /* ... that returned 0xffffffff (no free slot) */
static unsigned g_sfx_vs_consec;          /* consecutive start-failures (the wedge run) */
static unsigned g_sfx_startfail_n = 4;    /* ROTH_SFX_STARTFAIL_N: consecutive misses before the marker */
static int g_sfx_startfail_fired;         /* one-shot latch for detector (2); re-arms on the next success */

enum { SFXE_START = 1, SFXE_END, SFXE_DEACT, SFXE_CBQ, SFXE_CBDELIVER, SFXE_CBDROP, SFXE_DEAD, SFXE_RESUME,
       SFXE_VSTART, SFXE_VSTARTFAIL, SFXE_STARTFAIL };
static const char *const sfxe_name[] = {
    "?", "START", "END", "DEACT", "CB-QUEUE", "CB-DELIVER", "CB-DROP", "SFX-DEAD", "SFX-RESUME",
    "VSTART", "VSTARTFAIL", "STARTFAIL"
};
struct sfx_ev_rec { uint32_t tick; uint32_t ms; uint8_t type; uint8_t voice; uint16_t a; uint32_t b, c, d; };
struct sfx_health_rec { uint32_t ms; uint16_t active, wanted, stuck, cbq; uint32_t ring_level, underruns, mixed_frames;
                        uint16_t freeslots, vs_att_d, vs_ok_d; };
#define SFX_EVRING 4096u
#define SFX_HRING  4096u          /* once/sec => ~68 min of session at 1 Hz */
static struct sfx_ev_rec     g_sfx_ev[SFX_EVRING];
static struct sfx_health_rec g_sfx_hl[SFX_HRING];
static unsigned g_sfx_ev_head, g_sfx_ev_cnt;
static unsigned g_sfx_hl_head, g_sfx_hl_cnt;

static uint32_t sfx_now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    static uint32_t base; static int have;
    uint32_t ms = (uint32_t)(ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull);
    if (!have) { have = 1; base = ms; }
    return ms - base;
}
static void sfx_ev(uint8_t type, uint8_t voice, uint16_t a, uint32_t b, uint32_t c, uint32_t d)
{
    if (!g_sfxtrace) return;
    struct sfx_ev_rec *e = &g_sfx_ev[g_sfx_ev_head];
    e->tick = (uint32_t)g_mtick; e->ms = sfx_now_ms();
    e->type = type; e->voice = voice; e->a = a; e->b = b; e->c = c; e->d = d;
    g_sfx_ev_head = (g_sfx_ev_head + 1) % SFX_EVRING;
    if (g_sfx_ev_cnt < SFX_EVRING) g_sfx_ev_cnt++;
}

/* Dump both rings + the live voice table. Called from THREE contexts: the SIGALRM pump's total-silence
 * self-check (signal context, SIGALRM already blocked), the game thread's voice-start START-FAIL
 * detector + dump-on-exit (normal context), and the USR2-flag poll. Both detectors can be true at once
 * during a wedge, so a normal-context dump could be interrupted by the pump firing its OWN dump ->
 * reentrant fopen/malloc -> deadlock. Block SIGALRM (+USR2) for the duration to serialize all dumps;
 * when already in the handler this is a harmless no-op (the signal is masked there anyway). Appends a
 * timestamped section so an auto-capture is never lost to a later manual/exit dump. */
static void sfx_trace_dump(const char *reason)
{
    if (!g_sfxtrace) return;
    sigset_t blk, old; sigemptyset(&blk); sigaddset(&blk, SIGALRM); sigaddset(&blk, SIGUSR2);
    sigprocmask(SIG_BLOCK, &blk, &old);   /* Linux: per-thread; serialize vs the pump's self-check dump */
    const char *path = g_sfx_trace_file ? g_sfx_trace_file : "/tmp/roth_sfx_trace.txt";
    FILE *f = fopen(path, "a");
    if (!f) { LOGE("[sfx-trace] cannot open %s\n", path); sigprocmask(SIG_SETMASK, &old, NULL); return; }
    uint32_t ms = sfx_now_ms();
    fprintf(f, "\n===== SFX-TRACE DUMP  t=%lu  ms=%u  reason=%s =====\n", g_mtick, ms, reason);
    /* voice-start session summary (the START-FAIL signature's running arithmetic) */
    fprintf(f, "-- voice-start session: attempts=%lu ok=%lu fail=%lu  consec-fail=%u  startfail-fired=%d "
               "(detector N=%u) --\n",
            g_sfx_vs_att, g_sfx_vs_ok, g_sfx_vs_fail, g_sfx_vs_consec, g_sfx_startfail_fired,
            g_sfx_startfail_n);
    /* live voice table */
    fprintf(f, "-- voice table (dev=%d cbq head=%u tail=%u depth=%u) --\n",
            g_sfx_dev, g_cbq_head, g_cbq_tail, (g_cbq_tail - g_cbq_head + SFX_CBQ) % SFX_CBQ);
    if (g_farg_base) for (unsigned v = 0; v < VOICE_N; v++) {
        const uint8_t *vp = (const uint8_t *)(uintptr_t)(g_farg_base + VOICE_OFF + v * VOICE_SZ);
        uint16_t fl = *(const uint16_t *)(vp + 0x30);
        uint32_t start = *(const uint32_t *)(vp + 0x00);
        if (!(fl & 0x8000) && !g_vact[v] && !g_vended[v] && start == 0) continue; /* skip empty slots */
        fprintf(f, "  v%02u bit15=%d start=%08x +08=%08x len=%u vol=%04x tag=%04x cb=%08x  "
                   "act=%d ended=%d cur=%u gen=%u\n",
                v, (fl & 0x8000) ? 1 : 0, start, *(const uint32_t *)(vp + 0x08),
                *(const uint32_t *)(vp + 0x18), *(const uint16_t *)(vp + 0x32),
                *(const uint16_t *)(vp + 0x34), *(const uint32_t *)(vp + 0x3c),
                g_vact[v], g_vended[v], g_vcur[v], g_vgen[v]);
    }
    /* once/sec health trend (whole session). free = idle voice slots (the pool-drain curve); vsAtt/vsOk
     * = voice-start attempts/successes THAT second (nonzero-with-no-free = the wedge tightening). */
    fprintf(f, "-- health log (once/sec: ms a=active w=wanted stuck cbq ring ur mixed free vsAtt vsOk) --\n");
    unsigned hs = (g_sfx_hl_cnt < SFX_HRING) ? 0 : g_sfx_hl_head;
    for (unsigned i = 0; i < g_sfx_hl_cnt; i++) {
        struct sfx_health_rec *h = &g_sfx_hl[(hs + i) % SFX_HRING];
        fprintf(f, "  %8u  a=%u w=%u stuck=%u cbq=%u ring=%u ur=%u mixed=%u free=%u vsAtt+%u vsOk+%u\n",
                h->ms, h->active, h->wanted, h->stuck, h->cbq, h->ring_level, h->underruns, h->mixed_frames,
                h->freeslots, h->vs_att_d, h->vs_ok_d);
    }
    /* recent lifecycle events (tail). VSTART/VSTARTFAIL: a=free b=stuck c=ok d=att#; STARTFAIL:
     * a=stuck b=consec c=fail d=att. */
    unsigned show = g_sfx_ev_cnt < 600u ? g_sfx_ev_cnt : 600u;
    unsigned es = (g_sfx_ev_head + SFX_EVRING - show) % SFX_EVRING;
    fprintf(f, "-- last %u lifecycle events (of %u; ms tick type v a b c d) --\n", show, g_sfx_ev_cnt);
    for (unsigned i = 0; i < show; i++) {
        struct sfx_ev_rec *e = &g_sfx_ev[(es + i) % SFX_EVRING];
        unsigned nnames = (unsigned)(sizeof sfxe_name / sizeof sfxe_name[0]);
        fprintf(f, "  %8u t%-7u %-11s v%02u a=%u b=%08x c=%08x d=%u\n",
                e->ms, e->tick, sfxe_name[e->type < nnames ? e->type : 0], e->voice, e->a, e->b, e->c, e->d);
    }
    fprintf(f, "===== end dump =====\n");
    fclose(f);
    LOGE("[sfx-trace] dumped (%s) -> %s\n", reason, path);
    sigprocmask(SIG_SETMASK, &old, NULL);
}

/* Per-tick health snapshot (once/sec) + the dead-SFX self-check. `mixed_frames` = sfx PCM frames the
 * mixer produced THIS tick (from the pump's g_au->w delta). Scans the 32 voices once (gated, cheap). */
static void sfx_trace_tick(uint32_t mixed_frames, int movie_active)
{
    if (!g_sfxtrace || !g_au) return;
    unsigned active = 0, wanted = 0, stuck = 0, freeslots = 0;
    if (g_farg_base) for (unsigned v = 0; v < VOICE_N; v++) {
        const uint8_t *vp = (const uint8_t *)(uintptr_t)(g_farg_base + VOICE_OFF + v * VOICE_SZ);
        uint16_t fl = *(const uint16_t *)(vp + 0x30);
        if (!(fl & 0x8000)) { freeslots++; continue; }
        active++;
        int is_stream = (*(const uint32_t *)(vp + 0x3c) == CANON(0x4e394));
        uint32_t start = *(const uint32_t *)(vp + 0x00);
        uint32_t len   = *(const uint32_t *)(vp + 0x18);
        if (!is_stream && start > 0x10000 && len >= 2) { wanted++; if (g_vended[v]) stuck++; }
    }
    uint32_t ring_level = g_au->w - g_au->r;
    unsigned cbq_depth = (g_cbq_tail - g_cbq_head + SFX_CBQ) % SFX_CBQ;

    /* once/sec health record */
    static uint32_t last_hl_ms;
    static uint32_t mixed_accum;
    static unsigned long last_vs_att, last_vs_ok;   /* for the per-second voice-start deltas */
    mixed_accum += mixed_frames;
    uint32_t ms = sfx_now_ms();
    if (ms - last_hl_ms >= 1000) {
        last_hl_ms = ms;
        struct sfx_health_rec *h = &g_sfx_hl[g_sfx_hl_head];
        h->ms = ms; h->active = (uint16_t)active; h->wanted = (uint16_t)wanted;
        h->stuck = (uint16_t)stuck; h->cbq = (uint16_t)cbq_depth;
        h->ring_level = ring_level; h->underruns = g_au->underruns; h->mixed_frames = mixed_accum;
        h->freeslots = (uint16_t)freeslots;
        h->vs_att_d = (uint16_t)(g_sfx_vs_att - last_vs_att);
        h->vs_ok_d  = (uint16_t)(g_sfx_vs_ok  - last_vs_ok);
        last_vs_att = g_sfx_vs_att; last_vs_ok = g_sfx_vs_ok;
        g_sfx_hl_head = (g_sfx_hl_head + 1) % SFX_HRING;
        if (g_sfx_hl_cnt < SFX_HRING) g_sfx_hl_cnt++;
        mixed_accum = 0;
    }

    /* dead self-check: SFX demanded (wanted>0) but no PCM produced this tick AND the ring is not just
     * cushioned (level < SFX_CUSHION => the consumer would starve). Hold wall-clock; fire on threshold. */
    static double dead_since = -1.0;
    static int dead_fired;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;
    int dead_now = (!movie_active && wanted > 0 && mixed_frames == 0 && ring_level < SFX_CUSHION);
    if (dead_now) {
        if (dead_since < 0) dead_since = now;
        if (!dead_fired && (now - dead_since) >= g_sfx_dead_secs) {
            dead_fired = 1;
            sfx_ev(SFXE_DEAD, 0, (uint16_t)wanted, stuck, cbq_depth, ring_level);
            LOGE("\n[sfx-trace] *** SFX-DEAD DETECTED *** %u voices wanted, %u done-cb-STUCK, cbq=%u, "
                 "ring=%u, no sfx PCM for %.1fs — auto-dumping\n",
                 wanted, stuck, cbq_depth, ring_level, now - dead_since);
            sfx_trace_dump("auto: SFX-DEAD self-check");
        }
    } else {
        if (dead_fired) { sfx_ev(SFXE_RESUME, 0, (uint16_t)wanted, stuck, cbq_depth, ring_level);
                          LOGE("[sfx-trace] SFX resumed (re-arming the dead self-check)\n"); }
        dead_since = -1.0; dead_fired = 0;
    }

    /* manual dump request (kill -USR2), polled here off the signal path */
    if (g_sfx_dump_req) { g_sfx_dump_req = 0; sfx_trace_dump("manual: SIGUSR2"); }
}

/* SIGUSR2 handler: async-signal-safe (sets a flag only; the pump polls + dumps off the signal path). */
static void sfx_trace_sigusr2(int sig) { (void)sig; g_sfx_dump_req = 1; }

/* Voice-START observability (v2 — the REAL dropout signature). Called from the imgfree voice-start
 * veneer (os_audio_standalone.c) with the native's return: 0xffffffff = NO FREE SLOT (the wedge), else
 * the slot index. Runs on the game/pump thread (same context as the mixer), so fopen/fprintf in the
 * auto-dump are safe. Counts attempts/successes/failures, snapshots the free-slot + stuck (done-cb-
 * latched) counts to the event ring, and fires the START-FAIL detector after N consecutive failures —
 * the mode where every new SFX silently fails to allocate a voice while established loops keep mixing
 * (invisible to the mixer scan: a failed start populates no voice struct). Gated: no-op when disarmed. */
void sfx_trace_voice_start(uint32_t result)
{
    if (!g_sfxtrace) return;
    int ok = (result != 0xffffffffu);
    g_sfx_vs_att++;
    /* free-slot + stuck (done-cb-latched) counts across the 32 voice structs. On a success the native
     * has already taken its slot, so `free` is the pool remaining AFTER this start (the drain curve);
     * on a failure all 32 are held, so free==0 and `stuck` is the wedge depth. */
    unsigned freeslots = 0, stuck = 0;
    if (g_farg_base) for (unsigned v = 0; v < VOICE_N; v++) {
        const uint8_t *vp = (const uint8_t *)(uintptr_t)(g_farg_base + VOICE_OFF + v * VOICE_SZ);
        if (!(*(const uint16_t *)(vp + 0x30) & 0x8000u)) { freeslots++; continue; }
        if (g_vended[v]) stuck++;
    }
    if (ok) {
        g_sfx_vs_ok++;
        if (g_sfx_startfail_fired)
            LOGE("[sfx-trace] voice-start recovered (slot %u; %u free) — re-arming START-FAIL detector\n",
                 result, freeslots);
        g_sfx_vs_consec = 0;
        g_sfx_startfail_fired = 0;
        sfx_ev(SFXE_VSTART, (uint8_t)result, (uint16_t)freeslots, stuck, 1, (uint32_t)g_sfx_vs_att);
    } else {
        g_sfx_vs_fail++;
        g_sfx_vs_consec++;
        sfx_ev(SFXE_VSTARTFAIL, 0xff, (uint16_t)freeslots, stuck, 0, g_sfx_vs_consec);
        if (!g_sfx_startfail_fired && g_sfx_vs_consec >= g_sfx_startfail_n) {
            g_sfx_startfail_fired = 1;
            sfx_ev(SFXE_STARTFAIL, 0xff, (uint16_t)stuck, g_sfx_vs_consec,
                   (uint32_t)g_sfx_vs_fail, (uint32_t)g_sfx_vs_att);
            LOGE("\n[sfx-trace] *** SFX START-FAIL DETECTED *** %u consecutive voice-start failures "
                 "(no free slot); %u/%u voices done-cb-STUCK; session att=%lu ok=%lu fail=%lu — auto-dumping\n",
                 g_sfx_vs_consec, stuck, VOICE_N, g_sfx_vs_att, g_sfx_vs_ok, g_sfx_vs_fail);
            sfx_trace_dump("auto: SFX START-FAIL self-check");
        }
    }
}

/* Dump-on-exit (v2): flush the trace once, however the process leaves. Called from the host clean-
 * return points (main.c, after roth_boot/roth_main return) AND before the SDL-window-close hard _exit
 * (traps.c shm_tick). Once-guarded so the two hook families never double-write; gated no-op disarmed. */
void sfx_trace_exit_dump(void)
{
    if (!g_sfxtrace) return;
    static int done;
    if (done) return;
    done = 1;
    sfx_trace_dump("exit (dump-on-quit)");
}

/* Wire the SFX-dropout trace from audio_init (env read + the manual-dump signal). SIGUSR2 is free
 * unless ROTH_TRACE (calltrace) is set; we only claim it when ROTH_SFX_TRACE is on AND ROTH_TRACE
 * is off, so the two debug tools never clobber each other's handler. */
static void sfx_trace_init(void)
{
    g_sfxtrace = (getenv("ROTH_SFX_TRACE") != NULL);
    if (!g_sfxtrace) return;
    g_sfx_trace_file = getenv("ROTH_SFX_TRACE_FILE");
    { const char *s = getenv("ROTH_SFX_DEAD_SECS");
      if (s) { double d = atof(s); if (d >= 0.5) g_sfx_dead_secs = d; } }
    { const char *s = getenv("ROTH_SFX_STARTFAIL_N");
      if (s) { long n = strtol(s, NULL, 0); if (n >= 1) g_sfx_startfail_n = (unsigned)n; } }
    if (getenv("ROTH_TRACE") == NULL) {
        struct sigaction sa; memset(&sa, 0, sizeof sa);
        sa.sa_handler = sfx_trace_sigusr2;   /* flag-set only; SA_RESTART so no syscall is disturbed */
        sa.sa_flags = SA_RESTART;
        sigaction(SIGUSR2, &sa, NULL);
    }
    LOGE("[sfx-trace] ROTH_SFX_TRACE=1 armed (v2): total-silence dead-hold %.1fs, START-FAIL after %u "
         "consecutive voice-start misses; dump -> %s (the file writes itself when you quit); "
         "manual dump = kill -USR2 %d%s\n",
         g_sfx_dead_secs, g_sfx_startfail_n,
         g_sfx_trace_file ? g_sfx_trace_file : "/tmp/roth_sfx_trace.txt", (int)getpid(),
         getenv("ROTH_TRACE") ? " (SIGUSR2 NOT claimed: ROTH_TRACE owns it — use auto-dump/exit-dump only)" : "");
}

/* Software-mix the active digital voices (in-game SFX) into the ring for one
 * timer tick. 16-bit mono voice samples -> 16-bit stereo. Called each MAGIC_POLL
 * tick while no movie stream is active. Returns 1 if it wrote PCM. */
static int audio_mix_sfx(void)
{
    if (!g_au || !g_farg_base)
        return 0;
    /* Volume-zero transition tracer (ROTH_VOL_TRACE): sample every voice's +0x32 BEFORE the cushion
     * early-returns below, so a slew during a cushioned window is captured tick-by-tick. No-op when
     * the flag is off; read-only when on. Must run every mixer entry, so it precedes the pacing. */
    vol_trace_scan();
    /* Pace like the movie path: when a consumer is draining the ring, top up to
     * a small cushion (smooths the per-tick sawtooth that otherwise crackles);
     * otherwise (headless / muted) produce a fixed tick so SFX still advance. */
    static uint32_t last_r;
    static int draining;
    if (g_au->r != last_r) {
        last_r = g_au->r;
        draining = 70;
    } else if (draining > 0) {
        draining--;
    }
    uint32_t n;
    if (draining) {
        uint32_t level = g_au->w - g_au->r;
        if (level >= SFX_CUSHION)
            return 0; /* cushioned: let it drain before producing more */
        n = (SFX_CUSHION - level) / 4u;        /* frames to refill the cushion */
        uint32_t cap = (g_au_rate / 70u) * 2u; /* but don't burst too far ahead */
        if (n > cap)
            n = cap;
    } else {
        n = g_au_rate / 70u;
    }
    if (n == 0)
        return 0;
    if (n > 1024)
        n = 1024;
    static int32_t acc[1024 * 2]; /* stereo accumulator (avoid mid-mix clip) */
    memset(acc, 0, n * 2 * sizeof acc[0]);
    int any = 0;
    for (unsigned v = 0; v < VOICE_N; v++) {
        uint8_t *vp =
            (uint8_t *)(uintptr_t)(g_farg_base + VOICE_OFF + v * VOICE_SZ);
        uint16_t fl = *(uint16_t *)(vp + 0x30);
        if (!(fl & 0x8000)) { /* inactive (game freed it) */
            if (g_sptrace && g_vspeech[v]) { /* a speech voice we were mixing just went inactive */
                SPLOG("v%u DEACTIVATE (bit15 clear) g_vcur=%u g_vended=%u +08=%08x  "
                      "(sos_stop_voice from the skip's force_end, or clip end)\n",
                      v, g_vcur[v], g_vended[v], *(uint32_t *)(vp + 0x08));
                sptrace_globals("deactivate");
            }
            if (g_sfxtrace && g_vact[v])   /* the game freed a voice we were mixing (active->free edge) */
                sfx_ev(SFXE_DEACT, (uint8_t)v, (uint16_t)g_vcur[v],
                       *(uint32_t *)(vp + 0x08), *(uint32_t *)(vp + 0x3c), g_vended[v]);
            g_vspeech[v] = 0;
            g_vact[v] = 0;
            g_vended[v] = 0;
            mixtrace_reset(v, "inactive (bit15 clear)", 0); /* self-gates the log on the active->free edge */
            g_veff_valid[v] = 0; /* drop the governor shadow with the voice */
            continue;
        }
        if (*(uint32_t *)(vp + 0x3c) == CANON(0x4e394)) {
            /* a movie-stream voice (ADPCM, driven by the decode path) — not a
             * PCM SFX; skip so we don't mix its compressed data as garbage. */
            g_vact[v] = 0;
            mixtrace_reset(v, "movie-stream voice", 0);
            g_veff_valid[v] = 0;
            continue;
        }
        uint32_t start = *(uint32_t *)(vp + 0x00);
        uint32_t len = *(uint32_t *)(vp + 0x18); /* bytes */
        int16_t vol = *(int16_t *)(vp + 0x32);   /* 0..0x7fff */
        if (start <= 0x10000 || len < 2) {
            g_vact[v] = 1;
            continue;
        }
        /* Detect a (re)start off the voice struct itself, not just our parallel
         * host state. voice_start (0x4a641 vs_fill) / voice_load_to_slot (0x4ad03
         * vl_fill) UNCONDITIONALLY write [S+0x08] = sample base on every start
         * (disasm 0x4a755 / 0x4ade1), and a looping done-cb rewinds it too — so
         * the game's current-ptr moving BACKWARD past where we last left it is the
         * authoritative "this voice just (re)started" signal. The real .386 driver
         * was stateless per voice: it advanced the cursor IN the struct (+0x08 vs
         * end +0x10), so a restart's reset was automatic. Our g_vcur/g_vact only
         * reset when the host OBSERVED the inactive (bit15-clear) edge — but the
         * game can free+reuse a slot, or re-fire a sound into a pool buffer that
         * the previous occupant used (DAS chunks are relocatable → the same `start`
         * recurs), between the host's intermittent mixer ticks (esp. across the
         * SFX_CUSHION early-return above). When that inactive edge is missed, the
         * old g_vcur (at end-of-sample) carries into the new sound → it reads as
         * instantly finished and its done-cb kills it at start. That is exactly
         * the overlapping-one-shot bug (2nd concurrent SFX dies; 1st plays out —
         * the longer 1st sound gives the host time to see its inactive edge, the
         * short 2nd doesn't). Resync from +0x08 so every voice_start is honored. */
        uint32_t cur08 = *(uint32_t *)(vp + 0x08);
        int rewound = (cur08 >= start && cur08 < start + g_vcur[v] * 2u);
        if (!g_vact[v] || g_vstart[v] != start || rewound) { /* fresh start / reuse / restart */
            if (g_mixtrace) {
                /* a re-seed while the voice was CONTINUOUSLY active (g_vact set) is a mid-play
                 * misfire: it makes eff snap back to the raw word every recurrence, defeating the
                 * governor's fps knob. A re-seed out of !g_vact is the legit new-onset case. */
                int mid = g_vact[v] && (g_vstart[v] != start || rewound);
                const char *why = !g_vact[v] ? "fresh onset (!g_vact)"
                                : (g_vstart[v] != start) ? "start changed (slot reuse)"
                                : "rewound (+0x08 moved back)";
                mixtrace_reset(v, why, mid);
            }
            /* §SPEECH-SKIP: the decisive line. For a speech voice (tag 0xedXX) this is line 2's onset;
             * `why` = which host restart condition fired and `g_vcur[v]` = the cursor being reset. If a
             * skip ever leaves this NOT firing (or firing with a nonzero pre-reset cursor that then does
             * NOT go to 0), that is where line 2's front is lost. Static analysis says one of the three
             * conditions always fires and g_vcur always lands 0 — this proves/refutes it live. */
            if (g_sptrace && (*(uint16_t *)(vp + 0x34) & 0xff00u) == 0xed00u) {
                const char *why = !g_vact[v] ? "fresh(!g_vact)"
                                : (g_vstart[v] != start) ? "start-changed"
                                : "rewound(+08<-back)";
                SPLOG("v%u RESTART[%s] g_vcur %u->0 gen->%u vstart=%08x start=%08x cur08=%08x len=%u\n",
                      v, why, g_vcur[v], g_vgen[v] + 1, g_vstart[v], start, cur08, len);
                sptrace_globals("restart");
            }
            g_vcur[v] = 0;
            g_vended[v] = 0;
            g_veff_valid[v] = 0; /* re-seed the governor to this instance's onset volume */
            g_vgen[v]++; /* new sound instance in this slot -> any queued done-cb
                          * for the previous occupant is now stale (delivery guard) */
            if (g_sfxtrace)
                sfx_ev(SFXE_START, (uint8_t)v, *(uint16_t *)(vp + 0x34), start, len,
                       *(uint32_t *)(vp + 0x3c));
        }
        g_vstart[v] = start;
        g_vact[v] = 1;
        g_vspeech[v] = ((*(uint16_t *)(vp + 0x34) & 0xff00u) == 0xed00u); /* track speech for the deactivate edge */
        if (g_vended[v]) /* finished; waiting on the done-cb to resolve (free/loop) */
            continue;
        const int16_t *s = (const int16_t *)(uintptr_t)start;
        uint32_t nsamp = len / 2u; /* 16-bit mono */
        uint32_t cur = g_vcur[v];
        any = 1;
        /* Read-side volume-slew governor (ROTH_VOL_SMOOTH, default on).
         * ROOT (original, disasm-verified): the play_object_sound path (0x270ca; the clank arrives via
         * play_entity_sound) never initializes the record's TARGET-volume field rec+0x12 — it writes
         * only rec+0x10 (APPLIED, via start_sound_voice_vol 0x275cc) and rec+0xc (dist, via
         * compute_sound_volume_pan 0x26f48). update_active_sounds (0x27b05) reads the target from
         * rec+0x12 and recomputes it ONLY when the emitter distance changed by >=16 (|delta|>>4); while
         * the player is stationary the target holds its leftover slot value (0 on a fresh record), so
         * the applied volume is slewed toward 0 at <=0x200/call and the still-active voice goes silent.
         * (Contrast play_sound_unique 0x273f0, which DOES store rec+0x12 — world sounds self-init.)
         * AMPLIFIER (host): on DOS update_active_sounds runs once per render frame (~15-30 fps), so
         * 0x45ff->0 spans ~35 frames (~1.2-2.3 s) and the ~0.7 s clank finishes audibly; the host game
         * loop is unthrottled and calls it dozens of times per mixer tick (the VOL_TRACE ring shows ~30
         * inferred 0x200-steps in ONE tick), completing the same slew in 1-2 ticks (~15-30 ms) -> the
         * clank is cut instantly. The ONLY game-side writer of +0x32 is that <=0x200/frame slew; stop
         * and deactivate paths mute via the voice active bit (word[+0x30] bit15) and +0x34, NOT +0x32.
         * So rate-limiting the mixer's READ of the +0x32 DECREASE to the DOS cadence reproduces the
         * DOS-era audible fade WITHOUT slowing any legitimate hard mute (those bypass +0x32 entirely).
         * Host-only shadow (g_veff); reads voice state, writes no game memory. Increases pass through
         * instantly (a louder onset is never the clip artifact), and the shadow is re-seeded to the
         * current +0x32 on every voice (re)start, so new sounds begin at their true onset volume. */
        /* The governor must keep decaying THROUGH vol==0 — the game's word
         * hitting literal 0 within 1-3 ticks IS the amplified artifact being smoothed. Dropping
         * the shadow at vol<=0 (the first draft) snapped eff to 0 and reproduced the clip ~30ms
         * later. The shadow is released only at the voice restart/inactive reset points. */
        int32_t eff = vol;
        if (g_vsmooth) {
            uint16_t v16 = (uint16_t)(vol > 0 ? vol : 0);
            if (!g_veff_valid[v]) {
                /* SEED: at a voice's first sighting after start/
                 * reuse, reconstruct the DOS onset when the raw is the stale-target-slew artifact
                 * (rec+0x12==0 and raw below the game's own compute); else seed at the raw onset.
                 * governor_seed() excludes speech/ambiguous-record and caps at 0x7fff. */
                g_veff[v] = governor_seed(v, vp, v16, NULL);
                g_veff_valid[v] = 1;
            } else if (v16 >= g_veff[v]) {
                g_veff[v] = v16;                     /* rise: instant (never the clip artifact) */
            } else {                                 /* fall: discriminate CLIFF vs legit fade */
                uint32_t gap = (uint32_t)g_veff[v] - v16;
                /* CLIFF-HOLD (MIXTRACE-proven, mixtrace_fps60.log t1204: raw plunges full->0
                 * between two mixer ticks — the stale-target artifact's signature; the game's
                 * slew runs ~30x per tick so its 0x200-steps arrive as one cliff here). A
                 * LEGITIMATE fade (walk-away distance recompute) descends gradually across many
                 * ticks and never looks like this. On a cliff-to-(near-)zero, HOLD the onset
                 * volume — matching DOSBox, where the ear hears no fade at all — until the
                 * sample ends or the voice deactivates (bit15/stop paths are unaffected).
                 * EXCEPTION: if the MASTER volume [0x71d84] is 0 (user muted in the menu), the
                 * zero is intentional — follow it so a mute is never held open. */
                uint16_t master = *(uint16_t *)(uintptr_t)CANON(0x71d84);
                if (v16 <= 0x100u && gap > 0x1000u && master != 0) {
                    /* hold g_veff[v] (no change) */
                } else {
                    uint32_t dec = (0x200u * g_vsmooth_fps * n) / g_au_rate;
                    if (dec == 0) dec = 1;
                    g_veff[v] = (uint16_t)(gap > dec ? (uint32_t)g_veff[v] - dec : v16);
                }
            }
            eff = g_veff[v];
        }
        /* MIX-TRACE (ROTH_MIX_TRACE): the ground-truth line — raw word[+0x32] vs governor eff vs the
         * seed flag, for this actively-mixed voice. eff>raw & falling by ~dec/tick => governor clamping
         * as designed (fps-sensitive); eff==raw => governor not biting (bypass, or raw slower than the
         * clamp) => fps-insensitive, matching the report. Emit only on a raw/eff change (the
         * descent, compactly) or the first tick after a (re)seed. Read-only. */
        if (g_mixtrace) {
            uint16_t raw = (uint16_t)(vol > 0 ? vol : 0);
            if (!g_mt_seen[v] || raw != g_mt_lastraw[v] || eff != g_mt_lasteff[v]) {
                if (g_mt_lines < MIX_TRACE_MAX) {
                    g_mt_lines++;
                    uint32_t dec = g_vsmooth ? (0x200u * g_vsmooth_fps * n) / g_au_rate : 0;
                    fprintf(stderr,
                            "[audio] MIXTRACE t%lu v%u raw=%5u eff=%5d valid=%u dec/tick=%u n=%u"
                            " cur=%u/%u fl=%04x\n",
                            g_mtick, v, raw, eff, g_veff_valid[v], dec, n, cur, nsamp, fl);
                }
                g_mt_lastraw[v] = raw;
                g_mt_lasteff[v] = eff;
            }
            g_mt_seen[v] = 1;
        }
        for (uint32_t i = 0; i < n && cur < nsamp; i++, cur++) {
            if (eff > 0) { /* eff<=0: advance silently (still ends + frees handle) */
                int32_t smp = ((int32_t)s[cur] * eff) >> 15; /* volume scale */
                acc[i * 2] += smp;     /* L */
                acc[i * 2 + 1] += smp; /* R (center pan) */
            }
        }
        g_vcur[v] = cur;
        /* Mirror our cursor into the game's current-ptr (+0x08): on completion a
         * looping sound's done-cb resets it back to start, which is how we tell
         * a loop from a one-shot in MAGIC_AFTER. */
        *(uint32_t *)(vp + 0x08) = start + cur * 2u;
        if (cur >= nsamp) { /* finished: fire the done-cb ONCE, with the voice
                             * STILL active so its lookup + handle-count decrement
                             * work; MAGIC_AFTER then frees it (or detects a loop). */
            {
                static int el;
                if (el++ < 40)
                    ALOGV("t%lu VOICE %u END played=%u nsamp=%u cb=%08x\n", g_mtick,
                         v, cur, nsamp, *(uint32_t *)(vp + 0x3c));
            }
            if (g_sptrace && (*(uint16_t *)(vp + 0x34) & 0xff00u) == 0xed00u) {
                SPLOG("v%u BUFFER-END played=%u nsamp=%u +00=%08x cb=%08x -> queue done-cb\n",
                      v, cur, nsamp, *(uint32_t *)(vp + 0x00), *(uint32_t *)(vp + 0x3c));
                sptrace_globals("buffer-end");
            }
            uint32_t cboff = *(uint32_t *)(vp + 0x3c);
            if (g_sfxtrace)
                sfx_ev(SFXE_END, (uint8_t)v, *(uint16_t *)(vp + 0x34), cur, nsamp, cboff);
            unsigned nx = (g_cbq_tail + 1) % SFX_CBQ;
            if (cboff && sfx_device() >= 0 && nx != g_cbq_head) {
                g_vended[v] = 1; /* don't mix or re-queue until resolved */
                g_cbq[g_cbq_tail].off = cboff;
                g_cbq[g_cbq_tail].voice = (uint16_t)v;
                g_cbq[g_cbq_tail].gen = g_vgen[v];  /* identity: this sound instance */
                g_cbq[g_cbq_tail].start = start;    /* secondary identity check */
                g_cbq[g_cbq_tail].qtag =
                    *(uint16_t *)(vp + 0x34);       /* the record index the cb reads LIVE (0x27501) */
                g_cbq[g_cbq_tail].qfree =
                    (uint8_t)sfx_end_frees_record(vp); /* action-2-class end? (frees rec[qtag]) */
                g_cbq_tail = nx;
                if (g_sfxtrace)
                    sfx_ev(SFXE_CBQ, (uint8_t)v, *(uint16_t *)(vp + 0x34),
                           g_cbq[(g_cbq_tail + SFX_CBQ - 1) % SFX_CBQ].qfree,
                           (g_cbq_tail - g_cbq_head + SFX_CBQ) % SFX_CBQ, cboff);
            } else { /* no cb available: just free the voice directly */
                if (g_sfxtrace)   /* cbq FULL or no device -> voice freed with no callback */
                    sfx_ev(SFXE_CBDROP, (uint8_t)v, 0xffff, sfx_device() < 0,
                           (g_cbq_tail - g_cbq_head + SFX_CBQ) % SFX_CBQ, cboff);
                *(uint16_t *)(vp + 0x30) = (uint16_t)(fl & ~0x8000u);
                g_vact[v] = 0;
            }
        }
    }
    if (!any)
        return 0;
    uint32_t freeb = ROTH_AUDIO_RING - (g_au->w - g_au->r);
    uint32_t frames = (n * 4u <= freeb) ? n : freeb / 4u;
    static int16_t out[1024 * 2];
    for (uint32_t i = 0; i < frames * 2u; i++) {
        int32_t x = acc[i];
        if (x > 32767)
            x = 32767;
        else if (x < -32768)
            x = -32768;
        out[i] = (int16_t)x;
        g_au->ring[(g_au->w + i * 2) & ROTH_AUDIO_MASK] = (uint8_t)x;
        g_au->ring[(g_au->w + i * 2 + 1) & ROTH_AUDIO_MASK] = (uint8_t)(x >> 8);
    }
    g_au->w += frames * 4u;
    if (g_pcm_fd >= 0)
        (void)!write(g_pcm_fd, out, frames * 4u);
    if (frames < n)
        g_au->underruns++;
    return 1;
}

/* ===== extracted driver services (no cpu_t) ============================================
 * The M4 virtual driver's .386-function responses, lifted out of audio_trap's switch bodies into
 * callable host functions. audio_trap KEEPS calling them (so the image-based trap path is
 * byte-identical — this is extraction, not rewriting); the C2 host binding (os_audio.c) calls
 * the SAME functions directly instead of via a MAGIC fault. This is the audio analogue of
 * reusing dos_int21. */

/* fn 2 / fn 8 "detect/probe card": stage the SB16 descriptor in the far-args seg at DESC_OFF.
 * Returns DESC_OFF (the offset the SOS reads back in EDX). `base` must be a valid linear base. */
uint32_t haudio_detect_card(uint32_t base)
{
    memcpy((void *)(uintptr_t)(base + DESC_OFF), SB16_DESC, sizeof SB16_DESC);
    return DESC_OFF;
}

/* Image-free equivalent of find_driver_for_device's (0x48f79) fn=2 detect + the SOS 0x4fece copy:
 * stage the SB16 descriptor directly into the client's `out` far pointer (es:edi = dpmi_sel_base(sel)
 * + out) and patch the 4 driver-fnptr selectors (+0x44/+0x4c/+0x54/+0x5c) to the client DS `sel`. The
 * image-based trap path is unchanged (this new service is NOT called by audio_trap): call_orig
 * find_driver still runs the library, whose fn=2 far-call lands at MAGIC_OFF -> haudio_detect_card
 * stages SB16_DESC at the far-args seg, then 0x4fece rep-movs 0x6a bytes to `out` and patches the same
 * 4 selectors (0x4fef6/0x4fefb..0x4ff0a) — byte-for-byte identical to what this stages directly. Used
 * only by os_audio.c's os_audio_find_driver_for_device_native. */
void haudio_stage_driver_descriptor(uint32_t out, uint16_t sel)
{
    uint32_t lin = dpmi_sel_base(sel) + out;   /* mirror es:edi; flat DS base 0 => lin == out */
    if (!lin)
        return;
    memcpy((void *)(uintptr_t)lin, SB16_DESC, sizeof SB16_DESC); /* 0x4fef6 rep movsb 0x6a */
    for (uint32_t o = 0x44; o <= 0x5c; o += 8)                   /* 0x4fefb..0x4ff0a: 4 fnptr sels -> DS */
        *(uint16_t *)(uintptr_t)(lin + o) = sel;
    /* Image-free boot-log parity with the trap lane's audio_trap "fn2 -> SB16 descriptor staged"
     * line: the natives stage silently, which read as "install never happened". Gated on
     * g_standalone_boot so the trap-lane boot log is byte-unchanged. */
    if (g_standalone_boot)
        ALOG("c2 detect (fn2-equivalent) -> SB16 descriptor staged image-free at %#x:%#x\n", sel, out);
}

/* fn 0xa "open driver": bind `fbase` as the voice/position staging segment, zero the 32 voice
 * structs + play-position dword, and report the three fn-0xa outputs: *out_cb = the per-tick
 * output callback (code), *out_voices = base offset of the 32 voice structs, *out_pos = the
 * play-position dword offset (all relative to fbase). */
void haudio_open_driver(uint32_t fbase, uint32_t reqsz, uint32_t *out_cb,
                        uint32_t *out_voices, uint32_t *out_pos)
{
    g_dma_reqsz = reqsz;
    g_farg_base = fbase;
    g_pos_lin = fbase ? fbase + POS_OFF : 0;
    if (fbase)
        memset((void *)(uintptr_t)(fbase + VOICE_OFF), 0,
               VOICE_N * VOICE_SZ + 4); /* clean voices + zero position */
    /* (Re)opening the driver — which also happens across an in-game load — makes any
     * in-flight software-mixer state obsolete: clear the per-voice cursors and the
     * pending done-cb queue so a callback queued before the reset can't fire against
     * a freshly recycled slot. Belt-and-suspenders next to the per-delivery
     * generation guard; the stream-drive flags (g_in_drive / g_after_stream /
     * g_stream_fed) are deliberately left untouched (the movie path owns those). */
    memset(g_vcur, 0, sizeof g_vcur);
    memset(g_vstart, 0, sizeof g_vstart);
    memset(g_vact, 0, sizeof g_vact);
    memset(g_vended, 0, sizeof g_vended);
    memset(g_vgen, 0, sizeof g_vgen);
    g_cbq_head = g_cbq_tail = 0;
    g_cb_voice = -1;
    *out_cb = MAGIC_POLL + 0x00;
    *out_voices = VOICE_OFF;
    *out_pos = POS_OFF;
}

/* ===== C2 open-driver service — the open_voices (0x47dae) linchpin's host half ================
 * The veneer path runs the original open_voices, whose transitive closure allocates two DPMI
 * segments (0x54441 -> 0x4fc0f the DMA real-mode seg; 0x5473c the far-args seg fn-0xa uses),
 * far-calls the dispatch-computer (-> {MAGIC_OFF, game CS}) and fn-0xa (haudio_open_driver -> cb =
 * MAGIC_POLL, voices @ VOICE_OFF, pos @ POS_OFF), then the game code fans those runtime values into
 * the per-slot bookkeeping + the 0x97440 far-ptr table the voice natives read. This service supplies
 * that runtime half image-free: it allocates the same segments as REAL host-backed LDT selectors and
 * runs the SAME fn-0xa core (haudio_open_driver), so the values are structurally identical to the
 * trapped original — the DMA/streaming buffers are moot (the host mixer reads g_farg_base voice
 * structs directly), but their selectors are valid so nothing faults if the SOS/close path derefs
 * them. dpmi_note_sel_base binds the far-args base into the software cache the natives translate
 * through (dpmi_sel_base). The segments are allocated ONCE and reused across open/close cycles (the
 * veneer leaks its DPMI frees too — dpmi.c 0x0502/0x0101 are no-ops), so an in-game reload rebinds
 * the same selectors: no selector churn, no leak. */
static uint32_t g_svc_farg_base;   /* far-args seg linear base (== g_farg_base after fn-0xa) */
static uint16_t g_svc_farg_sel;    /* its LDT selector */
static uint32_t g_svc_dma_base;    /* moot DMA real-mode seg base */
static uint16_t g_svc_dma_sel;
static uint16_t g_svc_bufb_sel;    /* moot streaming decode-buffer-B selector */
static uint32_t g_svc_bufb_base;

/* mmap a 32-bit-addressable host region + a real LDT selector over it, and register the base in
 * dpmi.c's software cache. Returns the linear base (0 on failure) and *out_sel. */
static uint32_t svc_alloc_seg(uint32_t size, uint16_t *out_sel)
{
    void *p = sys_lowmem_alloc((size + 0xfffu) & ~0xfffu);
    if (!p)
        return 0;
    uint32_t base = (uint32_t)(uintptr_t)p;
    int sel = ldt_alloc(base, size ? size - 1 : 0);
    if (sel < 0) {
        munmap(p, (size + 0xfffu) & ~0xfffu);
        return 0;
    }
    dpmi_note_sel_base((uint16_t)sel, base);
    *out_sel = (uint16_t)sel;
    return base;
}

int haudio_open_voices_service(uint32_t reqsz, struct haudio_open_desc *d)
{
    if (!g_svc_farg_base) {
        /* far-args seg: VOICE_OFF(0x40) + 32*0x6c voice structs + the play-position dword; 0x2000
         * of slack. Allocated once; reused. */
        g_svc_farg_base = svc_alloc_seg(0x2000, &g_svc_farg_sel);
        if (!g_svc_farg_base)
            return 0xf;                       /* mirrors 0x5473c's open-fail return (0xf) */
    }
    if (!g_svc_dma_base) {
        g_svc_dma_base = svc_alloc_seg(0x2000, &g_svc_dma_sel);
        if (!g_svc_dma_base)
            return 0xf;
    }
    if (!g_svc_bufb_base) {
        g_svc_bufb_base = svc_alloc_seg(0x2000, &g_svc_bufb_sel);
        if (!g_svc_bufb_base)
            return 0xf;
    }

    uint32_t cb, voices, pos;
    haudio_open_driver(g_svc_farg_base, reqsz, &cb, &voices, &pos); /* fn-0xa staging (shared core) */

    d->farg_sel     = g_svc_farg_sel;
    d->farg_off     = 0;
    d->cb_sel       = g_game_cs ? g_game_cs : 0x23; /* game CS; 0x23 = flat ring-3 code sel (r1) */
    d->cb_off       = cb;                            /* MAGIC_POLL */
    d->voices_off   = voices;                        /* VOICE_OFF */
    d->pos_off      = pos;                           /* POS_OFF */
    d->dispatch_off = MAGIC_OFF;                     /* dispatch-computer result offset */
    d->dma_sel      = g_svc_dma_sel;
    d->dma_off      = 0;
    d->bufb_sel     = g_svc_bufb_sel;
    d->bufb_off     = 0;
    /* Image-free boot-log parity with the trap lane's "fn0xa -> cb=MAGIC_POLL voices@FS:.. pos@FS:.."
     * line (the sample-source staging the host stream reads). g_standalone_boot-gated: trap log unchanged. */
    if (g_standalone_boot)
        ALOG("c2 open-driver (fn0xa-equivalent) -> cb=MAGIC_POLL voices@FS:%#x pos@FS:%#x "
             "reqsz=%#x FS=%#x base=%#x (image-free)\n", voices, pos, reqsz,
             g_svc_farg_sel, g_svc_farg_base);
    return 0;
}

/* ===== driver-install service — the alloc/free driver-slot (0x44553/0x44a81) linchpin's host half.
 * See audio.h for the full model. A single host-backed selector stands in for the veneer's per-slot DPMI
 * descriptor: allocated ONCE, reused across install/teardown cycles (the veneer leaks its DPMI frees too —
 * 0x4fbd2/0x4fc9d are host no-ops), so an in-game music reload rebinds the same deterministic handle. */
static uint32_t g_svc_drv_base;   /* driver-descriptor host segment linear base (alloc-once) */
static uint16_t g_svc_drv_sel;    /* its LDT selector = the deterministic [0x920dc] DPMI handle */

/* Stamp the 12 SOS MIDI driver vtable methods [vtbl_lin + N*6] = {MAGIC_MIDI+N*4, sel} — the observable
 * end-state 0x541ad's driver far-call + copy-loop leaves (r1: [0x92f9c..0x92fe0]=e0d4000N, sel 0x23). The
 * same MAGIC_MIDI+N*4 sequence haudio_midi_load_table writes into the driver's SOURCE fn table (stride 8,
 * offsets only); here it lands straight in the per-slot vtable (stride 6, {off32,sel16}) so every far-call
 * to a stamped method faults at its MAGIC_MIDI page and audio_trap routes it to haudio_midi_send. */
static void haudio_stamp_midi_vtable(uint32_t vtbl_lin, uint16_t sel)
{
    for (unsigned i = 0; i < MIDI_NFN; i++) {
        *(uint32_t *)(uintptr_t)(vtbl_lin + i * 6)     = MAGIC_MIDI + i * 4;
        *(uint16_t *)(uintptr_t)(vtbl_lin + i * 6 + 4) = sel;
    }
}

int haudio_driver_install_service(uint32_t slot, uint32_t device, struct haudio_driver_desc *d)
{
    (void)device;
    if (!g_svc_drv_base) {
        g_svc_drv_base = svc_alloc_seg(0x1000, &g_svc_drv_sel);
        if (!g_svc_drv_base)
            return 0xf;                          /* mirrors 0x51681's crt_open-fail return (0xf) */
    }
    /* (b) stamp the per-slot MAGIC vtable directly (replaces 0x541ad's driver far-call + copy loop). The
     * vtable selector = the driver-code selector (r1: 0x23); the game CS captured off any audio fault is
     * that flat ring-3 code sel. The MAGIC_MIDI offset is selector-agnostic (audio_trap traps by EIP). */
    uint16_t vsel = g_game_cs ? g_game_cs : 0x23;
    haudio_stamp_midi_vtable(CANON(0x92f9c + slot * 0x48), vsel);
    /* (a)/(c) the moot-but-valid descriptor values 0x51681 produces and 0x5189d consumes. All are read
     * ONLY by DPMI real-mode plumbing that is a host no-op, so a stable host selector is byte-immaterial
     * and deterministic run-to-run (unlike the veneer's non-deterministic DPMI handle). */
    d->dpmi_handle = g_svc_drv_sel;
    d->lock_handle = g_svc_drv_sel;
    d->chan_byte   = 0;
    d->fptrA_sel   = g_svc_drv_sel;
    d->fptrA_off   = 0;
    d->fptrB_sel   = vsel;                        /* the vtable/driver-code selector */
    d->fptrB_off   = g_svc_drv_base;             /* a valid mapped driver-descriptor linear addr */
    /* Image-free boot-log parity (see haudio_stage_driver_descriptor): the trap lane logs its music
     * install via the int3 stub / MAGIC dispatch; the native stages silently. Trap log unchanged. */
    if (g_standalone_boot)
        ALOG("c2 driver-install -> slot %u dev %#x: MAGIC_MIDI vtable stamped image-free (sel %#x)\n",
             slot, device, vsel);
    return 0;
}

/* audio_trace.c snapshot hook: host-linear base + byte span of the 32 voice structs (see audio.h).
 * The far-ptr table [0x97440] spreads these same structs (offset VOICE_OFF+i*VOICE_SZ, fargSel), so
 * dpmi_sel_base(fargSel)+offset == this base + i*VOICE_SZ — the same linear range voice_start writes.
 * Pure read of g_farg_base; no behavior change. */
uint32_t haudio_voice_struct_base(uint32_t *out_span)
{
    if (out_span)
        *out_span = VOICE_N * VOICE_SZ;
    return g_farg_base ? g_farg_base + VOICE_OFF : 0;
}

/* MIDI fn 0: stage a zeroed 64-byte descriptor at mbase+woff (the thunk rep-movs 0x40 from here). */
void haudio_midi_load_descriptor(uint32_t mbase, uint32_t woff)
{
    if (mbase)
        memset((void *)(uintptr_t)(mbase + woff), 0, 0x40);
}

/* MIDI fn 1: write the 12-entry handler table (stride 8) at mbase+woff -> the MAGIC_MIDI pages, so
 * every MIDI event faults there and is captured by haudio_midi_event. */
void haudio_midi_load_table(uint32_t mbase, uint32_t woff)
{
    if (mbase)
        for (unsigned i = 0; i < MIDI_NFN; i++)
            *(uint32_t *)(uintptr_t)(mbase + woff + i * 8) = MAGIC_MIDI + i * 4;
}

/* MIDI event capture: push a channel-voice message (status 0x80..0xEF) to the viewer's SoundFont
 * synth ring. SysEx (0xF0+) is dropped. b3 is unused (kept for the 4-byte call shape). */
void haudio_midi_event(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3)
{
    (void)b3;
    if (g_midi && b0 >= 0x80 && b0 < 0xf0) {
        uint32_t w = g_midi->w;
        if (w - g_midi->r < ROTH_MIDI_RING) { /* drop on overrun */
            struct roth_midi_ev *e = &g_midi->ev[w & ROTH_MIDI_MASK];
            e->status = b0; e->d1 = b1; e->d2 = b2; e->pad = 0;
            g_midi->w = w + 1;
        }
    }
}

/* Read the 4-byte MIDI message at (moff:msel), push channel messages to the synth ring, and return
 * the four bytes packed b0 | b1<<8 | b2<<16 | b3<<24 (0 if the pointer is unreadable). Extracted
 * VERBATIM from audio_trap's MAGIC_MIDI case (below) — same selector-base read, same reachability
 * guard, same haudio_midi_event push — so the C2 host binding reproduces it byte-identically (the
 * driver_dispatch_simple retirement) and audio_trap keeps calling it (the trap path is UNCHANGED). */
uint32_t haudio_midi_send(uint32_t moff, uint16_t msel)
{
    uint32_t sb = dpmi_sel_base(msel);
    uint32_t mlin = sb + moff;
    uint8_t b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    if ((mlin >= OBJ1_BASE && mlin + 4 <= STACK_TOP) ||
        (mlin >= DOSMEM_LIN && mlin + 4 <= DOSMEM_LIN + DOSMEM_SIZE) ||
        (mlin >= 0xf3000000u && mlin < 0xf4000000u) || /* DPMI song buf (legacy window, kept so the
                                                        * previously-working lanes are unchanged) */
        dpmi_lin_alloc_contains(mlin, 4) ||            /* the REAL DPMI 0501 ledger (task #107): the
                                                        * kernel places the song buffer wherever mmap
                                                        * likes — under the windowed SDL layout it sat
                                                        * BELOW the legacy window, so every NOTE event
                                                        * (dispatched by raw stream pointer, unlike the
                                                        * CC7s staged in a canon global) was silently
                                                        * dropped -> music-specific silence */
        (sb && moff < 0x40000)) {
        const uint8_t *m = (const uint8_t *)(uintptr_t)mlin;
        b0 = m[0]; b1 = m[1]; b2 = m[2]; b3 = m[3];
    }
    haudio_midi_event(b0, b1, b2, b3);
    return (uint32_t)b0 | ((uint32_t)b1 << 8) | ((uint32_t)b2 << 16) | ((uint32_t)b3 << 24);
}

/* Reproduce driver_dispatch_simple (canon 0x45d28) for the image-free C2 host binding. The SOS
 * routine far-calls the driver-slot vtable [slot*0x48 + 0x92f9c] with the message far-pointer
 * (param:sel) on the stack, then UNCONDITIONALLY returns 0 (disasm 0x45d5b). alloc_driver_slot
 * populated slot 0 (the MIDI card 0xa004) with a vtable whose entry offset is MAGIC_MIDI+0 (captured
 * live: [0x92f9c]=0xe0d40000), so that far-call lands in audio_trap's MAGIC_MIDI case — i.e. exactly
 * haudio_midi_send on the message. We read the LIVE vtable-entry offset for `slot` and route to the
 * MIDI service only when it points into the MAGIC_MIDI handler table; anything else contributes no
 * game-memory writes (the trace's 9 boot calls showed an empty write-set) and returns 0. */
/* Generalized to any of the 12 per-slot vtable methods (stride 6 {off32,sel16}). Reads the LIVE method
 * entry for `slot`; routes to the MIDI service (synth ring) ONLY when it points into the MAGIC_MIDI
 * handler table — else no game-memory effect. driver_dispatch_simple is method 0; alloc's driver-init
 * dispatch is method 1; free's close dispatch is method 2. The far-call in the veneer path faults
 * at MAGIC_MIDI and audio_trap runs the same haudio_midi_send, so this reproduces it byte-identically. */
uint32_t haudio_dispatch_method(uint32_t slot, unsigned method, uint32_t param, uint16_t sel)
{
    uint32_t vtoff = *(uint32_t *)(uintptr_t)CANON(0x92f9c + slot * 0x48 + method * 6);
    if (vtoff >= MAGIC_MIDI && vtoff < MAGIC_MIDI + MIDI_NFN * 4)
        haudio_midi_send(param, sel);
    return 0;
}

uint32_t haudio_dispatch_simple(uint32_t slot, uint32_t param, uint16_t sel)
{
    return haudio_dispatch_method(slot, 0, param, sel);
}

int audio_trap(cpu_t *c)
{
    uint32_t eip = R_EIP(c);

    /* Capture the live game code selector (base-0 flat CS) off any audio fault. The open-driver
     * service stamps it into the poll-cb far-ptr the still-original timer ISR far-calls. */
    {
        uint16_t cs = (uint16_t)c->uc->uc_mcontext.gregs[REG_CS];
        if (cs)
            g_game_cs = cs;
    }

    /* (1) dispatch-computer entry int3 (SIGTRAP delivers eip = site+1).
     * Return MAGIC far-pointer: EAX = offset, EDX = selector (the SOS stores
     * EAX->[0x97af8] offset, EDX->[0x97afc] selector). Use the live CS as the
     * dispatch selector (valid flat code segment, base 0). cdecl: caller pops
     * its 2 args via `add esp,8`, so we only pop our return address. */
    if (eip == HMI_DISPATCH_COMPUTER + 1) {
        uint32_t esp = R_ESP(c);
        uint32_t ret = *(uint32_t *)(uintptr_t)esp;
        uint32_t cs = (uint32_t)c->uc->uc_mcontext.gregs[REG_CS];
        R_EAX(c) = MAGIC_OFF;
        R_EDX(c) = cs;
        R_EIP(c) = ret;
        R_ESP(c) = esp + 4;
        ALOG("dispatch-computer hit -> dispatch = {off=%#x, sel=%#x} (ret 0x%x)\n",
             MAGIC_OFF, cs, ret - OBJ_DELTA);
        return 1;
    }

    /* (1b) music-init stub: return error in EAX so the caller skips MIDI.
     * Entry is `push esi/edi/ebp; ...; ret 0x14` -> 5 stack args (callee-clean).
     * At the int3 (before the prologue) [esp] = return address. */
    if (g_defer_midi && eip == HMI_MUSIC_INIT + 1) {
        uint32_t esp = R_ESP(c);
        uint32_t ret = *(uint32_t *)(uintptr_t)esp;
        R_EAX(c) = 6; /* "music card out of range" */
        R_EIP(c) = ret;
        R_ESP(c) = esp + 4 + 0x14; /* emulate `ret 0x14` */
        ALOG("music-init stubbed -> no music (ret 0x%x)\n", ret - OBJ_DELTA);
        return 1;
    }

    /* (1c) null audio callback guard. With MIDI deferred, the unified audio
     * service (0x49eaf, called by the HMI timer) and the track-start path still
     * far-call MIDI slots/instances whose pointer is null -> the CPU jumps to
     * linear 0 and faults fetching there. The SOS voice-driver dispatch (e.g.
     * stop-voice 0x4ac55, which far-calls a voice descriptor's driver pointer)
     * does the same for an unconfigured voice — landing at a small linear offset
     * from a null base (seen: eip=0x80). Treat any far-call landing below the EXE
     * image (eip < 0x1000) as a no-op missing-driver callback: return 0 and
     * far-ret to skip it. (Real callbacks resolve to EXE addresses or MAGIC_OFF.) */
    if (eip < 0x1000) {
        uint32_t esp = R_ESP(c);
        const uint32_t *st = (const uint32_t *)(uintptr_t)esp;
        R_EAX(c) = 0;
        far_ret(c);
        ALOGV("null MIDI callback skipped (ret 0x%x)\n",
             (st[0] - OBJ_DELTA) & 0xffffffff);
        return 1;
    }

    /* (1c2) return from a host-initiated streaming-completion far-call: the
     * callee retf'd here leaving its 3 cdecl args on the stack. Drop them, then
     * finish the original driver-callback far-return that we deferred. */
    if (eip == MAGIC_AFTER) {
        g_in_drive = 0;
        /* 0x4e394 just ADPCM-decoded one block to *(0x91d4c) as 16-bit
         * interleaved stereo (count = 0x91d28>>1 frames). Ship it to the ring —
         * but ONLY when we got here from the stream drive. SFX done-callbacks
         * also return through MAGIC_AFTER; for those the decode buffer is stale
         * (garbage) and copying it produces a beep at every sound's end. */
        if (g_after_stream && g_au) {
            uint32_t decbuf = *(uint32_t *)(uintptr_t)CANON(0x91d4c);
            uint32_t frames = (*(uint32_t *)(uintptr_t)CANON(0x91d28)) >> 1;
            if (decbuf > 0x10000 && frames && frames < 0x4000) {
                uint32_t n = frames * 4u; /* 16-bit * 2ch */
                const uint8_t *p = (const uint8_t *)(uintptr_t)decbuf;
                uint32_t freeb = ROTH_AUDIO_RING - (g_au->w - g_au->r);
                if (n > freeb) {
                    n = freeb;
                    g_au->underruns++; /* ring full: viewer absent or slow */
                }
                /* MOVIE volume [0x91cb0] applied here (same rule as the imgfree ship loop; the
                 * original applies it at the SOS voice +0x32, which this ship path bypasses). */
                { uint32_t mv = *(uint32_t *)(uintptr_t)CANON(0x91cb0);
                  int32_t vol = (mv != 0 && mv < 0x6ff0u) ? (int32_t)mv : 0x7fff;
                  for (uint32_t i = 0; i + 1 < n; i += 2) {
                      int16_t s = (int16_t)(p[i] | ((uint16_t)p[i + 1] << 8));
                      s = (int16_t)(((int32_t)s * vol) >> 15);
                      g_au->ring[(g_au->w + i) & ROTH_AUDIO_MASK] = (uint8_t)s;
                      g_au->ring[(g_au->w + i + 1) & ROTH_AUDIO_MASK] = (uint8_t)((uint16_t)s >> 8);
                  } }
                g_au->w += n;
                if (g_pcm_fd >= 0)
                    (void)!write(g_pcm_fd, p, n);   /* debug tap stays raw */
                g_stream_fed = 14; /* a movie owns the ring; SFX yield ~0.2s */
            }
        } else if (g_cb_voice >= 0 && g_farg_base) {
            /* An SFX done-callback just ran (with the voice still active, so its
             * handle-count decrement worked). Now resolve: if the cb restarted a
             * looping sound it reset the current-ptr (+0x08) to start -> just
             * rewind our cursor; otherwise it was a one-shot -> free the voice. */
            uint8_t *vp =
                (uint8_t *)(uintptr_t)(g_farg_base + VOICE_OFF + g_cb_voice * VOICE_SZ);
            uint16_t sid = *(uint16_t *)(vp + 0x34);
            if ((sid & 0xff00u) == 0xed00u) {
                /* Speech: the type-0 callback re-queued the alternate (already
                 * decoded) half — resume on the swapped buffer (+0x00 changed).
                 * type-2 ended the clip — deactivate (the game stops the voice). */
                if (g_cb_action == 0) {
                    g_vstart[g_cb_voice] = *(uint32_t *)(vp + 0x00);
                    g_vcur[g_cb_voice] = 0;
                } else {
                    *(uint16_t *)(vp + 0x30) &= ~0x8000u;
                    g_vact[g_cb_voice] = 0;
                }
                SPLOG("v%d done-cb RESOLVED action=%u -> %s  g_vstart=%08x g_vcur=%u +00=%08x\n",
                      g_cb_voice, g_cb_action,
                      g_cb_action == 0 ? "continue(swap buffer, cur->0)" : "end(deactivate)",
                      g_vstart[g_cb_voice], g_vcur[g_cb_voice], *(uint32_t *)(vp + 0x00));
            } else {
                uint32_t start = *(uint32_t *)(vp + 0x00);
                uint32_t cur08 = *(uint32_t *)(vp + 0x08);
                if (cur08 <= start + 4) { /* looped: cb rewound the current-ptr */
                    g_vcur[g_cb_voice] = 0;
                } else { /* one-shot finished: deactivate (cb freed the handle) */
                    *(uint16_t *)(vp + 0x30) &= ~0x8000u;
                    g_vact[g_cb_voice] = 0;
                }
            }
            g_vended[g_cb_voice] = 0;
            g_cb_voice = -1;
        }
        R_ESP(c) += 12;
        far_ret(c);
        return 1;
    }

    /* (1d) digital poll callbacks (returned by fn 0xa; the unified audio service
     * far-calls them each timer tick). The main output poll is MAGIC_POLL+0 and
     * carries one tick of mixed PCM in ESI (per-voice callbacks at higher
     * offsets are no-ops for us). Copy the PCM into the ring for the viewer. */
    if (eip >= MAGIC_POLL && eip < MAGIC_POLL + 0x1000) {
        /* DEBUG: log each distinct poll-callback offset the first time it fires,
         * to learn whether the SOS calls per-voice mix stubs (offset != 0). */
        {
            static uint8_t seen[0x1000];
            uint32_t off = eip - MAGIC_POLL;
            if (off < sizeof seen && !seen[off]) {
                seen[off] = 1;
                ALOGV("poll cb offset +%#x first hit: eax=%#x ecx=%#x edx=%#x "
                     "esi=%#x edi=%#x\n", off, R_EAX(c), R_ECX(c), R_EDX(c),
                     R_ESI(c), R_EDI(c));
            }
        }
        if (eip == MAGIC_POLL) {
            /* Advance the voice-mixer play-position dword (in-game SFX path; SOS
             * reads it at 0x49fc2 / mixer 0x4a5f9). Harmless for movies. */
            if (g_pos_lin)
                *(uint32_t *)(uintptr_t)g_pos_lin += g_au_chunk ? g_au_chunk : 1;
            audio_voice_dump(c);
            audio_profile_dump(); /* safe spot to emit the SIGPROF histogram */
            au_trace_drain();     /* flush committed audio-trace records + ISR samples here
                                   * (same non-nesting discipline as audio_profile_dump). No-op
                                   * unless ROTH_AU_TRACE is set. */

            /* In-game SFX: when a movie isn't actively shipping blocks to the
             * ring, software-mix the active digital voices (the .386 driver's
             * old job). (0x97b6c stays set even at the menu, so we key off
             * recent movie output instead.) */
            if (g_stream_fed > 0) {
                g_stream_fed--;
            } else {
                audio_mix_sfx();
                /* fire one queued SFX done-callback: far-call cb(dev,0,voice)
                 * returning to MAGIC_AFTER (which pops the 3 args + far-rets). */
                while (!g_in_drive && g_cbq_head != g_cbq_tail) {
                    uint32_t cboff = g_cbq[g_cbq_head].off;
                    uint32_t voice = g_cbq[g_cbq_head].voice;
                    uint32_t qgen = g_cbq[g_cbq_head].gen;
                    uint32_t qstart = g_cbq[g_cbq_head].start;
                    uint32_t qtag = g_cbq[g_cbq_head].qtag;
                    int qfree = g_cbq[g_cbq_head].qfree;
                    g_cbq_head = (g_cbq_head + 1) % SFX_CBQ;
                    /* STALE / RETAGGED DELIVERY GUARD — the residual overlapping-SFX kill.
                     * A done-cb belongs to the specific sound INSTANCE that ended. The game
                     * cb (0x27501) reads its record index LIVE off word[voice+0x34] and acts
                     * on rec = 0x83ed4 + tag*0x9a; queue entries carry no tag. So delivering a
                     * cb after the slot was re-owned murders the NEW occupant:
                     *   (a) STALE — the game freed+reused the slot (bit15 cleared outside our
                     *       MAGIC_AFTER path — a stop_voice 0x4ac55 / load-time reset — then a
                     *       fresh voice_start re-owned it). Caught by bit15 || gen || start.
                     *   (b) RETAGGED — the SAME SAMPLE re-fires into the SAME DAS buffer on a
                     *       cushion tick (audio_mix_sfx early-returned, so g_vgen was NOT bumped),
                     *       so bit15 SET, gen MATCHES, start MATCHES (identical buffer address) —
                     *       the guard passes — yet the LIVE word[voice+0x34] has already flipped
                     *       to the new sound's record. This is the clank kill: fire, hit a wall,
                     *       fire again into the same buffer inside one cushion window.
                     * Either way the cb must NOT run against the live voice. But the ENDED sound
                     * still owes its own cb's effect: a non-speech one-shot end runs the action-2
                     * arm whose COMPLETE effect (disasm 0x275a6: `mov [eax],0` then pop/retf; eax
                     * = rec) is a single st32(rec,0). Reproduce exactly that for the OLD record
                     * (qtag captured at queue time), touching neither the live voice nor its
                     * record. Fire-and-forget one-shots are freed ONLY by this cb (stop_sound_*
                     * need an emitter/id the caller never keeps), so a silent drop LEAKS the
                     * 16-slot handle table — pressure that reshuffles free-slot allocation
                     * (plausibly the "walk away and back" cure). Loops/speech
                     * (qfree==0) never free the record here. The .386 driver fired the cb
                     * synchronously in the end-of-sample ISR against the ended sound's own record
                     * — never stale, never retagged; this restores that invariant by identity. */
                    uint8_t *cvp = (uint8_t *)(uintptr_t)(g_farg_base + VOICE_OFF +
                                                          voice * VOICE_SZ);
                    uint16_t live_tag = *(uint16_t *)(cvp + 0x34);
                    int qspeech = ((qtag & 0xff00u) == 0xed00u);
                    int stale = !(*(uint16_t *)(cvp + 0x30) & 0x8000u) ||
                                g_vgen[voice] != qgen ||
                                *(uint32_t *)(cvp + 0x00) != qstart;
                    int retagged =
                        !stale && !qspeech && live_tag != (uint16_t)qtag;
                    if (stale || retagged) {
                        /* Reclaim the ENDED sound's handle host-side (the cb's action-2
                         * effect on the OLD record) instead of running it against the
                         * re-owned slot. Bound to the 16-record table (real SFX tags are
                         * 0..15) so a garbage tag can't scribble host memory. */
                        if (qfree && qtag < 16u)
                            *(uint32_t *)(uintptr_t)(CANON(0x83ed4) + qtag * 0x9au) = 0;
                        g_vended[voice] = 0; /* already 0 in the reuse paths; be safe */
                        if (g_sptrace && (qspeech || (live_tag & 0xff00u) == 0xed00u))
                            SPLOG("v%u done-cb DROPPED %s (qgen=%u gen=%u qstart=%08x live+00=%08x "
                                  "bit15=%d qtag=%04x live_tag=%04x)  <== a line-1 cb NOT reaching line 2\n",
                                  voice, retagged ? "RETAGGED" : "STALE", qgen, g_vgen[voice], qstart,
                                  *(uint32_t *)(cvp + 0x00),
                                  (*(uint16_t *)(cvp + 0x30) & 0x8000u) ? 1 : 0, (uint16_t)qtag, live_tag);
                        {
                            static int sl;
                            if (sl++ < 40) {
                                if (retagged)
                                    ALOGV("t%lu sfx done-cb RETAGGED voice %u: rec %u "
                                         "freed host-side, live rec %u untouched\n",
                                         g_mtick, voice, qtag, live_tag);
                                else
                                    ALOGV("t%lu sfx done-cb STALE voice %u dropped "
                                         "(gen q%u vs %u, rec %u%s)\n",
                                         g_mtick, voice, qgen, g_vgen[voice], qtag,
                                         qfree ? " freed" : "");
                            }
                        }
                        continue; /* resolved host-side -> try the next queued cb */
                    }
                    g_in_drive = 1;
                    g_after_stream = 0; /* SFX cb: do NOT copy the stale decode buf */
                    g_cb_voice = (int)voice; /* MAGIC_AFTER resolves free vs loop */
                    /* action: 0 = loop (cb restarts it), 2 = free the sound slot
                     * (one-shot). With action 0 a one-shot only decrements a
                     * per-type count and never frees its slot -> after ~13 the
                     * pool fills and the game stops. Pick by the same loop test
                     * the cb uses: byte[*soundentry+8] bit0x80 & (==1) & voice+0x10. */
                    uint32_t action = 2;
                    {
                        uint8_t *vp = (uint8_t *)(uintptr_t)(g_farg_base +
                                                             VOICE_OFF +
                                                             voice * VOICE_SZ);
                        uint16_t sid = *(uint16_t *)(vp + 0x34);
                        if ((sid & 0xff00u) == 0xed00u) {
                            /* Speech: a streamed two-buffer ping-pong (see
                             * docs/reference/ROTH_audio_notes.md). Fire the voice
                             * SOS callback with type 0 (buffer drained -> the game
                             * re-queues the alternate half + voice_stream_pump
                             * refills the next disk block) while the clip still has
                             * data, then type 2 (final -> g_voice_stream_state = 2
                             * -> the dialogue interpreter advances to the next
                             * line). g_voice_bytes_remaining (canon 0x82018). */
                            int32_t rem = *(int32_t *)(uintptr_t)CANON(0x82018);
                            /* cntB = back-buffer decoded count (VA_g_voice_bytes_remaining+0x10, canon
                             * 0x82028) — the game's OWN queue guard: voice_stream_sos_callback (0x1e487)
                             * queues the next (ping-pong) buffer on type 0 IFF [0x82028] != 0. A SHORT
                             * clip whose whole payload fit in buffer A leaves cntB==0 (buffer B was never
                             * decoded), so there is no buffer to swap to. */
                            int32_t cntB = *(int32_t *)(uintptr_t)CANON(0x82028);
                            /* rem tracks DECODE; when a real back buffer exists (cntB!=0) one
                             * already-decoded buffer is still queued behind it, so at rem==0 grace it
                             * (type 0 once more) then type 2. But with NO next buffer (cntB==0) the
                             * original SOS driver fires VOICE-DONE (type 2) at THIS buffer's end (audio
                             * notes: "type 2 at true clip-end unblocks chunk N+1") — it never grace-plays
                             * a non-existent buffer B. The old machine instead reset `ended`, took the
                             * `!eof` grace, and the type-0 resolve rewound cur->0 with +0x00 still == bufA
                             * -> buffer A REPLAYED = the short-dialogue double-play (BUG C). The cntB==0
                             * branch below ends immediately, no swap. eof/ended reset per clip. */
                            static int eof, ended;
                            int eof_pre = eof, ended_pre = ended; /* §SPEECH-SKIP: pre-update snapshot */
                            if (ended) { eof = 0; ended = 0; } /* first buf, new clip */
                            if (rem > 0) { action = 0; eof = 0; }
                            else if (cntB == 0) { action = 2; ended = 1; } /* no next buffer -> END now (no replay) */
                            else if (!eof) { action = 0; eof = 1; } /* real buffer B queued: play last buf */
                            else { action = 2; ended = 1; }        /* clip truly done */
                            /* These eof/ended statics persist across clips and are NOT reset by a skip;
                             * a stale eof=1/ended=1 entering line 2 could mis-decide its first cb. This
                             * line shows the decision + the statics (and cntB) as line 2's buffers drain. */
                            SPLOG("v%u done-cb DELIVER action=%u rem=%d cntB=%d eof(%d->%d) ended(%d->%d)\n",
                                  voice, action, rem, cntB, eof_pre, eof, ended_pre, ended);
                        } else if (sid < 256u) {
                            uint32_t se = CANON(0x83ed4) + (uint32_t)sid * 0x9au;
                            uint32_t sp = *(uint32_t *)(uintptr_t)se;
                            if (sp > 0x10000u) {
                                uint8_t lf = *(uint8_t *)(uintptr_t)(sp + 8);
                                if ((lf & 0x80) && (lf & 7) == 1 &&
                                    *(uint16_t *)(uintptr_t)(se + 0x10) != 0)  /* rec+0x10 (applied vol) — the
                                     * cb's OWN requeue test (0x27564 cmp word [rec+0x10],0). The old
                                     * vp+0x10 (voice end-of-data ptr) was always nonzero -> a FADED
                                     * (applied==0) mode-1 loop got action=0, cb declined, voice freed
                                     * but record leaked with a dangling handle -> zombie clobbered
                                     * every future occupant's volume (the shotgun-volume lottery). */
                                    action = 0; /* audible looping sound: requeue */
                            }
                        }
                    }
                    g_cb_action = action;
                    uint32_t esp = R_ESP(c);
                    uint32_t cs = (uint32_t)c->uc->uc_mcontext.gregs[REG_CS];
                    uint32_t *s = (uint32_t *)(uintptr_t)(esp - 20);
                    s[0] = MAGIC_AFTER;         /* callee retf eip */
                    s[1] = cs;                  /* callee retf cs  */
                    s[2] = (uint32_t)g_sfx_dev; /* arg0 = device   */
                    s[3] = action;              /* arg1 = 0 loop / 2 free */
                    s[4] = voice;               /* arg2 = voice    */
                    R_ESP(c) = esp - 20;
                    R_EIP(c) = cboff;
                    {
                        static int cl;
                        if (cl++ < 40)
                            ALOGV("t%lu sfx done-cb: voice %u dev %d -> canon %#x\n",
                                 g_mtick, voice, g_sfx_dev, cboff - OBJ_DELTA);
                    }
                    return 1;
                }
            }

            /* Drive the streaming buffer-complete handler (no real IRQ5 to do
             * it): far-call g_drive_fn(0,0,0) returning to MAGIC_AFTER, where we
             * ship the decoded block. Pace so the feed rate ~= the sample rate
             * (period = ticks-per-block = 70*frames/rate) for correct pitch and
             * movie speed; ROTH_STREAM_DIV overrides with a fixed integer. */
            int16_t db4 = *(int16_t *)(uintptr_t)CANON(0x91db4);
            uint32_t d50 = *(uint32_t *)(uintptr_t)CANON(0x91d50); /* ring start */
            uint32_t d5c = *(uint32_t *)(uintptr_t)CANON(0x91d5c); /* ring end   */
            uint32_t d70 = *(uint32_t *)(uintptr_t)CANON(0x91d70); /* play cursor*/
            uint32_t frames = (*(uint32_t *)(uintptr_t)CANON(0x91d28)) >> 1;
            /* Only fire while the movie is actually executing its streaming/wait
             * code (canon 0x4dd00..0x4e070 — the drain loop, block fetch, etc.).
             * During teardown/transition/menu/rendering the timer preempts the
             * game elsewhere, so we never drive a half-built or torn-down stream
             * -> no memory corruption. g_irq_eip = the EIP this timer IRQ
             * preempted (traps.c). This is the primary transition-safety gate. */
            int in_stream =
                g_irq_eip >= CANON(0x4dd00) && g_irq_eip < CANON(0x4e070);
            /* The engine reallocates the stream ring (d50/d5c change) on every
             * movie flush/transition; during that window the cursors are briefly
             * stale vs the new bounds and driving corrupts memory the renderer
             * reads -> blend-path crash. Pause firing for ~0.3s after any ring
             * change ("settle"), then resume. (Replaces the old db8<1024 gate,
             * which wrongly blocked long movies whose legit queue depth exceeds
             * it — they froze on the first frame.) */
            static uint32_t prev_d50, prev_d5c;
            static int settle;
            if (d50 != prev_d50 || d5c != prev_d5c) {
                prev_d50 = d50;
                prev_d5c = d5c;
                settle = 20; /* ~0.3s at 70 Hz */
            } else if (settle > 0) {
                settle--;
            }
            int healthy = d50 && d5c > d50 && d70 >= d50 && d70 < d5c &&
                          settle == 0;
            /* Pace the completions. Preferred: closed-loop on the SDL ring level
             * — fire whenever the viewer has drained below a few-block cushion,
             * so the movie clocks to the actual audio output (exact pitch, and a
             * cushion against the per-block sawtooth that otherwise crackles).
             * Fallback when nothing is draining (headless / audio muted): a timer
             * paced to the sample rate so the movie still advances. */
            uint32_t blk = frames * 4u; /* decoded bytes per block (16-bit*2ch) */
            uint32_t cushion = blk ? blk * 4u : 32768u;
            static uint32_t last_r;
            static int drain_seen;
            if (g_au && g_au->r != last_r) {
                last_r = g_au->r;
                drain_seen = 70; /* ~1s latch: a live consumer is draining */
            } else if (drain_seen > 0) {
                drain_seen--;
            }
            double period = g_drive_div
                                ? (double)g_drive_div
                                : (frames && g_au_rate
                                       ? 70.0 * frames / (double)g_au_rate
                                       : 5.0);
            if (period < 1.0)
                period = 1.0;
            static double acc;
            int want;
            if (!healthy) {
                acc = 0.0;
                want = 0;
            } else if (drain_seen && g_au) {
                want = (g_au->w - g_au->r) < cushion; /* consumer-clocked */
            } else {
                acc += 1.0;
                want = acc >= period; /* free-running fallback */
            }
            if (g_drive_fn && !g_in_drive && healthy && in_stream &&
                *(uint8_t *)(uintptr_t)CANON(0x97b6c) && db4 < 2 && want) {
                /* db4<2 is the engine's own catch-up threshold: only signal a
                 * completion when the movie's drain loop is ready for one. */
                if (!drain_seen)
                    acc -= period;
                g_in_drive = 1;
                g_after_stream = 1; /* MAGIC_AFTER should ship the decoded block */
                uint32_t esp = R_ESP(c);
                uint32_t cs = (uint32_t)c->uc->uc_mcontext.gregs[REG_CS];
                uint32_t *s = (uint32_t *)(uintptr_t)(esp - 20);
                s[0] = MAGIC_AFTER; /* callee retf eip */
                s[1] = cs;          /* callee retf cs  */
                s[2] = 0;           /* arg0            */
                s[3] = 0;           /* arg1 (0=normal) */
                s[4] = 0;           /* arg2            */
                R_ESP(c) = esp - 20;
                R_EIP(c) = g_drive_fn;
                return 1; /* run the handler; MAGIC_AFTER finishes the far-ret */
            }
        }
        far_ret(c);
        return 1;
    }

    /* (1c4) a MIDI event: the SOS far-called one of the 12 driver fns we handed
     * back via MIDI fn=1 (now installed in the per-channel handler table 0x92fa2).
     * The handler index (eip - MAGIC_MIDI)/4 picks the event type; the cdecl args
     * follow the retf frame [esp]=eip [esp+4]=cs [esp+8..]=args. Step B: just log
     * to decode which index = note-on/off/program/control/pitch + the arg layout.
     * Caller is cdecl (pops its own args), so a plain far-ret is uniform. */
    if (eip >= MAGIC_MIDI && eip < MAGIC_MIDI + MIDI_NFN * 4) {
        unsigned idx = (unsigned)((eip - MAGIC_MIDI) / 4);
        uint32_t esp = R_ESP(c);
        const uint32_t *st = (const uint32_t *)(uintptr_t)esp;
        uint32_t ret = (st[0] - OBJ_DELTA) & 0xffffffff;
        /* The per-channel "send message" fn (0x92f9c) gets the MIDI message via
         * EDX -> the work buffer (canon 0x951b4): b0 = status (command|channel),
         * b1/b2/b3 = data. 0x44f4e is the sequencer's real per-event sender (its
         * *args* are constant but the buffer *content* — the notes — varies), so
         * read the message rather than filtering it. EDX is a flat runtime addr. */
        /* The MIDI message pointer is the LAST pushed arg: offset = st[2],
         * selector = st[3]. Two senders share this layout: 0x44f4e (CC/pitch,
         * flat via ds) and 0x44f6b (notes/program from the DPMI song segment).
         * Read the message, push channel messages (0x80..0xEF) to the synth ring
         * (skip 0xF0 SysEx — MT-32 patch data, irrelevant to a SoundFont). */
        uint32_t moff = st[2];
        uint16_t msel = (uint16_t)st[3];
        /* extracted: read the message at (moff:msel) + push to the synth ring. Same read/guard
         * as before, now shared with the driver_dispatch_simple host binding (haudio_dispatch_simple).
         * The packed bytes come back for the (unchanged) per-message log. */
        uint32_t mb = haudio_midi_send(moff, msel);
        uint8_t b0 = (uint8_t)mb, b1 = (uint8_t)(mb >> 8),
                b2 = (uint8_t)(mb >> 16), b3 = (uint8_t)(mb >> 24);
        if (g_midi_log_en && g_midi_evlog < 4000) {
            g_midi_evlog++;
            ALOG("MIDI fn%u ret=0x%x  msg=%02x %02x %02x %02x  (off=%#x sel=%#x)\n",
                 idx, ret, b0, b1, b2, b3, moff, msel);
        }
        R_EAX(c) = 0;
        far_ret(c);
        return 1;
    }

    /* (2) a driver call: faulted fetching at MAGIC_OFF. EAX = function number,
     * far-args at FS:EDI (the thunk did `lfs edi,[ebp+0x10]`). */
    if (eip == MAGIC_OFF) {
        uint32_t fn = R_EAX(c);
        uint16_t fs = (uint16_t)c->uc->uc_mcontext.gregs[REG_FS];
        uint32_t esp = R_ESP(c);
        const uint32_t *st = (const uint32_t *)(uintptr_t)esp;
        ALOGV("call #%lu fn=%u  ebx=%#x ecx=%#x edx=%#x esi=%#x edi=%#x "
             "fs=%#x  ret=0x%x\n",
             ++g_call_count, fn, R_EBX(c), R_ECX(c), R_EDX(c), R_ESI(c),
             R_EDI(c), fs, (st[0] - OBJ_DELTA) & 0xffffffff);

        /* The MIDI driver (card 0xa004) shares the dispatch but has its own fn
         * numbering; branch by the card id in ESI so we don't confuse fn=0/1 with
         * the digital card-init/descriptor fns. */
        if (R_ESI(c) == 0xa004) {
            uint32_t mbase = dpmi_sel_base(fs);
            /* The thunk pre-loaded FS:EDI = the caller's work area (in-limit);
             * stage our result there rather than a fixed offset (segment 0x67 is
             * small, so a fixed 0x300 overran its limit -> GP fault). */
            uint32_t woff = R_EDI(c);
            switch (fn) {
            case 0: /* get 64-byte descriptor at FS:EDX (thunk rep-movs 0x40) */
                haudio_midi_load_descriptor(mbase, woff);
                R_EDX(c) = woff;
                R_EAX(c) = 0;
                ALOG("  MIDI fn0 -> descriptor staged at %#x:%#x\n", fs, woff);
                break;
            case 1: /* get 12-entry fn table at FS:EDI (stride 8) -> MAGIC_MIDI */
                haudio_midi_load_table(mbase, woff);
                R_EDI(c) = woff;
                R_EAX(c) = 0;
                ALOG("  MIDI fn1 -> 12 handlers @ MAGIC_MIDI, table at %#x:%#x\n",
                     fs, woff);
                break;
            default:
                ALOGV("  MIDI fn%u (unhandled) ebx=%#x ecx=%#x edx=%#x edi=%#x\n",
                     fn, R_EBX(c), R_ECX(c), R_EDX(c), R_EDI(c));
                R_EAX(c) = 0;
                break;
            }
            far_ret(c);
            return 1;
        }

        switch (fn) {
        case 2:   /* detect/probe card */
        case 8: { /* get driver info for card id (ESI). Both copy a 106-byte
                   * descriptor from the far-args seg at the offset we return in
                   * EDX; stage the SB16 descriptor there. */
            uint32_t base = dpmi_sel_base(fs);
            if (base) {
                haudio_detect_card(base);
                ALOG("  fn%u -> SB16 descriptor staged at %#x:%#x\n", fn, fs,
                     DESC_OFF);
            } else {
                ALOG("  fn%u -> WARNING: FS %#x base unknown\n", fn, fs);
            }
            R_EDX(c) = DESC_OFF;
            R_EAX(c) = 0;
            break;
        }
        case 0xa: { /* "open driver". Returns THREE values inside fargSel (FS):
                     * EDI = per-tick output callback (far-called by the audio
                     * service -> our MAGIC_POLL), ESI = base of the 32 voice
                     * structs (0x6c each; 0x4fd30 spreads them into the voice
                     * table 0x97440; the mixer at 0x4a0xx reads/writes them),
                     * ECX = the play-position dword (-> slot 0x97800). We carve
                     * voices + position out of fargSel and zero them so no stale
                     * data looks like an active voice (which would mix as static). */
            uint16_t fsel = (uint16_t)c->uc->uc_mcontext.gregs[REG_FS];
            uint32_t fbase = dpmi_sel_base(fsel);
            uint32_t o_cb, o_voices, o_pos;
            haudio_open_driver(fbase, R_ECX(c), &o_cb, &o_voices, &o_pos);
            R_EDI(c) = o_cb;     /* output callback (code) */
            R_ESI(c) = o_voices; /* 32 voice structs (0x6c each) */
            R_ECX(c) = o_pos;    /* play-position dword */
            R_EAX(c) = 0;
            ALOG("  fn0xa -> cb=MAGIC_POLL voices@FS:%#x pos@FS:%#x reqsz=%#x "
                 "FS=%#x base=%#x\n", VOICE_OFF, POS_OFF, g_dma_reqsz, fsel,
                 fbase);
            break;
        }
        default:
            R_EAX(c) = 0;
            break;
        }
        far_ret(c);
        return 1;
    }

    return 0;
}

/* Diagnostic: is the SOS mixer actually rendering PCM into our fn-0xa buffer?
 * Called each host timer tick (~70 Hz). Logs when the buffer's content changes,
 * so a headless run reveals whether digital audio is flowing before we wire SDL. */
void audio_tick(void)
{
    /* audio co-dev trace: cache the poll tick for record timestamps and, under
     * ROTH_AU_ISR_SAMPLE, read-only-sample the SOS timer-event table (the passive ISR witness).
     * FIRST — before the g_dma_lin early-out below, which is currently always taken (g_dma_lin is
     * never assigned), so the rest of this diagnostic never runs. No-op when ROTH_AU_TRACE unset. */
    au_trace_tick((uint32_t)g_mtick);

    if (!g_dma_lin)
        return;
    /* One-time: confirm our DMA buffer got plumbed into the mixer slot tables
     * (0x97420 = digital buffer A offset, 0x97424 its selector). */
    static int dumped;
    if (!dumped) {
        dumped = 1;
        uint32_t b0 = *(uint32_t *)(uintptr_t)CANON(0x97420);
        uint16_t s0 = *(uint16_t *)(uintptr_t)CANON(0x97424);
        uint32_t b1 = *(uint32_t *)(uintptr_t)CANON(0x97800);
        uint32_t svc = *(uint32_t *)(uintptr_t)CANON(0x74590);
        ALOG("plumbing: slot bufA=%#x:%#x bufB=%#x  mixer-svc[0x74590]=%#x  "
             "(our buf=%#x)\n", b0, s0, b1, svc, g_dma_lin);
    }
    static uint32_t last_sum;
    static unsigned long ticks, changes;
    uint32_t sum = 0, nz = 0;
    for (unsigned i = 0; i < sizeof g_dma_buf; i++) {
        sum = sum * 131 + g_dma_buf[i];
        if (g_dma_buf[i])
            nz++;
    }
    ticks++;
    if (sum != last_sum) {
        changes++;
        if (changes <= 8 || (changes % 70) == 0)
            ALOGV("DMA buffer CHANGED (tick %lu, change #%lu): nonzero=%u/%zu "
                 "sum=%#x\n", ticks, changes, nz, sizeof g_dma_buf, sum);
        last_sum = sum;
    }
}

/* ===================================================================================================
 * ===== The IMAGE-FREE digital-audio pump (audio residual) =========================
 *
 * DIVERGENCE (probe-proven): image-free the game-side audio INSTALL succeeds completely —
 * the c2 natives detect the SB16 (guard [0x9740c]=0xe018), open the voices (fn0xa staging: g_farg_base
 * set, [0x7f550]=-1) and register the digital poll cb {MAGIC_POLL, CS} into SOS timer-table slot 0
 * (rate 0x3c). But the table is NEVER WALKED image-free: the SOS master-timer ISR (0x49eaf,
 * host_audio_driver class — never lifted) only runs in the trap lane via inject_irq -> the 0x54b05
 * original stub, and roth_boot bypasses inject_irq entirely (traps.c alarm_handler). So the
 * MAGIC_POLL body — the host mixer pump inside audio_trap — never executes: no SFX mix, no done-cb
 * delivery, no play-position advance, no movie-stream drive => TOTAL image-free silence despite a
 * fully-staged driver. These two functions ARE that poll body as callable host C, driven per SIGALRM
 * tick from shm_tick's imgfree surrogate (traps_if.o, #ifdef ROTH_STANDALONE — traps.o byte-unchanged),
 * exactly like the lifted GDV/heartbeat ISR bodies the surrogate already drives. The trap lane never
 * calls them (audio_trap's MAGIC_POLL case is untouched); the two paths are process-exclusive, so the
 * shared mixer/queue statics (g_cbq, g_vcur/g_vact/.., g_stream_fed) have a single driver per process.
 *
 * The OTHER live timer-table events are deliberately NOT fired here (enumerated dispatch, not a blind
 * walk):
 *   - slot 1  {0x1231b(->vsync_timer_tick 0x122e3), rate 0x46}: the frame-clock heartbeat — already
 *     stood in by shm_tick (vsync_timer_tick / the bare 0x90bcc bump); firing it here would
 *     double-drive the validated game clock.
 *   - slot 15 {0x49f78, rate 0xff00}: the SOS "system heartbeat" — disasm 0x49f78->0x54d5d is pure
 *     interrupt-CHAIN plumbing (stack switch + chain to the saved old int-8 vector [0x74584]); no
 *     game-memory effect worth reproducing under the host (the old vector is IRQ_RET_MAGIC).
 *   - GDV decode ISRs (0x4e2ed/0x4e24b/0x4e60b, registered by gdv_setup_voices): driven as lifted C by
 *     shm_tick's gdv_active branches at their own validated pacing.
 *   - the MIDI sequencer step {0x51ad5, registered by step_audio_sequence at music start}: NOW NATIVE
 *     (audio_if_seq_step below) — fired from the pump by the honest rate-respecting slot
 *     walk (the master ISR's Q16 accumulator fire logic, transcribed from 0x49f0d..0x49f60) for the
 *     ENUMERATED 0x51ad5 slots only. The trap lane still steps the original bytes via the master ISR;
 *     image-free the native emits the full event stream (notes + CCs) into the same c2 MIDI-router
 *     natives -> haudio_midi_send -> the ROTH_MIDI SoundFont ring. */

/* the lifted SFX/speech completion dispatcher (0x27501, lift_audio.c) — every image-free submit site
 * installs GADDR(0x27501) as the voice done-cb (lift_audio.c:677/716/1157), so the delivery below can
 * dispatch the enumerated token directly. Prototype mirrors engine.h:1367 (args = device, type, voice:
 * the trap lane far-calls cb(dev, action, voice) — audio_trap's s[2..4] frame). */
void sos_user_callback_trampoline(uint32_t dev, uint32_t type, uint32_t voice);
/* the c2 voice-queue dispatcher (0x4ad03 native image-free; os_api.h) — 0x55620's tail call */
uint32_t os_audio_voice_load_to_slot(uint32_t handle, uint32_t edx, uint32_t queued, uint16_t ds);

static inline uint16_t if_cur_ds(void) { uint16_t v; __asm__("mov %%ds,%0" : "=r"(v)); return v; }

static unsigned long g_ifp_sfx_pcm;   /* mechanical sample-flow counters (proof observables) */
static unsigned long g_ifp_gdv_bytes;

/* One queued done-cb delivery + resolution — the direct-C equivalent of audio_trap's MAGIC_POLL
 * delivery (far-call + MAGIC_AFTER resolve), transcribed 1:1 from that code (same stale/retagged
 * guards, same action decision incl. the speech rem/eof/ended state machine, same post-cb resolve).
 * At most ONE delivery per tick (the trap path far-calls one cb and returns); dropped stale entries
 * are consumed in the same pass exactly like the trap loop's `continue`. */
static void audio_if_deliver_cbs(void)
{
    while (g_cbq_head != g_cbq_tail) {
        uint32_t cboff = g_cbq[g_cbq_head].off;
        uint32_t voice = g_cbq[g_cbq_head].voice;
        uint32_t qgen = g_cbq[g_cbq_head].gen;
        uint32_t qstart = g_cbq[g_cbq_head].start;
        uint32_t qtag = g_cbq[g_cbq_head].qtag;
        int qfree = g_cbq[g_cbq_head].qfree;
        g_cbq_head = (g_cbq_head + 1) % SFX_CBQ;
        /* stale / retagged delivery guard — verbatim from audio_trap (see the long comment there) */
        uint8_t *cvp = (uint8_t *)(uintptr_t)(g_farg_base + VOICE_OFF + voice * VOICE_SZ);
        uint16_t live_tag = *(uint16_t *)(cvp + 0x34);
        int qspeech = ((qtag & 0xff00u) == 0xed00u);
        int stale = !(*(uint16_t *)(cvp + 0x30) & 0x8000u) ||
                    g_vgen[voice] != qgen ||
                    *(uint32_t *)(cvp + 0x00) != qstart;
        int retagged = !stale && !qspeech && live_tag != (uint16_t)qtag;
        if (stale || retagged) {
            if (g_sfxtrace)
                sfx_ev(SFXE_CBDROP, (uint8_t)voice, (uint16_t)qtag, stale ? 1u : 2u, live_tag, qgen);
            if (qfree && qtag < 16u)
                *(uint32_t *)(uintptr_t)(CANON(0x83ed4) + qtag * 0x9au) = 0;
            g_vended[voice] = 0;
            if (g_sptrace && (qspeech || (live_tag & 0xff00u) == 0xed00u))
                SPLOG("v%u done-cb DROPPED %s (qgen=%u gen=%u qstart=%08x live+00=%08x "
                      "bit15=%d qtag=%04x live_tag=%04x)  [imgfree pump]\n",
                      voice, retagged ? "RETAGGED" : "STALE", qgen, g_vgen[voice], qstart,
                      *(uint32_t *)(cvp + 0x00),
                      (*(uint16_t *)(cvp + 0x30) & 0x8000u) ? 1 : 0, (uint16_t)qtag, live_tag);
            continue; /* resolved host-side -> try the next queued cb */
        }
        /* Enumerated cb dispatch: every image-free submit site installs 0x27501. A different token
         * would be a NEW un-enumerated callback — fail loud (log) + the no-cb fallback so the voice
         * doesn't wedge, mirroring audio_mix_sfx's cb-less arm. */
        if (cboff != CANON(0x27501)) {
            static int warned;
            if (!warned) {
                warned = 1;
                ALOG("imgfree pump: UN-ENUMERATED done-cb token %#x (voice %u) — freeing the voice "
                     "without a cb; report this token\n", cboff, voice);
            }
            *(uint16_t *)(cvp + 0x30) &= ~0x8000u;
            g_vact[voice] = 0;
            g_vended[voice] = 0;
            continue;
        }
        /* action decision — verbatim from audio_trap's MAGIC_POLL case (0 = loop/continue, 2 = free) */
        uint32_t action = 2;
        {
            uint16_t sid = *(uint16_t *)(cvp + 0x34);
            if ((sid & 0xff00u) == 0xed00u) {
                int32_t rem = *(int32_t *)(uintptr_t)CANON(0x82018);
                /* cntB (VA_g_voice_bytes_remaining+0x10, canon 0x82028): the game's queue guard — no
                 * next buffer when cntB==0, so a short single-buffer clip must END (type 2) at its first
                 * buffer-end, not grace-replay buffer A. Verbatim with audio_trap's MAGIC_POLL arm; see
                 * the long comment there (BUG C fix). */
                int32_t cntB = *(int32_t *)(uintptr_t)CANON(0x82028);
                static int eof, ended;
                int eof_pre = eof, ended_pre = ended;
                if (ended) { eof = 0; ended = 0; }
                if (rem > 0) { action = 0; eof = 0; }
                else if (cntB == 0) { action = 2; ended = 1; } /* no next buffer -> END now (no replay) */
                else if (!eof) { action = 0; eof = 1; }
                else { action = 2; ended = 1; }
                SPLOG("v%u done-cb DELIVER action=%u rem=%d cntB=%d eof(%d->%d) ended(%d->%d) [imgfree pump]\n",
                      voice, action, rem, cntB, eof_pre, eof, ended_pre, ended);
            } else if (sid < 256u) {
                uint32_t se = CANON(0x83ed4) + (uint32_t)sid * 0x9au;
                uint32_t sp = *(uint32_t *)(uintptr_t)se;
                if (sp > 0x10000u) {
                    uint8_t lf = *(uint8_t *)(uintptr_t)(sp + 8);
                    if ((lf & 0x80) && (lf & 7) == 1 &&
                        *(uint16_t *)(uintptr_t)(se + 0x10) != 0)   /* rec+0x10, the cb's own test (0x27564)
                                                                     * — see the trap-arm twin above */
                        action = 0;
                }
            }
        }
        /* deliver: the trap lane far-calls cb(dev, action, voice) -> 0x27501; image-free the SAME
         * body is verified lifted C — call it directly (the shm_tick lifted-ISR-body precedent). */
        {
            static int cl;
            if (cl++ < 40)
                ALOGV("t%lu sfx done-cb: voice %u dev %d action %u -> lifted 0x27501 [imgfree pump]\n",
                      g_mtick, voice, g_sfx_dev, action);
        }
        if (g_sfxtrace)
            sfx_ev(SFXE_CBDELIVER, (uint8_t)voice, (uint16_t)action, (uint32_t)g_sfx_dev,
                   qtag, cboff);
        sos_user_callback_trampoline((uint32_t)g_sfx_dev, action, voice);
        /* resolve — verbatim from audio_trap's MAGIC_AFTER SFX arm */
        {
            uint16_t sid = *(uint16_t *)(cvp + 0x34);
            if ((sid & 0xff00u) == 0xed00u) {
                if (action == 0) {
                    g_vstart[voice] = *(uint32_t *)(cvp + 0x00);
                    g_vcur[voice] = 0;
                } else {
                    *(uint16_t *)(cvp + 0x30) &= ~0x8000u;
                    g_vact[voice] = 0;
                }
                SPLOG("v%u done-cb RESOLVED action=%u -> %s  g_vstart=%08x g_vcur=%u +00=%08x [imgfree pump]\n",
                      (int)voice, action,
                      action == 0 ? "continue(swap buffer, cur->0)" : "end(deactivate)",
                      g_vstart[voice], g_vcur[voice], *(uint32_t *)(cvp + 0x00));
            } else {
                uint32_t start = *(uint32_t *)(cvp + 0x00);
                uint32_t cur08 = *(uint32_t *)(cvp + 0x08);
                if (cur08 <= start + 4)
                    g_vcur[voice] = 0;
                else {
                    *(uint16_t *)(cvp + 0x30) &= ~0x8000u;
                    g_vact[voice] = 0;
                }
            }
            g_vended[voice] = 0;
        }
        break; /* one delivery per tick (the trap path returns after its far-call) */
    }
}

/* ===================================================================================================
 * ===== Image-free MIDI music: the HMI sequencer step (sos_sequence_timer_tick 0x51ad5) ============
 *
 * 0x51ad5 (5,346 B, host_audio_driver class — host-REPLACED by design, never lifted, exactly like the
 * MIDI-router / timer-table natives in os_audio.c) is the per-tick HMI MIDI sequencer: the FAR
 * SOS timer event step_audio_sequence (0x46d18, lifted) registers at the music driver's tick rate
 * (descriptor+0x38). It takes NO argument — the master ISR publishes the song index into the byte
 * [0x97ae4] before the far-call (0x49f54) and the step reads it back (0x51aea). Everything
 * it touches is modeled state:
 *   - plain game globals (fade 0x97038/0x97058/0x97078/0x97098/0x970b8, per-track tick 0x92b2c /
 *     next-delta 0x9272c / event far-ptrs 0x9212c/0x92130 / note-map far-ptrs 0x94bb4/0x94bb8 /
 *     loop-entry far-ptrs 0x95204/0x95208, live/total track counts 0x92f5c/0x92f7c, channel map
 *     0x92f2c/0x92f30, descriptor slot 0x93164/0x93168, timer handle 0x72908 + unmap byte 0x741fc,
 *     event-length tables 0x728e8/0x728f8, the in-callback flag 0x97034);
 *   - far {off,sel} derefs into the parsed song chunk (`lgs` idioms) -> dpmi_sel_base(sel)+off, the
 *     midi_router_core / haudio_midi_send translation;
 *   - callees ALL already native/lifted: emit_audio_sequence_event 0x4627d + parse_music_sequence_
 *     tracks 0x46eb3 + decode_midi_varlen 0x47150 (lifted, lift_audio.c), sos_dispatch_midi_event
 *     0x44e0d + midi_all_notes_off_channels 0x4594d + sos_timer_remove_event 0x49ca4 (c2 natives;
 *     image-free the dispatchers bind straight to the _native bodies via imgfree/os_audio_standalone.c —
 *     the KEY COMPOSITION RULE), and 0x53006 (the post-branch state replay, transcribed below).
 * Transcribed store-by-store from disasm 0x51ad5..0x53005 + 0x53006..0x531e1 (obj1.bin; the CC switch
 * jump table is the 19 dwords at 0x52d9c, index = cc-0x67). Canon VAs annotate every block.
 *
 * The HMI USER-CALLBACK slots (branch cb 0x95804 / loop cb 0x95834 / trigger cbs 0x95864 / song-end cb
 * desc+0x380) are far-called by the original when nonzero. In ROTH they are provably NEVER installed:
 * init_audio_sequence (0x464f9) zeroes all three tables + stores desc+0x380 from pair+8, and the ONLY
 * music client, process_audio_sequence_chunk (lift_audio.c:1755), zeroes pair+8/+0xc (0x7f3bc/0x7f3c0);
 * a corpus-wide grep shows no other writer. So each cb site keeps the exact nonzero gate and FAILS LOUD
 * (log + skip) on an un-enumerated callback instead of guessing at an un-callable far target.
 *
 * Called ONLY from audio_standalone_tick's slot walk (below); the trap lane never reaches any of this
 * (music there = original bytes via the master ISR 0x49eaf). */

/* lifted client helpers (lift_audio.c; prototypes mirror engine.h:1344/1345/1348) */
uint32_t decode_midi_varlen(uint32_t src_off, uint32_t src_sel, uint32_t out_off, uint32_t out_sel);
uint32_t emit_audio_sequence_event(uint32_t seq, uint32_t event);
uint32_t parse_music_sequence_tracks(uint32_t seq, uint32_t pair, uint32_t pair_sel);
/* c2 dispatchers (os_api.h:151/154/234; imgfree lane binds them to the natives, trap lane to the
 * bridge — never called there) */
uint32_t os_audio_midi_dispatch(uint32_t seq, uint32_t dev, uint32_t msg, uint16_t sel);
uint32_t os_audio_midi_all_notes_off(uint32_t seq);
uint32_t os_audio_timer_remove_event(uint32_t event);
/* the audio-timer table fence (traps.c): nonzero while a timer native edits the 16-slot table */
extern volatile int g_au_timer_locked;

/* absolute game globals (DS `mov [abs]` in the disasm) — os_audio.c's GP* idiom */
#define SGP32(x) (*(volatile uint32_t *)(uintptr_t)CANON(x))
#define SGP16(x) (*(volatile uint16_t *)(uintptr_t)CANON(x))
#define SGP8(x)  (*(volatile uint8_t  *)(uintptr_t)CANON(x))
/* far {off,sel} derefs (`lgs`/`mov gs,..` idioms): linear = dpmi_sel_base(sel)+off (GDT/flat -> +0) */
static inline uint8_t  sq_fr8 (uint16_t sel, uint32_t off)
{ return *(const volatile uint8_t  *)(uintptr_t)(dpmi_sel_base(sel) + off); }
static inline uint16_t sq_fr16(uint16_t sel, uint32_t off)
{ return *(const volatile uint16_t *)(uintptr_t)(dpmi_sel_base(sel) + off); }
static inline uint32_t sq_fr32(uint16_t sel, uint32_t off)
{ return *(const volatile uint32_t *)(uintptr_t)(dpmi_sel_base(sel) + off); }
static inline void sq_fw8(uint16_t sel, uint32_t off, uint8_t v)
{ *(volatile uint8_t *)(uintptr_t)(dpmi_sel_base(sel) + off) = v; }
static inline uint16_t if_cur_ss(void) { uint16_t v; __asm__("mov %%ss,%0" : "=r"(v)); return v; }

static unsigned long g_ifp_seq_steps; /* fired sequencer steps (proof observable, totals line) */

/* fail-loud: an installed HMI user callback would be un-enumerated (see the header proof) */
static void if_seq_cb_unexpected(uint32_t off, uint32_t sel, const char *site)
{
    static int warned;
    if (!warned) {
        warned = 1;
        ALOG("imgfree seq: UN-ENUMERATED HMI %s user callback %#x:%#x — SKIPPED (no such registrant "
             "exists in ROTH; report this)\n", site, off, sel);
    }
}

/* find the loop/branch entry with id byte `id` in track t's entry table ({0x95204,0x95208}+e*0x18,
 * id at +4). The original scan is UNBOUNDED (0x52220..0x52259 idiom — a missing id hangs the ISR);
 * transcribed faithfully. */
static uint32_t if_seq_find_entry(uint32_t song, uint32_t t, uint32_t id)
{
    uint32_t trk  = song * 0xc0 + t * 6;
    uint32_t base = SGP32(0x95204 + trk);
    uint16_t sel  = SGP16(0x95208 + trk);
    uint32_t e = 0;
    while ((uint32_t)sq_fr8(sel, base + e * 0x18 + 4) != id)
        e++;
    return e;
}

/* redirect track t's event ptr to entry e's target: ev = note-map(0x94bb4/0x94bb8) + entry[+0] + 0xc
 * (0x5249f..0x52510 / 0x52ad5..0x52b51 / 0x52cf9..0x52d75 — inline-triplicated in the original) */
static void if_seq_redirect(uint32_t song, uint32_t t, uint32_t e)
{
    uint32_t trk = song * 0xc0 + t * 6;
    uint32_t off = sq_fr32(SGP16(0x95208 + trk), SGP32(0x95204 + trk) + e * 0x18) + 0xc;
    SGP16(0x92130 + trk) = SGP16(0x94bb8 + trk);
    SGP32(0x9212c + trk) = SGP32(0x94bb4 + trk) + off;
}

/* 0x53006 (corpus, misclassed `crt` — it is the HMI post-branch channel-state replay;
 * disasm 0x53006..0x531e1): after a loop/branch jump, re-send the channel's program change (entry+5,
 * gated on desc+0x36c) and the entry's saved-CC list ({cc,val} pairs at the RESOLVED flat ptr
 * entry+8, count at entry+7 — the original derefs it with GS:=DS, 0x53135/0x5316b) gated per-CC on
 * desc+0x300+cc (the flags CC 0x67/0x68 toggle). DIVERGENCE (documented): the original builds the
 * 2/3-byte message on the ISR stack (game memory); a host-stack pointer fails haudio_midi_send's
 * guest-range guard, so the native stages it in the game's own 3-byte MIDI message buffer 0x951c0 —
 * the SAME staging buffer the only other message builder (emit_audio_sequence_event,
 * lift_audio.c:1472) uses before this very dispatcher. The bytes reaching the router/ring are
 * identical; the only extra effect is scratch writes to 0x951c0..0x951c2. */
static void if_seq_replay_state(uint32_t song, uint32_t t, uint32_t e)
{
    uint32_t trk      = song * 0xc0 + t * 6;
    uint32_t d_off    = SGP32(0x93164 + song * 6);
    uint16_t d_sel    = SGP16(0x93168 + song * 6);
    uint32_t ent_base = SGP32(0x95204 + trk);
    uint16_t ent_sel  = SGP16(0x95208 + trk);
    if (sq_fr8(d_sel, d_off + 0x36c) != 0) {                       /* 0x53026 */
        SGP8(0x951c0) = (uint8_t)((sq_fr8(SGP16(0x92130 + trk), SGP32(0x9212c + trk)) & 0xfu)
                                  | 0xc0u);                        /* 0x53048 (0xC0|chan) */
        SGP8(0x951c1) = sq_fr8(ent_sel, ent_base + e * 0x18 + 5);  /* 0x53075 patch */
        uint32_t dev = sq_fr32(SGP16(0x92f30 + song * 6),
                               SGP32(0x92f2c + song * 6) + t * 4); /* 0x5309a */
        os_audio_midi_dispatch(song, dev, CANON(0x951c0), if_cur_ds());  /* 0x530b1 (len-2 stack arg
                                                                          * dropped by the native) */
    }
    SGP8(0x951c0) = (uint8_t)((sq_fr8(SGP16(0x92130 + trk), SGP32(0x9212c + trk)) & 0xfu)
                              | 0xb0u);                            /* 0x530ca (0xB0|chan) */
    for (uint32_t i = 0; i < (uint32_t)sq_fr8(ent_sel, ent_base + e * 0x18 + 7); i += 2) { /* 0x53104 */
        uint32_t list = sq_fr32(ent_sel, ent_base + e * 0x18 + 8); /* 0x53137 (resolved flat ptr) */
        SGP8(0x951c1) = *(const volatile uint8_t *)(uintptr_t)(list + i);      /* 0x53142 cc  (GS:=DS) */
        SGP8(0x951c2) = *(const volatile uint8_t *)(uintptr_t)(list + i + 1);  /* 0x53178 val (GS:=DS) */
        if (sq_fr8(d_sel, d_off + 0x300 + SGP8(0x951c1)) != 0) {   /* 0x53198 per-CC replay flag */
            uint32_t dev = sq_fr32(SGP16(0x92f30 + song * 6),
                                   SGP32(0x92f2c + song * 6) + t * 4);         /* 0x531c0 */
            os_audio_midi_dispatch(song, dev, CANON(0x951c0), if_cur_ds());    /* 0x531e1-ish (len 3) */
        }
    }
}

/* the song-end epilogue, shared by the fade-out finish (0x51bea..0x51cfe) and the last-track
 * end-of-track (0x51faf..0x520c3) — byte-for-byte the same block in the original */
static void if_seq_song_end(uint32_t song)
{
    SGP32(0x93144 + song * 4) = 0;                                 /* 0x51bf4 armed=0 */
    os_audio_midi_all_notes_off(song);                             /* 0x51c05 -> 0x4594d native */
    if (SGP32(0x72908 + song * 4) != 0xffffffffu)                  /* 0x51c14 */
        os_audio_timer_remove_event(SGP32(0x72908 + song * 4));    /* 0x51c2d -> 0x49ca4 native */
    /* 0x51c42: [0x741fc + handle] = 0xff UNGATED — for handle -1 this is the original's wrapping
     * [-1+0x741fc] store (canon 0x741fb), the same idiom teardown_music_sequence reproduces */
    SGP8(0x741fc + SGP32(0x72908 + song * 4)) = 0xff;
    SGP32(0x72908 + song * 4) = 0xffffffffu;                       /* 0x51c53 */
    /* stage the re-parse pair {desc, song-end cb far} on the host stack (the original's ebp-0x4c
     * block; parse reads it via SS exactly like teardown_music_sequence's blk) */
    uint32_t d_off  = SGP32(0x93164 + song * 6);                   /* 0x51c99 */
    uint16_t d_sel  = SGP16(0x93168 + song * 6);                   /* 0x51c8e */
    uint32_t cb_off = sq_fr32(d_sel, d_off + 0x380);               /* 0x51c7a */
    uint16_t cb_sel = sq_fr16(d_sel, d_off + 0x384);               /* 0x51c6e */
    uint8_t blk[16];
    *(uint32_t *)(blk + 0)  = d_off;
    *(uint16_t *)(blk + 4)  = d_sel;
    *(uint32_t *)(blk + 8)  = cb_off;                              /* pair+8  (parse re-stores it) */
    *(uint16_t *)(blk + 12) = cb_sel;                              /* pair+0xc */
    SGP16(0x93168 + song * 6) = 0;                                 /* 0x51cb9 clear the slot */
    SGP32(0x93164 + song * 6) = 0;                                 /* 0x51cc2 */
    parse_music_sequence_tracks(song, (uint32_t)(uintptr_t)blk, if_cur_ss()); /* 0x51ce2 —
                                                                    * re-bind = the song LOOP restart */
    if (cb_off != 0 || cb_sel != 0)                                /* 0x51ce7: would far-call cb(song) */
        if_seq_cb_unexpected(cb_off, cb_sel, "song-end (desc+0x380)");
}

/* sos_sequence_timer_tick 0x51ad5 — the per-tick step body. Song = the published byte [0x97ae4]. */
static void audio_if_seq_step(void)
{
    uint32_t song = SGP8(0x97ae4);                                 /* 0x51aea */
    if (SGP32(0x93144 + song * 4) == 0 ||                          /* 0x51af4 not armed */
        SGP32(0x93104 + song * 4) != 0)                            /* 0x51b07 paused/muted */
        return;                                                    /* 0x51b10 -> 0x52fff */

    /* ---- volume-fade ramp (0x51b1f..0x51d72; a fade step every 4th tick via the 0x970b8 divider) */
    if (SGP32(0x97098 + song * 4) != 0) {                          /* fade steps remaining */
        uint8_t div = SGP8(0x970b8 + song);                        /* 0x51b33 (BYTE table, stride 1) */
        SGP8(0x970b8 + song) = (uint8_t)(div - 1);                 /* 0x51b39 dec */
        if (div == 0) {                                            /* 0x51b3f */
            SGP8(0x970b8 + song) = 3;                              /* 0x51b4e reload */
            SGP32(0x97098 + song * 4) -= 1;                        /* 0x51b5f steps-- */
            uint32_t mode = SGP32(0x97038 + song * 4);             /* 0x51b6f; switch 0x51d4e */
            if (mode == 2 || mode == 4) {                          /* ramp DOWN (0x51b7d) */
                SGP32(0x97078 + song * 4) -= SGP32(0x97058 + song * 4);      /* 0x51b97 */
                emit_audio_sequence_event(song,
                    (SGP32(0x97078 + song * 4) >> 16) & 0xffu);    /* 0x51bba -> 0x4627d */
                if ((SGP8(0x97038 + song * 4) & 4) &&              /* 0x51bc9 mode-4 = fade-out-stop */
                    SGP32(0x97098 + song * 4) == 0) {              /* 0x51bdc last step */
                    if_seq_song_end(song);                         /* 0x51bea..0x51cfe */
                    return;                                        /* 0x51cfe -> 0x52fff */
                }
            } else if (mode == 1) {                                /* ramp UP (0x51d6a -> 0x51d08) */
                SGP32(0x97078 + song * 4) += SGP32(0x97058 + song * 4);      /* 0x51d22 */
                emit_audio_sequence_event(song,
                    (SGP32(0x97078 + song * 4) >> 16) & 0xffu);    /* 0x51d45 */
            }                                                      /* mode 0/3/>4: nothing (0x51d4c) */
        }
    }

    /* ---- per-MIDI-track event pump (0x51d72..0x52ffa) ---- */
    for (uint32_t t = 0; t < SGP32(0x92f7c + song * 4); t++) {     /* 0x51d89 (count re-read per pass) */
        uint32_t tick = 0x92b2c + song * 0x80 + t * 4;             /* per-track tick counter */
        uint32_t dly  = 0x9272c + song * 0x80 + t * 4;             /* per-track next event delta */
        uint32_t trk  = song * 0xc0 + t * 6;                       /* far-ptr table row */
        SGP32(tick) += 1;                                          /* 0x51daa (ALWAYS, dead or alive) */
        if (SGP32(0x9212c + trk) == 0 && SGP16(0x92130 + trk) == 0)
            continue;                                              /* 0x51dc6 dead track */
        if (SGP32(dly) > SGP32(tick))
            continue;                                              /* 0x51e0b not due yet */
        do {
            SGP8(0x97034) = 0;                                     /* 0x51e17 in-callback flag */
            int need_delta = 1;                                    /* 0x51e1e [ebp-0xc] */
            int do_dispatch = 0;                                   /* -> 0x52e06 vs 0x52e04 */
            uint8_t status = sq_fr8(SGP16(0x92130 + trk), SGP32(0x9212c + trk));   /* 0x51e42 */
            uint32_t len = (status < 0xf0u)                        /* 0x51e4b */
                ? SGP8(0x728e8 + (status >> 4))                    /* 0x51e71 channel-msg len table */
                : SGP8(0x728f8 + (status & 0xfu));                 /* 0x51ea2 system-msg len table */
            if (status == 0xffu) {                                 /* 0x51ecd meta */
                uint8_t mt = sq_fr8(SGP16(0x92130 + trk), SGP32(0x9212c + trk) + 1);  /* 0x51ef5 */
                if (mt == 0x2f) {                                  /* 0x520e8 END OF TRACK (0x51f01) */
                    SGP16(0x92130 + trk) = 0;                      /* 0x51f17 kill this track */
                    SGP32(0x9212c + trk) = 0;                      /* 0x51f20 */
                    if (SGP32(0x92f5c + song * 4) - 1 == 1 &&      /* 0x51f3a live-1==1 */
                        (SGP32(0x9212c + song * 0xc0) != 0 ||
                         SGP16(0x92130 + song * 0xc0) != 0)) {     /* 0x51f4d conductor track alive */
                        SGP32(0x92f5c + song * 4) -= 1;            /* 0x51f6c */
                        SGP16(0x92130 + song * 0xc0) = 0;          /* 0x51f7f kill track 0 too */
                        SGP32(0x9212c + song * 0xc0) = 0;          /* 0x51f88 */
                    }
                    SGP32(0x92f5c + song * 4) -= 1;                /* 0x51f9c live-- */
                    if (SGP32(0x92f5c + song * 4) != 0) {          /* 0x51fa2 */
                        len = 3;                                   /* 0x520c8 */
                    } else {                                       /* LAST track -> song end 0x51faf */
                        if_seq_song_end(song);
                        return;                                    /* 0x520c3 -> 0x52fff */
                    }
                } else if (mt == 0x51) {                           /* 0x520f2 tempo meta */
                    len = 5;                                       /* 0x520d1 */
                }                                                  /* other metas: table len
                                                                    * (0x520fa/0x520da -> advance) */
            } else if ((status & 0xf0u) == 0xb0u) {                /* 0x52121 controller */
                uint8_t cc = sq_fr8(SGP16(0x92130 + trk), SGP32(0x9212c + trk) + 1);  /* 0x52148 */
                switch (cc) {                                      /* 0x52de8 jump table 0x52d9c */
                case 0x67:                                         /* 0x52154 CC-replay flag OFF */
                    sq_fw8(SGP16(0x93168 + song * 6),              /* 0x52180/0x5218d */
                           SGP32(0x93164 + song * 6) + 0x300 +
                           sq_fr8(SGP16(0x92130 + trk), SGP32(0x9212c + trk) + 2), 0); /* 0x52192 */
                    break;
                case 0x68:                                         /* 0x5219f CC-replay flag ON */
                    sq_fw8(SGP16(0x93168 + song * 6),              /* 0x521d2/0x521d8 */
                           SGP32(0x93164 + song * 6) + 0x300 +
                           sq_fr8(SGP16(0x92130 + trk), SGP32(0x9212c + trk) + 2), 1); /* 0x521dd */
                    break;
                case 0x6c: case 0x6e: case 0x74: case 0x78:        /* 0x521ea/0x521ef/0x528ba/0x52c15 */
                    break;                                         /* swallowed (no dispatch) */
                case 0x6d: case 0x73: {                            /* 0x521f4 / 0x528bf loop-count set */
                    uint32_t id = sq_fr8(SGP16(0x92130 + trk), SGP32(0x9212c + trk) + 2);
                    uint32_t e  = if_seq_find_entry(song, t, id);
                    sq_fw8(SGP16(0x95208 + trk), SGP32(0x95204 + trk) + e * 0x18 + 6,
                           sq_fr8(SGP16(0x92130 + trk), SGP32(0x9212c + trk) + 6)); /* 0x522a6/0x52971:
                                                                    * count := STREAM byte ev+6 */
                    break; }
                case 0x6f: case 0x70: {                            /* 0x522b6 LOOP POINT (all tracks) */
                    uint32_t id = sq_fr8(SGP16(0x92130 + trk), SGP32(0x9212c + trk) + 2); /* 0x522d3 */
                    uint32_t e  = if_seq_find_entry(song, t, id);  /* 0x522e2 */
                    uint32_t cnt = sq_fr8(SGP16(0x95208 + trk),
                                          SGP32(0x95204 + trk) + e * 0x18 + 6);  /* 0x52349 */
                    if (cnt != 0xffu && cnt != 0) {                /* 0x52351/0x5235a (0xff=forever) */
                        sq_fw8(SGP16(0x95208 + trk), SGP32(0x95204 + trk) + e * 0x18 + 6,
                               (uint8_t)(cnt - 1));                /* 0x5238e dec the entry counter */
                        cnt--;                                     /* 0x52392 */
                    }
                    uint32_t co = SGP32(0x95834 + song * 6), cs = SGP16(0x95838 + song * 6);
                    if (co != 0 || cs != 0)                        /* 0x5239f: would cb(song,t,id,cnt)
                                                                    * 0x523d6 + the 0x97034 machinery */
                        if_seq_cb_unexpected(co, cs, "loop (0x95834)");
                    if (cnt != 0) {                                /* 0x52407 loop again */
                        for (uint32_t tt = 1; tt < SGP32(0x92f7c + song * 4); tt++) { /* 0x52411 */
                            uint32_t trk2 = song * 0xc0 + tt * 6;
                            if (SGP32(0x9212c + trk2) == 0 && SGP16(0x92130 + trk2) == 0)
                                continue;                          /* 0x52449 */
                            uint32_t e2 = if_seq_find_entry(song, tt, id);   /* 0x52460 */
                            if_seq_redirect(song, tt, e2);         /* 0x5249f..0x52510 */
                            uint32_t n = decode_midi_varlen(SGP32(0x9212c + trk2),
                                             SGP16(0x92130 + trk2),
                                             CANON(0x9272c + song * 0x80 + tt * 4),
                                             if_cur_ds());         /* 0x5255b -> 0x47150 */
                            SGP32(0x9212c + trk2) += n;            /* 0x5259b */
                            SGP32(0x92b2c + song * 0x80 + tt * 4) = 0;       /* 0x525ba tick reset */
                            need_delta = 0;                        /* 0x525c4 */
                            if_seq_replay_state(song, tt, e2);     /* 0x525d8 -> 0x53006 */
                        }
                        len = 0;                                   /* 0x525e2 */
                    }
                    break; }
                case 0x72: {                                       /* 0x525f3 BRANCH (all tracks) */
                    uint32_t id = sq_fr8(SGP16(0x92130 + trk), SGP32(0x9212c + trk) + 2)
                                  | 0x80u;                         /* 0x52614 */
                    (void)if_seq_find_entry(song, t, id);          /* 0x52623 presence walk (result
                                                                    * unused; hangs if absent — as the
                                                                    * original does) */
                    int taken = 1;                                 /* 0x5265e */
                    uint32_t co = SGP32(0x95804 + song * 6), cs = SGP16(0x95808 + song * 6);
                    if (co != 0 || cs != 0)                        /* 0x5266f: would cb(song,t,id) */
                        if_seq_cb_unexpected(co, cs, "branch (0x95804)");
                    if (taken) {                                   /* 0x526d3 */
                        for (uint32_t tt = 1; tt < SGP32(0x92f7c + song * 4); tt++) { /* 0x526dd */
                            uint32_t trk2 = song * 0xc0 + tt * 6;
                            if (SGP32(0x9212c + trk2) == 0 && SGP16(0x92130 + trk2) == 0)
                                continue;                          /* 0x52715 */
                            uint32_t e2 = if_seq_find_entry(song, tt, id);   /* 0x5272c */
                            if_seq_redirect(song, tt, e2);         /* 0x5276b..0x527dc */
                            uint32_t n = decode_midi_varlen(SGP32(0x9212c + trk2),
                                             SGP16(0x92130 + trk2),
                                             CANON(0x9272c + song * 0x80 + tt * 4),
                                             if_cur_ds());         /* 0x52827 */
                            SGP32(0x9212c + trk2) += n;            /* 0x52867 */
                            SGP32(0x92b2c + song * 0x80 + tt * 4) = 0;       /* 0x52886 */
                            need_delta = 0;                        /* 0x52890 */
                            if_seq_replay_state(song, tt, e2);     /* 0x528a4 */
                        }
                        len = 0;                                   /* 0x528ae */
                    }
                    break; }
                case 0x75: case 0x76: {                            /* 0x52981 SELF-LOOP with count */
                    uint32_t id = sq_fr8(SGP16(0x92130 + trk), SGP32(0x9212c + trk) + 2); /* 0x5299e */
                    uint32_t e  = if_seq_find_entry(song, t, id);  /* 0x529a6 */
                    uint32_t cnt = sq_fr8(SGP16(0x95208 + trk),
                                          SGP32(0x95204 + trk) + e * 0x18 + 6);  /* 0x52a14 */
                    if (cnt != 0xffu && cnt != 0) {                /* 0x52a1c/0x52a25 */
                        sq_fw8(SGP16(0x95208 + trk), SGP32(0x95204 + trk) + e * 0x18 + 6,
                               (uint8_t)(cnt - 1));                /* 0x52a59 */
                        cnt--;                                     /* 0x52a5d */
                    }
                    uint32_t co = SGP32(0x95834 + song * 6), cs = SGP16(0x95838 + song * 6);
                    if (co != 0 || cs != 0)                        /* 0x52a6a: would cb(song,t,id,cnt) */
                        if_seq_cb_unexpected(co, cs, "loop (0x95834)");
                    if (cnt != 0) {                                /* 0x52acb */
                        if_seq_redirect(song, t, e);               /* 0x52ad5..0x52b51 (this track;
                                                                    * NO varlen / tick reset here — the
                                                                    * common tail decodes the delta) */
                        if_seq_replay_state(song, t, e);           /* 0x52b65 */
                        len = 0;                                   /* 0x52b6a */
                    }
                    break; }
                case 0x77: {                                       /* 0x52b76 TRIGGER callback */
                    uint32_t id = sq_fr8(SGP16(0x92130 + trk), SGP32(0x9212c + trk) + 2); /* 0x52b93 */
                    uint32_t co = SGP32(0x95864 + song * 0x2fa + id * 6);
                    uint32_t cs = SGP16(0x95868 + song * 0x2fa + id * 6);
                    if (co != 0 || cs != 0)                        /* 0x52bae: would cb(song,t,id) */
                        if_seq_cb_unexpected(co, cs, "trigger (0x95864)");
                    break; }
                case 0x79: {                                       /* 0x52c1a BRANCH (this track) */
                    uint32_t id = sq_fr8(SGP16(0x92130 + trk), SGP32(0x9212c + trk) + 2); /* 0x52c37,
                                                                    * NO |0x80 (unlike CC 0x72) */
                    uint32_t e  = if_seq_find_entry(song, t, id);  /* 0x52c3f */
                    int taken = 1;                                 /* 0x52c81 */
                    uint32_t co = SGP32(0x95804 + song * 6), cs = SGP16(0x95808 + song * 6);
                    if (co != 0 || cs != 0)                        /* 0x52c92: would cb(song,t,id) */
                        if_seq_cb_unexpected(co, cs, "branch (0x95804)");
                    if (taken) {                                   /* 0x52cef */
                        if_seq_redirect(song, t, e);               /* 0x52cf9..0x52d75 */
                        if_seq_replay_state(song, t, e);           /* 0x52d89 */
                        len = 0;                                   /* 0x52d8e */
                    }
                    break; }
                default:                                           /* cc 0x69/0x6a/0x6b (0x52d97) +
                                                                    * outside 0x67..0x79 (0x52df4 ja):
                                                                    * a NORMAL controller — dispatch */
                    do_dispatch = 1;
                    break;
                }
            } else {                                               /* channel voice msg (notes etc.) */
                do_dispatch = 1;
            }
            if (do_dispatch && t != 0) {                           /* 0x52e06/0x52e0a (track 0 = the
                                                                    * conductor: never dispatched) */
                uint32_t dev = sq_fr32(SGP16(0x92f30 + song * 6),
                                       SGP32(0x92f2c + song * 6) + t * 4);   /* 0x52e32 device map */
                os_audio_midi_dispatch(song, dev, SGP32(0x9212c + trk),
                                       SGP16(0x92130 + trk));      /* 0x52e63 -> 0x44e0d native (the
                                                                    * len stack arg is dropped) */
            }
            /* ---- common advance (0x52e68) ---- */
            if (SGP8(0x97034) == 0)                                /* (always: no cb ever runs) */
                SGP32(tick) = 0;                                   /* 0x52e86 tick reset */
            if (SGP32(0x9212c + trk) == 0 && SGP16(0x92130 + trk) == 0)
                break;                                             /* 0x52eb7 track died -> next track */
            SGP32(0x9212c + trk) += len;                           /* 0x52ee7..0x52f17 (sel unchanged) */
            if (need_delta) {                                      /* 0x52f1d */
                uint32_t n = decode_midi_varlen(SGP32(0x9212c + trk), SGP16(0x92130 + trk),
                                                       CANON(dly), if_cur_ds());  /* 0x52f70 */
                SGP32(0x9212c + trk) += n;                         /* 0x52fa2..0x52fd2 */
            }
        } while (SGP32(dly) == 0);                                 /* 0x52fed drain the same-tick batch */
    }                                                              /* 0x52ffa -> 0x51d78 next track */
}

/* Per-SIGALRM-tick image-free poll body — the MAGIC_POLL case of audio_trap minus the cpu_t frame
 * and minus the movie-stream drive (which lives in shm_tick's audio-cb GDV stand-in; see
 * audio_standalone_stream_ship below). Called from shm_tick under #ifdef ROTH_STANDALONE. */
void audio_standalone_tick(void)
{
    if (!g_standalone_boot || !g_farg_base)
        return;                                   /* install not staged yet (fn0xa not run) */
    {
        static int armed;
        if (!armed) {
            armed = 1;
            ALOG("imgfree pump armed: driving the digital poll natively (farg=%#x; mixer + done-cb + "
                 "pos advance; the SOS master-timer walk 0x49eaf never runs image-free)\n", g_farg_base);
        }
    }
    if (g_pos_lin)                                /* the poll's play-position advance */
        *(uint32_t *)(uintptr_t)g_pos_lin += g_au_chunk ? g_au_chunk : 1;
    audio_voice_dump(NULL);                       /* g_mtick++ + the (gated) voice probes */
    audio_profile_dump();
    au_trace_drain();
    {                                             /* ~30s sample-flow totals (mechanical observable) */
        static unsigned long t;
        if ((++t % 2100) == 0 && (g_ifp_sfx_pcm || g_ifp_gdv_bytes || g_ifp_seq_steps))
            ALOG("imgfree pump: sample-flow totals — SFX/speech PCM %lu B, movie stream %lu B, "
                 "MIDI seq steps %lu (midi ring w=%u) (ring w=%u r=%u underruns=%u)\n",
                 g_ifp_sfx_pcm, g_ifp_gdv_bytes, g_ifp_seq_steps, g_midi ? g_midi->w : 0,
                 g_au ? g_au->w : 0, g_au ? g_au->r : 0, g_au ? g_au->underruns : 0);
    }
    /* ---- the MIDI sequencer step (image-free MUSIC) -----------------------------------
     * step_audio_sequence (lift_audio.c:1497) registers sos_sequence_timer_tick — the table
     * slot's cb offset is GADDR(0x51ad5) == CANON(0x51ad5) — at the music driver's tick rate
     * (descriptor+0x38), and maps the slot's track byte [0x741fc+slot]=song. Fire it at that
     * REGISTERED rate, not the SIGALRM rate: this transcribes the master ISR's per-slot fire logic
     * (disasm 0x49f0d..0x49f60) — Q16 accumulator [0x97aa4+i*4] += step [0x97a64+i*4], fire on the
     * bit-16 carry (`test byte [acc+2],1`), clear the acc high word, publish the track byte into
     * [0x97ae4] — for the ENUMERATED 0x51ad5 slots only (the other events keep their dispositions in
     * the header comment; their accumulators stay untouched, as before). SIGALRM ticks at
     * the PIT rate (the imgfree timer natives retune it via sos_program_pit -> host_pit_program, and
     * the register native recomputes every step against that base), so the accumulator math divides
     * each slot down to its own registered rate exactly as the original ISR did. Skipped while a
     * timer native holds the table-edit fence (au_timer_lock; traps.c) — the original DEFERS the
     * whole ISR tick there (inject_irq int-8 deferral); the digital pump above never reads the table
     * so it stays un-gated, unchanged. */
    if (!g_au_timer_locked) {
        for (uint32_t i = 0; i < 0x10u; i++) {
            if (*(volatile uint32_t *)(uintptr_t)CANON(0x979c4 + i * 6) != CANON(0x51ad5))
                continue;                                          /* not the music step */
            *(volatile uint32_t *)(uintptr_t)CANON(0x97aa4 + i * 4) +=
                *(volatile uint32_t *)(uintptr_t)CANON(0x97a64 + i * 4);     /* 0x49f13 acc += step */
            if (!(*(volatile uint8_t *)(uintptr_t)CANON(0x97aa6 + i * 4) & 1))
                continue;                                          /* 0x49f21 no bit-16 carry: not due */
            *(volatile uint16_t *)(uintptr_t)CANON(0x97aa6 + i * 4) = 0;     /* 0x49f32 clear hi word */
            if (*(volatile uint8_t *)(uintptr_t)CANON(0x741fc + i) != 0xff)  /* 0x49f40 */
                *(volatile uint8_t *)(uintptr_t)CANON(0x97ae4) =
                    *(volatile uint8_t *)(uintptr_t)CANON(0x741fc + i);      /* 0x49f54 publish song */
            {
                static int armed_seq;
                if (!armed_seq) {
                    armed_seq = 1;
                    uint32_t rate = *(volatile uint32_t *)(uintptr_t)CANON(0x97a24 + i * 4);
                    ALOG("imgfree pump: MIDI sequencer slot %u armed natively (registered rate %u Hz, "
                         "Q16 step %#x, song %u) — firing sos_sequence_timer_tick 0x51ad5 as host C\n",
                         i, rate, *(volatile uint32_t *)(uintptr_t)CANON(0x97a64 + i * 4),
                         *(volatile uint8_t *)(uintptr_t)CANON(0x97ae4));
                }
            }
            audio_if_seq_step();                                   /* 0x49f60 far-call -> the native */
            g_ifp_seq_steps++;
        }
    }
    if (g_stream_fed > 0) {
        g_stream_fed--;                           /* a movie owns the ring; SFX yield */
        sfx_trace_tick(0, 1 /* movie active: SFX legitimately silent */);
    } else {
        uint32_t w0 = g_au ? g_au->w : 0;
        int mixed = audio_mix_sfx();
        uint32_t mixed_frames = (mixed && g_au) ? (g_au->w - w0) / 4u : 0;
        if (mixed && g_au) {
            g_ifp_sfx_pcm += g_au->w - w0;
            static int first;
            if (!first) {
                first = 1;
                ALOG("imgfree pump: FIRST SFX/speech PCM mixed into the ring (t=%lu, %u bytes) — "
                     "sample flow proven\n", g_mtick, g_au->w - w0);
            }
        }
        sfx_trace_tick(mixed_frames, 0);
        audio_if_deliver_cbs();
    }
}

/* ---- image-free movie-stream audio: the decode+queue+ship half of gdv_audio_stream_callback ------
 * Trap lane: the host paces "buffer-complete" far-calls into the ORIGINAL 0x4e394 (g_drive_fn), whose
 * arg1==0 arm decodes one audio block via `call [0x91898]` (EBX=[0x91d70]) and queues the result to
 * the stream voice via 0x55620; MAGIC_AFTER then ships the decoded PCM ([0x91d4c], [0x91d28]>>1
 * frames) to the ring. Image-free 0x4e394 is original bytes (host_audio_driver, never lifted) and the
 * validated shm_tick stand-in replicates only its VIDEO bookkeeping ([0x91db8]--/[0x91db4]++/the
 * [0x91d70] advance) — the audio half was "skipped, cutscene plays silently" (traps.c). This function
 * IS that audio half, called by the stand-in at its consume decision, just before the bookkeeping:
 *   - `call [0x91898]` -> the enumerated 3-writer dispatch (0x4e45f bare-ret / 0x4e460 16-bit /
 *     0x4e519 8-bit), decoder replicas below — duplicated from os_audio.c's gdv_decode16/8
 *     statics (disasm 0x4e460..0x4e4cc / 0x4e519..0x4e58f) because audio_c2_host.o is a trap-shared
 *     TU under strip-identity (an export there would change the trap lane's .o); the shared
 *     accumulators [0x9189c]/[0x918a0] keep both users (begin_playback native decodes block 1, this
 *     decodes blocks 2..n) byte-consistent.
 *   - 0x55620 gdv_audio_queue_buffer (disasm 0x55620..0x5563a): [0x97c5c]=decoder-out, [0x97c60]=DS,
 *     voice_load_to_slot(EAX=arg0, EDX=arg2, EBX=&0x97c5c, ECX=DS) — the host drive passes (0,0,0),
 *     so handle=0 slot=0; routed through the c2 dispatcher (KEY COMPOSITION RULE).
 *   - the MAGIC_AFTER ship of [0x91d4c] with the exact same guards, + the g_stream_fed SFX yield. */
static uint32_t if_gdv_decode16(uint32_t src)     /* [0x91898]==CANON(0x4e460); see os_audio.c */
{
    const uint8_t *S = (const uint8_t *)(uintptr_t)src;
    uint32_t count = *(uint32_t *)(uintptr_t)CANON(0x91d28);
    uint32_t a = *(uint32_t *)(uintptr_t)CANON(0x9189c);
    uint32_t b = *(uint32_t *)(uintptr_t)CANON(0x918a0);
    if (S[count + 4] & 0x40) { a = 0; b = 0; }
    int32_t  c = (int32_t)(count >> 1);
    int16_t *D = (int16_t *)(uintptr_t)*(uint32_t *)(uintptr_t)CANON(0x91d4c);
    const uint8_t *p = S;
    do {
        a += *(uint32_t *)(uintptr_t)CANON(0x918a4 + (uint32_t)p[0] * 4);
        D[0] = (int16_t)a;
        b += *(uint32_t *)(uintptr_t)CANON(0x918a4 + (uint32_t)p[1] * 4);
        D[1] = (int16_t)b;
        p += 2; D += 2;
    } while (--c > 0);
    *(uint32_t *)(uintptr_t)CANON(0x9189c) = a;
    *(uint32_t *)(uintptr_t)CANON(0x918a0) = b;
    return *(uint32_t *)(uintptr_t)CANON(0x91d4c);
}
static uint32_t if_gdv_decode8(uint32_t src)      /* [0x91898]==CANON(0x4e519); see os_audio.c */
{
    const uint8_t *S = (const uint8_t *)(uintptr_t)src;
    uint32_t count = *(uint32_t *)(uintptr_t)CANON(0x91d28);
    uint32_t a = *(uint32_t *)(uintptr_t)CANON(0x9189c);
    uint32_t b = *(uint32_t *)(uintptr_t)CANON(0x918a0);
    if (S[count + 4] & 0x40) { a = 0; b = 0; }
    int32_t  c = (int32_t)(count >> 1);
    uint8_t *D = (uint8_t *)(uintptr_t)*(uint32_t *)(uintptr_t)CANON(0x91d4c);
    const uint8_t *p = S;
    do {
        a += *(uint32_t *)(uintptr_t)CANON(0x918a4 + (uint32_t)p[0] * 4);
        D[0] = (uint8_t)((a >> 8) + 0x80);
        b += *(uint32_t *)(uintptr_t)CANON(0x918a4 + (uint32_t)p[1] * 4);
        D[1] = (uint8_t)((b >> 8) + 0x80);
        p += 2; D += 2;
    } while (--c > 0);
    *(uint32_t *)(uintptr_t)CANON(0x9189c) = a;
    *(uint32_t *)(uintptr_t)CANON(0x918a0) = b;
    return *(uint32_t *)(uintptr_t)CANON(0x91d4c);
}

void audio_standalone_stream_ship(void)
{
    uint32_t src = *(uint32_t *)(uintptr_t)CANON(0x91d70);      /* 0x4e3ee mov ebx,[0x91d70] */
    uint32_t handler = *(uint32_t *)(uintptr_t)CANON(0x91898);  /* 0x4e3f4 call [0x91898] */
    uint32_t out;
    if (handler == CANON(0x4e45f))
        out = src;                                              /* bare ret: EBX passthrough */
    else if (handler == CANON(0x4e460))
        out = if_gdv_decode16(src);
    else if (handler == CANON(0x4e519))
        out = if_gdv_decode8(src);
    else {
        static int warned;
        if (!warned) {
            warned = 1;
            ALOG("imgfree stream: UNKNOWN [0x91898]=%#x (expected CANON of 0x4e45f/0x4e460/0x4e519) "
                 "— block not decoded (movie stays silent)\n", handler);
        }
        return;
    }
    /* 0x55620 gdv_audio_queue_buffer: queue the (decoded) buffer to stream voice {handle 0, slot 0} */
    *(uint32_t *)(uintptr_t)CANON(0x97c5c) = out;               /* 0x55621 */
    *(uint16_t *)(uintptr_t)CANON(0x97c60) = if_cur_ds();       /* 0x5562e */
    os_audio_voice_load_to_slot(0, 0, CANON(0x97c5c), if_cur_ds()); /* 0x55634 call 0x4ad03 */
    /* ship the decoded block to the viewer ring — the MAGIC_AFTER g_after_stream arm, same guards */
    if (handler != CANON(0x4e45f) && g_au) {
        uint32_t decbuf = *(uint32_t *)(uintptr_t)CANON(0x91d4c);
        uint32_t frames = (*(uint32_t *)(uintptr_t)CANON(0x91d28)) >> 1;
        if (decbuf > 0x10000 && frames && frames < 0x4000) {
            uint32_t n = frames * 4u; /* 16-bit * 2ch */
            const uint8_t *p = (const uint8_t *)(uintptr_t)decbuf;
            uint32_t freeb = ROTH_AUDIO_RING - (g_au->w - g_au->r);
            if (n > freeb) {
                n = freeb;
                g_au->underruns++;
            }
            /* MOVIE volume: the original applies [0x91cb0] (= (g_vol_movie&0xfff)<<7 via
             * apply_audio_volume_settings 0x2626f -> gdv_decoder_open) at the SOS voice (+0x32,
             * gdv_start_voice rule @0x55715: use it iff 0 < v < 0x6ff0, else full). Our ship path
             * bypasses that voice, so scale the s16 samples here (NOT in the decoder — game memory
             * must stay byte-identical to the trap lane). */
            { uint32_t mv = *(uint32_t *)(uintptr_t)CANON(0x91cb0);
              int32_t vol = (mv != 0 && mv < 0x6ff0u) ? (int32_t)mv : 0x7fff;
              for (uint32_t i = 0; i + 1 < n; i += 2) {
                  int16_t s = (int16_t)(p[i] | ((uint16_t)p[i + 1] << 8));
                  s = (int16_t)(((int32_t)s * vol) >> 15);
                  g_au->ring[(g_au->w + i) & ROTH_AUDIO_MASK] = (uint8_t)s;
                  g_au->ring[(g_au->w + i + 1) & ROTH_AUDIO_MASK] = (uint8_t)((uint16_t)s >> 8);
              } }
            g_au->w += n;
            if (g_pcm_fd >= 0)
                (void)!write(g_pcm_fd, p, n);   /* debug tap stays raw */
            g_stream_fed = 14; /* a movie owns the ring; SFX yield ~0.2s */
            g_ifp_gdv_bytes += n;
            static int first;
            if (!first) {
                first = 1;
                ALOG("imgfree stream: FIRST movie audio block decoded+shipped (%u frames, %u bytes) — "
                     "GDV sample flow proven\n", frames, n);
            }
        }
    }
}
