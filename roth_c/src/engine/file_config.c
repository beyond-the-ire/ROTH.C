/* lift_file_config.c — the ROTH file_config subsystem lifted to verified C.
 * Per docs/operating/recomp.md §4.6: every subsystem gets its own TU.
 *
 * file_config = generic file/resource loaders (ROTH.RES, ICONS.ALL, dbase300), INI + cmdline
 * config parsing, and path/filename helpers. lift-lens:
 * docs/reference/lift/file_config.md; formats: docs/reference/ROTH_file_formats.md +
 * docs/reference/ROTH_cmdline_switches.md.
 *
 * ABI throughout is derived from the DISASM (tools/roth_disasm.py func <va>), not the corpus
 * pseudocode — the config-parse family in particular is Watcom shared-EBP-frame code (one frame
 * built by parse_config_keywords, threaded by register into line/typed_value/number), which the
 * decompiler renders unreliably. The DOS file-IO seam (raw inline `int 0x21`) routes through the
 * host soft-int hook g_os_soft_int (NULL in the oracle -> the test installs a mock, exactly like
 * lift_das_assets.c).
 */
#include "common.h"
#include "engine.h"
#include <string.h>
#include "os_api.h"      /* os_dos_delete (dos_delete_file 0x41ad4 -> c2 contract) */

/* raw runtime-memory accessors (for values that are ALREADY host/rebased addresses) */
#define R8(p)  (*(volatile uint8_t  *)(uintptr_t)(p))
#define R16(p) (*(volatile uint16_t *)(uintptr_t)(p))
#define R32(p) (*(volatile uint32_t *)(uintptr_t)(p))
/* read a stored pointer out of a canon obj3 global (value is a runtime address) */
#define PTRG(canon_va) (*(volatile uint32_t *)(uintptr_t)GADDR(canon_va))

/* small bridge into an un-lifted / other-subsystem callee (Watcom EAX/EDX/EBX/ECX) */
#ifdef ROTH_STANDALONE
/* dos_find_first/next (0x41c14/0x41c46) — faithful transcriptions over the g_os_soft_int DOS seam
 * (OBJ1-A boot finding; the host dos.c services AH=0x1a/0x4e/0x4f). first: one-time DTA set to
 * 0x90f4c (guard dword [0x90f48]) then AH=0x4e (EDX=pattern, ECX=attr); next: AH=0x4f. Both return
 * &0x90f6a (the DTA filename field, DTA+0x1e) or 0 on CF. */
static uint32_t fc_dos_find(uint32_t canon_va, uint32_t pattern, uint32_t attr)
{
    regs_t v;
    if (canon_va == 0x41c14u && G32(VA_g_dta_initialized) == 0) {
        memset(&v, 0, sizeof v); v.eax = 0x1a00; v.edx = (uint32_t)GADDR(VA_g_dos_dta);
        g_os_soft_int(0x21, &v);                       /* set DTA (once) */
        G32(VA_g_dta_initialized) = G32(VA_g_dta_initialized) + 1;
    }
    memset(&v, 0, sizeof v);
    v.eax = (canon_va == 0x41c14u) ? 0x4e00u : 0x4f00u;
    v.ecx = attr; v.edx = pattern;
    uint32_t fl = g_os_soft_int(0x21, &v);
    return (fl & 1u) ? 0 : (uint32_t)GADDR(VA_g_dos_dta_name);
}
#endif
static uint32_t fc_bridge(uint32_t canon_va, uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx)
{
    regs_t io; memset(&io, 0, sizeof io);
    io.va = canon_va + OBJ_DELTA;
    io.eax = eax; io.edx = edx; io.ebx = ebx; io.ecx = ecx;
#ifndef ROTH_STANDALONE
    call_orig(&io);
    return io.eax;
#else
    if (canon_va == 0x2e1e8u) { host_flip_video_page(eax); return 0; }  /* flip_video_page: host present */
    if (canon_va == 0x41c14u || canon_va == 0x41c46u)
        return fc_dos_find(canon_va, eax, edx);                         /* dos_find_first/next (EAX=pattern, EDX=attr) */
    if (canon_va == 0x41bc1u) {                                         /* dos_get_file_size: lseek END -> size; lseek SET back */
        uint32_t size = dos_lseek(eax, 0, 2);
        dos_lseek(eax, 0, 0);
        return size;
    }
    roth_unreachable(canon_va);                                        /* off the bare-title path */
    return 0;
#endif
}
/* same, but returns the callee's CF (for DOS loaders that signal via carry) */
static uint32_t fc_bridge_cf(uint32_t canon_va, uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx)
{
    regs_t io; memset(&io, 0, sizeof io);
    io.va = canon_va + OBJ_DELTA;
    io.eax = eax; io.edx = edx; io.ebx = ebx; io.ecx = ecx;
#ifndef ROTH_STANDALONE
    call_orig(&io);
    return io.eflags & 1u;
#else
    if (canon_va == 0x2f4b4u)
        return load_raw_map_file(eax) & 1u;   /* load_raw_map_file (lifted body) -> CF */
    roth_unreachable(canon_va);
    return 0;
#endif
}
/* build_game_path (0x2fb7f [L]): EAX=dest, EDX=src, EBX=third string (disasm 0x2fb81-85) -> direct C. */
static void fc_build_path(uint32_t dest, uint32_t src, uint32_t s3)
{
    build_game_path((uint8_t *)(uintptr_t)dest, (const uint8_t *)(uintptr_t)src,
                           (const uint8_t *)(uintptr_t)s3);
}

/* ---- DOS file-IO seam: raw inline `int 0x21` routed through the host soft-int hook.
 * NULL g_os_soft_int (the oracle default) -> the test installs a mock; in the host it is the
 * real int21 handler (real file IO). Mirrors lift_das_assets.c's da_int21_* helpers. ---- */
static uint32_t fc_int21_open(uint32_t path)     /* AX=3D00 open read-only -> EAX handle; ret CF */
{
    regs_t r; memset(&r, 0, sizeof r);
    r.eax = 0x3d00; r.edx = path;
    uint32_t cf = g_os_soft_int ? g_os_soft_int(0x21, &r) : 1u;
    return cf & 1 ? 0xffffffffu : r.eax;             /* caller checks == 0xffffffff for CF */
}
static int fc_int21_read(uint32_t handle, uint32_t buf, uint32_t count, uint32_t *bytes_out)
{                                                    /* AH=3F read -> EAX bytes; ret CF */
    regs_t r; memset(&r, 0, sizeof r);
    r.eax = 0x3f00; r.ebx = handle; r.edx = buf; r.ecx = count;
    uint32_t cf = g_os_soft_int ? g_os_soft_int(0x21, &r) : 1u;
    if (bytes_out) *bytes_out = r.eax;
    return (int)(cf & 1);
}
static void fc_int21_close(uint32_t handle)          /* AH=3E close */
{
    regs_t r; memset(&r, 0, sizeof r);
    r.eax = 0x3e00; r.ebx = handle;
    if (g_os_soft_int) g_os_soft_int(0x21, &r);
}

/* ================================================================================================
 * Cluster A — the CONFIG.INI parse stack (Watcom shared-EBP-frame family).
 *
 *   parse_config_keywords 0x41c5c  (frame owner; opens+reads the file via int21, then parses)
 *     -> parse_config_line 0x41d44        (per-line tokenizer; matches keyword template)
 *          -> parse_config_typed_value 0x41dec  (one value, per TYPE LETTER C/S/L/A/P/F)
 *               -> parse_config_number 0x41ed1  (integer leaf: decimal or 0x-hex)
 *
 * The inner three are pure (no int21); they are lifted with explicit C parameters standing in for
 * the shared frame slots the original reads via [ebp+..]:
 *   [ebp+8]  = keyword template string   (parse_config_line)
 *   [ebp+0x10] = result struct base      (parse_config_line)
 *   [ebp+0x14] = string-storage bump ptr (parse_config_typed_value, read+written)
 * ================================================================================================ */

/* parse_config_number (0x41ed1, 81 B) — leaf. EDI = value string, ECX = remaining count.
 * If the 2nd char upper-cased is 'X' (i.e. "0x.."/"?x..") read hex digits, else decimal; stop at
 * a byte <= 0x20 or ';' (hex) / first non-digit (decimal). Returns the accumulated value in EAX.
 * EDI/ECX are also live outputs: the caller (parse_config_line, via typed_value) resumes its
 * newline scan from them, so we thread them out exactly (incl. the decimal count-exhaust `inc ecx`
 * undo at 0x41f07 that the hex path does NOT do). */
