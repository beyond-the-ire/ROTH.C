/* lift_input.c — verified-C lifts for the `input` subsystem (keyboard ISR + scancode ring,
 * keymap dispatch + per-key handlers, mouse polling, cursor world use/examine, console field
 * editing). Analysis: docs/reference/lift/input.md.
 *
 * Cluster layout (bottom-up):
 *   A. keyboard ISR + scancode ring     — this file, top
 *   B. keymap + key handlers            — follows A
 *   C. mouse                            — follows B
 *   D. cursor world use/examine         — follows C
 *   E. console field editing            — follows D
 *
 * Shared input state (canon VAs, derived from the disasm):
 *   scancode ring: buffer 0x90c1c, write-head u16 0x7e91c, read-tail u16 0x7e91e, mask u16 0x7e91a
 *   held-key bitmap: 16 bytes @ 0x90c3c, byte = (scancode&0x7f)>>3, bit = bittable[scancode&7]
 *   bit table: 8 bytes @ 0x707e9 = {1,2,4,8,0x10,0x20,0x40,0x80}
 *   held-key action table @ 0x707f1: 5-byte entries {u8 scancode, u32 handler (runtime ptr)}, 0-term
 *   scancode->ASCII translate table @ 0x7082e: 3-byte entries {u8 sc, u8 ch, u8 shifted}, 0xff-term
 */
#include "common.h"

/* ===================== A. keyboard ISR + scancode ring ===================== */

/* keybit_mask_for (0x12976, 36B): AL = scancode; test the key's bit in the held-key bitmap.
 * Output = ZF (set => key NOT held); clobbers AX (ah=0, al=bit mask) — the sole caller
 * (dispatch_held_key_actions) consumes only ZF and reloads AL, so the ABI_ZF adapter's
 * GP-preserve is safe. EBX/CX preserved by push/pop. Returns nonzero => ZF set (ABI_ZF). */
int keybit_mask_for(uint32_t eax)
{
    uint32_t sc   = eax & 0xff;                       /* xor ah,ah; movzx ebx,ax */
    uint8_t  held = G8(VA_g_key_state_bitmap + ((sc & 0x7f) >> 3)); /* and bx,0x7f; shr bx,3 */
    uint8_t  mask = G8(VA_g_keybit_mask_table + (sc & 7));           /* and al,7; xlatb */
    return (uint8_t)(held & mask) == 0;               /* test cl,al -> ZF */
}

/* dequeue_translated_key (0x129ca, 61B): pop one scancode off the ring (via input_ring_dequeue
 * 0x1299a [L], whose `test ax,0xff` sets ZF=(al==0)) and translate it through the 3-byte
 * {sc,ch,shifted} table @0x7082e; if the RShift held-bit ([0x90c42]&0x40 — the ISR folds LShift
 * onto RShift 0x36) is set, take the shifted byte (`mov al,ah`). Outputs: CF=1 + EAX=translated
 * on a hit; CF=0 + AL=0 on empty ring / unmapped code (AH=0 from the ring dequeue's `xor ax,ax`,
 * upper 16 bits of the caller's EAX preserved). EBX clobbered (table cursor); EDI/ESI/ECX/EDX
 * push/pop-preserved. C shape: *eax_io in/out, returns CF. */
uint32_t dequeue_translated_key(uint32_t *eax_io)
{
    uint8_t al = input_ring_dequeue();         /* call 0x1299a; je empty */
    if (al != 0) {
        uint32_t t = 0x7082e;                         /* mov ebx,0x7082e */
        while (G8(t) != 0xff) {                       /* cmp byte [ebx],0xff */
            if (G8(t) == al) {                        /* cmp al,[ebx]; je found */
                uint32_t ax = G16(t + 1);             /* sub eax,eax; mov ax,[ebx+1] */
                if (G8(VA_g_key_modifier_flags) & 0x40)               /* RShift held-bit */
                    ax = (ax & 0xff00u) | ((ax >> 8) & 0xff);  /* mov al,ah */
                *eax_io = ax;
                return 1;                             /* stc */
            }
            t += 3;
        }
    }
    *eax_io &= 0xffff0000u;                           /* ah=0 (ring path), sub al,al */
    return 0;                                         /* clc */
}

#ifdef ROTH_STANDALONE
/* The two keybind code-ptr tables — held-key @0x707f1
 * (12 entries) and keymap @0x7093d (37 entries), both {u8 sc, u32 handler} obj3 DATA with no
 * runtime writers — dispatch RELOCATED RUNTIME pointers. All 49 handler values enumerated from
 * the obj3 image and routed here; every routed body is a void(void) register-preserving keybind
 * intent except the SPACE door-opener (selector-base params per its lift_doors.c body comment).
 * Unknown values fail loud. */

/* escape_menu_prompt (0x262f9; an UNCORPUSED body after apply_audio_volume_settings — disasm
 * 0x262f9-0x26329): re-entry guard [0x76758] -> return 1; else show_message_box(GADDR(0x71a00),
 * flags=0x200) (the lifted INTERACTIVE menu frame) -> return 1 iff the box returned 1. */
static uint32_t in_if_escape_menu_prompt(void)
{
    if (G16(VA_g_player_movement_enabled + 0xe) != 0) return 1;                        /* 0x262fd already in the box */
    uint32_t r = show_message_box(0x71a00u + OBJ_DELTA, 0x200);   /* 0x2630f/14/19 call 0x2508f */
    return (r == 1) ? 1u : 0u;                              /* 0x2631e cmp eax,1; ebx threading */
}

/* select_weapon_by_number (0x1beca; the UNCORPUSED multi-entry body after reset_weapon_hud —
 * keys '2'..'6' enter at 0x1bea9..0x1bec5 with eax=1..5; disasm-transcribed 0x1beca-0x1bf7a,
 * exit = the 0x18a23 shared Watcom epilogue = plain return). Walks the inventory slot array
 * (word ids @0x80c30), counting down [0x80c2c] non-empty entries in eax (id==0 slots skip
 * WITHOUT consuming the count — 0x1bef2 jumps past the 0x1bf6a dec), finds the Nth category-1
 * (weapon) record and equips it. */
static void in_if_select_weapon_by_number(uint32_t n)
{
    uint32_t cnt = (uint32_t)G32(VA_g_inventory_count);                  /* 0x1bed5 non-empty entry count */
    uint32_t sc  = 0x80c30u;                                /* canon slot cursor (ebx = 0x480c30) */
    uint32_t ecx = n;
    if (cnt == 0) return;                                   /* 0x1bedf/0x1bee1 */
    for (;;) {
        int32_t id = (int32_t)(int16_t)G16(sc);             /* 0x1beed movsx edx,[ebx] */
        if (id != 0) {                                      /* 0x1bef0 (empty: skip, NO count-down) */
            if (id < 0) goto next_dec;                      /* 0x1bef8 test dh,0x80 */
            uint32_t off = *(volatile uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_dbase100_inventory_table) + (uint32_t)id * 4u);  /* 0x1befd-0x1bf04 */
            if (off == 0) goto next_dec;                    /* 0x1bf09 */
            uint32_t r4  = *(volatile uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_dbase100_base) + off + 4u);  /* 0x1bf0d-16 g_dbase100_base */
            if (((r4 >> 8) & 0xfu) != 1u) goto next_dec;    /* 0x1bf19-25 category nibble != weapon */
            if (--ecx != 0) goto next_dec;                  /* 0x1bf27/29 not the Nth yet */
            {   uint32_t sp = sc + OBJ_DELTA;               /* ebx: the runtime slot POINTER */
                if (sp == (uint32_t)G32(VA_g_selected_item_secondary)) return;   /* 0x1bf2b already equipped -> 0x18a23 */
                if (sp == (uint32_t)G32(VA_g_selected_item_primary)) {         /* 0x1bf37 the selected item */
                    G32(VA_g_selected_item_primary) = 0;                       /* 0x1bf3f (ecx == 0 here) */
                    update_selected_item_icon();     /* 0x1bf45 call 0x1bb4b */
                }
                reset_weapon_fire_timing();          /* 0x1bf4a call 0x1765c */
                activate_weapon_item(sp, (uint32_t)id);        /* 0x1bf4f/51 eax=slot ptr, edx=id */
                render_weapon_hud(1, 0x811b4u + OBJ_DELTA);    /* 0x1bf56-60 (1, GADDR(0x811b4)) */
                return;                                     /* 0x1bf65 jmp 0x18a23 */
            }
        next_dec:
            cnt--;                                          /* 0x1bf6a dec eax */
        }
        sc += 4;                                            /* 0x1bf6b add ebx,4 */
        if (cnt == 0) return;                               /* 0x1bf6e/70 test eax,eax; ja loop */
    }
}

static void in_if_key_handler(uint32_t canon)
{
    switch (canon) {
    /* --- held-key table @0x707f1 (per-frame movement/look intents) --- */
    case 0x12668u: move_input_backward();       return;
    case 0x12686u: move_input_forward();        return;
    case 0x12703u: turn_input_right();          return;
    case 0x126b6u: turn_input_left();           return;
    case 0x126adu: move_input_strafe_left();    return;
    case 0x126a4u: move_input_strafe_right();   return;
    case 0x1294bu: look_pitch_recenter_down();  return;
    case 0x1296fu: look_pitch_recenter_up();    return;
    case 0x12927u: look_pitch_up();             return;
    case 0x12939u: look_pitch_down();           return;
    case 0x1c5f9u: key_a_jump();                return;
    case 0x1c5d0u: key_z_crouch();              return;
    /* --- keymap table @0x7093d (ring-drained key actions) --- */
    case 0x17fedu: key_toggle_sprint();         return;
    case 0x175d8u: key_toggle_weapon_overlay(); return;
    case 0x1f89bu: key_toggle_subtitles();      return;
    case 0x1801cu: key_toggle_easy_select();    return;
    case 0x17fd2u: key_toggle_mouse_swap();     return;
    case 0x14794u: key_cycle_display_type();    return;
    case 0x17fd1u: key_f7_noop();               return;
    case 0x14ce5u: key_quicksave();             return;
    case 0x14cc9u: key_quickload();             return;
    case 0x14c9cu: key_show_version();          return;
    case 0x14601u: key_gamma_increase();        return;
    case 0x145ecu: key_gamma_decrease();        return;
    case 0x11124u: check_snapshot_key();        return;
    case 0x14573u: key_set_view_scale();        return;
    case 0x14678u: key_view_size_decrease();    return;
    case 0x14613u: key_view_size_increase();    return;
    case 0x145a4u: key_n_vestigial();           return;
    case 0x144fbu: key_flush_texture_cache();   return;
    case 0x147a8u: key_cycle_display_type_alt(); return;
    case 0x1444cu: key_toggle_wireframe_map();  return;
    case 0x17fa4u: dev_toggle_map_menu();       return;
    case 0x17fc0u: key_toggle_help_overlay();   return;
    case 0x173f4u: use_enter_key_handler();     return;
    case 0x14ca7u: key_t_handler_vestigial();   return;
    case 0x144dau: key_m_toggle_render_mode();  return;
    case 0x1be8eu: reset_weapon_hud();          return;
    case 0x1a132u: update_ui_overlay();         return;
    case 0x14cb6u: key_fire_weapon();           return;
    case 0x146bau:                        /* Down in choice mode: choice_select_next wrapper — an
                                           * UNCORPUSED 15B appendage after key_view_size_decrease
                                           * (disasm 0x146ba-0x146c8: cmp [0x83aea],0; je ret; call 0x1fc16) */
        if (G32(VA_g_dialogue_busy_flag) != 0) choice_select_next();
        return;
    case 0x146c9u:                        /* Up in choice mode: choice_select_prev (disasm 0x146c9-0x146d7) */
        if (G32(VA_g_dialogue_busy_flag) != 0) choice_select_prev();
        return;
    case 0x145aeu:                        /* Esc: the quit/main-menu prompt (uncorpused appendage after
                                           * key_n_vestigial; disasm 0x145ae-0x145d2) */
        if (G8(VA_g_map_menu_marker_normal + 0x57) == 0 && in_if_escape_menu_prompt() != 0) {
            G16(VA_g_screen_resolution_index + 0x8) = 0xffff;                          /* 0x145c0 */
            G16(VA_g_roth_error_code) = 0;                               /* 0x145c9 */
        }
        return;
    case 0x1bea9u: in_if_select_weapon_by_number(1); return;   /* key '2' (eax=1 mid-entry) */
    case 0x1beb0u: in_if_select_weapon_by_number(2); return;   /* key '3' */
    case 0x1beb7u: in_if_select_weapon_by_number(3); return;   /* key '4' */
    case 0x1bebeu: in_if_select_weapon_by_number(4); return;   /* key '5' */
    case 0x1bec5u: in_if_select_weapon_by_number(5); return;   /* key '6' */
    case 0x3df96u: {                      /* SPACE: open_nearest_door — the body loads walk FS/GS from
                                           * [0x90be8]/[0x90bec] and spawn FS/GS from [0x852c8]/[0x852cc]
                                           * (lift_doors.c body comment); resolved here, the reflection-
                                           * leaf pattern (technique i) */
        extern uint32_t (*g_os_sel_base)(uint16_t);
        dev_open_nearest_door(g_os_sel_base((uint16_t)G16(VA_g_geometry_selector)),
                                     g_os_sel_base((uint16_t)G16(VA_g_geometry_selector + 0x4)),
                                     g_os_sel_base((uint16_t)G16(VA_g_surface_record_selector)),
                                     g_os_sel_base((uint16_t)G16(VA_g_vertex_selector)));
        return; }
    default: break;
    }
    roth_unreachable(canon);   /* an unknown table value — all 49 enumerated handlers are cased above */
}
#endif

