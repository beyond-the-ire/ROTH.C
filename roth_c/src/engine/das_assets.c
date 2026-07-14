/* lift_das_assets.c — the DAS asset cache & sprite/animation subsystem lifted to verified C.
 * Per docs/operating/recomp.md §4.6: every subsystem gets its own TU.
 *
 * das_assets is ROTH's handle-based sprite/texture/image asset system: DAS files (map
 * sprites/textures, ICONS.ALL, ademo, dbase200 sprites, backdrops) stream on demand into a
 * self-evicting DPMI-backed cache, get decoded/animated per frame, and blit to the framebuffer.
 * lift-lens: docs/reference/lift/das_assets.md; format
 * reference: docs/reference/ROTH_DAS_loader_notes.md.
 *
 * A4 — STORED POINTERS (the #1 hazard here): the DAS cache heap is the relocatable 'Pool'
 * handle allocator (g_das_cache_heap_handle 0x85c3c); an allocation can MOVE existing blocks.
 * Deref through the cache slot (g_das_cache_slots 0x89930) or the handle EVERY time — never
 * cache a raw block pointer across an allocation. Slot alloc ptrs, FAT buffer ptrs, and every
 * loaded block's +0x08 DPMI selector are runtime addresses: deref RAW, never via G8/G16/G32
 * (those add OBJ_DELTA and are only for fixed canon literals). ABI from the DISASM throughout.
 */
#include "common.h"
#include "engine.h"
#include "os_api.h"    /* re-point (C2): the .DAS block-load path's inline int21 read/lseek seams
                         * bind to the c2 DOS file contract (os_dos_read / os_dos_lseek) — the
                         * image-free loader pipeline. */
#include <string.h>

/* read a stored pointer/handle (a runtime address value) out of a canon obj3 global */
#define PTRG(canon_va) (*(volatile uint32_t *)(uintptr_t)GADDR(canon_va))

/* raw runtime-memory accessors (A4: for values that are ALREADY host addresses) */
#define R8(p)  (*(volatile uint8_t  *)(uintptr_t)(p))
#define R16(p) (*(volatile uint16_t *)(uintptr_t)(p))
#define R32(p) (*(volatile uint32_t *)(uintptr_t)(p))

static int da_lseek(uint32_t handle, uint32_t off);   /* fwd (defined with the cache core) */

/* ================================================================================================
 * Cluster B — file loaders (open / parse / read-into-cache).
 * The DOS/DPMI seams: raw inline int 0x21 (open/read/close/lseek) goes through g_os_soft_int;
 * the game's own DOS helpers (dos_open_file 0x41ae5 / dos_read_items 0x41b53 / dos_lseek
 * 0x41b9a / dos_close_handle 0x41b41) + the DPMI allocators (0x40a34/0x40a61) are bridged.
 * ================================================================================================ */

/* inline `int 0x21` AH=3E close */
static void da_int21_close(uint32_t handle)
{
    regs_t r; memset(&r, 0, sizeof r);
    r.ebx = handle; r.eax = 0x3e00;
    if (g_os_soft_int) g_os_soft_int(0x21, &r);
}

/* inline `int 0x21` AH=3F read -> CF; *count_out = bytes read */
static int da_int21_read(uint32_t handle, uint32_t buf, uint32_t count, uint32_t *count_out)
{
    regs_t r; memset(&r, 0, sizeof r);
    r.ebx = handle; r.edx = buf; r.ecx = count; r.eax = 0x3f00;
    uint32_t fl = g_os_soft_int ? g_os_soft_int(0x21, &r) : 1u;
    if (count_out) *count_out = r.eax;
    return (int)(fl & 1);
}

/* dos_seek_file_start (0x2f1a1): inline int21 AX=4200 from EAX=offset, EBX=handle -> CF */
static int da_seek_start(uint32_t handle, uint32_t off)
{
    return da_lseek(handle, off);
}

/* the game DOS helpers — re-pointed (C2) to the dpmi_dos_os wrappers (lift_dpmi_dos_os.c) over the
 * os_api.h file service. Their only callers (load_dbase200_sprite_cached / load_das_cache_resource)
 * are the in-game tier — never run in the batch oracle — so these conversions are oracle-neutral. */
static uint32_t da_dos_open(uint32_t path, uint32_t mode)     /* 0x41ae5: EAX=path, EDX=mode -> EAX=handle/0 */
{
    return dos_open_file(path, mode);
}
static void da_dos_lseek8(uint32_t handle, uint32_t off)      /* 0x41b9a: EAX=handle, EDX=off, whence=SET */
{
    /* the old bridge also staged ecx=handle, but dos_lseek push/pop-PRESERVES ECX (disasm
     * 0x41b9e/0x41bbf) so that 4th value was inert; whence is BL and 0 (SET) here. */
    dos_lseek(handle, off, 0);
}
static uint32_t da_dos_read_items(uint32_t buf, uint32_t isz, uint32_t n, uint32_t handle)
{                                                             /* 0x41b53 -> EAX = items read */
    return dos_read_items(buf, isz, n, handle);
}
static void da_dos_close(uint32_t handle)                     /* 0x41b41: EAX=handle */
{
    dos_close_handle(handle);
}

/* close_das_file_handle (0x2fd6b, 17 B) — take + close the map DAS file handle (0x85d00). */
void close_das_file_handle(void)
{
    uint32_t h = PTRG(0x85d00);
    PTRG(0x85d00) = 0;                       /* xchg */
    if (h != 0) da_int21_close(h);
}

/* alloc_image_view_descriptor (0x40b5c, 101 B) — carve a 0x18-byte view descriptor from the
 * game heap and fill it from the dims record EAX {+0 w, +2 h, +6 ?, +8 buffer ptr}:
 * {+0 buffer, +4 w, +6 field6, +8/+0xa 0, +0xc h-1, +0xe w-1, +0x10/+0x12 0} -> EAX=desc.
 * (The original would write through a NULL alloc — DOS null-page tolerance; the lift returns
 * 0 like the CF exit, unreachable in practice: an 0x18-byte heap alloc doesn't fail.) */
uint32_t alloc_image_view_descriptor(uint32_t rec)
{
    uint32_t d = game_heap_alloc_round4(0x18);
    if (d == 0) return 0;
    R32(d + 0x00) = R32(rec + 8);
    R16(d + 0x04) = R16(rec + 0);
    R16(d + 0x06) = R16(rec + 6);
    R16(d + 0x08) = 0;
    R16(d + 0x0a) = 0;
    R16(d + 0x0e) = (uint16_t)(R16(rec + 0) - 1);
    R16(d + 0x0c) = (uint16_t)(R16(rec + 2) - 1);
    R16(d + 0x10) = 0;
    R16(d + 0x12) = 0;
    return d;
}

/* init_backdrop_image_surface (0x2fd21, 27 B) — [0x89f0c]=7 + allocate the backdrop view
 * descriptor from the static dims record 0x71ef8 into 0x90a9c. Returns the descriptor. */
uint32_t init_backdrop_image_surface(void)
{
    R16(GADDR(VA_g_roth_error_code)) = 7;
    uint32_t d = alloc_image_view_descriptor(GADDR(VA_g_init_stage_error_strings + 0x128));
    PTRG(0x90a9c) = d;
    return d;
}

/* release_das_and_geometry_buffers (0x30149, 25 B) — return the DAS cache pool block to the
 * game heap + free the scene geometry buffers (render bridge). */
void release_das_and_geometry_buffers(void)
{
    uint32_t h = PTRG(0x85c3c);
    PTRG(0x85c3c) = 0;                       /* xchg */
    if (h != 0) game_heap_free((uint8_t *)(uintptr_t)h);
    free_scene_geometry_buffers();    /* re-pointed: free_scene_geometry_buffers (render_world) */
}

/* close_das_handles_and_buffers (0x2f163, 62 B) — reset the ademo section pointers, free the
 * ademo buffer (game_free_if_not_null [L]), close the ademo file handle. */
void close_das_handles_and_buffers(void)
{
    PTRG(0x85cf4) = 0;
    PTRG(0x85cf0) = 0;
    PTRG(0x85cf8) = 0;
    PTRG(0x85cfc) = 0;
    uint32_t buf = PTRG(0x85cec);
    PTRG(0x85cec) = 0;                       /* xchg */
    if (buf != 0) game_free_if_not_null((uint8_t *)(uintptr_t)buf);
    if (PTRG(0x85c38) != 0) {
        uint32_t h = PTRG(0x85c38);
        PTRG(0x85c38) = 0;                   /* xchg */
        da_int21_close(h);
    }
}

/* load_das_file_wrapper (0x10c32, 31 B) — build the map-DAS path (fmt 0x764a0) into a stack
 * buffer and load it (load_map_das_file 0x2f1b4 [L, map_load]). EAX=name arg (-> EBX). */
uint32_t load_das_file_wrapper(uint32_t name_arg)
{
    uint8_t path[0x78];
    /* re-pointed build_game_path: EAX->edi(dest), EDX->esi(prefix 0x764a0), EBX->ebx(name) */
    build_game_path(path, (const uint8_t *)(uintptr_t)GADDR(VA_g_dir_digi + 0x50),
                           (const uint8_t *)(uintptr_t)name_arg);
    /* re-pointed: load_map_das_file (map_load) — CF-returning inline-int21 loader routed through
     * g_os_soft_int. Restaged: the wrapper test now stc-patches the orig's open int21
     * site 0x2f1c8 (CF=1) and runs the lift with g_os_soft_int=NULL, so BOTH sides fail-open at
     * the open and take err_noheap identically (no fd is ever opened -> fd-liveness gotcha moot).
     * CF EXPOSURE: 0x10c32's tail is `call 0x2f1b4; pop ebp; mov esp,ebp; ret` — pop/mov leave
     * CF untouched, so load_map_das_file's CF propagates to the caller's `jb` (main_sequence 0x10298).
     * Return it so the image-free gc_call dispatch can thread it (was a void CF-drop). */
    return load_map_das_file((uint32_t)(uintptr_t)path);
}

/* load_ademo_das_wrapper (0x10c70, 31 B) — same but into load_ademo_das_file (this TU). */
void load_ademo_das_wrapper(uint32_t name_arg)
{
    uint8_t path[0x78];
    /* re-pointed build_game_path: EAX->edi(dest), EDX->esi(prefix 0x764a0), EBX->ebx(name) */
    build_game_path(path, (const uint8_t *)(uintptr_t)GADDR(VA_g_dir_digi + 0x50),
                           (const uint8_t *)(uintptr_t)name_arg);
    load_ademo_das_file((uint32_t)(uintptr_t)path);
}

/* load_icons_all (0x1602e, 51 B) — set_8495c(0x15ee2, fmt, name) then build the ICONS.ALL
 * path and blob-load it into g_ui_scratch [0x7f56c]. The build_game_path call consumes the
 * EDX/EBX values LEFT LIVE by the set_8495c call (gotcha H: thread the bridge's out regs). */
void load_icons_all(void)
{
    uint8_t path[0x78];
    /* re-pointed set_8495c: writes EAX (0x15ee2 = the ICONS.ALL frame callback) to [0x8495c].
     * The original loads EBX=0x75dcc / EDX=0x763b0 BEFORE this call and leaves them live for the
     * build_game_path call below (set_8495c touches only EAX) — so pass them straight through. */
    set_8495c(GADDR(0x15ee2));
    /* re-pointed build_game_path: EAX->edi(dest=path), EDX->esi(prefix 0x763b0), EBX->ebx(name 0x75dcc) */
    build_game_path(path, (const uint8_t *)(uintptr_t)GADDR(VA_g_dir_data),
                           (const uint8_t *)(uintptr_t)GADDR(VA_g_heap_free_list + 0x7bc));
    /* re-pointed: load_file_blob (file_config) — raw inline-int21 open/lseek/read routed through
     * g_os_soft_int. Restaged: the load_icons_all test now stc-patches the orig's open
     * int21 site 0x143bd (CF=1) and runs the lift with g_os_soft_int=NULL, so BOTH sides fail-open
     * at the open and return 0 (no file, no fd). load_icons_all stores 0 into [0x7f56c] on both. */
    PTRG(0x7f56c) = load_file_blob((uint32_t)(uintptr_t)path);
}

/* spawn_object_from_das_resource (0x302e0, 169 B) — spawn a dynamic entity from an ademo FAT
 * '$' entry: kind byte = fat[+7] -> spawn_entity_into_state_pool_a (bridge; returns the slot
 * in EDX — a non-EAX return), stamp the entity record (0x91e03 + slot), optional drop-below
 * adjust (EBX==0), the def-driven flag + spawn sound (play_entity_sound bridge, coords from
 * the source object), and face the player (compute_player_object_bearing bridge -> [esi+6]).
 * EAX=source object, DX=resource id, EBX=variant -> EAX = -1 spawned / 0. */