int32_t parse_config_number(uint8_t **edi_io, int32_t *ecx_io)
{
    uint8_t *edi = *edi_io;
    int32_t  ecx = *ecx_io;
    int32_t  acc = 0;                                  /* edx accumulator */
    if (((edi[1]) & 0xdf) == 0x58) {                   /* 'X' -> hex */
        edi += 2;                                      /* add edi,2 */
        ecx -= 2;                                      /* sub ecx,2 */
        for (;;) {
            uint8_t c = *edi;
            if (c <= 0x20 || c == 0x3b) break;         /* cmp al,0x20 jbe / cmp 0x3b je (edi at c) */
            uint8_t d = (uint8_t)(c - 0x30);
            if (d > 9) {                               /* not 0-9 -> try A-F */
                d = (uint8_t)((c & 0xdf) - 0x37);
                if (d > 0xf) break;
            }
            if (--ecx <= 0) break;                     /* dec ecx; jle (edi NOT advanced) */
            edi++;
            acc = (acc << 4) | d;                      /* shl edx,4; or dl,al */
        }
    } else {                                           /* decimal */
        for (;;) {
            uint8_t d = (uint8_t)(*edi - 0x30);
            if (d > 9) break;                          /* cmp al,9; ja (edi at non-digit) */
            acc = acc * 10 + d;                        /* lea edx,[edx*5]; add edx,edx; add edx,eax */
            edi++;
            if (--ecx <= 0) { ecx++; break; }          /* dec ecx; jle 0x41f07 (inc ecx undo) */
        }
    }
    *edi_io = edi;
    *ecx_io = ecx;
    return acc;
}

/* parse_config_typed_value (0x41dec, 229 B) — parse one value per its TYPE LETTER into *field:
 *   'C'=byte, 'S'=word, default/'L'=dword (all via parse_config_number); 'F'=flag (dword -1);
 *   'A'=ascii string, 'P'=path string (appends a trailing '\' if missing). String parsing handles
 * a double-quote delimiter, space-delimited words, ';' comments (unquoted), and {..} brace-quoted
 * values (collapsing runs of whitespace, control chars -> space, skipping ';' comment tails). The
 * string is bump-allocated at *bump (the frame's [ebp+0x14] slot); *field gets the string pointer.
 * Returns the advanced EDI; *ecx_io + *bump updated. Goto-labelled to mirror the disasm exactly. */
uint8_t *parse_config_typed_value(uint8_t type, uint8_t *field, uint8_t *edi,
                                         int32_t *ecx_io, uint8_t **bump)
{
    int32_t  ecx = *ecx_io;
    int32_t  v;
    uint8_t  al = 0, ah = 0, bl;
    uint8_t *esi;
    int      is_path;

    if (type == 'C') { v = parse_config_number(&edi, &ecx); *(int8_t  *)field = (int8_t)v;  goto done; }
    if (type == 'S') { v = parse_config_number(&edi, &ecx); *(int16_t *)field = (int16_t)v; goto done; }
    if (type == 'A') { is_path = 0; goto str; }
    if (type == 'P') { is_path = 1; goto str; }
    if (type == 'F') { *(int32_t *)field = (int32_t)0xffffffffu; goto done; }
    v = parse_config_number(&edi, &ecx); *(int32_t *)field = v;              /* default 'L' */
    goto done;

str:                                                    /* 0x41e1e */
    esi = *bump;
    *(uint32_t *)field = (uint32_t)(uintptr_t)esi;       /* field = &string (mov [edx],esi) */
    bl = *edi;                                           /* delimiter candidate */
    if (bl != 0x22) { bl = 0x20; edi--; }                /* 0x41e2d: no quote -> space-delim, back up */
    ecx++;                                               /* 0x41e30 */
    ah = 0;                                              /* eax=0 -> ah=0 */

L_skipws:                                                /* 0x41e33 */
    edi++;
    if (--ecx <= 0) goto L_62;                           /* jle 0x41e62 */
    al = *edi;
    if (al == 0x20 || al == 9) goto L_skipws;
    if (al == 0x7b) goto L_brace;                        /* '{' */
L_emittop:                                               /* 0x41e45 */
    ah = al;
    al = *edi;
    edi++;
    if (--ecx <= 0) goto L_64;                           /* jle 0x41e64 (no edi/ecx undo) */
    if (al < 0x20) goto L_62;                            /* jb 0x41e62 */
    if (al == bl) goto L_64;                             /* je 0x41e64 */
    if (bl == 0 && al == 0x3b) goto L_62;                /* unquoted ';' -> stop */
    *esi++ = al;                                         /* 0x41e5d */
    goto L_emittop;

L_62:                                                    /* 0x41e62 */
    ecx++;
    edi--;
L_64:                                                    /* 0x41e64 */
    if (is_path != 0 && ah != 0x5c) *esi++ = 0x5c;       /* path: append '\' if missing */
    goto L_72;

L_brace:                                                 /* 0x41e7a */
    edi++;
    ecx--;
    ah = 0x20;
L_brace_top:                                             /* 0x41e7f */
    al = *edi;
    if (al == 0) goto L_72;
    if (al == 0x3b) goto L_bcomment;
    if (al == 0x7d) goto L_72;
    if (al <= 0x1f) al = 0x20;                           /* control -> space */
    if (!(al == 0x20 && ah == al)) *esi++ = al;          /* emit unless collapsing a double space */
    edi++;
    ah = al;
    if (--ecx > 0) goto L_brace_top;                     /* dec ecx; jg */
    goto L_72;
L_bcomment:                                              /* 0x41ea6 */
    edi++;
    if (--ecx <= 0) goto L_brace_top;                    /* jle 0x41e7f */
    if (*edi > 0x1f) goto L_bcomment;
    goto L_brace_top;

L_72:                                                    /* 0x41e72 */
    *esi = 0;
    esi++;
    *bump = esi;
done:
    *ecx_io = ecx;
    return edi;
}

/* parse_config_line (0x41d44, 155 B) — tokenize CONFIG.INI text one line at a time: skip leading
 * whitespace and ';' comment lines, match the line's keyword against the template (esi=[ebp+8],
 * case-insensitive up to '='), track the destination field offset (struct=[ebp+0x10], +1/+2/+4 per
 * type letter C/S/other), and dispatch the value to parse_config_typed_value. Loops over every line
 * until ECX (bytes) is consumed. Goto-labelled to mirror the disasm. */
uint8_t *parse_config_line(uint8_t *edi, int32_t *ecx_io, uint8_t *tmpl,
                                  uint8_t *struct_base, uint8_t **bump)
{
    int32_t  ecx = *ecx_io;
    uint8_t  al;
    uint8_t *esi;                                        /* template cursor */
    uint8_t *field;                                      /* edx: destination field ptr */
    int32_t  ebx;                                        /* keyword match index */

L_top:                                                   /* 0x41d44 */
    al = *edi;
    if (al <= 0x20) {                                    /* leading whitespace */
        edi++;
        if (--ecx <= 0) goto L_end;                      /* dec ecx; jle 0x41deb */
        goto L_top;
    }
    if (*edi == 0x3b) goto L_nl;                          /* 0x41d56: ';' comment line */
    esi   = tmpl;                                        /* [ebp+8]  */
    field = struct_base;                                 /* [ebp+0x10] */
L_field:                                                 /* 0x41d60 */
    if (*esi == 0) goto L_nl;                             /* template exhausted */
    ebx = 0;
L_kwmatch:                                               /* 0x41d67 */
    al = edi[ebx];
    if (al >= 0x61 && al <= 0x7a) al &= 0xdf;             /* uppercase the input char */
    if (al != esi[ebx]) goto L_mismatch;
    ebx++;
    if (al == 0x3d) goto L_matched;                      /* matched through '=' */
    goto L_kwmatch;
L_mismatch:                                              /* 0x41d80 */
    if (esi[ebx] == 0x3d) {                              /* template is at '=' here */
        if (edi[ebx] <= 0x20) goto L_partial;            /* input ran out at the '=' */
        goto L_skipval;
    }
    do { ebx++; } while (esi[ebx] != 0x3d);              /* skip to this entry's '=' (0x41d8e) */
L_skipval:                                               /* 0x41d95 */
    esi = esi + ebx + 1;                                 /* -> the type letter */
    al = *esi++;
    field++;                                             /* inc edx */
    if (al == 0x43) goto L_afterinc;                     /* 'C' -> +1 total */
    field++;
    if (al == 0x53) goto L_afterinc;                     /* 'S' -> +2 total */
    field += 2;                                          /* else -> +4 total */
L_afterinc:                                              /* 0x41da7 */
    al = *esi++;
    if (al == 0x3b) goto L_field;                        /* ';' -> next template entry */
    goto L_nl;
L_partial:                                               /* 0x41dae */
    ebx++;
L_matched:                                               /* 0x41daf */
    al = esi[ebx];                                       /* type letter (char after '=') */
    edi += ebx;                                          /* advance past keyword+'=' */
    ecx -= ebx;
    edi = parse_config_typed_value(al, field, edi, &ecx, bump);
L_nl:                                                    /* 0x41dbd: run to end of line */
    for (;;) {
        al = *edi;
        if (al == 0xa || al == 0xd) break;
        edi++;
        ecx--;                                           /* NOTE: no ecx<=0 guard (faithful) */
    }
    /* 0x41dcb: consume the newline run, then loop to the next line */
    for (;;) {
        edi++;
        if (--ecx <= 0) goto L_end;
        al = *edi;
        if (al == 0xa || al == 0xd) continue;
        goto L_top;
    }
L_end:                                                   /* 0x41deb */
    *ecx_io = ecx;
    return edi;
}

/* parse_config_keywords (0x41c5c, 232 B) — the family's frame owner + file loader. EAX=filename,
 * EDX=keyword template, EBX=result struct base, ECX=total buffer capacity. Computes the struct
 * size from the template (1/2/4 bytes per '='-entry by type letter, rounded up to 4), zeroes it,
 * opens `filename` (falling back to <exe-dir>\filename via the CRT arg0 path @0x7253c on failure),
 * reads the file text into struct+size+0x80, null-terminates, and runs parse_config_line over it.
 * Returns the struct base (EAX), or 0 on any open/size failure. int21 open/read/close are the only
 * DOS seam -> g_os_soft_int. LIVE-SWAP (real file IO in-game); oracle via an int21 mock. */
