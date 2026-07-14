/* lift_collision_physics.c — the wall/sector/object collision & movement-sweep service, lifted to C.
 * Split out of renderer.c (per docs/operating/recomp.md §4.6: every subsystem gets its own TU).
 *
 * collision_physics is a PURE fixed-point geometry service over the map's sector/wall/portal
 * structures: it locates the containing sector for a world position, sweeps a moving circle against
 * walls/objects through portal links, casts rays, and reports hit flags/normals/sector crossings.
 * Consumed by player, entity_ai, raw_command_system, doors, and render_world. No jump table, no
 * flow_succ shared bodies; every remaining function is distinct.
 *
 * ABI from the DISASM (the corpus pseudocode is Borland-on-Watcom and UNRELIABLE). Watcom -3r:
 * args in EAX/EDX/EBX/ECX. A0/A1 — collision routines hand back a HIT FLAG IN CF and sector/
 * position data in non-EAX registers; the corpus drops those. Read the live-at-`ret` register set
 * from the disasm for every callee before trusting a signature.
 *
 * A4 — STORED POINTERS: the geometry section base (0x90aa8), vertex table base (g_sector_geom_base
 * 0x90aac), and any stored hit-object pointer hold RUNTIME addresses already offset at load time —
 * deref RAW, never through the G8/G16/G32 canon macros (those add OBJ_DELTA a second time). The
 * plain collision-state scalars (0x8c0xx/0x8c1xx) and the player/query coords (0x90a8x/0x90a9x) are
 * ordinary obj3 globals — G8/G16/G32 are correct for those.
 *
 * lift-lens: docs/reference/lift/collision_physics.md.
 */
#include "common.h"
#include "engine.h"
#include <string.h>

/* ============================================================ Layer A — distance / clip / scan leaves */

/* test_ray_reached_target (0x405f6, 121 B) — predicate: has the active sweep/ray target been reached?
 * No callees; pure reads of the plain collision-state scalars + the query/player position.
 *
 *   if (!g_collision_target_active 0x8c0f0) return MISS(CF=0).
 *   horizontal Chebyshev (max-norm) distance from the query pos (X 0x90a8e, Y 0x90a96) to the target
 *     (tgtX 0x8c122, tgtY 0x8c12a): dx=|X-tgtX|, dy=|Y-tgtY| (signed-16 differences, abs'd), and
 *     d = max(dx,dy). If d > g_collision_target_radius 0x8c0ec (UNSIGNED, `ja`) return MISS.
 *   vertical overlap: qZ (0x90a92) must satisfy qZ <= tgtZ (0x8c126) AND qZ + g_player_height
 *     (0x8c110) + 0xa >= tgtZ (both `jg`/`jl` are SIGNED-16). Else return MISS.
 *   HIT: g_collision_hit_flags 0x8c1e0 |= 2; g_collision_hit_entity 0x8c0f4 = 0; return CF=1.
 *
 * All adds/subs are 16-bit (the original uses dx/ax word ops; `neg edx`/`neg eax` are 32-bit but only
 * the low word feeds the subsequent word compares — negation mod 2^16 is unaffected by the high half).
 * Returns the hit flag (the original's CF): 1 = reached, 0 = not. */
int roth_test_ray_reached_target(void)
{
    if (G32(VA_g_collision_target_active) == 0)                 /* g_collision_target_active inactive -> clc */
        return 0;

    /* dx = |X - tgtX| as a signed-16 difference, abs'd to a 16-bit magnitude */
    uint16_t dx = (uint16_t)(G16(VA_g_player_x) - G16(VA_g_locate_query_x + 0x2));
    if ((int16_t)dx < 0) dx = (uint16_t)(-(int16_t)dx);
    /* dy = |Y - tgtY| */
    uint16_t dy = (uint16_t)(G16(VA_g_player_y) - G16(VA_g_locate_query_y + 0x2));
    if ((int16_t)dy < 0) dy = (uint16_t)(-(int16_t)dy);

    /* d = max(dx,dy): `cmp ax,dx; jg` keeps dy when dy>dx (signed), else takes dx */
    uint16_t d = ((int16_t)dy > (int16_t)dx) ? dy : dx;

    if (d > G16(VA_g_collision_target_radius))                  /* `ja` — UNSIGNED vs g_collision_target_radius */
        return 0;

    int16_t tgtZ = (int16_t)G16(VA_g_locate_query_z + 0x2);
    int16_t qZ   = (int16_t)G16(VA_g_player_z);
    if (qZ > tgtZ)                         /* `jg` signed — query above the target top */
        return 0;
    int16_t top = (int16_t)(qZ + (int16_t)G16(VA_g_player_height) + 0xa);   /* 16-bit adds */
    if (top < tgtZ)                        /* `jl` signed — query+height below the target */
        return 0;

    G8(VA_g_collision_hit_flags)  = (uint8_t)(G8(VA_g_collision_hit_flags) | 2);   /* g_collision_hit_flags |= 2 (target hit) */
    G32(VA_g_collision_hit_entity) = 0;                            /* g_collision_hit_entity = 0 */
    return 1;                                    /* stc — reached */
}

/* scan_sector_edges_at_y (0x3efb0, 211 B) — point-in-sector test by horizontal-scanline edge crossing.
 * Walks the sector's wall edges; for each edge that straddles the query Y (0x8c12a) it interpolates the
 * edge's X at that scanline (signed 16-bit imul/idiv); tracks the MAX crossing-X in scratch[+4] (init
 * 0x8000) and the MIN crossing-X in scratch[+6] (init 0x7fff). Inside iff minX <= queryX (0x8c122) <=
 * maxX → returns CF=0; outside → CF=1.
 *
 * ABI: ESI = sector record offset into the geometry buffer (es-base 0x90aa8); EBP = caller scratch for
 * the min/max X (the function only writes [ebp+4]/[ebp+6]). Sector record: byte[+0xd]=wall count,
 * word[+0xe]=wall-array offset. Wall record (0xc bytes): word[+0]=vtxA off, word[+2]=vtxB off. Vertex
 * record (via gs-base 0x90aac): word[+8]=X, word[+0xa]=Y. Selector offsets wrap at 16 bits.
 *
 * The position math is 16-bit (word ops); the abs/span registers are touched with 32-bit neg/sub but
 * only their low words ever reach a 16-bit consumer (imul/idiv/cmp), so 16-bit-typed locals are
 * byte-faithful. The edge straddle tests follow the original's je/js/jns branch fabric exactly so the
 * "vertex exactly on the scanline" cases (counted once on vtxA, skipped on vtxB) match.
 *
 * Returns the original's CF: 0 = inside, 1 = outside. */
int scan_sector_edges_at_y(uint32_t sector_off, void *ebp_scratch)
{
    uint8_t *geom  = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);   /* es base: sector + wall records */
    uint8_t *vgeom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_sector_geom_base);   /* gs base: vertex coords */
    uint8_t *bp    = (uint8_t *)ebp_scratch;

    uint8_t  cl = *(volatile uint8_t  *)(uintptr_t)(geom + (uint16_t)(sector_off + 0xd)); /* wall count */
    uint16_t si = *(volatile uint16_t *)(uintptr_t)(geom + (uint16_t)(sector_off + 0xe)); /* wall-array off */

    int16_t qx = (int16_t)G16(VA_g_locate_query_x + 0x2);
    int16_t qy = (int16_t)G16(VA_g_locate_query_y + 0x2);

    int16_t *maxX = (int16_t *)(bp + 4);
    int16_t *minX = (int16_t *)(bp + 6);
    *maxX = (int16_t)0x8000;
    *minX = (int16_t)0x7fff;

    #define VX(off) ((int16_t)*(volatile uint16_t *)(uintptr_t)(vgeom + (uint16_t)((off) + 8)))
    #define VY(off) ((int16_t)*(volatile uint16_t *)(uintptr_t)(vgeom + (uint16_t)((off) + 0xa)))

    for (;;) {
        uint16_t vB = *(volatile uint16_t *)(uintptr_t)(geom + (uint16_t)(si + 2));   /* vtxB offset */
        uint16_t vA = *(volatile uint16_t *)(uintptr_t)(geom + (uint16_t)si);         /* vtxA offset */
        int16_t bY = VY(vB);
        int16_t aY = VY(vA);

        int16_t da = (int16_t)(aY - qy);     /* sub ax, queryY */
        int crossed = 0;
        int16_t crossX = 0;

        if (da == 0) {                       /* je: vtxA sits on the scanline -> use A.X */
            crossX = VX(vA);
            crossed = 1;
        } else if (da > 0) {                 /* A above queryY; need B below (jns skips if B not below) */
            int16_t db = (int16_t)(bY - qy);
            if (db < 0) {
                int16_t yspan = (int16_t)(da - db);              /* aY - bY */
                int16_t num   = (int16_t)(qy - bY);              /* neg eax */
                int16_t xspan = (int16_t)(VX(vA) - VX(vB));      /* A.X - B.X */
                int16_t q = (int16_t)(((int32_t)num * xspan) / yspan);
                crossX = (int16_t)(q + VX(vB));                  /* + B.X */
                crossed = 1;
            }
        } else {                             /* da < 0: A below queryY; need B above (je/js skip else) */
            int16_t db = (int16_t)(bY - qy);
            if (db > 0) {
                int16_t yspan = (int16_t)(db - da);              /* bY - aY */
                int16_t num   = (int16_t)(qy - aY);              /* neg ax */
                int16_t xspan = (int16_t)(VX(vB) - VX(vA));      /* B.X - A.X */
                int16_t q = (int16_t)(((int32_t)num * xspan) / yspan);
                crossX = (int16_t)(q + VX(vA));                  /* + A.X */
                crossed = 1;
            }
        }

        if (crossed) {
            if (crossX >= *maxX) *maxX = crossX;   /* jl skips; else update -> MAX crossing X */
            if (crossX <= *minX) *minX = crossX;   /* jg skips; else update -> MIN crossing X */
        }

        si = (uint16_t)(si + 0xc);
        /* dec cl; jg loop — replicate the flag fabric (continue iff ZF==0 && SF==OF) */
        uint8_t prev = cl;
        cl = (uint8_t)(cl - 1);
        int zf = (cl == 0), sf = (cl & 0x80) != 0, of = (prev == 0x80);
        if (zf || sf != of) break;
    }

    #undef VX
    #undef VY

    if (*minX > qx) return 1;   /* cmp dx,queryX; jg -> outside */
    if (*maxX < qx) return 1;   /* cmp ax,queryX; jl -> outside */
    return 0;                   /* inside */
}

/* scan_portal_walls_near_query (0x3db20, 109 B) — is any PORTAL wall of the sector close to the player?
 * EAX = sector offset, DX = a guard sentinel. Entry: if AX==DX -> CF=1; if sector==0 -> CF=0. Else for
 * each wall (count byte[sector+0xd], array word[sector+0xe], stride 0xc): if the wall's neighbor link
 * fs:[wall+8] != 0xffff (a portal, not solid), compute point_to_wall_distance_sq(wall, radius=0x26); if
 * that squared distance <= 0x310 the player is near a portal -> CF=1. None within range -> CF=0.
 * Read-only (geometry via fs-base 0x90aa8; the distance leaf manages its own gs). Returns CF. */
int scan_portal_walls_near_query(uint32_t eax_sector, uint16_t dx)
{
    if ((uint16_t)eax_sector == dx) return 1;       /* cmp ax,dx; je -> stc */
    uint32_t sec = eax_sector & 0xffff;
    if (sec == 0) return 0;                         /* or eax,eax; je -> clc */

    uint8_t *geom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    uint8_t  cl  = *(volatile uint8_t  *)(uintptr_t)(geom + (uint16_t)(sec + 0xd)); /* wall count */
    uint16_t esi = *(volatile uint16_t *)(uintptr_t)(geom + (uint16_t)(sec + 0xe)); /* wall array off */

    for (;;) {
        uint16_t neighbor = *(volatile uint16_t *)(uintptr_t)(geom + (uint16_t)(esi + 8)); /* fs:[wall+8] */
        if (neighbor != 0xffff) {                                              /* portal, not solid */
            if (point_to_wall_distance_sq(esi, 0x26) <= 0x310u)         /* not (ja 0x310) */
                return 1;                                                      /* stc — near a portal */
        }
        esi = (uint16_t)(esi + 0xc);                /* next wall */
        uint8_t prev = cl; cl = (uint8_t)(cl - 1);  /* dec cl; jg */
        int zf = (cl == 0), sf = (cl & 0x80) != 0, of = (prev == 0x80);
        if (zf || sf != of) break;
    }
    return 0;            /* clc — no nearby portal */
}

/* clip_query_circle_to_edge (0x3fe2d, 929 B) — THE core sweep primitive: clip the moving circle (query
 * at 0x8c122/0x8c12a, target at 0x8c120/0x8c128 in 16.16-ish units) against ONE wall edge
 * A=(0x8c1ef,0x8c1f1) B=(0x8c1f3,0x8c1f5), updating the collision response if this edge yields a closer
 * hit than the running best at [ebp+0x24]. Pure fixed-point; calls the lifted isqrt_fixed/atan2_bearing
 * + a sin/cos table at obj1 0x72080. EBP = caller scratch frame (slots 0xc..0x30; +0x24 is in/out best
 * distance). NO register inputs besides ebp. ALWAYS returns CF=0 (every exit is clc); outputs are the
 * collision-state globals (hit flag 0x8c1e0|=4, new pos 0x8c120/8, portal-continue 0x8c190=0xffff) and
 * [ebp+0x24]. Box bounds 0x8c130/0x8c134; thresholds 0x8c138 (perp d^2) / 0x8c140 (endpoint d^2);
 * scales 0x8c13c / 0x8c144; restore point 0x90ab4/0x90ab8.
 *
 * Faithful to the Watcom 16-in-32 register quirks: word stores truncate to 16 (consumed via movsx/word
 * imul), the DX:AX->eax product-reassembly idiom (xchg/shl16/mov ax,dx), and the shl-by-16 that
 * discards garbage high halves on the special-case response coords. */
