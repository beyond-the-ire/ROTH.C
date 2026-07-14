/* lift_map_load.c — verified-C lifts for the `map_load` subsystem.
 *
 * map_load = the level-load/warp orchestrator + the raw-state stream codec + geometry/object-table
 * init. Bottom-up: Layer A (filename/geometry/metadata leaves, oracle) -> Layer B (stream codec +
 * file loaders) -> Layer C (warp/load entries, in-game). Canon VAs throughout (runtime = canon +
 * OBJ_DELTA). ABI is DISASM-derived (Watcom register conv); the corpus decompile is the fast path.
 *
 * STORED-POINTER globals (gotcha A4): the map geometry buffer base(s), g_object_table_header
 * (0x85c30), g_map_list_ptr (0x76710), the raw-state buffers — hold RUNTIME (DPMI/heap) addresses.
 * Read the pointer via G32(addr) then deref it RAW (never G8/G16/G32 on the pointed-to address,
 * which would add OBJ_DELTA a second time). Scratch string buffers at 0x701ec / 0x7023c live inside
 * obj3, so writes there ARE captured by the oracle's obj3 write-set.
 */
#include <stdint.h>
#include <string.h>
#include "common.h"
#include "os_api.h"      /* os_dpmi_free_descriptor (free_dpmi_selector 0x2f772 -> c2 contract) */

/* ======================================================================================
 * Layer A — leaf helpers (pure / oracle-able)
 * ====================================================================================== */

/* parse_map_das_filename 0x1078a: EAX=src token, EDX=dst. Copies src up to the first '\0' or ' '
 * into dst, null-terminates, then applies the '.DAS' extension (set_filename_extension, lifted). */
void parse_map_das_filename(char *src, char *dst)
{
    char *d = dst;
    *d = '\0';
    for (;;) {
        char c = *src;
        if (c == '\0' || c == ' ') break;
        *d++ = c;
        src++;
    }
    *d = '\0';
    set_filename_extension(dst, 0x534144);            /* 'DAS' */
}

/* lookup_map_raw_filename 0x10686: EAX=name to look up (EDX passthrough/unused output). Walks the
 * loaded map-list text (g_map_list_ptr @ 0x76710; entries whitespace-delimited, '\'-path-separated),
 * matching name case-insensitively (uppercase mask 0xdf) against the last path segment of each
 * entry's first token. On a match: copies the entry's full first token into scratch 0x701ec, applies
 * '.RAW', and returns EAX = the map-list cursor just past the matched entry name. Returns 0 if not
 * found. (Faithful goto-transcription of the disasm's tokenizer.) */
uint32_t lookup_map_raw_filename(const char *name)
{
    const uint8_t *esi = (const uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_list_ptr);   /* g_map_list_ptr */
    if (esi == 0) return 0;
    uint8_t *scratch = (uint8_t *)GADDR(VA_g_cfg_file_arg);
    const uint8_t *edi;          /* name compare cursor */
    const uint8_t *entry_start;  /* ecx */
    const uint8_t *tok;          /* ebx: last path-segment start */
    uint8_t al;

outer:
    edi = (const uint8_t *)name;
    entry_start = esi;
    tok = esi;
scan:
    al = *esi++;
    if (al <= 0x20) goto found_ws;
    if (al == 0x5c) { tok = esi; goto scan; }   /* backslash -> segment starts after it */
    goto scan;
found_ws:
    esi = tok;
cmp:
    al = *esi++;
    if (al == 0) return 0;
    if (al == 0x20) goto token_space;
    {
        uint8_t ah = *edi++;
        if ((uint8_t)(al & 0xdf) == (uint8_t)(ah & 0xdf)) goto cmp;
    }
skip1:
    al = *esi++; if (al == 0) return 0; if (al != 0x20) goto skip1;
skip2:
    al = *esi++; if (al == 0) return 0; if (al != 0x20) goto skip2;
    goto outer;
token_space:
    if (*edi == 0) goto matched;
    esi--;
    goto skip1;
matched:
    {
        const uint8_t *save = esi;
        const uint8_t *s = entry_start;
        uint8_t *d = scratch;
        while (*s > 0x20) *d++ = *s++;
        *d = 0;
        set_filename_extension((char *)scratch, 0x574152);    /* 'RAW' */
        return (uint32_t)(uintptr_t)save;
    }
}

/* select_map_entry_by_index 0x1073a: EAX=entry index. Skips 2*index whitespace-delimited fields into
 * the map-list, copies that entry's first field into scratch 0x701ec (+.RAW), and its second field
 * into scratch 0x7023c (+.DAS via parse_map_das_filename). */
void select_map_entry_by_index(uint32_t index)
{
    const uint8_t *esi = (const uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_list_ptr);
    uint8_t *scratch1 = (uint8_t *)GADDR(VA_g_cfg_file_arg);
    uint8_t *scratch2 = (uint8_t *)GADDR(VA_g_cfg_das_arg);
    uint8_t al;
    if (esi == 0) goto apply_raw;
    if (index != 0) {
        int32_t ecx = (int32_t)(index * 2u);
        for (;;) {
            al = *esi++;
            if (al == 0) goto apply_raw;
            if (al != 0x20) continue;
            if (--ecx <= 0) break;
        }
    }
    {
        uint8_t *d = scratch1;
        for (;;) {
            al = *esi++;
            if (al == 0) goto apply_raw;
            if (al == 0x20) break;
            *d++ = al;
        }
        *d = 0;
    }
    parse_map_das_filename((char *)esi, (char *)scratch2);
apply_raw:
    set_filename_extension((char *)scratch1, 0x574152);        /* 'RAW' */
}

/* flag_sectors_with_objects 0x4f221: EAX=geom (g_map_geometry_buffer), EDX=objects
 * (g_map_objects_buffer). Caches the sector-section offset (->0x91dfc) + count (->0x91df8) from the
 * geometry header (+4), then for each sector sets flag bit 0x2 at sector+0x16 when its per-sector
 * object-list word (objects[i+1]) is nonzero. Sector stride 0x1a. */
void flag_sectors_with_objects(uint8_t *geom, int16_t *objects)
{
    uint32_t off = *(uint16_t *)(geom + 4);
    G32(VA_g_sector_section_offset) = (int32_t)off;
    uint8_t *sec = geom + off;
    int32_t cnt = *(uint16_t *)(sec - 2);
    G32(VA_g_sector_count) = cnt;
    if (cnt != 0) {
        do {
            objects = objects + 1;
            if (*objects != 0)
                sec[0x16] |= 2;
            sec += 0x1a;
        } while (--cnt > 0);
    }
}

/* flag_referenced_object_textures 0x33c49: no args; ESI = g_object_table_header (0x85c30, stored
 * ptr). Two passes over the object index lists (header +0x28/+0x2a and +0x3c/+0x3e): for each
 * indexed object, if flag bit 0 at obj+6 is set, decrement it and bump the texture id (obj+8, and
 * obj+0xa in pass 2) by 0x1000; then mark the referenced-texture table byte at 0x86d31[texid*2]
 * (|=1 in pass 1, |=2 in pass 2). All 16-bit-wrapped exactly as the original. */
void flag_referenced_object_textures(void)
{
    uint8_t *tbl = (uint8_t *)GADDR(VA_g_das_entry_status_table + 0x1);
    uint8_t *esi = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_object_table_header);
    int32_t cnt = *(uint16_t *)(esi + 0x2a);
    if (cnt != 0) {
        uint8_t *edi = esi + *(uint16_t *)(esi + 0x28);
        do {
            uint32_t bx = *(uint16_t *)edi; edi += 2;
            uint32_t ax = *(uint16_t *)(esi + bx + 8);
            if (*(uint8_t *)(esi + bx + 6) & 1) {
                *(uint8_t *)(esi + bx + 6) -= 1;
                ax = (ax + 0x1000) & 0xffff;
                *(uint16_t *)(esi + bx + 8) = (uint16_t)ax;
            }
            ax &= 0xffff;
            tbl[ax * 2] |= 1;
        } while (--cnt > 0);
    }
    esi = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_object_table_header);
    cnt = *(uint16_t *)(esi + 0x3e);
    if (cnt != 0) {
        uint8_t *edi = esi + *(uint16_t *)(esi + 0x3c);
        do {
            uint32_t bx = *(uint16_t *)edi; edi += 2;
            uint32_t ax = *(uint16_t *)(esi + bx + 8);
            if (*(uint8_t *)(esi + bx + 6) & 1) {
                *(uint8_t *)(esi + bx + 6) -= 1;
                ax = (ax + 0x1000) & 0xffff;
                *(uint16_t *)(esi + bx + 8) = (uint16_t)ax;
                *(uint16_t *)(esi + bx + 0xa) += 0x1000;
            }
            ax &= 0xffff;
            tbl[ax * 2] |= 2;
        } while (--cnt > 0);
    }
}

/* init_movement_tuning_from_first_map 0x2f7bb: self-loads GS = g_geom_selector (0x90be8); off = u16
 * gs:[0xa]. Copies first-map metadata into the movement/lighting scalar globals (doubling some
 * fields, +1 on one). Its tail (gs+off +0x12/+0x10/+0x14/+0x18/+0x16) is identical to
 * init_map_lighting_from_metadata. `gs` = the selector's base (supplied by the adapter). */
