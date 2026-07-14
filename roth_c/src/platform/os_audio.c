/* os_audio.c — the FINAL host binding of the C2 audio API (see os_api.h).
 *
 * Host-C reimplementation of the SOS-library seam, over src/platform/audio.c's extracted haudio_*
 * services + the SOS bookkeeping the original library owned where the client observes it.
 * Unlike the INTERIM audio_c2_bridge.c (which call_origs the
 * original SOS bytes), the functions here run NO original image bytes for their VA, so they link
 * image-free — this is what makes the audio slice image-free-real.
 *
 * SCOPE (this file): the subset whose FULL observable effect is provably byte-faithful to the
 * trap-serviced original from disasm+client-reads alone — the GDV-audio wrappers that are pure
 * memory/table ops or thin orchestration over the c2 API. These reproduce the trapped original
 * EXACTLY (moot hardware — PIT/IRQ/DPMI — has no game-memory effect and is dropped), so in-game
 * behaviour for these VAs is UNCHANGED. Every sub-call routes back through the os_audio_* API so it
 * inherits the same binding (host-C here, or the interim call_orig fallback in audio_c2_bridge.c for
 * VAs not yet retired) — no direct original-byte call from this TU.
 *
 * The deep driver-load state machines (open_voices/voice_start/alloc_driver_slot/detection/
 * midi_dispatch's device-remap/timer-table-under-the-live-SOS-ISR/…) STAY on the interim call_orig
 * binding in audio_c2_bridge.c (per os_api.h): their observable effect
 * branches on runtime driver-slot/selector state populated by a chain of MAGIC-serviced far-calls and
 * consumed by the still-running original SOS timer ISR, so a blind (no in-game validation)
 * reimplementation cannot be proven byte-faithful. They are the
 * in-game co-development items; keeping them call_orig means ZERO
 * in-game drift now (each VA is either byte-faithful host-C here or the identical original via
 * call_orig) and the correct "audio not fully image-free yet" imgfree-link signal remains on them.
 *
 * Built into roth-host only (like os_api.c / audio_c2_bridge.c). The oracle links c2_mock.c instead
 * (same contract, canned) — so this binding is NOT exercised by the differential (the
 * host binding rides the in-game ROTH_LIFT=audio tier, not the oracle).
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>               /* stderr for the provably-unreachable [0x91898] fallback */
#include "os_api.h"
#include "roth_host.h"   /* OBJ_DELTA, dpmi_sel_base */
#include "audio.h"       /* haudio_* driver services (haudio_dispatch_simple) */
#include "g_names.h"             /* VA_<global> canon-VA constants for readable G-macro sites (generated) */

/* Game global at canon VA -> host linear (obj1 mapped at OBJ1_BASE in roth-host). Same convention as
 * src/platform/audio.c's CANON. A relocated code/data immediate stored into a global is itself canon,
 * so CANON() the value too (e.g. the 0x4e519 handler ptr gdv_init_silence installs). */
#define CANON(x) ((uint32_t)((x) + OBJ_DELTA))
#define GP32(x)  (*(volatile uint32_t *)(uintptr_t)CANON(x))
#define GP16(x)  (*(volatile uint16_t *)(uintptr_t)CANON(x))
#define GP8(x)   (*(volatile uint8_t  *)(uintptr_t)CANON(x))

/* The live host DS/CS selectors (flat, base 0 via dpmi_sel_base) — the same cur_ds()/cur_cs() the lifted
 * client passes to the os_audio_* dispatchers. A composition native that hands a c2 call a canon offset +
 * cur_ds()/cur_cs() reproduces the original's far pointer exactly (dpmi_sel_base(host DS/CS)=0, so
 * far[ds:CANON(x)] = CANON(x) = the runtime global; matches lift_audio.c's GADDR(...) + cur_ds()). */
static inline uint16_t cur_ds(void) { uint16_t v; __asm__("mov %%ds,%0" : "=r"(v)); return v; }
static inline uint16_t cur_cs(void) { uint16_t v; __asm__("mov %%cs,%0" : "=r"(v)); return v; }

/* 0x4e4f1 gdv_audio_init_silence — install the ADPCM decode entry [0x91898]=0x4e519 and fill the
 * silence/mix buffer [0x91d38] with unsigned-PCM silence (0x80808080), count=([0x91d2c]+7)>>3 dwords.
 * Pure memory; no driver call. Disasm 0x4e4f1..0x4e518. */
uint32_t os_audio_gdv_init_silence(void)
{
    GP32(0x91898) = CANON(0x4e519);
    uint32_t ndw = (GP32(0x91d2c) + 7) >> 3;
    uint32_t dst = GP32(0x91d38);                 /* runtime linear ptr to the silence buffer */
    if (dst)
        memset((void *)(uintptr_t)dst, 0x80, ndw * 4u); /* rep stosd 0x80808080 */
    return 0;
}

/* 0x552f0 gdv_audio_detect_driver — copy the 0x6c-byte driver descriptor from [0x91d04] into the
 * detect scratch block 0x97b80 (skipped when [0x91d04]==0). Pure memcpy; the args (dev/drvid) are
 * consumed into dead temps by the original, and the client discards EAX. Disasm 0x552f0..0x55320. */
uint32_t os_audio_gdv_detect_driver(uint32_t dev, uint32_t drvid)
{
    (void)dev; (void)drvid;
    uint32_t src = GP32(0x91d04);
    if (src)
        memcpy((void *)(uintptr_t)CANON(0x97b80), (const void *)(uintptr_t)src, 0x6c);
    return 0;
}

/* 0x55640 gdv_audio_stop_voice — if the "voice open" flag bit [0x91dc2]&0x40 is set, deactivate the
 * stream voice (handle=[0x97cdc], voice=[0x97ce0]) and clear the bit. The deactivate routes through
 * os_audio_voice_field op 0 = 0x4ac55 (a retired PURE-GAME-CODE far-ptr field op on the real voice
 * struct in the 0x97440 table open_voices populated — NOT a null-SOS no-op). Disasm 0x55640..0x55663. */
uint32_t os_audio_gdv_stop_voice(void)
{
    if (GP16(0x91dc2) & 0x40) {
        os_audio_voice_field(0, GP32(0x97cdc), GP32(0x97ce0), 0);
        GP16(0x91dc2) -= 0x40;
    }
    return 0;
}

/* 0x553b0 gdv_audio_shutdown — tear down whatever the GDV audio setup brought up, gated per [0x91dc2]
 * flag bit, in the original's order. Each teardown routes through the os_audio_* API (host-C here or
 * the interim call_orig fallback), so it is binding-consistent and byte-faithful. NB the original
 * clears ONLY the 0x40 bit (the deactivate branch); the other branches call but do not clear their
 * bit — reproduced verbatim. Disasm 0x553b0..0x55433. */
uint32_t os_audio_gdv_shutdown(void)
{
    if (GP16(0x91dc2) & 0x40) {                              /* stream voice active */
        os_audio_voice_field(0, GP32(0x97cdc), GP32(0x97ce0), 0); /* 0x4ac55 deactivate */
        GP16(0x91dc2) -= 0x40;
    }
    if (GP16(0x91dc2) & 0x10)                                /* decode timer event */
        os_audio_timer_remove_event(GP32(0x97cd4));          /* 0x49ca4 */
    if (GP16(0x91dc2) & 0x08)                                /* second timer event */
        os_audio_timer_remove_event(GP32(0x97cd8));          /* 0x49ca4 */
    if (GP16(0x91dc2) & 0x04)                                /* digital driver open */
        os_audio_close_voices(GP32(0x97cdc), 1, 1);          /* 0x48666 (ebx=edx=1) */
    if (GP16(0x91dc2) & 0x02)                                /* HMI timer running */
        os_audio_stop_timer_service();                       /* 0x498e9 */
    if (GP16(0x91dc2) & 0x01)                                /* output callback enabled */
        os_audio_disable_callback();                         /* 0x47d6e */
    return 0;
}

/* ---- GDV driver-open orchestration (retirement) ------------------------------------------
 * 0x55440 gdv_audio_setup_voices — the richest GDV driver-state path: enable the output callback,
 * configure the HMI timer rate, open the digital driver voices, then register the per-buffer + master
 * decode timer events, recording each completed step in the g_gdv_audio_init_flags bitmask [0x91dc2] so
 * gdv_audio_shutdown can unwind exactly what was set up. This native is pure COMPOSITION (the KEY
 * COMPOSITION RULE): every SOS sub-call routes BACK through the os_audio_* DISPATCHERS (not the _native
 * bodies), so the per-VA au_ab_va precedence keeps working — enable_callback (0x47cf5) + open_voices
 * (0x47dae) run their retired natives under the default config, while configure_timer_rate (0x4980d) and
 * the two sos_timer_register_event (0x49923) calls STAY on their call_orig veneers (the timer cluster
 * retires LAST). The rest is the disasm's own game-memory stores + a stack request descriptor R built
 * from the 0x97cec template (the `rep movsd` of 19 dwords) with the four field pokes. Transcribed
 * store-by-store from disasm 0x55440..0x55615 (canon VAs inline).
 *
 * Arg map (disasm 0x55446..0x5544c): EAX=dev (=[ctx+4], a rebased service-descriptor ptr, default
 * CANON(0x91dc6) — lift_gdv_cutscene.c:1439), EDX=param_2=[0x91d30], EBX=param_3=0x5622 (-> R+0x10),
 * ECX=param_4=0x3c (noaudio timer rate + [0x9187c] budget). Client discards the return (checks [0x91d10]).
 *
 * Far-ptr convention: canon global offsets + cur_ds()/cur_cs() (host DS/CS base 0), exactly as
 * lift_audio.c's open_voices/timer sites (GADDR(...) + cur_ds()). The R descriptor is a host-stack buffer
 * whose flat address open_voices derefs directly (native) or via far[ds:R]=R (veneer, base-0 DS) — the
 * same runtime-flat contract GADDR addresses use. STAGED ONLY (see os_api.h). */
uint32_t os_audio_gdv_setup_voices_native(uint32_t dev, uint32_t edx, uint32_t ebx, uint32_t ecx)
{
    uint32_t p2  = edx;                             /* param_2 = [0x91d30]; doubled for 16-bit/stereo */
    uint32_t p4  = ecx;                             /* param_4 = 0x3c (noaudio rate + budget scale) */
    uint32_t err = 0;                               /* ebx accumulator (0 = ok) */

    /* 0x55450 rep movsd (ecx=0x13): the request descriptor R <- template [0x97cec], 19 dwords (0x4c). */
    uint8_t R[0x4c];
    memcpy(R, (const void *)(uintptr_t)CANON(0x97cec), sizeof R);

    /* --- enable output callback (0x55466..0x5549f) --- */
    if (GP8(0x91dc8) != 0) {                        /* 0x55466 g_gdv_audio_enabled */
        if (GP32(0x97be4) == 0) {                   /* 0x55470 no device id -> disable audio */
            GP8(0x91dc8) = 0;                       /* 0x5549f (xor dh,bh; bh=0 -> byte[0x91dc8]=0) */
        } else if (!(GP8(0x91d0d) & 8)) {           /* 0x55474 g_gdv_stream_flags._1_1_ & 8 */
            err = os_audio_enable_callback(dev, cur_ds());   /* 0x5547f (EAX=dev, DX=ds) */
            if (err != 0) { GP32(0x91d10) = err; return 0; } /* 0x5548a/0x5548f early return */
            GP16(0x91dc2) += 1;                     /* 0x55494 inc: callback armed */
        }
    }

    /* --- configure timer rate (0x554a5..0x554c3) — TIMER cluster: call_orig veneer --- */
    if (!(GP8(0x91d0d) & 8) && !(GP8(0x91d0e) & 4)) {   /* 0x554a5/0x554ae */
        os_audio_configure_timer_rate(0xff00, 0);       /* 0x554be (EAX=0xff00, EDX=0) */
        GP16(0x91dc2) += 2;                             /* 0x554c3 */
    }

    /* --- open driver voices + per-buffer timer (0x554cb..0x55588) --- */
    if (GP8(0x91dc8) != 0) {                        /* 0x554cb audio enabled */
        if (GP32(0x97ba4) != 8) {                   /* 0x554d8 */
            p2 += p2;                               /* 0x554e3 double param_2 */
            GP8(0x91df0) = 1;                       /* 0x554e5 */
        }
        if (GP32(0x97ba8) != 0)                     /* 0x554eb */
            p2 += p2;                               /* 0x554f4 double again */

        if (!(GP8(0x91d0d) & 8)) {                  /* 0x554f6 not streaming: open the voices */
            *(uint32_t *)(R + 0x00) = p2;           /* 0x5550d R+0  = param_2 (open_voices reqsz) */
            *(uint32_t *)(R + 0x0c) = 1;            /* 0x5551c R+0xc = 1 */
            *(uint32_t *)(R + 0x10) = ebx;          /* 0x55516 R+0x10 = param_3 (0x5622) */
            *(uint32_t *)(R + 0x44) = 0;            /* 0x55520 R+0x44 = 0 (far-args alloc branch) */
            err = os_audio_open_voices(GP32(0x97be4), CANON(0x97b70),
                                       (uint32_t)(uintptr_t)R, CANON(0x97cdc), cur_ds()); /* 0x55539 */
        } else {                                    /* 0x55542 streaming: handle from [0x91d08] */
            GP32(0x97cdc) = GP32(0x91d08);          /* 0x55547 */
        }

        if (err == 0 && !(GP8(0x91d0d) & 8)) {      /* 0x5554c/0x55550 (opened && not streaming) */
            GP16(0x91dc2) += 4;                     /* 0x55576 (si=[0x91dc2]+4 stored back) */
            /* open_voices wrote the poll-cb far-ptr back into R+0x1c/0x20; register it. */
            uint32_t cb_off = *(uint32_t *)(R + 0x1c);   /* 0x5555e */
            uint16_t cb_sel = *(uint16_t *)(R + 0x20);   /* 0x55559 */
            err = os_audio_timer_register_event(0x3c, cb_off, cb_sel, CANON(0x97cd8), cur_ds()); /* 0x5557d */
            if (err == 0)
                GP16(0x91dc2) += 8;                 /* 0x55588 */
        }
    }

    /* --- master decode timer (0x55590..0x555fd) --- */
    GP32(0x97cd0) = *(uint32_t *)(R + 0x00);        /* 0x55593 _DAT_97cd0 = R+0 */
    if (err == 0) {                                 /* 0x55598 */
        if (GP8(0x91dc8) != 0) {                    /* 0x5559c audio enabled: the tick ISR */
            err = os_audio_timer_register_event(0x23, CANON(0x4e2ed), cur_cs(),
                                                CANON(0x97cd4), cur_ds());  /* 0x555b7 gdv_tick_timer_isr */
            GP8(0x97b6c) = 1;                       /* 0x555be */
            GP32(0x9187c) = 0x118;                  /* 0x555c5 */
        } else {                                    /* 0x555d1 the no-audio decode ISR */
            err = os_audio_timer_register_event(p4, CANON(0x4e24b), cur_cs(),
                                                CANON(0x97cd4), cur_ds());  /* 0x555e2 gdv_decode_timer_isr_noaudio */
            GP32(0x9187c) = p4 * 8;                 /* 0x555f4 */
        }
        if (err == 0)
            GP16(0x91dc2) += 0x10;                  /* 0x555fd */
    }
    if (err != 0)
        GP32(0x91d10) = err;                        /* 0x55609 */
    return 0;                                       /* client discards; reads [0x91d10] */
}