uint32_t spawn_object_from_das_resource(uint32_t obj, uint32_t id, uint32_t variant)
{
    id &= 0xffff;
    if (id < 0x200) return 0;
    uint32_t fat = PTRG(0x85cf0);
    if (fat == 0) return 0;
    uint32_t entry = fat + (id - 0x200) * 8;
    if (R8(entry + 6) != 0x24) return 0;                 /* flag_1 '$' */
    /* re-pointed: spawn_entity_into_state_pool_a 0x4f00d [L, entity_ai] direct-C. The
     * lifted body takes a const regs_t* (reads AH=kind idx + ESI=source object) and RETURNS the
     * EDX backref (slot-0x91e03) or 0 when the actor pool is full. The das spawn test now stages
     * the full state-pool-A fixture on BOTH sides (0x91e00 count / 0x91e04 slot array / 0x85cf4
     * 0x68-record base / the entity-def cache head 0x819d8 / resolve_object_owner_sector's
     * 0x90aa4/0x91df8/0x91dfc) — the test_entity_ai spawn-fixture pattern — so the real leaf +
     * its callees (entity_def_cache_lookup 0x1e2f6, resolve_object_owner_sector 0x4f263) run
     * symmetrically. NB the ESI=obj mapping (the callee reads word[esi+4] def id + writes
     * esi+9/+0xc) that the old bridge threaded is preserved. */
    regs_t io; memset(&io, 0, sizeof io);
    io.eax = (uint32_t)R8(entry + 7) << 8;               /* mov ah,[entry+7] (al = don't-care) */
    io.esi = obj;                                        /* mov esi,eax @0x30313 (source object) */
    int32_t bref = spawn_entity_into_state_pool_a(&io);
    if (bref == 0) return 0;                             /* pool full: EDX backref = 0 */
    uint32_t rec = (uint32_t)bref + GADDR(VA_g_state_pool_a_count + 0x3);
    R8(rec + 0xb) = 0;
    if (variant == 0) {
        R16(obj + 0xa) -= 0x50;
        R8(rec + 0xb) = 0x50;
        R8(obj + 7) |= 0x40;
    }
    R16(rec + 0x1a) = 0;
    uint32_t def = R32(rec);
    if (def != 0) {
        if (R16(def + 0x5c) > 0x200) R8(rec + 8) |= 0x44;
        uint32_t snd = R16(R32(def) + 0x64);
        if (snd != 0)                                    /* play_entity_sound [L, call-closed] */
            play_entity_sound(snd - 1, 0, R16(obj + 0), R16(obj + 2));
    }
    /* compute_player_object_bearing [L]: ESI = the source object (the original call site's
     * live ESI — NOT an EAX arg; a bridge passing eax-only would drop it) */
    R8(obj + 6) = (uint8_t)compute_player_object_bearing(obj);
    return 0xffffffffu;
}

/* read_das_palette (0x2f379, 178 B) — seek to the map DAS palette ([hdr+0xc], handle 0x85d00),
 * read the 768 6-bit palette bytes into the LOW-MEMORY palette buffer (segment word [0x90bca]
 * << 4; also published to [0x85488]), build the 64-entry brightness curve (16-bit fixed-point:
 * start 0x80, step (gamma<<5)+0x100 decaying by gamma, clamped to 0..0x3fff, table byte =
 * value>>8), then remap every palette byte through the curve IN PLACE. -> CF. */
int read_das_palette(void)
{
    uint32_t hdr = GADDR(VA_g_das_collision_buffer + 0x8);
    uint32_t fh  = PTRG(0x85d00);
    if (da_seek_start(fh, R32(hdr + 0xc))) return 1;
    uint32_t pal = (uint32_t)R16(GADDR(VA_g_vel_queue_b + 0x88)) << 4;   /* low-mem buffer (segment<<4) */
    PTRG(0x85488) = pal;
    uint32_t got;
    if (da_int21_read(fh, pal, 0x300, &got)) return 1;
    if (got != 0x300) return 1;
    uint8_t curve[0x40];
    uint16_t bx = R16(GADDR(VA_g_gamma_level));                   /* gamma setting */
    uint16_t dx = (uint16_t)((bx << 5) + 0x100);
    uint16_t ax = 0x80;
    for (int32_t i = 0x40; i > 0; i--) {
        uint16_t v = ax;
        if ((int16_t)v < 0) v = 0;                       /* jns */
        if (v >= 0x3fff) { v = 0x3fff; bx = 0; dx = 0; } /* clamp kills the slope */
        curve[0x40 - i] = (uint8_t)(v >> 8);
        ax = (uint16_t)(ax + dx);
        dx = (uint16_t)(dx - bx);
    }
    uint32_t p = (uint32_t)R16(GADDR(VA_g_vel_queue_b + 0x88)) << 4;
    for (int32_t i = 0x300; i > 0; i--, p++)
        R8(p) = curve[R8(p)];
    return 0;
}

/* allocate_das_worker_buffers (0x2fa29, 288 B) — startup: allocate the 0x400-para DOS block
 * (palette buffer segment -> [0x90bca], selector -> [0x89f0e]) + the 0x15b00-byte worker heap
 * block ([0x85c54]) and carve it: the 256-ALIGNED shade table [0x85d08] (selector [0x90be2],
 * 0x10000 window), [0x85d10]/[0x85d0c] (0x100), [0x86d14]/[0x90c0e] (0x2000, +0x100 limit),
 * the blend table [0x86d28], [0x86d1c]=[0x86d24] w/ selectors [0x90c10]=[0x89f14] (0x2000),
 * [0x86d20] (+[0x86d18] = -0x100 base, 0x200 limit, selector [0x89f16]), tail -> [0x85c50].
 * DPMI alloc/map bridged; any CF failure aborts mid-sequence (faithful). */
int allocate_das_worker_buffers(void)
{
    /* re-pointed (C2): dpmi_alloc_dos_memory 0x40a34 -> direct C. EAX=0x400 BYTES (the wrapper
     * converts to paragraphs (bytes+0xf)>>4 per disasm 0x40a37); returns EAX=segment (0 + STC on
     * fail) and DX=selector; *sel_io carries DX in/out, untouched on fail (engine.h:855). The
     * original bails on CF (jb 0x40a44); the lift keys that off the 0-segment return — a successful
     * DOS alloc never yields real-mode segment 0. */
    /* CF EXPOSURE: 0x2fa29 returns CF to main_sequence's `jb 0x102ab` (line 0x102a5). Faithful
     * CF per disasm: dpmi_alloc fail -> STC (jb 0x2fa33); game_heap_alloc fail -> CLC (`or eax,eax;
     * je 0x2fb48`, the or clears CF); any DA_MAP (0x40a61) fail -> STC (jb 0x2fb48); success falls
     * through -> CLC. Return 1=CF/0=CLC so the image-free gc_call dispatch can thread it. */
    uint32_t alloc_sel = 0;
    uint32_t alloc_seg = dpmi_alloc_dos_memory(0x400, &alloc_sel);
    if (alloc_seg == 0) return 1;                        /* dpmi_alloc STC -> CF=1 */
    R16(GADDR(VA_g_vel_queue_b + 0x88)) = (uint16_t)alloc_seg;           /* real-mode segment */
    R16(GADDR(VA_g_roth_error_code + 0x2)) = (uint16_t)alloc_sel;           /* selector */
    uint32_t blk = game_heap_alloc(0x15b00);
    if (blk == 0) return 0;                              /* heap-alloc fail: `or eax,eax; je` -> CF=0 (faithful) */
    PTRG(0x85c54) = blk;
    uint32_t a = (PTRG(0x85c54) + 0xff) & ~0xffu;        /* add 0xff; sub al,al — 256-align */
#ifdef ROTH_STANDALONE
    /* image-free boot: map_buffer_to_dpmi_selector 0x40a61 =
     * alloc 1 LDT descriptor (int31 0000), set base (int31 0007), set limit=lim-1 (int31 0008), return
     * the selector -> the c2 DPMI descriptor contract (os_api.h; cand_dasload proved this mint image-
     * free). CF from any of the three legs -> *cf_out. */
#define DA_MAP(addr, limit, cf_out)                                              \
    ({ uint16_t sel_ = 0; int cf_ = os_dpmi_alloc_descriptors(1, &sel_);         \
       if (!cf_) cf_ = os_dpmi_set_segment_base(sel_, (uint32_t)(addr));         \
       if (!cf_) cf_ = os_dpmi_set_segment_limit(sel_, (uint32_t)(limit) - 1u);  \
       *(cf_out) = cf_ ? 1 : 0; sel_; })
#else
#define DA_MAP(addr, limit, cf_out)                                         \
    ({ regs_t m_; memset(&m_, 0, sizeof m_);                                \
       m_.va = 0x40a61u + OBJ_DELTA; m_.eax = (addr); m_.edx = (limit);     \
       call_orig(&m_); *(cf_out) = (int)(m_.eflags & 1); (uint16_t)m_.eax; })
#endif
    int cf;
    uint16_t sel;
    PTRG(0x85d08) = a;                                   /* shade table (256-aligned) */
    sel = DA_MAP(a, 0x10000, &cf); if (cf) return 1;
    R16(GADDR(VA_g_transparency_blend_selector)) = sel;
    a += 0x10000;
    PTRG(0x85d10) = a;
    sel = DA_MAP(a, 0x100, &cf); if (cf) return 1;
    R16(GADDR(VA_g_das_remap_chunk_10000_ptr + 0x4)) = sel;
    a += 0x100;
    PTRG(0x86d14) = a;
    sel = DA_MAP(a, 0x2000 + 0x100, &cf); if (cf) return 1;
    R16(GADDR(VA_g_text_color_ramp_selector)) = sel;
    PTRG(0x86d28) = a;
    a += 0x2000;
    sel = DA_MAP(a, 0x2000 + 0x100, &cf); if (cf) return 1;
    PTRG(0x86d1c) = a;
    PTRG(0x86d24) = a;
    R16(GADDR(VA_g_text_color_ramp_selector + 0x2)) = sel;
    R16(GADDR(VA_g_gamma_level + 0x2)) = sel;
    a += 0x2000;
    PTRG(0x86d20) = a;
    PTRG(0x86d18) = a - 0x100;
    sel = DA_MAP(a - 0x100, 0x100 + 0x100, &cf); if (cf) return 1;
    R16(GADDR(VA_g_gamma_level + 0x4)) = sel;
    a += 0x100;
    PTRG(0x85c50) = a;
#undef DA_MAP
    return 0;                                            /* success -> CLC */
}

/* load_ademo_das_file (0x2effb, 360 B) — open the ademo DAS (raw int21 3D00), read + validate
 * the 0x44-byte DASHeader into 0x85c58 (version 5, palette offset 0), size + game_heap_alloc
 * one buffer for all sections, lay out the aligned section pointers (dir 0x85cf4 wait — the
 * buffer base 0x85cec, dir-table 0x85cf4, FAT 0x85cf0, optional 0x85cf8/0x85cfc), then seek +
 * read each present section (the 4-byte-entry table goes into the worker buffer 0x85c50 +
 * 0x800). EAX=path. CF-ish returns untested by the wrapper (faithful: plain returns). */
void load_ademo_das_file(uint32_t path)
{
    regs_t o; memset(&o, 0, sizeof o);                   /* int21 AX=3D00 open (read-only) */
    o.edx = path; o.eax = 0x3d00;
    uint32_t fl = g_os_soft_int ? g_os_soft_int(0x21, &o) : 1u;
    if (fl & 1) return;
    PTRG(0x85c38) = o.eax;
    uint32_t hdr = GADDR(VA_g_das_collision_buffer + 0x8);
    uint32_t got;
    if (da_int21_read(PTRG(0x85c38), hdr, 0x44, &got)) return;
    if (R16(hdr + 4) != 5) return;                       /* version */
    if (R32(hdr + 0xc) != 0) return;                     /* no palette */
    uint32_t dirsz = R16(hdr + 0x1a);
    uint32_t fatsz = R32(hdr + 0x2c);
    uint32_t na = R16(hdr + 0x34);                       /* ebp */
    uint32_t nb = (uint16_t)(na + R16(hdr + 0x36));
    uint32_t tab8 = nb * 8;                              /* ecx */
    uint32_t total = tab8 + R16(hdr + 0x3c) + R16(hdr + 0x3e) + dirsz + fatsz + 0x18;
    uint32_t buf = game_heap_alloc((int32_t)total);
    if (buf == 0) return;
    PTRG(0x85cec) = buf;
    uint32_t a = (buf + dirsz + 3) & ~3u;                /* and al,0xfc — align4 */
    PTRG(0x85cf4) = a;
    a = (a + fatsz + 3) & ~3u;
    PTRG(0x85cf0) = a;
    a += tab8;
    if (R16(hdr + 0x3c) != 0) { PTRG(0x85cf8) = a; a += R16(hdr + 0x3c); }
    if (R16(hdr + 0x3e) != 0) { PTRG(0x85cfc) = a; a += R16(hdr + 0x3e); }
    uint32_t fh = PTRG(0x85c38);
    if (da_seek_start(fh, R32(hdr + 8))) return;
    if (da_int21_read(fh, PTRG(0x85cf0), tab8, &got)) return;    /* the (a+b)*8 FAT */
    if (dirsz != 0) {
        if (da_seek_start(fh, R32(hdr + 0x1c))) return;
        if (da_int21_read(fh, PTRG(0x85cec), dirsz, &got)) return;
    }
    if (na != 0) {
        if (da_seek_start(fh, R32(hdr + 0x24))) return;
        if (da_int21_read(fh, PTRG(0x85c50) + 0x800, na * 4, &got)) return;
    }
    if (fatsz != 0) {
        if (da_seek_start(fh, R32(hdr + 0x28))) return;
        if (da_int21_read(fh, PTRG(0x85cf4), fatsz, &got)) return;
    }
    if (R16(hdr + 0x3c) != 0) {
        if (da_seek_start(fh, R32(hdr + 0x38))) return;
        if (da_int21_read(fh, PTRG(0x85cf8), R16(hdr + 0x3c), &got)) return;
    }
    if (R16(hdr + 0x3e) != 0) {
        if (da_seek_start(fh, R32(hdr + 0x40))) return;
        da_int21_read(fh, PTRG(0x85cfc), R16(hdr + 0x3e), &got);
    }
}

