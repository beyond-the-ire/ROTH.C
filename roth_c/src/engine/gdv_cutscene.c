/* lift_gdv_cutscene.c — verified-C lifts for the GDV (Gremlin Digital Video) cutscene player.
 *
 * Lift-lens      : docs/reference/lift/gdv_cutscene.md
 * Format/codec   : docs/reference/ROTH_gdv_format_notes.md
 *
 * STORED-POINTER HAZARD (gotcha A4 — the dominant data trap in this subsystem): the chunk-ring
 * globals DAT_91d6c (cur) / d68 (prev) / d50 (ring base) / d5c (ring end) hold RAW runtime/host
 * addresses (pointers into the streaming read buffer), NOT canon VAs. Access the GLOBAL SLOT via
 * G32(canon) (which adds OBJ_DELTA to the *address* 0x91d6c), but DEREFERENCE its VALUE raw (no
 * OBJ_DELTA) — the value is already a runtime pointer.
 */
#include <stdint.h>
#include <string.h>
#include "common.h"
#include "os_api.h"    /* C2: the os_audio_gdv_* SOS-driver service (client re-point) */

/* gdv_advance_chunk_ptr_inner (0x4dd38): advance the chunk-ring cursor past the current record.
 *
 *   prev = cur;
 *   cur += audio_sz;                                   // skip the audio sub-block
 *   cur += word16[cur + 2];                            // + this record's payload size
 *   cur  = (cur + 0xf) & ~7;                           // round up to an 8-byte boundary
 *   if (ring_end < cur + rec_stride) cur = ring_base;  // wrap if the next record won't fit
 *
 * Globals (canon): d6c=cur (stored ptr), d68=prev (stored ptr), d28=audio sub-block size,
 *   d9c=streaming record stride, d5c=ring end (stored ptr), d50=ring base (stored ptr).
 * ABI: __watcall void(void). Returns EAX = new cur (== [d6c]); preserves EDX; clears CF.
 * PURE — no I/O, no callees. The payload-size word is read RAW through the stored cur ptr (A4).
 *
 * Asm note: the original rounds with `add eax,0xf; and al,0xf8`. `and al,0xf8` clears only the low
 * byte's low 3 bits, but bits 0..2 are entirely within AL, so the net 32-bit result is identical to
 * `& 0xfffffff8u` (the upper 29 bits are untouched either way). Faithful. */
void gdv_advance_chunk_ptr_inner(void)
{
    uint32_t cur = (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x1c);          /* stored runtime ptr (A4: read slot, value is raw) */
    G32(VA_g_gdv_audio_stream_base + 0x18) = (int32_t)cur;                    /* prev = cur */

    uint32_t eax = cur + (uint32_t)G32(VA_g_gdv_context + 0x14);    /* skip the audio sub-block */
    uint16_t paysz = *(volatile uint16_t *)(uintptr_t)(eax + 2);  /* RAW deref of the stored ptr (A4) */
    eax += paysz;                                   /* + payload size */
    eax = (eax + 0xfu) & 0xfffffff8u;               /* round up to 8 (asm: add 0xf; and al,0xf8) */

    if ((uint32_t)G32(VA_g_gdv_audio_stream_base + 0xc) < eax + (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x4c))  /* next record won't fit -> wrap */
        eax = (uint32_t)G32(VA_g_gdv_audio_stream_base);               /* ring base */

    G32(VA_g_gdv_audio_stream_base + 0x1c) = (int32_t)eax;                    /* cur = new (also the EAX return value) */
}

/* gdv_init_pixel_tables (0x4bef0): build the colour-expansion LUT at the work-area base d3c, then
 * zero-fill the frame decode buffer at d40.
 *
 *   if (g_palette_dirty == 2)   // hi-colour: 0x400 pairs, step 0x200020
 *       for (i = 0x400; i > 0; i--) { tbl[k++]=eax; tbl[k++]=eax; eax += 0x200020; }
 *   else                        // 8-bpp: two passes of 256 pairs, step 0x1010101 (eax reset each pass)
 *       for (pass = 0; pass < 2; pass++) { eax=0; cl=0; do { tbl[k++]=eax; tbl[k++]=eax;
 *                                          eax += 0x1010101; } while (++cl != 0); }
 *   keyframe-pending [deb] = 0xff;
 *   memset32(d40, 0, (d94 + 3) >> 2);     // zero the decode buffer
 *   half-res-prev [ded] = 0;
 *
 * Globals (canon): d3c=work-area base (stored ptr A4), d40=frame-pixel base (stored ptr A4),
 *   d94=decode-buffer byte size, de6=g_palette_dirty (pixel format: 1=8bpp, 2=hi-colour),
 *   deb/ded = keyframe/half-res state bytes.
 * ABI: __watcall void(void). PURE — no I/O, no callees. Both buffers are reached RAW through the
 * stored pointers (A4); the original writes them via `stosd es:[edi]` (es==flat in the host). */
void gdv_init_pixel_tables(void)
{
    uint32_t *tbl = (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_gdv_decode_buffer + 0x8);   /* work-area base (A4 raw) */

    if ((uint8_t)G8(VA_g_palette_dirty) == 2) {                /* hi-colour */
        uint32_t eax = 0;
        for (int32_t ecx = 0x400; ecx > 0; ecx--) { /* asm: dec ecx; jg (signed) */
            *tbl++ = eax;
            *tbl++ = eax;
            eax += 0x200020u;
        }
    } else {                                        /* 8-bpp: two 256-entry passes */
        for (int pass = 0; pass < 2; pass++) {      /* asm: ch=2; dec ch; jne */
            uint32_t eax = 0;
            uint8_t cl = 0;
            do {                                    /* asm: inc cl; jne -> 256 iters */
                *tbl++ = eax;
                *tbl++ = eax;
                eax += 0x1010101u;
                cl++;
            } while (cl != 0);
        }
    }

    G8(VA_g_palette_dirty + 0x5) = (int8_t)0xff;                     /* keyframe-pending */

    uint32_t *p = (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_gdv_decode_buffer + 0xc);     /* frame-pixel base (A4 raw) */
    for (uint32_t n = ((uint32_t)G32(VA_g_gdv_audio_stream_base + 0x44) + 3u) >> 2; n != 0; n--)
        *p++ = 0;                                   /* zero the decode buffer (rep stosd) */

    G8(VA_g_palette_dirty + 0x7) = 0;                                /* half-res-prev */
}

/* gdv_reformat_pixel_buffer (0x4d541): when the per-frame half-res bits change (de2 ^ de3, bits
 * 0x08 vertical / 0x04 horizontal), de-interleave the decode buffer in place so it stays a valid
 * linear frame at the new resolution. Pure in-place transform of the buffer at d40 (A4 stored ptr);
 * dims from g_gdv_context (d14) +0x60 (width) / +0x62 (height) and d94 (buffer byte size).
 *
 * Four mutually-gated paths (each direction: change-set AND current-bit selects expand vs halve):
 *   vertical   change (chg&8): de2&8==0 -> EXPAND rows (double each source row into two);
 *                              de2&8!=0 -> HALVE  rows (keep every other row).
 *   horizontal change (chg&4): de2&4==0 -> EXPAND cols (byte -> doubled 16-bit word);
 *                              de2&4!=0 -> HALVE  cols (take low byte of each word).
 * The de2&4 (vertical paths) / de2&8 (horizontal paths) cross-terms halve the width/buffer span
 * because the OTHER dimension is already at half-res. The original is pushal/popal-framed and takes
 * no register args (Ghidra's `param_1` just passes through) -> __watcall void(void). PURE, no callees.
 *
 * Loop form: the original ends each loop with `dec ecx; jg` (signed) — a do-while that runs `count`
 * times for count>=1 (and once for count==0). Transcribed verbatim as `c_old=c; c--; if (c==0 ||
 * (int32_t)c_old <= 0) break;` so it is faithful for every input, not just count>0. */
void gdv_reformat_pixel_buffer(void)
{
    uint8_t de2 = (uint8_t)G8(VA_g_gdv_end_of_stream + 0x4);
    uint8_t chg = (uint8_t)(de2 ^ (uint8_t)G8(VA_g_gdv_end_of_stream + 0x5));
    uint8_t *buf  = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_gdv_decode_buffer + 0xc);   /* decode buffer (A4 raw) */
    uint32_t bufsz = (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x44);

    if (chg & 8) {                                  /* vertical resolution changed */
        uint8_t  *ctx = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_gdv_context);   /* gdv context (A4 raw) */
        uint32_t  w = *(uint16_t *)(ctx + 0x60);
        uint32_t  h = *(uint16_t *)(ctx + 0x62);
        if ((de2 & 8) == 0) {                       /* vertical EXPAND (double rows) */
            uint32_t W = w, sz = bufsz;
            if (de2 & 4) { sz = bufsz >> 1; W = w >> 1; }
            uint8_t *edi = buf + sz;                 /* dest cursor (walks down) */
            uint8_t *esi = buf + (sz >> 1);          /* src cursor  (walks down) */
            uint32_t outer = (uint16_t)h >> 1;
            for (;;) {
                edi -= W;
                uint32_t inner = W >> 2;
                for (;;) {
                    uint32_t v = *(uint32_t *)(esi - 4);
                    edi -= 4;
                    *(uint32_t *)(edi + W) = v;       /* lower copy of the doubled row */
                    esi -= 4;
                    *(uint32_t *)edi = v;             /* upper copy */
                    uint32_t io = inner; inner -= 1;
                    if (inner == 0 || (int32_t)io <= 0) break;
                }
                uint32_t oo = outer; outer -= 1;
                if (outer == 0 || (int32_t)oo <= 0) break;
            }
        } else {                                    /* vertical HALVE (keep alternate rows) */
            uint32_t W = w;
            if (de2 & 4) W = w >> 1;
            uint8_t *src = buf, *dst = buf;
            uint32_t outer = (uint16_t)h >> 1;
            for (;;) {
                uint32_t inner = W >> 2;
                for (;;) {
                    *(uint32_t *)dst = *(uint32_t *)src;
                    src += 4; dst += 4;
                    uint32_t io = inner; inner -= 1;
                    if (inner == 0 || (int32_t)io <= 0) break;
                }
                src += W;                            /* skip the discarded row */
                uint32_t oo = outer; outer -= 1;
                if (outer == 0 || (int32_t)oo <= 0) break;
            }
        }
    }

    if (chg & 4) {                                  /* horizontal resolution changed */
        if ((de2 & 4) == 0) {                       /* horizontal EXPAND (byte -> doubled word) */
            uint32_t sz = bufsz;
            if (de2 & 8) sz = bufsz >> 1;
            uint8_t *dst = buf + sz;                 /* dest cursor (walks down, 16-bit) */
            uint8_t *src = buf + (sz >> 1);          /* src cursor  (walks down, 8-bit) */
            uint32_t cnt = sz >> 1;
            for (;;) {
                uint8_t b = *(src - 1);
                src -= 1;
                dst -= 2;
                *(uint16_t *)dst = (uint16_t)(((uint32_t)b << 8) | b);
                uint32_t co = cnt; cnt -= 1;
                if (cnt == 0 || (int32_t)co <= 0) break;
            }
        } else {                                    /* horizontal HALVE (low byte of each word) */
            uint32_t cnt = bufsz >> 1;
            uint8_t *src = buf, *dst = buf;
            if (de2 & 8) cnt = bufsz >> 2;
            for (;;) {
                uint16_t v = *(uint16_t *)src;
                src += 2;
                *dst = (uint8_t)v;
                uint32_t co = cnt; cnt -= 1;
                dst += 1;
                if (cnt == 0 || (int32_t)co <= 0) break;
            }
        }
    }
}

/* gdv_init_frame_geometry (0x4bf4c): compute per-frame blit geometry from the header dims + the mode
 * dims + the stream flags — centering byte-offset (d870/da0/dd2), clip rect, scaled width/height
 * (ca8/cac/ccc/cd0/cd4/cc0), the 8 source-offset pointers (d74..d90), and stride deltas (da8/874/
 * 888/88c/890/894). Pure setup, no I/O, no callees.
 *
 * ABI: __watcall void(void) with a self-allocated 0x24-byte local frame (`sub esp,0x24; mov ebp,esp`).
 * Inputs (canon): stream flags = [d0c]; header ptr [d44] (+0x14 width / +0x16 height); context ptr
 * [d14] (+0x68 / +0x6c, the preset geometry used only when flags&0x4000); mode dims cec (W) / cf0 (H);
 * dest pitch ce8; decode-buffer base d40 (used only as an arithmetic addend for d74..d90 — never
 * dereferenced); g_palette_dirty de6 (==2 hi-colour); Mode-X/linear flag = high byte of [0x91dce]
 * (i.e. byte [0x91dcf]) & 0x80; VESA bank granularity dd0 (word). All outputs land in obj3 (A4: the
 * d74..d90 values = d40 + offset). Transcribed straight from the disasm; `sar` = signed >>, `shr` =
 * unsigned (cast to uint32_t), `and dl,0xfc` == `&~3` (bits 0..1 are in the low byte). The d44/d14
 * stored pointers are dereferenced RAW (A4). The dd2 store is 16-bit (`mov [dd2],ax`). */
void gdv_init_frame_geometry(void)
{
    int32_t L00 = 0, L04 = 0, L08 = 0, L0c = 0, L10 = 0, L14 = 0, L18 = 0, L1c = 0, L20 = 0;
    G8(VA_g_palette_dirty + 0x8) = 0;
    const uint8_t *hdr = (const uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_gdv_decode_buffer + 0x10);   /* esi (A4 raw) */
    const uint8_t *ctx = (const uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_gdv_context);   /* edi (A4 raw) */
    int32_t edx = *(const int32_t *)(ctx + 0x68);
    int32_t eax = *(const int32_t *)(ctx + 0x6c);
    uint32_t ecx = (uint32_t)G32(VA_g_gdv_stream_flags);          /* stream flags */

    if (!(ecx & 0x4000)) {                          /* not preset geometry: compute centering */
        eax = *(const uint16_t *)(hdr + 0x14);      /* width */
        if (ecx & 0x2000) eax += eax;
        eax -= G32(VA_g_dpcm_step_table + 0x448);
        eax = -eax;
        eax >>= 1;                                  /* sar */
        edx = *(const uint16_t *)(hdr + 0x16);      /* height */
        if (ecx & 0x10020) edx += edx;
        int to_fc0 = 0;
        if (ecx & 0x800000) {
            to_fc0 = 1;
        } else {
            edx -= G32(VA_g_dpcm_step_table + 0x44c);
            if (edx > 0 && (ecx & 0x80)) {          /* else: straight to neg below */
                edx += G32(VA_g_dpcm_step_table + 0x44c);
                to_fc0 = 1;
            }
        }
        if (to_fc0) {                               /* L4bfc0 */
            edx = (int32_t)((uint32_t)edx >> 1);    /* shr */
            edx -= G32(VA_g_dpcm_step_table + 0x44c);
        }
        edx = -edx;                                 /* L4bfc8 */
        edx >>= 1;                                  /* sar */
    }
    L1c = edx;
    L20 = eax;

    eax = *(const uint16_t *)(hdr + 0x14);          /* width */
    G32(VA_g_dpcm_step_table + 0x404) = eax;
    int32_t ebx = eax;
    if (ecx & 0x2000) eax += eax;
    edx = L20;
    eax += edx;
    if (eax >= G32(VA_g_dpcm_step_table + 0x448)) eax = G32(VA_g_dpcm_step_table + 0x448);    /* clamp to mode width (signed) */
    L00 = 0;
    if (edx < 0) {
        edx = -edx;
        if (ecx & 0x2000) edx = (int32_t)((uint32_t)edx >> 1);
        L00 = edx;
        edx = 0;
    }
    eax -= edx;
    L0c = G32(VA_g_dpcm_step_table + 0x448) - eax;
    L14 = eax;
    if (ecx & 0x2000) eax = (int32_t)((uint32_t)eax >> 1);
    ebx -= eax;
    L08 = ebx;
    L10 = edx;

    eax = *(const uint16_t *)(hdr + 0x16);          /* height */
    G32(VA_g_dpcm_step_table + 0x408) = eax;
    if (ecx & 0x10020) eax += eax;
    edx = L1c;
    eax += edx;
    int32_t cf0 = G32(VA_g_dpcm_step_table + 0x44c);
    ebx = eax;
    int32_t concat = (int32_t)(((uint32_t)eax & 0xffff0000u) | *(const uint16_t *)(hdr + 0x14));
    int land_87 = 0;
    if (ecx & 0x800000) {                           /* L4c071 */
        G8(VA_g_palette_dirty + 0x8) = 1;
        cf0 += cf0;
        L08 += concat;
        if (eax > cf0) land_87 = 1;
    } else if (eax <= cf0) {
        /* straight to L4c08b */
    } else if (!(ecx & 0x80)) {
        land_87 = 1;                                /* cf0 stays = original */
    } else {                                        /* fall into L4c071 */
        G8(VA_g_palette_dirty + 0x8) = 1;
        cf0 += cf0;
        L08 += concat;
        if (eax > cf0) land_87 = 1;
    }
    if (land_87) { eax = cf0; ebx = cf0; }          /* L4c087 */

    /* L4c08b */
    L04 = 0;
    edx = L1c;
    if (edx < 0) {
        edx = -edx;
        if (ecx & 0x10020) edx = (int32_t)((uint32_t)edx >> 1);
        L04 = edx;
        edx = 0;
    }
    ebx -= edx;
    L10 += edx * G32(VA_g_dpcm_step_table + 0x444);
    if (G8(VA_g_palette_dirty + 0x8) != 0) ebx = (int32_t)((uint32_t)ebx >> 1);
    if (ecx & 0x20)       ebx = (int32_t)((uint32_t)ebx >> 1);
    L18 = ebx;
    if (!(ecx & 0x4000)) L10 &= (int32_t)0xfffffffc;
    L0c += G32(VA_g_dpcm_step_table + 0x444) - G32(VA_g_dpcm_step_table + 0x448);
    G32(VA_g_dpcm_step_table + 0x42c) = L14 * L18;

    if ((uint8_t)G8(VA_g_palette_dirty) == 2 && !(ecx & 0x200)) {
        G32(VA_g_dpcm_step_table + 0x404) <<= 1;
        G32(VA_g_dpcm_step_table + 0x42c) <<= 1;
        L14 <<= 1; L08 <<= 1; L04 <<= 1; L00 <<= 1;
    }
    if (ecx & 0x20) L0c += G32(VA_g_dpcm_step_table + 0x444);
    int32_t lo2 = L14 & 3;
    L14 &= (int32_t)0xfffffffc;
    L0c += lo2;
    L08 += lo2;
    G32(VA_g_dpcm_step_table + 0x430) = L14;

    if ((uint8_t)G8(VA_g_gdv_audio_format + 0x5) & 0x80) {              /* Mode-X / non-linear surface */
        L10 = (int32_t)((uint32_t)L10 >> 2);
        L10 &= (int32_t)0xfffffffc;
        int32_t oldL14 = L14;
        L14 = (int32_t)((uint32_t)L14 >> 4);
        L14 <<= 1;
        int32_t e = oldL14 & 0xf;
        L08 += e;
        L0c += e;
        L0c = (int32_t)((uint32_t)L0c >> 2);
        G32(VA_g_dpcm_step_table + 0x430) = L14 << 3;
    }

    G32(VA_g_particle_pool + 0xc) = L10;
    G16(VA_g_gdv_audio_format + 0x8) = (uint16_t)((int16_t)(int16_t)(uint16_t)((uint32_t)L10 >> 16)
                              * (int16_t)(uint16_t)G16(VA_g_gdv_audio_format + 0x6));   /* 16-bit signed imul, word store */
    G32(VA_g_gdv_audio_stream_base + 0x50) = L10 & 0xffff;
    G32(VA_g_gdv_audio_stream_base + 0x58) = L0c;
    G32(VA_g_particle_pool + 0x10) = L0c + L14;

    int32_t base = G32(VA_g_gdv_decode_buffer + 0xc);
    int32_t iVar5 = (L04 & (int32_t)0xfffffffc) * G32(VA_g_dpcm_step_table + 0x404);   /* and dl,0xfc -> &~3 */
    int32_t ecx2 = iVar5;
    L00 &= (int32_t)0xfffffffc;                     /* in-place &~3 (affects all uses below) */
    int32_t iVar3 = iVar5 + L00;
    G32(VA_g_gdv_audio_stream_base + 0x24) = iVar3 + base;
    G32(VA_g_gdv_audio_stream_base + 0x3c) = iVar3 + base;
    G32(VA_g_gdv_audio_stream_base + 0x28) = (iVar3 >> 1) + base;             /* sar */
    G32(VA_g_gdv_audio_stream_base + 0x30) = ((iVar3 + L00) >> 2) + base;     /* sar */
    G32(VA_g_gdv_audio_stream_base + 0x2c) = (ecx2 >> 1) + L00 + base;        /* sar */
    G32(VA_g_gdv_audio_stream_base + 0x34) = L00 + base;
    G32(VA_g_gdv_audio_stream_base + 0x38) = (L00 >> 1) + base;               /* sar */
    G32(VA_g_gdv_audio_stream_base + 0x40) = ((L00 + L00) >> 2) + base;       /* sar */
    G32(VA_g_dpcm_step_table + 0x41c) = L08;

    int32_t cd4 = G32(VA_g_dpcm_step_table + 0x430);
    int32_t ca8 = G32(VA_g_dpcm_step_table + 0x404);
    int32_t a888 = ca8 - cd4;
    G32(VA_g_particle_pool + 0x24) = a888;
    int32_t d88c = ca8 + a888;
    G32(VA_g_particle_pool + 0x2c) = a888 >> 1;                       /* sar */
    G32(VA_g_particle_pool + 0x28) = d88c;
    G32(VA_g_particle_pool + 0x30) = d88c >> 1;                       /* sar */

    G32(VA_g_dpcm_step_table + 0x430) = L14;
    G32(VA_g_dpcm_step_table + 0x428) = L18;
}

/* ============================================================================================
 * gdv_decode_video_chunk (0x4d384): THE video bitstream decompressor. 2141 B, MIXED tier.
 *   entry bookkeeping (always) -> keyframe clear -> de2/de3 half-res update -> reformat -> dispatch
 *   on g_palette_dirty (de6) + sub = chunkflags & 0xf.
 * Sub-formats: 2/5/6/8 = pure LZ/RLE decoders (oracle); 0 = palette via write_vga_palette (bridge);
 *   1 = palette+clear+fade (DAC port-IO, in-game); 3 = no body (entry bookkeeping only).
 *
 * STATUS: format 8 (the dominant, ~96% of real frames) + format 3 are lifted in C and verified
 * byte-exact over a real .GDV frame SEQUENCE (test_gdv_cutscene.c harness). The remaining formats
 * (5/2/6 decoders + 0/1 palette) are TEMPORARILY bridged via call_orig until transcribed — so this
 * function is NOT yet in triage.py `done` (the gate). bit-reader/LZ semantics confirmed from disasm
 * (LE-low-bit accumulator, BL=bitcount, 16-bit LE refill; dist 0xfff = repeat-prev-byte;
 * dist>0xf80 && lowbyte==0xff = end). All `movsw`/`stosw` are 16-bit, forward; `movsb`/`rep movsb`
 * byte-granular forward; the d40 buffer is reached RAW (A4 stored ptr). */

/* --- format-8 bit reader: consume LOW bits from a 32-bit LE accumulator, refill a 16-bit LE word
 *     when the running bit-count drops to <= 0 (the `sub bl,n; jg` + `add bl,0x10; ror; mov dx,[esi];
 *     rol` idiom). p doubles as the byte-read cursor (esi is shared). --- */
typedef struct { uint32_t acc; int32_t bl; const uint8_t *p; } gdv_br;
static inline uint32_t gdv_ror32(uint32_t v, uint32_t n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }
static inline uint32_t gdv_rol32(uint32_t v, uint32_t n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }
static inline uint32_t gdv_take(gdv_br *s, int n)
{
    uint32_t val = s->acc & (uint32_t)((1u << n) - 1u);
    s->acc >>= n;
    s->bl -= n;
    if (s->bl <= 0) {                                   /* asm: sub bl,n; jg skip */
        s->bl += 0x10;
        uint32_t cl = (uint32_t)s->bl & 0x1f;
        s->acc = gdv_ror32(s->acc, cl);
        s->acc = (s->acc & 0xffff0000u) | (uint32_t)(s->p[0] | ((uint32_t)s->p[1] << 8));
        s->p += 2;
        s->acc = gdv_rol32(s->acc, cl);
    }
    return val;
}
static inline uint8_t gdv_byte(gdv_br *s) { return *s->p++; }

/* copy `cnt` bytes: optional leading movsb (cnt odd) + rep movsw (16-bit, forward; word read-then-write
 * so overlapping back-refs replicate the original 2-byte pattern). Advances + returns dst. */
static inline uint8_t *gdv_lz_copy(uint8_t *dst, const uint8_t *src, uint32_t cnt)
{
    if (cnt & 1) { *dst++ = *src++; }
    for (uint32_t w = cnt >> 1; w; w--) { uint8_t a = src[0], b = src[1]; dst[0] = a; dst[1] = b; dst += 2; src += 2; }
    return dst;
}
static inline uint8_t *gdv_copy_bytes(uint8_t *dst, const uint8_t *src, uint32_t cnt) /* rep movsb */
{ while (cnt--) *dst++ = *src++; return dst; }
static inline uint8_t *gdv_fill_prev(uint8_t *dst, uint32_t cnt)  /* LAB_4dc8e: repeat the previous byte */
{ uint8_t b = dst[-1]; if (cnt & 1) *dst++ = b; for (uint32_t w = cnt >> 1; w; w--) { dst[0] = b; dst[1] = b; dst += 2; } return dst; }

/* ---- sub-format 8 (0x4da30): the dominant full 12-bit LZ bitstream. Writes the DAT_91ca4 end-guard
 *      (= d40 + d94) and bounds the write loop with `dst > end`. The four ops: 0=literal/unary-run,
 *      1=skip, 3=12-bit back-ref (3-way 0x80/0x40 split + forward-ref), 2=short back-ref / fill-word
 *      / end marker (op2 sub-code). Verified byte-exact over real .GDV frame sequences. ---- */
static void gdv_fmt8(const uint8_t *payload, uint8_t *base, uint32_t flags)
{
    uint8_t *dst  = base + (flags >> 8);                /* resume offset */
    uint8_t *end  = base + (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x44);
    G32(VA_g_dpcm_step_table + 0x400) = (int32_t)(uint32_t)(uintptr_t)end;

    gdv_br br;
    br.acc = (uint32_t)(payload[0] | ((uint32_t)payload[1] << 8) |
                        ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24));
    br.p = payload + 4;
    br.bl = 0x10;

    for (;;) {
        if ((uintptr_t)dst > (uintptr_t)end) return;    /* ja 0x4dcc8 -> ret */
        uint32_t op = gdv_take(&br, 2);
        if (op == 0) {                                  /* literal / unary-length literal run */
            if (gdv_take(&br, 1) == 0) { *dst++ = gdv_byte(&br); continue; }
            /* unary length code. The original captures only `al` (8 bits) per iteration then masks
             * with ebp=(1<<cl)-1, while consuming `cl` bits. For cl>=9 that means v = acc&0xff (the
             * 9th+ bits are NOT read), and since acc&0xff <= 255 < mask, the loop always exits at cl=9.
             * gdv_take(cl) would read a 9th bit -> a too-large len -> overflow; so replicate the
             * 8-bit capture inline. */
            uint32_t len = 2, cl = 1, mask = 1;
            for (;;) {
                uint8_t al = (uint8_t)br.acc;           /* mov al, dl (pre-shift, 8-bit) */
                br.acc >>= cl; br.bl -= (int32_t)cl;
                if (br.bl <= 0) {
                    br.bl += 0x10; uint32_t rc = (uint32_t)br.bl & 0x1f;
                    br.acc = gdv_ror32(br.acc, rc);
                    br.acc = (br.acc & 0xffff0000u) | (uint32_t)(br.p[0] | ((uint32_t)br.p[1] << 8)); br.p += 2;
                    br.acc = gdv_rol32(br.acc, rc);
                }
                uint32_t v = (uint32_t)al & mask;        /* and eax, ebp */
                len += v;
                if (v != mask) break;
                cl++; mask = mask * 2 + 1;
            }
            dst = gdv_lz_copy(dst, br.p, len); br.p += len;
            continue;
        }
        if (op == 1) {                                  /* skip dst (delta: keep existing pixels) */
            if (gdv_take(&br, 1) == 0) { dst += gdv_take(&br, 4) + 2; continue; }
            uint32_t b = gdv_byte(&br);
            if (!(b & 0x80)) { dst += b + 0x12; continue; }
            uint32_t lo = gdv_byte(&br);
            dst += (((b & 0x7f) << 8) | lo) + 0x92;
            continue;
        }
        if (op == 3) {                                  /* 12-bit LZ back-ref copy */
            uint32_t b = gdv_byte(&br), count, dist;
            if (!(b & 0x80)) {
                count = (b >> 4) + 6;
                dist = ((b & 0xf) << 8) | gdv_byte(&br);
            } else if (!(b & 0x40)) {
                count = (uint8_t)(b - 0x72);
                uint32_t v4 = gdv_take(&br, 4);
                dist = (v4 << 8) | gdv_byte(&br);
            } else {                                    /* forward ref (+dist+1) */
                count = (uint8_t)(b - 0xb8);
                uint32_t v4 = gdv_take(&br, 4);
                dist = (v4 << 8) | gdv_byte(&br);
                dst = gdv_lz_copy(dst, dst + dist + 1, count);
                continue;
            }
            if (dist == 0xfff) { dst = gdv_fill_prev(dst, count); continue; }
            dst = gdv_lz_copy(dst, dst + dist - 0x1000, count);
            continue;
        }
        /* op == 2 */
        uint32_t op2 = gdv_take(&br, 2);
        if (op2 == 3) {                                 /* short byte-granular back-ref / repeat-prev */
            uint32_t b = gdv_byte(&br);
            uint32_t cnt = 2 + (b >> 7);
            uint32_t x = b & 0x7f;
            if (x == 0) { dst = gdv_fill_prev(dst, cnt); continue; }
            dst = gdv_copy_bytes(dst, dst - 1 - x, cnt);
            continue;
        }
        uint32_t v4 = gdv_take(&br, 4);
        uint32_t lo = gdv_byte(&br);
        uint32_t dist = (v4 << 8) | lo;
        if (op2 == 0 && dist > 0xf80) {                 /* special: end, or fill-word-from-back */
            if (lo == 0xff) return;
            uint32_t a = dist & 0x7f;
            uint32_t cnt = (a & 0xf) + 2;
            int32_t off = (int32_t)~(a >> 4);
            uint16_t w = *(const uint16_t *)(dst + off);
            for (uint32_t i = 0; i < cnt; i++) { dst[0] = (uint8_t)w; dst[1] = (uint8_t)(w >> 8); dst += 2; }
            continue;
        }
        uint32_t cnt = op2 + 3;
        if (dist == 0xfff) { dst = gdv_fill_prev(dst, cnt); continue; }
        dst = gdv_lz_copy(dst, dst + dist - 0x1000, cnt);
    }
}

