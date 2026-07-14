/* lift_text_font.c — verified-C lifts for the `text_font` subsystem.
 *
 * The game's glyph rasterizer + shadow/outline renderer + positioned-text wrapper. The
 * innermost tier of all in-game UI text (menus, timed messages, dialogue panels, choice
 * menus, inventory labels). See docs/reference/lift/text_font.md.
 *
 * Functions lifted here (canon VAs; runtime = canon + OBJ_DELTA):
 *   draw_text_glyph_with_shadow      0x13f2e  — independent glyph blit (descriptor struct);
 *                                               3 layout paths (ModeX / hires word-double / plain).
 *   draw_text_outline_prepass_helper 0x14f61  — outline/shadow mask prepass (static; shares
 *                                               draw_text_to_buffer's frame in the original).
 *   draw_text_to_buffer              0x14d04  — the control-coded string renderer (inlines its
 *                                               own glyph blit; calls the prepass).
 *   draw_text_at_screen_xy           0x1a079  — positioned wrapper -> draw_text_to_buffer.
 *
 * ABI / behavior was transcribed STRICTLY FROM THE DISASM (the corpus decompile is Borland-
 * cspec-on-Watcom and unreliable for register args / multi-reg returns). Two non-obvious
 * faithful details, both confirmed against the bytes:
 *  - The "page-2" immediates 0x1e242 / 0x12d687 in 0x13f2e are SMC PLACEHOLDERS overwritten at
 *    mode-1/mode-3 entry with the runtime dest/bg STRIDE; the lift just uses the runtime stride.
 *  - The shadow colour is a BG-INDEXED lookup: `mov al,[eax]` dereferences eax whose LOW BYTE was
 *    just overwritten by the bg pixel, so shadow = *((shad_base & 0xffffff00) | bg_pixel), where
 *    shad_base = *g_world_shading_table_ptr + 0xa00. This holds in ALL three paths (the decompile's
 *    CONCAT31 chain encodes exactly this).
 */
#include <stdint.h>
#include "common.h"

/* byte/word read+write through a flat 32-bit-style address; volatile so every faithful
 * read/write is emitted (write-set fidelity, like G8/G16/G32). */
#define RB(a)    (*(volatile uint8_t  *)(uintptr_t)(a))
#define WB(a,v)  (*(volatile uint8_t  *)(uintptr_t)(a) = (uint8_t)(v))
#define RW(a)    (*(volatile uint16_t *)(uintptr_t)(a))
#define WW(a,v)  (*(volatile uint16_t *)(uintptr_t)(a) = (uint16_t)(v))

/* ============================ draw_text_glyph_with_shadow (0x13f2e) ============================ */
/* EAX = pointer to a glyph-cell descriptor (pushal/popal => register-transparent; writes memory
 * only). Descriptor fields (dword unless noted):
 *   +0x00 src ptr     +0x04 dest ptr   +0x08 behind/bg ptr   +0x0c width    +0x10 row count
 *   +0x14 src stride  +0x18 dest stride +0x1c bg stride       +0x20 mode (1=ModeX,3=hires,else plain)
 *   +0x24 byte scratch (mode 3 only; = (shad_base>>8)&0xff)
 * Side effects on the descriptor: [+0x10] is decremented to 0 by the row loop; [+0x24] is written
 * in mode 3 (both reproduced for faithfulness). */
