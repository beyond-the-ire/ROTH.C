/* lift_inventory.c — verified-C lift of the ROTH `inventory` subsystem.
 *
 * Lift lens: docs/reference/lift/inventory.md.
 * Per-subsystem TU (recomp.md §4.6): includes common.h, defines the lifts declared in engine.h.
 *
 * Cluster order (bottom-up, increasing cross-subsystem coupling):
 *   A. item data model    — carried-array give/remove/query + template/record walkers (oracle).
 *   B. cursor-held         — pick-up/use/combine/swap held-item state machine (dbase100 bridges; in-game).
 *   C. rendering           — panel/grid/icon/label compositors (das_assets/text_font/blit bridges; in-game).
 *   D. tabs/lifecycle      — tab filter + per-frame screen state machine (in-game).
 *
 * Carried inventory: g_inventory_slots 0x80c30 = 256 * 4B {item_id:u16 @+0, quantity:u16 @+2};
 * g_inventory_count 0x80c2c. Item templates are DBASE100 records resolved via the inventory index
 * table at [0x81e20] (entry list) / [0x81e1c] (record base); see docs/reference/ROTH_inventory_notes.md.
 *
 * Already lifted in renderer.c (reached via engine.h): find_free_inventory_slot 0x1ce43,
 * stack_onto_inventory_slot 0x1ce14, reset_inventory 0x1c57e, begin_item_pickup_lock 0x1622d,
 * move_cursor_entry_clamped 0x1bb12, clear_dual_array_80afc 0x1c59e, clear_list_field30 0x4b378,
 * copy_word_90bcc_to_8532a 0x2ab21.
 */
#include <stdint.h>
#include <string.h>
#include "common.h"

/* ======================================================================================
 * A — Item data model + carried-inventory state (pure, oracle-able core).
 * ====================================================================================== */

/* reset_item_pickup_lock (0x18003): clears the pickup-lock control dwords [0x7fd84] + [0x71148],
 * then tail-calls clear_list_field30 0x4b378 (already lifted) which frees the pickup-anim list head
 * g_list_91864. Void. */
void reset_item_pickup_lock(void)
{
    G32(VA_g_item_pickup_flags) = 0;
    G32(VA_g_font_descriptor + 0x236) = 0;
    clear_list_field30();                    /* jmp 0x4b378 (tail) */
}

/* is_item_id_pickable (0x1dd50): EAX = item id. Looks the item up in the DBASE100 inventory index
 * (entry list [0x81e20], record base [0x81e1c]; entry count [base+0x10]). Returns 1 iff the matching
 * template's type/flags byte ([rec+5], = (dword[rec+4]>>8)&0xf) has neither bit 0x80 set (al test)
 * nor category 2; else 0. Item ids below 0x200 are never pickable. Pure (reads obj3 + the staged
 * index table); returns EAX. */
uint32_t is_item_id_pickable(uint32_t eax_in)
{
    int32_t ebx = (int32_t)eax_in;
    uint32_t eax = (uint32_t)G32(VA_g_dbase100_inventory_table);           /* entry-list head (stored ptr) */
    if ((uint32_t)ebx < 0x200) return 0;             /* jb 0x1dda1 (unsigned) */
    int32_t ecx = 0;
    for (;;) {
        uint32_t edx = (uint32_t)G32(VA_g_dbase100_base);       /* record base (stored ptr) */
        if (ecx >= *(int32_t *)(uintptr_t)(edx + 0x10)) return 0;  /* jge (signed) */
        eax += 4;
        if (*(uint32_t *)(uintptr_t)eax == 0) { ecx++; continue; } /* empty entry */
        edx += *(uint32_t *)(uintptr_t)eax;          /* record = base + entry offset */
        int32_t esi = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)(edx + 2);  /* record id */
        if (esi != ebx) { ecx++; continue; }
        eax = *(uint32_t *)(uintptr_t)(edx + 4);     /* type/flags dword */
        if (eax & 0x80) return 0;                    /* test al,0x80 */
        eax = (uint32_t)((int32_t)eax >> 8);         /* sar eax,8 */
        eax &= 0xf;
        if (eax == 2) return 0;                      /* category 2 not pickable */
        return 1;
    }
}

/* resolve_record_conditional_op2 (0x1a028): EAX = a DBASE100 record. If the record's [rec+4] flag bit
 * 0x4 is clear, returns 0. Otherwise walks the record's trigger-block list (rec+0x14): block header
 * = [p], size(bytes) = header&0xffff, opcode = (s32)header>>0x18. For a type-2 block it evaluates the
 * block condition via eval_dialogue_record_condition_with_cleanup 0x1db89 (dbase100 BRIDGE; EAX=block
 * ptr, EDX=0, ESI=[block+4]); on a true result returns ESI&0xffffff, else advances by (size&~3) bytes
 * and continues. Returns 0 when the list ends (size==0). Bridges dbase100; returns EAX. */
uint32_t resolve_record_conditional_op2(uint32_t eax_in)
{
    uint32_t eax = eax_in;
    if (!(*(uint8_t *)(uintptr_t)(eax + 4) & 4)) return 0;    /* test byte[eax+4],4 ; je ret0 */
    uint32_t ebx = eax + 0x14;
    for (;;) {
        eax = *(uint32_t *)(uintptr_t)ebx;                    /* block header */
        uint32_t ecx = eax & 0xffff;                          /* size in bytes (low 16) */
        if (ecx == 0) return 0;                               /* end of list */
        if (((int32_t)eax >> 0x18) == 2) {                    /* opcode == 2 (conditional) */
            uint32_t esi = *(uint32_t *)(uintptr_t)(ebx + 4);
            /* re-pointed: eval_dialogue_record_condition_with_cleanup 0x1db89 [L] direct-C
             * (EAX=block ptr, EDX=0). ESI is CALLEE-PRESERVED end-to-end (0x1d430 execute_dbase100_chain
             * push/pops esi, 0x1daea/0x1db5e don't touch it), so we keep the C `esi` and mask its low 24 on
             * a true result — the lifted proto's ebx_io/ecx_io out-params are unused here. t_resolve_record_
             * conditional_op2 now stages REAL dbase100 blocks: cond_true = a header+REC(0x0a) terminate chain
             * (eval->1), cond_false = a count-0 block (eval->0) — the real interpreter runs both sides. */
            uint32_t r = eval_dialogue_record_condition_with_cleanup(ebx, 0,
                                                                            (uint32_t *)0, (uint32_t *)0);
            if (r != 0)                                       /* condition true */
                return esi & 0xffffff;                        /* esi callee-preserved; masked low 24 */
        }
        ebx += (uint32_t)((int32_t)ecx >> 2) << 2;            /* advance by size & ~3 */
    }
}

/* tick_item_pickup_lock (0x15efe): per-frame driver for the "item flies into the pack" pickup animation.
 * If the lock isn't armed ([0x7fd84]&1 == 0) it does nothing. Otherwise it recomputes the flying item's
 * world anchor from the view angle (sincos table 0x72080) and advances the lock timer [0x7fda0] by the
 * frame step [0x85324]; at >= 0x1c frames it disarms the lock (clears [0x7fda0]/[0x7fd84]/[0x71144]),
 * else it linearly interpolates the item's screen X/Y/Z ([0x7114c]/[0x7114e]/[0x71156]) toward the
 * cached target ([0x7fd88..0x7fd9c]) by the per-frame fraction (table 0x7115c, &0x7f, /128). Void
 * (global write-set). Pure obj3 (no bridges). */
void tick_item_pickup_lock(void)
{
    if (!(G8(VA_g_item_pickup_flags) & 1)) return;                           /* test byte[0x7fd84],1 ; je end */

    int32_t ecx = 0x7d - (int32_t)(int16_t)G16(VA_g_player_angle);
    int32_t s1 = (int32_t)(int16_t)G16(VA_g_sincos_table + (uint32_t)((uint32_t)ecx & 0x1ff) * 2);
    int32_t ebx = (int32_t)(int16_t)G16(VA_g_player_x) + (s1 >> 8);
    int32_t s2 = (int32_t)(int16_t)G16(VA_g_sincos_table + (uint32_t)(((uint32_t)ecx + 0x80) & 0x1ff) * 2);
    int32_t esi = (int32_t)(int16_t)G16(VA_g_player_y) + (s2 >> 8);
    int32_t z   = (int32_t)(int16_t)G16(VA_g_player_z) + 0x50;
    G32(VA_g_item_pickup_flags + 0x18) = z;
    int32_t step = (int32_t)G32(VA_g_frame_time_scale);
    G32(VA_g_item_pickup_flags + 0x10) = ebx;
    G32(VA_g_item_pickup_flags + 0x1c) = (int32_t)((uint32_t)G32(VA_g_item_pickup_flags + 0x1c) + (uint32_t)step);
    G32(VA_g_item_pickup_flags + 0x14) = esi;
    if ((int32_t)G32(VA_g_item_pickup_flags + 0x1c) < 0x1c) {                       /* jl 0x15faa (interpolate) */
        int32_t t = (int32_t)G32(VA_g_item_pickup_flags + 0x1c);
        int32_t frac = (int32_t)G32((VA_g_font_descriptor + 0x24a) + (uint32_t)t * 4) & 0x7f;
        int32_t ax = (int32_t)((uint32_t)((int32_t)G32(VA_g_item_pickup_flags + 0x4) - ebx) * (uint32_t)frac);
        G16(VA_g_font_descriptor + 0x23a) = (uint16_t)(ax / 128 + ebx);
        int32_t ay = (int32_t)((uint32_t)((int32_t)G32(VA_g_item_pickup_flags + 0x8) - esi) * (uint32_t)frac);
        G16(VA_g_font_descriptor + 0x23c) = (uint16_t)(ay / 128 + esi);
        int32_t az = (int32_t)((uint32_t)((int32_t)G32(VA_g_item_pickup_flags + 0xc) - (int32_t)G32(VA_g_item_pickup_flags + 0x18)) * (uint32_t)frac);
        G16(VA_g_font_descriptor + 0x244) = (uint16_t)(az / 128 + (int32_t)G32(VA_g_item_pickup_flags + 0x18));
        if (G8(VA_g_item_pickup_flags) & 4) {                                /* test byte[0x7fd84],4 */
            G8(VA_g_font_descriptor + 0x242) = 0x80;
            G8(VA_g_font_descriptor + 0x243) = 0x10;
        }
    } else {                                                 /* >= 0x1c : disarm */
        G32(VA_g_item_pickup_flags + 0x1c) = 0;
        G32(VA_g_item_pickup_flags) = 0;
        G32(VA_g_font_descriptor + 0x232) = 0;
    }
}

/* init_inventory_item_object (0x18598): EAX = item id, EDX = output object ptr (0xc bytes). Zeroes the
 * object (mem_fill), stamps [obj+8]=0x10, then — only for STACKABLE templates ([rec+4]&0x20) — walks the
 * template's trigger blocks (rec+0x14) for the first render block (opcode 6) and copies its sub-fields
 * into the object: sub-op 0x15->word[+6], 0x16->byte[+4], 0x17->word[+0], 0x19->word[+2], 0x2e->byte[+5],
 * 0x2f->byte[+8], 0x30->byte[+9]. Returns the template id ([rec+2]) on success, 0 if not stackable / no
 * blocks. Bridges mem_fill (math_util, lifted). Returns EAX. */
uint32_t init_inventory_item_object(uint32_t eax_in, uint32_t edx_in)
{
    uint32_t ecx = edx_in;                                    /* output object ptr */
    uint32_t tbl = (uint32_t)G32(VA_g_dbase100_inventory_table);
    uint32_t esi = (uint32_t)G32(VA_g_dbase100_base) + *(uint32_t *)(uintptr_t)(tbl + eax_in * 4);  /* template record */
    mem_fill((void *)(uintptr_t)ecx, 0, 0xc);          /* mem_fill(dst=ecx, val=0, len=0xc) */
    *(uint8_t *)(uintptr_t)(ecx + 8) = 0x10;

    int32_t ebx;
    if (!(*(uint8_t *)(uintptr_t)(esi + 4) & 0x20)) {         /* not stackable */
        ebx = 0;
        return (uint32_t)ebx;
    }
    uint32_t edx = esi + 0x14;                                /* trigger-block list */
    for (;;) {
        uint32_t hdr = *(uint32_t *)(uintptr_t)edx;
        ebx = (int32_t)(hdr & 0xffffff);                      /* block size (bytes) */
        if (ebx == 0) return (uint32_t)ebx;                   /* end of list -> 0 */
        int32_t opcode = (int32_t)hdr >> 0x18;
        int32_t dwords = ebx >> 2;
        if (opcode != 6) {                                    /* not a render block: advance */
            edx += (uint32_t)(dwords << 2);
            continue;
        }
        /* render block: copy sub-fields */
        int32_t limit = dwords - 1;
        edx += 4;                                             /* skip header */
        for (int32_t i = 0; i < limit; i++) {
            uint32_t e = *(uint32_t *)(uintptr_t)edx;
            edx += 4;
            int32_t sub = ((int32_t)e >> 0x18) & 0x7f;
            uint32_t val = e & 0xffffff;
            switch (sub) {
            case 0x15: *(uint16_t *)(uintptr_t)(ecx + 6) = (uint16_t)val; break;
            case 0x16: *(uint8_t  *)(uintptr_t)(ecx + 4) = (uint8_t)val;  break;
            case 0x17: *(uint16_t *)(uintptr_t)(ecx)     = (uint16_t)val; break;
            case 0x19: *(uint16_t *)(uintptr_t)(ecx + 2) = (uint16_t)val; break;
            case 0x2e: *(uint8_t  *)(uintptr_t)(ecx + 5) = (uint8_t)val;  break;
            case 0x2f: *(uint8_t  *)(uintptr_t)(ecx + 8) = (uint8_t)val;  break;
            case 0x30: *(uint8_t  *)(uintptr_t)(ecx + 9) = (uint8_t)val;  break;
            default: break;
            }
        }
        ebx = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)(esi + 2);  /* template id */
        return (uint32_t)ebx;
    }
}

/* ----- A mid: give / remove / query (carried-array bookkeeping). Within-subsystem callees are direct
 * C calls (direct lifted C); cross-subsystem callees are bridged via call_orig:
 *   activate_weapon_item 0x184ab / rebuild_weapon_inventory_list 0x2245c  (weapon_combat)
 *   update_selected_item_icon 0x1bb4b                                      (inventory C-layer, not yet lifted). */
#define ACTIVATE_WEAPON_ITEM   0x184abu
#define REBUILD_WEAPON_LIST    0x2245cu
#define UPDATE_SELECTED_ICON   0x1bb4bu

#ifdef ROTH_STANDALONE
/* M3: route this TU's inline io-block bridges into their verified lifted bodies. Dispatch on the
 * canon VA; value-returning targets thread io->eax (the block's `return io.eax` tail reads it).
 * Any VA not listed still stops fail-loud. */
static void inv_standalone_route(regs_t *io)
{
    uint32_t va = io->va - OBJ_DELTA;
    switch (va) {
    case 0x184abu: activate_weapon_item(io->eax, io->edx); return;   /* activate_weapon_item(EAX=slot,EDX=id) */
    case 0x1e8ccu: io->eax = read_next_dialogue_line(io->eax, io->edx, io->ebx, io->ecx); return;  /* (dest,maxlen,voice_off,flag) */
    case 0x1f818u: io->eax = resolve_dbase100_text(io->eax, io->edx, io->ebx, io->ecx); return;    /* (dest,maxlen,index,flag) */
    case 0x1f859u: queue_timed_message_color((const char *)(uintptr_t)io->eax, (uint8_t)io->edx); return;
    case 0x1dc73u: io->eax = eval_dialogue_record_by_id(io->eax); return;
    case 0x1db5eu: finish_dialogue_record_eval(); return;
    case 0x1869bu: io->eax = load_das_cache_resource(io->eax, io->edx); return;   /* (res, handle passthrough) */
    case 0x18e68u: blit_reloc_das_image(io->eax, io->edx, io->ebx); return;       /* (dst, das id, dest stride) */
    default: roth_unreachable(va);   /* inventory bridge — un-routed target stays fail-loud */
    }
}
#endif

static void inv_bridge_void(uint32_t va)
{
    regs_t io; memset(&io, 0, sizeof io); io.va = va + OBJ_DELTA;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    switch (va) {                  /* M3 routes (all void/no-arg lifted bodies) */
    case UPDATE_SELECTED_ICON:     update_selected_item_icon();     return;
    case REBUILD_WEAPON_LIST:      rebuild_weapon_inventory_list(); return;
    case 0x1bf7bu: /* REFRESH_INVENTORY_PANEL (defined below) */ refresh_inventory_panel(); return;
    case 0x1be8eu: /* RESET_WEAPON_HUD (defined below) */        reset_weapon_hud();        return;
    default: break;
    }
    roth_unreachable(va);   /* inventory UI-refresh bridge — in-game inventory tier */
#endif
}

/* remove_inventory_item (0x1ce6b): EAX = slot ptr, EDX = template record (or 0). Internal slot remove:
 * if the slot is the equipped weapon ([0x81038]) unequip (activate_weapon_item(0,0) bridge); if it's the
 * selected item ([0x81044]) clear+refresh icon (update_selected_item_icon bridge); if it's the active
 * weapon's ammo ([0x811e4]) flag + dec the ammo display [0x83e6c]. Then for a stackable template with
 * quantity>1 decrement the quantity, else clear the slot id + dec the inventory count [0x80c2c]. Void. */
void remove_inventory_item(uint32_t eax_in, uint32_t edx_in)
{
    uint32_t ebx = eax_in;                                   /* slot ptr */
    uint32_t ecx = edx_in;                                   /* template */
    if (eax_in == (uint32_t)G32(VA_g_selected_item_secondary)) {                  /* equipped weapon -> unequip */
        regs_t io; memset(&io, 0, sizeof io);
        io.va = ACTIVATE_WEAPON_ITEM + OBJ_DELTA; io.eax = 0; io.edx = 0;
#ifndef ROTH_STANDALONE
        call_orig(&io);
#else
        activate_weapon_item(io.eax, io.edx);   /* M3 route: unequip = activate(0,0) */
#endif
    }
    if (ebx == (uint32_t)G32(VA_g_selected_item_primary)) {                     /* selected item -> clear + refresh */
        G32(VA_g_selected_item_primary) = 0;
        inv_bridge_void(UPDATE_SELECTED_ICON);
    }
    int32_t id = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)ebx;
    if (id == (int32_t)G32(VA_g_active_weapon_ammo_id)) {                       /* active weapon ammo */
        G32(VA_g_active_weapon_ammo_cap + 0xc) = 1;
        G32(VA_g_active_weapon_ammo) = (int32_t)((uint32_t)G32(VA_g_active_weapon_ammo) - 1);
    }
    if (ecx != 0 && (*(uint8_t *)(uintptr_t)(ecx + 5) & 0x20)) {   /* stackable */
        int32_t q = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)(ebx + 2);
        if (q > 1) {                                          /* jle -> clear */
            (*(uint16_t *)(uintptr_t)(ebx + 2))--;            /* dec quantity */
            return;
        }
    }
    *(uint16_t *)(uintptr_t)ebx = 0;                          /* clear slot id */
    G32(VA_g_inventory_count) = (int32_t)((uint32_t)G32(VA_g_inventory_count) - 1);     /* dec count */
}

/* remove_item (0x1d077): EAX = item id -> EAX -1 (found+removed) / 0. Removes one matching item: checks
 * the selected ([0x81044]) then equipped ([0x81038]) slots, then scans the carried array; on the first
 * match calls remove_inventory_item(slot, template) and returns -1. No match -> rebuild_weapon_inventory_list
 * (bridge) + return 0. */
