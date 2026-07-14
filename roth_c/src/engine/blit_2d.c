/* lift_blit_2d.c — verified-C lifts for the `blit_2d` subsystem.
 *
 * 2D drawing primitives: solid fills/clears, sprite/icon/overlay blitters, the save-under
 * backing store, the software cursor, and the screen slide transitions. Shared leaves lifted
 * earlier live in renderer.c (screen_xy_to_framebuffer_ptr 0x18040, clear_framebuffer_rect
 * 0x12cea, copy_nonzero_bytes 0x1426f/_2x 0x1428a, blit_save_region 0x130d4) and are reached
 * through engine.h.
 *
 * Framebuffer model (docs/reference/ROTH_renderer_notes.md): g_framebuffer_ptr 0x90a98 is the
 * active draw-target (a stored HOST address — deref raw, gotcha A4), g_screen_pitch 0x85498,
 * g_screen_height 0x8549c, g_hires_line_doubling_flag 0x90cbd (640-wide modes double every
 * y offset / row count).
 *
 * SMC cluster (gotchas C1/C2): draw_popup_shadow_border_smc 0x12dde is the PATCHER — in hires it
 * writes g_screen_pitch into seven displacement/limit/advance immediates inside the three
 * translucent-overlay fns (0x12f31/0x12f43/0x12f4d/0x12f5f in _block, 0x12fe4/0x12fed/0x13000
 * in _rows) before calling them. The static file bytes there are build-time placeholders
 * (0x1e240 = 123456 etc.), so the overlay lifts read those immediates LIVE from the code
 * bytes (G32 on the canon code address) and the SMC lift WRITES the live bytes — original and
 * lift stay interoperable in either direction.
 */
#include <string.h>
#include "common.h"

/* video_display bridge helpers (defined with the Group B mid blitters below) */
static void b2_add_dirty_rect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
static void b2_register_dirty_rect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
static void b2_set_vesa_bank(uint16_t dx_bank);
static void b2_set_vesa_bank_page2(void);

/* ======================================================================================
 * Group A — fill / clear / copy primitives
 * ====================================================================================== */

/* fill_rect_solid (0x12cd4, 22B): fill a width x height rect at (x,y) with the UI fill
 * colour byte [0x7f355] (replicated x4 for the rep stosd). Falls through into
 * clear_framebuffer_rect's common body at 0x12cee (same fill loop, EAX = the pattern).
 * ABI: EAX=x, EDX=y, EBX=width, ECX=height. Hires doubles the y offset AND the row count.
 * Row loop is dec/jg bottom-tested (B5): height<=0 still fills one row. */
void fill_rect_solid(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    uint8_t fill = G8(VA_g_rect_fill_color);                       /* al=[0x7f355], replicated ah/di */
    int32_t pitch = G32(VA_g_screen_pitch);                     /* g_screen_pitch */
    int32_t yoff = (int32_t)y * pitch;
    int32_t rows = (int32_t)height;
    if (G8(VA_g_hires_line_doubling_flag) != 0) { yoff += yoff; rows += rows; }
    uint8_t *p = (uint8_t *)(uintptr_t)((uint32_t)G32(VA_g_framebuffer_ptr) + x + (uint32_t)yoff);
    do {                                              /* dec edx; jg */
        memset(p, fill, width);                       /* rep stosd + rep stosb, all bytes == fill */
        p += pitch;
    } while (--rows > 0);
}

/* clear_buffer_rows (0x22760, 81B): zero `count` bytes per row for `rows` rows starting at
 * base + row0*stride + off, advancing `stride` per row, via mem_fill 0x4b360 (lifted —
 * math_util, called direct-C). ABI: EAX=base, EDX=off, EBX=row0, ECX=stride, stack args
 * [ebp+0x10]=count, [ebp+0x14]=rows; ret 8. Row loop is TOP-tested (jae): rows==0 fills none. */
void clear_buffer_rows(uint32_t base, uint32_t off, uint32_t row0, uint32_t stride,
                              uint32_t count, uint32_t rows)
{
    uint32_t p = base + row0 * stride + off;
    for (uint32_t i = 0; i < rows; i++) {             /* cmp/jae top-tested */
        mem_fill((void *)(uintptr_t)p, 0, count);
        p += stride;
    }
}

/* fill_span_solid (0x39fc0, 13B; DEAD): scaled-sprite span-emitter variant — loads the fill
 * WORD [0x89f10] (al=[0x89f10], ah=[0x89f11] — NOT forced equal like the 0x39fcd sibling),
 * latches the low byte into g_8a3b6, then falls into the shared word-fill tail at 0x39fd4
 * (render_world's render_sprite_span_fill_39fcd body): cx words of (al,ah) with an odd pixel
 * split off as a leading byte (edi even) or trailing byte (edi odd). Ends `jmp 0x39e52` (the
 * driver's shared loop tail — belongs to render_world; the span dispatcher in renderer.c already
 * routes case 0x39fc0). ABI: ECX=count (cx), EDI=dest offset, ES=screen. */
void fill_span_solid(uint32_t ecx, uint32_t edi, uint8_t *es_base)
{
    uint16_t w  = (uint16_t)G16(VA_g_das_palette_remap_prefix);
    uint8_t  al = (uint8_t)w, ah = (uint8_t)(w >> 8);
    G8(VA_g_sprite_span_flip + 0xa) = al;
    uint32_t cx  = ecx & 0xffffu;                     /* shr cx,1 (driver zero-extends ecx) */
    uint32_t odd = cx & 1u;
    cx >>= 1;
    int lead  = odd && (edi & 1u) == 0;               /* odd count, even edi: leading byte */
    int trail = odd && !lead;                         /* odd count, odd edi: trailing byte */
    if (lead)
        es_base[edi++] = al;
    for (uint32_t i = 0; i < cx; i++) {               /* rep stosw of ax */
        es_base[edi] = al; es_base[edi + 1] = ah; edi += 2;
    }
    if (trail)
        es_base[edi] = al;
}

/* ======================================================================================
 * Group C — translucent overlays (the popup drop-shadow border loops)
 *
 * All three blend destination pixels through the FS blend LUT: dst = fs:[(src<<8)|dst].
 * FS is a REGISTER INPUT (D5b flavour 2 — the caller draw_popup_shadow_border_smc loads
 * fs=[0x90c0e] before the calls), so the lifts take the resolved LUT base as a parameter.
 * The hires paths use the SMC-patched immediates — read LIVE (C1), see the header comment.
 * ====================================================================================== */

/* blit_translucent_overlay_block (0x12ec2, 171B): blend a 4x4 block at (x,y). Per-pixel
 * clips: row drawn only when (uint32)y < [0x8549c]; pixel only when (uint32)x < pitch
 * (hires: < the patched limit [0x12f31]). Hires blends BOTH interleaved lines ([edi] then
 * [edi+pitch]) and advances 2*pitch per row. src advances 4 bytes per row (drawn or not).
 * ABI: EAX=x, EDX=y, ESI=src, FS=blend LUT. pushal — no register outputs. */
void blit_translucent_overlay_block(int32_t x, int32_t y, const uint8_t *src,
                                           const uint8_t *fs_base)
{
    int32_t pitch = G32(VA_g_screen_pitch);
    int32_t off = y * pitch;
    if (G8(VA_g_hires_line_doubling_flag) == 0) {
        uint8_t *p = (uint8_t *)(uintptr_t)((uint32_t)G32(VA_g_framebuffer_ptr) + (uint32_t)(off + x));
        for (int32_t yy = y; yy < y + 4; yy++) {
            if ((uint32_t)yy < (uint32_t)G32(VA_g_screen_pitch + 0x4)) {
                uint8_t *q = p;
                const uint8_t *s = src;
                for (int32_t xx = x; xx < x + 4; xx++, q++, s++) {
                    if ((uint32_t)xx < (uint32_t)pitch)
                        *q = fs_base[((uint32_t)*s << 8) | *q];
                }
            }
            p += pitch;
            src += 4;
        }
    } else {
        /* hires: limit/displacement/advance immediates are SMC-patched to pitch / 2*pitch */
        uint32_t xlim   = (uint32_t)G32(0x12f31);     /* cmp eax,imm limit */
        uint32_t disp_r = (uint32_t)G32(0x12f43);     /* mov bl,[edi+imm] second-line read */
        uint32_t disp_w = (uint32_t)G32(0x12f4d);     /* mov [edi+imm],bl second-line write */
        uint32_t radv   = (uint32_t)G32(0x12f5f);     /* add edi,imm row advance */
        uint8_t *p = (uint8_t *)(uintptr_t)((uint32_t)G32(VA_g_framebuffer_ptr) + (uint32_t)(off + off + x));
        for (int32_t yy = y; yy < y + 4; yy++) {
            if ((uint32_t)yy < (uint32_t)G32(VA_g_screen_pitch + 0x4)) {
                uint8_t *q = p;
                const uint8_t *s = src;
                for (int32_t xx = x; xx < x + 4; xx++, q++, s++) {
                    if ((uint32_t)xx < xlim) {
                        uint8_t sb = *s;                       /* bh, kept for both lines */
                        q[0]      = fs_base[(uint16_t)(((uint32_t)sb << 8) | q[0])];
                        q[disp_w] = fs_base[(uint16_t)(((uint32_t)sb << 8) | q[disp_r])];
                    }
                }
            }
            p += radv;
            src += 4;
        }
    }
}

/* draw_shadow_border_edge_h (0x12f6d, 163B): blend up to 4 horizontal ROWS of `count`
 * pixels at (x,y) — one src byte per row. Row count = min(4, pitch - x) (SIGNED — the
 * original clamps by horizontal space; x >= pitch draws nothing). A row with a negative
 * framebuffer offset (or edi,edi; js) is skipped but still advances. The pixel run is
 * dec/jg bottom-tested (B5): count<=0 still blends one pixel per drawn row. Hires blends
 * the +pitch line FIRST then the base line (patched immediates), advances 2*pitch/row.
 * ABI: EAX=x, EDX=y, EBX=count, ESI=src, FS=blend LUT. No register outputs (EDX untouched). */
void draw_shadow_border_edge_h(int32_t x, int32_t y, uint32_t count,
                                          const uint8_t *src, const uint8_t *fs_base)
{
    int32_t pitch = G32(VA_g_screen_pitch);
    int32_t w = pitch - x;
    if (w >= 4) w = 4;                                /* cmp ebp,4; jl keeps the short count */
    const uint8_t *send = src + w;
    if (G8(VA_g_hires_line_doubling_flag) == 0) {
        int32_t off = y * pitch + x;
        for (; src < send; src++, off += pitch) {     /* top-tested cmp esi,ebp; jl */
            if (off < 0) continue;                    /* or edi,edi; js */
            uint8_t sb = *src;
            uint8_t *q = (uint8_t *)(uintptr_t)((uint32_t)G32(VA_g_framebuffer_ptr) + (uint32_t)off);
            int32_t n = (int32_t)count;
            do {                                      /* dec ecx; jg */
                *q = fs_base[((uint32_t)sb << 8) | *q];
                q++;
            } while (--n > 0);
        }
    } else {
        uint32_t disp_r = (uint32_t)G32(0x12fe4);     /* mov bl,[edi+imm] second-line read */
        uint32_t disp_w = (uint32_t)G32(0x12fed);     /* mov [edi+imm],al second-line write */
        uint32_t radv   = (uint32_t)G32(0x13000);     /* add edi,imm row advance */
        int32_t off = y * pitch; off += off; off += x;
        for (; src < send; src++, off += (int32_t)radv) {
            if (off < 0) continue;
            uint8_t sb = *src;
            uint8_t *q = (uint8_t *)(uintptr_t)((uint32_t)G32(VA_g_framebuffer_ptr) + (uint32_t)off);
            int32_t n = (int32_t)count;
            do {                                      /* +pitch line first, then the base line */
                q[disp_w] = fs_base[((uint32_t)sb << 8) | q[disp_r]];
                q[0]      = fs_base[((uint32_t)sb << 8) | q[0]];
                q++;
            } while (--n > 0);
        }
    }
}

/* draw_shadow_border_edge_v (0x13010, 82B): blend 4 vertical COLUMNS of `height` rows
 * at (x,y) — one src byte per column. Hires doubles the start offset AND the height (row
 * advance stays one pitch — the doubled lines are contiguous). A column with a negative
 * framebuffer offset is skipped but still advances (offset +1 per column, so a run that
 * starts negative can come back in range). Row loop is dec/jg bottom-tested (B5): height<=0
 * still blends one row per drawn column. LUT indexed fs:[bx] (16-bit — bh|bl only).
 * ABI: EAX=x, EDX=y, ECX=height, ESI=src, FS=blend LUT. No register outputs. */
void draw_shadow_border_edge_v(int32_t x, int32_t y, uint32_t height,
                                            const uint8_t *src, const uint8_t *fs_base)
{
    int32_t pitch = G32(VA_g_screen_pitch);
    int32_t off = y * pitch;
    int32_t h = (int32_t)height;
    if (G8(VA_g_hires_line_doubling_flag) != 0) { off += off; h += h; }
    off += x;
    const uint8_t *send = src + 4;
    for (; src < send; src++, off++) {
        if (off < 0) continue;                        /* or edi,edi; js */
        uint8_t sb = *src;
        uint8_t *q = (uint8_t *)(uintptr_t)((uint32_t)G32(VA_g_framebuffer_ptr) + (uint32_t)off);
        int32_t n = h;
        do {                                          /* dec ecx; jg */
            *q = fs_base[(uint16_t)(((uint32_t)sb << 8) | *q)];
            q += pitch;
        } while (--n > 0);
    }
}

/* ======================================================================================
 * Group B (leaves) — pure sprite / icon blitters
 * ====================================================================================== */

/* clip_sprite_extents_to_screen (0x11c3c, 109B): clamp a sprite's draw extents to the
 * screen. MULTI-REG in/out (A1 — the corpus decompile drops most of it): EAX=x, EDX=y,
 * EBX=src pixel ptr, ECX packed (CL=width, CH=height; upper 16 bits preserved). Clips top
 * (skipping -y source rows: src += (uint8)(-y)*width via 8-bit MUL + CWDE — a product
 * > 0x7fff sign-extends NEGATIVE, reproduced), bottom/right (overflow vs [0x8549c]/pitch;
 * an overflow > 0x7f or an 8-bit-negative clipped dim = fully clipped), left (src += -x).
 * The x<0 path jumps STRAIGHT to the exit (no right clip after a left clip). Returns the
 * new packed ECX (0 = fully clipped, `sub ecx,ecx`); x/y/src written back with whatever
 * partial updates the exit path left in the registers. */
