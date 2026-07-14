/* lift_dbase100.c — the ROTH `dbase100_interpreter` subsystem lifted to verified C.
 * Own TU per docs/operating/recomp.md §4.6.
 * lift-lens: docs/reference/lift/dbase100_interpreter.md; behavior/dispatch:
 * docs/reference/ROTH_command_system_notes.md (DBASE100 section).
 *
 * DBASE100 = the threaded-bytecode scripting brain: 4-byte records
 *   { opcode=(word>>24)&0x7f, bit31=If-NOT, operand=word&0xffffff }
 * that drive dialogue/item-use/entity-spawn/cutscene/audio. It fans out into nearly every
 * gameplay subsystem, so the interpreter core + dialogue-queue are in-game-only (non-idempotent
 * bridges); the record-access + entity-def-builder leaves are pure and oracle-verified here.
 *
 * Bottom-up lift order: A record-access leaves -> D entity-def builder ->
 * C dialogue-script/queue leaves -> C mid/entry -> B interpreter core.
 *
 * ABI is derived from the DISASM (the corpus is Borland-on-Watcom, unreliable — recomp.md §9.0).
 * Stored-pointer globals (g_dbase100_base 0x81e1c, g_dbase100_inventory_table 0x81e20,
 * g_dbase100_record_bitmap 0x81e28, ...) hold RUNTIME host addresses — read the stored value with
 * G32() then cast RAW to a pointer; never re-apply the canon GADDR delta (gotcha A4).
 */
#include "common.h"
#include "engine.h"
#include <string.h>

/* ---- Layer A: filter_dbase100_active_records (0x1d146) — pure, oracle-able ----
 * Pre-walks a threaded 4-byte-record chain and segments it into the active-record list the choice/
 * dialogue UI consumes. Self-contained leaf: the only callee is the already-lifted
 * test_dbase100_record_flag (0x1cb35) — no bridges.
 *
 * ABI (from disasm 0x1d146):
 *   EAX = cursor_p   — &slot holding the current 4-byte-record pointer; *slot advances by 4/record
 *   EDX = count      — number of records to scan (loop counter)
 *   EBX = out_p      — output struct base: array of 8-byte entries { u32 ptr; u32 count }
 *   ECX = result_p   — &slot; receives the active-record count
 *   ret EAX          — the loop counter's final (post-decrement) value
 *
 * The Ghidra body's switch is a balanced BST over the opcode; it is pure classification (no
 * side effects until the leaves), so it is faithfully reproduced as a three-way classify:
 *   KEEP set = {5,7,0xe,0x11,0x1a,0x1d,0x23,0x26,0x2d,0x36,0x37}; IF = {0xd}; else SKIP.
 * Opcode 0xc terminates the scan (checked at the loop tail, common to all paths).
 */
int32_t filter_dbase100_active_records(uint32_t cursor_p, uint32_t count,
                                              uint32_t out_p, uint32_t result_p)
{
    uint32_t *cursor = (uint32_t *)(uintptr_t)cursor_p;   /* esi */
    int32_t   edi    = (int32_t)count;                    /* edi (loop counter) */
    uint8_t  *edx    = (uint8_t *)(uintptr_t)out_p;       /* edx (moving output ptr) */
    uint32_t *result = (uint32_t *)(uintptr_t)result_p;   /* ecx */
    int32_t   active_count = 0;                           /* [ebp-0xc] */
    int32_t   cond_flag    = 0;                           /* [ebp-8]  */

    /* prologue: seed entry[0] from the first record (count field cleared first) */
    *(uint32_t *)(edx + 4) = 0;
    *(uint32_t *)(edx + 0) = *cursor;

    for (;;) {
        uint32_t *rec_ptr = (uint32_t *)(uintptr_t)(*cursor);  /* [ebp-0x14] */
        uint32_t word     = *rec_ptr;                          /* the 4-byte record */
        uint32_t operand  = word & 0x00ffffffu;                /* [ebp-0x10] */
        uint32_t notbit   = word & 0x80000000u;                /* [ebp-4] (If-NOT bit) */
        int32_t  opcode   = (int32_t)((word >> 24) & 0x7fu);   /* ebx (sar then mask) */
        *cursor = (uint32_t)((uintptr_t)rec_ptr + 4);          /* advance cursor by one record */

        /* opcode classification (faithful collapse of the side-effect-free BST) */
        switch (opcode) {
        case 0x05: case 0x07: case 0x0e: case 0x11: case 0x1a: case 0x1d:
        case 0x23: case 0x26: case 0x2d: case 0x36: case 0x37:
            /* KEEP (0x1d1bf): close the current segment, open a new one at the next record */
            if (cond_flag == 0) {
                edx += 8;
                cond_flag = 0;                      /* redundant (already 0 here) */
                active_count++;
                *(uint32_t *)(edx - 4) += 1;        /* bump the segment we just closed */
            }
            *(uint32_t *)(edx + 4) = 0;
            *(uint32_t *)(edx + 0) = *cursor;       /* next record (cursor already advanced) */
            break;

        case 0x0d: {
            /* IF (0x1d1e5): test a progress flag; set cond_flag iff the condition FAILS */
            int32_t r = test_dbase100_record_flag(operand);
            if (notbit) { if (r != 0) cond_flag = 1; }   /* If-NOT: fail when flag set   */
            else        { if (r == 0) cond_flag = 1; }   /* If    : fail when flag clear  */
            *(uint32_t *)(edx + 4) += 1;                 /* falls into the SKIP action */
            break;
        }

        default:
            /* SKIP (0x1d1b7): count this record into the current segment */
            *(uint32_t *)(edx + 4) += 1;
            break;
        }

        /* loop tail (0x1d23f): opcode 0xc terminates; otherwise loop while counter stays > 0 */
        edi--;
        if (opcode == 0x0c || edi <= 0) {
            *result = (uint32_t)active_count;
            return edi;
        }
    }
}

/* ---- Layer D: build_entity_def_record (0x1e128) — pure, oracle-able ----
 * Zeroes a 0x6c-byte entity-def struct, copies the record ID, then scans the record's trigger-block
 * list (record+0x14) for the AsMonster block (trigger code 0x0A) and scatters its sub-record
 * opcodes (0x24..0x32) into the def struct's fields. The 0x80 bit of the opcode byte selects the
 * secondary attack slot; some opcodes append into per-slot arrays (count + body).
 *
 * ABI (from disasm 0x1e128):  EAX = dest def struct (0x6c B), EDX = source DBASE100 record.
 *   ret EAX = 1 if an AsMonster block was found+processed, 0 if none (hit a 0-size terminator).
 *
 * The Ghidra `flow_succ -> give_item 0x1cedc` is a Ghidra ARTIFACT, not a real fall-through: the
 * function tail-jmps to the shared Watcom epilogue at 0x1d071 (leave; pop edi/esi/ecx/ebx; ret)
 * that merely sits just before give_item 0x1d077. So this is a normal `return`, no shared body.
 *
 * Only callee is the already-lifted mem_fill (0x4b360, edx=0/ebx=0x6c = memset to 0). No bridges.
 */
