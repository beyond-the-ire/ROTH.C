/* lift_savegame.c — verified-C lifts for the `savegame` subsystem.
 *
 * savegame = save/load to disk, per-chunk state serialization (player / dynamic
 * entities / object links / record lists), slot names + thumbnails, and the
 * screenshot/LBM snapshot path. It is heavily DOS-file-IO bound at its entries, but
 * has a pure-logic serialization CORE (the state-chunk read/write helpers + the
 * object-link pointer<->index fixups + the transient-flag stripper) that is
 * oracle-testable once the IO callees are bridged. Those chunk helpers are SHARED with
 * map_load's raw-state stream — lift them here first; map_load bridges them until its
 * own lift covers them (coordinate before promoting to a shared file).
 *
 * See docs/reference/lift/savegame.md +
 * docs/reference/ROTH_savegame_format.md.
 *
 * ABI / behaviour transcribed STRICTLY FROM THE DISASM (the corpus decompile is
 * Borland-cspec-on-Watcom and unreliable for register args / multi-reg returns). The
 * Layer-A leaves here take a single pointer arg (the raw-state buffer / object table,
 * a flat RUNTIME host address) and write through it; deref RAW (gotcha A4), never via
 * the G8/G16/G32 canon macros. They use no FS/GS/ES segment state and call nothing, so
 * they are oracle-verified by an obj3 write-set diff against call_orig over a staged
 * buffer.
 *
 * Functions lifted here (Layer A — state-chunk serialization leaves):
 *   strip_transient_flags_for_save 0x31e14 — clear runtime/transient flag bits across
 *       the three record arrays of a raw-state buffer before it is serialized.
 *   write_player_state_chunk 0x3e0f0 — serialize the player globals into a 0x30-byte record.
 *   read_player_state_chunk  0x3e1a0 — restore the player globals from a 0x30-byte record.
 *   write_state_object_links 0x35735 — serialize one object-link record (typed) into the stream.
 *   write_state_record_list  0x35648 — drive the object-link directory + emit the chunk trailer.
 *   build_screenshot_filename 0x114e2 — assemble the screenshot path into g_snapshot_filename_buf.
 *   load_state_dynamic_entities 0x4ef61 — restore the two entity pools from a count-prefixed chunk.
 *   load_state_record_list   0x3580c — drive the level-state record stream on restore.
 *   load_state_object_links  0x35839 — deserialize one typed object-link record from the stream.
 *   resolve_state_link_target 0x359ad — resolve a link target via the bridged RAW 0x30780 dispatch.
 *   save_snapshot_file       0x3cc02 — deferred ILBM/IFF screenshot writer (PackBits BODY; inline int 0x21).
 *   take_snapshot            0x11135 — interactive snapshot front-end (int 0x10 mode-sets; drives the menu; arms).
 *   snapshot_menu_and_save   0x111a0 — interactive DOS-console snapshot menu (single/anim; int 0x21 AH=9 prints).
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "common.h"

/* Diagnostic logging for the Layer-C live-swap bring-up (host only; gated on ROTH_SAVEDBG). The oracle
 * never calls the Layer-C orchestrators, so this is dormant there. -1 = unread; cached after the first. */
static int sv_dbg(void)
{
    static int v = -1;
    if (v < 0) v = (getenv("ROTH_SAVEDBG") != NULL);
    return v;
}
#define SVLOG(...) do { if (sv_dbg()) { fprintf(stderr, "[savedbg] " __VA_ARGS__); fflush(stderr); } } while (0)

/* g_inventory_slots base (canon 0x80c30). The two equipped-item globals hold live RUNTIME
 * pointers into this array; the chunk serializes them as OFFSETS relative to this base so they
 * survive relocation (A4), and the read side re-adds the base. The original code's `mov ecx,0x80c30`
 * is a relocated absolute, so at runtime it is the rebased base = GADDR(0x80c30). */
#define INV_BASE  ((int32_t)GADDR(VA_g_inventory_slots))

/* flat (host-address) byte/word/dword access; volatile so faithful reads/writes are emitted. */
#define RB(a)     (*(volatile uint8_t  *)(uintptr_t)(a))
#define RW(a)     (*(volatile uint16_t *)(uintptr_t)(a))
#define RD(a)     (*(volatile uint32_t *)(uintptr_t)(a))
#define WB(a,v)   (*(volatile uint8_t  *)(uintptr_t)(a) = (uint8_t)(v))
#define WW(a,v)   (*(volatile uint16_t *)(uintptr_t)(a) = (uint16_t)(v))
#define WD(a,v)   (*(volatile uint32_t *)(uintptr_t)(a) = (uint32_t)(v))

/* Per-load level-state path tally (SAVEDBG bring-up only; reset by load_state_record_list at ENTER,
 * logged at DONE) so a single load confirms which code path each record took — in particular whether
 * the bridged resolve_state_link_target fired (vs only the 0xfffe header-reloc / unknown-type paths). */
static uint32_t sv_ls_header, sv_ls_resolve, sv_ls_unknown;

/* ============================ strip_transient_flags_for_save (0x31e14) ============================
 * void __watcall strip_transient_flags_for_save(void *buf);   // buf in EAX
 *
 * `buf` is a raw-state buffer (flat runtime address). Its header holds three u16 list-offset
 * fields; each list is preceded by a u16 element count at (list_start - 2). The function walks
 * each list and clears runtime-only ("transient") flag bits so the on-disk state is clean:
 *   - OBJECTS list  @ u16 buf[+4], stride 0x1a: [r+0x16] &= 0xc9; [r+0x17] &= 0x3f; *(u16*)(r+4) = 0
 *   - WALLS   list  @ u16 buf[+8], stride 0xa, but +4 EXTRA when [r+1]&0x80 (record is then 0xe):
 *                                              if [r+1]&0x80 { [r+9] &= 0xf8; r += 4 }  r += 0xa
 *   - SECTORS list  @ u16 buf[+6], stride 0xc: [r+0xa] &= 0x9f
 * The original masks the dword at buf+{4,8,6} with 0xffff (== reading the u16 there); the loop
 * counter is a u16 zero-extended into ECX driven by `dec ecx; jg`, i.e. it runs `count` times for
 * any count in 1..0xffff (always positive, so the signed jg == "while counter != 0"). EAX/the buf
 * pointer is never modified; EBX/ECX are callee-saved (push/pop). No callees, no segments. */
void strip_transient_flags_for_save(uint32_t buf)
{
    uint32_t r;
    int32_t  n;   /* signed: models `dec ecx; jg` over a zero-extended u16 count */

    /* OBJECTS: list at u16 buf[+4], stride 0x1a */
    r = buf + (RW(buf + 4) & 0xffffu);
    n = (int32_t)(uint32_t)RW(r - 2);
    if (n != 0) {
        do {
            WB(r + 0x16, RB(r + 0x16) & 0xc9);
            WB(r + 0x17, RB(r + 0x17) & 0x3f);
            WW(r + 4, 0);
            r += 0x1a;
        } while (--n > 0);
    }

    /* WALLS: list at u16 buf[+8], stride 0xa with a conditional +4 (variable-length record) */
    r = buf + (RW(buf + 8) & 0xffffu);
    n = (int32_t)(uint32_t)RW(r - 2);
    if (n != 0) {
        do {
            if (RB(r + 1) & 0x80) {
                WB(r + 9, RB(r + 9) & 0xf8);
                r += 4;
            }
            r += 0xa;
        } while (--n > 0);
    }

    /* SECTORS: list at u16 buf[+6], stride 0xc */
    r = buf + (RW(buf + 6) & 0xffffu);
    n = (int32_t)(uint32_t)RW(r - 2);
    if (n != 0) {
        do {
            WB(r + 0xa, RB(r + 0xa) & 0x9f);
            r += 0xc;
        } while (--n > 0);
    }
}

/* ============================ write_player_state_chunk (0x3e0f0) ============================
 * uint32_t __watcall write_player_state_chunk(void *rec);   // rec in EAX; returns 0x30 (chunk size)
 *
 * Serialize the player into the 0x30-byte record at `rec`: position g_player_x/z/y (0x90a8c/90/94),
 * angle (0x90a8a), sector (0x90c12), height (0x8c110), view pitch + applied (0x90a74 / 0x8c108),
 * value-reduction factor (0x81e30), health (0x8a0f0), the misc dwords 0x8c114/0x8c108-pair, and the
 * two equipped items (0x81038 secondary / 0x81044 primary, stored as INV_BASE-relative offsets so
 * they survive reloc; 0 stays 0). The tail mirrors the stored height/word back into 0x8c110/0x8c112
 * and the dword into 0x8c114 (re-read from the record, matching the disasm exactly). No callees,
 * no segments. EDX/ECX callee-saved (push/pop). */
uint32_t write_player_state_chunk(uint32_t rec)
{
    WD(rec + 0x2c, (uint32_t)G32(VA_g_player_height + 0x4));
    WW(rec + 0x28, G16(VA_g_player_height));
    WD(rec + 0x1c, (uint32_t)G32(VA_g_view_pitch));
    WD(rec + 0x20, (uint32_t)G32(VA_g_view_pitch_applied));
    WD(rec + 0x24, (uint32_t)G32(VA_g_value_reduction_factor));
    WD(rec + 0x00, (uint32_t)G32(VA_g_player_angle + 0x2));
    WD(rec + 0x04, (uint32_t)G32(VA_g_player_x + 0x2));
    WD(rec + 0x08, (uint32_t)G32(VA_g_player_z + 0x2));
    WW(rec + 0x0c, G16(VA_g_player_angle));
    WW(rec + 0x0e, G16(VA_g_player_sector));

    int32_t v = G32(VA_g_selected_item_secondary);                       /* g_selected_item_secondary (ptr or 0) */
    if (v != 0) v -= INV_BASE;                       /* -> offset relative to inventory base */
    WD(rec + 0x10, (uint32_t)v);
    v = G32(VA_g_selected_item_primary);                               /* g_selected_item_primary */
    if (v != 0) v -= INV_BASE;
    WD(rec + 0x14, (uint32_t)v);

    WD(rec + 0x18, (uint32_t)G32(VA_g_player_health));

    uint16_t w = RW(rec + 0x28);                     /* re-read the just-stored height word */
    G16(VA_g_player_height) = w;
    G16(VA_g_player_height + 0x2) = w;
    G32(VA_g_player_height + 0x4) = (int32_t)RD(rec + 0x2c);
    return 0x30;
}

/* ============================ read_player_state_chunk (0x3e1a0) ============================
 * void __watcall read_player_state_chunk(const void *rec);   // rec in EAX
 *
 * Inverse of write_player_state_chunk: restore the player globals from the 0x30-byte record. NOTE
 * the two asymmetries vs the write side (faithful — transcribed independently): the sector field
 * [rec+0xe] is restored into 0x89f36 (NOT 0x90c12), and the read side does not touch 0x8c110/0x8c112/
 * 0x8c114 or the height word. Equipped-item offsets are re-based: 0 stays 0, else += INV_BASE. No
 * callees, no segments. */
void read_player_state_chunk(uint32_t rec)
{
    G32(VA_g_view_pitch) = (int32_t)RD(rec + 0x1c);
    G32(VA_g_view_pitch_applied) = (int32_t)RD(rec + 0x20);
    G32(VA_g_value_reduction_factor) = (int32_t)RD(rec + 0x24);
    G32(VA_g_player_angle + 0x2) = (int32_t)RD(rec + 0x00);
    G32(VA_g_player_x + 0x2) = (int32_t)RD(rec + 0x04);
    G32(VA_g_player_z + 0x2) = (int32_t)RD(rec + 0x08);
    G16(VA_g_player_angle) = RW(rec + 0x0c);
    G16(VA_g_das_special_fat_index + 0x2) = RW(rec + 0x0e);

    int32_t v = (int32_t)RD(rec + 0x10);             /* equipped secondary offset (or 0) */
    if (v != 0) v += INV_BASE;
    G32(VA_g_selected_item_secondary) = v;
    v = (int32_t)RD(rec + 0x14);                     /* equipped primary offset (or 0) */
    if (v != 0) v += INV_BASE;
    G32(VA_g_selected_item_primary) = v;

    G32(VA_g_player_health) = (int32_t)RD(rec + 0x18);
}

/* ============================ write_state_object_links (0x35735) ============================
 * uint32_t __watcall write_state_object_links(void *obj, void *rec, uint8_t type, uint8_t *dst);
 *   obj  in EAX  — the linked object pointer (-> serialized as a table INDEX via find_object_index_by_ptr)
 *   rec  in EDX  — the link record the typed fields are copied from (find_object_index preserves EDX)
 *   type in CL   — the record type, selecting which fields/size are emitted
 *   dst  in EDI  — the destination stream cursor
 *   returns the ADVANCED EDI (the caller, write_state_record_list, consumes it).
 *
 * Always writes the object index word at [dst+0]; then by type: a +2 flag byte (rec[5]), a +4 word
 * (rec[6]), and for some types a +6 dword from rec[0xc] that is a POINTER serialized as an OFFSET so
 * it survives reloc — types {3,7} subtract g@0x85c44; type 0x12 (only when the dword's hi word is
 * nonzero) subtracts the geometry base g@0x90aa8 and ORs 0xffff0000 back into the hi word. The type
 * groups + per-type emit sizes are transcribed exactly from the cl-dispatch chain (default = index
 * word only, EDI unchanged). find_object_index_by_ptr is already lifted (reach via engine.h). */