uint32_t clip_sprite_extents_to_screen(int32_t *x_io, int32_t *y_io, uint32_t *src_io,
                                              uint32_t wh)
{
    int32_t x = *x_io, y = *y_io;
    uint32_t src = *src_io;
    uint8_t w = (uint8_t)wh, h = (uint8_t)(wh >> 8);
    uint32_t ok = 0;

    if (y < 0) {                                      /* or edx,edx; jns */
        int32_t yh = y + h;                           /* add edx,ebp (32-bit) */
        if (yh <= 0) { y = yh; goto clipped; }        /* jle — edx keeps y+h */
        uint16_t prod = (uint16_t)((uint8_t)(h - (uint8_t)yh) * w);   /* mul cl (AX=AL*CL) */
        h = (uint8_t)yh;                              /* mov ch,dl */
        src += (uint32_t)(int32_t)(int16_t)prod;      /* cwde; add ebx,eax */
        y = 0;                                        /* sub edx,edx */
    } else {
        int32_t ovf = (int32_t)h - G32(VA_g_screen_pitch + 0x4) + y;  /* movzx; sub [0x8549c]; add edx */
        if (ovf > 0) {                                /* jle skips */
            if ((uint32_t)ovf > 0x7fu) goto clipped;  /* ja */
            h = (uint8_t)(h - (uint8_t)ovf);          /* sub ch,al */
            if ((int8_t)h < 0) goto clipped;          /* js */
        }
    }
    if (x < 0) {                                      /* or eax,eax; jns */
        int32_t xw = x + w;                           /* add eax,ebp */
        if (xw <= 0) { x = xw; goto clipped; }        /* jle — eax keeps x+w */
        uint8_t neww = (uint8_t)xw;                   /* mov cl,al */
        src += (uint32_t)w;                           /* add ebx,ebp */
        src -= (uint32_t)xw;                          /* sub ebx,eax  (net src += -x) */
        w = neww;
        x = 0;                                        /* sub eax,eax */
        goto done;                                    /* jmp 0x11ca2 — skips the right clip */
    } else {
        int32_t ovf = (int32_t)w - G32(VA_g_screen_pitch) + x;
        if (ovf > 0) {
            if ((uint32_t)ovf > 0x7fu) goto clipped;
            w = (uint8_t)(w - (uint8_t)ovf);          /* sub cl,al */
            if ((int8_t)w < 0) goto clipped;          /* js */
        }
    }
done:
    ok = 1;
clipped:
    *x_io = x; *y_io = y; *src_io = src;
    return ok ? ((wh & 0xffff0000u) | ((uint32_t)h << 8) | w) : 0;
}

/* rle_decode_scroll_segment (0x1374f, 95B): decode the next slice of a ByteRun-style RLE
 * stream into a 0x320-byte scroll segment, via an EBP descriptor (the caller's frame):
 *   +0    dst base            (read; +4 is REWRITTEN to this)
 *   +4    prev source         (read: cnt bytes copied dst<-prev first)
 *   +8    filled count        (read/write: += the decoded amount)
 *   +0x14 RLE stream cursor   (read/write)
 *   +0x24 remaining budget    (read/write: -= the decoded amount)
 * Decode budget = min(0x320 - cnt, remaining) (UNSIGNED min); the decode loop is entered
 * unconditionally (budget 0 still decodes one item) and a run can overshoot — the negative
 * leftover is added back to +8/+0x24 after. Opcode >= 0xf1 = run of (op-0xf0) x next byte;
 * else literal. Preserves ECX/EBX/ESI/EDI (EAX clobbered). */
void rle_decode_scroll_segment(uint8_t *d)
{
    uint32_t cnt   = *(uint32_t *)(void *)(d + 8);
    uint8_t *dst0  = (uint8_t *)(uintptr_t)*(uint32_t *)(void *)(d + 0);
    const uint8_t *prev = (const uint8_t *)(uintptr_t)*(uint32_t *)(void *)(d + 4);
    *(uint32_t *)(void *)(d + 4) = (uint32_t)(uintptr_t)dst0;
    for (uint32_t i = 0; i < cnt; i++) dst0[i] = prev[i];   /* rep movsb (forward) */

    uint8_t *dst = dst0 + cnt;
    const uint8_t *rle = (const uint8_t *)(uintptr_t)*(uint32_t *)(void *)(d + 0x14);
    int32_t bx = 0x320 - (int32_t)cnt;
    uint32_t rem = *(uint32_t *)(void *)(d + 0x24);
    if ((uint32_t)bx > rem) bx = (int32_t)rem;               /* jbe (unsigned min) */
    *(uint32_t *)(void *)(d + 8)    += (uint32_t)bx;
    *(uint32_t *)(void *)(d + 0x24) -= (uint32_t)bx;

    for (;;) {                                               /* entered unconditionally */
        uint8_t al = *rle++;
        if (al < 0xf1) {
            *dst++ = al;
            if (--bx > 0) continue;                          /* dec ebx; jg */
            break;
        }
        int32_t run = (int32_t)al - 0xf0;                    /* 1..15 */
        bx -= run;
        al = *rle++;
        for (int32_t i = 0; i < run; i++) *dst++ = al;       /* rep stosb */
        if (bx > 0) continue;                                /* or ebx,ebx; jg */
        break;
    }
    *(uint32_t *)(void *)(d + 8)    -= (uint32_t)bx;         /* bx <= 0: give back overshoot */
    *(uint32_t *)(void *)(d + 0x24) += (uint32_t)bx;
    *(uint32_t *)(void *)(d + 0x14) = (uint32_t)(uintptr_t)rle;
}

/* blit_opaque_rect (0x13183, 110B): copy a packed width x rows source block to the
 * framebuffer dest. ABI: EAX=src, EDX=dst ptr, EBX=width, ECX=rows. Normal: memcpy row,
 * dst += pitch. Hires: each source row is written to BOTH interleaved lines ([edi+pitch]
 * first, then [edi] — pitch read LIVE, not SMC), dst += 2*pitch. Bottom-tested dec/jg
 * (B5): rows<=0 still copies one row. */
void blit_opaque_rect(uint32_t src, uint32_t dst, uint32_t width, uint32_t rows)
{
    int32_t pitch = G32(VA_g_screen_pitch);
    const uint8_t *s = (const uint8_t *)(uintptr_t)src;
    uint8_t *p = (uint8_t *)(uintptr_t)dst;
    int32_t n = (int32_t)rows;
    if (G8(VA_g_hires_line_doubling_flag) == 0) {
        do {
            memcpy(p, s, width);                             /* rep movsd + movsb */
            s += width; p += pitch;
        } while (--n > 0);
    } else {
        do {
            for (uint32_t i = 0; i < width; i++) { p[pitch + (int32_t)i] = s[i]; p[i] = s[i]; }
            s += width; p += pitch * 2;                      /* lea edi,[edi+ebx*2] */
        } while (--n > 0);
    }
}

/* ---- the transparent-sprite viewmodel blitter family (0x13be0/0x13c60/0x13cf0/0x13d90) ----
 * Shared skeleton, four variants: x1/x2 horizontal doubling, unshaded/shaded (shade LUT via
 * fs=[0x90c0e], row = the clamped AL input). Inputs: EBX=column-record cursor ({u16 xoff,
 * u16 count} per source row), ESI=src texels, EDI=dest, EDX=dest row count, EBP=descriptor
 * (+0 screen X, +0x10 right limit, +0x1c record end, +0x20 v-accumulator [MUTATED],
 * +0x24 v-step; shaded also writes +0x28 = packed shade). Per dest row: draw the current
 * source record (left/right clipped; 0-texel = transparent skip; a record of 0 skips the
 * row body), then acc += step; when acc crosses 0x10000 advance (acc>>16) source records
 * (esi += record count each) and exit if the record cursor reaches +0x1c. The pixel loop is
 * dec/jg bottom-tested (a nonzero record with count 0 still draws one texel). */
static void b2_vm_sprite_core(uint32_t colrec, uint32_t src, uint32_t dst, int32_t rows,
                              int32_t *desc, int dbl, const uint8_t *lut)
{
    uint32_t shade_hi = lut ? ((uint32_t)desc[0x28 / 4] & 0xff00u) : 0;
    for (;;) {
        uint32_t rec = *(uint32_t *)(uintptr_t)colrec;       /* mov ecx,[ebx] */
        if (rec != 0) {
            int32_t off = (int32_t)(rec & 0xffffu);          /* movzx edx,cx */
            int32_t cnt = (int32_t)(rec >> 16);
            uint32_t di = dst + (uint32_t)off;               /* add edi,edx (x2: twice) */
            if (dbl) di += (uint32_t)off;
            uint32_t si = src;
            int32_t sx = off + desc[0];                      /* add edx,[ebp] */
            int drop = 0;
            if (sx < 0) {                                    /* left clip */
                di -= (uint32_t)sx; if (dbl) di -= (uint32_t)sx;
                si -= (uint32_t)sx;
                cnt += sx;
                if (cnt <= 0) drop = 1; else sx = 0;
            }
            if (!drop) {
                int32_t right = sx + cnt;                    /* add edx,ecx */
                if (right > desc[0x10 / 4]) {                /* right clip */
                    right -= cnt;                            /* sub edx,ecx */
                    cnt = desc[0x10 / 4] - right;
                    if (cnt <= 0) drop = 1;
                }
            }
            if (!drop) {
                do {                                         /* dec ecx; jg */
                    uint8_t al = *(const uint8_t *)(uintptr_t)si; si++;
                    if (al != 0) {
                        uint8_t v = lut ? lut[shade_hi | al] : al;
                        *(uint8_t *)(uintptr_t)di = v;
                        if (dbl) *(uint8_t *)(uintptr_t)(di + 1) = v;
                    }
                    di += dbl ? 2u : 1u;
                } while (--cnt > 0);
            }
        }
        uint32_t acc = (uint32_t)desc[0x20 / 4] + (uint32_t)desc[0x24 / 4];
        if (acc >= 0x10000u) {                               /* cmp/jb */
            int32_t k = (int32_t)(acc >> 16);
            do {                                             /* skip k source records */
                src += (uint32_t)*(uint16_t *)(uintptr_t)(colrec + 2);
                colrec += 4;
            } while (--k > 0);
            acc &= 0xffffu;
            if (colrec >= (uint32_t)desc[0x1c / 4]) return;  /* jae -> ret (acc NOT stored) */
        }
        desc[0x20 / 4] = (int32_t)acc;
        dst += (uint32_t)G32(VA_g_screen_pitch);
        if (--rows <= 0) return;                             /* dec edx; jg */
    }
}

void blit_transparent_sprite_clipped(uint32_t colrec, uint32_t src, uint32_t dst,
                                            int32_t rows, int32_t *desc)          /* 0x13be0 */
{ b2_vm_sprite_core(colrec, src, dst, rows, desc, 0, NULL); }

void blit_transparent_sprite_clipped_x2(uint32_t colrec, uint32_t src, uint32_t dst,
                                               int32_t rows, int32_t *desc)       /* 0x13c60 */
{ b2_vm_sprite_core(colrec, src, dst, rows, desc, 1, NULL); }

/* shaded prologue: eax = movzx(al); clamp >= 0x20 -> 0x1f; ah=al; [ebp+0x28]=eax; LUT row =
 * (shade<<8) via fs=[0x90c0e]. */
static const uint8_t *b2_shade_prime(uint32_t shade_al, int32_t *desc)
{
    uint32_t s = shade_al & 0xffu;
    if (s >= 0x20u) s = 0x1fu;
    desc[0x28 / 4] = (int32_t)((s << 8) | s);
    return (const uint8_t *)(uintptr_t)
        (g_os_sel_base ? g_os_sel_base((uint16_t)G16(VA_g_text_color_ramp_selector)) : 0);
}

void blit_transparent_sprite_clipped_shaded(uint32_t shade_al, uint32_t colrec,
                                                   uint32_t src, uint32_t dst,
                                                   int32_t rows, int32_t *desc)   /* 0x13cf0 */
{ b2_vm_sprite_core(colrec, src, dst, rows, desc, 0, b2_shade_prime(shade_al, desc)); }

void blit_transparent_sprite_clipped_x2_shaded(uint32_t shade_al, uint32_t colrec,
                                                      uint32_t src, uint32_t dst,
                                                      int32_t rows, int32_t *desc) /* 0x13d90 */
{ b2_vm_sprite_core(colrec, src, dst, rows, desc, 1, b2_shade_prime(shade_al, desc)); }

/* draw_translucent_icon_spans (0x13e81, 74B): blend the held-item icon's span rows through
 * the fs=[0x90be2] blend LUT. Source rows: [skip, runlen, texels...]; a drawn run writes a
 * 0 (black outline) byte before AND after it; 0-texels inside are transparent but the
 * outline bytes still surround them; runlen 0 = empty row (no outline). ABI: EAX=dst,
 * EDX=src, EBX=dest row stride, ECX=row count (never pass 0 — H-gotcha; dec/jg). */
void draw_translucent_icon_spans(uint32_t dst, uint32_t src, uint32_t stride,
                                        int32_t rows)
{
    const uint8_t *lut = (const uint8_t *)(uintptr_t)
        (g_os_sel_base ? g_os_sel_base((uint16_t)G16(VA_g_transparency_blend_selector)) : 0);
    const uint8_t *si = (const uint8_t *)(uintptr_t)src;
    do {
        uint8_t *di = (uint8_t *)(uintptr_t)dst + si[0];     /* leading skip */
        int32_t run = si[1];
        si += 2;
        if (run != 0) {
            *di++ = 0;                                       /* left outline */
            do {                                             /* dec eax; jg */
                uint8_t s = *si++;
                if (s != 0) *di = lut[((uint32_t)s << 8) | *di];
                di++;
            } while (--run > 0);
            *di = 0;                                         /* right outline */
        }
        dst += stride;                                       /* pop edi; add edi,ebp */
    } while (--rows > 0);
}

/* blit_image_scaled_to_framebuffer (0x202d5, 296B; Watcom C, ret 0xc): present a cutscene
 * frame block at (x,y) with scaling variants. ABI: EAX=src, EDX=x, EBX=y, ECX=width; stack
 * args rows / mode / submode. submode==1 -> source row stride doubles (skip odd source
 * rows) and submode becomes 2. mode==4 -> 2x horizontal pixel doubling (width halved,
 * SIGNED sar) — with submode==4 also 2x vertical (each pixel to a 2x2 block, rows/2, dest
 * advance ONE pitch per row); without, a continuous doubling loop with NO per-row dest
 * re-base (assumes 2*width == pitch). mode!=4: submode==4 -> line-double each source row
 * to two dest lines (rows/2); else plain row copy with the (possibly doubled) source
 * stride. All row loops are top-tested. */