uint32_t build_entity_def_record(uint32_t dest, uint32_t record)
{
    uint8_t *d   = (uint8_t *)(uintptr_t)dest;
    uint8_t *src = (uint8_t *)(uintptr_t)record;

    mem_fill(d, 0, 0x6c);                            /* zero the def struct */
    *(uint16_t *)(d + 4) = *(uint16_t *)(src + 2);          /* def ID <- record ID (record+2) */

    uint8_t *blk = src + 0x14;                              /* trigger-block list */
    for (;;) {
        uint32_t hdr  = *(uint32_t *)blk;
        uint32_t size = hdr & 0x00ffffffu;                 /* block byte size (incl. 4B header) */
        if (size == 0) return 0;                            /* terminator -> no AsMonster block */
        uint32_t code = hdr >> 24;                          /* trigger code */
        uint32_t nsub = size >> 2;                          /* dword count (incl. header) */
        if (code != 0x0a) { blk += (nsub << 2); continue; } /* skip non-AsMonster block */

        blk += 4;                                           /* skip header word */
        int32_t limit = (int32_t)nsub - 1;                 /* sub-record count */
        for (int32_t i = 0; i < limit; i++) {
            uint32_t w   = *(uint32_t *)blk;
            uint32_t top = w >> 24;                         /* full top byte (incl. 0x80 sec bit) */
            int32_t  op  = (int32_t)(top & 0x7f) - 0x24;    /* dispatch index */
            blk += 4;
            uint32_t operand = w & 0x00ffffffu;
            uint16_t ax  = (uint16_t)operand;
            uint8_t  al  = (uint8_t)operand;
            int      sec = (top & 0x80) != 0;
            if ((uint32_t)op > 0xe) continue;              /* opcode out of 0x24..0x32 -> ignore */

            switch (op) {
            case 0x24 - 0x24: *(uint16_t *)(d + 0x0c) = ax; break;
            case 0x25 - 0x24: *(uint16_t *)(d + 0x0e) = ax; break;
            case 0x26 - 0x24: *(uint32_t *)(d + 0x08) = operand; break;   /* 32-bit (top byte 0) */
            case 0x27 - 0x24:
                if (sec) { uint16_t k = *(uint16_t *)(d + 0x46);
                           *(uint16_t *)(d + k * 2 + 0x48) = ax; *(uint16_t *)(d + 0x46) = (uint16_t)(k + 1); }
                else     { uint16_t k = *(uint16_t *)(d + 0x2c);
                           *(uint16_t *)(d + k * 2 + 0x2e) = ax; *(uint16_t *)(d + 0x2c) = (uint16_t)(k + 1); }
                break;
            case 0x28 - 0x24: *(uint16_t *)(d + (sec ? 0x12 : 0x10)) = ax; break;
            case 0x29 - 0x24:
                if (sec) { uint16_t k = *(uint16_t *)(d + 0x22);
                           *(uint16_t *)(d + k * 2 + 0x24) = ax; *(uint16_t *)(d + 0x22) = (uint16_t)(k + 1); }
                else     { uint16_t k = *(uint16_t *)(d + 0x18);
                           *(uint16_t *)(d + k * 2 + 0x1a) = ax; *(uint16_t *)(d + 0x18) = (uint16_t)(k + 1); }
                break;
            case 0x2a - 0x24: *(uint16_t *)(d + (sec ? 0x16 : 0x14)) = ax; break;
            case 0x2b - 0x24: *(uint8_t  *)(d + (sec ? 0x07 : 0x06)) = al; break;
            case 0x2c - 0x24: *(uint8_t  *)(d + 0x6a) = al; break;
            case 0x2d - 0x24:
                if (operand == 2)      d[0x69] |= 2;
                else if (operand == 3) d[0x69] |= 4;
                break;
            case 0x2e - 0x24: break;                        /* no-op */
            case 0x2f - 0x24: break;                        /* no-op */
            case 0x30 - 0x24: *(uint8_t  *)(d + 0x68) = al; break;
            case 0x31 - 0x24: *(uint16_t *)(d + (sec ? 0x62 : 0x60)) = ax; break;
            case 0x32 - 0x24: *(uint16_t *)(d + (sec ? 0x66 : 0x64)) = ax; break;
            default: break;
            }
        }
        return 1;                                           /* AsMonster block processed */
    }
}

/* ---- Layer D: build_entity_def_by_id (0x1e2bd) — pure, oracle-able ----
 * Resolves a DBASE100 id -> record (via g_dbase100_base 0x81e1c + g_dbase100_inventory_table
 * 0x81e20, both A4 stored-pointer globals) then delegates to build_entity_def_record.
 * ABI: EAX = dest def struct, EDX = id.  ret EAX = build_entity_def_record's result (0/1), or 0
 * if id==0 / id>max / no record offset.
 */
uint32_t build_entity_def_by_id(uint32_t dest, uint32_t id)
{
    if (id == 0) return 0;                                   /* test edx,edx; je */
    uint32_t base = (uint32_t)G32(VA_g_dbase100_base);                  /* g_dbase100_base (stored ptr) */
    if (id > *(uint32_t *)(uintptr_t)(base + 0x10)) return 0; /* id > max -> 0 */
    int32_t *tab = (int32_t *)(uintptr_t)(uint32_t)G32(VA_g_dbase100_inventory_table);  /* g_dbase100_inventory_table */
    uint32_t off = (uint32_t)tab[id];                        /* per-id record offset */
    if (off == 0) return 0;                                  /* no record -> 0 */
    uint32_t rec = base + off;
    if (rec == 0) return 0;                                  /* add eax,edx; je (degenerate) */
    return build_entity_def_record(dest, rec);
}

/* bridge into an un-lifted/cross-subsystem callee with the full Watcom register arg set
 * (EAX/EDX/EBX/ECX); returns the callee's EAX (threaded for callees whose result is read).
 * In the HOST this runs the ORIGINAL bytes against the native OS/subsystem emulation (DOS file I/O,
 * audio, inventory, the dbase100 interpreter, ...) with all lifts suspended for the duration. */
static uint32_t db_bridge(uint32_t canon_va, uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx)
{
    regs_t io; memset(&io, 0, sizeof io);
    io.va  = canon_va + OBJ_DELTA;
    io.eax = eax; io.edx = edx; io.ebx = ebx; io.ecx = ecx;
#ifndef ROTH_STANDALONE
    call_orig(&io);
    return io.eax;
#else
    if (canon_va == 0x2fb7fu) {    /* build_game_path (clean lifted body) — open_dialogue_script's path build */
        build_game_path((uint8_t *)(uintptr_t)eax,
                               (const uint8_t *)(uintptr_t)edx, (const uint8_t *)(uintptr_t)ebx);
        return eax;
    }
    if (canon_va == 0x15191u) {    /* game_heap_free (clean lifted body) — init/reset_game_databases frees */
        game_heap_free((uint8_t *)(uintptr_t)eax);
        return eax;
    }
    if (canon_va == 0x20c16u)      /* play_record_gdv_cutscene (clean lifted body): boot intro FMV — under */
        return play_record_gdv_cutscene(eax);   /* --skip-gdv the .GDV open fails and it bails to the title */
    switch (canon_va) {            /* finalize_dbase100_chain's speech/audio-sequence quartet (clean lifted
                                    * bodies over C2/haudio) — fires as the world+menu comes up */
    case 0x15492u: return load_dbase300_chunk(eax);
    case 0x15689u: return finalize_audio_sequence_ref();
    case 0x1555fu: return emit_music_sequence_event(eax);
    case 0x1558du: process_audio_sequence_chunk(); return 0;
    case 0x20905u: exit_cutscene_overlay_mode(); return 0;   /* overlay teardown — armed by the
                                                                     * aborted --skip-gdv intros */
    /* deeper-gameplay tier (first --game entry): the dbase100 command
     * interpreter's remaining cross-subsystem effects — all clean lifted bodies. First stop was
     * give_item 0x1cedc (the new-game starting-items chain). */
    case 0x1cedcu: return give_item(eax, edx);                /* EAX=entry idx, EDX=ctx -> EAX */
    case 0x1d077u: return remove_item(eax);                   /* EAX=id -> -1/0 */
    case 0x1ccf7u: return query_player_inventory(eax, edx);   /* EAX=id, EDX=flags -> count */
    case 0x27270u: return play_sound_effect(eax, edx);        /* EAX=id, EDX=param */
    case 0x320a7u: advance_clamp_8a0f0(eax, edx); return 0;   /* void */
    case 0x1eabcu: return dbase100_open_dialogue_window((uint8_t *)(uintptr_t)eax);
    case 0x1ebb4u: return dbase100_open_dialogue_window_alt((uint8_t *)(uintptr_t)eax);
    case 0x1c9a0u: return (uint32_t)rng_next_index_for_count((int32_t)eax);
    case 0x305a1u: return (uint32_t)reset_command_chain_no_source((uint16_t)eax);
    case 0x30549u: return (uint32_t)run_object_commands_by_id((uint16_t)eax);
    case 0x20f81u: return show_fullscreen_image(eax);         /* EAX=operand24 -> EAX */
    default: break;
    }
    roth_unreachable(canon_va);   /* dbase100 cross-subsystem bridge — in-game only, off the bare title */
    return 0;
#endif
}
static uint32_t db_bridge3(uint32_t canon_va, uint32_t eax, uint32_t edx, uint32_t ebx) { return db_bridge(canon_va, eax, edx, ebx, 0); }
static uint32_t db_bridge_eax(uint32_t canon_va, uint32_t eax) { return db_bridge(canon_va, eax, 0, 0, 0); }