uint32_t write_state_object_links(uint32_t obj, uint32_t rec, uint8_t type, uint32_t dst)
{
    WW(dst + 0, (uint16_t)find_object_index_by_ptr(obj));   /* [dst]=ax (object index) */

    switch (type) {
    case 2: case 0xe: case 0xf: case 0x1d: case 0x23:   /* 0x357a8: byte + word, +6 */
        WB(dst + 2, RB(rec + 5));
        WW(dst + 4, RW(rec + 6));
        return dst + 6;

    case 3: case 7: {                                   /* 0x357ba: byte + word + reloc-dword(-0x85c44), +0xa */
        WB(dst + 2, RB(rec + 5));
        WW(dst + 4, RW(rec + 6));
        int32_t v = (int32_t)RD(rec + 0xc);
        if (v != 0) v -= G32(VA_g_sfx_nodes);
        WD(dst + 6, (uint32_t)v);
        return dst + 0xa;
    }

    case 9: case 0xa: case 0xb: case 0xc: case 0xd:     /* 0x3579e: byte only, +4 */
        WB(dst + 2, RB(rec + 5));
        return dst + 4;

    case 0x11:                                          /* 0x35784: byte + word(@6) + word(@0xc), +8 */
        WB(dst + 2, RB(rec + 5));
        WW(dst + 4, RW(rec + 6));
        WW(dst + 6, RW(rec + 0xc));
        return dst + 8;

    case 0x12: {                                        /* 0x357dc: word + byte + geometry-reloc dword, +0xa */
        WW(dst + 4, RW(rec + 6));
        WB(dst + 2, RB(rec + 5));
        int32_t v = (int32_t)RD(rec + 0xc);
        if (v != 0 && (v & (int32_t)0xffff0000) != 0) {
            v -= G32(VA_g_map_geometry_buffer);
            v |= (int32_t)0xffff0000;
        }
        WD(dst + 6, (uint32_t)v);
        return dst + 0xa;
    }

    default:                                            /* 0x35783: index word only, EDI unchanged */
        return dst;
    }
}

/* ============================ write_state_record_list (0x35648) ============================
 * uint32_t __watcall write_state_record_list(uint8_t *dst);   // dst in EAX; returns the chunk byte size
 *
 * The save-side chunk-4 driver (mirror of load_state_record_list 0x3580c). When the object table
 * (g@0x85c30) and the object-link directory head (g@0x8a118) are both non-null, it walks the link
 * directory and serializes each non-skipped object's link via write_state_object_links, advancing the
 * stream cursor. The directory walk: piVar3 = head; node = *piVar3; obj = node[+8]; if !(obj[+2]&1)
 * emit(obj, node, type=obj[+3]); piVar3 = node[+0] (next); until piVar3 == 0. (The original's post-loop
 * second table walk produces no memory writes — esi/ebx are discarded — so it is omitted: byte-identical.)
 * Then a fixed 0xc-byte trailer: 0xfffe marker, the two link words g@0x8a0e8/0x8a0ec, two pointers
 * serialized as reloc-surviving offsets (g@0x8a0e4 - g@0x85c30; g@0x8a13c - the geometry base g@0x90aa8;
 * 0 stays 0), and a 0xffff marker. Returns (cursor - dst + 3) & ~3 (round up to a multiple of 4; the
 * original's `and al,0xfc` only touches the low two bits, == & ~3). No obj3 writes (stream only). */
uint32_t write_state_record_list(uint32_t dst)
{
    uint32_t edi = dst;

    if (G32(VA_g_object_table_header) != 0 && G32(VA_g_active_effect_pool) != 0) {
        uint32_t cell = (uint32_t)G32(VA_g_active_effect_pool);
        while (cell != 0) {
            uint32_t node = RD(cell);
            uint32_t obj  = RD(node + 8);
            if ((RB(obj + 2) & 1) == 0)
                edi = write_state_object_links(obj, node, RB(obj + 3), edi);
            cell = RD(node + 0);
        }
    }

    WW(edi + 0, 0xfffe);
    WW(edi + 2, (uint16_t)G32(VA_g_state_link_word_a));
    WW(edi + 4, (uint16_t)G32(VA_g_state_link_word_b));
    int32_t v = G32(VA_g_state_link_obj_ptr);
    if (v != 0) v -= G32(VA_g_object_table_header);
    WW(edi + 6, (uint16_t)v);
    v = G32(VA_g_state_link_buf_ptr);
    if (v != 0) v -= G32(VA_g_map_geometry_buffer);
    WW(edi + 8, (uint16_t)v);
    edi += 0xa;
    WW(edi + 0, 0xffff);
    edi += 2;

    return (edi - dst + 3) & ~3u;
}

/* ============================ build_screenshot_filename (0x114e2) ============================
 * void __watcall build_screenshot_filename(void);
 *
 * Assemble the screenshot filename into g_snapshot_filename_buf (0x8b370). 15-byte entry stub that
 * pushes (counter=word g@0x70744, base string="ANIM" @0x70738) then JMPs into the shared body it
 * co-owns with build_snapshot_anim_filename (0x11500, already lifted) — same path prefix, '\' append,
 * decimal-digit count, and ".lbm" extension template; differs ONLY in the counter + base-string. (The
 * `jmp` means it never falls through to the anim entry; the corpus flow_succ is the linear-next only.)
 * Composes the lifted num_to_decimal_digits (no bridge). Mirrors build_snapshot_anim_filename
 * with the screenshot params. */
void build_screenshot_filename(void)
{
    uint16_t num = (uint16_t)G16(VA_g_snapshot_name_seq);                  /* g_screenshot_counter */
    uint8_t *d = (uint8_t *)(uintptr_t)(0x8b370u + OBJ_DELTA);
    const uint8_t *p;
    for (p = (const uint8_t *)(uintptr_t)(0x706e3u + OBJ_DELTA); *p; ) *d++ = *p++;  /* "C:\" prefix */
    if (d[-1] != 0x5c) *d++ = 0x5c;                         /* ensure trailing '\' */
    for (p = (const uint8_t *)(uintptr_t)(0x70738u + OBJ_DELTA); *p; ) *d++ = *p++;  /* base "ANIM" */
    d = num_to_decimal_digits(num, d);               /* append decimal digits */
    p = (const uint8_t *)(uintptr_t)(0x7248bu + OBJ_DELTA); /* "C:\Snap?.lbm" template */
    while (*p != 0x2e) p++;                                 /* skip to '.' */
    do { *d++ = *p; } while (*p++ != 0);                    /* copy ".lbm" incl. NUL */
}

/* entity_def_cache_lookup (0x1e2f6): a stateful MRU def cache; def-id in EAX -> def ptr in EAX. It
 * lives behind the static call_asm in renderer.c (same bridge revalidate_entity_def uses), so we
 * bridge it locally via the public call_orig — identical mechanism: EAX in, EAX out, no stack arg
 * (the callee reads only EAX; the original's `push eax` is a dead arg cleaned by the trailing pops). */
static uint32_t sv_call_eax(uint32_t canon_va, uint32_t eax)
{
    regs_t io;
    memset(&io, 0, sizeof io);
    io.va  = canon_va + OBJ_DELTA;
    io.eax = eax;
#ifndef ROTH_STANDALONE
    call_orig(&io);
    return io.eax;
#else
    switch (canon_va) {   /* M3 routes */
    case 0x1e2f6u: return entity_def_cache_lookup(eax);
    case 0x40a2au: game_free_if_not_null((uint8_t *)(uintptr_t)eax); return 0;
    case 0x11548u: return console_read_key(eax);
    case 0x11500u: build_snapshot_anim_filename(); return 0;
    case 0x114e2u: build_screenshot_filename(); return 0;
    case 0x114d4u: {   /* dos_print_char (corpus 114d4): {char,'$'} -> int21 AH=9 over the soft-int hook */
        uint8_t buf[2]; buf[0] = (uint8_t)eax; buf[1] = '$';
        regs_t v; memset(&v, 0, sizeof v);
        v.eax = 0x900; v.edx = (uint32_t)(uintptr_t)buf;
        if (g_os_soft_int) g_os_soft_int(0x21, &v);
        return 0;
    }
    default: break;
    }
    roth_unreachable(canon_va);   /* savegame orchestrator callee — the save/load path is off bare title */
    return 0;
#endif
}

/* General Watcom-arg bridge: invoke the original at `canon_va` with the four Watcom arg registers
 * (EAX, EDX, EBX, ECX) and return EAX. Used by the Layer-C orchestrators, whose callees all leave the
 * subsystem (DOS file I/O, the pool/das-cache allocators, the file_config path builders). In the HOST
 * live-swap build call_orig runs the original bytes (so the DOS `int 0x21` calls hit the host's DOS
 * emulation, exactly as the un-swapped game does); the oracle build never invokes these orchestrators. */
static uint32_t sv_bridge(uint32_t canon_va, uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx)
{
    regs_t io;
    memset(&io, 0, sizeof io);
    io.va  = canon_va + OBJ_DELTA;
    io.eax = eax;  io.edx = edx;  io.ebx = ebx;  io.ecx = ecx;
#ifndef ROTH_STANDALONE
    call_orig(&io);
    return io.eax;
#else
    switch (canon_va) {   /* M3 routes */
    case 0x114a3u: {   /* dos_print_concat (corpus 114a3): copy EDX-str then EAX-str, '$'-terminate, int21 AH=9 */
        char buf[100]; char *p = buf;
        const char *s = (const char *)(uintptr_t)edx;
        if (s) while (*s) *p++ = *s++;
        const char *t = (const char *)(uintptr_t)eax;
        while (*t) *p++ = *t++;
        *p = '$';
        regs_t v; memset(&v, 0, sizeof v);
        v.eax = 0x900; v.edx = (uint32_t)(uintptr_t)buf;
        if (g_os_soft_int) g_os_soft_int(0x21, &v);
        return 0;
    }
    case 0x41bc1u: {   /* dos_get_file_size: lseek END -> size; lseek SET back (same as fc_bridge) */
        uint32_t size = dos_lseek(eax, 0, 2);
        dos_lseek(eax, 0, 0);
        return size;
    }
    case 0x2196au: {   /* dos_write_u16 (corpus 2196a): {u16 EDX, u16 EBX} -> dos_write_items(buf,1,4,EAX) */
        uint16_t pair[2] = { (uint16_t)edx, (uint16_t)ebx };
        dos_write_items((uint32_t)(uintptr_t)pair, 1, 4, eax);
        return 0;
    }
    case 0x41be5u: {   /* dos_make_directory (EAX=path): open probe -> exists (close), else int21 AH=39
                        * (mirrors lift_map_load.c's ml_call route) */
        uint32_t h = dos_open_file(eax, 0);
        if (h != 0) { dos_close_handle(h); return 0xffffffffu; }
        regs_t v; memset(&v, 0, sizeof v);
        v.eax = 0x3900; v.edx = eax;
        if (g_os_soft_int) g_os_soft_int(0x21, &v);
        return v.eax;
    }
    default: break;
    }
    roth_unreachable(canon_va);   /* savegame Layer-C orchestrator bridge — off the bare-title path */
    return 0;
#endif
}

/* sprintf bridge (0x27c53): CDECL — it reads dest/fmt/args from the STACK (`mov edi,[esp+0x1c]` after 6
 * pushes = arg0), ignoring eax/ebx. call_orig pushes nstack dwords (stack[0] = lowest addr = arg0) and
 * the trampoline cleans them. The callers here format a slot number into a filename buffer (one vararg). */
static void sv_sprintf(uint32_t dest, uint32_t fmt, uint32_t arg)
{
    regs_t io;
    memset(&io, 0, sizeof io);
    io.va  = 0x27c53 + OBJ_DELTA;
    io.nstack = 3;
    io.stack[0] = dest;   /* arg0 (dest)  */
    io.stack[1] = fmt;    /* arg1 (fmt)   */
    io.stack[2] = arg;    /* arg2 (value) */
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    roth_sprintf((char *)(uintptr_t)dest, (const char *)(uintptr_t)fmt, (int)arg);  /* SAVE%D.SAV */
#endif
}

/* ============================ load_state_dynamic_entities (0x4ef61) ============================
 * void __watcall load_state_dynamic_entities(const void *rec);   // rec in EAX
 *
 * The load-side mirror of write_state_dynamic_entities: restore the two entity pools from a
 * count-prefixed state chunk. Layout: u32 count1, count1 * 0x22-byte STATE-entity records, u32 count2,
 * count2 * 0x1c-byte DYNAMIC-entity records. Each record is copied verbatim into its pool (state
 * @0x91e04 stride 0x22 = 8 dwords + 1 word, the original's `rep movsd`x8 + `movsw`; dynamic @0x90fe4
 * stride 0x1c = 7 dwords, `rep movsd`x7), the live-count global is reset (0x91e00 / 0x90fe0) then bumped
 * once per surviving record, and the embedded pointers are RELOCATED from stored offsets back to
 * runtime addresses:
 *   - STATE record [+4] (the entity-def instance ptr): += geometry base g@0x90aa4, only when nonzero.
 *     When the record's [+0] (draw/sprite-node ptr) is ALSO nonzero it is relocated by (g@0x85cf4 - 1)
 *     and the def is re-resolved through the MRU cache: *[+0] = entity_def_cache_lookup(word[[+4]+4]);
 *     word[*[+0] + 0x60] = that def-id. entity_def_cache_lookup is BRIDGED (sv_call_eax) — under the
 *     write-set oracle both the original and this lift invoke the identical original from the identical
 *     restored cache + DBASE state, so the bridge is deterministic and its writes match.
 *   - DYNAMIC record [+0]: += geometry base g@0x90aa4, only when nonzero (no def re-resolve).
 * Both loops guard on a signed count > 0 (the original's `or edx,edx; jle`) and run `count` times
 * (`dec edx; jg`). The dst pools/counters are obj3 absolutes (rebased -> GADDR); the src chunk is the
 * caller's flat buffer, read-only. The original copies through es: (flat in the host). */