/* ---- GDV begin_playback + its sole-caller tail start_voice (retirement) ---------
 * 0x4e066 gdv_audio_begin_playback — RETIRED to image-free host-C. Supersedes the earlier STOP:
 * that STOP's blocker was "the `call [0x91898]` is the ADPCM decoder = out of a driver-seam wave's
 * scope" + "the tail jmp gdv_audio_start_voice (0x55670) has no native." Both are resolved here:
 *
 *  (1) [0x91898] is a LIVE prime/decode hook holding EXACTLY three game-code values (verified:
 *      `tools/roth_disasm.py refs 0x91898 write` = 3 writers, all code we control):
 *        - CANON(0x4e45f): a bare `ret` (the trailing ret of gdv_audio_stream_callback) — a NO-OP;
 *          EBX (the src ptr) passes through unchanged. Installed at 0x4ba8c (lift_gdv_cutscene.c:895).
 *        - CANON(0x4e460): the 16-bit-output DPCM mix decoder (dual accumulators [0x9189c]/[0x918a0],
 *          dest [0x91d4c]; returns EBX=[0x91d4c]). Installed at 0x4bab7 (lift_gdv_cutscene.c:901).
 *        - CANON(0x4e519): the 8-bit unsigned-PCM decoder (0x80 bias; same dual accumulators + dest;
 *          returns EBX=[0x91d4c]). Installed at 0x4e4f3 (os_audio_gdv_init_silence, this file).
 *      NOTE the brief's model was imprecise: 0x4e519 is NOT a mid-entry into decode_dpcm_block (0x4e4cd,
 *      which ends at 0x4e4f0) and 0x4e45f/0x4e460 are NOT stream-callback mid-entries — they are three
 *      standalone routines. All three are PURE GAME-CODE memory ops (no MAGIC page, no driver far-call,
 *      no DPMI): count=[0x91d28], step table int32[256]@0x918a4, accumulators [0x9189c]/[0x918a0],
 *      output [0x91d4c]. So they re-lift faithfully in C (the shim reads GP32(0x91898) LIVE and
 *      dispatches). The still-original stream-callback ISR (0x4e394) keeps calling the ORIGINAL decoder
 *      bytes through the same [0x91898]; because this native reproduces the accumulator evolution
 *      byte-faithfully, the shared [0x9189c]/[0x918a0] state stays consistent across both paths.
 *
 *  (2) the tail `jmp gdv_audio_start_voice` (0x55670): its ONLY caller is this 0x4e088 jmp (verified:
 *      `roth_disasm.py callsto 0x55670` = 1 site), so 0x55670 is the sole-caller CONTINUATION of
 *      0x4e066 and is transcribed inline below. Its ONLY sub-call is sos_voice_start (0x4a641), reached
 *      here through the os_audio_voice_start DISPATCHER (the KEY COMPOSITION RULE) so
 *      the per-VA au_ab_va precedence keeps working: under AB_VA=0x4e066 voice_start is the veneer,
 *      under =native it is the retired 0x4a641 native — both resolve the CANON(0x97c5c)+cur_ds()
 *      descriptor far-ptr identically (dpmi_sel_base(host DS)=0). There is NO c2 dispatcher for 0x55670
 *      itself; the strict reading is STOP, but the sole-caller-tail fact + the
 *      composable sub-call make the inline transcription faithful (not invention) — flagged for the
 *      reviewer. STAGED ONLY (NOT in au_va_retired_default): a ROTH_LIFT=gdv paired run
 *      gates any default flip. */

/* [0x91898]==CANON(0x4e460): 16-bit mix decoder. src=EBX in; consumes count=[0x91d28] src bytes into
 * ([0x91d28]>>1) 16-bit sample PAIRS at [0x91d4c] via two independent accumulators; the do-while runs at
 * least once (dec/jg quirk). Disasm 0x4e460..0x4e4cc. Returns the decoded-buffer ptr [0x91d4c]. */
static uint32_t gdv_decode16(uint32_t src)
{
    const uint8_t *S = (const uint8_t *)(uintptr_t)src;   /* 0x4e466 mov esi,ebx */
    uint32_t count = GP32(0x91d28);                       /* 0x4e46a mov ecx,[0x91d28] */
    uint32_t a = GP32(0x9189c);                           /* 0x4e470 */
    uint32_t b = GP32(0x918a0);                           /* 0x4e475 */
    if (S[count + 4] & 0x40) { a = 0; b = 0; }            /* 0x4e47b test byte[esi+ecx+4],0x40 -> reset */
    int32_t  c = (int32_t)(count >> 1);                   /* 0x4e486 shr ecx,1 */
    int16_t *D = (int16_t *)(uintptr_t)GP32(0x91d4c);     /* 0x4e488 mov edi,[0x91d4c] */
    const uint8_t *p = S;
    do {
        a += GP32(0x918a4 + (uint32_t)p[0] * 4);          /* 0x4e48e..0x4e497 add eax,[bl*4+step] */
        D[0] = (int16_t)a;                                /* 0x4e499 mov word[edi],ax */
        b += GP32(0x918a4 + (uint32_t)p[1] * 4);          /* 0x4e49c..0x4e4ac add edx,[bl*4+step] */
        D[1] = (int16_t)b;                                /* 0x4e4ae mov word[edi-2],dx (edi pre-+4) */
        p += 2; D += 2;                                   /* 0x4e49f add esi,2; 0x4e4a2 add edi,4 */
    } while (--c > 0);                                    /* 0x4e4b2 dec ecx; 0x4e4b3 jg (signed) */
    GP32(0x9189c) = a;                                    /* 0x4e4b5 */
    GP32(0x918a0) = b;                                    /* 0x4e4ba */
    return GP32(0x91d4c);                                 /* 0x4e4c6 mov ebx,[0x91d4c] */
}

/* [0x91898]==CANON(0x4e519): 8-bit unsigned-PCM decoder. Twin of gdv_decode16 but writes bytes =
 * (uint8)((acc>>8)+0x80); the two `add ah/dh,0x80` per sample cancel mod 256, so the saved accumulator
 * is the un-biased running value. Disasm 0x4e519..0x4e58f. Returns [0x91d4c]. */
static uint32_t gdv_decode8(uint32_t src)
{
    const uint8_t *S = (const uint8_t *)(uintptr_t)src;   /* 0x4e51f mov esi,ebx */
    uint32_t count = GP32(0x91d28);                       /* 0x4e523 */
    uint32_t a = GP32(0x9189c);                           /* 0x4e529 */
    uint32_t b = GP32(0x918a0);                           /* 0x4e52e */
    if (S[count + 4] & 0x40) { a = 0; b = 0; }            /* 0x4e534 */
    int32_t  c = (int32_t)(count >> 1);                   /* 0x4e53f shr ecx,1 */
    uint8_t *D = (uint8_t *)(uintptr_t)GP32(0x91d4c);     /* 0x4e541 mov edi,[0x91d4c] */
    const uint8_t *p = S;
    do {
        a += GP32(0x918a4 + (uint32_t)p[0] * 4);          /* 0x4e547..0x4e550 */
        D[0] = (uint8_t)((a >> 8) + 0x80);                /* 0x4e552 add ah,0x80; 0x4e555 mov[edi],ah */
        b += GP32(0x918a4 + (uint32_t)p[1] * 4);          /* 0x4e55a..0x4e56a */
        D[1] = (uint8_t)((b >> 8) + 0x80);                /* 0x4e56c add dh,0x80; 0x4e56f mov[edi-1],dh (edi pre-+2) */
        p += 2; D += 2;                                   /* 0x4e55d add esi,2; 0x4e560 add edi,2 */
    } while (--c > 0);                                    /* 0x4e575 dec ecx; 0x4e576 jg */
    GP32(0x9189c) = a;                                    /* 0x4e578 */
    GP32(0x918a0) = b;                                    /* 0x4e57d */
    return GP32(0x91d4c);                                 /* 0x4e589 mov ebx,[0x91d4c] */
}

/* 0x55670 gdv_audio_start_voice (inlined tail): build the SOS streaming-voice descriptor 0x97c5c..
 * honoring the 8/16-bit + stereo flags in g_gdv_audio_format [0x91dca], call sos_voice_start via the
 * c2 dispatcher, record the handle [0x97ce0] and set g_gdv_audio_init_flags [0x91dc2]|=0x40. Store-by-
 * store from disasm 0x55670..0x55766. p1=[0x91d30] (EAX), p2=handler out (EDX), p3=[0x91cb0] (EBX). */
static void gdv_start_voice(uint32_t p1, uint32_t p2, uint32_t p3)
{
    uint16_t ds = cur_ds(), cs = cur_cs();
    uint32_t ecx = p1;                             /* 0x55674 mov ecx,eax (rate/count; doubled below) */
    uint32_t esi = 2;                              /* 0x55676 voice type (3 if stereo) */
    uint32_t flags = 0;                            /* 0x556b3 xor eax,eax -> [0x97c84] */
    uint8_t  fmt = GP8(0x91dca);                   /* 0x5569c mov dl,[0x91dca] */

    GP16(0x97c7c) = cs;                            /* 0x5568a mov [0x97c7c],cs */
    GP16(0x97c60) = ds;                            /* 0x055690 mov [0x97c60],ds */
    GP32(0x97c5c) = p2;                            /* 0x055696 buffer offset far-ptr */
    GP32(0x97c78) = CANON(0x4e394);                /* 0x0556a2 completion callback = gdv_audio_stream_callback */
    GP32(0x97c70) = 0x7fff;                        /* 0x0556a8 default rate */
    GP32(0x97c74) = 0xdead;                        /* 0x0556ae sentinel */

    if (fmt & 4) {                                 /* 0x556b5 test dl,4 */
        if (GP32(0x97ba4) > 8) {                   /* 0x556c1 ja 0x556e7 */
            ecx += ecx;                            /* 0x556e7 add ecx,ecx */
        } else {
            uint32_t edi  = (uint32_t)(fmt & 8);   /* 0x556c3 and dl,8; 0x556c6 movzx edi,dl (0 or 8) */
            uint32_t seta = (p3 > 0x6ff0) ? 1u : 0u; /* 0x556cf seta dl; 0x556d2 and edx,0xff (0 or 1) */
            if ((edi & seta) != 0) {               /* 0x556d8 test edi,edx: {0,8}&{0,1}==0 ALWAYS -> DEAD arm */
                GP8(0x91df2) = 1;                  /* 0x556dc (unreachable per the mask; transcribed faithfully) */
            } else {
                flags |= 0x80;                     /* 0x556e5 or al,0x80 */
                ecx += ecx;                        /* 0x556e7 add ecx,ecx */
            }
        }
    } else {                                       /* 0x556eb */
        if (GP32(0x97ba4) > 8)                     /* 0x556f2 jbe 0x556f9 (skip if <=8) */
            flags = 0x20;                          /* 0x556f4 mov eax,0x20 */
    }

    if (GP8(0x91dca) & 2) {                        /* 0x556f9 test byte[0x91dca],2 (stereo) */
        esi = 3;                                   /* 0x55702 mov esi,3 */
        ecx += ecx;                                /* 0x5570d add ecx,ecx */
        if (GP32(0x97ba8) == 0)                    /* 0x5570f test ebp,ebp; 0x55711 jne skip */
            flags |= 0x10;                         /* 0x55713 or al,0x10 */
    }
    if (p3 != 0 && p3 < 0x6ff0) {                  /* 0x55715 test ebx,ebx; 0x5571f jae skip */
        flags |= 0x100;                            /* 0x55721 or ah,1 */
        GP32(0x97c70) = p3;                        /* 0x55724 rate override */
    }
    GP32(0x97c84) = flags;                         /* 0x5572f */
    GP32(0x97c64) = ecx;                           /* 0x55734 */
    GP32(0x97c6c) = esi;                           /* 0x55741 voice type */

    uint32_t handle = GP32(0x97cdc);               /* 0x5573a mov eax,[0x97cdc] */
    /* 0x55747 call 0x4a641: voice_start(EAX=handle, CX=ds, EBX=&descriptor). EDX (format concat) is
     * push-saved+unread by voice_start, so it is not part of the ABI. Composed via the dispatcher. */
    uint32_t ret = os_audio_voice_start(handle, CANON(0x97c5c), ds);
    GP32(0x97ce0) = ret;                           /* 0x55756 mov [0x97ce0],eax */
    GP16(0x91dc2) += 0x40;                         /* 0x5575b (dx=[0x91dc2]+0x40 stored back) */
}

uint32_t os_audio_gdv_begin_playback_native(void)
{
    if (GP8(0x91dc8) == 0)                          /* 0x4e066 cmp byte[0x91dc8],0; 0x4e06d je 0x4e08d */
        return 0;
    uint32_t src     = GP32(0x91d50);               /* 0x4e06f mov ebx,[0x91d50] (stream base = decode src) */
    uint32_t handler = GP32(0x91898);               /* 0x4e075 call [0x91898] — read the LIVE hook */
    uint32_t p2;                                    /* the handler's EBX out -> start_voice's buffer far-ptr */
    if (handler == CANON(0x4e45f))                  /* bare `ret`: no decode, EBX (src) unchanged */
        p2 = src;
    else if (handler == CANON(0x4e460))             /* 16-bit mix decoder */
        p2 = gdv_decode16(src);
    else if (handler == CANON(0x4e519))             /* 8-bit unsigned-PCM decoder */
        p2 = gdv_decode8(src);
    else {                                          /* PROVABLY unreachable: [0x91898] has only the 3
                                                     * static writers above. Loud + safest (no-op). */
        fprintf(stderr, "[c2] gdv_begin_playback: UNKNOWN [0x91898]=%#x "
                        "(expected CANON of 0x4e45f/0x4e460/0x4e519) — no-op fallback\n", handler);
        p2 = src;
    }
    /* 0x4e07b..0x4e088: edx=p2 (handler out), ebx=[0x91cb0], eax=[0x91d30]; jmp gdv_audio_start_voice. */
    gdv_start_voice(GP32(0x91d30), p2, GP32(0x91cb0));
    return 0;
}

/* ---- SOS driver-slot dispatch (retirement: driver_dispatch_simple) ----
 * 0x45d28 sos_driver_dispatch_simple — RETIRED to image-free host-C. Disasm: the SOS far-calls the
 * driver-slot vtable [slot*0x48+0x92f9c] with the message
 * far-pointer (param:sel) pushed, then hardwires `return 0` (0x45d5b). The trace's 9 boot calls
 * (slot 0; cmd 0x0b then 8x cmd 0x8a with the message ptr stepping) ALL returned 0 with an EMPTY
 * game-memory write-set. alloc_driver_slot populated slot 0 (MIDI card 0xa004) with a vtable whose
 * entry offset is MAGIC_MIDI+0 (captured live: [0x92f9c]=0xe0d40000), so the far-call lands in
 * audio_trap's MAGIC_MIDI case = haudio_midi_send on the message — exactly what haudio_dispatch_simple
 * reproduces (byte-identical extraction; audio_trap keeps delegating). `cmd` is a dead
 * arg here: the SOS pushes it but the MAGIC_MIDI handler never reads it. No moot hardware to drop.
 *
 * STAGED: the bridge (audio_c2_bridge.c) reaches this only under ROTH_AU_AB=native; the call_orig
 * veneer stays the default path until the SFX re-test flips the default. */
uint32_t os_audio_driver_dispatch_simple_native(uint32_t slot, uint32_t cmd,
                                                uint32_t param, uint16_t sel)
{
    (void)cmd;                              /* pushed by the SOS but unread by the MIDI handler */
    return haudio_dispatch_simple(slot, param, sel);
}

/* ---- SOS driver-slot install / teardown (retirement) ---------------------------------------
 * 0x44553 sos_alloc_driver_slot / 0x44a81 sos_free_driver_slot — claim/release a slot in the 5-slot SOS
 * driver pool and install/tear down its MIDI driver. Image-free host-C over haudio_driver_install_service
 * (the host half: alloc-once driver descriptor + direct MAGIC vtable stamp) + haudio_dispatch_method (the
 * live-vtable method-1/2 driver-init/close far-calls). Store-by-store from disasm 0x44553..0x44a7e /
 * 0x44a81..0x44ba5 / 0x5189d..0x51967 with canon VAs inline.
 *
 * THE OBSERVED PATH is the 0xa004 music card -> LAB_000447bf first-boot arm (every r1 trace). In the
 * veneer that arm runs 0x51681 (loads HMIMDRV.386 + creates a DPMI descriptor -> the run-to-run
 * NON-deterministic [0x920dc] handle) then 0x4fad9/0x541ad (far-call the loaded driver + copy its 12
 * method far-ptrs into [0x92f9c+slot*0x48], which all fault to MAGIC under the host -> the vtable becomes
 * MAGIC_MIDI+N*4, sel 0x23). The service reproduces that observable end-state image-free: a STABLE host
 * descriptor selector for [0x920dc] + a direct MAGIC vtable stamp. Every
 * host-determined value 0x51681 leaves ([0x920dc]/[0x92080]/[0x9206c] + the two far-ptrs) is read ONLY by
 * DPMI real-mode plumbing (0x4fbd2 unlock / 0x4fc9d free / 0x4fad9 thunk — host no-ops), so a stable
 * service value is byte-immaterial and deterministic. STAGED: reached only under ROTH_AU_AB=native /
 * AB_VA=0x44553,0x44a81; veneer stays default until the flip. */

/* GS-flat (base-0) deref of an already-runtime pointer: the info/drv/slot_out far-ptrs run under a flat
 * gs (base 0), so gs:[ptr+off] == *(ptr+off); the lift passes them as GADDR(canon) runtime addresses. */
