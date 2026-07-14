/* lift_automap.c — verified-C lifts for the `automap` subsystem.
 *
 * The dev-mode overhead wireframe map: it reads the live world wall geometry + the
 * door / entity pools and projects them to a 2D overhead screen space, drawing walls,
 * doors, and entity/player markers as clipped Bresenham lines into a render buffer.
 * See docs/reference/lift/automap.md.
 *
 * Functions lifted here (canon VAs; runtime = canon + OBJ_DELTA):
 *   draw_bresenham_line          0x2ed21 — x/y-major Bresenham rasterizer (solid + XOR modes).
 *   map_line_clip_test           0x2eebc — 1D endpoint clip+interp against a [lo,hi] bound; ret CF.
 *   clip_map_line                0x2ee66 — full segment clip to the map rect (x then y); ret CF.
 *   map_draw_world_edge          0x2ec5f — world->map transform (scale/center) -> clip -> draw.
 *   map_draw_marker_edge         0x2ebfd — rotate a marker vertex (floorceil_rotation_sincos),
 *                                          then fall into the map_draw_world_edge tail.
 *   map_draw_player_marker       0x2eba7 — 3-edge direction arrow (marker_edge x3, on a stack frame).
 *   automap_draw_doors           0x2eb3a — walk the door pool, draw each door's 4 wall edges.
 *   automap_draw_entity_markers  0x2ea9f — walk the state + dynamic entity pools, draw markers.
 *   render_map_geometry          0x2e954 — the workhorse: compute map state, walk visible walls,
 *                                          draw doors + player + entity markers.
 *   draw_map_overlay             0x10dce — per-frame entry; build the map descriptor + dispatch.
 *
 * ABI / behavior transcribed STRICTLY FROM THE DISASM (the corpus decompile is Borland-cspec-on-
 * Watcom and unreliable for register args / multi-reg returns). The clip + Bresenham core mixes
 * 16-bit and 32-bit register ops; only the LOW 16 bits of each coordinate register ever affect
 * observable output (the high bits are dead — tested only via 16-bit `or si,si` / `cmp`, and pixel
 * addresses use movzx/cwde of the low 16). So coordinates are modeled as signed 16-bit values held
 * in `int`; the per-function oracle compares the low-16 coord outputs + CF, and the whole clip+draw
 * chain is additionally byte-verified through map_draw_world_edge's drawn pixels.
 *
 * Faithful details confirmed against the bytes:
 *  - draw_bresenham_line: `shr ...,0xc` in the projection is a LOGICAL shift of a possibly-negative
 *    product; the error-term sign test is `or si,si` (low 16 only); the loop counter is `dec cx`/`jge`
 *    (16-bit, draws major+1 pixels).
 *  - map_line_clip_test: the interpolation is a 16-bit `imul`/`idiv` (DX:AX = AX*BX, AX = DX:AX/CX)
 *    on the screen-space deltas; truncation toward zero matches C integer division.
 *  - map_draw_world_edge: X is negated before scaling (center_x - x), Y is not (y - center_y); X uses
 *    g_map_scale_x, Y uses g_map_scale_y; the `>>12` is logical; screen-center add is 16-bit.
 */
#include <stdint.h>
#include "common.h"

/* flat (host-address) byte/word access; volatile so faithful reads/writes are emitted. */
#define RB(a)    (*(volatile uint8_t  *)(uintptr_t)(a))
#define WB(a,v)  (*(volatile uint8_t  *)(uintptr_t)(a) = (uint8_t)(v))
#define RW(a)    (*(volatile uint16_t *)(uintptr_t)(a))
#define WW(a,v)  (*(volatile uint16_t *)(uintptr_t)(a) = (uint16_t)(v))
#define RW32(a)  (*(volatile uint32_t *)(uintptr_t)(a))
#define WW32(a,v)(*(volatile uint32_t *)(uintptr_t)(a) = (uint32_t)(v))

