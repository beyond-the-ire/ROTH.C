/* lift_raw_commands.c — the RAW map command system (dispatch table 0x30780) lifted to verified C.
 * Split out of renderer.c (per docs/operating/recomp.md §4.6: every subsystem gets its own TU).
 * Handler ABI: ESI=EDI=command-record ptr -> EAX (-1 acted / 0 no-op); the record + the stored buffers
 * (g_map_objects_buffer 0x90aa4 / g_map_geometry_buffer 0x90aa8 / g_object_table_header 0x85c30) are
 * HOST pointers -> deref RAW; obj3 globals via G8/G16/G32. Shared leaves (resolve_command_by_index,
 * apply_damage_to_player, atan2_bearing, the collectors, the dbase100 bitmap ops) stay in renderer.c. */
#include "common.h"
#include "engine.h"
#include <string.h>
#include <stdio.h>


#include <stdlib.h>

/* [DIAG, in-game] The command-chain walk/resolve transcription is byte-verified
 * faithful vs the disasm (resolve_command_by_index 0x315a7, walk_command_chain_flow 0x353c4, and the
 * two firers dispatch_entry_command_trigger 0x34d75 / _b 0x34f5a — no dropped bounds check, mask, or
 * field; the original resolve has NO upper-bound gate either). So the rare in-game crash (a wild
 * `rec` like 0x4c4b4a4b read at rec+3 under ROTH_LIFT=input) is a GARBAGE chain index that resolves
 * out of g_object_ptr_array's bounds — a runtime/data condition, not a transcription error. This flag
 * (ROTH_WALK_DEBUG=1) captures the offending (ax, rec) and terminates the walk so a later session
 * can pin the source without a SIGSEGV. Default OFF = byte-faithful (the wild deref still faults,
 * exactly as the original would). Dormant in the oracle (never set). */
static int rcw_walk_debug(void)
{
    static int v = -1;
    if (v < 0) v = (getenv("ROTH_WALK_DEBUG") != NULL);
    return v;
}

static int32_t rawcmd_flush_pending_command_record(void);
static int32_t rawcmd_step_count_command(uint32_t rec, uint16_t seed);
static int32_t rawcmd_step_count_apply_primary(uint32_t rec, uint16_t seed);
static int32_t rawcmd_step_count_apply_secondary(uint32_t rec, uint16_t seed);
static int32_t rawcmd_step_count_apply_faces(uint32_t rec, uint16_t seed);
static int32_t rawcmd_exec_loop(uint32_t edi, uint16_t ax);
static int32_t rawcmd_tick_rerun(uint32_t rec, int32_t eax_in);

/* cmd_jump_if_next_fails (base 0x38, 0x30f55): g_command_next_active[0x8a0cc] = (u16)record[8]; return 0.
 * (`sub eax,eax; mov ax,[esi+8]; mov [0x8a0cc],eax; sub eax,eax; ret` — zero-extended u16 store.) */
int32_t cmd_jump_if_next_fails(uint32_t rec)
{
    G32(VA_g_item_drop_position + 0x120) = (uint32_t)*(volatile uint16_t *)(uintptr_t)(rec + 8);
    return 0;
}

/* find_geometry_record (0x4f2e0): AX=key -> EAX = buffer-relative offset of the first SECTOR record
 * (section u16[buf+4], stride 0x1a, match u16[rec+0x14]==key), or 0. g_map_geometry_buffer 0x90aa8 = host
 * ptr. Leaf, read-only. (Sibling of the lifted geom_find_matches 0x4f313 / find_raw_state_record 0x4f52b.) */
int32_t find_geometry_record(uint16_t key)
{
    uint8_t *buf = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    uint8_t *rec = buf + *(uint16_t *)(buf + 4);
    int32_t count = (int32_t)*(uint16_t *)(rec - 2);      /* mov cx,[recs-2]; dec ecx; jg (signed) */
    while (count > 0) {
        if (*(uint16_t *)(rec + 0x14) == key) return (int32_t)(uint32_t)(rec - buf);
        rec += 0x1a; count--;
    }
    return 0;
}

/* find_face_record (0x4f567): AX=key -> EAX = offset of the first FACE record (section u16[buf+6],
 * stride 0xc, match u16[rec+4]==key), or 0. Same buffer/shape as 0x4f2e0; FACE constants (0xc / +4). */
int32_t find_face_record(uint16_t key)
{
    uint8_t *buf = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    uint8_t *rec = buf + *(uint16_t *)(buf + 6);
    int32_t count = (int32_t)*(uint16_t *)(rec - 2);
    while (count > 0) {
        if (*(uint16_t *)(rec + 4) == key) return (int32_t)(uint32_t)(rec - buf);
        rec += 0xc; count--;
    }
    return 0;
}

/* test_dbase100_record_flag (0x1cb35): read-only test of g_dbase100_record_bitmap (0x81e28, host ptr; 0 =
 * none) bit (id>>3 arithmetic / mask g_bit_mask_lut[id&7]). Returns -1 if SET or no bitmap, 0 if present &
 * clear. Mirrors the already-lifted dbase100_bitmap_test_set/clear's bm/p/mask access. Leaf, no writes. */
int32_t test_dbase100_record_flag(uint32_t idx)
{
    uint32_t bm = (uint32_t)G32(VA_g_dbase100_record_bitmap);
    if (bm == 0) return -1;                                       /* no bitmap -> -1 */
    uint8_t *p = (uint8_t *)(uintptr_t)(((int32_t)idx >> 3) + bm); /* sar -> arithmetic */
    uint8_t mask = (uint8_t)G8(VA_g_bit_mask_lut + (idx & 7));
    return (*p & mask) ? -1 : 0;                                  /* set -> -1, clear -> 0 */
}

/* gather_faces_by_id (0x34c38): AX=id, EBX=max, EDX=out -> EAX=count; writes the caller's out descriptor.
 * AX!=0 -> tail-jmp to the already-lifted collect_raw_state_matches (RAW-STATE id scan). AX==0 -> build a
 * 1-or-2 entry list from g_active_object (0x8a0f8) + g_active_object_secondary (0x8a0fc) keyed off the
 * primary's +0xc field, offsets relative to g_map_geometry_buffer (0x90aa8). Host ptrs -> deref RAW. */
int32_t gather_faces_by_id(uint16_t id, uint32_t max, uint32_t out)
{
    if (id != 0)
        return collect_raw_state_matches(id, (uint16_t *)(uintptr_t)out, max);  /* 0x4f36d tail-jmp */
    uint32_t ao = (uint32_t)G32(VA_g_active_object);                              /* g_active_object */
    if (ao == 0) return 0;                                            /* no active object -> 0, out untouched */
    uint8_t *o = (uint8_t *)(uintptr_t)out;
    *(uint32_t *)o = 1;                                               /* mov dword[edx],1 (zeroes out+1..+3) */
    uint32_t edi = (uint32_t)G32(VA_g_map_geometry_buffer);                            /* geometry buffer base */
    uint16_t cx = *(volatile uint16_t *)(uintptr_t)(ao + 0xc);        /* primary key (+0xc) */
    *(uint16_t *)(o + 4) = (uint16_t)(ao - edi);                      /* out+4 = primary buf-relative offset */
    uint32_t sec = (uint32_t)G32(VA_g_active_object + 0x4);                            /* g_active_object_secondary */
    if (sec != 0) {
        uint16_t bxi = *(volatile uint16_t *)(uintptr_t)(sec + 8);    /* movzx ebx,word[sec+8] (high16=0) */
        if (bxi != 0xffff) {
            uint16_t field = *(volatile uint16_t *)(uintptr_t)(edi + bxi + 4);       /* mov bx,word[edi+ebx+4] */
            /* `mov bx` CLOBBERS ebx's low word -> ebx = field; the key check resolves through a SECOND hop. */
            if (cx == *(volatile uint16_t *)(uintptr_t)(edi + field + 0xc)) {        /* cmp cx,word[edi+ebx(=field)+0xc] */
                *(uint16_t *)(o + 6) = field;                         /* out+6 = bx (= field) */
                (*(uint8_t *)o)++;                                    /* inc byte[edx] -> count 2 */
            }
        }
    }
    return (int32_t)*(uint32_t *)o;                                   /* eax = dword[edx] */
}

/* point_segment_distance_sq (0x40805) — pure-math leaf (no globals/segments/stores). Squared perpendicular
 * distance from a 2D query point to a line segment, with an AABB radius reject. AX=radius(u16), EDX=endpoint
 * A, ECX=endpoint B, EBX=query point (each packed x|y<<16, components s16). Returns dist²+1 on a hit within
 * radius, else 0x7ffff. NOTE (verified from bytes, spec prose was wrong): the cross uses (P - B), not (P - A);
 * cross = (P.x-B.x)*segDY - (P.y-B.y)*segDX where segDX=B.x-A.x, segDY=B.y-A.y; dist² = cross²/|seg|². */
uint32_t point_segment_distance_sq(uint16_t radius, uint32_t a, uint32_t b, uint32_t point)
{
    int16_t ax_ = (int16_t)(uint16_t)a,     ay_ = (int16_t)(uint16_t)(a >> 16);
    int16_t bx_ = (int16_t)(uint16_t)b,     by_ = (int16_t)(uint16_t)(b >> 16);
    int16_t px_ = (int16_t)(uint16_t)point, py_ = (int16_t)(uint16_t)(point >> 16);
    int16_t nr = (int16_t)(uint16_t)(0u - (uint32_t)radius);   /* -radius (16-bit) */
    int16_t pr = (int16_t)radius;                              /* +radius */

    /* X-axis AABB reject: diffs (B.x-P.x), (A.x-P.x); reject if max <= -radius or min >= +radius (signed16). */
    { int16_t da = (int16_t)(bx_ - px_), db = (int16_t)(ax_ - px_);
      int16_t mx = (da > db) ? da : db, mn = (da > db) ? db : da;
      if (mx <= nr || mn >= pr) return 0x7ffff; }
    /* Y-axis AABB reject. */
    { int16_t da = (int16_t)(by_ - py_), db = (int16_t)(ay_ - py_);
      int16_t mx = (da > db) ? da : db, mn = (da > db) ? db : da;
      if (mx <= nr || mn >= pr) return 0x7ffff; }

    int32_t segDY = (int16_t)(by_ - ay_);                      /* sub ax; cwde */
    int32_t segDX = (int16_t)(bx_ - ax_);
    int32_t seg2  = segDY * segDY + segDX * segDX;             /* |seg|² */
    int32_t t1 = (int32_t)(int16_t)(py_ - by_) * segDX;        /* (P.y-B.y)*segDX */
    int32_t t2 = (int32_t)(int16_t)(px_ - bx_) * segDY;        /* (P.x-B.x)*segDY */
    int32_t cross = t2 - t1;                                   /* sub eax,edx */
    int32_t dist2 = (int32_t)(((int64_t)cross * cross) / seg2);/* imul eax; idiv ecx (signed 64/32) */
    uint32_t r2 = (uint32_t)radius * (uint32_t)radius;         /* movzx; imul -> radius² */
    if ((uint32_t)dist2 > r2) return 0x7ffff;                 /* cmp eax,edx; ja (unsigned) */
    return (uint32_t)(dist2 + 1);                             /* inc eax */
}

/* Count step-fns — dispatched register-context via g_anim_step_fn_table (0x71f30)[g_anim_step_mode 0x8a104];
 * AX = seed counter -> AX = next. Read g_count_step_delta 0x8a108 (modes 1/2/3); mode 1 also reads the limit
 * word[ESI+8]. The handler seeds with high-16 of EAX = 0 (sub eax,eax; mov ax,..); arithmetic is 32-bit but
 * mode-1's wrap test is a 16-bit signed `or ax,ax`. (0x318dd/0x318df/0x318ef/0x318f6.) */
uint32_t step_mode_inc(uint32_t eax)                            /* 0x318dd: inc eax */
{ return eax + 1; }

uint32_t step_mode_sub_clamp(uint32_t eax, uint32_t esi)        /* 0x318df */
{
    eax -= (uint32_t)G32(VA_g_anim_step_mode + 0x4);                                     /* sub eax,[0x8a108] (32-bit) */
    if ((int16_t)(uint16_t)eax < 0)                                    /* or ax,ax; jns (16-bit signed) */
        eax = (eax & 0xffff0000u) |
              (uint16_t)((uint16_t)eax + *(volatile uint16_t *)(uintptr_t)(esi + 8));  /* add ax,word[esi+8] */
    return eax;
}

uint32_t step_mode_add(uint32_t eax)                            /* 0x318ef: add eax,[0x8a108] */
{ return eax + (uint32_t)G32(VA_g_anim_step_mode + 0x4); }

uint32_t step_mode_xor(uint32_t eax)                            /* 0x318f6: xor eax,[0x8a108] */
{ return eax ^ (uint32_t)G32(VA_g_anim_step_mode + 0x4); }

/* flush_pending_command_record (0x31963) — the Count family's shared "not-done" finalize. If a pending
 * dialogue record is latched (g_pending_command_record 0x8a0dc != 0) and not suppressed (g_pending_suppress
 * 0x89f60 == 0), fire it via run_command_dbase100_record 0x3540b (BRIDGE: dialogue/voice) + clear it; always
 * clear g_anim_step_mode 0x8a104. Returns EAX = [0x8a0dc] (0 unless a record was latched+suppressed).
 * ORACLE path: stage 0x8a0dc == 0 so the bridge is never reached. */
static int32_t rawcmd_flush_pending_command_record(void)
{
    int32_t ret = (int32_t)G32(VA_g_pending_command_record);                /* mov eax,[0x8a0dc] */
    if (ret != 0 && G32(VA_g_item_autoselected_flag) == 0) {                /* latched && not suppressed -> fire */
        run_command_dbase100_record((uint32_t)ret);  /* 0x3540b re-pointed (esi=record; §6.5 in-game) */
        G32(VA_g_pending_command_record) = 0; ret = 0;                      /* sub eax,eax; mov [0x8a0dc],eax */
    }
    G32(VA_g_anim_step_mode) = 0;                                   /* clear step mode */
    return ret;
}

/* step_count_command (0x3192d) — the Count dispatcher body: AX=seed -> advance record[+0xa] via
 * g_anim_step_fn_table[g_anim_step_mode 0x8a104], compare to record[+0xc]; reached target -> DONE
 * (clear interrupt/mode/pending, EAX=-1) else -> the shared flush. The fall-through tail of every Count
 * handler + the per-frame tick re-entry + the re-step `call`. */
static int32_t rawcmd_step_count_command(uint32_t rec, uint16_t seed)
{
    uint16_t nv;
    switch ((uint32_t)G32(VA_g_anim_step_mode)) {                  /* mov ebx,[0x8a104]; call [ebx*4+0x71f30] */
        case 1:  nv = (uint16_t)step_mode_sub_clamp(seed, rec); break;
        case 2:  nv = (uint16_t)step_mode_add(seed);           break;
        case 3:  nv = (uint16_t)step_mode_xor(seed);           break;
        default: nv = (uint16_t)step_mode_inc(seed);           break;   /* mode 0 (or invariant) */
    }
    *(volatile uint16_t *)(uintptr_t)(rec + 0xa) = nv;                /* mov word[esi+0xa],ax */
    if (nv == *(volatile uint16_t *)(uintptr_t)(rec + 0xc)) {         /* cmp ax,[esi+0xc]; jne flush */
        G8(VA_g_command_chain_interrupt) = 0; G32(VA_g_anim_step_mode) = 0; G32(VA_g_pending_command_record) = 0;          /* DONE (0x31944) */
        return -1;
    }
    return rawcmd_flush_pending_command_record();                         /* not done (0x31963) */
}

/* cmd_count (base 0x15, 0x318fd) — the bare loop-counter Count command. Sets g_command_chain_interrupt
 * 0x8a268=1 (keep the chain re-pumping per frame), seeds from record[+0xa] (clamped/wrapped vs the limit
 * record[+8] + flag bit 0x20), then steps toward the compare target record[+0xc] (flag bit 0x08 = re-step
 * once at target then finish). Multi-entry block (handler / step body 0x3192d / flush 0x31963). obj3-pure
 * (with 0x8a0dc==0). DX-vs-AX clamp quirk: the "already at compare" test uses the UNCLAMPED current. */
int32_t cmd_count(uint32_t rec)
{
    G8(VA_g_command_chain_interrupt) = 1;                                                  /* 0x318fd interrupt=1 */
    uint16_t unclamped = *(volatile uint16_t *)(uintptr_t)(rec + 0xa);/* current count (DX, unclamped) */
    uint16_t seed = unclamped;                                       /* AX seed */
    if (unclamped >= *(volatile uint16_t *)(uintptr_t)(rec + 8)) {    /* cmp ax,[esi+8]; jb (UNSIGNED) */
        if (!(*(volatile uint8_t *)(uintptr_t)(rec + 6) & 0x20))      /* at/over limit, no wrap bit */
            return rawcmd_flush_pending_command_record();                /* -> flush, no step */
        seed = 0;                                                    /* wrap: seed = 0 */
    }
    if (unclamped == *(volatile uint16_t *)(uintptr_t)(rec + 0xc) &&  /* cmp dx,[esi+0xc]; jne normal-step */
        (*(volatile uint8_t *)(uintptr_t)(rec + 6) & 8)) {           /* test [esi+6],8; je normal-step */
        (void)rawcmd_step_count_command(rec, seed);                      /* 0x31926 re-step once... */
        G8(VA_g_command_chain_interrupt) = 0; G32(VA_g_anim_step_mode) = 0; G32(VA_g_pending_command_record) = 0;         /* ...then force DONE (0x31944) */
        return -1;
    }
    return rawcmd_step_count_command(rec, seed);                          /* normal step (fall to 0x3192d) */
}

/* cmd_rotate_object (base 0x24, 0x3146d): resolve target objects (resolve_command_objects), then rotate each
 * object's facing byte (g_map_objects_buffer[entry+6]). Modes (flags record[+6]): bit2 clear = STEP (advance
 * frame [+0xb] through [+0xa] steps, add the signed per-step delta (newFrame*256/n - oldFrame*256/n)); bit2
 * set = PLAYER-RELATIVE (compute_player_object_bearing per object: n<=1 SNAP to bearing, else step toward it
 * clamped to +-n). bit0 [+2] = early-out (ret 0). bit4 = re-arm ([+2]|=8). Returns -1 (acted); the step-mode
 * n<2 early-out returns (resolve_count & 0xffffff00) | n (only AL is overwritten — stale-count high bytes). */
int32_t cmd_rotate_object(uint32_t rec)
{
    if (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 1) return 0;            /* 0x3146f bit0 -> ret 0 */
    uint8_t list[0x190];
    uint16_t id = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    int32_t cnt = resolve_command_objects(id, 0xc8, (uint32_t)(uintptr_t)list);
    if (cnt == 0) return 0;                                                 /* 0x31491 */
    const uint16_t *ent = (const uint16_t *)list;
    int32_t ecx = cnt;
    uint8_t flags = *(volatile uint8_t *)(uintptr_t)(rec + 6);
    uint32_t objbuf = (uint32_t)G32(VA_g_map_objects_buffer);

    if (!(flags & 2)) {                                                     /* STEP MODE */
        uint8_t n = *(volatile uint8_t *)(uintptr_t)(rec + 0xa);
        if (n <= 1) return (int32_t)(((uint32_t)cnt & 0xffffff00u) | n);    /* 0x314a2 n<2 early-out */
        uint8_t dir, sentinel, wrap;
        if (flags & 1) { dir = 0xff; sentinel = 0xff; wrap = (uint8_t)(n - 1); }   /* reverse */
        else           { dir = 0x01; sentinel = n;    wrap = 0; }
        uint8_t oldframe = *(volatile uint8_t *)(uintptr_t)(rec + 0xb);
        uint8_t newframe = (uint8_t)(oldframe + dir);
        if (newframe == sentinel) newframe = wrap;
        *(volatile uint8_t *)(uintptr_t)(rec + 0xb) = newframe;
        uint8_t newQ = (uint8_t)(((uint16_t)newframe << 8) / n);           /* div dl (8-bit) */
        uint8_t oldQ = (uint8_t)(((uint16_t)oldframe << 8) / n);
        uint8_t delta = (uint8_t)(newQ - oldQ);
        for (int32_t k = 0; k < ecx; k++) {                                /* add facing += delta */
            uint8_t *fp = (uint8_t *)(uintptr_t)(objbuf + ent[k] + 6);
            *fp = (uint8_t)(*fp + delta);
        }
    } else {                                                               /* PLAYER-RELATIVE MODE */
        uint8_t n = *(volatile uint8_t *)(uintptr_t)(rec + 0xa);
        if (n <= 1) {                                                      /* 0x31545 SNAP */
            for (int32_t k = 0; k < ecx; k++) {
                uint32_t objptr = objbuf + ent[k];
                uint32_t bearing = compute_player_object_bearing(objptr);
                *(volatile uint8_t *)(uintptr_t)(objptr + 6) = (uint8_t)bearing;
            }
        } else {                                                           /* 0x3150f STEP-TOWARD */
            int32_t edx = (int32_t)n;
            for (int32_t k = 0; k < ecx; k++) {
                uint32_t objptr = objbuf + ent[k];
                uint32_t bearing = compute_player_object_bearing(objptr);
                int32_t v = (int32_t)(int8_t)((uint8_t)bearing - *(volatile uint8_t *)(uintptr_t)(objptr + 6));
                if (v >= edx) v = edx;                                     /* cmp;jl skip; else =edx */
                v = -v;                                                     /* neg eax */
                if (v >= edx) v = edx;
                uint8_t *fp = (uint8_t *)(uintptr_t)(objptr + 6);
                *fp = (uint8_t)(*fp - (uint8_t)v);                         /* sub [esi+6],al */
            }
        }
    }
    if (flags & 0x10) *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 8;       /* re-arm */
    return -1;
}

/* compute_player_object_bearing (0x30389): ESI=object pos ptr (word[0]=objX, word[2]=objY) -> EAX = the
 * 8-bit facing from the player to the object = atan2_bearing(player->object) >> 1 (arithmetic). Reads
 * g_player_x 0x90a8e / g_player_y 0x90a96; calls the lifted atan2_bearing (x1=px, y2=objY, y1=py, x2=objX,
 * since it does cx=x2-x1=objX-px, dx=y2-y1=objY-py). Pure leaf, no writes. */
uint32_t compute_player_object_bearing(uint32_t esi)
{
    int16_t objX = *(volatile int16_t *)(uintptr_t)(esi + 0);
    int16_t objY = *(volatile int16_t *)(uintptr_t)(esi + 2);
    int16_t px = (int16_t)G16(VA_g_player_x);
    int16_t py = (int16_t)G16(VA_g_player_y);
    uint32_t bearing = atan2_bearing(px, objY, py, objX);   /* CX=objX,DX=objY,AX=px,BX=py */
    return (uint32_t)((int32_t)bearing >> 1);                       /* sar eax,1 */
}

/* resolve_command_objects (0x34c19): "which objects does this command act on?" AX=id, EBX=cap, EDX=out ->
 * EAX=count, writes u16 object-buffer offsets to out. AX==0 -> the single source object (g_command_source_
 * object 0x8a100): out[0]=(u16)(src-g_map_objects_buffer 0x90aa4), ret 1 (0 if none; g_object_match_count
 * 0x8a110 untouched). AX!=0 -> walk the index array (count from g_map_geometry_buffer 0x90aa8 [+4] section,
 * array at objbuf+2), each nonzero index -> object group record (objbuf+idx): byte[0]=sub-count, sub-records
 * stride 0x10 from +2 with id at +0xe; append every match (offset = sub-objbuf), inc 0x8a110, cap at EBX.
 * Returns g_object_match_count. Host ptrs deref RAW. */
int32_t resolve_command_objects(uint16_t id, uint32_t cap, uint32_t out)
{
    uint32_t objbuf = (uint32_t)G32(VA_g_map_objects_buffer);
    if (id == 0) {                                                   /* 0x34c20 single path */
        uint32_t src = (uint32_t)G32(VA_g_command_source_object);
        if (src == 0) return 0;
        *(volatile uint16_t *)(uintptr_t)out = (uint16_t)(src - objbuf);
        return 1;
    }
    /* 0x34c97 multi path */
    G32(VA_g_anim_step_mode + 0xc) = 0;
    int32_t ebp = (int32_t)cap;                                     /* cap budget */
    uint8_t *edi = (uint8_t *)(uintptr_t)out;                       /* out cursor */
    uint8_t *geom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    uint8_t *idxsec = geom + *(uint16_t *)(geom + 4);               /* index section */
    uint32_t idxcnt = *(uint16_t *)(idxsec - 2);                    /* index count */
    uint8_t *p   = (uint8_t *)(uintptr_t)(objbuf + 2);              /* first index entry */
    uint8_t *end = p + idxcnt * 2;                                  /* end of index array */
    while (p < end) {                                               /* scan (0x34cc3) */
        uint16_t idx = *(uint16_t *)p; p += 2;
        if (idx == 0) continue;                                     /* skip empty */
        uint8_t *esi = (uint8_t *)(uintptr_t)(objbuf + idx);        /* object group record */
        int32_t subcnt = *(uint8_t *)esi;                          /* sub-record count (8-bit) */
        if (subcnt == 0) continue;
        esi += 2;                                                   /* first sub-record */
        do {                                                        /* sub (0x34ce4), stride 0x10 */
            if (*(volatile uint16_t *)(uintptr_t)(esi + 0xe) == id) {
                *(volatile uint16_t *)(uintptr_t)edi = (uint16_t)((uint32_t)(uintptr_t)esi - objbuf);
                G32(VA_g_anim_step_mode + 0xc) = (uint32_t)G32(VA_g_anim_step_mode + 0xc) + 1;
                edi += 2;
                if (--ebp <= 0) return (int32_t)G32(VA_g_anim_step_mode + 0xc);       /* cap reached (stop) */
            }
            esi += 0x10;
        } while (--subcnt > 0);                                     /* dec edx; jg */
    }
    return (int32_t)G32(VA_g_anim_step_mode + 0xc);                                   /* done: eax = g_object_match_count */
}

/* cmd_smash_face_texture (base 0x2e, 0x31676): gather faces matching id record[+8] (gather_faces_by_id),
 * then for each toggle the LOW BIT of its texture-slot word (at geomBase + face-offset + dirOff, dirOff =
 * table{2,6,4,2}[flags&3]): new = cur^1; if !(flags&0x20) force bit0=1; if flags&4 force bit0=0; only AL is
 * touched (AH = the slot's high byte preserved). EAX accumulates 0xffffffff on any write -> `shr eax,0x10`
 * yields 0xffff (changed) / 0 (none); the 0/no-match path sets g_command_chain_interrupt 0x8a268=2 + ret 0.
 * Writes g_map_geometry_buffer (0x90aa8, host ptr). gather is the only callee (native). */
int32_t cmd_smash_face_texture(uint32_t rec)
{
    uint8_t desc[0x190];
    uint16_t id = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    int32_t cnt = gather_faces_by_id(id, 0xc8, (uint32_t)(uintptr_t)desc);
    if (cnt == 0) { G8(VA_g_command_chain_interrupt) = 2; return 0; }                  /* 0x316f2 no-match */
    uint8_t fl = *(volatile uint8_t *)(uintptr_t)(rec + 6);
    static const uint8_t dirtab[4] = { 2, 6, 4, 2 };              /* 0x31672 island */
    uint32_t edi = (uint32_t)G32(VA_g_map_geometry_buffer) + dirtab[fl & 3];       /* geomBase + dirOff */
    int32_t ecx = (int32_t)(*(uint32_t *)desc & 0xffff);          /* descriptor count */
    const uint16_t *ent = (const uint16_t *)(desc + 4);          /* entries (buf-relative offsets) */
    uint32_t eax = 0;
    for (int32_t k = 0; k < ecx; k++) {
        uint16_t off = ent[k];
        uint16_t cur = *(volatile uint16_t *)(uintptr_t)(edi + off);
        eax = (eax & 0xffff0000u) | cur;                          /* mov ax,[edi+ebx] (high16 preserved) */
        uint16_t dx = cur;                                        /* edx=eax: saved current */
        uint8_t al = (uint8_t)((uint8_t)eax ^ 1);                 /* al ^= 1 */
        if (!(fl & 0x20)) al |= 1;                                /* force bit0=1 (unless toggle-only) */
        if (fl & 4)       al &= 0xfe;                             /* force bit0=0 (restore intact) */
        eax = (eax & 0xffffff00u) | al;
        uint16_t ax = (uint16_t)eax;
        if (ax != dx) { *(volatile uint16_t *)(uintptr_t)(edi + off) = ax; eax = 0xffffffffu; }
    }
    eax >>= 0x10;                                                 /* 0xffff (changed) or 0 */
    if (eax == 0) { G8(VA_g_command_chain_interrupt) = 2; return 0; }                  /* iterated, nothing changed */
    return (int32_t)eax;                                          /* 0xffff */
}

/* cmd_change_face_texture (base 0x34, 0x32738): gather faces matching id record[+8], then per face SET its
 * texture-slot u16[geomBase+off+dirOff] = Texture Index record[+0xa] (dirOff = table{2,6,4,2}[flags&3]) AND
 * flip the transparency bit0 of the PAIRED face record (find_face_record(off) -> geomBase+paired, byte+0xa):
 * tgt = cur^1; if !(flags&4){ tgt|=1; if(flags&8) tgt&=0xfe; } if ((tgt^cur)&1) flip. EAX accumulates
 * 0xffffffff on any change -> `shr eax,0x10` = 0xffff/0. count==0 -> ret 0 (NO 0x8a268 write, unlike 0x2e);
 * id==0 && nothing changed -> 0x8a268=2. Writes g_map_geometry_buffer (0x90aa8). */
int32_t cmd_change_face_texture(uint32_t rec)
{
    uint8_t desc[0x190];
    uint16_t id = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    int32_t cnt = gather_faces_by_id(id, 0xc8, (uint32_t)(uintptr_t)desc);
    if (cnt == 0) return 0;                                       /* 0x327de (no 0x8a268) */
    uint8_t fl = *(volatile uint8_t *)(uintptr_t)(rec + 6);
    static const uint8_t dirtab[4] = { 2, 6, 4, 2 };
    uint32_t base = (uint32_t)G32(VA_g_map_geometry_buffer);
    uint32_t edi = base + dirtab[fl & 3];                         /* slot field base */
    int32_t ecx = (int32_t)(*(uint32_t *)desc & 0xffff);
    const uint16_t *ent = (const uint16_t *)(desc + 4);
    uint16_t dx = *(volatile uint16_t *)(uintptr_t)(rec + 0xa);   /* Texture Index (loaded once) */
    uint32_t eax = 0;
    for (int32_t k = 0; k < ecx; k++) {
        uint16_t off = ent[k];
        uint16_t cur_slot = *(volatile uint16_t *)(uintptr_t)(edi + off);
        eax = (eax & 0xffff0000u) | cur_slot;                    /* mov ax,[edi+ebx] */
        if (cur_slot != dx) {                                    /* cmp ax,dx; jne */
            eax = 0xffffffffu;                                   /* or eax,-1 */
            *(volatile uint16_t *)(uintptr_t)(edi + off) = dx;   /* set slot = Texture Index */
        }
        int32_t paired = find_face_record(off);           /* eax preserved across (push/pop) */
        if (paired != 0) {
            uint32_t face = base + (uint32_t)paired;
            uint8_t cur = *(volatile uint8_t *)(uintptr_t)(face + 0xa);
            uint8_t al = (uint8_t)(cur ^ 1);
            if (!(fl & 4)) { al |= 1; if (fl & 8) al &= 0xfe; }
            if (((al ^ cur) & 1) != 0) {                         /* target bit0 differs from current */
                *(volatile uint8_t *)(uintptr_t)(face + 0xa) ^= 1;   /* flip */
                eax = 0xffffffffu;                               /* or eax,-1 */
            }
        }
    }
    if (*(volatile uint16_t *)(uintptr_t)(rec + 8) == 0) {       /* idZero path */
        eax >>= 0x10;
        if (eax == 0) G8(VA_g_command_chain_interrupt) = 2;
    } else {
        eax >>= 0x10;
    }
    return (int32_t)eax;
}

/* cmd_set_player_speed_reduction (base 0x41, 0x30ab3): BTI "Slow Player Speed". Stores record[7] into
 * g_player_speed_reduction_request (0x89f54) — a velocity right-SHIFT count (1 = halve, 2 = quarter, ...), NOT a
 * linear scale; tick_world_effects latches it to g_player_speed_reduction_shift (0x89f58) and player_movement_tick
 * does player_vel_x/y >>= (shift & 0x1f). If record[6]&0x10 then record[2] |= 8 (re-arm bit); return -1.
 * Handler (rec = esi = edi). */
int32_t cmd_set_player_speed_reduction(uint32_t rec)
{
    G32(VA_g_player_speed_reduction_request) = (uint32_t)*(volatile uint8_t *)(uintptr_t)(rec + 7);   /* g_player_speed_reduction_request: sub eax,eax; mov al,[esi+7]; mov [0x89f54],eax */
    if (*(volatile uint8_t *)(uintptr_t)(rec + 6) & 0x10)                 /* test byte[edi+6],0x10; je */
        *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x8;                 /* or byte[edi+2],8 */
    return -1;                                                            /* or eax,-1 */
}

/* mark_object_trigger_links (0x30bb7): EAX=target obj ptr, DX=id -> void. Walks g_object_table_header
 * (0x85c30, host ptr; first record word[hdr+4], count word[hdr+6], per-record STRIDE word[R]); for each
 * record whose byte[R+3] is a trigger type {0x1b->0x20, 0x30->0x08, 0x39->0x04} and word[R+8]==id, OR the
 * link bit into byte[objptr+9]. Sole write = [objptr+9]. Pure leaf (host-ptr derefs RAW). */
void mark_object_trigger_links(uint32_t objptr, uint16_t id)
{
    uint32_t hdr = (uint32_t)G32(VA_g_object_table_header);
    if (hdr == 0) return;
    uint8_t *h = (uint8_t *)(uintptr_t)hdr;
    int32_t ecx = (uint16_t)*(uint16_t *)(h + 6);
    if (ecx == 0) return;
    uint8_t *r = h + *(uint16_t *)(h + 4);
    volatile uint8_t *flag = (volatile uint8_t *)(uintptr_t)(objptr + 9);
    do {
        uint8_t type = *(uint8_t *)(r + 3);
        if (type == 0x1b)      { if (*(uint16_t *)(r + 8) == id) *flag |= 0x20; }
        else if (type == 0x30) { if (*(uint16_t *)(r + 8) == id) *flag |= 0x08; }
        else if (type == 0x39) { if (*(uint16_t *)(r + 8) == id) *flag |= 0x04; }
        r += *(uint16_t *)r;
    } while (--ecx > 0);
}

/* cmd_change_object_id (base 0x3a, 0x31000): reassign g_command_source_object's id word [obj+0xe] to the
 * record's new id word[ESI+6]; if that == 0xffff, mint a fresh id from g_next_object_id 0x8a0c0 (inc, force
 * byte1=0xc1, store the full 32-bit to 0x8a0c0 AND the 0x8a0c4 mirror). Always [0x8a0c4]=0 at entry. Clears
 * the source obj's link bits ([obj+9] &= 0xd3) then, if id!=0, rebuilds them via mark_object_trigger_links.
 * EAX = -1 (acted) / 0 (no source object). obj3 + the source-object host buffer. */
int32_t cmd_change_object_id(uint32_t rec)
{
    G32(VA_g_item_drop_position + 0x118) = 0;
    if (G32(VA_g_command_source_object) == 0) return 0;
    uint16_t dx = *(volatile uint16_t *)(uintptr_t)(rec + 6);
    if (dx == 0xffff) {
        uint32_t counter = ((uint32_t)G32(VA_g_item_drop_position + 0x114) + 1u);
        counter = (counter & 0xffff00ffu) | 0xc100u;
        G32(VA_g_item_drop_position + 0x114) = counter;
        G32(VA_g_item_drop_position + 0x118) = counter;
        dx = (uint16_t)counter;
    }
    uint32_t obj = (uint32_t)G32(VA_g_command_source_object);
    *(volatile uint8_t *)(uintptr_t)(obj + 9) &= 0xd3;
    *(volatile uint16_t *)(uintptr_t)(obj + 0xe) = dx;
    if (dx != 0) mark_object_trigger_links(obj, dx);
    return -1;
}

/* cmd_06_empty_noop (base 0x06, 0x30f51): `or eax,-1; ret` — inert reserved stub, returns -1 (acted). */
int32_t cmd_06_empty_noop(uint32_t rec)
{
    (void)rec;
    return -1;
}

/* cmd_empty_allow_sfx (base 0x3e, 0x30b23): `or eax,-1; ret` — inert; returns -1 so the post-chain SFX fires. */
int32_t cmd_empty_allow_sfx(uint32_t rec)
{
    (void)rec;
    return -1;
}

/* cmd_dbase100_if_next_fails (base 0x36, 0x31326): clear g_item_autoselected_flag (0x89f60)=0 and latch the
 * record ptr into g_pending_command_record (0x8a0dc)=ESI; return 0 (no-op return). No callees. */
int32_t cmd_dbase100_if_next_fails(uint32_t rec)
{
    G32(VA_g_item_autoselected_flag) = 0;
    G32(VA_g_pending_command_record) = (int32_t)rec;                                          /* mov [0x8a0dc],esi (the record ptr) */
    return 0;                                                            /* sub eax,eax */
}

/* cmd_map_transition (base 0x3b, 0x3104a): latch the pending level-change globals from the record — dest
 * dword[rec+0xa]->0x8547c, dword[rec+0xe]->0x85480, and the (u16)word[rec+8]->0x85484 (zero-extended), set
 * the pending-transition flag 0x7fea8=1; if modifier byte[rec+6]&0x10, set the record's skip bit [rec+2]|=8.
 * Return -1 (acted). No callees. */
int32_t cmd_map_transition(uint32_t rec)
{
    G32(VA_g_warp_dest_a) = *(volatile int32_t *)(uintptr_t)(rec + 0xa);
    G32(VA_g_warp_dest_b) = *(volatile int32_t *)(uintptr_t)(rec + 0xe);
    G32(VA_g_pending_game_action) = 1;
    G32(VA_g_map_first_load_flag) = (int32_t)*(volatile uint16_t *)(uintptr_t)(rec + 8); /* sub eax,eax; mov ax,[esi+8] (zero-ext) */
    if (*(volatile uint8_t *)(uintptr_t)(rec + 6) & 0x10)
        *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x8;
    return -1;                                                          /* or eax,-1 */
}

/* cmd_toggle_command (base 0x17, 0x31563): resolve the target command by index word[rec+8]; toggle its
 * modifier byte's bit (bit1 0x02 if the target's base byte[tgt+3] is 0x06/0x2c, else bit3 0x08). Then if
 * this command's arg flags byte[rec+6] has bit 0x06 set, FORCE the bit: set it, and if flags bit 0x02 is
 * also set, clear it instead. Finally if !(flags&0x20) set this record's skip bit [rec+2]|=8. EAX = -1
 * (acted) / 0 (no target resolved). Sole external writes = the TARGET record byte [tgt+2] + this [rec+2]. */
int32_t cmd_toggle_command(uint32_t rec)
{
    uint16_t idx = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    uint32_t tgt = resolve_command_by_index(idx);
    if (tgt == 0) return 0;                                             /* test eax,eax; je -> ret (eax=0) */
    uint8_t tbase = *(volatile uint8_t *)(uintptr_t)(tgt + 3);
    uint8_t bit = (tbase == 0x06 || tbase == 0x2c) ? 0x02 : 0x08;       /* dl = 2, or 8 */
    *(volatile uint8_t *)(uintptr_t)(tgt + 2) ^= bit;                   /* xor [eax+2],dl */
    uint8_t fl = *(volatile uint8_t *)(uintptr_t)(rec + 6);             /* dh = [esi+6] */
    if (fl & 0x06) {
        *(volatile uint8_t *)(uintptr_t)(tgt + 2) |= bit;              /* or [eax+2],dl */
        if (fl & 0x02)                                                  /* test dh,cl (cl=2) */
            *(volatile uint8_t *)(uintptr_t)(tgt + 2) &= (uint8_t)~bit;/* not edx; and [eax+2],dl */
    }
    if (!(*(volatile uint8_t *)(uintptr_t)(rec + 6) & 0x20))            /* test [esi+6],0x20; jne skip */
        *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x8;
    return -1;                                                          /* or eax,-1 */
}

/* cmd_player_rotation (base 0x3f, 0x30acb): rotate the player view angle (0x90a8a) by -2*byte[rec+7].
 * If flags(byte[rec+6])&0x2: advance the RNG (g_random_seed 0x71f48 = seed*0x5e5+0x29) and scale the delta
 * by (seed&0xffff)>>16 (randomize), then set the record skip bit. If flags&0x1: frame-scale the delta by
 * g_frame_time_scale (0x85324) and 16-bit-add it to the angle (else the (truncated) delta IS the angle).
 * EAX = -1 (acted). No callees; obj3-only. */
int32_t cmd_player_rotation(uint32_t rec)
{
    uint32_t v = *(volatile uint8_t *)(uintptr_t)(rec + 7);             /* sub eax,eax; mov al,[esi+7] */
    uint32_t eax = 0u - v; eax += eax;                                  /* neg eax; add eax,eax  => -2*v */
    uint8_t flags = *(volatile uint8_t *)(uintptr_t)(rec + 6);          /* dl = [esi+6] */
    if (flags & 0x2) {                                                  /* randomize via the RNG */
        uint32_t seed = (uint32_t)G32(VA_g_command_rng);
        seed = seed * 0x5e5u + 0x29u;
        G32(VA_g_command_rng) = (int32_t)seed;
        eax = ((uint32_t)eax * (seed & 0xffffu)) >> 16;                 /* imul eax,ecx; shr eax,0x10 */
    }
    uint16_t ax = (uint16_t)eax;
    if (flags & 0x1) {                                                  /* frame-scale + add to angle */
        eax = (uint32_t)eax * (uint32_t)G32(VA_g_frame_time_scale);                  /* imul eax,[0x85324] */
        ax = (uint16_t)((uint16_t)eax + G16(VA_g_player_angle));                  /* add ax,[0x90a8a] (16-bit) */
    }
    G16(VA_g_player_angle) = ax;                                                  /* mov [0x90a8a],ax */
    if (flags & 0x2)
        *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x8;
    return -1;                                                          /* or eax,-1 */
}

/* cmd_set_flag (base 0x26, 0x35617): set/clear/toggle a DBASE100 progress flag (bitmap at *0x81e28) for the
 * id word[rec+8]. flags(byte[rec+6])&0x2 = TOGGLE (test current: if clear -> set, else clear); else flags&0x1
 * = CLEAR; else SET. On a real change (the bitmap op returns nonzero) it falls into the shared continue-tail
 * 0x355d1: if flags&0x10 set the record skip bit [rec+2]|=8, clear g_pending_command_record 0x8a0dc, return
 * -1. If the op reports no change, return 0 (no tail). Callees all lifted (test/test_set/test_clear). */
int32_t cmd_set_flag(uint32_t rec)
{
    uint16_t id = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    uint8_t flags = *(volatile uint8_t *)(uintptr_t)(rec + 6);
    int do_set;
    if (flags & 0x2)                                                    /* test path: clear -> set, set -> clear */
        do_set = (test_dbase100_record_flag(id) == 0);
    else if (flags & 0x1)
        do_set = 0;
    else
        do_set = 1;
    uint32_t r = do_set ? dbase100_bitmap_test_set(id)
                        : dbase100_bitmap_test_clear(id);
    if (r != 0) {                                                      /* jne 0x355d1 — the shared tail */
        if (flags & 0x10)
            *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x8;
        G32(VA_g_pending_command_record) = 0;
        return -1;
    }
    return 0;                                                          /* op reported no change -> plain ret */
}

/* cmd_spawn_object (base 0x16, 0x313b4): resolve the object set for id word[rec+8] (resolve_command_objects,
 * cap 0xc8 into a local list of 16-bit objects-buffer offsets); if no source object yet, adopt the first.
 * For each resolved object (obj = g_map_objects_buffer 0x90aa4 + offset): if byte[obj+9] bit0 is set AND it
 * should spawn (not the flags&2 re-toggle case, and not already hidden) -> WAKE its actor via the def table
 * at 0x91e03+word[obj+0xc] (if !(actor[8]&0x20) set actor[9]|=1) and skip the hide-toggle; otherwise toggle
 * the hidden bit byte[obj+7]^0x80 (force set/clear by flags&6 / flags&4). A def id of 0 falls into the
 * toggle. Finally if !(flags&0x20) set the record skip bit [rec+2]|=8. EAX = -1 (acted) / 0 (none resolved).
 * Writes the objects buffer (host) + def table (obj3) + g_command_source_object 0x8a100. */
int32_t cmd_spawn_object(uint32_t rec)
{
    uint16_t list[0xc8];                                                /* the 0x190-byte local out buffer */
    uint16_t id = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    int32_t count = resolve_command_objects(id, 0xc8, (uint32_t)(uintptr_t)list);
    if (count == 0) return 0;                                           /* je 0x31466 -> ret (eax=0) */
    uint8_t flags = *(volatile uint8_t *)(uintptr_t)(rec + 6);          /* dl = [esi+6] */
    uint32_t objbuf = (uint32_t)G32(VA_g_map_objects_buffer);                           /* g_map_objects_buffer (host ptr) */
    if (G32(VA_g_command_source_object) == 0)                                              /* no source object -> adopt the first */
        G32(VA_g_command_source_object) = (int32_t)(objbuf + list[0]);
    int i = 0;
    int32_t ecx = count;
    do {
        uint8_t *o = (uint8_t *)(uintptr_t)(objbuf + list[i]);          /* obj = objbuf + 16-bit offset */
        i++;
        if (*(volatile uint8_t *)(o + 9) & 1) {                        /* test [obj+9],1 — bit0 set */
            int to_toggle;
            if (flags & 0x6) {                                          /* test dl,6 */
                if (flags & 0x2) to_toggle = 1;                        /* test dl,2; jne 0x31440 */
                else to_toggle = (*(volatile uint8_t *)(o + 7) & 0x80) ? 1 : 0;  /* hidden? -> 0x31440 */
            } else {                                                    /* 0x3141b */
                to_toggle = (*(volatile uint8_t *)(o + 7) & 0x80) ? 1 : 0;
            }
            if (!to_toggle) {                                          /* 0x31421: wake the actor */
                uint16_t defid = *(volatile uint16_t *)(o + 0xc);
                if (defid != 0) {
                    volatile uint8_t *actor = (volatile uint8_t *)(GADDR(VA_g_state_pool_a_count + 0x3) + defid);
                    if (!(actor[8] & 0x20))
                        actor[9] |= 1;
                    goto next;                                         /* jmp 0x31456 (skip the toggle) */
                }
                /* defid == 0: fall into the toggle (0x3143f -> 0x31440) */
            }
        }
        *(volatile uint8_t *)(o + 7) ^= 0x80;                          /* 0x31440: toggle hidden bit */
        if (flags & 0x6) {
            *(volatile uint8_t *)(o + 7) |= 0x80;                      /* force set */
            if (!(flags & 0x4))
                *(volatile uint8_t *)(o + 7) &= 0x7f;                  /* ...then clear unless flags&4 */
        }
      next: ;
    } while (--ecx > 0);                                               /* dec ecx; jg 0x313f5 */
    if (!(*(volatile uint8_t *)(uintptr_t)(rec + 6) & 0x20))
        *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x8;
    return -1;
}

/* step_count_apply_to_primary_cells (0x319c0) — the Count worker for cmd_texture_change_count: step the
 * count (record[+0xa]) via the g_anim_step_fn_table[mode 0x8a104] (same dispatch as step_count_command),
 * then repaint every matching RAW-STATE geometry cell. Matches: collect_raw_state_matches(key word[rec+0xe])
 * into a local list of geometry-buffer offsets; for each, write (newcount-1 + word[rec+0x10]) into the cell's
 * slot word at geom + offset + dirOff, where dirOff = (flags&1)?6 : (flags&2)?4 : 2. Then compare newcount to
 * record[+0xc]: reached -> DONE (clear interrupt/mode/pending, -1); else -> the shared flush. Writes
 * g_map_geometry_buffer (0x90aa8, host). The fall-through worker of the 0x1f handler + its re-step `call`. */
static int32_t rawcmd_step_count_apply_primary(uint32_t rec, uint16_t seed)
{
    uint16_t nv;
    switch ((uint32_t)G32(VA_g_anim_step_mode)) {                                /* mov ebx,[0x8a104]; call [ebx*4+0x71f30] */
        case 1:  nv = (uint16_t)step_mode_sub_clamp(seed, rec); break;
        case 2:  nv = (uint16_t)step_mode_add(seed);           break;
        case 3:  nv = (uint16_t)step_mode_xor(seed);           break;
        default: nv = (uint16_t)step_mode_inc(seed);           break;
    }
    *(volatile uint16_t *)(uintptr_t)(rec + 0xa) = nv;               /* mov word[esi+0xa],ax */
    uint16_t outbuf[0xc8];                                           /* the 0x190-byte local list */
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 0xe);
    uint32_t count = (uint32_t)collect_raw_state_matches(key, outbuf, 0xc8) & 0xffffu;
    if (count != 0) {                                               /* je 0x31a2e skips the paint */
        uint16_t dx = (uint16_t)(nv - 1 + *(volatile uint16_t *)(uintptr_t)(rec + 0x10));
        uint8_t flags = *(volatile uint8_t *)(uintptr_t)(rec + 6);
        uint32_t dirOff = (flags & 1) ? 6u : (flags & 2) ? 4u : 2u;
        uint32_t geom = (uint32_t)G32(VA_g_map_geometry_buffer);                      /* g_map_geometry_buffer (host ptr) */
        for (uint32_t k = 0; k < count; k++) {                       /* out[2..] = match offsets */
            uint16_t off = outbuf[2 + k];
            *(volatile uint16_t *)(uintptr_t)(geom + off + dirOff) = dx;
        }
    }
    if (nv == *(volatile uint16_t *)(uintptr_t)(rec + 0xc)) {        /* reached target -> DONE (0x31a3e) */
        G8(VA_g_command_chain_interrupt) = 0; G32(VA_g_anim_step_mode) = 0; G32(VA_g_pending_command_record) = 0;
        return -1;
    }
    return rawcmd_flush_pending_command_record();                        /* 0x31a5d -> 0x31963 */
}

/* cmd_texture_change_count (base 0x1f, 0x3198e): a Count command that, on each step, repaints a group of
 * geometry cells' texture slots. Structurally identical to cmd_count (interrupt=1, seed/clamp/wrap vs limit
 * record[+8]+flag 0x20, re-step-at-target via flag 0x08) but its step worker is the geometry-painting
 * step_count_apply_to_primary_cells instead of the bare step_count_command. obj3 + geometry buffer. */
int32_t cmd_texture_change_count(uint32_t rec)
{
    G8(VA_g_command_chain_interrupt) = 1;                                                  /* 0x3198e interrupt=1 */
    uint16_t unclamped = *(volatile uint16_t *)(uintptr_t)(rec + 0xa);
    uint16_t seed = unclamped;
    if (unclamped >= *(volatile uint16_t *)(uintptr_t)(rec + 8)) {    /* cmp ax,[esi+8]; jb (unsigned) */
        if (!(*(volatile uint8_t *)(uintptr_t)(rec + 6) & 0x20))      /* at/over limit, no wrap */
            return rawcmd_flush_pending_command_record();                /* 0x31a5d -> flush */
        seed = 0;                                                    /* wrap */
    }
    if (unclamped == *(volatile uint16_t *)(uintptr_t)(rec + 0xc) &&  /* at target + re-step bit */
        (*(volatile uint8_t *)(uintptr_t)(rec + 6) & 8)) {
        (void)rawcmd_step_count_apply_primary(rec, seed);                /* re-step once... */
        G8(VA_g_command_chain_interrupt) = 0; G32(VA_g_anim_step_mode) = 0; G32(VA_g_pending_command_record) = 0;         /* ...then DONE (0x31a3e) */
        return -1;
    }
    return rawcmd_step_count_apply_primary(rec, seed);                   /* normal step + paint */
}

/* collect_secondary_state_records_by_key (0x34c97) — the multi-record collector (same body as
 * resolve_command_objects' multi path, but a standalone pusha/popa entry). Scans the object index array
 * (g_map_objects_buffer 0x90aa4 + 2, count = word[(g_map_geometry_buffer 0x90aa8 + word[geom+4]) - 2]); for
 * each non-zero index, walks that group's sub-records (count = byte[group], stride 0x10 from group+2) and for
 * each whose word[sub+0xe] == key, writes the sub-record's objects-buffer offset to out (no header) and bumps
 * g_object_match_count 0x8a110, stopping at cap. Returns the match count (EAX). Leaf, obj3 + objects buffer. */
int32_t collect_secondary_state_records_by_key(uint16_t key, uint32_t cap, uint32_t out)
{
    G32(VA_g_anim_step_mode + 0xc) = 0;
    int32_t ebp = (int32_t)cap;
    uint8_t *edi = (uint8_t *)(uintptr_t)out;
    uint8_t *geom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    uint32_t objbuf = (uint32_t)G32(VA_g_map_objects_buffer);
    uint8_t *idxsec = geom + *(uint16_t *)(geom + 4);
    uint32_t idxcnt = *(uint16_t *)(idxsec - 2);
    uint8_t *p   = (uint8_t *)(uintptr_t)(objbuf + 2);
    uint8_t *end = p + idxcnt * 2;
    while (p < end) {
        uint16_t idx = *(uint16_t *)p; p += 2;
        if (idx == 0) continue;
        uint8_t *esi = (uint8_t *)(uintptr_t)(objbuf + idx);
        int32_t subcnt = *(uint8_t *)esi;
        if (subcnt == 0) continue;
        esi += 2;
        do {
            if (*(volatile uint16_t *)(uintptr_t)(esi + 0xe) == key) {
                *(volatile uint16_t *)(uintptr_t)edi = (uint16_t)((uint32_t)(uintptr_t)esi - objbuf);
                G32(VA_g_anim_step_mode + 0xc) = (uint32_t)G32(VA_g_anim_step_mode + 0xc) + 1;
                edi += 2;
                if (--ebp <= 0) return (int32_t)G32(VA_g_anim_step_mode + 0xc);
            }
            esi += 0x10;
        } while (--subcnt > 0);
    }
    return (int32_t)G32(VA_g_anim_step_mode + 0xc);
}

/* step_count_apply_to_secondary_records (0x31a94) — the Count worker for cmd_count_addl_arg: step the count,
 * then for every secondary record matching key word[rec+0xe] (collect_secondary_state_records_by_key) write
 * (newcount-1 + word[rec+0x10]) into the OBJECTS-buffer record at objbuf + offset + 4 (fixed slot, no dirOff;
 * out entries have no header). Then compare to target -> DONE/flush. Writes g_map_objects_buffer (0x90aa4). */
static int32_t rawcmd_step_count_apply_secondary(uint32_t rec, uint16_t seed)
{
    uint16_t nv;
    switch ((uint32_t)G32(VA_g_anim_step_mode)) {
        case 1:  nv = (uint16_t)step_mode_sub_clamp(seed, rec); break;
        case 2:  nv = (uint16_t)step_mode_add(seed);           break;
        case 3:  nv = (uint16_t)step_mode_xor(seed);           break;
        default: nv = (uint16_t)step_mode_inc(seed);           break;
    }
    *(volatile uint16_t *)(uintptr_t)(rec + 0xa) = nv;
    uint16_t outbuf[0xc8];
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 0xe);
    uint32_t count = (uint32_t)collect_secondary_state_records_by_key(key, 0xc8,
                         (uint32_t)(uintptr_t)outbuf) & 0xffffu;
    if (count != 0) {
        uint16_t dx = (uint16_t)(nv - 1 + *(volatile uint16_t *)(uintptr_t)(rec + 0x10));
        uint32_t objbuf = (uint32_t)G32(VA_g_map_objects_buffer);
        for (uint32_t k = 0; k < count; k++)                            /* out[0..] = offsets (no header) */
            *(volatile uint16_t *)(uintptr_t)(objbuf + outbuf[k] + 4) = dx;
    }
    if (nv == *(volatile uint16_t *)(uintptr_t)(rec + 0xc)) {
        G8(VA_g_command_chain_interrupt) = 0; G32(VA_g_anim_step_mode) = 0; G32(VA_g_pending_command_record) = 0;
        return -1;
    }
    return rawcmd_flush_pending_command_record();
}

/* cmd_count_addl_arg (base 0x22, 0x31a62): the Count command that writes each step's value into a group of
 * secondary OBJECT records' slot. Identical cmd_count wrapper as 0x15/0x1f; worker =
 * step_count_apply_to_secondary_records. obj3 + objects buffer. */
int32_t cmd_count_addl_arg(uint32_t rec)
{
    G8(VA_g_command_chain_interrupt) = 1;
    uint16_t unclamped = *(volatile uint16_t *)(uintptr_t)(rec + 0xa);
    uint16_t seed = unclamped;
    if (unclamped >= *(volatile uint16_t *)(uintptr_t)(rec + 8)) {
        if (!(*(volatile uint8_t *)(uintptr_t)(rec + 6) & 0x20))
            return rawcmd_flush_pending_command_record();
        seed = 0;
    }
    if (unclamped == *(volatile uint16_t *)(uintptr_t)(rec + 0xc) &&
        (*(volatile uint8_t *)(uintptr_t)(rec + 6) & 8)) {
        (void)rawcmd_step_count_apply_secondary(rec, seed);
        G8(VA_g_command_chain_interrupt) = 0; G32(VA_g_anim_step_mode) = 0; G32(VA_g_pending_command_record) = 0;
        return -1;
    }
    return rawcmd_step_count_apply_secondary(rec, seed);
}

/* clear_geom_visited_bits (0x4f477): walk the SECTOR section (g_map_geometry_buffer 0x90aa8 + word[geom+4],
 * count = word[section-2], stride 0x1a) and clear bit 0x4 of byte[rec+0x16] on each. Flat (no selector).
 * NB: the original enters the loop body unconditionally (clears at least one record even if count==0). */
static void rawcmd_clear_geom_visited_bits(void)
{
    uint8_t *geom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    uint8_t *rec = geom + *(uint16_t *)(geom + 4);
    int32_t count = (uint16_t)*(uint16_t *)(rec - 2);
    do {
        *(volatile uint8_t *)(rec + 0x16) &= 0xfb;
        rec += 0x1a;
    } while (--count > 0);
}

/* walk_connected_geom (0x4f42d): the recursive sector flood-fill. `es` = the geometry buffer base (the
 * 0x90be8 selector aliases g_map_geometry_buffer 0x90aa8). Mark byte es[rec+0x16] |= 0x4, and (if budget
 * left) emit the record offset + recurse into each connected neighbour. Per edge in the record's edge list
 * (base = es[rec+0xe], count = es[rec+0xd] as int8, stride 0xc): skip if es[edge+0xa]&0x8; neighbour =
 * es[edge+8] (skip 0xffff); connected = es[neighbour+6]; recurse unless already visited. Budget/cursor are
 * shared (by-ref) across the recursion; the original preserves rec/edge/count but not budget/cursor. */
static void rawcmd_walk_connected_geom(uint8_t *es, uint16_t rec, int32_t *budget, uint8_t **cursor)
{
    *(volatile uint8_t *)(es + rec + 0x16) |= 0x4;                   /* or es:[ebx+0x16],4 */
    if (*budget <= 0) return;                                        /* or ecx,ecx; jle */
    (*budget)--;
    *(volatile uint16_t *)(*cursor) = rec;                           /* mov [edx],bx */
    *cursor += 2;
    uint16_t edge = *(volatile uint16_t *)(es + rec + 0xe);          /* si = es:[ebx+0xe] */
    int8_t cnt = (int8_t)*(volatile uint8_t *)(es + rec + 0xd);      /* al = es:[ebx+0xd] */
    do {                                                             /* 0x4f449 loop (dec al; jg) */
        if (!(*(volatile uint8_t *)(es + edge + 0xa) & 0x8)) {       /* test es:[esi+0xa],8 */
            uint16_t neighbour = *(volatile uint16_t *)(es + edge + 8);
            if (neighbour != 0xffff) {
                uint16_t connected = *(volatile uint16_t *)(es + neighbour + 6);
                if (!(*(volatile uint8_t *)(es + connected + 0x16) & 0x4))
                    rawcmd_walk_connected_geom(es, connected, budget, cursor);
            }
        }
        edge += 0xc;
    } while (--cnt > 0);
}

/* collect_connected_geometry_group (0x4f3d0, entry A): find the SECTOR record for `key`
 * (find_geometry_record), then flood-fill the connected-sector graph into `out`. out[1] (word at +2) = key;
 * out[2..] = the visited record offsets; out[0] = the visited count (also the EAX return). Bails to 0 if the
 * record isn't found, cap < 3, or the geometry selector 0x90be8 is null. Clears the visited bits first, then
 * walks with budget = cap-2. ES-selector aliases g_map_geometry_buffer 0x90aa8. */
int32_t collect_connected_geometry_group(uint16_t key, uint32_t cap, uint32_t out)
{
    *(volatile uint16_t *)(uintptr_t)(out + 2) = key;               /* mov [edx+2],ax (out[1] = key) */
    int32_t found = find_geometry_record(key);               /* call 0x4f2e0 */
    if (found == 0) return 0;                                       /* or eax,eax; je */
    if ((int32_t)cap < 3) return 0;                                /* cmp ecx,3; jb */
    if (G16(VA_g_geometry_selector) == 0) return 0;                               /* cmp word[0x90be8],0; je */
    rawcmd_clear_geom_visited_bits();                              /* call 0x4f477 */
    int32_t budget_start = (int32_t)cap - 2;                       /* sub ecx,2 */
    int32_t budget = budget_start;
    uint8_t *cursor = (uint8_t *)(uintptr_t)(out + 4);            /* edx += 4 (first offset slot) */
    uint8_t *es = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);   /* ES base = g_map_geometry_buffer */
    rawcmd_walk_connected_geom(es, (uint16_t)found, &budget, &cursor);  /* call 0x4f42d */
    int32_t count = budget_start - budget;                        /* sub eax,ecx */
    *(volatile uint16_t *)(uintptr_t)out = (uint16_t)count;        /* mov [edx],ax (out[0] = count) */
    return count;
}

/* flood_fill_geometry_neighbors (0x4f42d): the recursive connected-sector flood-fill worker behind
 * collect_connected_geometry_group (0x4f3d0). Mark es:[rec+0x16] visited, append the sector offset to the cursor
 * list, then for each edge recurse into the connected, unvisited neighbour while the budget holds. ES selector base
 * = es; the budget (ECX) and cursor (EDX) thread through the recursion by reference. EAX is a passthrough. */
void flood_fill_geometry_neighbors(uint32_t es_base, uint16_t rec, int32_t *budget, uint8_t **cursor)
{
    rawcmd_walk_connected_geom((uint8_t *)(uintptr_t)es_base, rec, budget, cursor);
}

/* step_count_apply_to_geometry_faces (0x31b4f) — the Count worker for cmd_animate_facegroup_texture: step
 * the count, then collect the face group — via collect_connected_geometry_group (flood) if record bit
 * byte[rec+2]&0x4 is set, else geom_find_matches — and paint (newcount-1 + word[rec+0x10]) into each match's
 * geometry slot. Slot select: when BOTH flags&2 and flags&1 (== flags&3==3) the write is INDIRECT through
 * word[geom+off+0x18] (skip if 0) at +6; otherwise DIRECT at geom+off + ((flags&1)?6:8). Then compare to
 * target -> DONE/flush. Writes g_map_geometry_buffer (0x90aa8). out header = {count,key} like the others. */
static int32_t rawcmd_step_count_apply_faces(uint32_t rec, uint16_t seed)
{
    uint16_t nv;
    switch ((uint32_t)G32(VA_g_anim_step_mode)) {
        case 1:  nv = (uint16_t)step_mode_sub_clamp(seed, rec); break;
        case 2:  nv = (uint16_t)step_mode_add(seed);           break;
        case 3:  nv = (uint16_t)step_mode_xor(seed);           break;
        default: nv = (uint16_t)step_mode_inc(seed);           break;
    }
    *(volatile uint16_t *)(uintptr_t)(rec + 0xa) = nv;
    uint16_t outbuf[0xc8];
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 0xe);
    uint32_t count;
    if (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 0x4)             /* record bit 0x4 -> connected flood */
        count = (uint32_t)collect_connected_geometry_group(key, 0xc8,
                    (uint32_t)(uintptr_t)outbuf) & 0xffffu;
    else                                                            /* else -> flat geom_find_matches */
        count = (uint32_t)geom_find_matches(key, 0xc8, (uint8_t *)outbuf) & 0xffffu;
    if (count != 0) {
        uint16_t dx = (uint16_t)(nv - 1 + *(volatile uint16_t *)(uintptr_t)(rec + 0x10));
        uint8_t flags = *(volatile uint8_t *)(uintptr_t)(rec + 6);
        uint32_t slot = (flags & 1) ? 6u : 8u;
        int indirect = ((flags & 3) == 3);                          /* flags&2 && flags&1 */
        uint8_t *geom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
        for (uint32_t k = 0; k < count; k++) {
            uint16_t off = outbuf[2 + k];
            if (indirect) {
                uint16_t ind = *(volatile uint16_t *)(uintptr_t)(geom + off + 0x18);
                if (ind != 0)
                    *(volatile uint16_t *)(uintptr_t)(geom + ind + slot) = dx;
            } else {
                *(volatile uint16_t *)(uintptr_t)(geom + off + slot) = dx;
            }
        }
    }
    if (nv == *(volatile uint16_t *)(uintptr_t)(rec + 0xc)) {
        G8(VA_g_command_chain_interrupt) = 0; G32(VA_g_anim_step_mode) = 0; G32(VA_g_pending_command_record) = 0;
        return -1;
    }
    return rawcmd_flush_pending_command_record();
}

/* cmd_animate_facegroup_texture (base 0x21, 0x31b1a): the Count command that animates a connected face
 * group's textures. LATENT (no shipped map triggers it) but fully implemented. Identical cmd_count wrapper;
 * worker = step_count_apply_to_geometry_faces. obj3 + geometry buffer (and the ES selector when record bit
 * 0x4 routes through the connected-flood collector). */
int32_t cmd_animate_facegroup_texture(uint32_t rec)
{
    G8(VA_g_command_chain_interrupt) = 1;
    uint16_t unclamped = *(volatile uint16_t *)(uintptr_t)(rec + 0xa);
    uint16_t seed = unclamped;
    if (unclamped >= *(volatile uint16_t *)(uintptr_t)(rec + 8)) {
        if (!(*(volatile uint8_t *)(uintptr_t)(rec + 6) & 0x20))
            return rawcmd_flush_pending_command_record();
        seed = 0;
    }
    if (unclamped == *(volatile uint16_t *)(uintptr_t)(rec + 0xc) &&
        (*(volatile uint8_t *)(uintptr_t)(rec + 6) & 8)) {
        (void)rawcmd_step_count_apply_faces(rec, seed);
        G8(VA_g_command_chain_interrupt) = 0; G32(VA_g_anim_step_mode) = 0; G32(VA_g_pending_command_record) = 0;
        return -1;
    }
    return rawcmd_step_count_apply_faces(rec, seed);
}

/* cmd_cycle_texture (base 0x1c, 0x3179c): step an animated sector/face texture directly (no Count effect).
 * Find a reference record (find_geometry_record(word[rec+0xa]) unless that == 0 or == the key word[rec+8]);
 * key 0 -> ret 0. Collect the group for the key (collect_connected flood if byte[rec+2]&4 else
 * geom_find_matches). If no reference resolved, use the first match. Read the reference's CURRENT texture-slot
 * value, +1, and write that to every match's slot whose value differs (tracking whether anything changed).
 * Slot select by flags&3: <2 = variant A (DIRECT slot geom[cell + ((flags&1)?6:8)]); >=2 = variant B
 * (INDIRECT through word[cell+0x18] -> geom[ind + ((flags&1)?0:6)], skipping cells with a 0 indirect; the
 * source value is taken once from the reference's indirect cell, or lazily from the first match's). Returns
 * -1 if anything changed (and sets the record skip bit when flags&0x10), else 0. Writes the geometry buffer. */
int32_t cmd_cycle_texture(uint32_t rec)
{
    uint16_t outbuf[0xc8];
    uint16_t av = *(volatile uint16_t *)(uintptr_t)(rec + 0xa);
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    uint16_t ref;
    if (av == 0 || av == key)
        ref = 0;                                                     /* 0x317bf: no reference record */
    else
        ref = (uint16_t)find_geometry_record(av);             /* call 0x4f2e0; and eax,0xffff */
    if (key == 0) return 0;                                          /* 0x31669 cleanup -> ret 0 */
    uint32_t count;
    if (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 0x4)
        count = (uint32_t)collect_connected_geometry_group(key, 0xc8,
                    (uint32_t)(uintptr_t)outbuf) & 0xffffu;
    else
        count = (uint32_t)geom_find_matches(key, 0xc8, (uint8_t *)outbuf) & 0xffffu;
    if (count == 0) return 0;                                       /* 0x3185a -> ret 0 */
    if (ref == 0) ref = outbuf[2];                                  /* 0x317f3: fall back to first match */
    uint8_t flags = *(volatile uint8_t *)(uintptr_t)(rec + 6);
    uint8_t *geom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    int changed = 0;
    if ((flags & 3) < 2) {
        /* variant A — direct slot */
        uint32_t ebx = (flags & 1) ? 6u : 8u;
        uint16_t cx = (uint16_t)(*(volatile uint16_t *)(uintptr_t)(geom + ref + ebx) + 1);  /* source + 1 */
        for (uint32_t k = 0; k < count; k++) {
            volatile uint16_t *slot = (volatile uint16_t *)(uintptr_t)(geom + outbuf[2 + k] + ebx);
            if (cx != *slot) { changed = 1; *slot = cx; }
        }
    } else {
        /* variant B — indirect slot through word[cell+0x18] */
        uint32_t ebx = (flags & 1) ? 0u : 6u;
        int32_t ecx = -1;                                            /* sentinel: source value not yet taken */
        uint16_t rind = *(volatile uint16_t *)(uintptr_t)(geom + ref + 0x18);
        if (rind != 0)
            ecx = (int32_t)(uint16_t)(*(volatile uint16_t *)(uintptr_t)(geom + rind + ebx)) + 1;
        for (uint32_t k = 0; k < count; k++) {
            uint16_t mind = *(volatile uint16_t *)(uintptr_t)(geom + outbuf[2 + k] + 0x18);
            if (mind == 0) continue;                                 /* je 0x318d5 */
            volatile uint16_t *slot = (volatile uint16_t *)(uintptr_t)(geom + mind + ebx);
            if (ecx < 0)                                             /* take source from this match (once) */
                ecx = (int32_t)(uint16_t)(*slot) + 1;
            uint16_t cx = (uint16_t)ecx;
            if (cx != *slot) { changed = 1; *slot = cx; }
        }
    }
    if (changed == 0) return 0;                                     /* 0x31842: nothing changed -> ret 0 */
    if (flags & 0x10)
        *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x8;
    return -1;
}

/* cmd_sync_facegroup_texture (base 0x14, 0x315c4, LATENT): sync a face group's texture slot to a reference
 * record's value + 1. A direct-only cousin of cmd_cycle_texture's variant A: reference =
 * find_raw_state_record(word[rec+0xa]) (unless 0 or == the key word[rec+8]); collect the faces via
 * gather_faces_by_id(key); if no reference resolved, use the first match. Read the reference's slot value at
 * geom + ref + dirOff (dirOff = (flags&1)?6 : (flags&2)?4 : 2), +1, write to every match's slot that differs.
 * Returns -1 if anything changed (skip bit when flags&0x10), else 0. Writes the geometry buffer (0x90aa8). */
int32_t cmd_sync_facegroup_texture(uint32_t rec)
{
    uint16_t outbuf[0xc8];
    uint16_t av = *(volatile uint16_t *)(uintptr_t)(rec + 0xa);
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    uint16_t ref = (av == 0 || av == key) ? 0 : (uint16_t)find_raw_state_record(av);
    uint32_t count = (uint32_t)gather_faces_by_id(key, 0xc8, (uint32_t)(uintptr_t)outbuf) & 0xffffu;
    if (count == 0) return 0;                                       /* 0x31669 cleanup -> ret 0 */
    if (ref == 0) ref = outbuf[2];                                  /* fall back to the first match */
    uint8_t flags = *(volatile uint8_t *)(uintptr_t)(rec + 6);
    uint32_t ebx = (flags & 1) ? 6u : (flags & 2) ? 4u : 2u;
    uint8_t *geom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    uint16_t cx = (uint16_t)(*(volatile uint16_t *)(uintptr_t)(geom + ref + ebx) + 1);
    int changed = 0;
    for (uint32_t k = 0; k < count; k++) {
        volatile uint16_t *slot = (volatile uint16_t *)(uintptr_t)(geom + outbuf[2 + k] + ebx);
        if (cx != *slot) { changed = 1; *slot = cx; }
    }
    if (changed == 0) return 0;
    if (flags & 0x10)
        *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x8;
    return -1;
}

/* [HOST-TOLERANCE] g_object_ptr_array element count. The command-ptr array is
 * allocated (0x33ea1) as word[g_object_table_header(0x85c30)+6] entries of 4 bytes, and ONLY when the
 * header magic word[hdr]==0x7533 (disasm 0x33eb8) is present. resolve_command_by_index (0x315a7) has NO
 * upper-bound gate (`sub ax,1; jb` catches only ax==0), so an out-of-bounds chain index reads past the
 * chunk. This returns the real element count when a valid header is loaded, else 0 => the OOB guards below
 * are DISABLED and behavior is byte-faithful. The magic gate is why the guard is dormant in the oracle:
 * no oracle fixture stages a 0x7533 header (verified), so cnt==0 there and every walk/exec case runs the
 * original code path unchanged. */
static uint32_t rcw_cmd_array_count(void)
{
    uint32_t hdr = (uint32_t)G32(VA_g_object_table_header);
    if (hdr == 0) return 0;
    if (*(volatile uint16_t *)(uintptr_t)hdr != 0x7533) return 0;    /* not the object-table header */
    return (uint32_t)*(volatile uint16_t *)(uintptr_t)(hdr + 6);     /* word[hdr+6] = entry count */
}

/* walk_command_chain_flow (0x353c4): the flow/condition PRE-PASS of the command spine. Given a 1-based chain
 * index (AX), follow the NEXT links (word[rec+4]) and decide whether the chain is gated. It runs only the two
 * flow opcodes, not the action handlers: a Delay Timer (base 0x12) BLOCKS when modifier bit 0x4 is clear and
 * bit 0x01|0x20 is set; a Modify Count (base 0x1e) recurses into its sub-chain word[rec+8] and blocks if that
 * sub-walk blocks. Returns 0 = chain OK / end, or the (host ptr of the) blocking record = gated. Read-only;
 * resolves records via resolve_command_by_index. Self-recursing for the 0x1e conditional.
 *
 * [HOST-TOLERANCE] carries a default-ON OOB guard (see rcw_cmd_array_count). Rationale: a chain index >= the
 * array's own element count is a latent 1996 data bug. On flat DOS/4GW resolve reads array[idx] out of the
 * pool chunk into ADJACENT IN-SEGMENT heap — a garbage 4-byte "record ptr" that is still a mapped flat-
 * segment address, so the deref does not fault; the garbage base byte[rec+3] almost never equals the only
 * two gate opcodes (0x12 Delay / 0x1e Modify-Count), so the walk chases garbage NEXT links to a terminating
 * 0 and returns "chain OK" (0). On the HOST that same array[idx] is a full host pointer (captured
 * values: 0xf3ea9f00 in the audio mmap arena, or unmapped 0x4c4b4a4b) and derefing rec+3 HARD-FAULTS.
 * The guard reproduces the flat-DOS statistical outcome (bail as chain-OK) so the host survives. It is a
 * NO-OP for every in-bounds index (1..count) => byte-exact with the original for all inputs the game or the
 * oracle can legitimately produce; it diverges ONLY on an OOB index, which is un-oracle-able (the original
 * side would wild-deref and crash the oracle process itself — see the raw_commands suite note). The action
 * pass rawcmd_exec_loop carries the SYMMETRIC guard (it derefs the same wild rec from the same unbounded
 * array[idx*4]), so a guard here alone would just move the fault there. ROTH_WALK_DEBUG logs before bailing:
 * FIRST iteration = a bad INITIAL index handed in by a caller (firer word[oth+4+u]); later = a bad NEXT
 * link word[rec+4]. */
uint32_t walk_command_chain_flow(uint16_t ax)
{
    int first = 1;
    for (;;) {
        if (ax == 0) return 0;                                       /* and eax,0xffff; je ret */
        uint32_t idx = (uint32_t)(uint16_t)(ax - 1);
        uint32_t cnt = rcw_cmd_array_count();
        if (cnt != 0 && idx >= cnt) {                                /* [HOST-TOLERANCE] OOB chain index */
            if (rcw_walk_debug()) {
                fprintf(stderr, "[walkdbg] OOB chain index ax=0x%04x (idx=%u) %s; array count=%u "
                                "(%d past end) -> [HOST-TOLERANCE] bail as chain-OK\n",
                        (unsigned)ax, (unsigned)idx,
                        first ? "INITIAL (handed in by a caller/firer)"
                              : "NEXT-link word[rec+4] (bad chain link)",
                        (unsigned)cnt, (int)(idx - cnt + 1));
                fflush(stderr);
            }
            return 0;                                                /* flat-DOS garbage walk -> chain OK */
        }
        uint32_t rec = resolve_command_by_index(ax);          /* call 0x315a7 */
        if (rec == 0) return 0;                                      /* or eax,eax; je ret */
        /* (A round-1 "wild rec" range backstop was REMOVED here: legitimate command records live in the
         * DPMI-heap mmap region (0xf3xxxxxx), so a rec>=0x10000000 test misfired on EVERY valid record.
         * The count gate above is the real OOB protection.) */
        uint8_t base = *(volatile uint8_t *)(uintptr_t)(rec + 3);
        uint8_t mod  = *(volatile uint8_t *)(uintptr_t)(rec + 2);
        if (base == 0x12 && !(mod & 0x4) && (mod & 0x21))            /* Delay Timer gate */
            return rec;
        if (base == 0x1e) {                                          /* Modify Count conditional */
            uint16_t sub = *(volatile uint16_t *)(uintptr_t)(rec + 8);
            if (sub != 0 && walk_command_chain_flow(sub) != 0)
                return rec;                                          /* sub-chain blocked -> this rec blocks */
        }
        ax = *(volatile uint16_t *)(uintptr_t)(rec + 4);             /* follow NEXT */
        first = 0;
    }
}

/* [DIAG] firer source tap: the entry firers 0x34d75 / 0x34f5a hand the walker an INITIAL chain index read
 * from word[oth+4+u]. Under ROTH_WALK_DEBUG, if that initial index is OOB, log which firer channel and the
 * (oth,u) that produced it so a live capture pins the SOURCE. Pure pass-through when the flag is off (and a
 * no-op even when on, unless the index is OOB) => oracle behavior is identical. */
static uint32_t rcw_walk_from_firer(uint16_t ax, const char *chan, uint32_t oth, uint32_t u)
{
    if (rcw_walk_debug() && ax != 0) {
        uint32_t cnt = rcw_cmd_array_count();
        if (cnt != 0 && (uint32_t)(uint16_t)(ax - 1) >= cnt) {
            fprintf(stderr, "[walkdbg] SOURCE=%s oth=0x%08x u=0x%x (index@0x%08x) -> INITIAL chain index "
                            "ax=0x%04x is OOB (array count=%u)\n",
                    chan, (unsigned)oth, (unsigned)u, (unsigned)(oth + 4 + u),
                    (unsigned)ax, (unsigned)cnt);
            fflush(stderr);
        }
    }
    return walk_command_chain_flow(ax);
}

/* find_secondary_state_record_by_key (0x34d14): scan the object index array (g_map_objects_buffer 0x90aa4 + 2,
 * count from g_map_geometry_buffer 0x90aa8 + word[geom+4] - 2) and its groups' sub-records (count = byte[grp],
 * stride 0x10 from grp+2) for the FIRST whose word[sub+0xe] == key. Returns the sub-record's HOST POINTER (or
 * 0). Pure obj3 leaf (only AX = key matters; the caller's EDX/EBX are ignored). */
uint32_t find_secondary_state_record_by_key(uint16_t key)
{
    uint8_t *geom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    uint32_t objbuf = (uint32_t)G32(VA_g_map_objects_buffer);
    uint8_t *idxsec = geom + *(uint16_t *)(geom + 4);
    uint32_t cnt = *(uint16_t *)(idxsec - 2);
    uint8_t *p   = (uint8_t *)(uintptr_t)(objbuf + 2);
    uint8_t *end = p + cnt * 2;
    while (p < end) {
        uint16_t idx = *(uint16_t *)p; p += 2;
        if (idx == 0) continue;
        uint8_t *esi = (uint8_t *)(uintptr_t)(objbuf + idx);
        int32_t subcnt = *(uint8_t *)esi;
        if (subcnt == 0) continue;
        esi += 2;
        do {
            if (*(volatile uint16_t *)(uintptr_t)(esi + 0xe) == key)
                return (uint32_t)(uintptr_t)esi;                     /* found -> host ptr */
            esi += 0x10;
        } while (--subcnt > 0);
    }
    return 0;
}

/* cmd_cycle_object_texture (base 0x20, 0x31700): step an animated OBJECT texture. The objects-buffer cousin
 * of cmd_cycle_texture variant A: reference = find_secondary_state_record_by_key(word[rec+0xa]) (host ptr;
 * unless 0 or == key word[rec+8]); collect the objects via resolve_command_objects(key) (offsets, no header);
 * if no reference, use the first match (objbuf + out[0]). Read the reference's slot word[ref+4] + 1, write it
 * to every match's slot objbuf[off+4] that differs. -1 if anything changed (skip bit when flags&0x10) else 0. */
int32_t cmd_cycle_object_texture(uint32_t rec)
{
    uint16_t outbuf[0xc8];
    uint16_t av = *(volatile uint16_t *)(uintptr_t)(rec + 0xa);
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    uint32_t ref = (av == 0 || av == key) ? 0 : find_secondary_state_record_by_key(av);
    uint32_t count = (uint32_t)resolve_command_objects(key, 0xc8, (uint32_t)(uintptr_t)outbuf) & 0xffffu;
    if (count == 0) return 0;
    uint32_t objbuf = (uint32_t)G32(VA_g_map_objects_buffer);
    if (ref == 0) ref = objbuf + outbuf[0];                          /* fall back to first match (offset -> ptr) */
    uint16_t cx = (uint16_t)(*(volatile uint16_t *)(uintptr_t)(ref + 4) + 1);
    int changed = 0;
    for (uint32_t k = 0; k < count; k++) {
        volatile uint16_t *slot = (volatile uint16_t *)(uintptr_t)(objbuf + outbuf[k] + 4);
        if (cx != *slot) { changed = 1; *slot = cx; }
    }
    if (changed == 0) return 0;
    if (*(volatile uint8_t *)(uintptr_t)(rec + 6) & 0x10)
        *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x8;
    return -1;
}

/* execute_command_chain (0x3065a) — THE executor loop of the command spine. Given the chain head EDI, walk
 * the NEXT-linked command records (resolved via g_object_ptr_array 0x8a0d8) and DISPATCH each through the
 * 0x30780 table (`call [base&0x7f *4 + 0x30780]`, the handler getting ESI=EDI=record), OR-accumulating each
 * return into g_command_chain_result 0x8a138. Stops on: a record skip-bit ([rec+2]&8 skips its dispatch), the
 * chain-interrupt 0x8a268 being set, a Delay Timer (base 0x12), or NEXT==0. Interrupt==2 with a saved active
 * index (0x8a0c8) RE-RUNS the chain. Then the post-chain done-tail fires SFX (0x27270/0x2730b, gated on
 * acted+!suppress+pending-rot), a door-flip, and finish_dialogue_record_eval (0x1db5e). The dispatch + the
 * done-tail host callees are BRIDGED via call_orig (intentional). Callers override the return with -1. */
int32_t execute_command_chain(uint32_t edi)
{
    return rawcmd_exec_loop(edi, *(volatile uint16_t *)(uintptr_t)(edi + 4));   /* 0x3065a: ax = NEXT(edi) */
}

/* finalize_command_chain (0x3065e): the executor loop's inner entry — AX = the chain index (already set; edi is
 * don't-care). reset_command_chain_state + the internal re-run reach the loop here. Same body as
 * execute_command_chain, minus the NEXT-link load. */
int32_t finalize_command_chain(uint16_t ax)
{
    return rawcmd_exec_loop(0, ax);                                              /* 0x3065e (edi unused) */
}

/* The executor loop body, entered at 0x3065e with `ax` already set (reset_command_chain_state + the internal
 * re-run use this entry; `edi` is don't-care until a command resolves). */
static int32_t rawcmd_exec_loop(uint32_t edi, uint16_t ax)
{
  exec_loop:                                                         /* 0x3065e (re-entry; ax preset) */
    for (;;) {
        if (ax == 0) break;                                          /* sub ax,1; jb done */
        uint32_t idx = (uint32_t)(uint16_t)(ax - 1);
        {                                                            /* [HOST-TOLERANCE] symmetric with the walker */
            uint32_t cnt = rcw_cmd_array_count();
            if (cnt != 0 && idx >= cnt) {
                /* The action pass derefs the SAME array[idx*4] the resolver reads unbounded — an OOB index
                 * would hand a wild host `rec` straight into the bridged handler dispatch below (which
                 * derefs rec+2/+3 and the record body) and HARD-FAULT on the host. On flat DOS the garbage
                 * dispatch is effectively a no-op that runs the chain to a terminating NEXT==0; terminating
                 * here reproduces "the chain does nothing further" without the fault. NO-OP for in-bounds
                 * indices (byte-exact) and dormant in the oracle (no 0x7533 header). */
                if (rcw_walk_debug()) {
                    fprintf(stderr, "[walkdbg] exec-loop OOB chain index ax=0x%04x (idx=%u) count=%u "
                                    "-> [HOST-TOLERANCE] terminate chain\n",
                            (unsigned)ax, (unsigned)idx, (unsigned)cnt);
                    fflush(stderr);
                }
                break;
            }
        }
        uint32_t base_arr = *(uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_object_ptr_array);   /* esi=[0x8a0d8]; esi=[esi] */
        uint32_t rec = *(uint32_t *)(uintptr_t)(base_arr + idx * 4);          /* esi=[esi+idx*4] */
        edi = rec;                                                   /* edi = esi */
        if (!(*(volatile uint8_t *)(uintptr_t)(edi + 2) & 8)) {      /* test [edi+2],8; jne skip_dispatch */
            uint32_t bs = *(volatile uint8_t *)(uintptr_t)(rec + 3) & 0x7fu;  /* COMMAND_BASE & 0x7f */
            G32(VA_g_item_drop_position + 0x11c) = (int32_t)(uint32_t)G32(VA_g_item_drop_position + 0x120);          /* [0x8a0c8] = [0x8a0cc] */
            G32(VA_g_item_drop_position + 0x120) = 0;
            uint32_t handler = *(uint32_t *)(uintptr_t)(0x30780u + OBJ_DELTA + bs * 4);   /* table[base] */
            regs_t io; memset(&io, 0, sizeof io);
            io.va = handler; io.esi = rec; io.edi = rec;
#ifndef ROTH_STANDALONE
            call_orig(&io);                                          /* call [ebx*4+0x30780] — BRIDGED */
#else
            io.eax = rawcmd_dispatch_30780(io.va, io.esi);   /* the 0x30780 table -> lifted cmd bodies */
#endif
            edi = rec;                                               /* pop edi */
            G32(VA_g_player_move_delta_z + 0xc) = (int32_t)((uint32_t)G32(VA_g_player_move_delta_z + 0xc) | io.eax);  /* accumulate the handler return */
            if (G8(VA_g_command_chain_interrupt) != 0) break;                            /* a handler broke the chain */
        }
        if (*(volatile uint8_t *)(uintptr_t)(edi + 3) == 0x12) break;          /* Delay Timer stops the chain */
        if (*(volatile uint16_t *)(uintptr_t)(edi + 4) != 0) {                 /* follow NEXT */
            ax = *(volatile uint16_t *)(uintptr_t)(edi + 4);
            continue;
        }
        break;
    }
    /* done (0x306bb): */
    uint32_t saved = (uint32_t)G32(VA_g_item_drop_position + 0x11c);
    G32(VA_g_item_drop_position + 0x11c) = 0;
    if (G8(VA_g_command_chain_interrupt) == 2 && saved != 0) {                          /* interrupt==2 + saved active -> re-run */
        G8(VA_g_command_chain_interrupt) = 0;
        ax = (uint16_t)saved;
        goto exec_loop;
    }
    /* post-chain done-tail (0x306e3): */
    uint8_t al = (uint8_t)((uint32_t)G32(VA_g_player_move_delta_z + 0xc)) | G8(VA_g_command_chain_interrupt);
    if ((al & 1) && G32(VA_g_item_autoselected_flag) == 0) {
        uint32_t snd = (uint32_t)G32(VA_g_state_link_buf_ptr + 0x128);                     /* mov eax,[0x8a264] (dword read) */
        if (snd != 0) {                                            /* pending command SFX (re-pointed) */
            /* 0x30704: sub edx,edx; dec eax -> BOTH branches play id = [0x8a264]-1, param 0
             * (the pre-fix hardcoded id 0 loaded SFX-bank row 0 — the silent-scripted-SFX bug) */
            if ((uint32_t)G32(VA_g_state_link_buf_ptr + 0x124) == 0x80008000u)
                play_sound_effect(snd - 1, 0);              /* 0x27270 (eax=id, edx=0) */
            else
                play_command_sound(snd - 1, 0,              /* 0x2730b (eax=id, edx=0, bx/cx = warp) */
                    *(volatile uint16_t *)GADDR(VA_g_state_link_buf_ptr + 0x124),
                    *(volatile uint16_t *)GADDR(VA_g_state_link_buf_ptr + 0x126));
        }
        if (G32(VA_g_player_move_delta_z + 0x8) != 0) {                                   /* door-flip on the active chain head */
            uint32_t da = (uint32_t)G32(VA_g_anim_step_mode + 0x8);
            if (da != 0 && *(volatile uint8_t *)(uintptr_t)(da + 3) == 0x8) {
                if (*(volatile uint8_t *)(uintptr_t)(da + 6) & 0x10)
                    *(volatile uint8_t *)(uintptr_t)(da + 2) |= 8;
                if (!(*(volatile uint8_t *)(uintptr_t)(da + 6) & 1)) {
                    uint32_t db = (uint32_t)G32(VA_g_player_move_delta_z + 0x8);
                    *(volatile uint16_t *)(uintptr_t)(db + 4) ^= 1;
                }
            }
        }
    }
    finish_dialogue_record_eval();                                                    /* 0x1db5e (re-pointed) */
    G8(VA_g_command_chain_interrupt + 0x4) = 0;
    G32(VA_g_item_autoselected_flag) = 0;
    return -1;
}

/* reset_command_chain_state (0x305b6): zero the executor state, then run the chain from index AX (the executor
 * re-entry 0x3065e), then exit (clear g_command_active_chain[0x8a134], return -1). A BALANCED pusha/popa entry
 * (unlike run_command_chain's fall-through body) — its callers are process_deferred_command_queue,
 * register_command_save_link, reset_command_chain_no_source. */
int32_t reset_command_chain_state(uint16_t ax)
{
    G32(VA_g_player_move_delta_z + 0xc) = 0;                                                /* g_command_chain_result */
    G8(VA_g_command_chain_interrupt) = 0;                                                 /* g_command_chain_interrupt */
    G32(VA_g_anim_step_mode + 0x8) = 0;                                                /* g_command_active_chain */
    G32(VA_g_if_item_list_count) = 0;
    G32(VA_g_item_drop_position + 0x120) = 0;                                                /* g_command_next_active */
    rawcmd_exec_loop(0, ax);                                         /* call 0x3065e (edi unused at entry) */
    G32(VA_g_player_move_delta_z + 0x8) = 0;                                               /* 0x304a9 exit */
    return -1;
}

/* ------------------------------------------------------------------------------------------------
 * The two map-load-installed texture -> RAW-chain code-pointer hooks. Both are CODE-PTR-ONLY
 * targets (callsto == 0 in the image):
 * installed by map load (lift_map_load.c:1129) into [0x90a34] / [0x8a2a0] and reached only via
 * `call [slot]` from the renderer (rwss `indirect:` 0x36858, the 0x39093 span-callback carrier,
 * faceres 0x2c790). The [0x85c30] record + its word-offset entry tables are HOST pointers -> deref
 * RAW (STORED-POINTER rule); chain-state globals via G8/G16/G32. Pure-DS leaves (no selectors);
 * the originals clobber only EBX (dead at every caller), so plain C is register-faithful. Both
 * funnel into reset_command_chain_state (0x305b6) above. */

/* texture_anim_command_hook (0x33cf3, via [0x90a34]): on an animated texture's frame boundary, fire
 * the map's per-texture RAW command chain. EAX = sext16(texture id) (the 0x39093 carrier cwde's
 * before the call), EDX = DAS block ptr. Walks the [0x85c30] record's binding table (off = +0x28,
 * count = +0x2a; entries are word offsets into the record). EAX return is the NATURAL residue:
 * the input id on every no-op path, the 0x305b6 result (-1) on the fire path — real downstream
 * data flow, so this must never be voided. */
uint32_t texture_anim_command_hook(uint32_t eax_cwde, uint32_t edx_block)
{
    uint32_t eax = eax_cwde;                                        /* the natural-return carrier */
    /* 0x33cf6-0x33d0e: rec = [0x85c30]; count = zx16(rec[+0x2a]); bail if either is 0 */
    uint32_t rec = (uint32_t)G32(VA_g_object_table_header);
    if (rec == 0) return eax;                                       /* 0x33cfc or esi,esi; je exit */
    uint32_t cnt = (uint32_t)*(volatile uint16_t *)(uintptr_t)(rec + 0x2a);
    if (cnt == 0) return eax;                                       /* 0x33d0e je exit */
    /* 0x33d14-0x33d1a: cursor = rec + zx16(rec[+0x28]) */
    uint32_t cur = rec + (uint32_t)*(volatile uint16_t *)(uintptr_t)(rec + 0x28);
    do {
        /* 0x33d1e-0x33d21: ent = zx16(*cursor); cursor += 2 */
        uint32_t ent = (uint32_t)*(volatile uint16_t *)(uintptr_t)cur;
        cur += 2;
        if (*(volatile uint16_t *)(uintptr_t)(rec + ent + 8) == (uint16_t)eax   /* 0x33d24 id match */
            && !(*(volatile uint8_t *)(uintptr_t)(rec + ent + 2) & 8)) {        /* 0x33d2f fired -> next entry */
            /* 0x33d3a: the block must be flagged animated (word test, bit 0x100) */
            if (!(*(volatile uint16_t *)(uintptr_t)(edx_block + 0xa) & 0x100)) return eax;
            /* 0x33d46: das tick [0x90c0a] == block frame [+0x18] -> nothing new this frame */
            if ((uint16_t)G16(VA_g_das_cache_tick) == *(volatile uint16_t *)(uintptr_t)(edx_block + 0x18)) return eax;
            uint32_t e = rec + ent;                                 /* 0x33d57 edi repurposed = entry ptr */
            /* 0x33d5b: arm the period [ent+0xa] = block[+0x16]-1 on first sight (32-bit dec, cx stored) */
            if (*(volatile uint16_t *)(uintptr_t)(e + 0xa) == 0)
                *(volatile uint16_t *)(uintptr_t)(e + 0xa) =
                    (uint16_t)(*(volatile uint16_t *)(uintptr_t)(edx_block + 0x16) - 1);
            /* 0x33d6b: track the block frame into [ent+0xc] */
            uint16_t frm = *(volatile uint16_t *)(uintptr_t)(edx_block + 0x18);
            if (frm == 0xffffu) {                                   /* 0x33d6f cmp cx,-1 */
                *(volatile uint16_t *)(uintptr_t)(e + 0xc) = 0;     /* 0x33dd4 reset tracker, NO fire */
                return eax;
            }
            if (frm == *(volatile uint16_t *)(uintptr_t)(e + 0xc)) return eax;  /* 0x33d75 unchanged */
            *(volatile uint16_t *)(uintptr_t)(e + 0xc) = frm;       /* 0x33d7b */
            if (frm != *(volatile uint16_t *)(uintptr_t)(e + 0xa)) return eax;  /* 0x33d7f not the wrap boundary */
            /* 0x33d85-0x33db2: zero the chain-state set, then fire the chain from [ent+4] */
            G32(VA_g_active_object) = 0;
            G32(VA_g_active_object + 0x4) = 0;
            G32(VA_g_command_source_object) = 0;
            G16(VA_g_state_link_buf_ptr + 0x128) = 0;
            G32(VA_g_player_move_delta_z + 0x8) = 0;
            /* 0x33db6: mov ax,[edi+4]; call 0x305b6 -> EAX = the executor result (-1) = the return */
            eax = (uint32_t)reset_command_chain_state(*(volatile uint16_t *)(uintptr_t)(e + 4));
            if (*(volatile uint8_t *)(uintptr_t)(e + 6) & 0x10)     /* 0x33dbf one-shot latch */
                *(volatile uint8_t *)(uintptr_t)(e + 2) |= 8;       /* 0x33dc5 */
            return eax;                                             /* 0x33dc9 jmp exit */
        }
    } while ((int32_t)--cnt > 0);                                   /* 0x33dcb dec ecx; jg loop */
    return eax;                                                     /* 0x33dda exit: eax untouched */
}

/* texture_id_remap_hook (0x33dde, via [0x8a2a0]): remap a texture id through the [0x85c30] record's
 * remap table (off = +0x3c, count = +0x3e), firing the RAW chain [ent+4] whenever the mapping-context
 * key (([0x90c0a] das tick << 16) | [ent+4]) differs from the cache [0x8a0d0]. EAX = plain id (the
 * rwss caller does the shr-by-1 itself, 0x36864). Return = the saved-EAX stack slot (push eax
 * 0x33de2 / pop eax 0x33e9b): the input id verbatim, EXCEPT on the remap path where 0x33e8c
 * `mov word [esp], ax` overwrites ONLY the low 16 bits with [ent+0xa] — the high 16 bits of the
 * return are the INPUT's high 16, preserved verbatim here (clean in practice: every caller passes
 * an id < 0x1200). The 0x305b6 result is DISCARDED (0x33e70 overwrites AL; the residue never
 * reaches the return slot). Only EBX clobbered (esi/edi/ecx/edx saved). */
uint32_t texture_id_remap_hook(uint32_t id)
{
    uint32_t ret = id;                                              /* 0x33de2: the saved-EAX slot */
    /* 0x33de3-0x33dfb: rec = [0x85c30]; count = zx16(rec[+0x3e]); bail if either is 0 */
    uint32_t rec = (uint32_t)G32(VA_g_object_table_header);
    if (rec == 0) return ret;                                       /* 0x33deb je exit */
    uint32_t cnt = (uint32_t)*(volatile uint16_t *)(uintptr_t)(rec + 0x3e);
    if (cnt == 0) return ret;                                       /* 0x33dfb je exit */
    uint32_t cur = rec + (uint32_t)*(volatile uint16_t *)(uintptr_t)(rec + 0x3c);  /* 0x33e01 */
    do {
        uint32_t ent = (uint32_t)*(volatile uint16_t *)(uintptr_t)cur;   /* 0x33e0b */
        cur += 2;
        if (*(volatile uint16_t *)(uintptr_t)(rec + ent + 8) == (uint16_t)id    /* 0x33e11 id match */
            && !(*(volatile uint8_t *)(uintptr_t)(rec + ent + 2) & 8)) {        /* 0x33e18 fired -> next entry */
            /* 0x33e1f-0x33e2d: key = ([0x90c0a]<<16) | [ent+4]; compare vs the cache [0x8a0d0] */
            uint32_t key = ((uint32_t)(uint16_t)G16(VA_g_das_cache_tick) << 16)
                         | (uint32_t)*(volatile uint16_t *)(uintptr_t)(rec + ent + 4);
            if (key != (uint32_t)G32(VA_g_item_drop_position + 0x124)) {                    /* 0x33e2d jne -> context change */
                G32(VA_g_item_drop_position + 0x124) = (int32_t)key;                        /* 0x33e35 update the cache */
                G16(VA_g_state_link_buf_ptr + 0x128) = 0;                                   /* 0x33e3a chain-state zero set */
                G32(VA_g_player_move_delta_z + 0x8) = 0;                                   /* 0x33e43 */
                G32(VA_g_active_object) = 0;                                   /* 0x33e4d */
                G32(VA_g_active_object + 0x4) = 0;                                   /* 0x33e57 */
                G32(VA_g_command_source_object) = 0;                                   /* 0x33e61 */
                /* 0x33e6b: call 0x305b6 with AX = key.low16 = [ent+4]; result discarded (see above) */
                (void)reset_command_chain_state((uint16_t)key);
                G8(VA_g_item_drop_position + 0x128) = (uint8_t)(G8(VA_g_command_chain_interrupt) & 2);           /* 0x33e70-0x33e77 the remap gate */
            }
            if (G8(VA_g_item_drop_position + 0x128) == 0)                                   /* 0x33e7c: gate open -> remap */
                ret = (ret & 0xffff0000u)                           /* 0x33e8c mov [esp],ax (low16 only) */
                    | (uint32_t)*(volatile uint16_t *)(uintptr_t)(rec + ent + 0xa);
            return ret;                                             /* 0x33e90 / 0x33e83 -> pop eax; ret */
        }
    } while ((int32_t)--cnt > 0);                                   /* 0x33e92 dec ecx; jg loop */
    return ret;                                                     /* 0x33e9b exit: input id through */
}

#ifdef ROTH_STANDALONE
/* two-value dispatch shims for the hook slots, imgfree lane ONLY (the trap lane keeps
 * its byte-identical `call [slot]` bridges). Each slot's value set is exactly {0, canon+OBJ_DELTA}:
 * the map-load installer (0x33f65/0x33f6f -> lift_map_load.c:1129, storing the canon+Delta token) is
 * the only writer in the whole image, and the slots start 0 (never re-zeroed). Anything else fails
 * loud (roth_unreachable aborts). NB the original 0x39093 carrier's NULL path returns the RAW
 * (un-sign-extended) ebx while the hook path cwde's first — identical whenever the id's high half is
 * clean (both render callers pass ids < 0x1200); the wiring site owns where the cwde happens. */

/* the 0x30780 RAW-command HANDLER TABLE dispatch —
 * 128 static obj1-resident code pointers (fixup-carrying; staged as canon+delta tokens by
 * roth_boot's rawcmd table loop), every distinct target a verified lifted body from the closed
 * raw_command_system subsystem (int32_t cmd_*(uint32_t rec)). All three exec-loop sites
 * consume the handler's EAX (the chain-result accumulate / ebx thread / queued word). */
uint32_t rawcmd_dispatch_30780(uint32_t handler_rt, uint32_t rec)
{
    /* ROTH_RAWCMD_TRACE=1: per-dispatch door-chain trace (re-open observe-first instrument). */
    static int trc = -1;
    if (trc < 0) { const char *e = getenv("ROTH_RAWCMD_TRACE"); trc = (e && *e == '1') ? 1 : 0; }
    if (trc == 1) {
        uint32_t canon = handler_rt - OBJ_DELTA;
        if (canon == 0x33a69u || canon == 0x32195u || canon == 0x32221u || canon == 0x32ac5u) {
            uint32_t r = 0;
            switch (canon) {
            case 0x33a69u: r = (uint32_t)cmd_open_door(rec); break;
            case 0x32195u: r = (uint32_t)cmd_delay_timer(rec); break;
            case 0x32221u: r = (uint32_t)tick_delay_timer(rec); break;
            case 0x32ac5u: r = (uint32_t)cmd_move_sector(rec); break;
            }
            fprintf(stderr, "[rawtrc] h=%05x rec=%08x b2=%02x b3=%02x b5=%02x b6=%02x next=%04x ret=%d acc=%08x\n",
                    canon, rec,
                    *(volatile uint8_t *)(uintptr_t)(rec+2), *(volatile uint8_t *)(uintptr_t)(rec+3),
                    *(volatile uint8_t *)(uintptr_t)(rec+5), *(volatile uint8_t *)(uintptr_t)(rec+6),
                    (unsigned)*(volatile uint16_t *)(uintptr_t)(rec+4),
                    (int32_t)r, (unsigned)G32(VA_g_player_move_delta_z + 0xc));
            return r;
        }
    }
    switch (handler_rt - OBJ_DELTA) {
    case 0x304b8u: return (uint32_t)cmd_run_indexed_object_command(rec);
    case 0x30ab0u: return (uint32_t)cmd_default_nop(rec);
    case 0x30ab3u: return (uint32_t)cmd_set_player_speed_reduction(rec);
    case 0x30acbu: return (uint32_t)cmd_player_rotation(rec);
    case 0x30b23u: return (uint32_t)cmd_empty_allow_sfx(rec);
    case 0x30d10u: return (uint32_t)cmd_spawn_object_adv(rec);
    case 0x30f51u: return (uint32_t)cmd_06_empty_noop(rec);
    case 0x30f55u: return (uint32_t)cmd_jump_if_next_fails(rec);
    case 0x30f63u: return (uint32_t)cmd_set_inventory_filter(rec);
    case 0x30f83u: return (uint32_t)cmd_face_emits_damage(rec);
    case 0x31000u: return (uint32_t)cmd_change_object_id(rec);
    case 0x3104au: return (uint32_t)cmd_map_transition(rec);
    case 0x31107u: return (uint32_t)cmd_change_object_height(rec);
    case 0x311adu: return (uint32_t)cmd_particle_effect(rec);
    case 0x3121cu: return (uint32_t)cmd_change_height(rec);
    case 0x312a1u: return (uint32_t)cmd_modify_sector(rec);
    case 0x31326u: return (uint32_t)cmd_dbase100_if_next_fails(rec);
    case 0x31339u: return (uint32_t)cmd_activate_sfx_node(rec);
    case 0x313b4u: return (uint32_t)cmd_spawn_object(rec);
    case 0x3146du: return (uint32_t)cmd_rotate_object(rec);
    case 0x31563u: return (uint32_t)cmd_toggle_command(rec);
    case 0x315c4u: return (uint32_t)cmd_sync_facegroup_texture(rec);
    case 0x31676u: return (uint32_t)cmd_smash_face_texture(rec);
    case 0x31700u: return (uint32_t)cmd_cycle_object_texture(rec);
    case 0x3179cu: return (uint32_t)cmd_cycle_texture(rec);
    case 0x318fdu: return (uint32_t)cmd_count(rec);
    case 0x3198eu: return (uint32_t)cmd_texture_change_count(rec);
    case 0x31a62u: return (uint32_t)cmd_count_addl_arg(rec);
    case 0x31b1au: return (uint32_t)cmd_animate_facegroup_texture(rec);
    case 0x31c31u: return (uint32_t)cmd_modify_count(rec);
    case 0x320e6u: return (uint32_t)cmd_apply_damage(rec);
    case 0x32195u: return (uint32_t)cmd_delay_timer(rec);
    case 0x32221u: return (uint32_t)tick_delay_timer(rec);
    case 0x32269u: return (uint32_t)cmd_flash_lights(rec);
    case 0x32324u: return (uint32_t)tick_flash_lights(rec);
    case 0x32473u: return (uint32_t)cmd_scroll_sector_texture(rec);
    case 0x324a7u: return (uint32_t)cmd_scroll_face_texture(rec);
    case 0x324d2u: return (uint32_t)tick_scroll_sector_texture(rec);
    case 0x32592u: return (uint32_t)tick_scroll_face_texture(rec);
    case 0x32626u: return (uint32_t)cmd_change_floor_texture(rec);
    case 0x32645u: return (uint32_t)cmd_change_face_texture_adv(rec);
    case 0x32738u: return (uint32_t)cmd_change_face_texture(rec);
    case 0x327f8u: return (uint32_t)cmd_change_object_texture(rec);
    case 0x3286bu: return (uint32_t)tick_change_object_texture(rec);
    case 0x32ac5u: return (uint32_t)cmd_move_sector(rec);
    case 0x32c05u: return (uint32_t)tick_moving_sector(rec);
    case 0x32d7du: return (uint32_t)tick_change_height(rec);
    case 0x33091u: return (uint32_t)tick_change_object_height(rec);
    case 0x33188u: return (uint32_t)tick_rotate_object(rec);
    case 0x33229u: return (uint32_t)tick_change_floor_texture(rec);
    case 0x333c0u: return (uint32_t)tick_change_floor_texture_b(rec);
    case 0x3354au: return (uint32_t)tick_change_face_texture_adv(rec);
    case 0x335ecu: return (uint32_t)tick_modify_sector(rec);
    case 0x33a69u: return (uint32_t)cmd_open_door(rec);
    case 0x33ac4u: return (uint32_t)cmd_change_lighting(rec);
    case 0x33b3bu: return (uint32_t)tick_change_lighting(rec);
    case 0x33b94u: return (uint32_t)cmd_light_switch(rec);
    case 0x33be2u: return (uint32_t)tick_light_switch(rec);
    case 0x3540bu: return (uint32_t)run_command_dbase100_record(rec);
    case 0x35437u: return (uint32_t)cmd_give_item(rec);
    case 0x354d3u: return (uint32_t)cmd_remove_item(rec);
    case 0x35544u: return (uint32_t)cmd_if_not_item(rec);
    case 0x355a7u: return (uint32_t)cmd_if_not_flag(rec);
    case 0x35617u: return (uint32_t)cmd_set_flag(rec);
    default: break;
    }
    roth_unreachable(handler_rt ? handler_rt - OBJ_DELTA : 0x30780u);   /* unknown table value (0 = unstaged) */
    return 0;
}

uint32_t rwss_span_callback_dispatch(uint32_t eax_cwde, uint32_t edx_block)
{
    uint32_t hook = (uint32_t)G32(VA_g_span_callback);
    if (hook == 0) return eax_cwde;                                 /* 0x3909d null slot: eax through */
    if (hook == 0x33cf3u + OBJ_DELTA)
        return texture_anim_command_hook(eax_cwde, edx_block);
    roth_unreachable(0x39093u);                                    /* fail-loud on an unknown value */
    return eax_cwde;                                                /* not reached (abort above) */
}

uint32_t rwss_id_remap_dispatch(uint32_t id)
{
    uint32_t hook = (uint32_t)G32(VA_g_pool_check_enabled + 0x28);
    if (hook == 0) return id;                                       /* null slot: id through (caller-shaped) */
    if (hook == 0x33ddeu + OBJ_DELTA)
        return texture_id_remap_hook(id);
    roth_unreachable(0x33ddeu);                                    /* fail-loud on an unknown value */
    return id;                                                      /* not reached (abort above) */
}
#endif /* ROTH_STANDALONE */

/* process_deferred_command_queue (0x3484b): swap the double-buffered deferred-command queue
 * (0x71f40 <-> 0x71f44), clear the new front buffer's count, then drain the (old front) back buffer.
 * Layout: a buffer starts with a dword count, followed by `count` 8-byte entries
 * {dword0 = command index, dword1 = key-or-record-pointer}. For dword1: if its high 16 bits are set it is a
 * host record pointer -> stage g@0x8a0fc = ptr, g@0x8a0f8 = word[ptr+4] + geom_base(0x90aa8), g@0x8a100 = 0.
 * Else it is a small key -> stage g@0x8a0c4 = key, g@0x8a100 = find_secondary_state_record_by_key(key) when
 * nonzero (0 otherwise), g@0x8a0fc = g@0x8a0f8 = 0. Then run the chain from dword0 via
 * reset_command_chain_state. NB the queue pointers are HOST pointers in obj3 (STORED-POINTER rule).
 * Returns the residual EAX: the (old) front-buffer pointer on the empty path, else the last
 * reset_command_chain_state result (-1) — register_command_save_link threads this high half into 0x8a0e8. */
uint32_t process_deferred_command_queue(void)
{
    uint32_t ebx = (uint32_t)G32(VA_g_anim_step_fn_table + 0x10);                  /* back buffer (to drain) */
    uint32_t eax = (uint32_t)G32(VA_g_anim_step_fn_table + 0x14);                  /* the other buffer (becomes front) */
    G32(VA_g_anim_step_fn_table + 0x14) = (int32_t)ebx;                            /* swap the two buffers */
    G32(VA_g_anim_step_fn_table + 0x10) = (int32_t)eax;
    *(uint32_t *)(uintptr_t)eax = 0;                        /* clear new front-buffer count */
    int32_t ecx = *(int32_t *)(uintptr_t)ebx;             /* count of the back buffer */
    if (ecx == 0) return eax;                               /* empty: eax = front-buffer ptr */
    ebx += 4;                                               /* -> first entry */
    G16(VA_g_state_link_buf_ptr + 0x128) = 0;
    do {
        uint32_t key = *(uint32_t *)(uintptr_t)(ebx + 4);  /* entry.dword1 */
        if (key & 0xffff0000u) {                            /* high path: a host record pointer */
            G32(VA_g_command_source_object) = 0;
            G32(VA_g_player_move_delta_z + 0x8) = 0;
            G32(VA_g_active_object + 0x4) = (int32_t)key;
            eax = (uint32_t)(*(uint16_t *)(uintptr_t)(key + 4)) + (uint32_t)G32(VA_g_map_geometry_buffer);
            G32(VA_g_active_object) = (int32_t)eax;
        } else {                                            /* low path: a small key */
            G32(VA_g_item_drop_position + 0x118) = (int32_t)key;
            eax = key ? find_secondary_state_record_by_key((uint16_t)key) : 0;
            G32(VA_g_command_source_object) = (int32_t)eax;
            G32(VA_g_player_move_delta_z + 0x8) = 0;
            G32(VA_g_active_object + 0x4) = 0;
            G32(VA_g_active_object) = 0;                               /* eax was reset to 0 before this store */
        }
        G32(VA_g_item_drop_position + 0x11c) = 0;
        uint32_t cmd = *(uint32_t *)(uintptr_t)ebx;         /* entry.dword0 = command index */
        ebx += 8;
        eax = (uint32_t)reset_command_chain_state((uint16_t)cmd); /* call 0x305b6 (ecx preserved) */
    } while (--ecx > 0);                                    /* dec ecx; jg */
    return eax;
}

/* register_command_save_link (0x31f3c): the trigger's chain-run + save-link latch. Reset+run the chain from
 * AX (reset_command_chain_state ✓), flush the deferred queue (process_deferred_command_queue ✓), then per the
 * record's flag byte[esi+6]: bit 0x40 -> latch g@0x8a0e4=rec, g@0x8a0ec = word[0x90c12] (zero-ext); else bit
 * 0x20 -> g@0x8a13c=0, g@0x8a0e4=rec, g@0x8a0e8 = (residual eax high) | word[0x90c12]. Finally, if the chain
 * produced a result (g@0x8a138 != 0) and the record is interrupt-flagged (bit 0x10), set the rerun bit
 * byte[esi+2] |= 8. Returns -1. Entered with AX = chain start index (word[rec+4]) set by the fire wrappers. */
int32_t register_command_save_link(uint32_t esi, uint16_t ax)
{
    reset_command_chain_state(ax);                  /* edi=esi at entry but unused by the reset */
    uint32_t eax = process_deferred_command_queue();/* residual eax flows into the 0x20 branch */
    G32(VA_g_state_link_word_b) = 0;
    G8(VA_g_command_chain_interrupt + 0x2) = 0;
    uint8_t f = *(volatile uint8_t *)(uintptr_t)(esi + 6);
    if (f & 0x40) {
        G32(VA_g_state_link_obj_ptr) = (int32_t)esi;
        eax = (uint32_t)G16(VA_g_player_sector);                      /* sub eax,eax; mov ax,word[0x90c12] */
        G32(VA_g_state_link_word_b) = (int32_t)eax;
    } else if (f & 0x20) {
        G32(VA_g_state_link_buf_ptr) = 0;
        G32(VA_g_state_link_obj_ptr) = (int32_t)esi;
        eax = (eax & 0xffff0000u) | (uint32_t)G16(VA_g_player_sector);/* mov ax,word[0x90c12]; high bits = residual eax */
        G32(VA_g_state_link_word_a) = (int32_t)eax;
    }
    if (G32(VA_g_player_move_delta_z + 0xc) != 0 && (*(volatile uint8_t *)(uintptr_t)(esi + 6) & 0x10))
        *(volatile uint8_t *)(uintptr_t)(esi + 2) |= 8;
    return -1;
}

/* the gated tail shared by fire_trigger_on_contact / fire_trigger_on_interact (0x31fd0): the flow pre-pass blocked
 * the chain. Clear g@0x8a0ec; if the record is interrupt-flagged (byte[+6]&0x20) latch g@0x8a13c=0, g@0x8a0e4=rec,
 * g@0x8a0e8 = (residual eax high) | word[0x90c12]. Returns 0 (sub eax,eax). */
static int32_t rawcmd_trigger_gated_tail(uint32_t esi, uint32_t eax)
{
    G32(VA_g_state_link_word_b) = 0;
    if (*(volatile uint8_t *)(uintptr_t)(esi + 6) & 0x20) {
        G32(VA_g_state_link_buf_ptr) = 0;
        G32(VA_g_state_link_obj_ptr) = (int32_t)esi;
        eax = (eax & 0xffff0000u) | (uint32_t)G16(VA_g_player_sector);
        G32(VA_g_state_link_word_a) = (int32_t)eax;
    }
    return 0;
}

/* fire_trigger_on_contact (0x31fb0): latch g@0x8a264 = word[rec+0xa], run the flow pre-pass
 * walk_command_chain_flow(word[rec+4]) ✓; if NOT gated (==0) tail-call register_command_save_link(rec, word[rec+4]);
 * else the gated tail with residual eax = the (nonzero) walk result. */
int32_t fire_trigger_on_contact(uint32_t esi)
{
    G16(VA_g_state_link_buf_ptr + 0x128) = *(volatile uint16_t *)(uintptr_t)(esi + 0xa);
    uint16_t ax = *(volatile uint16_t *)(uintptr_t)(esi + 4);
    uint32_t eax = walk_command_chain_flow(ax);
    if (eax == 0)
        return register_command_save_link(esi, *(volatile uint16_t *)(uintptr_t)(esi + 4));
    return rawcmd_trigger_gated_tail(esi, eax);
}

/* fire_trigger_on_interact (0x31ffe): g@0x8a264 = 0; if byte[rec+2]&0x29 -> gated tail (residual eax = the INCOMING
 * eax, set by the caller). Else run the flow pre-pass; if gated -> gated tail (eax = the walk result); else
 * tail-call register_command_save_link(rec, word[rec+4]). Shares the gated tail (0x31fd0) with on_contact. */
int32_t fire_trigger_on_interact(uint32_t esi, uint32_t eax_in)
{
    G16(VA_g_state_link_buf_ptr + 0x128) = 0;
    if (*(volatile uint8_t *)(uintptr_t)(esi + 2) & 0x29)
        return rawcmd_trigger_gated_tail(esi, eax_in);
    uint32_t eax = walk_command_chain_flow(*(volatile uint16_t *)(uintptr_t)(esi + 4));
    if (eax != 0)
        return rawcmd_trigger_gated_tail(esi, eax);
    return register_command_save_link(esi, *(volatile uint16_t *)(uintptr_t)(esi + 4));
}

/* the contact-trigger entry wrappers (0x351c3 / 0x351cd / 0x35201 / 0x3520b): the public entry points that fire a
 * record's command chain on contact. Each saves ES (=DS, a host no-op), seeds the warp sentinel g@0x8a260 and/or
 * the active-chain marker g@0x8a134, clears g@0x8a0dc, then — unless the record is mid-fire (byte[rec+2]&0x29) —
 * calls fire_trigger_on_contact(rec). Multi-entry pairs: the inner entry of each pair skips the 0x8a260 store;
 * the 0x35201/0x3520b pair also omits the 0x8a134 store. Returns fire_trigger_on_contact's result, else 0. */
static int32_t rawcmd_contact_trigger(uint32_t eax, int set_warp, int set_active)
{
    if (set_warp) G32(VA_g_state_link_buf_ptr + 0x124) = (int32_t)0x80008000u;
    G32(VA_g_pending_command_record) = 0;
    uint32_t esi = eax;                                     /* mov esi, eax */
    if (set_active) G32(VA_g_player_move_delta_z + 0x8) = 0;
    int32_t ret = 0;                                        /* sub eax, eax */
    if (!(*(volatile uint8_t *)(uintptr_t)(esi + 2) & 0x29))
        ret = fire_trigger_on_contact(esi);
    return ret;
}
int32_t fire_command_contact_trigger(uint32_t rec)  { return rawcmd_contact_trigger(rec, 1, 1); } /* 0x351c3 */
int32_t exec_object_contact_trigger(uint32_t rec)   { return rawcmd_contact_trigger(rec, 0, 1); } /* 0x351cd */
int32_t exec_object_trigger_no_source(uint32_t rec) { return rawcmd_contact_trigger(rec, 1, 0); } /* 0x35201 */
int32_t exec_object_trigger(uint32_t rec)           { return rawcmd_contact_trigger(rec, 0, 0); } /* 0x3520b */

/* begin_object_command_chain (0x35303): seed the command context from a source record then fire its target's chain.
 * g@0x8a100 = g@0x8a134 = src; g@0x8a0f8 = g@0x8a0fc = 0; latch the warp coords g@0x8a260 = word[src],
 * g@0x8a262 = word[src+2]; resolve the target object via find_object_list24(word[src+0xe]); if found, fire it via
 * exec_object_trigger. Returns the fire result, or 0 when no target. */
int32_t begin_object_command_chain(uint32_t eax)
{
    G32(VA_g_command_source_object) = (int32_t)eax;
    G32(VA_g_player_move_delta_z + 0x8) = (int32_t)eax;
    G32(VA_g_active_object) = 0;
    G32(VA_g_active_object + 0x4) = 0;
    G16(VA_g_state_link_buf_ptr + 0x124) = *(volatile uint16_t *)(uintptr_t)(eax);
    G16(VA_g_state_link_buf_ptr + 0x126) = *(volatile uint16_t *)(uintptr_t)(eax + 2);
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(eax + 0xe);
    uint32_t r = find_object_list24(key);
    if (r == 0) return 0;
    return exec_object_trigger(r);
}

/* run_object_commands_by_id (0x30549): scan the object table (g@0x85c30) for the first non-skip record whose id
 * (word[+8]) matches AX; if found, run its command chain via reset_command_chain_no_source(word[rec+4]) bracketed
 * by g@0x8a26c = 1/0. Returns reset_command_chain_no_source's result (-1) on a match, else 0. Table layout:
 * [+8]=cursor offset, [+0xa]=count; each cursor slot is a word offset to a record within the table. */
int32_t run_object_commands_by_id(uint16_t ax)
{
    uint32_t ebx = (uint32_t)G32(VA_g_object_table_header);
    if (ebx == 0) return 0;
    uint32_t ecx = *(volatile uint16_t *)(uintptr_t)(ebx + 0xa);     /* count */
    if (ecx == 0) return 0;
    uint32_t esi = *(volatile uint16_t *)(uintptr_t)(ebx + 8);       /* cursor offset */
    for (;;) {
        uint32_t edi = *(volatile uint16_t *)(uintptr_t)(ebx + esi); /* record offset (zero-ext) */
        if (!(*(volatile uint8_t *)(uintptr_t)(edi + ebx + 2) & 8) &&
            *(volatile uint16_t *)(uintptr_t)(edi + ebx + 8) == ax) {
            G8(VA_g_command_chain_interrupt + 0x4) = 1;
            uint16_t cmd = *(volatile uint16_t *)(uintptr_t)(edi + ebx + 4);
            int32_t r = reset_command_chain_no_source(cmd);
            G8(VA_g_command_chain_interrupt + 0x4) = 0;
            return r;
        }
        esi += 2;
        if (!(--ecx > 0)) break;                                     /* dec ecx; jg */
    }
    return 0;
}

/* fire_wall_object_trigger (0x3534b): fire a wall's command chain, latching the warp coords to the midpoint of the
 * wall's two vertices. Stage g@0x8a0dc=0, g@0x8a0f8=rec, g@0x8a0fc=vtxpair, g@0x8a100=0; the vertex records are
 * sector_geom(0x90aac) + word[vtxpair] / word[vtxpair+2], midpoint of their x (+8) / y (+0xa) -> g@0x8a260/0x8a262.
 * Resolve the object via find_object_list20(word[rec+0xc]); if found, fire via exec_object_contact_trigger. */
int32_t fire_wall_object_trigger(uint32_t eax, uint32_t edx)
{
    G32(VA_g_pending_command_record) = 0;
    G32(VA_g_active_object) = (int32_t)eax;
    G32(VA_g_active_object + 0x4) = (int32_t)edx;
    G32(VA_g_command_source_object) = 0;
    uint32_t sgeom = (uint32_t)G32(VA_g_sector_geom_base);
    uint32_t edi = (uint32_t)(*(volatile uint16_t *)(uintptr_t)(edx))     + sgeom;   /* vtxA record */
    uint32_t esi = (uint32_t)(*(volatile uint16_t *)(uintptr_t)(edx + 2)) + sgeom;   /* vtxB record */
    int32_t ax8 = *(volatile int16_t *)(uintptr_t)(edi + 8);
    int32_t bx8 = *(volatile int16_t *)(uintptr_t)(esi + 8);
    G16(VA_g_state_link_buf_ptr + 0x124) = (uint16_t)(int16_t)(((ax8 - bx8) >> 1) + bx8);
    int32_t axa = *(volatile int16_t *)(uintptr_t)(edi + 0xa);
    int32_t bxa = *(volatile int16_t *)(uintptr_t)(esi + 0xa);
    G16(VA_g_state_link_buf_ptr + 0x126) = (uint16_t)(int16_t)(((axa - bxa) >> 1) + bxa);
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(eax + 0xc);
    uint32_t r = find_object_list20(key);
    if (r == 0) return 0;
    return exec_object_contact_trigger(r);
}

/* tick_change_lighting (0x33b3b): step a record's lighting fade. If the countdown byte[rec+6] (low) is nonzero,
 * advance by min(byte, clamp([0x85324] to 0x40)), store the remainder, and apply that delta (negated when the
 * direction byte ah = byte[rec+7] is set) to the record-list at rec+0xc via apply_light_delta_to_record_list.
 * When the countdown reaches 0, finalize on the registration record edi=[rec+8]: if byte[edi+6]&0x10 set
 * byte[edi+2]|=8, then clear byte[edi+2]&=0xde, and return -1. Returns 0 while still fading. */
int32_t tick_change_lighting(uint32_t eax)
{
    uint32_t esi = eax;
    uint32_t reg = *(volatile uint32_t *)(uintptr_t)(esi + 8);      /* edi = [rec+8] registration record */
    uint16_t w6 = *(volatile uint16_t *)(uintptr_t)(esi + 6);
    uint8_t al = (uint8_t)w6;
    uint8_t ah = (uint8_t)(w6 >> 8);
    if (al == 0) {
        if (*(volatile uint8_t *)(uintptr_t)(reg + 6) & 0x10)
            *(volatile uint8_t *)(uintptr_t)(reg + 2) |= 8;
        *(volatile uint8_t *)(uintptr_t)(reg + 2) &= 0xde;
        return -1;
    }
    uint32_t edx = (uint32_t)G32(VA_g_frame_time_scale);
    if (edx >= 0x40) edx = 0x40;
    if (al <= (uint8_t)edx) edx = al;                              /* cmp al,dl; ja keeps dl else dl=al */
    al = (uint8_t)(al - (uint8_t)edx);
    *(volatile uint8_t *)(uintptr_t)(esi + 6) = al;
    if (ah != 0) edx = (uint32_t)((-(int32_t)(uint8_t)edx) & 0xff);/* neg dl (8-bit; high bytes stay 0) */
    apply_light_delta_to_record_list(esi + 0xc, edx);
    return 0;
}

/* tick_light_switch (0x33be2): step a light-switch toward its target. On the registration record edi=[rec+8],
 * clamp the signed delta byte[edi+0xc] to +/-[0x85324], add the step, and push that delta through the record-list
 * at rec+0xc via apply_light_delta_to_record_list. When the switch reaches 0 and is finalize-flagged
 * (byte[edi+6]&0x80), apply the on/off flag mask (apply_flag_mask_to_record_list, mask order keyed off the delta
 * sign), clear byte[edi+2]&=0xde and return -1. While still stepping, return 0. */
int32_t tick_light_switch(uint32_t eax)
{
    uint32_t esi = eax;
    uint32_t edi = *(volatile uint32_t *)(uintptr_t)(esi + 8);
    int32_t v = *(volatile int8_t *)(uintptr_t)(edi + 0xc);          /* movsx eax, byte[edi+0xc] */
    int32_t edx = (int32_t)G32(VA_g_frame_time_scale);
    if (!(v < edx)) v = edx;                                         /* cmp/jl: clamp magnitude to step */
    v = -v;                                                          /* neg */
    if (!(v > edx)) edx = v;
    uint8_t dl = (uint8_t)edx;
    *(volatile uint8_t *)(uintptr_t)(edi + 0xc) =
        (uint8_t)(*(volatile uint8_t *)(uintptr_t)(edi + 0xc) + dl); /* byte[edi+0xc] += dl */
    apply_light_delta_to_record_list(esi + 0xc, (uint32_t)edx);
    uint32_t eax_after = (uint32_t)edx;                             /* pop eax = the pushed edx (delta) */
    if (*(volatile uint8_t *)(uintptr_t)(edi + 0xc) != 0)
        return 0;                                                   /* still stepping */
    if (*(volatile uint8_t *)(uintptr_t)(edi + 6) & 0x80) {
        uint32_t edxm = 2, ebxm = 0;                                /* edx=2, ebx=0 */
        if (!((uint8_t)eax_after & 0x80)) { edxm = 0; ebxm = 2; }   /* or al,al; js skips the xchg */
        apply_flag_mask_to_record_list(esi + 0xc, ebxm, edxm);
    }
    *(volatile uint8_t *)(uintptr_t)(edi + 2) &= 0xde;
    return -1;
}

/* tick_command_timer_queue (0x345e2): step the fixed timer queue at g@0x89fc0 (ECX=AX entries of 8 bytes each:
 * [+0]=countdown, [+4]=record ptr). For each entry whose record isn't skip-flagged (byte[rec+2]&8), subtract the
 * frame step [0x85324]; if the countdown goes negative the timer fires — zero the command staging globals, set the
 * warp sentinel, and run fire_queued_command(record, slot=entry) (reloading the step after, since the call clobbers
 * EDX). EAX threads the input count unless a fire overwrites it with fire_queued_command's result. */
int32_t tick_command_timer_queue(uint32_t eax)
{
    int32_t ecx = (int32_t)eax;                                     /* mov ecx, eax */
    uint32_t ebx = (uint32_t)GADDR(VA_g_item_drop_position + 0x14);                        /* timer queue base */
    int32_t edx = (int32_t)G32(VA_g_frame_time_scale);
    for (;;) {
        uint32_t esi = *(volatile uint32_t *)(uintptr_t)(ebx + 4);  /* record ptr */
        if (!(*(volatile uint8_t *)(uintptr_t)(esi + 2) & 8)) {
            *(volatile int32_t *)(uintptr_t)ebx -= edx;             /* countdown -= step */
            if (*(volatile int32_t *)(uintptr_t)ebx < 0) {          /* jns skips when >= 0 */
                G32(VA_g_player_move_delta_z + 0x8) = 0; G32(VA_g_command_source_object) = 0; G32(VA_g_active_object) = 0; G32(VA_g_active_object + 0x4) = 0;
                G32(VA_g_state_link_buf_ptr + 0x124) = (int32_t)0x80008000u;
                eax = (uint32_t)fire_queued_command(esi, ebx);
                edx = (int32_t)G32(VA_g_frame_time_scale);                        /* reload step */
            }
        }
        ebx += 8;
        if (!(--ecx > 0)) break;                                    /* dec ecx; jg */
    }
    return (int32_t)eax;
}

/* tick_cmd_45 (0x339ff): one-shot connected-geometry light/flag pulse. Gated on g@0x89f5c == 0 and the record's
 * arm bit byte[rec+6]&0x20 (consumed). Flood-fill the connected sector group from word[rec+0xa] into a scratch
 * list (collect_connected_geometry_group, cap 0xc8); if non-empty apply the brightness delta byte[rec+7]
 * (apply_light_delta_to_record_list), toggle byte[rec+6]^=8 / byte[rec+2]^=2, and when byte[rec+6]&0x80 also
 * apply the on/off flag mask (apply_flag_mask_to_record_list, order keyed off byte[rec+7]&0x80). Always clears
 * byte[rec+2]&=0xde. Returns the incoming EAX on the gated early-outs, else the last apply call's result (or the
 * group count when empty). NB the residual EDX high bytes into apply_light_delta are unobserved (it uses BL). */
int32_t tick_cmd_45(uint32_t esi, uint32_t eax_in)
{
    if (G32(VA_g_state_record_list_count) != 0) return (int32_t)eax_in;
    *(volatile uint8_t *)(uintptr_t)(esi + 0xc) = 0;
    if (!(*(volatile uint8_t *)(uintptr_t)(esi + 6) & 0x20)) return (int32_t)eax_in;
    *(volatile uint8_t *)(uintptr_t)(esi + 6) -= 0x20;
    uint8_t scratch[0x190];
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(esi + 0xa);
    int32_t count = collect_connected_geometry_group(key, 0xc8, (uint32_t)(uintptr_t)scratch);
    uint32_t eax = (uint32_t)count;
    if (count != 0) {
        uint8_t dl = *(volatile uint8_t *)(uintptr_t)(esi + 7);
        eax = (uint32_t)apply_light_delta_to_record_list((uint32_t)(uintptr_t)scratch, dl);
        *(volatile uint8_t *)(uintptr_t)(esi + 6) ^= 8;
        *(volatile uint8_t *)(uintptr_t)(esi + 2) ^= 2;
        if (*(volatile uint8_t *)(uintptr_t)(esi + 6) & 0x80) {
            uint32_t edxm = 2, ebxm = 0;
            if (!(*(volatile uint8_t *)(uintptr_t)(esi + 7) & 0x80)) { edxm = 0; ebxm = 2; }  /* jne skips xchg */
            eax = (uint32_t)apply_flag_mask_to_record_list((uint32_t)(uintptr_t)scratch, ebxm, edxm);
        }
    }
    *(volatile uint8_t *)(uintptr_t)(esi + 2) &= 0xde;
    return (int32_t)eax;
}

/* apply_object_state_to_group (0x328aa): propagate the first group member's texture/flag state to a whole object
 * group and back into the record. EDI=record, ESI=group descriptor ([+0x10]=count, words at [+0x14...] index the
 * object table 0x90aa4). Unless the record is "frozen" (byte[edi+6]&8 -> EAX sign flag set), copy member[0]'s
 * texture word[obj+4] into byte/word[edi+0xc] and its masked flag (byte[obj+7]&0x10) into byte[edi+7]. Then for
 * every member write the record's texture word[edi+0xc] into word[obj+4], clear byte[edi+7]&=0xef, and OR
 * member[0]'s low-flag (byte[obj0+7]&0xef) into byte[obj+7]. Returns the composite EAX the asm leaves
 * (skip-flag high | (orig byte[edi+7]&0x10)<<8 | member0 byte[+7]&0xef). */
int32_t apply_object_state_to_group(uint32_t edi, uint32_t esi)
{
    int skip = (*(volatile uint8_t *)(uintptr_t)(edi + 6) & 8) != 0;     /* sub eax,eax; test; dec -> 0 or -1 */
    uint32_t ebx = esi + 0x10;
    uint32_t ecx = *(volatile uint32_t *)(uintptr_t)ebx & 0xffffu;       /* count */
    ebx += 4;                                                            /* -> first member word */
    uint32_t objects = (uint32_t)G32(VA_g_map_objects_buffer);
    uint32_t edx0 = objects + *(volatile uint16_t *)(uintptr_t)ebx;      /* member[0] object record */
    uint16_t w4 = *(volatile uint16_t *)(uintptr_t)(edx0 + 4);
    uint16_t bp = *(volatile uint16_t *)(uintptr_t)(edi + 0xc);
    uint8_t edi7_orig = *(volatile uint8_t *)(uintptr_t)(edi + 7);
    uint8_t edx0_7 = *(volatile uint8_t *)(uintptr_t)(edx0 + 7);
    if (!skip) {                                                         /* js gates these stores */
        *(volatile uint16_t *)(uintptr_t)(edi + 0xc) = w4;
        *(volatile uint8_t *)(uintptr_t)(edi + 7) = (uint8_t)(edx0_7 & 0x10);
    }
    uint8_t al = (uint8_t)(edx0_7 & 0xef);                               /* propagated low-flag */
    int32_t n = (int32_t)ecx;
    do {
        uint32_t edx = objects + *(volatile uint16_t *)(uintptr_t)ebx;
        *(volatile uint16_t *)(uintptr_t)(edx + 4) = bp;
        *(volatile uint8_t *)(uintptr_t)(edi + 7) &= 0xef;
        *(volatile uint8_t *)(uintptr_t)(edx + 7) |= al;
        ebx += 2;
    } while (--n > 0);
    uint32_t eax = (skip ? 0xffff0000u : 0u)
                 | ((uint32_t)(edi7_orig & 0x10) << 8)
                 | (uint32_t)(edx0_7 & 0xef);
    return (int32_t)eax;
}

/* tick_scroll_face_texture (0x32592): advance a face-group's UV scroll by one frame. The scroll-rate record
 * edi=[rec+8] holds signed per-axis rates (byte[+7]=u, byte[+8]=v). Each frame du = (rate_u*step + carry) >> 1
 * with the carry-out (CF of the shift = bit0) latched back into byte[rec+6] (u) / byte[rec+7] (v); step =
 * [0x85324]. Then for each of count=word[rec+0xc] faces (2-byte offsets from rec+0x10), add the du/dv low bytes
 * to geom[face].u (+0xa) / .v (+0xb). Self-contained (geom 0x90aa8). NB the loop is do-while (count==0 still
 * processes one face — faithful). Returns 0. */
int32_t tick_scroll_face_texture(uint32_t rec)
{
    uint32_t esi = rec;
    uint32_t edi = *(volatile uint32_t *)(uintptr_t)(esi + 8);
    int32_t step = (int32_t)G32(VA_g_frame_time_scale);
    uint32_t cu = *(volatile uint8_t *)(uintptr_t)(esi + 6) & 1u;
    int32_t prod_u = (int32_t)(*(volatile int8_t *)(uintptr_t)(edi + 7)) * step + (int32_t)cu;
    uint32_t du = (uint32_t)prod_u >> 1;
    *(volatile uint8_t *)(uintptr_t)(esi + 6) = (uint8_t)((uint32_t)prod_u & 1u);   /* shr CF = bit0 */
    uint32_t cv = *(volatile uint8_t *)(uintptr_t)(esi + 7) & 1u;
    int32_t prod_v = step * (int32_t)(*(volatile int8_t *)(uintptr_t)(edi + 8)) + (int32_t)cv;
    uint32_t dv = (uint32_t)prod_v >> 1;
    *(volatile uint8_t *)(uintptr_t)(esi + 7) = (uint8_t)((uint32_t)prod_v & 1u);
    uint8_t al = (uint8_t)du, ah = (uint8_t)dv;
    uint32_t p = esi + 0xc;
    int32_t count = *(volatile uint16_t *)(uintptr_t)p;
    p += 4;                                                          /* skip count(+key) word -> faces @rec+0x10 */
    uint32_t geom = (uint32_t)G32(VA_g_map_geometry_buffer);
    do {
        uint32_t face = geom + *(volatile uint16_t *)(uintptr_t)p;
        p += 2;
        *(volatile uint8_t *)(uintptr_t)(face + 0xa) += al;
        *(volatile uint8_t *)(uintptr_t)(face + 0xb) += ah;
    } while (--count > 0);
    return 0;
}

/* tick_scroll_sector_texture (0x324d2): the sector twin of tick_scroll_face_texture — same per-frame du/dv carry
 * accumulator, but each of count=word[rec+0xc] members applies the scroll to selected texture slots per the
 * scroll record's direction flags byte[edi+6]: bit0 -> face[+0x12/+0x13], bit1 -> face[+0x10/+0x11], then via the
 * sub-record geom+word[face+0x18]: bit2 -> sub[+0xa/+0xb], bit3 -> sub[+4/+5]. Each handled bit is subtracted and
 * the dispatch early-outs once the flags are exhausted. Self-contained (geom 0x90aa8, step 0x85324). Returns 0. */
int32_t tick_scroll_sector_texture(uint32_t rec)
{
    uint32_t esi = rec;
    uint32_t edi = *(volatile uint32_t *)(uintptr_t)(esi + 8);
    int32_t step = (int32_t)G32(VA_g_frame_time_scale);
    uint32_t cu = *(volatile uint8_t *)(uintptr_t)(esi + 6) & 1u;
    int32_t prod_u = (int32_t)(*(volatile int8_t *)(uintptr_t)(edi + 7)) * step + (int32_t)cu;
    uint32_t du = (uint32_t)prod_u >> 1;
    *(volatile uint8_t *)(uintptr_t)(esi + 6) = (uint8_t)((uint32_t)prod_u & 1u);
    uint32_t cv = *(volatile uint8_t *)(uintptr_t)(esi + 7) & 1u;
    int32_t prod_v = step * (int32_t)(*(volatile int8_t *)(uintptr_t)(edi + 8)) + (int32_t)cv;
    uint32_t dv = (uint32_t)prod_v >> 1;
    *(volatile uint8_t *)(uintptr_t)(esi + 7) = (uint8_t)((uint32_t)prod_v & 1u);
    uint8_t al = (uint8_t)du, ah = (uint8_t)dv;
    uint32_t p = esi + 0xc;
    int32_t count = *(volatile uint16_t *)(uintptr_t)p;
    p += 4;
    uint32_t geom = (uint32_t)G32(VA_g_map_geometry_buffer);
    do {
        uint32_t face = geom + *(volatile uint16_t *)(uintptr_t)p;
        p += 2;
        uint8_t cl = *(volatile uint8_t *)(uintptr_t)(edi + 6);
        for (;;) {                                                  /* the flag-dispatch with early-outs */
            if (cl & 1) {
                *(volatile uint8_t *)(uintptr_t)(face + 0x12) += al;
                *(volatile uint8_t *)(uintptr_t)(face + 0x13) += ah;
                cl -= 1; if (cl == 0) break;
            }
            if (cl & 2) {
                *(volatile uint8_t *)(uintptr_t)(face + 0x10) += al;
                *(volatile uint8_t *)(uintptr_t)(face + 0x11) += ah;
                cl -= 2; if (cl == 0) break;
            }
            uint32_t sub = *(volatile uint16_t *)(uintptr_t)(face + 0x18);
            if (sub == 0) break;
            sub += geom;
            if (cl & 4) {
                *(volatile uint8_t *)(uintptr_t)(sub + 0xa) += al;
                *(volatile uint8_t *)(uintptr_t)(sub + 0xb) += ah;
                cl -= 4; if (cl == 0) break;
            }
            if (cl & 8) {
                *(volatile uint8_t *)(uintptr_t)(sub + 4) += al;
                *(volatile uint8_t *)(uintptr_t)(sub + 5) += ah;
            }
            break;
        }
    } while (--count > 0);
    return 0;
}

/* The two tails shared by the texture/cell ticks (tick_change_floor_texture / _b / _face_adv / _object_texture).
 * rawcmd_texture_countdown (0x3305f -> 0x32ec8): the byte[rec+5]&0x40 "armed" path — decrement word[rec+6] by the
 * frame step (16-bit), and once it is no longer positive clear the arm bit (byte[rec+5]&=0xbf). Returns 0. */
static int32_t rawcmd_texture_countdown(uint32_t esi, int32_t step)
{
    uint16_t step16 = (uint16_t)step;
    uint16_t old = *(volatile uint16_t *)(uintptr_t)(esi + 6);
    *(volatile uint16_t *)(uintptr_t)(esi + 6) = (uint16_t)(old - step16);
    if (!((int16_t)old > (int16_t)step16))                          /* jg (signed) not taken -> clear */
        *(volatile uint8_t *)(uintptr_t)(esi + 5) &= 0xbf;
    return 0;
}

/* rawcmd_texture_tick_finalize (0x3300c): the post-swap state machine. EAX = (byte[edi+6]&0x20 ? 1 : 0) + (EDX ? 2
 * : 0) selects: 0 -> set byte[edi+2]=(.&0xde)|8, ret -1; 1 -> byte[edi+2]&=0xde then ^=2, ret -1; 2 with
 * byte[esi+5]&0x20 -> byte[edi+2]&=0xde, ret -1; otherwise (eax 3, or 2 with the bit clear) -> byte[esi+5]^=0xe0,
 * word[esi+6]=dx, ret 0. (esi = command record, edi = its [+8] cell/sub-record, edx = the post-swap latch word.) */
static int32_t rawcmd_texture_tick_finalize(uint32_t esi, uint32_t edi, uint32_t edx)
{
    uint32_t eax = (*(volatile uint8_t *)(uintptr_t)(edi + 6) & 0x20) ? 1u : 0u;
    if (edx != 0) eax += 2;
    if (eax == 0) {
        *(volatile uint8_t *)(uintptr_t)(edi + 2) =
            (uint8_t)((*(volatile uint8_t *)(uintptr_t)(edi + 2) & 0xde) | 8);
        return -1;
    }
    if (eax == 1) {
        *(volatile uint8_t *)(uintptr_t)(edi + 2) &= 0xde;
        *(volatile uint8_t *)(uintptr_t)(edi + 2) ^= 2;
        return -1;
    }
    if (eax == 2 && (*(volatile uint8_t *)(uintptr_t)(esi + 5) & 0x20)) {
        *(volatile uint8_t *)(uintptr_t)(edi + 2) &= 0xde;
        return -1;
    }
    *(volatile uint8_t *)(uintptr_t)(esi + 5) ^= 0xe0;             /* 0x33037 */
    *(volatile uint16_t *)(uintptr_t)(esi + 6) = (uint16_t)edx;
    return 0;
}

/* tick_change_face_texture_adv (0x3354a): per-frame tick that swaps a cell's linked-pair texture state then runs
 * the shared finalize. The armed path (byte[rec+5]&0x40) just counts the timer down; otherwise swap via
 * swap_cell_state_linked_pair (edi=[rec+8] cell) and finalize with the latch word[cell+0xe]. */
int32_t tick_change_face_texture_adv(uint32_t rec)
{
    uint32_t esi = rec;
    uint32_t edi = *(volatile uint32_t *)(uintptr_t)(esi + 8);
    int32_t step = (int32_t)G32(VA_g_frame_time_scale);
    if (*(volatile uint8_t *)(uintptr_t)(esi + 5) & 0x40)
        return rawcmd_texture_countdown(esi, step);
    swap_cell_state_linked_pair(edi);                        /* call 0x33571 (edi = cell/sub-record) */
    uint32_t edx = *(volatile uint16_t *)(uintptr_t)(edi + 0xe);
    return rawcmd_texture_tick_finalize(esi, edi, edx);
}

/* tick_change_object_texture (0x3286b): per-frame tick for an object-texture group. If the object table generation
 * [0x911c7] changed since the cached stamp word[rec+0xc], re-resolve the group (collect_secondary_matches_into_
 * struct, eax=rec+0x10). Then the armed path (byte[rec+5]&0x40) counts down; otherwise propagate the group state
 * (apply_object_state_to_group with edi=[rec+8] cell, esi=rec descriptor) and finalize with latch word[cell+0xa].
 * Composes the already-lifted 0x33072 + 0x328aa + the shared texture tails. */
int32_t tick_change_object_texture(uint32_t rec)
{
    uint32_t esi = rec;
    uint32_t edi = *(volatile uint32_t *)(uintptr_t)(esi + 8);
    uint16_t gen = (uint16_t)(uint32_t)G32(VA_g_object_table_generation);
    if (gen != *(volatile uint16_t *)(uintptr_t)(esi + 0xc)) {
        *(volatile uint16_t *)(uintptr_t)(esi + 0xc) = gen;
        collect_secondary_matches_into_struct(esi + 0x10);  /* call 0x33072 */
    }
    int32_t step = (int32_t)G32(VA_g_frame_time_scale);
    if (*(volatile uint8_t *)(uintptr_t)(esi + 5) & 0x40)
        return rawcmd_texture_countdown(esi, step);
    apply_object_state_to_group(edi, esi);                  /* call 0x328aa (edi=cell, esi=descriptor) */
    uint32_t edx = *(volatile uint16_t *)(uintptr_t)(edi + 0xa);
    return rawcmd_texture_tick_finalize(esi, edi, edx);
}

/* swap_cell_state_group_v1 (0x33255): swap a cell record's texture/flag state with the first member of a cell
 * group, then broadcast that member's new state to the rest of the group. EDI=cellrec, EBP=descriptor (count at
 * [ebp], member offsets at [ebp+4], [ebp+6...]). "frozen" = byte[cellrec+6]&8 suppresses the read-back into the
 * cellrec. Two layouts keyed on byte[cellrec+6]&4: the MAIN path writes the member directly at +8/+0x12 (flag-pack
 * +0xa/+0x17); the ALT path indirects through the member's sub-record (geom+word[member+0x18]) at +6/+0xa
 * (flag-pack +0xc/+0xd). Flag-pack: target lo &=0xcf |= (cellrec[7]&3)<<4 ; hi &=0xfc |= (cellrec[7]&0xc)>>2 ; and
 * (unless frozen) cellrec[7] = ((old_lo&0x30)>>4) | ((old_hi&3)<<2). EAX is dead (the caller recomputes). */
void swap_cell_state_group_v1(uint32_t edi, uint32_t ebp)
{
    int frozen = (*(volatile uint8_t *)(uintptr_t)(edi + 6) & 8) != 0;
    int32_t esi = *(volatile uint16_t *)(uintptr_t)ebp;             /* count */
    if (esi == 0) return;
    uint32_t geom = (uint32_t)G32(VA_g_map_geometry_buffer);
    uint32_t cell0 = geom + *(volatile uint16_t *)(uintptr_t)(ebp + 4);
    ebp += 6;                                                       /* -> member[1] */
    if (!(*(volatile uint8_t *)(uintptr_t)(edi + 6) & 4)) {
        /* MAIN path: member state at +8/+0x12, flag-pack at +0xa/+0x17 */
        uint16_t ax = *(volatile uint16_t *)(uintptr_t)(edi + 0xa);
        uint16_t dx = *(volatile uint16_t *)(uintptr_t)(edi + 0xc);
        if (!frozen) {
            *(volatile uint16_t *)(uintptr_t)(edi + 0xa) = *(volatile uint16_t *)(uintptr_t)(cell0 + 8);
            *(volatile uint16_t *)(uintptr_t)(edi + 0xc) = *(volatile uint16_t *)(uintptr_t)(cell0 + 0x12);
        }
        *(volatile uint16_t *)(uintptr_t)(cell0 + 8) = ax;
        *(volatile uint16_t *)(uintptr_t)(cell0 + 0x12) = dx;
        uint8_t al = *(volatile uint8_t *)(uintptr_t)(cell0 + 0xa), oal = al;
        uint8_t ah = *(volatile uint8_t *)(uintptr_t)(cell0 + 0x17), oah = ah;
        uint8_t cl = *(volatile uint8_t *)(uintptr_t)(edi + 7);
        al = (uint8_t)((al & 0xcf) | ((cl & 3) << 4));
        ah = (uint8_t)((ah & 0xfc) | ((cl & 0xc) >> 2));
        *(volatile uint8_t *)(uintptr_t)(cell0 + 0xa) = al;
        *(volatile uint8_t *)(uintptr_t)(cell0 + 0x17) = ah;
        if (!frozen)
            *(volatile uint8_t *)(uintptr_t)(edi + 7) = (uint8_t)(((oal & 0x30) >> 4) | ((oah & 3) << 2));
        if (--esi <= 0) return;
        uint16_t cx = *(volatile uint16_t *)(uintptr_t)(cell0 + 8);
        uint16_t dx2 = *(volatile uint16_t *)(uintptr_t)(cell0 + 0x12);
        do {
            uint32_t celln = geom + *(volatile uint16_t *)(uintptr_t)ebp; ebp += 2;
            *(volatile uint16_t *)(uintptr_t)(celln + 8) = cx;
            *(volatile uint16_t *)(uintptr_t)(celln + 0x12) = dx2;
        } while (--esi > 0);
        return;
    }
    /* ALT path (byte[cellrec+6]&4): indirect through each member's sub-record (geom+word[member+0x18]) */
    uint32_t sub0w = *(volatile uint16_t *)(uintptr_t)(cell0 + 0x18);
    if (sub0w == 0) return;
    uint32_t sub0 = geom + sub0w;
    uint16_t ax = *(volatile uint16_t *)(uintptr_t)(edi + 0xa);
    uint16_t dx = *(volatile uint16_t *)(uintptr_t)(edi + 0xc);
    if (!frozen) {
        *(volatile uint16_t *)(uintptr_t)(edi + 0xa) = *(volatile uint16_t *)(uintptr_t)(sub0 + 6);
        *(volatile uint16_t *)(uintptr_t)(edi + 0xc) = *(volatile uint16_t *)(uintptr_t)(sub0 + 0xa);
    }
    *(volatile uint16_t *)(uintptr_t)(sub0 + 6) = ax;
    *(volatile uint16_t *)(uintptr_t)(sub0 + 0xa) = dx;
    uint8_t al = *(volatile uint8_t *)(uintptr_t)(sub0 + 0xc), oal = al;
    uint8_t ah = *(volatile uint8_t *)(uintptr_t)(sub0 + 0xd), oah = ah;
    uint8_t cl = *(volatile uint8_t *)(uintptr_t)(edi + 7);
    al = (uint8_t)((al & 0xcf) | ((cl & 3) << 4));
    ah = (uint8_t)((ah & 0xfc) | ((cl & 0xc) >> 2));
    *(volatile uint8_t *)(uintptr_t)(sub0 + 0xc) = al;
    *(volatile uint8_t *)(uintptr_t)(sub0 + 0xd) = ah;
    if (!frozen)
        *(volatile uint8_t *)(uintptr_t)(edi + 7) = (uint8_t)(((oal & 0x30) >> 4) | ((oah & 3) << 2));
    if (--esi <= 0) return;
    uint16_t cx = *(volatile uint16_t *)(uintptr_t)(sub0 + 6);
    uint16_t dx2 = *(volatile uint16_t *)(uintptr_t)(sub0 + 0xa);
    do {
        uint32_t celln = geom + *(volatile uint16_t *)(uintptr_t)ebp; ebp += 2;
        uint32_t subnw = *(volatile uint16_t *)(uintptr_t)(celln + 0x18);
        if (subnw == 0) return;
        uint32_t subn = geom + subnw;
        *(volatile uint16_t *)(uintptr_t)(subn + 6) = cx;
        *(volatile uint16_t *)(uintptr_t)(subn + 0xa) = dx2;
    } while (--esi > 0);
}

/* swap_cell_state_group_v2 (0x333ec): the second cell-group swap variant. Same shape as v1 but different member
 * field offsets and flag-pack masks. MAIN path (byte[cellrec+6]&4 == 0): member state at +6/+0x10, flag-pack at
 * +0xa/+0x17 with mask 0xf3 and (cl&3)<<2 / (cl&0xc). ALT path: indirect through geom+word[member+0x18] at +0/+4,
 * flag-pack at +0xc/+0xd. cellrec[7] read-back (unless frozen) = ((old_lo&0xc)>>2) | (old_hi&0xc). EAX dead. */
void swap_cell_state_group_v2(uint32_t edi, uint32_t ebp)
{
    int frozen = (*(volatile uint8_t *)(uintptr_t)(edi + 6) & 8) != 0;
    int32_t esi = *(volatile uint16_t *)(uintptr_t)ebp;             /* count */
    if (esi == 0) return;
    uint32_t geom = (uint32_t)G32(VA_g_map_geometry_buffer);
    ebp += 4;
    uint32_t cell0 = geom + *(volatile uint16_t *)(uintptr_t)ebp;
    ebp += 2;
    uint8_t cl = *(volatile uint8_t *)(uintptr_t)(edi + 7);
    if (!(*(volatile uint8_t *)(uintptr_t)(edi + 6) & 4)) {
        uint16_t ax = *(volatile uint16_t *)(uintptr_t)(edi + 0xa);
        uint16_t dx = *(volatile uint16_t *)(uintptr_t)(edi + 0xc);
        if (!frozen) {
            *(volatile uint16_t *)(uintptr_t)(edi + 0xa) = *(volatile uint16_t *)(uintptr_t)(cell0 + 6);
            *(volatile uint16_t *)(uintptr_t)(edi + 0xc) = *(volatile uint16_t *)(uintptr_t)(cell0 + 0x10);
        }
        *(volatile uint16_t *)(uintptr_t)(cell0 + 6) = ax;
        *(volatile uint16_t *)(uintptr_t)(cell0 + 0x10) = dx;
        uint8_t al = *(volatile uint8_t *)(uintptr_t)(cell0 + 0xa), oal = al;
        uint8_t ah = *(volatile uint8_t *)(uintptr_t)(cell0 + 0x17), oah = ah;
        al = (uint8_t)((al & 0xf3) | ((cl & 3) << 2));
        ah = (uint8_t)((ah & 0xf3) | (cl & 0xc));
        *(volatile uint8_t *)(uintptr_t)(cell0 + 0xa) = al;
        *(volatile uint8_t *)(uintptr_t)(cell0 + 0x17) = ah;
        if (!frozen)
            *(volatile uint8_t *)(uintptr_t)(edi + 7) = (uint8_t)(((oal & 0xc) >> 2) | (oah & 0xc));
        if (--esi <= 0) return;
        uint16_t cx = *(volatile uint16_t *)(uintptr_t)(cell0 + 6);
        uint16_t dx2 = *(volatile uint16_t *)(uintptr_t)(cell0 + 0x10);
        do {
            uint32_t celln = geom + *(volatile uint16_t *)(uintptr_t)ebp; ebp += 2;
            *(volatile uint16_t *)(uintptr_t)(celln + 6) = cx;
            *(volatile uint16_t *)(uintptr_t)(celln + 0x10) = dx2;
        } while (--esi > 0);
        return;
    }
    uint32_t sub0w = *(volatile uint16_t *)(uintptr_t)(cell0 + 0x18);
    if (sub0w == 0) return;
    uint32_t sub0 = geom + sub0w;
    uint16_t ax = *(volatile uint16_t *)(uintptr_t)(edi + 0xa);
    uint16_t dx = *(volatile uint16_t *)(uintptr_t)(edi + 0xc);
    if (!frozen) {
        *(volatile uint16_t *)(uintptr_t)(edi + 0xa) = *(volatile uint16_t *)(uintptr_t)(sub0 + 0);
        *(volatile uint16_t *)(uintptr_t)(edi + 0xc) = *(volatile uint16_t *)(uintptr_t)(sub0 + 4);
    }
    *(volatile uint16_t *)(uintptr_t)(sub0 + 0) = ax;
    *(volatile uint16_t *)(uintptr_t)(sub0 + 4) = dx;
    uint8_t al = *(volatile uint8_t *)(uintptr_t)(sub0 + 0xc), oal = al;
    uint8_t ah = *(volatile uint8_t *)(uintptr_t)(sub0 + 0xd), oah = ah;
    al = (uint8_t)((al & 0xf3) | ((cl & 3) << 2));
    ah = (uint8_t)((ah & 0xf3) | (cl & 0xc));
    *(volatile uint8_t *)(uintptr_t)(sub0 + 0xc) = al;
    *(volatile uint8_t *)(uintptr_t)(sub0 + 0xd) = ah;
    if (!frozen)
        *(volatile uint8_t *)(uintptr_t)(edi + 7) = (uint8_t)(((oal & 0xc) >> 2) | (oah & 0xc));
    if (--esi <= 0) return;
    uint16_t cx = *(volatile uint16_t *)(uintptr_t)(sub0 + 0);
    uint16_t dx2 = *(volatile uint16_t *)(uintptr_t)(sub0 + 4);
    do {
        uint32_t celln = geom + *(volatile uint16_t *)(uintptr_t)ebp; ebp += 2;
        uint32_t subnw = *(volatile uint16_t *)(uintptr_t)(celln + 0x18);
        if (subnw == 0) return;
        uint32_t subn = geom + subnw;
        *(volatile uint16_t *)(uintptr_t)(subn + 0) = cx;
        *(volatile uint16_t *)(uintptr_t)(subn + 4) = dx2;
    } while (--esi > 0);
}

/* tick_change_floor_texture_b (0x333c0): per-frame variant using swap_cell_state_group_v2. Same countdown/swap/
 * finalize shape as tick_change_floor_texture. */
int32_t tick_change_floor_texture_b(uint32_t rec)
{
    uint32_t esi = rec;
    uint32_t edi = *(volatile uint32_t *)(uintptr_t)(esi + 8);
    int32_t step = (int32_t)G32(VA_g_frame_time_scale);
    if (*(volatile uint8_t *)(uintptr_t)(esi + 5) & 0x40)
        return rawcmd_texture_countdown(esi, step);
    swap_cell_state_group_v2(edi, esi + 0xc);               /* call 0x333ec */
    uint32_t edx = *(volatile uint16_t *)(uintptr_t)(edi + 0xe);
    return rawcmd_texture_tick_finalize(esi, edi, edx);
}

/* tick_change_floor_texture (0x33229): per-frame floor-texture tick — armed-bit countdown, else swap the cell
 * group via swap_cell_state_group_v1 and run the shared finalize (latch word[cell+0xe]). */
int32_t tick_change_floor_texture(uint32_t rec)
{
    uint32_t esi = rec;
    uint32_t edi = *(volatile uint32_t *)(uintptr_t)(esi + 8);
    int32_t step = (int32_t)G32(VA_g_frame_time_scale);
    if (*(volatile uint8_t *)(uintptr_t)(esi + 5) & 0x40)
        return rawcmd_texture_countdown(esi, step);
    swap_cell_state_group_v1(edi, esi + 0xc);               /* call 0x33255 (edi=cell, ebp=rec+0xc) */
    uint32_t edx = *(volatile uint16_t *)(uintptr_t)(edi + 0xe);
    return rawcmd_texture_tick_finalize(esi, edi, edx);
}

/* tick_rotate_object (0x33188): per-frame rotate-object tick. Generation-gated re-resolve (collect_secondary_
 * matches_into_struct). The step = [0x85324] * byte[cell+0xa] (rate). If the group is empty -> finalize
 * (byte[[rec+8]+2]&=0xde, ret -1). Otherwise per member: the MAIN path (byte[cell+6]&2 == 0) adds the step low
 * byte to each object's rotation byte[obj+6] (member = word offset into objects 0x90aa4); the ALT path turns each
 * object toward the player — bearing = compute_player_object_bearing(obj), delta = (int8)(bearing - byte[obj+6])
 * clamped to +/-step, applied as byte[obj+6] -= delta (member = byte offset). Returns 0 (per-member) / -1 (empty). */
int32_t tick_rotate_object(uint32_t rec)
{
    uint32_t esi = rec;
    uint32_t edi = *(volatile uint32_t *)(uintptr_t)(esi + 8);
    uint16_t gen = (uint16_t)(uint32_t)G32(VA_g_object_table_generation);
    if (gen != *(volatile uint16_t *)(uintptr_t)(esi + 0xc)) {
        *(volatile uint16_t *)(uintptr_t)(esi + 0xc) = gen;
        collect_secondary_matches_into_struct(esi + 0x10);
    }
    int32_t step = (int32_t)G32(VA_g_frame_time_scale) * (int32_t)(*(volatile uint8_t *)(uintptr_t)(edi + 0xa));
    uint32_t ebx = esi + 0x10;
    int32_t ebp = *(volatile uint16_t *)(uintptr_t)ebx;
    if (ebp == 0) {
        uint32_t e2 = *(volatile uint32_t *)(uintptr_t)(esi + 8);
        *(volatile uint8_t *)(uintptr_t)(e2 + 2) &= 0xde;
        return -1;
    }
    ebx += 4;
    uint32_t objects = (uint32_t)G32(VA_g_map_objects_buffer);
    if (*(volatile uint8_t *)(uintptr_t)(edi + 6) & 2) {
        int32_t edx = step;
        do {
            uint32_t obj = objects + *(volatile uint8_t *)(uintptr_t)ebx;       /* movzx byte[ebx] */
            uint32_t bearing = compute_player_object_bearing(obj);
            int32_t v = (int32_t)(int8_t)((uint8_t)bearing - *(volatile uint8_t *)(uintptr_t)(obj + 6));
            if (!(v < edx)) v = edx;
            v = -v;
            if (!(v < edx)) v = edx;
            *(volatile uint8_t *)(uintptr_t)(obj + 6) =
                (uint8_t)(*(volatile uint8_t *)(uintptr_t)(obj + 6) - (uint8_t)v);
            ebx += 2;
        } while (--ebp > 0);
        return 0;
    }
    uint8_t al = (uint8_t)step;
    do {
        uint32_t off = *(volatile uint16_t *)(uintptr_t)ebx;                    /* word[ebx] */
        *(volatile uint8_t *)(uintptr_t)(objects + off + 6) += al;
        ebx += 2;
    } while (--ebp > 0);
    return 0;
}

/* rawcmd_tick_height_exit (0x32ffd): the shared "overshoot" exit used by the height/move ticks. Reload edi=[rec+8]
 * and the latch edx=word[edi+0xe]; if byte[rec+5]&0x10 just toggle (byte[rec+5]^=0x90, ret 0 — 0x32fa0) else run
 * the shared post-swap finalize state machine. */
static int32_t rawcmd_tick_height_exit(uint32_t esi)
{
    uint32_t edi = *(volatile uint32_t *)(uintptr_t)(esi + 8);
    uint32_t edx = *(volatile uint16_t *)(uintptr_t)(edi + 0xe);
    if (*(volatile uint8_t *)(uintptr_t)(esi + 5) & 0x10) {
        *(volatile uint8_t *)(uintptr_t)(esi + 5) ^= 0x90;
        return 0;
    }
    return rawcmd_texture_tick_finalize(esi, edi, edx);
}

/* tick_change_object_height (0x33091): per-frame tick that ramps a group of objects' height word[obj+0xa] toward a
 * target. Generation-gated re-resolve (collect_secondary_matches_into_struct, eax=rec+0x10). The armed bit
 * (byte[rec+5]&0x40) just counts the timer down. delta = step([0x85324]) * byte[cell+7]; if byte[cell+6]&4 the
 * delta is the integer part of a fixed-point budget (byte[rec+6] keeps the low 6 fractional bits of (delta+old),
 * delta >>= 6, and a zero integer step means no movement this frame -> ret 0). With an empty group:
 * byte[[rec+8]+2]&=0xde, ret -1. Otherwise walk the count (word[rec+0x10]) member offsets at rec+0x14 and step each
 * object's height toward the limit (2*sword[cell+0xa] descending / 2*sword[cell+0xc] ascending; dir = byte[rec+5]&
 * 0x80). When a member overshoots, the surplus is folded back into the per-frame budget; once the budget is spent
 * the tick ends through the shared overshoot exit (rawcmd_tick_height_exit / 0x32ffd). The clamp arithmetic is
 * 16-bit (only AX is stored and only the low word of the budget is tested; the residual high bits of the x86
 * accumulator never escape), so it is modelled in int16. Returns 0 on a full sweep, finalize result on overshoot. */
int32_t tick_change_object_height(uint32_t rec)
{
    uint32_t esi = rec;
    uint32_t edi = *(volatile uint32_t *)(uintptr_t)(esi + 8);
    uint16_t gen = (uint16_t)(uint32_t)G32(VA_g_object_table_generation);
    if (gen != *(volatile uint16_t *)(uintptr_t)(esi + 0xc)) {
        *(volatile uint16_t *)(uintptr_t)(esi + 0xc) = gen;
        collect_secondary_matches_into_struct(esi + 0x10);  /* call 0x33072 */
    }
    int32_t step = (int32_t)G32(VA_g_frame_time_scale);
    if (*(volatile uint8_t *)(uintptr_t)(esi + 5) & 0x40)
        return rawcmd_texture_countdown(esi, step);                 /* armed -> countdown (0x3305f) */

    uint32_t delta = (uint32_t)(step * (int32_t)(*(volatile uint8_t *)(uintptr_t)(edi + 7)));  /* imul step,byte[cell+7] */
    if (*(volatile uint8_t *)(uintptr_t)(edi + 6) & 4) {            /* fixed-point budget */
        delta += (uint32_t)(*(volatile uint8_t *)(uintptr_t)(esi + 6) & 0x3f);
        *(volatile uint8_t *)(uintptr_t)(esi + 6) = (uint8_t)delta;  /* mov byte[rec+6], al */
        delta >>= 6;                                                /* shr eax, 6 */
        if (delta == 0)
            return 0;                                               /* 0x33130 */
    }

    uint32_t ebx = esi + 0x10;
    int32_t count = *(volatile uint16_t *)(uintptr_t)ebx;           /* bp = word[rec+0x10] */
    if (count == 0) {                                               /* 0x33136 */
        uint32_t e2 = *(volatile uint32_t *)(uintptr_t)(esi + 8);
        *(volatile uint8_t *)(uintptr_t)(e2 + 2) &= 0xde;
        return -1;
    }
    ebx += 4;                                                       /* member offsets at rec+0x14 */
    uint32_t objects = (uint32_t)G32(VA_g_map_objects_buffer);

    if (*(volatile uint8_t *)(uintptr_t)(esi + 5) & 0x80) {         /* ascending (0x33144) */
        int16_t acc   = (int16_t)(uint16_t)delta;
        int16_t limit = (int16_t)(uint16_t)(2 * (int32_t)*(volatile int16_t *)(uintptr_t)(edi + 0xc));
        do {
            uint32_t off = *(volatile uint16_t *)(uintptr_t)ebx;
            int16_t sum = (int16_t)((int16_t)*(volatile uint16_t *)(uintptr_t)(objects + off + 0xa) + acc);
            int16_t stored;
            if (sum <= limit) {                                     /* cmp ax,cx ; jle keep */
                stored = sum;
            } else {
                acc = (int16_t)(acc - (int16_t)(sum - limit));      /* sub eax,ecx ; sub [esp],eax */
                if (acc >= 0)                                       /* cmp word[esp],0 ; jge exit */
                    return rawcmd_tick_height_exit(esi);
                stored = limit;                                     /* mov eax,ecx */
            }
            *(volatile uint16_t *)(uintptr_t)(objects + off + 0xa) = (uint16_t)stored;
            ebx += 2;
        } while (--count > 0);
        return 0;                                                   /* 0x33182 */
    }

    /* descending (0x330f0) */
    int16_t acc   = (int16_t)(uint16_t)(-(int32_t)delta);
    int16_t limit = (int16_t)(uint16_t)(2 * (int32_t)*(volatile int16_t *)(uintptr_t)(edi + 0xa));
    do {
        uint32_t off = *(volatile uint16_t *)(uintptr_t)ebx;
        int16_t sum = (int16_t)((int16_t)*(volatile uint16_t *)(uintptr_t)(objects + off + 0xa) + acc);
        int16_t stored;
        if (sum >= limit) {                                         /* cmp ax,cx ; jge keep */
            stored = sum;
        } else {
            acc = (int16_t)(acc - (int16_t)(sum - limit));          /* sub eax,ecx ; sub [esp],eax */
            if (acc <= 0)                                           /* cmp word[esp],0 ; jle exit */
                return rawcmd_tick_height_exit(esi);
            stored = limit;                                         /* mov eax,ecx */
        }
        *(volatile uint16_t *)(uintptr_t)(objects + off + 0xa) = (uint16_t)stored;
        ebx += 2;
    } while (--count > 0);
    return 0;                                                       /* 0x33130 */
}

/* tick_change_height (0x32d7d): per-frame tick that ramps a group of SECTOR floor/ceiling heights toward a target
 * and carries the player/contained objects with them. Sibling of tick_change_object_height 0x33091 but operates on
 * the geometry buffer g@0x90aa8 (not the object buffer) and drives the player via apply_cell_move_to_player. There
 * is NO generation-gated re-resolve and NO count==0 guard here (the member loop is a do-while: count 0 still
 * processes member[0] once). The armed bit (byte[rec+5]&0x40) just counts the timer down (rawcmd_texture_countdown).
 * delta = step([0x85324]) * byte[cell+7]; byte[cell+6]&4 makes it a fixed-point budget (low 6 frac bits kept in
 * byte[rec+6], delta >>= 6, zero integer step -> ret 0). Four loop variants keyed on byte[cell+6]&1 (field at
 * geom+off+2 when clear / geom+off+0 when set) x byte[rec+5]&0x80 (ascending vs descending). Each cell's height is
 * stepped toward 2*sword[cell+0xa] (descending) / 2*sword[cell+0xc] (ascending); the surplus on a clamp folds back
 * into the budget. The carry: V1 (clear/desc) writes the full budget to g@0x8a12c and calls apply_cell_move_to_player
 * (flags=1, no portal); V2 (clear/asc) writes budget + apply_cell_move_to_player_portalcheck (accumulates the merge
 * flag); V3 (set/desc) writes 0 to g@0x8a12c + portalcheck; V4 (set/asc) does NO carry at all. On a full sweep V2/V3
 * branch to the toggle exit 0x32fa0 (byte[rec+5]^=0x90) when any portal merge fired, else all variants run the
 * 0x32e2f finalize (set byte[rec+5]|=2 + byte[[rec+0xc]+8]|=0x80 + play_world_sound, BRIDGED no-op) and ret 0. When
 * a cell overshoots its limit the tick takes the 0x32fab exit (optional stop_sound_handle_voice + play_entity_sound,
 * BRIDGED) then the shared rawcmd_tick_height_exit (0x32ffd) state machine. The clamp + budget arithmetic is modeled
 * in full 32-bit (the carry driver returns 0/2 so the accumulator high half stays the true budget high). */
int32_t tick_change_height(uint32_t rec)
{
    uint32_t esi = rec;
    uint32_t cell = *(volatile uint32_t *)(uintptr_t)(esi + 8);     /* edi = [rec+8] */
    int32_t step = (int32_t)G32(VA_g_frame_time_scale);

    if (*(volatile uint8_t *)(uintptr_t)(esi + 5) & 0x40)           /* armed -> 0x3305f countdown */
        return rawcmd_texture_countdown(esi, step);

    int32_t delta_dw = step * (int32_t)(uint32_t)*(volatile uint8_t *)(uintptr_t)(cell + 7);  /* imul step,byte[cell+7] */
    if (*(volatile uint8_t *)(uintptr_t)(cell + 6) & 4) {           /* fixed-point budget */
        delta_dw += (int32_t)(*(volatile uint8_t *)(uintptr_t)(esi + 6) & 0x3f);
        *(volatile uint8_t *)(uintptr_t)(esi + 6) = (uint8_t)delta_dw;
        delta_dw = (int32_t)((uint32_t)delta_dw >> 6);              /* shr eax,6 */
        if (delta_dw == 0) return 0;                               /* 0x32e4f */
    }

    int field2 = !(*(volatile uint8_t *)(uintptr_t)(cell + 6) & 1); /* byte[cell+6]&1 clear -> field at off+2 */
    int ascend = (*(volatile uint8_t *)(uintptr_t)(esi + 5) & 0x80) != 0;
    int fieldoff = field2 ? 2 : 0;

    int32_t budget_dw = ascend ? delta_dw : -delta_dw;             /* neg for descending */
    int32_t limit32 = ascend
        ? 2 * (int32_t)*(volatile int16_t *)(uintptr_t)(cell + 0xc)
        : 2 * (int32_t)*(volatile int16_t *)(uintptr_t)(cell + 0xa);
    int16_t limit16 = (int16_t)limit32;

    uint32_t geom = (uint32_t)G32(VA_g_map_geometry_buffer);
    uint32_t memb = esi + 0x14;                                    /* member offsets (count is at esi+0x10) */
    int32_t count = (int32_t)*(volatile uint16_t *)(uintptr_t)(esi + 0x10);

    uint8_t flagacc = 0;
    int overshoot = 0;
    int i = 0;
    do {                                                          /* do-while: count 0 -> one pass */
        uint16_t off = *(volatile uint16_t *)(uintptr_t)(memb + (uint32_t)i * 2);
        uint32_t gp = geom + off + (uint32_t)fieldoff;
        int16_t cur = *(volatile int16_t *)(uintptr_t)gp;
        if (ascend ? (cur >= limit16) : (cur <= limit16)) { overshoot = 1; break; }
        uint32_t sum32 = (uint32_t)(uint16_t)cur + (uint32_t)budget_dw;
        int16_t sum_lo = (int16_t)(uint16_t)sum32;
        uint16_t stored;
        if (ascend ? (sum_lo <= limit16) : (sum_lo >= limit16)) {
            stored = (uint16_t)sum32;                             /* keep */
        } else {
            int32_t e = (int32_t)sum32 - limit32;                /* surplus */
            budget_dw -= e;
            stored = (uint16_t)limit32;                          /* clamp to limit */
        }
        *(volatile uint16_t *)(uintptr_t)gp = stored;

        if (field2) {                                            /* V1/V2: field off+2, write budget to 0x8a12c */
            G32(VA_g_player_move_delta_z) = budget_dw;
            if (ascend)                                          /* V2 */
                flagacc |= (uint8_t)apply_cell_move_to_player_portalcheck(1, off);
            else                                                /* V1 */
                apply_cell_move_to_player(1, off);
        } else {                                                /* V3/V4: field off+0 */
            if (!ascend) {                                      /* V3 */
                G32(VA_g_player_move_delta_z) = 0;
                flagacc |= (uint8_t)apply_cell_move_to_player_portalcheck(1, off);
            }
            /* V4: no carry, no 0x8a12c write, no accumulate */
        }
        i++;
    } while (--count > 0);

    if (overshoot) {                                            /* 0x32fab exit tail */
        uint8_t dl = *(volatile uint8_t *)(uintptr_t)(esi + 5);
        if (dl & 3) {
            uint32_t rec0c = *(volatile uint32_t *)(uintptr_t)(esi + 0xc);
            if (rec0c != 0) {
                if (dl & 2) {
                    *(volatile uint8_t *)(uintptr_t)(esi + 5) -= 2;
                    uint8_t b8 = *(volatile uint8_t *)(uintptr_t)(rec0c + 8);
                    if ((b8 & 3) && (b8 & 0x80)) {
                        *(volatile uint8_t *)(uintptr_t)(rec0c + 8) -= 0x80;
                        stop_sound_handle_voice(rec0c);   /* 0x26d3e (re-pointed; gated no-op) */
                    }
                }
                uint32_t edi2 = *(volatile uint32_t *)(uintptr_t)(esi + 8);
                uint16_t snd = *(volatile uint16_t *)(uintptr_t)(edi2 + 0x12);
                if (snd != 0)                                   /* play_entity_sound 0x271c4 (re-pointed) */
                    play_entity_sound((uint32_t)(snd - 1), 0,
                        *(volatile uint16_t *)(uintptr_t)(rec0c),
                        *(volatile uint16_t *)(uintptr_t)(rec0c + 2));
            }
        }
        return rawcmd_tick_height_exit(esi);                     /* 0x32ffd */
    }

    /* full sweep: V2/V3 take the toggle exit when any portal merge fired (V1/V4 fall straight through) */
    if (((!field2 && !ascend) || (field2 && ascend)) && flagacc != 0) {
        *(volatile uint8_t *)(uintptr_t)(esi + 5) ^= 0x90;          /* 0x32fa0 */
        return 0;
    }

    /* 0x32e2f finalize: latch byte[rec+5]|=2 and fire the world sound once */
    uint8_t b5 = *(volatile uint8_t *)(uintptr_t)(esi + 5);
    if ((b5 & 3) && !(b5 & 2)) {
        *(volatile uint8_t *)(uintptr_t)(esi + 5) |= 2;
        uint32_t rec0c = *(volatile uint32_t *)(uintptr_t)(esi + 0xc);
        *(volatile uint8_t *)(uintptr_t)(rec0c + 8) |= 0x80;
        play_world_sound_squared_dist(rec0c, 0);             /* 0x2721c (re-pointed; gated no-op) */
    }
    return 0;
}

/* tms_step: one clamp-step of a 16-bit geometry height toward 'lim' with a signed-16 budget *bud (the live low word
 * of g@0x8a12c). For descending (desc=1) skip when cur<=lim and keep when cur+bud>lim; for ascending (desc=0) skip
 * when cur>=lim and keep when cur+bud<lim; clamp to lim otherwise. When 'decr', a clamp consumes the budget by the
 * surplus (sub word/dword [0x8a12c] — only the low 16 escapes, the high half is masked by the harness). Writes the
 * field and returns 1 on a move, 0 on a skip. Shared by every floor/ceiling sub-pass of tick_modify_sector. */
static int tms_step(volatile uint16_t *field, int16_t lim, uint16_t *bud, int desc, int decr)
{
    int16_t cur = (int16_t)*field;
    if (desc ? (cur <= lim) : (cur >= lim)) return 0;
    int16_t sum = (int16_t)(cur + (int16_t)*bud);
    uint16_t stored;
    if (desc ? (sum > lim) : (sum < lim)) {
        stored = (uint16_t)sum;                                /* keep */
    } else {
        if (decr) *bud = (uint16_t)(*bud - (uint16_t)((uint16_t)sum - (uint16_t)lim));
        stored = (uint16_t)lim;                                /* clamp */
    }
    *field = stored;
    return 1;
}

/* tick_modify_sector (0x335ec): the largest effect tick — per-frame advance of a sector-modify effect (raise/lower
 * floors+ceilings, fade sector light, carry the player) that ramps SECTOR heights toward either a fixed limit
 * (descending: 2*sword[cell+0xa]) or per-cell snapshot targets (ascending: the captured heights at [[cell+0x10]]+8).
 * Shares the prologue (armed countdown 0x3305f, fixed-point budget byte[rec+6]) and the 0x32e2f / 0x32fa0 / 0x3399c
 * tails with tick_change_height. Four loop bodies keyed on byte[rec+5]&0x80 (descending vs ascending) x byte[cell+6]&3
 * (simple floor-only vs the complex variant that also fades the sector light byte[geom+off+0xc] via g@0x8a0e0 and
 * moves a linked ceiling). The budget lives in g@0x8a12c and is RESET per sub-pass (it does not accumulate across
 * cells like tick_change_height); the carry reads only its low word so its high half is dead (harness masks 0x8a12e).
 * The two complex "second" sub-passes (linked cell floor) use the loop counter [esp+4] as their budget and take no
 * carry — a faithful quirk. Player carry: descending via apply_cell_move_to_player (no portal), ascending via
 * apply_cell_move_to_player_portalcheck (merge flag accumulated into the [esp+6] bit1 that routes the ascending
 * sweep to the toggle exit 0x32fa0). Sounds (play_world_sound 0x2721c / stop_sound_handle_voice 0x26d3e /
 * play_command_sound 0x2730b) are BRIDGED (no-ops with 0x7f550=0). */
int32_t tick_modify_sector(uint32_t rec)
{
    uint32_t esi = rec;
    uint32_t cell = *(volatile uint32_t *)(uintptr_t)(esi + 8);
    int32_t step = (int32_t)G32(VA_g_frame_time_scale);
    if (*(volatile uint8_t *)(uintptr_t)(esi + 5) & 0x40)
        return rawcmd_texture_countdown(esi, step);

    int32_t delta = step * (int32_t)(uint32_t)*(volatile uint8_t *)(uintptr_t)(cell + 7);
    if (*(volatile uint8_t *)(uintptr_t)(cell + 6) & 4) {
        delta += (int32_t)(*(volatile uint8_t *)(uintptr_t)(esi + 6) & 0x3f);
        *(volatile uint8_t *)(uintptr_t)(esi + 6) = (uint8_t)delta;
        delta = (int32_t)((uint32_t)delta >> 6);
        if (delta == 0) return 0;
    }

    uint32_t geom = (uint32_t)G32(VA_g_map_geometry_buffer);
    uint32_t memb = esi + 0x14;
    int32_t count = (int32_t)*(volatile uint16_t *)(uintptr_t)(esi + 0x10);
    uint8_t f6 = *(volatile uint8_t *)(uintptr_t)(cell + 6);
    uint8_t m6 = 0;                                            /* [esp+6]: bit0 moved, bit1 portal merge */
    int ascend = (*(volatile uint8_t *)(uintptr_t)(esi + 5) & 0x80) != 0;

    if (!ascend) {
        /* ---------------- DESCENDING (0x33630) ---------------- */
        uint16_t master = (uint16_t)(-(int32_t)delta);
        int16_t limD = (int16_t)(2 * (int32_t)*(volatile int16_t *)(uintptr_t)(cell + 0xa));
        if (!(f6 & 3)) {
            int32_t lc = (int16_t)count;                       /* desc-simple (0x33652) */
            for (;;) {
                uint16_t off = *(volatile uint16_t *)(uintptr_t)memb;
                uint16_t bud = master; G16(VA_g_player_move_delta_z) = bud;
                int moved = tms_step((volatile uint16_t *)(uintptr_t)(geom + off + 2), limD, &bud, 1, 1);
                G16(VA_g_player_move_delta_z) = bud;
                if (moved) { m6 |= 1; apply_cell_move_to_player(1, off); }
                memb += 2; lc = (int16_t)(lc - 1); if (!(lc > 0)) break;
            }
        } else {                                              /* desc-complex (0x336b1) */
            uint8_t bit = *(volatile uint8_t *)(uintptr_t)(esi + 7) & 1;
            uint32_t e = (uint32_t)delta + bit;
            *(volatile uint8_t *)(uintptr_t)(esi + 7) = (uint8_t)e;
            G32(VA_g_pending_command_record + 0x4) = -(int32_t)(e >> 1);                 /* light-fade (negated) */
            int32_t lc = (int16_t)count;
            for (;;) {
                uint16_t off = *(volatile uint16_t *)(uintptr_t)memb;
                uint32_t gp = geom + off;
                uint16_t bud = master; G16(VA_g_player_move_delta_z) = bud;
                int moved = tms_step((volatile uint16_t *)(uintptr_t)(gp + 2), limD, &bud, 1, 1);
                G16(VA_g_player_move_delta_z) = bud;
                if (moved) { m6 |= 1; apply_cell_move_to_player(1, off); }
                if ((f6 & 2) && !(*(volatile uint8_t *)(uintptr_t)(gp + 0xa) & 1)
                             && (int8_t)*(volatile uint8_t *)(uintptr_t)(gp + 0xc) < -1) {
                    uint8_t pos = (uint8_t)(-(int8_t)*(volatile uint8_t *)(uintptr_t)(gp + 0xc));
                    int32_t ee = (int32_t)pos + (int32_t)G32(VA_g_pending_command_record + 0x4);
                    if (!(ee > 1)) ee = 1;
                    *(volatile uint8_t *)(uintptr_t)(gp + 0xc) = (uint8_t)(0u - (uint8_t)ee);
                    m6 |= 1;
                }
                if (f6 & 1) {
                    uint16_t link = *(volatile uint16_t *)(uintptr_t)(gp + 0x18);
                    if (link != 0) {
                        uint32_t lcell = geom + link;
                        uint16_t bud2 = master; G16(VA_g_player_move_delta_z) = bud2;
                        int cm = tms_step((volatile uint16_t *)(uintptr_t)(lcell + 8), limD, &bud2, 1, 1);
                        G16(VA_g_player_move_delta_z) = bud2;
                        if (cm) { m6 |= 1; apply_cell_move_to_player(2, off); }
                        uint16_t rem = master;                 /* [esp+4] after push ebx == [E] == master budget */
                        if (tms_step((volatile uint16_t *)(uintptr_t)(lcell + 2), limD, &rem, 1, 0)) m6 |= 1;
                    }
                }
                memb += 2; lc = (int16_t)(lc - 1); if (!(lc > 0)) break;
            }
        }
        if (!(m6 & 1)) goto tail3399c;                        /* no move -> sound+exit */
        goto finalize;                                        /* 0x32e2f */
    } else {
        /* ---------------- ASCENDING (0x337d5) ---------------- */
        uint16_t master = (uint16_t)delta;
        uint32_t snap = *(volatile uint32_t *)(uintptr_t)(*(volatile uint32_t *)(uintptr_t)(cell + 0x10)) + 8;
        if (!(f6 & 3)) {
            int32_t lc = (int16_t)count;                       /* asc-simple (0x337f8) */
            for (;;) {
                uint16_t off = *(volatile uint16_t *)(uintptr_t)memb;
                int16_t tF = (int16_t)*(volatile uint16_t *)(uintptr_t)snap; snap += 2;
                uint16_t bud = master; G16(VA_g_player_move_delta_z) = bud;
                int moved = tms_step((volatile uint16_t *)(uintptr_t)(geom + off + 2), tF, &bud, 0, 1);
                G16(VA_g_player_move_delta_z) = bud;
                if (moved) { m6 |= 1; m6 |= (uint8_t)apply_cell_move_to_player_portalcheck(1, off); }
                memb += 2; lc = (int16_t)(lc - 1); if (!(lc > 0)) break;
            }
        } else {                                              /* asc-complex (0x33869) */
            uint8_t bit = *(volatile uint8_t *)(uintptr_t)(esi + 7) & 1;
            uint32_t e = (uint32_t)delta + bit;
            *(volatile uint8_t *)(uintptr_t)(esi + 7) = (uint8_t)e;
            G32(VA_g_pending_command_record + 0x4) = (int32_t)(e >> 1);                  /* light-fade (not negated) */
            int32_t lc = (int16_t)count;
            for (;;) {
                uint16_t off = *(volatile uint16_t *)(uintptr_t)memb;
                uint32_t gp = geom + off;
                int16_t tF = (int16_t)*(volatile uint16_t *)(uintptr_t)snap;
                int16_t floor0 = (int16_t)*(volatile uint16_t *)(uintptr_t)(gp + 2);
                uint16_t bud = master; G16(VA_g_player_move_delta_z) = bud;
                int moved = tms_step((volatile uint16_t *)(uintptr_t)(gp + 2), tF, &bud, 0, 1);
                G16(VA_g_player_move_delta_z) = bud;
                if (moved) { m6 |= 1; m6 |= (uint8_t)apply_cell_move_to_player_portalcheck(1, off); }
                if ((f6 & 2) && !(*(volatile uint8_t *)(uintptr_t)(gp + 0xa) & 1)) {
                    int8_t bc = (int8_t)*(volatile uint8_t *)(uintptr_t)(gp + 0xc);
                    int8_t tL = (int8_t)*(volatile uint8_t *)(uintptr_t)(snap + 2);
                    if (bc < 0 && bc != tL) {
                        uint32_t eax = moved
                            ? (uint8_t)(-bc)
                            : (((uint32_t)delta & 0xffff0000u) | ((uint32_t)(uint16_t)floor0 & 0xff00u) | (uint8_t)(-bc));
                        eax += (uint32_t)G32(VA_g_pending_command_record + 0x4);
                        uint32_t ecx = (uint8_t)(-tL);
                        if (!(eax < ecx)) eax = ecx;
                        *(volatile uint8_t *)(uintptr_t)(gp + 0xc) = (uint8_t)(0u - (uint8_t)eax);
                        m6 |= 1;
                    }
                }
                if (f6 & 1) {
                    uint16_t link = *(volatile uint16_t *)(uintptr_t)(gp + 0x18);
                    if (link != 0) {
                        uint32_t lcell = geom + link;
                        int16_t tC = (int16_t)*(volatile uint16_t *)(uintptr_t)(snap + 4);
                        uint16_t rem = master; G16(VA_g_player_move_delta_z) = rem;   /* [esp+4] after push ebx == [E] == master */
                        int cm = tms_step((volatile uint16_t *)(uintptr_t)(lcell + 8), tC, &rem, 0, 1);
                        G16(VA_g_player_move_delta_z) = rem;
                        if (cm) { m6 |= 1; m6 |= (uint8_t)apply_cell_move_to_player_portalcheck(2, off); }
                        int16_t tCF = (int16_t)*(volatile uint16_t *)(uintptr_t)(snap + 6);
                        uint16_t rem2 = master;
                        if (tms_step((volatile uint16_t *)(uintptr_t)(lcell + 2), tCF, &rem2, 0, 0)) m6 |= 1;
                    }
                }
                snap += 8; memb += 2; lc = (int16_t)(lc - 1); if (!(lc > 0)) break;
            }
        }
        if (!(m6 & 1)) goto tail3399c;
        if (m6 & 2) { *(volatile uint8_t *)(uintptr_t)(esi + 5) ^= 0x90; return 0; }  /* 0x32fa0 toggle */
        goto finalize;                                        /* 0x32e2f */
    }

finalize:                                                     /* 0x32e2f */
    {
        uint8_t b5 = *(volatile uint8_t *)(uintptr_t)(esi + 5);
        if ((b5 & 3) && !(b5 & 2)) {
            *(volatile uint8_t *)(uintptr_t)(esi + 5) |= 2;
            uint32_t r0c = *(volatile uint32_t *)(uintptr_t)(esi + 0xc);
            *(volatile uint8_t *)(uintptr_t)(r0c + 8) |= 0x80;
            play_world_sound_squared_dist(r0c, 0);     /* 0x2721c (re-pointed; gated no-op) */
        }
        return 0;
    }

tail3399c:                                                    /* 0x3399c sound + rawcmd_tick_height_exit */
    {
        uint8_t dl = *(volatile uint8_t *)(uintptr_t)(esi + 5);
        if (dl & 3) {
            uint32_t r0c = *(volatile uint32_t *)(uintptr_t)(esi + 0xc);
            if (r0c != 0) {
                if (dl & 2) {
                    *(volatile uint8_t *)(uintptr_t)(esi + 5) -= 2;
                    uint8_t b8 = *(volatile uint8_t *)(uintptr_t)(r0c + 8);
                    if ((b8 & 3) && (b8 & 0x80)) {
                        *(volatile uint8_t *)(uintptr_t)(r0c + 8) -= 0x80;
                        stop_sound_handle_voice(r0c);  /* 0x26d3e (re-pointed; gated no-op) */
                    }
                }
                uint16_t snd = *(volatile uint16_t *)(uintptr_t)
                    (*(volatile uint32_t *)(uintptr_t)(esi + 8) + 0xc);
                if (snd != 0)                                 /* play_command_sound 0x2730b (re-pointed) */
                    play_command_sound((uint32_t)(snd - 1), 0,
                        *(volatile uint16_t *)(uintptr_t)(r0c),
                        *(volatile uint16_t *)(uintptr_t)(r0c + 2));
            }
        }
        return rawcmd_tick_height_exit(esi);
    }
}

/* ===================================================================================================
 * TRIGGER FIRERS — in-game LIVE-SWAP group (ROTH_LIFT=firers). These are NOT static-oracle-verifiable:
 * each walks a trigger table then hands off to a non-idempotent command dispatcher/executor, so the
 * differential (run-orig-then-lift) would double-fire the command. Instead the firer's THIN native part
 * (global setup + table walk + dispatch decision) is transcribed and every non-idempotent callee is
 * BRIDGED via call_orig (which suspends the int3s, so the dispatch + its handlers run exactly once, as
 * the original). Validation = live-swap + play (confirm the trigger still fires correctly in-game).
 * =================================================================================================== */

/* fire_object_use_trigger (0x10d1e): the "use/activate object" entry-command trigger. `in` = the firer's incoming
 * GP-register snapshot (EAX = used object record; ESI/EDI/ECX/EDX = the ambient state the dispatcher + fixup read).
 * Zeroes the per-frame trigger flag byte[0x7e928], then hands the object to dispatch_entry_command_trigger 0x34d75
 * (BRIDGED), reproducing the original's register state at the call: EDX=EBX=obj? no — EDX=obj (mov edx,eax), EBX=
 * ydelta, EBP=incoming EDX, ESI/EDI/ECX = incoming (the dispatcher saves+restores them). On a dispatched hit
 * (EAX!=0) it runs the post-dispatch fixup 0x351c3 (BRIDGED — it dereferences the incoming ESI, so ESI MUST be
 * forwarded) and returns -1; else 0. pushal/popal-framed so live-swap preserves every GP register but EAX.
 * NOTE (lesson): the firer is NOT EAX/EDX-only — 0x351c3 reads [ESI+2]; passing a zeroed ESI faults at canon
 * 0x351ee. Hence ABI_FIRER forwards the whole incoming register snapshot. */
int32_t fire_object_use_trigger(const regs_t *in)
{
    (void)((int32_t)G32(VA_g_saved_int9_offset + 0x20) - (int32_t)G32(VA_g_view_y));              /* ydelta (dispatcher-internal; lifted body reads EAX only) */
    G8(VA_g_mouse_buttons_held) = 0;
    uint32_t disp;
    {                                                                  /* dispatch_entry_command_trigger 0x34d75 (re-pointed; body reads EAX) */
        regs_t sub; memset(&sub, 0, sizeof sub); sub.eax = in->eax;
        disp = (uint32_t)dispatch_entry_command_trigger(&sub);
    }
    if (disp != 0) {
        fire_command_contact_trigger(disp);                     /* 0x351c3 (re-pointed; rec=EAX=disp) */
        return -1;
    }
    return 0;
}

/* fire_entity_pending_trigger (0x42793): per-entity "pending trigger" processor. `in->edi` = the entity record;
 * [edi+4] = its sub-record. Clears byte[sub+7]&0xbf, and if byte[sub+9]&0x20 was set: clears it and runs the
 * sub-record's trigger setup 0x35303 (BRIDGED — reads EAX=sub-record), then (if [edi+4] still non-null and the
 * record is quiescent) clears byte[rec+7]&0xfd. If byte[edi+8]&0x40 and word[edi+0x16]!=0, copies the linked
 * geom cell's floor word[cell+2] into word[sub+0xa]. Finally fires the entity's command 0x4273e (BRIDGED — reads
 * EDI=record, EDX=[edi+0xc]). Record pointers are runtime addresses (direct deref); geom base 0x90aa8 is canon. */
int32_t fire_entity_pending_trigger(const regs_t *in)
{
    uint32_t edi = in->edi;
    uint32_t sub = *(volatile uint32_t *)(uintptr_t)(edi + 4);
    *(volatile uint8_t *)(uintptr_t)(sub + 7) &= 0xbf;
    if (*(volatile uint8_t *)(uintptr_t)(sub + 9) & 0x20) {
        *(volatile uint8_t *)(uintptr_t)(sub + 9) -= 0x20;
        begin_object_command_chain(sub);                        /* 0x35303 (re-pointed; body reads EAX=sub) */
        uint32_t r4 = *(volatile uint32_t *)(uintptr_t)(edi + 4);
        if (r4 == 0) return 0;                                     /* je 0x427f1 (skip the firing) */
        if (*(volatile uint16_t *)(uintptr_t)(r4 + 0xe) == 0 &&
            !(*(volatile uint8_t *)(uintptr_t)(r4 + 9) & 0x21))
            *(volatile uint8_t *)(uintptr_t)(r4 + 7) &= 0xfd;
    }
    if (*(volatile uint8_t *)(uintptr_t)(edi + 8) & 0x40) {
        uint16_t link = *(volatile uint16_t *)(uintptr_t)(edi + 0x16);
        if (link != 0) {
            uint32_t cell = (uint32_t)link + (uint32_t)G32(VA_g_map_geometry_buffer);
            uint32_t s2 = *(volatile uint32_t *)(uintptr_t)(edi + 4);
            *(volatile uint16_t *)(uintptr_t)(s2 + 0xa) = *(volatile uint16_t *)(uintptr_t)(cell + 2);
        }
    }
    uint32_t edx = *(volatile uint32_t *)(uintptr_t)(edi + 0xc);
    reset_entity_state_with_sound(edi, edx);                    /* 0x4273e (re-pointed; EDI=entity, EDX=threshold) */
    return 0;
}

/* firer_bridge: invoke an original (non-idempotent) callee from a trigger firer's lift with a fully-specified
 * register frame + an optional single stack argument. Returns the callee's EAX. (call_orig keeps live fs/gs.) */
static uint32_t firer_bridge(uint32_t canon_va, uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx,
                             uint32_t esi, uint32_t edi, uint32_t ebp, uint32_t nstack, uint32_t stack0)
{
    regs_t io; memset(&io, 0, sizeof io);
    io.va = canon_va + OBJ_DELTA;
    io.eax = eax; io.ebx = ebx; io.ecx = ecx; io.edx = edx; io.esi = esi; io.edi = edi; io.ebp = ebp;
    io.nstack = nstack; if (nstack) io.stack[0] = stack0;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    /* the sole caller is the active-effect pool walk —
     * entry = the 0x3088c effect table (its 16 distinct targets are a SUBSET of the 0x30780 set;
     * table staged by roth_boot + registrants write live tokens). The record is in EAX here
     * (e.g. tick_delay_timer 0x32221 'eax=rec'); each lifted body's single param IS the record
     * regardless of the original's carrying register. Return threaded (!=0 = effect finished). */
    io.eax = rawcmd_dispatch_30780(io.va, eax);
#endif
    return io.eax;
}

/* fire_sector_trigger (0x31e8f): the walk-over / sector entry-command trigger. `in->esi` = the sector trigger
 * record. Early-out (returns 0) when byte[rec+2]&0x29, when the optional "needs target" gate (byte[rec+7]&4 +
 * g@0x7e8d8) fails, or when the per-cell already-fired bit (computed from the player cell word[0x90a8a]) is set
 * in byte[rec+6]; the early-out path still latches the contact globals (0x8a0e4/0x8a0e8/0x8a13c) when byte[rec+6]
 * &0x20. The fire path: latch the command-chain seed globals, run walk_command_chain_flow 0x353c4 (BRIDGED; ax=
 * word[rec+4]); on a clear walk it begins the object command chain (begin_object_command_chain 0x305b6, BRIDGED,
 * stack arg + EDI=rec) + advances the deferred queue (0x3484b, BRIDGED, stack arg), latches the contact globals
 * per byte[rec+6]&0x40/0x20, and (if a chain fired g@0x8a138 and byte[rec+6]&0x10) marks byte[rec+2]|=8. Returns
 * -1 on the fire path, 0 on an early-out. Record ptr = runtime (direct deref); player/cell globals = canon. */
int32_t fire_sector_trigger(const regs_t *in)
{
    uint32_t esi = in->esi;
    uint8_t f6 = *(volatile uint8_t *)(uintptr_t)(esi + 6);
    int gate = !(*(volatile uint8_t *)(uintptr_t)(esi + 2) & 0x29)
            && !((*(volatile uint8_t *)(uintptr_t)(esi + 7) & 4) && (uint32_t)G32(VA_g_move_speed_accum) == 0);
    if (gate) {
        if (*(volatile uint8_t *)(uintptr_t)(esi + 7) & 4) {           /* needs-target gate passed -> latch aim */
            G16(VA_g_state_link_buf_ptr + 0x124) = G16(VA_g_player_x);
            G16(VA_g_state_link_buf_ptr + 0x126) = G16(VA_g_player_y);
        }
        uint32_t t = (((uint32_t)(uint16_t)G16(VA_g_player_angle)) - 0x40u) & 0x1ffu;
        uint8_t bit = (uint8_t)(1u << ((t >> 7) & 7));
        if (!(f6 & bit)) {
            G32(VA_g_player_move_delta_z + 0x8) = 0;
            G16(VA_g_state_link_buf_ptr + 0x128) = *(volatile uint16_t *)(uintptr_t)(esi + 0xa);
            G32(VA_g_active_object) = 0; G32(VA_g_active_object + 0x4) = 0; G32(VA_g_command_source_object) = 0;
            uint32_t walk = walk_command_chain_flow(           /* 0x353c4 re-pointed (ax=word[rec+4]) */
                *(volatile uint16_t *)(uintptr_t)(esi + 4));
            if (walk == 0) {
                if (f6 & 0x20) G8(VA_g_command_chain_interrupt + 0x2) = 1;
                /* reset_command_chain_state re-enters the executor at 0x3065e with ax=word[esi+4] (chain-START index). */
                reset_command_chain_state(*(volatile uint16_t *)(uintptr_t)(esi + 4)); /* 0x305b6 re-pointed */
                process_deferred_command_queue();              /* 0x3484b re-pointed (drain deferred) */
                G32(VA_g_state_link_word_b) = 0; G8(VA_g_command_chain_interrupt + 0x2) = 0;
                if (f6 & 0x40) {
                    G32(VA_g_state_link_obj_ptr) = esi; G32(VA_g_state_link_word_b) = (uint32_t)G16(VA_g_player_sector);
                } else if (f6 & 0x20) {
                    G32(VA_g_state_link_buf_ptr) = 0; G32(VA_g_state_link_obj_ptr) = esi; G32(VA_g_state_link_word_a) = (uint32_t)G16(VA_g_player_sector);
                }
                if ((uint32_t)G32(VA_g_player_move_delta_z + 0xc) != 0 && (f6 & 0x10))
                    *(volatile uint8_t *)(uintptr_t)(esi + 2) |= 8;
                return -1;                                              /* 0x31fac */
            }
        }
    }
    /* early-out tail 0x31fd0 */
    G32(VA_g_state_link_word_b) = 0;
    if (f6 & 0x20) {
        G32(VA_g_state_link_buf_ptr) = 0; G32(VA_g_state_link_obj_ptr) = esi; G32(VA_g_state_link_word_a) = (uint32_t)G16(VA_g_player_sector);
    }
    return 0;
}

/* run_leftclick_object_trigger (0x303ff): the left-click "use object" entry-command trigger. `in->eax` = the
 * clicked object (word[obj+0xe] = its id). Clears g@0x8a0dc, then walks the object-table command directory
 * (base g@0x85c30, dir offset word[ot+8], count word[ot+0xa]) for the first non-skipped command record whose
 * word[rec+8] == id. No match -> g@0x8a134=0, ret 0. On a match: latch g@0x8a100/0x8a134, and if byte[rec+6]&2
 * destroy the entity (destroy_dynamic_entity 0x41f24, BRIDGED; the toggle target becomes the &DAT_0008a248
 * sentinel). If byte[rec+2]&8 -> ret 0. Dispatch on byte[rec+3]: 0x02 Light-Switch -> lifted cmd_light_switch
 * 0x33b94; on success play the switch SFX (play_sound_effect 0x27270, BRIDGED) + toggle word[obj+4]^1; 0x08
 * On-Left-Click -> lifted run_command_chain 0x305f0 (which runs the chain + its own ret-(-1) epilogue). All
 * dispatched paths end g@0x8a134=0, ret -1. Object/record/table ptrs are runtime (direct deref). */
int32_t run_leftclick_object_trigger(const regs_t *in)
{
    G32(VA_g_pending_command_record) = 0;
    uint32_t obj = in->eax;
    uint16_t id = *(volatile uint16_t *)(uintptr_t)(obj + 0xe);
    uint32_t ot = (uint32_t)G32(VA_g_object_table_header);
    if (id == 0 || ot == 0 || *(volatile uint16_t *)(uintptr_t)(ot + 0xa) == 0) {
        G32(VA_g_player_move_delta_z + 0x8) = 0; return 0;
    }
    G32(VA_g_player_move_delta_z + 0x8) = obj;                                            /* set during the walk (0x3041e) */
    uint32_t off = *(volatile uint16_t *)(uintptr_t)(ot + 8);
    int32_t c = (int32_t)*(volatile uint16_t *)(uintptr_t)(ot + 0xa);
    uint32_t rec = 0; int found = 0;
    do {
        uint16_t u4 = *(volatile uint16_t *)(uintptr_t)(ot + off);
        if (!(*(volatile uint8_t *)(uintptr_t)(ot + u4 + 2) & 8) &&
            *(volatile uint16_t *)(uintptr_t)(ot + u4 + 8) == id) { rec = ot + u4; found = 1; break; }
        off += 2;
    } while (--c > 0);
    if (!found) { G32(VA_g_player_move_delta_z + 0x8) = 0; return 0; }

    uint32_t ebp_obj = obj;
    G32(VA_g_command_source_object) = obj;
    if (*(volatile uint8_t *)(uintptr_t)(rec + 6) & 2) {           /* destroy-on-use entity */
        ebp_obj = 0;
        G32(VA_g_player_move_delta_z + 0x8) = (int32_t)(0x8a248u + OBJ_DELTA);            /* &DAT_0008a248 sentinel (runtime addr) */
        G32(VA_g_command_source_object) = (int32_t)(0x8a248u + OBJ_DELTA);
        destroy_dynamic_entity(in->eax, 0x8a248u + OBJ_DELTA); /* 0x41f24 re-pointed (eax=obj, edx=dest scratch) */
    }
    G32(VA_g_active_object) = 0; G32(VA_g_active_object + 0x4) = 0;
    if (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 8) { G32(VA_g_player_move_delta_z + 0x8) = 0; return 0; }

    uint8_t base = *(volatile uint8_t *)(uintptr_t)(rec + 3);
    if (base == 2) {                                              /* Light Switch (0x3051a) */
        if (cmd_light_switch(rec) != 0) {
            uint16_t snd = *(volatile uint16_t *)(uintptr_t)(rec + 0xe);
            if (snd != 0)
                play_sound_effect((uint32_t)(snd - 1), 0);      /* 0x27270 re-pointed (id, param=0) */
            *(volatile uint16_t *)(uintptr_t)(ebp_obj + 4) ^= 1;
        }
        G32(VA_g_player_move_delta_z + 0x8) = 0; return -1;
    }
    if (base == 8)                                               /* On-Left-Click -> run the command chain */
        return run_command_chain(rec);                    /* 0x305f0 does its own 0x8a134=0 + ret -1 */
    G32(VA_g_player_move_delta_z + 0x8) = 0; return -1;                                 /* other base -> 0x304a9 */
}

/* fire_tracked_object_trigger (0x35260): the "tracked object" contact trigger. `in->eax` = the object. Latches the
 * seed globals, resolves the object's command record by id via find_object_list40 0x34526 (LIFTED). No record ->
 * ret 0. Simple records (byte[rec+6]&2 clear) just run exec_object_trigger_no_source 0x35201 (LIFTED). For tracked
 * records (&2 set): save the object's id, RETAG word[obj+0xe]=0xc200, fire the trigger (0x35201), then collect the
 * record(s) now keyed 0xc200 (collect_secondary_state_records_by_key 0x34c97, LIFTED, cap 1 into a local scratch);
 * if one came back, resolve it in the object buffer (g@0x90aa4 + index), RESTORE its id, and (if a command fired
 * g@0x8a138) mark byte[newobj+7]|=0x80. All callees are verified lifts (no call_orig). Object ptrs = runtime. */
int32_t fire_tracked_object_trigger(const regs_t *in)
{
    uint32_t obj = in->eax;
    G32(VA_g_command_source_object) = obj;
    G32(VA_g_player_move_delta_z + 0x8) = 0;
    G32(VA_g_active_object) = 0; G32(VA_g_active_object + 0x4) = 0; G32(VA_g_item_drop_position + 0x118) = 0;
    uint32_t rec = find_object_list40(*(volatile uint16_t *)(uintptr_t)(obj + 0xe));   /* 0x34526 */
    if (rec == 0) return 0;
    if (!(*(volatile uint8_t *)(uintptr_t)(rec + 6) & 2))
        return exec_object_trigger_no_source(rec);                                     /* 0x35201 simple */

    /* tracked path 0x352b2 */
    uint16_t orig_id = *(volatile uint16_t *)(uintptr_t)(obj + 0xe);
    *(volatile uint16_t *)(uintptr_t)(obj + 0xe) = 0xc200;
    exec_object_trigger_no_source(rec);                                                /* 0x35201 */
    uint16_t buf[0x10];                                                                       /* the 0x20 scratch */
    int32_t cnt = collect_secondary_state_records_by_key(0xc200, 1, (uint32_t)(uintptr_t)buf); /* 0x34c97 */
    if (cnt == 0) return 0;
    uint32_t newobj = (uint32_t)G32(VA_g_map_objects_buffer) + (uint32_t)buf[0];
    *(volatile uint16_t *)(uintptr_t)(newobj + 0xe) = orig_id;
    if ((uint32_t)G32(VA_g_player_move_delta_z + 0xc) != 0)
        *(volatile uint8_t *)(uintptr_t)(newobj + 7) |= 0x80;
    return cnt;
}

/* dispatch_entry_command_trigger (0x34d75): the directional floor/face ENTRY-trigger dispatcher. `in->eax` = the
 * trigger record (param_1). READ-ONLY apart from deterministic global seeds + the lifted (read-only) flow pre-pass
 * walk_command_chain_flow 0x353c4 — so it's safe to live-swap (no cutscene hang). Two channels keyed on byte[p1+1]:
 *   type 3 (player position): the geom sector word[geom+4+word[p1+8]] must be flagged (byte[geom+9+sec]&1); scan the
 *     object-table header's category-A refs ({offset,count} @ +0x18/+0x1a) for one whose sector word[oth+8+ref] ==
 *     word[geom+0xc+sec], not skipped (byte[oth+2+ref]&8), and whose direction mask byte[oth+6+ref] admits the
 *     approach (dir_mask[byte[p1+0x1a]&3]); fire if its bbox is unset (word[oth+0xc+ref]==0) or the player x/z
 *     (word[p1+4]/+6) lie inside [+0xc..+0xe]x[+0x10..+0x12]; on fire (walk OK) seed 0x8a0fc/0x8a100/0x8a0f8.
 *   type 8 / 2 (use): the geom cell geom+word[p1+0xa] must be byte[+0x16]&0x10; scan category-B refs (@ +0x1c/+0x1e),
 *     match sector word[cell+0x14], gate fire on (byte[oth+6+ref]&1 ? type==8 : type==2); same bbox; seed 0x8a0fc=
 *     0x8a134=0. Returns the matched record (oth+ref) on fire, else 0. All ptrs runtime; seeds canon. */
int32_t dispatch_entry_command_trigger(const regs_t *in)
{
    static const uint8_t dir_mask[4] = { 0x01, 0x04, 0x02, 0x01 };
    uint32_t p1 = in->eax;
    uint8_t type = *(volatile uint8_t *)(uintptr_t)(p1 + 1);
    uint32_t geom = (uint32_t)G32(VA_g_map_geometry_buffer);
    uint32_t oth  = (uint32_t)G32(VA_g_object_table_header);

    if (type == 3) {
        uint32_t sec = *(volatile uint16_t *)(uintptr_t)(geom + 4 + *(volatile uint16_t *)(uintptr_t)(p1 + 8));
        if (!(*(volatile uint8_t *)(uintptr_t)(geom + 9 + sec) & 1)) return 0;
        int32_t c = (int32_t)*(volatile uint16_t *)(uintptr_t)(oth + 0x1a);
        if (c == 0) return 0;
        uint32_t pp = oth + *(volatile uint16_t *)(uintptr_t)(oth + 0x18);
        do {
            uint16_t ref = *(volatile uint16_t *)(uintptr_t)pp; pp += 2;
            if (*(volatile uint16_t *)(uintptr_t)(oth + 8 + ref) == *(volatile uint16_t *)(uintptr_t)(geom + 0xc + sec)
                && !(*(volatile uint8_t *)(uintptr_t)(oth + 2 + ref) & 8)
                && (*(volatile uint8_t *)(uintptr_t)(oth + 6 + ref)
                    & dir_mask[*(volatile uint8_t *)(uintptr_t)(p1 + 0x1a) & 3])) {
                uint32_t u4;
                if (*(volatile int32_t *)(uintptr_t)(oth + 0xc + ref) == 0) {
                    u4 = ref;
                } else {
                    uint32_t r = oth + ref;
                    int16_t px = *(volatile int16_t *)(uintptr_t)(p1 + 4), pz = *(volatile int16_t *)(uintptr_t)(p1 + 6);
                    if (*(volatile int16_t *)(uintptr_t)(r + 0xc) <= px && px <= *(volatile int16_t *)(uintptr_t)(r + 0xe)
                        && *(volatile int16_t *)(uintptr_t)(r + 0x10) <= pz && pz <= *(volatile int16_t *)(uintptr_t)(r + 0x12))
                        u4 = ref;   /* BUGFIX (in-game, desk-item pickup): the original KEEPS edi=oth+ref on the
                                     * player-inside path (add edi,ebx @0x34ded; the pop @0x34e0f discards the save;
                                     * the later ebx zeroing is scratch) — firer index = word[oth+ref+4], return =
                                     * oth+ref, IDENTICAL to the bbox-unset case. u4=0 fed word[oth+4] (= the header's
                                     * offset-to-first-record, e.g. 1390) to the walker as a chain index. */
                    else
                        continue;
                }
                if (rcw_walk_from_firer(*(volatile uint16_t *)(uintptr_t)(oth + 4 + u4), "trigA.dir", oth, u4) == 0) {
                    uint32_t a0fc = (uint32_t)*(volatile uint16_t *)(uintptr_t)(p1 + 8) + geom;
                    G32(VA_g_active_object + 0x4) = (int32_t)a0fc;
                    G32(VA_g_command_source_object) = 0;
                    G32(VA_g_active_object) = (int32_t)((uint32_t)*(volatile uint16_t *)(uintptr_t)(a0fc + 4) + geom);
                    return (int32_t)(oth + u4);
                }
                return 0;
            }
        } while (--c > 0);
        return 0;
    }
    if (type == 8 || type == 2) {
        uint32_t cell = geom + *(volatile uint16_t *)(uintptr_t)(p1 + 0xa);
        if (!(*(volatile uint8_t *)(uintptr_t)(cell + 0x16) & 0x10)) return 0;
        int32_t c = (int32_t)*(volatile uint16_t *)(uintptr_t)(oth + 0x1e);
        if (c == 0) return 0;
        uint32_t pp = oth + *(volatile uint16_t *)(uintptr_t)(oth + 0x1c);
        do {
            uint16_t ref = *(volatile uint16_t *)(uintptr_t)pp; pp += 2;
            if (*(volatile uint16_t *)(uintptr_t)(oth + 8 + ref) == *(volatile uint16_t *)(uintptr_t)(cell + 0x14)
                && !(*(volatile uint8_t *)(uintptr_t)(oth + 2 + ref) & 8)) {
                int fire = (*(volatile uint8_t *)(uintptr_t)(oth + 6 + ref) & 1) ? (type == 8) : (type == 2);
                if (fire) {
                    uint32_t u5;
                    if (*(volatile int32_t *)(uintptr_t)(oth + 0xc + ref) == 0) {
                        u5 = ref;
                    } else {
                        uint32_t r = oth + ref;
                        int16_t px = *(volatile int16_t *)(uintptr_t)(p1 + 4), pz = *(volatile int16_t *)(uintptr_t)(p1 + 6);
                        if (*(volatile int16_t *)(uintptr_t)(r + 0xc) <= px && px <= *(volatile int16_t *)(uintptr_t)(r + 0xe)
                            && *(volatile int16_t *)(uintptr_t)(r + 0x10) <= pz && pz <= *(volatile int16_t *)(uintptr_t)(r + 0x12))
                            u5 = ref;   /* BUGFIX: original keeps edi=oth+ref on player-inside (add edi,ebx @0x34ef0;
                                         * pop eax @0x34f12 discards the save) — the CAPTURED channel (trigA.face,
                                         * ax=word[hdr+4]=1390 masqueraded as a chain index; the desk-document bug). */
                        else
                            continue;
                    }
                    G32(VA_g_active_object + 0x4) = 0; G32(VA_g_player_move_delta_z + 0x8) = 0;
                    if (rcw_walk_from_firer(*(volatile uint16_t *)(uintptr_t)(oth + 4 + u5), "trigA.face", oth, u5) == 0)
                        return (int32_t)(oth + u5);
                    return 0;
                }
            }
        } while (--c > 0);
    }
    return 0;
}

/* dispatch_entry_command_trigger_b (0x34f5a): the category-B twin of 0x34d75 — same read-only match/dispatch but
 * three channels: (1) type 3 player-position, geom flag byte[geom+9+sec]&4, category refs @ header+0x30/+0x32,
 * dir_mask1{1,4,2,1}, seeds 0x8a0fc/0x8a100/0x8a0f8 (identical to 0x34d75's); (2) type 4, refs @ +0x38/+0x3a, match
 * the TARGET object word[[p1+0xe]+0xe] (no dir mask, no bbox), seeds g_command_source_object 0x8a100 = [p1+0xe],
 * 0x8a0fc=0x8a0f8=0; (3) any type != 7 (idx = type2->0/type1->1/type8->2/other->3), geom cell flag byte[geom+0x16+
 * cell]&0x20, refs @ +0x34/+0x36, dir_mask2{1,2,4,8}[idx], NO seeds. All channels bbox-gate the non-direct refs and
 * fire via the lifted read-only walk_command_chain_flow. Returns the matched record (oth+ref) or 0. */
int32_t dispatch_entry_command_trigger_b(const regs_t *in)
{
    static const uint8_t dm1[4] = { 1, 4, 2, 1 };
    static const uint8_t dm2[4] = { 1, 2, 4, 8 };
    uint32_t p1 = in->eax;
    if (p1 == 0) return 0;
    uint8_t type = *(volatile uint8_t *)(uintptr_t)(p1 + 1);
    uint32_t geom = (uint32_t)G32(VA_g_map_geometry_buffer);
    uint32_t oth  = (uint32_t)G32(VA_g_object_table_header);

    if (type == 3) {                                              /* channel 1 (always returns if flag set) */
        uint32_t sec = *(volatile uint16_t *)(uintptr_t)(geom + 4 + *(volatile uint16_t *)(uintptr_t)(p1 + 8));
        if (*(volatile uint8_t *)(uintptr_t)(geom + 9 + sec) & 4) {
            int32_t c = (int32_t)*(volatile uint16_t *)(uintptr_t)(oth + 0x32);
            if (c != 0) {
                uint32_t pp = oth + *(volatile uint16_t *)(uintptr_t)(oth + 0x30);
                do {
                    uint16_t ref = *(volatile uint16_t *)(uintptr_t)pp; pp += 2;
                    if (*(volatile uint16_t *)(uintptr_t)(oth + 8 + ref) == *(volatile uint16_t *)(uintptr_t)(geom + 0xc + sec)
                        && !(*(volatile uint8_t *)(uintptr_t)(oth + 2 + ref) & 8)
                        && (*(volatile uint8_t *)(uintptr_t)(oth + 6 + ref) & dm1[*(volatile uint8_t *)(uintptr_t)(p1 + 0x1a) & 3])) {
                        uint32_t u;
                        if (*(volatile int32_t *)(uintptr_t)(oth + 0xc + ref) == 0) u = ref;
                        else {
                            uint32_t r = oth + ref; int16_t px = *(volatile int16_t *)(uintptr_t)(p1 + 4), pz = *(volatile int16_t *)(uintptr_t)(p1 + 6);
                            if (*(volatile int16_t *)(uintptr_t)(r + 0xc) <= px && px <= *(volatile int16_t *)(uintptr_t)(r + 0xe)
                                && *(volatile int16_t *)(uintptr_t)(r + 0x10) <= pz && pz <= *(volatile int16_t *)(uintptr_t)(r + 0x12))
                                u = ref;   /* BUGFIX: original keeps edi=oth+ref on player-inside (@0x34fda). */
                            else continue;
                        }
                        if (rcw_walk_from_firer(*(volatile uint16_t *)(uintptr_t)(oth + 4 + u), "trigB.c1", oth, u) == 0) {
                            uint32_t a = (uint32_t)*(volatile uint16_t *)(uintptr_t)(p1 + 8) + geom;
                            G32(VA_g_active_object + 0x4) = (int32_t)a; G32(VA_g_command_source_object) = 0;
                            G32(VA_g_active_object) = (int32_t)((uint32_t)*(volatile uint16_t *)(uintptr_t)(a + 4) + geom);
                            return (int32_t)(oth + u);
                        }
                        return 0;
                    }
                } while (--c > 0);
            }
            return 0;
        }
    }
    if (type == 4) {                                              /* channel 2 (target-object match, no dir/bbox) */
        int32_t c = (int32_t)*(volatile uint16_t *)(uintptr_t)(oth + 0x3a);
        if (c != 0) {
            uint32_t pp = oth + *(volatile uint16_t *)(uintptr_t)(oth + 0x38);
            uint32_t targ = *(volatile uint32_t *)(uintptr_t)(p1 + 0xe);
            do {
                uint16_t ref = *(volatile uint16_t *)(uintptr_t)pp; pp += 2;
                if (*(volatile uint16_t *)(uintptr_t)(oth + 8 + ref) == *(volatile uint16_t *)(uintptr_t)(targ + 0xe)
                    && !(*(volatile uint8_t *)(uintptr_t)(oth + 2 + ref) & 8)) {
                    uint32_t r4 = oth + ref;
                    if (rcw_walk_from_firer(*(volatile uint16_t *)(uintptr_t)(r4 + 4), "trigB.c2", oth, ref) == 0) {
                        G32(VA_g_command_source_object) = (int32_t)targ; G32(VA_g_active_object + 0x4) = 0; G32(VA_g_active_object) = 0;
                        return (int32_t)r4;
                    }
                    return 0;
                }
            } while (--c > 0);
        }
        return 0;
    }
    if (type != 7) {                                             /* channel 3 (idx-keyed dir mask, no seeds) */
        int idx = (type == 2) ? 0 : (type == 1) ? 1 : (type == 8) ? 2 : 3;
        uint32_t cell = *(volatile uint16_t *)(uintptr_t)(p1 + 0xa);
        if (*(volatile uint8_t *)(uintptr_t)(geom + 0x16 + cell) & 0x20) {
            int32_t c = (int32_t)*(volatile uint16_t *)(uintptr_t)(oth + 0x36);
            if (c != 0) {
                uint32_t pp = oth + *(volatile uint16_t *)(uintptr_t)(oth + 0x34);
                do {
                    uint16_t ref = *(volatile uint16_t *)(uintptr_t)pp; pp += 2;
                    if (*(volatile uint16_t *)(uintptr_t)(oth + 8 + ref) == *(volatile uint16_t *)(uintptr_t)(geom + 0x14 + cell)
                        && !(*(volatile uint8_t *)(uintptr_t)(oth + 2 + ref) & 8)
                        && (*(volatile uint8_t *)(uintptr_t)(oth + 6 + ref) & dm2[idx])) {
                        uint32_t u;
                        if (*(volatile int32_t *)(uintptr_t)(oth + 0xc + ref) == 0) u = ref;
                        else {
                            uint32_t r = oth + ref; int16_t px = *(volatile int16_t *)(uintptr_t)(p1 + 4), pz = *(volatile int16_t *)(uintptr_t)(p1 + 6);
                            if (*(volatile int16_t *)(uintptr_t)(r + 0xc) <= px && px <= *(volatile int16_t *)(uintptr_t)(r + 0xe)
                                && *(volatile int16_t *)(uintptr_t)(r + 0x10) <= pz && pz <= *(volatile int16_t *)(uintptr_t)(r + 0x12))
                                u = ref;   /* BUGFIX: original keeps edi=oth+ref on player-inside (@0x3516d). */
                            else continue;
                        }
                        if (rcw_walk_from_firer(*(volatile uint16_t *)(uintptr_t)(oth + 4 + u), "trigB.c3", oth, u) == 0)
                            return (int32_t)(oth + u);
                        return 0;
                    }
                } while (--c > 0);
            }
        }
    }
    return 0;
}

/* twe_link_state: the special-sector LINK (water/lava) state machine of tick_world_effects (disasm 0x34678..0x347ce).
 * Detects the player entering/leaving a "link" sector (sector flags byte +0x17 bits 0x80/0x40; player Z g@0x90a92
 * vs the sector's [+2] / the linked sector's [+8]) and fires that sector's command trigger (fire_sector_trigger,
 * seed g@0x8a260=0x80008000) on the leave (g@0x8a0ec) and the fresh-enter (g@0x8a0e8/0x8a0e4/g@0x8a13c latch) edges.
 * Returning early == the original's `goto 0x347cf`. Sector/object ptrs are runtime (direct deref); state = canon. */
static void twe_link_state(const regs_t *in)
{
    /* 0x34678: leaving the previously-latched link sector? */
    if ((uint32_t)G32(VA_g_state_link_word_b) != 0 &&
        (uint16_t)(uint32_t)G32(VA_g_state_link_word_b) != (uint16_t)G16(VA_g_player_sector)) {
        uint32_t obj = (uint32_t)G32(VA_g_state_link_obj_ptr);
        if (obj != 0 && *(volatile uint8_t *)(uintptr_t)(obj + 3) == 0x13) {
            G32(VA_g_state_link_buf_ptr + 0x124) = 0x80008000;
            regs_t f = *in; f.esi = obj;
            fire_sector_trigger(&f);
        }
        G32(VA_g_state_link_word_b) = 0;
    }
    /* 0x346e0: re-detect the current sector's link */
    uint32_t ps = (uint32_t)(uint16_t)G16(VA_g_player_sector);
    if (ps == 0) return;                                             /* je 0x347cf */
    uint32_t sec = ps + (uint32_t)G32(VA_g_map_geometry_buffer);
    uint8_t fl = *(volatile uint8_t *)(uintptr_t)(sec + 0x17);
    if ((fl & 0xc0) == 0) { G32(VA_g_state_link_buf_ptr) = 0; return; }              /* 0x346d1: clear buf + exit */
    uint32_t cell; int exit_link;
    if ((fl & 0x80) == 0) {                                          /* 0x34737: simple-Z path */
        cell = sec;
        exit_link = ((int16_t)G16(VA_g_player_z) > *(volatile int16_t *)(uintptr_t)(sec + 2));
    } else if ((fl & 0x40) == 0) {                                   /* 0x34748: no Z-check, update with sector */
        cell = sec; exit_link = 0;
    } else {                                                         /* 0xc0: linked-sector path */
        uint32_t linked = *(volatile uint16_t *)(uintptr_t)(sec + 0x18);
        if (linked == 0) return;                                     /* je 0x347cf (no buf clear) */
        cell = linked + (uint32_t)G32(VA_g_map_geometry_buffer);
        int16_t pz = (int16_t)G16(VA_g_player_z);
        int16_t c8 = *(volatile int16_t *)(uintptr_t)(cell + 8);
        exit_link = (pz > c8) || ((int16_t)(pz + 2) < c8);
    }
    if (exit_link) {                                                 /* 0x346be: exit-the-link */
        if ((uint32_t)G32(VA_g_state_link_word_b) != 0) G32(VA_g_state_link_word_b) = 0xffffffff;
        G32(VA_g_state_link_buf_ptr) = 0; return;
    }
    /* 0x34748: enter/update link */
    if (cell == (uint32_t)G32(VA_g_state_link_buf_ptr)) return;                      /* je 0x347cf (unchanged cell) */
    G32(VA_g_state_link_buf_ptr) = cell;
    uint32_t fire_obj = 0; int have_obj = 0;
    uint16_t wa = (uint16_t)(uint32_t)G32(VA_g_state_link_word_a);
    if (wa != 0) {                                                   /* 0x3475d */
        if (wa != (uint16_t)G16(VA_g_player_sector)) {                          /* jne 0x34797 */
            G32(VA_g_state_link_word_a) = 0;
        } else {
            uint32_t obj = (uint32_t)G32(VA_g_state_link_obj_ptr);                   /* 0x34768 */
            if (obj == 0) {                                          /* je 0x34797 */
                G32(VA_g_state_link_word_a) = 0;
            } else if (*(volatile uint8_t *)(uintptr_t)(obj + 3) != 0x13) { /* jne 0x3478d */
                G32(VA_g_state_link_obj_ptr) = 0; G32(VA_g_state_link_word_a) = 0;                  /* 0x3478d -> 0x34797 */
            } else {                                                 /* 0x34777 */
                G32(VA_g_state_link_word_a) = 0; G32(VA_g_state_link_obj_ptr) = 0;
                fire_obj = obj; have_obj = 1;                        /* jmp 0x347bc */
            }
        }
    }
    if (!have_obj) {                                                 /* 0x347a1: find by sector's object id */
        uint32_t pg = ((uint32_t)(uint16_t)G16(VA_g_player_sector)) + (uint32_t)G32(VA_g_map_geometry_buffer);
        uint16_t id = *(volatile uint16_t *)(uintptr_t)(pg + 0x14);
        int32_t rec = find_object_record_by_id(id);
        if (rec == 0) return;                                        /* je 0x347cf */
        fire_obj = (uint32_t)rec;
    }
    /* 0x347bc: fire the entered sector's trigger */
    G32(VA_g_state_link_buf_ptr + 0x124) = 0x80008000;
    regs_t f = *in; f.esi = fire_obj;
    fire_sector_trigger(&f);
}

/* tick_world_effects (0x3464c): the per-frame .RAW active-effect orchestrator (called from tick_dynamic_entities
 * 0x42d74). (1) latch+clear the player speed-reduction request (g@0x89f54 -> g@0x89f58 / byte g@0x8a269); (2) tick
 * the particle system (lifted tick_particles 0x4b396) + the sector damage emitter (damage_player_from_emitter
 * 0x34579, BRIDGED — register-transparent, reads g_damage_emitter_ptr 0x8a120); (3) the water/lava LINK state
 * machine (twe_link_state); (4) zero *[0x71f40]; (5) walk the active-effect pool (head g@0x8a118; interleaved
 * node/record handle list — [node]=record chunk, [record]=next node) dispatching each effect through the per-frame
 * TICK table g_command_tick_dispatch_table (0x3088c) indexed by byte[record+4] (BRIDGED per-effect: the handler
 * reads the record in EAX; the table holds relocated host pointers so canon = entry-OBJ_DELTA) — a nonzero return
 * means the effect finished -> unlink_finished_effect (lifted 0x344f9). The not-finished branch RE-DEREFS *node for
 * the next link because a handler may relocate the chunk (DAS-pool handle discipline). (6) flush the command timer
 * queue (lifted tick_command_timer_queue 0x345e2, gated by g@0x89fbc) + the deferred command queue (lifted
 * process_deferred_command_queue 0x3484b; pool!=0 path always flushes it, pool==0 path only when g@0x89fbc). Returns
 * g@0x71f40 (pool!=0) / g@0x89fbc (pool==0), matching the original (caller ignores it). LIVE-SWAP ONLY (per-frame:
 * a bug crashes at frame 1; test in isolation via ROTH_LIFT=tick_world_effects). A cutscene fired from the link
 * trigger or an effect tick can nest the GDV SIGTRAP -> hang (same limitation as the standalone firers). */
int32_t tick_world_effects(const regs_t *in)
{
    /* (1) latch + clear the speed-reduction request */
    uint32_t req = (uint32_t)G32(VA_g_player_speed_reduction_request);
    G32(VA_g_player_speed_reduction_shift) = req;
    G32(VA_g_player_speed_reduction_request) = 0;
    G8(VA_g_command_chain_interrupt + 0x1) = (uint8_t)req;

    /* (2) particles + (conditional) damage emitter */
    tick_particles();
    if ((uint32_t)G32(VA_g_damage_emitter_ptr) != 0)
        damage_player_from_emitter();                          /* 0x34579 re-pointed (register-transparent) */

    /* (3) special-sector link state machine */
    twe_link_state(in);

    /* (4) 0x347cf: clear the per-frame scratch slot */
    uint32_t p71 = (uint32_t)G32(VA_g_anim_step_fn_table + 0x10);
    *(volatile uint32_t *)(uintptr_t)p71 = 0;

    /* (5) walk the active-effect pool */
    uint32_t pool = (uint32_t)G32(VA_g_active_effect_pool);
    if (pool != 0) {
        uint32_t node = pool;
        uint32_t prev_link = 0x8a118u + OBJ_DELTA;                  /* &g_active_effect_pool head slot (host addr) */
        while (node != 0) {
            uint32_t record = *(volatile uint32_t *)(uintptr_t)node;
            uint8_t  idx    = *(volatile uint8_t  *)(uintptr_t)(record + 4);
            uint32_t entry  = (uint32_t)G32(0x3088c + (uint32_t)idx * 4); /* handler_canon + OBJ_DELTA */
            uint32_t ret = firer_bridge(entry - OBJ_DELTA, record, idx, 0, prev_link, 0, 0, 0, 0, 0);
            if (ret != 0) {                                          /* effect finished: splice + free */
                unlink_finished_effect(node, prev_link);     /* sets *prev_link = next, frees node */
                node = *(volatile uint32_t *)(uintptr_t)prev_link;  /* 0x3480e: next via prev_link */
            } else {                                                /* 0x34816: re-deref *node (handle may relocate) */
                uint32_t rec2 = *(volatile uint32_t *)(uintptr_t)node;
                prev_link = rec2;
                node = *(volatile uint32_t *)(uintptr_t)rec2;       /* 0x3481a: next = *record */
            }
        }
        /* 0x34820 */
        if ((uint32_t)G32(VA_g_item_drop_position + 0x10) != 0)
            tick_command_timer_queue((uint32_t)G32(VA_g_item_drop_position + 0x10));
        process_deferred_command_queue();
        return (int32_t)p71;
    }
    /* 0x34835: pool empty -> timer + deferred both gated by g@0x89fbc */
    if ((uint32_t)G32(VA_g_item_drop_position + 0x10) != 0) {
        tick_command_timer_queue((uint32_t)G32(VA_g_item_drop_position + 0x10));
        process_deferred_command_queue();
    }
    return (int32_t)(uint32_t)G32(VA_g_item_drop_position + 0x10);
}

/* reserve_object_buffer_space (0x420e1): compact/grow the per-sector object buffer (g_map_objects_buffer @0x90aa4) to
 * reclaim the holes left by removed object lists (called from spawn when free space @0x85c28 < 0x12, and on save).
 * Bumps the table generation (g@0x911c7) + marks it dirty (g@0x911c5=0xff), then: alloc a scratch copy of the whole
 * buffer (game_heap_alloc, size word[buf]+3), copy the buffer into it ((word[buf]+3)>>2 dwords — ECX is preserved
 * through the alloc when pool checks are off), and rebuild the buffer IN PLACE from the scratch copy — re-packing each
 * sector's surviving object list (scratch count byte != 0) contiguously after the directory (buf + sector_count*2 + 2),
 * recomputing its directory offset; a count-0 list is dropped (dir entry -> 0), an already-0 dir entry is skipped.
 * Store the new total size (word[buf]) + free space (g@0x85c28 = g@0x85c2c - total), free the scratch copy, and rebuild
 * the GS-arena back-pointer caches (rebuild_pool_a_object_pointers). When the alloc fails, only the rebuild runs.
 * Reads list data from the SCRATCH copy (not buf) so the in-place re-pack can't corrupt a not-yet-moved list. Composes
 * the verified game_heap_alloc/game_heap_free/rebuild helpers; buffers are runtime (direct deref). STATIC ORACLE. */
void reserve_object_buffer_space(void)
{
    G32(VA_g_object_table_generation) += 1;                                              /* 0x420e1: generation++ */
    G8(VA_g_object_table_dirty) |= 0xff;                                           /* 0x420e7: table dirty */
    uint32_t buf = (uint32_t)G32(VA_g_map_objects_buffer);
    uint32_t sz  = *(volatile uint16_t *)(uintptr_t)buf;           /* word[buf] = current total size */
    uint32_t neu = game_heap_alloc((int32_t)(sz + 3));       /* 0x420ff: scratch copy block */
    if (neu != 0) {
        uint32_t ndw = (sz + 3) >> 2;                              /* 0x4210d: shr ecx,2 (ECX preserved = sz+3) */
        const volatile uint32_t *src = (const volatile uint32_t *)(uintptr_t)buf;
        volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)neu;
        for (uint32_t i = 0; i < ndw; i++) dst[i] = src[i];       /* 0x42110: rep movsd buf -> scratch */

        uint32_t sc  = (uint32_t)G32(VA_g_sector_count);                     /* 0x42114: sector count */
        uint32_t wr  = buf + sc * 2 + 2;                           /* 0x4211c: compaction write cursor (puVar7) */
        uint32_t ent = buf + 2;                                    /* 0x42120: directory entry cursor (edx) */
        for (uint32_t s = sc; s != 0; s--) {                       /* 0x42123: do { } while (--sc > 0) */
            uint16_t off = *(volatile uint16_t *)(uintptr_t)ent;
            if (off != 0) {                                        /* 0x42128: skip an already-empty slot */
                uint8_t cnt = *(volatile uint8_t *)(uintptr_t)(neu + off); /* 0x4212a: list count (scratch) */
                if (cnt == 0) {
                    *(volatile uint16_t *)(uintptr_t)ent = 0;      /* 0x4215a: drop the emptied list */
                } else {
                    *(volatile uint16_t *)(uintptr_t)ent = (uint16_t)(wr - buf); /* 0x4213b: new offset */
                    *(volatile uint8_t *)(uintptr_t)wr = cnt;       /* 0x42147 */
                    *(volatile uint8_t *)(uintptr_t)(wr + 1) = cnt; /* 0x42149 */
                    uint32_t rsrc = neu + off + 2;                 /* 0x42143: src = scratch + off + 2 */
                    wr += 2;                                       /* 0x4214c */
                    uint32_t nw = (uint32_t)cnt << 3;              /* 0x4214f: cnt*8 words */
                    for (uint32_t k = 0; k < nw; k++) {            /* 0x42152: rep movsw scratch -> buf */
                        *(volatile uint16_t *)(uintptr_t)wr = *(volatile uint16_t *)(uintptr_t)rsrc;
                        wr += 2; rsrc += 2;
                    }
                }
            }
            ent += 2;                                             /* 0x4215f */
        }
        uint32_t total = wr - buf;                                /* 0x4216b: edi - buf */
        *(volatile uint16_t *)(uintptr_t)buf = (uint16_t)total;    /* 0x4216d: word[buf] = total */
        G32(VA_g_object_buffer_free) = (int32_t)((uint32_t)G32(VA_g_object_buffer_free + 0x4) - total);  /* 0x42177: free = base - total */
        game_heap_free((uint8_t *)(uintptr_t)neu);          /* 0x4217d: free the scratch copy */
    }
    rebuild_pool_a_object_pointers();                       /* 0x42182: rebuild the back-ptr caches */
}

/* init_sector_object_state (0x4f1a0): reset the per-object state for every sector on level (re)load. EAX = geometry
 * base (0x90aa8), EDX = object base (0x90aa4). word[geom+4] points at a sector directory whose [-2] word is the
 * sector count; walk that many sector records (stride 0x1a from geom+word[geom+4]). Each sector's directory entry
 * (objects+2, +2 per sector) is a byte offset into the object base; a nonzero entry marks the sector geometry
 * (byte[sector+0x16]|=2) and, if the object header byte (count) is nonzero, clears word[obj+0xc] across that many
 * object slots (stride 0x10 from obj+2). The slot loop's terminator is a signed 8-bit dec (jg), so a header byte
 * >=0x81 clears only one slot. EAX is dead. */
void init_sector_object_state(uint32_t geom, uint32_t objects)
{
    uint32_t esi = geom;
    uint32_t edi = objects;
    uint32_t count = *(volatile uint16_t *)(uintptr_t)(esi + 4);
    G32(VA_g_sector_section_offset) = (int32_t)count;
    esi += count;
    count = *(volatile uint16_t *)(uintptr_t)(esi - 2);
    G32(VA_g_sector_count) = (int32_t)count;
    if (count == 0)
        return;
    edi += 2;
    do {
        if (*(volatile uint16_t *)(uintptr_t)edi != 0) {
            *(volatile uint8_t *)(uintptr_t)(esi + 0x16) |= 2;
            uint32_t eax = objects + *(volatile uint16_t *)(uintptr_t)edi;   /* edx (=objects) + word[edi] */
            uint8_t cl = *(volatile uint8_t *)(uintptr_t)eax;
            if (cl != 0) {
                eax += 2;
                do {
                    *(volatile uint16_t *)(uintptr_t)(eax + 0xc) = 0;
                    eax += 0x10;
                } while ((int8_t)--cl > 0);                          /* dec cl ; jg (signed) */
            }
        }
        esi += 0x1a;
        edi += 2;
    } while ((int32_t)--count > 0);                                 /* dec ecx ; jg */
}

/* object_has_active_trigger_link (0x30c1a): read-only predicate — does the object table hold a live trigger record
 * that targets the id this command record points at? EAX = command record. byte[rec+1] selects the channel: 4 =
 * "use" links (directory at objtbl+0x24/+0x26, type byte 0x1b, target id = word[*(rec+0xe)+0xe]); 3 = "contact"
 * links (directory at objtbl+0x20/+0x22, type byte 0x1a, target id = word[geom+link+0xc] where link follows
 * rec+8 -> geom record +4, gated on its flag word &0x8000). A list entry matches when its record's type byte ==
 * the channel type, the skip bit (byte[rec+2]&8) is clear, and word[rec+8] == the target id. Returns -1 on a hit,
 * 0 otherwise (or when the object table / list / link is absent). No writes, no calls. */
int32_t object_has_active_trigger_link(uint32_t rec)
{
    uint32_t esi = (uint32_t)G32(VA_g_object_table_header);                          /* object table base */
    if (esi == 0)
        return 0;
    uint8_t type = *(volatile uint8_t *)(uintptr_t)(rec + 1);
    if (type == 4) {                                               /* 0x30c7d "use" channel */
        uint32_t edi = esi + *(volatile uint16_t *)(uintptr_t)(esi + 0x24);
        uint32_t ecx = *(volatile uint16_t *)(uintptr_t)(esi + 0x26);
        if (ecx == 0)
            return 0;
        uint32_t ptr = *(volatile uint32_t *)(uintptr_t)(rec + 0xe);
        uint16_t dx = *(volatile uint16_t *)(uintptr_t)(ptr + 0xe);
        if (dx == 0)
            return 0;
        do {
            uint16_t bx = *(volatile uint16_t *)(uintptr_t)edi;
            if (*(volatile uint8_t *)(uintptr_t)(esi + bx + 3) == 0x1b &&
                !(*(volatile uint8_t *)(uintptr_t)(esi + bx + 2) & 8) &&
                *(volatile uint16_t *)(uintptr_t)(esi + bx + 8) == dx)
                return -1;
            edi += 2;
        } while ((int32_t)--ecx > 0);
        return 0;
    }
    if (type != 3)                                                 /* 0x30c39 -> ret 0 */
        return 0;
    /* 0x30c3b "contact" channel */
    uint32_t edi = esi + *(volatile uint16_t *)(uintptr_t)(esi + 0x20);
    uint32_t ecx = *(volatile uint16_t *)(uintptr_t)(esi + 0x22);
    if (ecx == 0)
        return 0;
    uint32_t edx = (uint32_t)G32(VA_g_map_geometry_buffer);                          /* geometry base */
    uint32_t e8 = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    uint32_t link = *(volatile uint16_t *)(uintptr_t)(edx + e8 + 4);
    if (!(*(volatile uint16_t *)(uintptr_t)(edx + link) & 0x8000))
        return 0;
    uint16_t dx = *(volatile uint16_t *)(uintptr_t)(edx + link + 0xc);
    do {
        uint16_t bx = *(volatile uint16_t *)(uintptr_t)edi;
        if (*(volatile uint8_t *)(uintptr_t)(esi + bx + 3) == 0x1a &&
            !(*(volatile uint8_t *)(uintptr_t)(esi + bx + 2) & 8) &&
            *(volatile uint16_t *)(uintptr_t)(esi + bx + 8) == dx)
            return -1;
        edi += 2;
    } while ((int32_t)--ecx > 0);
    return 0;
}

/* set_state_record_count (0x33c3e): latch the active command-state record count g@0x89f5c = EAX, then run the
 * full command-state reset init_loaded_object_table 0x33f0e [L] (zeroes the effect/timer/queue globals, stores
 * the two tick handler code-ptrs, and when the object table g@0x85c30 is non-null walks it). Returns the
 * residual EAX — which is ALWAYS the input: 0x33f0e's pre-pushal body is pure `mov [mem],imm` (no register
 * writes) and its tail is `popal; ret` on every path, so no proto extension is needed (re-point). */
int32_t set_state_record_count(uint32_t eax)
{
    G32(VA_g_state_record_list_count) = (int32_t)eax;
    init_loaded_object_table();                             /* re-point 0x33f0e (EAX preserved) */
    return (int32_t)eax;
}

/* tick_flash_lights (0x32324): per-frame light-flash effect. ESI=rec, cell=[rec+8]. A "flash pattern" is selected
 * by byte[cell+7] indexing the rodata pointer table @0x32310 (count @0x3230c); pattern[0] = the half-period, so
 * period2 = 2*pattern[0]; pattern[1..] = signed brightness deltas. The current phase word[rec+6] advances by the
 * frame step [0x85324]. The deltas are applied to byte[geom+off+0xb] for every light in the rec's list (count
 * word[rec+0xe], 4-byte entries {u16 geom-off, u8 base} from rec+0x12), with the resulting byte = base + delta.
 *  - HOLDING (byte[rec+5]&0x80): count word[rec+0xc] down by the step; on underflow drop the holding bit. ret 0.
 *  - byte[cell+6]&4 (CHASE): each light samples a SUCCESSIVE pattern byte (index advances per light, wraps at
 *    period); delta negated iff byte[cell+6]&1. ret 0.
 *  - else SINGLE SAMPLE: all lights share one delta (pattern[1 + phase/2]). When the phase wraps past period2:
 *      * !(cell[6]&0x20): one final emit at period2-1 then ret -1 + clear byte[cell+2]&0xde (effect finished);
 *      * cell[6]&0x20 & word[cell+0xa]!=0: latch the hold countdown (optionally scaled by word[0x85328]>>16 when
 *        cell[6]&2), set the holding bit, reset the phase, ret 0;
 *      * cell[6]&0x20 & word[cell+0xa]==0: re-base the phase and keep emitting. ret 0.
 *    Delta negated iff byte[cell+6]&1. Fully self-contained (no sub-calls). */
int32_t tick_flash_lights(uint32_t rec)
{
    uint32_t esi = rec;
    uint32_t cell = *(volatile uint32_t *)(uintptr_t)(esi + 8);

    if (*(volatile uint8_t *)(uintptr_t)(esi + 5) & 0x80) {         /* holding path (0x3245f) */
        uint16_t step = (uint16_t)(uint32_t)G32(VA_g_frame_time_scale);
        uint16_t cd = *(volatile uint16_t *)(uintptr_t)(esi + 0xc);
        *(volatile uint16_t *)(uintptr_t)(esi + 0xc) = (uint16_t)(cd - step);
        if (cd < step)                                             /* sub borrow -> jae not taken -> clear */
            *(volatile uint8_t *)(uintptr_t)(esi + 5) -= 0x80;
        return 0;
    }

    uint32_t patIdx = *(volatile uint8_t *)(uintptr_t)(cell + 7);
    if (patIdx >= (uint32_t)G32(VA_g_light_pattern_data_block + 0x3e)) {                        /* index out of range (0x323f8) */
        *(volatile uint8_t *)(uintptr_t)(cell + 2) &= 0xde;
        return -1;
    }
    uint32_t ebx = *(volatile uint32_t *)(uintptr_t)GADDR(0x32310 + patIdx * 4);   /* pattern ptr */
    uint32_t period2 = 2u * (uint32_t)*(volatile uint8_t *)(uintptr_t)ebx;
    uint32_t ecx = (uint32_t)*(volatile uint16_t *)(uintptr_t)(esi + 6) + (uint32_t)G32(VA_g_frame_time_scale);
    uint8_t c6 = *(volatile uint8_t *)(uintptr_t)(cell + 6);

    if (c6 & 4) {                                                  /* CHASE path (0x32405) */
        if (!(ecx < period2)) {
            ecx -= period2;
            if (!(ecx < period2))
                ecx = 0;
        }
        *(volatile uint16_t *)(uintptr_t)(esi + 6) = (uint16_t)ecx;
        uint32_t period = period2 >> 1;
        ecx >>= 1;
        int neg = (c6 & 1) != 0;
        int32_t ebp = *(volatile uint16_t *)(uintptr_t)(esi + 0xe);
        uint32_t lp = esi + 0x12;
        do {
            uint8_t dl = *(volatile uint8_t *)(uintptr_t)(ebx + ecx + 1);
            if (neg) dl = (uint8_t)(-(int)dl);
            ecx++;
            if (!(ecx < period))
                ecx = 0;
            uint16_t off = *(volatile uint16_t *)(uintptr_t)lp;
            uint8_t base = *(volatile uint8_t *)(uintptr_t)(lp + 2);
            lp += 4;
            uint32_t g = (uint32_t)G32(VA_g_map_geometry_buffer) + off;
            *(volatile uint8_t *)(uintptr_t)(g + 0xb) = (uint8_t)(base + dl);
        } while ((int32_t)--ebp > 0);
        return 0;
    }

    /* SINGLE-SAMPLE path (0x3236a) */
    int neg_return = 0;
    if (ecx >= period2) {
        if (!(c6 & 0x20)) {                                        /* 0x323b3 */
            neg_return = 1;
            ecx = period2 - 1;
        } else if (*(volatile uint16_t *)(uintptr_t)(cell + 0xa) == 0) {   /* 0x323a6 */
            ecx -= period2;
            if (!(ecx < period2))
                ecx = 1;
        } else {                                                  /* hold-set path (0x3237b) */
            uint32_t dxv = *(volatile uint16_t *)(uintptr_t)(cell + 0xa);
            if (c6 & 2)
                dxv = (dxv * (uint32_t)(uint16_t)G16(VA_g_frame_time_scale + 0x4)) >> 16;
            *(volatile uint16_t *)(uintptr_t)(esi + 0xc) = (uint16_t)dxv;
            *(volatile uint8_t *)(uintptr_t)(esi + 5) |= 0x80;
            *(volatile uint16_t *)(uintptr_t)(esi + 6) = 0;
            return 0;
        }
    }
    *(volatile uint16_t *)(uintptr_t)(esi + 6) = (uint16_t)ecx;    /* emit one sample (0x323b9) */
    ecx >>= 1;
    uint8_t dl = *(volatile uint8_t *)(uintptr_t)(ebx + ecx + 1);
    if (c6 & 1)
        dl = (uint8_t)(-(int)dl);
    int32_t cnt = *(volatile uint16_t *)(uintptr_t)(esi + 0xe);
    uint32_t lp = esi + 0x12;
    do {
        uint16_t off = *(volatile uint16_t *)(uintptr_t)lp;
        uint8_t base = *(volatile uint8_t *)(uintptr_t)(lp + 2);
        lp += 4;
        uint32_t g = (uint32_t)G32(VA_g_map_geometry_buffer) + off;
        *(volatile uint8_t *)(uintptr_t)(g + 0xb) = (uint8_t)(base + dl);
    } while ((int32_t)--cnt > 0);
    if (!neg_return)
        return 0;
    *(volatile uint8_t *)(uintptr_t)(cell + 2) &= 0xde;            /* effect finished (0x323f8) */
    return -1;
}

/* resolve_object_template_record (0x1de59): look up a DBASE100 template record by id and return its sprite/template
 * index. EAX = id; if 0 and the "current selection" pointer g@0x81044 is set, the id is taken from sext(word[*]).
 * The id is latched to g@0x89fa8. With dbase base edx=g@0x81e1c, an id past the record count [edx+0x10] (unsigned)
 * yields 0. Otherwise the per-id offset table g@0x81e20 gives the record (rec = edx + table[id]); a zero offset
 * yields 0. dword[rec+4] is latched to g@0x81f02; the result is sext(word[rec+2]) unless that is < 0x200 (a
 * non-template/builtin marker) in which case 0. Read-only apart from the two global latches. */
int32_t resolve_object_template_record(uint32_t eax)
{
    if (eax == 0 && (uint32_t)G32(VA_g_selected_item_primary) != 0) {
        uint32_t p = (uint32_t)G32(VA_g_selected_item_primary);
        eax = (uint32_t)(int32_t)*(volatile int16_t *)(uintptr_t)p;   /* movsx word[*0x81044] */
    }
    G32(VA_g_last_item_record) = (int32_t)eax;
    if (eax == 0)
        return 0;
    uint32_t edx = (uint32_t)G32(VA_g_dbase100_base);                          /* dbase base */
    if (eax > *(volatile uint32_t *)(uintptr_t)(edx + 0x10))        /* cmp eax,[edx+0x10] ; jbe in-range */
        return 0;
    uint32_t ebx = (uint32_t)G32(VA_g_dbase100_inventory_table);                          /* per-id offset table */
    uint32_t off = *(volatile uint32_t *)(uintptr_t)(ebx + eax * 4);
    if (off == 0)
        return 0;
    uint32_t rec = off + edx;
    G32(VA_g_object_template_flags) = *(volatile int32_t *)(uintptr_t)(rec + 4);
    int32_t t = *(volatile int16_t *)(uintptr_t)(rec + 2);          /* movsx word[rec+2] */
    if (t < 0x200)
        return 0;
    return t;
}

/* alloc_particle (0x4b485): claim a free slot from the 0x20-entry particle array at 0x911e4 (stride 0x34). A slot
 * is free when its byte[+0x30]&1 is clear. On success the slot is zeroed (call 0x4b360 = memset(slot,0,0x34) with
 * fill 0), pushed onto the singly-linked free/active list (head g@0x91864, link at slot[+0]), marked busy
 * (byte[+0x30]=1) and returned. Returns 0 when every slot is busy. */
uint32_t alloc_particle(void)
{
    uint32_t ecx = (uint32_t)GADDR(VA_g_sfx_query_result_count + 0x4);                        /* particle array base */
    for (int idx = 0; idx < 0x20; idx++) {
        if (!(*(volatile uint8_t *)(uintptr_t)(ecx + 0x30) & 1)) {
            memset((void *)(uintptr_t)ecx, 0, 0x34);                /* call 0x4b360 (edx=0) */
            uint32_t head = (uint32_t)G32(VA_g_particle_pool);
            *(volatile uint32_t *)(uintptr_t)ecx = head;            /* slot->next = head */
            G32(VA_g_particle_pool) = (int32_t)ecx;                            /* head = slot */
            *(volatile uint8_t *)(uintptr_t)(ecx + 0x30) = 1;       /* mark busy */
            return ecx;
        }
        ecx += 0x34;
    }
    return 0;                                                       /* no free slot */
}

/* ---- GS object-record arena (per-object "secondary state" pool, addressed through the 0x90bea selector) ---- */
/* The object arena base g@0x90aa4 doubles as the GS segment base, so gs:[off] == *(arena+off). Per sector (count
 * g@0x91df8) a word header at gs:[2], gs:[4], ... gives a list offset (0 = none); each list is {u8 count, u8 cap}
 * then `count` 0x10-byte records from list+2. A record's byte[+9] low 2 bits route a back-pointer to it into one
 * of two obj3 caches by (word[+0xc]-1): bit2 -> g@0x90fe4, else bit1 -> g@0x91e08 (id 0 -> skipped). */

/* rebuild_pool_a_object_pointers (0x42cf9): rebuild both back-pointer caches from the live arena (called after any
 * relocation of the arena). Self-contained (no sub-calls). */
void rebuild_pool_a_object_pointers(void)
{
    uint32_t arena = (uint32_t)G32(VA_g_map_objects_buffer);
    uint32_t ecx = (uint32_t)G32(VA_g_sector_count);                         /* sector count (outer, do-while) */
    uint32_t hdr = 2;
    do {
        uint16_t slot = *(volatile uint16_t *)(uintptr_t)(arena + hdr);
        if (slot != 0) {
            uint32_t rec = arena + slot;
            uint32_t count = *(volatile uint8_t *)(uintptr_t)rec;
            if (count != 0) {
                rec += 2;
                do {
                    uint8_t f = *(volatile uint8_t *)(uintptr_t)(rec + 9);
                    if (f & 3) {
                        int32_t idx = (int32_t)*(volatile uint16_t *)(uintptr_t)(rec + 0xc) - 1;
                        if (f & 2) {
                            if (idx >= 0)
                                *(volatile uint32_t *)((uintptr_t)GADDR(VA_g_dynamic_entity_table) + (uint32_t)idx) = rec;
                        } else if (f & 1) {
                            if (idx >= 0)
                                *(volatile uint32_t *)((uintptr_t)GADDR(VA_g_state_pool_a_records + 0x4) + (uint32_t)idx) = rec;
                        }
                    }
                    rec += 0x10;
                } while ((int32_t)--count > 0);
            }
        }
        hdr += 2;
    } while ((int32_t)--ecx > 0);
}

/* remove_secondary_state_record (0x42056): remove one record from an object's GS-arena record list and shift the
 * tail down. EAX = the list header offset (gs:[eax] = count byte), EBX = the offset of the record to drop. Marks
 * the arena dirty (g@0x911c5=0xff, g@0x911c7++). The record index q = ((ebx-eax)&0xffff)/0x10; if q+1-count >= 0
 * (signed 8-bit; removing at/after the last) just decrement the count. Otherwise (and count!=0) move the (count-
 * (q+1)) trailing records down by 0x10, updating each moved record's back-pointer cache (byte[+9]&2 -> g@0x90fe4,
 * else &1 -> g@0x91e08, at word[+0xc]-1) to its new address, then decrement the count. EAX dead. Self-contained. */
void remove_secondary_state_record(uint32_t eax, uint32_t ebx)
{
    G8(VA_g_object_table_dirty) |= 0xff;
    G32(VA_g_object_table_generation) += 1;
    uint32_t arena = (uint32_t)G32(VA_g_map_objects_buffer);
    uint16_t si = (uint16_t)eax;
    uint8_t count = *(volatile uint8_t *)(uintptr_t)(arena + si);
    uint16_t q = (uint16_t)((uint16_t)(ebx - eax) / 0x10);
    uint8_t al = (uint8_t)((uint8_t)((uint8_t)q + 1) - count);
    if ((int8_t)al < 0 && count != 0) {                            /* shift the trailing records down */
        uint32_t edi = (uint16_t)ebx;
        uint32_t esi = edi + 0x10;
        uint8_t n = (uint8_t)(-(int8_t)al);
        if (n != 0) {
            esi += arena; edi += arena;
            do {
                uint8_t f = *(volatile uint8_t *)(uintptr_t)(esi + 9);
                if (f & 3) {
                    int32_t idx = (int32_t)*(volatile uint16_t *)(uintptr_t)(esi + 0xc) - 1;
                    if (f & 2) {
                        if (idx >= 0)
                            *(volatile uint32_t *)((uintptr_t)GADDR(VA_g_dynamic_entity_table) + (uint32_t)idx) = edi;
                    } else if (idx >= 0) {
                        *(volatile uint32_t *)((uintptr_t)GADDR(VA_g_state_pool_a_records + 0x4) + (uint32_t)idx) = edi;
                    }
                }
                memcpy((void *)(uintptr_t)edi, (void *)(uintptr_t)esi, 0x10);   /* rep movsd 4 */
                esi += 0x10; edi += 0x10;
            } while (--n != 0);
        }
    } else if ((int8_t)al < 0) {
        return;                                                    /* count == 0 -> ret without dec */
    }
    *(volatile uint8_t *)(uintptr_t)(arena + si) -= 1;             /* dec count */
}

/* release_object_secondary_state (0x42014): drop record EBX from object EAX(objid)'s secondary-state list. The
 * object's list-header slot is word gs:[(objid-g@0x91dfc)/0xd + 2]; remove_secondary_state_record(slot, ebx) does
 * the removal, and if the list is now empty (count gs:[slot]==0) the object's "has secondary state" geometry flag
 * (byte[geom+objid+0x16] bit1) is cleared. Composes the lifted remover; EAX dead. */
void release_object_secondary_state(uint32_t eax, uint32_t ebx)
{
    uint32_t objid = eax & 0xffff;
    uint32_t arena = (uint32_t)G32(VA_g_map_objects_buffer);
    uint16_t q = (uint16_t)((uint16_t)(objid - (uint32_t)G32(VA_g_sector_section_offset)) / 0xd);
    uint16_t slot = *(volatile uint16_t *)(uintptr_t)(arena + q + 2);
    remove_secondary_state_record(slot, ebx);
    if (*(volatile uint8_t *)(uintptr_t)(arena + slot) == 0) {
        uint32_t g = (uint32_t)G32(VA_g_map_geometry_buffer) + objid;
        *(volatile uint8_t *)(uintptr_t)(g + 0x16) &= 0xfd;
    }
}

/* insert_object_arena_gap (0x4222b): open a CX-byte gap at GS offset EDX in the object arena to make room for a new
 * record. Marks dirty (g@0x911c7++, g@0x911c5=0xff). First bumps every sector header (gs:[2],gs:[4],... count
 * g@0x91df8) that points at/after the insertion point by CX. Then, if there is data above the insertion point
 * (cursor gs:[0] > EDX), shift [EDX,cursor) up by CX (the original uses a std backward word copy == memmove) and
 * rebuild the back-pointer caches (0x42cf9 ✓); either way advance the cursor by CX and shrink the free space
 * g@0x85c28 by CX. Does not load GS itself (caller's GS). */
void insert_object_arena_gap(uint32_t ecx, uint32_t edx)
{
    G32(VA_g_object_table_generation) += 1;
    G8(VA_g_object_table_dirty) |= 0xff;
    uint32_t arena = (uint32_t)G32(VA_g_map_objects_buffer);
    uint16_t cx = (uint16_t)ecx;
    uint16_t dx = (uint16_t)edx;
    uint32_t cnt = (uint32_t)G32(VA_g_sector_count);
    uint32_t hdr = 2;
    do {
        if (*(volatile uint16_t *)(uintptr_t)(arena + hdr) >= dx)   /* cmp ; jb not taken (unsigned >=) */
            *(volatile uint16_t *)(uintptr_t)(arena + hdr) += cx;
        hdr += 2;
    } while ((int32_t)--cnt > 0);

    uint16_t cursor = *(volatile uint16_t *)(uintptr_t)(arena + 0);
    int32_t toMove = (int32_t)(uint32_t)cursor - (int32_t)edx;       /* sub ecx,edx ; jle skips */
    if (toMove > 0) {
        uint32_t bytes = ((uint32_t)toMove >> 1) * 2;               /* shr ecx,1 (word count) */
        uint32_t src_low = (uint32_t)cursor - bytes;
        memmove((void *)(uintptr_t)(arena + src_low + cx), (void *)(uintptr_t)(arena + src_low), bytes);
        *(volatile uint16_t *)(uintptr_t)(arena + 0) += cx;
        G32(VA_g_object_buffer_free) -= (int32_t)ecx;
        rebuild_pool_a_object_pointers();
    } else {
        G32(VA_g_object_buffer_free) -= (int32_t)ecx;
        *(volatile uint16_t *)(uintptr_t)(arena + 0) += cx;
    }
}

/* alloc_object_record_slot (0x42199): reserve the next record slot in object EAX(objid)'s secondary-state list and
 * return its GS offset. The object's header is gs:[(objid-g@0x91dfc)/0xd + 2]. If 0 (no list yet): carve a fresh
 * list at the arena cursor gs:[0] (header=cursor, gs:[cursor]=0x101 {count1,cap1}, cursor += 0x12, free space
 * g@0x85c28 -= 0x12) and return cursor+2. Else the list is at `slot`, packed word gs:[slot] = {count=lo, cap=hi};
 * the new record goes at edx = (count<<4)+slot+2. If count<cap (signed) just bump the count byte; otherwise bump
 * the cap, open a 0x10 gap there (insert_object_arena_gap ✓ — done BEFORE the count bump so the rebuild still sees
 * the old count), then bump the count. Returns edx. Loads GS itself. */
uint32_t alloc_object_record_slot(uint32_t eax)
{
    uint32_t arena = (uint32_t)G32(VA_g_map_objects_buffer);
    uint32_t objid = eax & 0xffff;
    uint16_t q = (uint16_t)((uint16_t)(objid - (uint32_t)G32(VA_g_sector_section_offset)) / 0xd);
    uint32_t hdrOff = (uint32_t)q + 2;
    uint16_t header = *(volatile uint16_t *)(uintptr_t)(arena + hdrOff);
    uint32_t edx;
    if (header == 0) {                                             /* 0x42202: carve a new list */
        uint16_t cursor = *(volatile uint16_t *)(uintptr_t)(arena + 0);
        *(volatile uint16_t *)(uintptr_t)(arena + hdrOff) = cursor;
        *(volatile uint16_t *)(uintptr_t)(arena + cursor) = 0x101;
        edx = (uint32_t)cursor + 2;
        *(volatile uint16_t *)(uintptr_t)(arena + 0) += 0x12;
        G32(VA_g_object_buffer_free) -= 0x12;
    } else {
        uint32_t slot = header;
        uint16_t packed = *(volatile uint16_t *)(uintptr_t)(arena + slot);
        uint8_t count = (uint8_t)packed;
        uint8_t cap = (uint8_t)(packed >> 8);
        edx = ((uint32_t)count << 4) + slot + 2;
        if ((int8_t)count < (int8_t)cap) {                        /* room (jl signed) */
            *(volatile uint8_t *)(uintptr_t)(arena + slot) += 1;
        } else {                                                  /* full -> grow */
            *(volatile uint16_t *)(uintptr_t)(arena + slot) += 0x100;   /* cap++ */
            insert_object_arena_gap(0x10, edx);
            *(volatile uint16_t *)(uintptr_t)(arena + slot) += 1;       /* count++ */
        }
    }
    return edx;
}

/* alloc_object_record_ensuring_space (0x42189): if the arena free space g@0x85c28 is below one record (0x12),
 * grow the whole object buffer first (reserve_object_buffer_space 0x420e1 — BRIDGED, it reallocs via the host heap
 * so it is non-idempotent and only the no-grow path is differential-checked), then reserve the slot
 * (alloc_object_record_slot ✓). Returns the slot's GS offset. */
uint32_t alloc_object_record_ensuring_space(uint32_t eax)
{
    if ((uint32_t)G32(VA_g_object_buffer_free) < 0x12)
        reserve_object_buffer_space();                      /* 0x420e1 re-pointed (§6.5 in-game; grow path) */
    return alloc_object_record_slot(eax);
}

/* add_secondary_state_record (0x42c72): move a 0x10-byte secondary-state record into an object's pool. EDX = the
 * source record, EAX = the object to remove it from, ECX = the object to add it to. The record is first copied to a
 * stack buffer (so a later arena relocation can't lose it), removed from EAX's list (release_object_secondary_state
 * ✓, by its arena offset), then a fresh slot is reserved for ECX (alloc_object_record_ensuring_space ✓). On success
 * the object's geometry flag byte[geom+ecx+0x16]|=2 is set, the saved record is copied into the new slot, and its
 * back-pointer cache (byte[+9]&2 -> g@0x90fe4 else &1 -> g@0x91e08, at word[+0xc]-1) points at the new location.
 * EAX dead. Does not load GS itself (the nested allocators do). */
void add_secondary_state_record(uint32_t eax, uint32_t ecx, uint32_t edx)
{
    uint32_t arena = (uint32_t)G32(VA_g_map_objects_buffer);
    uint8_t local[0x10];
    memcpy(local, (void *)(uintptr_t)edx, 0x10);                   /* save the source record */
    uint32_t src_off = edx - arena;
    release_object_secondary_state(eax, src_off);           /* call 0x42014 */
    uint32_t slot = alloc_object_record_ensuring_space(ecx);/* call 0x42189 */
    if (slot == 0)
        return;
    uint32_t g = (uint32_t)G32(VA_g_map_geometry_buffer) + ecx;
    *(volatile uint8_t *)(uintptr_t)(g + 0x16) |= 2;
    uint32_t rec = arena + slot;
    memcpy((void *)(uintptr_t)rec, local, 0x10);                   /* place it in the new slot */
    uint8_t f = *(volatile uint8_t *)(uintptr_t)(rec + 9);
    if (f & 2) {
        int32_t idx = (int32_t)*(volatile uint16_t *)(uintptr_t)(rec + 0xc) - 1;
        if (idx >= 0)
            *(volatile uint32_t *)((uintptr_t)GADDR(VA_g_dynamic_entity_table) + (uint32_t)idx) = rec;
    } else if (f & 1) {
        int32_t idx = (int32_t)*(volatile uint16_t *)(uintptr_t)(rec + 0xc) - 1;
        if (idx >= 0)
            *(volatile uint32_t *)((uintptr_t)GADDR(VA_g_state_pool_a_records + 0x4) + (uint32_t)idx) = rec;
    }
}

/* apply_floorceil_move_to_group (0x3427b): drive a group of cells toward a new floor (= 2*word[esi+0xa]), the
 * floor/ceiling sibling of apply_floor_move_to_group 0x3423e. ESI=command record, EBP={u16 count, members@+4}.
 * Per member cell (offset into geom 0x90aa8): if the old floor (word[cell+2]) is above the target, lower it and
 * carry the player/objects (apply_cell_move_to_player flags=1 ✓, delta -> g@0x8a12c). byte[esi+6]&2 clamps the
 * cell's signed step byte[cell+0xc] up to -1 when below it (and byte[cell+0xa]&1 clear). byte[esi+6]&1 also drives
 * a LINKED ceiling cell (word[cell+0x18]): set its floor + (if its ceiling word[link+8] is above target) lower it
 * and carry (flags=2). The g@0x8a12c delta is read as a u16 by the carry, so (per 0x3423e) it is computed cleanly
 * as target-old; its residual high half is dead. EAX dead. */
void apply_floorceil_move_to_group(uint32_t esi, uint32_t ebp)
{
    uint16_t bx = (uint16_t)(2u * (uint32_t)*(volatile uint16_t *)(uintptr_t)(esi + 0xa));
    uint8_t *geom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    int32_t count = (int32_t)*(volatile uint16_t *)(uintptr_t)ebp;
    ebp += 4;
    do {
        uint32_t edx = *(volatile uint16_t *)(uintptr_t)ebp;
        int16_t old = *(volatile int16_t *)(geom + edx + 2);
        if (old > (int16_t)bx) {                                    /* cmp ax,bx; jle skip (signed) */
            *(volatile uint16_t *)(geom + edx + 2) = bx;
            G32(VA_g_player_move_delta_z) = (int32_t)((int32_t)(uint32_t)bx - (int32_t)(uint32_t)(uint16_t)old);
            apply_cell_move_to_player(1, edx);
        }
        if (*(volatile uint8_t *)(uintptr_t)(esi + 6) & 2) {
            if (!(*(volatile uint8_t *)(geom + edx + 0xa) & 1) &&
                (int8_t)*(volatile uint8_t *)(geom + edx + 0xc) < -1)
                *(volatile uint8_t *)(geom + edx + 0xc) = 0xff;
        }
        if (*(volatile uint8_t *)(uintptr_t)(esi + 6) & 1) {
            uint32_t ecx = *(volatile uint16_t *)(geom + edx + 0x18);
            if (ecx != 0) {
                *(volatile uint16_t *)(geom + ecx + 2) = bx;
                int16_t cold = *(volatile int16_t *)(geom + ecx + 8);
                if (cold > (int16_t)bx) {
                    *(volatile uint16_t *)(geom + ecx + 8) = bx;
                    G32(VA_g_player_move_delta_z) = (int32_t)((int32_t)(uint32_t)bx - (int32_t)(uint32_t)(uint16_t)cold);
                    apply_cell_move_to_player(2, edx);
                }
            }
        }
        ebp += 2;
    } while (--count > 0);
}

/* run_command_chain (0x305f0): the spine's seeding wrapper — the trigger's pusha-framed body that every command
 * trigger falls into (EDI = chain head record). Seeds the executor context (g_warp_dest 0x8a260 sentinel,
 * g_command_active_chain 0x8a10c=edi, g_command_chain_result 0x8a138=0, interrupt 0x8a268=0, 0x89f64=0), runs the
 * flow/condition pre-pass (walk_command_chain_flow ✓); if it gates (nonzero) the chain is blocked -> exit. Else
 * latch g_command_pending_rot 0x8a264 = word[edi+0xa], clear the deferred-queue head (*0x71f40), and unless the
 * head is skip-bit ([edi+2]&8) run the executor (execute_command_chain ✓) then flush the deferred queue
 * (process_deferred_command_queue 0x3484b ✓). Exit 0x304a9: g_command_active_chain[0x8a134]=0, return -1.
 * Composed entirely of already-byte-verified spine pieces. (The fall-through pusha/popa frame
 * is supplied by the asm stub run_command_chain_orig for the oracle's original run — it can't be call_orig'd.) */
int32_t run_command_chain(uint32_t edi)
{
    G32(VA_g_state_link_buf_ptr + 0x124) = (int32_t)0x80008000u;                            /* g_warp_dest sentinel */
    G32(VA_g_anim_step_mode + 0x8) = (int32_t)edi;                                    /* g_command_active_chain */
    G32(VA_g_player_move_delta_z + 0xc) = 0;                                               /* g_command_chain_result */
    G8(VA_g_command_chain_interrupt) = 0;                                                /* g_command_chain_interrupt */
    G32(VA_g_if_item_list_count) = 0;
    uint16_t ax = *(volatile uint16_t *)(uintptr_t)(edi + 4);
    if (walk_command_chain_flow(ax) == 0) {                  /* not blocked by the flow pre-pass */
        G16(VA_g_state_link_buf_ptr + 0x128) = *(volatile uint16_t *)(uintptr_t)(edi + 0xa);    /* g_command_pending_rot */
        *(uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_anim_step_fn_table + 0x10) = 0;        /* clear the deferred-queue head (STORED PTR) */
        if (!(*(volatile uint8_t *)(uintptr_t)(edi + 2) & 8)) {     /* head not skip-bit */
            execute_command_chain(edi);                     /* the executor loop */
            process_deferred_command_queue();               /* flush the deferred queue (0x3484b) */
        }
    }
    G32(VA_g_player_move_delta_z + 0x8) = 0;                                               /* 0x304a9 exit */
    return -1;
}

/* cmd_run_indexed_object_command (base 0x40, 0x304b8): a SECOND copy of the executor inner loop — run a
 * sub-command chain starting from the index in word[rec+6] (instead of the main chain's NEXT). Per record:
 * resolve idx-1 -> record via g_object_ptr_array 0x8a0d8; unless its skip bit ([rec+2]&8) is set, dispatch it
 * through the 0x30780 table (BRIDGED, ESI=record) — keeping the LAST handler's return in EBX and OR-ing every
 * return into g_command_chain_result 0x8a138; stop on a handler setting g_command_chain_interrupt 0x8a268, on a
 * Delay Timer (base 0x12), or NEXT==0. Returns EBX (the last dispatched handler's EAX, or 0 if none dispatched
 * / word[rec+6]==0). NB unlike execute_command_chain it does NOT touch g_command_prev/next_active, has NO
 * done-tail (no SFX/door/dialogue), and does NOT reset the interrupt — so it's a plain ESI->EAX handler. */
int32_t cmd_run_indexed_object_command(uint32_t rec)
{
    uint32_t ebx = 0;                                                /* sub ebx,ebx */
    uint16_t ax = *(volatile uint16_t *)(uintptr_t)(rec + 6);        /* ax = word[rec+6]; and eax,0xffff */
    if (ax == 0) return 0;                                           /* je 0x30519 -> ret eax(=0) */
    uint32_t edi = rec;
    for (;;) {
        if (ax == 0) break;                                         /* sub ax,1; jb 0x30510 (ret ebx) */
        uint32_t idx = (uint32_t)(uint16_t)(ax - 1);                /* eax &= 0xffff */
        uint32_t base_arr = *(uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_object_ptr_array);   /* esi=[0x8a0d8]; esi=[esi] */
        uint32_t r = *(uint32_t *)(uintptr_t)(base_arr + idx * 4);            /* esi=[esi+eax*4] */
        edi = r;                                                    /* edi = esi */
        if (!(*(volatile uint8_t *)(uintptr_t)(edi + 2) & 8)) {     /* test [edi+2],8; jne skip_dispatch */
            uint32_t bs = *(volatile uint8_t *)(uintptr_t)(r + 3) & 0x7fu;    /* COMMAND_BASE & 0x7f */
            uint32_t handler = *(uint32_t *)(uintptr_t)(0x30780u + OBJ_DELTA + bs * 4);  /* table[base] */
            regs_t io; memset(&io, 0, sizeof io);
            io.va = handler; io.esi = r; io.edi = r;
#ifndef ROTH_STANDALONE
            call_orig(&io);                                         /* call [ebx*4+0x30780] — BRIDGED */
#else
            io.eax = rawcmd_dispatch_30780(io.va, io.esi);   /* the 0x30780 table -> lifted cmd bodies */
#endif
            edi = r;                                                /* pop edi */
            ebx = io.eax;                                           /* mov ebx,eax */
            G32(VA_g_player_move_delta_z + 0xc) = (int32_t)((uint32_t)G32(VA_g_player_move_delta_z + 0xc) | io.eax);   /* g_command_chain_result |= eax */
            if (G8(VA_g_command_chain_interrupt) != 0) break;                           /* interrupt -> 0x30510 (ret ebx) */
        }
        if (*(volatile uint8_t *)(uintptr_t)(edi + 3) == 0x12) break;        /* Delay Timer stops */
        if (*(volatile uint16_t *)(uintptr_t)(edi + 4) != 0) {               /* follow NEXT */
            ax = *(volatile uint16_t *)(uintptr_t)(edi + 4);
            continue;
        }
        break;
    }
    return (int32_t)ebx;                                            /* 0x30510: eax = ebx; ret */
}

/* cmd_modify_count (base 0x1e, 0x31c31): arm the step params for a referenced Count-family command, then re-run
 * that command's handler in-place to apply one step. (1) g_anim_step_delta 0x8a108 = word[rec+0xa]; (2)
 * g_anim_step_mode 0x8a104 = (byte[rec+6]&3)+1 (1..3, never the default inc 0); (3) resolve the target by 1-based
 * index word[rec+8] via resolve_command_by_index; (4) iff the target's base is a Count family (0x15/0x1f/0x21/
 * 0x22) dispatch it through the 0x30780 table (BRIDGED, nested RAW dispatch, ESI=target); (5) the nested handler
 * sets g_command_chain_interrupt 0x8a268 while "in progress" / clears it when done — if it's still set, clear it
 * and return -1 (no queue); else push a follow-up {NEXT_idx, 0} into the deferred queue *0x71f40 (<=16) and
 * return -1. Clears mode+interrupt and returns 0 on index-0 / resolve-fail / non-Count / queue-full. THIS fn
 * consumes (never sets) the interrupt. Returns -1 acted / 0. */
int32_t cmd_modify_count(uint32_t rec)
{
    G32(VA_g_anim_step_mode + 0x4) = (int32_t)(uint32_t)*(volatile uint16_t *)(uintptr_t)(rec + 0xa);   /* delta */
    G32(VA_g_anim_step_mode) = (int32_t)(((uint32_t)*(volatile uint8_t *)(uintptr_t)(rec + 6) & 3u) + 1u);  /* mode 1..3 */
    uint16_t index = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    if (index == 0) return 0;                                          /* 0x31cca ret 0 */
    uint32_t target = (uint32_t)resolve_command_by_index(index);
    if (target != 0) {
        uint8_t tb = *(volatile uint8_t *)(uintptr_t)(target + 3);
        if (tb == 0x15 || tb == 0x21 || tb == 0x22 || tb == 0x1f) {     /* Count family -> dispatch */
            uint32_t bs = (uint32_t)tb & 0x7fu;
            uint32_t handler = *(uint32_t *)(uintptr_t)(0x30780u + OBJ_DELTA + bs * 4);
            regs_t io; memset(&io, 0, sizeof io);
            io.va = handler; io.esi = target; io.edi = target;
#ifndef ROTH_STANDALONE
            call_orig(&io);                                            /* nested RAW dispatch — BRIDGED */
#else
            io.eax = rawcmd_dispatch_30780(io.va, io.esi);   /* the 0x30780 table -> lifted cmd bodies */
#endif
            uint32_t queued = (io.eax & 0xffff0000u)                   /* mov ax,[target+4]: low=NEXT, high=ret>>16 */
                            | (uint32_t)*(volatile uint16_t *)(uintptr_t)(target + 4);
            if (G8(VA_g_command_chain_interrupt) != 0) {                                    /* nested still in-progress */
                G8(VA_g_command_chain_interrupt) = 0;
                return -1;                                            /* 0x31cb3 */
            }
            uint32_t edi = (uint32_t)G32(VA_g_anim_step_fn_table + 0x10);                     /* deferred queue (STORED PTR) */
            uint32_t cnt = *(uint32_t *)(uintptr_t)edi;
            if (cnt < 0x10) {                                          /* not full -> push */
                *(uint32_t *)(uintptr_t)edi = cnt + 1;
                *(uint32_t *)(uintptr_t)(edi + cnt * 8 + 4) = queued;
                *(uint32_t *)(uintptr_t)(edi + cnt * 8 + 8) = 0;
                G8(VA_g_command_chain_interrupt) = 0;
                return -1;                                            /* 0x31cb3 */
            }
            /* full -> fall into the clear-ret0 tail */
        }
    }
    G8(VA_g_command_chain_interrupt) = 0;                                                   /* 0x31cb9 clear interrupt + mode */
    G32(VA_g_anim_step_mode) = 0;
    return 0;                                                          /* 0x31cca */
}

/* cmd_open_door (base 0x2f, 0x33a69): resolve a door sector record, then register a door swing. The record is
 * resolved one of three ways: (key=word[rec+8] != 0) find_raw_state_record(key) then find_face_record(that
 * offset's low16) [both LIFTED]; (key==0, g_active_object_secondary 0x8a0fc != 0) the secondary's buffer-
 * relative offset = secondary - g_map_geometry_buffer 0x90aa8 (both STORED PTRs); (key==0, secondary==0)
 * find_face_record(g_active_object 0x8a0f8). Any resolve miss -> ret 0. Then call register_door_swing 0x3d54b
 * (BRIDGED — the door subsystem: FS-selector geometry + door pool 0x8b3f8, NOT the active-effect list) with
 * eax=record, ebx=word[rec+0xa], ecx=byte[rec+7], edx=(word[rec+0x10]<<16)|word[rec+0xe], edi=word[rec+0xc].
 * register_door_swing returns nonzero only when it registers a NEW swing -> cmd ret -1; if a door already exists
 * for the sector it returns 0 -> cmd ret 0. (register only reads di/bx as 16-bit, so the args' high words are
 * don't-care.) */
int32_t cmd_open_door(uint32_t rec)
{
    uint32_t eax;
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    if (key != 0) {
        eax = (uint32_t)find_raw_state_record(key);            /* 0x4f52b */
        if (eax == 0) return 0;
        eax = (uint32_t)find_face_record((uint16_t)eax);       /* 0x4f567 (arg = low16 of prior offset) */
        if (eax == 0) return 0;
    } else {
        eax = (uint32_t)G32(VA_g_active_object + 0x4);                                 /* g_active_object_secondary (STORED PTR) */
        if (eax != 0) {
            eax -= (uint32_t)G32(VA_g_map_geometry_buffer);                            /* -> buffer-relative offset */
        } else {
            eax = (uint32_t)G32(VA_g_active_object);                             /* g_active_object */
            if (eax == 0) return 0;
            eax = (uint32_t)find_face_record((uint16_t)eax);   /* 0x4f567 */
            if (eax == 0) return 0;
        }
    }
    uint32_t ebx = *(volatile uint16_t *)(uintptr_t)(rec + 0xa);
    uint32_t ecx = *(volatile uint8_t *)(uintptr_t)(rec + 7);
    uint32_t edx = ((uint32_t)*(volatile uint16_t *)(uintptr_t)(rec + 0x10) << 16)
                 | (uint32_t)*(volatile uint16_t *)(uintptr_t)(rec + 0xe);
    uint32_t edi = *(volatile uint16_t *)(uintptr_t)(rec + 0xc);
    /* register_door_swing 0x3d54b — direct-C. Proto param order is
     * (eax_ax, ecx, ebx_in, edx_params, edi_param, fs, gs); the leaf self-loads FS=[0x852c8]
     * (door-graph) + GS=[0x852cc] (vertex graph, new-door geometry path only), so resolve both
     * via g_os_sel_base — the same idiom test_doors.c uses for the direct register_door_swing
     * calls. register returns EAX!=0 only when it registers a NEW swing -> cmd ret -1. */
    uint32_t fs = g_os_sel_base ? g_os_sel_base((uint16_t)G16(VA_g_surface_record_selector)) : 0;
    uint32_t gs = g_os_sel_base ? g_os_sel_base((uint16_t)G16(VA_g_vertex_selector)) : 0;
    uint32_t r = register_door_swing(eax, ecx, ebx, edx, edi, fs, gs);
    return (r != 0) ? -1 : 0;
}

/* cmd_spawn_object_adv (base 0x3c, 0x30d10): the largest handler — spawn/configure an object, projectile, or
 * un-hide a pre-placed object. EDI tracks the SOURCE object. key=word[rec+8]: 0xffff -> synthesize a source at
 * g@0x89fac from the player position (g@0x90a8e/96/92), clear rec[6]&2; else resolve_command_objects(key) -> the
 * first object (g@0x90aa4 + word[descbuf]); count 0 -> no-object exit. Resolve the template via
 * resolve_object_template_record 0x1de59 (BRIDGED); 0 -> no-object exit. Then three paths:
 *  - PROJECTILE (g@0x81f02&0x20): build a 3-dword descriptor from the source, optionally compute the player
 *    bearing (compute_player_object_bearing 0x30389 ✓), then spawn_object_projectile_at_player 0x42300 (rec[6]&0x40)
 *    or spawn_player_projectile_flagged 0x422ec — both BRIDGED (edx=descriptor, eax=word[rec+0xa]).
 *  - HIDE/UNHIDE existing (rec[6]&2): early-out if the object already is this template / is busy; else free its
 *    slot (g@0x91e08[]/g@0x91e00) and clear its active bit -> the shared configure tail.
 *  - SPAWN-NEW (else): copy the source's 5 dwords into the descriptor, locate_sector_at_position 0x3ee4b (BRIDGED,
 *    eax/edx/ebx = source pos), alloc_object_record_ensuring_space 0x42189 (BRIDGED, eax=sector) -> new object;
 *    copy the descriptor's 4 dwords in -> the shared configure tail.
 * SHARED CONFIGURE TAIL: clear obj[9] bits, latch g@0x8a264/0x8a260 from rec[0xe], mark_object_trigger_links
 * 0x30bb7 ✓ on rec[0xc], set obj[4]=template/obj[0xe]=rec[0xc]/clear obj[7] hidden bit, and (g@0x81f02&0x80) call
 * spawn_object_from_das_resource 0x302e0 (BRIDGED, eax=obj/edx=template/ebx=8). rec[6]&0x10 -> [rec+2]|=8; ret -1.
 * NO-OBJECT exit: rec[6]&4 clear -> g@0x8a268=2; ret 0. Oracle covers the no-object exit (resolve returns 0); the
 * deep spawn/projectile/DAS paths are verified in-game (live-swap). The bridge register inputs were each confirmed
 * from the callee entries. */
int32_t cmd_spawn_object_adv(uint32_t rec)
{
    uint8_t descbuf[0x190];
    uint32_t desc = (uint32_t)(uintptr_t)descbuf;
    uint32_t edi;
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    if (key == 0xffff) {                                            /* 0x30cde: synthesize a source at g@0x89fac */
        uint32_t s = (uint32_t)(uintptr_t)GADDR(VA_g_item_drop_position);
        *(volatile uint16_t *)(uintptr_t)(s + 0x0) = (uint16_t)G16(VA_g_player_x);
        *(volatile uint16_t *)(uintptr_t)(s + 0x2) = (uint16_t)G16(VA_g_player_y);
        *(volatile uint16_t *)(uintptr_t)(s + 0xa) = (uint16_t)G16(VA_g_player_z);
        *(volatile uint8_t  *)(uintptr_t)(s + 0x7) = 0;
        *(volatile uint16_t *)(uintptr_t)(s + 0x4) = 0x200;
        *(volatile uint8_t *)(uintptr_t)(rec + 6) &= 0xfd;
        edi = s;
    } else {
        int32_t cnt = resolve_command_objects(key, 0xc8, desc);
        if (cnt == 0) goto no_object;                              /* 0x30e8e */
        edi = (uint32_t)G32(VA_g_map_objects_buffer) + *(uint16_t *)descbuf;       /* first object ptr */
    }
    uint32_t tmpl;                                                 /* 0x30d40: resolve the template */
    tmpl = (uint32_t)resolve_object_template_record(         /* 0x1de59 re-pointed (§6.5 in-game) */
        *(volatile uint16_t *)(uintptr_t)(rec + 0xa));
    if (tmpl == 0) goto no_object;
    uint16_t cx = (uint16_t)tmpl;

    if (G8(VA_g_object_template_flags) & 0x20) {                                      /* ===== PROJECTILE path (0x30ea4) ===== */
        G32(VA_g_command_source_object) = (int32_t)edi;
        uint16_t de = *(volatile uint16_t *)(uintptr_t)(rec + 0xe);
        if (de != 0) { G32(VA_g_state_link_buf_ptr + 0x128) = (int32_t)(uint32_t)de; G32(VA_g_state_link_buf_ptr + 0x124) = *(volatile int32_t *)(uintptr_t)edi; }
        *(uint32_t *)(descbuf + 0x0) = *(volatile uint32_t *)(uintptr_t)(edi + 0x0);
        *(uint32_t *)(descbuf + 0x4) = *(volatile uint32_t *)(uintptr_t)(edi + 0x4);
        *(uint32_t *)(descbuf + 0x8) = *(volatile uint32_t *)(uintptr_t)(edi + 0x8);
        *(uint16_t *)(descbuf + 0xc) = 0xffff;
        uint16_t a = *(volatile uint16_t *)(uintptr_t)(rec + 0xa);
        if (*(volatile uint8_t *)(uintptr_t)(rec + 6) & 0x40) {     /* 0x30ee5: bearing then 0x42300 */
            descbuf[6] = (uint8_t)compute_player_object_bearing(desc);
            *(uint16_t *)(descbuf + 0xc) = 0xffff;
            if (a != 0) {
                G32(VA_g_dos_dta_name + 0x62) = 0;
                spawn_object_projectile_at_player(a, desc);  /* 0x42300 re-pointed (§6.5 in-game) */
            }
        } else {                                                   /* 0x30f17: 0x422ec */
            *(uint16_t *)(descbuf + 0xc) = 0xffff;
            if (a != 0) {
                G32(VA_g_dos_dta_name + 0x62) = 0;
                spawn_player_projectile_flagged(a, desc);   /* 0x422ec re-pointed (§6.5 in-game) */
            }
        }
        if (*(volatile uint8_t *)(uintptr_t)(rec + 6) & 0x10) *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 8;
        return -1;
    }

    uint32_t obj;
    if (*(volatile uint8_t *)(uintptr_t)(rec + 6) & 2) {           /* ===== HIDE/UNHIDE existing (0x30d68) ===== */
        obj = edi;
        if (!(*(volatile uint8_t *)(uintptr_t)(edi + 7) & 0x80))
            if (*(volatile uint16_t *)(uintptr_t)(edi + 4) == cx) goto no_object;
        if (*(volatile uint8_t *)(uintptr_t)(edi + 9) & 2) goto no_object;
        if (*(volatile uint8_t *)(uintptr_t)(edi + 9) & 1) {       /* free the object's slot */
            int32_t bx = (int32_t)(uint32_t)*(volatile uint16_t *)(uintptr_t)(edi + 0xc) - 1;
            if (bx >= 0) {
                *(volatile uint32_t *)(uintptr_t)((uint32_t)(uintptr_t)GADDR(VA_g_state_pool_a_records + 0x4) + (uint32_t)bx) = 0;
                G32(VA_g_state_pool_a_count) = (int32_t)G32(VA_g_state_pool_a_count) - 1;
            }
            *(volatile uint8_t *)(uintptr_t)(edi + 9) &= 0xfe;
            *(volatile uint16_t *)(uintptr_t)(edi + 0xc) = 0;
        }
        /* -> shared configure tail */
    } else {                                                       /* ===== SPAWN-NEW (0x30db3) ===== */
        *(uint32_t *)(descbuf + 0x0)  = *(volatile uint32_t *)(uintptr_t)(edi + 0x0);
        *(uint32_t *)(descbuf + 0x4)  = *(volatile uint32_t *)(uintptr_t)(edi + 0x4);
        *(uint32_t *)(descbuf + 0x8)  = *(volatile uint32_t *)(uintptr_t)(edi + 0x8);
        *(uint32_t *)(descbuf + 0xc)  = *(volatile uint32_t *)(uintptr_t)(edi + 0xc);
        *(uint32_t *)(descbuf + 0x10) = *(volatile uint32_t *)(uintptr_t)(edi + 0x10);
        uint32_t sector;
        sector = locate_sector_at_position(                 /* 0x3ee4b re-pointed (§6.5 in-game) */
            *(volatile uint16_t *)(uintptr_t)(edi + 0x0), *(volatile uint16_t *)(uintptr_t)(edi + 0x2),
            *(volatile uint16_t *)(uintptr_t)(edi + 0xa), 0);
        if (sector == 0) goto no_object;
        uint32_t off;
        off = alloc_object_record_ensuring_space(sector);   /* 0x42189 re-pointed (§6.5 in-game) */
        if (off == 0) goto no_object;
        obj = (uint32_t)G32(VA_g_map_objects_buffer) + off;                        /* new object ptr */
        G32(VA_g_command_source_object) = (int32_t)obj;
        *(volatile uint32_t *)(uintptr_t)(obj + 0x0) = *(uint32_t *)(descbuf + 0x0);   /* copy 4 dwords desc->new */
        *(volatile uint32_t *)(uintptr_t)(obj + 0x4) = *(uint32_t *)(descbuf + 0x4);
        *(volatile uint32_t *)(uintptr_t)(obj + 0x8) = *(uint32_t *)(descbuf + 0x8);
        *(volatile uint32_t *)(uintptr_t)(obj + 0xc) = *(uint32_t *)(descbuf + 0xc);
    }

    /* ===== SHARED CONFIGURE TAIL (0x30e27) ===== */
    *(volatile uint8_t *)(uintptr_t)(obj + 9) &= 0xd2;
    uint16_t de = *(volatile uint16_t *)(uintptr_t)(rec + 0xe);
    if (de != 0) { G32(VA_g_state_link_buf_ptr + 0x128) = (int32_t)(uint32_t)de; G32(VA_g_state_link_buf_ptr + 0x124) = *(volatile int32_t *)(uintptr_t)obj; }
    uint16_t dc = *(volatile uint16_t *)(uintptr_t)(rec + 0xc);
    if (dc != 0) mark_object_trigger_links(obj, dc);
    *(volatile uint16_t *)(uintptr_t)(obj + 0x4) = cx;
    *(volatile uint16_t *)(uintptr_t)(obj + 0xe) = dc;
    *(volatile uint8_t *)(uintptr_t)(obj + 0x7) &= 0x7f;
    *(volatile uint16_t *)(uintptr_t)(obj + 0xc) = 0;
    if (G8(VA_g_object_template_flags) & 0x80)                                        /* 0x302e0 re-pointed (§6.5 in-game; body threads ESI=obj) */
        spawn_object_from_das_resource(obj, tmpl, 8);       /* (obj=EAX, id=DX, variant=EBX) */
    if (*(volatile uint8_t *)(uintptr_t)(rec + 6) & 0x10) *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 8;
    return -1;

no_object:                                                         /* 0x30e8e */
    if (!(*(volatile uint8_t *)(uintptr_t)(rec + 6) & 4)) G8(VA_g_command_chain_interrupt) = 2;
    return 0;
}

/* point_to_wall_distance_sq (0x3e03f): squared distance from the player to a wall. The wall record (EAX = its
 * buffer offset) is read through the geometry selector 0x90be8 (== g_map_geometry_buffer 0x90aa8): vtxA =
 * word[rec], vtxB = word[rec+2]. Each vertex's packed (x,y) coords come through the sector-geom selector
 * 0x90bec (== g_sector_geom_base 0x90aac): A = dword[vtxA+8], B = dword[vtxB+8]. The query point is the player
 * (low = word[0x90a8e] x, high = the upper half of dword[0x90a94] = word[0x90a96] y). Tail-calls the lifted
 * point_segment_distance_sq with radius = DX. Returns dist^2 / the far sentinel in EAX. Read-only. The 16-bit
 * `gs:[bx]` addressing wraps offsets at 16 bits (faithful via the uint16 casts). */
uint32_t point_to_wall_distance_sq(uint32_t rec, uint16_t radius)
{
    uint8_t *geom  = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);   /* gs1 = g_geometry_selector base */
    uint8_t *sgeom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_sector_geom_base);   /* gs2 = sector-geom selector base */
    uint16_t vtxA = *(volatile uint16_t *)(uintptr_t)(geom + (uint16_t)rec);
    uint16_t vtxB = *(volatile uint16_t *)(uintptr_t)(geom + (uint16_t)(rec + 2));
    uint32_t a = *(volatile uint32_t *)(uintptr_t)(sgeom + (uint16_t)(vtxA + 8));
    uint32_t b = *(volatile uint32_t *)(uintptr_t)(sgeom + (uint16_t)(vtxB + 8));
    uint32_t point = ((uint32_t)G32(VA_g_player_z + 0x2) & 0xffff0000u) | (uint16_t)G16(VA_g_player_x);
    return point_segment_distance_sq(radius, a, b, point);
}

/* cmd_apply_damage (base 0x33, 0x320e6): radius/blast damage to the player. radius = word[rec+0xa]; if 0,
 * apply with no falloff. Else compute dist^2: from the source object (g_command_source_object 0x8a100) to
 * the player ((obj.x-px)^2+(obj.y-py)^2) if there is one, else from the active wall/face (g_active_face
 * 0x8a0fc, offset = ptr - g_map_geometry_buffer) via point_to_wall_distance_sq(2*radius+0x1c) (its 0x7ff00
 * sentinel = out of range). In range (dist^2 < (2*radius)^2) -> falloff = 0x100*dist^2/(2*radius)^2 and
 * apply_damage_to_player(base byte[rec+7], type byte[0x8a26a], falloff). Always sets the skip bit on
 * flags&0x10 and returns -1. */
int32_t cmd_apply_damage(uint32_t rec)
{
    uint32_t src = (uint32_t)G32(VA_g_command_source_object);
    uint16_t radius = *(volatile uint16_t *)(uintptr_t)(rec + 0xa);
    uint32_t falloff = 0;
    int do_damage = 1;
    if (radius != 0) {
        uint32_t sq2r = (uint32_t)(radius * 2) * (uint32_t)(radius * 2);
        if (src != 0) {                                              /* distance from the source object */
            int16_t ddx = (int16_t)(*(volatile uint16_t *)(uintptr_t)src       - G16(VA_g_player_x));
            int16_t ddy = (int16_t)(*(volatile uint16_t *)(uintptr_t)(src + 2)  - G16(VA_g_player_y));
            uint32_t dist2 = (uint32_t)((int32_t)ddx * ddx + (int32_t)ddy * ddy);
            if (dist2 >= sq2r) do_damage = 0;
            else falloff = (uint32_t)(((uint64_t)0x100u * dist2) / sq2r);
        } else {                                                     /* distance from the active wall/face */
            uint32_t face = (uint32_t)G32(VA_g_active_object + 0x4);
            if (face == 0) do_damage = 0;
            else {
                uint32_t walloff = face - (uint32_t)G32(VA_g_map_geometry_buffer);
                uint32_t dist2 = point_to_wall_distance_sq(walloff, (uint16_t)(radius * 2 + 0x1c));
                if (dist2 > 0x7ff00u || dist2 >= sq2r) do_damage = 0;
                else falloff = (uint32_t)(((uint64_t)0x100u * dist2) / sq2r);
            }
        }
    }
    if (do_damage)
        apply_damage_to_player(*(volatile uint8_t *)(uintptr_t)(rec + 7), G8(VA_g_command_chain_interrupt + 0x2), falloff);
    if (*(volatile uint8_t *)(uintptr_t)(rec + 6) & 0x10)
        *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x8;
    return -1;
}

/* cmd_face_emits_damage (base 0x35, 0x30f83): per-face hazard damage. If rec[2]&0x21 -> NOP (ret 0). Else
 * gather the faces for word[rec+8] (gather_faces_by_id); for each, find_face_record(match) then
 * point_to_wall_distance_sq(2*radius+0x1c); on the FIRST in-range face (dist^2 <= 0x7ff00),
 * apply_damage_to_player(base byte[rec+7], type 0, falloff 0), set the skip bit on flags&0x10, return -1.
 * No face gathered -> ret 0; faces gathered but none in range -> ret -1. */
int32_t cmd_face_emits_damage(uint32_t rec)
{
    if (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 0x21) return 0;   /* -> 0x30ab0 NOP */
    uint16_t outbuf[0xc8];
    uint16_t id = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    uint32_t count = (uint32_t)gather_faces_by_id(id, 0xc8, (uint32_t)(uintptr_t)outbuf) & 0xffffu;
    if (count == 0) return 0;
    for (uint32_t k = 0; k < count; k++) {
        int32_t face = find_face_record(outbuf[2 + k]);
        uint32_t dist2 = point_to_wall_distance_sq((uint32_t)face, (uint16_t)(*(volatile uint16_t *)(uintptr_t)(rec + 0xa) * 2 + 0x1c));
        if (dist2 <= 0x7ff00u) {                                      /* first in-range face -> apply + stop */
            apply_damage_to_player(*(volatile uint8_t *)(uintptr_t)(rec + 7), 0, 0);
            if (*(volatile uint8_t *)(uintptr_t)(rec + 6) & 0x10)
                *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x8;
            return -1;
        }
    }
    return -1;
}

/* alloc_active_effect (0x32910): the shared allocator behind the ~10 effect-registering command handlers.
 * Collects the geometry group for `key` (collect_connected_geometry_group if `flag`, else geom_find_matches)
 * into a local list, sizes an effect record = align4(2*(count+2) + size), allocs it from the DAS-cache pool
 * heap (alloc_effect_record 0x34464 — BRIDGED, it is the heap boundary), and copies the collected list into
 * the record at +size. Returns the record (chunk) ptr, or 0 if nothing matched / the alloc failed. Verified
 * with the heap-snapshot harness (a lifted-pool_create pool staged at g_das_cache_heap_handle 0x85c3c). */
uint32_t alloc_active_effect(uint16_t key, uint32_t size, uint32_t flag)
{
    uint16_t matches[0xc8];                                          /* the 0x190-byte local list */
    uint32_t count;
    if (flag != 0)
        count = (uint32_t)collect_connected_geometry_group(key, 0xc8, (uint32_t)(uintptr_t)matches) & 0xffffu;
    else
        count = (uint32_t)geom_find_matches(key, 0xc8, (uint8_t *)matches) & 0xffffu;
    if (count == 0) return 0;                                        /* 0x3295f */
    uint32_t total = (2u * (count + 2u) + size + 3u) & ~3u;          /* align4(2*(count+2)+size) */
    uint32_t chunk = alloc_effect_record(total);             /* 0x34464 re-pointed */
    if (chunk == 0) return 0;
    memcpy((void *)(uintptr_t)(chunk + size), matches, ((count + 3u) >> 1) * 4u);   /* rep movsd: (count+3)/2 dwords */
    return chunk;
}

/* cmd_light_switch (base 0x02, 0x33b94): toggle a light and register/refresh its brightness-ramp effect.
 * Find this command's existing effect (find_active_effect type 2, keyed to the record); if none, register a
 * new one (alloc_active_effect for the connected geometry group word[rec+0xa], flag=1 -> the connected-flood
 * collector), store the record back-ptr at chunk[8], and mark the record registered ([rec+2]|=0x20). Then
 * toggle: brightness step byte[rec+7] (negated unless flags&8) added to [rec+0xc], flip the direction bit
 * (rec[6]^8) and the toggle bit (rec[2]^2), and stamp the command base (rec[3]) into chunk[4]. Returns -1
 * (acted) / 0 (alloc failed). The first effect REGISTRANT lifted; alloc_active_effect bridges the heap. */
int32_t cmd_light_switch(uint32_t rec)
{
    uint32_t eff = find_active_effect(2, rec);
    if (eff == 0) {
        uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 0xa);
        eff = alloc_active_effect(key, 0xc, 1);
        if (eff == 0) return 0;
        *(volatile uint32_t *)(uintptr_t)(eff + 8) = rec;            /* chunk[8] = record back-ptr */
        *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x20;          /* mark registered */
    }
    uint8_t dl = *(volatile uint8_t *)(uintptr_t)(rec + 7);
    if (!(*(volatile uint8_t *)(uintptr_t)(rec + 6) & 0x8))
        dl = (uint8_t)(0u - dl);                                    /* neg dl */
    *(volatile uint8_t *)(uintptr_t)(rec + 0xc) += dl;
    *(volatile uint8_t *)(uintptr_t)(rec + 6) ^= 0x8;
    *(volatile uint8_t *)(uintptr_t)(rec + 2) ^= 0x2;
    *(volatile uint8_t *)(uintptr_t)(eff + 4) = *(volatile uint8_t *)(uintptr_t)(rec + 3);   /* chunk[4] = base */
    return -1;
}

/* cmd_change_floor_texture (base 0x0a/0x0b, 0x32626): the MINIMAL effect registrant. If already registered
 * ([rec+2]&0x21) -> ret 0. Else register an effect for the sector group word[rec+8] (alloc_active_effect,
 * size 0xc, flag=0 -> the flat geom_find_matches collector), link the record back-ptr (chunk[8]=rec), mark
 * registered ([rec+2]|=0x20), stamp the command base (rec[3]) into chunk[4]. Returns -1 / 0 (alloc failed).
 * The tick handler (tick[0x0a] 0x33229) then advances the floor texture each frame. */
int32_t cmd_change_floor_texture(uint32_t rec)
{
    if (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 0x21) return 0;
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    uint32_t eff = alloc_active_effect(key, 0xc, 0);          /* flag=0 -> geom_find_matches */
    if (eff == 0) return 0;
    *(volatile uint32_t *)(uintptr_t)(eff + 8) = rec;
    *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x20;
    *(volatile uint8_t *)(uintptr_t)(eff + 4) = *(volatile uint8_t *)(uintptr_t)(rec + 3);
    return -1;
}

/* cmd_scroll_sector_texture (base 0x0e, 0x32473): register a sector texture-scroll effect for the group key
 * word[rec+9] (note +9). alloc_active_effect size 0xc, flag = (rec[2]>>2)&1 (record bit 0x04 -> connected
 * flood, else flat). Link + mark + stamp base. Returns -1 / 0. */
int32_t cmd_scroll_sector_texture(uint32_t rec)
{
    if (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 0x21) return 0;
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 9);
    uint32_t flag = ((uint32_t)*(volatile uint8_t *)(uintptr_t)(rec + 2) >> 2) & 1u;
    uint32_t eff = alloc_active_effect(key, 0xc, flag);
    if (eff == 0) return 0;
    *(volatile uint32_t *)(uintptr_t)(eff + 8) = rec;
    *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x20;
    *(volatile uint8_t *)(uintptr_t)(eff + 4) = *(volatile uint8_t *)(uintptr_t)(rec + 3);
    return -1;
}

/* cmd_change_lighting (base 0x1d, 0x33ac4): register/refresh a brightness-ramp effect. Find this command's
 * existing effect (find_active_effect type 0x1d); if none, register one (alloc_active_effect key word[rec+8],
 * size 0xc, flag (rec[2]>>2)&1) + link + mark. Then compute the ramp into chunk[6]: step cl = byte[rec+7]
 * (negated if rec[2]&2); if cl is negative store dx = 0xff00 | (-cl) (a sign-magnitude-ish 16-bit) else dx =
 * cl; if rec[6]&1 flip rec[2]^2; stamp base into chunk[4]. (The found path's dx pre-load before 0x33afc is
 * dead — the ramp compute zeroes dx first — so the alloc and found paths share the same live logic.) -1/0. */
int32_t cmd_change_lighting(uint32_t rec)
{
    uint32_t eff = find_active_effect(0x1d, rec);
    if (eff == 0) {
        if (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 0x21) return 0;
        uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 8);
        uint32_t flag = ((uint32_t)*(volatile uint8_t *)(uintptr_t)(rec + 2) >> 2) & 1u;
        eff = alloc_active_effect(key, 0xc, flag);
        if (eff == 0) return 0;
        *(volatile uint32_t *)(uintptr_t)(eff + 8) = rec;
        *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x20;
    }
    uint8_t cl = *(volatile uint8_t *)(uintptr_t)(rec + 7);          /* 0x33afc: the ramp step */
    if (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 0x2) cl = (uint8_t)(0u - cl);
    uint16_t dx = (cl & 0x80) ? (uint16_t)(0xff00u | (uint8_t)(0u - cl)) : (uint16_t)cl;
    *(volatile uint16_t *)(uintptr_t)(eff + 6) = dx;
    if (*(volatile uint8_t *)(uintptr_t)(rec + 6) & 0x1)
        *(volatile uint8_t *)(uintptr_t)(rec + 2) ^= 0x2;
    *(volatile uint8_t *)(uintptr_t)(eff + 4) = *(volatile uint8_t *)(uintptr_t)(rec + 3);
    return -1;
}

/* cmd_delay_timer (base 0x12, 0x32195 — the EXECUTE entry; 0x32221 is a SEPARATE per-frame TICK callback that
 * shares the region). Unlike the geometry registrants this allocates a FIXED 0x10-byte effect record straight
 * from the heap (alloc_effect_record 0x34464 — BRIDGED, no geom match) and arms a countdown. Entry guard: if
 * !(rec[2]&4) and (rec[2]&0x21) -> ret 0 (already armed). chunk[8]=record back-ptr; chunk[0xc] = the secondary
 * active object 0x8a0fc, or if that is 0 the (u16)mirror 0x8a0c4; always clear 0x8a0c4 after. delay = (u16)
 * word[rec+6]; if rec[2]&2 randomize via the RNG (g_random_seed 0x71f48 = seed*0x5e5+0x29): delay =
 * (delay*(seed&0xffff))>>16 + 4. Store delay -> chunk[6]; if !(rec[2]&4) mark registered [rec+2]|=0x20; stamp
 * the command base rec[3] -> chunk[4]. Returns -1 (acted) / 0 (guard or alloc failed). */
int32_t cmd_delay_timer(uint32_t rec)
{
    uint8_t f2 = *(volatile uint8_t *)(uintptr_t)(rec + 2);
    if (!(f2 & 0x4) && (f2 & 0x21)) return 0;                        /* already armed */
    uint32_t chunk = alloc_effect_record(0x10u);             /* 0x34464 re-pointed */
    if (chunk == 0) return 0;
    *(volatile uint32_t *)(uintptr_t)(chunk + 8) = rec;             /* chunk[8] = record back-ptr */
    uint32_t edx = (uint32_t)G32(VA_g_active_object + 0x4);                          /* g_active_object_secondary */
    if (edx == 0) edx = (uint32_t)G32(VA_g_item_drop_position + 0x118) & 0xffffu;
    *(volatile uint32_t *)(uintptr_t)(chunk + 0xc) = edx;
    G32(VA_g_item_drop_position + 0x118) = 0;
    uint32_t d = *(volatile uint16_t *)(uintptr_t)(rec + 6);        /* sub edx,edx; mov dx,[esi+6] (zero-ext) */
    if (f2 & 0x2) {                                                 /* randomize the delay via the RNG */
        uint32_t seed = (uint32_t)G32(VA_g_command_rng);
        seed = seed * 0x5e5u + 0x29u;
        G32(VA_g_command_rng) = (int32_t)seed;
        d = (d * (seed & 0xffffu)) >> 16;                           /* imul edx,ecx; shr edx,0x10 */
        d += 4u;
    }
    *(volatile uint16_t *)(uintptr_t)(chunk + 6) = (uint16_t)d;
    if (!(*(volatile uint8_t *)(uintptr_t)(rec + 2) & 0x4))
        *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x20;
    *(volatile uint8_t *)(uintptr_t)(chunk + 4) = *(volatile uint8_t *)(uintptr_t)(rec + 3);
    return -1;
}

/* cmd_flash_lights (base 0x11, 0x32269 — EXECUTE entry; 0x32329 is the SEPARATE per-frame TICK callback). A
 * FACE registrant: alloc_face_effect 0x3296b (BRIDGED — eax=key word[rec+8], edx=size 0xe, ebx=flag (rec[2]>>
 * 2)&1) collects the face group (flat 0x4f313 / connected flood 0x4f3d0) and lays a 4-byte-stride match list at
 * chunk+0x12 (count = (u16)dword[chunk+0xe]). Guard rec[2]&0x21 -> ret 0. After alloc: chunk[8]=rec back-ptr,
 * [rec+2]|=0x20, chunk[4]=base rec[3], chunk[6]=0. Then snapshot each face's current brightness: for every
 * match, read (u8)(g_map_geometry_buffer 0x90aa8 + idx)[0xb] and stash it at match+2. Returns -1 / 0 (guard or
 * alloc failed). The TICK handler later ramps those bytes back. Heap-harness verified. */
int32_t cmd_flash_lights(uint32_t rec)
{
    if (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 0x21) return 0;     /* already armed */
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    uint32_t flag = ((uint32_t)*(volatile uint8_t *)(uintptr_t)(rec + 2) >> 2) & 1u;
    uint32_t chunk = alloc_face_effect(key, flag, 0xeu);        /* 0x3296b re-pointed (key=EAX, mode=EBX=flag, base=EDX=0xe) */
    if (chunk == 0) return 0;
    *(volatile uint32_t *)(uintptr_t)(chunk + 8) = rec;                /* chunk[8] = record back-ptr */
    *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x20;                 /* mark registered */
    *(volatile uint8_t *)(uintptr_t)(chunk + 4) = *(volatile uint8_t *)(uintptr_t)(rec + 3);
    *(volatile uint16_t *)(uintptr_t)(chunk + 6) = 0;
    uint32_t count = (uint32_t)*(volatile uint32_t *)(uintptr_t)(chunk + 0xe) & 0xffffu;
    uint32_t p = chunk + 0x12;                                         /* esi = chunk+0xe+4 (first match) */
    uint32_t geom = (uint32_t)G32(VA_g_map_geometry_buffer);                            /* g_map_geometry_buffer (stored ptr) */
    while (count-- > 0) {
        uint16_t idx = *(volatile uint16_t *)(uintptr_t)p;
        uint8_t b = *(volatile uint8_t *)(uintptr_t)(geom + idx + 0xb);
        *(volatile uint8_t *)(uintptr_t)(p + 2) = b;                   /* stash current brightness */
        p += 4;
    }
    return -1;
}

/* cmd_change_height (base 0x07, 0x3121c): register a sector floor/ceiling-height-ramp effect, or toggle an
 * existing one. REGISTER (rec[2]&0x21 clear): alloc_active_effect(key word[rec+8], size 0x10, flag (rec[2]>>2)
 * &1); if word[rec+0x10]!=0 resolve a linked SFX node (find_sfx_node_by_key 0x43b0b) and, if found, stash it at
 * chunk[0xc] + set chunk[5]|=1; then chunk[8]=rec back-ptr, [rec+2]|=0x20, chunk[5] |= (rec[2]&2 ? 0 : 0x80)
 * (direction), chunk[4]=base rec[3]. TOGGLE (rec[2]&0x21 set): only if word[rec+0xe]==0 AND rec[6]&0x20 —
 * find_active_effect(rec[3], rec); if found flip chunk[5]^0x80 and rec[2]^2. Returns -1 (acted) / 0. All three
 * callees already lifted (alloc_active_effect / find_sfx_node_by_key / find_active_effect). */
int32_t cmd_change_height(uint32_t rec)
{
    if (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 0x21) {            /* TOGGLE path */
        if (*(volatile uint16_t *)(uintptr_t)(rec + 0xe) != 0) return 0;
        if (!(*(volatile uint8_t *)(uintptr_t)(rec + 6) & 0x20)) return 0;
        uint32_t eff = find_active_effect(*(volatile uint8_t *)(uintptr_t)(rec + 3), rec);
        if (eff == 0) return 0;
        *(volatile uint8_t *)(uintptr_t)(eff + 5) ^= 0x80;
        *(volatile uint8_t *)(uintptr_t)(rec + 2) ^= 0x2;
        return -1;
    }
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    uint32_t flag = ((uint32_t)*(volatile uint8_t *)(uintptr_t)(rec + 2) >> 2) & 1u;
    uint32_t eff = alloc_active_effect(key, 0x10, flag);
    if (eff == 0) return 0;
    uint16_t sfxkey = *(volatile uint16_t *)(uintptr_t)(rec + 0x10);
    if (sfxkey != 0) {
        uint32_t node = find_sfx_node_by_key(sfxkey);
        if (node != 0) {
            *(volatile uint32_t *)(uintptr_t)(eff + 0xc) = node;
            *(volatile uint8_t *)(uintptr_t)(eff + 5) |= 1;
        }
    }
    *(volatile uint32_t *)(uintptr_t)(eff + 8) = rec;
    *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x20;
    uint8_t dir = (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 0x2) ? 0 : 0x80;
    *(volatile uint8_t *)(uintptr_t)(eff + 5) |= dir;
    *(volatile uint8_t *)(uintptr_t)(eff + 4) = *(volatile uint8_t *)(uintptr_t)(rec + 3);
    return -1;
}

/* cmd_change_object_texture (base 0x0d, 0x327f8 — EXECUTE entry; 0x3286b is the per-frame TICK). Guard
 * rec[2]&0x20 -> 0. REGISTER (key word[rec+8] != 0): snapshot_keyed_secondary_records 0x32a20 (BRIDGED, key,
 * size 0x10); chunk[8]=rec, [rec+2]|=0x20, chunk[0xc]=(u16)g_object_table_generation 0x911c7 (a relocation
 * stamp — the TICK re-resolves the object when (short)gen != chunk[0xc]), chunk[4]=base rec[3]. IMMEDIATE (key==0):
 * act on g_command_source_object 0x8a100 (STORED PTR); if word[rec+0xc] == obj[4] set g@0x8a268=2 & ret 0, else
 * obj[4]=word[rec+0xc], obj[7]=(obj[7]&0xef)|(rec[7]&0x10), ret -1. No source obj -> ret 0. Returns -1 / 0. */
int32_t cmd_change_object_texture(uint32_t rec)
{
    if (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 0x20) return 0;
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    if (key != 0) {                                                   /* REGISTER path */
        uint32_t chunk = snapshot_keyed_secondary_records(key, 0x10); /* 0x32a20 */
        if (chunk == 0) return 0;
        *(volatile uint32_t *)(uintptr_t)(chunk + 8) = rec;
        *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x20;
        *(volatile uint16_t *)(uintptr_t)(chunk + 0xc) = (uint16_t)(uint32_t)G32(VA_g_object_table_generation);
        *(volatile uint8_t *)(uintptr_t)(chunk + 4) = *(volatile uint8_t *)(uintptr_t)(rec + 3);
        return -1;
    }
    uint32_t obj = (uint32_t)G32(VA_g_command_source_object);                            /* IMMEDIATE path (stored ptr) */
    if (obj == 0) return 0;
    uint16_t bx = *(volatile uint16_t *)(uintptr_t)(rec + 0xc);
    if (bx == *(volatile uint16_t *)(uintptr_t)(obj + 4)) {
        G8(VA_g_command_chain_interrupt) = 2;
        return 0;
    }
    *(volatile uint16_t *)(uintptr_t)(obj + 4) = bx;
    uint8_t bl = *(volatile uint8_t *)(uintptr_t)(rec + 7) & 0x10;
    *(volatile uint8_t *)(uintptr_t)(obj + 7) = (*(volatile uint8_t *)(uintptr_t)(obj + 7) & 0xefu) | bl;
    return -1;
}

/* cmd_change_object_height (base 0x23, 0x31107): same two-path shape as cmd_change_height, but the register
 * path snapshots the keyed OBJECT secondary records instead — snapshot_keyed_secondary_records 0x32a20 (BRIDGED:
 * eax=key word[rec+8], edx=size 0x10; same (count+3)/2-dword stride-2 copy as alloc_active_effect, clean for an
 * EVEN match count). After alloc: chunk[8]=rec, [rec+2]|=0x20, chunk[0xc] (word) = (u16)g_object_table_generation
 * 0x911c7 (a relocation stamp, NOT a duration — the TICK re-resolves when (short)gen != chunk[0xc]), chunk[6]=0,
 * chunk[5] |= (rec[2]&2 ? 0 : 0x80) direction, chunk[4]=base rec[3]. TOGGLE path is
 * byte-identical to cmd_change_height (gate word[rec+0xe]==0 && rec[6]&0x20; find_active_effect then flip
 * chunk[5]^0x80 / rec[2]^2). Returns -1 / 0. */
int32_t cmd_change_object_height(uint32_t rec)
{
    if (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 0x21) {            /* TOGGLE path (identical to 0x07) */
        if (*(volatile uint16_t *)(uintptr_t)(rec + 0xe) != 0) return 0;
        if (!(*(volatile uint8_t *)(uintptr_t)(rec + 6) & 0x20)) return 0;
        uint32_t eff = find_active_effect(*(volatile uint8_t *)(uintptr_t)(rec + 3), rec);
        if (eff == 0) return 0;
        *(volatile uint8_t *)(uintptr_t)(eff + 5) ^= 0x80;
        *(volatile uint8_t *)(uintptr_t)(rec + 2) ^= 0x2;
        return -1;
    }
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    uint32_t chunk = snapshot_keyed_secondary_records(key, 0x10);  /* 0x32a20 */
    if (chunk == 0) return 0;
    *(volatile uint32_t *)(uintptr_t)(chunk + 8) = rec;
    *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x20;
    *(volatile uint16_t *)(uintptr_t)(chunk + 0xc) = (uint16_t)(uint32_t)G32(VA_g_object_table_generation);
    *(volatile uint16_t *)(uintptr_t)(chunk + 6) = 0;
    uint8_t dir = (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 0x2) ? 0 : 0x80;
    *(volatile uint8_t *)(uintptr_t)(chunk + 5) |= dir;
    *(volatile uint8_t *)(uintptr_t)(chunk + 4) = *(volatile uint8_t *)(uintptr_t)(rec + 3);
    return -1;
}

/* cmd_move_sector (base 0x09, 0x32ac5): register a sector-move effect that records the sector's sorted vertex
 * height-indices. REGISTER (rec[2]&0x20 clear): find the SECTOR record offset for key word[rec+8]
 * (find_sector_record_offset 0x4f2e0 — BRIDGED, pure geom scan); alloc a 0x16-byte effect (0x34464 — BRIDGED);
 * chunk[8]=rec, chunk[0xc]=(u16)secoff. Then from the sub-record at newoff=word[geom+secoff+0xe], read 4 vertex
 * slot indices (geom+newoff+{0,0xc,0x18,0x24}); each index +edx (edx = rec[6]&0x40 ? 8 : 0xa) gives an offset
 * into g_sector_geom_base 0x90aac whose (signed 16-bit) word is the height/coord. Optimized bubble-sort the 4
 * (coord,index) pairs ascending by coord (3 passes, signed compare), then write the 4 sorted INDEX words to
 * chunk[0xe..0x15]. Finish: [rec+2]|=0x20, chunk[5] |= (rec[2]&2 ? 0 : 0x80), chunk[4]=base. TOGGLE (rec[2]&
 * 0x20 set, gated word[rec+0xe]==0 && rec[6]&0x20): find_active_effect then flip chunk[5]^0x80 / rec[2]^2.
 * Returns -1 / 0. Both geom (0x90aa8) and sector_geom_base (0x90aac) are STORED POINTERS (deref raw). */
int32_t cmd_move_sector(uint32_t rec)
{
    if (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 0x20) {            /* TOGGLE path */
        if (*(volatile uint16_t *)(uintptr_t)(rec + 0xe) != 0) return 0;
        if (!(*(volatile uint8_t *)(uintptr_t)(rec + 6) & 0x20)) return 0;
        uint32_t eff = find_active_effect(*(volatile uint8_t *)(uintptr_t)(rec + 3), rec);
        if (eff == 0) return 0;
        *(volatile uint8_t *)(uintptr_t)(eff + 5) ^= 0x80;
        *(volatile uint8_t *)(uintptr_t)(rec + 2) ^= 0x2;
        return -1;
    }
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    uint32_t secoff = (uint32_t)find_geometry_record(key);     /* 0x4f2e0 re-pointed (SECTOR finder) */
    if (secoff == 0) return 0;
    uint32_t chunk = alloc_effect_record(0x16u);               /* 0x34464 re-pointed */
    if (chunk == 0) return 0;
    *(volatile uint32_t *)(uintptr_t)(chunk + 8) = rec;
    *(volatile uint16_t *)(uintptr_t)(chunk + 0xc) = (uint16_t)secoff;
    uint32_t edx = (*(volatile uint8_t *)(uintptr_t)(rec + 6) & 0x40) ? 8u : 0xau;
    uint8_t *geom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    uint8_t *sgb  = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_sector_geom_base);
    uint32_t newoff = *(uint16_t *)(geom + secoff + 0xe);
    static const uint8_t SLOT[4] = { 0x00, 0x0c, 0x18, 0x24 };
    struct { int16_t coord; uint16_t index; } pr[4];
    for (int k = 0; k < 4; k++) {
        uint32_t idx = (uint32_t)*(uint16_t *)(geom + newoff + SLOT[k]) + edx;
        pr[k].index = (uint16_t)idx;
        pr[k].coord = *(int16_t *)(sgb + idx);                         /* signed height/coord */
    }
    for (int pass = 0; pass < 3; pass++)                               /* optimized bubble sort (6 compares) */
        for (int k = 0; k < 3 - pass; k++)
            if (pr[k].coord > pr[k + 1].coord) {                       /* cmp/jle -> swap when strictly greater */
                int16_t tc = pr[k].coord; uint16_t ti = pr[k].index;
                pr[k].coord = pr[k + 1].coord; pr[k].index = pr[k + 1].index;
                pr[k + 1].coord = tc; pr[k + 1].index = ti;
            }
    for (int k = 0; k < 4; k++)
        *(volatile uint16_t *)(uintptr_t)(chunk + 0xe + 2 * k) = pr[k].index;
    *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x20;
    uint8_t dir = (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 0x2) ? 0 : 0x80;
    *(volatile uint8_t *)(uintptr_t)(chunk + 5) |= dir;
    *(volatile uint8_t *)(uintptr_t)(chunk + 4) = *(volatile uint8_t *)(uintptr_t)(rec + 3);
    return -1;
}

/* ---- shared continue-tails for the inventory/conditional cluster (0x27/0x28/0x29/0x2a) ---- */

/* tail 0x355ba (ret 0) / 0x355d1 (ret -1): identical body — if rec[6]&0x10 set the record skip bit [rec+2]|=8,
 * clear g_pending_command_record 0x8a0dc; differ only in the EAX returned (the caller picks 0 vs -1). */
static int32_t rawcmd_tail_consume(uint32_t rec, int32_t ret)
{
    if (*(volatile uint8_t *)(uintptr_t)(rec + 6) & 0x10)
        *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x8;
    G32(VA_g_pending_command_record) = 0;
    return ret;
}

/* tail 0x355ef: the pending-if-next evaluation. eax = g_pending_command_record 0x8a0dc; if it is set AND
 * g@0x89f60==0, eval the pending record (0x3540b — BRIDGED, esi=record), clear g@0x8a0dc, return 0; if
 * g@0x89f60!=0 return the (nonzero) g@0x8a0dc unchanged; if g@0x8a0dc==0 return 0. Always set g@0x8a268=2. */
static int32_t rawcmd_tail_ifnext(uint32_t rec)
{
    (void)rec;
    uint32_t eax = (uint32_t)G32(VA_g_pending_command_record);
    if (eax != 0 && (uint32_t)G32(VA_g_item_autoselected_flag) == 0) {
        run_command_dbase100_record(eax);                      /* 0x3540b re-pointed (esi=record) */
        eax = 0;
        G32(VA_g_pending_command_record) = 0;
    }
    G8(VA_g_command_chain_interrupt) = 2;
    return (int32_t)eax;
}

/* cmd_if_not_flag_query_consume (base 0x28, 0x355a7): query a DBASE100 flag (test_flag 0x1cb35, BRIDGED — bitmap at
 * *0x81e28, returns nonzero if set) for word[rec+8]; route on the result and rec[6]&1 into the shared tails.
 * Found (nonzero): rec[6]&1 -> consume (ret 0), else if-next eval. Not found (0): rec[6]&1 -> if-next eval,
 * else consume (ret -1). */
int32_t cmd_if_not_flag(uint32_t rec)
{
    uint16_t item = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    uint32_t eax = (uint32_t)test_dbase100_record_flag(item); /* 0x1cb35 re-pointed */
    int f1 = *(volatile uint8_t *)(uintptr_t)(rec + 6) & 1;
    if (eax != 0) return f1 ? rawcmd_tail_consume(rec, 0) : rawcmd_tail_ifnext(rec);
    return f1 ? rawcmd_tail_ifnext(rec) : rawcmd_tail_consume(rec, -1);
}

/* resolve_conditional_command (base 0x29, 0x35580): the "already-evaluated condition" sibling of cmd_if_not_flag —
 * the dispatcher supplies the predicate in EAX (nonzero = condition met) and the record in ESI, and this routes
 * into the same shared tails on EAX and rec[6]&1. Met (eax!=0): rec[6]&1 -> consume (ret 0), else if-next eval.
 * Not met (eax==0): if a command is already pending (g@0x89f60!=0) latch g@0x8a268=1 and ret 0; otherwise rec[6]&1
 * -> if-next eval, else consume (ret -1). */
int32_t resolve_conditional_command(uint32_t eax, uint32_t rec)
{
    int f1 = *(volatile uint8_t *)(uintptr_t)(rec + 6) & 1;
    if (eax != 0)
        return f1 ? rawcmd_tail_consume(rec, 0) : rawcmd_tail_ifnext(rec);
    if ((uint32_t)G32(VA_g_item_autoselected_flag) != 0) {                             /* 0x3559d */
        G8(VA_g_command_chain_interrupt) = 1;
        return 0;
    }
    return f1 ? rawcmd_tail_ifnext(rec) : rawcmd_tail_consume(rec, -1);
}

/* run_command_dbase100_record (base 0x2b, 0x3540b): fire a pending DBASE100 dialogue/command record. id =
 * word[rec+8]; on the FIRST run latch g@0x8a26c=1 + g@0x81e18=1; call eval_dialogue_record_by_id 0x1dc73
 * (BRIDGED — runs the record's commands through the DBASE100 interpreter; deep dialogue/voice side effects); if it
 * acted (nonzero) fall into the shared continue-tail 0x355d1 (rec[6]&0x10 -> [rec+2]|=8; clear g@0x8a0dc; ret -1),
 * else ret 0. Oracle covers the latch + the bridge-returns-0 path (stage g_dbase100_base 0x81e1c=0 so the bridge
 * early-exits 0 at its very first check); the dialogue-FIRES path (eval nonzero -> the already-verified 0x355d1
 * tail) is in-game (live-swap). Also reached via cmd_if_not_flag's if-next tail. */
int32_t run_command_dbase100_record(uint32_t rec)
{
    uint32_t id = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    if (G8(VA_g_command_chain_interrupt + 0x4) == 0) {                                           /* cmp [0x8a26c],0; jne -> skip latch */
        G8(VA_g_command_chain_interrupt + 0x4) = 1;
        G32(VA_g_entity_def_cache_count + 0x4) = 1;
    }
    uint32_t r = eval_dialogue_record_by_id(id);              /* 0x1dc73 re-pointed */
    if (r != 0)
        return rawcmd_tail_consume(rec, -1);                          /* jne 0x355d1 */
    return 0;                                                         /* 0x35436 ret (eax=0) */
}

/* cmd_change_face_texture_adv (base 0x0c, 0x32645): advanced face-texture change with two paths.
 *  - REGISTER (rec[6]&8 clear AND word[rec+8]!=0): guard rec[2]&0x21 -> ret 0; alloc a FIXED 0xc effect record
 *    (alloc_effect_record 0x34464 — BRIDGED, same fixed-alloc shape as cmd_delay_timer); chunk[8]=rec;
 *    rec[2]|=0x20; chunk[4]=base byte[rec+3]; ret -1.
 *  - IMMEDIATE (rec[6]&8 set OR key==0): gather faces + apply the texture/flags to each (the 0x32679 sub-block).
 *    That block has an IRREDUCIBLE undefined-behavior read at 0x32703 (`mov dh,byte[edx+8]` where edx is
 *    gather_faces_by_id's LEFTOVER register — the lifted gather returns EAX only, so a native lift can't reproduce
 *    the original's edx). We BRIDGE the original balanced sub-block (call_orig 0x32679) to PRESERVE byte-identity
 *    (it can't be "more correct" than the original, which itself reads undefined memory there); verified in-game.
 * Oracle covers the REGISTER path (heap harness); the immediate path is the documented bridged-UB case. */
int32_t cmd_change_face_texture_adv(uint32_t rec)
{
    uint8_t b6 = *(volatile uint8_t *)(uintptr_t)(rec + 6);
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    if (!(b6 & 8) && key != 0) {                                      /* REGISTER path */
        if (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 0x21) return 0;        /* already-armed guard */
        uint32_t chunk = alloc_effect_record(0xcu);           /* 0x34464 re-pointed */
        if (chunk == 0) return 0;
        *(volatile uint32_t *)(uintptr_t)(chunk + 8) = rec;
        *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x20;
        *(volatile uint8_t *)(uintptr_t)(chunk + 4) = *(volatile uint8_t *)(uintptr_t)(rec + 3);
        return -1;
    }
    /* IMMEDIATE path — bridge the original balanced 0x32679 sub-block (irreducible UB wild-read). */
    regs_t io; memset(&io, 0, sizeof io); io.va = 0x32679u + OBJ_DELTA; io.esi = rec; io.edi = rec;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    roth_unreachable(io.va - OBJ_DELTA);   /* RAW-command dispatch bridge - in-game raw-command tier (target in io.va) */
#endif
    return (int32_t)io.eax;
}

/* router 0x35580: after a query returns EAX, route on it and rec[6]&1. Found (nonzero): rec[6]&1 -> consume
 * (ret 0), else if-next eval. Not found (0): if g_item_autoselected_flag 0x89f60 != 0 set g@0x8a268=1 & ret 0 (0x3559d);
 * else rec[6]&1 -> if-next eval, else consume (ret -1). */
static int32_t rawcmd_route_35580(uint32_t rec, uint32_t eax)
{
    int f1 = *(volatile uint8_t *)(uintptr_t)(rec + 6) & 1;
    if (eax != 0) return f1 ? rawcmd_tail_consume(rec, 0) : rawcmd_tail_ifnext(rec);
    if ((uint32_t)G32(VA_g_item_autoselected_flag) != 0) { G8(VA_g_command_chain_interrupt) = 1; return 0; }
    return f1 ? rawcmd_tail_ifnext(rec) : rawcmd_tail_consume(rec, -1);
}

/* cmd_if_not_item (base 0x27, 0x35544): the cluster's most branchy conditional. rec[6]&8 -> list mode (0x354f3): if also
 * rec[6]&4, query the accumulated id list (0x1cb6c, BRIDGED) over g_query_list 0x89f68[count g@0x89f64] then
 * route; else APPEND word[rec+8] to that list (if count<0x10) and ret 0. rec[6]&4 (no &8) -> compare the last
 * item g@0x489fa8 (low16) with word[rec+8]: match -> consume(0), else if-next eval. Else -> query the item
 * (0x1ccf7, BRIDGED; mode = (rec[6]&2?1:0)+(rec[6]&0x20?2:0)) then route. */
int32_t cmd_if_not_item(uint32_t rec)
{
    uint8_t f6 = *(volatile uint8_t *)(uintptr_t)(rec + 6);
    if (f6 & 8) {
        if (!(f6 & 4)) {                                              /* 0x354f3: append to the id list */
            uint16_t id = *(volatile uint16_t *)(uintptr_t)(rec + 8);
            int32_t cnt = G32(VA_g_if_item_list_count);
            if (cnt < 0x10) {
                *(volatile uint32_t *)(uintptr_t)(0x89f68u + OBJ_DELTA + (uint32_t)cnt * 4) = id;
                G32(VA_g_if_item_list_count) = cnt + 1;
            }
            return 0;
        }
        int32_t cnt = G32(VA_g_if_item_list_count);                                  /* 0x35518: list query */
        if (cnt == 0) return rawcmd_tail_ifnext(rec);
        uint32_t mode = ((f6 & 2) ? 1u : 0u) + ((f6 & 0x20) ? 2u : 0u);
        uint32_t eax = find_or_autoselect_inventory_item(      /* 0x1cb6c re-pointed (eax=list, edx=mode, ebx=cnt) */
            0x89f68u + OBJ_DELTA, mode, (uint32_t)cnt);
        return rawcmd_route_35580(rec, eax);
    }
    if (f6 & 4) {                                                    /* 0x35573: compare last item */
        uint16_t a = (uint16_t)(uint32_t)G32(VA_g_last_item_record);
        if (a == *(volatile uint16_t *)(uintptr_t)(rec + 8)) return rawcmd_tail_consume(rec, 0);
        return rawcmd_tail_ifnext(rec);
    }
    uint32_t mode = ((f6 & 2) ? 1u : 0u) + ((f6 & 0x20) ? 2u : 0u);  /* 0x35562: single-item query */
    uint32_t eax = query_player_inventory(                     /* 0x1ccf7 re-pointed (eax=id, edx=mode) */
        *(volatile uint16_t *)(uintptr_t)(rec + 8), mode);
    return rawcmd_route_35580(rec, eax);
}

/* cmd_give_item (base 0x29, 0x35437): give an inventory item, optionally computing a drop position. If
 * g@0x8a134==0 AND g@0x8a0fc!=0 (a secondary active object, STORED PTR), compute a drop position into the
 * g@0x489fac struct: x=(sgb[obj[0]+8]+sgb[obj[2]+8])>>1, y=(...+0xa...)>>1 (sgb = g_sector_geom_base 0x90aac,
 * arithmetic >>1 on sign-extended coords), z=((geom[obj[6]+0]+geom[obj[6]+2])>>1)-0x10 (geom=0x90aa8), and
 * [pos+4]=0; the give_item 2nd arg (edx) then points at this struct (runtime 0x489fac). Otherwise edx = g@
 * 0x8a134 (or 0). Then give_item 0x1cedc(eax=item word[rec+8], edx) — BRIDGED — store result -> g@0x489fa8; if
 * nonzero ret -1, else g@0x8a268=2 & ret 0. (give_item early-returns 0 when the inventory is full, g@0x80c2c>=
 * 0x100 — the deterministic oracle path.) */
int32_t cmd_give_item(uint32_t rec)
{
    uint32_t give_edx;
    uint32_t s134 = (uint32_t)G32(VA_g_player_move_delta_z + 0x8);
    if (s134 != 0) {
        give_edx = s134;
    } else {
        uint32_t obj = (uint32_t)G32(VA_g_active_object + 0x4);
        if (obj == 0) {
            give_edx = 0;
        } else {
            uint8_t *sgb  = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_sector_geom_base);
            uint8_t *geom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
            G16(VA_g_item_drop_position + 0x4) = 0;                                          /* [pos+4] = 0 */
            uint8_t *e0 = sgb + *(volatile uint16_t *)(uintptr_t)(obj + 0);
            uint8_t *e1 = sgb + *(volatile uint16_t *)(uintptr_t)(obj + 2);
            int32_t x = ((int32_t)*(int16_t *)(e0 + 8)  + (int32_t)*(int16_t *)(e1 + 8))  >> 1;
            G16(VA_g_item_drop_position) = (uint16_t)x;
            int32_t y = ((int32_t)*(int16_t *)(e0 + 0xa) + (int32_t)*(int16_t *)(e1 + 0xa)) >> 1;
            G16(VA_g_item_drop_position + 0x2) = (uint16_t)y;
            uint8_t *e2 = geom + *(volatile uint16_t *)(uintptr_t)(obj + 6);
            int32_t z = (((int32_t)*(int16_t *)(e2 + 0) + (int32_t)*(int16_t *)(e2 + 2)) >> 1) - 0x10;
            G16(VA_g_item_drop_position + 0xa) = (uint16_t)z;
            give_edx = 0x89facu + OBJ_DELTA;                           /* runtime &g_drop_position 0x489fac */
        }
    }
    uint16_t item = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    uint32_t r = give_item(item, give_edx);                   /* 0x1cedc re-pointed (eax=item, edx=ctx) */
    G32(VA_g_last_item_record) = (int32_t)r;
    if (r != 0) return -1;
    G8(VA_g_command_chain_interrupt) = 2;
    return 0;
}

/* cmd_scroll_face_texture (base 0x0f, 0x324a7): the minimal FACE registrant. Guard rec[2]&0x21 -> 0. Register
 * an effect for the face group key word[rec+9] via build_effect_record_from_matches 0x329d5 (BRIDGED — eax=key,
 * edx=size 0xc; collector 0x4f36d scans the FACE section at geom+8, clean stride-2 copy so an EVEN match count
 * needs no mask). Link chunk[8]=rec, mark [rec+2]|=0x20, stamp chunk[4]=base rec[3]. Returns -1 / 0. */
int32_t cmd_scroll_face_texture(uint32_t rec)
{
    if (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 0x21) return 0;
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 9);        /* NB key at +9 */
    uint32_t chunk = build_effect_record_from_matches(key, 0xcu); /* 0x329d5 re-pointed (key=EAX, base=EDX=0xc) */
    if (chunk == 0) return 0;
    *(volatile uint32_t *)(uintptr_t)(chunk + 8) = rec;
    *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x20;
    *(volatile uint8_t *)(uintptr_t)(chunk + 4) = *(volatile uint8_t *)(uintptr_t)(rec + 3);
    return -1;
}

/* cmd_activate_sfx_node (base 0x10, 0x31339): toggle the keyed SFX nodes' active bit. Collect the nodes for key
 * word[rec+8] (collect_sfx_nodes_by_key 0x43ab4 — BRIDGED, writes match offsets to buf+4, returns count). For
 * each match, node = g_sfx_node_list 0x85c44 (STORED PTR) + offset; flip node[8] bit 0x80, then if rec[6]&6 force
 * it SET, and if rec[6]&4 force it CLEAR. If the node just went OFF (old bit set, new bit clear) AND rec[6]&1,
 * fire it (0x26d3e — BRIDGED audio; skipped when rec[6]&1 clear). After: if !(rec[6]&0x20) set [rec+2]|=8.
 * Returns -1 (acted) / 0 (no nodes). */
int32_t cmd_activate_sfx_node(uint32_t rec)
{
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    uint8_t buf[0x190];
    uint32_t count = collect_sfx_nodes_by_key(                /* 0x43ab4 re-pointed (key=EAX, out=EDX, cap=EBX) */
        key, (uint32_t)(uintptr_t)buf, 0xc8u);
    if (count == 0) return 0;
    uint8_t dh = *(volatile uint8_t *)(uintptr_t)(rec + 6);
    uint32_t base = (uint32_t)G32(VA_g_sfx_nodes);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t node = base + *(uint16_t *)(buf + 4 + i * 2);
        uint8_t dl = *(volatile uint8_t *)(uintptr_t)(node + 8);       /* old flag */
        *(volatile uint8_t *)(uintptr_t)(node + 8) ^= 0x80;
        if (dh & 0x6) {
            *(volatile uint8_t *)(uintptr_t)(node + 8) |= 0x80;
            if (dh & 0x4) *(volatile uint8_t *)(uintptr_t)(node + 8) &= 0x7f;
        }
        if (!(*(volatile uint8_t *)(uintptr_t)(node + 8) & 0x80) && (dl & 0x80) && (dh & 1))
            stop_sound_handle_voice(node);                     /* 0x26d3e re-pointed (fire SFX node) */
    }
    if (!(dh & 0x20)) *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x8;
    return -1;
}

/* cmd_particle_effect (base 0x2d, 0x311ad): spawn a burst of particles at a command object. If g@0x8a0f8==0 and
 * g@0x8a100 (source object, STORED PTR) != 0, spawn byte[rec+7] (>=1, do-while) particles via spawn_particle
 * 0x4b4e9 (BRIDGED) — all with the same args eax=word[obj], edx=word[obj+2], ebx=word[obj+0xa], ecx=word[rec+8].
 * Else if g@0x8a0f8!=0: if g@0x8a0fc==0 return g@0x8a0f8, else spawn via spawn_particle_on_edge 0x4b5b4 (BRIDGED)
 * with eax = geom(0x90aa8)+word[g@0x8a0fc + 6], edx=g@0x8a0fc, ebx=word[rec+8]. Returns -1 (acted) / 0 (no source
 * object). The spawns alloc from g_particle_pool (0x911e4, obj3) — deterministic given the staged RNG seed. */
int32_t cmd_particle_effect(uint32_t rec)
{
    uint32_t a0f8 = (uint32_t)G32(VA_g_active_object);
    if (a0f8 != 0) {                                                  /* edge path */
        uint32_t edx = (uint32_t)G32(VA_g_active_object + 0x4);
        if (edx == 0) return (int32_t)a0f8;                          /* ret g@0x8a0f8 (nonzero) */
        uint32_t geom = (uint32_t)G32(VA_g_map_geometry_buffer);
        uint32_t eax = (uint32_t)*(volatile uint16_t *)(uintptr_t)(edx + 6) + geom;
        uint16_t ebx = *(volatile uint16_t *)(uintptr_t)(rec + 8);
        int cnt = *(volatile uint8_t *)(uintptr_t)(rec + 7);
        do {
            spawn_particle_on_edge(eax, edx, ebx);             /* 0x4b5b4 re-pointed (eax, edx, ebx) */
        } while (--cnt > 0);
        return -1;
    }
    uint32_t obj = (uint32_t)G32(VA_g_command_source_object);
    if (obj == 0) return 0;                                          /* je past the `or eax,-1` -> eax=0 */
    uint16_t a = *(volatile uint16_t *)(uintptr_t)(obj + 0);
    uint16_t d = *(volatile uint16_t *)(uintptr_t)(obj + 2);
    uint16_t b = *(volatile uint16_t *)(uintptr_t)(obj + 0xa);
    uint16_t c = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    int cnt = *(volatile uint8_t *)(uintptr_t)(rec + 7);
    do {
        spawn_particle(a, c, d, b);                           /* 0x4b4e9 re-pointed (eax=a, ecx=c, edx=d, ebx=b) */
    } while (--cnt > 0);
    return -1;
}

/* cmd_remove_item (base 0x2a, 0x354d3): remove an inventory item. eax = item id word[rec+8], or if that is 0
 * the last-touched item g@0x489fa8; always store it back to g@0x489fa8; call remove_item 0x1d077 (BRIDGED — a
 * deep inventory mutation). If it reports a removal (nonzero) fall into the shared continue-tail 0x355d1: if
 * rec[6]&0x10 set the record skip bit [rec+2]|=8, clear g_pending_command_record 0x8a0dc, return -1. Else
 * return 0 (nothing removed). The 0x355d1 tail is the same one cmd_set_flag already verifies. */
int32_t cmd_remove_item(uint32_t rec)
{
    uint32_t eax = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    if (eax == 0) eax = (uint32_t)G32(VA_g_last_item_record);
    G32(VA_g_last_item_record) = (int32_t)eax;
    uint32_t r = remove_item(eax);                            /* 0x1d077 re-pointed */
    if (r != 0) {                                                     /* shared continue-tail 0x355d1 */
        if (*(volatile uint8_t *)(uintptr_t)(rec + 6) & 0x10)
            *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x8;
        G32(VA_g_pending_command_record) = 0;
        return -1;
    }
    return 0;
}

/* cmd_set_inventory_filter (base 0x42, 0x30f63): BTI "Take Inventory" — but mechanically a DISPLAY FILTER, not a
 * removal: items are NEVER taken out of g_inventory_slots (playtest-confirmed: the puzzle area that "takes"/
 * "returns" your gear). mode = (rec[6]&1)?1:0; call set_inventory_list_filter 0x1ca2e (BRIDGED): mode 0 hides all
 * slots except dbase100 type-nibble 2 (inventory *looks* emptied) via the slot hidden-bit + refresh
 * (0x1c59e/0x1bb4b/0x1be8e/0x2245c); mode 1 un-hides all ("given back"). Then if rec[6]&0x10 set the record skip
 * bit [rec+2]|=8. Always returns -1. Oracle: stage an EMPTY inventory (g@0x80c2c=0) so the bridge's item loops
 * skip. */
int32_t cmd_set_inventory_filter(uint32_t rec)
{
    uint32_t mode = (*(volatile uint8_t *)(uintptr_t)(rec + 6) & 1) ? 1u : 0u;
    set_inventory_list_filter(mode);                            /* 0x1ca2e re-pointed */
    if (*(volatile uint8_t *)(uintptr_t)(rec + 6) & 0x10)
        *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x8;
    return -1;
}

/* cmd_modify_sector (base 0x03, 0x312a1): byte-identical to cmd_change_height EXCEPT the linked-SFX key is read
 * from word[rec+0x14] (not rec+0x10). Register path: alloc_active_effect(key word[rec+8], size 0x10, flag
 * (rec[2]>>2)&1) + optional SFX node (find_sfx_node_by_key) -> chunk[0xc]+bit1, link/mark, direction flag,
 * base. Toggle path identical (find_active_effect, flip chunk[5]^0x80 / rec[2]^2). Returns -1 / 0. */
int32_t cmd_modify_sector(uint32_t rec)
{
    if (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 0x21) {            /* TOGGLE path */
        if (*(volatile uint16_t *)(uintptr_t)(rec + 0xe) != 0) return 0;
        if (!(*(volatile uint8_t *)(uintptr_t)(rec + 6) & 0x20)) return 0;
        uint32_t eff = find_active_effect(*(volatile uint8_t *)(uintptr_t)(rec + 3), rec);
        if (eff == 0) return 0;
        *(volatile uint8_t *)(uintptr_t)(eff + 5) ^= 0x80;
        *(volatile uint8_t *)(uintptr_t)(rec + 2) ^= 0x2;
        return -1;
    }
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    uint32_t flag = ((uint32_t)*(volatile uint8_t *)(uintptr_t)(rec + 2) >> 2) & 1u;
    uint32_t eff = alloc_active_effect(key, 0x10, flag);
    if (eff == 0) return 0;
    uint16_t sfxkey = *(volatile uint16_t *)(uintptr_t)(rec + 0x14);   /* NB +0x14 (vs 0x07's +0x10) */
    if (sfxkey != 0) {
        uint32_t node = find_sfx_node_by_key(sfxkey);
        if (node != 0) {
            *(volatile uint32_t *)(uintptr_t)(eff + 0xc) = node;
            *(volatile uint8_t *)(uintptr_t)(eff + 5) |= 1;
        }
    }
    *(volatile uint32_t *)(uintptr_t)(eff + 8) = rec;
    *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x20;
    uint8_t dir = (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 0x2) ? 0 : 0x80;
    *(volatile uint8_t *)(uintptr_t)(eff + 5) |= dir;
    *(volatile uint8_t *)(uintptr_t)(eff + 4) = *(volatile uint8_t *)(uintptr_t)(rec + 3);
    return -1;
}

/* ---- RAW effect-TICK system: the shared "mark records by key" leaves (called by the tick handlers to
 * stamp dirty/active flag bits onto geometry/object records). Each "by_key" body resolves a key into a
 * list of buffer-relative record offsets (via a collector: the already-lifted geom_find_matches 0x4f313 /
 * collect_raw_state_matches 0x4f36d / collect_secondary_state_records_by_key 0x34c97) then ORs a small mask
 * into a per-record flag byte. The mask is fixed by the entry stub (`mov cl,<mask>; jmp body`), each stub a
 * separate lift. ---- */

/* mark_geometry_records_by_id (0x31cdd): ESI=record. key=u16[rec+8]; key==0 -> no-op. mask = (rec[7]&1) ?
 * 0x80 : (rec[7]&3) ? 0xc0 : 0x40. Scan every SECTOR record (geom 0x90aa8; section geom+(u32[geom+4]&0xffff),
 * stride 0x1a, count u16[section-2]); where u16[rec+0x14]==key, OR mask into byte[rec+0x17]. No collector. */
void mark_geometry_records_by_id(uint32_t rec)
{
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    if (key == 0) return;
    uint8_t b7 = *(volatile uint8_t *)(uintptr_t)(rec + 7);
    uint8_t mask = 0x40;
    if (b7 & 3) { mask = 0xc0; if (b7 & 1) mask = 0x80; }
    uint8_t *buf = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    uint8_t *p = buf + (*(uint32_t *)(buf + 4) & 0xffff);
    int32_t count = (int32_t)*(uint16_t *)(p - 2);
    while (count > 0) {
        if (*(uint16_t *)(p + 0x14) == key) *(p + 0x17) |= mask;
        p += 0x1a; count--;
    }
}

/* mark_geometry_faces_by_key (body 0x31d31; entries mark_geom_faces_b20 0x31d2b cl=0x20, _b10 0x31d2f
 * cl=0x10): ESI=record. key=u16[rec+8]; key==0 -> no-op. Resolve every SECTOR record offset matching the key
 * via the lifted geom_find_matches (0x4f313; fills buf, offsets at buf+4, EAX=count&0xffff); for each, OR cl
 * into byte[geom+0x16+off] (geom 0x90aa8; off is buffer-relative). void. */
static void mark_geometry_faces_by_key(uint32_t rec, uint8_t cl)
{
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    if (key == 0) return;
    uint8_t buf[0x190];
    uint32_t cnt = geom_find_matches(key, 0xc8, buf) & 0xffff;
    uint8_t *geom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    uint16_t *list = (uint16_t *)(buf + 4);
    for (uint32_t i = 0; i < cnt; i++)
        *(geom + 0x16 + list[i]) |= cl;
}
void mark_geom_faces_b20(uint32_t rec) { mark_geometry_faces_by_key(rec, 0x20); }
void mark_geom_faces_b10(uint32_t rec) { mark_geometry_faces_by_key(rec, 0x10); }

/* mark_raw_state_records_by_key (body 0x31d84; entries _b04 0x31d7a cl=0x4, _b02 0x31d7e cl=0x2, _b01 0x31d82
 * cl=0x1): ESI=record. key=u16[rec+8]; key==0 -> no-op. Resolve matching RAW-STATE record offsets via the
 * lifted collect_raw_state_matches (0x4f36d; out[0]=count, out[1]=key, offsets at out[2..], EAX=count&0xffff);
 * for each, OR cl into byte[geom+off+9] (geom 0x90aa8). void. */
static void mark_raw_state_records_by_key(uint32_t rec, uint8_t cl)
{
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    if (key == 0) return;
    uint16_t buf[0xc8];
    uint32_t cnt = (uint32_t)collect_raw_state_matches(key, buf, 0xc8) & 0xffff;
    uint8_t *geom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    uint16_t *list = buf + 2;
    for (uint32_t i = 0; i < cnt; i++)
        *(geom + list[i] + 9) |= cl;
}
void mark_raw_state_b04(uint32_t rec) { mark_raw_state_records_by_key(rec, 0x4); }
void mark_raw_state_b02(uint32_t rec) { mark_raw_state_records_by_key(rec, 0x2); }
void mark_raw_state_b01(uint32_t rec) { mark_raw_state_records_by_key(rec, 0x1); }

/* mark_object_records_by_key (body 0x31dd5; entries _b08 0x31dcb cl=0x8, _b04 0x31dcf cl=0x4, _b20 0x31dd3
 * cl=0x20): ESI=record. key=u16[rec+8]; key==0 -> no-op. Resolve matching secondary-state record offsets via
 * the lifted collect_secondary_state_records_by_key (0x34c97; offsets at out[0..], EAX=count — not masked);
 * for each, OR cl into byte[obj+off+9] (obj = g_map_objects_buffer 0x90aa4). void. */
static void mark_object_records_by_key(uint32_t rec, uint8_t cl)
{
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    if (key == 0) return;
    uint16_t buf[0xc8];
    uint32_t cnt = (uint32_t)collect_secondary_state_records_by_key(key, 0xc8,
                       (uint32_t)(uintptr_t)buf);
    uint8_t *obj = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_objects_buffer);
    for (uint32_t i = 0; i < cnt; i++)
        *(obj + buf[i] + 9) |= cl;
}
void mark_objects_b08(uint32_t rec) { mark_object_records_by_key(rec, 0x8); }
void mark_objects_b04(uint32_t rec) { mark_object_records_by_key(rec, 0x4); }
void mark_objects_b20(uint32_t rec) { mark_object_records_by_key(rec, 0x20); }

/* ===== Effect-tick / geometry support leaves ===== */

/* clear_geometry_visited_flags (0x4f477): clear the visited bit (0x4 of byte[rec+0x16]) on every SECTOR
 * record of g_map_geometry_buffer (0x90aa8). Pure leaf, no callees — the exact body the connected-flood
 * collector already uses as rawcmd_clear_geom_visited_bits; this is its gate-recorded standalone entry. */
void clear_geometry_visited_flags(void)
{
    rawcmd_clear_geom_visited_bits();                                /* 0x4f477 body (see above) */
}

/* apply_geometry_face_write (0x343e1): for each of ECX (signed, do-while) entries in the EDI offset list
 * (u16 each), write BX (low word) into geom[EDX]; EDX's low word is reloaded from the list each pass while
 * its high word stays the caller's entry EDX (`mov dx,[edi]` partial load -> full `[esi+edx]` index). Pure
 * geom-buffer leaf, no callees, EAX preserved. (Sibling of apply_floor_move_to_group 0x3423e's inner loop.) */
void apply_geometry_face_write(uint32_t edi, int32_t ecx, uint16_t bx, uint32_t edx_in)
{
    uint8_t *geom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);    /* mov esi,[0x90aa8] */
    uint32_t edx = edx_in;
    do {
        edx = (edx & 0xffff0000u) | *(volatile uint16_t *)(uintptr_t)edi;   /* mov dx,[edi] */
        edi += 2;                                                   /* add edi,2 */
        *(volatile uint16_t *)(uintptr_t)(geom + edx) = bx;         /* mov [esi+edx],bx */
    } while (--ecx > 0);                                            /* dec ecx; jg (signed) */
}

/* Public gate-recorded entries for the Count-family shared workers (already implemented above as the static
 * helpers the cmd_count handlers reuse; these expose them for an independent oracle test). step_count_command
 * 0x3192d (ESI=rec, AX=seed -> EAX) and flush_pending_command_record 0x31963 (-> EAX). */
int32_t step_count_command(uint32_t rec, uint16_t seed) { return rawcmd_step_count_command(rec, seed); }
int32_t flush_pending_command_record(void) { return rawcmd_flush_pending_command_record(); }
/* The Count-family geometry/object APPLY workers (step then repaint), exposed for independent oracle tests. */
int32_t step_count_apply_to_primary_cells(uint32_t rec, uint16_t seed) { return rawcmd_step_count_apply_primary(rec, seed); }   /* 0x319c0 */
int32_t step_count_apply_to_secondary_records(uint32_t rec, uint16_t seed) { return rawcmd_step_count_apply_secondary(rec, seed); } /* 0x31a94 */
int32_t step_count_apply_to_geometry_faces(uint32_t rec, uint16_t seed) { return rawcmd_step_count_apply_faces(rec, seed); }    /* 0x31b4f */

/* apply_geometry_move_with_player (0x343b4): EDI = a list of ECX cell offsets (u16, no header), BX = the new
 * floor height. For each cell, set its floor word[geom+off+2]=BX, publish the move delta (BX-old) to 0x8a12c,
 * and run apply_cell_move_to_player(flags=1) to carry the player/objects. EDX's low word reloads per entry;
 * apply_cell_move preserves EDX, so its high word stays the caller's. Pure compose, no new bridge. */
void apply_geometry_move_with_player(uint32_t edi, int32_t ecx, uint16_t bx, uint32_t edx_in)
{
    uint8_t *geom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    uint32_t edx = edx_in;
    do {
        edx = (edx & 0xffff0000u) | *(volatile uint16_t *)(uintptr_t)edi; edi += 2;
        uint16_t old = *(volatile uint16_t *)(geom + edx + 2);
        *(volatile uint16_t *)(geom + edx + 2) = bx;
        G32(VA_g_player_move_delta_z) = (int32_t)((int32_t)(uint32_t)bx - (int32_t)(uint32_t)old);   /* sub/neg: BX-old */
        apply_cell_move_to_player(1, edx);
    } while (--ecx > 0);
}

/* apply_floor_move_to_group (0x3423e): EDI = {u32 count@0, u16 cell offsets@4}, BX = new floor. Like
 * apply_geometry_move_with_player but only lowers cells whose old floor is ABOVE BX (signed cmp; jle skips). */
void apply_floor_move_to_group(uint32_t edi, uint16_t bx, uint32_t edx_in)
{
    int32_t ecx = (int32_t)(*(volatile uint16_t *)(uintptr_t)edi);   /* count = word[edi] (& 0xffff) */
    edi += 4;
    uint8_t *geom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    uint32_t edx = edx_in;
    do {
        edx = (edx & 0xffff0000u) | *(volatile uint16_t *)(uintptr_t)edi; edi += 2;
        int16_t old = *(volatile int16_t *)(geom + edx + 2);
        if (old > (int16_t)bx) {                                     /* cmp ax,bx; jle skip (signed) */
            *(volatile uint16_t *)(geom + edx + 2) = bx;
            G32(VA_g_player_move_delta_z) = (int32_t)((int32_t)(uint32_t)bx - (int32_t)(uint32_t)(uint16_t)old);
            apply_cell_move_to_player(1, edx);
        }
    } while (--ecx > 0);
}

/* apply_cell_move_to_player shared body (entries: 0x348ed portalcheck=0 / 0x348f9 portalcheck=1). EAX=flags
 * (bl&3: bit0 floor, bit1 ceiling), EDX = cell offset into g_map_geometry_buffer 0x90aa8. When the player's
 * sector (cell offset @ g_player_sector_cell 0x90c12) matches, carry the player height (0x90a92) by the move
 * delta (0x8a12c) if it lies within the cell's moving floor/ceiling span; likewise carry the cell's contained
 * objects (via the lifted adjust_records 0x34a5f). portalcheck=1 additionally probes a thin (<=0x60) floor gap
 * through the collision portal check 0x3db20 (BRIDGED, returns CF; fs/gs selectors) -> set merge bit
 * [0x8a130]|=2. Returns EAX = [0x8a130]. (0x91dfc = object-region base for the cell->object-index divide.) */
static int32_t rawcmd_cell_move_body(uint32_t flags, uint32_t celloff, int32_t portalcheck)
{
    G32(VA_g_anim_step_mode + 0x10) = portalcheck;                                      /* 0x348ed=0 / 0x348f9=1 */
    G32(VA_g_player_move_delta_z + 0x4) = 0;
    uint8_t bl = (uint8_t)(flags & 3);                              /* and bl,3 */
    if (bl == 0) return (int32_t)G32(VA_g_player_move_delta_z + 0x4);                      /* je 0x34a55 */

    uint8_t *geom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    uint8_t *cell = geom + celloff;
    int16_t delta = (int16_t)G16(VA_g_player_move_delta_z);

    if ((uint16_t)celloff == G16(VA_g_player_sector)) {                        /* player is in this cell's sector */
        if (bl & 1) {                                              /* floor carry */
            int16_t H  = *(volatile int16_t *)(cell + 2);
            int16_t Hd = (int16_t)(H - delta);
            int16_t lo = (Hd > H) ? H : Hd, hi = (Hd > H) ? Hd : H;
            int16_t pz = (int16_t)G16(VA_g_player_z);
            if (pz >= lo && pz <= hi) G16(VA_g_player_z) = (uint16_t)(pz + delta);
        }
        if (bl & 2) {                                             /* ceiling carry via linked cell */
            uint16_t link = *(volatile uint16_t *)(cell + 0x18);
            if (link != 0) {
                int16_t H  = *(volatile int16_t *)(geom + link + 8);
                int16_t Hd = (int16_t)(H - delta);
                int16_t lo = (Hd > H) ? H : Hd, hi = (Hd > H) ? Hd : H;
                int16_t pz = (int16_t)G16(VA_g_player_z);
                if (pz >= lo && pz <= hi) G16(VA_g_player_z) = (uint16_t)(pz + delta);
            }
        }
    }

    if (portalcheck != 0 && (bl & 1)) {                            /* thin-gap portal merge (0x348f9 only) */
        int16_t gap = (int16_t)(*(volatile int16_t *)(cell + 0) - *(volatile int16_t *)(cell + 2));
        if (gap <= 0x60) {                                         /* 0x3db20 re-pointed (returns CF; §6.5 in-game) */
            if (scan_portal_walls_near_query((uint16_t)celloff, (uint16_t)G16(VA_g_player_sector)))
                G32(VA_g_player_move_delta_z + 0x4) |= 2;                                 /* CF set (stc) -> merge */
        }
    }

    if (*(volatile uint8_t *)(cell + 0x16) & 2) {                  /* cell carries objects */
        uint32_t diff = celloff - (uint32_t)G32(VA_g_sector_section_offset);          /* eax = celloff - object base */
        uint16_t q = (uint16_t)(diff & 0xffffu) / 0xd;             /* div si (16-bit) */
        uint32_t eidx = ((diff & 0xffff0000u) | q) + 2;            /* high bits preserved; +2 */
        uint8_t *objbuf = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_objects_buffer);
        uint16_t entry = *(volatile uint16_t *)(objbuf + eidx);
        if (entry != 0) {
            uint8_t *group = objbuf + entry;
            uint8_t count = group[0];
            if (count != 0) {
                if (bl & 1)
                    adjust_records_z_carry((uint16_t)*(volatile int16_t *)(cell + 2), group + 2, count);
                if (bl & 2) {
                    uint16_t link = *(volatile uint16_t *)(cell + 0x18);
                    if (link != 0)
                        adjust_records_z_carry((uint16_t)*(volatile int16_t *)(geom + link + 8),
                                                    group + 2, group[0]);
                }
            }
        }
    }
    return (int32_t)G32(VA_g_player_move_delta_z + 0x4);
}

/* apply_cell_move_to_player (0x348ed): the non-portal entry — carry the player + objects with a moving cell. */
int32_t apply_cell_move_to_player(uint32_t flags, uint32_t celloff)
{
    return rawcmd_cell_move_body(flags, celloff, 0);
}

/* apply_cell_move_to_player_portalcheck (0x348f9): same body, but also runs the thin-gap portal merge. */
int32_t apply_cell_move_to_player_portalcheck(uint32_t flags, uint32_t celloff)
{
    return rawcmd_cell_move_body(flags, celloff, 1);
}

/* fire_queued_command (0x30b7c): ESI=record, EBX=timer slot. Run the chain flow/condition pre-pass
 * (walk_command_chain_flow, idx word[rec+4]); if it blocks (nonzero) or the deferred queue (0x71f40) is full,
 * stamp slot[0]=0x20 and return (the block ptr / 0). Otherwise enqueue {idx, 0} and fall into
 * init_command_timer_countdown (writes slot). Composes two lifted fns; returns the block ptr or the countdown. */
int32_t fire_queued_command(uint32_t rec, uint32_t slot)
{
    uint16_t idx = *(volatile uint16_t *)(uintptr_t)(rec + 4);
    uint32_t w = walk_command_chain_flow(idx);
    if (w != 0) { *(volatile uint32_t *)(uintptr_t)slot = 0x20; return (int32_t)w; }   /* blocked */
    uint8_t *q = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_anim_step_fn_table + 0x10);
    uint32_t cnt = *(volatile uint32_t *)q;
    if (cnt >= 0x10) { *(volatile uint32_t *)(uintptr_t)slot = 0x20; return 0; }        /* queue full */
    *(volatile uint32_t *)(q + cnt * 8 + 4) = *(volatile uint16_t *)(uintptr_t)(rec + 4);  /* movzx idx */
    *(volatile uint32_t *)(q + cnt * 8 + 8) = 0;
    *(volatile uint32_t *)q = cnt + 1;
    init_command_timer_countdown(rec, slot);                                    /* fall into 0x30b45 */
    return (int32_t)*(volatile uint32_t *)(uintptr_t)slot;                             /* eax = countdown */
}

/* tick_apply_geometry_effect (0x34322): ESI=record. Gated by rec[2]&8 (skip) and (g_state_record_count 0x89f5c
 * != 0 && !(rec[2]&1)) (skip). Compute the delta value (rec[2]&2 ? word[rec+0xc] : word[rec+0xa]) * 2; collect
 * the geometry group for key word[rec+8] — connected flood (flag (rec[2]>>2)&1) or geom_find_matches — and, if
 * any, dispatch the apply (rec[6]&1 ? apply_geometry_face_write : apply_geometry_move_with_player) over the
 * match offsets (list+4, count). Then fall into tick_rerun_command_execute. All callees lifted. */
int32_t tick_apply_geometry_effect(uint32_t rec)
{
    if (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 8) return 0;             /* 0x34328 ret */
    if (G32(VA_g_state_record_list_count) != 0 && !(*(volatile uint8_t *)(uintptr_t)(rec + 2) & 1)) return 0;

    uint8_t al = *(volatile uint8_t *)(uintptr_t)(rec + 2);
    uint16_t value = (al & 2) ? *(volatile uint16_t *)(uintptr_t)(rec + 0xc)
                              : *(volatile uint16_t *)(uintptr_t)(rec + 0xa);
    uint16_t bx = (uint16_t)(value * 2);                                     /* add edx,edx (low 16 used) */
    int flag = (al >> 2) & 1;
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    uint16_t outbuf[0xc8 + 8];
    uint32_t count = flag
        ? (uint32_t)collect_connected_geometry_group(key, 0xc8, (uint32_t)(uintptr_t)outbuf) & 0xffffu
        : (uint32_t)geom_find_matches(key, 0xc8, (uint8_t *)outbuf) & 0xffffu;
    int32_t eax_in = 0;                                                     /* no-match: eax = count = 0 */
    if (count != 0) {                                                       /* je 0x3439d skips */
        uint32_t list = (uint32_t)(uintptr_t)outbuf + 4;                    /* edi = ebp+4 (skip {count,key}) */
        uint32_t idx = *(volatile uint8_t *)(uintptr_t)(rec + 6) & 1u;      /* dispatch idx (count<0x100) */
        if (idx) {
            apply_geometry_face_write(list, (int32_t)count, bx, 0);  /* table[1] = 0x343e1 (void; eax=idx) */
            eax_in = (int32_t)idx;                                          /* face_write leaves eax = dispatch idx */
        } else {
            apply_geometry_move_with_player(list, (int32_t)count, bx, 0); /* table[0] = 0x343b4 */
            eax_in = (int32_t)G32(VA_g_player_move_delta_z + 0x4);                                 /* move returns apply_cell_move's [0x8a130] */
        }
    }
    return rawcmd_tick_rerun(rec, eax_in);                                  /* jmp 0x34086 */
}

/* tick_rerun_command_execute (0x34086): ESI=record. If armed (rec[2]&1), clear the arm bits (rec[2]&=0xde) and
 * unless suppressed (rec[2]&8), re-dispatch the command through the 0x30780 table (base = rec[3]&0x7f, BRIDGED
 * — esi=edi=record), then re-arm (rec[2]|=1). On the early-outs the original PRESERVES the incoming EAX
 * (`je 0x30ab2` is a bare ret) — callers that fall in (FUN_000340b6 / tick_resolve / tick_apply) leave a live
 * value in EAX, so the body threads eax_in. The 0x34086 entry itself is dispatched with EAX=0. */
static int32_t rawcmd_tick_rerun(uint32_t rec, int32_t eax_in)
{
    if (!(*(volatile uint8_t *)(uintptr_t)(rec + 2) & 1)) return eax_in;     /* je 0x30ab2 (eax preserved) */
    *(volatile uint8_t *)(uintptr_t)(rec + 2) &= 0xde;
    if (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 8) return eax_in;        /* je 0x30ab2 (eax preserved) */
    uint32_t bs = *(volatile uint8_t *)(uintptr_t)(rec + 3) & 0x7fu;
    uint32_t handler = *(uint32_t *)(uintptr_t)(0x30780u + OBJ_DELTA + bs * 4);
    regs_t io; memset(&io, 0, sizeof io); io.va = handler; io.esi = rec; io.edi = rec;
#ifndef ROTH_STANDALONE
    call_orig(&io);                                                          /* call [ebx*4+0x30780] */
#else
    io.eax = rawcmd_dispatch_30780(io.va, io.esi);   /* the 0x30780 table -> lifted cmd bodies */
#endif
    int32_t r = (int32_t)io.eax;
    *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 1;
    return r;
}
int32_t tick_rerun_command_execute(uint32_t rec) { return rawcmd_tick_rerun(rec, 0); }   /* 0x34086 (eax=0) */

/* FUN_000340b6 (0x340b6): a thin alias entry — loads AX=word[rec+8] then falls into tick_rerun_command_execute.
 * The dispatched handlers are ESI=record (AX don't-care), but AX is live into the early-out's preserved EAX. */
int32_t FUN_000340b6(uint32_t rec)
{
    return rawcmd_tick_rerun(rec, (int32_t)(uint32_t)*(volatile uint16_t *)(uintptr_t)(rec + 8));
}

/* tick_resolve_state_and_rerun (0x340bc): ESI=record. Re-resolve the record's key word[rec+8] through
 * find_raw_state_record (0x4f52b) then find_face_record (0x4f567) — unless 0 or rec[6]&8 — store it back; if
 * rec[2]&2 and not suppressed, swap the linked cell state (swap_cell_state_linked_pair 0x33571); then fall into
 * tick_rerun_command_execute. All callees lifted. */
int32_t tick_resolve_state_and_rerun(uint32_t rec)
{
    uint16_t ax = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    if (ax != 0 && !(*(volatile uint8_t *)(uintptr_t)(rec + 6) & 8)) {
        ax = (uint16_t)find_raw_state_record(ax);                     /* 0x4f52b */
        if (ax != 0) ax = (uint16_t)find_face_record(ax);            /* 0x4f567 */
    }
    *(volatile uint16_t *)(uintptr_t)(rec + 8) = ax;
    int32_t eax_in = (int32_t)(uint32_t)ax;                                  /* eax = ax at the jmp (no-swap) */
    if ((*(volatile uint8_t *)(uintptr_t)(rec + 2) & 2) &&
        !(*(volatile uint8_t *)(uintptr_t)(rec + 2) & 8))
        eax_in = swap_cell_state_linked_pair(rec);                   /* 0x33571 -> eax = old B[0xa] */
    return rawcmd_tick_rerun(rec, eax_in);                                  /* jmp 0x34086 */
}

/* tick_delay_timer (0x32221): EAX=effect record. Decrement the timer word[rec+6] by the frame delta 0x85324;
 * while still positive, store it back and return 0. On expiry (<=0), enqueue the linked command (rec[8]) onto
 * the deferred-command queue (g_deferred_command_queue 0x71f40 — {u32 count@0, entries stride 8: chain-idx@+4,
 * extra@+8}) unless full (>=0x10), clearing the queued record's arm bits (rec8[2]&=0xde unless bit2), and
 * return -1; full -> 0. Self-contained leaf (no callees). */
int32_t tick_delay_timer(uint32_t rec)
{
    int32_t v = (int32_t)(uint16_t)*(volatile uint16_t *)(uintptr_t)(rec + 6) - (int32_t)G32(VA_g_frame_time_scale);
    if (v > 0) {                                                    /* jle expired */
        *(volatile uint16_t *)(uintptr_t)(rec + 6) = (uint16_t)v;
        return 0;
    }
    uint8_t *q = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_anim_step_fn_table + 0x10);
    uint32_t cnt = *(volatile uint32_t *)q;
    if (cnt >= 0x10) return 0;                                      /* jae -> return 0 */
    uint32_t edi = *(volatile uint32_t *)(uintptr_t)(rec + 8);      /* queued command record */
    if (!(*(volatile uint8_t *)(uintptr_t)(edi + 2) & 4))
        *(volatile uint8_t *)(uintptr_t)(edi + 2) &= 0xde;
    *(volatile uint32_t *)q = cnt + 1;
    *(volatile uint32_t *)(q + cnt * 8 + 4) = *(volatile uint16_t *)(uintptr_t)(edi + 4); /* movzx */
    *(volatile uint32_t *)(q + cnt * 8 + 8) = *(volatile uint32_t *)(uintptr_t)(rec + 0xc);
    return -1;
}

/* register_object_state_effect (0x31176): ESI=record. If unarmed (rec[2]&0x21 clear), snapshot the keyed OBJECT
 * secondary records into a new effect chunk (snapshot_keyed_secondary_records 0x32a20, BRIDGED; eax=key
 * word[rec+8], edx=size 0x10), then link it (chunk[8]=rec), stamp the object-table generation (chunk[0xc] =
 * (u16)g_object_table_generation 0x911c7), mark the record registered (rec[2]|=0x20) and copy rec[3]->chunk[4].
 * Returns -1 on register, 0 if armed or no chunk. */
int32_t register_object_state_effect(uint32_t rec)
{
    if (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 0x21) return 0;          /* jne 0x311ac */
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    uint32_t chunk = snapshot_keyed_secondary_records(key, 0x10);     /* 0x32a20 */
    if (chunk == 0) return 0;                                                /* je 0x311ac */
    *(volatile uint32_t *)(uintptr_t)(chunk + 8) = rec;
    *(volatile uint16_t *)(uintptr_t)(chunk + 0xc) = (uint16_t)G32(VA_g_object_table_generation); /* reloc/generation stamp */
    *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 0x20;
    *(volatile uint8_t *)(uintptr_t)(chunk + 4) = *(volatile uint8_t *)(uintptr_t)(rec + 3);
    return -1;
}

/* tick_register_object_state (0x3405e): ESI=record. If armed (rec[2]&0x21), clear those bits; unless bit 8 is
 * set, register the object-state effect (register_object_state_effect) and set rec[2] bit 0. Returns the
 * registrant's result (-1/0), or 0 on the early-outs. */
int32_t tick_register_object_state(uint32_t rec)
{
    if (!(*(volatile uint8_t *)(uintptr_t)(rec + 2) & 0x21)) return 0;       /* je 0x30ab2 */
    *(volatile uint8_t *)(uintptr_t)(rec + 2) &= 0xde;                       /* clear bits 0x21 */
    if (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 8) return 0;             /* je 0x30ab2 */
    int32_t r = register_object_state_effect(rec);
    *(volatile uint8_t *)(uintptr_t)(rec + 2) |= 1;
    return r;
}

/* cmd_default_nop (0x30ab0): the dispatch table's default/unimplemented-opcode slot. `sub eax,eax; ret`. */
int32_t cmd_default_nop(uint32_t rec) { (void)rec; return 0; }

/* tick_register_timed_effect (0x30b27): allocate a slot in the timed-effect array (g_timed_effects 0x89fc0,
 * max 0x20, stride 8, count g_timed_effect_count 0x89fbc) then fall into init_command_timer_countdown
 * (0x30b45, lifted) which writes {countdown@0, record-ptr@4} into the slot. ESI=record. Returns the countdown
 * (EAX), or 0 (the preserved entry EAX in the ESI=rec dispatch) when the array is full. */
int32_t tick_register_timed_effect(uint32_t rec)
{
    if ((uint32_t)G32(VA_g_item_drop_position + 0x10) >= 0x20) return 0;                   /* cmp [0x89fbc],0x20; jae ret */
    uint32_t slot = 0x89fc0u + (uint32_t)G32(VA_g_item_drop_position + 0x10) * 8;          /* ebx = 0x89fc0 + count*8 (canon) */
    G32(VA_g_item_drop_position + 0x10) += 1;                                              /* inc count */
    init_command_timer_countdown(rec, (uint32_t)GADDR(slot)); /* fall into 0x30b45 */
    return (int32_t)G32(slot);                                      /* eax = [slot] = countdown */
}

/* find_object_record_by_id (0x34531): AX = object id -> EAX = the matching record's HOST pointer (0 if none).
 * Search the object table g_object_table_header (0x85c30, a stored host ptr): count = word[tbl+0x16]; index
 * array at tbl + word[tbl+0x14] (u16 record offsets); for each, skip records with flag bit 8 at byte[rec+2],
 * else match word[rec+8] == id. Returns tbl + recoff. Read-only leaf, no callees/selectors. */
int32_t find_object_record_by_id(uint16_t id)
{
    uint8_t *tbl = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_object_table_header);
    int32_t count = (int32_t)(uint16_t)*(volatile uint16_t *)(tbl + 0x16);
    if (count == 0) return 0;                                       /* je 0x3456a -> eax=0 */
    uint8_t *idx = tbl + *(volatile uint16_t *)(tbl + 0x14);        /* index array base */
    do {
        uint16_t recoff = *(volatile uint16_t *)idx;               /* bx = word[edi] */
        if (!(*(volatile uint8_t *)(tbl + recoff + 2) & 8) &&      /* test [esi+ebx+2],8 */
            *(volatile uint16_t *)(tbl + recoff + 8) == id)        /* cmp [esi+ebx+8],ax */
            return (int32_t)(uint32_t)(uintptr_t)(tbl + recoff);   /* lea eax,[esi+ebx] */
        idx += 2;                                                  /* add edi,2 */
    } while (--count > 0);                                          /* dec ecx; jg */
    return 0;
}

/* tick_cache_effect_base (0x31ccd): a tiny effect-tick leaf — latch the record's base value. If the current
 * field word[rec+0xc] is 0, seed it from word[rec+8]; otherwise leave it. ESI=record. Returns EAX = the seeded
 * value on the seed path, else the preserved incoming EAX (0 under the ESI=rec->EAX dispatch). Pure record. */
int32_t tick_cache_effect_base(uint32_t rec)
{
    uint16_t cur = *(volatile uint16_t *)(uintptr_t)(rec + 0xc);
    if (cur != 0) return 0;                                          /* jne -> ret (eax preserved = 0) */
    uint16_t v = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    *(volatile uint16_t *)(uintptr_t)(rec + 0xc) = v;               /* mov [esi+0xc],ax */
    return v;                                                        /* eax = ax (low 16, high 0) */
}

/* init_command_timer_countdown (0x30b45): ESI=record, EBX=out (a queued-command timer slot). Compute the
 * countdown = word[rec+6], optionally RNG-scaled when rec[2]&2 ((v * (rng&0xffff)) >> 16 via the LCG at
 * g_command_rng_state 0x71f48), then +1; store out[4]=record-ptr, out[0]=countdown. void. */
void init_command_timer_countdown(uint32_t rec, uint32_t out)
{
    uint32_t v = *(volatile uint16_t *)(uintptr_t)(rec + 6);         /* ax = word[esi+6] */
    if (*(volatile uint8_t *)(uintptr_t)(rec + 2) & 2) {            /* test [esi+2],2 */
        uint32_t rng = (uint32_t)G32(VA_g_command_rng) * 0x5e5u + 0x29u;     /* LCG step */
        G32(VA_g_command_rng) = (int32_t)rng;
        v = (v * (rng & 0xffffu)) >> 16;                            /* imul eax,ecx; shr eax,0x10 */
    }
    v = (v & 0xffffu) + 1;                                          /* and eax,0xffff; inc eax */
    *(volatile uint32_t *)(uintptr_t)(out + 4) = rec;              /* mov [ebx+4],esi (record ptr) */
    *(volatile uint32_t *)(uintptr_t)out = v;                       /* mov [ebx],eax */
}

/* reset_command_chain_no_source (0x305a1): a reset_command_chain_state (0x305b6) variant that first clears the
 * pending-rotation flag g_command_pending_rot 0x8a264 and re-arms the warp sentinel g_warp_dest 0x8a260, then
 * falls into the shared reset body (run the chain from index AX). */
int32_t reset_command_chain_no_source(uint16_t ax)
{
    G16(VA_g_state_link_buf_ptr + 0x128) = 0;                                               /* mov word[0x8a264],0 */
    G32(VA_g_state_link_buf_ptr + 0x124) = (int32_t)0x80008000;                            /* mov [0x8a260],0x80008000 */
    return reset_command_chain_state(ax);                    /* jmp 0x305b6 */
}

/* ===== Active-effect pool ALLOC family — compose the lifted pool_alloc_handle (0x360f9), bridge only the
 * DAS-cache ensure-space (0x414d2). EAX = size -> EAX = handle (0 on failure). Prepends the new handle to an
 * effect list (head -> handle; *handle = chunk; chunk[0] = old head; chunk[6]=0, chunk[5]=0). ===== */

/* alloc_effect_record (0x34464): alloc + prepend to g_effect_list_a (0x8a118); also latches g_last_effect_handle
 * (0x89f50). The allocator bridged by ~10 effect registrants/handlers. */
uint32_t alloc_effect_record(uint32_t size)
{
    ensure_das_cache_heap_space(size);                      /* 0x414d2 re-pointed (DAS ensure) */
    uint32_t pool = (uint32_t)G32(VA_g_das_cache_heap_handle);
    uint32_t handle = pool_alloc_handle((uint32_t *)(uintptr_t)pool, (int32_t)size);
    G32(VA_g_resource_pool_small_flag + 0xf) = (int32_t)handle;                                 /* mov [0x89f50],eax */
    if (handle == 0) return 0;
    uint32_t old = (uint32_t)G32(VA_g_active_effect_pool);
    G32(VA_g_active_effect_pool) = (int32_t)handle;                                /* xchg [0x8a118],edx */
    uint32_t chunk = *(volatile uint32_t *)(uintptr_t)handle;      /* eax = *handle */
    *(volatile uint16_t *)(uintptr_t)(chunk + 6) = 0;
    *(volatile uint8_t  *)(uintptr_t)(chunk + 5) = 0;
    *(volatile uint32_t *)(uintptr_t)chunk = old;                  /* chunk[0] = old head */
    return chunk;                                                  /* eax = *handle (NOT the handle) */
}

/* alloc_effect_record_list_b (0x34499): same as alloc_effect_record but prepends to g_effect_list_b (0x8a11c)
 * and does NOT latch g_last_effect_handle. */
uint32_t alloc_effect_record_list_b(uint32_t size)
{
    ensure_das_cache_heap_space(size);                      /* 0x414d2 re-pointed (DAS ensure) */
    uint32_t pool = (uint32_t)G32(VA_g_das_cache_heap_handle);
    uint32_t handle = pool_alloc_handle((uint32_t *)(uintptr_t)pool, (int32_t)size);
    if (handle == 0) return 0;
    uint32_t old = (uint32_t)G32(VA_g_effect_list_b);
    G32(VA_g_effect_list_b) = (int32_t)handle;
    uint32_t chunk = *(volatile uint32_t *)(uintptr_t)handle;
    *(volatile uint16_t *)(uintptr_t)(chunk + 6) = 0;
    *(volatile uint8_t  *)(uintptr_t)(chunk + 5) = 0;
    *(volatile uint32_t *)(uintptr_t)chunk = old;
    return handle;
}

/* build_damage_emitter_from_matches (0x310bc): identical to build_effect_record_from_matches but allocates via
 * register_damage_emitter (0x344c9, list C). AX=key, EDX=base -> EAX=chunk (0 if no matches / alloc fails). */
uint32_t build_damage_emitter_from_matches(uint16_t key, uint32_t base)
{
    uint16_t outbuf[0xc8 + 8];
    uint32_t count = (uint32_t)collect_raw_state_matches(key, outbuf, 0xc8) & 0xffffu;
    if (count == 0) return 0;
    uint32_t ecx = count + 2;
    uint32_t size = (ecx * 2 + base + 3) & ~3u;
    uint32_t chunk = register_damage_emitter(size);
    if (chunk == 0) return 0;
    uint32_t dwords = (ecx + 1) >> 1;
    memcpy((void *)(uintptr_t)(chunk + base), outbuf, dwords * 4);
    return chunk;
}

/* tick_spawn_damage_emitter (0x3107d): ESI=record. If armed (rec[2]&0x21), build the damage-emitter record
 * (build_damage_emitter_from_matches, key word[rec+8], base 0xc), link the record back-ptr (chunk[8]=rec),
 * then walk its copied match list (count at chunk+0xc) resolving each entry to a FACE record offset via the
 * lifted find_face_record (0x4f567), storing the result back. Returns the last resolved offset (or 0). */
int32_t tick_spawn_damage_emitter(uint32_t rec)
{
    if (!(*(volatile uint8_t *)(uintptr_t)(rec + 2) & 0x21)) return 0;        /* je 0x30ab2 */
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    uint32_t chunk = build_damage_emitter_from_matches(key, 0xc);
    if (chunk == 0) return 0;                                                  /* je 0x310bb */
    *(volatile uint32_t *)(uintptr_t)(chunk + 8) = rec;                       /* chunk[8] = record */
    uint32_t ebx = chunk + 0xc;
    int32_t ecx = (int32_t)(*(volatile uint32_t *)(uintptr_t)ebx & 0xffffu);  /* count */
    ebx += 4;                                                                 /* skip {count,key} */
    int32_t ret = 0;
    do {
        uint16_t k = *(volatile uint16_t *)(uintptr_t)ebx;
        ret = find_face_record(k);                                     /* resolve -> face offset */
        *(volatile uint16_t *)(uintptr_t)ebx = (uint16_t)ret;
        ebx += 2;
    } while (--ecx > 0);
    return ret;
}

/* register_damage_emitter (0x344c9): like alloc_effect_record but prepends to g_effect_list_c (0x8a120) and
 * returns *handle (the chunk); no 0x89f50 latch. Composes pool_alloc_handle, bridges the DAS ensure (0x414d2). */
uint32_t register_damage_emitter(uint32_t size)
{
    ensure_das_cache_heap_space(size);                      /* 0x414d2 re-pointed (DAS ensure) */
    uint32_t pool = (uint32_t)G32(VA_g_das_cache_heap_handle);
    uint32_t handle = pool_alloc_handle((uint32_t *)(uintptr_t)pool, (int32_t)size);
    if (handle == 0) return 0;
    uint32_t old = (uint32_t)G32(VA_g_damage_emitter_ptr);
    G32(VA_g_damage_emitter_ptr) = (int32_t)handle;
    uint32_t chunk = *(volatile uint32_t *)(uintptr_t)handle;
    *(volatile uint16_t *)(uintptr_t)(chunk + 6) = 0;
    *(volatile uint8_t  *)(uintptr_t)(chunk + 5) = 0;
    *(volatile uint32_t *)(uintptr_t)chunk = old;
    return chunk;                                                  /* eax = *handle */
}

/* build_effect_record_from_matches (0x329d5): AX=key, EDX=base (per-record header size). Collect the RAW-STATE
 * matches (lifted collect_raw_state_matches 0x4f36d) into a local buffer; if any, alloc an effect record sized
 * align4(2*(count+2) + base) via the lifted alloc_effect_record, then copy (count+3)/2 dwords of the match
 * list into chunk+base. Returns the chunk (EAX), or 0 if no matches / alloc fails. NB the stride-2 dword copy
 * reads 1 word past the matches for an ODD count (uninitialized) — callers/tests use an EVEN match count. */
uint32_t build_effect_record_from_matches(uint16_t key, uint32_t base)
{
    uint16_t outbuf[0xc8 + 8];                                      /* the 0x190-byte local list */
    uint32_t count = (uint32_t)collect_raw_state_matches(key, outbuf, 0xc8) & 0xffffu;
    if (count == 0) return 0;                                      /* je 0x32a14 -> ret 0 */
    uint32_t ecx = count + 2;
    uint32_t size = (ecx * 2 + base + 3) & ~3u;                    /* align4(2*(count+2)+base) */
    uint32_t chunk = alloc_effect_record(size);
    if (chunk == 0) return 0;
    uint32_t dwords = (ecx + 1) >> 1;                              /* (count+3)/2 */
    memcpy((void *)(uintptr_t)(chunk + base), outbuf, dwords * 4); /* rep movsd es:[chunk+base] <- local */
    return chunk;
}

/* snapshot_keyed_secondary_records (0x32a20): the OBJECT-record twin of build_effect_record_from_matches — collect
 * the keyed OBJECT secondary records (collect_secondary_state_records_by_key 0x34c97, cap 0xc6) into a local list
 * laid out as {count, key, records...}, then allocate a sized effect chunk (alloc_effect_record 0x34464,
 * size = align4(2*(count+2) + base)) and copy (count+3)/2 dwords into chunk+base. Returns the chunk, or 0 when
 * there are no matches / the alloc fails. (Was BRIDGED inside cmd_change_object_texture/_height +
 * register_object_state_effect; composing the two already-verified callees de-bridges all three.) */
uint32_t snapshot_keyed_secondary_records(uint16_t key, uint32_t base)
{
    uint16_t outbuf[0xc8 + 8];                                      /* the 0x190-byte local list */
    outbuf[1] = key;                                               /* word[esp+2] = ax (key) */
    uint32_t count = (uint32_t)collect_secondary_state_records_by_key(
                         key, 0xc6, (uint32_t)(uintptr_t)(outbuf + 2)) & 0xffffu;
    if (count == 0) return 0;                                      /* je 0x32a6e -> ret 0 */
    outbuf[0] = (uint16_t)count;                                   /* word[esp] = count */
    uint32_t ecx = count + 2;
    uint32_t size = (ecx * 2 + base + 3) & ~3u;                    /* align4(2*(count+2)+base) */
    uint32_t chunk = alloc_effect_record(size);
    if (chunk == 0) return 0;
    uint32_t dwords = (ecx + 1) >> 1;                              /* (count+3)/2 */
    memcpy((void *)(uintptr_t)(chunk + base), outbuf, dwords * 4); /* rep movsd es:[chunk+base] <- {count,key,recs} */
    return chunk;
}

/* ===== Active-effect pool free family — compose the lifted pool_free_handle (0x360b3). The effect lists are
 * singly-linked through pool HANDLES: head -> node (handle); *node = chunk; chunk[0] = next node. ===== */

/* free_effect_list (0x34434): EAX = address of a list-head dword. Free every handle in the list (pool =
 * g_das_cache_heap_handle 0x85c3c) then clear the head. No pool -> just clear. Empty list -> return WITHOUT
 * clearing (the head is already 0). */
void free_effect_list(uint32_t head_ptr)
{
    uint32_t *head = (uint32_t *)(uintptr_t)head_ptr;
    uint32_t pool = (uint32_t)G32(VA_g_das_cache_heap_handle);
    if (pool == 0) { *head = 0; return; }                            /* je 0x3445b -> clear+ret */
    uint32_t node = *head;
    if (node == 0) return;                                           /* je 0x34461 -> ret, no clear */
    do {
        uint32_t chunk = *(volatile uint32_t *)(uintptr_t)node;      /* eax = *node */
        uint32_t next  = *(volatile uint32_t *)(uintptr_t)chunk;     /* eax = chunk[0] = next node */
        pool_free_handle((uint32_t *)(uintptr_t)pool, (uint32_t *)(uintptr_t)node);
        node = next;
    } while (node != 0);                                             /* or edx,edx; jne 0x34447 */
    *head = 0;                                                       /* mov [esi],0 (0x3445b) */
}

/* unlink_finished_effect (0x344f9): EAX = node (handle), EDX = address of the predecessor's link slot. Splice
 * the node out (*prev_link = chunk[0] = next) then free the handle. No pool null-check (caller guarantees it). */
void unlink_finished_effect(uint32_t node, uint32_t prev_link)
{
    uint32_t chunk = *(volatile uint32_t *)(uintptr_t)node;          /* ecx = *node */
    uint32_t next  = *(volatile uint32_t *)(uintptr_t)chunk;         /* ecx = chunk[0] */
    *(volatile uint32_t *)(uintptr_t)prev_link = next;               /* [edx] = next */
    uint32_t pool = (uint32_t)G32(VA_g_das_cache_heap_handle);
    pool_free_handle((uint32_t *)(uintptr_t)pool, (uint32_t *)(uintptr_t)node);
}

/* free_effect_pools (0x343f5): free all three active-effect lists (heads g_effect_list_a/b/c
 * 0x8a118/0x8a11c/0x8a120) via free_effect_list, then free the deferred-effect handle 0x8a0d8 if set
 * (clear it first). */
void free_effect_pools(void)
{
    free_effect_list((uint32_t)GADDR(VA_g_active_effect_pool));
    free_effect_list((uint32_t)GADDR(VA_g_effect_list_b));
    free_effect_list((uint32_t)GADDR(VA_g_damage_emitter_ptr));
    uint32_t h = (uint32_t)G32(VA_g_object_ptr_array);                             /* deferred-effect handle */
    if (h != 0) {
        G32(VA_g_object_ptr_array) = 0;
        uint32_t pool = (uint32_t)G32(VA_g_das_cache_heap_handle);
        pool_free_handle((uint32_t *)(uintptr_t)pool, (uint32_t *)(uintptr_t)h);
    }
}

/* swap_cell_state_linked_pair (0x33571): EDI = a moving-cell effect record. Swap geometry-cell state between
 * the record and its two linked geometry cells (g_map_geometry_buffer 0x90aa8). Cell A = geom + word[rec+8];
 * its flag byte A[0xa] keeps bits 0x7c and takes bits 0x83 from rec[0x14], while rec[0x14] takes A[0xa]'s old
 * full value. Cell B = geom + word[A+4]; then swap rec[0xa]<->B[2], rec[0x10]<->B[4], rec[0x12]<->B[6],
 * rec[7]<->B[8] (bytes), rec[0xc]<->B[0xa]. Pure geom+record leaf, no callees/selectors/heap. Returns EAX =
 * the OLD B[0xa] (the value loaded just before the final swap) — live into the rerun tail's preserved EAX. */
int32_t swap_cell_state_linked_pair(uint32_t rec)
{
    uint8_t *geom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    uint8_t *a = geom + *(volatile uint16_t *)(uintptr_t)(rec + 8);   /* cell A */
    uint8_t av = a[0xa];                                              /* flag-bit swap (0x7c kept / 0x83 from rec) */
    uint8_t tmp = *(volatile uint8_t *)(uintptr_t)(rec + 0x14);       /* xchg byte[edi+0x14],ah */
    *(volatile uint8_t *)(uintptr_t)(rec + 0x14) = av;
    a[0xa] = (uint8_t)((av & 0x7c) | (tmp & 0x83));
    uint8_t *b = geom + *(volatile uint16_t *)(a + 4);                /* cell B (linked via A+4) */
    uint16_t w; uint8_t c;
    w = *(volatile uint16_t *)(b + 2); *(volatile uint16_t *)(b + 2) = *(volatile uint16_t *)(uintptr_t)(rec + 0xa);  *(volatile uint16_t *)(uintptr_t)(rec + 0xa)  = w;
    w = *(volatile uint16_t *)(b + 4); *(volatile uint16_t *)(b + 4) = *(volatile uint16_t *)(uintptr_t)(rec + 0x10); *(volatile uint16_t *)(uintptr_t)(rec + 0x10) = w;
    w = *(volatile uint16_t *)(b + 6); *(volatile uint16_t *)(b + 6) = *(volatile uint16_t *)(uintptr_t)(rec + 0x12); *(volatile uint16_t *)(uintptr_t)(rec + 0x12) = w;
    c = b[8];                          b[8]                          = *(volatile uint8_t  *)(uintptr_t)(rec + 7);    *(volatile uint8_t  *)(uintptr_t)(rec + 7)    = c;
    w = *(volatile uint16_t *)(b + 0xa); *(volatile uint16_t *)(b + 0xa) = *(volatile uint16_t *)(uintptr_t)(rec + 0xc); *(volatile uint16_t *)(uintptr_t)(rec + 0xc) = w;
    return (int32_t)(uint32_t)w;                                      /* eax = old B[0xa] (mov ax,[ebx+0xa]) */
}

/* apply_light_delta_to_record_list (0x4f4a4): EAX = a match list {u16 count@0, u16 key@2, u16 offset[]@4}
 * (the collector output shape), EDX = signed brightness delta (low byte). For each listed geometry record
 * whose brightness byte es[off+0xb] is nonzero, add the delta and count it. ES = g_map_geometry_buffer
 * (0x90be8 selector, alias of 0x90aa8). Returns EAX = #records touched (0 if no selector / empty list).
 * edi is zero-extended once before the loop, so each offset is a clean u16. cx is the 16-bit do-while count. */
int32_t apply_light_delta_to_record_list(uint32_t list, uint32_t edx_delta)
{
    if (G16(VA_g_geometry_selector) == 0) return 0;                                /* no ES selector */
    if (*(volatile uint16_t *)(uintptr_t)list == 0) return 0;       /* empty list */
    uint8_t *es = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);      /* mov es,[0x90be8] (aliases 0x90aa8) */
    int16_t cx = (int16_t)*(volatile uint16_t *)(uintptr_t)list;    /* count */
    uint32_t p = list + 4;                                          /* skip {count,key} header */
    uint8_t bl = (uint8_t)edx_delta;
    int32_t applied = 0;
    do {
        uint16_t off = *(volatile uint16_t *)(uintptr_t)p; p += 2;
        if (es[off + 0xb] != 0) { es[off + 0xb] = (uint8_t)(es[off + 0xb] + bl); applied++; }
    } while (--cx > 0);                                             /* dec cx; jg (signed 16-bit) */
    return applied;
}

/* apply_flag_mask_to_record_list (0x4f4e8): EAX = a match list (same shape), EBX (low byte) = bits to CLEAR,
 * EDX (low byte) = bits to SET. For each listed record, es[off+0xa] = (es[off+0xa] & ~clear) | set. ES =
 * g_map_geometry_buffer (0x90be8). Returns EAX = list count (set once before the loop; 0 if no selector /
 * empty list). The `not dh` builds the AND mask = ~clear; dl is the OR mask. */
int32_t apply_flag_mask_to_record_list(uint32_t list, uint32_t ebx_clear, uint32_t edx_set)
{
    uint8_t dh = (uint8_t)~(uint8_t)ebx_clear;                      /* mov dh,bl; not dh -> AND mask */
    uint8_t dl = (uint8_t)edx_set;                                  /* OR mask */
    if (G16(VA_g_geometry_selector) == 0) return 0;
    if (*(volatile uint16_t *)(uintptr_t)list == 0) return 0;
    uint8_t *es = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    int16_t cx = (int16_t)*(volatile uint16_t *)(uintptr_t)list;
    int32_t ret = (int32_t)(uint16_t)cx;                            /* mov eax,ecx (count) — set once */
    uint32_t p = list + 4;
    do {
        uint16_t off = *(volatile uint16_t *)(uintptr_t)p; p += 2;
        es[off + 0xa] = (uint8_t)((es[off + 0xa] & dh) | dl);
    } while (--cx > 0);
    return ret;
}

/* collect_secondary_matches_into_struct (0x33072): EAX = a small struct {u16 cap@0, u16 key@2, u16 out[]@4}.
 * Collect the keyed OBJECT secondary records into out[] via the lifted collect_secondary_state_records_by_key
 * (0x34c97; eax=key, ebx=cap, edx=out -> EAX count, bumps g_object_match_count 0x8a110), then store the count
 * back into the struct's first word. void. (`ax=[edx+2]` key, `bx=[edx]` cap, `edx+=4` out, call, [st]=ax.) */
void collect_secondary_matches_into_struct(uint32_t st)
{
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(st + 2);
    uint16_t cap = *(volatile uint16_t *)(uintptr_t)(st + 0);
    int32_t count = collect_secondary_state_records_by_key(key, cap, st + 4);
    *(volatile uint16_t *)(uintptr_t)st = (uint16_t)count;
}

/* relocate_moving_objects_to_sectors (0x283ad): walk a linked list of moving-object nodes from EAX and
 * recompute each node's containing sector via the point-in-sector locate 0x3ee4b (BRIDGED — it loads the
 * ES/GS geometry selectors itself; the lift just re-passes the query). Node layout: +0 next, +4 sector
 * (also the locate's sector hint, ECX), +8 X (EAX), +0xa Y (EDX), +0x12 Z (EBX). 0x3ee4b `shl`s each of
 * EAX/EDX/EBX left 16 before storing, so only the low word matters — no residual-register hazard. EAX==0
 * -> return immediately (eax unchanged). Returns the last sector 0x3ee4b produced (EAX at ret). */
uint32_t relocate_moving_objects_to_sectors(uint32_t eax)
{
    if (eax == 0) return eax;                                       /* je 0x283d8 */
    uint32_t edi = eax;
    do {
        uint16_t x    = *(volatile uint16_t *)(uintptr_t)(edi + 8);
        uint16_t y    = *(volatile uint16_t *)(uintptr_t)(edi + 0xa);
        uint16_t z    = *(volatile uint16_t *)(uintptr_t)(edi + 0x12);
        uint32_t hint = *(volatile uint32_t *)(uintptr_t)(edi + 4);
        uint32_t sect = locate_sector_at_position(x, y, z, hint); /* 0x3ee4b re-pointed */
        *(volatile uint32_t *)(uintptr_t)(edi + 4) = sect;          /* store new sector */
        eax = sect;                                                 /* EAX at ret = last locate result */
        edi = *(volatile uint32_t *)(uintptr_t)edi;                 /* next node */
    } while (edi != 0);
    return eax;
}

/* relocate_moving_objects_if_dirty (0x15ee2): if the pickup-lock/moving-objects flag g@0x7fd84 bit0 is
 * ARMED, relocate the static list head g@0x71144 and return its runtime address; NOT armed -> return
 * NULL (15ee9 je 15efb; 15efb xor eax,eax; ret — the corpus decompile DROPPED this second ret path;
 * the old always-return fed [0x84958] forever = the floating-pickup-graphic bug: the item
 * overlay kept drawing at its last position after the fly animation disarmed). */
uint32_t relocate_moving_objects_if_dirty(void)
{
    if (!(*(volatile uint8_t *)(uintptr_t)(0x7fd84u + OBJ_DELTA) & 1))
        return 0;                                                   /* 0x15efb xor eax,eax; ret */
    relocate_moving_objects_to_sectors(0x71144u + OBJ_DELTA);
    return 0x71144u + OBJ_DELTA;                                    /* 0x15ef5 mov eax,0x471144; ret */
}

/* tick_particles (0x4b396): per-frame update of the active particle list (head g@0x91864), dt = g@0x85324.
 * Per particle (record fields: +0 next, +4 sector, +8 X, +0xa Y, +0x12 Z, +0x18 Zvel, +0x1c Xvel, +0x20 Yvel,
 * +0x24 Xacc, +0x28 Yacc, +0x2c Zacc, +0x30 busy, +0x31 static-flag, +0x32 lifetime):
 *   - lifetime -= dt; if <= 0 -> unlink (prev->next = next), clear busy, advance (prev unchanged);
 *   - else store lifetime; if static-flag == 0 integrate vertical physics: Zacc += dt*Zvel, Zvel -= dt*0x1900,
 *     bounce off the floor (sector's word[+2] << 16, or 0xf0000000 when sector==0) -> neg+halve Zvel & clamp;
 *     advance Xacc/Yacc by dt*Xvel/dt*Yvel; project the three accumulators >> 16 into X/Y/Z;
 *   - recompute the sector via the BRIDGED point-in-sector locate 0x3ee4b (hint = current sector), store it,
 *     set prev = this node. All multiplies are 32-bit truncating; shifts are arithmetic. */
void tick_particles(void)
{
    uint32_t esi = (uint32_t)G32(VA_g_particle_pool);
    uint32_t prev = 0x91864u + OBJ_DELTA;                          /* &head (prev-link slot) */
    if (esi == 0) return;
    do {
        int32_t life = (int32_t)*(volatile uint16_t *)(uintptr_t)(esi + 0x32);   /* movzx lifetime */
        life -= (int32_t)G32(VA_g_frame_time_scale);
        if (life > 0) {                                           /* alive (jg) */
            *(volatile uint16_t *)(uintptr_t)(esi + 0x32) = (uint16_t)life;
            if (*(volatile uint8_t *)(uintptr_t)(esi + 0x31) == 0) {              /* moving (jbe -> al==0) */
                int32_t floor;
                if (*(volatile uint32_t *)(uintptr_t)(esi + 4) != 0) {
                    uint32_t sp = (uint32_t)G32(VA_g_map_geometry_buffer) + *(volatile uint32_t *)(uintptr_t)(esi + 4);
                    floor = (int32_t)((uint32_t)(int32_t)(int16_t)*(volatile uint16_t *)(uintptr_t)(sp + 2) << 16);
                } else {
                    floor = (int32_t)0xf0000000u;
                }
                int32_t dt   = (int32_t)G32(VA_g_frame_time_scale);
                int32_t zvel = *(volatile int32_t *)(uintptr_t)(esi + 0x18);
                int32_t zacc = (int32_t)((uint32_t)*(volatile int32_t *)(uintptr_t)(esi + 0x2c)
                                         + (uint32_t)dt * (uint32_t)zvel);        /* Zacc += dt*Zvel */
                zvel = (int32_t)((uint32_t)zvel - (uint32_t)dt * 0x1900u);        /* Zvel -= dt*grav */
                if (zacc < floor) {                                              /* not jge -> bounce */
                    zvel = (int32_t)(0u - (uint32_t)zvel);
                    zacc = floor;
                    zvel >>= 1;
                }
                int32_t xadd = (int32_t)((uint32_t)*(volatile int32_t *)(uintptr_t)(esi + 0x1c) * (uint32_t)dt);
                *(volatile int32_t *)(uintptr_t)(esi + 0x24) =
                    (int32_t)((uint32_t)*(volatile int32_t *)(uintptr_t)(esi + 0x24) + (uint32_t)xadd);
                int32_t yadd = (int32_t)((uint32_t)*(volatile int32_t *)(uintptr_t)(esi + 0x20) * (uint32_t)dt);
                *(volatile int32_t *)(uintptr_t)(esi + 0x2c) = zacc;
                *(volatile int32_t *)(uintptr_t)(esi + 0x18) = zvel;
                int32_t xacc = *(volatile int32_t *)(uintptr_t)(esi + 0x24);
                *(volatile int32_t *)(uintptr_t)(esi + 0x28) =
                    (int32_t)((uint32_t)*(volatile int32_t *)(uintptr_t)(esi + 0x28) + (uint32_t)yadd);
                *(volatile uint16_t *)(uintptr_t)(esi + 8)    = (uint16_t)(xacc >> 16);
                *(volatile uint16_t *)(uintptr_t)(esi + 0x12) = (uint16_t)(zacc >> 16);
                int32_t yacc = *(volatile int32_t *)(uintptr_t)(esi + 0x28);
                *(volatile uint16_t *)(uintptr_t)(esi + 0xa)  = (uint16_t)(yacc >> 16);
            }
            uint32_t hint = *(volatile uint32_t *)(uintptr_t)(esi + 4);
            int32_t z = (int32_t)(int16_t)*(volatile uint16_t *)(uintptr_t)(esi + 0x12);
            int32_t y = (int32_t)(int16_t)*(volatile uint16_t *)(uintptr_t)(esi + 0xa);
            int32_t x = (int32_t)(int16_t)*(volatile uint16_t *)(uintptr_t)(esi + 8);
            uint32_t sect = locate_sector_at_position(      /* 0x3ee4b re-pointed */
                (uint32_t)x, (uint32_t)y, (uint32_t)z, hint);
            prev = esi;                                            /* mov edx, esi */
            *(volatile uint32_t *)(uintptr_t)(esi + 4) = sect;
        } else {                                                  /* dead */
            uint32_t next = *(volatile uint32_t *)(uintptr_t)esi;
            *(volatile uint32_t *)(uintptr_t)prev = next;          /* unlink */
            *(volatile uint8_t *)(uintptr_t)(esi + 0x30) = 0;      /* clear busy */
        }
        esi = *(volatile uint32_t *)(uintptr_t)esi;
    } while (esi != 0);
}

/* spawn_particle (0x4b4e9): allocate a particle (alloc_particle) and seed it at a point. Args EAX=X,
 * ECX=type/id (-> word+0xc), EDX=Y, EBX=Z. Stores X/Y/Z words + their <<16 accumulators, locates the sector
 * via the BRIDGED 0x3ee4b (hint 0), sets the fixed kind/flags (+0xf=2, +0x11=0x10), then derives the velocity
 * vector from two rng draws (0x4b4cb): Zvel = (rng<<2) + 0x2e630; a heading rng selects a sincos pair from the
 * table g@0x72080 (idx = heading&0x1ff and (heading+0x80)&0x1ff) scaled by heading>>9, >>3 into Xvel/Yvel;
 * byte+0xe = (heading+0x100)>>1; lifetime +0x32 = 0x12c. Returns the slot (0 if the pool was full). */
uint32_t spawn_particle(uint32_t eax, uint32_t ecx, uint32_t edx, uint32_t ebx)
{
    uint32_t edi = eax;                                            /* X */
    uint32_t esi = alloc_particle();
    if (esi == 0) return 0;
    *(volatile uint16_t *)(uintptr_t)(esi + 0xc)  = (uint16_t)ecx;
    *(volatile uint16_t *)(uintptr_t)(esi + 8)    = (uint16_t)edi;
    *(volatile uint16_t *)(uintptr_t)(esi + 0xa)  = (uint16_t)edx;
    *(volatile uint16_t *)(uintptr_t)(esi + 0x12) = (uint16_t)ebx;
    *(volatile int32_t *)(uintptr_t)(esi + 0x24)  = (int32_t)(edi << 16);
    *(volatile int32_t *)(uintptr_t)(esi + 0x28)  = (int32_t)(edx << 16);
    *(volatile int32_t *)(uintptr_t)(esi + 0x2c)  = (int32_t)(ebx << 16);
    uint32_t sect = locate_sector_at_position(              /* 0x3ee4b re-pointed */
        (uint32_t)(int32_t)(int16_t)edi, (uint32_t)(int32_t)(int16_t)edx,
        (uint32_t)(int32_t)(int16_t)ebx, 0);
    *(volatile uint8_t  *)(uintptr_t)(esi + 0xf)  = 2;
    *(volatile uint8_t  *)(uintptr_t)(esi + 0x11) = 0x10;
    *(volatile uint32_t *)(uintptr_t)(esi + 4)    = sect;
    uint32_t r1 = rng_next(0) & 0xffff;
    *(volatile int32_t *)(uintptr_t)(esi + 0x18)  = (int32_t)((r1 << 2) + 0x2e630u);
    uint32_t h = rng_next(0) & 0xffff;                      /* heading */
    int32_t hs9 = (int32_t)(h >> 9);
    *(volatile uint8_t *)(uintptr_t)(esi + 0xe) = (uint8_t)((h + 0x100) >> 1);
    int32_t s1 = (int32_t)*(volatile int16_t *)(uintptr_t)(0x72080u + OBJ_DELTA + (h & 0x1ff) * 2);
    int32_t xv = (int32_t)((uint32_t)s1 * (uint32_t)hs9);
    int32_t s2 = (int32_t)*(volatile int16_t *)(uintptr_t)(0x72080u + OBJ_DELTA + ((h + 0x80) & 0x1ff) * 2);
    int32_t yv = (int32_t)((uint32_t)hs9 * (uint32_t)s2);
    *(volatile uint16_t *)(uintptr_t)(esi + 0x32) = 0x12c;
    *(volatile int32_t *)(uintptr_t)(esi + 0x1c) = xv >> 3;
    *(volatile int32_t *)(uintptr_t)(esi + 0x20) = yv >> 3;
    return esi;
}

/* tick_moving_sector (0x32c05): the per-frame sector-SCROLL effect (EAX=effect record; rec[8]=sub-record edi).
 * Paused (byte[edi+2]&8) -> ret (eax=rec preserved); armed (byte[rec+5]&0x40) -> count the timer down
 * (rawcmd_texture_countdown). Otherwise compute a per-frame scroll budget dt*byte[edi+7] (negated on the Y axis,
 * byte[rec+5]&0x80) and clamp it so the value at the secondary-geometry buffer g@0x90aac + word[rec+0x14|0xe]
 * doesn't overshoot the target -2*sword[edi+0xa|0xc]; if it's already at target, exit via rawcmd_tick_height_exit.
 * Apply the (clamped) delta to four sgb corners (rec+0xe/0x10/0x14/0x12), carry the player/objects when byte
 * [edi+6]&5 (apply_moving_sector_carry, move vector -> g@0x8a124/0x8a128, delta g@0x8a12c=0), then fold the low
 * byte of the delta into the geometry byte-fields selected by byte[edi+6]&0xf. Models EAX's residual high half
 * (= high of the signed dt*speed product) into the 0x8a124/0x8a128 writes. Returns 0 on the main/armed paths. */
uint32_t tick_moving_sector(uint32_t rec)
{
    uint32_t esi = rec;
    uint32_t edi = *(volatile uint32_t *)(uintptr_t)(rec + 8);
    if (*(volatile uint8_t *)(uintptr_t)(edi + 2) & 8) return rec;          /* paused -> 0x3305e (eax=rec) */
    int32_t dt = (int32_t)G32(VA_g_frame_time_scale);
    if (*(volatile uint8_t *)(uintptr_t)(esi + 5) & 0x40)                   /* armed -> 0x3305f countdown */
        return (uint32_t)rawcmd_texture_countdown(esi, dt);

    uint32_t sgb = (uint32_t)G32(VA_g_sector_geom_base);
    uint8_t speed = *(volatile uint8_t *)(uintptr_t)(edi + 7);
    uint32_t budget_dw; int16_t target; uint16_t off;
    if (!(*(volatile uint8_t *)(uintptr_t)(esi + 5) & 0x80)) {              /* axis X */
        budget_dw = (uint32_t)(dt * (int32_t)(uint32_t)speed);
        target = (int16_t)(-2 * (int32_t)*(volatile int16_t *)(uintptr_t)(edi + 0xa));
        off = *(volatile uint16_t *)(uintptr_t)(esi + 0x14);
    } else {                                                               /* axis Y */
        budget_dw = (uint32_t)(-(dt * (int32_t)(uint32_t)speed));
        target = (int16_t)(-2 * (int32_t)*(volatile int16_t *)(uintptr_t)(edi + 0xc));
        off = *(volatile uint16_t *)(uintptr_t)(esi + 0xe);
    }
    int16_t current = *(volatile int16_t *)(uintptr_t)(sgb + off);
    if (current == target) return (uint32_t)rawcmd_tick_height_exit(esi);  /* je 0x32ffd */
    int16_t budget = (int16_t)budget_dw;
    int16_t tmp = (int16_t)(current + budget);
    int axisY = (*(volatile uint8_t *)(uintptr_t)(esi + 5) & 0x80) != 0;
    if ((!axisY && tmp > target) || (axisY && tmp < target))
        budget = (int16_t)(budget - (int16_t)(tmp - target));              /* clamp to target */
    uint32_t eax_full = (budget_dw & 0xffff0000u) | (uint16_t)budget;       /* mov ax,word[esp]: residual high */

    uint16_t delta = (uint16_t)budget;
    *(volatile uint16_t *)(uintptr_t)(sgb + *(volatile uint16_t *)(uintptr_t)(esi + 0xe))  += delta;
    *(volatile uint16_t *)(uintptr_t)(sgb + *(volatile uint16_t *)(uintptr_t)(esi + 0x10)) += delta;
    *(volatile uint16_t *)(uintptr_t)(sgb + *(volatile uint16_t *)(uintptr_t)(esi + 0x14)) += delta;
    *(volatile uint16_t *)(uintptr_t)(sgb + *(volatile uint16_t *)(uintptr_t)(esi + 0x12)) += delta;

    uint8_t f6 = *(volatile uint8_t *)(uintptr_t)(edi + 6);
    uint8_t cl = 3;
    if (!(f6 & 1)) cl &= 0xfe;
    if (!(f6 & 4)) cl &= 0xfd;
    if (cl != 0) {
        uint32_t vec_x = (f6 & 0x40) ? eax_full : 0;
        uint32_t vec_y = (f6 & 0x40) ? 0 : eax_full;
        G32(VA_g_player_move_delta_x) = (int32_t)vec_x; G32(VA_g_player_move_delta_y) = (int32_t)vec_y; G32(VA_g_player_move_delta_z) = 0;
        uint8_t list[0x10]; memset(list, 0, sizeof list);
        *(uint32_t *)list = 1; *(uint16_t *)(list + 4) = *(volatile uint16_t *)(uintptr_t)(esi + 0xc);
        apply_moving_sector_carry(cl, list);
    }
    uint8_t al = (uint8_t)eax_full;
    uint32_t ebx = (uint32_t)*(volatile uint16_t *)(uintptr_t)(esi + 0xc) + (uint32_t)G32(VA_g_map_geometry_buffer);
    uint32_t e2 = (f6 & 0x40) ? 0 : 1;
    uint8_t cl2 = f6;
    do {
        if (cl2 & 1) { *(volatile uint8_t *)(uintptr_t)(e2 + ebx + 0x12) += al; cl2 -= 1; if (cl2 == 0) break; }
        if (cl2 & 2) { *(volatile uint8_t *)(uintptr_t)(e2 + ebx + 0x10) += al; cl2 -= 2; if (cl2 == 0) break; }
        ebx = (uint32_t)*(volatile uint16_t *)(uintptr_t)(ebx + 0x18);
        if (ebx == 0) break;
        ebx += (uint32_t)G32(VA_g_map_geometry_buffer);
        if (cl2 & 4) { *(volatile uint8_t *)(uintptr_t)(e2 + ebx + 0xa) += al; cl2 -= 4; if (cl2 == 0) break; }
        if (cl2 & 8) { *(volatile uint8_t *)(uintptr_t)(e2 + ebx + 4) += al; }
    } while (0);
    return 0;
}

/* tick_move_floorceil (0x340f3): the per-frame floor/ceiling MOVE effect (ESI=effect record). Gathers the
 * keyed sectors (rec[8]) into a local list (connected flood 0x4f3d0 when byte[rec+2]&4, else linear 0x4f313),
 * snapshots each sector's current floor (and, for the linked-ceiling variant byte[rec+6]&3, a flag byte +
 * a linked-ceiling pair) into a freshly allocated effect record (alloc_effect_record_list_b, *handle=chunk,
 * data at chunk+8), then drives the actual move (apply_floor_move_to_group 0x3423e for the simple variant /
 * apply_floorceil_move_to_group 0x3427b for the linked one) UNLESS the direction word is negative (byte[rec+2]
 * &2), the effect is paused (byte[rec+2]&8) or a count command is active (g@0x89f5c). The target floor height
 * is 2*word[rec+0xa]. Tail-jumps to the shared effect epilogue 0x34086 (BRIDGED). */
void tick_move_floorceil(uint32_t rec)
{
    uint16_t outbuf[0xc8 + 8];
    uint8_t al = *(volatile uint8_t *)(uintptr_t)(rec + 2);
    int32_t edx = (al & 2) ? -2 : (int32_t)(2u * (uint32_t)*(volatile uint16_t *)(uintptr_t)(rec + 0xa));
    uint32_t connected = (al >> 2) & 1;
    uint16_t key = *(volatile uint16_t *)(uintptr_t)(rec + 8);
    uint32_t count = connected
        ? (uint32_t)collect_connected_geometry_group(key, 0xc8, (uint32_t)(uintptr_t)outbuf) & 0xffffu
        : geom_find_matches(key, 0xc8, (uint8_t *)outbuf) & 0xffffu;
    if (count != 0) {
        uint32_t geom = (uint32_t)G32(VA_g_map_geometry_buffer);
        uint16_t *src = outbuf + 2;                                /* skip {count,key} header */
        if (*(volatile uint8_t *)(uintptr_t)(rec + 6) & 3) {       /* linked-ceiling variant */
            uint32_t handle = alloc_effect_record_list_b((count << 3) + 8);
            if (handle != 0) {
                *(volatile uint32_t *)(uintptr_t)(rec + 0x10) = handle;
                uint8_t *edi = (uint8_t *)(uintptr_t)(*(volatile uint32_t *)(uintptr_t)handle + 8);
                for (uint32_t k = 0; k < count; k++) {
                    uint32_t cell = geom + src[k];
                    *(volatile uint16_t *)edi = *(volatile uint16_t *)(uintptr_t)(cell + 2);          /* floor */
                    *(volatile uint8_t *)(edi + 2) = *(volatile uint8_t *)(uintptr_t)(cell + 0xc);    /* flag */
                    uint16_t link = *(volatile uint16_t *)(uintptr_t)(cell + 0x18);
                    if (link != 0) {
                        uint32_t lc = geom + link;
                        *(volatile uint16_t *)(edi + 4) = *(volatile uint16_t *)(uintptr_t)(lc + 8);
                        *(volatile uint16_t *)(edi + 6) = *(volatile uint16_t *)(uintptr_t)(lc + 2);
                    }
                    edi += 8;
                }
                if (edx >= 0 && !(*(volatile uint8_t *)(uintptr_t)(rec + 2) & 8) && (uint32_t)G32(VA_g_state_record_list_count) == 0)
                    apply_floorceil_move_to_group(rec, (uint32_t)(uintptr_t)outbuf);
            }
        } else {                                                  /* simple floor variant */
            uint32_t handle = alloc_effect_record_list_b((count << 1) + 8);
            if (handle != 0) {
                *(volatile uint32_t *)(uintptr_t)(rec + 0x10) = handle;
                uint8_t *edi = (uint8_t *)(uintptr_t)(*(volatile uint32_t *)(uintptr_t)handle + 8);
                for (uint32_t k = 0; k < count; k++) {
                    uint32_t cell = geom + src[k];
                    *(volatile uint16_t *)edi = *(volatile uint16_t *)(uintptr_t)(cell + 2);          /* floor */
                    edi += 2;
                }
                if (edx >= 0 && !(*(volatile uint8_t *)(uintptr_t)(rec + 2) & 8) && (uint32_t)G32(VA_g_state_record_list_count) == 0)
                    apply_floor_move_to_group((uint32_t)(uintptr_t)outbuf,
                        (uint16_t)(2u * (uint32_t)*(volatile uint16_t *)(uintptr_t)(rec + 0xa)), 0);
            }
        }
    }
    tick_rerun_command_execute(rec);                        /* 0x34086 re-pointed (esi=rec, eax=0) */
}

/* alloc_face_effect (0x3296b): collect geometry matches for EAX=key into a local list, then allocate a sized
 * effect record (lifted alloc_effect_record) and copy the list in, EXPANDING each 16-bit match to a 32-bit
 * entry. EBX selects the collector (nonzero -> connected flood 0x4f3d0 over the ES geometry; 0 -> linear
 * geom_find_matches 0x4f313); EDX = base (header offset within the record). Record size = align4((count+2)*4 +
 * base). Copy: the {count,key} header is copied as one raw dword (movsd); then for count+1 more entries each
 * 16-bit word is written as a dword whose HIGH half is the chunk pointer's high half (a residual-EAX quirk of
 * the original `mov ax,[esi]; mov [edi],eax`), and the loop reads ONE word past the match list (uninitialized;
 * harmless — callers ignore it). Returns the chunk, or 0 on no-match / alloc failure. */
uint32_t alloc_face_effect(uint16_t key, uint32_t mode, uint32_t base)
{
    uint16_t outbuf[0xc8 + 8];
    uint32_t count;
    if (mode != 0)
        count = (uint32_t)collect_connected_geometry_group(key, 0xc8, (uint32_t)(uintptr_t)outbuf) & 0xffffu;
    else
        count = geom_find_matches(key, 0xc8, (uint8_t *)outbuf) & 0xffffu;
    if (count == 0) return 0;
    uint32_t ecx = count + 2;
    uint32_t size = (ecx * 4 + base + 3) & ~3u;
    uint32_t chunk = alloc_effect_record(size);
    if (chunk == 0) return 0;
    uint8_t *dst = (uint8_t *)(uintptr_t)(chunk + base);
    uint8_t *src = (uint8_t *)outbuf;
    *(uint32_t *)dst = *(uint32_t *)src;                       /* movsd: {count,key} header dword */
    dst += 4; src += 4;
    uint32_t rem = ecx - 1;                                    /* dec ecx */
    while ((int32_t)rem > 0) {                                 /* dec ecx / jg */
        *(uint32_t *)dst = (chunk & 0xffff0000u) | *(uint16_t *)src;
        dst += 4; src += 2; rem--;
    }
    return chunk;
}

/* spawn_particle_on_edge (0x4b5b4): allocate a particle and seed it at a random point ALONG a wall edge (no
 * 0x3ee4b locate — the edge already knows its sector). Args EAX = Z-range descriptor (word[0]=Zlo, word[2]=Zhi),
 * EDX = edge descriptor (word[0]/word[2] = vertex offsets into the secondary geometry buffer g@0x90aac, word[6]
 * = sector), EBX = type. Draws 4 rng (0x4b4cb): t lerps X/Y between the two edge vertices (interp = vA + (dX*t)
 * >>16); rngZ places Z within [Zhi+span/4 .. ] where span = Zlo-Zhi (Z = Zhi + span/4 + ((3*span/4)*rngZ)>>16);
 * Zvel = rng-0x8000; heading rng -> sincos(g@0x72080) Xvel/Yvel as in spawn_particle. Fixed: +0x10=0x80, +0xf=2,
 * +0x11=0x10, +0xe=(heading+0x100)>>1, lifetime +0x32=0x8c. Returns the slot (0 if full). */
uint32_t spawn_particle_on_edge(uint32_t eax, uint32_t edx, uint32_t ebx)
{
    uint32_t zrange = eax, edge = edx, type = ebx;
    uint32_t slot = alloc_particle();
    if (slot == 0) return 0;
    uint32_t sgb = (uint32_t)G32(VA_g_sector_geom_base);
    uint32_t vB = sgb + *(volatile uint16_t *)(uintptr_t)(edge + 2);
    uint32_t vA = sgb + *(volatile uint16_t *)(uintptr_t)(edge + 0);
    uint32_t t = rng_next(0) & 0xffff;
    int32_t vAx = (int32_t)(int16_t)*(volatile uint16_t *)(uintptr_t)(vA + 8);
    int32_t vBx = (int32_t)(int16_t)*(volatile uint16_t *)(uintptr_t)(vB + 8);
    int32_t interpX = vAx + ((int32_t)((uint32_t)(vBx - vAx) * t) >> 16);
    int32_t vBy = (int32_t)(int16_t)*(volatile uint16_t *)(uintptr_t)(vB + 0xa);
    int32_t vAy = (int32_t)(int16_t)*(volatile uint16_t *)(uintptr_t)(vA + 0xa);
    int32_t interpY = vAy + ((int32_t)((uint32_t)(vBy - vAy) * t) >> 16);
    int32_t zhi = (int32_t)(int16_t)*(volatile uint16_t *)(uintptr_t)(zrange + 2);
    int32_t zlo = (int32_t)(int16_t)*(volatile uint16_t *)(uintptr_t)(zrange + 0);
    int32_t span = zlo - zhi;
    uint32_t rngZ = rng_next(0) & 0xffff;
    int32_t b = ((int32_t)((uint32_t)span * 3u)) >> 2;            /* (3*span)>>2 */
    b = (int32_t)((uint32_t)b * rngZ);
    int32_t zbase = zhi + (span >> 2);
    int32_t z = zbase + (b >> 16);
    *(volatile uint8_t  *)(uintptr_t)(slot + 0x10) = 0x80;
    *(volatile uint16_t *)(uintptr_t)(slot + 0xc)  = (uint16_t)type;
    *(volatile uint16_t *)(uintptr_t)(slot + 8)    = (uint16_t)interpX;
    *(volatile uint16_t *)(uintptr_t)(slot + 0xa)  = (uint16_t)interpY;
    *(volatile uint16_t *)(uintptr_t)(slot + 0x12) = (uint16_t)z;
    *(volatile int32_t *)(uintptr_t)(slot + 0x24)  = (int32_t)((uint32_t)interpX << 16);
    *(volatile int32_t *)(uintptr_t)(slot + 0x2c)  = (int32_t)((uint32_t)z << 16);
    *(volatile int32_t *)(uintptr_t)(slot + 0x28)  = (int32_t)((uint32_t)interpY << 16);
    *(volatile uint8_t  *)(uintptr_t)(slot + 0xf)  = 2;
    *(volatile uint8_t  *)(uintptr_t)(slot + 0x11) = 0x10;
    *(volatile uint32_t *)(uintptr_t)(slot + 4)    = *(volatile uint16_t *)(uintptr_t)(edge + 6);
    uint32_t rzv = rng_next(0) & 0xffff;
    *(volatile int32_t *)(uintptr_t)(slot + 0x18)  = (int32_t)(rzv + 0xffff8000u);
    uint32_t h = rng_next(0) & 0xffff;
    int32_t hs9 = (int32_t)(h >> 9);
    *(volatile uint8_t *)(uintptr_t)(slot + 0xe) = (uint8_t)((h + 0x100) >> 1);
    int32_t s1 = (int32_t)*(volatile int16_t *)(uintptr_t)(0x72080u + OBJ_DELTA + (h & 0x1ff) * 2);
    int32_t xv = (int32_t)((uint32_t)s1 * (uint32_t)hs9);
    int32_t s2 = (int32_t)*(volatile int16_t *)(uintptr_t)(0x72080u + OBJ_DELTA + ((h + 0x80) & 0x1ff) * 2);
    int32_t yv = (int32_t)((uint32_t)s2 * (uint32_t)hs9);
    *(volatile uint16_t *)(uintptr_t)(slot + 0x32) = 0x8c;
    *(volatile int32_t *)(uintptr_t)(slot + 0x1c) = xv >> 3;
    *(volatile int32_t *)(uintptr_t)(slot + 0x20) = yv >> 3;
    return slot;
}