/* dispatch_held_key_actions (0x128fb, 44B): per-frame held-key scan. Clears the 0x819c0 action
 * bits (clear_819c0_bits 0x1c5c8 [L] — direct C), then walks the {u8 sc, u32 handler} table
 * @0x707f1; for each entry whose key tests held (keybit_mask_for ZF clear), zeroes [0x76858]
 * and dispatches the stored handler pointer. The table entries are RELOCATED RUNTIME code
 * pointers (gotcha A4) — called via call_orig, not deref'd through GADDR. The original passes
 * the scancode still in AL into the handler (all current table targets are void movement
 * intents, but the register state is reproduced anyway). */
void dispatch_held_key_actions(void)
{
    clear_819c0_bits();                        /* call 0x1c5c8 [L] */
    for (uint32_t e = 0x707f1; ; e += 5) {            /* mov ebx,0x707f1 */
        uint8_t al = G8(e);
        if (al == 0) return;                          /* or al,al; je done */
        if (!keybit_mask_for(al)) {            /* call 0x12976; je skip (ZF => not held) */
            G32(VA_g_console_input_numeric_only + 0x4) = 0;                         /* mov dword [0x76858],0 */
            regs_t io = {0};
            io.eax = al;                              /* scancode live in AL at the call */
            io.va  = (uint32_t)G32(e + 1);            /* stored runtime handler ptr (A4) */
#ifndef ROTH_STANDALONE
            call_orig(&io);                           /* push ebx; call [ebx+1]; pop ebx */
#else
            in_if_key_handler(io.va - OBJ_DELTA);     /* enumerated table dispatch (fail-loud default) */
#endif
        }
    }
}

/* keyboard_int9_isr (0x12393, body 0x12393..0x12436): the INT 9 hardware ISR. The hardware
 * dance — EOI (`out 0x20`), scancode read (`in al,0x60`), keyboard-ack pulse on port 0x61,
 * `mov ds,cs:[0x2ef54]` — is the host-replaced seam (the port-0x61 latch value is dead: AX is
 * overwritten by `mov ax,bx` right after). This C body takes the port-0x60 scancode as its
 * parameter and owns everything engine-visible:
 *   - LShift(0x2a) folded onto RShift(0x36), release bit preserved;
 *   - extended prefixes 0xe0/0xe1 ignored entirely;
 *   - held-key bitmap @0x90c3c: release (bit7) clears the key's bit, make sets it;
 *   - make codes except Alt(0x38)/Shift(0x2a/0x36) are pushed onto the scancode ring
 *     (write-head 0x7e91c advances masked by 0x7e91a; drop when the ring is full).
 * NEVER int3-swapped (vector target — same rule as the GDV decode-pump ISR bodies); the live
 * tier is the host input surrogate calling this body directly. */
void keyboard_int9_isr(uint8_t sc)
{
    uint8_t  al     = sc & 0x7f;                      /* and al,0x7f (release bit off) */
    uint8_t  cl     = sc;                             /* mov cx,bx (full scancode) */
    uint32_t bitidx = sc & 7;                         /* and bl,7 */
    if (al == 0x2a) {                                 /* LShift -> RShift fold */
        al = 0x36;
        bitidx = 0x36 & 7;
        cl = (uint8_t)((cl & 0x80) | al);             /* and cl,0x80; or cl,al */
    }
    uint32_t byteidx = (uint32_t)al >> 3;             /* shr ax,3 (ah=0) */
    if (cl == 0xe0 || cl == 0xe1) return;             /* extended prefixes: no state change */
    uint8_t mask = G8(VA_g_keybit_mask_table + bitidx);              /* mov ch,[ebx+0x707e9] */
    if (cl & 0x80) {                                  /* release: clear held bit */
        G8(VA_g_key_state_bitmap + byteidx) &= (uint8_t)~mask;      /* not ch; and [ebx+0x90c3c],ch */
        return;
    }
    G8(VA_g_key_state_bitmap + byteidx) |= mask;                    /* make: set held bit */
    if (cl == 0x38 || cl == 0x2a || cl == 0x36) return; /* Alt/Shift: never enqueued */
    uint16_t head = G16(VA_g_saved_int9_segment + 0x6);                     /* mov ax,[0x7e91c]; movzx ebx,ax */
    uint16_t next = (uint16_t)((head + 1) & G16(VA_g_saved_int9_segment + 0x4));
    if (next == G16(VA_g_saved_int9_segment + 0x8)) return;                 /* ring full: drop */
    G8((VA_g_render_shade_level + 0x2) + head) = cl;                          /* mov [ebx+0x90c1c],cl */
    G16(VA_g_saved_int9_segment + 0x6) = next;                              /* mov [0x7e91c],ax */
}

/* install_keyboard_int9 (0x1246a, 46B): reset the ring (reset_input_ring 0x12504 [L] — direct
 * C), save the current INT 9 vector (crt_dos_vector_op 0x4369a: int21 AX=2502 get-pmode-vector;
 * returns EAX=offset, EDX=selector) into [0x7e8e8]/[0x7e916], then point INT 9 at the game ISR
 * (crt_int21_dispatch 0x4366d: int21 AX=2504 set-pmode-vector, ECX=selector=CS, EBX=offset).
 * Register threading is byte-faithful (gotcha H): EBX carries the ISR entry (a relocated
 * immediate -> runtime 0x412393) across the FIRST call (which push/pops it) into the second;
 * only CX is overwritten with CS between the calls; EDX flows from the get into the set. */
void install_keyboard_int9(void)
{
    reset_input_ring();                        /* call 0x12504 [L] */
#ifdef ROTH_STANDALONE
    /* image-free boot: the always-interactive host-mode
     * drives the int-9 ISR body DIRECTLY from the shm key-ring (iso_apply_scancode) — there is NO
     * game-side INT 9 vector to install (obj1 unmapped; no int-9 fires). The CRT get/set-vector bridges
     * (crt_dos_vector_op 0x4369a get + crt_int21_dispatch 0x4366d set) are a live-swap-only concern
     * -> compiled out. The reset_input_ring above IS kept (menu nav drains the same ring). */
    return;
#else
    regs_t io = {0};
    io.eax = 9;                                       /* mov eax,9 */
    io.ebx = 0x12393 + OBJ_DELTA;                     /* mov ebx,0x12393 (reloc'd immediate) */
    io.va  = 0x4369a + OBJ_DELTA;                     /* crt_dos_vector_op: get INT 9 */
    call_orig(&io);
    G16(VA_g_saved_int9_segment) = (uint16_t)io.edx;                  /* saved vector selector */
    G32(VA_g_saved_int9_offset) = (int32_t)io.eax;                   /* saved vector offset */
    uint16_t cs;
    __asm__("mov %%cs, %0" : "=r"(cs));               /* mov cx,cs (host CS == the original's) */
    io.ecx = (io.ecx & 0xffff0000u) | cs;
    io.eax = 9;                                       /* mov eax,9 (ebx/edx = live leftovers) */
    io.va  = 0x4366d + OBJ_DELTA;                     /* crt_int21_dispatch: set INT 9 */
    call_orig(&io);
#endif
}

/* restore_keyboard_int9 (0x12498, 29B): if a vector was saved ([0x7e916] selector != 0), put
 * the saved selector:offset back on INT 9 via crt_int21_dispatch 0x4366d (ECX=selector,
 * EBX=offset, EAX=9). No-op when install never ran. */
void restore_keyboard_int9(void)
{
#ifdef ROTH_STANDALONE
    /* image-free: no game-side INT 9 vector was ever installed (see install above), so the
     * saved-vector selector [0x7e916] is 0 -> the original short-circuits (je ret) anyway. No-op. */
    return;
#else
    uint16_t cx = G16(VA_g_saved_int9_segment);                       /* mov cx,[0x7e916] */
    if (cx == 0) return;                              /* or cx,cx; je ret */
    regs_t io = {0};
    io.ecx = cx;
    io.ebx = (uint32_t)G32(VA_g_saved_int9_offset);                  /* saved offset */
    io.eax = 9;
    io.va  = 0x4366d + OBJ_DELTA;
    call_orig(&io);
#endif
}

/* ===================== B. keymap + key handlers ===================== */

#ifdef ROTH_STANDALONE
/* dos_print_char 0x114d4 / dos_print_concat 0x114a3 (CRT console echo; corpus-faithful)
 * composed over the C2 soft-int hook — int 21h AH=9 '$'-string print. */
static void in_if_dos_print_char(uint32_t eax)
{
    uint8_t buf[2]; buf[0] = (uint8_t)eax; buf[1] = '$';
    regs_t v = {0};
    v.eax = 0x900; v.edx = (uint32_t)(uintptr_t)buf;
    if (g_os_soft_int) g_os_soft_int(0x21, &v);
}
static uint32_t in_if_dos_print_concat(uint32_t eax_str, uint32_t edx_str)
{
    char buf[100]; char *p = buf;                       /* corpus 114a3: copy EDX then EAX, '$'-terminate */
    const char *s = (const char *)(uintptr_t)edx_str;
    if (s) while (*s) *p++ = *s++;
    const char *t = (const char *)(uintptr_t)eax_str;
    uint32_t n = 0;                                     /* 0x114bb inc ebp per EAX-string byte (dec'd once for
                                                         * the NUL) -> 0x114ce mov ecx,ebp: ECX = strlen(EAX str),
                                                         * the A1 multi-reg return the field editor consumes */
    while (*t) { *p++ = *t++; n++; }
    *p = '$';
    regs_t v = {0};
    v.eax = 0x900; v.edx = (uint32_t)(uintptr_t)buf;
    if (g_os_soft_int) g_os_soft_int(0x21, &v);
    return n;
}
#endif

/* bridge helpers: call an un-lifted (or oracle-stubbed) callee faithfully. */
static void in_bridge(uint32_t canon_va)
{
    regs_t io = {0};
    io.va = canon_va + OBJ_DELTA;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    switch (canon_va) {   /* routes (0x2e609 toggle_video_display_mode = host video, stays fail-loud) */
    case 0x1480cu: cycle_screen_resolution(); return;
    case 0x11135u: take_snapshot(); return;
    default: break;
    }
    roth_unreachable(io.va - OBJ_DELTA);   /* input action bridge — in-game input tier */
#endif
}
static void in_bridge_eax(uint32_t canon_va, uint32_t eax)
{
    regs_t io = {0};
    io.eax = eax;
    io.va  = canon_va + OBJ_DELTA;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    switch (canon_va) {   /* routes */
    case 0x17fe5u: show_status_message_wrap(eax); return;
    case 0x147f4u: set_resolution_index_and_cycle(eax); return;
    case 0x114d4u: in_if_dos_print_char(eax); return;   /* CRT console echo over the soft-int hook */
    default: break;
    }
    roth_unreachable(io.va - OBJ_DELTA);   /* input action bridge (EAX) — in-game input tier */
#endif
}

/* keymap_dispatch (0x14525, 56B): the per-frame key dispatcher. Drains the scancode ring
 * (input_ring_dequeue [L]); each dequeued code is looked up in the keymap table @0x7093d
 * ({u8 sc, u32 handler}, 0-terminated; 37 live entries): a match dispatches the stored
 * RUNTIME handler pointer (A4) with EAX=0 then zeroes [0x76858] and loops for the next key;
 * an UNMATCHED code exits the whole dispatcher (the rest of the ring waits for next frame).
 * The original pushal/popal + push/pop fs/gs frames the whole walk (ABI_VOID preserves). */
