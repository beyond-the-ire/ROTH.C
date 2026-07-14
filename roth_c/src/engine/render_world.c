/* lift_render_world.c — verified-C lifts for the 3D world-renderer's SCENE-ORCHESTRATION
 * spine (the layer ABOVE the already-native render_world_scene 0x28a79 / face-list / driver
 * subtree).
 *
 * These are the per-frame view drivers that run_gameplay_frame (0x103b3) and the level/warp
 * render sites (0x16a54/0x16c04/0x16c95) enter through: the frame-context setup, the primary
 * scene pass, the split-viewport lower pass, the reflection (mirror) subviews, and the
 * clipped-sector subscene that fans out to the native render tree. Every callee they invoke is
 * already a verified-C lift (render_world_scene, walk_visible_sectors, begin_sector_render_tree,
 * build_sector_draw_order, build_scene_draw_list, render_world_face_list, transform_world_vertices,
 * apply_view_camera_params, shift_wall_nodes_vertical, reflect_view_across_mirror_plane,
 * tick_ambient_render_animation, tick_doors_for_frame, update_active_sounds,
 * setup_surface_render_constants, set_909a4_save_old_to_852f2, signext_852f2_to_909a4,
 * init_84964_block) — so these orchestrators are almost pure C control flow + obj3 global writes
 * that call native children directly. The only bridged callees are the DOS-free/DPMI teardown
 * leaves and the vestigial per-frame overlay callback [0x8495c]. (clear_es_record_field4 0x293a3
 * is now a direct C call to its verified-C lift via g_os_sel_base — oracle-neutral.)
 *
 * VERIFICATION: these are segment-state + full-pipeline functions (§4.5 axis-2/3), so the gate
 * is the IN-GAME live-swap / differential run in the dedicated debug session, NOT the static
 * oracle. Registered in lift_registry.c so ROTH_LIFT=<name> runs the C in the real game; the
 * children resolve their own selector bases via g_os_sel_base in-process.
 *
 * ADDRESSING: canon VAs; GADDR/G8/G16/G32 add OBJ_DELTA. STORED-POINTER records (the frame
 * context ptr in EAX, the ptr at [0x85274]) are already runtime addresses -> deref RAW. fs:/gs:
 * geometry offsets are 16-bit -> masked to uint16_t. */

#include <string.h>
#include "common.h"
#include "os_api.h"      /* os_dpmi_free_descriptor (free_dpmi_selector 0x2f772 -> c2 contract) */

extern uint32_t (*g_os_sel_base)(uint16_t);

/* ================= render_world_view (0x10c8f) — the level/warp single-view entry ================= */
/* pushal-wrapped: eax = dword[0x707b3]-dword[0x85ce0], edx = dword[0x707b7]-dword[0x85ce4];
 * render_world_scene(eax,edx); returns eax = &g_something (0x90a48, rebased). All GP regs but eax
 * are preserved (pushal/popal). */
uint32_t render_world_view(uint16_t es, uint16_t fs, uint16_t gs)
{
    uint32_t eax = (uint32_t)(G32(VA_g_mouse_x) - G32(VA_g_view_x));   /* 0x10c90/0x10c95 */
    uint32_t edx = (uint32_t)(G32(VA_g_mouse_y) - G32(VA_g_view_y));   /* 0x10c9b/0x10ca1 */
    (void)render_world_scene(eax, edx, 0, es, fs, gs); /* call 0x28a79 */
    return (uint32_t)GADDR(VA_g_world_render_subpass_kind);                          /* mov eax,0x90a48 (relocated) */
}

/* ================= setup_frame_render_context (0x2aa3e) — per-frame view/camera commit ================= */
/* EAX = frame-context record (raw runtime ptr). Copies its fields into the 0x852xx/0x909xx render
 * globals, seeds the projection scale into the record at [0x85274], then runs the per-frame ambient
 * animation / door tick / sound update / surface-const setup. Preserves EAX/ESI/ECX + segments
 * (push/pop). */
void setup_frame_render_context(uint32_t eax_record)
{
    volatile uint8_t *rec = (volatile uint8_t *)(uintptr_t)eax_record;   /* esi = eax (raw ptr) */
    G32(VA_g_current_proc_tag + 0x128) = (int32_t)eax_record;                          /* [0x84ac4]=eax */
    G16(VA_g_surface_record_selector) = *(volatile uint16_t *)(rec + 0x10);           /* surface-record selector */
    G16(VA_g_surface_record_selector + 0x2) = *(volatile uint16_t *)(rec + 0x12);
    G16(VA_g_vertex_selector) = *(volatile uint16_t *)(rec + 0x14);           /* transformed-vertex selector */
    G32(VA_g_world_surface_draw_flags + 0x8) = *(volatile int32_t  *)(rec + 0x0c);
    G32(VA_g_view_bounds_rect) = *(volatile int32_t  *)(rec + 0x00);
    G32(VA_g_view_params_block) = *(volatile int32_t  *)(rec + 0x04);
    G32(VA_g_view_params_block + 0x4) = *(volatile int32_t  *)(rec + 0x08);           /* projection-record ptr */
    G16(VA_g_anim_clock + 0x2) = *(volatile uint16_t *)(rec + 0x16);

    volatile uint8_t *pr = (volatile uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_view_params_block + 0x4);  /* ebx = [0x85274] (raw) */
    *(volatile uint16_t *)(pr + 0x0a) = (uint16_t)G16(VA_g_view_center_x);  /* [ebx+0xa]=[0x90a70] */
    int32_t eax = *(volatile int32_t *)(rec + 0x18);             /* eax=[esi+0x18] */
    uint16_t cx = *(volatile uint16_t *)(pr + 8);                /* cx=[ebx+8] */
    eax = (int32_t)(eax * (int32_t)(uint32_t)cx);                /* imul eax,ecx */
    eax >>= 7;                                                   /* sar eax,7 */
    G32(VA_g_subpass_persp_step + 0x4) = eax;
    *(volatile uint16_t *)(pr + 0x0c) = (uint16_t)((uint16_t)eax + (uint16_t)G16(VA_g_anim_clock + 0x2)); /* add ax,[0x8532c] */

    uint16_t bcc = (uint16_t)G16(VA_g_frame_tick_counter);
    G16(VA_g_anim_clock) = bcc;                                          /* [0x8532a]=[0x90bcc] */
    uint16_t edx = (uint16_t)(bcc - (uint16_t)G16(VA_g_last_frame_tick));     /* dx = bcc - [0x85320] */
    G16(VA_g_last_frame_tick) = bcc;                                          /* [0x85320]=ax */
    uint32_t edxz = (uint32_t)edx;                               /* and edx,0xffff */
    if (edxz >= 0x2d) edxz = 0x2d;                               /* cmp 0x2d; jb; else edx=0x2d */
    G32(VA_g_frame_time_scale) = (int32_t)edxz;                                /* [0x85324]=edx (door/anim frame step) */

    tick_ambient_render_animation();                     /* call 0x2ab30 */
    G16(VA_g_das_cache_tick) = (uint16_t)(G16(VA_g_das_cache_tick) + 1);                 /* inc word[0x90c0a] */
    tick_doors_for_frame();                              /* call 0x3d959 (reads edx=[0x85324]) */
    update_active_sounds();                              /* call 0x27b05 */
    G16(VA_g_floor_tex_caps + 0x42) = 0;                                           /* mov word[0x909f4],0 */
    setup_surface_render_constants();                    /* call 0x2abfb */
}

/* ================= render_clipped_sector_subscene (0x29305) — the sector render-tree fan-out ================= */
/* Walks the [0x85220]-count visible-sector table at [0x85224] (stride 6): per sector seeds the
 * view-space bounds globals + builds its render tree (begin_sector_render_tree). Then draw-order,
 * scene draw-list, and the face-list render pass. Shared tail: render_primary_scene_view falls
 * through into this; render_reflection_subviews calls it directly (0x2867f). */
