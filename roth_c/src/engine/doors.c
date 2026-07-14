/* lift_doors.c — verified-C lifts for the `doors` subsystem.
 *
 * Doors are sector/portals that animate open: the subsystem owns a fixed-array door
 * pool, the per-frame swing animation + open/close state machine, and the "is there a
 * door near this sector / the player" queries. It bridges out only to `audio` (door
 * sounds), `collision_physics` (portal queries), `raw_command_system` (open triggers)
 * and the already-lifted `rotate_quad` / distance leaves. See
 * docs/reference/lift/doors.md.
 *
 * Door pool (canon VAs; runtime = canon + OBJ_DELTA), cross-checked against the
 * already-verified automap lift (lift_automap.c):
 *   0x8b3f4  door count (byte)
 *   0x8b3f8  first door record; stride 0x1f6 (502 bytes/record)
 *   record + 0x00  (u16) sector id A   (the "owning" sector key)
 *   record + 0x02  (u8)  state flags; bit 0x02 = open
 *   record + 0x10  (u16) sector id B   (the connected/neighbour sector key)
 *   record + 0x2e  (u32) STORED PTR -> corner/wall geometry (used by automap)
 *
 * ABI / behaviour transcribed STRICTLY FROM THE DISASM (the corpus decompile is
 * Borland-cspec-on-Watcom and unreliable for register args / multi-reg returns). The
 * three leaves here are pure obj3 readers (no FS/segment use) returning a record
 * pointer + a CF found-flag, so they are oracle-verified by register/CF compare against
 * call_orig over a staged door pool.
 *
 * Functions lifted here:
 *   find_door_by_sector  0x3cfcc — scan pool for sector id (matches +0x10 OR +0); ret EAX=rec, CF.
 *   find_next_door       0x3cff6 — flow_succ into find_door_by_sector: from the record AFTER
 *                                  EAX(cur), continue scanning for the next sector-id match.
 *   is_door_open         0x3d03c — scan pool for sector id (+0); AL = record[+2] & 2 open bit, CF.
 */
#include <stdint.h>
#include <string.h>
#include "common.h"

/* flat (host-address) byte/word access; volatile so faithful reads/writes are emitted. */
#define RB(a)     (*(volatile uint8_t  *)(uintptr_t)(a))
#define RW(a)     (*(volatile uint16_t *)(uintptr_t)(a))
#define RD(a)     (*(volatile uint32_t *)(uintptr_t)(a))
#define WB(a,v)   (*(volatile uint8_t  *)(uintptr_t)(a) = (uint8_t)(v))
#define WW(a,v)   (*(volatile uint16_t *)(uintptr_t)(a) = (uint16_t)(v))
#define WD(a,v)   (*(volatile uint32_t *)(uintptr_t)(a) = (uint32_t)(v))

#define DOOR_COUNT   0x8b3f4   /* door count (byte) */
#define DOOR_POOL    0x8b3f8   /* first door record */
#define DOOR_STRIDE  0x1f6     /* bytes per door record */

/* packed-byte index cycling (model the original's rol/ror of a 4-byte offset table exactly). */
static uint32_t rol32(uint32_t v, int n) { return (uint32_t)((v << n) | (v >> (32 - n))); }
static uint32_t ror32(uint32_t v, int n) { return (uint32_t)((v >> n) | (v << (32 - n))); }

/* ===================== shared scan loop (find_door_by_sector body @0x3cfdc) =====================
 * Scan `cl` records from runtime address `rec`, looking for a record whose +0x10 OR +0 word == si.
 * Mirrors 0x3cfdc..0x3cff5: on a match -> EAX=rec, CF=0 (clc); on exhaustion -> EAX=rec
 * (= start + cl*stride), CF=1 (stc). Caller guarantees the entry path; cl may be 0 (then no match,
 * EAX=rec unchanged, CF=1 — this is the find_next_door "advanced past the last record" tail). */
static int door_scan_loop(uint32_t rec, uint8_t cl, uint16_t si, uint32_t *out_eax)
{
    while (cl != 0) {
        if (RW(rec + 0x10) == si) { *out_eax = rec; return 0; }   /* cmp [eax+0x10],si; je found */
        if (RW(rec + 0x00) == si) { *out_eax = rec; return 0; }   /* cmp [eax],si;      je found */
        rec += DOOR_STRIDE;                                       /* add eax,0x1f6 (0x3cfe7) */
        cl--;                                                     /* dec cl */
    }
    *out_eax = rec;                                               /* 0x3cff0 stc */
    return 1;
}

/* find_door_by_sector 0x3cfcc: SI = sector key. Returns EAX = matching record pointer + CF
 * (0 = found). On an empty pool the original never loads EAX, so it returns the entry EAX
 * unchanged (eax_in). */
int find_door_by_sector(uint16_t si, uint32_t eax_in, uint32_t *out_eax)
{
    uint8_t count = G8(DOOR_COUNT);
    if (count == 0) { *out_eax = eax_in; return 1; }              /* or cl,cl; je 0x3cff0 (eax kept) */
    return door_scan_loop((uint32_t)GADDR(DOOR_POOL), count, si, out_eax);
}

/* find_next_door 0x3cff6: EAX(cur) = a door record pointer, SI = sector key. Locate `cur` in the
 * pool, then continue find_door_by_sector's scan from the NEXT record onward — i.e. "the next door
 * (after cur) matching sector si". If `cur` is not in the pool (or the pool is empty), CF=1 with
 * EAX = base + count*stride (the empty-pool case keeps EAX = cur). */
int find_next_door(uint32_t cur, uint16_t si, uint32_t *out_eax)
{
    uint8_t count = G8(DOOR_COUNT);
    if (count == 0) { *out_eax = cur; return 1; }                 /* or cl,cl; je 0x3d015 (eax=cur) */

    uint32_t base = (uint32_t)GADDR(DOOR_POOL);
    uint32_t rec  = base;
    uint8_t  cl   = count;
    while (cl != 0) {                                             /* 0x3d008 loop */
        if (rec == cur) {                                        /* cmp eax,edx; je 0x3cfe7 */
            rec += DOOR_STRIDE;                                  /* 0x3cfe7 add eax,0x1f6 */
            cl--;                                               /* dec cl */
            return door_scan_loop(rec, cl, si, out_eax);        /* jne 0x3cfdc / fall to stc */
        }
        rec += DOOR_STRIDE;                                      /* add eax,0x1f6 */
        cl--;                                                   /* dec cl */
    }
    *out_eax = rec;                                              /* 0x3d015 stc (eax=base+count*stride) */
    return 1;
}

/* is_door_open 0x3d03c: DI = sector key. Scans the pool for a record whose +0 word == di. On a
 * match: AL = record[+2] & 2 (the open bit), CF=0. Not found: AL = 0, CF=1. Only AL is modified —
 * the upper 24 bits of EAX are the entry EAX (eax_in). */
int is_door_open(uint16_t di, uint32_t eax_in, uint32_t *out_eax)
{
    uint8_t count = G8(DOOR_COUNT);
    if (count != 0) {                                            /* or cl,cl; je 0x3d05c */
        uint32_t rec = (uint32_t)GADDR(DOOR_POOL);
        uint8_t  cl  = count;
        do {
            if (RW(rec + 0x00) == di) {                         /* cmp [ebx],di; je 0x3d062 found */
                uint8_t al = (uint8_t)(RB(rec + 2) & 2);        /* mov al,[ebx+2]; and al,2 */
                *out_eax = (eax_in & 0xffffff00u) | al;
                return 0;                                       /* clc */
            }
            rec += DOOR_STRIDE;                                 /* add ebx,0x1f6 */
            cl--;                                               /* dec cl */
        } while (cl != 0);                                      /* jne 0x3d04d */
    }
    *out_eax = (eax_in & 0xffffff00u);                          /* sub al,al (AL=0) */
    return 1;                                                   /* stc */
}

/* ===================== init_door_pool (0x3d433) =====================
 * Resets both door pools (primary @0x8b3f8 stride 0x1f6, secondary @0x8bfc0 stride 0x2c) and builds
 * 6 primary-record templates. Each record:
 *   +0x34 (u16) = 8                       (a count/limit word the +0x2e ptr points at)
 *   +0x2e (u32) = &record[+0x34]          (self-referential stored ptr -> runtime addr)
 *   +0x2a (u32) = &record[+0xf6]          (self-referential stored ptr -> the sub-struct array)
 *   +0xf6..+0x1f6 = 4 sub-structs of 0x40 bytes; each sub-struct:
 *       +0x30 (u32) = &record[+0x36]      (ptr to record[+0x34]+2)
 *       +0x34 (u16) = 4
 *       +0x36 (5 u16) = 5 words copied from const table 0x724c8 (table re-read per record;
 *                       sub-struct k reads words [k*5 .. k*5+5))
 *       +0x08 (u32) = &next sub-struct    (link; written for sub 0..2, left 0 on the last)
 * Register-transparent (pushal/popal + push/pop es,ds). No FS. Reads the const table from the loaded
 * image; writes only obj3 -> oracle write-set target. */
void init_door_pool(void)
{
    /* secondary pool: count=0, zero 0x42 dwords @0x8bfc0 (6 records * 0x2c, ends @0x8c0c8). */
    G8(VA_g_secondary_door_count) = 0;
    uint32_t sec = (uint32_t)GADDR(VA_g_secondary_door_pool);
    for (int i = 0; i < 0x42; i++) WD(sec + (uint32_t)i * 4, 0);

    /* primary pool: count=0, zero 0x2f1 dwords @0x8b3f8 (6 records * 0x1f6, ends @0x8bfbc). */
    G8(VA_g_door_count) = 0;
    uint32_t base = (uint32_t)GADDR(VA_g_door_pool);
    for (int i = 0; i < 0x2f1; i++) WD(base + (uint32_t)i * 4, 0);

    uint32_t esi = base;
    for (int r = 0; r < 6; r++) {                       /* cl = 6 outer */
        uint32_t off724 = 0;                            /* mov ebx,0x724c8 (reset per record) */
        WW(esi + 0x34, 8);                              /* mov word [esi+0x34],8 */
        WD(esi + 0x2e, esi + 0x34);                     /* mov [esi+0x2e],edi (=esi+0x34) */
        uint32_t edi = esi + 0xf6;                       /* lea edi,[esi+0xf6] */
        WD(esi + 0x2a, edi);                            /* mov [esi+0x2a],edi */

        for (int k = 0; k < 4; k++) {                   /* cl = 4 inner */
            uint32_t edx = edi;                         /* mov edx,edi */
            WD(edi + 0x30, (esi + 0x34) + 2);           /* [esi+0x2e]+2 = esi+0x36 */
            WW(edi + 0x34, 4);                          /* mov word [edi+0x34],4 */
            edi += 0x36;                                /* add edi,0x36 */
            for (int w = 0; w < 5; w++) {               /* ch = 5: stosw from table */
                WW(edi, G16(VA_g_door_vertex_template + off724));
                off724 += 2;
                edi += 2;
            }
            if (k != 3) WD(edx + 8, edi);               /* cmp cl,1; je skip -> link all but the last */
        }
        esi += 0x1f6;                                   /* add esi,0x1f6 (next record) */
    }
}

/* ===================== compute_door_quad_bounds (0x3de36) =====================
 * EDI = door record. Computes the signed 16-bit bounding box of the door's 4 corner vertices
 * (corner array @ [edi+0x2e]+0x82, stride 0x10, x@+0 / y@+4), expands it by 0x1a on every side
 * (16-bit wrapping add/sub), and tests whether the query point (gx=[0x90a8e], gy=[0x90a96]) lies
 * inside. Returns CF: 1 = inside (stc @0x3decb), 0 = outside (clc @0x3dec9). Register-transparent
 * (esi/eax/ebx/edx/ecx pushed+popped), no FS. The min/max accumulators are the original's two
 * 16-bit halves packed into eax(max)/edx(min) and cycled with rol — modelled here as plain scalars. */