void blit_image_scaled_to_framebuffer(uint32_t src, uint32_t x, uint32_t y,
                                             uint32_t width, int32_t rows,
                                             int32_t mode, int32_t submode)
{
    int32_t w = (int32_t)width;
    int32_t srcstride = w;                                   /* [ebp-0x10] */
    if (submode == 1) { srcstride = w + w; submode = 2; }
    uint32_t dst = (uint32_t)G32(VA_g_framebuffer_ptr) + (uint32_t)((int32_t)y * G32(VA_g_screen_pitch)) + x;
    int32_t half_rows = rows >> 1;                           /* sar edi,1 */
    const uint8_t *s = (const uint8_t *)(uintptr_t)src;

    if (mode == 4) {
        w >>= 1;                                             /* sar ebx,1 */
        if (submode == 4) {
            /* 2x2: per src pixel write dst[0]/dst[1]/dst[pitch]/dst[pitch+1]; per row
             * advance ONE pitch (the doubled row is revisited by the continuous dest walk) */
            uint8_t *p = (uint8_t *)(uintptr_t)dst;
            for (int32_t i = 0; i < half_rows; i++) {
                for (int32_t j = 0; j < w; j++) {
                    int32_t pitch = G32(VA_g_screen_pitch);
                    uint8_t c = *s++;
                    p[0] = c; p[1] = c; p[pitch] = c; p[pitch + 1] = c;
                    p += 2;
                }
                p += G32(VA_g_screen_pitch);
            }
        } else {
            /* continuous 2x horizontal doubling, no per-row re-base */
            uint8_t *p = (uint8_t *)(uintptr_t)dst;
            for (int32_t i = 0; i < rows; i++) {
                for (int32_t j = 0; j < w; j++) {
                    uint8_t c = *s++;
                    p[0] = c; p[1] = c;
                    p += 2;
                }
            }
        }
    } else if (submode == 4) {
        /* line-double: each source row copied to two consecutive dest lines */
        uint8_t *p = (uint8_t *)(uintptr_t)dst;
        for (int32_t i = 0; i < half_rows; i++) {
            memcpy(p, s, (size_t)w);
            p += G32(VA_g_screen_pitch);
            memcpy(p, s, (size_t)w);
            s += w;
            p += G32(VA_g_screen_pitch);
        }
    } else {
        /* plain row copy, source advances srcstride (w, or 2w for submode-1 input) */
        uint8_t *p = (uint8_t *)(uintptr_t)dst;
        for (int32_t i = 0; i < rows; i++) {
            memcpy(p, s, (size_t)w);
            s += srcstride;
            p += G32(VA_g_screen_pitch);
        }
    }
}

/* ======================================================================================
 * Group F — screen slide transitions
 * ====================================================================================== */

static void b2_flush_dirty_rects(void)                                              /* 0x15dd9 */
{
    flush_dirty_rects();               /* lifted (video_display) — re-pointed */
}
static void b2_flip_video_page(uint32_t mode)                                       /* 0x2e1e8 */
{
    regs_t io; memset(&io, 0, sizeof io);
    io.va = 0x2e1e8u + OBJ_DELTA;
    io.eax = mode;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    host_flip_video_page(mode);                 /* image-free host present */
#endif
}

/* blit_slide_transition (0x12d27, 183B): one frame of the venetian-blind screen wipe.
 * EAX = descriptor {dst@0, src@4, rowlen@8, rows@0xc}, EDX = phase; fs = [0x90c0e] fade
 * LUT. Builds a per-ROW code table on the stack (symmetric from both ends: code =
 * byte2 of an accumulator seeded (phase-0x1f)<<17, stepped 0x800000/rows while < 0x640000),
 * then per row: code bit7 -> plain copy; code >= 0x3f -> black fill (src skipped);
 * else fade through LUT row code>>1 — an ODD code alternates two adjacent LUT rows per
 * pixel, phase-flipped by the ROW counter's parity (the `test [esp],1` on the pushed
 * counter). All loops dec/jg bottom-tested. pushal — no outputs. */
void blit_slide_transition(int32_t *desc, int32_t phase)
{
    const uint8_t *lut = (const uint8_t *)(uintptr_t)
        (g_os_sel_base ? g_os_sel_base((uint16_t)G16(VA_g_text_color_ramp_selector)) : 0);
    uint8_t table[0x200];
    int32_t rows = desc[0xc / 4];
    uint32_t step = 0x800000u / (uint32_t)rows;           /* div ecx */
    uint32_t acc = (uint32_t)((phase - 0x1f) << 17);
    int32_t lo = 0, hi = rows;
    do {                                                  /* cmp esi,edi; jge */
        hi--;                                             /* dec esi (loop top) */
        uint8_t code = (uint8_t)(acc >> 16);              /* ror/rol dance = byte2 */
        table[hi] = code;
        table[lo] = code;
        if ((int32_t)acc < 0x640000) acc += step;         /* jge skips */
        lo++;
    } while (hi >= lo);

    uint32_t src = (uint32_t)desc[4 / 4];
    uint32_t dst = (uint32_t)desc[0];
    int32_t r = desc[0xc / 4];
    int32_t ti = 0;
    do {
        uint8_t bh = table[ti++];
        int32_t len = desc[8 / 4];                        /* ebp = rowlen */
        if (bh & 0x80) {                                  /* js: plain copy row */
            do {
                *(uint8_t *)(uintptr_t)dst = *(const uint8_t *)(uintptr_t)src;
                src++; dst++;
            } while (--len > 0);
        } else if (bh >= 0x3f) {                          /* jb skips: black row */
            src += (uint32_t)len;
            do { *(uint8_t *)(uintptr_t)dst = 0; dst++; } while (--len > 0);
        } else {
            uint8_t b = (uint8_t)(bh >> 1);
            if (bh & 1) {                                 /* shr CF: dithered fade */
                uint8_t d = (uint8_t)(b + 1);             /* mov dh,bh; inc dh */
                if (r & 1) { uint8_t t = b; b = d; d = t; }   /* test [esp],1; xchg */
                do {
                    *(uint8_t *)(uintptr_t)dst =
                        lut[((uint32_t)b << 8) | *(const uint8_t *)(uintptr_t)src];
                    src++; dst++;
                    { uint8_t t = b; b = d; d = t; }      /* xchg bh,dh per pixel */
                } while (--len > 0);
            } else {                                      /* single-row fade */
                do {
                    *(uint8_t *)(uintptr_t)dst =
                        lut[((uint32_t)b << 8) | *(const uint8_t *)(uintptr_t)src];
                    src++; dst++;
                } while (--len > 0);
            }
        }
    } while (--r > 0);
}

/* play_screen_slide_in (0x1ffb7, 381B; BLOCKING timed loop — spins on the frame tick word
 * [0x90bcc], so live-swap needs interactive-lift mode, gotcha G3): slide the image *EDX in
 * over ~0x3e ticks. EAX = music-fade flag: 0 -> if a music sequence just finished, emit
 * event 0 + resume ([0x83c4c]=2); nonzero -> arm the fade-IN ramp ([0x83c4c]=1, volume
 * ramped from [0x71124] down by 2*t each frame). Per frame: blit_slide_transition(t),
 * add_dirty_rect(whole screen) + flush_dirty_rects + flip_video_page(3) (video_display
 * bridges). Afterwards the framebuffer is CLEARED (mem_fill [L]) and presented once more.
 * Audio callees are lifted C (audio subsystem closed). */
void play_screen_slide_in(uint32_t music_flag, uint32_t srcp)
{
    if (music_flag == 0 && is_music_sequence_finished() != 0) {
        G32(VA_g_dialogue_busy_flag + 0x162) = 2;
        emit_music_sequence_event(0);
        resume_music_sequence();
    }
    int32_t desc[5];
    desc[0] = G32(VA_g_framebuffer_ptr);
    desc[1] = *(int32_t *)(uintptr_t)srcp;
    desc[2] = G32(VA_g_screen_pitch);
    desc[3] = G32(VA_g_screen_height);
    int32_t last = (int32_t)(int16_t)G16(VA_g_frame_tick_counter);
    if (music_flag != 0 && is_music_sequence_finished() != 0)
        G32(VA_g_dialogue_busy_flag + 0x162) = 1;
    G32(VA_g_frame_time_scale) = 2;
    uint32_t t = 0, drawn = 0;
    do {
        t += (uint32_t)G32(VA_g_frame_time_scale);
        if (t != drawn) {
            drawn = t;
            if (G32(VA_g_dialogue_busy_flag + 0x162) == 1) {                      /* fade-in volume ramp */
                int32_t v = G32(VA_g_font_descriptor + 0x212) - (int32_t)(t + t);
                if (v > 0) emit_music_sequence_event((uint8_t)v);
                else {
                    emit_music_sequence_event(0);  /* xor eax,edx (v==v) */
                    resume_music_sequence();
                    G32(VA_g_dialogue_busy_flag + 0x162) = 2;
                }
            }
            blit_slide_transition(desc, (int32_t)drawn);
            b2_add_dirty_rect(0, 0, (uint32_t)(G32(VA_g_screen_pitch) - 1), (uint32_t)(G32(VA_g_screen_height) - 1));
            b2_flush_dirty_rects();
            b2_flip_video_page(3);
        }
        int32_t now = (int32_t)(int16_t)G16(VA_g_frame_tick_counter);
        G32(VA_g_frame_time_scale) = now - last;
        last = now;
    } while (t < 0x3e);                                   /* jb (unsigned) */
    mem_fill((void *)(uintptr_t)(uint32_t)G32(VA_g_framebuffer_ptr), 0,
                    (uint32_t)(G32(VA_g_screen_pitch) * G32(VA_g_screen_height)));
    b2_add_dirty_rect(0, 0, (uint32_t)(G32(VA_g_screen_pitch) - 1), (uint32_t)(G32(VA_g_screen_height) - 1));
    b2_flush_dirty_rects();
    b2_flip_video_page(3);
    if (G32(VA_g_dialogue_busy_flag + 0x162) == 1) {
        emit_music_sequence_event(0);
        resume_music_sequence();
        G32(VA_g_dialogue_busy_flag + 0x162) = 2;
    }
}

/* play_screen_slide_out (0x20134, 417B; BLOCKING timed loop — interactive-lift tier like
 * slide_in): slide the current screen OUT to the image *EAX (phase runs 0x40-t), with the
 * music fade-OUT ramp (volume 2*t up to [0x71124]; [0x83c4c]: 2 -> emit 0 + finalize ref ->
 * 1 -> 0 when the ramp completes). Ends by copying the target image over the framebuffer
 * and presenting. Tail `jmp 0x1fcdb` is a shared plain epilogue (return). */
void play_screen_slide_out(uint32_t srcp)
{
    if (G32(VA_g_dialogue_busy_flag + 0x162) == 2) {
        emit_music_sequence_event(0);
        finalize_audio_sequence_ref();
        G32(VA_g_dialogue_busy_flag + 0x162) = 1;
    } else if (is_music_sequence_finished() != 0) {
        G32(VA_g_dialogue_busy_flag + 0x162) = 0;
    }
    int32_t desc[5];
    desc[0] = G32(VA_g_framebuffer_ptr);
    desc[1] = *(int32_t *)(uintptr_t)srcp;
    desc[2] = G32(VA_g_screen_pitch);
    desc[3] = G32(VA_g_screen_height);
    int32_t vol = 0;                                      /* [ebp-4] */
    int32_t last = (int32_t)(int16_t)G16(VA_g_frame_tick_counter);
    G32(VA_g_frame_time_scale) = 2;
    uint32_t t = 0, drawn = 0;
    do {
        t += (uint32_t)G32(VA_g_frame_time_scale);
        if (t != drawn) {
            drawn = t;
            if (G32(VA_g_dialogue_busy_flag + 0x162) == 1 && vol != G32(VA_g_font_descriptor + 0x212)) {
                int32_t v = (int32_t)(t + t);
                vol = v;
                if ((uint32_t)v < (uint32_t)G32(VA_g_font_descriptor + 0x212)) {   /* jae */
                    emit_music_sequence_event((uint8_t)v);
                } else {
                    emit_music_sequence_event((uint8_t)(uint32_t)G32(VA_g_font_descriptor + 0x212));
                    G32(VA_g_dialogue_busy_flag + 0x162) = 0;
                    vol = G32(VA_g_font_descriptor + 0x212);
                }
            }
            blit_slide_transition(desc, 0x40 - (int32_t)drawn);
            b2_add_dirty_rect(0, 0, (uint32_t)(G32(VA_g_screen_pitch) - 1), (uint32_t)(G32(VA_g_screen_height) - 1));
            b2_flush_dirty_rects();
            b2_flip_video_page(3);
        }
        int32_t now = (int32_t)(int16_t)G16(VA_g_frame_tick_counter);
        G32(VA_g_frame_time_scale) = now - last;
        last = now;
    } while (t < 0x3e);
    {
        uint32_t n = (uint32_t)(G32(VA_g_screen_pitch) * G32(VA_g_screen_height));
        memcpy((void *)(uintptr_t)(uint32_t)G32(VA_g_framebuffer_ptr),
               (const void *)(uintptr_t)*(uint32_t *)(uintptr_t)srcp, n);   /* repne movsb */
    }
    b2_add_dirty_rect(0, 0, (uint32_t)(G32(VA_g_screen_pitch) - 1), (uint32_t)(G32(VA_g_screen_height) - 1));
    b2_flush_dirty_rects();
    b2_flip_video_page(3);
    if (G32(VA_g_dialogue_busy_flag + 0x162) == 1) {
        emit_music_sequence_event((uint8_t)(uint32_t)G32(VA_g_font_descriptor + 0x212));
        G32(VA_g_dialogue_busy_flag + 0x162) = 0;
    }
}

/* snapshot_screen_and_slide_out (0x20b91, 133B): snapshot the framebuffer into a fresh
 * DAS-cache pool block ([0x83d18] handle; any stale one freed first), slide out to it
 * (play_screen_slide_out — which redraws the SAME pixels, i.e. a slide-out over a frozen
 * copy of the screen), then free the block. CALL-CLOSED except the slide's own bridges:
 * pool_free/alloc_handle + das_cache_make_room are lifted C. A failed alloc skips the
 * slide entirely. */
void snapshot_screen_and_slide_out(void)
{
    uint32_t size = (uint32_t)(G32(VA_g_screen_pitch) * G32(VA_g_screen_height));
    if (G32(VA_g_screen_backup_handle) != 0) {
        pool_free_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle),
                                (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_screen_backup_handle));
        G32(VA_g_screen_backup_handle) = 0;
    }
    das_cache_make_room(size);
    uint32_t h = pool_alloc_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle),
                                          (int32_t)size);
    G32(VA_g_screen_backup_handle) = (int32_t)h;
    if (h == 0) return;
    memcpy((void *)(uintptr_t)*(uint32_t *)(uintptr_t)h,
           (const void *)(uintptr_t)(uint32_t)G32(VA_g_framebuffer_ptr), size);   /* repne movsb */
    play_screen_slide_out((uint32_t)G32(VA_g_screen_backup_handle));            /* eax = the handle */
    pool_free_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle),
                            (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_screen_backup_handle));
    G32(VA_g_screen_backup_handle) = 0;
}

/* ======================================================================================
 * Group E — the software cursor chain
 * ====================================================================================== */

/* blit_software_cursor (0x11ce1, 523B): RESTORE the pixels a blit_sprite_save_under record
 * saved — the exact mirror of the save: mode-dispatched ([0x146d8] LFB / [0x90c08]+[0x76634]
 * raw/banked / planar), and each path only proceeds when the record's TYPE byte (+7) matches
 * the current mode's expectation (a stale record from another mode is ignored). Clears the
 * record's surface ptr (+0) = the "armed" flag. Record data: {sprite,under} words (lores) /
 * {sprite,under1,under2,x} dwords (hires); only pixels whose saved sprite byte is nonzero
 * are restored. Planar paths re-select each plane (out 0x3c5 via g_os_port_out — write
 * only, no read-map needed); banked path re-walks the 16-bit window with page2 wraps and
 * restores the entry bank at exit. ESI=record. */