/* load_das_cache_resource (0x1869b, 178 B) — load resource `idx` from an open file `handle`
 * into a fresh cache-pool block: lseek to idx*8, read the 4-byte size, make room + alloc, then
 * read the payload with a retry/abort loop through show_resource_error_box (menu bridge; the
 * sticky auto-abort counter 0x810c8 short-circuits it). EAX=idx, EDX=handle -> EAX=handle/0. */
uint32_t load_das_cache_resource(uint32_t idx, uint32_t handle)
{
    if (idx == 0) return idx;                            /* je exit: EAX passthrough (=0) */
    da_dos_lseek8(handle, idx << 3);
    uint32_t size = 0;
    da_dos_read_items((uint32_t)(uintptr_t)&size, 4, 1, handle);
    ensure_das_cache_heap_space(size);
    uint32_t h = pool_alloc_handle((uint32_t *)(uintptr_t)PTRG(0x85c3c), (int32_t)size);
    if (h == 0) return 0;
    for (;;) {
        uint32_t st = 1;
        if (da_dos_read_items(R32(h), size, 1, handle) != 0)
            break;                                       /* read ok */
        if (PTRG(0x810c8) != 0) { st = 3; }
        else {
            /* show_resource_error_box -> direct lifted call [RE-POINTED]. ORACLE-NEUTRAL:
             * load_das_cache_resource 0x1869b is never RUN in the oracle — it is marker-/mov-clc-ret-
             * stubbed wherever it appears as a callee (test_inventory.c:1263, test_das_assets.c:1672),
             * so this DOS-read error branch never executes under roth-oracle. The direct call preserves
             * the modal button EAX (retry=1 / give-up=2 / abort). The lift_file_config.c sites already
             * call show_resource_error_box directly; the audio site stays bridged (its error path
             * IS oracle-tested via canned 0/2 stubs and the compositor cannot run in a batch oracle). */
            st = show_resource_error_box();        /* show_resource_error_box (0x2632a) */
            if (st == 2) PTRG(0x810c8) += 1;
        }
        if (st == 1) continue;                           /* retry */
        if (st == 3) {                                   /* abort: free + fail */
            pool_free_handle((uint32_t *)(uintptr_t)PTRG(0x85c3c),
                                    (uint32_t *)(uintptr_t)h);
            return 0;
        }
        break;                                           /* st==2 etc: keep the handle */
    }
    return h;
}

/* decode_das_to_padded_buffer (0x1874d, 132 B) — decode a DAS image handle into a fresh
 * (w+16)x(h+16) zero-filled cache block (blitted at +8,+8 via blit_das_image_to_buffer, row
 * mode 1), FREEING the source handle. Returns the padded handle (0 on alloc failure/null in).
 * EAX=src handle, EDX=&out_w, EBX=&out_h. */
uint32_t decode_das_to_padded_buffer(uint32_t src_h, uint32_t *out_w, uint32_t *out_h)
{
    uint32_t padded = 0;
    if (src_h == 0) return 0;
    uint32_t blk = R32(src_h);
    uint32_t w = (uint32_t)(int32_t)(int16_t)R16(blk + 4) + 0x10;
    uint32_t hh = (uint32_t)(int32_t)(int16_t)R16(blk + 6) + 0x10;
    *out_w = w;
    *out_h = hh;
    uint32_t size = w * hh;
    ensure_das_cache_heap_space(size);
    padded = pool_alloc_handle((uint32_t *)(uintptr_t)PTRG(0x85c3c), (int32_t)size);
    if (padded != 0) {
        uint32_t d = R32(padded);
        mem_fill((void *)(uintptr_t)d, 0, size);
        blit_das_image_to_buffer(R32(src_h), d + (w << 3) + 8, w, 1);
    }
    pool_free_handle((uint32_t *)(uintptr_t)PTRG(0x85c3c),
                            (uint32_t *)(uintptr_t)src_h);
    return padded;
}

/* load_dbase200_sprite_cached (0x16087, 221 B) — load dbase200 sprite `idx` into the cached
 * RLE handle [0x7f574] (freeing any previous): open the dbase200 file (path 0x81f06 via
 * dos_open_file), lseek idx*8, read the size dword, alloc size+8, read the payload at +4,
 * zero the scale tag, repair the frame table [L] and rescale for the render resolution (this
 * TU), close. EAX=idx. flow_succ tail 0x15e96 = flush_dirty_rects' epilogue (video_display
 * boundary — bare leave/pops, no body). */
void load_dbase200_sprite_cached(uint32_t idx)
{
    if (PTRG(0x7f574) != 0) {
        pool_free_handle((uint32_t *)(uintptr_t)PTRG(0x85c3c),
                                (uint32_t *)(uintptr_t)PTRG(0x7f574));
        PTRG(0x7f574) = 0;
    }
    if (idx == 0) return;
    uint32_t fh = da_dos_open(GADDR(VA_g_dbase200_filename), 0);
    if (fh == 0) return;
    da_dos_lseek8(fh, idx << 3);
    uint32_t size = 0;
    if (da_dos_read_items((uint32_t)(uintptr_t)&size, 4, 1, fh) != 1)
        size = 0;
    if (size != 0) {
        ensure_das_cache_heap_space(size + 8);
        uint32_t h = pool_alloc_handle((uint32_t *)(uintptr_t)PTRG(0x85c3c),
                                              (int32_t)(size + 8));
        PTRG(0x7f574) = h;
        if (h != 0) {
            uint32_t blk = R32(h);
            da_dos_read_items(blk + 4, 1, size, fh);
            R32(blk) = 0;                                /* clear the scale tag */
            repair_das_rle_frame_count((uint16_t *)(uintptr_t)(blk + 4), (int32_t)size);
            rescale_das_frame(blk);
        }
    }
    da_dos_close(fh);
}

/* init_das_cache_heap / reset_das_entry_status_table live in cluster A above. */

/* ================================================================================================
 * Cluster D — the backdrop / parallax-sky image codec (6 fns, self-contained cluster).
 *
 * All six operate on ONE shared descriptor addressed through EBP — NOT a stack frame: the caller
 * (load_backdrop_image 0x4b08c, render_world) carves a 0x64-byte record on its stack, points EBP
 * at it, and every cluster fn takes EBP = record. Field map (derived from the disasm of 0x4b08c +
 * the cluster; offsets in bytes):
 *   +0x00 dword  DOS file handle (BACKDROP.RAW)
 *   +0x14 dword  ptr to the dest record (whose [+0] = dest framebuffer ptr; +8/+0xa/+0xc/+0xe
 *                feed the dest width/rows/stride/flags fields below)
 *   +0x18 word   source image width   (from the 8-byte file header, [hdr+8])
 *   +0x1a word   source image height  ([hdr+0xa])
 *   +0x28 dword  RLE decode buffer base (0x1000 B window, 0x20 slack)
 *   +0x2c dword  file read buffer ptr (= decode base + 0x1000 [+ 2*width when h-scaled])
 *   +0x30 dword  file read chunk size (bytes per int21 AH=3F read, rounded down to 4)
 *   +0x34 dword  decoded bytes available in the decode buffer
 *   +0x38 dword  current decoded-read ptr (into the decode buffer)
 *   +0x3c dword  unconsumed file bytes left in the read buffer (int21 return; <0 = exhausted)
 *   +0x40 dword  dest width in pixels
 *   +0x44 dword  dest row count (counts down to 0 = done)
 *   +0x48 dword  dest stride advance per emitted row
 *   +0x4c dword  RLE input stream ptr (into the file read buffer)
 *   +0x50 dword  horizontal-scale table ptr (word indices into the src row; 0 = unscaled copy)
 *   +0x54 dword  vertical 16.16 scale accumulator (<0 = skip/blend row boundary)
 *   +0x58 dword  vertical step (0x10000 = 1:1, else ((srcH<<16)-0x7fff)/destRows)
 *   +0x5c dword  saved source-row ptr (re-blit the same src row while the accumulator holds)
 *
 * All pointers held in the record are runtime addresses (A4): deref RAW via R8/R16/R32.
 * ================================================================================================ */

/* ================================================================================================
 * Cluster A — the cache core: FAT -> load -> slot -> evict -> relocate (12 fns).
 *
 * The cache heap is the relocatable 'Pool' handle allocator; its handle lives at
 * g_das_cache_heap_handle 0x85c3c. A slot entry (g_das_cache_slots 0x89930, 240 x 6 B) is
 * {4 B pool-handle ptr, 2 B tick}. The status table g_das_entry_status_table 0x86d30 is 0x1600
 * WORD entries: low byte < 0xfc = cache-slot index, 0xfc/0xfd/0xfe = placeholder kinds (high
 * byte = a per-kind payload), 0xff = unloaded. All Pool primitives + the evict/reserve/FAT-select
 * layer are already lifted (engine.h) — the cache core below is CALL-CLOSED C except the two
 * intentional host seams: DPMI selector mint/refresh (setup_das_block_selector 0x41191 /
 * refresh_das_block_selector_base 0x412ed, bridged) and the raw inline int 0x21/0x31
 * (g_os_soft_int).
 * ================================================================================================ */

/* setup_das_block_selector (0x41191): mint a DPMI LDT selector for a block. Re-pointed (C2) to the
 * lifted wrapper (lift_dpmi_dos_os.c) over os_api.h. ESI=block, EDI=limit size -> CF (1 = fail);
 * the wrapper frees the descriptor on a set-base/limit failure and keeps [block+8] (§8.2). */
static int da_setup_selector(uint32_t block, uint32_t size)
{
    return setup_das_block_selector(block, size);
}

/* inline `int 0x31` AX=0001 (free LDT descriptor, BX=selector) -> the g_os_soft_int hook
 * (case 0x31 wired in lift_registry's host_soft_int; NULL in the oracle = skipped, matching
 * the nop-patched original the oracle runs). */
static void da_dpmi_free_selector(uint16_t sel)
{
    regs_t r; memset(&r, 0, sizeof r);
    r.eax = 1; r.ebx = sel;
    if (g_os_soft_int) g_os_soft_int(0x31, &r);
}

/* inline `int 0x21` AX=4200 (lseek SET, EBX=handle, CX:DX=offset) -> CF */
static int da_lseek(uint32_t handle, uint32_t off)
{
    regs_t r; memset(&r, 0, sizeof r);
    r.eax = 0x4200;
    r.ebx = handle;
    r.edx = off;
    r.ecx = off >> 16;
    uint32_t fl = g_os_soft_int ? g_os_soft_int(0x21, &r) : 1u;
    return (int)(fl & 1);
}

/* read_das_block_payload (0x41317, 26 B) — stamp the block header (self ptr, no prefix) and
 * int21-read `count` payload bytes to block+0xa. EBX=file handle, ECX=count, EDX=block.
 * Returns CF: 1 = DOS error or short read. */
int read_das_block_payload(uint32_t handle, uint32_t count, uint32_t block)
{
    R32(block) = block;                      /* mov [edx],edx — self ptr */
    R32(block + 4) = 0;                      /* no prefix */
    /* re-pointed (C2): the ORIGINAL's inline `int 0x21` AH=3F read (disasm 0x41325: EBX=handle,
     * ECX=count, EDX=block+0xa) -> the c2 DOS read contract (os_api.h). CF (jb 0x4132f) or a short
     * read (cmp ecx,eax; jne 0x4132f) -> stc; otherwise clc. os_dos_read leaves *got untouched on CF. */
    uint32_t got = 0;
    if (os_dos_read(handle, block + 0xa, count, &got)) return 1;   /* jb -> stc */
    if (got != count) return 1;                                    /* short read -> stc */
    return 0;                                                      /* clc */
}

/* read_das_block_with_size_prefix (0x41331, 25 B) — like the payload read but reads count+4
 * bytes to block+6 and copies the leading size-prefix dword to block+4. Shares the stc tail
 * with 0x41317 in the original (a shared error exit, NOT a fall-through body). */
int read_das_block_with_size_prefix(uint32_t handle, uint32_t count, uint32_t block)
{
    R32(block) = block;                      /* self ptr */
    /* re-pointed (C2): the ORIGINAL's inline `int 0x21` AH=3F read (disasm 0x4133b: EBX=handle,
     * ECX=count+4, EDX=block+6) -> c2 DOS read. CF/short-read -> stc; else copy the leading size
     * dword down (mov eax,[edx]; mov [edx-2],eax = block+6 -> block+4) and clc. */
    uint32_t got = 0;
    if (os_dos_read(handle, block + 6, count + 4, &got)) return 1;
    if (got != count + 4) return 1;
    R32(block + 4) = R32(block + 6);         /* prefix -> +4 */
    return 0;
}