void keymap_dispatch(void)
{
    for (;;) {
        uint8_t al = input_ring_dequeue();     /* call 0x1299a; je done */
        if (al == 0) return;                          /* ring empty */
        uint32_t e = 0x7093d;                         /* mov ebx,0x7093d */
        for (;;) {
            if (G8(e) == 0) return;                   /* table end: unmatched key exits */
            if (G8(e) == al) break;                   /* cmp [ebx],al; je dispatch */
            e += 5;
        }
        regs_t io = {0};                              /* push ebx; sub eax,eax */
        io.va = (uint32_t)G32(e + 1);                 /* stored runtime handler ptr (A4) */
#ifndef ROTH_STANDALONE
        call_orig(&io);                               /* call [ebx+1]; pop ebx */
#else
        in_if_key_handler(io.va - OBJ_DELTA);         /* enumerated table dispatch (fail-loud default) */
#endif
        G32(VA_g_console_input_numeric_only + 0x4) = 0;                             /* mov dword [0x76858],0 */
    }
}

/* key_toggle_wireframe_map (0x1444c, 24B): in-map ([0x7f560]&1) toggle the wireframe-map
 * byte 0x7f36e; outside a map force it off. */
void key_toggle_wireframe_map(void)
{
    if (G8(VA_g_dev_mode_flag) & 1)
        G8(VA_g_debug_map_enabled) = (uint8_t)~G8(VA_g_debug_map_enabled);          /* not byte [0x7f36e] */
    else
        G8(VA_g_debug_map_enabled) = 0;
}

/* key_n_vestigial (0x145a4, 10B; DEAD): word [0x90c14] = 0xffff. */
void key_n_vestigial(void)
{
    G16(VA_g_player_sector + 0x2) = 0xffff;
}

/* key_set_view_scale (0x14573, 28B): cycle the view-scale index 0x90bd4 down (3..0 wrap:
 * `sub byte,1; jns` — the decremented byte is written, then reset to 3 if it went negative),
 * set the viewport-dirty byte 0x8c1d2 and tail-call configure_render_viewport 0x408d1
 * (render_world; BRIDGED — nested render config, oracle-stubbed like test_player's
 * t_update_turn_view_scale). */
void key_set_view_scale(void)
{
    G8(VA_g_view_scale_flags) = (uint8_t)(G8(VA_g_view_scale_flags) - 1);
    if ((int8_t)G8(VA_g_view_scale_flags) < 0)                      /* jns 0x14583 */
        G8(VA_g_view_scale_flags) = 3;
    G8(VA_g_collision_sector_stack + 0x3e) = 1;
    /* Direct C: configure_render_viewport 0x408d1's full body is oracle-verified standalone
     * in test_render_world (writeset over setup_scale 0x2e458 + rebuild diffs green with 0x2fdfc
     * ret-patched and [0x8c1d2]!=0 skipping the 0x2e140 VGA bridge — which THIS caller guarantees by
     * setting 0x8c1d2=1 above). Caller test re-stages the viewport globals + ret-patches 0x2fdfc so
     * both sides run the real body symmetrically. Also converted in lift_player.c. */
    configure_render_viewport();               /* was in_bridge 0x408d1 (tail call) */
}

/* key_gamma_increase (0x14601, 18B; DEAD) / key_gamma_decrease (0x145ec, 21B) — flow_succ
 * pair: decrease falls into increase's shared tail at 0x14608. The gamma level is the word
 * 0x89f12 (decrease floors at 0, increase is unbounded); the tail re-reads the DAS palette
 * (read_das_palette 0x2f379 [L], BRIDGED: inline int21 file I/O — oracle-stubbed) and
 * uploads the DAC (upload_palette_dac 0x2febe, video_display bridge). */
static void gamma_apply_tail(void)                    /* 0x14608 */
{
    read_das_palette();                         /* 0x2f379 re-pointed: real body over
                                                        * the low-mem palette buffer ([0x90bca]<<4) +
                                                        * symmetrically-simmed inline int21s (test). */
    upload_palette_dac();                        /* 0x2febe re-pointed: counter==0 DAC
                                                        * out-loop is a no-op under the oracle (lift's
                                                        * g_os_port_out is NULL; orig's `out`s NOP'd). */
}
void key_gamma_increase(void)
{
    G16(VA_g_gamma_level) = (uint16_t)(G16(VA_g_gamma_level) + 1);      /* inc word [0x89f12] */
    gamma_apply_tail();
}
void key_gamma_decrease(void)
{
    if (G16(VA_g_gamma_level) == 0) return;                    /* test word,0xffff; je ret */
    G16(VA_g_gamma_level) = (uint16_t)(G16(VA_g_gamma_level) - 1);      /* dec word [0x89f12] */
    gamma_apply_tail();                               /* jmp 0x14608 */
}

/* key_toggle_help_overlay (0x17fc0, 17B; DEAD) -> key_f7_noop (0x17fd1, 1B; DEAD) flow_succ
 * pair: the toggle clears the timed-message timer 0x827e5 and XORs the help-overlay byte
 * 0x7fe38; the shared "body" is the bare ret. */
void key_f7_noop(void) { }
void key_toggle_help_overlay(void)
{
    G32(VA_g_timed_message_timer) = 0;
    G8(VA_g_help_overlay_enabled) ^= 1;
}

/* key_toggle_mouse_swap (0x17fd2, 19B): XOR the A-B/B-A button-mode byte 0x7feb8, then
 * report via the show_status_message_wrap tail 0x17fe5 (menu_hud_ui flow_succ tail — NOT
 * inlined here, BRIDGED: it sign-extends AL and falls into show_no_ammo_message 0x1f8cb).
 * Message id: swapped-on -> 7, off -> 8 (sete al; add al,7). */
void key_toggle_mouse_swap(void)
{
    G8(VA_g_mouse_button_swap) ^= 1;
    uint8_t al = (uint8_t)(((G8(VA_g_mouse_button_swap) & 1) == 0 ? 1 : 0) + 7);
    in_bridge_eax(0x17fe5, al);                       /* jmp show_status_message_wrap */
}

/* key_toggle_sprint (0x17fed, 22B): NOT the sprint dword 0x7e8dc, report id 2 (on) / 3 (off)
 * via the same show_status_message_wrap tail (sete al; inc al; inc al). */
void key_toggle_sprint(void)
{
    G32(VA_g_run_toggle) = ~G32(VA_g_run_toggle);
    uint8_t al = (uint8_t)(((G8(VA_g_run_toggle) & 1) == 0 ? 1 : 0) + 2);
    in_bridge_eax(0x17fe5, al);                       /* jmp 0x17fe5 */
}

/* key_toggle_subtitles (0x1f89b, 48B): NOT the subtitle dword 0x83e90; when enabling
 * (nonzero) clear 0x83e94 first; store the new value to 0x83e9c either way, then FALL INTO
 * show_no_ammo_message 0x1f8cb (weapon_combat flow_succ tail, [L]) with EAX = new&1 ? 1 : 0.
 * Called as direct C (call-closed; the tail is verified weapon_combat code). */
void key_toggle_subtitles(void)
{
    G32(VA_g_active_weapon_ammo_cap + 0x1c) = ~G32(VA_g_active_weapon_ammo_cap + 0x1c);
    int32_t eax = G32(VA_g_active_weapon_ammo_cap + 0x1c);
    if (eax != 0) {
        G32(VA_g_voice_decode_suspended) = 0;
        G32(VA_g_voice_decode_suspended + 0x8) = 0;
    }
    G32(VA_g_voice_decode_suspended + 0x8) = eax;
    show_no_ammo_message((eax & 1) ? 1u : 0u); /* test al,1; setne; movsx; fallthrough */
}

/* key_toggle_easy_select (0x1801c, 36B): XOR the easy-select byte 0x81e34 and queue the
 * matching status string via show_timed_message 0x1f88c ([L] — direct C; the string
 * immediates are relocated obj3 addresses). */
void key_toggle_easy_select(void)
{
    G8(VA_g_object_select_easy_flag) ^= 1;
    uint32_t msg = (G8(VA_g_object_select_easy_flag) & 1) ? 0x75ded : 0x75e0b;
    show_timed_message((const char *)GADDR(msg)); /* jmp 0x1f88c (tail) */
}

/* key_show_version (0x14c9c, 10B; DEAD): show the version string 0x767c0 via
 * show_timed_message [L]. */
void key_show_version(void)
{
    show_timed_message((const char *)GADDR(VA_g_version_string));
}

/* key_flush_texture_cache (0x144fb, 11B; DEAD): reset the DAS entry status table
 * (0x3001b [L]) + flush the object DAS handles (0x26cd4 [L]); both direct C (call-closed). */
void key_flush_texture_cache(void)
{
    reset_das_entry_status_table();
    flush_object_das_handles();
}

/* key_quickload (0x14cc9, 28B) / key_quicksave (0x14ce5, 28B): arm the savegame request
 * globals — mode 0x7feac = 1 (load) / 2 (save), slot-arg 0x7feb0 = 0, request-flags
 * 0x7fea8 |= 2. Pure global writes (the savegame subsystem consumes them next frame). */
void key_quickload(void)
{
    G32(VA_g_pending_game_action + 0x4) = 1;
    G32(VA_g_pending_game_action + 0x8) = 0;
    G32(VA_g_pending_game_action) |= 2;
}
void key_quicksave(void)
{
    G32(VA_g_pending_game_action + 0x4) = 2;
    G32(VA_g_pending_game_action + 0x8) = 0;
    G32(VA_g_pending_game_action) |= 2;
}

/* key_view_size_increase (0x14613) / key_view_size_decrease (0x14678) — three modes each:
 *   map-selector-menu mode ([0x7fe28]!=0, tails 0x173ab/0x173d9): rebuild the selector menu
 *     into a 0x400 stack buffer (build_map_selector_menu 0x17453 — BRIDGED: DOS dir scan)
 *     and step the selection index 0x7fe30 with wraparound against the count 0x7fe2c;
 *   wireframe-map mode ([0x7f36e]!=0, shared tails 0x14651/0x14665): step the map zoom word
 *     0x70498 by 0x20 within [0x40, 0x1000];
 *   normal: step the view-size byte 0x7049a (increase DECREMENTS it — smaller index = bigger
 *     view; floor 0, ceiling [0x703cc], SIGNED byte compare), then recompute_view_region_offsets
 *     0x10e4e ([L] pure leaf, direct C), viewport-dirty 0x8c1d2=1, configure_render_viewport
 *     0x408d1 (BRIDGED), redraw flag 0x7f570=1. */
static void view_size_commit(void)
{
    recompute_view_region_offsets();           /* call 0x10e4e [L] */
    G8(VA_g_collision_sector_stack + 0x3e) = 1;
    /* Direct C: 0x408d1 body oracle-verified standalone (test_render_world); [0x8c1d2]=1
     * above skips its 0x2e140 VGA bridge; t_view_size_pair stages the viewport + ret-patches 0x2fdfc. */
    configure_render_viewport();               /* was in_bridge 0x408d1 */
    G8(VA_g_reloc_base + 0x4) = 1;
}
void key_view_size_increase(void)
{
    if (G32(VA_g_map_menu_active) != 0) {                          /* jne 0x173ab (menu mode) */
        uint8_t buf[0x400];
        /* 0x17453 re-pointed: build_map_selector_menu is a PURE string-list formatter
         * (walks the in-memory map list [0x76710] into `buf` + obj3 count/selection globals — the old
         * "DOS dir scan" marker was WRONG; disasm shows no int21). Direct C; real on both sides. */
        build_map_selector_menu((char *)buf);
        G32(VA_g_map_menu_selected_index) = G32(VA_g_map_menu_selected_index) + 1;
        if (G32(VA_g_map_menu_selected_index) >= G32(VA_g_map_menu_count))             /* jl keep (signed) */
            G32(VA_g_map_menu_selected_index) = 0;
        return;
    }
    if (G8(VA_g_debug_map_enabled) != 0) {                           /* jne 0x14651 (wireframe zoom in) */
        if (G16(VA_g_cfg_das2_arg + 0x1bc) <= 0x1000)                   /* ja skip (unsigned) */
            G16(VA_g_cfg_das2_arg + 0x1bc) = (uint16_t)(G16(VA_g_cfg_das2_arg + 0x1bc) + 0x20);
        return;
    }
    if (G8(VA_g_cfg_das2_arg + 0x1be) != 0) {                           /* test byte,0xff; je ret */
        G8(VA_g_cfg_das2_arg + 0x1be) = (uint8_t)(G8(VA_g_cfg_das2_arg + 0x1be) - 1);
        view_size_commit();
    }
}
void key_view_size_decrease(void)
{
    if (G32(VA_g_map_menu_active) != 0) {                          /* jne 0x173d9 (menu mode) */
        G32(VA_g_map_menu_selected_index) = G32(VA_g_map_menu_selected_index) - 1;
        if (G32(VA_g_map_menu_selected_index) < 0)                         /* jge keep (signed) */
            G32(VA_g_map_menu_selected_index) = G32(VA_g_map_menu_count) - 1;
        return;
    }
    if (G8(VA_g_debug_map_enabled) != 0) {                           /* jne 0x14665 (wireframe zoom out) */
        if (G16(VA_g_cfg_das2_arg + 0x1bc) >= 0x40)                     /* jb skip (unsigned) */
            G16(VA_g_cfg_das2_arg + 0x1bc) = (uint16_t)(G16(VA_g_cfg_das2_arg + 0x1bc) - 0x20);
        return;
    }
    if ((int8_t)G8(VA_g_cfg_das2_arg + 0x1be) < (int8_t)G8(VA_g_cfg_das2_arg + 0xf0)) {  /* cmp byte,al; jge ret (SIGNED) */
        G8(VA_g_cfg_das2_arg + 0x1be) = (uint8_t)(G8(VA_g_cfg_das2_arg + 0x1be) + 1);
        view_size_commit();
    }
}