uint32_t remove_item(uint32_t eax_in)
{
    uint32_t ebx = eax_in;                                   /* item id (low 16 compared) */
    int32_t ecx = (int32_t)G32(VA_g_inventory_count);                     /* count */
    uint32_t edx = 0;                                        /* template */
    if (ecx == 0) { inv_bridge_void(REBUILD_WEAPON_LIST); return 0; }    /* empty */
    if ((uint16_t)ebx != 0) {                                /* id != 0 -> resolve template */
        int32_t esi = (int32_t)(int16_t)(uint16_t)ebx;
        uint32_t off = *(uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_dbase100_inventory_table) + (uint32_t)esi * 4);
        if (off != 0) edx = (uint32_t)G32(VA_g_dbase100_base) + off;
    }
    uint32_t sel = (uint32_t)G32(VA_g_selected_item_primary);
    if (sel != 0 && (uint16_t)ebx == *(uint16_t *)(uintptr_t)sel) {
        remove_inventory_item(sel, edx);
        return (uint32_t)-1;
    }
    uint32_t equ = (uint32_t)G32(VA_g_selected_item_secondary);
    if (equ != 0 && (uint16_t)ebx == *(uint16_t *)(uintptr_t)equ) {
        remove_inventory_item(equ, edx);
        return (uint32_t)-1;
    }
    uint32_t eax = 0x80c30u + OBJ_DELTA;                     /* carried array base */
    int32_t edi = (int32_t)(int16_t)(uint16_t)ebx;
    for (;;) {
        int32_t esi = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)eax;
        if (esi != 0) {                                      /* non-empty slot */
            if (esi == edi) {
                remove_inventory_item(eax, edx);
                return (uint32_t)-1;
            }
            ecx--;
        }
        eax += 4;
        if (ecx == 0) break;
    }
    inv_bridge_void(REBUILD_WEAPON_LIST);
    return 0;
}

/* consume_held_item (0x1d0fd): EAX = slot ptr -> EAX 1/0. Removes the slot (remove_inventory_item) +
 * rebuild_weapon_inventory_list (bridge); resolves the template from the slot's item id first. Returns 0
 * if the slot is null or the inventory is empty. */
uint32_t consume_held_item(uint32_t eax_in)
{
    if (eax_in == 0) return 0;
    if (G32(VA_g_inventory_count) == 0) return 0;
    int32_t id = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)eax_in;
    uint32_t ebx = 0;                                        /* template */
    if (id != 0) {
        uint32_t off = *(uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_dbase100_inventory_table) + (uint32_t)id * 4);
        if (off != 0) ebx = (uint32_t)G32(VA_g_dbase100_base) + off;
    }
    remove_inventory_item(eax_in, ebx);
    inv_bridge_void(REBUILD_WEAPON_LIST);
    return 1;
}

/* give_item_by_dbase_id (0x1dcef): EAX = dbase item id, EDX = context ptr -> EAX. Resolves the id to a
 * 1-based inventory entry index (walks the index table, matching record id with the same pickable filter
 * as is_item_id_pickable) and tail-calls give_item(index, context). Returns 0 if not found. */
uint32_t give_item_by_dbase_id(uint32_t eax_in, uint32_t edx_in)
{
    uint32_t ecx = eax_in;                                   /* dbase id */
    uint32_t esi = edx_in;                                   /* context */
    uint32_t eax = (uint32_t)G32(VA_g_dbase100_inventory_table);                   /* entry list head */
    if ((int32_t)ecx < 0x200) return 0;                      /* jl (signed) */
    int32_t ebx = 0;
    for (;;) {
        uint32_t edx = (uint32_t)G32(VA_g_dbase100_base);
        if (ebx >= *(int32_t *)(uintptr_t)(edx + 0x10)) return 0;
        eax += 4;
        if (*(uint32_t *)(uintptr_t)eax == 0) { ebx++; continue; }
        edx += *(uint32_t *)(uintptr_t)eax;
        int32_t edi = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)(edx + 2);
        if ((int32_t)ecx != edi) { ebx++; continue; }
        edx = *(uint32_t *)(uintptr_t)(edx + 4);             /* flags */
        if (edx & 0x80) { ebx++; continue; }                 /* test dl,0x80 */
        if ((((int32_t)edx >> 8) & 0xf) == 2) { ebx++; continue; }
        ebx++;                                               /* 1-based index */
        return give_item((uint32_t)(int32_t)(int16_t)ebx, esi);
    }
}

/* give_item (0x1cedc): EAX = inventory entry index (di), EDX = context record (or 0) -> EAX = id given / 0.
 * The carried-array give path. Resolves the item template, gives a stackable item by bumping an existing
 * stack or finding a free slot; for a weapon ([rec+4]&0x10) equips it (activate_weapon_item bridge) else
 * selects it (update_selected_item_icon bridge); arms the pickup-fly animation (begin_item_pickup_lock [L])
 * from the context drop coords; rebuilds the weapon list for weapons (bridge). di==0 -> resolve via
 * give_item_by_dbase_id([ctx+4]). Returns 0 if the inventory is full / id out of range / no free slot. */
uint32_t give_item(uint32_t eax_in, uint32_t edx_in)
{
    uint32_t edi = eax_in;                                   /* entry index (di = low 16) */
    uint32_t ecx = edx_in;                                   /* context ptr */
    uint8_t  attrbuf[0x60];                                  /* [ebp-0x60] weapon-attr scratch */
    int32_t  quantity = 0;                                   /* [ebp-4] */
    uint32_t esi = 0;                                        /* template record (0 = none) */
    uint32_t ebx = 0;                                        /* the slot (existing or new) */

    if (G32(VA_g_inventory_count) >= 0x100) return 0;                     /* inventory full */
    uint32_t base = (uint32_t)G32(VA_g_dbase100_base);                  /* [ebp-8] record base */
    int32_t di = (int32_t)(int16_t)(uint16_t)edi;
    if (di > *(int32_t *)(uintptr_t)(base + 0x10)) return 0; /* jg -> id out of range */
    if ((uint16_t)edi == 0)                                  /* di==0: resolve by dbase id */
        return give_item_by_dbase_id(*(uint16_t *)(uintptr_t)(ecx + 4), ecx);

    {                                                        /* 0x1cf2d: resolve template + stack */
        uint32_t off = *(uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_dbase100_inventory_table) + (uint32_t)di * 4);
        if (off == 0) goto L_8d;
        esi = base + off;
        if (*(uint8_t *)(uintptr_t)(esi + 4) & 0x10) {       /* WeaponAction -> parse attrs */
            apply_weapon_action_attributes((uint32_t)di, (uint32_t)(uintptr_t)attrbuf, 1);
            uint32_t v = *(uint32_t *)(attrbuf + 4);         /* [local+4] */
            if (attrbuf[0x31] & 0x80) v <<= 8;               /* infinite-ammo -> <<8 */
            quantity = (int32_t)v;
        }
        if (!(*(uint8_t *)(uintptr_t)(esi + 5) & 0x20)) goto L_8d;   /* not stackable */
        quantity = 1;
        ebx = stack_onto_inventory_slot((uint32_t)di);
        if (ebx == 0) goto L_8d;
        (*(uint16_t *)(uintptr_t)(ebx + 2))++;               /* inc existing stack quantity */
        goto L_c8;
    }

L_8d:                                                        /* 0x1cf8d */
    if (esi != 0 && (((int32_t)*(uint32_t *)(uintptr_t)(esi + 4) >> 8) & 0xf) == 2) {
        if (stack_onto_inventory_slot((uint32_t)di) != 0)
            goto L_6e;                                        /* already have a cat-2 instance */
    }
    ebx = find_free_inventory_slot();                  /* 0x1cfaf */
    if (ebx == 0) return 0;                                   /* no free slot */
    *(uint16_t *)(uintptr_t)ebx = (uint16_t)edi;              /* slot id = di */
    *(uint16_t *)(uintptr_t)(ebx + 2) = (uint16_t)quantity;

L_c8:                                                        /* 0x1cfc8 shared tail (ebx = slot) */
    if (esi != 0) {
        if (*(uint8_t *)(uintptr_t)(esi + 4) & 0x10) {       /* weapon -> equip */
            regs_t io; memset(&io, 0, sizeof io);
            io.va = ACTIVATE_WEAPON_ITEM + OBJ_DELTA;
            io.eax = ebx; io.edx = (uint32_t)(int32_t)(int16_t)*(uint16_t *)(uintptr_t)ebx;
#ifndef ROTH_STANDALONE
            call_orig(&io);
#else
            activate_weapon_item(io.eax, io.edx);   /* M3 route: equip(slot, id) */
#endif
            G32(VA_g_inspect_popup_state + 0x4) = (int32_t)((uint32_t)G32(VA_g_inspect_popup_state + 0x4) + 1);
            goto L_fd;
        } else if ((((int32_t)*(uint32_t *)(uintptr_t)(esi + 4) >> 8) & 0xf) == 2) {
            goto L_fd;                                        /* category 2 -> skip select */
        }
    }
    G32(VA_g_selected_item_primary) = (int32_t)ebx;                              /* 0x1cff2: select */
    inv_bridge_void(UPDATE_SELECTED_ICON);

L_fd:                                                        /* 0x1cffd: ammo display */
    {
        int32_t adi = (int32_t)(int16_t)(uint16_t)edi;
        if (adi == (int32_t)G32(VA_g_active_weapon_ammo_id)) {
            G32(VA_g_active_weapon_ammo_cap + 0xc) = 1;
            G32(VA_g_active_weapon_ammo) = (int32_t)((uint32_t)G32(VA_g_active_weapon_ammo) + 1);
        }
    }
    if (ecx != 0) {                                          /* 0x1d018: arm pickup-fly animation */
        uint32_t q = *(uint16_t *)(uintptr_t)(ecx + 4);      /* movzx */
        if (*(uint8_t *)(uintptr_t)(ecx + 7) & 1) q = 0;
        if (esi != 0) {
            int32_t rid = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)(esi + 2);
            if ((uint32_t)rid >= 0x200u) q = (uint32_t)rid;
        }
        if (q != 0) {
            int32_t bX = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)(ecx + 0xa) + 0x18;
            int32_t v0 = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)(ecx);
            int32_t vY = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)(ecx + 2);
            begin_item_pickup_lock((uint32_t)v0, (uint32_t)vY, (uint32_t)bX, (uint16_t)q);
        }
    }
    if (esi != 0 && (*(uint8_t *)(uintptr_t)(esi + 4) & 0x10))   /* 0x1d05f: weapon -> rebuild list */
        inv_bridge_void(REBUILD_WEAPON_LIST);
L_6e:                                                        /* 0x1d06e */
    return (uint32_t)(int32_t)(int16_t)(uint16_t)edi;
}

/* query_player_inventory (0x1ccf7): EAX = item id, EDX = flags -> EAX (count / quantity / 0/1). RESOLVES
 * GAP #1: the result is a SINGLE EAX return (= ecx), NOT a multi-register pack. flags bit0 = select
 * mode (select the first matching slot, returns 0/1 + sets [0x81044]/[0x89f60]); bit0 clear = count mode
 * (returns the number of matching slots, or — for exactly one match of a stackable item with quantity>1 —
 * that quantity). flags bit1 forces the local "skip-scan" flag (else seeded from [0x81e34]). The selected
 * ([0x81044]) and equipped ([0x81038]) fast paths short-circuit to 1 (the selected path also clears
 * [0x819bc]; the equipped path does NOT — a deliberate asymmetry). Bridges update_selected_item_icon only
 * on the select-scan match (the in-game path). */
uint32_t query_player_inventory(uint32_t eax_in, uint32_t edx_in)
{
    uint32_t ebx = eax_in;                                   /* item id (low 16) */
    uint32_t flags = edx_in;                                 /* [ebp-8] */
    uint32_t eax = 0x80c30u + OBJ_DELTA;                     /* carried array base */
    G32(VA_g_item_autoselected_flag) = 0;
    int32_t local4 = (int32_t)G32(VA_g_object_select_easy_flag);                  /* [ebp-4] */
    int32_t edx = 0;                                         /* last matching slot (count mode) */
    int32_t ecx = 0;                                         /* result */
    if (flags & 2) local4 = 1;

    if (flags & 1) {                                         /* select mode */
        uint32_t sel = (uint32_t)G32(VA_g_selected_item_primary);
        if (sel != 0 && (uint16_t)ebx == *(uint16_t *)(uintptr_t)sel) {
            G32(VA_g_held_item_slide_timer) = 0;
            return 1;
        }
        uint32_t equ = (uint32_t)G32(VA_g_selected_item_secondary);
        if (equ != 0 && (uint16_t)ebx == *(uint16_t *)(uintptr_t)equ) {
            return 1;                                        /* equipped path: NO 0x819bc clear */
        }
        if (!(local4 & 1)) {                                 /* select-scan */
            uint32_t esi = 0;
            for (;;) {
                if (esi >= (uint32_t)G32(VA_g_inventory_count)) break;
                if (*(uint16_t *)(uintptr_t)eax == 0) { eax += 4; continue; }
                if ((uint16_t)ebx == *(uint16_t *)(uintptr_t)eax) {
                    G32(VA_g_selected_item_primary) = (int32_t)eax;
                    inv_bridge_void(UPDATE_SELECTED_ICON);   /* in-game path */
                    G32(VA_g_item_autoselected_flag) = 1;
                    return 0;
                }
                esi++; eax += 4;
            }
        }
    } else {                                                /* count mode */
        int32_t idx = 0;
        for (;;) {
            if ((uint32_t)idx >= (uint32_t)G32(VA_g_inventory_count)) break;
            if (*(uint16_t *)(uintptr_t)eax == 0) { eax += 4; continue; }
            if ((uint16_t)ebx == *(uint16_t *)(uintptr_t)eax) { edx = (int32_t)eax; ecx++; }
            idx++; eax += 4;
        }
    }

    /* 0x1cddc shared tail: single match of a stackable item -> return its quantity */
    if (ecx == 1 && edx != 0) {
        int32_t qty = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)(edx + 2);
        if (qty > ecx) {
            uint32_t off = *(uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_dbase100_inventory_table) +
                              (uint32_t)(int32_t)(int16_t)*(uint16_t *)(uintptr_t)edx * 4);
            if (off != 0) {
                uint32_t base = (uint32_t)G32(VA_g_dbase100_base);
                if (*(uint8_t *)(uintptr_t)(base + off + 5) & 0x20)
                    ecx = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)(edx + 2);
            }
        }
    }
    return (uint32_t)ecx;
}

/* das_assets / dialogue_ui bridges used by the C (rendering) layer. */
#define POOL_FREE_HANDLE          0x360b3u
#define LOAD_DAS_CACHE_RESOURCE   0x1869bu
#define NODE_HAS_AVAILABLE_CHOICE 0x1fa91u

/* ======================================================================================
 * B — Cursor-held interaction (pick-up / use / combine / swap).
 * ====================================================================================== */

/* resolve_item_use_action (0x18060): the OnUse (0x0B) recipe matcher. EAX = item template record,
 * EDX = target id (0 = use-on-self, else the combine-partner item id), EBX = &out (an effect-command
 * dword, written). RESOLVES GAP #2 (output registers): the result is a SINGLE packed EAX
 * return = (first << 16) | second, where `second`/esi is the most-recent sign-flagged (sub-op 0xb4)
 * result item and `first` is the prior one; the EBX-pointed buffer receives the effect command-record
 * (sub-op 0x35 unsigned). Per OnUse block: sub-op 0x34 unsigned = the target this recipe matches, 0xb4 =
 * a result item, 0x35 = the effect command. Returns the packed result when target matches and a result
 * exists; -1 when target matches with only an effect (*out != 0); 0 when no recipe matches / not
 * combinable ([rec+5]&0x40 clear). Pure given staged templates -> oracle. */
uint32_t resolve_item_use_action(uint32_t eax_in, uint32_t edx_in, uint32_t ebx_out)
{
    uint32_t eax = eax_in;
    int32_t  target = (int32_t)edx_in;                      /* [ebp-0x1c] */
    *(uint32_t *)(uintptr_t)ebx_out = 0;                    /* [ebx] = 0 */
    if (eax == 0) return 0;
    if (!(*(uint8_t *)(uintptr_t)(eax + 5) & 0x40)) return 0;   /* not usable/combinable */
    eax += 0x14;                                            /* trigger-block list */
    for (;;) {
        uint32_t hdr = *(uint32_t *)(uintptr_t)eax;
        uint32_t size = hdr & 0xffffff;
        if (size == 0) return 0;
        if ((hdr >> 0x18) != 0xb) {                         /* not an OnUse block -> skip */
            eax += (size >> 2) << 2;
            continue;
        }
        uint32_t ecx = eax + 4;                             /* sub-entries */
        uint32_t esi = 0;                                   /* latest result item ("second") */
        uint32_t first = 0;                                 /* [ebp-0x10] (prior result) */
        int32_t  match_target = 0;                          /* [ebp-0x14] */
        *(uint32_t *)(uintptr_t)ebx_out = 0;                /* [ebx] = 0 (per block) */
        uint32_t limit = (size >> 2) - 1;
        for (uint32_t i = 0; i < limit; i++) {
            uint32_t e = *(uint32_t *)(uintptr_t)ecx;
            ecx += 4;
            uint32_t subraw = e >> 0x18;                    /* [ebp-8] */
            uint32_t sub = subraw & 0x7f;
            uint32_t val = e & 0xffffff;
            if (sub == 0x34) {
                if (subraw & 0x80) { first = esi; esi = val; }   /* 0xb4 -> result item */
                else match_target = (int32_t)val;          /* 0x34 -> target match value */
            } else if (sub == 0x35) {
                if (!(subraw & 0x80)) *(uint32_t *)(uintptr_t)ebx_out = val;  /* 0x35 -> effect cmd */
            }
        }
        if (match_target != target) {                       /* 0x18110: target mismatch -> next block */
            eax += (size >> 2) << 2;
            continue;
        }
        if (esi != 0) return (first << 16) | esi;           /* matched + result -> packed */
        if (*(uint32_t *)(uintptr_t)ebx_out != 0) return (uint32_t)-1;   /* effect-only */
        eax += (size >> 2) << 2;                            /* 0x18132: keep scanning */
    }
}

/* dbase100 / dialogue_ui / crt bridges used by use/combine (all leave the inventory subsystem). */
#define READ_NEXT_DIALOGUE_LINE   0x1e8ccu
#define RESOLVE_DBASE100_TEXT     0x1f818u
#define EVAL_DIALOGUE_RECORD      0x1dc73u
#define FINISH_DIALOGUE_EVAL      0x1db5eu
#define QUEUE_TIMED_MESSAGE       0x1f859u
#define REFRESH_INVENTORY_PANEL   0x1bf7bu
#define CRT_SPRINTF               0x27c53u
#define GET_DBASE100_ENTRY        0x18147u

/* bridge sprintf(out, fmt, name) — 3 cdecl stack args (caller-cleaned, ret N-safe via the trampoline). */
static void io_sprintf_msg(uint8_t *out, uint8_t *fmt, uint8_t *name)
{
    regs_t io; memset(&io, 0, sizeof io); io.va = CRT_SPRINTF + OBJ_DELTA;
    io.nstack = 3;
    io.stack[0] = (uint32_t)(uintptr_t)out;
    io.stack[1] = (uint32_t)(uintptr_t)fmt;
    io.stack[2] = (uint32_t)(uintptr_t)name;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    roth_sprintf((char *)out, (const char *)fmt, name);   /* item-message sprintf(out,fmt,name) */
#endif
}