void render_clipped_sector_subscene(void)
{
    int32_t ecx = G32(VA_g_visible_extent_count);                                 /* mov ecx,[0x85220] */
    if (ecx == 0) return;                                       /* test; je 0x29399 */
    G16(VA_g_vertex_selector + 0x20) = 0;                                           /* depth */
    G16(VA_g_vertex_selector + 0x22) = 0x8000;                                      /* header cursor */
    G16(VA_g_vertex_selector + 0x24) = 0;                                           /* sector count */
    G16(VA_g_vertex_selector + 0x2) = 2;
    uint32_t edi = 0x85224;                                     /* obj3 sector table */
    G16(VA_g_vertex_selector + 0x18) = 0;                                           /* parent id */
    do {                                                        /* loop 0x2933c */
        uint32_t esi = (uint16_t)G16(edi);                      /* sub esi,esi; mov si,[edi] */
        uint16_t a = (uint16_t)G16(edi + 2);
        G16(VA_g_view_bound_left) = a; G16(VA_g_sector_cull_coord + 0x4) = a;
        a = (uint16_t)G16(edi + 4);
        G16(VA_g_view_bound_right) = a; G16(VA_g_sector_cull_coord + 0x2) = a;
        begin_sector_render_tree(esi);                   /* call 0x29812 (edi/ecx preserved) */
        edi += 6;                                               /* add edi,6 */
        ecx--;                                                  /* dec ecx */
    } while (ecx > 0);                                          /* jg (signed) */
    build_sector_draw_order(0);                          /* call 0x2a6d0 (esi ignored) */
    G8(VA_g_render_sector_walk_mode + 0x25) = 0;                                            /* mov byte[0x853f8],0 */
    build_scene_draw_list(0);                            /* call 0x2a0a0 (ecx vestigial) */
    render_world_face_list(0, (uint16_t)G16(VA_g_vertex_selector));   /* call 0x2ad21 (ecx vestigial, gs=[0x852cc]) */
    G16(VA_g_vertex_selector + 0x1c) = 0xffff;                                      /* mov word[0x852e8],0xffff */
}

/* ================= render_primary_scene_view (0x292be) — main forward view ================= */
/* Seeds the view-space X/Y bounds from [0x85308]/[0x8530c], runs the visible-sector portal walk,
 * then FALLS THROUGH into render_clipped_sector_subscene (0x29305). (es=[0x85294]/gs=[0x852cc]
 * register loads are moot: the native children reload their own selectors from the globals.) */
void render_primary_scene_view(void)
{
    int32_t v = G32(VA_g_sector_cull_coord + 0xe);
    G16(VA_g_sector_cull_coord + 0x4) = (uint16_t)v; G16(VA_g_view_bound_left) = (uint16_t)v;
    G16(VA_g_view_bound_top) = (uint16_t)((uint32_t)v >> 16);               /* shr eax,0x10 */
    v = G32(VA_g_sector_cull_coord + 0x12);
    G16(VA_g_sector_cull_coord + 0x2) = (uint16_t)v; G16(VA_g_view_bound_right) = (uint16_t)v;
    G16(VA_g_view_bound_bottom) = (uint16_t)((uint32_t)v >> 16);
    walk_visible_sectors();                             /* call 0x294c0 */
    render_clipped_sector_subscene();                   /* fall through into 0x29305 */
}

/* ================= render_split_viewport_lower (0x288fb) — the lower split-viewport face pass ================= */
/* Offsets the framebuffer base by [0x89f2c], swaps the destination selector [0x90c06]<-[0x89f28],
 * bumps the top Y, y-shifts the wall nodes, renders the face list, then restores. */
void render_split_viewport_lower(void)
{
    int32_t fdelta = G32(VA_g_render_target_secondary_size);
    G32(VA_g_render_target_buffer) += fdelta;                                    /* add [0x85414],[0x89f2c] */
    uint16_t save_90c06 = (uint16_t)G16(VA_g_render_target_selector);
    G16(VA_g_render_target_selector) = (uint16_t)G16(VA_g_render_target_selector_secondary);                     /* [0x90c06]=[0x89f28] */
    G16(VA_g_floor_tex_caps + 0x42) |= 2;                                         /* or [0x909f4],2 */
    uint32_t e = (uint32_t)(uint16_t)G16(VA_g_view_bound_bottom);
    e += 1;                                                    /* inc eax */
    if (G8(VA_g_render_double_scanline_flag) != 0) e >>= 1;                             /* shr eax,1 */
    G16(VA_g_floor_tex_caps + 0x44) = (uint16_t)(G16(VA_g_floor_tex_caps + 0x44) + (uint16_t)e);     /* add [0x909f6],ax */
    if (g_os_sel_base)
        shift_wall_nodes_vertical(g_os_sel_base((uint16_t)G16(VA_g_map_geometry_selector)),
                                         g_os_sel_base((uint16_t)G16(VA_g_surface_record_selector)));  /* call 0x28972 */
    if ((uint16_t)G16(VA_g_surface_record_selector) != 0)
        render_world_face_list(0, (uint16_t)G16(VA_g_vertex_selector));  /* call 0x2ad21 */
    G16(VA_g_render_target_selector) = save_90c06;                                 /* pop [0x90c06] */
    G32(VA_g_render_target_buffer) -= fdelta;                                    /* sub [0x85414],[0x89f2c] */
}

/* ================= render_secondary_viewport_pass (0x286df) — mirror/secondary viewport pass ================= */
/* The 0x288fb pass wrapped in the [0x71ef6]/[0x90bf4] viewport-height save/adjust + the
 * set_909a4/signext view-plane fixups (used for the reflection subviews). */
void render_secondary_viewport_pass(void)
{
    uint16_t save_71ef6 = (uint16_t)G16(VA_g_init_stage_error_strings + 0x126);              /* push [0x71ef6] */
    uint16_t bx = (uint16_t)G16(VA_g_render_viewport_height);                      /* push [0x90bf4]; bx=[0x90bf4] */
    uint16_t save_90bf4 = bx;
    G16(VA_g_init_stage_error_strings + 0x126) = (uint16_t)(G16(VA_g_init_stage_error_strings + 0x126) - bx);              /* sub [0x71ef6],bx */
    G16(VA_g_span_src_wrap_reoffset + 0x16) = (uint16_t)(G16(VA_g_span_src_wrap_reoffset + 0x16) - bx);              /* sub [0x90992],bx */
    uint16_t axv = (uint16_t)(G16(VA_g_init_stage_error_strings + 0x11e) - bx);             /* ax=[0x71eee]-bx */
    G16(VA_g_reflection_view_list + 0x84) = bx;                                        /* [0x853c8]=bx */
    set_909a4_save_old_to_852f2(axv);                 /* call 0x2acfc (uses AX) */
    uint16_t save_909f6 = (uint16_t)G16(VA_g_floor_tex_caps + 0x44);            /* push [0x909f6] */
    int32_t fdelta = G32(VA_g_render_target_secondary_size);
    G32(VA_g_render_target_buffer) += fdelta;                                  /* add [0x85414],[0x89f2c] */
    uint16_t save_90c06 = (uint16_t)G16(VA_g_render_target_selector);           /* push [0x90c06] */
    G16(VA_g_render_target_selector) = (uint16_t)G16(VA_g_render_target_selector_secondary);                  /* [0x90c06]=[0x89f28] */
    G16(VA_g_floor_tex_caps + 0x42) |= 2;                                       /* or [0x909f4],2 */
    uint32_t e = (uint32_t)(uint16_t)G16(VA_g_view_bound_bottom);
    e += 1;
    if (G8(VA_g_render_double_scanline_flag) != 0) e >>= 1;
    G16(VA_g_floor_tex_caps + 0x44) = (uint16_t)(G16(VA_g_floor_tex_caps + 0x44) + (uint16_t)e);   /* add [0x909f6],ax */
    if (g_os_sel_base)
        shift_wall_nodes_vertical(g_os_sel_base((uint16_t)G16(VA_g_map_geometry_selector)),
                                         g_os_sel_base((uint16_t)G16(VA_g_surface_record_selector)));  /* call 0x28972 */
    if ((uint16_t)G16(VA_g_surface_record_selector) != 0)
        render_world_face_list(0, (uint16_t)G16(VA_g_vertex_selector));  /* call 0x2ad21 */
    G16(VA_g_render_target_selector) = save_90c06;                               /* pop [0x90c06] */
    G32(VA_g_render_target_buffer) -= fdelta;                                  /* sub [0x85414],[0x89f2c] */
    G16(VA_g_floor_tex_caps + 0x44) = save_909f6;                               /* pop [0x909f6] */
    (void)signext_852f2_to_909a4();                   /* call 0x2ad14 */
    G16(VA_g_render_viewport_height) = save_90bf4;                               /* pop [0x90bf4] */
    G16(VA_g_init_stage_error_strings + 0x126) = save_71ef6; G16(VA_g_span_src_wrap_reoffset + 0x16) = save_71ef6;    /* pop -> [0x71ef6] AND [0x90992] */
    G16(VA_g_floor_tex_caps + 0x42) = 0;                                        /* [0x909f4]=0 */
    G16(VA_g_reflection_view_list + 0x84) = 0;                                        /* [0x853c8]=0 */
}

