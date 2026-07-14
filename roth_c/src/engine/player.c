/* lift_player.c — the ROTH `player` subsystem (movement & camera) lifted to verified C.
 * Own TU per docs/operating/recomp.md §4.6. lift-lens:
 * docs/reference/lift/player.md; behavior: docs/reference/ROTH_movement_spec.md.
 *
 * Scope = the physics+camera half of the player update (the input/movement-intent half is already
 * lifted: apply_player_movement_input, player_movement_tick). Bottom-up lift order:
 *   L1 pure-math camera leaves (oracle) -> L2 vertical physics + view bob (oracle) ->
 *   L3 movement hub + damage flash (live-swap) -> L4 update_player_tick (live-swap).
 *
 * ABI is derived from the DISASM (the corpus is Borland-on-Watcom, unreliable — recomp.md §9.0).
 * All player state lives in obj3 globals (plain scalars -> G8/G16/G32); the camera record at
 * 0x89eec is a fixed obj3 block (deref via GADDR + byte offset, mirroring the original's ebx+N).
 */
#include "common.h"
#include "engine.h"
#include <string.h>

/* bridge with EAX/EDX register args (for the weapon/audio callees of the vertical-physics integrator) */
static uint32_t pl_bridge_eax_edx(uint32_t canon_va, uint32_t eax, uint32_t edx)
{
    regs_t io; memset(&io, 0, sizeof io);
    io.va = canon_va + OBJ_DELTA; io.eax = eax; io.edx = edx;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    switch (canon_va) {            /* routes into the verified lifted bodies (no returns consumed in this TU) */
    case 0x32058u: apply_direct_damage_to_player(eax); return 0;         /* EAX=dmg; void */
    case 0x271fbu: return start_persistent_looping_sound(eax, edx);      /* EAX=id, EDX=param */
    default: break;
    }
    roth_unreachable(canon_va);   /* player vertical-physics callee bridge — in-game player tier */
#endif
    return io.eax;
}

/* full-register bridge (the per-frame tick's callees set varied register args at each call site) */
static uint32_t pl_bridge5(uint32_t canon_va, uint32_t eax, uint32_t edx,
                           uint32_t ebx, uint32_t ecx, uint32_t esi)
{
    regs_t io; memset(&io, 0, sizeof io);
    io.va = canon_va + OBJ_DELTA;
    io.eax = eax; io.edx = edx; io.ebx = ebx; io.ecx = ecx; io.esi = esi;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    switch (canon_va) {            /* routes into the verified lifted bodies (no returns consumed in this TU) */
    case 0x16da2u: return fire_pending_weapon_shot(eax);                 /* EAX=weapon record ptr */
    case 0x1d0fdu: return consume_held_item(eax);                        /* EAX=slot -> 1/0 */
    case 0x184abu: activate_weapon_item(eax, edx); return 0;             /* (0,0) equip-clear; ESI is not an arg of the body */
    case 0x16087u: load_dbase200_sprite_cached(eax); return 0;           /* EAX=idx; void */
    case 0x139a0u: draw_player_viewmodel_sprite(eax, edx, ebx, ecx); return 0;  /* void blit */
    default: break;
    }
    roth_unreachable(canon_va);   /* per-frame player-tick callee bridge — in-game player tier */
#endif
    return io.eax;
}

/* DI as a 16-bit signed height: sign-extend the low word of the 32-bit "edi" model (movsx eax,di) */
static inline int32_t di_sx(uint32_t edi) { return (int32_t)(int16_t)(uint16_t)edi; }

/* ====================================================================== L1 — pure-math leaves */

/* update_view_transform_params (0x3e2ba, 151 B) — pure camera-transform math; NO callees. Negates the
 * player facing/offset angles into the world->view rotation params of the camera record at 0x89eec
 * (+0x00/+0x02/+0x04/+0x14 = 16-bit rotation/translation fields; +0x18 = a flags word), then clamps the
 * view pitch to [-0x7e,+0x7e] and commits it to 0x89ee8 (dword) / 0x89ee6 (word). All obj3. EAX unused. */
void update_view_transform_params(void)
{
    volatile uint8_t *rec = (volatile uint8_t *)GADDR(VA_g_das_cache_slots + 0x5bc);   /* mov ebx,0x89eec */

    /* +0x14 = -(player_angle + dword_8c104)  (16-bit, low word of the 32-bit add) */
    uint16_t a14 = (uint16_t)((uint32_t)(uint16_t)G16(VA_g_player_angle) + (uint32_t)G32(VA_g_collision_hit_entity + 0x10));
    *(volatile int16_t *)(rec + 0x14) = (int16_t)(uint16_t)(0u - a14);
    /* +0x00 = -word_90a8e */
    *(volatile int16_t *)(rec + 0x00) = (int16_t)(uint16_t)(0u - (uint16_t)G16(VA_g_player_x));
    /* +0x04 = -word_90a96 */
    *(volatile int16_t *)(rec + 0x04) = (int16_t)(uint16_t)(0u - (uint16_t)G16(VA_g_player_y));
    /* +0x02 = (-word_90a92) - word_8c112 */
    *(volatile int16_t *)(rec + 0x02) =
        (int16_t)(uint16_t)((uint16_t)(0u - (uint16_t)G16(VA_g_player_z)) - (uint16_t)G16(VA_g_player_height + 0x2));

    /* +0x18 flags: clear bit 0x4000, set it when (word_90be6 & 4) */
    uint16_t fl = (uint16_t)(*(volatile int16_t *)(rec + 0x18)) & 0xbfffu;
    if (G16(VA_g_video_mode_flags) & 4) fl |= 0x4000u;
    *(volatile int16_t *)(rec + 0x18) = (int16_t)fl;

    /* view pitch: source = g_view_pitch (0x90a74), or dword_8c108 when pitch==0. Clamp to [-0x7e,0x7e],
     * writing the clamp back to the source; commit the result to 0x89ee8 + the 0x90a72 word to 0x89ee6. */
    volatile int32_t *p = (volatile int32_t *)GADDR(VA_g_view_pitch);
    if (*p == 0) p = (volatile int32_t *)GADDR(VA_g_view_pitch_applied);
    int32_t v = *p;
    if (v >= 0x7e)  { v = 0x7e;  *p = v; }    /* cmp/jl  then clamp-high */
    if (v <= -0x7e) { v = -0x7e; *p = v; }    /* cmp/jg  then clamp-low */
    G32(VA_g_das_cache_slots + 0x5b8) = v;
    G16(VA_g_das_cache_slots + 0x5b6) = G16(VA_g_view_center_y);
}