/* bridge sprintf(out, fmt, name1, name2) — 4 cdecl stack args (combine's two-item message). */
static void io_sprintf_msg2(uint8_t *out, uint8_t *fmt, uint8_t *n1, uint8_t *n2)
{
    regs_t io; memset(&io, 0, sizeof io); io.va = CRT_SPRINTF + OBJ_DELTA;
    io.nstack = 4;
    io.stack[0] = (uint32_t)(uintptr_t)out;
    io.stack[1] = (uint32_t)(uintptr_t)fmt;
    io.stack[2] = (uint32_t)(uintptr_t)n1;
    io.stack[3] = (uint32_t)(uintptr_t)n2;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    roth_sprintf((char *)out, (const char *)fmt, n1, n2);   /* combine two-item sprintf(out,fmt,n1,n2) */
#endif
}

/* swap_inventory_entries (0x1b007): EAX = cursor entry A, EDX = cursor entry B. Swaps the two
 * cursor-entry records (and the underlying carried-slot id/quantity words they point at via [+4]),
 * re-points any of the held/selected/equipped trackers (0x81030/0x8103c/0x81048) that referenced A or B
 * to the other, refreshes each entry's cached id ([+8]), and rebuilds the weapon list. Void.
 * NOTE (resolves GAP #6): the trailing `jmp 0x18747` is the SHARED register-restore epilogue of
 * load_das_cache_resource (leave + pop edi/esi/ecx/ebx + ret), NOT a DAS call — the flow_succ edge into
 * das_assets is a shared-epilogue artifact, so swap is pure data + oracle-able (rebuild bridge stubbed). */
void swap_inventory_entries(uint32_t eax, uint32_t edx)
{
    uint32_t b0  = *(uint32_t *)(uintptr_t)edx;             /* [edx]   */
    uint32_t a0  = *(uint32_t *)(uintptr_t)eax;             /* [eax]   */
    uint32_t ecx = *(uint32_t *)(uintptr_t)(eax + 4);       /* A slot ptr */
    uint32_t ebx = *(uint32_t *)(uintptr_t)(edx + 4);       /* B slot ptr */
    if (eax == (uint32_t)G32(VA_g_left_hand_item)) {
        G32(VA_g_left_hand_item) = (int32_t)edx;
        G32(VA_g_selected_item_secondary) = (int32_t)*(uint32_t *)(uintptr_t)(edx + 4);
    }
    if (eax == (uint32_t)G32(VA_g_right_hand_item)) {
        G32(VA_g_right_hand_item) = (int32_t)edx;
        G32(VA_g_selected_item_primary) = (int32_t)*(uint32_t *)(uintptr_t)(edx + 4);
    }
    if (edx == (uint32_t)G32(VA_g_selected_item_primary + 0x4)) {
        G32(VA_g_selected_item_primary + 0x4) = (int32_t)eax;
        G32(VA_g_selected_item_primary + 0x8) = (int32_t)*(uint32_t *)(uintptr_t)(eax + 4);
    } else if (eax == (uint32_t)G32(VA_g_selected_item_primary + 0x4)) {
        G32(VA_g_selected_item_primary + 0x4) = (int32_t)edx;
        G32(VA_g_selected_item_primary + 0x8) = (int32_t)*(uint32_t *)(uintptr_t)(edx + 4);
    }
    *(uint32_t *)(uintptr_t)eax = b0;                       /* swap [+0] */
    *(uint32_t *)(uintptr_t)edx = a0;
    uint16_t di = *(uint16_t *)(uintptr_t)ecx;              /* swap slot id words */
    uint16_t si = *(uint16_t *)(uintptr_t)ebx;
    *(uint16_t *)(uintptr_t)ebx = di;
    *(uint16_t *)(uintptr_t)ecx = si;
    di = *(uint16_t *)(uintptr_t)(ecx + 2);                 /* swap slot quantity words */
    si = *(uint16_t *)(uintptr_t)(ebx + 2);
    *(uint16_t *)(uintptr_t)(ebx + 2) = di;
    *(uint16_t *)(uintptr_t)(ecx + 2) = si;
    *(int32_t *)(uintptr_t)(eax + 8) =                       /* refresh cached id [+8] = (s16)slot[0] */
        (int32_t)(int16_t)*(uint16_t *)(uintptr_t)*(uint32_t *)(uintptr_t)(eax + 4);
    *(int32_t *)(uintptr_t)(edx + 8) =
        (int32_t)(int16_t)*(uint16_t *)(uintptr_t)*(uint32_t *)(uintptr_t)(edx + 4);
    inv_bridge_void(REBUILD_WEAPON_LIST);                   /* rebuild_weapon_inventory_list */
}

/* use_item_on_self (0x1b141): apply the currently-held cursor item to the player (the "use" verb). Void;
 * reads g_current_cursor_entry 0x7fef0. Resolves the held template (get_dbase100_inventory_entry 0x18147
 * bridge), loads its name (read_next_dialogue_line bridge), matches its OnUse self-recipe
 * (resolve_item_use_action [L], target=0). With no recipe -> the "can't use that" message (resolve_dbase100_text
 * 0x2a + sprintf + queue_timed_message_color). With a recipe (esi != 0): the "used X" message (text 0x2d);
 * unless effect-only (esi == -1), consume the held item (consume_held_item [L]) and give the result item(s)
 * (give_item [L], low16 then high16); clears 0x7fef0; runs the effect command-record (eval_dialogue_record_by_id
 * + finish_dialogue_record_eval dbase100 bridges); refreshes the panel if 0x80b2c==0. flow_succ -> the shared
 * epilogue 0x18a24. LIVE-SWAP target (dbase100 effect chains); oracle stubs the text/crt/panel/dbase100 bridges. */
void use_item_on_self(void)
{
    uint8_t  buf_1a[0x64];                                  /* item-name buffer  [ebp+0x1a]  */
    uint8_t  buf_4a[0x64];                                  /* format string     [ebp-0x4a]  */
    uint8_t  buf_1da[0x190];                                /* sprintf output    [ebp-0x1da] */
    uint32_t effect_7e = 0;                                 /* effect command    [ebp+0x7e]  */
    regs_t io;

    uint32_t cur = (uint32_t)G32(VA_g_current_cursor_entry);
    uint32_t slot = *(uint32_t *)(uintptr_t)(cur + 4);
    int32_t item_id = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)slot;

    uint32_t tpl = get_dbase100_inventory_entry((uint32_t)item_id);  /* re-point 0x18147 (ecx/edx were unread) */
    uint32_t textid = *(uint32_t *)(uintptr_t)(tpl + 0x10);

    memset(&io, 0, sizeof io); io.va = READ_NEXT_DIALOGUE_LINE + OBJ_DELTA;
    io.eax = (uint32_t)(uintptr_t)buf_1a; io.ebx = textid; io.edx = 0x62;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    inv_standalone_route(&io);   /* M3 route (un-routed targets stay fail-loud inside) */
#endif

    uint32_t packed = resolve_item_use_action(tpl, 0, (uint32_t)(uintptr_t)&effect_7e);

    if (packed == 0) {                                     /* 0x1b224: no recipe -> can't use */
        memset(&io, 0, sizeof io); io.va = RESOLVE_DBASE100_TEXT + OBJ_DELTA;
        io.eax = (uint32_t)(uintptr_t)buf_4a; io.ebx = 0x2a; io.ecx = 0; io.edx = 0x62;
#ifndef ROTH_STANDALONE
        call_orig(&io);
#else
        inv_standalone_route(&io);   /* M3 route (un-routed targets stay fail-loud inside) */
#endif
        io_sprintf_msg(buf_1da, buf_4a, buf_1a);
        memset(&io, 0, sizeof io); io.va = QUEUE_TIMED_MESSAGE + OBJ_DELTA;
        io.eax = (uint32_t)(uintptr_t)buf_1da; io.edx = (uint8_t)G8(VA_g_default_message_color);
#ifndef ROTH_STANDALONE
        call_orig(&io);
#else
        inv_standalone_route(&io);   /* M3 route (un-routed targets stay fail-loud inside) */
#endif
        return;
    }

    /* recipe found */
    memset(&io, 0, sizeof io); io.va = RESOLVE_DBASE100_TEXT + OBJ_DELTA;
    io.eax = (uint32_t)(uintptr_t)buf_4a; io.ebx = 0x2d; io.ecx = 0; io.edx = 0x62;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    inv_standalone_route(&io);   /* M3 route (un-routed targets stay fail-loud inside) */
#endif

    if (packed != (uint32_t)-1) {                          /* not effect-only -> consume + give */
        consume_held_item(*(uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_current_cursor_entry) + 4));
        give_item((uint32_t)(int32_t)(int16_t)(uint16_t)packed, 0);   /* low16 = second */
        uint32_t first = packed >> 0x10;
        if (first != 0)
            give_item((uint32_t)(int32_t)(int16_t)(uint16_t)first, 0); /* high16 = first */
    }
    io_sprintf_msg(buf_1da, buf_4a, buf_1a);
    memset(&io, 0, sizeof io); io.va = QUEUE_TIMED_MESSAGE + OBJ_DELTA;
    io.eax = (uint32_t)(uintptr_t)buf_1da; io.edx = (uint8_t)G8(VA_g_default_message_color + 0x5);
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    inv_standalone_route(&io);   /* M3 route (un-routed targets stay fail-loud inside) */
#endif
    G32(VA_g_current_cursor_entry) = 0;
    if (effect_7e != 0) {                                  /* run the effect command-record */
        memset(&io, 0, sizeof io); io.va = EVAL_DIALOGUE_RECORD + OBJ_DELTA; io.eax = effect_7e;
#ifndef ROTH_STANDALONE
        call_orig(&io);
#else
        inv_standalone_route(&io);   /* M3 route (un-routed targets stay fail-loud inside) */
#endif
        memset(&io, 0, sizeof io); io.va = FINISH_DIALOGUE_EVAL + OBJ_DELTA;
#ifndef ROTH_STANDALONE
        call_orig(&io);
#else
        inv_standalone_route(&io);   /* M3 route (un-routed targets stay fail-loud inside) */
#endif
    }
    if (G32(VA_g_ui_panel_anchor_y + 0x4) == 0)
        inv_bridge_void(REFRESH_INVENTORY_PANEL);
}

/* combine_held_item_with_target (0x1b26d): EAX = target cursor index. Combine the held cursor item
 * (0x7fef0) with the inventory item at cursor index EAX. Resolves both templates (0x18147 bridges),
 * matches an OnUse recipe in either direction (resolve_item_use_action [L]); on no recipe shows the
 * "can't combine" message (text 0x29). On a recipe (esi != 0): the "combined X and Y" message (text 0x28),
 * then unless effect-only (esi == -1) it consumes/gives per which of the two ingredients the result item
 * ids match (the 5-way dispatch: second==held / first==held / second==target / first==target / neither),
 * clears 0x7fef0, refreshes the panel (the post-give held re-find loop is faithfully reproduced — its
 * `(esi<<count)` guard makes the body dead, a quirk of the original), and runs the effect record
 * (eval/finish dbase100 bridges). flow_succ -> use_item_on_self's shared epilogue 0x1b261. LIVE-SWAP. */
void combine_held_item_with_target(uint32_t eax_in)
{
    uint8_t  buf_24a[0x190];                                /* sprintf output  [ebp-0x24a] */
    uint8_t  buf_ba[0x64];                                  /* target name     [ebp-0xba]  */
    uint8_t  buf_56[0x64];                                  /* held name       [ebp-0x56]  */
    uint8_t  buf_e[0x64];                                   /* format string   [ebp+0xe]   */
    uint32_t effect = 0;                                    /* [ebp+0x72]      */
    regs_t io;

    uint32_t target_idx = eax_in;                          /* [ebp-0x24e] */
    uint32_t held_slot = *(uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_current_cursor_entry) + 4);
    int32_t  held_id = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)held_slot;     /* [ebp+0x7a] */
    uint32_t tgt_slot = (uint32_t)G32((VA_g_cursor_entry_table + 0x4) + target_idx * 0xc);              /* target entry[+4] */
    int32_t  target_id = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)tgt_slot;    /* [ebp+0x7e] */

    uint32_t held_tpl = get_dbase100_inventory_entry((uint32_t)held_id);    /* [ebp+0x76]; re-point 0x18147 */
    uint32_t tgt_tpl  = get_dbase100_inventory_entry((uint32_t)target_id);  /* re-point 0x18147 */

    uint32_t esi = 0;
    if (*(uint8_t *)(uintptr_t)(held_tpl + 5) & 0x40)       /* held combinable -> held x target */
        esi = resolve_item_use_action(held_tpl, (uint32_t)target_id, (uint32_t)(uintptr_t)&effect);
    if (esi == 0 && (*(uint8_t *)(uintptr_t)(tgt_tpl + 5) & 0x40))   /* else target x held */
        esi = resolve_item_use_action(tgt_tpl, (uint32_t)held_id, (uint32_t)(uintptr_t)&effect);

    memset(&io, 0, sizeof io); io.va = READ_NEXT_DIALOGUE_LINE + OBJ_DELTA;
    io.eax = (uint32_t)(uintptr_t)buf_56; io.ebx = *(uint32_t *)(uintptr_t)(held_tpl + 0x10); io.edx = 0x62;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    inv_standalone_route(&io);   /* M3 route (un-routed targets stay fail-loud inside) */
#endif
    memset(&io, 0, sizeof io); io.va = READ_NEXT_DIALOGUE_LINE + OBJ_DELTA;
    io.eax = (uint32_t)(uintptr_t)buf_ba; io.ebx = *(uint32_t *)(uintptr_t)(tgt_tpl + 0x10); io.edx = 0x62;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    inv_standalone_route(&io);   /* M3 route (un-routed targets stay fail-loud inside) */
#endif

    if (esi == 0) {                                         /* 0x1b314: no recipe -> can't combine */
        memset(&io, 0, sizeof io); io.va = RESOLVE_DBASE100_TEXT + OBJ_DELTA;
        io.eax = (uint32_t)(uintptr_t)buf_e; io.ebx = 0x29; io.ecx = 0; io.edx = 0x62;
#ifndef ROTH_STANDALONE
        call_orig(&io);
#else
        inv_standalone_route(&io);   /* M3 route (un-routed targets stay fail-loud inside) */
#endif
        io_sprintf_msg2(buf_24a, buf_e, buf_56, buf_ba);
        memset(&io, 0, sizeof io); io.va = QUEUE_TIMED_MESSAGE + OBJ_DELTA;
        io.eax = (uint32_t)(uintptr_t)buf_24a; io.edx = (uint8_t)G8(VA_g_default_message_color);
#ifndef ROTH_STANDALONE
        call_orig(&io);
#else
        inv_standalone_route(&io);   /* M3 route (un-routed targets stay fail-loud inside) */
#endif
        return;                                            /* -> 0x1b261 epilogue */
    }

    /* recipe found (0x1b35d): "combined X and Y" message */
    memset(&io, 0, sizeof io); io.va = RESOLVE_DBASE100_TEXT + OBJ_DELTA;
    io.eax = (uint32_t)(uintptr_t)buf_e; io.ebx = 0x28; io.ecx = 0; io.edx = 0x62;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    inv_standalone_route(&io);   /* M3 route (un-routed targets stay fail-loud inside) */
#endif
    io_sprintf_msg2(buf_24a, buf_e, buf_56, buf_ba);
    memset(&io, 0, sizeof io); io.va = QUEUE_TIMED_MESSAGE + OBJ_DELTA;
    io.eax = (uint32_t)(uintptr_t)buf_24a; io.edx = (uint8_t)G8(VA_g_default_message_color + 0x5);
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    inv_standalone_route(&io);   /* M3 route (un-routed targets stay fail-loud inside) */
#endif

    int cleared = 1;                                       /* paths jmp 0x1b47b (clear) by default; the two */
                                                           /* held-match paths jmp 0x1b485 (no clear) instead. */
    if (esi != (uint32_t)-1) {                             /* not effect-only -> give/remove dispatch */
        if (esi & 0xffff0000u) {                           /* both first(high) + second(low) */
            uint32_t second = esi & 0xffff;
            uint32_t first  = esi >> 0x10;
            if ((int32_t)second == held_id) {                       /* 0x1b3c8 -> 0x1b485 (no clear) */
                consume_held_item(tgt_slot);
                give_item((uint32_t)(int32_t)(int16_t)(uint16_t)first, 0);
                cleared = 0;
            } else if ((int32_t)first == held_id) {                 /* 0x1b3e7 -> 0x1b485 (no clear) */
                consume_held_item(tgt_slot);
                give_item((uint32_t)(int32_t)(int16_t)(uint16_t)second, 0);
                cleared = 0;
            } else if ((int32_t)second == target_id) {              /* 0x1b3fc */
                consume_held_item(*(uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_current_cursor_entry) + 4));
                give_item((uint32_t)(int32_t)(int16_t)(uint16_t)first, 0);
            } else if ((int32_t)first == target_id) {               /* 0x1b41b */
                consume_held_item(*(uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_current_cursor_entry) + 4));
                give_item((uint32_t)(int32_t)(int16_t)(uint16_t)second, 0);
            } else {                                                /* 0x1b433: neither -> both */
                consume_held_item(*(uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_current_cursor_entry) + 4));
                consume_held_item(tgt_slot);
                give_item((uint32_t)(int32_t)(int16_t)(uint16_t)second, 0);
                give_item((uint32_t)(int32_t)(int16_t)(uint16_t)first, 0);
            }
        } else {                                           /* 0x1b458: single result -> 0x1b47b (clear) */
            consume_held_item(*(uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_current_cursor_entry) + 4));
            consume_held_item(tgt_slot);
            give_item((uint32_t)(int32_t)(int16_t)(uint16_t)esi, 0);
        }
    }                                                      /* esi == -1 (effect-only) -> cleared stays 1 */

    if (cleared) G32(VA_g_current_cursor_entry) = 0;                         /* 0x1b47b */

    /* 0x1b485: post-give held re-find + panel refresh (the loop body is dead — see header) */
    if (G32(VA_g_current_cursor_entry) != 0) {
        uint32_t ebx = *(uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_current_cursor_entry) + 4);
        inv_bridge_void(REFRESH_INVENTORY_PANEL);
        uint32_t edx = 0x7fef4u + OBJ_DELTA;
        uint8_t  cl = (uint8_t)G8(VA_g_cursor_entry_count);
        uint32_t si = 0;
        for (;;) {
            uint32_t eax = si << (cl & 0x1f);
            if (eax == 0) break;                           /* je 0x1b4c9 (immediate on si==0) */
            if (ebx == *(uint32_t *)(uintptr_t)(edx + 4)) { G32(VA_g_current_cursor_entry) = (int32_t)edx; break; }
            edx += 0xc; si++;
        }
    } else {
        inv_bridge_void(REFRESH_INVENTORY_PANEL);
    }

    if (effect != 0) {                                     /* 0x1b4c9: run the effect record */
        memset(&io, 0, sizeof io); io.va = EVAL_DIALOGUE_RECORD + OBJ_DELTA; io.eax = effect;
#ifndef ROTH_STANDALONE
        call_orig(&io);
#else
        inv_standalone_route(&io);   /* M3 route (un-routed targets stay fail-loud inside) */
#endif
        memset(&io, 0, sizeof io); io.va = FINISH_DIALOGUE_EVAL + OBJ_DELTA;
#ifndef ROTH_STANDALONE
        call_orig(&io);
#else
        inv_standalone_route(&io);   /* M3 route (un-routed targets stay fail-loud inside) */
#endif
    }
}

/* ======================================================================================
 * C — Rendering (panel / grid / icons / labels). The oracle-able pieces (pure encoders + data builders)
 * are here; the framebuffer compositors are in-game (ROTH_LIFT_DIFF) — see below.
 * ====================================================================================== */