/* key_cycle_display_type (0x14794, 20B): begin_screen_draw 0x11ca9 -> cycle_screen_resolution
 * 0x1480c -> end_screen_draw 0x11cc6 -> tail reset_hud_icon_state 0x1bc91 (all video_display/
 * menu_hud_ui BRIDGES — mode-set/screen I/O, oracle-stubbed; live-swap validates). */
void key_cycle_display_type(void)
{
    begin_screen_draw();                       /* 0x11ca9 re-pointed: real body on
                                                       * both sides — pure inc [0x7e8c8] when the two
                                                       * cursor descriptors 0x76898/0x7a8a4 are unset. */
    in_bridge(0x1480c);   /* [IN-GAME-TIER] [KEPT-REPOINT: cycle_screen_resolution 0x1480c — 733B mode-set orchestrator.
                           * The seam-NOP conversion was ATTEMPTED and PROVED the wall by disasm +
                           * oracle-harness analysis (this is NOT oracle-neutral like the rwss
                           * conversion — the caller key_cycle_display_type IS oracle-gated:
                           * diff_writeset("key_cycle_display_type",0x14794,...) in test_input.c t_cycle_display,
                           * which currently stub_marker's 0x1480c). Three independent, disasm-confirmed walls:
                           *   (1) roth-oracle links NO int/out/int31 servicer. The only signal handler in the
                           *       oracle image is c2_mock's SIGTRAP-int3 (c2 DPMI subsystem only) — the
                           *       established recipe for a soft-int seam is to BYTE-PATCH the canon int site
                           *       (stc/NOP) and NULL the lift hook (see das_assets/blit_2d), i.e. the oracle
                           *       cannot EXECUTE a real int10/int31/out. Un-stubbing 0x1480c makes BOTH the
                           *       ORIGINAL side (canon 0x14794->0x1480c) AND the LIFT side (cycle_screen_
                           *       resolution -> its unconditional vd_bridge=call_orig sub-calls) hard-SIGSEGV
                           *       on the first unpatched seam — a crash, not a graceful diff-fail. This is
                           *       precisely why 0x1480c is registered live-swap-only + excluded from the `all`
                           *       set (lift_registry.c is_video, ~L1311).
                           *   (2) Selector/VESA returns feed obj3 and are UNSTAGEABLE by any byte-patch:
                           *       the common-tail configure_render_viewport 0x408d1 -> 0x2e458 unconditionally
                           *       runs `call 0x2fdfc` (DPMI) + a VGA `out dx,al`/`in al,dx` CRTC program block
                           *       (disasm 0x2e5ff/0x2e663..0x2e736); alloc_scene_render_arena/alloc_framebuffer_
                           *       surface mint DPMI selectors (0x2f72a/0x2fd7c/0x2fdbc) whose runtime-allocated
                           *       AX is STORED to obj3 (e.g. G16(0x85294)). A NOP'd alloc yields a garbage/zero
                           *       selector in obj3 -> guaranteed mismatch (or #GP on the subsequent selector
                           *       use); the host-allocated selector value is non-deterministic, so no patch
                           *       reproduces it.
                           *   (3) The VESA helper 0x14b24 (reachable for any non-VGA index) does int10 AX=4f02
                           *       then reads the VBE mode-info block [eax+0x10..0x14] (disasm 0x14b52..0x14b6e)
                           *       to derive the pitch/width/height obj3 writes — a real-mode buffer the DPMI
                           *       host must populate (assignment's named unstageable blocker). Avoidable only by
                           *       pinning a VGA index, but (2) still blocks the VGA path.
                           * NB the marker's "8 sub-fns each oracle-verified individually" was optimistic:
                           * test_video_display.c tests only the obj3-pure list/palette math + build_scanline_
                           * dest_offset_table; configure_render_viewport / upload_palette_dac's DAC path are NOT
                           * individually oracle-verified. Unblock: a dedicated video_display integration wave
                           * that MOCKS the DPMI selector allocs + VBE block + int10/VGA-out with return-value
                           * fidelity (own TU + new harness) — OR ride the live-swap in-game tier, where 0x1480c
                           * already runs lifted. NOT agent-closable as a re-point leaf. */
    end_screen_draw();                         /* 0x11cc6 re-pointed: real body — dec
                                                       * [0x7e8c8]; force_cursor_redraw early-exits while
                                                       * the draw-depth stays nonzero (no INT33/VGA I/O). */
    reset_hud_icon_state();                    /* jmp (tail) — 0x1bc91 direct C (was in_bridge) */
}

/* key_cycle_display_type_alt (0x147a8, 62B): begin_screen_draw, then either toggle the
 * display MODE (0x2e609, when the VESA-capable byte 0x7f372 is set) or flip the resolution
 * INDEX (set_resolution_index_and_cycle 0x147f4 with EAX = [0x7f358]==2 ? 1 : 2); common
 * tail: redraw flag 0x7f570=1, end_screen_draw, reset_hud_icon_state. */
void key_cycle_display_type_alt(void)
{
    begin_screen_draw();                       /* 0x11ca9 re-pointed */
    if (G8(VA_g_display_type) != 0) {
        in_bridge(0x2e609);                           /* call toggle_video_display_mode */
    } else {
        uint32_t eax = (G32(VA_g_screen_resolution_index) == 2) ? 1u : 2u;
        in_bridge_eax(0x147f4, eax);                  /* call set_resolution_index_and_cycle */
    }
    G8(VA_g_reloc_base + 0x4) = 1;                                  /* 0x147bb (shared tail) */
    end_screen_draw();                         /* 0x11cc6 re-pointed */
    reset_hud_icon_state();                    /* jmp reset_hud_icon_state — 0x1bc91 direct C */
}

/* check_snapshot_key (0x11124, 17B): when the snapshot-armed byte 0x76840 is set, run
 * take_snapshot 0x11135 (savegame; BRIDGED — interactive int 0x10/menu; the original is
 * pushal/popal-framed). Live-swap must be INTERACTIVE (the bridged menu spins on the tick). */
void check_snapshot_key(void)
{
    if (G8(VA_g_snapshot_enabled) == 0) return;
    /* [KEPT-REPOINT: take_snapshot 0x11135 — INTERACTIVE (registered lift_is_interactive -> raises
     * g_os_interactive). Re-confirmed the wall: this caller (check_snapshot_key 0x11124) IS
     * oracle-gated — test_input.c t_check_snapshot diff_writeset's the [armed] case and STUB_MARKERs
     * 0x11135 (stub_marker(0,0x11135,0x7692c)). So converting is NOT oracle-neutral: a direct call to
     * take_snapshot makes the LIFT side run the real body whose FIRST act (after the int 0x10
     * text-mode switch) is call 0x111a0 snapshot_menu_and_save, which BLOCKS on console_read_key —
     * pumped by shm_tick only in the live-swap tier, never the batch oracle -> infinite block; the
     * ORIGINAL side stays stubbed -> asymmetric. No bounded path (the menu is unconditional). (The
     * sub-call configure_render_viewport 0x408d1 is no longer a blocker — oracle-verified + converted
     * — but the console_read_key block is.) Unblock: interactive live-swap tier only, OR an
     * interactive-modal harness that stages a canned console key + symmetrizes the menu render. Verify
     * in-game, not the static oracle. */
    in_bridge(0x11135);                               /* [IN-GAME-TIER] pushal; call take_snapshot; popal */
}

/* use_enter_key_handler (0x173f4, 94B): the Enter/use key. If dialogue is busy (0x83aea!=0):
 * gate 0x83125 == 0x6ffff -> accept the selected choice (choice_accept_selected 0x1fbba,
 * dialogue_ui BRIDGE) and RETURN; any other nonzero gate -> force-end the dialogue voice
 * (dialogue_voice_force_end 0x1f671 with EAX=0/EDX=0, audio BRIDGE). Then (and also when not
 * busy): if the map-selector menu is open (0x7fe28!=0) close it — zero 0x7fe28, invalidate
 * 0x85484, commit the selected map 0x7fe34 -> 0x76630 and set the request bit 0x7fea8|=1. */
void use_enter_key_handler(void)
{
    if (G32(VA_g_dialogue_busy_flag) != 0) {
        if (G32(VA_g_move_freeze_gate) == 0x6ffff) {
            /* choice_accept_selected 0x1fbba — direct-C. The lifted body already calls the
             * lifted interpreter tail execute_dialogue_branch 0x1dc02 directly (not bridged); the
             * use_enter test runs it real+symmetric by staging the dbase100 early-terminate
             * (g_dbase100_active 0x81ea2=0 -> execute_dialogue_branch returns with no writes,
             * 0x1dc02 ret-stubbed on the original side). */
            choice_accept_selected();          /* commit choice + run branch */
            return;
        }
        if (G32(VA_g_move_freeze_gate) != 0) {
            /* [REPOINT] direct C: dialogue_voice_force_end 0x1f671 (EAX=EDX=0). This site is only
             * reached when 0x83125 is nonzero AND != 0x6ffff (the 0x6ffff case returned above via
             * choice_accept), so the real body's else-branch just zeroes 0x83125 — no SOS mixer, no
             * internal highlight. The use_enter test stages 0x8200c!=1 to pin the non-mixer path. */
            dialogue_voice_force_end(0, 0); /* 0x1f671 re-pointed */
        }
    }
    if (G32(VA_g_map_menu_active) != 0) {
        G32(VA_g_map_menu_active) = 0;
        G32(VA_g_map_first_load_flag) = (int32_t)0xffffffff;
        int32_t eax = G32(VA_g_map_menu_selected_entry);
        G8(VA_g_pending_game_action) |= 1;                             /* or byte [0x7fea8],1 */
        G32(VA_g_dir_gdv + 0x140) = eax;
    }
}

/* ===================== C. mouse ===================== */
/* The INT 33h mouse services are host-replaced: every inline `int 0x33` routes through the
 * g_os_soft_int hook (host mouse_int33 — the same handler the trap dispatch runs for the
 * original's int byte). AX=0 reset/detect, AX=3 buttons, AX=0xB motion counters. */

/* poll_mouse_motion (0x117db, 206B): read the INT 33h AX=0xB motion counters (sign-extended
 * CX/DX mickey deltas). Hardware-cursor mode ([0x7e8d0]!=0): accumulate into 0x76870/0x76874
 * and zero the idle counter 0x76858 on any movement. Software-cursor mode: scale deltas by
 * 32, advance the fixed-point cursor accumulators 0x7e8c0/0x7e8c4 (biased by the half-extent
 * 0x7e8b8/0x7e8bc), clamp to the screen extents 0x7e8b0/0x7e8b4, then derive g_mouse_x/y
 * (0x707b3/0x707b7) via a 16-bit SIGNED divide by the cursor scale word 0x707bb (idiv
 * dx:ax -> AX quotient, cwde — transcribed exactly). GP regs push/pop-preserved. */