/* ---- sub-format 6 (0x4d810): LZ variant. Byte-for-byte identical to format 8 for the literal (op0),
 *      skip (op1) and short-ref/fill-word/end (op2) paths, but: (a) it does NOT write/check the
 *      DAT_91ca4 end-guard (no `dst > end` bound — the stream self-terminates on the op2 end marker),
 *      and (b) op==3 uses a DIFFERENT 12-bit-LZ encoding: count = (b>>4)+6 with a 0xf escape that
 *      reads an extra count byte, dist = ((b&0xf)<<8)|next (no 0x80/0x40 sub-cases, no forward-ref).
 *      ZERO frames in any local cutscene -> synthetic / inspect-verified (the shared op0/1/2 paths are
 *      the same code proven on 90k+ fmt-8 frames; only op3 + the missing guard are new). ---- */
static void gdv_fmt6(const uint8_t *payload, uint8_t *base, uint32_t flags)
{
    uint8_t *dst = base + (flags >> 8);
    gdv_br br;
    br.acc = (uint32_t)(payload[0] | ((uint32_t)payload[1] << 8) |
                        ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24));
    br.p = payload + 4;
    br.bl = 0x10;

    for (;;) {
        uint32_t op = gdv_take(&br, 2);
        if (op == 0) {                                  /* literal / unary-length run (== fmt8) */
            if (gdv_take(&br, 1) == 0) { *dst++ = gdv_byte(&br); continue; }
            uint32_t len = 2, cl = 1, mask = 1;
            for (;;) {
                uint8_t al = (uint8_t)br.acc;
                br.acc >>= cl; br.bl -= (int32_t)cl;
                if (br.bl <= 0) {
                    br.bl += 0x10; uint32_t rc = (uint32_t)br.bl & 0x1f;
                    br.acc = gdv_ror32(br.acc, rc);
                    br.acc = (br.acc & 0xffff0000u) | (uint32_t)(br.p[0] | ((uint32_t)br.p[1] << 8)); br.p += 2;
                    br.acc = gdv_rol32(br.acc, rc);
                }
                uint32_t v = (uint32_t)al & mask;
                len += v;
                if (v != mask) break;
                cl++; mask = mask * 2 + 1;
            }
            dst = gdv_lz_copy(dst, br.p, len); br.p += len;
            continue;
        }
        if (op == 1) {                                  /* skip (== fmt8) */
            if (gdv_take(&br, 1) == 0) { dst += gdv_take(&br, 4) + 2; continue; }
            uint32_t b = gdv_byte(&br);
            if (!(b & 0x80)) { dst += b + 0x12; continue; }
            uint32_t lo = gdv_byte(&br);
            dst += (((b & 0x7f) << 8) | lo) + 0x92;
            continue;
        }
        if (op == 2) {                                  /* short back-ref / fill-word / end (== fmt8) */
            uint32_t op2 = gdv_take(&br, 2);
            if (op2 == 3) {
                uint32_t b = gdv_byte(&br);
                uint32_t cnt = 2 + (b >> 7);
                uint32_t x = b & 0x7f;
                if (x == 0) { dst = gdv_fill_prev(dst, cnt); continue; }
                dst = gdv_copy_bytes(dst, dst - 1 - x, cnt);
                continue;
            }
            uint32_t v4 = gdv_take(&br, 4);
            uint32_t lo = gdv_byte(&br);
            uint32_t dist = (v4 << 8) | lo;
            if (op2 == 0 && dist > 0xf80) {
                if (lo == 0xff) return;                 /* the only loop exit (no end-guard) */
                uint32_t a = dist & 0x7f;
                uint32_t cnt = (a & 0xf) + 2;
                int32_t off = (int32_t)~(a >> 4);
                uint16_t w = *(const uint16_t *)(dst + off);
                for (uint32_t i = 0; i < cnt; i++) { dst[0] = (uint8_t)w; dst[1] = (uint8_t)(w >> 8); dst += 2; }
                continue;
            }
            uint32_t cnt = op2 + 3;
            if (dist == 0xfff) { dst = gdv_fill_prev(dst, cnt); continue; }
            dst = gdv_lz_copy(dst, dst + dist - 0x1000, cnt);
            continue;
        }
        /* op == 3: 12-bit LZ (fmt6 encoding — full count nibble + 0xf escape; no 0x80/0x40 cases) */
        uint32_t b = gdv_byte(&br);
        uint32_t count = b >> 4;
        if (count == 0xf) count = gdv_byte(&br) + 0xf;  /* cl==0xf -> +next byte */
        count += 6;
        uint32_t dist = ((b & 0xf) << 8) | gdv_byte(&br);
        if (dist == 0xfff) { dst = gdv_fill_prev(dst, count); continue; }
        dst = gdv_lz_copy(dst, dst + dist - 0x1000, count);
    }
}

/* ---- sub-format 5 (0x4d732): nibble-RLE. Byte-oriented (no bit accumulator); each control byte holds
 *      four 2-bit codes (MSB pair first): 0=literal, 1=12-bit back-ref, 2=skip/end, 3=short back-ref.
 *      Verified byte-exact on HAWK03A (the only fmt-5 cutscene). ---- */
static void gdv_fmt5(const uint8_t *payload, uint8_t *base, uint32_t flags)
{
    const uint8_t *p = payload;
    uint8_t *dst = base + (flags >> 8);
    for (;;) {
        uint32_t b = *p++;
        for (int k = 0; k < 4; k++) {
            uint32_t code = (b >> (6 - 2 * k)) & 3;
            if (code == 0) {                            /* single literal */
                *dst++ = *p++;
            } else if (code == 1) {                     /* 12-bit back-ref (2-byte descriptor) */
                uint32_t w = (uint32_t)(p[0] | ((uint32_t)p[1] << 8)); p += 2;
                uint32_t dist = w >> 4, cnt = (w & 0xf) + 3;
                if (dist == 0xfff) dst = gdv_fill_prev(dst, cnt);
                else dst = gdv_lz_copy(dst, dst + dist - 0x1000, cnt);
            } else if (code == 2) {                     /* skip (delta) or end */
                uint32_t d = *p;
                if (d == 0) return;                     /* end of frame */
                if (d == 0xff) { uint32_t w = (uint32_t)(p[1] | ((uint32_t)p[2] << 8)); p += 3; dst += w + 1; }
                else { p++; dst += d + 1; }
            } else {                                    /* code == 3: short back-ref (1-byte) */
                uint32_t d = *p++;
                uint32_t dist = d >> 2, cnt = (d & 3) + 2;
                if (dist == 0) dst = gdv_fill_prev(dst, cnt);
                else dst = gdv_lz_copy(dst, dst - dist - 1, cnt);
            }
        }
    }
}

/* ---- sub-format 2, 8-bpp (0x4d64a): 2-bit-code RLE. Optionally (re)seeds the d3c colour-expand LUT
 *      (a dirty-flag pattern keyed on byte[d3c+8]), then walks control bytes of four MSB-first 2-bit
 *      codes: 0=literal, 1=12-bit back-ref, 2=skip(+2), 3=end. dst starts at d40 (NO resume offset).
 *      ZERO frames in any local cutscene -> synthetic-verified. ---- */
static void gdv_fmt2_8bpp(const uint8_t *payload, uint8_t *base)
{
    uint8_t *d3c = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_gdv_decode_buffer + 0x8);
    if (d3c[8] != 0) {                                  /* cmp byte[d3c+8],0; je skip -> seed if !=0 */
        uint32_t *tbl = (uint32_t *)d3c;
        uint32_t eax = 0; uint8_t cl = 0;
        do { tbl[0] = tbl[1] = tbl[2] = tbl[3] = eax; tbl += 4; eax += 0x1010101u; cl++; } while (cl != 0);
    }
    const uint8_t *p = payload;
    uint8_t *dst = base;
    for (;;) {
        uint32_t acc = *p++;                            /* rolling 2-bit extractor: shl eax,2; and ah,3 */
        for (int k = 0; k < 4; k++) {
            acc <<= 2;
            uint32_t code = (acc >> 8) & 3;
            if (code == 0) { *dst++ = *p++; }
            else if (code == 1) {
                uint32_t w = (uint32_t)(p[0] | ((uint32_t)p[1] << 8));
                uint32_t dist = w >> 4, cnt = (w & 0xf) + 3;
                dst = gdv_lz_copy(dst, dst + dist - 0x1000, cnt);
                p += 2;
            }
            else if (code == 2) { uint32_t d = *p++; dst += d + 2; }
            else return;                                /* code 3: end */
        }
    }
}

/* ---- sub-format 2, hi-colour (0x4d6bc): the de6==2 16-bit-pixel variant of fmt-2. Seeds the hi-colour
 *      LUT (dirty-flag = word[d3c+8]==0x20), then the same four codes but pixel-granular (2 B): 0=1-pixel
 *      literal, 1=12-bit pixel back-ref (cnt words, no leading byte), 2=skip(+2 pixels), 3=end.
 *      ZERO frames in any local cutscene -> synthetic-verified. ---- */
static void gdv_fmt2_hi(const uint8_t *payload, uint8_t *base)
{
    uint8_t *d3c = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_gdv_decode_buffer + 0x8);
    if (*(uint16_t *)(d3c + 8) != 0x20) {               /* cmp word[d3c+8],0x20; je skip */
        uint32_t *tbl = (uint32_t *)d3c;
        uint32_t eax = 0;
        for (int32_t ecx = 0x400; ecx > 0; ecx--) { tbl[0] = tbl[1] = eax; tbl += 2; eax += 0x200020u; }
    }
    const uint8_t *p = payload;
    uint8_t *dst = base;                                /* 16-bit pixels */
    for (;;) {
        uint32_t acc = *p++;
        for (int k = 0; k < 4; k++) {
            acc <<= 2;
            uint32_t code = (acc >> 8) & 3;
            if (code == 0) { dst[0] = p[0]; dst[1] = p[1]; dst += 2; p += 2; }
            else if (code == 1) {
                uint32_t w = (uint32_t)(p[0] | ((uint32_t)p[1] << 8));
                uint32_t dist = w >> 4, cnt = (w & 0xf) + 3;
                const uint8_t *src = dst + dist * 2 - 0x2000;   /* lea esi,[edi+esi*2-0x2000] */
                for (uint32_t i = 0; i < cnt; i++) { dst[0] = src[0]; dst[1] = src[1]; dst += 2; src += 2; }
                p += 2;
            }
            else if (code == 2) { uint32_t d = *p++; dst += d * 2 + 4; }
            else return;                                /* code 3: end */
        }
    }
}

/* ---- sub-format 0 (0x4d51b): palette frame. Copy the 0x300-byte palette to header[d44]+0x18, then
 *      hand it to write_vga_palette (0x4c334, ebx=0 start, ecx=0x100 count) — that fn does DAC port-I/O
 *      (and an optional registered callback via [0x91d00]), so it is a legit SUB-BRIDGE to a different
 *      function (video_display subsystem). ZERO frames in any local cutscene; the bridged callee's
 *      `out` is privileged -> cannot be oracled -> inspect-only. ---- */
static void gdv_fmt0(const uint8_t *payload)
{
    G8(VA_g_gdv_end_of_stream + 0x7) = 1;
    uint8_t *pal = (uint8_t *)(uintptr_t)((uint32_t)G32(VA_g_gdv_decode_buffer + 0x10) + 0x18);
    memcpy(pal, payload, 0x300);                        /* rep movsd ecx=0xc0 */
    write_vga_palette(0, (uint32_t)(uintptr_t)pal, 0, 0x100);  /* write_vga_palette(EDX=src,EBX=0,ECX=0x100) — re-pointed */
}

/* ---- sub-format 1 (0x4d490): palette + clear + optional DAC fade. The fade sub-path (taken only when
 *      flags&0xffffff00 != 0 && de6==1 && [d0c]&0x8000) issues the only port-I/O in the codec — four
 *      `out 0x3c8/0x3c9` DAC writes setting palette entry 0xff to a 6-bit RGB from the flags, and fills
 *      the decode buffer with 0xff (= that entry). Those DAC writes route through the g_os_port_out
 *      host hook (NULL in the oracle, where the fade path is staged out — the original's `out` would
 *      fault under bare-metal call_orig). The non-fade path (buffer clear to 0 + palette copy + state
 *      bytes) is oracle-verifiable; the fade path is in-game-live-swap-verified ([[in-game-testing-welcome]]).
 *      ~23 frames corpus-wide (INTRO/MOVIE/EPILOGUE/FULLKEEP). ---- */
static void gdv_fmt1(const uint8_t *chunk, uint32_t flags)
{
    G8(VA_g_gdv_end_of_stream + 0x7) = 1;
    uint32_t edx = flags & 0xffffff00u;                 /* mov edx,[esi-4]; sub dl,dl */
    G8(VA_g_palette_dirty + 0x6) = 0;                                    /* mov [dec], dl (=0) */
    uint32_t fillval = 0;                               /* sub eax,eax */

    if (edx != 0 && (uint8_t)G8(VA_g_palette_dirty) == 1 && (G32(VA_g_gdv_stream_flags) & 0x8000)) {
        G8(VA_g_palette_dirty + 0x6) = (int8_t)0xff;
        if (g_os_port_out) {                          /* live-swap: drive the real DAC; NULL in oracle */
            uint32_t rgb = (edx >> 0xa) & 0x3f3f3fu;    /* shr eax,0xa; and eax,0x3f3f3f */
            g_os_port_out(0x3c8, 0xff);               /* out 0x3c8, 0xff (write index 255) */
            g_os_port_out(0x3c9, (uint8_t)rgb);        /* out 0x3c9, R */
            g_os_port_out(0x3c9, (uint8_t)(rgb >> 8)); /* out 0x3c9, G */
            g_os_port_out(0x3c9, (uint8_t)(rgb >> 16));/* out 0x3c9, B */
        }
        fillval = 0xffffffffu;                          /* or eax,0xffffffff */
        G8(VA_g_palette_dirty + 0x5) = (int8_t)0xff;                     /* mov [deb], al (=0xff) */
    }

    uint32_t *clr = (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_gdv_decode_buffer + 0xc);   /* fill decode buf with eax */
    for (uint32_t n = ((uint32_t)G32(VA_g_gdv_audio_stream_base + 0x44) + 3u) >> 2; n; n--) *clr++ = fillval;
    G8(VA_g_palette_dirty + 0x7) = 0;
    G8(VA_g_gdv_end_of_stream + 0x6) = 2;

    uint8_t *pal = (uint8_t *)(uintptr_t)((uint32_t)G32(VA_g_gdv_decode_buffer + 0x10) + 0x18);
    memcpy(pal, chunk + 8, 0x300);                      /* rep movsd ecx=0xc0 from payload */
}

void gdv_decode_video_chunk(void)
{
    /* ---- entry bookkeeping (0x4d384..0x4d390): runs even on bad magic ---- */
    G32(VA_g_gdv_audio_stream_base + 0x4) += G32(VA_g_gdv_audio_stream_base + 0x8);
    *(volatile uint16_t *)GADDR(VA_g_gdv_audio_format + 0xa) += 1;          /* inc word [dd4] */

    uint32_t d6c = (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x1c), d28 = (uint32_t)G32(VA_g_gdv_context + 0x14);
    const uint8_t *chunk = (const uint8_t *)(uintptr_t)(d6c + d28);
    if (*(const uint16_t *)chunk != 0x1305) {           /* bad magic -> g_gdv_end_of_stream (0x4d37c) */
        G8(VA_g_gdv_end_of_stream) = (int8_t)0xff;
        return;
    }
    uint32_t flags = *(const uint32_t *)(chunk + 4);
    uint32_t sub   = flags & 0xf;

    if ((uint8_t)G8(VA_g_palette_dirty + 0x5) != 0 || (chunk[4] & 0x40)) {   /* keyframe clear */
        uint32_t *clr = (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_gdv_decode_buffer + 0xc);
        for (uint32_t n = ((uint32_t)G32(VA_g_gdv_audio_stream_base + 0x44) + 3u) >> 2; n; n--) *clr++ = 0;
        G8(VA_g_palette_dirty + 0x5) = 0; G8(VA_g_palette_dirty + 0x7) = 0;
    }
    G16(VA_g_gdv_audio_stream_base + 0x70) = (uint16_t)(flags & 0xff);
    if (flags & 0x10) G8(VA_g_gdv_end_of_stream + 0x4) |= 4; else G8(VA_g_gdv_end_of_stream + 0x4) &= (int8_t)0xfb;
    if (flags & 0x20) G8(VA_g_gdv_end_of_stream + 0x4) |= 8; else G8(VA_g_gdv_end_of_stream + 0x4) &= (int8_t)0xf7;
    if ((uint8_t)G8(VA_g_gdv_end_of_stream + 0x4) != (uint8_t)G8(VA_g_gdv_end_of_stream + 0x5)) {
        if ((uint8_t)G8(VA_g_palette_dirty + 0x7) != 0) gdv_reformat_pixel_buffer();
        G8(VA_g_gdv_end_of_stream + 0x5) = G8(VA_g_gdv_end_of_stream + 0x4);
    }
    G8(VA_g_palette_dirty + 0x7) = 1;

    const uint8_t *payload = chunk + 8;
    uint8_t *base = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_gdv_decode_buffer + 0xc);
    uint8_t de6 = (uint8_t)G8(VA_g_palette_dirty);

    /* ---- dispatch on pixel format (de6) + sub-format (flags & 0xf). 8bpp at 0x4d463, hi-colour at
     *      0x4d459; subs with no body (incl. 3) just return. Palette formats 0/1 are shared. ---- */
    if (de6 == 1) {                                     /* 8-bpp */
        switch (sub) {
        case 8: gdv_fmt8(payload, base, flags);  break;
        case 6: gdv_fmt6(payload, base, flags);  break;
        case 5: gdv_fmt5(payload, base, flags);  break;
        case 2: gdv_fmt2_8bpp(payload, base);    break;
        case 0: gdv_fmt0(payload);               break;
        case 1: gdv_fmt1(chunk, flags);          break;
        default: break;                          /* no body (incl. sub==3) */
        }
    } else if (de6 == 2) {                              /* hi-colour */
        switch (sub) {
        case 2: gdv_fmt2_hi(payload, base);      break;
        case 0: gdv_fmt0(payload);               break;
        case 1: gdv_fmt1(chunk, flags);          break;
        default: break;                          /* no body */
        }
    }
    /* de6 not in {1,2}: no body (0x4d458 ret) */
}

/* find_gdv_error_index (0x26a20): map a GDV error WORD to a category row 0..3, used by the cutscene
 * error box (show_cutscene_error_box). Circularly scans the 4-entry list table starting at row
 * [0x83eac]: row r's space-separated, case-insensitive word-list is lists[r] (lists = *[0x83ea8], the
 * struct's first field); the first row whose list contains the input word wins (match_word_in_list_ci
 * 0x150b8 != 0 -> already-lifted PURE leaf). No match (or a NULL table) -> 0x32 (the "unknown" index).
 * ABI: __watcall EAX=word ptr -> EAX=row; preserves EBX/ECX/EDX/ESI (all pushed). PURE except the one
 * lifted callee. The asm advance is `inc; cmp 4; jb; xor` (clamp-to-0 at 4) and loops while the next row
 * != the start row — i.e. it visits all 4 rows once. (Assumes [0x83eac] in 0..3, as the original does.) */
uint32_t find_gdv_error_index(const uint8_t *word)
{
    uint32_t start = (uint32_t)G32(VA_g_playback_menu_scroll_anchor + 0x8);            /* mov ebx,[0x83eac] (rotating start row) */
    uint32_t structp = (uint32_t)G32(VA_g_playback_menu_scroll_anchor + 0x4);
    if (structp == 0) return 0x32;                      /* cmp [0x83ea8],0; je default */
    const uint32_t *lists = (const uint32_t *)(uintptr_t)(*(const uint32_t *)(uintptr_t)structp); /* esi=[[0x83ea8]] */

    uint32_t row = start;
    for (;;) {
        const uint8_t *list = (const uint8_t *)(uintptr_t)lists[row];   /* edx = [esi + row*4] */
        if (list != NULL && match_word_in_list_ci(word, list) != 0)
            return row;                                 /* first matching row */
        row++; if (row >= 4) row = 0;                   /* inc; cmp 4; jb; xor */
        if (row == start) break;                        /* cmp ebx,[0x83eac]; jne loop */
    }
    return 0x32;
}

/* gdv_build_scale_table (0x4bb62): build the 256-dword scale/offset table at 0x918a4 from constants,
 * guarded by [0x9196c]==0 ("already built"). Entry 0 = 0; then a run of (val, -val) pairs where val
 * accumulates `eax>>5` and eax/edx walk by an increasing step; a final lone `val` lands at 0x91ca0.
 * PURE (the original is pushal/popal-framed, so it preserves all regs). All writes are obj3. */
static void gdv_build_scale_table(void)
{
    if ((uint32_t)G32(VA_g_dpcm_step_table + 0xc8) != 0) return;            /* cmp [0x9196c],0; jne ret */
    uint32_t eax = 0x40, edx = 0x2d, ecx = 0;
    uint32_t *edi = (uint32_t *)GADDR(VA_g_dpcm_step_table);
    *edi++ = 0;                                         /* [0x918a4] = 0; edi += 4 */
    do {                                               /* 0x4bb86 */
        ecx += eax >> 5;                               /* ebp = eax>>5; ecx += ebp */
        eax += edx;
        edx += 2;
        edi[0] = ecx;                                  /* [edi] = ecx */
        edi[1] = (uint32_t)(-(int32_t)ecx);            /* [edi+4] = -ecx */
        edi += 2;                                      /* edi += 8 */
    } while ((uintptr_t)edi < (uintptr_t)GADDR(VA_g_dpcm_step_table + 0x3fc));   /* cmp edi,0x91ca0; jl */
    ecx += eax >> 5;                                   /* tail: ebp=eax>>5; ecx+=ebp */
    *edi = ecx;                                        /* [0x91ca0] = ecx */
}

/* gdv_setup_decode_buffers (0x4ba30): lay out the decode work area from the allocated base [d34] — the
 * audio scratch [d38] (filled 0x80 or 0 per [dca]&4), the scale table (if [dca]&8), the work/pixel base
 * [d3c]/[d40] (+cc4 back-ref guard), and the streaming chunk ring [d50]/[d5c] + frame count [db6] — then
 * stashes the per-frame header field [0x91878] and the half-res flag [def]. Present fn-ptrs at 0x91898/
 * 9189c/918a0 select the audio-mixed vs plain path.
 * ABI: __watcall void -> CF (set = failure) + EBP (error code 0x12 on the two fail paths; gdv_decoder_open
 * returns EBP as its result when CF is set). The C lift returns CF (1=fail) and writes *ebp_out on fail.
 * Inputs (canon globals): d34 (alloc base, A4 stored ptr — only the [d38..] fill derefs it), d2c (audio
 * size), dc8 (audio-present), dca (layout flags 4/8), cc4 (guard), d94 (decode size), d9c (record stride,
 * MUST be != 0 — `div`), d44 (header ptr, reads +6), d0c (stream flags), da4 (alloc size). Transcribed
 * from disasm: `and dl,0xfc`==&~3 / `and dl,0xf8`==&~7 (low-byte clears = whole-dword clears since the
 * rounding add precedes them); `div [d9c]` is unsigned with EDX=0; the count gate `cmp ax,1; jle` is a
 * SIGNED 16-bit compare; the fn-ptr immediates are runtime (rebased) code addresses (+OBJ_DELTA). */
uint32_t gdv_setup_decode_buffers(uint32_t *ebp_out)
{
    uint32_t edx = (uint32_t)G32(VA_g_gdv_decode_buffer);              /* decode base (A4) */
    edx = (edx + 0x61b) & 0xfffffffcu;                  /* add 0x61b; and dl,0xfc */

    if ((uint8_t)G8(VA_g_gdv_audio_enabled) != 0) {                    /* cmp [dc8],0; je 0x4bad5 (skip if 0) */
        uint32_t d2c = (uint32_t)G32(VA_g_gdv_audio_buf_size);
        if (d2c == 0) {
            G8(VA_g_gdv_audio_enabled) = 0;                            /* [dc8] = 0; fall to 0x4bad5 */
        } else {
            uint8_t *edi = (uint8_t *)(uintptr_t)edx;
            uint32_t fill = ((uint8_t)G8(VA_g_gdv_audio_format) & 4) ? 0u : 0x80808080u;  /* test [dca],4 */
            G32(VA_g_gdv_decode_buffer + 0x4) = (int32_t)edx;                /* [d38] = audio scratch base */
            uint32_t ecx = d2c;
            edx += ecx;
            for (uint32_t n = (ecx + 3) >> 2; n; n--) { *(uint32_t *)edi = fill; edi += 4; }  /* rep stosd */
            edx = (edx + 7) & 0xfffffff8u;              /* add 7; and dl,0xf8 */
            G32(VA_g_particle_pool + 0x34) = (int32_t)(0x4e45fu + OBJ_DELTA);   /* present fn-ptr (rebased) */
            if ((uint8_t)G8(VA_g_gdv_audio_format) & 8) {             /* test [dca],8; je 0x4bad5 */
                gdv_build_scale_table();
                G32(VA_g_gdv_decode_buffer + 0x18) = (int32_t)edx;
                edx += (uint32_t)G32(VA_g_gdv_audio_buf_size);
                edx = (edx + 7) & 0xfffffffcu;          /* add 7; and dl,0xfc */
                G32(VA_g_particle_pool + 0x34) = (int32_t)(0x4e460u + OBJ_DELTA);   /* overwrite fn-ptr (rebased) */
                G32(VA_g_particle_pool + 0x38) = 0;
                G32(VA_g_particle_pool + 0x3c) = 0;
            }
        }
    }

    /* 0x4bad5 */
    G32(VA_g_gdv_decode_buffer + 0x8) = (int32_t)edx;                        /* work-area base */
    G32(VA_g_gdv_decode_buffer + 0xc) = (int32_t)edx;                        /* decode-pixel base ... */
    uint32_t eax = (uint32_t)G32(VA_g_dpcm_step_table + 0x420);
    G32(VA_g_gdv_decode_buffer + 0xc) += (int32_t)eax;                       /* ... + cc4 back-ref guard */
    eax += (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x44);
    eax += 0x110;
    edx += eax;
    edx &= 0xfffffff8u;                                 /* and dl,0xf8 */
    uint32_t d50 = edx;
    G32(VA_g_gdv_audio_stream_base) = (int32_t)d50;                        /* chunk-ring base */

    int32_t avail = (int32_t)((uint32_t)G32(VA_g_gdv_decode_buffer) - d50 + (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x54));
    if (avail < 0) { *ebp_out = 0x12; return 1; }       /* js 0x4bb5b: ebp=0x12; stc */

    G32(VA_g_gdv_audio_stream_base + 0xc) = (int32_t)((uint32_t)avail + d50);    /* chunk-ring end (= d34 + da4) */
    uint32_t count = (uint32_t)avail / (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x4c);   /* div [d9c] (EDX=0) */
    G16(VA_g_gdv_audio_stream_base + 0x66) = (uint16_t)count;
    if ((int16_t)(uint16_t)count <= 1) { *ebp_out = 0x12; return 1; }   /* cmp ax,1; jle (signed) */

    uint32_t hdr = (uint32_t)G32(VA_g_gdv_decode_buffer + 0x10);              /* header ptr (A4) */
    G32(VA_g_particle_pool + 0x14) = (int32_t)(*(const uint16_t *)(uintptr_t)(hdr + 6));  /* ax=[hdr+6]; and eax,0xffff */
    if ((uint32_t)G32(VA_g_gdv_stream_flags) & 8) G8(VA_g_palette_dirty + 0x9) = 1;    /* test [d0c],8 */
    return 0;                                           /* clc */
}

/* ============================================================================================
 * Palette / DAC cluster (sub-C). All five do VGA DAC port-I/O (`out 0x3c8/0x3c9`) which is
 * privileged and faults under bare-metal call_orig — so they are IN-GAME LIVE-SWAP ONLY (never
 * oracled). The DAC writes route through the g_os_port_out host hook (-> host_dac_port_out,
 * NULL in the oracle). gdv_write_scaled_palette / gdv_fade_out_palette also BUSY-WAIT on the
 * 16-bit fade accumulator [0x91dbc], which the GDV timer ISR (chained int-8, the audio path adds
 * 0xbaa @0x4e255 / the no-audio path 0x1400 @0x4e2f7) advances ~70 Hz. That ISR is frozen during a
 * lift's SIGTRAP dispatch, so fade_in/fade_out are registered INTERACTIVE — the host's shm_tick
 * surrogate pumps [0x91dbc] while they run (SIGALRM preempts the lift body, exactly like the
 * 0x90bcc frame-tick unblock for the blocking menus). The fade TIMING is therefore non-deterministic
 * (gotcha D1); the palette VALUES written are exact.
 * ============================================================================================ */

/* gdv_clear_vga_palette (0x4c392): black out DAC entries 0..[0x91dec)-1 (8-bpp only). No spin.
 * ABI: __watcall void(void); clobbers eax/ebx/edx (Watcom scratch — callers don't depend). The
 * loop is a do-while: index 0 is always cleared, then `inc bl; cmp bl,bh; jne` (bl 8-bit, so
 * bh==0 clears all 256). */
void gdv_clear_vga_palette(void)
{
    if ((uint8_t)G8(VA_g_palette_dirty) != 1) return;              /* cmp [de6],1; jne ret (8-bpp only) */
    uint8_t bh = (uint8_t)G8(VA_g_palette_dirty + 0x6);                  /* mov bh,[dec] (count boundary) */
    uint8_t bl = 0;                                     /* sub ebx,ebx */
    do {
        if (g_os_port_out) {
            g_os_port_out(0x3c8, bl);                 /* out 0x3c8, index */
            g_os_port_out(0x3c9, 0);                  /* out 0x3c9, R=0 */
            g_os_port_out(0x3c9, 0);                  /* out 0x3c9, G=0 */
            g_os_port_out(0x3c9, 0);                  /* out 0x3c9, B=0 */
        }
        bl = (uint8_t)(bl + 1);                         /* inc bl (8-bit wrap) */
    } while (bl != bh);                                 /* cmp bl,bh; jne */
}

/* gdv_settle_palette_fade (0x4d2e0): per-frame fade settler. Counts down [0x91de4]; while it stays
 * non-zero it BLANKS the palette (tail-jmp to gdv_clear_vga_palette); when it hits 0 it UPLOADS the
 * real frame palette (header[0x91d44]+0x18) via write_vga_palette 0x4c334 — a video_display-subsystem
 * function kept BRIDGED via call_orig (§6b). The bridge's `out` faults land in OBJ1 game code and are
 * serviced by the host's nested-fault path, so this works in-game (it cannot be oracled). No spin.
 * ABI: __watcall void(void); clobbers ebx/ecx/edx (scratch). Always CF=0. */
