/* lift_menu_hud_ui.c — verified-C lift of the ROTH `menu_hud_ui` subsystem.
 *
 * The UI-chrome layer: main/options/settings menus (blocking cursor-entry loops), the ornate
 * message-box/dialog frame compositor (show_message_box 0x2508f = 4567 B), HUD panels
 * (health/status/portrait/corner-peek), and the panel image+text compositing primitives used by
 * dialogue/inventory/weapon. Reads player state, drives no game logic. Composites ICONS.ALL RLE tiles
 * (das_assets), text (text_font), and framebuffer ops (blit_2d / video_display) — all reached through
 * bridges (call_orig); this subsystem has NO host bridges. See docs/reference/lift/menu_hud_ui.md.
 *
 * ABI derived from the disasm (tools/roth_disasm.py func <va>), NOT the corpus pseudocode.
 * Canon VAs throughout (runtime = canon + 0x400000).
 *
 * Verification tiers: Layer-A pure leaves (hit_test / scroll_entry index math) are
 * ORACLE-verified (return-value / obj3 write-set diff vs call_orig over staged geometry). The HUD
 * panels, message boxes, and menu loops drive DAS tile cache loads + the input ring + framebuffer
 * writes (non-idempotent / non-deterministic) — those are IN-GAME live-swap (ROTH_LIFT=menu_hud_ui),
 * registered here and validated in the dedicated debug session.
 */
#include <stdint.h>
#include <string.h>
#include "common.h"
#include "engine.h"

/* ============================================================================================
 * LAYER A — UI panel primitives (leaves)
 * ============================================================================================ */

/* hit_test_ui_element (canon 0x24b1e, 308 B) — the mouse-pick for menu/list rows + the frame's
 * button/scrollbar hotspots. Hit-tests a point against a vertical grid of UI cells plus a set of
 * fixed frame regions (close box, scroll up/down arrows, an alt title-bar box).
 *
 * ABI (Watcom __watcall, from disasm; note esi/edi are pushed BEFORE `enter`, so the 4 stack args
 * live at [ebp+0x10..0x1c] and the fn does `ret 0x10`):
 *   EAX = cells       — int32_t* array of cell records, stride 0x14 (5 dwords); [+0]=y-lo, [+4]=y-hi
 *   EDX = y           — cursor Y (raw; the row loop compares y-pitch, the frame boxes compare y)
 *   EBX = x           — cursor X (unsigned in the row loop, SIGNED in the frame-box tests)
 *   ECX = count       — number of cells to scan
 *   stack[0] = xbase  — first cell's X origin (steps +0xb per row)
 *   stack[1] = flags  — frame-region enable bits (0x80 alt-title box, 0x800 body box, 8 scroll arrows)
 *   stack[2] = ybox   — frame Y origin
 *   stack[3] = xbox   — frame X origin
 *   -> EAX = 1-based row index of the hit cell; else a negative frame-region code
 *      (-1 close/alt, -9 body, -6/-7 scroll arrows), else 0.
 * Global: DAT_00083e88 (canon 0x83e88) = the row pitch (subtracted from y for the row loop, added
 * back for the frame-box tests). Pure leaf (no callees, no segments). ORACLE-verified. */
int32_t hit_test_ui_element(int32_t *cells, int32_t y, uint32_t x, uint32_t count,
                                   uint32_t xbase, uint32_t flags, int32_t ybox, int32_t xbox)
{
    int32_t  pitch = (int32_t)G32(VA_g_active_weapon_ammo_cap + 0x14);
    int32_t  idx   = 1;
    uint32_t i     = 0;
    int32_t  iv    = y - pitch;

    for (;;) {
        if (i >= count) {
            iv += pitch;                       /* == y */
            int32_t xs = (int32_t)x;           /* frame-box tests compare X SIGNED */

            if ( (ybox + 4 < iv && iv < ybox + 0x20 && xbox + 4 < xs && xs < xbox + 0x20)
              || ((flags & 0x80) && ybox + 4 <= iv && iv <= ybox + 0x17
                                 && xbox + 0x23 <= xs && xs <= xbox + 0x36) ) {
                iv = -1;
            } else if ( !(flags & 0x800) || iv < ybox + 0x22 || ybox + 0x73 < iv
                        || xs < xbox + 0xf || xbox + 0x4a < xs ) {
                if (flags & 8) {
                    if (ybox + 0x116 <= iv && iv <= ybox + 0x11e
                        && xbox + 0x12 <= xs && xs <= xbox + 0x26)
                        return -6;
                    if (ybox + 0x116 <= iv && iv <= ybox + 0x11e
                        && xbox + 0x31 <= xs && xs <= xbox + 0x45)
                        return -7;
                }
                iv = 0;
            } else {
                iv = -9;
            }
            return iv;
        }
        if (xbase < x && x < xbase + 0xb && cells[0] < iv && iv < cells[1])
            break;                             /* hit -> return the 1-based row index */
        idx++;
        xbase += 0xb;
        i++;
        cells += 5;                            /* stride 0x14 bytes */
    }
    return idx;
}

/* scroll_entry_into_view (canon 0x1b0e3, 94 B) — clicked-entry scroll: given a POINTER to a
 * cursor-entry (EAX), linear-scan g_cursor_entry_table (0x7fef4, stride 0xc) bounded by
 * g_cursor_entry_count (0x80af4) to find its index, store it as the cursor position for the active
 * list (g_cursor_list_positions[ctx] @ 0x80afc), then clamp the scroll offset
 * (g_cursor_scroll_offsets[ctx] @ 0x80b10) so the 10-row window shows it: while index < offset,
 * offset -= 5; while index-9 > offset, offset += 5. ctx = g_cursor_active_list (0x80b38, dword index).
 * Not found (index reaches count) -> no writes. Pure obj3 (leaf; the corpus flow_succ into
 * get_item_tab_index is a shared-tail artifact, not called). ORACLE (obj3 write-set diff). */
void scroll_entry_into_view(uint32_t *param_1)
{
    int32_t *entry = (int32_t *)GADDR(VA_g_cursor_entry_table);         /* &g_cursor_entry_table */
    uint32_t count = (uint32_t)G32(VA_g_cursor_entry_count);
    uint32_t index = 0;
    for (;;) {
        if (index >= count)
            return;                                     /* not found -> no state change */
        if ((uintptr_t)entry == (uintptr_t)param_1)
            break;
        entry += 3;                                     /* +0xc bytes */
        index++;
    }
    int32_t ctx = G32(VA_g_cursor_active_list);                          /* g_cursor_active_list */
    volatile int32_t *pos = (volatile int32_t *)(GADDR(VA_g_cursor_list_positions) + (uintptr_t)ctx * 4);
    volatile int32_t *off = (volatile int32_t *)(GADDR(VA_g_cursor_scroll_offsets) + (uintptr_t)ctx * 4);
    *pos = (int32_t)index;
    while ((int32_t)index < *off)
        *off -= 5;
    int32_t iv = (int32_t)index - 9;
    if (iv > 0) {
        while (*off < iv)
            *off += 5;
    }
}

/* draw_ui_panel_image_at_xy (canon 0x2271d, 67 B) — resolve a UI image handle then blit it at (x,y).
 * ABI __watcall: EAX=dest base, EDX=image id, EBX=x, ECX=y, stack[0]=pitch; ret 4. dest = base +
 * x + y*pitch. resolve_reloc_ptr [L] deref (A4 raw ptr), blit_das_image_to_buffer [L] with mode 1.
 * In-game live-swap (framebuffer write via the DAS blitter). */
void draw_ui_panel_image_at_xy(uint32_t dest_base, uint32_t img_id, int32_t x, int32_t y,
                                      uint32_t pitch)
{
    uint32_t img = resolve_reloc_ptr(img_id);
    uint32_t dst = dest_base + (uint32_t)(x + y * (int32_t)pitch);
    blit_das_image_to_buffer(img, dst, pitch, 1);
}

/* draw_ui_panel_image_block (canon 0x227b1, 56 B) — blit an ALREADY-resolved image block (EDX is the
 * raw image ptr, no resolve_reloc_ptr). ABI: EAX=dest base, EDX=image ptr, EBX=x, ECX=y,
 * stack[0]=pitch; ret 4. dest = base + x + y*pitch; blit mode 1. In-game live-swap. */
void draw_ui_panel_image_block(uint32_t dest_base, uint32_t img, int32_t x, int32_t y,
                                      uint32_t pitch)
{
    uint32_t dst = dest_base + (uint32_t)(x + y * (int32_t)pitch);
    blit_das_image_to_buffer(img, dst, pitch, 1);
}

/* draw_ui_panel_count_element (canon 0x1a0ab, 95 B) — a fixed HUD badge: blit reloc DAS tile 200 at
 * (0x81058 + g_ui_panel_anchor_x - 2, g_ui_panel_anchor_y + 2); when enabled (0x8105e), stamp a
 * one-char count string ([0x8105c]=1 control, [0x8105d]=char from [0x76768]) via draw_text_at_screen_xy
 * at (0x81058+anchor_x, anchor_y+2). No register inputs (all computed from globals). blit_reloc_das_image
 * [L] + screen_xy_to_framebuffer_ptr [L] preserve ECX, so the text Y is anchor_y+2. In-game live-swap. */
void draw_ui_panel_count_element(void)
{
    int32_t x_anchor = (int32_t)G32(VA_g_selected_item_primary + 0x14) + (int32_t)G32(VA_g_ui_panel_anchor_x);   /* + g_ui_panel_anchor_x */
    int32_t y = (int32_t)G32(VA_g_ui_panel_anchor_y) + 2;                              /* g_ui_panel_anchor_y + 2 */
    int32_t pitch = (int32_t)G32(VA_g_screen_pitch);
    uint8_t *fb = screen_xy_to_framebuffer_ptr(x_anchor - 2, y);
    blit_reloc_das_image((uint32_t)(uintptr_t)fb, 0xc8, (uint32_t)pitch);
    if (G8(VA_g_selected_item_primary + 0x1a) != 0) {
        G8(VA_g_selected_item_primary + 0x19) = G8(VA_g_map_menu_marker_normal + 0x3);
        G8(VA_g_selected_item_primary + 0x18) = 1;
        draw_text_at_screen_xy((uint32_t)GADDR(VA_g_selected_item_primary + 0x18), (uint32_t)x_anchor, (uint32_t)y, 0);
    }
}

/* update_ui_overlay (canon 0x1a132, 70 B) — per-frame gate for the inventory bag panel. If a
 * dialogue/monologue owns the screen (g_active_dialogue_context 0x83115 != 0 AND g_move_freeze_gate
 * 0x83125 == 0x6ffff), skip. Else: when g_player_health (0x8a0f0) == 0, run close_inventory_panel [L]
 * + clear the render latch (0x7fec4=0); then, gated by that latch, render_inventory_panel [L] once and
 * set the latch to 2. Call-closed (both callees lifted). In-game live-swap. */
void update_ui_overlay(void)
{
    if (G32(VA_g_active_dialogue_context) != 0 && G32(VA_g_move_freeze_gate) == 0x6ffff)
        return;
    if (G32(VA_g_player_health) == 0) {
        close_inventory_panel();
        G32(VA_g_inventory_panel_open) = 0;
    }
    if (G32(VA_g_inventory_panel_open) != 0)
        return;
    render_inventory_panel();
    G32(VA_g_inventory_panel_open) = 2;
}

/* show_status_message_wrap (canon 0x17fe5, 8 B, DEAD) — movsx eax,al; jmp show_no_ammo_message.
 * Thin thunk: sign-extend the low byte (a message index) and forward to show_no_ammo_message [L]
 * (weapon_combat). Statically DEAD (no live caller); registered for gate coverage, review-only. */
void show_status_message_wrap(uint32_t eax)
{
    show_no_ammo_message((uint32_t)(int32_t)(int8_t)eax);
}

/* reset_hud_icon_state (canon 0x1bc91, 51 B, DEAD) — invalidate both HUD icon widgets: free the
 * portrait-corner save-under region (g_corner_icon_saveunder 0x7fdbc via pool_free_handle [L], pool =
 * g_das_cache_heap_handle 0x85c3c) and force the held-item icon to rebuild (dedup id 0x8130c = -1 +
 * update_selected_item_icon [L], reached via the shared 0x1a8c6 tail). Call-closed. DEAD; registered
 * for gate coverage, review-only. */
void reset_hud_icon_state(void)
{
    uint32_t *su = (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_corner_icon_saveunder);   /* g_corner_icon_saveunder */
    if (su != 0) {
        uint32_t *pool = (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle); /* g_das_cache_heap_handle */
        pool_free_handle(pool, su);
        G32(VA_g_corner_icon_saveunder) = 0;
    }
    G32(VA_g_pending_choice_accept_index + 0x8) = -1;
    update_selected_item_icon();
}

/* ============================================================================================
 * LAYER B — HUD panels (in-game live-swap: DAS tiles + framebuffer)
 * ============================================================================================ */

/* call_orig bridge for the two menu_hud_ui callees that are NOT lifted (dialogue_ui render_text_ui
 * 0x1f0e8, video_display flip_video_page 0x2e1e8). EAX in, result discarded. */
static void mh_bridge_eax(uint32_t canon_va, uint32_t eax)
{
    regs_t io;
    memset(&io, 0, sizeof io);
    io.va  = canon_va + OBJ_DELTA;
    io.eax = eax;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    if (canon_va == 0x2e1e8u) host_flip_video_page(eax);        /* flip_video_page: host present */
    else if (canon_va == 0x1f0e8u) render_text_ui((int32_t)eax);   /* text-UI dispatcher (lifted) */
    else roth_unreachable(canon_va);                           /* in-game-only targets — off bare title */
#endif
}

/* clear_corner_peek_icon (canon 0x167d7, 48 B) — discard the top-left corner peek-icon save-under:
 * clear the active flag (0x7fdc0) and free g_corner_icon_saveunder (0x7fdbc) via pool_free_handle [L]
 * on g_das_cache_heap_handle (0x85c3c). In-game live-swap. */
void clear_corner_peek_icon(void)
{
    G32(VA_g_corner_icon_saveunder + 0x4) = 0;
    uint32_t *su = (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_corner_icon_saveunder);
    if (su != 0) {
        uint32_t *pool = (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle);
        pool_free_handle(pool, su);
        G32(VA_g_corner_icon_saveunder) = 0;
    }
}

/* restore_corner_peek_icon (canon 0x16807, 42 B) — re-blit the saved corner peek-icon rows
 * (blit_descriptor_rows [L] with ECX=0x24) and mark the (0,0)-0x24 region dirty. blit_descriptor_rows
 * preserves all regs, so the dirty-rect extent is 0x24. In-game live-swap. */
void restore_corner_peek_icon(void)
{
    uint32_t su = (uint32_t)G32(VA_g_corner_icon_saveunder);
    if (su != 0) {
        blit_descriptor_rows(su);
        register_dirty_rect(0, 0, 0x24, 0x24);
    }
}