void init_movement_tuning_from_first_map(uint8_t *gs)
{
    uint32_t off = *(uint16_t *)(gs + 0xa);
    G16(VA_g_player_x) = *(uint16_t *)(gs + off + 0);
    G32(VA_g_player_x + 0x2) = (int32_t)((uint32_t)*(uint16_t *)(gs + off + 2) << 16);
    G32(VA_g_view_floor_clearance) = 0;
    G16(VA_g_player_y) = *(uint16_t *)(gs + off + 4);
    G16(VA_g_player_angle) = *(uint16_t *)(gs + off + 6);
    G16(VA_g_move_speed_immediate) = *(uint16_t *)(gs + off + 8);              /* NOTE: obj1 (code-region) word */
    { uint32_t v = ((uint32_t)*(uint16_t *)(gs + off + 0xa) * 2u) & 0xffffu;
      G32(VA_g_player_height + 0x4) = (int32_t)v; G16(VA_g_player_height) = (uint16_t)v; G16(VA_g_player_height + 0x2) = (uint16_t)v; }
    G16(VA_g_max_climb) = (uint16_t)(((uint32_t)*(uint16_t *)(gs + off + 0xc) * 2u + 1u) & 0xffffu);
    G16(VA_g_min_fit) = (uint16_t)(((uint32_t)*(uint16_t *)(gs + off + 0xe) * 2u) & 0xffffu);
    /* --- tail identical to init_map_lighting_from_metadata --- */
    G16(VA_g_das_cache_slots + 0x5d8) = *(uint16_t *)(gs + off + 0x12);
    G16(VA_g_player_sector + 0x4) = *(uint16_t *)(gs + off + 0x10);
    G16(VA_g_player_sector + 0x6) = *(uint16_t *)(gs + off + 0x14);
    G16(VA_g_das_special_fat_index) = *(uint16_t *)(gs + off + 0x18);
    int32_t  edx = G32(VA_g_das_remap_chunk_100_b_ptr + 0x4);
    uint16_t ax  = G16(VA_g_gamma_level + 0x2);
    if (*(int16_t *)(gs + off + 0x16) == 0) {
        ax  = G16(VA_g_text_color_ramp_selector);
        edx = G32(VA_g_world_shading_table_ptr);
    }
    G32(VA_g_world_tint_table_ptr) = edx;
    G16(VA_g_text_color_ramp_selector + 0x2) = ax;
}

/* ======================================================================================
 * Layer A/B/C — orchestration (IN-GAME live-swap).
 *   These bridge DOS file I/O / DPMI selectors / cross-subsystem callees via call_orig; the
 *   orchestration control flow is native C. Already-lifted leaves within this subsystem are
 *   direct C calls. Deferred to the in-game debug session (they mutate world/OS state, so they
 *   are not round-trip-oracle-able — §4.5 axis 1/2).
 * ====================================================================================== */

/* read a stored pointer/handle out of a canon obj3 global; raw runtime-address field accessors. */
#define PTRG(cv) (*(volatile uint32_t *)(uintptr_t)GADDR(cv))
#define RP8(p)   (*(volatile uint8_t  *)(uintptr_t)(p))
#define RP16(p)  (*(volatile uint16_t *)(uintptr_t)(p))
#define RP32(p)  (*(volatile uint32_t *)(uintptr_t)(p))

/* (ml_bridge removed — its only callers were the free_dpmi_selector 0x2f772 sites, migrated to the
 * os_dpmi_free_descriptor contract. ml_bridge_cf / ml_call below remain for the other bridges.) */

/* bridge a callee, threading a persistent regs_t r (for CF / clobber-sensitive sequences). */
static void ml_call(regs_t *r, uint32_t canon)
{
    r->va = canon + OBJ_DELTA;
#ifndef ROTH_STANDALONE
    call_orig(r);
#else
    switch (canon) {   /* the map-warp/temp-file targets, routed to their lifted bodies */
    case 0x210ec: split_path((const char *)(uintptr_t)r->eax, (char *)(uintptr_t)r->edx,
                                    (char *)(uintptr_t)r->ebx, (char *)(uintptr_t)r->ecx);  return;
    case 0x2fbbc: set_filename_extension((char *)(uintptr_t)r->eax, r->edx);         return;
    case 0x2114e: r->eax = write_raw_state_stream(r->eax);                           return;
    case 0x41be5: {   /* dos_make_directory (EAX=path): open probe -> exists (close), else int21 AH=39 */
        uint32_t h = dos_open_file(r->eax, 0);
        if (h != 0) { dos_close_handle(h); r->eax = 0xffffffffu; return; }   /* 0x41c07 tail */
        regs_t v; memset(&v, 0, sizeof v);
        v.eax = 0x3900; v.edx = r->eax;
        g_os_soft_int(0x21, &v);
        return;
    }
    default: roth_unreachable(canon);
    }
#endif
}

/* bridge returning CF (and optionally EAX). */
static int ml_bridge_cf(uint32_t canon, uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx,
                        uint32_t *eax_out)
{
#ifndef ROTH_STANDALONE
    regs_t io; memset(&io, 0, sizeof io);
    io.va = canon + OBJ_DELTA;
    io.eax = eax; io.edx = edx; io.ebx = ebx; io.ecx = ecx;
    call_orig(&io);
    if (eax_out) *eax_out = io.eax;
    return (int)(io.eflags & 1);
#else
    (void)edx; (void)ebx; (void)ecx;
    if (canon == 0x10c13u) {                             /* load_raw_file_wrapper (lifted body) -> CF */
        uint32_t cf = load_raw_file_wrapper(eax);
        if (eax_out) *eax_out = 0;                       /* EAX unread at the sole callsite */
        return (int)(cf & 1u);
    }
    roth_unreachable(canon);
    return 1;
#endif
}

/* one inline DOS int 0x21 through the host soft-int hook: sets AX/EBX/ECX/EDX; returns EAX, CF -> *cf. */
static uint32_t ml_int21(uint32_t ax, uint32_t ebx, uint32_t ecx, uint32_t edx, int *cf)
{
    regs_t r; memset(&r, 0, sizeof r);
    r.eax = ax; r.ebx = ebx; r.ecx = ecx; r.edx = edx;
    uint32_t fl = g_os_soft_int ? g_os_soft_int(0x21, &r) : 1u;   /* NULL in oracle (unreached) */
    if (cf) *cf = (int)(fl & 1);
    return r.eax;
}

/* dos_seek_file_start 0x2f1a1 = inline int21 AX=4200 (SEEK_SET), BX=handle, CX:DX=offset -> CF.
 * Route it through g_os_soft_int (the host DOS service) exactly like das_assets' da_lseek, so the
 * whole loader uses ONE int21 path (no nested call_orig / int3 suspend mid-lift). */
static int ml_seek_start(uint32_t handle, uint32_t off)
{
    regs_t r; memset(&r, 0, sizeof r);
    r.eax = 0x4200; r.ebx = handle; r.edx = off; r.ecx = off >> 16;
    uint32_t fl = g_os_soft_int ? g_os_soft_int(0x21, &r) : 1u;
    return (int)(fl & 1);
}

/* alloc_dpmi_selector 0x2f72a: EDI=base, ECX=size -> EAX=selector, CF. Bridged (DPMI int 0x31). */
static int ml_alloc_sel(uint32_t base, uint32_t size, uint32_t *sel)
{
#ifndef ROTH_STANDALONE
    regs_t io; memset(&io, 0, sizeof io);
    io.va = 0x2f72a + OBJ_DELTA;
    io.edi = base; io.ecx = size;
    call_orig(&io);
    if (sel) *sel = io.eax;
    return (int)(io.eflags & 1);
#else
    /* alloc_dpmi_selector 0x2f72a transcription over the g_os_soft_int int31 seam (same services
     * the trap host reaches through the original's `int 0x31`): 0000 alloc cx=1 -> 0007 set-base ->
     * 0008 set-limit(size-1); free (0001) + CF on a set failure. */
    regs_t v; memset(&v, 0, sizeof v);
    v.eax = 0; v.ecx = 1;
    if (g_os_soft_int(0x31, &v) & 1u) return 1;
    uint32_t s = v.eax & 0xffffu;
    memset(&v, 0, sizeof v); v.eax = 7; v.ebx = s; v.ecx = base >> 16; v.edx = base & 0xffffu;
    if (!(g_os_soft_int(0x31, &v) & 1u)) {
        memset(&v, 0, sizeof v); v.eax = 8; v.ebx = s;
        v.ecx = (size - 1) >> 16; v.edx = (size - 1) & 0xffffu;
        if (!(g_os_soft_int(0x31, &v) & 1u)) {
            if (sel) *sel = s;
            return 0;
        }
    }
    memset(&v, 0, sizeof v); v.eax = 1; v.ebx = s;
    g_os_soft_int(0x31, &v);                       /* 0x2f765: free on set failure */
    return 1;
#endif
}

/* load_map_das_file 0x2f1b4: EAX=name. Open the map DAS, read+validate the 0x44-byte 'DASP' v5 header
 * (@0x85c58), heap-alloc the directional-object block (@0x85ce8/0x90a38), and stream the sections in
 * (directional objects, then — unless in the das-switch mode g@0x89f3f — palette + the shade/blend/
 * translation tables, then the collision section, wall section, and the extra object block). Returns
 * CF (0 ok / 1 error). DOS int21 via the soft-int hook; heap/seek/palette callees lifted or bridged. */