/* ---- Layer C leaf: close_dialogue_script (0x1e8ae) ----
 * Closes the open dbase400.dat handle (g_dialogue_script_handle 0x824c5) if any. Bridges
 * dos_close_handle 0x41b41 (host DOS). ABI: void/void. */
void close_dialogue_script(void)
{
    if (G32(VA_g_dialogue_script_handle) != 0) {
        dos_close_handle((uint32_t)G32(VA_g_dialogue_script_handle));    /* dos_close_handle(handle) (C2) */
        G32(VA_g_dialogue_script_handle) = 0;
    }
}

/* ---- Layer C: free_dbase100_data (0x1e0a9) ----
 * Releases the loaded DBASE100 data block + the progress-flag bitmap (game_heap_free, bridged
 * memory_pool), then tail-jmps to close_dialogue_script 0x1e8ae (a REAL tail call: both the
 * bitmap-absent `je` and the bitmap-present `jmp` target 0x1e8ae). ABI: void/void. */
void free_dbase100_data(void)
{
    if (G32(VA_g_dbase100_base) != 0) {                                /* g_dbase100_base */
        db_bridge_eax(0x15191, (uint32_t)G32(VA_g_dbase100_base));     /* game_heap_free(base) */
        G32(VA_g_dbase100_base) = 0;
    }
    if (G32(VA_g_dbase100_record_bitmap) != 0) {                                /* g_dbase100_record_bitmap */
        db_bridge_eax(0x15191, (uint32_t)G32(VA_g_dbase100_record_bitmap));     /* game_heap_free(bitmap) */
        G32(VA_g_dbase100_record_bitmap) = 0;
    }
    close_dialogue_script();                         /* tail-jmp 0x1e8ae */
}

/* ---- Layer C leaf: finish_dialogue_record_eval (0x1db5e) ----
 * Clears g_dbase100_queue_active 0x81e18; when the action queue + choice are both idle AND the
 * cutscene-overlay flag 0x83c70 is set, first tears down the overlay (exit_cutscene_overlay_mode
 * 0x20905, bridged dialogue_ui/gdv). The caller discards EAX (saves/restores its own), so void.
 * ORACLE: stage 0x83c70=0 -> the heavy overlay bridge never fires; the only obj3 write is
 * 0x81e18=0, verified by the full write-set diff. */
void finish_dialogue_record_eval(void)
{
    if (G32(VA_g_dialogue_action_queue_count) == 0 && G32(VA_g_dbase100_choice_cursor) == 0 && G32(VA_g_cutscene_overlay_active) != 0)
        db_bridge_eax(0x20905, 0);                          /* exit_cutscene_overlay_mode */
    G32(VA_g_entity_def_cache_count + 0x4) = 0;
}

/* ---- Layer B: finalize_dbase100_chain (0x1d25e) ----
 * Commits any pending latent-opcode effect staged in g_dbase100_pending_topic 0x81eb2: loads its
 * dbase300 chunk (bridge load_dbase300_chunk 0x15492), and if that returns 0, runs the audio
 * sequence finalize/emit/process bridges (gated on g_audio_seq_state 0x83c4c==2), then clears the
 * pending topic. The chain argument (EAX) is saved in EDX and returned unchanged.
 * ABI: EAX=arg -> EAX=arg (passthrough). Bridges file_config + audio; oracle ret-stubs them.
 */
uint32_t finalize_dbase100_chain(uint32_t arg)
{
    if (G32(VA_g_dbase100_pending_topic) != 0) {
        uint32_t r = db_bridge_eax(0x15492, (uint32_t)G32(VA_g_dbase100_pending_topic));   /* load_dbase300_chunk */
        if (r == 0) {
            if (G32(VA_g_dialogue_busy_flag + 0x162) == 2) {
                G32(VA_g_dialogue_busy_flag + 0x162) = 0;
                db_bridge_eax(0x15689, 0);                  /* finalize_audio_sequence_ref */
                db_bridge_eax(0x1555f, (uint8_t)G8(VA_g_font_descriptor + 0x212)); /* emit_music_sequence_event(byte) */
            }
            db_bridge_eax(0x1558d, 0);                      /* process_audio_sequence_chunk */
        }
        G32(VA_g_dbase100_pending_topic) = 0;
    }
    return arg;
}

/* ---- Layer C leaf: open_dialogue_script (0x1e874) ----
 * Opens dbase400.dat (the dialogue-line file) once: if no handle is open (g_dialogue_script_handle
 * 0x824c5 == 0), build the game path (build_game_path 0x2fb7f, bridge file_config; name "DBASE400"
 * @0x76540, ext @0x75e69) into a local buffer, dos_open_file it (0x41ae5, host DOS), store the
 * handle. ABI: void/void. Only obj3 write is the handle slot. */
void open_dialogue_script(void)
{
    if (G32(VA_g_dialogue_script_handle) != 0) return;
    uint8_t path[0xc8];
    db_bridge3(0x2fb7f, (uint32_t)(uintptr_t)path, (uint32_t)GADDR(VA_g_dir_gdv + 0x50), (uint32_t)GADDR(VA_g_heap_free_list + 0x859));
    G32(VA_g_dialogue_script_handle) = (int32_t)dos_open_file((uint32_t)(uintptr_t)path, 0);   /* dos_open_file(path, 0) (C2) */
}

/* ===========================================================================================
 * Live-swap cluster (ROTH_LIFT=dbase100). These are the dialogue-script reader + the
 * deferred-action queue + the eval/branch entries. They are NON-IDEMPOTENT (give/remove item, play
 * sound, run cutscenes, file I/O) so they are LIVE-SWAP-only (no oracle). Cross-subsystem callees
 * are BRIDGED via call_orig (the host runs the originals against native state); the 1722B interpreter
 * execute_dbase100_chain 0x1d430 is now a DIRECT C call (re-point); calls to OTHER lifted
 * dbase100 fns go to C directly.
 * ABIs derived from the DISASM. Queue: g_dialogue_action_queue 0x81e42 (stride 0xc: {chain,count,
 * flag}), count g_dialogue_action_queue_count 0x81e3e (max 7+1).
 *
 * RE-POINTED bridges (converted to direct C):
 *   - execute_dbase100_chain 0x1d430 (the four caller sites below): now call
 *     execute_dbase100_chain() directly. The four caller oracle tests (test_eval_dialogue_by_id
 *     / _queue_drain / _branch / _cond) previously byte-STUBBED the interpreter and staged FAKE chain
 *     pointers (stage_action_queue's 0x1000+i*0x10 dummies); re-staged them with REAL minimal
 *     terminate-record chains (a single op-0x0a record, operand!=0 -> H_1d67a -> finalize(1)) and
 *     deleted the interpreter byte-stub, so the interpreter now runs REAL on BOTH sides. A
 *     terminate-only chain invokes NO cross-subsystem callee and (with g_dbase100_pending_topic
 *     0x81eb2==0) an inert finalize, so its obj3 write-set is empty + symmetric; the interpreter
 *     itself is independently lifted+verified in test_interpreter.
 *   - lookup_cached_timed_message 0x1e7c3 / store_cached_timed_message 0x1e827 in
 *     read_next_dialogue_line: RE-POINTED to direct C. Both are pure obj3
 *     manipulations of the timed-message LRU cache at [0x820c1] (nodes 0x820c5.., all in obj3), and
 *     both are independently verified in test_dialogue_ui (du_lk / du_st). The oracle's former IT_BR
 *     counting stubs (C_LOOKCACHE/C_STORECACHE) were removed: the interpreter test now stages a real
 *     cache free-list (it_reset zeroes the head 0x820c1 so the first lookup rebuilds it) and lets the
 *     real lookup/store run on BOTH sides — their observable cache-node writes are the assertion
 *     (stronger than the call-count proxy), and are symmetric because the whole cache is in obj3 and
 *     restored by snap_restore between the orig/lift runs.
 * =========================================================================================== */

