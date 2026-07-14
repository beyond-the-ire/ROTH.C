/* lift_math_util.c — the math/string/shared leaf primitives lifted to verified C.
 * Split out of renderer.c (per docs/operating/recomp.md §4.6: every subsystem gets its own TU).
 *
 * math_util is the foundational shared-leaf layer (fixed-point math, 2D rotate, string/format
 * helpers, the vsprintf core). It is the producer, not the consumer: it calls almost nothing out.
 * The only external bridge is memset (CRT, host-replaced) reached by mem_fill. Intra-subsystem
 * callees that are already lifted (isqrt_fixed 0x3bfe5, build_atan_table 0x3c1c9, rotate_point_2d
 * 0x2a898) are reached via engine.h declarations, not bridged.
 *
 * ABI is derived from the DISASM (the corpus pseudocode is Borland-on-Watcom, unreliable). Each
 * function's canon VA + the registers it actually reads/writes are documented at its definition.
 * lift-lens: docs/reference/lift/math_util.md.
 */
#include "common.h"
#include "engine.h"
#include <string.h>

/* ====================================================================== Layer 1 — pure leaves */

/* shared_epilogue_6reg (0x18a23, 7 B, DEAD) — the Watcom shared register-restore epilogue
 *   leave; pop edi; pop esi; pop edx; pop ecx; pop ebx; ret
 * It is tail-jumped to by ~14 functions (inventory UI etc.); the exact 7-byte sequence occurs 41×
 * across the image. It is NOT a normal callable — it tears down the CALLER's frame (leave) and
 * returns through the caller's pushed return address, so a call-based differential is meaningless.
 * The faithful lift is this byte-identical naked thunk; the oracle verifies the original 7 bytes
 * match this exact encoding (c9 5f 5e 5a 59 5b c3). */
__attribute__((naked, used)) void shared_epilogue_6reg(void)
{
    __asm__ volatile("leave\n\t"
                     "pop %edi\n\t"
                     "pop %esi\n\t"
                     "pop %edx\n\t"
                     "pop %ecx\n\t"
                     "pop %ebx\n\t"
                     "ret");
}

/* thunk_build_atan_table (0x3c28c, 6 B) — `call build_atan_table 0x3c1c9; ret`. Startup-only
 * trampoline that builds the fixed-point atan LUT. build_atan_table is already lifted. */
void thunk_build_atan_table(void)
{
    build_atan_table();
}

/* isqrt_fixed_wrapper_3bfd6 (0x3bfd6, 15 B) — saves ebx/ecx/edx (isqrt_fixed clobbers them),
 * calls isqrt_fixed(eax), `movzx eax,ax` (zero-extend the 16-bit result), restores, ret.
 * EAX in -> EAX out (= isqrt_fixed's 16-bit return, zero-extended to 32 bits). */
uint32_t isqrt_fixed_wrapper_3bfd6(uint32_t eax)
{
    return (uint32_t)(uint16_t)isqrt_fixed(eax);
}

/* mem_fill (0x4b360, 24 B) — replicates the fill byte (DL) into all 4 bytes of EDX, then calls
 * memset(EAX=dst, EDX=dword-pattern, ECX=EBX=count). The CRT memset 0x55240 is a byte memset that
 * uses the replicated dword for its stosd middle; net effect = `count` bytes at dst set to the
 * byte. Bridged to the host CRT memset (byte-identical write-set). EAX (dst) is preserved across
 * the push/pop, so the function returns dst.
 *   ABI: EAX=dst, EDX=fill byte (low 8 bits), EBX=count -> EAX=dst. */
void *mem_fill(void *dst, uint32_t val, uint32_t count)
{
    memset(dst, (int)(uint8_t)val, count);
    return dst;
}

/* rotate_point_2d_shifted (0x2b25b, 61 B) — translate (AX,DX) by the camera origin
 * (g@0x90a8e, g@0x90a96), build the 3-word point struct with rotation flags = -(g@0x909f8),
 * call rotate_point_2d 0x2a898 (already lifted; returns the EAX/EDX pair), then `sar eax,8` /
 * `sar edx,8` (arithmetic). 16-bit subs wrap mod 2^16; the flags field is the low 16 bits of the
 * 32-bit negation of the (zero-extended) 16-bit global.
 *   ABI: AX=x, DX=y -> EAX=rx>>8, EDX=ry>>8 (multi-reg return, A1). */