uint32_t load_map_das_file(uint32_t name)
{
    uint32_t hdr = (uint32_t)GADDR(VA_g_das_collision_buffer + 0x8);
    int cf;
    G16(VA_g_roth_error_code) = 9;
    uint32_t handle = ml_int21(0x3d00, 0, 0, name, &cf);     /* open read-only */
    if (cf) goto err_noheap;
    G32(VA_g_das_file_handle) = (int32_t)handle;
    uint32_t rd = ml_int21(0x3f00, handle, 0x44, hdr, &cf);  /* read header */
    if (cf || rd != 0x44) goto err_close;
    if (RP32(hdr) != 0x50534144) goto err_close;             /* 'DASP' */
    G16(VA_g_roth_error_code) = 0xe;
    if (RP16(hdr + 4) != 5) goto err_close;                  /* version 5 */
    G16(VA_g_roth_error_code) = 9;
    G16(VA_g_das_unk_0x22) = RP16(hdr + 0x22);
    uint32_t sz  = (uint32_t)RP16(hdr + 6) + (uint32_t)RP16(hdr + 0x1a) + 4;
    uint32_t buf = game_heap_alloc((int32_t)sz);
    if (buf == 0) goto err_close;
    G32(VA_g_map_das_dir_table_buffer) = (int32_t)buf;
    uint32_t adj = ((uint32_t)RP16(hdr + 0x1a) + 3) & ~3u;   /* round up to 4 (and dl,0xfc) */
    uint32_t p90a38 = buf + adj;
    G32(VA_g_map_das_fat_buffer) = (int32_t)p90a38;
    if (ml_seek_start(handle, RP32(hdr + 8))) goto err_close;
    if ((ml_int21(0x3f00, handle, RP16(hdr + 6), p90a38, &cf), cf)) goto err_close;
    if (G8(VA_g_das_skip_palette_load_flag) == 0) {                                  /* full load (not the das-switch fast path) */
        if (read_das_palette()) goto err_close;       /* 0x2f379 -> CF */
        if ((ml_int21(0x3f00, handle, 2,       (uint32_t)GADDR(VA_g_das_palette_remap_prefix), &cf), cf)) goto err_close;
        if ((ml_int21(0x3f00, handle, 0x4000,  PTRG(0x86d14),            &cf), cf)) goto err_close;
        if ((ml_int21(0x3f00, handle, 0x10000, PTRG(0x85d08),            &cf), cf)) goto err_close;
        if ((ml_int21(0x3f00, handle, 0x100,   PTRG(0x85d10),            &cf), cf)) goto err_close;
        if ((ml_int21(0x3f00, handle, 0x100,   PTRG(0x86d20),            &cf), cf)) goto err_close;
    }
    if (ml_seek_start(handle, RP32(hdr + 0x24))) goto err_close;
    if ((ml_int21(0x3f00, handle, 0x800, PTRG(0x85c50), &cf), cf)) goto err_close;
    if (ml_seek_start(handle, RP32(hdr + 0x10))) goto err_close;
    if ((ml_int21(0x3f00, handle, 0x1000, (uint32_t)GADDR(VA_g_texture_flat_color_table), &cf), cf)) goto err_close;
    if (RP32(hdr + 0x1c) != 0) {
        if (ml_seek_start(handle, RP32(hdr + 0x1c))) goto err_close;
        uint32_t cnt = RP16(hdr + 0x1a);
        rd = ml_int21(0x3f00, handle, cnt, PTRG(0x85ce8), &cf);
        if (cf || rd != cnt) goto err_close;
    }
    return 0;   /* 0x2f355: clc; ret — the original LEAVES THE FILE OPEN on success (handle g@0x85d00
                 * stays live) so the DAS cache lazily streams map textures/blocks from it for the rest of
                 * the level; it's closed by the NEXT map's close_das_file_handle / teardown, not here.
                 * (An erroneous success-path close blackened the world: identical memory, dead fd.) */

err_close:                                                   /* 0x2f357: close handle + free block */
    {
        uint32_t h = (uint32_t)G32(VA_g_das_file_handle);
        G32(VA_g_das_file_handle) = 0;
        ml_int21(0x3e00, h, 0, 0, NULL);
    }
err_noheap:                                                  /* 0x2f363: free the object block */
    {
        uint32_t old = (uint32_t)G32(VA_g_map_das_dir_table_buffer);
        G32(VA_g_map_das_dir_table_buffer) = 0;
        game_free_if_not_null((uint8_t *)(uintptr_t)old);   /* re-point 0x40a2a */
    }
    return 1;                                                /* stc */
}

/* load_raw_map_file 0x2f4b4: EAX=name. Reset pickup/door state, open + read the 0x1c-byte raw-map
 * header (@0x71f0e), validate ('WR' @+0xe, 0x70 @+2), heap-alloc the whole geometry allocation, copy
 * the header, DPMI-map the 4 geometry sections (sectors/walls/objects/extra) via alloc_dpmi_selector
 * (selectors -> 0x90be8/0x90bec/0x90bea/0x90bc4; bases -> 0x90aa8/0x90aac/0x85c30/0x90aa4/0x90aa0),
 * stream each section in, fix up sectors, and close. Returns CF (0 ok / 1 err). ES-segment section
 * writes go to the heap allocation (flat); DOS int21 via the soft-int hook; DPMI/pool bridged. */
uint32_t load_raw_map_file(uint32_t name)
{
    reset_item_pickup_lock();                         /* re-point 0x18003 */
    G32(VA_g_screen_backup_handle + 0x8) = (int32_t)0x80808080;
    G32(VA_g_reflection_view_count) = 0;
    G8(VA_g_render_active)  = 0;
    G32(VA_g_render_target_secondary_size + 0x4) = 0;
    init_door_pool();                                 /* re-point 0x3d433 */
    uint32_t esi = (uint32_t)GADDR(VA_g_init_stage_error_strings + 0x13e);                 /* raw-map header buffer */
    G16(VA_g_roth_error_code) = 4;
    G16(VA_g_player_sector) = 0;
    int cf;
    uint32_t handle = ml_int21(0x3d00, 0, 0, name, &cf);     /* open */
    if (cf) goto err;
    G32(VA_g_das_file_handle + 0x4) = (int32_t)handle;
    if ((ml_int21(0x3f00, handle, 0x1c, esi, &cf), cf)) goto err;   /* read 0x1c header */
    if (RP16(esi + 0xe) != 0x5257) goto errclose;            /* 'WR' */
    G16(VA_g_roth_error_code) = 0xd;
    if (RP16(esi + 2) != 0x70) goto errclose;
    G32(VA_g_render_target_secondary_size + 0x4) = RP16(esi + 0x1a);
    uint32_t total = (uint32_t)RP16(esi) + RP16(esi + 0x14) + RP16(esi + 0x12) + (uint32_t)G32(VA_g_render_target_secondary_size + 0x4);
    uint32_t base  = (uint32_t)G32(VA_g_object_table_header + 0x4);
    uint32_t colend = RP16(esi + 0x16) + base;
    G32(VA_g_object_buffer_free) = (int32_t)base;
    G32(VA_g_object_buffer_free + 0x4) = (int32_t)colend;
    total += colend;
    if (G16(VA_g_vel_queue_b + 0x84) != 0) total += RP16(esi + 0x18);
    uint32_t alloc = game_heap_alloc((int32_t)total);
    if (alloc == 0) goto errclose;
    G32(VA_g_sfx_nodes + 0x8) = (int32_t)alloc;
    memcpy((void *)(uintptr_t)alloc, (const void *)(uintptr_t)esi, 0x1c);   /* copy header */
    uint32_t edi = alloc;
    esi = edi;                                               /* esi now walks the allocation */
    /* section 0: sectors */
    uint32_t ecx = RP16((uintptr_t)GADDR(VA_g_init_stage_error_strings + 0x13e));          /* word[0x71f0e] = sector section size */
    uint32_t ax;
    if (ml_alloc_sel(edi, ecx, &ax)) goto errclose;   /* alloc_dpmi_selector(edi,ecx) */
    G32(VA_g_map_geometry_buffer) = (int32_t)edi;  G16(VA_g_geometry_selector) = (uint16_t)ax;
    edi += ecx;
    /* section 1: walls */
    ecx = RP16((uintptr_t)GADDR(VA_g_init_stage_error_strings + 0x152));
    if (ml_alloc_sel(edi, ecx, &ax)) goto errclose;
    G16(VA_g_geometry_selector + 0x4) = (uint16_t)ax;  G32(VA_g_sector_geom_base) = (int32_t)edi;
    edi += ecx;
    /* object-table header (section C): g_object_table_header = edi if size!=0 */
    ecx = (uint32_t)G32(VA_g_render_target_secondary_size + 0x4);
    G32(VA_g_object_table_header) = (int32_t)ecx;
    if (ecx != 0) G32(VA_g_object_table_header) = (int32_t)edi;
    edi += ecx;
    ecx = RP16((uintptr_t)GADDR(VA_g_init_stage_error_strings + 0x150));
    G32(VA_g_sfx_nodes) = (int32_t)edi;
    G32(VA_g_sfx_nodes + 0x4) = 0;
    edi += ecx;
    /* stream the first block: read (edi - esi - 0x1c) bytes into esi+0x1c */
    ecx = edi - esi - 0x1c;
    esi += 0x1c;
    if ((ml_int21(0x3f00, (uint32_t)G32(VA_g_das_file_handle + 0x4), ecx, esi, &cf), cf)) goto errclose;
    esi += ecx;
    /* g_object_table_header[6] link fixup: [0x85c48] = *(u16)[0x85c44] + [0x85c44] */
    {
        uint32_t b = (uint32_t)G32(VA_g_sfx_nodes);
        G32(VA_g_sfx_nodes + 0x4) = (int32_t)((uint32_t)RP16(b) + b);
    }
    /* section: extra (0x71f24) */
    ecx = RP16((uintptr_t)GADDR(VA_g_init_stage_error_strings + 0x154));
    if ((ml_int21(0x3f00, (uint32_t)G32(VA_g_das_file_handle + 0x4), ecx, edi, &cf), cf)) goto errclose;
    ecx += (uint32_t)G32(VA_g_object_table_header + 0x4);
    if (ml_alloc_sel(edi, ecx, &ax)) goto errclose;
    G32(VA_g_map_objects_buffer) = (int32_t)edi;  G16(VA_g_geometry_selector + 0x2) = (uint16_t)ax;
    edi += ecx;
    if (G16(VA_g_vel_queue_b + 0x84) != 0) {
        ecx = RP16((uintptr_t)GADDR(VA_g_init_stage_error_strings + 0x156));
        if ((ml_int21(0x3f00, (uint32_t)G32(VA_g_das_file_handle + 0x4), ecx, edi, &cf), cf)) goto errclose;
        if (ml_alloc_sel(edi, ecx, &ax)) goto errclose;
        G32(VA_g_image_surface + 0x4) = (int32_t)edi;  G16(VA_g_vel_queue_b + 0x82) = (uint16_t)ax;
        G16(VA_g_vel_queue_b + 0x86) = RP16(edi + 6);
        edi += ecx;
    }
    /* fixup_raw_sectors_after_load self-loads FS=g_geom_selector 0x90be8, base = g_map_geometry_buffer
     * 0x90aa8 (the sector section = `alloc`) — NOT the 0x1c header scratch. Pass the geometry base. */
    fixup_raw_sectors_after_load((uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer)); /* 0x2f782 */
    ml_int21(0x3e00, (uint32_t)G32(VA_g_das_file_handle + 0x4), 0, 0, NULL);   /* close */
    return 0;                                               /* ax=0x4f4b; clc */

errclose:
    ml_int21(0x3e00, (uint32_t)G32(VA_g_das_file_handle + 0x4), 0, 0, NULL);
err:
    return 1;                                               /* ax=0; stc */
}