int clip_query_circle_to_edge(void *ebp_frame)
{
    uint8_t *fp = (uint8_t *)ebp_frame;
    #define FW(off)   (*(int16_t  *)(fp + (off)))      /* word frame slot (signed) */
    #define FD(off)   (*(int32_t  *)(fp + (off)))      /* dword frame slot */

    const int16_t Ax = (int16_t)G16(VA_g_collision_edge_ax), Ay = (int16_t)G16(VA_g_collision_edge_ay);
    const int16_t Bx = (int16_t)G16(VA_g_collision_edge_bx), By = (int16_t)G16(VA_g_collision_edge_by);
    const int16_t qx = (int16_t)G16(VA_g_locate_query_x + 0x2), qy = (int16_t)G16(VA_g_locate_query_y + 0x2);
    /* box bounds: 0x8c134 is the LOW bound, 0x8c130 is the HIGH bound (separating-axis test) */
    const int16_t boxLo = (int16_t)G16(VA_g_collision_box_min), boxHi = (int16_t)G16(VA_g_collision_box_max);

    /* ---- Phase 1: X separating-axis box rejection (reject if the edge's X range vs query misses) ---- */
    int16_t ax = (int16_t)(Ax - qx);          /* A.x - qx */
    int16_t dx = (int16_t)(-(int16_t)(qx - Bx)); /* B.x - qx */
    if (!(ax > dx)) { int16_t t = ax; ax = dx; dx = t; }   /* ax=max, dx=min */
    if (ax <= boxLo) return 0;    /* jle: edge entirely below boxLo */
    if (dx >= boxHi) return 0;    /* jge: edge entirely above boxHi */
    int16_t xspan = (int16_t)(ax - dx);       /* ecx low: |A.x-B.x|; 0 => vertical edge */

    /* ---- Phase 1b: Y separating-axis box rejection ---- */
    ax = (int16_t)(Ay - qy);
    dx = (int16_t)(By - qy);
    if (!(ax > dx)) { int16_t t = ax; ax = dx; dx = t; }
    if (ax <= boxLo) return 0;
    if (dx >= boxHi) return 0;
    int yspan_zero = (ax == dx);              /* je 0x40018 horizontal edge */

    int32_t respX, respY;                     /* the candidate response position (16.16) -> common tail */

    if (yspan_zero) {
        /* ---- horizontal edge (0x40018): push out along Y by the box high bound ([0x8c130]) ---- */
        int32_t e = boxHi;
        if ((int16_t)(Ax - Bx) >= 0) e = -e;  /* js skips neg */
        e = (int16_t)(e - Ay);                /* sub dx,A.y (16-bit) */
        e = -e;                               /* neg edx -> A.y -/+ boxHi */
        respY = (int32_t)((uint32_t)(uint16_t)e << 16);
        respX = (int32_t)G32(VA_g_locate_query_x);        /* targetX */
    } else if (xspan == 0) {
        /* ---- vertical edge (0x3ffed): push out along X by the box high bound ---- */
        int32_t e = boxHi;
        if ((int16_t)(Ay - By) < 0) e = -e;   /* jns skips neg */
        e = (int16_t)(Ax - e);                /* A.x - (+/-boxHi), low 16 */
        respX = (int32_t)((uint32_t)(uint16_t)e << 16);
        respY = (int32_t)G32(VA_g_locate_query_y);        /* targetY */
    } else {
        /* ---- general case (0x3feb8): perpendicular projection ---- */
        int32_t dyA8 = (int32_t)(int16_t)(Ay - By) << 3;   /* (A.y-B.y)*8 */
        FW(0xc) = (int16_t)dyA8;
        FD(0xe) = (int32_t)((uint32_t)dyA8 * (uint32_t)dyA8);
        int32_t ecx = FD(0xe);
        int32_t dxA8 = (int32_t)(int16_t)(Ax - Bx) << 3;   /* (A.x-B.x)*8 */
        FW(0x12) = (int16_t)dxA8;
        FD(0x14) = (int32_t)((uint32_t)dxA8 * (uint32_t)dxA8);
        ecx += FD(0x14);                                   /* 64*|edge|^2 (denominator) */

        int32_t tY = (int32_t)G32(VA_g_locate_query_y) >> 13;          /* sar targetY,13 */
        FW(0x1a) = (int16_t)(tY - ((int32_t)(uint16_t)Ay << 3));   /* (tY - A.y*8) trunc */
        int32_t prod1 = (int32_t)((int16_t)FW(0x1a) * (int16_t)FW(0x12));  /* 16x16 imul */

        int32_t tX = (int32_t)G32(VA_g_locate_query_x) >> 13;          /* sar targetX,13 */
        FW(0x18) = (int16_t)(tX - ((int32_t)(uint16_t)Ax << 3));
        int32_t prod2 = (int32_t)((int16_t)FW(0x18) * (int16_t)FW(0xc));

        int32_t cross = prod2 - prod1;
        int32_t perp2 = (int32_t)(((int64_t)cross * cross) / (int32_t)ecx);  /* imul eax; idiv ecx */
        if (perp2 >= (int32_t)G32(VA_g_collision_radius_sq)) return 0;   /* jge reject */

        /* projection parameter t = -dot(edge, target-A) */
        int32_t t28 = (int32_t)((int16_t)FW(0x12) * -(int32_t)(int16_t)FW(0x18))
                    + (int32_t)((int16_t)FW(0xc)  * -(int32_t)(int16_t)FW(0x1a));
        FD(0x28) = t28;

        int32_t projX = (int32_t)(((int64_t)(int16_t)FW(0x12) * FD(0x28)) / (int32_t)ecx);
        int32_t projY = (int32_t)(((int64_t)(int16_t)FW(0xc)  * FD(0x28)) / (int32_t)ecx);
        FD(0x30) = (int32_t)((((int32_t)(int16_t)Ay << 3) - projY) << 13);  /* closest pt Y = (A.y*8 - projY)<<13 */
        FD(0x2c) = (int32_t)((((int32_t)(int16_t)Ax << 3) - projX) << 13);  /* closest pt X = (A.x*8 - projX)<<13 */

        /* push-out: radius * dx^2 / edge^2, isqrt, signed by edge component */
        int32_t s = (int32_t)(((int64_t)(int32_t)G32(VA_g_collision_radius_sq + 0x4) * FD(0x14)) / (int32_t)ecx);
        s = (int32_t)isqrt_fixed((uint32_t)s);
        if (!((uint16_t)FW(0x12) & 0x8000)) s = -s;        /* test [ebp+0x12],0x8000; jne skip neg */
        s = (int32_t)(int16_t)s;                           /* cwde */
        s <<= 13;
        FD(0x30) -= s;

        s = (int32_t)(((int64_t)(int32_t)G32(VA_g_collision_radius_sq + 0x4) * FD(0xe)) / (int32_t)ecx);
        s = (int32_t)isqrt_fixed((uint32_t)s);
        if ((uint16_t)FW(0xc) & 0x8000) s = -s;            /* test [ebp+0xc],0x8000; je skip neg */
        s = (int32_t)(int16_t)s;
        s <<= 13;
        FD(0x2c) -= s;

        respX = FD(0x2c);
        respY = FD(0x30);
    }

    /* ---- common tail (0x40041): is the response within the edge segment? compute distance ---- */
    int32_t cMaxX = (int32_t)((uint32_t)(uint16_t)Bx << 16), cMinX = (int32_t)((uint32_t)(uint16_t)Ax << 16);
    if (!(cMaxX > cMinX)) { int32_t t = cMaxX; cMaxX = cMinX; cMinX = t; }  /* ecx=max, ebp=min */
    int in_segment = 1;
    if (cMinX > respX) in_segment = 0;        /* jg out */
    if (cMaxX < respX) in_segment = 0;        /* jl out */
    if (in_segment) {
        int32_t cMaxY = (int32_t)((uint32_t)(uint16_t)By << 16), cMinY = (int32_t)((uint32_t)(uint16_t)Ay << 16);
        if (!(cMaxY > cMinY)) { int32_t t = cMaxY; cMaxY = cMinY; cMinY = t; }
        if (cMinY > respY) in_segment = 0;
        if (cMaxY < respY) in_segment = 0;
    }

    int32_t ddx = (respX - (int32_t)G32(VA_g_collision_restore_x)) >> 10;   /* sar 0xa */
    int32_t ddy = (respY - (int32_t)G32(VA_g_collision_restore_y)) >> 10;
    int32_t dist2 = (int32_t)((uint32_t)(ddx * ddx) + (uint32_t)(ddy * ddy));

    if (!in_segment) {
        int32_t adj = dist2 - 0x1d4c;
        if (adj >= 0 && adj > FD(0x24)) {
            /* ---- slide path (0x4010b): swing around endpoint A if target is within range ---- */
            int32_t ex = (int32_t)(int16_t)(((int32_t)G32(VA_g_locate_query_x) >> 13) - ((int32_t)(uint16_t)Ax << 3));
            int32_t ey = (int32_t)(int16_t)(((int32_t)G32(VA_g_locate_query_y) >> 13) - ((int32_t)(uint16_t)Ay << 3));
            if (ex * ex + ey * ey >= (int32_t)G32(VA_g_collision_corner_radius_sq)) return 0;   /* jge reject */

            uint32_t txi = (uint32_t)G32(VA_g_locate_query_x) >> 15;   /* shr targetX,15 */
            uint32_t tyi = (uint32_t)G32(VA_g_locate_query_y) >> 15;
            int16_t a2x = (int16_t)((uint16_t)Ax + (uint16_t)Ax);   /* A.x*2 (ax = x1) */
            int16_t a2y = (int16_t)((uint16_t)Ay + (uint16_t)Ay);   /* A.y*2 (bx = y1) */
            /* atan2_bearing(x1,y2,y1,x2): regs ax=x1,bx=y1,cx=x2,dx=y2 => (a2x, tyi, a2y, txi) */
            uint32_t bearing = atan2_bearing(a2x, (int16_t)tyi, a2y, (int16_t)txi);
            uint32_t bi = (uint32_t)((bearing + bearing) & 0x3fe);     /* table index */
            int32_t  scale = (int32_t)G32(VA_g_collision_radius);
            int32_t tbl = *(volatile int16_t *)(uintptr_t)(GADDR(VA_g_sincos_table) + bi);
            G32(VA_g_locate_query_x) = (int32_t)((uint32_t)((uint16_t)Ax) << 16) + tbl * scale;
            uint32_t bh = ((bi >> 8) + 1) & 3;             /* inc bh; and bh,3 */
            bi = (bi & 0x00ff) | (bh << 8);
            tbl = *(volatile int16_t *)(uintptr_t)(GADDR(VA_g_sincos_table) + bi);
            G32(VA_g_locate_query_y) = (int32_t)((uint32_t)((uint16_t)Ay) << 16) + tbl * scale;
            G8(VA_g_collision_hit_flags) = (uint8_t)(G8(VA_g_collision_hit_flags) | 4);
            G16(VA_g_collision_portal_continue) = (int16_t)0xffff;
            return 0;
        }
        /* else fall through to record with dist2 */
    }

    /* ---- record collision (0x400de) ---- */
    G8(VA_g_collision_hit_flags) = (uint8_t)(G8(VA_g_collision_hit_flags) | 4);
    G32(VA_g_locate_query_x) = respX;
    G32(VA_g_locate_query_y) = respY;
    if (!((uint32_t)dist2 > 0x100000u)) dist2 = 0x100000;  /* clamp best to >= 0x100000 */
    FD(0x24) = dist2;
    G16(VA_g_collision_portal_continue) = (int16_t)0xffff;
    return 0;

    #undef FW
    #undef FD
}

/* clip_locate_query_to_object (0x3fdb0, 125 B) — clip the moving circle against an OBJECT's 4-edge
 * bounding polygon. EAX = object pointer; EBP = caller scratch frame (threaded to clip). Reads the
 * object's vertex list at *(obj+0x2e)+0x82 (4 vertices, stride 0x10, x@+0 y@+4), packs them as
 * (y<<16)|x into the wrap-around buffer 0x8c17c[0..4] (5th = 1st), stashes ebp at 0x8c1eb and the
 * packed query at 0x8c150, then for each of the 4 edges sets edge B=0x8c1f3 / A=0x8c1ef and calls
 * clip_query_circle_to_edge. clip always returns CF=0 so the `jb` early-out is dead and all 4 edges
 * run; returns CF=0. *(obj+0x2e) is a STORED runtime pointer (deref raw). */
int clip_locate_query_to_object(uint32_t obj_ptr, void *ebp_frame)
{
    G32(VA_g_collision_step_active + 0x9) = (int32_t)(uintptr_t)ebp_frame;        /* save ebp for clip's frame */
    uint8_t *verts = (uint8_t *)(uintptr_t)(*(volatile uint32_t *)(uintptr_t)(obj_ptr + 0x2e) + 0x82);

    /* packed query (Y<<16)|X -> 0x8c150 */
    G32(VA_g_max_climb + 0x8) = (int32_t)(((uint32_t)(uint16_t)G16(VA_g_locate_query_y + 0x2) << 16) | (uint16_t)G16(VA_g_locate_query_x + 0x2));

    uint8_t *buf = (uint8_t *)(uintptr_t)GADDR(VA_g_collision_blocker_object + 0x10);
    uint8_t *p = verts;
    for (int i = 0; i < 4; i++) {                        /* copy 4 vertices packed (y<<16)|x */
        uint16_t vy = *(volatile uint16_t *)(uintptr_t)(p + 4);
        uint16_t vx = *(volatile uint16_t *)(uintptr_t)(p);
        *(volatile uint32_t *)(buf + i * 4) = ((uint32_t)vy << 16) | vx;
        p += 0x10;
    }
    *(volatile uint32_t *)(buf + 0x10) = *(volatile uint32_t *)(buf + 0);   /* wrap: 5th = 1st */

    int edi = 0;
    for (int e = 0; e < 4; e++) {
        G32(VA_g_collision_edge_bx) = *(volatile int32_t *)(buf + edi);        /* edge B = vertex[e] */
        edi += 4;
        G32(VA_g_collision_edge_ax) = *(volatile int32_t *)(buf + edi);        /* edge A = vertex[e+1] */
        if (clip_query_circle_to_edge(ebp_frame)) return 1;   /* jb (dead; clip always CF=0) */
    }
    return 0;
}

/* gather_nearby_doors 0x3f93b (doors subsystem; reads globals only, preserves all GP regs) —
 * lifted, called direct (re-pointed from the call_orig bridge once doors closed) */
static void crw_bridge_gather_doors(void)
{
    gather_nearby_doors();
}

/* collide_ray_walls_recursive (0x3fae0, 720 B) — portal-recursive wall cast. ESI = sector offset,
 * EBP = caller frame. Pushes the sector onto the visited stack (0x8c192 count / 0x8c194[], max 0x1e;
 * already-visited or full => return). For each wall: classify the portal (flags wall+0xa; LINK at
 * wall+8, neighbor sector = es:[LINK+6]); run the SAME box + perpendicular-distance test as
 * clip_query_circle_to_edge (inline, vertices via gs); if the wall is in range and SOLID -> HIT
 * (reset position to the restore point 0x90ab4/8, set g_collision_hit_flags|=1, fire the wall trigger);
 * if a PORTAL -> recurse into the neighbor with a narrowing bounds window ([ebp]/[ebp+2] clamped vs
 * [ebp+0x1e]). Callees: find_record_by_id + fire_wall_object_trigger (lifted), gather_nearby_doors
 * (bridge). Geometry via es-base 0x90aa8 (sectors/walls/portals) + gs-base 0x90aac (vertices). */