void gdv_settle_palette_fade(void)
{
    if ((uint8_t)G8(VA_g_gdv_end_of_stream + 0x6) == 0) return;              /* cmp [de4],0; je ret (clc) */
    uint8_t v = (uint8_t)((uint8_t)G8(VA_g_gdv_end_of_stream + 0x6) - 1);
    G8(VA_g_gdv_end_of_stream + 0x6) = (int8_t)v;                            /* dec [de4] */
    if (v != 0) {                                       /* jne 0x4d377 -> jmp gdv_clear_vga_palette */
        gdv_clear_vga_palette();
        return;
    }
    /* [de4] reached 0: restore the real palette via the bridged write_vga_palette */
    uint8_t *pal = (uint8_t *)(uintptr_t)((uint32_t)G32(VA_g_gdv_decode_buffer + 0x10) + 0x18);   /* edx = [d44]+0x18 */
    uint32_t count = 0x100;                             /* mov cx,0x100 */
    uint8_t cl = (uint8_t)G8(VA_g_palette_dirty + 0x6);                  /* mov cl,[dec] */
    if (cl != 0) count = cl;                            /* or cl,cl; jne -> sub ch,ch (cx=cl) */
    write_vga_palette(0, (uint32_t)(uintptr_t)pal, 0, count);  /* write_vga_palette — re-pointed */
}

/* gdv_write_scaled_palette (0x4e11a): upload all 256 DAC entries scaled by the fade level. EBX = fade
 * level (0..0x40); edi = ebx<<2 is the per-component multiplier `di` (mul di -> ah = comp*di>>8; at
 * di==0x100 that is full brightness, at 0 black). Reads the palette at header[0x91d44]+0x18.
 * COLD registry entry: only callers are the lifted gdv_fade_in/out_palette, which call this C version
 * directly (so its int3 never fires when the gdv group is swapped). ABI: __watcall, EBX in, void;
 * preserves ebx/edi/esi/edx/ecx (pushed), eax scratch.
 * BUSY-WAIT: `sub [0x91dbc],0xa00; while ((int16)[0x91dbc] < 0) {}` — paces on the fade accumulator the
 * GDV timer ISR advances; in-game the interactive surrogate pumps it (see the cluster header). */
void gdv_write_scaled_palette(uint32_t ebx)
{
    G16(VA_g_gdv_audio_stream_base + 0x6c) = (uint16_t)((uint16_t)G16(VA_g_gdv_audio_stream_base + 0x6c) - 0xa00);   /* sub word[dbc],0xa00 */
    const uint8_t *esi = (const uint8_t *)(uintptr_t)((uint32_t)G32(VA_g_gdv_decode_buffer + 0x10) + 0x18);  /* palette src */
    uint32_t di = (ebx << 2) & 0xffffu;                 /* shl ebx,2; edi=ebx; (mul di = low 16) */

    while ((int16_t)G16(VA_g_gdv_audio_stream_base + 0x6c) < 0) { /* spin */ }     /* cmp word[dbc],0; jl (timer pacing) */

    uint8_t bl = 0;                                     /* sub ebx,ebx (index) */
    do {
        uint8_t r = (uint8_t)(((uint32_t)esi[0] * di) >> 8);   /* mul di -> ah (comp*di>>8) */
        uint8_t g = (uint8_t)(((uint32_t)esi[1] * di) >> 8);
        uint8_t b = (uint8_t)(((uint32_t)esi[2] * di) >> 8);
        if (g_os_port_out) {
            g_os_port_out(0x3c8, bl);                 /* out 0x3c8, index */
            g_os_port_out(0x3c9, r);                  /* out 0x3c9, R (ecx low byte) */
            g_os_port_out(0x3c9, g);                  /* out 0x3c9, G */
            g_os_port_out(0x3c9, b);                  /* out 0x3c9, B */
        }
        esi += 3;                                       /* add esi,3 */
        bl = (uint8_t)(bl + 1);                         /* inc bl */
    } while (bl != 0);                                  /* jne (256 entries) */
}

/* gdv_fade_in_palette (0x4e08e): ramp the fade level 0 -> 0x40 by [0x91d14]+0x20 per step, calling
 * gdv_write_scaled_palette each step. 8-bpp only. INTERACTIVE (write_scaled spins). ABI void(void). */
void gdv_fade_in_palette(void)
{
    if ((uint8_t)G8(VA_g_palette_dirty) != 1) return;              /* cmp [de6],1; jne ret */
    G16(VA_g_gdv_audio_stream_base + 0x6c) = 0;                                   /* mov word[dbc],0 */
    const uint8_t *ctx = (const uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_gdv_context);
    uint16_t dx = *(const uint16_t *)(ctx + 0x20);      /* fade step */
    if (dx == 0) return;                                /* or dx,dx; je ret */
    uint16_t bx = 0;                                    /* sub ebx,ebx */
    bx = (uint16_t)(bx + dx);                           /* add bx,dx (before first call: jmp 0x4e0b8) */
    while ((int16_t)bx < 0x40) {                        /* cmp bx,0x40; jl */
        gdv_write_scaled_palette(bx);            /* ebx = bx (zero-extended) */
        bx = (uint16_t)(bx + dx);
    }
}

/* gdv_fade_out_palette (0x4e0c2): (1) settle loop — [0x91d14]+0x24 iterations of `sub [dbc],0x4600;
 * spin`; (2) ramp the fade level 0x40 -> 0 by [0x91d14]+0x22 per step via gdv_write_scaled_palette.
 * 8-bpp only. INTERACTIVE (both the settle spin and write_scaled spin). ABI void(void). */
void gdv_fade_out_palette(void)
{
    if ((uint8_t)G8(VA_g_palette_dirty) != 1) return;              /* cmp [de6],1; jne ret */
    G16(VA_g_gdv_audio_stream_base + 0x6c) = 0;                                   /* mov word[dbc],0 */
    const uint8_t *ctx = (const uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_gdv_context);

    uint16_t cx = *(const uint16_t *)(ctx + 0x24);      /* settle count */
    if (cx != 0) {                                      /* or cx,cx; je 0x4e0fa */
        do {
            G16(VA_g_gdv_audio_stream_base + 0x6c) = (uint16_t)((uint16_t)G16(VA_g_gdv_audio_stream_base + 0x6c) - 0x4600);  /* sub word[dbc],0x4600 */
            while ((int16_t)G16(VA_g_gdv_audio_stream_base + 0x6c) < 0) { /* spin */ }             /* cmp; jl */
            cx = (uint16_t)(cx - 1);                    /* dec cx */
        } while ((int16_t)cx > 0);                      /* jg (signed) */
    }

    uint16_t dx = *(const uint16_t *)(ctx + 0x22);      /* re-read [d14]+0x22: fade step */
    if (dx == 0) return;                                /* or dx,dx; je ret */
    uint16_t bx = 0x40;                                 /* mov bx,0x40 */
    bx = (uint16_t)(bx - dx);                           /* sub bx,dx (before first call: jmp 0x4e114) */
    while ((int16_t)bx >= 0) {                          /* jge (signed) */
        gdv_write_scaled_palette(bx);            /* ebx = bx */
        bx = (uint16_t)(bx - dx);
    }
}

/* ============================================================================================
 * Video-context init + display-buffer swap (sub-C leaves) — PURE (no callees, NO port-I/O), so
 * unlike the rest of the video/mode-set tier these ARE oracle-verifiable. Both take EAX = the GDV
 * context struct ptr (a runtime address = *[0x91d14]); reached from play_gdv_cutscene / the present
 * path. The two have NO callees → oracle-able if state seeded.
 * ============================================================================================ */

/* init_gdv_video_context (0x4ec70): seed the GDV context's display-geometry fields ([eax+0x19..0x74])
 * from the global video state — path A (preset LFB geometry, [0x76634]!=0) copies the configured
 * mode dims/flags + two code-embedded constants ([0x146dc] word / [0x146d8] dword = the host LFB
 * present hooks); path B (no LFB, [0x76634]==0) installs the default 320×... Mode-X/13h geometry
 * (0x140 width, page-base 0x8000/0x8001, height 0xc8 or doubled 0x190 in hi-res [0x90cbd]).
 * ABI: __watcall, EAX=ctx ptr -> void; preserves ebx/ecx/edx (pushed), EAX unmodified. PURE (reads
 * globals, writes only through the EAX struct ptr — no obj3 writes, no I/O). `jl 0x280` is SIGNED. */
void init_gdv_video_context(uint32_t eax)
{
    uint8_t *ctx = (uint8_t *)(uintptr_t)eax;
    uint32_t ebx = 0xc8;                                /* mov ebx,0xc8 */
    uint32_t edx = 0x13;                                /* mov edx,0x13 (dx default for path B's 0x90c08!=0) */
    uint32_t ecx = 0;                                   /* xor ecx,ecx */
    if ((uint16_t)G16(VA_g_init_stage_error_strings + 0x13a) != 0) ecx = 1;           /* cmp word[71f0a],0; je; mov ecx,1 */

    if ((int32_t)G32(VA_g_video_linear_flag) != 0) {                   /* cmp [76634],0; je 0x4ecfe (path B) */
        *(uint16_t *)(ctx + 0x26) = (uint16_t)G16(VA_g_video_linear_flag);
        *(uint16_t *)(ctx + 0x2a) = (uint16_t)G16(VA_g_screen_pitch);
        *(uint16_t *)(ctx + 0x2c) = (uint16_t)G16(VA_g_screen_height);
        *(uint16_t *)(ctx + 0x70) = (uint16_t)G16(VA_g_video_mode_width);   /* code-embedded present-hook const */
        *(uint8_t  *)(ctx + 0x29) = (uint8_t)G8(VA_g_vesa_page_bank_offset);
        *(uint8_t  *)(ctx + 0x28) = (uint8_t)G8(VA_g_vesa_available);
        uint32_t e = (uint32_t)G32(VA_g_linear_framebuffer_ptr);            /* code-embedded present-hook ptr/flag */
        *(uint32_t *)(ctx + 0x74) = e;
        if (e != 0) {                                   /* test edx,edx; je */
            *(uint16_t *)(ctx + 0x2e) = (uint16_t)ecx;
            ctx[0x1a] |= 0x40;
        }
        if ((int32_t)G32(VA_g_screen_pitch) >= 0x280)             /* cmp [85498],0x280; jl skip (signed) */
            ctx[0x19] |= 0x20;
        return;
    }

    /* path B (0x4ecfe): no preset LFB geometry */
    if ((uint8_t)G8(VA_g_rawscreen_flag) == 0) {                    /* cmp byte[90c08],0; jne 0x4ed1c */
        edx = 0x8000;
        if ((uint8_t)G8(VA_g_hires_line_doubling_flag) != 0) {                /* cmp byte[90cbd],0; je 0x4ed1c */
            edx = 0x8001;
            ebx += ebx;                                 /* hi-res: 0xc8 -> 0x190 */
        }
    }
    *(uint16_t *)(ctx + 0x2a) = 0x140;
    *(uint16_t *)(ctx + 0x70) = 0x140;
    *(uint16_t *)(ctx + 0x26) = (uint16_t)edx;
    *(uint16_t *)(ctx + 0x2c) = (uint16_t)ebx;
    *(uint16_t *)(ctx + 0x2e) = (uint16_t)ecx;
}

/* swap_cutscene_display_buffers (0x4ed38): flip the two double-buffer page-pointer pairs
 * ([0x71f06]<->[0x71f0a] and [0x71f04]<->[0x71f08]) — but only when single-buffering is off
 * ([0x90c08]&1 == 0) and the context's page-enable [eax+0x2e] agrees with whether a 2nd page exists
 * ([0x71f06]!=0): swap iff ([eax+0x2e]!=0) == ([0x71f06]!=0). ABI: __watcall, EAX=ctx ptr -> void;
 * preserves edx (pushed). PURE (reads [eax+0x2e] + 3 globals, writes 4 obj3 page globals; no I/O). */
void swap_cutscene_display_buffers(uint32_t eax)
{
    const uint8_t *ctx = (const uint8_t *)(uintptr_t)eax;
    if ((uint8_t)G8(VA_g_rawscreen_flag) & 1) return;               /* test byte[90c08],1; jne ret */

    int page2 = ((uint16_t)G16(VA_g_init_stage_error_strings + 0x136) != 0);
    /* [eax+0x2e]!=0: swap iff [71f06]==0 (jne ret); [eax+0x2e]==0: swap iff [71f06]!=0 (je ret). */
    int swap = (*(const uint16_t *)(ctx + 0x2e) != 0) ? !page2 : page2;
    if (!swap) return;

    uint16_t a = (uint16_t)G16(VA_g_init_stage_error_strings + 0x13a), b = (uint16_t)G16(VA_g_init_stage_error_strings + 0x136);
    G16(VA_g_init_stage_error_strings + 0x136) = (int16_t)a; G16(VA_g_init_stage_error_strings + 0x13a) = (int16_t)b;   /* swap 71f06 <-> 71f0a */
    uint16_t c = (uint16_t)G16(VA_g_init_stage_error_strings + 0x138), d = (uint16_t)G16(VA_g_init_stage_error_strings + 0x134);
    G16(VA_g_init_stage_error_strings + 0x134) = (int16_t)c; G16(VA_g_init_stage_error_strings + 0x138) = (int16_t)d;   /* swap 71f04 <-> 71f08 */
}

/* ============================================================================================
 * Container lifecycle — CLOSE path (sub-D). IN-GAME LIVE-SWAP ONLY (DOS file close + audio/timer
 * teardown; non-idempotent, never oracled). decoder_close is the live entry (reached at cutscene
 * teardown, NOT the frame loop → no publish-int3 nesting); close_input + free_decode_buffer are COLD
 * (only decoder_close calls them, via the C path).
 * ============================================================================================ */

/* gdv_free_decode_buffer (0x4b9a6): release the movie decode buffer back to the game heap. Guarded by
 * [0x91d22]&1; clears ctx[8] + [0x91d34] (the buffer base, A4) and frees it via the already-lifted
 * game_heap_free 0x15191. ABI: __watcall void(void); clobbers eax/edi (scratch). */
void gdv_free_decode_buffer(void)
{
    if (!((uint16_t)G16(VA_g_gdv_context + 0xe) & 1)) return;          /* test word[d22],1; je ret */
    G16(VA_g_gdv_context + 0xe) = (int16_t)((uint16_t)G16(VA_g_gdv_context + 0xe) - 1);   /* sub word[d22],1 */
    uint8_t *ctx = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_gdv_context);
    *(uint32_t *)(ctx + 8) = 0;                         /* mov [edi+8],0 */
    uint32_t buf = (uint32_t)G32(VA_g_gdv_decode_buffer);              /* xchg [d34],eax -> eax=old buf */
    G32(VA_g_gdv_decode_buffer) = 0;
    if (buf != 0) game_heap_free((uint8_t *)(uintptr_t)buf);   /* call game_heap_free(eax) */
}

/* gdv_close_input (0x4beb3): tear down the input/audio side — uninstall the DOS timer sync (only if the
 * audio-init flag [0x91dc2]&0x20 is set) + shut the SOS audio driver down. Both are BRIDGED via call_orig
 * (timer = DOS vector restore; audio_shutdown = host SOS driver) — they run nested in decoder_close's
 * trap and are serviced by the host's nested-fault paths (int 0x21 / audio MAGIC pages). The original's
 * tail recomputes ebp/eax from [0x91d10] but writes NO memory and its sole caller popals → omitted
 * (observationally dead). ABI: __watcall void(void). */
void gdv_close_input(void)
{
    if ((uint16_t)G16(VA_g_gdv_audio_stream_base + 0x72) & 0x20) {                /* test word[dc2],0x20; je skip */
        regs_t io; memset(&io, 0, sizeof io);
        io.va = 0x4e5dbu + OBJ_DELTA;                   /* gdv_uninstall_timer_sync */
#ifndef ROTH_STANDALONE
        call_orig(&io);
#else
        roth_unreachable(0x4e5dbu);                    /* GDV A/V-sync uninstall (PIT) — cutscene only (off --skip-gdv) */
#endif
    }
    os_audio_gdv_shutdown();                            /* 0x553b0 gdv_audio_shutdown (C2 seam) */
}

/* gdv_decoder_close (0x4b95e): the public lifecycle teardown. Records the final page-flip state into
 * ctx[0x2e] (=[0x91ce4]!=0), tears down input/audio (close_input) + frees the decode buffer
 * (free_decode_buffer), then closes the container file handle [0x91d98] via int 0x21 ah=0x3e (unless the
 * no-close stream flag [0x91d0c]&0x1000) through the g_os_soft_int host hook. pushal/popal-framed ->
 * register-transparent => ABI_VOID. Reached at cutscene teardown (3 callers outside the frame loop). */
void gdv_decoder_close(void)
{
    G8(VA_g_gdv_decoder_active) = 0;                                    /* [ddc] = 0 */
    uint8_t *ctx = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_gdv_context);
    *(uint16_t *)(ctx + 0x2e) = (uint16_t)(((int32_t)G32(VA_g_dpcm_step_table + 0x440) != 0) ? 1 : 0);  /* setne al */

    gdv_close_input();                           /* call 0x4beb3 */
    gdv_free_decode_buffer();                    /* call 0x4b9a6 */

    uint32_t handle = (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x48);           /* xchg [d98],ebx -> ebx=old handle */
    G32(VA_g_gdv_audio_stream_base + 0x48) = 0;
    if (handle != 0 && !((uint32_t)G32(VA_g_gdv_stream_flags) & 0x1000)) {   /* close the container file */
        if (g_os_soft_int) {
            regs_t io; memset(&io, 0, sizeof io);
            io.eax = 0x3e00;                            /* ah=0x3e (DOS close handle) */
            io.ebx = handle;                            /* bx = handle */
            g_os_soft_int(0x21, &io);                 /* int 0x21 */
        }
    }
}

/* ============================================================================================
 * Container lifecycle — OPEN/parse (sub-D). IN-GAME LIVE-SWAP ONLY (4 inline int 0x21 file ops →
 * g_os_soft_int; never oracled). See the format spec in
 * docs/reference/ROTH_gdv_format_notes.md §1.
 * ============================================================================================ */

extern uint16_t g_os_game_ds;   /* defined in renderer.c; the game DS captured per lift dispatch */

/* int 0x21 via the host soft-int hook (NULL in the oracle, where these lifts are never reached).
 * Returns CF (bit 0); *out_eax (if non-NULL) = the returned EAX. Mirrors lift_savegame.c's sv_int21. */
static int gdv_int21(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t *out_eax)
{
    regs_t io; memset(&io, 0, sizeof io);
    io.eax = eax; io.ebx = ebx; io.ecx = ecx; io.edx = edx;
    uint32_t fl = g_os_soft_int ? g_os_soft_int(0x21, &io) : 1u;
    if (out_eax) *out_eax = io.eax;
    return (int)(fl & 1u);
}

/* gdv_read_file_header (0x4bbb1, 558B): open (or lseek-resume) the .GDV container and parse its 0x18-byte
 * header into [0x91d44], read the 0x300 palette (ver 1) appended at +0x18 (+ duplicated at +0x300),
 * resolve dims via the 0x72770 {code,w,h} stride-6 table when height==0, then derive the audio block sizes
 * (d30/d58/d2c/d28/dca/dc8/dbe/d9c — same math as the codec harness's gdv_calc_d28), the pixel format
 * [0x91de6]/bpp [0x91cc8], the decode-buffer byte size [0x91d94] (= w*h*bpp, &~3), the back-ref guard
 * [0x91cc4] (= 0x1000*bpp), and the context dims ctx[0x60]/[0x62]. No subsystem callees — only the 4
 * int 0x21 ops (0x4200 lseek / 0x3d00 open / 0x3f read ×{header,palette}). ABI: __watcall void -> CF
 * (set=fail) + EBP (error code on fail: 0x20 open / 0x21 read / 0x30 magic/dims). The `mov [0x4e609],ds`
 * captures the game DS for the timer ISR's `mov ds,cs:[0x4e609]` — reproduced via g_os_game_ds. */
uint32_t gdv_read_file_header(uint32_t *ebp_out)
{
    uint8_t *ctx = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_gdv_context);
    uint32_t ebp = 0;

    if (*(uint32_t *)(ctx + 0x18) & 0x1000) {           /* resume: lseek the saved handle */
        uint32_t ofs = *(uint32_t *)(ctx + 0x5c);
        G32(VA_g_dpcm_step_table + 0x450) = (int32_t)ofs;
        uint32_t handle = *(uint32_t *)(ctx + 0x58);
        G32(VA_g_gdv_audio_stream_base + 0x48) = (int32_t)handle;
        if (gdv_int21(0x4200, handle, ofs >> 16, ofs & 0xffff, NULL)) { *ebp_out = ebp; return 1; }
    } else {                                            /* open the container by name */
        G32(VA_g_dpcm_step_table + 0x450) = 0;
        ebp = 0x20;
        uint32_t fname = *(uint32_t *)(ctx + 0);
        if (fname == 0) { *ebp_out = ebp; return 1; }
        uint32_t eax = 0;
        if (gdv_int21(0x3d00, 0, 0, fname, &eax)) { *ebp_out = ebp; return 1; }
        G32(VA_g_gdv_audio_stream_base + 0x48) = (int32_t)eax;                    /* handle */
    }

    G16(VA_g_gdv_saved_ds_selector) = (int16_t)g_os_game_ds;             /* mov word[0x4e609],ds (timer ISR DS) */
    uint32_t handle = (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x48);
    uint8_t *esi = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_gdv_decode_buffer + 0x10);   /* header buffer */
    ebp = 0x21;
    uint32_t nread = 0;
    if (gdv_int21(0x3f00, handle, 0x18, (uint32_t)(uintptr_t)esi, &nread)) { *ebp_out = ebp; return 1; }
    if (nread != 0x18) { *ebp_out = ebp; return 1; }    /* cmp eax,ecx; jne */
    G32(VA_g_dpcm_step_table + 0x450) += 0x18;
    ebp = 0x30;
    if (*(uint32_t *)esi != 0x29111994u) { *ebp_out = ebp; return 1; }   /* magic */

    if (*(uint16_t *)(esi + 0xe) == 1) {                /* ver 1 -> 0x300 palette follows */
        uint8_t *pal = esi + 0x18;
        ebp = 0x21;
        G32(VA_g_dpcm_step_table + 0x450) += 0x300;
        uint32_t pn = 0;
        if (gdv_int21(0x3f00, handle, 0x300, (uint32_t)(uintptr_t)pal, &pn)) { *ebp_out = ebp; return 1; }
        if (pn != 0x300) { *ebp_out = ebp; return 1; }
        memcpy(pal + 0x300, pal, 0x300);                /* rep movsd ecx=0xc0: pal -> pal+0x300 */
    }

    G32(VA_g_dpcm_step_table + 0x410) = G32(VA_g_dpcm_step_table + 0x450);

    if (*(uint16_t *)(esi + 0x16) == 0) {               /* height 0 -> dims-table lookup */
        uint16_t code = *(uint16_t *)(esi + 4);
        const uint16_t *tbl = (const uint16_t *)GADDR(VA_g_rng_state + 0x4);
        for (;;) {
            if (*tbl == 0xffff) { *ebp_out = ebp; return 1; }   /* table end -> fail (ebp=0x30) */
            if (*tbl == code) break;
            tbl += 3;                                   /* add ebx,6 ({code,w,h}) */
        }
        *(uint16_t *)(esi + 0x14) = tbl[1];             /* width */
        *(uint16_t *)(esi + 0x16) = tbl[2];             /* height */
    }

    /* audio block sizes (== gdv_calc_d28 math) */
    uint32_t a = 0;
    if (*(uint16_t *)(esi + 0xa) != 0) {                /* aflg != 0 */
        a = *(uint16_t *)(esi + 0xc);                   /* arate */
        uint16_t adiv = *(uint16_t *)(esi + 8);
        if (adiv != 0) a = (uint16_t)(a / adiv);        /* 16-bit divide */
    }
    G32(VA_g_gdv_audio_buf_size + 0x4) = (int32_t)a;
    G32(VA_g_gdv_audio_stream_base + 0x8) = (int32_t)a;
    if (a == 0) G32(VA_g_gdv_audio_stream_base + 0x8) = (int32_t)(0x5622u / 0xcu);   /* special default rate */
    uint8_t dl = (uint8_t)*(uint16_t *)(esi + 0xa);     /* aflg low byte */
    uint8_t dh = 0;
    if (dl & 2) { a += a; dh |= 2; }
    if (dl & 4) { dh |= 4; a += a; }
    G32(VA_g_gdv_audio_buf_size) = (int32_t)a;
    if (dl & 8) { dh |= 8; a >>= 1; }
    G32(VA_g_gdv_context + 0x14) = (int32_t)a;
    G8(VA_g_gdv_audio_format) = (int8_t)dh;
    if (a == 0) G8(VA_g_gdv_audio_enabled) = 0;
    G16(VA_g_gdv_audio_stream_base + 0x6e) = (int16_t)((uint16_t)((uint32_t)esi[8] << 8));   /* ah=byte[esi+8] */
    uint32_t d9c = (uint16_t)*(uint16_t *)(esi + 0x10);
    d9c += (uint32_t)G32(VA_g_gdv_context + 0x14);
    d9c = (d9c + 0xb) & 0xfffffffcu;                    /* add 0xb; and al,0xfc (=&~3) */
    G32(VA_g_gdv_audio_stream_base + 0x4c) = (int32_t)d9c;

    /* pixel format + decode-buffer size */
    uint8_t al = (uint8_t)*(uint16_t *)(esi + 0xe);     /* version (1=8bpp / 2=hicolour) */
    G8(VA_g_palette_dirty) = (int8_t)al;
    if (al > 2) al--;                                   /* cmp al,2; jbe; dec al */
    G32(VA_g_dpcm_step_table + 0x424) = al;                                  /* bytes-per-pixel */
    uint32_t w = *(uint16_t *)(esi + 0x14);
    uint32_t bufsz = (uint32_t)(*(uint16_t *)(esi + 0x16)) * w;   /* h*w */
    bufsz *= (uint32_t)G32(VA_g_dpcm_step_table + 0x424);                    /* *bpp */
    bufsz = (bufsz + 3) & 0xfffffffcu;
    G32(VA_g_gdv_audio_stream_base + 0x44) = (int32_t)bufsz;
    G32(VA_g_dpcm_step_table + 0x420) = (int32_t)(0x1000u * (uint32_t)G32(VA_g_dpcm_step_table + 0x424));   /* back-ref guard */
    uint8_t *ctx2 = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_gdv_context);
    *(uint16_t *)(ctx2 + 0x60) = *(uint16_t *)(esi + 0x14);
    *(uint16_t *)(ctx2 + 0x62) = *(uint16_t *)(esi + 0x16);
    return 0;                                           /* clc */
}

/* Read `cnt` bytes from `handle` into `buf` via int 0x21 0x3f, with the original's retry loop: on a
 * short/failed read, decrement the retry counter [0x91cfc] (give up + fail when it goes negative),
 * lseek (0x4200) back to the saved position [0x91cf8], and re-read. The caller resets [0x91cfc]=8 and
 * [0x91cf8] before each use. Advances [0x91cf4] by `cnt` on each attempt (as the original does). */
static int gdv_read_retry(uint32_t handle, uint32_t cnt, uint8_t *buf)
{
    for (;;) {
        G32(VA_g_dpcm_step_table + 0x450) += (int32_t)cnt;
        uint32_t nread = 0;
        int cf = gdv_int21(0x3f00, handle, cnt, (uint32_t)(uintptr_t)buf, &nread);
        if (!cf && nread == cnt) return 0;              /* full read */
        G8(VA_g_dpcm_step_table + 0x458) = (int8_t)((uint8_t)G8(VA_g_dpcm_step_table + 0x458) - 1);
        if ((int8_t)G8(VA_g_dpcm_step_table + 0x458) < 0) return 1;          /* sub [cfc],1; js -> error */
        uint32_t pos = (uint32_t)G32(VA_g_dpcm_step_table + 0x454);
        G32(VA_g_dpcm_step_table + 0x450) = (int32_t)pos;
        gdv_int21(0x4200, handle, pos >> 16, pos & 0xffff, NULL);   /* lseek back */
    }
}

/* gdv_read_frame_chunk (0x4c3ba, 483B): fetch the next frame chunk into the streaming ring slot
 * [0x91d60], then advance the ring cursor. Two modes: [0x91def]==2 = the chunk is ALREADY buffered in
 * the ring (no file I/O — preloaded); else READ it from the container via two int 0x21 0x3f reads (the
 * audio sub-block + 8-byte header, then the payload at +8), each with the gdv_read_retry lseek-back loop.
 * Validates the chunk magic 0x1305 + payload-size bound (<= header max-chunk [hdr+0x10]) + merges the
 * pending keyframe bit [0x91dd6]&0x40 into the chunk flags [slot+4]. Tail: advance [0x91d60] past the
 * record (rounded to 8, wrapping to [0x91d50] when it won't fit before [0x91d5c]) + the audio-pacing
 * spin (when [0x91880]!=0: [0x91884] -= bytes; wait until >=0 — [0x91884] is advanced by the GDV timer
 * ISR, frozen during a lift, so this fn is registered INTERACTIVE and the host shm_tick surrogate pumps
 * it). ABI: __watcall void -> CF (set=fail) + EBP (err 0x22/0x23 read, 0x31 magic, 0x32 size) + ECX
 * (bytes read, for the caller's backpressure). LIVE-SWAP ONLY (int 0x21 file I/O). */