static inline uint32_t gsf_r32(uint32_t p) { return *(volatile uint32_t *)(uintptr_t)p; }
static inline uint16_t gsf_r16(uint32_t p) { return *(volatile uint16_t *)(uintptr_t)p; }
static inline void     gsf_w32(uint32_t p, uint32_t v) { *(volatile uint32_t *)(uintptr_t)p = v; }
static inline void     gsf_w16(uint32_t p, uint16_t v) { *(volatile uint16_t *)(uintptr_t)p = v; }

uint32_t os_audio_alloc_driver_slot_native(uint32_t device, uint32_t drv_ptr, uint32_t info_ptr,
                                           uint32_t slot_out, uint16_t sel)
{
    /* slot scan 0x44592..0x445ad: first pool slot with type_ids==0 (else 6 -> error 0xb at 0x445b7). */
    uint32_t slot = 6;
    for (uint32_t i = 0; i < 5; i++)
        if (GP32(0x920f0 + i * 4) == 0) { slot = i; break; }
    if (slot == 6)
        return 0xb;                                              /* 0x445b7 no free slot */

    GP32(0x920f0 + slot * 4) = device;                           /* 0x445cc type_ids[slot]=device */
    for (uint32_t i = 0; i < 0x10; i++)                          /* 0x445de..0x44602 chan-busy config copy */
        GP8(0x736b8 + slot * 0x10 + i) = GP8(device * 0x10 - 0x2c9f8 + i); /* 0x445f6/0x445fc (static tbl) */

    uint32_t type = GP32(0x920f0 + slot * 4);                    /* 0x4460a local_38 */

    /* --- static-vtable arms a003/a005/a007 (UNOBSERVED: only a004/OPL/a001/a00a reach LAB_000447bf) ---
     * The descriptor helpers 0x5042d/0x50521/0x536bc are pure far-ptr constants (return the static
     * template addr into DEAD locals — verified: no game-memory writes/calls), so DROPPED. The template
     * copy IS image-free (install_sos_driver_vtables filled 0x93198/0x93c10/0x970d4 with relocated code
     * offsets). method-1 here dispatches into REAL driver code in the veneer; UNOBSERVED and not-host-backed
     * under the native (haudio_dispatch_method no-ops on the non-MAGIC entry). */
    if (type == 0xa003 || type == 0xa005 || type == 0xa007) {
        uint32_t tmpl = type == 0xa003 ? 0x93198 : type == 0xa005 ? 0x93c10 : 0x970d4;
        for (uint32_t i = 0; i < 0xc; i++) {                     /* 0x4463a..0x44662 12 {off32,sel16} */
            GP16(0x92fa0 + slot * 0x48 + i * 6) = GP16(tmpl + 4 + i * 6);
            GP32(0x92f9c + slot * 0x48 + i * 6) = GP32(tmpl + i * 6);
        }
        uint32_t r = haudio_dispatch_method(slot, 1, gsf_r32(info_ptr + 0x14),
                                            gsf_r16(info_ptr + 0x18)); /* 0x44683 method-1 init */
        if (r != 0) return r;                                    /* 0x4468f error propagate */
    } else {
        /* --- LAB_000447bf (0x447bf): the a004/OPL/a001/a00a driver-load arm --- */
        struct haudio_driver_desc dd;
        if (haudio_driver_install_service(slot, device, &dd) != 0) /* (a) desc alloc-once + (b) MAGIC stamp */
            return 0xf;                                          /* mirrors 0x51681 crt_open-fail return */
        GP16(0x951d8) = 0;                                       /* 0x447bf */
        GP32(0x951d4) = 0;                                       /* 0x447c8 */
        if (gsf_r32(info_ptr + 4) == 0 && gsf_r16(info_ptr + 8) == 0) {
            /* FIRST BOOT (0x44857): 0x51681's descriptor + far-ptr stores. */
            GP32(0x920dc + slot * 4) = dd.dpmi_handle;           /* 0x51681 *param_7 -> [0x920dc] */
            GP8 (0x92080 + slot * 4) = dd.chan_byte;             /* 0x51681 *param_8 -> [0x92080] */
            GP32(0x9206c + slot * 4) = dd.lock_handle;           /* 0x51681 *param_9 -> [0x9206c] */
            GP32(0x920c8 + slot * 4) = device;                   /* 0x51681 [0x920c8] (dead: 0 readers) */
            GP32(0x920b4 + slot * 4) = 1;                        /* 0x51681 g_sos_driver_active */
            gsf_w16(info_ptr + 8,    dd.fptrA_sel);              /* 0x448c0 gs:[info+8] */
            gsf_w32(info_ptr + 4,    dd.fptrA_off);              /* 0x448cc gs:[info+4] */
            gsf_w16(info_ptr + 0x10, dd.fptrB_sel);              /* 0x448d7 gs:[info+0x10] */
            gsf_w32(info_ptr + 0xc,  dd.fptrB_off);              /* 0x448e3 gs:[info+0xc] */
            GP16(0x92050 + slot * 6) = dd.fptrA_sel;             /* 0x448ee */
            GP32(0x9204c + slot * 6) = dd.fptrA_off;             /* 0x448f8 */
            GP16(0x92098 + slot * 6) = dd.fptrB_sel;             /* 0x44905 */
            GP32(0x92094 + slot * 6) = dd.fptrB_off;             /* 0x4490f */
        } else {
            /* REUSE (0x447f4): a prior install left the info descriptor populated — copy its far-ptrs into
             * the slot tables (no 0x51681, no new descriptor); the vtable was re-stamped by the service. */
            uint16_t a_sel = gsf_r16(info_ptr + 8);    uint32_t a_off = gsf_r32(info_ptr + 4);
            uint16_t b_sel = gsf_r16(info_ptr + 0x10); uint32_t b_off = gsf_r32(info_ptr + 0xc);
            GP16(0x92050 + slot * 6) = a_sel;                    /* 0x4482b */
            GP32(0x9204c + slot * 6) = a_off;                    /* 0x44835 */
            GP16(0x92098 + slot * 6) = b_sel;                    /* 0x44842 */
            GP32(0x92094 + slot * 6) = b_off;                    /* 0x4484c */
        }
        /* common continuation 0x44915: 0x4fad9 (DPMI real-mode dispatch) + 0x541ad (driver far-call +
         * vtable copy) are MOOT / service-done -> dropped. */
        GP32(0x920b4 + slot * 4) = 1;                            /* 0x44960 active=1 */
        gsf_w32(slot_out, slot);                                 /* 0x4496a..0x44972 gs:[slot_out]=slot */
        uint32_t r = haudio_dispatch_method(slot, 1, gsf_r32(drv_ptr + 8), sel); /* 0x44974..0x44996 method-1 */
        if (r != 0) return r;                                    /* 0x4499f error propagate */
    }

    /* common tail 0x449ef (all arms converge). The a00a external-player timer arm (0x44a00) is UNOBSERVED
     * for a004 and depends on the 0x541f3 driver far-call (not host-backed) — its register routes through
     * the os_audio_timer_register_event dispatcher (timer cluster retires LAST); the cb far-ptr is a
     * documented UNOBSERVED placeholder (never reached by the observed a004 card). */
    if (GP32(0x920f0 + slot * 4) == 0xa00a)                      /* 0x44a4b */
        os_audio_timer_register_event(0x78, 0 /*UNOBSERVED cb_off*/, cur_cs(),
                                      (uint32_t)CANON(0x95200), sel); /* 0x44a42 sos_timer_register_event */
    GP32(0x920b4 + slot * 4) = 1;                                /* 0x44a5c active=1 */
    gsf_w32(slot_out, slot);                                     /* 0x44a6d gs:[slot_out]=slot */
    return 0;                                                    /* 0x44a70 local_20=0 */
}

/* 0x44a81 sos_free_driver_slot (EAX=slot, EDX=flag) — release the slot. Early-out EAX=1 if inactive; else
 * dispatch driver method 2 (close), run the synth cleanup 0x5189d for non-{a003,a005,a007} cards, chain
 * the a00a timer-remove, then clear the slot tables. 0x5189d's ONLY game-memory write is [0x920b4]=0 — its
 * dpmi_unlock/free calls (0x4fbd2/0x4fc9d) are host no-ops (they read the moot descriptor handles). */
uint32_t os_audio_free_driver_slot_native(uint32_t slot, uint32_t flag)
{
    if (GP32(0x920b4 + slot * 4) == 0)                           /* 0x44a94 inactive early-out */
        return 1;
    haudio_dispatch_method(slot, 2, GP32(0x951d4), GP16(0x951d8)); /* 0x44aca method-2 close ({0,0} post-alloc) */
    if (flag != 0) {                                             /* 0x44ad3 */
        uint32_t type = GP32(0x920f0 + slot * 4);                /* 0x44adf */
        if (type != 0xa003 && type != 0xa005 && type != 0xa007) {
            /* reproduce 0x5189d: slot<6 && active!=0 -> active=0; DPMI unlock/free moot. */
            if (slot < 6 && GP32(0x920b4 + slot * 4) != 0)       /* 0x518ae/0x518c6 */
                GP32(0x920b4 + slot * 4) = 0;                    /* 0x518e1 */
        }
    }
    if (GP32(0x920f0 + slot * 4) == 0xa00a)                      /* 0x44b3d UNOBSERVED for a004 */
        os_audio_timer_remove_event(GP32(0x95200));             /* 0x44b34 sos_timer_remove_event */
    GP16(0x92050 + slot * 6) = 0;                                /* 0x44b4c */
    GP32(0x9204c + slot * 6) = 0;                                /* 0x44b55 */
    GP16(0x92098 + slot * 6) = 0;                                /* 0x44b63 */
    GP32(0x92094 + slot * 6) = 0;                                /* 0x44b6c */
    GP32(0x920f0 + slot * 4) = 0;                                /* 0x44b7c type_ids[slot]=0 */
    GP32(0x920b4 + slot * 4) = 0;                                /* 0x44b8c active[slot]=0 */
    return 0;                                                    /* 0x44b96 */
}

/* ---- MIDI-router cluster (retirement) ---------------------------------------------------
 * 0x44e0d sos_dispatch_midi_event (the ~400-instr router) + 0x4594d all_notes_off / 0x45dc5
 * restore_volumes / 0x45f1d mute_volumes. Each far-calls the per-device SOS MIDI driver vtable
 * method 0 = `call far [dev*0x48 + 0x92f9c]` (the HMI SOS vtable, alloc_driver_slot-populated). Under
 * the host that entry is MAGIC_MIDI+0, so the far-call lands in audio_trap's MAGIC_MIDI case =
 * haudio_midi_send on the staged message -> the SoundFont synth ring. A native C body cannot far-call
 * a MAGIC page, so — exactly the driver_dispatch_simple precedent — each vtable method-0 far-call
 * becomes haudio_dispatch_simple(dev, msg_flat, sel): it reads the LIVE vtable entry for `dev` and
 * routes to haudio_midi_send only when it is a MAGIC_MIDI handler (else no game-memory effect / 0).
 *
 * TWO ARMS on g_sos_voice_alloc_mode [0x951cc] (r1: ALL 2,425 dispatch calls branch=1 = alloc-mode==0,
 * so the direct-passthrough arm is the ONLY one exercised in-game; the alloc!=0 dynamic virtual-channel
 * allocation arm is transcribed 1:1 for image-free completeness but is UNOBSERVED and cannot be A/B
 * validated in-session). The device-remap in this arm is the one the lifted emit_audio_sequence_event
 * (lift_audio.c:1436) also gates on [0x951cc]; both are inert while [0x951cc]==0 (the in-game state),
 * so there is no live double-remap. The remap/channel-state TABLES are fixed game globals (absolute
 * `mov ..,[abs]` in the disasm) -> GP8/GP16/GP32; only the message byte stream + the sequence
 * descriptor note-map (`lgs`/`mov gs,dx`) are far. Transcribed store-by-store from disasm 0x44e0d..
 * 0x4594a / 0x4594d.. / 0x45dc5.. / 0x45f1d..
 *
 * STAGED ONLY: native under ROTH_AU_AB=native (or AB_VA=<va>); NOT in au_va_retired_default. The
 * paired A/B (music-playing + settings-volume + area-transition) gates any default flip. */

/* Far read/write of the parsed-MIDI stream / sequence descriptor: linear = dpmi_sel_base(sel)+off
 * (the haudio_midi_send / voice_start_native translation). Selector base 0 (GDT flat) for the fixed
 * 0x951b4/0x951c0 work buffers. */
static inline uint8_t  au_r8 (uint16_t sel, uint32_t off)
{ return *(const volatile uint8_t  *)(uintptr_t)(dpmi_sel_base(sel) + off); }
static inline uint32_t au_r32(uint16_t sel, uint32_t off)
{ return *(const volatile uint32_t *)(uintptr_t)(dpmi_sel_base(sel) + off); }
static inline void     au_w8 (uint16_t sel, uint32_t off, uint8_t v)
{ *(volatile uint8_t *)(uintptr_t)(dpmi_sel_base(sel) + off) = v; }

/* One SOS MIDI driver vtable method-0 far-call = one synth-ring push of (msg_flat:sel). */
static void midi_vtable_send(uint32_t dev, uint32_t msg_flat, uint16_t sel)
{ (void)haudio_dispatch_simple(dev, msg_flat, sel); }

/* 0x44e0d sos_dispatch_midi_event core (also invoked internally by restore/mute). `msg` is the
 * runtime-flat pointer to the parsed message, `sel` its selector (base 0). Returns the driver EAX:
 * 1 for the alloc-mode==0 arm; 0 (or -1 = no physical channel) for the alloc!=0 arm. The 3-byte
 * message length the SOS pushes to the driver is dropped — the host MIDI handler reads a fixed 4
 * bytes and ignores it. */