int compute_door_quad_bounds(uint32_t edi)
{
    int16_t maxX = (int16_t)0x8000, maxY = (int16_t)0x8000;   /* eax = 0x80008000 */
    int16_t minX = (int16_t)0x7fff, minY = (int16_t)0x7fff;   /* edx = 0x7fff7fff */

    uint32_t esi = RD(edi + 0x2e) + 0x82;                     /* [edi+0x2e] (stored ptr) + 0x82 */
    for (int i = 0; i < 4; i++) {                             /* ecx = 4 */
        int16_t x = (int16_t)RW(esi + 0);                    /* mov bx,[esi] */
        if (x >= maxX) maxX = x;                             /* cmp bx,ax; jl skip -> ax=bx if bx>=ax */
        if (x <= minX) minX = x;                             /* cmp bx,dx; jg skip -> dx=bx if bx<=dx */
        int16_t y = (int16_t)RW(esi + 4);                    /* mov bx,[esi+4] */
        if (y >= maxY) maxY = y;
        if (y <= minY) minY = y;
        esi += 0x10;
    }

    int16_t gy  = (int16_t)G16(VA_g_player_y);                     /* query Y */
    int16_t hiY = (int16_t)(maxY + 0x1a);                    /* add ax,0x1a (16-bit) */
    int16_t loY = (int16_t)(minY - 0x1a);                    /* sub dx,0x1a (16-bit) */
    if (gy > hiY) return 0;                                  /* cmp bx,ax; jg out */
    if (gy < loY) return 0;                                  /* cmp bx,dx; jl out */

    int16_t gx  = (int16_t)G16(VA_g_player_x);                     /* query X */
    int16_t hiX = (int16_t)(maxX + 0x1a);
    int16_t loX = (int16_t)(minX - 0x1a);
    if (gx > hiX) return 0;                                  /* cmp bx,ax; jg out */
    if (gx >= loX) return 1;                                 /* cmp bx,dx; jge in (stc) */
    return 0;                                                /* clc */
}

/* ===================== update_door_swing (0x3de31) =====================
 * 5-byte flow_succ entry stub: `call rotate_quad (0x3ded2)` then falls straight into
 * compute_door_quad_bounds (0x3de36). rotate_quad is pushal/popal (preserves every register incl.
 * EDI + the EAX passthrough), so EDI survives into the bounds check. Net: rotate the door's portal
 * quad to screen space (writing each point's +0x80/+0x84, which is exactly the +0x82 corner array
 * the bounds check reads), then test whether the query point is inside. Returns CF. */
int update_door_swing(uint32_t eax, uint32_t edi)
{
    rotate_quad(eax, (uint8_t *)(uintptr_t)edi);     /* call 0x3ded2 (already lifted) */
    return compute_door_quad_bounds(edi);            /* fall into 0x3de36 */
}

/* ===================== lookup_door_record_by_sector (0x3d4da) =====================
 * EAX = an offset into the FS geometry segment. Resolves a sector id via a chained deref —
 *   S   = u16 @ fs:[(EAX & 0xffff) + 8]   (16-bit addressing, offset wraps mod 0x10000)
 *   key = u16 @ fs:[S + 6]
 * then scans the primary door pool (count@0x8b3f4, base@0x8b3f8, stride 0x1f6) and, failing that,
 * the secondary pool (count@0x8bfbc, base@0x8bfc0, stride 0x2c) for a record whose +0 word == key.
 * Returns EAX = the matching record pointer, or 0 if neither pool matches. `fs_base` is the resolved
 * FS-selector base (caller/host resolves [0x852c8]); other regs (ebx/edi/ecx/fs) preserved. */
uint32_t lookup_door_record_by_sector(uint32_t eax_in, uint32_t fs_base)
{
    uint16_t bx  = (uint16_t)eax_in;
    uint16_t s   = RW(fs_base + (uint16_t)(bx + 8));        /* fs:[bx+8] (16-bit EA) */
    uint16_t key = RW(fs_base + s + 6);                     /* fs:[S+6] */

    uint8_t  cnt = G8(VA_g_door_count);                             /* primary pool */
    uint32_t rec = (uint32_t)GADDR(VA_g_door_pool);
    for (; cnt != 0; cnt--, rec += 0x1f6)
        if (RW(rec) == key) return rec;                    /* cmp [edi],ax; je -> mov eax,edi */

    cnt = G8(VA_g_secondary_door_count);                                      /* secondary pool */
    rec = (uint32_t)GADDR(VA_g_secondary_door_pool);
    for (; cnt != 0; cnt--, rec += 0x2c)
        if (RW(rec) == key) return rec;

    return 0;                                               /* sub eax,eax */
}

/* ===================== setup_door_corner_surface_a/_b (0x3d387 / 0x3d3a8) =====================
 * A flow_succ pair that fills one door corner-surface record (at EAX) from the FS geometry segment.
 * `_a` (0x3d387) is a 33-byte entry that computes [eax+0x26] from EDX, sets si=fs:[bx+4], then JMPs
 * into `_b`'s shared tail at 0x3d3c2. `_b` (0x3d3a8) is the full body: chase bx=fs:[bx+8],
 * si=fs:[bx+4], [eax+0x26]=fs:[si]&0xfff, then the shared tail. fs_base = resolved [0x852c8] base.
 * Inputs (read from disasm): EAX=out record, EBX=bx index, EDX=index (_a only), EDI=di index,
 * EBP=frame ptr ([ebp+0xc] word, [ebp+0x10] byte). All FS reads use 16-bit addressing (regs bx/si/di)
 * except _a's `fs:[edx+4]` (32-bit). The tail may write obj3 word [0x8c0c8]. */
static void door_corner_tail(uint32_t eax, uint16_t si, uint16_t di, uint32_t ebp, uint32_t fs)
{
    WW(eax + 0xc, RW(fs + (uint16_t)(si + 2)));             /* mov dx,fs:[si+2]; mov [eax+0xc],dx */
    uint8_t ch = RB(fs + (uint16_t)(si + 8));               /* mov ch,fs:[si+8] */
    uint32_t dx = ch;                                       /* sub edx,edx; mov dl,ch */
    if (!(ch & 4))    dx |= 0x100;                          /* test ch,4; jne; or dx,0x100 */
    dx &= 0x103;                                            /* and dx,0x103 */
    if (!(ch & 0x10)) dx |= 0x40;                           /* test ch,0x10; jne; or dx,0x40 */
    dx |= 0x18;                                             /* or dx,0x18 */
    if (ch & 9) {                                           /* test ch,9; je [eax+0x16] */
        if (ch & 8) {                                       /* test ch,8; je ch&1 */
            int32_t b = (int8_t)RB(fs + (uint16_t)(di + 0xc));  /* movsx ebx,byte fs:[di+0xc] */
            if (b != 0) WW(GADDR(VA_g_secondary_door_pool + 0x108), (uint16_t)(b << 2)); /* shl ebx,2; mov [0x8c0c8],bx */
        }
        if (!(ch & 1)) dx &= 0xfff7;                        /* test ch,1; jne; and dx,~8 */
    }
    WW(eax + 0x16, (uint16_t)dx);                           /* mov [eax+0x16],dx */
    WB(eax + 0xe, RB(ebp + 0x10) ? 0x80 : 0);              /* dl=[ebp+0x10]; or dl,dl; je; dl=0x80; [eax+0xe] */
    WW(eax + 0x24, RW(ebp + 0xc));                          /* mov dx,[ebp+0xc]; mov [eax+0x24],dx */
}

void setup_door_corner_surface_b(uint32_t eax, uint32_t ebx, uint32_t edi, uint32_t ebp, uint32_t fs)
{
    uint16_t bx  = RW(fs + (uint16_t)((uint16_t)ebx + 8));  /* mov bx,fs:[bx+8] */
    uint16_t si  = RW(fs + (uint16_t)(bx + 4));             /* mov si,fs:[bx+4] */
    WW(eax + 0x26, RW(fs + si) & 0xfff);                    /* mov dx,fs:[si]; and dx,0xfff; [eax+0x26] */
    door_corner_tail(eax, si, (uint16_t)edi, ebp, fs);
}

void setup_door_corner_surface_a(uint32_t eax, uint32_t ebx, uint32_t edx,
                                        uint32_t edi, uint32_t ebp, uint32_t fs)
{
    uint16_t si1 = RW(fs + (edx & 0xffff) + 4);            /* and edx,0xffff; mov si,fs:[edx+4] (32-bit EA) */
    WW(eax + 0x26, RW(fs + si1) & 0xfff);                  /* mov dx,fs:[si]; and dx,0xfff; [eax+0x26] */
    uint16_t si  = RW(fs + (uint16_t)((uint16_t)ebx + 4)); /* mov si,fs:[bx+4]; jmp tail */
    door_corner_tail(eax, si, (uint16_t)edi, ebp, fs);
}

/* ===================== resolve_door_neighbor_sector (0x3d749) =====================
 * Walks the FS sector/portal graph from the start node EAX to find a *neighbouring* sector (one whose
 * id differs from the start sector's id `di`), writing it to obj3 word [0x8c0cc] and returning CF=1.
 * FS is a register input (the caller sets it). EAX/ESI/EDI/ECX preserved. Addressing: fs:[eax+..] is
 * 32-bit (eax chains through the graph), fs:[di+..]/fs:[si+..] are 16-bit. Chain:
 *   a  = fs:[eax+8]; if 0xffff -> CF=0.  di = fs:[a+6] (sector id).  cl = fs:[di+0xd] (outer budget).
 *   outer (eax=fs:[di+0xe], do-while dec cl / jg): si=fs:[eax+8] (skip if 0xffff); si=fs:[si+6];
 *     skip if fs:[si+0x14] < 0xfffe (unsigned); else inner from eax=fs:[si+0xe]:
 *       s2=fs:[eax+8]; if 0xffff advance; if di != fs:[s2+6] -> FOUND ([0x8c0cc]=s2, CF=1); else advance.
 * Returns CF; writes obj3 [0x8c0cc] only on the found path. */
int resolve_door_neighbor_sector(uint32_t eax_in, uint32_t fs)
{
    uint16_t a = RW(fs + eax_in + 8);                      /* mov ax,fs:[eax+8] */
    if (a == 0xffff) return 0;                             /* cmp ax,-1; je clc */
    uint16_t di = RW(fs + a + 6);                          /* mov di,fs:[eax+6] (eax=a) */
    uint8_t  cl = RB(fs + (uint16_t)(di + 0xd));           /* mov cl,fs:[di+0xd] */
    uint32_t eax = RW(fs + (uint16_t)(di + 0xe));          /* mov ax,fs:[di+0xe] (E0) */

    do {                                                  /* 0x3d768 outer */
        uint16_t si = RW(fs + eax + 8);                   /* mov si,fs:[eax+8] */
        if (si != 0xffff) {                               /* cmp si,-1; je next */
            si = RW(fs + (uint16_t)(si + 6));             /* mov si,fs:[si+6] */
            if (RW(fs + (uint16_t)(si + 0x14)) >= 0xfffe) {  /* cmp word fs:[si+0x14],-2; jb next */
                eax = RW(fs + (uint16_t)(si + 0xe));     /* mov ax,fs:[si+0xe] (I0) */
                for (;;) {                               /* 0x3d788 inner (exits only via found) */
                    uint16_t s2 = RW(fs + eax + 8);      /* mov si,fs:[eax+8] */
                    if (s2 == 0xffff) { eax += 0xc; continue; }       /* je advance */
                    if (di != RW(fs + (uint16_t)(s2 + 6))) {          /* cmp di,fs:[si+6]; jne found */
                        WW(GADDR(VA_g_secondary_door_pool + 0x10c), s2);          /* mov ax,si; mov [0x8c0cc],ax */
                        return 1;                        /* stc */
                    }
                    eax += 0xc;                          /* advance */
                }
            }
        }
        eax += 0xc;                                       /* 0x3d7ac next */
        cl = (uint8_t)(cl - 1);                           /* dec cl */
    } while ((int8_t)cl > 0);                             /* jg 0x3d768 */
    return 0;                                             /* clc */
}