uint32_t gdv_read_frame_chunk(uint32_t *ebp_out, uint32_t *ecx_out)
{
    const uint8_t *hdr = (const uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_gdv_decode_buffer + 0x10);
    uint32_t ecx;

    if ((uint8_t)G8(VA_g_palette_dirty + 0x9) == 2) {                    /* mode 2: already in the ring */
        uint8_t *esi = (uint8_t *)(uintptr_t)((uint32_t)G32(VA_g_gdv_audio_stream_base + 0x10) + (uint32_t)G32(VA_g_gdv_context + 0x14));
        ecx = *(const uint16_t *)(hdr + 0x10);          /* max chunk size */
        if (ecx != 0) {
            if (*(uint16_t *)esi != 0x1305) { *ebp_out = 0x31; return 1; }
            uint8_t kf = (uint8_t)G8(VA_g_gdv_audio_format + 0xc) & 0x40; G8(VA_g_gdv_audio_format + 0xc) = 0; esi[4] |= kf;
            uint32_t paysz = *(uint16_t *)(esi + 2);
            if (paysz > ecx) { *ebp_out = 0x32; return 1; }
            if (paysz != 0) ecx = paysz;
        }
    } else {                                            /* read from the container */
        uint32_t handle = (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x48);
        uint8_t *edi = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_gdv_audio_stream_base + 0x10);
        G32(VA_g_dpcm_step_table + 0x454) = G32(VA_g_dpcm_step_table + 0x450);                    /* save file pos */
        G8(VA_g_dpcm_step_table + 0x458) = 8;                                /* retries */
        G32(VA_g_particle_pool + 0x20) = 0;
        if (gdv_read_retry(handle, (uint32_t)G32(VA_g_gdv_context + 0x14) + 8, edi)) { *ebp_out = 0x22; return 1; }
        G32(VA_g_dpcm_step_table + 0x454) = G32(VA_g_dpcm_step_table + 0x450);
        G8(VA_g_dpcm_step_table + 0x458) = 8;
        ecx = *(const uint16_t *)((const uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_gdv_decode_buffer + 0x10) + 0x10);
        if (ecx != 0) {
            uint8_t *esi = edi + (uint32_t)G32(VA_g_gdv_context + 0x14);
            if (*(uint16_t *)esi != 0x1305) { *ebp_out = 0x31; return 1; }
            uint8_t kf = (uint8_t)G8(VA_g_gdv_audio_format + 0xc) & 0x40; G8(VA_g_gdv_audio_format + 0xc) = 0; esi[4] |= kf;
            uint32_t paysz = *(uint16_t *)(esi + 2);
            if (paysz > ecx) { *ebp_out = 0x32; return 1; }
            if (paysz != 0) {
                ecx = paysz;
                if (gdv_read_retry(handle, ecx, esi + 8)) { *ebp_out = 0x23; return 1; }
            }
        }
    }

    /* tail: advance the chunk-ring cursor [0x91d60] past this record */
    uint32_t eax = (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x10) + (uint32_t)G32(VA_g_gdv_context + 0x14);
    eax += *(const uint16_t *)(uintptr_t)(eax + 2);     /* + payload size */
    eax = (eax + 0xf) & 0xfffffff8u;                    /* round to 8 (add 0xf; and al,0xf8) */
    if (eax + (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x4c) > (uint32_t)G32(VA_g_gdv_audio_stream_base + 0xc))   /* won't fit -> wrap */
        eax = (uint32_t)G32(VA_g_gdv_audio_stream_base);
    G32(VA_g_gdv_audio_stream_base + 0x10) = (int32_t)eax;

    if ((int32_t)G32(VA_g_particle_pool + 0x1c) != 0) {                   /* audio-pacing budget active */
        G32(VA_g_particle_pool + 0x20) -= (int32_t)ecx;
        while ((int32_t)G32(VA_g_particle_pool + 0x20) < 0) { /* spin: surrogate pumps [0x91884] += [0x91880] */ }
    }
    *ebp_out = 0;                                       /* sub ebp,ebp */
    *ecx_out = ecx;
    return 0;                                           /* clc */
}

/* gdv_decoder_open (0x4b710, 433B): the cutscene OPEN orchestrator. Zeroes the decoder state, copies the
 * caller's GDV context fields into the [0x91dxx] globals, then runs the open sequence — alloc decode buffer
 * (bridge 0x4b9d4, pool path), read_file_header [lifted], setup_decode_buffers [lifted], init_audio_output
 * (bridge 0x4bddf), optional audio_init_silence_buffer (bridge host 0x4e4f1), the byte-rate budget
 * [0x91880]=([0x91880]<<0xd)/[0x9187c], init_pixel_tables [lifted], and preload_frame_window (bridge
 * 0x4dddd, unless [0x91d0c]&4) — each callee short-circuits to the error return (EAX=its EBP error code) on
 * CF. The three lifted callees are called as C directly (their int3 stays cold while this is swapped); the
 * un-lifted/host callees are bridged via call_orig (alloc + preload need ESI=ctx; serviced nested by the
 * host int-0x21/audio-MAGIC fault paths). ABI: __watcall EAX=ctx ptr -> EAX = result (0=success, else the
 * error code) — both callers `test eax,eax` (ABI_EAX). Preserves edi/esi/ebp/ebx/ecx/edx (pushal-style).
 * Reached at cutscene open (2 callers, top-level — no frame-loop publish nesting). LIVE-SWAP ONLY. */
uint32_t gdv_decoder_open(uint32_t eax)
{
    G8(VA_g_palette_dirty + 0xa) = 0; G8(VA_g_palette_dirty + 0x9) = 0; G16(VA_g_gdv_audio_stream_base + 0x72) = 0; G8(VA_g_gdv_end_of_stream) = 0;
    G16(VA_g_gdv_audio_format + 0xa) = 0; G32(VA_g_gdv_audio_stream_base + 0x4) = 0; G8(VA_g_gdv_end_of_stream + 0x4) = 0; G16(VA_g_gdv_audio_stream_base + 0x64) = 0;
    G8(VA_g_palette_dirty + 0x6) = 0; G8(VA_g_palette_dirty + 0x5) = 0; G8(VA_g_sos_timer_event_count + 0x8) = 0; G32(VA_g_dpcm_step_table + 0x414) = 0; G8(VA_g_gdv_decoder_active) = 0;

    uint8_t *ctx = (uint8_t *)(uintptr_t)eax;
    G32(VA_g_gdv_user_callback + 0x4) = *(int32_t *)(ctx + 0x14);
    G32(VA_g_gdv_user_callback + 0x8) = *(int32_t *)(ctx + 0x10);
    G32(VA_g_gdv_user_callback) = *(int32_t *)(ctx + 0x1c);
    if (*(uint32_t *)(ctx + 4) == 0)
        *(uint32_t *)(ctx + 4) = 0x91dc6u + OBJ_DELTA;  /* default ptr (canon 0x91dc6, rebased) */
    G8(VA_g_gdv_audio_enabled) = 1;
    uint32_t flags = *(uint32_t *)(ctx + 0x18);
    if (flags & 0x10) G8(VA_g_gdv_audio_enabled) = 0;                  /* audio-off */
    G16(VA_g_gdv_audio_format + 0x4) = 0x13;
    G32(VA_g_gdv_context + 0x10) = 0;
    G32(VA_g_gdv_context) = (int32_t)eax;                        /* the context */
    G32(VA_g_gdv_context + 0x4) = *(int32_t *)(ctx + 0x54);
    G32(VA_g_gdv_stream_flags) = (int32_t)flags;                      /* stream flags */
    G32(VA_g_dpcm_step_table + 0x40c) = *(int32_t *)(ctx + 0x64);
    if (flags & 0x2000)  G8(VA_g_gdv_end_of_stream + 0x4) |= 1;
    if (flags & 0x10000) G8(VA_g_gdv_end_of_stream + 0x4) |= 2;
    G8(VA_g_gdv_end_of_stream + 0x5) = G8(VA_g_gdv_end_of_stream + 0x4);

    uint32_t ebp = 0;

    /* alloc decode buffer [lifted; was a call_orig bridge — now direct C. reads g_gdv_context from the
     * global (set just above), so the old eax/esi=ctx inputs were vestigial. returns CF + EBP err code. */
    if (gdv_alloc_decode_buffer(&ebp)) return ebp;   /* call 0x4b9d4 */

    if (gdv_read_file_header(&ebp))    return ebp;   /* call 0x4bbb1 [lifted] */
    if (gdv_setup_decode_buffers(&ebp)) return ebp;  /* call 0x4ba30 [lifted] */

    /* init audio output [lifted; was a call_orig bridge — now direct C. reads g_gdv_context from the
     * global, so the old eax/esi=ctx inputs were vestigial. returns CF + (on error) EBP error code. */
    if (gdv_init_audio_output(&ebp)) return ebp;   /* call 0x4bddf */

    if ((uint8_t)G8(VA_g_palette_dirty + 0xc) != 0)                      /* 0x4e4f1 gdv_audio_init_silence (C2 seam) */
        os_audio_gdv_init_silence();
    if ((int32_t)G32(VA_g_particle_pool + 0x18) != 0)                     /* byte-rate budget */
        G32(VA_g_particle_pool + 0x1c) = (int32_t)(((uint32_t)G32(VA_g_particle_pool + 0x1c) << 0xd) / (uint32_t)G32(VA_g_particle_pool + 0x18));

    gdv_init_pixel_tables();                     /* call 0x4bef0 [lifted] */

    if (!((uint32_t)G32(VA_g_gdv_stream_flags) & 4)) {                /* preload first frame window (lifted C — was bridge) */
        uint32_t ebp_v = 0;
        if (gdv_preload_frame_window(&ebp_v)) return ebp_v;   /* CF -> propagate read error code */
    }

    G8(VA_g_gdv_decoder_active) = (int8_t)0xff;
    uint8_t *edi = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_gdv_context);
    *(uint32_t *)(edi + 0x44) = (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x4c);
    *(uint32_t *)(edi + 0x4c) = (uint32_t)G32(VA_g_gdv_context + 0x14);
    *(uint32_t *)(edi + 0x48) = (uint16_t)G16(VA_g_gdv_audio_stream_base + 0x66);
    return 0;                                           /* success */
}

/* gdv_preload_frame_window (0x4dddd, 157B) — PURE read-ahead: fill the frame ring from the container
 * (read_frame_chunk in a loop) until the ring would overflow ([d60]+[d9c] > [d5c]), the read head wraps
 * back to the base ([d60]==[d50]), or all [0x91878] frames are buffered. No spin on the decoder head and no
 * decode pump — so it never hangs (unlike the playback loops). Called only by decoder_open (open time, no
 * audio spin yet). Sole subsystem callee = read_frame_chunk [lifted]. Returns CF (set=read error) + EBP
 * (read_frame_chunk's error code on fail). Tail seeds the frame counters [db8]/[dba] = [0x91878]-2 and the
 * decode head [d6c] = ring base. ABI_CF_EBP. Faithful goto-label transcription. LIVE-SWAP ONLY. */
uint32_t gdv_preload_frame_window(uint32_t *ebp_out)
{
    G8(VA_g_gdv_audio_end) = 0;                                    /* 4dddd */
    G32(VA_g_gdv_audio_stream_base + 0x10) = G32(VA_g_gdv_audio_stream_base);                        /* 4dde4: [d60] = [d50] (ring base) */
    int32_t ecx = (int32_t)G32(VA_g_particle_pool + 0x14);               /* 4ddee: ecx = [0x91878] */
L4ddf4:
    if ((uint32_t)G32(VA_g_gdv_audio_stream_base + 0x10) + (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x4c) > (uint32_t)G32(VA_g_gdv_audio_stream_base + 0xc))
        goto L4de5a;                                    /* 4ddf4..4de05: ja (ring would overflow -> done) */
    G32(VA_g_gdv_audio_stream_base + 0x14) = G32(VA_g_gdv_audio_stream_base + 0x10);                        /* 4de08/4de0d: [d64] = [d60] (save read pos) */
    {
        uint32_t ebp_v, ecx_v;
        if (gdv_read_frame_chunk(&ebp_v, &ecx_v)) { *ebp_out = ebp_v; return 1; }  /* 4de12: jb err */
    }
    G32(VA_g_dpcm_step_table + 0x414) = (int32_t)((uint32_t)G32(VA_g_dpcm_step_table + 0x414) + 1u);             /* 4de1a: inc [0x91cb8] */
    if ((uint32_t)G32(VA_g_gdv_audio_stream_base + 0x10) == (uint32_t)G32(VA_g_gdv_audio_stream_base)) goto L4de5a; /* 4de20..4de2b: je (wrapped -> full) */
    ecx--;                                              /* 4de2d: dec ecx */
    if (ecx > 0) goto L4ddf4;                           /* 4de2e: jg */
    /* 0x4de30: ran out of frames to preload before the ring filled */
    if ((uint8_t)G8(VA_g_palette_dirty + 0x9) == 0) goto L4de5a;         /* 4de30: je */
    G8(VA_g_palette_dirty + 0x9) = 2;                                    /* 4de39: mark fully-buffered */
    G32(VA_g_gdv_audio_stream_base + 0xc) = (int32_t)((uint32_t)G32(VA_g_gdv_audio_stream_base + 0x14) + (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x4c));  /* 4de40..4de4b: ring limit */
    G32(VA_g_gdv_audio_stream_base + 0x10) = G32(VA_g_gdv_audio_stream_base);                        /* 4de50: reset read head to base */
L4de5a:
    {
        uint16_t ax = (uint16_t)((uint32_t)G32(VA_g_particle_pool + 0x14) - 2u);  /* 4de5a/4de5f */
        G16(VA_g_gdv_audio_stream_base + 0x68) = ax;                              /* 4de62: [db8] = [0x91878]-2 */
        G16(VA_g_gdv_audio_stream_base + 0x6a) = ax;                              /* 4de68: [dba] = [0x91878]-2 */
    }
    G32(VA_g_gdv_audio_stream_base + 0x1c) = G32(VA_g_gdv_audio_stream_base);                        /* 4de6e/4de73: decode head = ring base */
    return 0;                                           /* 4de78: clc (CF=0) */
}

/* gdv_emit_decoded_frame (0x4dcfc) — present/loop KEYSTONE. Called once per decoded frame from the
 * (original-bytes) playback loop. The decode buffer DAT_91d40 holds a COMPLETE frame here, so this is
 * exactly where the host snapshots it: the host normally does that from the int3 it plants at this entry
 * (GDV_EMIT_SITE) and then runs the original body. Lifting emit moves that publish INTO the lift (via
 * g_os_publish_frame; the host gates its own publish-only branch off through g_gdv_emit_lifted), then
 * reproduces the original control flow exactly:
 *
 *   4dcfc cmp [g_gdv_user_callback 0x91d00],0; je blit         ; no callback (full-screen) -> blit
 *   4dd05 mov eax,0; call gdv_invoke_user_callback; test al,1; jne post   ; popup: pre-callback(0)
 *   4dd13 call gdv_blit_frame_to_vga_alt 0x4c788                ; (blit) host_video_driver
 *   4dd18 cmp [0x91d00],0; je dec                               ; full-screen skips the finalize callback
 *   4dd21 mov eax,1; call gdv_invoke_user_callback               ; post: finalize-callback(1)
 *   4dd2b dec [0x91cb8]; clc; ret                                ; frame counter
 *
 * The blitter 0x4c788 is host_video_driver (its Mode-X VGA output is thrown away — the host presents from
 * DAT_91d40), but it is BRIDGED, not skipped: it carries engine-visible side-effects (sets de4=1/de5=0 and
 * does the de5-gated 256-colour palette copy header[0x91d44]+0x318 -> +0x18 that write_vga_palette later
 * reads). Its `out 0x3c4/0x3c5` + INT10h AX=4F05 fault in OBJ1 and are serviced by the host's nested-fault
 * path. emit passes NO input registers to it (the blitter reads its state from globals), so a zeroed
 * regs_t matches. invoke_user_callback 0x4dd71 (the inspect-popup path only; g_gdv_user_callback==0 for
 * full-screen cutscenes, so it never runs there) is likewise bridged with just the op in EAX.
 * ABI_VOID; LIVE-SWAP ONLY (publishes a frame; cannot be oracled). */
void gdv_emit_decoded_frame(void)
{
    if (g_os_publish_frame) g_os_publish_frame();   /* publish the complete frame (was the entry int3) */

    regs_t io;
    if ((uint32_t)G32(VA_g_gdv_user_callback) != 0) {                  /* cmp [g_gdv_user_callback],0; jne (callback set) */
        if (gdv_invoke_user_callback(0) & 1u) goto post;  /* pre-callback; al&1 -> skip blit — re-pointed */
    }
    /* blit (full-screen, or popup pre-callback said "not presented") */
    memset(&io, 0, sizeof io);
    io.va = 0x4c788u + OBJ_DELTA;                        /* gdv_blit_frame_to_vga_alt (bridge; side-effects) */
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    gdv_blit_frame_to_vga_alt();  /* image-free native (host_video_driver); side-effects only, pixel blit skipped (host presents [0x91d40]) */
#endif
    if ((uint32_t)G32(VA_g_gdv_user_callback) == 0) goto dec;          /* cmp [0x91d00],0; je -> full-screen skips finalize */
post:
    gdv_invoke_user_callback(1);                 /* finalize-callback — re-pointed */
dec:
    G32(VA_g_dpcm_step_table + 0x414) = (uint32_t)G32(VA_g_dpcm_step_table + 0x414) - 1;          /* dec [0x91cb8] frame counter (clc; ret) */
}

#ifdef ROTH_STANDALONE
/* the inspect frame callback (0x18e09) — a MID-FUNCTION entry into the 0x18cb9 appendage region,
 * installed into gdvctx+0x1c by load_dbase300_resource_at_offset (lift_file_config.c:928) for the
 * inspect close-up popup's GDV loop. Action dispatcher (EAX=action, EBX=info record for action 0);
 * transcribed from disasm 0x18cf4..0x18e2b — all five callees already lifted:
 *   0 -> arm the one-shot info latch ([0x810f4]=1, [0x810f8]=EBX) if EBX!=0 and not armed; return 1
 *   1 -> return 0;   3 -> return 3 (18cf4);   4 -> return 1 (18e03);   >4 -> return 0
 *   2 (frame boundary, 18cfa): voice sample [0x7fec4] gone -> arm the exit latch [0x7fecc];
 *      latched -> [0x810c0]=0, return 2 (EOS; the invoke's al&2 latches [0x91dde]).
 *      [0x810c0]==2 -> return 2 (the store at 18d16 is SKIPPED — jmp lands on 18d20).
 *      armed info tick (18d2f): blit_saved_ui_block; tick delta [0x85324] = shm tick [0x90bcc] -
 *      [0x85320] (movswl both), decl [0x810f4], re-store the tick; if the delta is nonzero and
 *      update_inspect_popup_choices()!=0 -> flip_video_page(0) (18d76: xor eax; call 0x2e1e8).
 *      key poll (18d7d): [0x80b2c]==0 -> input_ring_dequeue (the cwtl of the zero-extended AL);
 *      Esc 0x01 / 'I' 0x17 -> try_interrupt_dialogue_voice: returns 0 -> [0x7fecc]=1, return 2
 *      (jmp 18d20, the [0x810c0] store NOT taken); Enter 0x1c -> try_interrupt only, return 0;
 *      Space 0x39 -> [0x810c0] 1->2 return 0, else the Esc path; anything else -> return 0. */
/* NON-STATIC: the same [0x91d00] dispatch exists in TWO consumers — gdv_invoke_user_callback
 * (below) AND write_vga_palette (lift_video_display.c, the DAC-upload path the inspect popup
 * actually hits FIRST — coredump-proven, frame #5). Both route here. */
uint32_t gdv_inspect_frame_callback(uint32_t action, uint32_t ebx)
{
    switch (action) {
    case 0:                                             /* 18de6 */
        if (ebx != 0 && G32(VA_g_inspect_popup_state + 0x34) == 0) {
            G32(VA_g_inspect_popup_state + 0x34) = 1;                           /* one-shot info latch */
            G32(VA_g_inspect_popup_state + 0x38) = (int32_t)ebx;
        }
        /* fall through */
    case 4:
        return 1;                                       /* 18e03: mov eax,1; ret */
    case 3:
        return 3;                                       /* 18cf4 */
    case 2:
        break;                                          /* 18cfa below */
    default:
        return 0;                                       /* action 1 / >4: xor eax,eax; ret */
    }
    if (G32(VA_g_inventory_panel_open) == 0)                              /* 18cfa: the voice sample is gone */
        G32(VA_g_inventory_panel_open + 0x8) = 1;                               /*   -> arm the exit latch */
    if (G32(VA_g_inventory_panel_open + 0x8) != 0) {                            /* 18d0d */
        G32(VA_g_inspect_popup_state) = 0;                               /* 18d16 */
        return 2;                                       /* 18d20 */
    }
    if (G32(VA_g_inspect_popup_state) == 2)                              /* 18d26: je 18d20 (store skipped) */
        return 2;
    if (G32(VA_g_inspect_popup_state + 0x34) != 0) {                            /* 18d2f: the armed info tick */
        blit_saved_ui_block();                   /* call 0x18a64 */
        int32_t now  = (int16_t)G16(VA_g_frame_tick_counter);           /* movswl the shm tick */
        int32_t last = (int16_t)G16(VA_g_last_frame_tick);
        G32(VA_g_frame_time_scale) = now - last;                      /* tick delta */
        G32(VA_g_inspect_popup_state + 0x34) = G32(VA_g_inspect_popup_state + 0x34) - 1;                /* decl (18d58) */
        G16(VA_g_last_frame_tick) = (int16_t)(uint16_t)G16(VA_g_frame_tick_counter); /* re-read + store (18d52/18d5e) */
        if (G32(VA_g_frame_time_scale) != 0) {
            if (update_inspect_popup_choices() != 0)   /* call 0x18ada */
                host_flip_video_page(0);                /* 18d76: xor eax,eax; call 0x2e1e8 */
        }
    }
    uint32_t key = 0;                                   /* 18d7d: xor eax,eax */
    if (G32(VA_g_ui_panel_anchor_y + 0x4) == 0)
        key = (uint32_t)input_ring_dequeue();    /* call 0x1299a; cwtl (AL zero-extended) */
    if (key < 0x17u) {                                  /* 18d8e: jb 18dde */
        if (key != 1u) return 0;                        /* only Esc falls to 18dc6 */
    } else if (key == 0x17u) {                          /* 'I' -> 18dc6 */
        ;
    } else if (key < 0x1cu) {
        return 0;                                       /* 18d98: jb 18e29 */
    } else if (key == 0x1cu) {                          /* Enter: 18da8 */
        try_interrupt_dialogue_voice();          /* call 0x18a2a */
        return 0;
    } else if (key == 0x39u) {                          /* Space: 18db0 */
        if (G32(VA_g_inspect_popup_state) == 1) {
            G32(VA_g_inspect_popup_state) = 2;
            return 0;                                   /* 18dc3 */
        }
        /* else -> 18dc6 */
    } else {
        return 0;                                       /* 18da5 */
    }
    if (try_interrupt_dialogue_voice() != 0)     /* 18dc6: call 0x18a2a */
        return 0;                                       /* jne 18e29 */
    G32(VA_g_inventory_panel_open + 0x8) = 1;                                   /* 18dcf: arm the exit latch */
    return 2;                                           /* jmp 18d20 (eax=2; the 18d16 store NOT taken) */
}
#endif /* ROTH_STANDALONE */

/* gdv_invoke_user_callback (0x4dd71): set up the per-frame user-callback args from the GDV decode
 * state and invoke the registered callback [0x91d00](eax=action, ebx=buf, ecx=halfres<<16|[0x91dd4],
 * edx=header). If the callback returns al&2, latch end-of-stream [0x91dde]=0xff. ABI_EAX (action in
 * -> callback's eax out). The callback itself (inspect-popup / fullscreen draw) is GAME code with
 * video side-effects, so the indirect call is BRIDGED via call_orig at the runtime target [0x91d00]
 * (already rebased); LIVE-SWAP ONLY (cannot be oracled — the callback does port I/O). Reached in-game
 * via callback_frame_boundary's `call 0x4dd71` (and present_first_frame) with the int3 armed. */
uint32_t gdv_invoke_user_callback(uint32_t action)
{
    if ((uint8_t)G8(VA_g_gdv_end_of_stream) == 0xff)                   /* cmp [0x91dde],0xff; je ret (EOS: skip) */
        return action;                                  /* eax unchanged */
    uint32_t ebx = (uint32_t)G32(VA_g_gdv_decode_buffer + 0xc);              /* mov ebx,[0x91d40] (decode buffer) */
    uint8_t cl = (uint8_t)G8(VA_g_gdv_audio_stream_base + 0x70);                  /* mov cl,[0x91dc0] (format byte) */
    if ((cl & 0x80) || (cl & 0xf) == 0 || (cl & 0xf) == 3)
        ebx = 0;                                        /* fmt &0x80 / lo==0 / lo==3 -> no buffer ptr */
    uint32_t ecx = 0;                                   /* sub ecx,ecx */
    uint8_t de2 = (uint8_t)G8(VA_g_gdv_end_of_stream + 0x4);                 /* half-res flags */
    if (de2) {                                          /* or dl,dl; je 0x4ddbb */
        uint8_t bits = 0;
        if (de2 & 5) bits = 1;                          /* test dl,5  -> cl=1  (horizontal half) */
        if (de2 & 0xc) bits |= 2;                       /* test dl,0xc-> cl|=2 (vertical half) */
        ecx = (uint32_t)bits << 0x10;                   /* shl ecx,0x10 */
    }
    uint32_t edx = (uint32_t)G32(VA_g_gdv_decode_buffer + 0x10);              /* mov edx,[0x91d44] (header) */
    ecx = (ecx & 0xffff0000u) | (uint16_t)G16(VA_g_gdv_audio_format + 0xa); /* mov cx,[0x91dd4] */
    regs_t io; memset(&io, 0, sizeof io);
    io.va = (uint32_t)G32(VA_g_gdv_user_callback);                     /* call [0x91d00] (runtime ptr, already rebased) */
    io.eax = action; io.ebx = ebx; io.ecx = ecx; io.edx = edx;
#ifndef ROTH_STANDALONE
    call_orig(&io);                                     /* user callback(action, buf, halfres|dd4, hdr) */
#else
    /* the [0x91d00] installs are enumerable — 0x1fce2 run_timed_message_sequence (gdv 0x20662;
     * lifted, EAX=mode,EDX,EBX,ECX -> EAX) and 0x18e09 (the inspect frame callback, a MID-FUNCTION
     * entry into the 0x18cb9 appendage region — lifted below, the inspect-modal path). */
    if (io.va - OBJ_DELTA == 0x1fce2u)
        io.eax = run_timed_message_sequence(io.eax, io.edx, io.ebx, io.ecx);
    else if (io.va - OBJ_DELTA == 0x18e09u)
        io.eax = gdv_inspect_frame_callback(io.eax, io.ebx);
    else
        roth_unreachable((uint32_t)G32(VA_g_gdv_user_callback) - OBJ_DELTA);  /* GDV user-callback code-ptr — cutscene only (off --skip-gdv) */
#endif
    if (io.eax & 2)                                     /* test al,2; jne -> latch EOS */
        G8(VA_g_gdv_end_of_stream) = 0xff;                             /* mov [0x91dde],0xff */
    return io.eax;
}

/* gdv_callback_frame_boundary (0x4e041) — the per-frame gate the playback loops `je` on. If a user
 * callback is installed ([0x91d00]!=0), invoke it with action=2 (frame boundary); report whether the
 * stream has ended. Returns ZF for the caller's `je`: ZF=1 (set) iff a callback ran AND it latched EOS
 * ([0x91dde]==0xff); ZF=0 otherwise (no callback, or not yet EOS).
 *   4e041 push eax; cmp [0x91d00],0; je .clr            ; no callback -> ZF=0
 *   4e04b mov eax,2; call invoke_user_callback 0x4dd71
 *   4e055 cmp [0x91dde],0xff; jne .clr; cmp al,al(ZF=1); pop eax; ret
 *   4e062 .clr: or al,1(ZF=0); pop eax; ret
 * EAX is push/pop-preserved across the body, so the only output is ZF. invoke runs as DIRECT C (no int3
 * nesting). ABI_ZF (return nonzero => ZF set). LIVE-SWAP ONLY (invoke publishes via the callback). */
int gdv_callback_frame_boundary(void)
{
    if ((uint32_t)G32(VA_g_gdv_user_callback) == 0) return 0;          /* no callback installed -> ZF clear */
    gdv_invoke_user_callback(2);                 /* frame-boundary callback (eax is caller-preserved) */
    return ((uint8_t)G8(VA_g_gdv_end_of_stream) == 0xff) ? 1 : 0;      /* EOS latched -> ZF set */
}

/* gdv_advance_chunk_ptr (0x4dd33) — the 5-byte entry `call gdv_emit_decoded_frame` that then FALLS
 * THROUGH into gdv_advance_chunk_ptr_inner (0x4dd38, already lifted). So: emit the just-decoded frame,
 * then advance the chunk-ring pointer. Both callees run as direct C (no int3 nesting). ABI_VOID.
 * Callers (the original playback loop / prime_first_frame) run as original bytes, so this dispatches
 * single-shot from their `call 0x4dd33`. */
void gdv_advance_chunk_ptr(void)
{
    gdv_emit_decoded_frame();        /* 4dd33: call 0x4dcfc */
    gdv_advance_chunk_ptr_inner();   /* fall through into 0x4dd38 */
}

/* gdv_decode_subframe (0x4e00d) — decode one subframe and (if not too far ahead) emit it, then step the
 * subframe/audio counters and advance the chunk ring. [0x91db4]/[0x91dba] are 16-bit words.
 *   4e00d cmp word [db4],0; je ret
 *   4e017 pushal; call gdv_decode_video_chunk 0x4d384; cmp word [db4],6; jae +; call gdv_emit 0x4dcfc; +: popal
 *   4e02d dec word [db4]; dec word [dba]; call gdv_advance_chunk_ptr_inner 0x4dd38; ret
 * The pushal/popal only preserve the incoming regs for the (unused) args to the inner advance, which
 * reads its state from globals — so ABI_VOID with direct C callees is faithful. db4 is RE-READ after the
 * codec call (the codec can change it). LIVE-SWAP ONLY (calls the publishing emit). */
void gdv_decode_subframe(void)
{
    if ((uint16_t)G16(VA_g_gdv_audio_stream_base + 0x64) == 0) return;            /* cmp word [db4],0; je ret */
    gdv_decode_video_chunk();                    /* call 0x4d384 */
    if ((uint16_t)G16(VA_g_gdv_audio_stream_base + 0x64) < 6)                     /* cmp word [db4],6; jae skip-emit */
        gdv_emit_decoded_frame();                /* call 0x4dcfc */
    G16(VA_g_gdv_audio_stream_base + 0x64) = (int16_t)((uint16_t)G16(VA_g_gdv_audio_stream_base + 0x64) - 1);   /* dec word [db4] */
    G16(VA_g_gdv_audio_stream_base + 0x6a) = (int16_t)((uint16_t)G16(VA_g_gdv_audio_stream_base + 0x6a) - 1);   /* dec word [dba] */
    gdv_advance_chunk_ptr_inner();               /* call 0x4dd38 */
}

/* gdv_drain_pending_subframes (0x4dff4) — audio catch-up: while audio is active ([0x97b6c]!=0) run
 * gdv_decode_subframe in a loop while the subframe counter [0x91db4] (16-bit) stays >= 2.
 *   4dff4 cmp byte [97b6c],0; je ret
 *   4dffd call gdv_decode_subframe 0x4e00d; cmp word [db4],2; jae loop; ret
 * A bounded compute loop (decode catch-up, no file I/O / timer busy-wait), so it hosts as a single-shot
 * dispatch even though it can emit several frames per call. ABI_VOID; LIVE-SWAP ONLY. */
void gdv_drain_pending_subframes(void)
{
    if ((uint8_t)G8(VA_g_sos_timer_event_count + 0x8) == 0) return;              /* cmp byte [97b6c],0; je ret */
    do {
        gdv_decode_subframe();                   /* call 0x4e00d */
    } while ((uint16_t)G16(VA_g_gdv_audio_stream_base + 0x64) >= 2);              /* cmp word [db4],2; jae loop */
}

/* gdv_prime_first_frame (0x4de7a) — seed the chunk-ring pointers at the audio stream base, decode the
 * first chunk, advance, and reset the subframe counter, before the playback loop spins up.
 *   4de7a mov eax,[d50](g_gdv_audio_stream_base); [d68]=eax; [d6c]=eax
 *   4de89 call gdv_decode_video_chunk 0x4d384; [de4]=0; call gdv_advance_chunk_ptr 0x4dd33; [de4]=1
 *   4dea1 [d70]=[d6c]; word [db4]=0; ret
 * Both callees lifted -> direct C. Single-shot (caller decode_frame runs as original bytes). ABI_VOID. */