static uint32_t midi_router_core(uint32_t song, uint32_t dev, uint32_t msg, uint16_t sel)
{
    uint8_t status = au_r8(sel, msg);                        /* 0x44e38 */
    uint8_t chan   = status & 0x0fu;                         /* 0x44e41 */

    if (GP32(0x951cc) == 0) {                                /* 0x44e46 je -> arm 0 (OBSERVED) */
        uint32_t command = status & 0xf0u;                   /* 0x44e5a */
        if (command == 0xb0u && au_r8(sel, msg + 1) == 7) {  /* 0x44f70 0xBn + 0x44f26 CC7 */
            GP8(0x951b4) = status;                            /* 0x44e7e */
            GP8(0x951b5) = 7;                                 /* 0x44e83 */
            uint32_t raw = au_r8(sel, msg + 2);               /* 0x44e94 */
            uint32_t l38 = (raw * GP32(0x7370c + song * 4)) >> 7;              /* 0x44e99/0x44ea0 */
            GP8(0x951b6) = (uint8_t)(((uint32_t)GP8(0x73708) * l38) >> 7);     /* 0x44eb4 */
            GP8(0x7372c + dev * 0x10 + chan) = (uint8_t)raw;  /* 0x44ecd raw-volume table */
            if (GP32(0x93124 + song * 4) != 0) GP8(0x951b6) = 0;              /* 0x44ee2 muted */
            midi_vtable_send(dev, CANON(0x951b4), sel);       /* 0x44f48 (0x951b4 variant) */
        } else if (command == 0xb0u) {                        /* 0x44f30 other controller: 4-byte pass */
            GP8(0x951b4) = au_r8(sel, msg + 0);               /* 0x44ef2 */
            GP8(0x951b5) = au_r8(sel, msg + 1);               /* 0x44eff */
            GP8(0x951b6) = au_r8(sel, msg + 2);               /* 0x44f0c */
            GP8(0x951b7) = au_r8(sel, msg + 3);               /* 0x44f19 */
            midi_vtable_send(dev, CANON(0x951b4), sel);       /* 0x44f48 */
        } else {                                              /* 0x44f53 non-0xBn: pass INPUT as-is */
            midi_vtable_send(dev, msg, sel);                  /* 0x44f65 (INPUT variant) */
        }
        return 1;                                             /* 0x44f7f */
    }

    /* ---- arm !=0 : dynamic virtual-channel allocation (UNOBSERVED; 1:1 faithful) ---- */
    uint16_t d_sel = GP16(0x93168 + song * 6);      /* sequence descriptor far ptr; note-map at +0x40 */
    uint32_t d_off = GP32(0x93164 + song * 6);
    uint8_t  age_max = 0;                            /* 0x44e25 highest-age (evict) accumulator */
    uint8_t  best    = 0xff;                         /* 0x44e29 best evict candidate */
    uint32_t modval  = 0xffffffffu;                  /* 0x44e2d raw-volume tracker for the final send */
    uint8_t  phys = GP8(0x729d8 + dev * 0x80 + song * 0x10 + chan);   /* 0x44f8b logical->physical */

router_head:                                          /* 0x44fa8 */
    if (phys != 0xff) {                               /* je -> allocate */
        au_w8(sel, msg + 0, (uint8_t)(phys | (status & 0xf0u)));      /* 0x44fbd remapped status */
        goto router_send;                             /* 0x44fc0 -> 0x456c3 */
    }
    if (chan == 9) {                                  /* 0x44fcc drums: fixed physical 9 */
        GP8(0x729d8 + dev * 0x80 + song * 0x10 + chan) = 9;           /* 0x44fe2 */
        phys = 9;                                     /* 0x44fe9 */
        goto router_head;
    }
    /* first scan (0x44fef): first busy physical channel whose reverse-map is free -> claim it */
    for (uint32_t i = 0; i < 0x10u; i++) {            /* 0x44ffb */
        while (GP8(0x736b8 + dev * 0x10 + i) == 0 && i < 0x10u) i++;  /* 0x45005 busy-skip */
        if (i < 0x10u && GP8(0x72ca8 + dev * 0x10 + i) == 0xff) {     /* 0x45024/0x4502e free slot */
            GP8(0x729d8 + dev * 0x80 + song * 0x10 + chan) = (uint8_t)i;   /* 0x4505b log->phys */
            phys = (uint8_t)i;                                             /* 0x45064 */
            GP8(0x72ca8 + dev * 0x10 + i) = chan;                          /* 0x45073 phys->log */
            GP8(0x72cf8 + dev * 0x10 + i) = (uint8_t)song;                 /* 0x45085 phys device */
            GP8(0x72c58 + dev * 0x10 + i) = au_r8(d_sel, d_off + chan * 4 + 0x40); /* 0x450b4 age/prio */
            uint32_t patch = GP8(0x73388 + dev * 0x80 + song * 0x10 + chan);   /* 0x450ce */
            if (patch == 0xff) {                       /* 0x450d8 no saved state: claim a slot, no replay */
                for (patch = 0; patch <= 3; patch++)   /* 0x4532c */
                    if (GP8(0x72d48 + patch * 5 + dev * 0x140 + chan * 0x14) == 0xff) {
                        GP8(0x72d48 + patch * 5 + dev * 0x140 + chan * 0x14) = 1;
                        GP8(0x73388 + dev * 0x80 + song * 0x10 + chan) = (uint8_t)patch;
                        break;
                    }
                goto router_head;
            }
            /* replay the saved controller state to the fresh voice (0x450e5) */
            uint32_t rec = patch * 5 + dev * 0x140 + chan * 0x14;
            GP8(0x7372c + dev * 0x10 + (i & 0xff)) = 0x7f;                 /* 0x450f1 */
            GP8(0x951b4) = (uint8_t)(phys | 0xb0u); GP8(0x951b5) = 0x79; GP8(0x951b6) = 0;  /* reset ctrl */
            midi_vtable_send(dev, CANON(0x951b4), sel);                   /* 0x4512d */
            if (GP8(0x72d4b + rec) != 0xff) {          /* 0x4514e program (Cn) */
                GP8(0x951b4) = (uint8_t)(phys | 0xc0u); GP8(0x951b5) = GP8(0x72d4b + rec);
                midi_vtable_send(dev, CANON(0x951b4), sel);               /* 0x451a1 */
            }
            if (GP8(0x72d49 + rec) != 0xff) {          /* 0x451c2 pitch (En) */
                GP8(0x951b4) = (uint8_t)(phys | 0xe0u); GP8(0x951b5) = 0; GP8(0x951b6) = GP8(0x72d49 + rec);
                midi_vtable_send(dev, CANON(0x951b4), sel);               /* 0x4521c */
            }
            if (GP8(0x72d4a + rec) != 0xff) {          /* 0x4523d CC7 volume */
                GP8(0x951b4) = (uint8_t)(phys | 0xb0u); GP8(0x951b5) = 7; GP8(0x951b6) = GP8(0x72d4a + rec);
                midi_vtable_send(dev, CANON(0x951b4), sel);               /* 0x45297 */
            }
            if (GP8(0x72d4c + rec) != 0xff) {          /* 0x452b8 pan (CC0x40) */
                GP8(0x951b4) = (uint8_t)(phys | 0xb0u); GP8(0x951b5) = 0x40; GP8(0x951b6) = GP8(0x72d4c + rec);
                midi_vtable_send(dev, CANON(0x951b4), sel);               /* 0x45312 */
            }
            goto router_head;                          /* 0x4531b -> 0x44fa8 */
        }
    }
    /* second scan (0x4539d): no free channel; pick the highest-age busy channel to evict */
    for (uint32_t i = 0; i < 0x10u; i++) {             /* 0x453a9 */
        while (GP8(0x736b8 + dev * 0x10 + i) == 0 && i < 0x10u) i++;  /* 0x453b3 busy-skip */
        if (i < 0x10u) {                               /* 0x453d2 */
            uint8_t a = GP8(0x72c58 + dev * 0x10 + i);
            if (a > age_max && a != 0xff) { age_max = a; best = (uint8_t)i; }  /* 0x453e7/0x45400 */
        }
    }
    if (best == 0xff) goto router_send;                /* 0x4541a nothing to evict (phys stays 0xff) */
    if (age_max <= au_r32(d_sel, d_off + chan * 4 + 0x40)) {  /* 0x45444 new prio not higher */
        if (GP8(0x73388 + dev * 0x80 + song * 0x10 + chan) != 0xff) goto router_send;  /* 0x4562f */
        for (uint32_t patch = 0; patch <= 3; patch++)  /* 0x4565c claim a saved-state slot, then send */
            if (GP8(0x72d48 + patch * 5 + dev * 0x140 + chan * 0x14) == 0xff) {
                GP8(0x72d48 + patch * 5 + dev * 0x140 + chan * 0x14) = 1;
                GP8(0x73388 + dev * 0x80 + song * 0x10 + chan) = (uint8_t)patch;
                break;
            }
        goto router_send;
    }
    /* evict `best`: steal it from its old owner, remap, all-notes-off + reset (0x4544e) */
    GP8(0x729d8 + dev * 0x80 + song * 0x10 + chan) = best;                /* 0x45465 */
    GP8(0x729d8 + dev * 0x80 + GP8(0x72cf8 + dev * 0x10 + best) * 0x10
                 + GP8(0x72ca8 + dev * 0x10 + best)) = 0xff;              /* 0x4549e clear old owner */
    GP8(0x72ca8 + dev * 0x10 + best) = chan;                             /* 0x454b4 */
    GP8(0x72cf8 + dev * 0x10 + best) = (uint8_t)song;                    /* 0x454c9 */
    phys = best;                                                        /* 0x454d2 */
    GP8(0x72c58 + dev * 0x10 + best) = au_r8(d_sel, d_off + chan * 4 + 0x40);  /* 0x45501 */
    GP8(0x7372c + dev * 0x10 + phys) = 0x7f;                            /* 0x45513 */
    GP8(0x951b4) = (uint8_t)(phys | 0xb0u); GP8(0x951b5) = 0x7b; GP8(0x951b6) = 0; /* all notes off */
    midi_vtable_send(dev, CANON(0x951b4), sel);                         /* 0x4554f */
    GP8(0x951b4) = (uint8_t)(phys | 0xb0u); GP8(0x951b5) = 0x79; GP8(0x951b6) = 0; /* reset ctrl */
    midi_vtable_send(dev, CANON(0x951b4), sel);                         /* 0x4558d */
    if (GP8(0x73388 + dev * 0x80 + song * 0x10 + chan) == 0xff)          /* 0x455aa */
        for (uint32_t patch = 0; patch < 4; patch++)                    /* 0x455b7 */
            if (GP8(0x72d48 + patch * 5 + dev * 0x140 + chan * 0x14) == 0xff) {
                GP8(0x72d48 + patch * 5 + dev * 0x140 + chan * 0x14) = 1;
                GP8(0x73388 + dev * 0x80 + song * 0x10 + chan) = (uint8_t)patch;
                break;
            }
    goto router_head;                                  /* 0x4562a -> 0x44fa8 */

router_send:                                           /* 0x456c3 emit the actual event */
    if (chan == 9) {                                   /* 0x456ca drum channel special */
        if (status == 0xb9 && au_r8(sel, msg + 1) == 7) {                /* 0x4586a */
            modval = au_r8(sel, msg + 2);                                /* 0x45885 */
            GP8(0x73735 + dev * 0x10) = (uint8_t)modval;                 /* 0x45896 drum volume */
        }
    } else {
        uint32_t command = status & 0xf0u;             /* 0x456d3 */
        if (command == 0xb0u) {                        /* 0x45858 controller */
            uint8_t cc = au_r8(sel, msg + 1);          /* 0x456e1 */
            uint32_t rec = (uint32_t)GP8(0x73388 + dev * 0x80 + song * 0x10 + chan) * 5
                           + dev * 0x140 + chan * 0x14;
            if (cc == 7) {                             /* 0x4579c CC7 volume */
                GP8(0x72d4a + rec) = au_r8(sel, msg + 2);                /* 0x45725 save */
                modval = au_r8(sel, msg + 2);                            /* 0x4572f */
                GP8(0x7372c + dev * 0x10 + phys) = (uint8_t)modval;      /* 0x45746 */
            } else if (cc == 0x40) {                   /* 0x457a6 pan */
                GP8(0x72d4c + rec) = au_r8(sel, msg + 2);                /* 0x45786 save */
            }
        } else if (command == 0xc0u) {                 /* 0x45846 program */
            uint32_t rec = (uint32_t)GP8(0x73388 + dev * 0x80 + song * 0x10 + chan) * 5
                           + dev * 0x140 + chan * 0x14;
            GP8(0x72d4b + rec) = au_r8(sel, msg + 1);                    /* 0x457ed save */
        } else if (command == 0xe0u) {                 /* 0x45850 pitch */
            uint32_t rec = (uint32_t)GP8(0x73388 + dev * 0x80 + song * 0x10 + chan) * 5
                           + dev * 0x140 + chan * 0x14;
            GP8(0x72d49 + rec) = au_r8(sel, msg + 2);                    /* 0x45830 save */
        }
    }
    if (phys == 0xff) return 0xffffffffu;              /* 0x4589c no physical channel -> -1 */
    if (modval != 0xffffffffu) {                       /* 0x458a6 scale the volume byte for the driver */
        if (GP32(0x93124 + song * 4) == 0) {           /* 0x458b2 */
            uint32_t l38 = (modval * GP32(0x7370c + song * 4)) >> 7;     /* 0x458cf */
            au_w8(sel, msg + 2, (uint8_t)(((uint32_t)GP8(0x73708) * l38) >> 7));  /* 0x458f2 */
        } else {
            au_w8(sel, msg + 2, 0);                    /* 0x458bf muted */
        }
    }
    midi_vtable_send(dev, msg, sel);                   /* 0x45908 INPUT variant (physical channel) */
    au_w8(sel, msg + 0, (uint8_t)(chan | (status & 0xf0u)));            /* 0x45920 restore logical status */
    if (modval != 0xffffffffu) au_w8(sel, msg + 2, (uint8_t)modval);    /* 0x45930 restore raw volume */
    return 0;                                          /* 0x45934 */
}

/* 0x44e0d — the sole client (emit_audio_sequence_event) passes a 3-byte controller message. */
uint32_t os_audio_midi_dispatch_native(uint32_t seq, uint32_t dev, uint32_t msg, uint16_t sel)
{
    return midi_router_core(seq, dev, msg, sel);
}

/* 0x4594d midi_all_notes_off_channels — per live track (1..[0x92f7c+song*4]) emit CC 0x7b/0x79 +
 * pitch-center + CC7 via the SOS driver, clearing the channel-map tables in the alloc!=0 arm. The
 * vtable index is the per-track device from the channel map (not a single arg). Disasm 0x4594d..;
 * work buffer 0x951b4, base-0 selector. Returns EAX=1 (0x45d17). */
uint32_t os_audio_midi_all_notes_off_native(uint32_t song)
{
    for (uint32_t i = 1; i < GP32(0x92f7c + song * 4); i++) {            /* 0x45967 */
        uint16_t cm_sel = GP16(0x92f30 + song * 6);
        uint32_t cm_off = GP32(0x92f2c + song * 6);
        uint32_t dev = au_r32(cm_sel, cm_off + i * 4);                   /* 0x4599b */
        if (dev == 0xffffffffu || dev == 0xff) continue;                /* 0x459a1/0x459a7 */
        uint32_t trk = song * 0xc0 + i * 6;
        uint16_t nm_sel = GP16(0x94bb4 + trk + 4);
        uint32_t nm_off = GP32(0x94bb4 + trk);
        uint8_t  mchan  = au_r8(nm_sel, nm_off + 8);                     /* 0x459c9 */
        if (GP32(0x951cc) == 0) {                       /* 0x459d0 arm 0 (OBSERVED) */
            GP8(0x951b4)=(uint8_t)(mchan|0xb0u); GP8(0x951b5)=0x7b; GP8(0x951b6)=0;   /* all notes off */
            midi_vtable_send(dev, CANON(0x951b4), 0);                    /* 0x45a12 */
            GP8(0x951b4)=(uint8_t)(mchan|0xb0u); GP8(0x951b5)=0x79; GP8(0x951b6)=0;   /* reset ctrl */
            midi_vtable_send(dev, CANON(0x951b4), 0);                    /* 0x45a50 */
            GP8(0x951b4)=(uint8_t)(mchan|0xe0u); GP8(0x951b5)=0x40; GP8(0x951b6)=0x40; /* pitch center */
            midi_vtable_send(dev, CANON(0x951b4), 0);                    /* 0x45a8e */
            GP8(0x951b4)=(uint8_t)(mchan|0xb0u); GP8(0x951b5)=7; GP8(0x951b6)=0;      /* CC7 */
            midi_vtable_send(dev, CANON(0x951b4), 0);                    /* 0x45acc */
        } else {                                        /* arm !=0 teardown (UNOBSERVED) */
            uint8_t phys  = GP8(0x729d8 + dev * 0x80 + song * 0x10 + mchan);
            GP8(0x729d8 + dev * 0x80 + song * 0x10 + mchan) = 0xff;
            uint8_t patch = GP8(0x73388 + dev * 0x80 + song * 0x10 + mchan);
            GP8(0x72ca8 + dev * 0x10 + phys) = 0xff;
            GP8(0x72cf8 + dev * 0x10 + phys) = 0xff;
            GP8(0x951b4)=(uint8_t)(phys|0xb0u); GP8(0x951b5)=0x7b; GP8(0x951b6)=0;
            midi_vtable_send(dev, CANON(0x951b4), 0);
            GP8(0x951b4)=(uint8_t)(phys|0xb0u); GP8(0x951b5)=0x79; GP8(0x951b6)=0;
            midi_vtable_send(dev, CANON(0x951b4), 0);
            GP8(0x951b4)=(uint8_t)(phys|0xe0u); GP8(0x951b5)=0x40; GP8(0x951b6)=0x40;
            midi_vtable_send(dev, CANON(0x951b4), 0);
            GP8(0x951b4)=(uint8_t)(phys|0xb0u); GP8(0x951b5)=7; GP8(0x951b6)=0;
            midi_vtable_send(dev, CANON(0x951b4), 0);
            if (patch != 0xff) {
                uint32_t rec = patch * 5 + dev * 0x140 + mchan * 0x14;
                GP8(0x72d4b + rec) = 0xff;
                GP8(0x72d49 + rec) = 0xff;
                GP8(0x72d4a + rec) = 0xff;
                GP8(0x72d4c + rec) = 0xff;
                GP8(0x72d48 + rec) = 0xff;
                GP8(0x73388 + dev * 0x80 + song * 0x10 + mchan) = 0xff;
            }
        }
    }
    return 1;
}

/* 0x45dc5 midi_restore_channel_volumes (site EDX=1 -> the loop always runs): [0x93104]=1, [0x93124]=1,
 * then per live voice re-send its stored raw CC7 volume through the router and mark [0x970f4]=1.
 * Disasm 0x45dc5..; returns EAX=0. */