/* ===================== collect_doors_near_query (0x3fa62) =====================
 * Append every nearby portal-wall to a caller list. EBX=sector key, ESI=record (reads word[esi+4]),
 * EDI=output cursor. Index = (uint16)((EBX - [esi+4]) / 0xd) + 2; off = word @ geom[index]
 * (geom = stored ptr [0x90aa4]); if 0 -> done. list = geom+off, count = byte[list], walls at list+2
 * (stride 0x10, x@+0 / y@+2 / flag bytes @+7,+9). Per wall: skip if (f7 & 0x82) || (f9 & 0x82); else
 * compute the Chebyshev distance max(|x-qx|,|y-qy|) (qx=[0x8c122], qy=[0x8c12a]) and, if <= 0x120,
 * write {0, wall_ptr} (8 bytes) and advance the cursor. Returns the advanced EDI. ESI/EBX/ECX
 * preserved. The abs is the original's 32-bit `neg` of a zero-extended 16-bit delta (low-16 magnitude). */
uint32_t collect_doors_near_query(uint32_t ebx, uint32_t esi_rec, uint32_t edi_out)
{
    uint16_t baseW = RW(esi_rec + 4);                      /* word[esi+4] */
    uint16_t delta = (uint16_t)((uint16_t)ebx - baseW);    /* sub ax,[esi+4] */
    uint16_t index = (uint16_t)((uint16_t)(delta / 0xd) + 2);  /* div bx (unsigned 16); add ax,2 */
    uint32_t geom  = (uint32_t)G32(VA_g_map_objects_buffer);               /* mov esi,[0x90aa4] */
    uint16_t off   = RW(geom + index);                     /* mov bx,[esi+ebx] */
    if (off == 0) return edi_out;                          /* or ebx,ebx; je done */
    uint32_t list  = geom + off;                           /* add ebx,esi */
    uint8_t  cnt   = RB(list);                             /* mov cl,[ebx] */
    if (cnt == 0) return edi_out;                          /* or cl,cl; je done */
    uint32_t w     = list + 2;                             /* add ebx,2 */
    int16_t  qx    = (int16_t)G16(VA_g_locate_query_x + 0x2);
    int16_t  qy    = (int16_t)G16(VA_g_locate_query_y + 0x2);

    do {
        if (!(RB(w + 7) & 0x82) && !(RB(w + 9) & 0x82)) {  /* test [ebx+7],0x82 / [ebx+9],0x82; jne skip */
            int16_t dxv = (int16_t)((uint16_t)RW(w + 0) - (uint16_t)qx);  /* sub ax,[0x8c122] */
            uint16_t adx = (dxv < 0) ? (uint16_t)(-dxv) : (uint16_t)dxv;  /* jns; neg eax */
            int16_t dyv = (int16_t)((uint16_t)RW(w + 2) - (uint16_t)qy);  /* sub dx,[0x8c12a] */
            uint16_t ady = (dyv < 0) ? (uint16_t)(-dyv) : (uint16_t)dyv;  /* jns; neg edx */
            uint16_t dist = (adx > ady) ? adx : ady;       /* cmp ax,dx; ja keep; else eax=edx */
            if (dist <= 0x120) {                           /* cmp ax,0x120; ja skip */
                WD(edi_out + 0, 0);                        /* mov [edi],0 */
                WD(edi_out + 4, w);                        /* mov [edi+4],ebx (wall ptr) */
                edi_out += 8;                              /* add edi,8 */
            }
        }
        w += 0x10;                                         /* add ebx,0x10 */
        cnt = (uint8_t)(cnt - 1);                          /* dec cl */
    } while ((int8_t)cnt > 0);                             /* jg loop */
    return edi_out;
}

/* ===================== gather_nearby_doors (0x3f93b) =====================
 * Builds the "nearby interactables" worklist consumed by collision (×3). Reads everything from globals
 * (register-transparent). Phase 1: for the current sector (offset @[0x90c12]) and each of its neighbour
 * sectors, append every matching door ({1, door_rec}) via find_door_by_sector/find_next_door, and the
 * nearby portal-walls ({0, wall}) via collect_doors_near_query — gated by the sector flag byte
 * [geom+key+0x16] (bit0 = doors, bit1 = walls). Phase 2: walk the state-entity pool (count@0x91e00,
 * recs@0x91e04 stride 0x22, ptr@+4), appending {0, ent} for each non-null entity within a ±0xa0 box of
 * the query (qx=[0x8c122], qy=[0x8c12a]; null entries skipped without consuming the count). Finally
 * [0x8c12c] = entry count = (edi - base)/8. geom = [0x90aa8], output base = [0x8498c]. */
static uint32_t door_gather_sector(uint32_t geom, uint16_t key, uint32_t edi)
{
    uint8_t flag = RB(geom + key + 0x16);
    if (flag & 1) {                                        /* test [esi+ebx+0x16],1 */
        uint32_t rec;
        int cf = find_door_by_sector(key, 0, &rec); /* mov esi,ebx; call 0x3cfcc */
        while (!cf) {                                      /* jb done / jae loop */
            WD(edi + 0, 1); WD(edi + 4, rec); edi += 8;    /* {1, door_rec} */
            cf = find_next_door(rec, key, &rec);    /* call 0x3cff6 */
        }
    }
    if (flag & 2)                                          /* test [esi+ebx+0x16],2 */
        edi = collect_doors_near_query(key, geom, edi);  /* call 0x3fa62 */
    return edi;
}

void gather_nearby_doors(void)
{
    uint32_t edi  = (uint32_t)G32(VA_g_door_worklist);                /* output buffer base */
    uint32_t geom = (uint32_t)G32(VA_g_map_geometry_buffer);               /* geometry buffer */
    uint16_t bx   = G16(VA_g_player_sector);                          /* current sector offset */

    if (bx != 0) {                                         /* or bx,bx; je phase2 */
        edi = door_gather_sector(geom, bx, edi);          /* current sector */
        uint8_t  cl = RB(geom + bx + 0xd);                /* neighbour count */
        uint16_t nb = RW(geom + bx + 0xe);                /* neighbour list start */
        do {                                              /* 0x3f99d */
            uint16_t ax = RW(geom + nb + 8);              /* mov ax,[esi+ebx+8] */
            if (ax != 0xffff) {                           /* cmp ax,-1; je next */
                uint16_t k = RW(geom + ax + 6);           /* mov bx,[esi+eax+6] */
                edi = door_gather_sector(geom, k, edi);
            }
            nb += 0xc;                                     /* add ebx,0xc */
            cl = (uint8_t)(cl - 1);                        /* dec cl */
        } while ((int8_t)cl > 0);                          /* jg 0x3f99d */
    }

    uint32_t ecx = (uint32_t)G32(VA_g_state_pool_a_count);                 /* state-pool count */
    if (ecx != 0) {
        uint32_t rec = (uint32_t)GADDR(VA_g_state_pool_a_records);
        for (;;) {                                         /* 0x3f9fa */
            uint32_t edx = RD(rec + 4);                    /* entity ptr @+4 */
            if (edx != 0) {                                /* or edx,edx; je next (no dec) */
                if (!(RB(edx + 7) & 0x82)) {               /* test [edx+7],0x82; jne skip */
                    int32_t dx = (int16_t)((uint16_t)G16(VA_g_locate_query_x + 0x2) - (uint16_t)RW(edx + 0)); /* sub ax,[edx]; cwde */
                    if (!(dx > 0xa0 || dx < -0xa0)) {      /* cmp eax,0xa0 jg; cmp eax,-0xa0 jl */
                        int32_t dy = (int16_t)((uint16_t)G16(VA_g_locate_query_y + 0x2) - (uint16_t)RW(edx + 2));
                        if (!(dy > 0xa0 || dy < -0xa0)) {
                            WD(edi + 0, 0); WD(edi + 4, edx); edi += 8;  /* {0, ent} */
                        }
                    }
                }
                ecx = (uint32_t)(ecx - 1);                 /* dec ecx */
                if ((int32_t)ecx <= 0) break;              /* jle phase-end */
            }
            rec += 0x22;                                   /* 0x3fa47 add esi,0x22 */
        }
    }

    edi -= (uint32_t)G32(VA_g_door_worklist);                         /* sub edi,[0x8498c] */
    G32(VA_g_collision_entity_count) = (int32_t)(edi >> 3);                    /* shr edi,3; mov [0x8c12c],edi */
}

/* ===================== setup_door_swing_geometry (0x3d147) =====================
 * Build a door's full swing-animation record from the FS portal/sector graph + the GS vertex segment.
 * ESI = the door record (a door-pool slot); EDI = sector key (offset into FS); EAX/EDX = caller inputs
 * stashed into the record. Pipeline:
 *   1. seed record[0]=key, record[2]=0, record[0x12]=dx; require the sector to have exactly 4 corners
 *      (fs:[key+0xd]==4) else return CF=1.
 *   2. search the 4 corners (packed-offset order {0,0xc,0x18,0x24} cycled by rol/ror of 0x24180c00) for
 *      the "hinge" corner whose edge has fs:[edge]&0x8000 set AND fs:[edge+0xc] in {0xfffd,0xfffe,0xffff}
 *      (i.e. >= -3 unsigned); not found -> CF=1. Record that marker word in [0x8c0ca].
 *   3. order the 4 corner pointers into a local frame (ror from the hinge), derive two packed sector ids
 *      (dxA<<16|dxB from corners 1 & 3), conditionally swap the quad winding (marker!=-2 && (marker==-3
 *      || dxB==record[0x12]) -> ror edx / rol ebx / record[2]|=1), tag the two touched sectors
 *      fs:[key+0x16]|=1 and fs:[edx_lo+0x16]|=1, then fill record fields 0xa/0xc/0x10/0x12/0x14/0x16 from
 *      the GS hinge vertex + the [0x8c0d0]/[0x8b3c4] globals; [0x8c0c8]=0.
 *   4. fill the record's 4 corner sub-structs (record+0xf6, chained via +8) with the already-lifted
 *      corner-surface fillers (_a/_a/_b/_b), build the 4 quad points (record+0x36 stride 0x10, +0x40
 *      mirror) from the GS vertices, rotate them to screen space (rotate_quad), and inc the door count
 *      [0x8b3f4]. Return CF=0.
 * FS is a register input (caller-set); GS is loaded by the original from [0x852cc] — the lift takes both
 * resolved flat bases (fs/gs) as params. obj3 writes (the record + [0x8c0ca]/[0x8c0c8]/[0x8b3f4]) plus
 * the two FS tag-writes are the oracle's compared write-set. */