/* ---- warp/relocate flow_succ pair: process_map_warp_or_load 0x1096f + relocate_player_to_warp_sector
 * 0x10a4d share the sector-search + player-position core (0x10a18..0x10b23) and the reload tail. The
 * map geometry is reached FLAT: fs=g_geom_selector 0x90be8 has base g_map_geometry_buffer 0x90aa8;
 * gs=0x90bec has base 0x90aac (walls) — so fs:[x] == *(0x90aa8+x), gs:[x] == *(0x90aac+x). ---- */

/* found-sector handler (0x10a62..0x10b23): centre the player in sector at offset `edi` from the
 * sector vertex bounds and stash the sector globals. */
static void ml_relocate_found(uint32_t fsbase, uint32_t gsbase, uint32_t edi)
{
    int32_t vcount = (uint8_t)RP8(fsbase + edi + 0xd);
    int16_t minx = 0x7fff, miny = 0x7fff, maxx = (int16_t)0x8000, maxy = (int16_t)0x8000;
    uint16_t adj = (uint16_t)(G16(VA_g_player_movement_enabled + 0xa) - RP16(fsbase + edi + 2));
    G16(VA_g_player_movement_enabled + 0xa) = adj;
    G16(VA_g_player_z) = (uint16_t)(G16(VA_g_player_z) - adj);
    G16(VA_g_player_sector) = (uint16_t)edi;
    G32(VA_g_state_link_buf_ptr) = (int32_t)((uint16_t)edi + (uint32_t)G32(VA_g_map_geometry_buffer));
    G16(VA_g_player_sector_cache) = (uint16_t)edi;
    uint32_t vlist = RP16(fsbase + edi + 0xe);
    do {
        uint32_t si = RP16(fsbase + vlist);
        int16_t vx = (int16_t)RP16(gsbase + si + 8);
        if (!(vx > minx)) minx = vx;             /* dx = min */
        if (!(vx < maxx)) maxx = vx;             /* bp = max */
        int16_t vy = (int16_t)RP16(gsbase + si + 0xa);
        if (!(vy > miny)) miny = vy;
        if (!(vy < maxy)) maxy = vy;
        vlist += 0xc;
    } while (--vcount > 0);
    int32_t cx = ((int32_t)minx + (int32_t)maxx) >> 1;
    int32_t cy = ((int32_t)miny + (int32_t)maxy) >> 1;
    G32(VA_g_player_z + 0x2) = cy << 16;
    G32(VA_g_player_angle + 0x2) = cx << 16;
}

/* sector-search core (0x10a18..): find the sector whose [+0x14] == `target` (id != `si`); relocate. */
static void ml_relocate_core(uint32_t target, uint32_t si)
{
    uint32_t fsbase = (uint32_t)G32(VA_g_map_geometry_buffer);
    uint32_t gsbase = (uint32_t)G32(VA_g_sector_geom_base);
    uint32_t edi = RP16(fsbase + 4);
    int32_t  cx  = (uint16_t)RP16(fsbase + edi - 2);
    for (;;) {
        if ((uint16_t)edi != (uint16_t)si &&
            (uint16_t)target == RP16(fsbase + edi + 0x14)) {
            ml_relocate_found(fsbase, gsbase, edi);
            return;
        }
        edi += 0x1a;
        if (--cx <= 0) return;
    }
}

/* relocate_player_to_warp_sector 0x10a4d: search the geometry for the warp-target sector
 * (g@0x85484), centre the player there, and clear the warp request. Returns CF=0. */
uint32_t relocate_player_to_warp_sector(void)
{
    ml_relocate_core((uint16_t)(uint32_t)G32(VA_g_map_first_load_flag), 0);
    G32(VA_g_map_first_load_flag) = 0;
    G8(VA_g_warp_dest_a)  = 0;
    return 0;
}

/* process_map_warp_or_load 0x1096f: the top-level warp/level-change entry. Compares the requested
 * map name (scratch 0x701ec) with the currently-loaded one; SAME map -> relocate the player to the
 * warp-target sector in place; DIFFERENT map -> save current raw-state, swap DAS, unload old geometry,
 * reset pools, load the new raw map, restore raw-state, re-init player/lighting/render, relocate.
 * Returns CF (0 ok / 1 load error). Reached FLAT; DOS/DPMI/cross-subsystem callees bridged or lifted. */
uint32_t process_map_warp_or_load(void)
{
    const uint8_t *esi, *edi;
    int32_t ecx;
    if (G32(VA_g_map_first_load_flag) == -1) {                        /* first load */
        esi = (const uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_dir_gdv + 0x140);
        G32(VA_g_dir_gdv + 0x140) = 0;
        edi = (const uint8_t *)GADDR(VA_g_cfg_file_arg);
        ecx = 0x40;
    } else {
        esi = (const uint8_t *)GADDR(VA_g_warp_dest_a);
        const uint8_t *p = (const uint8_t *)GADDR(VA_g_cfg_file_arg);
        const uint8_t *seg = p;                       /* strip 0x701ec to last '\'-segment */
        for (;;) {
            uint8_t c = *p++;
            if (c == 0) break;
            if (c == 0x5c) seg = p;
        }
        edi = seg;
        ecx = 8;
    }

    /* ---- name compare (0x109c3): esi=current, edi=new; case-insensitive, '.'=extension boundary ---- */
    const uint8_t *esi0 = esi, *edi0 = edi;           /* saved (push esi/edi) for the reload path */
    int same = 1;
    if (esi != 0 && *esi != 0) {
        uint8_t al = 0;
        for (;;) {
            al = *esi;
            uint8_t ah = *edi;
            if (ah == 0x2e) break;                    /* '.' -> check al */
            if ((uint8_t)(al & 0xdf) != (uint8_t)(ah & 0xdf)) { same = 0; break; }
            esi++; edi++;
            if (--ecx <= 0) { al = 0; break; }
        }
        if (same && al != 0) same = 0;
    }

    if (same) {
        /* ---- SAME map: relocate in place (0x109ee) ---- */
        uint32_t fsbase = (uint32_t)G32(VA_g_map_geometry_buffer);
        if (G32(VA_g_map_first_load_flag) != 0) {
            uint32_t s = (uint16_t)G16(VA_g_player_sector);
            G16(VA_g_player_movement_enabled + 0xa) = RP16(fsbase + s + 2);
            ml_relocate_core((uint16_t)(uint32_t)G32(VA_g_map_first_load_flag), s);
        }
        G32(VA_g_map_first_load_flag) = 0;
        G8(VA_g_warp_dest_a)  = 0;
        return 0;                                     /* clc */
    }

    /* ---- DIFFERENT map: full reload (0x10b28) ---- */
    write_raw_state_temp();                    /* 0x21879 */
    {
        uint32_t fsb = (uint32_t)G32(VA_g_map_geometry_buffer);
        uint32_t s = (uint16_t)G16(VA_g_player_sector);
        if (s != 0) G16(VA_g_player_movement_enabled + 0xa) = RP16(fsb + s + 2);
    }
    ecx = (G32(VA_g_map_first_load_flag) == -1) ? 0x78 : 8;
    {                                                 /* build "<name>.RAW" into 0x701ec (edi0) */
        const uint8_t *s = esi0;
        uint8_t *d = (uint8_t *)(uintptr_t)edi0;
        for (;;) {
            uint8_t c = *s++;
            if (c <= 0x20) break;
            *d++ = c;
            if (--ecx <= 0) break;
        }
        *(uint32_t *)d = 0x5741522e;                  /* ".RAW" */
        d[4] = 0;
    }
    switch_map_das_resources((uint32_t)GADDR(VA_g_cfg_file_arg), (uint32_t)GADDR(VA_g_cfg_das_arg));  /* 0x105c0 */
    if (G32(VA_g_map_das_dir_table_buffer) == 0) goto err;                  /* je 0x10bfb (DAS load failed) */
    unload_map_geometry();                     /* 0x2f459 */
    reset_entity_pools();                      /* re-point 0x4263e */
    if (ml_bridge_cf(0x10c13, (uint32_t)GADDR(VA_g_cfg_file_arg), 0, 0, 0, NULL)) goto err;  /* load_raw_file_wrapper */
    {
        uint32_t handle = open_raw_state_temp();   /* 0x218de */
        set_state_record_count(handle);        /* re-point 0x33c3e — original 0x10bb2 threads EAX=handle
                                                * (the .TMP handle from 0x218de, pushed): a NONZERO handle
                                                * latches g@0x89f5c so the first-visit object-INIT handlers
                                                * (0x30998 walk) SUPPRESS re-imposing the authored light/
                                                * door state on a revisit, leaving the restored .TMP delta
                                                * intact. Passing 0 made every reload a first visit ->
                                                * sector state reverted. The other two callers (0x10297 /
                                                * 0x110fa) DO pass 0 (new game / main-seq first load). */
        if (G32(VA_g_map_first_load_flag) == -1) {
            G32(VA_g_map_first_load_flag) = 0;
            init_player_position_from_metadata((uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer));  /* 0x2f8a2 */
        }
        init_map_lighting_from_metadata((uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer));         /* 0x2f8fa */
        if (load_raw_state_from_temp(handle) != 0) goto err;   /* 0x21934 */
    }
    mark_geom_sentinel_entries((uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer));      /* 0x2ec1a */
    if (G32(VA_g_map_first_load_flag) != 0) relocate_player_to_warp_sector();                   /* 0x10a4d */
    init_render_struct_89ed0();                /* re-point 0x2f962 */
    G32(VA_g_map_first_load_flag) = 0;
    G8(VA_g_warp_dest_a)  = 0;
    return 0;                                         /* clc (0x10a3e) */

err:
    G32(VA_g_map_first_load_flag) = 0;
    G8(VA_g_warp_dest_a)  = 0;
    return 0xffffffffu;                               /* EAX=-1 (0x10bff); callers `test eax,eax` */
}

