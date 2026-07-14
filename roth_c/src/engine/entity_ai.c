/* lift_entity_ai.c — the ROTH entity_ai subsystem lifted to verified C.
 * Per docs/operating/recomp.md §4.6: every subsystem gets its own TU.
 *
 * entity_ai = the dynamic-entity (actor/monster/projectile) pool, per-frame think loop, actor
 * state machine, AI locomotion/aim, and the entity-def cache.
 * lift-lens: docs/reference/lift/entity_ai.md; behavior ref: docs/reference/ROTH_enemy_ai_notes.md.
 *
 * Cluster A (this batch): the pool/def-cache leaves — all three are CALL-CLOSED (their only
 * callees, lookup_dbase100_record_by_id 0x1dcac + build_entity_def_record 0x1e128, are already
 * lifted in dbase100_interpreter) — no bridges.
 *
 * ABI throughout is derived from the DISASM (tools/roth_disasm.py func <va>), not the corpus
 * pseudocode (which drops the EAX returns of both cache functions — gotcha A1).
 */
#include "common.h"
#include "engine.h"
#include <string.h>

/* ---- entity_def_cache_build_entry (0x1e2a4): resolve id -> DBASE100 record -> def struct -----
 *   push ebx; ebx=eax(dest); eax=edx(id); call lookup_dbase100_record_by_id; test eax,eax; je out;
 *   edx=eax(rec); eax=ebx(dest); call build_entity_def_record; out: pop ebx; ret
 * EAX = dest (0x6c def struct), EDX = id. Returns EAX = 0 when the id has no DBASE100 record
 * (lookup's own 0 passes through), else build_entity_def_record's result (1 = AsMonster block
 * scattered into dest, 0 = record had no AsMonster block — dest is still zeroed + id'd in that
 * case, because the builder zero-fills before scanning). Both callees are lifted -> call-closed. */
uint32_t entity_def_cache_build_entry(uint32_t dest, uint32_t id)
{
    uint32_t rec = lookup_dbase100_record_by_id(id);
    if (rec == 0)
        return 0;                                   /* je: EAX = lookup's 0 */
    return build_entity_def_record(dest, rec);
}

/* ---- resolve_object_owner_sector (0x4f263): object record ptr -> owning sector record --------
 * EAX = object-record pointer (inside the map-objects buffer). Scans the per-sector object-list
 * offset table: word[i] at g_map_objects_buffer+2+2i for i in 0..count-1 (count =
 * CX of g_sector_count 0x91df8; `dec cx; jg` do-while, so ONE iteration always runs, and only the
 * low 16 bits of the count register are decremented). Tracks the LARGEST word <= the object's
 * 16-bit buffer offset (first occurrence wins ties: the `jbe` skip requires strictly greater).
 * Found  -> EAX = index*0x1a + g_sector_section_offset (0x91dfc)  [via lea 13*(2*index)].
 * None   -> EAX = the LAST table word read, zero-extended (the `sub eax,eax` prefill + lodsw
 *           low-word overwrite — a faithful quirk; callers only use the found case).
 * g_map_objects_buffer (0x90aa4) is a STORED-POINTER global (A4): holds a runtime host address,
 * deref raw. EBX/ESI/EDI/ECX/EBP saved/restored by the original; EAX is the only output. */
uint32_t resolve_object_owner_sector(uint32_t objrec)
{
    uint32_t base = (uint32_t)G32(VA_g_map_objects_buffer);          /* g_map_objects_buffer (stored ptr) */
    uint16_t cx   = (uint16_t)(uint32_t)G32(VA_g_sector_count);/* g_sector_count — only CX decremented */
    uint16_t bx   = (uint16_t)(objrec - base);       /* the object's 16-bit buffer offset */
    const uint8_t *esi = (const uint8_t *)(uintptr_t)(base + 2);
    uint16_t best = 0;                               /* di */
    const uint8_t *best_pos = 0;                     /* ebp = esi AFTER the accepted lodsw */
    uint16_t ax = 0;                                 /* sub eax,eax; lodsw writes only AX */
    do {
        ax = *(const uint16_t *)esi;                 /* lodsw */
        esi += 2;
        if (ax <= bx && ax > best) {                 /* jb skip (need ax<=bx); jbe skip (need ax>di) */
            best     = ax;
            best_pos = esi;
        }
    } while ((int16_t)--cx > 0);                     /* dec cx; jg */
    if (best == 0)
        return ax;                                   /* quirk: EAX = last word read (high half 0) */
    uint32_t twice_idx = (uint32_t)((uintptr_t)best_pos - base) - 4u;  /* ebp - base - 4 = 2*index */
    return twice_idx * 13u + (uint32_t)G32(VA_g_sector_section_offset); /* = index*0x1a + g_sector_section_offset */
}

/* ---- reset_entity_state_with_sound (0x4273e): settle an entity + play its def's sound --------
 * EDI = entity record, EDX = threshold (fire_entity_pending_trigger passes [rec+0xc]); no caller
 * consumes the return. Sets ent+8 = 0x20 and ent+0xb = 1; if the entity's def slot chain
 * (ctx = [ent+0], def = [ctx+0]) is non-null and revalidate_entity_def succeeds (CF clear), picks
 * the def's sound id: word[def+0x60], or word[def+0x62] + (ent+8 |= 1) when EDX >= dword[def+8]
 * (unsigned jb). Non-zero id -> play_entity_sound(id-1, 0, X, Y) with X/Y from the coord pair at
 * [ent+4] (the original leaves the coord POINTER's high half in EBX's upper word — dead, the
 * callee reads BX/CX 16-bit; reproduced anyway). Always clears word[ent+0x1a].
 * EBX THREADING (A0): after the call the original reads the def from EBX, which revalidate updates
 * on the lookup path; [ctx+0] holds the same value on every CF-clear path, so the lift re-reads the
 * slot. revalidate does not touch EDX (confirmed in the 0x426fc disasm). All callees lifted ->
 * call-closed. */
void reset_entity_state_with_sound(uint32_t edi, uint32_t edx)
{
    *(volatile uint8_t *)(uintptr_t)(edi + 8)   = 0x20;
    *(volatile uint8_t *)(uintptr_t)(edi + 0xb) = 1;
    uint32_t esi = *(volatile uint32_t *)(uintptr_t)edi;       /* entity-context ptr */
    uint32_t ebx = *(volatile uint32_t *)(uintptr_t)esi;       /* current def (unguarded, as orig) */
    if (ebx != 0 &&
        revalidate_entity_def((uint8_t *)(uintptr_t)esi,
                                     (uint8_t *)(uintptr_t)ebx) == 0) {
        ebx = *(volatile uint32_t *)(uintptr_t)esi;            /* refreshed def (orig: EBX out) */
        uint16_t snd = *(volatile uint16_t *)(uintptr_t)(ebx + 0x60);
        if (edx >= *(volatile uint32_t *)(uintptr_t)(ebx + 8)) {
            snd = *(volatile uint16_t *)(uintptr_t)(ebx + 0x62);
            *(volatile uint8_t *)(uintptr_t)(edi + 8) |= 1;
        }
        if (snd != 0) {
            uint32_t coords = *(volatile uint32_t *)(uintptr_t)(edi + 4);
            uint32_t bx = (coords & 0xffff0000u) | *(volatile uint16_t *)(uintptr_t)coords;
            uint32_t cx = *(volatile uint16_t *)(uintptr_t)(coords + 2);
            play_entity_sound((uint32_t)snd - 1u, 0, bx, cx);
        }
    }
    *(volatile uint16_t *)(uintptr_t)(edi + 0x1a) = 0;
}

/* ---- collide_entity_and_steer (0x3ea10): AI mover — collide along a delta, steer on a hit ----
 * ECX = dx, EDX = dy (the move delta). Sibling of the already-lifted probe_collision_step 0x3eb90
 * (same 0x34 scratch frame seeded with query-Z frame[+0x1e] = 0x8c126 and step budget frame[+0x24]
 * = max((dx>>10)^2+(dy>>10)^2, 0x100000); fire gate 0x8c1e6 cleared; find_sector_and_collide with
 * EAX = g_player_query 0x8c174). On a collision hit (g_collision_hit_flags 0x8c1e0 != 0) with the
 * actual distance moved dist = ((restore-query)>>10)^2 summed:
 *   dist*4  > budget  -> moved plenty, no steering;
 *   dist*16 < budget  -> barely moved: mark blocked (ent+9 |= 0x20) and NUDGE;
 *   else if !(ent+9 & 0x20): TURN — facing byte[geom+6] = ((atan2_bearing + 0x100 via `inc ah`,
 *       no carry past AH) >> 1) low byte, from the restore/query position high words;
 *   else NUDGE: facing += +-3 (sign from ent+9 & 0x10), turn counter word[ent+0x1a] += 2, flags
 *       ent+9 |= 8|0x40; counter >= 0x100 -> counter = 0, ent+9 &= ~8.
 * ent = g_collision_move_entity 0x8c0dc, geom = g_collision_move_entity_geom 0x8c0d8.
 * Tail (all paths): if in a sector (0x90c12 != 0) try the portal cross (find_query_sector); found
 * -> store + done. Else restore the saved query pos (0x8c160/64/68 -> 0x8c120/24/28) when the
 * saved sector 0x8c170 != 0, and store the saved sector to 0x90c12.
 * The 0x3eae2..0x3eafd block is DEAD CODE (no entry; the decompile omits it) — not transcribed.
 * No caller consumes the return (void), BUT the sole caller (move_entity_with_collision 0x3e590)
 * reads the callee's DEAD FRAME via the EBP the original leaves behind (frame+0/+2 = the resolved
 * Z window from find_sector_and_collide) — so the frame is a caller-supplied parameter here, not
 * a local (>= 0x40 bytes; zeroed inside). All callees lifted -> call-closed. */