uint32_t parse_config_keywords(uint8_t *filename, uint8_t *tmpl,
                                      uint8_t *struct_base, int32_t bufsize)
{
    /* --- template -> struct size (0x41c73) --- */
    int32_t sz = 0;
    const uint8_t *t = tmpl;
    for (;;) {
        uint8_t c = *t++;
        if (c == 0) break;
        if (c != 0x3d) continue;                     /* only count past each '=' */
        uint8_t ty = *t++;                           /* type letter after '=' */
        sz += 1;
        if (ty == 0x43) continue;                    /* 'C' -> +1 */
        sz += 1;
        if (ty == 0x53) continue;                    /* 'S' -> +2 */
        sz += 2;                                     /* else -> +4 */
    }
    sz += 3;                                         /* add ecx,3 */
    sz = (int32_t)((sz & ~0xff) | ((sz & 0xff) & 0xfc));  /* and cl,0xfc (byte-faithful round-to-4) */
    if (sz == 0) return 0;                           /* empty template -> bail (return 0) */

    /* --- zero the struct (0x41ca1) --- */
    memset(struct_base, 0, (size_t)sz);
    uint8_t *bumparea = struct_base + sz;            /* [ebp+0x14] */

    /* --- open filename, else <exe-dir>\filename (0x41caf) --- */
    uint32_t handle = fc_int21_open((uint32_t)(uintptr_t)filename);
    if (handle == 0xffffffffu) {
        /* build the prefixed path into bumparea, tracking the char after the last '\' */
        uint8_t *edi = bumparea;
        uint8_t *after_sep = edi;
        const uint8_t *s = (const uint8_t *)(uintptr_t)PTRG(0x7253c);  /* CRT arg0 (exe) path */
        for (;;) {
            uint8_t c = *s++;
            if (c == 0) break;
            *edi++ = c;
            if (c == 0x5c) after_sep = edi;          /* remember position after the last '\' */
        }
        edi = after_sep;                             /* rewind: drop the exe basename */
        const uint8_t *fn = filename;
        do { *edi = *fn; edi++; } while (*fn++ != 0); /* append filename incl. NUL */
        handle = fc_int21_open((uint32_t)(uintptr_t)bumparea);
        if (handle == 0xffffffffu) return 0;
    }

    /* --- read the file text (0x41ce9) --- */
    int32_t maxread = bufsize - sz - 0x80;
    if (maxread <= 0) return 0;
    uint8_t *textbuf = bumparea + 0x80;
    uint32_t bytes = 0;
    if (fc_int21_read(handle, (uint32_t)(uintptr_t)textbuf, (uint32_t)maxread, &bytes)) {
        fc_int21_close(handle);                      /* read error */
        return 0;
    }
    if ((int32_t)bytes == maxread) {                 /* filled the buffer (file too big) -> fail */
        fc_int21_close(handle);
        return 0;
    }
    fc_int21_close(handle);
    textbuf[bytes] = 0;                              /* null-terminate */

    /* --- parse (0x41d26) --- */
    int32_t ecx = (int32_t)bytes;
    uint8_t *bump = bumparea;
    parse_config_line(textbuf, &ecx, tmpl, struct_base, &bump);
    return (uint32_t)(uintptr_t)struct_base;
}

/* ================================================================================================
 * Cluster B — leaf file utilities (thin wrappers over the DOS IO layer + memory allocators).
 * All are DOS-side-effecting -> registered for the in-game live-swap tier (the DOS bridges run
 * the real int21 in the host). ABI derived from the disasm at each call site.
 * ================================================================================================ */

/* dispatch_arg_command (0x107b3, 51 B) — walk the cmdline-switch table @0x704a5 for a record whose
 * token (record+6, DX/2+1 words) matches the current arg token @0x76718; on a hit clear CF and call
 * the record's handler ([record+2]); on no match set CF. DX = token byte length. Returns via CF
 * (found/handled = CF clear). The handler is an indirect call to another engine fn -> bridged. */
uint32_t dispatch_arg_command(uint32_t dx)
{
    uint8_t *esi = (uint8_t *)(uintptr_t)GADDR(VA_g_arg_command_table);
    int32_t words = (int32_t)(((dx & 0xffff) + 2) >> 1);           /* movzx ecx,dx; add 2; shr 1 */
    const uint16_t *tok = (const uint16_t *)(uintptr_t)GADDR(VA_g_arg_token_buf);
    for (;;) {
        if (*(uint16_t *)esi == 0) return 1;                       /* end of table -> CF set (not found) */
        const uint16_t *rec = (const uint16_t *)(esi + 6);
        int match = 1;
        for (int32_t i = 0; i < words; i++)                        /* repe cmpsw over `words` words */
            if (rec[i] != tok[i]) { match = 0; break; }
        if (match) {
            uint32_t handler = *(uint32_t *)(esi + 2);             /* [record+2] = handler VA (rebased) */
            regs_t io; memset(&io, 0, sizeof io);
            io.va = handler;                                       /* clc; call [esi+2] */
#ifndef ROTH_STANDALONE
            call_orig(&io);
#else
            roth_unreachable(handler - OBJ_DELTA);                /* arg-command handler code-ptr (reloc) */
#endif
            return 0;                                              /* CF clear (found + handled) */
        }
        esi += *(uint16_t *)esi;                                   /* advance by record length */
    }
}

/* load_raw_file_wrapper (0x10c13, 31 B) — build "<0x764a0 prefix>NAME" into a stack path buffer via
 * build_game_path, then load it via load_raw_map_file. EAX = name. Returns CF (all 3 callers `jb`).
 * Both callees bridged (build_game_path is [L]; load_raw_map_file is map_load DOS IO -> in-game). */
uint32_t load_raw_file_wrapper(uint32_t name)
{
    uint8_t pathbuf[0x78];
    fc_build_path((uint32_t)(uintptr_t)pathbuf, (uint32_t)GADDR(VA_g_dir_digi + 0x50), name);         /* build_game_path */
    return fc_bridge_cf(0x2f4b4, (uint32_t)(uintptr_t)pathbuf, 0, 0, 0);                 /* load_raw_map_file -> CF */
}

/* load_file_fully (0x1522d, 83 B) — open EAX=filename (read-only), get its size (stashed @0x7f464),
 * alloc a buffer (pool-or-heap), read the whole file into it, close. Returns the buffer (EAX) or 0.
 * All callees are DOS/memory bridges. */
uint32_t load_file_fully(uint32_t filename)
{
    uint32_t handle = dos_open_file(filename, 0);           /* dos_open_file(name, mode 0) (C2) */
    if (handle == 0) return 0;
    G32(VA_g_dbase300_chunk_buf + 0x14) = (int32_t)fc_bridge(0x41bc1, handle, 0, 0, 0);    /* dos_get_file_size -> [0x7f464] (OOS-bridged) */
    uint32_t buf = alloc_block_or_heap((int32_t)G32(VA_g_dbase300_chunk_buf + 0x14)); /* 0x15210 alloc_block_or_heap */
    if (buf == 0) { dos_close_handle(handle); return 0; }    /* alloc fail -> close, 0 (C2) */
    dos_read_items(buf, (uint32_t)G32(VA_g_dbase300_chunk_buf + 0x14), 1, handle);  /* dos_read_items(buf,size,1,handle) (C2) */
    dos_close_handle(handle);                               /* dos_close_handle (C2) */
    return buf;
}

/* load_file_blob (0x143b0, 102 B) — raw-int21 loader: open EAX=filename, lseek END to get the size,
 * lseek back to 0, make DAS-cache room + game_heap_alloc a buffer, read the whole file, close.
 * Returns the buffer (EAX) or 0. The int21 seam -> g_os_soft_int; cache/heap -> bridges. */
uint32_t load_file_blob(uint32_t filename)
{
    regs_t r;
    /* open read-only */
    memset(&r, 0, sizeof r); r.eax = 0x3d00; r.edx = filename;
    uint32_t handle = 0;
    if ((g_os_soft_int ? g_os_soft_int(0x21, &r) : 1u) & 1u) return 0;
    handle = r.eax;
    /* lseek END -> DX:AX = size */
    memset(&r, 0, sizeof r); r.eax = 0x4202; r.ebx = handle;
    if ((g_os_soft_int ? g_os_soft_int(0x21, &r) : 1u) & 1u) goto fail;
    uint32_t size = (((uint32_t)r.edx & 0xffffu) << 16) | ((uint32_t)r.eax & 0xffffu);
    /* lseek SET 0 */
    memset(&r, 0, sizeof r); r.eax = 0x4200; r.ebx = handle;
    if ((g_os_soft_int ? g_os_soft_int(0x21, &r) : 1u) & 1u) goto fail;
    das_cache_make_room(size);                              /* 0x414b6 (need=EAX=size; orig 0x143e4) */
    /* game_heap_alloc_round4(size): EAX=ptr, CF on failure. Disasm 0x40a24/0x40a28: CF <=> ptr==0. */
    uint32_t buf = game_heap_alloc_round4((int32_t)size);   /* 0x40a17 */
    if (buf == 0) goto fail;                                       /* jb 0x14412 */
    /* read `size` bytes into buf (int21 AH=3F: BX=handle, CX=size, DX=buf) */
    memset(&r, 0, sizeof r); r.eax = 0x3f00; r.ebx = handle; r.ecx = size; r.edx = buf;
    if ((g_os_soft_int ? g_os_soft_int(0x21, &r) : 1u) & 1u) { /* read error */
        game_free_if_not_null((uint8_t *)(uintptr_t)buf);   /* 0x40a2a */
        goto fail;
    }
    if (handle != 0) { memset(&r, 0, sizeof r); r.eax = 0x3e00; r.ebx = handle;   /* close */
        if (g_os_soft_int) g_os_soft_int(0x21, &r); }
    return buf;
fail:
    if (handle != 0) { memset(&r, 0, sizeof r); r.eax = 0x3e00; r.ebx = handle;
        if (g_os_soft_int) g_os_soft_int(0x21, &r); }
    return 0;
}