/* draw_character_portrait_corner (canon 0x16831, 215 B) — the top-left character-portrait widget.
 * EAX = animation offset (0..9 approach, >=10 settled). Save/restore the under-region via
 * g_corner_icon_saveunder (save_framebuffer_region [L] / blit_save_region [L]); draw the drop-shadow
 * border frame (draw_popup_shadow_border_smc [L]) + cleared interior (clear_framebuffer_rect [L]);
 * when settled, overlay the active-item sub-icon (draw_item_icon_centered [L] from
 * g_active_item_hud_icon 0x7fed0, with a slide-in offset); mark (0,0)-0x24 dirty. In-game live-swap.
 * (The corpus flow_succ->flush_dirty_rects is a shared-return-tail artifact, not a call.) */
void draw_character_portrait_corner(int32_t param_1)
{
    if (G32(VA_g_corner_icon_saveunder) == 0) {
        uint32_t su = save_framebuffer_region(0, 0, 0x24, 0x24, NULL);
        G32(VA_g_corner_icon_saveunder) = (int32_t)su;
        if (su == 0)
            return;
    } else {
        blit_save_region((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_corner_icon_saveunder));
    }
    if (param_1 < 10) {
        int32_t iv = 0x12 - (param_1 + 4);
        int32_t u1 = (param_1 + 4) * 2;
        draw_popup_shadow_border_smc(iv, iv, u1, u1);
        clear_framebuffer_rect((uint32_t)iv, (uint32_t)iv, (uint32_t)u1, (uint32_t)u1);
    } else {
        draw_popup_shadow_border_smc(4, 4, 0x1c, 0x1c);
        clear_framebuffer_rect(4, 4, 0x1c, 0x1c);
        if (G32(VA_g_active_item_hud_icon) != 0) {
            int32_t s = param_1 - 10;
            uint32_t slide = (s < 0xe) ? (uint32_t)((0xf - s) << 9) : 0;
            uint32_t *icon = (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_active_item_hud_icon);
            draw_item_icon_centered(icon[0], 4, 4, slide);
        }
    }
    register_dirty_rect(0, 0, 0x24, 0x24);
}

/* render_health_status_panel (canon 0x22c84, 503 B) — render a HUD status panel into the handle's
 * buffer. EAX = pool handle (int*; [0]=buffer), EDX = layout descriptor (dword-indexed):
 *   d[0]=pitch d[1]=height d[4]=panel-img-id d[5]=x d[6]=y d[7]=normal-bar-img d[8]=alt-bar-img
 *   d[9]=bar-x d[0xb]=bar-full-width d[0xc]=bar-y d[0xe]=bar-rows.
 * Publishes the panel rect to 0x83e54.., clears the buffer (mem_fill [L]), blits the panel image
 * (draw_ui_panel_image_at_xy [Layer A]), resolves the bar image (resolve_reloc_ptr [L]; swaps to the
 * low-health image 0x298 below half fill), computes the fill from g_player_health (0x8a0f0) clamped
 * to max (0x7fe44), blits the bar into a work buffer (blit_das_image_to_buffer [L]) and copies it
 * row-by-row (memcpy_return_dest [L]), then records the dirty rect (0x83e3c..). In-game live-swap. */
void render_health_status_panel(uint32_t p1, uint32_t p2)
{
    int32_t  *handle = (int32_t *)(uintptr_t)p1;
    uint32_t *d      = (uint32_t *)(uintptr_t)p2;

    G32(VA_g_weapon_hud_anim_accum + 0x4c) = (int32_t)d[0];
    G32(VA_g_weapon_hud_anim_accum + 0x50) = (int32_t)d[1];
    G32(VA_g_weapon_hud_anim_accum + 0x44) = G32(VA_g_weapon_hud_anim_accum + 0x60);
    G32(VA_g_weapon_hud_anim_accum + 0x48) = G32(VA_g_weapon_hud_anim_accum + 0x5c);
    G32(VA_g_weapon_hud_anim_accum + 0x58) = 0;
    G32(VA_g_weapon_hud_anim_accum + 0x54) = 0;

    mem_fill((void *)(uintptr_t)(uint32_t)handle[0], 0, d[0] * d[1]);

    if (d[4] != 0)
        draw_ui_panel_image_at_xy((uint32_t)handle[0], d[4], (int32_t)d[5], (int32_t)d[6], d[0]);

    uint32_t img = resolve_reloc_ptr(d[7]);
    if (d[8] != 0)
        (void)resolve_reloc_ptr(d[8]);          /* faithful: resolved, result unused */

    uint32_t health = (uint32_t)G32(VA_g_player_health);
    if ((int32_t)health < 0) health = 0;
    uint32_t maxh = (uint32_t)G32(VA_g_help_overlay_enabled + 0xc);
    if (health > maxh) health = maxh;

    uint32_t fill = 0;
    if (maxh != 0)
        fill = (d[0xb] * health) / maxh;
    if (fill == 0 && health != 0) fill = 1;

    if (fill < (d[0xb] >> 1))
        img = resolve_reloc_ptr(0x298);

    if (fill != 0) {
        uint32_t work[250];
        blit_das_image_to_buffer(img, (uint32_t)(uintptr_t)work, 0x64, 1);
        uint8_t *dst = (uint8_t *)(uintptr_t)((uint32_t)handle[0] + d[0xc] * d[0] + d[9]);
        uint8_t *src = (uint8_t *)work;
        for (uint32_t r = 0; r < d[0xe]; r++) {
            memcpy_return_dest(dst, src, fill);
            src += 0x64;
            dst += d[0];
        }
    }

    G32(VA_g_weapon_hud_anim_accum + 0x34) = 0;
    G32(VA_g_weapon_hud_anim_accum + 0x38) = 0;
    G32(VA_g_weapon_hud_anim_accum + 0x3c) = G32(VA_g_weapon_hud_anim_accum + 0x4c);
    G32(VA_g_weapon_hud_anim_accum + 0x40) = G32(VA_g_weapon_hud_anim_accum + 0x50);
}

/* render_player_health_bar (canon 0x235fe, 619 B) — render the status panel's health bar. EAX = the
 * buffer BASE address (not a handle ptr here), EDX = descriptor (same field layout as above). Splits
 * the bar into a filled span (normal/low image) and an empty span (alt image d[8]), each blitted into
 * a work buffer (blit_das_image_to_buffer [L]) and copied row-by-row (memcpy_return_dest [L]), then
 * merges the drawn extent into the shared dirty rect (0x83e3c..; init-or-expand). No max-health==0
 * guard (faithful to the original). In-game live-swap. */
void render_player_health_bar(uint32_t p1, uint32_t p2)
{
    int32_t  base = (int32_t)p1;
    int32_t *d    = (int32_t *)(uintptr_t)p2;

    G32(VA_g_active_weapon_ammo_cap + 0x8) = 0;
    uint32_t A = (uint32_t)d[9];      /* bar x   */
    uint32_t B = (uint32_t)d[0xc];    /* bar y   */
    uint32_t health = (uint32_t)G32(VA_g_player_health);
    if ((int32_t)health < 0) health = 0;
    uint32_t C = (uint32_t)d[0xb];    /* full width */
    uint32_t D = (uint32_t)d[0xe];    /* rows       */
    if (health > (uint32_t)G32(VA_g_help_overlay_enabled + 0xc)) health = (uint32_t)G32(VA_g_help_overlay_enabled + 0xc);

    uint32_t fill = ((uint32_t)d[0xb] * health) / (uint32_t)G32(VA_g_help_overlay_enabled + 0xc);
    if (fill == 0 && health != 0) fill = 1;

    uint32_t dst0 = (uint32_t)base + (uint32_t)d[0xc] * (uint32_t)d[0] + (uint32_t)d[9];

    if (fill != 0) {
        uint32_t img = (fill < ((uint32_t)d[0xb] >> 1))
                     ? resolve_reloc_ptr(0x298)
                     : resolve_reloc_ptr((uint32_t)d[7]);
        uint32_t work[250];
        blit_das_image_to_buffer(img, (uint32_t)(uintptr_t)work, 0x64, 1);
        uint8_t *src = (uint8_t *)work;
        uint8_t *dst = (uint8_t *)(uintptr_t)dst0;
        for (uint32_t i = 0; i < (uint32_t)d[0xe]; i++) {
            memcpy_return_dest(dst, src, fill);
            src += 0x64;
            dst += (uint32_t)d[0];
        }
    }

    uint32_t drawn = fill;
    uint32_t dst1  = dst0 + drawn;
    uint32_t empty = (uint32_t)d[0xb] - fill;
    if (empty != 0) {
        uint32_t img = resolve_reloc_ptr((uint32_t)d[8]);
        uint32_t work[250];
        blit_das_image_to_buffer(img, (uint32_t)(uintptr_t)work, 0x64, 1);
        uint8_t *src = (uint8_t *)work + drawn;
        uint8_t *dst = (uint8_t *)(uintptr_t)dst1;
        for (uint32_t i = 0; i < (uint32_t)d[0xe]; i++) {
            memcpy_return_dest(dst, src, empty);
            src += 0x64;
            dst += (uint32_t)d[0];
        }
    }

    if (G32(VA_g_weapon_hud_anim_accum + 0x3c) == 0) {
        G32(VA_g_weapon_hud_anim_accum + 0x34) = (int32_t)A;
        G32(VA_g_weapon_hud_anim_accum + 0x38) = (int32_t)B;
        G32(VA_g_weapon_hud_anim_accum + 0x3c) = (int32_t)(A + C);
        G32(VA_g_weapon_hud_anim_accum + 0x40) = (int32_t)(B + D);
    } else {
        if (A < (uint32_t)G32(VA_g_weapon_hud_anim_accum + 0x34))        G32(VA_g_weapon_hud_anim_accum + 0x34) = (int32_t)A;
        if ((uint32_t)G32(VA_g_weapon_hud_anim_accum + 0x3c) < A + C)    G32(VA_g_weapon_hud_anim_accum + 0x3c) = (int32_t)(A + C);
        if (B < (uint32_t)G32(VA_g_weapon_hud_anim_accum + 0x38))        G32(VA_g_weapon_hud_anim_accum + 0x38) = (int32_t)B;
        if ((uint32_t)G32(VA_g_weapon_hud_anim_accum + 0x40) < B + D)    G32(VA_g_weapon_hud_anim_accum + 0x40) = (int32_t)(B + D);
    }
}

/* refresh_hud_layout (canon 0x243be, 263 B) — per-frame HUD setup: free the old weapon DAS handle
 * (free_hud_weapon_das_handle [L]), compute the resolution-dependent panel layout offsets (0x83e64/
 * 0x83e68; branches on g_view_h==g_screen_height, g_screen_pitch==640, and the per-resolution table
 * at 0x71390/0x71394 indexed by g_screen_resolution_index 0x7f358), reserve the panel buffer
 * (ensure_das_cache_heap_space [L] + pool_alloc_handle [L], 0x4b0 bytes) into g_hud_weapon_das_handle
 * (0x83d6c), and render the health/status panel into it (render_health_status_panel). In-game. */
void refresh_hud_layout(void)
{
    free_hud_weapon_das_handle();

    int32_t x, y;
    if (G32(VA_g_screen_pitch + 0x4) == G32(VA_g_view_h)) {          /* g_view_h == g_screen_height */
        x = 1;
        y = 8;
    } else {
        y = 6;
        int32_t s14;
        int32_t s1c = 0xa;
        if (G32(VA_g_screen_pitch) == 0x280) {             /* g_screen_pitch == 640 */
            s14 = 0x18;
            y <<= 1;                              /* -> 12 */
        } else {
            s14 = 0xc;
        }
        int32_t res = G32(VA_g_screen_resolution_index);              /* g_screen_resolution_index */
        y   += *(volatile int32_t *)(GADDR(VA_g_choice_selected_index + 0x20) + (uintptr_t)res * 8);
        s1c += *(volatile int32_t *)(GADDR(VA_g_choice_selected_index + 0x24) + (uintptr_t)res * 8);
        x = (G32(VA_g_screen_pitch + 0x4) - s14) - s1c;
    }
    G32(VA_g_weapon_hud_anim_accum + 0x5c) = x;
    G32(VA_g_weapon_hud_anim_accum + 0x60) = y;
    G32(VA_g_weapon_hud_anim_accum + 0x4c) = 0;

    ensure_das_cache_heap_space(0x4b0);
    uint32_t *pool = (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle);
    uint32_t handle = pool_alloc_handle(pool, 0x4b0);
    G32(VA_g_hud_weapon_das_handle) = (int32_t)handle;
    if (handle != 0)
        render_health_status_panel(handle, (uint32_t)GADDR(VA_g_choice_selected_index + 0x4f4));
}

/* repaint_hud_and_present (canon 0x17317, 148 B) — full gameplay-HUD repaint + present (on return to
 * gameplay, e.g. closing the inventory): close the inventory panel if open (0x7fec4), reload the HUD
 * backdrop (load_backdrop_raw [L]) + view-region shadow border (redraw_view_region_shadow_border [L])
 * + tick the player (update_player_tick [L]); rebuild the HUD (refresh_hud_layout / render_weapon_hud
 * [L] with EDX=&g_active_weapon_attrs 0x811b4 preserved across refresh_hud_layout / draw_active_ui_panels
 * / render_text_ui bridge); then present (flush_dirty_rects [L] + flip_video_page bridge, or the slide-out
 * path 0x20b91 [L]); stamp g_last_frame_tick (0x85320) from g_frame_tick_counter (0x90bcc). In-game. */
void repaint_hud_and_present(void)
{
    G32(VA_g_prev_dirty_rect_count) = 0;
    if (G32(VA_g_inventory_panel_open) != 0) {
        close_inventory_panel();
        G32(VA_g_inventory_panel_open) = 0;
    }
    load_backdrop_raw();
    redraw_view_region_shadow_border();
    update_player_tick();
    refresh_hud_layout();
    render_weapon_hud(0, (uint32_t)GADDR(VA_g_active_weapon_attrs));   /* EDX = &g_active_weapon_attrs (preserved) */
    draw_active_ui_panels();
    mh_bridge_eax(0x1f0e8, 0);                               /* render_text_ui(0)  [bridge dialogue_ui] */
    if (G32(VA_g_pending_fire_aim + 0x14) == 0) {
        flush_dirty_rects();
        mh_bridge_eax(0x2e1e8, 3);                           /* flip_video_page(3) [bridge video_display] */
    } else {
        snapshot_screen_and_slide_out();
    }
    G8(VA_g_reloc_base + 0x4) = 0;
    G32(VA_g_pending_fire_aim + 0x14) = 0;
    mark_overlay_dirty_rects();
    G16(VA_g_last_frame_tick) = G16(VA_g_frame_tick_counter);                             /* g_last_frame_tick = g_frame_tick_counter */
}

/* ============================================================================================
 * LAYER D — panel renderers + menu loops (in-game live-swap)
 * ============================================================================================ */

/* sprintf(dst, fmt@canon, arg) — cdecl CRT bridge (0x27c53; caller cleans the stack). */
static void mh_sprintf_i(char *dst, uint32_t fmt_canon, int32_t arg)
{
    regs_t io;
    memset(&io, 0, sizeof io);
    io.va  = 0x27c53 + OBJ_DELTA;
    io.nstack   = 3;
    io.stack[0] = (uint32_t)(uintptr_t)dst;
    io.stack[1] = (uint32_t)GADDR(fmt_canon);
    io.stack[2] = (uint32_t)arg;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    roth_sprintf(dst, (const char *)GADDR(fmt_canon), (int)arg);   /* menu-entry "\x9e%D" etc. */
#endif
}