void collide_ray_walls_recursive(uint32_t sector, void *ebp_frame)
{
    uint8_t *fp = (uint8_t *)ebp_frame;
    #define FW(o) (*(int16_t *)(fp + (o)))
    #define FD(o) (*(int32_t *)(fp + (o)))
    uint8_t *geom  = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    uint8_t *vgeom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_sector_geom_base);
    #define E8(o)   (*(volatile uint8_t  *)(uintptr_t)(geom + (uint16_t)(o)))
    #define E16(o)  (*(volatile uint16_t *)(uintptr_t)(geom + (uint16_t)(o)))
    #define VX(off) ((int16_t)*(volatile uint16_t *)(uintptr_t)(vgeom + (uint16_t)((off) + 8)))
    #define VY(off) ((int16_t)*(volatile uint16_t *)(uintptr_t)(vgeom + (uint16_t)((off) + 0xa)))

    /* --- visited-sector stack guard --- */
    uint16_t cnt = (uint16_t)G16(VA_g_collision_sector_stack_count);
    if ((int16_t)cnt >= 0x1e) { G16(VA_g_collision_portal_continue) = 0; return; }     /* stack full */
    uint16_t *stk = (uint16_t *)(uintptr_t)GADDR(VA_g_collision_sector_stack);
    uint16_t si16 = (uint16_t)(sector & 0xffff);
    for (uint16_t k = 0; k < cnt; k++)
        if (stk[k] == si16) { G16(VA_g_collision_portal_continue) = 0; return; }       /* already visited */
    stk[cnt] = si16;                                            /* push */
    G16(VA_g_collision_sector_stack_count) = (int16_t)(cnt + 1);

    uint8_t  wcount = E8(si16 + 0xd);                           /* wall count */
    uint16_t wall   = E16(si16 + 0xe);                          /* wall-array offset */

    for (;;) {
        uint16_t vA   = E16(wall);                              /* vtxA offset (wall+0) */
        uint16_t link = E16(wall + 8);                          /* portal link (wall+8) */

        if (link == 0xffff) {
            FW(0x1c) = (int16_t)0xffff;                         /* solid wall */
        } else {
            G8(VA_g_collision_step_active + 0x3) = 0;
            uint8_t dl = E8(wall + 0xa);                        /* wall flags */
            if ((dl & 2) || (dl & 3) == 0) {                   /* 0x3fb51 */
                uint16_t nbr = E16(link + 6);                  /* neighbor sector */
                FW(0x1c) = (int16_t)nbr;
                if (E16(nbr + 0x14) >= 0xfffe) {               /* not (jb) -> consult find_record_by_id */
                    if (find_record_by_id(nbr) != 0)     /* CF=1 -> no neighbor */
                        FW(0x1c) = (int16_t)0xffff;
                }
            } else {                                            /* (dl&3)==1, 0x3fb6f */
                if (!(dl & 0x80)) {
                    FW(0x1c) = (int16_t)0xffff;
                } else {
                    uint16_t nbr = E16(link + 6);
                    FW(0x1c) = (int16_t)nbr;
                    if (E16(nbr + 0x18) != 0) {                /* je 0x3fba0 proceeds when ==0 */
                        G8(VA_g_collision_step_active + 0x3) = 1;
                        /* test dl,3; jne proceed — dl&3==1 here so always proceeds; the
                         * [0x8c1e5]=2 branch (dl&3==0) is unreachable from here */
                    }
                }
            }
        }

        /* --- inline box + perpendicular-distance test (same shape as clip_query_circle_to_edge) --- */
        uint16_t vB = E16(wall + 2);                            /* vtxB offset (wall+2) */
        int reject = 0, special = 0;
        {
            int16_t qx = (int16_t)G16(VA_g_locate_query_x + 0x2);
            int16_t ax = (int16_t)(VX(vA) - qx);
            int16_t dx = (int16_t)(-(int16_t)(qx - VX(vB)));
            if (!(ax > dx)) { int16_t t = ax; ax = dx; dx = t; }
            if (ax <= (int16_t)G16(VA_g_collision_box_min)) reject = 1;        /* X box */
            else if (dx >= (int16_t)G16(VA_g_collision_box_max)) reject = 1;
            else {
                int16_t xspan = (int16_t)(ax - dx);
                int16_t qy = (int16_t)G16(VA_g_locate_query_y + 0x2);
                ax = (int16_t)(VY(vA) - qy);
                dx = (int16_t)(VY(vB) - qy);
                if (!(ax > dx)) { int16_t t = ax; ax = dx; dx = t; }
                if (ax <= (int16_t)G16(VA_g_collision_box_min)) reject = 1;    /* Y box */
                else if (dx >= (int16_t)G16(VA_g_collision_box_max)) reject = 1;
                else if (ax == dx) special = 1;                 /* yspan 0 -> skip perp */
                else if (xspan == 0) special = 1;               /* xspan 0 */
                else {
                    int32_t dyA8 = (int32_t)(int16_t)(VY(vA) - VY(vB)) << 3;
                    FW(0xc) = (int16_t)dyA8;
                    FD(0xe) = (int32_t)((uint32_t)dyA8 * (uint32_t)dyA8);
                    int32_t ecx = FD(0xe);
                    int32_t dxA8 = (int32_t)(int16_t)(VX(vA) - VX(vB)) << 3;
                    FW(0x12) = (int16_t)dxA8;
                    FD(0x14) = (int32_t)((uint32_t)dxA8 * (uint32_t)dxA8);
                    ecx += FD(0x14);
                    int32_t tY = (int32_t)G32(VA_g_locate_query_y) >> 13;
                    FW(0x1a) = (int16_t)(tY - ((int32_t)(uint16_t)VY(vA) << 3));
                    int32_t prod1 = (int32_t)((int16_t)FW(0x1a) * (int16_t)FW(0x12));
                    int32_t tX = (int32_t)G32(VA_g_locate_query_x) >> 13;
                    FW(0x18) = (int16_t)(tX - ((int32_t)(uint16_t)VX(vA) << 3));
                    int32_t prod2 = (int32_t)((int16_t)FW(0x18) * (int16_t)FW(0xc));
                    int32_t cross = prod2 - prod1;
                    int32_t perp2 = (int32_t)(((int64_t)cross * cross) / (int32_t)ecx);
                    if (perp2 >= (int32_t)G32(VA_g_collision_radius_sq)) reject = 1;   /* perp reject */
                }
            }
        }

        if (!reject) {
            (void)special;
            if (FW(0x1c) == (int16_t)0xffff) {
                goto hit;                                       /* no portal -> HIT */
            } else {
                /* --- portal: recurse into the neighbor with bounds clamping --- */
                uint16_t nbr = (uint16_t)FW(0x1c);
                int16_t ndx = (int16_t)E16(nbr + 2);
                int16_t nax = (int16_t)E16(nbr);
                int recurse = 1;
                if (ndx > FW(0x1e)) recurse = 0;                /* 0x3fd32 */
                else if (nax < FW(0x1e)) recurse = 0;
                else {
                    if (!(ndx > FW(0))) ndx = FW(0);            /* dx = (dx>[ebp]) ? dx : [ebp] */
                    if (!(nax < FW(2))) nax = FW(2);            /* ax = (ax<[ebp+2]) ? ax : [ebp+2] */
                    uint16_t lrec = E16(nbr + 0x18);
                    if (lrec != 0) {
                        int16_t bx = FW(0x1e);
                        if (bx > (int16_t)E16(lrec + 8)) {       /* 0x3fd16 */
                            if (G8(VA_g_collision_step_active + 0x3) == 2) recurse = 0;   /* je 0x3fd32 */
                        } else if (bx > (int16_t)E16(lrec + 2)) {/* 0x3fd08 jg 0x3fd31 */
                            recurse = 0;
                        } else if (G8(VA_g_collision_step_active + 0x3) == 1) {           /* je 0x3fd31 */
                            recurse = 0;
                        }
                    }
                }
                if (recurse) {
                    FW(0) = ndx;
                    FW(2) = nax;
                    collide_ray_walls_recursive(nbr, ebp_frame);
                    goto next_wall;                             /* jmp 0x3fd99 */
                }
                /* recurse rejected -> fall to next wall (0x3fd32/0x3fd33 then... no: 0x3fd33 falls to
                 * 0x3fd34 HIT!). The 0x3fd31/0x3fd32 paths pop and fall to 0x3fd34 (hit). */
                goto hit;
            }
        }
        goto next_wall;

    hit:
        {
            G32(VA_g_locate_query_x) = (int32_t)G32(VA_g_collision_restore_x);              /* reset position to restore point */
            G32(VA_g_locate_query_y) = (int32_t)G32(VA_g_collision_restore_y);
            G16(VA_g_collision_portal_continue) = 0;
            G8(VA_g_collision_hit_flags) = (uint8_t)(G8(VA_g_collision_hit_flags) | 1);          /* hit flag |= 1 */
            uint16_t rec = E16(wall + 4);                      /* wall+4 -> record offset */
            if ((E8(rec + 9) & 2) && !((int16_t)G16(VA_g_collision_target_active) > 0)) {  /* trigger gate */
                uint32_t recp = (uint32_t)G32(VA_g_map_geometry_buffer) + rec;
                uint32_t wallp = (uint32_t)G32(VA_g_map_geometry_buffer) + wall;
                G8(VA_g_object_table_dirty) = (uint8_t)(G8(VA_g_object_table_dirty) & 0xfd);
                fire_wall_object_trigger(recp, wallp);
                if (G8(VA_g_object_table_dirty) & 2)
                    crw_bridge_gather_doors();
            }
            return;                                            /* 0x3fd97 pop ecx; ret */
        }

    next_wall:
        wall = (uint16_t)(wall + 0xc);
        wcount = (uint8_t)(wcount - 1);
        if (!((int8_t)wcount > 0)) break;                      /* dec cl; jg */
    }
    G16(VA_g_collision_portal_continue) = 0;                                          /* 0x3fda6 fall-through return */

    #undef FW
    #undef FD
    #undef E8
    #undef E16
    #undef VX
    #undef VY
}

/* portal-narrow + recurse helper for collide_point_walls_recursive — the body shared verbatim by all
 * three edge orientations (0x3f2f0 / 0x3f4bd / 0x3f613). It tests whether the moving point can pass
 * through the portal's vertical span (floor/ceiling vs the frame Z-window [ebp+0x1e] and player height
 * 0x8c110, gated by the span thresholds 0x8c0e8 / 0x90bde and the portal-kind tag 0x8c1e5). If passable
 * it narrows the frame window [ebp]/[ebp+2] and recurses into the neighbour, returning 1 (caller -> next
 * wall). If blocked by geometry it returns 0 and the caller treats the wall as SOLID (computes a
 * response). fp = ebp frame; geom = es-base. Reads the alt-Z scratch 0x8c100 set by the classifier. */
static int crw2_portal_narrow(uint8_t *fp, uint8_t *geom, void *ebp_frame)
{
    #define FW(o)  (*(int16_t *)(fp + (o)))
    #define E16(o) (*(volatile uint16_t *)(uintptr_t)(geom + (uint16_t)(o)))
    uint16_t nbr  = (uint16_t)FW(0x1c);
    int32_t  altz = G32(VA_g_collision_hit_entity + 0xc);
    int16_t  dx   = (altz != 0) ? (int16_t)altz : (int16_t)E16(nbr + 2);   /* floor (or stacked alt-Z) */
    int16_t  ax   = (int16_t)E16(nbr);                                     /* ceiling */

    if ((int16_t)(FW(0x1e) - ax) > (int16_t)G16(VA_g_collision_move_entity + 0xc)) return 0;        /* 0x3f316 */
    int16_t b = (int16_t)(dx - FW(0x1e));
    if (b > (int16_t)G16(VA_g_max_step_height)) return 0;                              /* 0x3f329 */
    b = (int16_t)(b - (int16_t)G16(VA_g_player_height));
    if (b >= 0) return 0;                                                /* 0x3f336 jns */

    if (!(dx > FW(0)))  dx = FW(0);                                       /* 0x3f33c clamp low  */
    if (!(ax < FW(2)))  ax = FW(2);                                       /* 0x3f346 clamp high */
    if ((int16_t)(ax - dx) < (int16_t)G16(VA_g_collision_move_entity + 0xc)) return 0;             /* 0x3f35d */

    uint16_t lrec = E16(nbr + 0x18);                                      /* linked record */
    if (lrec != 0) {
        int16_t l8 = (int16_t)E16(lrec + 8);
        int16_t l2 = (int16_t)E16(lrec + 2);
        if (l8 < l2) goto recurse;                                        /* 0x3f37f jl -> recurse */
        if (G8(VA_g_collision_step_active + 0x3) == 2) goto from_top;                             /* 0x3f388 je 0x3f3d2 */
        {
            int16_t c = (int16_t)(ax - l8);                              /* 0x3f38a */
            if (c < (int16_t)G16(VA_g_collision_move_entity + 0xc)) goto from_bot;               /* 0x3f399 jl 0x3f3c9 */
            c = (int16_t)(l8 - FW(0x1e));                                /* 0x3f3a0 */
            if (c > (int16_t)G16(VA_g_max_step_height)) goto from_bot;               /* 0x3f3ab jg 0x3f3c9 */
            c = (int16_t)(c - (int16_t)G16(VA_g_player_height));                    /* 0x3f3ad */
            if (c >= 0) goto from_bot;                                  /* 0x3f3b4 jns 0x3f3c9 */
            if (l8 > dx) dx = l8;                                        /* 0x3f3bc -> keep larger floor */
            goto recurse;                                                /* 0x3f3c4 */
        }
    from_bot:                                                            /* 0x3f3c9 */
        if (G8(VA_g_collision_step_active + 0x3) == 1) return 0;                                  /* 0x3f3d0 je 0x3f405 blocked */
    from_top:                                                            /* 0x3f3d2 */
        if ((int16_t)(l2 - dx) < (int16_t)G16(VA_g_collision_move_entity + 0xc)) return 0;        /* 0x3f3e1 blocked */
        if (l2 < ax) ax = l2;                                           /* 0x3f3e9 -> keep smaller ceiling */
    }
recurse:                                                                 /* 0x3f3f1 */
    FW(0) = dx;
    FW(2) = ax;
    collide_point_walls_recursive(nbr, ebp_frame);
    return 1;
    #undef FW
    #undef E16
}

/* the point-collision response callback (call dword [0x8c178], 0x3f784) — a handler installed at runtime
 * by find_sector_and_collide. Gated by a non-null pointer AND es:[wall+0xa]&4; ABI eax=es:[wall+6] (word),
 * edx=es:[wall+0xb] (byte), ecx=0. The oracle stages 0x8c178==0 so it never fires there; bridged through
 * the installed (rebased) target for in-game faithfulness. */
static void crw2_invoke_collision_callback(uint8_t *geom, uint16_t wall)
{
    regs_t io; memset(&io, 0, sizeof io);
    io.va  = (uint32_t)G32(VA_g_collision_blocker_object + 0xc);
    io.eax = *(volatile uint16_t *)(uintptr_t)(geom + (uint16_t)(wall + 6));
    io.edx = *(volatile uint8_t  *)(uintptr_t)(geom + (uint16_t)(wall + 0xb));
    io.ecx = 0;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    roth_unreachable(io.va - OBJ_DELTA);   /* installed point-collision callback (code-ptr) — in-game collision tier */
#endif
}

/* collide_point_walls_recursive (0x3f0e0, 2100 B) — the moving-POINT portal sweep: like
 * collide_ray_walls_recursive it walks the sector's walls through portal links (visited-stack guard
 * 0x8c192/0x8c194, max 0x1e), but instead of a binary hit it computes a push-OUT/SLIDE response against
 * each blocking edge (the clip_query_circle_to_edge math: perpendicular projection + isqrt_fixed push-out
 * for the general case, box-bound push-out for axis-aligned edges, atan2/sin-cos-table slide around an
 * endpoint when the response leaves the segment). Portals are recursed into only when the point can pass
 * the portal's vertical span (crw2_portal_narrow). ESI = sector offset, EBP = the caller's shared scratch
 * frame (inherited, recursion reuses it). Geometry via es-base 0x90aa8 (sectors/walls/portals) + gs-base
 * 0x90aac (vertices). Callees: find_record_by_id, isqrt_fixed, atan2_bearing (all lifted) + the installed
 * 0x8c178 callback (bridged, gated). The 0x3f906 restore-point tail is unreachable dead code (0 xrefs),
 * intentionally omitted; the fall-through/early-exit returns write nothing (unlike collide_ray). */