void load_state_dynamic_entities(uint32_t rec)
{
    uint32_t src   = rec;
    uint32_t base0 = (uint32_t)G32(VA_g_ademo_das_fat_buffer + 0x4) - 1;     /* reloc base for the STATE record's [+0] ptr */

    /* ---- STATE-entity pool @0x91e04, stride 0x22 (8 dwords + 1 word) ---- */
    int32_t n = (int32_t)RD(src);  src += 4;
    if (n > 0) {
        uint32_t dst  = (uint32_t)GADDR(VA_g_state_pool_a_records);
        uint32_t geom = (uint32_t)G32(VA_g_map_objects_buffer);      /* reloc base for the [+4] def-instance ptr */
        G32(VA_g_state_pool_a_count) = 0;
        do {
            uint32_t r = dst;                                                       /* record start (ebx) */
            for (int i = 0; i < 8; i++) { WD(dst, RD(src)); dst += 4; src += 4; }   /* rep movsd x8 */
            WW(dst, RW(src)); dst += 2; src += 2;                                    /* movsw          */
            if (RD(r + 4) != 0) {
                G32(VA_g_state_pool_a_count) = G32(VA_g_state_pool_a_count) + 1;
                WD(r + 4, RD(r + 4) + geom);
                if (RD(r + 0) != 0) {
                    WD(r + 0, RD(r + 0) + base0);
                    uint16_t defid = RW(RD(r + 4) + 4);
                    uint32_t defp  = sv_call_eax(0x1e2f6, defid);   /* entity_def_cache_lookup(defid) */
                    uint32_t tgt   = RD(r + 0);
                    WD(tgt + 0, defp);
                    WW(tgt + 0x60, defid);
                }
            }
        } while (--n > 0);
    }

    /* ---- DYNAMIC-entity pool @0x90fe4, stride 0x1c (7 dwords) ---- */
    n = (int32_t)RD(src);  src += 4;
    if (n > 0) {
        uint32_t dst = (uint32_t)GADDR(VA_g_dynamic_entity_table);
        G32(VA_g_dynamic_entity_count) = 0;
        do {
            uint32_t r = dst;
            for (int i = 0; i < 7; i++) { WD(dst, RD(src)); dst += 4; src += 4; }   /* rep movsd x7 */
            if (RD(r + 0) != 0) {
                G32(VA_g_dynamic_entity_count) = G32(VA_g_dynamic_entity_count) + 1;
                WD(r + 0, RD(r + 0) + (uint32_t)G32(VA_g_map_objects_buffer));
            }
        } while (--n > 0);
    }
}

/* ============================ resolve_state_link_target (0x359ad) ============================
 * uint32_t __watcall resolve_state_link_target(obj /ESI/, stream /EDI/);   // returns EDX
 *
 * Read-side counterpart to write_state_object_links' index<->pointer fixup. Given the live object
 * record `obj` (ESI) and the current stream record `stream` (EDI), it re-dispatches the object's
 * command handler through the RAW command table at 0x30780 (base = obj[3] & 0x7f — the SAME typed
 * dispatch the RAW system uses, BRIDGED here because raw_command_system is a closed subsystem and the
 * table holds original handler pointers), which resolves and stashes the link target's
 * pointer-to-pointer at g@0x89f50. It frames the dispatch by clearing the object's arm bits
 * (obj[2] &= 0xde) before and clearing/re-setting them after (obj[2] &= 0xde; obj[2] |= 0x20), then
 * returns EDX = (g@0x89f50 == 0) ? 0 : *(g@0x89f50). The push/pop esi,edi preserve both across the
 * bridged handler (it consumes ESI=obj). See lift_raw_commands.c for the identical 0x30780 bridge. */
static uint32_t sv_resolve_state_link_target(uint32_t obj, uint32_t stream)
{
    sv_ls_resolve++;                                            /* SAVEDBG path tally */
    WB(obj + 2, RB(obj + 2) & 0xde);                            /* and byte[esi+2], 0xde */
    uint32_t bs = RB(obj + 3) & 0x7fu;                          /* bl=[esi+3]; and ebx, 0x7f */
    uint32_t handler = RD(0x30780u + OBJ_DELTA + bs * 4);       /* table[base] (relocated handler ptr) */
    regs_t io; memset(&io, 0, sizeof io);
    io.va = handler; io.esi = obj; io.edi = stream;
#ifndef ROTH_STANDALONE
    call_orig(&io);                                            /* call [ebx*4+0x30780] — BRIDGED */
#else
    (void)rawcmd_dispatch_30780(io.va, io.esi);                /* the 0x30780 table -> lifted cmd bodies (this
                                                                * site's return comes from [0x89f50], not eax;
                                                                * handlers consume ESI=obj) */
#endif
    WB(obj + 2, RB(obj + 2) & 0xde);                            /* and byte[esi+2], 0xde */
    WB(obj + 2, RB(obj + 2) | 0x20);                           /* or  byte[esi+2], 0x20 */
    uint32_t edx = (uint32_t)G32(VA_g_resource_pool_small_flag + 0xf);                      /* mov edx, [0x89f50] */
    if (edx) edx = RD(edx);                                     /* or edx,edx; je; mov edx,[edx] */
    return edx;
}
int32_t resolve_state_link_target(const regs_t *in)    /* registry wrapper (ESI=obj, EDI=stream -> EDX) */
{
    return (int32_t)sv_resolve_state_link_target(in->esi, in->edi);
}

/* ============================ load_state_object_links (0x35839) ============================
 * uint32_t __watcall load_state_object_links(eax /first u16/, stream /EDI/);   // returns bytes consumed
 *
 * Read-side inverse of write_state_object_links. The driver passes the stream record's first u16 (a
 * 1-based object index; the sentinel 0xfffe marks a header RELOCATION record) in EAX and the stream
 * record pointer in EDI. For 0xfffe it reloads the four reloc/base words into the restore globals
 * (object-base g@0x8a0e4 = word[+6] + g@0x85c30; geometry-base g@0x8a13c = word[+8] + g@0x90aa8;
 * plus the two raw words g@0x8a0e8/g@0x8a0ec) and consumes 0xa bytes. Otherwise it resolves the live
 * object via the directory ([[0x8a0d8]][index-1]), reads its type byte (obj[3] & 0x7f) and
 * deserializes the typed link payload from the stream into the resolved target (via
 * resolve_state_link_target), returning the per-type record length. Unknown types consume 0. */
static uint32_t sv_load_state_object_links(uint32_t eax_in, uint32_t edi)
{
    if ((uint16_t)eax_in == 0xfffeu) {                          /* 0x358d9: header relocation record */
        uint32_t v;
        sv_ls_header++;
        G32(VA_g_state_link_word_a) = (uint32_t)RW(edi + 2);                  /* [0x8a0e8] = word[edi+2] */
        G32(VA_g_state_link_word_b) = (uint32_t)RW(edi + 4);                  /* [0x8a0ec] = word[edi+4] */
        v = RW(edi + 6); if (v) v += (uint32_t)G32(VA_g_object_table_header);   /* word[edi+6] (+ object base) */
        G32(VA_g_state_link_obj_ptr) = v;                                      /* [0x8a0e4] = ... */
        v = RW(edi + 8); if (v) v += (uint32_t)G32(VA_g_map_geometry_buffer);   /* word[edi+8] (+ geometry base) */
        G32(VA_g_state_link_buf_ptr) = v;                                      /* [0x8a13c] = ... */
        return 0xa;
    }

    uint32_t index = (eax_in - 1) & 0xffffu;                   /* dec eax; and eax, 0xffff */
    uint32_t obj   = RD(RD((uint32_t)G32(VA_g_object_ptr_array)) + index * 4);/* esi = [[0x8a0d8]][index] */
    uint32_t type  = RB(obj + 3) & 0x7fu;                      /* al = [esi+3]; and eax, 0x7f */
    uint32_t edx, v;

    switch (type) {
    case 9: case 0xa: case 0xb: case 0xc: case 0xd:            /* 0x358a7 */
        edx = sv_resolve_state_link_target(obj, edi);
        if (edx) WB(edx + 5, RB(edi + 2));                     /* [edx+5] = byte[edi+2] */
        return 4;

    case 2: case 0xe: case 0xf: case 0x1d: case 0x23:         /* 0x358bc */
        edx = sv_resolve_state_link_target(obj, edi);
        if (edx) { WB(edx + 5, RB(edi + 2)); WW(edx + 6, RW(edi + 4)); }
        return 6;

    case 3: case 7:                                            /* 0x3591b */
        edx = sv_resolve_state_link_target(obj, edi);
        if (edx) {
            WB(edx + 5, RB(edi + 2));
            WW(edx + 6, RW(edi + 4));
            v = RD(edi + 6); if (v) v += (uint32_t)G32(VA_g_sfx_nodes);   /* object-record reloc */
            WD(edx + 0xc, v);
        }
        return 0xa;

    case 0x12:                                                 /* 0x35948 */
        edx = sv_resolve_state_link_target(obj, edi);
        if (edx) {
            WB(edx + 5, RB(edi + 2));
            WW(edx + 6, RW(edi + 4));
            v = RD(edi + 6);
            if ((v & 0xffff0000u) == 0xffff0000u)              /* geometry-base sentinel in the high word */
                v = (v & 0xffffu) + (uint32_t)G32(VA_g_map_geometry_buffer);
            WD(edx + 0xc, v);
        }
        return 0xa;

    case 0x11:                                                 /* 0x35988 */
        edx = sv_resolve_state_link_target(obj, edi);
        if (edx) {
            WB(edx + 5, RB(edi + 2));
            WW(edx + 6, RW(edi + 4));
            WW(edx + 0xc, RW(edi + 6));
        }
        return 8;

    default:                                                   /* 0x358a4: sub eax,eax; ret */
        sv_ls_unknown++;                                        /* SAVEDBG path tally */
        return 0;
    }
}
int32_t load_state_object_links(const regs_t *in)      /* registry wrapper (EAX=u16, EDI=stream -> EAX) */
{
    return (int32_t)sv_load_state_object_links(in->eax, in->edi);
}

/* ============================ load_state_record_list (0x3580c) ============================
 * void __watcall load_state_record_list(list /EAX/);
 *
 * Top of the level-state restore chain (called by read_raw_state_stream 0x214b9 during a level warp).
 * Clears the restore scratch global g@0x89f5c, then walks the record stream from `list`: each record's
 * first u16 either terminates the list (0xffff) or is fed to load_state_object_links, whose return is
 * the record length used to advance. A zero length (unknown type) also ends the walk. */
uint32_t load_state_record_list(uint32_t list)
{
    SVLOG("load_state_record_list ENTER list=0x%x\n", list);
    sv_ls_header = sv_ls_resolve = sv_ls_unknown = 0;          /* reset the SAVEDBG path tally */
    G32(VA_g_state_record_list_count) = 0;                                          /* mov dword [0x89f5c], 0 */
    uint32_t edi = list;                                       /* mov edi, eax */
    uint32_t recs = 0;
    for (;;) {
        uint16_t w = RW(edi);                                  /* mov ax, word[edi] */
        if (w == 0xffffu) break;                               /* cmp ax,-1; je done */
        uint32_t n = sv_load_state_object_links(w, edi);       /* call 0x35839 */
        edi += n;                                              /* add edi, eax */
        recs++;
        if (n == 0) break;                                     /* or eax,eax; jne loop */
    }
    SVLOG("load_state_record_list DONE recs=%u (term=%s) [header=%u resolve=%u unknown=%u]\n",
          recs, (RW(edi) == 0xffffu) ? "0xffff" : "zero-len",
          sv_ls_header, sv_ls_resolve, sv_ls_unknown);
    return 0;                                                  /* caller (0x21713) discards EAX */
}

/* ============================ write_snapshot_lbm (0x3cb85) ============================
 * void __watcall write_snapshot_lbm(void);
 *
 * Prepare the next screenshot/LBM snapshot: build the filename from the "C:\Snap?.lbm" template
 * (0x7248b) into g_snapshot_filename_buf (0x8b370), expanding the '?' to the snapshot counter; then
 * bump the counter, raise the "snapshot pending" flag, and allocate the 64 KB capture buffer.
 *
 * Filename: copy the template char-by-char; on the '?' the counter word g@0x72480 is expanded to three
 * decimal digits IN PLACE — faithfully the original's chained 16-bit divides: ax/1000 (thousands digit
 * written then immediately OVERWRITTEN, never advancing edi), then (counter%1000)/100 = hundreds,
 * /10 = tens, %10 = ones, so the field is (counter % 1000) as exactly 3 zero-padded digits and the '?'
 * widens to 3 bytes. NUL terminates (copied from the template). Then: counter g@0x72480++, the pending
 * flag g@0x90c04 = 0xffff, and a 0x10000-byte allocation via the lifted game_heap_alloc_round4 whose
 * pointer is stored at g@0x8b35c. The original's alloc-fail branch (`jb`) tail-jumps OUT to the disk
 * error path 0x3ce3a (a different function); on success (the only in-band path) it stores the ptr and
 * returns — modeled here as "store only when the alloc succeeds (nonzero)". */
void write_snapshot_lbm(void)
{
    uint32_t edi = (uint32_t)GADDR(VA_g_snapshot_filename_buf);            /* dest: g_snapshot_filename_buf */
    uint32_t esi = (uint32_t)GADDR(VA_g_snapshot_filenames + 0x9);            /* src:  "C:\Snap?.lbm" template */

    for (;;) {
        uint8_t al = RB(esi);
        WB(edi, al);                                    /* copy the template char (incl. NUL) */
        if (al == 0) break;
        if (al == 0x3f) {                               /* '?' -> 3 decimal digits of (counter % 1000) */
            uint32_t cnt = (uint16_t)G16(VA_g_snapshot_counter);
            uint32_t r1000 = cnt % 1000;
            WB(edi, (uint8_t)(((cnt / 1000) & 0xff) + 0x30));   /* thousands (dead-overwritten next) */
            WB(edi, (uint8_t)((r1000 / 100) + 0x30));            /* hundreds (overwrites at same edi)  */
            edi++;
            WB(edi, (uint8_t)(((r1000 % 100) / 10) + 0x30));     /* tens  */
            edi++;
            WB(edi, (uint8_t)((r1000 % 10) + 0x30));             /* ones  */
        }
        esi++; edi++;
    }

    G16(VA_g_snapshot_counter) = (uint16_t)(G16(VA_g_snapshot_counter) + 1);        /* counter++          */
    G16(VA_g_flush_predraw_flag) = (uint16_t)0xffff;                    /* snapshot pending   */
    uint32_t p = game_heap_alloc_round4(0x10000);
    if (p != 0) G32(VA_g_snapshot_work_buf) = (int32_t)p;              /* store the 64KB capture buffer ptr */
}