/* enumerate_files_by_pattern (0x217bc, 74 B) — dos_find_first/next over EAX=pattern, copying each
 * match's 14-byte name into the EDX dest buffer (stride 0xe). Returns the match count (EAX). */
uint32_t enumerate_files_by_pattern(uint32_t pattern, uint32_t dest)
{
    uint32_t count = 0;
    uint32_t rec = fc_bridge(0x41c14, pattern, 0, 0, 0);           /* dos_find_first(pattern, 0) */
    while (rec != 0) {
        count++;
        memcpy((void *)(uintptr_t)dest, (const void *)(uintptr_t)rec, 0xe);  /* copy 14-byte name */
        dest += 0xe;
        rec = fc_bridge(0x41c46, pattern, 0, 0, 0);               /* dos_find_next(pattern) */
    }
    return count;
}

/* ================================================================================================
 * Cluster C — config-field dispatch + mid-level loaders (interactive console / DOS IO).
 * All in-game live-swap tier: console editors need real keystrokes; the loaders do DOS IO + audio.
 * ================================================================================================ */

/* bridge that also presets ESI (+ECX) and returns the callee's CF — the console field editors read
 * their field descriptor from ESI and max length from ECX, and signal completion via carry. */
static uint32_t fc_bridge_esi_cf(uint32_t canon_va, uint32_t esi, uint32_t ecx)
{
    regs_t io; memset(&io, 0, sizeof io);
    io.va = canon_va + OBJ_DELTA;
    io.esi = esi; io.ecx = ecx;
#ifndef ROTH_STANDALONE
    call_orig(&io);
    return io.eflags & 1u;
#else
    roth_unreachable(canon_va);   /* config-field parse (ESI/CF) — off the bare-title path */
    return 0;
#endif
}

/* shared body of the two config-field dispatchers (0x11382 flow_succ<-0x1134d): the key arrives
 * in AL as an INPUT (the console loop at 0x11224/0x112aa reads it) — 0x11337 only ECHOES AL+LF+CR
 * and push/pop-PRESERVES EAX, then the dispatcher compares AL. 'N' uses a per-entry descriptor;
 * 'P' = path field; 'F'/'C' = numeric fields ('C' only from the primary entry). Returns CF (the
 * editor's completion flag; unknown key -> CF set).
 * The old reconstruction bridged 0x11337 with eax=0 and read the
 * PRESERVED eax back as the "key" -> always 0 -> every key fell through to stc. The key is now a
 * parameter threaded from the dispatcher's own EAX (registry: ABI_CF -> ABI_EAX_CF). */
static uint32_t fc_config_field_dispatch(uint32_t key_eax, uint32_t n_desc, int has_c)
{
    console_read_key_crlf(key_eax);           /* re-point 0x11337: echo AL + LF + CR (EAX preserved) */
    uint32_t key = key_eax & 0xff;
    if (key == 0x4e) return fc_bridge_esi_cf(0x113b9, (uint32_t)GADDR(n_desc), 4);      /* 'N' text  */
    if (key == 0x50) return fc_bridge_esi_cf(0x113b9, (uint32_t)GADDR(VA_g_arg_command_table + 0x23e), 0x3c);  /* 'P' path  */
    if (key == 0x46) return fc_bridge_esi_cf(0x11462, (uint32_t)GADDR(VA_g_snapshot_anim_frame), 0);     /* 'F' numeric */
    if (has_c && key == 0x43)
        return fc_bridge_esi_cf(0x11462, (uint32_t)GADDR(VA_g_snapshot_anim_frame + 0x2), 0);                  /* 'C' numeric */
    return 1;                                                        /* stc (unknown key) */
}
uint32_t dispatch_config_field_key(uint32_t eax)     { return fc_config_field_dispatch(eax, 0x70738, 1); } /* 0x11382 */
uint32_t dispatch_config_field_key_alt(uint32_t eax) { return fc_config_field_dispatch(eax, 0x70733, 0); } /* 0x1134d */

/* delete_temp_files (0x21806, 115 B) — build "<dir>*.TMP" (pattern @0x75e78), enumerate the matches,
 * then for each build "<dir><prefix @0x75e82><name>" and dos_delete_file it. No args, void. */
void delete_temp_files(void)
{
    uint8_t pathbuf[0x100];
    uint8_t namelist[0x300];
    fc_build_path((uint32_t)(uintptr_t)pathbuf,
                  (uint32_t)GADDR(VA_g_dir_gdv + 0x50), (uint32_t)GADDR(VA_g_heap_free_list + 0x868));  /* build_game_path(pat) */
    uint32_t count = enumerate_files_by_pattern(
        (uint32_t)(uintptr_t)pathbuf, (uint32_t)(uintptr_t)namelist);
    uint8_t *name = namelist;
    while (count != 0) {                                              /* test esi; ja (unsigned >0) */
        fc_build_path((uint32_t)(uintptr_t)pathbuf,
                      (uint32_t)GADDR(VA_g_dir_gdv + 0x50), (uint32_t)GADDR(VA_g_heap_free_list + 0x872));       /* build prefix */
        fc_build_path((uint32_t)(uintptr_t)pathbuf,
                      (uint32_t)(uintptr_t)pathbuf, (uint32_t)(uintptr_t)name);  /* append name */
        count--;                                                     /* dec esi (before delete) */
        os_dos_delete((uint32_t)(uintptr_t)pathbuf);                 /* was fc_bridge(0x41ad4): dos_delete_file (int21 41), return unused */
        name += 0xe;
    }
}

/* load_dbase300_chunk (0x15492, 205 B) — load resource chunk EAX=index from the dbase300 archive
 * (@0x81f86) into the dest buffer @0x7f450: cache-check @0x7f46c, stop music, open (retry via the
 * resource-error box @0x2632a), lseek index*8, read the 4-byte size, bail if >0x11800, read the
 * chunk, close, set the loaded flag @0x7f468. Returns 0 = freshly loaded, 1 = already-loaded/failed.
 * DOS IO + audio + UI -> in-game live-swap. */
uint32_t load_dbase300_chunk(uint32_t index)
{
    if ((int32_t)index == G32(VA_g_current_dbase300_chunk_id)) return 1;                    /* already loaded */
    G32(VA_g_current_dbase300_chunk_id) = (int32_t)index;
    stop_music_sequence();                                    /* 0x15630 */
    if (index == 0) return 0;                                        /* index 0 -> eax=0 */
    if (G32(VA_g_audio_sequence_ctx + 0x8) == 0) return 1;

    uint32_t handle;
    for (;;) {                                                       /* open with error-box retry */
        handle = dos_open_file((uint32_t)GADDR(VA_g_dbase300_filename), 0); /* dos_open_file(archive,0) (C2) */
        if (handle != 0) break;
        if (G32(VA_g_current_dbase300_chunk_id + 0x4) != 0) break;
        uint32_t choice = show_resource_error_box();          /* 0x2632a -> button EAX */
        if (choice == 2) G32(VA_g_current_dbase300_chunk_id + 0x4)++;
        if (choice != 1) break;                                      /* not "retry" -> give up */
    }
    if (handle == 0) return 1;

    dos_lseek(handle, index << 3, 0);                        /* dos_lseek(handle, index*8, whence 0=SET) (C2) */
    int32_t size = 0;
    dos_read_items((uint32_t)(uintptr_t)&size, 1, 4, handle);   /* dos_read_items(&size,1,4,handle) (C2) */
    if (size > 0x11800) return 0;                                    /* too big -> eax=0 */
    uint32_t items = dos_read_items((uint32_t)G32(VA_g_dbase300_chunk_buf),   /* read `size` bytes -> dest (C2) */
                                           (uint32_t)size, 1, handle);
    dos_close_handle(handle);                               /* dos_close_handle (C2) */
    if (items != 1) return 1;
    G32(VA_g_audio_sequence_progress) = 1;                                                /* loaded flag */
    return 0;
}

/* ================================================================================================
 * Cluster D — config entry points (startup / settings; DOS IO + pool + UI -> in-game).
 * ================================================================================================ */