void collide_point_walls_recursive(uint32_t sector, void *ebp_frame)
{
    uint8_t *fp = (uint8_t *)ebp_frame;
    #define FW(o) (*(int16_t *)(fp + (o)))
    #define FD(o) (*(int32_t *)(fp + (o)))
    uint8_t *geom  = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    uint8_t *vgeom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_sector_geom_base);
    #define E8(o)   (*(volatile uint8_t  *)(uintptr_t)(geom + (uint16_t)(o)))
    #define E16(o)  (*(volatile uint16_t *)(uintptr_t)(geom + (uint16_t)(o)))
    #define VX(off) ((int16_t)*(volatile uint16_t *)(uintptr_t)(vgeom + (uint16_t)((off) + 8)))
    #define VY(off) ((int16_t)*(volatile uint16_t *)(uintptr_t)(vgeom + (uint16_t)((off) + 0xa)))

    /* --- visited-sector stack guard (identical to collide_ray; the early-exits just ret, no writes) --- */
    uint16_t cnt = (uint16_t)G16(VA_g_collision_sector_stack_count);
    if ((int16_t)cnt >= 0x1e) return;                       /* 0x3f0eb stack full */
    uint16_t *stk = (uint16_t *)(uintptr_t)GADDR(VA_g_collision_sector_stack);
    uint16_t si16 = (uint16_t)(sector & 0xffff);
    for (uint16_t k = 0; k < cnt; k++)
        if (stk[k] == si16) return;                         /* 0x3f0fe already visited */
    stk[cnt] = si16;
    G16(VA_g_collision_sector_stack_count) = (int16_t)(cnt + 1);

    uint8_t  wcount   = E8(si16 + 0xd);                     /* wall count */
    uint16_t wall     = E16(si16 + 0xe);                    /* wall-array offset */
    uint8_t  flagmask = (uint8_t)G8(VA_g_collision_step_active + 0x2);              /* wall-flag AND mask */

    for (;;) {
        G32(VA_g_collision_hit_entity + 0xc) = 0;                                  /* 0x3f12c reset alt-Z each wall */
        uint16_t vA   = E16(wall);                         /* vtxA offset */
        uint16_t link = E16(wall + 8);                     /* portal link (wall+8) */

        /* --- classify solid / portal-kind ([ebp+0x1c], 0x8c1e5, 0x8c100) --- */
        if (link == 0xffff) {
            FW(0x1c) = (int16_t)0xffff;                    /* solid */
        } else {
            G8(VA_g_collision_step_active + 0x3) = 0;
            uint8_t dl = (uint8_t)(E8(wall + 0xa) & flagmask);   /* and dl,[0x8c1e4] */
            int resolve_dlm0 = 0;
            if (dl == 0) {
                resolve_dlm0 = 1;                          /* 0x3f157 direct neighbour */
            } else if (!(dl & 0x80)) {
                FW(0x1c) = (int16_t)0xffff;                /* 0x3f178 solid */
            } else {
                uint16_t rec = E16(wall + 4);
                int8_t   sc  = (int8_t)E8(si16 + 0xc);
                if ((E8(rec + 8) & 8) && sc < 0) {         /* 0x3f185/0x3f190: stacked-sector alt-Z */
                    G32(VA_g_collision_hit_entity + 0xc) = (int32_t)(uint16_t)
                        (((uint16_t)(uint8_t)(-sc) << 2) + (uint16_t)E16(si16 + 2));
                    resolve_dlm0 = 1;                      /* jmp 0x3f157 */
                } else {                                   /* 0x3f1a7 */
                    uint16_t nbr = E16(link + 6);
                    FW(0x1c) = (int16_t)nbr;
                    if (E16(nbr + 0x18) != 0) {
                        G8(VA_g_collision_step_active + 0x3) = 1;
                        if ((dl & 3) == 0) G8(VA_g_collision_step_active + 0x3) = 2;
                    }
                }
            }
            if (resolve_dlm0) {                            /* 0x3f157 neighbour resolution */
                uint16_t nbr = E16(link + 6);
                FW(0x1c) = (int16_t)nbr;
                if ((uint16_t)E16(nbr + 0x14) >= 0xfffe && find_record_by_id(nbr) != 0)
                    FW(0x1c) = (int16_t)0xffff;            /* no record -> solid */
            }
        }

        /* --- box separating-axis test (0x3f1d3) --- */
        uint16_t vB    = E16(wall + 2);
        int16_t  qx    = (int16_t)G16(VA_g_locate_query_x + 0x2), qy = (int16_t)G16(VA_g_locate_query_y + 0x2);
        int16_t  boxLo = (int16_t)G16(VA_g_collision_box_min), boxHi = (int16_t)G16(VA_g_collision_box_max);

        int16_t xa = (int16_t)(VX(vA) - qx);
        int16_t xb = (int16_t)(VX(vB) - qx);
        if (!(xa > xb)) { int16_t t = xa; xa = xb; xb = t; }
        if (xa <= boxLo) goto next_wall;                   /* 0x3f1fa */
        if (xb >= boxHi) goto next_wall;                   /* 0x3f207 */
        uint16_t xspan = (uint16_t)(xa - xb);

        int16_t ya = (int16_t)(VY(vA) - qy);
        int16_t yb = (int16_t)(VY(vB) - qy);
        if (!(ya > yb)) { int16_t t = ya; ya = yb; yb = t; }
        if (ya <= boxLo) goto next_wall;                   /* 0x3f236 */
        if (yb >= boxHi) goto next_wall;                   /* 0x3f243 */

        int case_horiz = (ya == yb);                       /* 0x3f24c yspan 0 -> horizontal edge */
        int case_vert  = !case_horiz && (xspan == 0);      /* 0x3f255 xspan 0 -> vertical edge */

        int32_t respX = 0, respY = 0, ecx = 0;             /* candidate response (16.16) + perp denom */

        if (!case_horiz && !case_vert) {
            /* --- general perpendicular-projection reject (0x3f25b) — clip's verified perp math --- */
            int16_t Ay = VY(vA), By = VY(vB), Ax = VX(vA), Bx = VX(vB);
            int32_t dyA8 = (int32_t)(int16_t)(Ay - By) << 3;
            FW(0xc) = (int16_t)dyA8;
            FD(0xe) = (int32_t)((uint32_t)dyA8 * (uint32_t)dyA8);
            ecx = FD(0xe);
            int32_t dxA8 = (int32_t)(int16_t)(Ax - Bx) << 3;
            FW(0x12) = (int16_t)dxA8;
            FD(0x14) = (int32_t)((uint32_t)dxA8 * (uint32_t)dxA8);
            ecx += FD(0x14);
            int32_t tY = (int32_t)G32(VA_g_locate_query_y) >> 13;
            FW(0x1a) = (int16_t)(tY - ((int32_t)(uint16_t)Ay << 3));
            int32_t prod1 = (int32_t)((int16_t)FW(0x1a) * (int16_t)FW(0x12));
            int32_t tX = (int32_t)G32(VA_g_locate_query_x) >> 13;
            FW(0x18) = (int16_t)(tX - ((int32_t)(uint16_t)Ax << 3));
            int32_t prod2 = (int32_t)((int16_t)FW(0x18) * (int16_t)FW(0xc));
            int32_t cross = prod2 - prod1;
            int32_t perp2 = (int32_t)(((int64_t)cross * cross) / (int32_t)ecx);
            if (perp2 >= (int32_t)G32(VA_g_collision_radius_sq)) goto next_wall;   /* 0x3f2df perp reject */
        }

        /* --- portal vs solid: recurse through a passable portal, else fall to the solid response --- */
        if (FW(0x1c) != (int16_t)0xffff) {
            if (crw2_portal_narrow(fp, geom, ebp_frame)) goto next_wall;  /* recursed */
            /* portal blocked by floor/ceiling -> treat as solid, compute response */
        }

        /* --- candidate response position per edge orientation --- */
        if (case_horiz) {                                  /* 0x3f72b horizontal: push out along Y */
            int16_t e = boxHi;
            if (!((int16_t)(VX(vA) - VX(vB)) < 0)) e = (int16_t)(-e);
            respY = (int32_t)((uint32_t)(uint16_t)(int16_t)(VY(vA) - e) << 16);
            respX = (int32_t)G32(VA_g_locate_query_x);
            G32(VA_g_max_climb + 0x4) = 0x1d4c;
        } else if (case_vert) {                            /* 0x3f5d5 vertical: push out along X */
            int16_t e = boxHi;
            if ((int16_t)(VY(vA) - VY(vB)) < 0) e = (int16_t)(-e);
            respX = (int32_t)((uint32_t)(uint16_t)(int16_t)(VX(vA) - e) << 16);
            respY = (int32_t)G32(VA_g_locate_query_y);
            G32(VA_g_max_climb + 0x4) = 0x1d4c;
        } else {                                           /* 0x3f408 general: closest point + isqrt push-out */
            int16_t Ay = VY(vA), Ax = VX(vA);
            int32_t t28 = (int32_t)((int16_t)FW(0x12) * -(int32_t)(int16_t)FW(0x18))
                        + (int32_t)((int16_t)FW(0xc)  * -(int32_t)(int16_t)FW(0x1a));
            FD(0x28) = t28;
            int32_t projX = (int32_t)(((int64_t)(int16_t)FW(0x12) * FD(0x28)) / (int32_t)ecx);
            int32_t projY = (int32_t)(((int64_t)(int16_t)FW(0xc)  * FD(0x28)) / (int32_t)ecx);
            FD(0x30) = (int32_t)((((int32_t)(int16_t)Ay << 3) - projY) << 13);
            FD(0x2c) = (int32_t)((((int32_t)(int16_t)Ax << 3) - projX) << 13);
            int32_t s = (int32_t)(((int64_t)(int32_t)G32(VA_g_collision_radius_sq + 0x4) * FD(0x14)) / (int32_t)ecx);
            s = (int32_t)isqrt_fixed((uint32_t)s);
            if (!((uint16_t)FW(0x12) & 0x8000)) s = -s;
            s = (int32_t)(int16_t)s; s <<= 13;
            FD(0x30) -= s;
            s = (int32_t)(((int64_t)(int32_t)G32(VA_g_collision_radius_sq + 0x4) * FD(0xe)) / (int32_t)ecx);
            s = (int32_t)isqrt_fixed((uint32_t)s);
            if ((uint16_t)FW(0xc) & 0x8000) s = -s;
            s = (int32_t)(int16_t)s; s <<= 13;
            FD(0x2c) -= s;
            respX = FD(0x2c);
            respY = FD(0x30);
            G32(VA_g_max_climb + 0x4) = (int32_t)0xe09c;
        }

        /* --- common response tail (0x3f758): hit-flag, optional callback, segment+distance, record --- */
        {
            G8(VA_g_collision_hit_flags) = (uint8_t)(G8(VA_g_collision_hit_flags) | 1);
            if (G32(VA_g_collision_blocker_object + 0xc) != 0 && (E8(wall + 0xa) & 4))
                crw2_invoke_collision_callback(geom, wall);     /* gated off in oracle (0x8c178==0) */

            int32_t cMaxX = (int32_t)((uint32_t)(uint16_t)VX(vB) << 16);
            int32_t cMinX = (int32_t)((uint32_t)(uint16_t)VX(vA) << 16);
            if (!(cMaxX > cMinX)) { int32_t t = cMaxX; cMaxX = cMinX; cMinX = t; }
            int in_seg = 1;
            if (cMinX > respX) in_seg = 0;
            if (cMaxX < respX) in_seg = 0;
            if (in_seg) {
                int32_t cMaxY = (int32_t)((uint32_t)(uint16_t)VY(vB) << 16);
                int32_t cMinY = (int32_t)((uint32_t)(uint16_t)VY(vA) << 16);
                if (!(cMaxY > cMinY)) { int32_t t = cMaxY; cMaxY = cMinY; cMinY = t; }
                if (cMinY > respY) in_seg = 0;
                if (cMaxY < respY) in_seg = 0;
            }

            int32_t ddx  = (respX - (int32_t)G32(VA_g_collision_restore_x)) >> 10;
            int32_t ddy  = (respY - (int32_t)G32(VA_g_collision_restore_y)) >> 10;
            int32_t dist2 = (int32_t)((uint32_t)(ddx * ddx) + (uint32_t)(ddy * ddy));

            if (!in_seg) {
                int32_t adj = dist2 - (int32_t)G32(VA_g_max_climb + 0x4);    /* out-of-segment: only record if not worse */
                if (adj >= 0 && adj > FD(0x24)) {
                    /* --- slide path (0x3f852): swing the target around vtxA via the sin/cos table --- */
                    int32_t ex = (int32_t)(int16_t)(((int32_t)G32(VA_g_locate_query_x) >> 13) - ((int32_t)(uint16_t)VX(vA) << 3));
                    int32_t ey = (int32_t)(int16_t)(((int32_t)G32(VA_g_locate_query_y) >> 13) - ((int32_t)(uint16_t)VY(vA) << 3));
                    if (ex * ex + ey * ey >= (int32_t)G32(VA_g_collision_corner_radius_sq)) goto next_wall;   /* endpoint out of range */
                    uint32_t txi = (uint32_t)G32(VA_g_locate_query_x) >> 15;
                    uint32_t tyi = (uint32_t)G32(VA_g_locate_query_y) >> 15;
                    int16_t  a2x = (int16_t)((uint16_t)VX(vA) + (uint16_t)VX(vA));
                    int16_t  a2y = (int16_t)((uint16_t)VY(vA) + (uint16_t)VY(vA));
                    uint32_t bearing = atan2_bearing(a2x, (int16_t)tyi, a2y, (int16_t)txi);
                    uint32_t bi    = (uint32_t)((bearing + bearing) & 0x3fe);
                    int32_t  scale = (int32_t)G32(VA_g_collision_radius);
                    int32_t  tbl   = *(volatile int16_t *)(uintptr_t)(GADDR(VA_g_sincos_table) + bi);
                    G32(VA_g_locate_query_x) = (int32_t)((uint32_t)((uint16_t)VX(vA)) << 16) + tbl * scale;
                    uint32_t bh = ((bi >> 8) + 1) & 3;
                    bi = (bi & 0x00ff) | (bh << 8);
                    tbl = *(volatile int16_t *)(uintptr_t)(GADDR(VA_g_sincos_table) + bi);
                    G32(VA_g_locate_query_y) = (int32_t)((uint32_t)((uint16_t)VY(vA)) << 16) + tbl * scale;
                    G16(VA_g_collision_portal_continue) = (int16_t)0xffff;
                    goto next_wall;
                }
            }

            /* --- record (0x3f829): commit the response position + best distance --- */
            G32(VA_g_locate_query_x) = respX;
            G32(VA_g_locate_query_y) = respY;
            if (!((uint32_t)dist2 > 0x100000u)) dist2 = 0x100000;   /* clamp best to >= 0x100000 */
            FD(0x24) = dist2;
            G16(VA_g_collision_portal_continue) = (int16_t)0xffff;
        }

    next_wall:
        wall   = (uint16_t)(wall + 0xc);
        wcount = (uint8_t)(wcount - 1);
        if (!((int8_t)wcount > 0)) break;                   /* 0x3f932 dec cl; jg 0x3f126 */
    }

    #undef FW
    #undef FD
    #undef E8
    #undef E16
    #undef VX
    #undef VY
}

/* ============================================================ Layer B — sector search / location */

/* search_sector_from_hint (0x3eceb, 19 B) — point-in-sector test of a single HINT sector.
 * EAX = hint sector offset (masked to 16 bits); 0 = no hint -> miss (CF=1). Otherwise tail into
 * scan_sector_edges_at_y(hint) and return its CF; EAX is preserved as the masked hint offset. EBP =
 * the caller's min/max scratch (threaded into scan). *out_sector receives EAX. Returns CF: 0 = inside
 * the hint sector, 1 = not. */
int search_sector_from_hint(uint32_t hint_eax, void *ebp_scratch, uint32_t *out_sector)
{
    uint32_t sec = hint_eax & 0xffff;
    *out_sector = sec;                              /* pop eax = the masked hint */
    if (sec == 0) return 1;                         /* je 0x3ecfc -> stc */
    return scan_sector_edges_at_y(sec, ebp_scratch);
}

/* search_sector_neighbors (0x3f090, 67 B) — point-in-sector test of the sector's PORTAL neighbors.
 * EAX = sector offset (0 = miss -> CF=1). For each wall: link = es:[wall+8]; if link != 0xffff
 * (a portal), the adjacent sector is es:[link+6] -> scan_sector_edges_at_y(neighbor). First neighbor
 * that contains the point wins: *out_sector = neighbor, CF=0. None -> *out_sector = 0, CF=1. EBP =
 * scratch. No obj3 writes. Wall record 0xc bytes; do-while over the wall count (byte[sector+0xd]). */
int search_sector_neighbors(uint32_t sector_eax, void *ebp_scratch, uint32_t *out_sector)
{
    uint8_t *geom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    uint32_t sec = sector_eax & 0xffff;
    if (sec == 0) { *out_sector = 0; return 1; }            /* je 0x3f0cd -> stc */

    uint8_t  cl  = *(volatile uint8_t  *)(uintptr_t)(geom + (uint16_t)(sec + 0xd)); /* wall count */
    uint16_t edi = *(volatile uint16_t *)(uintptr_t)(geom + (uint16_t)(sec + 0xe)); /* wall array off */

    for (;;) {
        uint16_t link = *(volatile uint16_t *)(uintptr_t)(geom + (uint16_t)(edi + 8)); /* es:[wall+8] */
        if (link != 0xffff) {                                                          /* not a solid wall */
            uint16_t neighbor = *(volatile uint16_t *)(uintptr_t)(geom + (uint16_t)(link + 6)); /* es:[link+6] */
            if (scan_sector_edges_at_y(neighbor, ebp_scratch) == 0) {           /* jae -> inside */
                *out_sector = neighbor;
                return 0;                                                              /* clc */
            }
        }
        edi = (uint16_t)(edi + 0xc);                         /* next wall */
        uint8_t prev = cl; cl = (uint8_t)(cl - 1);           /* dec cl; jg */
        int zf = (cl == 0), sf = (cl & 0x80) != 0, of = (prev == 0x80);
        if (zf || sf != of) break;
    }
    *out_sector = 0;     /* sub ax,ax */
    return 1;            /* stc */
}

/* search_sector_global (0x3edf0, 91 B) — full linear scan over every sector for the one containing the
 * query point. No sector arg. Sector array: es:[4] = first sector offset, es:[first-2] = sector count,
 * stride 0x1a. For each sector: scan_sector_edges_at_y; if inside, record g_best_sector (0x8c1d0) =
 * sector, then test the query Z (0x8c126) against the sector's ceiling es:[sec+0] and floor es:[sec+2]
 * (UNSIGNED ja/jb). First sector whose XY AND Z both contain the point wins (CF=0). If none match Z,
 * the LAST XY-inside sector (g_best_sector) is returned (CF=0); if no sector contains the XY point at
 * all, CF=1, EAX=0. EBP = scratch. Writes g_best_sector. */
int search_sector_global(void *ebp_scratch, uint32_t *out_sector)
{
    uint8_t *geom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    G16(VA_g_collision_sector_stack + 0x3c) = 0;                                                          /* clear g_best_sector */
    uint16_t esi = *(volatile uint16_t *)(uintptr_t)(geom + 4);               /* es:[4] = first sector */
    uint16_t cx  = *(volatile uint16_t *)(uintptr_t)(geom + (uint16_t)(esi - 2)); /* es:[first-2] = count */

    if (cx != 0) {                                                            /* or cx,cx; je -> skip loop */
        for (;;) {
            if (scan_sector_edges_at_y(esi, ebp_scratch) == 0) {       /* jb skips if outside */
                G16(VA_g_collision_sector_stack + 0x3c) = (int16_t)esi;                                  /* record XY-inside sector */
                uint16_t bz   = (uint16_t)G16(VA_g_locate_query_z + 0x2);                       /* query Z */
                uint16_t ceil = *(volatile uint16_t *)(uintptr_t)(geom + esi);              /* ceiling */
                uint16_t flr  = *(volatile uint16_t *)(uintptr_t)(geom + (uint16_t)(esi + 2)); /* floor */
                if (!(bz > ceil) && !(bz < flr)) {                            /* ja/jb -> in Z range */
                    *out_sector = esi;
                    return 0;                                                 /* clc — found */
                }
            }
            esi = (uint16_t)(esi + 0x1a);                                     /* next sector */
            uint16_t prev = cx; cx = (uint16_t)(cx - 1);                      /* dec cx; jg */
            int zf = (cx == 0), sf = (cx & 0x8000) != 0, of = (prev == 0x8000);
            if (zf || sf != of) break;
        }
    }
    uint16_t best = (uint16_t)G16(VA_g_collision_sector_stack + 0x3c);                                   /* mov ax,[0x8c1d0] */
    *out_sector = best;
    return (best != 0) ? 0 : 1;                                              /* jne -> clc; else stc */
}