void gdv_prime_first_frame(void)
{
    uint32_t base = (uint32_t)G32(VA_g_gdv_audio_stream_base);             /* g_gdv_audio_stream_base */
    G32(VA_g_gdv_audio_stream_base + 0x18) = (int32_t)base;                       /* [d68] = base */
    G32(VA_g_gdv_audio_stream_base + 0x1c) = (int32_t)base;                       /* [d6c] = base */
    gdv_decode_video_chunk();                    /* call 0x4d384 */
    G8(VA_g_gdv_end_of_stream + 0x6) = 0;                                    /* mov byte [de4],0 */
    gdv_advance_chunk_ptr();                     /* call 0x4dd33 (emit + advance_inner) */
    G8(VA_g_gdv_end_of_stream + 0x6) = 1;                                    /* mov byte [de4],1 */
    G32(VA_g_gdv_audio_stream_base + 0x20) = (int32_t)(uint32_t)G32(VA_g_gdv_audio_stream_base + 0x1c);     /* [d70] = [d6c] */
    G16(VA_g_gdv_audio_stream_base + 0x64) = 0;                                   /* mov word [db4],0 */
}

/* gdv_present_first_frame (0x4c75c) — show frame 0 exactly once ([0x91de7] one-shot latch). On the
 * inspect-popup path (g_gdv_user_callback set) it calls the user callback with op=4 and, if that
 * "presents" (al&1), skips the hardware blit; otherwise it blits via the MAIN blitter entry 0x4c7a5.
 *   4c75c cmp byte [de7],0; jne ret; inc byte [de7]
 *   4c76b cmp [g_gdv_user_callback 0x91d00],0; je blit; mov eax,4; call 0x4dd71; test al,1; jne ret
 *   4c782 call gdv_blit_frame_to_vga 0x4c7a5; ret
 * 0x4c7a5 is host_video_driver (same 2840B body emit bridges via _alt 0x4c788) — bridged not skipped for
 * its engine side-effects (de4/de5 + palette copy); its out/INT10h serviced nested. ABI_VOID; LIVE-SWAP. */
void gdv_present_first_frame(void)
{
    if ((uint8_t)G8(VA_g_palette_dirty + 0x1) != 0) return;              /* cmp byte [de7],0; jne ret (already shown) */
    G8(VA_g_palette_dirty + 0x1) = (int8_t)((uint8_t)G8(VA_g_palette_dirty + 0x1) + 1);   /* inc byte [de7] */
    regs_t io;
    if ((uint32_t)G32(VA_g_gdv_user_callback) != 0) {                  /* cmp [g_gdv_user_callback],0; je blit */
        if (gdv_invoke_user_callback(4) & 1u) return;  /* presented -> skip blit — re-pointed */
    }
    memset(&io, 0, sizeof io);
    io.va = 0x4c7a5u + OBJ_DELTA;                        /* gdv_blit_frame_to_vga (bridge) */
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    gdv_blit_frame_to_vga();  /* image-free native (host_video_driver); side-effects only, pixel blit skipped */
#endif
}

#ifdef ROTH_STANDALONE
/* ============================================================================================
 * GDV video mode-set cluster — IMAGE-FREE native bodies.
 *
 * gdv_setup_video_mode 0x4e67e + its 3 unlifted port-I/O sub-leaves (probe_vesa_bank_granularity
 * 0x4eba9, init_modex_unchained 0x4ea34, clear_display_surface 0x4c59d) were classified
 * host_video_driver and left as call_orig (trap lane) / roth_unreachable (image-free lane). These
 * transcriptions let the image-free host run GDV cutscenes: INT10h -> g_os_soft_int(0x10) (the host
 * video_int10 seam handles set-mode/VESA 4F02/4F05), VGA port I/O -> g_os_port_out/in, and the
 * es:0xa0000 aperture writes -> the host VGA_LIN map. Under the host these hardware ops are absorbed by
 * the seams; only the C-level game-memory (0x91xxx) writes are engine-visible, so those are transcribed
 * exactly from disasm (objdump of recomp/build/obj1.bin). clear_vga_palette 0x4c392 is already lifted.
 *
 * TRAP-LANE INVARIANCE: this whole cluster is #ifdef ROTH_STANDALONE, so the trap-lane object is byte-
 * unchanged (its call_orig bridge in gdv_begin_playback's #ifndef arm is untouched). Reached only
 * off --skip-gdv, so oracle/trap-differential are unaffected. ============================================ */

/* run an int 0x10 through the host video seam; returns the resulting AX (low 16 of EAX). */
static uint16_t gdv_soft_int10(uint32_t eax, uint32_t ebx, uint32_t edx)
{
    regs_t r; memset(&r, 0, sizeof r);
    r.eax = eax; r.ebx = ebx; r.edx = edx;
    if (g_os_soft_int) g_os_soft_int(0x10, &r);
    return (uint16_t)r.eax;
}
static uint8_t gdv_port_in8(uint16_t port)  { return g_os_port_in ? g_os_port_in(port) : 0; }
static void    gdv_port_out8(uint16_t port, uint8_t val) { if (g_os_port_out) g_os_port_out(port, val); }

/* ---- GDV frame blit (0x4c7a5 / 0x4c788) — IMAGE-FREE.
 * The host publishes each frame from the decode buffer [0x91d40] (g_os_publish_frame, called in
 * gdv_emit_decoded_frame BEFORE the blit), so the format-specific pixel copy to the VGA surface
 * [0x91cd8]/[0x91cdc] is throwaway. The ONLY engine-visible game-memory writes in the 2840-byte body are
 * [0x91de5]=0, [0x91de4]=1, and the de5-gated 8-bpp per-frame palette copy header[0x91d44]+0x318 -> +0x18
 * (which the DAC-upload path write_vga_palette later reads); [0x9186c]/[0x91cbc] are blit-internal source-
 * stride scratch with no external reader (verified by grep over obj1.bin). So the sanctioned native
 * reproduces those side-effects and skips the pixel blit. */
static void gdv_blit_side_effects(void)
{
    if ((uint8_t)G8(VA_g_gdv_end_of_stream + 0x7) != 0 && (uint8_t)G8(VA_g_palette_dirty) == 1) {   /* 4c7a5/ac/ae/b5: [de5]!=0 && [de6]==1 */
        uint32_t       *dst = (uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_gdv_decode_buffer + 0x10) + 0x18);   /* 4c7ba/c0 header+0x18 */
        const uint32_t *src = (const uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_gdv_decode_buffer + 0x10) + 0x318); /* 4c7c3 header+0x318 */
        uint32_t i;
        for (i = 0; i < 0xc0; i++) dst[i] = src[i];                /* 4c7c9/ce rep movsd 0xc0 dwords (0x300 B) */
    }
    G8(VA_g_gdv_end_of_stream + 0x7) = 0x00;                                            /* 4c7d3 [de5] = 0 */
    G8(VA_g_gdv_end_of_stream + 0x6) = 0x01;                                            /* 4c7da [de4] = 1 */
}
/* gdv_blit_frame_to_vga (0x4c7a5) — main entry (present_first_frame). Side-effects, pixel blit skipped. */
void gdv_blit_frame_to_vga(void)
{
    gdv_blit_side_effects();
}
/* gdv_blit_frame_to_vga_alt (0x4c788) — alt entry (emit_decoded_frame): dispatch on format [0x91dc0];
 * only fmt&0x40 falls into the 0x4c7a5 side-effect block, the rest is ret or pixel-blit-only (skipped). */
void gdv_blit_frame_to_vga_alt(void)
{
    uint8_t al = (uint8_t)G8(VA_g_gdv_audio_stream_base + 0x70);                            /* 4c788 al = [dc0] */
    if (al & 0x80) return;                                        /* 4c78d test 0x80; jne 0x4c816 (ret) */
    if (al & 0x40) { gdv_blit_side_effects(); return; }           /* 4c795 test 0x40; jne 0x4c7a5 */
    if ((al & 0xf) == 0) return;                                  /* 4c799/9b/9d je 0x4c816 (ret) */
    if ((al & 0xf) == 3) return;                                  /* 4c79f/a1 je 0x4c816 (ret) */
    /* 4c7a3 jmp 0x4c7e1: no de4/de5/palette side-effects; pixel-blit only -> skipped */
}

/* gdv_probe_vesa_bank_granularity (0x4eba9): probe the VESA bank granularity by writing a marker
 * pattern into the 0xa0000 aperture, switching windows (INT10h 4F05) and reading it back. Returns DX =
 * bank-size units (>=1). The only game-memory write is [0x91de8] (LFB-detect flag byte). EAX (=edi) is
 * dropped by both callers (they read only DX). Under the host, 4F05 is a no-op and 0xa0000 is a single
 * flat RAM window, so no bank switch occurs -> readback of [0xa0000]=1 -> DX resolves to 1. */
uint16_t gdv_probe_vesa_bank_granularity(void)
{
    volatile uint8_t *vga = (volatile uint8_t *)(uintptr_t)0xa0000u;
    uint32_t ebx, esi, edi, ecx;
    uint8_t  al, ah;

    (void)gdv_soft_int10(0x4f05u, 0, 0);                /* 4ebad int10 4F05 window0 pos0 */
    (void)gdv_soft_int10(0x4f05u, 1, 0);                /* 4ebb7 int10 4F05 window1 pos0 */
    vga[0] = 1;                                         /* 4ebc5 [0xa0000] = 1 */
    ebx = 1; al = 1;                                    /* 4ebc8/cd */
    for (;;) {                                          /* 4ebcf marker loop: [0xa0000 + 2^n] = n+1 */
        al = (uint8_t)(al + 1);                         /* 4ebcf inc al */
        ebx += ebx;                                     /* 4ebd1 add ebx,ebx */
        if (ebx >= 0x10000u) break;                     /* 4ebd3 cmp; jae 4ebe0 */
        vga[ebx] = al;                                  /* 4ebdb [esi+ebx] = al */
    }
    (void)gdv_soft_int10(0x4f05u, 0, 1);                /* 4ebe0 int10 4F05 window0 pos1 (bank 1) */
    esi = 1; edi = 0x10000u;                            /* 4ebf2/f7 */
    al = vga[0];                                        /* 4ebfc al = [0xa0000] (marker readback) */
    if (al != 0) {                                      /* 4ebfe or al,al; je 4ec0e */
        ah = (uint8_t)(0x11u - al);                     /* 4ec02/04 ah = 0x11 - al */
        do {                                            /* 4ec06 loop */
            esi += esi;                                 /* 4ec06 add esi,esi */
            edi >>= 1;                                  /* 4ec08 shr edi,1 */
            ah = (uint8_t)(ah - 1);                     /* 4ec0a dec ah */
        } while (ah != 0);                              /* 4ec0c jne */
    }
    ecx = 0;                                            /* 4ec0e sub ecx,ecx */
    (void)gdv_soft_int10(0x4f05u, 0, 1);                /* 4ec10 int10 4F05 window0 pos1 */
    vga[0] = 4;                                         /* 4ec23 [0xa0000] = 4 (LFB-detect probe) */
    if (vga[0] != 4) ecx = (uint32_t)-1;                /* 4ec26 cmp; je; dec ecx */
    G8(VA_g_palette_dirty + 0x2) = (int8_t)(uint8_t)ecx;                 /* 4ec2c [0x91de8] = cl (LFB flag) */
    (void)gdv_soft_int10(0x4f05u, 0, 0);                /* 4ec33 int10 4F05 window0 pos0 (reset) */
    vga[0] = 0;                                         /* 4ec43 [0xa0000] = 0 (clear markers) */
    ebx = 1;                                            /* 4ec46 */
    for (;;) {                                          /* 4ec4d clear loop: [0xa0000 + 2^n] = 0 */
        ebx += ebx;                                     /* 4ec4d add ebx,ebx */
        if (ebx >= 0x10000u) break;                     /* 4ec4f cmp; jae 4ec5c */
        vga[ebx] = 0;                                   /* 4ec57 [esi+ebx] = 0 */
    }
    (void)edi;                                          /* eax=edi is dropped by callers */
    { uint16_t dx = (uint16_t)esi;                      /* 4ec5f edx = esi */
      if (dx == 0) dx = (uint16_t)(dx + 1);             /* 4ec61 or dx,dx; jne; inc dx */
      return dx; }                                      /* 4ec5d..4ec6c return DX */
}

/* gdv_init_modex_unchained (0x4ea34): set BIOS mode 13h then reprogram the VGA sequencer/CRTC for an
 * unchained (Mode-X, planar) 320x200/240/400 surface. Game-memory writes: [0x91cd8]/[0x91ce0]/
 * [0x91cdc]/[0x91ce4] (the two Mode-X page bases + offsets). All VGA port I/O routes through
 * g_os_port_out/in (in reads NULL->0 under the host, matching "no planar hardware"); the host absorbs
 * the register writes. cli/sti are irrelevant under the host and omitted. */
void gdv_init_modex_unchained(void)
{
    uint8_t al;
    (void)gdv_soft_int10(0x0013u, 0, 0);                /* 4ea34 int10 set mode 13h */
    gdv_clear_vga_palette();                     /* 4ea3a call 0x4c392 */
    G32(VA_g_dpcm_step_table + 0x434) = 0xa0000;                             /* 4ea3f */
    G32(VA_g_dpcm_step_table + 0x43c) = 0x8000;                              /* 4ea49 */
    G32(VA_g_dpcm_step_table + 0x438) = 0xa8000;                             /* 4ea53 */
    G32(VA_g_dpcm_step_table + 0x440) = 0x0;                                 /* 4ea5d */
    gdv_port_out8(0x3c4, 0x04);                         /* 4ea68 seq index 4 */
    al = (uint8_t)(gdv_port_in8(0x3c5) & 0xf7);         /* 4ea71/72 in; and 0xf7 */
    gdv_port_out8(0x3c5, al);                           /* 4ea74 */
    gdv_port_out8(0x3ce, 0x05);                         /* 4ea75 gc index 5 */
    al = (uint8_t)(gdv_port_in8(0x3cf) & 0xef);         /* 4ea7e/7f */
    gdv_port_out8(0x3cf, al);                           /* 4ea81 */
    gdv_port_out8(0x3ce, 0x06);                         /* 4ea84 gc index 6 */
    al = (uint8_t)(gdv_port_in8(0x3cf) & 0xfd);         /* 4ea89/8a */
    gdv_port_out8(0x3cf, al);                           /* 4ea8c */
    gdv_port_out8(0x3d4, 0x14);                         /* 4ea8d crtc index 0x14 */
    al = (uint8_t)(gdv_port_in8(0x3d5) & 0xbf);         /* 4ea96/97 */
    gdv_port_out8(0x3d5, al);                           /* 4ea99 */
    gdv_port_out8(0x3d4, 0x17);                         /* 4ea9a crtc index 0x17 */
    al = (uint8_t)(gdv_port_in8(0x3d5) | 0x40);         /* 4eaa1/a2 */
    gdv_port_out8(0x3d5, al);                           /* 4eaa4 */
    gdv_port_out8(0x3d4, 0x09);                         /* 4eaa5 crtc index 9 */
    al = (uint8_t)(gdv_port_in8(0x3d5) & 0xe0);         /* 4eaac/ad */
    if ((uint8_t)G8(VA_g_gdv_audio_format + 0x4) != 1) al = (uint8_t)(al + 1);  /* 4eaaf cmp byte[dce],1; je; inc al */
    gdv_port_out8(0x3d5, al);                           /* 4eaba */
    if ((uint8_t)G8(VA_g_gdv_audio_format + 0x4) == 2) {                    /* 4eabb cmp byte[dce],2; jne 4eb01 */
        gdv_port_out8(0x3c2, 0xe3);                     /* 4eac9 misc output reg */
        gdv_port_out8(0x3d4, 0x11); gdv_port_out8(0x3d5, 0x28);  /* 4ead1 out 0x3d4, 0x2811 */
        gdv_port_out8(0x3d4, 0x06); gdv_port_out8(0x3d5, 0x00);  /* 4ead7 out 0x3d4, 0x0006 */
        gdv_port_out8(0x3d4, 0x07); gdv_port_out8(0x3d5, 0x3e);  /* 4eadd out 0x3d4, 0x3e07 */
        gdv_port_out8(0x3d4, 0x10); gdv_port_out8(0x3d5, 0xea);  /* 4eae3 out 0x3d4, 0xea10 */
        gdv_port_out8(0x3d4, 0x11); gdv_port_out8(0x3d5, 0xa8);  /* 4eae9 out 0x3d4, 0xa811 */
        gdv_port_out8(0x3d4, 0x12); gdv_port_out8(0x3d5, 0xdf);  /* 4eaef out 0x3d4, 0xdf12 */
        gdv_port_out8(0x3d4, 0x15); gdv_port_out8(0x3d5, 0xe7);  /* 4eaf5 out 0x3d4, 0xe715 */
        gdv_port_out8(0x3d4, 0x16); gdv_port_out8(0x3d5, 0x06);  /* 4eafb out 0x3d4, 0x0616 */
    }
}

/* gdv_clear_display_surface (0x4c59d): zero the freshly-set display surface, then TAIL-JMP into
 * gdv_settle_palette_fade (0x4d2e0, already lifted) — a shared-tail merge (the disasm ends every path
 * with `jmp 0x4d2e0`, not `ret`). On the sole caller (the mode-set tail) [0x91de7] has already been
 * incremented, so the body is bypassed (4c5a9 jne) and this reduces to settle_palette_fade; the surface
 * clears are transcribed faithfully for completeness. Paths: A linear-0xa0000, B VESA-banked, C linear-
 * [cd8]/[cdc], D Mode-X planar (in/out; in->0 under the host). Only [0x91de4]/[0x91de7] are engine-visible
 * game-memory writes. */
void gdv_clear_display_surface(void)
{
    volatile uint8_t *vga = (volatile uint8_t *)(uintptr_t)0xa0000u;
    volatile uint8_t *d1, *d2;
    volatile uint32_t *p;
    uint32_t n, i, remaining, ecx, bp, words;
    uint8_t dcf, lfb, save1, save2;

    if (!((uint32_t)G32(VA_g_gdv_stream_flags) & 0x100)) goto settle; /* 4c59d test [d0c],0x100; je 4c608 */
    if ((uint8_t)G8(VA_g_palette_dirty + 0x1) != 0) goto settle;         /* 4c5a9 cmp [de7],0; jne 4c608 */
    G8(VA_g_gdv_end_of_stream + 0x6) = 0x01;                                 /* 4c5b2 [de4] = 1 */
    G8(VA_g_palette_dirty + 0x1) = (int8_t)((uint8_t)G8(VA_g_palette_dirty + 0x1) + 1);   /* 4c5b9 inc [de7] */
    dcf = (uint8_t)G8(VA_g_gdv_audio_format + 0x5);                         /* mode-word high byte */
    if (dcf & 0x40) {                                   /* 4c5bf jne 4c714 -> path C */
        n = (uint32_t)G32(VA_g_dpcm_step_table + 0x444) * (uint32_t)G32(VA_g_dpcm_step_table + 0x44c);
        d1 = (volatile uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_dpcm_step_table + 0x434);
        for (i = 0; i < n; i++) d1[i] = 0;              /* 4c731/37 clear [cd8] */
        if ((uint32_t)G32(VA_g_dpcm_step_table + 0x438) != (uint32_t)G32(VA_g_dpcm_step_table + 0x434)) {  /* 4c740 cmp; je 4c754 */
            d2 = (volatile uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_dpcm_step_table + 0x438);
            for (i = 0; i < n; i++) d2[i] = 0;          /* 4c74c/52 clear [cdc] */
        }
        goto settle;
    }
    if (dcf & 0x1) {                                    /* 4c5cc jne 4c60d -> path B (VESA banked) */
        bp = 0;
        lfb = (uint8_t)((uint8_t)G8(VA_g_palette_dirty + 0x2) != 0);     /* 4c618 */
        if (lfb) (void)gdv_soft_int10(0x4f05u, 1, 0);   /* 4c621 */
        (void)gdv_soft_int10(0x4f05u, 0, 0);            /* 4c62c */
        remaining = (uint32_t)G32(VA_g_dpcm_step_table + 0x444) * (uint32_t)G32(VA_g_dpcm_step_table + 0x44c);  /* 4c636/3c */
        for (;;) {                                      /* 4c64a bank loop */
            ecx = remaining;
            if (ecx >= 0x10000u) ecx = 0x10000u;        /* 4c64c/54 clamp to one 64K bank */
            for (i = 0; i < ecx; i++) vga[i] = 0;       /* 4c65d clear at 0xa0000 */
            remaining -= ecx;                           /* 4c660 */
            if (remaining == 0) break;                  /* 4c662 je 4c697 */
            bp = (bp + (uint16_t)G16(VA_g_gdv_audio_format + 0x6)) & 0xffffu;   /* 4c665 add bp,[dd0] (16-bit) */
            if (lfb) (void)gdv_soft_int10(0x4f05u, 1, bp);  /* 4c67f window B, pos=bp */
            (void)gdv_soft_int10(0x4f05u, 0, bp);       /* 4c68a window A, pos=bp */
        }
        if (lfb) (void)gdv_soft_int10(0x4f05u, 1, 0);   /* 4c6a4 reset window */
        (void)gdv_soft_int10(0x4f05u, 0, 0);            /* 4c6af */
        goto settle;
    }
    if (dcf & 0x80) {                                   /* 4c5d5 jne 4c6c3 -> path D (Mode-X planar) */
        words = ((uint32_t)G32(VA_g_dpcm_step_table + 0x444) >> 4) * (uint32_t)G32(VA_g_dpcm_step_table + 0x44c);  /* 4c6c7/cd/d0 */
        save1 = gdv_port_in8(0x3c4);                    /* 4c6db */
        gdv_port_out8(0x3c4, 0x02);                     /* 4c6dd map-mask select */
        save2 = gdv_port_in8(0x3c5);                    /* 4c6e1 */
        gdv_port_out8(0x3c5, 0x0f);                     /* 4c6e3/ed all planes */
        p = (volatile uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_dpcm_step_table + 0x434);  /* 4c6e7 */
        for (i = 0; i < words; i++) p[i] = 0;           /* 4c6f2 rep stosd */
        p = (volatile uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_dpcm_step_table + 0x438);  /* 4c6f4 */
        for (i = 0; i < words; i++) p[i] = 0;           /* 4c6fc rep stosd */
        gdv_port_out8(0x3c4, 0x02);                     /* 4c6fe/702/04 */
        gdv_port_out8(0x3c5, save2);                    /* 4c705/06/07 restore map mask */
        gdv_port_out8(0x3c4, save1);                    /* 4c708/09/0a restore seq index */
        goto settle;
    }
    /* path A (4c5e2): linear clear of 0xa0000 for pitch*height bytes */
    n = (uint32_t)G32(VA_g_dpcm_step_table + 0x444) * (uint32_t)G32(VA_g_dpcm_step_table + 0x44c);/* 4c5ea/f0 */
    for (i = 0; i < n; i++) vga[i] = 0;                 /* 4c5fd/603 */
settle:
    gdv_settle_palette_fade();                   /* jmp 0x4d2e0 (shared tail) */
}

/* gdv_setup_video_mode (0x4e67e, 950 B): pick the VGA/VESA/Mode-X mode from either the ctx-carried
 * preset (ctx+0x26 != 0) or the movie header dims + stream flags, set it via INT10h, and write the mode
 * fields back into the context. Returns CF (carry = failure; the corpus-dropped EBP=2 error code on the
 * bad-format path is not consumed by the sole caller begin_playback, which reads only CF). Faithful
 * transcription of every branch + game-memory store from objdump; goto labels mirror the asm addresses. */
