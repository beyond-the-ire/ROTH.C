/* lift_video_display.c — verified-C lifts for the `video_display` subsystem.
 *
 * The game's mode-setting, page-flip, palette-DAC, and dirty-rectangle *present* layer.
 * See docs/reference/lift/video_display.md.
 *
 * The subsystem splits sharply into two halves:
 *   - PURE, oracle-able bookkeeping/math  — dirty-rect coalescing, palette remap LUTs,
 *     RGB nearest-colour, grayscale LUT, mode-byte math. Verified byte-exact in the oracle.
 *   - IN-GAME-only hardware programming    — VESA/VGA/Mode-X mode set, page flips, bank
 *     switches, DAC uploads via `out 0x3c4/0x3c8/0x3c9` + DPMI physical-map + real-mode int
 *     10h. These are lifted as the surrounding C bookkeeping with the actual hardware side
 *     bridged to the host's display emulation (verified via live-swap `ROTH_LIFT`).
 *
 * ABI / behaviour transcribed STRICTLY FROM THE DISASM (the corpus decompile is a fast path,
 * not the authority — non-EAX returns / carry are dropped). Canon VAs; runtime = canon+OBJ_DELTA.
 *
 * Functions lifted here (grouped by cluster; canon VAs):
 *   -- A. Dirty-rect present pipeline --
 *   add_dirty_rect          0x15b69 — the 624 B bounded-list overlap coalescer (RECURSIVE
 *                                     split/merge; PURE obj3 list math).
 *   register_dirty_rect     0x15b5b — 14 B hires-doubling prefix that flow-falls into
 *                                     add_dirty_rect (top*=2, bottom=bottom*2+1 when hires).
 */
#include <stdint.h>
#include <string.h>
#include "common.h"

/* ===================================================================================
 * A. Dirty-rect present pipeline
 * ===================================================================================
 * The dirty-region list (all obj3, canon addrs):
 *   g_dirty_rect_count  0x7f57c  (int32, MAX 0x40 = 64)
 *   g_dirty_rects       0x7f580  rect[i], stride 0x10:  +0=left +4=top +8=right +0xc=bottom
 *   g_screen_pitch      0x85498  (int32)  — right edge is 8-px aligned + clamped to this
 *   g_screen_height     0x854a0  (int32)  — bottom edge clamped to this
 *   g_hires_line_doubling_flag 0x90cbd (u8) — register_dirty_rect doubles y when set
 */
#define DR_COUNT   G32(VA_g_dirty_rect_count)
/* rect field lvalues: the four dwords live at 0x7f580/+4/+8/+0xc, stride 0x10 (4 int32s). */
static inline volatile int32_t *dr_base(void) { return (volatile int32_t *)GADDR(VA_g_dirty_rects); }
#define DR_L(i) (dr_base()[(i) * 4 + 0])   /* left   (0x7f580) */
#define DR_T(i) (dr_base()[(i) * 4 + 1])   /* top    (0x7f584) */
#define DR_R(i) (dr_base()[(i) * 4 + 2])   /* right  (0x7f588) */
#define DR_B(i) (dr_base()[(i) * 4 + 3])   /* bottom (0x7f58c) */

/* ---------------------- add_dirty_rect (0x15b69) ----------------------
 * ABI: EAX=left, EDX=top, EBX=right, ECX=bottom.
 * Clips/8-px-aligns the x edges, clamps to the screen, then inserts into the bounded rect
 * list keeping it NON-OVERLAPPING: on a Y-overlap with an existing rect it recurses on the
 * top/bottom remainders (splitting itself into disjoint strips) and merges the middle strip
 * horizontally into the existing entry. Faithful to the recursive x86 (the corpus renders
 * this as a triple while(true) with two self-calls). All arithmetic is 32-bit; the edge-mask
 * ops (`and di,0xfff8` / `and al,0xf8`) only ever clear bits 0-2 of a small non-negative
 * coordinate, so `& ~7`/`& ~3` on the full 32-bit value is byte-equivalent. */
void add_dirty_rect(uint32_t p_left, int32_t p_top, uint32_t p_right, uint32_t p_bottom)
{
    int32_t left   = (int32_t)p_left;
    int32_t top    = p_top;
    int32_t right  = (int32_t)p_right;
    int32_t bottom = (int32_t)p_bottom;

    for (;;) {                                   /* outer while(true); recursion tails jmp here */
        int32_t pitch  = G32(VA_g_screen_pitch);
        int32_t height = G32(VA_g_screen_height);

        if (DR_COUNT >= 0x40) return;            /* jge — list full */

        /* 0x15b8f: `cmp (pitch-8),right; jae skip` — unsigned. Left 8-align only when the
         * right edge reaches within 8 px of the pitch. */
        if ((uint32_t)(pitch - 8) < (uint32_t)right)
            left &= (int32_t)0xfffffff8;         /* and di,0xfff8 */
        left &= (int32_t)0xfffffffc;             /* and di,0xfffc (always) */

        /* right = ((right - left + 7) & ~7) - 1 + left  (round the width up to a mult. of 8) */
        right = (((right - left + 7) & (int32_t)0xfffffff8) - 1) + left;

        if ((uint32_t)right >= (uint32_t)pitch)   right  = pitch - 1;   /* jb skip; clamp */
        if ((uint32_t)bottom >= (uint32_t)height) bottom = height - 1;
        if (left < 0) left = 0;
        if (top  < 0) top  = 0;
        if (left >= right)   return;             /* jge — degenerate width */
        if (bottom < top)    return;             /* jl  — degenerate height */

        if (DR_COUNT == 0) {                      /* first rect: store & done */
            DR_T(0) = top;
            DR_COUNT = 1;
            DR_R(0) = right;
            DR_L(0) = left;
            DR_B(0) = bottom;
            return;
        }

        /* scan for the first rect that overlaps in Y */
        int cnt = DR_COUNT;
        int i, found = 0;
        for (i = 0; i < cnt; i++) {
            if (bottom < DR_T(i)) continue;      /* jl  — new is entirely above rect i */
            if (top    > DR_B(i)) continue;      /* jg  — new is entirely below rect i */
            found = 1; break;
        }

        if (!found) {                            /* 0x15da5: no Y-overlap — append as-is */
            int32_t s = DR_COUNT;
            DR_L(s) = left;
            DR_R(s) = right;
            DR_T(s) = top;
            DR_COUNT = s + 1;
            DR_B(s) = bottom;
            return;
        }

        /* --- Y-overlap with rect i --- */
        if (top < DR_T(i)) {                      /* 0x15c5f: new extends above rect i */
            add_dirty_rect((uint32_t)left, top, (uint32_t)right,
                                  (uint32_t)(DR_T(i) - 1));   /* recurse on the top strip */
            top = DR_T(i);
            continue;                            /* jmp 0x15b7a — reprocess the remainder */
        }
        if (bottom > DR_B(i)) {                   /* 0x15c8c: new extends below rect i */
            add_dirty_rect((uint32_t)left, DR_B(i) + 1, (uint32_t)right,
                                  (uint32_t)bottom);          /* recurse on the bottom strip */
            bottom = DR_B(i);
            continue;
        }

        /* 0x15cb6: new is within rect i vertically. If also within horizontally, it's covered. */
        if (left >= DR_L(i)) {                    /* jl skips this when left<rect.left */
            if (right <= DR_R(i)) return;
        }

        /* merge: widen rect i to span both, split off any exposed top/bottom of rect i */
        int32_t merged_right = DR_R(i);
        if (merged_right <= right) merged_right = right;      /* max(rect.right, new right) */
        int32_t merged_left = DR_L(i);
        if (left <= merged_left)   merged_left = left;        /* min(rect.left,  new left)  */

        if (top > DR_T(i)) {                      /* 0x15cf4: rect i pokes above — keep that strip */
            int32_t s = DR_COUNT;
            DR_L(s) = DR_L(i);
            DR_R(s) = DR_R(i);
            DR_T(s) = DR_T(i);
            DR_COUNT = s + 1;
            DR_B(s) = top - 1;
        }
        if (bottom < DR_B(i)) {                   /* 0x15d41: rect i pokes below — keep that strip */
            int32_t s = DR_COUNT;                 /* re-read: may have grown above */
            DR_L(s) = DR_L(i);
            DR_R(s) = DR_R(i);
            DR_T(s) = bottom + 1;
            DR_COUNT = s + 1;
            DR_B(s) = DR_B(i);
        }

        /* rewrite rect i as the merged middle strip */
        DR_L(i) = merged_left;
        DR_R(i) = merged_right;
        DR_T(i) = top;
        DR_B(i) = bottom;
        return;
    }
}

/* ---------------------- register_dirty_rect (0x15b5b) ----------------------
 * 14-byte prefix: when g_hires_line_doubling_flag is set, double the y extent
 * (top*=2, bottom=bottom*2+1), then flow-fall into add_dirty_rect. Same ABI. */
void register_dirty_rect(uint32_t left, int32_t top, uint32_t right, uint32_t bottom)
{
    if (G8(VA_g_hires_line_doubling_flag) != 0) {              /* cmp byte [0x90cbd],0; jne */
        bottom = bottom * 2 + 1;         /* add ecx,ecx; ...; inc ecx */
        top    = top * 2;                /* add edx,edx */
    }
    add_dirty_rect(left, top, right, bottom);
}

/* ===================================================================================
 * B. Palette DAC + RGB remap
 * ===================================================================================
 * g_palette_rgb_ptr  0x85488  STORED POINTER (A4) -> 256*3-byte palette (6-bit R/G/B)
 * g_view_grayscale_lut       0x7f254  256-byte grayscale-ramp LUT
 * g_view_grayscale_lut_valid 0x7f354  (u8) 1 once built
 */

