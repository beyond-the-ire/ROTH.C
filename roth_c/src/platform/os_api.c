/* os_api.c — the HOST binding of the C2 call-API (see os_api.h).
 *
 * Reuses the trap-host's DPMI services as CALLS: each primitive builds a synthetic cpu_t
 * (a local ucontext register frame), invokes dpmi_int31() — the exact code the trap handler
 * runs — and reads the results back. Zero behavior drift from the trapped path, no INT
 * instruction, no signal. 0500 is mirrored inline (its handler goes through dpmi_linear on
 * ES:EDI, which is trap-context-specific; the service itself is three lines).
 *
 * Built into roth-host only. The oracle links c2_mock.c instead (same contract, canned).
 */
#include <string.h>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ucontext.h>
#include "os_api.h"
#include "roth_host.h"

/* run dpmi_int31 on a synthetic register frame; returns the frame for result reads */
typedef struct { ucontext_t uc; cpu_t c; } os_frame_t;

static void os_frame_init(os_frame_t *f)
{
    memset(&f->uc, 0, sizeof f->uc);
    f->c.uc = &f->uc;
}

void os_dpmi_get_free_mem_info(uint8_t *buf)
{
    /* mirror of dpmi.c case 0x0500: 0xff fill + largest-free dword (64 MB) */
    memset(buf, 0xff, 0x30);
    *(uint32_t *)buf = 64u << 20;
}

int os_dpmi_alloc_linear(uint32_t bytes, uint32_t *base, uint32_t *handle)
{
    os_frame_t f; os_frame_init(&f);
    R_EAX(&f.c) = 0x0501;
    R_EBX(&f.c) = bytes >> 16;
    R_ECX(&f.c) = bytes & 0xffffu;
    dpmi_int31(&f.c);
    if (R_EFL(&f.c) & 1u)                     /* CF */
        return 1;
    *base   = ((R_EBX(&f.c) & 0xffffu) << 16) | (R_ECX(&f.c) & 0xffffu);
    *handle = ((R_ESI(&f.c) & 0xffffu) << 16) | (R_EDI(&f.c) & 0xffffu);
    return 0;
}

void os_dpmi_free_linear(uint32_t handle)
{
    os_frame_t f; os_frame_init(&f);
    R_EAX(&f.c) = 0x0502;
    R_ESI(&f.c) = handle >> 16;
    R_EDI(&f.c) = handle & 0xffffu;
    dpmi_int31(&f.c);
}

int os_dpmi_alloc_dos(uint32_t paragraphs, uint32_t *seg, uint32_t *sel)
{
    os_frame_t f; os_frame_init(&f);
    R_EAX(&f.c) = 0x0100;
    R_EBX(&f.c) = paragraphs & 0xffffu;
    dpmi_int31(&f.c);
    if (R_EFL(&f.c) & 1u)                     /* CF: leave seg/sel outputs untouched (orig contract) */
        return 1;
    *seg = R_EAX(&f.c) & 0xffffu;
    *sel = R_EDX(&f.c) & 0xffffu;
    return 0;
}

void os_dpmi_free_dos(uint32_t sel)
{
    os_frame_t f; os_frame_init(&f);
    R_EAX(&f.c) = 0x0101;
    R_EDX(&f.c) = sel & 0xffffu;
    dpmi_int31(&f.c);
}

/* ---- exception / PM-vector / DOS-print / low-memory (see os_api.h) ---- */

void os_dpmi_get_exc_handler(uint32_t vec, uint16_t *cs, uint32_t *off)
{
    os_frame_t f; os_frame_init(&f);
    R_EAX(&f.c) = 0x0202;
    R_EBX(&f.c) = vec & 0xffu;
    dpmi_int31(&f.c);
    *cs  = (uint16_t)(R_ECX(&f.c) & 0xffffu);
    *off = R_EDX(&f.c);
}

int os_dpmi_set_exc_handler(uint32_t vec, uint16_t cs, uint32_t off)
{
    os_frame_t f; os_frame_init(&f);
    R_EAX(&f.c) = 0x0203;
    R_EBX(&f.c) = vec & 0xffu;
    R_ECX(&f.c) = cs;
    R_EDX(&f.c) = off;
    dpmi_int31(&f.c);
    return (int)(R_EFL(&f.c) & 1u);              /* CF */
}