/* read_next_dialogue_line (0x1e8cc): EAX=dest, EDX=maxlen, EBX=voice_offset, ECX=flag -> EAX=bytes
 * read (or the cached message ptr, or 0 if no script handle). Seeks the dbase400.dat handle to the
 * voice offset, reads the 8-byte header {voice_clip_id; text_len:u16, color:u8}, sets g_text_color
 * 0x82040 (default 0x20), clamps text_len to maxlen, reads the text, then primes the voice clip
 * (flag!=0) or caches the timed message (flag==0 & len<=0x14). */
uint32_t read_next_dialogue_line(uint32_t dest, uint32_t maxlen, uint32_t voice_off, uint32_t flag)
{
    if (flag == 0) {
        /* re-pointed: lookup_cached_timed_message 0x1e7c3 [L, dialogue_ui] direct-C (EAX=dest,
         * EDX=key -> EAX=len; EBX duplicate voice_off was dead). Tests stage a real 0x820c1 cache. */
        uint32_t cached = lookup_cached_timed_message(dest, (int32_t)voice_off);
        if (cached != 0) return cached;
    }
    if (G32(VA_g_dialogue_script_handle) == 0) return 0;                                /* no script handle */
    uint32_t handle = (uint32_t)G32(VA_g_dialogue_script_handle);
    dos_lseek(handle, voice_off, 0);                         /* dos_lseek(handle, voice_off, whence 0=SET) (C2) */
    uint8_t hdr[8];
    dos_read_items((uint32_t)(uintptr_t)hdr, 8, 1, handle);  /* dos_read_items(hdr, 8, 1, handle) (C2) */
    uint32_t hw1 = *(uint32_t *)(hdr + 4);
    uint32_t text_len = hw1 & 0xffff;
    uint8_t  color    = (uint8_t)((hw1 >> 16) & 0xff);
    G8(VA_g_timed_message_color) = color;
    if (color == 0) G8(VA_g_timed_message_color) = 0x20;
    G32(VA_g_voice_bytes_remaining + 0x14) = 0;
    if (text_len > maxlen) text_len = maxlen;                       /* jbe keep / else clamp */
    handle = (uint32_t)G32(VA_g_dialogue_script_handle);                               /* mov ecx,[0x824c5] (re-read) */
    uint32_t bytes_read = dos_read_items(dest, 1, text_len, handle);  /* dos_read_items(dest,1,len,handle) (C2) */
    if (flag != 0) {
        uint32_t vc = *(uint32_t *)(hdr + 0);
        if (vc != 0) prime_voice_clip(vc);                 /* [repointed 0x1e54d] EAX=clip; in-game tier (flag!=0 path never oracle-exercised) */
    } else if (bytes_read <= 0x14) {
        /* re-pointed: store_cached_timed_message 0x1e827 [L, dialogue_ui] direct-C
         * (EAX=text, EDX=key, EBX=len, CL=color). Tests stage a real 0x820c1 cache free-list. */
        store_cached_timed_message(dest, (int32_t)voice_off, (int32_t)bytes_read,
                                          (uint8_t)G8(VA_g_timed_message_color));
    }
    return bytes_read;
}

/* resolve_dbase100_text (0x1f818): EAX=dest, EDX=maxlen, EBX=index, ECX=flag. Resolve a text record
 * from the DBASE100 base text table (count base+0x28, table base+0x2c, entry = voice offset) then
 * read it via read_next_dialogue_line. Early-returns reproduce the original's stray EAX values. */
uint32_t resolve_dbase100_text(uint32_t dest, uint32_t maxlen, uint32_t index, uint32_t flag)
{
    if (G32(VA_g_dbase100_base) != 0) {
        uint32_t base = (uint32_t)G32(VA_g_dbase100_base);
        if ((int32_t)index < *(int32_t *)(uintptr_t)(base + 0x28)) {
            uint32_t tbl = base + *(uint32_t *)(uintptr_t)(base + 0x2c);
            uint32_t voice_off = *(uint32_t *)(uintptr_t)(tbl + index * 4);
            if (voice_off != 0)
                return read_next_dialogue_line(dest, maxlen, voice_off, flag);
        }
    }
    /* not found (0x1f843): copy the default "Missing Text\0" (13 bytes @0x71374) into dest, return 13 */
    memcpy((void *)(uintptr_t)dest, (const void *)GADDR(VA_g_choice_selected_index + 0x4), 0xd);
    return 0xd;
}

/* eval_or_queue_dialogue_record_commands (0x1daea): EAX=record, EDX=flag -> EAX. If the queue is
 * active+nonempty, drain it first; then read the record's count (header u16 / 4 - 1) and either
 * queue the chain (count<=7) or run it now via the interpreter. */
uint32_t eval_or_queue_dialogue_record_commands(uint32_t record, uint32_t flag)
{
    uint32_t ecx = record;
    uint32_t ebx = flag;
    if (G32(VA_g_entity_def_cache_count + 0x4) != 0 && G32(VA_g_dialogue_action_queue_count) != 0)
        run_dialogue_action_queue();
    int32_t edx = (int32_t)(*(uint32_t *)(uintptr_t)ecx & 0xffff);
    edx >>= 2;
    ecx += 4;
    edx -= 1;
    if (G32(VA_g_dialogue_action_queue_count) == 0)
        return execute_dbase100_chain(ecx, (uint32_t)edx, ebx);  /* execute_dbase100_chain [re-pointed] */
    if ((uint32_t)G32(VA_g_dialogue_action_queue_count) > 7) return 0;                      /* queue full */
    uint8_t *q = (uint8_t *)GADDR(VA_g_dialogue_action_queue);
    uint32_t slot = (uint32_t)G32(VA_g_dialogue_action_queue_count) * 0xc;
    *(uint32_t *)(q + slot + 0) = ecx;                            /* chain ptr */
    *(uint32_t *)(q + slot + 4) = (uint32_t)edx;                  /* count */
    *(uint32_t *)(q + slot + 8) = ebx;                            /* flag */
    G32(VA_g_dialogue_action_queue_count) = (int32_t)G32(VA_g_dialogue_action_queue_count) + 1;
    return 1;
}

/* run_dialogue_action_queue (0x1d2aa): void. Drain the deferred dialogue-action queue: for each
 * entry run its chain (interpreter, BRIDGE); on a non -1 result pop+shift the queue down. Handles
 * the cutscene-voice-force-end teardown (0x83aea) before and after each run. */