uint32_t os_audio_midi_restore_volumes_native(uint32_t song)
{
    GP32(0x93104 + song * 4) = 1;                                       /* 0x45dde */
    GP32(0x93124 + song * 4) = 1;                                       /* 0x45df8 (EDX=1 baked) */
    for (uint32_t v = 0; v < 0x20u; v++) {                              /* 0x45e0e */
        uint32_t trk = song * 0xc0 + v * 6;
        if (GP32(0x9212c + trk) == 0 && GP16(0x92130 + trk) == 0) continue;   /* 0x45e25/0x45e2e */
        uint16_t nm_sel = GP16(0x94bb4 + trk + 4);
        uint32_t nm_off = GP32(0x94bb4 + trk);
        uint8_t  mchan  = au_r8(nm_sel, nm_off + 8);                    /* 0x45e50 */
        uint16_t cm_sel = GP16(0x92f30 + song * 6);
        uint32_t cm_off = GP32(0x92f2c + song * 6);
        uint32_t dev    = au_r32(cm_sel, cm_off + v * 4);               /* 0x45e73 */
        uint8_t  phys   = mchan;                                        /* 0x45ea1 */
        if (GP32(0x951cc) != 0)                                        /* 0x45e79 */
            phys = GP8(0x729d8 + dev * 0x80 + song * 0x10 + mchan);     /* 0x45e96 */
        GP8(0x951c0) = (uint8_t)(mchan | 0xb0u);                        /* 0x45eac */
        GP8(0x951c1) = 7;                                              /* 0x45eb1 */
        GP8(0x951c2) = GP8(0x7372c + dev * 0x10 + phys);               /* 0x45eca raw volume */
        midi_router_core(song, dev, CANON(0x951c0), 0);                /* 0x45eea call 0x44e0d */
        GP32(0x970f4 + dev * 0x40 + phys * 4) = 1;                      /* 0x45efe */
    }
    return 0;
}

/* 0x45f1d midi_mute_channel_volumes — twin of restore, gated on the CURRENT [0x93124] value: clears
 * [0x93104]/[0x93124], re-sends CC7 (the router zeroes it since [0x93124]==0 now) and marks [0x970f4]=0.
 * Disasm 0x45f1d..; returns EAX=0. */
uint32_t os_audio_midi_mute_volumes_native(uint32_t song)
{
    GP32(0x93104 + song * 4) = 0;                                       /* 0x45f34 */
    if (GP32(0x93124 + song * 4) != 0) {                               /* 0x45f44 gate on current */
        GP32(0x93124 + song * 4) = 0;                                  /* 0x45f57 */
        for (uint32_t v = 0; v < 0x20u; v++) {                         /* 0x45f6d */
            uint32_t trk = song * 0xc0 + v * 6;
            if (GP32(0x9212c + trk) == 0 && GP16(0x92130 + trk) == 0) continue;  /* 0x45f84/0x45f8d */
            uint16_t cm_sel = GP16(0x92f30 + song * 6);
            uint32_t cm_off = GP32(0x92f2c + song * 6);
            uint32_t dev    = au_r32(cm_sel, cm_off + v * 4);          /* 0x45fb7 */
            uint16_t nm_sel = GP16(0x94bb4 + trk + 4);
            uint32_t nm_off = GP32(0x94bb4 + trk);
            uint8_t  mchan  = au_r8(nm_sel, nm_off + 8);               /* 0x45fd1 */
            uint8_t  phys   = mchan;                                   /* 0x46000 */
            if (GP32(0x951cc) != 0)                                    /* 0x45fd8 */
                phys = GP8(0x729d8 + dev * 0x80 + song * 0x10 + mchan);/* 0x45ff5 */
            GP8(0x951c0) = (uint8_t)(mchan | 0xb0u);                   /* 0x4600b */
            GP8(0x951c1) = 7;                                         /* 0x46010 */
            GP8(0x951c2) = GP8(0x7372c + dev * 0x10 + phys);          /* 0x46023 */
            midi_router_core(song, dev, CANON(0x951c0), 0);           /* 0x46049 */
            GP32(0x970f4 + dev * 0x40 + phys * 4) = 0;                 /* 0x4605d */
        }
    }
    return 0;
}

/* ---- SOS detection driver trio (retirement) ---------------------------------------------
 * 0x48b21 load_detection_driver / 0x48f79 find_driver_for_device / 0x48c6b unload_detection_driver —
 * the boot-time card-detection sub-chain (lift_audio.c sos_audio_init). The originals load
 * DIGI/HMIDET.386 via int21 DOS file I/O, alloc a DPMI real-mode buffer, and run the SOS "dispatch-
 * computer" (0x4fcd3) to build a driver-dispatch far pointer — but under the M4 virtual driver that
 * int3 (audio.c HMI_DISPATCH_COMPUTER) hands back MAGIC_OFF and the real .386 is NEVER executed, so
 * the file contents + all DPMI/PIT/int21 plumbing are MOOT: no game-memory footprint the client reads,
 * exactly the moot-hardware class drops (the DPMI-real-mode-buffer precedent). What the client
 * (lift_audio.c:2153-2162) actually observes:
 *   - load returns 0 (success gate `st==0`); find returns 0 (SB16 found, gate `st!=0`); unload's
 *     return is DISCARDED.
 *   - find_driver stages the 0x6a-byte SB16 driver descriptor into its `out` buffer (0x7f4e4): the
 *     fn=2 detect far-call (0x490d6->0x4fece, EAX=2) lands at MAGIC_OFF -> haudio_detect_card stages
 *     SB16_DESC in the far-args seg, then the SOS 0x4fece rep-movs 0x6a to `out` + patches the 4 fnptr
 *     selectors to DS. haudio_stage_driver_descriptor reproduces that byte-exact result image-free.
 * The detection driver's private state block (0x97aec loaded-flag / 0x97b00 fd / 0x97af0/4/8/c buffer +
 * dispatch far-ptrs / 0x97b04/08/0c/10/14/18) is transient SOS bookkeeping the client never reads and
 * the trio frees at unload; only the re-entrancy flag 0x97aec is reproduced (load guard / unload clear)
 * so a repeat load still early-outs. The rest is dropped as moot, documented. The r1 trace
 * (1 call each, boot, ret=0, EMPTY snapshot diffs) confirms none of the
 * fixed-canon windows (0x97b7c/0x9740c/0x97420/0x97800/0x97440) are touched — the state block lives at
 * 0x97aec..0x97b18, outside them.
 *
 * STAGED (pending class): the bridge runs these native ONLY under ROTH_AU_AB=native; the
 * call_orig veneer stays the default until the paired-run A/B + in-game sign-off. */

/* 0x48b21: guard on the loaded-flag, else mark loaded, return 0. Disasm 0x48b35 (cmp [0x97aec],0; jne
 * -> ret 3 already-loaded), 0x48c51 (mov [0x97aec],1), 0x48c5b (ret 0). The intermediate 0xf (open
 * fail) / 5 (load fail) returns require a real file/DPMI failure that cannot occur under virtualization
 * (no file opened, no DPMI buffer allocated), so they are unreachable here. */
uint32_t os_audio_load_detection_driver_native(uint32_t path, uint16_t sel)
{
    (void)path; (void)sel;                          /* HMIDET.386 dir + sel: only the moot file I/O reads them */
    if (GP32(0x97aec) != 0)                          /* 0x48b35: driver already loaded */
        return 3;
    GP32(0x97aec) = 1;                               /* 0x48c51: mark loaded */
    return 0;                                        /* 0x48c5b */
}

/* 0x48f79: range-check the device, stage the SB16 descriptor for the modeled card (0xe018), return 0.
 * Disasm: 0x48f96/0x48f9c (out==0 && sel==0 -> ret 2, null output far-ptr), 0x48faf/0x48fb8 (dev in
 * [0xe000,0xe200] else ret 6), the record loop 0x49007.. (matches the device against HMIDET.386's
 * records), the fn=2 far-call 0x490d6 (stages the descriptor), 0x490f7 (ret 0) / 0x49100 (ret 7 not
 * found). The host models exactly the SB16 (id 0xe018 = SB16_DESC+0x64), so the loop "finds" it for
 * dev==0xe018 and reports not-found (7) for any other in-range id the host has no record for. Config
 * always sets SoundCard=0xe018 (the trace's a0), so the ret-0 arm is what runs. */
uint32_t os_audio_find_driver_for_device_native(uint32_t dev, uint32_t out, uint16_t sel)
{
    if (out == 0 && sel == 0)                        /* 0x48f96/0x48f9c: null output far-ptr */
        return 2;
    if (dev < 0xe000u || dev > 0xe200u)              /* 0x48faf/0x48fb8: device out of the digital range */
        return 6;
    if (dev != 0xe018u)                              /* the record loop finds only the host-modeled SB16 */
        return 7;                                    /* 0x49100: not found */
    haudio_stage_driver_descriptor(out, sel);        /* fn=2 detect: SB16 desc -> `out` + selector patch */
    return 0;                                        /* 0x490f7 */
}

/* 0x48c6b: clear the loaded-flag, return 0. Disasm 0x48c79 (mov [0x97aec],0), 0x48ce5 (ret 0). The fd
 * close (0x48c88->0x551e1) + DPMI unlock/free (0x4fbd2, 0x4fc9d x2) are moot (no real fd/buffers here);
 * the 5 (unlock fail) return is unreachable. The client discards this return. */
uint32_t os_audio_unload_detection_driver_native(void)
{
    GP32(0x97aec) = 0;                               /* 0x48c79 */
    return 0;                                        /* 0x48ce5 */
}

/* ---- SOS digital voice start / load-to-slot (retirement) ---------------------------------
 * 0x4a641 sos_voice_start and 0x4ad03 sos_voice_load_to_slot are PURE GAME CODE — the full disasm
 * (tools/roth_disasm.py func <va>) has ZERO call/int instructions. Each far-copies a fixed set of
 * fields from the voice-control-block (vcb) into a per-handle voice struct in the fn-0xa far-args
 * segment (the 0x97440 far-ptr table -> g_farg_base+VOICE_OFF+slot*VOICE_SZ). There is no host
 * service to route to; allocation/population IS this copy. Transcribed 1:1 from the disasm; unknown
 * field semantics are copied blindly (byte-faithful).
 *
 * Far-pointer translation mirrors haudio_midi_send: linear = dpmi_sel_base(selector) + offset.
 *   - voice struct i: offset = [0x97440 + handle*0xc0 + i*6] (dword), sel = [.. +4] (word). Equals
 *     g_farg_base + VOICE_OFF + i*VOICE_SZ (audio.c's own convention; see haudio_voice_struct_base).
 *   - vcb: offset = the `voice`/`queued` arg (EBX), sel = the `ds` arg (CX) -> the obj DS base.
 * The branch guard [handle*4 + 0x9740c] >= 0xe106 selects the unobserved jae arm (disasm 0x4a675/
 * 0x4ad39); under the M4 driver it is never taken (branch==0 for all 90 traced voice_start calls),
 * but it is transcribed faithfully anyway — 3 dead vcb reads then return 0 (no observable effect).
 * handle is used at full width (disasm `mov eax,[ebp-4]`), unlike au_branch's cosmetic 0xff mask.
 *
 * STAGED behind ROTH_AU_AB=native (audio_c2_bridge.c); the call_orig veneer stays the default until
 * the in-game A/B over the new voice-struct snapshot window signs off. */

/* voice_start populate: the 34 stores of the jb arm, in disasm order (0x4a6e0..0x4ac29). S = voice
 * struct linear, V = vcb linear. Every offset/width verified against the disasm store list. */
static void vs_fill(uint8_t *S, const uint8_t *V)
{
#define VW(o)   (*(const uint16_t *)(V + (o)))
#define VD(o)   (*(const uint32_t *)(V + (o)))
#define SW(o,v) (*(uint16_t *)(S + (o)) = (uint16_t)(v))
#define SD(o,v) (*(uint32_t *)(S + (o)) = (uint32_t)(v))
    SD(0x00, VD(0x00));                      /* 0x4a719 */
    SW(0x04, VW(0x04));                      /* 0x4a711 */
    SD(0x08, VD(0x00));                      /* 0x4a755 */
    SW(0x0c, VW(0x04));                      /* 0x4a74d */
    SD(0x10, VD(0x30) + VD(0x00));           /* 0x4a78e (add ebx,ecx) */
    SW(0x14, VW(0x04));                      /* 0x4a789 */
    if (V[0x28] & 0x40) {                    /* 0x4a796 test byte vcb+0x28,0x40 */
        SD(0x18, VD(0x30));                  /* 0x4a7ca */
        SD(0x1c, VD(0x30));                  /* 0x4a7f7 */
        SD(0x20, VD(0x34));                  /* 0x4a824 */
        SD(0x2c, VD(0x2c) - (VD(0x30) + VD(0x34))); /* 0x4a856 (sub ebx,eax) */
    } else {
        SD(0x18, VD(0x08));                  /* 0x4a885 */
        SD(0x1c, VD(0x08));                  /* 0x4a8b2 */
    }
    SW(0x32, VW(0x14));                      /* 0x4a8e0 */
    SW(0x34, VW(0x18));                      /* 0x4a90f */
    SW(0x30, VW(0x28) | 0xa000u);            /* 0x4a934 (or dh,0xa0) */
    SW(0x36, VW(0x10));                      /* 0x4a963 */
    SW(0x40, VW(0x20));                      /* 0x4a99a */
    SD(0x3c, VD(0x1c));                      /* 0x4a9a2 */
    SW(0x38, VW(0x0c));                      /* 0x4a9d0 */
    SW(0x4a, VW(0x24));                      /* 0x4a9ff */
    SW(0x3a, 0);                             /* 0x4aa18 */
    SD(0x44, VD(0x38));                      /* 0x4aa48 */
    SW(0x48, 0);                             /* 0x4aa60 */
    SD(0x4c, 0);                             /* 0x4aa7b */
    SD(0x50, VD(0x08));                      /* 0x4aaac */
    SW(0x54, VW(0x40));                      /* 0x4aada */
    SW(0x56, VW(0x44));                      /* 0x4ab09 */
    SW(0x58, VW(0x48));                      /* 0x4ab38 */
    SW(0x5a, VW(0x4c));                      /* 0x4ab67 */
    SW(0x5c, VW(0x50));                      /* 0x4ab96 */
    SW(0x5e, VW(0x54));                      /* 0x4abc5 */
    SW(0x60, VW(0x58));                      /* 0x4abf4 */
    SD(0x64, 0);                             /* 0x4ac0d */
    SW(0x68, 0);                             /* 0x4ac29 */
#undef VW
#undef VD
#undef SW
#undef SD
}

uint32_t os_audio_voice_start_native(uint32_t handle, uint32_t voice, uint16_t ds)
{
    if (GP32(0x9740c + handle * 4) >= 0xe106) {          /* 0x4a675/0x4a67f jae arm */
        const uint8_t *V = (const uint8_t *)(uintptr_t)(dpmi_sel_base(ds) + voice);
        (void)*(const volatile uint16_t *)(V + 4);       /* 0x4a685 dead reads (return 0) */
        (void)*(const volatile uint32_t *)(V + 0);       /* 0x4a692 */
        (void)*(const volatile uint32_t *)(V + 8);       /* 0x4a69c */
        return 0;                                        /* 0x4ac46 */
    }
    /* jb arm (0x4a6a8): scan 32 voice structs for the first free slot, populate it, return its index. */
    const uint8_t *V = (const uint8_t *)(uintptr_t)(dpmi_sel_base(ds) + voice);
    for (uint32_t i = 0; i < 0x20u; i++) {               /* 0x4a6b4 cmp i,0x20 */
        uint32_t tbl = 0x97440u + handle * 0xc0u + i * 6u;
        uint8_t *S = (uint8_t *)(uintptr_t)(dpmi_sel_base(GP16(tbl + 4)) + GP32(tbl)); /* 0x4a6cb lgs */
        if (*(uint16_t *)(S + 0x30) & 0x8000u)           /* 0x4a6d2 movsx + 0x4a6d7 test ah,0x80: in use */
            continue;                                    /* 0x4a6da jne -> next slot */
        vs_fill(S, V);
        return i;                                        /* 0x4ac30 retval = slot index */
    }
    return 0xffffffffu;                                  /* 0x4ac3d no free slot */
}

/* voice_load_to_slot populate: the 17 stores of the jb arm, in disasm order (0x4ada5..0x4b058). A
 * SUBSET of vs_fill into a DIRECT slot index (no scan); flag word uses |0x8800 (vs voice_start's
 * |0xa000). Untouched fields keep their prior value (partial update); the fn always returns 0. */