void blit_software_cursor(uint32_t rec)
{
    uint8_t type = *(uint8_t *)(uintptr_t)(rec + 7);
    if (G32(VA_g_linear_framebuffer_ptr) != 0) {                              /* ---- VESA LINEAR ---- */
        int32_t modew = G32(VA_g_video_mode_width);
        if (G8(VA_g_hires_line_doubling_flag) == 0) {                           /* lores: expects 0x11 */
            if (type != 0x11) return;
            uint32_t di = *(uint32_t *)(uintptr_t)rec;
            *(uint32_t *)(uintptr_t)rec = 0;
            uint32_t wh = *(uint32_t *)(uintptr_t)(rec + 4);
            uint32_t si = rec + 0xc;
            int32_t r = (int32_t)((wh >> 8) & 0xff);
            do {
                uint32_t d2 = di;
                int32_t k = (int32_t)(wh & 0xff);
                do {
                    uint16_t ax = *(uint16_t *)(uintptr_t)si; si += 2;
                    if ((uint8_t)ax != 0) *(uint8_t *)(uintptr_t)d2 = (uint8_t)(ax >> 8);
                    d2++;
                } while (--k > 0);
                di += (uint32_t)modew;
            } while (--r > 0);
            return;
        }
        if (type != 0x12) return;                         /* hires */
        uint32_t di = *(uint32_t *)(uintptr_t)rec;
        *(uint32_t *)(uintptr_t)rec = 0;
        uint32_t wh = *(uint32_t *)(uintptr_t)(rec + 4);
        uint32_t si = rec + 0xc;
        int32_t r = (int32_t)((wh >> 8) & 0xff);
        do {
            uint32_t d2 = di;
            int32_t k = (int32_t)(wh & 0xff);
            do {
                uint32_t e = *(uint32_t *)(uintptr_t)si; si += 4;
                if ((uint8_t)e != 0) {
                    *(uint8_t *)(uintptr_t)d2 = (uint8_t)(e >> 8);           /* under1 */
                    *(uint8_t *)(uintptr_t)(d2 + (uint32_t)modew)            /* ror 8: */
                        = (uint8_t)(e >> 16);                                /* under2 */
                }
                d2++;
            } while (--k > 0);
            di += (uint32_t)modew;
        } while (--r > 0);
        return;
    }
    if (G8(VA_g_rawscreen_flag) != 0) {
        if (G32(VA_g_video_linear_flag) == 0) {                          /* ---- RAW (type 0) ---- */
            if (type != 0) return;
            uint32_t di = *(uint32_t *)(uintptr_t)rec;
            *(uint32_t *)(uintptr_t)rec = 0;
            uint32_t wh = *(uint32_t *)(uintptr_t)(rec + 4);
            uint32_t si = rec + 0xc;
            int32_t r = (int32_t)((wh >> 8) & 0xff);
            do {
                uint32_t d2 = di;
                int32_t k = (int32_t)(wh & 0xff);
                do {
                    uint16_t ax = *(uint16_t *)(uintptr_t)si; si += 2;
                    if ((uint8_t)ax != 0) *(uint8_t *)(uintptr_t)d2 = (uint8_t)(ax >> 8);
                    d2++;
                } while (--k > 0);
                di += 0x140u;
            } while (--r > 0);
            return;
        }
        if (type != 2) return;                            /* ---- BANKED ---- */
        uint32_t lin = *(uint32_t *)(uintptr_t)rec;
        uint16_t oldbank = (uint16_t)G16(VA_g_current_vesa_bank);
        uint16_t bank = (uint16_t)(((lin >> 16) - 0xa) * (uint16_t)G16(VA_g_vesa_page_bank_offset));
        b2_set_vesa_bank(bank);
        uint16_t off = (uint16_t)lin;
        *(uint32_t *)(uintptr_t)rec = 0;
        uint32_t wh = *(uint32_t *)(uintptr_t)(rec + 4);
        uint32_t si = rec + 0xc;
        uint16_t bp16 = (uint16_t)(G32(VA_g_screen_pitch) - (int32_t)(wh & 0xff));
        int32_t r = (int32_t)((wh >> 8) & 0xff);
        do {
            int32_t k = (int32_t)(wh & 0xff);
            do {
                uint16_t ax = *(uint16_t *)(uintptr_t)si; si += 2;
                if ((uint8_t)ax != 0)
                    *(uint8_t *)(uintptr_t)(0xa0000u + off) = (uint8_t)(ax >> 8);
                uint16_t noff = (uint16_t)(off + 1);
                if (noff < off) b2_set_vesa_bank_page2();
                off = noff;
            } while (--k > 0);
            uint16_t noff = (uint16_t)(off + bp16);
            if (noff < off) b2_set_vesa_bank_page2();
            off = noff;
        } while (--r > 0);
        b2_set_vesa_bank(oldbank);
        return;
    }
    /* ---- PLANAR Mode-X ---- */
    if (G8(VA_g_hires_line_doubling_flag) == 0) {                               /* lores: expects type 1 */
        if (type != 1) return;
        uint32_t di0 = *(uint32_t *)(uintptr_t)rec;
        *(uint32_t *)(uintptr_t)rec = 0;
        uint32_t wh = *(uint32_t *)(uintptr_t)(rec + 4);
        uint8_t w = (uint8_t)wh, hgt = (uint8_t)(wh >> 8);
        uint8_t phase = *(uint8_t *)(uintptr_t)(rec + 6);
        uint32_t si = rec + 0xc;
        uint8_t mask = 1;
        int8_t start = (int8_t)phase;
        while (start < (int8_t)w) {
            uint32_t di = di0;
            if (g_os_port_out) g_os_port_out(0x3c5, mask);
            int8_t dh = start;
            if (dh >= 0)
                goto rcol_body_lo;
            goto rcol_adv_lo;
            while ((int8_t)dh < (int8_t)w) {
rcol_body_lo:;
                uint32_t d2 = di;
                int32_t r = (int32_t)hgt;
                do {
                    uint16_t ax = *(uint16_t *)(uintptr_t)si; si += 2;
                    if ((uint8_t)ax != 0) *(uint8_t *)(uintptr_t)d2 = (uint8_t)(ax >> 8);
                    d2 += 0x50;
                } while (--r > 0);
rcol_adv_lo:
                di += 1;
                dh = (int8_t)(dh + 4);
            }
            mask = (uint8_t)(mask + mask);
            start += 1;
            if (start >= 4) break;
        }
        return;
    }
    if (type != 0x80) return;                             /* hires */
    {
        uint32_t di0 = *(uint32_t *)(uintptr_t)rec;
        *(uint32_t *)(uintptr_t)rec = 0;
        uint32_t wh = *(uint32_t *)(uintptr_t)(rec + 4);
        uint8_t w = (uint8_t)wh, hgt = (uint8_t)(wh >> 8);
        uint8_t phase = *(uint8_t *)(uintptr_t)(rec + 6);
        uint32_t si = rec + 0xc;
        uint8_t mask = 1;
        int8_t start = (int8_t)phase;
        while (start < (int8_t)w) {
            uint32_t di = di0;
            if (g_os_port_out) g_os_port_out(0x3c5, mask);
            int8_t dh = start;
            if (dh >= 0)
                goto rcol_body_hi;
            goto rcol_adv_hi;
            while ((int8_t)dh < (int8_t)w) {
rcol_body_hi:;
                uint32_t d2 = di;
                int32_t r = (int32_t)hgt;
                do {
                    uint32_t e = *(uint32_t *)(uintptr_t)si; si += 4;
                    if ((uint8_t)e != 0) {
                        *(uint8_t *)(uintptr_t)d2 = (uint8_t)(e >> 8);        /* under1 */
                        *(uint8_t *)(uintptr_t)(d2 + 0x50) = (uint8_t)(e >> 16); /* rol 16 */
                    }
                    d2 += 0xa0;
                } while (--r > 0);
rcol_adv_hi:
                di += 1;
                dh = (int8_t)(dh + 4);
            }
            mask = (uint8_t)(mask + mask);
            start += 1;
            if (start >= 4) break;
        }
    }
}

/* restore_cursor_region_if_set (0x11cd2, 15B): EAX=save-under record; if its surface ptr
 * (+0) is armed, restore it (blit_software_cursor — direct C). pushal — no outputs. */
void restore_cursor_region_if_set(uint32_t rec)
{
    if (*(uint32_t *)(uintptr_t)rec != 0)
        blit_software_cursor(rec);
}

/* draw_software_cursor_sprite (0x118a9, 156B): draw the cursor at ([0x76880],[0x76884])
 * into the surface for segment DX (screen base = zx(DX)<<4), saving under into the record
 * at EAX. Reads the cursor shape header [0x76878] (A4 stored ptr; byte0=w, byte1=h), SMC-
 * patches the SIX sprite-stride immediates inside blit_sprite_save_under with the width
 * (C2 — live code bytes), clips via clip_sprite_extents_to_screen, publishes the clipped
 * rect to [0x76860..0x7686c] (hires doubles the y pair), and calls the save-under blitter
 * (both direct C). Bumps the hide counter [0x7e8c8] around the draw. pushal — no outputs. */
void draw_software_cursor_sprite(uint32_t rec, uint32_t dx_seg)
{
    uint32_t base = (uint32_t)(uint16_t)dx_seg << 4;      /* movzx esi,dx; shl 4 */
    G32(VA_g_screen_busy_depth) = G32(VA_g_screen_busy_depth) + 1;
    uint32_t shape = (uint32_t)G32(VA_g_saveunder_sprite_color_ptr);
    uint16_t cx = *(uint16_t *)(uintptr_t)shape;          /* cl=w, ch=h */
    uint32_t spr = shape + 2;
    uint8_t cl = (uint8_t)cx;
    G8(0x119e1) = cl; G8(0x11a7b) = cl; G8(0x11ba2) = cl; /* SMC: the save-under strides */
    G8(0x11c22) = cl; G8(0x11aed) = cl; G8(0x11b58) = cl;
    int32_t x = G32(VA_g_saveunder_sprite_color_ptr + 0x8), y = G32(VA_g_saveunder_sprite_color_ptr + 0xc);
    uint32_t wh = clip_sprite_extents_to_screen(&x, &y, &spr, cx);
    G32(VA_g_console_input_numeric_only + 0xc) = x;
    G32(VA_g_console_input_numeric_only + 0x10) = y;
    G32(VA_g_console_input_numeric_only + 0x14) = (int32_t)((wh & 0xff) + (uint32_t)x);
    int32_t bot = (int32_t)(((wh >> 8) & 0xff) + (uint32_t)y);
    if (G8(VA_g_hires_line_doubling_flag) != 0) {
        G32(VA_g_console_input_numeric_only + 0x10) = G32(VA_g_console_input_numeric_only + 0x10) << 1;                 /* shl dword */
        bot += bot;
    }
    G32(VA_g_console_input_numeric_only + 0x18) = bot;
    if (((wh >> 8) & 0xff) != 0 && (wh & 0xff) != 0)      /* or ch,ch / or cl,cl */
        blit_sprite_save_under((uint32_t)y, (uint32_t)x, wh, spr, base, rec);
    G32(VA_g_screen_busy_depth) = G32(VA_g_screen_busy_depth) - 1;
}

/* restore_and_redraw_cursor (0x11f49, 101B): full restore+redraw for the FRONT page.
 * Gated on hide counter <= 1. LFB mode jumps into present_dirty_cursor_region's TAIL
 * (0x11f19 — video_display's TU; the 6-instruction tail is inlined here: publish the front
 * page to [0x7685c] and restore the page's record WITHOUT redrawing). Planar mode selects
 * the VGA map-mask/read-map index registers first (out 0x3c4/0x3ce). Otherwise: poll the
 * mouse, pick the record for the front page ([0x71f04] < [0x71f08] -> 0x7a8a4 else
 * 0x76898), and draw it there. */
void restore_and_redraw_cursor(void)
{
    G32(VA_g_console_input_numeric_only + 0x8) = 0;
    if ((uint32_t)G32(VA_g_screen_busy_depth) > 1) return;               /* ja (unsigned) */
    if (G8(VA_g_rawscreen_flag) == 0) {
        if (G32(VA_g_linear_framebuffer_ptr) != 0) {
            /* LFB: the 0x11f19 cross-TU tail (present_dirty_cursor_region's body) */
            uint32_t page = (uint16_t)G16(VA_g_init_stage_error_strings + 0x134);
            G32(VA_g_console_input_numeric_only + 0x8) = (int32_t)page;
            if ((uint16_t)page < (uint16_t)G16(VA_g_init_stage_error_strings + 0x138))
                restore_cursor_region_if_set((uint32_t)GADDR(VA_g_cursor_prev_y + 0x4010));
            else
                restore_cursor_region_if_set((uint32_t)GADDR(VA_g_cursor_prev_y + 0x4));
            return;
        }
        if (g_os_port_out) {                            /* planar: select the VGA regs */
            g_os_port_out(0x3c4, 2);
            g_os_port_out(0x3ce, 4);
        }
    }
    poll_mouse_motion();
    uint16_t front = (uint16_t)G16(VA_g_init_stage_error_strings + 0x134);
    uint32_t rec = (front < (uint16_t)G16(VA_g_init_stage_error_strings + 0x138)) ? (uint32_t)GADDR(VA_g_cursor_prev_y + 0x4010)
                                                    : (uint32_t)GADDR(VA_g_cursor_prev_y + 0x4);
    draw_software_cursor_sprite(rec, front);
}

/* redraw_view_region_shadow_border (0x12b20, 37B): redraw the shadow border from the cached
 * rect globals ([0x85ce0],[0x85ce4],[0x85cd8],[0x85cdc]) via draw_popup_shadow_border_smc
 * (direct C). All GP regs push/pop-preserved. */
void redraw_view_region_shadow_border(void)
{
    draw_popup_shadow_border_smc(G32(VA_g_view_x), G32(VA_g_view_y), G32(VA_g_view_w), G32(VA_g_view_h));
}

/* update_software_cursor (0x116b6, 293B; DEAD — reached only via force_cursor_redraw /
 * vsync_timer_tick's cursor tick): move-detect + double-page cursor redraw. Gated on the
 * hide counter == 0 (bumped around the work). Polls the mouse, diffs ([0x707b3],[0x707b7])
 * against the last-drawn pair ([0x76890],[0x76894]) (always updating the pair); no movement
 * = nothing else. On movement: [0x76858]=0; in PLANAR mode the VGA map-mask + read-map
 * state is saved around the draws (in/out via the g_os_port_in/out hooks — skipped when
 * unhooked, matching a host with no planar emulation); each of the two page records
 * (0x76898/0x7a8a4, ordered by [0x71f04]<[0x71f08]) whose page differs from the displayed
 * one [0x7685c] is restored + redrawn at its page (the second page only in LFB/planar
 * modes — raw 320x200 has a single page). */