/* copy_path_ensure_trailing_slash (0x11057 [L]): ESI=src, EDI=dest -> direct C. */
static void fc_copy_path(uint32_t src, uint32_t dst)
{
    copy_path_ensure_trailing_slash((const uint8_t *)(uintptr_t)src, (uint8_t *)(uintptr_t)dst);
}
/* set_cfg_asset_name (0x10584 [L]): ESI=src, EDI=dest, EDX=extension -> direct C. */
static void fc_set_cfg_asset(uint32_t src, uint32_t dst, uint32_t ext)
{
    set_cfg_asset_name((const uint8_t *)(uintptr_t)src, (uint8_t *)(uintptr_t)dst, ext);
}

/* parse_config_ini_paths (0x10f6c, 235 B) — parse the path-config INI (@0x10f50, template @0x10f21)
 * into a stack struct, then copy its path fields into the g_dir_* prefix globals (with trailing '\'
 * normalization), and finally build_game_path the DATA + level dirs. `bp` = first 2 chars of field
 * [struct+8] upper-cased; when == "MI" the primary source is field[0] else field[4]. No args, void. */
void parse_config_ini_paths(void)
{
    uint8_t sbuf[0x400];
    uint32_t r = parse_config_keywords(
        (uint8_t *)(uintptr_t)GADDR(VA_g_config_string_pool + 0x2f), (uint8_t *)(uintptr_t)GADDR(VA_g_config_string_pool), sbuf, 0x400);
    if (r != 0) {
        uint8_t *ebx = (uint8_t *)(uintptr_t)r;
        uint32_t f0 = *(uint32_t *)(ebx);
        uint32_t f4 = *(uint32_t *)(ebx + 4);
        uint32_t f8 = *(uint32_t *)(ebx + 8);
        uint16_t bp = (uint16_t)(R16(f8) & 0xdfdf);              /* first 2 chars, upper-cased */
        fc_copy_path(f4, (uint32_t)GADDR(VA_g_dir_gdv + 0x50));
        fc_copy_path(f0, (uint32_t)GADDR(VA_g_dir_gdv + 0xa0));
        fc_copy_path(f4, (uint32_t)GADDR(VA_g_dir_digi));
        if (G8(VA_g_dir_gdv) == 0) {
            fc_copy_path(f0, (uint32_t)GADDR(VA_g_dir_gdv));
            fc_build_path((uint32_t)GADDR(VA_g_dir_gdv),
                          (uint32_t)GADDR(VA_g_dir_gdv), (uint32_t)GADDR(VA_g_config_string_pool + 0x46));  /* build_game_path */
        }
        uint32_t src = (bp == 0x494d) ? f0 : f4;                 /* "MI" -> field[0] else field[4] */
        fc_copy_path(src, (uint32_t)GADDR(VA_g_dir_digi + 0x50));
        fc_copy_path(src, (uint32_t)GADDR(VA_g_dir_data));
        src = (bp == 0x494d) ? f0 : f4;
        fc_copy_path(src, (uint32_t)GADDR(VA_g_dir_gdv + 0xf0));
        fc_copy_path(f4, (uint32_t)GADDR(VA_g_dir_midi));
    }
    fc_build_path((uint32_t)GADDR(VA_g_dir_midi),
                  (uint32_t)GADDR(VA_g_dir_midi), (uint32_t)GADDR(VA_g_config_string_pool + 0x3a));          /* build DATA dir */
    fc_build_path((uint32_t)GADDR(VA_g_dir_digi),
                  (uint32_t)GADDR(VA_g_dir_digi), (uint32_t)GADDR(VA_g_config_string_pool + 0x40));          /* build level dir */
}

/* load_disk_path_config (0x26965, 187 B) — open the multi-disk path file ("DATA\FILELIST.TXT"),
 * prompting retry on failure; pool_alloc_handle a block sized file+200; parse the
 * "DISK1 A DISK2 A DISK3 A DISK4 A" template into it (freeing on parse failure). EAX=p1, EDX=p2;
 * returns EAX = 0 (ok) / -1 (abort start), EDX passthrough. In-game (DOS + pool + UI). */
uint32_t load_disk_path_config(uint32_t p1, uint32_t p2)
{
    (void)p1; (void)p2;
    uint8_t pathbuf[0xc8];
    G32(VA_g_playback_menu_scroll_anchor + 0x8) = 0;
    fc_build_path((uint32_t)(uintptr_t)pathbuf,
                  (uint32_t)GADDR(VA_g_dir_gdv + 0xa0), (uint32_t)GADDR(VA_g_heap_free_list + 0xa35));          /* build FILELIST path */
    uint32_t handle;
    for (;;) {
        handle = dos_open_file((uint32_t)(uintptr_t)pathbuf, 0);         /* dos_open_file (C2) */
        if (handle != 0) break;
        uint32_t choice = show_simple_message_box();                     /* 0x2633c -> button EAX */
        if (choice != 1) return 0xffffffffu;                                    /* abort */
        /* choice == 1 -> retry the open */
    }
    uint32_t size = fc_bridge(0x41bc1, handle, 0, 0, 0);                        /* dos_get_file_size (OOS-bridged) */
    G32(VA_g_playback_menu_scroll_anchor + 0x4) = (int32_t)pool_alloc_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle),
                                                     (int32_t)(size + 0xc8)); /* 0x360f9 pool_alloc_handle */
    dos_close_handle(handle);                                            /* dos_close_handle (C2) */
    if (G32(VA_g_playback_menu_scroll_anchor + 0x4) != 0) {
        uint32_t block = *(uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_playback_menu_scroll_anchor + 0x4);        /* *handle = block ptr */
        uint32_t ok = parse_config_keywords(
            pathbuf, (uint8_t *)(uintptr_t)GADDR(VA_g_heap_free_list + 0xa47), (uint8_t *)(uintptr_t)block, (int32_t)(size + 0xc8));
        if (ok == 0) {
            pool_free_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle),
                                    (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_playback_menu_scroll_anchor + 0x4)); /* 0x360b3 pool_free_handle */
            G32(VA_g_playback_menu_scroll_anchor + 0x4) = 0;
        }
    }
    return 0;
}

/* write_roth_ini (0x266ec, 264 B) — serialize the in-game settings to ROTH.INI: sprintf the ON/OFF
 * flags + video/volume values (the game's own sprintf, for the Watcom %D/%x specifiers), build the
 * path, open for write, write, close. Shares its epilogue with show_cutscene_playback_menu (Watcom
 * code-sharing, not a call — truncate at return). No args, void. In-game (DOS write). */
void write_roth_ini(void)
{
    uint8_t sbuf[0x4c8];
    uint8_t pathbuf[0xc8];
#ifndef ROTH_STANDALONE
    int (*game_sprintf)(char *, const char *, ...) =
        (int (*)(char *, const char *, ...))(uintptr_t)GADDR(0x27c53);
#else
    /* raw code-ptr into obj1 — invisible to both the nm-clean gate and the closure oracle; must be
     * re-pointed by hand (the one fn-ptr-call sprintf site). Template 0x75f21 = %s/%D/%x only. */
    int (*game_sprintf)(char *, const char *, ...) = roth_sprintf;
#endif
#define FC_ONOFF(g, off_s, on_s) ((const char *)(uintptr_t)(G32(g) == 0 ? GADDR(on_s) : GADDR(off_s)))
    int len = game_sprintf((char *)sbuf, (const char *)(uintptr_t)GADDR(VA_g_heap_free_list + 0x911),
        FC_ONOFF(0x83e90, 0x75f1a, 0x75f1e),   /* SpeechSub */
        FC_ONOFF(0x83e94, 0x75f13, 0x75f17),   /* SpeechAud */
        FC_ONOFF(0x83e98, 0x75f0c, 0x75f10),   /* MovieSub  */
        FC_ONOFF(0x83e9c, 0x75f05, 0x75f09),   /* MovieAud  */
        (uint32_t)G32(VA_g_selected_video_mode),                /* VideoMode %D */
        (uint32_t)G8(VA_g_cfg_das2_arg + 0x1be),                 /* ViewSize  %D */
        (uint32_t)G32(VA_g_vol_soundfx) & 0xfff,        /* SoundFXVol %x */
        (uint32_t)G32(VA_g_vol_speech) & 0xfff,        /* SpeechVol  */
        (uint32_t)G32(VA_g_vol_movie) & 0xfff,        /* MovieVol   */
        (uint32_t)G32(VA_g_vol_music) & 0xfff,        /* MusicVol   */
        (uint32_t)G32(VA_g_mouse_speed) & 0xfff);       /* MouseSpeed */
#undef FC_ONOFF
    fc_build_path((uint32_t)(uintptr_t)pathbuf,
                  (uint32_t)GADDR(VA_g_dir_gdv + 0x50), (uint32_t)GADDR(VA_g_message_box_state + 0x38));          /* build ROTH.INI path */
    uint32_t h = dos_open_file((uint32_t)(uintptr_t)pathbuf, 1);         /* dos_open_file(write) (C2) */
    if (h != 0) {
        dos_write_items((uint32_t)(uintptr_t)sbuf, 1, (uint32_t)len, h); /* dos_write_items (C2) */
        dos_close_handle(h);                                            /* dos_close_handle (C2) */
    }
}

/* read_roth_ini (0x267f4, 369 B) — parse ROTH.INI (template @0x75fc3) into a stack struct, then load
 * the settings globals: the 5 volume words (low16 replace, add), the clamped view size (@0x7049a +
 * the derived @0x71988 bias), the video mode, the 4 ON/OFF flags (second char 'F' -> set -1), and
 * the @0x708e6 lookup. No args, void. In-game (DOS read). */