int setup_door_swing_geometry(uint32_t eax_in, uint32_t esi_rec, uint32_t edx_in,
                                     uint32_t edi_in, uint32_t fs, uint32_t gs)
{
    uint8_t frame[0x1c];                                    /* the 0x1c-byte stack frame ([ebp+..]) */
    #define FW(o) (*(uint16_t *)(frame + (o)))
    #define FB(o) (*(uint8_t  *)(frame + (o)))
    uint32_t frame_addr = (uint32_t)(uintptr_t)frame;

    uint16_t key = (uint16_t)edi_in;                       /* di = sector key (FS offset) */
    FW(0x18) = (uint16_t)eax_in;                           /* [ebp+0x18] = ax (stashed; unread) */
    WW(esi_rec + 0x12, (uint16_t)edx_in);                  /* record[0x12] = dx */
    WW(esi_rec + 0x00, key);                               /* record[0]    = di */
    WB(esi_rec + 0x02, 0);                                 /* record[2]    = 0 */
    FB(0x10) = RB(fs + (uint16_t)(key + 0xb));             /* [ebp+0x10] = fs:[di+0xb] */
    if (RB(fs + (uint16_t)(key + 0xd)) != 4) return 1;     /* cmp cl,4; jne error (stc) */

    /* --- find the hinge corner (rol the packed offset table) --- */
    uint32_t pk = 0x24180c00;
    uint16_t edge = 0;
    uint8_t  n = 4;
    int found = 0;
    for (;;) {
        uint16_t bx = (uint16_t)((pk & 0xff) + RW(fs + (uint16_t)(key + 0xe)));   /* bl=al; add bx,fs:[di+0xe] */
        edge = RW(fs + (uint16_t)(bx + 4));                                       /* mov si,fs:[bx+4] */
        if ((RW(fs + edge) & 0x8000) &&                                           /* test fs:[si],0x8000 */
            RW(fs + (uint16_t)(edge + 0xc)) >= 0xfffd) { found = 1; break; }      /* cmp fs:[si+0xc],-3; jae */
        pk = rol32(pk, 8);                                                        /* rol eax,8 */
        if ((int8_t)(--n) <= 0) break;                                           /* dec cl; jg */
    }
    if (!found) return 1;                                                         /* fell through -> stc */

    uint16_t marker = RW(fs + (uint16_t)(edge + 0xc));     /* cx = fs:[si+0xc] */
    WW(GADDR(VA_g_secondary_door_pool + 0x10a), marker);                            /* [0x8c0ca] = cx */

    /* --- order the 4 corner pointers into the frame (ror from the hinge) --- */
    for (int k = 0; k < 4; k++) {
        FW(k * 2) = (uint16_t)((pk & 0xff) + RW(fs + (uint16_t)(key + 0xe)));     /* [ebp+k*2] = base+al */
        pk = ror32(pk, 8);                                                        /* ror eax,8 */
    }

    /* --- derive the two packed sector ids + the hinge vertex dword --- */
    uint32_t ebx = RD(fs + FW(0));                         /* ebx = fs:[corner0] (dword) */
    uint16_t t   = RW(fs + (uint16_t)(FW(2) + 8));         /* di = fs:[corner1+8] */
    uint16_t dxA = RW(fs + (uint16_t)(t + 6));             /* dx = fs:[di+6] */
    t            = RW(fs + (uint16_t)(FW(6) + 8));         /* di = fs:[corner3+8] */
    uint16_t dxB = RW(fs + (uint16_t)(t + 6));
    uint32_t edx = ((uint32_t)dxA << 16) | dxB;            /* edx = (dxA<<16)|dxB */

    int swap;
    if (marker == 0xfffe)      swap = 0;                   /* cmp [0x8c0ca],-2; je no-swap */
    else if (marker == 0xfffd) swap = 1;                  /* cmp [0x8c0ca],-3; je swap */
    else swap = (dxB == RW(esi_rec + 0x12));              /* cmp dx,[esi+0x12]; je swap */
    if (swap) {
        edx = ror32(edx, 16);                             /* ror edx,0x10 */
        ebx = rol32(ebx, 16);                             /* rol ebx,0x10 */
        WB(esi_rec + 2, RB(esi_rec + 2) | 1);             /* or [esi+2],1 */
    }

    WW(GADDR(VA_g_secondary_door_pool + 0x108), 0);                                 /* [0x8c0c8] = 0 */
    uint16_t k0 = RW(esi_rec + 0);                         /* di = record[0] (sector key) */
    WB(fs + (uint16_t)(k0 + 0x16), RB(fs + (uint16_t)(k0 + 0x16)) | 1);   /* or fs:[di+0x16],1 */
    FW(0xc) = (uint16_t)(RW(fs + k0) - RW(fs + (uint16_t)(k0 + 2)));      /* ax=fs:[di]-fs:[di+2]; [ebp+0xc]=ax */
    int32_t negv = -(((int32_t)(int8_t)RB(fs + (uint16_t)(k0 + 0xc))) << 2);  /* movsx fs:[di+0xc]; shl 2; neg */
    uint16_t dch = RW(fs + (uint16_t)(FW(2) + 4));         /* di = fs:[corner1+4] */
    uint8_t  ch  = RB(fs + (uint16_t)(dch + 8));           /* ch = fs:[di+8] */
    if ((ch & 1) && (ch & 8)) FW(0xc) = (uint16_t)negv;   /* test ch,1/ch,8; -> [ebp+0xc]=ax */

    uint16_t edx_lo = (uint16_t)edx;                       /* edi=edx (persists across the corner calls) */
    WB(fs + (uint16_t)(edx_lo + 0x16), RB(fs + (uint16_t)(edx_lo + 0x16)) | 1);   /* or fs:[di+0x16],1 */
    WW(esi_rec + 0x10, edx_lo);                            /* record[0x10] = dx */
    WW(esi_rec + 0x12, (uint16_t)(edx >> 16));             /* record[0x12] = dx (high) */
    uint16_t bxv = (uint16_t)ebx;                          /* bx = ebx low (hinge vertex idx) */
    WW(esi_rec + 0x14, RW(gs + (uint16_t)(bxv + 8)));      /* record[0x14] = gs:[bx+8] */
    WW(esi_rec + 0x16, RW(gs + (uint16_t)(bxv + 0xa)));    /* record[0x16] = gs:[bx+0xa] */
    WW(esi_rec + 0x0a, G16(VA_g_secondary_door_pool + 0x110));                      /* record[0xa] = [0x8c0d0] */
    WD(esi_rec + 0x0c, (uint32_t)G32(VA_g_snapshot_filename_buf + 0x54));            /* record[0xc] = [0x8b3c4] */

    /* --- fill the 4 corner sub-structs (record+0xf6, chained via +8); edi = the packed edx pair --- */
    uint32_t sub = esi_rec + 0xf6;
    setup_door_corner_surface_a(sub, FW(6), FW(4), edx, frame_addr, fs);   /* _a(ebx=c3,edx=c2) */
    sub = RD(sub + 8);
    setup_door_corner_surface_a(sub, FW(2), FW(0), edx, frame_addr, fs);   /* _a(ebx=c1,edx=c0) */
    sub = RD(sub + 8);
    setup_door_corner_surface_b(sub, FW(6), edx, frame_addr, fs);          /* _b(ebx=c3) */
    sub = RD(sub + 8);
    setup_door_corner_surface_b(sub, FW(2), edx, frame_addr, fs);          /* _b(ebx=c1) */

    /* --- build the 4 quad points from the GS vertices (record+0x36 stride 0x10, +0x40 mirror) --- */
    uint32_t pt = RD(esi_rec + 0x2e) + 2;                  /* ebx = record[0x2e]; add ebx,2 */
    WB(esi_rec + 3, 0);                                    /* record[3] = 0 (rotate_quad angle) */
    FW(0xe) = RW(esi_rec + 0x16);                          /* [ebp+0xe] = record[0x16] */
    uint16_t sub_x = RW(esi_rec + 0x14);                  /* dx = record[0x14] */
    for (int k = 0; k < 4; k++) {
        uint16_t s = RW(fs + FW(k * 2));                  /* si=[ebp+k*2]; si=fs:[si] (vertex idx) */
        WW(pt + 2, FW(0xc));                              /* [ebx+2] = [ebp+0xc] */
        WW(pt + 0x12, 0);                                 /* [ebx+0x12] = 0 */
        uint16_t vx = (uint16_t)(RW(gs + (uint16_t)(s + 8)) - sub_x);             /* gs:[si+8]-dx */
        WW(pt + 0, vx);  WW(pt + 0x40, vx);
        uint16_t vy = (uint16_t)(RW(gs + (uint16_t)(s + 0xa)) - FW(0xe));         /* gs:[si+0xa]-[ebp+0xe] */
        WW(pt + 4, vy);  WW(pt + 0x44, vy);
        pt += 0x10;
    }

    rotate_quad(0, (uint8_t *)(uintptr_t)esi_rec);  /* call 0x3ded2 (eax irrelevant: pushal) */
    G8(VA_g_door_count) = (uint8_t)(G8(VA_g_door_count) + 1);              /* inc byte [0x8b3f4] */
    return 0;                                              /* clc */
    #undef FW
    #undef FB
}

/* ===================== test_door_query_near_player (0x3dafb) =====================
 * Vertical gate + portal-proximity test for a door query. BX = the door's top Z; EDI = a record whose
 * word[edi] is the sector key. If BX >= (int16)([0x90a92]+0x40) (the query is at/above the door top) ->
 * CF=0 (no interaction). Otherwise delegate to the already-lifted scan_portal_walls_near_query
 * (eax=word[edi], dx=[0x90c12] current-sector guard): CF = "a portal wall of that sector is near the
 * player". Register-transparent passthrough of the callee's CF (jae->clc / fall-through keeps stc). */
int test_door_query_near_player(uint16_t bx, uint32_t edi)
{
    int16_t thr = (int16_t)((uint16_t)G16(VA_g_player_z) + 0x40);    /* ax = [0x90a92]+0x40 */
    if ((int16_t)bx >= thr) return 0;                          /* cmp bx,ax; jge -> clc */
    uint16_t key = RW(edi);                                    /* mov ax,[edi] */
    return scan_portal_walls_near_query(key, (uint16_t)G16(VA_g_player_sector));  /* call 0x3db20; CF passthrough */
}

/* ===================== alloc_door_record (0x3d6c3) =====================
 * Allocate the next free PRIMARY door-pool slot (door_pool[count] @0x8b3f8 stride 0x1f6) and initialise
 * its swing state via setup_door_swing_geometry, then fill the open-animation fields + (if a sound id is
 * set) play the open SFX. EAX/EDX/EDI + FS (register input) / GS ([0x852cc]) thread straight into setup
 * (which also increments the door count [0x8b3f4]). On setup failure (CF=1) -> return 0. Else:
 *   [slot+4]=[0x8b3c8]; target vector [slot+0x26]=[0x8b3ce], [slot+0x28]=[0x8b3d0]; on the open sub-record
 *   e=slot+0x14: [e+9]=0, [e+0x10]=0x40 (90deg swing range), [e+8]=0x80, [e+0xa]=0; a unique id
 *   [e+6] = 0xef00 | byte[0x8c0d2] then inc [0x8c0d2]. If a sound id is set ([0x8b3cc]!=0): [e+4]=
 *   [0x8b3cc]-1, [e+0xa]=0x3e8 (1000-tick open timer), and play_world_sound_at_pos(eax=e, edx=cc-1) —
 *   DIRECT C (the SFX leaf is gated on g_sound_enabled 0x7f550 so it no-ops when sound is off). Ret slot. */