/* ---- raw-state stream codec (write/read_raw_state_stream) — the inter-map persistence of mutable
 * world state (sector deltas, object patches, record lists, dynamic entities, sfx). Chunk format:
 * each length-prefixed chunk assembled in a scratch pool buffer then dos_write_items'd; terminated by
 * the "EXIT"/0x45584954 tag. Object-patch entries pack type-specific byte fields keyed by rec[3]. The
 * DOS wrappers (dos_open/read/write/close_items) + the delta codec + savegame record writers are
 * bridged; strip_transient_flags_for_save is lifted. IN-GAME (non-idempotent DOS I/O + world state). */
/* re-point (C2): the DOS-file helpers call the native wrappers (lift_dpmi_dos_os.c)
 * directly. Only the helper BODIES change — every call site (and thus the open/read/write/close
 * SEQUENCE, incl. the .DAS fd staying open across the load) is byte-for-byte preserved. */
static uint32_t ml_dos_open(uint32_t path, uint32_t mode) { return dos_open_file(path, mode); }
static void     ml_dos_close(uint32_t h)                  { dos_close_handle(h); }
static uint32_t ml_dos_read(uint32_t buf, uint32_t isz, uint32_t n, uint32_t h)  { return dos_read_items(buf, isz, n, h); }
static void     ml_dos_write(uint32_t buf, uint32_t isz, uint32_t n, uint32_t h) { dos_write_items(buf, isz, n, h); }

/* write_raw_state_stream 0x2114e: EAX = output (.TMP) file path. Serializes the current world state. */
uint32_t write_raw_state_stream(uint32_t path)
{
    uint8_t gamepath[0x108];
    uint32_t pool = (uint32_t)G32(VA_g_das_cache_heap_handle);
    ensure_das_cache_heap_space(0x10000);
    uint32_t h1 = pool_alloc_handle((uint32_t *)(uintptr_t)pool, 0x10000);
    if (h1 == 0) return 0;
    ensure_das_cache_heap_space(0x19000);
    uint32_t h2 = pool_alloc_handle((uint32_t *)(uintptr_t)pool, 0x19000);
    if (h2 == 0) { pool_free_handle((uint32_t *)(uintptr_t)pool, (uint32_t *)(uintptr_t)h1); return 0; }  /* re-point 0x360b3: free h1 */

    uint32_t p2 = RP32(h2);                                          /* pool2 data (chunk buffer) */
    build_game_path(gamepath, (const uint8_t *)(uintptr_t)GADDR(VA_g_dir_digi + 0x50),
                           (const uint8_t *)(uintptr_t)GADDR(VA_g_cfg_file_arg));  /* re-point 0x2fb7f: build_game_path.
                            * dir = 0x764a0 (VA_g_dir_digi+0x50, the LEVEL-DATA source dir) per the original
                            * 0x211aa `mov edx,0x764a0` — the same prefix load_raw_file_wrapper 0x10c13 uses.
                            * This opens the pristine .RAW as the sector-delta ENCODE REFERENCE; the old
                            * VA_g_dir_gdv+0x50 (0x76540, the save/.TMP dir — copy-paste from 0x21879/0x218de,
                            * which legitimately use it) only worked when both dirs resolve the same file. */
    uint32_t objbuf = (uint32_t)G32(VA_g_map_geometry_buffer);
    uint32_t objcount = RP16(objbuf);
    reserve_object_buffer_space();                            /* re-point 0x420e1 (reads globals; ignored EAX=objcount) */

    uint32_t chunk1_ptr = objbuf, chunk1_len = objcount;             /* fallback if the delta open fails */
    uint32_t gh = ml_dos_open((uint32_t)(uintptr_t)gamepath, 0);     /* open original raw map (for delta) */
    if (gh != 0) {
        uint32_t d1 = RP32(h1);
        memcpy((void *)(uintptr_t)d1, (const void *)(uintptr_t)objbuf, objcount);  /* repne movsb */
        strip_transient_flags_for_save(RP32(h1));             /* 0x31e14 */
        uint32_t orig = p2 + 0x100;
        RP16(p2) = 0xffff;
        uint32_t rd = ml_dos_read(orig, 1, objcount, gh);            /* read original sectors */
        (void)rd;
        ml_dos_close(gh);
        /* re-point 0x4ee1f (out=eax, ref=edx, newb=ebx, len=ecx). The old
         * bridge passed ecx=0 (len=0 -> degenerate delta, revisited maps would lose sector state);
         * the ORIGINAL 0x2114e loads ecx = the objcount local ([ebp-0x10], the same count feeding
         * the memcpy + dos_read above; preserved across dos_close) into encode's loop bound
         * (dec ecx). NEEDS IN-GAME VERIFY: map transition + revisit round-trip under
         * ROTH_LIFT=map_load. */
        uint32_t enc = encode_literal_skip_delta_stream(
            (uint8_t *)(uintptr_t)(p2 + 4), (const uint8_t *)(uintptr_t)orig,
            (const uint8_t *)(uintptr_t)RP32(h1), (int)objcount);
        uint32_t clen = (enc + 1) & ~1u;                             /* round to even (inc; and 0xfe) */
        RP16(p2 + 2) = (uint16_t)clen;
        chunk1_ptr = p2;
        chunk1_len = clen + 4;
    }

    uint32_t fh = ml_dos_open(path, 1);                              /* open output for write */
    if (fh == 0) goto free_pools;
    ml_dos_write(chunk1_ptr, 1, chunk1_len, fh);                     /* sector-delta chunk */
    {                                                                /* secondary buffer chunk */
        uint32_t sb = (uint32_t)G32(VA_g_map_objects_buffer);
        uint32_t n = ((uint32_t)RP16(sb) + 1) & ~1u;
        ml_dos_write(sb, 1, n, fh);
    }
    if (G32(VA_g_object_table_header) != 0) {
        /* ---- object-patch chunk: pack type-keyed byte fields per object into p2 ---- */
        int32_t op_count = (int16_t)RP16((uint32_t)G32(VA_g_object_table_header) + 6);
        uint32_t op_src  = RP32((uint32_t)G32(VA_g_object_ptr_array));
        uint32_t out = p2;
        int32_t clen = op_count;                                     /* [ebp-0x30] seeds at op_count */
        for (int32_t i = 0; i < op_count; i++) {
            uint32_t rec = RP32(op_src); op_src += 4;
            uint8_t type = RP8(rec + 3);
            switch (type) {
            case 0x02:
                RP8(out) = RP8(rec + 6); RP8(out + 1) = RP8(rec + 0xc); clen += 2; out += 2; break;
            case 0x0a: case 0x0b:
                RP8(out) = RP8(rec + 7); RP8(out + 1) = RP8(rec + 0xa); RP8(out + 2) = RP8(rec + 0xb);
                RP8(out + 3) = RP8(rec + 0xc); RP8(out + 4) = RP8(rec + 0xd); clen += 5; out += 5; break;
            case 0x0d:
                RP8(out) = RP8(rec + 7); RP8(out + 1) = RP8(rec + 0xc); RP8(out + 2) = RP8(rec + 0xd);
                clen += 3; out += 3; break;
            case 0x15: case 0x1f: case 0x21: case 0x22:
                RP8(out) = RP8(rec + 0xa); RP8(out + 1) = RP8(rec + 0xb); clen += 2; out += 2; break;
            case 0x34:
                RP8(out) = RP8(rec + 6); RP8(out + 1) = RP8(rec + 0xa); RP8(out + 2) = RP8(rec + 0xb);
                clen += 3; out += 3; break;
            default: break;
            }
            RP8(out) = RP8(rec + 2); out += 1;                       /* common tail: rec[2] */
        }
        uint32_t rounded = ((uint32_t)clen + 3) & ~3u;
        uint32_t lenhdr = rounded;
        ml_dos_write((uint32_t)(uintptr_t)&lenhdr, 1, 4, fh);        /* length header */
        if (rounded != 0) ml_dos_write(p2, 1, rounded, fh);         /* chunk data */
    }
    {                                                                /* state record list */
        uint32_t len = write_state_record_list(p2);          /* re-point 0x35648 (edx/ebx unused) */
        uint32_t lenhdr = len;
        ml_dos_write((uint32_t)(uintptr_t)&lenhdr, 1, 4, fh);
        if (len != 0) ml_dos_write(p2, 1, len, fh);
    }
    {                                                                /* dynamic entities */
        uint32_t len = write_state_dynamic_entities((void *)(uintptr_t)p2);  /* re-point 0x4eee0 (edx unused) */
        ml_dos_write(p2, 1, len, fh);
    }
    {                                                                /* sfx node active state */
        uint32_t len = save_sfx_node_active_state((uint32_t *)(uintptr_t)(p2 + 4)); /* re-point 0x43d53 (edx unused) */
        RP32(p2) = len;
        ml_dos_write(p2, 1, len + 4, fh);
    }
    RP32(p2) = 0x45584954;                                          /* "EXIT" tag */
    ml_dos_write(p2, 1, 4, fh);
    ml_dos_close(fh);

free_pools:
    pool_free_handle((uint32_t *)(uintptr_t)pool, (uint32_t *)(uintptr_t)h2);  /* re-point 0x360b3: free h2 */
    pool_free_handle((uint32_t *)(uintptr_t)pool, (uint32_t *)(uintptr_t)h1);  /* re-point 0x360b3: free h1 */
    return 0;
}