void draw_text_glyph_with_shadow(uint32_t desc)
{
    uint8_t *d = (uint8_t *)(uintptr_t)desc;
    uint32_t src_p   = *(uint32_t *)(d + 0x00);
    uint32_t dest_p  = *(uint32_t *)(d + 0x04);
    uint32_t bg_p    = *(uint32_t *)(d + 0x08);
    int32_t  width   = *(int32_t  *)(d + 0x0c);
    int32_t *prows   = (int32_t  *)(d + 0x10);
    int32_t  srcstr  = *(int32_t  *)(d + 0x14);
    int32_t  deststr = *(int32_t  *)(d + 0x18);
    int32_t  bgstr   = *(int32_t  *)(d + 0x1c);
    int32_t  mode    = *(int32_t  *)(d + 0x20);

    uintptr_t src   = src_p;
    uintptr_t dst   = dest_p;
    uintptr_t bg    = bg_p;
    /* probe = src - 3*srcstride - 2 (edx): the "3 rows above, 2 cols left" glyph-edge probe. */
    uintptr_t probe = (uintptr_t)((intptr_t)src_p - 3 * (intptr_t)srcstr - 2);

    uint32_t shad_ptr  = *(uint32_t *)(uintptr_t)GADDR(VA_g_world_shading_table_ptr);   /* g_world_shading_table_ptr (A4: raw runtime ptr) */
    uint32_t shad_base = shad_ptr + 0xa00;                          /* eax = shadow colour table */
#define SHAD(bgv) RB((uintptr_t)((shad_base & 0xffffff00u) | (uint8_t)(bgv)))

    if (width <= 4)
        return;

    if (mode == 1) {                                  /* ---- ModeX two-page ---- */
        for (int r = 3; r > 0; r--) {                 /* first 3 rows: plain transparency, 2 pages */
            for (int x = 0; x < width; x++) {
                uint8_t al = RB(src + x);
                if (al == 0) { WB(dst + x, RB(bg + x)); al = RB(bg + x + bgstr); }
                else         { WB(dst + x, al); }
                WB(dst + x + deststr, al);
            }
            src += srcstr; probe += srcstr; bg += 2 * bgstr; dst += 2 * deststr;
        }
        *prows -= 3;
        while (*prows > 0) {
            for (int x = 0; x < 2; x++) {             /* cols 0,1: no shadow */
                uint8_t al = RB(src + x);
                if (al == 0) { WB(dst + x, RB(bg + x)); al = RB(bg + x + bgstr); }
                else         { WB(dst + x, al); }
                WB(dst + x + deststr, al);
            }
            for (int x = 2; x < width; x++) {         /* cols 2..: shadow when transparent over an edge */
                uint8_t al = RB(src + x);
                if (al != 0) { WB(dst + x, al); WB(dst + x + deststr, al); }
                else {
                    uint8_t b = RB(bg + x);
                    if (RB(probe + x) != 0) {
                        WB(dst + x, SHAD(b));
                        WB(dst + x + deststr, SHAD(RB(bg + x + bgstr)));
                    } else {
                        WB(dst + x, b);
                        WB(dst + x + deststr, RB(bg + x + bgstr));
                    }
                }
            }
            src += srcstr; probe += srcstr; bg += 2 * bgstr; dst += 2 * deststr;
            (*prows)--;
        }
        return;
    }

    if (mode == 3) {                                  /* ---- hires word-doubled ---- */
        d[0x24] = (uint8_t)(shad_base >> 8);          /* faithful: stash shad_base byte1 (scratch) */
        for (int r = 3; r > 0; r--) {                 /* first 3 rows: plain, words, 2 pages */
            for (int x = 0; x < width; x++) {
                uint8_t al = RB(src + x);
                uint16_t w;
                if (al == 0) { w = RW(bg + 2*x); WW(dst + 2*x, w); w = RW(bg + 2*x + bgstr); }
                else         { w = (uint16_t)((al << 8) | al); WW(dst + 2*x, w); }
                WW(dst + 2*x + deststr, w);
            }
            src += srcstr; probe += srcstr; bg += 2 * bgstr; dst += 2 * deststr;
        }
        *prows -= 3;
        while (*prows > 0) {
            for (int x = 0; x < 2; x++) {             /* cols 0,1: no shadow */
                uint8_t al = RB(src + x);
                uint16_t w;
                if (al == 0) { w = RW(bg + 2*x); WW(dst + 2*x, w); w = RW(bg + 2*x + bgstr); }
                else         { w = (uint16_t)((al << 8) | al); WW(dst + 2*x, w); }
                WW(dst + 2*x + deststr, w);
            }
            for (int x = 2; x < width; x++) {
                uint8_t al = RB(src + x);
                if (al != 0) {
                    uint16_t w = (uint16_t)((al << 8) | al);
                    WW(dst + 2*x, w);
                    WW(dst + 2*x + deststr, w);
                } else if (RB(probe + x) == 0) {
                    uint16_t w = RW(bg + 2*x);
                    WW(dst + 2*x, w);
                    WW(dst + 2*x + deststr, RW(bg + 2*x + bgstr));
                } else {                              /* shadow: 4 bg-indexed byte lookups */
                    WB(dst + 2*x + 0,           SHAD(RB(bg + 2*x + 0)));
                    WB(dst + 2*x + 1,           SHAD(RB(bg + 2*x + 1)));
                    WB(dst + 2*x + deststr + 0, SHAD(RB(bg + 2*x + bgstr + 0)));
                    WB(dst + 2*x + deststr + 1, SHAD(RB(bg + 2*x + bgstr + 1)));
                }
            }
            src += srcstr; probe += srcstr; bg += 2 * bgstr; dst += 2 * deststr;
            (*prows)--;
        }
        return;
    }

    /* ---- plain ---- */
    for (int r = 3; r > 0; r--) {                     /* first 3 rows: plain transparency copy */
        for (int x = 0; x < width; x++) {
            uint8_t al = RB(src + x);
            if (al == 0) al = RB(bg + x);
            WB(dst + x, al);
        }
        src += srcstr; probe += srcstr; bg += bgstr; dst += deststr;
    }
    *prows -= 3;
    while (*prows > 0) {
        for (int x = 0; x < 2; x++) {                 /* cols 0,1: no shadow */
            uint8_t al = RB(src + x);
            if (al == 0) al = RB(bg + x);
            WB(dst + x, al);
        }
        for (int x = 2; x < width; x++) {             /* cols 2..: shadow when transparent over an edge */
            uint8_t al = RB(src + x);
            if (al != 0) { WB(dst + x, al); }
            else {
                uint8_t b = RB(bg + x);
                WB(dst + x, RB(probe + x) != 0 ? SHAD(b) : b);
            }
        }
        src += srcstr; probe += srcstr; bg += bgstr; dst += deststr;
        (*prows)--;
    }
#undef SHAD
}