/* ---------------------- find_nearest_palette_color (0x20437) ----------------------
 * EAX=table (u16*, stride 3 = expanded R/G/B), EDX=R, EBX=G, ECX=B, [stack]=count.
 * Minimal Manhattan distance |dR|+|dG|+|dB| over `count` entries; an exact match (dist 0)
 * short-circuits returning the CURRENT index, otherwise the best index. Return AL. (ret 4) */
uint8_t find_nearest_palette_color(const uint16_t *table, int32_t r, int32_t g,
                                          int32_t b, int32_t count)
{
    int32_t best_dist = 0x1ffff;
    uint8_t best_idx = 0;
    const uint16_t *p = table;
    for (int32_t i = 0; i < count; i++) {
        int32_t dr = (int32_t)p[0] - r; dr = (dr ^ (dr >> 31)) - (dr >> 31);   /* cdq;xor;sub = abs */
        int32_t dg = (int32_t)p[1] - g; dg = (dg ^ (dg >> 31)) - (dg >> 31);
        int32_t db = (int32_t)p[2] - b; db = (db ^ (db >> 31)) - (db >> 31);
        int32_t dist = dr + dg + db;
        p += 3;
        if (dist == 0) return (uint8_t)i;                          /* exact -> current index */
        if (dist < best_dist) { best_dist = dist; best_idx = (uint8_t)i; }
    }
    return best_idx;
}

/* ---------------------- find_nearest_palette_index (0x2ffae) ----------------------
 * EAX=idx (target colour = g_palette_rgb_ptr[idx]), EDX=table (256-entry RGB, stride 3, scanned).
 * Weighted distance 4*|dG| + 2*|dR| + |dB|; return AL = best index (0..255). If table==NULL
 * return idx&0xff. Preserves all regs except EAX. */
uint8_t find_nearest_palette_index(uint32_t idx, const uint8_t *table)
{
    if (table == 0) return (uint8_t)idx;
    const uint8_t *seed = (const uint8_t *)(uintptr_t)(idx * 3 + (uint32_t)G32(VA_g_palette_rgb_ptr));
    int32_t seedR = seed[0], seedG = seed[1], seedB = seed[2];
    uint32_t best = 0x186a0;                       /* 100000 */
    uint8_t best_idx = 0;
    for (int i = 0; i < 256; i++) {
        int32_t dg = (int32_t)table[i * 3 + 1] - seedG; if (dg < 0) dg = -dg;
        int32_t dist = dg * 4;
        int32_t dr = (int32_t)table[i * 3 + 0] - seedR; if (dr < 0) dr = -dr;
        dist += dr + dr;
        int32_t db = (int32_t)table[i * 3 + 2] - seedB; if (db < 0) db = -db;
        dist += db;
        if ((uint32_t)dist <= best) { best = (uint32_t)dist; best_idx = (uint8_t)i; }
    }
    return best_idx;
}

/* ---------------------- build_view_grayscale_lut (0x12be2) ----------------------
 * void: for each of 256 g_palette_rgb_ptr entries (6-bit R/G/B) compute a green-weighted
 * luminance ((R*4 + B + G*8) * 0x500 >> 16) and store -(lum - 0x4f) into g_view_grayscale_lut;
 * set g_view_grayscale_lut_valid. Preserves all registers. */
void build_view_grayscale_lut(void)
{
    const uint8_t   *pal = (const uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_palette_rgb_ptr);   /* A4 stored ptr */
    volatile uint8_t *lut = (volatile uint8_t *)GADDR(VA_g_view_grayscale_lut);
    for (int i = 0; i < 0x100; i++) {
        int32_t r = pal[i * 3 + 0] & 0x3f;
        int32_t g = pal[i * 3 + 1] & 0x3f;
        int32_t b = pal[i * 3 + 2] & 0x3f;
        int32_t lum = ((r * 4 + b + g * 8) * 0x500) >> 16;
        lut[i] = (uint8_t)(-(lum - 0x4f));
    }
    G8(VA_g_view_grayscale_lut_valid) = 1;
}

/* ---------------------- remap_pixels_to_palette (0x204a3) ----------------------
 * EAX=src_pal (128 entries, 6-bit RGB), EDX=dst_pal (256 entries), EBX=out buffer,
 * ECX=index stream, [stack]=count (ret 8; a 2nd stack arg is dead). Remaps `count` source
 * palette indices from dst_pal's space into src_pal's 128-colour space: expand both palettes
 * (expand_rgb [L]), DOUBLE the src table, then for each stream byte look up its dst_pal RGB and
 * find the nearest src_pal entry (find_nearest_palette_color), caching per source index. All
 * scratch is on the local frame; the ONLY external write is the output buffer. */
void remap_pixels_to_palette(const uint8_t *src_pal, const uint8_t *dst_pal,
                                    uint8_t *out, const uint8_t *stream, int32_t count)
{
    uint16_t src_exp[128 * 3];              /* [ebp-0x50c]: expanded 128-entry src palette */
    uint16_t dst_exp[256 * 3];              /* [ebp-0xb0c]: expanded 256-entry dst palette */
    uint16_t cache[256];                    /* [ebp-0x20c]: per-source-index result cache */

    expand_rgb((uint8_t *)src_pal, src_exp, 0x80);
    expand_rgb((uint8_t *)dst_pal, dst_exp, 0x100);

    for (int i = 0; i < 0x180; i++) src_exp[i] = (uint16_t)(src_exp[i] << 1);  /* double 384 words */
    for (int i = 0; i < 256; i++)   cache[i] = 0xffff;                          /* mem_fill 0xff */

    for (int32_t n = 0; n < count; n++) {
        uint8_t sidx = *stream++;
        uint16_t cached = cache[sidx];
        if (cached == 0xffff) {
            int32_t R = dst_exp[sidx * 3 + 0];
            int32_t G = dst_exp[sidx * 3 + 1];
            int32_t B = dst_exp[sidx * 3 + 2];
            uint8_t idx = find_nearest_palette_color(src_exp, R, G, B, 0x80);
            cached = idx;
            cache[sidx] = idx;
        }
        *out++ = (uint8_t)cached;
    }
}

/* ---------------------- remap_rgb_to_palette_indices (0x301eb) ----------------------
 * EAX=count, EDX=src (3 bytes/pixel RGB), EBX=out (1 index/pixel). Builds a weighted-scaled copy
 * of the live 256-entry palette block ([0x90bca]<<4, a real-mode paragraph pointer into low DOS
 * memory; weights R*0x28/G*0x40/B*0x18) in a 768-short local table, then for each source pixel
 * (scaled R*0xa/G*0x10/B*6) picks the ref index (1..255; index 0 is SKIPPED) minimising the summed
 * 16-bit weighted abs-difference, early-out on an exact match. Register-transparent (pushal/popal);
 * only external write = the output buffer. */
void remap_rgb_to_palette_indices(int32_t count, const uint8_t *src, uint8_t *out)
{
    uint16_t ref[256 * 3];                                          /* local scaled palette table */
    const uint8_t *pal = (const uint8_t *)(uintptr_t)((uint32_t)G16(VA_g_vel_queue_b + 0x88) << 4);  /* low-mem block */
    for (int i = 0; i < 256; i++) {
        ref[i * 3 + 0] = (uint16_t)(pal[i * 3 + 0] * 0x28);
        ref[i * 3 + 1] = (uint16_t)(pal[i * 3 + 1] * 0x40);
        ref[i * 3 + 2] = (uint16_t)(pal[i * 3 + 2] * 0x18);
    }
    for (int32_t n = 0; n < count; n++) {
        int32_t sR = src[0] * 0xa, sG = src[1] * 0x10, sB = src[2] * 6;
        src += 3;
        uint16_t best = 0x7fff;
        uint8_t best_idx = 0;
        for (uint8_t idx = 1; idx != 0; idx++) {                    /* dl = 1..255, entry 0 skipped */
            int16_t dR = (int16_t)((uint16_t)(uint32_t)sR - ref[idx * 3 + 0]); if (dR < 0) dR = (int16_t)-dR;
            int16_t dG = (int16_t)((uint16_t)(uint32_t)sG - ref[idx * 3 + 1]); if (dG < 0) dG = (int16_t)-dG;
            int16_t dB = (int16_t)((uint16_t)(uint32_t)sB - ref[idx * 3 + 2]); if (dB < 0) dB = (int16_t)-dB;
            uint16_t dist = (uint16_t)((uint16_t)dR + (uint16_t)dG + (uint16_t)dB);
            if (dist == 0) { best_idx = idx; break; }               /* exact -> current index */
            if (dist < best) { best = dist; best_idx = idx; }
        }
        *out++ = best_idx;
    }
}

/* ===================================================================================
 * B (hardware half). Palette-DAC upload — IN-GAME live-swap only.
 * ===================================================================================
 * These do `out 0x3c8`(DAC write index) / `out 0x3c9`(6-bit R,G,B) — a privileged instruction
 * that faults under the oracle's bare-metal call_orig, so they CANNOT be oracled; the port writes
 * route through the host hook g_os_port_out (host_dac_port_out, same as the GDV fade path), and
 * they are verified in-game (live-swap). */
extern void (*g_os_port_out)(uint16_t, uint8_t);

/* The shared DAC upload loop: 256 colours, write index to 0x3c8 then R/G/B to 0x3c9 (shifted right
 * by `shift`: 0 = 6-bit source direct, 2 = 8->6-bit scale). */