void rotate_point_2d_shifted(int16_t x_in, int16_t y_in,
                                    int32_t *eax_out, int32_t *edx_out)
{
    int16_t pt[3];
    pt[0] = (int16_t)((uint16_t)x_in - (uint16_t)G16(VA_g_player_x));   /* sub ax,[0x90a8e] */
    pt[1] = (int16_t)((uint16_t)y_in - (uint16_t)G16(VA_g_player_y));   /* sub dx,[0x90a96] */
    pt[2] = (int16_t)(-(int32_t)(uint16_t)G16(VA_g_sprite_view_angle));          /* mov cx,[0x909f8]; neg ecx -> low 16 */
    int32_t e, d;
    rotate_point_2d(pt, &e, &d);
    *eax_out = e >> 8;                                            /* sar eax,8 */
    *edx_out = d >> 8;                                            /* sar edx,8 */
}

/* stricmp (0x5607b, 71 B) — case-insensitive string compare. Lowercases only A-Z (0x41..0x5a, the
 * char tested unsigned via ECX) by +0x20, compares byte-wise, and returns the signed difference of
 * the lowercased bytes at the first mismatch (or 0 at the shared NUL terminator).
 *   ABI: EAX=s1, EDX=s2 -> EAX = (int)s1lc - (int)s2lc. */
int32_t stricmp(const uint8_t *s1, const uint8_t *s2)
{
    for (;;) {
        uint8_t a = *s1, b = *s2;
        uint8_t al = (a >= 0x41 && a <= 0x5a) ? (uint8_t)(a + 0x20) : a;
        uint8_t bl = (b >= 0x41 && b <= 0x5a) ? (uint8_t)(b + 0x20) : b;
        if (al != bl)  return (int32_t)al - (int32_t)bl;
        if (bl == 0)   return 0;                       /* al==bl and both NUL */
        s1++; s2++;
    }
}

/* compare_name_token_ci (0x1063d, 73 B) — case-insensitive compare of a path *basename* token
 * against a word. First scans arg1 forward to whitespace (<=0x20), resetting the token start to the
 * char after each backslash ('\\', 0x5c) so directory prefixes are skipped. Then compares the
 * basename against arg2: a space (0x20) is treated as NUL; if arg2 ends and the token char is '.'
 * (0x2e) -> match; otherwise both chars are uppercased via &0xdf and compared. Returns 0 on match,
 * -1 on mismatch.
 *   ABI: EAX=arg1 (path/token), EDX=arg2 (word) -> EAX = 0 | -1. */
int32_t compare_name_token_ci(const uint8_t *arg1, const uint8_t *arg2)
{
    const uint8_t *esi = arg1;
    const uint8_t *tok = arg1;                 /* ebx — token start */
    for (;;) {                                 /* 0x10644 skip-to-whitespace / basename scan */
        uint8_t al = *esi++;
        if (al <= 0x20) break;
        if (al == 0x5c) tok = esi;             /* char after the backslash */
    }
    esi = tok;                                 /* 0x10651 */
    const uint8_t *edi = arg2;
    for (;;) {                                 /* 0x10656 compare loop */
        uint8_t al = *esi++;                   /* lodsb */
        uint8_t ah = *edi++;
        if (al == 0x20) al = 0;
        if (ah == 0x20) ah = 0;
        if (ah == 0) {
            if (al == 0x2e) return 0;          /* token '.' at arg2-end -> match */
        }
        al &= 0xdf; ah &= 0xdf;                /* and ax,0xdfdf (uppercase both) */
        if (al != ah) return -1;
        if (al == 0)  return 0;
    }
}

/* match_word_in_list_ci (0x150b8, 66 B) — find a word in a space-separated list, case-insensitively
 * (&0xdf uppercasing; note 0x20 also masks to 0, so a space terminates an entry just like NUL).
 * Returns the 0-based index of the matching list entry, or 0 if the list end (NUL) is reached
 * before a match (the original conflates "not found" with index 0).
 *   ABI: EAX=word, EDX=list -> EAX = entry index | 0. */