/* automap map-render state globals (canon VAs). G-macros (common.h) handle canon->runtime. */
#define G_MAP_PITCH        0x85438  /* g_map_buffer_pitch  (u32) */
#define G_MAP_BASE         0x8543c  /* g_map_buffer_base   (u32; flat host buffer address) */
#define G_MAP_AUX          0x8544c  /* dat_8544c (descriptor +0x14 aux dword; wall colours in low word) */
#define G_MAP_DOOR_COLOR   0x8544e  /* byte at aux+2 (door draw colour) */
#define G_MAP_RECT_BASE    0x85450  /* clip rect: [+0]=left [+2]=top [+4]=right [+6]=bottom (words) */
#define G_MAP_RECT_RIGHT   0x85454  /* (u16) w-1 */
#define G_MAP_RECT_BOTTOM  0x85456  /* (u16) h-1 */
#define G_MAP_DRAW_COLOR   0x85458  /* (u16) low byte=colour index, high byte==0xff => XOR mode */
#define G_MAP_SCREEN_CY    0x8545e  /* (u16) screen centre y */
#define G_MAP_SCALE_X      0x85440  /* (u32) world->map x scale (fixed-point >>12) */
#define G_MAP_SCALE_Y      0x85444  /* (u32) world->map y scale */
#define G_MAP_WORLD_CX     0x8545a  /* (u16) world centre x */
#define G_MAP_WORLD_CY     0x8545c  /* (u16) world centre y */
#define G_MAP_SCREEN_CX    0x85460  /* (u16) screen centre x */
#define G_MAP_GEOM_BUF     0x90aa8  /* STORED PTR -> wall geometry buffer */
#define G_MAP_SECTOR_GEOM  0x90aac  /* STORED PTR -> sector geometry (vertex table) */
#define G_DOOR_POOL        0x8b3f4  /* door count byte @[base]; records start at base+4 */
#define G_STATE_POOL_CNT   0x91e00  /* u32 state-pool entity count */
#define G_STATE_POOL_REC   0x91e04  /* state-pool records (stride 0x22) */
#define G_DYN_ENT_CNT      0x90fe0  /* u32 dynamic-entity count */
#define G_DYN_ENT_TBL      0x90fe4  /* dynamic-entity table (stride 0x1c) */

/* floorceil_rotation_sincos (0x3bdf3) — already lifted+verified in renderer.c. */
extern void floorceil_rotation_sincos(int32_t *pt);   /* pt[0]=x pt[1]=y pt[2]=angle (rotates x,y) */
/* compute_mode_bytes (0x302b3) — already lifted+verified. */
extern void compute_mode_bytes(uint8_t *out1, uint8_t *out2);

/* ===================== draw_bresenham_line (0x2ed21) ===================== */
/* Inner span loop. pix = host pixel address; count = major distance (draws count+1 pixels);
 * err = decision var; on err>=0 (16-bit sign) take the minor step and add err_step, else add
 * err_nostep. The `dec cx; jge` loop counter is 16-bit. */
static void amap_bres_run(uint32_t pix, int count, int err,
                          int err_step, int err_nostep,
                          int major_step, int minor_step,
                          uint8_t color, int xor_mode)
{
    int16_t c = (int16_t)count;
    for (;;) {
        if (xor_mode) RB(pix) ^= color;
        else          WB(pix, color);
        pix += (uint32_t)major_step;
        if ((int16_t)err >= 0) { pix += (uint32_t)minor_step; err += err_step; }
        else                   {                              err += err_nostep; }
        c = (int16_t)(c - 1);
        if (c < 0) break;
    }
}