/* free_das_cache_entry (0x41413, 163 B) — free one loaded cache entry: mark the status entry
 * unloaded (0xff), take the Pool handle out of the slot, free the block's DPMI selector(s) by
 * kind (plain / RLE-subimage secondary / grouped children), then pool_free the handle(s).
 * EDI=status-entry ptr, ESI=slot-entry ptr -> CF=0. Pool re-read from 0x85c3c per call (A4). */
int free_das_cache_entry(uint32_t status_ptr, uint32_t slot_entry)
{
    R8(status_ptr) = 0xff;
    uint32_t handle = R32(slot_entry);
    R32(slot_entry) = 0;
    uint32_t blk = R32(handle);
    uint16_t flags = R16(blk + 0xa);
    if (flags & 0x40) {                                  /* grouped: free every child selector */
        int32_t n = (int32_t)(R8(blk + 0x10) >> 1);
        uint32_t tab = blk + 0x12;
        for (; n > 0; n--, tab += 2) {
            uint16_t r2 = (uint16_t)(R16(tab) << 1);     /* add ax,ax — 16-bit wrap */
            uint32_t child = blk + ((uint32_t)r2 << 3);  /* child = blk + ref*16 */
            uint16_t sel = R16(child + 8);
            if (sel != 0) da_dpmi_free_selector(sel);
        }
        pool_free_handle((uint32_t *)(uintptr_t)PTRG(0x85c3c),
                                (uint32_t *)(uintptr_t)handle);
        return 0;
    }
    if ((flags & 0x100) && R16(blk + 0x16) == 0xfffe) {  /* RLE-subimage: free the secondary too */
        uint32_t sec = R32(blk + 0x10);
        if (sec != 0) {
            uint32_t sblk = R32(sec);
            da_dpmi_free_selector(R16(sblk + 8));
            pool_free_handle((uint32_t *)(uintptr_t)PTRG(0x85c3c),
                                    (uint32_t *)(uintptr_t)sec);
        }
        pool_free_handle((uint32_t *)(uintptr_t)PTRG(0x85c3c),
                                (uint32_t *)(uintptr_t)handle);
        return 0;
    }
    da_dpmi_free_selector(R16(blk + 8));                 /* plain block */
    pool_free_handle((uint32_t *)(uintptr_t)PTRG(0x85c3c),
                            (uint32_t *)(uintptr_t)handle);
    return 0;
}

/* release_das_cache_slot_resources (0x413fd, 22 B) — scan the status table for the entry whose
 * low byte == the slot index and free it. AL=slot index, ESI=slot-entry ptr -> CF (1 = no
 * status entry references the slot). flow_succ: falls into free_das_cache_entry on the hit. */
int release_das_cache_slot_resources(uint8_t idx, uint32_t slot_entry)
{
    uint32_t p = GADDR(VA_g_das_entry_status_table);
    for (int32_t cx = 0x1600; cx > 0; cx--, p += 2) {
        if (R8(p) == idx)
            return free_das_cache_entry(p, slot_entry);
    }
    return 1;                                            /* stc — not found */
}

/* free_das_cache_handle (0x13136, 30 B) — free a DAS image handle: restore the save-under
 * pixels (blit_descriptor_rows 0x13106, blit_2d [L] — preserves all regs), then pool_free.
 * EAX=handle -> EAX=0. */
uint32_t free_das_cache_handle(uint32_t handle)
{
    if (handle != 0) {
        blit_descriptor_rows(handle);             /* re-pointed: blit_descriptor_rows (save-under, EAX=**desc) */
        pool_free_handle((uint32_t *)(uintptr_t)PTRG(0x85c3c),
                                (uint32_t *)(uintptr_t)handle);
    }
    return 0;
}

/* refresh_moved_das_cache_block (0x41250, 157 B) — the engine's own fix-up after the Pool moved
 * a block: clear the moved flag (heap byte block-8 bit 0x04), then refresh the DPMI selector(s)
 * (plain/grouped) or re-base the internal-pointer chain by the relocation delta. ESI=block;
 * preserves all regs. The selector-base refresh (0x412ed) is a DPMI bridge. */
void refresh_moved_das_cache_block(uint32_t block)
{
    R8(block - 8) -= 4;                                  /* sub byte [esi-8],4 — clear moved bit */
    uint16_t flags = R16(block + 0xa);
    if (flags & 0x8000) {                                /* internal-pointer kind */
        if (!(R16(block + 0x1a) & 1)) {                  /* not yet initialized */
            R32(block) = block;
            initialize_das_block_internal_pointers((void *)(uintptr_t)block);
            return;
        }
        uint32_t delta = block - R32(block);             /* relocation delta */
        R32(block) = block;
        uint32_t p = block + 0xa;
        R32(p + 6) += delta;                             /* [block+0x10] += delta */
        R32(p + 0xa) = R32(p + 0xa) + delta;             /* [block+0x14] += delta */
        uint32_t node = R32(p + 6);                      /* chain head (already re-based) */
        for (;;) {                                       /* walk + re-base the node chain */
            R32(node + 0x30) += delta;
            R32(node) = 0;
            uint32_t nxt = R32(node + 8);
            if (nxt == 0) return;
            nxt += delta;
            R32(node + 8) = nxt;
            node = nxt;
        }
    }
    if (flags & 0x40) {                                  /* grouped: refresh every child */
        int32_t n = (int32_t)(R8(block + 0x10) >> 1);
        uint32_t tab = block + 0x12;
        for (; n > 0; n--, tab += 2) {
            uint16_t r2 = (uint16_t)(R16(tab) << 1);
            /* re-pointed (C2): refresh_das_block_selector_base 0x412ed -> direct C */
            refresh_das_block_selector_base(block + ((uint32_t)r2 << 3));   /* child selector */
        }
        return;
    }
    refresh_das_block_selector_base(block);       /* plain: refresh the block selector */
}

/* the shared eviction loop (0x414b9) both make_room and ensure's full-pool path run:
 * evict one LRU slot at a time until the pool's free payload covers `need` (unsigned). */
static void das_room_loop(uint32_t need)
{
    for (;;) {
        uint32_t pool = PTRG(0x85c3c);                   /* re-read per pass (A4) */
        if (block_payload_size(pool) >= need) return;
        if (evict_one_das_cache_slot() == 0) return;
    }
}

/* das_cache_make_room (0x414b6, 28 B) — EAX=needed bytes; evict until the pool payload fits.
 * flow_succ: the entry stub of the shared loop above. Preserves ECX. */
void das_cache_make_room(uint32_t need)
{
    das_room_loop(need);
}

/* ensure_das_cache_heap_space (0x414d2, 34 B) — the 17-caller make-room hook: like make_room
 * but sizes by block_size_field8 unless the pool's full flag (+0x12) is set, in which case it
 * jumps INTO make_room's loop (payload-size compare). Re-checks the flag every pass. EAX=need. */
void ensure_das_cache_heap_space(uint32_t need)
{
    for (;;) {
        uint32_t pool = PTRG(0x85c3c);
        if (R8(pool + 0x12) != 0) { das_room_loop(need); return; }   /* jne 0x414b9 */
        if (block_size_field8(pool) >= need) return;          /* jae — unsigned */
        if (evict_one_das_cache_slot() == 0) return;
    }
}

/* postprocess_loaded_das_block (0x41051, 320 B) — classify + finish a just-read block:
 *  - RLE-subimage (flag 0x100 + word[+0x16]==0xfffe): repair the frame table, allocate the
 *    secondary decode block (w*h+0x10) from the cache pool (pin + make room + retry once on
 *    failure), clone the header into it (clearing flag 0x100), and mint its selector (the
 *    mint's CF is discarded — the original does `clc` after the call).
 *  - grouped (flag 0x40): zero all child selector words, then mint one selector per unique
 *    child (w*h+0x20); a child mint failure returns CF=1.
 *  - plain: mint the block selector with limit [0x8c73c]; CF = the mint's CF.
 * EDX=block ptr (the raw read buffer; the RLE frame table sits at +0x1a) -> CF.
 * The current block is re-derefed through g_current_das_handle 0x8c738 after any allocation
 * (the Pool may move it — A4).
 *
 * ORIGINAL BUG (unreachable path): if the post-make-room retry allocation ALSO fails, the
 * original executes `pop esi; stc; ret` (0x4111e) — discarding its own return address and
 * "returning" to the caller-pushed EDI (a heap data pointer) = a wild jump. That state needs
 * a full pool that ensure_das_cache_heap_space could not evict anything from, which the
 * caller's reserve/evict sequencing prevents. The lift returns CF=1 to the caller instead. */
int postprocess_loaded_das_block(uint32_t edx_blk)
{
    uint32_t handle = PTRG(0x8c738);
    uint32_t blk = R32(handle);
    uint16_t flags = R16(blk + 0xa);

    if ((flags & 0x100) && R16(blk + 0x16) == 0xfffe) {  /* RLE-subimage */
        repair_das_rle_frame_count((uint16_t *)(uintptr_t)(edx_blk + 0x1a),
                                          (int32_t)(PTRG(0x8c72c) - 0x20));
        uint32_t size = (uint32_t)R16(blk + 0xc) * R16(blk + 0xe) + 0x10;
        uint32_t sec = pool_alloc_handle_sized((uint32_t *)(uintptr_t)PTRG(0x85c3c),
                                                      (int32_t)size);
        if (sec == 0) {                                  /* pin + make room + retry once */
            PTRG(0x85400) = PTRG(0x8c738);               /* protect the current block */
            uint32_t save_slot = PTRG(0x8c734);
            uint16_t save_idx = R16(GADDR(VA_g_current_das_cache_slot_index));
            ensure_das_cache_heap_space(size);
            sec = pool_alloc_handle((uint32_t *)(uintptr_t)PTRG(0x85c3c), (int32_t)size);
            R16(GADDR(VA_g_current_das_cache_slot_index)) = save_idx;
            PTRG(0x8c734) = save_slot;
            PTRG(0x85400) = 0;
        }
        blk = R32(PTRG(0x8c738));                        /* re-deref: the alloc may have MOVED it */
        if (sec == 0)
            return 1;                                    /* see the ORIGINAL BUG note above */
        R32(blk + 0x10) = sec;
        R16(blk + 0x18) = 0xffff;
        uint32_t sblk = R32(sec);
        R32(sblk + 4) = R32(blk + 4);
        R32(sblk) = sblk;
        R32(sblk + 0xc) = R32(blk + 0xc);
        R16(sblk + 0xa) = (uint16_t)(R16(blk + 0xa) & 0xfeff);   /* clear flag 0x100 */
        da_setup_selector(sblk, size);                   /* mint; CF discarded (clc follows) */
        return 0;
    }
    if (!(flags & 0x40))                                 /* plain block */
        return da_setup_selector(blk, PTRG(0x8c73c));    /* CF passthrough */

    /* grouped */
    int32_t n = (int32_t)(R8(blk + 0x10) >> 1);
    uint32_t tab = blk + 0x12;
    for (int32_t i = n; i > 0; i--, tab += 2) {          /* pass 1: zero the child selectors */
        uint16_t r2 = (uint16_t)(R16(tab) << 1);
        R16(blk + ((uint32_t)r2 << 3) + 8) = 0;
    }
    tab = blk + 0x12;
    for (int32_t i = n; i > 0; i--, tab += 2) {          /* pass 2: mint each unique child */
        uint16_t r2 = (uint16_t)(R16(tab) << 1);
        uint32_t child = blk + ((uint32_t)r2 << 3);
        if (R16(child + 8) == 0) {
            uint32_t csize = (uint32_t)R16(child + 0xc) * R16(child + 0xe) + 0x20;
            if (da_setup_selector(child, csize)) return 1;   /* jb -> CF=1 */
        }
    }
    return 0;
}

/* load_das_block_for_fat_index (0x40d7c, 719 B) — the loader spine: resolve a FAT index to a
 * loaded cache block. Placeholder shortcuts (index bitmask 0x8c740 / defer flag 0x90cc4 /
 * flag_1 ' '/'$' entries) mark the status table without I/O; otherwise select the FAT entry,
 * reserve a slot, allocate the block from the cache pool (evict + retry, then a last-ditch
 * smaller alloc), lseek + read the payload (prefix kind by flag 8), the flag-2 header shuffle,
 * postprocess, and finally store the slot index into the status entry. AX=FAT index -> CF.
 * Register-transparent in the original (pushes fs/es + all GP regs). Call-closed C except the
 * int21 lseek (g_os_soft_int) and the selector mint inside postprocess. */