/* update_turn_view_scale (0x3e22c, 142 B) — commit a discrete turn step from the signed turn accumulator
 * g16(0x90bd8): clamp it to {-8,0,+8} and, when the committed step changes, record it (0x90cc0), set the
 * viewport-mode word (0x90bfc = 1 small / 2 stepped), mark the active+dirty flags (0x90bd4/0x8c1d2), and
 * tail-call configure_render_viewport 0x408d1 (render_world -> bridge). The early gates return with no
 * writes. The byte 0x90cc0 caches the last committed step (0/8/0xf8). EAX unused (tail-call return). */
void update_turn_view_scale(void)
{
    /* gate: when a step is already active (0x90bd4!=0) but none was committed (0x90cc0==0) -> bail */
    if (G8(VA_g_view_scale_flags) != 0 && G8(VA_g_turn_view_scale_state) == 0) return;
    if (G16(VA_g_blur_flag) == 0) return;                       /* turn disabled -> bail */

    int16_t acc = (int16_t)G16(VA_g_turn_accum);
    if (acc < 8 && acc > -8) {                           /* sub-threshold: settle back to neutral */
        if (G8(VA_g_turn_view_scale_state) == 0) return;                    /* already neutral -> nothing to do */
        G16(VA_g_render_viewport_reconfig) = 1;
        G8(VA_g_turn_view_scale_state)  = 0;
        G8(VA_g_view_scale_flags)  = 0;
        G8(VA_g_collision_sector_stack + 0x3e)  = 1;
        configure_render_viewport();              /* was pl_bridge 0x408d1 (body oracle-verified in test_render_world, [0x8c1d2]=1 above skips the 0x2e140 VGA bridge; player tests stage viewport + ret-patch 0x2fdfc) */
        return;
    }
    uint8_t step = (acc >= 8) ? 0x08u : 0xf8u;           /* clamp to +8 / -8 (low byte) */
    if (step == G8(VA_g_turn_view_scale_state)) return;                     /* unchanged step -> nothing to do */
    G16(VA_g_render_viewport_reconfig) = 2;
    G8(VA_g_turn_view_scale_state)  = step;
    G8(VA_g_view_scale_flags)  = 1;
    G8(VA_g_collision_sector_stack + 0x3e)  = 1;
    configure_render_viewport();                  /* was pl_bridge 0x408d1 (see the sibling site above) */
}

/* apply_view_camera_params (0x2a952, 236 B) — per-frame commit of the active view/camera record into the
 * renderer's working globals. One-time init: if the view-buffer selector cache word (0x29e64, in code
 * space) is unset, allocate a DPMI selector (alloc_dpmi_selector 0x2f72a — DPMI, bridged) over base
 * 0x29e6c/limit 0x100 and cache it. Then copy fields out of three STORED-POINTER records — esi=g32(0x8526c),
 * ebx=g32(0x85270), ebx2=g32(0x85274) (A4: these globals hold runtime addresses; deref RAW, not via GADDR)
 * — into the camera/view globals (mostly mirrored into both the 0x852xx renderer block and the 0x909xx/
 * 0x9096x view block). Pure obj3 commit aside from the one-time selector alloc. EAX unused. */
void apply_view_camera_params(void)
{
    if (G16(VA_g_default_shadow_texture_record + 0x8) == 0) {                              /* one-time: allocate + cache the view selector */
        regs_t io; memset(&io, 0, sizeof io);
        io.va  = 0x2f72au + OBJ_DELTA;                   /* alloc_dpmi_selector(EDI=base, ECX=limit) */
        io.edi = 0x29e6c;
        io.ecx = 0x100;
#ifndef ROTH_STANDALONE
        call_orig(&io);
#else
        /* alloc_dpmi_selector transcription over the int31 seam (the ml_alloc_sel pattern).
         * The base is the RELOCATED 0x29e6c (the original's imm32 carries an LE fixup -> runtime
         * GADDR(0x29e6c), inside the obj1 arena). NOTE the trap-lane arm above stages the raw canon —
         * a gotcha-H latent that never fires live (the original's one-time init runs before any swap). */
        {
            regs_t v; memset(&v, 0, sizeof v);
            v.eax = 0; v.ecx = 1;
            if ((g_os_soft_int(0x31, &v) & 1u) == 0) {
                uint32_t sel = v.eax & 0xffffu, base = (uint32_t)GADDR(VA_g_default_shadow_texture_record + 0x10);
                memset(&v, 0, sizeof v);
                v.eax = 7; v.ebx = sel; v.ecx = base >> 16; v.edx = base & 0xffffu;
                g_os_soft_int(0x31, &v);
                memset(&v, 0, sizeof v);
                v.eax = 8; v.ebx = sel; v.ecx = 0; v.edx = 0x100 - 1;
                g_os_soft_int(0x31, &v);
                io.eax = sel;
            }
        }
#endif
        G16(VA_g_default_shadow_texture_record + 0x8) = (uint16_t)io.eax;
    }

    volatile uint8_t *esi = (volatile uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_view_bounds_rect);   /* A4 raw ptr */
    G32(VA_g_view_params_block + 0x8) = *(volatile int32_t *)(esi + 0x00);
    uint16_t va = *(volatile uint16_t *)(esi + 0x0a); G16(VA_g_view_bound_left) = va; G16(VA_g_sector_cull_coord + 0xe) = va;
    uint16_t vb = *(volatile uint16_t *)(esi + 0x08); G16(VA_g_view_bound_top) = vb; G16(VA_g_sector_cull_coord + 0x10) = vb;
    uint16_t vc = *(volatile uint16_t *)(esi + 0x0e); G16(VA_g_sector_cull_coord + 0x12) = vc; G16(VA_g_view_bound_right) = vc;
    uint16_t vd = *(volatile uint16_t *)(esi + 0x0c); G16(VA_g_sector_cull_coord + 0x14) = vd; G16(VA_g_view_bound_bottom) = vd;

    volatile uint8_t *eb = (volatile uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_view_params_block);    /* A4 raw ptr */
    G16(VA_g_world_alt_render_flags) = *(volatile uint16_t *)(eb + 0x18);
    G16(VA_g_vertex_selector + 0x28) = (uint16_t)(*(volatile uint16_t *)(eb + 0x12) & 0x1ff);
    uint16_t ve = *(volatile uint16_t *)(eb + 0x0e);
    G16(VA_g_vertex_selector + 0x2a) = ve;
    G16(VA_g_sprite_view_angle) = (uint16_t)(ve + *(volatile uint16_t *)(eb + 0x14));
    G16(VA_g_view_offset_x) = *(volatile uint16_t *)(eb + 0x00);
    G16(VA_g_view_offset_y) = *(volatile uint16_t *)(eb + 0x04);
    uint16_t vf = *(volatile uint16_t *)(eb + 0x02);
    G16(VA_g_vertex_selector + 0x2c) = vf;
    G16(VA_g_sector_cull_coord) = (uint16_t)(0u - vf);                  /* sub eax,eax; mov ax,[ebx+2]; neg eax */

    volatile uint8_t *eb2 = (volatile uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_view_params_block + 0x4);   /* A4 raw ptr */
    G16(VA_g_span_src_wrap_reoffset + 0x14) = *(volatile uint16_t *)(eb2 + 0x12);
    G16(VA_g_span_src_wrap_reoffset + 0x16) = *(volatile uint16_t *)(eb2 + 0x14);
}