void run_dialogue_action_queue(void)
{
    G32(VA_g_dbase100_choice_record_indices + 0x40) = 1;
    if (G32(VA_g_dialogue_busy_flag) != 0) {
        dialogue_voice_force_end(0, 0);                     /* [repointed 0x1f671] */
        G32(VA_g_entity_def_cache_count + 0x4) = 0;
        if (G32(VA_g_voice_stream_state) != 1) { G32(VA_g_voice_stream_state) = 0; G32(VA_g_dialogue_busy_flag) = 0; G32(VA_g_move_freeze_gate) = 0; G32(VA_g_active_dialogue_context) = 0; }
    }
    if (G32(VA_g_dialogue_action_queue_count) != 0) {
        do {
            uint8_t *q = (uint8_t *)GADDR(VA_g_dialogue_action_queue);
            uint32_t ebx = *(uint32_t *)(q + 8);                  /* flag */
            uint32_t edx = *(uint32_t *)(q + 4);                  /* count */
            uint32_t eax = *(uint32_t *)(q + 0);                  /* chain */
            uint32_t r = execute_dbase100_chain(eax, edx, ebx);  /* execute_dbase100_chain [re-pointed] */
            if (r != 0xffffffffu) {
                G32(VA_g_dialogue_action_queue_count) = (int32_t)G32(VA_g_dialogue_action_queue_count) - 1;
                if (G32(VA_g_dialogue_action_queue_count) != 0)
                    memmove((void *)GADDR(VA_g_dialogue_action_queue), (void *)GADDR(VA_g_dialogue_action_queue + 0xc), (size_t)((uint32_t)G32(VA_g_dialogue_action_queue_count) * 0xc));
            }
            if (G32(VA_g_dialogue_busy_flag) != 0) {
                dialogue_voice_force_end(0, 0);             /* [repointed 0x1f671] */
                G32(VA_g_entity_def_cache_count + 0x4) = 0;
                if (G32(VA_g_voice_stream_state) != 1) { G32(VA_g_voice_stream_state) = 0; G32(VA_g_move_freeze_gate) = 0; G32(VA_g_active_dialogue_context) = 0; G32(VA_g_dialogue_busy_flag) = 0; }
            }
        } while (G32(VA_g_dialogue_action_queue_count) != 0);
    }
    G32(VA_g_dbase100_choice_record_indices + 0x40) = 0;
}

/* advance_dialogue_action_queue (0x1db98): void. Run+pop ONE queue entry (vs run_dialogue_action_
 * queue which drains all). Skips if a cutscene overlay (0x83aea) is active or the queue is empty. */
void advance_dialogue_action_queue(void)
{
    if (G32(VA_g_dialogue_busy_flag) != 0) return;
    if (G32(VA_g_dialogue_action_queue_count) == 0) return;
    uint8_t *q = (uint8_t *)GADDR(VA_g_dialogue_action_queue);
    uint32_t ebx = *(uint32_t *)(q + 8);
    uint32_t edx = *(uint32_t *)(q + 4);
    uint32_t eax = *(uint32_t *)(q + 0);
    uint32_t r = execute_dbase100_chain(eax, edx, ebx);           /* execute_dbase100_chain [re-pointed] */
    if (r == 0xffffffffu) return;
    G32(VA_g_dialogue_action_queue_count) = (int32_t)G32(VA_g_dialogue_action_queue_count) - 1;
    if (G32(VA_g_dialogue_action_queue_count) == 0) { finish_dialogue_record_eval(); return; }
    memmove((void *)GADDR(VA_g_dialogue_action_queue), (void *)GADDR(VA_g_dialogue_action_queue + 0xc), (size_t)((uint32_t)G32(VA_g_dialogue_action_queue_count) * 0xc));
}

/* execute_dialogue_branch (0x1dc02): EAX=branch index. Walk the active choice record (0x81ea2,
 * count 0x81ea6, voice 0x81eaa); count opcode-8 sub-records into the index and, on the index-th
 * opcode-9 entry, run that branch's chain (interpreter, BRIDGE) then finish. */
uint32_t execute_dialogue_branch(uint32_t index)
{
    if (G32(VA_g_dbase100_choice_cursor) == 0) return index;
    uint32_t ecx = (uint32_t)G32(VA_g_dbase100_choice_cursor);
    int32_t  edx = (int32_t)G32(VA_g_dbase100_choice_remaining);
    uint32_t esi = (uint32_t)G32(VA_g_dbase100_choice_mode);
    uint32_t eax = *(uint32_t *)(uintptr_t)(GADDR(VA_g_dbase100_choice_record_indices) + index * 4);
    G32(VA_g_dbase100_choice_cursor) = 0;
    if (edx <= 0) return eax;
    for (;;) {
        uint32_t op = (*(uint32_t *)(uintptr_t)ecx >> 24) & 0xff;
        edx--;
        ecx += 4;
        if (op == 8) {
            eax++;
        } else if (op == 9) {
            if (eax == 0) {
                execute_dbase100_chain(ecx, (uint32_t)edx, esi);  /* execute_dbase100_chain [re-pointed] */
                finish_dialogue_record_eval();
                return 0;                                         /* eax = finish's (discarded) result */
            }
            eax--;
        }
        if (edx == 0) break;
    }
    return eax;
}

/* close_dialogue_and_run_branch (0x1fbe4): void. Clear the overlay/cutscene flags, then tail-call
 * execute_dialogue_branch with the saved branch index 0x8313d. */
void close_dialogue_and_run_branch(void)
{
    G32(VA_g_active_dialogue_context) = 0;
    G32(VA_g_move_freeze_gate) = 0;
    G32(VA_g_dialogue_busy_flag) = 0;
    uint32_t idx = (uint32_t)G32(VA_g_dialogue_reveal_ramp + 0x14);
    G32(VA_g_choice_selected_index) = 0xffffffff;
    execute_dialogue_branch(idx);
}

/* eval_dialogue_record_by_id (0x1dc73): EAX=id -> EAX. Resolve a dialogue record by id via the
 * dialogue record table (count base+0x18, table 0x81e24) then eval_or_queue it. Early-returns
 * reproduce the original's stray EAX values. */
uint32_t eval_dialogue_record_by_id(uint32_t id)
{
    if (G32(VA_g_dbase100_base) == 0) return 0;                              /* early exits -> 0 (0x1dca7 xor eax,eax) */
    uint32_t base = (uint32_t)G32(VA_g_dbase100_base);
    if ((int32_t)id > *(int32_t *)(uintptr_t)(base + 0x18)) return 0;
    uint32_t tbl = (uint32_t)G32(VA_g_dbase100_dialogue_table);
    uint32_t *p = (uint32_t *)(uintptr_t)(tbl + id * 4);
    if (*p == 0) return 0;
    uint32_t rec = *p + base;
    return eval_or_queue_dialogue_record_commands(rec, 0);
}

/* eval_dialogue_record_condition_with_cleanup (0x1db89): EAX=record, EDX=flag -> EAX. Composition:
 * r = eval_or_queue(record, flag); finish_dialogue_record_eval(); return r.
 *
 * EBX and ECX are PRESERVED across the whole call (input == output), proven by disasm:
 *   0x1db89 body:  call 0x1daea ; mov edx,eax ; call 0x1db5e ; mov eax,edx ; ret  -- touches only EAX/EDX.
 *   0x1daea (eval): `push ebx ; push ecx` at entry, `pop ecx ; pop ebx` at EVERY ret -> callee-saves both
 *                   regardless of the inner 0x1d2aa/0x1d430 calls.
 *   0x1db5e (finish): no writes to EBX/ECX; its optional `call 0x20905` also does `push ebx,ecx,edx`
 *                   at entry / `pop edx,ecx,ebx` at 0x20a86 -> preserves them too.
 * So Ghidra's "extraout_EBX/extraout_ECX" that the dialogue_ui callers read back are simply their OWN
 * preserved input regs. The ebx_io/ecx_io out-params model that pass-through (body leaves them
 * untouched -> output == the caller-supplied input); NULL when a caller doesn't consume them. */