/* ============================ capture_screen_thumbnail (0x142b7) ============================
 * void __watcall capture_screen_thumbnail(void *dst, const uint8_t *src, const uint8_t *blend_lut);
 *   dst        in EAX  — output buffer: an 8-byte header {u16 type=2, 0, u16 w=0x4e, u16 h=0x38} then
 *                        the 78*56 = 4368 8-bit thumbnail pixels.
 *   src        — the source framebuffer base (the engine reads g@0x90a98; here a plain base ptr).
 *   blend_lut  — the 64KB averaging table the engine reaches as `fs:[bx]` (fs self-loaded from the
 *                selector g@0x90be2); lut[(hi<<8)|lo] = blended(hi,lo). Passed as a base ptr.
 *
 * Pure 2x2-box-average downscale of the screen to a fixed 78x56 thumbnail (NO DOS-IO, NO callees —
 * answers savegame GAP #4). Each output pixel blends a 2x2 source neighbourhood through the LUT twice:
 * al = lut[word(src@col)] (horizontal pair, row 0), bl = lut[word(src@col+stride)] (row 1), then
 * out = lut[(al<<8)|bl] (vertical blend of the two). The source is walked with fixed-point Bresenham
 * steps: horizontal hstep = ((width-1)<<16)/78 split as the original's SMC-patched `add ebp,(hstep<<16)`
 * / `adc esi,(hstep>>16)` (ebp's high half is the sub-pixel accumulator, its overflow carries one source
 * pixel into the column pointer) — modeled here as a plain accumulator, the self-modifying immediate
 * write is NOT reproduced; vertical vstep = ((height-1)<<16)/56 accumulated in a 16-bit fraction that
 * advances the row pointer by whole rows. Geometry globals: start_row g@0x85ce4 and height g@0x85cdc are
 * DOUBLED when g@0x90cbd != 0 (line-doubled video mode); width g@0x85cd8 is never doubled; stride
 * g@0x85498; column offset g@0x85ce0. The engine's source base is g@0x90a98 (passed in as `src`). */
void capture_screen_thumbnail(uint32_t dst, uint32_t src, uint32_t lut)
{
    WW(dst + 0, 2);  WW(dst + 2, 0);  WW(dst + 4, 0x4e);  WW(dst + 6, 0x38);
    uint32_t out = dst + 8;                          /* edi: thumbnail pixel cursor */

    uint32_t start_row = (uint32_t)G32(VA_g_view_y);
    uint32_t height    = (uint32_t)G32(VA_g_view_h);
    if (RB((uint32_t)GADDR(VA_g_hires_line_doubling_flag)) != 0) { start_row += start_row; height += height; }

    uint32_t stride    = (uint32_t)G32(VA_g_screen_pitch);
    uint32_t width     = (uint32_t)G32(VA_g_view_w);
    uint32_t col_off   = (uint32_t)G32(VA_g_view_x);

    uint32_t hstep = ((width  - 1) << 16) / 0x4e;    /* per output column (16.16) */
    uint32_t vstep = ((height - 1) << 16) / 0x38;    /* per output row    (16.16) */
    uint32_t add_lo = (hstep & 0xffffu) << 16;       /* SMC `add ebp` immediate    */
    uint32_t add_hi = hstep >> 16;                   /* SMC `adc esi` immediate    */

    uint32_t row_esi = start_row * stride + col_off; /* source byte-offset of the current row start */
    uint32_t vacc = 0;

    for (int row = 0; row < 0x38; row++) {
        uint32_t ebp = 0;                            /* horizontal sub-pixel accumulator */
        uint32_t esi = row_esi;                      /* moving column scanner            */
        for (int col = 0; col < 0x4e; col++) {
            uint8_t al = RB(lut + RW(src + esi));            /* lut[word(src@col)]        row 0 */
            uint8_t bl = RB(lut + RW(src + esi + stride));   /* lut[word(src@col+stride)] row 1 */
            WB(out, RB(lut + (((uint32_t)al << 8) | bl)));   /* lut[(al<<8)|bl] = 2x2 avg */
            out++;
            uint64_t s = (uint64_t)ebp + add_lo;     /* add ebp, imm  (sets carry) */
            ebp = (uint32_t)s;
            esi += add_hi + (uint32_t)(s >> 32);     /* adc esi, imm                */
        }
        uint32_t t = vacc + vstep;
        vacc = t & 0xffffu;                          /* 16-bit fraction kept (word write-back) */
        row_esi += stride * (t >> 16);               /* advance whole source rows  */
    }
}

/* ============================ save_snapshot_file (0x3cc02) ============================
 * void __watcall save_snapshot_file(void);   // reads all state from globals
 *
 * The deferred ILBM/IFF screenshot writer. Armed by write_snapshot_lbm (which sets the gate g@0x90c04
 * and allocates the 0x10000-byte capture buffer at g@0x8b35c); run on the next frame by
 * flush_predraw_hook 0x3cf19 when the gate is set. Writes a FORM PBM file (BMHD + CMAP 6->8-bit palette
 * + PackBits-RLE BODY) to the "C:\Snap<n>.lbm" path built into g@0x8b370, lseek-back-patches the FORM and
 * BODY chunk sizes, frees the capture buffer, fires the optional completion callback g@0x8b368 (the ANIM
 * multi-frame finalizer), and marks the whole screen dirty. NO int 0x10; the inline int 0x21 file ops go
 * through the host soft-int hook (g_os_soft_int). Self-disarms (clears g@0x90c04). Transcribed from
 * the disasm; LIVE-SWAP only (real DOS file I/O) — validate by byte-diffing the .lbm vs the original. */

/* one inline DOS int 0x21 from the lift via the host hook: returns EAX; CF -> *cf (1=error); EDX -> *pedx. */
static uint32_t sv_int21(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx, int *cf, uint32_t *pedx)
{
    regs_t io; memset(&io, 0, sizeof io);
    io.eax = eax; io.ebx = ebx; io.ecx = ecx; io.edx = edx;
    uint32_t fl = g_os_soft_int ? g_os_soft_int(0x21, &io) : 1u;   /* NULL in oracle (never reached) */
    if (cf)   *cf   = (int)(fl & 1u);
    if (pedx) *pedx = io.edx;
    return io.eax;
}

static uint16_t sv_bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static uint32_t sv_bswap32(uint32_t v) { return __builtin_bswap32(v); }

/* emit_literal_run (0x3cf86): PackBits-flush a pending literal run of *pebx bytes ending at `esi`
 * (a positive (count-1) length byte then the count literal bytes from esi-count). Returns advanced dst. */
static uint32_t sv_rle_emit_literal(uint32_t esi, uint32_t edi, uint32_t *pebx)
{
    uint32_t ebx = *pebx;
    if (ebx != 0) {
        uint32_t s = esi - ebx;                      /* sub esi,ebx -> literal start */
        WB(edi, (uint8_t)(ebx - 1)); edi++;          /* length byte = count-1 (positive) */
        while (ebx != 0) { WB(edi, RB(s)); edi++; s++; ebx--; }   /* movsb count times */
    }
    *pebx = 0;
    return edi;
}

/* compress_scanline (0x3cf30): PackBits-RLE one scanline of width g@0x85498 from `src` into `dst`.
 * Mirrors the original's esi/edi/ebx(pending literal)/edx(run len)/ecx(remaining)/al(current) dance:
 * runs of >=3 (or ==2 with no pending literal) become a replicate; shorter runs accumulate as literals
 * (flushed at 128 bytes or end-of-line). Returns the advanced dst. */
static uint32_t sv_rle_compress_scanline(uint32_t src, uint32_t dst)
{
    uint32_t esi = src, edi = dst, ebx = 0;
    int32_t  ecx = (int32_t)G32(VA_g_screen_pitch);            /* width (remaining pixels) */
    for (;;) {
        uint32_t edx = 0;                            /* run length */
        uint8_t  al  = RB(esi);
        while (RB(esi) == al && (int32_t)edx < ecx && (uint8_t)edx < 0x80) { esi++; edx++; }
        esi -= edx;                                  /* rewind to run start */
        int emitRep = (edx >= 3) || (ebx == 0 && edx == 2);
        if (emitRep) {                               /* 0x3cf5b: flush literal + replicate run */
            edi = sv_rle_emit_literal(esi, edi, &ebx);
            WB(edi, (uint8_t)(0u - ((edx - 1) & 0xffu))); edi++;   /* dl--; neg dl -> -(run-1) */
            WB(edi, al); edi++;                      /* the repeated byte */
            esi += edx;                              /* advance past run */
            ecx -= (int32_t)edx;
            if (ecx > 0) continue;                   /* jg .outer */
            return edi;
        }
        /* .accum (0x3cf71): take one literal byte */
        ebx++; esi++; ecx--;
        if (ecx == 0) { return sv_rle_emit_literal(esi, edi, &ebx); }   /* flush at end-of-line */
        if ((uint8_t)ebx >= 0x80) edi = sv_rle_emit_literal(esi, edi, &ebx);   /* flush at 128 */
        /* else continue accumulating */
    }
}