void read_roth_ini(void)
{
    uint8_t pathbuf[0xc8];
    uint8_t sbuf[0x800];
    fc_build_path((uint32_t)(uintptr_t)pathbuf,
                  (uint32_t)GADDR(VA_g_dir_gdv + 0x50), (uint32_t)GADDR(VA_g_message_box_state + 0x38));          /* build ROTH.INI path */
    uint32_t r = parse_config_keywords(pathbuf, (uint8_t *)(uintptr_t)GADDR(VA_g_heap_free_list + 0x9b3), sbuf, 0x800);
    if (r == 0) return;
    uint8_t *p = (uint8_t *)(uintptr_t)r;

    G16(VA_g_vol_soundfx) = 0; G32(VA_g_vol_soundfx) += R16((uintptr_t)(p + 0x16));   /* SoundFXVol */
    G16(VA_g_vol_speech) = 0; G32(VA_g_vol_speech) += R16((uintptr_t)(p + 0x18));   /* SpeechVol  */
    G16(VA_g_vol_movie) = 0; G32(VA_g_vol_movie) += R16((uintptr_t)(p + 0x1a));   /* MovieVol   */
    G16(VA_g_vol_music) = 0; G32(VA_g_vol_music) += R16((uintptr_t)(p + 0x1c));   /* MusicVol   */
    G16(VA_g_mouse_speed) = 0; G32(VA_g_mouse_speed) += R16((uintptr_t)(p + 0x1e));   /* MouseSpeed */
    G16(VA_g_choice_selected_index + 0x618) = 0;

    uint32_t vs = R16((uintptr_t)(p + 0x14));
    if (vs > 0x10) vs = 0x10;
    G8(VA_g_cfg_das2_arg + 0x1be) = (uint8_t)vs;
    G32(VA_g_choice_selected_index + 0x618) += (int32_t)((0x10 - vs) << 4);
    G32(VA_g_selected_video_mode) = *(int32_t *)(p + 0x10);                          /* VideoMode */

    G32(VA_g_active_weapon_ammo_cap + 0x1c) = 0; G32(VA_g_voice_decode_suspended) = 0; G32(VA_g_voice_decode_suspended + 0x4) = 0; G32(VA_g_voice_decode_suspended + 0x8) = 0;
    uint32_t s0 = *(uint32_t *)(p);
    if (s0 != 0 && R8(s0 + 1) == 0x46) G32(VA_g_active_weapon_ammo_cap + 0x1c) = -1;           /* 'F' -> flag on */
    uint32_t s1 = *(uint32_t *)(p + 4);
    if (s1 != 0 && R8(s1 + 1) == 0x46) G32(VA_g_voice_decode_suspended) = -1;
    uint32_t s2 = *(uint32_t *)(p + 8);
    if (s2 != 0 && R8(s2 + 1) == 0x46) G32(VA_g_voice_decode_suspended + 0x4) = -1;
    uint32_t s3 = *(uint32_t *)(p + 0xc);
    if (s3 != 0 && R8(s3 + 1) == 0x46) G32(VA_g_voice_decode_suspended + 0x8) = -1;

    uint32_t idx = R16((uintptr_t)(p + 0x1e)) >> 4;                 /* sar eax,4 (value is unsigned) */
    G32(VA_g_cursor_mask_data) = (int32_t)R8((uintptr_t)GADDR(VA_g_vol_music + 0x4) + idx);
    /* 0x26954 movzx eax, byte[...]; 0x2695b mov DWORD [0x708e6], eax — a 32-bit store that also
     * ZEROES 0x708e7..0x708e9 (the consumer 0x12ac6 reads the full dword as the cursor scale).
     * The old G8 byte store left the upper 3 bytes untouched — write-set infidelity. */
}

/* load_roth_res (0x10458, 240 B) — the master startup config: walk the response-file / cmdline
 * switch tokens (copy_switch_token_upper -> dispatch_arg_command) until the '@' response-file token,
 * then pool_find_free_chunk a scratch block and parse the "VERSION A HIRES F BLUR F VESA F ..."
 * template (@0x1049b) into it; apply HIRES / VESA flags, set the SFX/level/DAS asset dirs, load the
 * map list, copy the 0x80-byte version string, and free the block. No args, void. In-game (startup). */
void load_roth_res(void)
{
    uint32_t esi = (uint32_t)G32(VA_g_response_file_arg);                         /* g_response_file_arg */
    if (esi == 0 || R8(esi) == 0 || R8(esi + 1) == 0)
        esi = (uint32_t)GADDR(VA_g_default_response_file_msg);                            /* "@roth.res" default */

    for (;;) {
        if (R8(esi) == 0) return;
        /* copy_switch_token_upper: parse next token into g_arg_token_buf; ZF set -> done.
         * Direct-C. Disasm resolved the "multi-reg" thread as a PASSTHROUGH ARTIFACT:
         * dispatch_arg_command(0x107b3) consumes ONLY DX (`movzx ecx,dx`) + the token globals
         * @0x76718/0x76719 — it overwrites esi/ecx/eax immediately and never reads ebx, so the old
         * bridge's io.eax/ebx/ecx thread was inert (only io.edx==dl mattered). copy_switch_token_upper
         * genuinely COMPUTES: dl (return + g@0x76718), the token buffer (g@0x76719), and the advanced
         * source cursor (now the *end out-param); ZF is (dl==0). All three consumers are covered here. */
        const uint8_t *tok_end = NULL;
        uint32_t dl = copy_switch_token_upper((const uint8_t *)(uintptr_t)esi, &tok_end); /* 0x10920 */
        if (dl == 0) return;                                       /* or dl,dl; jz -> no more tokens */
        esi = (uint32_t)(uintptr_t)tok_end;                        /* advanced source cursor */
        G32(VA_g_map_list_ptr + 0x4) = (int32_t)esi;
        if (G8(VA_g_arg_token_buf + 0x1) == 0x40) break;                            /* '@' -> parse config */
        uint32_t cf = dispatch_arg_command(dl);            /* 0x107b3 (DX=dl) -> CF */
        esi = (uint32_t)G32(VA_g_map_list_ptr + 0x4);
        if (cf & 1) return;                                        /* not handled (jae loops; jb rets) */
    }

    /* '@' response-file token: parse the master config.
     * NB size (0x800) is the EDX arg (proto arg2), not ECX — orig 0x104d8 `mov edx,0x800` before
     * call 0x35cb4; the old bridge put it in the ecx slot (edx=0). */
    uint32_t chunk = pool_find_free_chunk((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_game_heap_handle), 0x800); /* 0x35cb4 */
    if (chunk == 0) return;
    uint32_t r = parse_config_keywords(
        (uint8_t *)(uintptr_t)GADDR(VA_g_arg_token_buf + 0x2), (uint8_t *)(uintptr_t)GADDR(VA_g_config_keyword_template),
        (uint8_t *)(uintptr_t)chunk, 0x800);
    if (r != 0) {
        uint8_t *ebx = (uint8_t *)(uintptr_t)r;
        if (*(int32_t *)(ebx + 4) != 0) cmd_set_hires();           /* 0x1089c cmd_set_hires */
        if (*(int32_t *)(ebx + 8) != 0) set_90bf8_ffff();               /* 0x108d4 */
        fc_set_cfg_asset(*(uint32_t *)(ebx + 0x10), (uint32_t)GADDR(VA_g_cfg_snd_arg), 0x584653); /* "SFX" */
        fc_set_cfg_asset(*(uint32_t *)(ebx + 0x1c), (uint32_t)GADDR(VA_g_dir_gdv), 0);
        fc_set_cfg_asset(*(uint32_t *)(ebx + 0x14), (uint32_t)GADDR(VA_g_cfg_das2_arg), 0x534144); /* "DAS" */
        load_map_list((const uint8_t *)(uintptr_t)*(uint32_t *)(ebx + 0x18)); /* 0x1059b (ESI) */
        uint32_t vsrc = *(uint32_t *)(ebx);
        if (vsrc != 0)
            memcpy((void *)(uintptr_t)GADDR(VA_g_version_string), (const void *)(uintptr_t)vsrc, 0x80); /* version string */
    }
    pool_free_chunk((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_game_heap_handle),
                           (uint8_t *)(uintptr_t)chunk);                        /* 0x35d80 pool_free_chunk */
}


/* ================================================================================================
 * Cluster E — the inspect-document modal (load + blocking popup loop).
 *
 * load_dbase300_resource_at_offset (0x196b9, 1518 B): loads a map-DAS resource at a byte offset
 * (open g_dbase300_filename @0x81f86 w/ CD-retry, seek offset+0x14, read the dims), then runs the
 * inspect-window MODAL LOOP (Space in the inventory): render the popup, decode the close-up GDV
 * image, and spin a blocking per-frame loop (image scroll + Esc/I/Space/Enter/Up/Dn/Lf/Rt/B keys +
 * choice nav/accept) until dismissed. NON-IDEMPOTENT + spins on the frame tick (0x90bcc) -> IN-GAME
 * LIVE-SWAP ONLY, needs interactive-lift mode. Every callee is bridged; loop-control ECX/EBX/EDX
 * outputs are threaded per the corpus. THE in-game-validation target for this subsystem — its modal
 * register-threading is unverifiable by the oracle and must be confirmed in the debug session.
 * ================================================================================================ */