uint32_t alloc_door_record(uint32_t eax_in, uint32_t edx_in, uint32_t edi_in,
                                  uint32_t fs, uint32_t gs)
{
    uint32_t slot = (uint32_t)GADDR(VA_g_door_pool) + (uint32_t)G8(VA_g_door_count) * 0x1f6;   /* &door_pool[count] */
    if (setup_door_swing_geometry(eax_in, slot, edx_in, edi_in, fs, gs))
        return 0;                                  /* jb 0x3d746 -> sub eax,eax */

    WD(slot + 4, (uint32_t)G32(VA_g_snapshot_filename_buf + 0x58));          /* [slot+4] = [0x8b3c8] */
    WW(slot + 0x26, G16(VA_g_snapshot_filename_buf + 0x5e));                 /* target vec x = [0x8b3ce] */
    WW(slot + 0x28, G16(VA_g_snapshot_filename_buf + 0x60));                 /* target vec y = [0x8b3d0] */
    uint32_t e = slot + 0x14;                      /* lea eax,[esi+0x14] */
    WB(e + 9, 0);
    WB(e + 0x10, 0x40);                            /* swing range 0x40 = 90deg */
    WB(e + 8, 0x80);
    WW(e + 0xa, 0);
    uint8_t b = G8(VA_g_secondary_door_pool + 0x112);                       /* movzx edx, byte [0x8c0d2] */
    uint16_t id = (uint16_t)(0xef00u | b);         /* or dh,0xef */
    G8(VA_g_secondary_door_pool + 0x112) = (uint8_t)(b + 1);                /* inc [0x8c0d2] */
    WW(e + 6, id);                                 /* [e+6] = unique id */

    uint16_t cc = G16(VA_g_snapshot_filename_buf + 0x5c);
    if (cc != 0) {                                 /* or dx,dx; je 0x3d743 */
        WW(e + 4, (uint16_t)(cc - 1));             /* dec edx; [e+4] = dx */
        WW(e + 0xa, 0x3e8);                        /* [e+0xa] = 1000 (open timer) */
        /* play_world_sound_at_pos(eax=e, edx=cc-1) — direct C (0x27207 lifted in lift_audio.c).
         * EDX at the original call 0x3d73e = dec of word[0x8b3cc] = cc-1 (== the word[e+4] just set). */
        play_world_sound_at_pos(e, (uint32_t)(uint16_t)(cc - 1));
    }
    return slot;                                   /* mov eax,esi */
}

/* ===================== alloc_secondary_door_slot (0x3d7b9) =====================
 * Allocate/initialise a SECONDARY door-pool slot (pool @0x8bfc0, 6 records of stride 0x2c). DI = sector
 * key (FS offset); FS is a register input (caller-set). First scans the pool for an existing record of
 * `di` (word[slot]==di) -> already present, return 0. Else takes the first EMPTY slot (word[slot]==0);
 * none free -> return 0. Initialises the slot directly from the FS sector/portal graph (no setup call):
 *   [slot+0xa]=[0x8b3c8]; [slot]=di; [slot+0x10]=[0x8c0d0]; a signed MIN over the sector's corner-edge
 *   heights (fs:[corner+8] link -> fs:[link+6] -> fs:[edge]) minus 0xa -> [slot+4]; [slot+6]=fs:[di+2];
 *   [slot+8]=fs:[di]; [slot+2]=0; the hinge vertex (fs:[fs:[di+0xe]] indexed into the FLAT vertex base
 *   [0x90aac]) -> [slot+0x16]/[slot+0x18]; [slot+0x28]=[0x8b3ce], [slot+0x2a]=[0x8b3d0]. On the open
 *   sub-record e=slot+0x16: [e+9]=0,[e+0x10]=0x40,[e+8]=0x81,[e+0xa]=0; unique id (word) [e+6]=
 *   0xef00|byte[0x8c0d2] then inc [0x8c0d2].
 *   If sound id [0x8b3cc]!=0: [e+4]=[0x8b3cc]-1, [e+0xa]=0x3e8, play_world_sound_at_pos(eax=e, edx=cc-1)
 *   DIRECT C (gated on 0x7f550). Inc secondary count [0x8bfbc]. Returns the slot ptr. Vertices via the flat
 *   [0x90aac] base (not a GS segment); the leading `add dx,dx` is dead (dx reloaded before use). */
uint32_t alloc_secondary_door_slot(uint16_t di, uint32_t fs)
{
    /* loop1: is `di` already in the secondary pool? */
    uint32_t esi = (uint32_t)GADDR(VA_g_secondary_door_pool);
    for (int n = 6; ; ) {
        if (RW(esi) == di) return 0;               /* cmp [esi],di; je -> sub eax,eax */
        esi += 0x2c;
        if (--n <= 0) break;                       /* dec ecx; jg */
    }
    /* loop2: first empty slot (word[slot]==0) */
    esi = (uint32_t)GADDR(VA_g_secondary_door_pool);
    uint32_t slot = 0; int found = 0;
    for (int n = 6; ; ) {
        if (RW(esi) == 0) { slot = esi; found = 1; break; }   /* cmp word[esi],0; je 0x3d7ea */
        esi += 0x2c;
        if (--n <= 0) break;
    }
    if (!found) return 0;                          /* exhausted -> sub eax,eax */

    WD(slot + 0xa, (uint32_t)G32(VA_g_snapshot_filename_buf + 0x58));        /* [slot+0xa] = [0x8b3c8] */
    WW(slot + 0, di);                              /* [slot] = di */
    WW(slot + 0x10, G16(VA_g_secondary_door_pool + 0x110));                 /* [slot+0x10] = [0x8c0d0] */

    uint8_t  cl  = RB(fs + (uint16_t)(di + 0xd));  /* corner count */
    uint16_t dxv = RW(fs + (uint16_t)(di + 2));    /* fs:[di+2] -> [slot+6] */
    uint16_t bx  = RW(fs + (uint16_t)(di + 0xe));  /* corner array base */
    int16_t  mn  = 0x7fff;
    uint8_t  k   = cl;
    do {                                           /* 0x3d815: min over the sector's edges */
        uint16_t si = RW(fs + (uint16_t)(bx + 8)); /* fs:[corner+8] (edge link) */
        if (si != 0xffff) {                        /* cmp si,-1; je skip */
            si = RW(fs + (uint16_t)(si + 6));      /* fs:[link+6] */
            int16_t v = (int16_t)RW(fs + si);      /* fs:[edge] */
            if (!(mn < v)) mn = v;                 /* cmp ax,fs:[si]; jl skip; else ax = min */
        }
        bx = (uint16_t)(bx + 0xc);                 /* next corner */
        k  = (uint8_t)(k - 1);
    } while ((int8_t)k > 0);                        /* dec cl; jg */
    mn = (int16_t)(mn - 0xa);                       /* sub ax,0xa */

    WW(slot + 6, dxv);                             /* [slot+6] = fs:[di+2] */
    WW(slot + 4, (uint16_t)mn);                    /* [slot+4] = min-0xa */
    WW(slot + 8, RW(fs + di));                     /* [slot+8] = fs:[di] */
    WB(slot + 2, 0);                               /* [slot+2] = 0 */

    /* hinge vertex via the FLAT [0x90aac] base */
    uint16_t cb   = RW(fs + (uint16_t)(RW(slot) + 0xe));   /* bx=[slot]=di; bx=fs:[di+0xe] */
    uint16_t vidx = RW(fs + cb);                            /* bx=fs:[cb] */
    uint32_t vp   = (uint32_t)vidx + (uint32_t)G32(VA_g_sector_geom_base);/* ebx&=0xffff; ebx += [0x90aac] */
    uint32_t e    = slot + 0x16;                            /* lea eax,[esi+0x16] */
    WW(e + 0, RW(vp + 8));                          /* [slot+0x16] = vertex x */
    WW(e + 2, RW(vp + 0xa));                        /* [slot+0x18] = vertex y */
    WW(slot + 0x28, G16(VA_g_snapshot_filename_buf + 0x5e));                 /* [slot+0x28] = [0x8b3ce] */
    WW(slot + 0x2a, G16(VA_g_snapshot_filename_buf + 0x60));                 /* [slot+0x2a] = [0x8b3d0] */
    WB(e + 9, 0);
    WB(e + 0x10, 0x40);                            /* swing range 0x40 */
    WB(e + 8, 0x81);
    WW(e + 0xa, 0);
    uint8_t  b  = G8(VA_g_secondary_door_pool + 0x112);                     /* movzx edx, byte [0x8c0d2] */
    uint16_t id = (uint16_t)(0xef00u | b);         /* or dh,0xef -> dx = 0xefXX */
    G8(VA_g_secondary_door_pool + 0x112) = (uint8_t)(b + 1);                /* inc [0x8c0d2] */
    WW(e + 6, id);                                 /* mov word [eax+6], dx */

    uint16_t cc = G16(VA_g_snapshot_filename_buf + 0x5c);
    if (cc != 0) {                                 /* or dx,dx; je 0x3d8da */
        WW(e + 4, (uint16_t)(cc - 1));             /* dec edx; [e+4] = dx */
        WW(e + 0xa, 0x3e8);                        /* [e+0xa] = 1000 */
        /* play_world_sound_at_pos(eax=e, edx=cc-1) — direct C.
         * EDX at the original call 0x3d8d5 = dec of word[0x8b3cc] = cc-1 (== word[e+4]). */
        play_world_sound_at_pos(e, (uint32_t)(uint16_t)(cc - 1));
    }
    G8(VA_g_secondary_door_count) = (uint8_t)(G8(VA_g_secondary_door_count) + 1);      /* inc [0x8bfbc] (secondary count) */
    return slot;                                   /* mov eax,esi */
}

/* ===================== spawn_door_instance (0x3d586) =====================
 * THE door-spawn orchestrator. AX = sector key; ECX/EBX feed [0x8c0d0]=(ecx?ebx*ecx:ebx) & [0x8b3c4]=ecx;
 * EDX -> [0x8b3c8]. FS = the geom selector ([0x852c8] base, passed as fs); GS for the inner setup =
 * [0x852cc] base (gs). Flow:
 *   1. lookup_door_record_by_sector(sector): if the door already exists -> return 0.
 *   2. chase di = fs:[fs:[sector+8]+6] (the connected sector). If fs:[di+0x14] == -3 -> SECONDARY door:
 *      if the secondary pool ([0x8bfbc]) is full return 0, else alloc_secondary_door_slot(di) and return 0.
 *   3. resolve_door_neighbor_sector(sector) (writes [0x8c0cc]); CF=0 (no neighbour) -> SINGLE door: if the
 *      primary pool ([0x8b3f4]) is full (>=6) return 0, else alloc_door_record(sector) and return the slot.
 *   4. CF=1 (neighbour) -> PAIR: need 2 free primary slots (count<5 else 0). lookup(neighbour sector
 *      [0x8c0cc]); if its door EXISTS -> just alloc_door_record(sector) (the original) and return it. If it
 *      does NOT exist -> inline setup_door_swing_geometry(neighbour slot) [+ [slot+4]=[0x8b3c8]], then
 *      alloc_door_record(sector) for the original door and return THE NEIGHBOUR SLOT (0x3d697 push esi /
 *      0x3d69d pop eax discards alloc's return) — nonzero even if the original door's alloc fails.
 * The per-call edx = fs:[S+6] of the relevant sector S (the body's `mov dx,fs:[S+6]` only sets edx's low16;
 * setup consumes (uint16)edx for record[0x12]); edi = the chased di of S. Bridges out to the lifted leaves;
 * setup writes the FS tag bits, so the oracle compares EAX + obj3 + the FS buffer. */