static void vd_dac_upload_loop(const uint8_t *src, int shift)
{
    for (int i = 0; i < 256; i++) {
        if (g_os_port_out) {
            g_os_port_out(0x3c8, (uint8_t)i);
            g_os_port_out(0x3c9, (uint8_t)(src[i * 3 + 0] >> shift));
            g_os_port_out(0x3c9, (uint8_t)(src[i * 3 + 1] >> shift));
            g_os_port_out(0x3c9, (uint8_t)(src[i * 3 + 2] >> shift));
        }
    }
}

/* upload_palette_dac 0x2febe: void. When the palette-EFFECT counter [0x89f3b] is 0, upload the
 * 256-entry 6-bit palette at [0x90bca]<<4 to the DAC straight. When it is non-zero (a damage flash
 * / screen fade is active), build a TINTED copy first (the `jne 0x2ff44` path — NOT a return!): with
 * fade = max(0, 0xff - [0x89f3b]), each entry becomes R = ((R-0x2d)*fade>>8)+0x2d, G = G*fade>>8,
 * B = B*fade>>8 — i.e. the palette collapses toward dark red (R->0x2d, G/B->0) at full effect and
 * fades back to normal as the counter counts down — then upload it via upload_dac_palette_6bit. */
void upload_palette_dac(void)
{
    const uint8_t *src = (const uint8_t *)(uintptr_t)((uint32_t)G16(VA_g_vel_queue_b + 0x88) << 4);
    int32_t counter = G32(VA_g_damage_flash_level);
    if (counter != 0) {
        uint8_t tint[256 * 3];                         /* [esp-0x300]: tinted palette buffer */
        int32_t fade = 0xff - counter;
        if (fade < 0) fade = 0;                        /* jns; clamp negative to 0 */
        for (int i = 0; i < 256; i++) {
            int32_t r = src[i * 3 + 0];
            r = (((r - 0x2d) * fade) >> 8) + 0x2d;     /* sar 8 (signed) */
            int32_t g = src[i * 3 + 1] * fade;
            int32_t b = src[i * 3 + 2] * fade;
            tint[i * 3 + 0] = (uint8_t)r;
            tint[i * 3 + 1] = (uint8_t)((uint32_t)g >> 8);   /* shr 8 (unsigned) */
            tint[i * 3 + 2] = (uint8_t)((uint32_t)b >> 8);
        }
        upload_dac_palette_6bit((uint32_t)(uintptr_t)tint);
        return;
    }
    vd_dac_upload_loop(src, 0);
}

/* upload_dac_palette_6bit 0x2fefb: EDI=source; uploads 256 6-bit-direct entries (no counter check).
 * Reached from upload_palette_dac's tint path (0x2ffa1 call) — the corpus caller-scan misses it
 * because upload_palette_dac was mis-bounded to its first `ret`; called here in C directly. */
void upload_dac_palette_6bit(uint32_t src)
{
    vd_dac_upload_loop((const uint8_t *)(uintptr_t)src, 0);
}

/* upload_dac_palette_8to6 0x2feff: EAX=source; uploads with an 8->6-bit right-shift. 0-caller. */
uint32_t upload_dac_palette_8to6(uint32_t src)
{
    vd_dac_upload_loop((const uint8_t *)(uintptr_t)src, 2);
    return 0;                                      /* EAX clobbered in original; value unused */
}

/* refresh_palette_dac 0x2ff38 / refresh_palette_dac_wrapper 0x2fea0: thin void wrappers that
 * upload the palette to the DAC (register-preserving push/pop of the scratch regs). */
void refresh_palette_dac(void)         { upload_palette_dac(); }
void refresh_palette_dac_wrapper(void) { upload_palette_dac(); }

/* write_vga_palette 0x4c334: EDX=source, ECX=count(word), EBX=start DAC index. Only when the 8bpp
 * palette flag [0x91de6]==1. If a user callback is registered ([0x91d00], = g_gdv_user_callback),
 * invoke it (eax=3, edx=[0x91d44] header, ecx=word[0x91dd4], ebx=source) and, if it returns eax==3,
 * skip the port write. Otherwise write `count` 8-bit-direct colours to the DAC from `start index`,
 * then latch [0x91dea]=1. The indirect callback is BRIDGED via call_orig at the runtime target. */
void write_vga_palette(uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx)
{
    (void)eax;
    if (G8(VA_g_palette_dirty) != 1) return;
    uint32_t src = edx, start_idx = ebx;
    int32_t count = (int32_t)ecx;
    if ((uint32_t)G32(VA_g_gdv_user_callback) != 0) {
        regs_t io; memset(&io, 0, sizeof io);
        io.va  = (uint32_t)G32(VA_g_gdv_user_callback);           /* runtime callback ptr (already rebased) */
        io.eax = 3;
        io.edx = (uint32_t)G32(VA_g_gdv_decode_buffer + 0x10);
        io.ecx = (uint16_t)G16(VA_g_gdv_audio_format + 0xa);
        io.ebx = src;
#ifndef ROTH_STANDALONE
        call_orig(&io);
#else
        /* the [0x91d00] installs are enumerable — 0x1fce2 run_timed_message_sequence (gdv 0x20662;
         * lifted, EAX=mode,EDX,EBX,ECX -> EAX) and 0x18e09 (the inspect frame callback, a MID-FUNCTION
         * entry into the 0x18cb9 appendage — lifted in lift_gdv_cutscene.c; the inspect popup's DAC
         * upload lands HERE). */
        if (io.va - OBJ_DELTA == 0x1fce2u)
            io.eax = run_timed_message_sequence(io.eax, io.edx, io.ebx, io.ecx);
        else if (io.va - OBJ_DELTA == 0x18e09u)
            io.eax = gdv_inspect_frame_callback(io.eax, io.ebx);
        else
            roth_unreachable((uint32_t)G32(VA_g_gdv_user_callback) - OBJ_DELTA);  /* GDV DAC-fade code-ptr callback — off bare title */
#endif
        if (io.eax == 3) return;                    /* callback handled it */
    }
    const uint8_t *p = (const uint8_t *)(uintptr_t)src;
    uint16_t cx = (uint16_t)count;
    uint32_t idx = start_idx;
    for (;;) {                                      /* do-while: `dec cx; jg` (signed 16-bit) */
        if (g_os_port_out) {
            g_os_port_out(0x3c8, (uint8_t)idx);
            g_os_port_out(0x3c9, p[0]);
            g_os_port_out(0x3c9, p[1]);
            g_os_port_out(0x3c9, p[2]);
        }
        p += 3; idx++;
        cx = (uint16_t)(cx - 1);
        if ((int16_t)cx <= 0) break;
    }
    G8(VA_g_palette_dirty + 0x4) = 1;
}

/* ===================================================================================
 * A (mid/entry). Dirty-rect present pipeline — IN-GAME live-swap only.
 * ===================================================================================
 * These orchestrate the per-frame present: they bracket a frame (cursor save/restore), walk the
 * dirty-rect list and blit each rect to the display, and drive the double-buffered overlay-dirty
 * merge. They call already-lifted blit_2d cursor/blit functions + do planar VGA port I/O
 * (out 0x3c4/0x3ce, via g_os_port_out) — framebuffer/cursor side effects, non-idempotent, and
 * `out` faults under call_orig -> LIVE-SWAP ONLY (verified in the debug session).
 *   g_screen_draw_depth 0x7e8c8 ; cursor-armed flag 0x85434 ; display page word 0x71f04 / limit 0x71f08 */

/* begin_screen_draw 0x11ca9: bump the draw depth and restore both page cursor save-regions. */
void begin_screen_draw(void)
{
    G32(VA_g_screen_busy_depth) += 1;
    restore_cursor_region_if_set(GADDR(VA_g_cursor_prev_y + 0x4));
    restore_cursor_region_if_set(GADDR(VA_g_cursor_prev_y + 0x4010));
}

/* end_screen_draw 0x11cc6: drop the draw depth and force a cursor redraw. */
void end_screen_draw(void)
{
    G32(VA_g_screen_busy_depth) -= 1;
    force_cursor_redraw();
}

/* redraw_cursor_after_blit 0x2ddce: if the cursor was armed (overlapped a blit), restore+redraw it. */
void redraw_cursor_after_blit(void)
{
    if (G8(VA_g_render_target_buffer + 0x20) != 0) restore_and_redraw_cursor();
}

/* present_dirty_cursor_region 0x11eec: unless nested (draw depth > 1), program the planar VGA
 * read/write map (out 0x3c4=2 / 0x3ce=4, only in the planar mode where [0x90c08]==0 && [0x146d8]==0),
 * latch the current display page, and restore the save-region for whichever page is showing. */
void present_dirty_cursor_region(void)
{
    if ((uint32_t)G32(VA_g_screen_busy_depth) > 1) return;                     /* ja (unsigned) — nested draw */
    if (G8(VA_g_rawscreen_flag) == 0 && G32(VA_g_linear_framebuffer_ptr) == 0) {               /* planar mode -> set the VGA maps */
        if (g_os_port_out) { g_os_port_out(0x3c4, 2); g_os_port_out(0x3ce, 4); }
    }
    uint32_t page = (uint16_t)G16(VA_g_init_stage_error_strings + 0x134);
    G32(VA_g_console_input_numeric_only + 0x8) = (int32_t)page;
    if (page >= (uint16_t)G16(VA_g_init_stage_error_strings + 0x138))                        /* jae (unsigned) */
        restore_cursor_region_if_set(GADDR(VA_g_cursor_prev_y + 0x4));
    else
        restore_cursor_region_if_set(GADDR(VA_g_cursor_prev_y + 0x4010));
}