void update_software_cursor(void)
{
    if (G32(VA_g_screen_busy_depth) != 0) return;
    G32(VA_g_screen_busy_depth) = G32(VA_g_screen_busy_depth) + 1;
    poll_mouse_motion();
    int32_t nx = G32(VA_g_mouse_x), ny = G32(VA_g_mouse_y);
    int32_t dxm = nx - G32(VA_g_cursor_prev_x);
    G32(VA_g_cursor_prev_x) = nx;
    int32_t dym = ny - G32(VA_g_cursor_prev_y);
    G32(VA_g_cursor_prev_y) = ny;
    if ((dxm | dym) != 0) {
        G32(VA_g_console_input_numeric_only + 0x4) = 0;
        int planar = (G8(VA_g_rawscreen_flag) == 0 && G32(VA_g_linear_framebuffer_ptr) == 0);
        uint8_t sv_seq_idx = 0, sv_seq_val = 0, sv_gc_idx = 0, sv_gc_val = 0;
        int saved = 0;
        if (planar && g_os_port_in && g_os_port_out) {   /* save VGA latch state */
            sv_seq_idx = g_os_port_in(0x3c4);
            g_os_port_out(0x3c4, 2);
            sv_seq_val = g_os_port_in(0x3c5);
            sv_gc_idx = g_os_port_in(0x3ce);
            g_os_port_out(0x3ce, 4);
            sv_gc_val = g_os_port_in(0x3cf);
            saved = 1;
        }
        uint32_t recA = (uint32_t)GADDR(VA_g_cursor_prev_y + 0x4), recB = (uint32_t)GADDR(VA_g_cursor_prev_y + 0x4010);
        uint16_t front = (uint16_t)G16(VA_g_init_stage_error_strings + 0x134);
        if (front < (uint16_t)G16(VA_g_init_stage_error_strings + 0x138)) { uint32_t t = recA; recA = recB; recB = t; }
        if (front != (uint16_t)(uint32_t)G32(VA_g_console_input_numeric_only + 0x8)) {  /* cmp cx,dx (16-bit) */
            restore_cursor_region_if_set(recA);
            draw_software_cursor_sprite(recA, front);
        }
        if (G32(VA_g_linear_framebuffer_ptr) != 0 || G8(VA_g_rawscreen_flag) == 0) {      /* second page: LFB or planar */
            uint16_t back = (uint16_t)G16(VA_g_init_stage_error_strings + 0x138);
            if (back != (uint16_t)(uint32_t)G32(VA_g_console_input_numeric_only + 0x8)) {
                restore_cursor_region_if_set(recB);
                draw_software_cursor_sprite(recB, back);
            }
        }
        if (saved) {                                      /* restore VGA latch state */
            g_os_port_out(0x3ce, 4);
            g_os_port_out(0x3cf, sv_gc_val);
            g_os_port_out(0x3ce, sv_gc_idx);
            g_os_port_out(0x3c4, 2);
            g_os_port_out(0x3c5, sv_seq_val);
            g_os_port_out(0x3c4, sv_seq_idx);
        }
    }
    G32(VA_g_screen_busy_depth) = G32(VA_g_screen_busy_depth) - 1;
}

/* force_cursor_redraw (0x116aa, 12B): poison the last-drawn x ([0x76890]=0xffffff81) so the
 * move detector always fires, then fall into update_software_cursor. */
void force_cursor_redraw(void)
{
    G32(VA_g_cursor_prev_x) = (int32_t)0xffffff81;
    update_software_cursor();
}

/* set_cursor_shape (0x12a08, 273B): select + decode the cursor shape `id` (AX). The four
 * directional ids (0x248/0x258/0x250/0x260) map to alternate ids (0x3a8/0x3b8/0x3b0/0x3c0)
 * when keymap bit 0x10 of [0x90bcc] is set: already-alternate -> return; if the current
 * shape [0x708e4] equals the ORIGINAL id, the held-item flag [0x7f250] is raised (the
 * shape ptr pair then comes from the descriptor via 0x115dd instead of the mouse blit).
 * Same-id -> return. Otherwise: store the id, resolve the shape record through the table
 * at [0x7f56c] (base + *(int32*)(base + (int16)id)), build the 0x14-byte descriptor at
 * 0x7e93c {ptr 0x7e950 x2, w=rec[2], h=rec[3], scale=[0x708e6]}, write the w/h header
 * bytes rec[4]/rec[6] to 0x7e950 and decode rec+8 (ByteRun1 when rec[0]&1, via
 * decode_byterun1 [L] direct-C) into 0x7e952, then hand off: held-item -> 0x115dd ([L] direct-C:
 * [0x76878/7c] = descriptor[0]) else blit_scaled_sprite_at_mouse (direct C). */
void set_cursor_shape(uint32_t id_in)
{
    G8(VA_g_cursor_changed) = 0;
    uint32_t id = id_in & 0xffffu;
    uint16_t alt = 0;
    if      (id == 0x248) alt = 0x3a8;
    else if (id == 0x258) alt = 0x3b8;
    else if (id == 0x250) alt = 0x3b0;
    else if (id == 0x260) alt = 0x3c0;
    if (alt != 0 && (G16(VA_g_frame_tick_counter) & 0x10) != 0) {
        if ((uint16_t)G16(VA_g_current_cursor_id) == alt) return;        /* already the alternate */
        uint16_t orig = (uint16_t)id;
        id = alt;                                         /* xchg edx,eax */
        if ((uint16_t)G16(VA_g_current_cursor_id) == orig)
            G8(VA_g_cursor_changed) = 1;                              /* held-item handoff */
    }
    if ((uint16_t)G16(VA_g_current_cursor_id) == (uint16_t)id) return;
    G16(VA_g_current_cursor_id) = (uint16_t)id;
    if (G32(VA_g_reloc_base) == 0) return;
    int32_t idx = (int32_t)(int16_t)(uint16_t)id;         /* cwde */
    uint32_t base = (uint32_t)G32(VA_g_reloc_base);
    uint32_t recp = base + (uint32_t)*(int32_t *)(uintptr_t)(base + (uint32_t)idx);
    const uint8_t *r = (const uint8_t *)(uintptr_t)recp;
    int32_t *desc = (int32_t *)GADDR(VA_g_mouse_click_edges + 0x2);
    uint8_t *dst = (uint8_t *)GADDR(VA_g_cursor_sprite);
    desc[0] = (int32_t)GADDR(VA_g_cursor_sprite);
    desc[1] = (int32_t)GADDR(VA_g_cursor_sprite);
    desc[2] = r[2];                                       /* display w */
    desc[3] = r[3];                                       /* display h */
    desc[4] = G32(VA_g_cursor_mask_data);                               /* scale */
    uint8_t w = r[4], h = r[6];
    dst[0] = w; dst[1] = h;
    int32_t count = (int32_t)w * (int32_t)h;
    if (r[0] & 1) {
        decode_byterun1((uint8_t *)(uintptr_t)(recp + 8), dst + 2, count);
    } else {
        /* DEAD branch (all cursor shapes are RLE): the original's "loop" reuses EDX — the
         * dest pointer 0x7e952 — as its row counter and adds the caller's EBP to ESI per
         * pass, i.e. it would iterate ~2^26 times over garbage. One plain copy is the only
         * meaningful iteration; unreachable in practice. */
        memcpy(dst + 2, (const void *)(uintptr_t)(recp + 8), (size_t)count);
    }
    if (G8(VA_g_cursor_changed) != 0) {
        set_cursor_shape_ptr_pair((uint32_t)(uintptr_t)desc);  /* 0x115dd [L] direct-C */
    } else {
        blit_scaled_sprite_at_mouse((uint32_t)(uintptr_t)desc);
    }
}

/* draw_current_mouse_cursor_sprite (0x18bb2, 263B): draw the held-item ICON at the mouse
 * cursor. Gated on no inventory drag ([0x80b2c]==0) and a current cursor entry [0x7fef0]
 * (triple-deref -> the item record; word[+4]=w, word[+6]=h). Position = mouse
 * ([0x707b3],[0x707b7]) clamped to >=0 and (UNSIGNED) to pitch/height minus w/(h+1)/2;
 * saves under (save_framebuffer_region — direct C; handle -> [0x7fed4], rect ->
 * [0x7fed8..0x7fee4]), registers the dirty rect (bridge), and blits via blit_item_icon
 * (direct C) at screen_xy_to_framebuffer_ptr (direct C; EBX=pitch per gotcha H2) with
 * ECX = ([0x76762]<<8) | (lores?2:1). Ends in the shared 6-reg epilogue (math_util tail —
 * plain return here). */
void draw_current_mouse_cursor_sprite(void)
{
    if (G32(VA_g_ui_panel_anchor_y + 0x4) != 0) return;
    if (G32(VA_g_current_cursor_entry) == 0) return;
    uint32_t e = (uint32_t)G32(VA_g_current_cursor_entry);
    e = *(uint32_t *)(uintptr_t)e;
    e = *(uint32_t *)(uintptr_t)e;                        /* the item record */
    int32_t w = (int16_t)*(uint16_t *)(uintptr_t)(e + 4);
    int32_t hh = ((int32_t)(int16_t)*(uint16_t *)(uintptr_t)(e + 6) + 1) >> 1;  /* sar */
    int32_t x = G32(VA_g_mouse_x), y = G32(VA_g_mouse_y);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if ((uint32_t)(w + x) >= (uint32_t)G32(VA_g_screen_pitch)) x = G32(VA_g_screen_pitch) - w;   /* jb */
    if ((uint32_t)(hh + y) >= (uint32_t)G32(VA_g_screen_pitch + 0x4)) y = G32(VA_g_screen_pitch + 0x4) - hh;
    G32(VA_g_active_item_hud_icon + 0x8) = x;
    G32(VA_g_active_item_hud_icon + 0x10) = y;
    G32(VA_g_active_item_hud_icon + 0x4) = (int32_t)save_framebuffer_region((uint32_t)x, (uint32_t)y,
                                                           (uint32_t)w, (uint32_t)hh, NULL);
    int32_t x1 = w + x, y1 = hh + y;
    G32(VA_g_active_item_hud_icon + 0xc) = x1;
    G32(VA_g_active_item_hud_icon + 0x14) = y1;
    b2_register_dirty_rect((uint32_t)x, (uint32_t)y, (uint32_t)x1, (uint32_t)y1);
    uint32_t mode = (G8(VA_g_hires_line_doubling_flag) == 0) ? 2u : 1u;         /* sete al; inc ecx */
    uint32_t ecx = mode + ((uint32_t)(uint8_t)G8(VA_g_default_message_color + 0x4) << 8);
    uint8_t *fb = screen_xy_to_framebuffer_ptr(x, y);   /* ebx=pitch preserved (H2) */
    uint32_t rec2 = (uint32_t)G32(VA_g_current_cursor_entry);
    rec2 = *(uint32_t *)(uintptr_t)rec2;
    rec2 = *(uint32_t *)(uintptr_t)rec2;
    blit_item_icon(rec2, (uint32_t)(uintptr_t)fb, (uint32_t)G32(VA_g_screen_pitch), ecx);
}

/* ======================================================================================
 * Group B (mid) — the SMC shadow-border blitter
 * ====================================================================================== */

/* ---- video_display seams (dirty-rects now lifted + called direct; the rest bridged) ---- */
static void b2_add_dirty_rect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)   /* 0x15b69 */
{
    add_dirty_rect(x0, (int32_t)y0, x1, y1);            /* lifted — re-pointed */
}
static void b2_register_dirty_rect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) /* 0x15b5b */
{
    register_dirty_rect(x0, (int32_t)y0, x1, y1);       /* lifted — re-pointed */
}
static void b2_set_vesa_bank(uint16_t dx_bank)                                       /* 0x2e0cb */
{
    regs_t io; memset(&io, 0, sizeof io);
    io.va = 0x2e0cbu + OBJ_DELTA;
    io.edx = dx_bank;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    roth_unreachable(0x2e0cbu);   /* VESA bank-switch (host_video_driver): not on the bare-title path */
#endif
}
static void b2_set_vesa_bank_page2(void)                                             /* 0x2e104 */
{
    regs_t io; memset(&io, 0, sizeof io);
    io.va = 0x2e104u + OBJ_DELTA;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    roth_unreachable(0x2e104u);   /* VESA bank-switch page 2: not on the bare-title path */
#endif
}

/* blit_remapped_cursor_glyph (0x10d90, 62B): remap the built-in cursor glyph (0x70443,
 * 0xff-terminated) into the scratch buffer 0x76769 — palette indices 1/2 are translated
 * through the 2-entry xlat table 0x7675c, everything else copied — then draw it via
 * blit_scaled_sprite_at_mouse (0x115ea, LIFTED — input subsystem; called direct-C,
 * call-closed) with a 5-dword stack descriptor {dst, dst, 1, 1, 0x20}. No inputs. */
void blit_remapped_cursor_glyph(void)
{
    const uint8_t *src  = (const uint8_t *)GADDR(VA_g_cfg_das2_arg + 0x167);
    uint8_t       *dst  = (uint8_t *)GADDR(VA_g_map_menu_marker_normal + 0x4);
    const uint8_t *xlat = (const uint8_t *)GADDR(VA_g_player_movement_enabled + 0x12);
    for (;;) {
        uint8_t al = *src++;
        if (al == 0xff) break;
        uint8_t t = (uint8_t)(al - 1);                    /* mov ah,al; dec ah */
        if (t <= 1) al = xlat[t];                         /* xlatb on indices 1/2 */
        *dst++ = al;
    }
    uint32_t desc[5] = { (uint32_t)GADDR(VA_g_map_menu_marker_normal + 0x4), (uint32_t)GADDR(VA_g_map_menu_marker_normal + 0x4), 1, 1, 0x20 };
    blit_scaled_sprite_at_mouse((uint32_t)(uintptr_t)desc);
}

/* blit_scaled_viewport_to_framebuffer (0x2db40, 536B): post-render present of the 3D
 * viewport — scale it in place in the back buffer per the display type [0x90bd4], then
 * register the dirty rect (add_dirty_rect 0x15b69, bridge). Viewport: fb + [0x85418],
 * w=[0x90bf2], h=[0x90bf6]; rect origin [0x90bf0]/[0x90bee]. Modes:
 *   0 -> no scale, dirty rect only;
 *   3 -> 2x2 (each row expanded right-to-left into TWO lines at dst = src + h*pitch rows);
 *   2 -> line-double only (left-to-right dword copy to 2 lines);
 *   else [0x90cc0]==0 -> in-place 1x->2x horizontal expand (right-to-left, bottom-up);
 *   else -> the same expand SMOOTHED through the fs=[0x90be2] blend LUT (pairs get
 *          fs:[(right<<8)|cur] interpolants; row tail pins the leftmost pixel).
 * In-place overlap makes the right-to-left/bottom-up walk order load-bearing — kept
 * exactly. pushal — no register outputs. */
