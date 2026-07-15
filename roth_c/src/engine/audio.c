/* lift_audio.c — the ROTH `audio` subsystem (the game-side SOS client: MIDI music, digital SFX,
 * streamed speech/voice, volume/driver lifecycle) lifted to verified C. Own TU per
 * docs/operating/recomp.md §4.6. lift-lens: docs/reference/lift/audio.md;
 * behavior/format reference: docs/reference/ROTH_audio_notes.md.
 *
 * SCOPE: the SOS *client* only. The SOS driver library itself (sos_* at 0x44xxx-0x4axxx + the synth
 * drivers) is classified host_audio_driver — host-replaced, NEVER lifted; the client reaches it through
 * au_call bridges. So this TU is bookkeeping + parsing + the play/stop state machines, handing bytes to
 * host primitives.
 *
 * ADDRESSING: canon-literal globals via G8/G16/G32; every pointer VALUE loaded from a global/record is a
 * live runtime address — deref RAW via the ld/st helpers (gotcha A4). Stored-pointer globals here:
 * g_sfx_node_list [0x85c44], the SFX zone list [0x85c48], g_sound_sample_table base [0x848f4], the voice
 * descriptor table 0x84874 (a canon-literal ARRAY of runtime pointers: the array address is GADDR'd, the
 * slot VALUES deref raw).
 *
 * ABI: every signature derived from the DISASM (recomp.md §4.4) — Watcom __watcall
 * register args, full-EAX return fidelity where callers could consume more than AL (e.g.
 * find_free_voice_descriptor returns the original's address-remnant high bytes).
 */
#include "common.h"
#include "engine.h"
#include "os_api.h"    /* C2: the os_audio_* SOS-driver service (client re-point) */
#include <string.h>

/* raw flat deref of a runtime pointer + byte offset (stored pointers / staged records — gotcha A4) */
static inline uint32_t ld32u(uint32_t p) { return *(volatile uint32_t *)(uintptr_t)p; }
static inline int32_t  ld32s(uint32_t p) { return *(volatile int32_t  *)(uintptr_t)p; }
static inline uint16_t ld16 (uint32_t p) { return *(volatile uint16_t *)(uintptr_t)p; }
static inline uint8_t  ld8  (uint32_t p) { return *(volatile uint8_t  *)(uintptr_t)p; }
static inline void st32(uint32_t p, uint32_t v) { *(volatile uint32_t *)(uintptr_t)p = v; }
static inline void st16(uint32_t p, uint16_t v) { *(volatile uint16_t *)(uintptr_t)p = v; }
static inline void st8 (uint32_t p, uint8_t  v) { *(volatile uint8_t  *)(uintptr_t)p = v; }

/* ============================================================ A. SFX — Layer 1 leaves */

/* compute_sound_pan_from_position 0x43cce (54 B) — pan for a world position: rotate the (x,y) at `pos`
 * into camera space (rotate_point_2d_shifted 0x2b25b, lifted), take the lateral component (the low word
 * of the EDX output, sign-extended), scale by 0x7400/(dist+1), center on 0x8000, clamp to [0,0xffff].
 * ABI: EAX=pos (ptr to int16 x,y), EDX=dist -> EAX=pan. Leaf (callee lifted -> call-closed). */
uint32_t compute_sound_pan_from_position(uint32_t pos, uint32_t dist)
{
    int32_t rot_e, rot_d;
    rotate_point_2d_shifted((int16_t)ld16(pos), (int16_t)ld16(pos + 2), &rot_e, &rot_d);
    int32_t lat  = (int32_t)(int16_t)rot_d;              /* xchg edx,eax; cwde */
    int32_t ecx  = (int32_t)dist + 1;                    /* pop ecx (pushed edx); inc ecx */
    int32_t q    = (int32_t)(((int64_t)lat * 0x7400) / ecx);   /* imul edx; idiv ecx */
    uint32_t pan = (uint32_t)(q + 0x8000);
    if (pan < 0xffffu)        return pan;                /* jb  — in range */
    if ((int32_t)pan < 0xffff) return 0;                 /* jl  — negative -> 0 */
    return 0xffff;                                       /* >= 0xffff -> clamp */
}

/* compute_sound_volume_pan 0x26f48 (312 B) — distance -> attenuated volume for a playing-sound record.
 * rec: +0 sample ptr (-> +0xa u16 range, +0x10 u8 sample vol /64) · +4 flags (0x80 = use the record's
 * own coords) · +6 zone side (1-based; 0 = no zone attenuation) · +8 distance^2 in · +0xc distance out ·
 * +0x14/+0x16 own int16 coords. vol = 0x7fff - dist*0x7fff/range, scaled by sample vol (>>6), then the
 * zone-wall scan: the FIRST wall rect the player-relative position "hits" subtracts wall.byte0<<7 and
 * STOPS the scan (normal rect: hit = outside; flag-bit0 rect: hit = strictly inside). Finally scaled by
 * the SFX master volume [0x71d84] (imul then UNSIGNED shr 15 — B1) and double-clamped to 0x7fff (original
 * quirk). Early-negative paths return 0 WITHOUT master scaling. Writes [rec+0xc]=dist.
 * Shared TAIL (flow_succ) of load_sound_sample 0x277db / evict_oldest_voice_descriptor 0x27a3e /
 * update_active_sounds 0x27b05 — defined ONCE here, those lifts call it.
 * ABI: EAX=rec -> EAX=vol (0..0x7fff). Leaf (isqrt wrapper 0x3bfd6 lifted -> call-closed). */
int32_t compute_sound_volume_pan(uint32_t rec)
{
    uint32_t samp  = ld32u(rec);
    int32_t  range = (int32_t)ld16(samp + 0xa);          /* movzx esi,word[eax+0xa] */
    uint32_t dist  = isqrt_fixed_wrapper_3bfd6(ld32u(rec + 8));
    st32(rec + 0xc, dist);
#ifdef ROTH_STANDALONE
    /* SAFETY (imgfree): the original idiv would #DE (crash) on range==0, so valid game data never
     * reaches here with range 0 — but a degenerate/stale sample record has been seen with
     * range==0 (and sampvol==0) image-free (SIGFPE in update_active_sounds' per-frame volume pass).
     * Treat it as silent (like the edx<0 early return) instead of crashing. Trap host / oracle are
     * byte-identical (guard is standalone-only); the root (why a played sample has range 0 — likely
     * a stale DAS-slot sample pointer) is tracked separately. */
    if (range == 0) return 0;
#endif
    int32_t edx = 0x7fff - (int32_t)(dist * 0x7fffu) / range;   /* imul 0x7fff; idiv */
    if (edx < 0) return 0;                               /* jl 0x27024 */
    uint32_t sv = ld8(samp + 0x10);
    if (sv != 0x40) edx = (edx * (int32_t)sv) >> 6;      /* imul; sar 6 */
    uint8_t side = ld8(rec + 6);
    if (side != 0) {
        uint32_t blk   = (uint32_t)G32(VA_g_sfx_nodes + 0x4) + ((uint32_t)side << 5) - 0x20;
        int32_t  count = (int32_t)(int16_t)ld16(blk);
        int32_t  ex, ey;
        if (ld8(rec + 4) & 0x80) { ex = (int32_t)(int16_t)ld16(rec + 0x14); ey = (int32_t)(int16_t)ld16(rec + 0x16); }
        else                     { uint32_t p = ld32u(rec); ex = (int32_t)(int16_t)ld16(p); ey = (int32_t)(int16_t)ld16(p + 2); }
        int32_t rel_a = (G32(VA_g_player_z + 0x2) >> 16) - ey;       /* edi - ebx  (player X-ish - coord2) */
        int32_t rel_b = (G32(VA_g_player_angle + 0x2) >> 16) - ex;       /* esi - ecx  (player Y-ish - coord1) */
        uint32_t w = blk + 2;
        for (int32_t i = 0; i < count; i++, w += 0xa) {
            int hit;
            if (ld8(w + 1) & 1)                          /* inverted rect: hit = strictly inside */
                hit = ((int32_t)(int16_t)ld16(w + 2) < rel_b) && (rel_b < (int32_t)(int16_t)ld16(w + 6))
                   && ((int32_t)(int16_t)ld16(w + 4) < rel_a) && (rel_a < (int32_t)(int16_t)ld16(w + 8));
            else                                         /* normal rect: hit = outside any bound */
                hit = (rel_b < (int32_t)(int16_t)ld16(w + 2)) || (rel_b > (int32_t)(int16_t)ld16(w + 6))
                   || (rel_a < (int32_t)(int16_t)ld16(w + 4)) || (rel_a > (int32_t)(int16_t)ld16(w + 8));
            if (hit) {
                edx -= (int32_t)((uint32_t)ld8(w) << 7);
                if (edx < 0) return 0;                   /* jl 0x27024 */
                break;                                   /* jge 0x2704a — first hit stops the scan */
            }
        }
    }
    if (G32(VA_g_test_sfx_descriptor + 0x3a) == 0) return 0;
    edx = (int32_t)(((uint32_t)(edx * G32(VA_g_test_sfx_descriptor + 0x3a))) >> 15);    /* imul; SHR (unsigned, B1) */
    if (edx > 0x7fff) edx = 0x7fff;
    if (edx > 0x7fff) edx = 0x7fff;                      /* original's duplicate clamp */
    return edx;
}

/* find_free_voice_descriptor 0x2799c (32 B) — scan the 32-slot voice-descriptor pointer table 0x84874
 * for the first zero slot. Returns the index in AL; the HIGH BYTES of EAX are the original's scan-pointer
 * remnant (mov eax,imm relocated + 4*i, low byte replaced) — reproduced for full-EAX fidelity.
 * ABI: void -> EAX (AL = free index, or 0xff if none). Pure leaf. */
uint32_t find_free_voice_descriptor(void)
{
    uint32_t p = (uint32_t)GADDR(VA_g_sound_voice_descriptors);
    for (uint32_t i = 0; i < 0x20; i++, p += 4)
        if (ld32u(p) == 0)
            return (p & ~0xffu) | i;                     /* mov al,dl */
    return (p & ~0xffu) | 0xff;                          /* mov al,0xff (p = base+0x80) */
}

/* release_voice_descriptor 0x279e4 (90 B) — free descriptor slot AL: 0xff/empty -> 0; locked
 * (desc byte+8 != 0) -> 2; else unmark its sample-table row (byte +0xb = 0xff, stride 0xc, base
 * [0x848f4]), free the descriptor chunk (free_resource_chunk 0x26b4c, lifted), zero the slot -> 1.
 * ABI: EAX (AL=index) -> EAX = 0/1/2. */
uint32_t release_voice_descriptor(uint32_t eax_in)
{
    uint32_t idx = eax_in & 0xff;                        /* movzx edx,al */
    if (idx == 0xff) return 0;
    uint32_t slot = (uint32_t)GADDR(VA_g_sound_voice_descriptors) + (idx << 2);
    uint32_t desc = ld32u(slot);
    if (desc == 0) return 0;                             /* test eax,eax; je (eax=0) */
    if (ld8(desc + 8) != 0) return 2;
    int32_t row = (int32_t)(int16_t)ld16(desc + 0xa) * 0xc;
    st8((uint32_t)G32(VA_g_sound_sample_table) + (uint32_t)row + 0xb, 0xff);
    free_resource_chunk((uint8_t *)(uintptr_t)desc);
    st32(slot, 0);
    return 1;
}

/* collect_sfx_nodes_by_key 0x43ab4 (87 B) — fill a query buffer with the list-offsets of every SFX node
 * whose key word (+6) matches: out+0 u16 count, out+2 u16 key, out+4.. u16 offsets (node - list base).
 * cap = buffer capacity in u16 slots (must be >= 3; entry capacity = cap-2). The node-count check is at
 * the loop BOTTOM (a count of 0 still compares node 0 — original quirk, reproduced by the do/while).
 * Nodes: list [0x85c44], count u16 at +2, nodes at +4, stride 0x12.
 * ABI: EAX=key, EDX=out, EBX=cap -> EAX = entries written (0 if cap < 3). */
uint32_t collect_sfx_nodes_by_key(uint32_t key, uint32_t out, uint32_t cap)
{
    uint32_t ecx = cap;
    st16(out + 2, (uint16_t)key);
    if (ecx < 3) return 0;                               /* jb 0x43b03 (sub eax,eax) */
    ecx -= 2;
    uint32_t room = ecx;
    uint32_t edx  = out + 4;
    uint32_t list = (uint32_t)G32(VA_g_sfx_nodes);
    uint32_t node = list + 4;
    int32_t  n    = (int32_t)ld16(list + 2);             /* sub ebp,ebp; mov bp,[ebx+2] */
    do {
        if (ld16(node + 6) == (uint16_t)key) {
            st16(edx, (uint16_t)(node - list));
            edx += 2;
            if (--ecx == 0) break;
        }
        node += 0x12;
    } while (--n > 0);
    uint32_t written = room - ecx;
    st16(out, (uint16_t)written);
    return written;
}

/* sort_sfx_query_by_distance 0x43c89 (69 B) — bubble-sort the query buffer's 8-byte records
 * {+0 u32 node/key, +4 i32 dist} ascending by dist (signed), early-out when a pass swaps nothing.
 * q: +0 u32 array ptr, +6 u16 count. ABI: EAX=q -> void (regs preserved). Pure leaf. */
void sort_sfx_query_by_distance(uint32_t q)
{
    uint32_t arr = ld32u(q);
    int32_t  n   = (int32_t)ld16(q + 6);                 /* movzx; or/jle */
    if (n <= 0) return;
    int32_t ecx = n - 1;
    if (ecx == 0) return;
    do {
        int swapped = 0;
        int32_t  m   = ecx;
        uint32_t esi = arr;
        do {
            int32_t a = ld32s(esi + 4);
            if (a > ld32s(esi + 0xc)) {                  /* jle skip (signed) */
                int32_t  b = ld32s(esi + 0xc);           /* xchg [esi+0xc],eax */
                st32(esi + 0xc, (uint32_t)a);
                st32(esi + 4,  (uint32_t)b);
                uint32_t p  = ld32u(esi);                /* xchg [esi+8],eax */
                uint32_t p2 = ld32u(esi + 8);
                st32(esi + 8, p);
                st32(esi, p2);
                swapped = 1;
            }
            esi += 8;
        } while (--m > 0);
        if (!swapped) return;
    } while (--ecx > 0);
}

/* load_sfx_node_active_state 0x43d98 (64 B) — restore the per-node active bit (flag byte +8 bit 0x80)
 * from a savegame bitmask stream at src (MSB-first, 32 nodes per dword). flow_succ: when the node count
 * runs out MID-dword (or the list is empty) the original falls into save_sfx_node_active_state's TAIL
 * (0x43d7e): it left-shifts out the remaining bit budget, WRITES the shifted leftover to the dword slot
 * AFTER the one just consumed (an original scribble-quirk, reproduced), and returns bytes-consumed+4.
 * When the count is an exact multiple of 32 it exits via its OWN epilogue and returns src unchanged.
 * ABI: EAX=src -> EAX (see above). Nodes: [0x85c44], count = high u16 of dword +0, stride 0x12 from +4. */
uint32_t load_sfx_node_active_state(uint32_t src)
{
    uint32_t edi  = src;
    uint32_t list = (uint32_t)G32(VA_g_sfx_nodes);
    int32_t  ecx  = (int32_t)(ld32u(list) >> 16);
    uint32_t node = list + 4;
    for (;;) {                                           /* outer 0x43dad — per source dword */
        int32_t  edx = 0x20;
        uint32_t eax = ld32u(edi);
        edi += 4;
        for (;;) {                                       /* inner head 0x43db7 */
            if (ecx <= 0) {                              /* jle 0x43d7e — the shared save_ tail */
                node += 0x12; ecx--; edx--;
                while (edx > 0) { eax <<= 1; node += 0x12; ecx--; edx--; }
                st32(edi, eax);                          /* mov [edi],eax — the scribble */
                edi += 4;
                return edi - src;                        /* eax = edi - pushed eax_in */
            }
            st8(node + 8, (uint8_t)(ld8(node + 8) & 0x7f));
            uint32_t carry = eax >> 31;                  /* add eax,eax sets CF from bit 31 */
            eax <<= 1;
            if (carry) st8(node + 8, (uint8_t)(ld8(node + 8) | 0x80));
            node += 0x12; ecx--; edx--;
            if (edx <= 0) break;                         /* jg 0x43db7 */
        }
        if (ecx > 0) continue;                           /* jg 0x43dad — next dword */
        return src;                                      /* own epilogue: pop eax = input */
    }
}

/* ============================================================ shared bridge / segment helpers */

/* Bridge a cross-subsystem callee (SOS driver lib / DOS helpers) with explicit register inputs;
 * returns the callee's EAX. All four Watcom arg regs staged; outputs beyond EAX unused by these sites. */
static uint32_t au_bridge(uint32_t canon, uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx)
{
    regs_t r; memset(&r, 0, sizeof r);
    r.eax = eax; r.edx = edx; r.ebx = ebx; r.ecx = ecx;
    r.va = canon + OBJ_DELTA;
#ifndef ROTH_STANDALONE
    call_orig(&r);
    return r.eax;
#else
    if (canon == 0x43e71u) return 0;    /* dpmi_lock_sos_driver_regions: flat-host locks are accepted
                                         * no-ops (dpmi.c int31 0600) and no obj1 driver regions exist
                                         * image-free (the drivers are host-C natives) -> success (0) */
    if (canon == 0x531fbu) {            /* far-strcpy(dst=EAX@EDX-sel, src=EBX@ECX-sel): resolve both
                                         * through the selector-base provider (flat DS -> base 0) */
        uint8_t *dst = (uint8_t *)(uintptr_t)(eax + (g_os_sel_base ? g_os_sel_base((uint16_t)edx) : 0));
        const uint8_t *src = (const uint8_t *)(uintptr_t)(ebx + (g_os_sel_base ? g_os_sel_base((uint16_t)ecx) : 0));
        while ((*dst++ = *src++) != 0) {}
        return eax;
    }
    roth_unreachable(canon);   /* residual SOS-driver-lib bridge — the audio ladder owns this (STOP) */
    return 0;
#endif
}

#ifdef ROTH_STANDALONE
/* CRT printf 0x27c6d, image-free: sos_load_driver's two debug prints pass LITERAL fmts (no
 * conversions — "here 2\r\n$" @0x75cb7 / "midi failed\n$" @0x75ccd, dollars included); the CRT
 * printf bottoms out in a DOS fd-1 write, so reproduce exactly that via int21 AH=0x40. */
static void au_if_printf_literal(uint32_t fmt)
{
    uint32_t len = 0;
    while (*(const char *)(uintptr_t)(fmt + len) != 0) len++;
    regs_t w; memset(&w, 0, sizeof w);
    w.eax = 0x4000; w.ebx = 1; w.ecx = len; w.edx = fmt;
    g_os_soft_int(0x21, &w);
}
#endif

/* The original passes the live DS/CS selector values to the SOS driver (`mov ecx,ds` / `mov ecx,cs`).
 * Under this host the game runs natively in-process, so the current selectors ARE the game's — reading
 * them is faithful in both the oracle and a live dispatch. */