/* render_ui_texture_panel (canon 0x227e9, 1179 B) — the structured HUD/menu panel renderer. EAX = pool
 * handle (int*; [0]=buffer), EDX = panel descriptor. Publishes the panel geometry (0x83e1c.. + centering
 * on g_screen_pitch, doubled when g_view_h!=g_screen_height & g_ui_scale_flag 0x83d70), clears the buffer
 * (mem_fill [L]), blits the panel image (draw_ui_panel_image_at_xy [A]), resolves bar images
 * (resolve_reloc_ptr [L]), and renders the bar in one of 3 modes (d[0x14]): 0 = tile grid of ammo icons
 * (draw_ui_panel_image_block [A], g_active_weapon_item_id/_ammo_cap driven), 1 = single proportional fill,
 * 2 = split fill (blit_das_image_to_buffer [L] into a work buffer + row copy memcpy_return_dest [L]);
 * then the ammo icon + ammo count text (sprintf bridge + draw_text_to_buffer [L]). In-game live-swap. */
void render_ui_texture_panel(uint32_t p1, uint32_t p2)
{
    int32_t  *handle = (int32_t *)(uintptr_t)p1;
    uint32_t *d      = (uint32_t *)(uintptr_t)p2;

    G32(VA_g_active_weapon_ammo_cap + 0xc) = 0;
    G32(VA_g_weapon_hud_anim_accum + 0x1c) = (int32_t)d[0];
    G32(VA_g_weapon_hud_anim_accum + 0x20) = (int32_t)(d[1] + 3);

    if (G32(VA_g_hud_weapon_das_handle + 0x4) == 0 || G32(VA_g_screen_pitch + 0x4) == G32(VA_g_view_h)) {
        G32(VA_g_weapon_hud_anim_accum + 0x14) = (int32_t)(((uint32_t)G32(VA_g_screen_pitch) >> 1) - d[2]);
        G32(VA_g_weapon_hud_anim_accum + 0x18) = G32(VA_g_weapon_hud_anim_accum + 0x2c) + (int32_t)d[3];
        G32(VA_g_weapon_hud_anim_accum + 0x28) = (int32_t)d[3];
        G32(VA_g_weapon_hud_anim_accum + 0x24) = G32(VA_g_weapon_hud_anim_accum + 0x14) - G32(VA_g_weapon_hud_anim_accum + 0x30);
    } else {
        G32(VA_g_weapon_hud_anim_accum + 0x14) = (int32_t)(((uint32_t)G32(VA_g_screen_pitch) >> 1) - d[2] * 2);
        G32(VA_g_weapon_hud_anim_accum + 0x28) = (int32_t)(d[3] * 2);
        G32(VA_g_weapon_hud_anim_accum + 0x18) = G32(VA_g_weapon_hud_anim_accum + 0x2c) + G32(VA_g_weapon_hud_anim_accum + 0x28);
        G32(VA_g_weapon_hud_anim_accum + 0x24) = G32(VA_g_weapon_hud_anim_accum + 0x14) - G32(VA_g_weapon_hud_anim_accum + 0x30);
    }

    mem_fill((void *)(uintptr_t)(uint32_t)handle[0], 0, (d[1] + 3) * d[0]);

    if (d[4] != 0)
        draw_ui_panel_image_at_xy((uint32_t)handle[0], d[4], (int32_t)d[5], (int32_t)d[6], d[0]);

    uint32_t img = resolve_reloc_ptr(d[7]);
    uint32_t alt = (d[8] != 0) ? resolve_reloc_ptr(d[8]) : 0;
    int32_t  x0   = (int32_t)d[9];
    int32_t  y0   = (int32_t)d[0xc];
    int32_t  item = G32(VA_g_active_weapon_item_id);            /* g_active_weapon_item_id */
    uint32_t mode = d[0x14];

    if (mode == 0) {
        if ((uint32_t)G32(VA_g_active_weapon_ammo_cap) > 2)      /* g_active_weapon_ammo_cap > 2 */
            item = item >> 8;                /* arithmetic (sar) */
        G32(VA_g_hud_panel_das_handle1 + 0x4) = item;
        for (uint32_t r = 0; r < d[0xd]; r++) {
            int32_t x = x0;
            for (uint32_t c = 0; c < d[0xa]; c++) {
                item--;
                if (item < 0) {
                    if (alt == 0) break;
                    img = alt;
                }
                draw_ui_panel_image_block((uint32_t)handle[0], img, x, y0, d[0]);
                x += (int32_t)d[0xb];
            }
            y0 += (int32_t)d[0xe];
        }
    } else if (mode == 1) {
        if (item > 0x100) item = 0x100;
        uint32_t w = ((uint32_t)d[0xb] * (uint32_t)item) >> 8;
        if (w != 0) {
            uint32_t work[250];
            blit_das_image_to_buffer(img, (uint32_t)(uintptr_t)work, 0x64, 1);
            uint8_t *dst = (uint8_t *)(uintptr_t)((uint32_t)handle[0] + d[0xc] * d[0] + d[9]);
            uint8_t *src = (uint8_t *)work;
            for (uint32_t i = 0; i < d[0xe]; i++) {
                memcpy_return_dest(dst, src, w);
                src += 0x64;
                dst += d[0];
            }
        }
    } else if (mode == 2) {
        if (item > 0x100) item = 0x100;
        uint32_t w   = ((uint32_t)d[0xb] * (uint32_t)item) >> 8;
        uint32_t dstbase = (uint32_t)handle[0] + d[0xc] * d[0] + d[9];
        uint32_t rem = d[0xb] - w;
        if (w != 0) {
            uint32_t work[250];
            blit_das_image_to_buffer(img, (uint32_t)(uintptr_t)work, 0x64, 1);
            uint8_t *dst = (uint8_t *)(uintptr_t)(dstbase + rem);
            uint8_t *src = (uint8_t *)work + rem;
            for (uint32_t i = 0; i < d[0xe]; i++) {
                memcpy_return_dest(dst, src, w);
                src += 0x64;
                dst += d[0];
            }
        }
    }

    if ((uint32_t)G32(VA_g_active_weapon_ammo) != 0) {       /* g_active_weapon_ammo */
        if (d[0xf] != 0)
            draw_ui_panel_image_at_xy((uint32_t)handle[0], d[0xf], (int32_t)d[0x10], (int32_t)d[0x11], d[0]);
        if (d[0x12] != 0 && (uint32_t)G32(VA_g_active_weapon_ammo) > 1) {
            uint32_t tdst = (uint32_t)handle[0] + d[0x13] * d[0] + d[0x12];
            char sb[16];
            mh_sprintf_i(&sb[2], 0x75ef0, G32(VA_g_active_weapon_ammo));
            sb[0] = 1;
            sb[1] = (char)G8(VA_g_default_message_color);       /* g_default_message_color */
            draw_text_to_buffer((uint32_t)(uintptr_t)sb, tdst, d[0], 0);
        }
    }

    G32(VA_g_weapon_hud_anim_accum + 0x4) = 0;
    G32(VA_g_weapon_hud_anim_accum + 0x8) = 0;
    G32(VA_g_weapon_hud_anim_accum + 0xc) = G32(VA_g_weapon_hud_anim_accum + 0x1c);
    G32(VA_g_weapon_hud_anim_accum + 0x10) = G32(VA_g_weapon_hud_anim_accum + 0x20);
}

/* render_ui_panel_text: shared rows-clamp for the 3 glyph paths (rows = p3[3]-p3[1], clamped so the
 * bottom stays within g_view_h 0x8549c). */
static int32_t mh_glyph_rows(const int32_t *p3)
{
    int32_t rows = p3[3] - p3[1];
    if ((uint32_t)((uint32_t)rows + (uint32_t)p3[1] + (uint32_t)p3[5]) > (uint32_t)G32(VA_g_screen_pitch + 0x4))
        rows = (int32_t)G32(VA_g_screen_pitch + 0x4) - (p3[1] + p3[5]);
    return rows;
}

/* render_ui_panel_text: build the 0x28-byte draw_text_glyph_with_shadow descriptor and invoke it [L]. */
static void mh_glyph_call(uint32_t src, uint32_t dst, uint32_t bg, int32_t width, int32_t rows,
                          int32_t srcstr, int32_t deststr, int32_t bgstr, int32_t mode)
{
    int32_t desc[10];
    desc[0] = (int32_t)src;      /* +0x00 src   */
    desc[1] = (int32_t)dst;      /* +0x04 dst   */
    desc[2] = (int32_t)bg;       /* +0x08 bg    */
    desc[3] = width;             /* +0x0c width */
    desc[4] = rows;              /* +0x10 rows  */
    desc[5] = srcstr;            /* +0x14 srcstr  */
    desc[6] = deststr;           /* +0x18 deststr */
    desc[7] = bgstr;             /* +0x1c bgstr   */
    desc[8] = mode;              /* +0x20 mode    */
    draw_text_glyph_with_shadow((uint32_t)(uintptr_t)desc);
}

/* render_ui_panel_text (canon 0x23897, 1128 B) — render the text layer of a UI panel. EAX = text pixel
 * data (param_1), EDX = param_2 (metrics: [0]=dst base, [4]=bg base@+0xc, [1]=bgstr), EBX = param_3
 * (layout: [0]/[1]=x/y-min, [2]/[3]=x/y-max, [4]/[5]=fb x/y, [6]=width/srcstr, [7]=rows, [8]/[9]=inset).
 * When g_view_h==g_screen_height it copies the strip straight to the framebuffer (copy_nonzero_bytes [L],
 * line-doubled in hires); otherwise it composites via draw_text_glyph_with_shadow [L] in one of 3 modes
 * (g_ui_scale_flag 0x83d70 doubled=3, else hires=1 / lores=0) with resolution-dependent offsets, then
 * marks the region dirty (add_dirty_rect [L] doubled / register_dirty_rect [L]) and clears p3[3]. In-game. */
void render_ui_panel_text(uint32_t pa, uint32_t pb, uint32_t pc)
{
    int32_t  src = (int32_t)pa;
    int32_t *p2  = (int32_t *)(uintptr_t)pb;
    int32_t *p3  = (int32_t *)(uintptr_t)pc;

    if (G32(VA_g_screen_pitch + 0x4) == G32(VA_g_view_h)) {
        if (p3[6] == 0) return;
        uint32_t pitch = (uint32_t)G32(VA_g_screen_pitch);
        uint8_t *fb = (uint8_t *)(uintptr_t)((uint32_t)G32(VA_g_framebuffer_ptr) + (uint32_t)p3[4] + pitch);
        uint32_t width = (uint32_t)p3[6];
        uint8_t *sp = (uint8_t *)(uintptr_t)(uint32_t)src;
        if (G8(VA_g_hires_line_doubling_flag) != 0) {
            for (int32_t i = 0; i < p3[7]; i++) {
                copy_nonzero_bytes(fb, width, sp);
                fb += pitch;
                copy_nonzero_bytes(fb, width, sp);
                sp += width;
                fb += pitch;
            }
        } else {
            for (int32_t i = 0; i < p3[7]; i++) {
                copy_nonzero_bytes(fb, width, sp);
                sp += width;
                fb += pitch;
            }
        }
        return;
    }

    if (p3[3] == 0) return;
    if (p3[6] == 0) return;
    int32_t width = p3[2] - p3[0];
    if (width <= 0) return;

    int32_t l34 = p2[0];                 /* dst base */
    int32_t l38 = (int32_t)(pb + 0xc);   /* bg base = param_2 + 0xc */
    int32_t l28 = p2[1];                 /* bgstr */
    src += p3[1] * p3[6] + p3[0];
    int32_t pitch = (int32_t)G32(VA_g_screen_pitch);

    if (G32(VA_g_hud_weapon_das_handle + 0x4) != 0) {
        int32_t l30 = p3[0] * 2 + p3[8];
        l34 += l30;
        l38 += l30;
        int32_t l40 = p3[1] * 2 + p3[9];
        if (l40 != 0) {
            l38 += l40 * l28;
            l34 += l40 * pitch;
        }
        mh_glyph_call((uint32_t)src, (uint32_t)l34, (uint32_t)l38, width, mh_glyph_rows(p3),
                      p3[6], pitch, l28, 3);
    } else {
        int32_t l70 = p3[1] + p3[9];
        int32_t l30 = p3[8] + p3[0];
        l34 += l30;
        l38 += l30;
        if (G8(VA_g_hires_line_doubling_flag) != 0) {
            if (l70 != 0) {
                l70 *= 2;
                l38 += l70 * l28;
                l34 += l70 * pitch;
            }
            mh_glyph_call((uint32_t)src, (uint32_t)l34, (uint32_t)l38, width, mh_glyph_rows(p3),
                          p3[6], pitch, l28, 1);
        } else {
            if (l70 != 0) {
                l38 += l70 * l28;
                l34 += l70 * pitch;
            }
            mh_glyph_call((uint32_t)src, (uint32_t)l34, (uint32_t)l38, width, mh_glyph_rows(p3),
                          p3[6], pitch, l28, 0);
        }
    }

    int32_t x = p3[4];
    int32_t y = p3[5];
    if (G32(VA_g_hud_weapon_das_handle + 0x4) != 0)
        add_dirty_rect((uint32_t)(p3[0] * 2 + x), p3[1] * 2 + y,
                              (uint32_t)(p3[2] * 2 + x), (uint32_t)(p3[3] * 2 + y));
    else
        register_dirty_rect((uint32_t)(x + p3[0]), y + p3[1],
                                   (uint32_t)(x + p3[2]), (uint32_t)(y + p3[3]));
    p3[3] = 0;
}

/* draw_active_ui_panels (canon 0x240d7, 142 B) — per-frame composite of the two active UI panels: tick
 * the weapon HUD ammo anim [L]; for the main panel handle (0x83d78, dirty flag 0x83e80) render its
 * texture (render_ui_texture_panel) + text (render_ui_panel_text with &0x83e0c); for the weapon/status
 * panel handle (0x83d6c, flag 0x83e7c) render the health bar (render_player_health_bar) + blit it to the
 * framebuffer (blit_panel_image [L], &0x83e3c, g_framebuffer_ptr). In-game live-swap. */