uint32_t spawn_door_instance(uint32_t eax_ax, uint32_t ecx, uint32_t ebx_in,
                                    uint32_t edx, uint32_t fs, uint32_t gs)
{
    uint16_t sector = (uint16_t)eax_ax;                       /* movzx eax,ax */
    uint32_t ebx = (ecx != 0) ? (ebx_in * ecx) : ebx_in;     /* or ecx,ecx; je; imul ebx,ecx */
    G16(VA_g_secondary_door_pool + 0x110) = (uint16_t)ebx;                             /* [0x8c0d0] = bx */
    G32(VA_g_snapshot_filename_buf + 0x54) = (int32_t)ecx;                              /* [0x8b3c4] = ecx */
    G32(VA_g_snapshot_filename_buf + 0x58) = (int32_t)edx;                              /* [0x8b3c8] = edx */

    if (lookup_door_record_by_sector(sector, fs) != 0) /* call 0x3d4da (loads FS internally) */
        return 0;                                             /* door exists -> 0x3d600 */

    uint16_t di = RW(fs + (uint16_t)((uint16_t)sector + 8));  /* di = fs:[sector+8] */
    di = RW(fs + (uint16_t)(di + 6));                         /* di = fs:[di+6] (connected sector) */
    if ((int16_t)RW(fs + (uint16_t)(di + 0x14)) == -3) {      /* cmp fs:[di+0x14],-3; je secondary */
        if (G8(VA_g_secondary_door_count) >= 6) return 0;                       /* secondary pool full -> 0 */
        alloc_secondary_door_slot(di, fs);            /* call 0x3d7b9 */
        return 0;                                             /* jmp 0x3d600 (ret 0 even on success) */
    }

    if (!resolve_door_neighbor_sector(sector, fs)) {   /* call 0x3d749; CF=0 -> single door */
        if (G8(VA_g_door_count) >= 6) return 0;                       /* primary pool full -> 0 */
        uint16_t edx_s = RW(fs + (uint16_t)(sector + 6));     /* dx = fs:[sector+6] */
        return alloc_door_record(sector, edx_s, di, fs, gs);   /* call 0x3d6c3 -> slot */
    }

    /* neighbour path (0x3d60c): need room for a pair */
    if (G8(VA_g_door_count) >= 5) return 0;                           /* cmp [0x8b3f4],5; jae fail */
    uint16_t nbr = G16(VA_g_secondary_door_pool + 0x10c);                              /* ax = [0x8c0cc] (neighbour sector) */
    if (lookup_door_record_by_sector(nbr, fs) != 0) {  /* neighbour door EXISTS */
        uint16_t di2 = RW(fs + (uint16_t)((uint16_t)sector + 8));
        di2 = RW(fs + (uint16_t)(di2 + 6));                  /* chase di on the original sector */
        uint16_t edx_s = RW(fs + (uint16_t)(sector + 6));
        return alloc_door_record(sector, edx_s, di2, fs, gs);  /* alloc the original door */
    }

    /* neighbour door NEW -> create the pair (0x3d64f) */
    uint16_t di3 = RW(fs + (uint16_t)(nbr + 8));              /* chase di on the neighbour */
    di3 = RW(fs + (uint16_t)(di3 + 6));
    uint32_t slot = (uint32_t)GADDR(VA_g_door_pool) + (uint32_t)G8(VA_g_door_count) * 0x1f6;  /* door_pool[count] */
    uint16_t edx_n = RW(fs + (uint16_t)(nbr + 6));           /* dx = fs:[neighbour+6] */
    if (setup_door_swing_geometry(nbr, slot, edx_n, di3, fs, gs))      /* inline setup (incs count) */
        return 0;                                             /* jb 0x3d6a8 -> ret 0 */
    WD(slot + 4, (uint32_t)G32(VA_g_snapshot_filename_buf + 0x58));                    /* [neighbour_slot+4] = [0x8b3c8] */
    uint16_t di4 = RW(fs + (uint16_t)((uint16_t)sector + 8));
    di4 = RW(fs + (uint16_t)(di4 + 6));                      /* chase di on the original sector */
    uint16_t edx_s = RW(fs + (uint16_t)(sector + 6));
    (void)alloc_door_record(sector, edx_s, di4, fs, gs);  /* call 0x3d6c3 (ret DISCARDED) */
    return slot;   /* 0x3d697 push esi / 0x3d69d pop eax: ret = the NEIGHBOUR slot (nonzero even if
                    * the original door's alloc failed) — cmd_open_door's ret-(-1) bit0 rides this. */
}

/* ===================== register_door_swing (0x3d54b) =====================
 * The parameterised entry to spawn_door_instance: stash the open-door params into the globals
 * alloc_door_record reads ([0x8b3ce] = dx low, [0x8b3d0] = dx high, [0x8b3cc] = di), then run
 * spawn_door_instance with edx zeroed (so [0x8b3c8] = 0 -> the spawned door's [slot+4] = 0). EAX/ECX/EBX
 * thread straight through. */
uint32_t register_door_swing(uint32_t eax_ax, uint32_t ecx, uint32_t ebx_in,
                                    uint32_t edx_params, uint32_t edi_param, uint32_t fs, uint32_t gs)
{
    G16(VA_g_snapshot_filename_buf + 0x5e) = (uint16_t)edx_params;            /* [0x8b3ce] = dx */
    G16(VA_g_snapshot_filename_buf + 0x5c) = (uint16_t)edi_param;             /* [0x8b3cc] = di */
    G16(VA_g_snapshot_filename_buf + 0x60) = (uint16_t)(edx_params >> 16);    /* edx>>=16; [0x8b3d0] = dx */
    return spawn_door_instance(eax_ax, ecx, ebx_in, 0, fs, gs);  /* sub edx,edx; jmp 0x3d586 */
}

/* ===================== dev_open_nearest_door (0x3df96) =====================
 * Dev/debug shortcut (gated on g_dev_flags [0x7f560] bit0): walk the current sector's walls and spawn a
 * door on the FIRST one that is a near, in-front PORTAL. Walks the current sector ([0x90c12]) via FS/GS
 * from [0x90be8]/[0x90bec] (fs_walk/gs_walk): for each wall (count fs:[sec+0xd], array fs:[sec+0xe],
 * stride 0xc): skip if solid (fs:[wall+8]==0xffff); skip if the connected sector fs:[fs:[wall+8]+6]+0x14
 * is < 0xfffd; require in-front (gs:[fs:[wall]+4] > 0 OR gs:[fs:[wall+2]+4] >= 0); require near
 * (point_to_wall_distance_sq(wall, 0x60) <= 0xe10). The first qualifying wall -> spawn_door_instance
 * (eax=wall, ecx=0, ebx=0x258, edx=0) (which loads ITS own FS/GS from [0x852c8]/[0x852cc] = fs_spawn/
 * gs_spawn) and return. Register-transparent (EAX preserved). Returns nothing observable beyond the
 * spawned door's obj3/FS writes. */
void dev_open_nearest_door(uint32_t fs_walk, uint32_t gs_walk,
                                  uint32_t fs_spawn, uint32_t gs_spawn)
{
    if (!(G8(VA_g_dev_mode_flag) & 1)) return;                          /* test [0x7f560],1; je -> ret */
    uint16_t sector = G16(VA_g_player_sector);                          /* ax = [0x90c12] */
    if (sector == 0) return;                                 /* or ax,ax; je done */

    uint8_t  cl  = RB(fs_walk + (uint16_t)(sector + 0xd));   /* wall count */
    uint16_t esi = RW(fs_walk + (uint16_t)(sector + 0xe));   /* wall array offset */
    do {
        uint16_t link = RW(fs_walk + (uint16_t)(esi + 8));   /* fs:[wall+8] */
        if (link != 0xffff) {                                /* cmp bx,-1; je next */
            uint16_t conn = RW(fs_walk + (uint16_t)(link + 6));            /* fs:[link+6] */
            if (RW(fs_walk + (uint16_t)(conn + 0x14)) >= 0xfffd) {         /* cmp fs:[conn+0x14],-3; jb next */
                uint16_t vtxA = RW(fs_walk + esi);                        /* fs:[wall] */
                int front = ((int32_t)RD(gs_walk + (uint16_t)(vtxA + 4)) > 0);  /* gs:[vtxA+4] > 0 -> jg */
                if (!front) {
                    uint16_t vtxB = RW(fs_walk + (uint16_t)(esi + 2));    /* fs:[wall+2] */
                    front = !((int32_t)RD(gs_walk + (uint16_t)(vtxB + 4)) < 0);  /* gs:[vtxB+4] < 0 -> next */
                }
                if (front && point_to_wall_distance_sq(esi, 0x60) <= 0xe10u) {  /* near? */
                    spawn_door_instance(esi, 0, 0x258, 0, fs_spawn, gs_spawn);  /* spawn + done */
                    return;
                }
            }
        }
        esi = (uint16_t)(esi + 0xc);                         /* next wall */
        cl  = (uint8_t)(cl - 1);
    } while ((int8_t)cl > 0);                                 /* dec cl; jg */
}

/* ============================================================================================
 * Layer-D per-frame ticks + the open-toggle (the LAST 4 doors fns). These MUTATE live world
 * geometry (the FS sector buffer) + play sounds every frame, so they are LIVE-SWAP ONLY
 * (ROTH_LIFT=doors); they are NOT in test_doors.c (the round-trip oracle would double-apply the
 * geometry mutation + the pool-compaction). Transcribed strictly
 * from the disasm. The audio callees are DIRECT C calls (lift_audio.c); only the per-door indirect
 * callbacks (door_callback -> stored runtime fn ptr) remain bridged via call_orig.
 * ============================================================================================ */

/* --- audio callees: DIRECT C calls (all lifted+verified in lift_audio.c). The per-door indirect
 *     callbacks below (door_callback) remain call_orig — target is a stored runtime fn ptr. --- */
static void door_restart_open_sound(uint32_t rec)   /* 0x3d8f2 restart_door_open_sound; eax=door rec, void */
{
    restart_door_open_sound(rec);
}
static void door_play_world_sound(uint32_t e)       /* 0x27207 play_world_sound_at_pos; eax=e (pos rec) */
{
    /* Both tick call sites (0x3d9fa / 0x3dbfd) set EDX=param via `sub edx,edx; mov dx,word[eax+6]`,
     * i.e. param = word[e+6] (NOT 0 — the old bridge's edx=0 was masked by the sound gate). */
    play_world_sound_at_pos(e, (uint32_t)RW(e + 6));
}
static void door_stop_sound(uint16_t id)            /* 0x26d8a stop_sound_by_id; eax=sound id (movzx word) */
{
    stop_sound_by_id((uint32_t)id);          /* ret EAX (1/0) ignored by all callers */
}
static void door_play_entity_sound(uint16_t id_m1, uint16_t bx, uint16_t cx)  /* 0x271c4 play_entity_sound */
{
    /* Original sites (0x3da..d / 0x3dd..): EAX=id-1, BX=x, CX=y, EDX=0 (`sub edx,edx`). ret ignored. */
    play_entity_sound((uint32_t)id_m1, 0, (uint32_t)bx, (uint32_t)cx);
}
/* per-door completion callback `call [rec+off]` (pushal/popal-framed in the original => register-
 * transparent). target = the stored runtime fn ptr; eax=rec, edx=mode. fs_sel = the geom selector
 * the tick has loaded at the call site (so a callback that derefs FS sees the game's segment). */
static void door_callback(uint32_t target, uint32_t rec, uint32_t mode, uint16_t fs_sel)
{
    if (target == 0) return;                        /* cmp dword[rec+off],0; je skip */
    regs_t io; memset(&io, 0, sizeof io);
    io.va = target;                                 /* already a runtime fn ptr */
    io.eax = rec; io.edx = mode; io.fs = fs_sel;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    roth_unreachable(target - OBJ_DELTA);          /* stored door runtime callback (code-ptr) — in-game doors tier */
#endif
}