static inline uint16_t cur_ds(void) { uint16_t v; __asm__("mov %%ds,%0" : "=r"(v)); return v; }
static inline uint16_t cur_cs(void) { uint16_t v; __asm__("mov %%cs,%0" : "=r"(v)); return v; }

/* exact x86 cdq/xor/sub absolute value (matches INT_MIN behavior) */
static inline int32_t au_abs(int32_t v) { int32_t s = v >> 31; return (v ^ s) - s; }

/* ============================================================ D. SOS voice wrappers (lifted early —
 * the SFX tower call-closes into these; each is register marshalling + ONE host_audio_driver bridge).
 *   - 0x4a641 sos_voice_start / 0x4ad03 sos_voice_load_to_slot: host-replaced — NEVER lifted (the host
 *     virtual driver owns them); oracle ret-stubs their entries at the bridge boundary.
 *   - 0x49fe9 / 0x4a28c / 0x4a54a / 0x4ac55: [KEPT-REPOINT — PERMANENT host-boundary, NOT a deferred
 *     convert (investigated + ruled out). Yes, they HAVE lifted bodies (voice_xchg_w32/
 *     w54_if_active, voice_get_w34, voice_deactivate_slot in renderer.c) — but those exist ONLY to
 *     BYTE-VERIFY the data ops against call_orig over a STAGED voice table. Each leaf resolves its vcb
 *     via `lgs edx, [eax + 0x97440]` (g_sos_voice_table[handle*0xc0 + voice*6]). That table is
 *     populated by the REAL SOS driver's voice allocation — but sos_voice_start 0x4a641 is
 *     host-replaced and returns 0 (allocates nothing), so 0x97440 is UNPOPULATED in-game. In-game the
 *     original leaf lgs's a null/garbage selector -> a #GP the host SERVICES (traps.c null-SOS-handler
 *     / selector-window path). A direct lifted call would instead au_resolve_voice_vcb -> null base ->
 *     flat-ptr deref -> a HARD SIGSEGV the host cannot service. So these MUST stay call_orig in-game;
 *     converting would break audio. No unblock — permanent (retires only if the host ever populates a
 *     real voice table, which the virtual-driver design never will).] */

/* sos_submit_voice 0x15a4a (27 B) — install the far user-callback {off,sel} into the voice struct
 * (+0x1c/+0x20), then sos_voice_start(EAX=EDX=driver handle [0x7f4dc], EBX=voice, ECX=DS).
 * ABI: EAX=voice, EBX=cb offset, CX=cb selector -> EAX = driver return (voice handle). */
uint32_t sos_submit_voice(uint32_t voice, uint32_t cb_off, uint32_t cb_sel)
{
    st16(voice + 0x20, (uint16_t)cb_sel);
    st32(voice + 0x1c, cb_off);
    uint32_t h = (uint32_t)G32(VA_g_sos_digital_device);
    return os_audio_voice_start(h, voice, cur_ds());   /* was au_bridge(0x4a641,h,h,voice,ds): EAX==EDX==h collapsed */
}

/* sos_stop_voice 0x15a65 (20 B) — sos_voice_deactivate_slot(EAX=handle [0x7f4dc], EDX=voice; EBX=handle).
 * ABI: EAX=voice -> EAX = driver return. */
uint32_t sos_stop_voice(uint32_t voice)
{
    uint32_t h = (uint32_t)G32(VA_g_sos_digital_device);
    return os_audio_voice_field(0, h, voice, 0);  /* [HOST-BOUNDARY] deactivate 0x4ac55; EBX=h,ECX=0 in shim */
}

/* sos_voice_set_callback 0x15a92 (10 B) — sos_voice_load_to_slot(EAX,EDX,EBX passthrough, ECX=DS).
 * NB: the wrapper passes the CALLER's EBX through to the driver (the queued voice-struct pointer —
 * both callback sites load EBX=&0x82045 before calling) — an H-class leftover-register argument. */
uint32_t sos_voice_set_callback(uint32_t eax, uint32_t edx, uint32_t ebx)
{
    return os_audio_voice_load_to_slot(eax, edx, ebx, cur_ds());  /* was au_bridge(0x4ad03,eax,edx,ebx,ds) */
}

/* sos_voice_set_w32 0x15a9c (22 B) — sos_voice_xchg_w32_if_active(EAX=handle, EDX=voice, EBX=value).
 * ABI: EAX=voice, EDX=value -> EAX = driver return. (The game's volume setter.) */
uint32_t sos_voice_set_w32(uint32_t voice, uint32_t val)
{
    uint32_t h = (uint32_t)G32(VA_g_sos_digital_device);
    return os_audio_voice_field(1, h, voice, val);  /* [HOST-BOUNDARY] xchg_w32 0x49fe9; shim sets ECX=h */
}

/* sos_voice_set_w54 0x15ab2 (22 B) — sos_voice_xchg_w54_if_active(same shape). (The pan setter.) */
uint32_t sos_voice_set_w54(uint32_t voice, uint32_t val)
{
    uint32_t h = (uint32_t)G32(VA_g_sos_digital_device);
    return os_audio_voice_field(2, h, voice, val);  /* [HOST-BOUNDARY] xchg_w54 0x4a28c; shim sets ECX=h */
}

/* sos_voice_get_w34_wrapper 0x15a79 (5 B) — bare `jmp sos_voice_get_w34 0x4a54a` thunk (flow_succ);
 * pass the arg regs through to the driver. */
uint32_t sos_voice_get_w34_wrapper(uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx)
{
    (void)ecx;   /* 0x4a54a reads only EAX=handle,EDX=voice (disasm 0x4a557/0x4a55a) — EBX/ECX disasm-dead */
    return os_audio_voice_field(3, eax, edx, ebx);  /* [HOST-BOUNDARY] get_w34 0x4a54a */
}

/* ============================================================ D. audio stream buffers (lifted early —
 * load_sound_effect_bank call-closes into alloc; free_resource_buffers [L] tail-calls free) */

/* alloc_audio_stream_buffers 0x30051 (195 B) — one-time: the 0x2000 mix scratch [0x85494] (filled with
 * 0x80808080 unless [0x7f478], i.e. unsigned-PCM silence), the two 0x8000 voice stream buffers
 * [0x8548c]/[0x85490]; then, if the resource pool [0x85c40] is absent, size it from the free game heap
 * (query_game_heap_free - 0x32000 headroom; tiers 0x64000/0xc8000/0x190000 by 1/3/6 MB, the 1MB tier
 * also sets the low-memory flag 0x89f41) and create it (game_heap_alloc + init_resource_chunk_pool).
 * All callees already lifted -> call-closed. ABI: void (regs preserved around the pool part). */
void alloc_audio_stream_buffers(void)
{
    if (G32(VA_g_audio_decode_buffer_a) == 0) {
        uint32_t p = game_heap_alloc(0x2000);
        G32(VA_g_audio_silence_buffer) = (int32_t)p;
        if (p != 0) {
            uint32_t fill = (G32(VA_g_audio_signed_samples) != 0) ? 0 : 0x80808080u;
            for (uint32_t i = 0; i < 0x800; i++) st32(p + i * 4, fill);   /* rep stosd */
        }
        G32(VA_g_audio_decode_buffer_a) = (int32_t)game_heap_alloc(0x8000);
        G32(VA_g_audio_decode_buffer_b) = (int32_t)game_heap_alloc(0x8000);
    }
    if (G32(VA_g_resource_pool) == 0) {
        uint32_t avail = query_game_heap_free() - 0x32000;
        uint32_t sz = 0;
        if (avail >= 0x100000) {
            G8(VA_g_resource_pool_small_flag) = 1;
            sz = 0x64000;
            if (avail >= 0x300000) {
                sz += sz;
                G8(VA_g_resource_pool_small_flag) = 0;
                if (avail >= 0x600000)
                    sz += sz;                            /* (the >=0x700000 cmp is a no-op quirk) */
            }
        }
        if (sz != 0) {
            uint32_t p = game_heap_alloc((int32_t)sz);
            G32(VA_g_resource_pool) = (int32_t)init_resource_chunk_pool((uint32_t *)(uintptr_t)p, (int32_t)sz);
        }
    }
}

/* free_audio_stream_buffers 0x30162 (107 B) — free the three stream buffers + swap-out/free the
 * resource pool [0x85c40]. game_heap_free lifted -> call-closed. ABI: void (EAX preserved). */
void free_audio_stream_buffers(void)
{
    if (G32(VA_g_audio_decode_buffer_a) != 0) { uint32_t p = (uint32_t)G32(VA_g_audio_decode_buffer_a); G32(VA_g_audio_decode_buffer_a) = 0; game_heap_free((uint8_t *)(uintptr_t)p); }
    if (G32(VA_g_audio_decode_buffer_b) != 0) { uint32_t p = (uint32_t)G32(VA_g_audio_decode_buffer_b); G32(VA_g_audio_decode_buffer_b) = 0; game_heap_free((uint8_t *)(uintptr_t)p); }
    if (G32(VA_g_audio_silence_buffer) != 0) { uint32_t p = (uint32_t)G32(VA_g_audio_silence_buffer); G32(VA_g_audio_silence_buffer) = 0; game_heap_free((uint8_t *)(uintptr_t)p); }
    uint32_t pool = (uint32_t)G32(VA_g_resource_pool);
    G32(VA_g_resource_pool) = 0;                                    /* xchg [0x85c40],eax(=0) */
    if (pool != 0) game_heap_free((uint8_t *)(uintptr_t)pool);
}

/* ============================================================ A. SFX — Layer 2 mids */

/* resolve_sound_sample 0x27951 (75 B) — sample id -> mixer descriptor slot: out-of-range -> AL=0xff;
 * already loaded (table row byte +0xb != 0xff) -> that slot; else load_sound_sample() and stamp the row.
 * ABI: EAX=id -> EAX (AL = slot | 0xff; upper bytes = the original's leftovers, reproduced). */
uint32_t resolve_sound_sample(uint32_t id)
{
    if ((int32_t)id >= G32(VA_g_sound_sample_count)) return (id & ~0xffu) | 0xff;      /* jl signed */
    uint32_t row = id * 0xc;
    uint8_t slot = ld8((uint32_t)G32(VA_g_sound_sample_table) + row + 0xb);
    if (slot != 0xff) return (id & ~0xffu) | slot;
    uint32_t r = load_sound_sample(id);
    if ((r & 0xff) == 0xff) return (r & ~0xffu) | 0xff;                /* jmp 0x2795c: mov al,0xff */
    st8((uint32_t)G32(VA_g_sound_sample_table) + row + 0xb, (uint8_t)r);
    return r;                                                          /* mov al,dl (same AL) */
}

/* load_sound_sample 0x277db (374 B) — load sample `id` from the open SFX bank [0x848f8] into a fresh
 * resource-pool chunk and claim a voice-descriptor slot. Slot from find_free (evict on exhaustion);
 * sample row = [0x848f4] + id*0xc {+0 file ofs, +4 size, +8 rate?, +0xa flags, +0xb slot}. When
 * !(flags & 0xa) and the low-memory flag 0x89f41 is clear the sample is 2x-upsampled: read the raw
 * bytes into the TOP half of the chunk, interpolate_words into the bottom (both lifted). DOS
 * lseek/read are bridges (oracle stubs them). Descriptor: +0 size, +4 word rate, +6 flags, +7 slot,
 * +8 lock, +9 group, +0xa sample id, +0xc timestamp, +0xe data.
 * ABI: EAX=id -> EAX (AL = slot | 0xff | 0 on bad size; upper bytes reproduced per path). */
uint32_t load_sound_sample(uint32_t id)
{
    uint32_t slot = find_free_voice_descriptor() & 0xff;        /* movzx al */
    if (slot == 0xff) {
        slot = evict_oldest_voice_descriptor() & 0xff;
        if (slot == 0xff) return 0xff;                                 /* al=0xff over zx(al) */
    }
    if ((int32_t)id >= G32(VA_g_sound_sample_count)) return (id & ~0xffu) | 0xff;      /* jge (signed) */
    uint32_t tbl = (uint32_t)G32(VA_g_sound_sample_table);
    uint32_t row = tbl + id * 0xc;
    if (ld32s(row + 4) == 0 || (uint32_t)ld32s(row + 4) > 0x100000u)   /* je / jbe(unsigned) */
        return tbl & ~0xffu;                                           /* xor al,al over eax=tbl */
    uint32_t size2 = ld32u(row + 4);
    uint32_t upsample = 0;
    if (!(ld8(row + 0xa) & 0xa) && G8(VA_g_resource_pool_small_flag) == 0) {
        size2 <<= 1;                                                   /* shl [ebp-4],1 */
        upsample = 1;
    }
    uint32_t buf;
    for (;;) {
        buf = alloc_resource_pool_block((int32_t)(size2 + 0xe));
        if (buf != 0) break;
        if ((evict_oldest_voice_descriptor() & 0xff) == 0xff) return 0xff;
    }
    dos_lseek((uint32_t)G32(VA_g_sound_bank_file_handle), ld32u(row), 0);           /* dos_lseek(handle, ofs, whence 0) (C2) */
    uint32_t dst = buf + 0xe;
    uint32_t rdret;
    uint8_t  fb;                                                       /* descriptor flags byte */
    if (upsample) {
        uint32_t hi = dst + ld32u(row + 4);                            /* raw bytes -> top half */
        rdret = dos_read_items(hi, ld32u(row + 4), 1, (uint32_t)G32(VA_g_sound_bank_file_handle));   /* (C2) */
        if (rdret == 1)
            interpolate_words_43dd8((int16_t *)(uintptr_t)dst, (const int16_t *)(uintptr_t)hi,
                                           (int32_t)(ld32u(row + 4) >> 1));
        st32(buf, size2);                                              /* doubled size */
        fb = (uint8_t)(ld8(row + 0xa) | 2);
    } else {
        rdret = dos_read_items(dst, ld32u(row + 4), 1, (uint32_t)G32(VA_g_sound_bank_file_handle));   /* (C2) */
        st32(buf, ld32u(row + 4));
        fb = ld8(row + 0xa);
    }
    st8(buf + 6, fb);
    if (rdret != 1) {
        uint32_t r = free_resource_chunk((uint8_t *)(uintptr_t)buf);
        return (r & ~0xffu) | 0xff;
    }
    st32((uint32_t)GADDR(VA_g_sound_voice_descriptors) + slot * 4, buf);
    st8(buf + 8, 0);
    st8(buf + 7, (uint8_t)slot);
    st16(buf + 4, ld16(row + 8));
    st16(buf + 0xa, (uint16_t)id);
    st8(buf + 9, (uint8_t)G8(VA_g_pending_sound_param));
    uint16_t ts = (uint16_t)G16(VA_g_das_cache_tick);
    st16(buf + 0xc, ts);
    return (id & 0xffff0000u) | ((uint32_t)ts & 0xff00u) | slot;       /* al=slot over ax=ts over idx */
}

/* evict_oldest_voice_descriptor 0x27a3e (199 B) — pick a descriptor to recycle. Pass 1: the UNLOCKED
 * descriptor with the largest positive 16-bit age (now [0x90c0a] - stamp +0xc); release it. Pass 2 (none):
 * ignore locks, age measured past the grace byte +9 (stamp+grace); stop its playing handles
 * (stop_sounds_for_sample_slot) then release. ABI: void -> EAX (AL = freed slot | 0xff; the both-empty
 * path reproduces the original's scan-pointer remnant upper bytes). Exits via the 0x26f48 epilogue. */
uint32_t evict_oldest_voice_descriptor(void)
{
    uint16_t now = (uint16_t)G16(VA_g_das_cache_tick);
    uint32_t base = (uint32_t)GADDR(VA_g_sound_voice_descriptors);
    int32_t  best = -1;
    int16_t  bestage = 0;
    for (uint32_t i = 0; i < 0x20; i++) {
        uint32_t d = ld32u(base + i * 4);
        if (d == 0) continue;
        if (ld8(d + 8) != 0) continue;
        int16_t age = (int16_t)(now - (uint16_t)ld16(d + 0xc));
        if (age > bestage) { bestage = age; best = (int32_t)i; }       /* cmp cx,di; jle */
    }
    if (best != -1) {
        uint32_t r = release_voice_descriptor((uint32_t)best);
        if (r != 1) return (r & ~0xffu) | 0xff;
        return (r & ~0xffu) | (uint32_t)best;                          /* al=dl over eax=1 */
    }
    best = -1; bestage = 0;
    for (uint32_t i = 0; i < 0x20; i++) {                              /* pass 2: no lock check */
        uint32_t d = ld32u(base + i * 4);
        if (d == 0) continue;
        uint16_t due = (uint16_t)((uint16_t)ld16(d + 0xc) + (uint16_t)ld8(d + 9));
        int16_t  age = (int16_t)(now - due);
        if (age > bestage) { bestage = age; best = (int32_t)i; }
    }
    if (best == -1)
        return ((base + 0x80) & ~0xffu) | 0xff;                        /* both-empty remnant */
    uint32_t d = ld32u(base + (uint32_t)best * 4);
    if (d == 0) return 0xff;                                           /* eax=0; al=0xff */
    stop_sounds_for_sample_slot((uint32_t)best);
    uint32_t r = release_voice_descriptor((uint32_t)best);
    if (r == 1) return (r & ~0xffu) | (uint32_t)best;
    return (r & ~0xffu) | 0xff;
}

/* stop_sounds_for_sample_slot 0x26eaf (83 B) — stop + free every playing handle record (0x83ed4,
 * 16 x 0x9a) whose slot byte +5 == AL, decrementing the descriptor's lock byte per stop. flow_succ:
 * exits through load_sound_effect_bank's epilogue. ABI: EAX (AL=slot) -> EAX = the original's leftover
 * (last non-empty record's +5 byte over the input, or the last sos_stop_voice return) — reproduced. */
uint32_t stop_sounds_for_sample_slot(uint32_t al_in)
{
    uint8_t  key  = (uint8_t)al_in;
    uint32_t desc = ld32u((uint32_t)GADDR(VA_g_sound_voice_descriptors) + ((uint32_t)key << 2));
    uint32_t rec  = (uint32_t)GADDR(VA_g_active_sound_handles);
    uint32_t eax  = al_in;
    for (int i = 0; i < 0x10; i++, rec += 0x9a) {
        if (ld32u(rec) == 0) continue;
        eax = (eax & ~0xffu) | ld8(rec + 5);                           /* mov al,[edx+5] */
        if ((uint8_t)eax != key) continue;
        uint32_t voice = ld32u(rec + 0x18);
        st32(rec, 0);
        eax = sos_stop_voice(voice);
        if (ld8(desc + 8) != 0) st8(desc + 8, (uint8_t)(ld8(desc + 8) - 1));
    }
    return eax;
}

/* stop_sound_handle_voice 0x26d3e (76 B) — find the handle record whose emitter ptr +0 == EAX, stop its
 * voice, unlock its descriptor, clear the record. First match only. ABI: EAX=emitter -> EAX=1 found / 0. */
uint32_t stop_sound_handle_voice(uint32_t emitter)
{
    uint32_t rec = (uint32_t)GADDR(VA_g_active_sound_handles);
    for (int i = 0; i < 0x10; i++, rec += 0x9a) {
        if (ld32u(rec) != emitter) continue;
        sos_stop_voice(ld32u(rec + 0x18));
        uint32_t desc = ld32u((uint32_t)GADDR(VA_g_sound_voice_descriptors) + ((uint32_t)ld8(rec + 5) << 2));
        if (ld8(desc + 8) != 0) st8(desc + 8, (uint8_t)(ld8(desc + 8) - 1));
        st32(rec, 0);
        return 1;
    }
    return 0;
}