/* EAX=x0, EBX=y0, ECX=x1, EDX=y1 (low 16 of each is the signed screen coordinate). */
static void amap_draw_bresenham(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx)
{
    uint32_t pitch = (uint32_t)G32(G_MAP_PITCH);
    uint32_t base  = (uint32_t)G32(G_MAP_BASE);
    uint16_t cw    = G16(G_MAP_DRAW_COLOR);
    uint8_t  color = (uint8_t)cw;
    int      xor_mode = ((cw >> 8) & 0xff) == 0xff;

    int x0 = (int16_t)eax, y0 = (int16_t)ebx, x1 = (int16_t)ecx, y1 = (int16_t)edx;
    int adx = (int16_t)(x1 - x0); if (adx < 0) adx = -adx;   /* mov esi,ecx; sub si,ax; jns; neg */
    int ady = (int16_t)(y1 - y0); if (ady < 0) ady = -ady;   /* mov edi,edx; sub di,bx; jns; neg */

    int start_x, start_y, minor, count, err, err_step, err_nostep, major;
    if (ady < adx) {                       /* cmp di,si; jge ymajor  => xmajor when ady<adx */
        if (x0 >= x1) {                    /* cmp ax,cx; jl branchB  => branchA when x0>=x1 */
            minor = (ady == 0) ? 0 : ((y0 >= y1) ? (int)pitch : -(int)pitch);
            start_x = x1; start_y = y1;
        } else {
            minor = (ady == 0) ? 0 : ((y0 <= y1) ? (int)pitch : -(int)pitch);
            start_x = x0; start_y = y0;
        }
        count = adx; err = 2*ady - adx; err_step = 2*(ady - adx); err_nostep = 2*ady;
        major = 1;
    } else {                               /* ymajor */
        if (y0 >= y1) {                    /* cmp bx,dx; jl branchD => branchC when y0>=y1 */
            minor = (ady == 0) ? 0 : ((x0 < x1) ? -1 : 1);
            start_x = x1; start_y = y1;
        } else {
            minor = (ady == 0) ? 0 : ((x0 > x1) ? -1 : 1);
            start_x = x0; start_y = y0;
        }
        count = ady; err = 2*adx - ady; err_step = 2*(adx - ady); err_nostep = 2*adx;
        major = (int)pitch;
    }
    uint32_t pix = base + (uint32_t)((int32_t)(int16_t)start_y * (int32_t)pitch) + (uint16_t)start_x;
    amap_bres_run(pix, count, err, err_step, err_nostep, major, minor, color, xor_mode);
}

/* ===================== map_line_clip_test (0x2eebc) ===================== */
/* Clip the segment along its ax/cx axis to [lo,hi], interpolating bx/dx. Coords held in int
 * (low-16 meaningful). Returns 1 if CF set (reject), 0 if clear (accept). */
static int amap_line_clip_test(int *pax, int *pbx, int *pcx, int *pdx, int16_t lo, int16_t hi)
{
    int ax = *pax, bx = *pbx, cx = *pcx, dx = *pdx;

    if ((int16_t)ax > (int16_t)cx) { int t; t=ax;ax=cx;cx=t; t=bx;bx=dx;dx=t; }   /* make ax<=cx */

    if ((int16_t)ax < lo) {                         /* cmp ax,lo; jge skip */
        if ((int16_t)ax != (int16_t)cx) {           /* cmp ax,cx; je skip_interp */
            int16_t span = (int16_t)(cx - ax);      /* sub ecx,eax */
            int16_t da   = (int16_t)(ax - lo);      /* sub eax,ebp */
            int16_t dy   = (int16_t)(bx - dx);      /* sub ebx,edx */
            int16_t q    = (int16_t)(((int32_t)da * (int32_t)dy) / (int32_t)span);  /* imul bx; idiv cx */
            bx = (int16_t)(bx + q);                 /* add ebx,eax */
        }
        ax = lo;                                    /* mov eax,ebp */
        if ((int16_t)ax > (int16_t)cx) { *pax=ax;*pbx=bx;*pcx=cx;*pdx=dx; return 1; }  /* cmp ax,cx; jg reject */
    }
    if ((int16_t)cx > hi) {                         /* cmp cx,hi; jle success */
        if ((int16_t)ax != (int16_t)cx) {           /* cmp ax,cx; je skip_interp */
            int16_t over = (int16_t)(cx - hi);      /* sub ebp,ecx; neg ebp */
            int16_t span = (int16_t)(cx - ax);      /* sub ecx,eax */
            int16_t dy   = (int16_t)(dx - bx);      /* sub edx,ebx */
            int16_t q    = (int16_t)(((int32_t)dy * (int32_t)over) / (int32_t)span); /* imul bp; idiv cx */
            dx = (int16_t)(dx - q);                 /* sub edx,eax */
        }
        cx = hi;                                    /* mov cx,[esi+4] */
        if ((int16_t)ax > (int16_t)cx) { *pax=ax;*pbx=bx;*pcx=cx;*pdx=dx; return 1; }  /* cmp ax,cx; jg reject */
    }
    *pax=ax;*pbx=bx;*pcx=cx;*pdx=dx;
    return 0;
}