uint32_t eval_dialogue_record_condition_with_cleanup(uint32_t record, uint32_t flag,
                                                            uint32_t *ebx_io, uint32_t *ecx_io)
{
    (void)ebx_io; (void)ecx_io;   /* preserved: nothing to compute — caller's values stay valid */
    uint32_t r = eval_or_queue_dialogue_record_commands(record, flag);
    finish_dialogue_record_eval();
    return r;
}

/* ABI_REGS_EAX trampolines for the EAX+EDX -> EAX entries (no dedicated 2-arg adapter exists). */
int32_t eval_or_queue_reg(const regs_t *in)
{ return (int32_t)eval_or_queue_dialogue_record_commands(in->eax, in->edx); }
int32_t eval_dialogue_record_condition_reg(const regs_t *in)
{ return (int32_t)eval_dialogue_record_condition_with_cleanup(in->eax, in->edx, NULL, NULL); }  /* EBX/ECX preserved by the registry frame */

/* ===========================================================================================
 * Layer B: execute_dbase100_chain (0x1d430) — THE threaded-bytecode interpreter (1722 B).
 *
 * Walks a chain of 4-byte records { bit31=If-NOT, opcode=(w>>24)&0x7f, operand=w&0xffffff },
 * dispatching each opcode to a handler that fans out into nearly every gameplay subsystem
 * (items, audio, GDV cutscenes, dialogue windows, the choice menu, the deferred-action queue).
 *
 * Faithful goto-transcription of the disasm; labels named L_/H_ by their canon VA. The dispatch
 * was DECODED from the image (value table 0x1d3b4[25] + jump table 0x1d3cc[25]; handler =
 * jt[24 - pos(opcode-1)], not-found -> jt[0] = skip 0x1d47b) and is reproduced as a switch.
 *
 * ABI (from the prologue): EAX = chain pointer (a mutable record cursor, advances +4/record),
 * EDX = record count, EBX = caller-modifier flags (bit1=scan-only/skip-effects, bit0 = op-0x1c
 * sub-selector). Returns EAX = the effectful-action accumulator [ebp-4], EXCEPT early-outs return
 * 1 (scan-mode / a choice exists) or -1 (re-queued because a cutscene overlay is active). Only the
 * normal-exit + op-0x0a + terminate paths run finalize_dbase100_chain (L_1d482); the 1 / -1 / single-
 * choice (edi) returns bypass it.
 *
 * GDV-LIVE-SWAP-BLOCKED (op 0x07 plays GDV; the host's per-frame GDV int3 nests in a lift SIGTRAP),
 * so this is ORACLE-verified (test_dbase100.c): the interpreter's OWN logic (dispatch, per-opcode
 * obj3 writes, the choice-menu builder, queue interactions, op-0x0b recursion) is diffed vs call_orig
 * with all cross-subsystem/OS callees value/ret-stubbed. Already-lifted dbase100 callees run as C.
 * =========================================================================================== */