/* encode_item_icon_to_spans (0x13e35): EAX = dst span buffer, EDX = src icon pixels, EBX = width,
 * ECX = height (rows). Per row, RLE-encodes the icon to {skip_count, run_length, run_length pixel bytes}:
 * counts leading zero pixels (skip), copies the row, and trims trailing zeros (run_length = last-nonzero+1).
 * Pure (reads src, writes dst). Oracle: custom harness comparing the dst buffer. */
void encode_item_icon_to_spans(uint32_t eax_dst, uint32_t edx_src, uint32_t ebx_w, uint32_t ecx_h)
{
    uint8_t *esi = (uint8_t *)(uintptr_t)edx_src;
    uint8_t *edi = (uint8_t *)(uintptr_t)eax_dst;
    int32_t width = (int32_t)ebx_w;
    int32_t h = (int32_t)ecx_h;
    do {
        int32_t ebx = 0;
        while (esi[ebx] == 0) { ebx++; if (ebx >= width) break; }   /* leading skip */
        edi[0] = (uint8_t)ebx;
        edi += 2;
        uint8_t *saved = edi;
        int32_t ebp = 0;
        if (ebx < width) {
            int32_t ecx = 0;
            do {
                uint8_t al = esi[ebx];
                *edi++ = al;
                if (al != 0) ebp = ecx;
                ecx++; ebx++;
            } while (ebx < width);
            ebp++;
        }
        edi = saved;
        edi[-1] = (uint8_t)ebp;                                    /* run length */
        edi += ebp;
        esi += width;
    } while (--h != 0);
}

/* load_item_icon_resource (0x1816a): EAX = item index, EDX = open DAS file handle (PASSTHROUGH) -> EAX =
 * loaded DAS handle / 0. Resolves the item's template ([0x81e20][idx] + [0x81e1c]); if present, loads its
 * icon DAS resource ([rec+0xc]) via load_das_cache_resource 0x1869b (das_assets bridge; EAX=res, EDX=handle
 * threaded through — the original never reloads EDX). Non-idempotent -> in-game / sentinel-stub oracle. */
uint32_t load_item_icon_resource(uint32_t eax_in, uint32_t edx_in)
{
    uint32_t off = *(uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_dbase100_inventory_table) + eax_in * 4);
    if (off == 0) return 0;
    uint32_t res = *(uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_dbase100_base) + off + 0xc);
    /* [KEPT-REPOINT: load_das_cache_resource 0x1869b IS lifted, but (a) it is NOT independently
     * oracle-verified — the only test that touches it is this wrapper's SENTINEL byte-patch stub
     * (`mov [0x7e920],eax; mov [0x7e924],edx; ret`), which is precisely what proves res (EAX) + the
     * passthrough handle (EDX) are threaded; and (b) its real body does DOS lseek/read + cache-pool
     * alloc. A direct C call bypasses the sentinel, and running the callee REAL on both sides needs
     * the das_assets int21/DOS mock (g_os_soft_int) + a staged pool ported into test_inventory.c —
     * AND the res/handle threading assertion does not survive that swap: the read-count int21 mock is
     * fd/offset-agnostic, so handle threading is unobservable, and the res==0 early-out would elide
     * the handle path entirely. Unblock: an offset/fd-sensitive DOS mock that re-asserts the threading
     * via the loaded-resource pool state, plus a standalone test_das_assets test for 0x1869b. */
    regs_t io; memset(&io, 0, sizeof io); io.va = LOAD_DAS_CACHE_RESOURCE + OBJ_DELTA;
    io.eax = res; io.edx = edx_in;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    inv_standalone_route(&io);   /* M3 route (un-routed targets stay fail-loud inside) */
#endif
    return io.eax;
}

/* free_cursor_entry_icons (0x19ca7): clears the cursor-held / selected / equipped trackers (0x81048 /
 * 0x81030 / 0x8103c / 0x81034 / 0x81040), then walks the cursor-entry table (0x7fef4, [0x80af8] entries);
 * for each entry with a pool handle ([+0] != 0) it frees the handle (pool_free_handle bridge) and zeroes
 * every later entry that shares the same icon aux ([+8]) — deduping shared icon handles. Void; flow_succ
 * into shared_epilogue_6reg. Oracle: pool bridge stubbed, diff the obj3 cursor table + trackers. */
void free_cursor_entry_icons(void)
{
    uint32_t ebx = 0x7fef4u + OBJ_DELTA;
    G32(VA_g_selected_item_primary + 0x4) = 0; G32(VA_g_left_hand_item) = 0; G32(VA_g_right_hand_item) = 0; G32(VA_g_displayed_item_left) = 0; G32(VA_g_displayed_item_right) = 0;
    uint32_t ecx = 0;
    for (;;) {
        if (ecx >= (uint32_t)G32(VA_g_cursor_entry_count + 0x4)) return;
        if (*(uint32_t *)(uintptr_t)ebx == 0) { ebx += 0xc; ecx++; continue; }
        pool_free_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle),   /* re-point 0x360b3; edx=handle slot addr */
                                (uint32_t *)(uintptr_t)*(uint32_t *)(uintptr_t)ebx);
        uint32_t eax2 = ebx, edx2 = ecx, esi = *(uint32_t *)(uintptr_t)(ebx + 8);
        for (;;) {
            if (edx2 >= (uint32_t)G32(VA_g_cursor_entry_count + 0x4)) break;
            if (esi == *(uint32_t *)(uintptr_t)(eax2 + 8)) {
                *(uint32_t *)(uintptr_t)(eax2 + 8) = 0;
                *(uint32_t *)(uintptr_t)eax2 = 0;
            }
            eax2 += 0xc; edx2++;
        }
        ebx += 0xc; ecx++;
    }
}

/* format_inventory_item_label (0x1c0b1): EAX = item index, EDX = quantity. Resolves the item's label text
 * id (template [rec+0x10], overridden by resolve_record_conditional_op2 [L] if it returns non-zero), caches
 * it in [0x811a4]/[0x811a8] (no-op + return 0 if already current), then loads the label text into the global
 * buffer [0x8105e] (read_next_dialogue_line bridge) — formatting "<name> (<qty>)" via sprintf when qty != 0.
 * Returns 0 (cached/empty) or 1. Oracle: text/dbase100 bridges stubbed, diff obj3 + return. */
uint32_t format_inventory_item_label(uint32_t eax_in, uint32_t edx_in)
{
    uint32_t esi = edx_in;
    if (eax_in == 0) return 0;
    uint32_t off = *(uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_dbase100_inventory_table) + eax_in * 4);
    uint32_t rec = (uint32_t)G32(VA_g_dbase100_base) + off;
    uint32_t ebx = *(uint32_t *)(uintptr_t)(rec + 0x10);
    uint32_t r = resolve_record_conditional_op2(rec);
    if (r != 0) ebx = r;
    if (ebx == (uint32_t)G32(VA_g_inventory_arrow_state + 0x4) && esi == (uint32_t)G32(VA_g_inventory_arrow_state + 0x8)) return 0;
    G8(VA_g_selected_item_primary + 0x1a) = 0;
    G32(VA_g_inventory_arrow_state + 0x4) = (int32_t)ebx;
    G32(VA_g_inventory_arrow_state + 0x8) = (int32_t)esi;
    if (ebx == 0) return 1;
    if (esi == 0) {
        regs_t io; memset(&io, 0, sizeof io); io.va = READ_NEXT_DIALOGUE_LINE + OBJ_DELTA;
        io.eax = GADDR(VA_g_selected_item_primary + 0x1a); io.ebx = ebx; io.edx = 0x61;
#ifndef ROTH_STANDALONE
        call_orig(&io);
#else
        inv_standalone_route(&io);   /* M3 route (un-routed targets stay fail-loud inside) */
#endif
        return 1;
    }
    uint8_t buf[0x64];
    regs_t io; memset(&io, 0, sizeof io); io.va = READ_NEXT_DIALOGUE_LINE + OBJ_DELTA;
    io.eax = (uint32_t)(uintptr_t)buf; io.ebx = ebx; io.edx = 0x61;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    inv_standalone_route(&io);   /* M3 route (un-routed targets stay fail-loud inside) */
#endif
    regs_t io2; memset(&io2, 0, sizeof io2); io2.va = CRT_SPRINTF + OBJ_DELTA; io2.nstack = 4;
    io2.stack[0] = GADDR(VA_g_selected_item_primary + 0x1a); io2.stack[1] = GADDR(VA_g_heap_free_list + 0x81c);
    io2.stack[2] = (uint32_t)(uintptr_t)buf; io2.stack[3] = esi;
#ifndef ROTH_STANDALONE
    call_orig(&io2);
#else
    roth_sprintf((char *)GADDR(VA_g_selected_item_primary + 0x1a), (const char *)GADDR(VA_g_heap_free_list + 0x81c), buf, esi);   /* item-label sprintf(dst,fmt,buf,esi) */
#endif
    return 1;
}

/* video_display bridges used by the rendering wrappers. */
#define REGISTER_DIRTY_RECT       0x15b5bu
#define BLIT_RELOC_DAS_IMAGE      0x18e68u
/* blit_reloc_das_image (0x18e68) forwards EBX straight to the RLE blitter 0x1325b, which uses it as the
 * destination row stride. The original always has EBX=[0x85498] (framebuffer pitch) live at every call;
 * missing it makes the per-row advance -width -> runaway fill. Always route DAS blits through this. */
#define inv_blit_das(ptr, id) \
    inv_call(BLIT_RELOC_DAS_IMAGE, (uint32_t)(uintptr_t)(ptr), (uint32_t)(id), (uint32_t)G32(VA_g_screen_pitch), 0)
/* das_assets / blit_2d / dialogue_ui / dos bridges + within-subsystem callees (bridged for incremental
 * per-function ROTH_LIFT_DIFF verification). */
#define BLIT_DAS_IMAGE_TO_BUFFER  0x1325bu
#define DRAW_TRANSLUCENT_SPANS    0x13e81u
#define DRAW_UI_PANEL_COUNT       0x1a0abu
#define FREE_DAS_CACHE_HANDLE     0x13136u
#define FLUSH_OBJECT_DAS          0x26cd4u
#define RESET_WEAPON_HUD          0x1be8eu
#define RENDER_WEAPON_HUD         0x24165u
/* DOS file bridges (0x41ae5 open / 0x41b9a lseek / 0x41b53 read / 0x41b41 close) re-pointed to the
 * native dos_* wrappers (C2); the #defines were retired with their last users. */
#define IV_BUILD_ENTRY_LIST       0x19d30u
#define IV_RENDER_GRID            0x1c163u
#define IV_DRAW_ENTRY_LABEL       0x1c020u

static void inv_register_dirty_rect(uint32_t x, uint32_t y, uint32_t x2, uint32_t y2)
{
    regs_t io; memset(&io, 0, sizeof io); io.va = REGISTER_DIRTY_RECT + OBJ_DELTA;
    io.eax = x; io.edx = y; io.ebx = x2; io.ecx = y2;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    register_dirty_rect(x, (int32_t)y, x2, y2);   /* M3 route (same mapping as dialogue_ui's dlg_call4) */
#endif
}

/* generic register-threaded bridge (call the original at canon `va` with the given GP regs -> EAX out).
 * Used by the in-game framebuffer/lifecycle renderers to call their das/text/file/within-subsystem callees
 * (so each is independently ROTH_LIFT_DIFF-verifiable, callees = original; re-point to direct C is later). */
static uint32_t inv_call(uint32_t va, uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx)
{
    regs_t io; memset(&io, 0, sizeof io); io.va = va + OBJ_DELTA;
    io.eax = eax; io.edx = edx; io.ebx = ebx; io.ecx = ecx;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    switch (va) {   /* M3 routes into the verified lifted bodies. Args are register-positional
                     * (EAX,EDX,EBX,ECX) except where the body's proto comment says otherwise. */
    /* void, no-arg */
    case 0x19ca7u: free_cursor_entry_icons(); return 0;
    case 0x1bb4bu: update_selected_item_icon(); return 0;
    case 0x11124u: check_snapshot_key(); return 0;
    case 0x1a88bu: commit_held_cursor_item(); return 0;
    case 0x1a2b5u: select_inventory_tab(); return 0;
    case 0x1a7a1u: close_inventory_panel(); return 0;
    case 0x1bf7bu: refresh_inventory_panel(); return 0;
    case 0x167d7u: clear_corner_peek_icon(); return 0;
    case 0x1a37bu: compute_viewport_half_extents(); return 0;
    case 0x26cd4u: flush_object_das_handles(); return 0;
    case 0x1c469u: refresh_inventory_grid(); return 0;
    case 0x1a2efu: draw_equipped_item_left(); return 0;
    case 0x1bfaau: draw_equipped_item_right(); return 0;
    case 0x1a2d2u: draw_inventory_tabs(); return 0;
    case 0x1c163u: render_inventory_grid(); return 0;
    case 0x1a0abu: draw_ui_panel_count_element(); return 0;
    case 0x2ab21u: copy_word_90bcc_to_8532a(); return 0;
    case 0x12b45u: grayscale_background_view(); return 0;    /* body derives everything from
                                                                     * globals; the site's regs are dead */
    /* void, register args */
    case 0x19d30u: build_inventory_entry_list(eax); return 0;
    case 0x1c020u: draw_inventory_entry_label(eax); return 0;
    case 0x19ee6u: draw_panel_slot_tile(eax); return 0;
    case 0x414d2u: ensure_das_cache_heap_space(eax); return 0;
    case 0x19f34u: draw_item_icon_in_slot(eax, edx, ebx); return 0;
    case 0x24165u: render_weapon_hud(eax, edx); return 0;
    case 0x184abu: activate_weapon_item(eax, edx); return 0;
    case 0x18e68u: blit_reloc_das_image(eax, edx, ebx); return 0;
    case 0x1325bu: blit_das_image_to_buffer(eax, edx, ebx, ecx); return 0;
    case 0x13e81u: draw_translucent_icon_spans(eax, edx, ebx, (int32_t)ecx); return 0;
    case 0x13544u: blit_item_icon(eax, edx, ebx, ecx); return 0;
    case 0x12ddeu: draw_popup_shadow_border_smc((int32_t)eax, (int32_t)edx,
                                                       (int32_t)ebx, (int32_t)ecx); return 0;
    case 0x1a079u: draw_text_at_screen_xy(eax, ebx, ecx, edx); return 0;   /* EAX=str,EBX=x,ECX=y,EDX=flags */
    case 0x142b7u: capture_screen_thumbnail(eax, (uint32_t)G32(VA_g_framebuffer_ptr),
                                                   (uint32_t)G32(VA_g_das_remap_chunk_10000_ptr)); return 0;
                   /* the body's src/lut params = fb base + blend LUT: the ORIGINAL derives them from
                    * these globals internally (corpus 0x142b7); mirrors the savegame caller. The
                    * site's edx/ebx (0,0) are dead leftovers. */
    /* value-returning */
    case 0x1fa91u: return (uint32_t)node_has_available_choice(eax);
    case 0x1816au: return load_item_icon_resource(eax, edx);
    case 0x1c9a0u: return (uint32_t)rng_next_index_for_count((int32_t)eax);
    case 0x18260u: return apply_weapon_action_attributes(eax, edx, ebx);
    case 0x1c0b1u: return format_inventory_item_label(eax, edx);
    case 0x13136u: return free_das_cache_handle(eax);
    case 0x1299au: return (uint32_t)input_ring_dequeue();
    case 0x18a2au: return try_interrupt_dialogue_voice();
    case 0x26501u: return run_options_menu(eax);
    case 0x21dc6u: return write_savegame_file(eax);
    case 0x196b9u: return load_dbase300_resource_at_offset(eax, edx, ebx, ecx);
    case 0x360f9u: return pool_alloc_handle((uint32_t *)(uintptr_t)eax, (int32_t)edx);
    case 0x1f91fu: return (uint32_t)measure_control_text_width((const char *)(uintptr_t)eax);
    case 0x1e8ccu: return read_next_dialogue_line(eax, edx, ebx, ecx);
    default: break;
    }
    roth_unreachable(va);   /* inventory register-threaded bridge — in-game inventory tier */
#endif
    return io.eax;
}

/* draw_item_icon_in_slot (0x19f34): EAX = icon descriptor, EDX = grid slot index, EBX = color. Centers the
 * icon in the slot cell (slot screen tables 0x71208/0x7120a + panel origin [0x80b24]/[0x80b28]), registers
 * the dirty rect, and blits via blit_item_icon [L] (mode = (color<<8) | (hires?1:2)). The blit lands in the
 * staged framebuffer ([0x90a98]) -> oracle-able (register_dirty_rect bridge stubbed). */
void draw_item_icon_in_slot(uint32_t eax_in, uint32_t edx_in, uint32_t ebx_in)
{
    if (eax_in == 0) return;
    uint32_t icon = eax_in, slot = edx_in;
    int32_t w = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)(icon + 4);
    int32_t h = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)(icon + 6);
    int32_t x = (int32_t)(uint16_t)G16(VA_g_ui_slot_layout_table + slot * 4) + (int32_t)G32(VA_g_ui_panel_anchor_x) + ((int32_t)(0x1c - w) >> 1);
    int32_t y = (int32_t)(uint16_t)G16(VA_g_ui_slot_layout_table_ext + slot * 4) + (int32_t)G32(VA_g_ui_panel_anchor_y) + ((int32_t)(0x38 - h) >> 2);
    inv_register_dirty_rect((uint32_t)x, (uint32_t)y, (uint32_t)(x + w), (uint32_t)(y + (h >> 1)));
    uint32_t mode = (ebx_in << 8) | (uint32_t)((G8(VA_g_hires_line_doubling_flag) == 0) ? 2 : 1);
    uint8_t *ptr = screen_xy_to_framebuffer_ptr(x, y);
    blit_item_icon(icon, (uint32_t)(uintptr_t)ptr, (uint32_t)G32(VA_g_screen_pitch), mode);
}

/* draw_item_icon_centered (0x19fcf): EAX = icon, EDX = x, EBX = y, ECX = color. Centers the icon about
 * (x,y) (offset by (0x1c-w)/2, (0x38-h)/4) and blits via blit_item_icon [L]. Oracle-able (no dirty rect). */
void draw_item_icon_centered(uint32_t eax_in, uint32_t edx_in, uint32_t ebx_in, uint32_t ecx_in)
{
    uint32_t icon = eax_in;
    if (icon == 0) return;
    int32_t x = (int32_t)edx_in + ((int32_t)(0x1c - (int32_t)(int16_t)*(uint16_t *)(uintptr_t)(icon + 4)) >> 1);
    int32_t y = (int32_t)ebx_in + ((int32_t)(0x38 - (int32_t)(int16_t)*(uint16_t *)(uintptr_t)(icon + 6)) >> 2);
    uint32_t mode = (ecx_in << 8) | (uint32_t)((G8(VA_g_hires_line_doubling_flag) == 0) ? 2 : 1);
    uint8_t *ptr = screen_xy_to_framebuffer_ptr(x, y);
    blit_item_icon(icon, (uint32_t)(uintptr_t)ptr, (uint32_t)G32(VA_g_screen_pitch), mode);
}

/* draw_equipped_item_left (0x1a2ef): draws the left-equipped item's icon ([0x81034] -> [+0] -> [+0] = icon
 * descriptor) at a fixed panel position (color [0x7675e]). flow_succ -> shared epilogue. Oracle-able. */
