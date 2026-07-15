/* Virtual HMI digital-audio driver (recomp M4, "Option B").
 *
 * Realms of the Haunting uses the HMI Sound Operating System. The SOS (linked
 * into the EXE) calls its hardware driver through ONE far entry point with
 * EAX = function number (the thunk table at canon 0x4fe00..0x50250). Rather
 * than run the real HMIDRV.386 + emulate a Sound Blaster, we substitute a
 * trapping far-pointer for the driver dispatch and service every driver call
 * in host C, tapping the SOS software mixer's PCM and shipping it to SDL.
 *
 * Owned by the audio workstream. See src/platform/audio.c.
 */
#ifndef ROTH_AUDIO_H
#define ROTH_AUDIO_H

#include "roth_host.h"

struct roth_audio;   /* the PCM ring (shared_audio.h) */
struct roth_midi;    /* the MIDI-event ring (shared_midi.h) */

/* Plant the int3 hooks (call once after obj1 is mapped+loaded, before entry). */
void audio_init(void);

/* Select where the PCM/MIDI ring mailboxes are backed, before audio_init() runs.
 * in_process != 0 backs them with private in-process memory (the windowed default,
 * read directly by the in-process consumer); in_process == 0 backs them with named
 * shared-memory objects (so a separate viewer process can attach). Mirrors the
 * framebuffer backing choice and must be called on the boot thread before the game
 * thread starts. If never called, the shared backing is used. */
void audio_select_backing(int in_process);

/* Non-zero when the in-process backing was selected (the consumer uses this to
 * decide whether to read the producer's ring pointers directly or map its own). */
int audio_backing_in_process(void);

/* The producer's ring pointers, for a consumer sharing this address space. Each is
 * NULL until audio_init() has set the ring up (MIDI stays NULL when music is off).
 * Only meaningful under the in-process backing. */
struct roth_audio *audio_pcm_ring(void);
struct roth_midi  *audio_midi_ring(void);

/* Called at the very top of the SIGSEGV/SIGTRAP handler body. Returns 1 if the
 * fault was an audio dispatch/hook we handled (EIP advanced), 0 otherwise. */
int audio_trap(cpu_t *c);

/* Called each host timer tick (from shm_tick). Diagnostic: reports whether the
 * SOS mixer is rendering PCM into the DMA buffer. */
void audio_tick(void);

/* ===== image-free digital-audio pump (D/M3 audio residual; see audio.c "IMAGE-FREE pump") ========
 * Image-free the SOS master-timer walk (0x49eaf) never runs, so nothing fires the MAGIC_POLL digital
 * poll that the trap lane's audio_trap services — despite the c2 natives completing the whole driver
 * install. These are the poll body as callable host C, driven by shm_tick's imgfree surrogate
 * (traps_if.o, #ifdef ROTH_STANDALONE). Both are no-ops in the trap lane (g_standalone_boot gate / never
 * called). */
void audio_standalone_tick(void);        /* per-tick: mixer + done-cb delivery + play-pos advance */
void audio_standalone_stream_ship(void); /* one movie audio block: [0x91898]-decode + 0x55620 queue +
                                       * ring ship — the audio half of gdv_audio_stream_callback
                                       * 0x4e394; call at the GDV stand-in's consume decision */

/* ===== §3.2 extracted virtual-driver services (no cpu_t) =====================================
 * The M4 driver's .386-function responses as callable host C. audio_trap calls these to service
 * a MAGIC fault (image-based trap path, byte-identical); the C2 host binding (os_audio.c)
 * calls them directly instead of via a fault. See src/platform/audio.c "§3.2 extracted". */
uint32_t haudio_detect_card(uint32_t base);                       /* fn 2/8: stage SB16 desc; ret DESC_OFF */
void     haudio_open_driver(uint32_t fbase, uint32_t reqsz,       /* fn 0xa: stage voices/pos, report */
                            uint32_t *out_cb, uint32_t *out_voices, uint32_t *out_pos);

/* The runtime-determined values the lifted open_voices native (os_audio_open_voices_native,
 * os_audio.c) must stamp into game memory. The service allocates the far-args segment (the
 * DPMI seg the veneer's 0x54441/0x5473c allocate) + the moot DMA/streaming descriptors and runs the
 * fn-0xa staging (haudio_open_driver) so these mirror the trap-serviced original by construction.
 * All far pointers are {off, sel}; sel is a real LDT selector whose base dpmi_sel_base() resolves. */
struct haudio_open_desc {
    uint16_t farg_sel;      /* far-args segment selector (the veneer's DPMI 0x5f); voices/pos use it */
    uint32_t farg_off;      /* far-args segment base offset (0) — {farg_off,farg_sel} = FS:0 */
    uint16_t cb_sel;        /* poll-cb code selector (game CS) — the ISR loads it far-calling the cb */
    uint32_t cb_off;        /* MAGIC_POLL: the host poll trampoline (haudio_open_driver *out_cb) */
    uint32_t voices_off;    /* VOICE_OFF (0x40): base of the 32 voice structs in the far-args seg */
    uint32_t pos_off;       /* POS_OFF (0xdc0): the play-position dword offset */
    uint32_t dispatch_off;  /* MAGIC_OFF: driver-dispatch trampoline (dispatch-computer result); the
                             * native never far-calls it — it is only stashed into desc/slot tables */
    uint16_t dma_sel;       /* moot DMA real-mode descriptor selector (valid, non-zero) */
    uint32_t dma_off;       /* moot DMA buffer offset */
    uint16_t bufb_sel;      /* moot streaming decode-buffer-B selector (valid, non-zero) */
    uint32_t bufb_off;      /* moot streaming decode-buffer-B offset */
};