void blit_scaled_viewport_to_framebuffer(void)
{
    uint8_t mode = (uint8_t)G8(VA_g_view_scale_flags);
    uint32_t x0 = (uint16_t)G16(VA_g_geometry_selector + 0x8), y0 = (uint16_t)G16(VA_g_geometry_selector + 0x6);
    uint32_t w  = (uint16_t)G16(VA_g_render_width), h  = (uint16_t)G16(VA_g_render_height);

    if (mode == 0) {
        b2_add_dirty_rect(x0, y0, x0 + w - 1, y0 + h - 1);
        return;
    }
    uint32_t base = (uint32_t)G32(VA_g_framebuffer_ptr) + (uint32_t)G32(VA_g_render_target_buffer + 0x4);
    int32_t pitch = G32(VA_g_screen_pitch);

    if (mode == 3) {
        /* 2x2: src bottom row up; dst = base + 2*h*pitch, two lines per row */
        uint32_t si = base + h * (uint32_t)pitch + w;
        uint32_t di = base + 2u * h * (uint32_t)pitch + 2u * w;
        int32_t r = (int32_t)h;
        do {                                              /* dec edx; jg (bottom-tested) */
            si -= (uint32_t)pitch;
            di -= (uint32_t)pitch * 2u;
            uint32_t s = si, d = di;
            int32_t k = (int32_t)(w >> 1);
            do {                                          /* dec ecx; jg */
                s -= 2;
                uint8_t a = *(uint8_t *)(uintptr_t)s, b = *(uint8_t *)(uintptr_t)(s + 1);
                d -= 4;
                uint8_t *q = (uint8_t *)(uintptr_t)d;
                q[0] = a; q[1] = a; q[2] = b; q[3] = b;                  /* dword a,a,b,b */
                q[pitch] = a; q[pitch + 1] = a; q[pitch + 2] = b; q[pitch + 3] = b;
            } while (--k > 0);
        } while (--r > 0);
        b2_add_dirty_rect(x0, y0, x0 + w * 2 - 1, y0 + h * 2 - 1);
        return;
    }
    if (mode == 2) {
        /* line-double: dword copy each src row to two dest lines (row 0 dst == src) */
        uint32_t si = base + h * (uint32_t)pitch;
        uint32_t di = base + 2u * h * (uint32_t)pitch;
        int32_t r = (int32_t)h;
        do {                                              /* dec edx; jg */
            si -= (uint32_t)pitch;
            di -= (uint32_t)pitch * 2u;
            uint32_t s = si, d = di;
            int32_t k = (int32_t)(w >> 2);
            do {                                          /* dec ecx; jg */
                uint32_t v = *(uint32_t *)(uintptr_t)s; s += 4;
                *(uint32_t *)(uintptr_t)d = v; d += 4;
                *(uint32_t *)(uintptr_t)(d + (uint32_t)pitch - 4) = v;
            } while (--k > 0);
        } while (--r > 0);
        b2_add_dirty_rect(x0, y0, x0 + w - 1, y0 + h * 2 - 1);
        return;
    }
    if (G8(VA_g_turn_view_scale_state) == 0) {
        /* in-place 1x->2x horizontal, right-to-left, bottom-up */
        uint32_t si = base + h * (uint32_t)pitch + w;
        uint32_t di = base + h * (uint32_t)pitch + 2u * w;
        int32_t r = (int32_t)h;
        do {                                              /* dec edx; jg */
            si -= (uint32_t)pitch;
            di -= (uint32_t)pitch;
            uint32_t s = si, d = di;
            int32_t k = (int32_t)(w >> 1);
            do {                                          /* dec ebp; jg */
                s -= 2;
                uint8_t a = *(uint8_t *)(uintptr_t)s, b = *(uint8_t *)(uintptr_t)(s + 1);
                d -= 4;
                uint8_t *q = (uint8_t *)(uintptr_t)d;
                q[0] = a; q[1] = a; q[2] = b; q[3] = b;
            } while (--k > 0);
        } while (--r > 0);
        b2_add_dirty_rect(x0, y0, x0 + w * 2 - 1, y0 + h - 1);
        return;
    }
    /* smoothed 2x expand through the blend LUT (fs=[0x90be2]) */
    {
        const uint8_t *lut = (const uint8_t *)(uintptr_t)
            (g_os_sel_base ? g_os_sel_base((uint16_t)G16(VA_g_transparency_blend_selector)) : 0);
        uint32_t si = base + h * (uint32_t)pitch + w;
        uint32_t di = base + h * (uint32_t)pitch + 2u * w;
        int32_t pairs = (int32_t)(w >> 1) - 1;            /* shr ecx,1; dec */
        int32_t r = (int32_t)h;
        do {                                              /* dec edx; jg */
            si -= (uint32_t)G32(VA_g_screen_pitch);
            di -= (uint32_t)G32(VA_g_screen_pitch);
            uint32_t s = si, d = di;
            s -= 1;
            uint8_t bh = *(uint8_t *)(uintptr_t)s;        /* rightmost pixel */
            int32_t k = pairs;
            do {                                          /* dec ebp; jg (bottom-tested) */
                s -= 2;
                uint8_t cur = *(uint8_t *)(uintptr_t)(s + 1);      /* bl=[esi+1] */
                uint8_t bd  = lut[((uint32_t)bh << 8) | cur];      /* blend(right,cur) */
                uint8_t *q1 = (uint8_t *)(uintptr_t)(d - 2);
                q1[0] = bd; q1[1] = bh;                            /* word [edi-2] */
                bh = cur;
                uint8_t nxt = *(uint8_t *)(uintptr_t)s;            /* bl=[esi] */
                d -= 4;
                bd = lut[((uint32_t)bh << 8) | nxt];               /* blend(cur,next) */
                uint8_t *q2 = (uint8_t *)(uintptr_t)d;
                q2[0] = bd; q2[1] = bh;                            /* word [edi] */
                bh = nxt;
            } while (--k > 0);
            uint8_t last = *(uint8_t *)(uintptr_t)(s - 1);         /* bl=[esi-1] */
            uint8_t bd = lut[((uint32_t)bh << 8) | last];
            uint8_t *q = (uint8_t *)(uintptr_t)(d - 2);
            q[0] = bd; q[1] = bh;                                  /* word [edi-2] */
            q = (uint8_t *)(uintptr_t)(d - 4);
            q[0] = last; q[1] = last;                              /* word [edi-4] */
        } while (--r > 0);
        b2_add_dirty_rect(x0, y0, x0 + w * 2 - 1, y0 + h - 1);
    }
}

/* blit_panel_image (0x23cff, 984B; Watcom C): transparent (skip-0) blit of a UI panel's
 * content image via copy_nonzero_bytes/_2x (both LIFTED — direct C). ABI: EAX=src,
 * EDX=panel record, EBX=dest base. Panel: +0 x0, +4 y0, +8 x1, +0xc y1 (CLEARED at exit),
 * +0x10 scr_x, +0x14 scr_y, +0x18 src stride, +0x1c rows (full-view path), +0x20 dest
 * x-bias, +0x28 dest y-bias. Full-view path ([0x8549c]==[0x85cdc]): fb + scr_x + one pitch,
 * +0x1c rows (hires: doubled lines). Clipped path: dirty-row window y1-y0 clamped UNSIGNED
 * to the screen; widescreen (pitch==0x280) doubles x and lines via copy_nonzero_bytes_2x +
 * add_dirty_rect of the 2x rect; else copy_nonzero_bytes (hires doubled) +
 * register_dirty_rect (bridges). */
void blit_panel_image(uint32_t src, int32_t *panel, uint32_t dest_base)
{
    int32_t pitch = G32(VA_g_screen_pitch);
    if (G32(VA_g_screen_pitch + 0x4) == G32(VA_g_view_h)) {                   /* full-view path */
        if (panel[0x18 / 4] == 0) return;
        uint32_t dst = (uint32_t)G32(VA_g_framebuffer_ptr) + (uint32_t)panel[0x10 / 4] + (uint32_t)pitch;
        uint32_t stride = (uint32_t)panel[0x18 / 4];
        uint32_t s = src;
        if (G8(VA_g_hires_line_doubling_flag) == 0) {
            for (uint32_t i = 0; i < (uint32_t)panel[0x1c / 4]; i++) {
                copy_nonzero_bytes((uint8_t *)(uintptr_t)dst, stride,
                                          (const uint8_t *)(uintptr_t)s);
                s += stride;
                dst += (uint32_t)G32(VA_g_screen_pitch);
            }
        } else {
            for (uint32_t i = 0; i < (uint32_t)panel[0x1c / 4]; i++) {
                copy_nonzero_bytes((uint8_t *)(uintptr_t)dst, stride,
                                          (const uint8_t *)(uintptr_t)s);
                dst += (uint32_t)G32(VA_g_screen_pitch);
                copy_nonzero_bytes((uint8_t *)(uintptr_t)dst, stride,
                                          (const uint8_t *)(uintptr_t)s);
                s += stride;
                dst += (uint32_t)G32(VA_g_screen_pitch);
            }
        }
        return;
    }
    if (panel[0xc / 4] == 0 || panel[0x18 / 4] == 0) return;
    uint32_t wid    = (uint32_t)(panel[8 / 4] - panel[0]);          /* x1 - x0 */
    uint32_t stride = (uint32_t)panel[0x18 / 4];
    uint32_t s = src + (uint32_t)(panel[4 / 4] * panel[0x18 / 4]) + (uint32_t)panel[0];
    int32_t yoff = panel[4 / 4] + panel[0x28 / 4];                  /* y0 + y-bias */
    uint32_t dst = dest_base + (uint32_t)panel[0x10 / 4] + (uint32_t)panel[0x20 / 4]
                 + (uint32_t)panel[0];

    if (pitch == 0x280) {                                 /* widescreen 2x */
        dst += (uint32_t)panel[0];                        /* x doubled */
        yoff += panel[4 / 4];                             /* y doubled */
        if (yoff != 0) dst += (uint32_t)(yoff * 0x280);
        uint32_t rows = (uint32_t)(panel[0xc / 4] - panel[4 / 4]);
        if ((uint32_t)G32(VA_g_screen_pitch + 0x4) < rows + (uint32_t)(panel[4 / 4] + panel[0x14 / 4]))
            rows = (uint32_t)(G32(VA_g_screen_pitch + 0x4) - (panel[4 / 4] + panel[0x14 / 4]));
        for (uint32_t i = 0; i < rows; i++) {
            copy_nonzero_bytes_2x((uint8_t *)(uintptr_t)dst, wid,
                                         (const uint8_t *)(uintptr_t)s);
            dst += (uint32_t)G32(VA_g_screen_pitch);
            copy_nonzero_bytes_2x((uint8_t *)(uintptr_t)dst, wid,
                                         (const uint8_t *)(uintptr_t)s);
            s += stride;
            dst += (uint32_t)G32(VA_g_screen_pitch);
        }
    } else if (G8(VA_g_hires_line_doubling_flag) == 0) {                        /* normal */
        if (yoff != 0) dst += (uint32_t)(yoff * pitch);
        uint32_t rows = (uint32_t)(panel[0xc / 4] - panel[4 / 4]);
        if ((uint32_t)G32(VA_g_screen_pitch + 0x4) < rows + (uint32_t)(panel[4 / 4] + panel[0x14 / 4]))
            rows = (uint32_t)(G32(VA_g_screen_pitch + 0x4) - (panel[4 / 4] + panel[0x14 / 4]));
        for (uint32_t i = 0; i < rows; i++) {
            copy_nonzero_bytes((uint8_t *)(uintptr_t)dst, wid,
                                      (const uint8_t *)(uintptr_t)s);
            s += stride;
            dst += (uint32_t)G32(VA_g_screen_pitch);
        }
    } else {                                              /* hires line-doubled */
        if (yoff != 0) { yoff <<= 1; dst += (uint32_t)(yoff * pitch); }
        uint32_t rows = (uint32_t)(panel[0xc / 4] - panel[4 / 4]);
        if ((uint32_t)G32(VA_g_screen_pitch + 0x4) < rows + (uint32_t)(panel[4 / 4] + panel[0x14 / 4]))
            rows = (uint32_t)(G32(VA_g_screen_pitch + 0x4) - (panel[4 / 4] + panel[0x14 / 4]));
        for (uint32_t i = 0; i < rows; i++) {
            copy_nonzero_bytes((uint8_t *)(uintptr_t)dst, wid,
                                      (const uint8_t *)(uintptr_t)s);
            dst += (uint32_t)G32(VA_g_screen_pitch);
            copy_nonzero_bytes((uint8_t *)(uintptr_t)dst, wid,
                                      (const uint8_t *)(uintptr_t)s);
            s += stride;
            dst += (uint32_t)G32(VA_g_screen_pitch);
        }
    }
    int32_t sx = panel[0x10 / 4], sy = panel[0x14 / 4];
    if (G32(VA_g_screen_pitch) == 0x280)
        b2_add_dirty_rect((uint32_t)(panel[0] * 2 + sx), (uint32_t)(panel[4 / 4] * 2 + sy),
                          (uint32_t)(panel[8 / 4] * 2 + sx), (uint32_t)(panel[0xc / 4] * 2 + sy));
    else
        b2_register_dirty_rect((uint32_t)(panel[0] + sx), (uint32_t)(panel[4 / 4] + sy),
                               (uint32_t)(panel[8 / 4] + sx), (uint32_t)(panel[0xc / 4] + sy));
    panel[0xc / 4] = 0;
}

/* blit_image_to_video_target (0x2de3c, 616B): the back-buffer -> DISPLAY seam — copy a
 * width x rows image block to the active video surface, branching on the target type.
 * ABI (multi-reg, corpus dropped ESI/EDI): EAX=width, EDX=row, EBX=x byte offset,
 * ECX=rows, ESI=src, EDI=surface base (banked/raw/planar paths). src pitch = [0x85498].
 *   [0x146d8]!=0 -> VESA LINEAR framebuffer (A4 stored ptr): dest = LFB + xoff +
 *       (row [+ [0x854a0] when [0x71f04]!=0xa000]) * [0x146dc] (mode width); width ==
 *       mode-width fast path = one contiguous copy of rows*(width>>2) dwords.
 *   [0x76634]!=0 -> VESA BANKED window at EDI (0xa0000): 16-bit window arithmetic, bank =
 *       (linear>>16)*[0x71dc4] via set_vesa_bank (bridge); rows split at the 64K boundary
 *       (movsx of DI -> chunk to boundary, wrap, set_vesa_bank_page2); pushal (no reg
 *       outputs).
 *   [0x90c08]!=0 -> RAW 320x200 at EDI: fixed 0x140 width; full-width fast path count =
 *       16-bit mul (cx*width/4 truncated to AX — reproduced).
 *   else -> PLANAR Mode-X at EDI: plane mask out 0x3c4/0x3c5 (via g_os_port_out, NULL in
 *       the oracle), 4 passes packing every-4th source byte into word/dword writes;
 *       width==0x140 fast path packs rows*0x50/4 dwords per plane; partial path rounds
 *       width to 8 and writes [0x85424]=rows, [0x8541c]=4*(0x50-width/8*2... the plane
 *       row-advance) as it goes. */