void os_dpmi_get_pm_vector(uint32_t vec, uint16_t *cs, uint32_t *off)
{
    os_frame_t f; os_frame_init(&f);
    R_EAX(&f.c) = 0x0204;
    R_EBX(&f.c) = vec & 0xffu;
    dpmi_int31(&f.c);
    *cs  = (uint16_t)(R_ECX(&f.c) & 0xffffu);
    *off = R_EDX(&f.c);
}

void os_dpmi_set_pm_vector(uint32_t vec, uint16_t cs, uint32_t off)
{
    os_frame_t f; os_frame_init(&f);
    R_EAX(&f.c) = 0x0205;
    R_EBX(&f.c) = vec & 0xffu;
    R_ECX(&f.c) = cs;
    R_EDX(&f.c) = off;
    dpmi_int31(&f.c);
}

void os_dos_print_string(uint32_t edx)
{
    os_frame_t f; os_frame_init(&f);
    R_EAX(&f.c) = 0x0900;
    R_EDX(&f.c) = edx;
    dos_int21(&f.c);
}

uint32_t os_lowmem_read32(uint32_t ea)          { return host_lowmem_load(ea, 4); }
void     os_lowmem_write32(uint32_t ea, uint32_t v) { host_lowmem_store(ea, v, 4); }
void     os_lowmem_write8(uint32_t ea, uint8_t v)   { host_lowmem_store(ea, v, 1); }

/* ---- the DOS file service (int21 3c/3d/3e/3f/40/41/42, see os_api.h) ----
 * Register frames mirror what the engine's own file wrappers set before their `int 0x21`, so
 * dos_int21() (dos.c) — the exact code the trap runs — services them identically. dos.c reports
 * CF via set_cf() (EFL bit 0) and writes results as full-width R_Exx assignments (the result
 * registers land in the low 16 bits: DOS handles/positions fit, high bits are zeroed). */

int os_dos_open(uint32_t path, uint8_t access, uint32_t *handle)
{
    os_frame_t f; os_frame_init(&f);
    R_EAX(&f.c) = 0x3d00u | access;          /* AH=3d, AL=access */
    R_EDX(&f.c) = path;
    dos_int21(&f.c);
    if (R_EFL(&f.c) & 1u)                     /* CF: leave *handle untouched (orig contract) */
        return 1;
    *handle = R_EAX(&f.c) & 0xffffu;
    return 0;
}

int os_dos_create(uint32_t path, uint16_t attr, uint32_t *handle)
{
    os_frame_t f; os_frame_init(&f);
    R_EAX(&f.c) = 0x3c00u;                    /* AH=3c */
    R_ECX(&f.c) = attr;
    R_EDX(&f.c) = path;
    dos_int21(&f.c);
    if (R_EFL(&f.c) & 1u)                     /* CF: *handle untouched */
        return 1;
    *handle = R_EAX(&f.c) & 0xffffu;
    return 0;
}

int os_dos_delete(uint32_t path)
{
    os_frame_t f; os_frame_init(&f);
    R_EAX(&f.c) = 0x4100u;                    /* AH=41 */
    R_EDX(&f.c) = path;
    dos_int21(&f.c);
    return (int)(R_EFL(&f.c) & 1u);           /* CF */
}

int os_dos_read(uint32_t handle, uint32_t buf, uint32_t count, uint32_t *got)
{
    os_frame_t f; os_frame_init(&f);
    R_EAX(&f.c) = 0x3f00u;                    /* AH=3f */
    R_EBX(&f.c) = handle;
    R_ECX(&f.c) = count;
    R_EDX(&f.c) = buf;
    dos_int21(&f.c);
    if (R_EFL(&f.c) & 1u)                     /* CF: *got untouched */
        return 1;
    *got = R_EAX(&f.c);                        /* bytes read (full 32-bit, as dos.c writes it) */
    return 0;
}