/* ============================ draw_text_outline_prepass_helper (0x14f61) ============================ */
/* Marks the 4/6 neighbours of each set glyph bit with value 1 (the outline/shadow mask), BEFORE
 * the glyph pixels are written. In the original it borrows draw_text_to_buffer's EBP frame and
 * saves/restores all registers (register-transparent, memory-only). Here it is a static helper
 * with state passed explicitly; the row-iteration mirrors the glyph blit exactly.
 *   width    = AL (glyph width; <=4 => nibble-packed two-rows-per-byte, else one byte per row)
 *   rowcount = AH
 *   rows_ptr = EBX (glyph bitmask rows)
 *   pen      = EDI (blit start = pen + topoff*pitch)
 *   pitch    = [ebp+0x10],  flags = [ebp+0x1c]
 * flags bit0 set (hires double): esi = pitch/2, mark 6 neighbours; clear: esi = pitch, mark 4. */
static void text_outline_prepass(int8_t width, int rowcount, uintptr_t rows_ptr,
                                 uintptr_t pen, uint32_t pitch, uint32_t flags)
{
    int dbl = (flags & 1) != 0;
    uint32_t esi = dbl ? (pitch >> 1) : pitch;

/* mark the neighbourhood of pixel cursor `e`. */
#define MARK(e) do {                                        \
        if (dbl) {                                          \
            WB((e) + esi - 1, 1); WB((e) + esi + 1, 1);     \
            WB((e) - 1, 1);       WB((e) + 1, 1);           \
            WB((e) - esi, 1);     WB((e) + 2*esi, 1);       \
        } else {                                            \
            WB((e) - 1, 1);       WB((e) + 1, 1);           \
            WB((e) - esi, 1);     WB((e) + esi, 1);         \
        }                                                   \
    } while (0)

    if (width <= 4) {                                  /* nibble-packed */
        uint8_t dl = 0, cl = 0;
        int rc = rowcount;
        do {
            if (dl == 0) { cl = RB(rows_ptr); rows_ptr++; }
            dl = (uint8_t)~dl;
            if (cl & 0xf0) {
                uintptr_t e = pen;
                int8_t dh = width;
                do {
                    int carry = cl & 0x80; cl = (uint8_t)(cl << 1);
                    if (carry) MARK(e);
                    e++;
                } while (--dh > 0);
            } else {
                cl = (uint8_t)(cl << 4);
            }
            pen += pitch;
        } while (--rc > 0);
    } else {                                           /* one byte per row */
        int rc = rowcount;
        do {
            uint8_t cl = RB(rows_ptr); rows_ptr++;
            if (cl != 0) {
                uintptr_t e = pen;
                int8_t dh = width;
                do {
                    int carry = cl & 0x80; cl = (uint8_t)(cl << 1);
                    if (carry) MARK(e);
                    e++;
                } while (--dh > 0);
            }
            pen += pitch;
        } while (--rc > 0);
    }