/* ===================== clip_map_line (0x2ee66) ===================== */
/* Cohen–Sutherland-style segment clip to the map rect at `rect` (host addr of the 4 bound words:
 * [+0]=left [+2]=top [+4]=right [+6]=bottom). Returns 1 if CF set (reject), 0 if accept. On accept,
 * *pax..*pdx hold the clipped endpoints in the register roles the original leaves them. */
static int amap_clip_map_line(int *pax, int *pbx, int *pcx, int *pdx, uint32_t rect)
{
    int ax = *pax, bx = *pbx, cx = *pcx, dx = *pdx;
    int16_t left   = (int16_t)RW(rect + 0);
    int16_t top    = (int16_t)RW(rect + 2);
    int16_t right  = (int16_t)RW(rect + 4);
    int16_t bottom = (int16_t)RW(rect + 6);

    if (!((int16_t)ax > (int16_t)cx)) { int t; t=ax;ax=cx;cx=t; t=bx;bx=dx;dx=t; }  /* make ax=max x */
    if ((int16_t)ax < left)   return 1;
    if ((int16_t)cx > right)  return 1;
    if (!((int16_t)bx > (int16_t)dx)) { int t; t=ax;ax=cx;cx=t; t=bx;bx=dx;dx=t; }  /* make bx=max y */
    if ((int16_t)bx < top)    return 1;
    if ((int16_t)dx > bottom) return 1;
    if (!((int16_t)ax > (int16_t)cx)) { int t; t=ax;ax=cx;cx=t; t=bx;bx=dx;dx=t; }  /* make ax>cx */

    if (amap_line_clip_test(&ax, &bx, &cx, &dx, left, right)) return 1;             /* x clip */
    { int t; t=ax;ax=bx;bx=t; t=cx;cx=dx;dx=t; }                                    /* xchg eax,ebx; xchg ecx,edx */
    if (amap_line_clip_test(&ax, &bx, &cx, &dx, top, bottom)) return 1;             /* y clip (esi+2) */
    { int t; t=ax;ax=bx;bx=t; t=cx;cx=dx;dx=t; }                                    /* swap back */

    *pax=ax;*pbx=bx;*pcx=cx;*pdx=dx;
    return 0;
}

/* ===================== map_draw_world_edge (0x2ec5f) + tail ===================== */
/* tail (0x2ec7b): scale relative coords to screen space, clip, draw. Reached directly from
 * map_draw_marker_edge (which supplies already-relative coords). */
static void amap_world_edge_tail(int rx0, int ry0, int rx1, int ry1)
{
    int32_t scx = (int32_t)G32(G_MAP_SCALE_X);
    int32_t scy = (int32_t)G32(G_MAP_SCALE_Y);
    int16_t scr_cx = (int16_t)G16(G_MAP_SCREEN_CX);
    int16_t scr_cy = (int16_t)G16(G_MAP_SCREEN_CY);

    int16_t nx0 = (int16_t)(-(int16_t)rx0);    /* neg eax; cwde   (X is negated: center-relative) */
    int16_t nx1 = (int16_t)(-(int16_t)rx1);    /* neg ecx; movsx  */
    uint32_t ex0 = (uint32_t)((int32_t)nx0 * scx) >> 12;   /* shr ...,0xc  (LOGICAL) */
    uint32_t ex1 = (uint32_t)((int32_t)nx1 * scx) >> 12;
    uint32_t ey0 = (uint32_t)((int32_t)(int16_t)ry0 * scy) >> 12;   /* movsx ebx,bx (Y not negated) */
    uint32_t ey1 = (uint32_t)((int32_t)(int16_t)ry1 * scy) >> 12;

    int sx0 = (int16_t)((uint16_t)ex0 + (uint16_t)scr_cx);   /* add ax,[screen_cx] (16-bit) */
    int sy0 = (int16_t)((uint16_t)ey0 + (uint16_t)scr_cy);
    int sx1 = (int16_t)((uint16_t)ex1 + (uint16_t)scr_cx);
    int sy1 = (int16_t)((uint16_t)ey1 + (uint16_t)scr_cy);

    if (amap_clip_map_line(&sx0, &sy0, &sx1, &sy1, (uint32_t)GADDR(G_MAP_RECT_BASE))) return;
    amap_draw_bresenham((uint32_t)sx0, (uint32_t)sy0, (uint32_t)sx1, (uint32_t)sy1);
}