/* ====================================================================== L2 — vertical physics + view bob */

/* update_player_vertical_physics (0x1c648, 807 B) — the player vertical integrator: gravity/jump-arc,
 * crouch stance, fall accumulation + fall damage, and the floor/ceiling clamp. __watcall args:
 *   EAX = floor_z (lower clamp baseline) · EDX = ceil_raw (the fn uses ceil_raw-0x1c as the upper bound) ·
 *   EBX = pos (vertical coord it integrates) -> returns the new pos in EAX. The height delta lives in
 * g_player_height (word 0x8c110, tracked in DI; modeled as a 32-bit edi whose low word is live -> di_sx).
 * State-machine globals: 0x819c0 locomotion flags; 0x819c1 jump-arc phase (0 grounded / 1..0x13 rising,
 * indexing the gravity table 0x71304); 0x819c5 fall state (0 none / 1 small / 2 big / -1 ceiling-bonk);
 * 0x819c9 land-pending; 0x819cd jump-initiated-this-frame; 0x819d1 fall accumulator (-> fall damage +
 * landing sound). Callees: key_z_crouch 0x1c5d0 [L] (forced on death), approach_value 0x1c630 [L]
 * (toward-target clamped step), apply_direct_damage_to_player 0x32058 (weapon_combat -> bridge; fall
 * damage), start_persistent_looping_sound 0x271fb (audio -> bridge; landing thud). All compares signed. */