/* find_query_sector (0x3ec40, 26 B) — the 3-strategy sector locator: hint -> neighbors -> global.
 * EAX = hint sector offset. Tries search_sector_from_hint(hint); if that misses, search_sector_neighbors
 * of the hint; if that misses, search_sector_global. *out_sector = the located sector (0 = none).
 * EBP = scratch. CF is always 0 (the result is in EAX); callers test EAX. */
int find_query_sector(uint32_t hint_eax, void *ebp_scratch, uint32_t *out_sector)
{
    uint32_t s = 0;
    if (search_sector_from_hint(hint_eax, ebp_scratch, &s) == 0) { *out_sector = s; return 0; }
    /* EAX preserved = masked hint -> neighbors(hint) */
    if (search_sector_neighbors(s, ebp_scratch, &s) == 0) { *out_sector = s; return 0; }
    if (search_sector_global(ebp_scratch, &s) == 0) { *out_sector = s; return 0; }
    *out_sector = 0;     /* sub eax,eax */
    return 0;
}

/* locate_sector_at_position (0x3ee4b, 97 B) — public entry: stash the query position, then locate.
 * EAX=X, EDX=Y, EBX=Z, ECX=hint sector. Writes the query block (X<<16 @0x8c120 -> word 0x8c122,
 * Y<<16 @0x8c128 -> 0x8c12a, Z<<16 @0x8c124 -> 0x8c126), loads es/gs from 0x90be8/0x90bec, then runs
 * hint -> neighbors -> global (skipping hint/neighbors when ECX==0). Returns the located sector (EAX &
 * 0xffff; 0 = none). Sets up its own 0x34-byte scratch frame for the scan min/max. */
uint32_t locate_sector_at_position(uint32_t eax_x, uint32_t edx_y, uint32_t ebx_z, uint32_t ecx_hint)
{
    uint8_t scratch[0x34];                          /* sub esp,0x34; mov ebp,esp — scan's [ebp+4/6] */
    uint32_t hint = ecx_hint & 0xffff;              /* and ecx,0xffff */
    G32(VA_g_locate_query_x) = (int32_t)(eax_x << 16);          /* shl eax,16; [0x8c120]=eax  -> X @0x8c122 */
    G32(VA_g_locate_query_y) = (int32_t)(edx_y << 16);          /* Y @0x8c12a */
    G32(VA_g_locate_query_z) = (int32_t)(ebx_z << 16);          /* Z @0x8c126 */

    uint32_t s = 0;
    if (hint != 0) {                                /* or ecx,ecx; je -> global */
        if (search_sector_from_hint(hint, scratch, &s) == 0) return s & 0xffff;  /* jae -> done */
        if (search_sector_neighbors(hint, scratch, &s) == 0) return s & 0xffff;  /* jae -> done */
    }
    search_sector_global(scratch, &s);
    return s & 0xffff;                              /* and eax,0xffff */
}

/* ============================================================ Layer C — collision resolve / dispatch */

/* collide_sector_walls (0x3ef21, 131 B) — seed the recursion Z-window from the sector's floor/ceiling,
 * reset the visited-sector count, then dispatch to the wall walker selected by g_collision_step_active
 * (0x8c1e2): 0 -> the moving-POINT sweep (collide_point), nonzero -> the swept RAY cast (collide_ray).
 * EAX = sector offset (preserved across the call via push/pop -> returned). EBP = the caller's shared
 * frame (threaded into the walker). Seeds frame[+2] = ceiling es:[sector+0], frame[0] = floor es:[sector+2],
 * then if the sector carries a linked record (es:[sector+0x18]) and es:[lrec+8] >= es:[lrec+2], narrows
 * one window edge using the same span/queryZ/player-height test the walkers use (thresholds 0x8c0e8,
 * queryZ 0x8c126, height 0x8c110). Geometry via es-base 0x90aa8. */
uint32_t collide_sector_walls(uint32_t sector, void *ebp_frame)
{
    uint8_t *fp   = (uint8_t *)ebp_frame;
    uint8_t *geom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);
    #define E16(o) (*(volatile uint16_t *)(uintptr_t)(geom + (uint16_t)(o)))
    #define FW(o)  (*(int16_t *)(fp + (o)))

    uint16_t s       = (uint16_t)(sector & 0xffff);
    int16_t  ceiling = (int16_t)E16(s + 0);
    int16_t  floor   = (int16_t)E16(s + 2);
    FW(2) = ceiling;                                   /* [ebp+2] = ceiling */
    FW(0) = floor;                                     /* [ebp]   = floor   */

    uint16_t lrec = E16(s + 0x18);                     /* linked record */
    if (lrec != 0) {
        int16_t a = (int16_t)E16(lrec + 8);
        int16_t d = (int16_t)E16(lrec + 2);
        if (a >= d) {                                  /* 0x3ef56 jl skips */
            if ((int16_t)(ceiling - a) < (int16_t)G16(VA_g_collision_move_entity + 0xc)) {   /* 0x3ef65 jl */
                FW(2) = d;                             /* 0x3ef7f */
            } else {
                int16_t b = (int16_t)((int16_t)(a - (int16_t)G16(VA_g_locate_query_z + 0x2)) - (int16_t)G16(VA_g_player_height));
                if (b >= 0) FW(2) = d;                 /* 0x3ef77 jns */
                else        FW(0) = a;                 /* 0x3ef79 */
            }
        }
    }

    G16(VA_g_collision_sector_stack_count) = 0;                                  /* reset visited count */
    if (G8(VA_g_collision_step_active) == 0)                              /* g_collision_step_active */
        collide_point_walls_recursive(sector, ebp_frame);
    else
        collide_ray_walls_recursive(sector, ebp_frame);
    return sector;                                     /* pop eax -> input sector preserved */

    #undef E16
    #undef FW
}

/* collide_ray_entities (0x4066f, 405 B) — test the query point/circle against the worklist of tracked
 * objects (g_door_worklist 0x8498c: 8-byte entries {dword flag, dword obj-ptr}, count g_collision_entity_count
 * 0x8c12c). For each entry: if flag != 0, clip the query circle against the object's 4-edge polygon
 * (clip_locate_query_to_object) and HIT when it records (g_collision_hit_flags 0x8c1e0 bit2) -> set bit0,
 * clear g_collision_hit_entity 0x8c0f4. If flag == 0, a fixed-point bound test using the per-object size
 * record in g_das_collision_buffer (0x85c50, indexed by obj+4, stride 4): a Chebyshev radius (word[+2]
 * threshold) for normal objects, or an anisotropic [shift|h|w|d] box (obj+7 bit0) — plus a Z-overlap gate
 * (frame[+1e] + 0x8c0e8 vs obj Z 0x8c0a..; 0x8c0d4) — then HIT sets bit1 + g_collision_hit_entity = obj.
 * On a HIT it may fire the object's command chain (begin_object_command_chain + gather_nearby_doors) when
 * obj+9 bit5 set, obj+9 bit0 clear and g_collision_target_active 0x8c0f0 <= 0 — this non-idempotent tail is
 * GATED OFF in the oracle via 0x8c0f0>0 (faithful-by-composition: lifted begin_object_command_chain +
 * bridged gather_nearby_doors). Returns CF (1 = a hit was found; the loop returns on the first hit).
 * EBP = the caller frame ([ebp+0x1e] Z bound). Object ptr + worklist + das buffer are STORED runtime
 * pointers (A4) — deref RAW. */
int collide_ray_entities(void *ebp_frame)
{
    uint8_t *fp = (uint8_t *)ebp_frame;
    #define FW(o) (*(int16_t *)(fp + (o)))
    uint8_t *list   = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_door_worklist);  /* g_door_worklist */
    uint8_t *dasbuf = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_das_collision_buffer);  /* g_das_collision_buffer */
    int32_t  n      = (int32_t)G32(VA_g_collision_entity_count);                          /* g_collision_entity_count */
    uint8_t *edi    = list;

    for (;;) {
        uint32_t flag = *(volatile uint32_t *)(uintptr_t)edi;
        uint8_t *obj  = (uint8_t *)(uintptr_t)*(volatile uint32_t *)(uintptr_t)(edi + 4);
        edi += 8;

        if (flag != 0) {
            /* clip path (0x4068d) — clip the query circle vs the object's polygon */
            clip_locate_query_to_object((uint32_t)(uintptr_t)obj, ebp_frame);
            if (G8(VA_g_collision_hit_flags) & 4) {                              /* clip recorded a hit */
                G8(VA_g_collision_hit_flags) = (uint8_t)(G8(VA_g_collision_hit_flags) | 1);
                G32(VA_g_collision_hit_entity) = 0;
                return 1;
            }
            goto next_entry;
        }

        /* distance path (0x406bc) — |query - obj| in X/Y */
        int16_t objX = *(volatile int16_t *)(uintptr_t)(obj + 0);
        int16_t objY = *(volatile int16_t *)(uintptr_t)(obj + 2);
        int16_t dxv = (int16_t)((int16_t)G16(VA_g_locate_query_x + 0x2) - objX); if (dxv < 0) dxv = (int16_t)(-dxv);
        int16_t ayv = (int16_t)((int16_t)G16(VA_g_locate_query_y + 0x2) - objY); if (ayv < 0) ayv = (int16_t)(-ayv);

        uint8_t  o7 = *(volatile uint8_t *)(uintptr_t)(obj + 7);
        uint8_t *tbl = dasbuf + (uint16_t)((uint16_t)(*(volatile uint16_t *)(uintptr_t)(obj + 4)) << 2);
        int16_t bx_size;

        if (o7 & 1) {
            /* anisotropic [shift|h|w|d] box path (0x40790) */
            if (*(volatile uint32_t *)(uintptr_t)tbl == 0) goto next_entry;
            unsigned cl   = (unsigned)tbl[0] & 31;
            int32_t  v15c = (int32_t)((uint32_t)tbl[1] << cl);
            int32_t  m154 = (int32_t)((uint32_t)tbl[2] << cl);   /* compared vs dxv + STORED at 0x8c154 */
            int32_t  ebxv = (int32_t)((uint32_t)tbl[3] << cl);   /* compared vs ayv */
            if ((uint8_t)((uint8_t)(*(volatile uint8_t *)(uintptr_t)(obj + 6) + 0x20) & 0x40) != 0) {
                int32_t t = m154; m154 = ebxv; ebxv = t;          /* swap [0x8c154] <-> ebx */
            }
            G32(VA_g_max_climb + 0x14) = v15c;
            G32(VA_g_max_climb + 0xc) = m154;
            if ((uint16_t)ayv >= (uint16_t)(ebxv & 0xffff)) goto next_entry;   /* 0x407dd jae */
            if ((uint16_t)dxv >= (uint16_t)(m154 & 0xffff)) goto next_entry;   /* 0x407e2 jae */
            bx_size = (int16_t)v15c;                                            /* ebx = [0x8c15c] */
        } else {
            /* Chebyshev radius path (0x406e4) */
            int16_t cheby = ((int16_t)ayv > (int16_t)dxv) ? ayv : dxv;
            if ((uint16_t)cheby >= *(volatile uint16_t *)(uintptr_t)(tbl + 2)) goto next_entry; /* 0x4066ff jae */
            bx_size = (int16_t)*(volatile uint16_t *)(uintptr_t)(tbl + 0);
        }

        /* common Z-overlap gate (0x40708) */
        int16_t objZ = *(volatile int16_t *)(uintptr_t)(obj + 0xa);
        int16_t az = (int16_t)(FW(0x1e) + (int16_t)G16(VA_g_collision_move_entity + 0xc));
        if (az < objZ) goto next_entry;                                       /* 0x40717 jl */
        if (o7 & 8) bx_size = (int16_t)(-bx_size);                            /* 0x40723 neg */
        bx_size = (int16_t)(bx_size + objZ);
        if ((int16_t)(az - (int16_t)G16(VA_g_secondary_door_pool + 0x114)) > bx_size) goto next_entry; /* 0x4072f jg */

        if (*(volatile uint8_t *)(uintptr_t)(obj + 9) & 1) {                  /* 0x40738 */
            if ((int16_t)G16(VA_g_collision_target_active) == *(volatile int16_t *)(uintptr_t)(obj + 0xc))
                goto next_entry;                                             /* 0x40747 je (same entity) */
        }

        /* HIT (0x4074d) */
        G8(VA_g_collision_hit_flags) = (uint8_t)(G8(VA_g_collision_hit_flags) | 2);
        G32(VA_g_collision_hit_entity) = (int32_t)(uintptr_t)obj;
        {
            uint8_t o9 = *(volatile uint8_t *)(uintptr_t)(obj + 9);
            if ((o9 & 0x20) && !(o9 & 1) && !((int16_t)G16(VA_g_collision_target_active) > 0)) {   /* fire tail (0x40770) */
                G8(VA_g_object_table_dirty) = (uint8_t)(G8(VA_g_object_table_dirty) & 0xfd);
                begin_object_command_chain((uint32_t)(uintptr_t)obj);
                if (G8(VA_g_object_table_dirty) & 2) crw_bridge_gather_doors();
            }
        }
        return 1;

    next_entry:
        n--;                                                                 /* dec ecx */
        if (!(n > 0)) break;                                                 /* jg */
    }
    return 0;
    #undef FW
}

/* Position push-out snap (the 0x403cb convergence shared by the iso 0x40392 and aniso 0x4058c paths).
 * Builds candidate 16.16 query coords newX = objX +/- pushX, newY = objY +/- pushY (sign = which side of
 * the object the query sits on, word view 0x8c122/0x8c12a), then snaps whichever query axis is the FARTHER
 * one out to the object edge (Chebyshev-nearer-axis: the larger |delta| axis is moved) and flags a redraw
 * (0x8c190 = 0xffff). All adds keep only the low 16 (Watcom shl-16 discards the high half). 0x8c120/0x8c128
 * are the 16.16 query X/Y; their word views 0x8c122/0x8c12a alias the high half of the same dword. */
static void rcao_pos_pushout(uint8_t *esi, int32_t pushX, int32_t pushY)
{
    int16_t objX  = *(volatile int16_t *)(uintptr_t)(esi + 0);
    int16_t objY  = *(volatile int16_t *)(uintptr_t)(esi + 2);
    int16_t qXint = (int16_t)G16(VA_g_locate_query_x + 0x2);
    int16_t qYint = (int16_t)G16(VA_g_locate_query_y + 0x2);
    uint16_t nxlo = (uint16_t)((int32_t)objX + ((objX < qXint) ? pushX : -pushX));   /* 0x403b4 / 0x405a7 */
    uint16_t nylo = (uint16_t)((int32_t)objY + ((objY < qYint) ? pushY : -pushY));   /* 0x403cb (add edx,ecx) */
    uint32_t newX = (uint32_t)nxlo << 16;                                            /* 0x403d0 shl eax,16 */
    uint32_t newY = (uint32_t)nylo << 16;                                            /* 0x403cd shl edx,16 */
    int32_t  dX   = (int32_t)((uint32_t)G32(VA_g_locate_query_x) - newX); if (dX < 0) dX = -dX;  /* |qX - newX| */
    int32_t  dY   = (int32_t)((uint32_t)G32(VA_g_locate_query_y) - newY); if (dY < 0) dY = -dY;  /* |qY - newY| */
    if ((uint32_t)dY > (uint32_t)dX) G32(VA_g_locate_query_x) = newX;                            /* 0x403ed ja -> move X */
    else                             G32(VA_g_locate_query_y) = newY;                            /* 0x403ef -> move Y */
    G16(VA_g_collision_portal_continue) = 0xffff;                                                           /* 0x403fc redraw flag */
}