void draw_active_ui_panels(void)
{
    tick_weapon_hud_ammo_anim();
    if (G32(VA_g_hud_panel_das_handle1) != 0) {
        if (G32(VA_g_active_weapon_ammo_cap + 0xc) != 0)
            render_ui_texture_panel((uint32_t)G32(VA_g_hud_panel_das_handle1), (uint32_t)G32(VA_g_active_weapon_ammo_cap + 0x4));
        uint32_t h1 = (uint32_t)G32(VA_g_hud_panel_das_handle1);
        uint32_t h0 = (uint32_t)G32(VA_g_hud_panel_das_handle0);
        /* [0x83d74] is legitimately 0 in fullscreen-view mode: render_weapon_hud's
         * [0x8549c]==[0x85cdc] branch never saves a panel region, and render_ui_panel_text
         * never touches param_2 in that same mode. The original's `mov eax,[0x83d74];
         * mov edx,[eax]` then reads LINEAR 0 — real DOS maps the low 1 MB, and the host
         * services the game-code fault from its zero g_lowmem shadow (traps.c). A host-C
         * deref of 0 is a REAL SIGSEGV (gotcha H, DOS null-page tolerance) — reproduce the
         * benign zero read instead. (W31 finding: crashed loading a fullscreen-view save.) */
        uint32_t v0 = (h0 < 0x10000u) ? 0u : *(volatile uint32_t *)(uintptr_t)h0;
        render_ui_panel_text(*(volatile uint32_t *)(uintptr_t)h1, v0,
                                    (uint32_t)GADDR(VA_g_weapon_hud_anim_accum + 0x4));
    }
    if (G32(VA_g_hud_weapon_das_handle) != 0) {
        uint32_t h = (uint32_t)G32(VA_g_hud_weapon_das_handle);
        if (G32(VA_g_active_weapon_ammo_cap + 0x8) != 0)
            render_player_health_bar(*(uint32_t *)(uintptr_t)h, (uint32_t)GADDR(VA_g_choice_selected_index + 0x4f4));
        blit_panel_image(*(uint32_t *)(uintptr_t)h, (int32_t *)GADDR(VA_g_weapon_hud_anim_accum + 0x34),
                                (uint32_t)G32(VA_g_framebuffer_ptr));
    }
}

/* draw_scroll_indicators (canon 0x24ebe, 160 B) — the up/down scroll-arrow tiles for a scroll-list
 * message box (the save/load 9-slot list). EAX = box state (word [+0xc]=offset, [+0xe]=range),
 * EDX = box top Y, EBX = box left X. When range != 0: up arrow tile 0xa8(active,offset>0)/0xb0 and
 * down arrow tile 0xb8(active,offset<range)/0xc0, each via screen_xy_to_framebuffer_ptr [L] +
 * blit_reloc_das_image [L]; then register_dirty_rect [L] for the arrow column. In-game live-swap. */
void draw_scroll_indicators(uint32_t p1, int32_t p2, int32_t p3)
{
    uint16_t *box = (uint16_t *)(uintptr_t)p1;
    uint32_t  pitch = (uint32_t)G32(VA_g_screen_pitch);
    if (box[7] != 0) {                                     /* word[+0xe] = range */
        uint8_t *fb = screen_xy_to_framebuffer_ptr(p2 + 0x116, p3 + 0x12);
        uint32_t up = (box[6] != 0) ? 0xa8 : 0xb0;         /* word[+0xc] = offset */
        blit_reloc_das_image((uint32_t)(uintptr_t)fb, up, pitch);
        fb = screen_xy_to_framebuffer_ptr(p2 + 0x116, p3 + 0x31);
        uint32_t dn = (box[6] != box[7]) ? 0xb8 : 0xc0;
        blit_reloc_das_image((uint32_t)(uintptr_t)fb, dn, pitch);
    }
    register_dirty_rect((uint32_t)(p2 + 0x116), p3 + 0x12, (uint32_t)(p2 + 0x120), p3 + 0x45);
}

/* draw_menu_value_bar (canon 0x247bc, 355 B) — a horizontal settings VALUE GAUGE for one menu item.
 * EAX = x, EDX = y, EBX(BL) = marker byte, ECX = value. Fill length = (value * 0x23) >> 8, clamped to
 * the 0x23-px track; composes a LIT strip (reloc tile 0x350 selected / 0x340 else) for the fill and an
 * UNLIT strip (0x358 / 0x348) for the remainder (resolve_reloc_ptr [L] + blit_das_image_to_buffer [L]
 * into 36-px-pitch work buffers), then copies the 35-px x 9-row bar to the framebuffer (line-doubled in
 * hires). marker == g_map_menu_marker_selected (0x7675d) picks the highlighted tiles. In-game live-swap. */
void draw_menu_value_bar(int32_t x, int32_t y, uint32_t marker, int32_t value)
{
    uint8_t *fb = screen_xy_to_framebuffer_ptr(x, y);
    uint32_t fill = (uint32_t)(value * 0x23) >> 8;
    uint8_t  buf1[324], buf2[324];
    uint32_t lit_img = 0, unlit_img = 0;
    if (value != 0 && fill == 0) fill = 1;
    if (fill > 0x23) fill = 0x23;
    uint32_t unlit_len = 0x23 - fill;

    if ((uint8_t)marker == G8(VA_g_map_menu_marker_selected)) {                  /* g_map_menu_marker_selected */
        if (fill != 0) lit_img = resolve_reloc_ptr(0x350);
        if (unlit_len != 0) unlit_img = resolve_reloc_ptr(0x358);
    } else {
        if (fill != 0) lit_img = resolve_reloc_ptr(0x340);
        if (unlit_len != 0) unlit_img = resolve_reloc_ptr(0x348);
    }

    if (unlit_img != 0)
        blit_das_image_to_buffer(unlit_img, (uint32_t)(uintptr_t)buf2, 0x24, 1);
    uint8_t *b2 = buf2;
    if (lit_img != 0) {
        blit_das_image_to_buffer(lit_img, (uint32_t)(uintptr_t)buf1, 0x24, 1);
        b2 += fill;
    }

    uint8_t  *b1 = buf1;
    uint8_t  *fp = fb;
    uint32_t  pitch = (uint32_t)G32(VA_g_screen_pitch);
    uint32_t  row = 0;
    do {
        if (G8(VA_g_hires_line_doubling_flag) != 0) {                            /* hires: draw the doubled scanline */
            if (fill)      memcpy(fp, b1, fill);
            if (unlit_len) memcpy(fp + fill, b2, unlit_len);
            fp += pitch;
        }
        if (fill)      { memcpy(fp, b1, fill); b1 += 0x24; }
        if (unlit_len) { memcpy(fp + fill, b2, unlit_len); b2 += 0x24; }
        fp += pitch;
        row++;
    } while (row < 9);
}

/* sprintf(dst, fmt@canon, d, s) — cdecl CRT bridge for the "%d: %s" numbered-item label. */
static void mh_sprintf_ds(char *dst, uint32_t fmt_canon, int32_t d, const char *s)
{
    regs_t io;
    memset(&io, 0, sizeof io);
    io.va  = 0x27c53 + OBJ_DELTA;
    io.nstack   = 4;
    io.stack[0] = (uint32_t)(uintptr_t)dst;
    io.stack[1] = (uint32_t)GADDR(fmt_canon);
    io.stack[2] = (uint32_t)d;
    io.stack[3] = (uint32_t)(uintptr_t)s;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    roth_sprintf(dst, (const char *)GADDR(fmt_canon), (int)d, s);   /* menu-entry "%D: %s" */
#endif
}

/* render_menu_entry_list (canon 0x2491f, 511 B) — render the list of menu/settings/save-load entries.
 * EAX = per-entry output record array (rec, stride 0x14; [0]/[1]=drawn x-left/x-right, [4]=label str),
 * EDX = starting Y, EBX = entry count, ECX = list header (word[+0xc]=scroll base; entries at +0x14,
 * stride 0xc: [0]=type, [1]=data ptr, [2]=value, byte[+0xb]=flags). Per entry, by type: 0 = a
 * measured+centered label (measure_control_text_width [L] + draw_text_at_screen_xy [L]); 0x31/0x32 =
 * a numbered "%d: %s" item (dbase100 label via resolve_dbase100_text [L] unless inline, sprintf bridge);
 * else = a settings row with an optional value gauge (draw_menu_value_bar [D] when flag 0x10) + its
 * label. Accumulates the min/max drawn X and marks the list dirty (register_dirty_rect [L]). In-game. */
void render_menu_entry_list(uint32_t p1, int32_t p2, uint32_t count, uint32_t p4)
{
    uint32_t *rec   = (uint32_t *)(uintptr_t)p1;
    uint32_t *entry = (uint32_t *)(uintptr_t)(p4 + 0x14);
    int32_t   y  = p2;
    int32_t   y0 = p2;
    uint32_t  minX = 8000, maxX = 0, idx = 0;

    for (;;) {
        if (idx >= count) {
            if (minX < maxX)
                register_dirty_rect(minX + (uint32_t)G32(VA_g_active_weapon_ammo_cap + 0x14), y0,
                                           maxX + (uint32_t)G32(VA_g_active_weapon_ammo_cap + 0x14), y);
            return;
        }
        uint32_t type = entry[0];
        if (type == 0) {
            if (entry[1] != 0) {
                uint16_t base  = *(uint16_t *)(uintptr_t)(p4 + 0xc);
                uint32_t label = entry[1] + ((uint32_t)base + idx) * 0x32;
                int32_t  w = measure_control_text_width((const char *)(uintptr_t)label);
                draw_text_at_screen_xy(rec[4], (uint32_t)G32(VA_g_active_weapon_ammo_cap + 0x14), (uint32_t)y, 4);
                int32_t x = (int32_t)G32(VA_g_ui_panel_anchor_x) + (int32_t)G32(VA_g_active_weapon_ammo_cap + 0x14) + 0x99 - (w >> 1);
                draw_text_at_screen_xy(label, (uint32_t)x, (uint32_t)y, 4);
                rec[0] = (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_x) + 0x54);
                rec[1] = (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_x) + 0xd4);
            }
        } else if (type < 0x31 || type > 0x32) {
            uint32_t l24 = rec[4];
            if (*(uint8_t *)(uintptr_t)((uint32_t)(uintptr_t)entry + 0xb) & 0x10) {
                uint8_t marker = *(uint8_t *)(uintptr_t)(l24 + 1);
                int32_t value  = (int32_t)(entry[2] & 0x1ff);
                int32_t x = (int32_t)G32(VA_g_active_weapon_ammo_cap + 0x14) + (int32_t)rec[0];
                draw_menu_value_bar(x, y, marker, value);
            }
            draw_text_at_screen_xy(l24, (uint32_t)G32(VA_g_active_weapon_ammo_cap + 0x14), (uint32_t)y, 4);
        } else {                                          /* numbered item (0x31/0x32) */
            uint32_t l3c = idx + *(uint16_t *)(uintptr_t)(p4 + 0xc);
            uint32_t l20 = rec[4];
            char *pc = (char *)(uintptr_t)(entry[1] + l3c * 0x30);
            if (*pc == 0) {
                if (pc[1] == '~') {
                    pc = (char *)(uintptr_t)(l20 + 5);
                } else {
                    char dcbuf[0x50];
                    resolve_dbase100_text((uint32_t)(uintptr_t)dcbuf, 0x50, 0x33, 0);
                    pc = dcbuf;
                }
            }
            char sb[80];
            sb[0] = 1;
            sb[1] = *(char *)(uintptr_t)(l20 + 1);
            mh_sprintf_ds(&sb[2], 0x75ef9, (int32_t)(l3c + 1), pc);
            int32_t x = (int32_t)G32(VA_g_ui_panel_anchor_x) + 0x54 + (int32_t)G32(VA_g_active_weapon_ammo_cap + 0x14);
            draw_text_at_screen_xy((uint32_t)(uintptr_t)sb, (uint32_t)x, (uint32_t)y, 4);
            rec[0] = (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_x) + 0x54);
            rec[1] = (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_x) + 0xd4);
        }
        if (maxX < rec[1]) maxX = rec[1];
        if (rec[0] < minX) minX = rec[0];
        y += 0xb;
        idx++;
        rec   += 5;
        entry += 3;
    }
}

/* ============================================================================================
 * LAYER C — message boxes + fullscreen image (in-game live-swap)
 * ============================================================================================ */

/* draw_menu_box_zoom_anim (canon 0x24fb1, 206 B) — the message-box open/close ZOOM animation. EAX/EDX =
 * box x/y, EBX/ECX = box w/h, stack[0] = save-under descriptor. Draws progressively-scaled box frames
 * about the box center (scale 4/32 .. by g_frame_time_scale 0x85324 per step), each = shadow border
 * (draw_popup_shadow_border_smc [L]) + cleared interior (clear_framebuffer_rect [L]) + present
 * (register_dirty_rect [L] / flush_dirty_rects [L] / flip_video_page bridge) + restore the under-region
 * (blit_descriptor_rows [L]) + a frame-time tick (update_frame_time_scale [L]); then the full-size frame.
 * ret 4. In-game live-swap. */
void draw_menu_box_zoom_anim(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t desc)
{
    int32_t centerx = x + (int32_t)(w >> 1);
    int32_t centery = y + (int32_t)(h >> 1);
    int32_t scale = 4;
    do {
        uint32_t fw = (w * (uint32_t)scale) >> 5;
        int32_t  fx = centerx - (int32_t)fw;
        fw *= 2;
        uint32_t fh = (h * (uint32_t)scale) >> 5;
        int32_t  fy = centery - (int32_t)fh;
        fh *= 2;
        draw_popup_shadow_border_smc(fx, fy, (int32_t)fw, (int32_t)fh);
        clear_framebuffer_rect((uint32_t)fx, (uint32_t)fy, fw, fh);
        register_dirty_rect((uint32_t)(fx - 4), fy - 4,
                                   (uint32_t)((int32_t)fw + fx + 4), (int32_t)fh + fy + 4);
        flush_dirty_rects();
        mh_bridge_eax(0x2e1e8, 3);                      /* flip_video_page(3) */
        blit_descriptor_rows(desc);
        update_frame_time_scale();
        scale += (int32_t)G32(VA_g_frame_time_scale);                 /* += g_frame_time_scale */
    } while (scale < 0x10);
    draw_popup_shadow_border_smc(x, y, (int32_t)w, (int32_t)h);
}

/* sprintf(dst, fmt@canon, a, b) — cdecl CRT bridge for the "%s%s" label+input display string. */
static void mh_sprintf_ss(char *dst, uint32_t fmt_canon, const char *a, const char *b)
{
    regs_t io;
    memset(&io, 0, sizeof io);
    io.va  = 0x27c53 + OBJ_DELTA;
    io.nstack   = 4;
    io.stack[0] = (uint32_t)(uintptr_t)dst;
    io.stack[1] = (uint32_t)GADDR(fmt_canon);
    io.stack[2] = (uint32_t)(uintptr_t)a;
    io.stack[3] = (uint32_t)(uintptr_t)b;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    roth_sprintf(dst, (const char *)GADDR(fmt_canon), a, b);   /* menu-entry "%s%s" */
#endif
}

/* render_text_input_field (canon 0x244da, 738 B, INTERACTIVE) — the save-name text-entry widget, driven
 * by show_message_box. EAX = label str, EDX = current input str, EBX = output buf (0x30), ECX = field x,
 * stack[0] = field y; ret 4 -> EAX = 0 (backspace-out) / 1 (enter) / 2 (cancel via cursor action). Copies
 * the input into an edit buffer, then a blocking key loop: on each redraw it draws the label+input string
 * (draw_text_at_screen_xy [L] over an sprintf'd "%s%s") with a filled cursor rect (fill_rect_solid [L],
 * g_rect_fill_color 0x7f355), presents (register/flush_dirty_rects [L] + flip_video_page bridge), then
 * reads a translated key (dequeue_translated_key [L]): printable chars append (measure_control_text_width
 * [L], width-capped 0x78), key<=4 backspaces, 0xa confirms, 8 exits, and the cursor action flags
 * (0x7e938/0x7e939) cancel. On confirm/cancel the edit buffer is copied to the output. In-game live-swap
 * (interactive; the input ring is pumped via lift_is_interactive). */