void collide_entity_and_steer(int32_t ecx_dx, int32_t edx_dy, uint8_t *frame)
{
    memset(frame, 0, 0x40);
    *(int16_t *)(frame + 0x1e) = (int16_t)G16(VA_g_locate_query_z + 0x2);         /* 0x3ea18 frame[+1e] = query Z */

    int32_t cx = ecx_dx >> 10, dy = edx_dy >> 10;              /* 0x3ea22/25 sar ,0xa */
    uint32_t budget = (uint32_t)(cx * cx) + (uint32_t)(dy * dy);
    if (!(budget > 0x100000u)) budget = 0x100000u;             /* 0x3ea30 cmp; ja keep / clamp */
    G8(VA_g_collision_step_active + 0x4) = 0;                                            /* 0x3ea3d clear fire gate */
    *(uint32_t *)(frame + 0x24) = budget;                      /* 0x3ea44 frame[+24] = budget */

    find_sector_and_collide((uint32_t)G32(VA_g_collision_blocker_object + 0x8), frame);  /* 0x3ea4e */

    if (G8(VA_g_collision_hit_flags) != 0) {                                     /* 0x3ea55 collision hit -> steer */
        int32_t ddx = ((int32_t)G32(VA_g_collision_restore_x) - (int32_t)G32(VA_g_locate_query_x)) >> 10;
        int32_t ddy = ((int32_t)G32(VA_g_collision_restore_y) - (int32_t)G32(VA_g_locate_query_y)) >> 10;
        uint32_t dist  = (uint32_t)(ddx * ddx) + (uint32_t)(ddy * ddy);
        uint32_t dist4 = dist << 2;                             /* 0x3ea8b shl eax,2 */
        if (dist4 <= budget) {                                  /* 0x3ea8e ja -> skip steering */
            uint32_t ent  = (uint32_t)G32(VA_g_collision_move_entity);             /* g_collision_move_entity */
            uint32_t geom = (uint32_t)G32(VA_g_collision_move_entity_geom);             /* g_collision_move_entity_geom */
            uint32_t dist16 = dist4 << 2;                       /* 0x3eaa5 shl eax,2 */
            int nudge;
            if (dist16 < budget) {                              /* 0x3eaa8 jb -> barely moved */
                *(volatile uint8_t *)(uintptr_t)(ent + 9) |= 0x20;   /* 0x3eaff blocked */
                nudge = 1;
            } else if ((*(volatile uint8_t *)(uintptr_t)(ent + 9) & 0x20) == 0) {
                /* TURN toward the pre-collision position (0x3eab3..0x3eae0) */
                uint32_t r = atan2_bearing((int16_t)G16(VA_g_collision_restore_x + 0x2),   /* AX restore_x hi */
                                                  (int16_t)G16(VA_g_locate_query_y + 0x2),   /* DX query_y   hi */
                                                  (int16_t)G16(VA_g_collision_restore_y + 0x2),   /* BX restore_y hi */
                                                  (int16_t)G16(VA_g_locate_query_x + 0x2));  /* CX query_x   hi */
                r = (r & 0xffff00ffu) | ((r + 0x100u) & 0x0000ff00u);      /* inc ah (no carry out) */
                *(volatile uint8_t *)(uintptr_t)(geom + 6) = (uint8_t)((int32_t)r >> 1); /* sar,1 */
                nudge = 0;
            } else {
                nudge = 1;                                      /* 0x3eaad already blocked */
            }
            if (nudge) {                                        /* 0x3eb03.. */
                uint8_t step = (*(volatile uint8_t *)(uintptr_t)(ent + 9) & 0x10)
                               ? (uint8_t)0xfd : (uint8_t)3;    /* mov al,3; neg eax (AL only lives) */
                *(volatile uint8_t *)(uintptr_t)(geom + 6) += step;
                *(volatile uint16_t *)(uintptr_t)(ent + 0x1a) += 2;
                *(volatile uint8_t *)(uintptr_t)(ent + 9) |= 8;
                *(volatile uint8_t *)(uintptr_t)(ent + 9) |= 0x40;
                if (*(volatile uint16_t *)(uintptr_t)(ent + 0x1a) >= 0x100) {  /* 0x3eb1d jb */
                    *(volatile uint16_t *)(uintptr_t)(ent + 0x1a) = 0;
                    *(volatile uint8_t *)(uintptr_t)(ent + 9) &= 0xf7;
                }
            }
        }
    }

    /* tail (0x3eb37): portal cross, else restore saved query pos + sector */
    if ((uint16_t)G16(VA_g_player_sector) != 0) {
        uint32_t newsec = 0;
        find_query_sector((uint16_t)G16(VA_g_player_sector), frame, &newsec);   /* 0x3eb42 */
        if ((uint16_t)newsec != 0) {                            /* 0x3eb47 jne -> store, done */
            G16(VA_g_player_sector) = (uint16_t)newsec;                    /* 0x3eb83 */
            return;
        }
    }
    uint16_t saved = (uint16_t)G16(VA_g_collision_blocker_object + 0x4);                    /* 0x3eb4c saved sector */
    if (saved != 0) {                                           /* 0x3eb55 restore the query pos */
        G32(VA_g_locate_query_x) = G32(VA_g_max_climb + 0x18);
        G32(VA_g_locate_query_z) = G32(VA_g_max_climb + 0x1c);
        G32(VA_g_locate_query_y) = G32(VA_g_max_climb + 0x20);
    }
    G16(VA_g_player_sector) = saved;                                       /* 0x3eb7b / 0x3eb83 */
}

/* ---- move_entity_with_collision (0x3e590): entity-level swept move + writeback ---------------
 * EAX = dx, EDX = dy (the move delta), EBX = entity record, ECX = output/context struct.
 * pushal/popal — ALL registers restored at exit, so there is no register output (void); every
 * result goes through memory (the ctx struct + the entity/sub-record + collision globals).
 * Body: seeds the collision query block from the entity (position dwords assembled from sub-record
 * HIGH words [esi+0]/[+2]/[+0xa] + entity LOW words [ebx+0x10]/[+0x12]/[+0x14]; query = pos +
 * delta), stashes the entity in 0x8c0dc/0x8c0d8, saves/sets sector 0x90c12 + 0x8c170 from
 * [ebx+0x16], step height 0x90bde = 0x80 (0x260 when flag [ebx+0xa]&2), pulls the collision dims
 * from the DAS collision buffer (0x85c50)[sprite [esi+4]] into the 0x8c0e8/0x8c130..0x8c144 bound
 * block, clears the hit flags, then calls collide_entity_and_steer with [esi+7] bit1 forced during
 * the call. Writeback: resolved X/Y -> ctx+0/+4 + entity + sub-record; the resolved Z (read from
 * the collide scratch frame word 0 — the original reads the callee's dead frame through EBP; here
 * an explicit frame buffer) unless it still holds the 0x8000 sentinel high byte (`cmp ah,0x80`):
 * Z -> ctx+0x16/+8 (Z<<16), entity low word ZEROED (the shl artifact), sub-record hi word; the
 * head-clearance test (Z + (dim-0x10) - 0x18 - frame word 2, 16-bit js) drives the crush timer
 * ([ebx+0xc] -= 10; underflow -> 0x2ee0 + flags [ebx+9]|=1, [ebx+8]=1). Tail: ctx+0x18 = the
 * post-collide sector; when non-zero, ctx+0x10 = the linked cell's word[+8] (if cell exists,
 * word[+8] > word[+2], and word[+8]-0x20 < ctx Z) else the sector's word[+2]; ctx+0x1a = hit
 * flags byte; finally 0x90c12 is RESTORED to its entry value. All callees lifted -> call-closed. */
void move_entity_with_collision(int32_t dx, int32_t dy, uint32_t ebx, uint32_t ecx)
{
    G8(VA_g_collision_step_active + 0x2) = 0x83;                                         /* 0x3e590 (before pushal) */
    G32(VA_g_collision_target_active) = 0;
    G8(VA_g_collision_step_active)  = 0;
    G16(VA_g_collision_portal_continue) = 0;
    uint32_t esi = *(volatile uint32_t *)(uintptr_t)(ebx + 4);  /* sub-record */
    if (esi == 0) return;                                       /* 0x3e5b7 je -> popal/ret */
    G32(VA_g_collision_move_entity) = (int32_t)ebx;                                /* g_collision_move_entity */
    G32(VA_g_collision_move_entity_geom) = (int32_t)esi;                                /* g_collision_move_entity_geom */
    G32(VA_g_collision_blocker_object + 0x8) = 0;                                           /* query ctx = 0 */
    uint16_t entry_sector = (uint16_t)G16(VA_g_player_sector);             /* push eax ... restored at exit */

    uint32_t z = (uint32_t)*(volatile uint16_t *)(uintptr_t)(esi + 0xa) << 16;
    G32(VA_g_max_climb + 0x1c) = (int32_t)z;                                  /* saved Z */
    z |= *(volatile uint16_t *)(uintptr_t)(ebx + 0x14);
    G32(VA_g_locate_query_z) = (int32_t)z;                                  /* query Z */
    *(volatile uint32_t *)(uintptr_t)(ecx + 0xc) = z;

    uint32_t x = ((uint32_t)*(volatile uint16_t *)(uintptr_t)esi << 16)
               | *(volatile uint16_t *)(uintptr_t)(ebx + 0x10);
    G32(VA_g_collision_restore_x) = (int32_t)x;                                  /* restore X */
    G32(VA_g_max_climb + 0x18) = (int32_t)x;                                  /* saved X */
    G32(VA_g_locate_query_x) = (int32_t)(x + (uint32_t)dx);                 /* query X = pos + dx */

    uint32_t y = ((uint32_t)*(volatile uint16_t *)(uintptr_t)(esi + 2) << 16)
               | *(volatile uint16_t *)(uintptr_t)(ebx + 0x12);
    G32(VA_g_collision_restore_y) = (int32_t)y;
    G32(VA_g_max_climb + 0x20) = (int32_t)y;
    G32(VA_g_locate_query_y) = (int32_t)(y + (uint32_t)dy);                 /* query Y = pos + dy */

    uint16_t esec = *(volatile uint16_t *)(uintptr_t)(ebx + 0x16);
    G16(VA_g_player_sector) = esec;                                        /* current sector = entity's */
    G16(VA_g_collision_blocker_object + 0x4) = esec;                                        /* saved sector */
    G16(VA_g_max_step_height) = 0x80;                                        /* step height */
    if (*(volatile uint8_t *)(uintptr_t)(ebx + 0xa) & 2)
        G16(VA_g_max_step_height) = 0x260;

    /* collision bound block from the DAS collision dims (stored-ptr 0x85c50; A4 raw) */
    uint32_t dimrec = (uint32_t)G32(VA_g_das_collision_buffer)
                    + ((uint32_t)*(volatile uint16_t *)(uintptr_t)(esi + 4) << 2);
    uint16_t dim = (uint16_t)(*(volatile uint16_t *)(uintptr_t)dimrec - 0x10);
    G16(VA_g_collision_move_entity + 0xc) = dim;
    *(volatile uint16_t *)(uintptr_t)(ecx + 0x14) = dim;
    uint32_t r = *(volatile uint16_t *)(uintptr_t)(dimrec + 2);
    G32(VA_g_collision_box_max) = (int32_t)r;
    G32(VA_g_collision_radius) = (int32_t)(r << 2);
    uint32_t negr = (uint32_t)(-(int32_t)r);
    G32(VA_g_collision_box_min) = (int32_t)negr;
    uint32_t r2    = negr * negr;                               /* imul eax,eax on -r = r^2 */
    uint32_t r2_64 = r2 << 6;
    G32(VA_g_collision_radius_sq) = (int32_t)r2_64;
    G32(VA_g_collision_radius_sq + 0x4) = (int32_t)(r2 + r2_64);
    G32(VA_g_collision_corner_radius_sq) = (int32_t)(r2_64 + 0x40);

    G8(VA_g_collision_hit_flags) = 0;                                            /* clear hit flags */
    uint8_t f7 = *(volatile uint8_t *)(uintptr_t)(esi + 7);     /* save flag byte */
    *(volatile uint8_t *)(uintptr_t)(esi + 7) = (uint8_t)(f7 | 2);

    uint8_t frame[0x40];                                        /* the collide scratch (EBP channel) */
    collide_entity_and_steer(dx, dy, frame);             /* 0x3e6bf */

    *(volatile uint8_t *)(uintptr_t)(esi + 7) = f7;             /* restore flag byte */

    uint32_t edi = ecx;                                         /* pop edi <- the pushed ECX */
    uint32_t rx = (uint32_t)G32(VA_g_locate_query_x);                       /* resolved X */
    *(volatile uint32_t *)(uintptr_t)edi = rx;
    *(volatile uint16_t *)(uintptr_t)(ebx + 0x10) = (uint16_t)rx;
    *(volatile uint16_t *)(uintptr_t)esi = (uint16_t)(rx >> 16);
    uint32_t ry = (uint32_t)G32(VA_g_locate_query_y);                       /* resolved Y */
    *(volatile uint32_t *)(uintptr_t)(edi + 4) = ry;
    *(volatile uint16_t *)(uintptr_t)(ebx + 0x12) = (uint16_t)ry;
    *(volatile uint16_t *)(uintptr_t)(esi + 2) = (uint16_t)(ry >> 16);

    uint16_t fz = *(uint16_t *)(frame + 0);                     /* mov ax,[ebp] */
    if ((fz >> 8) != 0x80) {                                    /* cmp ah,0x80; je skip */
        *(volatile uint16_t *)(uintptr_t)(edi + 0x16) = fz;
        *(volatile uint32_t *)(uintptr_t)(edi + 8) = (uint32_t)fz << 16;
        *(volatile uint16_t *)(uintptr_t)(ebx + 0x14) = 0;      /* ax == 0 after the shl */
        *(volatile uint16_t *)(uintptr_t)(esi + 0xa) = fz;      /* shr back */
        uint16_t d2 = *(uint16_t *)(frame + 2);                 /* mov dx,[ebp+2] */
        *(volatile uint16_t *)(uintptr_t)(edi + 0x12) = d2;
        uint16_t t = (uint16_t)(fz + *(volatile uint16_t *)(uintptr_t)(edi + 0x14)
                                - 0x18 - d2);                   /* 16-bit chain; js */
        if (!(t & 0x8000u)) {
            int32_t timer = *(volatile int32_t *)(uintptr_t)(ebx + 0xc) - 0xa;
            *(volatile int32_t *)(uintptr_t)(ebx + 0xc) = timer;
            if (timer < 0) {                                    /* jns skip */
                *(volatile int32_t *)(uintptr_t)(ebx + 0xc) = 0x2ee0;
                *(volatile uint8_t *)(uintptr_t)(ebx + 9) |= 1;
                *(volatile uint8_t *)(uintptr_t)(ebx + 8) = 1;
            }
        }
    }

    uint16_t fsec = (uint16_t)G16(VA_g_player_sector);                     /* post-collide sector */
    *(volatile uint16_t *)(uintptr_t)(edi + 0x18) = fsec;
    if (fsec != 0) {
        uint32_t srec = (uint32_t)G32(VA_g_map_geometry_buffer) + fsec;          /* geom base (canon-held ptr) */
        uint16_t cell = *(volatile uint16_t *)(uintptr_t)(srec + 0x18);
        uint16_t out;
        int have = 0;
        if (cell != 0) {
            uint32_t crec = (uint32_t)G32(VA_g_map_geometry_buffer) + cell;
            uint16_t c8 = *(volatile uint16_t *)(uintptr_t)(crec + 8);
            if (c8 > *(volatile uint16_t *)(uintptr_t)(crec + 2)) {          /* jbe skip */
                if ((uint16_t)(c8 - 0x20) <
                    *(volatile uint16_t *)(uintptr_t)(edi + 0x16)) {         /* jae skip */
                    out = c8;                                   /* (cx-0x20)+0x20, low word */
                    have = 1;
                }
            }
        }
        if (!have)
            out = *(volatile uint16_t *)(uintptr_t)(srec + 2);  /* 0x3e77c */
        *(volatile uint16_t *)(uintptr_t)(edi + 0x10) = out;
    }
    *(volatile uint8_t *)(uintptr_t)(edi + 0x1a) = (uint8_t)G8(VA_g_collision_hit_flags);
    G16(VA_g_player_sector) = entry_sector;                                /* pop eax; restore */
}