/* resolve_collisions_against_objects (0x401cf, 1063 B) — iterative push-out of the query box/circle out of
 * every tracked object in the worklist (g_door_worklist 0x8498c, count 0x8c12c), up to 4 passes. Each pass
 * resets the pushed flag (0x8c1da) and walks the worklist; per object either clips (flag != 0 -> side-effect
 * push-out via clip_locate_query_to_object, CF always 0 so its collision branch 0x4020b is DEAD) or runs the
 * fixed-point distance/box test (flag == 0): a Chebyshev radius (obj+7 bit0 clear, das word[+2]) or an
 * anisotropic [shift|h|w|d] box (bit0 set), then a Z-window overlap test. On overlap it either softly narrows
 * the caller Z-window ([ebp]/[ebp+2]) with NO retry, or — when the footprint straddles the query — snaps the
 * 16.16 query X or Y (nearer axis) out to the object edge (0x8c120/0x8c128), sets the pushed flag
 * 0x8c1da = 0xff (drives the retry loop) and the redraw flag 0x8c190 = 0xffff. obj+7 bit3 selects the
 * floor/ceiling side; obj+9 bit2 + 0x8c1e6 != 0 fires the contact trigger (fire_tracked_object_trigger)
 * — GATED OFF in the oracle via 0x8c1e6 = 0. The 4 vertical-overlap branches are NOT symmetric in WHERE they
 * set the hit flag (0x8c1e0|2 + hit-obj 0x8c16c): iso bit3=1 and aniso bit3=0 set it at their HIT block; iso
 * bit3=0 and aniso bit3=1 never set it in the overlap block (only the iso position push-out does, at 0x40392).
 * Each branch is transcribed from its own disasm. Returns CF (1 = a position push-out happened on the final
 * pass). EBP = caller frame ([ebp]/[ebp+2] Z-window lo/hi, [ebp+0x1e] ref Z). Worklist / object / das buffer
 * are STORED runtime pointers (A4) — deref RAW. */
int resolve_collisions_against_objects(void *ebp_frame)
{
    uint8_t *fp     = (uint8_t *)ebp_frame;
    uint8_t *dasbuf = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_das_collision_buffer);
    #define FW(o)  (*(int16_t *)(fp + (o)))
    #define OS(o)  (*(volatile int16_t  *)(uintptr_t)(esi + (o)))   /* signed obj word   */
    #define OUW(o) (*(volatile uint16_t *)(uintptr_t)(esi + (o)))   /* unsigned obj word */
    #define OUB(o) (*(volatile uint8_t  *)(uintptr_t)(esi + (o)))   /* obj byte          */

    G8(VA_g_player_sector_cache + 0x6) = 4;                                            /* retry counter (0x401cf) */

    for (;;) {                                                  /* OUTER pass (0x401d6) */
        G8(VA_g_player_sector_cache + 0x4) = 0;                                        /* pushed/collision flag */
        int32_t  n   = (int32_t)G32(VA_g_collision_entity_count);                   /* entry count */
        uint8_t *edi = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_door_worklist);

        for (;;) {                                             /* INNER worklist walk (0x401ee, do-while) */
            uint32_t flag = *(volatile uint32_t *)(uintptr_t)edi;
            uint8_t *esi  = (uint8_t *)(uintptr_t)*(volatile uint32_t *)(uintptr_t)(edi + 4);
            edi += 8;

            int16_t zw_dx = 0, zw_ax = 0;                      /* L_zwrite operands ([ebp]/[ebp+2]) */

            if (flag != 0) {
                /* clip path (0x401fb) — clip does its own push-out via side effects; CF always 0 */
                if (clip_locate_query_to_object((uint32_t)(uintptr_t)esi, ebp_frame)) {
                    /* DEAD (clip CF == 0): 0x4020b */
                    G8(VA_g_player_sector_cache + 0x4) = 1; G8(VA_g_player_sector_cache + 0x6) = 1;
                    goto inner_exit;                            /* stc; jmp 0x405d8 */
                }
                goto next_entry;                                /* 0x40206 jmp 0x405d0 */
            }

            /* distance path (0x40220) — |query - obj| in X/Y (word integer view) */
            {
                int16_t objX = OS(0), objY = OS(2);
                int16_t dxv = (int16_t)((int16_t)G16(VA_g_locate_query_x + 0x2) - objX); if (dxv < 0) dxv = (int16_t)(-dxv);
                int16_t ayv = (int16_t)((int16_t)G16(VA_g_locate_query_y + 0x2) - objY); if (ayv < 0) ayv = (int16_t)(-ayv);
                uint8_t  o7  = OUB(7);
                uint8_t *tbl = dasbuf + (uint16_t)((uint16_t)OUW(4) << 2);

                if (!(o7 & 1)) {
                    /* ============ ISOTROPIC (0x40248) ============ */
                    if (!((int16_t)ayv > (int16_t)dxv)) ayv = dxv;                   /* ax = max(|dy|,|dx|) */
                    int32_t rad = (int32_t)(uint16_t)*(volatile uint16_t *)(uintptr_t)(tbl + 2);
                    if (rad == 0) goto next_entry;                                   /* 0x40263 je */
                    rad += (int32_t)G32(VA_g_collision_box_max);                                    /* + box bound */
                    if ((uint16_t)ayv >= (uint16_t)rad) goto next_entry;             /* 0x40272 jae */
                    G32(VA_g_max_climb + 0xc) = (uint32_t)rad;
                    int16_t das0 = (int16_t)*(volatile uint16_t *)(uintptr_t)(tbl + 0);
                    int16_t whi  = FW(2), wlo = FW(0);
                    int16_t objZ = OS(0xa);
                    int16_t narrow_cz;

                    if (o7 & 8) {
                        /* ---- iso bit3 = 1 (0x40324) ---- */
                        if (objZ <= wlo) goto next_entry;                            /* 0x4032c jle */
                        if ((int16_t)(objZ - das0) >= whi) goto next_entry;          /* 0x40339 jge */
                        int16_t cz = objZ;                                           /* 0x40347 */
                        if (cz >= whi) { narrow_cz = (int16_t)(objZ - das0); goto iso_narrow; } /* 0x4034b jge */
                        /* 0x40350 HIT */
                        G8(VA_g_collision_hit_flags) = (uint8_t)(G8(VA_g_collision_hit_flags) | 2);
                        G32(VA_g_collision_blocker_object) = (uint32_t)(uintptr_t)esi;
                        if ((int16_t)(cz - FW(0x1e)) > (int16_t)G16(VA_g_max_step_height)) { narrow_cz = (int16_t)(objZ - das0); goto iso_narrow; }
                        if ((int16_t)(whi - cz)      < (int16_t)G16(VA_g_collision_move_entity + 0xc)) { narrow_cz = (int16_t)(objZ - das0); goto iso_narrow; }
                        zw_dx = cz; zw_ax = whi; goto zwrite;                        /* [ebp]=objZ, [ebp+2]=whi */
                    } else {
                        /* ---- iso bit3 = 0 (0x40288) ---- */
                        if (objZ >= whi) goto next_entry;                            /* 0x40290 jge */
                        if ((int16_t)(objZ + das0) <= wlo) goto next_entry;          /* 0x4029d jle */
                        if ((OUB(9) & 4) && G8(VA_g_collision_step_active + 0x4) != 0) {                      /* fire gate (0x402a3) */
                            regs_t io; memset(&io, 0, sizeof io);
                            io.eax = (uint32_t)(uintptr_t)esi;
                            fire_tracked_object_trigger(&io);                 /* 0x35260 */
                        }
                        whi = FW(2); wlo = FW(0);                                    /* 0x402b9 re-read */
                        int16_t objTop = (int16_t)(objZ + das0);                     /* 0x402cb */
                        if (objTop >= whi) { narrow_cz = objZ; goto iso_narrow; }    /* 0x402d1 jge */
                        if ((int16_t)(objTop - FW(0x1e)) > (int16_t)G16(VA_g_max_step_height)) { narrow_cz = objZ; goto iso_narrow; } /* 0x402de jg */
                        if ((int16_t)(whi - objTop)     < (int16_t)G16(VA_g_collision_move_entity + 0xc)) { narrow_cz = objZ; goto iso_narrow; } /* 0x402ef jl */
                        zw_dx = objTop; zw_ax = whi; goto zwrite;                    /* [ebp]=objTop, [ebp+2]=whi */
                    }

                iso_narrow:                                                          /* 0x40300 (ax=whi, dx=wlo) */
                    {
                        int16_t cz = narrow_cz;
                        if (cz <= wlo) goto pos_iso;                                 /* 0x40303 jle */
                        int16_t clamped = (cz < whi) ? cz : whi;                     /* 0x4030c jl / mov ecx,whi */
                        if ((int16_t)(clamped - wlo) < (int16_t)G16(VA_g_collision_move_entity + 0xc)) goto pos_iso; /* 0x40319 jl */
                        zw_dx = wlo; zw_ax = clamped; goto zwrite;                   /* narrow ceiling to clamped */
                    }

                pos_iso:                                                             /* 0x40392 */
                    G8(VA_g_collision_hit_flags) = (uint8_t)(G8(VA_g_collision_hit_flags) | 2);
                    G8(VA_g_player_sector_cache + 0x4) = 0xff;
                    rcao_pos_pushout(esi, (int32_t)G32(VA_g_max_climb + 0xc), (int32_t)G32(VA_g_max_climb + 0xc));
                    goto next_entry;

                } else {
                    /* ============ ANISOTROPIC (0x4040a) ============ */
                    if (*(volatile uint32_t *)(uintptr_t)tbl == 0) goto next_entry;  /* 0x4041a je */
                    unsigned cl  = (unsigned)tbl[0] & 31;
                    int32_t v15c = (int32_t)((uint32_t)tbl[1] << cl);
                    int32_t m154 = (int32_t)((uint32_t)tbl[2] << cl) + (int32_t)G32(VA_g_collision_box_max);
                    int32_t m158 = (int32_t)((uint32_t)tbl[3] << cl) + (int32_t)G32(VA_g_collision_box_max);
                    if (((uint8_t)(OUB(6) + 0x20) & 0x40) != 0) { int32_t t = m154; m154 = m158; m158 = t; } /* 0x4045a swap */
                    G32(VA_g_max_climb + 0x14) = (uint32_t)v15c;
                    G32(VA_g_max_climb + 0xc) = (uint32_t)m154;
                    G32(VA_g_max_climb + 0x10) = (uint32_t)m158;
                    if ((uint16_t)ayv >= (uint16_t)m158) goto next_entry;            /* 0x40476 jae */
                    if ((uint16_t)dxv >= (uint16_t)m154) goto next_entry;            /* 0x40483 jae */

                    int16_t whi  = FW(2), wlo = FW(0);
                    int16_t objZ = OS(0xa);
                    int16_t narrow_cz;

                    if (o7 & 8) {
                        /* ---- aniso bit3 = 1 (0x40528) — NO hit flag ---- */
                        if (objZ <= wlo) goto next_entry;                            /* 0x40530 jle */
                        if ((int16_t)((int32_t)objZ - v15c) >= whi) goto next_entry; /* 0x40540 jge */
                        int16_t cz = objZ;                                           /* 0x4054e */
                        if (cz >= whi) { narrow_cz = (int16_t)((int32_t)objZ - v15c); goto aniso_narrow; } /* 0x40555 jge */
                        if ((int16_t)(cz - FW(0x1e)) > (int16_t)G16(VA_g_max_step_height)) { narrow_cz = (int16_t)((int32_t)objZ - v15c); goto aniso_narrow; }
                        if ((int16_t)(whi - cz)      < (int16_t)G16(VA_g_collision_move_entity + 0xc)) { narrow_cz = (int16_t)((int32_t)objZ - v15c); goto aniso_narrow; }
                        zw_dx = cz; zw_ax = whi; goto zwrite;                        /* [ebp]=objZ, [ebp+2]=whi */
                    } else {
                        /* ---- aniso bit3 = 0 (0x40493) ---- */
                        if (objZ >= whi) goto next_entry;                            /* 0x4049b jge */
                        if ((int16_t)((int32_t)objZ + v15c) <= wlo) goto next_entry; /* 0x404ab jle */
                        int16_t objTop = (int16_t)((int32_t)objZ + v15c);            /* 0x404c3 */
                        if (objTop >= whi) { narrow_cz = objZ; goto aniso_narrow; }  /* 0x404cc jge */
                        /* 0x404ce HIT */
                        G8(VA_g_collision_hit_flags) = (uint8_t)(G8(VA_g_collision_hit_flags) | 2);
                        G32(VA_g_collision_blocker_object) = (uint32_t)(uintptr_t)esi;
                        if ((int16_t)(objTop - FW(0x1e)) > (int16_t)G16(VA_g_max_step_height)) { narrow_cz = objZ; goto aniso_narrow; }
                        if ((int16_t)(whi - objTop)     < (int16_t)G16(VA_g_collision_move_entity + 0xc)) { narrow_cz = objZ; goto aniso_narrow; }
                        zw_dx = objTop; zw_ax = whi; goto zwrite;                    /* [ebp]=objTop, [ebp+2]=whi */
                    }

                aniso_narrow:                                                        /* 0x40508 (ax=whi, dx=wlo) */
                    {
                        int16_t cz = narrow_cz;
                        if (cz <= wlo) goto pos_aniso;                               /* 0x4050b jle */
                        int16_t clamped = (cz < whi) ? cz : whi;                     /* 0x40510 jl / mov ecx,whi */
                        if ((int16_t)(clamped - wlo) < (int16_t)G16(VA_g_collision_move_entity + 0xc)) goto pos_aniso; /* 0x4051d jl */
                        zw_dx = wlo; zw_ax = clamped; goto zwrite;
                    }

                pos_aniso:                                                           /* 0x4058c — NO 0x8c1e0 set */
                    G8(VA_g_player_sector_cache + 0x4) = 0xff;
                    rcao_pos_pushout(esi, (int32_t)G32(VA_g_max_climb + 0xc), (int32_t)G32(VA_g_max_climb + 0x10));
                    goto next_entry;
                }
            }

        zwrite:                                                                      /* 0x405c8 */
            FW(0) = zw_dx;
            FW(2) = zw_ax;
            /* fall through */
        next_entry:                                                                  /* 0x405d0 */
            if (--n <= 0) break;                                                      /* dec ecx; jg 0x401ee */
        }

    inner_exit:                                                                      /* 0x405dd */
        if (G8(VA_g_player_sector_cache + 0x4) == 0) return 0;                                              /* clc; ret */
        G8(VA_g_player_sector_cache + 0x6) = (uint8_t)(G8(VA_g_player_sector_cache + 0x6) - 1);                                     /* dec [0x8c1dc] */
        if (G8(VA_g_player_sector_cache + 0x6) != 0) continue;                                              /* jne -> retry pass */
        return 1;                                                                    /* stc; ret */
    }

    #undef FW
    #undef OS
    #undef OUW
    #undef OUB
}

/* resolve_collisions_in_sector (0x3eeb0, 113 B) — the per-sector collision orchestrator. Gather nearby doors
 * once (bridge -> 0x3f93b), then loop: clear the redraw flag (0x8c190), run the wall walker for the sector
 * (collide_sector_walls -> the point or ray recursion), then resolve against the tracked-object worklist.
 * The branch is selected by g_collision_step_active 0x8c1e2: nonzero -> RAY (test_ray_reached_target; if it
 * reports the target reached, or collide_ray_entities finds a hit, bail to the restore tail), zero -> POINT
 * (resolve_collisions_against_objects; on a push, bail). After each pass the iteration budget [ebp+0x20] is
 * decremented (underflow -> restore tail) and 0x8c1e2 is cleared (so passes 2+ are always the POINT branch),
 * then the loop repeats while the walker flagged a redraw (0x8c190 != 0). The restore tail (0x3ef0c) snaps the
 * query position back to the saved entry point (0x8c120/8 = 0x90ab4/0x90ab8). EAX is the sector offset (input,
 * threaded into collide_sector_walls — every callee preserves it); EBP = caller frame. The sole caller
 * (find_sector_and_collide 0x3ec5a) ignores the return (clc; ret), so this returns void. All callees are
 * already lifted (collide_sector_walls, test_ray_reached_target, collide_ray_entities,
 * resolve_collisions_against_objects); gather_nearby_doors is host-bridged. */