void save_snapshot_file(void)
{
    G16(VA_g_flush_predraw_flag) = 0;                                            /* clear the arm gate (self-disarm) */

    if ((uint32_t)G32(VA_g_snapshot_work_buf) != 0) {                          /* have a capture buffer -> write the .lbm */
        /* delete any existing file, then create it */
        sv_int21(0x4100, 0, 0, GADDR(VA_g_snapshot_filename_buf), NULL, NULL);     /* int21 AH=41 delete (edx=name) */
        int cf;
        uint32_t handle = sv_int21(0x3c00, 0, 0, GADDR(VA_g_snapshot_filename_buf), &cf, NULL);  /* AH=3c create -> EAX=handle */

        if (!cf) {                                              /* create ok -> emit the whole file */
            G32(VA_g_snapshot_work_buf + 0x8) = handle;

            /* init the RLE scratch tail (g@0x8b35c + 0xd04, width/2 dwords) to 0xffffffff */
            uint32_t n   = (uint32_t)G32(VA_g_screen_pitch) >> 1;
            uint32_t pdw = (uint32_t)G32(VA_g_snapshot_work_buf) + 0xd04;
            for (uint32_t i = 0; i < n; i++) { WD(pdw, 0xffffffffu); pdw += 4; }

            /* palette: convert the VGA 6-bit DAC snapshot (conventional-memory seg g@0x90bca) to 8-bit,
             * 0x300 bytes, into the head of the capture buffer */
            uint32_t pdst = (uint32_t)G32(VA_g_snapshot_work_buf);
            uint32_t psrc = (uint32_t)((uint32_t)G16(VA_g_vel_queue_b + 0x88) << 4);   /* real-mode seg<<4 = conv linear */
            for (uint32_t i = 0; i < 0x300; i++) {
                uint8_t al = RB(psrc); psrc++;
                uint8_t ah = (uint8_t)(al >> 6);
                al = (uint8_t)((al << 2) | ah);                 /* 6-bit -> 8-bit (replicate top 2 bits) */
                WB(pdst, al); pdst++;
            }

            /* IFF FORM/PBM/BMHD/CMAP header at g@0x72498 (48 bytes), big-endian fields */
            uint32_t hh   = GADDR(VA_g_snapshot_filenames + 0x16);
            uint16_t w_be = sv_bswap16((uint16_t)G32(VA_g_screen_pitch)); /* width  */
            uint16_t h_be = sv_bswap16((uint16_t)G32(VA_g_screen_height)); /* height */
            WD(hh + 0x00, 0x4d524f46);                          /* 'FORM' */
            WD(hh + 0x04, 0);                                   /* FORM size (patched at the end) */
            WD(hh + 0x08, 0x204d4250);                          /* 'PBM ' */
            WD(hh + 0x0c, 0x44484d42);                          /* 'BMHD' */
            WD(hh + 0x10, 0x14000000);                          /* BMHD size = 0x14 (BE) */
            WW(hh + 0x14, w_be); WW(hh + 0x24, w_be);           /* BMHD w + page w */
            WW(hh + 0x16, h_be); WW(hh + 0x26, h_be);           /* BMHD h + page h */
            WD(hh + 0x18, 0);                                   /* x,y origin */
            WD(hh + 0x1c, 0x10008);                             /* nPlanes=8, masking=0, compress=1 */
            WD(hh + 0x20, 0x605ff00);                           /* transparentColor / aspect */
            WD(hh + 0x28, 0x50414d43);                          /* 'CMAP' */
            WD(hh + 0x2c, 0x30000);                             /* CMAP size = 0x300 (BE) */

            /* write the 0x30-byte header, the 0x300-byte CMAP body, then 'BODY' tag + size placeholder */
            sv_int21(0x4000, (uint32_t)G32(VA_g_snapshot_work_buf + 0x8), 0x30,  GADDR(VA_g_snapshot_filenames + 0x16),         NULL, NULL);
            sv_int21(0x4000, (uint32_t)G32(VA_g_snapshot_work_buf + 0x8), 0x300, (uint32_t)G32(VA_g_snapshot_work_buf), NULL, NULL);
            WD(GADDR(VA_g_snapshot_filenames + 0x16), 0x59444f42);                     /* 'BODY' (size dword at +4 still 0) */
            sv_int21(0x4000, (uint32_t)G32(VA_g_snapshot_work_buf + 0x8), 8, GADDR(VA_g_snapshot_filenames + 0x16), NULL, NULL);

            /* BODY: PackBits-RLE each scanline into the capture buffer, flushing every >=0x8000 bytes */
            G32(VA_g_snapshot_work_buf + 0x10) = 0;                                   /* body byte counter */
            uint32_t edi  = (uint32_t)G32(VA_g_snapshot_work_buf);             /* RLE output cursor */
            uint32_t esi  = (uint32_t)G32(VA_g_framebuffer_ptr);             /* framebuffer source */
            int32_t  rows = (int32_t)G32(VA_g_screen_height);              /* height */
            while (rows > 0) {
                edi = sv_rle_compress_scanline(esi, edi);
                esi += (uint32_t)G32(VA_g_screen_pitch);                  /* advance src by stride */
                uint32_t used = edi - (uint32_t)G32(VA_g_snapshot_work_buf);
                if (used >= 0x8000) {                           /* flush a full buffer */
                    G32(VA_g_snapshot_work_buf + 0x10) = (uint32_t)G32(VA_g_snapshot_work_buf + 0x10) + used;
                    sv_int21(0x4000, (uint32_t)G32(VA_g_snapshot_work_buf + 0x8), used, (uint32_t)G32(VA_g_snapshot_work_buf), NULL, NULL);
                    edi = (uint32_t)G32(VA_g_snapshot_work_buf);
                }
                rows--;
            }
            uint32_t used = edi - (uint32_t)G32(VA_g_snapshot_work_buf);       /* final partial flush */
            G32(VA_g_snapshot_work_buf + 0x10) = (uint32_t)G32(VA_g_snapshot_work_buf + 0x10) + used;
            if (used != 0)
                sv_int21(0x4000, (uint32_t)G32(VA_g_snapshot_work_buf + 0x8), used, (uint32_t)G32(VA_g_snapshot_work_buf), NULL, NULL);

            /* pad BODY to an even byte count */
            if ((uint32_t)G32(VA_g_snapshot_work_buf + 0x10) & 1) {
                WB((uint32_t)G32(VA_g_snapshot_work_buf), 0);
                sv_int21(0x4000, (uint32_t)G32(VA_g_snapshot_work_buf + 0x8), 1, (uint32_t)G32(VA_g_snapshot_work_buf), NULL, NULL);
            }

            /* patch the chunk sizes: BE body size at the buffer head, BE FORM size at g@0x72498 */
            uint32_t body = (uint32_t)G32(VA_g_snapshot_work_buf + 0x10);
            WD((uint32_t)G32(VA_g_snapshot_work_buf), sv_bswap32(body));       /* BE body size */
            uint32_t form = ((body + 1) & ~1u) + 0x330;         /* round body up to even (and al,0xfe = clear bit0
                                                                 * of the 32-bit value) + 0x30 hdr + 0x300 cmap */
            WD(GADDR(VA_g_snapshot_filenames + 0x16), sv_bswap32(form));               /* BE FORM size */
            sv_int21(0x4200, (uint32_t)G32(VA_g_snapshot_work_buf + 0x8), 0, 0x334, NULL, NULL);   /* lseek BODY size field */
            sv_int21(0x4000, (uint32_t)G32(VA_g_snapshot_work_buf + 0x8), 4, (uint32_t)G32(VA_g_snapshot_work_buf), NULL, NULL);
            sv_int21(0x4200, (uint32_t)G32(VA_g_snapshot_work_buf + 0x8), 0, 4, NULL, NULL);       /* lseek FORM size field */
            sv_int21(0x4000, (uint32_t)G32(VA_g_snapshot_work_buf + 0x8), 4, GADDR(VA_g_snapshot_filenames + 0x16), NULL, NULL);

            sv_int21(0x3e00, (uint32_t)G32(VA_g_snapshot_work_buf + 0x8), 0, 0, NULL, NULL);       /* close */
        }

        /* success AND create-error both: free the capture buffer, fire the optional ANIM-finalize callback */
        sv_call_eax(0x40a2a, (uint32_t)G32(VA_g_snapshot_work_buf));           /* game_free_if_not_null(capture buffer) */
        if ((uint32_t)G32(VA_g_snapshot_work_buf + 0xc) != 0) {                      /* optional completion callback (reads globals) */
            regs_t io; memset(&io, 0, sizeof io);
            io.va = (uint32_t)G32(VA_g_snapshot_work_buf + 0xc);                     /* relocated fn ptr */
#ifndef ROTH_STANDALONE
            call_orig(&io);
#else
            roth_unreachable((uint32_t)G32(VA_g_snapshot_work_buf + 0xc) - OBJ_DELTA);  /* ANIM-finalize code-ptr (save path, off title) */
#endif
        }
    } else {                                                    /* no capture buffer */
        sv_call_eax(0x40a2a, (uint32_t)G32(VA_g_snapshot_work_buf));           /* free (no-op if 0) */
        G16(VA_g_flush_predraw_flag) = 0;
    }

    add_dirty_rect(0, 0, (uint32_t)G32(VA_g_screen_pitch), (uint32_t)G32(VA_g_screen_height));  /* add_dirty_rect(full screen) */
}

/* ============================ take_snapshot (0x11135) ============================
 * void __watcall take_snapshot(void);   // INTERACTIVE entry (snapshot key B -> check_snapshot_key)
 *
 * The snapshot front-end. Saves the frame tick, switches to DOS text mode (int 0x10 AH=0 mode 3) so the
 * DOS-console snapshot menu can prompt, runs snapshot_menu_and_save (returns CF: clear=do it / set=cancel),
 * restores the prior video mode (VESA 0x4f02 if g@0x76634 set, else VGA mode 0x13 + the hi-res line-doubling
 * recompute), re-uploads the palette / re-configures the render viewport / reloads the DAS palette, and — if
 * the menu said "do it" — ARMS the deferred capture (g@0x90c04 = 0xffff + a 0x10000-byte capture buffer at
 * g@0x8b35c, the original's tail-jump into the write_snapshot_lbm arm tail 0x3cbe3) so the next
 * flush_predraw_hook fires the lifted save_snapshot_file. On cancel it just marks the screen dirty.
 *
 * INTERACTIVE (the bridged menu blocks on keyboard input) -> lift_is_interactive(0x11135) raises
 * g_os_interactive so shm_tick stands in for the frozen ISRs (advance tick + forward scancodes), exactly
 * like prompt_save_overwrite. int 0x10 mode-sets run through g_os_soft_int; the render-resetup callees +
 * the menu + the heap alloc are BRIDGED via call_orig. */

/* one inline int 0x10 from the lift via the host hook (returns EAX; CF unused by the mode-sets here). */
static uint32_t sv_int10(uint32_t eax, uint32_t ebx)
{
    regs_t io; memset(&io, 0, sizeof io);
    io.eax = eax; io.ebx = ebx;
    if (g_os_soft_int) g_os_soft_int(0x10, &io);
    return io.eax;
}

/* bridge an original at canon_va with EAX in, returning EAX and the post-call CF (*cf). */
static uint32_t sv_bridge_cf(uint32_t canon_va, uint32_t eax, int *cf)
{
    regs_t io; memset(&io, 0, sizeof io);
    io.va = canon_va + OBJ_DELTA; io.eax = eax;
#ifndef ROTH_STANDALONE
    call_orig(&io);
    if (cf) *cf = (int)(io.eflags & 1u);
    return io.eax;
#else
    /* M3: was an UN-guarded call_orig (previously gc'd out of the imgfree link; the take_snapshot
     * route now keeps this subtree). Sole target 0x40a17 game_heap_alloc_round4 — its lifted body
     * has no CF exposure (the caller consumes CF as the alloc-fail flag), so this stays fail-loud
     * rather than guessing the CF mapping. */
    if (cf) *cf = 1;
    roth_unreachable(canon_va);   /* snapshot-ANIM heap alloc (CF consumer) — off the bare-title path */
    return 0;
#endif
}

/* print a '$'-terminated DOS string (canon address) via int 0x21 AH=9 through the soft-int hook. */
static void sv_print(uint32_t canon_str) { sv_int21(0x900, 0, 0, GADDR(canon_str), NULL, NULL); }

/* ============================ snapshot_menu_and_save (0x111a0) ============================
 * int __watcall snapshot_menu_and_save(void);   // returns CF: clear (1) = do it, set (0) = cancel
 *
 * The interactive DOS-console snapshot menu. Prints the main "[1] Single / [2] Anim" prompt, reads a key
 * (console_read_key -> the game ring), then runs the matching sub-menu (single: filename + [Return]/[N]ame/
 * [P]ath/[F]rame editor via 0x1134d; anim: filename + frame-count + the 0x11382 editor). On Return it commits
 * the frame-number globals (single: g@0x72480 = g@0x70746++; anim: g@0x72480/0x76850/0x90bcc + the multi-frame
 * callback g@0x8b368 = 0x112f6 when >=2 frames) and returns "do it"; Backspace cancels. All console I/O is
 * BRIDGED (prints via the soft-int hook int 0x21 AH=9; key reads + field editors + filename/number builders
 * via call_orig) — the menu only contributes the control-flow skeleton. INTERACTIVE: runs inside
 * take_snapshot's interactive dispatch (g_os_interactive) so the blocking key reads are pumped by shm_tick. */
static int sv_snapshot_menu_and_save(void)
{
    for (;;) {                                            /* 0x111a0: (re)print the main menu */
        sv_print(0x705e0);                                /* "[1] Single...[2] Anim...Press 1 or 2 :" */
        uint8_t sel;
        int invalid = 0;
        for (;;) {                                        /* 0x111ac: read the main selection */
            sel = (uint8_t)sv_call_eax(0x11548, 0);       /* console_read_key */
            if (sel == 8)   return 0;                     /* Backspace -> cancel (stc) */
            if (sel == 0)   continue;
            if (sel == 0xa) sel = (uint8_t)G8(VA_g_arg_command_table + 0x178);   /* Return -> use the stored default */
            if (sel == '1' || sel == '2') break;
            sv_call_eax(0x114d4, sel);                    /* echo, then reprint the menu (0x111ce->0x111a0) */
            invalid = 1; break;
        }
        if (invalid) continue;

        G8(VA_g_arg_command_table + 0x178) = sel;                                /* 0x111d5: store the selection */
        sv_call_eax(0x114d4, sel);                        /* echo the digit */

        if (sel == '1') {                                 /* ---- single frame (0x111e7) ---- */
            for (;;) {                                    /* (re)print after a field edit */
                sv_call_eax(0x11500, 0);                  /* build_snapshot_anim_filename */
                sv_print(0x70620);
                sv_bridge(0x114a3, GADDR(VA_g_snapshot_filename_buf), GADDR(VA_g_arg_command_table + 0x1aa), 0, 0);   /* "Filename :" + name */
                sv_print(0x70666);                        /* "[Return] Use, [N]ame, [P]ath, [F]rame :" */
                int reprint = 0;
                for (;;) {                                /* 0x011213: read the field key */
                    uint8_t k = (uint8_t)sv_call_eax(0x11548, 0);
                    if (k == 0) continue;
                    if (k == 8) return 0;                 /* cancel */
                    if (k == 0xa) {                       /* 0x1122d: accept single */
                        G16(VA_g_snapshot_counter) = G16(VA_g_snapshot_anim_frame);
                        G16(VA_g_snapshot_anim_frame) = (uint16_t)(G16(VA_g_snapshot_anim_frame) + 1);
                        G32(VA_g_snapshot_work_buf + 0xc) = 0;
                        return 1;                         /* do it (clc) */
                    }
                    /* re-point 0x1134d (scanner-invisible sv_bridge_cf site): AL=key -> CF */
                    int cf = (int)dispatch_config_field_key_alt(k);
                    if (cf) continue;                     /* unrecognized -> re-read (jb 0x11213) */
                    reprint = 1; break;                   /* field edited -> reprint (jmp 0x111e7) */
                }
                if (reprint) continue;
            }
        } else {                                          /* ---- anim (0x1124e) ---- */
            for (;;) {
                sv_call_eax(0x114e2, 0);                  /* build_screenshot_filename */
                sv_print(0x70636);
                sv_bridge(0x114a3, GADDR(VA_g_snapshot_filename_buf), GADDR(VA_g_arg_command_table + 0x1aa), 0, 0);   /* "Filename :" + name */
                {   /* num_to_decimal_digits(ax=count, edi=buf) -> advanced edi; caller NUL-terminates */
                    uint8_t *e = num_to_decimal_digits((uint16_t)G16(VA_g_snapshot_anim_frame + 0x2),
                                                              (uint8_t *)(uintptr_t)GADDR(VA_g_snapshot_basename + 0x5));
                    *e = 0;                               /* null-terminate (sub eax,eax; stosb) */
                }
                sv_bridge(0x114a3, GADDR(VA_g_snapshot_basename + 0x5), GADDR(VA_g_arg_command_table + 0x1b7), 0, 0);   /* "Frames :" + number */
                sv_print(0x70696);                        /* "[Return] Use, [N]ame, [P]ath, [F]rame :" */
                int reprint = 0;
                for (;;) {                                /* 0x011299: read the field key */
                    uint8_t k = (uint8_t)sv_call_eax(0x11548, 0);
                    if (k == 8) return 0;                 /* cancel */
                    if (k == 0) continue;
                    if (k == 0xa) {                       /* 0x112b3: accept anim */
                        G16(VA_g_frame_tick_counter) = G16(VA_g_console_input_buffer + 0x6);
                        G16(VA_g_snapshot_counter) = G16(VA_g_snapshot_name_seq);
                        G16(VA_g_console_input_buffer + 0x4) = G16(VA_g_snapshot_anim_frame + 0x2);
                        G32(VA_g_snapshot_work_buf + 0xc) = 0;
                        if (G16(VA_g_console_input_buffer + 0x4) >= 2) {          /* >=2 frames -> set the ANIM finalize callback + do it */
                            G32(VA_g_snapshot_work_buf + 0xc) = 0x112f6 + OBJ_DELTA;
                            return 1;
                        }
                        return 0;                         /* <2 frames -> cancel (CF set by the cmp) */
                    }
                    /* re-point 0x11382 (scanner-invisible sv_bridge_cf site): AL=key -> CF */
                    int cf = (int)dispatch_config_field_key(k);
                    if (cf) continue;                     /* unrecognized -> re-read (jb 0x11299) */
                    reprint = 1; break;                   /* field edited -> reprint (jmp 0x1124e) */
                }
                if (reprint) continue;
            }
        }
    }
}
/* registry thunk (cold: the only caller is the lifted take_snapshot, which calls sv_ directly). */
void snapshot_menu_and_save(void) { (void)sv_snapshot_menu_and_save(); }

