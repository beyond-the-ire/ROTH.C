/* lift_dpmi_dos_os.c — the DOS/DPMI OS-wrapper subsystem lifted to verified C.
 * Per docs/operating/recomp.md §4.6: every subsystem gets its own TU.
 *
 * dpmi_dos_os = the game's thin wrappers around the raw DOS/DPMI OS primitives (file open/read/
 * write/close/seek; the DAS-block LDT-selector helpers). These are C2 *deliverables* — they close
 * the OS-call boundary so the engine's file/selector callers go call-closed — and are NOT rows in
 * the 1171 engine gate (they sit in the CRT/OS klass, like dos_runtime cluster C's block allocators).
 *
 * The OS touch goes through the C2 call-API (os_api.h): the HOST binding reuses the trap-side DOS
 * services as calls; the ORACLE binding is the canned double whose SIGTRAP servicer also feeds the
 * patched ORIGINAL — so these lifts are verified by a TRUE differential over identical canned DOS
 * (test_c2_fileio.c).
 *
 * ABI throughout is derived from the DISASM (tools/roth_disasm.py func <va>), not the corpus
 * pseudocode: these are Watcom hand-asm register wrappers (EAX/EDX/EBX/ECX), NOT the naive EAX=fd.
 */
#include "common.h"
#include "os_api.h"

/* ---- dos_open_file (0x41ae5): mode-dispatched open/create ------------------------------------
 * EAX = path (flat), EDX = mode {0 = read, 1 = create, 2 = read/write + seek-END}. Returns EAX =
 * DOS handle, or an error/status code (0, or 2 for the `CON:` device). Disasm-faithful control flow:
 *   path==0                     -> 0                                   (or eax,eax; je)
 *   mode==0                     -> int21 3d00 open; handle or 0 on CF  (no CON: check on this leg)
 *   dword[path]=='CON:' (mode!=0) -> 2                                 (cmp [eax],0x3a4e4f43; je)
 *   mode==2                     -> int21 3d02 open; on success nested dos_lseek(h,0,END) then
 *                                  return h REGARDLESS of the seek; on OPEN-fail fall through ↓
 *   mode==1  (or the mode-2 open-fail fall-through) -> int21 41 delete, then 3c create (attr CX=0);
 *                                  handle or 0 on CF
 *   any other mode              -> 0                                   (sub eax,eax; ret) */
uint32_t dos_open_file(uint32_t path, uint32_t mode)
{
    if (path == 0)
        return 0;
    if (mode == 0) {                                         /* 3d00 — note: NO CON: check here */
        uint32_t h;
        return os_dos_open(path, 0, &h) ? 0 : h;
    }
    if (*(uint32_t *)(uintptr_t)path == 0x3a4e4f43u)         /* 'CON:' device short-circuit */
        return 2;
    if (mode == 2) {                                         /* 3d02 + seek to END */
        uint32_t h;
        if (!os_dos_open(path, 2, &h)) {
            os_dos_lseek(h, 0, 2, 0);                        /* nested dos_lseek(h, 0, whence=END) */
            return h;                                        /* returned regardless of the seek */
        }
        /* open-fail falls through to the shared delete+create (orig jb 0x41b2f) */
    } else if (mode != 1) {
        return 0;                                            /* invalid mode */
    }
    os_dos_delete(path);                                     /* int21 41 (its CF is ignored) */
    uint32_t h;
    return os_dos_create(path, 0, &h) ? 0 : h;              /* int21 3c, attributes CX=0 */
}

/* ---- dos_close_handle (0x41b41): close a DOS handle ------------------------------------------
 * EAX = handle. Skips stdin/stdout/stderr (fd <= 2: `cmp eax,2; jbe`). The wrappers ignore the
 * close result (every caller discards its CF), so the lift is void. */
void dos_close_handle(uint32_t handle)
{
    if (handle > 2)
        os_dos_close(handle);
}