int load_das_block_for_fat_index(uint32_t eax_in)
{
    G8(VA_g_das_special_index_load_flag) = 0;
    uint32_t idx = (uint32_t)(int32_t)(int16_t)(uint16_t)eax_in;   /* cwde */
    uint16_t ax = (uint16_t)idx;

    if (ax >= 0x1600) return 1;                          /* stc */

    if (ax == R16(GADDR(VA_g_das_special_fat_index)))                       /* the special FAT index */
        goto special;

    if ((uint8_t)(ax >> 8) < 0x10) {                     /* cmp ah,0x10; jae -> normal */
        uint32_t bit = (R8(GADDR(VA_g_das_low_index_bitset) + (idx >> 3)) >> (ax & 7)) & 1;
        if (!bit && G8(VA_g_flat_shading_flag) == 0)
            goto normal;
        /* placeholder: status = 0xfd | per-index byte << 8 */
        R16(GADDR(VA_g_das_entry_status_table) + idx * 2) =
            (uint16_t)(0xfd | ((uint16_t)R8(GADDR(VA_g_texture_flat_color_table) + idx) << 8));
        return 0;                                        /* clc */
    }
    goto normal;

special:
    G32(VA_g_current_das_fat_index_x2) = (int32_t)(idx * 2);
    G8(VA_g_das_special_index_load_flag) = 0xff;
    select_das_fat_entry();
    if (PTRG(0x8c72c) == 0) return 1;
    if (R16(GADDR(VA_g_current_das_fat_entry + 0x6)) & 0x100) goto size_x8;
    goto alloc;

normal:
    G32(VA_g_current_das_fat_index_x2) = (int32_t)(idx * 2);
    select_das_fat_entry();
    if (PTRG(0x8c72c) == 0) return 1;
    {
        uint8_t f1 = R8(GADDR(VA_g_current_das_fat_entry + 0x6));
        if (f1 == 0x20 || f1 == 0x24) {                  /* ' ' / '$' placeholder kinds */
            uint16_t w = (uint16_t)((R16(GADDR(VA_g_current_das_fat_entry + 0x6)) & 0xff00) | (f1 == 0x20 ? 0xfe : 0xfc));
            R16(GADDR(VA_g_das_entry_status_table) + PTRG(0x8c730)) = w;
            return 0;                                    /* clc */
        }
        if (R16(GADDR(VA_g_current_das_fat_entry + 0x6)) & 0x100) goto size_x8;
        if (R16(GADDR(VA_g_current_das_fat_entry + 0x6)) & 0x200) goto size_x8;
    }
    goto alloc;

size_x8:
    PTRG(0x8c72c) <<= 3;                                 /* frame kinds: size *= 8 */

alloc:
    if (!reserve_das_cache_slot()) goto giveup;   /* jb 0x40ff8 */
    {
        uint32_t slot, size, h;
retry:
        slot = PTRG(0x8c734);
        size = PTRG(0x8c72c);
        if (size == 0) goto giveup;
        PTRG(0x8c73c) = size + 0x1a;                     /* selector limit */
        h = pool_alloc_handle_sized((uint32_t *)(uintptr_t)PTRG(0x85c3c),
                                           (int32_t)((size + 0x1a + 3) & ~3u));
        if (h == 0) {
            if (evict_das_cache_slot(1)) goto retry;   /* jae — evicted, retry */
            /* eviction failed: last-ditch alloc bounded by total-free instead */
            slot = PTRG(0x8c734);
            size = PTRG(0x8c72c);
            PTRG(0x8c73c) = size + 0xa + 0x10;
            h = pool_alloc_handle((uint32_t *)(uintptr_t)PTRG(0x85c3c),
                                         (int32_t)((size + 0xa + 0x10 + 3) & ~3u));
            if (h == 0) {
                R8(GADDR(VA_g_das_entry_status_table) + PTRG(0x8c730)) = 0xff;    /* 0x40fea */
                goto giveup;
            }
        }
        /* 0x40ec3 alloc_ok */
        PTRG(0x8c738) = h;
        R32(slot) = h;
        R16(slot + 4) = R16(GADDR(VA_g_das_cache_tick));             /* stamp the cache tick */
        {
            uint32_t fileoff = PTRG(0x8c70c);
            uint32_t fh = PTRG(0x85d00);                 /* map DAS file handle */
            if (PTRG(0x8c730) >= 0x2400) fh = PTRG(0x85c38);   /* ademo file handle */
            uint16_t fl2 = R16(GADDR(VA_g_current_das_fat_entry + 0x6));
            uint32_t blk;
            if (fl2 & 8) {                               /* size-prefixed kind */
                /* re-pointed (C2): inline int21 42 lseek SET (disasm 0x40f39) -> os_dos_lseek */
                if (os_dos_lseek(fh, (int32_t)(fileoff - 4), 0, NULL)) goto fail_free;
                blk = R32(R32(slot));
                if (read_das_block_with_size_prefix(fh, size, blk)) goto fail_free;
            } else {
                /* re-pointed (C2): inline int21 42 lseek SET (disasm 0x40f10) -> os_dos_lseek */
                if (os_dos_lseek(fh, (int32_t)fileoff, 0, NULL)) goto fail_free;
                blk = R32(R32(slot));
                if (read_das_block_payload(fh, size, blk)) goto fail_free;
            }
            if (fl2 & 2) {                               /* header shuffle: +0xa byte -> word +0 */
                uint8_t v = R8(blk + 0xa);
                R8(blk + 0xa) = 0;
                R16(blk) = v;                            /* mov [edx],ax (ax = zero-extended) */
            }
            if (postprocess_loaded_das_block(blk)) goto fail_free;
        }
        R8(GADDR(VA_g_das_entry_status_table) + PTRG(0x8c730)) = (uint8_t)R16(GADDR(VA_g_current_das_cache_slot_index));   /* status = slot idx */
        return 0;                                        /* clc */

fail_free:                                               /* 0x40fd5 */
        {
            uint32_t hh = R32(slot);
            R32(slot) = 0;
            pool_free_handle((uint32_t *)(uintptr_t)PTRG(0x85c3c),
                                    (uint32_t *)(uintptr_t)hh);
            return 1;                                    /* stc */
        }
    }

giveup:                                                  /* 0x40ff8 */
    if (PTRG(0x8c730) >= 0x2000) return 1;               /* stc — index out of bitmask range */
    {
        uint32_t i2 = PTRG(0x8c730) >> 1;
        R8(GADDR(VA_g_das_low_index_bitset) + (i2 >> 3)) |= (uint8_t)(1u << (i2 & 7));
        R16(GADDR(VA_g_das_entry_status_table) + i2 * 2) =
            (uint16_t)(0xfd | ((uint16_t)R8(GADDR(VA_g_texture_flat_color_table) + i2) << 8));
        return 0;                                        /* clc */
    }
}

/* init_das_cache_heap (0x30114, 53 B) — startup: build the scene render arena (bridge), then
 * (once) carve the whole free game heap minus 0x32000 into the DAS cache Pool (0x258 handles).
 * All heap/Pool callees lifted. */
void init_das_cache_heap(void)
{
    alloc_scene_render_arena();                   /* re-pointed: alloc_scene_render_arena (video) */
    if (PTRG(0x85c3c) != 0) return;
    uint32_t sz = query_game_heap_free() - 0x32000;
    uint32_t blk = game_heap_alloc((int32_t)sz);
    PTRG(0x85c3c) = pool_create((uint32_t *)(uintptr_t)blk, 0x258, (int32_t)sz);
}

/* reset_das_entry_status_table (0x3001b, 54 B) — free every loaded entry (status low byte
 * < 0xfc = a live slot index) and reset ALL 0x1600 status entries to word 0x00ff (the word
 * store also clears the per-kind high byte — store width is load-bearing, gotcha B4). */
void reset_das_entry_status_table(void)
{
    uint32_t p = GADDR(VA_g_das_entry_status_table);
    for (int32_t cx = 0x1600; cx > 0; cx--, p += 2) {
        uint32_t idx = R8(p);
        if (idx < 0xfc)
            free_das_cache_entry(p, GADDR(VA_g_das_cache_slots) + idx * 6);
        R16(p) = 0xff;
    }
}

/* register_dirty_rect (0x15b5b — lifted video_display, called direct): x, y, x_end, y_end */
static void da_dirty_rect(uint32_t x, uint32_t y, uint32_t xe, uint32_t ye)
{
    register_dirty_rect(x, (int32_t)y, xe, ye);
}

/* shade_remap_blit (0x13ecb): EAX = &desc{dest, w, h, stride, mode}. Re-pointed — the lifted
 * proto takes the descriptor pointer directly (EAX = &desc). Converting in this helper routes
 * all three call sites in draw_das_panel_slide_reveal. */
static void da_shade_blit(uint32_t *desc)
{
    shade_remap_blit((uint8_t *)desc);
}

/* draw_das_panel_slide_reveal (0x187d1, 594 B) — the sliding HUD-panel reveal: cap the visible
 * w/h to the image dims (centering the partial reveal), advance the slide progress (rec+0x28,
 * += frame-scale*4; sentinel 0x4d2 = "just finished" -> clear + full-rect dirty + zero-fill the
 * panel rows when the reveal underdrew), register the dirty rect, copy the visible rows from
 * the panel image (**rec, src offset rec+0x20 + y2*imgw) to the framebuffer ([0x90a98], doubled
 * rows in 400-line mode), then shade the panel edges with two shade_remap_blit calls (a
 * 5-dword stack descriptor {dest, w, h, stride, 9}). EAX=rec. flow_succ tail 0x18a23 =
 * shared_epilogue_6reg (math_util boundary — the `leave;pops;ret` epilogue, not body). */
void draw_das_panel_slide_reveal(uint32_t rec)
{
    uint32_t stride = R32(rec + 0x1c);
    uint32_t imgw   = R32(rec + 0xc);
    uint32_t dst = PTRG(0x90a98);
    uint32_t src = R32(R32(rec));                     /* **rec — handle double deref (G8) */
    uint32_t x   = R32(rec + 4);
    uint32_t y   = R32(rec + 8);
    uint32_t h   = R32(rec + 0x10);
    uint32_t y2  = R32(rec + 0x24);
    if (h > R32(rec + 0x18)) h = R32(rec + 0x18);     /* jbe — cap height */
    if (R32(rec + 0x28) != 0) {                       /* slide in progress */
        R32(rec + 0x28) += (uint32_t)G32(VA_g_frame_time_scale) * 4;
        uint32_t prog = R32(rec + 0x28);
        if ((int32_t)prog < (int32_t)h) {             /* jge */
            uint32_t half = (uint32_t)((int32_t)(h - prog) >> 1);   /* sar 1 */
            h = prog;
            y += half; y2 += half;                    /* center the partial reveal */
        } else {
            R32(rec + 0x28) = 0x4d2;                  /* done sentinel */
        }
    }
    uint32_t yoff = stride * y;
    dst += yoff;
    if (G8(VA_g_hires_line_doubling_flag) != 0) dst += yoff;                /* 400-line: doubled rows */
    uint32_t w = R32(rec + 0x14);
    if (w > imgw) {                                   /* jbe — cap width, center x */
        w = imgw;
        x += (R32(rec + 0x14) - w) >> 1;              /* shr */
    }
    src += imgw * y2 + R32(rec + 0x20);
    dst += x;
    uint32_t desc[5];                                 /* [ebp-0x2c..-0x1c] */
    desc[0] = dst;
    desc[3] = stride;
    desc[4] = 9;
    uint32_t yend = h + y;
    uint32_t xend = w + x;
    if (R32(rec + 0x28) == 0x4d2) {                   /* just finished this call */
        R32(rec + 0x28) = 0;
        if (w < R32(rec + 0x14) || h < R32(rec + 0x18)) {
            /* the reveal underdrew: full-rect dirty + zero the full panel rows */
            uint32_t fx  = R32(rec + 4);
            uint32_t fy  = R32(rec + 8);
            uint32_t fh  = R32(rec + 0x18);
            uint32_t fdi = PTRG(0x90a98) + fx;
            da_dirty_rect(fx, fy, fx + R32(rec + 0x14), fh + fy);
            uint32_t sy = fy * stride;
            if (G8(VA_g_hires_line_doubling_flag) != 0) { sy += sy; fh += fh; }
            fdi += sy;
            for (uint32_t i = 0; i < fh; i++) {       /* jae — unsigned */
                mem_fill((void *)(uintptr_t)fdi, 0, R32(rec + 0x14));
                fdi += stride;
            }
            goto blitrows;                            /* skips the normal dirty rect */
        }
    }
    da_dirty_rect(x, y, xend, yend);
blitrows:
    if (G8(VA_g_hires_line_doubling_flag) != 0) {                           /* 400-line: each src row twice */
        uint32_t s = src, d = dst;
        for (int32_t i = 0; i < (int32_t)h; i++) {    /* jl — signed */
            uint32_t si = s, di = d;
            for (uint32_t n = w; n; n--) { R8(di) = R8(si); di++; si++; }
            d += stride;
            si = s; di = d;
            for (uint32_t n = w; n; n--) { R8(di) = R8(si); di++; si++; }
            d += stride;
            s += imgw;
        }
        desc[2] = h * 2;                              /* [ebp-0x24] */
        desc[1] = 4;                                  /* [ebp-0x28] */
        da_shade_blit(desc);
        desc[1] = w - 4;
        desc[2] = 8;
    } else {
        uint32_t s = src, d = dst;
        for (int32_t i = 0; i < (int32_t)h; i++) {
            uint32_t si = s, di = d;
            for (uint32_t n = w; n; n--) { R8(di) = R8(si); di++; si++; }
            d += stride;
            s += imgw;
        }
        desc[2] = h;
        desc[1] = 4;
        da_shade_blit(desc);
        desc[1] = w - 4;
        desc[2] = 4;
    }
    desc[0] += 4;                                     /* 0x18a1a: shift the left edge */
    da_shade_blit(desc);
}