uint32_t match_word_in_list_ci(const uint8_t *word, const uint8_t *list)
{
    uint32_t ecx = 0;                          /* entry index */
    const uint8_t *esi = list;
    for (;;) {                                 /* 0x150c2 outer (per list entry) */
        if (*esi == 0) return 0;               /* 0x150f3 end of list */
        const uint8_t *edi = word;             /* 0x150c7 */
        for (;;) {                             /* 0x150c9 inner (compare chars) */
            uint8_t alm = (uint8_t)(*edi & 0xdf);
            uint8_t ahm = (uint8_t)(*esi & 0xdf);
            if (alm == 0 && ahm == 0) return ecx;          /* both terminate -> match */
            edi++;
            uint8_t consumed = *esi;           /* the list char just passed (for the [esi-1] test) */
            esi++;
            if (ahm == alm) continue;          /* chars match -> keep comparing */
            ecx++;                             /* mismatch -> advance to next entry */
            if (consumed == 0x20) break;       /* already at the entry boundary -> outer */
            while (*esi++ != 0x20) { }         /* 0x150ea skip to the next space */
            break;                             /* -> outer */
        }
    }
}

/* copy_switch_token_upper (0x10920, 79 B) — copy one command-line switch token from *esi into the
 * global scratch buffer g@0x76719, uppercasing a-z (&0xdf), and store the length at g@0x76718.
 * Leading spaces are skipped; a leading '/' is copied (it introduces the switch); a later '/' (or a
 * space, or NUL) terminates. '-' is copied as-is. Reads the source via ES (== DS, flat). Returns
 * the length (DL); the caller branches on ZF (length != 0).
 *   ABI: ESI=src -> writes g@0x76719[..]=token, g@0x76718=len, returns len.
 * `end` (out, optional) receives the advanced source cursor (the original's post-call ESI) so the
 * load_roth_res caller can re-point off call_orig. Cursor-diffed in the oracle. */
uint32_t copy_switch_token_upper(const uint8_t *src, const uint8_t **end)
{
    const uint8_t *esi = src;
    uint8_t *edi = (uint8_t *)GADDR(VA_g_arg_token_buf + 0x1);
    uint8_t dl = 0;
    while (*esi != 0 && *esi == 0x20) esi++;    /* 0x10928 skip leading spaces */
    if (*esi == 0) goto done;
    if (*esi == 0x2f) {                         /* 0x10936 leading '/' is part of the token */
        *edi++ = 0x2f; dl++; esi++;
    }
    for (;;) {                                  /* 0x1093d main loop */
        uint8_t al = *esi;
        if (al == 0)            goto done;      /* NUL */
        if (al == 0x2d) {                       /* '-' store as-is */
            *edi++ = al; dl++; esi++; continue;
        }
        if (al <= 0x20) { esi++; goto done; }   /* <=space: consume one and stop (0x10962) */
        if (al == 0x2f)         goto done;      /* '/' terminates the token */
        if (al >= 0x61 && al <= 0x7a) al &= 0xdf;   /* a-z -> A-Z */
        *edi++ = al; dl++; esi++;
    }
done:
    *edi = 0;                                   /* NUL terminate */
    G8(VA_g_arg_token_buf) = dl;                           /* store length */
    if (end) *end = esi;                        /* advanced source cursor (original's post-call ESI) */
    return dl;
}

/* ============================================================ Layer 2 — the vsprintf chain */

/* format_decimal_grouped (0x27e46, 166 B) — emit a signed 32-bit integer in base 10 (MSB-first,
 * dividing by descending powers of ten) into *edi, with leading-position handling and optional
 * thousands grouping. Pure register-machine transcription:
 *   EAX = value (signed); CL = field/format code; EDI = output cursor (advanced).
 *   ch  = control byte: low nibble = leading-position suppression count, bit5(0x20)=negative,
 *         bit6(0x40)=digits-started/grouping-active, bit7(0x80)=cl==0x0b special.
 *   cl  = grouping sub-counter cycling 1->0->2 (comma every 3rd digit when it hits 0).
 *   g@0x8491d format flags: bit1 forces grouping-active; bit0 enables the ',' separator.
 * ECX is saved/restored (push/pop ecx) so the caller's ECX is preserved; the outputs are the
 * bytes written and the advanced EDI. */