/* ---- dos_read_items (0x41b53): fread-style block read ----------------------------------------
 * EAX = buf (flat), EDX = itemsize, EBX = nitems, ECX = handle. Returns EAX = items read =
 * bytesRead / itemsize (unsigned div). Guards BOTH nitems==0 and itemsize==0 (the two `or/je` at
 * entry -> tail `sub eax,eax; ret`). On the int21 3f CF path the byte count is zeroed before the
 * divide (`jae` skips; else `sub eax,eax`). */
uint32_t dos_read_items(uint32_t buf, uint32_t isz, uint32_t n, uint32_t handle)
{
    if (n == 0 || isz == 0)
        return 0;
    uint32_t got;
    if (os_dos_read(handle, buf, n * isz, &got))
        got = 0;
    return got / isz;                                        /* sub edx,edx; div ebp (unsigned) */
}

/* ---- dos_write_items (0x41b7a): fwrite-style block write -------------------------------------
 * EAX = buf (flat), EDX = itemsize, EBX = nitems, ECX = handle. Returns EAX = items written =
 * bytesWritten / itemsize. The ONLY entry guard is `or ecx,ecx; je` — ECX is the HANDLE (it lands
 * in BX for int21 AH=40 via the `xchg ebx,ecx`), so a 0 HANDLE returns 0 (§8.1). nitems==0 is NOT
 * guarded: it still issues int21 40 with CX=0, which DOS treats as "truncate the file at the
 * current position" — the lift calls os_dos_write(handle, buf, 0, ...) faithfully. itemsize==0
 * would #DE at `div ebp` in the original too (unreachable — callers never pass it); no guard added. */
uint32_t dos_write_items(uint32_t buf, uint32_t isz, uint32_t n, uint32_t handle)
{
    if (handle == 0)
        return 0;
    uint32_t put;
    if (os_dos_write(handle, buf, n * isz, &put))
        put = 0;
    return put / isz;
}

/* ---- dos_lseek (0x41b9a): seek a DOS handle --------------------------------------------------
 * EAX = handle, EDX = offset (32-bit), BL = whence {0=SET,1=CUR,2=END}. A 0 HANDLE returns 0
 * (`or eax,eax; je` — gotcha H1: a wrong eax=0 silently no-ops the seek). Otherwise returns EAX =
 * whatever int21 AH=42 leaves: the recomposed DX:AX new position on success, OR the raw DOS error
 * code on CF. NOTE (disasm, vs the design's "CF->0"): the CF branch `jb 0x41bbe` skips PAST the
 * `sub eax,eax` at 0x41bbc — that zero tail is DEAD CODE, so the error EAX passes straight through.
 * os_dos_lseek surfaces that raw register via *pos in both cases, so the lift just returns it. */
uint32_t dos_lseek(uint32_t handle, uint32_t off, uint32_t whence)
{
    if (handle == 0)
        return 0;
    uint32_t pos = 0;
    os_dos_lseek(handle, (int32_t)off, (uint8_t)whence, &pos);
    return pos;
}

/* ============================ the DAS-cache block selector pair =============================== *
 * These give a DAS-cache block record its own LDT descriptor mapping the block's memory. The record
 * fields are plain host memory (the caller passes a flat record pointer via ESI, like path/buf in
 * the file wrappers) — read/write them directly, NO GADDR. All four fields used are 16-bit:
 *   [rec+0x8]  = the allocated selector (written by setup, read by refresh)
 *   [rec+0xa]  = flags; bit 0x100 selects the "sub-block base adjust" mode
 *   [rec+0x14] = the 16-bit byte offset added to the record base when 0x100 is set (movzx: unsigned)
 *   [rec+0x16] = refresh's skip sentinel (int16 == -2 -> leave the base as-is)  */
static inline uint16_t das_rec16(uint32_t rec, uint32_t off) { return *(uint16_t *)(uintptr_t)(rec + off); }