/* read_raw_state_stream 0x214b9: EAX = input file handle. De-serializes the world state written by
 * write_raw_state_stream. Returns 0 on the "EXIT" terminator (inverted success), 1 on a missing tag. */
uint32_t read_raw_state_stream(uint32_t handle)
{
    uint32_t pool = (uint32_t)G32(VA_g_das_cache_heap_handle);
    uint32_t result = 0;
    ensure_das_cache_heap_space(0x19000);
    uint32_t h = pool_alloc_handle((uint32_t *)(uintptr_t)pool, 0x19000);
    if (h == 0) return 0;
    uint32_t edi = RP32(h);                              /* scratch read buffer */
    uint32_t objbuf = (uint32_t)G32(VA_g_map_geometry_buffer);

    uint8_t hdr[4];
    ml_dos_read((uint32_t)(uintptr_t)hdr, 1, 4, handle); /* sector chunk 4-byte header */
    uint16_t w0 = (uint16_t)(hdr[0] | (hdr[1] << 8));
    uint16_t w1 = (uint16_t)(hdr[2] | (hdr[3] << 8));
    if (w0 == 0xffff) {                                  /* delta chunk */
        ml_dos_read(edi, 1, w1, handle);
        apply_literal_skip_delta_stream((uint8_t *)(uintptr_t)objbuf,
                                               (const uint8_t *)(uintptr_t)edi);  /* re-point 0x4eeae */
    } else {                                            /* literal sector buffer */
        RP16(objbuf) = w0;
        RP16(objbuf + 2) = w1;
        ml_dos_read(objbuf + 4, 1, (uint32_t)w0 - 4, handle);
    }
    {                                                   /* secondary buffer */
        uint32_t sb = (uint32_t)G32(VA_g_map_objects_buffer);
        ml_dos_read(sb, 1, 2, handle);                  /* 2-byte count */
        uint32_t n = ((uint32_t)RP16(sb) - 1) & ~1u;
        ml_dos_read(sb + 2, 1, n, handle);
    }
    rebuild_object_pointer_array();              /* 0x33ea1 */

    {                                                   /* object-patch chunk */
        uint8_t lb[4];
        ml_dos_read((uint32_t)(uintptr_t)lb, 1, 4, handle);
        uint32_t clen = (uint32_t)lb[0] | (lb[1] << 8) | (lb[2] << 16) | ((uint32_t)lb[3] << 24);
        if (G32(VA_g_object_table_header) != 0 && clen != 0) {
            int32_t op_count = (int16_t)RP16((uint32_t)G32(VA_g_object_table_header) + 6);
            uint32_t op_src  = RP32((uint32_t)G32(VA_g_object_ptr_array));
            ml_dos_read(edi, 1, clen, handle);          /* read the packed chunk */
            uint32_t in = edi;
            for (int32_t i = 0; i < op_count; i++) {
                uint32_t rec = RP32(op_src); op_src += 4;
                uint8_t type = RP8(rec + 3);
                switch (type) {
                case 0x02:
                    RP8(rec + 6) = RP8(in); RP8(rec + 0xc) = RP8(in + 1); in += 2; break;
                case 0x0a: case 0x0b:
                    RP8(rec + 7) = RP8(in); RP8(rec + 0xa) = RP8(in + 1); RP8(rec + 0xb) = RP8(in + 2);
                    RP8(rec + 0xc) = RP8(in + 3); RP8(rec + 0xd) = RP8(in + 4); in += 5; break;
                case 0x0d:
                    RP8(rec + 7) = RP8(in); RP8(rec + 0xc) = RP8(in + 1); RP8(rec + 0xd) = RP8(in + 2);
                    in += 3; break;
                case 0x15: case 0x1f: case 0x21: case 0x22:
                    RP8(rec + 0xa) = RP8(in); RP8(rec + 0xb) = RP8(in + 1); in += 2; break;
                case 0x34:
                    RP8(rec + 6) = RP8(in); RP8(rec + 0xa) = RP8(in + 1); RP8(rec + 0xb) = RP8(in + 2);
                    in += 3; break;
                default: break;
                }
                RP8(rec + 2) = RP8(in); in += 1;        /* common tail */
            }
        }
    }
    {                                                   /* state record list */
        uint8_t lb[4]; ml_dos_read((uint32_t)(uintptr_t)lb, 1, 4, handle);
        uint32_t len = (uint32_t)lb[0] | (lb[1] << 8) | (lb[2] << 16) | ((uint32_t)lb[3] << 24);
        if (len != 0) {
            ml_dos_read(edi, 1, len, handle);
            load_state_record_list(edi);         /* re-point 0x3580c (Layer C live-swap) */
        }
    }
    {                                                   /* dynamic entities */
        uint8_t lb[4]; ml_dos_read((uint32_t)(uintptr_t)lb, 1, 4, handle);
        int32_t len = (int32_t)((uint32_t)lb[0] | (lb[1] << 8) | (lb[2] << 16) | ((uint32_t)lb[3] << 24));
        len -= 4;
        if (len > 0) {
            ml_dos_read(edi, 1, (uint32_t)len, handle);
            load_state_dynamic_entities(edi);        /* re-point 0x4ef61 (edx=len is an ignored input) */
        }
    }
    {                                                   /* sfx node active state */
        uint8_t lb[4]; ml_dos_read((uint32_t)(uintptr_t)lb, 1, 4, handle);
        uint32_t len = (uint32_t)lb[0] | (lb[1] << 8) | (lb[2] << 16) | ((uint32_t)lb[3] << 24);
        if (len != 0) {
            ml_dos_read(edi, 1, len, handle);
            load_sfx_node_active_state(edi);     /* re-point 0x43d98 */
        }
    }
    {                                                   /* EXIT tag */
        uint8_t lb[4]; ml_dos_read((uint32_t)(uintptr_t)lb, 1, 4, handle);
        uint32_t tag = (uint32_t)lb[0] | (lb[1] << 8) | (lb[2] << 16) | ((uint32_t)lb[3] << 24);
        if (tag != 0x45584954) result = 1;
    }
    pool_free_handle((uint32_t *)(uintptr_t)pool, (uint32_t *)(uintptr_t)h);  /* re-point 0x360b3 */
    return result;
}

/* init_loaded_map_state 0x2f6e6: if g_map_geometry_buffer (0x90aa8)!=0, and g_map_objects_buffer
 * (0x90aa4)!=0, reset per-sector object state; then (re)build the sfx nodes. Both callees lifted. */
void init_loaded_map_state(void)
{
    uint32_t geom = (uint32_t)G32(VA_g_map_geometry_buffer);
    if (geom == 0) return;
    uint32_t objs = (uint32_t)G32(VA_g_map_objects_buffer);
    if (objs != 0) init_sector_object_state(geom, objs);   /* 0x4f1a0 (EAX,EDX) */
    setup_sfx_nodes();                                      /* 0x43c46 */
}

/* release_raw_state_and_setup_sfx 0x2f708: same guard shape, but flags sectors that have objects
 * (instead of the per-object reset) before rebuilding the sfx nodes. */
void release_raw_state_and_setup_sfx(void)
{
    uint32_t geom = (uint32_t)G32(VA_g_map_geometry_buffer);
    if (geom == 0) return;
    uint32_t objs = (uint32_t)G32(VA_g_map_objects_buffer);
    if (objs != 0)
        flag_sectors_with_objects((uint8_t *)(uintptr_t)geom, (int16_t *)(uintptr_t)objs);
    setup_sfx_nodes();
}

/* unload_map_geometry 0x2f459: zero g_player_sector (0x90c12), free the 4 DPMI map-geometry
 * selectors (xchg each global -> 0, free the old value), free the object-table allocation
 * (g@0x85c4c), and free the effect pools. free_dpmi_selector + game_free_if_not_null are bridges;
 * free_effect_pools is lifted. */