uint32_t gdv_setup_video_mode(void)
{
    uint32_t ctx, hdr, c, v, t;
    uint16_t modeword = 0, dx = 0, ax = 0, g, retax;
    uint8_t  ah = 0, al = 0, fmt, d29;
    uint32_t r_eax = 0, r_ebx = 0, r_ecx = 0, r_edx = 0;

    G8(VA_g_palette_dirty + 0x3)  = 0x08;                                /* 4e67e bpp = 8 */
    G8(VA_g_palette_dirty + 0x1)  = 0x00;                                /* 4e685 mode-valid counter = 0 */
    G32(VA_g_dpcm_step_table + 0x434) = 0xa0000;                             /* 4e68c dest surface base */
    G32(VA_g_dpcm_step_table + 0x438) = 0xa0000;                             /* 4e696 dest surface base 2 */
    G16(VA_g_gdv_audio_format + 0x4) = (int16_t)0x13;                       /* 4e6a0 mode word = 0x13 (default) */
    ctx = (uint32_t)G32(VA_g_gdv_context);                       /* 4e6a9 esi = g_gdv_context */
    if (*(uint16_t *)(uintptr_t)(ctx + 0x26) == 0) goto compute;  /* 4e6af/b4 je -> header picker */

    /* ---------- PRESET path (ctx+0x26 != 0: use the ctx-carried mode) ---------- */
    r_eax = (uint16_t)*(uint16_t *)(uintptr_t)(ctx + 0x2a);   /* 4e6ba/bc ax = ctx+0x2a */
    G32(VA_g_dpcm_step_table + 0x448) = (int32_t)r_eax;                            /* 4e6c0 mode-W */
    if (*(uint16_t *)(uintptr_t)(ctx + 0x70) != 0)            /* 4e6c5 cmp; je 4e6d0 */
        r_eax = (uint16_t)*(uint16_t *)(uintptr_t)(ctx + 0x70);  /* 4e6cc */
    G32(VA_g_dpcm_step_table + 0x444) = (int32_t)r_eax;                           /* 4e6d0 pitch */
    r_eax = (uint16_t)*(uint16_t *)(uintptr_t)(ctx + 0x2c);  /* 4e6d5 */
    G32(VA_g_dpcm_step_table + 0x44c) = (int32_t)r_eax;                           /* 4e6d9 mode-H */
    modeword = (uint16_t)*(uint16_t *)(uintptr_t)(ctx + 0x26);   /* 4e6de ax = ctx+0x26 */
    G16(VA_g_gdv_audio_format + 0x4) = (int16_t)modeword;                        /* 4e6e2 mode word */
    ah = (uint8_t)(modeword >> 8);
    al = (uint8_t)(modeword & 0xff);
    if (ah & 0x40) {                                         /* 4e6e8 test ah,0x40; je 4e731 */
        if (*(int32_t *)(uintptr_t)(ctx + 0x74) == 0) {      /* 4e6ed cmp; jne 4e6fc */
            G8(VA_g_gdv_audio_format + 0x5) = (int8_t)((uint8_t)G8(VA_g_gdv_audio_format + 0x5) - 0x40);  /* 4e6f3 sub byte[dcf],0x40; jmp 4e731 */
        } else {
            v = *(uint32_t *)(uintptr_t)(ctx + 0x74);        /* 4e6fc */
            r_eax = v; r_ecx = 0; r_edx = 0; r_ebx = v;      /* 4e6ff/701/703 */
            if (*(uint32_t *)(uintptr_t)(ctx + 0x18) & 0x400000) {  /* 4e705 test; je 4e722 */
                r_edx  = (uint32_t)G32(VA_g_dpcm_step_table + 0x44c);             /* 4e70e */
                r_edx *= (uint32_t)G32(VA_g_dpcm_step_table + 0x444);             /* 4e714 imul */
                r_ebx  = v + r_edx;                          /* 4e71b add ebx,edx */
                goto store_block;                            /* 4e71d jmp 4e7c1 */
            }
            G32(VA_g_dpcm_step_table + 0x434) = (int32_t)v;                       /* 4e722 */
            G32(VA_g_dpcm_step_table + 0x438) = (int32_t)v;                       /* 4e727 */
            goto tail_a;                                     /* 4e72c jmp 4e9f1 */
        }
    }
    if (ah & 0x1) {                                          /* 4e731 test ah,0x1; je 4e78a */
        G8(VA_g_palette_dirty + 0x2) = (int8_t)*(uint8_t *)(uintptr_t)(ctx + 0x28);   /* 4e736/39 [de8]=ctx+0x28 */
        d29 = *(uint8_t *)(uintptr_t)(ctx + 0x29);           /* 4e741 dl=ctx+0x29 (dh=0) */
        dx = d29;
        if (dx == 0) {                                       /* 4e744 or dx,dx; jne 4e75c */
            dx = gdv_probe_vesa_bank_granularity();   /* 4e749 call -> dx */
            *(uint8_t *)(uintptr_t)(ctx + 0x29) = (uint8_t)dx;           /* 4e74e [ctx+0x29]=dl */
            *(uint8_t *)(uintptr_t)(ctx + 0x28) = (uint8_t)G8(VA_g_palette_dirty + 0x2);  /* 4e751/57 [ctx+0x28]=dh=[de8] */
            dx = (uint16_t)(dx & 0x00ff);                    /* 4e75a sub dh,dh */
        }
        G16(VA_g_gdv_audio_format + 0x6) = (int16_t)dx;                          /* 4e75c word[dd0]=dx */
        if ((uint8_t)G8(VA_g_palette_dirty + 0x2) != 0)                       /* 4e767 cmp byte[de8],0; je 4e77b */
            (void)gdv_soft_int10(0x4f05u, 1, 0);             /* 4e770/75/79 */
        (void)gdv_soft_int10(0x4f05u, 0, 0);                 /* 4e77b/7d/81 */
        goto tail_a;                                         /* 4e785 jmp 4e9f1 */
    }
    if (ah & 0x80) {                                         /* 4e78a test ah,0x80; je 4e7b3 */
        r_edx = 0x4000; r_ebx = 0xa4000;                    /* 4e78f/94 */
        if (al & 0x3) { r_edx = 0x8000; r_ebx = 0xa8000; }  /* 4e799/9d/a2 */
        r_eax = 0xa0000; r_ecx = 0;                         /* 4e7a7/ac */
        goto store_block;                                   /* 4e7b1 jmp 4e7c1 */
    }
    r_eax = 0xa0000; r_ecx = 0; r_edx = 0; r_ebx = 0xa0000; /* 4e7b3/b8/bd/bf */
store_block:                                                /* 4e7c1 */
    if (*(uint16_t *)(uintptr_t)(ctx + 0x2e) != 0) {        /* 4e7c1 cmp; je 4e7cb */
        t = r_edx; r_edx = r_ecx; r_ecx = t;               /* 4e7c8 xchg edx,ecx */
        t = r_ebx; r_ebx = r_eax; r_eax = t;               /* 4e7ca xchg ebx,eax */
    }
    G32(VA_g_dpcm_step_table + 0x434) = (int32_t)r_eax;                          /* 4e7cb */
    G32(VA_g_dpcm_step_table + 0x43c) = (int32_t)r_edx;                          /* 4e7d0 */
    G32(VA_g_dpcm_step_table + 0x438) = (int32_t)r_ebx;                          /* 4e7d6 */
    G32(VA_g_dpcm_step_table + 0x440) = (int32_t)r_ecx;                          /* 4e7dc */
    goto tail_a;                                            /* 4e7e2 jmp 4e9f1 */

    /* ---------- COMPUTE path (pick mode from header dims + flags) ---------- */
compute:                                                   /* 4e7e7 */
    hdr = (uint32_t)G32(VA_g_gdv_decode_buffer + 0x10);                           /* 4e7e7 esi = header */
    fmt = (uint8_t)G8(VA_g_palette_dirty);                             /* 4e7ed al = pixel format */
    if (fmt == 1) goto fmt8;                                /* 4e7f2/f4 je 4e805 */
    if (fmt == 2) goto fmt15;                               /* 4e7f6/f8 je 4ea01 */
    return 1;                                               /* 4e7fe/803/04 ebp=2; stc; ret (EBP dropped) */
fmt8:                                                       /* 4e805 (8-bpp) */
    G32(VA_g_dpcm_step_table + 0x448) = 0x140; G32(VA_g_dpcm_step_table + 0x444) = 0x140; G32(VA_g_dpcm_step_table + 0x44c) = 0xc8;  /* 4e805/0f/19 */
    dx = *(uint16_t *)(uintptr_t)(hdr + 0x14);              /* 4e823 dx = width */
    ax = *(uint16_t *)(uintptr_t)(hdr + 0x16);              /* 4e827 ax = height */
    if ((uint32_t)G32(VA_g_gdv_stream_flags) & 0x20)   ax = (uint16_t)(ax + ax);    /* 4e82b/37 */
    if ((uint32_t)G32(VA_g_gdv_stream_flags) & 0x2000) dx = (uint16_t)(dx + dx);    /* 4e83a/46 */
    if ((uint32_t)G32(VA_g_gdv_stream_flags) & 0x40) {                    /* 4e849 test flags,0x40; je 4e8ab */
        G16(VA_g_gdv_audio_format + 0x4) = (int16_t)0x8000;                     /* 4e855 Mode-X marker */
        if ((uint32_t)G32(VA_g_gdv_stream_flags) & 0x80) goto call_modex; /* 4e85e/68 jne 4e989 */
        if (ax <= 0xc8) goto call_modex;                    /* 4e86e/72 jbe 4e989 */
        if (ax <= 0xf0) {                                   /* 4e878/7c jbe 4e894 */
            G32(VA_g_dpcm_step_table + 0x44c) = 0xf0;                            /* 4e894 */
            G16(VA_g_gdv_audio_format + 0x4) = (int16_t)((uint16_t)G16(VA_g_gdv_audio_format + 0x4) + 2);   /* 4e89e add word[dce],2 */
        } else {
            G32(VA_g_dpcm_step_table + 0x44c) = 0x190;                           /* 4e87e */
            G16(VA_g_gdv_audio_format + 0x4) = (int16_t)((uint16_t)G16(VA_g_gdv_audio_format + 0x4) + 1);   /* 4e888 inc word[dce] */
        }
        goto call_modex;                                    /* 4e88f/a6 jmp 4e989 */
    }
    if (dx > 0x140) goto L8bc;                               /* 4e8ab ja 4e8bc */
    if (ax <= 0xc8) goto vga13;                              /* 4e8b2 jbe 4e990 */
L8bc:                                                        /* 4e8bc */
    if (ax > 0x1e0) goto L8c9;                               /* 4e8bc ja 4e8c9 */
    if (dx <= 0x280) goto mode101;                           /* 4e8c2 jbe 4e928 */
L8c9:                                                        /* 4e8c9 */
    if (ax > 0x258) goto mode105;                            /* 4e8c9 ja 4e8ff */
    if (dx > 0x320) goto mode105;                            /* 4e8cf ja 4e8ff */
    G16(VA_g_gdv_audio_format + 0x4) = (int16_t)0x103; G32(VA_g_dpcm_step_table + 0x448) = 0x320; G32(VA_g_dpcm_step_table + 0x444) = 0x320; G32(VA_g_dpcm_step_table + 0x44c) = 0x258;  /* 4e8d6.. mode 0x103 */
    goto set_vesa;                                           /* 4e8fd jmp 4e94f */
mode105:                                                     /* 4e8ff (1024x768) */
    G16(VA_g_gdv_audio_format + 0x4) = (int16_t)0x105; G32(VA_g_dpcm_step_table + 0x448) = 0x400; G32(VA_g_dpcm_step_table + 0x444) = 0x400; G32(VA_g_dpcm_step_table + 0x44c) = 0x300;  /* 4e8ff.. */
    goto set_vesa;                                           /* 4e926 jmp 4e94f */
mode101:                                                     /* 4e928 (640x480) */
    G16(VA_g_gdv_audio_format + 0x4) = (int16_t)0x101; G32(VA_g_dpcm_step_table + 0x448) = 0x280; G32(VA_g_dpcm_step_table + 0x444) = 0x280; G32(VA_g_dpcm_step_table + 0x44c) = 0x1e0;  /* 4e928.. */
    goto set_vesa;
fmt15:                                                       /* 4ea01 (15-bpp linear) */
    G8(VA_g_palette_dirty + 0x3) = 0x0f;                                      /* 4ea01 bpp = 15 */
    G16(VA_g_gdv_audio_format + 0x4) = (int16_t)0x110;                           /* 4ea08 mode 0x110 */
    G32(VA_g_dpcm_step_table + 0x448) = 0x280; G32(VA_g_dpcm_step_table + 0x444) = 0x500; G32(VA_g_dpcm_step_table + 0x44c) = 0x1e0;  /* 4ea11/1b/25 */
    /* fall to set_vesa (jmp 4e94f) */
set_vesa:                                                    /* 4e94f VESA set mode (4F02) */
    retax = gdv_soft_int10(0x4f02u, (uint16_t)G16(VA_g_gdv_audio_format + 0x4), 0);  /* 4e94f/53/5a bx=[dce] */
    if (retax != 0x4f) return 1;                             /* 4e95c cmp ax,0x4f; jne 4e9ff (stc,ret) */
    G8(VA_g_palette_dirty + 0x1) = (int8_t)((uint8_t)G8(VA_g_palette_dirty + 0x1) + 1);        /* 4e966 inc [de7] */
    gdv_clear_vga_palette();                          /* 4e96c call 0x4c392 */
    if ((uint16_t)G16(VA_g_gdv_audio_format + 0x6) == 0)                         /* 4e971 cmp word[dd0],0; jne 4e9ae */
        G16(VA_g_gdv_audio_format + 0x6) = (int16_t)gdv_probe_vesa_bank_granularity();  /* 4e97b/80 */
    goto tail_b;                                             /* 4e987 jmp 4e9ae */
call_modex:                                                  /* 4e989 */
    gdv_init_modex_unchained();                       /* call 0x4ea34 */
    goto tail_b;                                             /* 4e98e jmp 4e9ae */
vga13:                                                       /* 4e990 (BIOS mode 13h) */
    G8(VA_g_palette_dirty + 0x3) = 0x08;                                      /* 4e990 bpp = 8 */
    G16(VA_g_gdv_audio_format + 0x4) = (int16_t)0x13;                            /* 4e997/9b */
    (void)gdv_soft_int10(0x0013u, 0, 0);                     /* 4e9a1 int 0x10 set mode 13h */
    G8(VA_g_palette_dirty + 0x1) = (int8_t)((uint8_t)G8(VA_g_palette_dirty + 0x1) + 1);        /* 4e9a3 inc [de7] */
    gdv_clear_vga_palette();                          /* 4e9a9 call 0x4c392 */
    /* fall to tail_b */
tail_b:                                                      /* 4e9ae ctx write-back */
    c = (uint32_t)G32(VA_g_gdv_context);                              /* 4e9ae esi = ctx (re-read) */
    *(uint16_t *)(uintptr_t)(c + 0x2a) = (uint16_t)G32(VA_g_dpcm_step_table + 0x448);  /* 4e9b4/b9 */
    *(uint16_t *)(uintptr_t)(c + 0x70) = (uint16_t)G32(VA_g_dpcm_step_table + 0x444);  /* 4e9bd/c2 */
    *(uint16_t *)(uintptr_t)(c + 0x2c) = (uint16_t)G32(VA_g_dpcm_step_table + 0x44c);  /* 4e9c6/cb */
    *(uint16_t *)(uintptr_t)(c + 0x26) = (uint16_t)G16(VA_g_gdv_audio_format + 0x4);  /* 4e9cf/d5 */
    g = (uint16_t)G16(VA_g_gdv_audio_format + 0x6);                              /* 4e9d9 */
    if (g != 0) {                                            /* 4e9df or ax,ax; je 4e9f1 */
        *(uint8_t *)(uintptr_t)(c + 0x29) = (uint8_t)g;      /* 4e9e4 [ctx+0x29]=al */
        *(uint8_t *)(uintptr_t)(c + 0x28) = (uint8_t)((uint8_t)G8(VA_g_palette_dirty + 0x2) & 1);  /* 4e9e7/ec/ee */
    }
tail_a:                                                      /* 4e9f1 */
    G8(VA_g_gdv_end_of_stream + 0x6) = 0x00;                                      /* 4e9f1 */
    gdv_clear_display_surface();                      /* 4e9f8 call 0x4c59d (tail-jmps into settle) */
    return 0;                                                /* 4e9fd clc; ret (CF=0) */
}
#endif /* ROTH_STANDALONE */

/* gdv_begin_playback (0x4e17f) — set up the cutscene video mode then prime the first frame. Returns CF
 * (carry = setup failed), which the caller (decode_frame / present_streamed_frame) branches on.
 *   4e17f call gdv_setup_video_mode 0x4e67e; jb ret (CF=1, setup failed)
 *   4e186 call gdv_init_frame_geometry 0x4bf4c; call gdv_present_first_frame 0x4c75c; clc (CF=0); ret
 * setup_video_mode is host_video_driver (VESA/Mode-X mode-set: int 0x10 + port I/O) — BRIDGED via
 * call_orig; its faults are serviced nested in OBJ1. Its CF (io.eflags bit0) is begin_playback's result
 * on the failure path. init_frame_geometry + present_first_frame are lifted -> direct C. Single-shot
 * (caller runs as original bytes). ABI_CF; LIVE-SWAP ONLY (bridges the mode-set). */
uint32_t gdv_begin_playback(void)
{
    regs_t io; memset(&io, 0, sizeof io);
    io.va = 0x4e67eu + OBJ_DELTA;                       /* call gdv_setup_video_mode (host_video_driver) */
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    if (gdv_setup_video_mode()) io.eflags |= 1u;  /* image-free native (host_video_driver); CF=1 on mode-set failure */
#endif
    if (io.eflags & 1u) return 1;                       /* jb 0x4e191 -> return with CF=1 */
    gdv_init_frame_geometry();                   /* call 0x4bf4c */
    gdv_present_first_frame();                   /* call 0x4c75c */
    return 0;                                            /* clc -> CF=0 */
}

/* gdv_configure_audio_device (0x4e192) — configure the SOS digital-audio device for the movie. If the
 * ctx already carries a device descriptor ([ctx+0x30]!=0), copy it into the SOS globals
 * [0x97be4..0x97b7c] and (re)register via gdv_audio_detect_driver 0x552f0; else (unless flags&0x10 or
 * the ctx already names a device-file string) load the drivers via gdv_audio_load_drivers 0x55360 and
 * stash the resulting descriptor back into the ctx. Both SOS calls are host_audio_driver -> BRIDGED via
 * call_orig (their far-calls to the audio MAGIC pages are serviced nested), and set [0x91d10] (error).
 * Returns CF (carry = [0x91d10]!=0) — the corpus DROPPED this (decompile says void; the disasm clc/stc's).
 * Reached only via the lifted init_audio_output -> direct C (COLD int3). ABI_CF; LIVE-SWAP ONLY. */
uint32_t gdv_configure_audio_device(void)
{
    uint32_t ctx = (uint32_t)G32(VA_g_gdv_context);              /* mov esi,[g_gdv_context] */
    if (*(int32_t *)(uintptr_t)(ctx + 0x30) != 0) {     /* cmp [esi+0x30],0; jne */
        G32(VA_g_sos_timer_event_count + 0x80) = *(int32_t *)(uintptr_t)(ctx + 0x30);
        G32(VA_g_sos_timer_event_count + 0xc) = *(int32_t *)(uintptr_t)(ctx + 0x34);
        G32(VA_g_sos_timer_event_count + 0x10) = *(int32_t *)(uintptr_t)(ctx + 0x38);
        G32(VA_g_sos_timer_event_count + 0x14) = *(int32_t *)(uintptr_t)(ctx + 0x3c);
        G32(VA_g_sos_timer_event_count + 0x18) = *(int32_t *)(uintptr_t)(ctx + 0x40);
        uint32_t dev   = *(uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_gdv_context) + 4); /* eax = [g_gdv_context+4] */
        uint32_t drvid = (uint16_t)G16(VA_g_sos_timer_event_count + 0x80);        /* movzx edx, word [97be4] */
        os_audio_gdv_detect_driver(dev, drvid);         /* 0x552f0 gdv_audio_detect_driver (C2 seam) */
        return ((uint32_t)G32(VA_g_gdv_stream_flags + 0x4) != 0) ? 1u : 0u; /* cmp [d10],0; jne stc else clc */
    }
    /* [ctx+0x30]==0 */
    if ((uint32_t)G32(VA_g_gdv_stream_flags) & 0x10) return 0;        /* test [g_gdv_stream_flags],0x10; jne -> clc */
    uint32_t dev = *(uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_gdv_context) + 4);  /* [ctx+4] */
    if (dev != 0 && *(uint8_t *)(uintptr_t)dev != 0) {  /* or eax,eax; je; cmp byte[eax],0; jne */
        G8(VA_g_gdv_audio_enabled) = 0;                                /* g_gdv_audio_enabled = 0 (4e1e7) */
        return 0;                                        /* clc */
    }
    os_audio_gdv_load_drivers();                        /* 0x55360 gdv_audio_load_drivers (C2 seam) */
    if ((uint32_t)G32(VA_g_gdv_stream_flags + 0x4) != 0) return 1;          /* cmp [d10],0; jne stc */
    uint32_t ctx2 = (uint32_t)G32(VA_g_gdv_context);             /* re-read esi=[g_gdv_context] */
    *(uint32_t *)(uintptr_t)(ctx2 + 0x30) = (uint32_t)G32(VA_g_sos_timer_event_count + 0x80);
    *(uint32_t *)(uintptr_t)(ctx2 + 0x34) = (uint32_t)G32(VA_g_sos_timer_event_count + 0xc);
    *(uint32_t *)(uintptr_t)(ctx2 + 0x38) = (uint32_t)G32(VA_g_sos_timer_event_count + 0x10);
    *(uint32_t *)(uintptr_t)(ctx2 + 0x3c) = (uint32_t)G32(VA_g_sos_timer_event_count + 0x14);
    *(uint32_t *)(uintptr_t)(ctx2 + 0x40) = (uint32_t)G32(VA_g_sos_timer_event_count + 0x18);
    return 0;                                            /* clc */
}

/* gdv_init_audio_output (0x4bddf) — set up movie audio output. flags&2: the PIT timer-sync path —
 * install the A/V sync (gdv_install_timer_sync 0x4e590, BRIDGED: PIT port-I/O + int-vector hooking,
 * hardware the host owns) and set [0x91dc2]|0x20. Otherwise: configure the SOS device (lifted
 * configure_audio_device) then submit the first audio buffer via gdv_audio_setup_voices 0x55440
 * (host_audio_driver, SOS — BRIDGED), seeding the sample-rate budget [0x9187c]=word[hdr+8]<<3 and (for
 * the 0x52b0 rate) bumping [0x91dbe]+=0x80. Returns CF + (on error) EBP=[0x91d10]|0x80000000 — corpus
 * dropped both. The dead block at 0x4be66 (Ghidra "unreachable") is elided: the `ja` at 0x4be64 is always
 * taken (eax is the constant 0x5622 > 0x4a6a), so the setup_voices arg EBX is always 0x5622. Reached only
 * via the lifted decoder_open -> direct C (COLD int3). ABI_CF_EBP; LIVE-SWAP ONLY. */
uint32_t gdv_init_audio_output(uint32_t *ebp_out)
{
    if ((uint32_t)G32(VA_g_gdv_stream_flags) & 2) {                   /* test [g_gdv_stream_flags],2; jne */
        G8(VA_g_gdv_audio_enabled) = 0;                                /* g_gdv_audio_enabled = 0 */
        if (!((uint32_t)G32(VA_g_gdv_stream_flags) & 4)) {            /* test [d0c],4; jne ret */
            regs_t io; memset(&io, 0, sizeof io);       /* gdv_install_timer_sync (PIT) — bridge */
            io.va = 0x4e590u + OBJ_DELTA;
#ifndef ROTH_STANDALONE
            call_orig(&io);
#else
            roth_unreachable(0x4e590u);   /* GDV A/V-sync install (PIT) — cutscene only (off --skip-gdv) */
#endif
            G16(VA_g_gdv_audio_stream_base + 0x72) = (int16_t)((uint16_t)G16(VA_g_gdv_audio_stream_base + 0x72) | 0x20);   /* or word [dc2],0x20 */
        }
        return 0;                                        /* CF=0 (success) */
    }
    if (!((uint32_t)G32(VA_g_gdv_stream_flags) & 0x10)) {             /* test [d0c],0x10; jne 4be24 */
        if (gdv_configure_audio_device()) goto err;   /* call 0x4e192; jae ok else jmp 4bea5 */
    } else {                                            /* 4be24 */
        G8(VA_g_gdv_audio_enabled) = 0;                                /* g_gdv_audio_enabled = 0 */
        G32(VA_g_gdv_stream_flags + 0x4) = 0;
    }
    /* 4be35: submit first audio buffer */
    uint32_t edx = (uint32_t)G32(VA_g_gdv_audio_buf_size + 0x4);              /* mov edx,[0x91d30] */
    uint32_t hdr = (uint32_t)G32(VA_g_gdv_decode_buffer + 0x10);              /* mov esi,[0x91d44] (frame header) */
    uint16_t hc  = *(uint16_t *)(uintptr_t)(hdr + 0xc); /* movzx ebx, word [esi+0xc] */
    uint32_t ecx = (uint32_t)(*(uint16_t *)(uintptr_t)(hdr + 8)) << 3;  /* word[esi+8]<<3 */
    G32(VA_g_particle_pool + 0x18) = (int32_t)ecx;                        /* mov [0x9187c],ecx (sample-rate budget) */
    if (hc == 0x52b0)                                   /* cmp ebx,0x52b0; je */
        G16(VA_g_gdv_audio_stream_base + 0x6e) = (int16_t)((uint16_t)G16(VA_g_gdv_audio_stream_base + 0x6e) + 0x80);   /* add word [dbe],0x80 */
    uint32_t dev = *(uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_gdv_context) + 4); /* eax=[g_gdv_context+4] */
    /* ebx=0x5622 const (dead block 0x4be66 elided; eax const 0x5622>0x4a6a), ecx=0x3c */
    os_audio_gdv_setup_voices(dev, edx, 0x5622u, 0x3cu);/* 0x55440 gdv_audio_setup_voices (C2 seam) */
    if ((uint32_t)G32(VA_g_gdv_stream_flags + 0x4) != 0) goto err;          /* cmp [d10],0; jne 4bea5 */
    return 0;                                            /* 4be9f ret (CF=0, success) */
err:
    *ebp_out = (uint32_t)G32(VA_g_gdv_stream_flags + 0x4) | 0x80000000u;    /* 4bea5: ebp=[d10]|0x80000000; stc */
    return 1;
}

/* gdv_alloc_decode_buffer (0x4b9d4) — allocate + zero the linear decode buffer (ctx+0xc bytes) unless the
 * ctx already carries one ([ctx+8]!=0), recording the base in g_gdv_decode_buffer [0x91d34]/[0x91d44] and
 * [ctx+0x50]. The original takes ESI=ctx; here ctx is read from g_gdv_context [0x91d14] (decoder_open sets
 * it == ctx before this runs), so this is a clean void-input fn. game_heap_alloc 0x1517d is the DPMI-backed
 * game allocator (host-bridged, not C) -> call_orig (its int 31h is serviced nested in-game). Returns CF +
 * (on failure) EBP error code 0x10 (size==0) / 0x11 (alloc failed) — corpus dropped both. Reached only via
 * the lifted decoder_open -> direct C (COLD int3). ABI_CF_EBP; LIVE-SWAP ONLY (fresh pool ptr, not oracled). */
uint32_t gdv_alloc_decode_buffer(uint32_t *ebp_out)
{
    uint32_t ctx = (uint32_t)G32(VA_g_gdv_context);              /* esi = g_gdv_context (== decoder_open's ctx) */
    uint32_t size = *(uint32_t *)(uintptr_t)(ctx + 0xc);
    if (size == 0) { *ebp_out = 0x10; return 1; }       /* mov ebp,0x10; or edx,edx; je stc */
    G32(VA_g_gdv_audio_stream_base + 0x54) = (int32_t)size;                       /* mov [0x91da4],edx */
    if (*(uint32_t *)(uintptr_t)(ctx + 8) == 0) {       /* or eax,eax; jne skip-alloc */
        uint32_t p = game_heap_alloc((int32_t)size);   /* game_heap_alloc — re-pointed */
        if (p == 0) { *ebp_out = 0x11; return 1; }      /* or eax,eax; je stc */
        *(uint32_t *)(uintptr_t)(ctx + 8) = p;          /* mov [esi+8],eax */
        G16(VA_g_gdv_context + 0xe) = (int16_t)((uint16_t)G16(VA_g_gdv_context + 0xe) | 1);   /* or word [0x91d22],1 (alloc-owned) */
    }
    uint32_t *buf = (uint32_t *)(uintptr_t)(*(uint32_t *)(uintptr_t)(ctx + 8));
    uint32_t n = (*(uint32_t *)(uintptr_t)(ctx + 0xc)) >> 2;   /* shr ecx,2 */
    for (uint32_t i = 0; i < n; i++) buf[i] = 0;        /* sub eax,eax; rep stosd */
    uint32_t base = *(uint32_t *)(uintptr_t)(ctx + 8);
    G32(VA_g_gdv_decode_buffer) = (int32_t)base;                       /* g_gdv_decode_buffer */
    G32(VA_g_gdv_decode_buffer + 0x10) = (int32_t)base;                       /* [0x91d44] */
    *(uint32_t *)(uintptr_t)(ctx + 0x50) = base;        /* mov [esi+0x50],eax */
    return 0;                                            /* clc */
}

/* clear_cutscene_region (0x1fc5c) — black-fill the cutscene/letterbox rect (x1=[0x83b3c], y1=[0x83b40],
 * x2=[0x83b44], y2=[0x83b48]) row-by-row in the back buffer (g_framebuffer_ptr [0x90a98], pitch [0x85498]),
 * mark it dirty (add_dirty_rect 0x15b69, bridged — video_display), then clear the region flag [0x83b44]=0.
 * [0x83b44] doubles as x2 (right edge) and the "needs clearing" flag. mem_fill 0x4b360 is lifted -> C.
 * Reached single-shot from the (original-bytes) play_gdv_cutscene when [0x83b44]!=0 (letterboxed cutscene).
 * ABI_VOID; LIVE-SWAP ONLY (writes the live back buffer + dirty-rect list). */
void clear_cutscene_region(void)
{
    uint32_t y1 = (uint32_t)G32(VA_g_dialogue_busy_flag + 0x56), x1 = (uint32_t)G32(VA_g_dialogue_busy_flag + 0x52);
    uint32_t rows  = (uint32_t)G32(VA_g_dialogue_busy_flag + 0x5e) + 1 - y1;       /* [83b48]+1-[83b40] */
    uint32_t pitch = (uint32_t)G32(VA_g_screen_pitch);                /* g_screen_pitch */
    uint32_t width = (uint32_t)G32(VA_g_dialogue_busy_flag + 0x5a) - x1;           /* edi = [83b44] - [83b3c] */
    uint32_t dest  = (uint32_t)G32(VA_g_framebuffer_ptr) + pitch * y1 + x1;  /* fb + pitch*[83b40] + [83b3c] */
    for (uint32_t row = 0; row < rows; row++) {             /* cmp esi,[ebp-4]; jb */
        mem_fill((void *)(uintptr_t)dest, 0, width); /* mem_fill(eax=dest, edx=0, ebx=width) */
        dest += pitch;                                      /* add ecx,[g_screen_pitch] */
    }
    add_dirty_rect(x1, (int32_t)y1, (uint32_t)G32(VA_g_dialogue_busy_flag + 0x5a), (uint32_t)G32(VA_g_dialogue_busy_flag + 0x5e));  /* add_dirty_rect — re-pointed */
    G32(VA_g_dialogue_busy_flag + 0x5a) = 0;                                       /* clear region-dirty flag */
}

/* present_cutscene_frame (0x20a8a) — present one cutscene/splash frame to the back buffer + flip. If an
 * image handle is set ([0x83c74]=g_cutscene_image_handle), remap its pixels to the live palette
 * (remap_pixels_to_palette 0x204a3) + blit it scaled (blit_image_scaled_to_framebuffer 0x202d5), mark
 * the rect dirty, and free the handle; else mark the whole screen dirty. Then flush_dirty_rects +
 * flip_video_page(0x103) [host_video_driver] + refresh_palette_dac, clear the cursor click flags, end the
 * draw batch, and set g_player_movement_enabled=5. Every callee is in another subsystem (video_display/das/
 * blit) and not yet lifted -> all BRIDGED via call_orig (re-point debt for when they lift). Reached single-
 * shot from the (original-bytes) dbase100_open_dialogue_window (inspect/splash). ABI_VOID; LIVE-SWAP ONLY. */
void present_cutscene_frame(void)
{
    regs_t io;
    if ((uint32_t)G32(VA_g_cutscene_image_handle) != 0) {                  /* cmp [g_cutscene_image_handle],0; jne */
        uint32_t base = *(uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_cutscene_image_handle));   /* edx = *handle */
        uint32_t pix  = base + 0x300;                   /* lea esi,[edx+0x300] (pixels after 768B palette) */
        /* remap_pixels_to_palette(src_pal=[85488], dst_pal=base, out=pix, stream=pix, count=[83c88]);
         * original also pushed a 2nd (dead) stack dword=1 the lifted body ignores (ret 8) — re-pointed */
        remap_pixels_to_palette((const uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_palette_rgb_ptr),
                                       (const uint8_t *)(uintptr_t)base,
                                       (uint8_t *)(uintptr_t)pix, (const uint8_t *)(uintptr_t)pix,
                                       (int32_t)(uint32_t)G32(VA_g_cutscene_image_handle + 0x14));
        /* blit_image_scaled_to_framebuffer(src=pix,x=[83c78],y=[83c7c],width=[83c80],rows=[83c84],mode=[83c8c],submode=[83c90]) — re-pointed */
        blit_image_scaled_to_framebuffer(pix, (uint32_t)G32(VA_g_cutscene_image_handle + 0x4), (uint32_t)G32(VA_g_cutscene_image_handle + 0x8),
                                                (uint32_t)G32(VA_g_cutscene_image_handle + 0xc), (int32_t)G32(VA_g_cutscene_image_handle + 0x10),
                                                (int32_t)G32(VA_g_cutscene_image_handle + 0x18), (int32_t)G32(VA_g_cutscene_image_handle + 0x1c));
        /* add_dirty_rect(x=[83c78], y=[83c7c], x+w-1, y+h-1) — re-pointed */
        add_dirty_rect((uint32_t)G32(VA_g_cutscene_image_handle + 0x4), (int32_t)G32(VA_g_cutscene_image_handle + 0x8),
                              (uint32_t)G32(VA_g_cutscene_image_handle + 0x4) + (uint32_t)G32(VA_g_cutscene_image_handle + 0xc) - 1,
                              (uint32_t)G32(VA_g_cutscene_image_handle + 0x8) + (uint32_t)G32(VA_g_cutscene_image_handle + 0x10) - 1);
        /* pool_free_handle(pool=[0x85c3c], handle=[0x83c74]) — re-pointed */
        pool_free_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle),
                                (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_cutscene_image_handle));
        G32(VA_g_cutscene_image_handle) = 0;                               /* g_cutscene_image_handle = 0 */
    } else {                                            /* 20b34: no image -> full-screen dirty */
        add_dirty_rect(0, 0, (uint32_t)G32(VA_g_screen_pitch) - 1, (uint32_t)G32(VA_g_screen_height) - 1);  /* full-screen — re-pointed */
    }
    /* common tail (20b4b) */
    flush_dirty_rects();                                                /* flush_dirty_rects — re-pointed */
    memset(&io, 0, sizeof io); io.va = 0x2e1e8u + OBJ_DELTA; io.eax = 0x103;   /* flip_video_page(0x103) [host_video_driver] */
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    host_flip_video_page(io.eax);  /* flip_video_page(0x103) -> the host present hook */
#endif
    refresh_palette_dac();                                              /* refresh_palette_dac — re-pointed */
    G8(VA_g_cursor_secondary_action_flag) = 0;                                    /* g_cursor_secondary_action_flag */
    G8(VA_g_cursor_primary_action_flag) = 0;                                    /* g_cursor_primary_action_flag */
    if ((uint32_t)G32(VA_g_screen_backup_handle + 0x4) != 0) {                  /* cmp [83d1c],0; je skip */
        G32(VA_g_screen_backup_handle + 0x4) = 0;
        end_screen_draw();   /* end_screen_draw — re-pointed */
    }
    G8(VA_g_player_movement_enabled) = 5;                                    /* g_player_movement_enabled = 5 */
}

/* exit_cutscene_overlay_mode (0x20905) — tear down the fullscreen cutscene/overlay (gated on
 * g_cutscene_overlay_active [0x83c70]): black the screen + present, restore the saved screen (slide-out
 * via play_screen_slide_out 0x20134 if a backup handle [0x83d18] exists, else just bump [0x7fe24]),
 * resume music (finalize_audio_sequence_ref + emit_music_sequence_event), restore g_player_movement_enabled
 * from [0x83d24], end the draw batch, free the image handle, and clear the cursor click flags. All callees
 * are in other subsystems (video_display/das/audio/transition) and not yet lifted -> BRIDGED via call_orig
 * (re-point debt); mem_fill 0x4b360 is lifted -> C. INTERACTIVE: play_screen_slide_out is a frame-tick
 * ([0x90bcc])-paced slide animation -> the interactive-lift surrogate pumps the tick so it progresses
 * (slide path is skipped when no screen was backed up, e.g. the INTRO). Reached single-shot from the
 * (original-bytes) finish_dialogue_record_eval at cutscene/overlay end. ABI_VOID; LIVE-SWAP ONLY. */