/* Allocate/bind the digital driver's far-args + DMA/streaming segments and run fn-0xa staging.
 * `reqsz` = the SOS request size (R descriptor +0). Returns 0 and fills *d on success; nonzero on a
 * host allocation failure (the native then returns that as open_voices' error, like 0x54441's ret). */
int      haudio_open_voices_service(uint32_t reqsz, struct haudio_open_desc *d);
void     haudio_midi_load_descriptor(uint32_t mbase, uint32_t woff); /* MIDI fn 0 */
void     haudio_midi_load_table(uint32_t mbase, uint32_t woff);      /* MIDI fn 1 */
void     haudio_midi_event(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3); /* MAGIC_MIDI capture */
uint32_t haudio_midi_send(uint32_t moff, uint16_t msel);           /* read msg@(off:sel) + capture */
uint32_t haudio_dispatch_simple(uint32_t slot, uint32_t param, uint16_t sel); /* 0x45d28 host body */
uint32_t haudio_dispatch_method(uint32_t slot, unsigned method, uint32_t param, uint16_t sel); /* any vtable method */
void     haudio_stage_driver_descriptor(uint32_t out, uint16_t sel); /* find_driver fn2: SB16 desc -> `out` */

/* ===== §13 driver-install service — the alloc/free driver-slot (0x44553/0x44a81) linchpin's host half.
 * Mirrors haudio_open_voices_service's role for open_voices (§9): the veneer path runs the SOS driver
 * install/teardown machinery (0x51681 loads HMIMDRV.386 + creates a DPMI descriptor; 0x541ad far-calls
 * the loaded driver and copies its 12 method far-ptrs into the per-slot vtable [0x92f9c+slot*0x48]).
 * Under the host that driver far-call faults to a MAGIC page, so r1 captures the whole vtable becoming
 * MAGIC_MIDI+N*4 (sel 0x23). This service supplies that observable end-state image-free: it (a) allocates
 * a real host-backed driver-descriptor selector ONCE and reuses it — making [0x920dc]'s run-to-run
 * NON-deterministic DPMI handle a STABLE host value (the §9 selector precedent); (b) stamps the 12-method
 * MAGIC vtable directly; (c) returns the moot-but-valid far-ptr/handle values the alloc native transcribes
 * into the slot descriptor tables. All those handle/far-ptr values are read ONLY by DPMI real-mode
 * plumbing (0x4fbd2 unlock / 0x4fc9d free / 0x4fad9 thunk) that is a host no-op, so their exact bytes are
 * game-immaterial — a stable host selector is byte-safe and deterministic. */
struct haudio_driver_desc {
    uint32_t dpmi_handle;   /* [0x920dc+slot*4]: DPMI descriptor handle (host, deterministic per run) */
    uint32_t lock_handle;   /* [0x9206c+slot*4]: dpmi_lock_linear_region handle (0x51681 local_24) */
    uint8_t  chan_byte;     /* [0x92080+slot*4]: the HMIMDRV.386 record byte ([0x72984] lo) — moot */
    uint16_t fptrA_sel;     /* far-ptr A sel  -> gs:[info+8],   [0x92050+slot*6] */
    uint32_t fptrA_off;     /* far-ptr A off  -> gs:[info+4],   [0x9204c+slot*6] */
    uint16_t fptrB_sel;     /* far-ptr B sel  -> gs:[info+0x10], [0x92098+slot*6] (== the vtable selector) */
    uint32_t fptrB_off;     /* far-ptr B off  -> gs:[info+0xc],  [0x92094+slot*6] (loaded driver-code linear) */
};
/* Alloc-once the driver descriptor + stamp the slot's 12-method MAGIC vtable; fill *d with the runtime
 * values 0x51681 would produce. Returns 0 on success, 0xf on host-alloc failure (0x51681's crt_open-fail
 * return code). `device` is currently informational (the descriptor is device-agnostic under the host). */
int      haudio_driver_install_service(uint32_t slot, uint32_t device, struct haudio_driver_desc *d);

/* audio_trace.c snapshot hook (host-lane co-dev tool only): host-linear base of the 32 fn-0xa voice
 * structs — the voice_start/voice_load_to_slot write-set (each 0x6c bytes, contiguous at
 * g_farg_base+VOICE_OFF) — and their total byte span in *out_span (VOICE_N*VOICE_SZ = 0xd80).
 * Returns 0 until fn 0xa (haudio_open_driver) has staged the far-args segment. Read-only, no state,
 * no behavior change; not on any hot path. */
uint32_t haudio_voice_struct_base(uint32_t *out_span);

/* ===== §SFX-DROPOUT STANDING TRACE kit (ROTH_SFX_TRACE), v2 — see audio.c "§SFX-DROPOUT STANDING TRACE".
 * sfx_trace_voice_start: the imgfree voice-start veneer (os_audio_standalone.c) reports each native
 *   result here — 0xffffffff = no-free-slot FAILURE (the real dropout signature), else the slot index.
 *   Drives the START-FAIL auto-detector (N consecutive misses).
 * sfx_trace_exit_dump: the host exit hooks (main.c clean returns + traps.c shm_tick window-close _exit)
 *   flush the trace file once on quit — so the director just plays, quits, and sends the file.
 * Both are gated no-ops (one cached-int test) when ROTH_SFX_TRACE is unset. */
void sfx_trace_voice_start(uint32_t result);
void sfx_trace_exit_dump(void);

#endif