void draw_equipped_item_left(void)
{
    if (G32(VA_g_displayed_item_left) == 0) return;
    uint32_t esi = *(uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_displayed_item_left);
    if (esi == 0) return;
    esi = *(uint32_t *)(uintptr_t)esi;                     /* icon descriptor */
    int32_t h = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)(esi + 6);
    int32_t y = (int32_t)G32(VA_g_ui_panel_anchor_y) + 0xf + ((int32_t)(0x38 - h) >> 2);
    uint32_t submode = (uint32_t)((G8(VA_g_hires_line_doubling_flag) == 0) ? 1 : 0) + 1;
    uint32_t mode = submode + ((uint32_t)(uint8_t)G8(VA_g_default_message_color) << 8);
    int32_t w = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)(esi + 4);
    int32_t x = (int32_t)G32(VA_g_ui_panel_anchor_x) + 0x3a + ((int32_t)(0x1c - w) >> 1);
    uint8_t *ptr = screen_xy_to_framebuffer_ptr(x, y);
    blit_item_icon(esi, (uint32_t)(uintptr_t)ptr, (uint32_t)G32(VA_g_screen_pitch), mode);
}

/* draw_equipped_item_right (0x1bfaa): mirror of draw_equipped_item_left for the right-equipped item
 * ([0x81040], color [0x76763], x offset +0x59); shares the screen_xy + blit tail (jmp 0x1a360). Oracle-able. */
void draw_equipped_item_right(void)
{
    if (G32(VA_g_displayed_item_right) == 0) return;
    uint32_t esi = *(uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_displayed_item_right);
    if (esi == 0) return;
    esi = *(uint32_t *)(uintptr_t)esi;
    int32_t h = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)(esi + 6);
    int32_t y = (int32_t)G32(VA_g_ui_panel_anchor_y) + 0xf + ((int32_t)(0x38 - h) >> 2);
    uint32_t submode = (uint32_t)((G8(VA_g_hires_line_doubling_flag) == 0) ? 1 : 0) + 1;
    uint32_t mode = submode + ((uint32_t)(uint8_t)G8(VA_g_default_message_color + 0x5) << 8);
    int32_t w = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)(esi + 4);
    int32_t x = (int32_t)G32(VA_g_ui_panel_anchor_x) + 0x59 + ((int32_t)(0x1c - w) >> 1);
    uint8_t *ptr = screen_xy_to_framebuffer_ptr(x, y);
    blit_item_icon(esi, (uint32_t)(uintptr_t)ptr, (uint32_t)G32(VA_g_screen_pitch), mode);
}

/* set_inventory_list_filter (0x1ca2e): EAX = filter flags. Clears the cursor dual-array
 * (clear_dual_array_80afc [L]); for filter bit0 set, clears the per-slot "filtered" flag ([slot+1]&0x80)
 * on all used slots; for bit0 clear, sets that flag on every non-category-2 (non-weapon) item (template via
 * get_dbase100_inventory_entry 0x18147 bridge) and resets the selection ([0x81038]/[0x81044]=0 +
 * update_selected_item_icon + reset_weapon_hud bridges); then rebuild_weapon_inventory_list. Oracle: the
 * das/weapon bridges ret-stubbed (0x18147 + the [L] array clear run real), diff obj3 (slot flags + globals). */
void set_inventory_list_filter(uint32_t eax_in)
{
    uint32_t ebx = eax_in;
    clear_dual_array_80afc();                       /* 0x1c59e [L] */
    uint32_t edx = 0x80c30u + OBJ_DELTA;
    if (ebx & 1) {
        uint32_t eax = 0;
        for (;;) {
            if (eax >= (uint32_t)G32(VA_g_inventory_count)) break;
            if (*(uint16_t *)(uintptr_t)edx == 0) { edx += 4; continue; }
            eax++;
            *(uint8_t *)(uintptr_t)(edx + 1) &= 0x7f;
            edx += 4;
        }
    } else {
        uint32_t cnt = 0;
        for (;;) {
            if (cnt >= (uint32_t)G32(VA_g_inventory_count)) {
                G32(VA_g_selected_item_secondary) = 0; G32(VA_g_selected_item_primary) = 0;
                inv_bridge_void(UPDATE_SELECTED_ICON);
                inv_bridge_void(0x1be8eu);                 /* reset_weapon_hud (weapon_combat) */
                break;
            }
            if (*(uint16_t *)(uintptr_t)edx == 0) { edx += 4; continue; }
            uint32_t rec = get_dbase100_inventory_entry(              /* re-point 0x18147 */
                (uint32_t)(int32_t)(int16_t)*(uint16_t *)(uintptr_t)edx);
            uint32_t cat = ((uint32_t)*(uint32_t *)(uintptr_t)(rec + 4) & 0xf00) >> 8;
            if (cat != 2) *(uint8_t *)(uintptr_t)(edx + 1) |= 0x80;
            cnt++;
            edx += 4;
        }
    }
    inv_bridge_void(REBUILD_WEAPON_LIST);                  /* 0x2245c */
}

/* draw_panel_slot_tile (0x19ee6): EAX = slot index. Draws the empty-slot tile background (DAS image 0x110)
 * at the slot cell. flow_succ -> shared epilogue. Bridges register_dirty_rect + blit_reloc_das_image (das,
 * the actual tile blit) -> in-game (ROTH_LIFT_DIFF). */
void draw_panel_slot_tile(uint32_t eax_in)
{
    uint32_t slot = eax_in;
    int32_t y = (int32_t)(uint16_t)G16(VA_g_ui_slot_layout_table_ext + slot * 4) + (int32_t)G32(VA_g_ui_panel_anchor_y);
    int32_t x = (int32_t)(uint16_t)G16(VA_g_ui_slot_layout_table + slot * 4) + (int32_t)G32(VA_g_ui_panel_anchor_x);
    inv_register_dirty_rect((uint32_t)x, (uint32_t)y, (uint32_t)(x + 0x1b), (uint32_t)(y + 0x1b));
    uint8_t *ptr = screen_xy_to_framebuffer_ptr(x, y);
    regs_t io; memset(&io, 0, sizeof io); io.va = BLIT_RELOC_DAS_IMAGE + OBJ_DELTA;
    io.eax = (uint32_t)(uintptr_t)ptr; io.edx = 0x110; io.ebx = (uint32_t)G32(VA_g_screen_pitch);  /* dest stride */
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    inv_standalone_route(&io);   /* M3 route (un-routed targets stay fail-loud inside) */
#endif
}

/* build_inventory_entry_list (0x19d30): EAX = filter category. Rebuilds the cursor-entry list at 0x7fef4
 * from the carried array, keeping items of the given category (category-2 items also need an available
 * choice — node_has_available_choice bridge), re-points the held/equipped/selected trackers, appends the
 * equipped + selected items as special entries, then loads each entry's icon (dos_open + load_item_icon_resource
 * [L] dedup + dos_close). Void. In-game (das/dos/dialogue). Callees bridged for per-fn diff. */
void build_inventory_entry_list(uint32_t eax_in)
{
    int32_t filter = (int32_t)eax_in;
    G32(VA_g_current_cursor_entry) = 0;
    uint32_t edx = 0x7fef4u + OBJ_DELTA;                   /* cursor entry table */
    uint32_t ebx = 0x80c30u + OBJ_DELTA;                   /* carried base */
    inv_call(0x19ca7u, 0, 0, 0, 0);                        /* free_cursor_entry_icons */
    int32_t esi = 0;
    G16(VA_g_inventory_active_tab + 0x42) = 0x2a;
    int32_t count = (int32_t)G32(VA_g_inventory_count);
    G16(VA_g_inventory_active_tab + 0x4a) = 0x2b;
    while (count > 0) {
        int32_t ecx = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)ebx;
        if (ecx == 0) { ebx += 4; continue; }              /* empty -> no dec */
        if (ecx > 0) {
            uint32_t off = *(uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_dbase100_inventory_table) + (uint32_t)ecx * 4);
            if (off != 0) {
                uint32_t rec = (uint32_t)G32(VA_g_dbase100_base) + off;    /* [0x81e1c]+off = item record ptr */
                int32_t cat = ((int32_t)*(uint32_t *)(uintptr_t)(rec + 4) >> 8) & 0xf;
                if (cat == filter) {
                    int keep = 1;
                    if (cat == 2) {
                        if (inv_call(NODE_HAS_AVAILABLE_CHOICE, rec, 0, 0, 0) == 0) keep = 0;
                        else if (ebx == (uint32_t)G32(VA_g_selected_item_primary + 0x8)) G32(VA_g_selected_item_primary + 0x4) = (int32_t)edx;
                    } else {
                        if (ebx == (uint32_t)G32(VA_g_selected_item_secondary)) G32(VA_g_left_hand_item) = (int32_t)edx;
                        else if (ebx == (uint32_t)G32(VA_g_selected_item_primary)) G32(VA_g_right_hand_item) = (int32_t)edx;
                    }
                    if (keep) {
                        edx += 0xc;
                        *(uint32_t *)(uintptr_t)(edx - 8) = ebx;        /* entry[+4] = slot ptr */
                        esi++;
                        *(uint32_t *)(uintptr_t)(edx - 4) = (uint32_t)ecx;  /* entry[+8] = slot id */
                    }
                }
            }
        }
        count--; ebx += 4;                                 /* ecx<=0 or processed -> dec */
    }
    uint32_t equip = (uint32_t)G32(VA_g_selected_item_secondary);
    int32_t total = esi;
    G32(VA_g_cursor_entry_count) = esi;
    if (equip != 0 && (int32_t)(int16_t)*(uint16_t *)(uintptr_t)equip > 0) {
        G32(VA_g_displayed_item_left) = (int32_t)edx;
        *(uint32_t *)(uintptr_t)(edx + 4) = equip;
        edx += 0xc;
        total = esi + 1;
        *(uint32_t *)(uintptr_t)(edx - 4) = (uint32_t)(int32_t)(int16_t)*(uint16_t *)(uintptr_t)equip;
    }
    uint32_t selp = (uint32_t)G32(VA_g_selected_item_primary);
    if (selp != 0 && (int32_t)(int16_t)*(uint16_t *)(uintptr_t)selp > 0) {
        G32(VA_g_displayed_item_right) = (int32_t)edx;
        *(uint32_t *)(uintptr_t)(edx + 4) = selp;
        total++;
        *(uint32_t *)(uintptr_t)(edx + 8) = (uint32_t)(int32_t)(int16_t)*(uint16_t *)(uintptr_t)selp;
    }
    G32(VA_g_cursor_entry_count + 0x4) = total;
    if (total == 0) return;
    uint32_t handle = dos_open_file(0x81f06u + OBJ_DELTA, 0);  /* dos_open_file(archive,0) (C2); ebx/ecx inert to open */
    if (handle == 0) { G32(VA_g_cursor_entry_count) = handle; return; }
    uint32_t e = 0x7fef4u + OBJ_DELTA;
    for (int32_t si = 0; si < total; si++, e += 0xc) {
        uint32_t aux = *(uint32_t *)(uintptr_t)(e + 8);
        if (aux == 0 || *(uint32_t *)(uintptr_t)e != 0) continue;
        uint32_t h = inv_call(0x1816au, aux, handle, 0, 0);            /* load_item_icon_resource(idx, handle) */
        if (h == 0) continue;
        *(uint32_t *)(uintptr_t)e = h;
        uint32_t e2 = e;
        for (int32_t j = si; j < total; j++, e2 += 0xc)
            if (*(uint32_t *)(uintptr_t)(e2 + 8) == aux) *(uint32_t *)(uintptr_t)e2 = h;  /* dedup */
    }
    dos_close_handle(handle);                                  /* dos_close_handle (C2) */
}

/* find_or_autoselect_inventory_item (0x1cb6c): ECX = candidate-id array (the incoming EAX), EDX = flags,
 * EBX = candidate count -> EAX. Select mode (flags&1): try to keep the selected/equipped item if it's a
 * candidate; else (unless flags&2) collect every carried item matching a candidate and RANDOMLY pick one
 * (rng_next_index_for_count [L] bridge) + select it (update_selected_item_icon bridge). Count mode (flags&1
 * clear): returns the number of carried items matching a candidate. Void-ish (returns the count/result).
 * In-game (RNG non-determinism + DAS). */
uint32_t find_or_autoselect_inventory_item(uint32_t eax_in, uint32_t edx_in, uint32_t ebx_in)
{
    int32_t pick[24];                                      /* [ebp-0x64..] collected matches */
    uint32_t ecx = eax_in;                                 /* candidate array base */
    uint32_t inv = 0x80c30u + OBJ_DELTA;                   /* carried base (eax) */
    G32(VA_g_item_autoselected_flag) = 0;
    int32_t local8 = (int32_t)G32(VA_g_object_select_easy_flag);                /* [ebp-8] */
    int32_t result = 0;                                    /* [ebp-4] */
    if (edx_in & 2) local8 = 1;
    uint32_t cand_end = ebx_in * 4 + ecx;                  /* &candidate[count] */

    if (edx_in & 1) {                                      /* select mode */
        if (G32(VA_g_selected_item_primary) != 0) {
            int32_t sid = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)(uint32_t)G32(VA_g_selected_item_primary);
            for (uint32_t p = ecx; p < cand_end; p += 4)
                if (sid == *(int32_t *)(uintptr_t)p) {
                    G32(VA_g_held_item_slide_timer) = 0; G32(VA_g_last_item_record) = *(int32_t *)(uintptr_t)p;
                    return 1;
                }
        }
        if (G32(VA_g_selected_item_secondary) != 0) {
            int32_t eid = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)(uint32_t)G32(VA_g_selected_item_secondary);
            uint32_t end2 = ecx + ebx_in * 4;
            for (uint32_t p = ecx; p < end2; p += 4)
                if (eid == *(int32_t *)(uintptr_t)p) { G32(VA_g_last_item_record) = eid; return 1; }
        }
        if (!(local8 & 1)) {                               /* autoselect */
            uint32_t cend = ecx + ebx_in * 4;
            int32_t n = 0;
            for (uint32_t s = 0; s < (uint32_t)G32(VA_g_inventory_count); ) {
                uint32_t slot = inv;
                if (*(uint16_t *)(uintptr_t)slot == 0) { inv += 4; continue; }
                int32_t id = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)slot;
                for (uint32_t p = ecx; p < cend; p += 4)
                    if (id == *(int32_t *)(uintptr_t)p && n < 24) pick[n++] = (int32_t)slot;
                inv += 4; s++;
            }
            if (n != 0) {
                uint32_t idx = inv_call(0x1c9a0u, (uint32_t)n, 0, 0, 0) & 0xffff;  /* rng_next_index_for_count */
                G32(VA_g_selected_item_primary) = pick[idx];
                inv_call(0x1bb4bu, 0, 0, 0, 0);            /* update_selected_item_icon */
                G32(VA_g_item_autoselected_flag) = 1;
                return 0;
            }
        }
        return (uint32_t)result;
    }
    /* count mode */
    uint32_t cend = ecx + ebx_in * 4;
    for (uint32_t s = 0; s < (uint32_t)G32(VA_g_inventory_count); ) {
        uint32_t slot = inv;
        if (*(uint16_t *)(uintptr_t)slot == 0) { inv += 4; continue; }
        int32_t id = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)slot;
        for (uint32_t p = ecx; p < cend; p += 4)
            if (id == *(int32_t *)(uintptr_t)p) { result++; G32(VA_g_last_item_record) = id; }
        inv += 4; s++;
    }
    return (uint32_t)result;
}

/* ===== In-game framebuffer / lifecycle renderers (ROTH_LIFT_DIFF tier; callees bridged). ===== */

/* update_selected_item_icon (0x1bb4b): rebuild the selected-item drag icon. Clears the scale anim;
 * if no/unchanged selection -> reset state + return; else loads the item's icon DAS from disk
 * (dos_open/lseek/read/close bridges), decodes it (blit_das_image_to_buffer bridge) into a stack buffer,
 * and RLE-encodes the spans into the global icon descriptor 0x81310 (encode_item_icon_to_spans [L]).
 * Caches the selected id in [0x8130c]; sets the drag dims [0x819b0]/[0x819b4]. Void. In-game (DOS+DAS). */
void update_selected_item_icon(void)
{
    uint8_t buf[0x1004];                                   /* [ebp-0x1004] decode buffer */
    G32(VA_g_held_item_slide_timer) = 0;
    if (!(G32(VA_g_selected_item_primary) != 0 && G32(VA_g_held_item_icon_width + 0x4) != 0)) {       /* no live selection -> reset */
        G32(VA_g_held_item_icon_width) = 0; G32(VA_g_pending_choice_accept_index + 0x8) = 0;
        return;
    }
    uint32_t sel = (uint32_t)G32(VA_g_selected_item_primary);
    int32_t id = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)sel;
    if (id == (int32_t)G32(VA_g_pending_choice_accept_index + 0x8)) return;               /* unchanged */
    G32(VA_g_held_item_icon_width) = 0;
    G32(VA_g_pending_choice_accept_index + 0x8) = id;
    uint32_t off = *(uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_dbase100_inventory_table) + (uint32_t)id * 4);
    if (off == 0) return;
    uint32_t res = *(uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_dbase100_base) + off + 0xc);
    if (res == 0) return;
    uint32_t handle = dos_open_file(0x81f06u + OBJ_DELTA, 0);   /* dos_open_file(archive,0) (C2) */
    if (handle == 0) return;
    dos_lseek(handle, res << 3, 0);                 /* dos_lseek(handle, res<<3, whence 0) (C2); ecx=handle inert */
    uint32_t hdr = 0;
    dos_read_items((uint32_t)(uintptr_t)&hdr, 4, 1, handle);            /* size=4,count=1: chunk-size dword (C2) */
    dos_read_items((uint32_t)(uintptr_t)(buf + 0x800), 1, hdr, handle); /* size=1,count=hdr -> [ebp-0x804] (C2) */
    dos_close_handle(handle);                       /* dos_close_handle (C2) */
    G32(VA_g_held_item_icon_spans + 0x6a0) = (int32_t)(int16_t)*(uint16_t *)(buf + 0x804);           /* [ebp-0x800] width */
    G32(VA_g_held_item_icon_width) = (int32_t)(int16_t)*(uint16_t *)(buf + 0x806);           /* [ebp-0x7fe] height */
    if (G8(VA_g_hires_line_doubling_flag) == 0) G32(VA_g_held_item_icon_width) = (int32_t)((uint32_t)G32(VA_g_held_item_icon_width) >> 1);
    uint32_t cc = (uint32_t)((G8(VA_g_hires_line_doubling_flag) == 0) ? 2 : 1);
    inv_call(BLIT_DAS_IMAGE_TO_BUFFER, (uint32_t)(uintptr_t)(buf + 0x800),
             (uint32_t)(uintptr_t)buf, (uint32_t)(int32_t)(int16_t)*(uint16_t *)(buf + 0x804), cc);  /* ebx=(int16)[esi+4]=width */
    encode_item_icon_to_spans(GADDR(VA_g_held_item_icon_spans), (uint32_t)(uintptr_t)buf,
                                     (uint32_t)G32(VA_g_held_item_icon_spans + 0x6a0), (uint32_t)G32(VA_g_held_item_icon_width));
}

/* draw_held_item_icon (0x1bcc4): draws the selected item's icon translucently "stuck" to the cursor while
 * it scales in (the drag anim); advances the scale timer [0x819bc] by [0x85324] and blits via
 * draw_translucent_icon_spans (blit_2d bridge) from the icon descriptor 0x81310. Void. In-game. */