void unload_map_geometry(void)
{
    G16(VA_g_player_sector) = 0;
    uint16_t s;
    s = (uint16_t)G16(VA_g_geometry_selector); G16(VA_g_geometry_selector) = 0; if (s) os_dpmi_free_descriptor(s);   /* was ml_bridge(0x2f772): free_dpmi_selector (int31 0001), 0-guard preserved */
    s = (uint16_t)G16(VA_g_geometry_selector + 0x4); G16(VA_g_geometry_selector + 0x4) = 0; if (s) os_dpmi_free_descriptor(s);   /* was ml_bridge(0x2f772): free_dpmi_selector (int31 0001), 0-guard preserved */
    s = (uint16_t)G16(VA_g_geometry_selector + 0x2); G16(VA_g_geometry_selector + 0x2) = 0; if (s) os_dpmi_free_descriptor(s);   /* was ml_bridge(0x2f772): free_dpmi_selector (int31 0001), 0-guard preserved */
    s = (uint16_t)G16(VA_g_vel_queue_b + 0x82); G16(VA_g_vel_queue_b + 0x82) = 0; if (s) os_dpmi_free_descriptor(s);   /* was ml_bridge(0x2f772): free_dpmi_selector (int31 0001), 0-guard preserved */
    uint32_t p = (uint32_t)G32(VA_g_sfx_nodes + 0x8); G32(VA_g_sfx_nodes + 0x8) = 0; game_free_if_not_null((uint8_t *)(uintptr_t)p); /* re-point 0x40a2a */
    free_effect_pools();                                    /* 0x343f5 */
}

/* rebuild_object_pointer_array 0x33ea1: if not already built (g@0x8a0d8==0) and the object-table
 * header (0x85c30 stored ptr) carries the 0x7533 magic, alloc a handle array sized 4*count from the
 * DAS cache pool and fill it with pointers to each variable-length object record (record size at
 * [rec+0]). ensure_das_cache_heap_space + pool_alloc_handle are lifted (direct calls). */
void rebuild_object_pointer_array(void)
{
    if (G32(VA_g_object_ptr_array) != 0) return;
    uint32_t ebx = (uint32_t)G32(VA_g_object_table_header);
    if (ebx == 0) return;
    if (RP16(ebx) != 0x7533) { G32(VA_g_object_table_header) = 0; return; }
    uint32_t cnt = RP16(ebx + 6);
    if (cnt == 0) return;
    uint32_t need = cnt << 2;
    ensure_das_cache_heap_space(need);                      /* 0x414d2 (stack arg) */
    uint32_t h = pool_alloc_handle(
        (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle), (int32_t)need);  /* 0x360f9 */
    if (h == 0) return;
    G32(VA_g_object_ptr_array) = (int32_t)h;
    uint32_t edi = RP32(h);                                         /* handle -> data array */
    uint32_t ecx = RP16(ebx + 6);
    ebx += RP16(ebx + 4);                                          /* -> first object record */
    do {
        RP32(edi) = ebx;
        ebx += RP16(ebx);                                          /* += record size */
        edi += 4;
    } while (--ecx > 0);
}

/* open_raw_state_temp 0x218de: build the raw-state temp path from the current raw-name scratch
 * (0x701ec): split_path -> build "<dir>/<name>" via build_game_path x2 -> '.TMP' ext -> dos_open.
 * Returns EAX = file handle. All callees bridged. Local 0x6c-byte frame. */
uint32_t open_raw_state_temp(void)
{
    uint8_t frame[0x6c];
    regs_t r; memset(&r, 0, sizeof r);
    r.eax = GADDR(VA_g_cfg_file_arg);
    r.edx = (uint32_t)(uintptr_t)(frame + 0x00);      /* dir  (ebp-0x6c) */
    r.ebx = (uint32_t)(uintptr_t)(frame + 0x50);      /* name (ebp-0x1c) */
    r.ecx = (uint32_t)(uintptr_t)(frame + 0x64);      /* ext  (ebp-8)    */
    ml_call(&r, 0x210ec);                             /* split_path */
    build_game_path(frame + 0x00, (const uint8_t *)(uintptr_t)GADDR(VA_g_dir_gdv + 0x50),
                           (const uint8_t *)(uintptr_t)GADDR(VA_g_heap_free_list + 0x87a));   /* re-point 0x2fb7f (dst,dir,name) */
    build_game_path(frame + 0x00, (const uint8_t *)(uintptr_t)(frame + 0x00),
                           (const uint8_t *)(uintptr_t)(frame + 0x50));   /* re-point 0x2fb7f (dst,dst,name) */
    r.eax = (uint32_t)(uintptr_t)(frame + 0x00); r.edx = 0x504d54;   /* 'TMP' */
    ml_call(&r, 0x2fbbc);                             /* set_filename_extension */
    return dos_open_file((uint32_t)(uintptr_t)(frame + 0x00), 0);  /* dos_open_file(path, 0) (C2) */
}

/* write_raw_state_temp 0x21879: if the raw-name scratch (0x701ec) is empty, no-op. Else build the
 * '.TMP' path (as open_raw_state_temp) BUT dos_make_directory first, then write the raw-state stream
 * to it (write_raw_state_stream). Void. */
void write_raw_state_temp(void)
{
    if (RP8(GADDR(VA_g_cfg_file_arg)) == 0) return;
    uint8_t frame[0x6c];
    regs_t r; memset(&r, 0, sizeof r);
    r.eax = GADDR(VA_g_cfg_file_arg);
    r.edx = (uint32_t)(uintptr_t)(frame + 0x00);
    r.ebx = (uint32_t)(uintptr_t)(frame + 0x50);
    r.ecx = (uint32_t)(uintptr_t)(frame + 0x64);
    ml_call(&r, 0x210ec);                             /* split_path */
    build_game_path(frame + 0x00, (const uint8_t *)(uintptr_t)GADDR(VA_g_dir_gdv + 0x50),
                           (const uint8_t *)(uintptr_t)GADDR(VA_g_heap_free_list + 0x876));   /* re-point 0x2fb7f (dst,dir,name) */
    r.eax = (uint32_t)(uintptr_t)(frame + 0x00);
    r.ebx = (uint32_t)(uintptr_t)(frame + 0x50); r.edx = (uint32_t)(uintptr_t)(frame + 0x00);
    ml_call(&r, 0x41be5);                             /* dos_make_directory(dst, dir, name) */
    build_game_path(frame + 0x00, (const uint8_t *)(uintptr_t)(frame + 0x00),
                           (const uint8_t *)(uintptr_t)(frame + 0x50));   /* re-point 0x2fb7f (dst,dst,name) */
    r.eax = (uint32_t)(uintptr_t)(frame + 0x00); r.edx = 0x504d54;
    ml_call(&r, 0x2fbbc);                             /* set_filename_extension '.TMP' */
    r.eax = (uint32_t)(uintptr_t)(frame + 0x00);
    ml_call(&r, 0x2114e);                             /* write_raw_state_stream(path) */
}

/* load_raw_state_from_temp 0x21934: EAX = temp file handle (0 = none). Reads the raw-state stream,
 * closes the handle, and returns EAX = 1 on a read-error (nonzero stream result), else finishes the
 * map-state setup: read!=0 & handle set -> release_raw_state_and_setup_sfx; handle==0 (no temp) ->
 * init_loaded_map_state. Returns EAX = status (1 = the read produced a nonzero/error result). */
uint32_t load_raw_state_from_temp(uint32_t handle)
{
    if (handle == 0) {
        init_loaded_map_state();               /* 0x2f6e6 */
        return 0;
    }
    uint32_t rd = read_raw_state_stream(handle);  /* re-point 0x214b9 (EAX=handle -> EAX) */
    dos_close_handle(handle);                     /* dos_close_handle(handle) (C2) */
    if (rd != 0) return 1;
    release_raw_state_and_setup_sfx();            /* 0x2f708 */
    return 0;
}

/* load_map_list 0x1059b: ESI = source map-list text. Copy it (through the terminating null) into a
 * fresh game-heap buffer stored at g_map_list_ptr (0x76710), then select entry 0. */
void load_map_list(const uint8_t *src)
{
    const uint8_t *p = src;
    while (*p != 0) p++;
    p++;                                              /* include the terminating null */
    int32_t len = (int32_t)(p - src);
    uint32_t buf = game_heap_alloc(len);       /* 0x1517d */
    G32(VA_g_map_list_ptr) = (int32_t)buf;
    memcpy((void *)(uintptr_t)buf, src, (size_t)len); /* rep movsb */
    select_map_entry_by_index(0);              /* 0x1073a */
}

/* switch_map_das_resources 0x105c0: EAX=new-map name, EDX=das-arg name. Strip both to basenames
 * (0x10711), look the map up in the list (lookup_map_raw_filename), and if it differs from the
 * current das basename, swap the map DAS: parse the das filename, reset the das entry-status table,
 * close the current das handle, reset renderer tables, free the old object-table alloc (g@0x85ce8),
 * and load the new das (load_das_file_wrapper) around a g@0x89f3f guard. pushal-framed. */