/* stop_sound_by_id 0x26d8a (90 B) — same, but matches on the emitter's id word (+6 of the record's
 * emitter/header ptr) against the FULL 32-bit input. ABI: EAX=id -> EAX=1 found / 0. */
uint32_t stop_sound_by_id(uint32_t id)
{
    uint32_t rec = (uint32_t)GADDR(VA_g_active_sound_handles);
    for (int i = 0; i < 0x10; i++, rec += 0x9a) {
        uint32_t hdr = ld32u(rec);
        if (hdr == 0) continue;
        if ((uint32_t)ld16(hdr + 6) != id) continue;                   /* movzx; cmp eax,ecx (full) */
        sos_stop_voice(ld32u(rec + 0x18));
        uint32_t desc = ld32u((uint32_t)GADDR(VA_g_sound_voice_descriptors) + ((uint32_t)ld8(rec + 5) << 2));
        if (ld8(desc + 8) != 0) st8(desc + 8, (uint8_t)(ld8(desc + 8) - 1));
        st32(rec, 0);
        return 1;
    }
    return 0;
}

/* query_sfx_emitters_in_range 0x43b3b (267 B) — scan the SFX node list for ACTIVE (+8 bit7) emitters in
 * range of (posx,posy) (16.16, >>16), collecting {node ptr, dist^2} pairs into the query array and
 * sorting by distance (call-closed into sort_sfx_query_by_distance). Modes (+8 & 7): 0/1 always
 * eligible; 2/3+ tick the 16-bit respawn timer +0xe down by the frame scale [0x85324] — not yet
 * expired -> skip; expired -> reload from period +0xc (mode 2: (7*period)>>1; mode 3+:
 * (7*((period*[0x85328])>>16))>>1, all 16-bit), an ADD-carry keeps the sum else clamps 0 — then
 * eligible this pass. Budget [0x911dc] = q max (+4), found count in [0x911e0] + word q+6.
 * ABI: EAX=q {+0 array, +4 u16 max, +6 u16 count-out}, EDX=posx, EBX=posy -> EAX = found count. */
uint32_t query_sfx_emitters_in_range(uint32_t q, uint32_t posx, uint32_t posy)
{
    G32(VA_g_sfx_query_capacity) = (int32_t)(uint32_t)ld16(q + 4);
    G32(VA_g_sfx_query_result_count) = 0;
    uint32_t out  = ld32u(q);
    int32_t  bx32 = (int32_t)posx >> 16;
    int32_t  cx32 = (int32_t)posy >> 16;
    uint32_t list = (uint32_t)G32(VA_g_sfx_nodes);
    int32_t  n    = (int32_t)ld16(list + 2);
    uint32_t node = list + 4;
    for (int32_t i = 0; i < n; i++, node += 0x12) {
        uint8_t fl = ld8(node + 8);
        if (!(fl & 0x80)) continue;
        uint8_t mode = (uint8_t)(fl & 7);
        if (mode >= 2) {
            uint16_t t  = ld16(node + 0xe);
            uint16_t fs = (uint16_t)G16(VA_g_frame_time_scale);
            st16(node + 0xe, (uint16_t)(t - fs));
            if (t >= fs) continue;                       /* jae — not expired yet */
            uint16_t period = ld16(node + 0xc);
            uint16_t add;
            if (mode == 2)
                add = (uint16_t)((uint16_t)(period * 7) >> 1);
            else                                         /* mode >= 3: scale by [0x85328] first */
                add = (uint16_t)((uint16_t)(7 * (((uint32_t)period * (uint32_t)G16(VA_g_frame_time_scale + 0x4)) >> 16)) >> 1);
            uint32_t sum = (uint32_t)(uint16_t)ld16(node + 0xe) + add;
            st16(node + 0xe, (uint16_t)sum);
            if (sum <= 0xffffu)                          /* no carry -> clamp to 0 */
                st16(node + 0xe, 0);
        }
        int16_t dxv = (int16_t)(uint16_t)((uint16_t)bx32 - ld16(node));
        int16_t dyv = (int16_t)(uint16_t)((uint16_t)cx32 - ld16(node + 2));
        int32_t d2  = (int32_t)dyv * dyv + (int32_t)dxv * dxv;
        uint32_t rng = (uint32_t)ld16(node + 0xa);
        if ((uint32_t)d2 > rng * rng) continue;          /* ja — unsigned */
        st32(out, node); st32(out + 4, (uint32_t)d2); out += 8;
        G32(VA_g_sfx_query_result_count) = G32(VA_g_sfx_query_result_count) + 1;
        G32(VA_g_sfx_query_capacity) = G32(VA_g_sfx_query_capacity) - 1;
        if (G32(VA_g_sfx_query_capacity) == 0) break;
    }
    st16(q + 6, (uint16_t)G32(VA_g_sfx_query_result_count));
    sort_sfx_query_by_distance(q);
    return (uint32_t)G32(VA_g_sfx_query_result_count);
}

/* ============================================================ A. SFX — voice starters (submit to the
 * host driver via the lifted wrappers; oracle stubs the driver entry, in-game = live host mixer) */

/* start_sound_voice_vol 0x275cc (214 B) — fill the record's embedded SOS voice struct (+0x26) from its
 * descriptor (via slot byte +5) and submit with an EXPLICIT volume. Voice: +0 data ptr, +4 DS, +8 size,
 * +0xc 0, +0x10 2, +0x14 vol, +0x18 userdata, +0x1c/+0x20 far callback (trampoline 0x27501/CS),
 * +0x28 flags (base 0x300; loop 0x80/one-shot 0x20 from desc flags bit0), +0x38 rate override
 * (0x20000/0x8000 by desc flags bit1 vs driver id [0x71140]=='V"' 0x5622, flag +0x29 bit2), +0x40 EBX-in.
 * Stamps desc: lock++, timestamp, group. ABI: EAX=rec, EDX=vol, ECX=userdata, EBX=[v+0x40] -> EAX =
 * submit return (stored to rec+0x18), or rec+0x26 on a bad descriptor size. */
uint32_t start_sound_voice_vol(uint32_t rec, uint32_t vol, uint32_t userdata, uint32_t ebx_in)
{
    uint32_t desc = ld32u((uint32_t)GADDR(VA_g_sound_voice_descriptors) + ((uint32_t)ld8(rec + 5) << 2));
    uint32_t data = desc + 0xe;
    uint32_t v    = rec + 0x26;
    if (ld32s(desc) == 0 || ld32s(desc) > 0x100000) return v;          /* je / jg (signed) */
    st32(v + 0xc, 0);
    st32(v + 0x10, 2);
    st32(v + 0x28, 0x300);
    st32(v + 8, ld32u(desc));
    st16(v + 4, cur_ds());
    st32(v + 0x40, ebx_in);
    st32(v + 0, data);
    st32(v + 0x18, userdata);
    if (ld8(desc + 6) & 1) st8(v + 0x28, (uint8_t)(ld8(v + 0x28) | 0x80));
    else                   st8(v + 0x28, (uint8_t)(ld8(v + 0x28) | 0x20));
    if (ld8(desc + 6) & 2) {
        if (G32(VA_g_font_descriptor + 0x22e) != 0x5622) { st32(v + 0x38, 0x20000); st8(v + 0x29, (uint8_t)(ld8(v + 0x29) | 4)); }
    } else {
        if (G32(VA_g_font_descriptor + 0x22e) == 0x5622) { st32(v + 0x38, 0x8000);  st8(v + 0x29, (uint8_t)(ld8(v + 0x29) | 4)); }
    }
    st32(v + 0x14, vol);
    st16(rec + 0x10, (uint16_t)vol);
    st8(desc + 8, (uint8_t)(ld8(desc + 8) + 1));
    st16(desc + 0xc, (uint16_t)G16(VA_g_das_cache_tick));
    st8(desc + 9, (uint8_t)G8(VA_g_pending_sound_param));
    uint32_t r = sos_submit_voice(v, (uint32_t)GADDR(0x27501), cur_cs());
    st32(rec + 0x18, r);
    return r;
}

/* start_sound_voice 0x276a2 (235 B) — as above but base flags 0, no +0x40 write, and the volume is
 * implicit: 0x7fff, or the SFX master [0x71d84] when it is below 0x7ff0 (then +0x29 bit0). flow_succ:
 * exits through play_sound_unique's epilogue. ABI: EAX=rec, EDX=userdata -> EAX = submit return /
 * rec+0x26 on bad size. */
uint32_t start_sound_voice(uint32_t rec, uint32_t userdata)
{
    uint32_t desc = ld32u((uint32_t)GADDR(VA_g_sound_voice_descriptors) + ((uint32_t)ld8(rec + 5) << 2));
    uint32_t data = desc + 0xe;
    uint32_t v    = rec + 0x26;
    uint32_t vol  = 0x7fff;
    if (ld32s(desc) == 0 || ld32s(desc) > 0x100000) return v;
    st32(v + 0xc, 0);
    st32(v + 0x10, 2);
    st32(v + 0x28, 0);
    st32(v + 8, ld32u(desc));
    st16(v + 4, cur_ds());
    st32(v + 0x18, userdata);
    st32(v + 0, data);
    if (ld8(desc + 6) & 1) st8(v + 0x28, (uint8_t)(ld8(v + 0x28) | 0x80));
    else                   st8(v + 0x28, (uint8_t)(ld8(v + 0x28) | 0x20));
    if (ld8(desc + 6) & 2) {
        if (G32(VA_g_font_descriptor + 0x22e) != 0x5622) { st32(v + 0x38, 0x20000); st8(v + 0x29, (uint8_t)(ld8(v + 0x29) | 4)); }
    } else {
        if (G32(VA_g_font_descriptor + 0x22e) == 0x5622) { st32(v + 0x38, 0x8000);  st8(v + 0x29, (uint8_t)(ld8(v + 0x29) | 4)); }
    }
    if ((uint32_t)G32(VA_g_test_sfx_descriptor + 0x3a) < 0x7ff0u) {              /* jae skips */
        vol = (uint32_t)G32(VA_g_test_sfx_descriptor + 0x3a);
        st8(v + 0x29, (uint8_t)(ld8(v + 0x29) | 1));
    }
    st32(v + 0x14, vol);
    st16(rec + 0x10, (uint16_t)vol);
    st8(desc + 9, (uint8_t)G8(VA_g_pending_sound_param));
    st16(desc + 0xc, (uint16_t)G16(VA_g_das_cache_tick));
    st8(desc + 8, (uint8_t)(ld8(desc + 8) + 1));
    uint32_t r = sos_submit_voice(v, (uint32_t)GADDR(0x27501), cur_cs());
    st32(rec + 0x18, r);
    return r;
}

/* ============================================================ A. SFX — the play-entry tower
 * (flow_succ multi-entry: the bodies are defined ONCE as statics, the entries stage the group byte
 * 0x84906 + the static emitter and dispatch — mirroring the original's shared fall-through bodies) */

/* the 0x27283 body shared by play_sound_effect (group 0x6e) + start_persistent_looping_sound (0x20) */
static uint32_t au_play_effect_body(uint32_t id_in, uint32_t param)
{
    if (G32(VA_g_sound_enabled) == 0) return 0;
    if (G32(VA_g_test_sfx_descriptor + 0x3a) == 0) return 0;
    uint32_t idx = find_free_slot_83ed4();
    uint32_t id  = id_in & 0x7fff;
    if (idx == 0xffffffffu) return 0;
    uint32_t rec = (uint32_t)GADDR(VA_g_active_sound_handles) + idx * 0x9a;
    st32(rec, (uint32_t)GADDR(VA_g_pending_sound_param + 0x1));                 /* the static "effect" emitter */
    G16(VA_g_pending_sound_param + 0x5) = (uint16_t)id;                         /* its id word (+4) */
    st32(rec + 8, 0);
    st8(rec + 4, 0x20);
    st8(rec + 6, 0);
    st32(rec + 0x22, param);
    uint32_t slot = resolve_sound_sample(id) & 0xff;
    if (slot == 0xff) { st32(rec, 0); return 0; }
    st8(rec + 5, (uint8_t)slot);
    start_sound_voice(rec, idx);                  /* EDX leftover = find_free index */
    return rec;
}

/* play_sound_effect 0x27270 (167 B) — global one-shot (no position): group byte 0x6e, the static
 * emitter 0x84907. ABI: EAX=id(|flags), EDX=param -> EAX = record | 0. */
uint32_t play_sound_effect(uint32_t id, uint32_t param)
{
    if (G32(VA_g_sound_enabled) == 0) return 0;
    G8(VA_g_pending_sound_param) = 0x6e;
    return au_play_effect_body(id, param);
}

/* start_persistent_looping_sound 0x271fb (12 B) — group 0x20, then the 0x27283 body (the body re-checks
 * the gates). ABI: EAX=id, EDX=param -> EAX = record | 0. */
uint32_t start_persistent_looping_sound(uint32_t id, uint32_t param)
{
    G8(VA_g_pending_sound_param) = 0x20;
    return au_play_effect_body(id, param);
}

/* play_object_sound 0x270ca (250 B) — positional one-shot bound to a caller-supplied emitter object
 * (STACK arg; its id word +4 is stamped). Record: coords +0x14/+0x16, flags 0x80 (own coords), dist^2
 * from the player origin (x vs [0x90a8c]>>16, y vs [0x90a94]>>16), volume via compute_sound_volume_pan,
 * pan via project_point_to_screen_column (y vs [0x90a90]>>16, x vs [0x90a8c]>>16, 16-bit deltas).
 * ABI: EAX=id(|flags), EDX=param, EBX=x, ECX=y, stack[0]=emitter -> EAX = record | 0. ret 4. */
uint32_t play_object_sound(uint32_t id_in, uint32_t param, uint32_t ebx_x, uint32_t ecx_y,
                                  uint32_t emitter)
{
    uint32_t idx = find_free_slot_83ed4();
    uint32_t id  = id_in & 0x7fff;
    if (idx == 0xffffffffu) return 0;
    uint32_t rec = (uint32_t)GADDR(VA_g_active_sound_handles) + idx * 0x9a;
    st32(rec, emitter);
    st16(emitter + 4, (uint16_t)id);
    st32(rec + 8, 0);
    st8(rec + 4, 0x80);
    st16(rec + 0x14, (uint16_t)ebx_x);
    st16(rec + 0x16, (uint16_t)ecx_y);
    int32_t py16 = G32(VA_g_player_angle + 0x2) >> 16;
    int32_t x    = (int32_t)(int16_t)(uint16_t)ebx_x;
    int32_t y    = (int32_t)(int16_t)(uint16_t)ecx_y;
    int32_t px16 = G32(VA_g_player_z + 0x2) >> 16;
    int32_t d2   = (y - px16) * (y - px16) + (x - py16) * (x - py16);
    st8(rec + 6, 0);
    st32(rec + 0x22, param);
    st32(rec + 8, (uint32_t)d2);
    int32_t vol = compute_sound_volume_pan(rec);
    if (vol == 0) { st32(rec, 0); return 0; }
    uint32_t slot = resolve_sound_sample(id) & 0xff;
    if (slot == 0xff) { st32(rec, 0); return 0; }
    int32_t dy2 = y - (G32(VA_g_player_x + 0x2) >> 16);
    int32_t dx2 = x - (G32(VA_g_player_angle + 0x2) >> 16);
    uint32_t pan = project_point_to_screen_column((int16_t)dx2, (int16_t)dy2, ld32u(rec + 0xc));
    st8(rec + 5, (uint8_t)slot);
    start_sound_voice_vol(rec, (uint32_t)vol, idx, pan);        /* ECX leftover = index */
    return rec;
}

/* play_object_sound_thunk 0x271e2 (6 B) — the shared `call play_object_sound; ret` tail. */
uint32_t play_object_sound_thunk(uint32_t id, uint32_t param, uint32_t ebx_x, uint32_t ecx_y,
                                        uint32_t emitter)
{
    return play_object_sound(id, param, ebx_x, ecx_y, emitter);
}

/* the 0x271d5 body: sign-extend the 16-bit coords, group 0x20, play via the shared emitter */
static uint32_t au_play_entity_body(uint32_t id, uint32_t param, uint32_t bx, uint32_t cx, uint32_t emitter)
{
    int32_t x = (int32_t)(int16_t)(uint16_t)bx;          /* movsx ebx,bx */
    int32_t y = (int32_t)(int16_t)(uint16_t)cx;          /* movsx ecx,cx */
    G8(VA_g_pending_sound_param) = 0x20;
    return play_object_sound(id, param, (uint32_t)x, (uint32_t)y, emitter);
}

/* play_entity_sound 0x271c4 (30 B) — the hottest external entry: entity SFX via static emitter 0x83eb0.
 * ABI: EAX=id, EDX=param, BX=x, CX=y -> EAX = record | 0. */
uint32_t play_entity_sound(uint32_t id, uint32_t param, uint32_t bx, uint32_t cx)
{
    if (G32(VA_g_sound_enabled) == 0) return 0;
    return au_play_entity_body(id, param, bx, cx, (uint32_t)GADDR(VA_g_entity_sound_emitter));
}

/* play_entity_object_sound 0x271e8 (19 B) — same body, static emitter 0x83ec2. */
uint32_t play_entity_object_sound(uint32_t id, uint32_t param, uint32_t bx, uint32_t cx)
{
    if (G32(VA_g_sound_enabled) == 0) return 0;
    return au_play_entity_body(id, param, bx, cx, (uint32_t)GADDR(VA_g_entity_sound_emitter + 0x12));
}

/* play_command_sound 0x2730b (35 B) — RAW/dbase command SFX: group 0x64, emitter 0x83eb0, via the
 * call thunk. ABI: EAX=id, EDX=param, BX=x, CX=y -> EAX = record | 0. */
uint32_t play_command_sound(uint32_t id, uint32_t param, uint32_t bx, uint32_t cx)
{
    if (G32(VA_g_sound_enabled) == 0) return 0;
    int32_t x = (int32_t)(int16_t)(uint16_t)bx;
    int32_t y = (int32_t)(int16_t)(uint16_t)cx;
    G8(VA_g_pending_sound_param) = 0x64;
    return play_object_sound_thunk(id, param, (uint32_t)x, (uint32_t)y, (uint32_t)GADDR(VA_g_entity_sound_emitter));
}

/* play_sound_unique 0x273f0 (273 B) — play an SFX-node emitter (query entry {hdr, dist^2}) unless its
 * header is already playing (is_in_83ed4_table). Mode = hdr flags & 7 -> rec+4; zone side = hdr+9 ->
 * rec+6. Zero volume: a mode-0 node is DEACTIVATED (hdr+8 bit7 cleared) and the record dropped. Mode 1
 * jitters the volume by clamp_symmetric(vol, 0, ((0x7ff - range) clamped >=0x10) << 4). Pan from the
 * node position. After a successful start a mode-0 node is deactivated (one-shot fired).
 * ABI: EAX=entry {+0 hdr, +4 dist^2}, EDX=param -> EAX = record | 0. */