void draw_held_item_icon(void)
{
    if ((int32_t)G32(VA_g_view_h) < 0x46) return;
    int32_t edx = (int32_t)G32(VA_g_held_item_slide_timer) - 0x8c;
    int32_t ecx = (int32_t)G32(VA_g_held_item_icon_width);
    int32_t eax = ecx + 4;
    if (edx > 0) {
        eax -= edx;
        int32_t d = ecx >> 1;
        if (d < 8) { d = ecx; if (ecx > 8) d = 8; }
        if (eax < d) eax = d;
        else G32(VA_g_held_item_slide_timer) = (int32_t)((uint32_t)G32(VA_g_held_item_slide_timer) + (uint32_t)G32(VA_g_frame_time_scale));
        if ((uint32_t)ecx > (uint32_t)eax) ecx = eax;
        if (G8(VA_g_hires_line_doubling_flag) != 0) { eax >>= 1; ecx &= 0xfe; }
    } else {
        G32(VA_g_held_item_slide_timer) = (int32_t)((uint32_t)G32(VA_g_held_item_slide_timer) + (uint32_t)G32(VA_g_frame_time_scale));
        if (G8(VA_g_hires_line_doubling_flag) != 0) eax >>= 1;
    }
    if (eax <= 0 || ecx == 0) return;
    int32_t y = (int32_t)G32(VA_g_view_h) + (int32_t)G32(VA_g_view_y) - eax;
    int32_t x = (int32_t)G32(VA_g_view_w) + (int32_t)G32(VA_g_view_x) - ((int32_t)G32(VA_g_held_item_icon_spans + 0x6a0) + 4);
    uint8_t *ptr = screen_xy_to_framebuffer_ptr(x, y);
    inv_call(DRAW_TRANSLUCENT_SPANS, (uint32_t)(uintptr_t)ptr, 0x81310u + OBJ_DELTA, (uint32_t)G32(VA_g_screen_pitch), (uint32_t)ecx);
}

/* draw_inventory_tabs (0x1a2d2): draws the 5-tab strip (selected vs unselected DAS tab images from the
 * 0x712da table), building the tab hotspot list at 0x80b74, then registers the strip dirty rect. flow_succ
 * shared epilogue. Void. In-game (das). */
void draw_inventory_tabs(void)
{
    uint32_t esi = 0x712dau + OBJ_DELTA;                   /* das ids: [0]=unsel, [4]=sel */
    int32_t edi = 0;
    uint32_t ecx = 0x80b74u + OBJ_DELTA;                   /* hotspot list (stride 8) */
    int32_t i = 0;
    for (;;) {
        uint32_t cat = (uint8_t)G8(VA_g_inventory_tab_context_map + (uint32_t)edi);
        uint32_t das = (cat == (uint32_t)G32(VA_g_cursor_active_list)) ? *(uint32_t *)(uintptr_t)(esi + 4)
                                                       : *(uint32_t *)(uintptr_t)esi;
        int32_t x = (int32_t)(uint8_t)G8((VA_g_inventory_tab_context_map + 0x99) + (uint32_t)i) + (int32_t)G32(VA_g_ui_panel_anchor_x);
        int32_t y = (int32_t)G32(VA_g_ui_panel_anchor_y) + 0x38;
        *(uint16_t *)(uintptr_t)ecx = (uint16_t)x;
        *(uint16_t *)(uintptr_t)(ecx + 2) = (uint16_t)y;
        uint8_t *ptr = screen_xy_to_framebuffer_ptr(x, y);
        inv_blit_das((uint32_t)(uintptr_t)ptr, das);
        *(uint8_t *)(uintptr_t)(ecx + 4) = 0x13;
        edi++;
        *(uint8_t *)(uintptr_t)(ecx + 5) = 0x12;
        esi += 8;
        uint32_t w = (uint32_t)(uint8_t)G8((VA_g_ui_slot_layout_table_body + 0x2f) + (uint32_t)edi) + 0x17;
        ecx += 8;
        *(uint16_t *)(uintptr_t)(ecx - 2) = (uint16_t)w;
        i++;
        if (i >= 5) break;
    }
    *(uint16_t *)(uintptr_t)ecx = 0;
    inv_register_dirty_rect((uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_x) + 5), (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_y) + 0x38),
                            (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_x) + 0x69), (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_y) + 0x48));
}

/* select_inventory_tab (0x1a2b5): sets the active list category from the current tab ([0x80b70] ->
 * [0x7123c]), rebuilds the entry list (build_inventory_entry_list bridge), bumps the dirty flag, and
 * flow_succ-falls into draw_inventory_tabs [L]. Void. In-game. */
void select_inventory_tab(void)
{
    int32_t tab = (int32_t)G32(VA_g_inventory_active_tab);
    G32(VA_g_cursor_active_list) = (int32_t)(uint8_t)G8(VA_g_inventory_tab_context_map + (uint32_t)tab);
    inv_call(IV_BUILD_ENTRY_LIST, (uint32_t)G32(VA_g_cursor_active_list), 0, 0, 0);
    G8(VA_g_inventory_dirty_flags) = (uint8_t)(G8(VA_g_inventory_dirty_flags) + 2);
    draw_inventory_tabs();                          /* fall-through */
}

/* draw_inventory_entry_label (0x1c020): EAX = cursor entry index. Parses the item's weapon attrs
 * (apply_weapon_action_attributes bridge) for the count, formats the label (format_inventory_item_label
 * [L]), and draws the count panel element (draw_ui_panel_count_element bridge). flow_succ shared epilogue.
 * Void. In-game (text). */
void draw_inventory_entry_label(uint32_t eax_in)
{
    uint8_t attrs[0x50];                                   /* [ebp-0x50] */
    uint32_t esi = eax_in;
    uint32_t ecx = eax_in * 0xc;
    inv_call(0x18260u, *(uint32_t *)(uintptr_t)(ecx + 0x7fefcu + OBJ_DELTA),
             (uint32_t)(uintptr_t)attrs, 1, 0);            /* apply_weapon_action_attributes(eax=entry id) */
    uint32_t slot = *(uint32_t *)(uintptr_t)(ecx + 0x7fef8u + OBJ_DELTA);
    int32_t qty = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)(slot + 2);
    if (attrs[0x31] & 0x80) qty = (int32_t)((uint32_t)qty >> 8);   /* [ebp-0x1f] infinite ammo */
    uint32_t ent = esi * 0xc;
    inv_call(0x1c0b1u, *(uint32_t *)(uintptr_t)(ent + 0x7fefcu + OBJ_DELTA), (uint32_t)qty, 0, 0); /* format label */
    inv_call(DRAW_UI_PANEL_COUNT, 0, 0, 0, 0);             /* draw_ui_panel_count_element */
}

/* refresh_inventory_grid (0x1c469): if the grid is dirty ([0x7f571]&2), register the grid dirty rect,
 * re-render the grid (render_inventory_grid bridge), and redraw either the selected entry's label
 * (draw_inventory_entry_label bridge) or a scrollbar DAS tile. Void. In-game. */
void refresh_inventory_grid(void)
{
    if (!(G8(VA_g_inventory_dirty_flags) & 2)) return;
    uint32_t list = (uint32_t)G32(VA_g_cursor_active_list);
    int32_t cy = (int32_t)G32(VA_g_ui_panel_anchor_y), cx = (int32_t)G32(VA_g_ui_panel_anchor_x);
    uint32_t esi = (uint32_t)G32(VA_g_cursor_list_positions + list * 4);
    G8(VA_g_inventory_dirty_flags) = (uint8_t)(G8(VA_g_inventory_dirty_flags) - 2);
    inv_register_dirty_rect((uint32_t)cx, (uint32_t)cy, (uint32_t)(cx + 0x11f), (uint32_t)(cy + 0x4f));
    inv_call(IV_RENDER_GRID, 0, 0, 0, 0);
    int32_t rel = (int32_t)esi - (int32_t)G32(VA_g_cursor_scroll_offsets + (uint32_t)G32(VA_g_cursor_active_list) * 4);
    if ((uint32_t)rel < (uint32_t)G32(VA_g_cursor_entry_count)) {
        inv_call(IV_DRAW_ENTRY_LABEL, esi, 0, 0, 0);
        return;
    }
    int32_t x = (int32_t)G32(VA_g_selected_item_primary + 0x14) + cx - 2;
    int32_t y = cy + 2;
    uint8_t *ptr = screen_xy_to_framebuffer_ptr(x, y);
    inv_blit_das((uint32_t)(uintptr_t)ptr, 0xc8);
    G32(VA_g_inventory_arrow_state + 0x4) = 0;
}

/* redraw_inventory_cursor_cell (0x1a178): redraws the currently-highlighted grid cell's icon with the
 * frame-tick-animated highlight color (held/equipped/selected overrides). Calls refresh_inventory_grid +
 * draw_item_icon_in_slot (bridges). Void. In-game. */
void redraw_inventory_cursor_cell(void)
{
    if (G32(VA_g_ui_panel_anchor_y + 0x4) != 0) return;
    inv_call(0x1c469u, 0, 0, 0, 0);                        /* refresh_inventory_grid */
    uint32_t list4 = (uint32_t)G32(VA_g_cursor_active_list) * 4;
    int32_t pos = (int32_t)G32(VA_g_cursor_list_positions + list4);
    uint32_t ent = (uint32_t)pos * 0xc + 0x7fef4u + OBJ_DELTA;
    if ((uint32_t)pos >= (uint32_t)G32(VA_g_cursor_entry_count)) return;
    int32_t rel = pos - (int32_t)G32(VA_g_cursor_scroll_offsets + list4);
    if ((uint32_t)rel > 9) return;
    if (*(uint32_t *)(uintptr_t)ent == 0) return;
    int32_t tick = (int32_t)(int16_t)((uint16_t)G16(VA_g_frame_tick_counter) & 0x1f);   /* bh=0; bl&=0x1f */
    uint32_t color = (uint32_t)G32((VA_g_inventory_tab_context_map + 0x19) + (uint32_t)tick * 4);
    if (ent == (uint32_t)G32(VA_g_current_cursor_entry)) color = (uint8_t)G8(VA_g_map_menu_marker_selected);
    else if (ent == (uint32_t)G32(VA_g_left_hand_item)) color = (uint8_t)G8(VA_g_default_message_color);
    if (ent == (uint32_t)G32(VA_g_right_hand_item)) color = (uint8_t)G8(VA_g_default_message_color + 0x5);
    uint32_t icon = *(uint32_t *)(uintptr_t)*(uint32_t *)(uintptr_t)ent;  /* [[ent]] */
    inv_call(0x19f34u, icon, (uint32_t)rel, color, 0);     /* draw_item_icon_in_slot */
}

/* refresh_inventory_panel (0x1bf7b): repaints the open inventory panel — rebuild list, refresh grid, draw
 * the two corner slot tiles + the equipped-left icon, then flow_succ-falls into draw_equipped_item_right [L].
 * Void. In-game. */
void refresh_inventory_panel(void)
{
    G8(VA_g_inventory_dirty_flags) = (uint8_t)(G8(VA_g_inventory_dirty_flags) | 2);
    inv_call(IV_BUILD_ENTRY_LIST, (uint32_t)G32(VA_g_cursor_active_list), 0, 0, 0);
    inv_call(0x1c469u, 0, 0, 0, 0);                        /* refresh_inventory_grid */
    inv_call(0x19ee6u, 0xa, 0, 0, 0);                      /* draw_panel_slot_tile(0xa) */
    inv_call(0x1a2efu, 0, 0, 0, 0);                        /* draw_equipped_item_left */
    inv_call(0x19ee6u, 0xb, 0, 0, 0);                      /* draw_panel_slot_tile(0xb) */
    draw_equipped_item_right();                     /* fall-through */
}

/* close_inventory_panel (0x1a7a1): tears down the open inventory screen — frees cursor icons, restores the
 * saved-under background (free_das_cache_handle + the panel snapshot pool handle), registers the dirty
 * rect, restores screen state (copy_word_90bcc_to_8532a [L]), refreshes the selected icon + weapon HUD.
 * Void. In-game (DAS/pool). */
void close_inventory_panel(void)
{
    if (G32(VA_g_inspect_popup_active) != 0) return;
    G32(VA_g_current_cursor_entry) = 0;
    if (G8(VA_g_inventory_dirty_flags) & 4) {
        G8(VA_g_inventory_dirty_flags) = (uint8_t)(G8(VA_g_inventory_dirty_flags) - 4);
        inv_call(0x19ca7u, 0, 0, 0, 0);                    /* free_cursor_entry_icons */
    }
    if (G8(VA_g_inventory_dirty_flags) & 1) {
        G8(VA_g_inventory_dirty_flags) = (uint8_t)(G8(VA_g_inventory_dirty_flags) - 1);
        G32(VA_g_ui_panel_scratch_handle + 0x4) = (int32_t)inv_call(FREE_DAS_CACHE_HANDLE, (uint32_t)G32(VA_g_ui_panel_scratch_handle + 0x4), 0, 0, 0);
        if (G32(VA_g_ui_panel_scratch_handle) != 0) {
            pool_free_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle),      /* re-point 0x360b3 */
                                    (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_ui_panel_scratch_handle));
            G32(VA_g_ui_panel_scratch_handle) = 0;
        }
        inv_register_dirty_rect((uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_x) - 4), (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_y) - 4),
                                (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_x) + 0x123), (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_y) + 0x53));
        inv_call(0x2ab21u, 0, 0, 0, 0);                    /* copy_word_90bcc_to_8532a [L] */
        G32(VA_g_inventory_panel_open) = 0;
    }
    G8(VA_g_player_movement_enabled) = 1;
    inv_call(0x1bb4bu, 0, 0, 0, 0);                        /* update_selected_item_icon */
    if (G32(VA_g_inspect_popup_state + 0x4) != 0) {
        G32(VA_g_inspect_popup_state + 0x4) = 0;
        inv_call(RENDER_WEAPON_HUD, 1, 0x811b4u + OBJ_DELTA, 0, 0);
    }
    G8(VA_g_cursor_secondary_action_flag) = 0;
    G8(VA_g_cursor_primary_action_flag) = 0;
}

/* update_inventory_screen (0x1a8e5): the per-frame inventory state machine (the subsystem's driver).
 * Dequeues one keyboard scancode (input_ring_dequeue [L]) and dispatches: number/Tab keys switch tabs
 * (select_inventory_tab), arrow keys move the grid cursor / scroll, Enter equips/selects (activate_weapon_item
 * / update_selected_item_icon), the use/hotspot keys select+commit the held item (commit_held_cursor_item),
 * a key opens the options menu ([0x7febc] -> run_options_menu, jump-table on the result: save/load/quit),
 * and a hovered cell loads its dbase300 doc (load_dbase300_resource_at_offset). Closes the panel
 * (close_inventory_panel) when [0x80b2c] is raised, else refreshes it. Void. LIVE-SWAP (input + non-idempotent
 * side effects); every callee bridged. */