uint32_t render_text_input_field(uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4, int32_t p5)
{
    const char *label = (const char *)(uintptr_t)p1;
    const char *input = (const char *)(uintptr_t)p2;
    uint8_t    *out   = (uint8_t *)(uintptr_t)p3;
    uint32_t    x = p4;
    int32_t     y = p5;

    int32_t  w1 = measure_control_text_width(label);
    uint32_t len = 0;
    uint32_t prevfill = 0;
    int32_t  redraw = 1;
    int32_t  w2 = measure_control_text_width(input);
    uint32_t fill = (uint32_t)(w2 + 1);
    char     edit_buf[0x31];
    int32_t  cursorx = (int32_t)x + w1;
    uint32_t prevcursorx = (uint32_t)cursorx;
    memcpy(edit_buf, input, 0x2f);
    edit_buf[0x2f] = 0;
    char     disp[0x64];
    disp[0] = 1;
    disp[2] = 0;
    clear_framebuffer_rect(x, (uint32_t)y, 0x8c, 0xb);
    int32_t  firstkey = 1;
    uint32_t initcursorx = (uint32_t)cursorx;
    int32_t  y_plus_c = y + 0xc;
    uint32_t rc = x + 0x90;
    int32_t  result = 0;

    for (;;) {
        if (redraw != 0) {
            disp[1] = 0;
            draw_text_at_screen_xy((uint32_t)(uintptr_t)disp, x, (uint32_t)y, 4);
            redraw = 0;
            mh_sprintf_ss(&disp[2], 0x75ef4, label, edit_buf);
            if (prevfill != 0) {
                G8(VA_g_rect_fill_color) = 0;
                fill_rect_solid(prevcursorx, (uint32_t)y, prevfill, 0xb);
            }
            G8(VA_g_rect_fill_color) = G8(VA_g_default_message_color + 0x1);
            disp[1] = (char)G8(VA_g_default_message_color);
            fill_rect_solid((uint32_t)cursorx, (uint32_t)y, fill, 0xb);
            draw_text_at_screen_xy((uint32_t)(uintptr_t)disp, x, (uint32_t)y, 4);
            prevfill = fill;
            prevcursorx = (uint32_t)cursorx;
            register_dirty_rect(x, y, rc, y_plus_c);
            flush_dirty_rects();
            mh_bridge_eax(0x2e1e8, 3);                  /* flip_video_page(3) */
        }
        uint32_t keyv = 0;
        dequeue_translated_key(&keyv);
        uint8_t key = (uint8_t)keyv;
        if (G8(VA_g_cursor_primary_action_flag) != 0 || G8(VA_g_cursor_secondary_action_flag) != 0) {     /* cursor action -> cancel */
            G8(VA_g_cursor_secondary_action_flag) = 0;
            G8(VA_g_cursor_primary_action_flag) = 0;
            result = 2;
            goto copy_and_exit;
        }
        if (key == 0)
            continue;
        if (key == 0xa) { result = 1; goto copy_and_exit; }
        if (key == 8)   { result = 0; goto exit_field; }
        if (key == 5 && firstkey) {
            fill = 1;
            len = string_length_244c8((const uint8_t *)edit_buf);
            redraw = 1;
            int32_t w = measure_control_text_width(edit_buf);
            cursorx = (int32_t)initcursorx + w;
            firstkey = 0;
        }
        if ((key & 0xff) > 0x1f) {                      /* printable */
            if (firstkey) { edit_buf[0] = 0; fill = 1; firstkey = 0; }
            if (len < 0x2f) {
                edit_buf[len + 1] = 0;
                edit_buf[len]     = (char)key;
                int32_t w = measure_control_text_width(edit_buf);
                if (w <= 0x78) {
                    cursorx = w1 + (w + (int32_t)x);
                    len++;
                    redraw = 1;
                } else {
                    edit_buf[len] = 0;
                }
            }
        }
        if ((key & 0xff) < 5 && len != 0) {             /* backspace */
            len--;
            edit_buf[len] = 0;
            int32_t w = measure_control_text_width(edit_buf);
            cursorx = (int32_t)initcursorx + w;
            redraw = 1;
        }
    }

copy_and_exit:
    memcpy(out, edit_buf, 0x30);
    out[0x2f] = 0;
exit_field:
    clear_framebuffer_rect(x, (uint32_t)y, 0x8c, 0xb);
    register_dirty_rect(x, y, x + 0x90, (uint32_t)(y + 0xc));
    return (uint32_t)result;
}

/* call_orig bridge returning EAX (dos_open_file/dos_close_handle, not lifted). */
static uint32_t mh_bridge_eax_ret(uint32_t canon_va, uint32_t eax, uint32_t edx)
{
    regs_t io;
    memset(&io, 0, sizeof io);
    io.va  = canon_va + OBJ_DELTA;
    io.eax = eax;
    io.edx = edx;
#ifndef ROTH_STANDALONE
    call_orig(&io);
    return io.eax;
#else
    switch (canon_va) {   /* routes: the C2 dos wrappers (same re-point other TUs use directly) */
    case 0x41ae5u: return dos_open_file(eax, edx);   /* EAX=path, EDX=mode */
    case 0x41b41u: dos_close_handle(eax); return 0;  /* EAX=handle; callers ignore the return */
    default: break;
    }
    roth_unreachable(canon_va);   /* save/load slot-name path — off bare title */
    return 0;
#endif
}

/* load_and_center_fullscreen_image (canon 0x20e98, 233 B) — load a fullscreen image (DAS resource named
 * by the file at 0x81f86) and blit it CENTERED. EAX = resource id -> EAX = 1 if drawn, else 0. Opens the
 * file (dos_open_file bridge), loads the DAS record (load_das_cache_resource [L]); the image header gives
 * width/height (word[img+4/+6]); centers it in the view (x=(pitch-w)/2, y=(view_h-h)/2, bounded), marks it
 * dirty (register_dirty_rect [L]), uploads its palette unless flagged (upload_dac_palette_8to6 [L]), and
 * blits it (blit_das_image_to_buffer [L], line-doubled in hires); frees the record (pool_free_handle [L])
 * and closes the file (dos_close_handle bridge). In-game live-swap. */
uint32_t load_and_center_fullscreen_image(uint32_t p1)
{
    uint32_t ret = 0;
    uint32_t handle = mh_bridge_eax_ret(0x41ae5, (uint32_t)GADDR(VA_g_dbase300_filename), 0);   /* dos_open_file */
    if (handle != 0) {
        uint32_t rec = load_das_cache_resource(p1, handle);
        if (rec != 0) {
            uint32_t img    = *(uint32_t *)(uintptr_t)rec;
            int32_t  width  = *(int16_t *)(uintptr_t)(img + 4);
            int32_t  height = *(int16_t *)(uintptr_t)(img + 6);
            int32_t  pitch  = (int32_t)G32(VA_g_screen_pitch);
            uint32_t xoff   = (uint32_t)((int32_t)G32(VA_g_screen_pitch) - width)  >> 1;
            uint32_t yoff   = (uint32_t)((int32_t)G32(VA_g_screen_pitch + 0x4) - height) >> 1;
            if (xoff <= 0x140 && yoff <= 0xc8) {
                register_dirty_rect(xoff, (int32_t)yoff, xoff + (uint32_t)width,
                                           (int32_t)yoff + height);
                int32_t yp = (int32_t)yoff * pitch;
                uint32_t fb = (uint32_t)G32(VA_g_framebuffer_ptr) + xoff + (uint32_t)yp;
                uint32_t mode = 1;
                if (G8(VA_g_hires_line_doubling_flag) != 0) { fb += (uint32_t)yp; mode = 0; }   /* hires: double the row base */
                if (!(*(uint8_t *)(uintptr_t)img & 2))
                    upload_dac_palette_8to6(img + 8);
                blit_das_image_to_buffer(img, fb, (uint32_t)pitch, mode);
                ret = 1;
            }
            pool_free_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle),
                                    (uint32_t *)(uintptr_t)rec);
        }
        mh_bridge_eax_ret(0x41b41, handle, 0);   /* dos_close_handle */
    }
    return ret;
}

/* show_fullscreen_image (canon 0x20f81, 363 B, INTERACTIVE) — display a fullscreen image and wait for a
 * click/keypress. EAX = resource id -> EAX = 1 (0 if id==0). Sets the wait cursor / drains input; on first
 * entry saves the current screen to a pool buffer (das_cache_make_room [L] + pool_alloc_handle [L] +
 * play_screen_slide_in [L]); flushes DAS handles (flush_object_das_handles [L]) + begins the screen draw
 * (begin_screen_draw [L]); loads+centers the image (load_and_center_fullscreen_image), presents
 * (flush_dirty_rects [L] + flip_video_page bridge), then blocks polling input (poll_mouse_input [L] +
 * input_ring_dequeue [L]) until a click/key or a cursor action. In-game live-swap (interactive). */
uint32_t show_fullscreen_image(uint32_t p1)
{
    G8(VA_g_mouse_relative_mode) = 0;
    if (p1 == 0)
        return 0;

    if (G32(VA_g_screen_backup_handle + 0x4) == 0) {
        set_cursor_shape(8);
        force_cursor_redraw();
    } else {
        drain_input_and_clear_clicks();
    }
    if (G32(VA_g_damage_flash_level) != 0) {
        G32(VA_g_damage_flash_level) = 0;
        refresh_palette_dac();
    }
    if (G32(VA_g_dialogue_busy_flag) == 0 && G32(VA_g_screen_backup_handle) == 0) {
        if (G32(VA_g_screen_backup_handle + 0x4) == 0)
            G32(VA_g_saved_movement_enabled) = G8(VA_g_player_movement_enabled);
        if (G32(VA_g_dialogue_busy_flag + 0x36) != 0x4d2) {
            uint32_t size = (uint32_t)G32(VA_g_screen_pitch) * (uint32_t)G32(VA_g_screen_height);
            G32(VA_g_screen_backup_handle + 0x8) = 0;
            das_cache_make_room(size);
            uint32_t handle = pool_alloc_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle),
                                                       (int32_t)size);
            G32(VA_g_screen_backup_handle) = (int32_t)handle;
            if (handle != 0) {
                memcpy((void *)(uintptr_t)*(uint32_t *)(uintptr_t)handle,
                       (void *)(uintptr_t)(uint32_t)G32(VA_g_framebuffer_ptr), size);
                play_screen_slide_in(1, handle);
            }
        }
    }
    flush_object_das_handles();
    if (G32(VA_g_screen_backup_handle + 0x4) == 0) {
        G32(VA_g_screen_backup_handle + 0x4) = 1;
        begin_screen_draw();
    }
    G32(VA_g_cutscene_image_handle + 0x9c) = G32(VA_g_mouse_x);
    G32(VA_g_cutscene_image_handle + 0xa0) = G32(VA_g_mouse_y);
    G8(VA_g_player_movement_enabled) = 4;
    G32(VA_g_cutscene_image_handle) = 0;
    if (load_and_center_fullscreen_image(p1) != 0) {
        flush_dirty_rects();
        mh_bridge_eax(0x2e1e8, 3);          /* flip_video_page(3) */
        for (;;) {
            poll_mouse_input();
            if (input_ring_dequeue() == 1)
                break;
            if (G8(VA_g_cursor_primary_action_flag) != 0 || G8(VA_g_cursor_secondary_action_flag) != 0)
                break;
            G8(VA_g_cursor_primary_action_flag) = 0;
            G8(VA_g_cursor_secondary_action_flag) = 0;
        }
    }
    return 1;
}

/* ============================================================================================
 * show_message_box 0x2508f + its 5 dependents (the last 6 of the subsystem).
 * All in-game live-swap, INTERACTIVE.
 * ============================================================================================ */

/* layout_timed_message_text 0x1f3d3 (dialogue_ui — lifted, called direct; re-pointed from the
 * call_orig bridge): lay the newline-joined text out into a per-line table (stride 0x14:
 * [0]/[4]=x-min/x-max, [0x10]=control-string ptr). EAX=line-table out, EDX=work buf, EBX=source
 * text, ECX=pitch, stack[0]=entry count; ret 4 -> EAX = line count (incl. the title line). */
static uint32_t mh_layout_text(uint32_t table, uint32_t work, uint32_t text, uint32_t pitch,
                               uint32_t count)
{
    return (uint32_t)layout_timed_message_text(
        (int32_t *)(uintptr_t)table, (uint8_t *)(uintptr_t)work,
        (const uint8_t *)(uintptr_t)text, (int32_t)pitch, (int32_t)count);
}

/* frame-tile blit: screen_xy_to_framebuffer_ptr [L] -> blit_reloc_das_image [L] with
 * EBX = g_screen_pitch (read live per call, as every original site does). */
static void mh_tile(int32_t x, int32_t y, uint32_t tile)
{
    uint8_t *fb = screen_xy_to_framebuffer_ptr(x, y);
    blit_reloc_das_image((uint32_t)(uintptr_t)fb, tile, (uint32_t)G32(VA_g_screen_pitch));
}

/* show_message_box (canon 0x2508f, 4567 B, INTERACTIVE) — THE modal message-box/menu compositor +
 * blocking input loop. EAX = box descriptor, EDX = flags -> EAX = packed action code - 1
 * (0 = cancelled; sel+1+scroll = committed row; 0x7d0 = gallery; -4 = toggle flipped;
 * 0x?03e8 = settings action codes; flags&0x20 draw-only mode returns the save-under handle).
 *
 * Descriptor (ESI): [0]=title text id, [4]=body text id, word[8]=icon tile id, word[0xa]=last
 * selection, word[0xc]=scroll offset, word[0xe]=scroll range, [0x10]=entry count, entries at
 * +0x14 stride 0xc { [0]=text id, [4]=label base ptr, [8]=value/action dword, byte[0xb]=flags
 * (0x10 slider row, 0x80 conditional text via the toggle table 0x83e8c) }.
 *
 * Flags: 1=fixed 0x120x0x50 menu box (vs content-sized), 2=pre-composited image from
 * g_ui_panel_scratch_handle 0x811ac (DOUBLE deref, gotcha G8), 4=savegame thumbnails,
 * 0x10=entry list widths, 0x20=DRAW-ONLY (no input loop; returns the save-under handle),
 * 0x40=no border, 0x80=accent tile 0x20, 0x100=centered (vs compute_viewport_half_extents),
 * 0x200=zoom-in anim + inner fade, 0x400=space-pad empty entries, 0x1000=no left/right commit.
 *
 * Entry value/action bits (dword [8]): 0x800000 GDV gallery replay, 0x400000 eval dialogue
 * record, 0x80000000 toggle (mutual-exclusion pairs 1<->2 / 3<->4 via the 0x2507f jump table),
 * 0x40000000 text-input row (save name), 0x10000000 slider (+/- 0x10 clamp 0..0x100) with
 * sub-bits 0x4000000 apply-volume / 0x1000000 set_7049a / 0x8000000 gamma LUT 0x71b5c ->
 * 0x708e6, 0x2000000 recurse into run_settings_menu.
 *
 * Everything is call-closed except layout_timed_message_text 0x1f3d3 + flip_video_page 0x2e1e8
 * (call_orig bridges). Original preserves EBX/ECX (push/pop) -> ABI_EAX4 leaves ctx ebx/ecx
 * untouched, which reproduces that. In-game live-swap only (DAS tiles + input ring +
 * non-idempotent scroll state); interactive (shm_tick pumps tick 0x90bcc + the input ring). */