uint32_t play_sound_unique(uint32_t entry, uint32_t param)
{
    if (G32(VA_g_sound_bank_file_handle) == 0) return 0;
    uint32_t hdr = ld32u(entry);
    if (is_in_83ed4_table(hdr) != 0) return 0;
    uint32_t idx = find_free_slot_83ed4();
    if (idx == 0xffffffffu) return 0;
    uint32_t rec = (uint32_t)GADDR(VA_g_active_sound_handles) + idx * 0x9a;
    st32(rec, hdr);
    st32(rec + 8, ld32u(entry + 4));
    st8(rec + 4, (uint8_t)(ld8(hdr + 8) & 7));
    st8(rec + 6, ld8(hdr + 9));
    st32(rec + 0x22, param);
    int32_t vol = compute_sound_volume_pan(rec);
    uint8_t mode = (uint8_t)(ld8(rec + 4) & 7);
    if (vol == 0) {
        if (mode == 0) { uint32_t h = ld32u(rec); st8(h + 8, (uint8_t)(ld8(h + 8) & 0x7f)); }
        st32(rec, 0);
        return 0;
    }
    st16(rec + 0x12, (uint16_t)vol);
    int32_t evol = vol;
    if (mode == 1) {
        int32_t e = 0x7ff - (int32_t)ld16(hdr + 0xa);
        if (e < 0x10) e = 0x10;
        evol = clamp_symmetric_26f2d(vol, 0, e << 4);
    }
    uint32_t pan  = compute_sound_pan_from_position(hdr, ld32u(rec + 0xc));
    uint32_t slot = resolve_sound_sample((uint32_t)ld16(hdr + 4)) & 0xff;
    if (slot == 0xff) { st32(rec, 0); return 0; }
    st8(rec + 5, (uint8_t)slot);
    start_sound_voice_vol(rec, (uint32_t)evol, idx, pan);
    if (mode == 0) { uint32_t h = ld32u(rec); st8(h + 8, (uint8_t)(ld8(h + 8) & 0x7f)); }
    return rec;
}

/* the 0x2722f body shared by play_world_sound_at_pos (group 0x20) / _squared_dist (0x6e): dist^2 from
 * the player origin, then play_sound_unique over a stack-built {pos, d2} entry. */
static uint32_t au_play_world_body(uint32_t pos, uint32_t param)
{
    int32_t dx = (int32_t)(int16_t)ld16(pos)     - (G32(VA_g_player_angle + 0x2) >> 16);
    int32_t dy = (int32_t)(int16_t)ld16(pos + 2) - (G32(VA_g_player_z + 0x2) >> 16);
    struct { uint32_t p; int32_t d2; } e;
    e.p  = pos;
    e.d2 = dx * dx + dy * dy;
    return play_sound_unique((uint32_t)(uintptr_t)&e, param);
}

/* play_world_sound_at_pos 0x27207 (21 B) — ABI: EAX=pos rec (node-header layout), EDX=param. */
uint32_t play_world_sound_at_pos(uint32_t pos, uint32_t param)
{
    if (G32(VA_g_sound_enabled) == 0) return 0;
    G8(VA_g_pending_sound_param) = 0x20;
    return au_play_world_body(pos, param);
}

/* play_world_sound_squared_dist 0x2721c (84 B) — same body, group 0x6e. */
uint32_t play_world_sound_squared_dist(uint32_t pos, uint32_t param)
{
    if (G32(VA_g_sound_enabled) == 0) return 0;
    G8(VA_g_pending_sound_param) = 0x6e;
    return au_play_world_body(pos, param);
}

/* play_sound_sequence_group 0x27080 (62 B) — play every entry of a query result (8-byte stride),
 * group byte counting up from 0. Gate: SFX bank open. ABI: EAX=q {+0 array, +6 i16 count} -> void. */
void play_sound_sequence_group(uint32_t q)
{
    G8(VA_g_pending_sound_param) = 0;
    if (G32(VA_g_sound_bank_file_handle) == 0) return;
    int32_t n = (int32_t)(int16_t)ld16(q + 6);           /* movsx */
    if (n == 0) return;
    uint32_t e = ld32u(q);
    for (int32_t i = 0; i < n; i++, e += 8) {
        play_sound_unique(e, 0);
        G8(VA_g_pending_sound_param) = (uint8_t)(G8(VA_g_pending_sound_param) + 1);
    }
}

/* play_nearby_sfx_emitters 0x151c9 (70 B) — per-tick emitter pump: query 32 nearest active emitters
 * around the player and play them as a group. Locals mirror the original's stack query. ABI: void. */
void play_nearby_sfx_emitters(void)
{
    uint8_t arr[0x100];
    uint8_t qbuf[8];
    zero_memory(qbuf, 8);
    *(uint32_t *)qbuf       = (uint32_t)(uintptr_t)arr;
    *(uint16_t *)(qbuf + 4) = 0x20;
    if (query_sfx_emitters_in_range((uint32_t)(uintptr_t)qbuf,
                                           (uint32_t)G32(VA_g_player_angle + 0x2), (uint32_t)G32(VA_g_player_z + 0x2)) != 0)
        play_sound_sequence_group((uint32_t)(uintptr_t)qbuf);
}

/* restart_door_open_sound 0x3d8f2 (77 B) — doors: stop the previous door sound (id word +0x1a) if one
 * is latched (+0x1e), then (re)play the door's positional sound rec at +0x14 (id = +0x26 - 1, param =
 * +0x1a word of the pos rec, range forced 0x3e8) and flip the door state bits (+2: clear 2, set 4).
 * ABI: EAX=door rec -> void (EAX preserved). */
void restart_door_open_sound(uint32_t door)
{
    if (ld16(door + 0x1e) != 0)
        stop_sound_by_id((uint32_t)ld16(door + 0x1a));
    st16(door + 0x1e, 0);
    if (ld16(door + 0x26) != 0) {
        uint32_t pos = door + 0x14;
        st16(pos + 4, (uint16_t)(ld16(door + 0x26) - 1));
        uint32_t param = (uint32_t)ld16(pos + 6);
        st16(pos + 0xa, 0x3e8);
        play_world_sound_at_pos(pos, param);
    }
    st8(door + 2, (uint8_t)(ld8(door + 2) & 0xfd));
    st8(door + 2, (uint8_t)(ld8(door + 2) | 4));
}

/* play_distance_variant_sound 0x4269b (97 B) — entity sound with distance-picked variant: variant ids
 * (u16, 1-based, 0=none) at rec+0x24, count at +0x22. Multi-variant pick: beyond threshold +8 (or twice
 * it) -> the LAST variant; near -> a scaled pick from the RNG dword [0x7272c] (read-only — D1: stage
 * it). Then play_entity_sound at the coords (or start_persistent_looping_sound if coords==0).
 * ABI: EAX=coords ptr|0, EBX=variant rec, ECX=dist^2 -> void (regs preserved). */
void play_distance_variant_sound(uint32_t coords, uint32_t tab, uint32_t d2)
{
    uint32_t count = (uint32_t)ld16(tab + 0x22);
    if (count == 0) return;
    uint32_t vp = tab + 0x24;
    if (count != 1) {
        uint32_t idx = count - 1;
        if (!(d2 > ld32u(tab + 8))) {                    /* ja — unsigned */
            if (!(d2 * 2 > ld32u(tab + 8)))
                idx = (idx * (uint32_t)G32(VA_g_entity_damage_rng)) >> 16;
        }
        vp += idx * 2;
    }
    uint32_t id = (uint32_t)ld16(vp);
    if (id == 0) return;
    id--;
    if (coords != 0)
        play_entity_sound(id, 0, (uint32_t)ld16(coords), (uint32_t)ld16(coords + 2));
    else
        start_persistent_looping_sound(id, 0);
}

/* registry shim: ABI_VOID4 hands (EAX,EDX,EBX,ECX); the fn's Watcom args are (EAX,EBX,ECX). */
void play_distance_variant_sound_regs(uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx)
{
    (void)edx;
    play_distance_variant_sound(eax, ebx, ecx);
}

/* update_active_sounds 0x27b05 (265 B) — the per-frame SFX updater (from setup_frame_render_context):
 * for every live, non-static handle record re-derive dist^2 from the player origin; when it moved
 * enough (|delta| >> 4) recompute the volume (compute_sound_volume_pan); when the target volume differs
 * from the applied one slew it by max 0x200 (clamp_diff_200) and push it to the driver (set_w32);
 * re-project the pan and push when changed (|delta| >> 8, set_w54, cached at rec+0x66).
 * ABI: void. flow_succ exit via the 0x26f48 epilogue. */
void update_active_sounds(void)
{
    int32_t py16 = G32(VA_g_player_angle + 0x2) >> 16;                   /* [ebp-0xc] */
    int32_t px16 = G32(VA_g_player_z + 0x2) >> 16;                   /* [ebp-8]  */
    uint32_t rec = (uint32_t)GADDR(VA_g_active_sound_handles);
    for (int n = 0; n < 0x10; n++, rec += 0x9a) {
        if (ld32u(rec) == 0) continue;
        if (ld8(rec + 4) & 0x20) continue;
        uint32_t vol    = (uint32_t)ld16(rec + 0x12);
        uint32_t oldvol = (uint32_t)ld16(rec + 0x10);
        int32_t x, y;
        if (ld8(rec + 4) & 0x80) { y = (int32_t)(int16_t)ld16(rec + 0x16); x = (int32_t)(int16_t)ld16(rec + 0x14); }
        else { uint32_t p = ld32u(rec); x = (int32_t)(int16_t)ld16(p); y = (int32_t)(int16_t)ld16(p + 2); }
        int32_t rel_y = y - px16;                        /* [ebp-0x10] */
        int32_t rel_x = x - py16;                        /* edi */
        int32_t d2    = rel_x * rel_x + rel_y * rel_y;
        int32_t delta = ld32s(rec + 8) - d2;
        if ((au_abs(delta) >> 4) != 0) {
            vol = (uint32_t)compute_sound_volume_pan(rec);
            st16(rec + 0x12, (uint16_t)vol);
            st32(rec + 8, (uint32_t)d2);
        }
        if (vol != oldvol) {
            st16(rec + 0x12, (uint16_t)vol);
            uint32_t applied = (uint32_t)clamp_diff_200((int32_t)vol, (int32_t)oldvol);
            st16(rec + 0x10, (uint16_t)applied);
            sos_voice_set_w32(ld32u(rec + 0x18), applied);
        }
        uint32_t pan = project_point_to_screen_column((int16_t)rel_x, (int16_t)rel_y,
                                                             ld32u(rec + 0xc));
        int32_t pdelta = ld32s(rec + 0x66) - (int32_t)pan;
        if ((au_abs(pdelta) >> 8) != 0) {
            st32(rec + 0x66, pan);
            sos_voice_set_w54(ld32u(rec + 0x18), pan);
        }
    }
}

/* ============================================================ A. SFX — bank load / reload wrappers */

/* load_sound_effect_bank 0x26b66 (294 B) — (re)load the SFX bank file: reset the two built-in sample
 * headers (0x83eba/0x83ecc clusters), free any previous bank (free_resource_buffers [L]), ensure the
 * stream buffers/pool (alloc_audio_stream_buffers), open the file ("SFX0" magic), heap-alloc + read the
 * sample table ([0x848f4], count [0x848fc] = size/12), mark every row's slot byte +0xb free (0xff).
 * DOS open/read/lseek/close are bridges. Gate [0x7f550]; open failure clears it. ABI: EAX=filename ->
 * EAX = table ptr | 0. Fail paths verified in the oracle (DOS stubs); the success path is in-game tier
 * (real file I/O). */
uint32_t load_sound_effect_bank(uint32_t name)
{
    if (G32(VA_g_sound_enabled) == 0) return 0;
    G16(VA_g_entity_sound_emitter + 0x1c) = 0x3e8; G8(VA_g_entity_sound_emitter + 0x22) = 0x40;
    G16(VA_g_entity_sound_emitter + 0xa) = 0x7d0; G8(VA_g_entity_sound_emitter + 0x10) = 0x40;
    if (G32(VA_g_sound_bank_file_handle) != 0) free_resource_buffers();
    alloc_audio_stream_buffers();
    uint32_t h = dos_open_file(name, 0);          /* dos_open_file(name, mode 0) (C2) */
    G32(VA_g_sound_bank_file_handle) = (int32_t)h;
    if (h == 0) { G32(VA_g_sound_enabled) = 0; return 0; }
    uint8_t hdr[0x1c];
    dos_read_items((uint32_t)(uintptr_t)hdr, 1, 0x1c, h);       /* read header (C2) */
    if (*(uint32_t *)hdr != 0x53465830u) {               /* "0XFS" */
        dos_close_handle((uint32_t)G32(VA_g_sound_bank_file_handle));              /* (C2) */
        G32(VA_g_sound_bank_file_handle) = 0;
        return 0;
    }
    uint32_t tsize = *(uint32_t *)(hdr + 0x10);          /* [ebp-0xc] */
    uint32_t tbl   = game_heap_alloc((int32_t)tsize);
    G32(VA_g_sound_sample_table) = (int32_t)tbl;
    if (tbl == 0) {
        dos_close_handle((uint32_t)G32(VA_g_sound_bank_file_handle));              /* (C2) */
        G32(VA_g_sound_bank_file_handle) = 0;
        G32(VA_g_sound_enabled) = 0;
        return 0;
    }
    G32(VA_g_sound_sample_count) = (int32_t)(tsize / 0xcu);              /* div (unsigned) */
    dos_lseek((uint32_t)G32(VA_g_sound_bank_file_handle), *(uint32_t *)(hdr + 8), 0);        /* lseek (C2) */
    dos_read_items(tbl, 1, tsize, (uint32_t)G32(VA_g_sound_bank_file_handle));               /* read table (C2) */
    uint32_t p = tbl;
    for (int32_t i = 0; i < G32(VA_g_sound_sample_count); i++) {
        p += 0xc;
        st8(p - 1, 0xff);                                /* row i's +0xb */
    }
    return (uint32_t)G32(VA_g_sound_sample_table);
}

/* reload_sfx_bank_if_pending 0x15805 (14 B) — gate [0x7f548]: clear -> no-op (noop_ret_stub_15804),
 * set -> tail-jmp load_sound_effect_bank. ABI: EAX=filename -> EAX (bank ret, or the untouched input). */
uint32_t reload_sfx_bank_if_pending(uint32_t name)
{
    if (G32(VA_g_sos_digital_device + 0x6c) == 0) return name;
    return load_sound_effect_bank(name);
}

/* load_sfx_file_wrapper 0x10c51 (31 B) — build the full game path for the SFX bank name (template
 * 0x763b0) into a stack buffer and reload-if-pending. ABI: EAX=filename -> EAX (reload ret). */
uint32_t load_sfx_file_wrapper(uint32_t name)
{
    uint8_t buf[0x78];
    build_game_path(buf, (const uint8_t *)(uintptr_t)GADDR(VA_g_dir_data),
                           (const uint8_t *)(uintptr_t)name);
    return reload_sfx_bank_if_pending((uint32_t)(uintptr_t)buf);
}

/* ============================================================ B. voice / speech streaming
 * (the dbase500.dat dialogue-clip streamer: 2-buffer ping-pong [0x8201c]/[0x82020] refilled per frame,
 * with the SOS completion callback queueing the NEXT buffer into the driver slot). State globals:
 * file [0x82010], play state [0x8200c] (0 idle/1 playing/2 ended), bytes left
 * [0x82018], counts [0x82024]/[0x82028], refill-pending [0x820b9], DPCM flag [0x82034] + predictor
 * [0x82038] (D1: carried across blocks), rate [0x82041], the STATIC submit voice struct at 0x82045. */

/* decode_voice_block 0x1e3df (168 B) — read the next block of the open clip into `dst`: PCM reads
 * min(remaining, maxlen) raw bytes; DPCM reads min(remaining, maxlen/2) raw bytes into the TOP half
 * (dst + maxlen/2) and decodes them to s16 at dst (decode_dpcm_block [L], predictor [0x82038] carried).
 * Gate quirks: null dst / zero remaining / closed file / [0x83e94] set / rate < 0x80 -> 0.
 * ABI: EAX=dst, EDX=remaining, EBX=maxlen -> EAX = RAW bytes consumed (readers' return). */
uint32_t decode_voice_block(uint32_t dst, uint32_t remaining, uint32_t maxlen)
{
    if (dst == 0 || remaining == 0 || G32(VA_g_voice_file_handle) == 0) return 0;
    if (G32(VA_g_voice_decode_suspended) != 0) return 0;                     /* xor eax,ebx (eax==ebx) = 0 */
    if ((uint32_t)G32(VA_g_voice_sample_rate) < 0x80u) return 0;        /* jb */
    if (G32(VA_g_voice_is_dpcm) != 0) {                             /* DPCM */
        uint32_t budget = maxlen >> 1;
        uint32_t len = remaining;
        if (len > budget) len = budget;                  /* jbe */
        uint32_t raw = dst + budget;
        uint32_t rd  = dos_read_items(raw, 1, len, (uint32_t)G32(VA_g_voice_file_handle));   /* (C2) */
        uint32_t pred = decode_dpcm_block((uint32_t)G32(VA_g_voice_dpcm_predictor),
                                                 (const uint8_t *)(uintptr_t)raw,
                                                 (int16_t *)(uintptr_t)dst, (int32_t)rd);
        G32(VA_g_voice_dpcm_predictor) = (int32_t)pred;
        return rd;
    }
    uint32_t len = remaining;
    if (len > maxlen) len = maxlen;
    return dos_read_items(dst, 1, len, (uint32_t)G32(VA_g_voice_file_handle));   /* (C2) */
}

/* close_voice_file 0x1e774 (30 B) — close [0x82010] if open. ABI: void. */
void close_voice_file(void)
{
    if (G32(VA_g_voice_file_handle) != 0) {
        dos_close_handle((uint32_t)G32(VA_g_voice_file_handle));   /* (C2) */
        G32(VA_g_voice_file_handle) = 0;
    }
}

/* install_voice_sos_callback 0x27c0e (55 B) — save the voice's own far callback (+0x1c/+0x20) into
 * g_voice_sos_callback 0x84900/0x84904, tag the userdata (low byte + 0xed00), then submit with the
 * shared trampoline 0x27501/CS as the driver-visible callback. ABI: EAX=voice -> EAX = submit ret. */
uint32_t install_voice_sos_callback(uint32_t voice)
{
    G16(VA_g_voice_sos_callback + 0x4) = ld16(voice + 0x20);
    G32(VA_g_voice_sos_callback) = (int32_t)ld32u(voice + 0x1c);
    uint32_t tag = (ld32u(voice + 0x18) & 0xff) + 0xed00;
    st32(voice + 0x18, tag);
    return sos_submit_voice(voice, (uint32_t)GADDR(0x27501), cur_cs());
}

/* voice_stream_sos_callback 0x1e487 (198 B) — the ISR-side FAR (retf) completion callback the host
 * driver fires per buffer: type 2 = end-of-stream ([0x8200c]=2), type 1 ignored, type 0 = buffer
 * drained -> queue the NEXT buffer (or 0x2000 bytes of the silence scratch [0x85494] on an underrun,
 * i.e. refill still pending) into the driver slot via sos_voice_set_callback (EBX = &0x82045). A full
 * next-buffer (0x8000 decoded) means more to come; a short one was the last ([0x82028]=0).
 * ABI: FAR stack args (voice, type, arg3) -> void; a live-swap ROOT (the host fires it). */