uint32_t update_player_vertical_physics(uint32_t eax_in, uint32_t edx_in, uint32_t ebx_in)
{
    int32_t floor_z = (int32_t)eax_in;          /* esi */
    int32_t pos     = (int32_t)ebx_in;          /* ecx */

    /* entry gate: mode byte 0x7674a must be 0x20 or have bit0 set, else no-op (return pos unchanged) */
    uint8_t mode = G8(VA_g_player_movement_enabled);
    if (mode != 0x20 && (mode & 1) == 0)
        return ebx_in;

    int32_t ceil_lim = (int32_t)edx_in - 0x1c;  /* [ebp-4] = the usable ceiling */
    uint32_t edi = (uint16_t)G16(VA_g_player_height);      /* DI = g_player_height (low 16 live) */

    if ((int32_t)G32(VA_g_player_health) <= 0)             /* health<=0 -> forced crouch */
        key_z_crouch();

    if (ceil_lim - floor_z < 0x70)              /* cramped headroom -> set flags 0x04|0x20 */
        G8(VA_g_player_locomotion_flags) |= 0x24;

    /* "may begin/continue a jump-initiate" predicate read off the fall-state machine */
    int can_jump = 0;
    if (G32(VA_g_player_airborne + 0x4) == 0)      can_jump = 1;
    else if (G32(VA_g_player_airborne + 0x4) == 1) can_jump = (G32(VA_g_player_airborne + 0x10) < 5);
    else if (G32(VA_g_player_airborne + 0x4) == 2) can_jump = 1;

    if (can_jump && (G8(VA_g_player_locomotion_flags) & 0x10)) {     /* jump-initiate bit pressed */
        G8(VA_g_player_locomotion_flags) -= 0x10;                     /* consume it */
        if (G32(VA_g_player_airborne + 0xc) == 0 && pos < ceil_lim &&
            G32(VA_g_player_airborne) == 0 && G32(VA_g_player_airborne + 0x4) == 0)
            G32(VA_g_player_airborne) = 1;                    /* launch: enter jump phase 1 */
    }

    if (G32(VA_g_player_airborne) == 0) {                     /* grounded -> crouch-stance settle */
        if (G8(VA_g_player_locomotion_flags) & 4) {
            G32(VA_g_player_airborne + 0xc) = 1;
            pos = approach_value(pos, floor_z, 4);
            edi = (uint32_t)approach_value(di_sx(edi), 0x30, 6);
        } else {
            G32(VA_g_player_airborne + 0xc) = 0;
        }
    }

    int32_t phase = G32(VA_g_player_airborne);
    if (phase != 0) {                            /* in jump arc -> integrate the gravity profile */
        pos += (int32_t)G32((VA_g_inventory_tab_context_map + 0xc8) + 4 * (uint32_t)phase);
        int32_t apex = pos + di_sx(edi);
        if (apex > ceil_lim) {                   /* bonk ceiling */
            pos = ceil_lim;
            G32(VA_g_player_airborne) = 0;
            G32(VA_g_player_airborne + 0x4) = -1;
            G32(VA_g_player_airborne + 0x8) = 1;
            pos -= di_sx(edi);
        } else if (phase >= 0x13) {              /* reached apex phase */
            G32(VA_g_player_airborne) = 0;
            G32(VA_g_player_airborne + 0x4) = -1;
            G32(VA_g_player_airborne + 0x8) = 1;
        } else {
            G32(VA_g_player_airborne) = phase + 1;            /* advance phase */
        }
    }

    /* fall-state detection: above floor+2 and grounded -> begin a fall (small if within +0xd, else big) */
    if (pos > floor_z + 2 && G32(VA_g_player_airborne) == 0) {
        G32(VA_g_player_airborne + 0x8) = 1;
        G32(VA_g_player_airborne + 0x4) = (pos <= floor_z + 0xd) ? 1 : 2;
    }

    if (G32(VA_g_player_airborne + 0x4) != 0) {                     /* fall handling */
        int32_t step = pos - floor_z;            /* ebx = distance above floor */
        G8(VA_g_player_locomotion_flags) &= 0xeb;                     /* clear bits 0x10|0x04 */
        if (step > 0) {                          /* still airborne above floor */
            if (step > G32(VA_g_player_airborne + 0x10)) step = G32(VA_g_player_airborne + 0x10);   /* clamp to the accumulator */
            pos -= step;
            G32(VA_g_player_airborne + 0x10)++;
            if (pos <= floor_z) {                /* landed exactly */
                edi = (uint32_t)approach_value(di_sx(edi), 0x30, step);
                pos = floor_z;
            }
        } else {                                 /* at/below floor -> landing */
            if (G32(VA_g_player_airborne + 0x8) != 0) {
                G32(VA_g_player_airborne + 0x8) = 0;
                if (G32(VA_g_player_airborne + 0x10) > 0x16) {       /* hard landing -> fall damage */
                    int32_t dmg = G32(VA_g_player_airborne + 0x10) - 0x16;
                    dmg = dmg * dmg;
                    if (dmg > 0x1f40) dmg = 0x1f40;
                    pl_bridge_eax_edx(0x32058, (uint32_t)dmg, 0);   /* apply_direct_damage_to_player */
                }
                if (G32(VA_g_player_airborne + 0x10) > 0xe && G16(VA_g_help_overlay_enabled + 0x18) != 0) {      /* landing thud */
                    uint32_t snd = (uint32_t)(uint16_t)G16(VA_g_help_overlay_enabled + 0x18) - 1;
                    pl_bridge_eax_edx(0x271fb, snd, 0);             /* start_persistent_looping_sound */
                }
            }
            int32_t v = G32(VA_g_player_airborne + 0x10);            /* [ebp-8] = landing decel */
            if (v > 8) v -= 4;
            v--;
            if (v == 0) G32(VA_g_player_airborne + 0x4) = 0;        /* settled -> clear fall state */
            pos -= v;
            if (pos < floor_z) {
                edi = (uint32_t)approach_value(di_sx(edi), 0x30, v);
                pos = floor_z;
            }
            G32(VA_g_player_airborne + 0x10) = v;
        }
    }

    /* fully settled (no fall/jump/initiate) -> ease pos toward floor + height toward its resting target */
    if ((G32(VA_g_player_airborne + 0x4) | G32(VA_g_player_airborne) | G32(VA_g_player_airborne + 0xc)) == 0) {
        pos = approach_value(pos, floor_z, 4);
        edi = (uint32_t)approach_value(di_sx(edi), G32(VA_g_player_height + 0x4), 4);
    }

    /* final clamps: keep (pos+height) inside [floor_z, ceil_lim] */
    if (di_sx(edi) + pos < floor_z) pos = floor_z;
    if (pos > ceil_lim) pos = ceil_lim;
    if (di_sx(edi) + pos > ceil_lim) {
        edi = (uint32_t)(ceil_lim - pos);
        if (di_sx(edi) < 0) {
            edi = 0;
            if (pos < floor_z) pos = floor_z;
        }
    }

    G16(VA_g_player_height) = (uint16_t)edi;
    return (uint32_t)pos;
}

/* update_player_view_bob (0x3ecfe, 228 B) — drive the vertical integrator over N sub-steps then apply the
 * walk view-bob to the camera height. EBP = pointer to the current sector's {floor:i16, ceil:i16} pair.
 * Bails if the sub-step count g32(0x85324)==0 or the floor word's high byte is the 0x80 "no-floor"
 * sentinel. Each of the N steps: pos = update_player_vertical_physics(floor, ceil, pos) [L], then nudge
 * the applied view pitch g32(0x8c108) toward -g32(0x8497c) by up to 3. Commits the final pos to the view
 * offset 0x90a92, advances the bob phase 0x8c10c, and (when moving, g_move_speed_accum 0x7e8d8 != 0)
 * looks up the bob sine table 0x724f0 scaled by speed >>4 -> 0x8c1d4, sums height+bob+pos, clamps to
 * [floor+0x18, ceil-0x18], and stores the height offset (minus pos) to 0x8c112. All bob math is 16-bit
 * (B2). Returns void (caller ignores EAX). */