void take_snapshot(void)
{
    G16(VA_g_console_input_buffer + 0x6) = G16(VA_g_frame_tick_counter);                          /* save the current frame tick */
    sv_int10(3, 0);                                       /* int 0x10 AH=0: set DOS text mode (for the menu) */

    int do_it = sv_snapshot_menu_and_save();             /* 1 = do it (clc), 0 = cancel (stc); lifted directly */

    /* restore the prior video mode */
    if ((uint32_t)G32(VA_g_video_linear_flag) != 0) {
        sv_int10(0x4f02, (uint32_t)G32(VA_g_video_linear_flag));         /* VESA set mode (saved VBE mode number) */
    } else {
        sv_int10(0x13, 0);                                /* VGA mode 0x13 */
        recompute_hires_line_doubling();           /* recompute_hires_line_doubling */
    }
    upload_palette_dac();                          /* upload_palette_dac */
    configure_render_viewport();                   /* configure_render_viewport */
    (void)read_das_palette();                      /* read_das_palette (CF discarded, as the bridge did) */

    if (do_it) {                                          /* arm the deferred capture (tail-jump to 0x3cbe3) */
        G16(VA_g_flush_predraw_flag) = 0xffff;
        int acf;
        uint32_t buf = sv_bridge_cf(0x40a17, 0x10000, &acf);   /* game_heap_alloc(64KB) -> EAX, CF=fail */
        if (!acf) {
            G32(VA_g_snapshot_work_buf) = buf;
        } else {
            /* alloc fail (rare): the original jumps into save_snapshot_file's free+dirty tail (0x3ce3a..) */
            sv_call_eax(0x40a2a, (uint32_t)G32(VA_g_snapshot_work_buf));
            if ((uint32_t)G32(VA_g_snapshot_work_buf + 0xc) != 0) {
                regs_t io; memset(&io, 0, sizeof io); io.va = (uint32_t)G32(VA_g_snapshot_work_buf + 0xc);
#ifndef ROTH_STANDALONE
                call_orig(&io);
#else
                roth_unreachable(io.va - OBJ_DELTA);   /* ANIM-finalize code-ptr (0x112f6 = mid-fn entry; alloc-fail arm) */
#endif
            }
            add_dirty_rect(0, 0, (uint32_t)G32(VA_g_screen_pitch), (uint32_t)G32(VA_g_screen_height));
        }
    } else {                                              /* cancel (0x11189): mark the screen dirty */
        add_dirty_rect(0, 0, (uint32_t)G32(VA_g_screen_pitch), (uint32_t)G32(VA_g_screen_height));   /* add_dirty_rect */
    }
}

/* ===================== Layer C — DOS-IO save/load orchestrators (in-game LIVE-SWAP only) =====================
 * These call DOS file I/O and/or trigger cross-subsystem restores. They are non-idempotent (real file
 * writes, world-state restore) so the static double-run oracle does NOT apply — they are verified by an
 * in-game ROTH_LIFT live-swap. Every callee that leaves the subsystem is BRIDGED via sv_bridge/call_orig
 * (the host runs the original DOS/pool/path bytes natively). Register inputs at each call site are
 * transcribed verbatim from the disasm + each bridge's own ABI. */

/* ============================ bundle_level_states (0x2198e) ============================
 * uint32_t __watcall bundle_level_states(uint32_t save_handle);   // EAX = open save-file handle; ret 1 ok / 0 fail
 *
 * Append every per-level temp state file (the ones the map writer dropped, matched by a filename
 * pattern) into the open save file, each as a tagged record: a {tag 8, name-len 0xe} u16 pair, the
 * 0xe-byte filename, a {tag 9, file-size} u16 pair, then the file bytes copied through a temp heap
 * buffer in <=0xfde8-byte chunks. Threads the das-cache heap buffer (handle g@0x85c3c, re-dereffed each
 * use since the chunk is relocatable) + the enumerate result list (0xe-byte entries). Bridges:
 * ensure_das_cache_heap_space 0x414d2, pool_alloc/free_handle 0x360f9/0x360b3, build_game_path 0x2fb7f,
 * enumerate_files_by_pattern 0x217bc, dos_open_file 0x41ae5, dos_get_file_size 0x41bc1, dos_write_u16
 * 0x2196a (writes a {edx,ebx} u16 pair — ignores incoming ECX), dos_read_items/dos_write_items
 * 0x41b53/0x41b7a (eax=buf, edx=item_size, ebx=item_count, ecx=handle), dos_close_handle 0x41b41. The
 * format strings 0x75e8e / 0x75e98 / 0x76540 are obj3 data (rebased absolutes). On any failure (no
 * heap handle / open fails) it returns 0 WITHOUT freeing the handle — faithful to the original's leak. */
uint32_t bundle_level_states(uint32_t save_h)
{
    uint8_t path[0x80];        /* the original's [ebp-0x5e] path scratch  */
    uint8_t patbuf[0x400];     /* the [ebp-0x31a] enumerate result list (0xe-byte entries) */

    SVLOG("bundle ENTER save_h=0x%x das_handle=0x%x\n", save_h, (uint32_t)G32(VA_g_das_cache_heap_handle));
    ensure_das_cache_heap_space(0xfde8);                       /* ensure_das_cache_heap_space */
    /* pool_alloc_handle takes the allocation SIZE (EDX) — passed explicitly (the original relied on the
     * leftover edx=0xfde8 from the ensure setup; the direct call makes that explicit). */
    uint32_t buf_h = pool_alloc_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle), 0xfde8);
    if (buf_h == 0) { SVLOG("buf_h==0 -> return 0\n"); return 0; }

    uint32_t pat_cursor = (uint32_t)(uintptr_t)patbuf;
    build_game_path((uint8_t *)(uintptr_t)path,                /* build_game_path (the pattern) */
              (const uint8_t *)(uintptr_t)GADDR(VA_g_dir_gdv + 0x50), (const uint8_t *)(uintptr_t)GADDR(VA_g_heap_free_list + 0x87e));
    uint32_t count = enumerate_files_by_pattern((uint32_t)(uintptr_t)path,  /* -> count */
                               (uint32_t)(uintptr_t)patbuf);
    SVLOG("buf_h=0x%x pattern='%s' count=%u\n", buf_h, (char *)path, count);

    while (count != 0) {
        build_game_path((uint8_t *)(uintptr_t)path,             /* build_game_path (dir prefix)  */
                  (const uint8_t *)(uintptr_t)GADDR(VA_g_dir_gdv + 0x50), (const uint8_t *)(uintptr_t)GADDR(VA_g_heap_free_list + 0x888));
        build_game_path((uint8_t *)(uintptr_t)path,             /* build_game_path (append name) */
                  (const uint8_t *)(uintptr_t)path, (const uint8_t *)(uintptr_t)pat_cursor);
        uint32_t file_h = dos_open_file((uint32_t)(uintptr_t)path, 0);  /* dos_open_file (C2) */
        if (file_h == 0) { SVLOG("open '%s' FAILED -> return 0\n", (char *)path); return 0; }
        uint32_t filesize = sv_bridge(0x41bc1, file_h, 0, 0, 0);       /* dos_get_file_size (OOS-bridged) */

        sv_bridge(0x2196a, save_h, 8, 0xe, save_h);                    /* dos_write_u16 {tag 8, namelen 0xe} */
        dos_write_items(pat_cursor, 1, 0xe, save_h);            /* dos_write_items (0xe-byte name) (C2) */
        sv_bridge(0x2196a, save_h, 9, filesize, save_h);               /* dos_write_u16 {tag 9, filesize}    */

        uint32_t total_r = 0, total_w = 0;
        uint32_t remaining = filesize;
        do {
            uint32_t chunk = remaining > 0xfde8u ? 0xfde8u : remaining;
            uint32_t bytes = dos_read_items(RD(buf_h), 1, chunk, file_h);   /* dos_read_items (C2) */
            total_r += bytes;
            if (bytes != 0)
                total_w += dos_write_items(RD(buf_h), bytes, 1, save_h);    /* dos_write_items (C2) */
            remaining -= chunk;
        } while (remaining != 0);
        SVLOG("  file '%s' fh=0x%x size=%u read=%u wrote=%u\n",
              (char *)path, file_h, filesize, total_r, total_w);

        dos_close_handle(file_h);                               /* dos_close_handle (C2) */
        pat_cursor += 0xe;
        count--;
    }

    pool_free_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle),
                            (uint32_t *)(uintptr_t)buf_h);            /* pool_free_handle */
    SVLOG("bundle DONE -> return 1\n");
    return 1;
}

/* ============================ prompt_save_overwrite (0x26349) ============================
 * uint32_t __watcall prompt_save_overwrite(<regs>);   // -> EAX (user's choice; caller writes iff ==1)
 *
 * The "overwrite existing save?" confirmation. Body (canon, a shared-tail Watcom wrapper):
 *     push edx; mov edx,0x200; mov eax,0x71cad; jmp 0x26335   ; 0x26335: call show_message_box; pop edx; ret
 * i.e. a thin wrapper that calls show_message_box (0x2508f) with EAX = message string 0x71cad, EDX =
 * flags 0x200. The push/pop edx just preserves the incoming EDX across the clobbering call (net stack
 * effect = plain ret), and show_message_box reads ONLY EAX+EDX — so this lifts to a direct
 * show_message_box(GADDR(0x71cad), 0x200), the same form as show_resource_error_box 0x2632a in
 * lift_menu_hud_ui.c. The incoming regs (`in`) are therefore unused.
 *
 * INTERACTIVE: show_message_box is a blocking menu (zoom-open animation + input loop) that spins on the
 * frame-tick counter (0x90bcc) and waits on the game input ring (0x90c1c) — both frozen during a normal
 * lift dispatch, which deadlocked the first attempt. It is live-swappable ONLY under the
 * host's interactive-lift mode: lift_is_interactive(0x26349) — and its write_savegame_file caller
 * (0x21dc6) — raises g_os_interactive so shm_tick stands in for the frozen int-8/int-9 ISRs (advances
 * the tick + forwards scancodes into the ring). */
uint32_t prompt_save_overwrite(const regs_t *in)
{
    /* The wrapper is `push edx; mov edx,0x200; mov eax,0x71cad; call show_message_box; pop edx; ret`.
     * show_message_box (0x2508f) reads only EAX (desc) + EDX (flags) — the disasm's push/pop edx merely
     * preserves the incoming EDX across the clobbering call, and show_message_box never reads that stack
     * slot (nor EBX/ECX), which is why the lifted 2-arg proto is complete (see lift_menu_hud_ui.c, where
     * show_resource_error_box 0x2632a is the identical `show_message_box(GADDR(str),0x200)` form).
     * Interactive: reached only under an interactive-lift dispatch (0x26349 itself, or its write_savegame_file
     * caller 0x21dc6) so shm_tick pumps the modal's tick/input-ring exactly as for the bridged callee. */
    (void)in;
    return show_message_box((uint32_t)GADDR(VA_g_vol_music + 0x155), 0x200);
}

/* ============================ copy_save_chunk_to_file (0x21afd) ============================
 * uint32_t __watcall copy_save_chunk_to_file(uint32_t name, uint32_t size, uint32_t src_handle, uint32_t buffer);
 *   EAX=name (filename component), EDX=size, EBX=src_handle (the open save file), ECX=buffer; ret 1 ok / 0 fail.
 *
 * The LOAD-side inverse of bundle: extract ONE level-state chunk from the open save file back out to a
 * temp file. Build the path `<dir 0x76540>\<name>`, create it (dos_open_file mode 1), read up to one
 * 0xfde8 chunk of `size` bytes from src_handle into the caller's buffer, write it to the temp file,
 * close. (One chunk: the per-level temp files are < 0xfde8 = 65000.) Returns 1 on success/empty-read, 0
 * only if the temp write fails or the create fails. All callees bridged (LIVE-SWAP only). */
uint32_t copy_save_chunk_to_file(uint32_t name, uint32_t size, uint32_t src_handle, uint32_t buffer)
{
    uint8_t path[0x100];     /* the original's [ebp-0x198] path scratch */
    build_game_path((uint8_t *)(uintptr_t)path,                /* build_game_path (dir)     */
              (const uint8_t *)(uintptr_t)GADDR(VA_g_dir_gdv + 0x50), (const uint8_t *)(uintptr_t)GADDR(VA_g_heap_free_list + 0x88c));
    build_game_path((uint8_t *)(uintptr_t)path,                /* append the chunk name     */
              (const uint8_t *)(uintptr_t)path, (const uint8_t *)(uintptr_t)name);
    uint32_t temp_h = dos_open_file((uint32_t)(uintptr_t)path, 1);   /* dos_open_file(mode 1=create) (C2) */
    if (temp_h == 0) return 0;

    uint32_t chunk = size > 0xfde8u ? 0xfde8u : size;
    uint32_t bytes = dos_read_items(buffer, 1, chunk, src_handle);       /* dos_read_items (C2) */
    if (bytes != 0) {
        uint32_t w = dos_write_items(buffer, bytes, 1, temp_h);          /* dos_write_items (C2) */
        if (w == 0) { dos_close_handle(temp_h); return 0; }              /* write failed    */
    }
    dos_close_handle(temp_h);                                            /* dos_close_handle (C2) */
    return 1;
}

/* Shared tag-record reader epilogue helper: read a 4-byte record header {u16 tag, u16 len} from `fh`. */