/* ===================== toggle_door_open_state (0x3d93f) =====================
 * EAX (low 16) = the door record's pool offset. rec = 0x8b3f8 + (eax & 0xffff). Runs
 * restart_door_open_sound (0x3d8f2, BRIDGED — the audio flow_succ tail) in BOTH paths: the set-path
 * `call`s it then clears state bit4 (`sub byte[rec+2],4`), the clear-path tail-jumps to 0x3d8ec which
 * re-tests bit4 (clear) and falls straight into 0x3d8f2 — so bridging 0x3d8f2 is equivalent for both.
 * The bit4 test is on the PRE-bridge value; the bit4 clear (set-path only) is applied to the POST-bridge
 * value (restart_door_open_sound itself rewrites rec+2). Returns rec (the original leaves eax=rec). */
uint32_t toggle_door_open_state(uint32_t eax_in)
{
    uint32_t rec = (uint32_t)GADDR(VA_g_door_pool) + (eax_in & 0xffff);   /* and eax,0xffff; add eax,0x8b3f8 */
    int was_set = (RB(rec + 2) & 4) != 0;                          /* test byte[eax+2],4 (pre-bridge) */
    door_restart_open_sound(rec);                                 /* call/tail 0x3d8f2 */
    if (was_set) WB(rec + 2, (uint8_t)(RB(rec + 2) - 4));         /* set-path: sub byte[eax+2],4 (post) */
    return rec;
}

/* ===================== tick_secondary_doors (0x3d98f) =====================
 * Per-frame tick of the SECONDARY (sliding) door pool (pool @0x8bfc0, 6 slots of stride 0x2c, active
 * count @0x8bfbc). FS = the geom selector [0x90be8]; the frame step (edx in) is doubled. For each
 * active slot, advance the slide offset (+8) toward the open/closed limit ([+4]/[+6]) by the step,
 * run the open-dwell timer (+0xe vs +0x10), play/stop the loop sound, fire the per-door completion
 * callback (+0xa), and write the resulting offset back into the geometry word fs:[slot[0]]. On a full
 * close the slot is freed (word[slot]=0, count--). Audio + callbacks bridged. EDX in; void out. */
void tick_secondary_doors(uint32_t edx_in)
{
    uint32_t fs_base = g_os_sel_base ? g_os_sel_base(G16(VA_g_geometry_selector)) : 0;   /* mov fs,[0x90be8] */
    uint16_t fs_sel  = G16(VA_g_geometry_selector);
    uint8_t  cl = G8(VA_g_secondary_door_count);                       /* mov cl,byte[0x8bfbc] (active count) */
    if (cl == 0) return;                             /* or cl,cl; je 0x3dafa */
    uint8_t  ch = 6;                                 /* mov ch,6 (slot budget) */
    uint32_t edi = (uint32_t)GADDR(VA_g_secondary_door_pool);
    uint32_t edx = (uint32_t)edx_in + (uint32_t)edx_in;   /* add edx,edx (doubled step) */

    for (;;) {                                       /* 0x3d9ad loop top */
        if (RW(edi) == 0) goto advance;              /* cmp word[edi],0; je 0x3daef */

        if (RB(edi + 2) & 2) {                       /* test byte[edi+2],2; je 0x3da17 */
            /* --- bit2 set: open-dwell / close-after-timer (0x3d9bd) --- */
            if (RW(edi + 0x10) == 0) goto writeback_deccl;   /* je 0x3daeb */
            uint16_t old = RW(edi + 0xe), sub = (uint16_t)edx;
            WW(edi + 0xe, (uint16_t)(old - sub));    /* sub word[edi+0xe],dx */
            if (old >= sub) goto writeback_deccl;    /* jae 0x3daeb (no borrow) */
            WB(edi + 2, (uint8_t)(RB(edi + 2) + 2)); /* sub byte[edi+2],0xfe (== +2) */
            uint32_t e = edi + 0x16;
            WW(e + 0xa, 0);
            uint16_t dxv = RW(edi + 0x28);
            if (dxv != 0) {                          /* or dx,dx; je 0x3d9ff */
                WW(e + 4, (uint16_t)(dxv - 1));      /* dec edx; mov [eax+4],dx */
                WW(e + 0xa, 0x3e8);                  /* [eax+0xa]=1000 (open timer) */
                door_play_world_sound(e);            /* call 0x27207 (eax=e) */
            }
            door_callback(RD(edi + 0xa), edi, 3, fs_sel);   /* call [edi+0xa] (mode 3) */
            goto writeback_deccl;
        }

        /* --- bit2 clear (0x3da17) --- */
        if (RB(edi + 2) & 4) {                       /* test byte[edi+2],4; jne 0x3da5c */
            /* closing (0x3da5c) */
            uint16_t bx = (uint16_t)(RW(edi + 8) - (uint16_t)edx);   /* mov bx,word[edi+8]; sub bx,dx */
            if (!test_door_query_near_player(bx, edi)) {      /* call 0x3dafb; jae 0x3da76 (CF=0) */
                WW(edi + 8, bx);
                if ((int16_t)RW(edi + 8) > (int16_t)RW(edi + 6)) goto writeback_dadf;  /* jg 0x3dadf */
                /* reached the closed limit -> remove the door */
                if (RW(edi + 0x20) != 0) door_stop_sound(RW(edi + 0x1c));   /* 0x26d8a */
                uint16_t snd = RW(edi + 0x2a);       /* 0x3da94 */
                if ((snd & 0xffff) != 0)             /* and eax,0xffff; je 0x3dab5 */
                    door_play_entity_sound((uint16_t)(snd - 1), RW(edi + 0x16), RW(edi + 0x18));
                WW(edi + 8, RW(edi + 6));            /* ax=word[edi+6]; word[edi+8]=ax */
                uint16_t si = RW(edi);               /* mov si,word[edi] (saved) */
                WW(edi, 0);                          /* word[edi]=0 (free slot) */
                G8(VA_g_secondary_door_count) = (uint8_t)(G8(VA_g_secondary_door_count) - 1);   /* dec byte[0x8bfbc] */
                door_callback(RD(edi + 0xa), edi, 1, fs_sel);   /* call [edi+0xa] (mode 1) */
                WW(fs_base + (uint16_t)si, RW(edi + 8));        /* jmp 0x3dae2: fs:[si]=word[edi+8] */
                goto deccl;
            } else {                                 /* CF=1 (blocked) 0x3da6a */
                WW(edi + 0x10, 1);
                WB(edi + 2, (uint8_t)(RB(edi + 2) - 4));        /* sub byte[edi+2],4 */
                goto writeback_dadf;
            }
        }

        /* opening (0x3da1d) */
        WW(edi + 8, (uint16_t)(RW(edi + 8) + (uint16_t)edx));   /* add word[edi+8],dx */
        if ((int16_t)RW(edi + 8) < (int16_t)RW(edi + 4)) goto writeback_dadf;   /* jl 0x3dadf */
        WW(edi + 8, RW(edi + 4));                    /* clamp to open limit */
        WW(edi + 0xe, RW(edi + 0x10));               /* reset dwell timer */
        WB(edi + 2, (uint8_t)(RB(edi + 2) + 2));     /* add byte[edi+2],2 (-> bit2 dwell mode) */
        if (RW(edi + 0x20) != 0) door_stop_sound(RW(edi + 0x1c));   /* 0x26d8a */
        /* fall to writeback_dadf */

    writeback_dadf:                                  /* 0x3dadf: reload si */
        WW(fs_base + (uint16_t)RW(edi), RW(edi + 8));   /* mov si,word[edi]; fs:[si]=word[edi+8] */
        /* fall to deccl */
    deccl:                                           /* 0x3daeb */
    writeback_deccl:
        if (--cl == 0) return;                       /* dec cl; je 0x3dafa */
    advance:                                         /* 0x3daef */
        edi += 0x2c;
        if ((int8_t)(--ch) <= 0) return;             /* dec ch; jg 0x3d9ad */
    }
}

/* x86 forward string-move (DF=0); es=ds=flat in the host so es:[edi]/[esi] are plain addresses.
 * Pool-compaction step: shift record[k+1] DOWN into record[k]'s slot while PRESERVING record[k]'s
 * self-referential pointers (the original copies only the data fields, skipping the +0x2a..+0x36
 * self-ptr/count region + each sub-struct's +0..+0xc link region, and walks record[k]'s OWN sub-
 * struct link chain via [sub+8]). Mirrors 0x3dd7f..0x3ddb6 cursor-for-cursor. */
static void door_compact_one(uint32_t edi0)
{
    uint32_t edi = edi0;
    uint32_t esi = edi + 0x1f6;                                  /* lea esi,[edi+0x1f6] (next record) */
    memmove((void *)(uintptr_t)edi, (void *)(uintptr_t)esi, 0x28); edi += 0x28; esi += 0x28; /* rep movsd 0xa */
    memmove((void *)(uintptr_t)edi, (void *)(uintptr_t)esi, 0x02); edi += 0x02; esi += 0x02; /* movsw */
    edi += 0xc; esi += 0xc;                                      /* add edi,0xc; add esi,0xc (skip self-ptrs) */
    memmove((void *)(uintptr_t)edi, (void *)(uintptr_t)esi, 0xc0); edi += 0xc0;               /* rep movsd 0x30 */
    int dl = 4;                                                  /* mov dl,4 */
    do {                                                         /* 0x3dd9e inner (4 sub-structs) */
        uint32_t save = edi;                                    /* push edi */
        edi += 0xc;                                             /* add edi,0xc */
        esi = edi + 0x1f6;                                      /* lea esi,[edi+0x1f6] */
        memmove((void *)(uintptr_t)edi, (void *)(uintptr_t)esi, 0x1c);   /* rep movsw 0xe */
        edi = RD(save + 8);                                    /* pop edi; mov edi,[edi+8] (link) */
    } while (--dl > 0);                                         /* dec dl; jg */
}

/* ===================== tick_swinging_doors (0x3db8d) =====================
 * Per-frame tick of the PRIMARY (hinged) door pool (pool @0x8b3f8, stride 0x1f6, count @0x8b3f4). For
 * each door, integrate the swing angle ([+3], signed) toward the open/closed limit by the per-door step
 * (edx_in * [+0xc], or edx_in clamped to <=8 when [+0xc]==0), recompute the rotated quad each step via the
 * lifted update_door_swing / compute_door_quad_bounds / rotate_quad, manage the loop sounds, run the
 * per-door callback (+4), mark the two touched sectors clean/dirty (fs:[sector+0x16] bit0), and on a full
 * close REMOVE the door (pool-compaction shift + count--). Audio + callbacks bridged. EDX in; void out.
 * Faithful goto-per-block transcription (labels named by the canon address). */