uint32_t show_message_box(uint32_t desc_in, uint32_t flags)
{
    uint32_t *desc    = (uint32_t *)(uintptr_t)desc_in;
    uint8_t  *descb   = (uint8_t *)(uintptr_t)desc_in;
    uint8_t  *entries = descb + 0x14;
    #define MH_SCROLL (*(volatile uint16_t *)(descb + 0xc))
    #define MH_RANGE  (*(volatile uint16_t *)(descb + 0xe))

    /* -- save + freeze UI state ([ebp-0x72..-0x5e]) -- */
    int32_t  saved_busy   = G32(VA_g_screen_busy_depth);
    uint32_t saved_move   = G8(VA_g_player_movement_enabled);
    int32_t  saved_ax     = G32(VA_g_ui_panel_anchor_x);
    int32_t  saved_ay     = G32(VA_g_ui_panel_anchor_y);
    int32_t  saved_rowoff = G32(VA_g_active_weapon_ammo_cap + 0x14);
    uint32_t hold_accum   = 0;                 /* [ebp-0x7a] mouse-hold repeat accumulator */
    uint32_t hold_dir     = 0;                 /* [ebp-0x76] 1=up/inc, 2=down/dec */
    uint32_t hold_thresh  = 0x32;              /* [ebp-0x5e] */
    G8(VA_g_mouse_relative_mode) = 0;                           /* g_mouse_relative_mode */
    G8(VA_g_player_movement_enabled) = 0x11;                        /* g_player_movement_enabled */
    if (saved_busy != 0) {
        G32(VA_g_screen_busy_depth) = 1;
        end_screen_draw();
    }

    /* -- phase 1: resolve title/body/entry text, newline-joined ([ebp-0x622] buffer) -- */
    uint32_t count  = desc[4];                 /* [ebp-0x56] entry count */
    uint32_t boxh   = count * 0xb + 0x17;      /* [ebp-0x4a] */
    uint32_t header_lines = 1;                 /* [ebp-0x4e] */
    uint32_t result = 0;                       /* [ebp-0x46] local_f4 (exit code + 1) */
    uint32_t zoom_save = 0;                    /* [ebp-0x3a] inner save-under (zoom fade) */
    int32_t  box_y  = (int32_t)(((uint32_t)G32(VA_g_screen_pitch + 0x4) - boxh) >> 1);   /* edi */
    uint8_t  text[1200];                       /* [ebp-0x622] */
    uint8_t  scratch[1200];                    /* [ebp-0xad2] */
    uint8_t *cur = text;
    uint32_t len;

    if (desc[0] != 0)
        len = resolve_dbase100_text((uint32_t)(uintptr_t)cur, 0x50, desc[0], 0);
    else
        len = 1;
    cur += len;
    cur[-1] = 0xa;
    if (desc[1] != 0) {                        /* body line */
        len = resolve_dbase100_text((uint32_t)(uintptr_t)cur, 0x50, desc[1], 0);
        header_lines = 2;
        cur += len;
        cur[-1] = 0xa;
    }
    {
        uint8_t *ent = entries;
        for (uint32_t i = 0; i < desc[4]; i++, ent += 0xc) {
            uint32_t id = *(uint32_t *)(void *)ent;
            if ((ent[0xb] & 0x80) &&
                *(volatile int32_t *)(GADDR(VA_g_active_weapon_ammo_cap + 0x18) + (*(uint32_t *)(void *)(ent + 8) & 0xffu) * 4) != 0)
                id = *(uint32_t *)(void *)(ent + 4);
            if (id == 0) {
                if (flags & 0x400) { *cur = 0x20; len = 2; }
                else               len = 1;
            } else {
                len = resolve_dbase100_text((uint32_t)(uintptr_t)cur, 0x50, id, 0);
            }
            cur += len;
            cur[-1] = 0xa;
        }
    }
    *cur = 0;

    /* -- phase 2: lay the text out into the per-line table (BRIDGE 0x1f3d3) -- */
    G8(VA_g_timed_message_color) = G8(VA_g_default_message_color + 0x5);                 /* g_timed_message_color = default color */
    uint32_t minx = 8000;                      /* [ebp-0x2a] */
    uint32_t linetab[16 * 5];                  /* [ebp-0x172]; original capacity 10 lines */
    uint32_t nlines = mh_layout_text((uint32_t)(uintptr_t)linetab, (uint32_t)(uintptr_t)scratch,
                                     (uint32_t)(uintptr_t)text, (uint32_t)G32(VA_g_screen_pitch), count);

    /* widest/narrowest content line (lines 1..n-1; line 0 = title) */
    {
        int32_t maxx = 0;
        uint32_t *t = linetab + 5;
        for (uint32_t i = 1; i < nlines; i++, t += 5) {
            if (maxx < (int32_t)t[1]) maxx = (int32_t)t[1];
            if ((int32_t)t[0] < (int32_t)minx) minx = t[0];
        }
        uint32_t content_w = (uint32_t)maxx - minx;          /* [ebp+0x82] */
        uint32_t title_w   = linetab[1] - linetab[0];
        if (title_w > content_w + 0x16)
            content_w = title_w - 0x16;

        if (flags & 0x10) {
            /* entry-list width pass: sliders full width, plain rows discounted 0x26 */
            uint32_t maxw = 0, nslider = 0;                  /* ecx / [ebp-0x1e] */
            uint32_t *tt = linetab + 5;
            uint8_t  *ent = entries;
            for (uint32_t i = 0; i < nlines - 1; i++, tt += 5, ent += 0xc) {
                uint32_t w = tt[1] - tt[0];
                if (ent[0xb] & 0x10) nslider++;
                else                 w -= 0x26;
                if (maxw < w) maxw = w;
            }
            if (maxw != 0)
                content_w = maxw + 0x26;
            /* slider-row x adjust: re-embed the slider label x at minx+0x13 */
            uint32_t lo    = minx + 0x13;                    /* ecx */
            uint32_t left  = minx - 0x13;                    /* [ebp-0x26] */
            uint32_t hib   = (lo >> 8) & 0xff;               /* [ebp-0xe] */
            uint32_t old_x = 0;   /* [ebp-0x22] — faithful STALE CARRY across iterations
                                   * (uninitialized in the original on a first non-type-2 label) */
            tt = linetab + 5;
            ent = entries;
            for (uint32_t i = 0; i < nlines - 1; i++, tt += 5, ent += 0xc) {
                if (!(ent[0xb] & 0x10))
                    continue;
                uint8_t *lbl = (uint8_t *)(uintptr_t)tt[4];
                if (nslider != 1) {
                    if (lbl[2] == 2) {
                        old_x = ((uint32_t)lbl[4] << 8) | lbl[3];
                        lbl[3] = (uint8_t)lo;
                        lbl[4] = (uint8_t)hib;
                    }
                    tt[1] += lo - old_x;
                    tt[0]  = left;
                } else {
                    uint32_t ox = tt[0] - 0x13;
                    tt[0] = ox;
                    if (lbl[2] == 2) {
                        ox += 0x26;
                        lbl[3] = (uint8_t)ox;
                        lbl[4] = (uint8_t)((ox >> 8) & 0xff);
                    }
                    tt[1] += 0x13;
                }
            }
        }

        /* -- phase 3+4: size the window + composite the chrome -- */
        int32_t  box_x;                        /* [ebp-0x3e] */
        uint32_t box_w;                        /* [ebp+0x7a] */
        uint32_t save_under;                   /* [ebp-0x42] local_f0 */

        if (flags & 1) {
            /* FIXED 0x120 x 0x50 menu box at the panel anchor */
            if (flags & 0x100) {
                G32(VA_g_ui_panel_anchor_x) = (int32_t)(((uint32_t)G32(VA_g_screen_pitch) - 0x120) >> 1);
                G32(VA_g_ui_panel_anchor_y) = (int32_t)(((uint32_t)G32(VA_g_screen_pitch + 0x4) - 0x50) >> 1);
            } else {
                compute_viewport_half_extents();
            }
            G32(VA_g_active_weapon_ammo_cap + 0x14) = 0x2e;
            box_w = 0x120;
            boxh  = 0x50;
            box_x = G32(VA_g_ui_panel_anchor_x);
            box_y = G32(VA_g_ui_panel_anchor_y);
            save_under = save_framebuffer_region((uint32_t)(box_x - 4), (uint32_t)(box_y - 4),
                                                        0x128, 0x58, NULL);
            if (save_under == 0) { result = 1; goto teardown; }
            if (!(flags & 0x40)) {
                if (flags & 0x200)
                    draw_menu_box_zoom_anim(box_x, box_y, 0x11f, 0x50, save_under);
                else
                    draw_popup_shadow_border_smc(box_x, box_y, 0x11f, 0x50);
            }
            mh_tile(box_x, box_y, 0x328);                       /* ornate 288x80 panel bg */
            if (flags & 0x80)
                mh_tile(box_x + 3, box_y + 0x22, 0x20);         /* accent */
            if (*(uint16_t *)(void *)(descb + 8) != 0)
                mh_tile(box_x + 4, box_y + 4, *(uint16_t *)(void *)(descb + 8));
            if (flags & 2) {
                if (G32(VA_g_ui_panel_scratch_handle) != 0) {                        /* G8: DOUBLE deref of the handle */
                    uint32_t img = *(uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_ui_panel_scratch_handle);
                    uint8_t *fb = screen_xy_to_framebuffer_ptr(box_x + 0x24, box_y + 0x10);
                    blit_das_image_auto_scale((uint32_t)(uintptr_t)fb, img, (uint32_t)G32(VA_g_screen_pitch));
                }
            } else if (!(flags & 4)) {
                mh_tile(box_x + 0x27, box_y + 0xf, 0x360);      /* accent 73x58 */
            }
            draw_scroll_indicators(desc_in, box_x, box_y);
        } else {
            /* CONTENT-SIZED dialog */
            box_w = content_w + 0x41;
            box_x = (int32_t)(((uint32_t)G32(VA_g_screen_pitch) - box_w) >> 1);
            G32(VA_g_active_weapon_ammo_cap + 0x14) = 0xd;
            save_under = save_framebuffer_region((uint32_t)(box_x - 4), (uint32_t)(box_y - 4),
                                                        box_w + 8, boxh + 8, NULL);
            if (save_under == 0) { result = 1; goto teardown; }
            if (flags & 0x200)
                draw_menu_box_zoom_anim(box_x, box_y, box_w - 1, boxh, save_under);
            else
                draw_popup_shadow_border_smc(box_x, box_y, (int32_t)(box_w - 1), (int32_t)boxh);
            {   /* left ornate column 0xd0, its height field patched to the box height */
                uint32_t strip = resolve_reloc_ptr(0xd0);    /* 0x18e2c == 0x226c6 (dup) */
                if (strip != 0) {
                    *(uint16_t *)(uintptr_t)(strip + 6) = (uint16_t)(boxh - 3);
                    uint8_t *fb = screen_xy_to_framebuffer_ptr(box_x, box_y);
                    blit_das_image_auto_scale((uint32_t)(uintptr_t)fb, strip, (uint32_t)G32(VA_g_screen_pitch));
                }
            }
            if (*(uint16_t *)(void *)(descb + 8) != 0)
                mh_tile(box_x + 4, box_y + 4, *(uint16_t *)(void *)(descb + 8));
            mh_tile(box_x, box_y + (int32_t)boxh - 3, 0xd8);    /* bottom shadow rule */
            /* top edge: corner 0x2b8 then 0x33-wide 0x2a0 tiles + remainder tail */
            uint32_t rem = box_w - 0x47;                        /* [ebp-6] */
            mh_tile(box_x + 0x22, box_y, 0x2b8);
            int32_t tx = box_x + 0x3f;
            while (rem >= 0x33) {
                mh_tile(tx, box_y, 0x2a0);
                rem -= 0x33;
                tx  += 0x33;
            }
            if (rem != 0)
                mh_tile(tx - 0x33 + (int32_t)rem, box_y, 0x2a0);
            mh_tile(box_x + (int32_t)box_w - 8, box_y, 0x318);  /* top-right corner */
            /* interior clear + right edge column (one 7x12 tile per entry row) */
            int32_t rowy = box_y + 0xf;                         /* [ebp-0xa] */
            clear_framebuffer_rect((uint32_t)(box_x + 0x22), (uint32_t)rowy,
                                          box_w - 0x2a, count * 0xb + 1);
            for (uint32_t i = 0; i < count; i++) {
                mh_tile(box_x + (int32_t)box_w - 8, rowy, 0x2a8);
                rowy += 0xb;
            }
            rowy += 1;
            mh_tile(box_x + 0x21, rowy, 0x320);                 /* bottom-left corner */
            {   /* bottom rule 0x2c8: EBX packed ((w-0x2a)<<16) | pitch (width clip in the hi word) */
                uint8_t *fb = screen_xy_to_framebuffer_ptr(box_x + 0x23, rowy);
                blit_reloc_das_image((uint32_t)(uintptr_t)fb, 0x2c8,
                                            ((box_w - 0x2a) << 16) + (uint32_t)G32(VA_g_screen_pitch));
            }
            mh_tile(box_x + (int32_t)box_w - 8, rowy, 0x2b0);   /* bottom-right corner */
        }

        /* -- phase 5 (common, 0x257c5): draw the title/body + entry list -- */
        int32_t content_y = box_y + 0x10;      /* [ebp-2] */
        int32_t text_x    = box_x + 0x23;      /* [ebp+2] */
        {
            uint8_t *t0 = (uint8_t *)(uintptr_t)linetab[4];
            t0[3] = (uint8_t)text_x;
            t0[1] = G8(VA_g_map_menu_marker_selected);               /* selected-marker color for the title */
            t0[4] = (uint8_t)((uint32_t)text_x >> 8);
            draw_text_at_screen_xy(linetab[4], 0, (uint32_t)(box_y + 2), 0);
        }
        if (desc[1] != 0) {
            text_x += 0x58;
            uint8_t *t1 = (uint8_t *)(uintptr_t)linetab[5 + 4];
            t1[1] = G8(VA_g_map_menu_marker_selected);
            t1[3] = (uint8_t)text_x;
            t1[4] = (uint8_t)((uint32_t)text_x >> 8);
            nlines--;
            draw_text_at_screen_xy((uint32_t)(uintptr_t)t1, 0, (uint32_t)(content_y - 0xe), 0);
        }
        nlines--;                              /* drop the title line -> entry-row count */
        uint32_t *ctab = linetab + header_lines * 5;    /* [ebp-0x52] content line table */
        int32_t  prev_sel  = -2;               /* [ebp+6] */
        int32_t  prev_hit  = -2;               /* [ebp+0x12] */
        uint32_t shape_prev = 0;               /* [ebp+0x1a] */
        int32_t  kb_nav    = 1;                /* [ebp+0x1e] */
        uint32_t tick_accum = 2;               /* [ebp+0x26] */
        uint32_t dirty_y1 = boxh + (uint32_t)box_y + 4;
        uint32_t dirty_x1 = (uint32_t)box_x + box_w + 4;
        int32_t  dirty_y0 = box_y - 4;
        uint32_t dirty_x0 = (uint32_t)(box_x - 4);
        register_dirty_rect(dirty_x0, dirty_y0, dirty_x1, dirty_y1);
        int32_t  thumb_redraw = 0;             /* [ebp+0x2a] */
        render_menu_entry_list((uint32_t)(uintptr_t)ctab, content_y, nlines, desc_in);
        int32_t  sel = *(uint16_t *)(void *)(descb + 0xa);   /* [ebp+0xa] */
        G8(VA_g_cursor_secondary_action_flag) = 0;
        G8(VA_g_cursor_primary_action_flag) = 0;
        uint32_t tick_last = G16(VA_g_frame_tick_counter);     /* [ebp+0x92] (16-bit) */

        if (flags & 0x20) {
            /* DRAW-ONLY mode: present and hand the save-under handle back (+1; ret is -1) */
            flush_dirty_rects();
            mh_bridge_eax(0x2e1e8, 3);                          /* flip_video_page(3) */
            register_dirty_rect(dirty_x0, dirty_y0, dirty_x1, dirty_y1);
            result = save_under + 1;
            goto teardown;
        }

        /* zoom fade: an inner save-under + shade descriptor ([ebp-0xaa..-0x9a]) */
        uint32_t shade_desc[5];
        if (flags & 0x200) {
            zoom_save = save_framebuffer_region((uint32_t)box_x, (uint32_t)box_y,
                                                       box_w, boxh, NULL);
            uint32_t hh = boxh;
            if (G8(VA_g_hires_line_doubling_flag) != 0) hh += hh;                     /* hires: doubled rows */
            shade_desc[0] = (uint32_t)(uintptr_t)screen_xy_to_framebuffer_ptr(box_x, box_y);
            shade_desc[1] = box_w;
            shade_desc[2] = hh;
            shade_desc[3] = (uint32_t)G32(VA_g_screen_pitch);
        }

        /* loop invariants */
        int32_t  thumb_y  = box_y + 0x10;      /* [ebp-0x8a] */
        int32_t  thumb_x  = box_x + 0x24;      /* [ebp+0x5a] */
        int32_t  thumb_y1 = box_y + 0x48;      /* [ebp+0x5e] */
        int32_t  thumb_x1 = box_x + 0x72;      /* [ebp+0x62] */
        uint32_t br_y = boxh + (uint32_t)box_y;         /* [ebp+0x4a] */
        uint32_t br_x = (uint32_t)box_x + box_w;        /* [ebp+0x4e] */
        int32_t  list_y = box_y + 0xf;         /* [ebp+0x42] */
        int32_t  list_x = box_x + 0x55;        /* [ebp+0x46] */
        int32_t  last_idx = (int32_t)nlines - 1;        /* [ebp+0x66] */
        int32_t  present = 1;                  /* [ebp+0x2e] */

        /* -- phase 6: the blocking input loop -- */
        do {
            update_frame_time_scale();
            service_audio_sequence();
            uint8_t  key = input_ring_dequeue();         /* [ebp+0x96] */
            uint32_t cursor_shape = 0x240;                      /* [ebp+0x16] */
            uint32_t action = 0;                                /* [ebp+0x76] 1=inc 2=dec */
            int32_t  hit = hit_test_ui_element((int32_t *)ctab, G32(VA_g_mouse_x),
                                                      (uint32_t)G32(VA_g_mouse_y), nlines,
                                                      (uint32_t)content_y, flags, box_x, box_y);
            int32_t  hit_valid = (hit != 0);                    /* [ebp+0x22] */
            if (hit == -6 && MH_SCROLL == 0)        hit = 0;    /* up arrow, already at top */
            if (hit == -7 && MH_SCROLL == MH_RANGE) hit = 0;    /* down arrow, at bottom */
            if (hit == 0) {
                if (kb_nav == 0) sel = -2;
            } else {
                cursor_shape = 0x248;
                if (hit != prev_hit) {
                    hold_accum  = 0;
                    hold_thresh = 0x19;
                    sel = (hit != -1) ? hit - 1 : -1;
                    kb_nav = 0;
                }
            }
            /* mouse-hold repeat (scroll arrows / sliders) */
            if (hold_accum != 0) {
                if ((G8(VA_g_mouse_buttons_prev) & 3) == 0) {
                    hold_accum = 0;
                    hold_thresh = 0x19;
                } else {
                    hold_accum += (uint32_t)G32(VA_g_frame_time_scale);       /* g_frame_time_scale */
                    if (hold_accum > hold_thresh) {
                        hold_accum = 0;
                        hold_thresh = 8;
                        if (hold_dir == 2) G8(VA_g_cursor_primary_action_flag) = 1;
                        else               G8(VA_g_cursor_secondary_action_flag) = 1;
                    }
                }
            }
            if (G8(VA_g_mouse_buttons_prev) & 2)      cursor_shape = 0x268;
            else if (G8(VA_g_mouse_buttons_prev) & 1) cursor_shape = 0x270;
            /* cursor action flags -> synthesize a key from the hit */
            if (G8(VA_g_cursor_secondary_action_flag) != 0 || G8(VA_g_cursor_primary_action_flag) != 0) {
                uint32_t a = (G8(VA_g_cursor_secondary_action_flag) == 0) ? 1u : 0u;
                G8(VA_g_cursor_primary_action_flag) = 0;
                G8(VA_g_cursor_secondary_action_flag) = 0;
                action = a + 1;
                if (hit_valid) {
                    if (hit == -6)      key = 0xfe;
                    else if (hit == -7) key = 0xfd;
                    else if (hit == -9) key = 0x32;
                    else if (sel >= 0)  key = 0x1c;
                    else if (sel > -2)  key = 1;
                }
            }
            prev_hit = hit;
            /* arrow-key navigation */
            if (key == 0x48) {
                kb_nav = 1;
                if (sel == 0 && MH_RANGE != 0) {
                    key = 0xfe;
                } else {
                    sel--;
                    if (sel < 0) sel = last_idx;
                }
            } else if (key == 0x50) {
                kb_nav = 1;
                if (MH_RANGE != 0 && sel == last_idx) {
                    key = 0xfd;
                } else {
                    sel++;
                    if ((uint32_t)sel >= nlines) sel = 0;
                }
            }
            /* scroll the list window */
            {
                int scrolled = 0;
                if (key == 0xfe && MH_SCROLL != 0) {
                    hold_accum = 1;
                    hold_dir = 1;
                    MH_SCROLL = (uint16_t)(MH_SCROLL - 1);
                    scrolled = 1;
                } else if (key == 0xfd && MH_SCROLL < MH_RANGE) {
                    hold_accum = 1;
                    hold_dir = 1;
                    MH_SCROLL = (uint16_t)(MH_SCROLL + 1);
                    scrolled = 1;
                }
                if (scrolled) {
                    draw_scroll_indicators(desc_in, box_x, box_y);
                    clear_framebuffer_rect((uint32_t)(list_x + G32(VA_g_active_weapon_ammo_cap + 0x14)),
                                                  (uint32_t)list_y, 0x8c, 0x38);
                    prev_sel = sel - 1;                         /* force re-render */
                }
            }
            /* zoom fade: restore the inner region each frame */
            if (zoom_save != 0)
                blit_descriptor_rows(zoom_save);
            /* selection changed: recolor the labels + re-render the list */
            if (sel != prev_sel) {
                prev_sel = sel;
                uint32_t *t = ctab;
                for (uint32_t i = 0; i < nlines; i++, t += 5) {
                    uint8_t *s = (uint8_t *)(uintptr_t)t[4];
                    if (s != NULL && s[0] == 1)
                        s[1] = G8(VA_g_default_message_color + 0x5);
                }
                if ((uint32_t)sel < nlines) {
                    uint8_t *s = (uint8_t *)(uintptr_t)linetab[((uint32_t)sel + header_lines) * 5 + 4];
                    if (s != NULL && s[0] == 1)
                        s[1] = G8(VA_g_map_menu_marker_selected);
                    thumb_redraw = 1;
                }
                present = 1;
                render_menu_entry_list((uint32_t)(uintptr_t)ctab, content_y, nlines, desc_in);
            }
            /* savegame thumbnail for the selected slot */
            if (thumb_redraw != 0) {
                if ((uint32_t)sel < nlines && (flags & 4)) {
                    uint32_t th = read_savegame_thumbnail((uint32_t)MH_SCROLL + (uint32_t)sel);
                    if (th != 0) {
                        uint32_t img = *(uint32_t *)(uintptr_t)th;
                        uint8_t *fb = screen_xy_to_framebuffer_ptr(thumb_x, thumb_y);
                        blit_das_image_auto_scale((uint32_t)(uintptr_t)fb, img,
                                                         (uint32_t)G32(VA_g_screen_pitch));
                        free_das_image_handle(th);
                        present = 1;
                        register_dirty_rect((uint32_t)thumb_x, thumb_y,
                                                   (uint32_t)thumb_x1, (uint32_t)thumb_y1);
                    }
                }
                thumb_redraw = 0;
            }
            /* zoom fade: shade the restored region for 0x20 ticks after open */
            if ((flags & 0x200) && zoom_save != 0) {
                uint32_t now = G16(VA_g_frame_tick_counter);
                tick_accum += now - (tick_last & 0xffff);
                tick_last = now;
                if (tick_accum >= 0x20) {
                    free_das_cache_handle(zoom_save);
                    zoom_save = 0;
                } else {
                    blit_save_region((uint32_t *)(uintptr_t)zoom_save);
                    shade_desc[4] = 0x20 - tick_accum;
                    shade_remap_blit((uint8_t *)shade_desc);
                }
                present = 1;
                register_dirty_rect((uint32_t)box_x, box_y, br_x, br_y);
            }
            if (present != 0) {
                flush_dirty_rects();
                present = 0;
                mh_bridge_eax(0x2e1e8, 3);                      /* flip_video_page(3) */
            }
            /* result decode */
            if (key == 0x32)
                result = 0x7d1;                                 /* gallery */
            {
                uint32_t code = (uint32_t)sel + 2;
                if (key == 0x1c) {                              /* Enter / click commit */
                    if (sel < 0) result = 1;
                    else         result = code + MH_SCROLL;
                } else if (key == 0x4d) {                       /* right: slider inc */
                    if (!(flags & 0x1000) && sel >= 0) {
                        action = 1;
                        result = code + MH_SCROLL;
                    }
                } else if (key == 1 || key == 0x20) {           /* Esc: cancel */
                    result = 1;
                } else if (key == 0x4b && !(flags & 0x1000)) {  /* left: slider dec */
                    if (sel < 0 || !(entries[(uint32_t)sel * 0xc + 0xb] & 0x10))
                        result = 1;
                    else {
                        action = 2;
                        result = code + MH_SCROLL;
                    }
                }
            }
            /* -- activation: 2 <= result < 1000 selects an entry -- */
            if (result > 1 && result < 0x3e8) {
                uint8_t *ent = entries + (uint32_t)sel * 0xc;   /* [ebp-0x86] */
                uint32_t val = *(uint32_t *)(void *)(ent + 8);  /* [ebp+0x52] */
                if (val & 0x800000) {                           /* GDV gallery replay */
                    if ((uint32_t)sel < nlines) {
                        uint32_t base = *(uint32_t *)(void *)(ent + 4);
                        uint32_t gid = *(uint8_t *)(uintptr_t)
                            (base + ((uint32_t)MH_SCROLL + (uint32_t)sel) * 0x32 + 0x31);
                        uint32_t rec = (uint32_t)G32(VA_g_dbase100_base);  /* g_dbase100_base */
                        rec = rec + *(uint32_t *)(uintptr_t)(rec + 0x24) + gid * 0x14;
                        if (*(uint8_t *)(uintptr_t)rec != 0) {
                            /* 0x25f07 call 0x20c16 (play_record_gdv_cutscene, EAX=rec). The old
                             * standalone hatch mislabeled this as 0x26628 and assumed the gallery
                             * unreachable; the lifted body is standalone-proven (dbase100.c:224
                             * dispatches 0x20c16 to it image-free on the boot-intro path). */
                            play_record_gdv_cutscene(rec);   /* cutscene gallery replay */
                            G32(VA_g_dialogue_busy_flag + 0x36) = 0;
                            finish_dialogue_record_eval();
                        }
                    }
                    result = 0;
                }
                if (val & 0x400000) {                           /* eval a dialogue record */
                    result = 0;
                    eval_dialogue_record_by_id(val & 0xff);
                    G32(VA_g_dialogue_busy_flag + 0x36) = 0;
                    finish_dialogue_record_eval();
                }
                if (val & 0x80000000u) {                        /* toggle (mutual-exclusion pairs) */
                    uint32_t idx = val & 0xff;
                    volatile int32_t *tg = (volatile int32_t *)(GADDR(VA_g_active_weapon_ammo_cap + 0x18) + idx * 4);
                    uint32_t nv = ~(uint32_t)*tg;
                    *tg = (int32_t)nv;
                    if (nv != 0) {                              /* jump table @0x2507f */
                        switch (idx) {
                        case 1: G32(VA_g_voice_decode_suspended) = 0; break;
                        case 2: G32(VA_g_active_weapon_ammo_cap + 0x1c) = 0; break;
                        case 3: G32(VA_g_voice_decode_suspended + 0x8) = 0; break;
                        case 4: G32(VA_g_voice_decode_suspended + 0x4) = 0; break;
                        }
                    }
                    result = 0xfffffffd;                        /* -3 -> box returns -4 */
                } else if (val & 0x40000000u) {                 /* text-input row (save name) */
                    if ((flags & 2) && G32(VA_g_ui_panel_scratch_handle) != 0) {
                        uint32_t img = *(uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_ui_panel_scratch_handle);
                        uint8_t *fb = screen_xy_to_framebuffer_ptr(thumb_x, thumb_y);
                        blit_das_image_auto_scale((uint32_t)(uintptr_t)fb, img,
                                                         (uint32_t)G32(VA_g_screen_pitch));
                        thumb_redraw = 1;
                        register_dirty_rect((uint32_t)thumb_x, thumb_y,
                                                   (uint32_t)thumb_x1, (uint32_t)thumb_y1);
                    }
                    set_cursor_shape(cursor_shape);
                    char labelbuf[12];                          /* [ebp-0x96], 12 B */
                    uint32_t nameptr = *(uint32_t *)(void *)(ent + 4)
                                     + ((uint32_t)MH_SCROLL + (uint32_t)sel) * 0x30;
                    mh_sprintf_ds(labelbuf, 0x75f00, (int32_t)((uint32_t)sel + MH_SCROLL + 1),
                                  (const char *)(uintptr_t)nameptr);   /* "%D: " (ptr arg unused) */
                    uint32_t r = render_text_input_field((uint32_t)(uintptr_t)labelbuf,
                                     nameptr, (uint32_t)GADDR(VA_g_message_resource_handle + 0x14),
                                     (uint32_t)(G32(VA_g_ui_panel_anchor_x) + G32(VA_g_active_weapon_ammo_cap + 0x14) + 0x54),
                                     sel * 0xb + content_y);
                    if (r == 0) {                               /* backed out */
                        prev_sel = -5;
                        result = 0;
                    }
                    if (r == 2) {                               /* cancelled by a click elsewhere */
                        int32_t h2 = hit_test_ui_element((int32_t *)ctab, G32(VA_g_mouse_x),
                                         (uint32_t)G32(VA_g_mouse_y), nlines, (uint32_t)content_y,
                                         flags, box_x, box_y);
                        if (h2 != hit) {
                            prev_sel = -5;
                            result = 0;
                        }
                    }
                } else if (val & 0x10000000u) {                 /* slider row */
                    uint32_t v = val;                           /* edx */
                    if (action == 1) {
                        v += 0x10;
                        if ((int32_t)(int16_t)v > 0x100) v = 0x100;
                        hold_accum = 1; hold_dir = 1;
                        result = 0;
                        prev_sel = -8;
                    }
                    if (action == 2) {
                        v -= 0x10;
                        if ((int32_t)(int16_t)v < 0) v = 0;
                        hold_accum = 1; hold_dir = 2;
                        result = 0;
                        prev_sel = -8;
                    }
                    *(uint32_t *)(void *)(ent + 8) =
                        (val & 0xffff0000u) | (uint32_t)(int32_t)(int16_t)v;
                    if (val & 0x4000000)
                        apply_audio_volume_settings();
                    if (val & 0x1000000)
                        set_7049a_from_71988();
                    if (val & 0x8000000) {                      /* gamma LUT */
                        int32_t gi = ((int32_t)(int16_t)v) >> 4;
                        cursor_shape = 0x268;
                        G32(VA_g_cursor_mask_data) = (int32_t)*(volatile uint8_t *)(GADDR(VA_g_vol_music + 0x4) + gi);
                    }
                }
                if (val & 0x2000000) {                          /* recurse: settings dispatcher */
                    int32_t save_off = G32(VA_g_active_weapon_ammo_cap + 0x14);            /* 0x24c72 preserves EBX ->
                                                                 * the caller restores 0x83e88 */
                    result = run_settings_menu(val, result);
                    G32(VA_g_active_weapon_ammo_cap + 0x14) = save_off;
                }
            }
            if (cursor_shape != shape_prev) {
                shape_prev = cursor_shape;
                set_cursor_shape(cursor_shape);
            }
        } while (result == 0);

        /* -- phase 7: teardown of the loop tier -- */
        if (zoom_save != 0)
            free_das_cache_handle(zoom_save);
        free_das_cache_handle(save_under);
        register_dirty_rect((uint32_t)(box_x - 4), box_y - 4,
                                   (uint32_t)box_x + box_w + 4, (uint32_t)box_y + boxh + 4);
        G32(VA_g_frame_time_scale) = 0;                       /* g_frame_time_scale */
        G16(VA_g_last_frame_tick) = G16(VA_g_frame_tick_counter);            /* g_last_frame_tick */
        if (sel >= 0)
            *(uint16_t *)(void *)(descb + 0xa) = (uint16_t)sel;
    }

teardown:
    if (saved_busy != 0)
        begin_screen_draw();
    G8(VA_g_player_movement_enabled) = (uint8_t)saved_move;
    G32(VA_g_screen_busy_depth) = saved_busy;
    G32(VA_g_ui_panel_anchor_x) = saved_ax;
    G32(VA_g_ui_panel_anchor_y) = saved_ay;
    G32(VA_g_active_weapon_ammo_cap + 0x14) = saved_rowoff;
    return result - 1;
    #undef MH_SCROLL
    #undef MH_RANGE
}