void format_decimal_grouped(int32_t eax, uint8_t cl, uint8_t **edi_io)
{
    uint8_t *edi = *edi_io;
    uint32_t ebx = 0x3b9aca00u;                 /* 10^9 */
    uint8_t ch;
    if (cl == 0x0b) ch = 0x80;                  /* 0x27e4c */
    else            ch = (uint8_t)(0x0a - cl);  /* mov ch,0xa; sub ch,cl */
    cl = 1;                                      /* grouping sub-counter */

    uint32_t val;
    if (eax >= 0) {                             /* or eax,eax; jns */
        val = (uint32_t)eax;
    } else {
        val = (uint32_t)(-eax);                 /* neg eax */
        ch = (uint8_t)(ch + 0x20);              /* add ch,0x20 (negative flag) */
        if (!(ch & 0x80)) ch = (uint8_t)(ch + 1);   /* js skip; else inc ch */
    }
    if (G8(VA_g_format_flags) & 2) ch |= 0x40;            /* 0x27e68 */

    for (;;) {                                  /* 0x27e74 — per digit, MSB first */
        uint8_t al8 = (uint8_t)(val / ebx);     /* sub edx,edx; div ebx -> quotient (one digit) */
        uint32_t rem = val % ebx;               /* edx remainder */

        int go_output;                          /* 0x27e78.. choose output vs leading path */
        if (ebx == 1)        go_output = 1;     /* last digit always output */
        else if (al8 != 0)   go_output = 1;     /* non-zero digit */
        else if (ch & 0x40)  go_output = 1;     /* digits already started */
        else                 go_output = 0;

        int tail = 1;                           /* 1 -> 0x27ec7 (ch&0xf test); 2 -> 0x27ece (skip) */
        if (!go_output) {                       /* 0x27e81/0x27e86 leading-position */
            if (!(ch & 0x8f)) { *edi++ = 0x20; }   /* test ch,0x8f; je -> emit pad space */
            /* else skip output */
        } else {                                /* 0x27e91 */
            if (ch & 0x0f) {                    /* still suppressing -> consume one, no digit */
                ch |= 0x40;
                ch = (uint8_t)(ch - 1);
                tail = 2;                       /* jmp 0x27ece */
            } else {
                if (ch & 0x20) {                /* 0x27e9d negative flag */
                    ch = (uint8_t)(ch - 0x20);
                    *edi++ = 0x2d;              /* '-' */
                }
                if (cl == 0 && (ch & 0x40) && (G8(VA_g_format_flags) & 1))   /* 0x27ea9 grouping comma */
                    *edi++ = 0x2c;              /* ',' */
                ch |= 0x40;                     /* 0x27ebf */
                *edi++ = (uint8_t)(al8 + 0x30); /* emit digit */
            }
        }

        if (tail == 1) {                        /* 0x27ec7 */
            if (ch & 0x0f) ch = (uint8_t)(ch - 1);
        }
        /* 0x27ece */
        cl = (uint8_t)(cl - 1);                 /* dec cl; jns skip; else mov cl,2 */
        if ((int8_t)cl < 0) cl = 2;
        if (ebx == 1) break;                    /* cmp ebx,1; je done */
        ebx = ebx / 10;                         /* next lower power of ten */
        val = rem;
    }
    *edi_io = edi;
}

/* bounded_string_copy (0x27e0b, already lifted elsewhere but its register-threading — it advances
 * EDI and EBP and returns the residual CL — is needed inline by vsprintf_core's %s path; transcribed
 * here as a helper that updates the EDI and EBP cursors and returns the post-loop CL). Copies `cl` (signed
 * >0) chars of the string at *ebp (then ebp+=4) to *edi; stops at NUL or cl==0; preserves ESI. */