/* full-context bridge: preset registers via *io, run, read outputs back from *io. */
static void fc_call(uint32_t canon_va, regs_t *io) {
    io->va = canon_va + OBJ_DELTA;
#ifndef ROTH_STANDALONE
    call_orig(io);
#else
    switch (canon_va) {   /* routes. Every caller consumes io->eax only (the 0x18ada caller's
                           * io->edx/ebx feed the flip bridge, which is host_flip_video_page(eax)
                           * in this lane — edx/ebx unread), so EAX threading is sufficient. */
    case 0x18e9eu: io->eax = render_inspect_popup_window(io->eax, io->edx, io->ebx, io->ecx); return;
    case 0x4b710u: io->eax = gdv_decoder_open(io->eax); return;
    case 0x4b8c1u: io->eax = gdv_decode_frame(io->eax); return;
    case 0x4b95eu: gdv_decoder_close(); return;
    case 0x1951du: io->eax = (uint32_t)load_inspect_document_page(io->eax, io->edx, io->ebx, io->ecx); return;
    case 0x18adau: io->eax = update_inspect_popup_choices(); return;
    default: break;
    }
    roth_unreachable(canon_va);   /* dbase300/GDV inspect-doc bridge — in-game inspect/cutscene tier */
#endif
}

/* descriptor byte offsets inside the local page/scroll struct (base = [ebp+0x32] in the original) */
#define D_LO_X   0x04   /* 810ec */
#define D_LO_Y   0x08   /* 810ee */
#define D_HI_X   0x14   /* 810f0 */
#define D_HI_Y   0x18   /* 810f2 */
#define D_PITCH  0x1c   /* g_screen_pitch */
#define D_SCRL_X 0x20   /* local_3c */
#define D_SCRL_Y 0x24   /* local_38 */
#define D_REDRAW 0x28   /* local_34 */
#define D_PREV_X 0x48   /* local_14 */
#define D_PREV_Y 0x4c   /* local_10 */
#define DD32(o)  (*(int32_t *)(desc + (o)))