void blit_image_to_video_target(uint32_t width, uint32_t row, uint32_t xoff,
                                       uint32_t rows, uint32_t src, uint32_t edi_in)
{
    if (G32(VA_g_linear_framebuffer_ptr) != 0) {                              /* ---- VESA LINEAR ---- */
        uint32_t di = (uint32_t)G32(VA_g_linear_framebuffer_ptr) + xoff;
        uint32_t r = row;
        if ((uint16_t)G16(VA_g_init_stage_error_strings + 0x134) != 0xa000) r += (uint32_t)G32(VA_g_screen_height);
        int32_t modew = G32(VA_g_video_mode_width);
        int32_t sskip = G32(VA_g_screen_pitch) - (int32_t)width;
        di += (uint32_t)((int32_t)r * modew);
        int32_t dskip = modew - (int32_t)width;
        if (dskip == 0) {
            uint32_t n = rows * (width >> 2);             /* mul edx (32-bit) */
            memcpy((void *)(uintptr_t)di, (const void *)(uintptr_t)src, n * 4);
            return;
        }
        uint32_t w = width & 0xffffu;                     /* and edx,0xffff */
        int32_t rr = (int32_t)rows;
        do {                                              /* dec ecx; jg (bottom-tested) */
            memcpy((void *)(uintptr_t)di, (const void *)(uintptr_t)src, w);
            di += w + (uint32_t)dskip;
            src += w + (uint32_t)sskip;
        } while (--rr > 0);
        return;
    }
    if (G32(VA_g_video_linear_flag) != 0) {                              /* ---- VESA BANKED ---- */
        uint32_t di = edi_in + xoff;
        int32_t modew = G32(VA_g_video_mode_width);
        di += (uint32_t)((int32_t)row * modew);
        int32_t dskip = modew - (int32_t)width;
        int32_t sskip = G32(VA_g_screen_pitch) - (int32_t)width;
        uint32_t lin = di - 0xa0000u;
        uint16_t bank = (uint16_t)((lin >> 16) * (uint16_t)G16(VA_g_vesa_page_bank_offset));
        di = (lin & 0xffffu) + 0xa0000u;
        b2_set_vesa_bank(bank);
        uint32_t w = width & 0xffffu;                     /* and ebp,0xffff */
        int32_t rr = (int32_t)rows;
        do {                                              /* dec edx; jg (bottom-tested) */
            uint16_t di16 = (uint16_t)di;
            if ((uint32_t)di16 + w > 0xffffu) {           /* add di,cx sets CF: crosses 64K */
                uint32_t chunk = (uint32_t)-(int32_t)(int16_t)di16;   /* movsx; neg */
                memcpy((void *)(uintptr_t)di, (const void *)(uintptr_t)src, chunk);
                src += chunk;                             /* movs advances esi */
                di += chunk;
                di -= 0x10000u;                           /* wrap the window */
                b2_set_vesa_bank_page2();
                uint32_t rest = w - chunk;
                memcpy((void *)(uintptr_t)di, (const void *)(uintptr_t)src, rest);
                src += rest; di += rest;
            } else {
                memcpy((void *)(uintptr_t)di, (const void *)(uintptr_t)src, w);
                src += w; di += w;
            }
            uint16_t di_lo = (uint16_t)di;
            uint32_t sum = (uint32_t)di_lo + (uint16_t)dskip;   /* add di,bx */
            di = (di & 0xffff0000u) | (sum & 0xffffu);
            if (sum > 0xffffu) b2_set_vesa_bank_page2();  /* jb -> next bank */
            src += (uint32_t)sskip;
        } while (--rr > 0);
        return;
    }
    if (G8(VA_g_rawscreen_flag) != 0) {                               /* ---- RAW 320x200 ---- */
        uint32_t di = edi_in + xoff + row * 0x140u;
        int32_t skip = 0x140 - (int32_t)width;
        if ((uint16_t)width == 0x140u) {
            uint16_t n16 = (uint16_t)((uint32_t)(uint16_t)rows * (uint16_t)(width >> 2)); /* mul dx */
            memcpy((void *)(uintptr_t)di, (const void *)(uintptr_t)src, (uint32_t)n16 * 4);
            return;
        }
        uint32_t w = width & 0xffffu;
        int32_t rr = (int32_t)rows;
        do {                                              /* dec ecx; jg (bottom-tested) */
            memcpy((void *)(uintptr_t)di, (const void *)(uintptr_t)src, w);
            di += w + (uint32_t)skip;
            src += w + (uint32_t)skip;                    /* ebp = 0x140 - width too */
        } while (--rr > 0);
        return;
    }
    /* ---- PLANAR Mode-X ---- */
    {
        uint32_t di = edi_in + (xoff >> 2);
        di += row * 0x50u;
        if (width == 0x140u) {                            /* full-width fast path */
            uint32_t perplane = (rows * 0x50u) >> 2;      /* dwords per plane */
            if (g_os_port_out) g_os_port_out(0x3c4, 2);
            uint8_t mask = 1;
            for (int32_t plane = 4; plane > 0; plane--) {
                if (g_os_port_out) g_os_port_out(0x3c5, mask);
                uint32_t s = src, d = di;
                for (uint32_t k = perplane; k != 0; k--) {
                    uint8_t *q = (uint8_t *)(uintptr_t)d;
                    q[0] = *(const uint8_t *)(uintptr_t)s;
                    q[1] = *(const uint8_t *)(uintptr_t)(s + 4);
                    q[2] = *(const uint8_t *)(uintptr_t)(s + 8);
                    q[3] = *(const uint8_t *)(uintptr_t)(s + 12);
                    s += 16; d += 4;
                }
                mask = (uint8_t)(mask + mask);            /* add al,al */
                src += 1;                                 /* next plane's phase */
            }
            return;
        }
        /* partial width: round to 8, pairs = width/8 per plane row */
        uint32_t wal = (width + 7) & 0xfffffff8u;         /* add ebx,7; and bl,0xf8 */
        if (g_os_port_out) g_os_port_out(0x3c4, 2);
        G32(VA_g_render_target_buffer + 0x10) = (int32_t)rows;
        uint32_t pairs4 = wal >> 2;
        uint32_t dskip = 0x50u - pairs4;                  /* ebp = 0x50 - width/4 */
        G32(VA_g_render_target_buffer + 0x8) = (int32_t)(dskip << 2);             /* src row advance (x4) */
        uint32_t pairs = pairs4 >> 1;
        uint8_t mask = 1;
        for (int32_t plane = 4; plane > 0; plane--) {
            if (g_os_port_out) g_os_port_out(0x3c5, mask);
            uint32_t s = src, d = di;
            int32_t r2 = (int32_t)rows;
            do {                                          /* dec ebx; jg (bottom-tested) */
                int32_t k = (int32_t)pairs;
                do {                                      /* dec ecx; jg (bottom-tested) */
                    uint8_t *q = (uint8_t *)(uintptr_t)d;
                    q[0] = *(const uint8_t *)(uintptr_t)s;
                    q[1] = *(const uint8_t *)(uintptr_t)(s + 4);
                    s += 8; d += 2;
                } while (--k > 0);
                d += dskip;
                s += (uint32_t)G32(VA_g_render_target_buffer + 0x8);
            } while (--r2 > 0);
            mask = (uint8_t)(mask + mask);
            src += 1;
        }
    }
}

/* ======================================================================================
 * Group D — save-under / backing store
 * ====================================================================================== */

/* save_framebuffer_region (0x13062, 114B): allocate a DAS-cache pool block and copy the
 * dword-aligned framebuffer rect (x,y,w,h) into it. Record: {fb ptr, aligned width, height}
 * + pixel data at +0xc (the restore side is blit_save_region 0x130d4 [L]). x is aligned
 * down to 4, width up to cover x+w (8-bit ANDs on al/bl == &~3); hires doubles y and h.
 * CALL-CLOSED: ensure_das_cache_heap_space 0x414d2 + pool_alloc_handle 0x360f9 are both
 * lifted (direct C). Returns EAX = the pool handle. NOTE the original's `jb` after the
 * alloc is VESTIGIAL — the alloc's failure exit is `sub eax,eax; ret` (CF always 0), so a
 * real failure would fall through and deref page 0 (DOS null-page tolerance, gotcha H);
 * unreachable in practice because ensure_das_cache_heap_space evicts until the alloc fits.
 * The lift returns cleanly (handle 0, *cf_out=1) instead of scribbling through a null
 * handle. ABI: EAX=x, EDX=y, EBX=width, ECX=height. */
uint32_t save_framebuffer_region(uint32_t x, uint32_t y, uint32_t width,
                                        uint32_t height, uint32_t *cf_out)
{
    uint32_t xa = x & ~3u;                                /* and al,0xfc */
    uint32_t wa = ((x + width + 3) & ~3u) - xa;           /* lea; and bl,0xfc; sub */
    uint32_t yy = y, hh = height;
    if (G8(VA_g_hires_line_doubling_flag) != 0) { yy += yy; hh += hh; }
    uint32_t size = wa * hh + 0xc;
    ensure_das_cache_heap_space(size);
    uint32_t h = pool_alloc_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle),
                                          (int32_t)size);
    if (h == 0) {                                         /* jb (alloc CF) */
        if (cf_out) *cf_out = 1;
        return 0;
    }
    if (cf_out) *cf_out = 0;
    uint8_t *chunk = (uint8_t *)(uintptr_t)*(uint32_t *)(uintptr_t)h;
    uint32_t fbp = (uint32_t)G32(VA_g_framebuffer_ptr) + xa + (uint32_t)((int32_t)yy * G32(VA_g_screen_pitch));
    *(uint32_t *)(void *)(chunk + 0) = fbp;
    *(uint32_t *)(void *)(chunk + 8) = hh;
    *(uint32_t *)(void *)(chunk + 4) = wa;
    uint8_t *d = chunk + 0xc;
    const uint8_t *s = (const uint8_t *)(uintptr_t)fbp;
    int32_t pitch = G32(VA_g_screen_pitch);
    int32_t rows = (int32_t)hh;
    do {                                                  /* dec edx; jg */
        memcpy(d, s, wa);                                 /* rep movsd (wa 4-aligned) */
        d += wa; s += pitch;
    } while (--rows > 0);
    return h;
}

/* blit_saved_ui_block (0x18a64, 118B): restore the saved UI block (globals 0x810xx) —
 * gated on [0x810f4]/[0x810f8]/[0x810d0] nonzero and [0x83acd]!=1; consumes+clears the
 * saved-src pointer 0x810f8, blits it back via blit_opaque_rect (LIFTED — direct C), then
 * register_dirty_rect (bridge) of the sign-extended word rect 0x810dc..0x810e2. No args. */
void blit_saved_ui_block(void)
{
    if (G32(VA_g_inspect_popup_state + 0x34) == 0 || G32(VA_g_inspect_popup_state + 0x38) == 0 || G32(VA_g_inspect_popup_state + 0x10) == 0) return;
    uint32_t src = (uint32_t)G32(VA_g_inspect_popup_state + 0x38);
    G32(VA_g_inspect_popup_state + 0x38) = 0;
    if (G32(VA_g_choice_interaction_mode) == 1) return;
    blit_opaque_rect(src, (uint32_t)G32(VA_g_inspect_popup_state + 0x10),
                            (uint32_t)G32(VA_g_inspect_popup_state + 0x14), (uint32_t)G32(VA_g_inspect_popup_state + 0x18));
    int32_t x = (int16_t)G16(VA_g_inspect_popup_state + 0x1c), y = (int16_t)G16(VA_g_inspect_popup_state + 0x1e);   /* movsx */
    b2_register_dirty_rect((uint32_t)x, (uint32_t)y,
                           (uint32_t)(x + (int16_t)G16(VA_g_inspect_popup_state + 0x20)),
                           (uint32_t)(y + (int16_t)G16(VA_g_inspect_popup_state + 0x22)));
}

/* blit_sprite_save_under (0x11945, 759B): draw the software-cursor sprite (EBX; colour 0 =
 * transparent; the sprite ROW-STRIDE immediates — static file byte 0x12 — are TRUE SMC,
 * patched to the live cursor width by draw_software_cursor_sprite 0x118a9 at
 * 0x119e1/0x11a7b/0x11aed/0x11b58/0x11ba2/0x11c22 and read LIVE here, C1/C2) onto the
 * ACTIVE DISPLAY surface while saving the underlying
 * pixels into the save-under record at EDI ({surface ptr @+0, w/h @+4, phase @+6, type
 * @+7, base @+8, data @+0xc}); the record is consumed by blit_software_cursor 0x11ce1.
 * ABI: EAX=row, EDX=x, ECX=(ch=h, cl=w), EBX=sprite, ESI=surface base, EDI=record.
 * Four surface types:
 *   [0x146d8]!=0 -> VESA LINEAR (type 0x11 lores / 0x12 hires): interleaved {sprite,under}
 *       words (hires: {sprite,under1,under2,stale} dwords, both lines drawn). EAX is
 *       THREADED — a transparent pixel records the STALE AH from the last opaque one
 *       (reproduced by keeping the full 32-bit accumulator).
 *   [0x90c08]!=0 + [0x76634]==0 -> RAW 320x200 (type 0): same word records, stride 0x140.
 *   [0x90c08]!=0 + [0x76634]!=0 -> VESA BANKED (type 2): x&=~1; 16-bit SI window walk with
 *       per-pixel/per-row 64K wrap -> set_vesa_bank_page2 (bridge); saves the CURRENT bank
 *       [0x71dc6] and RESTORES it at exit via set_vesa_bank (bridge).
 *   else -> PLANAR Mode-X (type 1 lores / 0x80 hires): per plane (mask out 0x3c5, read-map
 *       out 0x3cf via g_os_port_out; [0x7e8cc] latch counter), column-major records,
 *       columns step by 4 pixels phase-shifted by -(x&3).
 * No register outputs consumed by callers; all record/screen writes are the contract. */