/* ================================================================================================
 * Cluster E — handle teardown / HUD free (4 fns).
 * ================================================================================================ */

/* free_das_image_handle (0x21cb1, 20 B) — pool_free_handle(cache pool, EAX=handle); no null
 * guard (faithful). Returns pool_free's EAX. */
uint32_t free_das_image_handle(uint32_t handle)
{
    return pool_free_handle((uint32_t *)(uintptr_t)PTRG(0x85c3c),
                                   (uint32_t *)(uintptr_t)handle);
}

/* free_hud_panel_das_handles (0x22633, 91 B) — free the two HUD panel handles 0x83d74/0x83d78.
 * ASYMMETRY (faithful): 0x83d74 is zeroed only when it was non-null (the je skips both the free
 * and the store); 0x83d78 is zeroed UNCONDITIONALLY (the je lands ON the store). */
void free_hud_panel_das_handles(void)
{
    if (PTRG(0x83d74) != 0) {
        pool_free_handle((uint32_t *)(uintptr_t)PTRG(0x85c3c),
                                (uint32_t *)(uintptr_t)PTRG(0x83d74));
        PTRG(0x83d74) = 0;
    }
    if (PTRG(0x83d78) != 0)
        pool_free_handle((uint32_t *)(uintptr_t)PTRG(0x85c3c),
                                (uint32_t *)(uintptr_t)PTRG(0x83d78));
    PTRG(0x83d78) = 0;
}

/* free_das_cache_handle_if (0x26266, 9 B) — null guard over free_das_cache_handle: `test eax;
 * je <a neighboring ret>; jmp 0x13136`. (The "flow_succ -> show_message_box" note in the plan
 * is a layout adjacency, not a shared body — the je targets the ret byte at 0x26265.) */
uint32_t free_das_cache_handle_if(uint32_t handle)
{
    if (handle == 0) return handle;
    return free_das_cache_handle(handle);     /* tail jmp -> returns 0 */
}

/* flush_object_das_handles (0x26cd4, 106 B) — walk the sound-sample table ([0x848f4], 0xc-byte
 * rows, count [0x848fc]) and release every loaded row: stop its sounds when the slot descriptor
 * is in use (+8 != 0), free the slot buffer (free_resource_chunk [L]), clear the slot-ptr table
 * entry (0x84874[key]) and mark the row unloaded (+0xb = 0xff). Gated on [0x7f550] (audio up)
 * and a non-null table. Call-closed C (stop_sounds + free_resource_chunk are lifted). */
void flush_object_das_handles(void)
{
    uint32_t row = PTRG(0x848f4);
    if (PTRG(0x7f550) == 0 || row == 0) return;
    int32_t count = (int32_t)PTRG(0x848fc);
    for (int32_t i = 0; i < count; i++, row += 0xc) {
        uint8_t key = R8(row + 0xb);
        if (key == 0xff) continue;
        uint32_t buf = R32(GADDR(VA_g_sound_voice_descriptors) + (uint32_t)key * 4);
        if (buf != 0) {
            if (R8(buf + 8) != 0)
                stop_sounds_for_sample_slot(key);
            free_resource_chunk((uint8_t *)(uintptr_t)buf);
            R32(GADDR(VA_g_sound_voice_descriptors) + (uint32_t)key * 4) = 0;
        }
        R8(row + 0xb) = 0xff;
    }
}

/* ================================================================================================
 * Cluster C — sprite blit + animation.
 * ================================================================================================ */

/* blit_das_image_to_buffer (0x1325b, 745 B) — THE DAS image blitter (menu/HUD/inventory/weapon
 * UI all come through here). EAX=image ptr (DAS sprite: flags byte, +4 width, +6 height, then
 * optional 8-B extra header [flag 4] / 0x300 palette [absent flag 2], payload at +8),
 * EDX=dest ptr, ECX = {high bytes: shade/remap selector, low byte: row mode 0=doubled 1=1:1
 * 2=every-other}, EBX = dest row stride — packed: when (stride - width) > 0xfff, EBX is
 * {high word: per-row fill count, low word: stride} and the blit becomes a one-byte-per-row
 * fill (variant B). RLE images (flags bit 0) stream through a stack decode window refilled by
 * rle_decode_scroll_segment (0x1374f, blit_2d [L] — an EBP-frame callee: it reads/updates
 * the window fields on this frame). Shaded blits remap through table (base|pixel) where base =
 * [0x85d08]+sel (sel<=0xff00) or [0x86d28]+(sel>>8) — the tables are 256-aligned and the asm
 * splices the pixel into BL, reproduced bit-exactly. Frame layout (offsets from `f`):
 *   +0 win base, +4 win read ptr, +8 avail, +0xc rows, +0x10 width, +0x14 RLE cursor,
 *   +0x1c row advance, +0x20 width copy, +0x24 total px, +0x28 mode, +0x2c shade, +0x30 fill n */
void blit_das_image_to_buffer(uint32_t img, uint32_t dst, uint32_t ebx_in, uint32_t ecx_in)
{
    uint32_t width = R16(img + 4);
    uint32_t fsz = width + 0x354;
    uint8_t framebuf[fsz] __attribute__((aligned(16)));
    uint32_t f = (uint32_t)(uintptr_t)framebuf;
    R32(f + 0x00) = f + 0x34;
    R32(f + 0x04) = f + 0x34;
    R32(f + 0x2c) = ecx_in & 0xffff00;
    R32(f + 0x28) = ecx_in & 0xff;
    R32(f + 0x08) = 0;
    R32(f + 0x0c) = R16(img + 6);
    R32(f + 0x10) = width;
    uint8_t fl = R8(img);
    uint32_t esi = img;
    if (fl & 4) esi += 8;
    if (!(fl & 2)) esi += 0x300;
    R32(f + 0x20) = width;
    R32(f + 0x1c) = ebx_in - width;
    R32(f + 0x24) = width * (uint32_t)R32(f + 0x0c);
    esi += 8;
    R32(f + 0x14) = esi;
    uint32_t edi = dst;
    uint32_t mode = R32(f + 0x28);

/* re-pointed rle_decode_scroll_segment (0x1374f): EBP-frame leaf — reads only [ebp+…]; the
 * old bridge's io_.eax=[f+8] was redundant (the callee reloads it via `mov ecx,[ebp+8]`). d = f. */
#define DA_BLIT_ENSURE()                                                     \
    do { if ((uint32_t)R32(f + 8) < (uint32_t)R32(f + 0x10)) {               \
        rle_decode_scroll_segment((uint8_t *)(uintptr_t)f);           \
    } } while (0)
#define DA_BLIT_TAKEROW()                                                    \
    (esi = R32(f + 4), R32(f + 8) -= R32(f + 0x10), R32(f + 4) += R32(f + 0x10))

    if ((uint32_t)R32(f + 0x1c) > 0xfff) {
        /* variant B — packed EBX: one decoded byte per row fills [f+0x30] pixels */
        R32(f + 0x30) = (uint32_t)R32(f + 0x1c) >> 16;
        R32(f + 0x1c) = (ebx_in & 0xffff) - (uint32_t)R32(f + 0x30);
        if (mode == 2) {                                    /* every-other row */
            uint8_t dl = 0;
            do {
                DA_BLIT_ENSURE(); DA_BLIT_TAKEROW();
                if (dl == 0) {
                    uint8_t al = R8(esi);
                    for (uint32_t n = R32(f + 0x30); n; n--) { R8(edi) = al; edi++; }
                    edi += R32(f + 0x1c);
                }
                dl = (uint8_t)~dl;
                R32(f + 0x0c) -= 1;
            } while ((int32_t)R32(f + 0x0c) > 0);
        } else if (mode == 0) {                             /* doubled rows */
            do {
                DA_BLIT_ENSURE(); DA_BLIT_TAKEROW();
                uint8_t al = R8(esi);
                for (uint32_t n = R32(f + 0x30); n; n--) { R8(edi) = al; edi++; }
                edi += R32(f + 0x1c);
                for (uint32_t n = R32(f + 0x30); n; n--) { R8(edi) = al; edi++; }
                edi += R32(f + 0x1c);
                R32(f + 0x0c) -= 1;
            } while ((int32_t)R32(f + 0x0c) > 0);
        } else {                                            /* 1:1 */
            do {
                DA_BLIT_ENSURE(); DA_BLIT_TAKEROW();
                uint8_t al = R8(esi);
                for (uint32_t n = R32(f + 0x30); n; n--) { R8(edi) = al; edi++; }
                edi += R32(f + 0x1c);
                R32(f + 0x0c) -= 1;
            } while ((int32_t)R32(f + 0x0c) > 0);
        }
        return;
    }

    if (fl & 1) {                                           /* RLE image */
        if (R32(f + 0x2c) != 0) {                           /* shaded/remapped */
            uint32_t sel = (uint32_t)R32(f + 0x2c);
            uint32_t ebx = (sel <= 0xff00) ? sel + PTRG(0x85d08)
                                           : (sel >> 8) + PTRG(0x86d28);
            if (mode == 2) {
                uint8_t dl = 0;
                do {
                    DA_BLIT_ENSURE(); DA_BLIT_TAKEROW();
                    if (dl == 0) {
                        for (int32_t n = (int32_t)R32(f + 0x10); n > 0; n--) {
                            ebx = (ebx & ~0xffu) | R8(esi); esi++;
                            R8(edi) = R8(ebx); edi++;
                        }
                        edi += R32(f + 0x1c);
                    }
                    dl = (uint8_t)~dl;
                    R32(f + 0x0c) -= 1;
                } while ((int32_t)R32(f + 0x0c) > 0);
            } else if (mode == 0) {                         /* doubled: write both rows */
                do {
                    DA_BLIT_ENSURE(); DA_BLIT_TAKEROW();
                    uint32_t full = R32(f + 0x1c) + R32(f + 0x20);   /* full stride */
                    for (int32_t n = (int32_t)R32(f + 0x10); n > 0; n--) {
                        ebx = (ebx & ~0xffu) | R8(esi); esi++;
                        uint8_t px = R8(ebx);
                        R8(edi) = px; R8(edi + full) = px; edi++;
                    }
                    edi += R32(f + 0x1c);
                    R32(f + 0x0c) -= 1;
                } while ((int32_t)R32(f + 0x0c) > 0);
            } else {
                do {
                    DA_BLIT_ENSURE(); DA_BLIT_TAKEROW();
                    for (int32_t n = (int32_t)R32(f + 0x10); n > 0; n--) {
                        ebx = (ebx & ~0xffu) | R8(esi); esi++;
                        R8(edi) = R8(ebx); edi++;
                    }
                    edi += R32(f + 0x1c);
                    R32(f + 0x0c) -= 1;
                } while ((int32_t)R32(f + 0x0c) > 0);
            }
            return;
        }
        if (mode == 2) {                                    /* RLE, every-other row */
            uint8_t dl = 0;
            do {
                DA_BLIT_ENSURE(); DA_BLIT_TAKEROW();
                if (dl == 0) {
                    for (uint32_t n = R32(f + 0x10); n; n--) { R8(edi) = R8(esi); esi++; edi++; }
                    edi += R32(f + 0x1c);
                }
                dl = (uint8_t)~dl;
                R32(f + 0x0c) -= 1;
            } while ((int32_t)R32(f + 0x0c) > 0);
            return;
        }
        if (mode == 0) {                                    /* RLE, doubled rows */
            do {
                DA_BLIT_ENSURE(); DA_BLIT_TAKEROW();
                uint32_t s2 = esi;
                for (uint32_t n = R32(f + 0x10); n; n--) { R8(edi) = R8(s2); s2++; edi++; }
                edi += R32(f + 0x1c);
                s2 = esi;
                for (uint32_t n = R32(f + 0x10); n; n--) { R8(edi) = R8(s2); s2++; edi++; }
                edi += R32(f + 0x1c);
                R32(f + 0x0c) -= 1;
            } while ((int32_t)R32(f + 0x0c) > 0);
            return;
        }
        do {                                                /* RLE, 1:1 */
            DA_BLIT_ENSURE(); DA_BLIT_TAKEROW();
            for (uint32_t n = R32(f + 0x10); n; n--) { R8(edi) = R8(esi); esi++; edi++; }
            edi += R32(f + 0x1c);
            R32(f + 0x0c) -= 1;
        } while ((int32_t)R32(f + 0x0c) > 0);
        return;
    }

    /* plain (non-RLE) image: raw rows from the payload */
    esi = R32(f + 0x14);
    if (mode != 0) {                                        /* 1:1 (modes 1 and 2 identical) */
        do {
            for (uint32_t n = R32(f + 0x10); n; n--) { R8(edi) = R8(esi); esi++; edi++; }
            edi += R32(f + 0x1c);
            R32(f + 0x0c) -= 1;
        } while ((int32_t)R32(f + 0x0c) > 0);
        return;
    }
    do {                                                    /* doubled rows */
        uint32_t row = esi;
        for (uint32_t n = R32(f + 0x10); n; n--) { R8(edi) = R8(esi); esi++; edi++; }
        edi += R32(f + 0x1c);
        esi = row;
        for (uint32_t n = R32(f + 0x10); n; n--) { R8(edi) = R8(esi); esi++; edi++; }
        edi += R32(f + 0x1c);
        R32(f + 0x0c) -= 1;
    } while ((int32_t)R32(f + 0x0c) > 0);
#undef DA_BLIT_ENSURE
#undef DA_BLIT_TAKEROW
}