void resolve_collisions_in_sector(uint32_t sector, void *ebp_frame)
{
    uint8_t *fp = (uint8_t *)ebp_frame;
    crw_bridge_gather_doors();                                  /* 0x3eeb0 call 0x3f93b */

    for (;;) {                                                  /* loop top 0x3eeb5 */
        G16(VA_g_collision_portal_continue) = 0;                                       /* clear redraw flag */
        collide_sector_walls(sector, ebp_frame);         /* 0x3eebe (EAX=sector preserved) */

        if (G8(VA_g_collision_step_active) != 0) {                                /* 0x3eec3 je 0x3eee5 -> POINT */
            /* RAY branch */
            if (roth_test_ray_reached_target()) goto restore; /* 0x3eed1 jb 0x3ef0c */
            if (G32(VA_g_collision_entity_count) != 0) {                            /* 0x3eed3 je 0x3eef5 */
                if (collide_ray_entities(ebp_frame)) goto restore; /* 0x3eee1 jae skip / 0x3eee3 jmp */
            }
        } else {
            /* POINT branch 0x3eee5 */
            if (G32(VA_g_collision_entity_count) != 0) {                            /* 0x3eee5 je 0x3eef5 */
                if (resolve_collisions_against_objects(ebp_frame)) goto restore; /* 0x3eef3 jb 0x3ef0c */
            }
        }

        if (--*(int32_t *)(fp + 0x20) < 0) goto restore;        /* 0x3eef5 dec [ebp+0x20]; js 0x3ef0c */
        G8(VA_g_collision_step_active) = 0;                                        /* 0x3eefa clear step-active */
        if (G16(VA_g_collision_portal_continue) != 0) continue;                        /* 0x3ef01 cmp; jne 0x3eeb5 (re-run) */
        return;                                                 /* 0x3ef0b ret (normal exit) */
    }

restore:                                                        /* 0x3ef0c — snap query back to the entry point */
    G32(VA_g_locate_query_x) = G32(VA_g_collision_restore_x);
    G32(VA_g_locate_query_y) = G32(VA_g_collision_restore_y);
    /* 0x3ef20 ret */
}

/* ============================================================ Layer D — sweep entries */

/* find_sector_and_collide (0x3ec5a, 100 B) — locate the query's sector, then run the per-sector collision.
 * Stashes the query context (EAX -> g_collision_query_ctx 0x8c178), loads the geometry/vertex selectors
 * (es=g_geometry_selector 0x90be8, gs=0x90bec — D3 segment postcondition: the caller relies on es=geometry
 * afterwards, so the live-swap adapter must write R_ES; the C lift reaches geometry via the linear bases
 * 0x90aa8/0x90aac and does not model es), seeds the Z-window ([ebp]=-32768/[ebp+2]=32767) and the iteration
 * budget ([ebp+0x20]=6). If there is no current sector (g_current_sector 0x90c12 == 0) it does a global scan;
 * otherwise it tries the hint sector, then its portal neighbours, then a global scan — storing the result to
 * 0x90c12 on the neighbour/global paths (the hint-hit path resolves without re-storing). On any successful
 * locate it runs resolve_collisions_in_sector for that sector and returns CF=0; if every scan misses it
 * returns CF=1 (no collision pass). EAX = the query sector hint; EBP = caller frame. All callees lifted. */
int find_sector_and_collide(uint32_t query_ctx, void *ebp_frame)
{
    uint8_t *fp = (uint8_t *)ebp_frame;
    G32(VA_g_collision_blocker_object + 0xc) = query_ctx;                                   /* 0x3ec5a stash query context */
    /* es=[0x90be8], gs=[0x90bec] — segment loads (see header; not modelled in the C lift) */
    *(uint16_t *)(fp + 0)    = 0x8000;                          /* 0x3ec6d Z-window lo = -32768 */
    *(uint16_t *)(fp + 2)    = 0x7fff;                          /* 0x3ec73 Z-window hi =  32767 */
    *(int32_t  *)(fp + 0x20) = 6;                               /* 0x3ec79 iteration budget */

    uint32_t sector = 0;
    uint16_t cur = (uint16_t)G16(VA_g_player_sector);                      /* 0x3ec80 mov ax,[0x90c12] — reuses EAX low16, so
                                                                * THIS (the current sector), not query_ctx, is the
                                                                * hint passed to from_hint/neighbors */
    if (cur == 0) {                                             /* 0x3ec86 no current sector -> global scan */
        if (search_sector_global(ebp_frame, &sector)) return 1;     /* 0x3ec90 jb -> fail (CF=1) */
        G16(VA_g_player_sector) = (uint16_t)sector;                        /* 0x3ec92 */
    } else {                                                    /* 0x3ec9a have a current sector */
        if (!search_sector_from_hint(cur, ebp_frame, &sector)) {
            /* 0x3eca1 jae: hint hit -> resolve WITHOUT re-storing 0x90c12 */
        } else if (!search_sector_neighbors(cur, ebp_frame, &sector)) {
            G16(VA_g_player_sector) = (uint16_t)sector;                    /* 0x3eca8 jae -> 0x3ecb1 store */
        } else if (!search_sector_global(ebp_frame, &sector)) {
            G16(VA_g_player_sector) = (uint16_t)sector;                    /* 0x3ecb1 store */
        } else {
            return 1;                                           /* 0x3ecaf jb -> fail (CF=1) */
        }
    }

    resolve_collisions_in_sector(sector, ebp_frame);     /* 0x3ecb7 */
    return 0;                                                   /* 0x3ecbc clc */
}

/* collide_substep_track_sector (0x3ecc0, 43 B) — one player-move substep: run find_sector_and_collide, then
 * cache the resulting sector in g_player_sector_cache (0x8c1d6) when it changes. EAX = the query context
 * (the caller passes g_player_query 0x8c174); EBP = caller frame. The sole caller (move_player_with_collision
 * 0x3e796) IGNORES the return (the orig reports the sector in EAX + carry=not-found, but the call site just
 * pops and loops), so this returns void. The dead `mov ax,[0x8c1d6]` at 0x3ecd4 (a discarded register load,
 * no memory effect) is omitted. */
void collide_substep_track_sector(uint32_t query_ctx, void *ebp_frame)
{
    if (find_sector_and_collide(query_ctx, ebp_frame)) return;   /* 0x3ecc5 jb -> not found */
    uint16_t sec = (uint16_t)G16(VA_g_player_sector);                             /* 0x3ecc9 current sector */
    if (sec == 0) return;                                              /* 0x3ecd2 je */
    if ((uint16_t)G16(VA_g_player_sector_cache) != sec)                                 /* 0x3ece1 jne (changed) */
        G16(VA_g_player_sector_cache) = sec;                                            /* 0x3ece3 update cache */
}

/* probe_collision_step (0x3eb90, 166 B) — one collision probe substep for sweep_move_with_collision. Builds
 * its OWN 0x34-byte scratch frame (the orig does sub esp,0x34 / mov ebp,esp), seeds frame[+0x1e]=query Z
 * (0x8c126) and frame[+0x24] = the per-step distance budget (clamped max((dx>>10)^2 + (dy>>10)^2, 0x100000),
 * dx/dy = ECX/EDX inputs), clears the fire gate (0x8c1e6=0), then runs the in-sector test
 * (find_sector_and_collide, EAX = g_player_query 0x8c174). If that found no hit (g_collision_hit_flags 0x8c1e0
 * == 0) and we are still in a sector (0x90c12 != 0), it crosses the portal to the next sector via
 * find_query_sector: a hit there (sector != 0) becomes the new current sector and returns immediately;
 * otherwise the portal cross failed -> set 0x8c1e0 bit0 and fall through. The fall-through (also taken on a
 * collision hit) restores the saved query position (0x8c120/0x8c124/0x8c128 <- 0x8c160/0x8c164/0x8c168) when a
 * saved sector exists (0x8c170 != 0), and stores the final sector to 0x90c12. ECX = dx, EDX = dy; reads/writes
 * globals only (the frame is internal). The sole caller (sweep_move_with_collision) branches on 0x8c1e0, not
 * the return -> void. es/gs are saved/restored around the body (the C lift reaches geometry via linear bases). */
void probe_collision_step(int32_t ecx_dx, int32_t edx_dy)
{
    uint8_t frame[0x40];                                        /* 0x34 in the orig; extra margin, internal */
    memset(frame, 0, sizeof frame);
    *(int16_t *)(frame + 0x1e) = (int16_t)G16(VA_g_locate_query_z + 0x2);         /* 0x3eb9e frame[+1e] = query Z */

    int32_t cx = ecx_dx >> 10, dy = edx_dy >> 10;              /* 0x3eba2/0x3eba5 sar ,0xa */
    int32_t sq = (int32_t)((uint32_t)(cx * cx) + (uint32_t)(dy * dy));  /* imul; imul; add */
    if (!((uint32_t)sq > 0x100000u)) sq = 0x100000;            /* 0x3ebb0 cmp; ja keep / else clamp */
    G8(VA_g_collision_step_active + 0x4) = 0;                                            /* 0x3ebbd clear fire gate */
    *(int32_t *)(frame + 0x24) = sq;                           /* 0x3ebc4 frame[+24] = step budget */

    find_sector_and_collide(G32(VA_g_collision_blocker_object + 0x8), frame);       /* 0x3ebce in-sector test */

    uint16_t ax;
    if (G8(VA_g_collision_hit_flags) == 0 && (uint16_t)G16(VA_g_player_sector) != 0) {      /* 0x3ebd5/0x3ebe4 no hit + still in a sector */
        uint32_t newsec = 0;
        find_query_sector((uint16_t)G16(VA_g_player_sector), frame, &newsec);  /* 0x3ebe9 portal cross */
        if ((uint16_t)newsec != 0) {                           /* 0x3ebf1 jne -> store new sector, done */
            G16(VA_g_player_sector) = (uint16_t)newsec;                   /* 0x3ec29 */
            return;
        }
        G8(VA_g_collision_hit_flags) = (uint8_t)(G8(VA_g_collision_hit_flags) | 1);              /* 0x3ebf3 portal cross failed */
    }

    /* 0x3ebfa — restore the saved query position if a saved sector exists, then store the final sector */
    ax = (uint16_t)G16(VA_g_collision_blocker_object + 0x4);                               /* saved sector */
    if (ax != 0) {                                             /* 0x3ec03 je skips the restore */
        G32(VA_g_locate_query_x) = G32(VA_g_max_climb + 0x18);                           /* 0x3ec05 */
        G32(VA_g_locate_query_z) = G32(VA_g_max_climb + 0x1c);                           /* 0x3ec11 */
        G32(VA_g_locate_query_y) = G32(VA_g_max_climb + 0x20);                           /* 0x3ec1d */
    }
    G16(VA_g_player_sector) = ax;                                         /* 0x3ec29 store final sector */
}

/* sweep_move_with_collision (0x3e351, 575 B) — top-level SWEPT-MOVE: move an entity by a velocity vector,
 * substepping (1/2/4/8 sub-moves chosen by magnitude thresholds 0xc/0x18/0x30) and running probe_collision_step
 * per substep until a hit. Register-context (pushal-framed) ABI from the sole caller 0x42997:
 *   EAX = velocity X (16.16), ECX = result struct ptr, EDX = velocity Y, EDI = magnitude (picks the substep
 *   count, g_locate_distance 0x8c0f8), EBP = velocity Z, EBX = entity ptr.
 * The entity's state record is esi = *[ebx] (bail if null). Seeds the saved/query position (0x8c120/0x8c124/
 * 0x8c128 = X/Z/Y 16.16, built from state high-words + entity low-words) + saved sector (0x90c12/0x8c170) +
 * the FIXED collision-dim block (0x8c130..0x8c144, 0x8c0e8/0x8c0d4/0x8c0ec) from the radius g_locate_radius
 * 0x90fdc. Per substep: copy query->saved (0x8c160/64/68/70), set ray mode (0x8c1e2=1), add the (scaled)
 * velocity to the query, call probe; stop on a hit (0x8c1e0 != 0). After the loop (or on a hit) it writes the
 * resolved position back into the entity (ebx+0xa/0xc/0xe = fraction, state esi+0/2/0xa = integer) AND the
 * result struct (dest+0/4/8 = 16.16 X/Y/Z, +0x10/+0x16 = Z low, +0x18 = final sector, +0x1a = hit flags
 * 0x8c1e0, +0x1c = hit entity 0x8c0f4), restores the entity flags (esi+7) and the original sector (0x90c12).
 * Returns void (the caller reads the result struct, not a register). Only callee = probe_collision_step. */