void exit_cutscene_overlay_mode(void)
{
    if ((uint32_t)G32(VA_g_cutscene_overlay_active) == 0) return;            /* cmp [g_cutscene_overlay_active],0; je ret */
    uint32_t pitch  = (uint32_t)G32(VA_g_screen_pitch);           /* g_screen_pitch */
    uint32_t height = (uint32_t)G32(VA_g_screen_height);           /* g_screen_height */
    uint32_t hdl    = (uint32_t)G32(VA_g_das_cache_heap_handle);           /* g_das_cache_heap_handle */
    G32(VA_g_cutscene_overlay_active) = 0;                                   /* g_cutscene_overlay_active = 0 */
    mem_fill((void *)(uintptr_t)(uint32_t)G32(VA_g_framebuffer_ptr), 0, pitch * height);  /* clear back buffer */
    G16(VA_g_current_vesa_bank) = (int16_t)0x22b8;                     /* g_current_vesa_bank = 0x22b8 */
    regs_t io;
    add_dirty_rect(0, 0, pitch - 1, height - 1);   /* add_dirty_rect — re-pointed */
    flush_dirty_rects();                           /* flush_dirty_rects — re-pointed */
    memset(&io, 0, sizeof io); io.va = 0x2e1e8u + OBJ_DELTA; io.eax = 0x103;   /* flip_video_page(0x103) */
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    host_flip_video_page(0x103);   /* overlay-exit present — the aborted --skip-gdv
                                    * intros leave 0x83c70 armed; finish_dialogue_record_eval tears the
                                    * overlay down at menu bring-up */
#endif
    refresh_palette_dac_wrapper();                 /* refresh_palette_dac_wrapper — re-pointed */
    if ((uint32_t)G32(VA_g_dialogue_busy_flag + 0x36) != 0) {                  /* cmp [83b20],0; je else */
        pool_free_handle((uint32_t *)(uintptr_t)hdl, (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_screen_backup_handle));  /* re-pointed */
        G32(VA_g_screen_backup_handle) = 0; G32(VA_g_dialogue_busy_flag + 0x36) = 0;
    } else if ((uint32_t)G32(VA_g_map_first_load_flag) != 0 || (uint32_t)G32(VA_g_screen_backup_handle + 0x8) != 0) {  /* g_map_first_load_flag || [83d20] */
        pool_free_handle((uint32_t *)(uintptr_t)hdl, (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_screen_backup_handle));  /* re-pointed */
        G32(VA_g_screen_backup_handle) = 0;
    }
    if ((uint32_t)G32(VA_g_screen_backup_handle) != 0) {                  /* cmp [83d18],0; je no-slide */
        play_screen_slide_out((uint32_t)G32(VA_g_screen_backup_handle));  /* play_screen_slide_out (INTERACTIVE) — re-pointed */
        pool_free_handle((uint32_t *)(uintptr_t)hdl, (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_screen_backup_handle));  /* re-pointed */
        G32(VA_g_screen_backup_handle) = 0;
    } else {
        G32(VA_g_pending_fire_aim + 0x14) = (int32_t)((uint32_t)G32(VA_g_pending_fire_aim + 0x14) + 1);   /* inc [0x7fe24] */
    }
    if ((uint32_t)G32(VA_g_dialogue_busy_flag + 0x162) != 0) {                  /* cmp [83c4c],0; je skip-music */
        G32(VA_g_dialogue_busy_flag + 0x162) = 0;
        finalize_audio_sequence_ref();                        /* finalize_audio_sequence_ref — re-pointed */
        emit_music_sequence_event((uint8_t)G8(VA_g_font_descriptor + 0x212));      /* emit_music_sequence_event(byte[71124]) — re-pointed */
    }
    G8(VA_g_player_movement_enabled) = 5;                                    /* g_player_movement_enabled = 5 */
    G8(VA_g_player_movement_enabled) = (int8_t)G8(VA_g_saved_movement_enabled);                  /* ...then restore from [0x83d24] (saved) */
    if ((uint32_t)G32(VA_g_screen_backup_handle + 0x4) != 0) {                  /* cmp [83d1c],0; je skip */
        G32(VA_g_screen_backup_handle + 0x4) = 0;
        end_screen_draw();   /* end_screen_draw — re-pointed */
    }
    if ((uint32_t)G32(VA_g_cutscene_image_handle) != 0) {                  /* cmp [83c74],0; je skip */
        pool_free_handle((uint32_t *)(uintptr_t)hdl, (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_cutscene_image_handle));  /* re-pointed */
        G32(VA_g_cutscene_image_handle) = 0;
    }
    G8(VA_g_cursor_secondary_action_flag) = 0;                                    /* g_cursor_secondary_action_flag */
    uint16_t tick = (uint16_t)G16(VA_g_frame_tick_counter);             /* ax = word [g_frame_tick_counter] */
    G8(VA_g_cursor_primary_action_flag) = 0;                                    /* g_cursor_primary_action_flag */
    G16(VA_g_last_frame_tick) = (int16_t)tick;                       /* [0x85320] = word[0x90bcc] */
}

/* show_cutscene_error_box (0x26aef) — on a GDV load failure, categorize the error by the failed file's
 * basename and pop a message box. path_basename 0x150fa (EAX=param_1 = the failing path) -> the word;
 * find_gdv_error_index 0x26a20 [LIFTED] categorizes it to 0..3 (>=4 -> return 0, no box); close_voice_file
 * 0x1e774; pick g_gdv_error_strings[idx] ([0x71d46]); show_message_box 0x2508f (the blocking menu box) on
 * &[0x71d56]/flags 0x1200. Returns 1 (+ records idx in [0x83eac]) iff the box returned 1, else 0. EDX/EBX
 * are saved/restored (not inputs). ABI_EAX. INTERACTIVE: show_message_box blocks on input — same proven
 * pattern as prompt_save_overwrite 0x26349. show_message_box's ECX arg is exactly what close_voice_file
 * leaves, so it's captured from that bridge. All sub-calls in other subsystems -> BRIDGED (re-point debt).
 * LIVE-SWAP ONLY. Validate: rename a needed .GDV so the load fails -> the error box must show + dismiss. */
uint32_t show_cutscene_error_box(uint32_t param_1)
{
    uint32_t bn = path_basename((const uint8_t *)(uintptr_t)param_1);  /* path_basename(param_1) — re-pointed */
    uint32_t index = find_gdv_error_index((const uint8_t *)(uintptr_t)bn);  /* find_gdv_error_index(EAX) */
    if (index >= 4) return 0;                           /* cmp eax,4; jae -> return 0 */
    close_voice_file();                          /* close_voice_file — re-pointed. (Original captured its
                                                         * leftover ECX into show_message_box's ECX, but the callee
                                                         * saves+ignores EBX/ECX — reads only EAX/EDX — so it is dead.) */
    uint32_t msg = (uint32_t)G32(VA_g_gdv_error_strings + index * 4);  /* g_gdv_error_strings[index] (ptr) */
    G32(VA_g_test_sfx_descriptor + 0xc) = (int32_t)msg;                        /* [0x71d56] = message ptr */
    /* show_message_box(&[71d56], 0x1200); original also set EBX=idx/ECX=leftover, both callee-ignored — re-pointed */
    if (show_message_box(0x71d56u + OBJ_DELTA, 0x1200u) == 1) {  /* cmp eax,1; jne -> return 0 */
        G32(VA_g_playback_menu_scroll_anchor + 0x8) = (int32_t)index;                  /* [0x83eac] = index */
        return 1;
    }
    return 0;
}

/* ============================================================================================
 * The GDV decode-pump TIMER ISR bodies (carved these as engine; the int-frame
 * boilerplate — pusha/segs/EOI/iret/retf + the original-vector chain via FUN_436cc — is host_timer_driver's
 * job, so these are the frameless BODIES). They have NO call-site (reached only via the installed int
 * vector), so the host timer driver calls them; in the recomp host that driver is the traps.c shm_tick
 * surrogate (g_gdv_loop_hosting / g_os_interactive). VALIDATED by cutscene playback (no-audio = the
 * inventory popup exercises 0x4e24b; timer = a [0x91d0c]&2 cutscene). Decode = lifted codec + advance C.
 * ============================================================================================ */

/* gdv_advance_decode_pump (0x4e310, 132B, near subroutine) — the decode-pacing core the audio timer ISR
 * (0x4e60b) calls when [0x91d0c]&2. Pace via the [0x91db0]/[0x91dbe] budget; on underflow decode one chunk
 * (unless a guard refills/resets) + advance the decoder head [0x91d68]. Does NOT touch [0x91dbc]/[0x91884]
 * (those bumps live in the ISR wrapper 0x4e60b). The [0x91de0]!=0 reentrancy case jumps to 0x4e2d4's
 * [0x91db0]=0 (the [0x91884]/retf there is the ISR frame). Faithful. */
void gdv_advance_decode_pump(void)
{
    int16_t db0 = (int16_t)((int16_t)G16(VA_g_gdv_audio_stream_base + 0x60) - (int16_t)G16(VA_g_gdv_audio_stream_base + 0x6e));
    G16(VA_g_gdv_audio_stream_base + 0x60) = (uint16_t)db0;
    if (db0 >= 0) return;                                              /* 4e31d jns ret */
    if ((uint32_t)G32(VA_g_gdv_stream_flags) & 4) { G16(VA_g_gdv_audio_stream_base + 0x60) += 0x3c00; return; }   /* 4e31f refill */
    if ((uint8_t)G8(VA_g_gdv_audio_format + 0xe) == 0) { G16(VA_g_gdv_audio_stream_base + 0x60) = 0; return; }       /* 4e32b reset0 */
    if ((uint8_t)G8(VA_g_gdv_audio_enabled) != 0) { G16(VA_g_gdv_audio_stream_base + 0x60) += 0x3c00; return; }    /* 4e334 refill */
    if ((int16_t)G16(VA_g_gdv_audio_stream_base + 0x68) < 0) { G16(VA_g_gdv_audio_stream_base + 0x60) += 0x3c00; return; }    /* 4e33d refill */
    if ((uint8_t)G8(VA_g_gdv_end_of_stream + 0x2) != 0) { G16(VA_g_gdv_audio_stream_base + 0x60) = 0; return; }       /* 4e347 reentrant -> 0x4e2d4 reset0 */
    {
        uint32_t d60 = (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x10);
        if (d60 != 0xffffffffu && d60 == (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x1c)) { G16(VA_g_gdv_audio_stream_base + 0x60) = 0; return; }  /* 4e35a caught up */
    }
    G8(VA_g_gdv_end_of_stream + 0x2)++;                       /* 4e362 reentrancy guard */
    gdv_decode_video_chunk();     /* 4e368 codec 0x4d384 */
    gdv_advance_chunk_ptr();      /* 4e36d emit + advance head 0x4dd33 */
    G16(VA_g_gdv_audio_stream_base + 0x6a)--;                      /* 4e372 frame counter */
    G8(VA_g_gdv_end_of_stream + 0x2)--;                       /* 4e379 */
}

/* gdv_decode_timer_isr (0x4e60b body) — the int-8 GDV timer ISR (audio path). Drives the pump when
 * [0x91d0c]&2, then bumps the fade accumulator [0x91dbc] (+0xa00) + the read-pacing budget [0x91884].
 * The [0x91d24] divider + original-vector chain (FUN_436cc) + PIC EOI + pusha/iret are host_timer_driver's
 * job (omitted). Faithful frameless body. */
void gdv_decode_timer_isr(void)
{
    if ((uint32_t)G32(VA_g_gdv_stream_flags) & 2) gdv_advance_decode_pump();  /* 4e622 */
    G16(VA_g_gdv_audio_stream_base + 0x6c) += 0xa00;                                            /* 4e662 fade accumulator (audio rate) */
    G32(VA_g_particle_pool + 0x20) += (int32_t)G32(VA_g_particle_pool + 0x1c);                           /* 4e66b read-pacing budget */
}

/* gdv_decode_timer_isr_noaudio (0x4e24b body) — the no-audio SOS timer ISR (the silent-cutscene / inventory
 * popup pacer). Bumps the fade accumulator [0x91dbc] (+0xbaa), then the [0x91db0]/[0x91dbe] budget gated on
 * the FRAME counter [0x91dba] (not the audio position); on a slot decode one chunk (refill [0x91db0] first
 * per 0x4e2aa) + advance the head; tail bumps the read budget [0x91884]. Faithful frameless body
 * (push/ds/retf = host frame). decode_subframe 0x4dceb = codec + emit/advance. */
void gdv_decode_timer_isr_noaudio(void)
{
    G16(VA_g_gdv_audio_stream_base + 0x6c) += 0xbaa;                                            /* 4e255 */
    int16_t db0 = (int16_t)((int16_t)G16(VA_g_gdv_audio_stream_base + 0x60) - (int16_t)G16(VA_g_gdv_audio_stream_base + 0x6e));
    G16(VA_g_gdv_audio_stream_base + 0x60) = (uint16_t)db0;                                     /* 4e264 */
    if (db0 >= 0) goto tail;                                          /* 4e26b jns */
    if ((uint8_t)G8(VA_g_gdv_audio_format + 0xe) == 0) { G16(VA_g_gdv_audio_stream_base + 0x60) = 0; goto tail; }   /* 4e26d reset0 (0x4e2d4) */
    if ((uint8_t)G8(VA_g_gdv_audio_enabled) != 0) { G16(VA_g_gdv_audio_stream_base + 0x60) += 0x3c00; goto tail; }  /* 4e276 refill (0x4e2c9) */
    if ((int16_t)G16(VA_g_gdv_audio_stream_base + 0x6a) < 0) { G16(VA_g_gdv_audio_stream_base + 0x60) += 0x3c00; goto tail; }  /* 4e27f refill */
    if ((uint8_t)G8(VA_g_gdv_end_of_stream + 0x2) != 0) { G16(VA_g_gdv_audio_stream_base + 0x60) = 0; goto tail; }   /* 4e289 reset0 */
    {
        uint32_t d60 = (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x10);
        if (d60 != 0xffffffffu && d60 == (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x1c)) { G16(VA_g_gdv_audio_stream_base + 0x60) = 0; goto tail; }  /* 4e29c caught up */
    }
    G8(VA_g_gdv_end_of_stream + 0x2)++;                       /* 4e2a4 */
    G16(VA_g_gdv_audio_stream_base + 0x60) += 0x3c00;              /* 4e2aa refill before decode */
    gdv_decode_video_chunk();     /* 4e2b4 0x4dceb = codec 0x4d384 */
    gdv_advance_chunk_ptr();      /*               + emit/advance 0x4dd33 */
    G16(VA_g_gdv_audio_stream_base + 0x6a)--;                      /* 4e2ba */
    G8(VA_g_gdv_end_of_stream + 0x2)--;                       /* 4e2c1 */
tail:
    G32(VA_g_particle_pool + 0x20) += (int32_t)G32(VA_g_particle_pool + 0x1c);   /* 4e2dd read-pacing budget */
}

/* gdv_tick_timer_isr (0x4e2ed body) — the no-decode tick ISR variant: bump the fade accumulator [0x91dbc]
 * (+0x1400) + the read-pacing budget [0x91884]. This is the general per-tick pump the interactive-lift
 * stand-in uses for the fade (gdv_fade_in/out spin on [0x91dbc]) + read (gdv_read_frame_chunk spins on
 * [0x91884]) lifts. Faithful frameless body (push eax/ds/retf = host frame). */
void gdv_tick_timer_isr(void)
{
    G16(VA_g_gdv_audio_stream_base + 0x6c) += 0x1400;                                          /* 4e2f7 */
    G32(VA_g_particle_pool + 0x20) += (int32_t)G32(VA_g_particle_pool + 0x1c);                           /* 4e305 */
}

/* play_gdv_cutscene (0x2059d, 872B) — the top-level "play a named GDV" entry. Builds a 0x98-byte GDV
 * descriptor on the stack (from the template at 0x83c98 + globals), allocs a ~1.2MB (0x12c000) streaming
 * decode buffer from the DAS cache, opens the container (with an error-0x20 retry loop that pops the
 * cutscene error box), drains input, plays via gdv_decode_frame, then (if a still-frame snapshot was
 * requested, [0x83b24]!=0) scales + caches the final frame into a right-sized DAS handle. Faithful
 * transcription guided by the decompiled C (ghidra_export/decomp/2059d_play_gdv_cutscene.c) — the decomp
 * resolves the pointer-vs-flag immediates: desc[0x14]=&DAT_7f4e4 and desc[0x1c]=run_timed_message_sequence
 * 0x1fce2 are POINTERS (rebased +OBJ_DELTA); 0x48980/0x8190 are FLAG words (no rebase). decoder_open sets
 * [0x91d14]=&desc, so the host-stack struct drives the whole lifted pipeline (valid while this C frame is
 * live). GDV-pipeline callees run as direct C (lifted); cross-subsystem ones (DAS cache / dirty-rect /
 * video flip+palette / input drain) are call_orig BRIDGES with the exact register state. ABI_EAX4
 * (EAX=name/path, EBX=opt record ptr; EDX/ECX unused). Args via Watcom: EAX, EBX. INTERACTIVE (decode_frame
 * + the error box block). LIVE-SWAP ONLY. */
uint32_t play_gdv_cutscene(uint32_t eax_in, uint32_t edx_in, uint32_t ebx_in, uint32_t ecx_in)
{
    (void)edx_in; (void)ecx_in;
    uint32_t param_1   = eax_in;     /* [ebp-0x1a] saved arg EAX (name/path) */
    uint32_t unaff_ebx = ebx_in;     /* [ebp-0x1e] saved arg EBX (optional record ptr) */
    uint8_t  desc[0x98];             /* the descriptor; &desc = lea [ebp-0x16] (ctx passed to callees) */
    uint32_t ctxp = (uint32_t)(uintptr_t)desc;
    regs_t io;

    memcpy(desc, (const void *)(uintptr_t)(0x83c98u + OBJ_DELTA), 0x78);  /* 205b1: rep movs 0x1e dwords (template) */
    G32(VA_g_dialogue_busy_flag + 0x5a) = 0;                                                     /* 205bb */
    G8(VA_g_mouse_relative_mode)  = 0;                                                     /* 205c5 g_mouse_relative_mode */
    if ((uint32_t)G32(VA_g_cutscene_image_handle) != 0) {                                   /* 205cc: free old g_cutscene_image_handle */
        pool_free_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle),
                                (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_cutscene_image_handle));
        G32(VA_g_cutscene_image_handle) = 0;
    }
    /* das_cache_make_room(0x12c000); callee preserves EDX (never writes it), so the pool_alloc size that
     * the original read from EDX stays 0x12c000 — re-pointed */
    das_cache_make_room(0x12c000u);
    uint32_t handle = pool_alloc_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle), (int32_t)0x12c000u);
    *(uint32_t *)(desc + 0x94) = handle;                                 /* 20608: [ebp+0x7e] = handle */

    if (handle != 0) {                                                   /* 2060d: je 0x208ec (alloc fail) */
        *(uint32_t *)(desc + 0x00) = param_1;                            /* 20616: [ebp-0x16] = param_1 */
        *(uint32_t *)(desc + 0x08) = *(uint32_t *)(uintptr_t)handle;     /* 20619: [ebp-0xe] = *handle (buffer) */
        *(uint32_t *)(desc + 0x0c) = 0x12c000u;                          /* 20623: [ebp-0xa] */
        *(uint32_t *)(desc + 0x10) = (uint32_t)G32(VA_g_sos_digital_device);             /* 2062a: [ebp-0x6] = g_sos_digital_device */
        uint32_t flags = ((uint8_t)G8(VA_g_player_movement_enabled + 0x10) == 0) ? 0x48980u : 0x8190u;  /* 2062d..2063f */
        *(uint32_t *)(desc + 0x30) = (uint32_t)G32(VA_g_sos_digital_device + 0x6c);             /* 2064b: [ebp+0x1a] */
        *(uint32_t *)(desc + 0x14) = 0x7f4e4u + OBJ_DELTA;               /* 20653: [ebp-0x2] = &DAT_7f4e4 (PTR) */
        *(uint32_t *)(desc + 0x34) = (uint32_t)G32(VA_g_audio_signed_samples + 0x4);             /* 2065a: [ebp+0x1e] */
        *(uint32_t *)(desc + 0x1c) = 0x1fce2u + OBJ_DELTA;               /* 20662: [ebp+0x6] = run_timed_message_sequence (CODE PTR) */
        *(uint32_t *)(desc + 0x3c) = (uint32_t)G32(VA_g_audio_signed_samples + 0xc);             /* 20669: [ebp+0x26] */
        G32(VA_g_dialogue_busy_flag + 0x4e) = 0;                                                /* 20671 */
        *(uint32_t *)(desc + 0x38) = (uint32_t)G32(VA_g_audio_signed_samples + 0x8);             /* 2067b: [ebp+0x22] */
        if ((uint32_t)G32(VA_g_sos_digital_device + 0x6c) == 0) flags |= 0x10u;                 /* 2067e: [ebp+0x1a]==0 */
        if ((uint32_t)G32(VA_g_voice_decode_suspended + 0x8) != 0) flags |= 0x10u;                 /* 20688 */
        if ((uint32_t)G32(VA_g_choice_selected_index + 0x18) < 0x7f00u) {                          /* 20695 */
            if ((uint32_t)G32(VA_g_choice_selected_index + 0x18) == 0) *(uint32_t *)(desc + 0x64) = 1;             /* 206aa: [ebp+0x4e]=1 */
            else *(uint32_t *)(desc + 0x64) = (uint32_t)G32(VA_g_choice_selected_index + 0x18);                    /* 206b8: [ebp+0x4e] */
        }
        G32(VA_g_dialogue_busy_flag + 0x3a) = 0;                                                /* 206bb */
        G32(VA_g_dialogue_busy_flag + 0x4a) = 0;                                                /* 206c5 */
        if ((uint32_t)G32(VA_g_screen_height) == 0x12cu) flags |= 0x800000u;        /* 206cf: g_screen_height==300 */
        *(uint32_t *)(desc + 0x18) = flags;                              /* [ebp+0x2] = flags */
        obj_counter12_inc((uint32_t)G32(VA_g_das_cache_heap_handle));                /* 206e4 */

        uint32_t iVar2, retry;
        do {
            init_gdv_video_context(ctxp);                         /* 206ec */
            iVar2 = gdv_decoder_open(ctxp);                       /* 206f1 -> result */
            if (iVar2 == 0 || iVar2 != 0x20) break;                      /* 206f8/206fc: break unless retryable */
            if ((uint32_t)G32(VA_g_cutscene_image_handle + 0x20) != 0) {                           /* 20701: video reset before retry */
                uint32_t ecx_a = (uint32_t)G32(VA_g_screen_height) - 1u;            /* g_screen_height-1 */
                uint32_t ebx_a = (uint32_t)G32(VA_g_screen_pitch) - 1u;            /* g_screen_pitch-1 */
                G32(VA_g_cutscene_image_handle + 0x20) = 0;                                        /* 2071c */
                add_dirty_rect(0, 0, ebx_a, ecx_a);   /* add_dirty_rect — re-pointed */
                flush_dirty_rects();                  /* flush_dirty_rects — re-pointed */
                memset(&io, 0, sizeof io); io.va = 0x2e1e8u + OBJ_DELTA; io.eax = 0x103u;   /* flip_video_page(0x103) */
#ifndef ROTH_STANDALONE
                call_orig(&io);
#else
                host_flip_video_page(io.eax);  /* flip_video_page(0x103) -> the host present hook */
#endif
                refresh_palette_dac();                /* refresh_palette_dac — re-pointed */
            }
            obj_counter12_dec((uint32_t)G32(VA_g_das_cache_heap_handle));            /* 20744 */
            retry = show_cutscene_error_box(param_1);             /* 2074c -> retry flag (edx) */
            obj_counter12_inc((uint32_t)G32(VA_g_das_cache_heap_handle));            /* 20758 */
        } while (retry != 0);                                            /* 2075d: jne 0x206e9 */

        if (unaff_ebx == 0) G32(VA_g_dialogue_busy_flag + 0x46) = 0;                            /* 20761/20773 */
        else G32(VA_g_dialogue_busy_flag + 0x46) = *(int32_t *)(uintptr_t)unaff_ebx;            /* 20767: *unaff_EBX */

        if (iVar2 == 0) {                                                /* 2077d: open succeeded */
            G32(VA_g_dialogue_busy_flag + 0x32) = (int32_t)iVar2;                              /* 20781 (=0) */
            drain_input_and_clear_clicks();  /* drain_input_and_clear_clicks — re-pointed */
            G32(VA_g_cutscene_image_handle + 0x20) = 1;                                            /* 2078f */
            gdv_decode_frame(ctxp);                              /* 20799: play (input ignored) */
            if ((uint32_t)G32(VA_g_dialogue_busy_flag + 0x5a) != 0) clear_cutscene_region();  /* 2079e/207a7 */
        }
        obj_counter12_dec((uint32_t)G32(VA_g_das_cache_heap_handle));               /* 207b1 */
        gdv_decoder_close();                                     /* 207b9 (reads globals; eax=ctx vestigial) */
        swap_cutscene_display_buffers(ctxp);                     /* 207c1 */

        if ((uint32_t)G32(VA_g_dialogue_busy_flag + 0x3a) != 0) {                              /* 207c6: snapshot requested */
            uint32_t r = compute_ratio_4c296((uint32_t *)(desc + 0x78));  /* 207d6: &[ebp+0x62] */
            if (r != 0) {                                               /* 207db */
                uint32_t base  = *(uint32_t *)(uintptr_t)handle;        /* *local_14 (decode buffer base) */
                uint32_t ecx_v = (uint16_t)*(uint16_t *)(desc + 0x60);  /* local_48 [ebp+0x4a] */
                uint32_t edx_v = (uint16_t)*(uint16_t *)(desc + 0x62);  /* local_46 [ebp+0x4c] */
                int32_t  ebx_v = (int32_t)(edx_v * ecx_v);              /* iVar2 = local_46*local_48 */
                uint32_t esi_v = 2, ax_v = 2;                           /* DAT_83c8c / DAT_83c90 seeds */
                if ((uint8_t)G8(VA_g_hires_line_doubling_flag) == 0) { ax_v = 1; edx_v = (uint32_t)((int32_t)edx_v >> 1); }  /* !hires */
                if ((uint32_t)G32(VA_g_screen_pitch) == 0x280u) { ax_v += ax_v; edx_v += edx_v; ecx_v += ecx_v; ebx_v += ebx_v; }  /* pitch */
                if ((uint8_t)G8(VA_g_dialogue_busy_flag + 0x42) & 1) { ebx_v >>= 1; esi_v += esi_v; }   /* 2083e */
                if ((uint8_t)G8(VA_g_dialogue_busy_flag + 0x42) & 2) { ebx_v >>= 1; ax_v += ax_v; }     /* 2084b */
                G32(VA_g_cutscene_image_handle + 0xc) = (int32_t)ecx_v;                          /* 2085b */
                G32(VA_g_cutscene_image_handle + 0x10) = (int32_t)edx_v;                          /* 20861 */
                G32(VA_g_cutscene_image_handle + 0x18) = (int32_t)esi_v;                          /* 20867 */
                G32(VA_g_cutscene_image_handle + 0x1c) = (int32_t)ax_v;                           /* 2086d */
                G32(VA_g_cutscene_image_handle + 0x14) = ebx_v;                                   /* 20872 */
                G32(VA_g_cutscene_image_handle + 0x4) = *(int32_t *)(desc + 0x78);               /* 20878: local_30 */
                G32(VA_g_cutscene_image_handle + 0x8) = *(int32_t *)(desc + 0x7c);               /* 20889: local_2c */
                /* ensure_das_cache_heap_space(iVar2+0x300); callee preserves EDX so pool_alloc's size = need — re-pointed */
                uint32_t need2 = (uint32_t)(ebx_v + 0x300);
                ensure_das_cache_heap_space(need2);
                uint32_t nh = pool_alloc_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle), (int32_t)need2);
                G32(VA_g_cutscene_image_handle) = (int32_t)nh;                             /* 2089e: g_cutscene_image_handle */
                if (nh != 0) {                                          /* 208a5 */
                    uint8_t *dst = (uint8_t *)(uintptr_t)*(uint32_t *)(uintptr_t)nh;  /* *new_handle */
                    uint8_t *src_pal = (uint8_t *)(uintptr_t)((uint32_t)G32(VA_g_dialogue_busy_flag + 0x3e) - base + base); /* [0x83b28] */
                    uint8_t *src_frm = (uint8_t *)(uintptr_t)((uint32_t)G32(VA_g_dialogue_busy_flag + 0x3a) - base + base); /* [0x83b24] */
                    memcpy(dst, src_pal, 0x300);                        /* 208c3: palette */
                    memcpy(dst + 0x300, src_frm, (size_t)(uint32_t)ebx_v);  /* 208d2: scaled frame */
                }
            }
        }
        pool_free_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle), (uint32_t *)(uintptr_t)handle);  /* 208dd */
        G32(VA_g_cutscene_overlay_active) = 1;                                               /* 208e2 g_cutscene_overlay_active */
    }
    G8(VA_g_cursor_secondary_action_flag) = 0;                                                    /* 208ec g_cursor_secondary_action_flag */
    G8(VA_g_cursor_primary_action_flag) = 0;                                                    /* 208f3 g_cursor_primary_action_flag */
    return 0;                                                           /* void (callers ignore EAX) */
}

/* play_record_gdv_cutscene (0x20c16, 642B) — play the FMV cutscene named by a story/dialogue record.
 * EAX=record ptr. Registers the record in the playback gallery (rec[0x13]==0 -> stamp the next index into
 * rec+0x10 high byte + bump g_cutscenes_seen_count), loads the record's caption/voice resource (24-bit
 * offset @rec+0x10, size @rec+0xa) into g_message_resource_handle [0x83d28], backs the framebuffer up into
 * g_screen_backup_handle [0x83d18] + slides it in, builds '<g_dir_gdv><name>.GDV', then calls the lifted
 * play_gdv_cutscene. Faithful transcription guided by the decompile (ghidra_export/decomp/20c16_*.c):
 * &g_dir_gdv 0x764f0 is a POINTER (rebased). pool_alloc/free + play_gdv_cutscene = direct C; the file-I/O
 * (dos_lseek/dos_read_items via int 0x21) + cursor/screen/path bridges = call_orig with exact registers.
 * Original tail `jmp 0x1fcdc` is the shared pop-regs epilogue (handled by the C return). ABI_EAX
 * (EAX=record -> EAX 0 skipped / 1 played). INTERACTIVE (play_screen_slide_in + play_gdv_cutscene block).
 * PER-NAME (ROTH_LIFT=play_record_gdv_cutscene), NOT is_gdv. LIVE-SWAP ONLY. */