void update_player_view_bob(uint32_t ebp)
{
    int32_t cnt = G32(VA_g_frame_time_scale);
    if (cnt == 0) return;                                     /* or edi,edi; je exit */

    uint16_t floor_w = *(volatile uint16_t *)(uintptr_t)(ebp + 0);
    uint16_t ceil_w  = *(volatile uint16_t *)(uintptr_t)(ebp + 2);
    int32_t floor = (int16_t)floor_w;                         /* movsx eax, word[ebp] */
    int32_t ceil  = (int16_t)ceil_w;                          /* movsx edx, word[ebp+2] */
    if (((floor_w >> 8) & 0xff) == 0x80) return;              /* cmp ah,0x80; je exit (no-floor sentinel) */

    int32_t esi = -(int32_t)G32(VA_g_view_floor_clearance);                     /* approach target for the applied pitch */
    int32_t ebx = (int16_t)G16(VA_g_player_z);                      /* pos seed = sx(view offset) */

    do {                                                      /* dec edi; jg loop */
        ebx = (int32_t)update_player_vertical_physics((uint32_t)floor, (uint32_t)ceil, (uint32_t)ebx);
        int32_t diff = (int32_t)G32(VA_g_view_pitch_applied) - esi;           /* mov eax,[0x8c108]; sub eax,esi */
        if (diff != 0) {                                      /* je skip */
            if (diff < 0) { if (diff < -3) diff = -3; }       /* js; cmp eax,-3; jg keep; mov eax,-3 */
            else          { if (diff >= 3) diff =  3; }       /* cmp eax,3; jl keep; mov eax,3 */
            G32(VA_g_view_pitch_applied) -= diff;                             /* sub [0x8c108], eax */
        }
    } while (--cnt > 0);

    G16(VA_g_player_z) = (uint16_t)ebx;                             /* commit the view offset */
    int32_t si = (int16_t)G16(VA_g_player_height);                       /* g_player_height (post-physics) */
    uint32_t phase = (uint32_t)G32(VA_g_frame_time_scale) + (uint32_t)G32(VA_g_view_pitch_applied + 0x4);
    G32(VA_g_view_pitch_applied + 0x4) = (int32_t)phase;                            /* advance the bob phase accumulator */

    if (G32(VA_g_move_speed_accum) != 0) {                                  /* moving -> apply bob */
        uint32_t idx = ((phase * 3u) >> 1) & 0x3e;            /* lea edi,[edi+edi*2]; shr edi,1; and 0x3e */
        int16_t cx = (int16_t)G16(VA_g_player_walk_bob_table + idx);             /* bob sine table */
        cx = (int16_t)(cx * (int16_t)G16(VA_g_move_speed_accum));           /* imul cx, g_move_speed_accum (16-bit) */
        cx = (int16_t)(cx >> 4);                              /* sar cx, 4 (signed) */
        G16(VA_g_collision_sector_stack + 0x40) = (uint16_t)cx;
        si = (int16_t)(si + cx);                              /* add si, cx (16-bit) */
        si = (int16_t)(si + (int16_t)ebx);                    /* add si, bx */
        int16_t lo = (int16_t)(floor + 0x18);                 /* add ax, 0x18 */
        int16_t hi = (int16_t)(ceil  - 0x18);                 /* sub dx, 0x18 */
        if ((int16_t)si <= lo) si = lo;                       /* cmp si,ax; jg keep; mov si,ax */
        if ((int16_t)si >= hi) si = hi;                       /* cmp si,dx; jl keep; mov si,dx */
        si = (int16_t)(si - (int16_t)ebx);                    /* sub si, bx */
    }
    G16(VA_g_player_height + 0x2) = (uint16_t)si;                              /* height offset for the camera */
}

/* ====================================================================== L3 — movement hub + damage flash */

/* clear_damage_flash (0x179d2, 28 B) — if the damage-flash / pain accumulator 0x89f3b is nonzero, clear
 * it and refresh the palette DAC to drop the red hit-flash; otherwise no-op (the original `je` targets a
 * bare shared `ret` at 0x1792b). refresh_palette_dac 0x2ff38 (video_display, DAC hardware) -> direct C
 * (the DAC `out` loop no-ops via NULL g_os_port_out).
 * The pain accumulator is the value apply_direct_damage_to_player accumulates into 0x89f3b. */
void clear_damage_flash(void)
{
    if (G32(VA_g_damage_flash_level) == 0) return;          /* je 0x1792b (a bare ret) */
    G32(VA_g_damage_flash_level) = 0;
    refresh_palette_dac();            /* 0x2ff38 re-pointed: real body over the low-mem
                                              * palette buffer ([0x90bca]<<4). The DAC `out 0x3c8/0x3c9`
                                              * loop routes through g_os_port_out (NULL in the oracle ->
                                              * no-op); the just-cleared counter
                                              * [0x89f3b]==0 picks the simple upload path (no obj3 writes). */
}

/* update_player_movement (0x1035a, 40 B) — the per-frame movement sequencer. Wrapped in pushal/popal so
 * it has no own register effect; its body is just five calls in order: commit the turn/view-scale + the
 * view transform (both player [L]) around the world-collision resolve, the ambient render/map tick, and
 * the nearby-SFX emit (all cross-subsystem bridges). The only register setup is EDX=0 before the collision
 * resolve. Player-owned content = the two [L] commits; the three bridges are verified in their own
 * subsystems + in-game. */
void update_player_movement(void)
{
    update_turn_view_scale();        /* 0x3e22c [L] */
    move_player_with_collision(0);    /* 0x3e796 re-pointed: query_ctx=EDX=0 (caller `sub
                                              * edx,edx`). Real body over the geometry ptr [0x90aa8] + the
                                              * velocity queues; the player oracle ports test_collision's
                                              * 2-sector map + selectors + walker globals and stages an
                                              * EMPTY velocity queue (one no-move pass, walker inert). */
    update_view_transform_params();  /* 0x3e2ba [L] */
    tick_ambient_render_and_map();   /* 0x10382 [L] (game_core; render sub-bridges 0x287b6/0x10dce stubbed in the player oracle) */
    play_nearby_sfx_emitters();       /* 0x151c9 re-pointed: real body. The player oracle
                                              * stages the emitter list [0x85c44] to a zero-count arena so
                                              * query_sfx_emitters_in_range takes the no-emitter branch
                                              * (writes only [0x911dc]/[0x911e0], skips the SOS group-play);
                                              * full behaviour verified in test_audio (stage_play_env). */
}