static int8_t vsc_bounded_copy(uint8_t **edi_io, const uint32_t **ebp_io, int8_t cl)
{
    const uint8_t *esi = (const uint8_t *)(uintptr_t)**ebp_io;   /* mov esi,[ebp] */
    (*ebp_io)++;                                                 /* add ebp,4 */
    uint8_t *edi = *edi_io;
    for (;;) {                                  /* 0x27e12 */
        uint8_t al = *esi;
        if (al == 0) break;                     /* NUL */
        *edi++ = al;                            /* mov [edi],al; inc edi */
        esi++;
        cl = (int8_t)(cl - 1);                  /* dec cl */
        if (!(cl > 0)) break;                   /* jg (signed) */
    }
    *edi_io = edi;
    return cl;
}

/* vsprintf_core (0x27ca6, 393 B) — the printf-family format engine, shared body behind the three
 * entry stubs (vsprintf_entry_printf 0x27c91 / _sprintf 0x27c98 / _engine 0x27ca0, which only set up
 * the EBP va-args pointer then jump in). Faithful goto-structured register-machine transcription
 * (labels named by canon VA). Inputs: ESI=fmt, EDI=out buffer, EBP=va-args pointer (dwords). Output:
 * EAX = formatted length (final EDI - start), with a NUL written but not counted. Handles literals,
 * the "\n" (0x5c 0x6e) -> CRLF escape, and %c/%s/%d/%D/%x/%X with optional 'l' modifier and 1-2 digit
 * width; format flags live in g@0x8491d (bit0 ','-enable, bit1 zero-pad/group-active, bit3 width-set).
 * Hex digits via the xlat tables g@0x71d94 (lower) / g@0x71da4 (upper). */