int os_dos_write(uint32_t handle, uint32_t buf, uint32_t count, uint32_t *put)
{
    os_frame_t f; os_frame_init(&f);
    R_EAX(&f.c) = 0x4000u;                    /* AH=40 */
    R_EBX(&f.c) = handle;
    R_ECX(&f.c) = count;
    R_EDX(&f.c) = buf;
    dos_int21(&f.c);
    if (R_EFL(&f.c) & 1u)                     /* CF: *put untouched */
        return 1;
    *put = R_EAX(&f.c);                        /* bytes written */
    return 0;
}

void os_dos_close(uint32_t handle)
{
    os_frame_t f; os_frame_init(&f);
    R_EAX(&f.c) = 0x3e00u;                    /* AH=3e */
    R_EBX(&f.c) = handle;
    dos_int21(&f.c);
}

int os_dos_lseek(uint32_t handle, int32_t off, uint8_t whence, uint32_t *pos)
{
    os_frame_t f; os_frame_init(&f);
    R_EAX(&f.c) = 0x4200u | whence;          /* AH=42, AL=whence */
    R_EBX(&f.c) = handle;
    R_ECX(&f.c) = ((uint32_t)off >> 16) & 0xffffu;  /* CX = offset high half */
    R_EDX(&f.c) = (uint32_t)off & 0xffffu;          /* DX = offset low half  */
    dos_int21(&f.c);
    if (R_EFL(&f.c) & 1u) {                   /* CF: EAX = the raw DOS error code (passed through) */
        if (pos) *pos = R_EAX(&f.c);
        return 1;
    }
    if (pos)                                  /* dos.c returns DX:AX (16:16) -> recompose */
        *pos = ((R_EDX(&f.c) & 0xffffu) << 16) | (R_EAX(&f.c) & 0xffffu);
    return 0;
}

/* ---- the DPMI selector service (int31 0000/0007/0008/0001, see os_api.h) ----
 * Register frames mirror what setup/refresh_das_block_selector set before their `int 0x31`, so
 * dpmi_int31() (dpmi.c) — the exact code the trap runs — services them identically. dpmi.c reports
 * CF via set_cf() (EFL bit 0); 0000 returns the first selector in AX (full R_EAX assignment). */

int os_dpmi_alloc_descriptors(uint16_t count, uint16_t *sel)
{
    os_frame_t f; os_frame_init(&f);
    R_EAX(&f.c) = 0x0000u;                    /* AX=0000, CX=count */
    R_ECX(&f.c) = count;
    dpmi_int31(&f.c);
    if (R_EFL(&f.c) & 1u)                     /* CF: leave *sel untouched (orig reads AX only on success) */
        return 1;
    *sel = (uint16_t)(R_EAX(&f.c) & 0xffffu);
    return 0;
}

int os_dpmi_set_segment_base(uint16_t sel, uint32_t base)
{
    os_frame_t f; os_frame_init(&f);
    R_EAX(&f.c) = 0x0007u;                    /* AX=0007, BX=sel, CX:DX=base */
    R_EBX(&f.c) = sel;
    R_ECX(&f.c) = (base >> 16) & 0xffffu;
    R_EDX(&f.c) = base & 0xffffu;
    dpmi_int31(&f.c);
    return (int)(R_EFL(&f.c) & 1u);           /* CF */
}

int os_dpmi_set_segment_limit(uint16_t sel, uint32_t limit)
{
    os_frame_t f; os_frame_init(&f);
    R_EAX(&f.c) = 0x0008u;                    /* AX=0008, BX=sel, CX:DX=limit */
    R_EBX(&f.c) = sel;
    R_ECX(&f.c) = (limit >> 16) & 0xffffu;
    R_EDX(&f.c) = limit & 0xffffu;
    dpmi_int31(&f.c);
    return (int)(R_EFL(&f.c) & 1u);           /* CF */
}

void os_dpmi_free_descriptor(uint16_t sel)
{
    os_frame_t f; os_frame_init(&f);
    R_EAX(&f.c) = 0x0001u;                    /* AX=0001, BX=sel */
    R_EBX(&f.c) = sel;
    dpmi_int31(&f.c);
}