/* ---- entity_apply_vertical_movement (0x43187): gravity / height integrator ------------------
 * EAX = dx, EDX = dy (forwarded to move_entity_with_collision), ESI = entity record, EDI = the
 * entity's sub-record ([esi+4] — every caller passes that). Runs the horizontal move first with
 * ECX = the FIXED ctx block at canon 0x911a4 (a relocated immediate — GADDR), then integrates the
 * 16.16 height ctx+0xc against the resolved bounds:
 *   FREE regime (flag [esi+0xa]&4 set, or word[[esi]+4] == 0): height vs target ctx+8 — equal ->
 *   settle (flags &= 0xf9, vel[esi+0x18] = 4); above -> FALL by vel<<16 (vel<0 resets to 4),
 *   clamped to the floor word ctx+0x10 (signed), vel += 2; below -> RISE by 0x80000, reaching the
 *   floor word settles.
 *   CONSTRAINED regime (bit4 clear and word[[esi]+4] != 0): vel = 0; word[[esi]+0x14]==0 -> land
 *   at current height frac0; inside [floor, ceil-0x20]: at the resolved Z (ctx+0xa) -> flags &=
 *   0xfd + vel=4; flags [esi+9]&0x28 or at player-Z+0x20 -> land frac0; else step toward player Z
 *   0x90a90 clamped +-0x20000 and land frac0. Outside the bounds -> step toward the violated
 *   bound clamped +-0x80000, flac |= 2, vel = 0xffff.
 * Writebacks: sub-record+0xa = height int word, entity+0x14 = frac word (0 on the land/clamp
 * paths). Tail: if the post-move sector ctx+0x18 changed vs entity+0x16, xchg it in and call
 * add_secondary_state_record(old, new, [esi+4]). The original also leaves EDI = [esi+4] (register
 * effect only — no C caller needs it). Return regs not consumed by any caller (void). */
void entity_apply_vertical_movement(int32_t dx, int32_t dy, uint32_t esi, uint32_t edi)
{
    uint32_t ebx = (uint32_t)GADDR(VA_g_dynamic_entity_table + 0x1c0);                    /* the vertical-move ctx block */
    move_entity_with_collision(dx, dy, esi, ebx);        /* 0x4318e */

    uint32_t ctxobj = *(volatile uint32_t *)(uintptr_t)esi;     /* [esi] */
    uint32_t eax;
    if ((*(volatile uint8_t *)(uintptr_t)(esi + 0xa) & 4) ||
        *(volatile uint16_t *)(uintptr_t)(ctxobj + 4) == 0) {
        /* ---- FREE regime (0x431a4) ---- */
        uint32_t h   = *(volatile uint32_t *)(uintptr_t)(ebx + 0xc);
        uint32_t tgt = *(volatile uint32_t *)(uintptr_t)(ebx + 8);
        int16_t floorw = (int16_t)*(volatile uint16_t *)(uintptr_t)(ebx + 0x10);
        if (h == tgt) goto settle;                              /* je 0x432fe */
        if ((int32_t)h > (int32_t)tgt) {
            /* FALL (0x432c4): integrate the velocity, clamp to the floor, accelerate */
            uint32_t vel = *(volatile uint16_t *)(uintptr_t)(esi + 0x18);
            if (vel & 0x8000u) {                                /* jns: negative -> reset 4 */
                vel = 4;
                *(volatile uint16_t *)(uintptr_t)(esi + 0x18) = 4;
            }
            eax = h - (vel << 16);
            if ((int16_t)(uint16_t)(eax >> 16) > floorw) {      /* jg: still above the floor */
                *(volatile uint16_t *)(uintptr_t)(edi + 0xa)  = (uint16_t)(eax >> 16);
                *(volatile uint16_t *)(uintptr_t)(esi + 0x14) = (uint16_t)eax;
            } else {                                            /* clamp: floor.0 */
                *(volatile uint16_t *)(uintptr_t)(edi + 0xa)  = (uint16_t)floorw;
                *(volatile uint16_t *)(uintptr_t)(esi + 0x14) = 0;
            }
            *(volatile uint16_t *)(uintptr_t)(esi + 0x18) += 2; /* gravity accel */
            goto tail;
        }
        /* RISE (0x431b6): +0x80000 per tick until the floor word is reached */
        eax = h + 0x80000u;
        if ((int16_t)(uint16_t)(eax >> 16) < floorw) {          /* jl: still below */
            *(volatile uint16_t *)(uintptr_t)(edi + 0xa)  = (uint16_t)(eax >> 16);
            *(volatile uint16_t *)(uintptr_t)(esi + 0x14) = (uint16_t)eax;
            *(volatile uint16_t *)(uintptr_t)(esi + 0x18) = 0xffff;
            goto tail;
        }
        *(volatile uint16_t *)(uintptr_t)(edi + 0xa)  = (uint16_t)floorw;
        *(volatile uint16_t *)(uintptr_t)(esi + 0x14) = 0;
settle: /* 0x432fe */
        *(volatile uint8_t *)(uintptr_t)(esi + 0xa) &= 0xf9;
setvel4: /* 0x43302 */
        *(volatile uint16_t *)(uintptr_t)(esi + 0x18) = 4;
        goto tail;
    } else {
        /* ---- CONSTRAINED regime (0x431f7) ---- */
        *(volatile uint16_t *)(uintptr_t)(esi + 0x18) = 0;
        if (*(volatile uint16_t *)(uintptr_t)(ctxobj + 0x14) == 0) {
            eax = *(volatile uint32_t *)(uintptr_t)(ebx + 0xc); /* 0x432a8 */
            goto land_frac0;
        }
        uint16_t ihi   = (uint16_t)(*(volatile uint32_t *)(uintptr_t)(ebx + 0xc) >> 16);
        int16_t floorw = (int16_t)*(volatile uint16_t *)(uintptr_t)(ebx + 0x10);
        int16_t ceilw  = (int16_t)(*(volatile uint16_t *)(uintptr_t)(ebx + 0x12) - 0x20);
        if ((int16_t)ihi < floorw || (int16_t)ihi > ceilw) {
            /* outside the bounds (0x4326d): step toward the violated bound, clamped +-8.0 */
            uint16_t bound = ((int16_t)ihi < floorw) ? (uint16_t)floorw : (uint16_t)ceilw;
            eax = *(volatile uint32_t *)(uintptr_t)(ebx + 0xc);
            int32_t d = (int32_t)((uint32_t)bound << 16) - (int32_t)eax;
            if (!(d < 0x80000)) d = 0x80000;                    /* jl skip */
            if (!(d > (int32_t)0xfff80000)) d = (int32_t)0xfff80000; /* jg skip */
            eax += (uint32_t)d;
            *(volatile uint8_t *)(uintptr_t)(esi + 0xa) |= 2;
            *(volatile uint16_t *)(uintptr_t)(esi + 0x14) = (uint16_t)eax;
            *(volatile uint16_t *)(uintptr_t)(edi + 0xa)  = (uint16_t)(eax >> 16);
            *(volatile uint16_t *)(uintptr_t)(esi + 0x18) = 0xffff;
            goto tail;
        }
        if (ihi == *(volatile uint16_t *)(uintptr_t)(ebx + 0xa)) {   /* at the resolved Z */
            *(volatile uint8_t *)(uintptr_t)(esi + 0xa) &= 0xfd;     /* 0x432be */
            goto setvel4;
        }
        if (*(volatile uint8_t *)(uintptr_t)(esi + 9) & 0x28) {
            eax = *(volatile uint32_t *)(uintptr_t)(ebx + 0xc);
            goto land_frac0;
        }
        if (ihi == (uint16_t)((uint16_t)G16(VA_g_player_z) + 0x20)) {      /* at player Z + 0x20 */
            eax = *(volatile uint32_t *)(uintptr_t)(ebx + 0xc);
            goto land_frac0;
        }
        /* step toward the player Z (0x90a90), clamped +-2.0 */
        eax = *(volatile uint32_t *)(uintptr_t)(ebx + 0xc);
        {
            int32_t d = (int32_t)eax - (int32_t)G32(VA_g_player_x + 0x2);
            if (!(d < 0x20000)) d = 0x20000;
            if (!(d > (int32_t)0xfffe0000)) d = (int32_t)0xfffe0000;
            eax -= (uint32_t)d;
        }
land_frac0: /* 0x432ab */
        *(volatile uint8_t *)(uintptr_t)(esi + 0xa) |= 2;
        *(volatile uint16_t *)(uintptr_t)(esi + 0x14) = 0;
        *(volatile uint16_t *)(uintptr_t)(edi + 0xa)  = (uint16_t)(eax >> 16);
        goto tail;
    }

tail: /* 0x43308: sector-change callback */
    {
        uint16_t newsec = *(volatile uint16_t *)(uintptr_t)(ebx + 0x18);
        uint16_t oldsec = *(volatile uint16_t *)(uintptr_t)(esi + 0x16);
        if (newsec != oldsec) {
            *(volatile uint16_t *)(uintptr_t)(esi + 0x16) = newsec;  /* xchg */
            add_secondary_state_record(oldsec, newsec,
                *(volatile uint32_t *)(uintptr_t)(esi + 4));
        }
    }
}

/* ---- tick_entity_vertical_only (0x43402): zero-delta vertical tick wrapper ------------------
 * EDI = entity record. push edi; esi = edi; edi = [esi+4]; eax = edx = 0; call 0x43187; pop edi.
 * Pure register marshaling — no memory effects of its own. */
void tick_entity_vertical_only(uint32_t edi_entity)
{
    entity_apply_vertical_movement(0, 0, edi_entity,
        *(volatile uint32_t *)(uintptr_t)(edi_entity + 4));
}