void voice_stream_sos_callback(uint32_t voice, uint32_t type, uint32_t arg3)
{
    if (type > 0) {                                      /* jbe 0x1e49e */
        if (type == 2) G32(VA_g_voice_stream_state) = 2;
        return;                                          /* type 1: ignored */
    }
    if (G32(VA_g_voice_file_handle) == 0) return;
    if (G32(VA_g_voice_bytes_remaining + 0x10) == 0) return;
    if (G32(VA_g_voice_refill_pending) == 0) {
        uint32_t sz = (uint32_t)G32(VA_g_voice_bytes_remaining + 0x10);
        if (G32(VA_g_voice_is_dpcm) != 0) sz += sz;                 /* DPCM: decoded = raw * 2 */
        G32(VA_g_voice_sample_rate + 0xc) = (int32_t)sz;
        G32(VA_g_voice_sample_rate + 0x4) = G32(VA_g_voice_bytes_remaining + 0x8);                     /* the next (back) buffer */
        G16(VA_g_voice_sample_rate + 0x8) = cur_ds();
        sos_voice_set_callback(voice, arg3, (uint32_t)GADDR(VA_g_voice_sample_rate + 0x4));
        G32(VA_g_voice_refill_pending) = 1;                                /* refill pending */
        if (sz == 0x8000u) return;                       /* full buffer -> stream continues */
        G32(VA_g_voice_bytes_remaining + 0x10) = 0;                                /* short -> that was the last */
        return;
    }
    G32(VA_g_voice_sample_rate + 0xc) = 0x2000;                               /* underrun -> queue silence */
    G16(VA_g_voice_sample_rate + 0x8) = cur_ds();
    G32(VA_g_voice_sample_rate + 0x4) = G32(VA_g_audio_silence_buffer);
    sos_voice_set_callback(voice, arg3, (uint32_t)GADDR(VA_g_voice_sample_rate + 0x4));
}

/* prime_voice_clip 0x1e54d (545 B) — open (once) + start streaming dialogue clip `clip` (a dbase400
 * voice_offset>>3): open dbase500 (path 0x76590 + name 0x75e5c; on failure prompt via
 * show_resource_error_box 0x2632a [bridge] — retry on 1, give up + latch [0x820bd] on 2), seek clip*8,
 * read the 0x2c header {+0x14 u16 tag ('*'=DPCM), +0x18 rate?, +0x20 i16 divisor, +0x28 total size},
 * set the stream state, DPCM setup (predictor reset + step table [L]), bind the ping-pong buffers,
 * decode the first two blocks, then build the static voice struct 0x82045 (rate [0x82041], far cb
 * 0x1e487/CS) and submit via install_voice_sos_callback. ABI: EAX=clip -> EAX = 1 started / 0. */
uint32_t prime_voice_clip(uint32_t clip)
{
    if (G32(VA_g_audio_decode_buffer_b) == 0) return 0;
    if (G32(VA_g_voice_stream_state) == 1) return 0;
    if (G32(VA_g_voice_file_handle) == 0) {
        uint8_t path[0x78];
        build_game_path(path, (const uint8_t *)(uintptr_t)GADDR(VA_g_dir_gdv + 0xa0),
                               (const uint8_t *)(uintptr_t)GADDR(VA_g_heap_free_list + 0x84c));
        for (;;) {
            uint32_t h = dos_open_file((uint32_t)(uintptr_t)path, 0);   /* dos_open_file(path, mode 0) (C2) */
            G32(VA_g_voice_file_handle) = (int32_t)h;
            if (h != 0) break;
            if (G32(VA_g_voice_refill_pending + 0x4) != 0) break;
            /* show_resource_error_box [KEPT-REPOINT — LAST bridging site of 0x2632a; the das_assets +
             * file_config sites are now DIRECT C, so retiring the target hinges on THIS one. The
             * wall: prime_voice_clip 0x1e54d IS oracle-tested (au_diff cases openfail /
             * openfail_latch, test_audio.c:1069/1076), which STUB 0x2632a to canned EAX=0 / EAX=2. This
             * is NOT oracle-neutral (unlike the das site, which never runs): a direct call to
             * show_resource_error_box makes the LIFT side run the real modal -> show_message_
             * box (0x2508f), which BLOCKS in `for(;;){ poll_mouse_input(); if(input_ring_dequeue()==1) }`
             * — no shm_tick fills the ring 0x90c1c in the batch oracle -> infinite hang. Even with a
             * staged ring key, au_diff compares the WHOLE snapshot write-set, and the real compositor
             * (flags 0x200 zoom-in anim driven by tick 0x90bcc + inner fade; text-layout bridge 0x1f3d3;
             * DAS icon tile + font glyph render; flip_video_page bridge 0x2e1e8) emits a large,
             * NON-IDEMPOTENT (scroll/anim/tick) write-set that the canned ST_EAX0/2 stub does not — and
             * 0x2508f has NO oracle test by design (menu_hud_ui comment: "in-game live-swap only"). The
             * original side (canon 0x1e54d->0x2632a) still hits the stub, so the diff is asymmetric.
             * Unblock: a dedicated menu_hud_ui interactive-modal harness that oracle-tests 0x2508f (stage the
             * input ring + DAS-tile cache + font + a non-blocking tick, handle the flip_video_page seam)
             * -> only then can BOTH sites run the real modal symmetrically. OR ride the live-swap in-game
             * tier, where 0x2632a already runs lifted. NOT agent-closable as a re-point leaf.] */
            uint32_t r = au_bridge(0x2632a, 0, 0, 0, 0); /* [IN-GAME-TIER] show_resource_error_box (UI bridge) */
            if (r == 2) G32(VA_g_voice_refill_pending + 0x4) = G32(VA_g_voice_refill_pending + 0x4) + 1;
            if (r != 1) break;                           /* 1 = retry the open */
        }
    }
    if (G32(VA_g_voice_file_handle) == 0) return 0;
    dos_lseek((uint32_t)G32(VA_g_voice_file_handle), clip << 3, 0);   /* (C2) */
    uint8_t hdr[0x2c];
    dos_read_items((uint32_t)(uintptr_t)hdr, 0x2c, 1, (uint32_t)G32(VA_g_voice_file_handle));   /* (C2) */
    G32(VA_g_voice_bytes_remaining) = *(int32_t *)(hdr + 0x28);             /* total size */
    G32(VA_g_voice_bytes_remaining + 0x18) = *(int32_t *)(hdr + 0x18);
    int32_t divi = (int32_t)*(int16_t *)(hdr + 0x20);    /* movsx */
    if (divi == 0) divi = 1;
    G32(VA_g_voice_bytes_remaining + 0x14) = (int32_t)(*(uint32_t *)(hdr + 0x28) / (uint32_t)divi);   /* div (unsigned) */
    if (*(uint16_t *)(hdr + 0x14) == 0x2a) {             /* '*' = DPCM */
        G32(VA_g_voice_is_dpcm) = 1;
        G32(VA_g_voice_dpcm_predictor) = 0;                                /* predictor reset (D1) */
        build_dpcm_step_table();
    } else
        G32(VA_g_voice_is_dpcm) = 0;
    if (G32(VA_g_voice_bytes_remaining + 0x4) == 0) {
        G32(VA_g_voice_bytes_remaining + 0x4) = G32(VA_g_audio_decode_buffer_a);
        G32(VA_g_voice_bytes_remaining + 0x8) = G32(VA_g_audio_decode_buffer_b);
    }
    uint32_t n1 = decode_voice_block((uint32_t)G32(VA_g_voice_bytes_remaining + 0x4), (uint32_t)G32(VA_g_voice_bytes_remaining), 0x8000);
    G32(VA_g_voice_bytes_remaining) = G32(VA_g_voice_bytes_remaining) - (int32_t)n1;
    G32(VA_g_voice_bytes_remaining + 0xc) = (int32_t)n1;
    uint32_t n2 = decode_voice_block((uint32_t)G32(VA_g_voice_bytes_remaining + 0x8), (uint32_t)G32(VA_g_voice_bytes_remaining), 0x8000);
    G32(VA_g_voice_bytes_remaining + 0x10) = (int32_t)n2;
    G32(VA_g_voice_bytes_remaining) = G32(VA_g_voice_bytes_remaining) - (int32_t)n2;
    G32(VA_g_voice_refill_pending) = 0;
    if (n1 == 0) {
        dos_close_handle((uint32_t)G32(VA_g_voice_file_handle));   /* (C2) */
        G32(VA_g_voice_file_handle) = 0;
        return 0;
    }
    uint32_t sz = n1;
    if (G32(VA_g_voice_is_dpcm) != 0) sz += sz;
    G32(VA_g_voice_sample_rate + 0x14) = 2;
    G32(VA_g_voice_sample_rate + 0x1c) = 0xedff;
    G8(VA_g_voice_sample_rate + 0x2c) = (uint8_t)(G8(VA_g_voice_sample_rate + 0x2c) | 0x80);
    G32(VA_g_voice_sample_rate + 0xc) = (int32_t)sz;
    G16(VA_g_voice_sample_rate + 0x8) = cur_ds();
    uint32_t rate = (uint32_t)G32(VA_g_voice_sample_rate);
    G32(VA_g_voice_sample_rate + 0x4) = G32(VA_g_voice_bytes_remaining + 0x4);
    G32(VA_g_voice_sample_rate + 0x18) = (int32_t)rate;
    if (rate < 0x7f00u) G8(VA_g_voice_sample_rate + 0x2d) = (uint8_t)(G8(VA_g_voice_sample_rate + 0x2d) | 1);   /* jae skips */
    G32(VA_g_voice_sample_rate + 0x20) = (int32_t)GADDR(0x1e487);              /* far cb = voice_stream_sos_callback */
    G16(VA_g_voice_sample_rate + 0x24) = cur_cs();
    uint32_t r = install_voice_sos_callback((uint32_t)GADDR(VA_g_voice_sample_rate + 0x4));
    G32(VA_g_voice_stream_state) = 1;
    G32(VA_g_voice_stop_handle) = (int32_t)r;
    return 1;
}

/* voice_stream_pump 0x1e9b5 (201 B) — the per-frame refill (game loop / dialogue UI / save-load):
 * when a refill is pending decode the next block into the just-drained buffer and ping-pong
 * (swap_voice_double_buffers [L]); a decode > 0x8000 zeroes the count (quirk, unsigned jbe). On
 * end-of-stream ([0x8200c]==2) reset the dialogue text timers ([0x83125]=0, [0x83121]=0x64). Then the
 * dialogue-busy gate: while busy and (still playing or [0x83115]) do nothing; else clear busy and
 * advance the dialogue action queue [L]. ABI: void. */
void voice_stream_pump(void)
{
    if (G32(VA_g_voice_file_handle) != 0 && G32(VA_g_voice_refill_pending) != 0) {
        if (G32(VA_g_voice_bytes_remaining) != 0) {
            uint32_t n = decode_voice_block((uint32_t)G32(VA_g_voice_bytes_remaining + 0x4),
                                                   (uint32_t)G32(VA_g_voice_bytes_remaining), 0x8000);
            G32(VA_g_voice_bytes_remaining + 0xc) = (int32_t)n;
            if (n > 0x8000u) G32(VA_g_voice_bytes_remaining + 0xc) = 0;           /* jbe */
            else G32(VA_g_voice_bytes_remaining) = G32(VA_g_voice_bytes_remaining) - (int32_t)n;
            G32(VA_g_voice_refill_pending) = 0;
        } else {
            G32(VA_g_voice_refill_pending) = 0;
            G32(VA_g_voice_bytes_remaining + 0xc) = 0;
        }
        swap_voice_double_buffers();
    }
    if (G32(VA_g_voice_stream_state) == 2) {
        G32(VA_g_voice_stream_state) = 0;
        G32(VA_g_move_freeze_gate) = 0;
        G32(VA_g_dialogue_segment_index) = 0x64;
    }
    if (G32(VA_g_dialogue_busy_flag) != 0) {
        if (G32(VA_g_voice_stream_state) != 0) return;
        if (G32(VA_g_active_dialogue_context) != 0) return;
        G32(VA_g_dialogue_busy_flag) = 0;
    }
    advance_dialogue_action_queue();
}

/* dialogue_voice_force_end 0x1f671 (91 B) — stop the playing clip (state 1 -> stop voice [0x82014],
 * state 2, clear [0x83115] unless the text timer [0x83125] is past 0x6ffff, zero the timer). When NOT
 * playing: a timer parked exactly AT 0x6ffff runs the choice-highlight update (0x1f71d, bridge — the
 * dialogue_ui subsystem) and, if it returns 1, tail-calls close_dialogue_and_run_branch [L]; any other
 * timer value is just zeroed. ABI: EAX/EDX passed through to the highlight updater. */
void dialogue_voice_force_end(uint32_t eax_in, uint32_t edx_in)
{
    if (G32(VA_g_voice_stream_state) == 1) {
        sos_stop_voice((uint32_t)G32(VA_g_voice_stop_handle));
        G32(VA_g_voice_stream_state) = 2;
        if (G32(VA_g_move_freeze_gate) < 0x6ffff) G32(VA_g_active_dialogue_context) = 0;    /* jge skips */
        G32(VA_g_move_freeze_gate) = 0;
        return;
    }
    if (G32(VA_g_move_freeze_gate) == 0x6ffff) {
        /* re-pointed: 0x1f71d update_dialogue_choice_highlight (lifted; EAX,EDX -> EAX) */
        uint32_t r = update_dialogue_choice_highlight((int32_t)eax_in, (int32_t)edx_in);
        if (r == 1) close_dialogue_and_run_branch();      /* jmp 0x1fbe4 */
        return;
    }
    G32(VA_g_move_freeze_gate) = 0;
}

/* dialogue_voice_stop_all 0x1f6cc (81 B) — hard voice+dialogue reset (save-load / loop): force-end,
 * pump once, finish the dialogue record eval [L], force-end again, clear busy/state/timer globals.
 * ABI: void. */
void dialogue_voice_stop_all(void)
{
    G32(VA_g_dialogue_action_queue_count) = 0;
    dialogue_voice_force_end(0, 0);
    voice_stream_pump();
    finish_dialogue_record_eval();
    dialogue_voice_force_end(0, 0);
    G32(VA_g_dialogue_busy_flag) = 0;
    G32(VA_g_dialogue_action_queue_count) = 0;
    G32(VA_g_active_dialogue_context) = 0;
    G32(VA_g_move_freeze_gate) = 0;
}

/* try_interrupt_dialogue_voice 0x18a2a (58 B) — user skip: if a line is active ([0x81e3e]) or a
 * choice is parked ([0x83115] with the timer at 0x7ffff), force-end with the stashed dialogue args
 * ([0x707b3]/[0x707b7]) and report 1; else 0. ABI: void -> EAX = 1/0. */
uint32_t try_interrupt_dialogue_voice(void)
{
    if (G32(VA_g_dialogue_action_queue_count) != 0 ||
        (G32(VA_g_active_dialogue_context) != 0 && G32(VA_g_move_freeze_gate) == 0x7ffff)) {
        dialogue_voice_force_end((uint32_t)G32(VA_g_mouse_x), (uint32_t)G32(VA_g_mouse_y));
        return 1;
    }
    return 0;
}

/* ============================================================ C. MIDI music sequencer
 * The sequencer state lives in DRIVER-SIDE tables reached through FAR (selector:offset) pointers —
 * the original uses lgs / gs: reads throughout. The lifts perform the SAME segment loads via the
 * gs helpers below (faithful to the bytes; works under the oracle's staged LDT selectors AND the
 * host's flat game selectors alike). Table map (canon; 8 sequences x 32 tracks):
 *   0x93164/0x93168 +s*6  descriptor far ptr (the loaded chunk)   0x93144 +s*4  running flag
 *   0x92f2c/0x92f30 +s*6  channel-map far ptr                     0x92f5c/0x92f7c +s*4 track counts
 *   0x9212c/0x92130 +s*0xc0+t*6 track event far ptr               0x94bb4/0x94bb8  track note-map far
 *   0x9272c +s*0x80+t*4  next delta   0x92b2c +s*0x80+t*4 tick    0x95204/8 +s*0xc0+t*6 device entry
 *   0x95804/0x95834/0x95864(+s*0x2fa) note tables                 0x72908 +s*4 timer handle
 *   0x920f0 +d*4 installed device ids · 0x7377c signature string · 0x741fc handle->seq byte map */

/* Far-pointer access: the original reads/writes these driver-side sequencer tables through
 * gs:-prefixed far pointers (a real %gs segment load per access). Here the selector's linear
 * base is resolved through the host selector-base hook instead of loading the selector into a
 * hardware segment register — a hardware segment load is not portable: modern hosts have no LDT
 * to hold a game selector (under Win32/WOW64 such a load faults). The behavior is identical, the
 * access lands at base(sel) + offset, the exact linear address the segment load would produce.
 * Volatile so these table accesses are neither cached, elided, nor reordered (the far access this
 * replaces carried a "memory" clobber for the same reason). */
static inline uint8_t au_gsr8(uint16_t sel, uint32_t off)
{
    return *(const volatile uint8_t *)(uintptr_t)
        ((g_os_sel_base ? g_os_sel_base(sel) : 0) + off);
}
static inline uint16_t au_gsr16(uint16_t sel, uint32_t off)
{
    return *(const volatile uint16_t *)(uintptr_t)
        ((g_os_sel_base ? g_os_sel_base(sel) : 0) + off);
}
static inline uint32_t au_gsr32(uint16_t sel, uint32_t off)
{
    return *(const volatile uint32_t *)(uintptr_t)
        ((g_os_sel_base ? g_os_sel_base(sel) : 0) + off);
}
static inline void au_gsw8(uint16_t sel, uint32_t off, uint8_t v)
{
    *(volatile uint8_t *)(uintptr_t)
        ((g_os_sel_base ? g_os_sel_base(sel) : 0) + off) = v;
}
static inline void au_gsw16(uint16_t sel, uint32_t off, uint16_t v)
{
    *(volatile uint16_t *)(uintptr_t)
        ((g_os_sel_base ? g_os_sel_base(sel) : 0) + off) = v;
}
static inline void au_gsw32(uint16_t sel, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(uintptr_t)
        ((g_os_sel_base ? g_os_sel_base(sel) : 0) + off) = v;
}

/* (au_bridge_stk retired: the SOS timer/dispatch/open/driver-slot entries that took
 * extra stack params are now reached through the os_audio_* API — the stack far-pairs are collapsed into
 * the call contract, see os_api.h. The plain-EAX au_bridge stays for the DOS/UI/CRT seams above.) */

/* decode_midi_varlen 0x47150 (138 B) — HMI little-endian 7-bit varint: accumulate (byte&0x7f)<<shift
 * (shift += 7) until a byte with bit 7 SET (the last-byte flag); store the value through the out far
 * ptr. ABI: EAX=src off, DX=src sel, EBX=out off, CX=out sel -> EAX = bytes consumed. Pure leaf. */