void poll_mouse_motion(void)
{
    regs_t io = {0};
    io.eax = 0xb;                                     /* mov ax,0xb; int 0x33 */
    if (g_os_soft_int) g_os_soft_int(0x33, &io);
    int32_t ecx = (int32_t)(int16_t)io.ecx;           /* movsx ecx,cx */
    int32_t edx = (int32_t)(int16_t)io.edx;           /* movsx edx,dx */
    if (G8(VA_g_mouse_relative_mode) != 0) {                           /* hardware-cursor mode */
        G32(VA_g_mouse_dx) += ecx;
        G32(VA_g_mouse_dy) += edx;
        if ((ecx | edx) != 0)
            G32(VA_g_console_input_numeric_only + 0x4) = 0;
        return;
    }
    ecx = (ecx << 5) + G32(VA_g_cursor_prev_y + 0x802c) + G32(VA_g_cursor_prev_y + 0x8024);   /* 0x11811 (software cursor) */
    edx = (edx << 5) + G32(VA_g_cursor_prev_y + 0x8030) + G32(VA_g_cursor_prev_y + 0x8028);
    if (ecx >= G32(VA_g_cursor_prev_y + 0x801c)) ecx = G32(VA_g_cursor_prev_y + 0x801c);      /* cmp; jl keep */
    if (ecx < 0) ecx = 0;                             /* or; jge keep */
    if (edx >= G32(VA_g_cursor_prev_y + 0x8020)) edx = G32(VA_g_cursor_prev_y + 0x8020);
    if (edx < 0) edx = 0;
    ecx -= G32(VA_g_cursor_prev_y + 0x8024);
    edx -= G32(VA_g_cursor_prev_y + 0x8028);
    G32(VA_g_cursor_prev_y + 0x802c) = ecx;
    G32(VA_g_cursor_prev_y + 0x8030) = edx;
    int16_t scale = (int16_t)G16(VA_g_pixel_extent_scale);
    int32_t qy = (int32_t)(int16_t)(edx / scale);     /* idiv word [0x707bb]; cwde */
    G32(VA_g_saveunder_sprite_color_ptr + 0xc) = qy;
    G32(VA_g_mouse_y) = qy + G32(VA_g_saveunder_sprite_color_ptr + 0x14);                 /* g_mouse_y */
    int32_t qx = (int32_t)(int16_t)(ecx / scale);
    G32(VA_g_saveunder_sprite_color_ptr + 0x8) = qx;
    G32(VA_g_mouse_x) = qx + G32(VA_g_saveunder_sprite_color_ptr + 0x10);                 /* g_mouse_x */
}

/* compute_screen_extents_7e8b0 (0x115b5): ALREADY LIFTED (renderer.c, early corpus-direct batch;
 * oracle-tested there + 3 more staging cases in test_input.c). Re-pinned render_world -> input
 * (the cursor clamp bounds are
 * consumed only by input; the other caller, video_display's cycle_screen_resolution, just
 * refreshes them after a mode change). The call below is DIRECT C — the original bridge here
 * was re-point debt to an already-lifted VA, now retired (workstream D). */

/* blit_scaled_sprite_at_mouse (0x115ea, 146B): configure the software cursor from a 0x14-byte
 * descriptor (EAX = a RUNTIME pointer, raw-deref'd [A4]): {+0/+4 sprite ptrs -> the
 * stored-pointer globals 0x76878/0x7687c, +8 w, +0xc h, +0x10 scale}. Computes the scaled
 * half-extents 0x7e8b8/0x7e8bc, the screen extents (compute_screen_extents_7e8b0 — direct C,
 * call-closed), re-bases the cursor accumulators from g_mouse_x/y, then polls motion once
 * under a bumped hide counter 0x7e8c8. */
void blit_scaled_sprite_at_mouse(uint32_t desc)
{
#define DD(o) (*(int32_t *)(uintptr_t)((desc) + (o)))
    int32_t p0 = DD(0), p1 = DD(4);
    G32(VA_g_cursor_prev_x) = (int32_t)0xffffff81;
    G32(VA_g_saveunder_sprite_color_ptr) = p0;                                /* stored sprite ptr (A4) */
    G32(VA_g_saveunder_sprite_color_ptr + 0x4) = p1;
    int32_t w = DD(8), h = DD(0xc), scale = DD(0x10);
    G32(VA_g_cursor_prev_y + 0x8024) = w * scale;
    G32(VA_g_cursor_prev_y + 0x8028) = h * scale;
    G16(VA_g_pixel_extent_scale) = (uint16_t)scale;
    compute_screen_extents_7e8b0();            /* call 0x115b5 (call-closed) */
    G32(VA_g_cursor_prev_y + 0x802c) = G32(VA_g_mouse_x) * scale - G32(VA_g_cursor_prev_y + 0x8024);
    G32(VA_g_cursor_prev_y + 0x8030) = G32(VA_g_mouse_y) * scale - G32(VA_g_cursor_prev_y + 0x8028);
    G32(VA_g_saveunder_sprite_color_ptr + 0x10) = w;
    G32(VA_g_saveunder_sprite_color_ptr + 0x14) = h;
    G32(VA_g_screen_busy_depth) += 1;                                /* hide cursor during the poll */
    poll_mouse_motion();                       /* call 0x117db */
    G32(VA_g_screen_busy_depth) -= 1;
#undef DD
}

/* init_software_mouse (0x11594, 33B): INT 33h AX=0 (reset/detect). Present (AX!=0):
 * configure the cursor from the default descriptor @0x7079f ({sprite 0x7074c x2, w=1, h=1,
 * scale=0x20}, a relocated immediate) and return 0. Absent: bump the hide counter by 0x50
 * and return -1 (EAX output — begin_frame's `or ax,ax` reads it, flags dead). */
uint32_t init_software_mouse(void)
{
    regs_t io = {0};                                  /* sub eax,eax; int 0x33 */
    if (g_os_soft_int) g_os_soft_int(0x33, &io);
    if ((io.eax & 0xffff) == 0) {                     /* or ax,ax; je absent */
        G32(VA_g_screen_busy_depth) += 0x50;                         /* 0x115aa */
        return 0xffffffffu;                           /* or eax,-1 */
    }
    blit_scaled_sprite_at_mouse(0x7079f + OBJ_DELTA); /* mov eax,0x7079f (reloc'd) */
    return 0;                                         /* sub eax,eax */
}

/* poll_mouse_input (0x11fae, 107B): INT 33h AX=3 (button state in BL). Button-swap mode
 * (dword [0x7feb8]!=0): shift left (`add bl,bl`) and fold the right button onto the primary
 * bit. Accumulates held buttons (0x7e928), computes PRESS EDGES against the previous state
 * byte 0x7e929: primary edge sets 0x7e938=0xff + 0x7e93a=1, secondary edge sets 0x7e939=0xff
 * + 0x7e93a|=2, then stores the new previous state. GP regs push/pop-preserved. */
void poll_mouse_input(void)
{
    regs_t io = {0};
    io.eax = 3;                                       /* mov ax,3; int 0x33 */
    if (g_os_soft_int) g_os_soft_int(0x33, &io);
    uint8_t bl = (uint8_t)io.ebx;
    if (G32(VA_g_mouse_button_swap) != 0) {                          /* cmp dword,0 (swap mode) */
        bl = (uint8_t)(bl + bl);                      /* add bl,bl */
        if (bl & 4)
            bl |= 1;                                  /* right button -> primary */
    }
    uint8_t bh = G8(VA_g_mouse_buttons_prev);                         /* previous buttons */
    G8(VA_g_mouse_buttons_held) |= bl;
    G8(VA_g_mouse_click_edges) = 0;
    if ((bl & 1) && !(bh & 1)) {                      /* primary press edge */
        G8(VA_g_cursor_primary_action_flag) = 0xff;                           /* g_cursor_primary_action_flag */
        G8(VA_g_mouse_click_edges) = 1;
    }
    if ((bl & 2) && !(bh & 2)) {                      /* secondary press edge */
        G8(VA_g_cursor_secondary_action_flag) = 0xff;                           /* g_cursor_secondary_action_flag */
        G8(VA_g_mouse_click_edges) |= 2;
    }
    G8(VA_g_mouse_buttons_prev) = bl;
}

/* drain_input_and_clear_clicks (0x2057a, 29B): drain the scancode ring, poll the buttons
 * once, clear both click-edge flags. Call-closed (all three callees are lifted). */
void drain_input_and_clear_clicks(void)
{
    while (input_ring_dequeue() != 0) { }      /* call 0x1299a; test al,al; jne */
    poll_mouse_input();                        /* call 0x11fae */
    G8(VA_g_cursor_secondary_action_flag) = 0;
    G8(VA_g_cursor_primary_action_flag) = 0;
}

/* begin_frame_then_init_mouse (0x103c8, 23B): begin_screen_draw 0x11ca9 (video_display
 * BRIDGE), init the software mouse (the `or ax,ax` on its return is flag-dead), and mark
 * the HUD-restore word 0x7674e. */
void begin_frame_then_init_mouse(void)
{
    begin_screen_draw();                       /* 0x11ca9 re-pointed: pure inc [0x7e8c8]
                                                       * when 0x76898/0x7a8a4 unset (staged in the test). */
    init_software_mouse();                     /* call 0x11594 */
    G16(VA_g_player_movement_enabled + 0x4) = 0xffff;
}

/* ===================== D. cursor world use/examine ===================== */
/* The pickers are the subsystem's dispatch heart: they read the clicked world-object record
 * (type byte +1, def ptr +0xe, distance^2 +0x20) and fan out into raw_command_system /
 * dialogue_ui / dbase100 / doors / entity_ai / inventory / audio. EVERY cross-subsystem
 * callee is BRIDGED via call_orig with a threaded regs_t: the callees return live values in
 * EDX (the record cursor — Ghidra models them as CONCAT44(edx,eax)) that the pickers keep
 * consuming (gotcha A1), so the io struct carries the machine register file call-to-call.
 * Record/def pointers are RUNTIME addresses (A4 — raw derefs). All non-idempotent (fire
 * triggers): oracle uses input-recording output-contract stubs; live tier = the debug run.
 *
 * Converted the tractable pure/bounded leaves to direct C (0x30c1a
 * object_has_active_trigger_link — read-only predicate; 0x1bb12 move_cursor_entry_clamped — pure
 * obj3 delta-clamp; 0x12a08 set_cursor_shape — early-return subset). Their tests stage the bounded
 * state so the real body runs symmetric on both sides. See the per-site [REPOINT] markers.
 *
 * Converted the raw-command TRIGGER-FIRER leaves to direct C, each verified by staging its real
 * BOUNDED/no-op path (real predicate runs, symmetric write-set) rather than a canned stub:
 *   - find_unflagged_object_by_key 0x303ab  (read-only object-table search; regs preserved; the +8/+0xa
 *     "find" directory is staged per-case for match / no-match).
 *   - fire_trigger_on_interact 0x31ffe      (fully lifted; return discarded; record byte[+2]&0x29 forces
 *     the bounded gated-tail early-return).
 *   - dispatch_entry_command_trigger 0x34d75 + dispatch_entry_command_trigger_b 0x34f5a (read-only
 *     match/dispatch; EDX preserved on every exit path — disasm-confirmed; the object-table channel
 *     directories + geom cell flags are left empty so the real predicate returns 0 = the inactive path).
 *   - fire_object_use_trigger 0x10d1e       (fully lifted, pushal-framed; dispatch returns 0 for the
 *     type-4/5 objects here, so it returns 0 after only zeroing 0x7e928).
 *   - run_leftclick_object_trigger 0x303ff  (fully lifted, pushal-framed; non-keyed def id=0 -> early
 *     return; keyed reuses the +8/+0xa entry with command base 0 -> matches + seeds, returns -1 WITHOUT
 *     firing a chain).
 * Key enabler: 0x303ab/0x10d1e/0x303ff are pushal- or push/pop-framed and 0x34d75/0x34f5a save+restore
 * EDX on every exercised exit, so the old stub_iorec edx_ret was a passthrough ARTIFACT — the real
 * callees preserve EDX, which the callers' `return io.edx` A1 paths now carry faithfully.  See the
 * per-site [REPOINT] markers.
 *
 * [REPOINT] the picker call_orig cluster is now DIRECT C — 0x1dc73 eval_dialogue_record_by_id,
 * 0x1ddeb find_oninspect_block_by_id, 0x1dd50 is_item_id_pickable, 0x1dcef give_item_by_dbase_id,
 * 0x41f24 destroy_dynamic_entity, 0x1f71d update_dialogue_choice_highlight, 0x1ad2f
 * hit_test_dialogue_ui_action, 0x1b4e5 dispatch_dialogue_ui_action, 0x1f671 dialogue_voice_force_end.
 * Each site runs the real (already-lifted) callee over a BOUNDED real fixture staged in test_input.c so
 * both the oracle's original-side and the lift-side execute the same body symmetrically (pure read-only
 * leaves; the id<0x200 give early-return + a full staged give-success for pickup_ok with its obj3
 * sub-bridges stubbed both sides; the destroy out-of-pool bail; the 0x827fd/0x71241 dialogue-UI tables;
 * the 0x8200c!=1 non-mixer voice path). Register frames were disasm-confirmed preserving in every case.
 *
 * toggle_door_open_state 0x3d93f (type-6 door site, above) is NOW direct-C — the last picker bridge is
 * retired. The earlier premise ("stop_sound clobbers EDX -> needs an EDX-output proto") was WRONG: the
 * disasm shows the door PRESERVES EDX (0x26d8a stop_sound push/pops it; the play path wraps it in
 * push/pop edx), so the caller's `return io.edx` already carries the live cursor code with no proto
 * change. The A1 "callee EDX" the old stub_iorec injected was a passthrough artifact (same finding as
 * slot-0 0x303ab). test_input now stages a real door (0x1e/0x26=0 -> both sound sub-calls
 * skipped) and runs 0x3d93f real+symmetric on both sides. */