/* ================= render_reflection_subviews (0x284df) — mirror-plane reflection passes ================= */
/* Iterates [0x85340] reflection records (8-byte stride at 0x85344): reflects the view across the
 * mirror edge, re-transforms vertices, projects the mirror's two edge endpoints (idiv), clamps the
 * span to the viewport, builds a 1-sector list, and renders the reflected subscene + secondary
 * viewport. Heavy fs:/gs: geometry + idiv -> in-game differential is the gate. */
void render_reflection_subviews(void)
{
    uint32_t count = (uint32_t)G32(VA_g_reflection_view_count);                 /* eax=[0x85340] */
    if (count == 0) return;                                  /* or eax,eax; je 0x286de */
    #ifndef ROTH_STANDALONE
    if (g_os_sel_base == NULL) {                           /* oracle safety (never called there) */
        regs_t io; memset(&io, 0, sizeof io); io.va = 0x284dfu + OBJ_DELTA; call_orig(&io); return;  /* [ORACLE-FALLBACK] */
    }
    #endif
    G8(VA_g_render_sector_walk_mode) = 1;                                         /* [0x853d3]=1 */
    uint32_t edx = 0x85340 + 4;                              /* edx = record ptr (canon 0x85344) */
    do {                                                    /* loop top 0x28501 */
        G32(VA_g_current_decoded_frame + 0x8) = (int32_t)count;                      /* [0x8494c]=eax */
        G32(VA_g_anim_clock + 0x12) = (int32_t)edx;                        /* [0x8533c]=edx */
        uint16_t save_909f8 = (uint16_t)G16(VA_g_sprite_view_angle);       /* push [0x909f8] (restored last) */
        uint16_t save_90a06 = (uint16_t)G16(VA_g_view_offset_y);       /* push [0x90a06] */
        uint16_t save_90a04 = (uint16_t)G16(VA_g_view_offset_x);       /* push [0x90a04] */
        if (G16(edx + 4) != 0) {                            /* cmp word[edx+4],0; je skip */
            uint32_t fs_base = g_os_sel_base((uint16_t)G16(VA_g_surface_record_selector));
            uint32_t gs_base = g_os_sel_base((uint16_t)G16(VA_g_vertex_selector));
            #define FS16(o) (*(volatile uint16_t *)(uintptr_t)(fs_base + (uint16_t)(o)))
            #define GS32(o) (*(volatile int32_t  *)(uintptr_t)(gs_base + (uint16_t)(o)))
            uint32_t ebx = (uint16_t)G16(edx + 6);          /* bx = word[edx+6] */
            reflect_view_across_mirror_plane(ebx, fs_base, gs_base);  /* call 0x28456 */
            transform_world_vertices();              /* call 0x2a814 */
            edx = (uint32_t)G32(VA_g_anim_clock + 0x12);                   /* reload edx */
            ebx = (uint16_t)G16(edx + 6);                   /* bx = word[edx+6] */

            /* endpoint 1 -> esi_res */
            uint32_t edi1 = (uint16_t)FS16(ebx);            /* di = fs:[bx] */
            int32_t esi_res;
            int32_t depth1 = GS32(edi1 + 4);
            if (depth1 < 0x1000) {
                esi_res = (int32_t)(uint16_t)G16(VA_g_sector_cull_coord + 0x12);  /* sub esi,esi; mov si,[0x8530c] */
            } else {
                int64_t prod = (int64_t)(int32_t)GS32(edi1) * (int32_t)G32(VA_g_view_params_block + 0xc); /* imul dword[0x8527c] */
                int32_t r = (int32_t)(prod / depth1);       /* idiv ecx */
                r += G32(VA_g_span_src_wrap_reoffset + 0x24);                          /* add eax,[0x909a0] */
                if (r < -0x3ffe) r = -0x3ffe;               /* clamp [-0x3ffe,0x3ffe] */
                else if (r >= 0x3ffe) r = 0x3ffe;
                esi_res = r;
            }
            /* endpoint 2 -> eax_res */
            uint32_t edi2 = (uint16_t)FS16(ebx + 2);        /* di = fs:[bx+2] */
            int32_t eax_res;
            int32_t depth2 = GS32(edi2 + 4);
            if (depth2 < 0x1000) {
                eax_res = (int32_t)(uint16_t)G16(VA_g_sector_cull_coord + 0xe);  /* sub eax,eax; mov ax,[0x85308] */
            } else {
                int64_t prod = (int64_t)(int32_t)GS32(edi2) * (int32_t)G32(VA_g_view_params_block + 0xc);
                int32_t r = (int32_t)(prod / depth2);
                r += G32(VA_g_span_src_wrap_reoffset + 0x24);
                if (r < -0x3ffe) r = -0x3ffe;
                else if (r >= 0x3ffe) r = 0x3ffe;
                eax_res = r;
            }
            #undef FS16
            #undef GS32
            /* combine + clamp (16-bit si/ax vs the view X bounds) */
            int16_t si16 = (int16_t)esi_res;
            int16_t ax16 = (int16_t)eax_res;
            int16_t lo = (int16_t)G16(VA_g_sector_cull_coord + 0xe);
            int16_t hi = (int16_t)G16(VA_g_sector_cull_coord + 0x12);
            if (si16 >= lo && ax16 <= hi) {                 /* jl/jg 0x2869a guards */
                if (!(ax16 > lo)) ax16 = lo;                /* ax = max(ax,[0x85308]) */
                if (!(si16 < hi)) si16 = hi;                /* si = min(si,[0x8530c]) */
                edx = (uint32_t)G32(VA_g_anim_clock + 0x12);
                G32(VA_g_current_decoded_frame + 0xc) = (int32_t)edx;                /* [0x84950]=edx */
                G32(VA_g_visible_extent_count) = 1;                           /* [0x85220]=1 (one sector) */
                G16(VA_g_visible_extent_list + 0x4) = (uint16_t)si16;              /* [edi+4]=si (edi=0x85224) */
                G16(VA_g_visible_extent_list + 0x2) = (uint16_t)ax16;              /* [edi+2]=ax */
                G16(VA_g_visible_extent_list) = (uint16_t)G16(edx + 4);      /* [edi]=word[edx+4] */
                /* clear_es_record_field4 0x293a3: reads ONLY es:[4]/es:[esi-2], zeroes es:[rec+4]
                 * per record (disasm 0x293a3..0x293c9 — no other input regs). es=[0x852c8]; fs_base
                 * already resolved that selector's base above. Direct-C; oracle-neutral —
                 * this path is in-game-differential tier, never oracle-run (guaranteed non-NULL here
                 * past the 0x284df g_os_sel_base guard). */
                clear_es_record_field4((uint8_t *)(uintptr_t)fs_base);  /* call 0x293a3 */
                int32_t v308 = G32(VA_g_sector_cull_coord + 0xe);
                G16(VA_g_sector_cull_coord + 0x4) = (uint16_t)v308; G16(VA_g_view_bound_left) = (uint16_t)v308;
                G16(VA_g_view_bound_top) = (uint16_t)((uint32_t)v308 >> 16);
                int32_t v30c = G32(VA_g_sector_cull_coord + 0x12);
                G16(VA_g_sector_cull_coord + 0x2) = (uint16_t)v30c; G16(VA_g_view_bound_right) = (uint16_t)v30c;
                G16(VA_g_view_bound_bottom) = (uint16_t)((uint32_t)v30c >> 16);
                render_clipped_sector_subscene();    /* call 0x29305 */
                if ((uint16_t)((uint16_t)G16(VA_g_render_height) - (uint16_t)G16(VA_g_render_viewport_height)) != 0)
                    render_secondary_viewport_pass(); /* call 0x286df */
            }
        }
        G8(VA_g_render_x_flip_flag) = 0;                                    /* [0x8a356]=0 (0x2869a) */
        G16(VA_g_sprite_view_angle) = save_909f8;                          /* pop -> [0x909f8] */
        G16(VA_g_view_offset_y) = save_90a06;                          /* pop -> [0x90a06] */
        G16(VA_g_view_offset_x) = save_90a04;                          /* pop -> [0x90a04] */
        edx = (uint32_t)G32(VA_g_anim_clock + 0x12) + 8;                   /* edx=[0x8533c]+8 */
        count--;                                            /* dec eax */
    } while ((int32_t)count > 0);                           /* jg (signed) */
    G8(VA_g_render_sector_walk_mode) = 0;                                        /* [0x853d3]=0 */
    G32(VA_g_reflection_view_count) = 0;                                       /* [0x85340]=0 */
}