/* ====================================================================== L4 — per-frame player tick */

/* sincos/bob table at 0x72080 (signed word entries; index masked to &0x1ff) */
#define SINCOS16(i) ((int32_t)(int16_t)G16(VA_g_sincos_table + (uint32_t)(i) * 2))

/* update_player_tick (0x1729c, the per-frame player hub) — runs movement + viewport blit, decays the
 * action timer, drives the weapon fire/cooldown/reload state machine (DAT_7fddc anim-state 1/2/3 +
 * DAT_7fdd0 fire-lock + DAT_7fdd8/7fdfc rate; gated on g_player_health>0), fires queued shots, uses/
 * cycles weapons + held items, manages the cached first-person weapon sprite (g@0x7f574), and draws the
 * viewmodel. Watcom void(void) — saves EBX/ECX/EDX/ESI + a 4-byte local (ebpm4). The corpus 1300-byte
 * "size" is a multi-entry artifact; the real body lives at 0x16e03..0x17296 which the 0x1729c entry tail-
 * jumps into (transcribed here with goto labels mirroring the disasm). g_active_weapon_ptr (g@0x7fe00) is
 * a STORED runtime pointer into the weapon-attrs array g@0x811b4 (stride 0x50); deref RAW (A4). Callees:
 * update_player_movement / halve_eax_if_90bd4 / rng_range / pool_free_handle are [L]; the rest bridge.
 *
 * LIVE-SWAP target (non-idempotent: fires weapons, uses items, frees handles, blits). The deterministic
 * state-machine portion is oracle-checked with the non-idempotent bridges stubbed (test_player.c);
 * full-behaviour confirmation is the in-game ROTH_LIFT=update_player_tick run. */
void update_player_tick(void)
{
    update_player_movement();                 /* 0x1035a [L] */
    blit_scaled_viewport_to_framebuffer();     /* 0x2db40 re-pointed: real body. The player
                                                       * oracle drives display-type [0x90bd4]=0 (turn disabled
                                                       * so update_turn_view_scale leaves it 0), i.e. the
                                                       * no-scale branch = dispatch + add_dirty_rect (lifted,
                                                       * pure obj3); the fb-scaling modes are covered in
                                                       * test_blit_2d. No VGA/port writes on any path. */

    int32_t ebpm4 = 0;                                /* [ebp-4] */
    uint8_t *esi;                                     /* the live weapon-record pointer */
    int32_t fl;                                       /* DAT_7fdd0 fire-lock (reloaded as needed) */

    if (G8(VA_g_render_sector_walk_mode + 0x23) != 0) {                           /* action timer decay */
        int32_t v = (int32_t)(uint32_t)(uint8_t)G8(VA_g_render_sector_walk_mode + 0x23) - (int32_t)G32(VA_g_frame_time_scale);
        if (v < 0) v = 0;
        G8(VA_g_render_sector_walk_mode + 0x23) = (uint8_t)v;
    }
    if (G32(VA_g_weapon_fire_lock + 0xc) == 0) return;                    /* not animating -> done */
    if ((int32_t)G32(VA_g_player_health) <= 0) return;           /* dead -> done */

    if ((uint32_t)G32(VA_g_weapon_fire_lock + 0x20) <= 0x578)              /* 0x16e03/0x172e1 */
        G32(VA_g_weapon_fire_lock + 0x20) += G32(VA_g_frame_time_scale);
    else if (G32(VA_g_weapon_fire_lock + 0xc) == 3) { G32(VA_g_weapon_fire_lock + 0x10) = 0; G32(VA_g_weapon_fire_lock + 0xc) = 1; }

    {
        int32_t step = G32(VA_g_frame_time_scale) << 2;             /* 0x16e0e: fts*4 */
        if (G32(VA_g_weapon_fire_lock + 0xc) == 2) {                      /* lowering */
            G32(VA_g_weapon_fire_lock + 0x10) -= step;
            if ((int32_t)G32(VA_g_weapon_fire_lock + 0x10) < 0) { G32(VA_g_weapon_fire_lock + 0x10) = 0; G32(VA_g_weapon_fire_lock + 0xc) = 3; }
        } else if (G32(VA_g_weapon_fire_lock + 0xc) == 1) {               /* raising */
            G32(VA_g_weapon_fire_lock + 0x10) += step;
            if ((int32_t)G32(VA_g_weapon_fire_lock + 0x10) > 0x64) G32(VA_g_weapon_fire_lock + 0xc) = 0;
        }
    }

    esi = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_active_weapon_ptr);          /* 0x16e66 */
    if (esi == NULL) { esi = (uint8_t *)(uintptr_t)GADDR(VA_g_active_weapon_attrs); G32(VA_g_active_weapon_ptr) = (int32_t)(uintptr_t)esi; }
    if (G32(VA_g_pending_weapon_def + 0x8) == 0) G32(VA_g_pending_weapon_def + 0x8) = 8;          /* 0x16e7b: default fire-rate */

    if (G32(VA_g_weapon_fire_lock) == 0) goto LAB_1709b;            /* 0x16e8e: not firing */
    if (G32(VA_g_weapon_fire_lock) != -1) goto LAB_16f51;           /* 0x16e9b: cooling down */

    /* 0x16ea8 — fire-lock == -1: trigger just pulled, arm the shot */
    G32(VA_g_weapon_fire_lock) = 1;
    G32(VA_g_weapon_fire_lock + 0x4) = 0;
    G32(VA_g_active_weapon_ptr + 0x4) = 0x3e8;
    G32(VA_g_pending_fire_aim + 0x10) = 1;
    G16(VA_g_weapon_fire_lock + 0x8) = (uint16_t)((uint16_t)G16(VA_g_pending_weapon_def + 0x8) + (uint16_t)G16(VA_g_frame_time_scale));
    G32(VA_g_pending_fire_aim + 0xc) = 0;
    if (G32(VA_g_dev_mode_flag + 0x8) != 0) {                          /* mouse-aim horizontal lead */
        uint32_t e = halve_eax_if_90bd4((uint32_t)G32(VA_g_mouse_x));
        int32_t t = ((int32_t)e - (int32_t)G32(VA_g_view_x)) * 0x140;
        int32_t bx = (int16_t)G16(VA_g_render_width);
        G32(VA_g_pending_fire_aim + 0xc) = t / bx - 0xa0;                 /* cdq; idiv (signed) */
    }
    fl = G32(VA_g_weapon_fire_lock);
    if ((uint32_t)fl >= *(volatile uint32_t *)(esi + 0xc) && G32(VA_g_weapon_fire_lock + 0x4) == 0) {
        pl_bridge5(0x16da2, (uint32_t)(uintptr_t)esi, 0, 0, 0, 0);   /* fire_pending_weapon_shot(esi) */
        G32(VA_g_weapon_fire_lock + 0x4) = 1;
    }
    goto LAB_1709b;