/* EAX=x0, EBX=y0, ECX=x1, EDX=y1 (world coordinates). */
static void amap_world_edge(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx)
{
    int16_t wcx = (int16_t)G16(G_MAP_WORLD_CX);
    int16_t wcy = (int16_t)G16(G_MAP_WORLD_CY);
    int rx0 = (int16_t)((int16_t)eax - wcx);   /* sub ax,[world_cx] */
    int ry0 = (int16_t)((int16_t)ebx - wcy);   /* sub bx,[world_cy] */
    int rx1 = (int16_t)((int16_t)ecx - wcx);
    int ry1 = (int16_t)((int16_t)edx - wcy);
    amap_world_edge_tail(rx0, ry0, rx1, ry1);
}

/* ===================== map_draw_marker_edge (0x2ebfd) ===================== */
/* frame[0]=x frame[1]=y frame[2]=angle frame[3]=offset_x frame[4]=offset_y (int32 each). Rotates
 * (frame[0],frame[1]) by frame[2] in place, then draws the edge from the marker centre (offset) to
 * the rotated tip via the world_edge tail. */
static void amap_marker_edge(int32_t *frame)
{
    floorceil_rotation_sincos(frame);            /* call 0x3bdf3 (rotates frame[0],frame[1]) */
    int rx0 = frame[3];                                 /* eax = offset_x */
    int ry0 = frame[4];                                 /* ebx = offset_y */
    int rx1 = frame[0] + frame[3];                      /* ecx = rotated_x + offset_x */
    int ry1 = frame[1] + frame[4];                      /* edx = rotated_y + offset_y */
    amap_world_edge_tail(rx0, ry0, rx1, ry1);
}

/* ===================== map_draw_player_marker (0x2eba7) ===================== */
/* EAX=angle, ECX=offset_x, EDX=offset_y. Draws a 3-edge direction arrow. */
static void amap_player_marker(uint32_t eax, uint32_t ecx, uint32_t edx)
{
    int32_t frame[6];
    frame[3] = (int32_t)ecx;                            /* [+0xc] offset_x */
    frame[4] = (int32_t)edx;                            /* [+0x10] offset_y */
    frame[2] = (int32_t)(0x80u - eax);                  /* [+8] = 0x80 - angle  (neg eax; add 0x80) */

    frame[0] = 0x20;  frame[1] = 0x10;  amap_marker_edge(frame);
    frame[0] = 0x20;  frame[1] = -0x10; amap_marker_edge(frame);
    frame[0] = 0x40;  frame[1] = 0;     amap_marker_edge(frame);
}

/* ===================== automap_draw_doors (0x2eb3a) ===================== */
static void amap_draw_doors(void)
{
    uint32_t base = (uint32_t)GADDR(G_DOOR_POOL);
    uint8_t  cl   = RB(base);                            /* door count */
    if (cl == 0) return;
    uint32_t rec  = base + 4;                            /* first door record */
    uint32_t scratch = (uint32_t)GADDR(G_MAP_RECT_BASE + 0x12);  /* 0x85462 scratch */

    do {
        uint32_t src = (uint32_t)((uint32_t)RW32(rec + 0x2e) + 0x82);  /* corner data ptr (+0x82) */
        uint32_t dst = scratch;
        for (int i = 0; i < 4; i++) {                   /* copy 4 corner (x,y) pairs */
            WW(dst + 0, RW(src + 0));
            WW(dst + 2, RW(src + 4));
            src += 0x10;
            dst += 4;
        }
        WW32(dst + 0, RW32(scratch + 0));               /* 5th corner = 1st (wrap) */

        uint32_t e = scratch;
        for (int i = 0; i < 4; i++) {                   /* draw the 4 edges */
            uint32_t ex0 = RW(e + 0);
            uint32_t ey0 = RW(e + 2);
            uint32_t ex1 = RW(e + 4);
            uint32_t ey1 = RW(e + 6);
            /* asm: cx=[edi] dx=[edi+2] ax=[edi+4] bx=[edi+6]; world_edge(ax,bx,cx,dx) */
            amap_world_edge(ex1, ey1, ex0, ey0);
            e += 4;
        }
        rec += 0x1f6;
    } while (--cl != 0);
}