#define RD8(a)  (*(uint8_t  *)(uintptr_t)(a))
#define RD16(a) (*(uint16_t *)(uintptr_t)(a))
#define RD32(a) (*(uint32_t *)(uintptr_t)(a))

/* examine_world_object (0x35235, 43B): RMB-inspect a found world object. Clears the inspect
 * scratch 0x8a0dc / 0x8a260, then evals the object's dialogue record by id ([rec+0xa]) and
 * fires its on-interact trigger (ESI=rec live across both calls; the second call consumes
 * the first's EAX). The original's `push ds; pop es` is bridge-ambient (host segs flat). */
void examine_world_object(uint32_t rec)
{
    G32(VA_g_pending_command_record) = 0;
    G32(VA_g_state_link_buf_ptr + 0x124) = (int32_t)0x80008000;
    regs_t io = {0};
    io.esi = rec;                                     /* mov esi,eax */
    /* [REPOINT] direct C: eval_dialogue_record_by_id 0x1dc73 (pushes ebx/edx -> preserved; EAX
     * out). The examine test stages g_dbase100_base 0x81e1c=0 so the real body takes the no-base early
     * return (xor eax,eax) WITHOUT entering the interpreter — a real, isolated, crash-free path,
     * symmetric on both sides. Its interpreter/run-now branch is exhaustively covered in test_dbase100.
     * The result feeds fire_trigger_on_interact, which ignores it here (byte[rec+6]&0x20 clear). */
    io.eax = eval_dialogue_record_by_id(RD16(rec + 0xa)); /* 0x1dc73 re-pointed */
    /* [REPOINT] direct C: fire_trigger_on_interact 0x31ffe is fully lifted (walk pre-pass +
     * gated tail + register_command_save_link, all lifted). Its return is DISCARDED here (last call of a
     * void fn). The examine test stages the record's byte[+2]&0x29 (mid-fire/inactive) so the real body
     * takes the bounded gated-tail early-return, symmetric on both sides. */
    (void)fire_trigger_on_interact(io.esi, io.eax); /* 0x31ffe re-pointed (esi=rec, eax=1dc73 result) */
}

/* examine_object_under_cursor (0x10cb3, 107B): the RMB examine path. Clears the held-button
 * byte, probes the object via dispatch_entry_command_trigger_b (EAX=EDX=the object, EBX=the
 * cursor-Y camera delta [0x7e908]-[0x85ce4]; the X-delta the original also computes is DEAD
 * — overwritten by `mov edx,eax`); a hit examines the FOUND record (the callee's EAX) via
 * examine_world_object (in-subsystem, direct C) -> returns -1. Otherwise, for a type-4
 * object, resolves its OnInspect block (find_oninspect_block_by_id on the def id) and evals
 * its condition (EDX=1) -> -1; else 0. pushal-framed (ABI_FIRER preserves). */
int32_t examine_object_under_cursor(const regs_t *in)
{
    G8(VA_g_mouse_buttons_held) = 0;
    regs_t io = *in;
    io.ebx = (uint32_t)(G32(VA_g_saved_int9_offset + 0x20) - G32(VA_g_view_y)); /* cursor-Y camera delta */
    io.edx = in->eax;                                 /* mov edx,eax */
    /* [REPOINT] direct C: dispatch_entry_command_trigger_b 0x34f5a is a read-only match/dispatch
     * that PRESERVES EDX on every exit path (each channel push edx ... pop edx; the ret-0 tails pop only
     * ebx/edi) — disasm-confirmed. With the object-table channel directories (+0x30..+0x3a / +0x34) empty
     * and the geom cell flag clear, the real predicate scans and returns 0 (no match) — the sanctioned
     * inactive/no-op path. io.edx (= the object, A1) is carried through faithfully by the preservation. */
    io.eax = (uint32_t)dispatch_entry_command_trigger_b(&io); /* 0x34f5a re-pointed */
    if (io.eax != 0) {
        examine_world_object(io.eax);          /* call 0x35235 (found record) */
        return -1;
    }
    if (RD8(io.edx + 1) == 4) {                       /* the callee's EDX = the object (A1) */
        uint32_t def = RD32(io.edx + 0xe);
        /* [REPOINT] direct C: find_oninspect_block_by_id 0x1ddeb is a pure read-only DBASE100
         * cache/table probe (pushes ebx/ecx/edx/esi -> all preserved; only EAX out). id = u16[def+4].
         * For the id<0x200 examine ids here it takes the cache-hit fast path (id==[0x81efa] -> [0x81efe])
         * or the cache-miss zero-then-return-0 path (both symmetric on both sides). Test stages the
         * 0x81efa/0x81efe cache pair per the old canned r1ddeb. */
        io.eax = find_oninspect_block_by_id(RD16(def + 4)); /* 0x1ddeb re-pointed */
        if (io.eax != 0) {
            /* re-pointed: eval_dialogue_record_condition_with_cleanup 0x1db89 [L] direct-C
             * (EAX=OnInspect block, EDX=1). Its RESULT IS DISCARDED (we return -1 unconditionally), so
             * only crash-safety matters: t_examine_pair now points find_oninspect's cache 0x81efe at a
             * REAL terminate-chain record (header 0x08 + REC(0x0a) -> the interpreter walks one record and
             * returns), staged with the eval prerequisites (0x81e3e/0x81e18/0x81eb2/0x83c70=0), so the real
             * interpreter runs REAL + symmetric on both sides (recipe-2, cf. test_dbase100 eval fixture). */
            (void)eval_dialogue_record_condition_with_cleanup(io.eax, 1, (uint32_t *)0, (uint32_t *)0);
            return -1;                                /* jmp 0x10ce9 */
        }
    }
    return 0;                                         /* 0x10d16: sub eax,eax */
}

/* activate_targeted_object (0x164c9, 188B): the LMB use path. EAX = the object record,
 * EDX = the DEFAULT cursor code (the caller passes 0x268) — several fail paths return the
 * LIVE EDX (threaded through every bridged callee, gotcha A1; the door path returns the
 * door-toggle's EDX output outright). Type 4: keyed objects ([def+9]&1) need
 * find_unflagged_object_by_key + run_leftclick_object_trigger; plain objects run the
 * left-click trigger, then either fire_object_use_trigger (-> 0x268/0x240) or pick the item
 * up (give_item_by_dbase_id; on success destroy_dynamic_entity with EDX=the pickup-fly
 * position 0x7114c [reloc'd] -> 0x268). Type 6: toggle_door_open_state on [rec+0x1c].
 * Anything else: fire_object_use_trigger then return the live EDX. */
int32_t activate_targeted_object(const regs_t *in)
{
    uint32_t rec = in->eax;                           /* mov ebx,eax */
    regs_t io = *in;
    io.ebx = rec;
    if ((int32_t)RD32(rec + 0x20) >= 0x258)           /* distance gate (signed) */
        return (int32_t)io.edx;                       /* 0x16581: mov eax,edx */
    uint32_t type = RD8(rec + 1);
    if (type == 4) {
        uint32_t def = RD32(rec + 0xe);
        uint8_t  fl  = RD8(def + 9);
        if (fl & 0x12) return (int32_t)io.edx;
        if (fl & 1) {                                 /* keyed object */
            /* [REPOINT] direct C: find_unflagged_object_by_key 0x303ab is a pure read-only
             * object-table search that PRESERVES every register (push edx/ecx/esi/edi/ebx ... pop) —
             * disasm-confirmed. The old stub_iorec's edx_ret=rec was a passthrough artifact; the real
             * callee leaves io.edx = its pre-call value (= in->edx here), which this `return io.edx`
             * path faithfully carries. Test stages the object table +8/+0xa directory. */
            io.eax = (uint32_t)find_unflagged_object_by_key((uint8_t *)(uintptr_t)def);
            if (io.eax == 0) return (int32_t)io.edx;
            io.eax = RD32(io.ebx + 0xe);              /* mov eax,[ebx+0xe] (post-call ebx) */
            /* [REPOINT] direct C: run_leftclick_object_trigger 0x303ff is fully lifted and
             * pushal/popal-framed -> preserves EDX/EBX. On the keyed path the test's +8/+0xa object-table
             * entry (staged for 0x303ab) has command base 0, so the real walk matches, seeds
             * 0x8a100/0x8a134, and returns -1 WITHOUT reaching cmd_light_switch/run_command_chain — a
             * bounded dispatch, no unstageable breadth. */
            io.eax = (uint32_t)run_leftclick_object_trigger(&io); /* 0x303ff re-pointed */
            if (io.eax == 0) return (int32_t)io.edx;
            return 0x268;                             /* 0x16516 */
        }
        if (G8(VA_g_item_pickup_flags) & 1) return (int32_t)io.edx;  /* 0x1651d: pickup lock live */
        if (fl & 0x10) return (int32_t)io.edx;
        io.eax = def;                                 /* eax still = def at 0x1652c */
        io.eax = (uint32_t)run_leftclick_object_trigger(&io); /* 0x303ff re-pointed (non-keyed def id=0 -> bounded early-return) */
        if (io.eax != 0) {
            io.eax = io.ebx;                          /* 0x16554: mov eax,ebx */
            /* [REPOINT] direct C: fire_object_use_trigger 0x10d1e is fully lifted (dispatch +
             * contact fixup) and pushal/popal-framed -> preserves EDX. It hands the object to
             * dispatch_entry_command_trigger, which returns 0 for the type-4 objects here (only 3/8/2
             * dispatch), so fire returns 0 after only zeroing 0x7e928 -> the sanctioned no-op path. */
            io.eax = (uint32_t)fire_object_use_trigger(&io); /* 0x10d1e re-pointed */
            if (io.eax != 0) return 0x268;
            return 0x240;                             /* 0x1655f */
        }
        uint32_t d2 = RD32(io.ebx + 0xe);             /* mov edx,[ebx+0xe] */
        /* [REPOINT] direct C: give_item_by_dbase_id 0x1dcef (pushes ebx/ecx/esi/edi ->
         * preserved; EAX out). id<0x200 -> 0 without mutation (every case here but pickup_ok); pickup_ok
         * stages a REAL DBASE100 give-index + cleared inventory + drop ctx (recipe-3, ported from
         * test_inventory give_item[stack_new]) with give_item's 3 obj3 sub-bridges (0x1bb4b/0x184ab/
         * 0x2245c) byte-stubbed on BOTH sides, so the real give runs to a non-weapon stackable slot and
         * returns the slot index — symmetric. */
        io.eax = give_item_by_dbase_id(RD16(d2 + 4), d2); /* 0x1dcef re-pointed */
        if (io.eax == 0) return 0x240;                /* je 0x1655f */
        /* [REPOINT] direct C: destroy_dynamic_entity 0x41f24 (push gs; pushal-ish frame). The
         * pickup_ok test stages g_map_objects_buffer 0x90aa4 / pool-size 0x85c2c so the object offset is
         * out of pool bounds -> the real body bails after ONLY the 16-byte record copy to the pickup-fly
         * scratch 0x7114c (no gs pool touch), symmetric on both sides. Full entity-pool removal is
         * covered in test_entity_ai. */
        destroy_dynamic_entity(RD32(io.ebx + 0xe), 0x7114c + OBJ_DELTA); /* 0x41f24 re-pointed */
        return 0x268;                                 /* jmp 0x16516 */
    }
    if (type == 6) {                                  /* 0x16568: door */
        /* re-pointed: toggle_door_open_state 0x3d93f [L, doors] direct-C. The original's `mov eax,edx`
         * after the call returns the LIVE EDX, which the door PRESERVES end-to-end: the flag toggle
         * touches no reg; its stop-sound sub-call 0x26d8a push/pops EDX (0x26d8c/0x26dda), and the
         * play-sound path (0x27207) is wrapped in a local push edx/pop edx (0x3d913/0x3d933). So EDX-out
         * == EDX-in — the earlier "stop_sound clobbers EDX" note was wrong. Thus io.edx here (== in->edx,
         * the default cursor code) is exactly what the original returns; the toggle's EAX(=rec) is dropped
         * (the `mov eax,edx` overwrites it). test_input stages a real door (0x1e/0x26=0 -> no sound). */
        (void)toggle_door_open_state(RD16(rec + 0x1c));
        return (int32_t)io.edx;                       /* mov eax,edx: the preserved live cursor code */
    }
    io.eax = io.ebx;                                  /* 0x1657a */
    io.eax = (uint32_t)fire_object_use_trigger(&io); /* 0x10d1e re-pointed (EDX preserved via pushal, no-op for type 5) */
    return (int32_t)io.edx;                           /* falls into 0x16581 */
}

