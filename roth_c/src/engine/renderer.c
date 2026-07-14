/* C reimplementations of original ROTH functions, verified byte-equivalent
 * against the originals by oracle.c. Each is a candidate "lift". */
#include "common.h"   /* GADDR + G8/G16/G32 + shared lift machinery (was inline here) */
#include <string.h>

/* int3 suspend/resume hooks: NULL in the oracle (no live-swap), set by the host's lift_install. */
void (*g_os_suspend_int3s)(void) = NULL;
void (*g_os_resume_int3s)(void)  = NULL;
uint16_t g_os_game_ds = 0;   /* the game's DS at lift time (host sets it per dispatch); the native rasterizer's indirect-resolve path needs it */
uint16_t g_os_game_cs = 0;   /* the game's CS at lift time (host sets it per dispatch); game_core's roth_main_sequence needs it for the far-pointer timer-event registration (mov dx,cs) */

/* call_orig wrapper: bracket the original run with a host-driven suspend/resume of ALL live-swapped
 * int3s, so the original function AND its entire call subtree run without re-trapping the non-reentrant
 * signal handler. This is what lets one lifted function's call_orig bridge transitively reach another
 * lifted entry under ROTH_LIFT=all (e.g. the sprite edge-walker reusing the floor/ceil rasterizer
 * 0x3b1c1; the wall driver's resolve bridge reaching zero_memory). The depth guard makes nested
 * call_origs no-ops (only the outermost suspends), so resume can't re-arm mid-subtree. No-op when the
 * hooks are unset (oracle) — then it is exactly the old call_orig. */
/* Shared by call_orig + host_run_isr_heartbeat so the int3 suspend/resume bracket is depth-correct:
 * when the SIGALRM surrogate's heartbeat fires nested inside the main dispatch's own call_orig (depth>0),
 * it must NOT resume (re-plant) the int3s the outer call is relying on being suspended. */
static int g_call_orig_depth = 0;

void call_orig(regs_t *io)
{
    int top = (g_call_orig_depth++ == 0);
    if (top && g_os_suspend_int3s) g_os_suspend_int3s();
    call_orig_raw(io);
    if (top && g_os_resume_int3s) g_os_resume_int3s();
    g_call_orig_depth--;
}

/* host_run_isr_heartbeat — host surrogate hook for a GAMEPLAY interactive live-swap.
 *
 * During ROTH_LIFT=game_play_loop (or any loop-tier swap) the trap holds g_in_handler, so the game's
 * int-8 (timer) ISR is frozen — the player position, software cursor and game clock stall. This runs the
 * near-safe heartbeat body so they advance:
 *     vsync_timer_tick 0x122e3 (near `ret` @0x12313):
 *       inc word[0x90bcc]                             ; g_frame_tick_counter
 *       if (g_player_movement_enabled 0x7674a & 1):   ; in gameplay
 *           update_software_cursor 0x116b6 + player_movement_tick 0x12520
 *       if ([0x7e8d4]): call [0x7e8d4]                ; the installed frame-tick hook
 * This is the game's own int-8 near-call target in BOTH timer configs (music on: it's the SOS event
 * 0x1231b->0x122e3; music off: 0x12336 near-calls it directly), so near-calling it is faithful and
 * near-safe for movement/cursor/clock.
 *
 * WHAT THIS DELIBERATELY DOES NOT DO — game AUDIO under the swap:
 *   - Digital SFX / voice are host-emulated (the SOS digital poll runs off the host audio-output tick in
 *     audio.c, independent of the frozen game ISRs), so they sound under the swap with no game-ISR call.
 *   - MIDI music is a SEPARATE SOS timer event, sos_sequence_timer_tick 0x51ad5 (FAR), stepped per its own
 *     registered rate with a per-track context arg. It is NOT reachable via a simple call:
 *       * 0x49eaf (the SOS dispatch that fans it out) is NOT a callable leaf — it's the body the DOS/4GW
 *         PM interrupt-reflection wrapper 0x54b05 invokes after saving the interrupted SS:ESP into
 *         [0x755aa]; jumping straight to it crashes in the reflection epilogue. And
 *       * step_audio_sequence 0x46d18 only ARMS the event (re-registers 0x51ad5), it doesn't step it.
 *     Driving MIDI correctly under the swap needs 0x51ad5 with its context arg at its registered rate — a
 *     careful follow-up (call_orig_far can far-call the leaf). Deferred: MIDI-under-swap is a live-swap
 *     nicety, not needed to validate game_core, and the final native port drives MIDI off its own audio
 *     timer anyway. So the swap plays movement + SFX; music is silent under it for now.
 *
 * Reentrant-safe: the trampoline saves/restores its state, so this is fine nested inside the main
 * dispatch's own call_orig bridges; the nested int 0x33 (poll_mouse_input) is serviced by the trap
 * handler's nested path. */
void host_run_isr_heartbeat(void)
{
    int top = (g_call_orig_depth++ == 0);
    if (top && g_os_suspend_int3s) g_os_suspend_int3s();
    regs_t io;
    memset(&io, 0, sizeof io);
    /* [KEPT-REPOINT: vsync_timer_tick 0x122e3 — POLICY keep (not a debt to burn). This IS the host
     * ISR surrogate: running the ORIGINAL near-callable heartbeat body under the int3 suspend/resume
     * bracket is the whole point (ISR fidelity under the gameplay live-swap). A direct C call would
     * defeat the surrogate. Stays call_orig by design. (w14)] */
    io.va = 0x122e3u + OBJ_DELTA;   /* vsync_timer_tick — near-callable heartbeat body [HOST-BOUNDARY] */
    call_orig_raw(&io);
    if (top && g_os_resume_int3s) g_os_resume_int3s();
    g_call_orig_depth--;
}

/* host_step_midi_tracks — OPT-IN (ROTH_LOOP_AUDIO=1) MIDI music under a loop-tier live-swap.
 *
 * The per-tick MIDI step sos_sequence_timer_tick 0x51ad5 is a FAR SOS event, normally far-called by
 * sos_timer_dispatch 0x49eaf. Unlike 0x49eaf it is NOT entangled with the DPMI interrupt-reflection
 * wrapper (it's a leaf callback, not the IRQ0 vector), so call_orig_far drives it directly. It takes no
 * register/stack arg — it reads the active track from a global byte [0x97ae4], which the dispatcher
 * publishes right before the far-call (verified from 0x49f54 / 0x51aea). So we replicate the
 * dispatcher: for each active track (per-track active flag [0x93144+track*4]==1, set at arm time by
 * step_audio_sequence 0x46d18), publish the track then far-call 0x51ad5. Per SIGALRM ≈ the registered
 * MIDI rate (MIDI is the fastest registered event → the PIT/SIGALRM rate). Default OFF — a live-swap
 * nicety only; the crash-tested reason it's opt-in is that it far-calls the SOS/MIDI driver from the
 * surrogate (the audio-reentrancy path). Untested in-game as of this writing. */
void host_step_midi_tracks(void)
{
    int top = (g_call_orig_depth++ == 0);
    if (top && g_os_suspend_int3s) g_os_suspend_int3s();
    for (int track = 0; track < 8; track++) {          /* a handful of tracks; scanning 8 is safe */
        if ((uint32_t)G32((VA_g_sos_driver_vtable + 0x1a8) + (uint32_t)track * 4) == 1) {  /* track active? */
            G8(VA_g_sos_timer_event_table + 0x120) = (uint8_t)track;              /* publish "current track" (the ctx channel 0x51ad5 reads) */
            call_orig_far(0x51ad5u + OBJ_DELTA);       /* the MIDI sequencer step (FAR leaf, wrapper-free) */
        }
    }
    if (top && g_os_resume_int3s) g_os_resume_int3s();
    g_call_orig_depth--;
}

/* sincos_lookup — canon 0x3c1f3:
 *     and    ebx, 0x3fe
 *     movsx  edx, WORD PTR [ebx + 0x472080]   ; g_sincos_table (rebased)
 *     ret
 * Input:  EBX  (byte offset into the 512-entry sine table; masked word-aligned)
 * Output: EDX  (table word, sign-extended 16 -> 32)
 * Pure leaf, no segment-register / DPMI-selector use. */
int32_t sincos_lookup(uint32_t ebx)
{
    const int16_t *g_sincos_table = (const int16_t *)GADDR(VA_g_sincos_table);
    uint32_t off = ebx & 0x3feu;            /* mask to even, 0..0x3fe */
    return (int32_t)g_sincos_table[off >> 1];   /* movsx: sign-extend 16->32 */
}

/* zero_memory — canon 0x41616:
 *     mov edi,eax; xor eax,eax; mov ecx,edx; shr ecx,2; rep stosd;
 *     mov ecx,edx; and ecx,3; rep stosb   (push ds;pop es => ES = flat)
 * Input: EAX = dest, EDX = byte length. Net effect: exactly `len` zero bytes at
 * dest (dword fill + byte tail). Leaf; ES is only saved/restored (flat). */
void zero_memory(void *dest, uint32_t len)
{
    unsigned char *d = (unsigned char *)dest;
    for (uint32_t i = 0; i < len; i++)
        d[i] = 0;
}

/* isqrt16_bsearch (FUN_0003c068): 16-bit binary-search integer sqrt; the kernel
 * that builds isqrt_fixed's lookup table. EAX in -> EAX out. All arithmetic is
 * 16-bit with a WRAPPING midpoint ((si+di) mod 2^16 >> 1), faithfully replicated.
 *   if ((int16)((in>>16) - (in_lo16 < 2)) < 0) return in;     // js skip-path
 *   si=0; di=0xfffe;
 *   do { mid = ((u16)(si+di)) >> 1;
 *        if ((int32)(mid*mid - in) >= 0) di = mid; else si = mid;
 *   } while ((int16)(u16)(di - si) > 1);
 *   return (u16)di; */
uint32_t isqrt16_bsearch(uint32_t eax)
{
    uint16_t lo16 = (uint16_t)eax;
    uint16_t hi16 = (uint16_t)(eax >> 16);
    unsigned cf   = (lo16 < 2) ? 1u : 0u;           /* sub ax,2 -> CF = (lo16 < 2) */
    uint16_t dxa  = (uint16_t)(hi16 - cf);          /* sbb dx,0 */
    if ((int16_t)dxa < 0)                           /* js -> return input unchanged */
        return eax;
    uint16_t si = 0, di = 0xfffe;
    do {
        uint16_t mid = (uint16_t)((uint16_t)(si + di) >> 1);   /* add ax,di; shr ax,1 */
        uint32_t sq  = (uint32_t)mid * (uint32_t)mid;          /* mul ax -> DX:AX */
        if ((int32_t)(sq - eax) >= 0)               /* sub ax,bx; sbb dx,cx; jns */
            di = mid;
        else
            si = mid;
    } while ((int16_t)(uint16_t)(di - si) > 1);     /* sub ax,si; cmp ax,1; jg */
    return (uint32_t)di;                            /* movzx eax,di */
}

/* isqrt_fixed — canon 0x3bfe5: fixed-point integer sqrt via a 0x300-entry table at
 * 0x8a446, lazily built on the first call (guard word at 0x8a444; built with
 * isqrt16_bsearch). Compute path:
 *     bsr ecx,eax; (eax==0 -> return 0); ecx = (31-bit) & ~1 = shift;
 *     eax <<= shift; ecx = shift>>1 = half; ebx = (eax>>22) - 0x100 = index;
 *     ax = (u16) table[index]; eax >>= half; edx = eax
 * Input: EAX = value. Output: EAX (mirrored to EDX) = sqrt. Complete lift (build +
 * compute) — no warm-up required. */
int32_t isqrt_fixed(uint32_t eax)
{
    if ((uint16_t)G16(VA_g_floorceil_edge_emitted + 0x4) == 0) {              /* test word[0x8a444]; je build */
        uint32_t v = 0x10000000u;
        for (int i = 0; i < 0x300; i++) {
            uint32_t r = isqrt16_bsearch(v);             /* sVar2 = FUN_0003c068(v) */
            G16((VA_g_floorceil_edge_emitted + 0x6) + i * 2) = (uint16_t)((r + r) & 0xffffu); /* *psVar6 = sVar2 * 2 */
            v += 0x100000u;
        }
        G16(VA_g_floorceil_edge_emitted + 0x4) = (uint16_t)0xffff;            /* _DAT_0008a444 = -1 */
    }
    if (eax == 0)
        return 0;                                   /* bsr ZF -> early ret, eax=0 */
    int bit = 31 - __builtin_clz(eax);              /* bsr eax */
    uint32_t shift = (uint32_t)(31 - bit) & 0xfeu;  /* sub/neg/and cl,0xfe */
    uint32_t norm = eax << shift;
    uint32_t half = shift >> 1;                      /* shr ecx,1 */
    uint32_t index = (norm >> 22) - 0x100u;          /* shr ebx,0x16; dec bh */
    return (int32_t)((uint32_t)(uint16_t)G16((VA_g_floorceil_edge_emitted + 0x6) + index * 2) >> half);
}

/* ======================= Batch 1 — Tier-1 leaves ======================= */

/* move_input_* (0x126a4/0x126ad): or word [g_move_input_bits], <bit> ; ret */
void move_input_strafe_right(void) { G16(VA_g_move_input_bits) |= 0x0002; }
void move_input_strafe_left(void)  { G16(VA_g_move_input_bits) |= 0x0008; }

/* move_input_forward/backward (0x12686/0x12668): set the bit unless a dialogue
 * freeze is active (busy != 0 AND freeze-gate == 0x6ffff). */
void move_input_forward(void)
{
    if (G32(VA_g_dialogue_busy_flag) != 0 && G32(VA_g_move_freeze_gate) == 0x6ffff)
        return;
    G16(VA_g_move_input_bits) |= 0x0001;
}
void move_input_backward(void)
{
    if (G32(VA_g_dialogue_busy_flag) != 0 && G32(VA_g_move_freeze_gate) == 0x6ffff)
        return;
    G16(VA_g_move_input_bits) |= 0x0004;
}

/* look_pitch_up/down (0x12927/0x12939): adjust g_view_pitch, mirror to applied. */
void look_pitch_up(void)   { G32(VA_g_view_pitch) += 2; G32(VA_g_view_pitch_applied) = G32(VA_g_view_pitch); }
void look_pitch_down(void) { G32(VA_g_view_pitch) -= 2; G32(VA_g_view_pitch_applied) = G32(VA_g_view_pitch); }

/* look_pitch_recenter_down (0x1294b): dump a nonzero accumulator into applied,
 * clear it, then always nudge applied down by 8. */
void look_pitch_recenter_down(void)
{
    int32_t pitch = G32(VA_g_view_pitch);
    if (pitch != 0) {
        G32(VA_g_view_pitch) = 0;
        G32(VA_g_view_pitch_applied) = pitch;
    }
    G32(VA_g_view_pitch_applied) += -8;
}

/* apply_render_mode (0x14506): signed-16-bit 3-state -> flat-shading byte.
 * mode 0 -> 0x00, mode 1 -> 0xff, mode >= 2 -> no write. */
void apply_render_mode(void)
{
    int16_t m = (int16_t)G16(VA_g_render_mode);
    m = (int16_t)(m - 1); if (m < 0) { G8(VA_g_flat_shading_flag) = 0x00; return; }
    m = (int16_t)(m - 1); if (m < 0) { G8(VA_g_flat_shading_flag) = 0xff; return; }
    /* mode >= 2: leave g_flat_shading_flag unchanged */
}

/* commit_player_position_delta (0x34bb2): add the LOW 16 bits of each 32-bit
 * delta to the 16-bit player position word (no carry into the high word). */
void commit_player_position_delta(void)
{
    G16(VA_g_player_x) = (uint16_t)(G16(VA_g_player_x) + (uint16_t)G32(VA_g_player_move_delta_x)); /* x */
    G16(VA_g_player_y) = (uint16_t)(G16(VA_g_player_y) + (uint16_t)G32(VA_g_player_move_delta_y)); /* y */
    G16(VA_g_player_z) = (uint16_t)(G16(VA_g_player_z) + (uint16_t)G32(VA_g_player_move_delta_z)); /* z */
}

/* key_z_crouch (0x1c5d0): net write-set is g_player_locomotion_flags |= 0x24
 * (the eax sub-state OR/branch is a no-op for memory). */
void key_z_crouch(void) { G8(VA_g_player_locomotion_flags) |= 0x24; }

/* apply_literal_skip_delta_stream (0x4eeae): EAX=dst, EDX=src. Sparse-patch
 * decoder — 0 ends; 0x01..0x7f = copy N literals; 0x80..0xff = skip (N-0x80). */
void apply_literal_skip_delta_stream(uint8_t *dst, const uint8_t *src)
{
    for (;;) {
        uint8_t b = *src++;
        if (b == 0)
            return;
        if (b < 0x80) {
            memcpy(dst, src, b);
            dst += b;
            src += b;
        } else {
            dst += (uint8_t)(b - 0x80);
        }
    }
}

/* save_sfx_node_active_state (0x43d53): pack bit-0x80 of 0x12-stride records
 * (count = high word of *g_sfx_nodes) MSB-first into out[]; returns
 * bytes written (4 * ceil(count/32)). */
uint32_t save_sfx_node_active_state(uint32_t *out)
{
    uint8_t *base = *(uint8_t **)(uintptr_t)(0x85c44u + OBJ_DELTA);
    int32_t count = (int32_t)(*(uint32_t *)base >> 16);
    uint8_t *rec = base + 4;
    uint32_t *p = out;
    /* outer loop is POST-tested (do-while @0x43d8c): one dword is always written,
     * so count==0 returns 4, not 0 — oracle-caught. */
    do {
        uint32_t acc = 0;
        for (int bit = 0x20; bit > 0; bit--) {
            acc <<= 1;
            if (count > 0 && (rec[8] & 0x80))
                acc |= 1;
            rec += 0x12;
            count--;
        }
        *p++ = acc;
    } while (count > 0);
    return (uint32_t)((uint8_t *)p - (uint8_t *)out);
}

/* ======================= Batch 2 — more leaves ======================= */

/* turn_input_left/right (0x126b6/0x12703): ramp the 16-bit signed turn
 * accumulator toward +/-max (widened when running), snapping when reversing.
 * Asymmetric at acc==0 (faithful): left ramps +1, right snaps -2. */
void turn_input_left(void)
{
    int16_t max = (G32(VA_g_run_active) != 0) ? 0x0d : 0x08;     /* g_run_active widens */
    int16_t acc = (int16_t)G16(VA_g_turn_accum);                 /* g_turn_accum */
    if (acc < 0)         G16(VA_g_turn_accum) = 2;
    else if (acc >= max) G16(VA_g_turn_accum) = (uint16_t)max;
    else                 G16(VA_g_turn_accum) = (uint16_t)(acc + 1);
    G16(VA_g_move_input_bits) |= 0x10;                                /* turning bit */
}
void turn_input_right(void)
{
    int16_t max = (G32(VA_g_run_active) != 0) ? (int16_t)-0x0d : (int16_t)-0x08;
    int16_t acc = (int16_t)G16(VA_g_turn_accum);
    if (acc >= 0)        G16(VA_g_turn_accum) = (uint16_t)(int16_t)-2;
    else if (acc <= max) G16(VA_g_turn_accum) = (uint16_t)max;
    else                 G16(VA_g_turn_accum) = (uint16_t)(acc - 1);
    G16(VA_g_move_input_bits) |= 0x10;
}

/* key_t_handler_vestigial (0x14ca7): dormant T-key toggle. */
void key_t_handler_vestigial(void)
{
    G8(VA_g_debug_map_enabled + 0x2) ^= 1;
    G8(VA_g_reloc_base + 0x4) = 1;
}

/* dev_toggle_map_menu (0x17fa4): W-key. dev on -> toggle bit0 (byte); dev off
 * -> clear the whole dword (faithful asymmetry). */
void dev_toggle_map_menu(void)
{
    if (G8(VA_g_dev_mode_flag) & 1)
        G8(VA_g_map_menu_active) ^= 1;        /* on: byte xor */
    else
        G32(VA_g_map_menu_active) = 0;        /* off: dword clear */
}

/* measure_font_char_advance (0x1508a): EAX=ch -> EAX=glyph advance width.
 * control chars (<=0x0d) -> 0; no per-char entry -> default advance; else
 * (glyph[0] low nibble)+1. Per-char offset table is SIGNED, relative to base. */
uint32_t measure_font_char_advance(uint32_t ch)
{
    const uint8_t *base = (const uint8_t *)(uintptr_t)(0x70f12u + OBJ_DELTA); /* g_font_descriptor */
    ch &= 0xff;
    if (ch <= 0x0d)
        return 0;
    int16_t off = *(const int16_t *)(base + 6 + ch * 2);
    if (off == 0)
        return *(const uint16_t *)(base + 4);
    uint8_t glyph0 = *(base + off);
    return (uint32_t)(glyph0 & 0x0f) + 1;
}

/* ======================= Batch 3 ======================= */

/* evict_das_cache_slot (0x41385): LRU-scan g_das_cache_slots for the oldest
 * evictable slot (max tick-age), record it in g_current_das_cache_slot[_index],
 * and report success. Returns 1 (CF clear) = a slot was selected, 0 (CF set) =
 * none evictable. The success path frees the selected slot's resources via
 * release_das_cache_slot_resources (0x413fd) exactly as the original does —
 * supplied by lift_das_assets.c. */
int evict_das_cache_slot(uint16_t forbid_current_tick)
{
    uint8_t *slot = (uint8_t *)(uintptr_t)(0x89930u + OBJ_DELTA);   /* g_das_cache_slots */
    uint8_t *best = (uint8_t *)0;
    uint16_t best_age = 0;
    uint8_t  best_idx = 0;
    for (unsigned i = 0; i < 0xf0u; i++, slot += 6) {
        uint16_t tick = *(volatile uint16_t *)(uintptr_t)(0x90c0au + OBJ_DELTA); /* re-read each iter */
        uint16_t age = (uint16_t)(tick - *(uint16_t *)(slot + 4));
        if (age < best_age)
            continue;
        if (*(uint32_t *)slot == 0)
            continue;                          /* empty: not evictable */
        if (age != 0) {
            best_age = age; best = slot; best_idx = (uint8_t)i;
        } else {                               /* touched this tick: extra gates */
            if (forbid_current_tick != 0)
                continue;
            if (*(uint32_t *)slot == *(uint32_t *)(uintptr_t)(0x85400u + OBJ_DELTA))
                continue;                      /* pinned/active block */
            best_age = 0; best = slot; best_idx = (uint8_t)i;
        }
    }
    if (best == (uint8_t *)0)
        return 0;                              /* nothing evictable (stc) */
    *(uint32_t *)(uintptr_t)(0x8c734u + OBJ_DELTA) = (uint32_t)(uintptr_t)best;
    *(uint16_t *)(uintptr_t)(0x8c940u + OBJ_DELTA) = best_idx;
    release_das_cache_slot_resources(best_idx, (uint32_t)(uintptr_t)best);   /* 0x413e1 */
    return 1;                                  /* slot selected (clc) */
}

/* evict_one_das_cache_slot (0x413ea): evict one slot; map CF -> EAX. */
int32_t evict_one_das_cache_slot(void)
{
    return evict_das_cache_slot(0) ? -1 : 0;   /* success (CF clear) -> -1 */
}

/* reserve_das_cache_slot (0x4134a): take the first free slot (ptr==0); if none,
 * evict one and retry. Returns 1 (CF clear = reserved) / 0 (CF set = gave up). */
int reserve_das_cache_slot(void)
{
    for (;;) {
        uint8_t *slot = (uint8_t *)(uintptr_t)(0x89930u + OBJ_DELTA);
        uint16_t idx = 0;
        for (int n = 0xf0; n > 0; n--, slot += 6, idx++) {
            if (*(uint32_t *)slot == 0) {
                *(uint32_t *)(uintptr_t)(0x8c734u + OBJ_DELTA) = (uint32_t)(uintptr_t)slot;
                *(uint16_t *)(uintptr_t)(0x8c940u + OBJ_DELTA) = idx;
                return 1;
            }
        }
        if (!evict_das_cache_slot(0))
            return 0;                          /* eviction failed -> give up */
        /* else a slot was freed -> rescan */
    }
}

/* find_free_entity_slot (0x42626): first free slot (+0==0) in the 16-entry
 * dynamic-entity table (stride 0x1c). Returns 0..15, or 0 if none free (the
 * faithful ambiguity: 0 = "slot 0 free" OR "none free"). Leaf.
 * A1 NOTE: the original ALSO returns the walked slot POINTER in ESI — which its only
 * caller (spawn_entity_at_position) consumes; the lift keeps the EAX index and the
 * caller reconstructs ESI as GADDR(0x90fe4)+idx*0x1c (see lift_entity_ai.c). */
int32_t find_free_entity_slot(void)
{
    uint8_t *slot = (uint8_t *)(uintptr_t)(0x90fe4u + OBJ_DELTA);   /* g_dynamic_entity_table */
    int32_t i = 0;
    while (*(uint32_t *)slot != 0) {
        slot += 0x1c;
        if (++i >= 0x10)
            return 0;
    }
    return i;
}

/* measure_control_text_width (0x1f91f): sum glyph advances over a control-coded
 * string. 0x01 = +1 arg byte, 0x02 = +2 arg bytes, >=0x20 = printable (add
 * advance), other <0x20 (incl NUL) = no width. Calls the verified font measurer. */
int32_t measure_control_text_width(const char *s)
{
    const uint8_t *p = (const uint8_t *)s;
    int32_t width = 0;
    uint8_t c;
    do {
        c = *p++;
        uint32_t ch = c;
        if (ch == 1)
            p += 1;
        else if (ch == 2)
            p += 2;
        else if (ch >= 0x20)
            width += (int32_t)measure_font_char_advance(ch);
    } while (c != 0);
    return width;
}

/* key_m_toggle_render_mode (0x144da): cycle g_render_mode 0<->1, apply it, then
 * refresh the renderer. The two render-refresh callees (0x3001b/0x26cd4) are
 * OS-bound side effects, not part of this lift (stubbed in the oracle). */
void key_m_toggle_render_mode(void)
{
    G16(VA_g_render_mode) += 1;                  /* g_render_mode++ */
    if (G16(VA_g_render_mode) >= 2)
        G16(VA_g_render_mode) = 0;               /* wrap: 0<->1 only */
    apply_render_mode();         /* 0x14506 (verified) -> g_flat_shading_flag */
}

#ifndef ROTH_STANDALONE   /* both callsites route to direct lifted bodies image-free (das cache fetch) */
/* Bridge variant: invoke an original with ESI set (non-Watcom arg convention),
 * return EAX. (Used for callees that take their pointer arg in ESI.) */
static uint32_t call_asm_esi(uint32_t canon_va, uint32_t esi_val)
{
    regs_t io;
    memset(&io, 0, sizeof io);
    io.va = canon_va + OBJ_DELTA;
    io.esi = esi_val;
    call_orig(&io);
    return io.eax;
}

/* Bridge variant: invoke an original with EAX set, return the captured carry flag
 * (CF = eflags bit 0) — for callees that report status in CF. */
static uint32_t call_asm_cf(uint32_t canon_va, uint32_t eax_val)
{
    regs_t io;
    memset(&io, 0, sizeof io);
    io.va = canon_va + OBJ_DELTA;
    io.eax = eax_val;
    call_orig(&io);
    return io.eflags & 1u;
}
#endif

/* queue_timed_message_color (0x1f859): set the message color, word-wrap the text
 * via layout_timed_message_text (0x1f3d3, lifted in lift_dialogue_ui.c — re-pointed:
 * was call_asm VA 0x1f3d3), then store the display timer + resulting line count. */
void queue_timed_message_color(const char *msg, uint8_t color)
{
    G8(VA_g_timed_message_color) = color;                                  /* g_timed_message_color */
    uint32_t lines = (uint32_t)layout_timed_message_text(
        (int32_t *)(uintptr_t)(0x824c9u + OBJ_DELTA),     /* EAX = g_timed_message_lines */
        (uint8_t *)(uintptr_t)(0x825e1u + OBJ_DELTA),     /* EDX = g_timed_message_text_buffer */
        (const uint8_t *)msg,                             /* EBX = msg */
        *(int32_t *)(uintptr_t)(0x85498u + OBJ_DELTA),    /* ECX = g_screen_pitch */
        0xa);                                             /* stack arg5 = wrap count 10 */
    G32(VA_g_timed_message_timer) = 0x8c;                                  /* g_timed_message_timer = 140 */
    G32(VA_g_timed_message_line_count) = (int32_t)lines;                        /* g_timed_message_line_count */
}

/* show_timed_message (0x1f88c): queue a message with the default color. */
void show_timed_message(const char *msg)
{
    queue_timed_message_color(msg, G8(VA_g_default_message_color));   /* g_default_message_color */
}

/* look_pitch_recenter_up (0x1296f): mirror of recenter_down — nudge the applied
 * pitch UP by 8 (dump+clear a nonzero accumulator first). */
void look_pitch_recenter_up(void)
{
    int32_t pitch = G32(VA_g_view_pitch);
    if (pitch != 0) {
        G32(VA_g_view_pitch) = 0;
        G32(VA_g_view_pitch_applied) = pitch;
    }
    G32(VA_g_view_pitch_applied) += 8;
}

/* encode_literal_skip_delta_stream (0x4ee1f): inverse of apply_literal_skip —
 * emit a literal/skip delta from (ref, newb, len). Self-contained; the two emit
 * helpers are inlined (identical output bytes). Returns bytes written incl. the
 * terminating 0. The `esi` cursor is kept one byte AHEAD; the literal flush
 * rewinds by (lit+1) and restores it. */
uint32_t encode_literal_skip_delta_stream(uint8_t *out, const uint8_t *ref,
                                                 const uint8_t *newb, int len)
{
    uint8_t *o = out;
    const uint8_t *esi = newb;    /* literal source, kept 1 ahead */
    const uint8_t *rbx = ref;
    int skip = 0, lit = 0, n = len;
step:
    esi++;
    if (*rbx++ != esi[-1])
        goto literal;
    if (skip != 0)
        goto add_skip;
    if (*rbx != *esi)             /* 1-byte lookahead: next differs -> literalize this */
        goto literal;
    if (lit != 0) {               /* flush pending literal run before starting a skip */
        esi -= (lit + 1); *o++ = (uint8_t)lit;
        memcpy(o, esi, (size_t)lit); o += lit; esi += lit; lit = 0; esi += 1;
    }
add_skip:
    skip++;
    if (--n <= 0)
        goto flush;
    if (skip >= 0x7f) { *o++ = (uint8_t)(skip + 0x80); skip = 0; }
    goto step;
literal:
    if (skip != 0) { *o++ = (uint8_t)(skip + 0x80); skip = 0; }
    lit++;
    if (--n <= 0)
        goto flush;
    if (lit >= 0x7f) {
        esi -= (lit + 1); *o++ = (uint8_t)lit;
        memcpy(o, esi, (size_t)lit); o += lit; esi += lit; lit = 0; esi += 1;
    }
    goto step;
flush:
    if (skip != 0) { *o++ = (uint8_t)(skip + 0x80); skip = 0; }
    if (lit != 0) {
        esi -= (lit + 1); *o++ = (uint8_t)lit;
        memcpy(o, esi, (size_t)lit); o += lit; esi += lit; lit = 0; esi += 1;
    }
    *o = 0;
    return (uint32_t)((o + 1) - out);
}

/* screen_xy_to_framebuffer_ptr (0x18040): framebuffer byte pointer for pixel (x,y).
 *   push ebx; ebx=x; eax=[g_framebuffer_ptr]; if [g_hires_doubling]!=0 y+=y;
 *   imul edx,[g_screen_pitch]; ebx+=edx; eax+=ebx.  Leaf, EBX preserved.
 * Result = fb_base + x + (hires?2y:y)*pitch, all 32-bit. */
uint8_t *screen_xy_to_framebuffer_ptr(int32_t x, int32_t y)
{
    int32_t yy = (G8(VA_g_hires_line_doubling_flag) != 0) ? (y + y) : y;          /* hires line doubling */
    uint32_t base  = (uint32_t)G32(VA_g_framebuffer_ptr);                /* g_framebuffer_ptr */
    int32_t  pitch = G32(VA_g_screen_pitch);                          /* g_screen_pitch (signed imul) */
    uint32_t r = base + (uint32_t)x + (uint32_t)(yy * pitch);
    return (uint8_t *)(uintptr_t)r;
}

/* key_a_jump (0x1c5f9): held 'A' jump handler. Always sets bit0 (pressed); if
 * grounded (g_player_airborne==0) and the jump sub-state permits, also sets
 * bit0x10 (initiate) via a second OR (net old|0x11 on accept). Leaf.
 *   sub-state s: 0 -> always; 1 -> only if stance counter < 5; 2 -> ok; >2 -> no. */
void key_a_jump(void)
{
    G8(VA_g_player_locomotion_flags) |= 0x01;                          /* jump pressed (or [flags],1) */
    if (G32(VA_g_player_airborne) != 0) return;                /* airborne -> no initiate */
    uint32_t s = (uint32_t)G32(VA_g_player_airborne + 0x4);
    int accept;
    if (s < 1)        accept = 1;                 /* jb -> test eax,eax;je (eax=0) */
    else if (s <= 1) {                            /* jbe -> stance-counter window */
        if (G32(VA_g_player_airborne + 0x10) >= 5) return;            /* cmp [counter],5; jge -> return */
        accept = 1;
    } else            accept = (s == 2);          /* cmp eax,2; je -> only s==2 */
    if (accept) G8(VA_g_player_locomotion_flags) |= 0x10;              /* jump initiate (or [flags],0x10) */
}

/* atan2_bearing (0x3c201): bearing of the vector (x1,y1)->(x2,y2) in the engine's
 * 0x200-per-turn units, masked to 9 bits. Pure leaf trig (companion to sincos_lookup).
 *   dx=x2-x1, dy=y2-y1; octant flags in `oct` (bit0=dx<0, bit1=dy<0, bit2=|dx|<=|dy|).
 *   first-octant ratio = (lo*0x4000)/hi matched against g_atan_table[0..63] (closest;
 *   ties favour the higher index via cl counter); folded back to full circle.
 * 16-/8-bit widths reproduced where they reach the masked low bits. NOTE: an axis
 * diff of exactly -0x8000 would #DE in the original `div`; such vectors are outside
 * the verification domain (the original faults there too). */
uint32_t atan2_bearing(int16_t x1, int16_t y2, int16_t y1, int16_t x2)
{
    uint8_t oct = 0;
    uint16_t cx = (uint16_t)((uint16_t)x2 - (uint16_t)x1);   /* sub cx,ax */
    if ((int16_t)cx < 0) { cx = (uint16_t)(-(int32_t)cx); oct |= 1; }
    uint16_t dx = (uint16_t)((uint16_t)y2 - (uint16_t)y1);   /* sub dx,bx */
    if ((int16_t)dx < 0) { dx = (uint16_t)(-(int32_t)dx); oct |= 2; }
    if (!((int16_t)cx > (int16_t)dx)) {                       /* cmp cx,dx; jg skips */
        uint16_t t = cx; cx = dx; dx = t; oct |= 4;          /* xchg ecx,edx; or ebp,4 */
    }
    uint16_t ratio = 0x4000;                                 /* mov ax,0x4000 */
    if (cx != 0)                                             /* or cx,cx; je skips div */
        ratio = (uint16_t)(((uint32_t)0x4000u * (uint32_t)dx) / (uint32_t)cx); /* mul dx;div cx */

    const int16_t *tbl = (const int16_t *)(uintptr_t)(0x8aa46u + OBJ_DELTA);   /* g_atan_table */
    uint16_t bestd = 0x4000;                                 /* mov dx,0x4000 */
    uint8_t  ch = 0;                                         /* ecx hi byte init (0x40 -> ch=0) */
    uint8_t  cl = 0x40;
    int idx = 0;
    do {
        uint16_t d = (uint16_t)((uint16_t)tbl[idx] - ratio); /* mov ax,[esi]; sub ax,bx */
        if ((int16_t)d < 0) d = (uint16_t)(-(int32_t)d);     /* jns; neg eax (abs, low16) */
        if (!(d > bestd)) { ch = cl; bestd = d; }            /* cmp ax,dx; ja skips update */
        idx++;
        cl--;                                                /* dec cl */
    } while ((int8_t)cl > 0);                                /* jg */

    uint16_t a = (uint8_t)(0x40 - ch);                       /* mov eax,0x40; sub al,ch */
    if (!(oct & 4)) a = (uint16_t)((uint16_t)(-(int32_t)a) + 0x80); /* neg; add ax,0x80 */
    if (oct & 1)    a = (uint16_t)(-(int32_t)a);             /* neg */
    if (oct & 2) { a = (uint16_t)(-(int32_t)a); a = (uint16_t)(a + 0x100); } /* neg; inc ah */
    return (uint32_t)a & 0x1ffu;                             /* and eax,0x1ff */
}

/* build_map_selector_menu (0x17453): build the in-game map-picker text into `out`
 * (EAX). Reads g_map_list_ptr (a heap copy of a space-separated map-name string),
 * copies a header, then emits per-item style markers, wrapping every 6 items and
 * truncating a "DIR\NAME" path prefix on '\'. This is a near-literal transcription
 * of 0x17453..0x17547 (byte-exact is the bar; the inner word-skip and marker
 * bookkeeping are subtle, so the asm — not intuition — is followed). Globals:
 *   g_map_menu_count 0x7fe2c, selected_index 0x7fe30, selected_entry 0x7fe34,
 *   marker_selected 0x7675d, marker_normal 0x76765, header 0x711cc. Leaf. */
void build_map_selector_menu(char *out_arg)
{
    uint8_t *out = (uint8_t *)out_arg;                  /* eax */
    uint8_t *pe;                                        /* edx (header, then src) */
    uint8_t *ebx_p, *esi_p;                             /* ebx/esi as pointers */
    uint32_t col;                                       /* ecx */
    uint8_t  bl;

    pe = (uint8_t *)(uintptr_t)(0x711ccu + OBJ_DELTA);  /* mov edx,header */
    G8(VA_g_font_descriptor + 0x2ba) = 0x01;                                 /* hdr[0] = style-control prefix */
    col = 0;                                            /* xor ecx,ecx */
    G8(VA_g_font_descriptor + 0x2bb) = 0x20;                                 /* hdr[1] = ' ' */
    G32(VA_g_map_menu_count) = 0;                                   /* g_map_menu_count = 0 */
    if (G32(VA_g_map_list_ptr) == 0) { *out = 0; return; }        /* NULL list -> single NUL */

    do {                                                /* header copy (incl. its NUL) */
        bl = *pe; pe++;
        *out = bl; out++;
    } while (bl != 0);

    pe = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_list_ptr);  /* edx = src = g_map_list_ptr */
    out--;                                              /* dec eax (back onto the NUL) */
    ebx_p = out + 1;                                    /* lea ebx,[eax+1] */
    esi_p = out + 2;                                    /* lea esi,[eax+2] */
    if ((uint32_t)G32(VA_g_map_menu_count) == (uint32_t)G32(VA_g_map_menu_selected_index)) {   /* first item selected? */
        *out = 0x01; *ebx_p = G8(VA_g_map_menu_marker_selected);              /* marker_selected */
        G32(VA_g_map_menu_selected_entry) = (int32_t)(uintptr_t)pe;          /* selected_entry = src */
    } else {
        *out = 0x01; *ebx_p = G8(VA_g_map_menu_marker_normal);              /* marker_normal */
    }
    out = esi_p;                                        /* mov eax,esi */
    esi_p = out;                                        /* mov esi,eax (line_start) */

    do {                                                /* main copy loop @0x174c5 */
        bl = *pe; pe++;
        *out = bl; out++;
        if (bl == 0x5c) out = esi_p;                    /* '\' -> restart token (path strip) */
        if (bl == 0x20) {                               /* item separator */
            G32(VA_g_map_menu_count) += 1;                          /* count++ */
            col += 1;
            *out = 0x20; out++;
            if ((int32_t)col > 5) { out[-1] = 0x0d; col = 0; }  /* wrap row after 6 */
            for (;;) {                                  /* skip to next space in src */
                bl = *pe; pe++;
                if (bl == 0) { bl = 0x20; pe--; }       /* NUL -> treat as space, unconsumed */
                if (bl == 0x20) break;
            }
            if ((uint32_t)(G32(VA_g_map_menu_selected_index) + 1) == (uint32_t)G32(VA_g_map_menu_count)) {  /* next is selected */
                *out = 0x01; out++;
                *out = G8(VA_g_map_menu_marker_normal); out++;              /* (emits marker_normal here) */
            }
            if ((uint32_t)G32(VA_g_map_menu_count) == (uint32_t)G32(VA_g_map_menu_selected_index)) {        /* this is selected */
                *out = 0x01; out++;
                *out = G8(VA_g_map_menu_marker_selected); out++;
                G32(VA_g_map_menu_selected_entry) = (int32_t)(uintptr_t)pe;  /* selected_entry = src */
            }
            esi_p = out;                                /* mov esi,eax */
        }
    } while (bl != 0);
    *out = 0;                                           /* terminate */
}

/* select_das_fat_entry (0x411e0): pick a DAS FAT entry by g_current_das_fat_index_x2,
 * from the map FAT buffer or (index>=0x2400) the ADEMO FAT buffer; copy its handle +
 * size into the g_current_das_fat_entry globals, or reject (zero them) unless flag_1
 * is ' '/'$' (special-accept with sentinel handle 0xa). Leaf; CF set but callers test
 * g_current_das_block_size_bytes==0 instead, so this verifies on the memory write-set.
 * Register-level transcription: the two `je 0x4122a` reach the reject check with edx in
 * different states (raw offset on the latent-ademo-null path vs entry ptr on handle==0);
 * the FAT buffers are real pointer values, so entries are dereferenced raw (not G32). */
void select_das_fat_entry(void)
{
    uint32_t eax = (uint32_t)G32(VA_g_map_das_fat_buffer);             /* g_map_das_fat_buffer (ptr) */
    uint32_t edx = (uint32_t)G32(VA_g_current_das_fat_index_x2);             /* g_current_das_fat_index_x2 */
    if (edx >= 0x2400) {
        edx -= 0x2400;
        eax = (uint32_t)G32(VA_g_ademo_das_fat_buffer);                  /* g_ademo_das_fat_buffer */
        if (eax == 0) goto reject_check;               /* je 0x4122a (latent: edx is raw) */
    }
    edx = (edx << 2) + eax;                            /* entry = base + idx*4 (8B stride) */
    eax = *(uint32_t *)(uintptr_t)edx;                 /* handle = entry[0] */
    if (eax == 0) goto reject_check;                   /* je 0x4122a */
store:
    G32(VA_g_current_das_fat_entry) = (int32_t)eax;                       /* g_current_das_fat_entry+0 = handle */
    eax = *(uint32_t *)(uintptr_t)(edx + 4);           /* second = entry[4] */
    G32(VA_g_current_das_fat_entry + 0x4) = (int32_t)eax;                       /* +4 */
    eax = (uint16_t)G32(VA_g_current_das_fat_entry + 0x4);                      /* movzx word: data_size_base */
    eax += eax;                                        /* *2 */
    G32(VA_g_current_das_block_size_bytes) = (int32_t)eax;                       /* g_current_das_block_size_bytes */
    return;                                            /* clc; ret */
reject_check:
    eax = 0xa;                                         /* sentinel handle */
    if (*(uint8_t *)(uintptr_t)(edx + 6) == 0x20) goto store;   /* flag_1 == ' ' */
    if (*(uint8_t *)(uintptr_t)(edx + 6) == 0x24) goto store;   /* flag_1 == '$' */
    G32(VA_g_current_das_block_size_bytes) = 0;                                  /* reject */
    G32(VA_g_current_das_fat_entry) = 0;
}

/* write_state_dynamic_entities (0x4eee0): serialize two source tables into `out` (EAX),
 * relocating pointer fields to offsets. Table A = 16×34B (rec[+4]-=secondary_buf,
 * rec[+0]-=(count-1)); table B = 16×28B (rec[+0]-=secondary_buf); zero fields left as 0.
 * Physical extent 1004B; returns the hardcoded logical size 0x3e4 (written to out[0] last).
 * Leaf; sources are read-only (copied then patched in the output). */
uint32_t write_state_dynamic_entities(void *out)
{
    uint8_t *p = (uint8_t *)out;
    int32_t reloc_a0 = G32(VA_g_ademo_das_fat_buffer + 0x4) - 1;               /* ebp = [0x85cf4] - 1 */
    *(uint32_t *)(p + 4) = 0x10;                        /* tag at out+4 */
    p += 8;
    const uint8_t *srcA = (const uint8_t *)(uintptr_t)(0x91e04u + OBJ_DELTA);
    for (int i = 0; i < 16; i++) {                      /* dec edx; jg with edx=0x10 */
        memcpy(p, srcA, 34); srcA += 34;               /* rep movsd ×8 + movsw */
        if (*(uint32_t *)(p + 4) != 0) *(uint32_t *)(p + 4) -= (uint32_t)G32(VA_g_map_objects_buffer);
        if (*(uint32_t *)(p + 0) != 0) *(uint32_t *)(p + 0) -= (uint32_t)reloc_a0;
        p += 34;
    }
    *(uint32_t *)p = 0x10; p += 4;                      /* tag before table B */
    const uint8_t *srcB = (const uint8_t *)(uintptr_t)(0x90fe4u + OBJ_DELTA);
    for (int i = 0; i < 16; i++) {
        memcpy(p, srcB, 28); srcB += 28;               /* rep movsd ×7 */
        if (*(uint32_t *)(p + 0) != 0) *(uint32_t *)(p + 0) -= (uint32_t)G32(VA_g_map_objects_buffer);
        p += 28;
    }
    *(uint32_t *)out = 0x3e4;                           /* size (written last) */
    return 0x3e4;
}

/* initialize_das_block_internal_pointers (0x41554): one-time fixup of a loaded DAS block
 * (ARG IN ESI). Converts two paragraph:offset pairs at block+0x10/+0x14 to flat pointers
 * in place, then walks the node chain from ptr10 building forward +8 links, writing
 * ptr14+2 to each node+0x30, and normalizing node+0xc (EXPL: zero-extend low byte and
 * copy the unaligned +0xd word to +0xe; non-EXPL: byteswap). Guard bit at block+0x1a.
 * Leaf. The unified loop below is proven byte-equivalent to the asm's two separate paths
 * (the +8 link write timing differs but the final memory is identical). */
void initialize_das_block_internal_pointers(void *blockv)
{
    uint8_t *block = (uint8_t *)blockv;
    if (*(uint16_t *)(block + 0x1a) & 1) return;       /* already initialized */
    *(uint16_t *)(block + 0x1a) |= 1;

    uint32_t ptr14 = (uint32_t)(uintptr_t)block
                   + ((uint32_t)*(uint16_t *)(block + 0x16) << 4)
                   + *(uint16_t *)(block + 0x14);
    *(uint32_t *)(block + 0x14) = ptr14;
    uint32_t ptr10 = (uint32_t)(uintptr_t)block
                   + ((uint32_t)*(uint16_t *)(block + 0x12) << 4)
                   + *(uint16_t *)(block + 0x10);
    *(uint32_t *)(block + 0x10) = ptr10;

    int is_expl = (*(uint32_t *)(uintptr_t)(ptr10 - 8) == 0x4c505845u);  /* "EXPL" */
    uint32_t ebx = ptr14 + 2;
    uint8_t *cur  = (uint8_t *)(uintptr_t)ptr10;
    uint8_t *node = (uint8_t *)(uintptr_t)ptr10;
    for (;;) {
        if (*(uint32_t *)node == 0) break;             /* [eax]==0 at loop top */
        *(uint32_t *)(cur + 8) = (uint32_t)(uintptr_t)node;   /* link prev -> this */
        cur = node;
        uint32_t next = (uint32_t)(uintptr_t)cur + *(uint16_t *)(cur + 0)
                      + ((uint32_t)*(uint16_t *)(cur + 2) << 4);
        *(uint32_t *)(cur + 0x30) = ebx;
        if (is_expl) {
            *(uint16_t *)(cur + 0x0e) = *(uint16_t *)(cur + 0x0d);   /* unaligned copy */
            *(uint16_t *)(cur + 0x0c) = (uint8_t)*(cur + 0x0c);      /* zero-extend low byte */
        } else {
            uint16_t w = *(uint16_t *)(cur + 0x0c);
            *(uint16_t *)(cur + 0x0c) = (uint16_t)((w >> 8) | (w << 8));  /* byteswap */
        }
        node = (uint8_t *)(uintptr_t)next;
        if (next == (uint32_t)(uintptr_t)cur) break;   /* defensive (unreachable post-guard) */
    }
    *(uint32_t *)(cur + 8) = 0;                          /* null-terminate last link */
}

/* apply_das_sprite_frame_delta_stream (0x4eda1): DAS sprite-frame RLE/delta decoder.
 * ARGS IN ESI=src, EDI=dst. Reads control bytes and emits literals / RLE fills / skips
 * into dst; the asm's dword/word-optimized copies/fills produce the same bytes as plain
 * memcpy/memset. Leaf (ES is only flat save/restore). EAX hi-16 is invariantly 0, so the
 * extended word is a clean 16-bit value. */
void apply_das_sprite_frame_delta_stream(void *dstv, const void *srcv)
{
    uint8_t *dst = (uint8_t *)dstv;
    const uint8_t *src = (const uint8_t *)srcv;
    for (;;) {
        uint8_t b = *src++;
        if (b == 0x00) {                               /* SHORT RLE: [count, fill] */
            uint8_t cnt = *src++, fill = *src++;
            memset(dst, fill, cnt); dst += cnt;
        } else if (b < 0x80) {                         /* SHORT LITERAL 0x01..0x7f */
            memcpy(dst, src, b); dst += b; src += b;
        } else if (b != 0x80) {                        /* SHORT SKIP 0x81..0xff */
            dst += (uint8_t)(b - 0x80);
        } else {                                       /* EXTENDED (b==0x80): 16-bit word */
            uint8_t wl = *src++, wh = *src++;
            if (wh == 0x00) return;                    /* TERMINATOR */
            else if (wh < 0x80)                        /* BIG SKIP */
                dst += (uint16_t)(wl | (wh << 8));
            else if (wh < 0xc0) {                      /* BIG LITERAL */
                uint16_t cnt = (uint16_t)(((wh - 0x80) << 8) | wl);
                memcpy(dst, src, cnt); dst += cnt; src += cnt;
            } else {                                   /* BIG RLE fill */
                uint16_t cnt = (uint16_t)(((wh & 0x3f) << 8) | wl);
                uint8_t fill = *src++;
                memset(dst, fill, cnt); dst += cnt;
            }
        }
    }
}

/* num_to_decimal_digits (0x1155f): write `num` as EXACTLY 4 zero-padded decimal digits
 * to [EDI], returning the advanced cursor. ABI: AX = num, EDI = dest (non-Watcom). Three
 * 16-bit unsigned divides (÷1000, ÷100, ÷10) peel the thousands/hundreds/tens; the final
 * remainder is the ones. Each digit is the low byte of the quotient + '0'. Leaf. */
uint8_t *num_to_decimal_digits(uint16_t num, uint8_t *edi)
{
    uint16_t ax = num, q, r;
    q = (uint16_t)(ax / 1000); r = (uint16_t)(ax % 1000);   /* div bx=0x3e8 */
    *edi++ = (uint8_t)((uint8_t)q + 0x30);
    ax = r;
    q = (uint16_t)(ax / 100);  r = (uint16_t)(ax % 100);    /* div bx=0x64 */
    *edi++ = (uint8_t)((uint8_t)q + 0x30);
    ax = r;
    q = (uint16_t)(ax / 10);   r = (uint16_t)(ax % 10);     /* div bx=0xa */
    *edi++ = (uint8_t)((uint8_t)q + 0x30);
    *edi++ = (uint8_t)((uint8_t)r + 0x30);                  /* dl + '0' (ones) */
    return edi;
}

/* build_snapshot_anim_filename (0x11500): assemble the screenshot path into
 * g_snapshot_filename_buf (0x8b370): "C:\" prefix + '\' (if missing) + "SNAP" base +
 * the 4-digit frame number + ".lbm" tail (from a template, skipped to its '.').
 * Composes with the lifted num_to_decimal_digits (no bridge needed). Leaf-ish. */
void build_snapshot_anim_filename(void)
{
    uint16_t frame = (uint16_t)G16(VA_g_snapshot_anim_frame);                /* g_snapshot_anim_frame */
    uint8_t *d = (uint8_t *)(uintptr_t)(0x8b370u + OBJ_DELTA);
    const uint8_t *p;
    for (p = (const uint8_t *)(uintptr_t)(0x706e3u + OBJ_DELTA); *p; ) *d++ = *p++;  /* "C:\" */
    if (d[-1] != 0x5c) *d++ = 0x5c;                         /* ensure trailing '\' */
    for (p = (const uint8_t *)(uintptr_t)(0x70733u + OBJ_DELTA); *p; ) *d++ = *p++;  /* "SNAP" */
    d = num_to_decimal_digits(frame, d);             /* append 4 decimal digits */
    p = (const uint8_t *)(uintptr_t)(0x7248bu + OBJ_DELTA); /* extension template */
    while (*p != 0x2e) p++;                                 /* skip to '.' */
    do { *d++ = *p; } while (*p++ != 0);                    /* copy ".lbm" incl. NUL */
}

/* get_loaded_das_block_for_index (0x414f4): resolve a FAT index to its loaded DAS block
 * pointer via g_das_entry_status_table. ARG: AX = index. Fast path (status low byte <
 * 0xfd = cache-slot index): touch the slot's LRU age = g_das_cache_tick, deref the slot's
 * pointer-to-pointer to the block, and (if the heap-meta "moved" bit at block-8 is set)
 * refresh it. 0xfd/0xfe = placeholder (->0); 0xff = not loaded (load, then retry).
 * The moved-refresh (0x41250, ESI arg) and OS-bound loader (0x40d7c, CF status) callees
 * are invoked through the bridge — those paths are NOT oracle-verified (file I/O); the
 * fast/placeholder paths are. */
void *get_loaded_das_block_for_index(uint16_t index)
{
    for (;;) {
        uint16_t status = (uint16_t)G16(VA_g_das_entry_status_table + (uint32_t)index * 2);
        uint8_t lo = (uint8_t)status;
        if (lo < 0xfd) {                                   /* cache-slot index */
            uint32_t off = (uint32_t)lo * 6;               /* 6-byte slot stride */
            G16((VA_g_das_cache_slots + 0x4) + off) = (uint16_t)G16(VA_g_das_cache_tick);   /* slot.age = g_das_cache_tick */
            uint32_t alloc_ptr = (uint32_t)G32(VA_g_das_cache_slots + off);
            uint8_t *block = *(uint8_t **)(uintptr_t)alloc_ptr;   /* *(slot.alloc_ptr) */
            if (*(block - 8) & 4) {                        /* heap-meta "moved" bit */
#ifndef ROTH_STANDALONE
                call_asm_esi(0x41250, (uint32_t)(uintptr_t)block);   /* refresh (ESI=block) */
#else
                refresh_moved_das_cache_block((uint32_t)(uintptr_t)block);   /* lifted body */
#endif
            }
            return block;
        }
        if (lo == 0xff) {                                  /* not loaded -> load */
#ifndef ROTH_STANDALONE
            if (call_asm_cf(0x40d7c, index) == 0)          /* CF clear = loaded -> retry */
                continue;
#else
            if (load_das_block_for_fat_index(index) == 0)   /* the loader spine -> CF */
                continue;
#endif
            return NULL;                                   /* load failed */
        }
        return NULL;                                       /* 0xfd / 0xfe placeholder */
    }
}

/* carry_objects_by_player_delta (0x34bd7): the object half of the moving-platform carry.
 * ARGS: EAX=sector height, ESI=object array, CL=count. For each object whose Z (obj+0xa)
 * lies within the sector's vertical span [min,max] of {height, height-delta_z}, add the
 * per-frame player move delta (delta_x/y/z, 16-bit) to its X/Y/Z — same add as
 * commit_player_position_delta. Leaf; span compare is signed 16-bit, do-while on CL. */
void carry_objects_by_player_delta(uint32_t height, void *objs, uint8_t count)
{
    uint32_t edx = height - (uint32_t)G32(VA_g_player_move_delta_z);        /* height - delta_z (32-bit) */
    uint16_t ax = (uint16_t)height;                        /* span endpoints (low 16) */
    uint16_t dx = (uint16_t)edx;
    if (!((int16_t)dx > (int16_t)ax)) {                    /* cmp dx,ax; jg skips xchg */
        uint16_t t = dx; dx = ax; ax = t;                  /* xchg dx,ax -> ax=lo, dx=hi */
    }
    uint8_t *o = (uint8_t *)objs;
    do {
        int16_t z = (int16_t)*(uint16_t *)(o + 0x0a);
        if (!((int16_t)z < (int16_t)ax) && !((int16_t)z > (int16_t)dx)) {  /* ax<=z<=dx */
            *(uint16_t *)(o + 0x0a) += (uint16_t)G32(VA_g_player_move_delta_z);            /* z += delta_z */
            *(uint16_t *)(o + 0x00) += (uint16_t)G32(VA_g_player_move_delta_x);            /* x += delta_x */
            *(uint16_t *)(o + 0x02) += (uint16_t)G32(VA_g_player_move_delta_y);            /* y += delta_y */
        }
        o += 0x10;
    } while (--count != 0);
}

/* helper for apply_moving_sector_carry: if g_player_z lies within the sector's vertical
 * span [min,max] of {height, height-delta_z}, carry the player (pure position add, NO
 * wall test — the platform-carry clip). Mirrors carry_objects' span logic. */
static void carry_player_span(uint16_t height)
{
    uint16_t ax = height;
    uint16_t dx = (uint16_t)((uint32_t)height - (uint32_t)G32(VA_g_player_move_delta_z));   /* height - delta_z */
    if (!((int16_t)dx > (int16_t)ax)) { uint16_t t = dx; dx = ax; ax = t; }
    uint16_t pz = (uint16_t)G16(VA_g_player_z);                                  /* g_player_z */
    if (!((int16_t)pz < (int16_t)ax) && !((int16_t)pz > (int16_t)dx))
        commit_player_position_delta();
}

/* apply_moving_sector_carry (0x34a8e): for each moving sector in `list`, carry the player
 * and/or contained objects by the per-frame move delta. EAX=mask (bit0 floor half, bit1
 * linked half), EDX=list (count-prefixed sector-index array, entries at list+4). Returns
 * mask unchanged (pusha/popa wraps the whole body). Composes with the verified
 * commit_player_position_delta + carry_objects_by_player_delta. */
uint32_t apply_moving_sector_carry(uint32_t mask, void *list)
{
    uint32_t primary = (uint32_t)G32(VA_g_map_geometry_buffer);          /* g_raw_state_primary_buffer */
    uint8_t  bl = (uint8_t)(mask & 3);
    if (bl == 0) return mask;
    uint8_t  *lp = (uint8_t *)list;
    uint32_t ec = *(uint16_t *)lp;                      /* count = list[0] */
    if (ec == 0) return mask;
    uint16_t *p = (uint16_t *)(lp + 4);                 /* entries start at list+4 */
    do {
        uint16_t idx = *p++;
        uint32_t edi = (uint32_t)idx + primary;         /* sec = idx + primary */
        uint8_t *sec = (uint8_t *)(uintptr_t)edi;
        if (idx == (uint16_t)G16(VA_g_player_sector)) {            /* == g_player_sector */
            if (bl & 1)
                carry_player_span(*(uint16_t *)(sec + 2));            /* floor: sec.height */
            if (bl & 2) {
                uint16_t linked = *(uint16_t *)(sec + 0x18);
                if (linked != 0) {
                    uint8_t *lsec = (uint8_t *)(uintptr_t)((uint32_t)linked + primary);
                    carry_player_span(*(uint16_t *)(lsec + 8));       /* linked: lsec+8 */
                }
            }
        }
        if (*(uint8_t *)(sec + 0x16) & 2) {             /* object-carry sector */
            uint32_t e = ((uint32_t)idx) - (uint32_t)G32(VA_g_sector_section_offset);    /* (sec-primary) - dat */
            uint16_t quot = (uint16_t)((uint16_t)e / 13);            /* div si=13 (16-bit) */
            uint32_t eidx = ((e & 0xffff0000u) | quot) + 2;          /* div keeps eax hi16; +2 */
            uint32_t sb = (uint32_t)G32(VA_g_map_objects_buffer);                    /* g_raw_state_secondary_buffer */
            uint32_t off = (e & 0xffff0000u) | *(uint16_t *)(uintptr_t)(sb + eidx);
            if (off != 0) {
                uint8_t *objlist = (uint8_t *)(uintptr_t)(sb + off);
                uint8_t cnt = objlist[0];
                if (cnt != 0) {
                    if (bl & 1)
                        carry_objects_by_player_delta(*(uint16_t *)(sec + 2),
                                                             objlist + 2, cnt);
                    if (bl & 2) {
                        uint16_t linked = *(uint16_t *)(sec + 0x18);
                        if (linked != 0) {
                            uint8_t *lsec = (uint8_t *)(uintptr_t)((uint32_t)linked + primary);
                            carry_objects_by_player_delta(*(uint16_t *)(lsec + 8),
                                                                 objlist + 2, objlist[0]);
                        }
                    }
                }
            }
        }
    } while (--ec != 0);                                /* dec ecx; jg (count is 16-bit, >0) */
    return mask;
}

/* player_movement_tick (0x12520): per-frame player movement driver. Updates run-state +
 * frame counters, gates on g_player_movement_enabled, decays the velocity accumulators on
 * a rate-counter underflow (deadzone <=0x20 -> 0, then halve), rebuilds input, then enqueues
 * the resulting velocity into the active ring buffer — TWICE when running (the sprint speed-
 * doubling). Composes the 3 lifted input-builders (0x11fae/0x121a1/0x128fb — re-pointed:
 * were call_asm bridges) with the lifted apply_player_movement_input. The function's own
 * logic is the run-state / decay / enqueue wrapper. */
void player_movement_tick(void)
{
    poll_mouse_input();                         /* 0x11fae (re-pointed) */
    uint32_t rt = (uint32_t)G32(VA_g_run_toggle);              /* g_run_toggle */
    if ((G8(VA_g_key_modifier_flags) & 0x40) && G32(VA_g_player_airborne + 0xc) != 0)
        rt = ~rt;                                      /* run-invert */
    G32(VA_g_run_active) = (int32_t)(rt & 1);                  /* g_run_active */
    G32(VA_g_console_input_numeric_only + 0x4) += 1;                                 /* DAT_76858++ (dword) */
    G16(VA_g_frame_tick_counter + 0x2) = (uint16_t)(G16(VA_g_frame_tick_counter + 0x2) + 1);       /* DAT_90bce++ (word) */
    if (G8(VA_g_player_movement_enabled) != 1) return;                      /* movement disabled -> ret */

    G16(VA_g_player_move_rate_counter) = (uint16_t)(G16(VA_g_player_move_rate_counter) - 1);       /* rate counter-- */
    if ((int16_t)G16(VA_g_player_move_rate_counter) < 0) {                   /* underflow -> decay */
        G16(VA_g_player_move_rate_counter) = (uint16_t)G16(VA_g_move_speed_immediate);         /* the SMC-patched per-map decay rate (imm16 @0x12570,
                                                        * written by map_load :205; was hardcoded 0x1234 = the
                                                        * FILE default -> velocity never decayed.
                                                        * Trap lane reads the live
                                                        * patched code byte; imgfree reads the arena slot (map_load
                                                        * writes it; default 0x1234 staged via gen_obj1data). */
        int32_t vx = G32(VA_g_player_vel_accum_x), vy = G32(VA_g_player_vel_accum_y);
        int32_t avx = vx < 0 ? -vx : vx, avy = vy < 0 ? -vy : vy;
        if (!(avx > 0x20)) G32(VA_g_player_vel_accum_x) = 0;           /* deadzone */
        if (!(avy > 0x20)) G32(VA_g_player_vel_accum_y) = 0;
        G32(VA_g_player_vel_accum_x) >>= 1;                            /* halve (sar) */
        G32(VA_g_player_vel_accum_y) >>= 1;
    }

    G16(VA_g_move_input_bits) = 0;                                  /* g_move_input_bits = 0 */
    mouse_edge_latch(0);                        /* 0x121a1 (re-pointed; EAX passthrough, site passes 0) */
    dispatch_held_key_actions();                /* 0x128fb (re-pointed) */
    apply_player_movement_input();              /* input -> velocity (lifted) */

    int32_t eax = G32(VA_g_player_vel_accum_x), edx = G32(VA_g_player_vel_accum_y);
    if ((eax | edx) != 0) {                            /* something to enqueue */
        uint8_t *q = (uint8_t *)(uintptr_t)((G16(VA_g_vel_queue_select) ? 0x90b42u : 0x90abeu) + OBJ_DELTA);
        if (!((int16_t)*(uint16_t *)q >= 0x10)) {      /* not full */
            uint32_t sh = (uint32_t)G32(VA_g_player_speed_reduction_shift);      /* g_player_speed_reduction_shift (latched from g_player_speed_reduction_request 0x89f54 by the "Slow Player Speed" cmd 0x41) */
            if (sh != 0) { eax >>= (sh & 0x1f); edx >>= (sh & 0x1f); }
            uint16_t i = *(uint16_t *)q;
            *(uint16_t *)q = (uint16_t)(i + 1);
            uint8_t *e = q + (uint16_t)(i << 3) + 2;   /* entry i: dx@+2, dy@+6 */
            *(int32_t *)e = eax;
            *(int32_t *)(e + 4) = edx;
            if ((G16(VA_g_move_input_bits) & 0x80) || G32(VA_g_run_active) != 0) {   /* RUN -> enqueue again */
                uint8_t *q2 = (uint8_t *)(uintptr_t)((G16(VA_g_vel_queue_select) ? 0x90b42u : 0x90abeu) + OBJ_DELTA);
                if (!((int16_t)*(uint16_t *)q2 >= 0x10)) {
                    uint16_t j = *(uint16_t *)q2;
                    *(uint16_t *)q2 = (uint16_t)(j + 1);
                    uint8_t *e2 = q2 + (uint16_t)(j << 3) + 2;
                    *(int32_t *)e2 = eax;
                    *(int32_t *)(e2 + 4) = edx;
                }
            }
        }
    }
    G16(VA_g_frame_tick_counter + 0x6) = (uint16_t)(G16(VA_g_frame_tick_counter + 0x6) + 1);       /* DAT_90bd2++ */
}

/* input_ring_dequeue (0x1299a, FUN_0001299a — unnamed; first corpus-direct lift): pop one
 * byte from a ring buffer. If head (0x7e91c) != tail (0x7e91e): return buffer[tail]
 * (0x90c1c) and advance tail = (tail+1) & mask (0x7e91a); else return 0. Pure leaf
 * (no calls/int/port). */
uint8_t input_ring_dequeue(void)
{
    uint16_t ax = 0;                                   /* xor ax,ax */
    uint16_t tail = (uint16_t)G16(VA_g_saved_int9_segment + 0x8);
    if ((uint16_t)G16(VA_g_saved_int9_segment + 0x6) != tail) {              /* head != tail */
        ax = G8((VA_g_render_shade_level + 0x2) + tail);                       /* al = buffer[tail] */
        tail = (uint16_t)((tail + 1) & (uint16_t)G16(VA_g_saved_int9_segment + 0x4));   /* inc + ring mask */
        G16(VA_g_saved_int9_segment + 0x8) = tail;
    }
    return (uint8_t)ax;
}

/* set_filename_extension (0x2fbbc, FUN_0002fbbc — unnamed; corpus-direct): set/replace the
 * extension on a filename in place. EAX=string, EDX=ext (4-byte extension, e.g. "lbm\0").
 * Scans for the last '.', resetting on '\' so only the final path component's '.' counts;
 * at '\0' writes '.' then the 4-byte ext (replacing an existing ext, or appending). Pure
 * leaf. */
void set_filename_extension(char *s, uint32_t ext)
{
    char *esi = s;
    char *dot = (char *)0;                     /* ebx: position just after the last '.' */
    for (;;) {
        char c = *esi++;
        if (c == '\\') { dot = (char *)0; continue; }   /* path separator -> reset */
        if (c == '\0') {                                 /* end of string -> finalize */
            if (dot) esi = dot;
            esi[-1] = '.';
            *(uint32_t *)esi = ext;
            return;
        }
        if (c == '.') dot = esi;                         /* remember (position after '.') */
    }
}

/* apply_player_movement_input (0x12750): add the per-tick movement impulse to the player
 * velocity accumulators (g_player_vel_accum_x/y) and apply turning. Two input paths plus a
 * shared turn tail. Composes with the verified sincos_lookup. Mouse path pinned from disasm
 * (its sincos calls are register-arg-mangled):
 *   MOUSE (gate [0x7e932]==1 && [0x7e8d0]!=0): clamp g_mouse_dx to +/-0x10, feed a smoothing
 *     accumulator (0x7e8f8/0x7e8fc), turn g_player_angle by accumulator>>1; clamp g_mouse_dy
 *     to +/-0x18, speed=|dy|>>1, impulse = (sincos(-angle*2) * -dy)>>2 into vel x/y.
 *   KEYBOARD (else): accel ramp; dir offset = g_kbd_dir_offset_table[bits&0xf] (-1 = no move);
 *     impulse = sincos(-(angle+off)*2) * 6 into vel x (sin) / y (cos, +0x100 quarter phase).
 *   TURN TAIL: if turning (bit0x10) angle += g_turn_accum>>1 (+ mirror); else g_turn_accum=0. */
void apply_player_movement_input(void)
{
    if (G8(VA_g_interaction_cursor_type) == 1 && G8(VA_g_mouse_relative_mode) != 0) {
        /* ---- MOUSE path ---- */
        int32_t eax = G32(VA_g_mouse_dx);                    /* g_mouse_dx */
        G32(VA_g_mouse_dx) = 0;
        if (!(eax < 0x10))  eax = 0x10;                /* clamp [-0x10, 0x10] */
        if (!(eax > -0x10)) eax = -0x10;
        eax <<= 1;
        eax += G32(VA_g_saved_int9_offset + 0x10);
        G32(VA_g_saved_int9_offset + 0x10) = eax;                            /* smoothing accumulator */
        if (eax == 0) G32(VA_g_saved_int9_offset + 0x14) = 1;
        G32(VA_g_saved_int9_offset + 0x14) += 1;
        { int32_t lim = G32(VA_g_saved_int9_offset + 0x14);
          if (!(eax < lim))  eax = lim;                /* clamp to +/- DAT_7e8fc */
          if (!(eax > -lim)) eax = -lim; }
        G32(VA_g_saved_int9_offset + 0x10) -= eax;
        eax >>= 1;                                     /* sar */
        G16(VA_g_player_angle) = (uint16_t)(G16(VA_g_player_angle) - (uint16_t)eax);   /* angle -= ax */
        if (G32(VA_g_mouse_dy) == 0) goto idle_decay;        /* g_mouse_dy == 0 */
        {
            int32_t dy = G32(VA_g_mouse_dy);
            if (!(dy < 0x18))  dy = 0x18;              /* clamp [-0x18, 0x18] */
            if (!(dy > -0x18)) dy = -0x18;
            G32(VA_g_mouse_dy) = dy;
            int32_t ady = (dy < 0) ? -dy : dy;         /* |dy| */
            ady >>= 1;
            G32(VA_g_move_speed_accum) = ady;                        /* g_move_speed_accum */
            G16(VA_g_player_move_rate_counter) = (uint16_t)(G16(VA_g_player_move_rate_counter) - 1);   /* rate counter-- */
            int32_t ecx = -G32(VA_g_mouse_dy);               /* -clamped dy */
            uint32_t ebx = (uint32_t)(uint16_t)G16(VA_g_player_angle);    /* movzx angle */
            ebx = (uint32_t)(-(int32_t)ebx);           /* neg ebx (32-bit) */
            ebx += ebx;
            int32_t sinv = sincos_lookup(ebx);
            int32_t px = (int32_t)((uint32_t)sinv * (uint32_t)ecx);   /* imul edx,ecx */
            G32(VA_g_player_vel_accum_x) += (px >> 2);                 /* sar 2; vel_x += */
            uint8_t bh = (uint8_t)((ebx >> 8) + 1);    /* inc bh (quarter phase) */
            ebx = (ebx & 0xffff00ffu) | ((uint32_t)bh << 8);
            int32_t cosv = sincos_lookup(ebx);
            int32_t py = (int32_t)((uint32_t)cosv * (uint32_t)ecx);
            G32(VA_g_player_vel_accum_y) += (py >> 2);                 /* vel_y += */
        }
        goto turn_tail;
    } else {
        /* ---- KEYBOARD path ---- */
        if (G16(VA_g_move_input_bits) & 0xf) {
            if (!((int32_t)G32(VA_g_move_speed_accum) >= 0x10)) G32(VA_g_move_speed_accum) += 1;   /* accel ramp */
            G16(VA_g_player_move_rate_counter) = (uint16_t)(G16(VA_g_player_move_rate_counter) - 1);              /* rate counter-- */
            uint16_t bx = G16(VA_g_kbd_dir_offset_table + (uint32_t)(G16(VA_g_move_input_bits) & 0xf) * 2);   /* dir offset */
            if (bx != 0xffff) {                        /* -1 = no move for this direction */
                bx = (uint16_t)(bx + G16(VA_g_player_angle));    /* += angle */
                bx = (uint16_t)(-(int32_t)bx);         /* neg (16-bit) */
                bx = (uint16_t)(bx + bx);              /* *2 */
                int32_t sinv = sincos_lookup((uint32_t)bx);
                G32(VA_g_player_vel_accum_x) += sinv * 6;              /* vel_x += sin*6 */
                bx = (uint16_t)(bx + 0x100);           /* quarter phase */
                int32_t cosv = sincos_lookup((uint32_t)bx);
                G32(VA_g_player_vel_accum_y) += cosv * 6;              /* vel_y += cos*6 */
            }
            goto turn_tail;
        }
        goto idle_decay;
    }
idle_decay:
    if ((int32_t)G32(VA_g_move_speed_accum) > 0) G32(VA_g_move_speed_accum) -= 1;  /* decay speed when idle */
turn_tail:
    if (G16(VA_g_move_input_bits) & 0x10) {                         /* turning */
        int16_t t = (int16_t)((int16_t)G16(VA_g_turn_accum) >> 1);   /* g_turn_accum >> 1 */
        G16(VA_g_player_angle) = (uint16_t)(G16(VA_g_player_angle) + (uint16_t)t);   /* angle += */
        G16(VA_g_saved_int9_offset + 0xc) = (uint16_t)(G16(VA_g_saved_int9_offset + 0xc) + (uint16_t)t);   /* mirror += */
    } else {
        G16(VA_g_turn_accum) = 0;                              /* not turning: reset ramp */
    }
}

/* string_copy (0x54ddf, FUN_00054ddf — unnamed; corpus-direct): a strcpy. EAX=dst, EDX=src;
 * copies src to dst incl. the NUL (2 bytes per loop iteration — Watcom unroll), returns dst.
 * Pure leaf. */
char *string_copy(char *dst, const char *src)
{
    char *od = dst;
    for (;;) {
        char c0 = src[0]; dst[0] = c0;
        if (c0 == 0) break;
        char c1 = src[1];
        src += 2;
        dst[1] = c1;
        dst += 2;
        if (c1 == 0) break;
    }
    return od;
}

/* resolve_reloc_ptr (0x226c6, FUN_000226c6 — unnamed; corpus-direct): resolve a base-relative
 * offset to an absolute pointer. base = DAT_7f56c; if base==0 return 0, else read the offset
 * stored at base+`offset` and add base: `*(uint32*)(base+offset) + base`. EAX=offset -> EAX.
 * Pure leaf (EBP frame + all GP regs preserved). */
uint32_t resolve_reloc_ptr(uint32_t offset)
{
    uint32_t base = (uint32_t)G32(VA_g_reloc_base);
    if (base == 0)
        return 0;
    uint32_t entry = *(uint32_t *)(uintptr_t)(base + offset);
    return entry + base;
}

/* clear_framebuffer_rect (0x12cea, FUN_00012cea — unnamed; corpus-direct): zero a width×height
 * rectangle at (x,y) in the framebuffer. ABI: EAX=x, EDX=y, EBX=width, ECX=height. Address is
 * g_framebuffer_ptr + x + y*pitch; in hires (0x90cbd) the y-offset AND the row count double.
 * Each row zeroes `width` bytes (rep stosd+stosb). Pure leaf. */
void clear_framebuffer_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    int32_t pitch = G32(VA_g_screen_pitch);                      /* g_screen_pitch */
    int32_t yoff = (int32_t)y * pitch;
    int32_t rows = (int32_t)height;
    if (G8(VA_g_hires_line_doubling_flag) != 0) { yoff += yoff; rows += rows; }   /* hires doubling */
    uint8_t *p = (uint8_t *)(uintptr_t)((uint32_t)G32(VA_g_framebuffer_ptr) + x + (uint32_t)yoff);
    do {                                               /* dec ecx; jg -> runs >=1 row */
        memset(p, 0, width);                           /* rep stosd + rep stosb */
        p += pitch;
    } while (--rows > 0);
}

/* rotate_point_2d (0x2a898, FUN_0002a898 — unnamed; corpus-direct): transform a 2D point by the
 * current view rotation. ARG: EBP = ptr to {int16 x@+0, int16 y@+2, uint16 flags@+4}. Returns a
 * PAIR (the corpus C captured only EAX): if (flags & 0x1ff) rotate via sin=DAT_85310 / cos=DAT_85312
 *   EAX = (y*sin - x*cos) >> 6,  EDX = (x*sin + y*cos) >> 6;
 * else (no rotation) EAX = y<<8, EDX = x<<8. Pure leaf. */
void rotate_point_2d(const int16_t *pt, int32_t *eax_out, int32_t *edx_out)
{
    int32_t x = pt[0], y = pt[1];                      /* movsx */
    uint16_t flags = (uint16_t)pt[2];
    if (flags & 0x1ff) {
        int32_t s = (int16_t)G16(VA_g_camera_sin);             /* movsx DAT_85310 (sin) */
        int32_t c = (int16_t)G16(VA_g_camera_cos);             /* movsx DAT_85312 (cos) */
        *eax_out = (y * s - x * c) >> 6;
        *edx_out = (x * s + y * c) >> 6;
    } else {
        *eax_out = y << 8;
        *edx_out = x << 8;
    }
}

/* ============================ Batch 10 (corpus-direct) ============================
 * Small pure leaves. Each disasm-verified to end
 * in ret with no call/int. Several are multi-register returns the corpus decompile dropped
 * (Ghidra has no Watcom cspec) — caught only by reading the disassembly. */

/* string_concat (0x54dfe, FUN_00054dfe): strcat. EAX=dst, EDX=src. `repnz scasb` walks dst to
 * its NUL, then copies src there incl. the NUL (2-byte unrolled, like string_copy). Returns dst.
 * Pure leaf; sibling of string_copy (0x54ddf). */
char *string_concat(char *dst, const char *src)
{
    char *p = dst;
    while (*p) p++;                          /* repnz scasb + dec edi: p = dst's NUL */
    for (;;) {
        char c0 = src[0]; p[0] = c0;
        if (c0 == 0) return dst;
        char c1 = src[1];
        src += 2;
        p[1] = c1;
        p += 2;
        if (c1 == 0) return dst;
    }
}

/* string_length (0x55bd0, FUN_00055bd0): strlen. EAX=str -> EAX=length. `repnz scasb` then
 * `not ecx; dec ecx`. Pure leaf. */
uint32_t string_length(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (uint32_t)(p - s);
}

/* block_payload_size (0x35fd9, FUN_00035fd9): EAX=ptr -> EAX. if ptr==0 return 0, else read the
 * dword at ptr+4 and subtract 0x10 (`mov eax,[eax+4]; sub eax,0x10`). Looks like an allocation
 * size/end field minus a 0x10 header. Pure leaf. */
uint32_t block_payload_size(uint32_t ptr)
{
    if (ptr == 0)
        return 0;
    return *(uint32_t *)(uintptr_t)(ptr + 4) - 0x10;
}

/* obj_counter12_inc / _dec (0x361e7 / 0x361ef, FUN_000361e7 / FUN_000361ef): EAX=ptr. Increment
 * (resp. decrement) the BYTE at ptr+0x12 in place and return the new value (`movzx` zero-extends
 * to EAX). Classic acquire/release of a byte counter. Pure leaves; caller-buffer write. */
uint8_t obj_counter12_inc(uint32_t ptr)
{
    uint8_t *f = (uint8_t *)(uintptr_t)(ptr + 0x12);
    *f = (uint8_t)(*f + 1);
    return *f;
}
uint8_t obj_counter12_dec(uint32_t ptr)
{
    uint8_t *f = (uint8_t *)(uintptr_t)(ptr + 0x12);
    *f = (uint8_t)(*f - 1);
    return *f;
}

/* get_errno_ptr (0x560c2, FUN_000560c2): `mov eax,0x497d44; ret` — returns the (rebased)
 * address of the global at canon 0x97d44. A bare address accessor. */
uint32_t get_errno_ptr(void)
{
    return 0x97d44u + OBJ_DELTA;
}

/* get_dbase100_inventory_entry (0x18147, FUN_00018147): AX=signed 16-bit index -> EAX. if (int16)AX<=0
 * return 0. Else entry = table[AX] where table = *(0x81e20) (dword stride 4); if entry==0 return 0,
 * else entry + base where base = *(0x81e1c). A handle/id -> absolute-pointer resolver. Pure leaf.
 * (Indexing uses the full EAX, matching the `[edx+eax*4]`; callers pass a clean 16-bit index.) */
uint32_t get_dbase100_inventory_entry(uint32_t eax)
{
    int32_t idx = (int16_t)(uint16_t)eax;            /* movsx ax */
    if (idx <= 0)
        return 0;
    uint32_t table = (uint32_t)G32(VA_g_dbase100_inventory_table);
    uint32_t entry = *(uint32_t *)(uintptr_t)(table + eax * 4);
    if (entry == 0)
        return 0;
    return entry + (uint32_t)G32(VA_g_dbase100_base);
}

/* sincos_pair (0x3bdd2, FUN_0003bdd2): EBX=angle -> sin in CX, cos in BX, ESI=table base. Reads
 * the 512-entry sine table at canon 0x72080 (same table the verified single-entry sincos_lookup
 * at 0x3c1f3 uses). idx=(angle&0x1ff)*2 byte offset; sin=tab16[idx]; cos=tab16[(idx+0x100)&0x3ff]
 * (a quarter-turn / 90 deg phase via `inc bh; and bh,3`). Both are 16-bit zero-extended (mov cx/bx,
 * NOT sign-extended — distinct from 0x3c1f3's movsx). The corpus C decompiled this to `return;`
 * (all three outputs are non-Watcom registers). Pure leaf. */
void sincos_pair(uint32_t angle, uint16_t *sin_out, uint16_t *cos_out, uint32_t *table_out)
{
    const uint8_t *tab = (const uint8_t *)(uintptr_t)(0x72080u + OBJ_DELTA);
    uint32_t idx  = (angle & 0x1ff) << 1;
    uint32_t idx2 = (idx + 0x100) & 0x3ff;
    *sin_out   = *(const uint16_t *)(tab + idx);
    *cos_out   = *(const uint16_t *)(tab + idx2);
    *table_out = 0x72080u + OBJ_DELTA;
}

/* find_record_by_id (0x3d018, FUN_0003d018): DI=key -> CF (0=found, 1=not found). Scans up to
 * count = BYTE[0x8b3f4] records of stride 0x1f6 starting at 0x8b3f8, comparing each record's
 * first u16 to key; clears CF on the first match (cmp-equal), else `stc`. count==0 -> not found.
 * Returns ONLY the carry flag (the match position in EBX is discarded). Pure leaf. */
int find_record_by_id(uint16_t key)
{
    uint8_t count = (uint8_t)G8(VA_g_door_count);
    if (count == 0)
        return 1;                                    /* stc */
    const uint8_t *rec = (const uint8_t *)(uintptr_t)(0x8b3f8u + OBJ_DELTA);
    do {
        if (*(const uint16_t *)rec == key)
            return 0;                                /* cmp-equal: CF=0 */
        rec += 0x1f6;
    } while (--count != 0);
    return 1;                                        /* exhausted: stc */
}

/* ============================ Batch 11 (corpus-direct) ============================ */

/* blit_descriptor_rows (0x13106, FUN_00013106): EAX = ptr to ptr to a self-contained blit
 * descriptor {+0 dst, +4 width(bytes), +8 rows, +0xc inline src bytes}. Copies `rows` rows of
 * `width & ~3` bytes (rep movsd) from the contiguous inline src to dst at g_screen_pitch
 * (0x85498) stride. Pure leaf. */
void blit_descriptor_rows(uint32_t eax)
{
    const uint8_t *desc = *(const uint8_t *const *)(uintptr_t)eax;     /* eax = [eax] */
    uint8_t *dst = *(uint8_t *const *)(uintptr_t)(desc + 0);
    int32_t width = *(const int32_t *)(desc + 4);
    int32_t rows  = *(const int32_t *)(desc + 8);
    const uint8_t *src = desc + 0xc;
    int32_t pitch = G32(VA_g_screen_pitch);
    int32_t ebp = width - pitch;                       /* dst row-advance adjust */
    int32_t nbytes = (width >> 2) * 4;                 /* rep movsd: width&~3 */
    do {
        for (int32_t i = 0; i < nbytes; i++) dst[i] = src[i];   /* rep movsd */
        src += nbytes;
        dst += nbytes;
        dst -= ebp;
    } while (--rows > 0);
}

/* find_free_slot_83ed4 (0x277b6, FUN_000277b6): scan 16 records of stride 0x9a at canon 0x83ed4
 * for the first whose first dword == 0 (free); return its index 0..15, or -1 if none. Pure leaf. */
uint32_t find_free_slot_83ed4(void)
{
    for (uint32_t i = 0; i < 0x10; i++)
        if (G32(VA_g_active_sound_handles + i * 0x9a) == 0)
            return i;
    return 0xffffffffu;
}

/* emit_vertex_bbox (0x2d29a, FUN_0002d29a): AX=x, DX=y, EBX=index-out, EDI=vertex-record,
 * ESI=bbox. Stores the vertex (x@+0xc, y@+0xe), writes its 16-bit id (EDI-0x484f9e) at [EBX],
 * and folds (x,y) into the min/max bbox at ESI+0x28..0x2e. On exit EDI+=0x10, EBX+=2 (the
 * advanced pointers — verified in the harness). Pure leaf; multi-buffer write. */
void emit_vertex_bbox(int16_t x, int16_t y, uint8_t *ebx, uint8_t *edi, uint8_t *esi)
{
    *(uint16_t *)ebx = (uint16_t)((uint32_t)(uintptr_t)edi - 0x484f9eu);
    *(int16_t *)(edi + 0xc) = x;
    *(int16_t *)(edi + 0xe) = y;
    int16_t *min_x = (int16_t *)(esi + 0x28), *min_y = (int16_t *)(esi + 0x2a);
    int16_t *max_x = (int16_t *)(esi + 0x2c), *max_y = (int16_t *)(esi + 0x2e);
    if (x <= *min_x) *min_x = x;
    if (y <= *min_y) *min_y = y;
    if (*max_x <= x) *max_x = x;
    if (*max_y <= y) *max_y = y;
}

/* identity_passthrough (0x51995, FUN_00051995): EAX in -> EAX out (Ghidra: `return param_1`).
 * Also re-emits DX in EDX's low 16 (high 16 is uninitialized stack — not part of the contract).
 * Pure leaf (a compiler-emitted value repackage). */
uint32_t identity_passthrough(uint32_t eax)
{
    return eax;
}

/* reset_renderer_tables (0x2f42b, FUN_0002f42b): rep-stosw init of three renderer globals —
 * 0x86d30 (0x1600 words = 0x00ff each; the per-poly coverage buffer), 0x89930 (0x2d0 words = 0;
 * writer-id table) and 0x8c740 (0x80 words = 0). No args/return. Pure leaf; global write-set. */
void reset_renderer_tables(void)
{
    volatile uint16_t *p;
    p = (volatile uint16_t *)(uintptr_t)(0x86d30u + OBJ_DELTA);
    for (uint32_t i = 0; i < 0x1600; i++) p[i] = 0x00ff;
    p = (volatile uint16_t *)(uintptr_t)(0x89930u + OBJ_DELTA);
    for (uint32_t i = 0; i < 0x2d0; i++) p[i] = 0;
    p = (volatile uint16_t *)(uintptr_t)(0x8c740u + OBJ_DELTA);
    for (uint32_t i = 0; i < 0x80; i++) p[i] = 0;
}

/* scan_tag4_chunk (0x1dda8, FUN_0001dda8): EAX=obj. If (obj[+4] & 0x40)==0 return 0. Stores
 * movsx obj[+2] to global 0x81efa, then walks a {high-byte tag, low-16 length} chunk list at
 * obj+0x14: on tag==4 store the chunk ptr to 0x81efe and return it; advance by (length & ~3);
 * stop (return 0) at length==0. Pure leaf; return-ptr + 2 global writes. */
uint32_t scan_tag4_chunk(uint32_t obj)
{
    if ((*(const uint8_t *)(uintptr_t)(obj + 4) & 0x40) == 0)
        return 0;
    G32(VA_g_dbase100_choice_record_indices + 0x44) = (uint32_t)(int32_t)*(const int16_t *)(uintptr_t)(obj + 2);   /* movsx obj[+2] */
    uint32_t edx = obj + 0x14;
    for (;;) {
        uint32_t val = *(const uint32_t *)(uintptr_t)edx;
        if (((int32_t)val >> 24) == 4) {               /* sar 0x18 == tag */
            G32(VA_g_dbase100_choice_record_indices + 0x48) = edx;
            return edx;
        }
        uint32_t low = val & 0xffff;
        edx += (low & ~3u);
        if (low == 0)
            return 0;
    }
}

/* ============================ Batch 12 (corpus-direct) ============================ */

/* init_render_struct_89ed0 (0x2f962, FUN_0002f962): populate the struct at canon 0x89ed0 from
 * the current selector/buffer globals + two baked pointer constants; also stamps a sub-field at
 * 0x89eec+0x1a. No args/return. Pure leaf; global write-set. */
void init_render_struct_89ed0(void)
{
    uint8_t *s = (uint8_t *)(uintptr_t)(0x89ed0u + OBJ_DELTA);
    *(uint16_t *)(s + 0x10) = G16(VA_g_geometry_selector);
    *(uint16_t *)(s + 0x12) = G16(VA_g_geometry_selector + 0x2);
    *(uint16_t *)(s + 0x14) = G16(VA_g_geometry_selector + 0x4);
    *(uint32_t *)(s + 0x0c) = (uint32_t)G32(VA_g_map_das_dir_table_buffer);
    *(uint32_t *)(s + 0x00) = (uint32_t)G32(VA_g_image_surface);
    *(uint32_t *)(s + 0x04) = 0x89eecu + OBJ_DELTA;                 /* mov eax,0x489eec */
    *(uint16_t *)(uintptr_t)(0x89f06u + OBJ_DELTA) = G16(VA_g_das_unk_0x22);  /* [0x489eec+0x1a] */
    *(uint32_t *)(s + 0x08) = 0x71ee2u + OBJ_DELTA;                 /* mov eax,0x471ee2 */
}

/* split_path (0x210ec, FUN_000210ec): _splitpath. EAX=src, EDX=dir-out, EBX=name-out, ECX=ext-out.
 * Splits on the last '\' (dir | filename) then the last '.' (name | extension). Pure leaf. */
void split_path(const char *src, char *dir, char *name, char *ext)
{
    dir[0] = 0; name[0] = 0; ext[0] = 0;
    char *d = dir, *dir_end = dir;
    const char *comp = src;                       /* start of the last path component */
    char c;
    do {
        c = *src++;
        if (c == '\\') { dir_end = d; comp = src; }
        *d++ = c;
    } while (c != 0);
    *dir_end = 0;                                  /* terminate dir at the last '\' */

    const char *s2 = comp;
    char *n = name, *ext_at = 0;                   /* ext_at = '.' position in `name` */
    do {
        c = *s2++;
        if (c == '.')       ext_at = n;
        else if (c == '\\') ext_at = 0;
        *n++ = c;
    } while (c != 0);

    if (ext_at) {
        *ext_at++ = 0;                             /* cut name at the '.' */
        char *e = ext;
        do { c = *ext_at++; *e++ = c; } while (c != 0);
    }
}

/* rotate_quad (0x3ded2, FUN_0003ded2): EDI = record; rotates 4 points by the angle byte[edi+3]
 * (via the 0x72080 sine table, fixed-point >>14), translates by (word[edi+0x14], word[edi+0x16]),
 * and writes screen coords to each point's +0x80 (x) / +0x84 (y). Points are at [edi+0x2e]+2,
 * stride 0x10 (x@+0, y@+4). EAX is passed through (return = input EAX). Pure leaf. */
uint32_t rotate_quad(uint32_t eax, uint8_t *edi)
{
    const int16_t *sintab = (const int16_t *)(uintptr_t)(0x72080u + OBJ_DELTA);
    uint32_t b = edi[3];
    uint32_t m = (0u - 2u * b) & 0x1ffu;                  /* (-2*angle) & 0x1ff */
    int32_t sinv = sintab[m];                             /* movsx */
    uint32_t boff2 = (2u * m + 0x100u) & 0x3ffu;          /* inc ah; and ah,3 (quarter phase) */
    int32_t cosv = sintab[boff2 >> 1];
    int32_t tx = *(int16_t *)(edi + 0x14);
    int32_t ty = *(int16_t *)(edi + 0x16);

    uint8_t *p = *(uint8_t *const *)(edi + 0x2e) + 2;     /* esi = [edi+0x2e]+2 */
    for (int i = 0; i < 4; i++, p += 0x10) {
        int32_t px = *(int16_t *)(p + 0);
        int32_t py = *(int16_t *)(p + 4);
        int32_t e1 = (py * sinv - px * cosv) >> 14;       /* sar 0xe */
        int32_t e2 = (px * sinv + py * cosv) >> 14;
        *(int16_t *)(p + 0x80) = (int16_t)(uint16_t)(tx - e1);
        *(int16_t *)(p + 0x84) = (int16_t)(uint16_t)(ty + e2);
    }
    return eax;
}

/* ============================ Batch 13 (corpus-direct) ============================ */

/* abs_i32 (0x560fa, FUN_000560fa): EAX -> |EAX| (test/jge/neg). Pure leaf. */
uint32_t abs_i32(int32_t eax)
{
    return (eax >= 0) ? (uint32_t)eax : (0u - (uint32_t)eax);     /* neg = unsigned (INT_MIN faithful) */
}

/* signext_852f2_to_909a4 (0x2ad14, FUN_0002ad14): EAX = movsx word[0x852f2] (cwde); store to
 * global 0x909a4; return it. Pure leaf; global read+write. */
uint32_t signext_852f2_to_909a4(void)
{
    int32_t v = (int16_t)G16(VA_g_vertex_selector + 0x26);                            /* cwde */
    G32(VA_g_span_src_wrap_reoffset + 0x28) = (uint32_t)v;
    return (uint32_t)v;
}

/* geom_find_matches (0x4f313, FUN_0004f313): AX=key, EBX=out-capacity, EDX=out buffer. Searches
 * the geometry buffer (*0x490aa8) records (stride 0x1a; count at recs-2; recs = geom+word[geom+4])
 * for those whose field +0x14 == key, writing each match's geom-relative offset to out+4,out+6,...
 * up to (cap-2) matches. out+2 = key (always), out+0 = match count (only when cap>=3). Returns the
 * match count (0 if cap<3). Pure leaf; reads the geom global, writes the caller's out buffer. */
uint32_t geom_find_matches(uint16_t key, uint32_t cap, uint8_t *out)
{
    *(uint16_t *)(out + 2) = key;
    if (cap < 3)
        return 0;
    uint32_t budget = cap - 2;
    uint8_t *geom = *(uint8_t *const *)(uintptr_t)(0x90aa8u + OBJ_DELTA);
    uint8_t *recs = geom + (*(uint16_t *)(geom + 4));
    uint16_t nrec = *(uint16_t *)(recs - 2);
    uint8_t *o = out + 4;
    uint32_t remaining = budget;
    uint8_t *r = recs;
    for (uint16_t i = 0; i < nrec; i++, r += 0x1a) {
        if (*(uint16_t *)(r + 0x14) == key) {
            *(uint16_t *)o = (uint16_t)(uint32_t)(r - geom);
            o += 2;
            if (--remaining == 0)
                break;
        }
    }
    uint32_t matches = budget - remaining;
    *(uint16_t *)out = (uint16_t)matches;
    return matches;
}

/* ============================ Batch 14 (corpus-direct; far-data / selector) ============================
 * First lifts that operate on a segment SELECTOR's data. The original loads ES from a global
 * selector (DPMI descriptor) whose base points at a data block; the C lift takes that block as a
 * plain base pointer and does the identical offset math. Verified via the oracle's new LDT-selector
 * support (make_selector). */

/* mark_geom_sentinel_entries (0x2ec1a, FUN_0002ec1a): ES = selector at 0x490be8 (geometry segment).
 * ebx = word[seg+6]; count = word[seg + (uint16)(ebx-2)]; then for `count` records of stride 0xc
 * starting at seg+ebx: if word[+8] == 0xffff, set bit 0x20 in byte[+0xa]. Pure leaf (self-loads +
 * restores ES). `seg` = the selector's base. */
void mark_geom_sentinel_entries(uint8_t *seg)
{
    uint32_t ebx = *(uint16_t *)(seg + 6);
    uint32_t count = *(uint16_t *)(seg + ((ebx - 2) & 0xffff));
    if (count == 0)
        return;
    do {
        if (*(uint16_t *)(seg + ebx + 8) == 0xffff)
            *(uint8_t *)(seg + ebx + 0xa) |= 0x20;
        ebx += 0xc;
    } while (--count != 0);
}

/* ============================ Batch 15 (case-2: preset-segreg far-data) ============================
 * Functions that assume a segment register is PRESET by the caller (they never load it themselves).
 * The trampoline now sets es/fs/gs from regs_t before the call; the C lift takes the segment base as
 * a plain pointer and does the identical offset math. This opens the renderer's gs:[..] inner loops. */

/* clear_es_record_field4 (0x293a3, FUN_000293a3): ES preset by caller (never loaded internally).
 * off = (u16)es:[4] (first record offset); count = (u16)es:[off-2]; for each of `count` records of
 * stride 0x1a from off, clear the u16 at es:[off+4]. The original's `dec ecx; jg` over a zero-extended
 * u16 count is a plain "do count times". `es_base` = the ES selector's base. (Field +4 semantics
 * unknown — the name reflects only what the code clears.) */
void clear_es_record_field4(uint8_t *es_base)
{
    uint32_t off   = *(uint16_t *)(es_base + 4);
    uint32_t count = *(uint16_t *)(es_base + (off - 2));
    while (count-- != 0) {
        *(uint16_t *)(es_base + off + 4) = 0;
        off += 0x1a;
    }
}

/* ROW = the SMC-patched framebuffer scanline stride. The renderer's mode-setup patches every stride immediate
 * in the span mappers (`mov edx,0x140` / `add edi,0x140`) to [0x85498] & 0xffff via the patchers at 0x36464
 * (wall/sprite) and 0x3a848 (floor/ceil): 0x140 for 320-wide, 0x280 for 640-wide (VESA). We read the patched
 * value from one of those immediates (0x38e5c, the resolver's `mov edx,0x140`; all patched sites hold the SAME
 * word). Reading the immediate — NOT [0x85498] — is what makes this match the original in BOTH contexts: the
 * oracle runs the ORIGINAL with UNPATCHED code (immediate still 0x140, and [0x85498] isn't set), while in-game
 * the immediate is patched. Every `di += ROW` / `di[ROW]` / `di -= ROW` / `di += 2*ROW` is one such stride. */
#define ROW ((int32_t)*(volatile uint32_t *)(uintptr_t)(0x38e5cu + OBJ_DELTA))

/* render_world_col_tint_gradient_38631 (0x38631, render_world_col_tint_gradient_38631): translucent vertical-span writer that
 * needs BOTH selector cases — ES is SELF-LOADED from g_transparency_blend_selector (0x90be2; case 1)
 * = the 64K blend LUT, and GS is PRESET by the caller (case 2) = the gradient/light ramp. Per row
 * (cx rows, stride 0x140 down g_render_target_buffer 0x85414): al = fb pixel; ah = gs:[(bh<<8)|bl];
 * al = es:[(ah<<8)|al]; store. Then a fixed-point step — esi += ebp (esi/ebp = the low-16 fracs at
 * 0x8a2dc/0x8a2e0, each <<16) and bh += dl(byte 0x8a2e2) + carry — advances the gradient index sub-
 * pixel. bl is the constant low byte from 0x90a24. Caller inputs: EDI=dest off, ECX=row count (cx),
 * BH=start gradient row. gs_base/es_base = the two selector bases. Faithful: same fb + gradient + LUT
 * -> identical written pixels. (No SMC: clean hand-asm, not a 0x1234567 self-patcher.) */
void render_world_col_tint_gradient_38631(uint32_t edi, uint32_t ecx, uint8_t bh,
                                    const uint8_t *gs_base, const uint8_t *es_base)
{
    uint32_t esi = (uint32_t)G32(VA_g_span_accum_init) << 16;
    uint32_t ebp = (uint32_t)G32(VA_g_span_pixel_step) << 16;
    uint8_t  dl  = G8(VA_g_span_eax_step_lo);
    uint8_t  bl  = G8(VA_g_sprite_fill_index);
    uint8_t *dst = (uint8_t *)(uintptr_t)(edi + (uint32_t)G32(VA_g_render_target_buffer));
    int32_t  n   = (int32_t)(ecx & 0xffff);
    do {
        uint8_t al = dst[0];
        uint8_t ah = gs_base[((uint32_t)bh << 8) | bl];
        al = es_base[((uint32_t)ah << 8) | al];
        dst[0] = al;
        uint32_t prev = esi;
        esi += ebp;
        bh = (uint8_t)(bh + dl + (esi < prev));      /* adc bh,dl: +carry from esi += ebp */
        dst += ROW;
    } while (--n > 0);
}

/* ============================ Batch 16 (renderer span cluster; case-2 grind) ============================
 * The render_world_span family (0x38xxx), reached via the fn-ptr dispatch table 0x71f80. Each is a
 * vertical span writer: ES self-loaded from g_transparency_blend_selector (0x90be2) = blend LUT, GS
 * preset by the caller = the shade. Faithful lift = identical framebuffer; the Watcom unroll/Duff
 * structure is an optimization that does not change observable output. */

/* render_world_col_tint_gs_385dc (0x385dc): 2x-unrolled CONSTANT-shade vertical blend. bl=G[0x90a24];
 * bh = gs:[(bh_in<<8)|bl] read ONCE (the shade level, constant over the span); then for `cx` rows
 * (stride 0x140 down g_render_target_buffer 0x85414) pixel = es:[(bh<<8)|pixel]. The asm splits
 * odd/even via `shr ecx,1`, and for odd counts backs up one row and enters the 2nd unrolled half;
 * net = blend `cx` consecutive rows. QUIRK kept faithfully: cx==0 still runs the do-while body once
 * (= 2 rows; the unrolled body has no pre-test). gs_base/es_base = the two selector bases. */
/* shared 0x38600+ loop: blend `cx` rows down the column with a CONSTANT shade row `bh` via es:[(bh<<8)|dst]. */
static void wmap_constshade_blend(uint32_t edi, uint32_t ecx, uint8_t bh, const uint8_t *es_base)
{
    uint8_t *p  = (uint8_t *)(uintptr_t)(edi + (uint32_t)G32(VA_g_render_target_buffer));
    int32_t  cnt = (int32_t)((ecx & 0xffff) >> 1);
    int      second = 0;
    if ((ecx & 0xffff) & 1) { cnt += 1; p -= ROW; second = 1; }/* odd: back up + enter 2nd half */
    do {
        if (!second) p[0] = es_base[((uint32_t)bh << 8) | p[0]];  /* 0x38610 */
        second = 0;
        p[ROW] = es_base[((uint32_t)bh << 8) | p[ROW]];       /* 0x38617 */
        p += 2 * ROW;
    } while (--cnt > 0);
}

void render_world_col_tint_gs_385dc(uint32_t edi, uint32_t ecx, uint8_t bh_in,
                                    const uint8_t *gs_base, const uint8_t *es_base)
{
    uint8_t bl = G8(VA_g_sprite_fill_index);
    wmap_constshade_blend(edi, ecx, gs_base[((uint32_t)bh_in << 8) | bl], es_base);  /* bh = gs[(bh_in<<8)|bl] */
}

/* render_world_col_tint_385d4 (0x385d4): the 8-byte stub entry to 385dc's blend loop with bh = byte[0x90a24]
 * DIRECTLY (skips the gs colormap lookup; `mov bh,[0x90a24]; jmp 0x385e6`). No GS. */
void render_world_col_tint_385d4(uint32_t edi, uint32_t ecx, const uint8_t *es_base)
{ wmap_constshade_blend(edi, ecx, G8(VA_g_sprite_fill_index), es_base); }

/* render_world_col_unshaded_masked_388be (0x388be): TEXTURED vertical span with transparency, NO segments (direct flat
 * writer). esi=texcoord (+texbase 0x84980), edi=dest (+fb 0x85414), eax=frac step (->ebp=eax<<16),
 * edx=int step, ecx=count. Walks the texture with a fixed-point coord (ebx=[0x8a344]<<16 accumulator;
 * per row ebx+=ebp, tex+=edx+carry) and writes the texel down the column (stride 0x140), SKIPPING
 * transparent texels (texel==0). 2x-unrolled with the odd/even entry trick. The tail (always run)
 * writes the final row after a texture WRAP check: if tex-texbase-[0x8a338] >= [0x90978], wrap tex to
 * [0x9097c]+texbase+[0x8a338]. QUIRK: count<=1 still writes 1 row via the tail. Net = max(1,count) rows. */
void render_world_col_unshaded_masked_388be(uint32_t eax, uint32_t ecx, uint32_t edx,
                                    uint32_t esi_in, uint32_t edi)
{
    uint32_t texbase = (uint32_t)G32(VA_g_render_source_base_ptr);
    uint8_t *tex = (uint8_t *)(uintptr_t)((esi_in & 0xffff) + texbase);
    uint8_t *di  = (uint8_t *)(uintptr_t)(edi + (uint32_t)G32(VA_g_render_target_buffer));
    uint32_t cx  = ecx & 0xffff;
    if (cx > 1) {
        uint32_t ebp = eax << 16;
        uint32_t ebx = (uint32_t)G32(VA_g_span_eax_accum_init) << 16;
        int32_t  cnt = (int32_t)(cx >> 1);
        int      second = !(cx & 1);                  /* even count enters at the 2nd unrolled half */
        if (second) di -= ROW;                      /* even: sub edi,0x140 */
        do {
            if (!second) {                            /* 0x3890e first half */
                uint8_t al = *tex;
                uint32_t o = ebx; ebx += ebp; tex += edx + (ebx < o);
                if (al) di[0] = al;
            }
            second = 0;
            {                                         /* 0x3891a second half */
                uint8_t al = *tex;
                uint32_t o = ebx; ebx += ebp; tex += edx + (ebx < o);
                if (al) di[ROW] = al;
            }
            di += 2 * ROW;
        } while (--cnt > 0);
    }
    /* tail (0x38933): final row, with the wrap check */
    uint32_t off = (uint32_t)(uintptr_t)tex - texbase - (uint32_t)G32(VA_g_span_src_wrap_base);
    if (off >= (uint32_t)G32(VA_g_span_src_row_width))
        tex = (uint8_t *)(uintptr_t)((uint32_t)G32(VA_g_span_src_wrap_reoffset) + texbase + (uint32_t)G32(VA_g_span_src_wrap_base));
    { uint8_t al = *tex; if (al) di[0] = al; }
}

/* render_world_span_390ac (0x390ac): the first SMC TEXTURED PERSPECTIVE COLUMN. 2x-unrolled, with
 * transparency (texel==0 -> skip) AND translucency (texel<0 / high bit -> es:[] 64K shadow blend).
 * Dual-segment: GS = 64K palette/shade remap (gs:[(coord_hi<<8)|texel]); ES = 64K translucency LUT
 * (the original self-loads ES from selector 0x90be2 — we take its base as es_base). Flat dest
 * [edi]=edi_in+g_render_target_buffer(0x85414), flat src [esi]=esi_in+g_render_source_base_ptr(0x84980).
 * The texture coordinate is a fixed-point accumulator over ebp(frac):eax(int):esi(srcptr) advanced by an
 * adc chain each pixel. The engine PATCHES the `add ebp`/`adc eax` step immediates from globals (the SMC
 * texture-mapper trick); we read those globals as ordinary locals:
 *   step_ebp = [0x8a2e0]<<16 ;  step_eax = (param_1<<16)|byte[0x8a2e2] ;  esi += param_2(EDX) + carry.
 * Inits: ebp=[0x8a2dc]<<16 ;  eax=([0x8a344]<<16) with low byte = bh (ebx_in>>8) ;  the coord-high byte
 * (bh) is refreshed to eax&0xff after each step. The always-run tail writes the final pixel after a
 * texture WRAP check (identical to 388be). ABI (fn-ptr dispatched): EAX=param_1, EDX=param_2 (src step),
 * ECX=param_3 (count, low16), EBX=ebx_in (bh=coord-high seed), ESI=esi_in (tex coord), EDI=edi_in (dest).
 * The SMC code-scratch is ephemeral (re-patched per call); observable output = the framebuffer, which the
 * oracle compares (and it saves/restores the patched code bytes to keep the image clean). */
static void px390(uint8_t *dst, uint32_t coord_hi, uint8_t texel,
                  const uint8_t *gs_base, const uint8_t *es_base)
{
    if (!texel) return;                                       /* transparent */
    uint8_t gscol = gs_base[((coord_hi & 0xffu) << 8) | texel];
    if (texel & 0x80u)                                        /* translucent: blend with existing dest */
        *dst = es_base[((uint32_t)(*dst) << 8) | gscol];
    else
        *dst = gscol;                                         /* opaque */
}

void render_world_span_390ac(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                    uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in,
                                    const uint8_t *gs_base, const uint8_t *es_base)
{
    uint32_t texbase  = (uint32_t)G32(VA_g_render_source_base_ptr);
    uint8_t *tex = (uint8_t *)(uintptr_t)((esi_in & 0xffff) + texbase);
    uint8_t *di  = (uint8_t *)(uintptr_t)(edi_in + (uint32_t)G32(VA_g_render_target_buffer));
    uint32_t cx  = param_3 & 0xffff;

    uint32_t step_ebp = (uint32_t)G32(VA_g_span_pixel_step) << 16;
    uint32_t step_eax = (param_1 << 16) | (uint32_t)G8(VA_g_span_eax_step_lo);
    uint32_t ebp = (uint32_t)G32(VA_g_span_accum_init) << 16;
    uint32_t eax = (uint32_t)G32(VA_g_span_eax_accum_init) << 16;
    eax = (eax & 0xffffff00u) | ((ebx_in >> 8) & 0xffu);      /* al = bh (coord-high seed) */

#define STEP390 do { \
        uint32_t o_ = ebp; ebp += step_ebp; unsigned cf_ = (ebp < o_); \
        uint64_t s_ = (uint64_t)eax + step_eax + cf_; eax = (uint32_t)s_; cf_ = (unsigned)(s_ >> 32); \
        tex += param_2 + cf_; \
    } while (0)

    if (cx > 1) {
        int32_t cnt = (int32_t)(cx >> 1);
        int second = !(cx & 1);                               /* even count enters at the 2nd half */
        if (second) di -= ROW;
        do {
            if (!second) { px390(&di[0], eax, *tex, gs_base, es_base); STEP390; }
            second = 0;
            px390(&di[ROW], eax, *tex, gs_base, es_base); STEP390;
            di += 2 * ROW;
        } while (--cnt > 0);
    }
    /* tail (always): texture wrap check, then the final pixel. */
    uint32_t off = (uint32_t)(uintptr_t)tex - texbase - (uint32_t)G32(VA_g_span_src_wrap_base);
    if (off >= (uint32_t)G32(VA_g_span_src_row_width))
        tex = (uint8_t *)(uintptr_t)((uint32_t)G32(VA_g_span_src_wrap_reoffset) + texbase + (uint32_t)G32(VA_g_span_src_wrap_base));
    px390(&di[0], eax, *tex, gs_base, es_base);
#undef STEP390
}

/* render_world_span_wrapped_391d0 (0x391d0): the TILED sibling of 390ac.
 * Same dual-segment ABI, but the source is a wrapped index `tex[esi + (ebx & MASK)]` (power-of-two texture
 * wrap) rather than a linear pointer walk, the shade/int accumulator is EDX (shade in dl/ah, not al/bh),
 * the index low byte advances via `adc bl,step_bl` (step_bl = param_2 low byte, patched from dl), and there
 * is NO always-run tail. The wrap MASK is itself an SMC immediate that the CALLER patches (not a global we
 * can read), so it is a parameter here (the oracle patches the code immediate to match). Step immediates
 * (add ebp / adc edx) are self-patched from globals — read as locals. Unroll entry is inverted vs 390ac:
 * even count enters the first half, odd count enters the second half (edi-=0x140); loop is `dec ecx; jne`
 * over cnt=(count+1)>>1 passes (so count==0 still writes 2 px — the unrolled-body quirk). ABI: EAX=param_1
 * (eax/edx step hi), EDX=param_2 (index step low), ECX=param_3 (count low16), EBX=ebx_in (bh=shade seed),
 * ESI=esi_in (src coord), EDI=edi_in (dest), MASK = caller-patched wrap mask. */
static void pxw391(uint8_t *dst, uint32_t shade, uint8_t texel,
                   const uint8_t *gs_base, const uint8_t *es_base)
{
    if (!texel) return;
    uint8_t gscol = gs_base[((shade & 0xffu) << 8) | texel];
    if (texel & 0x80u)
        *dst = es_base[((uint32_t)(*dst) << 8) | gscol];
    else
        *dst = gscol;
}

void render_world_span_wrapped_391d0(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                            uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in,
                                            uint32_t mask, const uint8_t *gs_base, const uint8_t *es_base)
{
    uint32_t wrap_base = (uint32_t)G32(VA_g_span_src_wrap_base);
    uint8_t *src = (uint8_t *)(uintptr_t)(wrap_base + (uint32_t)G32(VA_g_render_source_base_ptr));   /* esi: FIXED src base */
    uint8_t *di  = (uint8_t *)(uintptr_t)(edi_in + (uint32_t)G32(VA_g_render_target_buffer));
    uint32_t ebx = (esi_in & 0xffff) - wrap_base;                                /* wrapped index */
    uint32_t cx  = param_3 & 0xffff;

    uint32_t step_ebp = (uint32_t)G32(VA_g_span_pixel_step) << 16;
    uint32_t step_edx = (param_1 << 16) | (uint32_t)G8(VA_g_span_eax_step_lo);
    uint8_t  step_bl  = (uint8_t)param_2;
    uint32_t ebp = (uint32_t)G32(VA_g_span_accum_init) << 16;
    uint32_t edx = (uint32_t)G32(VA_g_span_eax_accum_init) << 16;
    edx = (edx & 0xffffff00u) | ((ebx_in >> 8) & 0xffu);        /* dl = bh (shade seed) */

#define STEPW391 do { \
        uint32_t o_ = ebp; ebp += step_ebp; unsigned cf_ = (ebp < o_); \
        uint64_t s_ = (uint64_t)edx + step_edx + cf_; edx = (uint32_t)s_; cf_ = (unsigned)(s_ >> 32); \
        uint32_t bl_ = (ebx & 0xffu) + step_bl + cf_; ebx = (ebx & 0xffffff00u) | (bl_ & 0xffu); \
    } while (0)

    int32_t cnt = (int32_t)((cx + 1) >> 1);
    int second_entry = (cx & 1);                                /* odd count enters the 2nd half */
    if (second_entry) di -= ROW;
    do {
        if (!second_entry) { ebx &= mask; pxw391(&di[0], edx, src[ebx], gs_base, es_base); STEPW391; }
        second_entry = 0;
        ebx &= mask; pxw391(&di[ROW], edx, src[ebx], gs_base, es_base); STEPW391;
        di += 2 * ROW;
    } while (--cnt > 0);
#undef STEPW391
}

/* render_world_col_shaded_blend_2axis_392cc (0x392cc): tiled variant whose wrap MASK comes from a GLOBAL
 * (g_span_src_wrap_reoffset 0x9097c) rather than a caller patch. There is NO frac accumulator — a single
 * fixed-point accumulator EDX (step = param_1<<16, self-patched) drives the carry into the wrapped index
 * EBX (`adc ebx, step_ebx`; step = sign-extended param_2 byte). The shade (gs colormap row = eax high byte)
 * is STATEFUL: it starts at BH and is PRESERVED across opaque/transparent pixels, but RESET to dl (edx&0xff,
 * post-step) after a translucent pixel — faithfully reproduced. dual-seg GS palette + ES blend; flat dst
 * [edi]+fb; src base = wrap_base+texbase; index init = (esi_in - wrap_base) low16. even count -> first half
 * (cnt=count>>1); odd -> second half (edi-=0x140, cnt=(count>>1)+1); no tail. ABI: EAX=param_1, EDX=param_2
 * (index step byte), ECX=param_3 (count), EBX=ebx_in (bh=shade seed), ESI=esi_in, EDI=edi_in. */
void render_world_col_shaded_blend_2axis_392cc(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                    uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in,
                                    const uint8_t *gs_base, const uint8_t *es_base)
{
    uint32_t wrap_base = (uint32_t)G32(VA_g_span_src_wrap_base);
    uint8_t *src = (uint8_t *)(uintptr_t)(wrap_base + (uint32_t)G32(VA_g_render_source_base_ptr));
    uint8_t *di  = (uint8_t *)(uintptr_t)(edi_in + (uint32_t)G32(VA_g_render_target_buffer));
    uint32_t mask = (uint32_t)G32(VA_g_span_src_wrap_reoffset);
    uint32_t ebx  = (uint16_t)((uint16_t)esi_in - (uint16_t)wrap_base);   /* sub bx,si (16-bit) */
    uint32_t edx  = (uint32_t)G32(VA_g_span_eax_accum_init) << 16;
    edx = (edx & 0xffffff00u) | ((ebx_in >> 8) & 0xffu);                  /* dl = bh */
    uint32_t shade = (ebx_in >> 8) & 0xffu;                               /* ah init = BH (stateful) */
    uint32_t step_edx = param_1 << 16;
    int32_t  step_ebx = (int32_t)(int8_t)(uint8_t)param_2;                /* adc ebx, imm8 (sign-extended) */
    uint32_t cx = param_3;

#define PX392(dstp) do { \
        ebx &= mask; \
        uint8_t texel = src[ebx]; \
        uint32_t ah_in = shade; \
        uint64_t s_ = (uint64_t)edx + step_edx; unsigned cf_ = (unsigned)(s_ >> 32); edx = (uint32_t)s_; \
        ebx = (uint32_t)((int32_t)ebx + step_ebx + (int32_t)cf_); \
        if (texel) { \
            uint8_t gscol = gs_base[((ah_in & 0xffu) << 8) | texel]; \
            if (texel & 0x80u) { *(dstp) = es_base[((uint32_t)(*(dstp)) << 8) | gscol]; shade = edx & 0xffu; } \
            else { *(dstp) = gscol; } \
        } \
    } while (0)

    uint32_t cnt = cx >> 1;
    int second_first = (cx & 1);
    if (second_first) { di -= ROW; cnt += 1; }
    do {
        if (!second_first) PX392(&di[0]);
        second_first = 0;
        PX392(&di[ROW]);
        di += 2 * ROW;
    } while ((int32_t)(--cnt) > 0);
#undef PX392
}

/* render_world_col_blend_2axis_39398 (0x39398): the UNLIT/unremapped variant — writes the raw texel directly (NO GS
 * colormap), with transparency (texel==0 skip) and translucency (texel<0 -> es[(dst<<8)|texel]). ES only,
 * no GS. The frac-step and the wrap-mask are packed into one value: edx = (param_1<<16) | word[0x9097c],
 * used BOTH as the `and ebx` mask AND the `add ebp` frac step; the index `adc ebx, step_ebx` step is the
 * input EDX (param_2, self-patched). src base = wrap_base+texbase; index init = (esi_in - wrap_base) low16.
 * even count -> first half (cnt=(count+1)>>1); odd -> second half (edi-=0x140). Step is AFTER the pixel.
 * ABI: EAX=param_1, EDX=param_2 (index step), ECX=param_3 (count), ESI=esi_in, EDI=edi_in (no BH/GS). */
void render_world_col_blend_2axis_39398(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                    uint32_t esi_in, uint32_t edi_in, const uint8_t *es_base)
{
    uint32_t wrap_base = (uint32_t)G32(VA_g_span_src_wrap_base);
    uint8_t *src = (uint8_t *)(uintptr_t)(wrap_base + (uint32_t)G32(VA_g_render_source_base_ptr));
    uint8_t *di  = (uint8_t *)(uintptr_t)(edi_in + (uint32_t)G32(VA_g_render_target_buffer));
    uint32_t maskval = (param_1 << 16) | (uint32_t)(uint16_t)G16(VA_g_span_src_wrap_reoffset);   /* edx: mask + ebp frac step */
    uint32_t ebx = (uint16_t)((uint16_t)esi_in - (uint16_t)wrap_base);
    uint32_t ebp = (uint32_t)G32(VA_g_span_eax_accum_init) << 16;
    uint32_t step_ebx = param_2;
    uint32_t cx = param_3 & 0xffff;

#define PX398(dstp) do { \
        ebx &= maskval; \
        uint8_t texel = src[ebx]; \
        if (texel) { \
            if (texel & 0x80u) *(dstp) = es_base[((uint32_t)(*(dstp)) << 8) | texel]; \
            else *(dstp) = texel; \
        } \
        uint64_t s_ = (uint64_t)ebp + maskval; unsigned cf_ = (unsigned)(s_ >> 32); ebp = (uint32_t)s_; \
        ebx = ebx + step_ebx + cf_; \
    } while (0)

    uint32_t cnt = (cx + 1) >> 1;
    int second_first = (cx & 1);
    if (second_first) di -= ROW;
    do {
        if (!second_first) PX398(&di[0]);
        second_first = 0;
        PX398(&di[ROW]);
        di += 2 * ROW;
    } while (--cnt > 0);
#undef PX398
}

/* render_world_col_blend_masked_39453 (0x39453): the LINEAR unlit variant — like 388be (linear texture walk +
 * transparency + wrap-tail) but with translucency (texel<0 -> es[(dst<<8)|texel]) and raw texels (no GS).
 * NOT self-modifying: the frac step lives in ebp (param_1<<16), not a patched immediate. Single carry
 * level: ebx (int accumulator = [0x8a344]<<16) += ebp each pixel, carry into esi (src ptr) which also
 * advances by edx(param_2). even count -> 2nd half (edi-=0x140), odd -> 1st (cnt=count>>1); the always-run
 * tail re-checks the texture wrap. ABI: EAX=param_1 (frac step), EDX=param_2 (src step), ECX=param_3
 * (count), ESI=esi_in, EDI=edi_in; ES self-loaded (no GS, no BH). */
static void pxlin39453(uint8_t *dst, uint8_t texel, const uint8_t *es_base)
{
    if (!texel) return;
    if (texel & 0x80u) *dst = es_base[((uint32_t)(*dst) << 8) | texel];
    else *dst = texel;
}

void render_world_col_blend_masked_39453(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                    uint32_t esi_in, uint32_t edi_in, const uint8_t *es_base)
{
    uint32_t texbase = (uint32_t)G32(VA_g_render_source_base_ptr);
    uint8_t *tex = (uint8_t *)(uintptr_t)((esi_in & 0xffff) + texbase);
    uint8_t *di  = (uint8_t *)(uintptr_t)(edi_in + (uint32_t)G32(VA_g_render_target_buffer));
    uint32_t cx  = param_3 & 0xffff;
    uint32_t ebp = param_1 << 16;                              /* frac step (held in a register, no SMC) */

#define STEP453 do { uint32_t o_ = ebx; ebx += ebp; tex += param_2 + (ebx < o_); } while (0)

    if (cx > 1) {
        uint32_t ebx = (uint32_t)G32(VA_g_span_eax_accum_init) << 16;           /* int accumulator */
        int32_t cnt = (int32_t)(cx >> 1);
        int second = !(cx & 1);                                /* even -> enter 2nd half */
        if (second) di -= ROW;
        do {
            if (!second) { uint8_t al = *tex; STEP453; pxlin39453(&di[0], al, es_base); }
            second = 0;
            { uint8_t al = *tex; STEP453; pxlin39453(&di[ROW], al, es_base); }
            di += 2 * ROW;
        } while (--cnt > 0);
    }
    /* tail (always): texture wrap check, then final pixel. */
    uint32_t off = (uint32_t)(uintptr_t)tex - texbase - (uint32_t)G32(VA_g_span_src_wrap_base);
    if (off >= (uint32_t)G32(VA_g_span_src_row_width))
        tex = (uint8_t *)(uintptr_t)((uint32_t)G32(VA_g_span_src_wrap_reoffset) + texbase + (uint32_t)G32(VA_g_span_src_wrap_base));
    pxlin39453(&di[0], *tex, es_base);
#undef STEP453
}

/* render_world_col_shaded_blend_masked_39520 (0x39520): linear variant with a FLAT colormap (not GS). Per pixel the texel is
 * looked up in a flat table via the low-byte-replace idiom: `bl=texel; bl=[ebx]` -> cmap[(base & ~0xff) |
 * texel], base = ebx_in + g_8a2ac (a constant shade-row pointer; the "shade" is its high bytes). Then
 * transparency (texel==0 skip) and translucency (texel<0 -> es[(dst<<8)|cmap_val]). SMC: the frac step is
 * patched into `add ebp,imm` from param_1<<16 (we read it as a local). Single carry: ebp(frac)+=step ->
 * carry into esi (src ptr, += param_2). Linear src (esi_in+texbase), wrap-tail like 39453. even count ->
 * 2nd half (edi-=0x140), odd -> 1st (cnt=count>>1). ABI: EAX=param_1, EDX=param_2 (src step), ECX=param_3
 * (count), EBX=ebx_in (colormap base part), ESI=esi_in, EDI=edi_in; ES self-loaded, no GS. */
void render_world_col_shaded_blend_masked_39520(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                    uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in,
                                    const uint8_t *es_base)
{
    uint32_t texbase = (uint32_t)G32(VA_g_render_source_base_ptr);
    uint8_t *tex = (uint8_t *)(uintptr_t)((esi_in & 0xffff) + texbase);
    uint8_t *di  = (uint8_t *)(uintptr_t)(edi_in + (uint32_t)G32(VA_g_render_target_buffer));
    uint32_t cmap = ebx_in + (uint32_t)G32(VA_g_active_world_remap_base);          /* flat colormap base (ebx) */
    uint32_t cx  = param_3 & 0xffff;
    uint32_t step_ebp = param_1 << 16;

    /* one pixel: texel -> low-byte-replace colormap -> opaque/translucent. */
    #define PX520(dstp, texel) do { \
        if (texel) { \
            uint8_t cval = *(const uint8_t *)(uintptr_t)((cmap & 0xffffff00u) | (uint32_t)(texel)); \
            if ((texel) & 0x80u) *(dstp) = es_base[((uint32_t)(*(dstp)) << 8) | cval]; \
            else *(dstp) = cval; \
        } \
    } while (0)

    if (cx > 1) {
        uint32_t ebp = (uint32_t)G32(VA_g_span_eax_accum_init) << 16;
        int32_t cnt = (int32_t)(cx >> 1);
        int second = !(cx & 1);                               /* even -> enter 2nd half */
        if (second) di -= ROW;
        do {
            if (!second) { uint8_t al = *tex; uint32_t o = ebp; ebp += step_ebp; tex += param_2 + (ebp < o); PX520(&di[0], al); }
            second = 0;
            { uint8_t al = *tex; uint32_t o = ebp; ebp += step_ebp; tex += param_2 + (ebp < o); PX520(&di[ROW], al); }
            di += 2 * ROW;
        } while (--cnt > 0);
    }
    /* tail (always): texture wrap check, then final pixel. */
    uint32_t off = (uint32_t)(uintptr_t)tex - (uint32_t)G32(VA_g_span_src_wrap_base) - texbase;
    if (off >= (uint32_t)G32(VA_g_span_src_row_width))
        tex = (uint8_t *)(uintptr_t)((uint32_t)G32(VA_g_span_src_wrap_reoffset) + texbase + (uint32_t)G32(VA_g_span_src_wrap_base));
    PX520(&di[0], *tex);
    #undef PX520
}

/* ===================== Batch 59 (wall-only column mappers, 0x37xxx/0x38xxx) =====================
 * The remaining draw_world_surface_spans (0x36b39) column mappers not shared with the sprite driver.
 * Same vertical-span shape (stride 0x140 down g_render_target_buffer 0x85414, src = texbase 0x84980 +
 * coord) but unshaded/shaded/clipped variants. Routed from dispatch_world_span_column's do_mapper. */

/* render_world_col_unshaded_37c60 (0x37c60): UNSHADED OPAQUE textured column (no segments, no transparency — the
 * plain texel copy; = 388be without the texel==0 skip). Linear walk (esi+texbase, ebx=[0x8a344]<<16
 * accumulator; per row ebx+=ebp, tex+=edx+carry), writes the texel down the column UNCONDITIONALLY.
 * 2x-unrolled (even count enters 2nd half); always-run wrap-checked tail. NON-SMC: frac step ebp=eax<<16
 * (from param_1, not a global). ABI: EAX=param_1 (frac step), EDX=param_2 (int step), ECX=count,
 * ESI=esi_in, EDI=edi_in. */
void render_world_col_unshaded_37c60(uint32_t eax, uint32_t ecx, uint32_t edx,
                                    uint32_t esi_in, uint32_t edi)
{
    uint32_t texbase = (uint32_t)G32(VA_g_render_source_base_ptr);
    uint8_t *tex = (uint8_t *)(uintptr_t)((esi_in & 0xffff) + texbase);
    uint8_t *di  = (uint8_t *)(uintptr_t)(edi + (uint32_t)G32(VA_g_render_target_buffer));
    uint32_t cx  = ecx & 0xffff;
    if (cx > 1) {
        uint32_t ebp = eax << 16;
        uint32_t ebx = (uint32_t)G32(VA_g_span_eax_accum_init) << 16;
        int32_t  cnt = (int32_t)(cx >> 1);
        int      second = !(cx & 1);                  /* even count enters the 2nd unrolled half */
        if (second) di -= ROW;
        do {
            if (!second) { uint8_t al = *tex; uint32_t o = ebx; ebx += ebp; tex += edx + (ebx < o); di[0] = al; }
            second = 0;
            { uint8_t al = *tex; uint32_t o = ebx; ebx += ebp; tex += edx + (ebx < o); di[ROW] = al; }
            di += 2 * ROW;
        } while (--cnt > 0);
    }
    uint32_t off = (uint32_t)(uintptr_t)tex - texbase - (uint32_t)G32(VA_g_span_src_wrap_base);
    if (off >= (uint32_t)G32(VA_g_span_src_row_width))
        tex = (uint8_t *)(uintptr_t)((uint32_t)G32(VA_g_span_src_wrap_reoffset) + texbase + (uint32_t)G32(VA_g_span_src_wrap_base));
    di[0] = *tex;
}

/* SHADED column (GS colormap) shared by 0x37ec8 (opaque) and 0x38198 (transparent — its sibling, identical
 * apart from the per-pixel `texel==0 -> skip` guard; the write-vs-step order differs cosmetically but the
 * written pixel/dest are computed pre-step, so the output is identical). Each pixel: pixel=gs[(bh<<8)|texel];
 * a 3-level fixed-point step advances sub-pixel shade + texel: ebp(frac)+=step_ebp -> carry into eax(shade
 * accum)+=step_eax -> carry into tex(src)+=param_2; then bh=al (new shade row = eax low byte). SMC steps read
 * live: step_ebp=[0x8a2e0]<<16, step_eax=(param_1<<16)|byte[0x8a2e2]. eax init=([0x8a344]<<16)|bh_in,
 * ebp init=[0x8a2dc]<<16, bh_in=(ebx_in>>8). 2x-unrolled; always-run wrap-checked tail. */
static void wmap_shaded(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                        uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in,
                        const uint8_t *gs_base, int transparent)
{
    uint32_t texbase = (uint32_t)G32(VA_g_render_source_base_ptr);
    uint8_t *tex = (uint8_t *)(uintptr_t)((esi_in & 0xffff) + texbase);
    uint8_t *di  = (uint8_t *)(uintptr_t)(edi_in + (uint32_t)G32(VA_g_render_target_buffer));
    uint32_t cx  = param_3 & 0xffff;
    uint8_t  bh  = (uint8_t)(ebx_in >> 8);

    if (cx > 1) {
        uint32_t step_ebp = (uint32_t)G32(VA_g_span_pixel_step) << 16;
        uint32_t step_eax = (param_1 << 16) | (uint32_t)G8(VA_g_span_eax_step_lo);
        uint32_t ebp = (uint32_t)G32(VA_g_span_accum_init) << 16;
        uint32_t eax = ((uint32_t)G32(VA_g_span_eax_accum_init) << 16) | bh;     /* al = bh_in */
        int32_t  cnt = (int32_t)(cx >> 1);
        int      second = !(cx & 1);

        #define STEP_SH do { \
            uint32_t o_ = ebp; ebp += step_ebp; unsigned cf_ = (ebp < o_); \
            uint64_t s_ = (uint64_t)eax + step_eax + cf_; eax = (uint32_t)s_; cf_ = (unsigned)(s_ >> 32); \
            tex += param_2 + cf_; \
        } while (0)
        #define WRPIX_SH(dstp) do { \
            uint8_t texel = *tex; \
            if (!transparent || texel) *(dstp) = gs_base[((uint32_t)bh << 8) | texel]; \
        } while (0)

        if (second) di -= ROW;
        do {
            if (!second) { WRPIX_SH(&di[0]); STEP_SH; bh = (uint8_t)eax; }
            second = 0;
            WRPIX_SH(&di[ROW]); STEP_SH; di += 2 * ROW; bh = (uint8_t)eax;
        } while (--cnt > 0);
        #undef STEP_SH
        #undef WRPIX_SH
    }
    uint32_t off = (uint32_t)(uintptr_t)tex - texbase - (uint32_t)G32(VA_g_span_src_wrap_base);
    if (off >= (uint32_t)G32(VA_g_span_src_row_width))
        tex = (uint8_t *)(uintptr_t)((uint32_t)G32(VA_g_span_src_wrap_reoffset) + texbase + (uint32_t)G32(VA_g_span_src_wrap_base));
    { uint8_t texel = *tex; if (!transparent || texel) di[0] = gs_base[((uint32_t)bh << 8) | texel]; }
}

/* render_world_col_shaded_gs_37ec8 (0x37ec8): SHADED OPAQUE column. ABI: EAX=param_1, EDX=param_2(src step),
 * ECX=count, EBX=ebx_in(bh=shade seed), ESI=esi_in, EDI=edi_in; GS=colormap. */
void render_world_col_shaded_gs_37ec8(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                    uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in, const uint8_t *gs_base)
{ wmap_shaded(param_1, param_2, param_3, ebx_in, esi_in, edi_in, gs_base, 0); }

/* render_world_col_shaded_masked_gs_38198 (0x38198): SHADED TRANSPARENT column (sibling of 37ec8 + texel==0 skip). */
void render_world_col_shaded_masked_gs_38198(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                    uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in, const uint8_t *gs_base)
{ wmap_shaded(param_1, param_2, param_3, ebx_in, esi_in, edi_in, gs_base, 1); }

/* WRAPPED RAW (no colormap, no segment) column shared by 0x3832c (opaque) and 0x383ac (transparent — its
 * sibling + texel==0 skip). The minimal tiled writer: src base = wrap_base(0x8a338)+texbase(0x84980), index
 * EBX = (esi_in - wrap_base) low16, wrapped each pixel by `ebx &= maskval` where maskval =
 * (param_1<<16)|word[0x9097c] is used BOTH as the AND mask (low16) AND the EBP frac step (= the 0x39398
 * idiom). Per pixel: texel = src[ebx&maskval]; write the RAW texel (no shade/no blend); then ebp += maskval
 * -> carry into ebx += step_ebx(param_2). 2x-unrolled, even count -> first half / odd -> second (edi-=0x140),
 * cnt=(count+1)>>1; no tail. SMC `adc ebx,imm` step read live as param_2. ABI: EAX=param_1, EDX=param_2
 * (index step), ECX=count, ESI=esi_in, EDI=edi_in (no segments). */
static void wmap_wrapped_raw(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                             uint32_t esi_in, uint32_t edi_in, int transparent)
{
    uint32_t wrap_base = (uint32_t)G32(VA_g_span_src_wrap_base);
    uint8_t *src = (uint8_t *)(uintptr_t)(wrap_base + (uint32_t)G32(VA_g_render_source_base_ptr));
    uint8_t *di  = (uint8_t *)(uintptr_t)(edi_in + (uint32_t)G32(VA_g_render_target_buffer));
    uint32_t maskval = (param_1 << 16) | (uint32_t)(uint16_t)G16(VA_g_span_src_wrap_reoffset);
    uint32_t ebx = (uint16_t)((uint16_t)esi_in - (uint16_t)wrap_base);
    uint32_t ebp = (uint32_t)G32(VA_g_span_eax_accum_init) << 16;
    uint32_t step_ebx = param_2;
    uint32_t cx = param_3 & 0xffff;

    #define PXRAW(dstp) do { \
        ebx &= maskval; \
        uint8_t texel = src[ebx]; \
        if (!transparent || texel) *(dstp) = texel; \
        uint64_t s_ = (uint64_t)ebp + maskval; unsigned cf_ = (unsigned)(s_ >> 32); ebp = (uint32_t)s_; \
        ebx = ebx + step_ebx + cf_; \
    } while (0)

    uint32_t cnt = (cx + 1) >> 1;
    int second_first = (cx & 1);
    if (second_first) di -= ROW;
    do {
        if (!second_first) PXRAW(&di[0]);
        second_first = 0;
        PXRAW(&di[ROW]);
        di += 2 * ROW;
    } while (--cnt > 0);
    #undef PXRAW
}

/* render_world_col_unshaded_2axis_3832c (0x3832c): UNSHADED OPAQUE wrapped column. */
void render_world_col_unshaded_2axis_3832c(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                    uint32_t esi_in, uint32_t edi_in)
{ wmap_wrapped_raw(param_1, param_2, param_3, esi_in, edi_in, 0); }

/* render_world_col_unshaded_masked_2axis_383ac (0x383ac): UNSHADED TRANSPARENT wrapped column (sibling of 3832c + texel==0 skip). */
void render_world_col_unshaded_masked_2axis_383ac(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                    uint32_t esi_in, uint32_t edi_in)
{ wmap_wrapped_raw(param_1, param_2, param_3, esi_in, edi_in, 1); }

/* render_world_col_unshaded_masked_2axis_38288 (0x38288): UNSHADED TRANSPARENT wrapped column with a FLAT colormap (the 39520
 * low-byte-replace idiom, but tiled instead of linear). src base = wrap_base(0x8a338)+texbase(0x84980),
 * index EBX = (esi_in - wrap_base) low16, wrapped `&= mask` (mask=dword[0x9097c]). Per pixel: texel =
 * src[ebx&mask]; a single fixed-point accumulator EDX (frac, init [0x8a344]<<16, step param_1<<16) carries
 * into the index `adc ebx,imm8` (sign-extended param_2 byte); transparent (texel==0 skip) -> write
 * cmap[(cmap_base&~0xff)|texel] where cmap_base = (ebx_in&0xffff)+[0x8a2ac]. NO segment, NO es blend. NOTE:
 * ecx is NOT masked here (faithful to the asm) and the loop is `dec ecx; jg` (signed). cnt=(count+1)>>1,
 * even->first half / odd->second (edi-=0x140); no tail. ABI: EAX=param_1, EDX=param_2(index step), ECX=count,
 * EBX=ebx_in(cmap base), ESI=esi_in, EDI=edi_in. */
void render_world_col_unshaded_masked_2axis_38288(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                    uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in)
{
    uint32_t wrap_base = (uint32_t)G32(VA_g_span_src_wrap_base);
    uint8_t *src = (uint8_t *)(uintptr_t)(wrap_base + (uint32_t)G32(VA_g_render_source_base_ptr));
    uint8_t *di  = (uint8_t *)(uintptr_t)(edi_in + (uint32_t)G32(VA_g_render_target_buffer));
    uint32_t mask = (uint32_t)G32(VA_g_span_src_wrap_reoffset);
    uint32_t cmap = (ebx_in & 0xffff) + (uint32_t)G32(VA_g_active_world_remap_base);
    uint32_t ebx = (uint16_t)((uint16_t)esi_in - (uint16_t)wrap_base);
    uint32_t edx = (uint32_t)G32(VA_g_span_eax_accum_init) << 16;
    uint32_t step_edx = param_1 << 16;
    int32_t  idx_step = (int32_t)(int8_t)(uint8_t)param_2;       /* adc ebx,imm8 (sign-extended) */
    uint32_t cx = param_3;                                       /* NOT masked (faithful) */

    #define PX288(dstp) do { \
        ebx &= mask; \
        uint8_t texel = src[ebx]; \
        uint64_t s_ = (uint64_t)edx + step_edx; unsigned cf_ = (unsigned)(s_ >> 32); edx = (uint32_t)s_; \
        ebx = (uint32_t)((int32_t)ebx + idx_step + (int32_t)cf_); \
        if (texel) *(dstp) = *(const uint8_t *)(uintptr_t)((cmap & 0xffffff00u) | texel); \
    } while (0)

    uint32_t cnt = (cx + 1) >> 1;
    int second_first = (cx & 1);
    if (second_first) di -= ROW;
    do {
        if (!second_first) PX288(&di[0]);
        second_first = 0;
        PX288(&di[ROW]);
        di += 2 * ROW;
    } while ((int32_t)(--cnt) > 0);
    #undef PX288
}

/* render_world_col_unshaded_opaque_37fac (0x37fac): UNSHADED OPAQUE wrapped column with a FLAT colormap (= 38288 minus the
 * transparency skip) PLUS an optional 2-row COLORMAP DITHER. byte[0x8a2dd]&0x80 selects: clear -> both rows
 * read cmap row N (offset 0; PATH A 0x37fb9); set -> alternate rows read cmap row N (off 0) and N-1 (off
 * -0x100, one colormap row darker), the phase from parity = (byte[0x8a34e]+count+dword[0x8a2b0])&1 (PATH B
 * 0x38070 even: top=0/mid=-0x100; PATH C 0x38107 odd: top=-0x100/mid=0). Per pixel: texel = src[ebx&mask];
 * a single accumulator EDX(frac, step param_1<<16) carries into the index `adc ebx,imm8`(sign-ext param_2);
 * write cmap[(base&~0xff)|texel + row_off], base=(ebx_in&0xffff)+[0x8a2ac]. ecx NOT masked. No segment, no
 * transparency. cnt=(count+1)>>1, even->first half/odd->second; no tail. ABI: EAX=param_1, EDX=param_2(index
 * step), ECX=count, EBX=ebx_in(cmap base), ESI=esi_in, EDI=edi_in. */
void render_world_col_unshaded_opaque_37fac(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                    uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in)
{
    uint32_t wrap_base = (uint32_t)G32(VA_g_span_src_wrap_base);
    uint8_t *src = (uint8_t *)(uintptr_t)(wrap_base + (uint32_t)G32(VA_g_render_source_base_ptr));
    uint8_t *di  = (uint8_t *)(uintptr_t)(edi_in + (uint32_t)G32(VA_g_render_target_buffer));
    uint32_t mask = (uint32_t)G32(VA_g_span_src_wrap_reoffset);
    uint32_t cmap = (ebx_in & 0xffff) + (uint32_t)G32(VA_g_active_world_remap_base);
    uint32_t ebx = (uint16_t)((uint16_t)esi_in - (uint16_t)wrap_base);
    uint32_t edx = (uint32_t)G32(VA_g_span_eax_accum_init) << 16;
    uint32_t step_edx = param_1 << 16;
    int32_t  idx_step = (int32_t)(int8_t)(uint8_t)param_2;
    uint32_t cx = param_3;                                       /* NOT masked (faithful) */

    int32_t top_off, mid_off;
    if (!(G8(VA_g_span_shade_seed) & 0x80)) { top_off = 0; mid_off = 0; }     /* PATH A: no dither */
    else {
        uint8_t parity = (uint8_t)((uint32_t)G8(VA_g_span_draw_mode_flags + 0x2) + cx + (uint32_t)G32(VA_g_active_world_remap_base + 0x4));
        if (parity & 1) { top_off = -0x100; mid_off = 0; }       /* PATH C (odd) */
        else            { top_off = 0; mid_off = -0x100; }       /* PATH B (even) */
    }

    #define PXFAC(dstp, off) do { \
        ebx &= mask; \
        uint8_t texel = src[ebx]; \
        uint64_t s_ = (uint64_t)edx + step_edx; unsigned cf_ = (unsigned)(s_ >> 32); edx = (uint32_t)s_; \
        ebx = (uint32_t)((int32_t)ebx + idx_step + (int32_t)cf_); \
        *(dstp) = *(const uint8_t *)(uintptr_t)(((cmap & 0xffffff00u) | texel) + (uint32_t)(int32_t)(off)); \
    } while (0)

    uint32_t cnt = (cx + 1) >> 1;
    int second_first = (cx & 1);
    if (second_first) di -= ROW;
    do {
        if (!second_first) PXFAC(&di[0], top_off);
        second_first = 0;
        PXFAC(&di[ROW], mid_off);
        di += 2 * ROW;
    } while ((int32_t)(--cnt) > 0);
    #undef PXFAC
}

/* render_world_col_unshaded_masked_38964 (0x38964): the LINEAR flat-colormap TRANSPARENT column with the same 2-row dither
 * as 37fac (the linear sibling of 38288). Only this entry's first 3 paths (0x38964..0x38b53) are reachable
 * from the wall dispatch table; the fs/x-flip bodies at 0x38e6d+ in the same 1864B region belong to the
 * (dead-in-host) subpass path and are NOT table-referenced. Linear walk: tex=(esi_in&0xffff)+texbase; per
 * pixel texel=*tex; ebp(frac,init [0x8a344]<<16)+=eax<<16 -> carry into tex+=edx; transparent (texel==0
 * skip) -> write cmap[(base&~0xff)|texel + row_off], base=ebx_in+[0x8a2ac] (UNMASKED). Dither (byte[0x8a2dd]
 * &0x80): parity=(byte[0x8a34e]+count+dword[0x8a2b0])&1 -> odd: top=-0x100/mid=0; even: top=0/mid=-0x100; the
 * always-run wrap-tail uses top_off. 388be-style unroll (cnt=count>>1, even count enters 2nd half). NON-SMC
 * (step in eax/edx regs). ABI: EAX=param_1(frac step), EDX=param_2(src step), ECX=count, EBX=ebx_in(cmap
 * base), ESI=esi_in, EDI=edi_in (no segments). */
/* shared LINEAR flat-colormap column for 0x37cec (opaque) and 0x38964 (transparent). See 0x38964 note. */
static void wmap_linear_flatcmap(uint32_t param_1, uint32_t ecx, uint32_t param_2,
                                 uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in, int transparent)
{
    uint32_t texbase = (uint32_t)G32(VA_g_render_source_base_ptr);
    uint8_t *tex = (uint8_t *)(uintptr_t)((esi_in & 0xffff) + texbase);
    uint8_t *di  = (uint8_t *)(uintptr_t)(edi_in + (uint32_t)G32(VA_g_render_target_buffer));
    uint32_t cmap = ebx_in + (uint32_t)G32(VA_g_active_world_remap_base);
    uint32_t cx  = ecx & 0xffff;
    uint32_t step_ebp = param_1 << 16;
    uint32_t step_esi = param_2;

    int32_t top_off, mid_off;
    if (!(G8(VA_g_span_shade_seed) & 0x80)) { top_off = 0; mid_off = 0; }
    else {
        uint8_t parity = (uint8_t)((uint32_t)G8(VA_g_span_draw_mode_flags + 0x2) + cx + (uint32_t)G32(VA_g_active_world_remap_base + 0x4));
        if (parity & 1) { top_off = -0x100; mid_off = 0; }       /* 0x38a44/0x37df4 (odd) */
        else            { top_off = 0; mid_off = -0x100; }       /* 0x38ac8/0x37e4c (even) */
    }
    #define LKF(texel, off) (*(const uint8_t *)(uintptr_t)( \
        ((cmap & 0xffffff00u) | (uint32_t)(texel)) + (uint32_t)(int32_t)(off) ))

    if (cx > 1) {
        uint32_t ebp = (uint32_t)G32(VA_g_span_eax_accum_init) << 16;
        int32_t  cnt = (int32_t)(cx >> 1);
        int      second = !(cx & 1);                 /* even count enters the 2nd unrolled half */
        if (second) di -= ROW;
        do {
            if (!second) { uint8_t t = *tex; uint32_t o = ebp; ebp += step_ebp; tex += step_esi + (ebp < o);
                           if (!transparent || t) di[0] = LKF(t, top_off); }
            second = 0;
            { uint8_t t = *tex; uint32_t o = ebp; ebp += step_ebp; tex += step_esi + (ebp < o);
              if (!transparent || t) di[ROW] = LKF(t, mid_off); }
            di += 2 * ROW;
        } while (--cnt > 0);
    }
    /* tail (always): texture wrap check, then final pixel at top_off */
    uint32_t off = (uint32_t)(uintptr_t)tex - (uint32_t)G32(VA_g_span_src_wrap_base) - texbase;
    if (off >= (uint32_t)G32(VA_g_span_src_row_width))
        tex = (uint8_t *)(uintptr_t)((uint32_t)G32(VA_g_span_src_wrap_reoffset) + texbase + (uint32_t)G32(VA_g_span_src_wrap_base));
    { uint8_t t = *tex; if (!transparent || t) di[0] = LKF(t, top_off); }
    #undef LKF
}

/* render_world_col_unshaded_opaque_37cec (0x37cec): the OPAQUE primary linear flat-colormap column (= 38964 without the
 * texel==0 skip). Table entry [0] = the common wall surface. */
void render_world_col_unshaded_opaque_37cec(uint32_t param_1, uint32_t ecx, uint32_t param_2,
                                    uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in)
{ wmap_linear_flatcmap(param_1, ecx, param_2, ebx_in, esi_in, edi_in, 0); }

void render_world_col_unshaded_masked_38964(uint32_t param_1, uint32_t ecx, uint32_t param_2,
                                    uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in)
{ wmap_linear_flatcmap(param_1, ecx, param_2, ebx_in, esi_in, edi_in, 1); }

/* render_world_col_solid_gradient_387f0 (0x387f0): SOLID SHADED GRADIENT column. Constant texel bl=byte[0x90a24]; the
 * shade row bh ramps down the column via a 16-bit sub-pixel accumulator (eax bits 16-31; al is scratch for
 * the pixel): per pixel = gs[(bh<<8)|bl]; acc += step([0x8a2e0]) -> carry into bh += dl(byte[0x8a2e2]).
 * acc init = word[0x8a2dc]. NO texture sample, NO transparency, NO wrap. Implemented as an 8x-unrolled
 * Duff's device (`sub ecx,8; jg`): faithfully, cx==0 writes 8 px (the unrolled-body quirk). ABI: ECX=count,
 * EBX=ebx_in (bh=shade seed), EDI=edi_in; GS=colormap. */
void render_world_col_solid_gradient_387f0(uint32_t ecx, uint32_t ebx_in, uint32_t edi_in, const uint8_t *gs_base)
{
    uint32_t cx = ecx & 0xffff;
    uint8_t *di = (uint8_t *)(uintptr_t)(edi_in + (uint32_t)G32(VA_g_render_target_buffer));
    uint8_t  bl = G8(VA_g_sprite_fill_index);
    uint8_t  bh = (uint8_t)(ebx_in >> 8);
    uint32_t acc  = (uint32_t)(uint16_t)G32(VA_g_span_accum_init);    /* eax[31:16] accumulator */
    uint32_t step = (uint32_t)(uint16_t)G32(VA_g_span_pixel_step);
    uint8_t  dl   = G8(VA_g_span_eax_step_lo);
    uint32_t n = (cx & 7) ? (cx & 7) : 8;               /* Duff entry: first partial pass */
    int32_t  ecx_loop = (int32_t)cx;
    for (;;) {
        for (uint32_t i = 0; i < n; i++) {
            di[0] = gs_base[((uint32_t)bh << 8) | bl];
            di += ROW;
            acc += step; uint8_t cy = (uint8_t)(acc >> 16); acc &= 0xffff;
            bh = (uint8_t)(bh + dl + cy);
        }
        ecx_loop -= 8;
        if (ecx_loop <= 0) break;
        n = 8;
    }
}

/* WRAPPED SHADED column shared by 0x38434 (opaque, 16-bit index init) and 0x384fc (transparent, 32-bit index
 * init). src base = wrap_base(0x8a338)+texbase(0x84980), index EBX wrapped each pixel by `&= mask`
 * (mask=G32(0x9097c), driver-patched). pixel = gs[(ah<<8)|texel] where ah = the shade row. A 3-accumulator
 * chain steps per pixel: ebp(frac)+=step_ebp -> carry into edx(shade)+=step_edx -> carry into ebx's LOW BYTE
 * (8-bit `adc bl`) += idx_step; then ah = dl(edx low byte). SMC steps read live: step_ebp=[0x8a2e0]<<16,
 * step_edx=(param_1<<16)|byte[0x8a2e2], idx_step=param_2&0xff. edx init=([0x8a344]<<16)|bh, ebp init=
 * [0x8a2dc]<<16, ah init=bh. 2x-unrolled (cnt=(count+1)>>1, even->first/odd->second); no tail. */
static void wmap_wrapped_shaded(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in,
                                const uint8_t *gs_base, int transparent, int idx16)
{
    uint32_t wrap_base = (uint32_t)G32(VA_g_span_src_wrap_base);
    uint8_t *src = (uint8_t *)(uintptr_t)(wrap_base + (uint32_t)G32(VA_g_render_source_base_ptr));
    uint8_t *di  = (uint8_t *)(uintptr_t)(edi_in + (uint32_t)G32(VA_g_render_target_buffer));
    uint32_t mask = (uint32_t)G32(VA_g_span_src_wrap_reoffset);
    uint32_t ebx = idx16 ? (uint32_t)(uint16_t)((uint16_t)esi_in - (uint16_t)wrap_base)
                         : ((esi_in & 0xffff) - wrap_base);
    uint32_t cx  = param_3 & 0xffff;
    uint8_t  bh  = (uint8_t)(ebx_in >> 8);

    uint32_t step_ebp = (uint32_t)G32(VA_g_span_pixel_step) << 16;
    uint32_t step_edx = (param_1 << 16) | (uint32_t)G8(VA_g_span_eax_step_lo);
    uint8_t  idx_step = (uint8_t)param_2;
    uint32_t ebp = (uint32_t)G32(VA_g_span_accum_init) << 16;
    uint32_t edx = ((uint32_t)G32(VA_g_span_eax_accum_init) << 16) | bh;
    uint8_t  ah  = bh;                                   /* sub eax,eax; mov ah,dl -> ah = bh */

    #define STEP_WSH do { \
        uint32_t o1 = ebp; ebp += step_ebp; unsigned cf1 = (ebp < o1); \
        uint64_t s2 = (uint64_t)edx + step_edx + cf1; edx = (uint32_t)s2; unsigned cf2 = (unsigned)(s2 >> 32); \
        uint8_t bl_ = (uint8_t)((uint8_t)ebx + idx_step + cf2); ebx = (ebx & 0xffffff00u) | bl_; \
        ah = (uint8_t)edx; \
    } while (0)
    #define WRPIX_WSH(dstp) do { \
        ebx &= mask; \
        uint8_t texel = src[ebx]; \
        if (!transparent || texel) *(dstp) = gs_base[((uint32_t)ah << 8) | texel]; \
    } while (0)

    uint32_t cnt = (cx + 1) >> 1;
    int second_first = (cx & 1);
    if (second_first) di -= ROW;
    do {
        if (!second_first) { WRPIX_WSH(&di[0]); STEP_WSH; }
        second_first = 0;
        WRPIX_WSH(&di[ROW]); STEP_WSH; di += 2 * ROW;
    } while (--cnt > 0);
    #undef STEP_WSH
    #undef WRPIX_WSH
}

/* SOLID vertical fill (0x38697): write color `al` down `cx` rows (stride 0x140) from edi_in+fb. 8x-unrolled
 * Duff's device (`mov [edi],al; add edi,edx` x8; sub ecx,8; jg). cx==0 writes 8 px (the Duff quirk). This is
 * also the dispatcher's EXIT D tail (al=[0x89f10]) and the 0x387e0 stub (al=[0x90a24]). */
static void wmap_solidfill(uint8_t al, uint32_t ecx, uint32_t edi_in)
{
    uint8_t *di = (uint8_t *)(uintptr_t)(edi_in + (uint32_t)G32(VA_g_render_target_buffer));
    uint32_t cx = ecx & 0xffff;
    uint32_t n = (cx & 7) ? (cx & 7) : 8;
    int32_t  ecx_loop = (int32_t)cx;
    for (;;) {
        for (uint32_t i = 0; i < n; i++) { di[0] = al; di += ROW; }
        ecx_loop -= 8;
        if (ecx_loop <= 0) break;
        n = 8;
    }
}

/* DITHER vertical fill (0x38706): checkerboard of c0/c1 down `cx` rows (stride 0x140), strict alternation by
 * pixel index. Same Duff structure (two interleaved tables for the two phases). */
static void wmap_dither(uint8_t c0, uint8_t c1, uint32_t ecx, uint32_t edi_in)
{
    uint8_t *di = (uint8_t *)(uintptr_t)(edi_in + (uint32_t)G32(VA_g_render_target_buffer));
    uint32_t cx = ecx & 0xffff;
    uint32_t n = (cx & 7) ? (cx & 7) : 8;
    int32_t  ecx_loop = (int32_t)cx;
    uint32_t j = 0;
    for (;;) {
        for (uint32_t i = 0; i < n; i++) { di[0] = (j & 1) ? c1 : c0; di += ROW; j++; }
        ecx_loop -= 8;
        if (ecx_loop <= 0) break;
        n = 8;
    }
}

/* render_world_col_solid_fill_38684 (0x38684): SOLID / DITHERED shaded fill (the "blended" variant). Computes the light
 * shade al = gs[(bh<<8)|byte[0x90a24]]; then byte[0x8a2dd]&0x80 selects: clear -> solid fill of al (0x38697);
 * set -> a 50% CHECKERBOARD dither of al (light) with ah = gs[((bh-1 if bh else 0)<<8)|byte[0x90a24]] (dark).
 * The dither's starting colour is (G8(0x8a34e)+G32(0x8a2b0))&1 -> ah (the per-column ecx terms cancel mod 2),
 * then strict per-row alternation. ABI: ECX=count, EBX=ebx_in (bh=shade), EDI=edi_in; GS=colormap (no es/fs). */
void render_world_col_solid_fill_38684(uint32_t ecx, uint32_t ebx_in, uint32_t edi_in, const uint8_t *gs_base)
{
    uint8_t bl = G8(VA_g_sprite_fill_index);
    uint8_t bh = (uint8_t)(ebx_in >> 8);
    uint8_t al = gs_base[((uint32_t)bh << 8) | bl];                 /* light shade (0x3868a) */
    if (!(G8(VA_g_span_shade_seed) & 0x80)) {
        wmap_solidfill(al, ecx, edi_in);                           /* 0x38697 */
    } else {
        uint8_t bh_dark = bh ? (uint8_t)(bh - 1) : 0;              /* or bh,bh; je; dec bh */
        uint8_t ah = gs_base[((uint32_t)bh_dark << 8) | bl];       /* dark shade */
        int start_ah = (int)(((uint32_t)G8(VA_g_span_draw_mode_flags + 0x2) + (uint32_t)G32(VA_g_active_world_remap_base + 0x4)) & 1);
        wmap_dither(start_ah ? ah : al, start_ah ? al : ah, ecx, edi_in);   /* 0x38706 */
    }
}

/* render_world_col_shaded_gs_wrapped_38434 (0x38434): SHADED OPAQUE wrapped column (16-bit index init). */
void render_world_col_shaded_gs_wrapped_38434(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                    uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in, const uint8_t *gs_base)
{ wmap_wrapped_shaded(param_1, param_2, param_3, ebx_in, esi_in, edi_in, gs_base, 0, 1); }

/* render_world_col_shaded_masked_gs_wrapped_384fc (0x384fc): SHADED TRANSPARENT wrapped column (32-bit index init; texel==0 skip). */
void render_world_col_shaded_masked_gs_wrapped_384fc(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                    uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in, const uint8_t *gs_base)
{ wmap_wrapped_shaded(param_1, param_2, param_3, ebx_in, esi_in, edi_in, gs_base, 1, 0); }

/* render_span_fill (0x38697): the EXIT-D solid vertical fill, entered standalone with al=EAX low byte
 * (the dispatcher's EXIT D loads al=[0x89f10]; the 0x387e0 stub loads al=[0x90a24]). Thin exported wrapper
 * over the static wmap_solidfill so the oracle can verify the standalone entry. */
void render_span_fill_38697(uint8_t al, uint32_t ecx, uint32_t edi_in)
{ wmap_solidfill(al, ecx, edi_in); }

/* render_world_col_fill_wrap (0x387e0): 12-byte stub `mov al,[0x90a24]; mov ah,1; jmp 0x38697` -> solid fill. */
void render_world_col_fill_wrap_387e0(uint32_t ecx, uint32_t edi_in)
{ wmap_solidfill(G8(VA_g_sprite_fill_index), ecx, edi_in); }

/* ===================== Batch 54 (scaled-sprite span inner loops, 0x39610 driver) =====================
 * The horizontal-span family dispatched by draw_scaled_sprite_spans (0x39610) via g_sprite_column_fn
 * (0x8a368). Unlike the vertical wall mapper (render_world_span_390ac, edi+=0x140), these fill
 * HORIZONTAL spans: ES:[EDI] with edi advancing +1/pixel (the 2px-unrolled variants do [edi],[edi+1],
 * edi+=2). Each inner loop ends with `jmp 0x39e52` (the shared driver loop tail); the oracle isolates a
 * single span by patching a `ret` over 0x39e52 around call_orig. Entry ABI (set by per-span setup 0x39bd2
 * -> dispatch 0x39e4c): ECX = span pixel count (cx; driver zero-extends), EDI = dest byte offset into the
 * screen segment ES (already includes the scanline base + start-x), ES = screen selector. */

/* render_sprite_span_fill_39fcd (0x39fcd): solid UNSHADED fill — the full-bright/flat shortcut.
 * al = ah = g_sprite_fill_index (0x90a24); `rep stosw` of (fill<<8|fill) across the span. The asm splits
 * an odd pixel off as a leading byte (edi even) or trailing byte (edi odd) to keep the word stores
 * aligned, but every byte written is `fill`, so the final span = `count` bytes of fill from edi.
 * ABI: ECX=count, EDI=dest offset, ES=screen. Non-SMC. */
void render_sprite_span_fill_39fcd(uint32_t ecx, uint32_t edi, uint8_t *es_base)
{
    uint32_t count = ecx & 0xffffu;                 /* cx = span pixel count */
    uint8_t  fill  = G8(VA_g_sprite_fill_index);                   /* g_sprite_fill_index */
    for (uint32_t i = 0; i < count; i++)
        es_base[edi + i] = fill;
}

/* SMC-patched inner-loop shift immediates. The driver (tex_width, 0x398aa-0x398d0) patches the textured inner
 * loops' `shl edx,imm` (the V accumulator/step build: (texX<<SHL)>>8) and `shr ebx,imm` (the texel-index
 * extraction) per-texture: SHL = 0x10 - bsr(w), SHR = 0x18 - bsr(w), where w = texture-width byte and bsr is on
 * (w|w<<8) (so bsr = 8 + bsr(w&0xff)); BOTH even+odd shr get the same SHR. The differential never snapshots
 * obj1 CODE, so the lift must mirror these. Defaults = the static file bytes (0x0c, 0x12, 0x08) so the oracle
 * (which tests the disassembled loop) still matches; the driver's tex_width recomputes them per-draw. */
static uint8_t g_tex_shl = 0x0c, g_tex_shr_even = 0x12, g_tex_shr_odd = 0x08;
/* SMC-patched clip-shade shift (0x39d20 `shr eax,imm`, default file byte 7): the driver's tex_width patches it
 * to 0xb - g_9098c (normal) / 0xa + g_9098c (degenerate). Stateful: set by textured draws, read by the clip-mode
 * shade ladder (incl. on untextured solid spans), so it persists across draws. We model it as TRUE SMC — the
 * tex_width setup WRITES the live byte 0x39d20 and the shade ladder READS it — rather than a private static,
 * because the static drifts from the original's code byte across the differential harness's restore boundary
 * (the code region isn't snapshot-restored): an untextured sprite reads the shift the last textured draw left,
 * and the external clip_base (0x399d8, patched by set_render_shade_level) keeps changing in between. */

/* render_sprite_span_solid_3a000 (0x3a000): solid SINGLE-SHADE fill. The pixel colour is looked up ONCE —
 * bl=g_sprite_fill_index, bh=g_sprite_span_shade, al = gs:[bx] = gs[(shade<<8)|fill] — replicated into all
 * four bytes of EAX and blasted across the span with alignment-aware `rep stosd`/`stosb` (the small-count
 * <=3 path is a plain `rep stosb`). Every byte written is that one colour, so the final span = `count`
 * bytes of colour from edi. ABI: ECX=count, EDI=dest offset, ES=screen, GS=colormap. Non-SMC. */
void render_sprite_span_solid_3a000(uint32_t ecx, uint32_t edi,
                                           const uint8_t *gs_base, uint8_t *es_base)
{
    uint8_t  fill  = G8(VA_g_sprite_fill_index);                   /* g_sprite_fill_index (bl) */
    uint8_t  shade = G8(VA_g_sprite_span_shade);                   /* g_sprite_span_shade  (bh) */
    uint8_t  color = gs_base[((uint32_t)shade << 8) | fill];  /* gs:[(shade<<8)|fill], once */
    uint32_t count = ecx;                           /* full ECX (driver zero-extends cx) */
    for (uint32_t i = 0; i < count; i++)
        es_base[edi + i] = color;
}

/* render_sprite_span_gradient_3a0b1 (0x3a0b1): solid run with a per-pixel GRADIENT shade. The texel is
 * the constant g_sprite_fill_index (bl); the colormap ROW (bh) ramps down the span. Each pixel writes
 * gs[(bh<<8)|fill]; then a 16.16 accumulator advances `ebp += edx` (step = g_sprite_src_coord_step<<16)
 * and `adc bh, ah` adds the shade step (g_8a37a) PLUS the accumulator's carry — so bh climbs by ah each
 * pixel with an extra +1 whenever the sub-pixel accumulator wraps. Opaque (no transparency). 2x-unrolled
 * in asm (a duff-style odd-pixel entry); semantically exactly `count` pixels. ABI: ECX=count, EDI=dest,
 * ES=screen, GS=colormap; ebp init=g_8a374, step=g_8a378, bh init=g_8a376, ah=g_8a37a. Non-SMC. */
void render_sprite_span_gradient_3a0b1(uint32_t ecx, uint32_t edi,
                                              const uint8_t *gs_base, uint8_t *es_base)
{
    uint32_t count = ecx & 0xffffu;                 /* run length (cx) */
    uint32_t fill  = G8(VA_g_sprite_fill_index);                   /* bl: constant texel (ebx low byte) */
    uint8_t  bh    = G8(VA_g_sprite_src_coord + 0x2);                   /* initial colormap row */
    uint8_t  ah    = G8(VA_g_sprite_src_coord_step + 0x2);                   /* shade step per pixel */
    uint32_t ebp   = (uint32_t)G32(VA_g_sprite_src_coord) << 16;  /* sub-pixel accumulator (16.16) */
    uint32_t step  = (uint32_t)G32(VA_g_sprite_src_coord_step) << 16;  /* accumulator step */
    for (uint32_t i = 0; i < count; i++) {
        es_base[edi + i] = gs_base[((uint32_t)bh << 8) | fill];
        uint32_t old = ebp; ebp += step;            /* add ebp,edx -> CF on unsigned overflow */
        bh = (uint8_t)(bh + ah + (ebp < old));      /* adc bh,ah (8-bit, wraps) */
    }
}

/* render_sprite_span_tex_3a100 (0x3a100): the first TEXTURED sprite span — transparency + translucency,
 * a constant shade, and a FLAT-remap colormap (not gs). Self-loads ES = g_transparency_blend_selector
 * (0x90be2) and patches the texture mask + step immediates from the g_sprite_tex_step_* globals (we read
 * them as locals). Two fixed-point accumulators feed the source index per pixel:
 *     ebx = (((ebp & 0xffff00ff) | (edx & 0xff00)) >> 8) & MASK ;  texel = texbase[ebx]
 *     ebp += ebp_step ;  edx += edx_step + carry(ebp)            (adc chain)
 * Per texel: 0 -> transparent (skip); &0x80 -> translucent (blend es[(dest<<8)|color]); else opaque. The
 * colour is the low-byte-replace flat remap remap[(base+(shade<<8) & ~0xff) | texel], base=g_8a2ac. 2x
 * unrolled (di[0]/di[1], edi+=2) with the odd-pixel entry (pre-dec di, enter at the second half). The
 * dest is DS:[edi] with edi += g_render_target_buffer; ES is only the blend table. ABI: ECX=count,
 * EDI=dest offset, ES(param es_base)=blend table. The texture index high word is 0 for MASK<=0xffff
 * (any real texture), so the blend lookup stays the 256x256 table es[(dest<<8)|color]. */
void render_sprite_span_tex_3a100(uint32_t ecx, uint32_t edi_in, const uint8_t *es_base)
{
    uint8_t *texbase  = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_render_source_base_ptr);     /* g_active_das_frame_ptr */
    uint8_t *di       = (uint8_t *)(uintptr_t)(edi_in + (uint32_t)G32(VA_g_render_target_buffer));
    uint32_t remap    = (uint32_t)G32(VA_g_active_world_remap_base) + ((uint32_t)G8(VA_g_sprite_span_shade) << 8); /* flat-remap base + shade row */
    /* SMC write-backs (0x3a117/0x3a167): pack (texU<<shl)&0xff / (texV<<shl)&0xff into the HIGH BYTES of
     * texU2 (0x8a390) / texV2 (0x8a398) -- the accumulator seed reads them below. NOT zero (the zero only
     * matched when texU/texV==0, i.e. floor/ceiling; nonzero on moving textured surfaces). */
    G8(VA_g_sprite_tex_step_u2 + 0x3) = (uint8_t)((uint32_t)G32(VA_g_sprite_tex_step_u) << g_tex_shl);
    G8(VA_g_sprite_tex_step_v2 + 0x3) = (uint8_t)((uint32_t)G32(VA_g_sprite_tex_step_v) << g_tex_shl);
    uint32_t mask     = (uint32_t)G32(VA_g_sprite_tex_step_frac);                          /* texture wrap mask */
    uint32_t ebp      = (uint32_t)G32(VA_g_sprite_tex_step_v2);                          /* accumulator init (u2) */
    uint32_t edx      = ((uint32_t)G32(VA_g_sprite_tex_step_v) << g_tex_shl) >> 8;             /* accumulator init (v) */
    uint32_t ebp_step = (uint32_t)G32(VA_g_sprite_tex_step_u2);
    uint32_t edx_step = ((uint32_t)G32(VA_g_sprite_tex_step_u) << g_tex_shl) >> 8;
    uint32_t cx       = ecx & 0xffffu;

    /* one pixel: index -> texel -> advance accumulators -> transparency-routed output. */
    #define PX3A100(dstp) do { \
        uint32_t ebx = (((ebp & 0xffff00ffu) | (edx & 0xff00u)) >> g_tex_shr_odd) & mask; \
        uint8_t texel = texbase[ebx]; \
        uint32_t o = ebp; ebp += ebp_step; edx += edx_step + (ebp < o); \
        if (texel) { \
            uint8_t color = *(const uint8_t *)(uintptr_t)((remap & 0xffffff00u) | texel); \
            if (texel & 0x80u) *(dstp) = es_base[((uint32_t)(*(dstp)) << 8) | color]; \
            else               *(dstp) = color; \
        } \
    } while (0)

    if (cx == 0) return;                            /* driver guarantees >=1 */
    int32_t cnt = (int32_t)(cx >> 1);
    int skip_first = (int)(cx & 1);                 /* odd -> enter at second half */
    if (skip_first) { cnt += 1; di -= 1; }
    do {
        if (!skip_first) PX3A100(&di[0]);
        skip_first = 0;
        PX3A100(&di[1]);
        di += 2;
    } while (--cnt > 0);
    #undef PX3A100
}

/* render_sprite_span_tex_3a220 (0x3a220): RAW-texel textured span (no remap, no transparency — every
 * texel is written straight to the framebuffer) with a forward/reverse split on g_sprite_span_flip
 * (0x8a3ac, signed byte). Companion to render_span_texmap_3a368. Both sub-loops are 2x-unrolled with the
 * odd-pixel entry.
 *   FORWARD (flip >= 0): two fixed-point accumulators eax(U)/edx(V); the two halves extract the source
 *     index ASYMMETRICALLY -- first half (eax>>18)&MASK, second half ((eax>>8 with dh injected))&MASK --
 *     then texel = texbase[idx]; eax += step_u; edx += step_v + carry.  (MASK = g_8a360.)
 *   REVERSED (flip < 0): a packed-byte affine walker. The U/V steps are rol-16 packed into eax/esi and a
 *     low sub-byte dl/dh, and the source index ebx=(ah<<8)|bl is advanced by the carry chain
 *     `add dl,dh` -> `adc eax,esi` -> `adc bl,cl`; texel = fs:[ebx] (FS = texture selector). ah (=eax
 *     byte1) becomes the index high byte each pixel.
 * ABI: ECX=count, EDI=dest offset (di += g_render_target_buffer), forward texbase=g_84980, reversed
 * FS(param fs_base)=texture. Both write raw texels DS:[edi]. SMC (forward only patches its step imms). */
void render_sprite_span_tex_3a220(uint32_t ecx, uint32_t edi_in, const uint8_t *fs_base)
{
    uint8_t *di = (uint8_t *)(uintptr_t)(edi_in + (uint32_t)G32(VA_g_render_target_buffer));
    uint32_t cx = ecx & 0xffffu;
    if (cx == 0) return;

    if ((int8_t)G8(VA_g_sprite_span_flip) >= 0) {
        /* ---- forward (raw texel, asymmetric index shifts) ---- */
        /* SMC write-backs (0x3a23c/0x3a26c): pack (texU<<shl)&0xff / (texV<<shl)&0xff into the HIGH BYTES
         * of texU2 (0x8a390) / texV2 (0x8a398) -- the step/accumulator seed reads them below. NOT zero. */
        G8(VA_g_sprite_tex_step_u2 + 0x3) = (uint8_t)((uint32_t)G32(VA_g_sprite_tex_step_u) << g_tex_shl);
        G8(VA_g_sprite_tex_step_v2 + 0x3) = (uint8_t)((uint32_t)G32(VA_g_sprite_tex_step_v) << g_tex_shl);
        uint8_t *texbase  = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_render_source_base_ptr);
        uint32_t mask     = (uint32_t)G32(VA_g_sprite_tex_step_frac);
        uint32_t eax      = (uint32_t)G32(VA_g_sprite_tex_step_v2);              /* U accumulator init */
        uint32_t edx      = ((uint32_t)G32(VA_g_sprite_tex_step_v) << g_tex_shl) >> 8; /* V accumulator init */
        uint32_t step_u   = (uint32_t)G32(VA_g_sprite_tex_step_u2);
        uint32_t step_v   = ((uint32_t)G32(VA_g_sprite_tex_step_u) << g_tex_shl) >> 8;
        int32_t  cnt = (int32_t)(cx >> 1);
        int skip_first = (int)(cx & 1);
        if (skip_first) { cnt += 1; di += 1; }                  /* odd: inc edi, enter second half */
        do {
            if (!skip_first) {                                  /* first half: (eax>>18)&mask -> [edi] */
                uint32_t ebx = (((eax & 0xffff00ffu) | (edx & 0xff00u)) >> g_tex_shr_even) & mask;
                uint8_t texel = texbase[ebx];
                uint32_t o = eax; eax += step_u; edx += step_v + (eax < o);
                di[0] = texel;
                di += 2;
            }
            skip_first = 0;
            {                                                   /* second half: (eax>>8)&mask -> [edi-1] */
                uint32_t ebx = (((eax & 0xffff00ffu) | (edx & 0xff00u)) >> g_tex_shr_odd) & mask;
                uint8_t texel = texbase[ebx];
                uint32_t o = eax; eax += step_u; edx += step_v + (eax < o);
                di[-1] = texel;
            }
        } while (--cnt > 0);
    } else {
        /* ---- reversed (packed-byte affine walker, fs: texture) ---- */
        uint32_t U  = (uint32_t)G32(VA_g_sprite_tex_step_u), U2 = (uint32_t)G32(VA_g_sprite_tex_step_u2);
        uint32_t V  = (uint32_t)G32(VA_g_sprite_tex_step_v), V2 = (uint32_t)G32(VA_g_sprite_tex_step_v2);
        #define ROL16(x) (((x) << 16) | ((x) >> 16))
        uint32_t eax = ROL16(V);
        eax = (eax & 0xffff0000u) | ((V2 >> 8) & 0xffffu);      /* mov ax,cx (cx=V2>>8) */
        uint8_t  dl  = (uint8_t)V2;                             /* low sub-byte (V) */
        uint32_t esi = ROL16(U);
        esi = (esi & 0xffff0000u) | ((U2 >> 8) & 0xffffu);      /* mov si,cx (cx=U2>>8) */
        uint8_t  dh  = (uint8_t)U2;                             /* low sub-byte (U step) */
        uint8_t  cl  = (uint8_t)ROL16(U);                       /* mov ecx,ebp(=rol U); cl */
        uint8_t  bl  = (uint8_t)ROL16(V);                       /* ebx=rol(V); bl */
        uint8_t  bh  = (uint8_t)(eax >> 8);                     /* mov bh,ah */
        uint32_t ebx = ((uint32_t)bh << 8) | bl;               /* and ebx,0xffff */
        int32_t  cnt = (int32_t)(cx >> 1);
        int skip_first = (int)(cx & 1);
        if (skip_first) { cnt += 1; di -= 1; }                  /* odd: dec edi, enter second half */
        /* one pixel: fetch fs:[ebx], write, advance the packed carry chain dl->eax->bl, rebuild ebx. */
        #define PX3A220R(dstp) do { \
            uint16_t td = (uint16_t)dl + dh; dl = (uint8_t)td; int cf = td >> 8; \
            *(dstp) = fs_base[ebx & 0xffffu]; \
            uint64_t se = (uint64_t)eax + esi + (uint32_t)cf; eax = (uint32_t)se; cf = (int)(se >> 32); \
            bl = (uint8_t)((uint16_t)bl + cl + cf); \
            bh = (uint8_t)(eax >> 8); \
            ebx = ((uint32_t)bh << 8) | bl; \
        } while (0)
        do {                                                    /* di[0]/di[1], edi+=2 (3a100-style) */
            if (!skip_first) PX3A220R(&di[0]);
            skip_first = 0;
            PX3A220R(&di[1]);
            di += 2;
        } while (--cnt > 0);
        #undef PX3A220R
        #undef ROL16
    }
}

/* render_span_texmap_3a368 (0x3a368): the previously-known SMC texmapper -- the SHADED sibling of
 * render_sprite_span_tex_3a220. Same fwd/rev flip split and affine addressing, but each fetched texel is
 * run through the flat-remap colormap remap[(base+(shade<<8) & ~0xff) | texel] before the write. Opaque
 * (no transparency). 2x-unrolled with the odd-pixel entry.
 *   FORWARD (flip>=0): accumulators ebp(U)/edx(V), asymmetric index shifts (>>18 / >>8) & MASK, texel =
 *     texbase[idx] -> remap -> [edi].
 *   REVERSED (flip<0): packed-byte walker; the accumulator is ECX (eax holds the remap base), index
 *     ebx=(ch<<8)|bl advanced by `add dl,dh`->`adc ecx,esi`->`adc bl,imm8` (imm8 = rol(U,16)&0xff, SMC),
 *     texel = fs:[ebx] -> remap -> bh -> [edi].
 * ABI: ECX=count, EDI=dest offset (di += g_render_target_buffer), forward texbase=g_84980, reversed
 * FS(param fs_base)=texture; remap base=g_8a2ac + (shade<<8). */
void render_span_texmap_3a368(uint32_t ecx, uint32_t edi_in, const uint8_t *fs_base)
{
    uint8_t *di    = (uint8_t *)(uintptr_t)(edi_in + (uint32_t)G32(VA_g_render_target_buffer));
    uint32_t remap = (uint32_t)G32(VA_g_active_world_remap_base) + ((uint32_t)G8(VA_g_sprite_span_shade) << 8);
    uint32_t cx    = ecx & 0xffffu;
    if (cx == 0) return;
    #define REMAP3A368(texel) (*(const uint8_t *)(uintptr_t)((remap & 0xffffff00u) | (uint32_t)(texel)))

    if ((int8_t)G8(VA_g_sprite_span_flip) >= 0) {
        /* ---- forward (shaded, asymmetric index shifts) ---- */
        /* SMC write-backs (0x3a384/0x3a3b4): pack (texU<<shl)&0xff / (texV<<shl)&0xff into the HIGH BYTES
         * of texU2 (0x8a390) / texV2 (0x8a398) before seeding step/accumulator below. NOT zero. */
        G8(VA_g_sprite_tex_step_u2 + 0x3) = (uint8_t)((uint32_t)G32(VA_g_sprite_tex_step_u) << g_tex_shl);
        G8(VA_g_sprite_tex_step_v2 + 0x3) = (uint8_t)((uint32_t)G32(VA_g_sprite_tex_step_v) << g_tex_shl);
        uint8_t *texbase  = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_render_source_base_ptr);
        uint32_t mask     = (uint32_t)G32(VA_g_sprite_tex_step_frac);
        uint32_t ebp      = (uint32_t)G32(VA_g_sprite_tex_step_v2);              /* U accumulator init */
        uint32_t edx      = ((uint32_t)G32(VA_g_sprite_tex_step_v) << g_tex_shl) >> 8; /* V accumulator init */
        uint32_t step_u   = (uint32_t)G32(VA_g_sprite_tex_step_u2);
        uint32_t step_v   = ((uint32_t)G32(VA_g_sprite_tex_step_u) << g_tex_shl) >> 8;
        int32_t  cnt = (int32_t)(cx >> 1);
        int skip_first = (int)(cx & 1);
        if (skip_first) { cnt += 1; di += 1; }
        do {
            if (!skip_first) {                                  /* first half: (ebp>>18)&mask -> [edi] */
                uint32_t ebx = (((ebp & 0xffff00ffu) | (edx & 0xff00u)) >> g_tex_shr_even) & mask;
                uint8_t texel = texbase[ebx];
                uint32_t o = ebp; ebp += step_u; edx += step_v + (ebp < o);
                di[0] = REMAP3A368(texel);
                di += 2;
            }
            skip_first = 0;
            {                                                   /* second half: (ebp>>shr)&mask -> [edi-1] */
                uint32_t ebx = (((ebp & 0xffff00ffu) | (edx & 0xff00u)) >> g_tex_shr_odd) & mask;
                uint8_t texel = texbase[ebx];
                uint32_t o = ebp; ebp += step_u; edx += step_v + (ebp < o);
                di[-1] = REMAP3A368(texel);
            }
        } while (--cnt > 0);
    } else {
        /* ---- reversed (shaded packed-byte walker, fs: texture) ---- */
        uint32_t U  = (uint32_t)G32(VA_g_sprite_tex_step_u), U2 = (uint32_t)G32(VA_g_sprite_tex_step_u2);
        uint32_t V  = (uint32_t)G32(VA_g_sprite_tex_step_v), V2 = (uint32_t)G32(VA_g_sprite_tex_step_v2);
        #define ROL16(x) (((x) << 16) | ((x) >> 16))
        uint32_t ecx_acc = (ROL16(V) & 0xffff0000u) | ((V2 >> 8) & 0xffffu);
        uint8_t  dl  = (uint8_t)V2;
        uint32_t esi = (ROL16(U) & 0xffff0000u) | ((U2 >> 8) & 0xffffu);
        uint8_t  dh  = (uint8_t)U2;
        uint8_t  bl_imm = (uint8_t)ROL16(U);                    /* adc bl,imm8 (SMC) */
        uint8_t  bl  = (uint8_t)ROL16(V);
        uint8_t  bh  = (uint8_t)(ecx_acc >> 8);                 /* mov bh,ch */
        uint32_t ebx = ((uint32_t)bh << 8) | bl;
        int32_t  cnt = (int32_t)(cx >> 1);
        int skip_first = (int)(cx & 1);
        if (skip_first) { cnt += 1; di -= 1; }
        /* one pixel: fetch fs:[ebx], remap, write; advance dl->ecx->bl carry chain; rebuild ebx. */
        #define PX3A368R(dstp) do { \
            uint16_t td = (uint16_t)dl + dh; dl = (uint8_t)td; int cf = td >> 8; \
            uint8_t texel = fs_base[ebx & 0xffffu]; \
            uint64_t se = (uint64_t)ecx_acc + esi + (uint32_t)cf; ecx_acc = (uint32_t)se; cf = (int)(se >> 32); \
            uint8_t color = REMAP3A368(texel); \
            bl = (uint8_t)((uint16_t)bl + bl_imm + cf); \
            *(dstp) = color; \
            bh = (uint8_t)(ecx_acc >> 8); \
            ebx = ((uint32_t)bh << 8) | bl; \
        } while (0)
        do {
            if (!skip_first) PX3A368R(&di[0]);
            skip_first = 0;
            PX3A368R(&di[1]);
            di += 2;
        } while (--cnt > 0);
        #undef PX3A368R
        #undef ROL16
    }
    #undef REMAP3A368
}

/* render_sprite_span_tex_shaded_3a4f8 (0x3a4f8): textured + per-pixel GRADIENT shade via a GS colormap
 * (gs[(shade<<8)|texel]), fwd/rev flip split. The richest-addressed sprite span. 2x-unrolled, odd-entry.
 *   FORWARD (flip>=0): U accumulator ebp (init g_8a398, step g_8a390); edx is a PACKED accumulator whose
 *     low 16 = V-coord frac ((g_8a394&0xfff)<<4, step (g_8a38c&0xfff)<<4) and high 16 = shade frac
 *     (g_8a374<<16, step g_8a378<<16). Index = (((ebp&0xffff00ff)|(edx&0xff00))>>{18,8})&MASK. The shade
 *     row `ah` (init g_8a376) ramps via `adc ah, g_8a37a` whose carry is the FULL-32-bit overflow of the
 *     edx accumulator (so the shade-frac high word carries into the integer shade). Colour = gs[(ah<<8)|
 *     texbase[idx]].
 *   REVERSED (flip<0): a 4-register packed walker. esi=shade-frac, ecx=shade-int (ah=cl), edx=V-coord,
 *     bl=index-low; advanced by the carry chain add esi -> adc ecx -> adc edx -> adc bl. Index ebx=
 *     (dh<<8)|bl (dh=edx byte1). texel=fs:[ebx], colour=gs[(ah<<8)|texel]. The accumulator init high
 *     bytes carry x86 register-reuse leftovers (see below), modelled exactly.
 * ABI: ECX=count, EDI=dest (di += g_render_target_buffer), GS(param gs_base)=colormap, forward texbase=
 * g_84980, reversed FS(param fs_base)=texture. */
void render_sprite_span_tex_shaded_3a4f8(uint32_t ecx, uint32_t edi_in,
                                                const uint8_t *gs_base, const uint8_t *fs_base)
{
    uint8_t *di = (uint8_t *)(uintptr_t)(edi_in + (uint32_t)G32(VA_g_render_target_buffer));
    uint32_t cx = ecx & 0xffffu;
    if (cx == 0) return;
    #define ROL16(x) (((x) << 16) | ((x) >> 16))

    if ((int8_t)G8(VA_g_sprite_span_flip) >= 0) {
        /* ---- forward (gs gradient shade, packed edx accumulator) ---- */
        /* SMC write-backs (0x3a51c/0x3a554): pack (texU<<shl)&0xff / (texV<<shl)&0xff into the
         * HIGH BYTES of texU2 (0x8a390) / texV2 (0x8a398) = byte3 of the ebp accumulator seed read below.
         * NOT zero (the zero only matched when texU/texV==0; nonzero on moving textured walls/sprites). */
        G8(VA_g_sprite_tex_step_u2 + 0x3) = (uint8_t)((uint32_t)G32(VA_g_sprite_tex_step_u) << g_tex_shl);
        G8(VA_g_sprite_tex_step_v2 + 0x3) = (uint8_t)((uint32_t)G32(VA_g_sprite_tex_step_v) << g_tex_shl);
        uint8_t *texbase = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_render_source_base_ptr);
        uint32_t mask     = (uint32_t)G32(VA_g_sprite_tex_step_frac);
        uint32_t ebp      = (uint32_t)G32(VA_g_sprite_tex_step_v2);
        uint32_t edx      = ((((uint32_t)G32(VA_g_sprite_tex_step_v) << g_tex_shl) >> 8) & 0xffffu) | ((uint32_t)G32(VA_g_sprite_src_coord) << 16);
        uint32_t step_ebp = (uint32_t)G32(VA_g_sprite_tex_step_u2);
        uint32_t step_edx = ((((uint32_t)G32(VA_g_sprite_tex_step_u) << g_tex_shl) >> 8) & 0xffffu) | ((uint32_t)G32(VA_g_sprite_src_coord_step) << 16);
        uint8_t  ah       = G8(VA_g_sprite_src_coord + 0x2);            /* shade row init */
        uint8_t  ah_step  = G8(VA_g_sprite_src_coord_step + 0x2);            /* adc ah,imm8 */
        int32_t  cnt = (int32_t)(cx >> 1);
        int skip_first = (int)(cx & 1);
        if (skip_first) { cnt += 1; di += 1; }
        #define PX3A4F8F(dstp, sh) do { \
            uint32_t ebx = (((ebp & 0xffff00ffu) | (edx & 0xff00u)) >> (sh)) & mask; \
            uint8_t texel = texbase[ebx]; \
            uint32_t o = ebp; ebp += step_ebp; int cf1 = (ebp < o); \
            uint64_t se = (uint64_t)edx + step_edx + (uint32_t)cf1; edx = (uint32_t)se; int cf2 = (int)(se >> 32); \
            *(dstp) = gs_base[((uint32_t)ah << 8) | texel]; \
            ah = (uint8_t)(ah + ah_step + cf2); \
        } while (0)
        do {
            if (!skip_first) { PX3A4F8F(&di[0], g_tex_shr_even); di += 2; }
            skip_first = 0;
            PX3A4F8F(&di[-1], g_tex_shr_odd);
        } while (--cnt > 0);
        #undef PX3A4F8F
    } else {
        /* ---- reversed (4-register packed walker) ---- */
        uint32_t U  = (uint32_t)G32(VA_g_sprite_tex_step_u), U2 = (uint32_t)G32(VA_g_sprite_tex_step_u2);
        uint32_t V  = (uint32_t)G32(VA_g_sprite_tex_step_v), V2 = (uint32_t)G32(VA_g_sprite_tex_step_v2);
        uint32_t SC = (uint32_t)G32(VA_g_sprite_src_coord), SS = (uint32_t)G32(VA_g_sprite_src_coord_step);   /* shade coord / step */
        /* accumulators */
        uint32_t esi = ROL16(SC);
        uint32_t ecx_acc = ((V2 & 0xffu) << 24) | ((U2 & 0xffu) << 16) | ((SC >> 16) & 0xffffu);
        uint32_t edx = (ROL16(V) & 0xffff0000u) | ((V2 >> 8) & 0xffffu);
        uint8_t  bl  = (uint8_t)ROL16(V);
        uint8_t  bh  = (uint8_t)(edx >> 8);
        uint32_t ebx = ((uint32_t)bh << 8) | bl;
        /* steps */
        uint32_t step_esi = ROL16(SS);
        uint32_t step_ecx = ((U2 & 0xffu) << 24) | ((SS >> 16) & 0xffffu);
        uint32_t step_edx = (ROL16(U) & 0xffff0000u) | ((U2 >> 8) & 0xffffu);
        uint8_t  bl_imm = (uint8_t)ROL16(U);
        uint8_t  ah;
        int32_t  cnt = (int32_t)(cx >> 1);
        int skip_first = (int)(cx & 1);
        if (skip_first) { cnt += 1; di -= 1; }
        /* one pixel: ah=cl, fetch fs:[ebx], colour gs[(ah<<8)|texel]; advance esi->ecx->edx->bl chain. */
        #define PX3A4F8R(dstp) do { \
            ah = (uint8_t)ecx_acc; \
            uint8_t texel = fs_base[ebx & 0xffffu]; \
            *(dstp) = gs_base[((uint32_t)ah << 8) | texel]; \
            uint32_t o1 = esi; esi += step_esi; int cf = (esi < o1); \
            uint64_t s2 = (uint64_t)ecx_acc + step_ecx + (uint32_t)cf; ecx_acc = (uint32_t)s2; cf = (int)(s2 >> 32); \
            uint64_t s3 = (uint64_t)edx + step_edx + (uint32_t)cf; edx = (uint32_t)s3; cf = (int)(s3 >> 32); \
            bh = (uint8_t)(edx >> 8); \
            bl = (uint8_t)((uint16_t)bl + bl_imm + cf); \
            ebx = ((uint32_t)bh << 8) | bl; \
        } while (0)
        do {
            if (!skip_first) PX3A4F8R(&di[0]);
            skip_first = 0;
            PX3A4F8R(&di[1]);
            di += 2;
        } while (--cnt > 0);
        #undef PX3A4F8R
    }
    #undef ROL16
}

/* render_sprite_span_tex_blend_3a700 (0x3a700): the RICHEST sprite span -- textured + per-pixel gs
 * gradient shade + TRANSPARENCY + translucency. Forward-only (no flip split), 2x-unrolled, odd-entry.
 * Setup is identical to render_sprite_span_tex_shaded_3a4f8's forward path: U accumulator ebp, the packed
 * edx (low16 = V-coord frac, high16 = shade frac), shade row `ah` ramped via `adc ah,g_8a37a` carrying
 * the edx-overflow. Per pixel the accumulators (ebp/edx) AND the shade ah are advanced BEFORE the output
 * (so even skipped/transparent pixels still ramp), then:
 *     texel 0           -> transparent (skip)
 *     texel & 0x80      -> translucent: colour = gs[(ah<<8)|texel]; out = es[(dest<<8)|colour]
 *     else              -> opaque:      out = gs[(ah<<8)|texel]
 * Index = (((ebp&0xffff00ff)|(edx&0xff00))>>{18,8})&MASK. ABI: ECX=count, EDI=dest (di += g_render_
 * target_buffer), GS(param gs_base)=colormap, ES(param es_base)=blend table (self-loaded from 0x90be2),
 * texbase=g_84980. SMC patches the step/mask/shade-step immediates in. */
void render_sprite_span_tex_blend_3a700(uint32_t ecx, uint32_t edi_in,
                                               const uint8_t *gs_base, const uint8_t *es_base)
{
    uint8_t *texbase = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_render_source_base_ptr);
    uint8_t *di       = (uint8_t *)(uintptr_t)(edi_in + (uint32_t)G32(VA_g_render_target_buffer));
    /* SMC write-backs (0x3a71f/0x3a757): pack (texU<<shl)&0xff / (texV<<shl)&0xff into texU2/texV2 high bytes. */
    G8(VA_g_sprite_tex_step_u2 + 0x3) = (uint8_t)((uint32_t)G32(VA_g_sprite_tex_step_u) << g_tex_shl);
    G8(VA_g_sprite_tex_step_v2 + 0x3) = (uint8_t)((uint32_t)G32(VA_g_sprite_tex_step_v) << g_tex_shl);
    uint32_t mask     = (uint32_t)G32(VA_g_sprite_tex_step_frac);
    uint32_t ebp      = (uint32_t)G32(VA_g_sprite_tex_step_v2);
    uint32_t edx      = ((((uint32_t)G32(VA_g_sprite_tex_step_v) << g_tex_shl) >> 8) & 0xffffu) | ((uint32_t)G32(VA_g_sprite_src_coord) << 16);
    uint32_t step_ebp = (uint32_t)G32(VA_g_sprite_tex_step_u2);
    uint32_t step_edx = ((((uint32_t)G32(VA_g_sprite_tex_step_u) << g_tex_shl) >> 8) & 0xffffu) | ((uint32_t)G32(VA_g_sprite_src_coord_step) << 16);
    uint8_t  ah       = G8(VA_g_sprite_src_coord + 0x2);
    uint8_t  ah_step  = G8(VA_g_sprite_src_coord_step + 0x2);
    uint32_t cx       = ecx & 0xffffu;
    if (cx == 0) return;
    /* one pixel: index -> texel; advance ebp/edx/ah (always); then transparency-routed gs/es output. */
    #define PX3A700(dstp, sh) do { \
        uint32_t ebx = (((ebp & 0xffff00ffu) | (edx & 0xff00u)) >> (sh)) & mask; \
        uint8_t texel = texbase[ebx]; \
        uint32_t o = ebp; ebp += step_ebp; int cf1 = (ebp < o); \
        uint64_t se = (uint64_t)edx + step_edx + (uint32_t)cf1; edx = (uint32_t)se; int cf2 = (int)(se >> 32); \
        ah = (uint8_t)(ah + ah_step + cf2);          /* shade advances before the lookup */ \
        if (texel) { \
            uint8_t color = gs_base[((uint32_t)ah << 8) | texel]; \
            if (texel & 0x80u) *(dstp) = es_base[((uint32_t)(*(dstp)) << 8) | color]; \
            else               *(dstp) = color; \
        } \
    } while (0)
    int32_t cnt = (int32_t)(cx >> 1);
    int skip_first = (int)(cx & 1);
    if (skip_first) { cnt += 1; di -= 1; }
    do {
        if (!skip_first) PX3A700(&di[0], g_tex_shr_even);
        skip_first = 0;
        PX3A700(&di[1], g_tex_shr_odd);
        di += 2;
    } while (--cnt > 0);
    #undef PX3A700
}

/* ===================== Batch 55 (sprite driver per-span setup math, 0x39bd2) =====================
 * sprite_span_setup_39bd2 — the computational CORE of the driver draw_scaled_sprite_spans
 * (0x39610): the per-span loop body that, for ONE span, extracts the span record, perspective-divides,
 * and builds the affine texture/colour steppers the inner loops consume. This is the part that is pure
 * computation (reads the span record + render-state globals, writes the stepper globals) and is provable
 * via write-set diff. The surrounding orchestration (mode ladder, run-list walk built by the bridged
 * edge-walker rasterize_floorceil_polygon, and the jmp [g_sprite_column_fn] dispatch to the verified
 * inner loops) is integration/trace-replay territory and is NOT modelled here.
 *
 * ABI at 0x39bd2 (entry of the per-span body): ESI -> per-scanline dest-offset table (descending),
 * EBX -> the 0x18-byte span record (+0 start-x, +0xc count, +2/+0xe source coords, +3 shade flag).
 * It dispatches via `jmp [g_sprite_column_fn]` (0x39e4c); the oracle patches that to `add esp,0x10; ret`
 * so the body returns after computing the steppers. This lift reproduces the GLOBAL writes only (the
 * register outputs EDI/EAX that flow into the inner loop are computed but not part of the write-set). */
span_dispatch_t sprite_span_setup_39bd2(uint32_t *p_esi, uint32_t *p_ebx, uint32_t persp_dividend,
                                               int32_t tex_divU, int32_t tex_divU2)
{
    uint32_t esi = *p_esi, ebx = *p_ebx;
    span_dispatch_t out = { 0, 0, 0, 0 };
    /* --- span-record extraction (0x39bd2) --- */
    uint32_t edi = *(volatile uint32_t *)(uintptr_t)esi;   /* mov edi,[esi] (scanline dest offset) */
    esi -= 4;
    G32(VA_g_current_span_record) = (int32_t)ebx;                           /* g_current_span_record = ebx (pre-advance) */
    uint32_t edx = (uint32_t)(*(volatile uint32_t *)(uintptr_t)ebx) & 0xffffu;  /* record+0 = start x */
    uint32_t ecx = *(volatile uint16_t *)(uintptr_t)(ebx + 0xc);               /* record+0xc = count */
    ebx -= 0x18;
    G32(VA_g_sprite_span_remaining) = (int32_t)G32(VA_g_sprite_span_remaining) - 1;              /* dec g_sprite_span_remaining */
    if ((int16_t)(uint16_t)ecx < (int16_t)(uint16_t)edx) { uint32_t t = ecx; ecx = edx; edx = t; }
    if (G8(VA_g_sprite_span_flip + 0xc) != 0) { esi += 8; ebx += 0x30; }       /* flip-dir record stride */
    *p_esi = esi; *p_ebx = ebx;                            /* loop-carried advance (write back) */
    if (G8(VA_g_render_x_flip_flag) != 0) {
        /* 0x39e69 alt/MIRROR path (x-flip): dest = scanline + g_84954 - end_x; swap the record's tex-coord
         * words (record[+2] <-> record[+0xe]); g_8a3a8 = screen-mirror of end_x about center 0x909a0. Then it
         * `jmp 0x39c26` -> rejoins the SAME perspective/shade/dispatch below (so we fall through, not return). */
        edi += (uint32_t)G32(VA_g_current_decoded_frame + 0x10);                     /* add edi,[0x84954] */
        edi -= ecx;                                        /* sub edi,ecx  (ecx = end x, pre-width) */
        uint32_t rec = (uint32_t)G32(VA_g_current_span_record);             /* swap record[+2] <-> record[+0xe] */
        uint16_t r_e = *(volatile uint16_t *)(uintptr_t)(rec + 0xe);
        *(volatile uint16_t *)(uintptr_t)(rec + 0xe) = *(volatile uint16_t *)(uintptr_t)(rec + 2);
        *(volatile uint16_t *)(uintptr_t)(rec + 2) = r_e;
        uint16_t ctr = (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x24);             /* g_8a3a8 = mirror of ecx about center (16/32-bit mix) */
        uint32_t ev = (ecx & 0xffff0000u) | (uint16_t)((uint16_t)ecx - ctr);  /* sub ax,[909a0] */
        ev = (uint32_t)(-(int32_t)ev);                                        /* neg eax */
        ev = (ev & 0xffff0000u) | (uint16_t)((uint16_t)ev + ctr);             /* add ax,[909a0] */
        G32(VA_g_sprite_tex_step_v2 + 0x10) = (int32_t)ev;
    } else {
        edi += edx;                                        /* 0x39c18 dest += start x */
        G32(VA_g_sprite_tex_step_v2 + 0x10) = (int32_t)edx;                       /* g_8a3a8 = span start x */
    }
    ecx = (ecx - edx) + 1;                                 /* span pixel width (both paths: 0x39c20 / 0x39e9c) */
    out.edi = edi; out.ecx = ecx;

    /* --- perspective divide (0x39c26): persp = DIVIDEND / remaining (unsigned), guarded.
     * The dividend is the SMC-patched immediate at 0x39c27 (mov eax,imm), NOT the literal 0x1234567 the
     * decompiler shows — the driver patches it to (g_90a12<<18)*g_90998 before the loop. persp_dividend
     * carries that value in. (cmp [0x8a358],1 / jle: signed >1 gates the div; edx=0 so it's 32-bit.) --- */
    uint32_t persp = persp_dividend;
    uint32_t remaining = (uint32_t)G32(VA_g_sprite_span_remaining);
    if ((int32_t)remaining > 1) persp = persp_dividend / remaining;
    G32(VA_g_sprite_perspective_step) = (int32_t)persp;                         /* g_sprite_perspective_step */

    uint8_t bh = 0;                                        /* colormap-row byte built across the tree */

    /* --- shade / source-coord decision tree (0x39c41) --- */
    if (G8(VA_g_span_blend_mode_flag) != 0) {
        /* 0x39ca6: fixed source coords + shade 1 -> normal dispatch */
        G32(VA_g_sprite_src_coord_step) = 0;                                  /* g_sprite_src_coord_step */
        G32(VA_g_sprite_src_coord) = 0x10000;                            /* g_sprite_src_coord */
        bh = 1;
        G8(VA_g_sprite_span_shade) = 1;                                   /* g_sprite_span_shade */
        G8(VA_g_sprite_span_flip + 0xa) = 0;
        G8(VA_g_sprite_span_shade) = bh;                                  /* 0x39d5c mov [0x8a3ba],bh */
    } else if (G8(VA_g_column_clip_mode) != 0) {
        /* 0x39d1c: clip-mode shade derived from the perspective step */
        uint32_t sh = persp >> ((uint8_t)G8(VA_g_clip_shade_shift_default) & 0x1fu);  /* 0x39d1e shr eax,imm — read the LIVE SMC byte (de-SMC; see tex-width setup) */
        int16_t ax = (int16_t)((uint16_t)sh - (uint16_t)G16(VA_g_floorceil_depth_clip_bias));
        if (ax > 0) {                                      /* jle 0x39d4a when <=0 */
            if (ax >= (int16_t)G16(VA_g_floorceil_clip_scale)) ax = (int16_t)G16(VA_g_floorceil_clip_scale);  /* clamp to 0x90a20 */
            if (ax <= 0x1f) {                              /* jg 0x39d43 fast path when >0x1f */
                bh = (uint8_t)(bh + (uint8_t)ax);          /* add bh,al */
                G8(VA_g_sprite_span_shade) = bh;                          /* 0x39d5c */
            } else {
                out.fn_va = 0x39fc0; return out;           /* 0x39d43 -> jmp 0x39fc0 */
            }
        } else {
            /* 0x39d4a: shade 0 */
            if (G16(VA_g_world_surface_draw_flags) & 8) G8(VA_g_sprite_span_flip + 0xa) = 0x80;
            G8(VA_g_sprite_span_shade) = bh;                              /* 0x39d5c (bh still 0) */
        }
    } else if (G8(VA_g_render_textured_flag) != 0) {
        /* 0x39c64: a22 set -> per-span source-coord interpolation or a constant-shade fast path */
        uint16_t coord_hi = *(volatile uint16_t *)(uintptr_t)(G32(VA_g_current_span_record) + 0xe);  /* record+0xe */
        uint8_t ah = (uint8_t)(coord_hi >> 8);
        uint8_t flag = *(volatile uint8_t *)(uintptr_t)(G32(VA_g_current_span_record) + 3);          /* record+3 */
        if (G8(VA_g_span_textured_mode_flag) == 0 && ah == flag) {
            /* 0x39cd0: constant-shade decision -> either a fast inner-loop or (textured) normal */
            G8(VA_g_sprite_span_shade) = ah;
            if (ah == 0x1f)      { out.fn_va = 0x39fc0; return out; }   /* 0x39d15 */
            else if (ah == 0)    { if (G16(VA_g_world_surface_draw_flags) & 8) G8(VA_g_sprite_span_flip + 0xa) = 0x80;
                                   else { out.fn_va = 0x39fcd; return out; } }   /* 0x39d07 */
            else                 { if (G16(VA_g_world_surface_draw_flags) & 8) G8(VA_g_sprite_span_flip + 0xa) = 1;
                                   else { out.fn_va = 0x3a000; return out; } }   /* 0x39d0e */
            /* (textured: fall through to the stepper build + normal dispatch) */
        } else {
            /* 0x39c7c: build the source-coord stepper */
            uint32_t width = ecx & 0xffffu;
            uint32_t start_coord = (uint32_t)(*(volatile uint16_t *)(uintptr_t)(G32(VA_g_current_span_record) + 2)) << 8;
            G32(VA_g_sprite_src_coord) = (int32_t)start_coord;           /* g_sprite_src_coord */
            int32_t num = (int32_t)(((uint32_t)(coord_hi & 0xffffu) << 8) - start_coord);
            G32(VA_g_sprite_src_coord_step) = (width != 0) ? (num / (int32_t)width) : 0;  /* idiv; g_sprite_src_coord_step */
        }
    }
    /* a354==0 && clip==0 && a22==0 -> falls straight through to the stepper build */

    /* --- affine texture stepper build (0x39d62), textured surfaces only --- */
    if (G16(VA_g_world_surface_draw_flags) & 8) {
        uint32_t guard_hi = (uint16_t)G16(VA_g_sprite_span_width);        /* bx = g_8a372 (loop-guard hi word) */
        if (guard_hi >= 2) {
            /* SMC-patched idiv dividends (0x39d80 = -(cos<<12), 0x39d8d = sin<<12; x-flip-negated), NOT the
             * literal 0x12345678 the decompiler shows. Signed idiv by guard_hi. (cos=0 -> texU=0.) */
            G32(VA_g_sprite_tex_step_u) = tex_divU  / (int32_t)guard_hi;  /* g_sprite_tex_step_u  (idiv 0x39d85) */
            G32(VA_g_sprite_tex_step_u2) = tex_divU2 / (int32_t)guard_hi;  /* g_sprite_tex_step_u2 (idiv 0x39d92) */
        }
        int32_t v  = ((int32_t)G32(VA_g_sprite_perspective_step) * (int32_t)G32(VA_g_sprite_src_coord_step + 0x4)) >> 2;   /* persp * sin >> 2 */
        int32_t v2 = ((int32_t)G32(VA_g_sprite_perspective_step) * (int32_t)G32(VA_g_sprite_src_coord_step + 0x8)) >> 2;   /* persp * cos >> 2 */
        int32_t scr = (int16_t)((uint32_t)G32(VA_g_sprite_tex_step_v2 + 0x10) - (uint32_t)G16(VA_g_span_src_wrap_reoffset + 0x24));  /* screen-rel x */
        v  += (int32_t)((uint32_t)G32(VA_g_sprite_tex_step_u) & 0xffffffu) * scr + (int32_t)G32(VA_g_sprite_src_coord_step + 0xc);
        v2 += (int32_t)((uint32_t)G32(VA_g_sprite_tex_step_u2) & 0xffffffu) * scr - (int32_t)G32(VA_g_sprite_src_coord_step + 0x10);
        G32(VA_g_sprite_tex_step_v) = v;                                  /* g_sprite_tex_step_v */
        G32(VA_g_sprite_tex_step_v2) = v2;                                 /* g_sprite_tex_step_v2 */
        uint16_t fl = (uint16_t)G16(VA_g_world_surface_draw_flags);
        if (fl & 6) {
            if (fl & 4) { G32(VA_g_sprite_tex_step_v2) = -G32(VA_g_sprite_tex_step_v2); G32(VA_g_sprite_tex_step_u2) = -G32(VA_g_sprite_tex_step_u2); }
            if (fl & 2) { G32(VA_g_sprite_tex_step_v) = -G32(VA_g_sprite_tex_step_v); G32(VA_g_sprite_tex_step_u) = -G32(VA_g_sprite_tex_step_u); }
        }
    }
    /* dispatch tail. FIRST gate (0x39e36): the SUBPASS write-out. When
     * g_world_render_subpass_kind (0x90a48) != 0 (the deferred/secondary render pass, e.g. the cursor),
     * the driver does NOT render this span — it builds a deferred-span record (0x39ea5-0x39f7e) and
     * RETURNS from the whole function (0x39f83 add esp,0x10; ret), skipping both the inner loop and the
     * 0x39e55 guard subtract. We signal that via out.terminate; the caller does the write-out + returns. */
    if (G8(VA_g_world_render_subpass_kind) != 0) { out.terminate = 1; return out; }
    /* The g_8a3b6 selector OVERRIDES the mode-ladder variant [0x8a368] and is RESET to 0 after use:
     *   g_8a3b6 == 0      -> normal jmp [g_sprite_column_fn]
     *   g_8a3b6 > 0 (1)   -> texmap 0x3a368 (DS colormap + constant shade)  [the constant-shade case]
     *   g_8a3b6 < 0 (0x80)-> a352 ? [0x8a368]-with-args : (flags&8 ? manual-unroll 0x3a220 : 0x39fc0) */
    if (G8(VA_g_sprite_span_flip + 0xa) == 0) {
        out.fn_va = (uint32_t)G32(VA_g_sprite_column_fn);
    } else {
        int nonneg = (int8_t)G8(VA_g_sprite_span_flip + 0xa) >= 0;         /* bVar25: g_8a3b6 positive */
        G8(VA_g_sprite_span_flip + 0xa) = 0;                               /* reset */
        if (nonneg)                       out.fn_va = g_os_force_base_dispatch
                                                          ? (uint32_t)G32(VA_g_sprite_column_fn) : 0x3a368; /* texmap */
        else if (G8(VA_g_span_textured_mode_flag) != 0)        out.fn_va = (uint32_t)G32(VA_g_sprite_column_fn);
        else if ((G16(VA_g_world_surface_draw_flags) & 8) == 0) out.fn_va = 0x39fc0;
        else                              out.fn_va = 0x3a220;
    }
    return out;
}

/* ===================== Batch 56 (full scaled-sprite span driver, 0x39610) =====================
 * draw_scaled_sprite_spans — the orchestration: render-mode ladder (selects an inner-loop variant
 * into g_sprite_column_fn + sets flags + calls the edge-walker), the per-draw setup (view-angle sincos
 * cache, scanline range, perspective loop-guard, texture-width mask), and the per-scanline span loop that
 * calls the verified per-span body and dispatches to the verified inner loops. The edge-walker
 * rasterize_floorceil_polygon (0x3b1c1) that builds the per-scanline run-list is BRIDGED via call_orig
 * (its `je` empty-test = the callee ZF). Verified IN-PROCESS (live-swap + framebuffer differential). */

/* edge-walker bridge: build the run-list from the surface geometry (esi); return nonzero if empty.
 * The original driver's first instruction is `mov gs,[0x8a2a8]` (the colormap selector), so the in-game
 * edge-walker + its floor/ceiling sub-renderers run with GS = colormap. The driver does NOT touch ES/FS
 * before this call, so they hold the driver's *entry* selectors (passed in as es_sel/fs_sel from the
 * trapping context). The textured floor/ceiling sub-renderers (render_floorceil_tex_* via in_FS_OFFSET,
 * blend via ES) read their source through FS/ES, so we MUST preset the game's real selectors here —
 * otherwise call_orig keeps the host's TLS FS/ES and the floor/ceiling textures render from garbage. */
static int sprite_edgewalk_empty(uint32_t esi, uint16_t es_sel, uint16_t fs_sel)
{
    /* re-point 0x3b1c1 rasterize_floorceil_polygon: the lifted body is pure-DS (all callees peeled;
     * selectors IGNORED, see its def), and returns 1 when the run-list is empty (== ZF set on the
     * original). The gs/es/fs selectors that call_orig needed are now defensive — dropped safely. */
    return rasterize_floorceil_polygon(esi, (uint16_t)G16(VA_g_active_world_remap_selector), es_sel, fs_sel);
}

/* optional per-span debug hook: host sets it during the byte-differential to pinpoint the first
 * span whose rendered pixels diverge from the original. NULL for the oracle / normal runs (no
 * behavior change, no overhead beyond a null check). */
void (*g_sprite_span_dbg)(uint32_t idx, uint32_t fn_va, uint32_t edi, uint32_t ecx) = NULL;

/* DEBUG: the driver exposes its per-call guard bookkeeping so the host differential can pin whether a
 * 0x8a370 divergence is in the setup guard or the subtract count. g_os_dbg_guard_setup = 0x8a370 just
 * before the span loop; g_os_dbg_range = the guard divisor (uVar13); g_os_dbg_subcount = how many
 * times the loop tail subtracted; g_os_dbg_subval = the per-span subtrahend (loop_tail_sub). */
uint32_t g_os_dbg_guard_setup = 0, g_os_dbg_range = 0, g_os_dbg_subcount = 0, g_os_dbg_subval = 0;

/* DEBUG A/B toggle (host sets from ROTH_LIFT_NOTEXMAP): route the positive-g_8a3b6 override to the
 * base column fn ([0x8a368], tex_shaded) instead of texmap (0x3a368). 0 = faithful. Tests whether the
 * original renders these const-shade spans via tex_shaded (GS colormap) rather than texmap (DS). */
int g_os_force_base_dispatch = 0;

/* dispatch one span to the mode-ladder-selected inner loop. The screen-writing variants take the
 * framebuffer base (g_render_target_buffer == the screen-segment base) as their es_base; the textured
 * ones read it internally and take the blend/texture bases. edi is the framebuffer-relative offset. */
static void sprite_dispatch(uint32_t fn_va, uint32_t ecx, uint32_t edi,
                            uint32_t gs_base, uint32_t es_base, uint32_t fs_base)
{
    uint8_t *fb = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_render_target_buffer);   /* screen base (== ES base) */
    uint8_t *gs = (uint8_t *)(uintptr_t)gs_base;
    uint8_t *es = (uint8_t *)(uintptr_t)es_base;
    uint8_t *fs = (uint8_t *)(uintptr_t)fs_base;
    /* g_sprite_column_fn (0x8a368) holds the host-REBASED handler VA; the fast-path keys we set directly
     * are canon. Normalize to canon for the switch (both forms select the same lifted inner loop). */
    if (fn_va >= OBJ_DELTA) fn_va -= OBJ_DELTA;
    switch (fn_va) {
    case 0x39fcd: render_sprite_span_fill_39fcd(ecx, edi, fb);            break;
    case 0x3a000: render_sprite_span_solid_3a000(ecx, edi, gs, fb);       break;
    case 0x3a0b1: render_sprite_span_gradient_3a0b1(ecx, edi, gs, fb);    break;
    case 0x3a100: render_sprite_span_tex_3a100(ecx, edi, es);             break;
    case 0x3a220: render_sprite_span_tex_3a220(ecx, edi, fs);            break;
    case 0x3a368: render_span_texmap_3a368(ecx, edi, fs);                break;
    case 0x3a4f8: render_sprite_span_tex_shaded_3a4f8(ecx, edi, gs, fs); break;
    case 0x3a700: render_sprite_span_tex_blend_3a700(ecx, edi, gs, es);  break;
    case 0x39fc0: {   /* FUN_00039fc0: g_8a3b6 = g_89f10 low byte, then fill the span with word g_89f10 */
        uint16_t w = (uint16_t)G16(VA_g_das_palette_remap_prefix);
        G8(VA_g_sprite_span_flip + 0xa) = (uint8_t)w;
        uint32_t n = ecx & 0xffffu;
        for (uint32_t i = 0; i < n; i++) fb[edi + i] = (uint8_t)w;   /* lo==hi for fill colours */
        break; }
    default: break;   /* unknown variant -> skip */
    }
}

/* Models the SMC-patched shift bytes the driver writes into its OWN code: 0x39aed (the `shl eax,imm`
 * that builds the perspective-divide dividend) and 0x39b08 (the `shr ax,imm` in the guard math). The
 * textured texture-width setup patches BOTH to (4 - g_9098c); untextured surfaces don't touch them, so
 * they keep the previous value (static image default 0x12 = 18). STATEFUL across calls, exactly like
 * the real code bytes — so the lift must persist it rather than recompute from scratch each call. */
static uint8_t g_persp_shift = 0x12;

/* secondary-surface deferred write-out (0x39ea5-0x39f7e): when g_world_render_subpass_kind (0x90a48) != 0
 * — the deferred/secondary render pass (e.g. the cursor) — the driver does NOT render this span. It records
 * the span's parameters into the deferred-span record (0x90a4a..0x90a68) and returns. The a352!=0 case first
 * samples the texel (FS=[0x909b0], whose base == the fs_base texture base); a 0 (transparent) texel skips the
 * record build entirely. Called once (the first span that reaches the dispatch tail), then the driver returns. */
static void sprite_secondary_writeout(uint32_t fs_base)
{
    if (G8(VA_g_span_textured_mode_flag) != 0) {
        /* transparency test (0x39eae-0x39ef0): idx = (g_8a39a & (h-1))*w + (g_8a396 & (w-1)); the original
         * uses the LOW BYTE of g_90978(w)/g_90988(h) with 8-bit `dec`s. */
        uint8_t  w  = (uint8_t)G16(VA_g_span_src_row_width);
        uint8_t  h1 = (uint8_t)((uint8_t)G16(VA_g_span_src_wrap_reoffset + 0xc) - 1u);
        uint8_t  w1 = (uint8_t)(w - 1u);
        uint32_t idx = (uint32_t)((uint8_t)G8(VA_g_sprite_tex_step_v2 + 0x2) & h1) * (uint32_t)w
                     + (uint32_t)((uint8_t)G8(VA_g_sprite_tex_step_v + 0x2) & w1);
        if (*(volatile uint8_t *)(uintptr_t)(fs_base + idx) == 0) return;   /* transparent -> no record */
    }
    /* build the deferred record (0x39ef6-0x39f7e); coordinate masks here use the 16-bit (width/height)-1. */
    G16(VA_g_world_render_subpass_kind + 0x6) = (uint16_t)((uint8_t)G8(VA_g_sprite_tex_step_v + 0x2) & (uint16_t)((uint16_t)G16(VA_g_span_src_row_width) - 1u));  /* texU */
    G16(VA_g_world_render_subpass_kind + 0x4) = (uint16_t)((uint8_t)G8(VA_g_sprite_tex_step_v2 + 0x2) & (uint16_t)((uint16_t)G16(VA_g_span_src_wrap_reoffset + 0xc) - 1u));  /* texV */
    G16(VA_g_world_render_subpass_kind + 0x2) = (uint16_t)G32(VA_g_current_das_entry_id);
    G8(VA_g_world_render_subpass_kind + 0x1)  = (uint8_t)G8(VA_g_world_render_subpass_kind);
    G32(VA_g_subpass_surfrec_ref) = (int32_t)G32(VA_g_map_das_fat_buffer + 0x8);
    G32(VA_g_subpass_reflect_param_b + 0x2) = (int32_t)G32(VA_g_current_proc_tag + 0x118);
    G16(VA_g_subpass_surfrec_ref + 0x4) = (uint16_t)G16(VA_g_world_surface_draw_flags);
    G32(VA_g_subpass_surfrec_ref + 0x6) = (int32_t)G32(VA_g_map_das_fat_buffer + 0xc);
    G8(VA_g_subpass_reflect_param_b + 0x6)  = (uint8_t)G8(VA_g_turn_view_scale_state + 0x2);
    G32(VA_g_subpass_persp_step) = (int32_t)(((uint32_t)G32(VA_g_sprite_perspective_step) << 1) >> (g_persp_shift & 0x1f));  /* (perspstep*2)>>persp */
}

void draw_scaled_sprite_spans(uint32_t esi, uint32_t gs_base, uint32_t es_base, uint32_t fs_base,
                                     uint16_t es_sel, uint16_t fs_sel)
{
    /* 0x39610: scale magnitude + clear the blend flag */
    { int16_t s = (int16_t)G16(VA_g_sprite_view_angle + 0x6); G16(VA_g_view_offset_y + 0xc) = (uint16_t)(s < 0 ? -s : s); }
    G8(VA_g_render_textured_flag) = 0;

    /* ---- render-mode ladder: select g_sprite_column_fn (0x8a368) + flags, call the edge-walker ---- */
    if ((G16(VA_g_world_surface_draw_flags) & 8) == 0) {
        /* untextured family */
        if (G8(VA_g_span_blend_mode_flag) != 0) { G8(VA_g_render_textured_flag) = 0xff; G8(VA_g_column_clip_mode) = 1; }
        G8(VA_g_render_textured_flag + 0x1) = 0;
        if (G8(VA_g_column_clip_mode) == 0 || ((G16(VA_g_world_surface_draw_flags) & 0x8000) == 0 && (G16(VA_g_world_alt_render_flags) & 0x8000) == 0)) {
            if (sprite_edgewalk_empty(esi, es_sel, fs_sel)) return;
        } else {
            G8(VA_g_render_textured_flag) = 1;
            if (sprite_edgewalk_empty(esi, es_sel, fs_sel)) return;
            if ((int8_t)G8(VA_g_sprite_render_mode) > 1) {
                if (G8(VA_g_sprite_render_mode) == 2) {
                    G8(VA_g_sprite_span_shade) = 0x1f; G8(VA_g_sprite_fill_index) = (uint8_t)G16(VA_g_das_palette_remap_prefix);
                    G8(VA_g_column_clip_mode) = 0; G8(VA_g_render_textured_flag) = 0; G32(VA_g_sprite_column_fn) = 0x3a000;
                } else { G32(VA_g_sprite_column_fn) = 0x3a0b1; G8(VA_g_column_clip_mode) = 0; }
                goto do_setup;
            }
            G8(VA_g_column_clip_mode) = 0; G8(VA_g_render_textured_flag) = 0;
        }
        G32(VA_g_sprite_column_fn) = 0x39fcd;
        if (G8(VA_g_column_clip_mode) != 0) G32(VA_g_sprite_column_fn) = 0x3a000;
        goto do_setup;
    }
    /* textured family (g_world_surface_draw_flags & 8) */
    if ((G8(VA_g_pool_check_enabled + 0x2c) & 0x80) == 0) {
        if (G8(VA_g_span_blend_mode_flag) != 0) { G8(VA_g_render_textured_flag) = 0xff; G8(VA_g_column_clip_mode) = 1; }
        if (!(G8(VA_g_column_clip_mode) == 0 || ((G16(VA_g_world_surface_draw_flags) & 0x8000) == 0 && (G16(VA_g_world_alt_render_flags) & 0x8000) == 0))) {
            G8(VA_g_render_textured_flag) = 1; G8(VA_g_render_textured_flag + 0x1) = 0;
            if (sprite_edgewalk_empty(esi, es_sel, fs_sel)) return;
            if (G8(VA_g_span_textured_mode_flag) == 0) {
                if ((int8_t)G8(VA_g_sprite_render_mode) > 1) {
                    if (G8(VA_g_sprite_render_mode) == 2) {
                        G8(VA_g_sprite_span_shade) = 0x1f; G8(VA_g_sprite_fill_index) = (uint8_t)G16(VA_g_das_palette_remap_prefix);
                        G16(VA_g_world_surface_draw_flags) = (uint16_t)(G16(VA_g_world_surface_draw_flags) & 0xfff7); G8(VA_g_column_clip_mode) = 0; G8(VA_g_render_textured_flag) = 0;
                        G32(VA_g_sprite_column_fn) = 0x3a000; goto do_setup;
                    }
                    G32(VA_g_sprite_column_fn) = 0x3a4f8; G8(VA_g_column_clip_mode) = 0; goto tex_width;
                }
                G8(VA_g_column_clip_mode) = 0; G8(VA_g_render_textured_flag) = 0; goto sel_828;
            }
            G32(VA_g_sprite_column_fn) = 0x3a700; G8(VA_g_column_clip_mode) = 0; goto tex_width;
        }
        /* clip==0 / no special flags -> fall through to the edge-walk below */
    } else {
        G8(VA_g_column_clip_mode) = 0; G8(VA_g_render_textured_flag) = 0;
    }
    G8(VA_g_render_textured_flag + 0x1) = 0;
    if (sprite_edgewalk_empty(esi, es_sel, fs_sel)) return;
sel_828:
    if (G8(VA_g_span_textured_mode_flag) == 0) {
        G32(VA_g_sprite_column_fn) = 0x3a220;
        if (G8(VA_g_column_clip_mode) != 0) G32(VA_g_sprite_column_fn) = 0x3a368;
    } else {
        G32(VA_g_sprite_column_fn) = 0x3a100;
        if (G8(VA_g_column_clip_mode) == 0) G8(VA_g_sprite_span_shade) = 0;
    }
tex_width:
    /* ---- texture-width setup (0x39862): mask g_8a360, flip g_8a3ac, the SMC shift g_persp_shift
     * (0x39aed/0x39b08 = 4 - g_9098c), and the base steppers g_8a384/_388. CORRECTION: the
     * bsr-based inner-loop SMC (0x39882-0x398da) IS needed — it patches each textured loop's shl/shr
     * immediates per-texture; we mirror it into g_tex_shl / g_tex_shr_* (the inner loops read those). */
    {
        uint16_t srw  = (uint16_t)G16(VA_g_span_src_row_width);            /* g_span_src_row_width */
        uint16_t d988 = (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0xc);
        uint16_t d98c = (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x10);
        /* clip-shade base: byte[0x399d8] is the imm of the degenerate `mov ax,0xa`, read as a memory
         * operand by the normal path. It = the HIGH BYTE of
         * g_shade_const_table_a[g_render_shade_level], SMC-patched EXTERNALLY by set_render_shade_level
         * (0x2d6a8, a per-frame/per-pass render param) — NOT by this driver, so it persists across calls
         * and modes. Read it live and apply the original's per-path arithmetic (clip@39d20). The eventual
         * de-SMC plan is a `shade_a` global; reading the byte is byte-exact meanwhile. (0x399d9 stays 0.) */
        uint8_t  clip_base = (uint8_t)G8(VA_g_shade_clip_base_default);
        uint16_t uVar12;
        int set_mask = 1;                                  /* original writes g_8a360 except in degenerate-a352==0 */
        if (!(srw == d988 && (uint8_t)srw == 0)) {
            /* normal (0x39879 -> 0x399c6): g_8a3ac=0; shift = 4 - g_9098c; cl=0x10, ch=7. */
            G16(VA_g_sprite_span_flip) = 0;
            uVar12 = 0x0710;
            g_persp_shift = (uint8_t)(4u - (uint32_t)d98c);
            /* textured -> 0x399c6: clip-shade shr imm = base + 1 - g_9098c (the `mov ax,0xa[base]; inc; sub edx`) */
            G8(VA_g_clip_shade_shift_default) = (uint8_t)((uint32_t)clip_base + 1u - (uint32_t)d98c);   /* SMC clip-shade shift (0x39d20 shr imm) */
            /* inner-loop shift SMC (0x39882 `mov ah,al; bsr bx,ax`, then 0x10-bx / +8): both 8a352 paths use
             * the same 0x10-bsr (shl) / 0x18-bsr (shr) formula; only the patched target loops differ. */
            {
                uint8_t w = (uint8_t)srw;
                if (w) {
                    uint32_t axv = ((uint32_t)w << 8) | w;
                    int b = 31 - __builtin_clz(axv);       /* bsr bx,ax */
                    g_tex_shl = (uint8_t)(0x10u - (uint32_t)b);
                    g_tex_shr_even = g_tex_shr_odd = (uint8_t)(0x18u - (uint32_t)b);
                }
            }
        } else if (G8(VA_g_span_textured_mode_flag) == 0) {
            /* degenerate, a352==0 (0x3995e -> 0x399f3): g_8a3ac=0xffff; shift = 4-(g_9098c-1); cl=0x11, ch=8.
             * NOTE: this path does NOT touch g_8a360 (it jumps past the mask write), so it stays stale. */
            G16(VA_g_sprite_span_flip) = 0xffff;
            uVar12 = 0x0811;
            g_persp_shift = (uint8_t)(4u - (uint32_t)(uint16_t)(d98c - 1u));
            /* untextured a352==0 -> 0x39967: clip-shade shr imm = base + g_9098c (dx=d98c, dec edx, +base, inc) */
            G8(VA_g_clip_shade_shift_default) = (uint8_t)((uint32_t)clip_base + (uint32_t)d98c);   /* SMC clip-shade shift */
            set_mask = 0;
        } else {
            /* degenerate, a352!=0 (0x39992 -> 0x39908 -> 0x39933 -> 0x399c6): g_8a3ac=0; shift = 4-g_9098c; cl=0x10, ch=8. */
            G16(VA_g_sprite_span_flip) = 0;
            uVar12 = 0x0810;
            /* 0x39992 does `mov eax,0; jmp 0x39908`, which patches the TRANSLUCENT inner-loop shift immediates
             * (0x3a116/0x3a166 shl, 0x3a190/0x3a1b6 shr inside 0x3a100; same in 0x3a700) to shl=0, shr=8 — the
             * natural shifts for a 256-wide degenerate texture (integer column = accumulator>>8, no pre-scale).
             * The normal path computes these via bsr; this branch must set the literals or they stay STALE from
             * the previous (smaller) texture and the cloud renders striped. (Differential missed it: cloud is
             * animated -> skipped.) g_tex_shl/g_tex_shr_* are the lift's model of those SMC bytes. */
            g_tex_shl = 0;
            g_tex_shr_even = g_tex_shr_odd = 8;
            g_persp_shift = (uint8_t)(4u - (uint32_t)d98c);
            /* untextured a352!=0 ALSO reaches 0x399c6: clip-shade shr imm = base + 1 - g_9098c (this is the
             * floor/ceiling path: base=8, d98c=1 -> clip=8, matching the runtime clip@39d20=08). */
            G8(VA_g_clip_shade_shift_default) = (uint8_t)((uint32_t)clip_base + 1u - (uint32_t)d98c);   /* SMC clip-shade shift */
        }
        if (set_mask) G32(VA_g_sprite_tex_step_frac) = (uint16_t)(srw * d988 - 1u);   /* texture wrap mask */
        if ((G8(VA_g_pool_check_enabled + 0x2c) & 0x10) == 0) {
            uint32_t sh1 = (uint8_t)((uint8_t)uVar12 - (uint8_t)d98c) & 0x1f;   /* bVar11 = cl - g_9098c */
            uint32_t sh2 = (uint8_t)(int8_t)(uVar12 >> 8) & 0x1f;               /* ch */
            G32(VA_g_sprite_src_coord_step + 0xc) = ((uint32_t)(uint16_t)G16(VA_g_view_offset_x) << sh1) + ((uint32_t)(uint16_t)G16(VA_g_view_offset_y + 0x2) << sh2);
            G32(VA_g_sprite_src_coord_step + 0x10) = ((uint32_t)(uint16_t)G16(VA_g_view_offset_y) << sh1) + ((uint32_t)(uint16_t)G16(VA_g_view_offset_y + 0x4) << sh2);
        } else {
            G32(VA_g_sprite_src_coord_step + 0xc) = (uint32_t)(uint16_t)G16(VA_g_view_offset_y + 0x2) << 8;
            G32(VA_g_sprite_src_coord_step + 0x10) = (uint32_t)(uint16_t)G16(VA_g_view_offset_y + 0x4) << 8;
        }
    }

do_setup:
    /* g_sprite_column_fn (0x8a368): the original stores the host-REBASED handler address; our mode ladder
     * wrote the canon VA, so rebase it once here so the stored slot is byte-identical (sprite_dispatch
     * normalizes it back to canon when selecting the inner loop). All paths to do_setup set it first. */
    G32(VA_g_sprite_column_fn) = (int32_t)((uint32_t)G32(VA_g_sprite_column_fn) + OBJ_DELTA);
    /* ---- per-draw setup (0x39a42) ---- */
    if (G8(VA_g_pool_check_enabled + 0x2c) & 0x80) {
        int neg = (int16_t)G16(VA_g_view_offset_y + 0xc) < 1;
        G16(VA_g_view_offset_y + 0xc) = 0x4b0;
        if (neg) G16(VA_g_view_offset_y + 0xc) = 0xfb50;
    }
    /* view-angle sincos cache: recompute only when the angle changed (bridge sincos_pair 0x3bdd2) */
    {
        uint16_t ang = (uint16_t)G16(VA_g_sprite_view_angle);
        if (ang != (uint16_t)G16(VA_g_view_offset_y + 0xa)) {
            G16(VA_g_view_offset_y + 0xa) = ang;
            /* re-point 0x3bdd2 sincos_pair (multi-reg): CX->*sin_out, BX->*cos_out (both consumed
             * outputs captured by the proto); table_out (ESI) unused here. Pure-DS leaf. */
            uint16_t sc_sin, sc_cos; uint32_t sc_tbl;
            sincos_pair((uint32_t)(-(int32_t)(int16_t)ang), &sc_sin, &sc_cos, &sc_tbl);
            (void)sc_tbl;
            G32(VA_g_sprite_src_coord_step + 0x4) = (int32_t)(int16_t)sc_sin;   /* sin (cx, sign-extended) */
            G32(VA_g_sprite_src_coord_step + 0x8) = (int32_t)(int16_t)sc_cos;   /* cos (bx, sign-extended) */
        }
    }
    /* SMC dividend patches for the texU/texU2 idivs (code 0x39d80/0x39d8d): the driver
     * patches them from the cached sin/cos here, x-flip-negates them in loop-init (0x39bc6, when g_8a356
     * set). Threaded into setup_39bd2 (each span's idiv divides these by guard_hi). */
    int32_t tex_divU  = -((int32_t)G32(VA_g_sprite_src_coord_step + 0x8) << 12);   /* 0x39d80 = -(cos<<12) */
    int32_t tex_divU2 =  ((int32_t)G32(VA_g_sprite_src_coord_step + 0x4) << 12);   /* 0x39d8d =  sin<<12   */
    if (G8(VA_g_render_x_flip_flag) != 0) { tex_divU = -tex_divU; tex_divU2 = -tex_divU2; }  /* x-flip negate */
    /* scanline range -> direction flag g_8a3b8; perspective loop-guard (0x8a370, read hi-word as g_8a372) */
    int do_guard = 1;
    uint16_t range_val = 0;            /* uVar13: the positive scanline range (guard divisor) */
    {
        int16_t cx = (int16_t)((uint16_t)G16(VA_g_view_offset_y + 0x16) - (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x28));
        if (cx > 0) { G8(VA_g_sprite_span_flip + 0xc) = 0; range_val = (uint16_t)cx; }
        else {
            cx = (int16_t)((uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x28) - (uint16_t)G16(VA_g_view_offset_y + 0x14));
            if (cx <= 0) do_guard = 0;
            else { G8(VA_g_sprite_span_flip + 0xc) = 0xff; range_val = (uint16_t)cx; }
        }
    }
    /* SMC patch #1 (code 0x39c27): perspective-divide DIVIDEND = (g_90a12 << shift) * g_90998, where
     * shift is the SMC byte 0x39aed (= g_persp_shift, 18 for untextured / 4-g_9098c for textured).
     * Passed into setup_39bd2 each span. (0x39afa: shl eax,[0x39aed]; imul eax,[0x90998].) */
    uint32_t persp_dividend;
    if (do_guard) {
        persp_dividend = ((uint32_t)(uint16_t)G16(VA_g_view_offset_y + 0xc) << (g_persp_shift & 0x1f)) * (uint32_t)(uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x1c);
        G32(VA_g_perspective_dividend_stale) = (int32_t)persp_dividend;   /* 0x39afa mov [0x39c27],eax — PATCH
                                * the SMC imm so future !do_guard draws read THIS draw's value (the old C read
                                * the stale byte but never wrote it -> it stayed at the 0x1234567 template
                                * placeholder forever image-free; the oracle staged 0x39c27 itself = vacuous). */
    } else {
        persp_dividend = (uint32_t)G32(VA_g_perspective_dividend_stale);   /* !do_guard (0x39ad1 jle 0x39b3f): 0x39c27 NOT patched this draw -> stale SMC */
    }
    /* guard 0x8a370 + SMC patch #2 (code 0x39e5b = loop-tail subtrahend), once-setup 0x39ae3-0x39b3a.
     * Faithful transcription: ax = g_90a14 >> shift (the 0x39b08 SMC byte = g_persp_shift); dx:ax =
     * ax*range; clamp the high word to a12-1; guard = ((dx:ax<<16)/a12)*2; subtrahend = guard/range.
     * (For untextured shift=18 makes ax=0 -> guard=0 -> subtrahend=0, matching the original.) */
    uint32_t loop_tail_sub;
    {
        uint16_t a12 = (uint16_t)G16(VA_g_view_offset_y + 0xc);
        if (do_guard && a12 != 0) {
            uint32_t cnt  = g_persp_shift & 0x1f;
            uint32_t axv  = ((uint32_t)(uint16_t)G16(VA_g_view_offset_y + 0xe) >> cnt) & 0xffffu;   /* shr ax,shift (16-bit) */
            uint32_t prod = axv * (uint32_t)range_val;                            /* mul cx (cx = range) */
            uint32_t dxv  = (prod >> 16) & 0xffffu;
            uint32_t axlo = prod & 0xffffu;
            uint32_t edx  = ((int16_t)(uint16_t)dxv < (int16_t)a12) ? dxv
                                                                    : (((uint32_t)a12 - 1u) & 0xffffu);
            uint64_t num  = ((uint64_t)edx << 32) | ((uint64_t)axlo << 16);       /* edx:(ax<<16) */
            uint32_t guard = (uint32_t)(num / (uint64_t)a12) * 2u;                /* div a12; add eax,eax */
            G32(VA_g_sprite_column_loop_guard) = (int32_t)guard;
            loop_tail_sub = (range_val != 0) ? (guard / (uint32_t)range_val) : 0; /* div range */
            G32(VA_g_loop_tail_sub_stale) = (int32_t)loop_tail_sub;   /* 0x39b3a mov [0x39e5b],eax — PATCH the
                                * loop-tail SMC subtrahend for future stale (a12==0 / !do_guard) draws (the old
                                * C never wrote it -> frozen at the 0x12345678 template placeholder image-free). */
        } else {
            /* guard setup skipped (!do_guard via 0x39ad1) OR a12==0 (0x39b14 je 0x39b3f): 0x8a370 and the
             * 0x39e5b subtrahend are NOT written -> the loop tail still `sub [0x8a370], imm` with the STALE
             * subtrahend (the last do_guard&&a12!=0 draw's value). Read it live so we match byte-for-byte;
             * 0x8a370 is left stale too (we don't write it here, exactly like the original). */
            loop_tail_sub = (uint32_t)G32(VA_g_loop_tail_sub_stale);
        }
    }
    /* loop init (0x39b3f): start scanline, count, and the descending dest-offset / record table ptrs */
    uint32_t Yc, count;
    if (G8(VA_g_sprite_span_flip + 0xc) == 0) {
        uint32_t e = (uint16_t)G16(VA_g_view_offset_y + 0x16) + 1u;          /* maxY+1 */
        G32(VA_g_sprite_span_remaining) = (uint16_t)(e - (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x28));
        count = (uint16_t)(e - (uint16_t)G16(VA_g_view_offset_y + 0x14));
        Yc = e - 1u;                                       /* start at maxY (descending) */
    } else {
        uint32_t mY = (uint16_t)G16(VA_g_view_offset_y + 0x14);              /* minY */
        G32(VA_g_sprite_span_remaining) = (uint16_t)((uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x28) - (mY - 1u));
        count = (uint16_t)((uint16_t)G16(VA_g_view_offset_y + 0x16) - (mY - 1u));
        Yc = mY;
    }
    uint32_t esi_l = (uint32_t)GADDR(VA_g_scanline_dest_offset_table + Yc * 4u);  /* dest-offset table @ Y */
    uint32_t ebx_l = (uint32_t)GADDR(VA_g_floorceil_span_records + Yc * 0x18u);/* span-record table @ Y */

    /* ---- per-scanline span loop (0x39bd2 .. loop tail 0x39e52) ---- */
    g_os_dbg_guard_setup = (uint32_t)G32(VA_g_sprite_column_loop_guard);    /* DEBUG: 0x8a370 just before the loop */
    g_os_dbg_range = range_val;
    g_os_dbg_subval = loop_tail_sub;
    if ((int32_t)count <= 0) { g_os_dbg_subcount = 0; return; }
    uint32_t span_idx = 0;
    do {
#ifdef ROTH_STANDALONE
        /* SAFETY sibling of the 62e5b86 edge guard (4171): a reflection-subpass sprite whose
         * re-projected edges land at Y in [screen_height, 641] passes the <=641 record guard, but
         * the dest-offset table @0x854a8 holds only g_screen_height valid rows — beyond them the
         * walk reads flat-color bytes as the DEST OFFSET -> wild write (core-proven: Y=598 ->
         * edi=0xA07630F4, same room/geometry as 62e5b86). Skip the PIXEL DISPATCH only; all
         * bookkeeping (setup walk, guard subtract, remaining) still runs so visible spans stay
         * byte-identical. Bound = LIVE height (0x8549c), not table capacity: Mode-X leaves rows
         * height..479 STALE from the previous mode. */
        uint32_t row_ = (uint32_t)(esi_l - (uint32_t)GADDR(VA_g_scanline_dest_offset_table)) >> 2;
        int y_vis_ = row_ < (uint32_t)G32(VA_g_screen_pitch + 0x4);
#endif
        span_dispatch_t d = sprite_span_setup_39bd2(&esi_l, &ebx_l, persp_dividend, tex_divU, tex_divU2);
        if (d.terminate) {            /* 0x39e36: subpass write-out (g_90a48 != 0) -> record + RETURN, no render/subtract */
            sprite_secondary_writeout(fs_base);
            g_os_dbg_subcount = span_idx;
            return;
        }
        if (d.fn_va
#ifdef ROTH_STANDALONE
            && y_vis_
#endif
            ) sprite_dispatch(d.fn_va, d.ecx, d.edi, gs_base, es_base, fs_base);
        if (g_sprite_span_dbg) g_sprite_span_dbg(span_idx, d.fn_va, d.edi, d.ecx);
        span_idx++;
        G32(VA_g_sprite_column_loop_guard) = (int32_t)G32(VA_g_sprite_column_loop_guard) - (int32_t)loop_tail_sub;  /* loop tail: sub [0x8a370],imm (0x39e5b) */
    } while ((int32_t)--count > 0);
    g_os_dbg_subcount = span_idx;                     /* DEBUG: actual subtract count this call */
}

/* ===================== Batch 57 (floor/ceiling edge-walker, 0x3b1c1) =====================
 * rasterize_floorceil_polygon — the polygon edge-walker that BUILDS the per-scanline span
 * run-list (table at 0x8cd0c, stride 0x18: left X @+0, right X @+0xc) which the scaled-sprite driver's
 * span loop consumes. Bbox-culls the surface vs the view window (0x90968..0x9096e), projects vertices
 * (0x3bbb0), optionally clips (0x3b4a2), then walks each projected edge top->bottom writing X per
 * scanline via fixed-point slope interpolation. Returns the empty test: 1 (ZF set) iff no spans were
 * emitted (g_8a440 == 0) — the driver's `je 0x39e68`.
 *   FULLY PEELED — all callees are now native C, ZERO call_orig: the vertex-record builder
 *   (0x3bbb0), the polygon clipper (0x3b4a2 subtree), the per-edge texcoord interpolator (0x3b8b7 +
 *   leaves 0x3bb1e/0x3badf), the texcoord projector (0x3b724) and the flat-edge copier (0x3b84a). All are
 *   pure-DS, so the entry selectors (formerly passed to the call_orig bridges) are no longer needed.
 *   Verified via the in-process differential (ROTH_LIFT_DIFF=rasterize_floorceil_polygon; obj3 run-list + ZF). */

/* PEEL 2: project_floorceil_edge_texcoord (0x3b724) — maps a span-edge's screen coords to a texture
 * coordinate with near-plane/depth clip (bias g_90a1e), accumulating g_sprite_render_mode bits into
 * g_90a30. esi = the edge/vertex record (+0 X / +2 Y / +4 depth); ax_in = the caller's AX (used only by
 * the 9093c&0x8000 paths). Returns AX (callers use AX only). LEAF — fully native; the `sub dx,imm` at
 * 0x3b791 is a FIXED constant (0 writers) read live from 0x3b793. */
uint16_t project_floorceil_edge_texcoord(uint32_t esi, uint16_t ax_in)
{
    #define EW16(o) ((uint16_t)*(volatile uint16_t *)(uintptr_t)(esi + (uint32_t)(o)))
    uint16_t bias = (uint16_t)G16(VA_g_floorceil_depth_clip_bias);

    if (G16(VA_g_world_surface_draw_flags) & 0x8000) {                         /* 0x3b724 jne 0x3b7f6 */
        if (!(G16(VA_g_world_alt_render_flags) & 0x8000)) {                  /* 0x3b820 */
            uint16_t depth = EW16(4);
            uint8_t  al = (uint8_t)ax_in;                /* incoming AL (used at 0x3b83f) */
            /* SMC shade-shift counts: patch_span_driver_shade 0x2d6a8 retargets these per map shade
             * level (g_shade_const_table_b: L0 shr5/shl3, L1 shr6/shl2, L2 shr7/shl1) — read LIVE. */
            uint8_t  shr_n = (uint8_t)(*(volatile uint8_t *)GADDR(0x3b829) & 0x1f);
            uint8_t  shl_n = (uint8_t)(*(volatile uint8_t *)GADDR(0x3b841) & 0x1f);
            int16_t  dx = (int16_t)((uint16_t)(depth >> shr_n) - bias); /* shr dx,imm@0x3b829; sub dx,90a1e */
            if (dx > 0) {                                /* 0x3b831 jle 0x3b801 (else) */
                int add_s = (int)(int8_t)(uint8_t)dx + (int)(int8_t)(uint8_t)(ax_in >> 8);   /* add dl,ah (8-bit) */
                uint8_t  dl = (uint8_t)((uint8_t)dx + (uint8_t)(ax_in >> 8));
                uint16_t dxw = (uint16_t)(((uint16_t)dx & 0xff00u) | dl);
                if (add_s <= 0) { G8(VA_g_sprite_render_mode) |= 1; return 0; }                 /* 0x3b835 jle 0x3b7de */
                if ((int16_t)dxw >= 0x1f) { G8(VA_g_sprite_render_mode) |= 2; return 0x1f00; }  /* 0x3b837 jge 0x3b7ea */
                G8(VA_g_sprite_render_mode) |= 4;                                               /* 0x3b842 */
                return (uint16_t)(((uint16_t)dl << 8) | (uint8_t)(al << shl_n));    /* ah=dl, al<<imm@0x3b841 */
            }
            /* dx <= 0 -> fall to the 0x3b801 AX clamp */
        }
        /* 0x3b801 AX clamp (shared with the 909ae&0x8000 set case) */
        if ((int16_t)ax_in <= 0) { G8(VA_g_sprite_render_mode) |= 4; return 0x100; }    /* 0x3b804 jle 0x3b7ad */
        if (ax_in < 0x1fffu)     { G8(VA_g_sprite_render_mode) |= 4; return ax_in; }     /* 0x3b80a jb 0x3b818 */
        G8(VA_g_sprite_render_mode) |= 2; return 0x1f00;                                /* 0x3b80c */
    }

    /* 9093c&0x8000 clear (0x3b733): dx = depth */
    uint16_t depth = EW16(4);
    if (G16(VA_g_world_alt_render_flags) & 0x8000) {                         /* 0x3b742: perspective-correct projection */
        int16_t  d0 = (int16_t)((uint16_t)EW16(0) - (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x14));
        uint32_t p0 = (uint32_t)(uint16_t)(d0 < 0 ? -d0 : d0) * (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x18);  /* mul 90994 */
        int16_t  d1 = (int16_t)((uint16_t)EW16(2) - (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x16));
        uint32_t p1 = (uint32_t)(uint16_t)(d1 < 0 ? -d1 : d1) * (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x1a);  /* mul 90996 */
        uint16_t pk0 = (uint16_t)(p0 >> 8), pk1 = (uint16_t)(p1 >> 8);   /* (mov al,ah; mov ah,dl) = bits8-23 */
        uint16_t mx  = (pk0 > pk1) ? pk0 : pk1;          /* cmp ax,bx; ja keep first else second */
        uint16_t e   = (uint16_t)(depth + mx);           /* add edx,eax (low 16) */
        uint8_t  bl  = (uint8_t)e;
        uint8_t  shr_n = (uint8_t)(*(volatile uint8_t *)GADDR(0x3b789) & 0x1f);   /* SMC shr count (0x2d6a8-patched per shade level) */
        uint8_t  shl_n = (uint8_t)(*(volatile uint8_t *)GADDR(0x3b7a2) & 0x1f);   /* SMC shl count */
        int16_t  dx  = (int16_t)((uint16_t)(e >> shr_n) - bias - (uint16_t)G16(VA_g_floorceil_edge_bias_default));  /* shr dx,imm@0x3b789; sub 90a1e; sub dx,imm.
            * The imm @0x3b794 is SMC-PATCHED per-frame (writer 0x2ac13 `mov [0x3b794],eax`, a perspective/near-plane
            * setup) — file byte is 0x1234 but runtime is small; read it LIVE. (imm = prefix66+opcode81+modrmea then imm16.) */
        if (dx <= 0)    { G8(VA_g_sprite_render_mode) |= 4; return 0x100; }              /* 0x3b796 jle 0x3b7ad */
        if (dx >= 0x1f) { G8(VA_g_sprite_render_mode) |= 2; return 0x1f00; }             /* 0x3b798 jge 0x3b7ea */
        G8(VA_g_sprite_render_mode) |= 4;                                                /* 0x3b7a5 */
        return (uint16_t)(((uint16_t)(uint8_t)dx << 8) | (uint8_t)(bl << shl_n));   /* bh=dl, bl<<imm@0x3b7a2 */
    }
    /* 0x3b7ba: simple (no perspective): dx = (depth>>5) - bias */
    {
        uint8_t  bl = (uint8_t)depth;
        uint8_t  shr_n = (uint8_t)(*(volatile uint8_t *)GADDR(0x3b7bf) & 0x1f);   /* SMC shr count (0x2d6a8-patched per shade level) */
        uint8_t  shl_n = (uint8_t)(*(volatile uint8_t *)GADDR(0x3b7d3) & 0x1f);   /* SMC shl count */
        int16_t  dx = (int16_t)((uint16_t)(depth >> shr_n) - bias);
        if (dx <= 0)    { G8(VA_g_sprite_render_mode) |= 1; return 0; }                 /* 0x3b7c7 jle 0x3b7de */
        if (dx >= 0x1f) { G8(VA_g_sprite_render_mode) |= 2; return 0x1f00; }            /* 0x3b7c9 jge 0x3b7ea */
        G8(VA_g_sprite_render_mode) |= 4;                                               /* 0x3b7d6 */
        return (uint16_t)(((uint16_t)(uint8_t)dx << 8) | (uint8_t)(bl << shl_n));
    }
    #undef EW16
}

/* PEEL 1: store_floorceil_flat_edge_texcoords (0x3b84a) — the dy==0 (single-scanline) edge fast path. Copies
 * the edge's endpoint texcoords straight into the one span-record half (no per-scanline interp). esi=edge
 * record, edi=span record. Now fully native: projects both endpoints via project_floorceil_edge_texcoord
 * (0x3b724, peeled). */
void store_floorceil_flat_edge_texcoords(uint32_t esi, uint32_t edi)
{
    #define E16(o) (*(volatile uint16_t *)(uintptr_t)(esi + (uint32_t)(o)))
    #define D16(o) (*(volatile uint16_t *)(uintptr_t)(edi + (uint32_t)(o)))
    #define D32(o) (*(volatile uint32_t *)(uintptr_t)(edi + (uint32_t)(o)))
    if (G8(VA_g_render_textured_flag + 0x1) != 0) {                              /* 0x3b84b: copy the 4 endpoint texcoord words */
        D16(4)    = E16(8);                              /* edi+4    = word[esi+8]    */
        D16(6)    = E16(0xa);                            /* edi+6    = word[esi+0xa]  */
        D16(0x10) = E16(0x14);                           /* edi+0x10 = word[esi+0x14] */
        D16(0x12) = E16(0x16);                           /* edi+0x12 = word[esi+0x16] */
    }
    D32(8)    = (uint32_t)E16(4)   << 16;                /* 0x3b874: edi+8    = word[esi+4]    << 16 */
    D32(0x14) = (uint32_t)E16(0x10) << 16;               /* 0x3b87e: edi+0x14 = word[esi+0x10] << 16 */
    if (G8(VA_g_render_textured_flag) != 0) {                              /* 0x3b888: textured -> project both endpoints */
        D16(2)   = project_floorceil_edge_texcoord(esi,       E16(6));     /* 0x3b891 */
        D16(0xe) = project_floorceil_edge_texcoord(esi + 0xc, E16(0x12));  /* 0x3b8a0 next vertex */
    }
    #undef E16
    #undef D16
    #undef D32
}

/* PEEL 3: build_floorceil_vertex_records (0x3bbb0 + the 0x3bb41 variant) — projects the surface's
 * vertex ring into the per-scanline edge-record array at 0x8c944 (count word @+0, then stride-0xc
 * records: +0 screen XY, +4 depth, +6 shade, +8/+0xa texcoord B/A). Four builders, selected by surface
 * flags: flat (90a22==0; XY+depth only), normal, the 0x1000-flag packed-texcoord variant (0x3bb41),
 * and the 0x2000-flag secondary-shade-list variant (0x3bd08). First computes the texture-extent quads
 * (0x8a410/14/18/1c) from the level texture dims (0x90978 w / 0x90988 h), flags (0x9093c) and the
 * per-surface scale nibble (0x90971). LEAF (no calls), pure-DS. esi = surface geom (flat runtime ptr,
 * same as the edge-walker); ebp=[esi+0x30] = the projected-vertex base, [esi+0x34] = vertex count,
 * esi+0x18 = per-vertex shade bytes, esi+0x36 = the vertex index list. Returns leftover EAX (the edge
 * loop inherits its hi16 for 0x8a3bc): flat = (last vtx XY & 0xffff0000)|depth; textured = last
 * texcoord-B extent. Verified via the in-process differential (ROTH_LIFT_DIFF=rasterize_floorceil_polygon). */
uint32_t build_floorceil_vertex_records(uint32_t esi)
{
    #define P8(a)  (*(volatile uint8_t  *)(uintptr_t)(a))
    #define P16(a) (*(volatile uint16_t *)(uintptr_t)(a))
    #define P32(a) (*(volatile uint32_t *)(uintptr_t)(a))
    uint32_t eax = 0, ebx, ebp, edi, edx;
    uint8_t  cl, ch;

    if (G16(VA_g_render_textured_flag) == 0) goto flat;                       /* 0x3bbb0 je 0x3bd8a */
    if (G8(VA_g_render_textured_flag + 0x1) == 0)  goto records;                    /* 0x3bbbe je 0x3bc7b */
    if (G16(VA_g_world_surface_draw_flags) & 0x1000) goto variant1000;            /* 0x3bbcb jne 0x3bb41 */

    /* --- texture-extent setup (0x3bbda): 8a410/14/18/1c texture-space quad --- */
    G32(VA_g_floorceil_step_b + 0x24) = 0;
    G32(VA_g_floorceil_step_b + 0x18) = 0;
    if (G16(VA_g_world_surface_draw_flags) & 0x20) {                              /* 0x3bc2f scaled extent (90971 nibbles) */
        uint32_t h = (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0xc);
        uint8_t  lo = (uint8_t)(G8(VA_g_column_clip_mode + 0x1) & 0xf);
        if (lo) h *= (uint8_t)(1 + lo);                     /* imul eax,(1+nib) */
        G32(VA_g_floorceil_step_b + 0x1c) = (h << 6) - 1u;
        uint32_t w = (uint16_t)G16(VA_g_span_src_row_width);
        uint8_t  hi = (uint8_t)(G8(VA_g_column_clip_mode + 0x1) >> 4);
        if (hi) w *= (uint8_t)(1 + hi);
        G32(VA_g_floorceil_step_b + 0x20) = (w << 6) - 1u;
    } else {                                                /* 0x3bbf1 non-scaled */
        uint32_t h = ((uint32_t)(uint16_t)G16(VA_g_span_src_wrap_reoffset + 0xc) << 6) - 1u;
        if (G16(VA_g_world_surface_draw_flags) & 2) { G32(VA_g_floorceil_step_b + 0x1c) = h; }         /* 0x3bc17 */
        else { G32(VA_g_floorceil_step_b + 0x1c) = 0; G32(VA_g_floorceil_step_b + 0x24) = h; }        /* 0x3bc06 */
        G32(VA_g_floorceil_step_b + 0x20) = ((uint32_t)(uint16_t)G16(VA_g_span_src_row_width) << 6) - 1u;
    }

records:                                                    /* 0x3bc7b */
    ebx = 0x8c944u + OBJ_DELTA;
    ebp = P32(esi + 0x30);
    edx = esi + 0x18;
    G8(VA_g_floorceil_edge_emitted + 0x2) = P8(edx);                                  /* default shade = first vtx byte */
    cl = P8(esi + 0x34); ch = P8(esi + 0x35);               /* sub ecx,ecx; mov cx,[esi+0x34] */
    esi += 0x36;
    P16(ebx) = (uint16_t)(((uint16_t)ch << 8) | cl);        /* [0x8c944] = count word */
    ebx += 2;
    ch--;
    if (P16(esi - 0x22) & 0x2000) goto variant2000;         /* 0x3bca1 jne 0x3bd08 */

    do {                                                    /* 0x3bca9 normal builder */
        uint8_t sh = (cl == 0) ? (uint8_t)G8(VA_g_floorceil_edge_emitted + 0x2) : P8(edx);  /* default shade on last vtx */
        if (cl != 0) edx++;
        eax = ((uint32_t)sh << 8) & 0x1f00u;
        P16(ebx + 6) = (uint16_t)eax;                       /* shade */
        edi = ebp + P16(esi); esi += 2;
        eax = P32(edi + 0xc); P32(ebx) = eax;               /* XY */
        { uint16_t d = P16(edi + 0xa); eax = (eax & 0xffff0000u) | d; P16(ebx + 4) = d; }  /* depth */
        eax = G32(VA_g_floorceil_step_b + 0x1c); if (ch & 2) eax = G32(VA_g_floorceil_step_b + 0x24);
        P16(ebx + 0xa) = (uint16_t)eax;                     /* texcoord A */
        eax = G32(VA_g_floorceil_step_b + 0x18); ch++; if (ch & 2) eax = G32(VA_g_floorceil_step_b + 0x20);
        P16(ebx + 8) = (uint16_t)eax;                       /* texcoord B */
        ebx += 0xc;
    } while ((int8_t)(--cl) >= 0);                          /* 0x3bd05 dec cl; jge */
    return eax;                                             /* 0x3bd07 */

variant2000:                                                /* 0x3bd08 secondary-shade-list variant */
    G32(VA_g_floorceil_span_fn_alt + 0x4) = esi + (uint32_t)cl * 2u + 2u;
    do {                                                    /* 0x3bd18 */
        uint8_t s_cl = cl, s_ch = ch; uint32_t s_edx = edx; /* push ecx; push edx */
        if (cl != 0) {                                      /* or cl,cl; je 0x3bd36 */
            uint32_t bx = G32(VA_g_floorceil_span_fn_alt + 0x4); G32(VA_g_floorceil_span_fn_alt + 0x4) += 2;
            uint16_t w = P16(bx);
            ch = (uint8_t)(ch + (uint8_t)w);                /* add ch,al */
            edx += w;                                       /* add edx,eax (16-bit zero-ext) */
        }
        { uint8_t sh = P8(edx); edx = s_edx;                /* mov ah,[edx]; pop edx */
          eax = ((uint32_t)sh << 8) & 0x1f00u; P16(ebx + 6) = (uint16_t)eax; }
        edi = ebp + P16(esi); esi += 2;
        eax = P32(edi + 0xc); P32(ebx) = eax;
        { uint16_t d = P16(edi + 0xa); eax = (eax & 0xffff0000u) | d; P16(ebx + 4) = d; }
        eax = G32(VA_g_floorceil_step_b + 0x1c); if (ch & 2) eax = G32(VA_g_floorceil_step_b + 0x24);
        P16(ebx + 0xa) = (uint16_t)eax;
        eax = G32(VA_g_floorceil_step_b + 0x18); ch++; if (ch & 2) eax = G32(VA_g_floorceil_step_b + 0x20);
        P16(ebx + 8) = (uint16_t)eax;
        ebx += 0xc;
        cl = s_cl; ch = s_ch;                               /* pop ecx (cl,ch restored; ch resets each iter) */
    } while ((int8_t)(--cl) >= 0);                          /* 0x3bd85 dec cl; jge */
    return eax;                                             /* 0x3bd89 */

variant1000:                                                /* 0x3bb41 packed-texcoord variant */
    ebx = 0x8c944u + OBJ_DELTA;
    ebp = P32(esi + 0x30);
    edx = esi + 0x18;
    cl = P8(esi + 0x34); ch = P8(esi + 0x35);
    esi += 0x36;
    P16(ebx) = (uint16_t)(((uint16_t)ch << 8) | cl);
    ebx += 2;
    { uint32_t s_edx = edx;                                 /* push edx (once, pre-loop) */
      do {                                                  /* 0x3bb5e */
        if (cl == 0) edx = s_edx;                           /* pop edx only on last iter (cl==0) */
        eax = ((uint32_t)P8(edx) << 8);                     /* sub eax,eax; mov ah,[edx] */
        edi = eax << 2;                                     /* mov edi,eax; shl edi,2 */
        eax &= 0x1f00u;                                     /* and ax,0x1f00 */
        P16(ebx + 6) = (uint16_t)eax;                       /* shade */
        eax = (edi & 0xffff00ffu) | ((uint32_t)P8(edx + 8) << 8);  /* mov eax,edi; mov ah,[edx+8] */
        eax >>= 1;                                          /* shr eax,1 */
        eax = (eax & 0xffff0000u) | ((eax & 0xffffu) >> 1); /* shr ax,1 */
        P16(ebx + 8) = (uint16_t)eax;                       /* texcoord B */
        edi >>= 1;                                          /* shr edi,1 */
        eax = (edi & 0xffff00ffu) | ((uint32_t)P8(edx + 4) << 8);  /* mov eax,edi; mov ah,[edx+4] */
        eax >>= 2;                                          /* shr eax,2 */
        P16(ebx + 0xa) = (uint16_t)eax;                     /* texcoord A */
        edx++;                                              /* inc edx */
        edi = ebp + P16(esi); esi += 2;
        eax = P32(edi + 0xc); P32(ebx) = eax;
        { uint16_t d = P16(edi + 0xa); eax = (eax & 0xffff0000u) | d; P16(ebx + 4) = d; }
        ebx += 0xc;
      } while ((int8_t)(--cl) >= 0);                        /* 0x3bbad dec cl; jge */
    }
    return eax;                                             /* 0x3bbaf */

flat:                                                       /* 0x3bd8a flat builder (XY+depth only) */
    ebx = 0x8c944u + OBJ_DELTA;
    ebp = P32(esi + 0x30);
    cl = P8(esi + 0x34); ch = P8(esi + 0x35);
    esi += 0x36;
    P16(ebx) = (uint16_t)(((uint16_t)ch << 8) | cl);
    ebx += 2;
    ch--;
    do {                                                    /* 0x3bda3 */
        edi = ebp + P16(esi); esi += 2;
        eax = P32(edi + 0xc); P32(ebx) = eax;
        { uint16_t d = P16(edi + 0xa); eax = (eax & 0xffff0000u) | d; P16(ebx + 4) = d; }
        ebx += 0xc;
    } while ((int8_t)(--cl) >= 0);                          /* 0x3bdbf dec cl; jge */
    return eax;                                             /* 0x3bdc1 */
    #undef P8
    #undef P16
    #undef P32
}

#define P16(a) (*(volatile uint16_t *)(uintptr_t)(a))
#define P32(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* PEEL 4a: write_ror_ramp (0x3bb1e) — shade-ramp inner loop. Walks the shade accumulator (0x8a3d4,
 * stepped by gradient 0x8a3d0) into each scanline's span-record shade slot (word @[edi], edi pre-
 * advanced +2). LEAF. Returns final edi (the caller's mode-1 path does `sub edi,ebp`). */
static uint32_t write_ror_ramp(uint32_t edi, uint32_t ebp, uint16_t cx)
{
    uint32_t edx = G32(VA_g_clip_output_vertex_count + 0x8);                            /* shade accumulator */
    uint32_t ebx = G32(VA_g_clip_output_vertex_count + 0x4);                            /* shade gradient */
    edi += 2;
    do {
        uint32_t r = (edx >> 8) | (edx << 24);              /* ror eax,8 (eax=edx) */
        P16(edi) = (uint16_t)r;                             /* word[edi] = bits 8-23 */
        edx += ebx;
        edi += ebp;
    } while ((int16_t)(--cx) >= 0);                         /* dec cx; jge */
    edi -= 2;
    return edi;
}

/* PEEL 4b: write_floorceil_span_texcoords (0x3badf) — texcoord-ramp inner loop. Per scanline writes
 * W (dword @[edi+8], from accum 0x8a3d8 stepped by gradient 0x8a3f0), U (word @[edi+4], from `eax`
 * accum 0x8a3dc stepped by `ebx` grad 0x8a3f4), V (word @[edi+6], from `edx` accum stepped by `esi`
 * grad). edx/esi = the V accumulator/gradient passed in registers. LEAF. Returns final edi. */
uint32_t write_floorceil_span_texcoords(uint32_t edi, uint32_t ebp, uint16_t cx,
                                               uint32_t edx, uint32_t esi)
{
    uint32_t eax = G32(VA_g_floorceil_accum_a);                            /* U accumulator */
    uint32_t ebx = G32(VA_g_floorceil_step_a);                            /* U gradient */
    do {
        uint32_t u_save = eax;                              /* push eax */
        eax = G32(VA_g_clip_output_vertex_count + 0xc);                                 /* W accumulator */
        P32(edi + 8) = eax;                                 /* [edi+8] = W (dword) */
        eax += G32(VA_g_floorceil_accum_b + 0x10);                                /* W += W gradient */
        G32(VA_g_clip_output_vertex_count + 0xc) = eax;
        eax = u_save;                                       /* pop eax */
        { uint32_t r = (eax >> 8) | (eax << 24); P16(edi + 4) = (uint16_t)r; }  /* U = ror(eax,8) */
        { uint32_t r = (edx >> 8) | (edx << 24); P16(edi + 6) = (uint16_t)r; }  /* V = ror(edx,8) */
        edx += esi;                                         /* V accum += V grad */
        eax += ebx;                                         /* U accum += U grad */
        edi += ebp;
    } while ((int16_t)(--cx) >= 0);                         /* dec cx; jge */
    return edi;
}

/* PEEL 4c: interp_floorceil_edge_texcoords (0x3b8b7) — for a textured edge spanning cx+1 scanlines,
 * sets up per-scanline gradients (shade, then W/U/V texcoords) and walks them into the span run-list
 * via the two ramp writers above. Two halves: a shade ramp (only if 0x90a22 != 0) + the texcoord
 * ramp. Mode g_8a43a (0/1/2 = x-major-neg / x-major-pos / y-major) selects the half-step bias and
 * which boundary endpoint is written after each writer. cx = scanline count, esi = projected edge
 * record (stride-0xc; +4 W/+6 shade/+8 U/+0xa V, next vtx at +0xc), edi = span dest, ebp = column
 * stride (+/-0x18). Calls lifted 0x3b724 (projector) + the two peeled leaves. The edx/esi low-16
 * reuse (0x8a408/0x8a40c carry idiv-remainder hi-bits | texcoord lo-bits) is tracked exactly. */
void interp_floorceil_edge_texcoords(uint32_t ecx_in, uint32_t esi, uint32_t edi, uint32_t ebp)
{
    if ((uint16_t)ecx_in == 0) return;                      /* or cx,cx; je ret */
    uint16_t cx   = (uint16_t)ecx_in;
    uint16_t mode = (uint16_t)G16(VA_g_floorceil_edge_orientation);

    /* ===== first half: shade ramp (0x3b8ca), only if textured ===== */
    if (G16(VA_g_render_textured_flag) != 0) {
        uint32_t projA = (uint16_t)project_floorceil_edge_texcoord(esi, P16(esi + 6));        /* movzx */
        int32_t  projB = (int16_t) project_floorceil_edge_texcoord(esi + 0xc, P16(esi + 0x12)); /* cwde */
        G32(VA_g_floorceil_step_b + 0x10) = (uint32_t)projB;
        int32_t  ecx  = (int32_t)(uint16_t)cx;
        int32_t  diff = projB - (int32_t)projA;
        if (diff == 0) {
            G32(VA_g_clip_output_vertex_count + 0x4) = 0;
            G32(VA_g_clip_output_vertex_count + 0x8) = projA << 8;
        } else {
            int32_t grad = (int32_t)((uint32_t)diff << 8) / ecx;
            G32(VA_g_clip_output_vertex_count + 0x4) = (uint32_t)grad;
            int32_t bias = (mode == 2) ? 0 : ((mode & 1) ? (grad >> 1) : -(grad >> 1));
            G32(VA_g_clip_output_vertex_count + 0x8) = (uint32_t)(bias + (int32_t)(projA << 8));
        }
        if (mode == 2) {
            write_ror_ramp(edi, ebp, cx);
        } else if (mode & 1) {
            uint32_t e = write_ror_ramp(edi, ebp, cx) - ebp;
            P16(e + 2) = (uint16_t)G32(VA_g_floorceil_step_b + 0x10);            /* mov [edi+2], texB-leftover (projB) */
        } else {
            write_ror_ramp(edi, ebp, cx);
            P16(edi + 2) = (uint16_t)projA;
        }
    }

    /* ===== second half: texcoord (W/U/V) ramp (0x3b97f) ===== */
    {
        int32_t  ecx = (int32_t)(uint16_t)cx;
        uint32_t W1 = (uint32_t)P16(esi + 0x10) << 16;      /* esi+0x10 (W1 = depth1<<16) */
        uint32_t W0 = (uint32_t)P16(esi + 4)    << 16;      /* esi+4    (W0 = depth0<<16) */
        G32(VA_g_floorceil_step_b + 0x28) = W1;
        G32(VA_g_floorceil_step_b + 0x2c) = W0;
        uint32_t edx;                                       /* tracked for the 0x8a408 store hi-bits */
        int32_t  dW = (int32_t)(W1 - W0);
        if (dW == 0) {
            G32(VA_g_floorceil_accum_b + 0x10) = 0;
            G32(VA_g_clip_output_vertex_count + 0xc) = W0;
            edx = W1;                                       /* edx unclobbered (= W1) */
        } else {
            int32_t grad = dW / ecx;
            edx = (uint32_t)(dW % ecx);                     /* edx = idiv remainder */
            G32(VA_g_floorceil_accum_b + 0x10) = (uint32_t)grad;
            int32_t bias = (mode == 2) ? 0 : ((mode & 1) ? (grad >> 1) : -(grad >> 1));
            G32(VA_g_clip_output_vertex_count + 0xc) = (uint32_t)(bias + (int32_t)W0);
        }
        uint32_t U0 = P16(esi + 8);                         /* texB0 (U0) */
        uint32_t V0 = P16(esi + 0xa);                       /* texC0 (V0) */
        edx   = (edx & 0xffff0000u) | P16(esi + 0x14);      /* mov dx, U1 */
        uint32_t esi_v = (esi & 0xffff0000u) | P16(esi + 0x16); /* mov si, V1 (esi hi = edge-ptr hi) */
        G32(VA_g_floorceil_step_b + 0x10) = edx;
        G32(VA_g_floorceil_step_b + 0x14) = esi_v;
        /* U gradient (texB) */
        int32_t dU = (int32_t)(uint16_t)edx - (int32_t)U0;
        if (dU == 0) {
            G32(VA_g_floorceil_step_a) = 0;
            G32(VA_g_floorceil_accum_a) = U0 << 8;
        } else {
            int32_t grad = (int32_t)((uint32_t)dU << 8) / ecx;
            G32(VA_g_floorceil_step_a) = (uint32_t)grad;
            int32_t bias = (mode == 2) ? 0 : ((mode & 1) ? (grad >> 1) : -(grad >> 1));
            G32(VA_g_floorceil_accum_a) = (uint32_t)(bias + (int32_t)(U0 << 8));
        }
        /* V gradient (texC) -> edx_vacc (accum) + esi_grad (gradient) registers for 0x3badf */
        uint32_t edx_vacc, esi_grad;
        int32_t dV = (int32_t)(uint16_t)esi_v - (int32_t)V0;
        if (dV == 0) {
            esi_grad = 0;
            edx_vacc = V0 << 8;
        } else {
            int32_t grad = (int32_t)((uint32_t)dV << 8) / ecx;
            esi_grad = (uint32_t)grad;
            if (mode == 2) edx_vacc = V0 << 8;
            else { int32_t bias = (mode & 1) ? (grad >> 1) : -(grad >> 1); edx_vacc = (uint32_t)(bias + (int32_t)(V0 << 8)); }
        }
        if (mode == 2) {
            write_floorceil_span_texcoords(edi, ebp, cx, edx_vacc, esi_grad);
        } else if (mode & 1) {
            uint32_t e = write_floorceil_span_texcoords(edi, ebp, cx, edx_vacc, esi_grad) - ebp;
            P16(e + 4) = (uint16_t)G32(VA_g_floorceil_step_b + 0x10);            /* U1 */
            P16(e + 6) = (uint16_t)G32(VA_g_floorceil_step_b + 0x14);            /* V1 */
            P32(e + 8) = G32(VA_g_floorceil_step_b + 0x28);                      /* W1 */
        } else {
            write_floorceil_span_texcoords(edi, ebp, cx, edx_vacc, esi_grad);
            P16(edi + 4) = (uint16_t)U0;
            P16(edi + 6) = (uint16_t)V0;
            P32(edi + 8) = G32(VA_g_floorceil_step_b + 0x2c);                    /* W0 */
        }
    }
}
#undef P16
#undef P32

#define P16(a) (*(volatile uint16_t *)(uintptr_t)(a))
#define P32(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* PEEL 5: floor/ceil polygon clipper (0x3b4a2 subtree) — Sutherland-Hodgman clip of the projected
 * vertex ring against the view window. Buffers 0x8c944 / 0x8cb28 (count word @+0, then stride-0xc
 * vertices: +0 X/+2 Y/+4 W(depth)/+6 shade/+8 U/+0xa V). This is the LAST call_orig bridge in the
 * floor/ceiling renderer. */

/* simple per-field linear interpolation at the crossing: out = a + (a-b)*cx/bp (16x16->32 imul, /bp). */
static int16_t clip_lerp(int16_t a, int16_t b, int16_t cx, int16_t bp)
{
    int16_t d = (int16_t)(a - b);
    if (d != 0) d = (int16_t)(((int32_t)d * (int32_t)cx) / (int32_t)bp);   /* imul cx; idiv bp */
    return (int16_t)(d + a);                                              /* add ax,[esi] */
}

/* 0x3b5d2: write the interpolated crossing vertex from esi (toward ebx) into [edi]; advance edi+0xc,
 * count++. X/Y always; W/shade/U/V gated by 0x90a22(word/byte)/0x90a23. The W field uses near-plane
 * clamping (clamps [ebx+4] IN PLACE) + a precision-reduce loop that MUTATES cx/bp for shade/U/V. */
static uint32_t clip_interp_vertex(uint32_t esi, uint32_t ebx, uint32_t edi, int16_t cx, int16_t bp)
{
    P16(edi + 0) = (uint16_t)clip_lerp((int16_t)P16(esi + 0), (int16_t)P16(ebx + 0), cx, bp);   /* X */
    P16(edi + 2) = (uint16_t)clip_lerp((int16_t)P16(esi + 2), (int16_t)P16(ebx + 2), cx, bp);   /* Y */
    if (G16(VA_g_render_textured_flag) != 0) {                                /* 0x3b5fe (word) */
        int16_t esiW = (int16_t)P16(esi + 4), ebxW = (int16_t)P16(ebx + 4);
        P16(edi + 4) = (uint16_t)esiW;                     /* default out.W = esi.W */
        if (esiW != ebxW) {                                /* 0x3b618 */
            G16(VA_g_floorceil_step_b + 0x3a) = (uint16_t)ebxW;                 /* orig ebx.W */
            int16_t cEbxW;
            if (ebxW < 16) { P16(ebx + 4) = 16; cEbxW = 16; }   /* near-clip clamp, IN PLACE */
            else cEbxW = ebxW;
            G16(VA_g_floorceil_step_b + 0x34) = (uint16_t)cEbxW;
            G16(VA_g_floorceil_step_b + 0x38) = (uint16_t)esiW;
            int16_t cEsiW = (esiW < 16) ? 16 : esiW;
            G16(VA_g_floorceil_step_b + 0x30) = (uint16_t)cEsiW;
            if (cEsiW != cEbxW) {                          /* 0x3b65d */
                /* dword reads pick up the word-written value | stale hi-16 from shared obj3 */
                int32_t eax = (int32_t)((uint32_t)((int32_t)bp + (int32_t)cx) * G32(VA_g_floorceil_step_b + 0x34));   /* (bp+cx)*cEbxW */
                int32_t ecx = (int32_t)((uint32_t)(int32_t)cx * G32(VA_g_floorceil_step_b + 0x30));                   /* cx*cEsiW */
                eax = (int32_t)((uint32_t)eax - (uint32_t)ecx);
                while (eax < -32767 || ecx < -32767 || eax > 32767 || ecx >= 32767) { ecx >>= 1; eax >>= 1; }
                bp = (int16_t)eax;                         /* mov ebp,eax (reduced denom) */
                cx = (int16_t)ecx;                         /* reduced numerator (for W + shade/U/V) */
                int16_t d = (int16_t)(cEsiW - cEbxW);
                int16_t q = (int16_t)(((int32_t)d * (int32_t)cx) / (int32_t)bp);
                P16(edi + 4) = (uint16_t)(q + cEsiW);
            }
        }
    }
    if (G8(VA_g_render_textured_flag) != 0)                                  /* 0x3b6c0 (byte) */
        P16(edi + 6) = (uint16_t)clip_lerp((int16_t)P16(esi + 6), (int16_t)P16(ebx + 6), cx, bp);  /* shade */
    if (G8(VA_g_render_textured_flag + 0x1) != 0) {                                /* 0x3b6e1 (byte) */
        P16(edi + 8)   = (uint16_t)clip_lerp((int16_t)P16(esi + 8),   (int16_t)P16(ebx + 8),   cx, bp);  /* U */
        P16(edi + 0xa) = (uint16_t)clip_lerp((int16_t)P16(esi + 0xa), (int16_t)P16(ebx + 0xa), cx, bp);  /* V */
    }
    edi += 0xc;                                            /* 0x3b71a */
    G32(VA_g_clip_output_vertex_count)++;                                        /* inc [0x8a3cc] (out count) */
    return edi;
}

/* 0x3b5a8: prev OUTSIDE, curr INSIDE -> entering crossing (interpolate from PREV vertex, clamp curr.W) */
static uint32_t clip_emit_enter(uint32_t esi_curr, uint32_t edi, int16_t curr_coord, int16_t prev_coord)
{
    int16_t boundary = (int16_t)G16(VA_g_clip_boundary);
    int16_t cx = (int16_t)(prev_coord - boundary);
    int16_t bp = (int16_t)(curr_coord - prev_coord);       /* sub ebp,eax; neg -> curr-prev */
    return clip_interp_vertex(esi_curr - 0xc, esi_curr, edi, cx, bp);   /* esi=prev, ebx=curr */
}

/* 0x3b5c2: prev INSIDE, curr OUTSIDE -> leaving crossing (interpolate from CURR vertex, clamp prev.W) */
static uint32_t clip_emit_leave(uint32_t esi_curr, uint32_t edi, int16_t curr_coord, int16_t prev_coord)
{
    int16_t boundary = (int16_t)G16(VA_g_clip_boundary);
    int16_t cx = (int16_t)(curr_coord - boundary);
    int16_t bp = (int16_t)(prev_coord - curr_coord);       /* sub ebp,eax -> prev-curr */
    return clip_interp_vertex(esi_curr, esi_curr - 0xc, edi, cx, bp);   /* esi=curr, ebx=prev */
}

/* 0x3b506: clip the vertex list against ONE boundary plane. ax=boundary; bh=axis/dir byte (bit7=inside-
 * is-greater-side, bit0=use-Y-coord), bl=prev-side seed (0xff). Ping-pongs 0x8c944<->0x8cb28 by bh sign.
 * Returns out vertex count; *empty iff count==0 (the caller's ZF). */
static uint32_t clip_one_boundary(uint16_t ax_boundary, uint16_t bx, int *empty)
{
    uint8_t bh = (uint8_t)(bx >> 8), bl = (uint8_t)bx;
    G32(VA_g_clip_output_vertex_count) = 0;
    G16(VA_g_clip_boundary) = ax_boundary;
    uint32_t edi, esi;
    if (bh & 0x80) { esi = 0x8c944u + OBJ_DELTA; edi = 0x8cb28u + OBJ_DELTA; }   /* js: no swap */
    else           { edi = 0x8c944u + OBJ_DELTA; esi = 0x8cb28u + OBJ_DELTA; }   /* xchg edi,esi */
    uint32_t out_base = edi;                               /* push edi */
    P16(edi) = ax_boundary;                                /* mov [edi],ax (temp in count slot) */
    edi += 2;
    uint16_t cx = P16(esi);                                /* in count */
    esi += 2;
    int16_t boundary = (int16_t)ax_boundary;
    int16_t prev_coord = 0;
    do {                                                   /* 0x3b535 */
        int16_t coord = (bh & 1) ? (int16_t)P16(esi + 2) : (int16_t)P16(esi);   /* Y or X */
        int inside = (coord == boundary) ? 1 : ((coord > boundary) ? ((bh & 0x80) != 0) : ((bh & 0x80) == 0));
        if (inside) {
            if (bl == 1) edi = clip_emit_enter(esi, edi, coord, prev_coord);     /* prev outside */
            prev_coord = coord;
            if ((uint8_t)cx != 0) {                        /* or cl,cl; je (skip copy on closing vtx) */
                bl = 0;
                P32(edi + 0) = P32(esi + 0); P32(edi + 4) = P32(esi + 4); P32(edi + 8) = P32(esi + 8);
                esi += 0xc; edi += 0xc;
                G32(VA_g_clip_output_vertex_count)++;
            }
        } else {
            if ((bl & 1) == 0) edi = clip_emit_leave(esi, edi, coord, prev_coord);   /* prev inside */
            bl = 1;
            prev_coord = coord;
            esi += 0xc;
        }
    } while ((int16_t)(--cx) >= 0);                        /* dec cx; jge */
    uint32_t count = G32(VA_g_clip_output_vertex_count);
    if (count == 0) { *empty = 1; return count; }          /* or eax,eax; je (ZF=1) */
    *empty = 0;
    P16(out_base) = (uint16_t)count;                       /* out[0] = count */
    uint32_t v0 = out_base + 2;
    P32(edi + 0) = P32(v0 + 0); P32(edi + 4) = P32(v0 + 4); P32(edi + 8) = P32(v0 + 8);   /* close the ring */
    return count;
}

/* 0x3b4a2: clip against up to 4 view-window edges (right/left then bottom/top), each via clip_one_boundary;
 * the clip flags g_8a434 (lo byte=X, hi byte=Y) gate which axes run. Returns leftover EAX (= out count,
 * hi-16=0 -> 0x8a3bc); *out_empty = ZF (clip produced empty polygon -> edge-walker bails). */
uint32_t build_floorceil_clip_edges(int *out_empty)
{
    uint32_t eax = 0;
    int empty = 0;
    if (G16(VA_g_floorceil_clip_flags) & 0x00ff) {                           /* X clip */
        eax = clip_one_boundary(G16(VA_g_view_bound_left), 0x80ff, &empty);   /* right */
        if (empty) { *out_empty = 1; return eax; }
        eax = clip_one_boundary(G16(VA_g_view_bound_right), 0x00ff, &empty);   /* left */
        if (empty) { *out_empty = 1; return eax; }
    }
    if (G16(VA_g_floorceil_clip_flags) & 0xff00) {                           /* Y clip */
        eax = clip_one_boundary(G16(VA_g_view_bound_top), 0x81ff, &empty);   /* bottom */
        if (empty) { *out_empty = 1; return eax; }
        eax = clip_one_boundary(G16(VA_g_view_bound_bottom), 0x01ff, &empty);   /* top */
        *out_empty = empty;                                /* 0x3b4fc: ZF = top's empty */
    } else {
        *out_empty = (G16(VA_g_floorceil_vertex_count) == 0) ? 1 : 0;          /* 0x3b4fd: cmp [0x8c944],0 */
    }
    return eax;
}
#undef P16
#undef P32

int rasterize_floorceil_polygon(uint32_t esi_geom, uint16_t gs_sel, uint16_t es_sel, uint16_t fs_sel)
{
    #define SG16(o) (*(volatile int16_t *)(uintptr_t)(esi_geom + (uint32_t)(o)))
    #define RL16(a) (*(volatile int16_t *)(uintptr_t)(a))
    (void)gs_sel; (void)es_sel; (void)fs_sel;   /* all callees peeled to pure-DS C; selectors no longer used */

    /* --- clip-shade param setup (0x3b1c1) --- */
    G16(VA_g_floorceil_depth_clip_bias) = 8;
    if ((uint8_t)G8(VA_g_column_clip_mode) != 0) {
        int16_t  axv  = (int16_t)((uint16_t)(uint8_t)G8(VA_g_column_clip_mode) - 0x80u);   /* sub ax,0x80 */
        uint32_t ebx  = (uint16_t)axv;                                       /* mov ebx,eax (hi=0) */
        int16_t  bxp4 = (int16_t)((uint16_t)ebx + 4u);                       /* add bx,4 (16-bit) */
        ebx = (bxp4 < 0) ? 0u : (uint16_t)bxp4;                              /* jns / sub ebx,ebx */
        G16(VA_g_floorceil_depth_clip_bias) = (uint16_t)((uint16_t)G16(VA_g_floorceil_depth_clip_bias) + (uint16_t)axv);   /* add [0x90a1e],ax */
        uint16_t bxs = (uint16_t)((uint16_t)ebx >> 2);                       /* shr bx,2 */
        G16(VA_g_floorceil_clip_scale) = (uint16_t)(-((int32_t)bxs - 0x20));                   /* sub ebx,0x20; neg; [0x90a20]=bx */
    }
    G8(VA_g_sprite_render_mode)  = 0;
    G16(VA_g_floorceil_edge_emitted) = 0;

    /* --- bbox cull vs view window (0x3b209). +0x28/+0x2c/+0x2a/+0x2e = surface screen bbox. --- */
    int16_t ax = SG16(0x28), bx = SG16(0x2c), cx = SG16(0x2a), dx = SG16(0x2e);
    int16_t v68 = (int16_t)G16(VA_g_view_bound_right), v6a = (int16_t)G16(VA_g_view_bound_left),
            v6c = (int16_t)G16(VA_g_view_bound_bottom), v6e = (int16_t)G16(VA_g_view_bound_top);
    if (ax >= v68 || bx < v6a || cx > v6c || dx <= v6e) return 1;            /* 0x3b268: fully outside */

    uint32_t last_eax;          /* the project/clip bridge's leftover EAX -> 0x8a3bc high word (0x3b2d2) */
    if (ax < v6a || bx > v68 || cx < v6e || dx > v6c) {
        /* partial (0x3b26b): clip flags g_8a434, project, clip */
        uint16_t bp = 0;
        if (!(ax >= v6a && bx < v68)) bp |= 0x00ffu;
        if (!(cx >= v6e && dx < v6c)) bp |= 0xff00u;
        G16(VA_g_floorceil_clip_flags) = bp;
        build_floorceil_vertex_records(esi_geom);                                  /* 0x3b2a1 project PEELED */
        int clip_empty = 0;
        last_eax = build_floorceil_clip_edges(&clip_empty);                         /* 0x3b2a6 clip PEELED */
        if (clip_empty) return 1;                       /* ZF set -> clip produced empty */
    } else {
        last_eax = build_floorceil_vertex_records(esi_geom);                        /* 0x3b261 project PEELED */
    }

    /* --- edge-walk loop (0x3b2b2): walk each projected edge into the run-list --- */
    uint32_t ep     = 0x8c946u + OBJ_DELTA;             /* esi = projected edge array (stride 0xc) */
    uint8_t  edge_n = (uint8_t)G16(VA_g_floorceil_vertex_count);            /* loop uses CL (low byte) */
    int16_t  y_first = RL16(ep + 2);
    G16(VA_g_view_offset_y + 0x14) = (uint16_t)y_first;                   /* minY */
    G16(VA_g_view_offset_y + 0x16) = (uint16_t)y_first;                   /* maxY */
    G32(VA_g_sprite_span_shade + 0x2) = (last_eax & 0xffff0000u) | (uint16_t)(*(volatile uint16_t *)(uintptr_t)(ep + 4));
        /* 0x3b2ce-d2: mov ax,[esi+4]; mov [0x8a3bc],eax — hi16 = the bridge's leftover eax, lo16 = word[ep+4]. */

    for (uint32_t ei = 0; ei < edge_n; ei++, ep += 0xc) {
        int16_t  X1 = RL16(ep + 0), Y1 = RL16(ep + 2);
        int16_t  X2 = RL16(ep + 0xc), Y2 = RL16(ep + 0xe);
#ifdef ROTH_STANDALONE
        /* SAFETY (imgfree): g_floorceil_span_records holds 642 records (0x3c30/0x18), indexed by the
         * edge scanline Y. A floor/ceiling edge projected PAST that range (seen in reflection
         * subpasses at Y up to ~790) walks the span write off the buffer end and clobbers the globals
         * that follow it in memory — the view bounds (0x9096c), the render-target/framebuffer_ptr
         * (0x90a98), player position — causing cascading corruption + SIGSEGV (root-caused via
         * ROTH_CRASH_DIAG + the fbwatch HW-watchpoint: write_ror_ramp over g_framebuffer_ptr). Those
         * out-of-buffer scanlines are off-screen (invisible), so skipping the edge is loss-free.
         * Trap host / oracle are byte-identical (this guard is standalone-only); the upstream reason
         * the surface isn't culled (view-bound / bbox projection) is under separate investigation. */
        {
            int16_t ylo = Y1 < Y2 ? Y1 : Y2, yhi = Y1 < Y2 ? Y2 : Y1;
            if (ylo < 0 || yhi > 641) continue;
        }
#endif
        uint32_t edi = 0x8cd0cu + OBJ_DELTA + (uint32_t)(uint16_t)Y1 * 0x18u;
        int32_t  height, ebp_stride;

        if (Y1 > Y2) {                                  /* descending edge -> left column, edi-- */
            if ((int16_t)G16(VA_g_view_offset_y + 0x14) > Y2) G16(VA_g_view_offset_y + 0x14) = (uint16_t)Y2;
            if ((int16_t)G16(VA_g_view_offset_y + 0x16) < Y1) G16(VA_g_view_offset_y + 0x16) = (uint16_t)Y1;
            height = (int32_t)Y1 - (int32_t)Y2;
            ebp_stride = -0x18;
        } else {                                        /* ascending edge */
            if ((int16_t)G16(VA_g_view_offset_y + 0x14) > Y1) G16(VA_g_view_offset_y + 0x14) = (uint16_t)Y1;
            if ((int16_t)G16(VA_g_view_offset_y + 0x16) <= Y2) G16(VA_g_view_offset_y + 0x16) = (uint16_t)Y2;
            height = (int32_t)(int16_t)(Y2 - Y1);
            if (height == 0) {                          /* horizontal edge (0x3b349) */
                if (G16(VA_g_floorceil_edge_emitted) != 0) continue;
                int16_t left = X1, right = X2;          /* cmp bx,dx; jns (bx>=dx no swap); else xchg */
                if (X1 < X2) { left = X2; right = X1; }
                RL16(edi + 0)   = left;
                RL16(edi + 0xc) = right;
                G16(VA_g_floorceil_edge_emitted) = 0xffff;
                if (G16(VA_g_render_textured_flag) != 0)
                    store_floorceil_flat_edge_texcoords(ep, edi);  /* 0x3b37c PEELED (incl. 0x3b724) */
                continue;
            }
            edi += 0xc;                                 /* 0x3b386: right column, edi++ */
            ebp_stride = 0x18;
        }
        /* common (0x3b38e) */
        G16(VA_g_floorceil_edge_x_end) = (uint16_t)X2;
        G16(VA_g_floorceil_edge_x_start) = (uint16_t)X1;
        int16_t deltaX = (int16_t)(X2 - X1);
        int32_t absdx  = deltaX < 0 ? -(int32_t)deltaX : (int32_t)deltaX;
        int32_t slope, acc;

        if (absdx > height) {                           /* x-major (0x3b3af) */
            slope = ((int32_t)deltaX << 16) / height;                       /* shl;cdq;idiv */
            acc   = (int32_t)((uint32_t)(uint16_t)X1 << 16);                 /* shl ebx,0x10 */
            uint16_t xord = (uint16_t)((uint16_t)((uint32_t)slope >> 16) ^ (uint16_t)ebp_stride);
            if ((int16_t)xord < 0) {                    /* 0x3b3c8 (jns false) */
                if (G16(VA_g_render_textured_flag) != 0) { G16(VA_g_floorceil_edge_orientation) = 0;
                    interp_floorceil_edge_texcoords((uint32_t)height, ep, edi, (uint32_t)ebp_stride); }  /* 0x3b3d6 PEELED */
                acc -= slope >> 1;                                          /* sar edx,1; sub ebx,edx */
                RL16(edi) = (int16_t)G16(VA_g_floorceil_edge_x_start);                          /* [edi]=X1 (first) */
                edi += (uint32_t)ebp_stride; acc += slope;
                int32_t c = (uint16_t)height;                               /* movzx ecx,cx */
                do { RL16(edi) = (int16_t)((uint32_t)acc >> 16);
                     edi += (uint32_t)ebp_stride; acc += slope; } while (--c > 0);   /* 0x3b472 */
                G16(VA_g_floorceil_edge_emitted) = 0xffff;
                continue;
            } else {                                    /* 0x3b3fd */
                if (G16(VA_g_render_textured_flag) != 0) { G16(VA_g_floorceil_edge_orientation) = 1;
                    interp_floorceil_edge_texcoords((uint32_t)height, ep, edi, (uint32_t)ebp_stride); }  /* 0x3b40f PEELED */
                acc += slope >> 1;                                          /* sar edx,1; add ebx,edx */
                int32_t c = height;
                do { RL16(edi) = (int16_t)((uint32_t)acc >> 16);            /* 0x3b41f */
                     edi += (uint32_t)ebp_stride; acc += slope; } while ((int16_t)(--c) > 0);  /* dec cx (16-bit) */
                G16(VA_g_floorceil_edge_emitted) = 0xffff;
                RL16(edi) = (int16_t)G16(VA_g_floorceil_edge_x_end);                          /* [edi]=X2 (last) */
                continue;
            }
        } else {                                        /* y-major (0x3b444) */
            if (G16(VA_g_render_textured_flag) != 0) { G16(VA_g_floorceil_edge_orientation) = 2;
                interp_floorceil_edge_texcoords((uint32_t)height, ep, edi, (uint32_t)ebp_stride); }  /* 0x3b452 PEELED */
            int32_t c = (int32_t)(uint16_t)height + 1;                      /* and ecx,0xffff; inc ecx */
            slope = ((int32_t)deltaX << 16) / c;                            /* shl;cdq;idiv */
            acc   = (int32_t)((uint32_t)(uint16_t)X1 << 16) + 0x7fff;       /* shl ebx,0x10; add 0x7fff */
            do { RL16(edi) = (int16_t)((uint32_t)acc >> 16);                /* 0x3b472 */
                 edi += (uint32_t)ebp_stride; acc += slope; } while (--c > 0);
            G16(VA_g_floorceil_edge_emitted) = 0xffff;
            continue;
        }
    }
    return (G16(VA_g_floorceil_edge_emitted) == 0) ? 1 : 0;                 /* 0x3b496: cmp [0x8a440],0; ret (ZF=empty) */
    #undef SG16
    #undef RL16
}

/* classify_surface_floorceil (0x38b54): the floor/ceil-vs-wall dispatch helper called by
 * rasterize_world_spans_scanline at 0x36a1e. It ALWAYS returns al=0 (both branches
 * end `xor al,al; ret`); its real job is to PROJECT the surface's quad into the renderer working set
 * [0x90958..0x90966]. ESI = a host pointer to the surface record (DS-near); the geometry base [esi+0x30] and
 * the 4 vertex indices [esi+0x36/+0x38/+0x3a/+0x3c] are host pointers/offsets — dereffed DIRECTLY (the
 * stored-pointer-is-host-address rule), while the [0x9095x] working set + [0x8a29c]/[0x8a318] are FIXED canon
 * globals (G-macros). When [0x90a26]!=0 it also stashes geom ptr -> [0x8a29c] and surf+0x18 -> [0x8a318].
 * The 8 dest words map: {v0.x@+0xc, v0.y@+0xe, v3.y@+0xe, v3.?@+0xa, v1.x@+0xc, v1.y@+0xe, v2.y@+0xe, v2.?@+0xa}
 * where v0=idx[+0x36], v1=idx[+0x38], v2=idx[+0x3a], v3=idx[+0x3c]. (ABI_SECLIST: esi-in, obj3-out, void.) */
uint32_t classify_surface_floorceil(uint32_t esi)
{
    #define HP16(p) (*(volatile uint16_t *)(uintptr_t)(p))
    #define HP32(p) (*(volatile uint32_t *)(uintptr_t)(p))
    if (G8(VA_g_sprite_fill_index + 0x2) != 0) {                               /* 0x38b5d: jne 0x38bca */
        G32(VA_g_pool_check_enabled + 0x24) = (int32_t)esi;                      /* 0x38bcf: [0x8a29c]=esi (geom-owning surf rec) */
        G32(VA_g_span_round_half + 0x2) = (int32_t)HP32(esi + 0x18);         /* 0x38bd5: [0x8a318]=surf+0x18 (shade word) */
    }
    uint32_t i0 = HP16(esi + 0x36), i1 = HP16(esi + 0x38);    /* vertex indices */
    uint32_t i2 = HP16(esi + 0x3a), i3 = HP16(esi + 0x3c);
    uint32_t geom = HP32(esi + 0x30);                        /* geometry base (host ptr) */
    G16(VA_g_wall_proj_y3 + 0xa) = HP16(geom + i0 + 0xc);                    /* v0.x */
    G16(VA_g_wall_proj_y3 + 0xc) = HP16(geom + i0 + 0xe);                    /* v0.y */
    G16(VA_g_wall_proj_y3 + 0xe) = HP16(geom + i3 + 0xe);                    /* v3.y */
    G16(VA_g_wall_proj_y3 + 0x10) = HP16(geom + i3 + 0xa);                    /* v3.? */
    G16(VA_g_wall_proj_y3 + 0x12) = HP16(geom + i1 + 0xc);                    /* v1.x */
    G16(VA_g_wall_proj_y3 + 0x14) = HP16(geom + i1 + 0xe);                    /* v1.y */
    G16(VA_g_wall_proj_y3 + 0x16) = HP16(geom + i2 + 0xe);                    /* v2.y */
    G16(VA_g_wall_proj_y3 + 0x18) = HP16(geom + i2 + 0xa);                    /* v2.? */
    return 0;                                               /* xor al,al */
    #undef HP16
    #undef HP32
}

/* ===================== Batch 58 (WALL span driver, 0x36b39) — WIP =====================
 * draw_world_surface_spans — the VERTICAL wall-column rasterizer (3356B, the 3rd SMC span driver,
 * tail-jmped from rasterize_world_spans_scanline 0x366cb alongside the sprite 0x39610 + floorceil 0x3a84e
 * drivers). Bbox-culls the wall vs the view window, projects the 4 wall-edge corners to screen rows
 * (project_wall_edge_y 0x38c46 x4 -> g_wall_proj_y0..3), builds the vertical texcoord-endpoint steppers,
 * selects a column mapper from the 0x71f80 pair table into g_world_span_fn (0x8a2bc), then per-column:
 * render_parallax_sky_columns 0x38d6c / compute_wall_column_source_offset 0x378dc -> dispatch_world_span_column
 * 0x3778b (-> jmp the mapper). It reads its own gs/es/fs selectors from globals (0x8a2a8/0x90c06/0x909b0).
 *   FULLY NATIVE: the driver body, all 24 column mappers + the dispatcher (0x3778b), the subpass
 *   deferred-surface write-out (EXIT F, 0x37a65/0x37b8d), AND both texture-resolve call sites (now call the
 *   native render_parallax_sky_columns 0x38d6c) — verified byte-exact by the in-process differential
 *   (ROTH_LIFT_DIFF=draw_world_surface_spans, incl. ..._FB=1). No call_orig left in the path. ecx_entry=entry ECX. */
/* DIAGNOSTIC: retained for the host's recursive-fault dumper (traps.c reads it). Now always 0 since the wall
 * driver is bridge-free; kept so the dumper's read site still compiles. */
volatile int g_wd_dbg_phase = 0;

/* PEELED: project_wall_edge_y (0x38c46) — native C (was a call_orig bridge). AX=worldY, CX=clamp, DX=offset
 * -> AX screen row. Clamps to the frustum [0x9096e..0x9096c], |AX-0x90992|*0x90996>>8, max vs CX, +DX, >>5,
 * -bias(0x90a1e) -SMC(0x38c94 read LIVE, per-frame-patched like the floor/ceil projector's 0x3b794), then
 * classify: <=0 -> 0x100; clamp to 0x90a20; >=0x1f -> 0x1fff (+inc 0x8a350); else (lo<<8)|((dl<<3)&0xff). */
static uint16_t wd_project(uint16_t ax_in, uint16_t cx, uint16_t dx)
{
    int16_t ax = (int16_t)ax_in;
    if (ax >= (int16_t)G16(VA_g_view_bound_bottom)) ax = (int16_t)G16(VA_g_view_bound_bottom);   /* min vs 0x9096c */
    if (ax <= (int16_t)G16(VA_g_view_bound_top)) ax = (int16_t)G16(VA_g_view_bound_top);   /* max vs 0x9096e */
    int16_t  t   = (int16_t)(ax - (int16_t)G16(VA_g_span_src_wrap_reoffset + 0x16));
    uint32_t a   = (uint32_t)(uint16_t)(t < 0 ? -t : t);           /* |t| */
    uint16_t axp = (uint16_t)((a * (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x1a)) >> 8);  /* mul; al=ah; ah=dl (bits 8-23) */
    if (axp <= cx) axp = cx;                                       /* ja skip; else max vs CX (unsigned) */
    uint32_t eax = (uint32_t)axp + (uint32_t)dx;
    uint8_t  dl  = (uint8_t)eax;
    /* shr/shl counts are SMC @0x38c8a/@0x38cb3 (0x2d6a8-patched per shade level) — read LIVE,
     * like the bias below (the bias was already live; the shifts were hardcoded to level-0). */
    uint8_t  shr_n = (uint8_t)(*(volatile uint8_t *)GADDR(VA_g_shade_shift_count_default) & 0x1f);
    uint8_t  shl_n = (uint8_t)(*(volatile uint8_t *)GADDR(0x38cb3) & 0x1f);
    int16_t  s   = (int16_t)((uint16_t)eax >> shr_n);              /* shr ax,imm@0x38c8a (16-bit) */
    s = (int16_t)(s - (int16_t)G16(VA_g_floorceil_depth_clip_bias));
    s = (int16_t)(s - (int16_t)G16(VA_g_wd_project_bias_default));                      /* sub ax,imm (SMC @0x38c94, live) */
    if (s <= 0) return 0x100;                                      /* jle 0x38cb5 */
    if (s >= (int16_t)G16(VA_g_floorceil_clip_scale)) s = (int16_t)G16(VA_g_floorceil_clip_scale);     /* min vs 0x90a20 */
    if (s >= 0x1f) { G8(VA_g_span_draw_mode_flags + 0x4) = (uint8_t)(G8(VA_g_span_draw_mode_flags + 0x4) + 1); return 0x1fff; }   /* jge 0x38cbb */
    return (uint16_t)(((uint16_t)(uint8_t)s << 8) | (uint8_t)((uint8_t)dl << shl_n));
}

/* PEELED: compute_wall_column_source_offset (0x378dc) — the CLIPPED-column source/step/shade calc with
 * vertical clipping. Returns 1 to SKIP the column (CF set), else 0 + the dispatch regs via out-params
 * (the inputs dispatch_world_span_column consumes). esi starts 0 (per-column src offset). */
int compute_wall_column_source_offset(uint32_t edi_in,
        uint32_t *o_esi, uint32_t *o_edi, uint32_t *o_ecx, uint32_t *o_edx, uint32_t *o_eax)
{
    uint32_t esi = 0, edi = edi_in, ecx = 0, edx = 0, eax = 0;
    int32_t Vint = (int16_t)G16(VA_g_span_texV_accum + 0x2), Uint = (int16_t)G16(VA_g_span_texU_accum + 0x2);
    if (G8(VA_g_world_surface_draw_flags) & 8) {                                       /* perspective (0x378e9) */
        uint32_t a  = (uint32_t)G32(VA_g_span_texV_accum + 0x18) / (uint32_t)G32(VA_g_span_texV_accum + 0x10);  /* x86 `div` = UNSIGNED (8a308 hi bit can be set) */
        uint16_t ax = (uint16_t)((uint16_t)a + (uint16_t)G16(VA_g_span_texV_accum + 0x4));
        ax = (uint16_t)(ax >> 1);
        if (G8(VA_g_world_surface_draw_flags) & 2) ax = (uint16_t)((uint16_t)(~ax) + (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0xe));
        ax = (uint16_t)(ax & (uint16_t)G16(VA_g_column_clip_mode + 0x4));
        ax = (uint16_t)(ax * (uint16_t)G16(VA_g_span_src_row_width));
        esi += ax; G32(VA_g_span_src_wrap_base) = esi; esi += G32(VA_g_span_src_wrap_base + 0x4);
        int16_t top = (int16_t)(Vint - (int16_t)G16(VA_g_view_bound_bottom));   /* Vint-0x9096c, >=0 -> top-clip */
        if (top >= 0) edx = (uint16_t)top;                       /* js skip (edx stays 0) */
        int32_t rc0 = Vint - Uint + 1;
        G16(VA_g_span_src_wrap_base + 0x8) = 0;
        int32_t clampedU = Uint;
        int16_t v6e = (int16_t)G16(VA_g_view_bound_top);
        if (v6e > Uint) {                                        /* cmp ax,bx; jge skip */
            int16_t bp = (int16_t)(v6e - (int16_t)Uint);
            G16(VA_g_span_src_wrap_base + 0x8) = (uint16_t)bp; edx += (uint16_t)bp; clampedU = v6e;
        }
        G8(VA_g_span_draw_mode_flags + 0x2) = (uint8_t)clampedU;
        edi += G32(VA_g_scanline_dest_offset_table + (uint32_t)(uint16_t)clampedU * 4u);
        int32_t rc = rc0 - (int32_t)edx;                         /* mov ebx,ecx; sub ecx,edx */
        if (rc <= 0) return 1;                                   /* jle -> stc; ret (skip) */
        ecx = (uint32_t)rc;
        uint32_t ebxd = (uint32_t)((uint16_t)rc0) * 2u - 1u;     /* (Vint-Uint+1)*2-1 [PRE-clip] */
        if (ebxd != 0) eax = G32(VA_g_wall_proj_y3 + 0x2) / ebxd;
        if (G16(VA_g_span_round_half) != 0x8000) {                            /* 0x379b4 shade via clampedU */
            int16_t bp = (int16_t)((int16_t)clampedU - (int16_t)G16(VA_g_span_src_wrap_reoffset + 0x28));
            int32_t sh = (int32_t)eax * (int32_t)bp - (int32_t)G32(VA_g_render_double_scanline_flag + 0x4);
            if (sh < 0) sh = 0;
            G16(VA_g_span_eax_accum_init) = (uint16_t)sh;
            esi = (esi & 0xffff0000u) | (uint16_t)((uint16_t)esi + (uint16_t)((uint32_t)sh >> 16));  /* add si,ax */
        } else if (G16(VA_g_span_src_wrap_base + 0x8) != 0) {                          /* 0x379e0 shade via 8a340 */
            uint32_t sh = eax * (uint16_t)G16(VA_g_span_src_wrap_base + 0x8);
            G16(VA_g_span_eax_accum_init) = (uint16_t)sh;
            esi += sh >> 16;                                     /* add esi,eax (32-bit) */
        } else {
            G16(VA_g_span_eax_accum_init) = 0;
        }
        edx = eax >> 16;
    } else {                                                     /* non-perspective clip (0x37a18) */
        int16_t v6c = (int16_t)G16(VA_g_view_bound_bottom);
        int32_t cxv = (v6c < Vint) ? (int32_t)v6c : Vint;        /* min(0x9096c, Vint) */
        edx = (uint32_t)(uint16_t)(int16_t)(Vint - Uint);
        int16_t v6e = (int16_t)G16(VA_g_view_bound_top);
        eax = (uint32_t)(uint16_t)v6e;                           /* 0x37a2e `mov ax,[0x9096e]` (orig leaves eax=v6e) */
        int32_t clampedU = (v6e > Uint) ? (int32_t)v6e : Uint;   /* max(0x9096e, Uint) */
        G8(VA_g_span_draw_mode_flags + 0x2) = (uint8_t)clampedU;
        int32_t rc = (int32_t)(int16_t)(cxv - clampedU);         /* sub cx,bx */
        if (rc < 0) return 1;                                    /* jl -> stc; ret */
        rc++;
        ecx = (uint32_t)rc;
        if ((uint16_t)rc != 0) edi += G32(VA_g_scanline_dest_offset_table + (uint32_t)(uint16_t)clampedU * 4u);
    }
    *o_esi = esi; *o_edi = edi; *o_ecx = ecx; *o_edx = edx; *o_eax = eax;
    return 0;
}

/* Selector bases for the peeled column mappers, resolved by the host and set at driver entry (the oracle
 * never calls the wall driver, so these stay 0 there). gs = shade colormap (sel 0x8a2a8); es = the 64K
 * translucency/blend LUT (sel 0x90be2 — the mappers SELF-LOAD it, so this is NOT the driver's 0x90c06 es);
 * fs = texture source (sel 0x909b0, for the x-flip mega-mapper 0x38964). Read in dispatch's do_mapper. */
static uint32_t g_wd_gs_base = 0, g_wd_es_base = 0, g_wd_fs_base = 0;

/* Set by the dispatcher's EXIT F (subpass deferred-surface write-out). In a subpass (g_world_render_
 * subpass_kind 0x90a48 != 0) the wall driver forces [0x90970]=0, so the FIRST column that reaches the
 * dispatch tail takes EXIT F (0x37a65), records ONE deferred-surface descriptor (or skips it), and the
 * original's `add esp,0xc; ret` unwinds out of the whole driver. The C column loop reproduces that by
 * breaking out the moment this flag is set. (Sprite analogue: the `terminate` field, sprite_secondary_
 * writeout 0x39ea5.) */
static int g_wd_terminate = 0;

/* EXIT F neighbor (anti-alias boundary) test, 0x37abc-0x37b87. Reached only from the single-texel path
 * (mapper 0x388be/0x39453) when the column's own source texel is transparent (0). Tests the 8 surrounding
 * texels (2 up, 2 down, 2 left, 2 right, each guarded by the texture-edge coords); returns 1 (record the
 * deferred surface) if ANY neighbor is opaque, 0 (skip — fully inside a transparent region) if all clear.
 * fs_base == the texture source base ([0x909b0]); esi == the (16-bit-masked) source texel offset. */
static int exit_f_neighbor_test(uint32_t esi, uint32_t fs_base)
{
    const volatile uint8_t *fb = (const volatile uint8_t *)(uintptr_t)fs_base;
    /* V coord (0x37abc): div [0x8a308]/[0x8a300]; +[0x8a2f4]; >>1; (flag2) ~ +[0x9098a]; &[0x90974] */
    uint16_t vcoord;
    {
        uint16_t a = (uint16_t)((uint32_t)G32(VA_g_span_texV_accum + 0x18) / (uint32_t)G32(VA_g_span_texV_accum + 0x10));
        a = (uint16_t)(a + (uint16_t)G16(VA_g_span_texV_accum + 0x4));
        a = (uint16_t)(a >> 1);
        if (G8(VA_g_world_surface_draw_flags) & 2) a = (uint16_t)((uint16_t)(~a) + (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0xe));
        vcoord = (uint16_t)(a & (uint16_t)G16(VA_g_column_clip_mode + 0x4));
    }
    /* U coord (0x37af0): (esi-[0x8a338]) low16 mod width; div word uses edx=0:ax */
    uint16_t width  = (uint16_t)G16(VA_g_span_src_row_width);
    uint16_t height = (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0xe);
    uint16_t ucoord = (uint16_t)((uint16_t)((uint32_t)esi - (uint32_t)G32(VA_g_span_src_wrap_base)) % width);

    uint16_t bx = vcoord;
    int32_t  edx = (int32_t)width;                              /* edx = width (row stride) */
    if (bx != 0) {                                             /* or bx,bx; je 0x37b34 */
        if (fb[(uint32_t)(esi + (uint32_t)(-edx))] != 0) return 1;          /* esi - width  (1 row up) */
        if (bx > 1) {                                                       /* cmp bx,1; jbe 0x37b34 */
            if (fb[(uint32_t)(esi + (uint32_t)(-edx * 2))] != 0) return 1;  /* esi - 2*width (2 up) */
        }
    }
    bx = (uint16_t)(bx + 1);                                   /* inc ebx */
    if (bx < height) {                                         /* cmp bx,bp; jae 0x37b4e */
        if (fb[(uint32_t)(esi + (uint32_t)edx)] != 0) return 1;             /* esi + width  (1 row down) */
        bx = (uint16_t)(bx + 1);
        if (bx < height) {
            if (fb[(uint32_t)(esi + (uint32_t)(edx * 2))] != 0) return 1;   /* esi + 2*width (2 down) */
        }
    }
    uint16_t ax = ucoord;
    if (ax != 0) {                                            /* or ax,ax; je 0x37b67 */
        if (fb[(uint32_t)(esi - 1)] != 0) return 1;                        /* esi - 1 (1 left) */
        if (ax > 1) {                                                      /* cmp ax,1; jbe 0x37b67 */
            if (fb[(uint32_t)(esi - 2)] != 0) return 1;                    /* esi - 2 (2 left) */
        }
    }
    ax = (uint16_t)(ax + 1);                                  /* inc eax */
    if (ax >= width) return 0;                                /* cmp ax,dx; jae 0x37c54 (skip) */
    if (fb[(uint32_t)(esi + 1)] != 0) return 1;                            /* esi + 1 (1 right) */
    ax = (uint16_t)(ax + 1);
    if (ax >= width) return 0;
    if (fb[(uint32_t)(esi + 2)] != 0) return 1;                            /* esi + 2 (2 right) */
    return 0;                                                  /* all clear -> skip */
}

/* EXIT F deferred-surface write-out, 0x37b8d-0x37c54. Fills the single deferred-surface descriptor
 * (0x90a49..0x90a68) consumed later by the subpass compositor. Mirror of sprite_secondary_writeout but
 * with the wall driver's perspective-correct U/V and the SMC step-shift byte at [0x38c8a]. esi == the
 * (possibly [0x8a338]-reloaded, masked-path) source texel offset. */
static void exit_f_writeout(uint32_t esi)
{
    G8(VA_g_world_render_subpass_kind + 0x1) = (uint8_t)G8(VA_g_world_render_subpass_kind);                         /* copy the pass kind */
    /* texV (0x37b97): same V math as the neighbor test */
    {
        uint16_t a = (uint16_t)((uint32_t)G32(VA_g_span_texV_accum + 0x18) / (uint32_t)G32(VA_g_span_texV_accum + 0x10));
        a = (uint16_t)(a + (uint16_t)G16(VA_g_span_texV_accum + 0x4));
        a = (uint16_t)(a >> 1);
        if (G8(VA_g_world_surface_draw_flags) & 2) a = (uint16_t)((uint16_t)(~a) + (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0xe));
        G16(VA_g_world_render_subpass_kind + 0x4) = (uint16_t)(a & (uint16_t)G16(VA_g_column_clip_mode + 0x4));
    }
    /* texU (0x37bcf): esi -= [0x8a338]; (low16) mod width */
    {
        uint16_t e16 = (uint16_t)((uint32_t)esi - (uint32_t)G32(VA_g_span_src_wrap_base));
        G16(VA_g_world_render_subpass_kind + 0x6) = (uint16_t)(e16 % (uint16_t)G16(VA_g_span_src_row_width));
    }
    G16(VA_g_world_render_subpass_kind + 0x2) = (uint16_t)(uint32_t)G32(VA_g_current_das_entry_id);
    G32(VA_g_subpass_surfrec_ref) = G32(VA_g_map_das_fat_buffer + 0x8);
    G32(VA_g_subpass_reflect_param_b + 0x2) = G32(VA_g_current_proc_tag + 0x118);
    G16(VA_g_subpass_surfrec_ref + 0x4) = (uint16_t)G16(VA_g_world_surface_draw_flags);
    G32(VA_g_subpass_surfrec_ref + 0x6) = G32(VA_g_map_das_fat_buffer + 0xc);
    G8(VA_g_subpass_reflect_param_b + 0x6)  = (uint8_t)G8(VA_g_turn_view_scale_state + 0x2);
    G16(VA_g_subpass_reflect_param_b + 0x8) = (uint16_t)((uint32_t)G32(VA_g_current_decoded_frame + 0x34) - 0x8b3f8u);
    /* step (0x37c36): edx:eax = (2*[0x8a304]) << [0x38c8a] (last-shifted-out bit -> edx); / [0x8a300] */
    {
        uint32_t val2 = (uint32_t)G32(VA_g_span_texV_accum + 0x14) * 2u;
        uint8_t  sh   = (uint8_t)(*(volatile uint8_t *)GADDR(VA_g_shade_shift_count_default) & 0x1f);   /* SMC shift count */
        uint32_t lo   = (sh == 0) ? val2 : (val2 << sh);
        uint32_t cf   = (sh == 0) ? 0u   : ((val2 >> (32 - sh)) & 1u);           /* adc edx,edx <- shl CF */
        uint64_t num  = ((uint64_t)cf << 32) | lo;
        G32(VA_g_subpass_persp_step) = (int32_t)(uint32_t)(num / (uint32_t)G32(VA_g_span_texV_accum + 0x10));
    }
}

/* patch_span_mapper_pitch (0x36464): SMC stride patcher. EAX (low 16 = scanline stride) is written as a
 * 32-bit immediate into ~97 span-mapper code sites — the `mov edx,STRIDE` / `add edi,STRIDE` operands of the
 * 24 column mappers. 72 sites get the stride (x1); 25 get 2*stride (x2, the double-step mappers, after the
 * `add eax,eax`). Pure code-image writer (every target is in obj1); writes go through +OBJ_DELTA. */
void patch_span_mapper_pitch(uint32_t eax)
{
    static const uint32_t g1[] = {  /* x1 stride sites */
        0x38e5c,0x38f3a,0x38ea6,0x38f8f,0x38f98,0x3867b,0x38607,0x38619,0x38622,0x37c93,
        0x37cb2,0x388f1,0x38926,0x39490,0x394bc,0x39511,0x3951a,0x37d2e,0x37d56,0x389a6,
        0x389cc,0x39569,0x39598,0x395f5,0x395fe,0x37dde,0x37e0c,0x37e60,0x37e88,0x38a58,
        0x38a84,0x38adc,0x38b10,0x380f9,0x380c3,0x38189,0x3815a,0x3873e,0x37f31,0x37f5e,
        0x381fc,0x38235,0x39118,0x3914d,0x391c0,0x391c9,0x38045,0x38012,0x3831c,0x382e1,
        0x39362,0x39329,0x39384,0x3938f,0x38396,0x38377,0x3841e,0x383f7,0x39422,0x393ed,
        0x39444,0x3944d,0x385b5,0x3856d,0x39289,0x39249,0x392ba,0x392c3,0x384dd,0x384a6,
        0x386a9,0x38803 };
    static const uint32_t g2[] = {  /* x2 stride sites (after add eax,eax) */
        0x38628,0x3892c,0x37cb8,0x394c2,0x37d5c,0x389d2,0x3959e,0x38a8a,0x38b16,0x37e12,
        0x37e8e,0x380ff,0x3818f,0x37f71,0x38248,0x39160,0x3804b,0x38322,0x39368,0x3839c,
        0x38424,0x39428,0x385bb,0x3928f,0x384e3 };
    eax &= 0xffffu;
    for (size_t i = 0; i < sizeof g1 / sizeof g1[0]; i++)
        *(volatile uint32_t *)(uintptr_t)(g1[i] + OBJ_DELTA) = eax;
    uint32_t eax2 = eax + eax;
    for (size_t i = 0; i < sizeof g2 / sizeof g2[0]; i++)
        *(volatile uint32_t *)(uintptr_t)(g2[i] + OBJ_DELTA) = eax2;
}

/* PEELED: dispatch_world_span_column (0x3778b) — the per-column dispatcher (was a call_orig bridge).
 * Computes the per-pixel vertical step ([0x8a2e0]), the fraction seed ([0x8a2dc] word / [0x8a2dd] byte) and
 * the shade index (ebx.bh), advances the texcoord endpoint accumulators ([0x8a2cc]+=[0x8a2d0],
 * [0x8a2d4]+=[0x8a2d8]), then tail-calls one of the installed column mappers ([0x8a2bc]/[0x8a2c0]/[0x8a2c4])
 * or the solid-fill tail 0x38697. io carries the incoming per-column regs (from wd_column); across all
 * exits eax/edx/esi/edi/ebp pass through (eax restored except EXIT D = solid fill, al<-[0x89f10]) — only
 * ebx/ecx and the mapper target vary. The [0x8a2bc..] fn-ptrs are flat (the original jmps through them),
 * so they're bridged as-is; 0x38697 is a canon code addr (+OBJ_DELTA). The [0x90a48] subpass exit (0x37a65)
 * is rare/unused in the host (mirror subpass not running) -> bridged via call_orig from the entry. */
void dispatch_world_span_column(regs_t *io)
{
    uint32_t eax_in = io->eax, ecx_in = io->ecx, edx_in = io->edx;
    uint32_t esi_in = io->esi, edi_in = io->edi, ebp_in = io->ebp, ebx_in = io->ebx;
    uint32_t eax = eax_in, ebx = ebx_in, ecx = ecx_in, edx = edx_in;
    uint32_t mapper_va; int mapper_canon = 0;

    if (G8(VA_g_column_clip_mode) == 0) goto L_3789b;          /* je 0x37898 -> 0x3789b (no pushes) */
    if (G8(VA_g_span_round_half + 0x6) == 0) goto L_37803;          /* je 0x37803 */
    {
        if (G8(VA_g_span_endpoint_b + 0x2) == G8(VA_g_span_endpoint_a + 0x2)) goto L_37851;          /* je 0x37851 (ecx still unmasked) */
        ecx = ecx_in & 0xffffu;                                /* and ecx,0xffff */
        int32_t q = (int32_t)(G32(VA_g_span_endpoint_b) - G32(VA_g_span_endpoint_a)) / (int32_t)ecx;   /* cdq; idiv ecx */
        G32(VA_g_span_pixel_step) = (uint32_t)q;
        if (q == 0) goto L_37851;                              /* je 0x37851 (ecx masked) */
        uint32_t cc = G32(VA_g_span_endpoint_a);
        G16(VA_g_span_accum_init) = (uint16_t)cc;                           /* mov [0x8a2dc],bx */
        ebx = cc >> 8;                                         /* shr ebx,8 (texel start addr) */
        G32(VA_g_span_endpoint_a) = cc + G32(VA_g_span_endpoint_a_colstep);
        G32(VA_g_span_endpoint_b) += G32(VA_g_span_endpoint_b_colstep);
        mapper_va = G32(VA_g_world_span_fn);                              /* jmp [0x8a2bc] -- EXIT A */
        goto do_mapper;
    }
L_37803:
    {
        uint64_t dd  = (uint64_t)G32(VA_g_span_texV_accum + 0x14) << 1;            /* add eax,eax; adc edx,edx (edx=0) */
        uint32_t q   = (uint32_t)(dd / (uint32_t)G32(VA_g_span_texV_accum + 0x10)); /* div [0x8a300], UNSIGNED divisor */
        uint8_t  cf  = (uint8_t)(q & 1u);                      /* shr eax,1 -> CF */
        uint32_t eaxq = q >> 1;
        G8(VA_g_span_shade_seed) = (uint8_t)~(uint8_t)(cf << 7);            /* sub dl,dl; rcr dl,1; not dl */
        edx = edx_in;                                          /* pop edx */
        ebx = 0;                                               /* sub ebx,ebx */
        int16_t ax = (int16_t)((uint16_t)eaxq - (uint16_t)G16(VA_g_floorceil_depth_clip_bias));   /* sub ax,[0x90a1e] */
        if (ax <= 0) goto L_3789a;                             /* jle 0x3789a */
        ebx = 0x1f00;                                          /* mov bh,0x1f */
        if (!(ax < (int16_t)G16(VA_g_floorceil_clip_scale))) ax = (int16_t)G16(VA_g_floorceil_clip_scale);     /* jl skip; else mov ax,[0x90a20] */
        if (ax > 0x1f) goto L_37883;                           /* jg 0x37883 */
        {
            uint8_t al = (uint8_t)((uint8_t)(uint16_t)ax - 0x1fu);         /* sub al,0x1f */
            ebx = (uint32_t)(uint8_t)(0x1fu + al) << 8;                    /* add bh,al (bl stays 0) */
        }
        goto L_3784a;                                          /* fall to 0x3784a -- EXIT B */
    }
L_37851:
    G8(VA_g_span_shade_seed) = 0;                                           /* mov byte [0x8a2dd],0 */
    ebx = (uint32_t)G8(VA_g_span_endpoint_a + 0x2) << 8;                         /* sub ebx,ebx; mov bh,[0x8a2ce] */
    G32(VA_g_span_endpoint_a) += G32(VA_g_span_endpoint_a_colstep);
    G32(VA_g_span_endpoint_b) += G32(VA_g_span_endpoint_b_colstep);
    edx = edx_in;                                             /* pop edx */
    if ((uint8_t)(ebx >> 8) >= 0x20) goto L_37883;            /* cmp bh,0x20; jge */
    mapper_va = G32(VA_g_world_span_fn_alt);                                 /* jmp [0x8a2c4] -- EXIT C */
    goto do_mapper;
L_37883:
    if (G8(VA_g_world_surface_draw_flags) & 1) goto L_3784a;                        /* test [0x9093c],1; jne 0x3784a */
    eax = (eax_in & 0xffff0000u) | (uint16_t)G16(VA_g_das_palette_remap_prefix);    /* pop eax; mov ax,[0x89f10] */
    mapper_va = 0x38697; mapper_canon = 1;                    /* jmp 0x38697 -- EXIT D (solid fill) */
    goto do_mapper;
L_3784a:
    mapper_va = G32(VA_g_world_span_fn);                                 /* pop eax; jmp [0x8a2bc] -- EXIT B */
    goto do_mapper;
L_3789a:                                                      /* pop eax (eax already = eax_in) */
L_3789b:
    if (G8(VA_g_world_render_subpass_kind) != 0) {                                   /* cmp [0x90a48],0; jne 0x37a65 (subpass EXIT F) */
        uint32_t esi    = esi_in & 0xffffu;                   /* 0x37a65 and esi,0xffff */
        uint32_t fs_base = g_wd_fs_base;
        uint32_t fn2    = (uint32_t)G32(VA_g_world_span_fn2);             /* mov eax,[0x8a2c0] (installed mapper, flat) */
        int writeout;
        if (fn2 == (0x388be + OBJ_DELTA) || fn2 == (0x39453 + OBJ_DELTA)) {
            /* 0x37ab1: single-texel opacity test */
            if (*(volatile uint8_t *)(uintptr_t)(fs_base + esi) != 0) {
                writeout = 1;                                 /* opaque -> jne 0x37b8d (record) */
            } else {
                writeout = exit_f_neighbor_test(esi, fs_base);/* transparent -> 0x37abc boundary test */
            }
        } else if (fn2 == (0x39398 + OBJ_DELTA) || fn2 == (0x383ac + OBJ_DELTA)) {
            /* 0x37a90: masked (wrapped) texel test; esi reloads to [0x8a338] for the record */
            uint32_t base338 = (uint32_t)G32(VA_g_span_src_wrap_base);
            uint32_t off = ((uint32_t)esi - base338) & (uint32_t)G32(VA_g_span_src_wrap_reoffset);
            if (*(volatile uint8_t *)(uintptr_t)(fs_base + base338 + off) == 0) {
                writeout = 0;                                 /* je 0x37c54 (skip) */
            } else {
                esi = base338; writeout = 1;                  /* jmp 0x37b8d (record) */
            }
        } else {
            writeout = 1;                                     /* jne 0x37b8d (record, no opacity test) */
        }
        if (writeout) exit_f_writeout(esi);                   /* 0x37b8d build deferred descriptor */
        g_wd_terminate = 1;                                   /* add esp,0xc; ret -> unwind the driver */
        return;
    }
    mapper_va = G32(VA_g_world_span_fn2);                                 /* jmp [0x8a2c0] -- EXIT E */
    goto do_mapper;

do_mapper:
    {
        const uint8_t *gs_base = (const uint8_t *)(uintptr_t)g_wd_gs_base;
        const uint8_t *es_base = (const uint8_t *)(uintptr_t)g_wd_es_base;
        uint32_t canon = mapper_canon ? mapper_va : (mapper_va - OBJ_DELTA);
        switch (canon) {     /* PEELED column mappers (shared render_world_span_*; param_1=eax,_2=edx,_3=ecx) */
        case 0x37cec: render_world_col_unshaded_opaque_37cec(eax, ecx, edx, ebx, esi_in, edi_in); return;
        case 0x37c60: render_world_col_unshaded_37c60(eax, ecx, edx, esi_in, edi_in); return;
        case 0x37ec8: render_world_col_shaded_gs_37ec8(eax, edx, ecx, ebx, esi_in, edi_in, gs_base); return;
        case 0x38198: render_world_col_shaded_masked_gs_38198(eax, edx, ecx, ebx, esi_in, edi_in, gs_base); return;
        case 0x3832c: render_world_col_unshaded_2axis_3832c(eax, edx, ecx, esi_in, edi_in); return;
        case 0x383ac: render_world_col_unshaded_masked_2axis_383ac(eax, edx, ecx, esi_in, edi_in); return;
        case 0x37fac: render_world_col_unshaded_opaque_37fac(eax, edx, ecx, ebx, esi_in, edi_in); return;
        case 0x38288: render_world_col_unshaded_masked_2axis_38288(eax, edx, ecx, ebx, esi_in, edi_in); return;
        case 0x385d4: render_world_col_tint_385d4(edi_in, ecx, es_base); return;
        case 0x38434: render_world_col_shaded_gs_wrapped_38434(eax, edx, ecx, ebx, esi_in, edi_in, gs_base); return;
        case 0x384fc: render_world_col_shaded_masked_gs_wrapped_384fc(eax, edx, ecx, ebx, esi_in, edi_in, gs_base); return;
        case 0x38684: render_world_col_solid_fill_38684(ecx, ebx, edi_in, gs_base); return;
        case 0x38697: wmap_solidfill((uint8_t)eax, ecx, edi_in); return;       /* EXIT D solid fill (al=[0x89f10]) */
        case 0x38964: render_world_col_unshaded_masked_38964(eax, ecx, edx, ebx, esi_in, edi_in); return;
        case 0x387e0: wmap_solidfill(G8(VA_g_sprite_fill_index), ecx, edi_in); return;        /* stub: al=[0x90a24] -> 0x38697 */
        case 0x387f0: render_world_col_solid_gradient_387f0(ecx, ebx, edi_in, gs_base); return;
        case 0x38631: render_world_col_tint_gradient_38631(edi_in, ecx, (uint8_t)(ebx >> 8), gs_base, es_base); return;
        case 0x385dc: render_world_col_tint_gs_385dc(edi_in, ecx, (uint8_t)(ebx >> 8), gs_base, es_base); return;
        case 0x388be: render_world_col_unshaded_masked_388be(eax, ecx, edx, esi_in, edi_in); return;
        case 0x390ac: render_world_span_390ac(eax, edx, ecx, ebx, esi_in, edi_in, gs_base, es_base); return;
        case 0x391d0: render_world_span_wrapped_391d0(eax, edx, ecx, ebx, esi_in, edi_in,
                                                             (uint32_t)G32(VA_g_span_src_wrap_reoffset), gs_base, es_base); return;
        case 0x392cc: render_world_col_shaded_blend_2axis_392cc(eax, edx, ecx, ebx, esi_in, edi_in, gs_base, es_base); return;
        case 0x39398: render_world_col_blend_2axis_39398(eax, edx, ecx, esi_in, edi_in, es_base); return;
        case 0x39453: render_world_col_blend_masked_39453(eax, edx, ecx, esi_in, edi_in, es_base); return;
        case 0x39520: render_world_col_shaded_blend_masked_39520(eax, edx, ecx, ebx, esi_in, edi_in, es_base); return;
        default: break;
        }
        regs_t m; memset(&m, 0, sizeof m);                   /* unpeeled mapper -> call_orig bridge */
        m.va = mapper_canon ? (mapper_va + OBJ_DELTA) : mapper_va;
        m.eax = eax; m.ebx = ebx; m.ecx = ecx; m.edx = edx;
        m.esi = esi_in; m.edi = edi_in; m.ebp = ebp_in;
        m.es = io->es; m.fs = io->fs; m.gs = io->gs;
#ifndef ROTH_STANDALONE
        call_orig(&m);
#else
        roth_unreachable(mapper_canon ? mapper_va : mapper_va - OBJ_DELTA);   /* 3D span mapper — render tier */
#endif
    }
}

/* One column of the wall driver (the loop body, 0x374a6/0x37620). edi0 = the column's dest base
 * ([0x8a2b0]). Either bridges compute_wall_column_source_offset (clipped columns, 0x8a34c&3) or does the
 * inline source-texel/pixel-step/shade calc, then bridges dispatch_world_span_column (-> the mapper ->
 * writes the framebuffer). esi (per-column src offset) starts 0 (push/pop'd around the body). */
static void wd_column(uint32_t edi0, uint16_t es_sel, uint16_t fs_sel, uint16_t gs_sel)
{
#ifdef ROTH_STANDALONE
    /* SAFETY (imgfree): a PERSPECTIVE column (flags&8) always divides the texel accumulators by
     * g_span_texV_accum+0x10 (0x8a300) — the clipped path in compute_wall_column_source_offset
     * (0x378f6 `div ebx`), its inline twin below (same div), and the dispatch tail
     * dispatch_world_span_column L_37803 (0x37806 `div [0x8a300]`). That divisor is set once, per
     * span, UNCONDITIONALLY: 0x8a300 = ecxv * g_wall_segment_width (orig 0x36d88 `imul eax,ebx;
     * mov [0x8a300],eax`). A DEGENERATE span whose projected columns collapse to zero width
     * (g_wall_segment_width == 0, i.e. xr == xl-1 — seen for a zero-width reflected sprite in a
     * secondary-surface/mirror subpass, core 228566: a door-area SIGFPE, xl=621 xr=620, subpass
     * 0x90a48!=0) makes the divisor 0. None of those three `div`s has a zero-guard, so the original
     * #DEs (crashes) here too — meaning valid geometry never reaches a perspective column with a
     * 0 divisor (the author DID guard this function's OTHER divide, 0x379a7 `je`, against exactly a
     * zero divisor); only this degenerate zero-width span does. Skip the (invisible) column. Trap
     * host / oracle stay byte-identical (standalone-only). The upstream reason the zero-width mirror
     * span isn't culled (secondary-surface bbox/projection, cf. 62e5b86 / 629ef99) is tracked
     * separately; this stops the crash at the divide. */
    if ((G8(VA_g_world_surface_draw_flags) & 8) && G32(VA_g_span_texV_accum + 0x10) == 0) return;
#endif
    if (G8(VA_g_span_draw_mode_flags) & 3) {                                   /* clipped column (0x37560/0x376da) PEELED */
        uint32_t esi, edi, ecx, edx, eax;
        if (compute_wall_column_source_offset(edi0, &esi, &edi, &ecx, &edx, &eax)) return;
        regs_t d; memset(&d, 0, sizeof d);                   /* dispatch with compute_offset's outputs */
        d.eax = eax; d.ecx = ecx; d.edx = edx; d.esi = esi; d.edi = edi;
        d.es = es_sel; d.fs = fs_sel; d.gs = gs_sel;
        dispatch_world_span_column(&d);               /* PEELED (was call_orig 0x3778b) */
        return;
    }
    /* inline source calc (8a34c&3 == 0) */
    int32_t Vint = (int16_t)G16(VA_g_span_texV_accum + 0x2), Uint = (int16_t)G16(VA_g_span_texU_accum + 0x2);
    int32_t rc = Vint - Uint;                                /* sub ecx,ebx */
    if (rc < 0) return;                                      /* jl -> skip column */
    rc++;
    uint32_t ecx = (uint32_t)rc, esi = 0, eax = 0, edx = 0;
    G8(VA_g_span_draw_mode_flags + 0x2) = (uint8_t)Uint;
    uint32_t edi = edi0 + G32(VA_g_scanline_dest_offset_table + (uint32_t)(uint16_t)Uint * 4u);   /* add edi,[ebx*4+0x854a8] */
    if (G8(VA_g_world_surface_draw_flags) & 8) {                                   /* perspective source/step/shade */
        uint32_t a  = (uint32_t)G32(VA_g_span_texV_accum + 0x18) / (uint32_t)G32(VA_g_span_texV_accum + 0x10);  /* eax=8a308/8a300, x86 `div` UNSIGNED */
        uint16_t ax = (uint16_t)((uint16_t)a + (uint16_t)G16(VA_g_span_texV_accum + 0x4));
        ax = (uint16_t)(ax >> 1);
        if (G8(VA_g_world_surface_draw_flags) & 2) ax = (uint16_t)((uint16_t)(~ax) + (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0xe));
        ax = (uint16_t)(ax & (uint16_t)G16(VA_g_column_clip_mode + 0x4));
        ax = (uint16_t)(ax * (uint16_t)G16(VA_g_span_src_row_width));        /* imul ax,[0x90978] */
        esi += ax; G32(VA_g_span_src_wrap_base) = esi; esi += G32(VA_g_span_src_wrap_base + 0x4);
        uint32_t ebx = (uint32_t)((uint16_t)ecx) * 2u - 1u;  /* pixel step denom */
        if (ebx != 0) eax = G32(VA_g_wall_proj_y3 + 0x2) / ebx;
        if (G16(VA_g_span_round_half) != 0x8000) {                        /* shade seed */
            int16_t bp = (int16_t)((int16_t)G16(VA_g_span_texU_accum + 0x2) - (int16_t)G16(VA_g_span_src_wrap_reoffset + 0x28));
            int32_t sh = (int32_t)eax * (int32_t)bp - (int32_t)G32(VA_g_render_double_scanline_flag + 0x4);
            if (sh < 0) sh = 0;
            G16(VA_g_span_eax_accum_init) = (uint16_t)sh;
            uint32_t hi = (uint32_t)sh >> 16; if (hi > 0xf) hi = 0xf;
            esi += hi;
        } else {
            G16(VA_g_span_eax_accum_init) = 0;
        }
        edx = eax >> 16;
    }
    regs_t d; memset(&d, 0, sizeof d);                       /* dispatch_world_span_column (0x375a4) */
    d.eax = eax; d.ecx = ecx; d.edx = edx; d.esi = esi; d.edi = edi;
    d.es = es_sel; d.fs = fs_sel; d.gs = gs_sel;
    dispatch_world_span_column(&d);                   /* PEELED (was call_orig 0x3778b) */
}

/* The shared WALL BODY (0x36b68) — entered by BOTH the wall driver draw_world_surface_spans (0x36b39,
 * after its prelude) AND rasterize_world_spans_scanline's wall path (0x366cb, after the texcoord setup).
 * Factored out so both share it. Reads its selectors from globals + the wall texcoord/extent globals the caller's
 * prelude set; mapper BASES come via g_wd_*_base (set by the caller first). ecx_entry: only the HIGH 16 bits are used
 * (the parallax-sky degenerate path); 0x366cb's wall path leaves ECX high = 0 (it `sub ecx,ecx` at entry). */
static void wall_body_36b68(uint32_t ecx_entry, int set_8a334)
{
    uint16_t fs_sel = (uint16_t)G16(VA_g_world_alt_render_flags + 0x2);                /* 0x36b3f / 0x36a42 mov fs,[0x909b0] */
    if (set_8a334) G16(VA_g_wall_render_flags) = (uint16_t)(G16(VA_g_world_surface_draw_flags) & 1);  /* 0x36b68 (skipped when entered at 0x36b78) */
    uint16_t es_sel = (uint16_t)G16(VA_g_render_target_selector);                /* 0x36b78 mov es,[0x90c06] */
    uint16_t gs_sel = (uint16_t)G16(VA_g_active_world_remap_selector);                /* mov gs,[0x8a2a8] */
    (void)fs_sel; (void)es_sel; (void)gs_sel;                /* (used by the bridged callees + sky path) */

    uint8_t clipflags = 0;                                   /* edx (dl) */
    int16_t y0a = (int16_t)G16(VA_g_wall_proj_y3 + 0xc), y0b = (int16_t)G16(VA_g_wall_proj_y3 + 0x14);
    int16_t sMin = (y0a < y0b) ? y0a : y0b;                  /* 0x36b88 sVar = min */

    if ((int16_t)G16(VA_g_view_bound_bottom) <= sMin) {                     /* 0x36b9c jle 0x38cc6 (degenerate band) */
        if (G8(VA_g_parallax_sky_active) != 0) {                              /* 0x38cc6 */
            int16_t ax = (int16_t)G16(VA_g_wall_proj_y3 + 0xa), cx = (int16_t)G16(VA_g_wall_proj_y3 + 0x12);
            if ((int16_t)G16(VA_g_view_bound_left) < cx && (int16_t)G16(VA_g_view_bound_right) > ax) {
                if (ax < (int16_t)G16(VA_g_view_bound_left)) { ax = (int16_t)G16(VA_g_view_bound_left); G16(VA_g_wall_proj_y3 + 0xa) = (uint16_t)ax; }
                if (cx > (int16_t)G16(VA_g_view_bound_right))   cx = (int16_t)G16(VA_g_view_bound_right);
                G16(VA_g_span_texV_accum + 0x1e) = 0;
                G16(VA_g_span_texU_accum) = 0;
                int16_t span = (int16_t)(cx - ax);
                if (span >= 0) {                             /* 0x38d2c jns */
                    G16(VA_g_span_texU_accum + 0x2) = (uint16_t)((int16_t)G16(VA_g_view_bound_bottom) - (int16_t)G16(VA_g_view_bound_top));
                    G32(VA_g_wall_vstep) = 0;
                    uint32_t rcx = ((ecx_entry & 0xffff0000u) | (uint16_t)span) + 1u;
                    uint32_t saved_84980 = (uint32_t)G32(VA_g_render_source_base_ptr);       /* 0x38d53 push eax; call; pop eax; mov [0x84980],eax */
                    (void)render_parallax_sky_columns(            /* 0x38d6c (NATIVE) — [0x84980] is CALLER-SAVED */
                        (uint16_t)ax, rcx, 0, saved_84980, es_sel, fs_sel, gs_sel);
                    G32(VA_g_render_source_base_ptr) = (int32_t)saved_84980;                 /* restore (discard the sky renderer's return) */
                }
            }
        }
        return;                                              /* 0x38d64 xor ah,ah; ret */
    }

    int16_t vtop = (int16_t)G16(VA_g_view_bound_top), vbot = (int16_t)G16(VA_g_view_bound_bottom);
    if (sMin < vtop || y0b < vtop) clipflags |= 1;           /* 0x36ba9 */
    int16_t y1a = (int16_t)G16(VA_g_wall_proj_y3 + 0xe), y1b = (int16_t)G16(VA_g_wall_proj_y3 + 0x16);
    int16_t sMax = (y1a > y1b) ? y1a : y1b;                  /* 0x36bbe max */
    if (vtop >= sMax) return;                                /* 0x36bd2 jge 0x36e9b ret */
    if (vbot < sMax || vbot < y1b) clipflags |= 2;           /* 0x36bdf */
    int16_t xl = (int16_t)G16(VA_g_wall_proj_y3 + 0xa), xr = (int16_t)G16(VA_g_wall_proj_y3 + 0x12);
    if ((int16_t)G16(VA_g_view_bound_left) >= xr) return;                 /* 0x36c01 jge ret */
    if ((int16_t)G16(VA_g_view_bound_right) < xl) return;                  /* 0x36c0e jl ret */
    G16(VA_g_wall_segment_width) = (uint16_t)(xr - xl + 1);                  /* g_wall_segment_width */
    G8(VA_g_span_draw_mode_flags)  = clipflags;

    /* === clip-shade setup (0x36c2b) — identical to the edge-walker's === */
    G16(VA_g_floorceil_depth_clip_bias) = 8;
    if ((uint8_t)G8(VA_g_column_clip_mode) != 0) {
        int16_t  axv  = (int16_t)((uint16_t)(uint8_t)G8(VA_g_column_clip_mode) - 0x80u);
        uint32_t ebxv = (uint16_t)axv;
        int16_t  bxp4 = (int16_t)((uint16_t)ebxv + 4u);
        ebxv = (bxp4 < 0) ? 0u : (uint16_t)bxp4;
        G16(VA_g_floorceil_depth_clip_bias) = (uint16_t)((uint16_t)G16(VA_g_floorceil_depth_clip_bias) + (uint16_t)axv);
        uint16_t bxs = (uint16_t)((uint16_t)ebxv >> 2);
        G16(VA_g_floorceil_clip_scale) = (uint16_t)(-((int32_t)bxs - 0x20));
    }

    /* === flags&0x40 depth-bias adjust (0x36c64) === */
    if (G16(VA_g_world_surface_draw_flags) & 0x40) {
        int16_t ax = (int16_t)G16(VA_g_span_src_wrap_reoffset + 0x8);
        int16_t bx = (int16_t)G16(VA_g_wall_proj_y3 + 0x10);
        int16_t dx = (int16_t)G16(VA_g_wall_proj_y3 + 0x18);
        int proceed = 1;
        if ((int16_t)(dx | bx) < 0) {                        /* mov ebp,edx; or bp,bx; jns 0x36caa */
            int16_t bp = (int16_t)(dx - bx); if (bp < 0) bp = (int16_t)(-bp);   /* sub bp,bx; jns; neg bp */
            int16_t base = (bx < 0) ? bx : dx;               /* test bx,bx; js (keep bx) else mov ebx,edx */
            int16_t t = (int16_t)((int16_t)(base - 0x10) + bp);
            if (t <= 0) proceed = 0;                         /* jle 0x36ccb */
            else ax = (int16_t)(((int32_t)ax * (int32_t)t) / (int32_t)bp);    /* imul bx; idiv bp */
        }
        if (proceed) {
            uint16_t cxv = (uint16_t)ax;                     /* mov ecx,eax */
            if (ax != 0) {                                   /* test ax,ax; jne */
                int16_t s = (int16_t)(ax + (int16_t)G16(VA_g_span_src_wrap_reoffset + 0x12));
                if (s > 0) {                                 /* jle 0x36ccb */
                    uint16_t e16 = (uint16_t)((uint16_t)s << 4);     /* shl eax,4 (16-bit dividend) */
                    G16(VA_g_floorceil_depth_clip_bias) = (uint16_t)(G16(VA_g_floorceil_depth_clip_bias) - (uint16_t)(e16 / cxv));   /* div cx; sub [0x90a1e],ax */
                }
            }
        }
    }

    /* === clip-bound setup (0x36ccb) === */
    G32(VA_g_span_texV_accum + 0x4) = 0;                                        /* mov dword [0x8a2f4],0 (also zeroes 0x8a2f6) */
    {
        int16_t v5e = (int16_t)G16(VA_g_wall_proj_y3 + 0x10), v66 = (int16_t)G16(VA_g_wall_proj_y3 + 0x18);
        G16(VA_g_span_round_half + 0x12) = (uint16_t)v5e;
        G16(VA_g_span_round_half + 0x14) = (uint16_t)v5e;
        G16(VA_g_span_round_half + 0x16) = (uint16_t)v66;
        int16_t d = (int16_t)(v66 - v5e);
        if (d < 0) {                                         /* 0x36d2d (966<95e) */
            int16_t s14 = (int16_t)(0x10 - v66);
            if (s14 != 0 && v66 < 0x11) {
                G16(VA_g_span_round_half + 0x16) = (uint16_t)(v66 + s14);        /* = 0x10 */
                G16(VA_g_span_round_half + 0x8) = (uint16_t)(-d);               /* 95e-966 */
                G16(VA_g_span_texV_accum + 0x6) = (uint16_t)((int16_t)(-d) - s14);
            }
        } else if (d != 0) {                                 /* (966>95e) */
            int16_t s14 = (int16_t)(0x10 - v5e);
            if (s14 != 0 && v5e < 0x11) {
                G16(VA_g_span_round_half + 0x14) = (uint16_t)(v5e + s14);        /* = 0x10 */
                uint16_t num = (uint16_t)(int16_t)(v66 - 0x10);          /* -(s14-d) = 966-0x10 */
                uint32_t qq  = ((uint32_t)num * (uint16_t)G16(VA_g_wall_proj_y3 + 0x8)) / (uint16_t)d;  /* mul cx; div bx */
                G16(VA_g_span_texV_accum + 0x4) = (uint16_t)(~(uint16_t)(qq - (uint16_t)G16(VA_g_wall_proj_y3 + 0x8)));    /* sub eax,ecx; not eax */
            }
        }
    }

    /* === perspective steps (0x36d4e) + SMC patches 0x375d4/0x375c4 === */
    {
        /* `mov edx,<imm>` @0x36d4e/0x36d5e is SMC-patched per-frame (static placeholder 8 -> 0x80000; the
         * real perspective dividend is patched in, read LIVE from the code). DX:AX = (imm&0xffff)<<16. */
        uint32_t ecxv = ((uint32_t)G16(VA_g_span_shade_dividend_default) << 16) / (uint16_t)G16(VA_g_span_round_half + 0x14);
        uint32_t eaxv = ((uint32_t)G16(VA_g_span_shade_dividend2_default) << 16) / (uint16_t)G16(VA_g_span_round_half + 0x16);
        G16(VA_g_span_round_half + 0x1a) = (uint16_t)eaxv;
        G32(VA_g_texstep_step_stale_b) = (uint32_t)(int32_t)(int16_t)((uint16_t)eaxv - (uint16_t)ecxv);   /* sub eax,ecx; cwde; SMC */
        G32(VA_g_span_texV_accum + 0x10) = (uint32_t)((int32_t)(ecxv & 0xffffu) * (int32_t)(uint16_t)G16(VA_g_wall_segment_width));  /* ecx*width */
        if (G8(VA_g_world_surface_draw_flags) & 8) {
            uint16_t sub = (uint16_t)((int16_t)G16(VA_g_wall_proj_y3 + 0x8) - (int16_t)G16(VA_g_span_texV_accum + 0x4));
            uint32_t e = (uint32_t)(uint16_t)G16(VA_g_span_round_half + 0x1a) * sub;             /* 8a330 * (956-8a2f4) */
            if ((int16_t)G16(VA_g_span_texV_accum + 0x6) != 0)
                e = (uint32_t)(((uint64_t)e * (uint16_t)G16(VA_g_span_texV_accum + 0x6)) / (uint16_t)G16(VA_g_span_round_half + 0x8));
            G32(VA_g_span_texV_accum + 0x18) = 0;
            G32(VA_g_texstep_step_stale_a) = e;                                /* SMC */
        }
    }

    /* === 8a304 + left-column perspective extend (0x36dda) === */
    G32(VA_g_span_texV_accum + 0x14) = (uint32_t)((uint16_t)G16(VA_g_wall_segment_width) << 12);
    {
        int16_t c = (int16_t)((int16_t)G16(VA_g_view_bound_left) - (int16_t)G16(VA_g_wall_proj_y3 + 0xa));
        if (c < 0) c = 0;
        G16(VA_g_span_texV_accum + 0x1e) = (uint16_t)c;
        uint32_t ecxv = (uint16_t)c;
        if (ecxv != 0) {
            G32(VA_g_span_texV_accum + 0x10) = (uint32_t)((int32_t)G32(VA_g_span_texV_accum + 0x10) + (int32_t)G32(VA_g_texstep_step_stale_b) * (int32_t)ecxv);
            if (G8(VA_g_world_surface_draw_flags) & 8) {
                uint64_t prod = (uint64_t)G32(VA_g_texstep_step_stale_a) * ecxv;   /* mul ecx (32x32->64) */
                uint32_t lo = (uint32_t)prod, hi = (uint32_t)(prod >> 32);
                while (hi != 0) {                                /* reduce loop 0x36e2d */
                    lo = (lo >> 1) | (hi << 31); hi >>= 1;       /* shr edx,1; rcr eax,1 */
                    G32(VA_g_texstep_step_stale_b) = (uint32_t)((int32_t)G32(VA_g_texstep_step_stale_b) >> 1);   /* sar */
                    G32(VA_g_texstep_step_stale_a) >>= 1;
                    G32(VA_g_span_texV_accum + 0x10) >>= 1;
                    G32(VA_g_span_texV_accum + 0x14) >>= 1;
                }
                G32(VA_g_span_texV_accum + 0x18) = lo;
            }
        }
    }

    /* === 8a30c + endpoint save (0x36e52) === */
    {
        int16_t c = (int16_t)((int16_t)G16(VA_g_wall_proj_y3 + 0x12) - (int16_t)G16(VA_g_view_bound_right));
        if (c < 0) c = 0;
        G16(VA_g_span_texV_accum + 0x1c) = (uint16_t)c;
        if ((uint16_t)((uint16_t)c | (uint16_t)G16(VA_g_span_texV_accum + 0x1e)) != 0) {
            G16(VA_g_span_round_half + 0xe) = G16(VA_g_wall_proj_y3 + 0xc);
            G16(VA_g_span_round_half + 0x10) = G16(VA_g_wall_proj_y3 + 0xe);
            G16(VA_g_span_round_half + 0x12) = G16(VA_g_span_round_half + 0x14);
        }
    }

    /* === column count (0x36e9e) === */
    {
        int16_t cc = (int16_t)((int16_t)G16(VA_g_wall_segment_width) - (int16_t)G16(VA_g_span_texV_accum + 0x1e) - (int16_t)G16(VA_g_span_texV_accum + 0x1c));
        G16(VA_g_wall_column_count) = (cc == 0) ? 1u : (uint16_t)cc;        /* or ax,ax; jne; inc eax */
    }
    if (G8(VA_g_world_surface_draw_flags) & 8) {                                   /* flags&8: 8a316 / 8a314 (0x36ebe) */
        int16_t a  = (int16_t)((int16_t)G16(VA_g_wall_proj_y3 + 0xe) - (int16_t)G16(VA_g_span_src_wrap_reoffset + 0x28));
        int16_t a2 = (int16_t)((int16_t)G16(VA_g_wall_proj_y3 + 0xc) - (int16_t)G16(VA_g_span_src_wrap_reoffset + 0x28));
        if (a < 0 || a2 >= 0) {
            G16(VA_g_span_round_half) = 0x8000;
        } else if (G16(VA_g_world_surface_draw_flags + 0x4) & 0x100) {
            G32(VA_g_render_double_scanline_flag + 0x4) = (uint32_t)((int32_t)(int16_t)G16(VA_g_span_src_wrap_reoffset + 0xa) << 15);
        } else {
            uint16_t na = (uint16_t)(-(int16_t)G16(VA_g_span_src_wrap_reoffset + 0xa));
            uint32_t pr = (uint32_t)((((uint64_t)((uint32_t)na << 15)) * (uint16_t)G16(VA_g_wall_proj_y3 + 0x4)) / (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x4));
            G32(VA_g_render_double_scanline_flag + 0x4) = (uint32_t)(-(int32_t)pr);
        }
    }

    /* === corner-clip adjust (0x36f37): clip the corner Ys + recompute 8a32a/8a32c for clipped cols === */
    {
        int16_t bx = (int16_t)G16(VA_g_wall_segment_width);                  /* width */
        int16_t cx = (int16_t)G16(VA_g_span_texV_accum + 0x1e);
        if (cx != 0) {
            int32_t ebp = (int16_t)G16(VA_g_wall_proj_y3 + 0xe);
            int16_t d1 = (int16_t)(((int32_t)(int16_t)((int16_t)G16(VA_g_wall_proj_y3 + 0xe) - (int16_t)G16(VA_g_wall_proj_y3 + 0x16)) * cx) / bx);
            G16(VA_g_wall_proj_y3 + 0xe) = (uint16_t)((int16_t)G16(VA_g_wall_proj_y3 + 0xe) - d1);
            int16_t a5a = (int16_t)G16(VA_g_wall_proj_y3 + 0xc);
            ebp -= a5a;
            int16_t d2 = (int16_t)(((int32_t)(int16_t)(a5a - (int16_t)G16(VA_g_wall_proj_y3 + 0x14)) * cx) / bx);
            G16(VA_g_wall_proj_y3 + 0xc) = (uint16_t)(a5a - d2);
            int16_t den = (int16_t)((int16_t)G16(VA_g_wall_proj_y3 + 0xe) - (int16_t)G16(VA_g_wall_proj_y3 + 0xc));
            if (den > 0) G16(VA_g_span_round_half + 0x14) = (uint16_t)(((uint32_t)(uint16_t)G16(VA_g_span_round_half + 0x14) * (uint16_t)(int16_t)ebp) / (uint16_t)den);
        }
    }
    {
        int16_t bx = (int16_t)G16(VA_g_wall_segment_width);
        int16_t cx = (int16_t)G16(VA_g_span_texV_accum + 0x1c);
        if (cx != 0) {
            int32_t ebp = (int16_t)G16(VA_g_wall_proj_y3 + 0x16);
            int16_t d1 = (int16_t)(((int32_t)(int16_t)((int16_t)G16(VA_g_wall_proj_y3 + 0x16) - (int16_t)G16(VA_g_span_round_half + 0x10)) * cx) / bx);
            G16(VA_g_wall_proj_y3 + 0x16) = (uint16_t)((int16_t)G16(VA_g_wall_proj_y3 + 0x16) - d1);
            int16_t a62 = (int16_t)G16(VA_g_wall_proj_y3 + 0x14);
            ebp -= a62;
            int16_t d2 = (int16_t)(((int32_t)(int16_t)(a62 - (int16_t)G16(VA_g_span_round_half + 0xe)) * cx) / bx);
            G16(VA_g_wall_proj_y3 + 0x14) = (uint16_t)(a62 - d2);
            int16_t den = (int16_t)((int16_t)G16(VA_g_wall_proj_y3 + 0x16) - (int16_t)G16(VA_g_wall_proj_y3 + 0x14));
            if (den > 0) G16(VA_g_span_round_half + 0x16) = (uint16_t)(((uint32_t)(uint16_t)G16(VA_g_span_round_half + 0x16) * (uint16_t)(int16_t)ebp) / (uint16_t)den);
        }
    }

    /* === corner projection (0x3701e) -> g_wall_proj_y0..3 (0x90948/4a/4c/4e); 3 variants === */
    int do_steppers = 1;
    G8(VA_g_span_round_half + 0x6) = 0;
    if (G16(VA_g_world_surface_draw_flags) & 0x8000) {                             /* variant 1: high-byte (0x37034) */
        G8(VA_g_span_round_half + 0x6) = 1;
        if (G8(VA_g_column_clip_mode) != 0) {                              /* clip-scaled (0x3704a) */
            uint8_t bias = (uint8_t)G8(VA_g_floorceil_depth_clip_bias);
            /* SMC shl counts @0x37062/@0x370a4 (0x2d6a8-patched per shade level: L0=3/L1=2/L2=1) */
            uint8_t shlA = (uint8_t)(*(volatile uint8_t *)GADDR(0x37062) & 0x1f);
            uint8_t shlB = (uint8_t)(*(volatile uint8_t *)GADDR(0x370a4) & 0x1f);
            uint32_t sA = (uint32_t)(uint16_t)G16(VA_g_span_round_half + 0x14) << shlA;
            { int8_t ch = (int8_t)((uint8_t)(sA >> 8) - bias); sA = (ch < 0) ? 0u : ((sA & 0xffff00ffu) | ((uint32_t)(uint8_t)ch << 8)); }
            uint32_t sB = (uint32_t)(uint16_t)G16(VA_g_span_round_half + 0x16) << shlB;
            { int8_t ch = (int8_t)((uint8_t)(sB >> 8) - bias); sB = (ch < 0) ? 0u : ((sB & 0xffff00ffu) | ((uint32_t)(uint8_t)ch << 8)); }
            uint16_t e;
            e = (uint16_t)(((uint32_t)(uint8_t)G8(VA_g_span_round_half + 0x2) << 8) + sA); G16(VA_g_wall_proj_y0) = (e >= 0x2000) ? 0x2000 : e;
            e = (uint16_t)(((uint32_t)(uint8_t)G8(VA_g_span_round_half + 0x5) << 8) + sA); G16(VA_g_wall_proj_y3) = (e >= 0x2000) ? 0x2000 : e;
            e = (uint16_t)(((uint32_t)(uint8_t)G8(VA_g_span_round_half + 0x3) << 8) + sB); G16(VA_g_wall_proj_y1) = (e >= 0x2000) ? 0x2000 : e;
            e = (uint16_t)(((uint32_t)(uint8_t)G8(VA_g_span_round_half + 0x4) << 8) + sB); G16(VA_g_wall_proj_y2) = (e >= 0x2000) ? 0x2000 : e;
        } else {                                             /* simple high-byte (0x370dc) */
            G16(VA_g_wall_proj_y0) = (uint16_t)((uint16_t)(uint8_t)G8(VA_g_span_round_half + 0x2) << 8);
            G16(VA_g_wall_proj_y1) = (uint16_t)((uint16_t)(uint8_t)G8(VA_g_span_round_half + 0x3) << 8);
            G16(VA_g_wall_proj_y2) = (uint16_t)((uint16_t)(uint8_t)G8(VA_g_span_round_half + 0x4) << 8);
            G16(VA_g_wall_proj_y3) = (uint16_t)((uint16_t)(uint8_t)G8(VA_g_span_round_half + 0x5) << 8);
        }
    } else if (G16(VA_g_world_alt_render_flags) & 0x8000) {                      /* variant 2: perspective (0x37123) */
        G8(VA_g_span_round_half + 0x6) = 1;
        G16(VA_g_span_draw_mode_flags + 0x4) = 0;
        { int16_t v = (int16_t)((int16_t)G16(VA_g_wall_proj_y3 + 0xa) + (int16_t)G16(VA_g_span_texV_accum + 0x1e) - (int16_t)G16(VA_g_span_src_wrap_reoffset + 0x14));
          uint32_t a = (v < 0) ? (uint32_t)(uint16_t)(-v) : (uint32_t)(uint16_t)v;
          G16(VA_g_span_round_half + 0xa) = (uint16_t)((a * (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x18)) >> 8); }
        { int16_t v = (int16_t)((int16_t)G16(VA_g_wall_proj_y3 + 0x12) - (int16_t)G16(VA_g_span_texV_accum + 0x1c) - (int16_t)G16(VA_g_span_src_wrap_reoffset + 0x14));
          uint32_t a = (v < 0) ? (uint32_t)(uint16_t)(-v) : (uint32_t)(uint16_t)v;
          G16(VA_g_span_round_half + 0xc) = (uint16_t)((a * (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x18)) >> 8); }
        G16(VA_g_wall_proj_y0) = wd_project(G16(VA_g_wall_proj_y3 + 0xc), G16(VA_g_span_round_half + 0xa), G16(VA_g_span_round_half + 0x14));   /* y0 */
        G16(VA_g_wall_proj_y3) = wd_project(G16(VA_g_wall_proj_y3 + 0xe), G16(VA_g_span_round_half + 0xa), G16(VA_g_span_round_half + 0x14));   /* y3 (cx reused) */
        G16(VA_g_wall_proj_y1) = wd_project(G16(VA_g_wall_proj_y3 + 0x14), G16(VA_g_span_round_half + 0xc), G16(VA_g_span_round_half + 0x16));   /* y1 */
        G16(VA_g_wall_proj_y2) = wd_project(G16(VA_g_wall_proj_y3 + 0x16), G16(VA_g_span_round_half + 0xc), G16(VA_g_span_round_half + 0x16));   /* y2 (cx reused) */
        if (G8(VA_g_span_draw_mode_flags + 0x4) == 4 && !(G16(VA_g_world_surface_draw_flags) & 1)) {       /* 0x371f3 */
            G8(VA_g_world_surface_draw_flags) = (uint8_t)(G8(VA_g_world_surface_draw_flags) & 0xf7);
            G8(VA_g_sprite_fill_index) = (uint8_t)G16(VA_g_das_palette_remap_prefix);
            G8(VA_g_span_round_half + 0x6) = 0;
            do_steppers = 0;
        } else if (G8(VA_g_span_draw_mode_flags + 0x5) == 4) {                       /* 0x37220 */
            G8(VA_g_column_clip_mode) = 0;
            G8(VA_g_span_round_half + 0x6) = 0;
            do_steppers = 0;
        }
    } else {
        do_steppers = 0;                                     /* variant 3: no projection (jmp 0x37293) */
    }

    /* === endpoint steppers (0x3723c): epA/epB + per-column colsteps === */
    if (do_steppers) {
        int32_t cols = (uint16_t)G16(VA_g_wall_column_count);   /* sub ecx,ecx; mov cx -> ZERO-extended idiv divisor */
        int32_t y0 = (uint16_t)G16(VA_g_wall_proj_y0), y1 = (uint16_t)G16(VA_g_wall_proj_y1);
        G32(VA_g_span_endpoint_a) = (uint32_t)(y0 << 8);
        G32(VA_g_span_endpoint_a_colstep) = (uint32_t)(((y1 << 8) - (y0 << 8)) / cols);
        int32_t y3 = (uint16_t)G16(VA_g_wall_proj_y3), y2 = (uint16_t)G16(VA_g_wall_proj_y2);
        G32(VA_g_span_endpoint_b) = (uint32_t)(y3 << 8);
        G32(VA_g_span_endpoint_b_colstep) = (uint32_t)(((y2 << 8) - (y3 << 8)) / cols);
    }

    /* === variant install (0x37293): build the variant index, install fn-ptrs from 0x71f80 === */
    if (G8(VA_g_span_blend_mode_flag) != 0) {                                  /* 0x37293 degenerate */
        G32(VA_g_span_endpoint_b_colstep) = 0; G32(VA_g_span_endpoint_a_colstep) = 0;
        G8(VA_g_span_endpoint_b + 0x2) = 1; G8(VA_g_span_endpoint_a + 0x2) = 1;
        G8(VA_g_span_round_half + 0x6) = 0xff; G8(VA_g_column_clip_mode) = 1;
        G16(VA_g_span_accum_init) = 0;
    }
    G32(VA_g_span_src_wrap_base + 0x4) = (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x2e);                   /* 0x372d5 */
    {
        uint16_t flags = (uint16_t)G16(VA_g_world_surface_draw_flags);
        uint32_t ebx = (uint32_t)(flags & 1) << 3;
        if (!(flags & 8)) {
            ebx |= 0x20;
        } else {
            if (flags & 0x80) {                              /* 0x372ff */
                uint32_t edx = G32(VA_g_span_src_row_width) * 2u;
                uint16_t dx = (uint16_t)((uint16_t)edx - (uint16_t)G16(VA_g_wall_proj_y3 + 0x4));
                dx = (uint16_t)((int16_t)dx >> 1);
                dx = (uint16_t)(dx - (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x2e));
                if ((int16_t)dx < 0) { uint32_t e = ((edx & 0xffff0000u) | dx) & G32(VA_g_span_src_wrap_reoffset); dx = (uint16_t)e; }
                G16(VA_g_span_src_wrap_base + 0x4) = dx;
            }
            int32_t edv = (int32_t)((int32_t)G32(VA_g_span_src_row_width) - (int32_t)G32(VA_g_span_src_wrap_base + 0x4));   /* 0x37327 */
            edv += edv;
            if ((int16_t)edv < (int16_t)G16(VA_g_wall_proj_y3 + 0x4)) {
                G16(VA_g_wall_render_flags) = (uint16_t)(G16(VA_g_wall_render_flags) | 0x8000);
                ebx |= 0x10;
            }
        }
        if (G8(VA_g_span_textured_mode_flag) != 0) ebx |= 0x80;                   /* 0x3734a */
        if (G8(VA_g_span_round_half + 0x6) != 0) {                              /* 0x37359 alt install + SMC wrap-masks */
            G32(VA_g_world_span_fn_alt) = G32(VA_g_world_span_variant_table + ebx);
            G32(VA_g_world_span_fn_alt2) = G32((VA_g_world_span_variant_table + 0x4) + ebx);
            uint32_t wm = G32(VA_g_span_src_wrap_reoffset);
            G32(0x384b2) = wm; G32(0x384d1) = wm; G32(0x38582) = wm;
            G32(0x385a5) = wm; G32(0x39252) = wm; G32(0x39277) = wm;
            ebx |= 0x40;
        }
        G32(VA_g_world_span_fn) = G32(VA_g_world_span_variant_table + ebx);                   /* 0x373b0 */
        G32(VA_g_world_span_fn2) = G32((VA_g_world_span_variant_table + 0x4) + ebx);
    }

    /* === per-column loop setup (0x373c6) === */
    int32_t cols = (int16_t)G16(VA_g_wall_column_count);
    if (cols <= 0) return;                                   /* test cx,cx; jle 0x375e7 */
    {
        int32_t ebx = cols - 1;                              /* dec ebx */
        if (ebx != 0) {
            int16_t d1 = (int16_t)((int16_t)G16(VA_g_wall_proj_y3 + 0x14) - (int16_t)G16(VA_g_wall_proj_y3 + 0xc));
            int32_t e1 = (d1 == 0) ? 0 : (int32_t)((uint32_t)(uint16_t)d1 << 16) / ebx;   /* shl 0x10; cdq; idiv */
            G32(VA_g_wall_vstep) = (uint32_t)e1; G32(VA_g_texstep_u_stale) = (uint32_t)e1;
            int16_t d2 = (int16_t)((int16_t)G16(VA_g_wall_proj_y3 + 0x16) - (int16_t)G16(VA_g_wall_proj_y3 + 0xe));
            int32_t e2 = (d2 == 0) ? 0 : (int32_t)((uint32_t)(uint16_t)d2 << 16) / ebx;
            G32(VA_g_texstep_v_stale) = (uint32_t)e2;
        } else {
            G32(VA_g_texstep_v_stale) = G32(VA_g_world_span_fn2);                     /* cols==1: leftover eax (= g_world_span_fn2); 0x375b0 stale */
        }
    }
    G32(VA_g_span_texU_accum) = ((uint32_t)G16(VA_g_wall_proj_y3 + 0xc) << 16) | 0x7fffu;
    G32(VA_g_span_texV_accum) = ((uint32_t)G16(VA_g_wall_proj_y3 + 0xe) << 16) | 0x7fffu;
    uint32_t edi = (uint16_t)((int16_t)G16(VA_g_wall_proj_y3 + 0xa) + (int16_t)G16(VA_g_span_texV_accum + 0x1e));   /* di = 90958 + 8a30e */
    if (G8(VA_g_parallax_sky_active) != 0) {                                  /* first-column resolve (0x37448) */
        int16_t v6e = (int16_t)G16(VA_g_view_bound_top);
        if ((int16_t)G16(VA_g_wall_proj_y3 + 0xc) > v6e || (int16_t)G16(VA_g_wall_proj_y3 + 0x14) > v6e) {
            uint32_t saved_84980 = (uint32_t)G32(VA_g_render_source_base_ptr);       /* 0x3746e push eax; call; pop eax; mov [0x84980],eax */
            (void)render_parallax_sky_columns(            /* 0x38d6c first-column — [0x84980] is CALLER-SAVED */
                edi, (uint32_t)cols, 0, saved_84980, es_sel, fs_sel, gs_sel);
            G32(VA_g_render_source_base_ptr) = (int32_t)saved_84980;                 /* restore (discard the sky renderer's return) */
        }
    }
    G16(VA_g_span_texV_accum + 0x4) = (uint16_t)((int16_t)G16(VA_g_span_texV_accum + 0x4) + (int16_t)G16(VA_g_span_src_wrap_reoffset + 0x2c));   /* add [0x8a2f4],ax */

    /* === the per-column loop (normal 0x3749e / x-flip 0x375e8); steps shared (x-flip imms are copies) === */
    int xflip = (G8(VA_g_render_x_flip_flag) != 0);
    uint32_t loop_edi = xflip ? (G32(VA_g_current_decoded_frame + 0x10) - edi) : edi;  /* x-flip: edi = [0x84954] - edi (0x375e8) */
    int32_t stepU = (int32_t)G32(VA_g_texstep_u_stale), stepV = (int32_t)G32(VA_g_texstep_v_stale);
    int32_t step308 = (int32_t)G32(VA_g_texstep_step_stale_a), step300 = (int32_t)G32(VA_g_texstep_step_stale_b);
    int32_t counter = cols;
    do {
        G32(VA_g_active_world_remap_base + 0x4) = loop_edi;
        wd_column(loop_edi, es_sel, fs_sel, gs_sel);
        if (g_wd_terminate) return;                          /* EXIT F (subpass): descriptor recorded, unwind */
        G32(VA_g_span_texU_accum) += (uint32_t)stepU;                     /* loop tail: step the accumulators */
        G32(VA_g_span_texV_accum) += (uint32_t)stepV;
        { uint32_t old = G32(VA_g_span_texV_accum + 0x18), nw = old + (uint32_t)step308; G32(VA_g_span_texV_accum + 0x18) = nw;
          if (nw < old) {                                    /* carry -> perspective reduce (halve steps + accums) */
              step300 >>= 1;                                 /* sar (signed) */
              step308 = (int32_t)((uint32_t)step308 >> 1);   /* shr (unsigned) */
              G32(VA_g_span_texV_accum + 0x10) >>= 1; G32(VA_g_span_texV_accum + 0x14) >>= 1;
              G32(VA_g_span_texV_accum + 0x18) = (G32(VA_g_span_texV_accum + 0x18) >> 1) | 0x80000000u;
          }
        }
        G32(VA_g_span_texV_accum + 0x10) += (uint32_t)step300;
        loop_edi += xflip ? (uint32_t)-1 : 1u;               /* dec / inc edi */
    } while (--counter > 0);                                 /* dec ecx; jg */
    return;
}

/* draw_world_surface_spans (0x36b39) — the WALL span DRIVER ENTRY: prelude (inc counter + subpass-clear)
 * then the shared body 0x36b68. ABI_WALLDRIVER (ecx_entry + resolved gs/es/fs bases). */
void draw_world_surface_spans(uint32_t ecx_entry, uint32_t gs_base, uint32_t es_base, uint32_t fs_base)
{
    g_wd_gs_base = gs_base; g_wd_es_base = es_base; g_wd_fs_base = fs_base;
    g_wd_terminate = 0;                                      /* cleared each driver call (EXIT F unwind flag) */
    G32(VA_g_perspective_scale + 0x8)++;                                          /* 0x36b39 inc [0x85290] */
    if (G8(VA_g_world_render_subpass_kind) != 0) {                                  /* 0x36b46 subpass clear */
        G16(VA_g_world_surface_draw_flags) = (uint16_t)(G16(VA_g_world_surface_draw_flags) & 0x7fff);
        G8(VA_g_column_clip_mode)  = 0;
        G16(VA_g_world_alt_render_flags) = (uint16_t)(G16(VA_g_world_alt_render_flags) & 0x7fff);
    }
    wall_body_36b68(ecx_entry, 1);                           /* enters at 0x36b68 (sets [0x8a334]) */
}

/* ===================== Batch 59 (mirror/reflection SUBPASS subtree, 0x2b298+) =====================
 * build_secondary_surface_list (0x2b298) — the kind-4 COLLECT stage of the mirror/reflection subpass.
 * Walks the door/portal worklist (esi = head; node+0 = next, relocated by g_worklist_base 0x8498c) and
 * copies up to 0x40 entries into g_secondary_surface_list (0x84b18, 0x10-byte records), then latches the
 * view bounds (0x85314/0x85316) and g_has_secondary_surfaces (0x853d0). Pure obj3 writer, NO callees.
 * Record layout (g_secondary_surface_list 0x84b18): rec+0xc=[ebx+0],
 * rec+0=[ebx+4], rec+4=worklist node ptr, rec+8 = (word[[ebx+8]+0xa]<<16) | byte[ebx+0x12] (param<<16|type),
 * where ebx = node+4 (the surface sub-record). The entry prologue's transient stack temp
 * ([ebp+4]=-[0x909f8] after sub esp,0x10) is written-and-never-read (host stack only, not obj3) so it has
 * no observable effect and is omitted. esi = worklist head (0 -> empty list, count stays 0). */
void build_secondary_surface_list(uint32_t esi)
{
    G16(VA_g_span_src_wrap_reoffset + 0x10) = 1;                                            /* 0x2b298 */
    G16(VA_g_secondary_surface_count) = 0;                                            /* g_secondary_surface_count = 0 */
    uint8_t *rec = (uint8_t *)GADDR(VA_g_secondary_surface_list);                    /* g_secondary_surface_list dest */
    if (esi != 0) {
        esi += (uint32_t)G32(VA_g_door_worklist);                          /* relocate worklist head (0x2b2c4) */
        while (esi != 0) {
            if ((uint16_t)G16(VA_g_secondary_surface_count) < 0x40) {                /* cap 64 records (0x2b2ca jae) */
                uint32_t ebx = *(volatile uint32_t *)(uintptr_t)(esi + 4);   /* surface sub-record */
                *(volatile uint32_t *)(rec + 0xc) = *(volatile uint32_t *)(uintptr_t)ebx;          /* +0xc */
                *(volatile uint32_t *)(rec + 0)   = *(volatile uint32_t *)(uintptr_t)(ebx + 4);    /* +0   */
                *(volatile uint32_t *)(rec + 4)   = esi;                                           /* +4   */
                uint32_t p = *(volatile uint32_t *)(uintptr_t)(ebx + 8);
                uint32_t v = ((uint32_t)*(volatile uint16_t *)(uintptr_t)(p + 0xa) << 16)
                           | (uint8_t)*(volatile uint8_t *)(uintptr_t)(ebx + 0x12);                /* +8   */
                *(volatile uint32_t *)(rec + 8) = v;
                G16(VA_g_secondary_surface_count) = (uint16_t)((uint16_t)G16(VA_g_secondary_surface_count) + 1);
                rec += 0x10;
            }
            esi = *(volatile uint32_t *)(uintptr_t)esi;          /* next = node+0 (0x2b2fe) */
        }
    }
    if ((uint16_t)G16(VA_g_secondary_surface_count) != 0) {                          /* 0x2b30e */
        G16(VA_g_camera_cos + 0x2) = (uint16_t)G16(VA_g_view_bound_left);
        G16(VA_g_camera_cos + 0x4) = (uint16_t)G16(VA_g_view_bound_right);
        G8(VA_g_has_secondary_surfaces)  = 0xff;                                     /* g_has_secondary_surfaces */
    }
}

/* ---- the secondary-surface PASSES (0x2b333/0x2b36f/0x2b407): thin iterators over g_secondary_surface_list
 * (0x84b18, count 0x85318) that set the per-record draw anchors (g_view_clip_plane 0x85268 / 0x85260 /
 * 0x85264) and call the per-surface renderer render_world_secondary_surface (0x2bc3c, BRIDGED via call_orig
 * for now). pass1/pass2 additionally mark the subpass (g_secondary_subpass_id 0x853f9,
 * g_secondary_subpass_flags 0x853d2 |= 1/2), swap a clip bound (0x852b4<-0x84940 / 0x852b6<-0x84942), and
 * filter by the record TYPE byte (rec+8) and the horizon split line 0x853ce on rec+0xa: pass1 renders
 * type==1 or (type>=2 AND param>=split); pass2 renders type==1 or (type>=2 AND param<split). The push/pop of
 * esi/ecx/ebx/es is the original's callee-save (host-stack, not obj3) -> omitted. es/fs/gs = the trap entry
 * selectors, forwarded to the 0x2bc3c bridge (the renderer's wall-driver callee re-loads its own). ---- */
static void secpass_render_record(uint32_t rec, uint16_t es, uint16_t fs, uint16_t gs)
{
    G32(VA_g_view_clip_plane + 0x4) = (int32_t)rec;                                       /* anchor record ptr */
    G32(VA_g_visible_extent_list + 0x3c) = *(volatile int32_t *)(uintptr_t)(rec + 0xc);        /* [rec+0xc] */
    G32(VA_g_view_clip_plane) = *(volatile int32_t *)(uintptr_t)(rec);              /* [rec+0]  */
    /* [RE-POINTED: render_world_secondary_surface 0x2bc3c -> direct lifted call. The
     * "genuine keep" premise (the body needs g_rwss_trap_* entry regs the direct call can't populate)
     * was DISPROVEN by disasm: the ONLY consumer of the 0x2bc3c entry GP regs ebx/ecx/edx/edi/ebp is
     * the resolver 0x2c720's bridged rare paths, and 0x2c720 provably reads NONE of them (it clobbers
     * ebx at 0x2c732 before any read; ecx/edi/ebp have ZERO occurrences in 0x2c720..0x2cbac; edx is
     * always `sub edx,edx`-cleared before use). 0x2bc3c itself never reads them either (pure
     * pass-through). The prior call_orig bridge already `memset`'d io to 0 -> it ran the ORIGINAL
     * 0x2bc3c with ebx=ecx=edx=edi=ebp=0, and the type-0/1/4 rwss_bridge path does the same, so
     * register-zeros IS the established production input. The memory globals the resolver actually
     * reads ([0x85260]/[0x85264]) are set above, identical to the bridge. No batch-oracle test
     * exercises secpass (only the in-game ROTH_LIFT_DIFF differential lift_diff_secsurf does), so this
     * is oracle-neutral; the differential still traps+diffs 0x2bc3c via the live-swap invoke, now
     * threading the real trapped regs into the explicit params. Pass 0 for ebx/ecx/edx/edi/ebp to
     * exactly match the prior bridge's memset (disasm-proven don't-cares). */
    render_world_secondary_surface(rec, /*ebx*/0, /*ecx*/0, /*edx*/0, /*edi*/0, /*ebp*/0,
                                          es, fs, gs);
}

void render_secondary_surface_list(uint16_t es, uint16_t fs, uint16_t gs)
{
    if (G8(VA_g_has_secondary_surfaces) == 0) return;                                      /* g_has_secondary_surfaces */
    int16_t cx = (int16_t)G16(VA_g_secondary_surface_count);                               /* count */
    uint32_t rec = (uint32_t)GADDR(VA_g_secondary_surface_list);
    do { secpass_render_record(rec, es, fs, gs); rec += 0x10; } while (--cx > 0);   /* dec cx; jg */
}

void render_secondary_surface_pass1(uint16_t es, uint16_t fs, uint16_t gs)
{
    if (G8(VA_g_has_secondary_surfaces) == 0) return;
    G8(VA_g_secondary_subpass_id) = 1;                                                   /* g_secondary_subpass_id = 1 */
    G8(VA_g_secondary_subpass_flags) = (uint8_t)(G8(VA_g_secondary_subpass_flags) | 1);                          /* g_secondary_subpass_flags |= 1 */
    int16_t cx = (int16_t)G16(VA_g_secondary_surface_count);
    uint32_t rec = (uint32_t)GADDR(VA_g_secondary_surface_list);
    uint16_t saved = (uint16_t)G16(VA_g_world_span_bottom);                          /* save + swap clip bound */
    G16(VA_g_world_span_bottom) = (uint16_t)G16(VA_g_format_flags + 0x23);
    do {
        uint8_t ty = *(volatile uint8_t *)(uintptr_t)(rec + 8);
        int render = (ty != 0);                                       /* type 0 -> skip */
        if (render && ty != 1) {                                      /* type>=2 -> split filter */
            int16_t param = *(volatile int16_t *)(uintptr_t)(rec + 0xa);
            if (param < (int16_t)G16(VA_g_reflection_view_list + 0x8a)) render = 0;            /* jl 0x2b3de */
        }
        if (render) secpass_render_record(rec, es, fs, gs);
        rec += 0x10;
    } while (--cx > 0);
    G16(VA_g_world_span_bottom) = saved;                                             /* restore clip bound */
    G8(VA_g_secondary_subpass_id) = 0;
}

void render_secondary_surface_pass2(uint16_t es, uint16_t fs, uint16_t gs)
{
    if (G8(VA_g_has_secondary_surfaces) == 0) return;
    G8(VA_g_secondary_subpass_id) = 2;                                                   /* g_secondary_subpass_id = 2 */
    G8(VA_g_secondary_subpass_flags) = (uint8_t)(G8(VA_g_secondary_subpass_flags) | 2);                          /* g_secondary_subpass_flags |= 2 */
    int16_t cx = (int16_t)G16(VA_g_secondary_surface_count);
    uint32_t rec = (uint32_t)GADDR(VA_g_secondary_surface_list);
    uint16_t saved = (uint16_t)G16(VA_g_world_span_top);                          /* the OTHER clip bound */
    G16(VA_g_world_span_top) = (uint16_t)G16(VA_g_format_flags + 0x25);
    do {
        uint8_t ty = *(volatile uint8_t *)(uintptr_t)(rec + 8);
        int render = (ty != 0);
        if (render && ty != 1) {
            int16_t param = *(volatile int16_t *)(uintptr_t)(rec + 0xa);
            if (param >= (int16_t)G16(VA_g_reflection_view_list + 0x8a)) render = 0;          /* jge 0x2b47a (pass2: below the line) */
        }
        if (render) secpass_render_record(rec, es, fs, gs);
        rec += 0x10;
    } while (--cx > 0);
    G16(VA_g_world_span_top) = saved;
    G8(VA_g_secondary_subpass_id) = 0;
}

/* store_secondary_surface_record (0x289de) — leaf, pure obj3. Builds the secondary-surface clip/render
 * record at 0x84964 IF the surface's vertical screen span (0x90958 top / 0x90960 bottom, clamped to the
 * split window 0x85314/0x85316) is non-empty. Stores: +0 (0x84964)=param_1 (EAX), +8 (0x8496c)=height,
 * +4 (0x84968)=center-Y offset (rel view half-height 0x84960), +6 (0x8496a)=center-X offset (rel 0x84962),
 * +0xa (0x8496e)=width, +0xc (0x84970)=g_view_clip_plane (0x85264), +0x10 (0x84974)=0x90a44. All math is
 * 16-bit (the original loads dx/cx/ax as words; the add/sub spill into edx/ecx but only the low 16 is ever
 * stored, so the high garbage is irrelevant). Returns the original's final EAX (=eax_in on the no-overlap
 * early-out, else [0x90a44]) for the live-swap clobber. */
uint32_t store_secondary_surface_record(uint32_t eax_in)
{
    int16_t dx = (int16_t)G16(VA_g_wall_proj_y3 + 0xa);
    if (!(dx > (int16_t)G16(VA_g_camera_cos + 0x2))) dx = (int16_t)G16(VA_g_camera_cos + 0x2);   /* dx = max(top, 0x85314) */
    int16_t cx = (int16_t)G16(VA_g_wall_proj_y3 + 0x12);
    if (!(cx < (int16_t)G16(VA_g_camera_cos + 0x4))) cx = (int16_t)G16(VA_g_camera_cos + 0x4);   /* cx = min(bottom, 0x85316) */
    if (cx <= dx) return eax_in;                                     /* no overlap -> ret (jle 0x28a78) */

    G32(VA_g_current_decoded_frame + 0x20) = (int32_t)eax_in;                                  /* +0 param_1 */
    G16(VA_g_current_decoded_frame + 0x28) = (uint16_t)((uint16_t)cx - (uint16_t)dx);          /* +8 height = cx - dx */
    int16_t cy = (int16_t)((uint16_t)dx + (uint16_t)cx);             /* edx = dx + cx (low16) */
    cy = (int16_t)((int16_t)(cy >> 1) - (int16_t)G16(VA_g_current_decoded_frame + 0x1c));      /* sar 1; -= half-height */
    G16(VA_g_current_decoded_frame + 0x24) = (uint16_t)cy;                                     /* +4 center-Y offset */
    int16_t cxo = (int16_t)((uint16_t)G16(VA_g_wall_proj_y3 + 0x14) + (uint16_t)G16(VA_g_wall_proj_y3 + 0x16));  /* left + right */
    cxo = (int16_t)((int16_t)(cxo >> 1) - (int16_t)G16(VA_g_current_decoded_frame + 0x1e));    /* sar 1; -= half-width */
    G16(VA_g_current_decoded_frame + 0x26) = (uint16_t)cxo;                                    /* +6 center-X offset */
    G16(VA_g_current_decoded_frame + 0x2a) = (uint16_t)((uint16_t)G16(VA_g_wall_proj_y3 + 0x16) - (uint16_t)G16(VA_g_wall_proj_y3 + 0x14));  /* +0xa width */
    G32(VA_g_current_decoded_frame + 0x2c) = G32(VA_g_view_clip_plane);                                     /* +0xc g_view_clip_plane */
    G32(VA_g_current_decoded_frame + 0x30) = G32(VA_g_map_das_fat_buffer + 0xc);                                     /* +0x10 */
    return (uint32_t)G32(VA_g_map_das_fat_buffer + 0xc);                                   /* final EAX */
}

/* render_world_secondary_surface (0x2bc3c) — the per-secondary-surface renderer (~2.6KB, 0x2b581..0x2bfa3).
 * 5-way dispatch on the record type [esi+8] (0/1/4/0xff/other), each a render sub-path; reaches the wall
 * driver (0x36b39) / store_secondary_surface_record (0x289de) / billboard (0x2d70c) / the texture resolver
 * (0x2c720) / sprite-anim decode, plus two indirect calls. INCREMENTAL LIFT: transcribe one type-path at a
 * time; any not-yet-transcribed type BRIDGES the whole original from the entry. The differential restores
 * obj3+fb between the original and lift runs, so a bridged path passes trivially while a transcribed path
 * gets real verification — the diff stays green as paths convert one by one. esi=record, es/fs/gs=entry
 * selectors (forwarded to the bridge / the path callees). */
/* path-coverage counters (host prints them) so we can confirm the transcribed type-0xff path is actually
 * EXERCISED — a bridged path is trivially green, so green only proves the transcription if g_rwss_lin > 0. */
volatile unsigned long g_rwss_lin = 0, g_rwss_bridged = 0;
volatile unsigned long g_rwss_rot = 0, g_rwss_rot_cull = 0, g_rwss_rot_flip = 0;   /* rotated-tail coverage */
volatile unsigned long g_rwss_type[256];   /* histogram of [record+8] type bytes seen (which paths matter) */
volatile int g_rwss_dbg_skiprender = 0;    /* host-set (ROTH_RWSS_SKIPRENDER): debug the projection bug */
volatile int g_rwss_live = 0;              /* host-set (ROTH_RWSS_LIVE): full transcription (WITH render) for types 2/3/0xff */
volatile unsigned long g_rwss_badsel = 0;  /* wall-driver calls skipped due to invalid fs selector ([0x909b0]) */
uint32_t (*g_os_sel_base)(uint16_t) = NULL;   /* host hook -> dpmi_sel_base (selector validity) */
uint32_t (*g_os_soft_int)(uint8_t, regs_t *) = NULL;   /* host hook -> host_soft_int (inline int 0x21/0x10) */
void (*g_os_port_out)(uint16_t, uint8_t) = NULL;       /* host hook -> host_dac_port_out (GDV fmt-1 DAC fade) */
uint8_t (*g_os_port_in)(uint16_t) = NULL;              /* host hook: byte port IN (unwired — Mode-X latch save; see engine.h) */
void (*g_os_publish_frame)(void) = NULL;               /* host hook -> host_gdv_publish_frame (lifted gdv_emit_decoded_frame) */
volatile int g_gdv_loop_hosting = 0;                     /* set by gdv_decode_frame around its loop call_orig bridge; the host SIGALRM surrogate (shm_tick) then drives the frozen GDV timer ISR's per-tick frame decode (see traps.c) */
volatile uint32_t g_rwss_dbg[4];           /* [0]=84ac0-entry [1]=[0x90956] [2]=cl [3]=resolver-CF */

static void rwss_bridge(uint32_t esi_in, uint16_t es, uint16_t fs, uint16_t gs)
{
    g_rwss_bridged++;
    regs_t io; memset(&io, 0, sizeof io);
    io.va = 0x2bc3cu + OBJ_DELTA; io.esi = esi_in; io.es = es; io.fs = fs; io.gs = gs;  /* [ORACLE-FALLBACK] */
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    roth_unreachable(0x2bc3cu);   /* render_world_secondary_surface Class-B live bridge — render tier */
#endif
}

/* perspective project: eax_in * mul / div (signed 32*32->64, 64/32 idiv) + center, clamped [-0x3ffe,0x3ffe].
 * Models the repeated `imul [0x8527c]/[0x85288]; idiv [0x85264]; add [0x909a*]; cmp/jl clamp` idiom. */
static int32_t rwss_proj(int32_t eax_in, int32_t mul, int32_t div, int32_t center)
{
    int64_t p = (int64_t)eax_in * (int64_t)mul;
    int32_t q = (int32_t)(p / (int64_t)div);
    q += center;
    if (q < -0x3ffe) q = -0x3ffe; else if (q >= 0x3ffe) q = 0x3ffe;
    return q;
}

/* bridge a callee of 0x2bc3c with explicit eax/ecx/esi + entry selectors; return CF (eflags bit0). */
static int rwss_call(uint32_t va, uint32_t eax, uint32_t ecx, uint32_t esi, uint16_t es, uint16_t fs, uint16_t gs)
{
    regs_t io; memset(&io, 0, sizeof io);
    io.va = va + OBJ_DELTA; io.eax = eax; io.ecx = ecx; io.esi = esi;
    io.es = es; io.fs = fs; io.gs = gs;
#ifndef ROTH_STANDALONE
    call_orig(&io);
    return (int)(io.eflags & 1u);
#else
    roth_unreachable(va);         /* rwss callee bridge — render tier */
    return 1;
#endif
}

/* render_parallax_sky_columns (0x38d6c) — the PARALLAX-SKY column renderer (NOT a texture-source resolver;
 * playtest confirmed). Called per-column by
 * the wall driver (0x36b39) when the sky flag g_parallax_sky_active (0x90a2a, = face flag &0x40) is set and
 * the wall top is above the horizon (0x9096e) — it fills that open region with the per-map sky texture
 * g_das_special_fat_index (0x89f34, map-header +0x18), offsetting the source column by g_sprite_view_angle
 * (the parallax drift). Gates on the subpass (0x90a48); resolves the sky DAS block via get_loaded_das_block_
 * for_index (0x414f4, LIFTED); optionally advances the sprite-anim frame (0x38fec); builds a 0x1a-byte frame;
 * then blits the columns (fs=block[8] source -> es:[edi] framebuffer), in two variants (double/single scanline).
 * It does NOT write [0x84980] — the caller saves/restores it around the call (the old "src'" puzzle).
 * ABI: EDI=screenX, ECX=colcount, ESI=0, src=[0x84980] (unused stack arg). es/fs/gs = entry selectors.
 * BOTH blit variants are transcribed natively: SINGLE-scanline ([0x8a310]==0, 0x38e1a) and DOUBLE-scanline
 * ([0x8a310]!=0, 0x38eff; selected when the 3D viewport height >= 0xbe, set at 0x409cf). The blit is a
 * faithful 1:1 of the x86 (byte/word sub-registers, the Watcom even/odd unroll incl. its N==2->0 quirk). */
volatile unsigned long g_texres_native = 0, g_texres_double = 0, g_texres_bridged = 0;

uint32_t render_parallax_sky_columns(uint32_t edi, uint32_t ecx, uint32_t esi, uint32_t src,
                                             uint16_t es, uint16_t fs, uint16_t gs)
{
    (void)esi;                                  /* stack arg 'src' is unread by the body */

    /* Defensive: the native blits need the framebuffer/texture segment bases. If the host selector hook
     * is unavailable (never the case in-game), bridge the whole original once. */
    #ifndef ROTH_STANDALONE
    if (g_os_sel_base == NULL) {
        g_texres_bridged++;
        regs_t io; memset(&io, 0, sizeof io);
        io.va = 0x38d6cu + OBJ_DELTA; io.edi = edi; io.ecx = ecx; io.esi = esi;  /* [ORACLE-FALLBACK] */
        io.es = es; io.fs = fs; io.gs = gs;
        io.nstack = 1; io.stack[0] = src;
        call_orig(&io);
        return io.eax;
    }
    #else
    (void)src; (void)es; (void)fs; (void)gs;   /* the compiled-out fallback was their only consumer
                                                * (the native body resolves bases via g_os_sel_base) */
    #endif

    /* ===== shared setup (0x38d6c..0x38e0d) ===== */
    if (G8(VA_g_world_render_subpass_kind) != 0) { G8(VA_g_world_render_subpass_kind + 0x1) = 5; return 0; }            /* 0x38d6c gate -> 0x38fe3 (eax=0) */

    uint8_t *blk = (uint8_t *)get_loaded_das_block_for_index((uint16_t)G16(VA_g_das_special_fat_index));  /* 0x38d81 = 0x414f4 */
    if (blk == NULL) return 0;                                       /* 0x38d88 -> 0x38efc (eax=0) */

    G32(VA_g_active_world_remap_base + 0x8) = (uint32_t)blk[0];                                 /* 0x38d94 [0x8a2b4]=blk[0] */
    if ((*(uint16_t *)(blk + 0xa) & 0x100) != 0) {                  /* 0x38d99 animated frame? */
        /* re-point 0x38fec advance_das_sprite_animation_frame: pure-DS leaf (ESI=blk; no segment use). */
        advance_das_sprite_animation_frame((uint32_t)(uintptr_t)blk);
    }

    /* 0x38da6: build the 0x1a-byte projection frame (ebp locals) */
    int32_t  f_dir = 1;                                              /* [ebp+0] step direction */
    if (G8(VA_g_render_x_flip_flag) != 0) {                                          /* 0x38db0 x-flip */
        f_dir = -1; edi = (uint32_t)((int32_t)G32(VA_g_current_decoded_frame + 0x10) - (int32_t)edi);
    }
    uint32_t f4  = (uint32_t)G32(VA_g_span_texU_accum);                           /* [ebp+4] texU accumulator (hi word=int) */
    uint32_t f8  = (uint32_t)G32(VA_g_wall_vstep);                           /* [ebp+8] texU step (== return EAX) */
    uint16_t fC  = (uint16_t)((uint16_t)G16(VA_g_sprite_view_angle) << 1);          /* [ebp+0xc] [0x909f8]*2 */
    uint16_t fH  = (uint16_t)(*(uint16_t *)(blk + 0xe));             /* [ebp+0xe] block height */
    uint8_t  f10 = (uint8_t)(fH - 1);                                /* [ebp+0x10] height-1 */
    uint16_t f12 = (uint16_t)((uint16_t)G16(VA_g_floor_tex_caps + 0x44) + (uint16_t)G32(VA_g_active_world_remap_base + 0x8));  /* [ebp+0x12] start row */
    uint16_t f16 = (uint16_t)G16(VA_g_view_bound_bottom);                           /* [ebp+0x16] max run clamp */
    uint16_t blk8 = (uint16_t)(*(uint16_t *)(blk + 8));              /* fs = texture source selector */

    uint32_t es_base = g_os_sel_base((uint16_t)G16(VA_g_render_target_selector));      /* 0x38e28 es = framebuffer */
    uint32_t fs_base = g_os_sel_base(blk8);

    uint32_t colidx = (uint16_t)((uint16_t)G16(VA_g_wall_proj_y3 + 0xa) + (uint16_t)G16(VA_g_span_texV_accum + 0x1e));   /* 0x38e1a ebx (high 0) */
    int32_t  colcount = (int32_t)ecx;
    /* The scanline stride is SMC-patched (not the static 0x140): the rasterizer setup at 0x36464 writes the
     * framebuffer row stride into every stride immediate in this blit — both the `mov edx,0x140` (0x38e5c/
     * 0x38f3a) AND the `add edi,0x140` literals (0x38ea6/0x38f8f/0x38f98). It's 0x140 for 320-wide modes but
     * 0x280 for 640-wide (VESA). Read the patched value (all sites get the same word) instead of hardcoding. */
    uint32_t stride = *(volatile uint32_t *)(uintptr_t)(0x38e5cu + OBJ_DELTA);

    #define INC_BH(b) ((b) = (uint16_t)(((b) & 0x00ff) | (((((b) >> 8) + 1) & 0xff) << 8)))
    #define WR(off, v) (*(uint8_t *)(uintptr_t)(es_base + (uint32_t)(off)) = (uint8_t)(v))
    #define RD(b)      (*(uint8_t *)(uintptr_t)(fs_base + (uint16_t)(b)))

    /* ===== DOUBLE-scanline variant (0x38eff): each texture row -> two adjacent screen rows ===== */
    if (G8(VA_g_render_double_scanline_flag) != 0) {                                         /* 0x38e0d -> 0x38eff */
        g_texres_double++;
        do {                                                        /* 0x38f0d per-column outer loop */
            uint32_t e; uint16_t bx; uint8_t al; int32_t run; int16_t si_w;
            int16_t texU_int = (int16_t)((f4 >> 16) & 0xffff);      /* dx = [ebp+6] */
            if (texU_int <= 0) goto dcol_done;                      /* or dx,dx; jle 0x38fcb */

            run = texU_int;                                         /* ecx = dx */
            if ((int16_t)run > (int16_t)f16) run = (int16_t)f16;    /* clamp (signed jle) */
            run = (int32_t)((uint16_t)run) + 1;                    /* inc ecx */

            e  = edi;                                              /* push edi */
            al = 0;
            {   uint8_t bl = G8(VA_g_render_column_source_table + colidx);                 /* mov bl,[ebx+0x8c484] */
                bx = (uint16_t)((colidx & 0xff00) | bl);
                bx = (uint16_t)(bx - fC);                          /* sub bx,[ebp+0xc] */
                bx = (uint16_t)((bx & 0x00ff) | (((uint16_t)f12 & 0xff) << 8)); }  /* mov bh,[ebp+0x12] */
            si_w = (int16_t)f12;                                   /* mov si,[ebp+0x12] */

            if (si_w < 0) {                                        /* jns 0x38f68 not taken: top-clip */
                int16_t ax = si_w;
                bx = (uint16_t)(bx & 0x00ff);                      /* sub bh,bh */
                uint8_t t = RD(bx);                                /* mov bh,fs:[ebx] */
                bx = (uint16_t)((bx & 0x00ff) | ((uint16_t)t << 8));
                for (;;) {                                         /* 0x38f4e */
                    WR(e, t); e += stride;                          /* es:[edi]=bh; +0x140 (row 1) */
                    if (--run <= 0) goto dcol_done;                /* dec cx; jle 0x38fc9 */
                    WR(e, t); e += stride;                          /* row 2 */
                    if (--run <= 0) goto dcol_done;                /* dec cx; jle */
                    if (++ax == 0) break;                          /* inc ax; jne 0x38f4e */
                }
                bx = (uint16_t)(bx & 0x00ff);                      /* sub bh,bh */
                si_w = 0;                                          /* sub esi,esi */
            }

            /* 0x38f68 */
            {   uint16_t ax2 = (uint16_t)((uint16_t)(((uint16_t)run) >> 1) + (uint16_t)si_w);  /* shr ax,1; add esi */
                if ((int16_t)ax2 < (int16_t)fH) {                  /* cmp ax,[ebp+0xe]; jl 0x38fb4: middle */
                  Ldfb4: al = RD(bx);                              /* 0x38fb4 */
                    WR(e, al); e += stride;                         /* row 1 */
                    if (--run <= 0) goto dcol_done;                /* dec ecx; jle 0x38fc9 */
                    WR(e, al); e += stride;                         /* row 2 (same texel) */
                    INC_BH(bx);                                    /* inc bh */
                    if (--run > 0) goto Ldfb4;                     /* dec ecx; jg 0x38fb4 */
                    goto dcol_done;
                } else {                                          /* bottom-clip 0x38f75 */
                    int16_t dcount = (int16_t)((uint16_t)fH - (uint16_t)si_w);  /* mov dx,[ebp+0xe]; sub dx,si */
                    if (dcount > 0) {                              /* jg 0x38f87 */
                        do {                                       /* 0x38f87 */
                            al = RD(bx);
                            WR(e, al); e += stride;                 /* row 1 */
                            WR(e, al); e += stride;                 /* row 2 */
                            run -= 2;                              /* sub ecx,2 */
                            INC_BH(bx);                            /* inc bh */
                        } while (--dcount > 0);                    /* dec dx; jg 0x38f87 */
                    } else {                                      /* 0x38f7f: no real texels */
                        bx = (uint16_t)((bx & 0x00ff) | ((uint16_t)f10 << 8));  /* mov bh,[ebp+0x10] */
                        al = RD(bx);
                    }
                    if (run > 0) {                                /* or ecx,ecx; jle 0x38fc9 */
                        do { WR(e, al); e += stride; } while (--run > 0);   /* 0x38faa dec ecx; jg */
                    }
                    goto dcol_done;                               /* 0x38fb2 jmp */
                }
            }
          dcol_done:
            edi = (uint32_t)((int32_t)edi + f_dir);               /* add edi,[ebp] */
            f4 += f8;                                             /* [ebp+4]+=[ebp+8] */
            colidx++;                                             /* inc ebx */
        } while (--colcount > 0);                                 /* dec ecx; jg 0x38f0d */
        return f8;                                                /* EAX = [ebp+8] */
    }

    g_texres_native++;
    /* ===== SINGLE-scanline variant (0x38e1a) ===== */
    do {                                                            /* 0x38e2f per-column outer loop */
        uint32_t e; uint16_t bx; uint8_t al; int32_t run; int16_t si_w;
        int16_t texU_int = (int16_t)((f4 >> 16) & 0xffff);          /* dx = [ebp+6] */
        if (texU_int <= 0) goto col_done;                           /* test dx,dx; jle 0x38ee7 */

        run = texU_int;                                             /* ecx = dx */
        if ((int16_t)run > (int16_t)f16) run = (int16_t)f16;        /* cmp cx,[ebp+0x16]; jle (signed clamp) */
        run = (int32_t)((uint16_t)run) + 1;                        /* inc ecx */

        e  = edi;                                                  /* push edi (working dest) */
        al = 0;
        {   uint8_t bl = G8(VA_g_render_column_source_table + colidx);                     /* mov bl,[ebx+0x8c484] */
            bx = (uint16_t)((colidx & 0xff00) | bl);               /* (bh=colidx>>8) */
            bx = (uint16_t)(bx - fC);                              /* sub bx,[ebp+0xc] */
            bx = (uint16_t)((bx & 0x00ff) | (((uint16_t)f12 & 0xff) << 8)); }  /* mov bh,[ebp+0x12] */
        si_w = (int16_t)f12;                                       /* mov si,[ebp+0x12] */

        if (si_w < 0) {                                            /* jns 0x38e80 not taken: top-clip */
            int16_t ax = si_w;                                    /* mov eax,esi */
            bx = (uint16_t)(bx & 0x00ff);                         /* sub bh,bh */
            uint8_t t = RD(bx);                                   /* mov bh,fs:[ebx] (bh=0 -> addr=bl) */
            bx = (uint16_t)((bx & 0x00ff) | ((uint16_t)t << 8));  /* bh = texel */
            for (;;) {                                            /* 0x38e70 */
                WR(e, (uint8_t)(bx >> 8)); e += stride;            /* mov es:[edi],bh; add edi,edx */
                if (--run == 0) goto col_done;                    /* dec ecx; je 0x38ee5 */
                if (++ax == 0) break;                             /* inc ax; jne 0x38e70 */
            }
            bx = (uint16_t)(bx & 0x00ff);                         /* sub bh,bh */
            si_w = 0;                                             /* sub esi,esi */
        }

        /* 0x38e80 */
        {   uint16_t ax2 = (uint16_t)((uint16_t)run + (uint16_t)si_w);
            if (ax2 > fH) {                                       /* cmp ax,[ebp+0xe]; ja (not jbe): bottom-clip */
                int16_t dcount = (int16_t)((uint16_t)fH - (uint16_t)si_w);  /* mov dx,[ebp+0xe]; sub dx,si */
                if (dcount > 0) {                                 /* jg 0x38e9c */
                    run -= dcount;                                /* sub ecx,edx */
                    do {                                          /* 0x38e9e */
                        al = RD(bx); WR(e, al); e += stride;       /* mov al,fs:[ebx]; store; add edi,0x140 */
                        INC_BH(bx);                               /* inc bh */
                    } while (--dcount > 0);                       /* dec dx; jg 0x38e9e */
                } else {                                          /* 0x38e94: no real texels */
                    bx = (uint16_t)((bx & 0x00ff) | ((uint16_t)f10 << 8));  /* mov bh,[ebp+0x10] */
                    al = RD(bx);                                  /* mov al,fs:[ebx] */
                }
                /* 0x38eb0 fill last texel 'al', run/2 even/odd unroll (edx=0x140) */
                { int odd = run & 1; run = (int32_t)((uint32_t)run >> 1);   /* shr ecx,1 */
                  if (odd) goto Lebd;                             /* jb 0x38ebd */
                  if (--run <= 0) goto col_done;                  /* dec ecx; jle 0x38ee5 */
                  Leb8: WR(e, al); e += stride;                    /* 0x38eb8 EVEN (fall through) */
                  Lebd: WR(e, al); e += stride;                    /* 0x38ebd ODD */
                  if (--run >= 0) goto Leb8;                      /* dec ecx; jge 0x38eb8 */
                  goto col_done; }                                /* 0x38ec5 jmp done */
            } else {                                              /* 0x38ec7 middle: emit run texels w/ row step */
                int odd = run & 1; run = (int32_t)((uint32_t)run >> 1);     /* shr ecx,1 */
                if (odd) goto Led8;                               /* jb 0x38ed8 */
                if (--run <= 0) goto col_done;                    /* dec ecx; jle 0x38ee5 */
                Lece: al = RD(bx); WR(e, al); e += stride; INC_BH(bx);   /* 0x38ece EVEN (fall through) */
                Led8: al = RD(bx); WR(e, al); e += stride; INC_BH(bx);   /* 0x38ed8 ODD */
                if (--run >= 0) goto Lece;                        /* dec ecx; jge 0x38ece */
            }
        }
      col_done:
        edi = (uint32_t)((int32_t)edi + f_dir);                   /* add edi,[ebp] */
        f4 += f8;                                                 /* [ebp+4]+=[ebp+8] */
        colidx++;                                                 /* inc ebx */
    } while (--colcount > 0);                                     /* dec ecx; jg 0x38e2f */

    #undef INC_BH
    #undef WR
    #undef RD
    return f8;                                                    /* EAX = [ebp+8] */
}

/* emit_world_face_spans (0x2c720) — the secondary-surface (floor/ceil + sprite) texture/block resolver.
 * 0x2bc3c's bridged resolver (the one that returns the live ESI record) + a second caller at 0x2a516. Walks
 * g_das_entry_status_table (0x86d30) by the cursor [0x90a78], resolves the DAS cache slot (0x89930) to the
 * block record, handles the placeholder/animation/entity-callback cases, and sets the source globals
 * ([0x8493c]/[0x849a0]/[0x84980]/[0x84ab8]/...). ABI: EAX=descriptor (+entry esi/ebx/ecx/edx/edi/ebp + es/fs/gs)
 * -> ESI=resolved record (return), *out_eax=EAX, *out_cf=CF (the caller's load-fail `jb`).
 * INCREMENTAL: the common path (simple cache-slot resolve + the block-setup tails) is native; the rarer
 * branches bridge the WHOLE original from the top (idempotent re-run): cur==0xffff default record, the
 * entity-callback (status&0x200), placeholders (al>=0xfc), block-moved refresh (0x41250), span-callback
 * (0x39093), and animated textures (block[0xa]&0x100). */
volatile unsigned long g_faceres_native = 0, g_faceres_bridged = 0;

__attribute__((unused))   /* imgfree lane: every faceres branch is native now (0xfc/0xfe walks landed) */
static uint32_t faceres_bridge(uint32_t eax, uint32_t esi, uint32_t ebx, uint32_t ecx, uint32_t edx,
                               uint32_t edi, uint32_t ebp, uint16_t es, uint16_t fs, uint16_t gs,
                               uint32_t *out_eax, uint32_t *out_cf)
{
    g_faceres_bridged++;
    regs_t io; memset(&io, 0, sizeof io);
    io.va = 0x2c720u + OBJ_DELTA;  /* [ORACLE-FALLBACK] */
    io.eax = eax; io.esi = esi; io.ebx = ebx; io.ecx = ecx; io.edx = edx; io.edi = edi; io.ebp = ebp;
    io.es = es; io.fs = fs; io.gs = gs;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    roth_unreachable(0x2c720u);   /* emit_world_face_spans branch bridge — render tier */
#endif
    if (out_eax) *out_eax = io.eax;
    if (out_cf)  *out_cf  = io.eflags & 1u;
    return io.esi;
}

#ifdef ROTH_STANDALONE
/* ===== faceres join at 0x2c810 (imgfree lane): the 0x39093 span-callback test + the 0x2ca22 join +
 * every tail (early 0x8000 / anim 0x2caf0 / simple 0x2cb66 / wall resolve 0x2ca55). Factored out of
 * emit_world_face_spans so TWO entrants share the verbatim flow: the normal path (descword =
 * the status word st, all commit stores done) and the cur==0xffff default-record path (descword =
 * desc with ah=0xff — bit 8 set, so the callback carrier ALWAYS runs there). Returns the
 * resolved record (the final esi). The trap lane keeps its own in-function copy of the tails. */
static uint32_t faceres_join_0x2c810(uint32_t descword, uint32_t block,
                                     uint32_t *out_eax, uint32_t *out_cf)
{
    uint32_t jeax = descword;                                        /* eax entering the 0x2ca22 join */
    if (descword & 0x100) {                                          /* 0x2c810 test ah,1 (span callback) */
        /* 0x2c819: ebx=[0x90a78]; call 0x39093 = { eax=ebx; null [0x90a34] -> RAW eax through (no
         * cwde); else cwde, edx=esi=block, es=ds, call hook } — residue eax NOT restored (its only
         * consumer is the 0x8000 early tail below); ebx clobbered but dead at this caller. */
        uint32_t d = (uint32_t)G32(VA_g_current_das_entry_id);
        if (G32(VA_g_span_callback) == 0) jeax = d;                             /* 0x3909d null slot */
        else jeax = rwss_span_callback_dispatch((uint32_t)(int32_t)(int16_t)(uint16_t)d, block);
    }
    /* join 0x2ca22 — [esi+0xa] RE-READ from memory (the callback can fire RAW command chains) */
    uint16_t b_a = *(volatile uint16_t *)(uintptr_t)(block + 0xa);
    if (b_a & 0x8000) { if (out_eax) *out_eax = jeax; if (out_cf) *out_cf = 0; return block; }  /* 0x2ca28 -> 0x2cbab clc;ret */

    G8(VA_g_span_textured_mode_flag)  = (uint8_t)((b_a >> 8) & 4);                       /* 0x2ca37 */
    G16(VA_g_secondary_surface_count + 0x2) = b_a;                                             /* 0x2ca3d */

    if (b_a & 0x100) {                                               /* 0x2ca43 -> 0x2caf0 anim (commit
                                                                      * stores + 0x8a352/0x8531a already done above,
                                                                      * original order). Callees lifted (0x38fec/0x2b5ea). */
        if (*(volatile uint16_t *)(uintptr_t)(block + 0x16) != 0xfffe) {   /* 0x2caf0 cmp [esi+0x16],-2 */
            advance_das_sprite_animation_frame(block);             /* 0x2caf7 call 0x38fec (publishes [0x84980]) */
            uint32_t e4 = *(volatile uint32_t *)(uintptr_t)(block + 4);    /* 0x2cb6e tail (skips the 0x2cb66 store) */
            G32(VA_g_current_proc_tag + 0x11c) = e4;
            int32_t ev = (int32_t)(int16_t)(uint16_t)e4; ev <<= 8;        /* cwde; shl eax,8 */
            if (G16(VA_g_world_surface_draw_flags) & 2) ev = -ev;                              /* 0x2cb7a neg */
            G32(VA_g_visible_extent_list + 0x3c) += (uint32_t)ev;
            G32(VA_g_current_proc_tag + 0x4) -= block;
            if (out_eax) *out_eax = (uint32_t)ev;
            if (out_cf) *out_cf = 0;
            return block;
        } else {                                                         /* 0x2cafe == 0xfffe: surface-linked select */
            uint32_t e4 = *(volatile uint32_t *)(uintptr_t)(block + 4);   /* 0x2cafe */
            G32(VA_g_current_proc_tag + 0x11c) = e4;                                            /* 0x2cb01 */
            int32_t ev = (int32_t)(int16_t)(uint16_t)e4; ev <<= 8;        /* cwde; shl 8 */
            if (G16(VA_g_world_surface_draw_flags) & 2) ev = -ev;                              /* 0x2cb15 neg */
            G32(VA_g_visible_extent_list + 0x3c) += (uint32_t)ev;                                /* 0x2cb17 */
            G32(VA_g_current_proc_tag + 0x4) = 0;                                            /* 0x2cb1d */
            uint32_t f = select_surface_anim_frame((uint32_t)G32(VA_g_map_das_fat_buffer + 0xc), block);  /* 0x2cb27/2c call 0x2b5ea */
            G32(VA_g_render_source_base_ptr) = f + 0x10;                                     /* 0x2cb31/34 lea eax,[esi+0x10] (esi=ret) */
            if (out_eax) *out_eax = f + 0x10;
            if (out_cf) *out_cf = 0;
            return f;
        }
    }

    if (!(b_a & 0x40)) {                                            /* 0x2ca4d je 0x2cb66 (simple) */
        G32(VA_g_render_source_base_ptr) = block + 0x10;                               /* 0x2cb66 */
        uint32_t e4 = *(volatile uint32_t *)(uintptr_t)(block + 4);/* 0x2cb6e Ltail */
        G32(VA_g_current_proc_tag + 0x11c) = e4;
        int32_t ev = (int32_t)(int16_t)(uint16_t)e4;               /* cwde */
        ev <<= 8;                                                  /* shl eax,8 */
        if (G16(VA_g_world_surface_draw_flags) & 2) ev = -ev;                            /* neg */
        G32(VA_g_visible_extent_list + 0x3c) += (uint32_t)ev;
        G32(VA_g_current_proc_tag + 0x4) -= block;
        if (out_eax) *out_eax = (uint32_t)ev;
        if (out_cf) *out_cf = 0;
        return block;
    }

    /* 0x2ca55: per-wall angle/coord resolve */
    uint16_t cw = *(volatile uint16_t *)(uintptr_t)(block + 0x10);
    uint32_t ci;                                                   /* coord index (0x2cb42 input) */
    if (!(cw & 0x8000)) {                                          /* 0x2cb3b */
        ci = (uint16_t)G16((VA_g_floor_tex_caps + 0x2) + (uint32_t)cw);
    } else {
        uint32_t cdiv = ((int32_t)G32(VA_g_view_clip_plane) > 0)
            ? (uint32_t)(((int32_t)((uint32_t)G32(VA_g_visible_extent_list + 0x3c) << 6)) / (int32_t)G32(VA_g_view_clip_plane)) : 0;
        uint32_t t = (uint32_t)(uint8_t)G8(VA_g_current_proc_tag + 0x11a);
        if (cw & 0x2000) {                                         /* 0x2cab1: >>4 & 0x1e, +0x110 */
            t = t * 2 + 0x110;
            t = (uint16_t)(t - (uint16_t)G16(VA_g_sprite_view_angle));
            t = t - cdiv;
            ci = (t >> 4) & 0x1e;
        } else {                                                  /* 0x2ca6f: >>5 & 0xe, +0x120 */
            t = t * 2 + 0x120;
            t = (uint16_t)(t - (uint16_t)G16(VA_g_sprite_view_angle));
            t = t - cdiv;
            ci = (t >> 5) & 0xe;
        }
    }
    /* 0x2cb42 */
    uint16_t bw = *(volatile uint16_t *)(uintptr_t)(block + ci + 0x12);
    uint32_t idx = bw;
    if (bw & 0x8000) { G16(VA_g_world_surface_draw_flags) ^= 2; idx = bw & 0x7fff; }     /* test bh,bh; jns */
    idx <<= 4;
    block += idx;                                                 /* esi advanced */
    /* 0x2cb95 Ltail2 */
    G32(VA_g_current_proc_tag + 0x11c) = *(volatile uint32_t *)(uintptr_t)(block + 4);
    G32(VA_g_render_source_base_ptr) = block + 0x10;
    G32(VA_g_current_proc_tag + 0x4) -= block;
    if (out_eax) *out_eax = block + 0x10;
    if (out_cf) *out_cf = 0;
    return block;
}
#endif /* ROTH_STANDALONE */

uint32_t emit_world_face_spans(uint32_t eax, uint32_t esi, uint32_t ebx, uint32_t ecx,
                                      uint32_t edx, uint32_t edi, uint32_t ebp,
                                      uint16_t es, uint16_t fs, uint16_t gs,
                                      uint32_t *out_eax, uint32_t *out_cf)
{
    #define BRIDGE() faceres_bridge(eax, esi, ebx, ecx, edx, edi, ebp, es, fs, gs, out_eax, out_cf)

    /* 0x2c720: descriptor = eax with ah += 0x10; cursor [0x90a78] = descriptor */
    uint32_t desc = (eax & 0xffff00ffu) | (((((eax >> 8) & 0xffu) + 0x10u) & 0xffu) << 8);
    uint16_t cur  = (uint16_t)(desc & 0xffffu);
#ifndef ROTH_STANDALONE
    if (cur == 0xffffu) return BRIDGE();                              /* default record path */
#else
    (void)ebx; (void)ecx; (void)edx; (void)edi; (void)ebp; (void)es; (void)fs; (void)gs;
                               /* imgfree lane: their only consumer was BRIDGE(), now fully retired in
                                * this function (every branch native — see the al>=0xfc walks below) */
    uint32_t pend_84998 = 0;   /* 0x2c728 [0x84998]=0 entry store — deferred with the commit stores; the
                                * al==0xfc walk (0x2c8ed) re-points it to the entity slot record. NO reader
                                * of 0x484994/0x484998 exists anywhere in the image (byte-scan verified),
                                * so only the committed FINAL value is observable. Declared before B1
                                * because B1 commits too (pend is provably 0 there: B1 is entry-only —
                                * the walk retries re-enter BELOW the cur==0xffff test, see the retry
                                * tail's nid<0x8000 note). */
    if (cur == 0xffffu) {                       /* 0x2c7d0: esi = 0x29e58 (the STATIC obj1 default-record
                                                 * ptr cell) ; jmp 0x2c7f7 — B1. Arena-
                                                 * staged: record bytes via gen_obj1data_c.py, the fixup-
                                                 * carrying [0x29e58] cell via roth_boot. eax at 0x2c810
                                                 * = desc (ah=0xff -> the span-callback carrier ALWAYS runs;
                                                 * the hook scans for id 0xffff = no match, id through). */
        g_faceres_native++;
        G32(VA_g_door_worklist + 0xc) = pend_84998;                                    /* 0x2c728 (entry store; pend always 0 at B1) */
        G32(VA_g_current_das_entry_id) = desc;                                          /* 0x2c723 (entry store) */
        uint32_t pcell = 0x29e58u + OBJ_DELTA;
        G32(VA_g_format_flags + 0x1f) = pcell;                                         /* 0x2c7f7 ([0x8493c] = the CELL, not the block) */
        uint32_t blk = *(volatile uint32_t *)(uintptr_t)pcell;        /* 0x2c7fd = 0x29e5c+delta (staged reloc) */
        if (blk == 0) roth_unreachable(0x2c7d0u);                    /* staging regressed — fail loud */
        if (*(volatile uint8_t *)(uintptr_t)(blk - 8) & 4)            /* 0x2c7ff moved test (staged byte = 0:
                                                                       * never taken; transcribed faithfully) */
            refresh_moved_das_cache_block(blk);
        G32(VA_g_current_proc_tag + 0x4) = blk;                                           /* 0x2c80a */
        return faceres_join_0x2c810(desc, blk, out_eax, out_cf);
    }
#endif

#ifdef ROTH_STANDALONE
 reclassify_after_load: ;   /* 0x2c732 retry target (imgfree lazy-load loop) */
#endif
    uint16_t st = (uint16_t)G16(VA_g_das_entry_status_table + (uint32_t)cur * 2);         /* status word */
#ifndef ROTH_STANDALONE
    if (st & 0x200) return BRIDGE();                                  /* entity-callback path */
#else
    if (st & 0x200) {                       /* 0x2c790 — the texture-id REMAP hook (the old
                                             * "entity-callback" name was a misnomer) */
        if (G32(VA_g_pool_check_enabled + 0x28) != 0) {                                      /* 0x2c798 test ebx,ebx */
            uint32_t nid = rwss_id_remap_dispatch((uint32_t)cur);     /* 0x2c79c shr eax,1; es=ds; call [0x8a2a0] */
            G32(VA_g_current_das_entry_id) = nid;                                       /* 0x2c7a6 full 32-bit PLAIN-id store */
            desc = nid; cur = (uint16_t)nid;                          /* keep the deferred commit store + the
                                                                       * loader-fail `*out_eax = desc` faithful */
            st = (uint16_t)G16(VA_g_das_entry_status_table + (uint32_t)cur * 2);          /* 0x2c7ab/ad re-read (ebx = nid*2) */
        } else {
            /* 0x2c7b6 null hook (dead in practice — the hook is installed at map load and faceres only
             * runs with a world loaded): QUIRKY re-double, ebx=cur*2 then add ebx,ebx -> status read
             * from [cur*4+0x86d30]; NO [0x90a78] store. Transcribed faithfully. */
            st = (uint16_t)G16(VA_g_das_entry_status_table + (uint32_t)cur * 4);
        }
        /* fall through = jmp 0x2c75a: re-entry AFTER the ah&2 test — no remap loop */
    }
#endif
    uint8_t al = (uint8_t)st;
    if (al >= 0xfc) {                                                 /* 0x2c75a placeholder/load cases */
#ifndef ROTH_STANDALONE
        return BRIDGE();                                             /* placeholder/load cases */
#else
        /* 0x2c7e0 dispatch: al==0xff/0xfd -> lazy DAS load then retry (0x2c783). The loader
         * (0x40d7c = load_das_block_for_fat_index, image-free-proven) resolves the
         * placeholder to a resident cache slot, so the retry re-reads a slot index (<0xfc) and takes
         * the committed native path. al==0xfc (0x2c829) / 0xfe (0x2c93e) resolve the placeholder to a
         * REAL id below and retry the same loop head. */
        if (al == 0xffu || al == 0xfdu) {
            if (load_das_block_for_fat_index(desc) == 0)      /* clc = loaded -> 0x2c78d jae retry */
                goto reclassify_after_load;
            if (out_eax) *out_eax = desc;                            /* 0x2c78f ret: eax=[0x90a78]=desc, */
            if (out_cf)  *out_cf  = 1u;                              /*   CF=1, esi = the live esi (entry, */
            return esi;                                              /*   or a 0xfc/0xfe walk record) */
        }
        {
        uint32_t nid;                                                /* ebx at the 0x2c924/0x2ca08 join */
        if (al == 0xfcu) {
            /* ---- 0x2c829..0x2c923: sprite/entity placeholder resolve ---- */
            uint32_t srec = (uint32_t)G32(VA_g_map_das_fat_buffer + 0xc);                  /* 0x2c829 esi = current object record */
            uint32_t bref = (uint32_t)*(volatile uint16_t *)(uintptr_t)(srec + 0xc);  /* 0x2c82f/31 sub edx,edx; mov dx,[esi+0xc] */
            if (bref == 0) {                                         /* 0x2c835 no pool-A backref yet -> spawn */
                /* 0x2c839 push eax; es=ds; call 0x4f00d; pop es; pop eax. EDX-RETURN spawn
                 * (engine.h: AH = actor-record idx, ESI = object -> EDX backref / 0). EAX is
                 * clobbered by the original callee but push/popped AT THIS SITE; ESI/EBX are
                 * preserved by the callee (disasm 0x4f00d..0x4f0a9: only edi/ecx saved+used). */
                regs_t io; memset(&io, 0, sizeof io);
                io.eax = (uint32_t)st;                               /* ah = idx (loop head 0x2c74c zeroed eax-high) */
                io.esi = srec;
                bref = (uint32_t)spawn_entity_into_state_pool_a(&io);
            }
            if (bref == 0) {                                         /* 0x2c844/46 pool full -> static table pick */
                uint32_t cdiv = ((int32_t)G32(VA_g_view_clip_plane) > 0)          /* 0x2c84b jle (eax stays 0) */
                    ? (uint32_t)(((int32_t)((uint32_t)G32(VA_g_visible_extent_list + 0x3c) << 6)) / (int32_t)G32(VA_g_view_clip_plane)) : 0;  /* 0x2c855-5e shl 6; cdq; idiv */
                uint32_t t = (uint32_t)(uint8_t)G8(VA_g_current_proc_tag + 0x11a) * 2u + 0x120u;   /* 0x2c865-6f bl*2+0x120 */
                t = (uint16_t)(t - (uint16_t)G16(VA_g_sprite_view_angle));          /* 0x2c875 sub bx,[0x909f8] (16-bit; ebx-high 0) */
                t = t - cdiv;                                        /* 0x2c87c sub ebx,eax (32-bit) */
                uint32_t ci = (t >> 5) & 0xeu;                       /* 0x2c87f/82 view-angle index */
                uint32_t aid = ((uint32_t)st >> 8) & 0xffu;          /* 0x2c885 movzx eax,ah */
                uint32_t ent = (uint32_t)G32(VA_g_ademo_das_fat_buffer + 0x4) + aid * 0x68u; /* 0x2c88a-99 id*13*8 + [0x85cf4] */
                nid = (uint32_t)*(volatile uint16_t *)(uintptr_t)(ent + ci + 0x14u);  /* 0x2c89b/a0 and ebx,0xffff */
                esi = ent;                                           /* live esi at the 0x2c8a6 jmp 0x2c924 */
            } else {                                                 /* 0x2c8a8 entity-linked frame resolve */
                G8(VA_g_secondary_subpass_id + 0x1) = (uint8_t)((uint32_t)st >> 8);          /* 0x2c8a8 mov [0x853fa],ah */
                uint32_t slotrec = bref + (uint32_t)GADDR(VA_g_state_pool_a_count + 0x3);  /* 0x2c8ae/b0 ebx = edx+0x491e03 */
                uint32_t cdiv = ((int32_t)G32(VA_g_view_clip_plane) > 0)          /* 0x2c8b9 jle (edx stays 0) */
                    ? (uint32_t)(((int32_t)((uint32_t)G32(VA_g_visible_extent_list + 0x3c) << 6)) / (int32_t)G32(VA_g_view_clip_plane)) : 0;  /* 0x2c8c2-d1 -> edx */
                /* 0x2c8d3 mov al,[esi+6] — esi is STILL srec (0x4f00d preserves it): the object's
                 * facing byte. FIDELITY NOTE: on the divide-taken path the original leaves the idiv
                 * quotient's bytes 1-3 in eax through this mov al; (t>>5)&0xe reads bits 6-8 only,
                 * and bits 0-8 of ((2*eax+0x120 [16-bit sub w]) - cdiv) depend on al/w/cdiv mod 2^9
                 * alone (add/sub carries propagate upward), so the zero-extended form is bit-exact. */
                uint32_t t = (uint32_t)*(volatile uint8_t *)(uintptr_t)(srec + 6) * 2u + 0x120u;  /* 0x2c8d3-d8 */
                t = (uint16_t)(t - (uint16_t)G16(VA_g_sprite_view_angle));          /* 0x2c8dd sub ax,[0x909f8] (16-bit) */
                t = t - cdiv;                                        /* 0x2c8e4 sub eax,edx */
                uint32_t ci = (t >> 5) & 0xeu;                       /* 0x2c8e7/ea */
                pend_84998 = slotrec;                                /* 0x2c8ed mov [0x84998],ebx — deferred with the
                                                                      *   entry store (commit sites); reader-free global */
                G32(VA_g_door_worklist + 0x8) = 0x44f0aa;                             /* 0x2c8f3 code-ptr WRITE-ONLY TOKEN:
                                                                      *   0x4f0aa is a bare RET; [0x84994] has NO reader
                                                                      *   anywhere in the image (byte-scan verified) */
                uint32_t ent = *(volatile uint32_t *)(uintptr_t)slotrec;   /* 0x2c8fd mov esi,[ebx] */
                if (ent == 0) {                                      /* 0x2c8ff/901 lazy-fill the cached entry ptr */
                    uint32_t aid = (uint32_t)(uint8_t)G8(VA_g_secondary_subpass_id + 0x1);   /* 0x2c904 movzx esi,[0x853fa] */
                    ent = (uint32_t)G32(VA_g_ademo_das_fat_buffer + 0x4) + aid * 0x68u;      /* 0x2c90d-16 id*13*8 + [0x85cf4] */
                    *(volatile uint32_t *)(uintptr_t)slotrec = ent;  /* 0x2c91c mov [ebx],esi */
                }
                nid = resolve_face_surface_id(slotrec, ent, ci);   /* 0x2c91f call 0x4f0ab (rec=ebx, base=esi,
                                                                           *   off=eax) -> ebx = frame id (u16) */
                esi = ent;                                           /* live esi at 0x2c924 */
            }
        } else {                                                     /* al == 0xfe (0x2c7ea je 0x2c93e; the dispatch's
                                                                      *   0x2c7f2 fall-through is dead — al>=0xfc here) */
            /* ---- 0x2c93e..0x2ca21: anim-table id walk (rwss `animated:` sibling
             * with the faceres-only [0x85cec] table switch + divide term + 0x110/0x2000 variant) ---- */
            uint32_t tab = (uint32_t)G32(VA_g_world_surface_draw_flags + 0x8);                   /* 0x2c93e */
            if (cur >= 0x1200u)                                      /* 0x2c944 cmp word[0x90a78],0x1200 — the cursor
                                                                      *   low16 == cur (deferred-store twin); jb keeps */
                tab = (uint32_t)G32(VA_g_map_das_dir_table_buffer + 0x4);                        /* 0x2c94f high-id table */
            uint32_t aid = ((uint32_t)st >> 8) & 0xffu;              /* 0x2c955/57 sub ebx,ebx; mov bl,ah */
            uint32_t off = (uint32_t)*(volatile uint16_t *)(uintptr_t)(tab + aid * 2u);  /* 0x2c95b/5f and ebx,0xffff */
            uint32_t rec = tab + off;                                /* 0x2c965 add esi,ebx */
            uint32_t w = (uint32_t)*(volatile uint16_t *)(uintptr_t)rec;  /* 0x2c967 mov bx,[esi] (ebx-high 0) */
            uint32_t ci;
            if (w & 0x8000u) {                                       /* 0x2c96a view-angle frame pick */
                uint32_t cdiv = ((int32_t)G32(VA_g_view_clip_plane) > 0)          /* shared divide (0x2c981-9a / 0x2c9c0-d9) */
                    ? (uint32_t)(((int32_t)((uint32_t)G32(VA_g_visible_extent_list + 0x3c) << 6)) / (int32_t)G32(VA_g_view_clip_plane)) : 0;
                uint32_t t = (uint32_t)(uint8_t)G8(VA_g_current_proc_tag + 0x11a) * 2u;    /* mov bl,[0x84ab6]; add ebx,ebx */
                if (w & 0x2000u) {                                   /* 0x2c976 jne 0x2c9bd: fine 32-step variant */
                    t += 0x110u;                                     /* 0x2c9e4 */
                    t = (uint16_t)(t - (uint16_t)G16(VA_g_sprite_view_angle));      /* 0x2c9ea sub bx,[0x909f8] */
                    t = t - cdiv;                                    /* 0x2c9f1 sub ebx,eax */
                    ci = (t >> 4) & 0x1eu;                           /* 0x2c9f4/f7 */
                } else {                                             /* 0x2c97e: coarse 16-step variant */
                    t += 0x120u;                                     /* 0x2c9a5 */
                    t = (uint16_t)(t - (uint16_t)G16(VA_g_sprite_view_angle));      /* 0x2c9ab */
                    t = t - cdiv;                                    /* 0x2c9b2 */
                    ci = (t >> 5) & 0xeu;                            /* 0x2c9b5/b8 */
                }
            } else {
                ci = (uint16_t)G16((VA_g_floor_tex_caps + 0x2) + w);                     /* 0x2c9fc mov bx,[ebx+0x909b4] (the common-path
                                                                      *   0x2cb3b table; full 16-bit, no mask) */
            }
            nid = (uint32_t)*(volatile uint16_t *)(uintptr_t)(rec + ci + 2u);  /* 0x2ca03 mov bx,[esi+ebx+2] */
            esi = rec;                                               /* live esi at 0x2ca08 */
        }
        /* shared retry tail — 0x2c924 (fc) / 0x2ca08 (fe): optional x-flip, cursor re-store, loop */
        if (nid & 0x8000u) {                                         /* test bh,bh; jns */
            G16(VA_g_world_surface_draw_flags) ^= 2;                                       /* 0x2c928/0x2ca0c xor WORD [0x9093c],2 (16-bit) */
            nid &= 0x7fffu;                                          /* and bh,0x7f (nid-high16 = 0 on every path) */
        }
        desc = nid;                                                  /* 0x2c933/0x2ca17 mov [0x90a78],ebx — full 32-bit,
                                                                      *   high16 = 0; deferred like the entry store, so
                                                                      *   the commit/loader-fail sites stay faithful */
        cur  = (uint16_t)nid;
        goto reclassify_after_load;                                  /* 0x2c939/0x2ca1d jmp 0x2c732. nid < 0x8000 here
                                                                      *   (bit15 cleared above), so the loop head's
                                                                      *   0x2c73e cur==0xffff test can never hit — the
                                                                      *   C label sitting after that test (and after B1)
                                                                      *   is exact. */
        }
#endif
    }

    uint32_t slot = (uint32_t)al;                                    /* cache-slot index (al<0xfc) */
    uint32_t ppu  = (uint32_t)G32(VA_g_das_cache_slots + slot * 6);               /* slot.alloc_ptr (read-only) */
#ifdef ROTH_STANDALONE
    if (ppu == 0) {                                                  /* 0x2c782 int3 breadcrumb falls */
        if (load_das_block_for_fat_index(desc) == 0)         /* through into the SAME 0x2c783 lazy-load- */
            goto reclassify_after_load;                             /* retry as al==0xff (loader spine) */
        if (out_eax) *out_eax = desc;
        if (out_cf)  *out_cf  = 1u;
        return esi;
    }
#else
    if (ppu == 0) return BRIDGE();                                    /* int3 path (defensive) */
#endif
    uint32_t block = *(volatile uint32_t *)(uintptr_t)ppu;           /* *ppuVar10 = block record */
#ifdef ROTH_STANDALONE
    if (*(volatile uint8_t *)(uintptr_t)(block - 8) & 4)             /* 0x2c7ff block moved (bit2) -> */
        refresh_moved_das_cache_block(block);                /* 0x2c805 refresh IN PLACE (addr unchanged), continue */
#else
    if (*(volatile uint8_t *)(uintptr_t)(block - 8) & 4) return BRIDGE();   /* block moved -> refresh 0x41250 */
#endif
    uint32_t descword = (uint32_t)st;                                /* eax at LAB_0002c7f7 (no refresh) */
#ifndef ROTH_STANDALONE
    if (descword & 0x100) return BRIDGE();                            /* span-callback 0x39093 */
    uint16_t b_a = *(volatile uint16_t *)(uintptr_t)(block + 0xa);
    if (b_a & 0x100) return BRIDGE();                                 /* animated texture */

    /* ===== committed to the native common path ===== */
    g_faceres_native++;
    G32(VA_g_door_worklist + 0xc) = 0;                                                /* 0x2c728 */
    G32(VA_g_current_das_entry_id) = desc;                                             /* 0x2c723 cursor */
    G16((VA_g_das_cache_slots + 0x4) + slot * 6) = (uint16_t)G16(VA_g_das_cache_tick);               /* 0x2c771 slot.tick = g_das_cache_tick */
    G32(VA_g_format_flags + 0x1f) = ppu;                                             /* 0x2c7f7 */
    G32(VA_g_current_proc_tag + 0x4) = block;                                           /* 0x2c80a */

    if (b_a & 0x8000) { if (out_eax) *out_eax = descword; if (out_cf) *out_cf = 0; return block; }  /* 0x2cbab clc;ret */

    G8(VA_g_span_textured_mode_flag)  = (uint8_t)((b_a >> 8) & 4);                       /* 0x2ca37 */
    G16(VA_g_secondary_surface_count + 0x2) = b_a;                                             /* 0x2ca3d */

    if (!(b_a & 0x40)) {                                            /* 0x2ca4d je 0x2cb66 (simple) */
        G32(VA_g_render_source_base_ptr) = block + 0x10;                               /* 0x2cb66 */
        uint32_t e4 = *(volatile uint32_t *)(uintptr_t)(block + 4);/* 0x2cb6e Ltail */
        G32(VA_g_current_proc_tag + 0x11c) = e4;
        int32_t ev = (int32_t)(int16_t)(uint16_t)e4;               /* cwde */
        ev <<= 8;                                                  /* shl eax,8 */
        if (G16(VA_g_world_surface_draw_flags) & 2) ev = -ev;                            /* neg */
        G32(VA_g_visible_extent_list + 0x3c) += (uint32_t)ev;
        G32(VA_g_current_proc_tag + 0x4) -= block;
        if (out_eax) *out_eax = (uint32_t)ev;
        if (out_cf) *out_cf = 0;
        return block;
    }

    /* 0x2ca55: per-wall angle/coord resolve */
    uint16_t cw = *(volatile uint16_t *)(uintptr_t)(block + 0x10);
    uint32_t ci;                                                   /* coord index (0x2cb42 input) */
    if (!(cw & 0x8000)) {                                          /* 0x2cb3b */
        ci = (uint16_t)G16((VA_g_floor_tex_caps + 0x2) + (uint32_t)cw);
    } else {
        uint32_t cdiv = ((int32_t)G32(VA_g_view_clip_plane) > 0)
            ? (uint32_t)(((int32_t)((uint32_t)G32(VA_g_visible_extent_list + 0x3c) << 6)) / (int32_t)G32(VA_g_view_clip_plane)) : 0;
        uint32_t t = (uint32_t)(uint8_t)G8(VA_g_current_proc_tag + 0x11a);
        if (cw & 0x2000) {                                         /* 0x2cab1: >>4 & 0x1e, +0x110 */
            t = t * 2 + 0x110;
            t = (uint16_t)(t - (uint16_t)G16(VA_g_sprite_view_angle));
            t = t - cdiv;
            ci = (t >> 4) & 0x1e;
        } else {                                                  /* 0x2ca6f: >>5 & 0xe, +0x120 */
            t = t * 2 + 0x120;
            t = (uint16_t)(t - (uint16_t)G16(VA_g_sprite_view_angle));
            t = t - cdiv;
            ci = (t >> 5) & 0xe;
        }
    }
    /* 0x2cb42 */
    uint16_t bw = *(volatile uint16_t *)(uintptr_t)(block + ci + 0x12);
    uint32_t idx = bw;
    if (bw & 0x8000) { G16(VA_g_world_surface_draw_flags) ^= 2; idx = bw & 0x7fff; }     /* test bh,bh; jns */
    idx <<= 4;
    block += idx;                                                 /* esi advanced */
    /* 0x2cb95 Ltail2 */
    G32(VA_g_current_proc_tag + 0x11c) = *(volatile uint32_t *)(uintptr_t)(block + 4);
    G32(VA_g_render_source_base_ptr) = block + 0x10;
    G32(VA_g_current_proc_tag + 0x4) -= block;
    if (out_eax) *out_eax = block + 0x10;
    if (out_cf) *out_cf = 0;
    return block;
#else
    /* ===== faithful native flow from 0x2c7f7: commit stores, then the shared 0x2c810 join helper
     * (faceres_join_0x2c810 above — callback + 0x2ca22 join + all tails; also entered by the
     * cur==0xffff B1 default-record path). Disasm-ordered: the original completes ALL commit stores
     * by 0x2c80a BEFORE the callback/anim forks. */
    g_faceres_native++;
    G32(VA_g_door_worklist + 0xc) = pend_84998;                                       /* 0x2c728 entry store / 0x2c8ed 0xfc-walk value */
    G32(VA_g_current_das_entry_id) = desc;                                             /* 0x2c723 cursor (post-remap/post-walk: nid) */
    G16((VA_g_das_cache_slots + 0x4) + slot * 6) = (uint16_t)G16(VA_g_das_cache_tick);               /* 0x2c771 slot.tick = g_das_cache_tick */
    G32(VA_g_format_flags + 0x1f) = ppu;                                             /* 0x2c7f7 */
    G32(VA_g_current_proc_tag + 0x4) = block;                                           /* 0x2c80a */
    return faceres_join_0x2c810(descword, block, out_eax, out_cf);
#endif

    #undef BRIDGE
}

/* ---- factored epilogue blocks: shared verbatim by the linear path and the rotated tail —
 * the original's two store-gate/wall-call regions are instruction-for-instruction identical
 * (0x2bf0e..0x2bf62 vs 0x2c1f6..0x2c24e), so the already-verified linear C is single-sourced here. ---- */
static void rwss_store_gate(void)
{   /* store-record gate + store_secondary_surface_record (0x2bf0e / 0x2c1f6) */
    int16_t ax = (int16_t)G16(VA_g_current_decoded_frame + 0x1e);
    int do_store = !(ax < (int16_t)G16(VA_g_wall_proj_y3 + 0x14) || ax > (int16_t)G16(VA_g_wall_proj_y3 + 0x16));   /* jl/jg skip (signed) */
    int32_t gate_e = 0;
    if (do_store) {                                                 /* center-Y magnitude */
        int16_t cyc = (int16_t)((uint16_t)G16(VA_g_wall_proj_y3 + 0xa) + (uint16_t)G16(VA_g_wall_proj_y3 + 0x12));   /* mov ax; add ax */
        cyc = (int16_t)(cyc >> 1);                                  /* sar ax,1 */
        cyc = (int16_t)(cyc - (int16_t)G16(VA_g_current_decoded_frame + 0x1c));               /* sub ax,[0x84960] */
        uint16_t mag = (cyc < 0) ? (uint16_t)(-cyc) : (uint16_t)cyc;   /* jns/neg ax */
        gate_e = (int32_t)((int32_t)mag * G32(VA_g_view_clip_plane));            /* imul eax,[0x85264] (32-bit two-op) */
        if ((uint32_t)gate_e > (uint32_t)G32(VA_g_current_decoded_frame + 0x20)) do_store = 0;   /* ja (unsigned) */
    }
    if (do_store && !g_rwss_dbg_skiprender)
        (void)store_secondary_surface_record((uint32_t)gate_e);             /* NATIVE */
}

static void rwss_wall_call(uint32_t esi_in, uint16_t es, uint16_t fs, uint16_t gs)
{   /* guarded wall-driver call (0x2bf58 / 0x2c240: push es/fs; call 0x36b39; pop fs/es).
     * SELECTOR GUARD: the wall driver internally does `mov fs,[0x909b0]`; an invalid selector means the
     * projection diverged — count it (g_rwss_badsel) + skip so the differential REPORTS, not crashes. */
    if (g_rwss_dbg_skiprender) return;                                             /* DEBUG: skip the render */
    uint16_t fssel = (uint16_t)G16(VA_g_world_alt_render_flags + 0x2);
    if (g_os_sel_base == NULL) {                                                 /* no host sel hook -> bridge */
        rwss_call(0x36b39, 0, (uint32_t)(uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x4), esi_in, es, fs, gs);
    } else if (g_os_sel_base(fssel) == 0) {
        g_rwss_badsel++;                                                           /* invalid fs sel -> skip */
    } else {
        /* NATIVE draw_world_surface_spans (0x36b39). ecx = (u16)[0x90980]; selector bases per the
         * wall driver's ABI (gs=shade colormap 0x8a2a8, es=64K blend LUT 0x90be2 which the mappers
         * self-reload, fs=texture 0x909b0). Exercised + verified by the secsurf diff. */
        uint32_t gs_base = g_os_sel_base((uint16_t)G16(VA_g_active_world_remap_selector));
        uint32_t es_base = g_os_sel_base((uint16_t)G16(VA_g_transparency_blend_selector));
        uint32_t fs_base = g_os_sel_base(fssel);
        draw_world_surface_spans((uint32_t)(uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x4), gs_base, es_base, fs_base);
    }
}

/* rwss ROTATED-projection tail (0x2bfa4..0x2c24e) — the
 * oblique-in-plane surface: sincos of (2*([0x84ab6]+0x40) - [0x909f8]) & 0x1ff scaled by the half-extent
 * gives PER-EDGE depths ([0x85264] +/- cos_s) and laterals ([0x85260] +/- sin_s) — a perspective trapezoid
 * with 4 distinct corner Ys, x-flip via [0x9093c]^=2 on edge swap, then the SAME store-gate + wall-driver
 * epilogue as the linear path (disasm-identical, factored above). Gotchas honored (disasm-checked):
 * the sin/cos*ext imuls are 32-bit TRUNCATING two-op (0x2bfd5/0x2bfd8: explicit u32 wrap, then SAR 8);
 * the stale bh at `mov bl,[0x84ab6]` provably cancels mod 512 (angle computed cleanly; sincos masks &0x1ff);
 * depth words [0x90966]/[0x9095e] take the UNCLAMPED divisor via LOGICAL shr 8; the early-out (0x2c075)
 * fires only when divB_raw<=0x1000, compares signed (s16)[0x90966]<0x10 PRE-swap, and returns MID-FLOW
 * ([0x90958]/[0x90940]/Y-quad/[0x90a26] left unwritten, no store/draw); the rotated path OMITS the
 * linear-only [0x84aba] term, the [0x90968] save/restore, the [0x90970]=[0x90a28] refresh and the
 * [0x85314]/[0x85316] X-extent rejects — real asymmetries, disasm-confirmed, do NOT "fix". rwss_proj
 * reproduces the original's per-v 64-bit product-reuse exactly (deterministic product, two idivs).
 * esi_in/es/fs/gs threaded only for the wall-call bridge fallback. */
static void rwss_rotated_tail(uint8_t cl, uint32_t esi_in, uint16_t es, uint16_t fs, uint16_t gs)
{
    uint32_t ext = (uint32_t)(uint16_t)G16(VA_g_wall_proj_y3 + 0x8) << ((uint8_t)(cl - 6) & 0x1f);  /* 0x2bfa4 shl eax,cl-6 */
    uint16_t sin16, cos16; uint32_t tbl;
    uint32_t angle = (2u * ((uint32_t)(uint8_t)G8(VA_g_current_proc_tag + 0x11a) + 0x40u)
                      - (uint32_t)(uint16_t)G16(VA_g_sprite_view_angle)) & 0x1ffu;                 /* 0x2bfb7..0x2bfc2 */
    sincos_pair(angle, &sin16, &cos16, &tbl);                                /* 0x2bfc9 (sin=CX cos=BX) */
    int32_t cos_s = (int32_t)((uint32_t)(int32_t)(int16_t)cos16 * ext) >> 8;        /* imul ebx,eax; sar ebx,8 */
    int32_t sin_s = (int32_t)((uint32_t)(int32_t)(int16_t)sin16 * ext) >> 8;        /* imul ecx,eax; sar ecx,8 */

    /* edge A (+cos): 0x2bfe9..0x2c039 */
    int32_t divA = G32(VA_g_view_clip_plane) + cos_s;
    G16(VA_g_wall_proj_y3 + 0x18) = (uint16_t)((uint32_t)divA >> 8);                                 /* shr (UNCLAMPED depth) */
    if (divA <= 0x1000) divA = 0x1000;                                              /* cmp/jg; clamp */
    G16(VA_g_wall_proj_y3 + 0x12) = (uint16_t)rwss_proj(G32(VA_g_visible_extent_list + 0x3c) + sin_s, G32(VA_g_view_params_block + 0xc), divA, G32(VA_g_span_src_wrap_reoffset + 0x24));

    /* edge B (-cos): 0x2c03f..0x2c0a6, with the mid-flow early-out (0x2c060..0x2c075) */
    int32_t divB = G32(VA_g_view_clip_plane) - cos_s;
    G16(VA_g_wall_proj_y3 + 0x10) = (uint16_t)((uint32_t)divB >> 8);                                 /* shr (UNCLAMPED depth) */
    if (divB <= 0x1000) {                                                           /* jg skips clamp AND test */
        divB = 0x1000;
        if ((int16_t)(uint16_t)G16(VA_g_wall_proj_y3 + 0x18) < 0x10) { g_rwss_rot_cull++; return; }  /* jl 0x2c24b mid-flow ret */
    }
    G16(VA_g_wall_proj_y3 + 0xa) = (uint16_t)rwss_proj(G32(VA_g_visible_extent_list + 0x3c) - sin_s, G32(VA_g_view_params_block + 0xc), divB, G32(VA_g_span_src_wrap_reoffset + 0x24));
    G16(VA_g_world_surface_draw_flags + 0x4) = 0;                                                               /* 0x2c0ac */

    /* swap to enforce left<=right (0x2c0b5..0x2c0f8): strict >, signed words; x-flip bit for the drivers */
    if ((int16_t)(uint16_t)G16(VA_g_wall_proj_y3 + 0xa) > (int16_t)(uint16_t)G16(VA_g_wall_proj_y3 + 0x12)) {
        uint16_t t;
        t = (uint16_t)G16(VA_g_wall_proj_y3 + 0x12); G16(VA_g_wall_proj_y3 + 0x12) = G16(VA_g_wall_proj_y3 + 0xa); G16(VA_g_wall_proj_y3 + 0xa) = t;
        t = (uint16_t)G16(VA_g_wall_proj_y3 + 0x18); G16(VA_g_wall_proj_y3 + 0x18) = G16(VA_g_wall_proj_y3 + 0x10); G16(VA_g_wall_proj_y3 + 0x10) = t;
        int32_t td = divA; divA = divB; divB = td;                                  /* [ebp+8] <-> [ebp+0xc] */
        G16(VA_g_world_surface_draw_flags) = (uint16_t)(G16(VA_g_world_surface_draw_flags) ^ 2);                                /* x-flip for the drivers */
        g_rwss_rot_flip++;
    }

    /* vertical base (0x2c100..0x2c12d) — NO [0x84aba] term (linear-only; 0x2c11b/0x2c126 vs 0x2be48/0x2be5d) */
    int32_t ecx  = (int32_t)(uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x4);
    int32_t topv = (int32_t)(int16_t)G16(VA_g_anim_clock + 0xa);
    uint8_t dl   = (uint8_t)G8(VA_g_secondary_surface_count + 0x2);
    if (dl & 0x10) topv = topv - 2 * (int32_t)(dl & 0xf) + ecx;                     /* sub eax,edx; add eax,ecx */
    else           topv = topv + 2 * (int32_t)(dl & 0xf);
    int32_t bottomv = topv - ecx;                                                   /* 0x2c12e */
    G16(VA_g_span_src_wrap_reoffset + 0xa) = (uint16_t)bottomv;

    /* 4 corner Ys (0x2c136..0x2c1e9): per-edge divisors; store order = original */
    int32_t mul2 = G32(VA_g_perspective_scale), cy0 = G32(VA_g_span_src_wrap_reoffset + 0x28);
    G16(VA_g_wall_proj_y3 + 0xc) = (uint16_t)rwss_proj(bottomv, mul2, divB, cy0);                   /* idiv [ebp+8]  */
    G16(VA_g_wall_proj_y3 + 0x14) = (uint16_t)rwss_proj(bottomv, mul2, divA, cy0);                   /* idiv [ebp+0xc] */
    G16(VA_g_wall_proj_y3 + 0x16) = (uint16_t)rwss_proj(topv,    mul2, divA, cy0);                   /* idiv [ebp+0xc] */
    G16(VA_g_wall_proj_y3 + 0xe) = (uint16_t)rwss_proj(topv,    mul2, divB, cy0);                   /* idiv [ebp+8]  */

    G8(VA_g_sprite_fill_index + 0x2) = 0;                                                                /* 0x2c1ef */
    rwss_store_gate();                                                              /* 0x2c1f6 (identical block) */
    rwss_wall_call(esi_in, es, fs, gs);                                             /* 0x2c240 */
}                                                                                   /* add esp,0x20; ret */

#ifdef ROTH_STANDALONE
/* ===== rwss TYPE-1 handler (0x2b716..0x2bc33, 1310 B) — the image-free render stop =====
 * (IMGFREE-ONLY so the trap lane stays byte-identical — type 1 has no
 * static-scene differential, the bar is faithfulness-to-disasm.) A PRE-PROJECTED portal/window span:
 * NO projection math on the main path — narrows the global x-window from the packed [sr+8] pair,
 * stores the already-projected screen quad from the L2 record (+0x16..+0x25) as dword word-pairs,
 * resolves the texture id through the DAS cache (remap-hook / lazy-load / LRU-tick retry loop, the
 * same shape as rasterize_world_spans_scanline's resolve/indirect/classify), then runs the
 * store-gate + wall-driver epilogue shared verbatim with the linear/rotated paths — with sky/subpass
 * variants that temporarily clamp the y-window pair via the standard rwss_proj +/-0x3ffe idiom on the
 * STALE [0x85264] divisor (intentionally the previous surface's depth base; do not
 * seed). Never rejoins 0x2bcd1; exits = the silent ret 0x2b7a5 (x-window untouched) + the three
 * restoring epilogues (0x2babd / 0x2bb62 / 0x2bc24-0x2bc33). No SMC; no selector derefs (fs sel
 * [0x909b0] is written as plain data; the driver loads it itself).
 * sr = [entry_esi+4] (the dispatch 0x2bc5f deref, done by the caller); es/fs/gs = entry selectors,
 * threaded only to rwss_wall_call. */
volatile unsigned long g_rwss_t1 = 0;                             /* type-1 coverage: green claims need >0 */
volatile unsigned long g_rwss_t1_sky = 0, g_rwss_t1_sub = 0, g_rwss_t1_hook = 0,
                       g_rwss_t1_anim = 0, g_rwss_t1_remap = 0;   /* sub-path coverage */
static void rwss_type1_handler(uint32_t sr, uint16_t es, uint16_t fs, uint16_t gs)
{
    /* ---- prologue (0x2b716..0x2b76a): the early-commit set — precedes every possible silent ret:
     * [0x90a44], [0x85334], [0x90970], [0x9093c], [0x90a78]. ---- */
    uint32_t l2  = *(volatile uint32_t *)(uintptr_t)(sr + 4);           /* 0x2b716 ebx=[esi+4] = L2 wall-face record */
    uint32_t geo = *(volatile uint32_t *)(uintptr_t)(l2 + 8);           /* 0x2b719 eax=[ebx+8] = geometry record */
    G32(VA_g_map_das_fat_buffer + 0xc) = (int32_t)geo;                                        /* 0x2b71c g_current_render_record */
    { int16_t t = (int16_t)((uint16_t)*(volatile uint16_t *)(uintptr_t)(geo + 0xa)
                            - (uint16_t)G16(VA_g_sector_cull_coord));                  /* 0x2b721/0x2b725 */
      G16(VA_g_anim_clock + 0xa) = (uint16_t)(-t); }                                  /* 0x2b72c neg edx on a stale-upper edx —
                                                                         * only dx is stored: mod-2^16 exact */
    { uint8_t al = *(volatile uint8_t *)(uintptr_t)(geo + 8);           /* 0x2b735 shade (== main 0x2bc70) */
      if (al != 0 && G8(VA_g_span_clip_source) != 0) al = (uint8_t)((uint8_t)(al - 0x80) + (uint8_t)G8(VA_g_span_clip_source));
      G8(VA_g_column_clip_mode) = al; }                                               /* 0x2b74d */
    G16(VA_g_world_surface_draw_flags) = (uint16_t)(*(volatile uint16_t *)(uintptr_t)(l2 + 0x14) | 0x19);  /* 0x2b752 mode word FROM
                                                                         * THE RECORD (main path: = 0x19) */
    G32(VA_g_current_das_entry_id) = (int32_t)(uint32_t)*(volatile uint16_t *)(uintptr_t)(l2 + 0x10);  /* 0x2b760 id cursor
                                                                         * (bx load + and ebx,0xffff = zx16) */

    /* ---- texture resolve retry loop (0x2b770..0x2b7fd). Two re-entry points, mirroring
     * emit_world_face_spans / the rasterizer: the loader retry re-enters the FULL
     * loop (0x2b770, re-tests ah&2); the remap path re-enters at the al<0xfc check (0x2b791) WITHOUT
     * re-testing ah&2 (here: plain fall-through past the remap `if`). ---- */
    uint32_t hcell;                     /* edx: the DAS cache handle cell (-> block ptr), or the default record */
    uint32_t status, idx2;
retry:                                                                  /* 0x2b770 */
    { uint32_t id = (uint32_t)(uint16_t)G16(VA_g_current_das_entry_id);                   /* sub ebx,ebx; mov bx,[0x90a78] (WORD re-read
                                                                         * even after the full-32-bit remap store) */
      if (id == 0xffffu) {                                              /* 0x2b779 cmp ebx,0xffff; je 0x2b7a6 */
          /* 0x2b7a6: edx = 0x29e58 — the STATIC obj1 default-record pointer-cell ([0x29e58] -> 0x29e5c+delta,
           * its 0x0404 block flags take the static source path incl. the -[l2+0xc] bias).
           * Arena-staged now: the record bytes ride gen_obj1data_c.py; the fixup-carrying
           * ptr cell is re-expressed symbolically by roth_boot. Assert the staging, then join. */
          hcell = 0x29e58u + OBJ_DELTA;                                 /* 0x2b7a6 mov edx,0x429e58 */
          if (*(volatile uint32_t *)(uintptr_t)hcell == 0)
              roth_unreachable(0x2b7a6u);                              /* staging regressed — fail loud */
          goto have_src;                                                /* 0x2b7ab jmp 0x2b7ff */
      }
      idx2 = id + id; }                                                 /* 0x2b781 add ebx,ebx */
    status = (uint32_t)(uint16_t)G16(VA_g_das_entry_status_table + idx2);                   /* 0x2b785 g_das_entry_status_table */
    if (status & 0x200u) {                                              /* 0x2b78c test ah,2 -> remap (0x2b7ad) */
        uint32_t hook = (uint32_t)G32(VA_g_pool_check_enabled + 0x28);                         /* 0x2b7af */
        if (hook != 0) {                                                /* 0x2b7b5/0x2b7b7 */
            /* 0x2b7b9 shr eax,1 (id); 0x2b7be call ebx = [0x8a2a0] -> 0x33dde texture_id_remap_hook, NOW
             * NATIVE via the two-value dispatch shim (lift_raw_commands.c; fail-loud on unknown values).
             * The push es/ds; pop es swap around the call is moot in C (no segment regs). */
            g_rwss_t1_remap++;
            uint32_t nid = rwss_id_remap_dispatch(idx2 >> 1);
            idx2 = nid + nid;                                           /* 0x2b7c1/0x2b7c3 mov ebx,eax; add ebx,ebx */
            G32(VA_g_current_das_entry_id) = (int32_t)nid;                                /* 0x2b7c5 FULL 32-bit cursor update
                                                                         * BEFORE the status re-read */
        } else {
            /* 0x2b7d3 null-hook RE-DOUBLE QUIRK: ebx=eax(=id*2); add ebx,ebx -> reads [id*4+0x86d30];
             * the cursor [0x90a78] is NOT updated. Same original quirk as faceres 0x2c7b6 — dead in
             * practice (the hook is installed at map load, never cleared); kept faithful. */
            idx2 = idx2 + idx2;
        }
        status = (uint32_t)(uint16_t)G16(VA_g_das_entry_status_table + idx2);               /* 0x2b7ca / 0x2b7d7 */
        /* jmp 0x2b791: falls through — NO ah&2 re-test */
    }
    /* recheck (0x2b791) */
    { uint8_t al = (uint8_t)status;
      if (al >= 0xfc) {                                                 /* 0x2b791 cmp al,0xfc; jb slot */
          if (al != 0xff) return;                                       /* 0x2b795/0x2b797: 0xfc/0xfd/0xfe = SILENT
                                                                         * give-up (DIFFERENT from faceres;
                                                                         * x-window untouched, ret 0x2b7a5) */
          /* al==0xff: lazy DAS load with the FULL [0x90a78] dword (0x2b799); 0x40d7c = the NATIVE
           * loader spine. CF clear (0) = loaded -> the FULL retry loop; CF set = silent ret. */
          if (load_das_block_for_fat_index((uint32_t)G32(VA_g_current_das_entry_id)) == 0)
              goto retry;                                               /* 0x2b7a3 jae 0x2b770 */
          return;                                                       /* 0x2b7a5 */
      }
      /* slot (0x2b7e0) */
      { uint32_t slot6 = (uint32_t)al * 6u;                             /* movzx ebx,al; lea ebx,[ebx+ebx*2]; add */
        G16((VA_g_das_cache_slots + 0x4) + slot6) = (uint16_t)G16(VA_g_das_cache_tick);                  /* 0x2b7e8 LRU tick touch — COMMITTED even
                                                                         * on the empty-slot ret */
        hcell = (uint32_t)G32(VA_g_das_cache_slots + slot6);                         /* 0x2b7f5 handle slot (Pool: deref via slot) */
        if (hcell == 0) return; } }                                     /* 0x2b7fd je 0x2b7a5 silent */

    /* ---- have_src (0x2b7ff): portal x-window narrowing + the pre-projected extent stores ----
     * The x-window save happens only HERE — the silent rets above never touch [0x90968]. */
 have_src: ;                                                            /* 0x2b7ff (join: slot path falls through,
                                                                         * the id==0xffff default record jmps here) */
    int32_t saved_x = G32(VA_g_view_bound_right);                                     /* push dword [0x90968]: the {0x90968,0x9096a}
                                                                         * pair saved/restored as a DWORD */
    {   /* 0x2b805 re-loads ebx=[esi+4] (the id-cursor load clobbered ebx) = l2, already held */
        uint32_t xw = *(volatile uint32_t *)(uintptr_t)(sr + 8);        /* 0x2b808 eax=[esi+8]: packed portal x-window */
        if ((int16_t)(uint16_t)xw < (int16_t)G16(VA_g_view_bound_right))              /* 0x2b80b jge skip (signed) */
            G16(VA_g_view_bound_right) = (uint16_t)xw;                                /* 0x2b814 narrow right bound (min) */
        uint16_t xhi = (uint16_t)(xw >> 16);                            /* 0x2b81a shr eax,0x10 */
        if ((int16_t)xhi > (int16_t)G16(VA_g_view_bound_left))                       /* 0x2b81d jle skip (signed) */
            G16(VA_g_view_bound_left) = xhi;                                         /* 0x2b826 narrow left bound (max) */
        if ((int16_t)xhi >= (int16_t)G16(VA_g_view_bound_right)) goto epi;            /* 0x2b82c empty window -> restore+ret */

        uint16_t cxv    = (uint16_t)G16(VA_g_reflection_view_list + 0x84);                       /* 0x2b839 view-pass Y shift (lo16) */
        uint32_t yshift = (uint32_t)cxv << 16;                          /* 0x2b840/0x2b842 edi = ecx<<16 (entry-ecx
                                                                         * high bits shift out — cx-only, exact) */
        uint32_t e = *(volatile uint32_t *)(uintptr_t)(l2 + 0x16);      /* 0x2b845 {x=+0x16, yBot=+0x18} */
        if ((int16_t)(uint16_t)e >= (int16_t)G16(VA_g_view_bound_right)) goto epi;    /* 0x2b848 jge (signed, vs the NARROWED bound) */
        G32(VA_g_wall_proj_y3 + 0xa) = (int32_t)(e - yshift);                           /* 0x2b855 sub eax,edi: hi16 -= cx, x untouched
                                                                         * (lo16 of edi is 0 -> no borrow) */
        e = *(volatile uint32_t *)(uintptr_t)(l2 + 0x1e);               /* 0x2b85c {x=+0x1e, yBot=+0x20} */
        if ((int16_t)(uint16_t)e <= (int16_t)G16(VA_g_view_bound_left)) goto epi;    /* 0x2b85f jle (signed) */
        G32(VA_g_wall_proj_y3 + 0x12) = (int32_t)(e - yshift);                           /* 0x2b86c/0x2b86e */
        e = *(volatile uint32_t *)(uintptr_t)(l2 + 0x1a);               /* 0x2b873 {yTopL=+0x1a, depth=+0x1c} */
        e = (e & 0xffff0000u) | (uint16_t)((uint16_t)e - cxv);          /* 0x2b876 sub ax,cx (LOW word only) */
        G32(VA_g_wall_proj_y3 + 0xe) = (int32_t)e;                                      /* 0x2b879 {yTopL-cx, depthL=[l2+0x1c]} */
        e = (e & 0xffff0000u)
          | (uint16_t)((uint16_t)*(volatile uint16_t *)(uintptr_t)(l2 + 0x22) - cxv);
                                                                        /* 0x2b87e mov ax,[ebx+0x22] replaces ONLY
                                                                         * eax.lo16 — eax.hi16 STILL [l2+0x1c] from
                                                                         * the 0x2b873 dword load: depthR = [l2+0x1c]
                                                                         * too, NOT +0x24 (do not "fix") */
        G32(VA_g_wall_proj_y3 + 0x16) = (int32_t)e;                                      /* 0x2b885 {yTopR-cx, depthR} */
        G16(VA_g_span_src_wrap_reoffset + 0xa) = (uint16_t)*(volatile uint16_t *)(uintptr_t)(l2 + 0x24);   /* 0x2b88a bottomv word */
    }

    /* ---- texture-source fork (0x2b894): esi = [edx] = the block record ---- */
    uint32_t blk = *(volatile uint32_t *)(uintptr_t)hcell;              /* 0x2b894 */
    G8(VA_g_span_textured_mode_flag) = (uint8_t)(*(volatile uint8_t *)(uintptr_t)(blk + 0xb) & 4);
                                                                        /* 0x2b896..0x2b89c g_span_textured_mode_flag —
                                                                         * committed BEFORE the &0x20 test: even
                                                                         * no-draw records mutate it */
    if (*(volatile uint8_t *)(uintptr_t)(blk + 0xa) & 0x20) {           /* 0x2b8a2 byte test, bit 5 -> hookblk */
        /* hookblk (0x2b8f1..0x2b976): &0x20 surfaces DRAW NOTHING — no store gate, no driver, no
         * globals fill; both arms end at the epilogue. [0x849a4]'s whole-image value set is
         * {0, 0x1792b+delta} (sole installer 0x17a54 -> lift_game_core.c storing GADDR(0x1792b) via the
         * lifted xchg setter 0x283a0) and 0x1792b is a BARE RET — with fn==0 the original jumps
         * straight to epi (0x2b8f8); with the token it builds a 13-dword arg block on the STACK (no
         * global writes; the ebp clobber + es=ds swap are frame-local), calls the ret, and
         * unwinds. Net observable effect of BOTH values = nothing. Two-value check, fail-loud on any
         * unknown value. */
        g_rwss_t1_hook++;
        { uint32_t fn = (uint32_t)G32(VA_g_current_proc_tag + 0x8);
          if (fn != 0 && fn != 0x1792bu + OBJ_DELTA)
              roth_unreachable(0x2b96du); }
        goto epi;                                                       /* 0x2b8f8 / 0x2b976 jmp 0x2babd */
    }
    if (*(volatile uint16_t *)(uintptr_t)(blk + 0xa) & 0x100) {         /* 0x2b8a8 WORD test -> animated */
        if (*(volatile uint16_t *)(uintptr_t)(blk + 0x16) == 0xfffeu) { /* 0x2b8b4 cmp word[esi+0x16],-2 */
            /* 0x2b8da/0x2b8df: eax=[0x90a44]; call 0x2b5ea select_surface_anim_frame (NATIVE, returns the
             * frame ptr in ESI; its own tail does the [esi-8]&4 refresh) — the join reads the header
             * fields from the RETURNED frame record. */
            g_rwss_t1_anim++;
            blk = select_surface_anim_frame((uint32_t)G32(VA_g_map_das_fat_buffer + 0xc), blk);
            G32(VA_g_render_source_base_ptr) = (int32_t)(blk + 0x10);                       /* 0x2b8e4/0x2b8e7 */
        } else {
            if (*(volatile uint8_t *)(uintptr_t)(blk - 8) & 4)          /* 0x2b8bb */
                refresh_moved_das_cache_block(blk);              /* 0x2b8c1 (in-place, reg-transparent) */
            G32(VA_g_render_source_base_ptr) = (int32_t)(blk
                + (uint32_t)*(volatile uint16_t *)(uintptr_t)(blk + 0x14) + 0x10u);
                                                                        /* 0x2b8c6..0x2b8d0 src = esi+zx16[esi+0x14]+0x10;
                                                                         * esi itself UNCHANGED for the join */
        }
    } else {                                                            /* static (0x2b97b) */
        if (*(volatile uint8_t *)(uintptr_t)(blk - 8) & 4)              /* 0x2b97b */
            refresh_moved_das_cache_block(blk);                  /* 0x2b981 */
        blk -= (uint32_t)*(volatile uint32_t *)(uintptr_t)(l2 + 0xc);   /* 0x2b986 sub esi,[ebx+0xc]: the source BIAS
                                                                         * offsets the pointer ITSELF — the join reads
                                                                         * +8/+0xa/+0xc/+0xe from the BIASED ptr
                                                                         * (do not "sanity-fix" to block) */
        G32(VA_g_render_source_base_ptr) = (int32_t)(blk + 0x10);                           /* 0x2b989/0x2b98c */
    }

    /* ---- join (0x2b991): globals fill from the resolved source record — the same instructions as the
     * main-path fill (renderer.c main path 0x2bce8..0x2bd49) on a different esi ---- */
    { uint32_t g2 = *(volatile uint32_t *)(uintptr_t)(l2 + 8);          /* 0x2b991 re-read of [ebx+8] (== geo) */
      if (*(volatile uint8_t *)(uintptr_t)(g2 + 7) & 0x10)              /* 0x2b994 x-flip (main path: 0x2bcc3) */
          G16(VA_g_world_surface_draw_flags) = (uint16_t)(G16(VA_g_world_surface_draw_flags) ^ 2); }                /* 0x2b99a */
    G16(VA_g_world_alt_render_flags + 0x2) = (uint16_t)*(volatile uint16_t *)(uintptr_t)(blk + 8);   /* 0x2b9a2 fs texture sel (as DATA) */
    { uint16_t h = *(volatile uint16_t *)(uintptr_t)(blk + 0xe);        /* 0x2b9ae height */
      G16(VA_g_span_src_wrap_reoffset + 0xc) = h; G16(VA_g_span_src_wrap_reoffset + 0xe) = h;
      uint16_t h2 = (uint16_t)(h + h);
      G16(VA_g_wall_proj_y3 + 0x8) = h2; G16(VA_g_span_src_wrap_reoffset + 0x8) = h2; G16(VA_g_span_src_wrap_reoffset + 0x12) = h2; }
    G16(VA_g_column_clip_mode + 0x4) = 0xffff;                                              /* 0x2b9d2 */
    { uint16_t wd = *(volatile uint16_t *)(uintptr_t)(blk + 0xc);       /* 0x2b9db width */
      G16(VA_g_span_src_row_width) = wd; G16(VA_g_span_src_wrap_reoffset) = (uint16_t)(wd - 1);             /* dec/inc eax */
      uint16_t w2 = (uint16_t)(wd + wd);
      G16(VA_g_span_fill_mode_word + 0xe) = w2; G16(VA_g_wall_proj_y3 + 0x4) = w2; G16(VA_g_span_src_wrap_reoffset + 0x4) = w2; }        /* (mov cl,7 dead in type 1: no
                                                                         * shl-extent consumer here) */
    { uint16_t dxw = *(volatile uint16_t *)(uintptr_t)(blk + 0xa);      /* 0x2ba03 bit-decode == main 0x2bd49 */
      if (dxw & 0x80) {                                                 /* test dl,0x80 */
          if (dxw & 0x6000) {                                           /* test dx,0x6000 */
              uint16_t rdx = (uint16_t)((dxw << 3) | (dxw >> 13));      /* rol dx,3 */
              G16(VA_g_span_src_wrap_reoffset + 0x4) = (uint16_t)(G16(VA_g_span_src_wrap_reoffset + 0x4) << (rdx & 3));     /* and dl,3; xchg; shl [0x90980],cl */
          } else {
              G16(VA_g_span_src_wrap_reoffset + 0x4) = (uint16_t)((uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x4) >> 1);   /* 0x2ba29 shr [0x90980],1 */
          }
      }
    }
    G16(VA_g_world_surface_draw_flags + 0x4) = 0;                                                   /* 0x2ba30 */
    G8(VA_g_sprite_fill_index + 0x2) = 0;                                                    /* 0x2ba39 */

    /* ---- store gate (0x2ba40..0x2ba85): instruction-identical to 0x2bf0e/0x2c1f6 ---- */
    rwss_store_gate();

    /* ---- wall-driver gating (0x2ba8a..): push es/fs collapse into rwss_wall_call's threaded params.
     * The y-window narrowing below is IN MEMORY before each driver call — the wall body's first
     * compares read [0x9096c]/[0x9096e] internally. Driver ecx: rwss_wall_call passes
     * (u16)[0x90980] with hi16=0 — the body consumes only ecx-hi16, = 0 in the lifted world. */
    { uint32_t rec = (uint32_t)G32(VA_g_map_das_fat_buffer + 0xc);                            /* 0x2ba8d */
      if (*(volatile uint8_t *)(uintptr_t)(rec + 7) & 0x40) goto sky;   /* 0x2ba92 parallax-sky face flag */
      if (G8(VA_g_secondary_subpass_id) != 0) goto subpass;                               /* 0x2ba98 g_secondary_subpass_id live */
      if ((int16_t)G16(VA_g_anim_clock + 0xa) > (int16_t)G16(VA_g_world_span_bottom)) goto sky;      /* 0x2baa5 jg: below the world span bottom */
      rwss_wall_call(blk, es, fs, gs); }                                /* 0x2bab5 call 0x36b39; pop fs/es */
epi:                                                                    /* 0x2babd */
    G32(VA_g_view_bound_right) = saved_x;                                             /* pop eax; mov [0x90968],eax */
    return;                                                             /* 0x2bac3 ret */

sky:                                                                    /* 0x2bac4: y-window clamped variant */
    g_rwss_t1_sky++;
    { int32_t saved_y = G32(VA_g_view_bound_bottom);                                   /* push dword [0x9096c] ({0x9096c,0x9096e}) */
      int32_t mul2 = G32(VA_g_perspective_scale), divv = G32(VA_g_view_clip_plane), cy0 = G32(VA_g_span_src_wrap_reoffset + 0x28);   /* divisor STALE by design */
      int32_t v = rwss_proj((int32_t)(int16_t)G16(VA_g_world_span_bottom), mul2, divv, cy0); /* 0x2baca..0x2bafd */
      if ((int16_t)(uint16_t)v <= (int16_t)G16(VA_g_view_bound_bottom))                /* jg-skip -> min-update */
          G16(VA_g_view_bound_bottom) = (uint16_t)v;                                   /* 0x2bb06 bottom bound */
      v = rwss_proj((int32_t)(int16_t)G16(VA_g_world_span_top), mul2, divv, cy0);   /* 0x2bb0c.. */
      if ((int16_t)(uint16_t)v >= (int16_t)G16(VA_g_view_bound_top))                /* jl-skip -> max-update */
          G16(VA_g_view_bound_top) = (uint16_t)v;                                   /* 0x2bb48 top bound */
      if ((int16_t)G16(VA_g_view_bound_top) <= (int16_t)G16(VA_g_view_bound_bottom))               /* 0x2bb54 jg-skip: call when top<=bottom */
          rwss_wall_call(blk, es, fs, gs);                              /* 0x2bb5d */
      G32(VA_g_view_bound_bottom) = saved_y;                                           /* 0x2bb62 pop -> restore the y pair */
      G32(VA_g_view_bound_right) = saved_x;                                           /* 0x2bb6b pop -> restore the x pair */
      return; }                                                         /* 0x2bb71 ret (pop fs/es collapse) */

subpass:                                                                /* 0x2bb72: pass-1/2 y-clamp variant */
    g_rwss_t1_sub++;
    { int32_t saved_y = G32(VA_g_view_bound_bottom);                                   /* push dword [0x9096c] */
      int32_t mul2 = G32(VA_g_perspective_scale), divv = G32(VA_g_view_clip_plane), cy0 = G32(VA_g_span_src_wrap_reoffset + 0x28);
      if (G8(VA_g_secondary_subpass_id) == 1                                              /* 0x2bb78 cmp,1; jne else */
          && (int16_t)G16(VA_g_world_span_bottom) < (int16_t)G16(VA_g_anim_clock + 0xa)) {           /* 0x2bb81 movsx BEFORE cmp; jge else */
          int32_t v = rwss_proj((int32_t)(int16_t)G16(VA_g_world_span_bottom), mul2, divv, cy0);   /* 0x2bb91.. */
          if ((int16_t)(uint16_t)v <= (int16_t)G16(VA_g_view_bound_bottom))            /* 0x2bbbd jg 0x2bc10 -> min-update */
              G16(VA_g_view_bound_bottom) = (uint16_t)v;                               /* pass1: clamp bottom */
      } else {                                                          /* 0x2bbce (pass2 or pass1 fallthrough) */
          int32_t v = rwss_proj((int32_t)(int16_t)G16(VA_g_world_span_top), mul2, divv, cy0);
          if ((int16_t)(uint16_t)v >= (int16_t)G16(VA_g_view_bound_top))            /* 0x2bc01 jl-skip -> max-update */
              G16(VA_g_view_bound_top) = (uint16_t)v;                               /* clamp top */
      }
      if ((int16_t)G16(VA_g_view_bound_top) <= (int16_t)G16(VA_g_view_bound_bottom))               /* 0x2bc10/0x2bc16 same driver gate */
          rwss_wall_call(blk, es, fs, gs);                              /* 0x2bc1f */
      G32(VA_g_view_bound_bottom) = saved_y;                                           /* 0x2bc24 */
      G32(VA_g_view_bound_right) = saved_x;                                           /* 0x2bc2d */
      return; }                                                         /* 0x2bc33 ret */
}

/* ===== rwss types 0/4 (0x2b58c / 0x2b581): billboard-sprite secondary surface (disasm re-verified 0x2b581-0x2b5ea + 0x2d732-0x2d756). The original
 * splices into draw_world_sprite_billboard mid-body (jmp 0x2d732) with fs/es pushed at 0x2b5a5 and
 * popped by the billboard epilogue (0x2d753) — in C the stack splice collapses into the four native
 * sprite-queue stage calls, with fs = [0x852c8] at the draw stage (loaded 0x2b5b0 AFTER the push:
 * the one real semantic difference vs the plain billboard body). All four stages are
 * already-native C. */
volatile unsigned long g_rwss_t04 = 0;                              /* coverage: green claims need >0 */
static void rwss_type04(uint8_t type, uint32_t esi_in, uint16_t es, uint16_t fs_entry, uint16_t gs)
{
    extern uint32_t (*g_os_sel_base)(uint16_t);
    (void)fs_entry;                                                 /* the draw runs under the [0x852c8] override */
    uint32_t rec = (type == 4)
        ? *(volatile uint32_t *)(uintptr_t)(*(volatile uint32_t *)(uintptr_t)(
              *(volatile uint32_t *)(uintptr_t)(esi_in + 4) + 4) + 8)    /* 0x2b581 triple chain (as types 2/3) */
        : *(volatile uint32_t *)(uintptr_t)(esi_in + 4);                 /* 0x2b58c direct record */
    if (G8(VA_g_world_render_subpass_kind) != 0) { G8(VA_g_world_render_subpass_kind) = 6; G32(VA_g_current_decoded_frame + 0x34) = (int32_t)rec; }   /* 0x2b58f subpass id 6 + sole
                                                                               * [0x84978] latch (reader 0x37c26) */
    uint32_t p_ebx = (uint32_t)(int32_t)*(volatile int16_t *)(uintptr_t)(rec + 0x14);   /* 0x2b5a8 movsx */
    uint32_t p_edx = (uint32_t)(int32_t)*(volatile int16_t *)(uintptr_t)(rec + 0x16);   /* 0x2b5ac movsx */
    uint16_t fs_ov = (uint16_t)G16(VA_g_surface_record_selector);                        /* 0x2b5b0 draw-stage fs override */
    uint16_t si    = *(volatile uint16_t *)(uintptr_t)(rec);        /* 0x2b5b7 si = [edi] */
    uint16_t cxv   = (uint16_t)(*(volatile uint16_t *)(uintptr_t)(g_os_sel_base(es) + (uint16_t)(si + 2u))
                                - (uint16_t)G16(VA_g_sector_cull_coord));          /* 0x2b5ba/c0 cx = es:[si+2] (TRUE 16-bit
                                                                     * wrap, 67-prefixed) - g_sector_cull_coord */
    G16(VA_g_sprite_view_angle + 0xa)   = (uint16_t)(0u - cxv);                          /* 0x2b5c7/cb/d2 neg;store;neg — write-only
                                                                     * global, KEPT (differential compares obj3) */
    uint32_t p_ecx = (uint32_t)cxv;                                 /* zero-extended: entry ecx hi16 = 0 and the
                                                                     * native project reads only cx */
    uint32_t p_eax = 2u * (uint32_t)(uint8_t)*(volatile uint8_t *)(uintptr_t)(rec + 3);  /* 0x2b5d4 frame*2
                                                                     * ([edi+3] — NOT the billboard's [edi+6]) */
    init_sprite_render_queue();                              /* 0x2b5dc call 0x3c294 (pure global seed) */
    uint32_t sub = rec + 0x1a;                                      /* 0x2b5e1/e2 pop esi; add esi,0x1a */
    /* jmp 0x2d732 splice: project([sub+0x14]) -> CF cull skips finalize+draw; finalize([sub+0x10]) */
    if (project_sprite_to_render_queue(p_eax, p_ecx, p_ebx, p_edx,
            *(volatile uint32_t *)(uintptr_t)(sub + 0x14)) & 1u)
        return;                                                     /* 0x2d742 jb 0x2d753 (frustum cull) */
    uint32_t head = finalize_sprite_render_queue(*(volatile uint32_t *)(uintptr_t)(sub + 0x10));
    draw_sprite_render_queue(head, es, fs_ov, gs);           /* 0x2d74e — fs override, es/gs = entry */
}
#endif /* ROTH_STANDALONE */

/* ===== rwss SHARED TAIL (0x2bcd1..0x2bf63) — the resolver call + everything downstream ===== */
/* Factored VERBATIM out of render_world_secondary_surface so the 0x2b6c8 seed block can enter at the resolver. Entrant
 * census (brute-force any-alignment scan): exactly TWO live callers — the types-2/3/0xff
 * main path (0x2bcc7 je / 0x2bcc9 fall-through) and the 0x2b6c8 sprite side entry (jmp at 0x2b709);
 * the third site (call 0x2bcd1 at 0x2b506) sits inside the statically-DEAD 0x2b496 orphan region
 * and needs no caller here.
 * reax   = the original's live eax at 0x2bcd1: [subrec+4] (main path, the 0x2bcb2 load) or the
 *          zero-extended descriptor word (u32)(u16)[rec+0xc] (side entry, 0x2b6f5..0x2b6f7).
 * subrec = esi at 0x2bcd1 (the rendered sub-record / the sprite-queue record). Consumed only by the
 *          resolver's bridged rare paths + as rwss_rotated_tail/rwss_wall_call's bridge-fallback esi
 *          — disasm-proven dead in the native lanes (draw_world_surface_spans takes no record
 *          param); the pre-factor code threaded the entry esi_in there (itself != the original's
 *          live esi at 0x2bf58), so the exact value is a proven don't-care — re-verified by the
 *          rwss types-2/3 differential staying green.
 * ebx/ecx/edx/edi/ebp = the GP regs at 0x2bcd1 (resolver bridge passthrough only). */
static void rwss_shared_tail(uint32_t reax, uint32_t subrec, uint32_t ebx, uint32_t ecx,
                             uint32_t edx, uint32_t edi, uint32_t ebp,
                             uint16_t es, uint16_t fs, uint16_t gs)
{
    uint32_t live_esi;   /* the resolver 0x2c720 RETURNS the resolved DAS/block record here (Ghidra dropped it) */
    {   /* 0x2bcd1 resolver — now the NATIVE emit_world_face_spans (common path native; rare paths bridge). */
        uint32_t r_eax = 0, r_cf = 0;
        /* esi=subrec (set by 0x2bc34/0x2bc6a); ebx/ecx/edx/edi/ebp = the 0x2bc3c entry GP regs (0x2bc4f..0x2bcd1
         * never writes them), now explicit params — consumed only by the resolver's bridged rare paths, which
         * DISASM-PROVEN never read them (0x2c720 clobbers ebx @0x2c732, ecx/edx/edi/ebp unread). */
        live_esi = emit_world_face_spans(reax, subrec, ebx, ecx, edx,
                                                edi, ebp, es, fs, gs, &r_eax, &r_cf);
        if (r_cf & 1u) return;                               /* jb 0x2bf69 (resolver CF / load fail) */
    }

    /* 0x2bcdc/0x2bce2: the translucency branch tests the RESOLVED record's +0xa (NOT subrec's) -> 0x2bf6a
     * tail (init block 0x41554[esi] / pool lock 0x361e7 / billboard 0x2d70c[eax=0x90a44] / pool unlock 0x361ef).
     * None of these re-enters the resolver 0x2c720, so bridging them is safe (no double-resolve). */
    if (*(volatile uint16_t *)(uintptr_t)(live_esi + 0xa) & 0x8000) {
        initialize_das_block_internal_pointers((void *)(uintptr_t)live_esi);  /* re-point 0x41554 (pure self-contained leaf; selectors defensive) */
        (void)obj_counter12_inc((uint32_t)G32(VA_g_das_cache_heap_handle));                       /* re-point 0x361e7 pool-lock (++[ptr+0x12]; ret unused; pure-DS) */
        G32(VA_g_das_eviction_protected_allocation) = (int32_t)G32(VA_g_format_flags + 0x1f);                                       /* 0x2bf79 */
        /* 0x2d70c draw_world_sprite_billboard — now the NATIVE orchestrator. Its four
         * stage bodies are fully lifted C; contract is eax=record ([0x90a44]), esi=live_esi, es/fs/gs threaded
         * (the body re-derives all other regs internally, matching the 0x2d70c entry ABI). */
        draw_world_sprite_billboard((uint32_t)G32(VA_g_map_das_fat_buffer + 0xc), live_esi, es, fs, gs);
        G32(VA_g_das_eviction_protected_allocation) = 0;                                                            /* 0x2bf8f */
        (void)obj_counter12_dec((uint32_t)G32(VA_g_das_cache_heap_handle));                       /* re-point 0x361ef pool-unlock (--[ptr+0x12]; ret unused; pure-DS) */
        return;
    }

    /* ---- fill the projection-input globals from the RESOLVED record (0x2bce8..0x2bd49, esi=live_esi) ---- */
    #define SR16(o) (*(volatile uint16_t *)(uintptr_t)(live_esi + (o)))   /* live_esi = resolver's returned record */
    G16(VA_g_world_alt_render_flags + 0x2) = (uint16_t)SR16(8);                                  /* fs selector */
    { uint16_t h = (uint16_t)SR16(0xe); G16(VA_g_span_src_wrap_reoffset + 0xc) = h; G16(VA_g_span_src_wrap_reoffset + 0xe) = h;
      uint16_t h2 = (uint16_t)(h + h); G16(VA_g_wall_proj_y3 + 0x8) = h2; G16(VA_g_span_src_wrap_reoffset + 0x8) = h2; G16(VA_g_span_src_wrap_reoffset + 0x12) = h2; }
    G16(VA_g_column_clip_mode + 0x4) = 0xffff;
    uint8_t cl;
    { uint16_t w = (uint16_t)SR16(0xc); G16(VA_g_span_src_row_width) = w;
      G16(VA_g_span_src_wrap_reoffset) = (uint16_t)(w - 1); uint16_t w2 = (uint16_t)(w + w); cl = 7;
      G16(VA_g_span_fill_mode_word + 0xe) = w2; G16(VA_g_wall_proj_y3 + 0x4) = w2; G16(VA_g_span_src_wrap_reoffset + 0x4) = w2; }
    { uint16_t dx = (uint16_t)SR16(0xa);                              /* 0x2bd49 bit-decode of [0x90980] */
      if (dx & 0x80) {
        if (dx & 0x6000) {
            uint16_t rdx = (uint16_t)((dx << 3) | (dx >> 13));         /* rol dx,3 */
            uint8_t dl = (uint8_t)(rdx & 3);                           /* and dl,3 */
            uint8_t oc = cl; cl = dl; dl = oc;                        /* xchg cl,dl */
            G16(VA_g_span_src_wrap_reoffset + 0x4) = (uint16_t)(G16(VA_g_span_src_wrap_reoffset + 0x4) << (cl & 0x1f));   /* shl [0x90980],cl */
            cl = (uint8_t)(cl + dl);                                   /* add cl,dl */
        } else {
            cl = (uint8_t)(cl - 1);                                    /* dec cl */
            G16(VA_g_span_src_wrap_reoffset + 0x4) = (uint16_t)((uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x4) >> 1);   /* shr [0x90980],1 */
        }
      }
    }

    /* 0x2bd76: cmp byte [0x853d6],0 ; jne 0x2bfa4 — the rotated-projection divergence. */
    if (G8(VA_g_render_sector_walk_mode + 0x3) != 0) {
        g_rwss_rot++;
        rwss_rotated_tail(cl, subrec, es, fs, gs);
        return;
    }

    /* ---- linear projection of the 4 screen extents (0x2bd83..0x2bf63), [0x90968] saved/restored ---- */
    int32_t saved_90968 = G32(VA_g_view_bound_right);                               /* push [0x90968] */
    int32_t mul1 = G32(VA_g_view_params_block + 0xc), divv = G32(VA_g_view_clip_plane);
    int32_t cx0 = G32(VA_g_span_src_wrap_reoffset + 0x24), cy0 = G32(VA_g_span_src_wrap_reoffset + 0x28);
    { uint32_t e = ((uint32_t)(uint16_t)G16(VA_g_wall_proj_y3 + 0x8)) << (cl & 0x1f);  /* 0x2bd89 */
      /* (g_rwss_dbg now holds the resolver diagnostics — don't clobber here) */
      G32(VA_g_current_proc_tag + 0x124) = (int32_t)e;
      int32_t v = rwss_proj((int32_t)e + G32(VA_g_visible_extent_list + 0x3c), mul1, divv, cx0);    /* 0x2bd98 -> [0x90960] */
      G16(VA_g_wall_proj_y3 + 0x12) = (uint16_t)v;
      if ((int16_t)(uint16_t)v <= (int16_t)G16(VA_g_camera_cos + 0x2)) { G32(VA_g_view_bound_right) = saved_90968; return; }   /* 0x2bdd0 */
    }
    { int32_t v = rwss_proj(G32(VA_g_visible_extent_list + 0x3c) - G32(VA_g_current_proc_tag + 0x124), mul1, divv, cx0);  /* 0x2bddd -> [0x90958] */
      G16(VA_g_wall_proj_y3 + 0xa) = (uint16_t)v;
      if ((int16_t)(uint16_t)v >= (int16_t)G16(VA_g_camera_cos + 0x4)) { G32(VA_g_view_bound_right) = saved_90968; return; }   /* 0x2be1a */
    }
    int32_t mul2 = G32(VA_g_perspective_scale);
    { int32_t ecx = (int32_t)(uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x4);                  /* 0x2be27 cx=[0x90980] */
      int32_t eax = (int32_t)(int16_t)G16(VA_g_anim_clock + 0xa);                   /* movsx [0x85334] */
      uint8_t dl = (uint8_t)G8(VA_g_secondary_surface_count + 0x2);
      int32_t edx = (int32_t)(int16_t)(uint16_t)(((uint16_t)(dl & 0xf) * 2u) + (uint16_t)G16(VA_g_current_proc_tag + 0x11e));
      if (dl & 0x10) eax = eax - edx + ecx;                           /* 0x2be3d branch */
      else           eax = eax + edx;
      int32_t topv = eax;                                             /* push eax (0x2be68) */
      G16(VA_g_span_src_wrap_reoffset + 0xa) = (uint16_t)(eax - ecx);                           /* 0x2be69 [0x90986]=ax-cx */
      int32_t v = rwss_proj(eax - ecx, mul2, divv, cy0);              /* 0x2be71 -> [0x90962]/[0x9095a] */
      G16(VA_g_wall_proj_y3 + 0x14) = (uint16_t)v; G16(VA_g_wall_proj_y3 + 0xc) = (uint16_t)v;
      v = rwss_proj(topv, mul2, divv, cy0);                           /* 0x2beaa (pop eax) -> [0x90964]/[0x9095c] */
      G16(VA_g_wall_proj_y3 + 0x16) = (uint16_t)v; G16(VA_g_wall_proj_y3 + 0xe) = (uint16_t)v;
    }
    { uint16_t a = (uint16_t)G16(VA_g_view_clip_plane + 0x1); G16(VA_g_wall_proj_y3 + 0x10) = a; G16(VA_g_wall_proj_y3 + 0x18) = a; }   /* 0x2bee2 */
    G8(VA_g_column_clip_mode) = (uint8_t)G8(VA_g_span_clip_source);                               /* 0x2bef4 */
    G16(VA_g_world_surface_draw_flags + 0x4) = 0;                                                 /* 0x2befe */
    G8(VA_g_sprite_fill_index + 0x2) = 0;                                                  /* 0x2bf07 */

    /* ---- store-record gate + wall driver: the factored epilogue blocks (0x2bf0e..0x2bf62; the
     * verified linear C moved verbatim into rwss_store_gate/rwss_wall_call above, shared with the
     * rotated tail whose original blocks are disasm-identical) ---- */
    rwss_store_gate();                                               /* 0x2bf0e..0x2bf53 */
    rwss_wall_call(subrec, es, fs, gs);                              /* 0x2bf58..0x2bf62 */
    G32(VA_g_view_bound_right) = saved_90968;                                      /* pop [0x90968] (0x2bf63) */
    #undef SR16
}

#ifdef ROTH_STANDALONE
/* ===== 0x2b6c8 sprite side entry (70-B / 13-insn seed block) =====
 * render_world_sprite's [rec+0x34]==0 branch (`je 0x2b6c8` at 0x36655 — scan-proven the block's ONLY
 * entrant): a sprite-queue record with NO drawable sprite frame is rendered as a secondary SURFACE —
 * re-seed the five projection globals from the record's projected vertex record A ([rec+0x30]+
 * (u16)[rec+0x36]; fields per project_sprite_to_render_queue's writer: +6 viewX / +8 height /
 * +0xa depth), then enter the rwss shared tail AT the resolver (`jmp 0x2bcd1` at 0x2b709).
 * Deliberately NOT re-seeded (stale by original design — do not "fix"): the [0x853d6]
 * rotated latch, [0x90a44] current render record, [0x90970]/[0x84ab8]/[0x90a26]; and the main path's
 * [subrec+7]&0x10 x-flip test (0x2bcc3) is bypassed — [0x9093c] = plain 0x19.
 * IMGFREE-ONLY (the rwss_type1_handler precedent): the trap lane keeps the byte-faithful call_orig
 * bridge in lift_render_world.c. Tail GP zeros = the disasm-proven don't-cares (at 0x2b709
 * ebx = the vertex ptr, ecx/edx/edi/ebp = caller state — all dead downstream). */
volatile unsigned long g_rwss_sprite_side = 0;   /* coverage: an imgfree green claim needs this >0 */
void rwss_sprite_side_entry(uint32_t rec, uint16_t es, uint16_t fs, uint16_t gs)
{
    g_rwss_sprite_side++;
    uint32_t v = *(volatile uint32_t *)(uintptr_t)(rec + 0x30)
               + (uint32_t)*(volatile uint16_t *)(uintptr_t)(rec + 0x36);  /* 0x2b6c8 sub ebx,ebx; mov bx,
                                                          * [esi+0x36]; add ebx,[esi+0x30] -> vertex record A */
    G16(VA_g_anim_clock + 0xa) = (uint16_t)(-(int16_t)*(volatile uint16_t *)(uintptr_t)(v + 8));
                                                          /* 0x2b6d1/0x2b6d5/0x2b6d7: mov ax,[ebx+8]; neg eax
                                                           * (stale-upper eax; low word mod-2^16 exact); w16 store */
    G32(VA_g_visible_extent_list + 0x3c) = ((int32_t)(int16_t)*(volatile uint16_t *)(uintptr_t)(v + 6)) << 8;
                                                          /* 0x2b6dd/0x2b6e1/0x2b6e4: movsx eax,[ebx+6]; shl 8; W32 */
    G32(VA_g_view_clip_plane) = ((int32_t)(int16_t)*(volatile uint16_t *)(uintptr_t)(v + 0xa)) << 8;
                                                          /* 0x2b6e9/0x2b6ed/0x2b6f0: movsx eax,[ebx+0xa]; shl 8; W32 */
    uint32_t desc = (uint32_t)*(volatile uint16_t *)(uintptr_t)(rec + 0xc);
                                                          /* 0x2b6f5/0x2b6f7: sub eax,eax; mov ax,[esi+0xc] (exact) */
    G32(VA_g_current_proc_tag + 0x118) = (int32_t)desc;                         /* 0x2b6fb FULL 32-bit store -> angle byte [0x84ab6] = 0 */
    G16(VA_g_world_surface_draw_flags) = 0x19;                                  /* 0x2b700 mode word (w16; no 0x2bcc3 x-flip xor) */
    rwss_shared_tail(desc, rec, 0, 0, 0, 0, 0, es, fs, gs);   /* 0x2b709 jmp 0x2bcd1 (eax=desc, esi=rec) */
}
#endif /* ROTH_STANDALONE */


void render_world_secondary_surface(uint32_t esi_in, uint32_t ebx, uint32_t ecx, uint32_t edx,
                                           uint32_t edi, uint32_t ebp,
                                           uint16_t es, uint16_t fs, uint16_t gs)
{
    uint8_t type = *(volatile uint8_t *)(uintptr_t)(esi_in + 8);
    g_rwss_type[type]++;
    /* Types 0/1/4 have their own original handlers (0x2b58c/0x2b716/0x2b581) — bridge them. Types 2/3/0xff
     * take the 0x2bc6a main path, now TRANSCRIBED + VERIFIED byte-identical (the rotated-0x80 / translucency-
     * 0x8000 / resolver-skip sub-branches bridge internally). (Was opt-in via ROTH_RWSS_LIVE; now default.)
     * IMGFREE lane: type 1 is NATIVE (rwss_type1_handler). Dispatch order mirrors 0x2bc3c exactly:
     * types 0/4 branch away at 0x2bc41/0x2bc49 BEFORE the 0x2bc4f subpass poke; the poke + the 0x2bc5f
     * esi=[esi+4] deref run for type 1 (and 2/3/0xff, whose poke stays at its existing site below —
     * byte-equivalent order for them) before the 0x2bc62/0x2b70e fork into the handler. */
    if (type == 0 || type == 1 || type == 4) {
#ifdef ROTH_STANDALONE
        if (type == 1) {
            if (G8(VA_g_world_render_subpass_kind) != 0) G8(VA_g_world_render_subpass_kind) = 4;                                   /* 0x2bc4f poke (hoisted
                                                                                      * for type 1) */
            g_rwss_t1++;
            rwss_type1_handler(*(volatile uint32_t *)(uintptr_t)(esi_in + 4),        /* 0x2bc5f sr=[esi+4] */
                               es, fs, gs);
            return;
        }
        /* types 0/4 (0x2bc41 je / 0x2bc49 je) branch away BEFORE the 0x2bc4f subpass poke — they set
         * their own subpass id 6 inside the handler. */
        g_rwss_t04++;
        rwss_type04(type, esi_in, es, fs, gs);
        return;
#endif
        rwss_bridge(esi_in, es, fs, gs); return;
    }
    uint32_t sr = *(volatile uint32_t *)(uintptr_t)(esi_in + 4);
    uint32_t subrec = (type == 0xff) ? sr                                            /* 0x2bc5f */
        : *(volatile uint32_t *)(uintptr_t)(*(volatile uint32_t *)(uintptr_t)(sr + 4) + 8);  /* 0x2bc34/37 */
    /* the rotated-projection branch ([subrec+9]&0x80) is NATIVE now — the pre-emptive whole-function
     * bridge that used to live here is GONE. The shared path below stores the true flag into [0x853d6]
     * (the original's only write, 0x2bc99) and the divergence happens at the faithful 0x2bd76 site (after
     * the bit-decode, where `cl` is live) into rwss_rotated_tail. The translucency branch (0x8000) stays a
     * POST-resolver test on the resolver's RETURNED record (live_esi+0xa) and PRECEDES the divergence —
     * a rotated+translucent record takes the billboard tail and never reaches 0x2bd76. */

    /* ===== MAIN path (0x2bc6a..0x2bf69): linear-projection, non-translucency. 'subrec' = the rendered record. */
    g_rwss_lin++;
    if (G8(VA_g_world_render_subpass_kind) != 0) G8(VA_g_world_render_subpass_kind) = 4;                              /* 0x2bc4f */
    G32(VA_g_map_das_fat_buffer + 0xc) = (int32_t)subrec;                                     /* 0x2bc6a */
    {                                                                   /* 0x2bc70 [0x90970] from [subrec+8] */
        uint8_t al = *(volatile uint8_t *)(uintptr_t)(subrec + 8);
        if (al != 0 && G8(VA_g_span_clip_source) != 0) al = (uint8_t)((uint8_t)(al - 0x80) + (uint8_t)G8(VA_g_span_clip_source));
        G8(VA_g_column_clip_mode) = al;
    }
    G32(VA_g_current_proc_tag + 0x11c) = 0;                                                   /* 0x2bc8d */
    G8(VA_g_render_sector_walk_mode + 0x3) = (uint8_t)(*(volatile uint8_t *)(uintptr_t)(subrec + 9) & 0x80);   /* 0x2bc94 (==0 here) */
    { int16_t t = (int16_t)((uint16_t)*(volatile uint16_t *)(uintptr_t)(subrec + 0xa) - (uint16_t)G16(VA_g_sector_cull_coord));
      G16(VA_g_anim_clock + 0xa) = (uint16_t)(-t); }                                  /* 0x2bc9e */
    G32(VA_g_current_proc_tag + 0x118) = *(volatile int32_t *)(uintptr_t)(subrec + 4);        /* 0x2bcb2 */
    G16(VA_g_world_surface_draw_flags) = 0x19;                                               /* 0x2bcba */
    if (*(volatile uint8_t *)(uintptr_t)(subrec + 7) & 0x10) G16(VA_g_world_surface_draw_flags) = (uint16_t)(G16(VA_g_world_surface_draw_flags) ^ 2);  /* 0x2bcc3 */
    /* ---- 0x2bcd1 onward: the SHARED TAIL (resolver + downstream), factored into rwss_shared_tail
     * above so the 0x2b6c8 sprite side entry can reuse it. eax at 0x2bcd1 = [subrec+4] — the 0x2bcb2
     * load re-read here (the pre-factor code also read [subrec+4] twice: 0x84ab4 store + reax). ---- */
    rwss_shared_tail(*(volatile uint32_t *)(uintptr_t)(subrec + 4), subrec,
                     ebx, ecx, edx, edi, ebp, es, fs, gs);
}

/* draw_world_sprite_billboard (0x2d70c) — draws one billboard sprite (entity/object or secondary surface).
 * 74B orchestrator over the sprite-queue pipeline: init_sprite_render_queue (0x3c294) -> project_sprite_to_
 * render_queue (0x3c2bd, CF set on frustum cull / frame-index overflow -> skip the draw) -> finalize_sprite_
 * render_queue (0x3c477) -> draw_sprite_render_queue (0x3b1b1). NATIVE: all four stage
 * bodies are fully lifted C — init = pure global seed; project = native (calls floorceil_rotation_
 * sincos); finalize = native (calls build + depth_sort); draw = native loop over render_world_sprite
 * (whose deep SMC-rasterizer tail 0x366d2 + secondary-surface resolver 0x2b6c8 stay byte-faithfully bridged).
 * The original relies on callee GP-register preservation across the stages (e.g. edi=record survives 0x3c294
 * for the [edi+6] frame-index read); here each stage's inputs are plain locals, so preservation is automatic.
 * eax=record ptr, esi=a second record ptr ([esi+0x14] projected, [esi+0x10] finalized); es/fs/gs = trap entry
 * selectors, consumed only by the draw stage's per-sprite rasterizer. Disasm-verified frame (0x2d70c):
 *   init(void); project(eax,ecx,ebx,edx,esi)->CF; finalize(esi)->eax; draw(esi,es/fs/gs).
 * The original loads edi=[0x85270] before project, but 0x3c2bd (and its sincos callee 0x3bdf3) never read
 * EDI, so the lifted project proto correctly drops it (confirmed: no EDI use in either disasm). */
void draw_world_sprite_billboard(uint32_t eax_in, uint32_t esi_in,
                                        uint16_t es, uint16_t fs, uint16_t gs)
{
    uint32_t record = eax_in;                                                      /* 0x2d70f mov edi,eax */

    /* 0x2d711-0x2d723: record fields for the PROJECT stage — set before init, must survive it (init writes
     * only globals; here p_ebx/p_edx/p_ecx are locals so survival is automatic). */
    uint32_t p_ebx = (uint32_t)(int32_t)*(volatile int16_t *)(uintptr_t)(record);          /* movsx [edi+0] */
    uint32_t p_edx = (uint32_t)(int32_t)*(volatile int16_t *)(uintptr_t)(record + 2);       /* movsx [edi+2] */
    uint32_t p_ecx = (uint32_t)(int32_t)(int16_t)((uint16_t)*(volatile uint16_t *)(uintptr_t)(record + 0xa)
                                          - (uint16_t)G16(VA_g_sector_cull_coord));               /* movsx (cx - [0x852fa]) */

    init_sprite_render_queue();                                             /* 0x3c294 (pure global seed) */

    /* 0x2d72b-0x2d73c: frame-index*2 + the two source ptrs -> project_sprite_to_render_queue (0x3c2bd) */
    uint32_t p_eax = (uint32_t)(uint8_t)*(volatile uint8_t *)(uintptr_t)(record + 6) * 2u;  /* al=[edi+6]; *2 */
    uint32_t p_esi = *(volatile uint32_t *)(uintptr_t)(esi_in + 0x14);
    if (project_sprite_to_render_queue(p_eax, p_ecx, p_ebx, p_edx, p_esi) & 1u)
        return;                                                                    /* jb 0x2d753 (cull/overflow) */

    /* 0x2d744-0x2d747: finalize_sprite_render_queue (0x3c477), esi=[esi_in+0x10] -> sorted queue head */
    uint32_t queue = finalize_sprite_render_queue(*(volatile uint32_t *)(uintptr_t)(esi_in + 0x10));

    /* 0x2d74c-0x2d74e: draw_sprite_render_queue (0x3b1b1), esi = finalize result; es/fs/gs -> rasterizer */
    draw_sprite_render_queue(queue, es, fs, gs);
}

/* ===================== Render — tier-3 scene geometry ===================== */

/* transform_world_vertices (0x2a814) — per-frame world->view vertex transform. NO register/stack args;
 * all state is global + the selector-mapped transformed-vertex table (es = g_vertex_selector 0x852cc).
 * Computes the camera-yaw sin/cos from g_sprite_view_angle (0x909f8), stores them into g_camera_sin/_cos
 * (0x85310/0x85312), then walks the vertex table (stride 0xc): for each vertex it translates the world
 * X/Y (vtx+8/+0xa) by g_view_offset_x/_y (0x90a04/0x90a06) in 16-bit (WRAPPING), rotates by the camera
 * basis, and writes the view coords back as dwords vtx+0=edx, vtx+4=eax. Reuses the already-verified
 * leaves sincos_pair (0x3bdd2) + rotate_point_2d (0x2a898).
 *
 * sin/cos wiring (subtle, byte-checked vs disasm): sincos_pair returns CX=table[base] in *sin_out
 * and BX=table[base+0x80words] in *cos_out; the original then stores g_camera_sin=BX (=*cos_out) and
 * g_camera_cos=CX (=*sin_out). Kept EXACT (the analyst's C skeleton had these swapped — disasm wins).
 *
 * Output = obj3 (the two camera globals) + the in-place vertex-table mutation. The table is selector-
 * mapped, OUTSIDE obj3 (a DPMI buffer), so the differential snapshots/diffs that region too
 * (lift_diff_vtxtransform). double-run-OK: idempotent — recomputes view coords from the static world
 * coords (+8/+0xa, never written) each call. EBP is clobbered in the original (raw mov ebp,esp scratch
 * frame, no push/pop) — irrelevant to a C lift. Addressing is via the full record pointer (matches the
 * `add esi,0xc` 32-bit advance); the index stays <64K for real tables so 16-bit si wrap never bites. */
void transform_world_vertices(void)
{
    extern uint32_t (*g_os_sel_base)(uint16_t);
    #ifndef ROTH_STANDALONE
    if (g_os_sel_base == NULL) {                       /* selector hook required for the table access */
        regs_t io; memset(&io, 0, sizeof io);
        io.va = 0x2a814u + OBJ_DELTA;  /* [ORACLE-FALLBACK] */
        call_orig(&io);
        return;
    }
    #endif
    uint8_t *vbase = (uint8_t *)(uintptr_t)g_os_sel_base((uint16_t)G16(VA_g_vertex_selector));   /* es = g_vertex_selector */

    /* 0x2a81b..0x2a830: bx=g_sprite_view_angle; neg ebx; sincos_pair; g_camera_sin=bx, g_camera_cos=cx */
    uint16_t sc_sin, sc_cos; uint32_t sc_tbl;
    sincos_pair((uint32_t)(-(int32_t)(int16_t)G16(VA_g_sprite_view_angle)), &sc_sin, &sc_cos, &sc_tbl);
    (void)sc_tbl;
    G16(VA_g_camera_sin) = sc_cos;                              /* g_camera_sin = bx (= *cos_out) */
    G16(VA_g_camera_cos) = sc_sin;                              /* g_camera_cos = cx (= *sin_out) */

    uint16_t arr_off = *(volatile uint16_t *)(uintptr_t)(vbase + 2);                  /* si = es:[2] */
    uint16_t count   = *(volatile uint16_t *)(uintptr_t)(vbase + (uint16_t)(arr_off - 2));  /* cx = es:[si-2] */
    if (count == 0) return;                             /* test cx,cx; je 0x2a897 */

    int16_t neg_ang = (int16_t)(-(int32_t)(int16_t)G16(VA_g_sprite_view_angle));   /* scratch[+4] = -angle, set ONCE */
    uint8_t *rec = vbase + arr_off;
    uint16_t cx = count;
    do {
        int16_t pt[3];
        pt[0] = (int16_t)((uint16_t)*(volatile uint16_t *)(uintptr_t)(rec + 8)
                          + (uint16_t)G16(VA_g_view_offset_x));    /* X' = worldX + g_view_offset_x  (16-bit wrap) */
        pt[1] = (int16_t)((uint16_t)*(volatile uint16_t *)(uintptr_t)(rec + 0xa)
                          + (uint16_t)G16(VA_g_view_offset_y));    /* Y' = worldY + g_view_offset_y  (16-bit wrap) */
        pt[2] = neg_ang;                                /* flags gate = -angle */
        int32_t r_eax = 0, r_edx = 0;
        rotate_point_2d(pt, &r_eax, &r_edx);
        *(volatile int32_t *)(uintptr_t)(rec + 0) = r_edx;   /* es:[si]   = edx (view X) */
        *(volatile int32_t *)(uintptr_t)(rec + 4) = r_eax;   /* es:[si+4] = eax (depth)  */
        rec += 0xc;                                     /* add esi,0xc */
        cx = (uint16_t)(cx - 1);                        /* dec cx */
    } while ((int16_t)cx > 0);                          /* jg (signed) */
}

/* clip_sector_walls_to_view (0x2d793) — per-sector wall→view-frustum clip. ESI=sector record offset (ES =
 * g_surface_record_selector 0x852c8); GS = g_vertex_selector 0x852cc (transformed view coords, READ-ONLY).
 * Returns the result in CF: CF=1 "this sector crosses/continues the portal chain", CF=0 "terminal/visible".
 *
 * INCREMENTAL: only the **pass-kind-2** path (g_clip_pass_kind 0x853d8 == 2 — the per-frame portal-walk
 * extent+CF computation that walk_visible_sectors drives) is native here. It has NO recursion and NO worklist
 * arena — it just scans the sector's walls, finds where each edge straddles the clip plane g_view_clip_plane
 * (0x85264), tracks the screen-X extent [ebx,ebp] + the two clip endpoints (0x849a8/0x849ac, obj3), projects
 * the extent, and returns CF. pass-kind 0 (depth-sorted insert -> worklist arena, OUTSIDE obj3, via 0x2a446)
 * and 1 (extent triple) are BRIDGED once (deferred until the arena-snapshot harness exists). The differential
 * SKIPS non-pass2 calls (bridging them inside the double-run would double-advance the arena bump cursor).
 *
 * ABI quirks kept byte-exact: ESI-in / CF-out (both dropped by the corpus); the 64-bit signed imul/idiv
 * clip-plane interpolation; the projection clamp [-0x1000,0x1000]; the signed-byte wall counter; the
 * min/max sentinels 0x7fffffff/0x80000000. The wall offset stored at the endpoints is the loop's wall ptr
 * (sector ptr's high 16 bits are 0, so it equals the original's full ESI). */
int clip_sector_walls_to_view(uint32_t esi_sector, uint16_t gs_sel, uint16_t es_sel)
{
    extern uint32_t (*g_os_sel_base)(uint16_t);
#ifndef ROTH_STANDALONE
    if (G8(VA_g_render_sector_walk_mode + 0x5) != 2 || g_os_sel_base == NULL) {              /* trap host: bridge pass-kind 0/1 once.
                                                                    * The differential SKIPS non-pass2 calls
                                                                    * (running them in the double-run would
                                                                    * double-advance the worklist arena cursor),
                                                                    * so the native 0/1 body below is imgfree-only. */
        regs_t io; memset(&io, 0, sizeof io);
        io.va = 0x2d793u + OBJ_DELTA; io.esi = esi_sector;  /* [ORACLE-FALLBACK] */
        io.gs = gs_sel; io.es = es_sel; io.fs = (uint16_t)G16(VA_g_map_geometry_selector);
        call_orig(&io);
        return (int)(io.eflags & 1u);
    }
#else
    if (g_os_sel_base == NULL) {                                  /* imgfree: selector base must be bound */
        roth_unreachable(0x2d793u);
        return 1;
    }
    /* imgfree: pass-kinds 0/1/2 ALL run native (no call_orig). (below) transcribes the 0/1
     * recursive portal-frustum walk instruction-literal from the disasm at 0x2d8a8..0x2da5a. */
#endif
    uint32_t es_base = g_os_sel_base(es_sel);
    uint32_t gs_base = g_os_sel_base(gs_sel);
    #define ES8(o)  (*(volatile uint8_t  *)(uintptr_t)(es_base + (uint32_t)(o)))
    #define ES16(o) (*(volatile uint16_t *)(uintptr_t)(es_base + (uint32_t)(o)))
    #define GS32(o) ((int32_t)*(volatile int32_t *)(uintptr_t)(gs_base + (uint32_t)(o)))

    int32_t  clip = (int32_t)G32(VA_g_view_clip_plane);                          /* g_view_clip_plane (edx) */
    uint8_t  cl   = ES8(esi_sector + 0xd);                          /* WALL_COUNT (signed byte counter) */
    G32(VA_g_current_proc_tag + 0xc) = 0;                                               /* clip_endpoint_left  = 0 */
    G32(VA_g_current_proc_tag + 0x10) = 0;                                               /* clip_endpoint_right = 0 */
    int32_t  ebx  = 0x7fffffff;                                     /* LEFT extent accumulator (min) */
    int32_t  ebp  = (int32_t)0x80000000u;                           /* RIGHT extent accumulator (max) */
    uint32_t wall = ES16(esi_sector + 0xe);                         /* current wall record offset */

    do {
        uint32_t diA = ES16(wall);                                 /* wall[+0] = vtxA off */
        int32_t depthA = GS32(diA + 4);
        uint32_t onplane_off = 0; int do_onplane = 0;

        if (clip < depthA) {                                       /* 0x2d819: A in front of the plane */
            uint32_t diB = ES16(wall + 2);
            int32_t depthB = GS32(diB + 4);
            if (clip < depthB) {                                   /* both in front -> skip */
            } else if (clip == depthB) {                           /* B on plane */
                onplane_off = diB; do_onplane = 1;
            } else if (G8(VA_g_render_sector_walk_mode + 0x4) & 2) {                          /* B behind, right edge open -> right interp */
                int32_t dpA = GS32(diA + 4);                       /* depthA */
                int32_t ed  = clip - dpA;
                int32_t vxA = GS32(diA);
                int32_t den = GS32(diB + 4) - dpA;                 /* depthB - depthA */
                int32_t num = GS32(diB) - vxA;                     /* viewX_B - viewX_A */
                ebp = (int32_t)(((int64_t)num * (int64_t)ed) / (int64_t)den) + vxA;
                G32(VA_g_current_proc_tag + 0x10) = (int32_t)wall;                      /* clip_endpoint_right = wall */
            }
        } else if (clip == depthA) {                              /* 0x2d860: A exactly on plane */
            uint32_t diB = ES16(wall + 2);
            int32_t depthB = GS32(diB + 4);
            if (clip == depthB) {                                  /* both on plane -> skip */
            } else { onplane_off = diA; do_onplane = 1; }
        } else {                                                   /* 0x2d7cb: A behind the plane */
            uint32_t diB = ES16(wall + 2);
            int32_t depthB = GS32(diB + 4);
            if (clip > depthB) {                                   /* both behind -> skip */
            } else if (clip == depthB) {                           /* B on plane */
                onplane_off = diB; do_onplane = 1;
            } else if (G8(VA_g_render_sector_walk_mode + 0x4) & 1) {                          /* B in front, left edge open -> left interp */
                int32_t dpB = GS32(diB + 4);                       /* depthB */
                int32_t ed  = clip - dpB;
                int32_t vxB = GS32(diB);
                int32_t den = GS32(diA + 4) - dpB;                 /* depthA - depthB */
                int32_t num = GS32(diA) - vxB;                     /* viewX_A - viewX_B */
                ebx = (int32_t)(((int64_t)num * (int64_t)ed) / (int64_t)den) + vxB;
                G32(VA_g_current_proc_tag + 0xc) = (int32_t)wall;                      /* clip_endpoint_left = wall */
            }
        }

        if (do_onplane) {                                          /* 0x2d86d: widen by the on-plane endpoint */
            int32_t x = GS32(onplane_off);
            if (x >= ebp) { ebp = x; G32(VA_g_current_proc_tag + 0x10) = (int32_t)wall; }   /* widen RIGHT (max) */
            if (x <= ebx) { ebx = x; G32(VA_g_current_proc_tag + 0xc) = (int32_t)wall; }   /* widen LEFT  (min) */
        }
        wall += 0xc;
    } while ((int8_t)(cl = (uint8_t)(cl - 1)) > 0);                /* dec cl; jg (signed byte) */

    if (ebp == ebx) return 0;                                      /* 0x2d893 je 0x2d9ff (degenerate; CF=0,
                                                                    * and the return value is dead for 0/1) */

#ifdef ROTH_STANDALONE
    if (G8(VA_g_render_sector_walk_mode + 0x5) != 2) {                                        /* 0x2d89b: pass-kind 2 -> 0x2da5b (below).
                                                                    * imgfree-only: under the trap host pass 0/1
                                                                    * bridge at the top guard, so this native body
                                                                    * never compiles into the normal build (keeps
                                                                    * lifted.o strip-debug-identical). */
        /* ===== pass-kinds 0/1 (0x2d8a8): recursive portal-frustum walk =====
         * Transcribed instruction-literal from 0x2d8a8..0x2da5a. The original's register saves
         * (`push ebx` = the loop's LEFT extent; the prologue `push esi` = the incoming sector) are
         * AUTOMATIC across a C self-recursion: the C locals `ebx`/`ebp` and the param `esi_sector`
         * live in the parent frame and are preserved by the compiler. Only the shared obj3 GLOBALS
         * the child mutates need explicit save/restore, and the original's ASYMMETRIC set is honored
         * exactly (Block A saves 3, Block B saves 1; [0x9096a] is deliberately left modified). */
        int32_t scale = (int32_t)G32(VA_g_view_params_block + 0xc), center = (int32_t)G32(VA_g_span_src_wrap_reoffset + 0x24);

        /* --- Block A: right-edge recursion (0x2d8a8) --- */
        if (G8(VA_g_render_sector_walk_mode + 0x4) & 2) {                                     /* test [0x853d7],2 ; je 0x2d951 */
            int32_t p = (int32_t)(((int64_t)ebp * (int64_t)scale) / (int64_t)clip) + center;  /* project RIGHT */
            if (p < -0x1000) p = -0x1000;                          /* clamp [-0x1000,0x1000] */
            if (p >  0x1000) p =  0x1000;
            int16_t ax = (int16_t)p;
            if ((int16_t)ax < (int16_t)(uint16_t)G16(VA_g_wall_proj_y3 + 0x12)) {   /* cmp ax,[0x90960] ; jge -> skip */
                G16(VA_g_view_bound_right) = (uint16_t)ax;                       /* 0x2d8ea window-right = projected */
                uint32_t ep = (uint32_t)G32(VA_g_current_proc_tag + 0x10);              /* 0x2d8f0 right clip endpoint (wall off) */
                if (ep != 0 && (uint16_t)ES16(ep + 8) != 0xffff) { /* 0x2d8f6 null / 0x2d8fa neighbor solid */
                    uint32_t sv_90968 = (uint32_t)G32(VA_g_view_bound_right);    /* 0x2d902 SAVE [0x90968] (full dword) */
                    G16(VA_g_view_bound_left) = (uint16_t)ax;                   /* 0x2d909 */
                    G16(VA_g_view_bound_right) = (uint16_t)G16(VA_g_wall_proj_y3 + 0x12);         /* 0x2d90f/15 child win-right = work-right */
                    uint8_t  sv_853d7 = G8(VA_g_render_sector_walk_mode + 0x4);               /* 0x2d91c SAVE [0x853d7] */
                    uint32_t sv_849a8 = (uint32_t)G32(VA_g_current_proc_tag + 0xc);    /* 0x2d922 SAVE [0x849a8] */
                    G8(VA_g_render_sector_walk_mode + 0x4) = 2;                               /* 0x2d928 child edge = right-only */
                    uint32_t link = (uint16_t)ES16(ep + 8);        /* 0x2d92f neighbor sector link */
                    uint32_t next = (uint16_t)ES16(link + 6);      /* 0x2d934 next sector record off */
                    clip_sector_walls_to_view(next, gs_sel, es_sel);   /* 0x2d939 RECURSE */
                    G32(VA_g_current_proc_tag + 0xc) = (int32_t)sv_849a8;              /* 0x2d93e RESTORE */
                    G8(VA_g_render_sector_walk_mode + 0x4)  = sv_853d7;                       /* 0x2d944 RESTORE */
                    G32(VA_g_view_bound_right) = (int32_t)sv_90968;              /* 0x2d94b RESTORE [0x90968] (dword) */
                }
            }
        }

        /* --- Block B: left-edge recursion (0x2d951). test reads [0x853d7] AFTER Block A restored it. --- */
        if (G8(VA_g_render_sector_walk_mode + 0x4) & 1) {                                     /* test [0x853d7],1 ; je 0x2d9e0 */
            int32_t p = (int32_t)(((int64_t)ebx * (int64_t)scale) / (int64_t)clip) + center;  /* project LEFT */
            if (p < -0x1000) p = -0x1000;
            if (p >  0x1000) p =  0x1000;
            int16_t ax = (int16_t)p;
            if ((int16_t)ax > (int16_t)(uint16_t)G16(VA_g_wall_proj_y3 + 0xa)) {   /* cmp ax,[0x90958] ; jle -> skip */
                G16(VA_g_view_bound_left) = (uint16_t)ax;                       /* 0x2d993 */
                uint32_t ep = (uint32_t)G32(VA_g_current_proc_tag + 0xc);              /* 0x2d999 left clip endpoint */
                if (ep != 0 && (uint16_t)ES16(ep + 8) != 0xffff) { /* 0x2d9a3 */
                    uint32_t sv_90968 = (uint32_t)G32(VA_g_view_bound_right);    /* 0x2d9ab SAVE [0x90968] (only save) */
                    G16(VA_g_view_bound_right) = (uint16_t)ax;                   /* 0x2d9b2 */
                    G16(VA_g_view_bound_left) = (uint16_t)G16(VA_g_wall_proj_y3 + 0xa);         /* 0x2d9b8/be child win-left = work-left */
                    G8(VA_g_render_sector_walk_mode + 0x4) = 1;                               /* 0x2d9c4 child edge = left-only */
                    uint32_t link = (uint16_t)ES16(ep + 8);        /* 0x2d9cb */
                    uint32_t next = (uint16_t)ES16(link + 6);      /* 0x2d9d0 */
                    clip_sector_walls_to_view(next, gs_sel, es_sel);   /* 0x2d9d5 RECURSE */
                    G32(VA_g_view_bound_right) = (int32_t)sv_90968;              /* 0x2d9da RESTORE [0x90968] */
                }
            }
        }

        /* --- join (0x2d9e0): the prologue `pop ebx` -> ebx = incoming sector = esi_sector (C param) --- */
        if (G8(VA_g_render_sector_walk_mode + 0x5) != 0) {                                    /* 0x2d9e1 cmp [0x853d8],0 ; jne 0x2da0a */
            /* pass-kind-1 tail (0x2da0a): append the sector's visible span {sector,leftX,rightX} */
            int16_t dx = (int16_t)(uint16_t)G16(VA_g_view_bound_left);          /* window LEFT (post-recursion narrowed) */
            if (dx <= (int16_t)(uint16_t)G16(VA_g_map_geometry_selector + 0x4)) dx = (int16_t)(uint16_t)G16(VA_g_map_geometry_selector + 0x4);  /* clamp work-left */
            int16_t cx = (int16_t)(uint16_t)G16(VA_g_view_bound_right);          /* window RIGHT */
            if (cx >= (int16_t)(uint16_t)G16(VA_g_map_geometry_selector + 0x8)) cx = (int16_t)(uint16_t)G16(VA_g_map_geometry_selector + 0x8);  /* clamp work-right */
            if (dx < cx) {                                         /* 0x2da38 cmp dx,cx ; jge -> no append */
                uint32_t cnt = (uint32_t)G32(VA_g_visible_extent_count);             /* 0x2da42 count (dword read; hi16=0) */
                G16(VA_g_visible_extent_count) = (uint16_t)(cnt + 1);                /* 0x2da49 inc word [0x85220] */
                volatile uint8_t *slot = (volatile uint8_t *)GADDR(VA_g_visible_extent_list) + (uintptr_t)cnt * 6u;
                *(volatile uint16_t *)(slot + 0) = (uint16_t)esi_sector;  /* [edi+4] = sector (bx) */
                *(volatile uint16_t *)(slot + 2) = (uint16_t)dx;         /* [edi+6] = leftX */
                *(volatile uint16_t *)(slot + 4) = (uint16_t)cx;         /* [edi+8] = rightX */
            }
            return 0;                                              /* 0x2da5a ret (CF dead for kind 1) */
        }
        /* pass-kind-0 tail (0x2d9ea): depth-sorted worklist insert. [0x853c4] holds the LIVE host
         * frame ptr set by clip_and_emit_floor_walls (ebp = finalize_draw_list_entry's frame, which wrote
         * frame[0x1c]=entry @6924 and threads build_scene_draw_list's cursor at frame[8]); on i386 the
         * uint32_t token IS the host pointer. 0x2a446 = the already-lifted insert_worklist_entry. */
        uint32_t fp     = (uint32_t)G32(VA_g_reflection_view_list + 0x80);
        uint32_t entry  = *(volatile uint32_t *)(uintptr_t)(fp + 0x1c);   /* 0x2d9f0 eax = [ebp+0x1c] */
        uint32_t cursor = *(volatile uint32_t *)(uintptr_t)(fp + 8);      /* 0x2d9f3 edi = [ebp+8] */
        uint32_t fs_base = g_os_sel_base((uint16_t)G16(VA_g_map_geometry_selector));       /* fs = G16(0x85294) */
        cursor = insert_worklist_entry(cursor, entry, esi_sector, es_base, fs_base);  /* 0x2d9f6 */
        *(volatile uint32_t *)(uintptr_t)(fp + 8) = cursor;              /* 0x2d9fb [ebp+8] = edi */
        return 0;                                                  /* 0x2d9fe ret (CF dead for kind 0) */
    }
#endif  /* ROTH_STANDALONE — native pass-kind 0/1 body */

    /* PASS2 (0x2da5b): the CF-returning path */
    if (G32(VA_g_current_proc_tag + 0xc) == 0 || G32(VA_g_current_proc_tag + 0x10) == 0) return 1;          /* missing endpoint -> stc (continue) */
    int32_t scale = (int32_t)G32(VA_g_view_params_block + 0xc), center = (int32_t)G32(VA_g_span_src_wrap_reoffset + 0x24);
    int32_t pr = (int32_t)(((int64_t)ebp * (int64_t)scale) / (int64_t)clip) + center;   /* project RIGHT */
    if (pr < -0x1000) pr = -0x1000;
    if (pr >  0x1000) pr =  0x1000;
    ebp = pr;
    int32_t pl = (int32_t)(((int64_t)ebx * (int64_t)scale) / (int64_t)clip) + center;   /* project LEFT */
    if (pl < -0x1000) pl = -0x1000;
    if (pl >  0x1000) pl =  0x1000;
    ebx = pl;
    if (ebx <= ebp) { int32_t t = ebx; ebx = ebp; ebp = t; }       /* 0x2dacd: `jg` SKIPS the swap, so the swap
                                                                    * runs on fall-through (ebx<=ebp) => force
                                                                    * ebx=max, ebp=min. The next two tests are then
                                                                    * an OVERLAP test vs [workL,workR] (NOT a
                                                                    * containment test — a subtle inverted-branch). */
    if (ebx < (int32_t)G32(VA_g_map_geometry_selector + 0x4)) return 1;                     /* max < work-left  -> fully left  -> stc */
    if (ebp > (int32_t)G32(VA_g_map_geometry_selector + 0x8)) return 1;                     /* min > work-right -> fully right -> stc */
    return 0;                                                      /* clc -> terminal (extent overlaps the window) */
    #undef ES8
    #undef ES16
    #undef GS32
}

/* compute_sector_wall_depth_range (0x293ca) — leaf: scan a sector's walls (ES=g_surface_record_selector,
 * GS=g_vertex_selector) and return the MAX transformed-depth (gs:[vtxA+4]) over all walls' first vertex.
 * EAX=sector record off in. The original returns min in EAX (dropped by the caller) + MAX in EDX; the only
 * consumed output is EDX (caller does `cmp edx,0x4000`), so we return the max. Pure read (no obj3 writes). */
int32_t compute_sector_wall_depth_range(uint32_t sec, uint32_t gs_base, uint32_t es_base)
{
    #define ES8(o)  (*(volatile uint8_t  *)(uintptr_t)(es_base + (uint32_t)(o)))
    #define ES16(o) (*(volatile uint16_t *)(uintptr_t)(es_base + (uint32_t)(o)))
    #define GS32(o) ((int32_t)*(volatile int32_t *)(uintptr_t)(gs_base + (uint32_t)(o)))
    uint8_t  cl   = ES8(sec + 0xd);                 /* wall count */
    uint32_t wall = ES16(sec + 0xe);                /* wall array off */
    int32_t  maxd = (int32_t)0x80000000u;           /* edx = MAX (eax=MIN is dropped) */
    do {
        uint32_t diA = ES16(wall);
        int32_t  d   = GS32(diA + 4);               /* gs:[vtxA+4] = transformed depth */
        if (d >= maxd) maxd = d;                    /* cmp d,edx; jl skip; mov edx,d */
        wall += 0xc;
        cl = (uint8_t)(cl - 1);
    } while ((int8_t)cl > 0);
    return maxd;
    #undef ES8
    #undef ES16
    #undef GS32
}

/* compute_floor_clearance_for_render (0x29403) — portal-walks from g_player_sector (0x90c12) toward the
 * eye-straddling portal (same straddle test as walk mode-0), stopping when a sector's max wall depth
 * exceeds 0x4000 (compute_sector_wall_depth_range) or the chain ends. Then derives a floor-clearance
 * delta from the terminal sector's floor/ceil sub-record (+0x18) and writes g_render_floor_clearance
 * (0x8497c). All bp arithmetic is 16-bit (the final movsx bp -> ebp discards the high bits). Pure obj3
 * write (0x8497c). Gated by the caller: only invoked when [0x853da]!=0 && g_player_airborne==0. */
void compute_floor_clearance_for_render(uint32_t gs_base, uint32_t es_base)
{
    #define ES8(o)  (*(volatile uint8_t  *)(uintptr_t)(es_base + (uint32_t)(o)))
    #define ES16(o) (*(volatile uint16_t *)(uintptr_t)(es_base + (uint32_t)(o)))
    #define GS32(o) ((int32_t)*(volatile int32_t *)(uintptr_t)(gs_base + (uint32_t)(o)))
    uint32_t sec = (uint32_t)G16(VA_g_player_sector);          /* g_player_sector */
    if (sec == 0) return;                           /* 0x29410: sec==0 -> no write */
    int16_t bp = (int16_t)G16(VA_g_player_z);             /* clearance base */

    for (;;) {                                      /* 0x2941d portal-walk top (esi=sec) */
        if (compute_sector_wall_depth_range(sec, gs_base, es_base) > 0x4000)
            break;                                  /* 0x29424 cmp edx,0x4000; jg 0x29474 (terminal) */
        uint8_t  cl  = ES8(sec + 0xd);              /* wall count */
        uint32_t edi = ES16(sec + 0xe);             /* wall array off */
        int found = 0;
        for (;;) {                                  /* 0x29435 wall scan */
            int32_t ax = GS32(ES16(edi));           /* view-X of vtxA */
            int32_t bx = GS32(ES16(edi + 2));       /* view-X of vtxB */
            int straddle;
            if      (ax > bx)  straddle = 0;        /* 0x29448 jg -> advance */
            else if (ax == 0)  straddle = 1;        /* 0x2944a or eax,eax; je -> portal */
            else if (bx == 0)  straddle = 1;        /* 0x2944e or ebx,ebx; je -> portal */
            else               straddle = ((int32_t)(ax ^ bx) < 0);  /* 0x29452 xor; js */
            if (straddle) { found = 1; break; }
            edi += 0xc;
            cl = (uint8_t)(cl - 1);
            if ((int8_t)cl <= 0) break;             /* 0x2945b jg fails -> 0x2945d jmp terminal */
        }
        if (!found) break;                          /* no straddling wall -> terminal */
        uint32_t nb = ES16(edi + 8);                /* portal neighbor */
        if (nb == 0xffff) break;                    /* solid -> terminal */
        sec = (uint32_t)ES16(nb + 6);               /* cross: next = neighbor+6 */
    }

    /* 0x29474: floor-clearance delta from the terminal sector */
    uint32_t subrec = ES16(sec + 0x18);             /* floor/ceil sub-record off (0 = none) */
    int clamp_check = 0;
    if (subrec != 0) {
        uint16_t a8 = ES16(subrec + 8);
        int16_t  s2 = (int16_t)ES16(subrec + 2);
        if ((int16_t)a8 >= s2 && (int16_t)a8 <= (int16_t)G16(VA_g_sector_cull_coord)) {   /* jl/jg skip the sub */
            bp = (int16_t)((uint16_t)bp - a8);      /* 0x2949c sub bp,ax */
            clamp_check = 1;                        /* 0x2949f jmp 0x294a8 (skip the jns) */
        }
    }
    if (!clamp_check) {                             /* 0x294a1 */
        bp = (int16_t)((uint16_t)bp - ES16(sec + 2));   /* sub bp, es:[esi+2] */
        if (bp < 0) clamp_check = 1;                /* jns 0x294b0 (>=0 -> store); else fall to clamp */
    }
    if (clamp_check && bp <= -0x30)                 /* 0x294a8 cmp bp,-0x30; jg 0x294b0 (>-0x30 store) */
        bp = 0;                                     /* 0x294ae sub ebp,ebp */
    G32(VA_g_view_floor_clearance) = (int32_t)bp;                     /* movsx ebp,bp; mov [0x8497c],ebp */
    #undef ES8
    #undef ES16
    #undef GS32
}

/* walk_visible_sectors (0x294c0) — per-frame builder of the visible-extent list ([0x85220] count +
 * [0x85224] 6-byte-stride entries). NO register args (EAX is preserved-but-unused). Two bodies gated by
 * g_render_sector_walk_mode (0x853d3): mode 0 = first-person PORTAL WALK (the normal path), mode !=0 =
 * flat-array BBOX COLUMN CULL. Caller precondition: GS = g_vertex_selector (0x852cc, transformed view
 * coords); ES is loaded internally from g_surface_record_selector (0x852c8).
 *
 * Mode 0 follows one forward portal chain from g_player_sector toward the eye-straddling portal, calling
 * clip_sector_walls_to_view (0x2d793) per sector: pass-kind 2 in the loop (NATIVE, verified), pass-kind 1
 * for the final pass (BRIDGED inside clip via call_orig -> worklist arena, write-only). The
 * differential is obj3-only and SOUND: the native part writes only obj3, and the bridged final clip is
 * literally the original code (identical obj3 given identical restored input; its arena bump is outside
 * obj3 and never read back here).
 *
 * Mode 1 patches its own bbox-compare immediates from g_sector_cull_coord (0x852fa) via SMC; the lift
 * reads 0x852fa directly (NOT a hardcoded 0x1234) so it matches the patched original.
 *
 * ES OUTPUT (gotcha #9): walk does `mov es,[0x852c8]` and never restores ES (popal is GP-only / mode-1 has no
 * popal), so its postcondition is ES=g_surface_record_selector. The native C lift can't set the guest ES, so
 * the host's ABI_WALK dispatch writes ES=[0x852c8] into the trap context after this returns (lift_registry.c).
 * The portal-walk loop has no visited-set, matching the original (relies on acyclic geometry). */
void walk_visible_sectors(void)
{
    extern uint32_t (*g_os_sel_base)(uint16_t);
    uint16_t gs_sel = (uint16_t)G16(VA_g_vertex_selector);          /* caller-set GS = g_vertex_selector */
    uint16_t es_sel = (uint16_t)G16(VA_g_surface_record_selector);          /* ES = g_surface_record_selector (loaded internally) */
    #ifndef ROTH_STANDALONE
    if (g_os_sel_base == NULL) {                      /* selector hook required for gs:/es: access */
        regs_t io; memset(&io, 0, sizeof io);
        io.va = 0x294c0u + OBJ_DELTA;  /* [ORACLE-FALLBACK] */
        io.gs = gs_sel; io.es = es_sel; io.fs = (uint16_t)G16(VA_g_map_geometry_selector);
        call_orig(&io);
        return;
    }
    #endif
    uint32_t gs_base = g_os_sel_base(gs_sel);
    uint32_t es_base = g_os_sel_base(es_sel);
    #define ES8(o)  (*(volatile uint8_t  *)(uintptr_t)(es_base + (uint32_t)(o)))
    #define ES16(o) (*(volatile uint16_t *)(uintptr_t)(es_base + (uint32_t)(o)))
    #define GS32(o) ((int32_t)*(volatile int32_t *)(uintptr_t)(gs_base + (uint32_t)(o)))

    if (G8(VA_g_render_sector_walk_mode) == 0) {
        /* ---- Mode 0: portal walk (0x294cd) ---- */
        if (G8(VA_g_render_sector_walk_mode + 0x7) != 0 && G32(VA_g_player_airborne) == 0)     /* floor-clearance gate */
            compute_floor_clearance_for_render(gs_base, es_base);
        G32(VA_g_visible_extent_count) = 0;                              /* OUTPUT count = 0 */
        uint32_t sec = (uint32_t)G16(VA_g_player_sector);         /* g_player_sector */
        if (sec == 0) return;

        for (;;) {                                     /* 0x29508 portal-walk loop */
            G8(VA_g_render_sector_walk_mode + 0x6) = 0; G8(VA_g_render_sector_walk_mode + 0x5) = 2; G8(VA_g_render_sector_walk_mode + 0x4) = 3;
            G32(VA_g_view_clip_plane) = 0x1000;                     /* g_view_clip_plane */
            int32_t wl = (int32_t)(int16_t)G16(VA_g_view_bound_left);
            G32(VA_g_map_geometry_selector + 0x4) = wl; G16(VA_g_wall_proj_y3 + 0xa) = (uint16_t)wl;   /* work-left  = sext view_bound_left  */
            int32_t wr = (int32_t)(int16_t)G16(VA_g_view_bound_right);
            G32(VA_g_map_geometry_selector + 0x8) = wr; G16(VA_g_wall_proj_y3 + 0x12) = (uint16_t)wr;   /* work-right = sext view_bound_right */

            if (clip_sector_walls_to_view(sec, gs_sel, es_sel) == 0)
                break;                                 /* CF==0 -> final pass (terminal/visible leaf) */

            /* CF==1: scan walls for the eye-straddling portal */
            uint8_t  cl  = ES8(sec + 0xd);
            uint32_t edi = ES16(sec + 0xe);
            for (;;) {                                 /* 0x2955f wall scan */
                int32_t ax = GS32(ES16(edi));          /* view-X of vtxA */
                int32_t bx = GS32(ES16(edi + 2));      /* view-X of vtxB */
                int straddle;
                if      (ax > bx)  straddle = 0;
                else if (ax == 0)  straddle = 1;
                else if (bx == 0)  straddle = 1;
                else               straddle = ((int32_t)(ax ^ bx) < 0);
                if (straddle) {                        /* 0x29589 portal cross */
                    uint32_t nb = ES16(edi + 8);
                    if (nb == 0xffff) return;          /* solid -> done */
                    sec = (uint32_t)ES16(nb + 6);      /* next node = neighbor+6 */
                    break;                             /* -> outer loop (re-clip new sector) */
                }
                edi += 0xc;
                cl = (uint8_t)(cl - 1);
                if ((int8_t)cl <= 0) return;           /* no straddling wall -> done */
            }
        }
        /* 0x295a1 final pass: one clip in pass-kind 1 (CF ignored) */
        G8(VA_g_render_sector_walk_mode + 0x5) = 1; G8(VA_g_render_sector_walk_mode + 0x4) = 3; G32(VA_g_view_clip_plane) = 0x1000;
        { int32_t wl = (int32_t)(int16_t)G16(VA_g_view_bound_left);
          G32(VA_g_map_geometry_selector + 0x4) = wl; G16(VA_g_wall_proj_y3 + 0xa) = (uint16_t)wl;
          int32_t wr = (int32_t)(int16_t)G16(VA_g_view_bound_right);
          G32(VA_g_map_geometry_selector + 0x8) = wr; G16(VA_g_wall_proj_y3 + 0x12) = (uint16_t)wr; }
        clip_sector_walls_to_view(sec, gs_sel, es_sel);
        return;
    }

    /* ---- Mode 1: bbox column cull (0x295f0) ---- */
    G32(VA_g_map_geometry_selector + 0x4) = (int32_t)(uint16_t)G16(VA_g_view_bound_left);    /* work-left  = ZEXT view_bound_left  */
    G32(VA_g_map_geometry_selector + 0x8) = (int32_t)(uint16_t)G16(VA_g_view_bound_right);    /* work-right = ZEXT view_bound_right */
    int16_t cull = (int16_t)G16(VA_g_sector_cull_coord);              /* SMC source (read directly, not the patched code) */
    G32(VA_g_visible_extent_count) = 0;
    uint32_t sec = (uint32_t)ES16(4);                  /* first sector record off */
    int16_t  n   = (int16_t)ES16(sec - 2);             /* sector count */
    if (n == 0) return;

    for (;;) {                                         /* 0x2964c per-sector (stride 0x1a) */
        uint8_t wallcount = ES8(sec + 0xd);
        if ((int16_t)ES16(sec)     >= cull &&          /* bbox cull (jl skip) */
            (int16_t)ES16(sec + 2) <= cull) {          /* bbox cull (jg skip) */
            /* pass 1: accumulate view-X range [minX,maxX] + depth range [minD,maxD] over each wall's vtxA */
            int32_t maxX = (int32_t)0x80000000u, minX = 0x7fffffff;
            int32_t maxD = (int32_t)0x80000000u, minD = 0x7fffffff;
            { uint32_t w = ES16(sec + 0xe); uint8_t k = wallcount;
              do {
                  uint32_t di = ES16(w);
                  int32_t vx = GS32(di);
                  if (vx >= maxX) maxX = vx;
                  if (vx <= minX) minX = vx;
                  int32_t dp = GS32(di + 4);
                  if (dp >= maxD) maxD = dp;
                  if (dp <= minD) minD = dp;
                  w += 0xc; k = (uint8_t)(k - 1);
              } while ((int8_t)k > 0); }
            if (maxX >= (int32_t)0xffffec00 && minX <= 0x1400 &&   /* frustum gate */
                maxD >  0x1000          && minD <= 0x1000) {
                /* pass 2: clip each wall edge to depth 0x1000, accumulate clipped view-X [cMinX,cMaxX] */
                int32_t cMaxX = (int32_t)0x80000000u, cMinX = 0x7fffffff;
                { uint32_t w = ES16(sec + 0xe); uint8_t k = ES8(sec + 0xd);
                  do {
                      uint32_t diB = ES16(w + 2); int32_t depthB = GS32(diB + 4);
                      uint32_t diA = ES16(w);     int32_t depthA = GS32(diA + 4);
                      int32_t clippedX; int emit = 1;
                      if (depthA == 0x1000) {
                          clippedX = GS32(diA);                    /* vtxA on the near plane */
                      } else if (depthA < 0x1000) {                /* A in front of near */
                          if (depthB <= 0x1000) emit = 0;          /* both in front -> skip */
                          else {
                              int32_t den = depthB - depthA;
                              int32_t numf = 0x1000 - depthA;
                              int32_t dx = GS32(diB) - GS32(diA);  /* viewX_B - viewX_A */
                              clippedX = (int32_t)(((int64_t)numf * (int64_t)dx) / (int64_t)den) + GS32(diA);
                          }
                      } else {                                     /* A beyond near (depthA>0x1000) */
                          if (depthB >= 0x1000) emit = 0;          /* both beyond -> skip */
                          else {
                              int32_t den = depthA - depthB;
                              int32_t numf = 0x1000 - depthB;
                              int32_t dx = GS32(diA) - GS32(diB);  /* viewX_A - viewX_B */
                              clippedX = (int32_t)(((int64_t)numf * (int64_t)dx) / (int64_t)den) + GS32(diB);
                          }
                      }
                      if (emit) {
                          if (clippedX >= cMaxX) cMaxX = clippedX;
                          if (clippedX <= cMinX) cMinX = clippedX;
                      }
                      w += 0xc; k = (uint8_t)(k - 1);
                  } while ((int8_t)k > 0); }
                /* project + emit (0x29784): slot = 0x85224 + count*6 */
                uint32_t cnt = (uint32_t)G32(VA_g_visible_extent_count);
                volatile uint8_t *slot = (volatile uint8_t *)GADDR(VA_g_visible_extent_list) + (uintptr_t)cnt * 6u;
                *(volatile uint16_t *)(slot + 0) = (uint16_t)sec;          /* entry+0 = sector ptr */
                int32_t scale = (int32_t)G32(VA_g_view_params_block + 0xc), center = (int32_t)G32(VA_g_span_src_wrap_reoffset + 0x24);
                int32_t pr = (int32_t)(((int64_t)cMinX * (int64_t)scale) / (int64_t)0x1000) + center;
                if (pr < (int32_t)G32(VA_g_map_geometry_selector + 0x8)) {                          /* jge work-right -> skip emit */
                    if (pr < (int32_t)G32(VA_g_map_geometry_selector + 0x4)) pr = (int32_t)G32(VA_g_map_geometry_selector + 0x4);   /* clamp to work-left */
                    *(volatile uint16_t *)(slot + 2) = (uint16_t)pr;       /* entry+2 = screenLeft */
                    int32_t pl = (int32_t)(((int64_t)cMaxX * (int64_t)scale) / (int64_t)0x1000) + center;
                    if (pl > (int32_t)G32(VA_g_map_geometry_selector + 0x4)) {                      /* jle work-left -> skip emit */
                        if (pl > (int32_t)G32(VA_g_map_geometry_selector + 0x8)) pl = (int32_t)G32(VA_g_map_geometry_selector + 0x8);  /* clamp to work-right */
                        if ((uint16_t)pr != (uint16_t)pl) {                /* degenerate (L==R) -> skip emit */
                            *(volatile uint16_t *)(slot + 4) = (uint16_t)pl;   /* entry+4 = screenRight */
                            G32(VA_g_visible_extent_count) = (int32_t)(cnt + 1);             /* count++ */
                        }
                    }
                }
            }
        }
        sec += 0x1a;
        n = (int16_t)(n - 1);
        if (n <= 0) break;                             /* dec cx; jg */
    }
    #undef ES8
    #undef ES16
    #undef GS32
}

/* ---- build_sector_render_tree_recursive (0x29830) + its leaf record_portal_clip_entry (0x3cf98) ---- */

/* screen clamp [-0x3ffe, 0x3ffe] (0x29830's `cmp eax,0xffffc002; jl ...; cmp eax,0x3ffe; jl ...` ladder). */
static int32_t clamp_screen_3ffe(int32_t v)
{
    if (v < -0x3ffe) return -0x3ffe;       /* 0xffffc002 */
    if (v <  0x3ffe) return v;
    return 0x3ffe;
}
/* perspective project: signed 64-bit imul/idiv (the `imul <scale>; idiv <depth>; add <center>` blocks). */
static int32_t proj64(int32_t src, int32_t scale, int32_t depth, int32_t center)
{
    return (int32_t)(((int64_t)src * (int64_t)scale) / (int64_t)depth) + center;
}

/* record_portal_clip_entry (0x3cf98): append {neighbor di, sector si} to the obj3 arrays 0x8b3dc/0x8b3e8
 * (count 0x8b3d8, capped <6), UNLESS the record at es:[es:[si+6]+0x14] is >= 0xfffe (unsigned). Reads es:. */
void record_portal_clip_entry(uint32_t si, uint32_t di, uint32_t es_base)
{
    #define ES16(o) (*(volatile uint16_t *)(uintptr_t)(es_base + (uint32_t)(uint16_t)(o)))
    uint16_t bx = ES16(si + 6);
    if (ES16(bx + 0x14) >= 0xfffeu) return;                 /* cmp es:[bx+0x14],-2; jae (unsigned) */
    uint8_t count = G8(VA_g_snapshot_filename_buf + 0x68);
    if (count >= 6) return;
    *(volatile uint16_t *)GADDR((VA_g_snapshot_filename_buf + 0x6c) + (uint32_t)count * 2) = (uint16_t)di;
    *(volatile uint16_t *)GADDR((VA_g_snapshot_filename_buf + 0x78) + (uint32_t)count * 2) = (uint16_t)si;
    G8(VA_g_snapshot_filename_buf + 0x68) = (uint8_t)(count + 1);
    #undef ES16
}

/* build_sector_render_tree_recursive (0x29830) — recursive portal traversal that builds the per-frame
 * projected sector/wall NODE ARENA (the spine of draw-list assembly). For sector ESI it allocates a 10-byte
 * header node (cursor 0x852ee) + a run of 0x14-byte wall nodes (cursor 0x852ce), copies each wall's GS
 * view-space vertex coords, marks portals, perspective-projects the endpoints to screen X (near-plane clip
 * at depth 0x1000), then recurses through every visible portal within the running clip-X window.
 *
 * Selectors: ES = g_surface_record_selector (0x852c8, sector/wall records), FS = g_map_geometry_selector
 * (0x85294, the node arena — written), GS = vertex-table selector (0x852cc, transformed verts — read). The
 * outputs are the FS arena (OUTSIDE obj3) + a back-ref es:[esi+4] (ES block, outside obj3) + obj3 cursor
 * scratch (0x852xx). NOT double-run-safe alone (bump cursors are reset by the grand-caller render_world_scene,
 * not 0x29812) -> verify the {0x29812+0x29830} pair with the cursor reset prepended (ABI_SECTORTREE).
 *
 * Leaves: record_portal_clip_entry (0x3cf98, helper above) + find_record_by_id (0x3d018, lifted, returns CF).
 * No DAS/fb. ES OUTPUT (gotcha #9): 0x29830 reloads FS=[0x85294] and does NOT restore it; the entry wrapper
 * 0x29812 does `push fs … pop fs`, so for the PAIR there is no FS postcondition to thread. */
void build_sector_render_tree_recursive(uint32_t esi, uint32_t fs_base, uint32_t es_base, uint32_t gs_base)
{
    #define ES8(o)  (*(volatile uint8_t  *)(uintptr_t)(es_base + (uint32_t)(o)))
    #define ES16(o) (*(volatile uint16_t *)(uintptr_t)(es_base + (uint32_t)(o)))
    #define FS16(o) (*(volatile uint16_t *)(uintptr_t)(fs_base + (uint32_t)(o)))
    #define FS32(o) (*(volatile int32_t  *)(uintptr_t)(fs_base + (uint32_t)(o)))
    #define GS32(o) (*(volatile int32_t  *)(uintptr_t)(gs_base + (uint32_t)(o)))

    if ((uint16_t)G16(VA_g_vertex_selector + 0x24) >= 0xc65) return;             /* 0x29830 sector-count cap -> plain ret */
    G16(VA_g_vertex_selector + 0x20) = (uint16_t)(G16(VA_g_vertex_selector + 0x20) + 1);             /* depth++ */
    uint32_t hdr = (uint16_t)G16(VA_g_vertex_selector + 0x22);                   /* di = FS header cursor */
    uint16_t parent = (uint16_t)G16(VA_g_vertex_selector + 0x18);                /* ax = parent id */
    G16(VA_g_vertex_selector + 0x22) = (uint16_t)(G16(VA_g_vertex_selector + 0x22) + 0xa);           /* header cursor += 0xa */
    G16(VA_g_vertex_selector + 0x24) = (uint16_t)(G16(VA_g_vertex_selector + 0x24) + 1);             /* sector count++ */
    FS16(hdr + 8) = parent;
    FS16(hdr + 2) = (uint16_t)esi;
    FS16(hdr + 4) = (uint16_t)G16(VA_g_sector_cull_coord + 0x4);                  /* clip-X right mirror */
    FS16(hdr + 6) = (uint16_t)G16(VA_g_sector_cull_coord + 0x2);                  /* clip-X left (lo16 of packed window) */

    if ((uint16_t)ES16(esi + 4) != 0) {
        /* 0x29ccb already-visited relink: splice this header into the prior wall list */
        uint32_t rbx = (uint16_t)ES16(esi + 4);
        uint16_t ax = FS16(rbx - 2);
        FS16(rbx - 2) = (uint16_t)hdr;
        FS16(hdr) = ax;
    } else {
        /* ---- first visit: claim a wall-node block + record + project ---- */
        FS16(hdr) = 0;
        uint32_t ebx = (uint16_t)G16(VA_g_vertex_selector + 0x2);               /* FS wall-node byte cursor */
        if (ebx >= 0x7ce0) { G16(VA_g_vertex_selector + 0x20) = (uint16_t)(G16(VA_g_vertex_selector + 0x20) - 1); return; }  /* 0x29dc6 overflow bail */
        ebx += 8;
        ES16(esi + 4) = (uint16_t)ebx;                       /* es:[esi+4] = wall base (visited back-ref) */
        uint8_t wallcount = ES8(esi + 0xd);
        uint16_t v0 = (uint16_t)ES16(esi);
        uint16_t v1 = (uint16_t)ES16(esi + 2);
        uint16_t vz = (uint16_t)G16(VA_g_sector_cull_coord);
        G32(VA_g_vertex_selector + 0x10) = (int32_t)(int16_t)v0;                                 /* X-A */
        G32(VA_g_vertex_selector + 0x8) = -(int32_t)(int16_t)(uint16_t)(v0 - vz);               /* depth-delta A */
        G32(VA_g_vertex_selector + 0x14) = (int32_t)(int16_t)v1;                                 /* X-B */
        G32(VA_g_vertex_selector + 0xc) = -(int32_t)(int16_t)(uint16_t)(v1 - vz);               /* depth-delta B */
        uint32_t wall0 = ebx;
        FS16(wall0 - 8) = (uint16_t)esi;
        FS16(wall0 - 2) = (uint16_t)hdr;

        /* ---- wall-record loop (0x29903): one 0x14 node per wall ---- */
        uint32_t warr = (uint16_t)ES16(esi + 0xe);           /* wall-array walker */
        uint8_t cl = wallcount;
        do {
            FS16(ebx + 0x12) = 0;
            uint32_t di;
            uint16_t nb = (uint16_t)ES16(warr + 8);          /* neighbor sector (0xffff = solid) */
            if (nb == 0xffff) {
                di = 0xffff;
            } else {
                uint32_t nbr_rec = (uint16_t)ES16(nb + 6);
                di = nbr_rec;
                int do_vis = 0;
                if ((uint16_t)ES16(nbr_rec + 0x14) < 0xfffe) {              /* jb 0x29936 */
                    do_vis = 1;
                } else {
                    record_portal_clip_entry(warr, nbr_rec, es_base);
                    int cf = find_record_by_id((uint16_t)nbr_rec);   /* returns CF (1=not-found) */
                    if (!cf) do_vis = 1;                                    /* 0x2992c jae: CF=0 (found) -> vis test */
                    else if ((uint16_t)ES16(nbr_rec + 0x14) != 0xfffe) {    /* CF=1: 0x2992e jne -> sentinel */
                        di = 0xffff; FS16(ebx + 0x12) |= 0x20;
                    } else do_vis = 1;
                }
                if (do_vis) {                                              /* 0x29936 visibility test */
                    int16_t n0 = (int16_t)ES16(nbr_rec);
                    int16_t n2 = (int16_t)ES16(nbr_rec + 2);
                    int vis = (n0 > n2) && ((int32_t)n0 > G32(VA_g_vertex_selector + 0x14)) && ((int32_t)n2 < G32(VA_g_vertex_selector + 0x10));
                    if (!vis) { di = 0xffff; FS16(ebx + 0x12) |= 0x20; }
                }
            }
            FS16(ebx + 0x10) = (uint16_t)di;
            uint16_t vtx = (uint16_t)ES16(warr);
            FS32(ebx + 0xc) = GS32(vtx + 4);                 /* depth from GS */
            FS32(ebx + 8) = GS32(vtx);                       /* view-X from GS */
            ebx += 0x14;
            warr += 0xc;
            cl = (uint8_t)(cl - 1);
        } while ((int8_t)cl > 0);

        /* ---- close the ring (copy wall0's coords into the synthetic closing node) ---- */
        uint32_t loop_end = ebx;
        ebx = wall0;
        FS32(loop_end + 0xc) = FS32(wall0 + 0xc);
        FS32(loop_end + 8) = FS32(wall0 + 8);
        uint32_t cursor_end = loop_end + 0x14;
        FS16(wall0 - 6) = (uint16_t)cursor_end;
        FS16(wall0 - 4) = 0;
        G16(VA_g_vertex_selector + 0x2) = (uint16_t)cursor_end;

        /* ---- per-wall PROJECTION loop (0x299c1): X + Y to screen, near-plane clip at depth 0x1000 ---- */
        int32_t Xcenter = G32(VA_g_span_src_wrap_reoffset + 0x24);                      /* edi */
        int32_t Xscale  = G32(VA_g_view_params_block + 0xc);                      /* ebp */
        int32_t Pscale  = G32(VA_g_perspective_scale);                      /* g_perspective_scale (Y) */
        int32_t Ycenter = G32(VA_g_span_src_wrap_reoffset + 0x28);
        uint32_t pbx = wall0;
        uint8_t pcl = wallcount;
        uint8_t pch = 0;
        do {
            int32_t depth = FS32(pbx + 0xc);
            int both_behind = 0;
            if (depth < 0x1000) {                            /* 0x29b3b: this wall behind near plane */
                if (FS32(pbx + 0x20) < 0x1000) {             /* next also behind -> 0x29cc0 sentinel */
                    FS16(pbx) = 0x8000;
                    both_behind = 1;
                } else {                                     /* 0x29b49: interp X at near plane */
                    int32_t num = 0x1000 - depth;
                    int32_t dx  = FS32(pbx + 0x1c) - FS32(pbx + 8);
                    int32_t den = FS32(pbx + 0x20) - depth;
                    int32_t vx  = (int32_t)(((int64_t)num * (int64_t)dx) / (int64_t)den) + FS32(pbx + 8);
                    FS16(pbx)     = (uint16_t)clamp_screen_3ffe(proj64(vx, Xscale, 0x1000, Xcenter));
                    FS16(pbx + 4) = (uint16_t)clamp_screen_3ffe(proj64(G32(VA_g_vertex_selector + 0x8), Pscale, 0x1000, Ycenter));
                    FS16(pbx + 6) = (uint16_t)clamp_screen_3ffe(proj64(G32(VA_g_vertex_selector + 0xc), Pscale, 0x1000, Ycenter));
                    FS16(pbx + 0x12) |= 4;
                }
            } else {                                         /* depth >= 0x1000: in front */
                if (pch != 0 && FS32(pbx - 8) >= 0x1000)      /* prev wall in front -> share edge */
                    FS16(pbx) = FS16(pbx - 0x12);
                else                                         /* 0x299e8 normal X */
                    FS16(pbx) = (uint16_t)clamp_screen_3ffe(proj64(FS32(pbx + 8), Xscale, depth, Xcenter));
                FS16(pbx + 4) = (uint16_t)clamp_screen_3ffe(proj64(G32(VA_g_vertex_selector + 0x8), Pscale, depth, Ycenter));  /* 0x29a12 */
                FS16(pbx + 6) = (uint16_t)clamp_screen_3ffe(proj64(G32(VA_g_vertex_selector + 0xc), Pscale, depth, Ycenter));
            }
            if (!both_behind) {                              /* 0x29a7a: next-edge (+2) projection */
                if (FS32(pbx + 0x20) < 0x1000) {             /* next behind near -> 0x29c04 interp */
                    int32_t num = 0x1000 - FS32(pbx + 0x20);
                    int32_t dx  = FS32(pbx + 8) - FS32(pbx + 0x1c);
                    int32_t den = FS32(pbx + 0xc) - FS32(pbx + 0x20);
                    int32_t vx  = (int32_t)(((int64_t)num * (int64_t)dx) / (int64_t)den) + FS32(pbx + 0x1c);
                    FS16(pbx + 2)    = (uint16_t)clamp_screen_3ffe(proj64(vx, Xscale, 0x1000, Xcenter));
                    FS16(pbx + 0x18) = (uint16_t)clamp_screen_3ffe(proj64(G32(VA_g_vertex_selector + 0x8), Pscale, 0x1000, Ycenter));
                    FS16(pbx + 0x1a) = (uint16_t)clamp_screen_3ffe(proj64(G32(VA_g_vertex_selector + 0xc), Pscale, 0x1000, Ycenter));
                    FS16(pbx + 0x12) |= 8;
                } else {                                     /* 0x29a88 normal +2 */
                    FS16(pbx + 2) = (uint16_t)clamp_screen_3ffe(proj64(FS32(pbx + 0x1c), Xscale, FS32(pbx + 0x20), Xcenter));
                }
            }
            pbx += 0x14;
            pch = (uint8_t)(pch + 1);
            pcl = (uint8_t)(pcl - 1);
        } while ((int8_t)pcl > 0);

        /* 0x29ac0: closing node Y projection (depth clamped to >= 0x1000) */
        int32_t cd = FS32(pbx + 0xc);
        if (cd <= 0x1000) cd = 0x1000;
        FS16(pbx + 4) = (uint16_t)clamp_screen_3ffe(proj64(G32(VA_g_vertex_selector + 0x8), Pscale, cd, Ycenter));
        FS16(pbx + 6) = (uint16_t)clamp_screen_3ffe(proj64(G32(VA_g_vertex_selector + 0xc), Pscale, cd, Ycenter));
    }

    /* ---- recursion loop (0x29ced): recurse through visible portals within the clip-X window ---- */
    uint32_t rbx = (uint16_t)ES16(esi + 4);
    uint8_t rcl = ES8(esi + 0xd);
    do {
        uint16_t axw = FS16(rbx);
        if (axw == 0x8000) goto rcont;                       /* sentinel */
        int16_t ax = (int16_t)axw;
        int16_t dx = (int16_t)FS16(rbx + 2);
        if (ax >= dx) goto rcont;
        if (ax >= (int16_t)G16(VA_g_sector_cull_coord + 0x2)) goto rcont;         /* >= clip-X left */
        if (dx <= (int16_t)G16(VA_g_sector_cull_coord + 0x4)) goto rcont;         /* <= clip-X right */
        FS16(rbx + 0x12) |= 1;
        if ((uint16_t)FS16(rbx + 0x10) == 0xffff) goto rcont;
        FS16(rbx + 0x12) |= 0x22;
        if ((uint16_t)G16(VA_g_vertex_selector + 0x20) > 0x3c) goto rcont;       /* ja recursion-depth cap (60) */
        {
            int32_t saved_fc = G32(VA_g_sector_cull_coord + 0x2);                 /* push clip-X window */
            if (ax >= (int16_t)G16(VA_g_sector_cull_coord + 0x4)) G16(VA_g_sector_cull_coord + 0x4) = (uint16_t)ax;   /* narrow right */
            if (dx <= (int16_t)G16(VA_g_sector_cull_coord + 0x2)) G16(VA_g_sector_cull_coord + 0x2) = (uint16_t)dx;   /* narrow left */
            if ((int16_t)G16(VA_g_sector_cull_coord + 0x4) < (int16_t)G16(VA_g_sector_cull_coord + 0x2)) {            /* window non-degenerate */
                uint16_t cx = (uint16_t)FS16(rbx + 0x10);    /* neighbor id */
                if (cx != (uint16_t)G16(VA_g_vertex_selector + 0x18) && cx != (uint16_t)esi) {  /* not parent, not self */
                    uint16_t saved_e4 = (uint16_t)G16(VA_g_vertex_selector + 0x18);
                    G16(VA_g_vertex_selector + 0x18) = (uint16_t)esi;
                    build_sector_render_tree_recursive(cx, fs_base, es_base, gs_base);  /* RECURSE */
                    G16(VA_g_vertex_selector + 0x18) = saved_e4;
                }
            }
            G32(VA_g_sector_cull_coord + 0x2) = saved_fc;                         /* pop clip-X window */
        }
    rcont:
        rbx += 0x14;
        rcl = (uint8_t)(rcl - 1);
    } while ((int8_t)rcl > 0);

    G16(VA_g_vertex_selector + 0x20) = (uint16_t)(G16(VA_g_vertex_selector + 0x20) - 1);             /* 0x29dbe depth-- */
    #undef ES8
    #undef ES16
    #undef FS16
    #undef FS32
    #undef GS32
}

/* begin_sector_render_tree (0x29812) — the entry wrapper: clears the portal-clip-entry count then calls
 * build_sector_render_tree_recursive. `push fs … pop fs` preserves the caller's FS (so the pair has no FS
 * postcondition). ESI = root sector. Resolves the three selector bases here. */
void begin_sector_render_tree(uint32_t esi)
{
    extern uint32_t (*g_os_sel_base)(uint16_t);
    G8(VA_g_snapshot_filename_buf + 0x68) = 0;                                          /* mov byte[0x8b3d8],0 */
    #ifndef ROTH_STANDALONE
    if (g_os_sel_base == NULL) {                            /* no sel hook -> bridge */
        regs_t io; memset(&io, 0, sizeof io);
        io.va = 0x29812u + OBJ_DELTA; io.esi = esi;  /* [ORACLE-FALLBACK] */
        io.es = (uint16_t)G16(VA_g_surface_record_selector); io.gs = (uint16_t)G16(VA_g_vertex_selector); io.fs = (uint16_t)G16(VA_g_map_geometry_selector);
        call_orig(&io);
        return;
    }
    #endif
    uint32_t fs_base = g_os_sel_base((uint16_t)G16(VA_g_map_geometry_selector));
    uint32_t es_base = g_os_sel_base((uint16_t)G16(VA_g_surface_record_selector));
    uint32_t gs_base = g_os_sel_base((uint16_t)G16(VA_g_vertex_selector));
    build_sector_render_tree_recursive(esi, fs_base, es_base, gs_base);
}

/* ---------------------------------------------------------------------------------------------
 * build_sector_draw_order (0x2a6d0) — the draw-list consumer of the 0x29830 sector render tree.
 *
 * Operates on the FS node arena (sel g_map_geometry_selector 0x85294) + ES back-ref block (sel
 * g_surface_record_selector 0x852c8), plus obj3 scratch. No GP-register inputs. Two phases:
 *   A. Recursive occlusion DFS (mark_sector_draw_order 0x2a7af) over every node in the [2, bp)
 *      chain: visit unmarked portal-adjacent neighbours first (painter's depth order), mark the
 *      node ordered (+0x1a |= 0x10), then APPEND its offset to the ordered list at fs:[0xfc02]
 *      (cursor pre-seeded to 0xfc02). Node count -> fs:[0xfc00]. Recursion depth in word[0x85300],
 *      capped at 0x32.
 *   B. Per-node X-interval coalescing: for each node, walk its interval sub-list (head fs:[node+6],
 *      +0 next link), merge overlapping [xmin@+4, xmax@+6] spans into the head and mark each merged
 *      node removed (+2 = 0xffff). This is the loop whose 0x2a747 PC spun on the cyclic list built
 *      from walk's dropped-ES bug (gotcha #9) — now traverses an acyclic list.
 * Output count word[0x85302] (= word[0x852f0]) feeds render_world_face_list_subpass 0x28dbe.
 * ------------------------------------------------------------------------------------------- */

/* mark_sector_draw_order (0x2a7af): the recursive DFS. `ebx` = node offset; `*edip` = shared output
 * cursor (NOT saved across recursion — accumulates). Returns the eax the original leaves live in eax
 * (reproduced faithfully: it is set to the last neighbour id touched, clobbered by recursion). */
uint32_t mark_sector_draw_order(uint32_t ebx, uint32_t fs_base,
                                       uint32_t es_base, uint32_t *edip)
{
    #define FS16(o) (*(volatile uint16_t *)(uintptr_t)(fs_base + (uint32_t)(o)))
    #define ES16(o) (*(volatile uint16_t *)(uintptr_t)(es_base + (uint32_t)(o)))
    G16(VA_g_sector_cull_coord + 0x6) = (uint16_t)(G16(VA_g_sector_cull_coord + 0x6) + 1);            /* inc word[0x85300] (depth) */
    uint32_t eax  = FS16(ebx + 0);                          /* ax  = self id */
    uint32_t esi  = FS16(ebx + 6);                          /* esi = adjacency-list head */
    uint32_t node = ebx;                                    /* push ebx */
    while (1) {
        if (FS16(esi + 8) != 0) {                           /* cmp fs:[esi+8],0 ; je skip */
            uint32_t bx = FS16(esi + 8);                    /* bx = neighbour id (ebx = bx) */
            if (bx != (eax & 0xffff)) {                     /* cmp bx,ax ; je skip */
                eax = bx;                                   /* mov eax, ebx */
                uint32_t rec = ES16(bx + 4);                /* mov bx, es:[ebx+4] (ebx = rec) */
                if (!(FS16(rec + 0x12) & 0x10)) {           /* test fs:[ebx+0x12],0x10 ; jne skip */
                    uint32_t child = rec - 8;               /* sub ebx,8 */
                    if ((int16_t)G16(VA_g_sector_cull_coord + 0x6) < 0x32)       /* cmp word[0x85300],0x32 ; jge skip */
                        eax = mark_sector_draw_order(child, fs_base, es_base, edip); /* recurse */
                }
            }
        }
        esi = FS16(esi + 0);                                /* mov si, fs:[esi] */
        if (esi == 0) break;                                /* test esi,esi ; jne loop */
    }
    FS16(node + 0x1a) |= 0x10;                               /* pop ebx ; or fs:[ebx+0x1a],0x10 */
    FS16(*edip) = (uint16_t)node;                            /* mov fs:[edi], bx */
    *edip += 2;                                              /* add edi,2 */
    G16(VA_g_sector_cull_coord + 0x6) = (uint16_t)(G16(VA_g_sector_cull_coord + 0x6) - 1);            /* dec word[0x85300] */
    #undef FS16
    #undef ES16
    return eax;
}

void build_sector_draw_order(uint32_t esi_unused)
{
    (void)esi_unused;                                       /* no GP-register inputs */
    extern uint32_t (*g_os_sel_base)(uint16_t);
    G16(VA_g_sector_cull_coord + 0x6) = 0;                                       /* mov word[0x85300],0 */
    uint32_t cx = G16(VA_g_vertex_selector + 0x24);                             /* cx = word[0x852f0] (node count) */
    G16(VA_g_sector_cull_coord + 0x8) = (uint16_t)cx;                            /* mov word[0x85302],cx */
    if ((uint16_t)cx == 0) return;                          /* test cx,cx ; je ret */

    #ifndef ROTH_STANDALONE
    if (g_os_sel_base == NULL) {                          /* no sel hook -> bridge */
        regs_t io; memset(&io, 0, sizeof io);
        io.va = 0x2a6d0u + OBJ_DELTA;  /* [ORACLE-FALLBACK] */
        io.es = (uint16_t)G16(VA_g_surface_record_selector); io.fs = (uint16_t)G16(VA_g_map_geometry_selector);
        call_orig(&io);
        return;
    }
    #endif
    uint32_t fs_base = g_os_sel_base((uint16_t)G16(VA_g_map_geometry_selector));
    uint32_t es_base = g_os_sel_base((uint16_t)G16(VA_g_surface_record_selector));
    #define FS16(o)  (*(volatile uint16_t *)(uintptr_t)(fs_base + (uint32_t)(o)))
    #define FS16S(o) (*(volatile int16_t  *)(uintptr_t)(fs_base + (uint32_t)(o)))

    uint32_t bp  = G16(VA_g_vertex_selector + 0x2);                            /* bp  = node-limit */
    uint32_t edi = 0xfc02;                                  /* di  = 0xfc02 (ordered-list cursor) */
    uint32_t ebx = 2;                                       /* bl  = 2 (first node) */
    uint32_t ecx = 0;                                       /* ecx = 0 */

    /* ---- Phase A: DFS depth-order every node in [2, bp) ---- */
    do {
        if (!(FS16(ebx + 0x1a) & 0x10))                     /* test fs:[ebx+0x1a],0x10 ; jne skip */
            mark_sector_draw_order(ebx, fs_base, es_base, &edi);
        ecx++;                                              /* inc ecx */
        ebx = FS16(ebx + 2);                                /* mov bx, fs:[ebx+2] */
    } while ((uint16_t)ebx < (uint16_t)bp);                 /* cmp bx,bp ; jb loop */
    FS16(0xfc00) = (uint16_t)ecx;                           /* mov fs:[0xfc00], cx */

    /* ---- Phase B: per-node X-interval coalescing ---- */
    ebx = 2;                                                /* mov ebx,2 */
    do {
        uint32_t esi = FS16(ebx + 6);                       /* mov si, fs:[ebx+6] */
      recheck:
        if (FS16(esi + 0) == 0) goto next_node;             /* cmp fs:[esi],0 ; je next */
        {
            uint32_t edx_edi = esi;                         /* mov edi, esi */
          inner:
            edx_edi = FS16(edx_edi + 0);                    /* mov di, fs:[edi] */
            if (edx_edi == 0) {                             /* test edi,edi ; je advance */
                esi = FS16(esi + 0);                        /* mov si, fs:[esi] (je 0x2a73e, always) */
                goto recheck;
            }
            if (FS16(edx_edi + 2) == 0xffff) goto inner;    /* cmp fs:[edi+2],-1 ; je inner (removed) */
            int16_t ax = FS16S(esi + 4);                    /* ax = head.xmin */
            int16_t dx = FS16S(esi + 6);                    /* dx = head.xmax */
            if (ax > FS16S(edx_edi + 6)) goto inner;        /* cmp ax,fs:[edi+6] ; jg inner (no overlap) */
            if (dx < FS16S(edx_edi + 4)) goto inner;        /* cmp dx,fs:[edi+4] ; jl inner (no overlap) */
            if (!(ax < FS16S(edx_edi + 4)))                 /* cmp ax,fs:[edi+4] ; jl keep */
                ax = FS16S(edx_edi + 4);                    /*   else ax = min -> edi.xmin */
            if (!(dx > FS16S(edx_edi + 6)))                 /* cmp dx,fs:[edi+6] ; jg keep */
                dx = FS16S(edx_edi + 6);                    /*   else dx = max -> edi.xmax */
            FS16(esi + 4) = (uint16_t)ax;                   /* mov fs:[esi+4], ax */
            FS16(esi + 6) = (uint16_t)dx;                   /* mov fs:[esi+6], dx */
            FS16(edx_edi + 2) = 0xffff;                     /* mov fs:[edi+2], -1 (mark removed) */
            edx_edi = esi;                                  /* mov edi, esi ; jmp 0x2a747 */
            goto inner;
        }
      next_node:
        ebx = FS16(ebx + 2);                                /* mov bx, fs:[ebx+2] */
    } while ((int16_t)(--ecx) > 0);                         /* dec cx ; jg loop */
    #undef FS16
    #undef FS16S
}

/* ---------------------------------------------------------------------------------------------
 * The build_scene_draw_list (0x2a0a0) cluster — bottom-up. FOUNDATION leaf first:
 *
 * finalize_draw_list_entry (0x2a4d0): projects ONE face's screen extents into the draw-list entry
 * (`eax` = a g_door_worklist 0x8498c cursor, stride 0x26), via emit_world_face_spans (0x2c720, the
 * texture/block resolver) + a clip pass-0 (0x2d757 -> clip_sector_walls_to_view 0x2d793). esi = the
 * face/surface record (flat ptr into geometry section E). ebp = caller's stack frame (scratch fields
 * +0x14/+0x18/+0x1c, shared with 0x2c720 + the clip pass-0). Void return (callers reload from frame).
 * All obj3 globals via G-macros (OBJ_DELTA alias); the flat face/entry/frame pointers deref'd direct
 * at canon (same mechanism as the block pointers in emit_world_face_spans). ------------------ */

/* 0x2d757: stash the frame ptr, copy two screen-Y globals, set clip pass-kind 0, run the clip. The clip
 * lift bridges pass-kind != 2 to call_orig (worklist arena), so this delegates the pass-0 insert. */
void clip_and_emit_floor_walls(uint32_t ebp, uint16_t gs_sel, uint16_t es_sel)  /* 0x2d757 clip_and_emit_floor_walls */
{
    G32(VA_g_reflection_view_list + 0x80) = ebp;                                                  /* mov [0x853c4], ebp */
    /* mov gs,[0x852cc] — selector load (no-op in the lift) */
    uint32_t esi = *(volatile uint32_t *)(uintptr_t)(ebp + 0x14);        /* esi = frame[0x14] */
    G16(VA_g_view_bound_left) = G16(VA_g_wall_proj_y3 + 0xa);                                         /* [0x9096a] = [0x90958] */
    G16(VA_g_view_bound_right) = G16(VA_g_wall_proj_y3 + 0x12);                                         /* [0x90968] = [0x90960] */
    G8(VA_g_render_sector_walk_mode + 0x4) = 3;
    G8(VA_g_render_sector_walk_mode + 0x5) = 0;                                                     /* pass-kind 0 */
    clip_sector_walls_to_view(esi, gs_sel, es_sel);              /* -> bridges (pass-kind 0) */
}

void finalize_draw_list_entry(uint32_t eax, uint32_t esi, uint32_t ebx, uint32_t ecx,
                                     uint32_t edx, uint32_t edi, uint32_t ebp,
                                     uint16_t es, uint16_t fs, uint16_t gs)
{
    #define FACE8(o)  (*(volatile uint8_t  *)(uintptr_t)(esi + (uint32_t)(o)))
    #define FACE16(o) (*(volatile uint16_t *)(uintptr_t)(esi + (uint32_t)(o)))
    #define FACE32(o) (*(volatile uint32_t *)(uintptr_t)(esi + (uint32_t)(o)))
    #define E16(o)    (*(volatile uint16_t *)(uintptr_t)(entry + (uint32_t)(o)))
    #define E32(o)    (*(volatile uint32_t *)(uintptr_t)(entry + (uint32_t)(o)))

    *(volatile uint32_t *)(uintptr_t)(ebp + 0x14) = ebx;                 /* frame[0x14] = ebx */
    G16(VA_g_anim_clock + 0xa) = (uint16_t)(-(uint16_t)(FACE16(0xa) - G16(VA_g_sector_cull_coord)));  /* [0x85334] = -(esi.a - [0x852fa]) */
    *(volatile uint32_t *)(uintptr_t)(ebp + 0x18) = eax;                 /* frame[0x18] = eax (entry ptr) */
    uint32_t entry = eax;
    G16(VA_g_world_surface_draw_flags) = 0;
    G32(VA_g_current_proc_tag + 0x11c) = 0;
    G32(VA_g_map_das_fat_buffer + 0xc) = esi;                                                  /* current face record */
    if (FACE8(7) & 0x10) G16(VA_g_world_surface_draw_flags) = 2;                              /* flip flag */
    G32(VA_g_current_proc_tag + 0x118) = FACE32(4);

    /* emit_world_face_spans(0x2c720): eax = esi.field_4 (descriptor); edx low16 = the [0x85334] sign value the
     * original carries in dx at the call. RETURNS the resolved texture/block record in ESI + CF (load fail).
     * The projection below reads texture W/H from the RESOLVED record, NOT the face record — so rebind esi. */
    uint32_t out_eax = 0, out_cf = 0;
    uint32_t edx_call = (edx & 0xffff0000u) | (uint32_t)G16(VA_g_anim_clock + 0xa);
    esi = emit_world_face_spans(FACE32(4), esi, ebx, ecx, edx_call, edi, ebp, es, fs, gs, &out_eax, &out_cf);
    if (out_cf) return;                                                 /* jb 0x2a6c9 */

    entry = *(volatile uint32_t *)(uintptr_t)(ebp + 0x18);              /* ebx = frame[0x18] */
    E16(0x14) = G16(VA_g_world_surface_draw_flags);
    E16(0x10) = G16(VA_g_current_das_entry_id);
    E32(0xc)  = G32(VA_g_current_proc_tag + 0x4);
    G16(VA_g_wall_proj_y3 + 0x8) = (uint16_t)(FACE16(0xe) << 1);

    /* texel-step shift count cl + [0x90980] scale (0x2a555..0x2a592) */
    uint16_t w980 = (uint16_t)(FACE16(0xc) << 1);
    uint8_t  cl   = 7;
    uint16_t fa   = FACE16(0xa);
    if (fa & 0x80) {
        if (fa & 0x6000) {                                              /* 0x2a56d rol/and/xchg */
            uint8_t t = (uint8_t)(((uint16_t)((fa << 3) | (fa >> 13))) & 3);
            w980 = (uint16_t)(w980 << t);
            cl   = (uint8_t)(t + 7);
        } else {                                                        /* 0x2a581 dec/shr */
            cl   = 6;
            w980 = (uint16_t)(w980 >> 1);
        }
    }
    G16(VA_g_span_src_wrap_reoffset + 0x4) = w980;

    int32_t a = (int32_t)((uint32_t)G16(VA_g_wall_proj_y3 + 0x8) << cl);                /* shl eax, cl */
    G32(VA_g_current_proc_tag + 0x124) = (uint32_t)a;
    a += (int32_t)G32(VA_g_visible_extent_list + 0x3c);
    int32_t proj1 = proj64(a, (int32_t)G32(VA_g_view_params_block + 0xc), (int32_t)G32(VA_g_view_clip_plane), (int32_t)G32(VA_g_span_src_wrap_reoffset + 0x24));
    E16(0x1e) = (uint16_t)proj1;
    if ((int16_t)(uint16_t)proj1 <= (int16_t)G16(VA_g_sector_cull_coord + 0xe)) return;      /* jle 0x2a6c9 */
    G16(VA_g_wall_proj_y3 + 0x12) = (uint16_t)proj1;
    E32(0) = G32(VA_g_visible_extent_list + 0x3c);
    int32_t a2 = (int32_t)G32(VA_g_visible_extent_list + 0x3c) - (int32_t)G32(VA_g_current_proc_tag + 0x124);
    int32_t proj2 = proj64(a2, (int32_t)G32(VA_g_view_params_block + 0xc), (int32_t)G32(VA_g_view_clip_plane), (int32_t)G32(VA_g_span_src_wrap_reoffset + 0x24));
    E16(0x16) = (uint16_t)proj2;
    if ((int16_t)(uint16_t)proj2 >= (int16_t)G16(VA_g_sector_cull_coord + 0x12)) return;      /* jge 0x2a6c9 */
    G16(VA_g_wall_proj_y3 + 0xa) = (uint16_t)proj2;

    /* horizontal texel base (0x2a5fe..0x2a63d) */
    int32_t ecxv = (int32_t)(uint32_t)G16(VA_g_span_src_wrap_reoffset + 0x4);
    int32_t eaxv = (int32_t)(int16_t)G16(VA_g_anim_clock + 0xa);
    uint8_t dl   = G8(VA_g_secondary_surface_count + 0x2);
    int32_t edxv = (int32_t)(dl & 0xf);
    edxv += edxv;
    edxv = (int32_t)(int16_t)(uint16_t)((uint16_t)edxv + G16(VA_g_current_proc_tag + 0x11e));
    if (dl & 0x10) eaxv = eaxv - edxv + ecxv;                          /* 0x2a628 */
    else           eaxv = eaxv + edxv;                                  /* 0x2a63d */

    int32_t saved = eaxv;                                               /* push eax */
    int32_t t24 = eaxv - ecxv;
    E16(0x24) = (uint16_t)t24;
    int32_t p3 = clamp_screen_3ffe(proj64(t24,  (int32_t)G32(VA_g_perspective_scale), (int32_t)G32(VA_g_view_clip_plane), (int32_t)G32(VA_g_span_src_wrap_reoffset + 0x28)));
    E16(0x20) = (uint16_t)p3;
    E16(0x18) = (uint16_t)p3;
    int32_t p4 = clamp_screen_3ffe(proj64(saved,(int32_t)G32(VA_g_perspective_scale), (int32_t)G32(VA_g_view_clip_plane), (int32_t)G32(VA_g_span_src_wrap_reoffset + 0x28)));
    E16(0x22) = (uint16_t)p4;
    E16(0x1a) = (uint16_t)p4;
    E32(0) = G32(VA_g_visible_extent_list + 0x3c);
    E16(0x1c) = G16(VA_g_view_clip_plane + 0x1);                                          /* unaligned: hi word of dword[0x85264] */
    *(volatile uint32_t *)(uintptr_t)(ebp + 0x1c) = entry;             /* frame[0x1c] = ebx */
    clip_and_emit_floor_walls(ebp, gs, es);                           /* call 0x2d757 -> clip pass-0 */
    #undef FACE8
    #undef FACE16
    #undef FACE32
    #undef E16
    #undef E32
}

/* emit_object_draw_entries (0x29dcf): walk a linked list of placed-object/sprite nodes (head = eax; node
 * layout: +0 next, +4 back-ref id [read via es:], +8 appearance sub-record). For each VISIBLE node (back-ref
 * present, es:[id+4]!=0, appearance flag [+7]&1 clear), project its anchor via rotate_point_2d and — if the
 * depth clears the near plane (>= 0x1000) — append a type-1 draw-list entry at the shared cursor [ebp+8] and
 * finalize it (0x2a4d0). ecx = passthrough to 0x2c720. es = g_surface_record_selector. ebp = caller frame
 * ([ebp+4] = the rotate sign value the caller seeded; [ebp],[ebp+2] = projection scratch). Self-jump LOOP
 * (push/pop eax balanced), not recursion. Void. */
void emit_object_draw_entries(uint32_t eax, uint32_t ecx, uint32_t ebp,
                                     uint16_t es, uint16_t fs, uint16_t gs)
{
    extern uint32_t (*g_os_sel_base)(uint16_t);
    #ifndef ROTH_STANDALONE
    if (g_os_sel_base == NULL) {                       /* no sel hook -> bridge the whole walk */
        regs_t io; memset(&io, 0, sizeof io);
        io.va = 0x29dcfu + OBJ_DELTA; io.eax = eax; io.ecx = ecx; io.ebp = ebp;  /* [ORACLE-FALLBACK] */
        io.es = es; io.fs = fs; io.gs = gs;
        call_orig(&io);
        return;
    }
    #endif
    uint32_t es_base = g_os_sel_base(es);
    #define ES16(o) (*(volatile uint16_t *)(uintptr_t)(es_base + (uint32_t)(o)))
    uint32_t node = eax;
    do {
        uint32_t ebx = *(volatile uint32_t *)(uintptr_t)(node + 4);     /* node[+4] = back-ref id */
        if (ebx != 0 && ES16(ebx + 4) != 0) {                          /* es:[id+4] != 0 */
            uint32_t esi = node + 8;                                    /* appearance sub-record */
            if (!(*(volatile uint8_t *)(uintptr_t)(esi + 7) & 1)) {     /* [esi+7]&1 == 0 (visible) */
                /* seed the rotate scratch [ebp],[ebp+2] then project */
                *(volatile uint16_t *)(uintptr_t)(ebp) =
                    (uint16_t)(*(volatile uint16_t *)(uintptr_t)(esi)     + G16(VA_g_view_offset_x));
                *(volatile uint16_t *)(uintptr_t)(ebp + 2) =
                    (uint16_t)(*(volatile uint16_t *)(uintptr_t)(esi + 2) + G16(VA_g_view_offset_y));
                int32_t depth = 0, edxv = 0;
                rotate_point_2d((const int16_t *)(uintptr_t)ebp, &depth, &edxv);  /* eax=depth, edx */
                if (depth >= 0x1000) {                                  /* near-plane gate */
                    uint32_t edi   = *(volatile uint32_t *)(uintptr_t)(ebp + 8);  /* cursor */
                    uint32_t entry = edi;
                    *(volatile uint32_t *)(uintptr_t)(entry + 4) = (uint32_t)depth;
                    G32(VA_g_view_clip_plane) = (uint32_t)depth;
                    *(volatile uint32_t *)(uintptr_t)(entry + 0) = (uint32_t)edxv;
                    G32(VA_g_visible_extent_list + 0x3c) = (uint32_t)edxv;
                    *(volatile uint32_t *)(uintptr_t)(entry + 8) = esi;
                    *(volatile uint16_t *)(uintptr_t)(entry + 0x12) = 1;          /* type 1 */
                    edi += 0x26;
                    *(volatile uint32_t *)(uintptr_t)(ebp + 8) = edi;            /* advance cursor */
                    finalize_draw_list_entry(entry, esi, ebx, ecx, (uint32_t)edxv, edi, ebp, es, fs, gs);
                }
            }
        }
        node = *(volatile uint32_t *)(uintptr_t)node;                  /* node = node[0] (next) */
    } while (node != 0);
    #undef ES16
}

/* build_weapon_billboard_record (0x29f50): build the player weapon/active-item billboard record at `eax`
 * (= 0x853dc) from view globals, advance its animation STATE MACHINE (state record at 0x92024: +8 phase,
 * +0xa flags, +0x1a frame timer; driven by globals 0x84920 [mode/request], 0x819cd [alt mode], 0x7e8d8
 * [flag], 0x85324 [timer step], 0x81050 [active item ptr]), then tail-call emit_object_draw_entries
 * (0x29dcf) to enqueue it as a 1-node list. edx (in) -> [eax+4]. ecx = passthrough. Void.
 * NOTE relocated address-immediates: `mov ecx,0x92024` -> 0x92024+OBJ_DELTA; `sub edx,0x91e03` -> the two
 * OBJ_DELTAs cancel to the canonical offset (0x92024-0x91e03 = 0x221). */
void build_weapon_billboard_record(uint32_t eax, uint32_t ecx, uint32_t edx, uint32_t ebp,
                                          uint16_t es, uint16_t fs, uint16_t gs)
{
    #define R8(o)  (*(volatile uint8_t  *)(uintptr_t)(eax + (uint32_t)(o)))
    #define R16(o) (*(volatile uint16_t *)(uintptr_t)(eax + (uint32_t)(o)))
    #define R32(o) (*(volatile uint32_t *)(uintptr_t)(eax + (uint32_t)(o)))
    R32(4)    = edx;                                       /* [eax+4] = edx (input) */
    R16(8)    = G16(VA_g_player_x);
    R16(0xa)  = G16(VA_g_player_y);
    R8(0x10)  = 0x80;
    R16(0x12) = G16(VA_g_player_z);
    R16(0x14) = 0x221;
    R8(0x11)  = 1;
    uint32_t wptr = G32(VA_g_selected_item_primary + 0xc);                          /* active weapon/item ptr */
    if (wptr == 0) return;                                 /* je 0x2a09c */
    uint16_t cxv = *(volatile uint16_t *)(uintptr_t)(wptr + 2);
    R16(0xc) = cxv;
    if (cxv <= 0x200) return;                              /* jbe 0x2a09c */

    uint32_t ecxp = 0x92024u + OBJ_DELTA;                  /* anim-state record (relocated ptr) */
    #define A8(o)  (*(volatile uint8_t  *)(uintptr_t)(ecxp + (uint32_t)(o)))
    #define A16(o) (*(volatile uint16_t *)(uintptr_t)(ecxp + (uint32_t)(o)))
    #define A32(o) (*(volatile uint32_t *)(uintptr_t)(ecxp + (uint32_t)(o)))
    A32(4) = eax;
    A32(4) = A32(4) + 8;                                   /* [ecx+4] = eax+8 */
    /* mov edx,[0x85cf4] at 0x29fb0 is dead (edx overwritten at 0x29ff7) -> skip */
    A32(0) = 0;                                            /* [ecx] = 0 */

    int do_reset = 0;
    if (G32(VA_g_player_airborne + 0xc) != 0) {                               /* ---- branch B (0x2a06a) ---- */
        if (A8(8) & 0x40) {
            if (A16(0x1a) <= 0x100)                        /* ja 0x29ff7 -> add only when <= */
                A16(0x1a) = (uint16_t)(A16(0x1a) + G16(VA_g_frame_time_scale));
        } else {                                           /* 0x2a08d */
            A8(8) = 0x41;
            A16(0x1a) = 0;
        }
    } else if (G32(VA_g_format_flags + 0x3) != 0) {                        /* ---- branch A (0x2a01b) ---- */
        if ((G32(VA_g_format_flags + 0x3) & 0x100) || !(A8(8) & 8)) {      /* 0x2a04b (no reset) */
            A16(0x1a) = 0;
            uint32_t v = G32(VA_g_format_flags + 0x3) & 3;
            G32(VA_g_format_flags + 0x3) = v;
            A8(8) = (uint8_t)(v + 7);
        } else {
            if (A16(0x1a) > 0x100) do_reset = 1;           /* ja 0x29fd2 */
            else {
                A16(0x1a) = (uint16_t)(A16(0x1a) + G16(VA_g_frame_time_scale));
                if (A16(0x1a) > 0x100) do_reset = 1;       /* ja 0x29fd2 */
            }
        }
    } else {
        do_reset = 1;
    }
    if (do_reset) {                                        /* ---- 0x29fd2 reset ---- */
        G32(VA_g_format_flags + 0x3) = 0;
        A16(0x1a) = 0;
        A8(8) = 4;
        A8(0xa) = 0;
        if (G32(VA_g_move_speed_accum) == 0) A8(0xa) = (uint8_t)(A8(0xa) | 2);
    }

    /* ---- 0x29ff7 converge ---- */
    uint32_t edxv = ecxp - (0x91e03u + OBJ_DELTA);         /* edx = ecx - 0x91e03 (= 0x221) */
    R16(0x14) = (uint16_t)edxv;
    uint16_t dxv = (uint16_t)(-(int32_t)(uint16_t)G16(VA_g_player_angle));   /* dx=[0x90a8a]; neg edx (low16) */
    dxv = (uint16_t)(dxv + 0x100);                         /* inc dh (16-bit wrap) */
    dxv = (uint16_t)(dxv >> 1);                            /* shr dx,1 */
    R8(0xe) = (uint8_t)dxv;                                /* [eax+0xe] = dl */
    emit_object_draw_entries(eax, ecx, ebp, es, fs, gs);   /* tail-call 0x29dcf */
    #undef R8
    #undef R16
    #undef R32
    #undef A8
    #undef A16
    #undef A32
}

/* insert_worklist_entry (0x2a446): depth-sorted insert of draw-entry `eax` into the per-sector worklist.
 * Worklist node (0xc bytes) at cursor `edi`: +0 next (ABS ptr into the [0x8498c] buffer), +4 entry ptr,
 * +8 = [0x90968]. The per-sector list HEAD is a REL offset at fs:[sectornode+4] (sectornode = es:[ebx+4]-8).
 * Depth key = [entry+4] (signed). Returns the new cursor (advanced 0xc on insert, else unchanged). */
uint32_t insert_worklist_entry(uint32_t edi, uint32_t eax, uint32_t ebx,
                                      uint32_t es_base, uint32_t fs_base)
{
    #define IES16(o) (*(volatile uint16_t *)(uintptr_t)(es_base + (uint32_t)(o)))
    #define IFS16(o) (*(volatile uint16_t *)(uintptr_t)(fs_base + (uint32_t)(o)))
    #define IA32(p)  (*(volatile uint32_t *)(uintptr_t)(p))
    if (edi >= (uint32_t)G32(VA_g_door_worklist + 0x4)) return edi;        /* cmp edi,[0x84990]; jae */
    uint32_t esi = IES16(ebx + 4);                        /* si = es:[ebx+4] */
    if (esi == 0) return edi;
    IA32(edi + 4) = eax;                                  /* node.entry = eax */
    esi -= 8;                                             /* sector node base */
    uint32_t base = G32(VA_g_door_worklist);
    uint32_t ecx = IFS16(esi + 4);                        /* head rel (zero-ext) */
    int update_head = 1;
    if (ecx != 0) {
        ecx += base;                                      /* head ABS */
        int32_t newd = (int32_t)IA32(eax + 4);
        if (newd < (int32_t)IA32(IA32(ecx + 4) + 4)) {    /* cmp new,head.depth; jge front */
            for (;;) {                                    /* 0x2a484 walk */
                if (IA32(ecx) == 0) break;                /* ecx.next==0 -> tail */
                uint32_t prev = ecx;
                ecx = IA32(ecx);                          /* ecx = ecx.next */
                if ((int32_t)IA32(IA32(ecx + 4) + 4) <= newd) { ecx = prev; break; }  /* new>=x -> after prev */
            }
            IA32(edi) = IA32(ecx);                        /* node.next = ecx.next */
            IA32(ecx) = edi;                              /* ecx.next = node */
            update_head = 0;                              /* jmp 0x2a4b4 (skip head update) */
        }
        /* else front insert: ecx = head abs -> head update with node.next = head */
    }
    if (update_head) {
        IA32(edi) = ecx;                                  /* node.next = ecx (head abs or 0) */
        IFS16(esi + 4) = (uint16_t)(edi - base);          /* fs head = node rel */
    }
    IA32(edi + 8) = G32(VA_g_view_bound_right);                         /* node[+8] = [0x90968] */
    return edi + 0xc;
    #undef IES16
    #undef IFS16
    #undef IA32
}

/* build_scene_draw_list (0x2a0a0): the draw-list cluster ORCHESTRATOR. Allocates its own 0x20-byte frame,
 * runs the object passes (0x29dcf x2 + 0x29f50), then the main face loop (back-to-front over the painter
 * order from 0x2a6d0): per visible sector it walks section-E faces and emits draw entries by type —
 * type 1 (+ a mirror double via the 0x84924 synthetic record) finalized through 0x2a4d0, types 2/3
 * inserted via 0x2a446. Then a second pass over the door pool (0x8b3f8, stride 0x1f6) emitting type-4
 * entries. No GP-register inputs except ecx (passthrough to 0x2c720). Void. */
void build_scene_draw_list(uint32_t ecx_in)
{
    extern uint32_t (*g_os_sel_base)(uint16_t);
    uint16_t gs_sel = (uint16_t)G16(VA_g_vertex_selector), fs_sel = (uint16_t)G16(VA_g_map_geometry_selector), es_sel = (uint16_t)G16(VA_g_surface_record_selector);
    #ifndef ROTH_STANDALONE
    if (g_os_sel_base == NULL) {
        regs_t io; memset(&io, 0, sizeof io);
        io.va = 0x2a0a0u + OBJ_DELTA; io.ecx = ecx_in;  /* [ORACLE-FALLBACK] */
        io.gs = gs_sel; io.fs = fs_sel; io.es = es_sel;
        call_orig(&io);
        return;
    }
    #endif
    uint32_t fs_base = g_os_sel_base(fs_sel);
    uint32_t es_base = g_os_sel_base(es_sel);
    #define FS16(o) (*(volatile uint16_t *)(uintptr_t)(fs_base + (uint32_t)(o)))
    #define ES8(o)  (*(volatile uint8_t  *)(uintptr_t)(es_base + (uint32_t)(o)))
    #define ES16(o) (*(volatile uint16_t *)(uintptr_t)(es_base + (uint32_t)(o)))
    #define P8(p)   (*(volatile uint8_t  *)(uintptr_t)(p))
    #define P16(p)  (*(volatile uint16_t *)(uintptr_t)(p))
    #define P32(p)  (*(volatile uint32_t *)(uintptr_t)(p))

    uint32_t frame[8];                                    /* the local 0x20-byte ebp frame */
    uint32_t ebp = (uint32_t)(uintptr_t)frame;
    #define FRM16(o) (*(volatile uint16_t *)(uintptr_t)(ebp + (uint32_t)(o)))
    #define FRM32(o) (*(volatile uint32_t *)(uintptr_t)(ebp + (uint32_t)(o)))

    FRM16(4) = (uint16_t)(-(int32_t)(uint16_t)G16(VA_g_sprite_view_angle));   /* [ebp+4] = -[0x909f8] (rotate sign) */
    FRM32(8) = G32(VA_g_door_worklist);                                   /* [ebp+8] = cursor = worklist base */

    if (G8(VA_g_render_sector_walk_mode + 0x25) == 0) {                                    /* object passes */
        if (G32(VA_g_current_decoded_frame + 0x14) != 0)
            emit_object_draw_entries(G32(VA_g_current_decoded_frame + 0x14), ecx_in, ebp, es_sel, fs_sel, gs_sel);
        if (G8(VA_g_render_sector_walk_mode) != 0) {
            uint16_t ps = G16(VA_g_player_sector);
            if (ps != 0)
                build_weapon_billboard_record(0x853dcu + OBJ_DELTA, ecx_in, ps, ebp, es_sel, fs_sel, gs_sel);
        }
        if (G32(VA_g_particle_pool) != 0)
            emit_object_draw_entries(G32(VA_g_particle_pool), ecx_in, ebp, es_sel, fs_sel, gs_sel);
    }

    /* ---- main face loop: back-to-front over the painter order from 0x2a6d0 ---- */
    uint32_t count = FS16(0xfc00);
    uint32_t lp = 0xfc00 + 2 * count;                         /* list ptr (walk backwards by 2) */
    int32_t outer = (int32_t)count;
    do {
        uint32_t node  = FS16(lp);                            /* ordered node id */
        uint32_t head  = FS16(node + 6);                      /* interval list head */
        uint32_t recid = FS16(head + 2);                      /* fs:[head+2] = sector record id */
        if (ES8(recid + 0x16) & 2) {                          /* es:[recid+0x16] & 2 */
            uint32_t cursor = FRM32(8);                       /* edi = [ebp+8] */
            uint32_t ebx_recid = recid;
            uint32_t idx = (uint16_t)(recid - ES16(4));       /* ax -= es:[4] */
            idx = (uint16_t)(idx / 0xd);                      /* div cx (0xd, 16-bit unsigned) */
            idx = (uint16_t)(idx + 2);
            uint32_t se = G32(VA_g_map_objects_buffer) + P16(G32(VA_g_map_objects_buffer) + idx);  /* esi = section-E + word[se+idx] */
            uint8_t  fc = P8(se);                             /* cl = [esi] face count */
            if (fc != 0) {
                uint32_t fr = se + 2;                          /* first face */
                uint32_t ecx_fc = fc;
                do {
                    uint8_t f7 = P8(fr + 7);
                    if (f7 & 0x80) goto next_face;             /* test [esi+7],0x80; jne 0x2a333 */
                    if (G8(VA_g_render_sector_walk_mode + 0x25) != 0 && (P8(fr + 9) & 2)) goto next_face;
                    if (cursor >= (uint32_t)G32(VA_g_door_worklist + 0x4)) return;   /* jae 0x2a440 = pop/pop/ret (abort all) */
                    FRM16(0) = (uint16_t)(P16(fr)     + G16(VA_g_view_offset_x));
                    FRM16(2) = (uint16_t)(P16(fr + 2) + G16(VA_g_view_offset_y));
                    int32_t depth = 0, edxv = 0;
                    rotate_point_2d((const int16_t *)(uintptr_t)ebp, &depth, &edxv);
                    if (f7 & 1) {
                        /* ---- type 3 (0x2a2f5) ---- */
                        if (depth < -0x8000) goto next_face;   /* cmp eax,0xffff8000; jl */
                        int32_t ad = (edxv < 0) ? -edxv : edxv;       /* abs(edx) */
                        int32_t q  = (depth != 0) ? (ad / depth) : 0; /* idiv */
                        if (q > 0x28) goto next_face;
                        P32(cursor + 8) = fr;
                        P32(cursor)     = (uint32_t)edxv;
                        P32(cursor + 4) = (uint32_t)depth;
                        P16(cursor + 0x12) = 3;
                        uint32_t en = cursor; cursor += 0x26;
                        cursor = insert_worklist_entry(cursor, en, ebx_recid, es_base, fs_base);
                        goto next_face;
                    }
                    if (depth < 0x1000) goto next_face;        /* near plane */
                    uint8_t f9 = P8(fr + 9);
                    if (f9 & 0x80) {
                        /* ---- type 2 (0x2a2e0) ---- */
                        P32(cursor + 8) = fr;
                        P32(cursor)     = (uint32_t)edxv;
                        P32(cursor + 4) = (uint32_t)depth;
                        P16(cursor + 0x12) = 2;
                        uint32_t en = cursor; cursor += 0x26;
                        cursor = insert_worklist_entry(cursor, en, ebx_recid, es_base, fs_base);
                        goto next_face;
                    }
                    if (f9 & 1) {
                        /* ---- type 1 + mirror double (0x2a227) ---- */
                        P32(cursor + 4) = (uint32_t)depth;  G32(VA_g_view_clip_plane) = (uint32_t)depth;
                        P32(cursor)     = (uint32_t)edxv;   G32(VA_g_visible_extent_list + 0x3c) = (uint32_t)edxv;
                        P32(cursor + 8) = fr;
                        P16(cursor + 0x12) = 1;
                        uint32_t en1 = cursor; cursor += 0x26; FRM32(8) = cursor;
                        finalize_draw_list_entry(en1, fr, ebx_recid, ecx_fc, (uint32_t)edxv, cursor, ebp, es_sel, fs_sel, gs_sel);
                        cursor = FRM32(8);
                        /* second (mirror) entry via the 0x84924 synthetic record */
                        uint32_t en2 = cursor;
                        P32(cursor + 4) = (uint32_t)depth;  G32(VA_g_view_clip_plane) = (uint32_t)depth;
                        P32(cursor)     = (uint32_t)edxv;   G32(VA_g_visible_extent_list + 0x3c) = (uint32_t)edxv;
                        P32(cursor + 8) = 0x84924u + OBJ_DELTA;
                        P16(cursor + 0x12) = 1;
                        G32(VA_g_format_flags + 0x7) = P32(fr);                /* [0x84924] = [esi] */
                        uint32_t d = ES16(ebx_recid + 0x18);
                        uint16_t mv;
                        if (d != 0 && (int16_t)ES16(d + 8) <= (int16_t)P16(fr + 0xa))
                            mv = ES16(d + 8);                  /* jle -> keep es:[d+8] */
                        else
                            mv = ES16(ebx_recid + 2);          /* d==0 or es:[d+8] > [face+0xa] */
                        G16(VA_g_format_flags + 0x11) = mv;
                        G16(VA_g_format_flags + 0xb) = 0xefff;
                        G32(VA_g_format_flags + 0xd) = 0;
                        G32(VA_g_format_flags + 0x13) = 0;
                        cursor += 0x26; FRM32(8) = cursor;
                        finalize_draw_list_entry(en2, 0x84924u + OBJ_DELTA, ebx_recid, ecx_fc, (uint32_t)edxv, cursor, ebp, es_sel, fs_sel, gs_sel);
                        cursor = FRM32(8);
                        goto next_face;
                    }
                    /* ---- type 1 (0x2a1f1) ---- */
                    P32(cursor + 4) = (uint32_t)depth;  G32(VA_g_view_clip_plane) = (uint32_t)depth;
                    P32(cursor)     = (uint32_t)edxv;   G32(VA_g_visible_extent_list + 0x3c) = (uint32_t)edxv;
                    P32(cursor + 8) = fr;
                    P16(cursor + 0x12) = 1;
                    uint32_t en = cursor; cursor += 0x26; FRM32(8) = cursor;
                    finalize_draw_list_entry(en, fr, ebx_recid, ecx_fc, (uint32_t)edxv, cursor, ebp, es_sel, fs_sel, gs_sel);
                    cursor = FRM32(8);
                  next_face:
                    fr += 0x10;
                } while ((int8_t)(--ecx_fc) > 0);             /* dec cl; jg */
            }
            FRM32(8) = cursor;                                /* 0x2a33e mov [ebp+8], edi */
        }
        lp -= 2;
    } while (--outer > 0);                                    /* dec ecx; jg 0x2a128 */

    /* ---- second pass: door pool at 0x8b3f8 (stride 0x1f6) -> type-4 entries ---- */
    uint8_t dc = G8(VA_g_door_count);
    if (dc != 0) {
        uint32_t cursor = FRM32(8);
        uint32_t door = 0x8b3f8u + OBJ_DELTA;
        do {
            uint32_t bx = P16(door);                          /* [door+0] */
            uint32_t ax = P16(door + 0x10);                   /* [door+0x10] */
            int process = 0;
            if (ax != 0 && ES16(ax + 4) != 0) process = 1;    /* 0x2a370 jne 0x2a384 */
            else if (ES16(bx + 4) != 0) process = 1;          /* 0x2a378 */
            if (process && bx != 0) {                         /* 0x2a384 or ebx,ebx; je skip */
                P32(cursor + 8) = door;                        /* [edi+8] = door */
                uint32_t cd = P32(door + 0x2e) + 0x82;         /* corner data */
                int32_t cx = (int32_t)(int16_t)P16(cd) + (int16_t)P16(cd + 0x10)
                           + (int16_t)P16(cd + 0x20) + (int16_t)P16(cd + 0x30);
                FRM16(0) = (uint16_t)((cx >> 2) + (int16_t)G16(VA_g_view_offset_x));
                int32_t cy = (int32_t)(int16_t)P16(cd + 4) + (int16_t)P16(cd + 0x14)
                           + (int16_t)P16(cd + 0x24) + (int16_t)P16(cd + 0x34);
                FRM16(2) = (uint16_t)((cy >> 2) + (int16_t)G16(VA_g_view_offset_y));
                int32_t depth = 0, edxv = 0;
                rotate_point_2d((const int16_t *)(uintptr_t)ebp, &depth, &edxv);
                P32(cursor)     = (uint32_t)edxv;
                P32(cursor + 4) = (uint32_t)depth;
                P16(cursor + 0x12) = 4;
                uint32_t en = cursor; cursor += 0x26;
                uint32_t s10 = P16(door + 0x10);              /* bx = [door+0x10] */
                if (s10 != 0 && ES16(s10 + 4) != 0)
                    cursor = insert_worklist_entry(cursor, en, s10, es_base, fs_base);
                if (!(P8(door + 2) & 2) && bx != 0 && ES16(bx + 4) != 0)
                    cursor = insert_worklist_entry(cursor, en, bx, es_base, fs_base);
            }
            door += 0x1f6;
        } while ((int8_t)(--dc) > 0);
        FRM32(8) = cursor;
    }
    #undef FS16
    #undef ES8
    #undef ES16
    #undef P8
    #undef P16
    #undef P32
    #undef FRM16
    #undef FRM32
}

/* ---------------------------------------------------------------------------------------------
 * Face-list render subtree (the "missing middle"), bottom-up. First LEAF:
 *
 * compute_face_span_extents (0x2c250): from a face's two edge vertices (A = es:[esi+6], B = es:[es:[esi+8]+6])
 * compute the screen-space span extents + 4 projected screen coords. esi = face/draw record (ES offset), ebx =
 * FS edge record offset. Pure obj3 writer (0x852ba/0x852c0/0x852b8 + the 4 extents 0x852b0/0x852ac/0x852b2/
 * 0x852ae); reads es:/fs: geometry + globals 0x8531c/0x90a2e/0x85288/0x909a4. NO framebuffer. Leaf.
 * Subtle 16/32-bit: the original's `neg edx`/`neg eax` low16 is re-derived by movsx/cwde, so high bits don't
 * matter; the 4 projections reuse proj64 + clamp_screen_3ffe (the ±0x3ffe screen clamp). ------------------- */
void compute_face_span_extents(uint32_t esi, uint32_t ebx, uint16_t es, uint16_t fs)
{
    extern uint32_t (*g_os_sel_base)(uint16_t);
    #ifndef ROTH_STANDALONE
    if (g_os_sel_base == NULL) {
        regs_t io; memset(&io, 0, sizeof io);
        io.va = 0x2c250u + OBJ_DELTA; io.esi = esi; io.ebx = ebx; io.es = es; io.fs = fs;  /* [ORACLE-FALLBACK] */
        call_orig(&io);
        return;
    }
    #endif
    uint32_t es_base = g_os_sel_base(es);
    uint32_t fs_base = g_os_sel_base(fs);
    /* 16-bit address arithmetic: the original uses `es:[si+...]` / `fs:[bx+...]` (16-bit registers), so the
     * effective offset is masked to 0xffff — the caller may pass esi/ebx with high bits set (a flat pointer) that
     * the function ignores. Mask every offset, else a large esi double-counts the base (-> wrong reads). */
    #define ES8(o)  (*(volatile uint8_t  *)(uintptr_t)(es_base + (uint16_t)(o)))
    #define ES16(o) (*(volatile uint16_t *)(uintptr_t)(es_base + (uint16_t)(o)))
    #define FS16(o) (*(volatile uint16_t *)(uintptr_t)(fs_base + (uint16_t)(o)))
    #define FS32(o) (*(volatile uint32_t *)(uintptr_t)(fs_base + (uint16_t)(o)))

    uint32_t di = ES16(ES16(esi + 8) + 6);                 /* vertex B */
    uint32_t si = ES16(esi + 6);                           /* vertex A */

    int16_t minx = (int16_t)ES16(si);                      /* min(A.x, B.x) signed */
    if ((int16_t)ES16(di) < minx) minx = (int16_t)ES16(di);
    G16(VA_g_face_span_top) = (uint16_t)minx;
    G16(VA_g_face_span_top + 0x6) = (uint16_t)(0u - (uint16_t)((uint16_t)minx - G16(VA_g_secondary_surface_count + 0x4)));   /* -(min.x - [0x8531c]) lo16 */
    int32_t edxv = (int32_t)(int16_t)G16(VA_g_face_span_top + 0x6);

    int16_t maxy = (int16_t)ES16(si + 2);                  /* max(A.y, B.y) signed */
    if ((int16_t)ES16(di + 2) > maxy) maxy = (int16_t)ES16(di + 2);
    G16(VA_g_face_span_bottom) = (uint16_t)maxy;
    int32_t eaxv = (int32_t)(int16_t)(uint16_t)(0u - (uint16_t)((uint16_t)maxy - G16(VA_g_secondary_surface_count + 0x4)));

    if ((G8(VA_g_current_surface_render_flags) & 8) && ES8(si + 0xc) != 0) {         /* optional slope adjust by es:[si+0xc] */
        int32_t ebxv = (int32_t)(int8_t)ES8(si + 0xc) << 2;            /* signed *4 */
        if ((int8_t)ES8(si + 0xc) >= 0) {                  /* > 0 */
            G16(VA_g_face_span_bottom) = (uint16_t)((uint16_t)G16(VA_g_face_span_top) - (uint32_t)ebxv);
            eaxv = (int32_t)(int16_t)(uint16_t)((uint32_t)edxv + (uint32_t)ebxv);
        } else {                                           /* < 0 */
            G16(VA_g_face_span_top) = (uint16_t)((uint16_t)G16(VA_g_face_span_bottom) - (uint32_t)ebxv);
            edxv = (int32_t)(int16_t)(uint16_t)((uint32_t)eaxv + (uint32_t)ebxv);
            G16(VA_g_face_span_top + 0x6) = (uint16_t)edxv;
        }
    }

    /* 4 projections: A=eaxv, B=edxv; divisor ecx1 (fs:[bx+0xc] or 0x1000) then ecx2 (fs:[bx+0x20] or 0x1000) */
    int32_t scale = (int32_t)G32(VA_g_perspective_scale), center = (int32_t)G32(VA_g_span_src_wrap_reoffset + 0x28);
    int32_t ecx1 = (FS16(ebx + 0x12) & 4) ? 0x1000 : (int32_t)FS32(ebx + 0xc);
    G16(VA_g_map_geometry_selector + 0x1c) = (uint16_t)clamp_screen_3ffe(proj64(eaxv, scale, ecx1, center));
    G16(VA_g_map_geometry_selector + 0x18) = (uint16_t)clamp_screen_3ffe(proj64(edxv, scale, ecx1, center));
    int32_t ecx2 = (FS16(ebx + 0x12) & 8) ? 0x1000 : (int32_t)FS32(ebx + 0x20);
    G16(VA_g_map_geometry_selector + 0x1e) = (uint16_t)clamp_screen_3ffe(proj64(eaxv, scale, ecx2, center));
    G16(VA_g_map_geometry_selector + 0x1a) = (uint16_t)clamp_screen_3ffe(proj64(edxv, scale, ecx2, center));
    #undef ES8
    #undef ES16
    #undef FS16
    #undef FS32
}

/* emit_world_span_record (0x2d130): build the 4-vertex span record at 0x84f18 (flag word from [0x90a2e], a vertex
 * quad via the existing leaf emit_vertex_bbox 0x2d29a from the projected coords 0x852c2/0x852c4 x 0x852a4/6/a/8,
 * the index ring, the bbox) then render it via the (bridged) rasterizer rasterize_world_spans_scanline 0x366cb. No
 * GP-register inputs; es/fs/gs pass through to 0x366cb. Native build (fixed obj3 pointers — no fault risk) + bridged
 * render. NOTE the existing emit_vertex_bbox does NOT advance edi/ebx — the caller advances (+0x10 / +2). */

/* Render the span record at 0x84f18 via the NATIVE rasterizer (0x366cb is lifted as of W23), replacing the
 * old `call_orig(0x366cb)` bridge every span emitter used (they were lifted in W12-W16 while 0x366cb was
 * still bridged). Resolves the rasterizer's mapper bases from its selector globals, exactly like the
 * ABI_SPANSCANLINE live-swap adapter; g_os_game_ds (host-set per dispatch) feeds the rare indirect-resolve
 * path's es=ds. Falls back to the original bridge when there's no selector resolver (oracle). */
static void rwss_render_span_record(uint16_t es, uint16_t fs, uint16_t gs)
{
    extern uint32_t (*g_os_sel_base)(uint16_t);
    extern uint16_t g_os_game_ds;
    #ifndef ROTH_STANDALONE
    if (g_os_sel_base == NULL) {                          /* oracle / no resolver: keep the bridge */
        regs_t io; memset(&io, 0, sizeof io);
        io.va = 0x366cbu + OBJ_DELTA; io.esi = 0x84f18u + OBJ_DELTA;  /* [ORACLE-FALLBACK] */
        io.es = es; io.fs = fs; io.gs = gs;
        call_orig(&io);
        return;
    }
    #endif
    rasterize_world_spans_scanline(
        0x84f18u + OBJ_DELTA,
        g_os_sel_base((uint16_t)G16(VA_g_active_world_remap_selector)),            /* gs_base (colormap) */
        g_os_sel_base((uint16_t)G16(VA_g_transparency_blend_selector)),            /* blend_base */
        g_os_sel_base((uint16_t)G16(VA_g_world_alt_render_flags + 0x2)),            /* fs_tex_base */
        g_os_sel_base((uint16_t)G16(VA_g_render_target_selector)),            /* es_fb_base */
        es, fs, gs, g_os_game_ds);
}

void emit_world_span_record(uint16_t es, uint16_t fs, uint16_t gs)
{
    uint8_t *esi = (uint8_t *)(uintptr_t)(0x84f18u + OBJ_DELTA);   /* the span record (fixed) */
    #define SP8(p)  (*(volatile uint8_t  *)(p))
    #define SP16(p) (*(volatile uint16_t *)(p))
    uint8_t cl = G8(VA_g_current_surface_render_flags);
    uint16_t ax = cl;                                       /* al = cl, eax cleared */
    if (!(cl & 4))    ax |= 0x100;
    ax &= 0x183;
    if (!(cl & 0x10)) ax |= 0x40;
    ax |= 0x18;
    if (SP8(esi + 0xd) & 0x80) ax &= 0xfff7;
    SP16(esi + 0x16) = ax;                                  /* span flag word */
    SP16(esi + 0x34) = 4;                                   /* vertex count */
    uint8_t *ebx = esi + 0x36;                              /* index list */
    uint8_t *edi = (uint8_t *)(uintptr_t)(0x84f9eu + OBJ_DELTA);   /* vertex buffer */
    SP16(esi + 0x28) = 0x7fff; SP16(esi + 0x2a) = 0x7fff;   /* bbox init (min seeds) */
    SP16(esi + 0x2c) = 0x8000; SP16(esi + 0x2e) = 0x8000;   /* (max seeds) */
    uint8_t *start_edi = edi;
    const struct { uint32_t xg, yg; } v[4] = {             /* projected-coord global addrs feeding the 4 vertices */
        { 0x852c2, 0x852a4 }, { 0x852c4, 0x852a6 }, { 0x852c4, 0x852aa }, { 0x852c2, 0x852a8 } };
    for (int i = 0; i < 4; i++) {
        emit_vertex_bbox((int16_t)G16(v[i].xg), (int16_t)G16(v[i].yg), ebx, edi, esi);
        edi += 0x10; ebx += 2;                             /* the advance the original does in 0x2d29a */
    }
    SP16(ebx) = (uint16_t)((uint32_t)(uintptr_t)start_edi - 0x484f9eu);   /* close the index ring (= 0) */
    G8(VA_g_column_clip_mode) = G8(VA_g_span_clip_source);
    rwss_render_span_record(es, fs, gs);   /* render via the NATIVE rasterizer (was call_orig 0x366cb) */
    #undef SP8
    #undef SP16
}

/* emit_world_span_unclipped_indexed (0x2d5b0): build the span record at 0x84f18 from a backwards-walked array of
 * cl clipped-vertex records (stride 0x14 from fs:[ebx], starting at record cl-1), emitting 1 or 2 vertices each
 * via the native leaf emit_vertex_bbox (0x2d29a) + an extra [vtx+0xa] field, then render via bridged 0x366cb.
 * Inputs: EAX (->[0x84f24]), CL=record count, EBX=record-array FS offset, + es/fs/gs. 16-bit fs: addressing (mask
 * offsets); fault recovery covers a transient/garbage ebx. NATIVE build + bridged render (ABI_SPANEMIT). */
void emit_world_span_unclipped_indexed(uint32_t eax_in, uint32_t ecx_in, uint32_t ebx_in,
                                              uint16_t es, uint16_t fs, uint16_t gs)
{
    extern uint32_t (*g_os_sel_base)(uint16_t);
    #ifndef ROTH_STANDALONE
    if (g_os_sel_base == NULL) {
        regs_t io; memset(&io, 0, sizeof io);
        io.va = 0x2d5b0u + OBJ_DELTA; io.eax = eax_in; io.ecx = ecx_in; io.ebx = ebx_in;  /* [ORACLE-FALLBACK] */
        io.es = es; io.fs = fs; io.gs = gs;
        call_orig(&io);
        return;
    }
    #endif
    uint32_t fs_base = g_os_sel_base(fs);
    #define FS16(o) (*(volatile uint16_t *)(uintptr_t)(fs_base + (uint16_t)(o)))   /* 16-bit fs: addressing */
    #define FS32(o) (*(volatile int32_t  *)(uintptr_t)(fs_base + (uint16_t)(o)))
    uint8_t *esi = (uint8_t *)(uintptr_t)(0x84f18u + OBJ_DELTA);
    #define SP16(p) (*(volatile uint16_t *)(p))
    G16(VA_g_world_span_record + 0xc) = (uint16_t)eax_in;
    uint8_t cl = (uint8_t)ecx_in;
    uint32_t ebx = ebx_in + ((uint32_t)cl - 1u) * 0x14u;             /* point at record cl-1 */
    uint8_t *ebp = esi + 0x36;                                       /* index list */
    uint8_t *edi = (uint8_t *)(uintptr_t)(0x84f9eu + OBJ_DELTA);     /* vertex buffer */
    SP16(esi + 0x34) = 0;                                            /* vertex count */
    uint8_t *start_edi = edi;
    SP16(esi + 0x28) = 0x7fff; SP16(esi + 0x2a) = 0x7fff;
    SP16(esi + 0x2c) = 0x8000; SP16(esi + 0x2e) = 0x8000;
    do {
        uint16_t ax = FS16(ebx);
        if (ax != 0x8000) {                                         /* 0x8000 = skip-sentinel */
            if (FS32(ebx + 0x20) < 0x1000) {                        /* two-vertex record */
                SP16(edi + 0xa) = FS16(ebx + 0x21);
                emit_vertex_bbox((int16_t)FS16(ebx + 2), (int16_t)FS16(ebx + 0x18), ebp, edi, esi);
                edi += 0x10; ebp += 2; SP16(esi + 0x34) = (uint16_t)(SP16(esi + 0x34) + 1);
                SP16(edi + 0xa) = FS16(ebx + 0xd);
                emit_vertex_bbox((int16_t)FS16(ebx), (int16_t)FS16(ebx + 4), ebp, edi, esi);
                edi += 0x10; ebp += 2; SP16(esi + 0x34) = (uint16_t)(SP16(esi + 0x34) + 1);
            } else {                                                /* single-vertex record */
                SP16(edi + 0xa) = FS16(ebx + 0xd);
                emit_vertex_bbox((int16_t)FS16(ebx), (int16_t)FS16(ebx + 4), ebp, edi, esi);
                edi += 0x10; ebp += 2; SP16(esi + 0x34) = (uint16_t)(SP16(esi + 0x34) + 1);
            }
        }
        ebx -= 0x14;
    } while ((int8_t)(--cl) > 0);
    SP16(ebp) = (uint16_t)((uint32_t)(uintptr_t)start_edi - 0x484f9eu);   /* close the index ring */
    G8(VA_g_column_clip_mode) = G8(VA_g_span_clip_source);
    rwss_render_span_record(es, fs, gs);                            /* render via the NATIVE rasterizer (was call_orig 0x366cb) */
    #undef FS16
    #undef FS32
    #undef SP16
}

/* emit_world_span_unclipped (0x2d3d0): the FORWARD-walked sibling of 0x2d5b0. Same span record at 0x84f18, but walks
 * the cl clipped-vertex records (stride 0x14 from fs:[ebx]) from record 0 FORWARD (ebx += 0x14; no pre-advance, the
 * `sub ch,ch` of the original is a no-op for us). Per record: skip 0x8000 sentinel; if fs:[ebx+0x20] < 0x1000 emit
 * TWO vertices else ONE, each via the native leaf emit_vertex_bbox (0x2d29a) + an extra [vtx+0xa] field. The vertex
 * field offsets differ from 0x2d5b0 (forward order): two-vert -> {extra +0xd, x +0, y +6} then {extra +0x21, x +2,
 * y +0x1a}; single -> {extra +0xd, x +0, y +6}. Then render via bridged 0x366cb. Inputs: EAX (->[0x84f24]), CL=count,
 * EBX=record-array FS offset, + es/fs/gs. 16-bit fs: addressing (mask offsets); fault recovery covers a transient/
 * garbage ebx. NATIVE build + bridged render (ABI_SPANEMIT). */
void emit_world_span_unclipped(uint32_t eax_in, uint32_t ecx_in, uint32_t ebx_in,
                                      uint16_t es, uint16_t fs, uint16_t gs)
{
    extern uint32_t (*g_os_sel_base)(uint16_t);
    #ifndef ROTH_STANDALONE
    if (g_os_sel_base == NULL) {
        regs_t io; memset(&io, 0, sizeof io);
        io.va = 0x2d3d0u + OBJ_DELTA; io.eax = eax_in; io.ecx = ecx_in; io.ebx = ebx_in;  /* [ORACLE-FALLBACK] */
        io.es = es; io.fs = fs; io.gs = gs;
        call_orig(&io);
        return;
    }
    #endif
    uint32_t fs_base = g_os_sel_base(fs);
    #define FS16(o) (*(volatile uint16_t *)(uintptr_t)(fs_base + (uint16_t)(o)))   /* 16-bit fs: addressing */
    #define FS32(o) (*(volatile int32_t  *)(uintptr_t)(fs_base + (uint16_t)(o)))
    uint8_t *esi = (uint8_t *)(uintptr_t)(0x84f18u + OBJ_DELTA);
    #define SP16(p) (*(volatile uint16_t *)(p))
    G16(VA_g_world_span_record + 0xc) = (uint16_t)eax_in;
    uint8_t cl = (uint8_t)ecx_in;
    uint32_t ebx = ebx_in;                                          /* forward walk from record 0 */
    uint8_t *ebp = esi + 0x36;                                       /* index list */
    uint8_t *edi = (uint8_t *)(uintptr_t)(0x84f9eu + OBJ_DELTA);     /* vertex buffer */
    uint8_t *start_edi = edi;                                        /* saved (push edi) */
    SP16(esi + 0x34) = 0;                                            /* vertex count */
    SP16(esi + 0x28) = 0x7fff; SP16(esi + 0x2a) = 0x7fff;
    SP16(esi + 0x2c) = 0x8000; SP16(esi + 0x2e) = 0x8000;
    do {
        uint16_t ax = FS16(ebx);
        if (ax != 0x8000) {                                         /* 0x8000 = skip-sentinel */
            if (FS32(ebx + 0x20) < 0x1000) {                        /* two-vertex record */
                SP16(edi + 0xa) = FS16(ebx + 0xd);
                emit_vertex_bbox((int16_t)FS16(ebx), (int16_t)FS16(ebx + 6), ebp, edi, esi);
                edi += 0x10; ebp += 2; SP16(esi + 0x34) = (uint16_t)(SP16(esi + 0x34) + 1);
                SP16(edi + 0xa) = FS16(ebx + 0x21);
                emit_vertex_bbox((int16_t)FS16(ebx + 2), (int16_t)FS16(ebx + 0x1a), ebp, edi, esi);
                edi += 0x10; ebp += 2; SP16(esi + 0x34) = (uint16_t)(SP16(esi + 0x34) + 1);
            } else {                                                /* single-vertex record */
                SP16(edi + 0xa) = FS16(ebx + 0xd);
                emit_vertex_bbox((int16_t)FS16(ebx), (int16_t)FS16(ebx + 6), ebp, edi, esi);
                edi += 0x10; ebp += 2; SP16(esi + 0x34) = (uint16_t)(SP16(esi + 0x34) + 1);
            }
        }
        ebx += 0x14;
    } while ((int8_t)(--cl) > 0);
    SP16(ebp) = (uint16_t)((uint32_t)(uintptr_t)start_edi - 0x484f9eu);   /* close the index ring */
    G8(VA_g_column_clip_mode) = G8(VA_g_span_clip_source);
    rwss_render_span_record(es, fs, gs);                            /* render via the NATIVE rasterizer (was call_orig 0x366cb) */
    #undef FS16
    #undef FS32
    #undef SP16
}

/* clip_project_emit (orig 0x2d200 / 0x2d24d): the CLIPPED emitters' per-vertex helper. Perspective-projects a fixed
 * view-space Y (`src` = movsx [0x909fe]) through the per-vertex depth (clamped >= 0x1000) into a screen Y, then emits
 * the vertex via the leaf emit_vertex_bbox with the RAW x (caller's ax) + the projected/clamped y. 0x2d200 reads the
 * depth from fs:[bx+0xc]; 0x2d24d from fs:[bx+0x20] — the only difference, so the caller passes `depth` in. Uses the
 * existing proj64 (signed 64-bit imul/idiv) + clamp_screen_3ffe (±0x3ffe). */
void clip_project_emit(int32_t depth, int32_t src, int16_t x_in,
                       uint8_t *ebp, uint8_t *edi, uint8_t *esi)
{
    if (depth <= 0x1000) depth = 0x1000;
    int32_t y = clamp_screen_3ffe(proj64(src, G32(VA_g_perspective_scale), depth, G32(VA_g_span_src_wrap_reoffset + 0x28)));
    emit_vertex_bbox(x_in, (int16_t)y, ebp, edi, esi);
}

/* emit_world_span_clipped (0x2d2dd): the CLIPPED forward span emitter — same shape as 0x2d3d0 but each vertex Y is
 * NOT read directly; it is perspective-PROJECTED from a fixed view-space Y ([0x909fe]) through that vertex's depth via
 * clip_project_emit (orig 0x2d200/0x2d24d). Forward walk (record 0..cl-1, ebx += 0x14). Per record: two-vert ->
 * {extra +0xd, x +0, depth +0xc} then {extra +0x21, x +2, depth +0x20}; single -> {extra +0xd, x +0, depth +0xc}.
 * Then render via bridged 0x366cb. ABI/inputs identical to 0x2d3d0 (ABI_SPANEMIT). */
void emit_world_span_clipped(uint32_t eax_in, uint32_t ecx_in, uint32_t ebx_in,
                                    uint16_t es, uint16_t fs, uint16_t gs)
{
    extern uint32_t (*g_os_sel_base)(uint16_t);
    #ifndef ROTH_STANDALONE
    if (g_os_sel_base == NULL) {
        regs_t io; memset(&io, 0, sizeof io);
        io.va = 0x2d2ddu + OBJ_DELTA; io.eax = eax_in; io.ecx = ecx_in; io.ebx = ebx_in;  /* [ORACLE-FALLBACK] */
        io.es = es; io.fs = fs; io.gs = gs;
        call_orig(&io);
        return;
    }
    #endif
    uint32_t fs_base = g_os_sel_base(fs);
    #define FS16(o) (*(volatile uint16_t *)(uintptr_t)(fs_base + (uint16_t)(o)))   /* 16-bit fs: addressing */
    #define FS32(o) (*(volatile int32_t  *)(uintptr_t)(fs_base + (uint16_t)(o)))
    uint8_t *esi = (uint8_t *)(uintptr_t)(0x84f18u + OBJ_DELTA);
    #define SP16(p) (*(volatile uint16_t *)(p))
    G16(VA_g_world_span_record + 0xc) = (uint16_t)eax_in;
    G8(VA_g_column_clip_mode) = G8(VA_g_span_clip_source);                                      /* (also re-written before 0x366cb) */
    int32_t src = (int16_t)G16(VA_g_sprite_view_angle + 0x6);                            /* movsx [0x909fe] (fixed view-space Y) */
    uint8_t cl = (uint8_t)ecx_in;
    uint32_t ebx = ebx_in;                                          /* forward walk from record 0 */
    uint8_t *ebp = esi + 0x36;                                       /* index list */
    uint8_t *edi = (uint8_t *)(uintptr_t)(0x84f9eu + OBJ_DELTA);     /* vertex buffer */
    uint8_t *start_edi = edi;                                        /* saved (push edi) */
    SP16(esi + 0x34) = 0;                                            /* vertex count */
    SP16(esi + 0x28) = 0x7fff; SP16(esi + 0x2a) = 0x7fff;
    SP16(esi + 0x2c) = 0x8000; SP16(esi + 0x2e) = 0x8000;
    do {
        uint16_t ax = FS16(ebx);
        if (ax != 0x8000) {                                         /* 0x8000 = skip-sentinel */
            if (FS32(ebx + 0x20) < 0x1000) {                        /* two-vertex record */
                SP16(edi + 0xa) = FS16(ebx + 0xd);
                clip_project_emit(FS32(ebx + 0xc), src, (int16_t)FS16(ebx), ebp, edi, esi);
                edi += 0x10; ebp += 2; SP16(esi + 0x34) = (uint16_t)(SP16(esi + 0x34) + 1);
                SP16(edi + 0xa) = FS16(ebx + 0x21);
                clip_project_emit(FS32(ebx + 0x20), src, (int16_t)FS16(ebx + 2), ebp, edi, esi);
                edi += 0x10; ebp += 2; SP16(esi + 0x34) = (uint16_t)(SP16(esi + 0x34) + 1);
            } else {                                                /* single-vertex record */
                SP16(edi + 0xa) = FS16(ebx + 0xd);
                clip_project_emit(FS32(ebx + 0xc), src, (int16_t)FS16(ebx), ebp, edi, esi);
                edi += 0x10; ebp += 2; SP16(esi + 0x34) = (uint16_t)(SP16(esi + 0x34) + 1);
            }
        }
        ebx += 0x14;
    } while ((int8_t)(--cl) > 0);
    SP16(ebp) = (uint16_t)((uint32_t)(uintptr_t)start_edi - 0x484f9eu);   /* close the index ring */
    G8(VA_g_column_clip_mode) = G8(VA_g_span_clip_source);
    rwss_render_span_record(es, fs, gs);                            /* render via the NATIVE rasterizer (was call_orig 0x366cb) */
    #undef FS16
    #undef FS32
    #undef SP16
}

/* emit_world_span_clipped_indexed (0x2d4b5): the CLIPPED backward sibling of 0x2d2dd (mirrors 0x2d5b0 : 0x2d3d0).
 * Walks records cl-1..0 backward (pre-advance ebx, ebx -= 0x14), Y perspective-projected via clip_project_emit. Per
 * record (note the swapped call order): two-vert -> {extra +0x21, x +2, depth +0x20} then {extra +0xd, x +0, depth
 * +0xc}; single -> {extra +0xd, x +0, depth +0xc}. Then bridged 0x366cb. ABI_SPANEMIT. */
void emit_world_span_clipped_indexed(uint32_t eax_in, uint32_t ecx_in, uint32_t ebx_in,
                                            uint16_t es, uint16_t fs, uint16_t gs)
{
    extern uint32_t (*g_os_sel_base)(uint16_t);
    #ifndef ROTH_STANDALONE
    if (g_os_sel_base == NULL) {
        regs_t io; memset(&io, 0, sizeof io);
        io.va = 0x2d4b5u + OBJ_DELTA; io.eax = eax_in; io.ecx = ecx_in; io.ebx = ebx_in;  /* [ORACLE-FALLBACK] */
        io.es = es; io.fs = fs; io.gs = gs;
        call_orig(&io);
        return;
    }
    #endif
    uint32_t fs_base = g_os_sel_base(fs);
    #define FS16(o) (*(volatile uint16_t *)(uintptr_t)(fs_base + (uint16_t)(o)))   /* 16-bit fs: addressing */
    #define FS32(o) (*(volatile int32_t  *)(uintptr_t)(fs_base + (uint16_t)(o)))
    uint8_t *esi = (uint8_t *)(uintptr_t)(0x84f18u + OBJ_DELTA);
    #define SP16(p) (*(volatile uint16_t *)(p))
    G16(VA_g_world_span_record + 0xc) = (uint16_t)eax_in;
    G8(VA_g_column_clip_mode) = G8(VA_g_span_clip_source);                                      /* (also re-written before 0x366cb) */
    int32_t src = (int16_t)G16(VA_g_sprite_view_angle + 0x6);                            /* movsx [0x909fe] (fixed view-space Y) */
    uint8_t cl = (uint8_t)ecx_in;
    uint32_t ebx = ebx_in + ((uint32_t)cl - 1u) * 0x14u;            /* point at record cl-1 (backward walk) */
    uint8_t *ebp = esi + 0x36;
    uint8_t *edi = (uint8_t *)(uintptr_t)(0x84f9eu + OBJ_DELTA);
    uint8_t *start_edi = edi;
    SP16(esi + 0x34) = 0;
    SP16(esi + 0x28) = 0x7fff; SP16(esi + 0x2a) = 0x7fff;
    SP16(esi + 0x2c) = 0x8000; SP16(esi + 0x2e) = 0x8000;
    do {
        uint16_t ax = FS16(ebx);
        if (ax != 0x8000) {
            if (FS32(ebx + 0x20) < 0x1000) {                        /* two-vertex record (swapped emit order) */
                SP16(edi + 0xa) = FS16(ebx + 0x21);
                clip_project_emit(FS32(ebx + 0x20), src, (int16_t)FS16(ebx + 2), ebp, edi, esi);
                edi += 0x10; ebp += 2; SP16(esi + 0x34) = (uint16_t)(SP16(esi + 0x34) + 1);
                SP16(edi + 0xa) = FS16(ebx + 0xd);
                clip_project_emit(FS32(ebx + 0xc), src, (int16_t)FS16(ebx), ebp, edi, esi);
                edi += 0x10; ebp += 2; SP16(esi + 0x34) = (uint16_t)(SP16(esi + 0x34) + 1);
            } else {                                                /* single-vertex record */
                SP16(edi + 0xa) = FS16(ebx + 0xd);
                clip_project_emit(FS32(ebx + 0xc), src, (int16_t)FS16(ebx), ebp, edi, esi);
                edi += 0x10; ebp += 2; SP16(esi + 0x34) = (uint16_t)(SP16(esi + 0x34) + 1);
            }
        }
        ebx -= 0x14;
    } while ((int8_t)(--cl) > 0);
    SP16(ebp) = (uint16_t)((uint32_t)(uintptr_t)start_edi - 0x484f9eu);   /* close the index ring */
    G8(VA_g_column_clip_mode) = G8(VA_g_span_clip_source);
    rwss_render_span_record(es, fs, gs);                            /* render via the NATIVE rasterizer (was call_orig 0x366cb) */
    #undef FS16
    #undef FS32
    #undef SP16
}

/* =============================================================================================
 * draw_world_face_clipped_spans (0x2cbb0) + subtree — the SKY/PORTAL special-face span path (#4b).
 * Bridged twin of draw_world_face_projected_spans (0x2cf60); the face-list orchestrators (0x28dbe/0x2ad21)
 * call THIS instead when fs:[bx+0x12]&0x20 (special face). Same ABI_FACEWRAP (EAX=colour, EBX=fs:face record,
 * ESI=es:surface record, +es/fs/gs). All callees native: compute_face_span_extents (0x2c250), emit_world_span_
 * record (0x2d130), find_record_by_id (0x3d018); the 3 sub-fns (0x2c400/0x2cc48/0x2d040) + the door leaf
 * (0x3d03c) are lifted here as static helpers. The 4 corner-Y projections share fcs_proj_y. */

/* corner screen-Y = clamp_+-3ffe( num16 * g_perspective_scale[0x85288] / depth + view_center[0x909a4] ).
 * imul/idiv are SIGNED (64-bit product / 32-bit divide); num16 is a sign-extended 16-bit value. */
static uint16_t fcs_proj_y(int32_t num16, int32_t depth)
{
    int64_t prod = (int64_t)num16 * (int32_t)G32(VA_g_perspective_scale);
    int32_t v = (int32_t)(prod / depth) + (int32_t)G32(VA_g_span_src_wrap_reoffset + 0x28);
    if (v < -0x3ffe) v = -0x3ffe; else if (v >= 0x3ffe) v = 0x3ffe;
    return (uint16_t)v;
}

/* is_door_open (0x3d03c): DI=sector id -> AL=(record+2 & 2), *o_cf = CF (0=found, 1=not found).
 * Scans g_door_pool (count [0x8b3f4], base 0x8b3f8, stride 0x1f6). */
static uint8_t door_is_open_3d03c(uint16_t di, int *o_cf)
{
    uint8_t cnt = (uint8_t)G8(VA_g_door_count);
    uint32_t rec = 0x8b3f8;
    while (cnt != 0) {
        if ((uint16_t)G16(rec) == di) { *o_cf = 0; return (uint8_t)(G8(rec + 2) & 2); }
        rec += 0x1f6; cnt--;
    }
    *o_cf = 1; return 0;
}

/* project_wall_face_span_extents (0x2c400): es:si=surface rec, fs:bx=face rec. Band-clips the face's
 * world-Y extents [ax(top)..dx(bottom)] against the surface/sub-record windows, then projects the 4 corners
 * (near/far x top/bottom) into 0x852b0/ac/b2/ae. Returns 1 (CF=stc, reject) or 0 (CF=clc, ok). Per-flag
 * depth override is INDEPENDENT here (flags&4 -> depthA=0x1000; flags&8 -> depthB=0x1000). */
int project_wall_face_span_extents_2c400(uint32_t esi_in, uint32_t ebx_in,
                                         uint32_t es_base, uint32_t fs_base)
{
    #define ES16(o) (*(volatile uint16_t *)(uintptr_t)(es_base + (uint16_t)(o)))
    #define FS16(o) (*(volatile uint16_t *)(uintptr_t)(fs_base + (uint16_t)(o)))
    #define FS32(o) (*(volatile int32_t  *)(uintptr_t)(fs_base + (uint16_t)(o)))
    uint16_t di = ES16(ES16(esi_in + 8) + 6);
    di = ES16(di + 0x18);
    uint16_t si2 = ES16(esi_in + 6);
    int16_t ax = (int16_t)ES16(di + 2);
    if (ax >= (int16_t)ES16(di + 8))  return 1;           /* 2c426 */
    if (ax >= (int16_t)ES16(si2))     return 1;           /* 2c431 */
    if (ax <  (int16_t)ES16(si2 + 2)) ax = (int16_t)ES16(si2 + 2);   /* 2c43b */
    int16_t dx = (int16_t)ES16(di + 8);
    if (dx <= (int16_t)ES16(si2 + 2)) return 1;           /* 2c44c */
    if (dx >  (int16_t)ES16(si2))     dx = (int16_t)ES16(si2);       /* 2c457 */
    uint16_t bx2 = ES16(si2 + 0x18);                      /* 2c464 sub-record clip */
    if (bx2 != 0) {
        int16_t b8 = (int16_t)ES16(bx2 + 8), b2 = (int16_t)ES16(bx2 + 2);
        if (ax < b8 && dx > b2) {
            if (ax >= b2) ax = b8;
            if (dx <= b8) dx = b2;
        }
    }
    if (ax >= dx) return 1;                                /* 2c4a2 */
    uint16_t center = (uint16_t)G16(VA_g_secondary_surface_count + 0x4);
    G16(VA_g_face_span_top) = (uint16_t)dx;
    G16(VA_g_face_span_top + 0x6) = (uint16_t)(0u - (uint16_t)((uint16_t)dx - center));
    int16_t num_dx = (int16_t)G16(VA_g_face_span_top + 0x6);
    G16(VA_g_face_span_bottom) = (uint16_t)ax;
    int16_t num_ax = (int16_t)(uint16_t)(0u - (uint16_t)((uint16_t)ax - center));
    int32_t depthA = FS32(ebx_in + 0xc);  if (FS16(ebx_in + 0x12) & 4) depthA = 0x1000;
    int32_t depthB = FS32(ebx_in + 0x20); if (FS16(ebx_in + 0x12) & 8) depthB = 0x1000;
    G16(VA_g_map_geometry_selector + 0x1c) = fcs_proj_y(num_ax, depthA);            /* near-top */
    G16(VA_g_map_geometry_selector + 0x18) = fcs_proj_y(num_dx, depthA);            /* near-bottom */
    G16(VA_g_map_geometry_selector + 0x1e) = fcs_proj_y(num_ax, depthB);            /* far-top */
    G16(VA_g_map_geometry_selector + 0x1a) = fcs_proj_y(num_dx, depthB);            /* far-bottom */
    return 0;
    #undef ES16
    #undef FS16
    #undef FS32
}

/* build_world_face_edge_spans (0x2cc48): emits up to TWO edge spans (top edge -> kind 1, bottom edge ->
 * kind 2) for the face. es:si=surface rec, fs:bx=face rec, AX=colour. The face-id pair is the dword
 * es:[es:[si+4]+4] (low word = top edge id, high word = bottom edge id). depth override: flags&4 ->
 * depthA=0x1000; flags&8(only) -> depthB=0x1000 (NOTE: &4 takes precedence, unlike 0x2c400). */
void build_world_face_edge_spans_2cc48(uint16_t colour_ax, uint32_t esi_in, uint32_t ebx_in,
                                       uint16_t es, uint16_t fs, uint16_t gs,
                                       uint32_t fs_base, uint32_t es_base)  /* 0x2cc48 */
{
    #define ES16(o) (*(volatile uint16_t *)(uintptr_t)(es_base + (uint16_t)(o)))
    #define ES32(o) (*(volatile int32_t  *)(uintptr_t)(es_base + (uint16_t)(o)))
    #define ES8(o)  (*(volatile uint8_t  *)(uintptr_t)(es_base + (uint16_t)(o)))
    #define FS16(o) (*(volatile uint16_t *)(uintptr_t)(fs_base + (uint16_t)(o)))
    #define FS32(o) (*(volatile int32_t  *)(uintptr_t)(fs_base + (uint16_t)(o)))
    G16(VA_g_span_gouraud_colour_b) = colour_ax;
    G16(VA_g_span_gouraud_colour_a) = FS16(ebx_in);
    uint32_t edx = (uint32_t)ES32(ES16(esi_in + 4) + 4);       /* two face ids */
    uint16_t di = ES16(esi_in + 8);
    if (di == 0xffff) return;
    di = ES16(di + 6);                                         /* height sub-record */

    int32_t depthA = FS32(ebx_in + 0xc);
    int32_t depthB = FS32(ebx_in + 0x20);
    G32(VA_g_span_fill_mode_word + 0xa9) = (uint32_t)depthA;                           /* stored BEFORE override */
    G32(VA_g_span_fill_mode_word + 0x99) = (uint32_t)depthB;
    G16(VA_g_world_span_record + 0x14) = 0;
    uint16_t f12 = FS16(ebx_in + 0x12);
    if (f12 & 0xc) {
        G16(VA_g_world_span_record + 0x14) = 0x2000;
        if (f12 & 4) depthA = 0x1000;                          /* depthB stays */
        else         depthB = 0x1000;                          /* depthA stays */
    }
    uint16_t center = (uint16_t)G16(VA_g_secondary_surface_count + 0x4);

    /* --- top edge (span-kind 1) --- */
    int16_t bx = (int16_t)(uint16_t)(0u - (uint16_t)((uint16_t)ES16(di) - center));
    int16_t clip_top = (int16_t)G16(VA_g_world_span_top);
    if (bx > clip_top) {                                       /* 2cce3 jle skips */
        if (bx >= (int16_t)G16(VA_g_world_span_bottom)) bx = (int16_t)G16(VA_g_world_span_bottom);   /* 2cce9 clamp */
        G16(VA_g_span_src_wrap_reoffset + 0xa) = (uint16_t)clip_top;
        G16(VA_g_world_span_record + 0xc) = (uint16_t)edx;                          /* dx = low word = top edge id */
        if ((uint16_t)edx != G16(VA_g_world_span_colorkey)) {                   /* colorkey */
            ES8(esi_in + 0xa) = (uint8_t)(ES8(esi_in + 0xa) | 0x40);
            G16(VA_g_map_geometry_selector + 0x10) = fcs_proj_y(clip_top, depthA);
            G16(VA_g_map_geometry_selector + 0x12) = fcs_proj_y(clip_top, depthB);
            G16(VA_g_map_geometry_selector + 0x14) = fcs_proj_y(bx, depthA);
            G16(VA_g_map_geometry_selector + 0x16) = fcs_proj_y(bx, depthB);
            G16(VA_g_span_fill_mode_word + 0xe) = (uint16_t)((uint16_t)bx - (uint16_t)clip_top);
            G8(VA_g_turn_view_scale_state + 0x2) = 1;
            emit_world_span_record(es, fs, gs);
            G8(VA_g_parallax_sky_active) = 0;
        }
    }

    /* --- bottom edge (span-kind 2) --- */
    int16_t bbx = (int16_t)(uint16_t)(0u - (uint16_t)((uint16_t)ES16(di + 2) - center));
    int16_t clip_bot = (int16_t)G16(VA_g_world_span_bottom);
    if (bbx < clip_bot) {                                      /* 2ce29 jge exits */
        if (bbx <= (int16_t)G16(VA_g_world_span_top)) bbx = (int16_t)G16(VA_g_world_span_top);  /* 2ce2f clamp up */
        G16(VA_g_span_src_wrap_reoffset + 0xa) = (uint16_t)bbx;
        uint16_t face2 = (uint16_t)(edx >> 16);                /* ror edx,0x10 -> high word */
        G16(VA_g_world_span_record + 0xc) = face2;
        if (face2 != G16(VA_g_world_span_colorkey)) {
            ES8(esi_in + 0xa) = (uint8_t)(ES8(esi_in + 0xa) | 0x40);
            G16(VA_g_map_geometry_selector + 0x14) = fcs_proj_y(clip_bot, depthA);
            G16(VA_g_map_geometry_selector + 0x16) = fcs_proj_y(clip_bot, depthB);
            G16(VA_g_map_geometry_selector + 0x10) = fcs_proj_y(bbx, depthA);
            G16(VA_g_map_geometry_selector + 0x12) = fcs_proj_y(bbx, depthB);
            G16(VA_g_span_fill_mode_word + 0xe) = (uint16_t)(0u - (uint16_t)((uint16_t)bbx - (uint16_t)clip_bot));
            G8(VA_g_turn_view_scale_state + 0x2) = 2;
            emit_world_span_record(es, fs, gs);
        }
    }
    #undef ES16
    #undef ES32
    #undef ES8
    #undef FS16
    #undef FS32
}

/* finalize_world_span_overlay (0x2d040): overlay/finalize pass after the base. Door/record gating
 * (skip if a door record matches, or a find_record_by_id hit), then copies the projected corners into the
 * span-record globals and emits once. es:si=surface rec, fs:bx=face rec. */
void finalize_world_span_overlay_2d040(uint32_t esi_in, uint32_t ebx_in,
                                       uint16_t es, uint16_t fs, uint16_t gs,
                                       uint32_t fs_base, uint32_t es_base)  /* 0x2d040 */
{
    #define ES16(o) (*(volatile uint16_t *)(uintptr_t)(es_base + (uint16_t)(o)))
    #define ES8(o)  (*(volatile uint8_t  *)(uintptr_t)(es_base + (uint16_t)(o)))
    #define FS16(o) (*(volatile uint16_t *)(uintptr_t)(fs_base + (uint16_t)(o)))
    #define FS32(o) (*(volatile int32_t  *)(uintptr_t)(fs_base + (uint16_t)(o)))
    G8(VA_g_turn_view_scale_state + 0x2) = 0;
    uint16_t di = ES16(esi_in + 6);
    if ((uint16_t)ES16(di + 0x14) >= 0xfffe) {                 /* 2d04d jb skips door check */
        int cf; (void)door_is_open_3d03c(di, &cf);             /* DI = es:[si+6] */
        if (cf == 0) return;                                   /* door record found -> skip */
        if ((uint16_t)ES16(di + 0x14) == 0xfffe) return;       /* 2d05e */
    }
    di = ES16(ES16(esi_in + 8) + 6);
    if ((uint16_t)ES16(di + 0x14) >= 0xfffe) {                 /* 2d073 */
        if (find_record_by_id(di) == 0) return;         /* CF=0 (found) -> skip */
    }
    G32(VA_g_map_geometry_selector + 0x10) = (uint32_t)G32(VA_g_map_geometry_selector + 0x18);                     /* 2d088: corners -> span-record globals */
    G32(VA_g_map_geometry_selector + 0x14) = (uint32_t)G32(VA_g_map_geometry_selector + 0x1c);
    G16(VA_g_span_src_wrap_reoffset + 0xa) = G16(VA_g_face_span_top + 0x6);
    uint16_t face = ES16(ES16(esi_in + 4) + 2);
    G16(VA_g_world_span_record + 0xc) = face;
    if (face == G16(VA_g_world_span_colorkey)) return;                          /* colorkey */
    G16(VA_g_span_fill_mode_word + 0xe) = (uint16_t)(G16(VA_g_face_span_top) - G16(VA_g_face_span_bottom));
    G32(VA_g_span_fill_mode_word + 0xa9) = (uint32_t)FS32(ebx_in + 0xc);
    G32(VA_g_span_fill_mode_word + 0x99) = (uint32_t)FS32(ebx_in + 0x20);
    G16(VA_g_world_span_record + 0x14) = 0;
    if (FS16(ebx_in + 0x12) & 0xc) G16(VA_g_world_span_record + 0x14) = 0x2000;
    uint32_t saved_852bc = (uint32_t)G32(VA_g_face_span_top + 0x2);
    G32(VA_g_face_span_top + 0x2) = (uint32_t)G32(VA_g_face_span_bottom);
    ES8(esi_in + 0xa) = (uint8_t)(ES8(esi_in + 0xa) | 0x40);
    emit_world_span_record(es, fs, gs);
    G32(VA_g_face_span_top + 0x2) = (int32_t)saved_852bc;
    #undef ES16
    #undef ES8
    #undef FS16
    #undef FS32
}

/* draw_world_face_clipped_spans (0x2cbb0): THE entry (ABI_FACEWRAP). [0x90a2e]&1 -> base+overlay multipass
 * (compute_face_span_extents + masked build + finalize); else an es:/fs:-addressed reject ladder
 * (di sub-record == 0, surface colorkey, 0x2c400 projection CF) where each reject tail-calls the builder,
 * and the pass-through case runs the masked build + finalize. */
void draw_world_face_clipped_spans(uint32_t eax_in, uint32_t ebx_in, uint32_t esi_in,
                                          uint16_t es, uint16_t fs, uint16_t gs)
{
    extern uint32_t (*g_os_sel_base)(uint16_t);
    #ifndef ROTH_STANDALONE
    if (g_os_sel_base == NULL) {
        regs_t io; memset(&io, 0, sizeof io);
        io.va = 0x2cbb0u + OBJ_DELTA; io.eax = eax_in; io.ebx = ebx_in; io.esi = esi_in;  /* [ORACLE-FALLBACK] */
        io.es = es; io.fs = fs; io.gs = gs;
        call_orig(&io);
        return;
    }
    #endif
    uint32_t fs_base = g_os_sel_base(fs);
    uint32_t es_base = g_os_sel_base(es);
    #define ES16(o) (*(volatile uint16_t *)(uintptr_t)(es_base + (uint16_t)(o)))
    uint16_t colour = (uint16_t)eax_in;

    if (G8(VA_g_current_surface_render_flags) & 1) {                                      /* 0x2cbb0: base+overlay multipass */
        G8(VA_g_turn_view_scale_state + 0x2) = 3;
        compute_face_span_extents(esi_in, ebx_in, es, fs);   /* 0x2c250 (verified) */
        uint32_t saved = (uint32_t)G32(VA_g_current_surface_render_flags);               /* push dword [0x90a2e] */
        G8(VA_g_current_surface_render_flags) = (uint8_t)(G8(VA_g_current_surface_render_flags) & 0xfa);
        build_world_face_edge_spans_2cc48(colour, esi_in, ebx_in, es, fs, gs, fs_base, es_base);
        G32(VA_g_current_surface_render_flags) = (int32_t)saved;                         /* pop dword [0x90a2e] */
        finalize_world_span_overlay_2d040(esi_in, ebx_in, es, fs, gs, fs_base, es_base);
        G8(VA_g_current_surface_render_flags) = (uint8_t)(G8(VA_g_current_surface_render_flags) & 0xfe);
        return;
    }
    /* 0x2cbec: reject ladder (each reject tail-calls the builder) */
    uint16_t di = ES16(ES16(esi_in + 8) + 6);
    if (ES16(di + 0x18) != 0 &&                                                 /* 0x2cbf8 */
        ES16(ES16(esi_in + 4) + 2) != G16(VA_g_world_span_colorkey) &&                           /* 0x2cc02 surface colorkey */
        project_wall_face_span_extents_2c400(esi_in, ebx_in, es_base, fs_base) == 0) {  /* 0x2cc19 CF clear */
        uint16_t saved16 = (uint16_t)G16(VA_g_current_surface_render_flags);             /* push word [0x90a2e] */
        G8(VA_g_current_surface_render_flags) = (uint8_t)(G8(VA_g_current_surface_render_flags) & 0xfa);
        build_world_face_edge_spans_2cc48(colour, esi_in, ebx_in, es, fs, gs, fs_base, es_base);
        G16(VA_g_current_surface_render_flags) = saved16;                                /* pop word [0x90a2e] */
        finalize_world_span_overlay_2d040(esi_in, ebx_in, es, fs, gs, fs_base, es_base);
        G8(VA_g_current_surface_render_flags) = (uint8_t)(G8(VA_g_current_surface_render_flags) & 0xfe);
    } else {
        build_world_face_edge_spans_2cc48(colour, esi_in, ebx_in, es, fs, gs, fs_base, es_base);
    }
    #undef ES16
}

/* draw_world_face_projected_spans (0x2cf60, + its tail-jmp continuation block 0x2d088): the per-face span DISPATCHER.
 * Stashes the projected face vertices (fs:[bx+..] / the input eax) into the 0x852xx + 0x84fxx render globals, then:
 *   (a) if [0x90a2e] bits 0x01 AND 0x08 are BOTH set -> SPLIT face: compute_face_span_extents (0x2c250), then a SECOND
 *       projection setup (orig block 0x2d088: overwrite 0x852a4/0x852a8 from the dword pair 0x852ac/0x852b0, take
 *       0x90986 from 0x852c0, [0x84f3c]=0x852ba-0x852b8, and a temporary 0x852bc<-0x852b8 swap held only across the
 *       render) then emit_world_span_record (0x2d130), else
 *   (b) single emit_world_span_record (0x2d130).
 * Early-out with NO render when the face id es:[es:[si+4]+2] == [0x90a2c]. Both callees are verified-native; the
 * deepest rasterizer 0x366cb is bridged inside 0x2d130. Inputs: EAX (face value -> 0x852c4), EBX (fs: face record),
 * ESI (es: vertex record), + es/fs/gs (gs only forwarded to the render). 16-bit es:/fs: addressing (mask offsets). */
void draw_world_face_projected_spans(uint32_t eax_in, uint32_t ebx_in, uint32_t esi_in,
                                            uint16_t es, uint16_t fs, uint16_t gs)
{
    extern uint32_t (*g_os_sel_base)(uint16_t);
    #ifndef ROTH_STANDALONE
    if (g_os_sel_base == NULL) {
        regs_t io; memset(&io, 0, sizeof io);
        io.va = 0x2cf60u + OBJ_DELTA; io.eax = eax_in; io.ebx = ebx_in; io.esi = esi_in;  /* [ORACLE-FALLBACK] */
        io.es = es; io.fs = fs; io.gs = gs;
        call_orig(&io);
        return;
    }
    #endif
    uint32_t fs_base = g_os_sel_base(fs);
    uint32_t es_base = g_os_sel_base(es);
    #define FS16(o) (*(volatile uint16_t *)(uintptr_t)(fs_base + (uint16_t)(o)))   /* 16-bit fs: addressing */
    #define FS32(o) (*(volatile int32_t  *)(uintptr_t)(fs_base + (uint16_t)(o)))
    #define ES16(o) (*(volatile uint16_t *)(uintptr_t)(es_base + (uint16_t)(o)))   /* 16-bit es: addressing */
    #define ES8(o)  (*(volatile uint8_t  *)(uintptr_t)(es_base + (uint16_t)(o)))

    G8(VA_g_turn_view_scale_state + 0x2)  = 0;
    G16(VA_g_span_gouraud_colour_b) = (uint16_t)eax_in;                                /* caller's ax */
    G16(VA_g_span_gouraud_colour_a) = FS16(ebx_in);
    G16(VA_g_map_geometry_selector + 0x10) = FS16(ebx_in + 4);
    G16(VA_g_map_geometry_selector + 0x14) = FS16(ebx_in + 6);
    G16(VA_g_map_geometry_selector + 0x12) = FS16(ebx_in + 0x18);
    G16(VA_g_span_src_wrap_reoffset + 0xa) = G16(VA_g_world_span_top);
    G16(VA_g_map_geometry_selector + 0x16) = FS16(ebx_in + 0x1a);

    uint16_t di      = ES16(esi_in + 4);
    uint16_t face_id = ES16(di + 2);
    G16(VA_g_world_span_record + 0xc) = face_id;
    if (face_id == G16(VA_g_world_span_colorkey))                                    /* face already emitted -> no render */
        return;

    uint8_t flags = G8(VA_g_current_surface_render_flags);
    if ((flags & 1) && (flags & 8)) {                              /* SPLIT face */
        compute_face_span_extents(esi_in, ebx_in, es, fs);  /* 0x2c250 (verified) */

        G32(VA_g_map_geometry_selector + 0x10) = (uint32_t)G32(VA_g_map_geometry_selector + 0x18);                     /* block 0x2d088: 2nd projection setup */
        G32(VA_g_map_geometry_selector + 0x14) = (uint32_t)G32(VA_g_map_geometry_selector + 0x1c);
        G16(VA_g_span_src_wrap_reoffset + 0xa) = G16(VA_g_face_span_top + 0x6);
        uint16_t di2      = ES16(esi_in + 4);
        uint16_t face_id2 = ES16(di2 + 2);
        G16(VA_g_world_span_record + 0xc) = face_id2;
        if (face_id2 == G16(VA_g_world_span_colorkey))
            return;
        G16(VA_g_span_fill_mode_word + 0xe) = (uint16_t)(G16(VA_g_face_span_top) - G16(VA_g_face_span_bottom));
        G32(VA_g_span_fill_mode_word + 0xa9) = (uint32_t)FS32(ebx_in + 0xc);
        G32(VA_g_span_fill_mode_word + 0x99) = (uint32_t)FS32(ebx_in + 0x20);
        G16(VA_g_world_span_record + 0x14) = 0;
        if (FS16(ebx_in + 0x12) & 0xc)
            G16(VA_g_world_span_record + 0x14) = 0x2000;
        uint32_t saved_852bc = (uint32_t)G32(VA_g_face_span_top + 0x2);             /* push [0x852bc] */
        G32(VA_g_face_span_top + 0x2) = (uint32_t)G32(VA_g_face_span_bottom);
        ES8(esi_in + 0xa) = (uint8_t)(ES8(esi_in + 0xa) | 0x40);
        emit_world_span_record(es, fs, gs);                 /* 0x2d130 (-> bridged 0x366cb) */
        G32(VA_g_face_span_top + 0x2) = saved_852bc;                                /* pop [0x852bc] */
    } else {                                                       /* single face emit */
        ES8(esi_in + 0xa) = (uint8_t)(ES8(esi_in + 0xa) | 0x40);
        G16(VA_g_span_fill_mode_word + 0xe) = G16(VA_g_span_gouraud_colour_b + 0x2);
        G32(VA_g_span_fill_mode_word + 0xa9) = (uint32_t)FS32(ebx_in + 0xc);
        G32(VA_g_span_fill_mode_word + 0x99) = (uint32_t)FS32(ebx_in + 0x20);
        G16(VA_g_world_span_record + 0x14) = 0;
        if (FS16(ebx_in + 0x12) & 0xc)
            G16(VA_g_world_span_record + 0x14) = 0x2000;
        emit_world_span_record(es, fs, gs);                 /* 0x2d130 (-> bridged 0x366cb) */
    }
    #undef FS16
    #undef FS32
    #undef ES16
    #undef ES8
}

/* render_secondary_surface_pass_clipped (0x2b3fa): the secondary-surface pass ALTERNATOR. Its only own logic is the
 * byte test `[0x853d2] & 2`: if set, tail-jmp to pass1 (0x2b36f); else fall through into pass2's body (0x2b407). Both
 * targets are verified-native (each renders the 0x84b18 surface list via bridged 0x2bc3c and handles its own
 * selectors), and 0x2b3fa itself derefs no es:/fs: memory — so this needs no selector base. ABI_SECPASS. */
void render_secondary_surface_pass_clipped(uint16_t es, uint16_t fs, uint16_t gs)
{
    if (G8(VA_g_secondary_subpass_flags) & 2)
        render_secondary_surface_pass1(es, fs, gs);   /* jne 0x2b36f */
    else
        render_secondary_surface_pass2(es, fs, gs);   /* fall through into 0x2b407 */
}

/* render_world_face_list_subpass (0x28dbe + its out-of-line tail 0x292b4): the FACE-LIST SUBPASS orchestrator. Builds
 * the per-frame draw list (build_scene_draw_list 0x2a0a0) then walks it — an OUTER loop over the draw-order list at
 * fs:[0xfc00] (descending) x a CHAIN loop (fs:[bx] linked list) x per-face work: two edge-span emitters (top via
 * 0x2d5b0, bottom via 0x2d3d0), an INNER face loop calling the per-face dispatcher 0x2cf60 (or the rare special-face
 * path 0x2cbb0, now NATIVE), then a secondary-surface section (build_secondary_surface_list 0x2b298 + pass1/pass2 0x2b36f
 * /0x2b407 + clipped emitters 0x2d4b5/0x2d2dd + the pass alternator 0x2b3fa / list 0x2b333). EVERY callee is verified-
 * native (0x2cbb0 sky/portal special face lifted W24-2). Loads its own fs=[0x85294]/es=
 * [0x852c8] selectors; gs + the build's ecx are inherited from the caller. 16-bit es:/fs: addressing. (ABI_SUBPASS) */
void render_world_face_list_subpass(uint32_t ecx, uint16_t gs)
{
    extern uint32_t (*g_os_sel_base)(uint16_t);
    #ifndef ROTH_STANDALONE
    if (g_os_sel_base == NULL) {
        regs_t io; memset(&io, 0, sizeof io);
        io.va = 0x28dbeu + OBJ_DELTA; io.ecx = ecx; io.gs = gs;  /* [ORACLE-FALLBACK] */
        call_orig(&io);
        return;
    }
    #endif
    /* ---- setup 0x28dbe..0x28dea ---- */
    G16(VA_g_world_render_subpass_kind) = 0xff;                                          /* 0x28dbe */
    G16(VA_g_secondary_surface_count + 0x4) = G16(VA_g_sector_cull_coord);                                  /* 0x28dc7 */
    if ((int16_t)G16(VA_g_sector_cull_coord + 0x8) <= 0) return;                       /* 0x28dd3: cmp [0x85302],0; jle ret */
    G16(VA_g_reflection_view_list + 0x84) = 0;                                             /* 0x28de1 */
    G8(VA_g_render_sector_walk_mode + 0x25)  = 1;                                             /* 0x28dea */
    build_scene_draw_list(ecx);                            /* 0x28df1: call 0x2a0a0 */

    uint16_t fs = G16(VA_g_map_geometry_selector), es = G16(VA_g_surface_record_selector);                /* 0x28df6/0x28dfd: load selectors */
    uint32_t fs_base = g_os_sel_base(fs);
    uint32_t es_base = g_os_sel_base(es);
    #define FS16(o) (*(volatile uint16_t *)(uintptr_t)(fs_base + (uint16_t)(o)))   /* 16-bit fs: addressing */
    #define ES16(o) (*(volatile uint16_t *)(uintptr_t)(es_base + (uint16_t)(o)))   /* 16-bit es: addressing */
    #define ES8(o)  (*(volatile uint8_t  *)(uintptr_t)(es_base + (uint16_t)(o)))

    uint32_t count = FS16(0xfc00);                               /* 0x28e09: cx = fs:[0xfc00] */
    uint32_t edi = 0xfc00u + 2u * count;                         /* 0x28e0d: edi += ecx; edi += ecx */

    for (uint32_t oc = count; oc != 0; oc--, edi -= 2) {         /* OUTER loop 0x28e11 */
        uint32_t chain = FS16(edi);                             /* 0x28e13: bx = fs:[di] */
        chain = FS16(chain + 6);                                /* 0x28e18: bx = fs:[bx+6] (face-list head) */
        while (chain != 0) {                                    /* CHAIN loop 0x28e1e / advance 0x29298 */
            uint32_t face_si = FS16(chain + 2);                 /* 0x28e1e: si = fs:[bx+2] */
            if ((int16_t)(uint16_t)face_si == -1)               /* 0x28e24: cmp si,-1; je advance */
                goto chain_advance;

            G16(VA_g_view_bound_left) = FS16(chain + 4);                     /* 0x28e2e: [0x9096a]=fs:[bx+4] */
            uint16_t edx_v = FS16(chain + 4);                   /*  dx (persists for the inner cmp) */
            uint16_t ebp_v = FS16(chain + 6);                   /* 0x28e3b: bp=fs:[bx+6] */
            G16(VA_g_view_bound_right) = ebp_v;                               /*  [0x90968]=bp */

            uint32_t edge = ES16(face_si + 4);                  /* 0x28e49: bx=es:[si+4] */
            if (edge == 0)                                      /* 0x28e51: or ebx,ebx; je advance */
                goto chain_advance;

            uint32_t cl = ES8(face_si + 0xd);                   /* 0x28e59: cl=es:[si+0xd] */
            G8(VA_g_column_clip_mode) = 0; G8(VA_g_span_clip_source) = 0;                   /* 0x28e5e */
            G16(VA_g_world_alt_render_flags) = (uint16_t)(G16(VA_g_world_alt_render_flags) & 0x7fff);   /* 0x28e6a */
            G8(VA_g_span_blend_mode_flag) = 0;                                    /* 0x28e73 */
            G16(VA_g_map_das_fat_buffer + 0xa) = (uint16_t)face_si;                   /* 0x28e7b */

            /* ---- block A (top edge) 0x28e82 ---- */
            uint16_t axA = (uint16_t)(G16(VA_g_sector_cull_coord) - ES16(face_si));   /* ax=es:[si]; sub [0x852fa]; neg (low16) */
            G16(VA_g_world_span_top) = axA;
            if ((int16_t)axA < 0) {                             /* 0x28e96: jge 0x28f29 skips (run when <0) */
                G16(VA_g_sprite_view_angle + 0x6) = axA;                             /* 0x28e9f */
                uint16_t a6 = ES16(face_si + 6);                /* 0x28ea5: ax=es:[si+6] */
                if ((int16_t)a6 < 0) {                          /* 0x28eae: jns 0x28ed0 (skip when >=0) */
                    G16(VA_g_span_src_wrap_reoffset + 0x10) = 1; G16(VA_g_span_fill_mode_word) = 0x30; G8(VA_g_world_render_subpass_kind) = 1;            /* 0x28eb0 */
                    emit_world_span_unclipped_indexed(a6, cl, edge, es, fs, gs);/* 0x28ec9: 0x2d5b0 */
                } else if (a6 != G16(VA_g_world_span_colorkey)) {                /* 0x28ed0: cmp ax,[0x90a2c]; je 0x28f29 */
                    uint16_t dxs = ((int16_t)ES16(face_si + 0x14) == -3) ? 0 : ES16(face_si + 0x10); /* 0x28edb */
                    G8(VA_g_view_offset_y + 0x3) = (uint8_t)dxs; G8(VA_g_view_offset_y + 0x5) = (uint8_t)(dxs >> 8);     /* 0x28eea */
                    G16(VA_g_span_src_wrap_reoffset + 0x10) = (uint16_t)((ES8(face_si + 0xa) & 0xc) >> 2);        /* 0x28ef6 */
                    G8(VA_g_world_render_subpass_kind) = 1; G16(VA_g_span_fill_mode_word) = 0x38;                              /* 0x28f03/0x28f11 */
                    emit_world_span_unclipped_indexed(a6, cl, edge, es, fs, gs);/* 0x28f1a: 0x2d5b0 */
                    G32(VA_g_view_offset_y + 0x2) = 0;                           /* 0x28f1f */
                }
            }

            /* ---- block B (bottom edge) 0x28f29 ---- */
            uint16_t axB = (uint16_t)(G16(VA_g_sector_cull_coord) - ES16(face_si + 2));   /* ax=es:[si+2]; sub; neg */
            G16(VA_g_world_span_bottom) = axB;
            if ((int16_t)axB > 0) {                             /* 0x28f3e: jle 0x28fc1 skips (run when >0) */
                G16(VA_g_sprite_view_angle + 0x6) = axB;                             /* 0x28f43 */
                uint16_t a8 = ES16(face_si + 8);                /* 0x28f49: ax=es:[si+8] */
                if ((int16_t)a8 < 0) {                          /* 0x28f52: jns 0x28f74 (skip when >=0) */
                    G16(VA_g_span_fill_mode_word) = 0xb0; G16(VA_g_span_src_wrap_reoffset + 0x10) = 1; G8(VA_g_world_render_subpass_kind) = 2;            /* 0x28f54 */
                    emit_world_span_unclipped(a8, cl, edge, es, fs, gs);        /* 0x28f6d: 0x2d3d0 */
                } else if (a8 != G16(VA_g_world_span_colorkey)) {                /* 0x28f74: cmp; je 0x28fc1 */
                    uint16_t dx12 = ES16(face_si + 0x12);       /* 0x28f7d */
                    G8(VA_g_view_offset_y + 0x3) = (uint8_t)dx12; G8(VA_g_view_offset_y + 0x5) = (uint8_t)(dx12 >> 8);
                    G16(VA_g_span_src_wrap_reoffset + 0x10) = (uint16_t)((ES8(face_si + 0xa) & 0x30) >> 4);       /* 0x28f8f */
                    G16(VA_g_span_fill_mode_word) = 0xb8; G8(VA_g_world_render_subpass_kind) = 2;                              /* 0x28fa2 */
                    emit_world_span_unclipped(a8, cl, edge, es, fs, gs);        /* 0x28fb2: 0x2d3d0 */
                    G32(VA_g_view_offset_y + 0x2) = 0;                           /* 0x28fb7 */
                }
            }

            /* ---- per-face setup 0x28fc2 ---- */
            G16(VA_g_perspective_scale + 0x4) = (uint16_t)face_si;                   /* 0x28fc2 */
            G8(VA_g_anim_clock + 0x6)  = ES8(face_si + 0x16);                 /* 0x28fc9 */
            G16(VA_g_span_gouraud_colour_b + 0x2) = (uint16_t)(ES16(face_si) - ES16(face_si + 2));   /* 0x28fd5 */
            uint32_t esi_in = ES16(face_si + 0xe);              /* 0x28fe6: si = es:[si+0xe] (inner vtx record) */
            uint32_t ebx_in = edge;                             /* inner face/edge record (= es:[face_si+4]) */

            /* ---- inner face loop 0x28fec (do-while: runs >=1) ---- */
            uint8_t ic = (uint8_t)cl;
            do {
                int do_render = 1;
                if (!(FS16(ebx_in + 0x12) & 1)) do_render = 0;                      /* 0x28fec */
                else if ((int16_t)FS16(ebx_in) >= (int16_t)ebp_v) do_render = 0;     /* 0x28ffa: cmp ax,bp; jge */
                else if ((int16_t)FS16(ebx_in + 2) <= (int16_t)edx_v) do_render = 0; /* 0x29008: cmp ax,dx; jle */
                if (do_render) {
                    uint32_t newbx = ES16(esi_in + 4);          /* 0x29019: bx=es:[si+4] */
                    G16(VA_g_map_das_fat_buffer + 0x8) = (uint16_t)esi_in;            /* 0x2901f */
                    G8(VA_g_current_surface_render_flags) = ES8(newbx + 8);               /* 0x29026 */
                    uint16_t esbx = ES16(newbx);                /* 0x29030: ax=es:[bx] */
                    if ((int16_t)esbx < 0) {                    /* jns 0x29061 (skip when >=0) */
                        G16(VA_g_span_src_wrap_reoffset + 0x2c) = (uint16_t)(2 * ES8(newbx + 0xa));   /* 0x2903a */
                        G16(VA_g_span_src_wrap_reoffset + 0x2e) = ES8(newbx + 0xb);        /* 0x2904a */
                        G16(VA_g_span_fill_mode_word + 0x10) = (uint16_t)(ES16(newbx) & 0xfff);    /* 0x29057 */
                    } else {
                        G16(VA_g_span_fill_mode_word + 0x10) = esbx;                    /* 0x29061 (jns path) */
                    }
                    G16(VA_g_span_src_wrap_reoffset + 0x12) = (uint16_t)(FS16(ebx_in + 9) - FS16(ebx_in + 0x1d)); /* 0x29068 */
                    if (G8(VA_g_current_surface_render_flags) & 0x40) G8(VA_g_parallax_sky_active) = 0xff; /* 0x2907b/0x29084 */
                    G8(VA_g_world_render_subpass_kind) = 3;                            /* 0x2908b */
                    if (FS16(ebx_in + 0x12) & 0x20) {           /* 0x29092: jne 0x292b4 (special face) */
                        draw_world_face_clipped_spans(FS16(ebx_in + 2), ebx_in, esi_in, es, fs, gs); /* 0x292b4 (NATIVE; was call_orig 0x2cbb0) */
                    } else {
                        draw_world_face_projected_spans(FS16(ebx_in + 2), ebx_in, esi_in, es, fs, gs); /* 0x290a0 */
                    }
                }
                G32(VA_g_span_src_wrap_reoffset + 0x2c) = 0;                               /* 0x290a5 (skip + render both reach) */
                G8(VA_g_parallax_sky_active)  = 0;                               /* 0x290af */
                esi_in += 0xc; ebx_in += 0x14;                  /* 0x290b6 */
            } while ((int8_t)(--ic) > 0);                       /* 0x290bc: dec cl; jg */

            /* ---- secondary surfaces 0x290c4 ---- */
            G8(VA_g_current_surface_render_flags) = 0;                                    /* 0x290c4 */
            G8(VA_g_has_secondary_surfaces) = 0;                                    /* 0x290cb */
            {
                uint32_t e2 = G32(VA_g_perspective_scale + 0x4);                     /* 0x290d2: esi=[0x8528c] */
                e2 = (e2 & 0xffff0000u) | ES16(e2 + 4);         /* 0x290d8: si=es:[esi+4] */
                if (e2 != 0) {                                  /* 0x290dd: or esi,esi; je 0x290fa */
                    e2 = (e2 & 0xffff0000u) | FS16(e2 - 4);     /* 0x290e1: si=fs:[esi-4] */
                    e2 &= 0xffff;                               /* 0x290e6: and esi,0xffff */
                    if (e2 != 0) {                              /* 0x290ec: je 0x290fa */
                        G8(VA_g_world_render_subpass_kind) = 4;                        /* 0x290ee */
                        build_secondary_surface_list(e2);/* 0x290f5: call 0x2b298 */
                    }
                }
            }
            /* 0x290fa */
            {
                uint32_t e3 = G32(VA_g_perspective_scale + 0x4);                     /* 0x290fa: esi=[0x8528c] */
                if (ES16(e3 + 0x18) == 0) {                     /* 0x29100: cmp es:[esi+0x18],0; je 0x29292 */
                    render_secondary_surface_list(es, fs, gs);   /* 0x29292: call 0x2b333 */
                    goto chain_advance;
                }
                G8(VA_g_secondary_subpass_flags) = 0;                                /* 0x2910c */
                G16(VA_g_view_bound_left) = FS16(chain + 4);                /* 0x29114: dx=fs:[bx+4] (bx=chain) */
                G16(VA_g_view_bound_right) = FS16(chain + 6);                /* 0x29121: bp=fs:[bx+6] */
                uint32_t cl2   = ES8(e3 + 0xd);                /* 0x2912f: cl=es:[esi+0xd] */
                uint32_t edge2 = ES16(e3 + 4);                 /* 0x29133: bx=es:[esi+4] */
                uint32_t geo   = (e3 & 0xffff0000u) | ES16(e3 + 0x18);   /* 0x29138: si=es:[esi+0x18] */
                uint16_t dxg = ES16(geo + 8);                  /* 0x2913d: dx=es:[esi+8] (esi=geo) */
                uint16_t axg = ES16(geo + 2);                  /* 0x29142: ax=es:[esi+2] */
                {                                              /* 0x29148: edx+=eax; neg; sar dx,1 */
                    uint16_t s = (uint16_t)(0u - (uint16_t)(dxg + axg));
                    G16(VA_g_reflection_view_list + 0x8a) = (uint16_t)((int16_t)s >> 1);
                }
                uint16_t axS = (uint16_t)(G16(VA_g_sector_cull_coord) - axg); /* 0x29157: ax-=[0x852fa]; neg */
                uint16_t dxS = (uint16_t)(G16(VA_g_sector_cull_coord) - dxg); /* 0x29160: dx-=[0x852fa]; neg */
                G16(VA_g_format_flags + 0x23) = dxS;                            /* 0x29169 */
                G16(VA_g_format_flags + 0x25) = axS;
                if ((int16_t)axS < 0) {                        /* 0x29176: jge 0x291ff (near path when <0) */
                    render_secondary_surface_pass1(es, fs, gs);   /* 0x2917f: call 0x2b36f */
                    G16(VA_g_sprite_view_angle + 0x6) = axS;                        /* 0x29184: [0x909fe]=[0x84942] */
                    G8(VA_g_world_render_subpass_kind) = 7;                           /* 0x2918a */
                    uint16_t g0 = ES16(geo);                   /* 0x29197: ax=es:[esi] */
                    if ((int16_t)g0 < 0) {                     /* jns 0x291bc (skip when >=0) */
                        G16(VA_g_span_src_wrap_reoffset + 0x10) = 1; G16(VA_g_span_fill_mode_word) = 0x30; /* 0x291a0 */
                        emit_world_span_clipped_indexed(g0, cl2, edge2, es, fs, gs); /* 0x291b2: 0x2d4b5 */
                    } else if (g0 != G16(VA_g_world_span_colorkey)) {           /* 0x291bc: cmp; je 0x29281 */
                        uint16_t d4 = ES16(geo + 4);           /* 0x291c9 */
                        G8(VA_g_view_offset_y + 0x3) = (uint8_t)d4; G8(VA_g_view_offset_y + 0x5) = (uint8_t)(d4 >> 8);
                        G16(VA_g_span_src_wrap_reoffset + 0x10) = (uint16_t)((ES8(geo + 0xc) & 0xc) >> 2);        /* 0x291da */
                        G16(VA_g_span_fill_mode_word) = 0x38;                   /* 0x291ec */
                        emit_world_span_clipped_indexed(g0, cl2, edge2, es, fs, gs); /* 0x291f5: 0x2d4b5 */
                    }
                } else {                                       /* far path 0x291ff */
                    if ((int16_t)dxS > 0) {                    /* 0x291ff: test ax,ax; jle 0x29281 (run when >0) */
                        render_secondary_surface_pass2(es, fs, gs);   /* 0x2920a: call 0x2b407 */
                        G16(VA_g_sprite_view_angle + 0x6) = dxS;                    /* 0x2920f: [0x909fe]=[0x84940] */
                        G8(VA_g_world_render_subpass_kind) = 8;                       /* 0x29215 */
                        uint16_t g6 = ES16(geo + 6);           /* 0x29222: ax=es:[esi+6] */
                        if ((int16_t)g6 < 0) {                 /* jns 0x29245 (skip when >=0) */
                            G16(VA_g_span_fill_mode_word) = 0xb0; G16(VA_g_span_src_wrap_reoffset + 0x10) = 1;   /* 0x2922c */
                            emit_world_span_clipped(g6, cl2, edge2, es, fs, gs); /* 0x2923e: 0x2d2dd */
                        } else if (g6 != G16(VA_g_world_span_colorkey)) {       /* 0x29245: cmp; je 0x29281 */
                            uint16_t dA = ES16(geo + 0xa);     /* 0x2924e: dx=es:[si+0xa] (si=geo) */
                            G8(VA_g_view_offset_y + 0x3) = (uint8_t)dA; G8(VA_g_view_offset_y + 0x5) = (uint8_t)(dA >> 8);
                            G16(VA_g_span_src_wrap_reoffset + 0x10) = (uint16_t)((ES8(geo + 0xc) & 0x30) >> 4);   /* 0x29260 */
                            G16(VA_g_span_fill_mode_word) = 0xb8;               /* 0x29273 */
                            emit_world_span_clipped(g6, cl2, edge2, es, fs, gs); /* 0x2927c: 0x2d2dd */
                        }
                    }
                }
                /* 0x29281 */
                if (G8(VA_g_secondary_subpass_flags) != 0)                          /* 0x29281: cmp [0x853d2],0; je 0x29292 */
                    render_secondary_surface_pass_clipped(es, fs, gs);   /* 0x2928a: call 0x2b3fa */
                else
                    render_secondary_surface_list(es, fs, gs);          /* 0x29292: call 0x2b333 */
            }

          chain_advance:
            chain = FS16(chain);                               /* 0x29298: bx=fs:[bx]; jne loop */
        }
    }
    #undef FS16
    #undef ES16
    #undef ES8
}

/* patch_span_driver_shade (0x2d6a8): SELF-MODIFYING-CODE shade patcher used by the reflection pass. Caches the shade
 * index at [0x90c1a], looks up two shade-pair words in the obj3 table 0x71db4/0x71dba (indexed by 2*sign-extended ax),
 * and pokes their low/high bytes into ~13 immediate-operand slots in the span drivers' CODE (wall/sprite/floorceil
 * mappers at 0x36xxx/0x37xxx/0x38xxx/0x39xxx/0x3bxxx). Pure code+[0x90c1a] writer; the table is obj3, the targets are
 * the loaded guest code (writable, at canon+OBJ_DELTA). The 0x2ad21 lift calls this UNCONDITIONALLY (the original
 * gates it on ax==[0x90c1a]; always-patching is byte-equivalent — a gated skip means the code already holds that
 * shade — and removes any dependency on the non-restored code segment's cross-run residual). */
void patch_span_driver_shade(uint16_t ax)
{
    G16(VA_g_render_shade_level) = ax;
    uint32_t off = (uint32_t)(2 * (int32_t)(int16_t)ax);             /* cwde; add eax,eax */
    #define CODE8(a) (*(volatile uint8_t *)(uintptr_t)((a) + OBJ_DELTA))
    uint16_t w0 = G16(VA_g_shade_const_table_a + off);
    CODE8(0x36d4f) = (uint8_t)w0; CODE8(0x36d5f) = (uint8_t)w0; CODE8(0x399d8) = (uint8_t)(w0 >> 8);
    uint16_t w1 = G16(VA_g_shade_const_table_b + off);
    CODE8(0x3b829) = (uint8_t)w1; CODE8(0x3b7bf) = (uint8_t)w1; CODE8(0x3b789) = (uint8_t)w1; CODE8(0x38c8a) = (uint8_t)w1;
    CODE8(0x37062) = (uint8_t)(w1 >> 8); CODE8(0x370a4) = (uint8_t)(w1 >> 8); CODE8(0x3b7d3) = (uint8_t)(w1 >> 8);
    CODE8(0x3b7a2) = (uint8_t)(w1 >> 8); CODE8(0x3b841) = (uint8_t)(w1 >> 8); CODE8(0x38cb3) = (uint8_t)(w1 >> 8);
    #undef CODE8
}

/* render_world_face_list (0x2ad21 + its out-of-line tail 0x2b251): the TWIN of 0x28dbe — the SHADED/TRANSLUCENT
 * reflection face-list pass. Same OUTER draw-order x CHAIN x per-face skeleton and the same rendering callees, but
 * (a) NO build (consumes the pre-built draw list), (b) NO [0x90a48] pass-id writes, (c) a per-face SHADE/BLEND prologue
 * that selects the active colormap [0x8a2a8] + texture table [0x8a2ac] from the face flags es:[esi+0xa] (bit 2 / bit
 * 0x40+[0x8a355]&0x49) and SMC-patches the drivers via 0x2d6a8, and (d) a shade nibble (es:[esi+0x17]) folded into the
 * edge emitters' [0x84f2e]. Loads its own fs/es; gs inherited. Rare special-face path 0x2cbb0 now NATIVE. (ABI_SUBPASS) */
void render_world_face_list(uint32_t ecx_unused, uint16_t gs)
{
    extern uint32_t (*g_os_sel_base)(uint16_t);
    #ifndef ROTH_STANDALONE
    if (g_os_sel_base == NULL) {
        regs_t io; memset(&io, 0, sizeof io);
        io.va = 0x2ad21u + OBJ_DELTA; io.ecx = ecx_unused; io.gs = gs;  /* [ORACLE-FALLBACK] */
        call_orig(&io);
        return;
    }
    #endif
    (void)ecx_unused;
    G16(VA_g_secondary_surface_count + 0x4) = G16(VA_g_sector_cull_coord);                                    /* 0x2ad21 */
    if ((int16_t)G16(VA_g_sector_cull_coord + 0x8) <= 0) return;                         /* 0x2ad2d: cmp [0x85302],0; jle ret */
    uint16_t fs = G16(VA_g_map_geometry_selector), es = G16(VA_g_surface_record_selector);                  /* 0x2ad3b/0x2ad42 */
    uint32_t fs_base = g_os_sel_base(fs);
    uint32_t es_base = g_os_sel_base(es);
    #define FS16(o) (*(volatile uint16_t *)(uintptr_t)(fs_base + (uint16_t)(o)))
    #define ES16(o) (*(volatile uint16_t *)(uintptr_t)(es_base + (uint16_t)(o)))
    #define ES8(o)  (*(volatile uint8_t  *)(uintptr_t)(es_base + (uint16_t)(o)))

    uint32_t count = FS16(0xfc00);                                 /* 0x2ad4d */
    uint32_t edi = 0xfc00u + 2u * count;                           /* 0x2ad52 */

    for (uint32_t oc = count; oc != 0; oc--, edi -= 2) {           /* OUTER 0x2ad56 (cx = oc) */
        uint32_t node  = FS16(edi);                               /* 0x2ad5a: bx = fs:[di] */
        uint32_t chain = FS16(node + 6);                          /* 0x2ad5f: bx = fs:[bx+6] */
        G16(VA_g_reflection_view_list + 0x86) = FS16(chain + 4);                           /* 0x2ad64: [0x853ca]=fs:[bx+4] (once/outer) */
        while (chain != 0) {                                      /* CHAIN 0x2ad6f / advance 0x2b235 */
            uint32_t esi = FS16(chain + 2);                       /* 0x2ad71: si = fs:[bx+2] */
            if ((int16_t)(uint16_t)esi == -1)                     /* 0x2ad77: cmp si,-1; je 0x2b235 */
                goto chain_advance;
            G16(VA_g_perspective_scale + 0x4) = (uint16_t)esi;                         /* 0x2ad81 */
            uint16_t edx_v = FS16(chain + 4);                     /* 0x2ad88: dx=fs:[bx+4]; [0x9096a]=dx */
            G16(VA_g_view_bound_left) = edx_v;
            uint16_t ebp_v = FS16(chain + 6);                     /* 0x2ad94: bp=fs:[bx+6]; [0x90968]=bp */
            G16(VA_g_view_bound_right) = ebp_v;

            /* ---- shade/blend prologue 0x2ada1 ---- */
            { uint8_t al = ES8(esi + 0xb);                        /* 0x2ada1 */
              if (al != 0) al = (uint8_t)(al + G8(VA_g_render_sector_walk_mode + 0x23));      /* 0x2ada9 */
              G8(VA_g_column_clip_mode) = al; G8(VA_g_span_clip_source) = al; }               /* 0x2adaf */
            G16(VA_g_world_alt_render_flags) = (uint16_t)(G16(VA_g_world_alt_render_flags) & 0x7fff);     /* 0x2adb9 */
            uint16_t shade_ax = G16(VA_g_player_sector + 0x4);                     /* 0x2adc2 */
            uint16_t cmap_bx  = G16(VA_g_text_color_ramp_selector);                     /* 0x2adc8 */
            uint32_t tex_ecx  = (uint32_t)G32(VA_g_world_shading_table_ptr);           /* 0x2adcf */
            if (ES16(esi + 0xa) & 2) {                            /* 0x2add5: test es:[esi+0xa],2 */
                cmap_bx  = G16(VA_g_text_color_ramp_selector + 0x2);                          /* 0x2adde */
                tex_ecx  = (uint32_t)G32(VA_g_world_tint_table_ptr);
                shade_ax = G16(VA_g_player_sector + 0x6);
                G16(VA_g_world_alt_render_flags) = (uint16_t)(G16(VA_g_world_alt_render_flags) | 0x8000);
            }
            G8(VA_g_span_blend_mode_flag) = 0;                                      /* 0x2adfa */
            if ((ES16(esi + 0xa) & 0x40) && (G8(VA_g_span_blend_mode_flag + 0x1) & 0x49)) {   /* 0x2ae01 / 0x2ae0a */
                cmap_bx = G16(VA_g_gamma_level + 0x4);                           /* 0x2ae13 */
                tex_ecx = (uint32_t)G32(VA_g_world_glow_table_ptr);
                G8(VA_g_span_blend_mode_flag) = 1;
                G16(VA_g_world_alt_render_flags) = (uint16_t)(G16(VA_g_world_alt_render_flags) & 0x7fff);
            }
            G16(VA_g_active_world_remap_selector) = cmap_bx;                               /* 0x2ae30 */
            G32(VA_g_active_world_remap_base) = tex_ecx;                               /* 0x2ae37 */

            uint32_t edge = ES16(esi + 4);                        /* 0x2ae3d: bx=es:[esi+4] */
            if (edge == 0)                                        /* 0x2ae42: or bx,bx; je 0x2b234 */
                goto chain_advance;
            uint32_t cl = ES8(esi + 0xd);                         /* 0x2ae4b: cl=es:[esi+0xd] */
            patch_span_driver_shade(shade_ax);             /* 0x2ae4f gate dropped; 0x2ae58 call 0x2d6a8 */

            /* ---- block A (top edge) 0x2ae5e ---- */
            G16(VA_g_face_span_top + 0x4) = ES16(esi);                             /* 0x2ae5e: ax=es:[esi]; [0x852be]=ax */
            uint16_t axA = (uint16_t)(G16(VA_g_sector_cull_coord) - ES16(esi));  /* sub [0x852fa]; neg */
            G16(VA_g_world_span_top) = axA;
            if ((int16_t)axA < 0) {                               /* 0x2ae77: jge 0x2aef4 skips */
                G16(VA_g_sprite_view_angle + 0x6) = axA;                               /* 0x2ae7c */
                uint16_t a6 = ES16(esi + 6);                      /* 0x2ae82: ax=es:[esi+6] */
                if ((int16_t)a6 < 0) {                            /* 0x2ae8a: jns 0x2aea5 */
                    G16(VA_g_span_src_wrap_reoffset + 0x10) = 1; G16(VA_g_span_fill_mode_word) = 0x30;        /* 0x2ae8c */
                    emit_world_span_unclipped_indexed(a6, cl, edge, es, fs, gs);   /* 0x2ae9e: 0x2d5b0 */
                } else if (a6 != G16(VA_g_world_span_colorkey)) {                  /* 0x2aea5: cmp; je 0x2aef4 */
                    uint16_t dxs = ES16(esi + 0x10);              /* 0x2aeae */
                    G8(VA_g_view_offset_y + 0x3) = (uint8_t)dxs; G8(VA_g_view_offset_y + 0x5) = (uint8_t)(dxs >> 8);
                    G16(VA_g_span_src_wrap_reoffset + 0x10) = (uint16_t)((ES8(esi + 0xa) & 0xc) >> 2);              /* 0x2aebf */
                    G16(VA_g_span_fill_mode_word) = (uint16_t)(0x38 | ((ES8(esi + 0x17) & 0xc) >> 1));    /* 0x2aed2 */
                    emit_world_span_unclipped_indexed(a6, cl, edge, es, fs, gs);   /* 0x2aee5: 0x2d5b0 */
                    G32(VA_g_view_offset_y + 0x2) = 0;                             /* 0x2aeea */
                }
            }

            /* ---- block B (bottom edge) 0x2aef4 ---- */
            G16(VA_g_face_span_top + 0x2) = ES16(esi + 2);                         /* 0x2aef4: ax=es:[esi+2]; [0x852bc]=ax */
            uint16_t axB = (uint16_t)(G16(VA_g_sector_cull_coord) - ES16(esi + 2));   /* sub; neg */
            G16(VA_g_world_span_bottom) = axB;
            if ((int16_t)axB > 0) {                               /* 0x2af0e: jle 0x2af8e skips */
                G16(VA_g_sprite_view_angle + 0x6) = axB;                               /* 0x2af13 */
                uint16_t a8 = ES16(esi + 8);                      /* 0x2af19: ax=es:[esi+8] */
                if ((int16_t)a8 < 0) {                            /* 0x2af1e: jns 0x2af3c */
                    G16(VA_g_span_fill_mode_word) = 0xb0; G16(VA_g_span_src_wrap_reoffset + 0x10) = 1;        /* 0x2af23 */
                    emit_world_span_unclipped(a8, cl, edge, es, fs, gs);          /* 0x2af35: 0x2d3d0 */
                } else if (a8 != G16(VA_g_world_span_colorkey)) {                  /* 0x2af3c: cmp; je 0x2af8e */
                    uint16_t dx12 = ES16(esi + 0x12);             /* 0x2af45 */
                    G8(VA_g_view_offset_y + 0x3) = (uint8_t)dx12; G8(VA_g_view_offset_y + 0x5) = (uint8_t)(dx12 >> 8);
                    G16(VA_g_span_src_wrap_reoffset + 0x10) = (uint16_t)((ES8(esi + 0xa) & 0x30) >> 4);             /* 0x2af56 */
                    G16(VA_g_span_fill_mode_word) = (uint16_t)(0xb8 | ((ES8(esi + 0x17) & 3) << 1));      /* 0x2af69 */
                    emit_world_span_unclipped(a8, cl, edge, es, fs, gs);          /* 0x2af7f: 0x2d3d0 */
                    G32(VA_g_view_offset_y + 0x2) = 0;                             /* 0x2af84 */
                }
            }

            /* ---- per-face setup 0x2af8f ---- */
            G8(VA_g_anim_clock + 0x6)  = ES8(esi + 0x16);                       /* 0x2af8f/0x2af91 */
            G16(VA_g_span_gouraud_colour_b + 0x2) = (uint16_t)(ES16(esi) - ES16(esi + 2)); /* 0x2af9a */
            uint32_t esi_in = ES16(esi + 0xe);                    /* 0x2afa9: si = es:[si+0xe] */
            uint32_t ebx_in = edge;                               /* inner face/edge record */

            /* ---- inner face loop 0x2afaf (do-while >=1) ---- */
            uint8_t ic = (uint8_t)cl;
            do {
                int do_render = 1;
                if (!(FS16(ebx_in + 0x12) & 1)) do_render = 0;                          /* 0x2afaf */
                else if ((int16_t)FS16(ebx_in) >= (int16_t)ebp_v) do_render = 0;         /* 0x2afbd */
                else if ((int16_t)FS16(ebx_in + 2) <= (int16_t)edx_v) do_render = 0;     /* 0x2afcb */
                if (do_render) {
                    uint32_t newbx = ES16(esi_in + 4);            /* 0x2afdc: bx=es:[esi+4] */
                    G16(VA_g_reflection_view_list + 0x88) = (uint16_t)esi_in;              /* 0x2afe1: [0x853cc]=si */
                    G8(VA_g_current_surface_render_flags) = ES8(newbx + 8);                 /* 0x2afe8 */
                    uint16_t esbx = ES16(newbx);                  /* 0x2aff2: ax=es:[bx] */
                    if ((int16_t)esbx < 0) {                      /* 0x2affa: jns 0x2b022 */
                        G16(VA_g_span_src_wrap_reoffset + 0x2c) = (uint16_t)(2 * ES8(newbx + 0xa));   /* 0x2affc */
                        G16(VA_g_span_src_wrap_reoffset + 0x2e) = ES8(newbx + 0xb);          /* 0x2b00c */
                        G16(VA_g_span_fill_mode_word + 0x10) = (uint16_t)(ES16(newbx) & 0xfff);    /* 0x2b019 */
                    } else {
                        G16(VA_g_span_fill_mode_word + 0x10) = esbx;                      /* 0x2b022 */
                    }
                    G16(VA_g_span_src_wrap_reoffset + 0x12) = (uint16_t)(FS16(ebx_in + 9) - FS16(ebx_in + 0x1d));   /* 0x2b029 */
                    if (G8(VA_g_current_surface_render_flags) & 0x40) G8(VA_g_parallax_sky_active) = 0xff;   /* 0x2b03c/0x2b045 */
                    if (FS16(ebx_in + 0x12) & 0x20) {             /* 0x2b04c: jne 0x2b251 (special) */
                        draw_world_face_clipped_spans(FS16(ebx_in + 2), ebx_in, esi_in, es, fs, gs); /* 0x2b251 (NATIVE; was call_orig 0x2cbb0) */
                    } else {
                        draw_world_face_projected_spans(FS16(ebx_in + 2), ebx_in, esi_in, es, fs, gs); /* 0x2b05a */
                    }
                }
                G32(VA_g_span_src_wrap_reoffset + 0x2c) = 0;                                 /* 0x2b05f */
                G8(VA_g_parallax_sky_active)  = 0;                                 /* 0x2b069 */
                esi_in += 0xc; ebx_in += 0x14;                    /* 0x2b070 */
            } while ((int8_t)(--ic) > 0);                         /* 0x2b076: dec cl; jg */

            /* ---- secondary surfaces 0x2b07e ---- */
            G8(VA_g_current_surface_render_flags) = 0;                                      /* 0x2b07e */
            G8(VA_g_has_secondary_surfaces) = 0;                                      /* 0x2b085 */
            {
                uint32_t e2 = G32(VA_g_perspective_scale + 0x4);                       /* 0x2b08c */
                e2 = (e2 & 0xffff0000u) | ES16(e2 + 4);           /* 0x2b092: si=es:[esi+4] */
                if (e2 != 0) {                                    /* 0x2b097: or esi,esi; je */
                    e2 = (e2 & 0xffff0000u) | FS16(e2 - 4);       /* 0x2b09b: si=fs:[esi-4] */
                    /* 0x2b0a0: and ecx,0xffff; je. ecx is NOT the outer counter — the shade prologue clobbered it with
                     * the texture-table ptr (0x2adcf mov ecx,[0x86d28] / [0x86d1c] / [0x86d18] -> tex_ecx -> [0x8a2ac]),
                     * then the inner loop's `dec cl` left cl_final (0 normally, 0xff if facecount==0) in the low byte.
                     * So ecx&0xffff = (tex_ecx & 0xff00) | cl_final (ic). */
                    if (((tex_ecx & 0xff00u) | (uint32_t)ic) != 0)
                        build_secondary_surface_list(e2);  /* 0x2b0a8: 0x2b298 */
                }
            }
            {
                uint32_t e3 = G32(VA_g_perspective_scale + 0x4);                       /* 0x2b0ad */
                if (ES16(e3 + 0x18) == 0) {                       /* 0x2b0b3: je 0x2b22f */
                    render_secondary_surface_list(es, fs, gs);   /* 0x2b22f: call 0x2b333 */
                    goto chain_advance;
                }
                G8(VA_g_secondary_subpass_flags) = 0;                                  /* 0x2b0bf */
                G16(VA_g_view_bound_left) = FS16(chain + 4);                  /* 0x2b0c7: dx=fs:[bx+4] */
                G16(VA_g_view_bound_right) = FS16(chain + 6);                  /* 0x2b0d4: bp=fs:[bx+6] */
                uint32_t cl2   = ES8(e3 + 0xd);                  /* 0x2b0e2 */
                uint32_t edge2 = ES16(e3 + 4);                   /* 0x2b0e6 */
                uint32_t geo   = (e3 & 0xffff0000u) | ES16(e3 + 0x18);   /* 0x2b0eb */
                uint16_t dxg = ES16(geo + 8);                    /* 0x2b0f0 */
                uint16_t axg = ES16(geo + 2);                    /* 0x2b0f5 */
                G16(VA_g_reflection_view_list + 0x8a) = (uint16_t)((int16_t)(uint16_t)(dxg + axg) >> 1);          /* 0x2b0fb: add; sar (NO neg) */
                uint16_t axS = (uint16_t)(G16(VA_g_sector_cull_coord) - axg);   /* 0x2b108 */
                uint16_t dxS = (uint16_t)(G16(VA_g_sector_cull_coord) - dxg);   /* 0x2b111 */
                G16(VA_g_format_flags + 0x23) = dxS;                              /* 0x2b11a */
                G16(VA_g_format_flags + 0x25) = axS;
                if ((int16_t)axS < 0) {                          /* 0x2b127: jge 0x2b1a2 (near when <0) */
                    render_secondary_surface_pass1(es, fs, gs);   /* 0x2b12c: 0x2b36f */
                    G16(VA_g_sprite_view_angle + 0x6) = axS;                          /* 0x2b131 */
                    uint16_t g0 = ES16(geo);                     /* 0x2b13d */
                    if ((int16_t)g0 < 0) {                       /* 0x2b144: jns 0x2b162 */
                        G16(VA_g_span_src_wrap_reoffset + 0x10) = 1; G16(VA_g_span_fill_mode_word) = 0x30;   /* 0x2b146 */
                        emit_world_span_clipped_indexed(g0, cl2, edge2, es, fs, gs); /* 0x2b158: 0x2d4b5 */
                    } else if (g0 != G16(VA_g_world_span_colorkey)) {             /* 0x2b162: cmp; je 0x2b21e */
                        uint16_t d4 = ES16(geo + 4);             /* 0x2b16f */
                        G8(VA_g_view_offset_y + 0x3) = (uint8_t)d4; G8(VA_g_view_offset_y + 0x5) = (uint8_t)(d4 >> 8);
                        G16(VA_g_span_src_wrap_reoffset + 0x10) = (uint16_t)((ES8(geo + 0xc) & 0xc) >> 2);          /* 0x2b180 */
                        G16(VA_g_span_fill_mode_word) = 0x38;                     /* 0x2b192 */
                        emit_world_span_clipped_indexed(g0, cl2, edge2, es, fs, gs); /* 0x2b19b: 0x2d4b5 */
                    }
                } else {                                         /* far path 0x2b1a2 */
                    if ((int16_t)dxS > 0) {                      /* 0x2b1a8: jle 0x2b21e (run when >0) */
                        render_secondary_surface_pass2(es, fs, gs);   /* 0x2b1ad: 0x2b407 */
                        G16(VA_g_sprite_view_angle + 0x6) = dxS;                      /* 0x2b1b2 */
                        uint16_t g6 = ES16(geo + 6);             /* 0x2b1be */
                        if ((int16_t)g6 < 0) {                   /* 0x2b1c6: jns 0x2b1e1 */
                            G16(VA_g_span_fill_mode_word) = 0xb0; G16(VA_g_span_src_wrap_reoffset + 0x10) = 1;   /* 0x2b1c8 */
                            emit_world_span_clipped(g6, cl2, edge2, es, fs, gs);  /* 0x2b1da: 0x2d2dd */
                        } else if (g6 != G16(VA_g_world_span_colorkey)) {         /* 0x2b1e1: cmp; je 0x2b21e */
                            uint16_t dA = ES16(geo + 0xa);       /* 0x2b1ea */
                            G8(VA_g_view_offset_y + 0x3) = (uint8_t)dA; G8(VA_g_view_offset_y + 0x5) = (uint8_t)(dA >> 8);
                            G16(VA_g_span_src_wrap_reoffset + 0x10) = (uint16_t)((ES8(geo + 0xc) & 0x30) >> 4);     /* 0x2b1fc */
                            G16(VA_g_span_fill_mode_word) = 0xb8;                 /* 0x2b210 */
                            emit_world_span_clipped(g6, cl2, edge2, es, fs, gs);  /* 0x2b219: 0x2d2dd */
                        }
                    }
                }
                if (G8(VA_g_secondary_subpass_flags) != 0)                            /* 0x2b21e: cmp [0x853d2],0; je 0x2b22f */
                    render_secondary_surface_pass_clipped(es, fs, gs);   /* 0x2b227: 0x2b3fa */
                else
                    render_secondary_surface_list(es, fs, gs);          /* 0x2b22f: 0x2b333 */
            }

          chain_advance:
            chain = FS16(chain);                                 /* 0x2b235: bx=fs:[bx]; jne loop */
        }
    }
    #undef FS16
    #undef ES16
    #undef ES8
}

/* ---------------------------------------------------------------------------------------------
 * setup_surface_render_constants (0x2abfb) — pure-DS leaf: initializes the per-surface/view
 * perspective + span constants from two geometry/view records [0x85270] (recA) / [0x85274] (recB).
 * NO GP-register inputs, no es:/fs:/gs:, no fb. Writes obj3 globals PLUS two SMC code-byte biases
 * (0x38c94 = wall-edge projector / 0x3b794 = floor-ceil edge projector — read live by wd_project /
 * the floor-ceil edge-walker), modelled here as G16 writes (de-SMC: faithful AND portable). This was
 * the LAST bridged callee of render_world_scene. recA/recB are host pointers stored in globals ->
 * deref RAW (the stored-pointer-is-host-address rule). The two `mul;div` and
 * the two `0x40000/dim` are 16-bit DX:AX divides (faithful for valid non-zero dims). Verified by
 * ABI_SURFCONST (obj3 diff + the two SMC code words, which live outside obj3). */
void setup_surface_render_constants(void)
{
    const uint8_t *recA = (const uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_view_params_block);
    {
        uint16_t ax = (uint16_t)((uint16_t)*(const uint16_t *)(recA + 0x1c) + (uint16_t)G16(VA_g_player_sector_cache + 0x2));
        G16(VA_g_wd_project_bias_default) = ax;          /* 0x2ac0c SMC: wall-edge projector bias (read by wd_project) */
        G16(VA_g_floorceil_edge_bias_default) = ax;          /* 0x2ac12 SMC: floor/ceil edge projector bias */
    }
    G16(VA_g_render_shade_level) = 0xffff;          /* 0x2ac18 g_render_shade_level reset */
    G16(VA_g_world_span_colorkey) = *(const uint16_t *)(recA + 0x1a);   /* 0x2ac21 */

    const uint8_t *recB = (const uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_view_params_block + 0x4);
    G16(VA_g_view_offset_y + 0x6) = 0x8123;          /* 0x2ac35 */
    G16(VA_g_view_offset_y + 0x8) = 0x8123;          /* 0x2ac3b */
    G16(VA_g_view_offset_y + 0xa) = 0x8123;          /* 0x2ac41 */
    G32(VA_g_span_fill_mode_word + 0x1a) = (int32_t)GADDR(VA_g_span_fill_mode_word + 0x70);   /* 0x2ac47: stored runtime ptr (canon 0x84f9e + OBJ_DELTA) */
    {
        uint16_t w6 = *(const uint16_t *)(recB + 0x6);
        uint16_t w8 = *(const uint16_t *)(recB + 0x8);
        G16(VA_g_span_src_wrap_reoffset + 0x24) = *(const uint16_t *)(recB + 0xa);                     /* 0x2ac55 (re-stored 0x2ac90) */
        G16(VA_g_sprite_view_angle + 0x2) = (uint16_t)(((uint32_t)0x800u  * w6) / 0x10u);        /* 0x2ac5b: 0x800*w6/0x10 */
        G16(VA_g_view_offset_y + 0xe) = (uint16_t)(((uint32_t)0x2000u * w6) / (uint32_t)w8); /* 0x2ac70: 0x2000*w6/w8 */
        G32(VA_g_span_src_wrap_reoffset + 0x28) = (int32_t)(int16_t)*(const uint16_t *)(recB + 0xc);   /* 0x2ac82: cwde (signed) */
        G16(VA_g_span_src_wrap_reoffset + 0x20) = w6;                                                  /* 0x2ac9c */
        G32(VA_g_view_params_block + 0xc) = (int32_t)(uint32_t)w6;                              /* 0x2aca2 */
        G32(VA_g_view_params_block + 0x10) = (int32_t)((uint32_t)w6 << 8);                       /* 0x2acaa */
        G16(VA_g_span_src_wrap_reoffset + 0x1c) = w8;                                                 /* 0x2acb5 */
        G32(VA_g_view_params_block + 0x14) = (int32_t)(uint32_t)w8;                              /* 0x2acbb */
        G32(VA_g_perspective_scale) = (int32_t)((uint32_t)w8 << 8);                       /* 0x2acc3: g_perspective_scale */
        G16(VA_g_span_src_wrap_reoffset + 0x1a) = (uint16_t)(0x40000u / (uint32_t)w8);                /* 0x2acd6: 0x40000/w8 */
        G16(VA_g_span_src_wrap_reoffset + 0x18) = (uint16_t)(0x40000u / (uint32_t)w6);                /* 0x2acea: 0x40000/w6 */
        G32(VA_g_sector_cull_coord + 0xe) = 0;                                                  /* 0x2acf0 */
    }
}

/* ---------------------------------------------------------------------------------------------
 * render_world_scene (0x28a79) — THE SCENE-ROOT spine node. Called by render_world_view 0x10c8f
 * once per frame with EAX = view-centre X, EDX = view-centre Y (EBX preserved-but-dead: pushed at
 * 0x28ac8, popped at 0x28b32, then overwritten by mov ebx,[0x909a0] before any use). Returns EAX =
 * 0x90a48 (the address of the per-frame status byte), like the original's `mov eax,0x90a48; ret`.
 *
 * Body = entry validation/early-out -> view-scale setup from the [0x85274] record -> ES field-4
 * clear (0x293a3) -> the visible-sector walk (0x294c0) with a split-screen retry when the first walk
 * finds nothing -> per-entry sector render-tree build (0x29812) -> draw-order coalesce (0x2a6d0) ->
 * the face-list SUBPASS (0x28dbe) -> a mirror/reflection-params block (gated [0x90a49]) -> the
 * conditional mirror FINALIZE (0x2abfb, gated [0x90bfe]) -> restore [0x909a4] + clear [0x90a48].
 *
 * EVERY callee is now native C (0x2abfb setup_surface_render_constants lifted W24-1, verified obj3+SMC
 * byte-identical over 212k+ samples — it writes a couple of SMC code bytes 0x38c94/0x3b794 outside obj3).
 * 16-bit es:/fs: addressing handled by the helpers, which each re-read their selectors from globals
 * ([0x852c8]/[0x85294]/[0x852cc]) — the root's CPU segment loads are therefore C no-ops.
 * (ABI_SCENEROOT: clone of ABI_SUBPASS's preservation set.) */
uint32_t render_world_scene(uint32_t eax, uint32_t edx, uint32_t ebx_dead,
                                   uint16_t es, uint16_t fs, uint16_t gs)
{
    extern uint32_t (*g_os_sel_base)(uint16_t);
    #ifndef ROTH_STANDALONE
    if (g_os_sel_base == NULL) {                                /* no sel hook -> bridge whole node */
        regs_t io; memset(&io, 0, sizeof io);
        io.va = 0x28a79u + OBJ_DELTA;  /* [ORACLE-FALLBACK] */
        io.eax = eax; io.edx = edx; io.ebx = ebx_dead;
        io.es = es; io.fs = fs; io.gs = gs;
        call_orig(&io);
        return io.eax;
    }
    #endif
    (void)ebx_dead; (void)es; (void)fs; (void)gs;                /* segs reloaded internally by callees */

    G16(VA_g_world_render_subpass_kind) = 0;                                            /* 0x28a7e */
    if (G8(VA_g_render_active) == 0) return 0x90a48;                        /* 0x28a87: je 0x28db3 (early-out) */
    if ((uint32_t)eax >= (uint32_t)G32(VA_g_view_w)) return 0x90a48; /* 0x28a94: jae 0x28db3 */
    if ((uint32_t)edx >= (uint32_t)G32(VA_g_view_h)) return 0x90a48; /* 0x28aa0: jae 0x28db3 */
    if (G8(VA_g_view_scale_flags) & 1) eax = (uint32_t)((int32_t)eax >> 1);    /* 0x28aac: sar eax,1 */
    if (G8(VA_g_view_scale_flags) & 2) edx = (uint32_t)((int32_t)edx >> 1);    /* 0x28ab7: sar edx,1 */

    int32_t saved_909a4 = G32(VA_g_span_src_wrap_reoffset + 0x28);                          /* 0x28ac2: push [0x909a4] (restored at end) */
    uint32_t saved_eax  = eax;                                  /* 0x28ac9: push eax */

    if (G16(VA_g_player_sector + 0x4) != G16(VA_g_render_shade_level))                           /* 0x28aca: cmp ax,[0x90c1a]; je 0x28ade */
        patch_span_driver_shade((uint16_t)G16(VA_g_player_sector + 0x4)); /* 0x28ad9: call 0x2d6a8 */

    /* ---- view-scale setup from the [0x85274] camera record (0x28ade) ---- */
    /* [0x85274] holds a RUNTIME (host) pointer — image/obj3 ptrs are canon+OBJ_DELTA, DPMI buffers are raw
     * mmap addresses — so deref it DIRECTLY, NOT through the canon->host G-macros (which would add OBJ_DELTA
     * a second time and land in unmapped memory). Same for every near-DS pointer read out of a global. */
    uint32_t rec = (uint32_t)G32(VA_g_view_params_block + 0x4);                      /* ebx = [0x85274] (host record ptr) */
    uint32_t ax  = *(volatile uint16_t *)(uintptr_t)(rec + 8);  /* ax = word[rec+8] (rec already host) */
    if (G8(VA_g_hires_line_doubling_flag) != 0) {                                     /* 0x28aea: hires (400-line) branch */
        G16(VA_g_span_src_wrap_reoffset + 0x28) = (uint16_t)((int16_t)G16(VA_g_span_src_wrap_reoffset + 0x28) >> 1);  /* 0x28af3: sar word[0x909a4],1 */
        ax = (uint16_t)((int32_t)(uint32_t)ax >> 1);            /* 0x28afa: sar eax,1 (ax 16-bit) */
        G16(VA_g_span_src_wrap_reoffset + 0x1c) = (uint16_t)ax;                            /* 0x28afc */
        uint32_t prod = 0x2000u * (uint32_t)(uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x20);  /* 0x28b04: mul word[0x9099c] */
        uint16_t den = (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x1c);
        G16(VA_g_view_offset_y + 0xe) = den ? (uint16_t)(prod / den) : 0;        /* 0x28b0f: div word[0x90998] */
    }
    G16(VA_g_span_src_wrap_reoffset + 0x1c)  = (uint16_t)ax;                               /* 0x28b1e */
    G32(VA_g_view_params_block + 0x14)  = (int32_t)ax;                                /* 0x28b24 (ax zero-extended in eax) */
    G32(VA_g_perspective_scale)  = (int32_t)(ax << 8);                         /* 0x28b29: shl eax,8 */
    eax = saved_eax;                                            /* 0x28b31: pop eax (X) */

    /* 0x28b35: es=[0x852c8]; call 0x293a3 (clear ES field-4); es=[0x85294] */
    clear_es_record_field4((uint8_t *)(uintptr_t)g_os_sel_base((uint16_t)G16(VA_g_surface_record_selector)));

    /* ---- bounds re-check vs the view extents (0x28b4a) ---- */
    uint32_t bx = (uint32_t)(uint16_t)G32(VA_g_span_src_wrap_reoffset + 0x24) * 2u;        /* ebx=[0x909a0]; add ebx,ebx */
    if ((uint16_t)eax >= (uint16_t)bx) goto after_render;       /* 0x28b52: cmp ax,bx; jae 0x28d97 */
    bx = (uint16_t)G16(VA_g_anim_clock + 0x2);                                /* 0x28b5b: bx=[0x8532c] */
    if (G8(VA_g_hires_line_doubling_flag) == 0) bx = (uint16_t)(bx * 2u);             /* 0x28b62: non-hires -> add ebx,ebx */
    if ((uint16_t)edx >= (uint16_t)bx) goto after_render;       /* 0x28b6d: cmp dx,bx; jae 0x28d97 */

    /* ---- set the primary view window + first walk (0x28b76) ---- */
    G16(VA_g_anim_clock + 0xe) = (uint16_t)eax; G16(VA_g_sector_cull_coord + 0x4) = (uint16_t)eax; G16(VA_g_view_bound_left) = (uint16_t)eax;
    eax = (uint16_t)(eax + 1);                                  /* inc eax (X+1) */
    G16(VA_g_sector_cull_coord + 0x2) = (uint16_t)eax; G16(VA_g_view_bound_right) = (uint16_t)eax;
    G16(VA_g_view_bound_top) = (uint16_t)edx; G16(VA_g_view_bound_bottom) = (uint16_t)edx;
    walk_visible_sectors();                             /* 0x28baa: call 0x294c0 */
    G16(VA_g_vertex_selector + 0x20) = 0; G16(VA_g_vertex_selector + 0x22) = 0x8000; G16(VA_g_vertex_selector + 0x24) = 0; G16(VA_g_vertex_selector + 0x2) = 2;

    uint32_t edi;                                              /* the active draw-order list cursor */
    uint32_t ecx = (uint16_t)G32(VA_g_visible_extent_count);                    /* 0x28bd3: visible-sector count */
    if ((uint16_t)ecx != 0) {                                 /* 0x28bdc: jne 0x28cce (normal) */
        edi = 0x85224u;                                       /* 0x28cce */
        G32(VA_g_reflection_view_count) += 0x10;                                 /* 0x28cd3 */
    } else {
        /* ---- cx==0: split-screen retry with a clamped window (0x28be2) ---- */
        uint16_t sx = (uint16_t)G16(VA_g_anim_clock + 0xe);                /* ax = [0x85338] */
        uint16_t bx2 = (uint16_t)(sx + 1);                   /* ebx = ax; inc ebx */
        if ((int16_t)sx < (int16_t)G16(VA_g_span_src_wrap_reoffset + 0x24))             /* 0x28beb: jge 0x28bfc */
            bx2 = (uint16_t)G32(VA_g_span_src_wrap_reoffset + 0x24);                    /* 0x28bf4: ebx=[0x909a0] */
        else if ((int16_t)sx > (int16_t)G16(VA_g_span_src_wrap_reoffset + 0x24))        /* 0x28bfc: jle 0x28c0a */
            sx = (uint16_t)G32(VA_g_span_src_wrap_reoffset + 0x24);                     /* 0x28c05: eax=[0x909a0] */
        G16(VA_g_sector_cull_coord + 0x4) = sx; G16(VA_g_view_bound_left) = sx;                /* 0x28c0a */
        G16(VA_g_sector_cull_coord + 0x2) = bx2; G16(VA_g_view_bound_right) = bx2;
        walk_visible_sectors();                       /* 0x28c2b: call 0x294c0 */
        G16(VA_g_vertex_selector + 0x20) = 0; G16(VA_g_vertex_selector + 0x22) = 0x8000; G16(VA_g_vertex_selector + 0x24) = 0; G16(VA_g_vertex_selector + 0x2) = 2;
        ecx = (uint16_t)G32(VA_g_visible_extent_count);                        /* 0x28c54 */
        if ((uint16_t)ecx == 0) goto after_render;           /* 0x28c5d: je 0x28d97 */
        edi = 0x85224u;                                      /* 0x28c63 */
        G32(VA_g_reflection_view_count) += 0x10;                                /* 0x28c68 */
        if ((uint16_t)ecx != 1) {                            /* 0x28c6f: cmp cx,1; je 0x28cda */
            /* scan the list for the entry whose [+2,+4] interval straddles dx=[0x85338] (0x28c75) */
            int16_t dxv = (int16_t)G16(VA_g_anim_clock + 0xe);
            uint32_t p = 0x85224u;
            uint32_t cnt = (uint16_t)ecx;
            int found = 0;
            for (; cnt != 0; cnt--, p += 6) {
                if ((int16_t)G16(p + 2) > dxv) continue;     /* 0x28c80: jg 0x28cc2 */
                int16_t hi = (int16_t)G16(p + 4);
                if (hi < dxv) continue;                      /* 0x28c8c: jl 0x28cc2 */
                uint32_t e0 = 0x85224u;                      /* esi = first entry */
                if (hi == dxv) {                             /* 0x28c93: jne 0x28ca0 (ZF: hi==dx) */
                    G16(e0 + 4) = (uint16_t)dxv;             /* [esi+4]=dx */
                    G16(e0 + 2) = (uint16_t)(dxv - 1);       /* dec edx; [esi+2]=dx-1 */
                } else {
                    G16(e0 + 2) = (uint16_t)dxv;             /* [esi+2]=dx */
                    G16(e0 + 4) = (uint16_t)(dxv + 1);       /* inc edx; [esi+4]=dx+1 */
                }
                G16(e0) = (uint16_t)G16(p);                  /* 0x28ca9: [esi]=[edi] (sector id) */
                G32(VA_g_visible_extent_count) = 1;                            /* 0x28caf */
                edi = e0; ecx = 1;                           /* 0x28cb9/0x28cbb */
                found = 1;
                break;
            }
            if (!found) goto after_render;                   /* 0x28cc9: jmp 0x28d97 */
        }
    }

    /* ---- per-entry sector render-tree build (0x28cda render loop) ---- */
    {
        uint32_t cnt = (uint16_t)ecx;
        for (; cnt != 0; cnt--, edi += 6) {
            G16(VA_g_vertex_selector + 0x18) = 0;                                /* 0x28cda */
            uint32_t esi = (uint16_t)G16(edi);              /* 0x28ce5: si=[edi] (sector id) */
            uint16_t lo = (uint16_t)G16(edi + 2);
            G16(VA_g_view_bound_left) = lo; G16(VA_g_sector_cull_coord + 0x4) = lo;           /* 0x28cec/0x28cf2 */
            uint16_t hi = (uint16_t)G16(edi + 4);
            G16(VA_g_view_bound_right) = hi; G16(VA_g_sector_cull_coord + 0x2) = hi;           /* 0x28cfc/0x28d02 */
            begin_sector_render_tree(esi);           /* 0x28d0a: call 0x29812 */
        }
    }
    build_sector_draw_order(0);                       /* 0x28d17: call 0x2a6d0 */
    render_world_face_list_subpass((uint32_t)(uint16_t)G16(VA_g_sector_cull_coord + 0x8),
                                          (uint16_t)G16(VA_g_vertex_selector));   /* 0x28d1c: call 0x28dbe (ecx dead) */
    G32(VA_g_reflection_view_count) -= 0x10;                                    /* 0x28d21 */

    /* ---- mirror/reflection params block (0x28d28; gated [0x90a49]) ---- */
    if (G8(VA_g_world_render_subpass_kind + 0x1) != 0) {
        int32_t d = G32(VA_g_current_decoded_frame + 0x4) - G32(VA_g_span_src_wrap_reoffset + 0x24);            /* 0x28d31/0x28d36 */
        if (d < 0) d = -d;                                  /* jns/neg */
        d <<= 7;                                            /* 0x28d40: shl eax,7 */
        int32_t den = G32(VA_g_view_w);
        int32_t q = den ? (d / den) : 0;                    /* 0x28d45: idiv [0x85cd8] (edx=0) */
        G32(VA_g_subpass_persp_step) += q;                                  /* 0x28d4b */
        uint8_t *esb = (uint8_t *)(uintptr_t)g_os_sel_base((uint16_t)G16(VA_g_surface_record_selector));  /* es=[0x852c8] */
        #define ESC8(o)  (*(volatile uint8_t  *)(uintptr_t)(esb + (uint16_t)(o)))
        #define ESC16(o) (*(volatile uint16_t *)(uintptr_t)(esb + (uint16_t)(o)))
        uint32_t si = (uint16_t)G16(VA_g_subpass_surfrec_ref + 0x2);               /* 0x28d58 */
        G16(VA_g_subpass_reflect_param_b) = ESC16(si + 0x14);                    /* 0x28d5f/0x28d65 */
        if (G8(VA_g_world_render_subpass_kind + 0x1) == 3) {                             /* 0x28d6b: jne 0x28d97 */
            uint32_t si2 = (uint16_t)G16(VA_g_subpass_surfrec_ref);          /* 0x28d74 */
            si2 = (uint16_t)ESC16(si2 + 4);                 /* 0x28d7b: si = es:[si+4] */
            uint16_t av = 0;                                /* sub eax,eax */
            if (ESC8(si2 + 1) & 0x80)                       /* 0x28d83: test es:[si+1],0x80 */
                av = ESC16(si2 + 0xc);                      /* 0x28d8b: ax = es:[si+0xc] */
            G16(VA_g_subpass_reflect_param_a) = av;                              /* 0x28d91 */
        }
        #undef ESC8
        #undef ESC16
    }

after_render:
    if (G16(VA_g_subpass_patch_gate) != 0)                                  /* 0x28d97: cmp [0x90bfe],0; je 0x28da6 */
        setup_surface_render_constants();           /* 0x28da1: call 0x2abfb (NATIVE; was call_asm bridge) */
    G32(VA_g_span_src_wrap_reoffset + 0x28) = saved_909a4;                             /* 0x28da6: pop eax; mov [0x909a4],eax */
    G8(VA_g_world_render_subpass_kind) = 0;                                        /* 0x28dac: mov byte[0x90a48],0 */
    return 0x90a48;                                         /* 0x28db3: mov eax,0x90a48; ret */
}

/* ============================ Batch 17 (corpus-direct leaves) ============================ */

/* reset_movement_velocity_queues (0x124dd): zero the movement velocity ring-buffer cursors/arrays.
 * Pure global writer (no args). word[0x90b42]=0, word[0x90abe]=0, dword[0x90a84]=0, dword[0x90a80]=0. */
void reset_movement_velocity_queues(void)
{
    G16(VA_g_vel_queue_b) = 0;
    G16(VA_g_vel_queue_a) = 0;
    G32(VA_g_player_vel_accum_y) = 0;
    G32(VA_g_player_vel_accum_x) = 0;
}

/* compute_viewport_half_extents (0x1a37b): [0x80b28] = [0x8549c] >> 1 (unsigned);
 * [0x80b24] = ([0x85498] - 0x130) >> 1.  0x85498 = g_screen_pitch; reads two screen globals,
 * writes two half-extent globals. Pure global leaf (no args). */
void compute_viewport_half_extents(void)
{
    G32(VA_g_ui_panel_anchor_y) = (int32_t)((uint32_t)G32(VA_g_screen_pitch + 0x4) >> 1);
    G32(VA_g_ui_panel_anchor_x) = (int32_t)(((uint32_t)G32(VA_g_screen_pitch) - 0x130) >> 1);
}

/* store_indexed_dword_flagged (0x561f9): table = *(void**)0x75690; table[eax] = edx | 0x4000.
 * (`or dh,0x40` sets bit 14 of edx.) EAX = dword index, EDX = value. Void. */
void store_indexed_dword_flagged(uint32_t eax, uint32_t edx)
{
    uint32_t base = (uint32_t)G32(VA_g_heap_free_list + 0x80);
    *(uint32_t *)(uintptr_t)(base + eax * 4) = edx | 0x4000;
}

/* set_909a4_save_old_to_852f2 (0x2acfc): edx=old [0x909a4]; [0x909a4] = (int32)(int16)ax (cwde);
 * word[0x852f2] = low16(old). AX = new value. Void (eax/edx restored). Sibling of 0x2ad14. */
void set_909a4_save_old_to_852f2(uint32_t eax)
{
    int32_t old = G32(VA_g_span_src_wrap_reoffset + 0x28);
    G32(VA_g_span_src_wrap_reoffset + 0x28) = (int32_t)(int16_t)(uint16_t)eax;       /* cwde: sign-extend AX */
    G16(VA_g_vertex_selector + 0x26) = (uint16_t)old;
}

/* string_copy_bytewise (0x558ec): byte-by-byte strcpy incl. the terminating NUL. EAX=dst, EDX=src.
 * (A separate compiler-emitted copy from the 2-byte-unroll string_copy at 0x54ddf.) */
void string_copy_bytewise(uint8_t *dst, const uint8_t *src)
{
    uint8_t b;
    do { b = *src++; *dst++ = b; } while (b != 0);
}

/* copy_nonzero_bytes (0x1426f): copy `count` bytes src->dst SKIPPING zero source bytes (leaves dst
 * unchanged where src==0). EAX=dst, EBX=count, EDX=src. Walks indices count-1..0 (order irrelevant to
 * the result); does nothing when count<=1 (the entry guard `sub esi,1; jle`). */
void copy_nonzero_bytes(uint8_t *dst, uint32_t ebx, const uint8_t *src)
{
    int32_t i = (int32_t)ebx - 1;
    if (i <= 0)
        return;
    do {
        uint8_t al = src[i];
        if (al) dst[i] = al;
    } while (--i >= 0);
}

/* noop_passthrough_50e1d (0x50e1d): builds a frame, stores EAX to an ephemeral stack local, leaves,
 * and returns — no observable effect; EAX passes through unchanged (all other GP regs preserved). */
uint32_t noop_passthrough_50e1d(uint32_t eax)
{
    return eax;
}

/* ============================ Batch 18 (corpus-direct tiny leaves: accessors) ============================ */

/* Simple global setters: store EAX to a single dword global. EAX=value, void. */
void set_voice_sample_rate(uint32_t eax) { G32(VA_g_voice_sample_rate) = (int32_t)eax; }   /* 0x1e76e */
void set_71388(uint32_t eax) { G32(VA_g_choice_selected_index + 0x18) = (int32_t)eax; }   /* 0x20597 */
void set_71d84(uint32_t eax) { G32(VA_g_test_sfx_descriptor + 0x3a) = (int32_t)eax; }   /* 0x26b60 */
void set_8495c(uint32_t eax) { G32(VA_g_current_decoded_frame + 0x18) = (int32_t)eax; }   /* 0x283a7 */

/* get_doserrno_ptr (0x560c8): returns &DAT_97d48 — the immediate is a RELOCATED pointer, so at runtime
 * it is the rebased address (canon 0x97d48 + OBJ_DELTA = 0x497d48). Sibling of get_errno_ptr (0x560c2). */
uint32_t get_doserrno_ptr(void) { return 0x97d48u + OBJ_DELTA; }

/* get_72540 (0x58503): returns dword global [0x72540]. */
uint32_t get_72540(void) { return (uint32_t)G32(VA_g_response_file_arg + 0x8); }

/* cmd_snap_toggle (0x11077): toggles (bitwise NOT) the byte at [0x76840]. Void. */
void cmd_snap_toggle(void) { G8(VA_g_snapshot_enabled) = (uint8_t)~G8(VA_g_snapshot_enabled); }

/* clear_819c0_bits (0x1c5c8): byte[0x819c0] &= 0xfa (clears bits 0 and 2). Void. */
void clear_819c0_bits(void) { G8(VA_g_player_locomotion_flags) &= 0xfa; }

/* set_90bf8_ffff (0x108d4): word[0x90bf8] = 0xffff. Void. */
void set_90bf8_ffff(void) { G16(VA_g_blur_flag) = 0xffff; }

/* xchg_849a4 (0x283a0): atomic swap — returns old [0x849a4], stores EAX into it. EAX in/out. */
uint32_t xchg_849a4(uint32_t eax)
{
    uint32_t old = (uint32_t)G32(VA_g_current_proc_tag + 0x8);
    G32(VA_g_current_proc_tag + 0x8) = (int32_t)eax;
    return old;
}

/* emit_biased_byte (0x4ee93): *edi = DL + 0x80; edi++ (returned); EDX cleared to 0. DL=value byte,
 * EDI=dest pointer. Observable: the written byte + advanced EDI + EDX=0. */
uint32_t emit_biased_byte(uint8_t dl, uint8_t *edi)
{
    *edi = (uint8_t)(dl + 0x80);
    return (uint32_t)(uintptr_t)(edi + 1);
}

/* ============================ Batch 19 (corpus-direct small leaves) ============================ */

/* set_default_mouse_button_swap (0x15b50): dword[0x7feb8] = 1. Void. */
void set_default_mouse_button_swap(void) { G32(VA_g_mouse_button_swap) = 1; }

/* block_size_field8 (0x35fe4): EAX ? *(u32*)(EAX+8) - 0x10 : 0. Pointer arg; sibling of
 * block_payload_size (0x35fd9, reads +4). The EAX==0 path jumps to the shared ret at 0x35fe3 (=> 0). */
uint32_t block_size_field8(uint32_t eax)
{
    if (eax == 0) return 0;
    return *(uint32_t *)(uintptr_t)(eax + 8) - 0x10;
}

/* halve_eax_if_90bd4 (0x2e35f): if (byte[0x90bd4] & 1) EAX = (int32)EAX >> 1 (arithmetic). Return EAX. */
uint32_t halve_eax_if_90bd4(uint32_t eax)
{
    if (G8(VA_g_view_scale_flags) & 1) return (uint32_t)((int32_t)eax >> 1);
    return eax;
}

/* nibble_to_hex_char (0x583d1): EAX += 0x30; if ((int32)EAX > 0x39) EAX += 0x27; return EAX.
 * Converts a 0-15 nibble to its ASCII hex char ('0'-'9' / 'a'-'f'). */
uint32_t nibble_to_hex_char(uint32_t eax)
{
    eax += 0x30;
    if ((int32_t)eax > 0x39) eax += 0x27;
    return eax;
}

/* set_cursor_shape_ptr_pair (0x115dd): v = *(u32*)EAX; [0x76878] = v; [0x7687c] = v; return v.
 * EAX = pointer; loads a dword and mirrors it into two globals. */
uint32_t set_cursor_shape_ptr_pair(uint32_t eax)
{
    uint32_t v = *(uint32_t *)(uintptr_t)eax;
    G32(VA_g_saveunder_sprite_color_ptr) = (int32_t)v;
    G32(VA_g_saveunder_sprite_color_ptr + 0x4) = (int32_t)v;
    return v;
}

/* enable_dev_mode (0x14464): byte[0x7f560] = 1; byte[0x7f36f] = ~byte[0x7f36f]. Void. */
void enable_dev_mode(void)
{
    G8(VA_g_dev_mode_flag) = 1;
    G8(VA_g_debug_map_enabled + 0x1) = (uint8_t)~G8(VA_g_debug_map_enabled + 0x1);
}

/* copy_word_90bcc_to_8532a (0x2ab21): word[0x8532a] = word[0x90bcc]. Void. */
void copy_word_90bcc_to_8532a(void) { G16(VA_g_anim_clock) = G16(VA_g_frame_tick_counter); }

/* ============================ Batch 20 (corpus-direct: string utils + global setups) ============================ */

/* string_length_244c8 (0x244c8): strlen — EAX=str, returns length. (A 3rd strlen-class compiler copy.) */
uint32_t string_length_244c8(const uint8_t *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

/* string_find_char (0x57da2): strchr — EAX=str, DL=char. Returns ptr to first occurrence (matches the
 * terminating NUL when DL==0), or 0 if not found. */
uint32_t string_find_char(const uint8_t *s, uint8_t dl)
{
    while (*s != dl) {
        if (*s == 0) return 0;
        s++;
    }
    return (uint32_t)(uintptr_t)s;
}

/* path_basename (0x150fa): EAX=path, returns ptr to the component after the last '\' (0x5c). */
uint32_t path_basename(const uint8_t *s)
{
    const uint8_t *esi = s, *edi = s;
    for (;;) {
        uint8_t al = *esi++;
        if (al == 0x5c) { edi = esi; continue; }
        if (al != 0) continue;
        break;
    }
    return (uint32_t)(uintptr_t)edi;
}

/* cmd_set_hires (0x1089c): word[0x90be6] |= 4; if (byte[0x90c08] != 0) word[0x90be6] &= 3. Void. */
void cmd_set_hires(void)
{
    G16(VA_g_video_mode_flags) |= 4;
    if (G8(VA_g_rawscreen_flag) != 0) G16(VA_g_video_mode_flags) &= 3;
}

/* reset_input_ring (0x12504): word[0x7e91c]=0 (head), [0x7e91e]=0 (tail), [0x7e91a]=3 (mask). Resets the
 * input ring buffer consumed by input_ring_dequeue (0x1299a). Void. */
void reset_input_ring(void)
{
    G16(VA_g_saved_int9_segment + 0x6) = 0;
    G16(VA_g_saved_int9_segment + 0x8) = 0;
    G16(VA_g_saved_int9_segment + 0x4) = 3;
}

/* init_84964_block (0x289bf): [0x84964]=0xffffffff, [0x84970]=0x800000, [0x84974]=0. Void. */
void init_84964_block(void)
{
    G32(VA_g_current_decoded_frame + 0x20) = (int32_t)0xffffffff;
    G32(VA_g_current_decoded_frame + 0x2c) = 0x800000;
    G32(VA_g_current_decoded_frame + 0x30) = 0;
}

/* advance_clamp_8a0f0 (0x320a7): [0x83e7c]=1; [0x8a0f0]+=EAX; if ((u32)[0x8a0f0] >= EDX) [0x8a0f0]=EDX.
 * EAX=increment, EDX=max (unsigned clamp). Void. */
void advance_clamp_8a0f0(uint32_t eax, uint32_t edx)
{
    G32(VA_g_active_weapon_ammo_cap + 0x8) = 1;
    uint32_t v = (uint32_t)G32(VA_g_player_health) + eax;
    if (v >= edx) v = edx;
    G32(VA_g_player_health) = (int32_t)v;
}

/* ============================ Batch 21 (corpus-direct: resolver, path/blit utils, global setups) ====== */

/* resolve_command_by_index (0x315a7): AX = 1-based index. Returns (*(*0x8a0d8))[AX-1] (4-byte entries),
 * or 0 for AX==0. The command-system index->record-ptr resolver (g_object_ptr_array = *0x8a0d8). */
uint32_t resolve_command_by_index(uint16_t ax)
{
    if (ax == 0) return 0;
    uint32_t base = *(uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_object_ptr_array);   /* esi=[0x8a0d8]; esi=[esi] */
    return *(uint32_t *)(uintptr_t)(base + (uint32_t)(uint16_t)(ax - 1) * 4);
}

/* ===================== RAW command handlers (RAW map command system, dispatch table 0x30780) =====================
 * Handler ABI (from the executor's call site 0x30697): ESI = EDI = command-record ptr (the executor sets
 * edi=esi at 0x30674 before dispatch, so a handler may read the record through EITHER reg). Return EAX =
 * -1 (acted / "continue") or 0 (no-op / "stop test"); a handler may set g_command_chain_interrupt (0x8a268)
 * to break/restart the chain. The record ptr is a RUNTIME/host pointer (resolved out of g_object_ptr_array)
 * -> deref it RAW; obj3 GLOBALS go through the G8/G16/G32 macros (fixed canon addrs). One ptr arg `rec`
 * serves both the esi- and edi-relative accesses. */

/* copy_path_ensure_trailing_slash (0x11057): ESI=src, EDI=dst. Copies the string (incl NUL); if the
 * last real char wasn't '\' (0x5c), replaces the NUL with '\' and re-terminates. (ESI==0 => no-op.) */
void copy_path_ensure_trailing_slash(const uint8_t *esi, uint8_t *edi)
{
    if (esi == 0) return;
    uint8_t al = 0x5c, ah;
    do {
        ah = al;
        al = *esi++;
        *edi++ = al;
    } while (al != 0);
    if (ah != 0x5c) {
        edi[-1] = 0x5c;
        *edi++ = 0;
    }
}

/* copy_nonzero_bytes_2x (0x1428a): EAX=dst, EBX=count, EDX=src. Like copy_nonzero_bytes (0x1426f) but
 * writes each non-zero source byte to TWO adjacent dst bytes (horizontal 2x doubling); no-op if count<=1. */
void copy_nonzero_bytes_2x(uint8_t *dst, uint32_t ebx, const uint8_t *src)
{
    int32_t i = (int32_t)ebx - 1;
    if (i <= 0) return;
    uint8_t *edi = dst + 2 * (uint32_t)i;
    do {
        uint8_t al = src[i];
        if (al) { edi[0] = al; edi[1] = al; }
        edi -= 2;
    } while (--i >= 0);
}

/* begin_item_pickup_lock (0x1622d): g_item_pickup_flags[0x7fd84] |= 5; [0x7fd88]=EAX (item id);
 * [0x7fd8c]=EDX (qty); [0x7fd90]=EBX; word[0x71150]=CX. Called by give_item to arm the ~0x1b-frame
 * pickup lock (blocks re-grab while the item flies). Void. (The 0x7fd84 cluster is an item-pickup
 * lock, not a "scripted camera".) */
void begin_item_pickup_lock(uint32_t eax, uint32_t edx, uint32_t ebx, uint16_t cx)
{
    G8(VA_g_item_pickup_flags) |= 5;
    G32(VA_g_item_pickup_flags + 0x4) = (int32_t)eax;
    G32(VA_g_item_pickup_flags + 0x8) = (int32_t)edx;
    G32(VA_g_item_pickup_flags + 0xc) = (int32_t)ebx;
    G16(VA_g_font_descriptor + 0x23e) = cx;
}

/* compute_screen_extents_7e8b0 (0x115b5): ecx = word[0x707bb]; [0x7e8b0] = [0x85498]*ecx - 1;
 * [0x7e8b4] = [0x8549c]*ecx - 1. (0x85498 = g_screen_pitch.) Void. */
void compute_screen_extents_7e8b0(void)
{
    uint32_t ecx = G16(VA_g_pixel_extent_scale);
    G32(VA_g_cursor_prev_y + 0x801c) = (int32_t)((uint32_t)G32(VA_g_screen_pitch) * ecx - 1);
    G32(VA_g_cursor_prev_y + 0x8020) = (int32_t)((uint32_t)G32(VA_g_screen_pitch + 0x4) * ecx - 1);
}

/* compute_view_offsets_90a74 (0x12179): eax = -((int32)[0x76874] >> 1); [0x90a74]=eax; [0x8c108]=eax
 * (g_view_pitch_applied); word[0x90a8a] = (word[0x7e8f4] - [0x76870]) low16. Void. */
void compute_view_offsets_90a74(void)
{
    int32_t eax = -((int32_t)G32(VA_g_mouse_dy) >> 1);
    G32(VA_g_view_pitch) = eax;
    G32(VA_g_view_pitch_applied) = eax;
    G16(VA_g_player_angle) = (uint16_t)((uint32_t)(uint16_t)G16(VA_g_saved_int9_offset + 0xc) - (uint32_t)G32(VA_g_mouse_dx));
}

/* ============================ Batch 22 (clamps, pool search, codecs) ============================ */

/* approach_value (0x1c630): move EAX toward target EDX by step EBX, clamping at EDX (signed). */
int32_t approach_value(int32_t eax, int32_t edx, int32_t ebx)
{
    if (eax == edx) return edx;
    if (eax < edx) { eax += ebx; return (eax <= edx) ? eax : edx; }
    else           { eax -= ebx; return (eax >= edx) ? eax : edx; }
}

/* clamp_symmetric_26f2d (0x26f2d): clamp EAX to [EDX-EBX, EDX+EBX] (signed); delta = EAX-EDX. */
int32_t clamp_symmetric_26f2d(int32_t eax, int32_t edx, int32_t ebx)
{
    int32_t delta = eax - edx;
    if (delta > ebx)  return edx + ebx;
    if (delta < -ebx) return edx - ebx;
    return eax;
}

/* find_active_effect (0x32606): walk the active-effect pool (head ptr-cell = [0x8a118]); for each
 * record R = *cell, match byte[R+4]==AL (type) && dword[R+8]==ESI (value); next cell = *R (record[0]).
 * Returns the matching record, or 0. AL=type, ESI=value. */
uint32_t find_active_effect(uint8_t al, uint32_t esi)
{
    uint32_t cell = (uint32_t)G32(VA_g_active_effect_pool);
    if (cell == 0) return 0;
    for (;;) {
        uint32_t rec = *(uint32_t *)(uintptr_t)cell;             /* R = *cell */
        if (*(uint8_t *)(uintptr_t)(rec + 4) == al &&
            *(uint32_t *)(uintptr_t)(rec + 8) == esi)
            return rec;
        cell = *(uint32_t *)(uintptr_t)rec;                      /* next cell = *R */
        if (cell == 0) return 0;
    }
}

/* decode_dpcm_block (0x4e4cd): the voice codec. EAX=predictor, EBX=src bytes, EDX=dest s16*, ECX=count.
 * Per byte: predictor += g_dpcm_step_table[byte] (int32[256] @0x918a4); *dest++ = (s16)predictor.
 * Returns updated predictor. (count<=0 still does 1 iteration — the do-while quirk.) */
uint32_t decode_dpcm_block(uint32_t eax, const uint8_t *src, int16_t *dest, int32_t count)
{
    int32_t c = count;
    do {
        eax += (uint32_t)G32(VA_g_dpcm_step_table + (uint32_t)(*src) * 4);
        *dest = (int16_t)eax;
        src++; dest++;
    } while (--c > 0);
    return eax;
}

/* interpolate_words_43dd8 (0x43dd8): EAX=dst s16*, EDX=src s16*, EBX=count. Per input word w (sign-ext):
 * writes pair [ (prev_sum + w) >>1 (UNSIGNED shr — faithful to the asm), w ]; prev_sum carries w. 2x
 * upsample/interpolate. */
void interpolate_words_43dd8(int16_t *dst, const int16_t *src, int32_t ebx)
{
    uint32_t edx = 0;
    int32_t n = ebx;
    do {
        int32_t eax = *src++;                       /* movsx word */
        edx = (uint32_t)((int32_t)edx + eax);
        edx >>= 1;                                  /* shr (unsigned) */
        dst[0] = (int16_t)edx;
        dst[1] = (int16_t)eax;
        edx = (uint32_t)eax;
        dst += 2;
    } while (--n > 0);
}

/* write_ror_ramp_3bb1e (0x3bb1e): EDI=dst, CX=count, EBP=stride. Writes word[dst+2 + k*stride] =
 * low16(ror(edx,8)) for k=0..CX (CX+1 words; `dec cx; jge`), edx starts at [0x8a3d4] and steps by
 * [0x8a3d0] each write. (A stepped/gradient word writer.) */
void write_ror_ramp_3bb1e(uint32_t edi, uint16_t cx, uint32_t ebp)
{
    uint32_t edx = (uint32_t)G32(VA_g_clip_output_vertex_count + 0x8);
    uint32_t step = (uint32_t)G32(VA_g_clip_output_vertex_count + 0x4);
    uint8_t *p = (uint8_t *)(uintptr_t)(edi + 2);
    int32_t c = cx;
    do {
        uint32_t eax = (edx >> 8) | (edx << 24);    /* ror eax,8 */
        *(uint16_t *)p = (uint16_t)eax;
        edx += step;
        p += ebp;
    } while (--c >= 0);
}

/* ============================ Batch 23 (mid-size game logic) ============================ */

/* apply_damage_to_player (0x32023): EAX=damage, DL=scale-flag, ECX=mult-flag. Scales damage (large
 * damage >0xc8 compressed; DL!=0 -> *[0x85324]; ECX!=0 -> reduce by ECX/256), bumps the pain-flash
 * accumulator [0x89f3b] (clamped 0x164), sets [0x83e7c]=1, then subtracts from health [0x8a0f0]
 * (clamped at 0). EAX==0 special-cases to current health w/ no mult. Void (global write-set). */
void apply_damage_to_player(uint32_t eax, uint8_t dl, uint32_t ecx)
{
    if (eax == 0) {
        eax = (uint32_t)G32(VA_g_player_health);
        ecx = 0;
    } else {
        if (eax > 0xc8) eax = (eax - 0xc7) << 7;
        if (dl != 0) eax = (uint32_t)((int32_t)eax * (int32_t)G32(VA_g_frame_time_scale));
    }
    if (ecx != 0) {
        ecx = (uint32_t)((int32_t)ecx * (int32_t)eax);
        ecx >>= 8;
        eax -= ecx;
        if ((int32_t)eax <= 0) return;
    }
    uint32_t dmg = eax;
    G32(VA_g_active_weapon_ammo_cap + 0x8) = 1;
    eax = dmg + 0x20;
    if ((int32_t)eax < 0x35c6 && (int32_t)eax >= (int32_t)G32(VA_g_damage_flash_level)) {
        eax >>= 1;
        uint32_t pain = (uint32_t)G32(VA_g_damage_flash_level) + eax;
        G32(VA_g_damage_flash_level) = (int32_t)pain;
        if (pain >= 0x164) G32(VA_g_damage_flash_level) = 0x164;
    }
    uint32_t h = (uint32_t)G32(VA_g_player_health) - dmg;
    G32(VA_g_player_health) = (int32_t)h;
    if ((int32_t)h < 0) G32(VA_g_player_health) = 0;
}

/* find_free_inventory_slot (0x1ce43): scans the 256-entry inventory at 0x80c30 (4-byte slots) for the
 * first whose word is 0; on success increments the count [0x80c2c] and returns the slot ptr, else 0. */
uint32_t find_free_inventory_slot(void)
{
    uint32_t eax = 0x80c30u + OBJ_DELTA;
    for (uint32_t edx = 0; (int32_t)edx < 0x100; edx++, eax += 4) {
        if (*(uint16_t *)(uintptr_t)eax == 0) {
            G32(VA_g_inventory_count) = (int32_t)((uint32_t)G32(VA_g_inventory_count) + 1);
            return eax;
        }
    }
    return 0;
}

/* stack_onto_inventory_slot (0x1ce14): EAX=item id. Scans the inventory (0x80c30), skipping empty slots
 * (not counted), until it has examined [0x80c2c] used slots; returns the slot whose (sign-extended) word
 * equals the item id, or 0. */
uint32_t stack_onto_inventory_slot(uint32_t eax_in)
{
    int32_t ebx = (int32_t)eax_in;
    uint32_t eax = 0x80c30u + OBJ_DELTA;
    uint32_t edx = 0;
    for (;;) {
        if (edx >= (uint32_t)G32(VA_g_inventory_count)) return 0;
        uint16_t w = *(uint16_t *)(uintptr_t)eax;
        if (w == 0) { eax += 4; continue; }
        if ((int32_t)(int16_t)w == ebx) return eax;
        edx++; eax += 4;
    }
}

/* init_sprite_render_queue (0x3c294): seeds the sprite-render-queue globals — [0x8b32c]=&0x8aad0,
 * [0x8b334]=0x800, [0x8b340]=0, [0x8b304]=&0x8b340 (the two stored immediates are relocated pointers). */
void init_sprite_render_queue(void)
{
    G32(VA_g_sprite_node_pool + 0x85c) = (int32_t)(0x8aad0u + OBJ_DELTA);
    G32(VA_g_sprite_node_pool + 0x864) = 0x800;
    G32(VA_g_sprite_render_queue_head) = 0;
    G32(VA_g_sprite_node_pool + 0x834) = (int32_t)(0x8b340u + OBJ_DELTA);
}

/* swap_voice_double_buffers (0x1e3b0): swaps [0x8201c]<->[0x82020] and [0x82024]<->[0x82028]. */
void swap_voice_double_buffers(void)
{
    uint32_t a = (uint32_t)G32(VA_g_voice_bytes_remaining + 0x8), b = (uint32_t)G32(VA_g_voice_bytes_remaining + 0x4);
    G32(VA_g_voice_bytes_remaining + 0x4) = (int32_t)a; G32(VA_g_voice_bytes_remaining + 0x8) = (int32_t)b;
    uint32_t c = (uint32_t)G32(VA_g_voice_bytes_remaining + 0x10), d = (uint32_t)G32(VA_g_voice_bytes_remaining + 0xc);
    G32(VA_g_voice_bytes_remaining + 0xc) = (int32_t)c; G32(VA_g_voice_bytes_remaining + 0x10) = (int32_t)d;
}

/* build_game_path (0x2fb7f): EAX=dst, EDX=src(dir), EBX=name. If name is drive-prefixed ("X:..."),
 * dst = name only; else dst = src + ('\' if src non-empty and not already trailing-'\') + name. */
void build_game_path(uint8_t *edi, const uint8_t *esi, const uint8_t *ebx)
{
    if (!(*ebx != 0 && ebx[1] == 0x3a)) {       /* not drive-prefixed -> copy src + separator */
        uint8_t al = 0, ah;
        do {
            ah = al;
            al = *esi++;
            if (al != 0) *edi++ = al;
        } while (al != 0);
        if (ah != 0 && ah != 0x5c) *edi++ = 0x5c;
    }
    uint8_t al;
    do { al = *ebx++; *edi++ = al; } while (al != 0);
}

/* check_entity_sector_clearance (0x42c04): EAX=sector index, EDI=entity record. Sector record =
 * EAX+[0x90aa8]. Returns CF (0=clear / 1=blocked): blocked if word[sec+2] > word[edi+0xa] (signed), or
 * if (word[sec] - word[ [0x85c50] + word[edi+4]*4 ]) < word[edi+0xa]. (Returns the CF value 0/1.) */
int check_entity_sector_clearance(uint32_t eax, uint32_t edi)
{
    uint32_t sec = eax + (uint32_t)G32(VA_g_map_geometry_buffer);
    int16_t ent_h = *(int16_t *)(uintptr_t)(edi + 0xa);
    if (*(int16_t *)(uintptr_t)(sec + 2) > ent_h) return 1;
    uint32_t lkp = ((uint32_t)*(uint16_t *)(uintptr_t)(edi + 4) << 2) + (uint32_t)G32(VA_g_das_collision_buffer);
    int16_t dxv = (int16_t)(*(int16_t *)(uintptr_t)sec - *(int16_t *)(uintptr_t)lkp);
    if (dxv < ent_h) return 1;
    return 0;
}

/* ============================ Batch 24 (named game logic: codec-build, node setup, pool reset) ====== */

/* build_dpcm_step_table (0x4bb62): one-time builder of g_dpcm_step_table (0x918a4, int32[256]) — the
 * table decode_dpcm_block (0x4e4cd) consumes. Guarded by [0x9196c]!=0 (skip if already built). table[0]=0;
 * then parabolic pairs: ecx += eax>>5; eax += edx (edx starts 0x2d, +=2); store ecx and -ecx, advance.
 * Final lone entry at 0x91ca0. No args (writes the global table). */
void build_dpcm_step_table(void)
{
    if (G32(VA_g_dpcm_step_table + 0xc8) != 0) return;
    uint32_t eax = 0x40, edx = 0x2d, ecx = 0;
    uint32_t *p = (uint32_t *)(uintptr_t)(0x918a4u + OBJ_DELTA);
    uint32_t *end = (uint32_t *)(uintptr_t)(0x91ca0u + OBJ_DELTA);
    *p++ = 0;
    while (p < end) {
        ecx += eax >> 5;
        eax += edx;
        edx += 2;
        p[0] = ecx;
        p[1] = (uint32_t)(-(int32_t)ecx);
        p += 2;
    }
    ecx += eax >> 5;
    *p = ecx;
}

/* setup_sfx_nodes (0x43c46): walks the [0x85c44] record table (count = word[+2], records stride 0x12
 * from +4); for each with byte[+8]&0x80 set AND (byte[+8]&7) > 1, sets word[+0xe] = low16(7*word[+0xc]) >> 1. */
void setup_sfx_nodes(void)
{
    uint8_t *esi = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_sfx_nodes);
    uint32_t ecx = *(uint16_t *)(esi + 2);
    if (ecx == 0) return;
    esi += 4;
    do {
        if ((esi[8] & 0x80) && (esi[8] & 7) > 1) {
            uint16_t x = *(uint16_t *)(esi + 0xc);
            *(uint16_t *)(esi + 0xe) = (uint16_t)((uint16_t)(7u * x) >> 1);
        }
        esi += 0x12;
    } while (--ecx > 0);
}

/* reset_entity_pools (0x4263e): clears pool1 (count [0x90fe0], base 0x90fe4, stride 0x1c: zeroes [+0] of
 * each active entry) and pool2 (count [0x91e00], base 0x91e04, stride 0x22: zeroes [+0] and [+4] of each
 * active entry — "active" tested via [+4]), decrementing the count per active entry; then zeroes both counts. */
void reset_entity_pools(void)
{
    int32_t ecx = (int32_t)G32(VA_g_dynamic_entity_count);
    uint8_t *esi = (uint8_t *)(uintptr_t)(0x90fe4u + OBJ_DELTA);
    do {
        if (*(uint32_t *)esi != 0) { *(uint32_t *)esi = 0; ecx--; }
        esi += 0x1c;
    } while (ecx > 0);
    ecx = (int32_t)G32(VA_g_state_pool_a_count);
    esi = (uint8_t *)(uintptr_t)(0x91e04u + OBJ_DELTA);
    do {
        if (*(uint32_t *)(esi + 4) != 0) { *(uint32_t *)esi = 0; *(uint32_t *)(esi + 4) = 0; ecx--; }
        esi += 0x22;
    } while (ecx > 0);
    G32(VA_g_dynamic_entity_count) = 0;
    G32(VA_g_state_pool_a_count) = 0;
}

/* ---- Batch 25: clean leaves + case-1(self-load FS) palette remap ---- */

/* memcpy_return_dest (0x4f2b7): EAX=dst, EDX=src, EBX=count. Forward copy n>>2 dwords then n&3
 * bytes (es is set = ds, flat). Returns the original dst in EAX. */
uint32_t memcpy_return_dest(uint8_t *dst, const uint8_t *src, uint32_t n)
{
    uint32_t *d32 = (uint32_t *)dst;
    const uint32_t *s32 = (const uint32_t *)src;
    for (uint32_t dw = n >> 2; dw != 0; dw--) *d32++ = *s32++;
    uint8_t *db = (uint8_t *)d32;
    const uint8_t *sb = (const uint8_t *)s32;
    for (uint32_t b = n & 3; b != 0; b--) *db++ = *sb++;
    return (uint32_t)(uintptr_t)dst;
}

/* repair_das_rle_frame_count (0x41631): EAX=block ptr (ushort*), EDX=size limit (bytes). If header
 * flag 0x10 is set, walk frames (stride = int32 at +0xc) while each frame's word[+8] matches the
 * header's word[+8] and stays within [ptr, ptr+size]; if fewer valid frames than the header claims,
 * write the actual count back to word[ptr+8]. */
void repair_das_rle_frame_count(uint16_t *p, int32_t size)
{
    uint16_t cnt = 0;
    if ((*p & 0x10) != 0) {
        uint16_t expect = p[4];                          /* word[+8] */
        uint16_t *q = p;
        uintptr_t end = (uintptr_t)p + (uint32_t)size;   /* ptr + size bytes */
        if ((uintptr_t)p <= end) {
            do {
                if (q[4] != expect) break;
                if (*(int32_t *)(q + 6) < 1) break;      /* int32 at +0xc */
                q = (uint16_t *)((uint8_t *)q + *(int32_t *)(q + 6));
                if (end < (uintptr_t)q) break;
                cnt++;
            } while (cnt < expect);
        }
        if (cnt != expect) p[4] = cnt;
    }
}

/* apply_ui_palette_rect (0x12c36): palette/ramp remap over a framebuffer rectangle. ABI EAX=x0,
 * EDX=y0, EBX=x1, ECX=y1, stack[0]=level (ret 4). Self-loads FS = g_text_color_ramp_selector
 * (0x90c0e, CASE-1) over a 0x2000-byte ramp; each pixel becomes ramp[((level&0x1f)<<8) | pixel].
 * For >=3 rows the top & bottom rows skip their first+last pixel (rounded corners); for <=2 rows
 * every row is full width. `fs` = the ramp buffer (the FS-selector base). Faithful do-while quirks. */
void apply_ui_palette_rect(int32_t x0, int32_t y0, int32_t y1,
                                  uint32_t level, int32_t x1, uint8_t *fs)
{
    int32_t ecx = y1 + 1;
    if (G8(VA_g_hires_line_doubling_flag) != 0) { ecx += ecx; y0 += y0; }      /* g_hires_line_doubling_flag */
    int32_t pitch = (int32_t)G32(VA_g_screen_pitch);               /* g_screen_pitch (signed) */
    int32_t rows  = ecx - y0;
    uint8_t *p = (uint8_t *)(uintptr_t)((uint32_t)G32(VA_g_framebuffer_ptr)
                  + (uint32_t)(y0 * pitch) + (uint32_t)x0);   /* g_framebuffer_ptr + y0*pitch + x0 */
    int32_t width  = (x1 - x0) + 1;
    int32_t stride = pitch - width;                      /* edi: bytes from row end to next row start */
    uint32_t base  = (level & 0x1f) << 8;                /* high bits of the ramp index */

    if (rows < 3) {
        int32_t r = rows;
        do {
            int32_t c = width;
            do { *p = fs[base | *p]; p++; } while (--c > 0);
            p += stride;
        } while (--r > 0);
        return;
    }
    /* rows >= 3 */
    int32_t mid = rows - 2;
    p++;                                                 /* skip top-left corner */
    { int32_t c = width - 2; do { *p = fs[base | *p]; p++; } while (--c > 0); }
    p += stride; p++;
    do {
        int32_t c = width;
        do { *p = fs[base | *p]; p++; } while (--c > 0);
        p += stride;
    } while (--mid > 0);
    p++;                                                 /* skip bottom-left corner */
    { int32_t c = width - 2; do { *p = fs[base | *p]; p++; } while (--c > 0); }
}

/* ---- Batch 26: inventory reset + case-1 self-load SEG cluster ---- */

/* reset_inventory (0x1c57e): g_inventory_count(0x80c2c)=0, then zero 0x400 bytes of g_inventory_slots
 * (0x80c30). Original broadcasts DL=0 and calls the Watcom memset core (0x55240, ECX=0x400 BYTES). */
void reset_inventory(void)
{
    G32(VA_g_inventory_count) = 0;
    uint8_t *p = (uint8_t *)(uintptr_t)(0x80c30u + OBJ_DELTA);
    for (int i = 0; i < 0x400; i++) p[i] = 0;
}

/* fixup_raw_sectors_after_load (0x2f782): self-loads FS = g_geom_selector (0x90be8). off = u16 fs:[4];
 * count = u16 fs:[off-2]; for each of `count` records (stride 0x1a from off), if word fs:[r+0x14]==-3
 * copy word[r+0]->[r+0x10] and word[r+2]->[r+0]. `fs` = the geom selector buffer. Watcom do-while:
 * count==0 still runs the body once. */
void fixup_raw_sectors_after_load(uint8_t *fs)
{
    uint32_t esi = *(uint16_t *)(fs + 4);                 /* offset (zero-extended) */
    int32_t  ecx = *(uint16_t *)(fs + esi - 2);           /* count (zero-extended) */
    do {
        if (*(int16_t *)(fs + esi + 0x14) == -3) {
            uint16_t a = *(uint16_t *)(fs + esi);
            *(uint16_t *)(fs + esi + 0x10) = a;
            a = *(uint16_t *)(fs + esi + 2);
            *(uint16_t *)(fs + esi) = a;
        }
        esi += 0x1a;
    } while (--ecx > 0);
}

/* init_player_position_from_metadata (0x2f8a2): self-loads GS = g_geom_selector (0x90be8). If
 * DAT_89f36 != 0, g_player_sector(0x90c12)=it; then DAT_89f36=0. off = u16 gs:[0xa]; copies metadata
 * INIT_X/Z/Y -> 0x90a8e/0x90a92/0x90a96 and ROTATION -> g_player_angle(0x90a8a). `gs` = selector buf. */
void init_player_position_from_metadata(uint8_t *gs)
{
    uint16_t sec = G16(VA_g_das_special_fat_index + 0x2);
    if (sec != 0) G16(VA_g_player_sector) = sec;
    G16(VA_g_das_special_fat_index + 0x2) = 0;
    uint32_t off = *(uint16_t *)(gs + 0xa);
    G16(VA_g_player_x) = *(uint16_t *)(gs + off + 0);
    G16(VA_g_player_z) = *(uint16_t *)(gs + off + 2);
    G16(VA_g_player_y) = *(uint16_t *)(gs + off + 4);
    G16(VA_g_player_angle) = *(uint16_t *)(gs + off + 6);
}

/* init_map_lighting_from_metadata (0x2f8fa): self-loads GS = g_geom_selector (0x90be8). off = u16
 * gs:[0xa]; copies sky/lighting metadata (+0x12/+0x10/+0x14/+0x18) into globals; selects tint table
 * (0x86d1c) + ramp (0x90c10) from defaults, or the text-ramp/shading-table pair when word[off+0x16]==0. */
void init_map_lighting_from_metadata(uint8_t *gs)
{
    uint32_t off = *(uint16_t *)(gs + 0xa);
    G16(VA_g_das_cache_slots + 0x5d8) = *(uint16_t *)(gs + off + 0x12);
    G16(VA_g_player_sector + 0x4) = *(uint16_t *)(gs + off + 0x10);
    G16(VA_g_player_sector + 0x6) = *(uint16_t *)(gs + off + 0x14);
    G16(VA_g_das_special_fat_index) = *(uint16_t *)(gs + off + 0x18);        /* g_das_special_fat_index */
    int32_t  edx = G32(VA_g_das_remap_chunk_100_b_ptr + 0x4);
    uint16_t ax  = G16(VA_g_gamma_level + 0x2);
    if (*(int16_t *)(gs + off + 0x16) == 0) {
        ax  = G16(VA_g_text_color_ramp_selector);                               /* g_text_color_ramp_selector */
        edx = G32(VA_g_world_shading_table_ptr);
    }
    G32(VA_g_world_tint_table_ptr) = edx;                                   /* g_world_tint_table_ptr */
    G16(VA_g_text_color_ramp_selector + 0x2) = ax;
}

/* ---- Batch 27: Pool allocator constructors (DAS-cache handle pool) ---- */

/* pool_init (0x35b68): construct a Pool over an arena. EAX=pool ptr, EDX=header size, EBX=arena size.
 * avail = size - hdrsize; if avail<=8 return 0. Writes magic 0x506f6f6c, {free,total}=avail @+4/+8,
 * hdrsize @+0xc, cursor(u16)=0 @+0x10, full(u8)=0 @+0x12, used-count=1 @+0x14; zeroes the handle table
 * @+0x18 ((hdrsize-0x18)/4 dwords) unless hdrsize==0x18; lays the initial free chunk at pool+hdrsize
 * {prev=0, size=avail, flags=2(last)}. Returns the pool. */
uint32_t pool_init(uint32_t *pool, int32_t hdrsize, int32_t size)
{
    int32_t avail = size - hdrsize;
    if (avail <= 8) return 0;
    pool[0] = 0x506f6f6cu;                                /* 'Pool' magic */
    pool[1] = (uint32_t)avail;                            /* free */
    pool[2] = (uint32_t)avail;                            /* total free */
    pool[3] = (uint32_t)hdrsize;                          /* header size */
    *((uint8_t  *)pool + 0x12) = 0;                       /* full flag */
    *(uint16_t *)((uint8_t *)pool + 0x10) = 0;            /* cursor */
    pool[5] = 1;                                          /* used count */
    if (hdrsize != 0x18) {
        uint32_t *p = pool + 6;                           /* handle table @+0x18 */
        for (uint32_t n = ((uint32_t)hdrsize - 0x18) >> 2; n != 0; n--) *p++ = 0;
    }
    uint32_t *chunk = (uint32_t *)((uint8_t *)pool + hdrsize);
    chunk[1] = (uint32_t)avail;                           /* chunk size */
    chunk[2] = 2;                                         /* chunk flags = last */
    chunk[0] = 0;                                         /* chunk prev-link */
    return (uint32_t)(uintptr_t)pool;
}

/* pool_create (0x36088): EAX=block, EBX=nhandles, EDX=arena size. If block==0 return 0; else
 * header size = nhandles*4 + 0x18 and tail-call pool_init(block, hdrsize, size). */
uint32_t pool_create(uint32_t *block, int32_t nhandles, int32_t size)
{
    if (block == 0) return 0;
    int32_t hdrsize = nhandles * 4 + 0x18;
    return pool_init(block, hdrsize, size);
}

/* Shared chunk-free + bidirectional-coalesce body (the bytes at 0x35d9b..0x35e13 that pool_free_chunk
 * 0x35d80 and pool_release_chunk 0x35d96 both run after their differing prologues). Chunk header 0x10B:
 * +0 prev-link, +4 size (incl. header; next=chunk+size), +8 flags (bit0=allocated, bit1=last). Clears
 * flag bits 0/2/3/4, returns size to pool total-free (+8), merges an unallocated prev and/or next
 * neighbor (fixing the far neighbor's prev-link or the last-flag, used-count-- per merge), then bumps
 * largest-free (+4). `pool` has already passed get_pool_descriptor (identity when checks off). */
static void pool_coalesce_chunk_body(uint32_t *pool, uint8_t *block)
{
    uint8_t *chunk = block - 0x10;
    *(uint32_t *)(chunk + 8) &= 0xffffffe2u;             /* clear flag bits 0,2,3,4 */
    pool[2] += *(uint32_t *)(chunk + 4);                 /* total-free += size */

    if (*(uint32_t *)(chunk + 0) != 0) {                 /* prev-link present */
        uint8_t *prev = (uint8_t *)(uintptr_t)*(uint32_t *)(chunk + 0);
        if (!(*(uint8_t *)(prev + 8) & 1)) {             /* prev not allocated -> merge into prev */
            if (*(uint8_t *)(chunk + 8) & 2) {           /* this is last */
                *(uint8_t *)(prev + 8) |= 2;
            } else {
                uint8_t *next = chunk + *(uint32_t *)(chunk + 4);
                *(uint32_t *)(next + 0) = (uint32_t)(uintptr_t)prev;
            }
            *(uint32_t *)(prev + 4) += *(uint32_t *)(chunk + 4);
            pool[5] -= 1;                                /* used-count-- */
            chunk = prev;
        }
    }
    if (!(*(uint8_t *)(chunk + 8) & 2)) {                /* this not last */
        uint8_t *next = chunk + *(uint32_t *)(chunk + 4);
        if (!(*(uint8_t *)(next + 8) & 1)) {             /* next not allocated -> merge next in */
            if (*(uint8_t *)(next + 8) & 2) {            /* next is last */
                *(uint8_t *)(chunk + 8) |= 2;
            } else {
                uint8_t *nn = next + *(uint32_t *)(next + 4);
                *(uint32_t *)(nn + 0) = (uint32_t)(uintptr_t)chunk;
            }
            *(uint32_t *)(chunk + 4) += *(uint32_t *)(next + 4);
            pool[5] -= 1;                                /* used-count-- */
        }
    }
    uint32_t finalsz = *(uint32_t *)(chunk + 4);
    if (pool[1] <= finalsz) pool[1] = finalsz;           /* largest-free (unsigned ja) */
}

/* pool_free_chunk (0x35d80): EAX=pool, EDX=block. Gated to raw hdrsize==0x18 pools + null-checks, then
 * the shared coalesce body. Returns 0. */
uint32_t pool_free_chunk(uint32_t *pool, uint8_t *block)
{
    if (pool == 0 || block == 0) return 0;
    if (pool[3] != 0x18) return 0;                       /* [pool+0xc] != 0x18 */
    pool_coalesce_chunk_body(pool, block);               /* get_pool_descriptor is identity (checks off) */
    return 0;
}

/* pool_release_chunk (0x35d96): the second entry into pool_free_chunk's body (pool_free_handle's
 * worker) — no hdrsize gate / null-checks (entered past them), so it also serves handle pools.
 * EAX=pool, EDX=block. Returns 0. */
uint32_t pool_release_chunk(uint32_t *pool, uint8_t *block)
{
    pool_coalesce_chunk_body(pool, block);               /* get_pool_descriptor is identity (checks off) */
    return 0;
}

/* pool_recompute_max_free (FUN_00035d52, proposed name): ESI=pool. Walks all `used-count` chunks from
 * pool+hdrsize, records the max free-chunk size into pool largest-free (+4), and (re-)marks the last
 * chunk's last-flag (bit1). Shared by pool_carve_chunk / pool_find_free_chunk. */
void pool_recompute_max_free(uint8_t *pool)
{
    uint32_t maxfree = 0;
    uint8_t *p = pool + *(int32_t *)(pool + 0xc);        /* first chunk */
    int32_t cnt = *(int32_t *)(pool + 0x14);             /* used-count */
    uint32_t sz;
    do {
        sz = *(uint32_t *)(p + 4);
        if (!(*(uint8_t *)(p + 8) & 1) && maxfree <= sz) maxfree = sz;
        p += sz;
    } while (--cnt > 0);
    p -= sz;                                             /* back up to the last chunk */
    *(uint8_t *)(p + 8) |= 2;                            /* mark last */
    *(uint32_t *)(pool + 4) = maxfree;                   /* largest-free */
}

/* pool_free_handle (0x360b3): free a relocatable block via its handle — THE object free (47 callers).
 * EAX=pool, EDX=&handle slot (the slot holds the block ptr). Guards (handle, pool, *handle nonzero,
 * hdrsize>0x18), restores the pool cursor (+0x10) from the chunk's stored handle index (block-6),
 * releases+coalesces the chunk (pool_release_chunk), then clears the handle slot. Returns 0. */
uint32_t pool_free_handle(uint32_t *pool, uint32_t *handle)
{
    if (handle == 0 || pool == 0 || *handle == 0) return 0;
    if ((int32_t)pool[3] <= 0x18) return 0;              /* hdrsize > 0x18 (jle) */
    uint8_t *block = (uint8_t *)(uintptr_t)*handle;
    *(uint16_t *)((uint8_t *)pool + 0x10) = *(uint16_t *)(block - 6);   /* cursor = chunk handle index */
    pool_release_chunk(pool, block);
    *handle = 0;
    return 0;
}

/* pool_find_free_chunk (0x35cb4): raw-pool (hdrsize==0x18) allocator. EAX=pool, EDX=size. needed =
 * align4(size)+0x10. LAST-fit walk over used-count chunks; carves the ALLOCATED chunk from the HIGH
 * end of the chosen free chunk (the free remainder stays at the low address). Splits only when the
 * remainder >= 0x18, else takes the whole chunk. Returns the carved data ptr (chunk+0x10), or 0 (too
 * small / no fit), or the pool ptr unchanged when hdrsize!=0x18. */
uint32_t pool_find_free_chunk(uint32_t *pool, int32_t size)
{
    if (pool[3] != 0x18) return (uint32_t)(uintptr_t)pool;          /* gate fail -> returns the pool ptr */
    uint32_t needed = ((uint32_t)(size + 3) & 0xfffffffcu) + 0x10;
    if ((uint32_t)pool[1] < needed) return 0;                       /* largest-free < needed */
    uint8_t *edi = (uint8_t *)pool + pool[3];                       /* first chunk */
    int32_t cnt = pool[5];
    uint8_t *cand = 0;
    do {
        if (!(*(uint8_t *)(edi + 8) & 1) && (uint32_t)*(uint32_t *)(edi + 4) >= needed)
            cand = edi;                                             /* last fitting free chunk */
        edi += *(uint32_t *)(edi + 4);
    } while (--cnt > 0);
    if (cand == 0) return 0;
    edi = cand;
    int32_t remainder = (int32_t)(*(uint32_t *)(edi + 4) - needed);
    uint32_t taken = needed;
    if (remainder < 0x18) { taken = *(uint32_t *)(edi + 4); remainder = 0; }   /* take whole */
    pool[2] -= taken;                                               /* total-free -= taken */
    uint8_t *ebx = edi;
    if (remainder != 0) {                                           /* split: carved = high part */
        *(uint32_t *)(edi + 4) = (uint32_t)remainder;               /* low remainder stays free */
        ebx = edi + remainder;
        *(uint32_t *)(ebx + 0) = (uint32_t)(uintptr_t)edi;          /* carved.prev = remainder */
        pool[5] += 1;                                               /* used-count++ */
        if (!(*(uint32_t *)(edi + 8) & 2)) {                        /* remainder not last */
            uint8_t *nxt = ebx + taken;
            *(uint32_t *)(nxt + 0) = (uint32_t)(uintptr_t)ebx;      /* next.prev = carved */
        }
        *(uint32_t *)(edi + 8) &= 0xfffffffdu;                      /* remainder not last */
    }
    *(uint32_t *)(ebx + 4) = taken;                                 /* carved.size */
    *(uint32_t *)(ebx + 8) = 1;                                     /* carved allocated */
    *(uint32_t *)(ebx + 0xc) = 0;                                   /* relocation callback = 0 */
    *(uint32_t *)(ebx + 8) &= 0xfffffffdu;                          /* clear last */
    pool_recompute_max_free((uint8_t *)pool);
    return (uint32_t)(uintptr_t)(ebx + 0x10);
}

/* pool_carve_chunk (0x35c1d): the Pool's inner malloc for handle pools. EAX=pool (via the identity
 * validator), EDX=size. needed = align4(size)+0x10. FIRST-fit walk; carves the ALLOCATED chunk from
 * the LOW end (the candidate becomes allocated; the free remainder is split off at the HIGH end).
 * Returns the carved data ptr (chunk+0x10) or 0. */
uint32_t pool_carve_chunk(uint32_t *pool, int32_t size)
{
    uint32_t needed = ((uint32_t)(size + 3) & 0xfffffffcu) + 0x10;
    if (needed <= (uint32_t)pool[1]) {                              /* largest-free >= needed */
        uint8_t *c = (uint8_t *)pool + pool[3];                     /* first chunk */
        int32_t cnt = pool[5];
        do {
            if (!(*(uint8_t *)(c + 8) & 1) && needed <= (uint32_t)*(uint32_t *)(c + 4)) {   /* first fit */
                int32_t remainder = (int32_t)(*(uint32_t *)(c + 4) - needed);
                uint32_t taken = needed;
                if (remainder < 0x18) { taken = *(uint32_t *)(c + 4); remainder = 0; }
                pool[2] -= taken;                                   /* total-free -= taken */
                uint8_t *rem = c + taken;                           /* remainder chunk = high part */
                if (remainder != 0) {                               /* split */
                    *(uint32_t *)(rem + 0) = (uint32_t)(uintptr_t)c;        /* remainder.prev = candidate */
                    *(uint32_t *)(rem + 8) = 0;                     /* remainder free */
                    *(uint32_t *)(rem + 4) = *(uint32_t *)(c + 4) - taken;  /* remainder.size */
                    pool[5] += 1;                                   /* used-count++ */
                    if (!(*(uint32_t *)(c + 8) & 2)) {              /* candidate not last */
                        uint8_t *nxt = rem + *(uint32_t *)(rem + 4);
                        *(uint32_t *)(nxt + 0) = (uint32_t)(uintptr_t)rem;
                    }
                }
                *(uint32_t *)(c + 4) = taken;                       /* candidate.size = taken */
                *(uint32_t *)(c + 8) |= 1;                          /* allocated */
                *(uint32_t *)(c + 8) &= 0xfffffffdu;                /* not last */
                *(uint32_t *)(c + 0xc) = 0;
                pool_recompute_max_free((uint8_t *)pool);
                return (uint32_t)(uintptr_t)(c + 0x10);
            }
            c += *(uint32_t *)(c + 4);
        } while (--cnt > 0);
    }
    return 0;
}

/* Shared tail of pool_alloc_handle / pool_alloc_handle_sized (the carve+store body both reach, the
 * sized variant via a jump into 0x36167): pick a free handle slot (round-robin from cursor +0x10 over
 * the handle table +0x18, else scan from slot 0), advance the cursor, carve a chunk; on carve failure
 * coalesce + retry when `allow_coalesce` (the full hub) and the pool isn't flagged full (+0x12). Stores
 * the slot index into the chunk's handle field (chunk-6), sets handle_table[slot]=chunk, returns &slot. */
static uint32_t pool_alloc_common(uint32_t *pool, int32_t size, int allow_coalesce)
{
    uint32_t nslots = ((uint32_t)pool[3] - 0x18) >> 2;
    uint32_t cursor = (uint16_t)pool[4];                     /* pool[+0x10] & 0xffff */
    uint32_t *htab = pool + 6;                               /* handle table @ +0x18 */
    uint32_t slot;
    if (htab[cursor] == 0) {
        slot = cursor;
    } else {                                                 /* scan from slot 0 for the first free slot */
        uint32_t i;
        for (i = 0; i < nslots; i++) if (htab[i] == 0) break;
        if (i >= nslots) return 0;
        slot = i;
    }
    uint32_t nextcur = slot + 1;
    if (nextcur >= nslots) nextcur = 0;                      /* wrap cursor */
    *(uint16_t *)((uint8_t *)pool + 0x10) = (uint16_t)nextcur;
    uint32_t chunk = pool_carve_chunk(pool, size);
    if (chunk == 0) {
        if (!allow_coalesce) return 0;
        if (*((uint8_t *)pool + 0x12) != 0) return 0;        /* pool flagged full */
        pool_coalesce_free(pool);   /* now lifted (Batch 33) — direct C call, no bridge */
        chunk = pool_carve_chunk(pool, size);         /* retry */
        if (chunk == 0) return 0;
    }
    *(uint16_t *)((uintptr_t)chunk - 6) = (uint16_t)slot;    /* chunk handle index */
    htab[slot] = chunk;
    return (uint32_t)(uintptr_t)&htab[slot];
}

/* pool_alloc_handle (0x360f9): THE relocatable-object allocator (26 callers; all pass
 * g_das_cache_heap_handle 0x85c3c). EAX=pool, EDX=size. Bounded by total-free (+8); carve, and on
 * failure coalesce + retry. Returns the handle (&slot) or 0. */
uint32_t pool_alloc_handle(uint32_t *pool, int32_t size)
{
    if (size == 0 || pool == 0) return 0;
    if ((int32_t)pool[3] <= 0x18) return 0;                  /* hdrsize > 0x18 */
    if ((uint32_t)pool[2] <= (uint32_t)size) return 0;       /* total-free > size (jbe) */
    return pool_alloc_common(pool, size, 1);
}

/* pool_alloc_handle_sized (0x3618c): variant bounded by largest-free (+4) instead of total-free, and
 * with NO coalesce retry (shares pool_alloc_handle's carve+store tail). EAX=pool, EDX=size. */
uint32_t pool_alloc_handle_sized(uint32_t *pool, int32_t size)
{
    if (size == 0 || pool == 0) return 0;
    if ((int32_t)pool[3] <= 0x18) return 0;
    if ((uint32_t)pool[1] <= (uint32_t)size) return 0;       /* largest-free > size (jbe) */
    return pool_alloc_common(pool, size, 0);
}

/* pool_coalesce_free (0x361f7): compacting collector. EAX=pool. No-ops if the pool is flagged full
 * (+0x12) or has a single chunk (+0x14==1). Walks the chunk list with a read cursor (rd) and a write
 * cursor (wr): allocated chunks are slid down to wr (rewriting handle_table[chunk handle-idx @+0xa] to
 * the moved data ptr, clearing last + setting bit2, and — only when flag bit3 is set — invoking the
 * per-chunk relocation callback at moved[+0xc] with (new_data, old_data)); free chunks accumulate their
 * size and advance only rd (squeezing the gap). All freed space becomes one trailing free chunk (size =
 * accumulated, flags=last), and largest-free (+4) is set to it. Returns the pool. The `rep movsd`
 * forward copy also advances rd/wr past the moved chunk. */
uint32_t pool_coalesce_free(uint32_t *pool)
{
    uint8_t *poolb = (uint8_t *)pool;
    if (*(uint8_t *)(poolb + 0x12) != 0) return (uint32_t)(uintptr_t)pool;   /* full flag */
    if (pool[5] == 1) return (uint32_t)(uintptr_t)pool;                      /* used-count == 1 */
    uint32_t accumulated = 0;                            /* ebp */
    uint8_t *rd = poolb + pool[3];                       /* esi: read cursor (first chunk) */
    uint8_t *wr = rd;                                    /* edi: write cursor */
    uint8_t *htab = poolb + 0x18;                        /* handle table base */
    int32_t cnt = (int32_t)pool[5];                      /* ecx: old used-count */
    pool[5] = 0;                                         /* reset used-count (xchg) */
    uint32_t prevlink = 0;                               /* edx: prev free-list link */
    do {
        if (*(uint8_t *)(rd + 8) & 1) {                  /* ALLOCATED -> compact down */
            pool[5] += 1;
            *(uint32_t *)(rd + 0) = prevlink;            /* chunk.prev = prevlink */
            prevlink = (uint32_t)(uintptr_t)wr;
            if (rd == wr) {                              /* already in place */
                rd += *(uint32_t *)(rd + 4);
                wr = rd;
            } else {                                     /* move chunk down to wr */
                uint16_t idx = *(uint16_t *)(rd + 0xa);
                *(uint32_t *)(htab + (uint32_t)idx * 4) = (uint32_t)(uintptr_t)(wr + 0x10);
                *(uint8_t *)(rd + 8) &= 0xfd;            /* clear last */
                *(uint8_t *)(rd + 8) |= 4;              /* set bit2 */
                uint32_t ndw = *(uint32_t *)(rd + 4) >> 2;
                int has_cb = (*(uint8_t *)(rd + 8) & 8) != 0;
                uint8_t *old_rd = rd, *old_wr = wr;
                for (uint32_t i = 0; i < ndw; i++) { *(uint32_t *)wr = *(uint32_t *)rd; wr += 4; rd += 4; }
                if (has_cb) {                            /* relocation callback (only if bit3) */
                    void (*cb)(void *, void *) = *(void (**)(void *, void *))(old_wr + 0xc);
                    cb(old_wr + 0x10, old_rd + 0x10);
                }
            }
        } else if (*(uint8_t *)(rd + 8) & 2) {           /* FREE + last */
            accumulated += *(uint32_t *)(rd + 4);
            break;
        } else {                                         /* FREE, not last */
            accumulated += *(uint32_t *)(rd + 4);
            rd += *(uint32_t *)(rd + 4);                 /* advance read only (gap squeezed) */
        }
    } while (--cnt > 0);
    if (accumulated != 0) {                              /* emit the trailing free chunk */
        pool[5] += 1;
        *(uint32_t *)(wr + 0) = prevlink;
        pool[1] = accumulated;                           /* largest-free */
        *(uint32_t *)(wr + 4) = accumulated;
        *(uint32_t *)(wr + 8) = 2;                       /* free + last */
        prevlink = (uint32_t)(uintptr_t)wr;
    }
    *(uint32_t *)((uintptr_t)prevlink + 8) = 2;          /* mark last ([edx+8]=2) */
    return (uint32_t)(uintptr_t)pool;
}

/* ---- Batch 34: dbase100 id lookup + entity-player contact ---- */

/* lookup_dbase100_record_by_id (0x1dcac): EAX=id. For ids >= 0x200, scans g_dbase100_inventory_table
 * (0x81e20; entries read from index 1, pre-incremented) for the first non-zero offset whose record
 * (g_dbase100_base 0x81e1c + offset) has word[+2] (sign-extended) == id; returns that record ptr, else
 * 0. base/count (count=[base+0x10]) are re-read each iteration. */
uint32_t lookup_dbase100_record_by_id(uint32_t id)
{
    int32_t *tab = (int32_t *)(uintptr_t)(uint32_t)G32(VA_g_dbase100_inventory_table);
    if (id < 0x200) return 0;
    for (int32_t i = 0; ; i++) {
        uint32_t base = (uint32_t)G32(VA_g_dbase100_base);
        if (i >= *(int32_t *)(uintptr_t)(base + 0x10)) return 0;
        tab++;                                            /* add eax,4 (pre-increment) */
        if (*tab != 0) {
            uint32_t rec = base + (uint32_t)*tab;
            if ((int32_t)*(int16_t *)(uintptr_t)(rec + 2) == (int32_t)id)
                return rec;
        }
    }
}

/* check_entity_player_contact (0x43413): ESI=entity record, EDI=point (int16[]). Chebyshev
 * max(|x-px|,|y-py|) minus a radius (0x30 if entity[+9]&0x80 else 0x60) minus the per-type radius
 * word[g_das_collision_buffer(0x85c50) + point[2]*4 + 2]; if that is < 0 (xy overlap) and |z-pz| <=
 * 0xaa, returns -1 (contact), else returns the running value. Faithfully models the 16-bit partial
 * ops over the 32-bit EAX (the high half carries from the EAX argument through the `neg`/`sub eax,edx`
 * steps). Player pos: x@0x90a8e, y@0x90a96, z@0x90a92. */
/* The sole caller (update_actor_movement_ai 0x43326) branches on `js` — the SIGN FLAG the callee's
 * LAST 16-bit op leaves, which is NOT derivable from the 32-bit return (two exits can produce the
 * same EAX with different SF). Exported here so the lifted caller can reproduce the branch:
 *   xy-no-overlap exit (jns taken)  -> SF = 0
 *   z-no-contact exit (ja after cmp ax,0xaa) -> SF = bit15 of (|dz| - 0xaa)
 *   contact exit (or eax,-1)        -> SF = 1 */
int g_os_contact_sf;

int32_t check_entity_player_contact(uint32_t eax, const uint8_t *esi, const int16_t *edi)
{
    /* ax = |point.x - player_x| (16-bit sub, full-width neg) */
    eax = (eax & 0xffff0000u) | (uint16_t)edi[0];
    {
        uint16_t lo = (uint16_t)((uint16_t)eax - (uint16_t)G16(VA_g_player_x));
        eax = (eax & 0xffff0000u) | lo;
        if (lo & 0x8000u) eax = (uint32_t)(-(int32_t)eax);
    }
    /* dy = |point.y - player_y| (only the low 16 bits are used downstream) */
    uint16_t dy = (uint16_t)((uint16_t)edi[1] - (uint16_t)G16(VA_g_player_y));
    if (dy & 0x8000u) dy = (uint16_t)(-(int16_t)dy);
    /* ax = signed-max(ax, dy): cmp ax,dx; jg keeps ax, else mov ax,dx */
    if (!((int16_t)(uint16_t)eax > (int16_t)dy))
        eax = (eax & 0xffff0000u) | dy;
    /* eax -= radius (FULL 32-bit subtract) */
    eax -= (esi[9] & 0x80) ? 0x30u : 0x60u;
    /* eax.lo -= per-type radius (16-bit); jns (sign clear) -> no xy overlap, return */
    {
        uint32_t type = (uint16_t)edi[2];
        uint16_t ptr  = *(uint16_t *)(uintptr_t)((uint32_t)G32(VA_g_das_collision_buffer) + type * 4 + 2);
        uint16_t lo   = (uint16_t)((uint16_t)eax - ptr);
        int neg = (lo & 0x8000u) != 0;
        eax = (eax & 0xffff0000u) | lo;
        if (!neg) { g_os_contact_sf = 0; return (int32_t)eax; }   /* jns exit: SF clear */
    }
    /* ax = |point.z - player_z| (16-bit sub, full-width neg) */
    eax = (eax & 0xffff0000u) | (uint16_t)edi[5];
    {
        uint16_t lo = (uint16_t)((uint16_t)eax - (uint16_t)G16(VA_g_player_z));
        eax = (eax & 0xffff0000u) | lo;
        if (lo & 0x8000u) eax = (uint32_t)(-(int32_t)eax);
    }
    if ((uint16_t)eax > 0xaa) {                           /* ja: |dz| > 0xaa -> no contact */
        g_os_contact_sf = (int)(((uint16_t)((uint16_t)eax - 0xaau) >> 15) & 1u); /* cmp SF */
        return (int32_t)eax;
    }
    g_os_contact_sf = 1;                                /* or eax,-1 -> SF set */
    return -1;                                            /* contact */
}

/* ---- Batch 35: combat — entity-sector refresh + projectile hit damage ---- */

/* revalidate_entity_def (0x426fc): ESI=entity-context ptr, EBX=current entity-DEF record. If the
 * cached expected def id (word[esi+0x60]) already matches word[ebx+4] (the def record's id), it's a
 * no-op success (CF clear). Otherwise it re-resolves the def through entity_def_cache_lookup
 * (FUN_0001e2f6 — a stateful MRU def cache; bridged to the original here), stores the result at [esi],
 * and returns CF set (fail) iff the lookup returned 0. Returns CF (0=success, 1=fail). (The original
 * also leaves the def ptr in EBX on the lookup path; not modeled — callers that take the no-op path
 * keep their own EBX.) (It's an entity-DEF cache revalidation, not a sector lookup.) */
int revalidate_entity_def(uint8_t *esi, uint8_t *ebx)
{
    uint16_t cached = *(uint16_t *)(esi + 0x60);
    if (cached == *(uint16_t *)(ebx + 4)) return 0;          /* clc — cached def id still valid */
    uint32_t r = entity_def_cache_lookup(cached);     /* re-pointed: was call_asm VA 0x1e2f6 */
    *(uint32_t *)esi = r;
    return (r == 0) ? 1 : 0;                                 /* stc on miss, else clc */
}

/* compute_projectile_hit_damage (0x427f3): EAX=param1 (ptr->ptr->target entity), EDX=damage type. The
 * target = **param1; if null (or the sector refresh fails) returns the base damage word[*0x90fd8+0x16].
 * Otherwise ebp starts at 1; if any entry in the target's resist list (count word[+0x2c], entries from
 * +0x2e) equals -1 or the damage type, ebp=0 (immune); then for each entry in the vuln list (count
 * word[+0x46], entries from +0x48) equal to -1 or the type, ebp = (ebp?ebp*2:1). Returns
 * base * ebp. (List scans are signed do-while; list1 breaks on first match, list2 does not.) */
uint32_t compute_projectile_hit_damage(uint32_t *param1, uint16_t dx)
{
    uint8_t *esi = (uint8_t *)(uintptr_t)*param1;
    uint8_t *ebx = (uint8_t *)(uintptr_t)*(uint32_t *)esi;
    uint32_t base = *(uint16_t *)(uintptr_t)((uint32_t)G32(VA_g_dos_dta_name + 0x6e) + 0x16);
    if (ebx == 0) return base;
    if (revalidate_entity_def(esi, ebx)) return base;
    int32_t ebp = 1;
    int32_t c = (int32_t)*(uint16_t *)(ebx + 0x2c);          /* resist list */
    if (c != 0) {
        const int16_t *p = (const int16_t *)(ebx + 0x2e);
        do {
            if (*p == -1 || *p == (int16_t)dx) { ebp = 0; break; }
            p++;
        } while (--c > 0);
    }
    c = (int32_t)*(uint16_t *)(ebx + 0x46);                  /* vuln list */
    if (c != 0) {
        const int16_t *p = (const int16_t *)(ebx + 0x48);
        do {
            if (*p == -1 || *p == (int16_t)dx) ebp = (ebp != 0) ? ebp * 2 : 1;
            p++;
        } while (--c > 0);
    }
    return base * (uint32_t)ebp;
}

/* ---- Batch 36: config asset-name + small leaves ---- */

/* set_cfg_asset_name (0x10584): ESI=src, EDI=dst, EDX=ext. If src!=0, copy 0x14 dwords (80 bytes)
 * src->dst, then if EDX!=0 apply the extension via set_filename_extension(dst, ext). */
void set_cfg_asset_name(const uint8_t *esi, uint8_t *edi, uint32_t edx_ext)
{
    if (esi == 0) return;
    const uint32_t *s = (const uint32_t *)esi;
    uint32_t *d = (uint32_t *)edi;
    for (int i = 0; i < 0x14; i++) *d++ = *s++;          /* rep movsd ecx=0x14 */
    if (edx_ext != 0)
        set_filename_extension((char *)edi, edx_ext);
}

/* rng_next (0x4b4cb, FUN_0004b4cb): 16-bit LCG over g_rng_state (0x7276c): state = state*0x5e5 + 0x29;
 * returns it in AX (EAX high half carries from the caller's EAX). */
uint32_t rng_next(uint32_t eax)
{
    uint16_t g = (uint16_t)((uint16_t)G16(VA_g_rng_state) * 0x5e5 + 0x29);
    G16(VA_g_rng_state) = g;
    return (eax & 0xffff0000u) | g;
}

/* clear_list_field30 (0x4b378, FUN_0004b378): if the list head g_list_91864 (0x91864) is non-null,
 * clears it, then walks the chain (next = node[0]) zeroing byte[node+0x30] of each node. */
void clear_list_field30(void)
{
    uint8_t *p = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_particle_pool);
    if (p == 0) return;
    G32(VA_g_particle_pool) = 0;
    do {
        p[0x30] = 0;
        p = (uint8_t *)(uintptr_t)*(uint32_t *)p;
    } while (p != 0);
}

/* bounded_string_copy (0x27e0b, FUN_00027e0b): EBP=ptr to the src pointer, EDI=dst, CL=max count.
 * Copies bytes from *EBP to dst until a NUL or CL bytes (the count check is post-copy, so a non-NUL
 * first byte is always copied even when CL==0). */
void bounded_string_copy(const uint32_t *ebp, uint8_t *edi, int8_t cl)
{
    const uint8_t *src = (const uint8_t *)(uintptr_t)*ebp;
    for (;;) {
        uint8_t c = *src;
        if (c == 0) return;
        *edi++ = c; src++;
        if (!(--cl > 0)) return;                         /* dec cl; jg */
    }
}

/* ---- Batch 37: RNG-range, dual-array clear, RLE literal-run emitters ---- */

/* rng_range (0x16d79, FUN): EAX=range. Advances the 16-bit LCG g_rng_state2 (0x7fe08) then returns
 * (state * range) >> 16 — i.e. a uniform value in [0, range). */
uint32_t rng_range(uint32_t range)
{
    uint16_t g = (uint16_t)((uint16_t)G16(VA_g_rng_state2) * 0x5e5 + 0x29);
    G16(VA_g_rng_state2) = g;
    return ((uint32_t)g * range) >> 16;
}

/* clear_dual_array_80afc (0x1c59e, FUN): zeroes 5 dwords at 0x80afc and 5 dwords at 0x80b10. */
void clear_dual_array_80afc(void)
{
    uint32_t *a = (uint32_t *)(uintptr_t)(0x80afcu + OBJ_DELTA);
    uint32_t *b = (uint32_t *)(uintptr_t)(0x80b10u + OBJ_DELTA);
    for (int i = 0; i < 5; i++) { a[i] = 0; b[i] = 0; }
}

/* emit_literal_run_3cf86 (0x3cf86, FUN): ByteRun1 literal-run emit. EBX=count, ESI=end-of-run ptr,
 * EDI=dst. If count==0, no-op. Else writes a (count-1) header byte then copies the `count` bytes ending
 * at ESI (src = ESI-count). Returns the advanced EDI (ESI ends back at its input; EBX=0; EAX preserved). */
uint8_t *emit_literal_run_3cf86(uint32_t count, uint8_t *esi_end, uint8_t *edi)
{
    if (count == 0) return edi;
    const uint8_t *src = esi_end - count;
    *edi++ = (uint8_t)(count - 1);
    for (uint32_t i = 0; i < count; i++) *edi++ = *src++;
    return edi;
}

/* emit_literal_run_4ee9c (0x4ee9c, FUN): sibling emitter. EBP=count, ESI=end ptr, EDI=dst. Writes a
 * `count` header byte then copies `count` bytes from src=ESI-count-1. Returns advanced EDI (ESI ends
 * back at its input; EBP=0; EAX=count). Even count==0 writes the 0 header. */
uint8_t *emit_literal_run_4ee9c(uint32_t count, uint8_t *esi_end, uint8_t *edi)
{
    const uint8_t *src = esi_end - count - 1;
    *edi++ = (uint8_t)count;
    for (uint32_t i = 0; i < count; i++) *edi++ = *src++;
    return edi;
}

/* ---- Batch 38: table searches + locomotion reset ---- */

/* is_in_83ed4_table (0x2778d, FUN): EAX=key. Returns 1 if any of the 16 records at 0x83ed4 (stride
 * 0x9a) has its first dword == key, else 0. */
uint32_t is_in_83ed4_table(uint32_t key)
{
    uint8_t *p = (uint8_t *)(uintptr_t)(0x83ed4u + OBJ_DELTA);
    for (int i = 0; i < 0x10; i++) {
        if (*(uint32_t *)p == key) return 1;
        p += 0x9a;
    }
    return 0;
}

/* find_sfx_node_by_key (0x43b0b, FUN): AX=key. Scans the [0x85c44] record table (count word[+2],
 * records stride 0x12 from +4, key word[+6]) for word[rec+6]==key; returns the record ptr or 0. */
uint32_t find_sfx_node_by_key(uint16_t key)
{
    uint8_t *base = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_sfx_nodes);
    if (base == 0) return 0;
    uint32_t cnt = *(uint16_t *)(base + 2);
    if (cnt == 0) return 0;
    uint8_t *rec = base + 4;
    do {
        if (*(uint16_t *)(rec + 6) == key) return (uint32_t)(uintptr_t)rec;
        rec += 0x12;
    } while (--cnt > 0);
    return 0;
}

/* reset_player_locomotion_state (0x1c96f, FUN): zeroes byte[0x819c0] + dwords at 0x819c1/c5/cd/d1
 * (player locomotion/airborne flags). */
void reset_player_locomotion_state(void)
{
    G8(VA_g_player_locomotion_flags) = 0;
    G32(VA_g_player_airborne) = 0;
    G32(VA_g_player_airborne + 0x4) = 0;
    G32(VA_g_player_airborne + 0xc) = 0;
    G32(VA_g_player_airborne + 0x10) = 0;
}

/* ======================= Batch 39 — game-heap free + isqrt completion ====== */

/* game_heap_free (0x15191): free a block through the pool handle in
 * g_game_heap_handle (0x7f374). Marshals EAX(block) -> pool_free_chunk(EAX=handle,
 * EDX=block). The handle is a runtime pool pointer (raw, not OBJ_DELTA-relative). */
void game_heap_free(uint8_t *block)
{
    uint32_t handle = (uint32_t)G32(VA_g_game_heap_handle);       /* mov ebx,[0x47f374] */
    pool_free_chunk((uint32_t *)(uintptr_t)handle, block);
}

/* pool_alloc_checked (0x35c03): validating prologue that falls through into
 * pool_carve_chunk (0x35c1d). Rejects with EAX=0 if pool==0, size==0, or the pool's
 * hdrsize field [pool+0xc] != 0x18; otherwise carves. EAX=pool, EDX=size -> EAX. */
uint32_t pool_alloc_checked(uint32_t pool, int32_t size)
{
    if (pool == 0)                                    return 0;  /* test eax; je (eax=0)        */
    if (size == 0)                                    return 0;  /* test edx; je (sub eax,eax)  */
    if (*(uint32_t *)(uintptr_t)(pool + 0xc) != 0x18) return 0;  /* cmp [eax+0xc],0x18; jne     */
    return pool_carve_chunk((uint32_t *)(uintptr_t)pool, size);   /* fall through */
}

/* game_heap_alloc (0x1517d): allocate `size` bytes from the pool in g_game_heap_handle
 * (0x7f374). Marshals EAX(size) -> pool_alloc_checked(EAX=handle, EDX=size); the
 * ECX/EBX passthrough seen in the decompile is dead. Returns the chunk ptr (or 0). */
uint32_t game_heap_alloc(int32_t size)
{
    uint32_t handle = (uint32_t)G32(VA_g_game_heap_handle);       /* mov ebx,[0x47f374] */
    return pool_alloc_checked(handle, size);
}

/* ======================= Batch 41 — flat pure leaves ====================== */

/* recompute_hires_line_doubling (0x2fd3c): if g_rawscreen_flag(0x90c08)&1, clamp the
 * mode word [0x90be6] to its low 2 bits; then set g_hires_line_doubling_flag(0x90cbd)
 * to 0xff iff [0x90be6]&4 (after the clamp) else 0; finally clear [0x90be4]. */
void recompute_hires_line_doubling(void)
{
    if (G8(VA_g_rawscreen_flag) & 1)
        G16(VA_g_video_mode_flags) &= 3;
    G8(VA_g_hires_line_doubling_flag) = (G16(VA_g_video_mode_flags) & 4) ? 0xff : 0x00;
    G16(VA_g_vga_mode_configured) = 0;
}

/* arm_weapon_fire (0x17629): clears [0x7fdf0]; then on the state word [0x7fddc]:
 * ==0 -> set timer [0x7fde0]=100 and state=2; ==1 -> state=2; anything else -> leave
 * state (only [0x7fdf0] was cleared). (Sibling alt-entry 0x1765c jumps into the body.) */
void arm_weapon_fire(void)
{
    G32(VA_g_weapon_fire_lock + 0x20) = 0;
    int32_t s = G32(VA_g_weapon_fire_lock + 0xc);
    if (s == 0)
        G32(VA_g_weapon_fire_lock + 0x10) = 0x64;
    else if (s != 1)
        return;
    G32(VA_g_weapon_fire_lock + 0xc) = 2;
}

/* fetch_dbcs_char (0x57422): EAX=char ptr. If DBCS is enabled ([0x758c8]!=0) and the
 * lead-byte table bit (byte[0x758cd + *p] & 1) is set, return the wide char
 * (*p<<8 | p[1]); otherwise return the single byte *p. */
uint32_t fetch_dbcs_char(uint8_t *p)
{
    uint32_t lead = 0;
    if ((uint32_t)G32(VA_g_heap_free_list + 0x2b8) != 0)
        lead = (uint32_t)(uint8_t)G8((VA_g_heap_free_list + 0x2bd) + p[0]) & 1;
    if (lead != 0)
        return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
    return (uint32_t)p[0];
}

/* move_cursor_entry_clamped (0x1bb12): EAX=delta. idx=[0x80b38]; new=delta+base[idx]
 * (0x80b10[]). If new>=count(0x80af4) (unsigned) or new==cur(0x80afc[idx]) do nothing;
 * else cur=new and, unless [0x7f571]&2, add 2 to [0x7f571]. */
void move_cursor_entry_clamped(uint32_t delta)
{
    uint32_t off = (uint32_t)(G32(VA_g_cursor_active_list) << 2);
    uint32_t newv = delta + (uint32_t)G32(VA_g_cursor_scroll_offsets + off);
    if (newv >= (uint32_t)G32(VA_g_cursor_entry_count))
        return;
    if (newv == (uint32_t)G32(VA_g_cursor_list_positions + off))
        return;
    G32(VA_g_cursor_list_positions + off) = (int32_t)newv;
    if (G8(VA_g_inventory_dirty_flags) & 2)
        return;
    G8(VA_g_inventory_dirty_flags) += 2;
}

/* ============ Batch 42 — SOS voice-block accessors (far-ptr / lgs cluster) ===========
 * Each resolves a voice control block via the 48-bit far pointer at
 * g_sos_voice_table[p1*0xc0 + p2*6] (lgs GS:EDX), then touches gs:[block + field]. Per
 * the established far-data pattern (cf. mark_geom_sentinel_entries), the C lift is handed
 * the resolved block base; the fixture installs the LDT selector. The voice "active"
 * bit is bit15 of the status word at +0x30 (== bit7 of byte +0x31). */

/* shared body for the two field-xchg accessors (0x4a28c/0x49fe9): if the voice is
 * active, swap the u16 at `field` with `newval` and return the OLD value (sign-extended
 * to int via movsx); otherwise return 0 without writing. */
static int32_t voice_xchg_field_if_active(uint8_t *vcb, uint16_t newval, unsigned field)
{
    if ((*(int16_t *)(vcb + 0x30) & 0x8000) == 0)   /* movsx ...[+0x30]; test ah,0x80; je */
        return 0;
    int16_t old = *(int16_t *)(vcb + field);        /* movsx old */
    *(uint16_t *)(vcb + field) = newval;            /* mov gs:[+field], ax(BX) */
    return (int32_t)old;
}

/* sos_voice_xchg_w54_if_active (0x4a28c): EAX=p1, EDX=p2, BX=newval -> field +0x54. */
int32_t sos_voice_xchg_w54_if_active(uint8_t *vcb, uint16_t newval)
{
    return voice_xchg_field_if_active(vcb, newval, 0x54);
}

/* sos_voice_xchg_w32_if_active (0x49fe9): EAX=p1, EDX=p2, BX=newval -> field +0x32. */
int32_t sos_voice_xchg_w32_if_active(uint8_t *vcb, uint16_t newval)
{
    return voice_xchg_field_if_active(vcb, newval, 0x32);
}

/* sos_voice_deactivate_slot (0x4ac55): EAX=p1, EDX=p2(voice idx). If p2 >= 0x20 return 10.
 * Else, if the voice is active (status bit15) AND not locked (byte[+0x31] & 0x10 == 0),
 * clear the active bit (byte[+0x31] &= 0x7f) and zero the u16 at +0x34. Always ret 0
 * for a valid index. */
uint32_t sos_voice_deactivate_slot(uint8_t *vcb, uint32_t p2)
{
    if (p2 >= 0x20)
        return 0xa;
    if ((*(int16_t *)(vcb + 0x30) & 0x8000) == 0)
        return 0;
    if ((*(uint8_t *)(vcb + 0x31) & 0x10) != 0)
        return 0;
    *(uint8_t *)(vcb + 0x31) &= 0x7f;
    *(uint16_t *)(vcb + 0x34) = 0;
    return 0;
}

/* sos_voice_get_w34 (0x4a54a): EAX=set, EDX=idx -> the sign-extended u16 at voice block
 * [+0x34] (the field sos_voice_deactivate_slot zeroes). */
int32_t sos_voice_get_w34(uint8_t *vcb)
{
    return (int32_t)*(int16_t *)(vcb + 0x34);          /* movsx eax, word gs:[edx+0x34] */
}

/* sos_voice_clamp_w38 (0x53b01, cdecl stack args p1,p2): unconditionally clear the flat
 * flag byte [0x73c5c + p2*0x14]; resolve the voice block (the original via set =
 * [0x94b50 + p1*4]) and clamp its u16 [+0x38] down to 1 if it exceeds 1. Returns 0. */
uint32_t sos_voice_clamp_w38(uint8_t *vcb, uint32_t p2)
{
    G8((VA_g_midi_channel_raw_volume + 0x530) + p2 * 0x14) = 0;
    if (*(uint16_t *)(vcb + 0x38) > 1)
        *(uint16_t *)(vcb + 0x38) = 1;
    return 0;
}

/* ====================== Batch 44 — object-table search + reloc resolve ============== */

/* find_unflagged_object_by_key (0x303ab): EAX=record. key = u16[rec+0xe]; if key==0 or
 * g_object_table_header(0x85c30)==0 or its count u16[+0xa]==0, return 0. Else scan the
 * `count` entry-offsets starting at u16[hdr+8] (stride 2): the entry lives at hdr+off;
 * if its flag byte[+2]&8 is clear AND its key u16[+8]==key, return -1; else 0. */
uint32_t find_unflagged_object_by_key(uint8_t *rec)
{
    uint16_t key = *(uint16_t *)(rec + 0xe);
    if (key == 0)
        return 0;
    uint8_t *hdr = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_object_table_header);
    if (hdr == 0)
        return 0;
    uint32_t count = *(uint16_t *)(hdr + 0xa);
    if (count == 0)
        return 0;
    uint32_t cursor = *(uint16_t *)(hdr + 8);
    do {
        uint32_t off = *(uint16_t *)(hdr + cursor);
        if ((*(uint8_t *)(hdr + off + 2) & 8) == 0 &&
            *(uint16_t *)(hdr + off + 8) == key)
            return 0xffffffffu;
        cursor += 2;
    } while (--count != 0);
    return 0;
}

/* resolve_reloc_record_fields (0x1c06b): EAX=out1, EDX=out2, EBX=offset, CL=flags.
 * base = g_reloc_base(0x7f56c); if base==0 return 0. rec = *(u32*)(base+offset) + base;
 * if rec==0 return 0. If flags&1: *out1 = -(int16[rec+4] >> 1). If flags&2:
 * *out2 = -(int16[rec+6] >> 1). Returns rec. */
uint32_t resolve_reloc_record_fields(int32_t *out1, int32_t *out2,
                                            uint32_t offset, uint8_t flags)
{
    uint32_t base = (uint32_t)G32(VA_g_reloc_base);
    if (base == 0)
        return 0;
    uint32_t rec = *(uint32_t *)(uintptr_t)(base + offset) + base;
    if (rec == 0)
        return 0;
    if (flags & 1)
        *out1 = -((int32_t)*(int16_t *)(uintptr_t)(rec + 4) >> 1);
    if (flags & 2)
        *out2 = -((int32_t)*(int16_t *)(uintptr_t)(rec + 6) >> 1);
    return rec;
}

/* ====================== Batch 45 — more flat leaves ========================= */

/* is_entry_93144_zero (0x476fd): EAX=index -> 1 if the dword [0x93144 + index*4] is 0,
 * else 0. */
uint32_t is_entry_93144_zero(uint32_t index)
{
    return G32((VA_g_sos_driver_vtable + 0x1a8) + index * 4) == 0 ? 1 : 0;
}

/* rng_next_index_for_count (0x1c9a0): EAX=count -> a count-scaled pseudo-random value
 * (count*rng)>>16. A per-call "weight" (2/4/8/0x10 by count magnitude) decrements the
 * countdown [0x81e3a]; when it goes <= 0 the 16-bit LCG [0x71364] (state*0x5e5 + 0x29)
 * reseeds the rng [0x81e38]. The rng is then rotated left by (weight & 0xf) for next
 * time. The return uses the PRE-rotate rng. */
int32_t rng_next_index_for_count(int32_t count)
{
    int32_t weight = count > 0x100 ? 0x10 : count > 0x10 ? 8 : count > 4 ? 4 : 2;
    G32(VA_g_object_select_easy_flag + 0x6) -= weight;
    if (G32(VA_g_object_select_easy_flag + 0x6) <= 0) {
        G32(VA_g_object_select_easy_flag + 0x6) = 0x10 - weight;
        uint16_t s = (uint16_t)((uint16_t)G16(VA_g_inventory_tab_context_map + 0x128) * 0x5e5);   /* imul ax,...,0x5e5 */
        s = (uint16_t)(s + 0x29);
        G16(VA_g_inventory_tab_context_map + 0x128) = s;
        G16(VA_g_object_select_easy_flag + 0x4) = s;
    }
    uint16_t rng = (uint16_t)G16(VA_g_object_select_easy_flag + 0x4);
    int32_t result = (int32_t)((uint32_t)count * (uint32_t)rng) >> 16;   /* imul edx,eax; sar 0x10 */
    unsigned w = (unsigned)weight & 0xf;                                  /* rol ax, cl (& 0xf) */
    G16(VA_g_object_select_easy_flag + 0x4) = w ? (uint16_t)((rng << w) | (rng >> (16 - w))) : rng;
    return result;
}

/* fetch_dbcs_char_advance (0x575e9): EAX=char ptr, EDX=out u32. The pointer-advancing
 * sibling of fetch_dbcs_char (0x57422). If DBCS disabled ([0x758c8]==0): *out=*p, ret
 * p+1. Else: *out=*p; if *p==0 ret p; if not a lead byte (table[*p]&1==0) ret p+1; else
 * if p[1]==0 set *p=0 and ret p; else *out=(*p<<8)|p[1], ret p+2. */
uint8_t *fetch_dbcs_char_advance(uint8_t *p, uint32_t *out)
{
    if ((uint32_t)G32(VA_g_heap_free_list + 0x2b8) == 0) {
        *out = p[0];
        return p + 1;
    }
    uint32_t c = p[0];
    *out = c;
    if (c == 0)
        return p;
    if (((uint32_t)(uint8_t)G8((VA_g_heap_free_list + 0x2bd) + c) & 1) == 0)      /* not a lead byte */
        return p + 1;
    if (p[1] == 0) {                                        /* lead byte, no trail */
        p[0] = 0;
        return p;
    }
    *out = (c << 8) | (uint32_t)p[1];                       /* wide char */
    return p + 2;
}

/* ====================== Batch 46 — stub + SMC setter + object searches ============== */

/* noop_stub_10d87 (0x10d87): push esi/edi/ecx/ebx then pop ebx/ecx/edi/esi; ret — a
 * register-preserving no-op (vestigial stub). No observable effect. */
void noop_stub_10d87(void) { }

/* set_floorceil_span_value (0x3a848): EAX=value. Patches the operand at the
 * setup_floorceil_span SMC site (0x3ac9b, inside the renderer's floor/ceil span code). */
void set_floorceil_span_value(uint32_t v)
{
    G32(VA_g_floorceil_row_pitch_default) = (int32_t)v;          /* mov [0x43ac9b], eax */
}

/* shared body for the object-table searches at 0x34510/0x3451b/0x34526 (each entry sets
 * EBX = a (cursor,count) field-base then jmps here). AX = key. count = u16[hdr+base+2];
 * cursor = u16[hdr+base]; scan `count` u16 entry-offsets from hdr+cursor: entry at
 * hdr+off, if flag byte[+2]&8 clear AND key u16[+8]==key return hdr+off; else 0.
 * NB: no null-header guard (callers guarantee g_object_table_header 0x85c30). */
static uint32_t object_table_search(uint16_t key, uint32_t fieldbase)
{
    uint8_t *hdr = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_object_table_header);
    uint32_t count = *(uint16_t *)(hdr + fieldbase + 2);
    if (count == 0)
        return 0;
    uint8_t *cur = hdr + *(uint16_t *)(hdr + fieldbase);
    do {
        uint32_t off = *(uint16_t *)cur;
        if ((*(uint8_t *)(hdr + off + 2) & 8) == 0 && *(uint16_t *)(hdr + off + 8) == key)
            return (uint32_t)(uintptr_t)(hdr + off);
        cur += 2;
    } while (--count != 0);
    return 0;
}

uint32_t find_object_list20(uint16_t key) { return object_table_search(key, 0x20); } /* 0x34510 */
uint32_t find_object_list24(uint16_t key) { return object_table_search(key, 0x24); } /* 0x3451b */
uint32_t find_object_list40(uint16_t key) { return object_table_search(key, 0x40); } /* 0x34526 */

/* ====================== Batch 47 — bare-ret stubs + raw-state search ================ */

/* Four bare `ret` (1-byte) vestigial no-op stubs: 0x557e7, 0x15804, 0x3cc01, 0x55e7b.
 * (Distinct entries, identical behaviour — no observable effect.) */
void noop_ret_stub(void) { }

/* collect_raw_state_matches (0x4f36d): EAX=key, EDX=out (u16[]), EBX=max. Always sets
 * out[1]=key; if max<3 returns 0. Else scans the raw-state primary buffer's records
 * (start = u16[buf+8], count = u16[buf+start-2]; valid records (byte[+1]&0x80) are 14
 * bytes, invalid 10) and writes the buf-relative offset of each record whose key
 * (u16[+0xc]) matches into out[2..], up to (max-2) of them. out[0] = match count;
 * returns the count. */
int32_t collect_raw_state_matches(uint16_t key, uint16_t *out, uint32_t max)
{
    out[1] = key;
    if (max < 3)
        return 0;
    uint32_t slots = max - 2;
    uint16_t *dst = out + 2;
    uint8_t  *buf = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    uint8_t  *rec = buf + *(uint16_t *)(buf + 8);
    int32_t   count = (int32_t)*(uint16_t *)(rec - 2);
    uint32_t  rem = slots;
    while (count > 0) {
        if (*(uint8_t *)(rec + 1) & 0x80) {                 /* valid record */
            if (*(uint16_t *)(rec + 0xc) == key) {
                *dst++ = (uint16_t)(uint32_t)(rec - buf);
                if (--rem == 0)
                    break;
            }
            rec += 4;                                        /* valid -> +14 total */
        }
        rec += 0xa;
        count--;
    }
    int32_t matches = (int32_t)slots - (int32_t)rem;
    out[0] = (uint16_t)matches;
    return matches;
}

/* find_raw_state_record (0x4f52b): AX=key. Single-match sibling of collect_raw_state_
 * matches: scans the raw-state primary buffer records (same start/count/variable-stride
 * layout) and returns the buf-relative offset of the first valid record (byte[+1]&0x80)
 * whose key u16[+0xc]==key, or 0 if none. */
int32_t find_raw_state_record(uint16_t key)
{
    uint8_t *buf = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    uint8_t *rec = buf + *(uint16_t *)(buf + 8);
    int32_t count = (int32_t)*(uint16_t *)(rec - 2);
    while (count > 0) {
        if (*(uint8_t *)(rec + 1) & 0x80) {
            if (*(uint16_t *)(rec + 0xc) == key)
                return (int32_t)(uint32_t)(rec - buf);
            rec += 4;
        }
        rec += 0xa;
        count--;
    }
    return 0;
}

/* ring_push (0x50e35, stdcall ret 4): EAX=idx, EDX/EBX/ECX=first 3 entry dwords,
 * stack[0]=4th. A bounded ring (32 slots of 0x10 bytes at 0x931c0 + idx*0x200). If the
 * count [0x93bfc+idx*4] has reached capacity [0x93be8+idx*4], returns -1; else writes
 * the 4 dwords at slot (head [0x93bc0+idx*4]), bumps count, advances head (mod 0x20),
 * and returns the OLD head. */
int32_t ring_push(uint32_t idx, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3)
{
    if ((uint32_t)G32((VA_g_extmidi_out_callback + 0xa44) + idx * 4) >= (uint32_t)G32((VA_g_extmidi_out_callback + 0xa30) + idx * 4))
        return -1;
    int32_t head = G32((VA_g_extmidi_out_callback + 0xa08) + idx * 4);
    uint32_t slot = 0x931c0 + idx * 0x200 + (uint32_t)head * 0x10;
    G32(slot + 0)   = (int32_t)d0;
    G32(slot + 4)   = (int32_t)d1;
    G32(slot + 8)   = (int32_t)d2;
    G32(slot + 0xc) = (int32_t)d3;
    G32((VA_g_extmidi_out_callback + 0xa44) + idx * 4) += 1;
    G32((VA_g_extmidi_out_callback + 0xa08) + idx * 4) = (head + 1) & 0x1f;
    return head;
}

/* ====================== Batch 49 — flat math/setter/table leaves ==================== */

/* clamp_diff_200 (0x26f02): EAX=value, EDX=center. Clamp `value` to within ±0x200 of
 * `center`: if value-center > 0x200 -> center+0x200; if < -0x200 -> center-0x200; else
 * value. (Signed.) */
int32_t clamp_diff_200(int32_t value, int32_t center)
{
    int32_t diff = (int32_t)((uint32_t)value - (uint32_t)center);
    if (diff > 0x200)
        return center + 0x200;
    if (diff < -0x200)
        return center - 0x200;
    return value;
}

/* compute_mode_bytes (0x302b3): EAX=out1, EDX=out2 (byte ptrs). out1 = (g_90bd4&1)?1:2;
 * out2 starts 2, doubled to 4 if g_hires_line_doubling_flag(0x90cbd)!=0, then halved if
 * g_90bd4&2. Writes the two step/size bytes. */
void compute_mode_bytes(uint8_t *out1, uint8_t *out2)
{
    uint8_t cl = 2, ch = 2;
    if (G8(VA_g_hires_line_doubling_flag) != 0)
        ch = (uint8_t)(ch + ch);          /* -> 4 */
    uint8_t bl = (uint8_t)G8(VA_g_view_scale_flags);
    if (bl & 1)
        cl--;                              /* -> 1 */
    if (bl & 2)
        ch >>= 1;
    *out1 = cl;
    *out2 = ch;
}

/* set_7049a_from_71988 (0x26a6c): byte[0x7049a] = 0x10 - min((u32[0x71988] & 0x1ff) >> 4,
 * 0x10). No args. */
void set_7049a_from_71988(void)
{
    int32_t v = (G32(VA_g_choice_selected_index + 0x618) & 0x1ff) >> 4;
    if (v > 0x10)
        v = 0x10;
    G8(VA_g_cfg_das2_arg + 0x1be) = (uint8_t)(0x10 - v);
}

/* build_atan_table (0x3c1c9): g_atan_table[i] = (0x4000 * g_sincos_table[i]) /
 * g_sincos_table[i+0x80], for i in 0..0x3f (16-bit mul then 16-bit div). */
void build_atan_table(void)
{
    for (int i = 0; i < 0x40; i++) {
        uint32_t num = 0x4000u * (uint32_t)(uint16_t)G16(VA_g_sincos_table + i * 2);
        uint16_t den = (uint16_t)G16(VA_g_sincos_table + 0x100 + i * 2);
        G16(VA_g_atan_table + i * 2) = (uint16_t)(num / den);
    }
}

/* basename_strip_ext (0x10711): EAX=path, EDX=out. Copies the path's basename (the part
 * after the last '\\') up to (but excluding) the first '.' into `out`, NUL-terminated. */
void basename_strip_ext(const char *path, char *out)
{
    const char *base = path, *p = path;
    char c;
    while ((c = *p) != '\0') {
        p++;
        if (c == '\\')
            base = p;
    }
    while ((c = *base) != '\0' && c != '.') {
        *out++ = c;
        base++;
    }
    *out = '\0';
}

/* ====================== Batch 50 — RLE / list / search / expand leaves ============== */

/* decode_byterun1 (0x13154): EAX=src, EDX=dst, EBX=output length. ByteRun1 decode:
 * bytes <= 0xf0 copy literally; a marker 0xf1..0xff means "repeat the next byte
 * (marker-0xf0 = 1..15) times". Stops when the remaining length runs out. */
void decode_byterun1(uint8_t *src, uint8_t *dst, int32_t len)
{
    for (;;) {
        uint8_t b;
        for (;;) {
            b = *src++;
            if (b >= 0xf1)
                break;                          /* run marker */
            *dst++ = b;
            if (--len <= 0)                     /* dec ebx; jle */
                return;
        }
        uint32_t run = (uint8_t)(b - 0xf0);     /* movzx; sub cl,0xf0 */
        len -= (int32_t)run;
        uint8_t rb = *src++;
        while (run--)
            *dst++ = rb;
        if (len <= 0)                           /* or ebx,ebx; jg loop else ret */
            return;
    }
}

/* init_freelist_820c1 (0x1e792): builds a 32-node free list — head [0x820c1] -> first
 * node 0x820c5, each node 0x20 bytes with *node=next (last=0) and node[+4]=0. */
void init_freelist_820c1(void)
{
    uint32_t node = 0x820c5u + OBJ_DELTA;
    uint32_t cur  = 0x820c1u + OBJ_DELTA;
    *(uint32_t *)(uintptr_t)cur = node;                 /* head -> first */
    for (int i = 0; i < 0x20; i++) {
        *(uint32_t *)(uintptr_t)cur = node;             /* *cur = node */
        cur = node;
        *(uint32_t *)(uintptr_t)(node + 4) = 0;         /* node[+4] = 0 */
        node += 0x20;
    }
    *(uint32_t *)(uintptr_t)cur = 0;                    /* last->next = 0 */
}

/* find_sound_sample_index (0x27374): AX=key. If g_7f550 set, scan the [0x848f4] array (count
 * [0x848fc], stride 0xc) for an entry with key u16[+8]==key; return its 1-based index,
 * else 0. */
int32_t find_sound_sample_index(uint16_t key)
{
    if (G32(VA_g_sound_enabled) == 0)
        return 0;
    uint8_t *e = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_sound_sample_table);
    int32_t n = G32(VA_g_sound_sample_count);
    for (int32_t i = 0; i < n; i++, e += 0xc)
        if (*(uint16_t *)(e + 8) == key)
            return i + 1;
    return 0;
}

/* expand_rgb (0x203fd): EAX=src (bytes), EDX=dst (u16), EBX=count. For each triple:
 * dst[0]=src[0]<<5, dst[1]=src[1]*0x30, dst[2]=src[2]<<4 (16-bit). */
void expand_rgb(uint8_t *src, uint16_t *dst, int32_t count)
{
    for (int32_t i = 0; i < count; i++) {
        dst[0] = (uint16_t)((uint32_t)src[0] << 5);
        dst[1] = (uint16_t)((uint32_t)src[1] * 0x30);
        dst[2] = (uint16_t)((uint32_t)src[2] << 4);
        src += 3;
        dst += 3;
    }
}

/* adjust_records_z_carry (0x34a5f): EAX=center, ESI=records, CL=count. Builds the sorted
 * range [center-delta, center] (delta = g_player_move_delta_z 0x8a12c); for each 0x10-
 * byte record whose word[+0xa] lies in range, adds delta to it. */
void adjust_records_z_carry(uint32_t center, uint8_t *recs, uint8_t count)
{
    int16_t delta = (int16_t)G16(VA_g_player_move_delta_z);
    int16_t hi = (int16_t)(uint16_t)center;
    int16_t lo = (int16_t)(uint16_t)((uint16_t)center - (uint16_t)delta);
    if (lo > hi) { int16_t t = lo; lo = hi; hi = t; }   /* cmp/jg/xchg */
    do {
        int16_t v = *(int16_t *)(recs + 0xa);
        if (lo <= v && v <= hi)
            *(int16_t *)(recs + 0xa) = (int16_t)(v + delta);
        recs += 0x10;
        count--;
    } while (count != 0);
}

/* ====================== Batch 51 — ring init + descriptor blits ===================== */

/* ring_init (0x50d51): EAX=idx, EDX=capacity. The init counterpart to ring_push — fills
 * all 32 16-byte slots (0x931c0 + idx*0x200) with 0xffffffff, zeroes head [0x93bc0],
 * [0x93bd4], count [0x93bfc], and sets capacity [0x93be8] = EDX (all +idx*4). */
void ring_init(uint32_t idx, uint32_t capacity)
{
    for (uint32_t i = 0; i < 0x20; i++) {
        uint32_t s = idx * 0x200 + i * 0x10;
        G32((VA_g_extmidi_out_callback + 0x8) + s)       = -1;
        G32((VA_g_extmidi_out_callback + 0x8) + s + 4)   = -1;
        G32((VA_g_extmidi_out_callback + 0x8) + s + 0xc) = -1;
        G32((VA_g_extmidi_out_callback + 0x8) + s + 8)   = -1;
    }
    G32((VA_g_extmidi_out_callback + 0xa08) + idx * 4) = 0;
    G32((VA_g_extmidi_out_callback + 0xa1c) + idx * 4) = 0;
    G32((VA_g_extmidi_out_callback + 0xa44) + idx * 4) = 0;
    G32((VA_g_extmidi_out_callback + 0xa30) + idx * 4) = (int32_t)capacity;
}

/* blit_save_region (0x130d4): EAX = ptr to a descriptor ptr. desc=*EAX; copies a strided
 * 2D block from desc[0] (src, row pitch = g_screen_pitch 0x85498) into desc+0xc (dst,
 * contiguous): per row, desc[1]/4 dwords; height = desc[2]. Returns the EAX arg. */
uint32_t *blit_save_region(uint32_t *pp)
{
    uint32_t *desc = (uint32_t *)(uintptr_t)pp[0];
    uint8_t  *src  = (uint8_t *)(uintptr_t)desc[0];
    uint32_t  wb   = desc[1];
    int32_t   h    = (int32_t)desc[2];
    uint8_t  *dst  = (uint8_t *)desc + 0xc;
    int32_t   adj  = (int32_t)wb - (int32_t)G32(VA_g_screen_pitch);   /* esi -= (width - pitch) bytes */
    uint32_t  dwords = wb >> 2;
    for (int32_t r = h; r > 0; r--) {                       /* dec edx; jg */
        for (uint32_t k = 0; k < dwords; k++) {             /* rep movsd */
            *(uint32_t *)dst = *(uint32_t *)src;
            dst += 4; src += 4;
        }
        src -= adj;
    }
    return pp;
}

/* shade_remap_blit (0x13ecb): EAX = descriptor. Remaps a 2D pixel block in place through a
 * 256-entry shade row: row = byte[desc+0x10]; if 0, table = g_das_remap_chunk_4000_ptr
 * (0x86d14) and row = byte[desc+0x11], else table = g_world_shading_table_ptr (0x86d28).
 * For each pixel p in the block: p = table[(row<<8) + p]. width=desc[1], height=desc[2],
 * stride=desc[3], buffer=desc[0]. */
void shade_remap_blit(uint8_t *desc)
{
    uint32_t tbl;
    uint8_t s10 = desc[0x10];
    if (s10 == 0)
        tbl = ((uint32_t)desc[0x11] << 8) + (uint32_t)G32(VA_g_das_remap_chunk_4000_ptr);
    else
        tbl = ((uint32_t)s10 << 8) + (uint32_t)G32(VA_g_world_shading_table_ptr);
    uint32_t width  = *(uint32_t *)(desc + 4);
    int32_t  adj    = (int32_t)*(uint32_t *)(desc + 0xc) - (int32_t)width;   /* desc[3]-desc[1] */
    uint8_t *px     = (uint8_t *)(uintptr_t)*(uint32_t *)desc;
    int32_t  h      = (int32_t)*(uint32_t *)(desc + 8);
    do {                                                    /* outer: dec eax; jg */
        int32_t c = (int32_t)width;
        do {                                                /* inner: dec ecx; jg */
            /* faithful to `mov bl,[esi]; mov dl,[ebx]`: the pixel REPLACES the low byte
             * of the row base (not an add) — equal to (base+row*0x100)+pix only when the
             * table is 256-aligned (as the real game's tables are). */
            *px = *(uint8_t *)(uintptr_t)((tbl & 0xffffff00u) | (uint32_t)*px);
            px++;
        } while (--c > 0);
        px += adj;
    } while (--h > 0);
}

/* ====================== Batch 52 — bitmap / ratio / mouse / table-search leaves ====== */

/* dbase100_bitmap_test_set (0x1cab7): EAX=bit index. If g_dbase100_record_bitmap(0x81e28)
 * is set, test bit `index` (byte[(int)index>>3], mask g_bit_mask_lut[index&7] @0x71366):
 * if already set return 0; else set it and return -1. (No bitmap -> -1.) */
uint32_t dbase100_bitmap_test_set(uint32_t idx)
{
    uint32_t bm = (uint32_t)G32(VA_g_dbase100_record_bitmap);
    if (bm == 0)
        return 0xffffffffu;
    uint8_t *p = (uint8_t *)(uintptr_t)(((int32_t)idx >> 3) + bm);
    uint8_t mask = (uint8_t)G8(VA_g_bit_mask_lut + (idx & 7));
    if (*p & mask)
        return 0;
    *p |= mask;
    return 0xffffffffu;
}

/* dbase100_bitmap_test_clear (0x1caf4): sibling of test_set — if the bit is already clear
 * return 0; else clear it and return -1. */
uint32_t dbase100_bitmap_test_clear(uint32_t idx)
{
    uint32_t bm = (uint32_t)G32(VA_g_dbase100_record_bitmap);
    if (bm == 0)
        return 0xffffffffu;
    uint8_t *p = (uint8_t *)(uintptr_t)(((int32_t)idx >> 3) + bm);
    uint8_t mask = (uint8_t)G8(VA_g_bit_mask_lut + (idx & 7));
    if ((*p & mask) == 0)
        return 0;
    *p &= (uint8_t)~mask;
    return 0xffffffffu;
}

/* compute_ratio_4c296 (0x4c296): EAX=out (u32[2]). div = [0x91874] (×2 if byte[0x91dcf]&0x80);
 * if div==0 return 0. Else out[1] = [0x91870] / div; out[0] = ([0x91ce8] * ([0x91870] %
 * div)) / div (64-bit). Returns -1. A fixed-point integer.fraction split. */
uint32_t compute_ratio_4c296(uint32_t *out)
{
    uint32_t div = (uint32_t)G32(VA_g_particle_pool + 0x10);
    if (G8(VA_g_gdv_audio_format + 0x5) & 0x80)
        div += div;
    if (div == 0)
        return 0;
    uint32_t num = (uint32_t)G32(VA_g_particle_pool + 0xc);
    out[1] = num / div;
    uint32_t rem = num % div;
    uint32_t scale = (uint32_t)G32(VA_g_dpcm_step_table + 0x444);
    out[0] = (uint32_t)(((uint64_t)scale * (uint64_t)rem) / (uint64_t)div);
    return 0xffffffffu;
}

/* mouse_edge_latch (0x121a1): EAX passthrough. On a rising edge of the button byte
 * [0x7e93a] (cur!=0, prev [0x7e93b]==0) and only if not already latched ([0x7e930]==0),
 * latch: set [0x7e930]=1 and snapshot g_mouse_x/y (0x707b3/0x707b7) -> [0x7e904]/[0x7e908].
 * Always copies cur -> prev. Returns EAX unchanged. */
uint32_t mouse_edge_latch(uint32_t eax)
{
    uint8_t cur = (uint8_t)G8(VA_g_mouse_click_edges);
    uint8_t prev = (uint8_t)G8(VA_g_mouse_click_edges + 0x1);
    G8(VA_g_mouse_click_edges + 0x1) = cur;
    if (cur != 0 && prev == 0 && G8(VA_g_mouse_buttons_prev + 0x7) == 0) {
        G8(VA_g_mouse_buttons_prev + 0x7) = 1;
        G32(VA_g_saved_int9_offset + 0x1c) = G32(VA_g_mouse_x);
        G32(VA_g_saved_int9_offset + 0x20) = G32(VA_g_mouse_y);
    }
    return eax;
}

/* mark_sound_handle_by_id (0x26de4): EAX=id. Scans the 16 entries (stride 0x9a) at 0x83ed4;
 * for the first whose record ptr (*entry) is non-null and record's u16[+6]==id, set
 * entry's dword[+8] = 0x7f00 and return 1; else 0. */
uint32_t mark_sound_handle_by_id(uint32_t id)
{
    uint8_t *e = (uint8_t *)(uintptr_t)(0x83ed4u + OBJ_DELTA);
    for (int i = 0; i < 0x10; i++, e += 0x9a) {
        int32_t rec = *(int32_t *)e;
        if (rec != 0 && (uint32_t)*(uint16_t *)(uintptr_t)(rec + 6) == id) {
            *(int32_t *)(e + 8) = 0x7f00;
            return 1;
        }
    }
    return 0;
}

/* find_object_index_by_ptr (0x356fa): EAX=entry ptr. Walks g_object_table_header's
 * stride-linked entries (first stride = u16[hdr+4], count = u16[hdr+6]; each step advances
 * by the current stride and reads the next stride from the new position); returns the
 * 1-based index where the address equals EAX, else 0. */
int32_t find_object_index_by_ptr(uint32_t ptr)
{
    uint8_t *hdr = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_object_table_header);
    if (hdr == 0 || *(uint16_t *)(hdr + 6) == 0)
        return 0;
    uint16_t stride = *(uint16_t *)(hdr + 4);
    int32_t  count  = *(uint16_t *)(hdr + 6);
    uint8_t *p = hdr;
    int32_t  idx = 0;
    do {
        idx++;
        p += stride;
        if (ptr == (uint32_t)(uintptr_t)p)
            return idx;
        stride = *(uint16_t *)p;
        count--;
    } while (count != 0);
    return 0;
}

/* ===================== floor/ceiling span DRIVER (0x3a84e, draw_floorceil_surface) =====================
 * The 3rd top-level span driver (sibling of the native sprite driver 0x39610), reached by a tail-`jmp`
 * from rasterize_world_spans_scanline 0x366cb when [0x9093c]&0x200. It does per-surface setup (optional
 * per-poly shade via the bridged 0x3c0be), bridges the polygon edge-walker (0x3b1c1) that builds the
 * per-scanline span run-list (table 0x8cd0c, stride 0x18), runs the render-mode matrix that picks a FILL
 * inner-loop pair (main g_floorceil_span_fn / alt), then a per-row loop that perspective-divides the U/V/W
 * steppers and dispatches one of ~15 fill inner-loops (0x3acec..0x3b1c0). Mirrors the sprite driver's
 * structure. The edge-walker + 0x3c0be are BRIDGED (call_orig) — identical original code, byte-safe.
 *
 * PACKED STEPPERS (review #2): the row loop writes WORDS at 0x8a3dd/0x8a3e0/0x8a3e3 and DWORDS at
 * 0x8a3f4/0x8a3f7/0x8a3fa which OVERLAP; the fills read back dword[0x8a3f4]/[0x8a3f8]/[0x8a3dc]/[0x8a3e0].
 * We perform the EXACT same G16/G32 stores in the same order and the fills READ THE PACKED DWORDS BACK
 * from the live obj3 globals — so byte-identity AND correctness follow from the original's own memory.
 * SMC mask (review #3): the engine self-patches the fill loops' ror/and immediates; we compute them as
 * locals: b=bsr16([0x90978]); ror_imm=0x10-b; ebx_mask=(u16)([0x90974]<<b); dh_mask=(u8)[0x9097c]. */

typedef struct {
    uint32_t gs_base;     /* [0x8a2a8] colormap selector base                          */
    uint32_t fs_base;     /* [0x909b0] texture-source selector base                    */
    uint32_t blend_base;  /* [0x90be2] 64K translucency LUT selector base              */
    uint32_t cmap_flat;   /* [0x8a2ac] flat colormap base (the 0x3af38 flat-remap path)*/
    uint32_t es_fb_base;  /* [0x90c06] framebuffer selector base (es:[edi] fills)      */
    uint32_t flat_fb;     /* [0x85414] framebuffer flat base (DS [edi] fills)          */
    uint8_t  ror_imm;     /* SMC: 0x10 - bsr16(width)                                  */
    uint8_t  dh_mask;     /* SMC: (u8)[0x9097c]                                        */
    uint32_t ebx_mask;    /* SMC: (u16)([0x90974] << bsr16(width))                     */
} fc_ctx_t;

static inline uint32_t fc_rotr32(uint32_t v, unsigned n) {
    n &= 31u;
    return n ? ((v >> n) | (v << (32u - n))) : v;
}

/* the uniform perspective-index helper: mask edx's dh in place (texture V wrap) then build the texel
 * offset = (rotr(ebp,ror_imm) & ebx_mask) | dh  — exactly `ror ebx,imm; and ebx,mask; and dh,mask; or bl,dh`. */
static inline uint32_t fc_index(uint32_t ebp, uint32_t *edx, const fc_ctx_t *cx) {
    uint8_t dh = (uint8_t)(((*edx) >> 8) & cx->dh_mask);
    *edx = ((*edx) & 0xffff00ffu) | ((uint32_t)dh << 8);
    return (fc_rotr32(ebp, cx->ror_imm) & cx->ebx_mask) | dh;
}
#define FC_RDB(a)  (*(volatile uint8_t  *)(uintptr_t)(a))   /* read host byte at flat addr */

/* ---- untextured fills (es:[edi]) ---- */
static void fc_solid_3acec(uint32_t edi_cur, int32_t count, const fc_ctx_t *cx) {
    uint8_t *dst = (uint8_t *)(uintptr_t)(cx->es_fb_base + edi_cur);
    uint8_t v = (uint8_t)G8(VA_g_sprite_fill_index);
    for (int32_t i = 0; i < count; i++) dst[i] = v;
}
static void fc_colormap_3ad04(uint32_t edi_cur, int32_t count, uint8_t ah, const fc_ctx_t *cx) {
    uint8_t *dst = (uint8_t *)(uintptr_t)(cx->es_fb_base + edi_cur);
    uint32_t idx = ((uint32_t)ah << 8) | (uint8_t)G8(VA_g_sprite_fill_index);  /* ebx=zext[0x90a24]; bh=ah */
    uint8_t v = FC_RDB(cx->gs_base + idx);
    for (int32_t i = 0; i < count; i++) dst[i] = v;
}
static void fc_affine_3ad28(uint32_t edi_cur, int32_t count, const fc_ctx_t *cx) {
    uint8_t *dst = (uint8_t *)(uintptr_t)(cx->es_fb_base + edi_cur);
    uint16_t si = (uint16_t)G16(VA_g_floorceil_accum_b + 0x2);
    uint16_t bp = (uint16_t)G16(VA_g_floorceil_step_b + 0x2);
    uint8_t  dl = (uint8_t)G8(VA_g_floorceil_step_b + 0x4);
    uint8_t  bh = (uint8_t)G8(VA_g_floorceil_accum_b + 0x4);
    uint8_t  bl = (uint8_t)G8(VA_g_sprite_fill_index);          /* constant low byte (texel column) */
    for (int32_t i = 0; i < count; i++) {
        uint16_t idx = (uint16_t)(((uint16_t)bh << 8) | bl);   /* gs:[bx] 16-bit */
        dst[i] = FC_RDB(cx->gs_base + idx);
        uint32_t s = (uint32_t)si + bp; si = (uint16_t)s; uint8_t cf = (uint8_t)(s >> 16);
        bh = (uint8_t)(bh + dl + cf);            /* add si,bp; adc bh,dl */
    }
}

/* ---- perspective textured fills (DS [edi], flat_fb) ---- */
static void fc_persp_opaque_3ad80(uint32_t edi_cur, int32_t count, const fc_ctx_t *cx) {
    uint8_t *dst = (uint8_t *)(uintptr_t)(cx->flat_fb + edi_cur);
    uint32_t step_lo = (uint32_t)G32(VA_g_floorceil_step_a), step_hi = (uint32_t)G32(VA_g_floorceil_step_b);
    uint32_t ebp = (uint32_t)G32(VA_g_floorceil_accum_a), edx = (uint32_t)G32(VA_g_floorceil_accum_b);
    for (int32_t i = 0; i < count; i++) {
        uint32_t ebx = fc_index(ebp, &edx, cx);
        dst[i] = FC_RDB(cx->fs_base + ebx);
        uint32_t o = ebp; ebp += step_lo; unsigned c = (ebp < o); edx = edx + step_hi + c;
    }
}
static void fc_persp_transp_3adc0(uint32_t edi_cur, int32_t count, const fc_ctx_t *cx) {
    uint8_t *dst = (uint8_t *)(uintptr_t)(cx->flat_fb + edi_cur);
    uint32_t step_lo = (uint32_t)G32(VA_g_floorceil_step_a), step_hi = (uint32_t)G32(VA_g_floorceil_step_b);
    uint32_t ebp = (uint32_t)G32(VA_g_floorceil_accum_a), edx = (uint32_t)G32(VA_g_floorceil_accum_b);
    for (int32_t i = 0; i < count; i++) {
        uint32_t ebx = fc_index(ebp, &edx, cx);
        uint8_t t = FC_RDB(cx->fs_base + ebx);
        if (t) dst[i] = t;                       /* or bl,bl; je skip */
        uint32_t o = ebp; ebp += step_lo; unsigned c = (ebp < o); edx = edx + step_hi + c;
    }
}
static void fc_3ae04(uint32_t edi_cur, int32_t count, const fc_ctx_t *cx) {   /* transp + translucent */
    uint8_t *dst = (uint8_t *)(uintptr_t)(cx->flat_fb + edi_cur);
    uint32_t step_lo = (uint32_t)G32(VA_g_floorceil_step_a), step_hi = (uint32_t)G32(VA_g_floorceil_step_b);
    uint32_t ebp = (uint32_t)G32(VA_g_floorceil_accum_a), edx = (uint32_t)G32(VA_g_floorceil_accum_b);
    for (int32_t i = 0; i < count; i++) {
        uint32_t ebx = fc_index(ebp, &edx, cx);
        uint8_t t = FC_RDB(cx->fs_base + ebx);
        if (t) {
            if (t & 0x80u) { uint8_t d = dst[i]; dst[i] = FC_RDB(cx->blend_base + (((uint32_t)d << 8) | t)); }
            else dst[i] = t;
        }
        uint32_t o = ebp; ebp += step_lo; unsigned c = (ebp < o); edx = edx + step_hi + c;
    }
}
static void fc_3ae68(uint32_t edi_cur, int32_t count, uint8_t ah, const fc_ctx_t *cx) {  /* +const shade */
    if (ah == 0) { fc_3ae04(edi_cur, count, cx); return; }                        /* or ah,ah; je 0x3ae04 */
    uint8_t shade = ah;
    uint8_t *dst = (uint8_t *)(uintptr_t)(cx->flat_fb + edi_cur);
    uint32_t step_lo = (uint32_t)G32(VA_g_floorceil_step_a), step_hi = (uint32_t)G32(VA_g_floorceil_step_b);
    uint32_t ebp = (uint32_t)G32(VA_g_floorceil_accum_a), edx = (uint32_t)G32(VA_g_floorceil_accum_b);
    for (int32_t i = 0; i < count; i++) {
        uint32_t ebx = fc_index(ebp, &edx, cx);
        uint8_t t = FC_RDB(cx->fs_base + ebx);
        if (t) {
            uint8_t sh = FC_RDB(cx->gs_base + (((uint32_t)shade << 8) | t));      /* gs:[(shade<<8)|texel] */
            if (t & 0x80u) { uint8_t d = dst[i]; dst[i] = FC_RDB(cx->blend_base + (((uint32_t)d << 8) | sh)); }
            else dst[i] = sh;
        }
        uint32_t o = ebp; ebp += step_lo; unsigned c = (ebp < o); edx = edx + step_hi + c;
    }
}
static void fc_3aed8(uint32_t edi_cur, int32_t count, uint8_t ah, const fc_ctx_t *cx) {  /* transp + const shade */
    if (ah == 0) { fc_persp_transp_3adc0(edi_cur, count, cx); return; }           /* or ah,ah; je 0x3adc0 */
    uint8_t shade = ah;
    uint8_t *dst = (uint8_t *)(uintptr_t)(cx->flat_fb + edi_cur);
    uint32_t step_lo = (uint32_t)G32(VA_g_floorceil_step_a), step_hi = (uint32_t)G32(VA_g_floorceil_step_b);
    uint32_t ebp = (uint32_t)G32(VA_g_floorceil_accum_a), edx = (uint32_t)G32(VA_g_floorceil_accum_b);
    for (int32_t i = 0; i < count; i++) {
        uint32_t ebx = fc_index(ebp, &edx, cx);
        uint8_t t = FC_RDB(cx->fs_base + ebx);
        if (t) dst[i] = FC_RDB(cx->gs_base + (((uint32_t)shade << 8) | t));
        uint32_t o = ebp; ebp += step_lo; unsigned c = (ebp < o); edx = edx + step_hi + c;
    }
}
static void fc_3af38(uint32_t edi_cur, int32_t count, uint8_t ah, const fc_ctx_t *cx) {  /* opaque + const shade (es:, flat cmap) */
    if (ah == 0) { fc_persp_opaque_3ad80(edi_cur, count, cx); return; }           /* or ah,ah; je 0x3ad80 (DS) */
    uint8_t shade = ah;
    uint8_t *dst = (uint8_t *)(uintptr_t)(cx->es_fb_base + edi_cur);
    uint32_t cmap_row = cx->cmap_flat + ((uint32_t)shade << 8);                   /* eax=[0x8a2ac]+(shade<<8) */
    uint32_t step_lo = (uint32_t)G32(VA_g_floorceil_step_a), step_hi = (uint32_t)G32(VA_g_floorceil_step_b);
    uint32_t ebp = (uint32_t)G32(VA_g_floorceil_accum_a), edx = (uint32_t)G32(VA_g_floorceil_accum_b);
    for (int32_t i = 0; i < count; i++) {
        uint32_t ebx = fc_index(ebp, &edx, cx);
        uint8_t t = FC_RDB(cx->fs_base + ebx);
        dst[i] = FC_RDB((cmap_row & 0xffffff00u) | t);                            /* al=fs:[ebx]; al=[eax] */
        uint32_t o = ebp; ebp += step_lo; unsigned c = (ebp < o); edx = edx + step_hi + c;
    }
}
static void fc_3af94(uint32_t edi_cur, int32_t count, const fc_ctx_t *cx) {  /* gs-reshade RAMP (es:) */
    uint8_t *dst = (uint8_t *)(uintptr_t)(cx->es_fb_base + edi_cur);
    uint32_t step_lo = (uint32_t)G32(VA_g_floorceil_step_a), step_hi = (uint32_t)G32(VA_g_floorceil_step_b);
    uint8_t  shade_step = (uint8_t)G8(VA_g_floorceil_step_b + 0x4);
    uint32_t ebp = (uint32_t)G32(VA_g_floorceil_accum_a), edx = (uint32_t)G32(VA_g_floorceil_accum_b);
    uint8_t  ah = (uint8_t)G8(VA_g_floorceil_accum_b + 0x4);
    for (int32_t i = 0; i < count; i++) {
        uint32_t ebx = fc_index(ebp, &edx, cx);
        uint8_t t = FC_RDB(cx->fs_base + ebx);
        dst[i] = FC_RDB(cx->gs_base + (((uint32_t)ah << 8) | t));
        uint32_t o = ebp; ebp += step_lo; unsigned c1 = (ebp < o);
        uint64_t s = (uint64_t)edx + step_hi + c1; edx = (uint32_t)s; unsigned c2 = (unsigned)(s >> 32);
        ah = (uint8_t)(ah + shade_step + c2);                                     /* adc ah,imm */
    }
}
static void fc_3aff4(uint32_t edi_cur, int32_t count, const fc_ctx_t *cx) {  /* gs-reshade ramp + transp + translucent (DS) */
    uint8_t *dst = (uint8_t *)(uintptr_t)(cx->flat_fb + edi_cur);
    uint32_t step_lo = (uint32_t)G32(VA_g_floorceil_step_a), step_hi = (uint32_t)G32(VA_g_floorceil_step_b);
    uint8_t  shade_step = (uint8_t)G8(VA_g_floorceil_step_b + 0x4);
    uint32_t ebp = (uint32_t)G32(VA_g_floorceil_accum_a), edx = (uint32_t)G32(VA_g_floorceil_accum_b);
    uint8_t  ah = (uint8_t)G8(VA_g_floorceil_accum_b + 0x4);
    for (int32_t i = 0; i < count; i++) {
        uint32_t ebx = fc_index(ebp, &edx, cx);
        uint8_t t = FC_RDB(cx->fs_base + ebx);
        if (t) {
            if (t & 0x80u) { uint8_t d = dst[i]; dst[i] = FC_RDB(cx->blend_base + (((uint32_t)d << 8) | t)); }  /* RAW texel blend */
            else dst[i] = FC_RDB(cx->gs_base + (((uint32_t)ah << 8) | t));
        }
        uint32_t o = ebp; ebp += step_lo; unsigned c1 = (ebp < o);
        uint64_t s = (uint64_t)edx + step_hi + c1; edx = (uint32_t)s; unsigned c2 = (unsigned)(s >> 32);
        ah = (uint8_t)(ah + shade_step + c2);
    }
}
static void fc_3b070(uint32_t edi_cur, int32_t count, const fc_ctx_t *cx) {  /* gs-reshade ramp + transp, NO translucent (DS) */
    uint8_t *dst = (uint8_t *)(uintptr_t)(cx->flat_fb + edi_cur);
    uint32_t step_lo = (uint32_t)G32(VA_g_floorceil_step_a), step_hi = (uint32_t)G32(VA_g_floorceil_step_b);
    uint8_t  shade_step = (uint8_t)G8(VA_g_floorceil_step_b + 0x4);
    uint32_t ebp = (uint32_t)G32(VA_g_floorceil_accum_a), edx = (uint32_t)G32(VA_g_floorceil_accum_b);
    uint8_t  ah = (uint8_t)G8(VA_g_floorceil_accum_b + 0x4);
    for (int32_t i = 0; i < count; i++) {
        uint32_t ebx = fc_index(ebp, &edx, cx);
        uint8_t t = FC_RDB(cx->fs_base + ebx);
        if (t) dst[i] = FC_RDB(cx->gs_base + (((uint32_t)ah << 8) | t));
        uint32_t o = ebp; ebp += step_lo; unsigned c1 = (ebp < o);
        uint64_t s = (uint64_t)edx + step_hi + c1; edx = (uint32_t)s; unsigned c2 = (unsigned)(s >> 32);
        ah = (uint8_t)(ah + shade_step + c2);
    }
}

/* ---- degenerate-texture fills (width low byte 0): fixed ror 8, 16-bit fs:[bx], no V mask ---- */
static void fc_deg_opaque_3b0d8(uint32_t edi_cur, int32_t count, const fc_ctx_t *cx) {
    uint8_t *dst = (uint8_t *)(uintptr_t)(cx->flat_fb + edi_cur);
    uint32_t step_lo = (uint32_t)G32(VA_g_floorceil_step_a), step_hi = (uint32_t)G32(VA_g_floorceil_step_b);
    uint32_t ebp = (uint32_t)G32(VA_g_floorceil_accum_a), edx = (uint32_t)G32(VA_g_floorceil_accum_b);
    for (int32_t i = 0; i < count; i++) {
        uint32_t ebx = fc_rotr32(ebp, 8);
        uint16_t idx = (uint16_t)((ebx & 0xff00u) | (uint8_t)(edx >> 8));         /* bl=dh (no mask) */
        dst[i] = FC_RDB(cx->fs_base + idx);
        uint32_t o = ebp; ebp += step_lo; unsigned c = (ebp < o); edx = edx + step_hi + c;
    }
}
static void fc_deg_3b110(uint32_t edi_cur, int32_t count, uint8_t ah, const fc_ctx_t *cx) {  /* degenerate + const shade (es:) */
    if (ah == 0) { fc_deg_opaque_3b0d8(edi_cur, count, cx); return; }             /* or ah,ah; je 0x3b0d8 (DS) */
    uint8_t shade = ah;
    uint8_t *dst = (uint8_t *)(uintptr_t)(cx->es_fb_base + edi_cur);
    uint32_t step_lo = (uint32_t)G32(VA_g_floorceil_step_a), step_hi = (uint32_t)G32(VA_g_floorceil_step_b);
    uint32_t ebp = (uint32_t)G32(VA_g_floorceil_accum_a), edx = (uint32_t)G32(VA_g_floorceil_accum_b);
    for (int32_t i = 0; i < count; i++) {
        uint32_t ebx = fc_rotr32(ebp, 8);
        uint16_t idx = (uint16_t)((ebx & 0xff00u) | (uint8_t)(edx >> 8));
        uint8_t t = FC_RDB(cx->fs_base + idx);
        dst[i] = FC_RDB(cx->gs_base + (((uint32_t)shade << 8) | t));
        uint32_t o = ebp; ebp += step_lo; unsigned c = (ebp < o); edx = edx + step_hi + c;
    }
}
static void fc_deg_3b15c(uint32_t edi_cur, int32_t count, const fc_ctx_t *cx) {  /* degenerate + gs-reshade ramp (es:) */
    uint8_t *dst = (uint8_t *)(uintptr_t)(cx->es_fb_base + edi_cur);
    uint32_t step_lo = (uint32_t)G32(VA_g_floorceil_step_a), step_hi = (uint32_t)G32(VA_g_floorceil_step_b);
    uint8_t  shade_step = (uint8_t)G8(VA_g_floorceil_step_b + 0x4);
    uint32_t ebp = (uint32_t)G32(VA_g_floorceil_accum_a), edx = (uint32_t)G32(VA_g_floorceil_accum_b);
    uint8_t  ah = (uint8_t)G8(VA_g_floorceil_accum_b + 0x4);
    for (int32_t i = 0; i < count; i++) {
        uint32_t ebx = fc_rotr32(ebp, 8);
        uint16_t idx = (uint16_t)((ebx & 0xff00u) | (uint8_t)(edx >> 8));
        uint8_t t = FC_RDB(cx->fs_base + idx);
        dst[i] = FC_RDB(cx->gs_base + (((uint32_t)ah << 8) | t));
        uint32_t o = ebp; ebp += step_lo; unsigned c1 = (ebp < o);
        uint64_t s = (uint64_t)edx + step_hi + c1; edx = (uint32_t)s; unsigned c2 = (unsigned)(s >> 32);
        ah = (uint8_t)(ah + shade_step + c2);
    }
}

static void fc_dispatch(uint32_t key, uint32_t edi_cur, int32_t count, uint8_t ah, const fc_ctx_t *cx) {
    switch (key) {
    case 0x3acec: fc_solid_3acec(edi_cur, count, cx);          break;
    case 0x3ad04: fc_colormap_3ad04(edi_cur, count, ah, cx);   break;
    case 0x3ad28: fc_affine_3ad28(edi_cur, count, cx);         break;
    case 0x3ad80: fc_persp_opaque_3ad80(edi_cur, count, cx);   break;
    case 0x3adc0: fc_persp_transp_3adc0(edi_cur, count, cx);   break;
    case 0x3ae04: fc_3ae04(edi_cur, count, cx);                break;
    case 0x3ae68: fc_3ae68(edi_cur, count, ah, cx);            break;
    case 0x3aed8: fc_3aed8(edi_cur, count, ah, cx);            break;
    case 0x3af38: fc_3af38(edi_cur, count, ah, cx);            break;
    case 0x3af94: fc_3af94(edi_cur, count, cx);                break;
    case 0x3aff4: fc_3aff4(edi_cur, count, cx);                break;
    case 0x3b070: fc_3b070(edi_cur, count, cx);                break;
    case 0x3b0d8: fc_deg_opaque_3b0d8(edi_cur, count, cx);     break;
    case 0x3b110: fc_deg_3b110(edi_cur, count, ah, cx);        break;
    case 0x3b15c: fc_deg_3b15c(edi_cur, count, cx);            break;
    default: break;
    }
}

/* fill_projection_linear_ramp (0x40cb1): linearly interpolate a run of `count` LUT entries between two
 * endpoint samples, writing into the projection LUT in the g_projection_build_dir [0x8c200] direction
 * (a SIGNED byte stride; its sign bit ALSO selects the path). Register ABI (from disasm — the Borland
 * decomp's CONCAT11/CARRY2 is unreliable): BP low byte = end sample, BL = start sample, EDI = dest ptr
 * (raw, no base add), DX = count. uVar2 = bp_low*0x101 - bl*0x101 (16-bit). step = (signext16(v)*2)/count
 * via `idiv cx`. accumulator edx seeded ((bp_low<<8)|0x80)<<1; per entry writes dh=(acc>>8)&0xff and
 * acc-=step. The two dir paths differ in (a) which 16-bit field of uVar2 feeds the step divide and (b) the
 * WRITE-vs-SUBTRACT order — negative dir writes THEN subtracts, positive dir subtracts THEN writes. Only
 * edx bits 0-15 reach dh (sub borrow propagates low->high), so edx/eax high bits are don't-cares. */
void fill_projection_linear_ramp(uint8_t bp_low, uint8_t bl, uint8_t *edi, uint16_t count)
{
    uint16_t uVar2 = (uint16_t)((uint16_t)(bp_low * 0x0101) - (uint16_t)(bl * 0x0101));
    int32_t  dir   = (int32_t)G32(VA_g_projection_build_dir);                  /* signed byte stride; bit15 selects path */
    uint32_t acc   = (((uint32_t)bp_low << 8) | 0x80u) << 1; /* mov edx,ebp; dh=dl; dl=0x80; add edx,edx */
    int16_t  c     = (int16_t)count;

    if (dir & 0x8000) {                                      /* NEGATIVE-dir: write then subtract */
        uint16_t v = uVar2;
        if ((int16_t)v >= 0) v = (uint16_t)(v + 0x100);      /* or ah,ah; jns -> inc ah */
        int16_t step = (int16_t)(((int32_t)(int16_t)v * 2) / (int16_t)count);  /* idiv cx */
        do {
            *edi = (uint8_t)(acc >> 8);
            acc -= (uint32_t)(int32_t)step;
            edi += dir;
        } while (--c > 0);
    } else {                                                 /* POSITIVE-dir: subtract then write */
        uint16_t v = (uint16_t)(uVar2 & 0xff00u);            /* xor al,al */
        int16_t step = (int16_t)(((int32_t)(int16_t)v * 2) / (int16_t)count);
        do {
            acc -= (uint32_t)(int32_t)step;
            *edi = (uint8_t)(acc >> 8);
            edi += dir;
        } while (--c > 0);
    }
}

/* interpolate_projection_gaps (0x40d1e): walk the projection LUT in the [0x8c200] dir, accumulating each
 * known sample in place (`*esi += sample`) and filling runs of unset (0x80) entries between two known
 * samples — a single-entry gap inline (`*edi = prev+cur`), longer runs via fill_projection_linear_ramp.
 * Register ABI (disasm): ESI=read ptr, EDI=write base (start of the current gap run), CX=count. DX=gap
 * length, BX=prev sample (sign-extended). Trailing-gap quirk: ebx -= dir before the final fill (so its
 * start sample = last_sample - dir). fill preserves ECX/EBX/EBP (push/pop), clobbers EDI/EAX/EDX — all
 * overwritten right after. */
void interpolate_projection_gaps(uint8_t *esi, uint8_t *edi, uint16_t cx_count)
{
    int32_t  dir = (int32_t)G32(VA_g_projection_build_dir);
    int16_t  dx  = 0;                               /* gap length */
    uint16_t bx  = 0;                               /* previous sample (sign-extended) */
    int16_t  cx  = (int16_t)cx_count;
    do {
        uint8_t al = *esi;
        if (al == 0x80) {                           /* sentinel -> extend the gap */
            esi += dir;
            dx++;
        } else {
            uint16_t bp = bx;                       /* mov bp,bx (prev sample) */
            bx = (uint16_t)(int16_t)(int8_t)al;     /* cbw; mov bx,ax (current, sign-extended) */
            *esi = (uint8_t)(*esi + (uint8_t)bx);   /* add [esi],bl (accumulate in place) */
            esi += dir;
            if (dx != 0) {
                if (dx == 1)
                    *edi = (uint8_t)((uint16_t)(bp + bx));                    /* single-gap inline */
                else
                    fill_projection_linear_ramp((uint8_t)bp, (uint8_t)bx, edi, (uint16_t)dx);
            }
            edi = esi;                              /* mov edi,esi */
            dx  = 0;                                /* xor dx,dx */
        }
    } while (--cx > 0);
    if (dx != 0) {                                  /* trailing gap */
        uint16_t bp = bx;
        bx = (uint16_t)((uint32_t)bx - (uint32_t)dir);                       /* sub ebx,[0x8c200] */
        fill_projection_linear_ramp((uint8_t)bp, (uint8_t)bx, edi, (uint16_t)dx);
    }
}

/* build_projection_table (0x40bd6): build/refresh the angle<->column projection LUT at 0x8c484 (then
 * mirror it to 0x8c204). DX selects the mode: 1 = dword-copy 0x8c204->0x8c484; 2 = word->byte downconvert
 * copy; 0 = the TRIG build; >=3 = nop. Mode-0 ABI (disasm): EBP = column range (&0xffff), ESI = a
 * projection-params struct (word fields +6 scale / +0xa center / +0xe min-cos gate). For each of 512
 * angles: sincos_pair(angle) -> sin/cos; if cos >= [esi+0xe], screen = [esi+0xa] - (sin*[esi+6])/cos
 * (signed 16-bit imul/idiv); if (uint16)screen < range, LUT[screen] = the ANGLE INDEX low byte (cl = the
 * restored loop counter — NOT sin, which only feeds the divide). Then interpolate_projection_gaps fills
 * the 0x80 gaps outward from the midpoint (dir +1 then -1), and the LUT is copied to 0x8c204. Both
 * callees (sincos_pair 0x3bdd2, interpolate_projection_gaps 0x40d1e) are native. [0x85498] = LUT byte
 * count. */
void build_projection_table(uint16_t mode, uint32_t ebp, const uint8_t *esi)
{
    uint8_t *lut = (uint8_t *)(uintptr_t)(0x8c484u + OBJ_DELTA);
    uint8_t *src = (uint8_t *)(uintptr_t)(0x8c204u + OBJ_DELTA);
    uint32_t n   = (uint32_t)G32(VA_g_screen_pitch);

    if (mode == 1) { memcpy(lut, src, (n >> 2) * 4); return; }           /* rep movsd (n/4 dwords) */
    if (mode == 2) {                                                     /* word -> byte downconvert */
        uint32_t cnt = n >> 1;
        for (uint32_t i = 0; i < cnt; i++) lut[i] = src[2 * i];
        return;
    }
    if (mode != 0) return;                                              /* mode >= 3: ret */

    ebp &= 0xffffu;
    memset(lut, 0x80, (n >> 2) * 4);                                    /* rep stosd 0x80808080 (n/4 dwords) */
    int16_t s6 = *(const int16_t *)(esi + 6);                          /* scale numerator */
    int16_t sa = *(const int16_t *)(esi + 0xa);                        /* screen center */
    int16_t se = *(const int16_t *)(esi + 0xe);                        /* min-cos gate */

    int32_t ecx = 0x200;
    do {
        uint16_t sin16, cos16; uint32_t tbl;
        sincos_pair((uint32_t)ecx, &sin16, &cos16, &tbl);
        if ((int16_t)cos16 >= se) {                                    /* cmp bx,[esi+0xe]; jl skip */
            int32_t prod = (int32_t)(int16_t)sin16 * (int32_t)s6;       /* imul word [esi+6] */
            int16_t ax   = (int16_t)((int16_t)(prod / (int16_t)cos16)); /* idiv bx -> ax */
            ax = (int16_t)(sa - ax);                                    /* neg ax; add ax,[esi+0xa] */
            if ((uint16_t)ax < (uint16_t)ebp)                          /* cmp ax,bp; jae skip (unsigned) */
                lut[(uint16_t)ax] = (uint8_t)ecx;                      /* [ebx+0x8c484] = cl (angle index) */
        }
    } while (--ecx > 0);

    uint32_t mid = ebp >> 1;                                            /* midpoint */
    G32(VA_g_projection_build_dir) = 1;                                                   /* dir +1 (forward) */
    interpolate_projection_gaps(lut + mid, lut + mid, (uint16_t)mid);
    G32(VA_g_projection_build_dir) = (int32_t)(-(int32_t)G32(VA_g_projection_build_dir));                   /* dir -1 (backward) */
    interpolate_projection_gaps(lut + mid, lut + mid, (uint16_t)(mid + 1));

    memcpy(src, lut, (n >> 2) * 4);                                     /* copy LUT 0x8c484 -> 0x8c204 */
}

/* rebuild_projection_table (0x409b4): the per-frame wrapper that recomputes the view-region offset and
 * dispatches build_projection_table with the live mode/range. Pulls the projection-params struct base
 * (0x71ee2), clears [0x8a310], computes the vertical region offset [0x90c0c] = (0xc8 - width)/2 (width
 * from [0x90bf6], halved + flag set when [esi+8] >= 0xbe), then calls build_projection_table with
 * DX=[0x90bfc] (mode, then cleared), BP=[0x90bf2] (range), ESI=the params struct. No register inputs. */
void rebuild_projection_table(void)
{
    const uint8_t *esi = (const uint8_t *)(uintptr_t)(0x71ee2u + OBJ_DELTA);
    G8(VA_g_render_double_scanline_flag) = 0;
    uint32_t ebx = (uint16_t)G16(VA_g_render_height);
    if (*(const uint16_t *)(esi + 8) >= 0xbe) {      /* cmp [esi+8],0xbe; jb skip */
        G8(VA_g_render_double_scanline_flag) = 1;
        ebx >>= 1;
    }
    uint32_t eax = (0xc8u - ebx) >> 1;               /* mov eax,0xc8; sub eax,ebx; shr eax,1 */
    G16(VA_g_viewport_top_margin) = (uint16_t)eax;
    uint16_t mode = (uint16_t)G16(VA_g_render_viewport_reconfig);
    G16(VA_g_render_viewport_reconfig) = 0;
    uint16_t bp = (uint16_t)G16(VA_g_render_width);
    build_projection_table(mode, bp, esi);
}

/* project_point_to_screen_column (0x43d04): perspective-project a 2D point to a clamped screen column.
 * ABI (disasm): DX = x, BX = y, EAX = depth/scale base (the divisor is EAX+1). Builds the rotate frame
 * {x, y, flags=-[0x909f8]}, calls rotate_point_2d (0x2a898, native), takes its EDX output (the rotated
 * x-projection) >> 8, scales by 0x7400, divides by (EAX+1), biases by 0x8000, and saturates to [0,0xffff].
 * Returns EAX. Pure (reads [0x909f8]/[0x85310]/[0x85312], writes no obj3). */
uint32_t project_point_to_screen_column(int16_t dx, int16_t bx, uint32_t eax_in)
{
    int16_t frame[3];
    frame[0] = dx;                                                  /* [ebp]   = dx (x) */
    frame[1] = bx;                                                  /* [ebp+2] = bx (y) */
    frame[2] = (int16_t)(-(int32_t)(uint16_t)G16(VA_g_sprite_view_angle));         /* [ebp+4] = -[0x909f8] (flags, low16) */
    int32_t eo, ed;
    rotate_point_2d(frame, &eo, &ed);
    int32_t a = ed >> 8;                                           /* sar edx,8; xchg edx,eax -> eax = ed>>8 */
    int32_t ebx = (int32_t)(eax_in + 1);                           /* pop ebx (entry eax); inc ebx */
    int64_t prod = (int64_t)a * 0x7400;                            /* mov edx,0x7400; imul edx (EDX:EAX) */
    int32_t q = (int32_t)(prod / ebx) + 0x8000;                    /* idiv ebx; add eax,0x8000 */
    if ((uint32_t)q < 0xffffu) return (uint32_t)q;                 /* jb -> keep [0,0xfffe] */
    if (q < 0xffff) return 0;                                      /* jl -> negative -> 0 */
    return 0xffff;                                                 /* else saturate high */
}

/* setup_render_projection_scale (0x2e458): recompute the perspective SCALE factors + view geometry from
 * the viewport dims and aspect/FOV constants, then rebuild the perspective LUT. Pure fixed-point global
 * plumbing (transcribed from DISASM — widths/signedness exact). Writes the aspect scale [0x85430], view
 * center (0x90a70/72, 0x71eec/ee), the image-surface clip bounds via the stored ptr [0x90a9c]+0xc/+0xe, the
 * world/sprite scales (0x8540c/0x85410/0x85408/0x85404/0x71ee8/0x71eea, halved/doubled per [0x90bd4] &
 * [0x90cbd]), the viewport area (0x89f2a/0x89f2c), and the centered framebuffer (0x85414/0x85418). Then
 * bridges the DPMI selector-limit set (0x2fdfc, int 0x31) and tail-calls rebuild_projection_table (native).
 * The 0x854a4 aspect const, view_w/h (0x85cd8/0x85cdc), dims (0x90bf2/4/6) are inputs. */
void setup_render_projection_scale(void)
{
    G32(VA_g_render_target_buffer + 0x1c) = ((uint32_t)((int32_t)G32(VA_g_screen_height + 0x4) * 0x132)) >> 8;          /* imul 0x132; shr 8 */
    uint16_t a = (uint16_t)((uint16_t)G16(VA_g_render_width) >> 1);
    uint16_t b = (uint16_t)((uint16_t)G16(VA_g_render_height) >> 1);
    G16(VA_g_init_stage_error_strings + 0x11c) = a; G16(VA_g_init_stage_error_strings + 0x11e) = b; G16(VA_g_view_center_y) = b; G16(VA_g_view_center_x) = a;
    uint16_t aa = (uint16_t)((uint16_t)G16(VA_g_render_width) - 1);
    uint16_t bb = (uint16_t)((uint16_t)G16(VA_g_render_viewport_height) - 1);
    uint32_t edi = (uint32_t)G32(VA_g_image_surface);                                    /* stored ptr = host addr */
    *(volatile uint16_t *)(uintptr_t)(edi + 0xe) = aa;
    *(volatile uint16_t *)(uintptr_t)(edi + 0xc) = bb;

    uint32_t eax = (uint16_t)G16(VA_g_view_w);                                    /* view_w (zext16) */
    eax = (uint32_t)((int32_t)eax * (int32_t)G32(VA_g_render_target_buffer + 0x1c));
    uint32_t edx = (uint32_t)(uint16_t)G16(VA_g_view_h) << 16;
    uint32_t e, f;
    if (eax >= edx) {                                                         /* jb branch2 -> else here */
        e = (uint16_t)G16(VA_g_view_w);
        e = ((uint32_t)((int32_t)e * 0x7c)) >> 8;
    } else {
        uint32_t q = 0xa00000u / (uint32_t)G32(VA_g_screen_height + 0x4);                      /* unsigned div */
        e = (uint16_t)G16(VA_g_view_h);
        e = ((uint32_t)((int32_t)e * (int32_t)q)) >> 8;
    }
    G16(VA_g_init_stage_error_strings + 0x118) = (uint16_t)e;
    G32(VA_g_das_render_scale + 0x8) = e; G32(VA_g_das_render_scale + 0xc) = e;
    f = ((uint32_t)((int32_t)e * (int32_t)G32(VA_g_render_target_buffer + 0x1c))) >> 16;
    G16(VA_g_init_stage_error_strings + 0x11a) = (uint16_t)f;

    if (G8(VA_g_hires_line_doubling_flag) != 0) {                                                   /* hires line doubling */
        G16(VA_g_init_stage_error_strings + 0x11a) = (uint16_t)((uint16_t)G16(VA_g_init_stage_error_strings + 0x11a) << 1);
        G32(VA_g_das_render_scale + 0xc) = G32(VA_g_das_render_scale + 0xc) << 1;
    }
    G32(VA_g_das_render_scale) = G32(VA_g_das_render_scale + 0x8);
    int32_t eax2 = (int32_t)G32(VA_g_das_render_scale + 0xc);
    if ((uint32_t)G32(VA_g_screen_height + 0x4) != 0xcccc) {                                   /* imul [0x85430]; idiv 0xcccc */
        int64_t prod = (int64_t)eax2 * (int32_t)G32(VA_g_render_target_buffer + 0x1c);
        eax2 = (int32_t)(prod / 0xcccc);
    }
    G32(VA_g_das_render_scale + 0x4) = (uint32_t)eax2;
    if (G8(VA_g_view_scale_flags) & 1) {                                                    /* view_scale_flags & 1 */
        G32(VA_g_das_render_scale + 0x8) = G32(VA_g_das_render_scale + 0x8) >> 1;
        G16(VA_g_init_stage_error_strings + 0x118) = (uint16_t)((uint16_t)G16(VA_g_init_stage_error_strings + 0x118) >> 1);
    }
    if (G8(VA_g_view_scale_flags) & 2) {                                                    /* view_scale_flags & 2 */
        G32(VA_g_das_render_scale + 0xc) = G32(VA_g_das_render_scale + 0xc) >> 1;
        G16(VA_g_init_stage_error_strings + 0x11a) = (uint16_t)((uint16_t)G16(VA_g_init_stage_error_strings + 0x11a) >> 1);
    }
    uint16_t h = (uint16_t)G16(VA_g_render_viewport_height);
    if (h != (uint16_t)G16(VA_g_render_target_secondary_height)) {                                        /* viewport area refresh */
        G16(VA_g_render_target_secondary_height) = h;
        G32(VA_g_render_target_secondary_size) = (uint32_t)((int32_t)(int16_t)h * (int32_t)G32(VA_g_screen_pitch)); /* cwde; imul */
    }
    uint32_t e3 = (uint32_t)G32(VA_g_view_y);
    if (G8(VA_g_hires_line_doubling_flag) != 0) e3 += e3;
    e3 = (uint32_t)((int32_t)e3 * (int32_t)G32(VA_g_screen_pitch));
    e3 += (uint32_t)G32(VA_g_view_x);
    G32(VA_g_render_target_buffer + 0x4) = e3;
    e3 += (uint32_t)G32(VA_g_framebuffer_ptr);
    G32(VA_g_render_target_buffer) = e3;

    { regs_t io; memset(&io, 0, sizeof io);                                  /* bridge 0x2fdfc (DPMI limit) */
      io.va = 0x2fdfcu + OBJ_DELTA; io.eax = (uint32_t)G32(VA_g_render_target_buffer);
#ifndef ROTH_STANDALONE
      call_orig(&io);
#else
      vd_standalone_set_fb_bases(io.eax);   /* 0x2fdfc transcription (lift_video_display.c); CF unread here */
#endif
    }
    rebuild_projection_table();                                       /* tail jmp 0x409b4 */
}

/* configure_render_viewport (0x408d1): derive the live viewport dims/scales from the raw view-window
 * width/height (0x85cd8/0x85cdc, aligned to 8 / even), the secondary dims (0x85ce0/0x85ce4, masked), the
 * scale flags [0x90bd4] (bit0 halves width, bit1 halves height) and the hires flag [0x90cbd] (doubles),
 * writing the resolved dims [0x90bf2/4/6], strides [0x90bee/f0], and an over-0xc8-height clamp flag
 * [0x90bfe]. Then calls setup_render_projection_scale (native) and, when [0x8c1d2]==0, bridges
 * blank_active_video_page (0x2e140, VGA). Clears [0x8c1d2]. Transcribed from disasm (sub-register widths
 * exact: `and al,0xfe`, 16-bit `shr ax,1`, the cmp ax,0xc8 clamp leaving EBX at its pre-double value). */
void configure_render_viewport(void)
{
    uint32_t eax = (uint32_t)G32(VA_g_screen_pitch + 0x4);
    if (G8(VA_g_hires_line_doubling_flag) != 0) eax += eax;
    G32(VA_g_screen_height) = eax;
    G16(VA_g_frame_tick_counter + 0x4) = (uint16_t)((uint16_t)G16(VA_g_frame_tick_counter) - 7);
    eax = (uint32_t)G32(VA_g_view_w) & 0xfffffff8u;
    G32(VA_g_view_w) = eax;
    uint32_t ecx = (uint32_t)G32(VA_g_view_x);
    if (G8(VA_g_view_scale_flags) & 1) eax >>= 1;                              /* shr eax,1 (32-bit) */
    G16(VA_g_render_width) = (uint16_t)eax;
    ecx &= 0xfffffffcu;
    G16(VA_g_geometry_selector + 0x8) = (uint16_t)ecx;
    G16(VA_g_subpass_patch_gate) = 0;
    eax = (uint32_t)G32(VA_g_view_h) & 0xfffffffeu;                  /* and al,0xfe (clears bit0) */
    G32(VA_g_view_h) = eax;
    ecx = (uint32_t)G32(VA_g_view_y);
    uint32_t ebx;
    if (G8(VA_g_view_scale_flags) & 2) {
        eax = (eax & 0xffff0000u) | ((uint32_t)((uint16_t)eax >> 1));   /* shr ax,1 (16-bit) */
        ebx = eax;
        if (G8(VA_g_hires_line_doubling_flag) != 0) { ecx += ecx; eax += eax; ebx = eax; }
    } else {
        ebx = eax;
        ecx = (uint32_t)G32(VA_g_view_y);
        if (G8(VA_g_hires_line_doubling_flag) != 0) {
            ecx += ecx; eax += eax;
            if ((uint16_t)eax <= 0xc8) ebx = eax;               /* cmp ax,0xc8; ja -> clamp */
            else G16(VA_g_subpass_patch_gate) = 1;                              /* (EBX stays the pre-double value) */
        }
    }
    G16(VA_g_render_viewport_height) = (uint16_t)ebx;
    G16(VA_g_geometry_selector + 0x6) = (uint16_t)ecx;
    G16(VA_g_render_height) = (uint16_t)eax;
    setup_render_projection_scale();                     /* call 0x2e458 (native) */
    if (G8(VA_g_collision_sector_stack + 0x3e) == 0) {                                     /* bridge blank_active_video_page (VGA) */
        regs_t io; memset(&io, 0, sizeof io); io.va = 0x2e140u + OBJ_DELTA;
#ifndef ROTH_STANDALONE
        call_orig(&io);
#else
        host_blank_active_video_page();   /* 0x2e140 host body (traps.c) */
#endif
    }
    G8(VA_g_collision_sector_stack + 0x3e) = 0;
}

/* floorceil_rotation_sincos — canon 0x3bdf3: rotate the 2D point {x=[ebp], y=[ebp+4]} in place by
 * the angle in [ebp+8] (units of 512 = one turn). cos = sintab[a], sin = sintab[(a+0x80)&0x1ff]
 * (the quarter-turn offset comes from `inc bh; and bh,3` on the 2a byte offset). Products are 32-bit
 * `imul r32,r32` (low 32 truncated); the result is `>>14` arithmetic:
 *     y' = (y*sin - x*cos) >> 14,   x' = (x*sin + y*cos) >> 14.
 * No-op if (angle & 0x1ff)==0. Reads only the 512-entry sine table at 0x72080 (pure leaf, ebp-struct). */
void floorceil_rotation_sincos(int32_t *pt)   /* pt[0]=x, pt[1]=y, pt[2]=angle */
{
    const int16_t *sintab = (const int16_t *)GADDR(VA_g_sincos_table);
    uint32_t a = (uint32_t)pt[2] & 0x1ffu;          /* test ebx,0x1ff (only low 9 bits) */
    if (a == 0) return;                             /* je 0x3be42 (ret) */
    int32_t cosv = sintab[a];                       /* movsx ecx, word[2a + 0x72080] */
    int32_t sinv = sintab[(a + 0x80u) & 0x1ffu];    /* movsx ebx, word[((2a+0x100)&0x3ff) + 0x72080] */
    int32_t x = pt[0], y = pt[1];                   /* captured before either store */
    int32_t xcos = (int32_t)((uint32_t)x * (uint32_t)cosv);   /* imul (32-bit truncating) */
    int32_t ysin = (int32_t)((uint32_t)y * (uint32_t)sinv);
    pt[1] = (ysin - xcos) >> 14;                    /* sar edx,0xe ; mov [ebp+4],edx */
    int32_t ycos = (int32_t)((uint32_t)y * (uint32_t)cosv);
    int32_t xsin = (int32_t)((uint32_t)x * (uint32_t)sinv);
    pt[0] = (xsin + ycos) >> 14;                    /* sar edx,0xe ; mov [ebp],edx */
}

/* shift_wall_nodes_vertical — canon 0x28972: walk the wall-node list (ES = record selector [0x85294],
 * FS = surface selector [0x852c8]) from offset 2 to the end [0x852ce]. For each node, count = fs:[node+0xd]
 * gives count+1 sub-records (header 8 bytes, sub-record stride 0x14); subtract the vertical shift [0x853c8]
 * from both y-fields (es:[rec+4], es:[rec+6]). Only the ES buffer is mutated (no obj3/global writes). The
 * outer compare is 16-bit (si = low word of esi). */
void shift_wall_nodes_vertical(uint32_t es_base, uint32_t fs_base)
{
    #define ES16(o) (*(volatile uint16_t *)(uintptr_t)(es_base + (uint32_t)(o)))
    #define FS8(o)  (*(volatile uint8_t  *)(uintptr_t)(fs_base + (uint32_t)(o)))
    uint16_t di = (uint16_t)G16(VA_g_vertex_selector + 0x2);              /* end offset */
    uint16_t dx = (uint16_t)G16(VA_g_reflection_view_list + 0x84);              /* shift amount */
    uint32_t esi = 2;
    while ((uint16_t)esi < di) {                       /* cmp si,di; jae ret / jb loop (unsigned) */
        uint16_t bx = ES16(esi);                       /* node surface index */
        uint8_t  cl = (uint8_t)(FS8(bx + 0xd) + 1);    /* cl = count; inc ecx */
        esi += 8;                                      /* skip node header */
        do {
            ES16(esi + 4) = (uint16_t)(ES16(esi + 4) - dx);
            ES16(esi + 6) = (uint16_t)(ES16(esi + 6) - dx);
            esi += 0x14;
        } while ((int8_t)(--cl) > 0);                  /* dec cl; jg */
    }
    #undef ES16
    #undef FS8
}

/* compute_surface_normal_shade (0x3c0be): from a 3-vertex structure at EBP (3 coords x 3 verts as dwords
 * [ebp+0..0x2c] + a light/scale factor [ebp+0x30]), build two edge-cross-product normals, take each
 * magnitude via isqrt_fixed (0x3bfe5, native), and return the normalized dot:
 *     (dot . [ebp+0x30]) / (|n1| * |n2|)   (64-bit imul then idiv; magprod==0 -> low-32 of the product).
 * Pure (stack-frame locals only, output EAX). All intermediate products are 32-bit imul (truncating). */
int32_t compute_surface_normal_shade(uint32_t ebp)
{
    #define P(o)    ((int32_t)*(volatile int32_t *)(uintptr_t)(ebp + (uint32_t)(o)))
    #define MUL(a,b)((int32_t)((uint32_t)(a) * (uint32_t)(b)))   /* 32-bit imul (low 32) */
    int32_t d20 = P(0x20), d1c = P(0x1c), d18 = P(0x18);
    int32_t a28 = P(0x2c) - d20, a2c = P(0x14) - d20, a30 = P(0x08) - d20;   /* [esp+0x28/0x2c/0x30] */
    int32_t a14 = P(0x28) - d1c, a18 = P(0x10) - d1c, a1c = P(0x04) - d1c;   /* [esp+0x14/0x18/0x1c] */
    int32_t a00 = P(0x24) - d18, a04 = P(0x0c) - d18, a08 = P(0x00) - d18;   /* [esp+0/4/8] */
    int32_t nA = MUL(a18, a00) - MUL(a04, a14);                  /* [esp+0x34] */
    int32_t ebx = MUL(nA, nA);
    int32_t nB = MUL(a28, a04) - MUL(a2c, a00);                  /* [esp+0x20] */
    ebx += MUL(nB, nB);
    int32_t nC = MUL(a2c, a14) - MUL(a28, a18);                  /* [esp+0xc] */
    int32_t s1 = isqrt_fixed((uint32_t)(MUL(nC, nC) + ebx));          /* isqrt(|n1|^2) */
    int32_t s2 = isqrt_fixed((uint32_t)(MUL(a30, a30) + MUL(a1c, a1c) + MUL(a08, a08)));  /* isqrt(|n2|^2) */
    int32_t magprod = MUL(s1, s2);
    int32_t dot = MUL(a30, nA) + MUL(a1c, nB) + MUL(a08, nC);
    int64_t prod = (int64_t)dot * (int64_t)P(0x30);              /* imul dword [ebp+0x30] (64-bit) */
    if (magprod == 0) return (int32_t)prod;                      /* or ebx,ebx; je (skip idiv) */
    return (int32_t)(prod / magprod);                            /* idiv ebx */
    #undef P
    #undef MUL
}

/* project_wall_edge_y (0x38c46): map a wall-edge Y (AX) to a shade/texcoord word. Clamp AX to the view band
 * [0x9096e, 0x9096c]; a = |AX - 0x90992|; AX = (a * 0x90996) >> 8; AX = max(AX, CX) (unsigned); AX += EDX
 * (low 16); shade = (AX>>5) - 0x90a1e - imm@0x38c94 (SMC, read live). shade<=0 -> 0x100; clamp to 0x90a20;
 * shade>=0x1f -> 0x1fff (and [0x8a350]++); else return (shade<<8) | ((AX&0xff)<<3). Pure leaf; output depends
 * only on the low 16 of EAX (the `neg` stale hi-word never reaches the result). */
uint16_t project_wall_edge_y(int16_t ax_in, uint16_t cx, uint32_t edx)
{
    int16_t ax = ax_in;
    if (ax >= (int16_t)G16(VA_g_view_bound_bottom)) ax = (int16_t)G16(VA_g_view_bound_bottom);     /* cmp ax,[9096c]; jl skip; mov ax,[9096c] */
    if (ax <= (int16_t)G16(VA_g_view_bound_top)) ax = (int16_t)G16(VA_g_view_bound_top);     /* cmp ax,[9096e]; jg skip; mov ax,[9096e] */
    int16_t t = (int16_t)((uint16_t)ax - (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x16));    /* sub ax,[90992] */
    uint16_t a = (t < 0) ? (uint16_t)(-(int)t) : (uint16_t)t;        /* jns skip; neg eax */
    uint32_t prod = (uint32_t)a * (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x1a);            /* mul [0x90996] (unsigned) */
    uint16_t axw = (uint16_t)(prod >> 8);                            /* al=ah; ah=dl -> bits 8..23 */
    if (!(axw > cx)) axw = cx;                                       /* cmp ax,cx; ja skip; mov eax,ecx (unsigned) */
    uint16_t ax2 = (uint16_t)(axw + (uint16_t)edx);                  /* add eax,edx (low 16) */
    uint8_t  dl = (uint8_t)ax2;                                      /* mov dl,al */
    /* SMC shr/shl counts @0x38c8a/@0x38cb3 (0x2d6a8-patched per shade level) — read LIVE */
    uint8_t  shr_n = (uint8_t)(*(volatile uint8_t *)GADDR(VA_g_shade_shift_count_default) & 0x1f);
    uint8_t  shl_n = (uint8_t)(*(volatile uint8_t *)GADDR(0x38cb3) & 0x1f);
    int16_t  sh = (int16_t)((uint16_t)ax2 >> shr_n);                 /* shr ax,imm@0x38c8a */
    sh = (int16_t)((uint16_t)sh - (uint16_t)G16(VA_g_floorceil_depth_clip_bias));           /* sub ax,[90a1e] */
    sh = (int16_t)((uint16_t)sh - (uint16_t)G16(VA_g_wd_project_bias_default));           /* sub ax,imm (SMC @0x38c94, live) */
    if (sh <= 0) return 0x100;                                       /* jle 0x38cb5 */
    if (sh >= (int16_t)G16(VA_g_floorceil_clip_scale)) sh = (int16_t)G16(VA_g_floorceil_clip_scale);     /* cmp; jl skip; mov ax,[90a20] */
    if (sh >= 0x1f) { G8(VA_g_span_draw_mode_flags + 0x4) = (uint8_t)(G8(VA_g_span_draw_mode_flags + 0x4) + 1); return 0x1fff; }  /* jge 0x38cbb */
    return (uint16_t)(((uint16_t)(uint8_t)sh << 8) | (uint8_t)((uint8_t)dl << shl_n));  /* ah=al; al=dl<<imm@0x38cb3 */
}

/* 2D cross of (P2-P0)x(P1-P0) over object vertices: cross = dx2*dy1 - dx1*dy2, with the per-vertex screen
 * coords at [vtxbase + idx + 0xc] (X) / +0xe (Y), movsx 16->32, products 32-bit. Used by the object
 * backface/winding test in compute_object_screen_bbox. i0/i1/i2 are the (already-dereferenced) vtx indices. */
static int32_t bbox_winding_cross(uint32_t vb, uint32_t i0, uint32_t i1, uint32_t i2)
{
    #define VX(ix) ((int32_t)(int16_t)*(volatile uint16_t *)(uintptr_t)(vb + (uint32_t)(ix) + 0xc))
    #define VY(ix) ((int32_t)(int16_t)*(volatile uint16_t *)(uintptr_t)(vb + (uint32_t)(ix) + 0xe))
    int32_t x0 = VX(i0), y0 = VY(i0);
    int32_t dx1 = VX(i1) - x0, dy1 = VY(i1) - y0;
    int32_t dx2 = VX(i2) - x0, dy2 = VY(i2) - y0;
    return dx2 * dy1 - dx1 * dy2;
    #undef VX
    #undef VY
}

/* compute_object_screen_bbox (0x3c598, entered via thunk 0x3cac5 which reserves the stack local): two
 * stages. (1) Scan the object's `count` vertices ([esi+0x34]) for the screen X/Y min/max (X at [vb+idx+0xc],
 * Y at +0xe; vb=[esi+0x30]); cull (return CF=1) if the bbox falls outside the view window [0x90968..0x9096e],
 * else store {minX,minY,maxX,maxY} -> [esi+0x28/0x2a/0x2c/0x2e] (and leave the live min/max at [0x8b354]/
 * [0x8b356]). (2) Backface/winding test gated by flags [esi+0x16]: the 0x10&!0x200 fast paths (0x20 -> a
 * single signed [vb+idx+8] with optional 0x80 negate; else compare two X coords) or the general up-to-three
 * 2D-cross-product chain over the first verts -> CF=0 (visible, also clears [esi+0x14] bit8) / CF=1 (cull). */
int compute_object_screen_bbox(uint32_t esi)
{
    #define R8(a)   (*(volatile uint8_t  *)(uintptr_t)(a))
    #define R16(a)  (*(volatile uint16_t *)(uintptr_t)(a))
    #define R32(a)  (*(volatile uint32_t *)(uintptr_t)(a))
    #define W16(a,v)(*(volatile uint16_t *)(uintptr_t)(a) = (uint16_t)(v))
    uint32_t vb = R32(esi + 0x30);                         /* vtx base */
    uint32_t count = R16(esi + 0x34);
    int16_t minY = 0x7fff, maxY = -1, minX = 0x7fff, maxX = -1;
    uint32_t list = esi + 0x36;
    for (uint32_t i = 0; i < count; i++) {                 /* dec ecx; jg -> count iters */
        uint16_t idx = R16(list); list += 2;
        int16_t vy = (int16_t)R16(vb + idx + 0xe);
        if (vy < minY) minY = vy;                          /* jge skip; mov [8b354],ax */
        if (vy > maxY) maxY = vy;
        int16_t vx = (int16_t)R16(vb + idx + 0xc);
        if (vx < minX) minX = vx;
        if (vx > maxX) maxX = vx;
    }
    W16(0x8b354 + OBJ_DELTA, minY);                        /* live min/max Y (the orig writes them in-loop) */
    W16(0x8b356 + OBJ_DELTA, maxY);
    if (minX >= (int16_t)G16(VA_g_view_bound_right)) return 1;           /* cull */
    if (maxX <= (int16_t)G16(VA_g_view_bound_left)) return 1;
    W16(esi + 0x28, minX); W16(esi + 0x2c, maxX);
    if (minY > (int16_t)G16(VA_g_view_bound_bottom)) return 1;
    W16(esi + 0x2a, minY);
    if (maxY <= (int16_t)G16(VA_g_view_bound_top)) return 1;
    W16(esi + 0x2e, maxY);

    /* ---- backface / winding ---- */
    int count2 = (int)(uint8_t)R8(esi + 0x34) - 2;         /* movzx byte; sub 2 */
    if (count2 < 0) goto visible;                          /* jl 0x3c7d7 */
    uint16_t f16 = R16(esi + 0x16);
    if ((f16 & 0x10) && !(f16 & 0x200)) {                  /* 0x3c659/0x3c661 */
        if (f16 & 0x20) {                                  /* 0x3c6af */
            int32_t v = (int16_t)R16(vb + R16(esi + 0x38) + 8);  /* movsx [vb+idx+8] */
            if (f16 & 0x80) v = -v;
            if (v > 0) goto visible;                       /* or eax,eax; jg */
            return 1;
        }
        int16_t x0 = (int16_t)R16(vb + R16(esi + 0x36) + 0xc);   /* 0x3c674 */
        int16_t d  = (int16_t)(x0 - (int16_t)R16(vb + R16(esi + 0x38) + 0xc));
        if (d < 0) goto visible;                           /* js */
        if (d != 0) return 1;                              /* jne */
        int16_t x2 = (int16_t)R16(vb + R16(esi + 0x3c) + 0xc);
        int16_t d2 = (int16_t)(x2 - (int16_t)R16(vb + R16(esi + 0x3a) + 0xc));
        if (d2 < 0) goto visible;
        return 1;
    }
    /* general path (0x3c6cf): up to 3 cross products */
    {
        uint32_t i0 = R16(esi + 0x36), i1 = R16(esi + 0x38);
        uint32_t i2 = R16(esi + (((uint32_t)count2) & ~1u) + 0x3a);
        int32_t c1 = bbox_winding_cross(vb, i0, i1, i2);
        if (c1 < 0) goto visible;
        if (c1 != 0) return 1;
        if ((uint8_t)R8(esi + 0x34) < 4) return 1;         /* 0x3c729 cmp byte,4; jl cull */
        int32_t c2 = bbox_winding_cross(vb, R16(esi + 0x36), R16(esi + 0x3a), R16(esi + 0x3c));
        if (c2 < 0) goto visible;
        if (c2 != 0) return 1;
        int32_t c3 = bbox_winding_cross(vb, R16(esi + 0x38), R16(esi + 0x3a), R16(esi + 0x3c));
        /* FIDELITY FIX (disasm 0x3c7d3 `jns 0x3c7e2` / 0x3c7d5 `je 0x3c7e2`): the FINAL cross
         * rejects on c3 >= 0 INCLUDING c3 == 0 (a degenerate/collinear polygon is culled), unlike
         * c1/c2 which use `js`/`jne` where ==0 continues to the next cross. The earlier lift let
         * c3 == 0 fall through to `visible`, keeping degenerate polygons the original culls. Found
         * during the task-#111 tree-streak hunt; verified faithful vs the original (ROTH_LIFT_DIFF
         * A/B: 0 keep/cull divergences with this fix). NOTE: this is NOT the tree-streak cause —
         * the streak cards take the FAST backface path (flag16 bit-0x10), and clip_object_to_frustum
         * + compute_object_screen_bbox are both A/B-proven byte/decision-faithful. */
        if (c3 < 0) goto visible;                              /* js 0x3c7d7 -> visible */
        return 1;                                              /* jns/je 0x3c7e2 -> cull (c3 >= 0) */
    }
visible:
    W16(esi + 0x14, R16(esi + 0x14) & 0xfeffu);            /* and [esi+0x14],0xfeff */
    return 0;
    #undef R8
    #undef R16
    #undef R32
    #undef W16
}

/* thunk_compute_object_screen_bbox (0x3cac5): `sub esp,4; jmp 0x3c598` — reserves the stack local and tail-
 * jumps into the body above. Native form is just the call. */
int thunk_compute_object_screen_bbox(uint32_t esi) { return compute_object_screen_bbox(esi); }

/* interpolate_object_clip_vertex (0x3cad6): emit the clip-crossing vertex EDI between vertices ESI(A) and
 * EBX(B) at parameter DX(t). Fields +6/+8 are linearly interpolated by t over the depth delta bp =
 * [A+0xa]-[B+0xa] (16-bit imul t / idiv bp), +0xa copied from A, and +0..+5 (the first 6 bytes) copied from
 * A. Then it perspective-projects the new vertex to screen X([edi+0xc]) / Y([edi+0xe]) via the 16x16->32
 * imul/idiv-0x10 with a +-8 high-word range gate: if X's product hi-word is out of (-8,8) it returns
 * WITHOUT writing X or Y; likewise Y's gate skips only Y. Leaf (idiv only). Returns CF: 1 when either gate
 * trips (orig `stc; ret` @0x3cb7c — used by clip_object_to_frustum to reject), 0 on full projection. */
int interpolate_object_clip_vertex(uint32_t esi, uint32_t ebx, uint32_t edi, int16_t t)
{
    #define A16(o) ((int16_t)*(volatile uint16_t *)(uintptr_t)(esi + (uint32_t)(o)))
    #define B16(o) ((int16_t)*(volatile uint16_t *)(uintptr_t)(ebx + (uint32_t)(o)))
    #define DW(o,v) (*(volatile uint16_t *)(uintptr_t)(edi + (uint32_t)(o)) = (uint16_t)(v))
    #define DR(o)  ((int16_t)*(volatile uint16_t *)(uintptr_t)(edi + (uint32_t)(o)))
    int16_t bp = (int16_t)(A16(0xa) - B16(0xa));                       /* depth delta */
    int16_t q6 = (int16_t)(((int32_t)(int16_t)(B16(6) - A16(6)) * (int32_t)t) / (int32_t)bp); /* imul dx; idiv bp */
    DW(6, (int16_t)(q6 + A16(6)));
    int16_t q8 = (int16_t)(((int32_t)(int16_t)(B16(8) - A16(8)) * (int32_t)t) / (int32_t)bp);
    DW(8, (int16_t)(q8 + A16(8)));
    DW(0xa, (uint16_t)A16(0xa));
    DW(0, (uint16_t)A16(0)); DW(2, (uint16_t)A16(2)); DW(4, (uint16_t)A16(4));  /* movsd+movsw copy of +0..5 */
    /* screen X (0x3cb2a): [edi+6] * g_9099c, gated by hi-word in (-8,8) */
    int32_t px = (int32_t)DR(6) * (int32_t)(int16_t)G16(VA_g_span_src_wrap_reoffset + 0x20);
    int16_t hx = (int16_t)(px >> 16);
    if (hx >= 8 || hx <= -8) return 1;                               /* jge/jle 0x3cb7c -> stc (CF=1) */
    DW(0xc, (int16_t)((int16_t)(px / 0x10) + (int16_t)G16(VA_g_span_src_wrap_reoffset + 0x24))); /* idiv bx(0x10); add [0x909a0] */
    /* screen Y (0x3cb4f): (-[edi+8]) * g_90998, same range gate */
    int16_t ny = (int16_t)(uint16_t)(-(uint16_t)(uint16_t)DR(8));     /* neg (16-bit) */
    int32_t py = (int32_t)ny * (int32_t)(int16_t)G16(VA_g_span_src_wrap_reoffset + 0x1c);
    int16_t hy = (int16_t)(py >> 16);
    if (hy <= -8 || hy >= 8) return 1;                               /* jle/jge 0x3cb7c -> stc (CF=1) */
    DW(0xe, (int16_t)((int16_t)(py / 0x10) + (int16_t)G16(VA_g_span_src_wrap_reoffset + 0x28)));
    return 0;                                                        /* clc (CF=0) */
    #undef A16
    #undef B16
    #undef DW
    #undef DR
}

/* clip_object_to_frustum (0x3c892): Sutherland-Hodgman near-plane clip of an object polygon. Reads the source
 * record at ESI (header 0x30 + vcount byte[+0x34] + index list words[+0x36]); builds a NEW clipped record in
 * the node arena ([0x8b32c] cursor, [0x8b334] remaining bytes). Each source vertex is tested vs the near plane
 * (word[vtx+0xa] >= 0x10 = inside); inside<->outside transitions interpolate the crossing vertex via 0x3cad6
 * (CF=1 -> reject). A parallel "provenance" word list at [0x8b308] records which input vertex each clipped
 * vertex came from (cnt / cnt-1), copied at the end to the caller's vtx buffer at EDI+0x38+2*outcount. Finishes
 * with the screen-bbox/backface test (0x3cac5). Returns CF (1 = reject). EDI(in)= caller's source vtx base
 * (== [ESI+0x30]). es base is FLAT (0) here: arena cursor / vtx base / record pointers are absolute host
 * addresses, every es:[edi]/DS access is a raw deref. The orig reverse-pushes the source index list onto the
 * cpu stack then pops it forward -> modelled as the straight forward walk word[src+0x36+2*k]. The transient
 * saved-esp [0x8b2e8] is a meaningless sentinel in C (the oracle masks it). */
int clip_object_to_frustum(uint32_t esi_in, uint32_t edi_in)
{
    uint8_t *src = (uint8_t *)(uintptr_t)esi_in;
    uint32_t n = (uint32_t)src[0x34] + 1;                          /* vcount+1 (loop count / index entries) */
    G32(VA_g_sprite_node_pool + 0x810) = 0x8b308u + OBJ_DELTA;                          /* provenance cursor -> [0x8b308] (BEFORE check) */
    G32(VA_g_sprite_node_pool + 0x828) = 0;                                             /* input-vertex counter (cnt) (BEFORE check) */
    if ((int32_t)(0x38u + 20u * n) > (int32_t)G32(VA_g_sprite_node_pool + 0x864)) return 1;  /* arena overflow -> reject */
    uint32_t ebx_off = 0x38u + 4u * n;                            /* clipped-vtx region offset within record */

    uint32_t arena = (uint32_t)G32(VA_g_sprite_node_pool + 0x85c);                      /* es:[edi] base = new record base */
    uint8_t *rec = (uint8_t *)(uintptr_t)arena;
    memcpy(rec, src, 0x30);                                       /* rep movsd 0xc (header copy) */
    *(uint32_t *)(rec + 0x30) = arena;                            /* record self-ptr */
    G32(VA_g_sprite_node_pool + 0x814) = arena;                                         /* record base */
    *(uint16_t *)(rec + 0x14) |= 0x2000u;
    uint32_t list = arena + 0x36;                                 /* index-list write cursor (edi) */
    uint32_t cvp  = arena + ebx_off;                              /* clipped-vtx write ptr (ebx) */

    G32(VA_g_sprite_node_pool + 0x818) = 0xdeadbeefu;                                   /* saved-esp sentinel (oracle-masked) */
    G32(VA_g_sprite_render_queue_head + 0x4) = *(uint32_t *)(src + 0x30);                     /* source vtx base */
    G16(VA_g_sprite_render_queue_head + 0xa) = 0;                                             /* output vtx count */
    G32(VA_g_sprite_node_pool + 0x820) = 0xffffffffu;                                   /* prev-inside vtx ptr */
    G32(VA_g_sprite_node_pool + 0x824) = 0xffffffffu;                                   /* prev-outside vtx ptr */
    G32(VA_g_sprite_node_pool + 0x81c) = cvp;                                           /* clipped-vtx region base */
    uint32_t recbase = arena;

    for (uint32_t ecx = n, k = 0; ecx > 0; ecx--, k++) {
        uint32_t idx = *(uint16_t *)(src + 0x36 + 2u * k);        /* popped src index (forward) */
        uint32_t vtx = (uint32_t)G32(VA_g_sprite_render_queue_head + 0x4) + idx;
        int32_t  edx = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)(vtx + 0xa) - 0x10;
        if (edx >= 0) {                                           /* INSIDE (near plane) */
            if ((uint32_t)G32(VA_g_sprite_node_pool + 0x824) != 0xffffffffu) {         /* out->in transition: emit crossing */
                G16(VA_g_sprite_render_queue_head + 0xa)++;
                *(uint16_t *)(uintptr_t)list = (uint16_t)(cvp - recbase); list += 2;
                uint32_t prev_out = (uint32_t)G32(VA_g_sprite_node_pool + 0x824);
                int16_t t = (int16_t)((int16_t)*(uint16_t *)(uintptr_t)(prev_out + 0xa) - 0x10);
                if (interpolate_object_clip_vertex(prev_out, vtx, cvp, t)) return 1;  /* CF -> 0x3cacd */
                *(uint16_t *)(uintptr_t)G32(VA_g_sprite_node_pool + 0x810) = (uint16_t)(G32(VA_g_sprite_node_pool + 0x828) - 1); G32(VA_g_sprite_node_pool + 0x810) += 2;
                cvp += 0x10;
            }
            if (ecx != 1) {                                       /* emit the inside vtx (skip on closing iter) */
                G32(VA_g_sprite_node_pool + 0x824) = 0xffffffffu;
                G16(VA_g_sprite_render_queue_head + 0xa)++;
                *(uint16_t *)(uintptr_t)list = (uint16_t)(cvp - recbase); list += 2;
                G32(VA_g_sprite_node_pool + 0x820) = vtx;
                memcpy((void *)(uintptr_t)cvp, (void *)(uintptr_t)vtx, 0x10);   /* rep movsd 4 */
                *(uint16_t *)(uintptr_t)G32(VA_g_sprite_node_pool + 0x810) = (uint16_t)G32(VA_g_sprite_node_pool + 0x828); G32(VA_g_sprite_node_pool + 0x810) += 2;
                cvp += 0x10;
            }
        } else {                                                 /* OUTSIDE */
            if ((uint32_t)G32(VA_g_sprite_node_pool + 0x820) != 0xffffffffu && (uint32_t)G32(VA_g_sprite_node_pool + 0x824) == 0xffffffffu) {  /* in->out */
                G16(VA_g_sprite_render_queue_head + 0xa)++;
                *(uint16_t *)(uintptr_t)list = (uint16_t)(cvp - recbase); list += 2;
                uint32_t prev_in = (uint32_t)G32(VA_g_sprite_node_pool + 0x820); G32(VA_g_sprite_node_pool + 0x820) = 0xffffffffu;
                if (interpolate_object_clip_vertex(vtx, prev_in, cvp, (int16_t)edx)) return 1;
                *(uint16_t *)(uintptr_t)G32(VA_g_sprite_node_pool + 0x810) = (uint16_t)G32(VA_g_sprite_node_pool + 0x828); G32(VA_g_sprite_node_pool + 0x810) += 2;
                cvp += 0x10;
            }
            G32(VA_g_sprite_node_pool + 0x824) = vtx;                                   /* mark prev-outside */
        }
        G32(VA_g_sprite_node_pool + 0x828)++;                                          /* cnt++ (loop tail) */
    }

    *(uint16_t *)(uintptr_t)list = (uint16_t)(G32(VA_g_sprite_node_pool + 0x81c) - G32(VA_g_sprite_node_pool + 0x814));   /* closing index -> first vtx */
    G32(VA_g_sprite_node_pool + 0x868) = cvp;                                           /* final clipped-vtx ptr */
    uint32_t rec2 = (uint32_t)G32(VA_g_sprite_node_pool + 0x85c);
    *(uint16_t *)(uintptr_t)(rec2 + 0x34) = G16(VA_g_sprite_render_queue_head + 0xa);         /* record vtx count */
    if ((uint8_t)G16(VA_g_sprite_render_queue_head + 0xa) < 2) return 1;                     /* <2 verts -> reject */
    if (thunk_compute_object_screen_bbox(rec2)) return 1; /* bbox/backface cull */
    uint32_t used = (uint32_t)G32(VA_g_sprite_node_pool + 0x868) - (uint32_t)G32(VA_g_sprite_node_pool + 0x85c);
    G32(VA_g_sprite_node_pool + 0x864) -= used;                                         /* shrink arena remaining */
    G32(VA_g_sprite_node_pool + 0x85c) = (uint32_t)G32(VA_g_sprite_node_pool + 0x868);                        /* advance arena cursor */
    uint32_t cnt2 = *(uint8_t *)(uintptr_t)(rec2 + 0x34);
    /* dest = the EDI left by the bbox call (0x3cac5 does `mov edi,[esi+0x30]` = rec2; caller_edi is dead) */
    (void)edi_in;
    memcpy((void *)(uintptr_t)(rec2 + 0x38u + 2u * cnt2),
           (void *)(uintptr_t)(0x8b308u + OBJ_DELTA), 2u * cnt2); /* provenance -> record tail */
    return 0;
}

/* clip_cull_object_to_view (0x3c511): per-object frustum cull/dispatch. Marks the object culled
 * (word[esi+0x14] |= 0x100), tests every vertex's clip coord word[vtx+0xa] against the caller plane BX:
 * all-inside -> compute_object_screen_bbox (0x3c598, tail-call; it owns the bit8/CF), all-outside -> reject,
 * straddle -> clip_object_to_frustum (0x3c892). vcount==0 is the single-vertex fast path (>= 0x10 -> stash
 * [esi+0x10]/[esi+0x12]). Visible exits clear bit8 (word[esi+0x14] &= 0xfeff) + return CF=0; rejects return
 * CF=1. AX(in) is dead (orig stores it to the throwaway stack local). es flat (host pointers). */
/* g_clip_cull_queued_rec models the ESI register at 0x3c511's ret. The original does NOT preserve ESI:
 * the source-visible paths (single-vertex / all-inside compute_object_screen_bbox) keep ESI = the source
 * object, but the STRADDLE path replaces it with the freshly-built CLIPPED record. build_sprite_render_queue
 * enqueues THIS record, so a near-clipped straddler enqueues its clipped polygon rather than the unclipped
 * source (task #111 tree-leaf smear). Modeled as a file global (not an ABI param) so the override/oracle
 * ABI for 0x3c511 stays exactly (ESI, BX); its sole caller reads it immediately, single-threaded. */
uint32_t g_clip_cull_queued_rec = 0;
int clip_cull_object_to_view(uint32_t esi, int16_t bx)
{
    g_clip_cull_queued_rec = esi;                                 /* default: source (unless straddle overrides) */
    uint8_t *rec = (uint8_t *)(uintptr_t)esi;
    *(uint16_t *)(rec + 0x14) |= 0x100u;                          /* mark culled */
    uint32_t vtxb = *(uint32_t *)(rec + 0x30);
    uint32_t vcount = rec[0x34];
    if (vcount == 0) {                                           /* single-vertex path (0x3c573) */
        uint16_t idx = *(uint16_t *)(rec + 0x36);
        int16_t a = *(int16_t *)(uintptr_t)(vtxb + idx + 0xa);
        if (a < 0x10) return 1;                                  /* < near plane -> reject */
        *(uint16_t *)(rec + 0x10) = (uint16_t)a;
        *(uint16_t *)(rec + 0x12) = (uint16_t)a;
        *(uint16_t *)(rec + 0x14) &= 0xfeffu;                    /* visible */
        return 0;
    }
    uint8_t dh = 0x00, dl = 0xff;                                /* mov dx,0xff -> dh=0, dl=0xff */
    uint32_t ip = esi + 0x36, base = vtxb + 0xa;
    for (uint32_t c = vcount; c > 0; c--) {
        int32_t idx = (int16_t)*(uint16_t *)(uintptr_t)ip; ip += 2;     /* lodsw + cwde */
        int16_t a = *(int16_t *)(uintptr_t)((uint32_t)idx + base);      /* word[vtx_base + idx + 0xa] */
        int16_t d = (int16_t)(a - bx);
        uint8_t ah = (uint8_t)((uint16_t)d >> 8);
        dh |= ah; dl &= ah;                                     /* OR/AND of the sign bytes */
    }
    uint16_t dx = (uint16_t)((((uint16_t)dh << 8) | dl) & 0x8080u);
    if (dx == 0)      return compute_object_screen_bbox(esi);  /* all inside -> bbox */
    if (dx == 0x8080) return 1;                                 /* all outside -> reject */
    /* straddle -> clip. NOTE 0x3c892 clobbers ESI (it isn't preserved), leaving ESI = the clipped record base
     * ([0x8b32c] before the clip advanced it); so the visible-exit `and [esi+0x14],0xfeff` clears bit8 of the
     * CLIPPED record, NOT the source. */
    uint32_t clipped_rec = (uint32_t)G32(VA_g_sprite_node_pool + 0x85c);
    if (clip_object_to_frustum(esi, vtxb)) return 1;     /* CF=1 -> reject */
    *(uint16_t *)(uintptr_t)(clipped_rec + 0x14) &= 0xfeffu;    /* visible (esi=clipped rec at 0x3c7d7) */
    g_clip_cull_queued_rec = clipped_rec;                      /* ESI = clipped record at the ret -> enqueue it */
    return 0;
}

/* build_sprite_render_queue (0x3c4a1): walk the object list (head [0x8b2fc], link at +8), clip-cull each via
 * clip_cull_object_to_view (0x3c511, near plane BX=0x10); each survivor gets compute_object_y_center (0x3c843,
 * sets the depth key [obj+0x12]) and is appended to the render queue (link at +0, tail ptr [0x8b304], count
 * [0x8b328]). The arena cursor/room [0x8b32c]/[0x8b334] are saved across the build (clip scratch) + restored.
 * No GP-register inputs (all state via globals). */
void build_sprite_render_queue(void)
{
    G32(VA_g_sprite_node_pool + 0x860) = G32(VA_g_sprite_node_pool + 0x85c);                                /* save arena cursor */
    G32(VA_g_sprite_node_pool + 0x86c) = G32(VA_g_sprite_node_pool + 0x864);                                /* save arena room */
    uint32_t esi = (uint32_t)G32(VA_g_sprite_node_pool + 0x82c);
    while (esi != 0) {
        /* clip_cull may CLOBBER esi (like the original 0x3c511 which does not preserve it): for a
         * near-plane STRADDLING object it leaves ESI = the freshly-built CLIPPED record (arena base),
         * so the original enqueues THAT (near-clipped) record — not the unclipped source. The C returns
         * that record via q_rec; the list walk still advances from the SOURCE object's +8 link. Missing
         * this made straddling sprites (e.g. the near tree leaf cards) enqueue the unclipped polygon,
         * which the rasterizer drew as a diagonal smear across the sky (task #111). */
        if (!clip_cull_object_to_view(esi, 0x10)) {            /* CF=0 -> survives the cull */
            uint32_t q_rec = g_clip_cull_queued_rec;           /* = ESI after clip_cull (clipped rec on straddle) */
            compute_object_y_center(q_rec);
            *(uint32_t *)(uintptr_t)q_rec = 0;                 /* clear the queue link (+0) */
            uint32_t tail = (uint32_t)G32(VA_g_sprite_node_pool + 0x834);
            *(uint32_t *)(uintptr_t)tail = q_rec;              /* [tail] = obj */
            G32(VA_g_sprite_node_pool + 0x834) = q_rec;                                /* tail = obj */
            G32(VA_g_sprite_node_pool + 0x858)++;                                    /* count++ */
        }
        esi = *(uint32_t *)(uintptr_t)(esi + 8);               /* next object (list link +8, from SOURCE) */
    }
    G32(VA_g_sprite_node_pool + 0x864) = G32(VA_g_sprite_node_pool + 0x86c);                                /* restore arena room/cursor */
    G32(VA_g_sprite_node_pool + 0x85c) = G32(VA_g_sprite_node_pool + 0x860);
}

/* finalize_sprite_render_queue (0x3c477): seed the object-list head ([0x8b2fc]/[0x8b300] = ESI), reset the
 * queue count, build_sprite_render_queue (0x3c4a1) then depth_sort_sprite_queue (0x3c7e7); returns the sorted
 * queue head [0x8b340]. (orig sets ES=DS — a no-op in the flat lift.) */
uint32_t finalize_sprite_render_queue(uint32_t esi)
{
    G32(VA_g_sprite_node_pool + 0x82c) = esi;
    G32(VA_g_sprite_node_pool + 0x830) = esi;
    G32(VA_g_sprite_node_pool + 0x858) = 0;
    build_sprite_render_queue();
    depth_sort_sprite_queue();
    return (uint32_t)G32(VA_g_sprite_render_queue_head);
}

/* add_reflection_view_entry (0x283d9): append (or dedup) a reflection-view entry in the array at [0x85340]
 * (dword count, then 8-byte entries at 0x85344: +0 key, +4 = fs:[viewid+6], +6 = viewid [0x853cc]). Full
 * (count>=0x10) or gated ([0x90a26]!=0) or a non-zero key ([0x84950]!=0 with no dup) -> CF=1, no change. A
 * matching {key,viewid} entry already present -> CF=0, no add. Else (key [0x84950]==0, gate clear) append +
 * inc count, CF=0. pushal/popal: only CF + memory matter; the input EDX is dead. Pure leaf (no calls); fs via
 * the passed base (technique i). */
int add_reflection_view_entry(uint32_t fs_base)
{
    if ((uint32_t)G32(VA_g_reflection_view_count) >= 0x10) return 1;                /* array full -> stc (early, no pushal) */
    uint16_t viewid = (uint16_t)G16(VA_g_reflection_view_list + 0x88);
    uint32_t edx = (uint32_t)G32(VA_g_current_decoded_frame + 0xc);                       /* search key */
    uint32_t edi = 0x85344;                                      /* first entry ([0x85340]+4) */
    uint32_t ecx = (uint32_t)G32(VA_g_reflection_view_count);                       /* count */
    while (ecx != 0) {
        if ((uint32_t)G32(edi) == edx && (uint16_t)G16(edi + 6) == viewid) return 0;  /* dup -> clc */
        edi += 8; ecx--;
    }
    if (G8(VA_g_sprite_fill_index + 0x2) != 0) return 1;                              /* gated -> skip stc */
    uint32_t eax = (uint32_t)G32(VA_g_current_decoded_frame + 0xc);
    if (eax != 0) return 1;                                      /* non-zero key, no dup -> skip stc */
    G16(edi + 6) = viewid;                                       /* append entry */
    G32(edi)     = eax;                                          /* key = 0 */
    G16(edi + 4) = *(uint16_t *)(uintptr_t)(fs_base + viewid + 6);  /* fs:[viewid+6] */
    G32(VA_g_reflection_view_count)++;                                              /* inc count */
    return 0;                                                    /* clc */
}

/* reflect_view_across_mirror_plane (0x28456): reflect the view position across a mirror edge. Toggles the
 * mirror-side flag [0x8a356], reads the edge's two endpoint indices from fs:[ebx]/fs:[ebx+2], looks up their
 * gs-segment coords (+8 = X, +0xa = Y); whichever axis the edge spans more (|dX| vs |dY|) picks the reflection
 * axis: span-X (>=) flips the view Y [0x90a06] across the edge Y + sets [0x909f8]=-[0x909f8]+0x100; span-Y
 * flips the view X [0x90a04] across the edge X + sets [0x909f8]=-[0x909f8]. Pure leaf (no calls). The selector
 * loads (fs/gs from [0x84ac4]+0x10/+0x14) are modeled by the passed bases (technique i). */
void reflect_view_across_mirror_plane(uint32_t ebx, uint32_t fs_base, uint32_t gs_base)
{
    G8(VA_g_render_x_flip_flag) ^= 1u;
    uint32_t di = *(uint16_t *)(uintptr_t)(fs_base + (uint16_t)ebx);
    uint32_t si = *(uint16_t *)(uintptr_t)(fs_base + (uint16_t)(ebx + 2));
    int16_t ax = (int16_t)(*(uint16_t *)(uintptr_t)(gs_base + di + 8)
                         - *(uint16_t *)(uintptr_t)(gs_base + si + 8));
    if (ax < 0) ax = (int16_t)(0 - (uint16_t)ax);                 /* 16-bit abs */
    int16_t dx = (int16_t)(*(uint16_t *)(uintptr_t)(gs_base + di + 0xa)
                         - *(uint16_t *)(uintptr_t)(gs_base + si + 0xa));
    if (dx < 0) dx = (int16_t)(0 - (uint16_t)dx);
    if (ax >= dx) {                                               /* edge spans X more -> flip view Y */
        G16(VA_g_sprite_view_angle) = (uint16_t)(0 - (uint16_t)G16(VA_g_sprite_view_angle));
        G16(VA_g_sprite_view_angle) = (uint16_t)(G16(VA_g_sprite_view_angle) + 0x100);
        uint16_t t = (uint16_t)(G16(VA_g_view_offset_y) + *(uint16_t *)(uintptr_t)(gs_base + di + 0xa));
        t = (uint16_t)(t + t);
        G16(VA_g_view_offset_y) = (uint16_t)(G16(VA_g_view_offset_y) - t);
    } else {                                                      /* edge spans Y more -> flip view X */
        G16(VA_g_sprite_view_angle) = (uint16_t)(0 - (uint16_t)G16(VA_g_sprite_view_angle));
        uint16_t t = (uint16_t)(G16(VA_g_view_offset_x) + *(uint16_t *)(uintptr_t)(gs_base + di + 8));
        t = (uint16_t)(t + t);
        G16(VA_g_view_offset_x) = (uint16_t)(G16(VA_g_view_offset_x) - t);
    }
}

/* resolve_face_surface_id (0x4f0ab): resolve a face's surface/texture id from its flags. EBX=face record,
 * ESI=base, EAX=offset; returns the id in EBX. If the cache byte [rec+0x21]!=0, validate the cached id
 * ([base+off+0x44] vs the primary [base+off+0x14]) and clear the cache flag on a match. Otherwise pick a
 * field by the flag byte [rec+8] (priority 0x20 > 0x40 > 8 > 0x80 > ==5), each with a bit0 sub-select and a
 * zero-fallback to either the default lookup (+0x14) or the dispatch-default (+0x20000 -> +4 else +0x14).
 * Pure leaf (no calls); only side effect is the optional [rec+0x21]=0. */
uint32_t resolve_face_surface_id(uint32_t rec, uint32_t base, uint32_t off)
{
    #define R8(o)  (*(volatile uint8_t  *)(uintptr_t)(rec + (uint32_t)(o)))
    #define R32(o) (*(volatile uint32_t *)(uintptr_t)(rec + (uint32_t)(o)))
    #define B16(o) ((uint32_t)*(volatile uint16_t *)(uintptr_t)(base + off + (uint32_t)(o)))  /* [esi+eax+o] */
    #define S16(o) ((uint32_t)*(volatile uint16_t *)(uintptr_t)(base + (uint32_t)(o)))         /* [esi+o]     */
    if (R8(0x21) != 0) {                              /* 0x4f0ab: cached id -> validate */
        uint32_t edx = B16(0x14);                    /* primary */
        uint32_t v   = B16(0x44);
        if (v == 0 || v == edx) { R8(0x21) = 0; return edx; }   /* 0x4f114 */
        return v;                                                /* 0x4f11d */
    }
    uint32_t flags = R32(8);                          /* mov ebx,[rec+8] */
    uint8_t  bl = (uint8_t)flags;
    if (bl & 0xfc) {                                  /* 0x4f0b4 test bl,0xfc */
        if (bl & 0x20) {                              /* 0x4f17f */
            if (bl & 1) { uint32_t v = S16(0x58); if (v) return v; }   /* 0x4f194 -> fall to +0x54 */
            uint32_t v = S16(0x54); if (v) return v;
            goto default_lookup;
        }
        if (bl & 0x40) {                              /* 0x4f14d */
            uint32_t v = (bl & 1) ? S16(0x5e) : S16(0x5c);
            if (v) return v;
            goto dispatch_default;
        }
        if (bl & 8) {                                 /* 0x4f12e */
            if (bl & 1) { uint32_t v = B16(0x34); if (v) return v; }   /* 0x4f140 -> fall to +0x24 */
            uint32_t v = B16(0x24); if (v) return v;
            goto default_lookup;
        }
        if (bl & 0x80) {                              /* 0x4f16e */
            uint32_t v = B16(0x44); if (v) return v;
            goto default_lookup;
        }
        if (bl == 5) {                                /* 0x4f122 */
            uint32_t v = S16(0x62); if (v) return v;
            goto default_lookup;
        }
    }
dispatch_default:                                     /* 0x4f0de */
    if (flags & 0x20000) { uint32_t v = B16(4); if (v) return v; }   /* 0x4f0ec */
default_lookup:                                       /* 0x4f0e6 */
    return B16(0x14);
    #undef R8
    #undef R32
    #undef B16
    #undef S16
}

/* depth_sort_sprite_queue (0x3c7e7): bubble-sort the singly-linked sprite queue (head ptr at [0x8b340],
 * node count [0x8b328]) by the 16-bit depth field [+0x12], descending — swapping when curr.depth <
 * next.depth. Re-links in place via the node `next` pointers [+0]; tracks per-pass `swapped` at [0x8b358]
 * for early-out (loopne). Faithful transcription of the two inner entry points (0x3c809 advance / 0x3c80f
 * re-compare after a swap). Pure leaf — operates entirely on the obj3 queue + nodes. */
void depth_sort_sprite_queue(void)
{
    #define HP32(p) (*(volatile uint32_t *)(uintptr_t)(p))
    #define HP16(p) (*(volatile uint16_t *)(uintptr_t)(p))
    int32_t  ecx = (int32_t)G32(VA_g_sprite_node_pool + 0x858);                   /* count */
    if (ecx <= 1) return;                                   /* cmp ecx,1; jle ret */
    uint32_t edi = 0x8b340u + OBJ_DELTA;                    /* &head */
    uint32_t esi, ebx = 0, eax, edi_saved = 0;
    int32_t  ecx_outer;
    uint16_t ax;
outer:                                                      /* 0x3c7fb */
    esi = HP32(edi);                                        /* esi = head */
    ecx_outer = ecx;                                        /* push ecx */
    ecx = ecx - 1;                                          /* dec ecx */
    if (ecx <= 0) goto pass_end_noedi;                      /* jle 0x3c835 (no edi push, no flag reset) */
    edi_saved = edi;                                        /* push edi */
    G8(VA_g_sprite_render_queue_head + 0x18) = 0;                                        /* swapped = 0 */
setup:                                                      /* 0x3c809 (advance window) */
    ebx = edi;
    edi = esi;
    esi = HP32(esi);
compare:                                                    /* 0x3c80f */
    ax = HP16(edi + 0x12);
    if ((int16_t)ax < (int16_t)HP16(esi + 0x12)) {          /* curr < next -> swap (0x3c81e) */
        eax = HP32(esi);                                    /* next.next */
        HP32(edi) = eax;                                    /* curr.next = next.next */
        HP32(esi) = edi;                                    /* next.next = curr */
        HP32(ebx) = esi;                                    /* prev.next = next */
        ebx = esi;                                          /* prev = next */
        esi = eax;                                          /* next = next.next */
        G8(VA_g_sprite_render_queue_head + 0x18) = 0xff;                                 /* swapped = 1 */
        ecx--;
        if (ecx > 0) goto compare;                          /* jg 0x3c80f (re-compare curr vs new next) */
        goto pass_end;
    } else {
        ecx--;
        if (ecx > 0) goto setup;                            /* jg 0x3c809 (advance) */
        goto pass_end;                                      /* jmp 0x3c834 */
    }
pass_end:                                                   /* 0x3c834 */
    edi = edi_saved;                                        /* pop edi */
pass_end_noedi:                                             /* 0x3c835 */
    ecx = ecx_outer;                                        /* pop ecx */
    {
        int swapped = (G8(VA_g_sprite_render_queue_head + 0x18) != 0);                   /* test [0x8b358]; ZF = !swapped */
        ecx--;                                              /* loopne: dec ecx */
        if (ecx != 0 && swapped) goto outer;                /* ... jump if ecx!=0 && ZF==0 */
    }
    #undef HP32
    #undef HP16
}

/* compute_object_y_center (0x3c843): over an object's vertex ring, find min & max of the per-vertex Y
 * field (mem[ [esi+0x30] + 0xa + index ], a flat DS read) across [esi+0x34] vertices, then store the
 * midpoint (min+max)/2 (16-bit add + arithmetic sar) into [esi+0x10] AND [esi+0x12]. esi = object record
 * (+0x30 vtx base, +0x34 count, +0x36 index list; idx0 is read once to seed min/max then the loop runs
 * `count` times re-reading idx0 first). Pure leaf, no calls; all 16-bit. */
void compute_object_y_center(uint32_t esi)
{
    #define O8(o)  (*(volatile uint8_t  *)(uintptr_t)(esi + (uint32_t)(o)))
    #define O16(o) (*(volatile uint16_t *)(uintptr_t)(esi + (uint32_t)(o)))
    #define O32(o) (*(volatile uint32_t *)(uintptr_t)(esi + (uint32_t)(o)))
    #define Y16(p) (*(volatile uint16_t *)(uintptr_t)(p))
    uint32_t count = O8(0x34);                          /* movzx ecx, byte[esi+0x34] */
    if (count == 0) return;                             /* or ecx,ecx; je ret */
    O32(0x10) = 0;                                      /* mov [esi+0x10],0 (clears 0x10 & 0x12) */
    uint32_t edi  = O32(0x30) + 0xa;                    /* vtx base + 0xa (the Y field) */
    uint32_t list = esi + 0x36;                         /* index list */
    uint16_t idx0 = Y16(list);                          /* movzx ebx, word[esi] (no advance) */
    int16_t dmin = (int16_t)Y16(edi + idx0);            /* dx = Y0 */
    int16_t dmax = dmin;                                /* ebp = dx */
    for (uint32_t i = 0; i < count; i++) {              /* dec ecx; jg -> `count` iterations */
        uint16_t idx = Y16(list); list += 2;            /* lodsw (1st iter re-reads idx0) */
        int16_t y = (int16_t)Y16(edi + idx);            /* bx = Y */
        if (dmin >= y) dmin = y;                        /* cmp dx,bx; jl skip; mov dx,bx (min) */
        if (dmax <= y) dmax = y;                        /* cmp bp,bx; jg skip; mov bp,bx (max) */
    }
    int16_t center = (int16_t)((int16_t)((uint16_t)dmin + (uint16_t)dmax) >> 1);  /* add dx,bp; sar dx,1 */
    O16(0x12) = (uint16_t)center;
    O16(0x10) = (uint16_t)center;
    #undef O8
    #undef O16
    #undef O32
    #undef Y16
}

/* Oracle test hook: exercise one floor/ceil fill inner-loop (fc_dispatch -> fc_*) with a caller-built
 * context, so test_render_world.c can verify each fill VA against call_orig in isolation. Not used by
 * the production driver (which builds its own cx); exists only for static verification. */
void floorceil_fill_dispatch(uint32_t key, uint32_t edi_cur, int32_t count, uint8_t ah,
                                    uint32_t gs_base, uint32_t fs_base, uint32_t blend_base,
                                    uint32_t cmap_flat, uint32_t es_fb_base, uint32_t flat_fb,
                                    uint8_t ror_imm, uint8_t dh_mask, uint32_t ebx_mask)
{
    fc_ctx_t cx;
    cx.gs_base = gs_base; cx.fs_base = fs_base; cx.blend_base = blend_base; cx.cmap_flat = cmap_flat;
    cx.es_fb_base = es_fb_base; cx.flat_fb = flat_fb;
    cx.ror_imm = ror_imm; cx.dh_mask = dh_mask; cx.ebx_mask = ebx_mask;
    fc_dispatch(key, edi_cur, count, ah, &cx);
}

void draw_floorceil_surface(uint32_t esi, uint32_t gs_base, uint32_t fs_base,
                                   uint32_t es_fb_base, uint32_t blend_base,
                                   uint16_t es_sel, uint16_t fs_sel)
{
#define HP16(p) (*(volatile uint16_t *)(uintptr_t)(p))
#define HP32(p) (*(volatile uint32_t *)(uintptr_t)(p))
    /* ---- setup 0x3a84e ---- */
    G16(VA_g_render_textured_flag) = 0;   /* 0x3a855 is a WORD store -> clears BOTH a22 AND a23 (a23 is stateful otherwise) */
    if (G8(VA_g_span_blend_mode_flag) == 0) {
        if (G8(VA_g_column_clip_mode) != 0 || (G16(VA_g_world_surface_draw_flags) & 0x8000) || (G16(VA_g_world_alt_render_flags) & 0x8000))
            G8(VA_g_render_textured_flag) = 0xff;
    }
    if (G16(VA_g_world_surface_draw_flags) & 8) G8(VA_g_render_textured_flag + 0x1) = 0xff;   /* textured -> fs=[0x909b0] (== fs_base, resolved by host) */

    /* ---- optional per-poly shade 0x3a8a6 (gated [0x90cc8]==1 && [0x8a354]==0) — bridge 0x3c0be ---- */
    if (G8(VA_g_flat_shading_flag + 0x4) == 1 && G8(VA_g_span_blend_mode_flag) == 0) {
        uint32_t frame[14];                                   /* the pushed 0x38-byte arg frame */
        frame[0] = frame[1] = frame[2] = 0;
        uint32_t geom = HP32(esi + 0x30);
        for (int v = 0; v < 3; v++) {
            uint16_t idx = HP16(esi + 0x36 + (uint32_t)v * 2);
            uint32_t vp  = geom + idx;
            frame[11 - v * 3] = (uint32_t)(int32_t)(int16_t)HP16(vp + 6);
            frame[10 - v * 3] = (uint32_t)(int32_t)(int16_t)HP16(vp + 8);
            frame[9  - v * 3] = (uint32_t)(int32_t)(int16_t)HP16(vp + 0xa);
        }
        frame[12] = 0x20; frame[13] = 0xffffffffu;
        /* re-point 0x3c0be compute_surface_normal_shade: pure-math leaf, reads its arg frame via EBP,
         * returns the shade in EAX; no segment use, so es/fs/gs are dropped. */
        uint8_t al = (uint8_t)compute_surface_normal_shade((uint32_t)(uintptr_t)frame);
        uint8_t t  = (uint8_t)((uint8_t)(0u - al) + 0x1fu);   /* neg al; add al,0x1f */
        if (t & 0x80u) t = 0;                                 /* jns; else sub al,al */
        if (t >= 0x1f)  t = 0x1f;                             /* cmp al,0x1f; jb; else mov al,0x1f */
        uint32_t bc = (uint32_t)t * 0x01010101u;              /* broadcast to all 4 bytes */
        HP32(esi + 0x18) = (HP32(esi + 0x18) & 0xe0e0e0e0u) | bc;
        G8(VA_g_render_textured_flag) = 0xff; G16(VA_g_world_surface_draw_flags) |= 0x8000;
    }

    /* ---- edge-walk 0x3a93a: re-point rasterize_floorceil_polygon (0x3b1c1), return on empty ---- */
    /* pure-DS lifted body (selectors ignored); returns 1 == empty run-list (== original ZF set). */
    if (rasterize_floorceil_polygon(esi, (uint16_t)G16(VA_g_active_world_remap_selector), es_sel, fs_sel)) return;

    /* ---- render-mode matrix 0x3a945: select main(ebx_fn)/alt(ecx_fn) fill keys ---- */
    {
        int8_t al = (int8_t)(uint8_t)G8(VA_g_sprite_render_mode);
        if (al <= 1) G8(VA_g_render_textured_flag) = 0;
        else if ((uint8_t)al == 2) { G8(VA_g_sprite_fill_index) = (uint8_t)G16(VA_g_das_palette_remap_prefix); G8(VA_g_render_textured_flag + 0x1) = 0; G8(VA_g_render_textured_flag) = 0; }
    }
    fc_ctx_t cx; memset(&cx, 0, sizeof cx);
    cx.gs_base = gs_base; cx.fs_base = fs_base; cx.blend_base = blend_base;
    cx.es_fb_base = es_fb_base; cx.flat_fb = (uint32_t)G32(VA_g_render_target_buffer); cx.cmap_flat = (uint32_t)G32(VA_g_active_world_remap_base);
    uint32_t ebx_fn, ecx_fn;
    if (G8(VA_g_render_textured_flag + 0x1) == 0) {                                   /* untextured */
        ecx_fn = 0x3ad04; ebx_fn = 0x3acec;
        if (G8(VA_g_render_textured_flag) != 0) ebx_fn = 0x3ad28;
    } else if ((uint8_t)(G8(VA_g_span_src_row_width) | G8(VA_g_span_src_wrap_reoffset + 0xc)) == 0) {   /* degenerate (low bytes both 0) */
        ecx_fn = 0x3b110; ebx_fn = 0x3b0d8;
        if (G8(VA_g_render_textured_flag) != 0) ebx_fn = 0x3b15c;
    } else {                                                  /* textured: compute SMC mask + matrix */
        uint16_t width = (uint16_t)G16(VA_g_span_src_row_width);
        int b = width ? (31 - __builtin_clz(width)) : 0;      /* bsr16 */
        cx.ror_imm  = (uint8_t)(0x10 - b);
        cx.ebx_mask = (uint32_t)(uint16_t)((uint16_t)G16(VA_g_column_clip_mode + 0x4) << b);
        cx.dh_mask  = (uint8_t)G16(VA_g_span_src_wrap_reoffset);
        if (G16(VA_g_world_surface_draw_flags) & 1) {
            if (G16(VA_g_floor_tex_caps) & 0x400) { ecx_fn = 0x3ae68; ebx_fn = 0x3ae04; if (G8(VA_g_render_textured_flag) != 0) ebx_fn = 0x3aff4; }
            else                      { ecx_fn = 0x3aed8; ebx_fn = 0x3adc0; if (G8(VA_g_render_textured_flag) != 0) ebx_fn = 0x3b070; }
        } else                        { ecx_fn = 0x3af38; ebx_fn = 0x3ad80; if (G8(VA_g_render_textured_flag) != 0) ebx_fn = 0x3af94; }
    }
    if (G8(VA_g_span_blend_mode_flag) != 0) ebx_fn = ecx_fn;                    /* force the alt */
    G32(VA_g_floorceil_span_fn) = (int32_t)(ebx_fn + OBJ_DELTA);             /* store RELOCATED code ptrs (review #7) */
    G32(VA_g_floorceil_span_fn_alt) = (int32_t)(ecx_fn + OBJ_DELTA);

    /* ---- row-loop preamble 0x3ab11 ---- */
    G32(VA_g_floorceil_accum_a) = 0; G32(VA_g_floorceil_accum_b) = 0; G32(VA_g_floorceil_accum_b + 0x4) = 0;
    uint32_t minY = (uint16_t)G16(VA_g_view_offset_y + 0x14), maxY = (uint16_t)G16(VA_g_view_offset_y + 0x16);
    int32_t  rc   = (int32_t)maxY - (int32_t)minY;
    uint32_t edi  = (uint32_t)G32(VA_g_scanline_dest_offset_table + minY * 4);        /* scanline base-offset table @ minY */
    uint32_t rec_canon = 0x8cd0c + minY * 0x18;               /* per-Y span record array */
    const int32_t fcrow = (int32_t)*(volatile uint32_t *)(uintptr_t)(0x3ac9bu + OBJ_DELTA);  /* SMC row stride */

    /* ---- per-row loop 0x3ab50 ---- */
    do {
        uint8_t *rp = (uint8_t *)(uintptr_t)(rec_canon + OBJ_DELTA);
        uint32_t xL = *(uint16_t *)(rp);                      /* zero-extended (sub eax,eax; mov ax,[ebx]) */
        uint32_t xR = *(uint16_t *)(rp + 0xc);
        int32_t pix = (int32_t)xR - (int32_t)xL;
        if (pix < 0) { xL = *(uint16_t *)(rp + 0xc); pix = -pix; }
        pix += 1;                                             /* pixel count = |xR-xL| + 1 */
        uint32_t edi_cur;
        if (G8(VA_g_render_x_flip_flag) != 0) {                               /* x-flip 0x3acaa */
            edi_cur = edi + (uint32_t)G32(VA_g_current_decoded_frame + 0x10) - (uint32_t)pix + 1u - (uint32_t)xL;
            uint16_t a, old;
            a = *(uint16_t *)(rp + 0xe); old = *(uint16_t *)(rp + 2);  *(uint16_t *)(rp + 2) = a;  *(uint16_t *)(rp + 0xe) = old;
            if (G8(VA_g_render_textured_flag) != 0) {
                a = *(uint16_t *)(rp + 0x12); old = *(uint16_t *)(rp + 6);  *(uint16_t *)(rp + 6) = a;  *(uint16_t *)(rp + 0x12) = old;
                a = *(uint16_t *)(rp + 0x10); old = *(uint16_t *)(rp + 4);  *(uint16_t *)(rp + 4) = a;  *(uint16_t *)(rp + 0x10) = old;
            }
        } else {
            edi_cur = edi + (uint32_t)xL;                     /* add edi, eax */
        }
        /* 0x3ab78: textured perspective steppers (EXACT overlapping stores; fills read packed back) */
        if (G8(VA_g_render_textured_flag + 0x1) != 0) {
            int16_t v6 = (int16_t)*(uint16_t *)(rp + 6);
            G16(VA_g_floorceil_accum_a + 0x1) = (uint16_t)v6;  G16(VA_g_floorceil_accum_a + 0x1) = (uint16_t)(G16(VA_g_floorceil_accum_a + 0x1) << 2);
            int32_t d = (int32_t)(int16_t)*(uint16_t *)(rp + 0x12) - (int32_t)v6;
            uint32_t vstep = (uint32_t)d;
            if (d != 0) { int32_t hi = (d < 0) ? -1 : 0; int64_t num = ((int64_t)(uint32_t)hi << 32) | (uint32_t)(int32_t)((uint32_t)d << 10);
                          vstep = (uint32_t)(int32_t)(num / (int64_t)pix); }
            G32(VA_g_floorceil_step_a) = (int32_t)vstep;
            int16_t u4 = (int16_t)*(uint16_t *)(rp + 4);
            G16(VA_g_floorceil_accum_b) = (uint16_t)u4;  G16(VA_g_floorceil_accum_b) = (uint16_t)(G16(VA_g_floorceil_accum_b) << 2);
            int32_t d2 = (int32_t)(int16_t)*(uint16_t *)(rp + 0x10) - (int32_t)u4;
            uint32_t ustep = (uint32_t)d2;
            if (d2 != 0) { int32_t hi = (d2 < 0) ? -1 : 0; int64_t num = ((int64_t)(uint32_t)hi << 32) | (uint32_t)(int32_t)((uint32_t)d2 << 10);
                           ustep = (uint32_t)(int32_t)(num / (int64_t)pix); }
            G32(VA_g_floorceil_step_a + 0x3) = (int32_t)ustep;
        }
        /* 0x3abcd: select the fill + its entry AH, or take the deferred-surface tail */
        uint8_t  fill_ah = 0;
        uint32_t fill_key = ebx_fn;
        int      do_deferred = 0, do_fill = 1;
        if (G8(VA_g_span_blend_mode_flag) != 0) {                               /* a354 -> alt fill, ah=1 */
            fill_ah = 1; fill_key = ecx_fn;
        } else {
            int go_main = 1;
            if (G8(VA_g_render_textured_flag) != 0) {                           /* 0x3abde: 2nd-derivative (W) stepper */
                uint32_t ee = *(uint16_t *)(rp + 0xe), e2 = *(uint16_t *)(rp + 2);
                uint8_t ah = (uint8_t)(ee >> 8), dh = (uint8_t)(e2 >> 8);
                if (ah == dh) { fill_ah = ah; fill_key = ecx_fn; go_main = 0; }   /* jmp [0x8a3c4] */
                else {
                    G16(VA_g_floorceil_accum_b + 0x3) = (uint16_t)e2;
                    int32_t dd = (int32_t)ee - (int32_t)e2; int32_t hi = (dd < 0) ? -1 : 0;
                    int64_t num = ((int64_t)(uint32_t)hi << 32) | (uint32_t)(int32_t)((uint32_t)dd << 8);
                    G32(VA_g_floorceil_step_b + 0x2) = (int32_t)(uint32_t)(int32_t)(num / (int64_t)pix);
                }
            }
            if (go_main) {
                if (G8(VA_g_world_render_subpass_kind) == 0) { fill_ah = 0; fill_key = ebx_fn; }         /* jmp [0x8a3c0] */
                else { do_deferred = 1; do_fill = 0; }
            }
        }
        if (do_deferred) {                                    /* deferred-surface tail 0x3ac1c -> ret */
            G8(VA_g_world_render_subpass_kind + 0x1)  = (uint8_t)G8(VA_g_world_render_subpass_kind);
            G32(VA_g_subpass_reflect_param_b + 0x2) = (int32_t)G32(VA_g_current_proc_tag + 0x118);
            G16(VA_g_subpass_surfrec_ref + 0x4) = (uint16_t)G16(VA_g_world_surface_draw_flags);
            G16(VA_g_world_render_subpass_kind + 0x4) = (uint16_t)(uint8_t)G8(VA_g_floorceil_accum_a + 0x2);
            G16(VA_g_world_render_subpass_kind + 0x6) = (uint16_t)(uint8_t)G8(VA_g_floorceil_accum_b + 0x1);
            G32(VA_g_subpass_surfrec_ref) = (int32_t)G32(VA_g_map_das_fat_buffer + 0x8);
            G32(VA_g_subpass_surfrec_ref + 0x6) = (int32_t)G32(VA_g_map_das_fat_buffer + 0xc);
            G16(VA_g_world_render_subpass_kind + 0x2) = (uint16_t)G32(VA_g_current_das_entry_id);
            G8(VA_g_subpass_reflect_param_b + 0x6)  = (uint8_t)G8(VA_g_turn_view_scale_state + 0x2);
            G32(VA_g_subpass_persp_step) = (int32_t)(((uint32_t)G32(VA_g_sprite_span_shade + 0x2) & 0xffffu) * 2u);
            return;
        }
        if (do_fill
#ifdef ROTH_STANDALONE
            /* twin of the draw_scaled_sprite_spans guard: rows at Y >= live screen height read
             * past the valid dest-offset rows (base read @minY, then fcrow strides). Y here =
             * minY + rows_done; skip the FILL only, keep the row walk byte-identical. */
            && (minY + (uint32_t)((rec_canon - (0x8cd0cu + minY * 0x18u)) / 0x18u))
               < (uint32_t)G32(VA_g_screen_pitch + 0x4)
#endif
            ) fc_dispatch(fill_key, edi_cur, pix, fill_ah, &cx);
        /* row tail 0x3ac96 */
        edi += (uint32_t)fcrow;
        rec_canon += 0x18;
    } while (--rc >= 0);
#undef HP16
#undef HP32
}

/* ===================== rasterize_world_spans_scanline (0x366cb) — THE LAST span-rasterizer bridge =====================
 * The SMC scanline DISPATCHER. ESI = span record (host ptr). (1) load flags, (2) RESOLVE the texture source
 * (id -> descriptor 0x86d30 -> DAS cache slot 0x89930, bridging the cache/anim callees), (3) set the texture
 * dim/source/translucency globals, (4) 3-way dispatch to the now-native drivers (sprite 0x39610 / floorceil 0x3a84e /
 * wall body 0x36b68 via classify 0x38b54). Transcribed register-faithfully (the resolution loop has width/clobber
 * effects: e.g. [0x8a2a4]=eax writes 4 bytes after only ax was set). BRIDGES (call_orig): cache-load 0x40d7c,
 * relocate 0x41250, anim 0x38fec, reflection 0x283d9, callback 0x39093, the [0x8a2a0]->0x33dde indirect resolver.
 * int3s @0x36845/0x36981 are NOPs in this build. Open Qs flagged FIXME. */

/* bridge one cache/IO/anim callee; returns its output EAX (for the callees whose EAX clobber is read downstream:
 * 0x41250 feeds `test ah,1`; 0x39093/indirect feed eax-high into [0x8a2a4]). cf_out = output carry. */
__attribute__((unused))   /* imgfree lane: every rwss bridge site is routed native now (hook wiring pass) */
static uint32_t rwss_brdg(uint32_t va_host, uint32_t eax, uint32_t ebx, uint32_t esi_w,
                          uint16_t es_sel, uint16_t fs_sel, uint16_t gs_sel, int *cf_out)
{
    regs_t io; memset(&io, 0, sizeof io);
    io.va = va_host; io.eax = eax; io.ebx = ebx; io.esi = esi_w;
    io.es = es_sel; io.fs = fs_sel; io.gs = gs_sel;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    roth_unreachable(va_host - OBJ_DELTA);   /* rasterize-scanline cache/IO/anim bridge — render tier */
#endif
    if (cf_out) *cf_out = (int)(io.eflags & 1u);
    return io.eax;
}


#ifdef ROTH_STANDALONE
/* the 0x366d2 sprite-rasterizer mid-entry. render_world_sprite
 * jmps to 0x366d2 = this rasterizer's body ONE statement past its prologue — the single marker-clear
 * `G8(0x90a26)=0` @0x366cb — which the sprite path must NOT run (the caller pre-set the 0xff marker,
 * consumed by classify/reflection inside the body; that's the ONLY prologue difference, disasm-confirmed).
 * Rather than move the ~500-line CENTRAL rasterizer body (risk), a minimal imgfree-only flag skips just
 * that one clear; the body stays byte-identical (trap lane strip-identical). Bases resolved exactly like
 * rwss_render_span_record (renderer.c:7883). */
static int g_rwss_skip_marker_clear = 0;
volatile unsigned long g_rwss_sprite_body = 0;
void rwss_sprite_body_entry(uint32_t esi_rec, uint16_t es, uint16_t fs, uint16_t gs)
{
    extern uint32_t (*g_os_sel_base)(uint16_t);
    extern uint16_t g_os_game_ds;
    g_rwss_sprite_body++;
    g_rwss_skip_marker_clear = 1;                              /* consumed at the rasterizer's entry below */
    rasterize_world_spans_scanline(
        esi_rec,
        g_os_sel_base((uint16_t)G16(VA_g_active_world_remap_selector)),               /* gs_base (colormap) */
        g_os_sel_base((uint16_t)G16(VA_g_transparency_blend_selector)),               /* blend_base */
        g_os_sel_base((uint16_t)G16(VA_g_world_alt_render_flags + 0x2)),               /* fs_tex_base */
        g_os_sel_base((uint16_t)G16(VA_g_render_target_selector)),               /* es_fb_base */
        es, fs, gs, g_os_game_ds);
}
#endif

void rasterize_world_spans_scanline(uint32_t esi_rec,
        uint32_t gs_base, uint32_t blend_base, uint32_t fs_tex_base, uint32_t es_fb_base,
        uint16_t es_sel, uint16_t fs_sel, uint16_t gs_sel, uint16_t ds_sel)
{
#ifdef ROTH_STANDALONE
    (void)gs_sel; (void)ds_sel;   /* consumed only by the trap-lane rwss_brdg sites (all routed native here) */
#endif
#define HP8(p)  (*(volatile uint8_t  *)(uintptr_t)(p))
#define HP16(p) (*(volatile uint16_t *)(uintptr_t)(p))
#define HP32(p) (*(volatile uint32_t *)(uintptr_t)(p))
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0, w = esi_rec;   /* x86 working regs; w = working ESI */

    /* ---- Phase 0: flags + textured/untextured split (0x366cb) ---- */
#ifdef ROTH_STANDALONE
    if (g_rwss_skip_marker_clear) g_rwss_skip_marker_clear = 0;  /* 0x366d2 mid-entry: skip the prologue clear */
    else G8(VA_g_sprite_fill_index + 0x2) = 0;
#else
    G8(VA_g_sprite_fill_index + 0x2) = 0;
#endif
    ecx = (uint16_t)HP16(esi_rec + 0x16);                       /* sub ecx,ecx; mov cx,[esi+0x16] */
    G16(VA_g_world_surface_draw_flags) = (uint16_t)ecx;
    eax = (uint16_t)HP16(esi_rec + 0xc);                        /* sub eax,eax; mov ax,[esi+0xc] */
    if ((int8_t)(uint8_t)(eax >> 8) < 0) {                      /* test ah,ah; jns 0x36731 -> textured; here ah<0 */
        uint8_t ah = (uint8_t)(ecx & 3);                        /* 0x366ea untextured solid colour */
        if (ah != 3) ah = 0;
        G8(VA_g_span_textured_mode_flag) = ah;
        G16(VA_g_world_surface_draw_flags) = (uint16_t)(G16(VA_g_world_surface_draw_flags) & 0xfff7);
        { uint8_t al = (uint8_t)eax; G16(VA_g_sprite_fill_index) = (uint16_t)(al | (al << 8)); }
        G16(VA_g_span_src_wrap_reoffset + 0x8) = (uint16_t)HP16(esi_rec + 0x26);
        goto dispatch;                                          /* jmp 0x369d6 */
    }
    /* 0x36731 textured */
    G32(VA_g_current_das_entry_id) = (int32_t)eax;                                /* texture id */
    G8(VA_g_sprite_fill_index)  = (uint8_t)G16(VA_g_das_palette_remap_prefix);
    G8(VA_g_column_clip_mode + 0x1)  = (uint8_t)HP8(esi_rec + 0xf);
    G16(VA_g_span_src_wrap_reoffset + 0x8) = (uint16_t)HP16(esi_rec + 0x26);
    G16(VA_g_span_src_wrap_reoffset + 0x4) = (uint16_t)HP16(esi_rec + 0x24);

resolve:                                                        /* 0x3675d */
    ebx = (uint32_t)G32(VA_g_current_das_entry_id);
    if ((uint16_t)ebx >= 0x1200) goto solid;                   /* cmp bx,0x1200; jae 0x367ac */
    ebx += ebx;                                                 /* id*2 */
    eax = (uint16_t)G16(VA_g_das_entry_status_table + ebx);                         /* sub eax,eax; mov ax,[ebx+0x86d30] */
    if ((eax >> 8) & 2) goto indirect;                          /* 0x36775 test ah,2; jne 0x36858 */
classify:                                                       /* 0x3677e (re-entered from indirect, skipping ah&2) */
    {
        uint8_t al = (uint8_t)eax;
        if (al < 0xfd) goto cacheslot;                          /* jb 0x36825 */
        if (al == 0xfd) goto solid;                             /* je 0x367ac */
        if (al == 0xfe) goto animated;                          /* je 0x367ca */
        /* al == 0xff: lazy DAS cache load. imgfree calls the lifted loader spine directly
         * (0x40d7c = load_das_block_for_fat_index); trap host keeps rwss_brdg. */
        { int cf;
#ifdef ROTH_STANDALONE
          cf = load_das_block_for_fat_index((uint32_t)G32(VA_g_current_das_entry_id)) ? 1 : 0;
#else
          (void)rwss_brdg(0x40d7cu + OBJ_DELTA, (uint32_t)G32(VA_g_current_das_entry_id), 0, w, es_sel, fs_sel, gs_sel, &cf);
#endif
          if (!cf) goto resolve;                                /* jae 0x3675d (loaded -> retry) */ }
        G16(VA_g_world_surface_draw_flags) = (uint16_t)(G16(VA_g_world_surface_draw_flags) & 0xfff7);       /* load failed -> untextured */
        HP16(esi_rec + 0x16) = (uint16_t)(HP16(esi_rec + 0x16) & 0xfff7);
        goto solid_tail;                                        /* jmp 0x3671b */
    }

solid:                                                          /* 0x367ac */
    { uint8_t ah = (uint8_t)(eax >> 8); G16(VA_g_sprite_fill_index) = (uint16_t)((ah << 8) | ah); }
    G8(VA_g_span_textured_mode_flag) = 0;
    G16(VA_g_world_surface_draw_flags) = (uint16_t)(G16(VA_g_world_surface_draw_flags) & 0xfff7);
solid_tail:                                                     /* 0x3671b */
    G16(VA_g_span_src_wrap_reoffset + 0x8) = (uint16_t)HP16(esi_rec + 0x26);
    G8(VA_g_span_textured_mode_flag) = 0;
    goto dispatch;                                              /* jmp 0x369d6 */

animated:                                                       /* 0x367ca — walk the [0x90944] anim table (FIXME review) */
    w = (uint32_t)G32(VA_g_world_surface_draw_flags + 0x8);
    ebx = ((eax >> 8) & 0xff) * 2;                              /* sub ebx,ebx; mov bl,ah; add ebx,ebx */
    ebx = (uint16_t)HP16(w + ebx);                              /* movzx ebx,word[ebx+esi] */
    w += ebx;                                                   /* add esi,ebx */
    ebx = (uint16_t)HP16(w);                                    /* sub ebx,ebx; mov bx,word[esi] */
    if (ebx & 0x8000) {                                         /* billboard: view-angle frame */
        ebx = (uint16_t)(0x120 - (uint16_t)G16(VA_g_sprite_view_angle));       /* mov ebx,0x120; sub bx,[0x909f8] */
        ebx = (ebx >> 5) & 0xe;
    } else {
        ebx = (uint16_t)G16((VA_g_floor_tex_caps + 0x2) + ebx);                     /* movzx ebx,word[ebx+0x909b4] */
    }
    ebx += 2;                                                   /* 0x36803 */
    ebx = (uint16_t)HP16(w + ebx);                              /* movzx ebx,word[esi+ebx] */
    if ((int8_t)(uint8_t)(ebx >> 8) < 0) { G8(VA_g_world_surface_draw_flags) ^= 2; ebx &= 0x7fff; }   /* test bh,bh; jns; xor byte */
    G32(VA_g_current_das_entry_id) = (int32_t)ebx;
    goto resolve;                                               /* jmp 0x3675d */

indirect:                                                       /* 0x36858 — [0x8a2a0]->0x33dde hook (BRIDGE) */
    eax = ebx;                                                  /* eax = id*2 */
    { uint32_t hook = (uint32_t)G32(VA_g_pool_check_enabled + 0x28);
      if (hook != 0) {
#ifdef ROTH_STANDALONE
          eax = rwss_id_remap_dispatch(eax >> 1);   /* two-value [0x8a2a0] dispatch -> lifted 0x33dde (fail-loud else) */
#else
          eax = rwss_brdg(hook, eax >> 1, hook, w, ds_sel, fs_sel, gs_sel, NULL);  /* eax=id, ebx=hook, es=ds (push ds;pop es) */
#endif
          ebx = eax; G32(VA_g_current_das_entry_id) = (int32_t)eax;
          ebx += ebx; eax = (eax & 0xffff0000u) | (uint16_t)G16(VA_g_das_entry_status_table + ebx);    /* mov ax preserves eax-high */
      } else {                                                  /* 0x36881 null hook */
          ebx = eax; eax = (eax & 0xffff0000u) | (uint16_t)G16(VA_g_das_entry_status_table + ebx);
      }
    }
    goto classify;                                              /* jmp 0x3677e */

cacheslot:                                                      /* 0x36825 */
    ebx = (eax & 0xff) * 6;                                     /* movzx ebx,al; lea ebx,[ebx+ebx*2]; add ebx,ebx */
    G16((VA_g_das_cache_slots + 0x4) + ebx) = (uint16_t)G16(VA_g_das_cache_tick);                /* mov si,[0x90c0a]; mov [ebx+0x89934],si */
    w = (uint32_t)G32(VA_g_das_cache_slots + ebx);                           /* mov esi,[ebx+0x89930] */
    if (w == 0) {                                               /* slot empty (int3 NOP) -> load + retry */
        int cf;
#ifdef ROTH_STANDALONE
        cf = load_das_block_for_fat_index((uint32_t)G32(VA_g_current_das_entry_id)) ? 1 : 0;   /* 0x40d7c native */
#else
        (void)rwss_brdg(0x40d7cu + OBJ_DELTA, (uint32_t)G32(VA_g_current_das_entry_id), 0, esi_rec, es_sel, fs_sel, gs_sel, &cf);
#endif
        if (!cf) goto resolve;                                  /* jae 0x3675d */
        return;                                                 /* pop esi; ret (0x36856) */
    }
    w = HP32(w);                                                /* mov esi,[esi] */
    if (HP8(w - 8) & 4) {                                       /* test byte[esi-8],4; jne 0x3689c relocate */
#ifdef ROTH_STANDALONE
        refresh_moved_das_cache_block(w);               /* B-1: 0x41250 native (register-transparent -> eax/ah unchanged) */
#else
        (void)rwss_brdg(0x41250u + OBJ_DELTA, eax, ebx, w, es_sel, fs_sel, gs_sel, NULL);  /* preserves GP -> eax (ah) unchanged */
#endif
    }
    if ((eax >> 8) & 1) {                                       /* 0x368ab test ah,1; jne 0x3688f callback */
#ifdef ROTH_STANDALONE
        /* 0x39093 carrier: eax=ebx=[0x90a78]; null [0x90a34] -> RAW eax through (no cwde); else
         * cwde, edx=esi=block, es=ds, call hook — residue eax kept (flows into [0x8a2a4] below,
         * the :12388 FIXME's documented original behavior) */
        { uint32_t d = (uint32_t)G32(VA_g_current_das_entry_id);
          if (G32(VA_g_span_callback) == 0) eax = d;
          else eax = rwss_span_callback_dispatch((uint32_t)(int32_t)(int16_t)(uint16_t)d, w); }
#else
        eax = rwss_brdg(0x39093u + OBJ_DELTA, (uint32_t)G32(VA_g_current_das_entry_id), (uint32_t)G32(VA_g_current_das_entry_id), w, es_sel, fs_sel, gs_sel, NULL);
#endif
    }
    /* fall to params (0x368b0) */

    /* ---- Phase 2: surface params (0x368b0) ---- */
    eax = (eax & 0xffff0000u) | (uint16_t)HP16(w + 0xa);        /* mov ax,word[esi+0xa] */
    G32(VA_g_pool_check_enabled + 0x2c) = (int32_t)eax;                                /* mov [0x8a2a4],eax (eax-high carried, FIXME review) */
    {
        uint8_t ah = (uint8_t)(eax >> 8);
        if (ah & 8) {                                          /* 0x368b9 reflection (BRIDGE 0x283d9; eax preserved) */
            int cf;
#ifdef ROTH_STANDALONE
            /* 0x283d9 = the lifted leaf add_reflection_view_entry. It loads its OWN fs
             * (mov ebx,[0x84ac4]; mov fs,[ebx+0x10]) — the rasterizer's fs_sel is irrelevant —
             * so resolve that selector here (the test-harness ABI, technique i). Only CF is
             * consumed; the bridge's eax/w inputs are dead (pushal/popal in the original). */
            { extern uint32_t (*g_os_sel_base)(uint16_t);
              uint32_t vrec = (uint32_t)G32(VA_g_current_proc_tag + 0x128);
              uint16_t rsel = *(volatile uint16_t *)(uintptr_t)(vrec + 0x10);
              cf = add_reflection_view_entry(g_os_sel_base(rsel)); }
#else
            (void)rwss_brdg(0x283d9u + OBJ_DELTA, (uint32_t)G32(VA_g_current_das_entry_id), 0, w, es_sel, fs_sel, gs_sel, &cf);
#endif
            if (cf) {                                          /* jb 0x36900 -> reset to untextured */
                G8(VA_g_span_textured_mode_flag) = 0; G16(VA_g_world_surface_draw_flags) = (uint16_t)(G16(VA_g_world_surface_draw_flags) & 0xfff7); G16(VA_g_sprite_fill_index) = 0;
                goto solid_tail;
            }
            if (ah & 4) goto srcptr;                           /* 0x368f9 test ah,4; jne 0x3696a */
            return;                                            /* pop esi; ret (0x368fe) */
        }
        if (ah & 1) {                                          /* 0x368be anim (re-point 0x38fec) */
            /* pure-DS leaf: ESI=block=w; eax/ebx + selectors unused, eax reloaded at texsel. */
            advance_das_sprite_animation_frame(w);
            goto texsel;                                       /* jmp 0x36972 (eax reloaded there) */
        }
        if (!((uint8_t)eax & 0x40)) goto srcptr;               /* 0x368c3 test al,0x40; je 0x3696a */
        /* 0x368cb sub-record select */
        ebx = (uint16_t)HP16(w + 0x10);                        /* movzx ebx,word[esi+0x10] */
        if ((ebx >> 8) & 0x80) {                               /* test bh,0x80; je 0x3693f */
            ebx = (uint16_t)(0x120 - (uint16_t)G16(VA_g_sprite_view_angle));  /* billboard view-angle */
            ebx = (ebx >> 5) & 0xe;
            ebx = (uint16_t)HP16(w + ebx + 0x12);              /* 0x3694d movzx ebx,word[esi+ebx+0x12] */
        } else if (ebx & 0x4000) {                             /* 0x3693f test bx,0x4000; jne 0x36926 */
            ebx = (uint8_t)HP8(esi_rec + 0x1c);                /* [esp+8]=esi_rec; movzx ebx,byte[ebx+0x1c] */
            ebx += ebx;
            ebx = (uint16_t)HP16(w + ebx + 0x12);              /* 0x36938 (the and ax,0x1ff is dead -> omitted) */
        } else {
            ebx = (uint16_t)G16((VA_g_floor_tex_caps + 0x2) + ebx);                /* 0x36946 movzx ebx,word[ebx+0x909b4] */
            ebx = (uint16_t)HP16(w + ebx + 0x12);              /* 0x3694d */
        }
        if ((int16_t)(uint16_t)ebx < 0) { G8(VA_g_world_surface_draw_flags) ^= 2; ebx &= 0x7fff; }   /* 0x36952 test bx,bx; jns */
        ebx <<= 4;                                             /* shl ebx,4 */
        w += ebx;                                              /* add esi,ebx */
    }

srcptr:                                                         /* 0x3696a */
    G32(VA_g_render_source_base_ptr) = (int32_t)(w + 0x10);                        /* lea eax,[esi+0x10]; mov [0x84980],eax */
texsel:                                                         /* 0x36972 */
    eax = HP32(w + 8);                                          /* mov eax,[esi+8] */
    G32(VA_g_world_alt_render_flags + 0x2) = (int32_t)eax;
    if (eax & 0x4000000u) {                                     /* translucent (int3 NOP @0x36981) */
        G8(VA_g_span_textured_mode_flag) = 0xff;
    } else {
        G8(VA_g_span_textured_mode_flag) = 0;
        if (eax & 0x2000000u) { G8(VA_g_world_surface_draw_flags) = (uint8_t)(G8(VA_g_world_surface_draw_flags) & 0xfe); G16(VA_g_wall_render_flags) = 0; }  /* BYTE op (0x36999) */
    }
    /* dims (0x369a9) */
    edx = (uint16_t)HP16(w + 0xc);  G16(VA_g_span_src_row_width) = (uint16_t)edx;  G16(VA_g_span_src_wrap_reoffset) = (uint16_t)(edx - 1);
    edx = (uint16_t)HP16(w + 0xe);  G16(VA_g_span_src_wrap_reoffset + 0xc) = (uint16_t)edx;  G16(VA_g_span_src_wrap_reoffset + 0xe) = (uint16_t)edx;  G16(VA_g_column_clip_mode + 0x4) = (uint16_t)(edx - 1);

dispatch:                                                       /* 0x369d6 (esi = span record) */
    if (G8(VA_g_world_render_subpass_kind) != 0) {                                     /* subpass: clear shade bits */
        G16(VA_g_world_surface_draw_flags) = (uint16_t)(G16(VA_g_world_surface_draw_flags) & 0x7fff);
        G8(VA_g_column_clip_mode)  = 0;
        G16(VA_g_world_alt_render_flags) = (uint16_t)(G16(VA_g_world_alt_render_flags) & 0x7fff);
    }
    G32(VA_g_perspective_scale + 0x8)++;                                            /* inc [0x85290] */
#ifdef ROTH_STANDALONE
    /* BUGFIX (couch / 3D-object mesh faces): the floorceil driver reloads
     * FS from [0x909b0] INSIDE its textured path (0x3a89f `mov fs,[0x909b0]`) — i.e. from the selector
     * this function's resolve loop JUST stored at texsel (0x36975 `mov [0x909b0],eax`, eax=[block+8]).
     * The fs_tex_base parameter was resolved by the CALLER before entry, so on the sprite-queue
     * mid-entry path (0x366d2, rwss_sprite_body_entry) — where the texture is first resolved
     * HERE (a mesh face's Graphics-Folder child via the per-face +0x1c sub-select at 0x36926-0x36966,
     * e.g. the couch's DEMO.DAS fat 0x1c5) — it is STALE (the previous surface's selector) and the
     * floorceil fills sample the wrong block: metadata-as-colors on the &0x200 "render-as-floor" mesh
     * faces, varying with draw order/camera. Re-resolve from the live [0x909b0] (low16 = selector),
     * exactly as the wall path below already does for its own 0x36a43 reload. The sprite branch
     * (base = [0x84980] = sub-record+0x10 == the minted selector base, 0x411b3-0x411b8) and the wall
     * branch were already fresh. Imgfree-only: the trap lane reaches this native rasterizer solely via
     * paths that pre-store [0x909b0] before entry (kept byte-frozen). */
    if (g_os_sel_base) fs_tex_base = g_os_sel_base((uint16_t)G16(VA_g_world_alt_render_flags + 0x2));
#endif
    if (G16(VA_g_world_surface_draw_flags) & 0x20) {                                  /* 0x369ff SPRITE */
        draw_scaled_sprite_spans(esi_rec, gs_base, blend_base, (uint32_t)G32(VA_g_render_source_base_ptr), es_sel, fs_sel);
        return;
    }
    if (G16(VA_g_world_surface_draw_flags) & 0x200) {                                /* 0x36a0e FLOORCEIL */
        draw_floorceil_surface(esi_rec, gs_base, fs_tex_base, es_fb_base, blend_base, es_sel, fs_sel);
        return;
    }
    if ((uint8_t)classify_surface_floorceil(esi_rec) != 0) {   /* 0x36a1d classify -> FLOORCEIL */
        draw_floorceil_surface(esi_rec, gs_base, fs_tex_base, es_fb_base, blend_base, es_sel, fs_sel);
        return;
    }

    /* ---- Phase 4: WALL path (0x36a2c) -> shared body 0x36b68 ---- */
    G16(VA_g_world_surface_draw_flags + 0x4) = 0;
    g_wd_gs_base = gs_base; g_wd_es_base = blend_base; g_wd_terminate = 0;
    /* 0x36a42 reloads FS from the FRESHLY-resolved [0x909b0] (per-texture selector); re-resolve its base
     * (the adapter's fs_tex_base was the PRE-resolution selector). [0x909b0] low16 = the texture selector. */
    g_wd_fs_base = g_os_sel_base ? g_os_sel_base((uint16_t)G16(VA_g_world_alt_render_flags + 0x2)) : fs_tex_base;
    if (!(G16(VA_g_world_surface_draw_flags) & 8)) { wall_body_36b68(0, 0); return; }   /* je 0x36b78 untextured */
    if (G16(VA_g_world_surface_draw_flags) & 0x100) {                                 /* 0x36a49 stored-extents path */
        uint32_t axw = (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x4), cxw = (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x8);
        if (G8(VA_g_current_surface_render_flags) & 0x20) { cxw += cxw; axw += axw; G16(VA_g_span_src_wrap_reoffset + 0xa) = (uint16_t)(G16(VA_g_span_src_wrap_reoffset + 0xa) << 1); }
        uint32_t dxw = (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0xc);  dxw += dxw;  dxw = (uint16_t)(dxw - (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0x2c));
        if ((int16_t)(uint16_t)dxw >= (int16_t)(uint16_t)cxw) G16(VA_g_column_clip_mode + 0x4) = 0xffff;   /* cmp dx,cx; jl skip */
        G16(VA_g_wall_proj_y3 + 0x4) = (uint16_t)axw;  G16(VA_g_wall_proj_y3 + 0x8) = (uint16_t)cxw;  G16(VA_g_world_surface_draw_flags + 0x4) = 0x100;
        wall_body_36b68(0, 1); return;
    }
    /* 0x36aae computed-extents */
    G16(VA_g_span_src_wrap_reoffset + 0x2c) = 0; G16(VA_g_span_src_wrap_reoffset + 0x2e) = 0;
    {
        uint8_t cl = (uint8_t)G8(VA_g_column_clip_mode + 0x1);
        if (cl == 0) {
            G16(VA_g_column_clip_mode + 0x4) = 0xffff;
            uint32_t e = (uint32_t)G32(VA_g_span_src_row_width); e += e; G16(VA_g_wall_proj_y3 + 0x4) = (uint16_t)e;
            uint32_t e2 = (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0xc); e2 += e2; G16(VA_g_wall_proj_y3 + 0x8) = (uint16_t)e2;
        } else {
            uint8_t clh = (uint8_t)(cl >> 4);
            uint32_t e = (uint16_t)G16(VA_g_span_src_row_width);
            if (clh != 0) e = (uint16_t)(e * (uint16_t)(1 + clh));   /* mul bx (16-bit) */
            e += e; G16(VA_g_wall_proj_y3 + 0x4) = (uint16_t)e;
            uint8_t cll = (uint8_t)(G8(VA_g_column_clip_mode + 0x1) & 0xf);
            uint32_t e2 = (uint16_t)G16(VA_g_span_src_wrap_reoffset + 0xc);
            if (cll != 0) e2 = (uint16_t)(e2 * (uint16_t)(1 + cll));
            e2 += e2; G16(VA_g_wall_proj_y3 + 0x8) = (uint16_t)e2;
        }
    }
    wall_body_36b68(0, 1);
#undef HP8
#undef HP16
#undef HP32
}

/* ---------------------------------------------------------------------------
 * recompute_view_region_offsets (0x10e4e) — pure leaf. From the active view-mode
 * entry (byte[0x7049a]*4 indexing the u16 pair table @0x703d0/0x703d2) and the
 * viewport extents [0x85498]/[0x8549c], compute the centered blit offsets
 * [0x85cd8]/[0x85ce0] (X) and [0x85cdc]/[0x85ce4] (Y). No-op when [0x76634]==0.
 * Register inputs preserved/unused (input edx saved+restored). divs are UNSIGNED.
 * ------------------------------------------------------------------------- */
void recompute_view_region_offsets(void)
{
    uint32_t off = (uint32_t)(uint8_t)G8(VA_g_cfg_das2_arg + 0x1be) << 2;          /* sub ebx; mov bl; shl 2 */
    uint32_t v0 = (uint16_t)G16((VA_g_cfg_das2_arg + 0xf4) + off);                  /* movzx eax,word[ebx+0x703d0] */
    uint32_t v1 = (uint16_t)G16((VA_g_cfg_das2_arg + 0xf6) + off);                  /* movzx eax,word[ebx+0x703d2] */

    if ((uint32_t)G32(VA_g_video_linear_flag) != 0) {                           /* cmp [0x76634],0; je unscaled */
        /* scaled/zoomed: project the u16 pair through [0x85498]/[0x8549c]. */
        uint32_t w = (uint32_t)G32(VA_g_screen_pitch);
        uint32_t r0 = (uint32_t)(v0 * w) / 0x140u;               /* imul; div 0x140 */
        G32(VA_g_view_w) = (int32_t)r0;
        G32(VA_g_view_x) = (int32_t)((w - r0) >> 1);                 /* (w-r0)>>1 */
        uint32_t h = (uint32_t)G32(VA_g_screen_pitch + 0x4);
        G32(VA_g_view_y) = 0;                                        /* mov dword[0x85ce4],0 */
        uint32_t r1 = (uint32_t)(v1 * h) / 0xc8u;                /* imul; div 0xc8 */
        if (v1 < 0xaau)                                          /* cmp edx,0xaa; jae skip */
            G32(VA_g_view_y) = (int32_t)((h - r1) >> 1);
        G32(VA_g_view_h) = (int32_t)r1;
    } else {
        /* 0x10ed1: unscaled — use the raw u16 vs the fixed 0x140/0xc8 viewport. */
        G32(VA_g_view_w) = (int32_t)v0;
        G32(VA_g_view_x) = (int32_t)((0x140u - v0) >> 1);
        G32(VA_g_view_y) = 0;
        G32(VA_g_view_h) = (int32_t)v1;
        if (v1 < 0xaau)                                          /* cmp eax,0xaa; jae 0x10f18 */
            G32(VA_g_view_y) = (int32_t)((0xc8u - v1) >> 1);
    }
}

/* ---------------------------------------------------------------------------
 * tick_ambient_render_animation (0x2ab30) — pure leaf. Advances the ambient
 * light/colour cycle: if the 16-bit accumulator [0x90bcc]-[0x90bd0] reaches >=7,
 * step a 16-bit LCG [0x85328] and derive two jittered light positions
 * [0x71ef4]/[0x71ef6] from the per-axis spans [0x71ee8]/[0x71eea]. Always: scroll
 * accumulator [0x8531e]/[0x85324] feeds a >>2 step that wrap-normalises the 28
 * u16 cycle slots @0x909b8 each modulo their per-slot stride (4,6,..,0x3a).
 * ------------------------------------------------------------------------- */
void tick_ambient_render_animation(void)
{
    int16_t diff = (int16_t)((uint16_t)G16(VA_g_frame_tick_counter) - (uint16_t)G16(VA_g_frame_tick_counter + 0x4));
    if (diff >= 7) {                                            /* cmp ax,7; jl skip */
        G16(VA_g_frame_tick_counter + 0x4) = (uint16_t)((uint16_t)G16(VA_g_frame_tick_counter + 0x4) + (uint16_t)diff);
        uint16_t rng = (uint16_t)((uint16_t)((uint16_t)G16(VA_g_frame_time_scale + 0x4) * 0x5e5u) + 0x29u);
        G16(VA_g_frame_time_scale + 0x4) = rng;                                    /* LCG step */

        int32_t e0 = ((int32_t)((rng & 0x1fu)) - 0x10) * (int32_t)(uint16_t)G16(VA_g_init_stage_error_strings + 0x118);
        uint32_t e0u = (uint32_t)e0 >> 8;                      /* shr eax,8 (logical) */
        G16(VA_g_init_stage_error_strings + 0x124) = (uint16_t)((uint16_t)e0u + (uint16_t)G16(VA_g_view_center_x));

        int32_t e1 = ((int32_t)(((rng >> 8) & 0x1fu)) - 0x10) * (int32_t)(uint16_t)G16(VA_g_init_stage_error_strings + 0x11a);
        uint32_t e1u = (uint32_t)e1 >> 8;
        G16(VA_g_init_stage_error_strings + 0x126) = (uint16_t)((uint16_t)e1u + (uint16_t)G16(VA_g_anim_clock + 0x2));
    }

    /* 0x2abad: scroll-accumulate, then wrap-normalise the 28 cycle slots. */
    uint32_t eax = (uint32_t)G32(VA_g_frame_time_scale);                     /* mov eax,[0x85324] (dword) */
    uint16_t s = (uint16_t)G16(VA_g_secondary_surface_count + 0x6);
    uint16_t ax_val = (uint16_t)((uint16_t)(eax & 0xffff) + s);/* add ax,[0x8531e] (16-bit) */
    uint16_t ax_masked = (uint16_t)(ax_val & 0xfffcu);         /* and al,0xfc */
    G16(VA_g_secondary_surface_count + 0x6) = (uint16_t)(ax_val - ax_masked);             /* sub dx,ax -> low 2 bits */
    eax = (eax & 0xffff0000u) | (uint16_t)(ax_masked >> 2);    /* shr ax,2 (upper 16 preserved) */

    uint32_t addr = 0x909b8;                                   /* mov esi,0x909b8 */
    uint32_t ecx = 4;                                          /* mov ecx,4 */
    do {
        uint32_t edx = (uint16_t)G16(addr);                   /* sub edx; mov dx,[esi] */
        edx += eax;                                            /* add edx,eax (32-bit) */
        if (edx >= ecx) {                                      /* jb store (inverted) */
            do {
                edx -= ecx;                                    /* sub edx,ecx */
                if ((int32_t)edx < 0) edx &= 0x3eu;            /* jns skip; and edx,0x3e */
            } while (edx >= ecx);                              /* cmp/jae */
        }
        G16(addr) = (uint16_t)edx;                            /* mov [esi],dx */
        addr += 2;
        ecx += 2;
    } while ((int8_t)(uint8_t)ecx < 0x3c);                     /* cmp cl,0x3c; jl */
}