/* ================= render_scene_body (0x2885d) — the main + reflection + primary orchestrator ================= */
/* Commits the view/camera params, computes the view-space half-extents, runs the reflection
 * subviews, re-inits the geom block, transforms vertices, then the primary forward view. */
void render_scene_body(void)
{
    apply_view_camera_params();                     /* call 0x2a952 */
    G16(VA_g_reflection_view_list + 0x84) = 0;                                       /* sub eax,eax; [0x853c8]=ax */
    int16_t d1 = (int16_t)((uint16_t)G16(VA_g_view_bound_bottom) - (uint16_t)G16(VA_g_view_bound_top));
    G16(VA_g_current_decoded_frame + 0x1e) = (uint16_t)(d1 >> 1);                    /* [0x84962]=(( [0x9096c]-[0x9096e] )>>1) */
    uint16_t d2 = (uint16_t)((uint16_t)G16(VA_g_view_bound_right) - (uint16_t)G16(VA_g_view_bound_left));
    G32(VA_g_current_decoded_frame + 0x10) = (int32_t)(uint32_t)d2;                  /* [0x84954]=eax (zero-ext d2) */
    G16(VA_g_current_decoded_frame + 0x1c) = (uint16_t)((int16_t)d2 >> 1);           /* sar ax,1; [0x84960]=ax */
    uint32_t eax = (uint32_t)(uint16_t)G16(VA_g_viewport_top_margin);
    int32_t edx = G32(VA_g_subpass_persp_step + 0x4);
    if (G8(VA_g_render_double_scanline_flag) != 0) edx >>= 1;                       /* sar edx,1 */
    eax = (uint32_t)((int32_t)eax - edx);                  /* sub eax,edx */
    G16(VA_g_floor_tex_caps + 0x44) = (uint16_t)eax;                          /* [0x909f6]=ax */
    render_reflection_subviews();                  /* call 0x284df */
    G32(VA_g_current_decoded_frame + 0xc) = 0;                                      /* [0x84950]=0 */
    init_84964_block();                             /* call 0x289bf */
    if ((uint16_t)G16(VA_g_surface_record_selector) != 0) {                    /* cmp [0x852c8],0; je 0x288f4 */
        transform_world_vertices();                /* call 0x2a814 */
        if (g_os_sel_base)                              /* es=[0x852c8]; call 0x293a3 (direct-C) */
            clear_es_record_field4((uint8_t *)(uintptr_t)g_os_sel_base((uint16_t)G16(VA_g_surface_record_selector)));
        render_primary_scene_view();               /* call 0x292be */
    }
}

/* ================= render_world_view_pass (0x287b6) — the per-frame render entry ================= */
/* EAX = frame-context record. Runs setup_frame_render_context, the optional overlay callback
 * [0x8495c], render_scene_body, then (if the split-viewport height differs) the lower split pass. */
void render_world_view_pass(uint32_t eax_record)
{
    G8(VA_g_render_sector_walk_mode + 0x1) = 0;                                       /* [0x853d4]=0 */
    G8(VA_g_render_active) = 0xff;                                    /* [0x89f38]=0xff */
    setup_frame_render_context(eax_record);        /* call 0x2aa3e (eax=record) */
    if (G32(VA_g_current_decoded_frame + 0x18) != 0) {                              /* cmp [0x8495c],0; je 0x287dd */
        regs_t io; memset(&io, 0, sizeof io);            /* call dword[0x8495c] (overlay hook, bridge) */
        io.va = (uint32_t)G32(VA_g_current_decoded_frame + 0x18);
#ifndef ROTH_STANDALONE
        call_orig(&io);
#else
        /* CONSUMED code-ptr token: dispatch
         * the known installed values to their lifted bodies. load_icons_all stores GADDR(0x15ee2)
         * = relocate_moving_objects_if_dirty; EAX-out feeds [0x84958] like the original call. */
        if (io.va == (uint32_t)GADDR(0x15ee2))
            io.eax = relocate_moving_objects_if_dirty();
        else
            roth_unreachable(io.va - OBJ_DELTA);        /* unknown installed overlay hook — render tier */
#endif
        G32(VA_g_current_decoded_frame + 0x14) = (int32_t)io.eax;                  /* [0x84958]=eax */
    }
    if (G16(VA_g_subpass_patch_gate) == 0)                                /* cmp word[0x90bfe],0; jne 0x287ed */
        G8(VA_g_render_sector_walk_mode + 0x1) = (uint8_t)(G8(VA_g_render_sector_walk_mode + 0x1) + 1);        /* inc byte[0x853d4] */
    render_scene_body();                           /* call 0x2885d */
    uint16_t diff = (uint16_t)((uint16_t)G16(VA_g_render_height) - (uint16_t)G16(VA_g_render_viewport_height));
    if (diff != 0) {                                     /* sub ax; je 0x28852 */
        G8(VA_g_render_sector_walk_mode + 0x1) = (uint8_t)(G8(VA_g_render_sector_walk_mode + 0x1) - 1);        /* dec byte[0x853d4] */
        uint16_t save_71ef6 = (uint16_t)G16(VA_g_init_stage_error_strings + 0x126);    /* push [0x71ef6] */
        uint16_t bx = (uint16_t)G16(VA_g_render_viewport_height);            /* push [0x90bf4]; bx=[0x90bf4] */
        uint16_t save_90bf4 = bx;
        G16(VA_g_init_stage_error_strings + 0x126) = (uint16_t)(G16(VA_g_init_stage_error_strings + 0x126) - bx);    /* sub [0x71ef6],bx */
        G16(VA_g_span_src_wrap_reoffset + 0x16) = (uint16_t)(G16(VA_g_span_src_wrap_reoffset + 0x16) - bx);    /* sub [0x90992],bx */
        uint16_t axv = (uint16_t)(G16(VA_g_init_stage_error_strings + 0x11e) - bx);   /* ax=[0x71eee]-bx */
        G16(VA_g_reflection_view_list + 0x84) = bx;                               /* [0x853c8]=bx */
        set_909a4_save_old_to_852f2(axv);        /* call 0x2acfc */
        render_split_viewport_lower();           /* call 0x288fb */
        (void)signext_852f2_to_909a4();          /* call 0x2ad14 */
        G16(VA_g_render_viewport_height) = save_90bf4;                       /* pop -> [0x90bf4] */
        G16(VA_g_init_stage_error_strings + 0x126) = save_71ef6;                       /* pop -> [0x71ef6] */
    }
    G32(VA_g_current_decoded_frame + 0x14) = 0;                                    /* mov dword[0x84958],0 */
}