/* ---- aim_enemy_at_player (0x4355a): turn toward the player + attack dispatch -----------------
 * EDI = entity record, EAX = the max turn step (low word, sign-extended after the atan2 call —
 * the input is pushed/popped around it). Advances the LCG 0x72730 and stores its bit4 into the
 * entity steer-direction flag ([edi+9] bit4). Computes the bearing error: (atan2_bearing(player
 * 0x90a8e/0x90a96 vs subrec coords) << 7, 16-bit) minus the facing 16-bit pair (LO = entity+0xb,
 * HI = subrec+6), sign-extended; clamps to +-limit. If the game-active gate 0x8a0f0 <= 0 (signed):
 * clear [edi+8] bit2 and return (no turn). If |err| > 2 or no def ([[edi]] == 0): apply the turn
 * (facing16 += err via the add/adc byte pair). Otherwise (aligned + def):
 *   state bit4 ([edi+8] & 0x10) SET: landed ([edi+0xa]&2) with def word+0x12 == 0 -> set
 *     [edi+0xa] bit2 + apply; else attack-rate counter ++[edi+0x20] < def byte+0x6a -> apply;
 *     counter reset; state bit3 -> clear bit2 + ret; else begin_enemy_attack, clear bit2, ret.
 *   state bit4 CLEAR: bit3 -> clear bit2 + ret; no [edi+9]&6 flags, or LCG low byte > 0xc, or
 *     landed-with-no-anim, or counter below rate -> plain ret (NO turn); else counter reset,
 *     launch_enemy_attack_animation, clear bit2, ret.
 * D1: the LCG 0x72730 must be seeded for the oracle. All callees lifted -> call-closed.
 * Return regs not consumed (void). */
void aim_enemy_at_player(uint32_t limit_eax, uint32_t edi)
{
    /* LCG advance -> steer-direction bit */
    uint32_t lcg = (uint32_t)G32(VA_g_ai_wander_rng) * 0x5e5u + 0x29u;
    G32(VA_g_ai_wander_rng) = (int32_t)lcg;
    *(volatile uint8_t *)(uintptr_t)(edi + 9) =
        (uint8_t)((*(volatile uint8_t *)(uintptr_t)(edi + 9) & 0xef) | (lcg & 0x10));

    uint32_t sub = *(volatile uint32_t *)(uintptr_t)(edi + 4);
    uint32_t br  = atan2_bearing((int16_t)G16(VA_g_player_x),                     /* AX player x */
                                        (int16_t)*(volatile int16_t *)(uintptr_t)(sub + 2), /* DX ent y */
                                        (int16_t)G16(VA_g_player_y),                     /* BX player y */
                                        (int16_t)*(volatile int16_t *)(uintptr_t)sub);      /* CX ent x */
    uint16_t ax = (uint16_t)(br << 7);                          /* shl ax,7 (16-bit) */
    uint8_t  cl = *(volatile uint8_t *)(uintptr_t)(edi + 8);    /* state byte */
    uint16_t facing = (uint16_t)(*(volatile uint8_t *)(uintptr_t)(edi + 0xb)
                     | ((uint16_t)*(volatile uint8_t *)(uintptr_t)(sub + 6) << 8));
    int32_t err = (int32_t)(int16_t)(uint16_t)(ax - facing);    /* sub al/sbb ah; cwde */
    int32_t lim = (int32_t)(int16_t)(uint16_t)limit_eax;        /* movsx edx,dx */
    if (!(err < lim)) err = lim;                                /* jl skip */
    if (!(err > -lim)) err = -lim;                              /* jg skip */

    uint16_t apply_ax = (uint16_t)err;   /* the AX the apply block adds (AL mutates below!) */
    if ((int32_t)G32(VA_g_player_health) <= 0) goto clear_ret;             /* jle 0x43614 */
    if (err > 2 || err < -2) goto apply;                        /* not aligned -> just turn */
    uint32_t def = *(volatile uint32_t *)(uintptr_t)(*(volatile uint32_t *)(uintptr_t)edi);
    if (def == 0) goto apply;

    if (cl & 0x10) {                                            /* 0x435e0 primary-attack mode */
        if ((*(volatile uint8_t *)(uintptr_t)(edi + 0xa) & 2) &&
            *(volatile uint16_t *)(uintptr_t)(def + 0x12) == 0) {
            *(volatile uint8_t *)(uintptr_t)(edi + 0xa) |= 4;   /* 0x4365f */
            goto apply;
        }
        uint8_t c = (uint8_t)(*(volatile uint8_t *)(uintptr_t)(edi + 0x20) + 1);
        *(volatile uint8_t *)(uintptr_t)(edi + 0x20) = c;
        if (c < *(volatile uint8_t *)(uintptr_t)(def + 0x6a)) {
            /* jb 0x43663 with AL = the counter byte (`mov al,[edi+0x20]` at 0x435f5) — the
             * original applies (err_hi<<8)|counter here, not the clamped error */
            apply_ax = (uint16_t)((apply_ax & 0xff00u) | c);
            goto apply;
        }
        *(volatile uint8_t *)(uintptr_t)(edi + 0x20) = 0;
        if (cl & 8) goto clear_ret;                             /* 0x43601 */
        *(volatile uint8_t *)(uintptr_t)(edi + 0x20) = 0;
        begin_enemy_attack(edi);                         /* 0x434d8 */
        goto clear_ret;
    }
    /* 0x4361a: secondary/animation mode */
    if (cl & 8) goto clear_ret;
    if (!(*(volatile uint8_t *)(uintptr_t)(edi + 9) & 6)) return;        /* 0x4365a plain ret */
    if ((uint8_t)G8(VA_g_ai_wander_rng) > 0xc) return;                              /* LCG low-byte gate */
    if ((*(volatile uint8_t *)(uintptr_t)(edi + 0xa) & 2) &&
        *(volatile uint16_t *)(uintptr_t)(def + 0x12) == 0) return;
    {
        uint8_t c = (uint8_t)(*(volatile uint8_t *)(uintptr_t)(edi + 0x20) + 1);
        *(volatile uint8_t *)(uintptr_t)(edi + 0x20) = c;
        if (c < *(volatile uint8_t *)(uintptr_t)(def + 0x6a)) return;
    }
    *(volatile uint8_t *)(uintptr_t)(edi + 0x20) = 0;
    launch_enemy_attack_animation(edi);                  /* 0x4347e */
clear_ret: /* 0x43614 */
    *(volatile uint8_t *)(uintptr_t)(edi + 8) &= 0xfb;
    return;
apply: /* 0x43663: facing16 += AX (add lo / adc hi byte pair) */
    {
        uint16_t nf = (uint16_t)(facing + apply_ax);
        *(volatile uint8_t *)(uintptr_t)(edi + 0xb) = (uint8_t)nf;
        *(volatile uint8_t *)(uintptr_t)(sub + 6)   = (uint8_t)(nf >> 8);
    }
}

/* ---- update_actor_movement_ai (0x43326): per-tick locomotion dispatcher ----------------------
 * EDI = entity record; EAX is THREADED into check_entity_player_contact 0x43413 (its 16-bit
 * partial ops carry the caller's EAX high half — gotcha A0; the sign of the 32-bit result is the
 * contact test). Contact (negative): zero-delta vertical tick, [esi+8] |= 0x10, ([esi+9]&8 ->
 * word[esi+0x1a] = 0), [esi+9] &= 0x57, ret. No contact: [esi+8] &= 0xef; movement accumulator
 * byte[esi+0x1f] += the speed scale 0x72740; < 0x10 -> ret (no step). Else -= 0x10; the turn
 * counter block ([esi+9]&8 && word[esi+0x1a]!=0): --counter == 0 -> flags &= 0xd7; else clear
 * bit6, and if it was clear and counter >= 0x50 (signed) advance the LCG 0x72730 — its byte1
 * <= 0x1e re-rolls the steer bit ([esi+9] bit4). Step: vel word[esi+0x18] < 0 (signed) -> dx=dy=0;
 * else facing hi byte[subrec+6]+0x80 indexes the sincos table 0x72080 (sin, +0x100 bytes = cos),
 * each <<5 -> the move delta. Tail-calls entity_apply_vertical_movement(dx, dy, esi, subrec).
 * All callees lifted -> call-closed. Return regs not consumed (void). */
void update_actor_movement_ai(uint32_t eax, uint32_t edi_entity)
{
    uint32_t esi = edi_entity;
    uint32_t sub = *(volatile uint32_t *)(uintptr_t)(esi + 4);
    check_entity_player_contact(eax, (const uint8_t *)(uintptr_t)esi,
                                       (const int16_t *)(uintptr_t)sub);
    if (g_os_contact_sf) {   /* js 0x433e3 — the callee's exit SF, NOT the EAX32 sign (A0) */
        entity_apply_vertical_movement(0, 0, esi, sub);
        *(volatile uint8_t *)(uintptr_t)(esi + 8) |= 0x10;
        if (*(volatile uint8_t *)(uintptr_t)(esi + 9) & 8)
            *(volatile uint16_t *)(uintptr_t)(esi + 0x1a) = 0;
        *(volatile uint8_t *)(uintptr_t)(esi + 9) &= 0x57;
        return;
    }
    uint8_t scale = (uint8_t)G8(VA_g_actor_move_rate);
    *(volatile uint8_t *)(uintptr_t)(esi + 8) &= 0xef;
    uint8_t acc = (uint8_t)(*(volatile uint8_t *)(uintptr_t)(esi + 0x1f) + scale);
    *(volatile uint8_t *)(uintptr_t)(esi + 0x1f) = acc;
    if (acc < 0x10) return;                                     /* jb 0x433e1 — no step */
    *(volatile uint8_t *)(uintptr_t)(esi + 0x1f) = (uint8_t)(acc - 0x10);

    if ((*(volatile uint8_t *)(uintptr_t)(esi + 9) & 8) &&
        *(volatile uint16_t *)(uintptr_t)(esi + 0x1a) != 0) {
        uint16_t ctr = (uint16_t)(*(volatile uint16_t *)(uintptr_t)(esi + 0x1a) - 1);
        *(volatile uint16_t *)(uintptr_t)(esi + 0x1a) = ctr;
        if (ctr == 0) {
            *(volatile uint8_t *)(uintptr_t)(esi + 9) &= 0xd7;
        } else {
            uint8_t f9 = *(volatile uint8_t *)(uintptr_t)(esi + 9);
            *(volatile uint8_t *)(uintptr_t)(esi + 9) = (uint8_t)(f9 & 0xbf);
            if (!(f9 & 0x40) && (int16_t)ctr >= 0x50) {         /* jl skips */
                uint32_t lcg = (uint32_t)G32(VA_g_ai_wander_rng) * 0x5e5u + 0x29u;
                G32(VA_g_ai_wander_rng) = (int32_t)lcg;
                if ((uint8_t)(lcg >> 8) <= 0x1e) {              /* cmp dh,0x1e; ja skip */
                    *(volatile uint8_t *)(uintptr_t)(esi + 9) =
                        (uint8_t)((*(volatile uint8_t *)(uintptr_t)(esi + 9) & 0xef)
                                  | (lcg & 0x10));
                }
            }
        }
    }
    /* 0x433a2: the movement step from the sincos table */
    int32_t dx = 0, dy = 0;
    if ((int16_t)*(volatile uint16_t *)(uintptr_t)(esi + 0x18) >= 0) {   /* jge */
        uint32_t idx = ((((uint32_t)*(volatile uint8_t *)(uintptr_t)(sub + 6) + 0x80u)
                         & 0xffu) << 2) & 0x3feu;
        dx = (int32_t)*(volatile int16_t *)GADDR(VA_g_sincos_table + idx);
        idx = (idx + 0x100u) & 0x3feu;                          /* inc bh */
        dy = (int32_t)*(volatile int16_t *)GADDR(VA_g_sincos_table + idx);
        dx <<= 5;
        dy <<= 5;
    }
    entity_apply_vertical_movement(dx, dy, esi, sub);    /* 0x433dc */
}