/* blit_screen_rect 0x2dddd: EAX=left EDX=top EBX=right ECX=bottom. If the rect overlaps the armed
 * cursor save-region ([0x76860..0x7686c]) and the cursor isn't already armed, arm it + present the
 * cursor region; then blit the rect to the display (blit_image_to_video_target) from the back
 * buffer at fb+top*pitch+left, with EDI=page<<4. */
void blit_screen_rect(uint32_t p_left, int32_t p_top, uint32_t p_right, uint32_t p_bottom)
{
    int32_t left = (int32_t)p_left, top = p_top, right = (int32_t)p_right, bottom = (int32_t)p_bottom;
    if (G8(VA_g_render_target_buffer + 0x20) == 0
        && left   <= G32(VA_g_console_input_numeric_only + 0x14)                              /* jg skips if left  > cur_right  */
        && top    <= G32(VA_g_console_input_numeric_only + 0x18)                              /* jg skips if top   > cur_bottom */
        && right  >= G32(VA_g_console_input_numeric_only + 0xc)                              /* jl skips if right < cur_left   */
        && bottom >= G32(VA_g_console_input_numeric_only + 0x10)) {                           /* jl skips if bottom< cur_top    */
        G8(VA_g_render_target_buffer + 0x20) = 1;
        present_dirty_cursor_region();
    }
    int32_t width  = right - left + 1;
    int32_t height = bottom - top + 1;
    uint32_t edi = (uint32_t)((uint16_t)G16(VA_g_init_stage_error_strings + 0x134)) << 4;
    uint32_t esi = (uint32_t)(top * G32(VA_g_screen_pitch) + left + (uint32_t)G32(VA_g_framebuffer_ptr));
    blit_image_to_video_target((uint32_t)width, (uint32_t)top, (uint32_t)left,
                                      (uint32_t)height, esi, edi);
}

/* blit_dirty_rect_list 0x2dd85: EAX=list ptr, EDX=count. Blit each {l,t,r,b} rect (stride 0x10) to
 * the display via blit_screen_rect, bracketed by the cursor-armed reset + a final cursor redraw. */
void blit_dirty_rect_list(uint32_t list, int32_t count)
{
    if (count == 0) return;
    G32(VA_g_screen_busy_depth) += 1;
    G8(VA_g_render_target_buffer + 0x20) = 0;
    volatile int32_t *p = (volatile int32_t *)(uintptr_t)list;
    for (int32_t i = 0; i < count; i++) {
        blit_screen_rect((uint32_t)p[0], p[1], (uint32_t)p[2], (uint32_t)p[3]);
        p += 4;                                                /* += 0x10 */
    }
    redraw_cursor_after_blit();
    G32(VA_g_screen_busy_depth) -= 1;
}

/* flush_predraw_hook 0x3cf19: when the predraw snapshot is armed (word [0x90c04]), take a screen
 * snapshot (save_snapshot_file; the 0x3cc01 sibling is a noop). Register-transparent (pushal). */
void flush_predraw_hook(void)
{
    if (G16(VA_g_flush_predraw_flag) != 0)
        save_snapshot_file();
}

/* flush_dirty_rects 0x15dd9: the present ENTRY. Optionally run the predraw hook, snapshot the
 * current dirty list, then present: in single-page mode ([0x90c08]!=0 && [0x146d8]==0) blit the live
 * list directly; otherwise (double-buffered) merge the previous frame's overlay-dirty list into the
 * current one, blit, and stash the pre-merge list as next frame's overlay-dirty. Clears the count. */
void flush_dirty_rects(void)
{
    int32_t local_copy[0x40 * 4];                              /* [ebp-0x404]: dirty-list snapshot */
    if (G8(VA_g_flush_predraw_flag) != 0) flush_predraw_hook();

    int32_t count = G32(VA_g_dirty_rect_count);
    {
        volatile int32_t *src = (volatile int32_t *)GADDR(VA_g_dirty_rects);
        for (int32_t i = 0; i < count * 4; i++) local_copy[i] = src[i];
    }

    if (G8(VA_g_rawscreen_flag) != 0 && G32(VA_g_linear_framebuffer_ptr) == 0) {               /* single-page: direct blit */
        blit_dirty_rect_list(GADDR(VA_g_dirty_rects), G32(VA_g_dirty_rect_count));
        G32(VA_g_dirty_rect_count) = 0;
    } else {                                                   /* double-buffered: merge + stash */
        volatile int32_t *ov = (volatile int32_t *)GADDR(VA_g_prev_dirty_rects);
        int32_t ovcount = G32(VA_g_prev_dirty_rect_count);
        for (int32_t i = 0; i < ovcount; i++)
            add_dirty_rect((uint32_t)ov[i * 4 + 0], ov[i * 4 + 1],
                                  (uint32_t)ov[i * 4 + 2], (uint32_t)ov[i * 4 + 3]);
        blit_dirty_rect_list(GADDR(VA_g_dirty_rects), G32(VA_g_dirty_rect_count));
        G32(VA_g_dirty_rect_count) = 0;
        volatile int32_t *dst = (volatile int32_t *)GADDR(VA_g_prev_dirty_rects);
        for (int32_t i = 0; i < count * 4; i++) dst[i] = local_copy[i];
        G32(VA_g_prev_dirty_rect_count) = count;
    }
}

/* mark_overlay_dirty_rects 0x1f330: for each of the 3 overlay save-slots, if its DAS cache handle
 * is set, register its saved rect as dirty (register_dirty_rect) and free the handle. */
void mark_overlay_dirty_rects(void)
{
    if (G32(VA_g_timed_message_timer + 0x4) != 0) {
        register_dirty_rect((uint32_t)G32(VA_g_timed_message_timer + 0x8), G32(VA_g_timed_message_timer + 0xc),
                                   (uint32_t)G32(VA_g_timed_message_timer + 0x10), (uint32_t)G32(VA_g_timed_message_timer + 0x14));
        G32(VA_g_timed_message_timer + 0x4) = (int32_t)free_das_cache_handle((uint32_t)G32(VA_g_timed_message_timer + 0x4));
    }
    if (G32(VA_g_dialogue_reveal_ramp + 0x18) != 0) {
        register_dirty_rect((uint32_t)G32(VA_g_dialogue_busy_flag + 0x4), G32(VA_g_dialogue_busy_flag + 0x8),
                                   (uint32_t)G32(VA_g_dialogue_busy_flag + 0xc), (uint32_t)G32(VA_g_dialogue_busy_flag + 0x10));
        G32(VA_g_dialogue_reveal_ramp + 0x18) = (int32_t)free_das_cache_handle((uint32_t)G32(VA_g_dialogue_reveal_ramp + 0x18));
    }
    if (G32(VA_g_dialogue_reveal_ramp + 0x1c) != 0) {
        register_dirty_rect((uint32_t)G32(VA_g_dialogue_busy_flag + 0x14), G32(VA_g_dialogue_busy_flag + 0x18),
                                   (uint32_t)G32(VA_g_dialogue_busy_flag + 0x1c), (uint32_t)G32(VA_g_dialogue_busy_flag + 0x20));
        G32(VA_g_dialogue_reveal_ramp + 0x1c) = (int32_t)free_das_cache_handle((uint32_t)G32(VA_g_dialogue_reveal_ramp + 0x1c));
    }
}

/* remap_builtin_palette_image 0x10d67: remap the built-in cursor-glyph RGB image (count word
 * [0x7041a], pixels 0x7041c) into palette indices at 0x7675c, then blit the remapped glyph. */
void remap_builtin_palette_image(void)
{
    int32_t count = (uint16_t)G16(VA_g_cfg_das2_arg + 0x13e);
    remap_rgb_to_palette_indices(count, (const uint8_t *)GADDR(VA_g_cfg_das2_arg + 0x140),
                                        (uint8_t *)GADDR(VA_g_player_movement_enabled + 0x12));
    blit_remapped_cursor_glyph();
}

/* ---------------------- copy_vesa_mode_list_block (0x27f9b) ----------------------
 * EAX=VBE-info block (+0xe=mode-list offset word, +0x10=mode-list segment word), ES:EDI=dest.
 * Copies 0x40 dwords (256 bytes) of the VESA mode list from the real-mode far pointer
 * (seg<<4 + off, low DOS mem) to the dest buffer, terminates it with 0xffff, and returns the word
 * count 0x80 in ECX (multi-reg return — the host ABI adapter must write ECX). `dest` is the flat
 * es:edi target. */
uint32_t copy_vesa_mode_list_block(const uint8_t *info_block, uint8_t *dest)
{
    uint16_t off = *(const uint16_t *)(info_block + 0xe);
    uint16_t seg = *(const uint16_t *)(info_block + 0x10);
    const uint8_t *src = (const uint8_t *)(uintptr_t)(((uint32_t)seg << 4) + off);
    for (int i = 0; i < 0x100; i++) dest[i] = src[i];              /* rep movsd 0x40 dwords */
    *(uint16_t *)(dest + 0xfe) = 0xffff;                           /* word[edi-2] after edi += 0x100 */
    return 0x80;                                                    /* ECX = word count */
}