/* classify_cursor_target_object (0x1624d, 636B): decide the interaction the object under
 * the cursor offers, per frame. EAX = the object record; EDX (the live record cursor) is
 * threaded through every probe callee. Writes the interaction type byte 0x7e932 (1 none /
 * 3 use / 5 blocked) + the flag bits 0x7fdac (bit1 = has command trigger, bit2 = blocked/
 * keyed), the inspectable counter 0x7fdb0, the out-of-range latch 0x7fdb4; returns the
 * cursor-shape id (0x240/0x248/0x250/0x258/0x260/0x278/0x398/0x3a0), banded by the
 * distance^2 thresholds 600 / 2000 / 5000. Faithful quirk: the pickable probe at 0x16429
 * sets 0x7e932=3 BEFORE its tests — it sticks even when the probe then bails. */
int32_t classify_cursor_target_object(const regs_t *in)
{
    regs_t io = *in;
    io.edx = in->eax;                                 /* mov edx,eax */
    G32(VA_g_cursor_interaction_flags) = 0;
    G32(VA_g_cursor_interaction_flags + 0x4) = 0;
    G32(VA_g_cursor_interaction_flags + 0x8) = 0;
    uint32_t type = RD8(in->eax + 1);
    if (type == 4) {
        uint32_t def = RD32(io.edx + 0xe);
        uint8_t  fl  = RD8(def + 9);
        if (fl & 1) {                                 /* keyed object */
            if ((int32_t)RD32(io.edx + 0x20) >= 0x258) goto L162b3;
            io.eax = (uint32_t)find_unflagged_object_by_key((uint8_t *)(uintptr_t)def); /* 0x303ab re-pointed (read-only, regs preserved) */
            if (io.eax == 0) goto L162b3;
            G8(VA_g_interaction_cursor_type) = 3;
            return (G32(VA_g_cursor_interaction_flags + 0x4) != 0) ? 0x250 : 0x248;
        }
        if (fl & 0x20) {
            /* [REPOINT] direct C: read-only trigger-link predicate (no writes, no calls);
             * test stages objtbl 0x85c30 use-dir so the real body returns -1 (was stub_iorec slot 1). */
            if (object_has_active_trigger_link(io.edx) != 0) G8(VA_g_cursor_interaction_flags) |= 4; /* call 0x30c1a */
        }
    } else if (type == 3) {                           /* wall/sector face */
        uint32_t geom = (uint32_t)G32(VA_g_map_geometry_buffer);       /* g_map_geometry_buffer (A4 raw) */
        uint32_t si   = RD16(io.edx + 8);
        uint32_t k    = RD16(geom + si + 4);
        if (RD8(geom + k + 9) & 2) {
            /* [REPOINT] direct C: same predicate, contact channel (test leaves the objtbl
             * contact-dir empty so this type-3 case early-returns 0, matching the old stub r30c1a=0). */
            if (object_has_active_trigger_link(io.edx) != 0) G8(VA_g_cursor_interaction_flags) |= 4; /* call 0x30c1a */
        }
    }
    /* 0x16309 */
    if ((int32_t)RD32(io.edx + 0x20) >= 0x1388) goto L164a2;
    io.eax = io.edx;
    io.eax = (uint32_t)dispatch_entry_command_trigger_b(&io); /* 0x34f5a re-pointed (no-op path, EDX preserved) */
    if (io.eax != 0) {
        G8(VA_g_cursor_interaction_flags) |= 2;
        G32(VA_g_cursor_interaction_flags + 0x4) += 1;
    }
    if (G32(VA_g_cursor_interaction_flags + 0x4) == 0 && RD8(io.edx + 1) == 4) {
        uint32_t def = RD32(io.edx + 0xe);
        if ((RD8(def + 9) & 0x13) == 0) {
            /* [REPOINT] direct C: find_oninspect_block_by_id 0x1ddeb (pure read-only, regs
             * preserved). Same cache/return-0 bounded paths as the examine site; test stages 0x81efa/
             * 0x81efe per the old canned r1ddeb. */
            io.eax = find_oninspect_block_by_id(RD16(def + 4)); /* 0x1ddeb re-pointed */
            if (io.eax != 0) G32(VA_g_cursor_interaction_flags + 0x4) += 1;
        }
    }
    if (RD8(io.edx + 1) == 6) {                       /* 0x16360: door */
        if ((int32_t)RD32(io.edx + 0x20) < 0x258) {
            G8(VA_g_interaction_cursor_type) = 3;                          /* 0x1636e */
            goto L16375;
        }
        G32(VA_g_cursor_interaction_flags + 0x8) = 1;                             /* 0x1638a */
        if ((int32_t)RD32(io.edx + 0x20) < 0x7d0) {
            if (G32(VA_g_cursor_interaction_flags + 0x4) != 0) return 0x3a0;      /* 0x163a6 */
            G8(VA_g_interaction_cursor_type) = 1;
            return 0x398;                             /* 0x1640f */
        }
        if (G32(VA_g_cursor_interaction_flags + 0x4) == 0) goto L164ba;
        return 0x278;                                 /* 0x163c4 */
    }
    /* 0x163cc: not a door */
    if ((int32_t)RD32(io.edx + 0x20) < 0x258) {
        io.eax = io.edx;
        /* [REPOINT] direct C: dispatch_entry_command_trigger 0x34d75 is a read-only entry-trigger
         * dispatcher that handles only record types 3/8/2; for the type-4/5 objects reaching here it runs
         * the real type predicate and returns 0 WITHOUT touching EDX (disasm ret-0 tail 0x34f1d pops only
         * ebx/edi) — the sanctioned inactive/no-op path. io.edx (=the record) is preserved for the reads
         * below. The old stub's r34d75 was a value the real body cannot produce for a type-4/5 record. */
        io.eax = (uint32_t)dispatch_entry_command_trigger(&io); /* 0x34d75 re-pointed */
        if (io.eax != 0) {
            G8(VA_g_interaction_cursor_type) = 3;                          /* jmp 0x1636e */
            goto L16375;
        }
    } else if (!(G8(VA_g_cursor_interaction_flags) & 4) && (int32_t)RD32(io.edx + 0x20) < 0x7d0) {
        io.eax = io.edx;
        io.eax = (uint32_t)dispatch_entry_command_trigger(&io); /* 0x34d75 re-pointed (no-op for type 4/5, EDX preserved) */
        if (io.eax != 0) {
            G8(VA_g_interaction_cursor_type) = 3;                          /* 0x163ff */
            if (G32(VA_g_cursor_interaction_flags + 0x4) != 0) return 0x3a0;
            return 0x398;                             /* 0x1640f */
        }
    }
    /* 0x16417: the pickable probe */
    if (RD8(io.edx + 1) == 4) {
        uint32_t def = RD32(io.edx + 0xe);
        if ((RD8(def + 9) & 0x13) == 0) {
            G8(VA_g_interaction_cursor_type) = 3;                          /* set EARLY — sticks on bail */
            io.eax = (uint32_t)find_unflagged_object_by_key((uint8_t *)(uintptr_t)def); /* 0x303ab re-pointed (io.edx=edx preserved) */
            if (io.eax == 0) {
                uint32_t d2 = RD32(io.edx + 0xe);
                /* [REPOINT] direct C: is_item_id_pickable 0x1dd50 (pushes ebx/ecx/edx/esi ->
                 * preserved; EAX out). Pure read-only DBASE100 template query: id<0x200 -> 0; else scans
                 * the index for a matching, pickable (bit7 clear, category nibble != 2) template. The
                 * classify test drives both branches with real staging (item ids <0x200 return 0;
                 * pick_mid stages a real id>=0x200 pickable template -> 1) — symmetric on both sides. */
                io.eax = is_item_id_pickable(RD16(d2 + 4)); /* 0x1dd50 re-pointed */
                if (io.eax == 0) goto L16475;
            }
            if ((int32_t)RD32(io.edx + 0x20) < 0x258) goto L16375;
            G32(VA_g_cursor_interaction_flags + 0x8) = 1;
            if (G8(VA_g_cursor_interaction_flags) & 4) goto L16475;
            if ((int32_t)RD32(io.edx + 0x20) < 0x7d0) { /* jl 0x16406 */
                if (G32(VA_g_cursor_interaction_flags + 0x4) != 0) return 0x3a0;
                return 0x398;
            }
        }
    }
L16475:
    if (G32(VA_g_cursor_interaction_flags + 0x4) == 0) goto L164a2;
    if (G8(VA_g_cursor_interaction_flags) & 4) {
        G8(VA_g_interaction_cursor_type) = 5;
        return 0x260;                                 /* 0x1648e */
    }
    G8(VA_g_interaction_cursor_type) = 3;
    return 0x278;                                     /* jmp 0x163c4 */
L16375:
    return (G32(VA_g_cursor_interaction_flags + 0x4) != 0) ? 0x250 : 0x248;       /* 0x162ab / 0x16382 */
L164a2:
    if (G8(VA_g_cursor_interaction_flags) & 4) {
        G8(VA_g_interaction_cursor_type) = 5;
        return 0x258;                                 /* 0x164b2 */
    }
L164ba:
    G8(VA_g_interaction_cursor_type) = 1;
    return 0x240;
L162b3:
    G8(VA_g_interaction_cursor_type) = 5;
    G8(VA_g_cursor_interaction_flags) |= 4;                                 /* or byte [0x7fdac],4 */
    return 0x258;
}

/* handle_cursor_click (0x1661f, 440B): route a mouse event over the dialogue/inspect UI.
 * Builds the click code ECX (1 = primary edge, 2 = secondary edge, held buttons as
 * fallback, 8 = the inspect popup [0x80b30] hold), then:
 *   HOVER (ecx==0): update_dialogue_choice_highlight (when dialogue active; ==1 -> arrow
 *   0x248), else hit_test_dialogue_ui_action (EBX=0) and map the action id to a shape —
 *   choice rows 0x1c..0x25 also move the choice cursor (move_cursor_entry_clamped) when
 *   the row changed ([0x81300] latch); id 0x27 = the voice-skip zone (0x240 when the
 *   dialogue freeze is full, else shape 8).
 *   CLICK: hit_test with EBX=the click code; id 0x27 -> dialogue_voice_force_end (+shape
 *   0x268/[0x7f564]=2 under full freeze, else 0x108/[0x7f564]=1 — the gate is read BEFORE
 *   the call, the callee clears it); id 4 -> dispatch_dialogue_ui_action -> 0x100; other
 *   nonzero ids dispatch too, then 0x268/0x270 by the held-secondary bit. The chosen shape
 *   goes to set_cursor_shape; both click-edge flags are cleared on every exit. */