uint32_t decode_midi_varlen(uint32_t src_off, uint32_t src_sel, uint32_t out_off, uint32_t out_sel)
{
    uint32_t val = 0, shift = 0, count = 0;
    int last;
    do {
        count++;
        uint8_t b = au_gsr8((uint16_t)src_sel, src_off);
        src_off++;
        last = (b & 0x80) != 0;
        val |= (uint32_t)(b & 0x7f) << (shift & 31);     /* shl eax,cl */
        shift += 7;
    } while (!last);
    au_gsw32((uint16_t)out_sel, out_off, val);
    return count;
}

/* emit_audio_sequence_event 0x4627d (321 B) — push a volume-controller MIDI message (0xb0|chan, 7,
 * volume-LUT byte) to every live track's device: stores the event byte as the sequence's channel
 * volume [0x7370c+s*4], then per track (live = event far ptr nonzero) reads the track's MIDI channel
 * (note-map +8), its device from the channel map, remaps through the 0x729d8 LUT when [0x951cc] is
 * set, builds the 3-byte message at 0x951c0 and dispatches (sos_dispatch_midi_event, stack arg = 3).
 * ABI: EAX=seq, DL=event -> EAX=0. */
uint32_t emit_audio_sequence_event(uint32_t seq, uint32_t event)
{
    st32((uint32_t)GADDR(VA_g_sound_channel_volume) + seq * 4, event & 0xff);
    for (uint32_t chan = 0; chan < 0x20; chan++) {
        uint32_t trk = seq * 0xc0 + chan * 6;
        if (ld32u((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0x3c) + trk) == 0 &&
            ld16((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0x40) + trk) == 0) continue;
        uint16_t nm_sel = ld16((uint32_t)GADDR(VA_g_extmidi_out_callback + 0x19fc) + trk + 4);
        uint32_t nm_off = ld32u((uint32_t)GADDR(VA_g_extmidi_out_callback + 0x19fc) + trk);
        uint8_t  mchan  = au_gsr8(nm_sel, nm_off + 8);
        uint16_t cm_sel = ld16((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xe40) + seq * 6);
        uint32_t cm_off = ld32u((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xe3c) + seq * 6);
        uint32_t dev    = au_gsr32(cm_sel, cm_off + chan * 4);
        uint8_t  rem;
        if (G32(VA_g_sos_voice_alloc_mode) != 0)
            rem = ld8((uint32_t)GADDR(VA_g_midi_logical_to_physical_chan) + dev * 0x80 + seq * 0x10 + mchan);
        else
            rem = mchan;
        G8(VA_g_sos_midi_message + 0xc) = (uint8_t)(mchan | 0xb0);
        G8(VA_g_sos_midi_message + 0xd) = 7;
        G8(VA_g_sos_midi_message + 0xe) = ld8((uint32_t)GADDR(VA_g_midi_channel_raw_volume) + dev * 0x10 + rem);
        os_audio_midi_dispatch(seq, dev, (uint32_t)GADDR(VA_g_sos_midi_message + 0xc), cur_ds());  /* was au_bridge_stk(0x44e0d,…,stk=3) */
    }
    return 0;
}

/* clear_music_sequence_slot 0x46cce (74 B) — zero the descriptor far ptr for slot < 8; 0xa on range. */
uint32_t clear_music_sequence_slot(uint32_t slot)
{
    if (slot >= 8) return 0xa;
    st16((uint32_t)GADDR(VA_g_sos_driver_descriptor + 0x4) + slot * 6, 0);
    st32((uint32_t)GADDR(VA_g_sos_driver_descriptor) + slot * 6, 0);
    return 0;
}

/* step_audio_sequence 0x46d18 (143 B) — start the sequence clock: register the driver timer at the
 * descriptor's tick rate (+0x38) with the host tick fn sos_sequence_timer_tick 0x51ad5 (CS far cb) and
 * the handle out-ptr [0x72908+s*4]; on success map the handle to the sequence (0x741fc table) and set
 * the running flag. ABI: EAX=seq -> EAX = 0 | timer error. */
uint32_t step_audio_sequence(uint32_t seq)
{
    uint16_t d_sel = ld16((uint32_t)GADDR(VA_g_sos_driver_descriptor + 0x4) + seq * 6);
    uint32_t d_off = ld32u((uint32_t)GADDR(VA_g_sos_driver_descriptor) + seq * 6);
    uint32_t rate  = au_gsr32(d_sel, d_off + 0x38);
    uint32_t r = os_audio_timer_register_event(rate, (uint32_t)GADDR(0x51ad5), cur_cs(),
                                               (uint32_t)GADDR(VA_g_sos_sequence_ctx_table) + seq * 4, cur_ds());
                                               /* was au_bridge_stk(0x49923,…): EDX=0x51ad5 disasm-dead, dropped */
    if (r != 0) return r;
    uint32_t h = ld32u((uint32_t)GADDR(VA_g_sos_sequence_ctx_table) + seq * 4);
    st8(h + (uint32_t)GADDR(VA_g_sos_timer_event_countdown), (uint8_t)seq);
    st32((uint32_t)GADDR(VA_g_sos_driver_vtable + 0x1a8) + seq * 4, 1);
    return 0;
}

/* parse_music_sequence_tracks 0x46eb3 (669 B) — (re)bind a sequence to a chunk given a far-pair block
 * {chunk_off,chunk_sel,data_off,data_sel} read through `pair_sel`: store the descriptor far ptr, track
 * count (desc+0x30) into 0x92f5c/0x92f7c, the data far into desc+0x380, then walk the track headers at
 * desc+0x388 (stride = header +4 length): per track reset the tick, point the note-map/event far ptrs,
 * pre-decode the first delta into 0x9272c and advance. Finally kill tracks whose channel-map entry is
 * 0xff. ABI: EAX=seq, EBX=pair ptr, CX=pair sel -> EAX=0. */
uint32_t parse_music_sequence_tracks(uint32_t seq, uint32_t pair, uint32_t pair_sel)
{
    uint16_t psel  = (uint16_t)pair_sel;
    uint32_t accum = 0;
    uint32_t c_off = au_gsr32(psel, pair);
    uint16_t c_sel = au_gsr16(psel, pair + 4);
    st16((uint32_t)GADDR(VA_g_sos_driver_descriptor + 0x4) + seq * 6, c_sel);
    st32((uint32_t)GADDR(VA_g_sos_driver_descriptor) + seq * 6, c_off);
    uint16_t d_sel   = ld16((uint32_t)GADDR(VA_g_sos_driver_descriptor + 0x4) + seq * 6);
    uint32_t d_off   = ld32u((uint32_t)GADDR(VA_g_sos_driver_descriptor) + seq * 6);
    uint32_t trkbase = d_off + 0x388;
    st32((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xe6c) + seq * 4, (int32_t)au_gsr32(d_sel, d_off + 0x30));
    st32((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xe8c) + seq * 4, ld32u((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xe6c) + seq * 4));
    au_gsw16(d_sel, d_off + 0x384, au_gsr16(psel, pair + 0xc));
    au_gsw32(d_sel, d_off + 0x380, au_gsr32(psel, pair + 8));
    for (uint32_t i = 0; i < ld32u((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xe6c) + seq * 4); i++) {
        uint32_t trk = seq * 0xc0 + i * 6;
        st32((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xa3c) + seq * 0x80 + i * 4, 0);
        uint32_t nm = trkbase + accum;
        st16((uint32_t)GADDR(VA_g_extmidi_out_callback + 0x1a00) + trk, d_sel);
        st32((uint32_t)GADDR(VA_g_extmidi_out_callback + 0x19fc) + trk, nm);
        st16((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0x40) + trk, d_sel);
        st32((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0x3c) + trk, nm + 0xc);
        uint32_t n = decode_midi_varlen(nm + 0xc, d_sel,
                                               (uint32_t)GADDR(VA_g_sos_driver_type_ids + 0x63c) + seq * 0x80 + i * 4, cur_ds());
        st32((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0x3c) + trk, ld32u((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0x3c) + trk) + n);
        accum += au_gsr32(d_sel, nm + 4);
    }
    for (uint32_t i = 0; i < ld32u((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xe8c) + seq * 4); i++) {
        uint16_t cm_sel = ld16((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xe40) + seq * 6);
        uint32_t cm_off = ld32u((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xe3c) + seq * 6);
        if (au_gsr32(cm_sel, cm_off + i * 4) != 0xff) continue;
        uint32_t trk = seq * 0xc0 + i * 6;
        st16((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0x40) + trk, 0);
        st32((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0x3c) + trk, 0);
        st32((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xe6c) + seq * 4, ld32u((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xe6c) + seq * 4) - 1);
    }
    return 0;
}

/* teardown_music_sequence 0x46da7 (268 B) — stop a sequence: remove its driver timer (handle != -1),
 * unmap the handle byte (NB: written even for handle -1 — the original's wrapping [-1+0x741fc] store,
 * reproduced by the uint32 add), and when running: save the descriptor+data far ptrs into a stack
 * pair-block, all-notes-off (bridge), clear running/descriptor state, and re-parse from the saved pair
 * (call-closed, pair read via SS). ABI: EAX=seq -> EAX = 0 | 0xa. */
uint32_t teardown_music_sequence(uint32_t seq)
{
    if (seq >= 8) return 0xa;
    if (ld32s((uint32_t)GADDR(VA_g_sos_sequence_ctx_table) + seq * 4) != -1)
        os_audio_timer_remove_event(ld32u((uint32_t)GADDR(VA_g_sos_sequence_ctx_table) + seq * 4));  /* was au_bridge(0x49ca4,…) */
    st8(ld32u((uint32_t)GADDR(VA_g_sos_sequence_ctx_table) + seq * 4) + (uint32_t)GADDR(VA_g_sos_timer_event_countdown), 0xff);
    st32((uint32_t)GADDR(VA_g_sos_sequence_ctx_table) + seq * 4, 0xffffffff);
    if (ld32s((uint32_t)GADDR(VA_g_sos_driver_vtable + 0x1a8) + seq * 4) != 0) {
        uint8_t blk[16];                                 /* {off,sel, data_off,data_sel} @0/4/8/0xc */
        *(uint32_t *)blk       = ld32u((uint32_t)GADDR(VA_g_sos_driver_descriptor) + seq * 6);
        *(uint16_t *)(blk + 4) = ld16((uint32_t)GADDR(VA_g_sos_driver_descriptor + 0x4) + seq * 6);
        uint16_t dsel = *(uint16_t *)(blk + 4);
        uint32_t doff = *(uint32_t *)blk;
        *(uint32_t *)(blk + 8)   = au_gsr32(dsel, doff + 0x380);
        *(uint16_t *)(blk + 0xc) = au_gsr16(dsel, doff + 0x384);
        os_audio_midi_all_notes_off(seq);                /* was au_bridge(0x4594d,…): midi_all_notes_off_channels */
        st32((uint32_t)GADDR(VA_g_sos_driver_vtable + 0x1a8) + seq * 4, 0);
        st16((uint32_t)GADDR(VA_g_sos_driver_descriptor + 0x4) + seq * 6, 0);
        st32((uint32_t)GADDR(VA_g_sos_driver_descriptor) + seq * 6, 0);
        uint16_t ss; __asm__("mov %%ss,%0" : "=r"(ss));
        parse_music_sequence_tracks(seq, (uint32_t)(uintptr_t)blk, ss);
    }
    return 0;
}

/* finalize_audio_sequence 0x471da (105 B) — set the master music volume byte [0x73708] and re-emit
 * every running sequence's channel volume. ABI: AL=volume -> EAX=0. */
uint32_t finalize_audio_sequence(uint32_t vol)
{
    G8(VA_g_sound_master_volume) = (uint8_t)vol;
    for (uint32_t s = 0; s < 8; s++) {
        if (ld32s((uint32_t)GADDR(VA_g_sos_driver_vtable + 0x1a8) + s * 4) == 0) continue;
        emit_audio_sequence_event(s, (uint32_t)ld8((uint32_t)GADDR(VA_g_sound_channel_volume) + s * 4));
    }
    return 0;
}

/* init_audio_sequence 0x464f9 (2005 B — the music-chunk parser, the biggest fn in the subsystem).
 * Verify the chunk signature (string 0x7377c), claim a free descriptor slot (err 0xb; bad sig 0xe),
 * clear the slot's track/note tables, bind the descriptor to the chunk, per-track setup (as in parse),
 * then resolve the device-map section (chunk+desc[0x20]): a per-track device-count byte array followed
 * by 0x18-byte device entries whose +0xc chunk-relative ptr is resolved absolute into +8; finally pick
 * each track's device by walking its 5-entry preference list (desc+0x80+t*0x14) against the installed
 * device ids [0x920f0] (0xa000 matches 0xa000/0xa001/0xa008; 0xa002 matches 0xa002/0xa009; else exact),
 * writing the channel map (no prefs -> device 0; no match -> track killed, map 0xff), init the note
 * state (desc+0x300, 0x80 bytes = 1) and store the slot through the out far ptr.
 * ABI: EAX=chunk far-pair ptr, DX=pair sel, EBX=channel-map off, CX=map sel,
 *      stack: {out_off, out_sel} (ret 8) -> EAX = 0 | 0xb | 0xe. */
uint32_t init_audio_sequence(uint32_t pair, uint32_t pair_sel, uint32_t map_off, uint32_t map_sel,
                                    uint32_t out_off, uint32_t out_sel)
{
    uint16_t psel = (uint16_t)pair_sel;
    for (uint32_t i = 0; G8((VA_g_midi_channel_raw_volume + 0x50) + i) != 0; i++) {    /* signature check */
        uint32_t co = au_gsr32(psel, pair);
        uint16_t cs = au_gsr16(psel, pair + 4);
        if ((uint32_t)G8((VA_g_midi_channel_raw_volume + 0x50) + i) != au_gsr8(cs, co + i)) return 0xe;
    }
    uint32_t slot = 8;
    for (uint32_t s = 0; s < 8; s++)
        if (ld32u((uint32_t)GADDR(VA_g_sos_driver_descriptor) + s * 6) == 0 &&
            ld16((uint32_t)GADDR(VA_g_sos_driver_descriptor + 0x4) + s * 6) == 0) { slot = s; break; }
    if (slot == 8) return 0xb;
    for (uint32_t ch = 0; ch < 0x20; ch++) {
        uint32_t trk = slot * 0xc0 + ch * 6;
        st16((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0x40) + trk, 0);
        st32((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0x3c) + trk, 0);
    }
    uint32_t c_off = au_gsr32(psel, pair);
    uint16_t c_sel = au_gsr16(psel, pair + 4);
    st16((uint32_t)GADDR(VA_g_sos_driver_descriptor + 0x4) + slot * 6, c_sel);
    st32((uint32_t)GADDR(VA_g_sos_driver_descriptor) + slot * 6, c_off);
    for (uint32_t n = 0; n < 0x7f; n++) {
        uint32_t e = slot * 0x2fa + n * 6;
        st16((uint32_t)GADDR(VA_g_sos_timer_tick_flag + 0x698) + e, 0);
        st32((uint32_t)GADDR(VA_g_sos_timer_tick_flag + 0x694) + e, 0);
    }
    st16((uint32_t)GADDR(VA_g_sos_timer_tick_flag + 0x638) + slot * 6, 0);
    st32((uint32_t)GADDR(VA_g_sos_timer_tick_flag + 0x634) + slot * 6, 0);
    st16((uint32_t)GADDR(VA_g_sos_timer_tick_flag + 0x668) + slot * 6, 0);
    st32((uint32_t)GADDR(VA_g_sos_timer_tick_flag + 0x664) + slot * 6, 0);
    uint16_t d_sel = ld16((uint32_t)GADDR(VA_g_sos_driver_descriptor + 0x4) + slot * 6);
    uint32_t d_off = ld32u((uint32_t)GADDR(VA_g_sos_driver_descriptor) + slot * 6);
    uint32_t sub_off;                                    /* the +0x20 device-map sub-block */
    uint16_t sub_sel;
    {
        uint32_t os_off = au_gsr32(psel, pair);
        uint16_t os_sel = au_gsr16(psel, pair + 4);
        sub_off = au_gsr32(d_sel, d_off + 0x20) + os_off;
        sub_sel = os_sel;
    }
    uint32_t trkbase = d_off + 0x388;
    st16((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xe40) + slot * 6, (uint16_t)map_sel);
    st32((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xe3c) + slot * 6, map_off);
    st32((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xe6c) + slot * 4, (int32_t)au_gsr32(d_sel, d_off + 0x30));
    st32((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xe8c) + slot * 4, ld32u((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xe6c) + slot * 4));
    au_gsw16(d_sel, d_off + 0x384, au_gsr16(psel, pair + 0xc));
    au_gsw32(d_sel, d_off + 0x380, au_gsr32(psel, pair + 8));
    uint32_t accum = 0;
    for (uint32_t i = 0; i < ld32u((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xe6c) + slot * 4); i++) {
        uint32_t trk = slot * 0xc0 + i * 6;
        st32((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xa3c) + slot * 0x80 + i * 4, 0);
        uint32_t nm = trkbase + accum;
        st16((uint32_t)GADDR(VA_g_extmidi_out_callback + 0x1a00) + trk, d_sel);
        st32((uint32_t)GADDR(VA_g_extmidi_out_callback + 0x19fc) + trk, nm);
        st16((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0x40) + trk, d_sel);
        st32((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0x3c) + trk, nm + 0xc);
        uint32_t n = decode_midi_varlen(nm + 0xc, d_sel,
                                               (uint32_t)GADDR(VA_g_sos_driver_type_ids + 0x63c) + slot * 0x80 + i * 4, cur_ds());
        st32((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0x3c) + trk, ld32u((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0x3c) + trk) + n);
        accum += au_gsr32(d_sel, nm + 4);
    }
    /* device-map walk: count bytes (one per track), then 0x18-byte entries */
    uint32_t dm_off = sub_off;
    uint32_t de_off = sub_off + ld32u((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xe6c) + slot * 4);
    for (uint32_t i = 0; i < ld32u((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xe6c) + slot * 4); i++) {
        uint32_t trk = slot * 0xc0 + i * 6;
        if (au_gsr8(sub_sel, dm_off) != 0) {
            st16((uint32_t)GADDR(VA_g_sos_timer_tick_flag + 0x38) + trk, sub_sel);
            st32((uint32_t)GADDR(VA_g_sos_timer_tick_flag + 0x34) + trk, de_off);
        }
        for (uint32_t j = 0; j < au_gsr8(sub_sel, dm_off); j++) {
            uint32_t ent = de_off + j * 0x18;
            uint32_t cc_off = au_gsr32(psel, pair);
            au_gsw32(sub_sel, ent + 8, au_gsr32(sub_sel, ent + 0xc) + cc_off);
        }
        de_off += (uint32_t)au_gsr8(sub_sel, dm_off) * 0x18;
        dm_off++;
    }
    /* per-track device selection */
    for (uint32_t i = 0; i < ld32u((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xe8c) + slot * 4); i++) {
        uint16_t cm_sel = ld16((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xe40) + slot * 6);
        uint32_t cm_off = ld32u((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xe3c) + slot * 6);
        if (au_gsr32(cm_sel, cm_off + i * 4) != 0xff) continue;
        uint32_t assigned = 0;
        for (uint32_t prio = 0; ; prio++) {
            uint16_t ds2 = ld16((uint32_t)GADDR(VA_g_sos_driver_descriptor + 0x4) + slot * 6);
            uint32_t do2 = ld32u((uint32_t)GADDR(VA_g_sos_driver_descriptor) + slot * 6);
            uint32_t pref = au_gsr32(ds2, do2 + i * 0x14 + prio * 4 + 0x80);
            if (pref == 0) break;
            if (assigned) break;
            if (prio >= 5) break;
            for (uint32_t dev = 0; dev < 5; dev++) {
                uint32_t dv = ld32u((uint32_t)GADDR(VA_g_sos_driver_type_ids) + dev * 4);
                int hit;
                if (pref == 0xa000)      hit = (dv == 0xa000 || dv == 0xa001 || dv == 0xa008);
                else if (pref == 0xa002) hit = (dv == 0xa002 || dv == 0xa009);
                else                     hit = (dv == pref);
                if (hit) {
                    au_gsw32(cm_sel, cm_off + i * 4, dev);
                    assigned = 1;
                    break;
                }
            }
        }
        uint16_t ds3 = ld16((uint32_t)GADDR(VA_g_sos_driver_descriptor + 0x4) + slot * 6);
        uint32_t do3 = ld32u((uint32_t)GADDR(VA_g_sos_driver_descriptor) + slot * 6);
        if (au_gsr32(ds3, do3 + i * 0x14 + 0x80) == 0) { /* no preferences at all -> device 0 */
            au_gsw32(cm_sel, cm_off + i * 4, 0);
            continue;
        }
        if (!assigned) {                                 /* prefs but no installed match -> kill track */
            uint32_t trk = slot * 0xc0 + i * 6;
            st16((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0x40) + trk, 0);
            st32((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0x3c) + trk, 0);
            au_gsw32(cm_sel, cm_off + i * 4, 0xff);
            st32((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xe6c) + slot * 4, ld32u((uint32_t)GADDR(VA_g_sos_driver_type_ids + 0xe6c) + slot * 4) - 1);
        }
    }
    {
        uint16_t ds4 = ld16((uint32_t)GADDR(VA_g_sos_driver_descriptor + 0x4) + slot * 6);
        uint32_t do4 = ld32u((uint32_t)GADDR(VA_g_sos_driver_descriptor) + slot * 6);
        for (uint32_t n = 0; n < 0x80; n++)
            au_gsw8(ds4, do4 + n + 0x300, 1);
    }
    au_gsw32((uint16_t)out_sel, out_off, slot);
    return 0;
}