static void vl_fill(uint8_t *S, const uint8_t *V)
{
#define VW(o)   (*(const uint16_t *)(V + (o)))
#define VD(o)   (*(const uint32_t *)(V + (o)))
#define SW(o,v) (*(uint16_t *)(S + (o)) = (uint16_t)(v))
#define SD(o,v) (*(uint32_t *)(S + (o)) = (uint32_t)(v))
    SD(0x00, VD(0x00));                      /* 0x4ada5 */
    SW(0x04, VW(0x04));                      /* 0x4ad9d */
    SD(0x08, VD(0x00));                      /* 0x4ade1 */
    SW(0x0c, VW(0x04));                      /* 0x4add9 */
    SD(0x10, VD(0x00) + VD(0x30));           /* 0x4ae18 (add ebx,gs:[eax+0x30]) */
    SW(0x14, VW(0x04));                      /* 0x4ae13 */
    SD(0x50, VD(0x08));                      /* 0x4ae45 */
    if (V[0x28] & 0x40) {                    /* 0x4ae4d test byte vcb+0x28,0x40 */
        SD(0x18, VD(0x30));                  /* 0x4ae81 */
        SD(0x1c, VD(0x30));                  /* 0x4aeae */
        SD(0x20, VD(0x34));                  /* 0x4aedb */
        SD(0x2c, VD(0x2c) - (VD(0x30) + VD(0x34))); /* 0x4af0d (sub ebx,eax) */
    } else {
        SD(0x18, VD(0x08));                  /* 0x4af3c */
        SD(0x1c, VD(0x08));                  /* 0x4af69 */
    }
    SW(0x34, VW(0x18));                      /* 0x4af97 */
    SW(0x30, VW(0x28) | 0x8800u);            /* 0x4afbc (or bh,0x88) */
    SW(0x40, VW(0x20));                      /* 0x4aff3 */
    SD(0x3c, VD(0x1c));                      /* 0x4affb */
    SW(0x38, VW(0x0c));                      /* 0x4b029 */
    SW(0x4a, VW(0x24));                      /* 0x4b058 */
#undef VW
#undef VD
#undef SW
#undef SD
}

uint32_t os_audio_voice_load_to_slot_native(uint32_t handle, uint32_t edx,
                                            uint32_t queued, uint16_t ds)
{
    if (GP32(0x9740c + handle * 4) >= 0xe106) {          /* 0x4ad39/0x4ad43 jae arm */
        const uint8_t *V = (const uint8_t *)(uintptr_t)(dpmi_sel_base(ds) + queued);
        (void)*(const volatile uint16_t *)(V + 4);       /* 0x4ad49 dead reads (return 0) */
        (void)*(const volatile uint32_t *)(V + 0);       /* 0x4ad56 */
        (void)*(const volatile uint32_t *)(V + 8);       /* 0x4ad60 */
        return 0;                                        /* 0x4b066 */
    }
    /* jb arm (0x4ad6c): populate the DIRECT slot `edx` (no scan / no 0x20 bound — faithful). */
    uint32_t tbl = 0x97440u + handle * 0xc0u + edx * 6u;
    uint8_t *S = (uint8_t *)(uintptr_t)(dpmi_sel_base(GP16(tbl + 4)) + GP32(tbl)); /* 0x4ad79 */
    const uint8_t *V = (const uint8_t *)(uintptr_t)(dpmi_sel_base(ds) + queued);
    vl_fill(S, V);
    return 0;                                            /* 0x4b05d retval = 0 */
}

/* ---- SOS digital voice-field leaves (retirement) -----------------------------------------
 * 0x4a54a get_w34 / 0x49fe9 xchg_w32 / 0x4a28c xchg_w54 / 0x4ac55 deactivate — PURE GAME CODE (zero
 * call/int, reviewer-verified). Each is a straight-line far-ptr field op on the per-voice struct
 * addressed by `lgs edx,[bank*0xc0 + voice*6 + 0x97440]` — the SAME 0x97440 far-ptr table
 * voice_start's slot scan reads, populated at driver-open by open_voices (0x47dae; r1 boot write-set
 * seq 5 fills 0x9740c and 0x97440.. with a live LDT selector 0x005f), NOT a null selector. There is
 * no host service to route to; the leaves read/write real voice structs (r1: 5,756 calls, non-zero
 * chained returns — the "empty table / null-SOS #GP" premise is FALSE). Transcribed 1:1 from the
 * disasm; far-ptr translation mirrors voice_start_native / haudio_midi_send: linear =
 * dpmi_sel_base(sel) + offset, resolved from the table entry [0x97440 + bank*0xc0 + voice*6] (dword
 * offset) + [.. +4] (word selector).
 *
 * get_w34 + both xchg ops are RETIRED-DEFAULT (native default; the r1 chained-return coverage self-
 * evidences the RMW). deactivate is pending-class (native only under ROTH_AU_AB=native) pending the
 * one "voice stops" A/B action — the wiring lives in audio_c2_bridge.c's os_audio_voice_field. */

/* Resolve the voice-struct linear base for (bank,voice) from the 0x97440 far-ptr table entry — the
 * same lgs the leaves (and voice_start's scan) do: off = [tbl], sel = [tbl+4]. */
static uint8_t *voice_field_slot(uint32_t bank, uint32_t voice)
{
    uint32_t tbl = 0x97440u + bank * 0xc0u + voice * 6u;
    return (uint8_t *)(uintptr_t)(dpmi_sel_base(GP16(tbl + 4)) + GP32(tbl)); /* 0x..lgs */
}

/* 0x4a54a: pure read of word@0x34, movsx-extended into EAX. No guard, no write. Disasm 0x4a56a lgs,
 * 0x4a571 movsx word gs:[edx+0x34]. */
uint32_t os_audio_voice_get_w34_native(uint32_t bank, uint32_t voice)
{
    uint8_t *S = voice_field_slot(bank, voice);
    return (uint32_t)(int32_t)*(int16_t *)(S + 0x34);        /* 0x4a571 movsx */
}

/* 0x49fe9: if the active bit (word@0x30 & 0x8000) is set, swap word@0x32 and return the OLD value
 * (movsx-extended); else return 0, no write. Disasm 0x4a012 movsx word+0x30 / 0x4a017 test ah,0x80;
 * active arm 0x4a030 movsx old word+0x32, 0x4a04f store new word+0x32; else arm 0x4a056 ret 0. */
uint32_t os_audio_voice_xchg_w32_native(uint32_t bank, uint32_t voice, uint32_t val)
{
    uint8_t *S = voice_field_slot(bank, voice);
    if (!(*(uint16_t *)(S + 0x30) & 0x8000u))               /* 0x4a017 test ah,0x80: not active */
        return 0;                                           /* 0x4a056 */
    int32_t old = *(int16_t *)(S + 0x32);                   /* 0x4a030 movsx old */
    *(uint16_t *)(S + 0x32) = (uint16_t)val;                /* 0x4a04f store new */
    return (uint32_t)old;
}

/* 0x4a28c: identical shape to 0x49fe9 but field 0x54. Disasm 0x4a2b5 movsx word+0x30 / 0x4a2ba test
 * ah,0x80; active arm 0x4a2d3 movsx old word+0x54, 0x4a2f2 store new word+0x54; else 0x4a2f9 ret 0. */
uint32_t os_audio_voice_xchg_w54_native(uint32_t bank, uint32_t voice, uint32_t val)
{
    uint8_t *S = voice_field_slot(bank, voice);
    if (!(*(uint16_t *)(S + 0x30) & 0x8000u))               /* 0x4a2ba test ah,0x80: not active */
        return 0;                                           /* 0x4a2f9 */
    int32_t old = *(int16_t *)(S + 0x54);                   /* 0x4a2d3 movsx old */
    *(uint16_t *)(S + 0x54) = (uint16_t)val;                /* 0x4a2f2 store new */
    return (uint32_t)old;
}

/* 0x4ac55: range-guard voice<0x20 (else ret 0xa); then iff active (word@0x30 & 0x8000) AND NOT
 * protected (byte@0x31 & 0x10 clear): clear the active bit (byte@0x31 &= 0x7f) and zero word@0x34.
 * Always returns 0 (deactivated OR no-op); 0xa only on the range guard. Disasm 0x4ac68 cmp 0x20 /
 * 0x4ac6c jae -> 0xa; 0x4ac8b test ah,0x80; 0x4aca4 test byte+0x31,0x10; 0x4acc1 and byte+0x31,0x7f;
 * 0x4acda word+0x34=0. */
uint32_t os_audio_voice_deactivate_native(uint32_t bank, uint32_t voice)
{
    if (voice >= 0x20u)                                     /* 0x4ac68/0x4ac6c: range guard */
        return 0xa;                                         /* 0x4acf3 */
    uint8_t *S = voice_field_slot(bank, voice);
    if ((*(uint16_t *)(S + 0x30) & 0x8000u) &&              /* 0x4ac8b test ah,0x80: active */
        !(*(uint8_t *)(S + 0x31) & 0x10)) {                 /* 0x4aca4 test byte+0x31,0x10: not protected */
        *(uint8_t *)(S + 0x31) &= 0x7f;                     /* 0x4acc1: clear the active bit (0x8000) */
        *(uint16_t *)(S + 0x34) = 0;                        /* 0x4acda */
    }
    return 0;                                               /* 0x4ace1/0x4acea */
}

/* ---- SOS audio-callback enable / disable (retirement) ------------------------------------
 * 0x47cf5 sos_enable_audio_callback / 0x47d6e sos_disable_audio_callback — the driver-load core's
 * poll-callback install/remove. Both open with a DPMI memory-lock/unlock pass over four code ranges
 * (enable: 0x47769 -> int-31 AX=0x600 x4; disable: 0x47a2f -> int-31 AX=0x601 x4). Under the host
 * that lock/unlock is a NO-OP that leaves CF clear (recomp/host/dpmi.c case 0x0600/0x0601 just
 * `break`s -> success), so 0x4fb95/0x4fbd2 return 0 and the SOS wrappers return 0: the r1/r2 traces
 * confirm it (enable_callback 0x47cf5 ret=0 boot; disable_callback 0x47d6e ret=0 x2 teardown). The
 * lock is therefore MOOT (no game-memory footprint) and dropped — the moot-DPMI-plumbing class, the
 * same precedent as the detection trio. What the client-visible original does:
 *   enable:  [0x97b30] = 1 (the "callback armed" flag other SOS code reads); then, iff the service
 *            far-ptr (svc:sel) is non-null, strcpy the null-terminated service descriptor from
 *            (svc:sel) into [0x7420c] via the SOS byte-copy 0x531fb (else zero [0x7420c].b0). ret 0.
 *   disable: [0x97b30] = 0. ret 0.
 * [0x7420c] is consumed by the still-running SOS timer/service dispatch (0x544bf/0x5478b read it), so
 * faithfulness matters — but the native copies the SAME bytes from the SAME source through the SAME
 * algorithm, so it is byte-identical to the veneer by construction (like haudio_midi_send extraction).
 *
 * STAGED (pending class): the bridge runs these native ONLY under ROTH_AU_AB=native; the
 * call_orig veneer stays default until the paired-run A/B + in-game sign-off. */

/* 0x531fb: copy the null-terminated service descriptor from src (2-byte-unrolled strcpy; stops after
 * storing the first zero byte). Byte-faithful to the SOS routine; src is the client's own small,
 * null-terminated descriptor (as the original, unbounded — a valid pointer is the caller's contract). */
static void svc_strcpy(uint8_t *dst, const uint8_t *src)
{
    uint32_t i = 0;
    for (;;) {
        dst[i] = src[i];                 /* 0x5320c/0x53219 store */
        if (src[i] == 0)                  /* 0x5320f/0x53220 stop after the terminator */
            break;
        i++;
    }
}

/* 0x47cf5: DPMI-lock (moot) -> [0x97b30]=1 -> install-or-clear the service descriptor -> ret 0. The
 * 0x47769 lock-fail (ret 5) path is unreachable under the host (locks always succeed). Disasm:
 * 0x47d21 (mov [0x97b30],1); 0x47d2b/0x47d31 (if svc==0 && sel==0 -> clear); 0x47d51 (call 0x531fb);
 * 0x47d58 (mov byte [0x7420c],0); 0x47d5f (ret 0). */
uint32_t os_audio_enable_callback_native(uint32_t svc, uint16_t sel)
{
    GP32(0x97b30) = 1;                                    /* 0x47d21: callback armed */
    if (svc != 0 || sel != 0) {                          /* 0x47d2b/0x47d31: non-null service far-ptr */
        const uint8_t *src = (const uint8_t *)(uintptr_t)(dpmi_sel_base(sel) + svc);
        svc_strcpy((uint8_t *)(uintptr_t)CANON(0x7420c), src);   /* 0x47d51 -> 0x531fb strcpy */
    } else {
        GP8(0x7420c) = 0;                                /* 0x47d58: no service -> clear */
    }
    return 0;                                            /* 0x47d5f */
}

/* 0x47d6e: DPMI-unlock (moot) -> [0x97b30]=0 -> ret 0. The 0x47a2f unlock-fail (ret 0, no clear) path
 * is unreachable under the host (unlocks always succeed), so [0x97b30]=0 always runs. Disasm 0x47d93
 * (mov [0x97b30],0), 0x47d9d (ret 0). */
uint32_t os_audio_disable_callback_native(void)
{
    GP32(0x97b30) = 0;                                    /* 0x47d93 */
    return 0;                                            /* 0x47d9d */
}

/* ---- SOS driver open_voices (the audio imgfree linchpin) --------------------------
 * 0x47dae sos_driver_open_voices (~400 instr) — the digital driver-open core. It claims a driver
 * slot, allocates the DPMI DMA real-mode segment (0x54441 -> 0x4fc0f) and the far-args segment the
 * driver's fn-0xa uses (0x5473c), far-calls the dispatch-computer (0x4fcd3 -> {MAGIC_OFF, game CS})
 * and fn-0xa (0x4ff6f -> haudio_open_driver: cb=MAGIC_POLL, voices@VOICE_OFF, pos@POS_OFF), then
 * fans those runtime values into the per-slot bookkeeping + the 0x97440 far-ptr table that
 * voice_start / voice_load / the 4 voice-field leaves read. Both allocations + the far-calls are
 * host-determined at runtime, so the veneer STOPPED (audio_c2_bridge.c FALLBACK); this native
 * supplies that half image-free via the NEW haudio_open_voices_service (audio.c), which allocates
 * the same segments as real host-backed LDT selectors and runs the SAME fn-0xa core. The moot
 * hardware (DPMI real-mode DMA, PIT, the driver-control far-calls 0x501c0/0x4ffc2/0x50046/0x50020/
 * 0x4fff7 that hit the virtual driver's no-op default) has no game-memory footprint and is dropped,
 * exactly the moot-hardware class the detection trio / close_voices already drop.
 *
 * The observed (r1) path is SFX card 0xe018: alloc DMA branch (R+0x24==R+0x28==0), SB16 record match
 * (guard [h*4+0x9740c]=desc), flags!=0 streaming buffer-B, alloc-branch-B far-args (R+0x34==R+0x38==
 * R+0x44==0 -> 0x5473c), and the jb driver-open arm (desc 0xe018 < 0xe106). The unobserved arms (DMA
 * from-descriptor; jae >= 0xe106) are transcribed for completeness but not host-modeled. The r1
 * fixed-canon window write-set (0x9740c=e018, 0x97420/24={MAGIC_POLL,CS}, 0x97800/04={POS_OFF,farg},
 * 0x97440={VOICE_OFF+i*0x6c,farg}) is reproduced by construction.
 *
 * KEY (disasm-cited, corrects the veneer STOP note's model): the poll cb the still-original master-
 * timer ISR (0x49eaf) far-calls each tick is NOT registered by open_voices. open_voices only writes
 * the cb far-ptr {MAGIC_POLL, CS} BACK into the request descriptor (gs:[R+0x1c/0x20] = [0x7f4a8]);
 * the SEPARATE sos_timer_register_event (0x49923, still a call_orig veneer — the timer cluster
 * retires LAST) registers THAT into the ISR-consumed timer table (0x979c4). The driver-control
 * far-calls 0x501c0/0x4ffc2/0x50046/0x50020/0x4fff7 write NO game memory under the virtual driver
 * (default case R_EAX=0). So this native does not touch the timer table; it feeds the original
 * registration the correct cb far-ptr. Correctness of the ISR far-call reduces to: the cb offset =
 * MAGIC_POLL (host-intercepted by EIP) and the cb selector = a loadable code selector (the game CS)
 * — the service supplies both. This is the unprovable-without-in-game dependency: STAGED ONLY
 * (native under ROTH_AU_AB=native or AB_VA 0x47dae; NOT in au_va_retired_default), the
 * paired run gates any default flip. A poisoned native would break voice_start + the 4 leaves that
 * read the table this populates, so the guard is deliberate.
 *
 * Far-ptr note: req_ptr/size_ptr/handle_out arrive as RUNTIME flat addresses (the lifted site passes
 * GADDR(...)), so R/Q/HO deref directly (no CANON). The per-slot tables (0x97xxx) are canon globals
 * (GP32/GP16/GP8). The far-args selector the service returns has its base registered in dpmi.c's
 * cache (dpmi_note_sel_base), so the voice natives' software dpmi_sel_base(fargSel)+off resolves to
 * g_farg_base+off — the same linear range haudio_open_driver zeroed. */