uint32_t load_dbase300_resource_at_offset(uint32_t offset, uint32_t param_2,
                                                 uint32_t record, uint32_t param_4)
{
    regs_t io;
    uint8_t gdvctx[0x78];                       /* the GDV decode context/descriptor (local_d4) */
    uint8_t desc[0x50];                          /* the page/scroll descriptor (local_5c..) */
    uint32_t handle, cache = 0, window = 0, page = 0;
    int gdv_open = 0, refcounted = 0;

    G32(VA_g_inspect_popup_active) = 1;                            /* g_inspect_popup_active = 1 */
    G32(VA_g_inspect_popup_state + 0x34) = 0;
    G32(VA_g_inspect_popup_state + 0x10) = 0;

    for (;;) {                                   /* open with CD-swap retry prompt */
        handle = dos_open_file((uint32_t)GADDR(VA_g_dbase300_filename), 0);       /* dos_open_file (C2) */
        if (handle != 0 || G32(VA_g_inspect_info_available + 0x14) != 0) break;
        uint32_t choice = show_resource_error_box();               /* 0x2632a -> button EAX */
        if (choice == 2) G32(VA_g_inspect_info_available + 0x14)++;
        if (choice != 1) break;
    }
    if (handle == 0) goto done;                  /* open failed */

    memset(gdvctx, 0, sizeof gdvctx);            /* zero_memory(local_d4, 0x78) */
    ensure_das_cache_heap_space(0x50000);                         /* 0x414d2 (need=EAX) */
    cache = pool_alloc_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle), 0x50000); /* 0x360f9 */
    if (cache == 0) goto teardown_close;

    *(uint32_t *)(gdvctx + 0x0c) = 0x50000;
    *(uint32_t *)(gdvctx + 0x10) = (uint32_t)G32(VA_g_sos_digital_device);               /* g_sos_digital_device */
    *(uint32_t *)(gdvctx + 0x18) = (G8(VA_g_player_movement_enabled + 0x10) == 0) ? 0x1890u : 0x1090u;
    if (param_2 == 0) *(uint32_t *)(gdvctx + 0x18) |= 8u;
    *(uint32_t *)(gdvctx + 0x1c) = (uint32_t)GADDR(0x18e09);             /* frame callback */
    *(uint16_t *)(gdvctx + 0x26) = 0x8000;
    *(uint16_t *)(gdvctx + 0x2a) = 0x140;
    *(uint16_t *)(gdvctx + 0x2c) = 0xc8;
    G32(VA_g_inspect_popup_state) = 0;                            /* g_inspect_popup_state = 0 */
    G32(VA_g_inspect_info_available) = 0;                            /* g_inspect_info_available = 0 */
    /* gdvctx+0x18 bit 0x1000 = "read the GDV from an EXISTING handle" -> [+0x58]=handle, [+0x5c]=seek
     * (0x197b9/0x197c2: [ebp+0x12]/[ebp+0x16] = gdvctx+0x58/0x5c). Without these gdv_read_file_header
     * reads from handle 0 -> err 0x21. */
    *(uint32_t *)(gdvctx + 0x58) = handle;
    *(uint32_t *)(gdvctx + 0x5c) = offset;
    G32(VA_g_inventory_panel_open + 0x4) = (int32_t)record;

    dos_lseek(handle, offset + 0x14, 0);                        /* dos_lseek(handle, off+0x14, whence 0=SET) (C2) */
    uint16_t dims[2] = {0, 0};
    dos_read_items((uint32_t)(uintptr_t)dims, 1, 4, handle);     /* dos_read_items(&dims,1,4,h) (C2) */

    memset(&io, 0, sizeof io);                                          /* render_inspect_popup_window */
    io.eax = dims[0]; io.edx = dims[1]; io.ebx = record; io.ecx = param_4;
    fc_call(0x18e9e, &io);
    window = io.eax;                             /* 0x197fc: mov edx,eax -> the GDV gate tests THIS */

    refcounted = 1;
    obj_counter12_inc((uint32_t)G32(VA_g_das_cache_heap_handle));                  /* 0x361e7 obj_counter12_inc */
    gdv_open = 1;
    *(uint32_t *)(gdvctx + 0x08) = *(uint32_t *)(uintptr_t)cache;      /* 0x1981b: gdvctx+8 = *cache (decode buf) */

    if (window != 0) {                           /* 0x1981e: test edx,edx (edx = window ptr) */
        memset(&io, 0, sizeof io); io.eax = (uint32_t)(uintptr_t)gdvctx;
        fc_call(0x4b710, &io);                                         /* gdv_decoder_open */
        if (io.eax == 0) {
            if ((R8(record + 7) & 0x80) == 0) {                        /* first view -> run OnInspect */
                R8(record + 7) = (uint8_t)(R8(record + 7) - 0x80);
                uint32_t rec4 = scan_tag4_chunk(record);        /* 0x1dda8 scan_tag4_chunk */
                /* re-point 0x1db89: EAX=rec4, EDX=0; return + EBX/ECX discarded — not skip-list. */
                if (rec4 != 0) eval_dialogue_record_condition_with_cleanup(rec4, 0, NULL, NULL);
            }
            G32(VA_g_inspect_popup_state + 0x34) = 0;
            /* 0x1985c: RE-SET gdvctx+8 = *cache — gdv_decoder_open's alloc_decode_buffer overwrote it;
             * gdv_decode_frame reads [esi+8] as the decode target + computes a pointer delta from it,
             * so a stale value corrupts its pointer table and faults. */
            *(uint32_t *)(gdvctx + 0x08) = *(uint32_t *)(uintptr_t)cache;
            memset(&io, 0, sizeof io); io.eax = (uint32_t)(uintptr_t)gdvctx;
            fc_call(0x4b8c1, &io);                                     /* gdv_decode_frame */
            uint32_t decode_ebx = io.eax;         /* 0x19869: mov ebx,eax (decode result in EAX) */
            int want_loop = (int)param_2;
            if (G32(VA_g_inspect_popup_state) != 0) { want_loop = 1; decode_ebx = 0; }  /* popup_state: loop=1, xor ebx,eax=0 */

            gdv_open = 0;
            memset(&io, 0, sizeof io); io.eax = (uint32_t)(uintptr_t)gdvctx;
            fc_call(0x4b95e, &io);                                     /* gdv_decoder_close */
            blit_saved_ui_block();                              /* 0x18a64 */
            pool_free_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle),
                                    (uint32_t *)(uintptr_t)cache);     /* 0x360b3 pool_free_handle */
            cache = 0;
            obj_counter12_dec((uint32_t)G32(VA_g_das_cache_heap_handle));          /* 0x361ef obj_counter12_dec */
            refcounted = 0;

            if (decode_ebx != 0x100 && want_loop != 0) {
                /* ---- the blocking inspect modal loop ---- */
                memset(desc, 0, sizeof desc);
                DD32(D_HI_X) = (int16_t)R16(GADDR(VA_g_inspect_popup_state + 0x30));
                DD32(D_HI_Y) = (int16_t)R16(GADDR(VA_g_inspect_popup_state + 0x32));
                DD32(D_PITCH) = (int32_t)G32(VA_g_screen_pitch);
                DD32(D_LO_X) = (int16_t)R16(GADDR(VA_g_inspect_popup_state + 0x2c));
                DD32(D_LO_Y) = (int16_t)R16(GADDR(VA_g_inspect_popup_state + 0x2e));
                page = 0;
                if (G32(VA_g_inspect_page_count) != 0) {                               /* g_inspect_page_count */
                    memset(&io, 0, sizeof io);
                    io.eax = 0; io.edx = handle; io.ebx = (uint32_t)(uintptr_t)desc;
                    fc_call(0x1951d, &io);                             /* load_inspect_document_page */
                    page = io.eax;
                }
                G32(VA_g_inspect_popup_state) = 3;                                      /* g_inspect_popup_state = 3 */
                DD32(D_PREV_X) = -1; DD32(D_SCRL_X) = 0; DD32(D_SCRL_Y) = 0; DD32(D_REDRAW) = 4;
                int32_t exit_acc = 0;
                G16(VA_g_last_frame_tick) = G16(VA_g_frame_tick_counter);                          /* g_last_frame_tick = tick */
                do {
                    /* spin until the frame tick advances (interactive-lift surrogate) */
                    do { G32(VA_g_frame_time_scale) = (int32_t)G16(VA_g_frame_tick_counter) - (int16_t)G16(VA_g_last_frame_tick); }
                    while (G32(VA_g_frame_time_scale) == 0);
                    G16(VA_g_last_frame_tick) = G16(VA_g_frame_tick_counter);

                    if (page != 0 && G32(VA_g_inspect_page_count + 0x84) != 0) {             /* page turn pending */
                        DD32(D_PREV_X) = -1;
                        memset(&io, 0, sizeof io);
                        io.eax = page; io.edx = handle; io.ebx = (uint32_t)(uintptr_t)desc;
                        fc_call(0x1951d, &io);                        /* load_inspect_document_page */
                        page = io.eax;
                    }
                    if (page != 0) {                                  /* redraw the panel if scrolled */
                        int changed = (DD32(D_PREV_X) != DD32(D_SCRL_X))
                                    + (DD32(D_PREV_Y) != DD32(D_SCRL_Y))
                                    + (DD32(D_REDRAW) != 0 ? 1 : 0);
                        if (changed) {
                            DD32(D_PREV_X) = DD32(D_SCRL_X);
                            DD32(D_PREV_Y) = DD32(D_SCRL_Y);
                            draw_das_panel_slide_reveal((uint32_t)(uintptr_t)desc); /* 0x187d1 */
                        }
                    }
                    /* mouse info-button drag -> image scroll */
                    if (G32(VA_g_inspect_info_available) == 1) {
                        G32(VA_g_inspect_info_available + 0xc) = DD32(D_SCRL_X);
                        G32(VA_g_inspect_info_available + 0x10) = DD32(D_SCRL_Y);
                        G32(VA_g_inspect_info_available) = 2;
                    }
                    if (G32(VA_g_inspect_info_available) != 0) {
                        /* IN-GAME-VALIDATE: the mouse-drag -> scroll clamp. Confirmed from disasm:
                         * g_mouse_buttons_prev @0x7e929 (test &3), g_mouse_x @0x707b3 / g_mouse_y
                         * @0x707b7, drag anchors @0x8118c/0x81190. The range = page-dim (desc+0x0c) -
                         * view offset (desc+0x14/0x18); exact desc field offsets need the debug run. */
                        if ((G8(VA_g_mouse_buttons_prev) & 3) == 0) {
                            G32(VA_g_inspect_info_available) = 0;
                        } else {
                            int32_t range_x = DD32(0x0c) - DD32(D_HI_X);
                            int32_t dy = (int32_t)G32(VA_g_mouse_y) - (int32_t)G32(VA_g_inspect_info_available + 0x8);
                            if (range_x > 0) {
                                int32_t v = (int32_t)G32(VA_g_inspect_info_available + 0xc) - ((int32_t)G32(VA_g_mouse_x) - (int32_t)G32(VA_g_inspect_info_available + 0x4));
                                if (v < 0) v = 0;
                                DD32(D_SCRL_X) = v;
                                if (range_x < v) DD32(D_SCRL_X) = range_x;
                            }
                            int32_t range_y = DD32(0x10) - DD32(D_HI_Y);
                            if (range_y > 0) {
                                int32_t v = (int32_t)G32(VA_g_inspect_info_available + 0x10) - dy;
                                if (v < 0) v = 0;
                                DD32(D_SCRL_Y) = v;
                                if (range_y < v) DD32(D_SCRL_Y) = range_y;
                            }
                        }
                    }
                    memset(&io, 0, sizeof io);                        /* update_inspect_popup_choices */
                    fc_call(0x18ada, &io);
                    if (io.eax != 0) fc_bridge(0x2e1e8, 3, io.edx, io.ebx, 0);  /* flip_video_page */

                    /* drain the input ring, dispatching the popup keymap */
                    int32_t drain;
                    do {
                        uint8_t key = input_ring_dequeue();              /* 0x1299a */
                        drain = 0;
                        if (key == 1 || key == 0x17 || key == 0x39) {          /* Esc / I / Space */
                            uint32_t r = try_interrupt_dialogue_voice(); /* 0x18a2a (ignores key) */
                            if (r == 0) exit_acc++;
                        } else if (key == 0x1c) {                              /* Enter */
                            if (G32(VA_g_choice_selected_index) == -1 ||
                                (G32(VA_g_move_freeze_gate) != 0x6ffff && G32(VA_g_choice_interaction_mode) != 1)) {
                                uint32_t r = try_interrupt_dialogue_voice(); /* 0x18a2a (ignores key) */
                                if (r == 0) exit_acc++;
                            } else {
                                choice_accept_selected();              /* 0x1fbba */
                            }
                        } else if (key == 0x30) {                             /* 'B' screenshot */
                            check_snapshot_key();                       /* 0x11124 (ignores key) */
                        } else if (key == 0x48) {                             /* Up */
                            if (G32(VA_g_choice_interaction_mode) == 0) {
                                if (G32(VA_g_inspect_popup_state) != 0) {
                                    if (DD32(D_SCRL_Y) > 0) DD32(D_SCRL_Y) -= 8;
                                    if (DD32(D_SCRL_Y) < 0) DD32(D_SCRL_Y) = 0;
                                    drain = 1;
                                }
                            } else choice_select_prev();               /* 0x1fb1e */
                        } else if (key == 0x4b) {                             /* Left */
                            if (G32(VA_g_inspect_popup_state) != 0) {
                                if (DD32(D_SCRL_X) > 0) DD32(D_SCRL_X) -= 8;
                                if (DD32(D_SCRL_X) < 0) DD32(D_SCRL_X) = 0;
                                drain = 1;
                            }
                        } else if (key == 0x4d) {                             /* Right */
                            if (G32(VA_g_inspect_popup_state) != 0) {
                                int32_t range = DD32(0x0c) - DD32(D_HI_X);     /* 0x19b97: [ebp+0x3e]-[ebp+0x46] */
                                if (range > 0) {
                                    if (DD32(D_SCRL_X) < range) DD32(D_SCRL_X) += 8;
                                    if (range < DD32(D_SCRL_X)) DD32(D_SCRL_X) = range;
                                }
                                drain = 1;
                            }
                        } else if (key == 0x50) {                             /* Down */
                            if (G32(VA_g_choice_interaction_mode) == 0) {
                                if (G32(VA_g_inspect_popup_state) != 0) {
                                    int32_t range = DD32(0x10) - DD32(D_HI_Y); /* 0x19b4b: [ebp+0x42]-[ebp+0x4a] */
                                    if (range > 0) {
                                        if (DD32(D_SCRL_Y) < range) DD32(D_SCRL_Y) += 8;
                                        if (range < DD32(D_SCRL_Y)) DD32(D_SCRL_Y) = range;
                                    }
                                    drain = 1;
                                }
                            } else choice_select_next();               /* 0x1fc16 */
                        }
                    } while (drain != 0);
                    if (G32(VA_g_inventory_panel_open + 0x8) != 0) exit_acc++;
                    if (G32(VA_g_inventory_panel_open) == 0) exit_acc++;
                } while (exit_acc == 0);

                if (page != 0) pool_free_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle),
                                                       (uint32_t *)(uintptr_t)page); /* 0x360b3 */
            }
        }
    }

    if (gdv_open != 0) { memset(&io, 0, sizeof io); io.eax = (uint32_t)(uintptr_t)gdvctx; fc_call(0x4b95e, &io); }
    free_inspect_popup_and_redraw(window, 0);                   /* 0x19678 (EAX=window, EDX=0) */

teardown_close:
    if (refcounted != 0) obj_counter12_dec((uint32_t)G32(VA_g_das_cache_heap_handle));      /* 0x361ef */
    if (cache != 0) pool_free_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle),
                                            (uint32_t *)(uintptr_t)cache);      /* 0x360b3 */
    dos_close_handle(handle);                                 /* dos_close_handle (C2) */
    G32(VA_g_inventory_panel_open + 0x4) = 0;
    G32(VA_g_choice_interaction_mode) = 0;                            /* g_choice_interaction_mode = 0 */
    G32(VA_g_inventory_ui_action + 0x4) = 0;
done:
    G32(VA_g_inspect_popup_active) = 0;                            /* g_inspect_popup_active = 0 */
    return 0;
}