/* ===================================================================================
 * C / D. Mode set / resolution + framebuffer surfaces — IN-GAME live-swap only.
 * ===================================================================================
 * These program the display mode, build the scanline table, and allocate/free the framebuffer
 * surface + its DPMI selectors. The DPMI (int 0x31) / DOS (int 0x21) / VESA-int-10h callees are
 * privileged and host-serviced -> BRIDGED via call_orig (reproducing the call-site register state);
 * the video_display-local bookkeeping is lifted C; lifted callees (patch_span_mapper_pitch,
 * set_floorceil_span_value, game_heap_alloc[_round4], key_cycle_display_type, and the other
 * video_display lifts) are called directly. LIVE-SWAP ONLY, verified in the end debug session. */

#ifdef ROTH_STANDALONE
/* ---- the DPMI framebuffer-selector composites, image-free (OBJ1-A follow-on) ----
 * Faithful transcriptions of 0x2fd7c/0x2fdbc/0x2fdfc/0x2fe77/0x2f72a over the g_os_soft_int int31
 * seam (host dpmi.c services 0000 alloc / 0001 free / 0007 set-base / 0008 set-limit — the SAME
 * services the trap host reaches through the original bytes' `int 0x31`). Non-oracle-able in the
 * batch lane (non-deterministic selector returns), which is why they stayed bridges until now. */
static uint32_t vd_if_int31(uint32_t fn, uint32_t ebx, uint32_t v32, regs_t *out)
{
    regs_t r; memset(&r, 0, sizeof r);
    r.eax = fn; r.ebx = ebx;
    r.ecx = (fn == 0) ? 1u : (v32 >> 16);          /* 0000: CX=count; 0007/0008: CX:DX=value */
    r.edx = v32 & 0xffffu;
    uint32_t fl = g_os_soft_int(0x31, &r);
    if (out) *out = r;
    return fl & 1u;                                /* CF */
}
static uint32_t vd_if_alloc_desc(void)            /* int31 0000 cx=1 -> selector, or 0xffffffff on CF */
{
    regs_t r;
    return vd_if_int31(0, 0, 0, &r) ? 0xffffffffu : (r.eax & 0xffffu);
}
/* 0x2fdfc set_framebuffer_selector_bases(EAX=base) -> CF. [0x85414]=base; limit value = [0x76634] ?
 * [0x85498]*[0x8549c] : 0x10000 (the raw value the original passes to int31 0008); sel [0x90c06]
 * gets base+limit, sel [0x89f28] gets base+[0x89f2c]+limit. Exported: renderer.c's
 * setup_render_projection_scale bridges the same VA. */
uint32_t vd_standalone_set_fb_bases(uint32_t base)
{
    uint32_t lim = (G32(VA_g_video_linear_flag) != 0)
                 ? (uint32_t)G32(VA_g_screen_pitch) * (uint32_t)G32(VA_g_screen_pitch + 0x4) : 0x10000u;
    G32(VA_g_render_target_buffer) = (int32_t)base;
    if (vd_if_int31(7, G16(VA_g_render_target_selector), base, NULL)) return 1;
    if (vd_if_int31(8, G16(VA_g_render_target_selector), lim, NULL))  return 1;
    if (vd_if_int31(7, G16(VA_g_render_target_selector_secondary), base + (uint32_t)G32(VA_g_render_target_secondary_size), NULL)) return 1;
    return vd_if_int31(8, G16(VA_g_render_target_selector_secondary), lim, NULL);
}
static uint32_t vd_if_alloc_fb_selectors(uint32_t canon_va)   /* 0x2fd7c hires / 0x2fdbc lores -> CF */
{
    uint32_t sel = vd_if_alloc_desc();
    if (sel == 0xffffffffu) return 1;
    G16(VA_g_render_target_selector) = (uint16_t)sel;
    if (canon_va == 0x2fd7cu) { G32(VA_g_render_target_secondary_size) = 0; G16(VA_g_render_target_secondary_height) = 0; }         /* hires: page delta 0 */
    sel = vd_if_alloc_desc();
    if (sel == 0xffffffffu) return 1;
    G16(VA_g_render_target_selector_secondary) = (uint16_t)sel;
    if (canon_va == 0x2fdbcu) { G32(VA_g_render_target_secondary_size) = 0xfa00; G16(VA_g_render_target_secondary_height) = 0xc8; } /* lores: 64000 B / 200 rows */
    return vd_standalone_set_fb_bases((uint32_t)G32(VA_g_framebuffer_ptr));
}
static void vd_if_free_fb_selectors(void)                     /* 0x2fe77: xchg-out + free both */
{
    uint16_t s = G16(VA_g_render_target_selector); G16(VA_g_render_target_selector) = 0; if (s) vd_if_int31(1, s, 0, NULL);
    s = G16(VA_g_render_target_selector_secondary); G16(VA_g_render_target_selector_secondary) = 0; if (s) vd_if_int31(1, s, 0, NULL);
}
/* 0x27eec (VBE 4F00 controller info) / 0x27f6f (4F01 mode info, EAX=mode) — faithful transcriptions
 * of the DPMI-0300 simulate-real-mode-int10 wrappers: lazily mint the 4K DOS transfer buffer
 * (0x40b08, lifted), build the RM register frame at its base (RM EAX=4Fxx, RM ECX=mode for 4F01,
 * RM ES:DI -> the 16-aligned data area at buf+0x41, RM SS:SP inside the page), zero 0x200 data
 * bytes, then int31 0300 with ES:EDI = the buffer selector:0 — the SAME host service the trap lane
 * reaches (dpmi.c fills the VbeInfo/ModeInfo block). Returns the flat data-area ptr, 0 on failure. */
static uint32_t vd_if_vesa_info(uint32_t fn_ax, uint32_t mode)
{
    ensure_dos_transfer_buffer(0);
    uint32_t buf = (uint32_t)G32(VA_g_dos_transfer_buffer_linear);
    if (buf == 0) return 0;
    for (int i = 0; i < 0x40; i++) *(volatile uint8_t *)(uintptr_t)(buf + (uint32_t)i) = 0;
    *(volatile uint32_t *)(uintptr_t)(buf + 0x1c) = fn_ax;             /* RM EAX */
    if (fn_ax == 0x4f01u)
        *(volatile uint32_t *)(uintptr_t)(buf + 0x18) = mode;          /* RM ECX = mode */
    uint32_t data = (buf + 0x41u) & ~0xfu;
    *(volatile uint32_t *)(uintptr_t)(buf + 0x00) = 0;                 /* RM EDI = 0 */
    *(volatile uint16_t *)(uintptr_t)(buf + 0x22) = (uint16_t)(data >> 4);  /* RM ES  = data seg */
    *(volatile uint16_t *)(uintptr_t)(buf + 0x30) = (uint16_t)(buf >> 4);   /* RM SS  */
    *(volatile uint16_t *)(uintptr_t)(buf + 0x2e) = 0xffc;                  /* RM SP  */
    for (int i = 0; i < 0x200; i++) *(volatile uint8_t *)(uintptr_t)(data + (uint32_t)i) = 0;
    regs_t v; memset(&v, 0, sizeof v);
    v.eax = 0x300; v.ebx = 0x10; v.ecx = 0; v.edi = 0;
    v.es = (uint32_t)G16(VA_g_dos_transfer_buffer_selector);                                     /* transfer-buffer selector */
    if (g_os_soft_int(0x31, &v) & 1u) return 0;
    if (*(volatile uint16_t *)(uintptr_t)(buf + 0x1c) != 0x4f) return 0;    /* AX out == 004F */
    return data;
}
static void vd_if_alloc_sel(regs_t *io)                       /* 0x2f72a: EDI=base ECX=size -> EAX=sel, CF */
{
    uint32_t sel = vd_if_alloc_desc();
    if (sel == 0xffffffffu) { io->eflags |= 1u; return; }
    if (vd_if_int31(7, sel, io->edi, NULL) || vd_if_int31(8, sel, io->ecx - 1, NULL)) {
        vd_if_int31(1, sel, 0, NULL);                          /* 0x2f765: free on set failure */
        io->eflags |= 1u;
        return;
    }
    io->eax = sel;
    io->eflags &= ~1u;
}
/* set_vesa_bank 0x2e0cb (EDX=bank): cached in [0x71dc6]; int10 4F05 window A, plus window B when
 * the winattr bit [0x71dcc] is set (the 0x2e0ef arm). The mode-set seeds [0x71dc6]=0x378 so the
 * first set(0) is never cached away. */
static void vd_if_set_vesa_bank(uint32_t dx)
{
    if (G16(VA_g_current_vesa_bank) == (uint16_t)dx) return;
    G16(VA_g_current_vesa_bank) = (uint16_t)dx;
    regs_t v; memset(&v, 0, sizeof v);
    v.eax = 0x4f05; v.ebx = 0; v.edx = dx & 0xffffu;           /* window A */
    g_os_soft_int(0x10, &v);
    if (G8(VA_g_vesa_available) != 0) {
        memset(&v, 0, sizeof v);
        v.eax = 0x4f05; v.ebx = 1; v.edx = (uint32_t)G16(VA_g_current_vesa_bank);  /* window B */
        g_os_soft_int(0x10, &v);
    }
}
/* dpmi phys-map 0x28350 (EAX=phys, EDX=size) -> linear or 0. Faithful quirks kept: phys <= 64K
 * returns 0; CX=0 drops the low phys word (VESA_LFB_LIN 0x100000 is 64K-aligned, so lossless);
 * SI:DI carries size-1 (as the original passes it). Host int31 0800 identity-maps. */