void switch_map_das_resources(uint32_t name_eax, uint32_t das_edx)
{
    uint8_t frame[0x40];
    /* basename_strip_ext(name -> frame+0);  basename_strip_ext(das -> frame+0x20) */
    basename_strip_ext((const char *)(uintptr_t)name_eax, (char *)(frame + 0x00));
    basename_strip_ext((const char *)(uintptr_t)das_edx,  (char *)(frame + 0x20));
    uint32_t hit = lookup_map_raw_filename((const char *)(frame + 0x00));  /* 0x10686 -> cursor */
    if (hit == 0) return;
    /* compare the map-list RAW-field cursor (hit) against the current das basename (frame+0x20). */
    int32_t cmp = compare_name_token_ci((const uint8_t *)(uintptr_t)hit, frame + 0x20); /* 0x1063d */
    if (cmp == 0) return;                             /* je 0x10638: same das, no swap */
    /* different das -> swap. parse the map-list das token (at hit) into scratch 0x7023c (+.DAS). */
    parse_map_das_filename((char *)(uintptr_t)hit, (char *)GADDR(VA_g_cfg_das_arg));  /* 0x1078a */
    reset_das_entry_status_table();            /* 0x3001b */
    close_das_file_handle();                   /* 0x2fd6b */
    reset_renderer_tables();                   /* 0x2f42b */
    uint32_t old = (uint32_t)G32(VA_g_map_das_dir_table_buffer);
    G32(VA_g_map_das_dir_table_buffer) = 0;
    game_free_if_not_null((uint8_t *)(uintptr_t)old);  /* re-point 0x40a2a (old object table) */
    G8(VA_g_das_skip_palette_load_flag) = 1;
    load_das_file_wrapper((uint32_t)(uintptr_t)GADDR(VA_g_cfg_das_arg));   /* 0x10c32 (EAX name) */
    G8(VA_g_das_skip_palette_load_flag) = 0;
}

#ifdef ROTH_STANDALONE
/* The object-record per-type init-handler table at obj1 0x30998, re-expressed as C (the risk-3
 * "reloc'd code-ptr table -> C owner" pattern): 67 LE-fixup-resolved canon VAs sliced from
 * flat_reloc.bin (entry 67 is code bytes = the table end), dispatched to the lifted bodies.
 * Every distinct handler is lifted; 0x30ab2 is the original's bare-ret filler. ABI per the
 * bridge callsite: ESI=record (EAX=0 at the one two-arg handler); returns discarded. */
static const uint32_t ml_objinit_tab[67] = {
    0x30ab2u, 0x30ab2u, 0x339ffu, 0x340f3u, 0x30ab2u, 0x30ab2u, 0x30ab2u, 0x34322u,
    0x30ab2u, 0x34086u, 0x340b6u, 0x340b6u, 0x340bcu, 0x34086u, 0x34086u, 0x34086u,
    0x30ab2u, 0x34086u, 0x30ab2u, 0x31cddu, 0x30ab2u, 0x31ccdu, 0x30ab2u, 0x30ab2u,
    0x31d82u, 0x31d2fu, 0x31d7eu, 0x31dd3u, 0x30ab2u, 0x30ab2u, 0x30ab2u, 0x30ab2u,
    0x30ab2u, 0x30ab2u, 0x30ab2u, 0x34086u, 0x3405eu, 0x30ab2u, 0x30ab2u, 0x30ab2u,
    0x30ab2u, 0x30ab2u, 0x30ab2u, 0x30ab2u, 0x30ab2u, 0x30ab2u, 0x30ab2u, 0x30ab2u,
    0x31dcbu, 0x31d2bu, 0x31d7au, 0x30ab2u, 0x30ab2u, 0x3107du, 0x30ab2u, 0x30ab2u,
    0x30ab2u, 0x31dcfu, 0x30ab2u, 0x30ab2u, 0x30ab2u, 0x30b27u, 0x30ab2u, 0x30ab2u,
    0x30ab2u, 0x30ab2u, 0x30ab2u,
};
static void ml_if_objinit_dispatch(uint32_t type, uint32_t esi)
{
    if (type >= 67) { roth_unreachable(0x30998u); return; }   /* past the table = garbage handler */
    switch (ml_objinit_tab[type]) {
    case 0x30ab2u: return;                                          /* bare ret */
    case 0x30b27u: tick_register_timed_effect(esi);  return;
    case 0x3107du: tick_spawn_damage_emitter(esi);   return;
    case 0x31ccdu: tick_cache_effect_base(esi);      return;
    case 0x31cddu: mark_geometry_records_by_id(esi); return;
    case 0x31d2bu: mark_geom_faces_b20(esi);         return;
    case 0x31d2fu: mark_geom_faces_b10(esi);         return;
    case 0x31d7au: mark_raw_state_b04(esi);          return;
    case 0x31d7eu: mark_raw_state_b02(esi);          return;
    case 0x31d82u: mark_raw_state_b01(esi);          return;
    case 0x31dcbu: mark_objects_b08(esi);            return;
    case 0x31dcfu: mark_objects_b04(esi);            return;
    case 0x31dd3u: mark_objects_b20(esi);            return;
    case 0x339ffu: tick_cmd_45(esi, 0);              return; /* EAX=0 at this callsite (memset io) */
    case 0x3405eu: tick_register_object_state(esi);  return;
    case 0x34086u: tick_rerun_command_execute(esi);  return;
    case 0x340b6u: FUN_000340b6(esi);                return;
    case 0x340bcu: tick_resolve_state_and_rerun(esi); return;
    case 0x340f3u: tick_move_floorceil(esi);         return;
    case 0x34322u: tick_apply_geometry_effect(esi);  return;
    }
}
#endif

/* init_loaded_object_table 0x33f0e: reset the object-table runtime scalars, install the two vtable
 * code pointers (0x90a34/0x8a2a0), then (if the object-table header 0x85c30 has the 0x7533 magic)
 * flag referenced textures, rebuild the object pointer array, resolve each indexed object's
 * face/state links (find_raw_state_record + find_face_record + the two-hop object-link chase through
 * g_map_objects_buffer 0x90aa8), and finally dispatch a per-type init handler ([type*4 + 0x30998])
 * for every object record. Handlers bridged via call_orig (ESI=record). pushal-framed. */
void init_loaded_object_table(void)
{
    G32(VA_g_state_link_buf_ptr + 0x4) = 0; G32(VA_g_state_link_buf_ptr + 0x8) = 0; G32(VA_g_state_link_word_b) = 0; G32(VA_g_state_link_word_a) = 0;
    G8(VA_g_command_chain_interrupt + 0x2) = 0;  G32(VA_g_state_link_obj_ptr) = 0; G32(VA_g_state_link_buf_ptr) = 0; G32(VA_g_item_drop_position + 0x124) = 0; G32(VA_g_item_drop_position + 0x10) = 0;
    /* the two object-table vtable slots hold RUNTIME code ptrs (the original's immediates are
     * relocated; consumers call through them — rwss `indirect:` hook + invoke_span_callback).
     * LATENT BUG FIX: stored canon (would crash a hook call in-game); exposed the
     * moment set_state_record_count re-pointed to this body and the oracle diffed its writes. */
    G32(VA_g_span_callback) = (int32_t)(0x33cf3u + OBJ_DELTA); G32(VA_g_pool_check_enabled + 0x28) = (int32_t)(0x33ddeu + OBJ_DELTA);
    if (G32(VA_g_object_table_header) == 0) return;
    flag_referenced_object_textures();          /* 0x33c49 */
    uint32_t ot = (uint32_t)G32(VA_g_object_table_header);
    if (RP16(ot) != 0x7533) { G32(VA_g_object_table_header) = 0; return; }
    rebuild_object_pointer_array();             /* 0x33ea1 */
    ot = (uint32_t)G32(VA_g_object_table_header);
    if (ot == 0) return;

    /* --- pass 1: resolve each indexed object's link field (obj+6) --- */
    int32_t cx = RP16(ot + 0xe);
    if (cx != 0) {
        uint32_t edi = RP16(ot + 0xc) + ot;
        do {
            uint32_t si = RP16(edi) + ot;
            edi += 2;
            uint16_t store;
            int32_t rr = find_raw_state_record((uint16_t)RP16(si + 6));   /* 0x4f52b */
            if (rr == 0) {
                store = 0;
            } else {
                int32_t F = find_face_record((uint16_t)rr);              /* 0x4f567 */
                uint32_t objs = (uint32_t)G32(VA_g_map_geometry_buffer);
                store = (uint16_t)F;
                uint32_t l = RP16(objs + (uint32_t)F + 6);
                if ((uint16_t)RP16(objs + l + 0x14) >= 0xfffd) {               /* jb NOT taken */
                    uint32_t G = RP16(objs + (uint32_t)F + 8);
                    if ((uint16_t)G != 0xffff) {
                        uint32_t l2 = RP16(objs + G + 6);
                        if ((uint16_t)RP16(objs + l2 + 0x14) < 0xfffd)
                            store = (uint16_t)G;
                    }
                }
            }
            RP16(si + 6) = store;
        } while (--cx > 0);
    }

    /* --- pass 2: run each object record's per-type init handler --- */
    uint32_t esi = (uint32_t)G32(VA_g_object_table_header);
    int32_t ecx = RP16(esi + 6);
    esi += RP16(esi + 4);
    if (ecx != 0) {
        do {
            uint32_t type = RP8(esi + 3) & 0x7f;
#ifndef ROTH_STANDALONE
            uint32_t handler = RP32((uintptr_t)GADDR(0x30998) + type * 4);
            regs_t io; memset(&io, 0, sizeof io);
            io.va  = handler;                 /* dispatch-table code ptr (already runtime) */
            io.esi = esi; io.ebx = type; io.ecx = (uint32_t)ecx;
            call_orig(&io);
#else
            ml_if_objinit_dispatch(type, esi);   /* the 0x30998 reloc'd code-ptr table, re-expressed as C */
#endif
            esi += RP16(esi);                 /* advance by record size */
        } while (--ecx > 0);
    }
}