/* ---- the 0x155xx control shims (canon-literal state at 0x7f444 ctx / 0x7f468 phase bits) ---- */

/* emit_music_sequence_event 0x1555f (16 B) — emit event AL on the active sequence. */
uint32_t emit_music_sequence_event(uint32_t event)
{
    return emit_audio_sequence_event((uint32_t)G32(VA_g_audio_sequence_ctx), event & 0xff);
}

/* set_music_master_volume 0x1556f (30 B) — store [0x71124]; re-emit when playing (phase bit2). */
uint32_t set_music_master_volume(uint32_t vol)
{
    G32(VA_g_font_descriptor + 0x212) = (int32_t)vol;
    if (G8(VA_g_audio_sequence_progress) & 4)
        return emit_audio_sequence_event((uint32_t)G32(VA_g_audio_sequence_ctx), vol & 0xff);
    return vol;                                          /* EAX untouched on the gated path */
}

/* process_audio_sequence_chunk 0x1558d (163 B) — the per-frame music lifecycle (main loop / dbase100 /
 * savegame): phase bits [0x7f468]: 1=chunk loaded, 2=inited, 4=playing. Init builds the far-pair block
 * 0x7f3b4 {chunk [0x7f450], DS} + zeroed data far, channel map 0x7f3c4, ctx out 0x7f444; then start the
 * timer (step), emit volume event 0x7f, apply the master volume, and mark playing. */
void process_audio_sequence_chunk(void)
{
    if (!(G8(VA_g_audio_sequence_progress) & 1)) return;
    if (G8(VA_g_audio_sequence_progress) & 4) return;
    if (!(G8(VA_g_audio_sequence_progress) & 2)) {
        G16(VA_g_game_heap_handle + 0x4c) = 0;
        G32(VA_g_game_heap_handle + 0x48) = 0;
        G32(VA_g_game_heap_handle + 0x40) = G32(VA_g_dbase300_chunk_buf);
        G16(VA_g_game_heap_handle + 0x44) = cur_ds();
        uint32_t r = init_audio_sequence((uint32_t)GADDR(VA_g_game_heap_handle + 0x40), cur_ds(),
                                                (uint32_t)GADDR(VA_g_game_heap_handle + 0x50), cur_ds(),
                                                (uint32_t)GADDR(VA_g_audio_sequence_ctx), cur_ds());
        if (r != 0) return;
        G8(VA_g_audio_sequence_progress) = (uint8_t)(G8(VA_g_audio_sequence_progress) | 2);
    }
    if (step_audio_sequence((uint32_t)G32(VA_g_audio_sequence_ctx)) != 0) return;
    emit_audio_sequence_event((uint32_t)G32(VA_g_audio_sequence_ctx), 0x7f);
    finalize_audio_sequence((uint32_t)G8(VA_g_font_descriptor + 0x212));
    G8(VA_g_audio_sequence_progress) = (uint8_t)(G8(VA_g_audio_sequence_progress) | 4);
}

/* stop_music_sequence 0x15630 (65 B) — stop + unload the active sequence per phase bits; reset to 1. */
void stop_music_sequence(void)
{
    if (!(G8(VA_g_audio_sequence_progress) & 1)) return;
    if (G8(VA_g_audio_sequence_progress) & 4) {
        finalize_audio_sequence(0);
        teardown_music_sequence((uint32_t)G32(VA_g_audio_sequence_ctx));
    }
    if (G8(VA_g_audio_sequence_progress) & 2)
        clear_music_sequence_slot((uint32_t)G32(VA_g_audio_sequence_ctx));
    G32(VA_g_audio_sequence_progress) = 1;
}

/* resume_music_sequence 0x15671 (24 B) — refcount up + midi_restore_channel_volumes(ctx, 1) [bridge]. */
uint32_t resume_music_sequence(void)
{
    uint32_t ctx = (uint32_t)G32(VA_g_audio_sequence_ctx);
    G32(VA_g_audio_sequence_pending) = G32(VA_g_audio_sequence_pending) + 1;
    return os_audio_midi_restore_volumes(ctx);   /* was au_bridge(0x45dc5,ctx,1,0,0): EDX=1 baked into contract */
}

/* finalize_audio_sequence_ref 0x15689 (16 B) — refcount down + tail-jmp midi_mute_channel_volumes
 * [bridge; consumes EAX only — verified in the disasm]. */
uint32_t finalize_audio_sequence_ref(void)
{
    uint32_t ctx = (uint32_t)G32(VA_g_audio_sequence_ctx);
    G32(VA_g_audio_sequence_pending) = G32(VA_g_audio_sequence_pending) - 1;
    return os_audio_midi_mute_volumes(ctx);   /* was au_bridge(0x45f1d,ctx,0,0,0) */
}

/* is_music_sequence_finished 0x15699 (36 B) — all three phase bits set AND the running flag cleared. */
uint32_t is_music_sequence_finished(void)
{
    if ((G32(VA_g_audio_sequence_progress) & 7) != 7) return 0;
    if (is_entry_93144_zero((uint32_t)G32(VA_g_audio_sequence_ctx)) != 0) return 0;
    return 1;
}

/* service_audio_sequence 0x156bd (69 B) — per-frame loop restart: when fully playing and the driver
 * cleared the running flag (song ended), teardown + re-parse from the 0x7f3b4 pair + restart the timer. */
void service_audio_sequence(void)
{
    if ((G32(VA_g_audio_sequence_progress) & 7) != 7) return;
    if (is_entry_93144_zero((uint32_t)G32(VA_g_audio_sequence_ctx)) == 0) return;
    teardown_music_sequence((uint32_t)G32(VA_g_audio_sequence_ctx));
    parse_music_sequence_tracks((uint32_t)G32(VA_g_audio_sequence_ctx), (uint32_t)GADDR(VA_g_game_heap_handle + 0x40), cur_ds());
    step_audio_sequence((uint32_t)G32(VA_g_audio_sequence_ctx));
}

/* register_music_timer_event 0x159fa (54 B) — gate [0x7f554] bit3; register the 70 Hz (0x46) driver
 * timer with the caller's far callback {EAX=offset, EDX=selector}, handle out at 0x7f4e0; count up
 * [0x7f55d] on success. ABI: EAX=cb_off, EDX=cb_sel -> EAX = 1 | 0. */
uint32_t register_music_timer_event(uint32_t cb_off, uint32_t cb_sel)
{
    if (!(G8(VA_g_sound_enabled + 0x4) & 8)) return 0;
    uint32_t r = os_audio_timer_register_event(0x46, cb_off, (uint16_t)cb_sel,
                                               (uint32_t)GADDR(VA_g_sos_digital_device + 0x4), cur_ds());
                                               /* was au_bridge_stk(0x49923,0x46,cb_sel(EDX-dead),cb_off,cb_sel,…) */
    if (r != 0) return 0;
    G8(VA_g_sound_enabled + 0xd) = (uint8_t)(G8(VA_g_sound_enabled + 0xd) + 1);
    return 1;
}

/* remove_music_timer_event 0x15a30 (26 B) — remove the 0x7f4e0 timer when registered ([0x7f55d] bit0). */
void remove_music_timer_event(void)
{
    if (G8(VA_g_sound_enabled + 0xd) & 1) {
        os_audio_timer_remove_event((uint32_t)G32(VA_g_sos_digital_device + 0x4));   /* was au_bridge(0x49ca4,…) */
        G8(VA_g_sound_enabled + 0xd) = (uint8_t)(G8(VA_g_sound_enabled + 0xd) - 1);
    }
}

/* ============================================================ D. SOS client glue — lifecycle
 * (driver load/unload, init/shutdown, vtable install, the user-callback trampoline). Almost every
 * callee is a sanctioned host_audio_driver / DPMI / CRT bridge; verification = fail-branch oracle
 * (driver entries stubbed) + in-game live-swap at startup for the success paths. */

/* apply_audio_volume_settings 0x2626f (138 B) — settings menu/startup: read the four slider globals
 * (0x71b34/40/4c/58), &0xfff, <<7, clamp 0x7fff, and push them to the four volume sinks: SFX master
 * (set_71d84 [L]), speech rate/volume (set_voice_sample_rate [L]), 0x71388 (set_71388 [L]) and the music master
 * (set_music_master_volume, raw>>1 clamped 0x7f). ABI: void -> EAX = music setter ret. */
uint32_t apply_audio_volume_settings(void)
{
    uint32_t v1 = ((uint32_t)G32(VA_g_vol_soundfx) & 0xfff) << 7;
    uint32_t v2 = ((uint32_t)G32(VA_g_vol_speech) & 0xfff) << 7;
    uint32_t v3 = ((uint32_t)G32(VA_g_vol_movie) & 0xfff) << 7;
    uint32_t v4 = (uint32_t)G32(VA_g_vol_music) & 0xfff;
    if (v1 > 0x7fff) v1 = 0x7fff;
    if (v2 > 0x7fff) v2 = 0x7fff;
    if (v3 > 0x7fff) v3 = 0x7fff;
    set_71d84(v1);
    set_voice_sample_rate(v2);
    v4 >>= 1;
    set_71388(v3);
    if (v4 > 0x7f) v4 = 0x7f;
    return set_music_master_volume(v4);
}

/* free_sfx_scratch_buffer 0x15ec4 (30 B) — free [0x7f56c] via game_heap_free [L]. ABI: void. */
void free_sfx_scratch_buffer(void)
{
    if (G32(VA_g_reloc_base) != 0) {
        game_heap_free((uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_reloc_base));
        G32(VA_g_reloc_base) = 0;
    }
}

/* resolve_dbase100_sound_ids 0x1def8 (202 B) — startup pass over the dbase100 inventory table
 * ([0x81e20], count [dbase base 0x81e1c +0x10]): walk each entry's opcode records (headers at
 * rec+0x14, sub-record dword streams) and replace the low id word of every SOUND opcode
 * ({0x19,0x21,0x29,0x2a,0x31,0x32}) with its resolved sample index (find_sound_sample_index [L]) IN PLACE.
 * ABI: void. Pure obj3/heap transform -> oracle. */
void resolve_dbase100_sound_ids(void)
{
    if (G32(VA_g_dbase100_inventory_table) == 0) return;
    uint32_t base  = (uint32_t)G32(VA_g_dbase100_base);
    uint32_t count = ld32u(base + 0x10);
    if (count == 0) return;
    uint32_t tbl = (uint32_t)G32(VA_g_dbase100_inventory_table) + 4;
    for (uint32_t i = 0; i < count; i++, tbl += 4) {
        uint32_t rel = ld32u(tbl);
        if (rel == 0) continue;
        uint32_t p = rel + base + 0x14;
        for (;;) {
            uint32_t hdr = ld32u(p);
            p += 4;
            if (hdr == 0) break;
            uint32_t n = ((hdr & 0xffff) >> 2) - 1;      /* sar 2; dec (unsigned loop, faithful) */
            for (uint32_t j = 0; j < n; j++) {
                uint32_t w    = ld32u(p);
                uint32_t site = p;
                p += 4;
                uint32_t op = (w >> 24) & 0x7f;
                if (op == 0x19 || op == 0x21 || op == 0x29 || op == 0x2a || op == 0x31 || op == 0x32)
                    st16(site, (uint16_t)find_sound_sample_index((uint16_t)w));
            }
        }
    }
}

/* generic far call with 3 stack args (cdecl, caller-clean) — the trampoline's `call far [0x84900]` */
static void au_farcall3(uint32_t off, uint16_t sel, uint32_t a1, uint32_t a2, uint32_t a3)
{
    static struct { uint32_t off; uint16_t sel; } __attribute__((packed)) fp;
    static uint32_t s1, s2, s3;
    fp.off = off; fp.sel = sel;
    s1 = a1; s2 = a2; s3 = a3;
    __asm__ volatile(
        "pushl %2\n\t"
        "pushl %1\n\t"
        "pushl %0\n\t"
        "lcall *%3\n\t"
        "addl $12, %%esp"
        :
        : "m"(s1), "m"(s2), "m"(s3), "m"(fp)
        : "eax", "ebx", "ecx", "edx", "esi", "edi", "memory", "cc");
}

/* sos_user_callback_trampoline 0x27501 (203 B) — the FAR (retf) completion dispatcher the host driver
 * fires per voice event. Reads the voice userdata tag (get_w34 wrapper): 0xedXX = the SPEECH stream ->
 * far-call the registered g_voice_sos_callback 0x84900/0x84904 (call-closed to the lifted
 * voice_stream_sos_callback when it points at 0x1e487 — the only installer). Otherwise the tag is the
 * SFX handle-record index: type 0 (drained) either REQUEUES a still-audible looping node voice
 * (restamps the descriptor + set_callback on rec+0x26) or unlocks the descriptor; type 2 frees the
 * record. ABI: FAR stack args (voice, type, arg3) -> void; a live-swap ROOT. */
void sos_user_callback_trampoline(uint32_t voice, uint32_t type, uint32_t arg3)
{
    uint32_t tag = sos_voice_get_w34_wrapper(voice, arg3, type, arg3);
    if ((tag & 0xff00) == 0xed00) {
        uint32_t off = (uint32_t)G32(VA_g_voice_sos_callback);
        uint16_t sel = (uint16_t)G16(VA_g_voice_sos_callback + 0x4);
        if (off == (uint32_t)GADDR(0x1e487))
            voice_stream_sos_callback(voice, type, arg3);
        else
            au_farcall3(off, sel, voice, type, arg3);
        return;
    }
    uint32_t rec = (uint32_t)GADDR(VA_g_active_sound_handles) + tag * 0x9a;
    if (type == 0) {                                     /* jbe (unsigned) == 0 */
        uint32_t slot = (uint32_t)ld8(rec + 5) << 2;
        if (ld32u((uint32_t)GADDR(VA_g_sound_voice_descriptors) + slot) == 0) return;
        if (ld32u(rec) == 0) return;
        uint32_t hdr = ld32u(rec);
        uint32_t fl  = ld8(hdr + 8);
        if ((fl & 0x80) && ld16(rec + 0x10) != 0 && (fl & 7) == 1) {
            uint32_t desc = ld32u((uint32_t)GADDR(VA_g_sound_voice_descriptors) + slot);
            st16(desc + 0xc, (uint16_t)G16(VA_g_das_cache_tick));
            sos_voice_set_callback(voice, arg3, rec + 0x26);   /* requeue the loop voice */
            return;
        }
        uint32_t desc = ld32u((uint32_t)GADDR(VA_g_sound_voice_descriptors) + ((uint32_t)ld8(rec + 5) << 2));
        st8(desc + 8, (uint8_t)(ld8(desc + 8) - 1));     /* one-shot done -> unlock */
        return;
    }
    if (type == 2) { st32(rec, 0); return; }             /* end-of-sample -> free the record */
    /* type 1: ignored */
}

/* install_sos_driver_vtables 0x443a7 (364 B) — lock the driver regions (DPMI bridge), stash the driver
 * directory string into 0x7378c (far-strcpy bridge 0x531fb; empty when no path), then install the
 * three host driver far-method template tables (0x93198/0x93c10/0x970d4 clusters, 5 entries each,
 * {relocated code offset, CS}) and set the installed flag 0x93194. ABI: EAX=path off, DX=path sel,
 * EBX unused -> EAX = 0 | the DPMI lock error. */
uint32_t install_sos_driver_vtables(uint32_t path_off, uint32_t path_sel, uint32_t ebx_in)
{
    (void)ebx_in;
    /* NOT a os_audio_* seam — dpmi_lock_sos_driver_regions is DPMI real-mode lock
     * plumbing (class-a); it drops to a no-op under the virtual driver, so a re-point
     * would be a live behaviour change. Stays au_bridge/call_orig (zero-behaviour-change contract). */
    uint32_t r = au_bridge(0x43e71, 0, 0, 0, 0);         /* dpmi_lock_sos_driver_regions */
    if (r != 0) return r;
    /* NOT a driver boundary — 0x531fb is a plain far-strcpy util (class-a). Plain-lift or
     * keep, not a os_audio_* call. Stays au_bridge. */
    if (path_off != 0 || (path_sel & 0xffff) != 0)
        au_bridge(0x531fb, (uint32_t)GADDR(VA_g_midi_channel_raw_volume + 0x60), cur_ds(), path_off, path_sel & 0xffff);
    else
        G8(VA_g_midi_channel_raw_volume + 0x60) = 0;
    static const struct { uint32_t slot, fn; } vt[15] = {
        { 0x93198, 0x50450 }, { 0x9319e, 0x50480 }, { 0x931a4, 0x50499 },
        { 0x931aa, 0x504b2 }, { 0x931b0, 0x504cb },
        { 0x93c10, 0x50544 }, { 0x93c16, 0x5091f }, { 0x93c1c, 0x50ad0 },
        { 0x93c22, 0x50b4b }, { 0x93c28, 0x50b78 },
        { 0x970d4, 0x5322c }, { 0x970da, 0x536df }, { 0x970e0, 0x5389a },
        { 0x970e6, 0x53915 }, { 0x970ec, 0x53942 },
    };
    for (unsigned i = 0; i < 15; i++) {
        st16((uint32_t)GADDR(vt[i].slot) + 4, cur_cs());
        st32((uint32_t)GADDR(vt[i].slot), (uint32_t)GADDR(vt[i].fn));
    }
    G32(VA_g_sos_drivers_installed) = 1;
    return 0;
}