uint32_t execute_dbase100_chain(uint32_t chain, uint32_t count_in, uint32_t flags_in)
{
    uint32_t cur    = chain;               /* [ebp-0xc7c] record cursor (runtime ptr) */
    int32_t  count  = (int32_t)count_in;   /* esi  remaining record count */
    uint32_t flags  = flags_in;            /* [ebp-8]  caller-modifier flags */
    int32_t  acc    = 0;                   /* [ebp-4]  effectful-action accumulator (return value) */
    uint32_t notbit = 0;                   /* [ebp-0xc] current record's If-NOT bit */
    int32_t  sub_idx = 0;                  /* [ebp-0x10] sub-record index (choice builder) */
    int32_t  if_failed = 0, prev_if_failed = 0;  /* [ebp-0x14] per-iter if-condition-failed flag */
    uint32_t textw = 0, choff = 0;         /* [ebp-0x18] text write ptr, [ebp-0x20] -> 0x81eb6 table */
    int32_t  textspace = 0, stop_build = 0, edi = 0;  /* [ebp-0x1c] space, [ebp-0x28] stop, choice count */
    uint32_t eax = 0, ebx = 0, eax16 = 0, word = 0, sw = 0;
    int32_t  opcode = 0, subop = 0;

    uint8_t  filt_out[0x400];              /* [ebp-0x78 .. -0x478] op-0x0b filter output {ptr,count}[] */
    uint8_t  text_buf[0xc00];              /* [ebp-0xc78] choice-menu text builder */
    uint8_t  alt_text[0x3fc];              /* [ebp-0x478] op-0x05 alt dialogue-window text */

L_1d44b:                                   /* main loop */
    eax  = cur;                            /* mov eax,[ebp-0xc7c] (cursor = a record pointer) */
    cur += 4;                              /* add [ebp-0xc7c],4 */
    word = *(uint32_t *)(uintptr_t)eax;    /* mov eax,[eax] -> record word */
    count--;                               /* dec esi */
    notbit = word & 0x80000000u;           /* [ebp-0xc] */
    ebx    = word & 0x00ffffffu;           /* operand (24-bit) */
    opcode = (int32_t)((word >> 24) & 0x7fu);
    if (ebx != 0) goto DISPATCH;           /* records with operand==0 are skipped */
L_1d47b:                                    /* skip_or_continue */
    if (count > 0) goto L_1d44b;
    eax = (uint32_t)acc;
L_1d482:
    return finalize_dbase100_chain(eax);   /* finalize + return eax */

DISPATCH:                                   /* 0x1dac5 */
    if ((uint32_t)(opcode - 1) > 0x36u) goto L_1d47b;   /* opcodes 1..0x37 only */
    eax16 = ebx & 0xffffu;                  /* mov eax,ebx; and eax,0xffff */
    switch (opcode) {
        case 0x01: goto H_1d799;  case 0x02: goto H_1d7c9;  case 0x03: goto H_1d7f0;
        case 0x04: goto H_1d6c3;  case 0x05: goto H_1d84f;  case 0x07: goto H_1da1e;
        case 0x08: goto H_1d4c3;  case 0x0a: goto H_1d67a;  case 0x0b: goto H_1d684;
        case 0x0d: goto H_1d48c;  case 0x0e: goto H_1d9c5;  case 0x10: goto L_1d672;
        case 0x11: goto H_1d6e5;  case 0x19: goto H_1d83a;  case 0x1a: goto H_1d73b;
        case 0x1b: goto H_1d711;  case 0x1c: goto H_1d8d7;  case 0x1d: goto H_1d8ff;
        case 0x23: goto H_1d816;  case 0x26: goto H_1d992;  case 0x2d: goto H_1d977;
        case 0x36: goto H_1d828;  case 0x37: goto H_1d72d;  default:   goto L_1d47b;
    }

/* op 0x0d (0x1d48c): if-flag gate with a next-record look-ahead */
H_1d48c:
    if (*(uint8_t *)(uintptr_t)(cur + 3) == 8) goto H_1d4c3;   /* next opcode byte == 8 -> choice builder */
    if (notbit == 0) { if (test_dbase100_record_flag(ebx) != 0) goto L_1d47b; }
    else             { if (test_dbase100_record_flag(ebx) == 0) goto L_1d47b; }
    cur += 4;                               /* skip the gated next record */
L_1d4c0:
    count--;
    goto L_1d47b;

/* op 0x08 (0x1d4c3): build the choice menu */
H_1d4c3:
    if (flags & 2) return 1;                /* scan-mode -> "a choice exists" (no finalize) */
    edi = 0;
    stop_build = 0;
    if (G32(VA_g_dialogue_busy_flag) != 0) goto REQUEUE_A;  /* cutscene overlay active -> re-queue + return -1 */
    textspace = 0x800;                      /* [ebp-0x1c] */
    choff = (uint32_t)GADDR(VA_g_dbase100_choice_record_indices);       /* [ebp-0x20] -> branch-index table */
    cur -= 4;                               /* back up to re-read the 0x08 record */
    G32(VA_g_entity_def_cache_count + 0x4) = 0;
    if_failed = 0;
    count++;
    textw = (uint32_t)(uintptr_t)text_buf;  /* [ebp-0x18] */
    if (count == 0) goto L_1d600;
L_1d547:                                    /* sub-record loop */
    sw     = *(uint32_t *)(uintptr_t)cur;
    subop  = (int32_t)((sw >> 24) & 0x7fu);
    prev_if_failed = if_failed;             /* edx = previous if_failed */
    if_failed = 0;
    if (subop != 8) {
        if (subop == 0xd) {                 /* an If-condition gating the NEXT choice */
            uint32_t arg = sw & 0xffffu;
            int hold = (sw & 0x80000000u) ? (test_dbase100_record_flag(arg) == 0)
                                          : (test_dbase100_record_flag(arg) != 0);
            if (!hold) if_failed = 1;
            cur += 4;
            goto L_1d597;
        }
        stop_build = 1;                     /* any other opcode -> stop building */
        goto L_1d5ee;
    }
    /* subop == 8: a choice line */
    if (prev_if_failed != 0) goto L_1d5db;  /* prev If failed -> skip this choice (advance only) */
    {
        uint32_t r = read_next_dialogue_line(textw, (uint32_t)textspace, sw & 0x00ffffffu, 0);
        if (r == 0) goto L_1d5db;
        uint32_t newtw = textw + r;
        uint32_t choff_old = choff;
        textspace -= (int32_t)r;
        edi++;
        *(uint8_t *)(uintptr_t)(newtw - 1) = 0x0a;   /* '\n' over the last byte read */
        textw = newtw;
        *(uint8_t *)(uintptr_t)newtw = 0;            /* re-NUL terminate */
        choff += 4;
        *(uint32_t *)(uintptr_t)choff_old = (uint32_t)sub_idx;   /* record this choice's sub-record index */
    }
L_1d5db:
    cur += 4;
    sub_idx++;
L_1d597:
    count--;
L_1d5ee:
    if (count <= 0) return 1;               /* return 1 (no finalize) */
    if (stop_build == 0) goto L_1d547;
L_1d600:                                    /* menu built (edi = #choices) */
    if (edi == 0) goto L_1d672;             /* no choices -> count + continue */
    G32(VA_g_dbase100_choice_remaining) = count;                   /* publish the choice record */
    G32(VA_g_dbase100_choice_cursor) = (int32_t)cur;
    G32(VA_g_dbase100_choice_count) = edi;
    G32(VA_g_dbase100_choice_mode) = (int32_t)flags;
    if (edi == 1) goto L_1d646;             /* single choice -> auto-run branch 0 */
    if (db_bridge_eax(0x1eabc, (uint32_t)(uintptr_t)text_buf) == 0) goto L_1d672;  /* open the window */
    G32(VA_g_dialogue_busy_flag) = 1;
    return 1;
L_1d646:                                    /* exactly one choice -> auto-select branch 0 */
    G32(VA_g_dialogue_busy_flag) = 0;
    G32(VA_g_move_freeze_gate) = 0;
    G32(VA_g_active_dialogue_context) = 0;
    execute_dialogue_branch(0);
    return (uint32_t)edi;

L_1d672:                                    /* op 0x10 / count-and-continue */
    acc++;
    goto L_1d47b;

/* op 0x0a (0x1d67a): unconditional terminate-success */
H_1d67a:
    eax = 1;
    goto L_1d482;

/* op 0x0b (0x1d684): run a RANDOM active sub-chain (filter + rng + recurse) */
H_1d684:
    if (count == 0) goto L_1d672;
    {
        int32_t active_cnt = 0;
        count = filter_dbase100_active_records(
                    (uint32_t)(uintptr_t)&cur, (uint32_t)count,
                    (uint32_t)(uintptr_t)filt_out, (uint32_t)(uintptr_t)&active_cnt);
        if (active_cnt == 0) goto L_1d672;
        uint32_t idx = db_bridge_eax(0x1c9a0, (uint32_t)active_cnt) & 0xffffu;   /* rng_next_index_for_count */
        uint32_t sub_ptr   = *(uint32_t *)(uintptr_t)(filt_out + idx * 8 + 0);
        uint32_t sub_count = *(uint32_t *)(uintptr_t)(filt_out + idx * 8 + 4);
        execute_dbase100_chain(sub_ptr, sub_count, flags);   /* recursion -> lifted C */
    }
    goto L_1d672;

/* op 0x04 (0x1d6c3): set/clear a DBASE100 progress flag */
H_1d6c3:
    if (flags & 2) return 1;
    if (notbit == 0) dbase100_bitmap_test_set(ebx);
    else             dbase100_bitmap_test_clear(ebx);
    goto L_1d672;

/* op 0x11 (0x1d6e5): give/remove item */
H_1d6e5:
    if (flags & 2) return 1;
    if (notbit == 0) db_bridge3(0x1cedc, (uint32_t)(int32_t)(int16_t)ebx, 0, 0);  /* give_item((int16)operand) */
    else             db_bridge_eax(0x1d077, (uint32_t)(int32_t)(int16_t)ebx);     /* remove_item((int16)operand) */
    goto L_1d672;

/* op 0x1b (0x1d711): stage pending topic (0x81eb2 = *0x7f46c) */
H_1d711:
    if (flags & 2) goto L_1d47b;
    acc++;
    G32(VA_g_dbase100_pending_topic) = G32(VA_g_current_dbase300_chunk_id);
    goto L_1d47b;

/* op 0x37 (0x1d72d): add signed16 operand to 0x81e30 */
H_1d72d:
    G32(VA_g_value_reduction_factor) = (int32_t)G32(VA_g_value_reduction_factor) + (int32_t)(int16_t)ebx;
    goto L_1d47b;

/* op 0x1a (0x1d73b): finalize pending audio/dbase300 (REQUEUE-B variant) */
H_1d73b:
    if (flags & 2) goto L_1d47b;
    if (G32(VA_g_dialogue_busy_flag) != 0) {                /* REQUEUE-B (0x1d74e): writes 0x81e46 + 0x81e42 ONLY (no 0x81e4a) */
        G32(VA_g_dialogue_action_queue + 0x4) = count + 1;
        G32(VA_g_dialogue_action_queue) = (int32_t)(cur - 4);
        goto L_1d4fe;
    }
    if (G32(VA_g_dialogue_busy_flag + 0x162) == 2) {
        db_bridge_eax(0x15689, 0);          /* finalize_audio_sequence_ref */
        G32(VA_g_dialogue_busy_flag + 0x162) = 0;
    }
    if (db_bridge_eax(0x15492, ebx) != 0) goto L_1d672;   /* load_dbase300_chunk(operand) */
    db_bridge_eax(0x1558d, 0);              /* process_audio_sequence_chunk */
    goto L_1d672;

/* op 0x01 (0x1d799): if-flag gate -> terminate-the-chain on the selecting polarity */
H_1d799:
    if (notbit == 0) { if (test_dbase100_record_flag(ebx) != 0) goto L_1d47b; }  /* If */
    else             { if (test_dbase100_record_flag(ebx) == 0) goto L_1d47b; }  /* If-NOT */
L_1d7ae:
    eax = 0;
    goto L_1d482;

/* op 0x02 (0x1d7c9): inventory-count compare gate */
H_1d7c9:
    {
        uint32_t cnt = db_bridge3(0x1ccf7, (uint32_t)(int32_t)(int16_t)ebx, 0, 0);  /* query_player_inventory */
        uint32_t thr = ebx >> 16;
        if (notbit == 0) { if (cnt >  thr) goto L_1d47b; }   /* If:     count >  thr -> skip */
        else             { if (cnt <= thr) goto L_1d47b; }   /* If-NOT: count <= thr -> skip */
        goto L_1d7ae;
    }

/* op 0x03 (0x1d7f0): inventory-count EQUALITY gate (terminates with eax = cnt^thr = 0) */
H_1d7f0:
    {
        uint32_t cnt = db_bridge3(0x1ccf7, (uint32_t)(int32_t)(int16_t)ebx, 0, 0);
        uint32_t thr = ebx >> 16;
        if (notbit == 0) {                  /* If: equal -> terminate */
            if (cnt != thr) goto L_1d47b;
            eax = cnt ^ thr;
            goto L_1d482;
        }
        if (cnt == thr) goto L_1d47b;       /* If-NOT: equal -> skip */
        goto L_1d7ae;
    }

/* op 0x23 (0x1d816): reset_command_chain_no_source(operand16) */
H_1d816:
    if (eax16 == 0) goto L_1d47b;
    db_bridge_eax(0x305a1, eax16);
    goto L_1d47b;

/* op 0x36 (0x1d828): run_object_commands_by_id(operand16) */
H_1d828:
    if (eax16 == 0) goto L_1d47b;
    db_bridge_eax(0x30549, eax16);
    goto L_1d47b;

/* op 0x19 (0x1d83a): play_sound_effect(operand24-1, 0xfcfe) */
H_1d83a:
    acc++;
    db_bridge3(0x27270, ebx - 1, 0xfcfe, 0);
    goto L_1d47b;

/* op 0x05 (0x1d84f): open dialogue window (alt) */
H_1d84f:
    if (flags & 2) return 1;
    if (G32(VA_g_dbase100_choice_record_indices + 0x40) != 0) goto L_1d672;
    if (G32(VA_g_active_dialogue_context) != 0 || G32(VA_g_dialogue_busy_flag) != 0) {
        if (G32(VA_g_entity_def_cache_count + 0x4) != 0) run_dialogue_action_queue();
    }
    if (G32(VA_g_dialogue_busy_flag) != 0) goto REQUEUE_A;
    finish_dialogue_record_eval();
    if (read_next_dialogue_line((uint32_t)(uintptr_t)alt_text, 0x3fc, ebx, 1) == 0) goto L_1d672;
    if (db_bridge_eax(0x1ebb4, (uint32_t)(uintptr_t)alt_text) == 0) goto L_1d672;   /* dbase100_open_dialogue_window_alt */
    G32(VA_g_dialogue_busy_flag) = 1;
    goto L_1d672;

/* op 0x1c (0x1d8d7): flags-only gate, NO bitmap call (continue iff !!(flags&1)==!!IfNot, else terminate) */
H_1d8d7:
    if (flags & 2) goto L_1d47b;
    if (notbit == 0) { if ((flags & 1) == 0) goto L_1d47b; }   /* If */
    else             { if ((flags & 1) != 0) goto L_1d47b; }   /* If-NOT */
    goto L_1d7ae;

/* op 0x1d (0x1d8ff): jump the cursor to a referenced dialogue record + keep interpreting it */
H_1d8ff:
    {
        uint32_t base = (uint32_t)G32(VA_g_dbase100_base);
        if (ebx > *(uint32_t *)(uintptr_t)(base + 0x18)) goto L_1d47b;
        uint32_t *ent = (uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_dbase100_dialogue_table) + ebx * 4);
        if (*ent == 0) goto L_1d47b;
        uint32_t target = *ent + base;
        cur   = target + 4;
        count = (int32_t)((*(uint32_t *)(uintptr_t)target & 0xffffu) >> 2);
        sub_idx = 0;
        goto L_1d4c0;
    }