/* ---- entity_def_cache_lookup (0x1e2f6): 10-entry move-to-front LRU def cache -----------------
 * EAX = def-id. The cache is an intrusive singly-linked list over the node array at 0x819dc
 * (10 nodes x 0x6c; node +0 = next ptr, +4 = key word, +6.. = def attrs), head ptr at
 * g_entity_def_cache_head 0x819d8, allocated-count at g_entity_def_cache_count 0x81e14 (adjacent
 * to the array end). Node pointers are RUNTIME addresses of obj3 nodes (GADDR-computed, so the
 * stored values byte-match the original's rebased pointers in the absolute build).
 *
 *   HIT   -> move-to-front (unless already head); EAX = node.
 *   MISS  -> build into a 0x6c stack temp via entity_def_cache_build_entry; 0 -> EAX = 0 (no
 *            cache mutation — the temp is never copied); else append at node[count] (count<10,
 *            count++) or evict the list TAIL (count stays 10), link at head; EAX = the node.
 * The temp copy is fully deterministic: build_entity_def_record zero-fills all 0x6c bytes first.
 * KEY COMPARE IS FULL-WIDTH: `movzx esi,word[node+4]; cmp esi,ecx` — the 16-bit key is zero-
 * extended and compared against the WHOLE 32-bit id (an id > 0xffff never hits).
 * Corpus decompile shows this fn as void — the EAX return (node ptr / 0) is real and consumed
 * (revalidate_entity_def, spawn_entity_into_state_pool_a, savegame). Gotcha A1. */
uint32_t entity_def_cache_lookup(uint32_t id)
{
    uint32_t cur  = (uint32_t)G32(VA_g_entity_def_cache_head);          /* g_entity_def_cache_head */
    uint32_t prev = 0;

    while (cur != 0) {
        if ((uint32_t)*(volatile uint16_t *)(uintptr_t)(cur + 4) == id) {
            /* HIT */
            if (prev == 0)
                return cur;                          /* already at head */
            uint32_t oldhead = (uint32_t)G32(VA_g_entity_def_cache_head);
            uint32_t nxt     = *(volatile uint32_t *)(uintptr_t)cur;
            G32(VA_g_entity_def_cache_head) = (int32_t)cur;             /* head = cur */
            *(volatile uint32_t *)(uintptr_t)cur  = oldhead;  /* cur->next = old head */
            *(volatile uint32_t *)(uintptr_t)prev = nxt;      /* prev->next = cur's old next */
            return cur;
        }
        prev = cur;
        cur  = *(volatile uint32_t *)(uintptr_t)cur;
    }

    /* MISS: build the def into a stack temp (zero-filled by the builder on every reached path) */
    uint8_t temp[0x6c];
    if (entity_def_cache_build_entry((uint32_t)(uintptr_t)temp, id) == 0)
        return 0;                                    /* no record / no AsMonster -> EAX 0 */

    uint32_t count = (uint32_t)G32(VA_g_entity_def_cache_count);         /* g_entity_def_cache_count */
    if (count < 10u) {
        /* append at node[count] */
        uint32_t node = (uint32_t)GADDR(VA_g_entity_def_cache_nodes) + count * 0x6cu;
        memcpy((void *)(uintptr_t)node, temp, 0x6c);
        *(volatile uint32_t *)(uintptr_t)node = (uint32_t)G32(VA_g_entity_def_cache_head); /* node->next = head */
        G32(VA_g_entity_def_cache_count) = (int32_t)(count + 1u);
        G32(VA_g_entity_def_cache_head) = (int32_t)node;                /* head = node */
        return node;
    }
    /* evict the tail: walk to the node whose ->next == 0 (count==10 -> length >= 2) */
    uint32_t p = (uint32_t)G32(VA_g_entity_def_cache_head), tprev;
    do {
        tprev = p;
        p     = *(volatile uint32_t *)(uintptr_t)p;
    } while (*(volatile uint32_t *)(uintptr_t)p != 0);
    *(volatile uint32_t *)(uintptr_t)tprev = 0;      /* unlink tail (before the copy, as orig) */
    memcpy((void *)(uintptr_t)p, temp, 0x6c);
    *(volatile uint32_t *)(uintptr_t)p = (uint32_t)G32(VA_g_entity_def_cache_head);  /* tail->next = old head */
    G32(VA_g_entity_def_cache_head) = (int32_t)p;                       /* head = tail (count stays 10) */
    return p;
}

/* ==================== clusters B + D — spawn/destroy + the think loop ====================
 * The last five entity_ai fns; ALL non-idempotent (pool mutation, attack fire, RNG advance) ->
 * LIVE-SWAP tier only (group ROTH_LIFT=entity_ai; no oracle differential — the DoD anchor is the
 * in-game pass). Transcribed from the disasm; every callee is already
 * lifted (call-closed, zero call_orig bridges except the regs_t firers which bridge internally).
 *
 * Pool model (derived from the disasm; matches spawn/destroy/update consistently):
 *  - WALKER/projectile pool: 16 records, stride 0x1c, at 0x90fe4; live count 0x90fe0.
 *      rec+0 obj ptr | +4 angle | +6 sector | +8 mode (1=projectile 2=dying) | +0xa/+0xc frac
 *      accum | +0x10 z-vel | +0x14 damage id | +0x16 player damage | +0x18 speed | +0x19 ttl |
 *      +0x1a pool-A backref (idx+1)
 *  - state-pool-A (the ACTOR pool): 16 slots, stride 0x22, at 0x91e04; live count 0x91e00.
 *      slot+0 0x68-record ptr | +4 obj ptr | +8 flags (1=dying-fired… 5=special-idle, 8=attack
 *      recover, 0x20=corpse, 0x40=stagger, 0x40|1=rise) | +9 state bits | +0xb settle/knock
 *      speed | +0x16 owner sector | +0x1a phase counter | +0x1c projectile id | +0x1e attack
 *      delay | +0x21 pain timer
 *  - object-list offset table at g_map_objects_buffer+2 (2-byte entries, count 0x91df8);
 *    secondary-state records stride 0x1a at g_map_geometry_buffer + [0x91dfc]. */

/* inc/dec of a register's second byte (INC BH / DEC AH): wraps within the byte, no carry out. */
static uint32_t ea_bh_add(uint32_t v, uint32_t d)
{
    return (v & 0xffff00ffu) | ((((v >> 8) + d) & 0xffu) << 8);
}

/* the think loop's damage LCG (word state 0x7272c — distinct from aim's 0x72730 and the
 * ambient-sound LCG 0x72738): word = word*0x5e5 + 0x29, returns the new low word. */
static uint16_t ea_lcg_2c(void)
{
    uint32_t r = (uint32_t)(uint16_t)G16(VA_g_entity_damage_rng) * 0x5e5u + 0x29u;
    G16(VA_g_entity_damage_rng) = (uint16_t)r;
    return (uint16_t)r;
}

/* ---- destroy_dynamic_entity (0x41f24): drop an object's entity/actor pool links ---------------
 * EAX = object record ptr, EDX = 16-byte dest (the caller's scratch) that receives the object's
 * first 4 dwords (copied unconditionally, before the range check). GS in the original = the
 * objects-buffer selector (base [0x90aa4]) so gs:[off+n] == obj+n flat. Clears the walker-pool
 * link (obj bit1: rec+0 dword at 0x90fe4+idx-1 — which IS the record's obj ptr — count 0x90fe0)
 * and/or the pool-A link (bit0: slot dwords 0x91e04/0x91e08+idx-1, count 0x91e00), then scans
 * the object-list offset table for the owning sector (same walk as resolve_object_owner_sector),
 * removes the secondary-state record, and — if that sector's list is NOW EMPTY (count byte at
 * gs:[(u16)found], threaded back in ESI by 0x42056's clobber) — drops the sector record's
 * has-objects render bit1 at +0x16. QUIRK kept: the found-value register inherits the pool offset's
 * high word (only its full-32 zero test gates the remove call). EAX clobbered (no caller reads). */
void destroy_dynamic_entity(uint32_t eax_obj, uint32_t edx_dest)
{
    memcpy((void *)(uintptr_t)edx_dest, (const void *)(uintptr_t)eax_obj, 16);

    uint32_t objbuf = (uint32_t)G32(VA_g_map_objects_buffer);            /* g_map_objects_buffer (stored ptr) */
    uint32_t off = eax_obj - objbuf;
    if (off >= (uint32_t)G32(VA_g_object_buffer_free + 0x4))
        return;                                          /* jae: outside the object pool */
    volatile uint8_t *obj = (volatile uint8_t *)(uintptr_t)eax_obj;

    if (obj[9] & 2) {                                    /* linked in the walker pool */
        int32_t bx = (int32_t)*(volatile uint16_t *)(uintptr_t)(eax_obj + 0xc) - 1;
        if (bx >= 0) {                                   /* jl skips */
            *(volatile uint16_t *)(uintptr_t)(eax_obj + 0xc) = 0;
            *(volatile uint32_t *)GADDR(VA_g_dynamic_entity_table + (uint32_t)bx) = 0;   /* rec+0 (obj ptr) */
            G32(VA_g_dynamic_entity_count) -= 1;
        }
    }
    if (obj[9] & 1) {                                    /* linked in pool-A */
        obj[9] -= 1;
        int32_t bx = (int32_t)*(volatile uint16_t *)(uintptr_t)(eax_obj + 0xc) - 1;
        if (bx >= 0) {
            *(volatile uint16_t *)(uintptr_t)(eax_obj + 0xc) = 0;
            *(volatile uint32_t *)GADDR((VA_g_state_pool_a_records + 0x4) + (uint32_t)bx) = 0;   /* slot+4 (obj ptr) */
            *(volatile uint32_t *)GADDR(VA_g_state_pool_a_records + (uint32_t)bx) = 0;   /* slot+0 (record) */
            G32(VA_g_state_pool_a_count) -= 1;
        }
    }

    /* owning-sector scan (the resolve_object_owner_sector walk, inlined by the original):
     * largest table word <= the object's 16-bit offset; do-while — one pass even at count 0 */
    int16_t cx = (int16_t)(uint32_t)G32(VA_g_sector_count);        /* g_sector_count */
    uint32_t hi = off & 0xffff0000u;                     /* the stale-high-word quirk */
    uint16_t bx16 = (uint16_t)off;
    uint32_t si = 2, found = 0, found_si = 0;
    do {
        uint16_t ax = *(volatile uint16_t *)(uintptr_t)(objbuf + si);
        if (bx16 >= ax && (int16_t)ax > (int16_t)(uint16_t)found) {
            found = hi | ax;                             /* mov edi,eax (full 32) */
            found_si = si;
        }
        si += 2;
    } while (--cx > 0);
    if (found == 0)
        return;

    remove_secondary_state_record(found, off);    /* 0x42056 (EAX=value, EBX=offset) */
    /* 0x41fe8 `cmp byte gs:[si],0` — ESI here is NOT the scan cursor: 0x42056 does `mov esi,eax`
     * @0x42063 and returns ESI = the list-head offset on EVERY exit path (early exits unrestored;
     * shift path push@0x42085/pop@0x420db). Addressing uses 16-bit SI, so the tested byte is
     * gs:[(u16)found] = the list's count byte, which 0x42056 just decremented. Semantics: clear the
     * sector's has-objects render flag (+0x16 bit1, gate at 0x2a13a) only when the list is NOW
     * EMPTY. The old C tested objbuf+si (the byte after the directory = the first list's live count
     * byte) — once that sector emptied, EVERY destroy wrongly cleared its own sector's flag: the
     * "cursed sector" bug (items/enemy stop rendering on pickup/death/projectile-impact). */
    if (*(volatile uint8_t *)(uintptr_t)(objbuf + (uint16_t)found) != 0)
        return;
    uint32_t rec = 13u * (found_si - 2u);                /* = 0x1a * table index */
    rec += (uint32_t)G32(VA_g_sector_section_offset);
    rec += (uint32_t)G32(VA_g_map_geometry_buffer);                       /* g_map_geometry_buffer */
    *(volatile uint8_t *)(uintptr_t)(rec + 0x16) &= 0xfd;
}