void sweep_move_with_collision(uint32_t velX, uint32_t dest_ptr, uint32_t velY,
                                      uint32_t magnitude, uint32_t velZ, uint32_t entity_ptr)
{
    uint8_t *ebx = (uint8_t *)(uintptr_t)entity_ptr;
    G16(VA_g_collision_portal_continue) = 0;                                          /* 0x3e351 */
    G32(VA_g_collision_hit_entity + 0x4) = magnitude;                                  /* 0x3e35a [0x8c0f8] = edi */
    G32(VA_g_collision_hit_entity + 0x8) = velZ;                                       /* 0x3e360 [0x8c0fc] = ebp */
    G8(VA_g_collision_step_active + 0x2) = 0x81;                                        /* 0x3e366 */

    uint8_t *esi = (uint8_t *)(uintptr_t)*(volatile uint32_t *)(uintptr_t)(ebx + 0);  /* 0x3e36e esi=[ebx] */
    if (esi == 0) return;                                      /* 0x3e372 je 0x3e58e (popal;ret) */

    uint32_t dest = dest_ptr;                                  /* ecx -> the result struct (saved across) */
    uint16_t saved_sector = (uint16_t)G16(VA_g_player_sector);            /* 0x3e392 push eax(sector) — restored at the end */
    G32(VA_g_collision_blocker_object + 0x8) = 0;                                          /* 0x3e37a */
    G32(VA_g_collision_target_active) = (uint16_t)*(volatile uint16_t *)(uintptr_t)(ebx + 0x1a);  /* 0x3e384 entity id */

    /* build the 16.16 query/saved position from state (high) + entity (low) */
    uint32_t pZ = (uint32_t)((uint32_t)*(volatile uint16_t *)(uintptr_t)(esi + 0xa) << 16);  /* 0x3e39c */
    G32(VA_g_max_climb + 0x1c) = pZ;                                         /* 0x3e3a3 saved-Z = high<<16 (no low) */
    pZ |= (uint16_t)*(volatile uint16_t *)(uintptr_t)(ebx + 0xe);                            /* 0x3e3a8 */
    G32(VA_g_locate_query_z) = pZ;                                         /* 0x3e3ac query-Z (full 16.16) */

    uint32_t pX = (uint32_t)((uint32_t)*(volatile uint16_t *)(uintptr_t)(esi + 0) << 16)
                | (uint16_t)*(volatile uint16_t *)(uintptr_t)(ebx + 0xa);                    /* 0x3e3b1 */
    G32(VA_g_collision_restore_x) = pX; G32(VA_g_max_climb + 0x18) = pX; G32(VA_g_locate_query_x) = pX;   /* 0x3e3bb/c0/c5 */

    uint32_t pY = (uint32_t)((uint32_t)*(volatile uint16_t *)(uintptr_t)(esi + 2) << 16)
                | (uint16_t)*(volatile uint16_t *)(uintptr_t)(ebx + 0xc);                    /* 0x3e3ca */
    G32(VA_g_collision_restore_y) = pY; G32(VA_g_max_climb + 0x20) = pY; G32(VA_g_locate_query_y) = pY;   /* 0x3e3d5/da/df */

    uint16_t sector0 = (uint16_t)*(volatile uint16_t *)(uintptr_t)(ebx + 6);                 /* 0x3e3e4 */
    G16(VA_g_player_sector) = sector0; G16(VA_g_collision_blocker_object + 0x4) = sector0;            /* 0x3e3e8/ee */

    /* fixed collision-dim block from the radius (0x90fdc) */
    int32_t r = (int32_t)G32(VA_g_projectile_collision_width);                         /* 0x3e3f4 */
    G32(VA_g_collision_box_max) = (uint32_t)r;                                /* box bound hi (dword) */
    G16(VA_g_collision_move_entity + 0xc) = (uint16_t)r;                                /* span threshold (word) */
    int32_t two_r = r + r;        G32(VA_g_secondary_door_pool + 0x114) = (uint32_t)two_r;          /* 2r */
    int32_t four_r = two_r + two_r; G32(VA_g_collision_radius) = (uint32_t)four_r;       /* 4r */
    int32_t neg_r = -r;           G32(VA_g_collision_box_min) = (uint32_t)neg_r;          /* -r (box lo) */
    int32_t r2 = neg_r * neg_r;                                            /* r^2 (imul) */
    int32_t r2_64 = (int32_t)((uint32_t)r2 << 6);                          /* 64 r^2 */
    G32(VA_g_collision_radius_sq) = (uint32_t)r2_64;                                        /* perp threshold */
    G32(VA_g_collision_radius_sq + 0x4) = (uint32_t)(r2 + r2_64);                                 /* 65 r^2 */
    G32(VA_g_collision_corner_radius_sq) = (uint32_t)(r2_64 + 0x40);                               /* slide threshold */
    G32(VA_g_collision_target_radius) = (uint32_t)((int32_t)G32(VA_g_projectile_collision_width) + 0x20);               /* target radius = r + 0x20 */
    G16(VA_g_max_step_height) = 0;                                          /* 0x3e447 */

    G8(VA_g_collision_hit_flags) = 0;                                           /* 0x3e452 */
    uint8_t saved_flags = *(volatile uint8_t *)(uintptr_t)(esi + 7);       /* 0x3e459 saved, restored at end */
    *(volatile uint8_t *)(uintptr_t)(esi + 7) =
        (uint8_t)(*(volatile uint8_t *)(uintptr_t)(esi + 7) | 2);          /* 0x3e45d state flags |= 2 */

    int32_t vx = (int32_t)velX, vy = (int32_t)velY, vz = (int32_t)velZ;    /* ecx/edx/edi (velZ reload 0x3e461) */

    /* substep count + velocity halving (0x3e467) */
    int32_t substeps;
    if (G32(VA_g_collision_hit_entity + 0x4) < 0xc) {                                  /* mag < 0xc -> single substep */
        substeps = 0;                                         /* loop skipped; only the 0x3e507 single move */
    } else {
        substeps = 1; vx >>= 1; vy >>= 1; vz >>= 1;           /* 0x3e474 */
        if (G32(VA_g_collision_hit_entity + 0x4) >= 0x18) {
            vx >>= 1; vy >>= 1; vz >>= 1; substeps = 3;        /* 0x3e488 */
            if (G32(VA_g_collision_hit_entity + 0x4) >= 0x30) {
                vx >>= 1; vy >>= 1; vz >>= 1; substeps = 7;    /* 0x3e49c */
            }
        }
    }

    /* substep loop (0x3e4a7): apply (scaled) velocity, probe, stop on a hit */
    while (substeps > 0) {
        G32(VA_g_max_climb + 0x18) = G32(VA_g_locate_query_x); G32(VA_g_max_climb + 0x1c) = G32(VA_g_locate_query_z); G32(VA_g_max_climb + 0x20) = G32(VA_g_locate_query_y);  /* saved=query */
        G16(VA_g_collision_blocker_object + 0x4) = (uint16_t)G16(VA_g_player_sector);                /* saved sector */
        G8(VA_g_collision_step_active) = 1;                                      /* ray mode */
        G32(VA_g_locate_query_x) += (uint32_t)vx; G32(VA_g_locate_query_y) += (uint32_t)vy; G32(VA_g_locate_query_z) += (uint32_t)vz;
        probe_collision_step(vx, vy);                  /* 0x3e4f0 (ECX=velX, EDX=velY -> probe step budget) */
        if (G8(VA_g_collision_hit_flags) != 0) goto writeback;                 /* 0x3e4fb jne 0x3e529 (hit) */
        substeps--;                                           /* 0x3e504 dec eax; jg */
    }

    /* single / final substep (0x3e507) */
    G8(VA_g_collision_step_active) = 1;
    G32(VA_g_locate_query_x) += (uint32_t)vx; G32(VA_g_locate_query_y) += (uint32_t)vy; G32(VA_g_locate_query_z) += (uint32_t)vz;
    probe_collision_step(vx, vy);                      /* 0x3e522 (ECX=velX, EDX=velY) */

writeback:                                                    /* 0x3e529 */
    *(volatile uint8_t *)(uintptr_t)(esi + 7) = saved_flags;  /* restore state flags */
    {
        uint8_t *dst = (uint8_t *)(uintptr_t)dest;
        uint32_t rX = G32(VA_g_locate_query_x), rY = G32(VA_g_locate_query_y), rZ = G32(VA_g_locate_query_z);
        *(volatile uint32_t *)(uintptr_t)(dst + 0)    = rX;                          /* 0x3e533 */
        *(volatile uint16_t *)(uintptr_t)(ebx + 0xa)  = (uint16_t)rX;                /* 0x3e535 */
        *(volatile uint16_t *)(uintptr_t)(esi + 0)    = (uint16_t)(rX >> 16);        /* 0x3e53c */
        *(volatile uint32_t *)(uintptr_t)(dst + 4)    = rY;                          /* 0x3e544 */
        *(volatile uint16_t *)(uintptr_t)(ebx + 0xc)  = (uint16_t)rY;                /* 0x3e547 */
        *(volatile uint16_t *)(uintptr_t)(esi + 2)    = (uint16_t)(rY >> 16);        /* 0x3e54e */
        *(volatile uint32_t *)(uintptr_t)(dst + 8)    = rZ;                          /* 0x3e557 */
        *(volatile uint16_t *)(uintptr_t)(ebx + 0xe)  = (uint16_t)rZ;                /* 0x3e55a */
        *(volatile uint16_t *)(uintptr_t)(dst + 0x10) = (uint16_t)rZ;                /* 0x3e55e */
        *(volatile uint16_t *)(uintptr_t)(dst + 0x16) = (uint16_t)rZ;                /* 0x3e562 */
        *(volatile uint16_t *)(uintptr_t)(esi + 0xa)  = (uint16_t)(rZ >> 16);        /* 0x3e569 */
        *(volatile uint16_t *)(uintptr_t)(dst + 0x18) = (uint16_t)G16(VA_g_player_sector);      /* 0x3e573 final sector */
        *(volatile uint8_t  *)(uintptr_t)(dst + 0x1a) = G8(VA_g_collision_hit_flags);                 /* 0x3e57c hit flags */
        *(volatile uint32_t *)(uintptr_t)(dst + 0x1c) = G32(VA_g_collision_hit_entity);                /* 0x3e584 hit entity */
    }
    G16(VA_g_player_sector) = saved_sector;                              /* 0x3e588 restore the original sector */
}

/* move_player_with_collision (0x3e796, 623 B) — integrate the PLAYER position (g_player_x/z/y 16.16 at
 * 0x90a8c/0x90a90/0x90a94) by the double-buffered input velocity queue, substepping each delta through
 * collide_substep_track_sector. EDX = query context (-> g_player_query 0x8c174; the caller passes 0). Toggles
 * the queue selector (0x90bd6) and picks queue A (0x90abe) or B (0x90b42); each queue is [count word][dX,dY
 * dword pairs]. Sets the player's FIXED collision dims (radius 0x1c -> hardcoded 0x8c130..0x8c144 + span
 * 0x8c0e8 from 0x90be0) and the climb threshold (0x90bde = g_max_climb 0x8c148, or 0x10 in the special mode
 * 0x819cd). Builds its own 0x34 scratch frame. Per queue entry: copy query->saved (0x8c160/64/68/70), apply
 * the delta, compute the step budget (frame[+0x24]), call collide_substep_track_sector. After the queue: cross
 * the portal to the resolved sector (find_query_sector) or restore the saved position, store the final sector
 * (0x90c12), recompute the sector ambient (0x8c1de from es:[sector+0xb] + g_light_offset 0x853f6, and 0x8c1d8
 * from the sector flag es:[sector+0xa] bit1), write the resolved position back to g_player_x/z/y, and run the
 * view-bob/vertical-physics update (bridged). The sole caller ignores the return -> void. es-base via 0x90aa8. */
void move_player_with_collision(uint32_t query_ctx)
{
    uint8_t frame[0x40];
    memset(frame, 0, sizeof frame);
    uint8_t *geom = (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer);

    G16(VA_g_vel_queue_select) = (uint16_t)~(uint16_t)G16(VA_g_vel_queue_select);          /* 0x3e796 not [0x90bd6] (toggle select) */
    uint8_t *esi = (uint8_t *)(uintptr_t)(G16(VA_g_vel_queue_select) != 0 ? (0x90abeu + OBJ_DELTA) : (0x90b42u + OBJ_DELTA));

    G32(VA_g_collision_target_active) = 0;                                          /* 0x3e7b1 */
    G32(VA_g_collision_box_max) = 0x1c;                                       /* fixed player dims */
    G32(VA_g_collision_box_min) = 0xffffffe4u;
    G32(VA_g_collision_radius_sq) = 0xc400;
    G32(VA_g_collision_radius_sq + 0x4) = 0xc710;
    G32(VA_g_collision_corner_radius_sq) = 0xc440;
    G32(VA_g_collision_radius) = 0x70;
    G16(VA_g_collision_move_entity + 0xc) = (uint16_t)G16(VA_g_min_fit);                     /* 0x3e7f7 span threshold */
    G16(VA_g_collision_portal_continue) = 0;
    G8(VA_g_collision_step_active)  = 0;                                          /* POINT mode */
    G16(VA_g_collision_sector_stack_count) = 0;
    {
        uint16_t climb = (uint16_t)G16(VA_g_max_climb);              /* 0x3e81c max climb */
        if (G32(VA_g_player_airborne + 0xc) != 0) climb = 0x10;                  /* 0x3e822 special mode */
        G16(VA_g_max_step_height) = climb;                                 /* 0x3e82f */
    }
    G32(VA_g_collision_blocker_object + 0x8) = query_ctx;                                  /* 0x3e835 [0x8c174] = edx */
    /* es=[0x90be8], gs=[0x90bec] (segment loads; geometry via 0x90aa8 in the lift) */
    *(int16_t *)(frame + 0x1e) = (int16_t)G16(VA_g_player_z);        /* 0x3e851 refZ = player Z int */
    *(int32_t *)(frame + 0x24) = 0x100000;                    /* 0x3e85b */
    G8(VA_g_collision_step_active + 0x2) = 0x81;

    /* seed the query/saved position from g_player_x/z/y */
    uint32_t pX = G32(VA_g_player_angle + 0x2), pZ = G32(VA_g_player_x + 0x2), pY = G32(VA_g_player_z + 0x2);
    G32(VA_g_collision_restore_x) = pX; G32(VA_g_max_climb + 0x18) = pX; G32(VA_g_locate_query_x) = pX;
    G32(VA_g_max_climb + 0x1c) = pZ; G32(VA_g_locate_query_z) = pZ;
    G32(VA_g_collision_restore_y) = pY; G32(VA_g_max_climb + 0x20) = pY; G32(VA_g_locate_query_y) = pY;

    /* velocity-queue loop */
    uint16_t count = *(volatile uint16_t *)(uintptr_t)esi;     /* 0x3e8a7 */
    int32_t cx;
    int empty;
    if (count == 0) {                                          /* 0x3e8aa empty queue -> one no-move pass */
        cx = 1; empty = 1;
        G16(VA_g_collision_blocker_object + 0x4) = (uint16_t)G16(VA_g_player_sector);                /* 0x3e8b4 */
    } else {
        cx = count; empty = 0;
        *(volatile uint16_t *)(uintptr_t)esi = 0;            /* 0x3e8c2 consume the count */
        esi += 2;                                            /* 0x3e8c7 -> deltas */
    }

    for (;;) {
        if (!empty) {                                         /* 0x3e8ca loop top (non-empty) */
            G16(VA_g_collision_blocker_object + 0x4) = (uint16_t)G16(VA_g_player_sector);            /* saved sector */
            uint32_t qX = G32(VA_g_locate_query_x), qZ = G32(VA_g_locate_query_z), qY = G32(VA_g_locate_query_y);
            G32(VA_g_collision_restore_x) = qX; G32(VA_g_max_climb + 0x18) = qX;            /* saved X */
            G32(VA_g_max_climb + 0x1c) = qZ;                               /* saved Z */
            G32(VA_g_collision_restore_y) = qY; G32(VA_g_max_climb + 0x20) = qY;            /* saved Y */
            int32_t dX = *(volatile int32_t *)(uintptr_t)(esi + 0);  /* 0x3e8fe */
            int32_t dY = *(volatile int32_t *)(uintptr_t)(esi + 4);  /* 0x3e906 */
            G32(VA_g_locate_query_x) += (uint32_t)dX; G32(VA_g_locate_query_y) += (uint32_t)dY;
            int32_t bx = dX >> 10, by = dY >> 10;            /* 0x3e90f step budget */
            int32_t sq = (int32_t)((uint32_t)(bx * bx) + (uint32_t)(by * by));
            if (!((uint32_t)sq > 0x100000u)) sq = 0x100000;
            *(int32_t *)(frame + 0x24) = sq;
            G8(VA_g_collision_step_active + 0x4) = 1;
        }
        empty = 0;
        collide_substep_track_sector(G32(VA_g_collision_blocker_object + 0x8), frame);  /* 0x3e93a */
        esi += 8;                                            /* 0x3e941 */
        cx = (int16_t)(cx - 1);                              /* 0x3e944 dec cx */
        if (!(cx > 0)) break;                                /* jg 0x3e8ca */
    }

    /* resolve the final sector (portal cross) or restore the saved position */
    uint16_t ax;
    if ((uint16_t)G16(VA_g_player_sector) != 0) {                        /* 0x3e948 */
        uint32_t fq = 0;
        find_query_sector((uint16_t)G16(VA_g_player_sector), frame, &fq);  /* 0x3e953 */
        ax = (uint16_t)fq;
        if (ax != 0) goto store_sector;                      /* 0x3e958 jne 0x3e98c */
    }
    ax = (uint16_t)G16(VA_g_collision_blocker_object + 0x4);                              /* 0x3e95d saved sector */
    if (ax != 0) {                                            /* 0x3e966 je skips restore */
        G32(VA_g_locate_query_x) = G32(VA_g_max_climb + 0x18);                          /* 0x3e968 restore saved pos */
        G32(VA_g_locate_query_z) = G32(VA_g_max_climb + 0x1c);
        G32(VA_g_locate_query_y) = G32(VA_g_max_climb + 0x20);
    }
store_sector:                                                /* 0x3e98c */
    G16(VA_g_player_sector) = ax;

    /* sector ambient / flags */
    if (ax != 0) {                                            /* 0x3e995 or si,si; je 0x3e9cc */
        uint16_t si = ax;
        uint8_t al = *(volatile uint8_t *)(uintptr_t)(geom + (uint16_t)(si + 0xb));   /* 0x3e99a es:[si+0xb] */
        if (al != 0) al = (uint8_t)(al + G8(VA_g_render_sector_walk_mode + 0x23));                                /* 0x3e9a3 += light offset */
        G8(VA_g_viewmodel_shade_level) = al;                                                             /* 0x3e9a9 */
        if (*(volatile uint8_t *)(uintptr_t)(geom + (uint16_t)(si + 0xa)) & 2) {      /* 0x3e9ae test bit1 */
            G16(VA_g_player_sector_cache + 0x2) = 0;                                                         /* 0x3e9b6 */
            G8(VA_g_viewmodel_shade_level) = (uint8_t)(G8(VA_g_viewmodel_shade_level) - 0x10);                              /* 0x3e9bf jne -> 0x3e9cc */
        } else {
            G16(VA_g_player_sector_cache + 0x2) = 0xfff3;                                                    /* 0x3e9c1 */
        }
    } else {
        G8(VA_g_viewmodel_shade_level) = (uint8_t)(G8(VA_g_viewmodel_shade_level) - 0x10);                                  /* 0x3e9cc (sector 0) */
    }

    /* write the resolved position back to g_player_x/z/y, then run the view-bob update */
    G32(VA_g_player_angle + 0x2) = G32(VA_g_locate_query_x);                              /* 0x3e9e4 */
    G32(VA_g_player_x + 0x2) = G32(VA_g_locate_query_z);
    G32(VA_g_player_z + 0x2) = G32(VA_g_locate_query_y);
    /* 0x3e9f5 update_player_view_bob — direct-C. Original does `mov ebp,esp`, so EBP = the
     * base of THIS function's 0x34 local frame; the sole call site is here (bytesearch: 1 E8 site).
     * The body reads floor=word[ebp+0]/ceil=word[ebp+2] ONLY when g_view_root 0x85324 != 0 (else it is
     * push es;pop es;ret). move_player_with_collision never initialises frame[0]/[2] (original: uninit
     * stack; lift: memset 0), so pass the true EBP = &frame — faithful to the hardware and, unlike the
     * old ebp=0 bridge, never flat-NULL-derefs if the 0x85324!=0 path fires under the host. Oracle
     * stages 0x85324=0 (early-exit), so ebp is not consulted there. */
    update_player_view_bob((uint32_t)(uintptr_t)frame);
    /* 0x3e9fa ax=[ebp] (return, ignored by the caller) */
}