/* ---- setup_das_block_selector (0x41191): alloc + program the block's LDT descriptor -----------
 * ESI = record, EDI = end-ptr (one past the block's last mapped byte). Returns CF (1 = failure).
 * Disasm-faithful (§8.2): alloc one descriptor (0000); on CF return 1 with the record UNTOUCHED
 * (0x411de: `stc; ret`). Store the selector to [rec+8] (16-bit) BEFORE the base/limit ops. Compute
 * the base record (+= [rec+0x14] when flag 0x100 is set), set base = base_rec+0x10 (0007) and limit
 * = endp-0x10 (0008). If EITHER leg fails (`jb 0x411d8`, short-circuit — set-base fail skips the
 * set-limit), free the descriptor (0x411d8: `mov ax,1; int 0x31` with BX = the still-loaded sel)
 * and return 1; the stored [rec+8] KEEPS the (now-freed) selector value — the original never clears
 * it. Success -> return 0 (0x411d7: CF clear). */
int setup_das_block_selector(uint32_t rec, uint32_t endp)
{
    uint16_t sel;
    if (os_dpmi_alloc_descriptors(1, &sel))                    /* 0x411de: stc; ret (record untouched) */
        return 1;
    *(uint16_t *)(uintptr_t)(rec + 8) = sel;                   /* [rec+8] = sel (BEFORE base/limit) */
    uint32_t base_rec = rec;
    if (das_rec16(rec, 0xa) & 0x100)                           /* test word [rec+0xa], 0x100 */
        base_rec += das_rec16(rec, 0x14);                      /* movzx [rec+0x14]; add (16-bit, unsigned) */
    if (os_dpmi_set_segment_base(sel, base_rec + 0x10) ||      /* 0007 (jb 0x411d8 on CF) */
        os_dpmi_set_segment_limit(sel, endp - 0x10)) {         /* 0008 (jb 0x411d8 on CF) */
        os_dpmi_free_descriptor(sel);                          /* 0x411d8: AX=0001 free, BX=sel */
        return 1;                                              /* then stc; ret */
    }
    return 0;
}

/* ---- refresh_das_block_selector_base (0x412ed): re-program the block's descriptor base ---------
 * ESI = record. Void: the observable effect is the 0007 set-base (plus its args). BX = [rec+8] (the
 * selector). When flag 0x100 is set, first test the skip sentinel: [rec+0x16] == -2 (int16) short-
 * circuits to `ret` WITHOUT issuing the int31 (0x412fe: `je 0x41316`); otherwise add [rec+0x14] to
 * the base record. Set base = base_rec+0x10 (0007 at 0x41314).
 * NOTE (disasm): the original leaks 0007's CF to its caller at `ret`, but no consumer reads it
 * (da_bridge_esi ignores flags; the das_assets test stub is a bare `ret`), so a void lift is
 * faithful-in-practice. */
void refresh_das_block_selector_base(uint32_t rec)
{
    uint16_t sel = das_rec16(rec, 8);                          /* mov bx, [rec+8] */
    uint32_t base_rec = rec;
    if (das_rec16(rec, 0xa) & 0x100) {                         /* test word [rec+0xa], 0x100 */
        if ((int16_t)das_rec16(rec, 0x16) == -2)               /* cmp word [rec+0x16], -2; je -> ret */
            return;                                            /* skip: NO int31 issued */
        base_rec += das_rec16(rec, 0x14);                      /* movzx [rec+0x14]; add */
    }
    os_dpmi_set_segment_base(sel, base_rec + 0x10);            /* 0007 (CF leaked at ret; no consumer) */
}

/* ---- dpmi_free_dos_memory (0x40a50): free a DOS-memory block by selector ----------------------
 * AX = selector. A 0 selector is skipped (`or ax,ax; je`); otherwise int31 0101 with DX = sel
 * (the os_dpmi_free_dos primitive). EDX is push/pop-preserved in the original — a non-issue
 * in C (it is neither an argument nor a result here). No lifted call sites yet (a workstream-D
 * re-point target); landed as a trivial call-closure over the existing primitive. */
void dpmi_free_dos_memory(uint32_t sel)
{
    if ((uint16_t)sel)                                         /* or ax,ax; je 0x40a60 (skip 0) */
        os_dpmi_free_dos(sel & 0xffffu);                       /* int31 0101, DX = sel */
}