uint32_t os_audio_open_voices_native(uint32_t desc, uint32_t req_ptr, uint32_t size_ptr,
                                     uint32_t handle_out, uint16_t sel)
{
    uint8_t *Rp = (uint8_t *)(uintptr_t)size_ptr;    /* R = the request/size descriptor (runtime flat) */
    uint8_t *Qp = (uint8_t *)(uintptr_t)req_ptr;     /* Q = the request block (runtime flat) */
    (void)sel;                                       /* far-ptr selectors come from the service/descriptor */
#define RD(o) (*(volatile uint32_t *)(Rp + (o)))
#define RW(o) (*(volatile uint16_t *)(Rp + (o)))
#define QD(o) (*(volatile uint32_t *)(Qp + (o)))

    /* slot scan (0x47e1d..0x47e5c): first free [0x97824+i*4], i<5; else ret 0xb. */
    uint32_t h = 6;                                  /* 0x47e1d [ebp-0x54]=6 */
    for (uint32_t i = 0; i < 5; i++) {               /* 0x47e30 cmp 5; jae */
        if (GP32(0x97824 + i * 4) == 0) {            /* 0x47e3c */
            h = i;                                    /* 0x47e48 */
            break;
        }
    }
    if (h == 6)                                      /* 0x47e4f cmp 6; jne */
        return 0xb;                                  /* 0x47e55: no free slot */
    GP32(0x97824 + h * 4) = desc;                    /* 0x47e6a: claim the slot */

    /* host driver-open service: allocate the far-args + moot DMA/streaming segments and run fn-0xa
     * staging (haudio_open_driver). One call folds the veneer's 0x54441 + 0x5473c + 0x4ff6f allocs;
     * the interleave is immaterial (no game code reads the allocated state between them). reqsz =
     * R+0 (the SOS request size). Failure mirrors 0x54441/0x5473c's error return. */
    struct haudio_open_desc od;
    uint32_t svc = haudio_open_voices_service(RD(0x00), &od);
    if (svc != 0)                                    /* 0x47f16 jne -> LAB_0004865c: the original
                                                      * returns 0x54441's code with the 0x97824 slot
                                                      * STILL CLAIMED (no release on this path) */
        return svc;

    /* DMA descriptor (0x47e70 branch). SFX: R+0x24==0 && R+0x28==0 -> the 0x54441 alloc branch. Its
     * DPMI real-mode DMA alloc is moot; its game-visible outputs are the DMA seg far-ptr, the DMA-
     * driver dispatch pair {MAGIC_OFF,CS}, the [h*4+0x9740c]=desc guard ([0x74578]==desc post-match)
     * and [h*4+0x97374]=1. The from-descriptor arm (unobserved) copies the pre-supplied far-ptrs. */
    if (RD(0x24) == 0 && RW(0x28) == 0) {            /* 0x47e70/0x47e7b: alloc branch (0x47eea) */
        RW(0x28) = od.dma_sel;  RD(0x24) = od.dma_off;        /* 0x47f53/0x47f5f */
        RW(0x30) = od.cb_sel;   RD(0x2c) = od.dispatch_off;   /* 0x47f6a/0x47f76: DMA dispatch {MAGIC_OFF,CS} */
        GP16(0x972a8 + h * 6) = od.dma_sel; GP32(0x972a4 + h * 6) = od.dma_off;        /* 0x47f81/0x47f8b */
        GP16(0x972c8 + h * 6) = od.cb_sel;  GP32(0x972c4 + h * 6) = od.dispatch_off;   /* 0x47f98/0x47fa2 */
        GP32(0x9740c + h * 4) = desc;                /* 0x5463e: guard = the matched device id */
        GP32(0x97374 + h * 4) = 1;                   /* 0x5464a */
        GP8(0x972e4 + h * 4)  = 1;                   /* 0x54441 DMA channel byte (moot) */
        GP32(0x9727c + h * 4) = od.dma_off;          /* 0x54441 DMA buffer offset (moot) */
    } else {                                         /* 0x47e87: descriptor pre-supplies the DMA seg */
        GP16(0x972a8 + h * 6) = RW(0x28); GP32(0x972a4 + h * 6) = RD(0x24);   /* 0x47ebe/0x47ec8 */
        GP16(0x972c8 + h * 6) = RW(0x30); GP32(0x972c4 + h * 6) = RD(0x2c);   /* 0x47ed5/0x47edf */
    }

    /* 0x54285 DMA-copy guard (0x47fa8): returns 1 (-> open_voices error) if BOTH halves of the slot
     * DMA buffer far-ptr [h*6+0x972a4 dword / +0x972a8 word] are null; else a moot far-call -> 0.
     * The alloc branch set it non-zero. On the ret-1 path the original returns with the 0x97824 slot
     * still claimed (no release). */
    if (GP32(0x972a4 + h * 6) == 0 && GP16(0x972a8 + h * 6) == 0) /* 0x54285: off==0 && sel==0 */
        return 1;

    uint32_t m28 = 0;                                /* [ebp-0x28]: driver-info / bufB seg (moot; jb arm) */

    if (desc < 0xe106u) {                            /* 0x47fdd/0x47fe7: jb -> the full driver-open */
        GP32(0x973b0 + h * 4) = RD(0x00);            /* 0x47ff6 */

        uint16_t buf_sel; uint32_t buf_off;          /* [ebp-0x4c]/[ebp-0x50] at the 0x48137 merge */
        if (RD(0x0c) != 0) {                         /* 0x47ffc flags: streaming buffer-B path */
            GP32(0x97b1c + h * 4) = 1;               /* 0x4800c */
            /* 0x4887e allocates the moot streaming decode buffer-B (DPMI DOS mem); its far-ptr
             * {bufb_off,bufb_sel} is non-null so the 0x48046 alloc-fail arm is not taken. The buffer
             * memset (0x48092..0x48113) fills a moot host buffer the mixer never reads — dropped. */
            buf_sel = od.bufb_sel; buf_off = od.bufb_off;
            m28 = od.bufb_off;                       /* [ebp-0x28] = bufB segment (moot) */
            GP32(0x973f8 + h * 4) = od.bufb_off;     /* 0x4887e EBX-out (moot) */
            GP16(0x973c8 + h * 6) = buf_sel; GP32(0x973c4 + h * 6) = buf_off;   /* 0x4807f/0x48089 */
        } else {                                     /* 0x48115 flags==0 */
            GP16(0x973c8 + h * 6) = RW(0x08); GP32(0x973c4 + h * 6) = RD(0x04); /* 0x4811c/0x48126 */
            buf_sel = RW(0x08); buf_off = RD(0x04);
            m28 = RD(0x48);                          /* 0x48130 [ebp-0x28]=gs:[R+0x48] */
        }
        RW(0x08) = buf_sel; RD(0x04) = buf_off;      /* 0x4813e/0x4814a merge */
        RD(0x48) = m28;                              /* 0x48155 */

        /* far-args seg (0x48159 branch). SFX: R+0x34==R+0x38==R+0x44==0 -> alloc-branch-B (0x481e9 ->
         * 0x5473c). The service allocated it; 0x5473c's [0x97290/f8/360+h*4] DPMI bookkeeping is moot
         * (unread by the mixer/close) -> zeroed. The 0x501c0 driver-control far-call (0x48281, gated
         * by [h*0x6c+0x73f8d]&2) writes no game memory under the virtual driver -> dropped. */
        GP32(0x97290 + h * 4) = 0; GP32(0x972f8 + h * 4) = 0; GP32(0x97360 + h * 4) = 0; /* 0x5473c moot */
        RW(0x38) = od.farg_sel; RD(0x34) = od.farg_off;          /* 0x482bd/0x482c9 */
        RW(0x40) = od.cb_sel;   RD(0x3c) = od.dispatch_off;      /* 0x482d4/0x482e0: driver dispatch */
        GP16(0x97310 + h * 6) = od.farg_sel; GP32(0x9730c + h * 6) = od.farg_off;        /* 0x482eb/0x482f5 */
        GP16(0x97330 + h * 6) = od.cb_sel;   GP32(0x9732c + h * 6) = od.dispatch_off;    /* 0x48302/0x4830c */
        GP32(0x97388 + h * 4) = 1;                   /* 0x48318 */

        /* fn-0xa result block [0x97b44] (0x4ff6f writes it from the driver return); the service ran
         * haudio_open_driver, so stamp the same {off,sel} the wrapper would. */
        GP32(0x97b44) = od.cb_off;    GP16(0x97b48) = od.cb_sel;      /* cb  {MAGIC_POLL, CS}   */
        GP32(0x97b4c) = od.pos_off;   GP16(0x97b50) = od.farg_sel;    /* pos {POS_OFF,  farg}   */
        GP32(0x97b54) = od.voices_off; GP16(0x97b58) = od.farg_sel;   /* vox {VOICE_OFF, farg}  */

        GP16(0x97424 + h * 6) = GP16(0x97b48); GP32(0x97420 + h * 6) = GP32(0x97b44); /* 0x4837c/0x48383 cb */
        GP16(0x97804 + h * 6) = GP16(0x97b50); GP32(0x97800 + h * 6) = GP32(0x97b4c); /* 0x4839a/0x483a1 pos */
        /* 0x4fd30 fan: 32 voice-struct far-ptrs {voices_off + i*0x6c, farg_sel} into [0x97440+h*0xc0]. */
        uint32_t voff = GP32(0x97b54); uint16_t vsel = GP16(0x97b58);
        for (uint32_t i = 0; i < 0x20u; i++) {       /* 0x4fd30 loop (ecx=0x20) */
            GP32(0x97440 + h * 0xc0 + i * 6)     = voff + i * 0x6cu;   /* [esi]=edi; edi+=0x6c */
            GP16(0x97440 + h * 0xc0 + i * 6 + 4) = vsel;               /* [esi+4]=es */
        }
    } else {                                         /* 0x483e0: jae arm (desc>=0xe106; unobserved) */
        GP16(0x97424 + h * 6) = 0; GP32(0x97420 + h * 6) = 0;         /* 0x483e4/0x483ed */
    }

    /* --- common tail (0x483f7..0x48663) --- */
    /* cb write-back into the descriptor (0x483f7): sos_timer_register_event (call_orig) later
     * registers THIS {off,sel} into the ISR-consumed timer table. */
    RW(0x20) = GP16(0x97424 + h * 6); RD(0x1c) = GP32(0x97420 + h * 6); /* 0x4840c/0x48415 */
    /* 0x50097 (int-2f 1600 / int-4b lock): moot, no game-memory write -> dropped. */
    GP32(0x973e4 + h * 4) = m28;                     /* 0x48432 */
    GP8(0x9783c + h) = (uint8_t)(QD(0x08) & 0xffu);  /* 0x48463/0x48469 (Q+8 low byte) */
    /* moot driver-init far-calls 0x4ffc2 / 0x50046 / 0x50020 / 0x4fff7: default case, no game writes. */
    GP32(0x97374 + h * 4) = 1;                       /* 0x484ff */
    GP32(0x9739c + h * 4) = 1;                       /* 0x4850f */

    /* per-slot driver-info block copy (0x48519..0x48645): the now-populated R descriptor -> the 0x4c-
     * stride block at [0x97844 + h*0x4c]. Moot (the virtual driver does not read it; close does not
     * touch it) but transcribed faithfully. */
    {
        uint32_t b = h * 0x4c;
        GP32(0x97844 + b) = RD(0x00);                                 /* 0x48524 */
        GP16(0x9784c + b) = RW(0x08); GP32(0x97848 + b) = RD(0x04);   /* 0x4853f/0x48546 */
        GP32(0x97850 + b) = RD(0x0c);                                 /* 0x48558 */
        GP32(0x97854 + b) = RD(0x10);                                 /* 0x4856a */
        GP32(0x97858 + b) = desc;                                     /* 0x48577 */
        GP32(0x9785c + b) = RD(0x18);                                 /* 0x48589 */
        GP16(0x97864 + b) = RW(0x20); GP32(0x97860 + b) = RD(0x1c);   /* 0x485a4/0x485ab */
        GP16(0x9786c + b) = RW(0x28); GP32(0x97868 + b) = RD(0x24);   /* 0x485c6/0x485cd */
        GP16(0x9787c + b) = RW(0x38); GP32(0x97878 + b) = RD(0x34);   /* 0x485e8/0x485ef */
        GP16(0x97874 + b) = RW(0x30); GP32(0x97870 + b) = RD(0x2c);   /* 0x4860a/0x48611 */
        GP16(0x97884 + b) = RW(0x40); GP32(0x97880 + b) = RD(0x3c);   /* 0x4862c/0x48633 */
        GP32(0x97888 + b) = RD(0x44);                                 /* 0x48645 */
    }

    *(volatile uint32_t *)(uintptr_t)handle_out = h; /* 0x48652: handle_out = slot index */
    return 0;                                        /* 0x48655 */
#undef RD
#undef RW
#undef QD
}

/* ---- SOS driver close_voices (retirement) ------------------------------------------------
 * 0x48666 sos_driver_close_voices — the teardown counterpart of open_voices (0x47dae). Frees the
 * per-handle voice slot: it far-calls a chain of DPMI + driver primitives to release the DMA/real-mode
 * buffers and stop the driver voice, then zeroes the per-slot bookkeeping tables the client/leaves read.
 * Every far-call is MOOT under the host (audited): block-A stop-voice 0x50220(int-2f status)/0x48a7a
 * (driver far-calls, no game-memory write)/0x4fd96(int-31); block-B buffer frees 0x4fe44/0x4ff4c
 * (indirect driver free-cb `call [ebp+8]`), voice free 0x500cc (int-2f/int-4b), and the a2-gated
 * 0x54664/0x54916 (DPMI unlock/free 0x4fbd2/0x4fc9d + a redundant [h*4+0x97374]=0). None write game
 * memory the client reads beyond that redundant slot-flag clear, which this native does anyway. The
 * host mixer stops a voice on its active bit (word[+0x30]&0x8000, cleared by voice-deactivate, NOT
 * here) and DPMI free is a no-op leak (dpmi.c 0x0502), so dropping the far-calls has no observable
 * effect. The r2 teardown trace (1 call: handle=0, a1=1, a2=1, ret=0, diff 97420:e0d20000->0
 * 97424:23->0) matches the block-B [h*6+0x97420/24]=0 store.
 *
 * The [h*4+0x9740c] >= 0xe106 guard (0x486ac jae) selects between the block-A driver-teardown and
 * skipping it; both arms converge on block B. Observed arm: [0x9740c]=0xe018 < 0xe106 -> jae NOT taken
 * -> block A runs (conditionally, iff [0x97b1c]!=0 && a1!=0). Transcribed faithfully; block A's only
 * game store is [h*4+0x97b1c]=0. (The jae arm just skips block A — reproduced by the same guard.)
 *
 * STAGED (pending class): native ONLY under ROTH_AU_AB=native; veneer default. */