static uint32_t vd_if_phys_map(uint32_t phys, uint32_t size)
{
    if (phys <= 0x10000u) return 0;
    regs_t v; memset(&v, 0, sizeof v);
    v.eax = 0x800;
    v.ebx = phys >> 16; v.ecx = 0;
    size -= 1;
    v.esi = size >> 16; v.edi = size & 0xffffu;
    if (g_os_soft_int(0x31, &v) & 1u) return 0;
    return ((v.ebx & 0xffffu) << 16) | (v.ecx & 0xffffu);
}
/* set_vesa_video_mode 0x14b24 (EAX=mode, bit14=LFB request) -> CF. Faithful transcription:
 * int10 4F02 -> 4F01 mode info (vd_if_vesa_info) -> stash w/h/pitch in the obj1 slots
 * [0x146e2/e4/dc]; LFB arm (0x14c54): phys-map PhysBasePtr for height*pitch*2 -> [0x146d8];
 * banked arm: page words {a000,0,a000,0}, winattr/granularity ([0x71dcc]/[0x71dc4] = 0x40/gran),
 * bank cache seed 0x378, set_vesa_bank(0). Both arms load [0x854a4] from the per-resolution
 * scale table at obj1 0x146e8 (obj1data-staged) then run the common tail: mode -> [0x76634],
 * w/h -> [0x85498/9c], scanline table rebuild, [0x90c08]=0xff, and the portrait check
 * (w <= h -> 320x400 double-scan: [0x90c08]=2, [0x90be6]=4, h/2, [0x90cbd]=1). */
static uint32_t vd_if_set_vesa_video_mode(uint32_t mode)
{
    G16(VA_g_current_vesa_mode) = (uint16_t)mode;
    regs_t v; memset(&v, 0, sizeof v);
    v.eax = 0x4f02; v.ebx = mode;
    g_os_soft_int(0x10, &v);
    if ((v.eax & 0xffffu) != 0x4f) return 1;                   /* stc */
    uint32_t blk = vd_if_vesa_info(0x4f01, mode & 0xfffu);
    if (blk == 0) return 1;
    G16(VA_g_vesa_mode_found_word_a) = *(volatile uint16_t *)(uintptr_t)(blk + 0x12);   /* width  */
    G16(VA_g_vesa_mode_found_word_b) = *(volatile uint16_t *)(uintptr_t)(blk + 0x14);   /* height */
    G32(VA_g_video_mode_width) = (int32_t)(uint32_t)*(volatile uint16_t *)(uintptr_t)(blk + 0x10); /* pitch */
    if (mode & 0x4000u) {                                      /* LFB arm (test dh,0x40 -> 0x14c54) */
        uint32_t phys = *(volatile uint32_t *)(uintptr_t)(blk + 0x28);
        if (phys == 0) return 1;
        uint32_t size = (uint32_t)G16(VA_g_vesa_mode_found_word_b) * (uint32_t)G32(VA_g_video_mode_width) * 2u;
        uint32_t lin = vd_if_phys_map(phys, size);             /* call 0x28350 */
        if (lin == 0) return 1;
        G32(VA_g_linear_framebuffer_ptr) = (int32_t)lin;                           /* g_linear_framebuffer_ptr */
    } else {                                                   /* banked arm */
        G16(VA_g_init_stage_error_strings + 0x136) = 0;      G16(VA_g_init_stage_error_strings + 0x13a) = 0;
        G16(VA_g_init_stage_error_strings + 0x134) = 0xa000; G16(VA_g_init_stage_error_strings + 0x138) = 0xa000;
        G8(VA_g_vesa_available) = *(volatile uint8_t *)(uintptr_t)(blk + 3) & 1;
        uint16_t gran = *(volatile uint16_t *)(uintptr_t)(blk + 4);
        G16(VA_g_vesa_page_bank_offset) = (uint16_t)(0x40u / gran);
        G16(VA_g_current_vesa_bank) = 0x378;
        vd_if_set_vesa_bank(0);
    }
    {   /* [0x854a4] = scale_table[[0x7f358]&0xff] (obj1 0x146e8, staged) — both arms */
        uint32_t idx = (uint32_t)G32(VA_g_screen_resolution_index) & 0xffu;
        G32(VA_g_screen_height + 0x4) = G32(VA_g_vesa_scale_and_mode_table + idx * 4);
    }
    /* common tail 0x14be7 */
    G32(VA_g_video_linear_flag) = (int32_t)(uint32_t)G16(VA_g_current_vesa_mode);
    G16(VA_g_screen_pitch) = G16(VA_g_vesa_mode_found_word_a);
    G16(VA_g_screen_pitch + 0x4) = G16(VA_g_vesa_mode_found_word_b);
    G16(VA_g_video_mode_flags) = 0;
    build_scanline_dest_offset_table();                 /* call 0x2fb49 (lifted) */
    G8(VA_g_rawscreen_flag) = 0xff;
    if (G16(VA_g_screen_pitch) <= G16(VA_g_screen_pitch + 0x4)) {                        /* ja skips: portrait/double-scan */
        G8(VA_g_rawscreen_flag) = 2;
        G16(VA_g_video_mode_flags) = 4;
        G16(VA_g_screen_pitch + 0x4) = (uint16_t)(G16(VA_g_screen_pitch + 0x4) >> 1);
        G8(VA_g_hires_line_doubling_flag) = 1;
    }
    return 0;                                                  /* clc */
}
#endif

/* Bridge a hardware/DPMI/DOS callee to the original bytes (host-serviced). */
static void vd_bridge(uint32_t canon_va, regs_t *io)
{
    io->va = canon_va + OBJ_DELTA;
#ifndef ROTH_STANDALONE
    call_orig(io);
#else
    switch (canon_va) {
    case 0x2fd7cu: case 0x2fdbcu:                              /* alloc_framebuffer_selectors hires/lores -> CF */
        io->eflags = (io->eflags & ~1u) | vd_if_alloc_fb_selectors(canon_va); return;
    case 0x2fe77u: vd_if_free_fb_selectors();                  return;
    case 0x2f72au: vd_if_alloc_sel(io);                        return;
    case 0x2e140u: host_blank_active_video_page();             return;
    case 0x27eecu: io->eax = vd_if_vesa_info(0x4f00, 0);       return;   /* VBE controller info -> flat blk/0 */
    case 0x27f6fu: io->eax = vd_if_vesa_info(0x4f01, io->eax); return;   /* VBE mode info (EAX=mode) */
    case 0x14b24u:                                                       /* set_vesa_video_mode -> CF */
        io->eflags = (io->eflags & ~1u) | vd_if_set_vesa_video_mode(io->eax); return;
    case 0x28387u:                                                       /* re-map prev LFB linear (EAX=addr; 0=skip) */
        if (io->eax != 0) {
            regs_t v; memset(&v, 0, sizeof v);
            v.eax = 0x800; v.ebx = io->eax >> 16; v.ecx = io->eax & 0xffffu;   /* SI:DI caller-leftover = 0 here */
            g_os_soft_int(0x31, &v);
        }
        return;
    default:
        roth_unreachable(canon_va);   /* off the bare-title path — fail loud, precise stop */
    }
#endif
}

/* build_scanline_dest_offset_table 0x2fb49: fill the scanline-dest table [0x854a8] with row*pitch
 * for each of g_screen_height [0x8549c] rows, then patch the renderer's SMC stride
 * (patch_span_mapper_pitch) + set the floor/ceil span value. ORACLE-ABLE (obj1 SMC + obj3 table). */
void build_scanline_dest_offset_table(void)
{
    volatile int32_t *tbl = (volatile int32_t *)GADDR(VA_g_scanline_dest_offset_table);
    int32_t pitch  = G32(VA_g_screen_pitch);
    int32_t height = G32(VA_g_screen_pitch + 0x4);
    int32_t acc = 0;
    for (int32_t i = 0; i < height; i++) { tbl[i] = acc; acc += pitch; }
    patch_span_mapper_pitch((uint32_t)pitch);
    set_floorceil_span_value((uint32_t)pitch);
}

/* set_resolution_index_and_cycle_display 0x147e6: EAX=index. Store index-1 and cycle the display
 * type (key_cycle_display_type [L]). Register-transparent (pushal). */
void set_resolution_index_and_cycle_display(uint32_t idx)
{
    G32(VA_g_screen_resolution_index) = (int32_t)idx - 1;
    key_cycle_display_type();
}

/* set_resolution_index_and_cycle 0x147f4: EAX=index. Store index-1 and run the mode-set orchestrator
 * (cycle_screen_resolution). Preserves ebx..ebp. */
void set_resolution_index_and_cycle(uint32_t idx)
{
    G32(VA_g_screen_resolution_index) = (int32_t)idx - 1;
    cycle_screen_resolution();
}

/* init_video_mode_table_once 0x14772: build the VESA mode table once (guard byte [0x14770]).
 * match_vesa_video_modes(EAX=mode-table 0x14716, EDX=8). */
void init_video_mode_table_once(void)
{
    if (G8(VA_g_vesa_mode_table_built) != 0) return;
    match_vesa_video_modes(GADDR(VA_g_vesa_scale_and_mode_table + 0x2e), 8);
    G8(VA_g_vesa_mode_table_built) = 1;
}

/* init_video_surface 0x2fc98: when no external surface is bound ([0x76634]==0), set the default VGA
 * mode (320x200) + build the scanline table; then allocate the framebuffer surface.
 * CF EXPOSURE: the original tail is `call 0x2fbe8 (alloc_framebuffer_surface); ret` — the ret
 * preserves alloc_framebuffer_surface's CF (its 0x2fc96 `stc` fail-tail), which main_sequence reads via
 * `jb 0x102ab` (line 0x10298). Return that CF (1=alloc failed) so the image-free gc_call dispatch can
 * thread it — was a void CF-drop, the remaining on-path boot blocker. */