void handle_cursor_click(void)
{
    regs_t io = {0};
    uint32_t shape;
    uint32_t ecx = 0;                                 /* xor ecx,ecx */
    if (G8(VA_g_cursor_secondary_action_flag) != 0) ecx = 2;
    if (G8(VA_g_cursor_primary_action_flag) != 0) ecx += 1;
    if (ecx == 0) ecx = (uint32_t)(G8(VA_g_mouse_buttons_prev) & 3);  /* held buttons */
    if (ecx == 0 && G32(VA_g_inventory_ui_action) != 0) ecx = 8;       /* inspect popup hold */
    if (ecx == 0) {
        /* hover path (0x16664) */
        if (G32(VA_g_dialogue_busy_flag) != 0) {
            io.edx = (uint32_t)G32(VA_g_mouse_y);
            io.eax = (uint32_t)G32(VA_g_mouse_x);
            /* [REPOINT] direct C: update_dialogue_choice_highlight 0x1f71d (pushes ebx/ecx/esi/
             * edi + enter frame -> preserved; EAX out). Gated body: (0x83aea==0 || 0x83125!=0x6ffff)->0.
             * The click test stages the 0x827fd segment table so the real region-test returns 1 for the
             * hover_highlight case (-> 0x248), symmetric on both sides. */
            io.eax = update_dialogue_choice_highlight((int32_t)io.eax, (int32_t)io.edx); /* 0x1f71d re-pointed */
            if (io.eax == 1) { shape = 0x248; goto setcur; }
        }
        io.edx = (uint32_t)G32(VA_g_mouse_y);
        io.eax = (uint32_t)G32(VA_g_mouse_x);
        io.ebx = 0;
        /* [REPOINT] direct C: hit_test_dialogue_ui_action 0x1ad2f (pushes ecx/esi/edi + enter
         * frame -> preserved; EAX out). Pure region-test leaf over the dialogue-UI slot/scroll tables.
         * The click test stages the 0x71241 slot-entry list (+ bounds 0x80b24/0x80b28) so the real scan
         * returns each case's action id — symmetric on both sides. */
        io.eax = (uint32_t)hit_test_dialogue_ui_action((int32_t)io.eax, (int32_t)io.edx, io.ebx); /* 0x1ad2f re-pointed */
        uint32_t act = io.eax;
        if (act < 0x1c) { shape = (act == 0) ? 0x240 : 0x248; goto setcur; }
        if (act <= 0x25) {                            /* choice rows */
            if (act != (uint32_t)G32(VA_g_inventory_synthetic_primary + 0x4)) {
                G32(VA_g_inventory_synthetic_primary + 0x4) = (int32_t)act;
                /* [REPOINT] direct C: pure obj3 delta-clamp (0x80afc/0x80b10/0x80af4/
                 * 0x80b38/0x7f571). Return discarded; test stages those globals so the real body
                 * runs deterministic + symmetric on both sides (was stub_iorec slot 2). */
                move_cursor_entry_clamped(act - 0x1c);   /* call 0x1bb12 */
            }
            shape = 0x250; goto setcur;
        }
        if (act < 0x27)  { shape = 0x248; goto setcur; }
        if (act <= 0x27) {                            /* the voice-skip zone */
            shape = (G32(VA_g_move_freeze_gate) == 0x6ffff) ? 0x240 : 8;
            goto setcur;
        }
        if (act < 0x2a)  { shape = 0x248; goto setcur; }
        if (act <= 0x2b) { shape = 0x250; goto setcur; }
        shape = 0x248; goto setcur;
    }
    /* click path (0x16704) */
    if ((((uint32_t)G8(VA_g_cursor_primary_action_flag) | (uint32_t)G8(VA_g_cursor_secondary_action_flag)) | (ecx & 8)) == 0)
        goto clear;                                   /* held-only, no popup: just clear */
    io.edx = (uint32_t)G32(VA_g_mouse_y);
    io.eax = (uint32_t)G32(VA_g_mouse_x);
    io.ebx = ecx;
    G8(VA_g_mouse_buttons_prev + 0x7) = 0;
    /* [REPOINT] direct C: hit_test_dialogue_ui_action 0x1ad2f (EBX=click code; regs preserved).
     * ebx&8 -> scroll-drag branch (popup_hold: 0x80b30 staged to a non-1/2 value -> returns 0); else the
     * staged slot scan returns the case's action id. Symmetric on both sides. */
    io.eax = (uint32_t)hit_test_dialogue_ui_action((int32_t)io.eax, (int32_t)io.edx, io.ebx); /* 0x1ad2f re-pointed */
    uint32_t act = io.eax;
    if (act == 0x27) {                                /* voice skip */
        int full = (G32(VA_g_move_freeze_gate) == 0x6ffff);         /* read BEFORE the callee clears it */
        io.edx = (uint32_t)G32(VA_g_mouse_y);
        io.eax = (uint32_t)G32(VA_g_mouse_x);
        /* [REPOINT] direct C: dialogue_voice_force_end 0x1f671. The click test stages the SOS
         * voice-active latch 0x8200c!=1 so the real body takes the non-mixer path (no sos_stop_voice
         * host-boundary): 0x83125==0x6ffff -> the internal update_dialogue_choice_highlight (0x83aea==0 ->
         * returns 0, no close); else -> zero 0x83125. Symmetric on both sides. */
        dialogue_voice_force_end(io.eax, io.edx); /* 0x1f671 re-pointed */
        if (full) { shape = 0x268; G32(VA_g_dev_mode_flag + 0x4) = 2; }
        else      { shape = 0x108; G32(VA_g_dev_mode_flag + 0x4) = 1; }
        goto setcur;
    }
    if (act == 4) {
        /* [REPOINT] direct C: dispatch_dialogue_ui_action 0x1b4e5 (EAX=action, EDX=flags/click;
         * pushes ebx/ecx/esi/edi -> preserved; return discarded here). action 4 = the scroll-drag state
         * machine: the test stages the drag latch 0x810c0 to a non-1/3 value so the real body falls to
         * the done tail (clears 0x7e938/0x7e939, no bridges) — bounded, symmetric. */
        dispatch_dialogue_ui_action(act, ecx, 0, 0); /* 0x1b4e5 re-pointed */
        shape = 0x100; goto setcur;
    }
    if (act != 0) {
        /* [REPOINT] direct C: dispatch_dialogue_ui_action 0x1b4e5. For the action-9 case this is
         * the actions-7..0x16 tail (0x81304 = action-6, then done) — bounded, symmetric. */
        dispatch_dialogue_ui_action(act, ecx, 0, 0); /* 0x1b4e5 re-pointed */
    }
    shape = (G8(VA_g_mouse_buttons_prev) & 2) ? 0x268 : 0x270;
setcur:
    /* [REPOINT] direct C: set_cursor_shape. Test forces 0x7f56c=0 (shape table absent)
     * + 0x90bcc=0 (no keymap remap) + 0x708e4=sentinel, so the real body stores 0x708e4=shape
     * and returns before the RLE decode/blit — a verified subset (cf. test_blit_2d "no-table"),
     * observing the computed shape via the real cursor-shape global (was stub_iorec slot 5). */
    set_cursor_shape(shape);                   /* call 0x12a08 */
clear:
    G8(VA_g_cursor_primary_action_flag) = 0;
    G8(VA_g_cursor_secondary_action_flag) = 0;
}

/* ===================== E. console field editing ===================== */
/* The DOS console echo helpers are CRT bridges: dos_print_char 0x114d4 (builds
 * "<al>$" on the stack, int21 AH=9) and dos_print_concat 0x114a3 (concats EDX+EAX strings,
 * prints, and RETURNS ECX = strlen of the EAX string — an A1 multi-reg return the editor's
 * length tracking depends on). Both bridged via call_orig; the oracle NOPs their inline
 * int21s so the real (pure) logic runs on both sides. */

/* console_read_key_crlf (0x11337, 22B): echo the caller's AL, then LF + CR. EAX preserved. */
void console_read_key_crlf(uint32_t eax)
{
    in_bridge_eax(0x114d4, eax);                      /* echo AL */
    in_bridge_eax(0x114d4, (eax & 0xffffff00u) | 0xa);/* mov al,0xa (AH kept live) */
    in_bridge_eax(0x114d4, (eax & 0xffffff00u) | 0xd);/* mov al,0xd */
}

/* console_read_key (0x11548, 23B): dequeue+translate one key (call-closed into
 * dequeue_translated_key); '$' passes through verbatim; lowercase folds to uppercase
 * (and al,0xdf). Returns EAX (upper bits per the dequeue semantics). */
uint32_t console_read_key(uint32_t eax)
{
    dequeue_translated_key(&eax);              /* call 0x129ca */
    uint8_t al = (uint8_t)eax;
    if (al != 0x24 && al >= 0x61 && al <= 0x7a)       /* cmp/jb/ja */
        eax = (eax & 0xffffff00u) | (al & 0xdfu);     /* and al,0xdf */
    return eax;
}

/* console_edit_field (0x113c0, 162B; DEAD) — the BLOCKING console line editor (spins on the
 * scancode ring; live-swap would need interactive mode, but the fn is statically dead — the
 * oracle drives it with scripted ring content ending in Enter). ECX = max chars, ESI = the
 * edit buffer (RUNTIME ptr, raw deref). Loop: redraw (CR + dos_print_concat -> current
 * length into 0x76848), then keys: 0x0a (Enter) -> done (clc); 1/4 (Backspace/Left) ->
 * backspace (echo BS,SP,BS + truncate, if len>0); <=7 ignored; numeric mode ([0x76854]!=0)
 * accepts only '0'-'9'; append at [base+len] + re-terminate while len < max (SIGNED jge).
 * console_edit_text_field (0x113b9, 7B; DEAD) = the alnum-mode entry stub. */
void console_edit_field(uint32_t ecx, uint32_t esi)
{
    G32(VA_g_console_input_maxlen) = (int32_t)ecx;                      /* max chars */
    G32(VA_g_console_input_buffer) = (int32_t)esi;                      /* buffer ptr (stored runtime ptr) */
    for (;;) {
        in_bridge_eax(0x114d4, 0xd);                  /* 0x113cc: CR */
        regs_t io = {0};
        io.eax = (uint32_t)G32(VA_g_console_input_buffer);
        io.va  = 0x114a3 + OBJ_DELTA;                 /* dos_print_concat (edx=0) */
#ifndef ROTH_STANDALONE
        call_orig(&io);
#else
        io.ecx = in_if_dos_print_concat(io.eax, io.edx);   /* route over the soft-int hook; ECX =
                                                            * strlen(EAX str) threaded (A1 return) */
#endif
        G32(VA_g_console_input_pos) = (int32_t)io.ecx;               /* current length (A1 threaded) */
        for (;;) {                                    /* 0x113e5: key loop */
            uint8_t al = (uint8_t)console_read_key(0);
            if (al == 0xa) return;                    /* Enter: clc; ret */
            if (al == 1 || al == 4) {                 /* 0x1142d: backspace */
                if (G32(VA_g_console_input_pos) == 0) continue;
                in_bridge_eax(0x114d4, 8);
                in_bridge_eax(0x114d4, 0x20);
                in_bridge_eax(0x114d4, 8);
                uint32_t p = (uint32_t)G32(VA_g_console_input_buffer) + (uint32_t)G32(VA_g_console_input_pos);
                *(uint8_t *)(uintptr_t)(p - 1) = 0;
                break;                                /* jmp redraw */
            }
            if (al <= 7) continue;
            if (G8(VA_g_console_input_numeric_only) != 0 && (al < 0x30 || al > 0x39))
                continue;                             /* numeric mode: digits only */
            if (G32(VA_g_console_input_pos) >= G32(VA_g_console_input_maxlen)) continue; /* full (SIGNED jge) */
            uint32_t p = (uint32_t)G32(VA_g_console_input_buffer) + (uint32_t)G32(VA_g_console_input_pos);
            *(uint8_t *)(uintptr_t)p = al;            /* append */
            *(uint8_t *)(uintptr_t)(p + 1) = 0;
            break;                                    /* jmp redraw */
        }
    }
}
void console_edit_text_field(uint32_t ecx, uint32_t esi)
{
    G8(VA_g_console_input_numeric_only) = 0;                                  /* alnum mode */
    console_edit_field(ecx, esi);
}

/* console_edit_numeric_field (0x11462, 65B; DEAD): edit a WORD value as decimal text.
 * ESI = pointer to the value word. Renders it into the scratch text buffer 0x7073d
 * (num_to_decimal_digits 0x1155f [L] — direct C, returns the advanced cursor), edits with
 * max 4 chars in numeric mode, then parses the buffer back: the original's digit check is
 * `sub al,0x30; cmp al,0x39; ja done` — it accepts bytes 0x3a..0x69 as "digits" 10..57
 * (transcribed faithfully; harmless since numeric mode only lets real digits in). */
void console_edit_numeric_field(uint32_t esi)
{
    G8(VA_g_console_input_numeric_only) = 0xff;                               /* numeric mode */
    uint16_t val = *(uint16_t *)(uintptr_t)esi;       /* mov ax,[esi] */
    uint8_t *edi = num_to_decimal_digits(val, (uint8_t *)(uintptr_t)(0x7073d + OBJ_DELTA));
    *edi = 0;                                         /* sub eax,eax; stosb */
    console_edit_field(4, 0x7073d + OBJ_DELTA);/* ecx=4; esi=scratch */
    uint32_t p = 0x7073d + OBJ_DELTA;                 /* 0x1148c: parse back */
    uint32_t edx = 0;
    for (;;) {
        uint8_t d = (uint8_t)(*(uint8_t *)(uintptr_t)p - 0x30); /* lodsb; sub al,0x30 */
        p++;
        if (d > 0x39) break;                          /* ja done (faithful loose check) */
        edx = edx * 10 + d;                           /* imul edx,edx,0xa; add edx,eax */
    }
    *(uint16_t *)(uintptr_t)esi = (uint16_t)edx;      /* mov [edi],dx (the value ptr) */
}