/* ============================ read_savegame_slot_names (0x21cc5) ============================
 * uint32_t __watcall read_savegame_slot_names(uint32_t buf);   // EAX=slot-name buffer (9 slots, stride 0x30)
 *
 * For each of the 9 save slots: build `<dir>\<sprintf(0x75ebd, slot)>`, open it, default the name cell to
 * {0, 0x7e}, then scan the file's 4-byte tag records — tag 0xa = the slot name (read `len` bytes into the
 * cell), tag 1 with len>=3 = keep scanning, anything else (incl. tag<1 / tag 1 len<3) = stop this slot.
 * Closes each file and advances 0x30. The caller ignores the return (the data is the buffer). LIVE-SWAP. */
uint32_t read_savegame_slot_names(uint32_t buf)
{
    uint8_t path[0x140], namebuf[0x40], hdr[8];
    uint32_t cell = buf;
    for (int slot = 1; slot <= 9; slot++, cell += 0x30) {
        build_game_path((uint8_t *)(uintptr_t)path,
                  (const uint8_t *)(uintptr_t)GADDR(VA_g_dir_gdv + 0x50), (const uint8_t *)(uintptr_t)GADDR(VA_g_heap_free_list + 0x8a4));  /* dir */
        sv_sprintf((uint32_t)(uintptr_t)namebuf, (uint32_t)GADDR(VA_g_heap_free_list + 0x8ad), (uint32_t)slot);
        build_game_path((uint8_t *)(uintptr_t)path,
                  (const uint8_t *)(uintptr_t)path, (const uint8_t *)(uintptr_t)namebuf);  /* append slot file name */
        uint32_t fh = dos_open_file((uint32_t)(uintptr_t)path, 0);   /* dos_open_file (C2) */
        WB(cell + 0, 0);
        WB(cell + 1, 0x7e);
        if (fh == 0) continue;
        int got = 0;
        while (!got) {
            dos_read_items((uint32_t)(uintptr_t)hdr, 4, 1, fh);          /* read 4-byte header (C2) */
            uint16_t tag = *(uint16_t *)hdr, len = *(uint16_t *)(hdr + 2);
            if (tag < 1) {
                got = 1;
            } else if (tag == 1) {
                if (len < 3) got = 1;                                           /* len>=3 -> keep scanning */
            } else if (tag == 0xa) {
                dos_read_items(cell, 1, len, fh);                        /* read the name (C2) */
                if (RB(cell) == 0) WB(cell + 1, 0);
                got = 1;
            } else {
                got = 1;
            }
        }
        dos_close_handle(fh);                                            /* dos_close_handle (C2) */
    }
    return 1;
}

/* ============================ read_savegame_thumbnail (0x21b9f) ============================
 * uint32_t __watcall read_savegame_thumbnail(uint32_t slot);   // EAX=slot index; ret das-cache handle / 0
 *
 * Allocate a das-cache buffer (handle g@0x85c3c, size 0x1130 — note pool_alloc_handle takes the size in
 * EDX, fed by the leftover 0x1130, same pattern as bundle), build `<dir>\<sprintf(0x75ea9, slot+1)>`,
 * open it, then scan 4-byte tag records: tag 0xa = skip its `len`-byte name field via dos_lseek(+len);
 * tag 1 len>=3 = keep scanning; tag 0xb = THE THUMBNAIL — read `len` bytes into the buffer, close, return
 * the HANDLE (caller owns it, not freed); anything else = close + free handle + return 0. LIVE-SWAP. */
uint32_t read_savegame_thumbnail(uint32_t slot)
{
    uint8_t path[0x80], namebuf[0xd0], hdr[8];
    ensure_das_cache_heap_space(0x1130);                                 /* ensure_das_cache_heap_space */
    uint32_t h = pool_alloc_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle), 0x1130);  /* pool_alloc_handle(handle, size=0x1130) */
    if (h == 0) return 0;

    build_game_path((uint8_t *)(uintptr_t)path,
              (const uint8_t *)(uintptr_t)GADDR(VA_g_dir_gdv + 0x50), (const uint8_t *)(uintptr_t)GADDR(VA_g_heap_free_list + 0x890));  /* dir */
    sv_sprintf((uint32_t)(uintptr_t)namebuf, (uint32_t)GADDR(VA_g_heap_free_list + 0x899), slot + 1);  /* esi was inc'd before sprintf */
    build_game_path((uint8_t *)(uintptr_t)path,
              (const uint8_t *)(uintptr_t)path, (const uint8_t *)(uintptr_t)namebuf);  /* append slot file name */
    uint32_t fh = dos_open_file((uint32_t)(uintptr_t)path, 0);           /* dos_open_file (C2) */
    if (fh == 0) { pool_free_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle), (uint32_t *)(uintptr_t)h); return 0; }

    for (;;) {
        dos_read_items((uint32_t)(uintptr_t)hdr, 4, 1, fh);              /* read 4-byte header (C2) */
        uint16_t tag = *(uint16_t *)hdr, len = *(uint16_t *)(hdr + 2);
        if (tag == 0xb) {                                                       /* the thumbnail data */
            dos_read_items(RD(h), 1, len, fh);                          /* read into the buffer (C2) */
            dos_close_handle(fh);                                        /* close (C2) */
            return h;                                                           /* return the handle (kept) */
        } else if (tag == 0xa) {
            dos_lseek(fh, len, 1);                                       /* dos_lseek(+len, whence 1) (C2) */
        } else if (tag == 1 && len >= 3) {
            /* keep scanning */
        } else {                                                               /* stop: close + free + 0 */
            dos_close_handle(fh);                                        /* dos_close_handle (C2) */
            pool_free_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle), (uint32_t *)(uintptr_t)h);
            return 0;
        }
    }
}

/* ============================ load_savegame_file (0x22129) ============================
 * uint32_t __watcall load_savegame_file(uint32_t slot);   // EAX = slot index; ret 1 ok / 0 fail
 *
 * The .SAV slot LOADER — read counterpart of write_savegame_file. Allocates the 0xfde8 das-cache heap
 * scratch buffer, stops voices, builds the path "<dir 0x76540>\savegame\SAVE<slot>.SAV", opens it
 * (read), then runs a CHUNK LOOP: each iteration reads a 4-byte {u16 id, u16 size} header and dispatches
 * on `id` (the author's jump table 0x220f1, ids 1..14) to read that chunk's payload into its destination
 * global / restore handler. The stream ends on a short header read OR an id=12 "end" chunk (which sets
 * the success flag). On success it copies the level name (chunk id=3 -> buffer 0x701ec) into g_warp_dest_a
 * (0x8547c, stripped at the first '.'), re-activates the selected weapon, and stamps the load-pending
 * globals so game_play_loop warps to the saved level next tick. LIVE-SWAP only (DOS file I/O +
 * cross-subsystem restore, non-idempotent); every cross-subsystem callee bridged via sv_bridge/call_orig,
 * the inline chunk handlers transcribed from the disasm.
 *
 * Faithful detail: an id=1 chunk whose size<3 (corrupt header) JUMPS straight to the shared `return 0`
 * tail — it does NOT close the file or free the heap handle (the original leaks both there). Reproduced.
 * Register notes (disasm): pool_alloc_handle takes the SIZE in EDX from the leftover edx=0xfde8 of the
 * ensure call above (same hazard as bundle); dos_read_items = (eax=buf, edx=item_size, ebx=count, ecx=fh). */
uint32_t load_savegame_file(uint32_t slot)
{
    uint8_t path[0x200];     /* local_f8 [ebp-0x5e] — path + generic chunk scratch (orig 200B) */
    uint8_t name[0x200];     /* local_1c0 [ebp-0x126] — filename + player-state chunk buffer (orig 200B) */

    SVLOG("load ENTER slot=%u das_handle=0x%x\n", slot, (uint32_t)G32(VA_g_das_cache_heap_handle));
    ensure_das_cache_heap_space(0xfde8);                        /* ensure_das_cache_heap_space */
    uint32_t buf_h = pool_alloc_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle), 0xfde8);  /* pool_alloc_handle(handle, size=0xfde8) */
    uint32_t success = 0;                                              /* local_20 [ebp+0x7a] */
    if (buf_h != 0) {
        dialogue_voice_stop_all();                            /* dialogue_voice_stop_all() */
        reset_movement_velocity_queues();                     /* reset_movement_velocity_queues() */
        build_game_path((uint8_t *)(uintptr_t)path,           /* build_game_path(path, dir 0x76540, "savegame" 0x75edc) */
                  (const uint8_t *)(uintptr_t)GADDR(VA_g_dir_gdv + 0x50), (const uint8_t *)(uintptr_t)GADDR(VA_g_heap_free_list + 0x8cc));
        sv_sprintf((uint32_t)(uintptr_t)name, (uint32_t)GADDR(VA_g_heap_free_list + 0x8d5), slot);  /* sprintf(name, "SAVE%D.SAV", slot) */
        build_game_path((uint8_t *)(uintptr_t)path,           /* build_game_path(path, path, name) — append filename */
                  (const uint8_t *)(uintptr_t)path, (const uint8_t *)(uintptr_t)name);
        uint32_t fh = dos_open_file((uint32_t)(uintptr_t)path, 0);   /* dos_open_file(path, mode 0=read) (C2) */
        SVLOG("load path='%s' fh=0x%x buf_h=0x%x\n", (char *)path, fh, buf_h);

        if (fh != 0) {
            reset_player_locomotion_state();                 /* reset_player_locomotion_state() */
            reset_movement_velocity_queues();                /* reset_movement_velocity_queues() */

            uint32_t hdr = 0;                                       /* local_30 [ebp+0x6a]: {u16 id, u16 size} */
            int end = 0;                                            /* local_2c [ebp+0x6e] */
            while (!end) {
                uint32_t n = dos_read_items((uint32_t)(uintptr_t)&hdr, 4, 1, fh);  /* dos_read_items(&hdr,sz=4,cnt=1,fh) (C2) */
                if (n == 0) { end = 1; break; }                    /* short read -> end stream */
                uint32_t id   = hdr & 0xffff;
                uint32_t size = (hdr >> 16) & 0xffff;
                SVLOG("load chunk id=%u size=%u\n", id, size);
                switch (id) {
                case 1:  /* version/marker: size<3 = corrupt -> immediate return 0 (no close/free; faithful leak) */
                    if (size < 3) { SVLOG("load id1 size<3 -> bail (leak)\n"); return 0; }
                    delete_temp_files();                    /* delete_temp_files() */
                    break;
                case 2:  /* player-state chunk -> read into name, then read_player_state_chunk(name) */
                    dos_read_items((uint32_t)(uintptr_t)name, 1, size, fh);   /* (C2) */
                    read_player_state_chunk((uint32_t)(uintptr_t)name);
                    break;
                case 3:  /* level filename -> "test.raw" buffer 0x701ec */
                    dos_read_items((uint32_t)GADDR(VA_g_cfg_file_arg), 1, size, fh);   /* (C2) */
                    break;
                case 4:  /* g_cursor_list_positions 0x80afc */
                    dos_read_items((uint32_t)GADDR(VA_g_cursor_list_positions), 1, size, fh);   /* (C2) */
                    break;
                case 5:  /* g_cursor_scroll_offsets 0x80b10 */
                    dos_read_items((uint32_t)GADDR(VA_g_cursor_scroll_offsets), 1, size, fh);   /* (C2) */
                    break;
                case 6:  /* g_dbase100_record_bitmap (*0x81e28) */
                    dos_read_items((uint32_t)G32(VA_g_dbase100_record_bitmap), 1, size, fh);   /* (C2) */
                    break;
                case 7: {  /* inventory slots 0x80c30 -> recount nonzero (u16, stride 4) -> 0x80c2c -> rebuild */
                    dos_read_items((uint32_t)GADDR(VA_g_inventory_slots), 1, size, fh);   /* (C2) */
                    uint32_t cnt = 0;
                    for (uint32_t i = 0; i < 0x100; i++)
                        if (RW(GADDR(VA_g_inventory_slots) + i * 4) != 0) cnt++;
                    WD(GADDR(VA_g_inventory_count), cnt);                       /* g_inventory_count */
                    rebuild_weapon_inventory_list();        /* rebuild_weapon_inventory_list() */
                    break;
                }
                case 8:  /* discardable scratch into local_f8 */
                    dos_read_items((uint32_t)(uintptr_t)path, 1, size, fh);   /* (C2) */
                    break;
                case 9:  /* extract a level-state chunk to a temp file (the bundle's load inverse) */
                    copy_save_chunk_to_file((uint32_t)(uintptr_t)path, size, fh, RD(buf_h));  /* copy_save_chunk_to_file */
                    break;
                case 10:
                case 11: /* seek forward `size` */
                    dos_lseek(fh, size, 1);                 /* dos_lseek(fh, size, whence 1) (C2) */
                    break;
                case 12: /* end-of-stream marker -> success */
                    success = 1;
                    end = 1;
                    break;
                case 13: {  /* cutscenes-seen flags: read `size` bytes, fold into the dbase100 record array */
                    dos_read_items((uint32_t)(uintptr_t)path, 1, size, fh);   /* (C2) */
                    uint32_t base = (uint32_t)G32(VA_g_dbase100_base);        /* g_dbase100_base */
                    uint32_t rec  = base + RD(base + 0x24);        /* record array start */
                    uint32_t lim  = RD(base + 0x20);               /* record count */
                    WD(GADDR(VA_g_cutscenes_seen_count), 1);                         /* g_cutscenes_seen_count = 1 */
                    uint32_t cur = (uint32_t)(uintptr_t)path;
                    for (uint32_t k = 0; k < lim; k++) {
                        uint32_t v = RB(cur); cur++;
                        if ((uint32_t)RD(GADDR(VA_g_cutscenes_seen_count)) <= v) WD(GADDR(VA_g_cutscenes_seen_count), v + 1);
                        WB(rec + 0x13, 0);
                        WD(rec + 0x10, RD(rec + 0x10) | (v << 0x18));
                        rec += 0x14;
                    }
                    break;
                }
                case 14: {  /* dbase300 resource id -> load (audio-sequence fallback on failure) */
                    uint32_t dbid = 0;                             /* local_28 [ebp+0x72] */
                    dos_read_items((uint32_t)(uintptr_t)&dbid, 1, size, fh);   /* (C2) */
                    SVLOG("  id14: dbase300 dbid=0x%x\n", dbid);
                    if (dbid != 0) {
                        if (load_dbase300_chunk(dbid) == 0)  /* load_dbase300_chunk(dbid) */
                            process_audio_sequence_chunk();  /* process_audio_sequence_chunk() */
                    }
                    SVLOG("  id14: done\n");
                    break;
                }
                default:
                    break;                                        /* unknown id (>14 / 0): skip */
                }
            }
            SVLOG("load loop done (success=%u) -> close+free\n", success);
            dos_close_handle(fh);                           /* dos_close_handle(fh) (C2) */
        }
        pool_free_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle),
                                (uint32_t *)(uintptr_t)buf_h);     /* pool_free_handle(handle, buf) */
    }

    if (success == 0) { SVLOG("load -> 0 (no end-marker)\n"); return 0; }
    SVLOG("load SUCCESS path: name='%s' isel=0x%x\n", (char *)GADDR(VA_g_cfg_file_arg), (uint32_t)G32(VA_g_selected_item_secondary));

    /* SUCCESS: copy the level name (0x701ec, set by chunk id=3) into g_warp_dest_a (0x8547c), truncating
     * at the first '.', re-activate the selected weapon, then stamp the load-pending globals so
     * game_play_loop warps to the saved level next tick. */
    {
        uint32_t src = (uint32_t)GADDR(VA_g_cfg_file_arg);
        uint32_t dst = (uint32_t)GADDR(VA_g_warp_dest_a);
        uint8_t c;
        do {
            c = RB(src); src++;
            if (c == '.') c = 0;
            WB(dst, c); dst++;
        } while (c != 0);
        WD(GADDR(VA_g_map_first_load_flag), 0);                                     /* g_map_first_load_flag = 0 */
        WB(GADDR(VA_g_cfg_file_arg), 0);                                     /* clear the level-name buffer's first byte */
        uint32_t isel = (uint32_t)G32(VA_g_selected_item_secondary);                    /* g_selected_item_secondary */
        uint32_t ival = 0;
        if (isel != 0) ival = (uint32_t)(int32_t)(int16_t)RW(isel);/* movsx word[ptr] */
        SVLOG("  pre activate_weapon_item(0x%x,0x%x)\n", isel, ival);
        activate_weapon_item(isel, ival);                  /* activate_weapon_item(ptr, val) */
        SVLOG("  pre update_selected_item_icon\n");
        update_selected_item_icon();                       /* update_selected_item_icon() */
        SVLOG("  post restore\n");
        WD(GADDR(VA_g_active_weapon_ammo_cap + 0x8), 1);                                     /* load-pending flag */
        WD(GADDR(VA_g_choice_selected_index + 0x1c), slot);                                  /* g_pending_load_slot = slot */
    }
    SVLOG("load -> 1 (warp='%s')\n", (char *)GADDR(VA_g_warp_dest_a));
    return 1;
}