/* ================= select_surface_anim_frame (0x2b5ea) — pick + decode a surface's anim frame ================= */
/* eax = anim-control record (raw ptr, [0x90a44]); esi = surface record (raw). Two flagged paths (the record's
 * flag byte +9 bit 1 -> table 0x91e03, bit 2 -> table 0x90fe3) compute a frame index from the per-entry counter
 * (+0x1a) / the surface frame count ([esi+0x14]); otherwise a default index from the global clock [0x8532a].
 * decode_das_anim_frame (0x2c5c5) resolves the frame ptr -> ESI (direct C: lifted proto extended to return EAX);
 * a cache-slot miss ([0x84944] != frame low8, NON-DETERMINISTIC) re-marks the entry 0xffff; a moved cache block
 * ([esi-8]&4) refreshes via 0x41250 (register-transparent). Returns the frame ptr in ESI. */
static uint32_t anim_decode_frame(uint16_t frame, uint32_t esi_surf)   /* call 0x2c5c5 -> ESI = EAX return */
{
    /* Direct C: the lifted proto was extended to return EAX = the resolved
     * frame ptr [[rec+0x10]] that THIS caller needs (-> ESI). Both original exit paths (0x2c70c
     * no-op, 0x2c704 main) return that ptr in EAX; oracle-gated in test_das_assets (oeax==ceax).
     * Render-path is in-game differential — verify in the render session (ROTH_LIFT=render_world). */
    return decode_das_anim_frame(frame, esi_surf);
}
static uint32_t anim_flagged_path(uint32_t table_canon, uint32_t esi_surf)
{
    uint32_t frame = (uint16_t)G16(table_canon + 0x1a);            /* movzx word[ebx+0x1a] */
    uint16_t fc = *(volatile uint16_t *)(uintptr_t)(esi_surf + 0x14);  /* bx=[esi+0x14] */
    if (fc > 1) frame = frame / fc;                               /* cmp bx,1; jbe skip; div bx */
    uint32_t esi = anim_decode_frame((uint16_t)frame, esi_surf);
    if (G8(VA_g_current_decoded_frame) != (uint8_t)frame)                            /* cmp [0x84944],al; jne */
        G16(table_canon + 0x1a) = 0xffff;                         /* [ebx+0x1a]=0xffff (cache miss) */
    return esi;
}
static uint32_t anim_default_path(uint32_t esi_surf)
{
    uint32_t v = (uint32_t)((uint16_t)G16(VA_g_anim_clock)) & 0x7fffu;    /* ax=[0x8532a]; and eax,0x7fff */
    uint16_t d1 = *(volatile uint16_t *)(uintptr_t)(esi_surf + 0x14);
    uint16_t d2 = *(volatile uint16_t *)(uintptr_t)(esi_surf + 0x22);
    uint16_t q1 = (uint16_t)(v / d1);                             /* div word[esi+0x14] -> quotient */
    uint16_t frame = (uint16_t)(q1 % d2);                         /* div word[esi+0x22] -> remainder */
    return anim_decode_frame(frame, esi_surf);
}
uint32_t select_surface_anim_frame(uint32_t eax_rec, uint32_t esi_surf)
{
    uint32_t esi;
    if (eax_rec != 0) {                                           /* or eax,eax; je default */
        volatile uint8_t *rec = (volatile uint8_t *)(uintptr_t)eax_rec;
        uint8_t f9 = *(volatile uint8_t *)(rec + 9);
        if (f9 & 1) {                                            /* test byte[eax+9],1 */
            uint16_t idx = *(volatile uint16_t *)(rec + 0xc);
            if (idx != 0 && (G8((VA_g_state_pool_a_count + 0x3) + idx + 8) & 0x6c))       /* je default; test byte[eax+8],0x6c */
                esi = anim_flagged_path(0x91e03 + idx, esi_surf);
            else esi = anim_default_path(esi_surf);
        } else if (f9 & 2) {                                     /* 0x2b64a: test byte[eax+9],2 */
            uint16_t idx = *(volatile uint16_t *)(rec + 0xc);
            if (idx != 0 && (G8((VA_g_dynamic_entity_count + 0x3) + idx + 8) & 2))          /* test byte[eax+8],2 */
                esi = anim_flagged_path(0x90fe3 + idx, esi_surf);
            else esi = anim_default_path(esi_surf);
        } else {
            esi = anim_default_path(esi_surf);
        }
    } else {
        esi = anim_default_path(esi_surf);
    }
    if (*(volatile uint8_t *)(uintptr_t)(esi - 8) & 4) {         /* test byte[esi-8],4; jne */
        /* Direct C: 0x41250 is fully register-transparent — disasm shows push
         * eax/ebx/ecx/edx/edi/esi at entry and pop-restore of all six before every ret, so ESI is
         * input-only and preserved (the caller keeps its own esi). The lifted body is oracle-verified
         * in test_das_assets.c (t_refresh_moved: plain/grouped/internal_init/internal_delta all green;
         * its inner 0x412ed DPMI selector refresh is bridged and stubbed symmetrically both sides).
         * Conversion is oracle-neutral AND correct-by-construction. Render-path stays in-game
         * differential — spot-check in ROTH_LIFT=render_world. */
        refresh_moved_das_cache_block(esi);                 /* was call_orig 0x41250 */
    }
    return esi;
}

/* ================= shutdown_render_subsystem (0x2fcd4) — renderer teardown ================= */
/* Frees the render color selectors (DPMI, bridged), the framebuffer surface, the resource buffers,
 * and closes the voice file. Non-idempotent teardown -> in-game (fires on level unload). */
void shutdown_render_subsystem(void)
{
#ifndef ROTH_STANDALONE
    { regs_t io; memset(&io, 0, sizeof io); io.va = 0x2f9b0u + OBJ_DELTA; call_orig(&io); } /* free_render_color_selectors (DPMI) */
#else
    /* image-free boot: free_render_color_selectors 0x2f9b0
     * frees the 5 DPMI color selectors (shade/blend/pal) + the DOS palette block + the worker heap
     * block. Under the flat imgfree model the selectors are host-flat (g_os_sel_base), so the DPMI
     * frees are moot; this teardown fires only on level-unload/quit (OFF the boot-to-title path), so
     * the heap/DOS-block leak is quit-only and harmless. Compiled out; the surface/buffer/voice frees
     * below stay. */
#endif
    free_framebuffer_surface();                   /* call 0x2fce9 */
    free_resource_buffers();                      /* call 0x26c8c */
    close_voice_file();                           /* call 0x1e774 */
}

/* ================= free_scene_geometry_buffers (0x2a93c) — geometry buffer teardown ================= */
/* Frees the geometry-arena DPMI selector [0x85294] (bridged) + the geometry heap block [0x8498c]. */
void free_scene_geometry_buffers(void)
{
    { uint16_t s = (uint16_t)G16(VA_g_map_geometry_selector); if (s) os_dpmi_free_descriptor(s); }  /* was bridge 0x2f772 free_dpmi_selector (int31 0001); 0-guard preserved */
    game_free_if_not_null((uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_door_worklist));  /* call 0x40a2a */
}

/* ============= free_geometry_buffer_and_selector (0x40bc7) — main-sequence teardown leaf ============= */
/* EAX = buffer (roth_main_sequence passes g_image_surface [0x90a9c]): game_free_if_not_null(EAX) when
 * non-null (0x40bc7 `or eax,eax; je`), then release_map_geometry_selector 0x2a8e9: when the geometry
 * selector [0x85294] is set, 0x40adf (free_selector_and_heap_block) frees BOTH the descriptor AND its
 * base memory — DPMI 0006 get-segment-base -> game_heap_free(base) -> DPMI 0001 free-descriptor; a
 * get-base failure (0x40aef jb) skips BOTH frees. [0x85294] is zeroed regardless (0x2a8ff). The base is
 * the alloc_scene_render_arena mint (0x2a909: heap block + 0x8000 -> alloc_dpmi_selector 0x2f72a), so
 * under the imgfree lane g_os_sel_base (dpmi_sel_base — the SAME LDT the int31 seam minted from)
 * resolves it and the frees ride the c2 contract, per the free_scene_geometry_buffers idiom above.
 * Quit-path teardown (Quit-to-DOS stop); disasm-faithful, both callees lifted. */