/* ===================== automap_draw_entity_markers (0x2ea9f) ===================== */
/* Per-marker angle byte -> player_marker angle: eax = inc_ah(neg(2*type)). */
static uint32_t amap_marker_angle(uint8_t type)
{
    uint32_t eax = (uint32_t)type;
    eax = eax + eax;                                    /* add eax,eax */
    eax = (uint32_t)(-(int32_t)eax);                    /* neg eax */
    uint8_t ah = (uint8_t)((eax >> 8) + 1);             /* inc ah */
    eax = (eax & 0xffff00ffu) | ((uint32_t)ah << 8);
    return eax;
}

static void amap_draw_entity_markers(void)
{
    int16_t wcx = (int16_t)G16(G_MAP_WORLD_CX);
    int16_t wcy = (int16_t)G16(G_MAP_WORLD_CY);

    uint32_t ecx = (uint32_t)G32(G_STATE_POOL_CNT);
    if (ecx != 0) {
        uint32_t rec = (uint32_t)GADDR(G_STATE_POOL_REC);
        do {
            uint32_t ptr = RW32(rec + 4);               /* record pointer */
            if (ptr != 0) {
                ecx--;
                uint8_t type = RB(ptr + 6);
                uint32_t ang = amap_marker_angle(type);
                int relx = (int16_t)((int16_t)RW(ptr + 0) - wcx);
                int rely = (int16_t)((int16_t)RW(ptr + 2) - wcy);
                amap_player_marker(ang, (uint32_t)relx, (uint32_t)rely);
            }
            rec += 0x22;
        } while (ecx != 0);
    }

    ecx = (uint32_t)G32(G_DYN_ENT_CNT);
    if (ecx != 0) {
        uint32_t rec = (uint32_t)GADDR(G_DYN_ENT_TBL);
        do {
            uint32_t ptr = RW32(rec + 0);               /* entry pointer */
            if (ptr != 0) {
                ecx--;
                uint8_t type = RB(ptr + 9);             /* dynamic-entity type at +9 */
                uint32_t ang = amap_marker_angle(type);
                int relx = (int16_t)((int16_t)RW(ptr + 0) - wcx);
                int rely = (int16_t)((int16_t)RW(ptr + 2) - wcy);
                amap_player_marker(ang, (uint32_t)relx, (uint32_t)rely);
            }
            rec += 0x1c;
        } while (ecx != 0);
    }
}

/* ===================== render_map_geometry (0x2e954) ===================== */
/* EAX=render buffer (host addr), EDX=pitch, EBX=map descriptor (host addr). pushal/popal =>
 * register-transparent; observable output is the obj3 map-state globals + the drawn buffer. */