LAB_16f51:                                            /* 0x16f51 — cooling down: advance the lock */
    {
        uint32_t a = (uint32_t)(int32_t)(int16_t)G16(VA_g_weapon_fire_lock + 0x8);       /* movsx then unsigned div */
        G32(VA_g_weapon_fire_lock) = (int32_t)(a / (uint32_t)G32(VA_g_pending_weapon_def + 0x8));
        G16(VA_g_weapon_fire_lock + 0x8) = (uint16_t)((uint16_t)G16(VA_g_weapon_fire_lock + 0x8) + (uint16_t)G16(VA_g_frame_time_scale));
    }
    fl = G32(VA_g_weapon_fire_lock);
    if ((uint32_t)fl >= *(volatile uint32_t *)(esi + 0xc) && G32(VA_g_weapon_fire_lock + 0x4) == 0) {
        pl_bridge5(0x16da2, (uint32_t)(uintptr_t)esi, 0, 0, 0, 0);
        G32(VA_g_weapon_fire_lock + 0x4) = 1;
    }
    if ((uint32_t)G32(VA_g_weapon_fire_lock) < (uint32_t)G32(VA_g_active_weapon_ptr + 0x4)) goto LAB_1709b;   /* 0x16f98 */
    G32(VA_g_weapon_fire_lock) = 0;                                 /* shot cycle complete */

    if (G32(VA_g_pending_weapon_def) != 0 &&
        *(volatile uint32_t *)(esi + 0x38) == *(volatile uint32_t *)(esi + 8)) {   /* 0x16fbc */
        if (G32(VA_g_selected_item_secondary) != 0) {                      /* consume the held secondary item, revert weapon */
            pl_bridge5(0x1d0fd, (uint32_t)G32(VA_g_selected_item_secondary), 0, 0, 0, 0);     /* consume_held_item */
            esi = (uint8_t *)(uintptr_t)GADDR(VA_g_active_weapon_attrs);
            pl_bridge5(0x184ab, 0, 0, 0, 0, (uint32_t)(uintptr_t)esi);   /* activate_weapon_item(0,0) */
            G32(VA_g_active_weapon_ptr) = (int32_t)(uintptr_t)esi;
        }
        if (G32(VA_g_weapon_fire_lock + 0xc) == 3) { G32(VA_g_weapon_fire_lock + 0x10) = 0; G32(VA_g_weapon_fire_lock + 0xc) = 1; }   /* 0x16feb */
    }

    /* 0x17008 — weapon cycle/selection */
    if ((uint32_t)G32(VA_g_active_weapon_ammo_id + 0x110) > 1) {
        int32_t b8 = G32(VA_g_active_weapon_attrs + 0x4);
        if (b8 == 0) {                                /* random pick from all */
            uint32_t r = rng_range((uint32_t)G32(VA_g_active_weapon_ammo_id + 0x110));
            esi = (uint8_t *)(uintptr_t)(GADDR(VA_g_active_weapon_attrs) + r * 0x50);     /* 0x17074 */
        } else if (esi != (uint8_t *)(uintptr_t)GADDR(VA_g_active_weapon_attrs)) {        /* 0x1702a (else keep esi) */
            int32_t ecx = G32(VA_g_active_weapon_ammo_id + 0x110) - 1;
            if (b8 < 0) {                             /* 0x17039 */
                if (-b8 <= (int32_t)G32(VA_g_player_health + 0x4)) {
                    esi = (uint8_t *)(uintptr_t)GADDR(VA_g_active_weapon_attrs);          /* 0x17047 */
                } else {
                    uint32_t r = rng_range((uint32_t)ecx) + 1;    /* 0x1704e */
                    esi = (uint8_t *)(uintptr_t)(GADDR(VA_g_active_weapon_attrs) + r * 0x50);
                }
            } else {                                  /* 0x17058 */
                uint8_t *p = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_selected_item_secondary);
                if (*(volatile int16_t *)(p + 2) == 0 &&
                    esi != (uint8_t *)(uintptr_t)GADDR(VA_g_active_weapon_attrs)) {
                    uint32_t r = rng_range((uint32_t)ecx) + 1;    /* 0x1706c */
                    esi = (uint8_t *)(uintptr_t)(GADDR(VA_g_active_weapon_attrs) + r * 0x50);
                }
            }
        }
    }
    G32(VA_g_active_weapon_ptr) = (int32_t)(uintptr_t)esi;           /* 0x1707e */
    G32(VA_g_pending_weapon_def + 0x4) = *(volatile int32_t *)(esi + 0x00); /* 0x17084 */
    G32(VA_g_weapon_fire_lock + 0x18) = *(volatile int32_t *)(esi + 0x20);
    G32(VA_g_weapon_fire_lock + 0x1c) = *(volatile int32_t *)(esi + 0x1c);