uint32_t init_video_surface(void)
{
    if (G32(VA_g_video_linear_flag) == 0) {
        G32(VA_g_screen_pitch) = 0x140;                          /* pitch 320 */
        G32(VA_g_screen_pitch + 0x4) = 0xc8;                           /* height 200 */
        G32(VA_g_screen_height) = 0xc8;
        G32(VA_g_screen_height + 0x4) = 0xcccc;
        build_scanline_dest_offset_table();
    }
    return alloc_framebuffer_surface();         /* CF propagates: `call; ret` tail */
}

/* alloc_scene_render_arena 0x2a909: allocate a 0x18000-byte scene render arena (game_heap_alloc_round4
 * [L]), record base [0x8498c] + the selector base [0x84990] = base+0x8000-0x26, then mint a DPMI
 * selector (alloc_dpmi_selector, ECX=limit 0x18000, EDI=base+0x8000) stored to word [0x85294]. */
void alloc_scene_render_arena(void)
{
    uint32_t base = game_heap_alloc_round4(0x18000);
    G32(VA_g_door_worklist) = (int32_t)base;
    uint32_t sel_base = base + 0x8000 - 0x26;
    G32(VA_g_door_worklist + 0x4) = (int32_t)sel_base;
    regs_t io; memset(&io, 0, sizeof io);
    io.eax = sel_base; io.ecx = 0x18000; io.edi = base + 0x8000;
    vd_bridge(0x2f72a, &io);                            /* alloc_dpmi_selector -> AX = selector */
    G16(VA_g_map_geometry_selector) = (uint16_t)io.eax;
}

/* alloc_framebuffer_surface 0x2fbe8: allocate the back-buffer + DPMI selectors. Two paths by the
 * resolution index [0x7f578]: index 7/8 -> 0x4b000-byte surface + selectors via 0x2fd7c; else ->
 * 0x20000-byte surface + selectors via 0x2fdbc. Each tries a DOS block (alloc_dos_block, [0x89f3a]=1)
 * then falls back to the game heap ([0x89f3a]=0). Returns CF (1 = allocation failed). */
uint32_t alloc_framebuffer_surface(void)
{
    G16(VA_g_roth_error_code) = 1;
    int32_t res = G32(VA_g_selected_video_mode);
    regs_t io;
    if (res == 8 || res == 7) {
        G32(VA_g_framebuffer_bytes) = 0x4b000;
        G8(VA_g_framebuffer_from_dos_block) = 1;
        uint32_t ptr = alloc_dos_block(0x4b000);  /* [repointed 0x35a12] EAX=size -> ptr */
        if (ptr == 0) {
            G8(VA_g_framebuffer_from_dos_block) = 0;
            ptr = game_heap_alloc(0x4b000);      /* heap fallback */
            if (ptr == 0) return 1;                     /* stc; ret */
        }
        G32(VA_g_framebuffer_ptr) = (int32_t)ptr; G32(VA_g_init_stage_error_strings + 0x130) = (int32_t)ptr;
        memset(&io, 0, sizeof io);
        vd_bridge(0x2fd7c, &io);                        /* alloc_framebuffer_selectors "hires" -> CF */
        return io.eflags & 1;                           /* 0x2fc47 call; 0x2fc4c ret — the selector-alloc CF
                                                         * propagates to main_sequence's jb 0x102ab (same
                                                         * threading as the lores arm; old C hardcoded 0). */
    } else {
        G32(VA_g_framebuffer_bytes) = 0x20000;
        G8(VA_g_framebuffer_from_dos_block) = 1;
        uint32_t ptr = alloc_dos_block(0x20000);  /* [repointed 0x35a12] EAX=size -> ptr */
        if (ptr == 0) {
            G8(VA_g_framebuffer_from_dos_block) = 0;
            ptr = game_heap_alloc(0);            /* faithful: EAX=0 at the call site */
            if (ptr == 0) return 1;
        }
        G32(VA_g_framebuffer_ptr) = (int32_t)ptr; G32(VA_g_init_stage_error_strings + 0x130) = (int32_t)ptr;
        G16(VA_g_roth_error_code) = 0xc;
        memset(&io, 0, sizeof io);
        vd_bridge(0x2fdbc, &io);                        /* alloc_framebuffer_selectors "lores" -> CF */
        return io.eflags & 1;
    }
}

/* free_framebuffer_surface 0x2fce9: free the DPMI selectors (free_framebuffer_selectors), then free
 * the back buffer via the DOS-block path ([0x89f3a]!=0 -> free_os_block_guarded) or the heap path
 * (-> game_free_if_not_null); clears [0x90a98]. */
void free_framebuffer_surface(void)
{
    regs_t io; memset(&io, 0, sizeof io);
    vd_bridge(0x2fe77, &io);                            /* free_framebuffer_selectors (DPMI) */
    uint32_t ptr = (uint32_t)G32(VA_g_framebuffer_ptr);
    G32(VA_g_framebuffer_ptr) = 0;
    if (G8(VA_g_framebuffer_from_dos_block) != 0) free_os_block_guarded(ptr);                 /* [repointed 0x35af2] EAX=ptr */
    else                  game_free_if_not_null((uint8_t *)(uintptr_t)ptr); /* [repointed 0x40a2a] EAX=block */
}

/* Show a formatted resolution OSD: sprintf(buf, fmt, width, height) [CRT bridge] then show it. */
extern uint32_t (*g_os_soft_int)(uint8_t vec, regs_t *io);
static void vd_show_res_msg(uint32_t fmt_canon, uint16_t width, uint16_t height)
{
    char buf[0x64];
    regs_t io; memset(&io, 0, sizeof io);
    io.va = 0x27c53 + OBJ_DELTA;                       /* sprintf (cdecl, caller cleans) */
    io.nstack = 4;
    io.stack[0] = (uint32_t)(uintptr_t)buf;            /* dest   */
    io.stack[1] = (uint32_t)GADDR(fmt_canon);          /* format */
    io.stack[2] = width;
    io.stack[3] = height;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    roth_sprintf(buf, (const char *)GADDR(fmt_canon), (int)width, (int)height); /* "Vesa %dx%d" */
#endif
    show_timed_message(buf);
}

/* cycle_screen_resolution 0x1480c (733B): THE mode-set orchestrator. Reads the resolution index
 * [0x7f358], resets the page/mode globals (unmapping any DPMI phys-map at [0x146d8]), then dispatches:
 * special indices 0x64/0x65/0x66 -> the VGA fallbacks (320x200 / 320x400 / mode 0x13 via int 0x10);
 * otherwise looks up the mode table [0x1470c] (stride 0xa) and, for VESA modes, programs the mode
 * (set_vesa_video_mode; falls back to VGA if it fails) + shows a resolution OSD. The common tail
 * recomputes line-doubling/view offsets, reconfigures the render viewport, re-uploads the palette,
 * and recomputes the screen extents. Hardware callees (set_vesa_video_mode / blank_active_video_page /
 * dpmi_map_physical_address / sprintf / int 0x10) are host-serviced (bridged / g_os_soft_int); the
 * lifted render + video_display callees are called directly. */