/* blit_das_image_auto_scale (0x18e48, 32 B) — swap EAX/EDX and pick the row mode from the
 * 400-line flag ([0x90cbd] != 0 -> mode 0 = doubled rows, else 1). EAX=dest, EDX=image,
 * EBX = stride passthrough (gotcha H2: callers leave the pitch in EBX). */
void blit_das_image_auto_scale(uint32_t dst, uint32_t img, uint32_t ebx_in)
{
    uint32_t mode = (G8(VA_g_hires_line_doubling_flag) != 0) ? 0u : 1u;
    blit_das_image_to_buffer(img, dst, ebx_in, mode);
}

/* blit_das_image_at_xy (0x1a10a, 40 B) — EAX=image, EDX=x, EBX=y: resolve the framebuffer ptr
 * (screen_xy_to_framebuffer_ptr [L]) with EBX=pitch [0x85498], then fall into auto_scale's
 * body (flow_succ 0x18e5c). */
void blit_das_image_at_xy(uint32_t img, int32_t x, int32_t y)
{
    uint32_t mode = (G8(VA_g_hires_line_doubling_flag) != 0) ? 0u : 1u;
    uint8_t *fb = screen_xy_to_framebuffer_ptr(x, y);
    blit_das_image_to_buffer(img, (uint32_t)(uintptr_t)fb, (uint32_t)G32(VA_g_screen_pitch), mode);
}

/* blit_reloc_das_image (0x18e68, 54 B) — blit an image stored in the UI scratch buffer
 * ([0x7f56c]) addressed by a RELATIVE offset slot: EDX = byte offset of the dword (relative to
 * the buffer base) holding the image's offset; EAX = dest. No-op when the buffer is absent.
 * EBX = stride passthrough (H2). */
void blit_reloc_das_image(uint32_t dst, uint32_t off_slot, uint32_t ebx_in)
{
    uint32_t base = PTRG(0x7f56c);
    if (base == 0) return;
    uint32_t mode = (G8(VA_g_hires_line_doubling_flag) != 0) ? 0u : 1u;
    uint32_t img = base + R32(off_slot + base);
    blit_das_image_to_buffer(img, dst, ebx_in, mode);
}

/* rle_compress_byterun1 (0x3cf30, 86 B) — ByteRun1-encode ONE row of [0x85498] (pitch) bytes
 * from ESI to EDI: runs of >= 3 (or an initial run of exactly 2 with no pending literals)
 * become {-(len-1), value}; literals accumulate (max 0x80) and flush through
 * emit_literal_run_3cf86 [L]. Runs cap at 0x80 bytes and at the remaining count. */
void rle_compress_byterun1(uint32_t esi, uint32_t edi)
{
    uint32_t lits = 0;                                  /* ebx — pending literal count */
    int32_t remain = G32(VA_g_screen_pitch);                      /* ecx */
    for (;;) {
        uint32_t run = 0;                               /* edx */
        uint8_t al = R8(esi);
        while (R8(esi) == al) {
            if (run >= (uint32_t)remain) break;         /* jae — unsigned */
            esi++; run++;
            if ((uint8_t)run >= 0x80) break;
        }
        esi -= run;
        if ((lits == 0 && run == 2) || run >= 3) {      /* emit a run */
            edi = (uint32_t)(uintptr_t)emit_literal_run_3cf86(
                      lits, (uint8_t *)(uintptr_t)esi, (uint8_t *)(uintptr_t)edi);
            lits = 0;
            R8(edi) = (uint8_t)-(int8_t)((uint8_t)run - 1); edi++;
            R8(edi) = al; edi++;
            esi += run;
            remain -= (int32_t)run;
            if (remain > 0) continue;                   /* jg */
            return;
        }
        /* literal byte */
        lits++; esi++; remain--;
        if (remain == 0) {                              /* input exhausted: flush */
            edi = (uint32_t)(uintptr_t)emit_literal_run_3cf86(
                      lits, (uint8_t *)(uintptr_t)esi, (uint8_t *)(uintptr_t)edi);
            return;                                     /* sub ecx,0; jg fails */
        }
        if ((uint8_t)lits >= 0x80) {                    /* literal buffer full: flush */
            edi = (uint32_t)(uintptr_t)emit_literal_run_3cf86(
                      lits, (uint8_t *)(uintptr_t)esi, (uint8_t *)(uintptr_t)edi);
            lits = 0;
        }
        if (remain > 0) continue;
        return;
    }
}

/* advance_das_sprite_animation_frame (0x38fec, 167 B) — the render-driven animation step for a
 * loaded animated block (ESI): tick-gated ([blk+0x10] vs g_das_cache_tick 0x90c0a) frame-timer
 * countdown by the frame-time scale [0x85324], then advance the frame (wrap at [blk+0x16]) and
 * apply the OLD frame's delta stream ([blk + old*4 + 0x1c]) to the current-frame pixels.
 * Publishes the frame ptr in g_active_das_frame_ptr 0x84980. Negative frame numbers: -1 arms a
 * restart (frame 0 + timer reload unless rate bit7), others just republish the ptr. */
void advance_das_sprite_animation_frame(uint32_t blk)
{
    if (R16(blk + 0x16) == 0xfffe) return;              /* static image */
    if ((int16_t)R16(blk + 0x18) < 0) {
        if (R16(blk + 0x18) != 0xffff)
            goto set_ptr;
        R16(blk + 0x18) = 0;
        uint8_t al = R8(blk + 0x1b);
        if (!(al & 0x80)) {
            R8(blk + 0x1a) = al;
            goto set_ptr;
        }
        goto tick;                                      /* js 0x39025 */
    }
tick:
    if (R8(blk + 0x1b) != 0xff) {                       /* rate 0xff = advance every call */
        uint16_t now = R16(GADDR(VA_g_das_cache_tick));
        if (R16(blk + 0x10) == now) goto set_ptr;       /* once per tick */
        R16(blk + 0x10) = now;
        R8(blk + 0x1a) -= (uint8_t)G32(VA_g_frame_time_scale);        /* timer -= frame-time scale */
        if (R8(blk + 0x1a) < 0x10) goto set_ptr;        /* jb — not yet due */
        uint8_t rate = (uint8_t)((R8(blk + 0x1b) & 0x1f) + 1);
        uint8_t t = (uint8_t)(R8(blk + 0x1a) + rate);
        R8(blk + 0x1a) = t;
        if (t & 0x80) R8(blk + 0x1a) = 0;               /* jns / clamp */
    }
    {                                                   /* 0x39059 — advance */
        uint16_t oldf = R16(blk + 0x18);
        uint16_t newf = (uint16_t)(oldf + 1);
        if ((int16_t)newf >= (int16_t)R16(blk + 0x16)) newf = 0;
        R16(blk + 0x18) = newf;
        uint32_t framep = blk + R16(blk + 0x14) + 0x10;
        PTRG(0x84980) = framep;
        uint32_t delta = R32(blk + (uint32_t)oldf * 4 + 0x1c);
        if (delta != 0)
            apply_das_sprite_frame_delta_stream((void *)(uintptr_t)framep,
                                                       (void *)(uintptr_t)(blk + delta));
        return;
    }
set_ptr:
    PTRG(0x84980) = blk + R16(blk + 0x14) + 0x10;
}

/* decode_das_anim_frame (0x2c5c5, 343 B) — decode animation frame AX of the animated record
 * EDX into its secondary block's pixel buffer (*[rec+0x10] + 0x10): walk the frame headers
 * (magic 0x17 at rec+0x1a, next += [hdr+0xc]), zero the leading margin (top*stride + left),
 * RLE-decode [hdr+0x12]-wide spans for ([hdr+6] - bottom - top) rows with a per-row gap of
 * (stride - span), zero the trailing margin. Degenerate frames (bottom==1 or span==1) clear
 * width*stride... whole-frame instead. Stores the (clamped) frame into rec+0x18 and the
 * clamped index into the anim-skip global 0x84944 (harness key D1). No-op when rec+0x18
 * already == AX or the magic is wrong. */
uint32_t decode_das_anim_frame(uint32_t ax_in, uint32_t rec)
{
    uint16_t ax = (uint16_t)ax_in;
    /* Both original exit paths return EAX = [[rec+0x10]] (the resolved frame/secondary-block ptr):
     * 0x2c70c no-op path does `mov edx,[edx+0x10]; mov eax,[edx]`, and the main path pushes that
     * same edi at 0x2c5e0 (before edi+=0x10) and pops it into eax at 0x2c704. */
    if (R16(rec + 0x18) == ax) {          /* je 0x2c70c: the no-op path still PUBLISHES the */
        PTRG(0x84944) = ax;               /* frame index to the anim-skip global */
        return R32(R32(rec + 0x10));      /* eax = [[rec+0x10]] */
    }
    R16(rec + 0x18) = ax;
    uint32_t frame_ptr = R32(R32(rec + 0x10));          /* pushed edi = the EAX return */
    uint32_t edi = frame_ptr;                           /* secondary block (handle deref) */
    uint32_t esi = rec + 0x1a;
    if (R16(esi) != 0x17) return frame_ptr;             /* jne 0x2c704: pop eax = frame ptr */
    if ((int16_t)ax >= (int16_t)R16(esi + 8))
        ax = (uint16_t)(R16(esi + 8) - 1);              /* clamp to count-1 */
    uint32_t idx = ax;
    PTRG(0x84944) = idx;                                /* the anim-skip global */
    for (uint32_t n = idx; n != 0; n--)
        esi += R32(esi + 0xc);                          /* walk to frame idx */
    edi += 0x10;                                        /* dest pixels */

    uint32_t bot  = R16(esi + 0x16);                    /* ebx */
    uint32_t span = R16(esi + 0x12);                    /* ebp */
    uint32_t tail_clear;
    if (bot == 1 || span == 1) {
        tail_clear = (uint32_t)(uint16_t)(R16(esi + 6) * R16(esi + 4));
    } else {
        uint32_t lead = (uint32_t)(uint16_t)(R16(esi + 0x14) * R16(esi + 4) + R16(esi + 0x10));
        if (lead != 0) {                                /* zero the leading margin */
            for (uint32_t n = lead; n; n--) { R8(edi) = 0; edi++; }
        }
        uint16_t rows16 = (uint16_t)(R16(esi + 6) - (uint16_t)bot - R16(esi + 0x14));
        uint16_t acc = rows16;
        if (acc != 0) acc = (uint16_t)(acc * R16(esi + 4));
        uint32_t gap = (uint32_t)(uint16_t)(R16(esi + 4) - (uint16_t)span);
        acc = (uint16_t)(acc - R16(esi + 0x10));
        tail_clear = (uint16_t)(acc + (uint16_t)gap);
        uint32_t s = esi + 0x18;                        /* RLE stream */
        if (gap != 0) {
            int32_t rows = (int32_t)bot;                /* ebx counts the row loop */
            do {                                        /* 0x2c680 */
                int32_t n = (int32_t)span;
                while (n > 0) {                         /* RLE-decode one span */
                    uint8_t al = R8(s); s++;
                    if (al < 0xf1) { R8(edi) = al; edi++; n--; }
                    else {
                        uint32_t cnt = (uint32_t)(uint8_t)(al - 0xf0);
                        n -= (int32_t)cnt;
                        al = R8(s); s++;
                        for (; cnt; cnt--) { R8(edi) = al; edi++; }
                    }
                }
                if (rows == 1) goto tail;               /* last row: no gap fill */
                for (uint32_t g = gap; g; g--) { R8(edi) = 0; edi++; }
                rows--;
            } while (rows > 0);
        } else {
            int32_t rows = (int32_t)bot;
            do {                                        /* 0x2c6c3 — full-width rows */
                int32_t n = (int32_t)span;
                while (n > 0) {
                    uint8_t al = R8(s); s++;
                    if (al < 0xf1) { R8(edi) = al; edi++; n--; }
                    else {
                        uint32_t cnt = (uint32_t)(uint8_t)(al - 0xf0);
                        n -= (int32_t)cnt;
                        al = R8(s); s++;
                        for (; cnt; cnt--) { R8(edi) = al; edi++; }
                    }
                }
                rows--;
            } while (rows > 0);
        }
    }
tail:
    if (tail_clear != 0)                                /* zero the trailing margin */
        for (uint32_t n = tail_clear; n; n--) { R8(edi) = 0; edi++; }
    return frame_ptr;                                   /* pop eax at 0x2c704 = frame ptr */
}

/* rescale_das_frame (0x1384d, 339 B) — post-load horizontal rescale of a cached dbase200 DAS
 * sprite for the current render scale ([0x85404] vs the 0x9b=155 reference): for each
 * sub-frame, scale the span width ((w * scale) / 0x9b, bail entirely on upscale) and resample
 * every row IN PLACE through a stack row buffer with the 16.16 step 0x9b0000/scale. Row
 * descriptors are {word dest-skip, word count} dwords, one per row, ahead of the packed
 * pixels; empty rows (0) pass through. Tags the frame with the scale ([rec] = scale). */