void blit_sprite_save_under(uint32_t y, uint32_t x, uint32_t wh, uint32_t sprite,
                                   uint32_t screen_base, uint32_t rec)
{
    uint8_t w = (uint8_t)wh, hgt = (uint8_t)(wh >> 8);
    int32_t pitch;

    if (G32(VA_g_linear_framebuffer_ptr) != 0) {                              /* ---- VESA LINEAR ---- */
        int32_t modew = G32(VA_g_video_mode_width);
        if (G8(VA_g_hires_line_doubling_flag) == 0) {                           /* lores (type 0x11) */
            uint32_t yy = y;
            if (screen_base != 0xa0000u) yy += (uint32_t)G32(VA_g_screen_height);
            *(uint32_t *)(uintptr_t)(rec + 8) = screen_base;
            uint32_t si = (uint32_t)G32(VA_g_linear_framebuffer_ptr) + x + (uint32_t)((int32_t)yy * modew);
            *(uint32_t *)(uintptr_t)(rec + 0) = si;
            *(uint16_t *)(uintptr_t)(rec + 4) = (uint16_t)wh;
            *(uint8_t  *)(uintptr_t)(rec + 7) = 0x11;
            uint32_t di = rec + 0xc;
            uint32_t eax = (uint32_t)((int32_t)yy * modew);   /* threaded accumulator */
            uint32_t sp = sprite;
            int32_t r = (int32_t)hgt;
            do {
                uint32_t s2 = si, b2 = sp;
                int32_t k = (int32_t)w;
                do {
                    eax = (eax & 0xffffff00u) | *(const uint8_t *)(uintptr_t)b2; b2++;
                    if ((uint8_t)eax != 0) {
                        eax = (eax & 0xffff00ffu)
                            | ((uint32_t)*(uint8_t *)(uintptr_t)s2 << 8);   /* ah=under */
                        *(uint8_t *)(uintptr_t)s2 = (uint8_t)eax;           /* draw */
                    }
                    s2++;
                    *(uint16_t *)(uintptr_t)di = (uint16_t)eax; di += 2;
                } while (--k > 0);
                sp += (uint32_t)G8(0x11aed);      /* SMC stride (patched by 0x118a9) */
                si += (uint32_t)modew;
            } while (--r > 0);
            return;
        }
        /* hires (type 0x12): dword records, both lines drawn */
        uint32_t yy = y + y;
        if (screen_base != 0xa0000u) yy += (uint32_t)G32(VA_g_screen_height);
        *(uint32_t *)(uintptr_t)(rec + 8) = screen_base;
        uint32_t si = (uint32_t)G32(VA_g_linear_framebuffer_ptr) + x + (uint32_t)((int32_t)yy * modew);
        *(uint32_t *)(uintptr_t)(rec + 0) = si;
        *(uint16_t *)(uintptr_t)(rec + 4) = (uint16_t)wh;
        *(uint8_t  *)(uintptr_t)(rec + 7) = 0x12;
        uint32_t di = rec + 0xc;
        uint32_t eax = (uint32_t)((int32_t)yy * modew);       /* threaded */
        uint32_t sp = sprite;
        int32_t r = (int32_t)hgt;
        do {
            uint32_t s2 = si, b2 = sp;
            int32_t k = (int32_t)w;
            do {
                eax = (eax & 0xffffff00u) | *(const uint8_t *)(uintptr_t)b2; b2++;
                if ((uint8_t)eax != 0) {
                    eax = (eax & 0xffff00ffu)
                        | ((uint32_t)*(uint8_t *)(uintptr_t)(s2 + (uint32_t)modew) << 8);
                    eax = (eax << 8) | (eax >> 24);           /* rol eax,8 */
                    eax = (eax & 0xffffff00u) | ((eax >> 8) & 0xffu);   /* mov al,ah */
                    eax = (eax & 0xffff00ffu)
                        | ((uint32_t)*(uint8_t *)(uintptr_t)s2 << 8);   /* ah=under1 */
                    *(uint8_t *)(uintptr_t)s2 = (uint8_t)eax;
                    *(uint8_t *)(uintptr_t)(s2 + (uint32_t)modew) = (uint8_t)eax;
                }
                s2++;
                *(uint32_t *)(uintptr_t)di = eax; di += 4;
            } while (--k > 0);
            sp += (uint32_t)G8(0x11b58);          /* SMC stride (patched by 0x118a9) */
            si += (uint32_t)modew * 2u;                       /* lea esi,[esi+edx*2] */
        } while (--r > 0);
        return;
    }
    if (G8(VA_g_rawscreen_flag) != 0) {
        if (G32(VA_g_video_linear_flag) == 0) {                          /* ---- RAW 320x200 (type 0) ---- */
            *(uint32_t *)(uintptr_t)(rec + 8) = screen_base;
            uint32_t si = screen_base + x + y * 0x140u;
            *(uint32_t *)(uintptr_t)(rec + 0) = si;
            *(uint16_t *)(uintptr_t)(rec + 4) = (uint16_t)wh;
            *(uint8_t  *)(uintptr_t)(rec + 7) = 0;
            uint32_t di = rec + 0xc;
            uint32_t eax = y * 0x140u;                        /* threaded (imul result) */
            uint32_t sp = sprite;
            int32_t r = (int32_t)hgt;
            do {
                uint32_t s2 = si, b2 = sp;
                int32_t k = (int32_t)w;
                do {
                    eax = (eax & 0xffffff00u) | *(const uint8_t *)(uintptr_t)b2; b2++;
                    if ((uint8_t)eax != 0) {
                        eax = (eax & 0xffff00ffu)
                            | ((uint32_t)*(uint8_t *)(uintptr_t)s2 << 8);
                        *(uint8_t *)(uintptr_t)s2 = (uint8_t)eax;
                    }
                    s2++;
                    *(uint16_t *)(uintptr_t)di = (uint16_t)eax; di += 2;
                } while (--k > 0);
                sp += (uint32_t)G8(0x11ba2);      /* SMC stride */
                si += 0x140u;
            } while (--r > 0);
            return;
        }
        /* ---- VESA BANKED (type 2) ---- */
        *(uint32_t *)(uintptr_t)(rec + 8) = screen_base;
        pitch = G32(VA_g_screen_pitch);
        uint32_t xe = x & ~1u;                            /* and dl,0xfe (bit0 only) */
        uint32_t lin = screen_base + xe + (uint32_t)((int32_t)y * pitch);
        *(uint32_t *)(uintptr_t)(rec + 0) = lin;
        uint16_t oldbank = (uint16_t)G16(VA_g_current_vesa_bank);        /* pushed; restored at exit */
        uint16_t bank = (uint16_t)(((lin >> 16) - 0xa) * (uint16_t)G16(VA_g_vesa_page_bank_offset));
        b2_set_vesa_bank(bank);
        uint16_t off = (uint16_t)lin;                     /* si (window 0xa0000 | off) */
        *(uint16_t *)(uintptr_t)(rec + 4) = (uint16_t)wh;
        *(uint8_t  *)(uintptr_t)(rec + 7) = 2;
        uint32_t di = rec + 0xc;
        uint32_t eax = (uint8_t)wh;                       /* movzx eax,cl */
        uint16_t bp16 = (uint16_t)(pitch - (int32_t)(uint8_t)wh);
        uint32_t sp = sprite;
        int32_t r = (int32_t)hgt;
        do {
            uint32_t b2 = sp;
            int32_t k = (int32_t)w;
            do {
                eax = (eax & 0xffffff00u) | *(const uint8_t *)(uintptr_t)b2; b2++;
                if ((uint8_t)eax != 0) {
                    eax = (eax & 0xffff00ffu)
                        | ((uint32_t)*(uint8_t *)(uintptr_t)(0xa0000u + off) << 8);
                    *(uint8_t *)(uintptr_t)(0xa0000u + off) = (uint8_t)eax;
                }
                *(uint16_t *)(uintptr_t)di = (uint16_t)eax; di += 2;
                uint16_t noff = (uint16_t)(off + 1);      /* add si,1 */
                if (noff < off) b2_set_vesa_bank_page2(); /* jb: 64K wrap */
                off = noff;
            } while (--k > 0);
            uint16_t noff = (uint16_t)(off + bp16);       /* add si,bp */
            if (noff < off) b2_set_vesa_bank_page2();
            off = noff;
            sp += (uint32_t)G8(0x11c22);          /* SMC stride */
        } while (--r > 0);
        b2_set_vesa_bank(oldbank);                        /* pop edx; restore the bank */
        return;
    }
    /* ---- PLANAR Mode-X (type 1 lores / 0x80 hires) ---- */
    pitch = 0;
    (void)pitch;
    G8(VA_g_screen_busy_depth + 0x4) = 0;                                      /* read-map latch counter */
    if (G8(VA_g_hires_line_doubling_flag) == 0) {                               /* lores (type 1) */
        *(uint32_t *)(uintptr_t)(rec + 8) = screen_base;
        int32_t yoff = (int32_t)(int16_t)(uint16_t)((uint16_t)y * 0x50u);   /* mul bp; cwde */
        uint32_t si0 = screen_base + (uint32_t)yoff + (x >> 2);
        *(uint32_t *)(uintptr_t)(rec + 0) = si0;
        uint8_t phase = (uint8_t)(0 - (x & 3));           /* ah = -(x&3) */
        *(uint16_t *)(uintptr_t)(rec + 4) = (uint16_t)wh;
        *(uint8_t  *)(uintptr_t)(rec + 6) = phase;
        *(uint8_t  *)(uintptr_t)(rec + 7) = 1;
        uint32_t di = rec + 0xc;
        uint32_t eax = 1 | ((uint32_t)phase << 8);        /* al=mask, ah=column start */
        while ((int8_t)((eax >> 8) & 0xff) < (int8_t)w) { /* cmp ah,cl; jge */
            uint32_t save_eax = eax;
            uint32_t si = si0;
            if (g_os_port_out) {
                g_os_port_out(0x3c5, (uint8_t)eax);     /* plane mask */
                g_os_port_out(0x3cf, (uint8_t)G8(VA_g_screen_busy_depth + 0x4));   /* read-map select */
            }
            G8(VA_g_screen_busy_depth + 0x4) = (uint8_t)(G8(VA_g_screen_busy_depth + 0x4) + 1);
            int8_t dh = (int8_t)(save_eax >> 8);
            if (dh >= 0)
                goto col_body_lo;
            goto col_adv_lo;
            while ((int8_t)dh < (int8_t)w) {
col_body_lo:;
                uint32_t s2 = si, b2 = sprite;
                int32_t r = (int32_t)hgt;
                do {
                    /* mov al,dh; xlatb */
                    eax = (eax & 0xffffff00u)
                        | *(const uint8_t *)(uintptr_t)(b2 + (uint8_t)dh);
                    if ((uint8_t)eax != 0) {
                        eax = (eax & 0xffff00ffu)
                            | ((uint32_t)*(uint8_t *)(uintptr_t)s2 << 8);
                        *(uint8_t *)(uintptr_t)s2 = (uint8_t)eax;
                    }
                    *(uint16_t *)(uintptr_t)di = (uint16_t)eax; di += 2;
                    s2 += 0x50; b2 += (uint32_t)G8(0x119e1);   /* SMC stride */
                } while (--r > 0);
col_adv_lo:
                si += 1;
                dh = (int8_t)(dh + 4);
            }
            eax = save_eax;
            eax = (eax & 0xffffff00u) | (uint8_t)((uint8_t)eax + (uint8_t)eax); /* add al,al */
            eax += 0x100;                                                       /* inc ah */
            if ((int8_t)((eax >> 8) & 0xff) >= 4) break;   /* cmp ah,4; jl */
        }
        return;
    }
    /* hires (type 0x80): dword column records, both plane lines drawn */
    *(uint32_t *)(uintptr_t)(rec + 8) = screen_base;
    uint32_t si0 = screen_base + y * 0xa0u + (x >> 2);    /* imul eax,eax,0xa0 (32-bit) */
    *(uint32_t *)(uintptr_t)(rec + 0) = si0;
    uint8_t phase = (uint8_t)(0 - (x & 3));
    *(uint32_t *)(uintptr_t)(rec + 4) = wh;               /* dword store, then patched: */
    *(uint8_t  *)(uintptr_t)(rec + 6) = phase;
    *(uint8_t  *)(uintptr_t)(rec + 7) = 0x80;
    uint32_t di = rec + 0xc;
    uint32_t eax = 1 | ((uint32_t)phase << 8);
    while ((int8_t)((eax >> 8) & 0xff) < (int8_t)w) {
        uint32_t save_eax = eax;
        uint32_t si = si0;
        if (g_os_port_out) {
            g_os_port_out(0x3c5, (uint8_t)eax);
            g_os_port_out(0x3cf, (uint8_t)G8(VA_g_screen_busy_depth + 0x4));
        }
        G8(VA_g_screen_busy_depth + 0x4) = (uint8_t)(G8(VA_g_screen_busy_depth + 0x4) + 1);
        int8_t dh = (int8_t)(save_eax >> 8);
        if (dh >= 0)
            goto col_body_hi;
        goto col_adv_hi;
        while ((int8_t)dh < (int8_t)w) {
col_body_hi:;
            uint32_t s2 = si, b2 = sprite;
            int32_t r = (int32_t)hgt;
            do {
                uint32_t e = *(const uint8_t *)(uintptr_t)(b2 + (uint8_t)dh); /* movzx+xlat */
                if ((uint8_t)e != 0) {
                    e |= (uint32_t)*(uint8_t *)(uintptr_t)(s2 + 0x50) << 8;   /* ah=under2 */
                    e = (e << 8) | (e >> 24);                                  /* rol 8 */
                    e = (e & 0xffffff00u) | ((e >> 8) & 0xffu);                /* al=ah */
                    e = (e & 0xffff00ffu)
                      | ((uint32_t)*(uint8_t *)(uintptr_t)s2 << 8);            /* ah=under1 */
                    *(uint8_t *)(uintptr_t)s2 = (uint8_t)e;
                    *(uint8_t *)(uintptr_t)(s2 + 0x50) = (uint8_t)e;
                }
                *(uint32_t *)(uintptr_t)di = e; di += 4;
                s2 += 0xa0; b2 += (uint32_t)G8(0x11a7b);       /* SMC stride */
            } while (--r > 0);
col_adv_hi:
            si += 1;
            dh = (int8_t)(dh + 4);
        }
        eax = save_eax;
        eax = (eax & 0xffffff00u) | (uint8_t)((uint8_t)eax + (uint8_t)eax);
        eax += 0x100;
        if ((int8_t)((eax >> 8) & 0xff) >= 4) break;
    }
}

/* draw_popup_shadow_border_smc (0x12dde, 228B): draw the 4px translucent drop-shadow BORDER
 * around a wxh rect at (x,y) — top/bottom edges via _rows, left/right via _column, four 4x4
 * corners via _block (TL/BL/BR/TR order), blending through fs=[0x90c0e] (the text colour-ramp
 * / blend LUT selector). Edges are clipped against the screen (top only when y>0, bottom when
 * y+h<[0x8549c], left when x>0, right when x+w<pitch); the CORNERS are unconditional (their
 * per-pixel clips handle the edges). In hires it first SMC-patches the seven overlay
 * immediates with pitch / 2*pitch (TRUE SMC, C2 — write the live code bytes so original and
 * lifted overlays interoperate). FS is push/pop'd — no segment postcondition.
 * ABI: EAX=x, EDX=y, EBX=w, ECX=h. Gradient sources: 0x708ec (top/left), 0x708f0
 * (bottom/right), corners 0x708f4/0x70914/0x70924/0x70904. */
void draw_popup_shadow_border_smc(int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (G8(VA_g_hires_line_doubling_flag) != 0) {                           /* hires: patch the overlay immediates */
        int32_t pitch = G32(VA_g_screen_pitch);
        G32(0x12f43) = pitch;  G32(0x12f4d) = pitch;  G32(0x12f31) = pitch;
        G32(0x12fe4) = pitch;  G32(0x12fed) = pitch;
        G32(0x12f5f) = pitch + pitch;  G32(0x13000) = pitch + pitch;
    }
    const uint8_t *lut = (const uint8_t *)(uintptr_t)
        (g_os_sel_base ? g_os_sel_base((uint16_t)G16(VA_g_text_color_ramp_selector)) : 0);
    const uint8_t *g_tl = (const uint8_t *)GADDR(VA_g_cursor_mask_data + 0x6);   /* top/left gradient */
    const uint8_t *g_br = (const uint8_t *)GADDR(VA_g_cursor_mask_data + 0xa);   /* bottom/right gradient */

    if (y > 0)                                        /* cmp edx,0; jle */
        draw_shadow_border_edge_h(x, y - 4, (uint32_t)w, g_tl, lut);
    if (y + h < G32(VA_g_screen_pitch + 0x4))                         /* cmp ebp,[0x8549c]; jge */
        draw_shadow_border_edge_h(x, y + h, (uint32_t)w, g_br, lut);
    if (x > 0)                                        /* cmp eax,0; jle */
        draw_shadow_border_edge_v(x - 4, y, (uint32_t)h, g_tl, lut);
    if (x + w < G32(VA_g_screen_pitch))                         /* cmp ebp,[0x85498]; jge */
        draw_shadow_border_edge_v(x + w, y, (uint32_t)h, g_br, lut);

    blit_translucent_overlay_block(x - 4, y - 4, (const uint8_t *)GADDR(VA_g_cursor_mask_data + 0xe), lut);
    blit_translucent_overlay_block(x - 4, y + h, (const uint8_t *)GADDR(VA_g_cursor_mask_data + 0x2e), lut);
    blit_translucent_overlay_block(x + w, y + h, (const uint8_t *)GADDR(VA_g_cursor_mask_data + 0x3e), lut);
    blit_translucent_overlay_block(x + w, y - 4, (const uint8_t *)GADDR(VA_g_cursor_mask_data + 0x1e), lut);
}