uint32_t os_audio_close_voices_native(uint32_t handle, uint32_t a1, uint32_t a2)
{
    (void)a2;                                            /* a2 gates only the moot 0x54664/0x54916 frees */
    if (GP32(0x97374 + handle * 4) == 0)                 /* 0x48681: slot not open */
        return 1;                                        /* 0x4868a */
    GP32(0x9739c + handle * 4) = 0;                      /* 0x4869c */
    /* block A (0x486bc): the jb arm ([0x9740c] < 0xe106) does the driver stop-voice iff the slot has a
     * live streaming buffer ([0x97b1c]!=0) and a1!=0. The stop-voice far-calls are moot; its only game
     * store is clearing [0x97b1c]. The jae arm (>= 0xe106) skips straight to block B. */
    if (GP32(0x9740c + handle * 4) < 0xe106u) {          /* 0x486ac jae -> 0x48753 */
        if (GP32(0x97b1c + handle * 4) != 0 && a1 != 0)  /* 0x486c2/0x486cb */
            GP32(0x97b1c + handle * 4) = 0;              /* 0x48743 (after the moot stop-voice) */
    }
    /* block B (0x48753): moot buffer/voice/driver frees, then zero the per-slot bookkeeping. Offsets
     * with *6 stride are the {off:sel} far-ptr pairs (word sel then dword off); *4 are dword slots. */
    uint32_t s6 = handle * 6u, s4 = handle * 4u;
    GP16(0x972a8 + s6) = 0; GP32(0x972a4 + s6) = 0;      /* 0x487e0/0x487e9: buffer-A far-ptr */
    GP16(0x972c8 + s6) = 0; GP32(0x972c4 + s6) = 0;      /* 0x487f7/0x48800: buffer-B far-ptr */
    GP16(0x97424 + s6) = 0; GP32(0x97420 + s6) = 0;      /* 0x4880e/0x48817: DMA buffer far-ptr */
    GP16(0x97310 + s6) = 0; GP32(0x9730c + s6) = 0;      /* 0x48825/0x4882e */
    GP16(0x97330 + s6) = 0; GP32(0x9732c + s6) = 0;      /* 0x4883c/0x48845 */
    GP32(0x97824 + s4) = 0;                              /* 0x48855: slot owner id */
    GP32(0x97374 + s4) = 0;                              /* 0x48865: slot-active flag */
    return 0;                                            /* 0x4886f */
}

/* ---- SOS HMI timer service (retirement) ---------------------------
 * 0x49923 register_event / 0x49ca4 remove_event / 0x4980d configure_timer_rate / 0x498e9 stop_timer.
 * These edit the 16-slot SOS timer-event table (off32 0x979c4+i*6 / sel16 0x979c8+i*6 / rate 0x97a24+i*4
 * / Q16 step 0x97a64+i*4 / Q16 accumulator 0x97aa4+i*4) that the SOS master-timer ISR 0x49eaf walks on
 * EVERY int-8 tick (driven by the host SIGALRM inject_irq(0x08)). The original brackets each edit with a
 * PIC IRQ0 mask (0x54c89/0x54cb5); the host substitute is au_timer_lock()/au_timer_unlock() —
 * while locked, inject_irq DEFERS (not drops) int-8, so the ISR can never sample a torn table. Every
 * write to a table column here is inside a lock span that MATCHES OR EXCEEDS the original's mask span;
 * lock/unlock balance on all paths. The moot hardware is dropped and reproduced host-side: the PIT
 * `out 0x40` port writes -> host_pit_program (the port-0x40 trap's twin); the int21 25/35 (DOS) vector
 * install/teardown -> g_pm_vec_int21[8] (the array inject_irq delivers through) + IRQ_RET_MAGIC; the
 * int31 0204/0205 (DPMI) arm -> dpmi_get/set_pm_vec (DEAD in-game). Store-by-store from disasm with
 * canon VAs inline. STAGED ONLY (NOT in au_va_retired_default).
 *
 * The 0x741f8 "base divisor" (g_sos_timer_base_rate) is NOT read by the ISR — it is editor-private — so
 * its stores don't need the fence, but they ride inside it where they sit next to table writes. */

/* 0x49e7a sos_program_pit_divisor(EAX=div): sets [0x741f8]=div (unconditional), then 0x54b81 does the
 * `out 0x40` PIT program GATED on [0x755b4]!=0. Host substitute mirrors both halves; div is truncated to
 * 16 bits by 0x54b81's `out al`/`out ah` (the uint16_t cast). */
static void sos_program_pit(uint32_t div)
{
    GP32(0x741f8) = div;                                  /* 0x49e8e (always) */
    if (GP32(0x755b4))                                    /* 0x54b8d gate */
        host_pit_program((uint16_t)div);                 /* 0x54b81 out 0x40 -> host_pit_program */
}

/* Recompute the Q16 per-tick step for occupied slot j against the current base divisor [0x741f8]. The
 * 0xff00 heartbeat slot uses the 0x123333 numerator; every other slot uses rate<<16. Identical in
 * register (0x49a27..0x49aab) and remove (0x49d92..0x49e16). All 32-bit unsigned (matches the `div`s). */
static void sos_recompute_step(uint32_t j)
{
    uint32_t r = GP32(0x97a24 + j * 4);
    if (r == 0xff00) {                                   /* 0x49a2d / 0x49d98 */
        if (GP32(0x741f8) == 0xffff)                     /* 0x49a39 / 0x49da4 */
            GP32(0x97a64 + j * 4) = 0x10000;             /* 0x49a4b / 0x49db6 */
        else
            GP32(0x97a64 + j * 4) = 0x123333u / (0x1234dcu / GP32(0x741f8)); /* 0x49a57 / 0x49dc2 */
    } else {
        GP32(0x97a64 + j * 4) = (r << 16) / (0x1234dcu / GP32(0x741f8));     /* 0x49a7f / 0x49dea */
    }
}

/* An occupied slot = off!=0 || sel!=0 (the same predicate the ISR uses to decide a slot is live). */
static inline int sos_slot_occupied(uint32_t j)
{
    return GP32(0x979c4 + j * 6) != 0 || GP16(0x979c8 + j * 6) != 0;
}

/* 0x49923 sos_timer_register_event(EAX=rate, EBX=cb_off, CX=cb_sel, [+0x14]=off:sel handle-out far-ptr).
 * Claim the first free slot; if the new event's PIT divisor beats the current base, retune + rescale;
 * recompute every occupied step; store the slot index through the far pointer. Returns 0 (or 0xb full). */
uint32_t os_audio_timer_register_event_native(uint32_t rate, uint32_t cb_off, uint16_t cb_sel,
                                              uint32_t handle_out, uint16_t out_sel)
{
    uint32_t slot;                                       /* 0x4994c..0x4996b free-slot scan */
    for (slot = 0; slot < 0x10u; slot++)
        if (!sos_slot_occupied(slot))
            break;
    if (slot >= 0x10u)
        return 0xb;                                      /* 0x49971 table full */

    uint32_t factor = 0;                                 /* [ebp-0x14] accumulator rescale (Q16) */

    au_timer_lock();                                     /* 0x49986 PIC mask (gated [0x755b4]); always fence */
    GP16(0x979c8 + slot * 6) = cb_sel;                   /* 0x49998 selector */
    GP32(0x979c4 + slot * 6) = cb_off;                   /* 0x499a2 offset */
    GP32(0x97a24 + slot * 4) = rate;                     /* 0x499b1 rate */
    if ((0x1234dcu / rate) < GP32(0x741f8)) {            /* 0x499be/0x499c1 jae skip (only if faster) */
        sos_program_pit(0x1234dcu / rate);               /* 0x499d3 -> [0x741f8]=newdiv + PIT retune */
        /* factor = ([0x741f8]<<16)/newdiv; [0x741f8] was JUST set to newdiv, so this is exactly 0x10000.
         * Transcribed literally via [0x741f8] so the C is bit-identical to 0x499d8..0x499f1. */
        factor = (GP32(0x741f8) << 16) / (0x1234dcu / rate);
    }
    for (uint32_t j = 0; j < 0x10u; j++) {               /* 0x499f6..0x49b04 */
        if (!sos_slot_occupied(j))                       /* 0x49a10/0x49a19 */
            continue;
        sos_recompute_step(j);                           /* 0x49a27..0x49aab */
        if (factor != 0) {                               /* 0x49ab1 */
            uint32_t acc = GP32(0x97aa4 + j * 4);
            GP32(0x97aa4 + j * 4) =
                ((acc * (factor & 0xffffu)) >> 16)       /* 0x49ac6..0x49acf */
              + ((acc * (factor >> 16)) >> 16);          /* 0x49ae1..0x49af9 */
        }
    }
    au_timer_unlock();                                   /* 0x49b0d PIC unmask */

    /* 0x49b15/0x49b19 lgs edx,[ebp+0x14]; mov gs:[edx],slot — far-store the slot index. Base-flat DS
     * from the client -> dpmi_sel_base(out_sel)==0 -> linear == handle_out (same as au_r32/au_w8). */
    *(volatile uint32_t *)(uintptr_t)(dpmi_sel_base(out_sel) + handle_out) = slot;
    return 0;                                            /* 0x49b1c */
}

/* 0x49ca4 sos_timer_remove_event(EAX=event slot). Clear the slot, retune the PIT to the highest
 * remaining rate (or idle 0xffff), recompute occupied steps + ZERO their accumulators. Returns 0. */
uint32_t os_audio_timer_remove_event_native(uint32_t event)
{
    au_timer_lock();                                     /* fence the whole edit — EXCEEDS the original
                                                          * span (the original clears sel/off + scans +
                                                          * programs the PIT UNMASKED, then masks only the
                                                          * recompute loop; one lock is strictly safer and
                                                          * the final memory image is identical). */
    GP16(0x979c8 + event * 6) = 0;                       /* 0x49cc0 clear selector */
    GP32(0x979c4 + event * 6) = 0;                       /* 0x49cc9 clear offset */

    uint32_t maxrate = 0;                                /* 0x49cdf..0x49d32 highest remaining rate */
    for (uint32_t j = 0; j < 0x10u; j++) {
        if (!sos_slot_occupied(j))                       /* 0x49ce9/0x49cf2 */
            continue;
        uint32_t r = GP32(0x97a24 + j * 4);
        if (r > maxrate && r != 0xff00)                  /* 0x49d08 jbe skip; 0x49d13 exclude heartbeat */
            maxrate = r;                                 /* 0x49d27 */
    }
    if (maxrate != 0)                                    /* 0x49d32 */
        sos_program_pit(0x1234dcu / maxrate);            /* 0x49d3f/0x49d42 */
    else
        sos_program_pit(0xffff);                         /* 0x49d49/0x49d4e idle base */

    for (uint32_t j = 0; j < 0x10u; j++) {               /* 0x49d61..0x49e31 */
        if (!sos_slot_occupied(j))
            continue;
        sos_recompute_step(j);                           /* 0x49d92..0x49e16 */
        GP32(0x97aa4 + j * 4) = 0;                       /* 0x49e22 zero accumulator */
    }
    au_timer_unlock();
    return 0;                                            /* 0x49e3f */
}

/* 0x54bc7 vector install (repro): stash the service far-ptr the host inject_irq->0x54b05 stub
 * `call [0x74590]`s (= SOS master ISR 0x49eaf : CS), then — gated on [0x755b4] — install int-8 -> 0x54b05
 * saving the old vector to [0x74584]/[0x74588], then program the PIT to 0xffff. The DOS int21 25/35 arm
 * ([0x755b8]==0) is LIVE in-game (inject_irq delivers via g_pm_vec_int21); the DPMI int31 0204/0205 arm
 * ([0x755b8]!=0) is DEAD (single caller passes flags=0). The whole body sits under one lock matching the
 * CALLER's outer PIC mask (0x49843..0x49861), which spans the [0x74590]/[0x74594] far-ptr pair the ISR
 * stub reads — so no torn far-ptr. */
static void sos_install_timer_vector(void)
{
    au_timer_lock();                                     /* 0x49843 caller's outer PIC mask */
    GP32(0x74590) = CANON(0x49eaf);                      /* 0x54bd6 service off = master ISR */
    GP16(0x74594) = cur_cs();                            /* 0x54bdc service sel = CS */
    GP16(0x74598) = cur_ds();                            /* 0x54be3 */
    GP16(0x7459e) = cur_ds();                            /* 0x54bea */
    if (GP32(0x755b4)) {                                 /* 0x54bf1 (0x54bc7's inner mask subsumed by outer) */
        if (GP32(0x755b8)) {                             /* 0x54c06 DPMI arm (DEAD in-game) */
            GP32(0x74584) = dpmi_get_pm_vec(8);          /* 0x54c13 int31 0204 -> old off */
            GP16(0x74588) = 0x23;                        /* 0x54c1b old sel (0204 returns CX=0x23) */
            dpmi_set_pm_vec(8, CANON(0x54b05));          /* 0x54c28 int31 0205 -> 0x54b05 stub */
        } else {                                         /* 0x54c3d DOS arm (LIVE in-game) */
            GP32(0x74584) = g_pm_vec_int21[8] ? g_pm_vec_int21[8]
                                              : IRQ_RET_MAGIC; /* 0x54c3d int21 35 (ES:EBX) */
            GP16(0x74588) = cur_cs();                    /* 0x54c49 ES = game CS */
            g_pm_vec_int21[8] = CANON(0x54b05);          /* 0x54c56 int21 2508 -> 0x54b05 stub */
        }
        host_pit_program(0xffff);                        /* 0x54c64 out 0x40 (divisor [ebp+8]=0xffff) */
    }
    au_timer_unlock();                                   /* 0x49861 caller's outer PIC unmask */
}

/* 0x4980d sos_configure_timer_rate(EAX=rate, EDX=flags). In-game: rate=0xff00, flags=0. Sets the
 * DPMI/DOS mode flag [0x755b8], (re)installs the int-8 vector + service ([0x755b4]=1 arm), then either
 * idles the base rate or programs the PIT + installs the "system heartbeat" as table slot 15. Ret 0. */
uint32_t os_audio_configure_timer_rate_native(uint32_t rate, uint32_t flags)
{
    GP8(0x755b8) = (flags & 2) ? 1 : 0;                  /* 0x49820..0x4982f (byte write) */
    if (!(flags & 1)) {                                  /* 0x49836 */
        GP8(0x755b4) = 1;                                /* 0x4983c */
        sos_install_timer_vector();                      /* 0x49843 mask / 0x54bc7 / 0x49861 unmask */
    } else {
        GP8(0x755b4) = 0;                                /* 0x49868 */
    }
    /* 0x4986f: idle when rate==0 OR (flags&1); else program the PIT + install table slot 15. */
    if (rate == 0 || (flags & 1)) {
        GP32(0x741f8) = 0xffff;                          /* 0x498cf base rate idle (no PIT reprogram) */
    } else {
        au_timer_lock();                                 /* slot-15 table writes -> fence (EXCEEDS original,
                                                          * which writes slot 15 UNMASKED at 0x498b3) */
        if (rate == 0xff00) {                            /* 0x4987d */
            sos_program_pit(0xffff);                     /* 0x4988b */
            GP32(0x97a60) = 0xff00;                      /* 0x49890 slot-15 rate (0x97a24+15*4) */
        } else {
            sos_program_pit(0x1234dcu / rate);           /* 0x4989c/0x498a6 */
            GP32(0x97a60) = rate;                        /* 0x498ae slot-15 rate */
        }
        GP16(0x97a22) = cur_cs();                        /* 0x498b3 slot-15 sel (0x979c8+15*6) */
        GP32(0x97a1e) = CANON(0x49f78);                  /* 0x498b9 slot-15 off (0x979c4+15*6) = system stub */
        GP32(0x97aa0) = 0x10000;                         /* 0x498c3 slot-15 step (0x97a64+15*4) = 1.0 Q16 */
        au_timer_unlock();
    }
    return 0;                                            /* 0x498d9 */
}

/* 0x498e9 sos_stop_timer_service — if the service was installed ([0x755b4]), tear it down (0x54ce1):
 * restore the saved int-8 vector then `out 0x40,0` (host-ignored, like the port trap). No table writes;
 * the lock matches the original's teardown mask span (and guards the vector restore inject_irq reads).
 * NB the original does NOT clear [0x755b4] — reproduced (it stays 1). Returns 0. */
uint32_t os_audio_stop_timer_service_native(void)
{
    if (GP32(0x755b4)) {                                 /* 0x498fa */
        au_timer_lock();                                 /* 0x49903 PIC mask + 0x54ce1 inner mask */
        if (GP32(0x755b8))                               /* 0x54d02 DPMI arm (DEAD) */
            dpmi_set_pm_vec(8, GP32(0x74584));           /* 0x54d0f int31 0205 restore */
        else                                             /* 0x54d29 DOS arm (LIVE) */
            g_pm_vec_int21[8] = GP32(0x74584);           /* 0x54d3d int21 2508 restore */
        host_pit_program(0);                             /* 0x54d40 out 0x40,0 -> div 0 (host no-op) */
        au_timer_unlock();                               /* 0x4990d PIC unmask */
    }
    return 0;                                            /* 0x49912 */
}