#undef MARK
}

/* ============================ draw_text_to_buffer (0x14d04) ============================ */
/* The glyph blit inlined inside draw_text_to_buffer: writes `ramp[ramp_idx + row]`-coloured pixels
 * for each set bit of the glyph bitmask. dbl = flags bit0 (hires; also write a copy at +pitch/2). */
static void text_blit_glyph(int8_t width, int rowcount, uintptr_t rows_ptr, uintptr_t pen,
                            const uint8_t *ramp, uint32_t ramp_idx, uint32_t pitch, int dbl)
{
    uint32_t half = pitch >> 1;
    if (width <= 4) {                                  /* nibble-packed (two rows per byte) */
        uint8_t dl = 0, cl = 0;
        int rc = rowcount;
        do {
            uint8_t ch = ramp[ramp_idx++];
            if (dl == 0) { cl = RB(rows_ptr); rows_ptr++; }
            dl = (uint8_t)~dl;
            if (cl & 0xf0) {
                uintptr_t e = pen;
                int8_t dh = width;
                do {
                    int carry = cl & 0x80; cl = (uint8_t)(cl << 1);
                    if (carry) { WB(e, ch); if (dbl) WB(e + half, ch); }
                    e++;
                } while (--dh > 0);
            } else {
                cl = (uint8_t)(cl << 4);
            }
            pen += pitch;
        } while (--rc > 0);
    } else {                                           /* one byte per row */
        int rc = rowcount;
        do {
            uint8_t ch = ramp[ramp_idx++];
            uint8_t cl = RB(rows_ptr); rows_ptr++;
            if (cl != 0) {
                uintptr_t e = pen;
                int8_t dh = width;
                do {
                    int carry = cl & 0x80; cl = (uint8_t)(cl << 1);
                    if (carry) { WB(e, ch); if (dbl) WB(e + half, ch); }
                    e++;
                } while (--dh > 0);
            }
            pen += pitch;
        } while (--rc > 0);
    }
}

/* EAX = control-coded string, EDX = dest ptr, EBX = pitch, ECX = flags.
 *  flag bit0 = hires (double pitch + 2nd copy at +pitch/2);  bit2 = skip outline prepass;
 *  bit3 = flat colour (0x01 fills ramp with the literal colour, no FS gradient table).
 * Control bytes: 0 ends; 0x0d/0x0a newline; 0x01 <c> sets the 12-entry colour ramp (FS gradient
 * unless bit3); 0x02 <lo><hi> advances the pen; other <0x0d are ignored. */