void rescale_das_frame(uint32_t rec)
{
    uint8_t rowbuf[0x348 - 0x24] __attribute__((aligned(16)));   /* the [ebp+0x24..] row window */
    uint32_t scale = PTRG(0x85404);
    if (R32(rec) == scale) return;
    R32(rec) = scale;
    if (scale == 0x9b) return;
    if (scale > 0x9b) {
        scale >>= 1;
        if (scale >= 0x9b) return;
    }
    uint32_t esi = rec + 4;
    uint32_t frames = 0;
    if (R16(esi) & 0x10) frames = R16(esi + 8);
    do {                                                /* per sub-frame (runs once if 0) */
        uint32_t hdr = esi;
        uint16_t flags = R16(esi);
        esi += 8;
        if (flags & 0x10) esi += 8;
        uint32_t rows  = (uint32_t)R16(esi + 6);
        uint32_t width = (uint32_t)R16(esi + 2);
        uint64_t scaled = (uint64_t)width * scale / 0x9b;   /* mul/div — 64-bit unsigned */
        if ((uint32_t)scaled > width) return;               /* upscale/overflow: bail ALL */
        R16(esi + 2) = (uint16_t)scaled;
        uint32_t step = (uint32_t)((int64_t)0x9b0000 / (int32_t)scale);   /* idiv */
        if (flags & 4) esi += 8;
        if (!(flags & 2)) esi += 0x300;
        uint32_t tab = esi;                              /* row-descriptor table (dwords) */
        uint32_t src = tab + rows * 4;                   /* packed pixels */
        uint32_t wr  = src;                              /* in-place write cursor (edi) */
        int32_t r = (int32_t)rows;
        do {                                             /* dec edx; jg — BOTTOM-tested: rows==0
                                                            still processes one row (faithful) */
            uint32_t desc = R32(tab);
            if (desc == 0) goto next_row;
            uint32_t skip = desc & 0xffff;
            uint32_t cnt  = desc >> 16;
            memset(rowbuf, 0, ((width + 3) >> 2) * 4);   /* rep stosd — dword-granular zero */
            uint32_t up = (uint32_t)(uintptr_t)rowbuf + skip;
            for (uint32_t n = cnt; n; n--) { R8(up) = R8(src); src++; up++; }
            uint32_t end = up;                           /* edx — end of unpacked data */
            uint32_t scan = (uint32_t)(uintptr_t)rowbuf;
            uint32_t newskip = 0, frac = 0, newcnt = 0;
            for (;;) {                                   /* leading-zero scan */
                if (R8(scan) != 0) break;
                newskip++;
                frac += step;
                scan += frac >> 16;
                frac &= 0xffff;
                if ((int32_t)scan >= (int32_t)end) {     /* jl — signed ptr compare */
                    newskip = 0; newcnt = 0;
                    goto store;
                }
            }
            do {                                         /* resample the run in place */
                R8(wr) = R8(scan); wr++; scan++;
                newcnt++;
                frac += step;
                scan += (frac >> 16) - 1;
                frac &= 0xffff;
            } while ((int32_t)scan < (int32_t)end);
store:
            R16(tab)     = (uint16_t)newskip;
            R16(tab + 2) = (uint16_t)newcnt;
next_row:
            tab += 4;
            r--;
        } while (r > 0);
        esi = hdr + R32(hdr + 0xc);                      /* next sub-frame */
        frames--;
    } while ((int32_t)frames > 0);
}

/* backdrop_refill_buffer (0x4b30b, 23 B) — one DOS int21 AH=3F read: [d+0x30] bytes from file
 * handle [d+0] into the read buffer [d+0x2c]; stores the returned byte count into [d+0x3c].
 * RAW inline int 0x21 -> the g_os_soft_int host hook; NULL-guarded so the
 * TU links into the oracle (the oracle test installs a mock g_os_soft_int). EBP=d. */
void backdrop_refill_buffer(uint32_t d)
{
    regs_t r; memset(&r, 0, sizeof r);
    r.ecx = R32(d + 0x30);
    r.edx = R32(d + 0x2c);
    r.ebx = R32(d + 0x00);
    r.eax = 0x3f00;                          /* mov ah,0x3f (AL = don't-care for the handler) */
    if (g_os_soft_int) g_os_soft_int(0x21, &r);
    R32(d + 0x3c) = r.eax;                   /* bytes read (0 = EOF) */
}

/* backdrop_decode_rle (0x4b27d, 142 B) — RLE-decode the next window into the decode buffer:
 * literal if byte < 0xf1, else a run of (byte-0xf0) copies of the following byte; pulls raw
 * file bytes via backdrop_refill_buffer across chunk boundaries. Moves the unconsumed decoded
 * tail to the buffer start first (rep movsd of (avail>>2)+1 dwords — the original over-copies
 * up to the dword granularity; kept byte-exact). EBP=d.
 *
 * FAITHFUL QUIRK: on the run path the value byte is PRE-READ at [esi] BEFORE the input-credit
 * check (0x4b2df); if the refill then fires, the code does NOT re-read it — the run emits the
 * stale pre-read byte and `inc esi` skips the first byte of the fresh chunk. Reproduced as-is. */
void backdrop_decode_rle(uint32_t d)
{
    uint32_t avail = R32(d + 0x34);
    uint32_t base  = R32(d + 0x28);
    uint32_t src   = R32(d + 0x38);
    R32(d + 0x38) = base;
    if (avail != 0) {                        /* move the leftover tail to the buffer start */
        uint32_t n = (avail >> 2) + 1;       /* rep movsd — dword count, over-copies the tail */
        for (uint32_t i = 0; i < n; i++)
            R32(base + i * 4) = R32(src + i * 4);
    }
    uint32_t edi = R32(d + 0x38) + R32(d + 0x34);   /* dest = base + leftover */
    uint32_t esi = R32(d + 0x4c);                   /* RLE input stream ptr */
    R32(d + 0x34) = 0x1000 - 0x20;                  /* window budget 0xfe0 */
    int32_t ebx = (int32_t)(0x1000u - 0x20u - avail);
    uint8_t al;

    for (;;) {                                       /* 0x4b2b0 */
        int32_t credit = (int32_t)R32(d + 0x3c) - 1;
        R32(d + 0x3c) = (uint32_t)credit;
        if (credit < 0) {                            /* input exhausted -> refill */
            backdrop_refill_buffer(d);
            esi = R32(d + 0x2c);
            if (R32(d + 0x3c) == 0) break;           /* EOF */
            R32(d + 0x3c) -= 1;
        }
        al = R8(esi); esi++;
        if (al < 0xf1) {                             /* literal byte */
            R8(edi) = al; edi++;
            ebx--;
            if (ebx > 0) continue;                   /* dec ebx; jg loop */
            break;
        }
        /* run: count = al - 0xf0 (1..15), value = next stream byte */
        uint32_t cnt = (uint32_t)(uint8_t)(al - 0xf0);   /* movzx ecx,al; sub cl,0xf0 */
        ebx -= (int32_t)cnt;
        al = R8(esi);                                /* PRE-READ the run value (see header note) */
        credit = (int32_t)R32(d + 0x3c) - 1;
        R32(d + 0x3c) = (uint32_t)credit;
        if (credit < 0) {
            backdrop_refill_buffer(d);
            esi = R32(d + 0x2c);
            if (R32(d + 0x3c) == 0) break;           /* EOF — run NOT emitted */
            R32(d + 0x3c) -= 1;
        }
        esi++;                                       /* inc esi (consume the value byte) */
        for (; cnt != 0; cnt--) { R8(edi) = al; edi++; }   /* rep stosb */
        if (ebx > 0) continue;                       /* or ebx,ebx; jg loop */
        break;
    }
    /* 0x4b300 */
    R32(d + 0x34) -= (uint32_t)ebx;                  /* avail = produced (budget - remaining) */
    R32(d + 0x4c) = esi;                             /* save the stream ptr */
}

/* backdrop_ensure_source_bytes (0x4b269, 20 B) — guarantee >= ECX decoded bytes are buffered
 * (decode more when short), consume them, and return EAX = the source ptr they start at.
 * EBP=d, ECX=count -> EAX. */
uint32_t backdrop_ensure_source_bytes(uint32_t d, uint32_t ecx)
{
    if ((int32_t)ecx > (int32_t)R32(d + 0x34))       /* cmp ecx,[d+0x34]; jle skip */
        backdrop_decode_rle(d);
    R32(d + 0x34) -= ecx;
    uint32_t eax = R32(d + 0x38);
    R32(d + 0x38) += ecx;
    return eax;
}

/* blit_backdrop_row (0x4b1ee, 126 B) — emit output rows from ONE decoded source row: straight
 * byte copy, or per-column sample through the h-scale word table ([d+0x50]) when scaled; steps
 * the vertical 16.16 accumulator, re-emitting the same source row (via the 0x4b1eb re-entry,
 * [d+0x5c]) while the accumulator stays >= step. EAX=src row, EDI=dest (in/out), EBP=d.
 * Returns the ZF postcondition the caller loops on: 1 = all [d+0x44] rows emitted (done). */
int blit_backdrop_row(uint32_t d, uint32_t src, uint32_t *edi_io)
{
    uint32_t edi = *edi_io;
    for (;;) {
        R32(d + 0x5c) = src;                         /* 0x4b1ee (0x4b1eb re-entry re-stores it) */
        if ((int32_t)R32(d + 0x54) < 0) {            /* cmp [d+0x54],0; jl 0x4b226 */
            R32(d + 0x54) += 0x10000;
            *edi_io = edi;
            return 0;                                /* or al,1 -> ZF clear (row consumed, not done) */
        }
        if (R32(d + 0x50) == 0) {                    /* unscaled: copy width bytes (movsd+movsb) */
            uint32_t w = R32(d + 0x40);
            for (uint32_t i = 0; i < w; i++) R8(edi + i) = R8(src + i);
        } else {                                     /* 0x4b230: h-scaled sample via the word table */
            uint32_t t = R32(d + 0x50);
            int32_t n = (int32_t)R32(d + 0x40);
            uint32_t q = edi;
            do {                                     /* dec ecx; jg — do-while (w==0 writes once) */
                uint16_t ix = R16(t); t += 2;
                R8(q) = R8(src + ix); q++;
                n--;
            } while (n > 0);
        }
        edi += R32(d + 0x48);                        /* advance dest by the row stride */
        R32(d + 0x44) -= 1;                          /* output rows remaining */
        if (R32(d + 0x44) == 0) { *edi_io = edi; return 1; }   /* je 0x4b22f -> ZF set (done) */
        int32_t acc  = (int32_t)R32(d + 0x54);
        int32_t step = (int32_t)R32(d + 0x58);
        R32(d + 0x54) = (uint32_t)(acc - step);      /* sub [d+0x54],step */
        if (acc >= step) {                           /* jge 0x4b1eb: repeat the SAME source row */
            src = R32(d + 0x5c);
            continue;
        }
        R32(d + 0x54) += 0x10000;                    /* 0x4b226 */
        *edi_io = edi;
        return 0;                                    /* or al,1 -> ZF clear */
    }
}

/* blit_backdrop_rows (0x4b1b7, 52 B) — the row loop: reset the decode/accumulator state, then
 * for each source row (dx = [d+0x1a]) pull [d+0x18] decoded bytes and blit until the row fn
 * reports done. EBP=d; dest ptr = *[d+0x14] (double deref). */
void blit_backdrop_rows(uint32_t d)
{
    R32(d + 0x54) = 0;
    R32(d + 0x34) = 0;
    R32(d + 0x3c) = 0;
    uint32_t edi = R32(R32(d + 0x14));               /* mov edi,[d+0x14]; mov edi,[edi] */
    int32_t rows = (int32_t)R16(d + 0x1a);           /* dx = source height */
    for (;;) {                                       /* do-while: body runs once even if dx==0 */
        uint32_t need = R16(d + 0x18);               /* cx = source width */
        uint32_t src = backdrop_ensure_source_bytes(d, need);
        if (blit_backdrop_row(d, src, &edi))
            return;                                  /* je 0x4b1ea — all output rows emitted */
        rows--;
        if (rows <= 0) return;                       /* dec edx; jg loop */
    }
}

/* build_backdrop_hscale_table (0x4b322, 50 B) — fill the h-scale table ([d+0x50], [d+0x40]
 * word entries) with source-column indices: 16.16 step = ((srcW<<16)-0x7fff)/destW (unsigned),
 * initial accumulator = (step-1)>>1, entry = accumulator high word. EBP=d; preserves all regs. */
void build_backdrop_hscale_table(uint32_t d)
{
    uint32_t acc  = ((uint32_t)R16(d + 0x18) << 16) - 0x7fff;
    uint32_t step = acc / (uint32_t)R32(d + 0x40);   /* unsigned div (B1) */
    acc = (step - 1) >> 1;                           /* dec eax; shr eax,1 */
    uint32_t t = R32(d + 0x50);
    int32_t n = (int32_t)R32(d + 0x40);
    do {                                             /* dec ecx; jg — do-while */
        R16(t) = (uint16_t)(acc >> 16);              /* ror eax,16; stosw (store the high word) */
        t += 2;
        acc += step;
        n--;
    } while (n > 0);
}