void tick_swinging_doors(uint32_t edx_in)
{
    uint8_t cl = G8(VA_g_door_count);                         /* movzx ecx,byte[0x8b3f4] */
    if (cl == 0) return;                             /* or ecx,ecx; je 0x3de30 */
    uint32_t ebx = 0;                                /* sub ebx,ebx (removed-any flag) */
    uint32_t edi = (uint32_t)GADDR(VA_g_door_pool);
    uint32_t fs_base = g_os_sel_base ? g_os_sel_base(G16(VA_g_surface_record_selector)) : 0;   /* mov fs,[0x852c8] */
    uint16_t fs_sel  = G16(VA_g_surface_record_selector);
    int32_t  edx;                                    /* the per-door step */

loop_top:                                            /* 0x3dba3 */
    {
        int32_t mul = (int32_t)RD(edi + 0xc);        /* mov eax,[edi+0xc] */
        if (mul != 0) edx = (int32_t)edx_in * mul;   /* imul edx,eax */
        else          edx = ((uint32_t)edx_in >= 8) ? 8 : edx_in;   /* cmp edx,8; jb keep; mov edx,8 */
    }

    if (RB(edi + 2) & 2) {                           /* test byte[edi+2],2; je 0x3dc1a */
        /* --- bit2 set: open-dwell / close-after-timer (0x3dbc0) --- */
        if (RW(edi + 0xa) == 0) goto L_3ddea;        /* je 0x3ddea */
        uint16_t old = RW(edi + 8), sub = (uint16_t)edx;
        WW(edi + 8, (uint16_t)(old - sub));          /* sub word[edi+8],dx */
        if (old >= sub) goto L_3ddea;                /* jae 0x3ddea (no borrow) */
        WB(edi + 2, (uint8_t)(RB(edi + 2) + 2));     /* sub byte[edi+2],0xfe (== +2) */
        uint32_t e = edi + 0x14;
        WW(e + 0xa, 0);
        uint16_t dxv = RW(edi + 0x26);
        if (dxv != 0) {                              /* or dx,dx; je 0x3dc02 */
            WW(e + 4, (uint16_t)(dxv - 1));          /* dec edx; mov [eax+4],dx */
            WW(e + 0xa, 0x3e8);                      /* open timer = 1000 */
            door_play_world_sound(e);                /* call 0x27207 (eax=e) */
        }
        door_callback(RD(edi + 4), edi, 2, fs_sel);  /* call [edi+4] (mode 2) */
        goto L_3ddea;
    }

    /* --- bit2 clear (0x3dc1a): actively swinging --- */
    if (RW(edi + 0x1e) != 0 && RB(edi + 0x24) != 0x40) {   /* sound-handle mark */
        WB(edi + 0x24, 0x40);
        mark_sound_handle_by_id(RW(edi + 0x1a));        /* 0x26de4 (lifted) */
    }
    if (RB(edi + 2) & 4) goto L_3dced;               /* test byte[edi+2],4; jne 0x3dced */
    if (RB(edi + 2) & 1) goto L_3dcac;               /* test byte[edi+2],1; jne 0x3dcac */

    /* 0x3dc46: opening, positive direction (bit4=0,bit1=0) */
    if (compute_door_quad_bounds(edi)) goto L_3ddd0;   /* call 0x3de36; jb 0x3ddd0 (inside) */
    {
        int32_t a = (int8_t)RB(edi + 3);             /* movsx eax,byte[edi+3] */
        G32(VA_g_snapshot_filename_buf + 0x50) = a;                            /* [0x8b3c0]=eax */
        a += edx;                                    /* add eax,edx */
        WB(edi + 3, (uint8_t)a);
        if (a < 0x40) goto L_3dc94;                  /* cmp eax,0x40; jl 0x3dc94 */
        WB(edi + 3, 0x40);                           /* clamp open */
        goto L_3dc68;
    }

L_3dc68:                                             /* open/close-complete check */
    if (update_door_swing(0, edi)) goto L_3dc9f;   /* call 0x3de31; jb 0x3dc9f (inside->revert) */
    WW(edi + 8, RW(edi + 0xa));                      /* word[edi+8]=word[edi+0xa] (reset dwell) */
    WB(edi + 2, (uint8_t)(RB(edi + 2) + 2));         /* add byte[edi+2],2 (-> bit2 dwell mode) */
    if (RW(edi + 0x1e) != 0) door_stop_sound(RW(edi + 0x1a));   /* 0x26d8a */
    goto L_3dde5;

L_3dc94:                                             /* mid-swing: accept if outside, revert if inside */
    if (!update_door_swing(0, edi)) goto L_3dde5;   /* call 0x3de31; jae 0x3dde5 (outside) */
    /* fall to L_3dc9f */
L_3dc9f:                                             /* revert angle */
    WB(edi + 3, (uint8_t)G32(VA_g_snapshot_filename_buf + 0x50));              /* eax=[0x8b3c0]; byte[edi+3]=al */
    goto L_3ddd0;

L_3dcac:                                             /* 0x3dcac: opening, negative direction (bit4=0,bit1=1) */
    if (compute_door_quad_bounds(edi)) goto L_3ddd0;   /* jb 0x3ddd0 */
    {
        int32_t a = (int8_t)RB(edi + 3);             /* movsx eax,byte[edi+3] */
        G32(VA_g_snapshot_filename_buf + 0x50) = a;
        a -= edx;                                    /* sub eax,edx */
        WB(edi + 3, (uint8_t)a);
        if (a > -0x40) goto L_3dc94;                 /* cmp eax,-0x40; jg 0x3dc94 */
        WB(edi + 3, 0xc0);                           /* clamp open (negative) */
        goto L_3dc68;
    }

L_3dced:                                             /* 0x3dced: bit4 set (closing) */
    if (compute_door_quad_bounds(edi)) goto L_3ddd0;   /* jb 0x3ddd0 */
    if (RB(edi + 2) & 1) goto L_3dcd0;               /* test byte[edi+2],1; jne 0x3dcd0 */
    {
        /* 0x3dcfe: closing from positive (bit4=1,bit1=0) — angle ZERO-extended */
        int32_t a = (uint8_t)RB(edi + 3);            /* sub eax,eax; mov al,byte[edi+3] */
        G32(VA_g_snapshot_filename_buf + 0x50) = a;
        a -= edx;                                    /* sub eax,edx */
        WB(edi + 3, (uint8_t)a);
        if (a > 0) goto L_3dc94;                     /* cmp eax,0; jg 0x3dc94 */
        WB(edi + 3, 0);                              /* clamp closed */
        if (update_door_swing(0, edi)) goto L_3dc9f;   /* call 0x3de31; jb 0x3dc9f */
        goto L_3dd1a;                                /* -> removal (fully closed) */
    }
L_3dcd0:                                             /* 0x3dcd0: closing from negative (bit4=1,bit1=1) */
    {
        int32_t a = (int8_t)RB(edi + 3);             /* movsx eax,byte[edi+3] */
        G32(VA_g_snapshot_filename_buf + 0x50) = a;
        a += edx;                                    /* add eax,edx */
        WB(edi + 3, (uint8_t)a);
        if (a < 0) goto L_3dc94;                     /* js 0x3dc94 (sign set) */
        WB(edi + 3, 0);                              /* clamp closed */
        if (update_door_swing(0, edi)) goto L_3dc9f;   /* call 0x3de31; jb 0x3dc9f */
        goto L_3dd1a;                            /* jmp 0x3dd1a -> removal (fully closed) */
    }

L_3dd1a:                                             /* door fully CLOSED -> remove it */
    if (RW(edi + 0x1e) != 0) door_stop_sound(RW(edi + 0x1a));   /* 0x26d8a */
    {
        uint16_t snd = RW(edi + 0x28);               /* mov ax,word[edi+0x28]; and eax,0xffff */
        if ((snd & 0xffff) != 0)                     /* je 0x3dd4b */
            door_play_entity_sound((uint16_t)(snd - 1), RW(edi + 0x14), RW(edi + 0x16));  /* 0x271c4 */
    }
    door_callback(RD(edi + 4), edi, 0, fs_sel);      /* call [edi+4] (mode 0) */
    {
        uint16_t siA = RW(edi);                      /* fs:[siA+0x16] &= 0xfe */
        WB(fs_base + (uint16_t)(siA + 0x16), (uint8_t)(RB(fs_base + (uint16_t)(siA + 0x16)) & 0xfe));
        uint16_t siB = RW(edi + 0x10);               /* fs:[siB+0x16] &= 0xfe */
        WB(fs_base + (uint16_t)(siB + 0x16), (uint8_t)(RB(fs_base + (uint16_t)(siB + 0x16)) & 0xfe));
    }
    /* pool-compaction: shift the (cl-1) records after this one down by one slot (0x3dd78) */
    if ((int8_t)(cl - 1) > 0) {                      /* dec cl; jle 0x3ddc4 (skip if last) */
        uint8_t shifts = (uint8_t)(cl - 1);
        uint32_t d = edi;
        do {                                         /* 0x3dd7e outer */
            door_compact_one(d);
            d += 0x1f6;
        } while ((int8_t)(--shifts) > 0);            /* dec cl; jg 0x3dd7e */
    }
    G8(VA_g_door_count) = (uint8_t)(G8(VA_g_door_count) - 1);        /* dec byte[0x8b3f4] */
    ebx++;                                           /* inc ebx (removed-any) */
    goto L_3ddf0;                                    /* jmp 0x3ddf0 (do NOT advance edi) */

L_3ddd0:                                             /* sound-off / swing-blocked path */
    if (RB(edi + 0x24) != 0) {                       /* cmp byte[edi+0x24],0; je 0x3dde5 */
        WB(edi + 0x24, 0);
        mark_sound_handle_by_id(RW(edi + 0x1a)); /* 0x26de4 (lifted) */
    }
    /* fall to L_3dde5 */
L_3dde5:                                             /* rotate the quad to screen space then advance */
    rotate_quad(0, (uint8_t *)(uintptr_t)edi);   /* call 0x3ded2 */
    /* fall to L_3ddea */
L_3ddea:
    edi += 0x1f6;                                    /* add edi,0x1f6 (advance to next record) */
    /* fall to L_3ddf0 */
L_3ddf0:
    if (--cl != 0) goto loop_top;                    /* dec cl; jne 0x3dba3 */

    /* 0x3ddf9: if any door was removed, re-tag ALL remaining doors' two sectors dirty */
    if (ebx != 0) {
        uint8_t c2 = G8(VA_g_door_count);                    /* mov cl,byte[0x8b3f4] */
        if (c2 != 0) {                               /* or cl,cl; je 0x3de30 */
            uint32_t p = (uint32_t)GADDR(VA_g_door_pool);
            do {                                     /* 0x3de0e */
                uint16_t sB = RW(p + 0x10);          /* fs:[sB+0x16] |= 1 */
                WB(fs_base + (uint16_t)(sB + 0x16), (uint8_t)(RB(fs_base + (uint16_t)(sB + 0x16)) | 1));
                uint16_t sA = RW(p);                 /* fs:[sA+0x16] |= 1 */
                WB(fs_base + (uint16_t)(sA + 0x16), (uint8_t)(RB(fs_base + (uint16_t)(sA + 0x16)) | 1));
                p += 0x1f6;
            } while ((int8_t)(--c2) > 0);            /* dec cl; jg 0x3de0e */
        }
    }
}

/* ===================== tick_doors_for_frame (0x3d959) =====================
 * Per-frame door driver (called from setup_frame_render_context). If no map is loaded
 * ([0x852c8]==0) do nothing; else run both door pools for this frame's time slice ([0x85324]) via
 * the two lifted ticks. Register-transparent (the original saves/restores ecx/edi/esi/edx/es/fs/ebx);
 * es=ds is flat in the host, fs is resolved inside each tick. ABI_VOID. */
void tick_doors_for_frame(void)
{
    if (G16(VA_g_surface_record_selector) == 0) return;                   /* cmp word[0x852c8],0; je 0x3d986 */
    uint32_t edx = (uint32_t)G32(VA_g_frame_time_scale);           /* mov edx,[0x85324] (frame time scale) */
    tick_secondary_doors(edx);                /* call 0x3d98f */
    tick_swinging_doors(edx);                 /* call 0x3db8d */
}