uint32_t play_record_gdv_cutscene(uint32_t eax_in)
{
    uint32_t param_1 = eax_in;            /* [ebp-0x4e] record ptr */
    uint8_t  pathbuf[0x100];              /* [ebp-0x4a] full path (build_game_path dest; play_gdv_cutscene EAX) */
    uint8_t  namebuf[0x20];               /* [ebp+0x7e] name buffer (template + record name + .GDV) */
    regs_t   io;

    memcpy(namebuf, (const void *)(uintptr_t)(0x83d2cu + OBJ_DELTA), 16);   /* 20c26: 4-dword template */
    G8(VA_g_mouse_relative_mode) = 0;                                                        /* 20c32 g_mouse_relative_mode */
    if (param_1 == 0) return 0;                                            /* 20c39 */
    if (*(uint8_t *)(uintptr_t)param_1 == 0) return 0;                     /* 20c41: empty name */

    G32(VA_g_timed_message_timer) = 0;                                                      /* 20c4d g_timed_message_timer */
    if ((uint32_t)G32(VA_g_message_resource_handle) != 0)                                       /* 20c57: free old resource handle */
        G32(VA_g_message_resource_handle) = (int32_t)pool_free_handle(
            (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle),
            (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_message_resource_handle));

    if ((uint32_t)G32(VA_g_choice_selected_index + 0x14) != 0 && (uint32_t)G32(VA_g_dialogue_script_handle) != 0) {       /* 20c75/20c82 */
        if (*(uint8_t *)(uintptr_t)(param_1 + 0x13) == 0) {                /* 20c8f: not yet in gallery */
            uint32_t seen = (uint32_t)G32(VA_g_cutscenes_seen_count);
            *(uint32_t *)(uintptr_t)(param_1 + 0x10) |= (seen << 0x18);    /* stamp playback index */
            G32(VA_g_cutscenes_seen_count) = (int32_t)(seen + 1);                            /* g_cutscenes_seen_count++ */
        }
        if (*(int16_t *)(uintptr_t)(param_1 + 0xa) != 0 && (uint32_t)G32(VA_g_voice_decode_suspended + 0x4) == 0) {  /* 20caf/20cb6 */
            int32_t rsize = (int32_t)*(int16_t *)(uintptr_t)(param_1 + 0xa);               /* movsx size */
            das_cache_make_room((uint32_t)rsize);  /* das_cache_make_room (EDX=param_1 unused by callee) — re-pointed */
            G32(VA_g_message_resource_handle) = (int32_t)pool_alloc_handle(
                (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle), rsize);     /* 20cd4 */
            uint32_t ofs = *(uint32_t *)(uintptr_t)(param_1 + 0x10) & 0xffffffu;           /* 24-bit offset */
            memset(&io, 0, sizeof io); io.va = 0x41b9au + OBJ_DELTA; io.eax = (uint32_t)G32(VA_g_dialogue_script_handle); io.edx = ofs; io.ebx = 0;  /* dos_lseek */
#ifndef ROTH_STANDALONE
            call_orig(&io);
#else
            dos_lseek((uint32_t)G32(VA_g_dialogue_script_handle), ofs, 0);   /* caption resource seek (host DOS layer) — boot intro caption load */
#endif
            uint32_t buf = *(uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_message_resource_handle);                 /* *resource_handle */
            memset(&io, 0, sizeof io); io.va = 0x41b53u + OBJ_DELTA;        /* dos_read_items(buf,1,size,handle) */
            io.eax = buf; io.edx = 1; io.ebx = (uint32_t)rsize; io.ecx = (uint32_t)G32(VA_g_dialogue_script_handle);
#ifndef ROTH_STANDALONE
            call_orig(&io);
#else
            dos_read_items(buf, 1, (uint32_t)rsize, (uint32_t)G32(VA_g_dialogue_script_handle));   /* caption resource read (host DOS layer) — boot intro caption load */
#endif
        }
    }
    if ((uint32_t)G32(VA_g_screen_backup_handle + 0x4) == 0) {                                     /* 20d14 */
        set_cursor_shape(8);   /* set_cursor_shape(8) — re-pointed */
        force_cursor_redraw(); /* force_cursor_redraw — re-pointed */
    }
    if ((uint32_t)G32(VA_g_damage_flash_level) != 0) {                                     /* 20d2c g_damage_flash_level */
        G32(VA_g_damage_flash_level) = 0;
        refresh_palette_dac();   /* refresh_palette_dac — re-pointed */
    }
    G32(VA_g_prev_dirty_rect_count) = 0;                                                      /* 20d44 g_prev_dirty_rect_count */
    G32(VA_g_dirty_rect_count) = 0;                                                      /* 20d4e g_dirty_rect_count */
    if ((uint32_t)G32(VA_g_dialogue_busy_flag) == 0 && (uint32_t)G32(VA_g_screen_backup_handle) == 0) {       /* 20d58/20d65: no backup yet */
        if ((uint32_t)G32(VA_g_screen_backup_handle + 0x4) == 0)                                   /* 20d72 */
            G32(VA_g_saved_movement_enabled) = (uint8_t)G8(VA_g_player_movement_enabled);                           /* save g_player_movement_enabled */
        G32(VA_g_cutscene_image_handle + 0x20) = 0;                                                  /* 20d87 */
        if ((uint32_t)G32(VA_g_dialogue_busy_flag + 0x36) != 0x4d2u) {                            /* 20d91 */
            uint32_t fbsize = (uint32_t)G32(VA_g_screen_pitch) * (uint32_t)G32(VA_g_screen_height);  /* pitch*height */
            G32(VA_g_screen_backup_handle + 0x8) = 0;                                              /* 20dac */
            das_cache_make_room(fbsize);  /* das_cache_make_room — re-pointed */
            uint32_t bh = pool_alloc_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle), (int32_t)fbsize);
            G32(VA_g_screen_backup_handle) = (int32_t)bh;                                    /* g_screen_backup_handle */
            if (bh != 0) {                                                 /* 20dce */
                memcpy((void *)(uintptr_t)*(uint32_t *)(uintptr_t)bh,
                       (const void *)(uintptr_t)(uint32_t)G32(VA_g_framebuffer_ptr), fbsize);  /* fb -> backup */
                play_screen_slide_in(1, (uint32_t)G32(VA_g_screen_backup_handle));  /* play_screen_slide_in(1,handle) — re-pointed */
            }
        }
    }
    flush_object_das_handles();    /* 20dec flush_object_das_handles — re-pointed */
    memcpy(namebuf, (const void *)(uintptr_t)param_1, 8);                      /* 20df6: record name (8 bytes) */
    /* set_filename_extension(&name,'GDV'); original also set EBX=&name (callee zeroes EBX at entry, so unused) — re-pointed */
    set_filename_extension((char *)namebuf, 0x564447u);
    /* build_game_path(&path, &g_dir_gdv[0x764f0 rebased ptr], &name) — re-pointed */
    build_game_path(pathbuf, (const uint8_t *)(uintptr_t)(0x764f0u + OBJ_DELTA), namebuf);
    if ((uint32_t)G32(VA_g_screen_backup_handle + 0x4) == 0) {                                         /* 20e1d */
        G32(VA_g_screen_backup_handle + 0x4) = 1;
        begin_screen_draw();  /* begin_screen_draw — re-pointed */
    }
    G32(VA_g_cutscene_image_handle + 0x9c) = (int32_t)G32(VA_g_mouse_x);                                      /* 20e35 g_mouse_x */
    uint32_t rec_ebx = (uint32_t)G32(VA_g_message_resource_handle);                                 /* 20e3a: play_gdv_cutscene's EBX */
    G32(VA_g_cutscene_image_handle + 0xa0) = (int32_t)G32(VA_g_mouse_y);                                      /* 20e45 g_mouse_y */
    G8(VA_g_player_movement_enabled)  = 4;                                                          /* 20e54 g_player_movement_enabled */
    play_gdv_cutscene((uint32_t)(uintptr_t)pathbuf, 0, rec_ebx, 0);     /* 20e5b */
    if ((uint32_t)G32(VA_g_message_resource_handle) != 0)                                          /* 20e60: free resource handle */
        G32(VA_g_message_resource_handle) = (int32_t)pool_free_handle(
            (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle),
            (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_message_resource_handle));
    G16(VA_g_current_vesa_bank) = (uint16_t)0x22b8;                                          /* 20e7e g_current_vesa_bank */
    return 1;                                                                 /* 20e87: uVar1 = 1 */
}

/* The two host-driver sub-calls the playback loops make: write_vga_palette 0x4c334 (host_video_driver;
 * re-pointed to lifted C — its DAC `out` faults in OBJ1 and is serviced nested) + gdv_audio_begin_playback
 * 0x4e066 (host_audio_driver; C2 seam — reads its inputs from the audio globals, so it takes no
 * GP args). Both took an all-zero io frame before; neither is a call_orig bridge now. */
static void gdv_bridge_write_vga_palette(void)
{
    write_vga_palette(0, (uint32_t)G32(VA_g_gdv_decode_buffer + 0x10) + 0x18u, 0, 0x100u);  /* write_vga_palette — re-pointed */
}
static void gdv_bridge_audio_begin_playback(void)
{
    os_audio_gdv_begin_playback();                      /* 0x4e066 gdv_audio_begin_playback (C2 seam) */
}

/* gdv_present_streamed_frame (0x4c2d1, 99B) — the STREAMED-mode ([0x91d0c]&4) decode loop. SELF-CONTAINED
 * synchronous read->codec->emit per frame (pacing comes from read_frame_chunk's [0x91884] audio spin, which
 * the host timer driver / shm_tick surrogate pumps). begin_playback [lifted, CF] + write_vga_palette
 * [host BRIDGE] + read_frame_chunk/callback_frame_boundary/codec/advance [all lifted C]. Returns CF
 * (clc=ok / stc=read-error or EOS). Only caller = decode_frame, which ignores the result. LIVE-SWAP ONLY
 * (codec/advance publish via emit). Faithful transcription. */
uint32_t gdv_present_streamed_frame(void)
{
    if (gdv_begin_playback()) return 1;          /* 4c2d1: call 0x4e17f; jb ret (CF=1) */
    gdv_bridge_write_vga_palette();                     /* 4c2d8..4c2eb: write_vga_palette(hdr+0x18,0,0x100) */
    int32_t ecx = (int32_t)G32(VA_g_particle_pool + 0x14);                /* 4c2f0: ecx = [0x91878] (frame count) */
L4c2f6:
    /* original push ecx — the counter is preserved across the calls; it's a C local here */
    G32(VA_g_gdv_audio_stream_base + 0x10) = G32(VA_g_gdv_audio_stream_base);                        /* 4c2f7: [d60] = [d50] (ring base) */
    {
        uint32_t ebp_v, ecx_v;
        if (gdv_read_frame_chunk(&ebp_v, &ecx_v)) return 1;  /* 4c301: call 0x4c3ba; jb err (CF=1) */
    }
    G32(VA_g_dpcm_step_table + 0x414) = (int32_t)((uint32_t)G32(VA_g_dpcm_step_table + 0x414) + 1u);          /* 4c308: inc [0x91cb8] */
    if (gdv_callback_frame_boundary()) return 1; /* 4c30e: call 0x4e041; je err (ZF=EOS -> CF=1) */
    G32(VA_g_gdv_audio_stream_base + 0x1c) = G32(VA_g_gdv_audio_stream_base);                        /* 4c315: [d6c] = [d50] */
    gdv_decode_video_chunk();                    /* 4c31f: codec 0x4d384 */
    gdv_advance_chunk_ptr();                     /* 4c324: emit + advance 0x4dd33 */
    ecx--;                                              /* 4c32a: dec ecx */
    if (ecx > 0) goto L4c2f6;                            /* 4c32b: jg */
    return 0;                                           /* 4c32d/4c32f: sub ebp,ebp; clc (CF=0) */
}

/* gdv_run_playback_loop (0x4deb5, 319B) — THE per-playback-pass frame loop. PRODUCER: reads frame chunks
 * ahead into the ring (read_frame_chunk) and spins on the decoder head [0x91d68], which the decode pump
 * advances (the host_timer_driver-driven ISR body 0x4e310 — currently the shm_tick surrogate). The
 * interrupt-frame mechanics are the host timer driver's job, so this is the loop BODY, not the ISR.
 * Faithful goto-label transcription (the streamed branch
 * 0x4dfc4 jumps BACK into the preload read at 0x4df2c, so labels mirror the disasm addresses). Callees:
 * write_vga_palette 0x4c334 + gdv_audio_begin_playback 0x4e066 [host BRIDGES] + callback_frame_boundary /
 * drain / read_frame_chunk / codec / advance [all lifted C]. Streamed branch ([0x91d0c]&8) lseeks via
 * int 0x21 (g_os_soft_int). Returns CF (clc=ok / stc=read error); only caller = decode_frame (ignores
 * it). LIVE-SWAP ONLY. */
uint32_t gdv_run_playback_loop(void)
{
    uint32_t eax;
    int32_t  ecx;                                       /* frame-remaining counter (signed; jg/jle) */

    gdv_bridge_write_vga_palette();                     /* 4deb5..4dec8: write_vga_palette(hdr+0x18,0,0x100) */
    G8(VA_g_gdv_end_of_stream + 0x7)  = 0;                                   /* 4decd */
    G16(VA_g_gdv_audio_stream_base + 0x60) = (uint16_t)0x3c00;                    /* 4ded4: word [0x91db0] = 0x3c00 */
    G8(VA_g_gdv_audio_format + 0xe)  = (uint8_t)0xff;                       /* 4dedd */
    gdv_bridge_audio_begin_playback();                  /* 4dee4: call 0x4e066 (host audio) */
    ecx  = (int32_t)((uint32_t)G32(VA_g_particle_pool + 0x14) - 1u);      /* 4dee9: ecx = [0x91878] - 1 */
    ecx -= (int32_t)G32(VA_g_dpcm_step_table + 0x414);                       /* 4def2: ecx -= [0x91cb8] */
    if (ecx <= 0) goto L4df42;                          /* 4def8: jle (skip preload) */

L4defa:
    G8(VA_g_gdv_audio_end) = 0;                                    /* 4defa */
L4df01:
    if (gdv_callback_frame_boundary()) goto L4dff0; /* 4df01: call 0x4e041; je done (ZF=EOS) */
    gdv_drain_pending_subframes();               /* 4df0c: call 0x4dff4 */
    eax = (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x10);                       /* 4df11 */
    if (eax > (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x18)) goto L4df2c;      /* 4df16/4df1c: ja (unsigned) */
    eax += (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x4c);                      /* 4df1e */
    if (eax >= (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x18)) goto L4defa;     /* 4df24/4df2a: jae (unsigned) -> decode more */
L4df2c:
    {
        uint32_t ebp_v, ecx_v;
        if (gdv_read_frame_chunk(&ebp_v, &ecx_v)) goto L4dff2;  /* 4df2d: call 0x4c3ba; jb err */
    }
    G32(VA_g_dpcm_step_table + 0x414) = (int32_t)((uint32_t)G32(VA_g_dpcm_step_table + 0x414) + 1u);  /* 4df39: inc [0x91cb8] */
    ecx--;                                              /* 4df3f: dec ecx */
    if (ecx > 0) goto L4df01;                           /* 4df40: jg */

L4df42:
    if (!((uint32_t)G32(VA_g_gdv_stream_flags) & 8u)) goto L4dfc9;    /* 4df42/4df4c: test 8; je eos-tail (not streamed) */
    if ((uint8_t)G8(VA_g_palette_dirty + 0x9) != 2) {                    /* 4df4e: cmp [0x91def],2; jne -> lseek */
        uint32_t off = (uint32_t)G32(VA_g_dpcm_step_table + 0x410);          /* 4df59 */
        G32(VA_g_dpcm_step_table + 0x450) = (int32_t)off;                    /* 4df5f */
        gdv_int21(0x4200u, (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x48), off >> 16, off & 0xffffu, NULL);  /* 4df65..4df77: LSEEK (result ignored) */
    }                                                   /* (==2 case: 4df57 jmp 0x4df79 — falls through here) */
    /* 0x4df79 (not a C goto target — both the lseek branch and the ==2 case fall through): */
    G8(VA_g_gdv_audio_format + 0xc) = (uint8_t)0xff;                        /* 4df79 */
L4df80:
    G8(VA_g_gdv_audio_end) = 0;                                    /* 4df80 */
    if (gdv_callback_frame_boundary()) goto L4dff0; /* 4df87: je done */
    gdv_drain_pending_subframes();               /* 4df8e */
    eax = (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x10);                       /* 4df93 */
    if (eax > (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x18)) goto L4dfae;      /* 4df98/4df9e: ja */
    eax += (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x4c);                      /* 4dfa0 */
    if (eax >= (uint32_t)G32(VA_g_gdv_audio_stream_base + 0x18)) goto L4df80;     /* 4dfa6/4dfac: jae -> loop (to 0x4df80) */
L4dfae:
    ecx = (int32_t)(uint32_t)G32(VA_g_particle_pool + 0x14);   /* 4dfae: mov ecx,[0x91878] — RE-SEED the per-loop read
                                                                * counter. (BUGFIX: the prior lift only loaded this into a
                                                                * local `ax` for the [db8]/[dba] adds and dropped the ecx
                                                                * re-seed, so ecx stayed <=0 -> ONE chunk per L4df42 visit,
                                                                * each re-setting the loop-keyframe flag [0x91dd6]=0xff ->
                                                                * EVERY loop-2+ chunk got the 0x40 keyframe-clear bit ->
                                                                * the codec cleared the decode buffer to black before each
                                                                * in-place delta -> "black pixels after the first frame of
                                                                * every loop". With ecx re-seeded, the L4df01/L4df2c loop
                                                                * reads a full [0x91878]-frame batch per loop and only the
                                                                * FIRST re-read chunk consumes the keyframe flag.) */
    {
        uint16_t ax = (uint16_t)ecx;                     /* 4dfb4: eax=ecx; ax used below */
        G16(VA_g_gdv_audio_stream_base + 0x68) = (uint16_t)((uint16_t)G16(VA_g_gdv_audio_stream_base + 0x68) + ax);  /* 4dfb6: word [0x91db8] += ax */
        G16(VA_g_gdv_audio_stream_base + 0x6a) = (uint16_t)((uint16_t)G16(VA_g_gdv_audio_stream_base + 0x6a) + ax);  /* 4dfbd: word [0x91dba] += ax */
    }
    goto L4df2c;                                        /* 4dfc4: jmp 0x4df2c (re-enter the read) */

L4dfc9:
    G32(VA_g_gdv_audio_stream_base + 0x10) = (int32_t)0xffffffffu;                /* 4dfc9: [0x91d60] = -1 (EOS marker) */
L4dfd3:
    G8(VA_g_gdv_audio_end) = 0;                                    /* 4dfd3 */
    if (gdv_callback_frame_boundary()) goto L4dff0; /* 4dfda: je done */
    gdv_drain_pending_subframes();               /* 4dfe1 */
    if ((uint16_t)G16(VA_g_gdv_audio_stream_base + 0x6a) != 0xffffu) goto L4dfd3; /* 4dfe6: cmp word [0x91dba],0xffff; jne (drain till -1) */

L4dff0:
    return 0;                                           /* 4dff0: clc (CF=0) */
L4dff2:
    return 1;                                           /* 4dff2: stc (CF=1) */
}

/* gdv_decode_frame (0x4b8c1) — the per-playback-pass driver. Plays the whole cutscene: (rebase 13 working
 * ptrs if the input buffer moved) then either the streamed path (flags&4 -> present_streamed_frame) or
 * begin_playback + prime + fade_in + run_playback_loop + audio_stop + fade_out. The two multi-frame LOOPS
 * (run_playback_loop 0x4deb5 / present_streamed_frame 0x4c2d1) are now LIFTED C, called directly (their
 * int3s never fire — decode_frame is their only caller). Marked INTERACTIVE so shm_tick's surrogate (the
 * host_timer_driver prototype) pumps the pacing globals (0x90bcc frame tick / 0x91884 read budget /
 * 0x91dbc fade / the decode pump) while the lifted loop runs. g_gdv_loop_hosting is set around the loop
 * calls so the surrogate stands in for the frozen decode-pump ISR. Returns EAX: 0 played / 1 idle /
 * 0x100 EOS. ABI_EAX (input ignored). LIVE-SWAP (PER-NAME or gdv group). */
uint32_t gdv_decode_frame(uint32_t ignored)
{
    (void)ignored;
    uint32_t ebp = 1;                                   /* mov ebp,1 (return: idle) */
    if ((uint8_t)G8(VA_g_gdv_decoder_active) != 0) {                    /* cmp [g_gdv_decoder_active],0; je end */
        uint32_t ctx = (uint32_t)G32(VA_g_gdv_context);          /* g_gdv_context */
        uint32_t delta = *(uint32_t *)(uintptr_t)(ctx + 8) - (uint32_t)G32(VA_g_gdv_decode_buffer);  /* [ctx+8] - g_gdv_decode_buffer */
        if (delta != 0) {                               /* je 0x4b905 (skip rebase) */
            for (int i = 0; i < 13; i++) {              /* mov ecx,0xd; rebase ptrs at 0x91d34.. */
                uint32_t a = 0x91d34u + (uint32_t)i * 4;
                if ((uint32_t)G32(a) != 0) G32(a) = (int32_t)((uint32_t)G32(a) + delta);
            }
            *(uint32_t *)(uintptr_t)(ctx + 0x50) = (uint32_t)G32(VA_g_gdv_decode_buffer + 0x10);  /* [ctx+0x50] = [0x91d44] */
        }
        if ((uint32_t)G32(VA_g_gdv_stream_flags) & 4) {               /* test [g_gdv_stream_flags],4; jne streamed */
            ebp = 0;
            /* The lifted loop runs in the trap with int3s suspended, freezing the decode-pump ISR
             * (0x4e60b -> 0x4e310: codec + advance of the decoder head [0x91d68] the loop spins on).
             * Flag the host SIGALRM surrogate (the host_timer_driver prototype) to stand in for it. */
            g_gdv_loop_hosting = 1;
            gdv_present_streamed_frame();         /* 0x4c2d1 (lifted C — was call_orig bridge) */
            g_gdv_loop_hosting = 0;
        } else {
            if (gdv_begin_playback()) goto done;  /* call begin_playback; jb -> skip (ebp=1) */
            gdv_prime_first_frame();              /* call 0x4de7a */
            gdv_fade_in_palette();                /* call 0x4e08e */
            ebp = 0;
            g_gdv_loop_hosting = 1;                      /* host surrogate drives the frozen decode-pump ISR's per-tick decode */
            gdv_run_playback_loop();              /* 0x4deb5 (lifted C — was call_orig bridge) */
            g_gdv_loop_hosting = 0;
            G8(VA_g_gdv_audio_format + 0xe) = 0;                             /* mov byte [0x91dd8],0 */
            os_audio_gdv_stop_voice();                   /* 0x55640 gdv_audio_stop_voice (C2 seam) */
            gdv_fade_out_palette();               /* call 0x4e0c2 */
        }
    }
done:
    if ((uint8_t)G8(VA_g_gdv_end_of_stream) != 0) return 0x100;        /* cmp [g_gdv_end_of_stream],0; jne -> 0x100 */
    return ebp;
}

/* show_cutscene_playback_menu (0x26356) — the FMV cutscene GALLERY (the LAST engine GDV fn). Opened by
 * clicking the LEFT MARGIN of the options menu (run_options_menu, action 2000). Lists every cutscene/story
 * record SEEN so far (1..g_cutscenes_seen_count-1 [0x82006]): for each playback index, scan the dbase100
 * record array for the FIRST record tagged with that index (high byte of *(rec+0x10)), copy its 8-byte name
 * + (if it has a dialogue line, *(rec+0xc)!=0) overwrite with the localized title via read_next_dialogue_line,
 * and build a scrollable list box. Selecting an entry returns its action (idx+1)|0x800000, which
 * show_message_box dispatches -> finish_dialogue_record_eval -> a NESTED cutscene replay.
 *
 * Source of truth: ghidra_export/decomp/26356_show_cutscene_playback_menu.c + the 0x26356 disasm. ABI: EAX=
 * param_1 (menu flags from the caller). Returns void -> ABI_EAX (return 0). PER-NAME + INTERACTIVE.
 *
 * BYTE LAYOUT (verified from disasm, not the decompile — Ghidra's local naming is offset by the unusual
 * `enter 0x3348` + `sub ebp,0x82` prologue):
 *   desc[] (the show_message_box descriptor, host stack; original = the [ebp+0x2] frame area):
 *     +0x00 u32 0x48   +0x04 u32 0x49   +0x08 u16 0x380   +0x0a u16 seen-1 (16-bit dec)
 *     +0x0c u16 scroll anchor [0x83ea4] (IN/OUT — show_message_box updates it as the user scrolls)
 *     +0x0e u16 max-scroll (clamped n-5)   +0x10 u32 visible row count = min(n,5)
 *     +0x14.. up to 5 entries, stride 0xc: {u32 flags=0, u32 textptr=&textbuf (the FIXED buffer base,
 *             confirmed `lea [ebp-0x32c6]` == the txt-cursor init), u32 action=(n+1)|0x800000}.
 *   textbuf[13000] (local_3360): the 0x32-stride item records show_message_box indexes by row -
 *     [0..7]=name, [8]=0, then overwritten by the dialogue line (maxlen 0x2e), [0x31]=record scan index.
 * The descriptor's entry textptr is the SAME buffer base for every entry; show_message_box derives each
 * row's text as base + row*0x32 (and scrolls via the +0xc anchor over n>5 items). Both buffers live on the
 * host (lift) stack -> show_message_box (bridged, runs in-process via call_orig) derefs them fine (32-bit
 * host; same proven host-stack-ptr pattern as play_gdv_cutscene's desc).
 *
 * read_next_dialogue_line 0x1e8cc = LIFTED -> call C (NOTE: the 3rd Watcom arg EBX = *(rec+0xc), the line
 * ref — NOT 0; the decompile dropped it). show_message_box 0x2508f = BRIDGE via call_orig (interactive
 * blocking list box; EAX=&desc, EDX=EBX=param_1|0xc09 -> EAX = selected action). g_dbase100_base [0x81e1c]
 * holds a runtime ptr (deref raw). LIVE-SWAP ONLY (interactive -> not oracle-able), PER-NAME.
 *
 * LOOP-COUPLING (the hard part): selecting an entry replays a cutscene INSIDE the bridged show_message_box;
 * that nested decode runs as original bytes (call_orig SUSPENDS the lift int3s) so it does NOT dispatch the
 * loop-hosted lifted C nor set g_gdv_loop_hosting -> the decode-pump ISR is frozen -> it would hang. FIX:
 * set g_gdv_loop_hosting=1 around the show_message_box loop so the shm_tick surrogate (= host_timer_driver
 * prototype) stands in for the frozen pump while show_message_box runs. Its guards (decoder-active [0x91dd8])
 * make the pump a no-op while the list just shows; it drives decode once a selected cutscene opens. Frame
 * publish still fires (the GDV_EMIT_SITE 0x4dcfc int3 is NOT a lift int3, so call_orig doesn't suspend it).
 * 0x26356 is in lift_is_interactive -> g_os_interactive pumps the frame tick + input ring so the blocking
 * show_message_box gets keyboard/mouse. */
uint32_t show_cutscene_playback_menu(uint32_t param_1)
{
    if ((uint32_t)G32(VA_g_cutscenes_seen_count) <= 1) return 0;          /* cmp [0x82006],1; jbe -> ret (nothing to show) */

    uint8_t desc[0x60];                                 /* show_message_box descriptor (header + 5 entries) */
    uint8_t textbuf[13000];                             /* local_3360: the 0x32-stride item records */
    memset(desc, 0, sizeof desc);                       /* slots beyond the visible count aren't read; safe */

    *(uint32_t *)(desc + 0x00) = 0x48;
    *(uint32_t *)(desc + 0x04) = 0x49;
    *(uint16_t *)(desc + 0x08) = 0x380;
    *(uint16_t *)(desc + 0x0a) = (uint16_t)((uint16_t)G16(VA_g_cutscenes_seen_count) - 1);   /* mov ax,[0x82006]; dec ax */

    uint8_t *entry = desc + 0x14;                       /* local_28: entry-list cursor (5 slots, 0xc each) */
    uint8_t *txt   = textbuf;                            /* local_24: text-record cursor (stride 0x32) */
    uint32_t n     = 0;                                  /* local_1c: entry count */
    uint32_t seen  = (uint32_t)G32(VA_g_cutscenes_seen_count);             /* local_38 */
    uint32_t base  = (uint32_t)G32(VA_g_dbase100_base);             /* g_dbase100_base (stored runtime ptr) */
    uint8_t *rec_base = (uint8_t *)(uintptr_t)(base + *(int32_t *)(uintptr_t)(base + 0x24));  /* local_34 */

    for (uint32_t pidx = 1; pidx < seen; pidx++) {       /* local_2c: playback index */
        uint8_t *rec = rec_base;
        uint32_t scan = 0;                               /* local_30 */
        uint8_t *found = 0;
        for (;;) {                                       /* scan the record array for this playback index */
            uint32_t b = (uint32_t)G32(VA_g_dbase100_base);                       /* re-read base each iter (faithful) */
            if (scan >= *(uint32_t *)(uintptr_t)(b + 0x20)) break;     /* out of records -> no match */
            if ((*(uint32_t *)(uintptr_t)(rec + 0x10) >> 0x18) == pidx) { found = rec; break; }
            scan++; rec += 0x14;
        }
        if (!found) continue;                            /* LAB_00026459: next playback index */

        memcpy(txt, found, 8);                           /* 8-byte record name (repnz movs, DF=0) */
        txt[8] = 0;
        if (*(int32_t *)(uintptr_t)(found + 0xc) != 0)   /* has a dialogue line -> localized title */
            read_next_dialogue_line((uint32_t)(uintptr_t)txt, 0x2e,
                                           (uint32_t)*(int32_t *)(uintptr_t)(found + 0xc), 0);
        if ((int32_t)n < 5) {                            /* max 5 visible entry slots */
            *(uint32_t *)(entry + 0x00) = 0;
            *(uint32_t *)(entry + 0x04) = (uint32_t)(uintptr_t)textbuf;   /* &local_3360 (FIXED buffer base) */
            *(uint32_t *)(entry + 0x08) = (n + 1) | 0x800000u;           /* replay-action id */
            entry += 0xc;
        }
        n++;
        txt[0x31] = (uint8_t)scan;                       /* store the record scan index */
        txt += 0x32;
    }

    if (n == 0) return 0;                                /* no records matched */

    *(uint32_t *)(desc + 0x10) = (n > 5) ? 5 : n;        /* visible row count = min(n,5) */
    int32_t scroll = (int32_t)n - 5;                     /* local_1c -= 5; clamp >= 0 */
    if (scroll < 0) scroll = 0;
    if ((uint32_t)G32(VA_g_playback_menu_scroll_anchor) > (uint32_t)scroll) G32(VA_g_playback_menu_scroll_anchor) = scroll;  /* clamp saved anchor (unsigned) */
    *(uint16_t *)(desc + 0x0c) = (uint16_t)(uint32_t)G32(VA_g_playback_menu_scroll_anchor);          /* scroll anchor (IN/OUT) */
    *(uint16_t *)(desc + 0x0e) = (uint16_t)scroll;                          /* max-scroll */

    /* the blocking interactive list box; 2000 = the left-margin reopen hotspot, looped (treated as 0). The
     * surrogate hosts any nested cutscene replay triggered by selecting an entry (see header). */
    g_gdv_loop_hosting = 1;
    uint32_t result;
    do {
        /* show_message_box(&desc, param_1|0xc09); original also set EBX=same value, callee saves+ignores it — re-pointed */
        result = show_message_box((uint32_t)(uintptr_t)desc, param_1 | 0xc09u);
        if (result == 0x7d0) result = 0;                 /* 2000 -> 0 (continue looping) */
    } while (result != 0);
    g_gdv_loop_hosting = 0;

    G32(VA_g_playback_menu_scroll_anchor) = (int32_t)(uint32_t)*(uint16_t *)(desc + 0x0c);   /* save the final scroll anchor */
    return 0;
}