/* show_resource_error_box (canon 0x2632a, 18 B) — show_message_box(&0x71d05, 0x200). The push/pop
 * thunk preserves EDX only; show_message_box's EAX (the user's button: 1=retry / 2=give-up) flows
 * through to ret, and callers DO branch on it (prime_voice_clip 0x1e54d). Was declared void
 * (dropped return). */
uint32_t show_resource_error_box(void)
{
    return show_message_box((uint32_t)GADDR(VA_g_message_box_state), 0x200);
}

/* show_simple_message_box (canon 0x2633c, 13 B) — show_message_box(&0x71cd9, 0x1200); jmps into
 * the shared call+pop+ret tail of show_resource_error_box (the corpus flow_succ edge), so EAX
 * flows through identically. */
uint32_t show_simple_message_box(void)
{
    return show_message_box((uint32_t)GADDR(VA_g_vol_music + 0x181), 0x1200);
}

/* run_settings_menu (canon 0x24c72, 588 B, INTERACTIVE) — the settings dispatcher recursed into
 * from show_message_box (entry bit 0x2000000). AL = case id, EDX = default return -> EAX
 * (0x?03e9 action codes override the default). Preserves EBX/ECX (push/pop) — the caller's
 * `mov ebx,[0x83e88] / call / mov [0x83e88],ebx` restore rides on that.
 * case 0 = quit confirm (0x71a00 -> 0x203e9); case 1 = the settings submenu loop
 * (0x71a9c reduced / 0x71a58 full per g_menu_frontend_flag 0x83ea0) -> Volume 0x71b18 /
 * Subtitles 0x71ad4 (re-shown while a toggle returns -4) / Screen (reduced 0x719e0, or the full
 * VESA path: mode-list build via code-resident latch byte 0x14770 + mode words 0x1471e/0x14728/
 * 0x1475a/0x14764, list box 0x7196c -> 0x503e9, low-memory warning 0x7194c -> 0x603e9) /
 * Input 0x719e0; case 2 = 0x71a2c confirm -> 0x703e9; case 3 = 0x71914 info box. */