void update_inventory_screen(void)
{
    G32(VA_g_inventory_ui_action) = 0;
    uint32_t io_eax = inv_call(0x1299au, 0, 0, 0, 0);      /* input_ring_dequeue -> EAX (scancode) */
    uint32_t edx = io_eax;
    uint32_t ax = io_eax & 0xffff;
    uint32_t dx = edx & 0xffff;

    int32_t new_tab = 0;                                   /* the CANDIDATE tab (EAX at 0x1a9cb) — not committed yet */
    if (ax != 0) {                                         /* 0x1ab3d dispatch */
        if (ax == 1 || ax == 0x17) goto L_sel;
        if (ax == 2) { new_tab = 0; goto L_tab; }
        if (ax == 3) { new_tab = 1; goto L_tab; }
        if (ax == 4) { new_tab = 2; goto L_tab; }
        if (ax == 5) { new_tab = 3; goto L_tab; }
        if (ax == 6) { new_tab = 4; goto L_tab; }
        if (ax == 0xf) { new_tab = (int32_t)(int16_t)(uint16_t)G16(VA_g_inventory_active_tab) + 1; goto L_tab; }   /* 0x1a9c4 mov ax,[0x80b70]; inc eax */
        if (ax == 0x16) { G32(VA_g_inventory_ui_action) = 2; goto L_post; }
        if (ax == 0x1c) {                                 /* Enter -> equip/select (0x1a912) */
            if (G32(VA_g_cursor_active_list) == 2) goto L_post;
            uint32_t pos = (uint32_t)G32(VA_g_cursor_list_positions + (uint32_t)G32(VA_g_cursor_active_list) * 4);
            if (G32(VA_g_cursor_active_list) == 1) {
                if (pos < (uint32_t)G32(VA_g_cursor_entry_count))
                    inv_call(0x184abu, (uint32_t)G32((VA_g_cursor_entry_table + 0x4) + pos * 0xc), (uint32_t)G32((VA_g_cursor_entry_table + 0x8) + pos * 0xc), 0, 0);
            } else if (pos < (uint32_t)G32(VA_g_cursor_entry_count)) {
                G32(VA_g_selected_item_primary) = (int32_t)G32((VA_g_cursor_entry_table + 0x4) + pos * 0xc);
                inv_call(0x1bb4bu, 0, 0, 0, 0);
            }
            goto L_sel;
        }
        if (ax == 0x1f) { G32(VA_g_inventory_ui_action) = 1; goto L_post; }
        if (ax == 0x20) { G32(VA_g_inventory_options_request) = 1; goto L_post; }
        if (ax == 0x30) { inv_call(0x11124u, 0, 0, 0, 0); goto L_post; }  /* check_snapshot_key */
        if (ax == 0x39) goto L_hover;
        if (dx == 0x4b) goto L_left;
        if (dx == 0x4d) goto L_right;
        if (dx == 0x50) goto L_pgdn;
        if (dx == 0x48) goto L_up;
        goto L_post;
    }
    goto L_post;

L_sel:                                                    /* 0x1a970 */
    if (inv_call(0x18a2au, 0, 0, 0, 0) != 0) goto L_post;  /* try_interrupt_dialogue_voice */
    G32(VA_g_ui_panel_anchor_y + 0x4) = (int32_t)((uint32_t)G32(VA_g_ui_panel_anchor_y + 0x4) + 1);
    inv_call(0x1a88bu, 0, 0, 0, 0);                        /* commit_held_cursor_item */
    goto L_post;

L_tab: {                                                  /* 0x1a9cb cwde */
    /* 0x1a9cc `cmp eax,[0x80b70]; je 0x1ad2a` — the CANDIDATE is compared BEFORE [0x80b70] is
     * written, and 0x1ad2a is the function EPILOGUE (pop x4; ret): pressing the already-active
     * tab's key does NOTHING (no icon free / DAS reload / redraw, and L_post is skipped too).
     * The old C committed the global first (vacuous compare) and always reloaded + ran L_post. */
    if (new_tab == (int32_t)G32(VA_g_inventory_active_tab)) return;   /* je 0x1ad2a (epilogue) */
    G32(VA_g_inventory_active_tab) = new_tab;             /* 0x1a9d8 commit */
    if (new_tab >= 5) G32(VA_g_inventory_active_tab) = 0; /* 0x1a9dd cmp eax,5; jl skip; wrap */
    inv_call(0x1a2b5u, 0, 0, 0, 0);                        /* select_inventory_tab */
    goto L_post;
}
L_left: {                                                 /* 0x1a9f6 cursor left / scroll up */
    uint32_t l4 = (uint32_t)G32(VA_g_cursor_active_list) * 4;
    if ((int32_t)G32(VA_g_cursor_list_positions + l4) <= 0) goto L_post;
    G32(VA_g_cursor_list_positions + l4) = (int32_t)((uint32_t)G32(VA_g_cursor_list_positions + l4) - 1);
    if ((int32_t)G32(VA_g_cursor_list_positions + l4) < (int32_t)G32(VA_g_cursor_scroll_offsets + l4))
        G32(VA_g_cursor_scroll_offsets + l4) = (int32_t)((uint32_t)G32(VA_g_cursor_scroll_offsets + l4) - 5);
    G8(VA_g_inventory_dirty_flags) = (uint8_t)(G8(VA_g_inventory_dirty_flags) + 2);
    goto L_post;
}
L_right: {                                                /* 0x1aa2a cursor right / scroll down */
    uint32_t l4 = (uint32_t)G32(VA_g_cursor_active_list) * 4;
    if ((uint32_t)((int32_t)G32(VA_g_cursor_entry_count) - 1) <= (uint32_t)G32(VA_g_cursor_list_positions + l4)) goto L_post;
    G32(VA_g_cursor_list_positions + l4) = (int32_t)((uint32_t)G32(VA_g_cursor_list_positions + l4) + 1);
    if ((int32_t)G32(VA_g_cursor_scroll_offsets + l4) + 9 < (int32_t)G32(VA_g_cursor_list_positions + l4) &&
        (uint32_t)((int32_t)G32(VA_g_cursor_entry_count) - 9) > (uint32_t)G32(VA_g_cursor_scroll_offsets + l4))
        G32(VA_g_cursor_scroll_offsets + l4) = (int32_t)((uint32_t)G32(VA_g_cursor_scroll_offsets + l4) + 5);
    G8(VA_g_inventory_dirty_flags) = (uint8_t)(G8(VA_g_inventory_dirty_flags) + 2);
    goto L_post;
}
L_up: {                                                   /* 0x1aaf1 page up */
    uint32_t l4 = (uint32_t)G32(VA_g_cursor_active_list) * 4;
    if ((int32_t)G32(VA_g_cursor_list_positions + l4) >= 5) G32(VA_g_cursor_list_positions + l4) = (int32_t)((uint32_t)G32(VA_g_cursor_list_positions + l4) - 5);
    if ((int32_t)G32(VA_g_cursor_list_positions + l4) < (int32_t)G32(VA_g_cursor_scroll_offsets + l4))
        G32(VA_g_cursor_scroll_offsets + l4) = (int32_t)((uint32_t)G32(VA_g_cursor_scroll_offsets + l4) - 5);
    G8(VA_g_inventory_dirty_flags) = (uint8_t)(G8(VA_g_inventory_dirty_flags) + 2);
    goto L_post;
}
L_pgdn: {                                                 /* 0x1aa6e page down (clamp to last row) */
    uint32_t rem = (uint32_t)G32(VA_g_cursor_entry_count) % 5; int32_t e = (rem != 0) ? (int32_t)rem : 5;
    int32_t lim = (5 - e) + (int32_t)G32(VA_g_cursor_entry_count);
    uint32_t l4 = (uint32_t)G32(VA_g_cursor_active_list) * 4;
    int32_t np = (int32_t)G32(VA_g_cursor_list_positions + l4) + 5;
    if (np < lim) G32(VA_g_cursor_list_positions + l4) = np;
    if ((uint32_t)G32(VA_g_cursor_list_positions + l4) >= (uint32_t)G32(VA_g_cursor_entry_count))
        G32(VA_g_cursor_list_positions + l4) = (int32_t)G32(VA_g_cursor_entry_count) - 1;
    if ((int32_t)G32(VA_g_cursor_scroll_offsets + l4) + 9 < (int32_t)G32(VA_g_cursor_list_positions + l4) &&
        (uint32_t)((int32_t)G32(VA_g_cursor_entry_count) - 9) > (uint32_t)G32(VA_g_cursor_scroll_offsets + l4))
        G32(VA_g_cursor_scroll_offsets + l4) = (int32_t)((uint32_t)G32(VA_g_cursor_scroll_offsets + l4) + 5);
    G8(VA_g_inventory_dirty_flags) = (uint8_t)(G8(VA_g_inventory_dirty_flags) + 2);
    goto L_post;
}
L_hover: {                                                /* 0x1ab0e: set the hovered entry */
    uint32_t l4 = (uint32_t)G32(VA_g_cursor_active_list) * 4;
    uint32_t p = (uint32_t)G32(VA_g_cursor_list_positions + l4);
    if (p >= (uint32_t)G32(VA_g_cursor_entry_count)) goto L_post;
    G32(VA_g_inventory_inspect_request) = (int32_t)(p * 0xc + 0x7fef4u + OBJ_DELTA);
    goto L_post;
}

L_post:                                                   /* 0x1ac1d */
    if (G32(VA_g_inventory_options_request) != 0) {
        G32(VA_g_inventory_options_request) = 0;
        uint32_t r = inv_call(0x26501u, 0, 0, 0, 0);       /* run_options_menu -> result */
        uint32_t redx = r;
        if (r != 0) {
            uint32_t ebx = (r & 0xffff) - 1;
            if (ebx <= 5) {
                int32_t resv = (int32_t)((uint32_t)G32(VA_g_ui_panel_anchor_y + 0x4) + 1);
                int32_t shi = (int32_t)((uint32_t)redx >> 0x10);
                switch (ebx) {                             /* jump table 0x1a8cd */
                case 0:  G16(VA_g_screen_resolution_index + 0x8) = 0x64; G32(VA_g_ui_panel_anchor_y + 0x4) = resv; break;
                case 1:  inv_call(0x21dc6u, (uint32_t)shi, 0, 0, 0); break;  /* write_savegame_file */
                case 2:  G32(VA_g_pending_game_action + 0x4) = 1; G8(VA_g_pending_game_action) |= 2; G32(VA_g_pending_game_action + 0x8) = (int32_t)shi;
                         G32(VA_g_ui_panel_anchor_y + 0x4) = resv; break;       /* handler C: no [0x7f360] write */
                case 3:  G8(VA_g_pending_game_action) |= 4; G32(VA_g_ui_panel_anchor_y + 0x4) = resv; break;   /* handler D: no [0x7f360] */
                case 4:  break;                            /* 0x1acb0 (no-op continue) */
                case 5:  G16(VA_g_screen_resolution_index + 0x8) = 1; G8(VA_g_pending_game_action) |= 8; G32(VA_g_ui_panel_anchor_y + 0x4) = resv; break;  /* handler E: [0x7f360]=1 */
                }
            }
        }
    }
    /* orig `movswl (eax),esi` at 1acb0 derefs [0x81044]; after picking an item onto the cursor that slot
     * ptr is 0. DOS lets *0 read low memory (result only feeds the change-compare); the host has no null
     * page -> guard it. Sentinel 0x7fffffff can't equal a valid int16 id, so null==null => no refresh. */
    uint32_t sel_ptr0 = (uint32_t)G32(VA_g_selected_item_primary);
    int32_t prev_sel = sel_ptr0 ? (int32_t)(int16_t)*(uint16_t *)(uintptr_t)sel_ptr0 : 0x7fffffff;
    if (G32(VA_g_inventory_inspect_request) != 0) {                               /* 0x1acb8: load the hovered cell's doc */
        uint32_t hov = (uint32_t)G32(VA_g_inventory_inspect_request);
        uint32_t recoff = *(uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_dbase100_inventory_table) +
                              *(uint32_t *)(uintptr_t)(hov + 8) * 4);
        uint32_t rec = (uint32_t)G32(VA_g_dbase100_base) + recoff;
        uint32_t arg = *(uint32_t *)(uintptr_t)(rec + 8) << 3;
        if (arg != 0)
            inv_call(0x196b9u, arg + 4, *(uint32_t *)(uintptr_t)(rec + 4) & 1, rec, hov);  /* eax=off, edx=flag, ebx=rec, ecx=hov */
        G32(VA_g_inventory_inspect_request) = 0;
    }
    if (G32(VA_g_ui_panel_anchor_y + 0x4) != 0 || G32(VA_g_inventory_panel_open) == 0) {
        inv_call(0x1a7a1u, 0, 0, 0, 0);                    /* close_inventory_panel */
        return;
    }
    uint32_t sel_ptr1 = (uint32_t)G32(VA_g_selected_item_primary);
    int32_t cur_sel = sel_ptr1 ? (int32_t)(int16_t)*(uint16_t *)(uintptr_t)sel_ptr1 : 0x7fffffff;
    if (cur_sel != prev_sel)
        inv_call(0x1bf7bu, 0, 0, 0, 0);                    /* refresh_inventory_panel */
}

/* render_inventory_panel (0x1a399): the full inventory-screen compositor (opens the panel). Sets screen
 * state, saves the background-under region + allocates the panel snapshot pool buffer, draws the panel
 * frame + slot tiles + the equipped/held item icons + the title text + the tab strip + the item grid +
 * the selected label, and stores all the UI hotspot rectangles (0x80ba4..0x80bc2). No-op if already open
 * ([0x7f571]&1) or no dbase ([0x81e1c]==0). flow_succ shared epilogue. Void. In-game (the biggest UI fn;
 * every callee bridged). */
void render_inventory_panel(void)
{
    uint8_t loc[0x64];                                     /* enter 0x64: title text + flag locals */
    G8(VA_g_cursor_primary_action_flag) = 0; G8(VA_g_cursor_secondary_action_flag) = 0; G8(VA_g_mouse_relative_mode) = 0;
    if (G8(VA_g_inventory_dirty_flags) & 1) return;                           /* already open */
    inv_call(0x167d7u, 0, 0, 0, 0);                        /* prepare screen mode */
    inv_call(0x1a37bu, 0, 0, 0, 0);                        /* recompute panel origin (0x80b24/0x80b28) */
    G32(VA_g_inventory_inspect_request) = 0;
    if (G32(VA_g_dbase100_base) == 0) return;                         /* no dbase */
    G32(VA_g_inventory_panel_open + 0x8) = 0; G32(VA_g_ui_panel_anchor_y + 0x4) = 0;
    inv_call(FLUSH_OBJECT_DAS, 0, 0, 0, 0);                /* 0x26cd4 */
    G32(VA_g_inventory_arrow_state + 0x4) = (int32_t)0xffffffff;
    G8(VA_g_player_movement_enabled) = 3;
    G32(VA_g_inventory_panel_open) = 1;
    if (G32(VA_g_ui_panel_scratch_handle) == 0) {                               /* allocate panel snapshot buffer */
        inv_call(0x414d2u, 0x1130, 0x1130, 0, 0);          /* ensure_das_cache_heap_space */
        G32(VA_g_ui_panel_scratch_handle) = (int32_t)inv_call(0x360f9u, (uint32_t)G32(VA_g_das_cache_heap_handle), 0x1130, 0, 0);  /* pool_alloc_handle(pool, EDX=size).
                             * Original stages `mov edx,0x1130` @0x1a427 BEFORE the ensure call and the alloc
                             * consumes the leftover EDX (the same idiom write_savegame_file/bundle document).
                             * The old edx=0 ALWAYS failed (0x360f9 head: test edx,edx; je fail) -> the panel
                             * snapshot handle [0x811ac] never allocated -> no thumbnail capture from here. */
    }
    if (G32(VA_g_ui_panel_scratch_handle) != 0)
        inv_call(0x142b7u, *(uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_ui_panel_scratch_handle), 0, 0, 0);  /* init snapshot */
    { uint32_t c = (uint8_t)G8(VA_g_default_message_color); uint32_t col = c | (c << 8);
      inv_call(0x12b45u, col, 0x128, 0x58, 0); }           /* grayscale_background_view (ecx=0x58,ebx=0x128) */
    /* save_framebuffer_region 0x13062 direct-C: EAX=x,EDX=y,EBX=w=0x128,ECX=h=0x58 (w/h
     * leftover from the 0x12b45 grayscale call). No oracle test exercises render_inventory_panel (in-game
     * orchestrator); in-game the lifted call reads the real fb + DAS pool. */
    G32(VA_g_ui_panel_scratch_handle + 0x4) = (int32_t)save_framebuffer_region((uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_x) - 4),
                                     (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_y) - 4), 0x128, 0x58, NULL);
    inv_register_dirty_rect((uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_x) - 4), (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_y) - 4),
                            (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_x) + 0x123), (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_y) + 0x53));
    inv_call(0x12ddeu, (uint32_t)G32(VA_g_ui_panel_anchor_x), (uint32_t)G32(VA_g_ui_panel_anchor_y), 0x120, 0x50);   /* blit_clipped_sprite_smc */
    inv_blit_das((uint32_t)(uintptr_t)screen_xy_to_framebuffer_ptr((int32_t)G32(VA_g_ui_panel_anchor_x), (int32_t)G32(VA_g_ui_panel_anchor_y)), 0x10);                                  /* panel frame */
    inv_call(0x19ee6u, 0xa, 0, 0, 0);
    inv_call(0x19ee6u, 0xb, 0, 0, 0);
    inv_call(0x19ee6u, 0xc, 0, 0, 0);
    G32(VA_g_inventory_arrow_state) = 0;
    inv_call(IV_BUILD_ENTRY_LIST, (uint32_t)G32(VA_g_cursor_active_list), 0, 0, 0);
    G32(VA_g_current_cursor_entry) = 0; G32(VA_g_inspect_popup_state) = 0; G32(VA_g_inventory_panel_open + 0x8) = 0;
    G32(VA_g_selected_item_primary + 0x14) = 0x7b;
    /* UI hotspot rectangles (0x80ba4..) */
    G16(VA_g_inventory_active_tab + 0x34) = (uint16_t)((uint16_t)G16(VA_g_ui_panel_anchor_x) + 4); G8(VA_g_inventory_active_tab + 0x38) = 0x1c; G8(VA_g_inventory_active_tab + 0x39) = 0x1c;
    G16(VA_g_inventory_active_tab + 0x36) = (uint16_t)((uint16_t)G16(VA_g_ui_panel_anchor_y) + 4); G16(VA_g_inventory_active_tab + 0x3a) = 0x26;
    G16(VA_g_inventory_active_tab + 0x3c) = (uint16_t)((uint16_t)G16(VA_g_ui_panel_anchor_x) + 0x3a); G8(VA_g_inventory_active_tab + 0x40) = 0x1c; G8(VA_g_inventory_active_tab + 0x41) = 0x1c;
    G16(VA_g_inventory_active_tab + 0x3e) = (uint16_t)((uint16_t)G16(VA_g_ui_panel_anchor_y) + 0xf); G16(VA_g_inventory_active_tab + 0x46) = (uint16_t)((uint16_t)G16(VA_g_ui_panel_anchor_y) + 0xf);
    G8(VA_g_inventory_active_tab + 0x48) = 0x1c; G8(VA_g_inventory_active_tab + 0x49) = 0x1c;
    G16(VA_g_inventory_active_tab + 0x44) = (uint16_t)((uint16_t)G16(VA_g_ui_panel_anchor_x) + 0x59);
    G8(VA_g_inventory_dirty_flags) = (uint8_t)(G8(VA_g_inventory_dirty_flags) + 4);
    inv_call(0x1a2efu, 0, 0, 0, 0);                        /* draw_equipped_item_left */
    inv_call(0x1bfaau, 0, 0, 0, 0);                        /* draw_equipped_item_right */
    if (G32(VA_g_selected_item_primary + 0xc) != 0) {                               /* a held item -> draw it + its name */
        uint32_t held_icon = (uint32_t)G32(VA_g_active_item_hud_icon);
        if (held_icon != 0) {
            uint32_t ic = *(uint32_t *)(uintptr_t)held_icon;
            int32_t h = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)(ic + 6);
            int32_t y = ((int32_t)(0x38 - h) >> 2) + (int32_t)G32(VA_g_ui_panel_anchor_y) + 4;   /* lea edx,0x4(eax): +4 inset */
            int32_t w = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)(ic + 4);
            int32_t x = ((int32_t)(0x1c - w) >> 1) + (int32_t)G32(VA_g_ui_panel_anchor_x) + 4;
            /* ecx = the icon-blit MODE (leftover from 1a5f1, preserved through draw_equipped_*): (widescreen?1:2).
             * ecx low byte 0 would take blit_item_icon's double-write path (row advance pitch+width) -> smear. */
            inv_call(0x13544u, ic, (uint32_t)(uintptr_t)screen_xy_to_framebuffer_ptr(x, y),
                     (uint32_t)G32(VA_g_screen_pitch), (uint32_t)(G8(VA_g_hires_line_doubling_flag) == 0 ? 2 : 1));
        }
        uint32_t txt = *(uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_selected_item_primary + 0xc) + 0x10);
        if (txt != 0 && inv_call(READ_NEXT_DIALOGUE_LINE, (uint32_t)(uintptr_t)(loc + 2), 0x62, txt, 0) != 0) {
            loc[0] = 1; loc[1] = (uint8_t)G8(VA_g_map_menu_marker_selected);
            inv_call(0x1a079u, (uint32_t)(uintptr_t)loc, 0,
                     (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_x) + 0x23),
                     (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_y) + 2)); /* draw_text_at_screen_xy(EAX=str,EBX=x,ECX=y,EDX=0) */
            int32_t wdt = (int32_t)inv_call(0x1f91fu, (uint32_t)(uintptr_t)(loc + 2), 0, 0, 0) + 0x28;
            if (wdt > 0x7b) G32(VA_g_selected_item_primary + 0x14) = wdt;
        }
    }
    inv_blit_das((uint32_t)(uintptr_t)screen_xy_to_framebuffer_ptr((int32_t)G32(VA_g_ui_panel_anchor_x) + 0x23, (int32_t)G32(VA_g_ui_panel_anchor_y) + 0xe), 0x78);
    inv_blit_das((uint32_t)(uintptr_t)screen_xy_to_framebuffer_ptr((int32_t)G32(VA_g_ui_panel_anchor_x) + 5, (int32_t)G32(VA_g_ui_panel_anchor_y) + 0x24), 0x18);
    G16(VA_g_inventory_active_tab + 0x4e) = (uint16_t)((uint16_t)G16(VA_g_ui_panel_anchor_y) + 0x23); G8(VA_g_inventory_active_tab + 0x50) = 0x13; G8(VA_g_inventory_active_tab + 0x51) = 0x13;
    G16(VA_g_inventory_active_tab + 0x52) = 0x2c; G16(VA_g_inventory_active_tab + 0x4c) = (uint16_t)((uint16_t)G16(VA_g_ui_panel_anchor_x) + 4);
    inv_call(0x1a2d2u, 0, 0, 0, 0);                        /* draw_inventory_tabs */
    inv_call(IV_RENDER_GRID, 0, 0, 0, 0);                  /* render_inventory_grid */
    uint32_t pos = (uint32_t)G32(VA_g_cursor_list_positions + (uint32_t)G32(VA_g_cursor_active_list) * 4);
    if (pos < (uint32_t)G32(VA_g_cursor_entry_count)) {
        inv_call(IV_DRAW_ENTRY_LABEL, pos, 0, 0, 0);       /* draw_inventory_entry_label(eax=pos) */
    } else {
        int32_t x = (int32_t)G32(VA_g_selected_item_primary + 0x14) + (int32_t)G32(VA_g_ui_panel_anchor_x) - 2;
        int32_t y = (int32_t)G32(VA_g_ui_panel_anchor_y) + 2;
        inv_blit_das((uint32_t)(uintptr_t)screen_xy_to_framebuffer_ptr(x, y), 0xc8);
        G32(VA_g_inventory_arrow_state + 0x4) = 0;
    }
    G8(VA_g_inventory_dirty_flags) = (uint8_t)(G8(VA_g_inventory_dirty_flags) + 1);
}

/* render_inventory_grid (0x1c163): draws the 10-cell item grid — per cell the slot-tile background
 * (draw_panel_slot_tile bridge) + the item icon with selection-tinted color (draw_item_icon_in_slot
 * bridge), building the cell hotspot list at 0x80bc4 — then the up/down scroll arrows (DAS 0xa8/0xb0/
 * 0xb8/0xc0 via blit_reloc_das_image) + their hotspots, gated on the scroll position + [0x811a0] flags.
 * flow_succ shared epilogue. Void. In-game (das). */