void draw_text_to_buffer(uint32_t str_p, uint32_t dest_p, uint32_t pitch_in, uint32_t flags)
{
    const uint8_t *font = (const uint8_t *)(uintptr_t)GADDR(VA_g_font_descriptor);   /* g_font_descriptor */
    uint32_t pitch = pitch_in;
    if (flags & 1) pitch += pitch;                                       /* hires: double pitch */
    uint32_t max_char    = *(const uint16_t *)(font + 0);
    uint32_t linestep    = *(const uint16_t *)(font + 2);
    uint32_t default_adv = *(const uint16_t *)(font + 4);
    uint32_t newline_adv = linestep * pitch;

    uintptr_t str       = str_p;
    uintptr_t pen       = (uintptr_t)dest_p + pitch + 1;                 /* lea edi,[edi+ebx+1] */
    uintptr_t line_base = pen;

    static uint8_t ramp[16];                                             /* 12 used ([ebp+0x24..]).
        * STATIC on purpose: the original never prefills this ramp (entry sets only [ebp+0x30]);
        * a {0x01,color}-prefixed string fills it (0x14ec6-0x14ef9), and a PREFIX-LESS string relies
        * on the ramp bytes LEFT ON THE STACK by the previous same-depth call — Watcom's fixed
        * frames make that deterministic, and the GDV gallery uses it as its hover-highlight
        * mechanism (render_menu_entry_list 0x2491f draws the {1,color} layout line, then the raw
        * name inherits its ramp). A fresh local made that UB: garbage colors, then invisible
        * black-on-black gallery names. static = the de-facto "last 0x01-set color" contract;
        * single-threaded engine, no reentrancy. */

    for (;;) {
        uint8_t c = RB(str); str++;
        if (c == 0)
            return;
        if (c == 0x0d) { pen = line_base + newline_adv; line_base = pen; continue; }
        if (c < 0x0d) {                                                  /* control bytes 1..0x0c */
            if (c == 1) {
                uint8_t color = RB(str); str++;
                if (flags & 8) {
                    for (int i = 0; i < 12; i++) ramp[i] = color;       /* flat */
                } else {
                    uint32_t fsb = g_os_sel_base ? g_os_sel_base((uint16_t)G16(VA_g_text_color_ramp_selector)) : 0;
                    for (int i = 0; i < 12; i++)                          /* FS gradient: fs:[(i<<8)|color] */
                        ramp[i] = RB(fsb + (uint32_t)(i << 8) + color);
                }
            } else if (c == 2) {
                uint16_t adv = RW(str); str += 2;
                pen += adv;
            } else if (c == 0x0a) {
                pen = line_base + newline_adv; line_base = pen;
            }
            continue;
        }
        if (c > max_char) { pen += default_adv; continue; }
        int16_t goff = *(const int16_t *)(font + 6 + c * 2);
        if (goff == 0) { pen += default_adv; continue; }
        const uint8_t *glyph = font + goff;
        uint16_t metrics  = *(const uint16_t *)glyph;
        uintptr_t rows_ptr = (uintptr_t)(glyph + 2);
        uint8_t mlow  = (uint8_t)(metrics & 0xff);
        uint8_t mhigh = (uint8_t)(metrics >> 8);
        uint32_t adv   = (uint32_t)(mlow & 0x0f) + 1;                     /* x-advance = lownibble+1 */
        int8_t  width  = (int8_t)(mlow & 0x0f);
        if (mlow & 0x20) width++;
        uint8_t  topoff   = (uint8_t)(mhigh >> 4);
        uint8_t  rowcount = (uint8_t)((mhigh & 0x0f) + 1);
        uintptr_t blit_pen = pen + (uintptr_t)topoff * pitch;
        if (!(flags & 4))
            text_outline_prepass(width, rowcount, rows_ptr, blit_pen, pitch, flags);
        text_blit_glyph(width, rowcount, rows_ptr, blit_pen, ramp, topoff, pitch, (flags & 1) != 0);
        pen += adv;
    }
}

/* ============================ draw_text_at_screen_xy (0x1a079) ============================ */
/* EAX = string, EBX = x, ECX = y, EDX = flags. Computes g_framebuffer_ptr + x + y*pitch (y/flags
 * doubled in hi-res) then calls draw_text_to_buffer. */
void draw_text_at_screen_xy(uint32_t str, uint32_t x, uint32_t y, uint32_t flags)
{
    if (G8(VA_g_hires_line_doubling_flag) != 0) {                                              /* g_hires_line_doubling_flag */
        flags += 1;
        y += y;
    }
    uint32_t pitch = (uint32_t)G32(VA_g_screen_pitch);                             /* g_screen_pitch */
    uint32_t dest  = (uint32_t)G32(VA_g_framebuffer_ptr) + x + y * pitch;             /* g_framebuffer_ptr + ... */
    draw_text_to_buffer(str, dest, pitch, flags);
}