uint32_t run_settings_menu(uint32_t item, uint32_t default_ret)
{
    uint32_t ret = default_ret;                 /* ebx */
    switch (item & 0xff) {
    case 0:
        if (show_message_box((uint32_t)GADDR(VA_g_mouse_speed + 0x4), 0) == 1)
            ret = 0x203e9;
        break;
    case 1:
        for (;;) {
            uint32_t r = show_message_box(
                (uint32_t)(G32(VA_g_menu_frontend_flag) != 0 ? GADDR(VA_g_mouse_speed + 0xa0) : GADDR(VA_g_mouse_speed + 0x5c)), 0);
            if (r - 1 <= 3) {                   /* jump table @0x24c52 */
                switch (r) {
                case 1:                         /* Volume */
                    show_message_box((uint32_t)GADDR(VA_g_mouse_speed + 0x11c), 0x10);
                    break;
                case 2:                         /* Subtitles: re-show while a toggle flips (-4) */
                    while (show_message_box((uint32_t)GADDR(VA_g_mouse_speed + 0xd8), 0) == 0xfffffffc)
                        ;
                    break;
                case 3:                         /* Screen */
                    if (G32(VA_g_menu_frontend_flag) != 0) {
                        show_message_box((uint32_t)GADDR(VA_g_choice_selected_index + 0x670), 0x10);
                    } else {
                        /* full VESA screen-mode list. 0x14770/0x1471e/0x14728/0x1475a/0x14764
                         * are CODE-RESIDENT (obj1) mode-table state kept by
                         * init_video_mode_table_once [L] — read/write the live bytes. */
                        if (G8(VA_g_vesa_mode_table_built) == 2)
                            G8(VA_g_vesa_mode_table_built) = 0;
                        if (G8(VA_g_vesa_mode_table_built) == 0) {
                            uint32_t h = show_message_box((uint32_t)GADDR(VA_g_choice_selected_index + 0x584), 0x20);
                            init_video_mode_table_once();    /* draw-only box up meanwhile */
                            free_das_cache_handle_if(h);
                        }
                        G32(VA_g_choice_selected_index + 0x60c) = 4;       /* the Screen list desc 0x7196c entry count */
                        uintptr_t e = GADDR(VA_g_choice_selected_index + 0x640);   /* entry 4 of desc 0x7196c */
                        if (G16(VA_g_vesa_scale_and_mode_table + 0x36) != 0) {
                            G32(VA_g_choice_selected_index + 0x60c) = 5;
                            G32(VA_g_choice_selected_index + 0x640) = 0x3a;
                            G32(VA_g_choice_selected_index + 0x648) = 1;
                            e = GADDR(VA_g_choice_selected_index + 0x64c);
                        }
                        if (G16(VA_g_vesa_scale_and_mode_table + 0x40) != 0) {
                            G32(VA_g_choice_selected_index + 0x60c) += 1;
                            *(volatile uint32_t *)e = 0x3b;
                            *(volatile uint32_t *)(e + 8) = 2;
                            e += 0xc;
                        }
                        if (G16(VA_g_vesa_scale_and_mode_table + 0x72) != 0) {
                            G32(VA_g_choice_selected_index + 0x60c) += 1;
                            *(volatile uint32_t *)e = 0x3c;
                            *(volatile uint32_t *)(e + 8) = 7;
                            e += 0xc;
                        }
                        if (G16(VA_g_vesa_scale_and_mode_table + 0x7c) != 0) {
                            G32(VA_g_choice_selected_index + 0x60c) += 1;
                            *(volatile uint32_t *)e = 0x3d;
                            *(volatile uint32_t *)(e + 8) = 8;
                        }
                        uint8_t prev_res = G8(VA_g_cfg_das2_arg + 0x1be);
                        G16(VA_g_choice_selected_index + 0x618) = 0;       /* word clear, then dword add (width faithful) */
                        G32(VA_g_choice_selected_index + 0x618) += (int32_t)((0x10u - prev_res) << 4);
                        uint32_t r2 = show_message_box((uint32_t)GADDR(VA_g_choice_selected_index + 0x5fc), 0x10);
                        if (r2 != 0) {
                            uint32_t mode = *(volatile uint32_t *)
                                (GADDR(VA_g_choice_selected_index + 0x610) + (r2 - 1) * 0xc + 8) & 0xffff;
                            G32(VA_g_selected_video_mode) = (int32_t)mode;
                            if ((mode == 7 || mode == 8) && (uint32_t)G32(VA_g_framebuffer_bytes) < 0x3e800) {
                                show_message_box((uint32_t)GADDR(VA_g_choice_selected_index + 0x5dc), 0);
                                return 0x603e9; /* not-enough-memory warning path */
                            }
                            return 0x503e9;     /* apply-resolution action */
                        }
                        if (prev_res != G8(VA_g_cfg_das2_arg + 0x1be))
                            return 0x503e9;     /* cancelled, but the slider changed it */
                    }
                    break;
                case 4:                         /* Input (same reduced box) */
                    show_message_box((uint32_t)GADDR(VA_g_choice_selected_index + 0x670), 0x10);
                    break;
                }
            }
            if (r == 0)
                break;
        }
        break;
    case 2:
        if (show_message_box((uint32_t)GADDR(VA_g_mouse_speed + 0x30), 0) == 1)
            ret = 0x703e9;
        break;
    case 3:
        show_message_box((uint32_t)GADDR(VA_g_choice_selected_index + 0x5a4), 0);
        break;
    }
    return ret;
}

/* run_options_menu (canon 0x26501, 295 B, INTERACTIVE) — the in-game pause menu loop
 * (g_menu_frontend_flag=0; desc 0x71b6d flags 0x8c1) -> EAX: 0 = resume, (slot<<16)+2 = save,
 * (slot<<16)+3 = load, hi16-1 for the 0x?03e8 settings action codes; gallery (0x7d0) runs
 * show_cutscene_playback_menu(0x40) and loops. The 5 slot-list label ptrs all point at ONE
 * local names buffer (rows index base + row*0x30); the box flags come from EDX left live
 * across read_savegame_slot_names 0x21cc5 (it push/pops EDX): save=0x4f, load=0x4d. Each slot
 * box copies its new scroll offset to the sibling desc (0x71c19 <-> 0x71c69). */
uint32_t run_options_menu(uint32_t eax)
{
    (void)eax;
    for (;;) {
        G32(VA_g_menu_frontend_flag) = 0;
        uint32_t raw = show_message_box((uint32_t)GADDR(VA_g_vol_music + 0x15), 0x8c1);
        uint32_t m = raw & 0x1fff;
        if (m < 3) {
            if (m == 2) {                       /* Load (desc 0x71c5d, flags 0x4d) */
                uint8_t names[0x1b0];
                for (uint32_t i = 0; i < 5; i++)
                    *(volatile uint32_t *)(GADDR(VA_g_vol_music + 0x11d) + i * 0xc) = (uint32_t)(uintptr_t)names;
                read_savegame_slot_names((uint32_t)(uintptr_t)names);
                uint32_t r = show_message_box((uint32_t)GADDR(VA_g_vol_music + 0x105), 0x4d);
                G16(VA_g_vol_music + 0xc1) = G16(VA_g_vol_music + 0x111);
                if (r != 0)
                    return (r << 16) + 3;
            }
        } else if (m == 3) {                    /* Save (desc 0x71c0d, flags 0x4f) */
            uint8_t names[0x1b0];
            for (uint32_t i = 0; i < 5; i++)
                *(volatile uint32_t *)(GADDR(VA_g_vol_music + 0xcd) + i * 0xc) = (uint32_t)(uintptr_t)names;
            read_savegame_slot_names((uint32_t)(uintptr_t)names);
            uint32_t r = show_message_box((uint32_t)GADDR(VA_g_vol_music + 0xb5), 0x4f);
            G16(VA_g_vol_music + 0x111) = G16(VA_g_vol_music + 0xc1);
            if (r != 0)
                return (r << 16) + 2;
        } else if (m == 0x3e8) {                /* settings action code channel */
            uint32_t hi = raw >> 16;
            if (hi != 0)
                return hi - 1;
        } else if (m == 0x7d0) {                /* cutscene gallery */
            show_cutscene_playback_menu(0x40);
        }
        if (raw == 0)
            return 0;
    }
}

/* run_main_menu (canon 0x26628, 196 B, INTERACTIVE) — the front-end main menu loop
 * (g_menu_frontend_flag=1; desc 0x71bbd, flags 0x201 first show then 1) -> EAX: 0xa = play,
 * (slot<<16)+3 = load slot (desc 0x71c5d, flags 0xd — EDX live across 0x21cc5), hi16-1 for the
 * 0x?03e8 action codes, 0 = quit (raw result 0). */
uint32_t run_main_menu(uint32_t eax)
{
    (void)eax;
    uint32_t fl = 0x201;
    for (;;) {
        G32(VA_g_menu_frontend_flag) = 1;
        uint32_t raw = show_message_box((uint32_t)GADDR(VA_g_vol_music + 0x65), fl);
        fl = 1;
        uint32_t m = raw & 0x1fff;
        if (m <= 1)
            return 0xa;                         /* play (Esc/row 1 both start the game) */
        if (m == 2) {                           /* Load */
            uint8_t names[0x1b0];
            for (uint32_t i = 0; i < 5; i++)
                *(volatile uint32_t *)(GADDR(VA_g_vol_music + 0x11d) + i * 0xc) = (uint32_t)(uintptr_t)names;
            read_savegame_slot_names((uint32_t)(uintptr_t)names);
            uint32_t r = show_message_box((uint32_t)GADDR(VA_g_vol_music + 0x105), 0xd);
            G16(VA_g_vol_music + 0xc1) = G16(VA_g_vol_music + 0x111);
            if (r != 0)
                return (r << 16) + 3;
        } else if (m == 0x3e8) {
            uint32_t hi = raw >> 16;
            if (hi != 0)
                return hi - 1;
        }
        if (raw == 0)
            return 0;
    }
}