/* ---- spawn_entity_at_position (0x4254e): create a walker/projectile pool entity ---------------
 * SIX register args (regs_t in — gotcha A0): EAX=x, EDX=y, EBX=the object's +0xa word (floor z),
 * ECX=z, EBP=angle source (negated, second byte decremented, then >>1 for the object heading
 * byte), ESI=a dword stored at rec+0x10 (z-velocity). Returns EAX = the pool record ptr, 0 when
 * the pool is full (16), no sector resolves, or no object slot allocates. Stamps the entry
 * breadcrumb [0x8499c]=0x4254d (relocated code addr) exactly as the original. */
uint32_t spawn_entity_at_position(const regs_t *in)
{
    G32(VA_g_current_proc_tag) = (int32_t)(0x4254du + OBJ_DELTA);
    if ((uint32_t)G32(VA_g_dynamic_entity_count) >= 0x10u)
        return 0;
    if ((uint32_t)G32(VA_g_object_buffer_free) < 0x12u)
        reserve_object_buffer_space();            /* 0x420e1 */
    uint32_t sec = locate_sector_at_position(in->eax, in->edx, in->ebx, 0);
    if ((sec & 0xffffu) == 0)
        return 0;                                        /* or ax,ax; je */
    uint32_t secrec = sec + (uint32_t)G32(VA_g_map_geometry_buffer);
    uint32_t slot = alloc_object_record_slot(sec);   /* 0x42199 */
    if ((slot & 0xffffu) == 0)
        return 0;
    *(volatile uint8_t *)(uintptr_t)(secrec + 0x16) |= 2;
    uint32_t obj = (slot & 0xffffu) + (uint32_t)G32(VA_g_map_objects_buffer);
    /* find_free_entity_slot 0x42626 returns the slot POINTER in ESI (what this, its only caller,
     * consumes) and the index in EAX; the lifted helper kept only the EAX index (in-game
     * crash: using the index as a pointer wrote address 6). Reconstruct ESI; the
     * count guard above guarantees a free slot, so index 0 unambiguously means slot 0. */
    uint32_t rec = (uint32_t)GADDR(VA_g_dynamic_entity_table)
                 + (uint32_t)find_free_entity_slot() * 0x1cu;
    G32(VA_g_dynamic_entity_count) += 1;
    *(volatile uint16_t *)(uintptr_t)(obj + 0xc) = (uint16_t)(rec - (uint32_t)GADDR(VA_g_dynamic_entity_count + 0x3));
    *(volatile uint16_t *)(uintptr_t)(rec + 6)  = (uint16_t)sec;
    *(volatile uint16_t *)(uintptr_t)(obj)      = (uint16_t)in->eax;
    *(volatile uint16_t *)(uintptr_t)(obj + 2)  = (uint16_t)in->edx;
    *(volatile uint16_t *)(uintptr_t)(obj + 0xa) = (uint16_t)in->ebx;
    *(volatile uint16_t *)(uintptr_t)(obj + 4)  = (uint16_t)in->ecx;
    *(volatile uint8_t  *)(uintptr_t)(obj + 7)  = 2;
    *(volatile uint8_t  *)(uintptr_t)(obj + 9)  = 2;
    *(volatile uint16_t *)(uintptr_t)(obj + 0xe) = 0;
    *(volatile uint8_t  *)(uintptr_t)(obj + 8)  = 0x80;
    uint32_t ang = ea_bh_add(0u - in->ebp, 0xffu);       /* neg eax; dec ah */
    *(volatile uint16_t *)(uintptr_t)(rec + 4)  = (uint16_t)ang;
    *(volatile uint32_t *)(uintptr_t)(rec + 0x10) = in->esi;
    *(volatile uint32_t *)(uintptr_t)(rec)      = obj;
    *(volatile uint8_t  *)(uintptr_t)(rec + 0x18) = 8;
    *(volatile uint16_t *)(uintptr_t)(rec + 0xa) = 0;
    *(volatile uint16_t *)(uintptr_t)(rec + 0xc) = 0;
    *(volatile uint8_t  *)(uintptr_t)(obj + 6)  = (uint8_t)((int32_t)ang >> 1);   /* sar */
    return rec;
}

/* ---- spawn_entity_into_state_pool_a (0x4f00d): create an ACTOR in state-pool-A ----------------
 * Args: AH = index into the 0x68-stride actor-record array ([0x85cf4]); ESI = object record.
 * RETURNS EDX (gotcha A0 — both callers `test edx,edx`): slot - 0x91e03 (the idx*0x22+1 backref,
 * also stored at obj+0xc) on success, 0 when the pool is full. EAX is clobbered by the original
 * (def-lookup / sector-resolve results); neither caller reads it. */
int32_t spawn_entity_into_state_pool_a(const regs_t *in)
{
    if ((uint32_t)G32(VA_g_state_pool_a_count) == 0x10u)
        return 0;
    uint32_t slot = (uint32_t)GADDR(VA_g_state_pool_a_records);
    for (uint32_t i = 0; *(volatile uint32_t *)(uintptr_t)(slot + 4) != 0; ) {
        slot += 0x22; i++;
        if (i >= 0x10u)
            return 0;                                    /* full: edx = 0 */
    }
    uint32_t esi = in->esi;
    uint32_t def = entity_def_cache_lookup(
        (uint32_t)*(volatile uint16_t *)(uintptr_t)(esi + 4));    /* id = word[obj+4] */
    uint32_t rec = ((in->eax >> 8) & 0xffu) * 0x68u + (uint32_t)G32(VA_g_ademo_das_fat_buffer + 0x4);
    *(volatile uint32_t *)(uintptr_t)slot = rec;
    *(volatile uint32_t *)(uintptr_t)rec = def;
    *(volatile uint8_t  *)(uintptr_t)(slot + 9) = 0;
    uint32_t edx = def;
    if (edx != 0) {
        *(volatile uint8_t *)(uintptr_t)(slot + 9) = *(volatile uint8_t *)(uintptr_t)(edx + 0x69);
        edx = *(volatile uint32_t *)(uintptr_t)(edx + 8);
    }
    *(volatile uint32_t *)(uintptr_t)(slot + 0xc) = edx;
    *(volatile uint16_t *)(uintptr_t)(rec + 0x60) =
        *(volatile uint16_t *)(uintptr_t)(esi + 4);
    *(volatile uint32_t *)(uintptr_t)(slot + 4) = esi;
    *(volatile uint16_t *)(uintptr_t)(slot + 0x16) =
        (uint16_t)resolve_object_owner_sector(esi);        /* 0x4f263 */
    *(volatile uint8_t  *)(uintptr_t)(slot + 8) = 0;
    *(volatile uint16_t *)(uintptr_t)(slot + 0x1c) = 0;
    uint32_t backref = slot - (uint32_t)GADDR(VA_g_state_pool_a_count + 0x3);
    *(volatile uint16_t *)(uintptr_t)(esi + 0xc) = (uint16_t)backref;
    *(volatile uint8_t  *)(uintptr_t)(esi + 9) |= 1;
    G32(VA_g_state_pool_a_count) += 1;
    return (int32_t)backref;
}

/* ==================== cluster D — the think loop ==================== */

/* -- update_dynamic_entities helpers (one static per state path; ud_ prefix) -- */

/* walker: integrate the angle-driven velocity (sincos * speed*4, 16.16 accum at rec+0xa/+0xc),
 * relocate the sector, clearance-check, move — or turn (angle high byte--) when blocked. */
static void ud_walker(uint32_t rec)
{
    uint32_t obj = *(volatile uint32_t *)(uintptr_t)rec;
    uint32_t ebx = (uint32_t)(uint16_t)(*(volatile uint16_t *)(uintptr_t)(rec + 4) - 0x100u);
    ebx += ebx;                                          /* dec bh; add ebx,ebx */
    int32_t A = sincos_lookup(ebx);
    int32_t B = sincos_lookup(ea_bh_add(ebx, 1)); /* inc bh */
    int32_t spd4 = (int32_t)((uint32_t)*(volatile uint8_t *)(uintptr_t)(rec + 0x18) << 2);
    uint32_t a = (uint32_t)(A * spd4);                   /* imul */
    uint32_t b = (uint32_t)(B * spd4);

    /* add word[rec+0xa],ax ; adc bx,[obj]  (16.16 accumulate with carry into the position) */
    uint32_t sum = (uint32_t)*(volatile uint16_t *)(uintptr_t)(rec + 0xa) + (uint16_t)a;
    *(volatile uint16_t *)(uintptr_t)(rec + 0xa) = (uint16_t)sum;
    uint16_t newx = (uint16_t)((a >> 16) + *(volatile uint16_t *)(uintptr_t)obj + (sum >> 16));
    sum = (uint32_t)*(volatile uint16_t *)(uintptr_t)(rec + 0xc) + (uint16_t)b;
    *(volatile uint16_t *)(uintptr_t)(rec + 0xc) = (uint16_t)sum;
    uint16_t newy = (uint16_t)((b >> 16) + *(volatile uint16_t *)(uintptr_t)(obj + 2) + (sum >> 16));

    uint32_t sec = locate_sector_at_position(newx, newy,
        (uint32_t)*(volatile uint16_t *)(uintptr_t)(obj + 0xa),
        (uint32_t)*(volatile uint16_t *)(uintptr_t)(rec + 6));
    if ((uint16_t)sec != 0) {
        int cf = (*(volatile uint8_t *)(uintptr_t)(rec + 8) & 1)      /* re-test (faithful) */
                     ? check_projectile_sector_clearance(sec, obj)
                     : check_entity_sector_clearance(sec, obj);
        if (!cf) {
            if ((uint16_t)sec != *(volatile uint16_t *)(uintptr_t)(rec + 6)) {
                uint16_t old = *(volatile uint16_t *)(uintptr_t)(rec + 6);
                *(volatile uint16_t *)(uintptr_t)(rec + 6) = (uint16_t)sec;
                add_secondary_state_record(old, sec & 0xffffu, obj);   /* 0x42c72 */
                obj = *(volatile uint32_t *)(uintptr_t)rec;                   /* re-read */
            }
            *(volatile uint16_t *)(uintptr_t)(obj + 2) = newy;                /* moved */
            *(volatile uint16_t *)(uintptr_t)obj = newx;
            return;
        }
    }
    /* blocked/no sector: turn — angle high byte--, heading byte = angle>>1 */
    uint16_t ang = (uint16_t)ea_bh_add(*(volatile uint16_t *)(uintptr_t)(rec + 4), 0xffu);
    *(volatile uint16_t *)(uintptr_t)(rec + 4) = ang;
    *(volatile uint8_t *)(uintptr_t)(obj + 6) = (uint8_t)(ang >> 1);
}

/* projectile: sweep along the heading byte (obj+6), then resolve the hit/expiry:
 * ctx 0x911a4 (+0x18 landing sector, +0x1a hit flags, +0x1c hit object). */