void render_inventory_grid(void)
{
    int32_t base = (int32_t)G32(VA_g_cursor_scroll_offsets + (uint32_t)G32(VA_g_cursor_active_list) * 4);   /* [ebp-4] scroll base */
    uint32_t ecx = (uint32_t)base * 0xc + 0x7fef4u + OBJ_DELTA;
    int32_t edi = 0;
    uint32_t esi = 0x80bc4u + OBJ_DELTA;                   /* hotspot list */
    int32_t i8 = 0;                                        /* [ebp-8] */
    for (;;) {
        inv_call(0x19ee6u, (uint32_t)edi, 0, 0, 0);        /* draw_panel_slot_tile(edi) */
        if (base < (int32_t)G32(VA_g_cursor_entry_count)) {
            if (*(uint32_t *)(uintptr_t)ecx != 0) {        /* non-empty entry */
                uint32_t color = 0;
                if (ecx == (uint32_t)G32(VA_g_left_hand_item)) color = (uint8_t)G8(VA_g_default_message_color);
                else if (ecx == (uint32_t)G32(VA_g_right_hand_item)) color = (uint8_t)G8(VA_g_default_message_color + 0x5);
                else if (ecx == (uint32_t)G32(VA_g_current_cursor_entry)) color = (uint8_t)G8(VA_g_map_menu_marker_selected);
                uint32_t icon = *(uint32_t *)(uintptr_t)*(uint32_t *)(uintptr_t)ecx;   /* [[ecx]] */
                inv_call(0x19f34u, icon, (uint32_t)edi, color, 0);   /* draw_item_icon_in_slot */
                *(uint16_t *)(uintptr_t)esi = (uint16_t)((uint16_t)G16(VA_g_ui_panel_anchor_x) + (uint16_t)G16(VA_g_ui_slot_layout_table + (uint32_t)i8));
                esi += 8;
                *(uint16_t *)(uintptr_t)(esi - 6) = (uint16_t)((uint16_t)G16(VA_g_ui_panel_anchor_y) + (uint16_t)G16(VA_g_ui_slot_layout_table_ext + (uint32_t)i8));
                *(uint8_t *)(uintptr_t)(esi - 4) = 0x1c;
                *(uint8_t *)(uintptr_t)(esi - 3) = 0x1c;
                *(uint16_t *)(uintptr_t)(esi - 2) = (uint16_t)(edi + 0x1c);
            }
            ecx += 0xc;                                    /* drawn or empty -> advance */
        }
        base++; edi++; i8 += 4;
        if (edi >= 0xa) break;
    }
    /* up-scroll arrow (0x1c239) */
    int32_t ax = (int32_t)G32(VA_g_ui_panel_anchor_x) + 0x116, ay = (int32_t)G32(VA_g_ui_panel_anchor_y) + 0x12;
    if ((int32_t)G32(VA_g_cursor_scroll_offsets + (uint32_t)G32(VA_g_cursor_active_list) * 4) > 0) {
        if (!(G8(VA_g_inventory_arrow_state) & 1)) {
            G32(VA_g_inventory_arrow_state) = (int32_t)((uint32_t)G32(VA_g_inventory_arrow_state) + 1);
            inv_blit_das((uint32_t)(uintptr_t)screen_xy_to_framebuffer_ptr(ax, ay), 0xa8);
            inv_register_dirty_rect((uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_x) + 0x116), (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_y) + 0x12),
                                    (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_x) + 0x11c), (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_y) + 0x29));
        }
        *(uint16_t *)(uintptr_t)esi = (uint16_t)((int32_t)G32(VA_g_ui_panel_anchor_x) + 0x115);
        esi += 8;
        *(uint16_t *)(uintptr_t)(esi - 6) = (uint16_t)((int32_t)G32(VA_g_ui_panel_anchor_y) + 0x12);
        *(uint8_t *)(uintptr_t)(esi - 4) = 9; *(uint8_t *)(uintptr_t)(esi - 3) = 0x18;
        *(uint16_t *)(uintptr_t)(esi - 2) = 0x28;
    } else if (G8(VA_g_inventory_arrow_state) & 1) {
        G32(VA_g_inventory_arrow_state) = (int32_t)((uint32_t)G32(VA_g_inventory_arrow_state) - 1);
        inv_blit_das((uint32_t)(uintptr_t)screen_xy_to_framebuffer_ptr(ax, ay), 0xb0);
        inv_register_dirty_rect((uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_x) + 0x116), (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_y) + 0x12),
                                (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_x) + 0x11c), (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_y) + 0x29));
    }
    /* down-scroll arrow (0x1c332): visible when scroll base + 10 < total entries */
    uint32_t rem = (uint32_t)G32(VA_g_cursor_entry_count) % 5; int32_t edxv = (rem != 0) ? (int32_t)rem : 5;
    int32_t thresh = (5 - edxv) + (int32_t)G32(VA_g_cursor_entry_count) - 0xa;
    int32_t bx2 = (int32_t)G32(VA_g_ui_panel_anchor_x) + 0x116, by2 = (int32_t)G32(VA_g_ui_panel_anchor_y) + 0x31;
    if (thresh > (int32_t)G32(VA_g_cursor_scroll_offsets + (uint32_t)G32(VA_g_cursor_active_list) * 4)) {
        if (!(G8(VA_g_inventory_arrow_state) & 2)) {
            G32(VA_g_inventory_arrow_state) = (int32_t)((uint32_t)G32(VA_g_inventory_arrow_state) + 2);
            inv_blit_das((uint32_t)(uintptr_t)screen_xy_to_framebuffer_ptr(bx2, by2), 0xb8);
            inv_register_dirty_rect((uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_x) + 0x116), (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_y) + 0x31),
                                    (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_x) + 0x11c), (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_y) + 0x48));
        }
        *(uint16_t *)(uintptr_t)esi = (uint16_t)((int32_t)G32(VA_g_ui_panel_anchor_x) + 0x115);
        esi += 8;
        *(uint16_t *)(uintptr_t)(esi - 6) = (uint16_t)((int32_t)G32(VA_g_ui_panel_anchor_y) + 0x31);
        *(uint8_t *)(uintptr_t)(esi - 4) = 9; *(uint8_t *)(uintptr_t)(esi - 3) = 0x18;
        *(uint16_t *)(uintptr_t)(esi - 2) = 0x29;
    } else if (G8(VA_g_inventory_arrow_state) & 2) {
        G32(VA_g_inventory_arrow_state) = (int32_t)((uint32_t)G32(VA_g_inventory_arrow_state) - 2);
        inv_blit_das((uint32_t)(uintptr_t)screen_xy_to_framebuffer_ptr(bx2, by2), 0xc0);
        inv_register_dirty_rect((uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_x) + 0x116), (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_y) + 0x31),
                                (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_x) + 0x11c), (uint32_t)((int32_t)G32(VA_g_ui_panel_anchor_y) + 0x48));
    }
    *(uint16_t *)(uintptr_t)esi = 0;
}

/* --- blit_item_icon (0x13544): the core icon blit primitive. EAX = icon descriptor, EDX = dest ptr,
 * EBX = dest pitch, ECX = mode/color (low byte = sub-mode 0/1/2, bits 8-23 = colormap color). Decodes the
 * icon's per-row RLE stream (the inline 0x1374f decoder -> scratch) and blits it (skipping 0 = transparent)
 * in one of 6 modes: plain / dither (alternate rows) / doubled (write a 2nd copy at +pitch) x {raw,
 * colormap-remapped}. Pure given a staged dest buffer -> oracle. The scratch buffer + per-field state mirror
 * the original's dynamically-sized stack frame. */
struct blit_icon_ctx {
    uint8_t *base, *rd, *src;     /* [ebp+0] scratch base, [ebp+4] read ptr, [ebp+0x14] RLE src */
    int32_t  remain, rows, width, stride, width2, total;  /* [ebp+8/0xc/0x10/0x1c/0x20/0x24] */
};

/* colormap lookup via the `mov bl,[esi]; mov bl,[ebx]` low-byte-replace idiom: the pixel replaces the
 * low byte of the colormap pointer (the maps are 256-aligned), so cmap[px] = *((cmap & ~0xff) | px). */
static inline uint8_t cmap_lut(uint8_t *cmap, uint8_t px)
{
    return *(uint8_t *)(uintptr_t)(((uint32_t)(uintptr_t)cmap & 0xffffff00u) | (uint32_t)px);
}

/* inline RLE row decoder (0x1374f): tops up the scratch ring to 0x320 bytes from the RLE src. */
static void blit_icon_decode(struct blit_icon_ctx *c)
{
    memmove(c->base, c->rd, (size_t)c->remain);            /* compact remaining to base */
    c->rd = c->base;
    uint8_t *edi = c->base + c->remain;
    uint8_t *esi = c->src;
    int32_t ebx = 0x320 - c->remain;                       /* space to fill */
    if ((uint32_t)ebx > (uint32_t)c->total) ebx = c->total; /* 0x13772 jbe: UNSIGNED min */
    c->remain += ebx;
    c->total  -= ebx;
    for (;;) {                                             /* 0x13780: decode loop entered unconditionally */
        uint8_t al = *esi++;
        if (al < 0xf1) {                                   /* literal byte */
            *edi++ = al;
            if (--ebx > 0) continue;                       /* dec ebx; jg */
            break;
        }
        int32_t run = al - 0xf0;                           /* run of 1..15 */
        ebx -= run;
        uint8_t v = *esi++;
        while (run-- > 0) *edi++ = v;                      /* rep stosb */
        if (ebx > 0) continue;                             /* or ebx,ebx; jg */
        break;
    }
    c->remain -= ebx;                                      /* 0x137a0: ebx<=0 -> give the overshoot back */
    c->total  += ebx;                                      /*   (sub [ebp+8],ebx / add [ebp+0x24],ebx) */
    c->src = esi;
}

void blit_item_icon(uint32_t eax_in, uint32_t edx_in, uint32_t ebx_in, uint32_t ecx_in)
{
    uint8_t  scratch[0x340];
    struct blit_icon_ctx c;
    uint8_t *esi = (uint8_t *)(uintptr_t)eax_in;            /* icon descriptor */
    uint8_t *edi = (uint8_t *)(uintptr_t)edx_in;            /* dest */
    int32_t  width = *(uint16_t *)(esi + 4);
    uint32_t color = ecx_in & 0xffff00u;                   /* [ebp+0x2c] */
    int32_t  mode  = (int32_t)(ecx_in & 0xff);             /* [ebp+0x28] */
    c.base = scratch; c.rd = scratch;
    c.remain = 0;
    c.rows  = *(uint16_t *)(esi + 6);
    c.width = width;
    uint8_t flags = esi[0];
    if (flags & 4) esi += 8;
    if (!(flags & 2)) esi += 0x300;
    c.width2 = width;
    c.stride = ebx_in - (uint32_t)width;                   /* pitch - width (sub;neg) */
    c.total  = width * c.rows;
    esi += 8;
    c.src = esi;

    if (color == 0) {                                      /* raw modes */
        if (mode == 2) {                                   /* 0x13605 dither (alternate rows) */
            int dl = 0;
            for (; c.rows > 0; c.rows--) {
                if (c.remain < c.width) blit_icon_decode(&c);
                uint8_t *p = c.rd; int32_t ecx = c.width; c.remain -= ecx; c.rd += ecx;
                if (!dl) { for (; ecx > 0; ecx--) { uint8_t al = *p++; if (al) *edi = al; edi++; } edi += c.stride; }
                dl = !dl;
            }
        } else if (mode == 0) {                            /* 0x1363d doubled */
            int32_t ebx = c.width2;
            for (; c.rows > 0; c.rows--) {
                if (c.remain < c.width) blit_icon_decode(&c);
                uint8_t *p = c.rd; int32_t ecx = c.width; c.remain -= ecx; c.rd += ecx;
                for (; ecx > 0; ecx--) { uint8_t al = *p++; if (al) { edi[0] = al; edi[ebx] = al; } edi++; }
                edi += ebx; edi += c.stride;
            }
        } else {                                           /* 0x135d2 plain */
            for (; c.rows > 0; c.rows--) {
                if (c.remain < c.width) blit_icon_decode(&c);
                uint8_t *p = c.rd; int32_t ecx = c.width; c.remain -= ecx; c.rd += ecx;
                for (; ecx > 0; ecx--) { uint8_t al = *p++; if (al) *edi = al; edi++; }
                edi += c.stride;
            }
        }
    } else {                                               /* colormap modes (0x13677) */
        uint8_t *cmap;
        if (color <= 0xff00u) cmap = (uint8_t *)(uintptr_t)((uint32_t)G32(VA_g_das_remap_chunk_10000_ptr) + color);
        else                  cmap = (uint8_t *)(uintptr_t)((uint32_t)G32(VA_g_world_shading_table_ptr) + (color >> 8));
        if (mode == 2) {                                   /* 0x136d4 dither + remap */
            int dl = 0;
            for (; c.rows > 0; c.rows--) {
                if (c.remain < c.width) blit_icon_decode(&c);
                uint8_t *p = c.rd; int32_t ecx = c.width; c.remain -= ecx; c.rd += ecx;
                if (!dl) { for (; ecx > 0; ecx--) { uint8_t bl = *p++; if (bl) *edi = cmap_lut(cmap, bl); edi++; } edi += c.stride; }
                dl = !dl;
            }
        } else if (mode == 0) {                            /* 0x13711 doubled + remap */
            int32_t ebx = c.stride + c.width2;
            for (; c.rows > 0; c.rows--) {
                if (c.remain < c.width) blit_icon_decode(&c);
                uint8_t *p = c.rd; int32_t ecx = c.width; c.remain -= ecx; c.rd += ecx;
                for (; ecx > 0; ecx--) { uint8_t bl = *p++; if (bl) { uint8_t v = cmap_lut(cmap, bl); edi[0] = v; edi[ebx] = v; } edi++; }
                edi += c.stride;
            }
        } else {                                           /* 0x1369f plain + remap */
            for (; c.rows > 0; c.rows--) {
                if (c.remain < c.width) blit_icon_decode(&c);
                uint8_t *p = c.rd; int32_t ecx = c.width; c.remain -= ecx; c.rd += ecx;
                for (; ecx > 0; ecx--) { uint8_t bl = *p++; if (bl) *edi = cmap_lut(cmap, bl); edi++; } edi += c.stride;
            }
        }
    }
}

/* get_item_tab_index (0x1b0b2): EAX = item index -> EAX = tab index (0..4) / 0. Resolves the template
 * (get_dbase100_inventory_entry 0x18147 bridge), extracts the category nibble ((rec[+4]>>8)&0xf), and
 * returns its position in the 5-entry tab-category table at 0x7123c (or 0 if not found / index 0). */
uint32_t get_item_tab_index(uint32_t eax_in)
{
    if (eax_in == 0) return 0;
    uint32_t rec = get_dbase100_inventory_entry(eax_in);   /* re-point 0x18147 (get_dbase100_inventory_entry) */
    int32_t ebx = ((int32_t)*(uint32_t *)(uintptr_t)(rec + 4) >> 8) & 0xf;
    for (uint32_t eax = 0; eax < 5; eax++)
        if ((uint32_t)(uint8_t)G8(VA_g_inventory_tab_context_map + eax) == (uint32_t)ebx) return eax;
    return 0;
}

/* free_active_item_hud_icon (0x1823a): if the cached cursor-held HUD icon handle [0x7fed0] is non-zero,
 * frees it on the DAS cache pool (pool_free_handle 0x360b3, memory_pool bridge) and clears the slot. Void. */
void free_active_item_hud_icon(void)
{
    if (G32(VA_g_active_item_hud_icon) != 0) {
        pool_free_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle),   /* re-point 0x360b3 */
                                (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_active_item_hud_icon));
        G32(VA_g_active_item_hud_icon) = 0;
    }
}

/* commit_held_cursor_item (0x1a88b): commits the currently-held cursor item (g_current_cursor_entry
 * 0x7fef0, a stored ptr into the cursor-entry table — A4 deref RAW). No-op if nothing held or the cursor
 * list is 2. For list 1 -> equip it (activate_weapon_item(entry[+4], entry[+8]) bridge); else (list 0)
 * select it ([0x81044]=entry[+4] + update_selected_item_icon bridge). Void. */
void commit_held_cursor_item(void)
{
    uint32_t cur = (uint32_t)G32(VA_g_current_cursor_entry);
    if (cur == 0) return;
    if (G32(VA_g_cursor_active_list) == 2) return;
    if (G32(VA_g_cursor_active_list) == 1) {                                /* equip */
        regs_t io; memset(&io, 0, sizeof io); io.va = ACTIVATE_WEAPON_ITEM + OBJ_DELTA;
        io.eax = *(uint32_t *)(uintptr_t)(cur + 4);
        io.edx = *(uint32_t *)(uintptr_t)(cur + 8);
#ifndef ROTH_STANDALONE
        call_orig(&io);
#else
        inv_standalone_route(&io);   /* M3 route (un-routed targets stay fail-loud inside) */
#endif
        return;
    }
    G32(VA_g_selected_item_primary) = (int32_t)*(uint32_t *)(uintptr_t)(cur + 4);  /* select */
    inv_bridge_void(UPDATE_SELECTED_ICON);
}

/* restore_active_held_item (0x1818d): re-establish the cursor-held item after a screen/state change. Frees
 * the cached HUD icon (free_active_item_hud_icon [L]), scans the carried array for the first slot whose
 * template carries the "held" flag ([rec+5]&0x10); on finding one it (optionally) reloads the item icon DAS
 * (dos_open_file 0x41ae5 -> load_das_cache_resource 0x1869b -> dos_close_handle 0x41b41 bridges, only if
 * [rec+0xc]!=0), latches the held-item globals [0x81050]/[0x81054]/[0x8104c], and unequips the weapon if
 * none is equipped (activate_weapon_item(0,0) bridge). Void. LIVE-SWAP target (DAS/DOS bridges). */
void restore_active_held_item(void)
{
    free_active_item_hud_icon();                     /* 0x1823a [L] */
    uint32_t esi = (uint32_t)G32(VA_g_inventory_count);                  /* count */
    uint32_t ecx = 0x80c30u + OBJ_DELTA;                    /* carried base */
    if (esi == 0) return;
    for (;;) {
        int32_t ebx = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)ecx;   /* slot id */
        if (ebx != 0) {
            uint32_t off = *(uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_dbase100_inventory_table) + (uint32_t)ebx * 4);
            if (off != 0) {
                uint32_t rec = (uint32_t)G32(VA_g_dbase100_base) + off;
                if (!(*(uint8_t *)(uintptr_t)(rec + 5) & 0x10)) { esi--; }   /* not held -> 0x18229 */
                else {
                    if (*(uint32_t *)(uintptr_t)(rec + 0xc) != 0) {          /* reload icon DAS */
                        uint32_t handle = dos_open_file(0x81f06u + OBJ_DELTA, 0);  /* dos_open_file (C2) */
                        if (handle != 0) {
                            G32(VA_g_active_item_hud_icon) = (int32_t)load_das_cache_resource(  /* re-point 0x1869b */
                                *(uint32_t *)(uintptr_t)(rec + 0xc), handle);
                            dos_close_handle(handle);                 /* dos_close_handle (C2) */
                        }
                    }
                    G32(VA_g_selected_item_primary + 0xc) = (int32_t)rec;                             /* 0x181fe */
                    G32(VA_g_selected_item_primary + 0x10) = (int32_t)(int16_t)*(uint16_t *)(uintptr_t)ecx;
                    G32(VA_g_selected_item_primary + 0x8) = (int32_t)ecx;
                    if (G32(VA_g_selected_item_secondary) == 0) {
                        regs_t io; memset(&io, 0, sizeof io);
                        io.va = ACTIVATE_WEAPON_ITEM + OBJ_DELTA; io.eax = 0; io.edx = 0;
#ifndef ROTH_STANDALONE
                        call_orig(&io);
#else
                        inv_standalone_route(&io);   /* M3 route (un-routed targets stay fail-loud inside) */
#endif
                    }
                    return;
                }
            }
        }
        ecx += 4;                                           /* 0x1822a */
        if (esi == 0) return;                               /* ja 0x181a9 (loop while esi != 0) */
    }
}