static void amap_render_map_geometry(uint32_t eax, uint32_t edx, uint32_t ebx)
{
    G32(G_MAP_BASE)  = (int32_t)eax;
    G32(G_MAP_PITCH) = (int32_t)edx;

    uint32_t w = RW(ebx + 0xc);                          /* rect width */
    uint32_t edi = w; edi = (edi - 1) & 0xffffffffu;     /* w-1 */
    G16(G_MAP_RECT_RIGHT) = (uint16_t)edi;
    G16(G_MAP_SCREEN_CX)  = (uint16_t)((w & 0xffff) >> 1);

    uint32_t h = RW(ebx + 0xe);                          /* rect height */
    uint32_t ecx = (h - 1) & 0xffffffffu;                /* h-1 */
    G16(G_MAP_RECT_BOTTOM) = (uint16_t)ecx;
    G16(G_MAP_SCREEN_CY)   = (uint16_t)((h & 0xffff) >> 1);

    G16(G_MAP_WORLD_CX) = RW(ebx + 2);
    G16(G_MAP_WORLD_CY) = RW(ebx + 6);
    G32(G_MAP_AUX)      = (int32_t)RW32(ebx + 0x14);

    /* scale selection: pick the axis whose aspect-corrected extent fits (unsigned compare).
     * The mul/div pairs are unsigned 32x32->64 / 64-by-32 (edx:eax) — use 64-bit intermediates. */
    uint32_t hh    = (ecx + 1);                          /* inc ecx -> h */
    uint32_t wfull = (edi + 1);                          /* inc edi -> w */
    uint32_t aspX  = RB(ebx + 0x12);
    uint32_t aspY  = RB(ebx + 0x13);
    uint32_t scale = RW(ebx + 0x10);                     /* user scale factor */
    uint32_t lhs = hh    * (aspX * 0x140u);              /* h * aspectX*320 (imul, low 32) */
    uint32_t rhs = wfull * (aspY * 0xc8u);               /* w * aspectY*200 */
    if (lhs > rhs) {                                     /* cmp eax,edx; jbe branch2 => branch1 if > */
        uint32_t sy = (uint32_t)(((uint64_t)scale * hh) / 0xc8u);
        G32(G_MAP_SCALE_Y) = (int32_t)sy;
        uint32_t sx = (uint32_t)(((uint64_t)sy * aspX) / aspY);
        G32(G_MAP_SCALE_X) = (int32_t)sx;
    } else {
        uint32_t sx = (uint32_t)(((uint64_t)scale * wfull) / 0x140u);
        G32(G_MAP_SCALE_X) = (int32_t)sx;
        uint32_t sy = (uint32_t)(((uint64_t)sx * aspY) / aspX);
        G32(G_MAP_SCALE_Y) = (int32_t)sy;
    }

    /* wall walk (the original enters the body before the dec/jg, so it runs at least once). */
    uint32_t geom   = (uint32_t)G32(G_MAP_GEOM_BUF);     /* STORED PTR */
    uint32_t sector = (uint32_t)G32(G_MAP_SECTOR_GEOM);  /* STORED PTR */
    uint32_t wp = geom + RW(geom + 6);
    int32_t  count = (int32_t)(uint32_t)RW(wp - 2);
    uint16_t aux = G16(G_MAP_AUX);                       /* wall colour word */
    do {
        if (RB(wp + 0xa) & 0x40) {                       /* visible flag */
            uint8_t color;
            if (RB(wp + 0xa) & 0x20) color = (uint8_t)aux;          /* keep al */
            else                     color = (uint8_t)(aux >> 8);   /* xchg ah,al -> al = high byte */
            G8(G_MAP_DRAW_COLOR) = color;
            uint32_t i0 = RW(wp + 0);
            uint32_t i1 = RW(wp + 2);
            uint32_t x1 = RW(sector + i1 + 8), y1 = RW(sector + i1 + 0xa);
            uint32_t x0 = RW(sector + i0 + 8), y0 = RW(sector + i0 + 0xa);
            amap_world_edge(x0, y0, x1, y1);
        }
        wp += 0xc;
    } while (--count > 0);                                /* dec ecx; jg (signed 32-bit) */

    G8(G_MAP_DRAW_COLOR) = RB((uint32_t)GADDR(G_MAP_DOOR_COLOR));
    amap_draw_doors();

    G8(G_MAP_DRAW_COLOR) = RB(ebx + 0x17);               /* player marker colour */
    amap_player_marker((uint32_t)RW32(ebx + 8), 0, 0);   /* angle = descriptor[+8], offset (0,0) */
    amap_draw_entity_markers();
}