static void ud_projectile(uint32_t rec)
{
    uint32_t obj = *(volatile uint32_t *)(uintptr_t)rec;
    uint32_t ebx = (uint32_t)(uint8_t)(*(volatile uint8_t *)(uintptr_t)(obj + 6) + 0x80u) << 2;
    int32_t A = sincos_lookup(ebx);
    int32_t B = sincos_lookup(ea_bh_add(ebx, 1));
    int32_t spd = (int32_t)(uint32_t)*(volatile uint8_t *)(uintptr_t)(rec + 0x18);
    uint32_t ctx = (uint32_t)GADDR(VA_g_dynamic_entity_table + 0x1c0);
    G32(VA_g_projectile_collision_width) = (int32_t)(uint32_t)*(volatile uint8_t *)(uintptr_t)(rec + 9);
    G32(VA_g_dos_dta_name + 0x6e) = (int32_t)rec;
    sweep_move_with_collision((uint32_t)((A * spd) << 3), ctx,
                                     (uint32_t)((B * spd) << 3),
                                     (uint32_t)spd,
                                     (uint32_t)((*(volatile int32_t *)(uintptr_t)(rec + 0x10) * spd) << 10),
                                     rec);               /* 0x3e351 */
    uint32_t sec = (uint32_t)*(volatile uint16_t *)(uintptr_t)(ctx + 0x18);
    uint8_t hitf = *(volatile uint8_t *)(uintptr_t)(ctx + 0x1a);

    if (!(hitf & 3)) {                                   /* clean flight */
        if (!check_projectile_sector_clearance(sec, obj)) {
            uint8_t ttl = *(volatile uint8_t *)(uintptr_t)(rec + 0x19);
            if (ttl != 0) {
                *(volatile uint8_t *)(uintptr_t)(rec + 0x19) = (uint8_t)(ttl - 1);
                if (ttl == 1)
                    goto destroy_mark;                   /* ttl hit 0 */
            }
            if ((uint16_t)sec != *(volatile uint16_t *)(uintptr_t)(rec + 6)) {
                uint16_t old = *(volatile uint16_t *)(uintptr_t)(rec + 6);
                *(volatile uint16_t *)(uintptr_t)(rec + 6) = (uint16_t)sec;
                add_secondary_state_record(old, sec, *(volatile uint32_t *)(uintptr_t)rec);
            }
            return;
        }
        goto expire;                                     /* clearance CF */
    }

    if (hitf & 2) {                                      /* hit an object (or the player) */
        uint32_t hobj = *(volatile uint32_t *)(uintptr_t)(ctx + 0x1c);
        if (hobj == 0) {
            /* the player: scale word[rec+0x16] by the LCG, halve, apply + pain sound */
            if (G32(VA_g_player_health) > 0) {
                uint32_t dmg = (uint32_t)*(volatile uint16_t *)(uintptr_t)(rec + 0x16);
                uint32_t part = ((uint32_t)ea_lcg_2c() * dmg) >> 16;
                int32_t val = (int32_t)(dmg + part) >> 1;
                if (val != 0) {
                    apply_reduced_damage_to_player((uint32_t)val);      /* 0x320c6 */
                    play_distance_variant_sound_regs(0, part,
                        (uint32_t)GADDR(VA_g_help_overlay_enabled + 0x4), (uint32_t)val);              /* 0x4269b */
                }
            }
            goto expire;
        }
        if (*(volatile uint8_t *)(uintptr_t)(hobj + 9) & 1) {   /* hit object has an actor */
            int32_t idx = (int32_t)*(volatile uint16_t *)(uintptr_t)(hobj + 0xc) - 1;
            if (idx >= 0) {
                uint32_t slot = (uint32_t)idx + (uint32_t)GADDR(VA_g_state_pool_a_records);
                if (!(*(volatile uint8_t *)(uintptr_t)(slot + 8) & 0x20) &&
                    !(*(volatile uint8_t *)(uintptr_t)(slot + 9) & 1)) {
                    uint32_t base = compute_projectile_hit_damage(
                        (uint32_t *)(uintptr_t)slot,
                        *(volatile uint16_t *)(uintptr_t)(rec + 0x14));        /* 0x427f3 */
                    uint32_t part = ((uint32_t)ea_lcg_2c() * base) >> 16;
                    int32_t dmg = (int32_t)(base + part) >> 1;
                    if (dmg != 0) {
                        /* d2 = the inherited ECX at the original call (0x42a5f) = dmg (disasm-proven,
                         * ECX-preserved through revalidate/0x1e2f6); resolve threads it to the
                         * distance-variant impact sound. */
                        resolve_projectile_target_entity(slot, (uint32_t)dmg);  /* 0x4271c (EAX preserved) */
                        *(volatile uint8_t *)(uintptr_t)(slot + 0x21) = 0x18;  /* pain timer */
                        int32_t hp = *(volatile int32_t *)(uintptr_t)(slot + 0xc) - dmg;
                        *(volatile int32_t *)(uintptr_t)(slot + 0xc) = hp;
                        if (hp <= 0) {                   /* killed: hp := damage, dying flag */
                            *(volatile int32_t *)(uintptr_t)(slot + 0xc) = dmg;
                            *(volatile uint8_t *)(uintptr_t)(slot + 9) = 1;
                        }
                    }
                }
            }
        }
    }
    /* wall (or resolved object hit): flag the firing actor's pool-A slot */
    {
        int16_t backref = (int16_t)*(volatile uint16_t *)(uintptr_t)(rec + 0x1a);
        if (backref > 0)
            *(volatile uint8_t *)(uintptr_t)((uint32_t)(uint16_t)backref
                                             + (uint32_t)GADDR(VA_g_state_pool_a_count + 0x3) + 9) |= 0x80;
    }
expire:
    if (init_projectile_from_item(rec) == 0)      /* 0x42b8c: CF=0 -> respawned */
        return;
    goto destroy;
destroy_mark:
    {
        int16_t backref = (int16_t)*(volatile uint16_t *)(uintptr_t)(rec + 0x1a);
        if (backref > 0)
            *(volatile uint8_t *)(uintptr_t)((uint32_t)(uint16_t)backref
                                             + (uint32_t)GADDR(VA_g_state_pool_a_count + 0x3) + 9) |= 0x80;
    }
destroy:
    {
        uint8_t scratch[0x20];
        destroy_dynamic_entity(*(volatile uint32_t *)(uintptr_t)rec,
                                      (uint32_t)(uintptr_t)scratch);
    }
}

/* ---- update_dynamic_entities (0x42872): the per-entity walker/projectile/dying body ----------
 * No register args (all pushed/popped); void. Walks the 0x1c-stride pool at 0x90fe4 counting
 * down the LIVE count 0x90fe0 (empty slots don't count — the original relies on the count
 * invariant exactly as this does). LIVE-SWAP tier. */
void update_dynamic_entities(void)
{
    int32_t left = G32(VA_g_dynamic_entity_count);
    if (left == 0)
        return;
    uint32_t rec = (uint32_t)GADDR(VA_g_dynamic_entity_table);
    for (;;) {
        if (*(volatile uint32_t *)(uintptr_t)rec != 0) {
            uint8_t mode = *(volatile uint8_t *)(uintptr_t)(rec + 8);
            if (mode & 1) {
                ud_projectile(rec);
            } else if (mode & 2) {                       /* dying: count 0x100 ticks, drop */
                uint16_t c = *(volatile uint16_t *)(uintptr_t)(rec + 0x1a);
                if (c < 0x100u) {
                    *(volatile uint16_t *)(uintptr_t)(rec + 0x1a) = (uint16_t)(c + 1);
                } else {
                    uint8_t scratch[0x20];
                    destroy_dynamic_entity(*(volatile uint32_t *)(uintptr_t)rec,
                                                  (uint32_t)(uintptr_t)scratch);
                }
            } else {
                ud_walker(rec);
            }
            left--;
        }
        rec += 0x1c;
        if (left <= 0)
            break;
    }
}

/* -- tick_dynamic_entities helpers (per-state statics; ta_ prefix; edi = the pool-A slot) -- */

/* corpse settle (flags&0x20): step the object's +0xa toward the owner sector's floor word at an
 * accelerating clamp (slot+0xb, capped by the AL-signed compare quirk), then age the phase
 * counter — pinned at 0xffff — until it passes 0x100 with nothing left to settle: decay. */
static void ta_corpse(uint32_t edi)
{
    *(volatile uint8_t *)(uintptr_t)(edi + 0x21) = 0;
    uint32_t secoff = (uint32_t)*(volatile uint16_t *)(uintptr_t)(edi + 0x16);
    if (secoff != 0) {
        uint32_t cell = secoff + (uint32_t)G32(VA_g_map_geometry_buffer);
        uint32_t sub = *(volatile uint32_t *)(uintptr_t)(edi + 4);
        int16_t d = (int16_t)(*(volatile uint16_t *)(uintptr_t)(cell + 2)
                              - *(volatile uint16_t *)(uintptr_t)(sub + 0xa));
        if (d != 0) {
            uint32_t step = (uint32_t)*(volatile uint8_t *)(uintptr_t)(edi + 0xb) + 1u;
            uint32_t neg = 0u - step;                    /* neg eax */
            if (d > (int16_t)(uint16_t)step) d = (int16_t)(uint16_t)step;
            if (d < (int16_t)(uint16_t)neg)  d = (int16_t)(uint16_t)neg;
            *(volatile uint16_t *)(uintptr_t)(sub + 0xa) += (uint16_t)d;
            if ((int8_t)(uint8_t)neg > (int8_t)0xe0)     /* cmp al,0xe0; jg — clamp growth */
                (*(volatile uint8_t *)(uintptr_t)(edi + 0xb))++;
            goto count;                                  /* jmp 0x43016 (skips the decay gate) */
        }
    }
    if (*(volatile uint16_t *)(uintptr_t)(edi + 0x1a) > 0x100u) {
        /* decay (0x4302b): sink by rec+0x56/+0x5a (>0x1200 keeps the corpse object, frees the
         * slot), else full destroy */
        *(volatile uint16_t *)(uintptr_t)(edi + 0x1a) = 0;
        uint32_t rec = *(volatile uint32_t *)(uintptr_t)edi;
        uint16_t z = *(volatile uint16_t *)(uintptr_t)(rec + 0x56);
        if (*(volatile uint8_t *)(uintptr_t)(edi + 8) & 1)
            z = *(volatile uint16_t *)(uintptr_t)(rec + 0x5a);
        uint32_t sub = *(volatile uint32_t *)(uintptr_t)(edi + 4);
        if (z > 0x1200u) {
            *(volatile uint16_t *)(uintptr_t)(sub + 4) = (uint16_t)(z - 0x1000u);
            if (*(volatile uint16_t *)(uintptr_t)(sub + 0xe) == 0)
                *(volatile uint8_t *)(uintptr_t)(sub + 9) |= 2;
            *(volatile uint8_t *)(uintptr_t)(sub + 9) &= 0xfe;
            *(volatile uint16_t *)(uintptr_t)(sub + 0xc) = 0;
            *(volatile uint32_t *)(uintptr_t)edi = 0;
            *(volatile uint32_t *)(uintptr_t)(edi + 4) = 0;
            G32(VA_g_state_pool_a_count) -= 1;
        } else {
            uint8_t scratch[0x20];
            destroy_dynamic_entity(sub, (uint32_t)(uintptr_t)scratch);
        }
        return;
    }
count:
    if (*(volatile uint16_t *)(uintptr_t)(edi + 0x1a) != 0xffffu)
        (*(volatile uint16_t *)(uintptr_t)(edi + 0x1a))++;
}

/* stagger (flags&0x40): knockdown (bit0 clear — sink +2/tick) or rise (bit0 set — lift -2/tick),
 * slot+0xb tracks the remaining offset. Returns 1 when the FALL phase times out into THINK. */