/* op 0x2d (0x1d977): set 0x83b20 (sub-code 4) or 0x80b2c (sub-code 7, gated on 0x7fec4) */
H_1d977:
    {
        uint32_t sc = ebx & 0xffu;
        if (sc < 4) goto L_1d47b;
        if (sc == 4) { G32(VA_g_dialogue_busy_flag + 0x36) = 1; goto L_1d47b; }
        if (sc == 7) {
            if (G32(VA_g_inventory_panel_open) == 0) goto L_1d47b;
            G32(VA_g_ui_panel_anchor_y + 0x4) = 1;
        }
        goto L_1d47b;
    }

/* op 0x26 (0x1d992): advance/reset the 0x8a0f0 clamp */
H_1d992:
    {
        int32_t v = (int32_t)(int16_t)ebx;
        if (v <= 0) { G32(VA_g_player_health) = 0; G32(VA_g_pending_game_action + 0xc) = 1; goto L_1d47b; }
        db_bridge3(0x320a7, (uint32_t)v, (uint32_t)G32(VA_g_help_overlay_enabled + 0xc), 0);   /* advance_clamp_8a0f0(v, *0x7fe44) */
        goto L_1d47b;
    }

/* op 0x0e (0x1d9c5): show fullscreen image */
H_1d9c5:
    if (flags & 2) return 1;
    {
        uint32_t e = (G32(VA_g_dialogue_busy_flag) != 0) ? 1u : 0u;
        if (G32(VA_g_inventory_panel_open) != 0 && G32(VA_g_inspect_popup_active) == 1) { G32(VA_g_inventory_panel_open) = 2; G32(VA_g_inventory_panel_open + 0x8) = 1; e++; }
        if (e != 0) goto REQUEUE_A;
    }
    acc++;
    db_bridge_eax(0x20f81, ebx & 0x00ffffffu);   /* show_fullscreen_image(operand24) */
    goto L_1d47b;

/* op 0x07 (0x1da1e): play a GDV cutscene */
H_1da1e:
    if (flags & 2) return 1;
    if (G32(VA_g_active_dialogue_context) != 0 || G32(VA_g_dialogue_busy_flag) != 0) {
        if (G32(VA_g_entity_def_cache_count + 0x4) != 0) run_dialogue_action_queue();
    }
    {
        uint32_t e = (G32(VA_g_dialogue_busy_flag) != 0) ? 1u : 0u;
        if (G32(VA_g_inventory_panel_open) != 0 && G32(VA_g_inspect_popup_active) == 1) { G32(VA_g_inventory_panel_open) = 2; G32(VA_g_inventory_panel_open + 0x8) = 1; e++; }
        if (e != 0) goto REQUEUE_A;
    }
    {
        uint32_t base = (uint32_t)G32(VA_g_dbase100_base);
        uint32_t idx  = ebx - 1;
        if (idx >= *(uint32_t *)(uintptr_t)(base + 0x20)) goto L_1d672;
        if (*(uint32_t *)(uintptr_t)(base + 0x30) != 2) goto L_1d672;
        uint32_t rec = base + *(uint32_t *)(uintptr_t)(base + 0x24) + idx * 0x14;
        if (*(uint8_t *)(uintptr_t)rec == 0) goto L_1d672;
        db_bridge_eax(0x20c16, rec);        /* play_record_gdv_cutscene(rec) */
    }
    G32(VA_g_dialogue_busy_flag + 0x36) = 1;
    goto L_1d672;

/* REQUEUE-A (0x1d4e1): re-queue the current record (all 3 queue fields) + return -1.
 * Reached from the choice builder + the busy overlay paths (op 0x05/0x07/0x0e). */
REQUEUE_A:
    G32(VA_g_dialogue_action_queue) = (int32_t)(cur - 4);
    G32(VA_g_dialogue_action_queue + 0x4) = count + 1;
    G32(VA_g_dialogue_action_queue + 0x8) = (int32_t)flags;
L_1d4fe:
    if (G32(VA_g_dialogue_action_queue_count) == 0) G32(VA_g_dialogue_action_queue_count) = (int32_t)G32(VA_g_dialogue_action_queue_count) + 1;
    return 0xffffffffu;
}