/* ===================== draw_map_overlay (0x10dce) ===================== */
static void amap_draw_overlay(void)
{
    uint8_t desc[0x18];
    RW32(desc + 0)    = (uint32_t)G32(VA_g_player_angle + 0x2);          /* +0 dword (world centre x in high word) */
    RW32(desc + 4)    = (uint32_t)G32(VA_g_player_z + 0x2);          /* +4 dword (world centre y in high word) */
    RW32(desc + 8)    = (uint32_t)G16(VA_g_player_angle);          /* +8 player angle (movzx word) */
    WW(desc + 0xe,      G16(VA_g_render_height));                   /* +0xe rect height */
    WW(desc + 0xc,      G16(VA_g_render_width));                   /* +0xc rect width */
    uint32_t t = (uint32_t)GADDR(VA_g_player_movement_enabled + 0x12);
    WB(desc + 0x14,     RB(t + 1));
    WB(desc + 0x15,     RB(t + 8));
    WB(desc + 0x16,     RB(t + 5));
    WB(desc + 0x17,     RB(t + 2));                      /* player marker colour */
    WW(desc + 0x10,     G16(VA_g_cfg_das2_arg + 0x1bc));                   /* +0x10 scale factor */
    compute_mode_bytes(desc + 0x12, desc + 0x13); /* +0x12/+0x13 aspect bytes */

    amap_render_map_geometry((uint32_t)G32(VA_g_render_target_buffer),     /* g_render_target_buffer */
                             (uint32_t)G32(VA_g_screen_pitch),     /* g_screen_pitch */
                             (uint32_t)(uintptr_t)desc);
}

/* ===================== public lift entry points ===================== */

void draw_bresenham_line(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx)
{
    amap_draw_bresenham(eax, ebx, ecx, edx);
}

/* clip leaves: in/out coords via the low 16 bits of the pointed regs; return CF (1=reject). */
int map_line_clip_test(uint32_t *io_eax, uint32_t *io_ebx, uint32_t *io_ecx,
                              uint32_t *io_edx, uint32_t esi)
{
    int ax = (int16_t)*io_eax, bx = (int16_t)*io_ebx, cx = (int16_t)*io_ecx, dx = (int16_t)*io_edx;
    int16_t lo = (int16_t)RW(esi + 0), hi = (int16_t)RW(esi + 4);
    int cf = amap_line_clip_test(&ax, &bx, &cx, &dx, lo, hi);
    *io_eax = (*io_eax & 0xffff0000u) | (uint16_t)ax;
    *io_ebx = (*io_ebx & 0xffff0000u) | (uint16_t)bx;
    *io_ecx = (*io_ecx & 0xffff0000u) | (uint16_t)cx;
    *io_edx = (*io_edx & 0xffff0000u) | (uint16_t)dx;
    return cf;
}

int clip_map_line(uint32_t *io_eax, uint32_t *io_ebx, uint32_t *io_ecx,
                         uint32_t *io_edx, uint32_t esi)
{
    int ax = (int16_t)*io_eax, bx = (int16_t)*io_ebx, cx = (int16_t)*io_ecx, dx = (int16_t)*io_edx;
    int cf = amap_clip_map_line(&ax, &bx, &cx, &dx, esi);
    *io_eax = (*io_eax & 0xffff0000u) | (uint16_t)ax;
    *io_ebx = (*io_ebx & 0xffff0000u) | (uint16_t)bx;
    *io_ecx = (*io_ecx & 0xffff0000u) | (uint16_t)cx;
    *io_edx = (*io_edx & 0xffff0000u) | (uint16_t)dx;
    return cf;
}

void map_draw_world_edge(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx)
{ amap_world_edge(eax, ebx, ecx, edx); }

void map_draw_player_marker(uint32_t eax, uint32_t ecx, uint32_t edx)
{ amap_player_marker(eax, ecx, edx); }

void automap_draw_doors(void)            { amap_draw_doors(); }
void automap_draw_entity_markers(void)   { amap_draw_entity_markers(); }

void render_map_geometry(uint32_t eax, uint32_t edx, uint32_t ebx)
{ amap_render_map_geometry(eax, edx, ebx); }

void draw_map_overlay(void)              { amap_draw_overlay(); }