void free_geometry_buffer_and_selector(uint32_t buf)
{
    if (buf != 0)                                                     /* 0x40bc7 or eax,eax; je 0x40bd0 */
        game_free_if_not_null((uint8_t *)(uintptr_t)buf);      /* 0x40bcb call 0x40a2a */
    uint16_t sel = (uint16_t)G16(VA_g_map_geometry_selector);                            /* 0x2a8e9/0x2a8ef guard */
    if (sel != 0) {
        uint32_t base = g_os_sel_base ? g_os_sel_base(sel) : 0;   /* 0x40adf int31 0006 get base (CX:DX);
                                                                       *   0/invalid == the original's CF fail */
        if (base != 0) {                                              /* 0x40aef jb 0x40b05: skip both frees */
            game_heap_free((uint8_t *)(uintptr_t)base);        /* 0x40afa call 0x15191 */
            os_dpmi_free_descriptor(sel);                             /* 0x40aff int31 0001 (bx=sel) */
        }
        G16(VA_g_map_geometry_selector) = 0;                                             /* 0x2a8ff — stored regardless of 0x40adf's path */
    }
}

/* ================= load_backdrop_image (0x4b08c) — open + decode a backdrop .RAW ================= */
/* EAX = filename, EDX = descriptor record, EBX = dest buffer, ECX = dest capacity. Opens the file (int21
 * 0x3d), reads the 8-byte header (int21 0x3f), builds a local 0x64-byte frame descriptor, computes the
 * horizontal scale, then the two lifted decode helpers do the work: build_backdrop_hscale_table 0x4b322
 * (if scaled) + blit_backdrop_rows 0x4b1b7 (reads the pixel rows via its own soft-int). Closes the file
 * (int21 0x3e). Returns EAX = an error code (0 ok / 1 open-fail / 2 read-fail / 5 buffer-fail). DOS-I/O ->
 * in-game (validated when a level with a backdrop loads). */
static int bd_int21_open(uint32_t fname, uint32_t *handle_out)
{
    regs_t r; memset(&r, 0, sizeof r);
    r.eax = 0x3d00; r.edx = fname; r.ecx = 0;
    uint32_t fl = g_os_soft_int ? g_os_soft_int(0x21, &r) : 1u;
    *handle_out = r.eax;
    return (int)(fl & 1);
}
static int bd_int21_read(uint32_t handle, uint32_t buf, uint32_t count, uint32_t *got)
{
    regs_t r; memset(&r, 0, sizeof r);
    r.eax = 0x3f00; r.ebx = handle; r.edx = buf; r.ecx = count;
    uint32_t fl = g_os_soft_int ? g_os_soft_int(0x21, &r) : 1u;
    *got = r.eax;
    return (int)(fl & 1);
}
static void bd_int21_close(uint32_t handle)
{
    regs_t r; memset(&r, 0, sizeof r);
    r.eax = 0x3e00; r.ebx = handle;
    if (g_os_soft_int) g_os_soft_int(0x21, &r);
}
uint32_t load_backdrop_image(uint32_t eax_fname, uint32_t edx_desc, uint32_t ebx_dest, uint32_t ecx_cap)
{
    uint8_t fr[0x64];
    memset(fr, 0, sizeof fr);
    #define F32(o) (*(volatile uint32_t *)(fr + (o)))
    #define F16(o) (*(volatile uint16_t *)(fr + (o)))
    volatile uint8_t *desc = (volatile uint8_t *)(uintptr_t)edx_desc;   /* edi = descriptor */
    F32(0x14) = edx_desc;
    F32(0x40) = (uint32_t)*(volatile uint16_t *)(desc + 8);
    F32(0x44) = (uint32_t)*(volatile uint16_t *)(desc + 0xa);
    F32(0x48) = (uint32_t)*(volatile uint16_t *)(desc + 0xc);
    F16(0x60) = *(volatile uint16_t *)(desc + 0xe);
    F32(0x1c) = ebx_dest;
    F32(0x24) = ecx_cap;
    uint32_t err;
    uint32_t handle = 0;
    if (bd_int21_open(eax_fname, &handle)) { err = 1; goto done; }     /* int21 0x3d */
    F32(0) = handle;
    uint32_t got = 0;
    if (bd_int21_read(handle, (uint32_t)(uintptr_t)(fr + 4), 8, &got)) { err = 2; goto done; }  /* int21 0x3f */
    if (got != 8) { err = 2; goto done; }                             /* short read */
    F16(0x1a) = F16(0xa);                                             /* header height */
    uint32_t width = F16(8);                                          /* header width -> edi */
    F16(0x18) = (uint16_t)width;
    F32(0x58) = 0x10000;                                             /* default hscale */
    uint32_t eax_v = 0x1408;
    if (F16(0x60) & 1) {                                             /* test word[+0x60],1 */
        uint32_t hs = (((uint32_t)F16(0x1a) << 16) - 0x7fff) / F32(0x44);
        F32(0x58) = hs;                                             /* eax restored to 0x1408 (push/pop) */
        if (width != F32(0x40)) {                                   /* cmp edi,[+0x40] */
            F32(0x50) += 1;
            eax_v = 0x1408 + F32(0x40) + F32(0x40);
        }
    }
    uint32_t ecx = F32(0x24);
    if (F32(0x1c) == 0) { err = 5; goto done; }                      /* no dest */
    if (eax_v > ecx) { err = 5; goto done; }                         /* cmp eax,ecx; jbe cont else err */
    uint32_t eax2 = F32(0x1c);
    F32(0x28) = eax2;
    eax2 += 0x1000;
    if (F32(0x50) != 0) {                                            /* scaled */
        F32(0x50) = eax2;
        build_backdrop_hscale_table((uint32_t)(uintptr_t)fr);  /* call 0x4b322 */
        eax2 += F32(0x40); eax2 += F32(0x40);
    }
    F32(0x2c) = eax2;
    eax2 -= F32(0x1c);
    ecx -= eax2;
    ecx &= ~3u;                                                     /* and cl,0xfc */
    F32(0x30) = ecx;
    blit_backdrop_rows((uint32_t)(uintptr_t)fr);              /* call 0x4b1b7 */
    err = 0;
done:
    if (F32(0) != 0) bd_int21_close(F32(0));                         /* int21 0x3e close */
    return err;
    #undef F32
    #undef F16
}

/* ================= load_backdrop_raw (0x16164) — load the level backdrop ================= */
/* No args (all from globals). Marks the whole screen dirty (add_dirty_rect), builds the backdrop file
 * path (build_game_path), reserves cache heap (ensure_das_cache_heap_space), allocates a pool handle,
 * loads+decodes the image into it (load_backdrop_image), then frees the handle. All callees are lifted C. */