/* noop_ret_stub_1548c 0x1548c (6 B) — sos_load_driver's shared `leave/pops/ret` epilogue tail, carved
 * as a function by the boundary pass (load_dbase300_chunk tail-jmps into it; dead as a call target).
 * The C equivalent of "return through the caller's frame" is simply returning. */
void noop_ret_stub_1548c(void)
{
}

/* sos_load_driver 0x15290 (508 B) — load + configure the MUSIC driver for device [0x7f3ac]: install
 * the vtables, allocate the driver slot (4 stack-arg bridge; out-ptrs 0x7f37c/0x7f3b0), then the
 * per-device patch banks: OPL pair (0xa002/0xa009) loads two banks (build_game_path + load_file_fully
 * [bridge] -> sos_driver_call_m4); 0xa001 a single dispatch; 0xa004 loads a bank (its NAME comes out
 * of printf's LEFTOVER EBX — threaded from the bridge, gotcha H) then uploads 8 0x8a-stride patch
 * blocks (printf's leftover EDX likewise threaded); 0xa00a spawns an external player (CRT bridge).
 * Finally allocs the 0x11800 music chunk buffer from the game heap pool [L].
 * ABI: void -> EAX = 0 | 0xff on alloc/bank failure. In-game tier (one-shot startup, file I/O). */
uint32_t sos_load_driver(void)
{
    if (G32(VA_g_game_heap_handle + 0x38) == 0) return 0;
    G16(VA_g_game_heap_handle + 0x10) = 0;
    G32(VA_g_game_heap_handle + 0xc) = 0;
    if (G32(VA_g_game_heap_handle + 0x38) == 0xa00a)
        au_bridge(0x43dfc, (uint32_t)GADDR(VA_g_heap_free_list + 0x674), 0, 0, 0);   /* crt_spawn_via_comspec */
    install_sos_driver_vtables((uint32_t)GADDR(VA_g_dir_midi), cur_ds(), 0);
    G8(VA_g_audio_sequence_ctx + 0x4) = (uint8_t)(G8(VA_g_audio_sequence_ctx + 0x4) + 2);
    uint32_t r = os_audio_alloc_driver_slot((uint32_t)G32(VA_g_game_heap_handle + 0x38), (uint32_t)GADDR(VA_g_game_heap_handle + 0x2c),
                                            (uint32_t)GADDR(VA_g_game_heap_handle + 0x8), (uint32_t)GADDR(VA_g_game_heap_handle + 0x3c), cur_ds());
                                            /* was au_bridge_stk(0x44553,…): two {off,sel} far pairs collapsed */
    if (r != 0)
        return 0xff;                                     /* (the noop stub call = nothing) */
    G8(VA_g_audio_sequence_ctx + 0x4) = (uint8_t)(G8(VA_g_audio_sequence_ctx + 0x4) + 1);
    uint32_t dev = (uint32_t)G32(VA_g_game_heap_handle + 0x38);
    if (dev == 0xa002 || dev == 0xa009) {
        uint8_t buf[0x78];
        build_game_path(buf, (const uint8_t *)(uintptr_t)GADDR(VA_g_dir_midi),
                               (const uint8_t *)(uintptr_t)GADDR(VA_g_heap_free_list + 0x692));
        uint32_t p = load_file_fully((uint32_t)(uintptr_t)buf);   /* re-pointed: EAX=name -> EAX=buf/0 */
        G32(VA_g_dbase300_chunk_buf + 0x4) = (int32_t)p;
        if (p == 0) return 0xff;
        os_audio_driver_call_m4((uint32_t)G32(VA_g_game_heap_handle + 0x3c), 1, p, cur_ds());      /* was au_bridge(0x44cad,…) */
        build_game_path(buf, (const uint8_t *)(uintptr_t)GADDR(VA_g_dir_midi),
                               (const uint8_t *)(uintptr_t)GADDR(VA_g_heap_free_list + 0x69e));
        p = load_file_fully((uint32_t)(uintptr_t)buf);   /* re-pointed: load_file_fully */
        G32(VA_g_dbase300_chunk_buf + 0x8) = (int32_t)p;
        if (p == 0) return 0xff;
        os_audio_driver_call_m4((uint32_t)G32(VA_g_game_heap_handle + 0x3c), 1, p, cur_ds());      /* was au_bridge(0x44cad,…) */
    } else if (dev == 0xa001) {
        os_audio_driver_dispatch_simple((uint32_t)G32(VA_g_game_heap_handle + 0x3c), 0xb, (uint32_t)GADDR(VA_g_font_descriptor + 0x216), cur_ds());
    } else if (dev == 0xa004) {
        regs_t pr; memset(&pr, 0, sizeof pr);            /* printf(fmt@stack) with EBX = bank name */
        pr.ebx = (uint32_t)GADDR(VA_g_heap_free_list + 0x6b1);
        pr.nstack = 1; pr.stack[0] = (uint32_t)GADDR(VA_g_heap_free_list + 0x6a7);
        pr.va = 0x27c6d + OBJ_DELTA;
#ifndef ROTH_STANDALONE
        call_orig(&pr);
#else
        au_if_printf_literal(pr.stack[0]);   /* CRT printf, literal fmt (a004 driver-detect debug) */
#endif
        uint8_t buf[0x78];
        build_game_path(buf, (const uint8_t *)(uintptr_t)GADDR(VA_g_dir_midi),
                               (const uint8_t *)(uintptr_t)pr.ebx);           /* leftover EBX (H) */
        uint32_t p = load_file_fully((uint32_t)(uintptr_t)buf);   /* re-pointed: load_file_fully */
        G32(VA_g_dbase300_chunk_buf + 0xc) = (int32_t)p;
        if (p == 0) return 0xff;
        os_audio_driver_dispatch_simple((uint32_t)G32(VA_g_game_heap_handle + 0x3c), 0xb, (uint32_t)GADDR(VA_g_font_descriptor + 0x222), cur_ds());
        uint32_t esi = 0;
        for (;;) {
            regs_t p2; memset(&p2, 0, sizeof p2);
            p2.edx = 0x8a;
            p2.nstack = 1; p2.stack[0] = (uint32_t)GADDR(VA_g_heap_free_list + 0x6bd);
            p2.va = 0x27c6d + OBJ_DELTA;
#ifndef ROTH_STANDALONE
            call_orig(&p2);
#else
            au_if_printf_literal(p2.stack[0]);   /* CRT printf, literal fmt (a004 driver-load debug) */
#endif
            uint32_t b = (uint32_t)G32(VA_g_dbase300_chunk_buf + 0xc) + esi;
            esi += 0x8a;
            os_audio_driver_dispatch_simple((uint32_t)G32(VA_g_game_heap_handle + 0x3c), p2.edx, b, cur_ds()); /* leftover EDX (H); was au_bridge(0x45d28,…) */
            if (esi == 0x450) break;
        }
    }
    uint32_t chunk = pool_alloc_checked((uint32_t)G32(VA_g_game_heap_handle), 0x11800);
    G32(VA_g_dbase300_chunk_buf) = (int32_t)chunk;
    if (chunk != 0) G32(VA_g_audio_sequence_ctx + 0x8) = -1;
    return 0;
}

/* sos_unload_driver 0x15702 (257 B) — stop the music, release the driver slot (device-specific 0xb
 * dispatch first for 0xa001/0xa004), free the loaded patch banks (free_block_or_pool 0x15280 bridge),
 * disable the audio callback (flag bit1), free the music chunk back to the heap pool [L]. ABI: void. */
void sos_unload_driver(void)
{
    stop_music_sequence();
    if (G8(VA_g_audio_sequence_ctx + 0x4) & 1) {
        uint32_t dev = (uint32_t)G32(VA_g_game_heap_handle + 0x38);
        if (dev == 0xa001)
            os_audio_driver_dispatch_simple((uint32_t)G32(VA_g_game_heap_handle + 0x3c), 0xb, (uint32_t)GADDR(VA_g_font_descriptor + 0x216), cur_ds());
        else if (dev == 0xa004)
            os_audio_driver_dispatch_simple((uint32_t)G32(VA_g_game_heap_handle + 0x3c), 0xb, (uint32_t)GADDR(VA_g_font_descriptor + 0x222), cur_ds());
        os_audio_free_driver_slot((uint32_t)G32(VA_g_game_heap_handle + 0x3c), 1);     /* was au_bridge(0x44a81,…): sos_free_driver_slot */
        /* re-pointed: 0x15280 free_block_or_pool (lifted; EAX=block; high-addr -> game_heap_free) */
        if (G32(VA_g_dbase300_chunk_buf + 0xc) != 0) { free_block_or_pool((uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_dbase300_chunk_buf + 0xc)); G32(VA_g_dbase300_chunk_buf + 0xc) = 0; }
        if (G32(VA_g_dbase300_chunk_buf + 0x4) != 0) { free_block_or_pool((uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_dbase300_chunk_buf + 0x4)); G32(VA_g_dbase300_chunk_buf + 0x4) = 0; }
        if (G32(VA_g_dbase300_chunk_buf + 0x8) != 0) { free_block_or_pool((uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_dbase300_chunk_buf + 0x8)); G32(VA_g_dbase300_chunk_buf + 0x8) = 0; }
        G8(VA_g_audio_sequence_ctx + 0x4) = (uint8_t)(G8(VA_g_audio_sequence_ctx + 0x4) - 1);
    }
    if (G8(VA_g_audio_sequence_ctx + 0x4) & 2) {
        os_audio_disable_callback();                     /* was au_bridge(0x47d6e,…): sos_disable_audio_callback */
        G8(VA_g_audio_sequence_ctx + 0x4) = (uint8_t)(G8(VA_g_audio_sequence_ctx + 0x4) - 2);
    }
    if (G32(VA_g_dbase300_chunk_buf) != 0) {
        pool_free_chunk((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_game_heap_handle),
                               (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_dbase300_chunk_buf));
        G32(VA_g_dbase300_chunk_buf) = 0;
        G32(VA_g_audio_sequence_progress) = 0;
    }
}

/* sos_audio_shutdown 0x15ac8 (112 B) — full teardown: unload the music driver, then unwind the
 * [0x7f554] init flags in order: bit3 house timer, bit2 close voices, bit0 audio callback, bit1 timer
 * service (all driver bridges). ABI: void. */
void sos_audio_shutdown(void)
{
    sos_unload_driver();
    if (G8(VA_g_sound_enabled + 0x4) & 8) {
        os_audio_timer_remove_event((uint32_t)G32(VA_g_audio_signed_samples + 0x60));      /* was au_bridge(0x49ca4,…) */
        G8(VA_g_sound_enabled + 0x4) = (uint8_t)(G8(VA_g_sound_enabled + 0x4) - 8);
    }
    if (G8(VA_g_sound_enabled + 0x4) & 4) {
        os_audio_close_voices((uint32_t)G32(VA_g_sos_digital_device), 1, 1);      /* was au_bridge(0x48666,…): sos_driver_close_voices */
        G8(VA_g_sound_enabled + 0x4) = (uint8_t)(G8(VA_g_sound_enabled + 0x4) - 4);
    }
    if (G8(VA_g_sound_enabled + 0x4) & 1) {
        os_audio_disable_callback();                     /* was au_bridge(0x47d6e,…) */
        G8(VA_g_sound_enabled + 0x4) = (uint8_t)(G8(VA_g_sound_enabled + 0x4) - 1);
    }
    if (G8(VA_g_sound_enabled + 0x4) & 2) {
        os_audio_stop_timer_service();                   /* was au_bridge(0x498e9,0,0,0,0): sos_stop_timer_service */
        G8(VA_g_sound_enabled + 0x4) = (uint8_t)(G8(VA_g_sound_enabled + 0x4) - 2);
    }
}

/* sos_audio_init 0x15813 (458 B) — startup: parse the audio config (parse_config_keywords bridge; on
 * failure print + latch [0x7675a]=0xff, return 0), latch the device/config words (digital [0x7f548],
 * music [0x7f3ac], ports/IRQ/DMA...), detect the digital device (load/find/unload detection driver
 * bridges; failure prints + returns 0) and enable the audio callback; configure the timer (0xff00),
 * size the mix buffer (0x800/0x1000/x2 by config), open the digital voices (4 stack-arg bridge; sets
 * the SFX-enable [0x7f550]=-1), register the 60 Hz house timer (far cb from [0x7f4a8/0x7f4ac]), and
 * chain into sos_load_driver. ABI: void -> EAX = 1 ok / 0 failed. In-game tier (one-shot startup). */
uint32_t sos_audio_init(void)
{
    uint8_t cfg[0x400];
    /* parse_config_keywords 0x41c5c — DIRECT C. Lifted (EAX=file,EDX=tmpl,EBX=struct,ECX=cap
     * -> EAX=struct/0); the file_config family sites were already direct-C, this audio site was the last
     * bridge and it retires the target. In-game g_os_soft_int is the host int21 -> reads the real
     * CONFIG.INI. Oracle (test_audio audio_init[ok]/[cfgfail]): the "local stack buffer, can't
     * pre-stage text, 4-byte read slot won't fit a copy" blocker is solved WITHOUT a detour trampoline —
     * the LIFT side runs the real parser under a COPYING int21 mock (audio_kw_mock memcpy's the crafted
     * CONFIG text into io->edx at ah==0x3f) and the ORIGINAL side is redirected onto a known static buffer
     * (byte-patch 0x1581f `lea ebx,[ebp-0x400]` -> `mov ebx,au_cfg`) with the text pre-staged there + the
     * three int21 sites 0x41cb8/0x41d04/0x41d18 replaced by the t_keywords semantic sims (open->handle0,
     * read->`lea eax,[ecx-1]` no-read, close->nop). Both sides then parse byte-identical text @struct+0x9c
     * (sz=0x1c for template @0x75cdc) -> identical device record -> the latched obj3 write-set matches by
     * construction. The record buffers differ in address but are only dereferenced (never stored), so the
     * difference is invisible. */
    uint32_t rec = (uint32_t)parse_config_keywords((uint8_t *)GADDR(VA_g_heap_free_list + 0x756),
                                                          (uint8_t *)GADDR(VA_g_heap_free_list + 0x6cc), cfg, 0x400);
    if (rec == 0) {
        os_dos_print_string((uint32_t)GADDR(VA_g_heap_free_list + 0x761));   /* re-pointed: was au_bridge(0x27c48) dos_print_string (int21 AH=09; EAX->EDX) -> c2 DOS contract */
        G8(VA_g_player_movement_enabled + 0x10) = 0xff;
        return 0;
    }
    G32(VA_g_audio_signed_samples + 0x4) = (int32_t)(uint32_t)ld16(rec + 0x10);
    G32(VA_g_audio_signed_samples + 0xc) = (int32_t)(uint32_t)ld16(rec + 0x14);
    G32(VA_g_audio_signed_samples + 0x10) = 0;
    G32(VA_g_audio_signed_samples + 0x8) = (int32_t)(uint32_t)ld16(rec + 0x12);
    G32(VA_g_sos_digital_device + 0x6c) = (int32_t)(uint32_t)ld16(rec + 0xe);
    G32(VA_g_game_heap_handle + 0x38) = (int32_t)(uint32_t)ld16(rec + 0x16);
    G32(VA_g_game_heap_handle + 0x2c) = (int32_t)(uint32_t)ld16(rec + 0x18);
    G32(VA_g_game_heap_handle + 0x34) = 0;
    G32(VA_g_game_heap_handle + 0x30) = (int32_t)(uint32_t)ld16(rec + 0x1a);
    if (G32(VA_g_sos_digital_device + 0x6c) != 0) {
        uint32_t st = os_audio_load_detection_driver((uint32_t)GADDR(VA_g_dir_digi), cur_ds()); /* was au_bridge(0x48b21,…) */
        if (st == 0) {
            st = os_audio_find_driver_for_device((uint32_t)G32(VA_g_sos_digital_device + 0x6c), (uint32_t)GADDR(VA_g_sos_digital_device + 0x8), cur_ds()); /* was 0x48f79 */
            os_audio_unload_detection_driver();          /* was au_bridge(0x48c6b,…) */
        }
        if (st != 0) {
            os_dos_print_string((uint32_t)GADDR(VA_g_heap_free_list + 0x79d));   /* re-pointed: was au_bridge(0x27c48) dos_print_string -> c2 DOS contract */
            return 0;
        }
        os_audio_enable_callback((uint32_t)GADDR(VA_g_dir_digi), cur_ds());   /* was au_bridge(0x47cf5,…): sos_enable_audio_callback */
        G8(VA_g_sound_enabled + 0x4) = (uint8_t)(G8(VA_g_sound_enabled + 0x4) + 1);
    }
    os_audio_configure_timer_rate(0xff00, 0);            /* was au_bridge(0x4980d,0xff00,0,0,0): sos_configure_timer_rate */
    G8(VA_g_sound_enabled + 0x4) = (uint8_t)(G8(VA_g_sound_enabled + 0x4) + 2);
    G32(VA_g_audio_signed_samples + 0x24) = G32(VA_g_font_descriptor + 0x22e);
    uint32_t sz = 0x800;
    if (G32(VA_g_sos_digital_device + 0x30) != 0) sz = 0x1000;
    if ((uint32_t)G32(VA_g_sos_digital_device + 0x2c) > 8) { G32(VA_g_audio_signed_samples) = 0x10; sz += sz; }
    G32(VA_g_audio_signed_samples + 0x20) = 1;
    G32(VA_g_audio_signed_samples + 0x58) = 0;
    G32(VA_g_audio_signed_samples + 0x14) = (int32_t)sz;
    if (G32(VA_g_sos_digital_device + 0x6c) != 0) {
        uint32_t r = os_audio_open_voices((uint32_t)G32(VA_g_sos_digital_device + 0x6c), (uint32_t)GADDR(VA_g_audio_signed_samples + 0x4),
                                          (uint32_t)GADDR(VA_g_audio_signed_samples + 0x14), (uint32_t)GADDR(VA_g_sos_digital_device), cur_ds());
                                          /* was au_bridge_stk(0x47dae,…): req/size/handle {off,sel} far pairs collapsed */
        if (r != 0) return 0;
        G32(VA_g_sound_enabled) = -1;
        G8(VA_g_sound_enabled + 0x4) = (uint8_t)(G8(VA_g_sound_enabled + 0x4) + 4);
    }
    uint32_t r2 = os_audio_timer_register_event(0x3c, (uint32_t)G32(VA_g_audio_signed_samples + 0x30), (uint16_t)G16(VA_g_audio_signed_samples + 0x34),
                                                (uint32_t)GADDR(VA_g_audio_signed_samples + 0x60), cur_ds());
                                                /* was au_bridge_stk(0x49923,…): house-timer cb far pair collapsed */
    if (r2 != 0) return 0;
    G8(VA_g_sound_enabled + 0x4) = (uint8_t)(G8(VA_g_sound_enabled + 0x4) + 8);
    if (G32(VA_g_sound_enabled) != 0) sos_load_driver();
    return 1;
}