/* ============================ write_savegame_file (0x21dc6) ============================
 * uint32_t __watcall write_savegame_file(uint32_t slot);   // EAX = slot index; ret 1 (always, after writing)
 *
 * The .SAV slot WRITER — emits the chunked save format the loader reads back. Builds the player-state
 * chunk + raw-state temp files, constructs "savegame\SAVE<slot>.SAV", and (if the slot already exists)
 * confirms overwrite via the INTERACTIVE prompt_save_overwrite — so this entry is itself marked
 * interactive (lift_is_interactive 0x21dc6): the direct prompt_save_overwrite -> show_message_box
 * modal runs inside this dispatch and needs the host's interactive-lift mode (frame tick + input ring) or it
 * deadlocks. Then it captures a screen thumbnail into the das-cache scratch handle
 * (g_ui_panel_scratch_handle 0x811ac), opens for write, and emits every chunk as a {u16 id, u16 size}
 * header (dos_write_u16, which writes the edx,ebx pair) + payload (dos_write_items): id 1 (version) /
 * 0xa,0xb (thumbnail, skipped on load) / 2 (player) / 3 (level-name component, via split_path) / 0xe
 * (dbase300 id) / 7 (inventory) / 4,5 (cursor) / 6 (dbase100 bitmap) / 0xd (cutscenes-seen, high byte of
 * each record's +0x10) / then bundle_level_states appends the per-level temp files and id=0xc{size 0} is
 * the end marker. LIVE-SWAP only; cross-subsystem lifted callees are DIRECT C, DOS file I/O stays bridged.
 *
 * The original threaded several args as register LEFTOVERS (build_game_path's edx=0x76540/ebx=0x75ec8 from
 * write_player_state_chunk; pool_alloc_handle's SIZE in EDX from ensure; the end-marker dos_write_u16's
 * edx=0xc from bundle_level_states); the direct calls pass each explicitly, so no leftover dependency. */
uint32_t write_savegame_file(uint32_t slot)
{
    uint8_t path[0x200];   /* local_218 [ebp-0x4a]: save path, then split-name out, then cutscene bytes */
    uint8_t fname[0x200];  /* local_150 [ebp+0x7e]: "SAVE<slot>.SAV", split-dir out */
    uint8_t ext[0x100];    /* local_208 [ebp-0x3a]: split-ext out */
    uint8_t pbuf[0x80];    /* local_88  [ebp+0x146]: the player-state chunk */

    SVLOG("write ENTER slot=%u\n", slot);
    uint32_t psize = write_player_state_chunk((uint32_t)(uintptr_t)pbuf);  /* -> chunk size (0x30) */
    write_raw_state_temp();                                  /* write_raw_state_temp() */
    build_game_path((uint8_t *)(uintptr_t)path,              /* build_game_path(path, dir 0x76540, "savegame" 0x75ec8) */
              (const uint8_t *)(uintptr_t)GADDR(VA_g_dir_gdv + 0x50), (const uint8_t *)(uintptr_t)GADDR(VA_g_heap_free_list + 0x8b8));
    sv_bridge(0x41be5, (uint32_t)(uintptr_t)path, 0, 0, 0);          /* dos_make_directory(path) */
    sv_sprintf((uint32_t)(uintptr_t)fname, (uint32_t)GADDR(VA_g_heap_free_list + 0x8c1), slot);  /* sprintf(fname, "SAVE%D.SAV", slot) */
    build_game_path((uint8_t *)(uintptr_t)path,              /* build_game_path(path, path, fname) — append filename */
              (const uint8_t *)(uintptr_t)path, (const uint8_t *)(uintptr_t)fname);

    /* overwrite confirm if the slot file already exists */
    uint32_t fh_check = dos_open_file((uint32_t)(uintptr_t)path, 0);  /* dos_open_file(path, read) (C2) */
    if (fh_check != 0) {
        dos_close_handle(fh_check);                           /* dos_close_handle(fh_check) (C2) */
        regs_t pin; memset(&pin, 0, sizeof pin);                     /* prompt bridge passed eax=edx=ebx=ecx=0 */
        if (prompt_save_overwrite(&pin) != 1) {               /* prompt_save_overwrite() (INTERACTIVE) -> 1=overwrite */
            SVLOG("write: overwrite declined -> 0\n");
            return 0;
        }
    }

    /* capture a screen thumbnail into the das-cache scratch handle (only if not already held) */
    int snap_allocated = 0;
    if ((uint32_t)G32(VA_g_ui_panel_scratch_handle) == 0) {
        ensure_das_cache_heap_space(0x1130);                 /* ensure_das_cache_heap_space */
        snap_allocated = 1;
        uint32_t h = pool_alloc_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle), 0x1130);  /* pool_alloc_handle(handle, size=0x1130) */
        WD(GADDR(VA_g_ui_panel_scratch_handle), h);                                       /* g_ui_panel_scratch_handle = h */
        if (h != 0)
            /* capture_screen_thumbnail(dst=*h, src=framebuffer [0x90a98], lut=shade-table [0x85d08]).
             * The original resolves src from g@0x90a98 and the blend LUT via the fs selector g@0x90be2;
             * that selector's flat base IS the 256-aligned shade table stored at g@0x85d08 (set in
             * allocate_das_worker_buffers), so passing G32(0x85d08) reproduces fs:[bx] under the flat host.
             * IN-GAME VERIFY (oracle can't reach this live-swap path): thumbnail must match the original. */
            capture_screen_thumbnail(RD(h), (uint32_t)G32(VA_g_framebuffer_ptr), (uint32_t)G32(VA_g_das_remap_chunk_10000_ptr));
    }

    /* open for write + emit the chunk stream */
    uint32_t fh = dos_open_file((uint32_t)(uintptr_t)path, 1);  /* dos_open_file(path, mode 1=create/write) (C2) */
    SVLOG("write path='%s' fh=0x%x psize=0x%x\n", (char *)path, fh, psize);
    if (fh != 0) {
        sv_bridge(0x2196a, fh, 1, 3, fh);                          /* chunk 1: {id=1, size=3} version (header only) */

        sv_bridge(0x2196a, fh, 0xa, 0x30, fh);                     /* chunk 0xa header */
        dos_write_items((uint32_t)GADDR(VA_g_message_resource_handle + 0x14), 1, 0x30, fh); /*   + 0x30 bytes from 0x83d3c (C2) */

        if ((uint32_t)G32(VA_g_ui_panel_scratch_handle) != 0) {                         /* chunk 0xb: thumbnail (only if held) */
            sv_bridge(0x2196a, fh, 0xb, 0x1130, fh);
            /* DOUBLE deref: g_ui_panel_scratch_handle holds the HANDLE; *handle is the relocatable
             * thumbnail chunk buffer capture_screen_thumbnail wrote (orig: mov eax,[0x811ac]; mov eax,[eax]). */
            dos_write_items(RD((uint32_t)G32(VA_g_ui_panel_scratch_handle)), 1, 0x1130, fh);  /* (C2) */
        }

        sv_bridge(0x2196a, fh, 2, psize, fh);                      /* chunk 2: player state */
        dos_write_items((uint32_t)(uintptr_t)pbuf, 1, psize, fh);  /* (C2) */

        sv_bridge(0x2196a, fh, 3, 0xe, fh);                        /* chunk 3: level-name component (0xe bytes) */
        split_path((const char *)(uintptr_t)GADDR(VA_g_cfg_file_arg),  /* split_path(0x701ec -> dir=fname, name=path, ext=ext) */
                  (char *)(uintptr_t)fname, (char *)(uintptr_t)path, (char *)(uintptr_t)ext);
        dos_write_items((uint32_t)(uintptr_t)path, 1, 0xe, fh); /*   write the 0xe-byte name part (C2) */

        sv_bridge(0x2196a, fh, 0xe, 4, fh);                        /* chunk 0xe: dbase300 chunk id (4 bytes) */
        dos_write_items((uint32_t)GADDR(VA_g_current_dbase300_chunk_id), 1, 4, fh);  /* (C2) */

        sv_bridge(0x2196a, fh, 7, 0x400, fh);                      /* chunk 7: inventory slots (0x400 bytes) */
        dos_write_items((uint32_t)GADDR(VA_g_inventory_slots), 1, 0x400, fh);  /* (C2) */

        sv_bridge(0x2196a, fh, 4, 0x14, fh);                       /* chunk 4: cursor list positions (0x14 bytes) */
        dos_write_items((uint32_t)GADDR(VA_g_cursor_list_positions), 1, 0x14, fh);  /* (C2) */

        sv_bridge(0x2196a, fh, 5, 0x14, fh);                       /* chunk 5: cursor scroll offsets (0x14 bytes) */
        dos_write_items((uint32_t)GADDR(VA_g_cursor_scroll_offsets), 1, 0x14, fh);  /* (C2) */

        uint32_t bmsize = (uint32_t)G32(VA_g_dbase100_record_bitmap + 0x4);                  /* chunk 6: dbase100 record bitmap */
        sv_bridge(0x2196a, fh, 6, bmsize & 0xffff, fh);
        dos_write_items((uint32_t)G32(VA_g_dbase100_record_bitmap), 1, bmsize, fh);  /* (C2) */

        {   /* chunk 0xd: cutscenes-seen — high byte of each dbase100 record's +0x10, packed into `path` */
            uint32_t base = (uint32_t)G32(VA_g_dbase100_base);                /* g_dbase100_base */
            uint32_t rec  = base + RD(base + 0x24);                /* record array start */
            uint32_t cnt  = RD(base + 0x20);                       /* record count */
            uint32_t cur  = (uint32_t)(uintptr_t)path;
            for (uint32_t k = 0; k < cnt; k++) {
                WB(cur, (RD(rec + 0x10) >> 0x18) & 0xff);
                cur++;
                rec += 0x14;
            }
            sv_bridge(0x2196a, fh, 0xd, cnt & 0xffff, fh);
            dos_write_items((uint32_t)(uintptr_t)path, 1, cnt, fh);  /* (C2) */
        }

        bundle_level_states(fh);                            /* bundle_level_states(fh) — append per-level temp files */
        sv_bridge(0x2196a, fh, 0xc, 0, fh);                        /* end marker {id=0xc, size=0} (edx=0xc passed explicitly) */
        dos_close_handle(fh);                               /* dos_close_handle(fh) (C2) */
    }

    if (snap_allocated && (uint32_t)G32(VA_g_ui_panel_scratch_handle) != 0) {           /* free the thumbnail handle iff we allocated it */
        pool_free_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle),
                                (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_ui_panel_scratch_handle));  /* pool_free_handle */
        WD(GADDR(VA_g_ui_panel_scratch_handle), 0);
    }
    WD(GADDR(VA_g_choice_selected_index + 0x1c), slot);                                      /* g_pending_load_slot = slot */
    SVLOG("write DONE -> 1\n");
    return 1;
}