void load_backdrop_raw(void)
{
    uint8_t desc[0x18]; memset(desc, 0, sizeof desc);            /* the [ebp+0x70] descriptor */
    uint8_t path[0x80]; memset(path, 0, sizeof path);            /* the [ebp-8] path buffer */
    *(uint32_t *)(desc + 0)   = (uint32_t)G32(VA_g_framebuffer_ptr);          /* [ebp+0x70]=[0x90a98] */
    G32(VA_g_dirty_rect_count) = 0;                                            /* [0x7f57c]=0 */
    *(uint16_t *)(desc + 8)   = (uint16_t)G32(VA_g_screen_pitch);          /* [ebp+0x78]=word[0x85498] */
    *(uint16_t *)(desc + 0xa) = (uint16_t)G32(VA_g_screen_height);          /* [ebp+0x7a]=word[0x854a0] */
    *(uint16_t *)(desc + 0xc) = (uint16_t)G32(VA_g_screen_pitch);          /* [ebp+0x7c]=word[0x85498] */
    *(uint16_t *)(desc + 0xe) = 1;                               /* [ebp+0x7e]=1 */
    G32(VA_g_prev_dirty_rect_count) = 0;                                            /* [0x7f980]=0 */
    add_dirty_rect(0, 0, (uint32_t)G32(VA_g_screen_pitch), (uint32_t)G32(VA_g_screen_height));   /* call 0x15b69 */
    build_game_path(path, (const uint8_t *)(uintptr_t)GADDR(VA_g_dir_data),
                           (const uint8_t *)(uintptr_t)GADDR(VA_g_heap_free_list + 0x7cb));            /* call 0x2fb7f */
    ensure_das_cache_heap_space(0xfac0);                                    /* call 0x414d2 */
    uint32_t handle = pool_alloc_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle), 0xfac0);  /* 0x360f9 */
    if (handle != 0) {                                          /* test eax; je 0x1621a */
        uint32_t buf = *(volatile uint32_t *)(uintptr_t)handle;   /* ebx = [eax] (handle -> buffer) */
        (void)load_backdrop_image((uint32_t)(uintptr_t)path, (uint32_t)(uintptr_t)desc, buf, 0xfac0);  /* 0x4b08c */
        pool_free_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle),
                                (uint32_t *)(uintptr_t)handle);                    /* call 0x360b3 */
    }
    G8(VA_g_inventory_dirty_flags) = 0;                                            /* [0x7f571]=0 */
}

/* ================= project_sprite_to_render_queue (0x3c2bd) — sprite vertex projection ================= */
/* Rotates the sprite origin by the view angle (floorceil_rotation_sincos), then per vertex: rotates by
 * the sprite's own angle (inline sincos of angle-[0x8b2dc]), perspective-divides X/Y (16-bit imul/idiv by
 * depth), writes the projected coords back into the vertex record, and accumulates a screen bbox
 * [0x8b34c..0x8b352]. Returns CF: 0 = visible (bbox inside the frustum), 1 = culled. esi = vertex list. */
uint32_t project_sprite_to_render_queue(uint32_t eax, uint32_t ecx, uint32_t ebx, uint32_t edx, uint32_t esi)
{
    G32(VA_g_sprite_node_pool + 0x80c) = (int32_t)eax;                          /* [0x8b2dc]=eax */
    G32(VA_g_sprite_node_pool + 0x804) = (int32_t)ecx;                          /* [0x8b2d4]=ecx */
    ebx += (uint32_t)(int32_t)(int16_t)G16(VA_g_view_offset_x);      /* movsx [0x90a04]; add ebx,eax */
    edx += (uint32_t)(int32_t)(int16_t)G16(VA_g_view_offset_y);      /* movsx [0x90a06]; add edx,eax */

    int32_t pt[3];                                        /* rotate the sprite origin by the view angle */
    pt[0] = (int32_t)edx; pt[1] = (int32_t)ebx;
    pt[2] = (int32_t)(uint16_t)G16(VA_g_sprite_view_angle);
    floorceil_rotation_sincos(pt);                 /* call 0x3bdf3 */
    edx = (uint32_t)pt[0]; ebx = (uint32_t)pt[1];         /* pop edx; pop ebx */
    G32(VA_g_sprite_node_pool + 0x800) = (int32_t)ebx;                          /* [0x8b2d0]=ebx */
    G32(VA_g_sprite_node_pool + 0x808) = (int32_t)edx;                          /* [0x8b2d8]=edx */

    G16(VA_g_sprite_render_queue_head + 0xc) = 0x7fff; G16(VA_g_sprite_render_queue_head + 0xe) = 0x7fff;         /* bbox min = +inf */
    G16(VA_g_sprite_render_queue_head + 0x10) = 0x8000; G16(VA_g_sprite_render_queue_head + 0x12) = 0x8000;         /* bbox max = -inf */

    volatile uint8_t *sp = (volatile uint8_t *)(uintptr_t)esi;
    uint32_t vcount = (uint16_t)*(volatile uint16_t *)(sp);   /* movzx word[esi] */
    sp += 2;                                              /* esi += 2 */
    if (vcount == 0) return 1;                            /* je 0x3c475 -> stc; ret */

    int16_t ang = (int16_t)((uint16_t)G16(VA_g_sprite_view_angle) - (uint16_t)G32(VA_g_sprite_node_pool + 0x80c));  /* ax=[0x909f8]-[0x8b2dc]; cwde */
    uint32_t bsin = (uint32_t)((uint32_t)ang & 0x1ff) * 2u;   /* ebx = (ang&0x1ff)*2 */
    G32(VA_g_atan_table + 0x82) = (int32_t)(int16_t)G16(VA_g_sincos_table + bsin);     /* [0x8aac8]=sin */
    uint32_t bcos = (bsin + 0x100u) & 0x3ffu;                 /* inc bh; and bh,3 */
    G32(VA_g_atan_table + 0x86) = (int32_t)(int16_t)G16(VA_g_sincos_table + bcos);     /* [0x8aacc]=cos */

    uint32_t cnt = vcount;
    do {                                                  /* loop top 0x3c362 */
        int16_t v8 = (int16_t)((uint16_t)*(volatile uint16_t *)(sp + 2) + (uint16_t)G32(VA_g_sprite_node_pool + 0x804));
        *(volatile uint16_t *)(sp + 8) = (uint16_t)v8;    /* [esi+8]=ax */
        int32_t vx = (int32_t)(int16_t)*(volatile uint16_t *)(sp);       /* movsx [esi] */
        int32_t vy = (int32_t)(int16_t)*(volatile uint16_t *)(sp + 4);   /* movsx [esi+4] */
        int32_t sinv = G32(VA_g_atan_table + 0x82), cosv = G32(VA_g_atan_table + 0x86);
        int32_t rx = (int32_t)((vx * cosv - vy * sinv) >> 14);           /* eax = x*cos - y*sin; sar 0xe */
        int32_t ry = (int32_t)((vx * sinv + vy * cosv) >> 14);           /* ebx = x*sin + y*cos; sar 0xe */
        ry += G32(VA_g_sprite_node_pool + 0x808);                               /* add ebx,[0x8b2d8] */
        rx += G32(VA_g_sprite_node_pool + 0x800);                               /* add eax,[0x8b2d0] */
        *(volatile uint16_t *)(sp + 6) = (uint16_t)rx;    /* [esi+6]=ax */
        int16_t depth = (int16_t)ry;
        *(volatile uint16_t *)(sp + 0xa) = (uint16_t)depth;  /* [esi+0xa]=bx */
        if (depth >= 0x10) {                              /* cmp bx,0x10; jl 0x3c426 */
            int32_t px = (int32_t)(int16_t)rx * (int32_t)(int16_t)G16(VA_g_span_src_wrap_reoffset + 0x20);  /* imul word[0x9099c] */
            int16_t sx = (int16_t)((int16_t)(px / depth) + (int16_t)G16(VA_g_span_src_wrap_reoffset + 0x24)); /* idiv bx; add [0x909a0] */
            *(volatile uint16_t *)(sp + 0xc) = (uint16_t)sx;   /* [esi+0xc]=ax (screen X) */
            if (!(sx > (int16_t)G16(VA_g_sprite_render_queue_head + 0xc))) G16(VA_g_sprite_render_queue_head + 0xc) = (uint16_t)sx;  /* min X */
            if (!(sx < (int16_t)G16(VA_g_sprite_render_queue_head + 0x10))) G16(VA_g_sprite_render_queue_head + 0x10) = (uint16_t)sx;  /* max X */
            int16_t ny = (int16_t)(-(int16_t)v8);          /* ax=[esi+8]; neg ax */
            int32_t py = (int32_t)ny * (int32_t)(int16_t)G16(VA_g_span_src_wrap_reoffset + 0x1c);      /* imul word[0x90998] */
            int16_t sy = (int16_t)((int16_t)(py / depth) + (int16_t)G16(VA_g_span_src_wrap_reoffset + 0x28)); /* idiv bx; add [0x909a4] */
            *(volatile uint16_t *)(sp + 0xe) = (uint16_t)sy;   /* [esi+0xe]=ax (screen Y) */
            if (!(sy > (int16_t)G16(VA_g_sprite_render_queue_head + 0xe))) G16(VA_g_sprite_render_queue_head + 0xe) = (uint16_t)sy;  /* min Y */
            if (!(sy < (int16_t)G16(VA_g_sprite_render_queue_head + 0x12))) G16(VA_g_sprite_render_queue_head + 0x12) = (uint16_t)sy;  /* max Y */
        }
        sp += 0x10;                                       /* esi += 0x10 */
        cnt--;                                            /* dec ecx */
    } while ((int32_t)cnt > 0);                           /* jg 0x3c362 */

    int16_t minx = (int16_t)G16(VA_g_sprite_render_queue_head + 0xc);                 /* frustum cull */
    if ((uint16_t)minx == 0x8000) return 1;               /* cmp ax,0x8000; je fail */
    if (minx >= (int16_t)G16(VA_g_view_bound_right)) return 1;          /* jge fail */
    if ((int16_t)G16(VA_g_sprite_render_queue_head + 0x10) <= (int16_t)G16(VA_g_view_bound_left)) return 1;  /* jle fail */
    if ((int16_t)G16(VA_g_sprite_render_queue_head + 0xe) >= (int16_t)G16(VA_g_view_bound_bottom)) return 1;  /* jge fail */
    if ((int16_t)G16(VA_g_sprite_render_queue_head + 0x12) <= (int16_t)G16(VA_g_view_bound_top)) return 1;  /* jle fail */
    return 0;                                             /* clc; ret (visible) */
}