void cycle_screen_resolution(void)
{
    if ((uint8_t)G8(VA_g_screen_resolution_index) <= 9)                     /* ja 0x1481a skips the (one-time) table build */
        init_video_mode_table_once();

  retry: {                                             /* 0x1481a */
    G8(VA_g_display_type) = 0;
    G8(VA_g_hires_line_doubling_flag) = 0;
    G16(VA_g_video_mode_flags) = 0;
    G16(VA_g_init_stage_error_strings + 0x13a) = 0;
    G16(VA_g_init_stage_error_strings + 0x134) = 0xa000;  G16(VA_g_init_stage_error_strings + 0x136) = 0x4000;  G16(VA_g_init_stage_error_strings + 0x138) = 0xa400;
    if (G32(VA_g_linear_framebuffer_ptr) != 0) {                           /* unmap a previous LFB phys-map */
        regs_t io; memset(&io, 0, sizeof io);
        io.eax = (uint32_t)G32(VA_g_linear_framebuffer_ptr);
        G32(VA_g_linear_framebuffer_ptr) = 0;
        vd_bridge(0x28387, &io);                       /* dpmi_map_physical_address */
    }
    /* 0x14872 */
    int32_t idx = G32(VA_g_screen_resolution_index) + 1;
    if (idx == 0x64) goto vga320x200;
    if (idx == 0x65) goto vga320x400;
    if (idx == 0x66) goto mode13;
    if ((uint8_t)idx > 9) idx = 0;                     /* cmp al,9; jbe; else sub eax,eax */
    idx &= 0xff;
    G32(VA_g_screen_resolution_index) = idx;
    uint8_t *ent = (uint8_t *)(GADDR(VA_g_vesa_scale_and_mode_table + 0x24) + (uint32_t)idx * 0xa);   /* mode_table[idx] (in obj1) */
    uint16_t width  = *(uint16_t *)(ent + 0);
    uint16_t height = *(uint16_t *)(ent + 2);
    if ((int32_t)((uint32_t)width * height) > G32(VA_g_framebuffer_bytes)) goto retry;  /* won't fit the surface */
    uint16_t vmode = *(uint16_t *)(ent + 6);
    if (vmode == 0xd) goto mode13;
    if (vmode == 0)   goto vga_lowres;

    {   /* VESA mode: program it (with the LFB bit); on failure fall back / retry */
        regs_t io; memset(&io, 0, sizeof io);
        io.eax = (uint32_t)vmode | 0x4000;
        vd_bridge(0x14b24, &io);                       /* set_vesa_video_mode -> CF */
        if ((io.eflags & 1) == 0) { vd_show_res_msg(0x70a21, width, height); goto common_tail; }
        if (width >= 0x280) {                           /* wide mode: retry without the LFB bit */
            memset(&io, 0, sizeof io);
            io.eax = (uint32_t)*(uint16_t *)(ent + 6);
            vd_bridge(0x14b24, &io);
            if ((io.eflags & 1) == 0) { vd_show_res_msg(0x70a2f, width, height); goto common_tail; }
            *(uint16_t *)(ent + 6) = 0;
            goto retry;
        }
        *(uint16_t *)(ent + 6) = 0;                     /* clear VESA number, fall to the VGA path */
    }
  vga_lowres:                                           /* 0x148f7 */
    if (width != 0x140) goto retry;
    if (height == 0xc8)  goto vga320x200;
    if (height == 0x190) goto vga320x400;
    goto retry;
  }

  common_tail:                                          /* 0x14969 */
    G8(VA_g_reloc_base + 0x4) = 1;
    recompute_hires_line_doubling();
    G8(VA_g_collision_sector_stack + 0x3e) = 0;
    recompute_view_region_offsets();
    configure_render_viewport();
    upload_palette_dac();
    compute_screen_extents_7e8b0();
    return;

  vga320x200:                                           /* 0x14991 */
    G32(VA_g_screen_resolution_index) = 1;
    G16(VA_g_video_mode_flags) = 0;
    G16(VA_g_screen_pitch + 0x4) = 0xc8;
    G32(VA_g_video_linear_flag) = 0;
    G16(VA_g_screen_pitch) = 0x140;
    show_timed_message((const char *)GADDR(VA_g_keymap_table + 0xc6));
    goto vga_common;
  vga320x400:                                           /* 0x149cc */
    G32(VA_g_video_linear_flag) = 0;
    G32(VA_g_screen_resolution_index) = 2;
    G16(VA_g_video_mode_flags) = 4;
    G16(VA_g_screen_pitch + 0x4) = 0xc8;
    G16(VA_g_screen_pitch) = 0x140;
    show_timed_message((const char *)GADDR(VA_g_keymap_table + 0xd5));
    /* fall through */
  vga_common: {                                         /* 0x14a05 */
    G8(VA_g_display_type) = 1;
    G8(VA_g_rawscreen_flag) = 0;
    G32(VA_g_video_linear_flag) = 0;
    G16(VA_g_vga_mode_configured) = 0;
    G16(VA_g_current_vesa_mode) = 0;
    regs_t io; memset(&io, 0, sizeof io); io.eax = 0x13;
    if (g_os_soft_int) g_os_soft_int(0x10, &io);   /* set VGA mode 0x13 */
    G32(VA_g_video_mode_width) = 0x140;
    build_scanline_dest_offset_table();
    G32(VA_g_screen_height + 0x4) = 0xcccc;
    goto common_tail;
  }

  mode13: {                                             /* 0x14a58 */
    G32(VA_g_screen_resolution_index) = 0;
    G8(VA_g_rawscreen_flag) = 1;
    regs_t io; memset(&io, 0, sizeof io); io.eax = 0x13;
    if (g_os_soft_int) g_os_soft_int(0x10, &io);
    G16(VA_g_screen_pitch) = 0x140;
    G16(VA_g_screen_pitch + 0x4) = 0xc8;
    G16(VA_g_init_stage_error_strings + 0x136) = 0;  G16(VA_g_init_stage_error_strings + 0x13a) = 0;  G16(VA_g_init_stage_error_strings + 0x134) = 0xa000;
    G8(VA_g_hires_line_doubling_flag) = 0;
    G16(VA_g_init_stage_error_strings + 0x138) = 0xa000;
    show_timed_message((const char *)GADDR(VA_g_keymap_table + 0xba));
    G32(VA_g_video_mode_width) = 0x140;
    G32(VA_g_screen_height + 0x4) = 0xcccc;
    G32(VA_g_video_linear_flag) = 0;
    G16(VA_g_video_mode_flags) = 0;
    build_scanline_dest_offset_table();
    regs_t io2; memset(&io2, 0, sizeof io2);
    vd_bridge(0x2e140, &io2);                           /* blank_active_video_page */
    goto common_tail;
  }
}

/* match_vesa_video_modes 0x27fbf: EAX=target mode-descriptor table (param_2 entries, stride 0xa:
 * +0 width, +2 height, +4 format, +6 mode#(out), +8 pitch(out)), EDX=entry count. Probe VESA
 * (dpmi_simulate_real_int = int 10h get-VBE-info -> info block), copy the VBE mode list
 * (copy_vesa_mode_list_block [L]), and for each supported mode (vesa_get_mode_info_block) compute
 * its format code + resolve the mode# and byte-pitch into any matching target-table entry (by
 * format+width+height). The DPMI/VESA int callees are host-serviced -> bridged. */
void match_vesa_video_modes(uint32_t table, uint32_t count)
{
    uint16_t modelist[128];                             /* [ebp+0x14]: copied VBE mode list */
    regs_t io;

    memset(&io, 0, sizeof io);
    io.eax = table; io.edx = count;
    vd_bridge(0x27eec, &io);                            /* dpmi_simulate_real_int -> EAX = VBE info block */
    uint32_t info = io.eax;
    if (info == 0) return;

    copy_vesa_mode_list_block((const uint8_t *)(uintptr_t)info, (uint8_t *)modelist);

    const uint8_t *table_p = (const uint8_t *)(uintptr_t)table;
    for (int m = 0; m < 128; m++) {
        uint16_t mode = modelist[m];
        if ((mode & 0xff00) == 0) continue;            /* or ah,ah; je (skip low mode) */
        if (mode == 0xffff) break;                     /* cmp ax,-1; je (terminator) */

        memset(&io, 0, sizeof io); io.eax = mode;
        vd_bridge(0x27f6f, &io);                       /* vesa_get_mode_info_block(mode) -> EAX = info */
        uint32_t mib = io.eax;
        if (mib == 0) continue;
        const uint8_t *b = (const uint8_t *)(uintptr_t)mib;
        if ((*(const uint16_t *)b & 1) == 0) continue; /* attr bit0 (supported) clear -> skip */

        uint32_t fmt = ((uint32_t)b[0x20] << 16) | ((uint32_t)b[0x22] << 8) | b[0x24];
        uint8_t bpp = b[0x19], al;
        if (bpp == 0x20 || bpp == 8)   al = bpp;
        else if (fmt == 0x100800)      al = 0x18;
        else if (fmt == 0xa0500)       al = 0xf;
        else if (fmt == 0xb0500)       al = 0x10;
        else if (b[0x1b] == 4)         al = 8;
        else                           al = bpp;

        const uint8_t *e = table_p;
        for (uint32_t i = 0; i < count; i++) {
            if (*(const uint16_t *)(e + 4) == al
                && *(const uint16_t *)(e + 0) == *(const uint16_t *)(b + 0x12)
                && *(const uint16_t *)(e + 2) == *(const uint16_t *)(b + 0x14)) {
                *(uint16_t *)(e + 6) = mode;
                *(uint16_t *)(e + 8) = *(const uint16_t *)(b + 0x10);
            }
            e += 0xa;
        }
    }
}

/* ---------------------- grayscale_background_view (0x12b45) ----------------------
 * void: ensure the grayscale LUT is built, mark the render view region dirty, then desaturate
 * every pixel of the view region in the back buffer through g_view_grayscale_lut. View region:
 * left [0x85ce0], top [0x85ce4], width [0x85cd8], height [0x85cdc]; hires [0x90cbd] doubles
 * top+height. Pitch [0x85498], back buffer [0x90a98]. (The (ah<<8)+[0x85d08] value computed at
 * entry is DEAD — pushed/popped around the dirty call, never consumed.) Fully register-transparent
 * (pushal/popal). Processes 2 px/inner iteration; a trailing odd column is skipped (width>>1). */
void grayscale_background_view(void)
{
    if (G8(VA_g_view_grayscale_lut_valid) == 0) build_view_grayscale_lut();

    int32_t top    = G32(VA_g_view_y);
    int32_t height = G32(VA_g_view_h);
    int32_t left   = G32(VA_g_view_x);
    int32_t width  = G32(VA_g_view_w);
    if (G8(VA_g_hires_line_doubling_flag) != 0) { top += top; height += height; }        /* hires: double the y extent */

    int32_t right  = width + left - 1;
    int32_t bottom = top + height - 1;
    add_dirty_rect((uint32_t)left, top, (uint32_t)right, (uint32_t)bottom);

    int32_t pitch = G32(VA_g_screen_pitch);
    volatile uint8_t *fb  = (volatile uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_framebuffer_ptr);   /* A4 host ptr */
    const volatile uint8_t *lut = (const volatile uint8_t *)GADDR(VA_g_view_grayscale_lut);
    uint32_t row  = (uint32_t)(top * pitch + left);
    int32_t  half = (int32_t)((uint32_t)width >> 1);               /* 2 px per inner step */
    for (int32_t r = 0; r < height; r++) {
        uint32_t p = row;
        for (int32_t k = 0; k < half; k++) {
            fb[p]     = lut[fb[p]];
            fb[p + 1] = lut[fb[p + 1]];
            p += 2;
        }
        row += (uint32_t)pitch;
    }
}