uint32_t vsprintf_core(const uint8_t *fmt, uint8_t *out, const uint32_t *args)
{
    const uint8_t *esi = fmt;
    uint8_t *edi = out;
    const uint32_t *ebp = args;
    uint8_t *const edi0 = out;
    uint32_t ecx = 0, edx, ebx;
    uint8_t al;

main_loop:                                      /* 0x27ca6 */
    al = *esi++;                                /* lodsb */
    if (al == 0) goto done;                     /* 0x27cc6 */
    if (al == 0x25) goto fmt_spec;              /* '%' */
    if (al == 0x5c && *esi == 0x6e) {           /* "\n" escape -> CRLF */
        esi++;
        *edi++ = 0x0d; *edi++ = 0x0a;           /* mov word [edi],0xa0d (LE) */
        goto main_loop;
    }
    *edi++ = al;                                /* 0x27cc3 stosb literal */
    goto main_loop;

done:                                           /* 0x27cc6 */
    *edi = 0;                                   /* NUL terminate (not counted) */
    return (uint32_t)(edi - edi0);              /* mov eax,edi; sub eax,edi0 */

fmt_spec:                                       /* 0x27cd0 */
    G8(VA_g_format_flags) = 0;
    al = *esi++;                                /* lodsb */
    ecx = 1;
    if (al == 0x63) goto conv_c;                /* 'c' */
    ecx = 0x7f;
    if (al == 0x73) goto conv_s;                /* 's' */
    ecx = 0x080b;                               /* cl=0x0b, ch=8 */
    if (al == 0x64) goto conv_d;                /* 'd' */
    if (al == 0x44) goto conv_D;                /* 'D' */
    ecx = 0x0108;                               /* cl=8, ch=1 */
    if (al == 0x6c) goto conv_l;                /* 'l' modifier */
    ecx = (ecx & 0xffffff00u) | 4;              /* mov cl,4 (ch stays 1 -> 0x0104) */
    if (al == 0x78) goto conv_x;                /* 'x' */
    if (al == 0x58) goto conv_X;                /* 'X' */
    ecx = 0;                                    /* sub ecx,ecx */
    if (al < 0x30) goto dispatch2;              /* 0x27d4c */
    if (al != 0x30) goto wd_flag8;              /* '1'..'9' */
    G8(VA_g_format_flags) |= 2;                           /* '0' -> zero-pad flag */
wd_flag8:
    G8(VA_g_format_flags) |= 8;                           /* width-specified flag */
    if (al > 0x39) { *edi++ = al; goto main_loop; }   /* invalid -> emit char */
    al = (uint8_t)(al - 0x30);
    ecx = (ecx & 0xffffff00u) | al;             /* cl = first width digit */
    al = *esi++;                                /* lodsb */
    if (al < 0x30) goto dispatch2;
    if (al > 0x39) goto dispatch2;
    ecx = ecx * 10;                             /* lea/add: cx *= 10 */
    al = (uint8_t)(al - 0x30);
    ecx = (ecx & 0xffff0000u) | ((ecx + al) & 0xffff);   /* add cx,ax (ah=0) */
conv_l:                                         /* 0x27d4b */
    al = *esi++;                                /* lodsb */
dispatch2:                                      /* 0x27d4c */
    if (al == 0x64) goto conv_d;
    if (al == 0x44) goto conv_D;
    if (al == 0x73) goto conv_s;
    if (al == 0x78) goto conv_x;
    if (al == 0x58) goto conv_X;
    if (al == 0x63) goto conv_c;
    *edi++ = al; goto main_loop;                /* unknown spec -> emit char */

conv_X:                                         /* 0x27d75 */
    ebx = 0x71da4u;                             /* uppercase hex table */
    goto hex_body;
conv_x:                                         /* 0x27d7c */
    ebx = 0x71d94u;                             /* lowercase hex table */
hex_body: {                                     /* 0x27d81 */
    edx = *ebp++;                               /* value */
    uint8_t cl = (uint8_t)ecx;
    uint8_t ch = (uint8_t)(ecx >> 8);
    if (ch != 0) {                              /* 0x27d87 — variable width */
        if (edx == 0) {
            cl = 1;                             /* emits a single "0" */
        } else {
            cl = 8;                             /* mov ecx,1; add ecx,7 */
            while (!(edx & 0xf0000000u)) { edx <<= 4; cl--; }   /* count significant nibbles */
        }
    } else {                                    /* 0x27da5 — fixed width = cl nibbles */
        uint8_t a = cl;                         /* movzx eax,cl (ah=0) */
        while (a != 8) { edx <<= 4; a = (uint8_t)(a + 1); }      /* position low cl nibbles to top */
    }
    const uint8_t *tbl = (const uint8_t *)GADDR(ebx);
    uint8_t ah = 0;                             /* "non-zero digit seen" flag */
    for (;;) {                                  /* 0x27db3 emit loop */
        edx = (edx << 4) | (edx >> 28);         /* rol edx,4 */
        uint8_t outc = tbl[edx & 0xf];          /* and al,0xf; xlatb */
        if (ah == 0 && outc == 0x30) {          /* cmp ax,0x30 — leading zero */
            if (cl == 1) { /* last nibble -> emit '0' */ }
            else if (G8(VA_g_format_flags) & 2) { /* zero-pad -> emit '0' */ }
            else outc = 0x20;                   /* else suppress with a space */
        } else {
            ah++;                               /* inc ah */
        }
        *edi++ = outc;                          /* stosb */
        cl = (uint8_t)(cl - 1);                 /* dec cl */
        if (!((int8_t)cl > 0)) break;           /* jg */
    }
    goto main_loop;
}

conv_c: {                                       /* 0x27e22 */
    uint32_t arg = *ebp++;
    uint8_t c = (uint8_t)arg;
    for (uint32_t i = 0; i < ecx; i++) *edi++ = c;   /* rep stosb (ecx times) */
    goto main_loop;
}

conv_d:                                         /* 0x27e2f */
    G8(VA_g_format_flags) |= 1;                           /* enable ',' grouping */
    /* fall through */
conv_D: {                                       /* 0x27e36 */
    int32_t v = (int32_t)*ebp++;
    format_decimal_grouped(v, (uint8_t)ecx, &edi);   /* cl = field code */
    goto main_loop;
}

conv_s:                                         /* 0x27ddf */
    if (G8(VA_g_format_flags) & 8) {                      /* width specified */
        int8_t rem = vsc_bounded_copy(&edi, &ebp, (int8_t)(uint8_t)ecx);
        if (rem > 0) {                          /* or cl,cl; jg -> pad with spaces */
            for (int8_t i = 0; i < rem; i++) *edi++ = 0x20;
        }
    } else {                                    /* 0x27e01 — copy up to 0x7f */
        vsc_bounded_copy(&edi, &ebp, (int8_t)(uint8_t)ecx);
    }
    goto main_loop;
}