LAB_1709b:                                            /* 0x1709b — viewmodel / sprite */
    ;
    if (G32(VA_g_weapon_fire_lock) == 0)
        G32(VA_g_corner_icon_saveunder + 0x10) += G32(VA_g_frame_time_scale);                 /* advance bob phase when idle */
    int32_t ebx = SINCOS16(((uint32_t)G32(VA_g_corner_icon_saveunder + 0x10) << 2) & 0x1ff);
    ebpm4 = SINCOS16(((uint32_t)G32(VA_g_corner_icon_saveunder + 0x10) << 3) & 0x1ff) >> 0xc;
    ebx >>= 0xb;                                       /* 0x170e4 (signed) */

    if (G32(VA_g_pending_weapon_def + 0x4) != G32(VA_g_weapon_fire_lock + 0x14)) {               /* sprite changed -> free the old cached handle */
        G32(VA_g_weapon_fire_lock + 0x14) = G32(VA_g_pending_weapon_def + 0x4);
        if (G32(VA_g_cached_dbase200_sprite_handle) != 0) {
            pool_free_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle),
                                    (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_cached_dbase200_sprite_handle));
            G32(VA_g_cached_dbase200_sprite_handle) = 0;
        }
    }
    if (G32(VA_g_pending_weapon_def + 0x4) == 0) goto LAB_epilogue;         /* 0x17117 */
    if (G32(VA_g_cached_dbase200_sprite_handle) == 0) {                          /* 0x1711e: load the sprite if not cached */
        pl_bridge5(0x16087, (uint32_t)G32(VA_g_pending_weapon_def + 0x4), 0, 0, 0, 0);        /* load_dbase200_sprite_cached */
        if (G32(VA_g_cached_dbase200_sprite_handle) == 0) goto LAB_epilogue;
    }
    /* 0x17144: validate the cached sprite matches the active palette/def, else reload */
    if (*(volatile int32_t *)(uintptr_t)*(volatile int32_t *)(uintptr_t)(uint32_t)G32(VA_g_cached_dbase200_sprite_handle)
            != (int32_t)G32(VA_g_das_render_scale + 0x8)) {
        pl_bridge5(0x16087, (uint32_t)G32(VA_g_pending_weapon_def + 0x4), 0, 0, 0, 0);
        if (G32(VA_g_cached_dbase200_sprite_handle) == 0) goto LAB_epilogue;
    }

    /* 0x1716c: horizontal offset (mouse lead when the weapon tracks aim) */
    {
        int32_t hx = 0;
        if (*(volatile uint8_t *)(esi + 0x4c) & 1) {
            if (G32(VA_g_pending_fire_aim + 0x10) != 0) {
                hx = G32(VA_g_pending_fire_aim + 0xc);
            } else if (G32(VA_g_dev_mode_flag + 0x8) != 0) {
                uint32_t e = halve_eax_if_90bd4((uint32_t)G32(VA_g_mouse_x));
                int32_t t = ((int32_t)e - (int32_t)G32(VA_g_view_x)) * 0x140;
                int32_t cx = (int16_t)G16(VA_g_render_width);
                hx = t / cx - 0xa0;
            }
        }
        G32(VA_g_pending_fire_aim + 0x8) += (hx - (int32_t)G32(VA_g_pending_fire_aim + 0x8)) >> 1;    /* smooth toward hx */
        ebx += G32(VA_g_pending_fire_aim + 0x8);                          /* 0x171c6 */
        ebx += G32(VA_g_weapon_fire_lock + 0x18);                          /* 0x171d2 */
    }

    {
        uint32_t handle = (uint32_t)G32(VA_g_cached_dbase200_sprite_handle);
        int32_t hbase = *(volatile int32_t *)(uintptr_t)handle;          /* *handle (the value drawn) */
        int32_t flags = *(volatile uint8_t *)(esi + 0x4c);
        int32_t ecx_v = (int32_t)G32(VA_g_weapon_fire_lock + 0x10) + (int32_t)G32(VA_g_weapon_fire_lock + 0x1c);   /* 0x171c0/0x171cc */
        int32_t lock = G32(VA_g_weapon_fire_lock);
        int32_t vbob = ((int32_t)(int16_t)G16(VA_g_collision_sector_stack + 0x40) >> 1) + 0x20 + ebpm4 + (int32_t)G32(VA_g_weapon_fire_lock + 0x10);

        if (lock != 0) {                              /* 0x171d8: firing */
            G32(VA_g_active_weapon_ptr + 0x4) = (int16_t)*(volatile int16_t *)(uintptr_t)(hbase + 0xc);
            if (flags & 2) {                          /* 0x171f8 */
                pl_bridge5(0x139a0, (uint32_t)hbase, (uint32_t)lock, (uint32_t)G32(VA_g_weapon_fire_lock + 0x18), (uint32_t)ecx_v, 0);
            } else {                                  /* 0x17210 */
                int32_t ecx2 = (int32_t)G32(VA_g_weapon_fire_lock + 0x1c) + vbob;
                pl_bridge5(0x139a0, (uint32_t)hbase, (uint32_t)lock, (uint32_t)ebx, (uint32_t)ecx2, 0);
            }
            goto LAB_epilogue;
        }
        /* 0x17237: idle/ready */
        if (flags & 2) {                              /* 0x1723d */
            pl_bridge5(0x139a0, (uint32_t)hbase, 0, (uint32_t)G32(VA_g_weapon_fire_lock + 0x18), (uint32_t)ecx_v, 0);
        } else {                                      /* 0x1724a */
            int32_t ecx2 = (int32_t)G32(VA_g_weapon_fire_lock + 0x1c) + vbob;
            pl_bridge5(0x139a0, (uint32_t)hbase, 0, (uint32_t)ebx, (uint32_t)ecx2, 0);
        }
        G32(VA_g_pending_fire_aim + 0x10) = 0;                             /* 0x17275 */
        G32(VA_g_pending_weapon_def + 0x4) = *(volatile int32_t *)(esi + 0x00);
        G32(VA_g_weapon_fire_lock + 0x18) = *(volatile int32_t *)(esi + 0x20);
        G32(VA_g_weapon_fire_lock + 0x1c) = *(volatile int32_t *)(esi + 0x1c);
    }

LAB_epilogue:
    return;
}