/* ================= render_world_sprite (0x36651) — per-sprite rasterizer setup ================= */
/* Extracts the sprite's screen-space setup from its record (esi) into the rasterizer globals
 * ([0x90a26]=0xff sprite marker + the 5 span-extent globals + the shade byte), then runs the SHARED
 * SMC rasterizer body from 0x366d2 (bridged — it's the tail of the already-lifted 0x366cb, entered
 * past its [0x90a26]=0). If the record has no drawable sprite ([esi+0x34]==0) it delegates to the
 * 0x2b6c8 seed block into the rwss shared tail — render the record as a secondary surface (trap lane
 * bridged; imgfree lane native via rwss_sprite_side_entry). Return unused (the queue caller
 * reloads esi). */
void render_world_sprite(uint32_t esi_rec, uint16_t es, uint16_t fs, uint16_t gs)
{
    volatile uint8_t *rec = (volatile uint8_t *)(uintptr_t)esi_rec;
    if (*(volatile uint8_t *)(rec + 0x34) == 0) {              /* cmp byte[esi+0x34],0; je 0x2b6c8 */
#ifndef ROTH_STANDALONE
        regs_t io; memset(&io, 0, sizeof io); io.va = 0x2b6c8u + OBJ_DELTA;
        io.esi = esi_rec; io.es = es; io.fs = fs; io.gs = gs;
        call_orig(&io);                                        /* -> secondary-surface resolver */
#else
        /* 0x2b6c8 is a 70-B data-seed block that tail-jumps into the rwss shared tail at the
         * resolver — NATIVE via rwss_sprite_side_entry
         * (renderer.c; imgfree lane only, the trap lane keeps the byte-faithful bridge above). */
        rwss_sprite_side_entry(esi_rec, es, fs, gs);
#endif
        return;
    }
    G8(VA_g_sprite_fill_index + 0x2) = 0xff;                                        /* sprite marker */
    uint32_t vp = *(volatile uint32_t *)(rec + 0x30) + (uint16_t)*(volatile uint16_t *)(rec + 0x36);
    volatile uint8_t *v = (volatile uint8_t *)(uintptr_t)vp;   /* vertex record A (esi.30 + esi.36) */
    G16(VA_g_sprite_view_angle + 0x4) = *(volatile uint16_t *)(v + 2);
    uint16_t a8 = *(volatile uint16_t *)(v + 8);
    G16(VA_g_sprite_view_angle + 0x6) = a8;
    G16(VA_g_span_src_wrap_reoffset + 0xa) = (uint16_t)(-(int16_t)a8);                   /* neg (low 16) */
    G16(VA_g_sprite_view_angle + 0x8) = *(volatile uint16_t *)(v + 0xa);
    uint16_t a6 = *(volatile uint16_t *)(v + 6);
    uint32_t vp2 = *(volatile uint32_t *)(rec + 0x30) + (uint16_t)*(volatile uint16_t *)(rec + 0x38);
    a6 = (uint16_t)(a6 - *(volatile uint16_t *)((uintptr_t)vp2 + 6));  /* vertex record B */
    G16(VA_g_span_src_wrap_reoffset + 0x12) = a6;
    uint8_t sh = *(volatile uint8_t *)(rec + 0xe);
    if (sh != 0) {                                             /* test al,al; je 0x366bf */
        uint8_t g = G8(VA_g_span_clip_source);
        if (g != 0) g -= 0x80;                                 /* je 0x366bc; sub al,0x80 */
        sh = (uint8_t)(g + *(volatile uint8_t *)(rec + 0xe));  /* add al,[esi+0xe] */
    }
    G8(VA_g_column_clip_mode) = sh;
#ifndef ROTH_STANDALONE
    regs_t io; memset(&io, 0, sizeof io); io.va = 0x366d2u + OBJ_DELTA;  /* jmp 0x366d2 -> rasterizer body */
    io.esi = esi_rec; io.es = es; io.fs = fs; io.gs = gs;
    call_orig(&io);
#else
    rwss_sprite_body_entry(esi_rec, es, fs, gs);   /* 0x366d2 mid-entry: rasterizer body, prologue clear skipped */
#endif
}

/* ================= draw_sprite_render_queue (0x3b1b1) — render the sprite queue ================= */
/* Walks the linked list of sprite records (esi -> [esi]), rendering each via render_world_sprite. */
void draw_sprite_render_queue(uint32_t esi, uint16_t es, uint16_t fs, uint16_t gs)
{
    while (esi != 0) {                                         /* or esi,esi; je 0x3b1c0 */
        render_world_sprite(esi, es, fs, gs);           /* push esi; call 0x36651; pop esi */
        esi = *(volatile uint32_t *)(uintptr_t)esi;            /* esi = [esi] (next) */
    }
}

/* ================= invoke_span_callback (0x39093) — optional per-span hook dispatch ================= */
/* EBX = span arg, ESI = record. If a callback is installed at [0x90a34], invoke it with EAX=cwde(BX),
 * EDX=ESI, ES=DS. The hook is normally unset (pure-C early return); the rare installed path defers to
 * the original bytes so the ES=DS setup + indirect call stay byte-faithful. */
void invoke_span_callback(uint32_t ebx, uint32_t esi)
{
    if (G32(VA_g_span_callback) == 0) return;                       /* mov ebx,[0x90a34]; test; je ret */
    /* SELF-REFERENTIAL faithful bridge (native-un-reproducible):
     * the installed-hook path (rare, but fires in real gameplay) does `push es; push ds; pop es`
     * (ES=DS) then an INDIRECT `call [0x90a34]` through a stored code ptr (disasm 0x3909f..0x390a9).
     * The native lift cannot reproduce the indirect dispatch without the hook's own callee lifted +
     * a fn-ptr shim, and a direct C call to THIS very wrapper would RECURSE — so it stays call_orig,
     * tagged [ORACLE-FALLBACK] (un-convertible self-bridge; counted in .oracle_fallbacks, NOT
     * .repoint_debt). The two OTHER 0x39093 sites (renderer.c rwss resolver: `descword&0x100` @0x36xxx
     * and `test ah,1` @0x368ab) read the EAX return + pass es/fs/gs selectors and belong to the
     * renderer.c render-pipeline — neither could use this void wrapper anyway.
     * Unblock: lift the indirect-hook dispatch natively (hook callee + fn-ptr shim). */
    regs_t io; memset(&io, 0, sizeof io);
    io.va = 0x39093u + OBJ_DELTA; io.ebx = ebx; io.esi = esi;  /* [ORACLE-FALLBACK] self-bridge */
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    roth_unreachable(0x39093u);                               /* installed span-callback hook — render tier */
#endif
}