static int ta_stagger(uint32_t edi, uint8_t fl)
{
    *(volatile uint8_t *)(uintptr_t)(edi + 0x21) = 0;
    uint16_t c = *(volatile uint16_t *)(uintptr_t)(edi + 0x1a);
    uint32_t sub;
    if (fl & 1) {                                        /* RISE (0x43114) */
        if (c <= 0x100u) {
            *(volatile uint16_t *)(uintptr_t)(edi + 0x1a) = (uint16_t)(c + 1);
            sub = *(volatile uint32_t *)(uintptr_t)(edi + 4);
            uint8_t b = *(volatile uint8_t *)(uintptr_t)(edi + 0xb);
            if (b != 0) {
                *(volatile uint16_t *)(uintptr_t)(sub + 0xa) -= 2;
                *(volatile uint8_t *)(uintptr_t)(edi + 0xb) = (uint8_t)(b >= 2 ? b - 2 : 0);
            }
            return 0;
        }
        /* rise complete (0x43146): back to knockdown-armed stagger + the rise sound */
        *(volatile uint8_t *)(uintptr_t)(edi + 8) &= 0xbc;
        *(volatile uint8_t *)(uintptr_t)(edi + 8) |= 0x40;
        *(volatile uint16_t *)(uintptr_t)(edi + 0x1a) = 0;
        *(volatile uint8_t *)(uintptr_t)(edi + 0xb) = 0x50;
        uint32_t def = *(volatile uint32_t *)(uintptr_t)(*(volatile uint32_t *)(uintptr_t)edi);
        if (def != 0) {
            uint32_t snd = (uint32_t)*(volatile uint16_t *)(uintptr_t)(def + 0x64);
            if (snd != 0) {
                sub = *(volatile uint32_t *)(uintptr_t)(edi + 4);
                play_entity_sound(snd - 1, 0,
                    (uint32_t)*(volatile uint16_t *)(uintptr_t)sub,
                    (uint32_t)*(volatile uint16_t *)(uintptr_t)(sub + 2));     /* 0x271c4 */
            }
        }
        return 0;
    }
    /* FALL (0x430b5) */
    if (c <= 0x100u) {
        *(volatile uint16_t *)(uintptr_t)(edi + 0x1a) = (uint16_t)(c + 1);
        sub = *(volatile uint32_t *)(uintptr_t)(edi + 4);
        uint8_t b = *(volatile uint8_t *)(uintptr_t)(edi + 0xb);
        if (b != 0) {
            *(volatile uint16_t *)(uintptr_t)(sub + 0xa) += 2;
            *(volatile uint8_t *)(uintptr_t)(edi + 0xb) = (uint8_t)(b >= 2 ? b - 2 : 0);
        }
        return 0;
    }
    /* fall complete (0x430e7): clear, unfreeze the object, special-idle if rec+0x62 -> THINK */
    *(volatile uint16_t *)(uintptr_t)(edi + 0x1a) = 0;
    *(volatile uint8_t *)(uintptr_t)(edi + 0xb) = 0;
    *(volatile uint8_t *)(uintptr_t)(edi + 8) &= 0xbc;
    sub = *(volatile uint32_t *)(uintptr_t)(edi + 4);
    *(volatile uint8_t *)(uintptr_t)(sub + 7) &= 0xbf;
    uint32_t rec = *(volatile uint32_t *)(uintptr_t)edi;
    if (*(volatile uint16_t *)(uintptr_t)(rec + 0x62) != 0) {
        *(volatile uint8_t *)(uintptr_t)(edi + 8) = 5;
        *(volatile uint16_t *)(uintptr_t)(edi + 0x1a) = 0;
    }
    return 1;
}

/* attack recover (flags&8): once the phase counter reaches the attack delay (slot+0x1e), spawn
 * the queued projectile at the player; keep aiming+gravity until 0x100 ticks, then back to
 * THINK (returns 1). */
static int ta_attack_recover(uint32_t edi)
{
    *(volatile uint8_t *)(uintptr_t)(edi + 0x21) = 0;
    uint8_t delay = *(volatile uint8_t *)(uintptr_t)(edi + 0x1e);
    if (delay != 0 &&
        (uint8_t)*(volatile uint16_t *)(uintptr_t)(edi + 0x1a) >= delay) {
        *(volatile uint8_t *)(uintptr_t)(edi + 0x1e) = 0;
        uint32_t proj = (uint32_t)*(volatile uint16_t *)(uintptr_t)(edi + 0x1c);
        if (proj != 0) {
            uint32_t rec = *(volatile uint32_t *)(uintptr_t)edi;
            if (rec != 0) {
                uint32_t def = *(volatile uint32_t *)(uintptr_t)rec;
                if (def != 0) {                          /* alert level bump */
                    uint8_t lvl = *(volatile uint8_t *)(uintptr_t)(def + 0x68);
                    if (lvl > (uint8_t)G8(VA_g_render_sector_walk_mode + 0x23))
                        G8(VA_g_render_sector_walk_mode + 0x23) = lvl;
                }
            }
            G32(VA_g_dos_dta_name + 0x62) = 0x40;
            spawn_object_projectile_at_player(proj,
                *(volatile uint32_t *)(uintptr_t)(edi + 4));      /* 0x42300 */
        }
    }
    uint16_t c = *(volatile uint16_t *)(uintptr_t)(edi + 0x1a);
    if (c <= 0x100u) {
        *(volatile uint16_t *)(uintptr_t)(edi + 0x1a) = (uint16_t)(c + 1);
        aim_enemy_at_player(0x2710, edi);         /* 0x4355a */
        tick_entity_vertical_only(edi);           /* 0x43402 */
        return 0;
    }
    *(volatile uint16_t *)(uintptr_t)(edi + 0x1a) = 0;
    *(volatile uint8_t *)(uintptr_t)(edi + 8) &= 0xf4;
    return 1;                                            /* jmp 0x42df5 (THINK) */
}

/* the ambient/idle sound roll (THINK, def+0x18 table present): countdown dword 0x7273c; on
 * expiry roll the 0x72738 LCG (dword-read quirk kept), pick table entry 1..count, play it
 * (0 = silent, 0xffff = the "special" longer respite), then re-arm the countdown scaled by the
 * live actor count. */
static void ta_ambient_sound(uint32_t edi, uint32_t def)
{
    int32_t cd = G32(VA_g_actor_anim_scheduler) - 1;
    if (cd >= 0) {                                       /* jns: just count down */
        G32(VA_g_actor_anim_scheduler) = cd;
        return;
    }
    uint32_t r = (uint32_t)G32(VA_g_ai_anim_rng) >> 8;
    if (r == 0) {
        r = (uint32_t)G32(VA_g_ai_anim_rng_seed) * 0x5e5u + 0x29u;     /* dword read, word writeback (quirk) */
        G16(VA_g_ai_anim_rng_seed) = (uint16_t)r;
    }
    G32(VA_g_ai_anim_rng) = (int32_t)r;
    uint32_t rnd8 = r & 0xffu;
    uint32_t count = (uint32_t)*(volatile uint16_t *)(uintptr_t)(def + 0x18);
    uint32_t idx;
    if (count == 1) {
        idx = 1;
    } else {
        uint32_t p = (count * rnd8) >> 8;
        idx = ((uint16_t)p < (uint16_t)count) ? p + 1 : count;
    }
    uint32_t entry = (uint32_t)*(volatile uint16_t *)(uintptr_t)(def + 0x18 + (idx & 0xffffu) * 2);
    uint32_t add = 0x46;
    if (entry == 0xffffu) {
        add = 0xaa;
    } else if (entry != 0) {
        uint32_t sub = *(volatile uint32_t *)(uintptr_t)(edi + 4);
        play_entity_object_sound(entry - 1, rnd8,
            (uint32_t)*(volatile uint16_t *)(uintptr_t)sub,
            (uint32_t)*(volatile uint16_t *)(uintptr_t)(sub + 2));  /* 0x271e8 */
    }
    G32(VA_g_actor_anim_scheduler) = (int32_t)((((rnd8 & 0xfu) + add) * ((uint32_t)G32(VA_g_state_pool_a_count) + 1u)) >> 3);
}

/* THINK (0x42df5): revalidate the def (EBX re-read from the record slot after — the A1 thread),
 * publish speed/aim-limit to 0x72740/0x72744 (special-idle 5 forces 0x2710 + its own counter),
 * ambient sound, then locomotion + aim. */
static void ta_think(uint32_t edi)
{
    uint32_t rec = *(volatile uint32_t *)(uintptr_t)edi;
    uint32_t def = *(volatile uint32_t *)(uintptr_t)rec;
    if (def == 0)
        return;
    if (revalidate_entity_def((uint8_t *)(uintptr_t)rec, (uint8_t *)(uintptr_t)def))
        return;                                          /* CF: stale def */
    def = *(volatile uint32_t *)(uintptr_t)rec;          /* the callee re-threads EBX via [rec] */
    G8(VA_g_actor_move_rate) = *(volatile uint8_t *)(uintptr_t)(def + 0xc);
    G32(VA_g_actor_tick_rate) = (int32_t)(uint32_t)*(volatile uint16_t *)(uintptr_t)(def + 0xe);
    if (*(volatile uint8_t *)(uintptr_t)(edi + 8) == 5) {
        G32(VA_g_actor_tick_rate) = 0x2710;
        uint16_t c = *(volatile uint16_t *)(uintptr_t)(edi + 0x1a);
        if (c <= 0x100u) {
            *(volatile uint16_t *)(uintptr_t)(edi + 0x1a) = (uint16_t)(c + 1);
        } else {
            *(volatile uint16_t *)(uintptr_t)(edi + 0x1a) = 0;
            *(volatile uint8_t *)(uintptr_t)(edi + 8) &= 0xf8;
        }
    }
    if (*(volatile uint16_t *)(uintptr_t)(def + 0x18) != 0)
        ta_ambient_sound(edi, def);
    G32(VA_g_actor_tick_rate + 0x4) = 0;
    update_actor_movement_ai(0, edi);             /* 0x43326 */
    if (!(*(volatile uint8_t *)(uintptr_t)(edi + 9) & 8))
        aim_enemy_at_player((uint32_t)G32(VA_g_actor_tick_rate), edi);    /* 0x4355a */
}

/* one pool-A slot dispatch (the 0x42dc6..0x42def test chain) */
static void ta_slot(uint32_t edi, const regs_t *in)
{
    if (*(volatile uint8_t *)(uintptr_t)(edi + 9) & 1) { /* died this frame: fire the trigger */
        *(volatile uint8_t *)(uintptr_t)(edi + 9) &= 0xfe;
        regs_t io; memset(&io, 0, sizeof io);
        io.eax = in->eax; io.ebx = in->ebx; io.ecx = in->ecx; io.edx = in->edx;
        io.esi = in->esi; io.ebp = in->ebp;
        io.edi = edi;
        fire_entity_pending_trigger(&io);         /* 0x42793 */
        return;
    }
    uint8_t fl = *(volatile uint8_t *)(uintptr_t)(edi + 8);
    if (fl & 0x20) {
        ta_corpse(edi);
        return;
    }
    if (fl & 0x40) {
        if (!ta_stagger(edi, fl))
            return;
    } else if (fl & 8) {
        if (!ta_attack_recover(edi))
            return;
    } else if (*(volatile uint8_t *)(uintptr_t)(edi + 0x21) != 0) {
        /* pain (0x43093): aim at a tight limit + gravity, burn the pain timer */
        aim_enemy_at_player(0x3e8, edi);
        tick_entity_vertical_only(edi);
        (*(volatile uint8_t *)(uintptr_t)(edi + 0x21))--;
        return;
    }
    ta_think(edi);
}

/* ---- tick_dynamic_entities (0x42d74): the per-frame entry from gameplay_frame_step ------------
 * regs_t in (ABI_FRAME): the ambient snapshot feeds tick_world_effects (AL = the once-per-frame
 * bit of 0x911c5) and the dying-trigger firer. Repeats the whole update+actor pass [0x85324]&0xff
 * times (8-bit `dec dl; jg` counter — faithful). Returns in->eax (EAX preserved by the original's
 * push/pop frame). LIVE-SWAP tier — the subsystem's DoD anchor. */
int32_t tick_dynamic_entities(const regs_t *in)
{
    {
        regs_t io = *in;
        io.eax = (in->eax & 0xffffff00u) | (uint32_t)(G8(VA_g_object_table_dirty) & 1);
        tick_world_effects(&io);                  /* 0x3464c */
    }
    G8(VA_g_object_table_dirty) &= 0xfe;
    if (G32(VA_g_frame_time_scale) == 0)                               /* frame time scale */
        return (int32_t)in->eax;
    int8_t dl = (int8_t)(uint8_t)(uint32_t)G32(VA_g_frame_time_scale);
    for (;;) {
        update_dynamic_entities();                /* 0x42872 */
        int32_t left = G32(VA_g_state_pool_a_count);
        if (left != 0) {
            uint32_t edi = (uint32_t)GADDR(VA_g_state_pool_a_records);
            for (;;) {
                if (*(volatile uint32_t *)(uintptr_t)(edi + 4) != 0) {
                    if (*(volatile uint32_t *)(uintptr_t)edi != 0)
                        ta_slot(edi, in);
                    left--;                              /* record-less slots still count */
                }
                edi += 0x22;
                if (left <= 0)
                    break;
            }
        }
        dl--;
        if (dl <= 0)
            break;
    }
    return (int32_t)in->eax;
}
