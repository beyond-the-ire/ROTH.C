/* Function-lift harness, shared declarations.
 *
 * The differential-verification oracle (oracle.c) maps the rebased ROTH image
 * and runs each original function through the trampoline (trampoline.S) while
 * also running a C reimplementation (renderer.c) on the same inputs, then diffs.
 * See ../../../docs/operating/recomp.md sec.4.
 */
#ifndef ROTH_LIFTED_H
#define ROTH_LIFTED_H

#include <stdint.h>
#include "g_names.h"   /* VA_<global> canon-VA constants for readable G-macro sites (generated) */

/* canon (Ghidra) + OBJ_DELTA = rebased (mapped / called). Mirrors host. */
#define OBJ_DELTA 0x400000u

/* Register file marshalled to/from an original function by call_orig().
 * On entry the fields are loaded into the CPU registers; the function at
 * `va` (rebased) is called; on return the fields hold the output registers.
 * Field order MUST match the byte offsets used in trampoline.S.
 * Callee flags are captured (eflags, off 68). es/fs/gs (off 72/76/80) are
 * optional INPUT selectors preset before the call (0 = leave the host segreg
 * unchanged); the trampoline saves and restores the host's segregs exactly. */
typedef struct {
    uint32_t eax;       /* off 0  */
    uint32_t ebx;       /* off 4  */
    uint32_t ecx;       /* off 8  */
    uint32_t edx;       /* off 12 */
    uint32_t esi;       /* off 16 */
    uint32_t edi;       /* off 20 */
    uint32_t ebp;       /* off 24 */
    uint32_t va;        /* off 28 — rebased target address */
    uint32_t nstack;    /* off 32 — number of stack-arg dwords to push        */
    uint32_t stack[8];  /* off 36 — stack args; stack[0] = first (lowest addr) */
    uint32_t eflags;    /* off 68 — flags captured AFTER the call (CF = bit 0) */
    uint32_t es;        /* off 72 — input selector preset before the call; 0 = keep host */
    uint32_t fs;        /* off 76 — input selector preset before the call; 0 = keep host */
    uint32_t gs;        /* off 80 — input selector preset before the call; 0 = keep host (GS=TLS) */
} regs_t;

/* Call the original function at io->va with io's input registers; on return
 * io holds the output registers. call_orig_raw is the trampoline.S primitive;
 * call_orig (renderer.c) wraps it to temporarily DISARM a live-swapped target's
 * int3 (ROTH_LIFT) so a lift's bridge into another lifted function runs the
 * original cleanly instead of re-trapping the non-reentrant signal handler. */
void call_orig_raw(regs_t *io);
void call_orig(regs_t *io);
/* Far-call an original FAR (retf) routine reached only via a far vector (e.g. sos_timer_dispatch
 * 0x49eaf). va = rebased target. No register inputs (ISR-style); host es/fs/gs preserved. */
void call_orig_far(uint32_t va);

/* Host hook: execute an inline software interrupt (DOS int 0x21 / video int 0x10) from a lift, against
 * the host's DOS/video emulation — the same service the trap dispatch runs for the original's `int`
 * instruction. io.eax/ebx/ecx/edx/esi/edi/ebp are the inputs; on return io holds the output registers
 * and the return value is EFLAGS (CF = bit 0). NULL in the oracle (the inline-interrupt lifts that use
 * it are live-swap-only and never oracled). Wired to host_soft_int in lift_registry.c install. */
extern uint32_t (*g_os_soft_int)(uint8_t vec, regs_t *io);

/* Host hook: drive an `out dx,al` byte port write from a lift, against the host's port emulation (the
 * same path the trap dispatch runs for the original's `out` instruction). Used by the GDV codec's
 * format-1 DAC palette-fade (out 0x3c8/0x3c9). NULL in the oracle (the original's `out` is privileged
 * and would fault under call_orig, so the fade path is never oracled — it is in-game-live-swap-verified).
 * Wired to host_dac_port_out in lift_registry.c install. */
extern void (*g_os_port_out)(uint16_t port, uint8_t val);

/* Host hook: byte port IN for a lift (the `in al,dx` mirror of g_os_port_out). Used by
 * update_software_cursor's planar VGA-latch save (in 0x3c4/0x3c5/0x3ce/0x3cf). NULL in the
 * oracle AND currently unwired in the host (Mode-X planar state is not emulated — the save/
 * restore block is skipped when either port hook is NULL, matching a host with no planar
 * hardware; the fn is statically DEAD). Wire to a traps.c port-in service if Mode-X support
 * ever lands. */
extern uint8_t (*g_os_port_in)(uint16_t port);

/* Host hook: publish the COMPLETE decoded GDV cutscene frame from a lift. The host normally publishes
 * each frame from the int3 planted at GDV_EMIT_SITE (gdv_emit_decoded_frame entry) — when that function
 * is itself lifted, the lift must reproduce the publish, so it calls this FIRST. Wraps the static
 * gdv_publish_frame() in traps.c. NULL in the oracle (emit is live-swap-only; it cannot be oracled). */
extern void (*g_os_publish_frame)(void);

/* GDV loop-hosting coordination flag. gdv_decode_frame sets this around the call_orig bridge
 * of the multi-frame playback loop (run_playback_loop / present_streamed_frame). While set, the host
 * SIGALRM surrogate (traps.c shm_tick) stands in for the frozen GDV timer ISR's per-tick frame decode
 * (codec 0x4d384 + emit/advance 0x4dd33), since that ISR — which paces decode and advances the decoder
 * head the loop spins on — is suspended inside the trap. Set by the lifted side; read by the host. */
extern volatile int g_gdv_loop_hosting;

/* Host-installed hooks (NULL in the oracle, set by lift_install). suspend() lifts EVERY armed lift
 * int3; resume() re-plants them. call_orig brackets the original run with these so the original AND
 * its whole call subtree execute without re-trapping the non-reentrant handler — required for
 * ROTH_LIFT=all, where a lifted function's bridge can transitively reach another lifted entry. */
extern void (*g_os_suspend_int3s)(void);
extern void (*g_os_resume_int3s)(void);

/* build_secondary_surface_list (canon 0x2b298) — mirror/reflection subpass kind-4 collect; ESI=worklist head. */
void build_secondary_surface_list(uint32_t esi);

/* secondary-surface passes (canon 0x2b333/0x2b36f/0x2b407); args = trap entry es/fs/gs selectors.
 * They bridge render_world_secondary_surface (0x2bc3c) via call_orig per rendered record. */
void render_secondary_surface_list (uint16_t es, uint16_t fs, uint16_t gs);
void render_secondary_surface_pass1(uint16_t es, uint16_t fs, uint16_t gs);
void render_secondary_surface_pass2(uint16_t es, uint16_t fs, uint16_t gs);
void render_secondary_surface_pass_clipped(uint16_t es, uint16_t fs, uint16_t gs); /* 0x2b3fa (pass alternator) */

/* store_secondary_surface_record (canon 0x289de) — EAX=param_1 in -> final EAX out; pure obj3. */
uint32_t store_secondary_surface_record(uint32_t eax_in);

/* draw_world_sprite_billboard (canon 0x2d70c) — EAX=record, ESI=record2; renders via the sprite queue
 * (4 bridged callees); es/fs/gs = trap entry selectors. obj3+fb output. */
void draw_world_sprite_billboard(uint32_t eax_in, uint32_t esi_in,
                                        uint16_t es, uint16_t fs, uint16_t gs);

/* render_world_secondary_surface (canon 0x2bc3c) — ESI=record; per-surface renderer (incremental lift,
 * bridges un-transcribed type-paths); es/fs/gs = trap entry selectors. obj3+fb output. The entry GP regs
 * ebx/ecx/edx/edi/ebp are pure PASS-THROUGH to the resolver 0x2c720's bridged rare paths (0x2bc3c itself
 * never reads or writes them). DISASM-PROVEN don't-cares for the resolver: 0x2c720 clobbers ebx at 0x2c732
 * and never reads ecx/edx/edi/ebp as inputs (edx is always sub-edx-edx-cleared) — so any value renders
 * identically; the live-swap invoke passes the real trapped regs for the differential's fidelity. */
void render_world_secondary_surface(uint32_t esi_in, uint32_t ebx, uint32_t ecx, uint32_t edx,
                                           uint32_t edi, uint32_t ebp,
                                           uint16_t es, uint16_t fs, uint16_t gs);
/* render_parallax_sky_columns 0x38d6c — wall texture resolver/column renderer (ABI_TEXRESOLVE; incremental).
 * EDI=screenX, ECX=colcount, ESI=0, src=[0x84980] (stack arg) -> EAX=src'. es/fs/gs = entry selectors. */
uint32_t render_parallax_sky_columns(uint32_t edi, uint32_t ecx, uint32_t esi, uint32_t src,
                                             uint16_t es, uint16_t fs, uint16_t gs);
extern volatile unsigned long g_texres_native, g_texres_bridged;  /* 0x38d6c single(native) vs double(bridged) split */
/* emit_world_face_spans 0x2c720 — secondary-surface texture/block resolver (ABI_FACERESOLVE; incremental).
 * EAX=descriptor (+entry esi/ebx/ecx/edx/edi/ebp + es/fs/gs) -> ESI=resolved record (ret), *out_eax, *out_cf(CF). */
uint32_t emit_world_face_spans(uint32_t eax, uint32_t esi, uint32_t ebx, uint32_t ecx,
                                      uint32_t edx, uint32_t edi, uint32_t ebp,
                                      uint16_t es, uint16_t fs, uint16_t gs,
                                      uint32_t *out_eax, uint32_t *out_cf);
extern volatile unsigned long g_faceres_native, g_faceres_bridged;
extern volatile unsigned long g_rwss_lin, g_rwss_bridged;   /* 0x2bc3c path coverage (transcribed vs bridged) */
extern volatile unsigned long g_rwss_type[256];             /* 0x2bc3c [record+8] type histogram */
extern volatile int g_rwss_dbg_skiprender;                  /* ROTH_RWSS_SKIPRENDER: debug the projection bug */
extern volatile int g_rwss_live;                            /* ROTH_RWSS_LIVE: full transcription (WITH render) for types 2/3/0xff */
extern volatile unsigned long g_rwss_badsel;                /* wall-driver calls skipped due to invalid fs selector ([0x909b0]) */
extern volatile unsigned long g_rwss_sprite_side;           /* 0x2b6c8 sprite side-entry coverage (defined under ROTH_STANDALONE only) */
extern uint32_t (*g_os_sel_base)(uint16_t);               /* host hook -> dpmi_sel_base (selector validity; 0 = invalid) */

/* ---- C reimplementations under verification (canon addresses noted) ---- */
int32_t sincos_lookup(uint32_t ebx);    /* canon 0x3c1f3 */
void    zero_memory(void *dest, uint32_t len);  /* canon 0x41616 */
uint32_t isqrt16_bsearch(uint32_t eax);  /* canon 0x3c068 (16-bit bsearch sqrt) */
int32_t isqrt_fixed(uint32_t eax);      /* canon 0x3bfe5 (build + compute) */
void    game_heap_free(uint8_t *block); /* canon 0x15191 (-> pool_free_chunk) */
uint32_t pool_alloc_checked(uint32_t pool, int32_t size); /* canon 0x35c03 (validate + carve) */
uint32_t game_heap_alloc(int32_t size); /* canon 0x1517d (-> pool_alloc_checked) */
void     recompute_hires_line_doubling(void); /* canon 0x2fd3c */
void     arm_weapon_fire(void);  /* canon 0x17629 */
uint32_t fetch_dbcs_char(uint8_t *p);   /* canon 0x57422 (DBCS lead-byte fetch) */
void     move_cursor_entry_clamped(uint32_t delta); /* canon 0x1bb12 */
int32_t  sos_voice_xchg_w54_if_active(uint8_t *vcb, uint16_t newval); /* canon 0x4a28c */
int32_t  sos_voice_xchg_w32_if_active(uint8_t *vcb, uint16_t newval); /* canon 0x49fe9 */
uint32_t sos_voice_deactivate_slot(uint8_t *vcb, uint32_t p2);        /* canon 0x4ac55 */
int32_t  sos_voice_get_w34(uint8_t *vcb);                             /* canon 0x4a54a */
uint32_t sos_voice_clamp_w38(uint8_t *vcb, uint32_t p2);              /* canon 0x53b01 */
uint32_t find_unflagged_object_by_key(uint8_t *rec);             /* canon 0x303ab */
uint32_t resolve_reloc_record_fields(int32_t *out1, int32_t *out2,
                                            uint32_t offset, uint8_t flags); /* canon 0x1c06b */
uint32_t is_entry_93144_zero(uint32_t index);                    /* canon 0x476fd */
int32_t  rng_next_index_for_count(int32_t count);                /* canon 0x1c9a0 */
uint8_t *fetch_dbcs_char_advance(uint8_t *p, uint32_t *out);     /* canon 0x575e9 */
void     noop_stub_10d87(void);                                  /* canon 0x10d87 */
void     set_floorceil_span_value(uint32_t v);                   /* canon 0x3a848 */
uint32_t find_object_list20(uint16_t key);                       /* canon 0x34510 */
uint32_t find_object_list24(uint16_t key);                       /* canon 0x3451b */
uint32_t find_object_list40(uint16_t key);                       /* canon 0x34526 */
void     noop_ret_stub(void);          /* canon 0x557e7/0x15804/0x3cc01/0x55e7b (bare ret) */
int32_t  collect_raw_state_matches(uint16_t key, uint16_t *out, uint32_t max); /* canon 0x4f36d */
int32_t  find_raw_state_record(uint16_t key);                    /* canon 0x4f52b */
int32_t  ring_push(uint32_t idx, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3); /* canon 0x50e35 */
int32_t  clamp_diff_200(int32_t value, int32_t center);          /* canon 0x26f02 */
void     compute_mode_bytes(uint8_t *out1, uint8_t *out2);       /* canon 0x302b3 */
void     set_7049a_from_71988(void);                             /* canon 0x26a6c */
void     build_atan_table(void);                                 /* canon 0x3c1c9 */
void     basename_strip_ext(const char *path, char *out);        /* canon 0x10711 */
void     decode_byterun1(uint8_t *src, uint8_t *dst, int32_t len);/* canon 0x13154 */
void     init_freelist_820c1(void);                              /* canon 0x1e792 */
int32_t  find_sound_sample_index(uint16_t key);                         /* canon 0x27374 */
void     expand_rgb(uint8_t *src, uint16_t *dst, int32_t count);  /* canon 0x203fd */
void     adjust_records_z_carry(uint32_t center, uint8_t *recs, uint8_t count); /* canon 0x34a5f */
void     ring_init(uint32_t idx, uint32_t capacity);             /* canon 0x50d51 */
uint32_t *blit_save_region(uint32_t *pp);                        /* canon 0x130d4 */
void     shade_remap_blit(uint8_t *desc);                        /* canon 0x13ecb */
uint32_t dbase100_bitmap_test_set(uint32_t idx);                 /* canon 0x1cab7 */
uint32_t dbase100_bitmap_test_clear(uint32_t idx);               /* canon 0x1caf4 */
uint32_t compute_ratio_4c296(uint32_t *out);                     /* canon 0x4c296 */
uint32_t mouse_edge_latch(uint32_t eax);                         /* canon 0x121a1 */
uint32_t mark_sound_handle_by_id(uint32_t id);                      /* canon 0x26de4 */
int32_t  find_object_index_by_ptr(uint32_t ptr);                 /* canon 0x356fa */

/* Batch 1 — Tier-1 keybind/state leaves. The 10 "void" ones
 * take no args and their observable output is a global memory write-set. */
void move_input_strafe_right(void);     /* canon 0x126a4 */
void move_input_strafe_left(void);      /* canon 0x126ad */
void move_input_backward(void);         /* canon 0x12668 */
void move_input_forward(void);          /* canon 0x12686 */
void look_pitch_up(void);               /* canon 0x12927 */
void look_pitch_down(void);             /* canon 0x12939 */
void look_pitch_recenter_down(void);    /* canon 0x1294b */
void apply_render_mode(void);           /* canon 0x14506 */
void commit_player_position_delta(void);/* canon 0x34bb2 */
void key_z_crouch(void);                /* canon 0x1c5d0 */
/* Buffer / pointer-arg ones. */
void     apply_literal_skip_delta_stream(uint8_t *dst, const uint8_t *src); /* 0x4eeae */
uint32_t save_sfx_node_active_state(uint32_t *out);                        /* 0x43d53 */

/* Batch 2 — more Tier-1/2 leaves. */
void     turn_input_left(void);          /* canon 0x126b6 */
void     turn_input_right(void);         /* canon 0x12703 */
void     key_t_handler_vestigial(void);  /* canon 0x14ca7 */
void     dev_toggle_map_menu(void);      /* canon 0x17fa4 */
uint32_t measure_font_char_advance(uint32_t ch); /* canon 0x1508a (EAX in/out) */

/* text_font subsystem; lift_text_font.c. All register-arg, void. */
void draw_text_glyph_with_shadow(uint32_t desc);          /* canon 0x13f2e (EAX=descriptor ptr) */
void draw_text_to_buffer(uint32_t str, uint32_t dest,
                                uint32_t pitch, uint32_t flags); /* canon 0x14d04 (EAX/EDX/EBX/ECX) */
void draw_text_at_screen_xy(uint32_t str, uint32_t x,
                                   uint32_t y, uint32_t flags);  /* canon 0x1a079 (EAX/EBX/ECX/EDX) */

/* Batch 3. evict_das_cache_slot returns its status in CF: 1 = slot selected
 * (CF clear / success), 0 = none evictable (CF set / failure). */
int evict_das_cache_slot(uint16_t forbid_current_tick);   /* canon 0x41385 */
int32_t evict_one_das_cache_slot(void);                   /* canon 0x413ea (EAX=-1/0) */
int reserve_das_cache_slot(void);                         /* canon 0x4134a (1=reserved) */
int32_t find_free_entity_slot(void);                      /* canon 0x42626 */
int32_t measure_control_text_width(const char *s);        /* canon 0x1f91f */
void    key_m_toggle_render_mode(void);                   /* canon 0x144da */
void    queue_timed_message_color(const char *msg, uint8_t color); /* canon 0x1f859 */
void    show_timed_message(const char *msg);              /* canon 0x1f88c */
void    look_pitch_recenter_up(void);                     /* canon 0x1296f */
uint32_t encode_literal_skip_delta_stream(uint8_t *out, const uint8_t *ref,
                                                 const uint8_t *newb, int len); /* canon 0x4ee1f */

/* Batch 5 — Tier-1 completers + math/pixel leaves. */
uint8_t *screen_xy_to_framebuffer_ptr(int32_t x, int32_t y);   /* canon 0x18040 */
void     key_a_jump(void);                                     /* canon 0x1c5f9 */
uint32_t atan2_bearing(int16_t x1, int16_t y2, int16_t y1, int16_t x2); /* 0x3c201 */
void     build_map_selector_menu(char *out);                   /* canon 0x17453 */

/* Batch 6 — DAS serialization/codec leaves. Two have NON-STANDARD
 * argument registers (ESI/EDI, not the Watcom EAX default) — see comments. */
void     select_das_fat_entry(void);                          /* canon 0x411e0 */
uint32_t write_state_dynamic_entities(void *out);             /* canon 0x4eee0 (EAX=out) */
void     initialize_das_block_internal_pointers(void *block); /* canon 0x41554 (ESI=block) */
void     apply_das_sprite_frame_delta_stream(void *dst, const void *src); /* 0x4eda1 (EDI,ESI) */

/* Batch 7 — Tier-3 near-leaf (snapshot/DAS-loader; internal calls). */
uint8_t *num_to_decimal_digits(uint16_t num, uint8_t *edi);  /* canon 0x1155f (AX,EDI) */
void     build_snapshot_anim_filename(void);                 /* canon 0x11500 */
void    *get_loaded_das_block_for_index(uint16_t index);     /* canon 0x414f4 (AX) */

/* Batch 8 — player-movement / platform-carry physics (Tier-3 cluster). */
void     carry_objects_by_player_delta(uint32_t height, void *objs, uint8_t count); /* 0x34bd7 (EAX,ESI,CL) */
uint32_t apply_moving_sector_carry(uint32_t mask, void *list);  /* 0x34a8e (EAX,EDX) */
void     apply_player_movement_input(void);                     /* canon 0x12750 */
void     player_movement_tick(void);                            /* canon 0x12520 */

/* Batch 9 — first corpus-direct lifts. */
uint8_t  input_ring_dequeue(void);                              /* canon 0x1299a */
void     set_filename_extension(char *s, uint32_t ext);        /* canon 0x2fbbc */
char    *string_copy(char *dst, const char *src);             /* canon 0x54ddf */
uint32_t resolve_reloc_ptr(uint32_t offset);                  /* canon 0x226c6 */
void     clear_framebuffer_rect(uint32_t x, uint32_t y,
                                        uint32_t width, uint32_t height); /* 0x12cea (EAX,EDX,EBX,ECX) */
/* 0x2a898: EBP=ptr to {x@+0,y@+2,flags@+4}; returns a pair (EAX,EDX). */
void     rotate_point_2d(const int16_t *pt, int32_t *eax_out, int32_t *edx_out);

/* Batch 10 — corpus-direct small leaves. */
char    *string_concat(char *dst, const char *src);             /* 0x54dfe (EAX=dst,EDX=src) */
uint32_t string_length(const char *s);                         /* 0x55bd0 (EAX=str -> len) */
uint32_t block_payload_size(uint32_t ptr);                     /* 0x35fd9 (EAX=ptr -> EAX) */
uint8_t  obj_counter12_inc(uint32_t ptr);                      /* 0x361e7 (EAX=ptr, ++[ptr+0x12]) */
uint8_t  obj_counter12_dec(uint32_t ptr);                      /* 0x361ef (EAX=ptr, --[ptr+0x12]) */
uint32_t get_errno_ptr(void);                               /* 0x560c2 (-> &DAT_97d44) */
uint32_t get_dbase100_inventory_entry(uint32_t eax);                 /* 0x18147 (AX=index -> EAX) */
/* 0x3bdd2 (EBX=angle): sin in CX, cos in BX, table base in ESI — multi-reg return. */
void     sincos_pair(uint32_t angle, uint16_t *sin_out, uint16_t *cos_out, uint32_t *table_out);
int      find_record_by_id(uint16_t key);                      /* 0x3d018 (DI=key -> CF) */
/* 0x18e2c: byte-identical duplicate of resolve_reloc_ptr (0x226c6) — verified
 * against resolve_reloc_ptr; no separate impl. */

/* Batch 11 — corpus-direct. */
void     blit_descriptor_rows(uint32_t eax);                   /* 0x13106 (EAX=**desc) */
uint32_t find_free_slot_83ed4(void);                           /* 0x277b6 (-> idx/-1) */
/* 0x2d29a (AX=x,DX=y,EBX=idx-out,EDI=vertex,ESI=bbox; EDI+=0x10,EBX+=2 on exit). */
void     emit_vertex_bbox(int16_t x, int16_t y, uint8_t *ebx, uint8_t *edi, uint8_t *esi);
uint32_t identity_passthrough(uint32_t eax);                   /* 0x51995 (EAX->EAX) */
void     reset_renderer_tables(void);                          /* 0x2f42b (global memset) */
uint32_t scan_tag4_chunk(uint32_t obj);                        /* 0x1dda8 (EAX=obj -> ptr/0) */

/* Batch 12 — corpus-direct. */
void     init_render_struct_89ed0(void);                       /* 0x2f962 (global write-set) */
void     split_path(const char *src, char *dir, char *name, char *ext); /* 0x210ec (EAX,EDX,EBX,ECX) */
uint32_t rotate_quad(uint32_t eax, uint8_t *edi);              /* 0x3ded2 (EDI=record; EAX passthrough) */

/* Batch 13 — corpus-direct. */
uint32_t abs_i32(int32_t eax);                                 /* 0x560fa (|EAX|) */
uint32_t signext_852f2_to_909a4(void);                         /* 0x2ad14 (movsx global copy) */
uint32_t geom_find_matches(uint16_t key, uint32_t cap, uint8_t *out); /* 0x4f313 (AX,EBX,EDX) */

/* Batch 14 — far-data / selector lifts (need the oracle's make_selector). */
void     mark_geom_sentinel_entries(uint8_t *seg);                 /* 0x2ec1a (ES=*0x490be8) */

/* Batch 15 — case-2 preset-segreg far-data (trampoline presets es/fs/gs before the call). */
void     clear_es_record_field4(uint8_t *es_base);             /* 0x293a3 (ES preset) */
void     render_world_col_tint_gradient_38631(uint32_t edi, uint32_t ecx, uint8_t bh,
                                        const uint8_t *gs_base, const uint8_t *es_base); /* 0x38631 (ES self-load + GS preset) */

/* Batch 16 — renderer world-span cluster (case-2 grind; ES self-load + GS preset). */
void     render_world_col_tint_gs_385dc(uint32_t edi, uint32_t ecx, uint8_t bh_in,
                                        const uint8_t *gs_base, const uint8_t *es_base); /* 0x385dc */
void     render_world_col_unshaded_masked_388be(uint32_t eax, uint32_t ecx, uint32_t edx,
                                        uint32_t esi_in, uint32_t edi);                  /* 0x388be (textured, no seg) */

/* Batch 53 — first SMC textured perspective column (dual-segment GS palette + ES shadow LUT). */
void     render_world_span_390ac(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                        uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in,
                                        const uint8_t *gs_base, const uint8_t *es_base);  /* 0x390ac (SMC) */
void     render_world_span_wrapped_391d0(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                                uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in,
                                                uint32_t mask, const uint8_t *gs_base, const uint8_t *es_base); /* 0x391d0 (SMC tiled) */
void     render_world_col_shaded_blend_2axis_392cc(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                        uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in,
                                        const uint8_t *gs_base, const uint8_t *es_base);  /* 0x392cc (SMC tiled, global mask, stateful shade) */
void     render_world_col_blend_2axis_39398(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                        uint32_t esi_in, uint32_t edi_in, const uint8_t *es_base); /* 0x39398 (SMC unlit, raw texel, ES-only) */
void     render_world_col_blend_masked_39453(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                        uint32_t esi_in, uint32_t edi_in, const uint8_t *es_base); /* 0x39453 (linear unlit, ES-only, no SMC) */
void     render_world_col_shaded_blend_masked_39520(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                        uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in,
                                        const uint8_t *es_base); /* 0x39520 (linear, flat low-byte-replace colormap, SMC) */
/* Batch 59 — wall-only column mappers (0x36b39 driver, not shared with the sprite driver). */
void     render_world_col_unshaded_37c60(uint32_t eax, uint32_t ecx, uint32_t edx,
                                        uint32_t esi_in, uint32_t edi);            /* 0x37c60 unshaded opaque */
void     render_world_col_shaded_gs_37ec8(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                        uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in,
                                        const uint8_t *gs_base);                   /* 0x37ec8 shaded opaque */
void     render_world_col_shaded_masked_gs_38198(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                        uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in,
                                        const uint8_t *gs_base);                   /* 0x38198 shaded transparent */
void     render_world_col_unshaded_2axis_3832c(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                        uint32_t esi_in, uint32_t edi_in);         /* 0x3832c unshaded opaque wrapped */
void     render_world_col_unshaded_masked_2axis_383ac(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                        uint32_t esi_in, uint32_t edi_in);         /* 0x383ac unshaded transp wrapped */
void     render_world_col_unshaded_masked_2axis_38288(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                        uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in); /* 0x38288 flat-cmap wrapped transp */
void     render_world_col_unshaded_opaque_37fac(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                        uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in); /* 0x37fac flat-cmap wrapped opaque+dither */
void     render_world_col_unshaded_masked_38964(uint32_t param_1, uint32_t ecx, uint32_t param_2,
                                        uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in); /* 0x38964 linear flat-cmap transp+dither */
void     render_world_col_unshaded_opaque_37cec(uint32_t param_1, uint32_t ecx, uint32_t param_2,
                                        uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in); /* 0x37cec linear flat-cmap opaque+dither */
void     render_world_col_tint_385d4(uint32_t edi, uint32_t ecx, const uint8_t *es_base); /* 0x385d4 const-shade-blend stub */
void     render_world_col_solid_gradient_387f0(uint32_t ecx, uint32_t ebx_in, uint32_t edi_in,
                                        const uint8_t *gs_base);                   /* 0x387f0 solid shaded gradient */
void     render_world_col_solid_fill_38684(uint32_t ecx, uint32_t ebx_in, uint32_t edi_in,
                                        const uint8_t *gs_base);                   /* 0x38684 solid/dither shaded fill */
void     render_world_col_shaded_gs_wrapped_38434(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                        uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in,
                                        const uint8_t *gs_base);                   /* 0x38434 shaded opaque wrapped */
void     render_world_col_shaded_masked_gs_wrapped_384fc(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                        uint32_t ebx_in, uint32_t esi_in, uint32_t edi_in,
                                        const uint8_t *gs_base);                   /* 0x384fc shaded transp wrapped */
void     render_span_fill_38697(uint8_t al, uint32_t ecx, uint32_t edi_in);            /* 0x38697 solid fill (al=EAX) */
void     render_world_col_fill_wrap_387e0(uint32_t ecx, uint32_t edi_in);              /* 0x387e0 stub -> 0x38697 */
void     fill_projection_linear_ramp(uint8_t bp_low, uint8_t bl, uint8_t *edi, uint16_t count); /* 0x40cb1 */
void     interpolate_projection_gaps(uint8_t *esi, uint8_t *edi, uint16_t cx_count);            /* 0x40d1e */
void     build_projection_table(uint16_t mode, uint32_t ebp, const uint8_t *esi);              /* 0x40bd6 */
void     rebuild_projection_table(void);                                                       /* 0x409b4 */
uint32_t project_point_to_screen_column(int16_t dx, int16_t bx, uint32_t eax_in);              /* 0x43d04 */
void     setup_render_projection_scale(void);                                                  /* 0x2e458 */
void     configure_render_viewport(void);                                                      /* 0x408d1 */
void     floorceil_rotation_sincos(int32_t *pt);                                               /* 0x3bdf3 */
/* Already-lifted PEEL floorceil helpers, exposed for independent oracle verification. */
uint32_t write_floorceil_span_texcoords(uint32_t edi, uint32_t ebp, uint16_t cx,
                                               uint32_t edx, uint32_t esi);                           /* 0x3badf */
void     patch_span_driver_shade(uint16_t ax);                                                 /* 0x2d6a8 */
int32_t  compute_sector_wall_depth_range(uint32_t sec, uint32_t gs_base, uint32_t es_base);    /* 0x293ca */
void     compute_floor_clearance_for_render(uint32_t gs_base, uint32_t es_base);               /* 0x29403 */
void     shift_wall_nodes_vertical(uint32_t es_base, uint32_t fs_base);                        /* 0x28972 */
uint16_t project_floorceil_edge_texcoord(uint32_t esi, uint16_t ax_in);                       /* 0x3b724 */
void     store_floorceil_flat_edge_texcoords(uint32_t esi, uint32_t edi);                      /* 0x3b84a */
void     interp_floorceil_edge_texcoords(uint32_t ecx, uint32_t esi, uint32_t edi, uint32_t ebp);/* 0x3b8b7 */
uint32_t build_floorceil_clip_edges(int *out_empty);  /* 0x3b4a2 (+inlines 0x3b506/3b5a8/3b5c2/3b5d2) */
uint32_t build_floorceil_vertex_records(uint32_t esi);  /* 0x3bbb0 */
void     record_portal_clip_entry(uint32_t si, uint32_t di, uint32_t es_base);  /* 0x3cf98 */
int      compute_wall_column_source_offset(uint32_t edi_in, uint32_t *o_esi, uint32_t *o_edi,
                                                  uint32_t *o_ecx, uint32_t *o_edx, uint32_t *o_eax); /* 0x378dc */
uint32_t mark_sector_draw_order(uint32_t ebx, uint32_t fs_base, uint32_t es_base, uint32_t *edip); /* 0x2a7af */
void     compute_object_y_center(uint32_t esi);  /* 0x3c843 */
void     depth_sort_sprite_queue(void);          /* 0x3c7e7 */
uint32_t resolve_face_surface_id(uint32_t rec, uint32_t base, uint32_t off);  /* 0x4f0ab */
int      interpolate_object_clip_vertex(uint32_t esi, uint32_t ebx, uint32_t edi, int16_t t); /* 0x3cad6 (ret CF) */
uint16_t project_wall_edge_y(int16_t ax_in, uint16_t cx, uint32_t edx);  /* 0x38c46 */
int32_t  compute_surface_normal_shade(uint32_t ebp);  /* 0x3c0be */
int      project_wall_face_span_extents_2c400(uint32_t esi_in, uint32_t ebx_in,
                                              uint32_t es_base, uint32_t fs_base);  /* 0x2c400 */
int      compute_object_screen_bbox(uint32_t esi);        /* 0x3c598 (via thunk 0x3cac5) */
int      thunk_compute_object_screen_bbox(uint32_t esi);  /* 0x3cac5 */
uint32_t insert_worklist_entry(uint32_t edi, uint32_t eax, uint32_t ebx,
                                      uint32_t es_base, uint32_t fs_base);  /* 0x2a446 */
void     clip_project_emit(int32_t depth, int32_t src, int16_t x_in,
                           uint8_t *ebp, uint8_t *edi, uint8_t *esi);  /* 0x2d200 / 0x2d24d body */
void     patch_span_mapper_pitch(uint32_t eax);  /* 0x36464 (SMC stride patcher) */
void     dispatch_world_span_column(regs_t *io); /* 0x3778b (per-column dispatcher) */
int      clip_object_to_frustum(uint32_t esi_in, uint32_t edi_in);  /* 0x3c892 (ret CF) */
int      clip_cull_object_to_view(uint32_t esi, int16_t bx);        /* 0x3c511 (ret CF; sets g_clip_cull_queued_rec = ESI-at-ret) */
extern uint32_t g_clip_cull_queued_rec;   /* 0x3c511's ESI-at-ret (source, or the clipped record on straddle) */
void     build_sprite_render_queue(void);                           /* 0x3c4a1 */
uint32_t finalize_sprite_render_queue(uint32_t esi);                /* 0x3c477 */
int      add_reflection_view_entry(uint32_t fs_base);               /* 0x283d9 (ret CF) */
void     reflect_view_across_mirror_plane(uint32_t ebx, uint32_t fs_base, uint32_t gs_base);  /* 0x28456 */
void     recompute_view_region_offsets(void);                       /* 0x10e4e (pure leaf) */
void     tick_ambient_render_animation(void);                       /* 0x2ab30 (pure leaf) */
/* floor/ceil fill inner-loop test hook (one of 0x3acec..0x3b15c via fc_dispatch; oracle-only). */
void     floorceil_fill_dispatch(uint32_t key, uint32_t edi_cur, int32_t count, uint8_t ah,
                                        uint32_t gs_base, uint32_t fs_base, uint32_t blend_base,
                                        uint32_t cmap_flat, uint32_t es_fb_base, uint32_t flat_fb,
                                        uint8_t ror_imm, uint8_t dh_mask, uint32_t ebx_mask);

/* Batch 54 — scaled-sprite horizontal-span inner loops (the 0x39610 draw_scaled_sprite_spans driver
 * family, reached via g_sprite_column_fn 0x8a368; each ends `jmp 0x39e52`, oracle isolates one span). */
void     render_sprite_span_fill_39fcd(uint32_t ecx, uint32_t edi, uint8_t *es_base); /* 0x39fcd (solid unshaded fill) */
void     render_sprite_span_solid_3a000(uint32_t ecx, uint32_t edi,
                                               const uint8_t *gs_base, uint8_t *es_base); /* 0x3a000 (single-shade gs fill) */
void     render_sprite_span_gradient_3a0b1(uint32_t ecx, uint32_t edi,
                                                  const uint8_t *gs_base, uint8_t *es_base); /* 0x3a0b1 (per-pixel gradient shade) */
void     render_sprite_span_tex_3a100(uint32_t ecx, uint32_t edi_in,
                                             const uint8_t *es_base); /* 0x3a100 (SMC textured, transparency, flat remap) */
void     render_sprite_span_tex_3a220(uint32_t ecx, uint32_t edi_in,
                                             const uint8_t *fs_base); /* 0x3a220 (raw texel, fwd/rev flip split) */
void     render_span_texmap_3a368(uint32_t ecx, uint32_t edi_in,
                                         const uint8_t *fs_base); /* 0x3a368 (shaded remap, fwd/rev flip split) */
void     render_sprite_span_tex_shaded_3a4f8(uint32_t ecx, uint32_t edi_in,
                                                    const uint8_t *gs_base, const uint8_t *fs_base); /* 0x3a4f8 (gs gradient shade, fwd/rev) */
void     render_sprite_span_tex_blend_3a700(uint32_t ecx, uint32_t edi_in,
                                                   const uint8_t *gs_base, const uint8_t *es_base); /* 0x3a700 (richest: gs shade + transparency) */
/* One span's dispatch descriptor produced by the driver per-span body (0x39bd2): the dest offset, pixel
 * count, and the inner-loop VA to render it (0 = the a356 alt-path / no normal dispatch). */
typedef struct { uint32_t edi, ecx, fn_va; int terminate; } span_dispatch_t;
span_dispatch_t sprite_span_setup_39bd2(uint32_t *p_esi, uint32_t *p_ebx, uint32_t persp_dividend,
                                               int32_t tex_divU, int32_t tex_divU2); /* 0x39bd2 per-span body; persp_dividend=SMC 0x39c27, tex_divU/U2=SMC 0x39d80/0x39d8d */
/* Full scaled-sprite span driver (0x39610). esi = surface record; gs_base/es_base/fs_base = the
 * resolved colormap / blend / texture segment bases (the caller resolves the LDT selectors).
 * es_sel/fs_sel = the game's *entry* ES/FS selector VALUES: the bridged edge-walker
 * (rasterize_floorceil_polygon 0x3b1c1) and its render_floorceil_tex_* sub-renderers read texture
 * data via FS (and blend via ES), so call_orig must run them with the game's real selectors, not the
 * host's. Dispatches to the verified inner loops. Verified in-process (live-swap + obj3/framebuffer
 * differential), not the per-call oracle. */
/* floor/ceiling polygon edge-walker (0x3b1c1): builds the per-scanline span run-list; returns 1 if
 * empty (ZF set). esi=surface geom; gs/es/fs = the driver's entry selectors. Callees bridged for now. */
int      rasterize_floorceil_polygon(uint32_t esi_geom, uint16_t gs_sel, uint16_t es_sel, uint16_t fs_sel);
uint32_t classify_surface_floorceil(uint32_t esi);                    /* 0x38b54 (floor/ceil dispatch helper) */
void     draw_scaled_sprite_spans(uint32_t esi, uint32_t gs_base,
                                         uint32_t es_base, uint32_t fs_base,
                                         uint16_t es_sel, uint16_t fs_sel); /* 0x39610 */
void     draw_world_surface_spans(uint32_t ecx_entry,
             uint32_t gs_base, uint32_t es_base, uint32_t fs_base);        /* 0x36b39 WALL driver (WIP) */
void     draw_floorceil_surface(uint32_t esi, uint32_t gs_base, uint32_t fs_base,
             uint32_t es_fb_base, uint32_t blend_base, uint16_t es_sel, uint16_t fs_sel); /* 0x3a84e FLOOR/CEIL driver */
void     rasterize_world_spans_scanline(uint32_t esi, uint32_t gs_base, uint32_t blend_base,
             uint32_t fs_tex_base, uint32_t es_fb_base, uint16_t es_sel, uint16_t fs_sel,
             uint16_t gs_sel, uint16_t ds_sel);                                           /* 0x366cb SMC dispatcher */
/* render — tier-3 scene geometry. transform_world_vertices (0x2a814): per-frame world->view
 * vertex transform; NO args (all global + the selector-mapped vertex table es=g_vertex_selector 0x852cc). */
void     transform_world_vertices(void);                                /* 0x2a814 */
/* clip_sector_walls_to_view (0x2d793): per-sector wall->view clip. ESI=sector off; GS=vtx-sel 0x852cc,
 * ES=record-sel 0x852c8. Returns CF (1=continue/cross portal, 0=terminal). pass-kind-2 native; 0/1 bridged. */
int      clip_sector_walls_to_view(uint32_t esi_sector, uint16_t gs_sel, uint16_t es_sel);  /* 0x2d793 */
/* walk_visible_sectors (0x294c0): per-frame visible-extent list builder (count 0x85220 + list 0x85224).
 * NO register args; caller precondition GS=g_vertex_selector 0x852cc. Two bodies gated by 0x853d3 (mode 0
 * = portal walk, calls clip_sector_walls_to_view; mode 1 = bbox column cull). Pure obj3 output. */
void     walk_visible_sectors(void);                                    /* 0x294c0 */
/* build_sector_render_tree_recursive (0x29830): recursive portal traversal building the FS-selector node
 * arena. ESI=sector off; fs/es/gs = the three selector BASES (resolved via g_os_sel_base). Recursive;
 * writes the arena (outside obj3) + es:[esi+4] back-ref + obj3 cursor scratch. Verify the {0x29812+0x29830}
 * pair with the grand-caller's cursor reset prepended (ABI_SECTORTREE). */
void     build_sector_render_tree_recursive(uint32_t esi, uint32_t fs_base, uint32_t es_base, uint32_t gs_base);  /* 0x29830 */
void     begin_sector_render_tree(uint32_t esi);                        /* 0x29812 (the verified pair entry) */
void     build_sector_draw_order(uint32_t esi_unused);                  /* 0x2a6d0 (draw-list consumer; esi ignored) */
void     finalize_draw_list_entry(uint32_t eax, uint32_t esi, uint32_t ebx, uint32_t ecx,
                                         uint32_t edx, uint32_t edi, uint32_t ebp,
                                         uint16_t es, uint16_t fs, uint16_t gs); /* 0x2a4d0 (draw-list cluster foundation) */
void     emit_object_draw_entries(uint32_t eax, uint32_t ecx, uint32_t ebp,
                                         uint16_t es, uint16_t fs, uint16_t gs); /* 0x29dcf (draw-list cluster: object list walk) */
void     build_weapon_billboard_record(uint32_t eax, uint32_t ecx, uint32_t edx, uint32_t ebp,
                                         uint16_t es, uint16_t fs, uint16_t gs); /* 0x29f50 (draw-list cluster: weapon billboard + anim) */
void     build_scene_draw_list(uint32_t ecx);                            /* 0x2a0a0 (draw-list cluster ORCHESTRATOR) */
void     compute_face_span_extents(uint32_t esi, uint32_t ebx, uint16_t es, uint16_t fs); /* 0x2c250 (face-list: span extents leaf) */
void     emit_world_span_record(uint16_t es, uint16_t fs, uint16_t gs);                  /* 0x2d130 (face-list: build span quad + render via bridged 0x366cb) */
void     emit_world_span_unclipped_indexed(uint32_t eax, uint32_t ecx, uint32_t ebx,
                                                  uint16_t es, uint16_t fs, uint16_t gs);        /* 0x2d5b0 (face-list: indexed span emitter) */
void     emit_world_span_unclipped(uint32_t eax, uint32_t ecx, uint32_t ebx,
                                          uint16_t es, uint16_t fs, uint16_t gs);                /* 0x2d3d0 (face-list: forward span emitter) */
void     emit_world_span_clipped(uint32_t eax, uint32_t ecx, uint32_t ebx,
                                        uint16_t es, uint16_t fs, uint16_t gs);                  /* 0x2d2dd (face-list: forward clipped/projected span emitter) */
void     emit_world_span_clipped_indexed(uint32_t eax, uint32_t ecx, uint32_t ebx,
                                                uint16_t es, uint16_t fs, uint16_t gs);          /* 0x2d4b5 (face-list: backward clipped/projected span emitter) */
void     draw_world_face_projected_spans(uint32_t eax, uint32_t ebx, uint32_t esi,
                                                uint16_t es, uint16_t fs, uint16_t gs);          /* 0x2cf60 (face-list: per-face span dispatcher) */
void     draw_world_face_clipped_spans(uint32_t eax, uint32_t ebx, uint32_t esi,
                                              uint16_t es, uint16_t fs, uint16_t gs);            /* 0x2cbb0 (sky/portal special-face span path) */
void     render_world_face_list_subpass(uint32_t ecx, uint16_t gs);                      /* 0x28dbe (face-list SUBPASS orchestrator) */
void     render_world_face_list(uint32_t ecx, uint16_t gs);                              /* 0x2ad21 (twin: shaded reflection face-list pass) */
uint32_t render_world_scene(uint32_t eax, uint32_t edx, uint32_t ebx_dead,
                                   uint16_t es, uint16_t fs, uint16_t gs);                       /* 0x28a79 (THE scene root) */
void     setup_surface_render_constants(void);                                            /* 0x2abfb (pure-DS perspective/span const setup) */
/* --- render_world scene-orchestration spine (lift_render_world.c) --- */
uint32_t render_world_view(uint16_t es, uint16_t fs, uint16_t gs);                         /* 0x10c8f (level/warp single-view entry) */
void     setup_frame_render_context(uint32_t eax_record);                                 /* 0x2aa3e (per-frame view/camera commit) */
void     render_clipped_sector_subscene(void);                                            /* 0x29305 (sector render-tree fan-out) */
void     render_primary_scene_view(void);                                                 /* 0x292be (main forward view; falls into 0x29305) */
void     render_split_viewport_lower(void);                                               /* 0x288fb (lower split-viewport face pass) */
void     render_secondary_viewport_pass(void);                                            /* 0x286df (mirror/secondary viewport pass) */
void     render_reflection_subviews(void);                                                /* 0x284df (mirror-plane reflection passes) */
void     render_scene_body(void);                                                         /* 0x2885d (main+reflection+primary orchestrator) */
void     render_world_view_pass(uint32_t eax_record);                                     /* 0x287b6 (per-frame render entry) */
void     shutdown_render_subsystem(void);                                                 /* 0x2fcd4 (renderer teardown) */
void     free_scene_geometry_buffers(void);                                               /* 0x2a93c (geometry buffer teardown) */
void     free_geometry_buffer_and_selector(uint32_t buf);                                 /* 0x40bc7 (EAX=buffer; + 0x2a8e9 [0x85294] selector release via 0x40adf sel-base heap free) */
void     invoke_span_callback(uint32_t ebx, uint32_t esi);                                /* 0x39093 (optional per-span hook) */
void     render_world_sprite(uint32_t esi_rec, uint16_t es, uint16_t fs, uint16_t gs);    /* 0x36651 (per-sprite rasterizer setup) */
void     rwss_sprite_side_entry(uint32_t rec, uint16_t es, uint16_t fs, uint16_t gs);     /* 0x2b6c8 (frameless sprite-queue record -> rwss shared tail; defined under ROTH_STANDALONE only) */
void     rwss_sprite_body_entry(uint32_t esi_rec, uint16_t es, uint16_t fs, uint16_t gs); /* 0x366d2 (sprite rasterizer mid-entry -> rasterizer body, prologue clear skipped; ROTH_STANDALONE only) */
void     draw_sprite_render_queue(uint32_t esi, uint16_t es, uint16_t fs, uint16_t gs);   /* 0x3b1b1 (render sprite queue) */
uint32_t project_sprite_to_render_queue(uint32_t eax, uint32_t ecx, uint32_t ebx,
                                               uint32_t edx, uint32_t esi);                       /* 0x3c2bd (returns CF: 1=culled) */
uint32_t select_surface_anim_frame(uint32_t eax_rec, uint32_t esi_surf);                  /* 0x2b5ea (returns frame ptr in ESI) */
uint32_t load_backdrop_image(uint32_t eax_fname, uint32_t edx_desc,
                                    uint32_t ebx_dest, uint32_t ecx_cap);                         /* 0x4b08c (returns EAX error code) */
void     load_backdrop_raw(void);                                                         /* 0x16164 (level backdrop loader) */
void     clip_and_emit_floor_walls(uint32_t ebp, uint16_t gs_sel, uint16_t es_sel);             /* 0x2d757 clip_and_emit_floor_walls */
void     build_world_face_edge_spans_2cc48(uint16_t colour_ax, uint32_t esi_in, uint32_t ebx_in,
                                           uint16_t es, uint16_t fs, uint16_t gs,
                                           uint32_t fs_base, uint32_t es_base);                   /* 0x2cc48 build_world_face_edge_spans */
void     finalize_world_span_overlay_2d040(uint32_t esi_in, uint32_t ebx_in,
                                           uint16_t es, uint16_t fs, uint16_t gs,
                                           uint32_t fs_base, uint32_t es_base);                   /* 0x2d040 finalize_world_span_overlay */
/* per-span debug hook (host sets during the byte-differential; NULL otherwise). */
extern void (*g_sprite_span_dbg)(uint32_t idx, uint32_t fn_va, uint32_t edi, uint32_t ecx);
/* debug A/B: route positive-g_8a3b6 override to the base colfn instead of texmap (ROTH_LIFT_NOTEXMAP). */
extern int g_os_force_base_dispatch;
/* DEBUG: per-call guard bookkeeping exposed for the 0x8a370 differential pinpoint. */
extern uint32_t g_os_dbg_guard_setup, g_os_dbg_range, g_os_dbg_subcount, g_os_dbg_subval;
/* wall-driver call_orig-bridge phase (0=native, 1=resolve via wd_bridge, 2=EXIT F subpass, 3=first-col resolve). */
extern volatile int g_wd_dbg_phase;

/* Batch 17 — corpus-direct leaves. */
void     reset_movement_velocity_queues(void);                 /* 0x124dd */
void     compute_viewport_half_extents(void);                  /* 0x1a37b */
void     store_indexed_dword_flagged(uint32_t eax, uint32_t edx); /* 0x561f9 */
void     set_909a4_save_old_to_852f2(uint32_t eax);            /* 0x2acfc */
void     string_copy_bytewise(uint8_t *dst, const uint8_t *src); /* 0x558ec */
void     copy_nonzero_bytes(uint8_t *dst, uint32_t ebx, const uint8_t *src); /* 0x1426f */
uint32_t noop_passthrough_50e1d(uint32_t eax);                 /* 0x50e1d (no-op identity) */

/* Batch 18 — corpus-direct tiny leaves (accessors). */
void     set_voice_sample_rate(uint32_t eax);                              /* 0x1e76e */
void     set_71388(uint32_t eax);                              /* 0x20597 */
void     set_71d84(uint32_t eax);                              /* 0x26b60 */
void     set_8495c(uint32_t eax);                              /* 0x283a7 */
uint32_t get_doserrno_ptr(void);                               /* 0x560c8 */
uint32_t get_72540(void);                                      /* 0x58503 */
void     cmd_snap_toggle(void);                                /* 0x11077 */
void     clear_819c0_bits(void);                               /* 0x1c5c8 */
void     set_90bf8_ffff(void);                                 /* 0x108d4 */
uint32_t xchg_849a4(uint32_t eax);                             /* 0x283a0 */
uint32_t emit_biased_byte(uint8_t dl, uint8_t *edi);           /* 0x4ee93 */

/* Batch 19 — corpus-direct small leaves. */
void     set_default_mouse_button_swap(void);                                  /* 0x15b50 */
uint32_t block_size_field8(uint32_t eax);                      /* 0x35fe4 */
uint32_t halve_eax_if_90bd4(uint32_t eax);                     /* 0x2e35f */
uint32_t nibble_to_hex_char(uint32_t eax);                     /* 0x583d1 */
uint32_t set_cursor_shape_ptr_pair(uint32_t eax);              /* 0x115dd */
void     enable_dev_mode(void);                                /* 0x14464 */
void     copy_word_90bcc_to_8532a(void);                       /* 0x2ab21 */

/* Batch 20 — string utils + global setups. */
uint32_t string_length_244c8(const uint8_t *s);                /* 0x244c8 (strlen) */
uint32_t string_find_char(const uint8_t *s, uint8_t dl);       /* 0x57da2 (strchr) */
uint32_t path_basename(const uint8_t *s);                      /* 0x150fa */
void     cmd_set_hires(void);                             /* 0x1089c */
void     reset_input_ring(void);                               /* 0x12504 */
void     init_84964_block(void);                               /* 0x289bf */
void     advance_clamp_8a0f0(uint32_t eax, uint32_t edx);      /* 0x320a7 */

/* Batch 21 — resolver, path/blit utils, global setups. */
uint32_t resolve_command_by_index(uint16_t ax);                /* 0x315a7 */
int32_t  cmd_jump_if_next_fails(uint32_t rec);                 /* 0x30f55 (RAW cmd base 0x38) */
int32_t  find_geometry_record(uint16_t key);                  /* 0x4f2e0 (SECTOR single-match finder) */
int32_t  find_face_record(uint16_t key);                      /* 0x4f567 (FACE single-match finder) */
int32_t  test_dbase100_record_flag(uint32_t idx);             /* 0x1cb35 (progress-flag read) */
int32_t  cmd_set_player_speed_reduction(uint32_t rec);       /* 0x30ab3 (RAW cmd base 0x41, "Slow Player Speed" vel >>= shift) */
int32_t  gather_faces_by_id(uint16_t id, uint32_t max, uint32_t out); /* 0x34c38 (face/object id gather) */
uint32_t step_mode_inc(uint32_t eax);                         /* 0x318dd (Count step mode 0) */
uint32_t step_mode_sub_clamp(uint32_t eax, uint32_t esi);     /* 0x318df (Count step mode 1) */
uint32_t step_mode_add(uint32_t eax);                         /* 0x318ef (Count step mode 2) */
uint32_t step_mode_xor(uint32_t eax);                         /* 0x318f6 (Count step mode 3) */
uint32_t point_segment_distance_sq(uint16_t radius, uint32_t a, uint32_t b, uint32_t point); /* 0x40805 */
int32_t  cmd_count(uint32_t rec);                             /* 0x318fd (RAW cmd base 0x15, Count) */
void     clear_geometry_visited_flags(void);                  /* 0x4f477 (clear SECTOR visited bit 0x4) */
void     apply_geometry_face_write(uint32_t edi, int32_t ecx, uint16_t bx, uint32_t edx_in); /* 0x343e1 */
void     collect_secondary_matches_into_struct(uint32_t st);  /* 0x33072 (collect 0x34c97 -> struct) */
uint32_t relocate_moving_objects_to_sectors(uint32_t eax);    /* 0x283ad (list walk -> 0x3ee4b locate) */
uint32_t relocate_moving_objects_if_dirty(void);              /* 0x15ee2 (dirty-gated relocate) */
void     tick_particles(void);                                /* 0x4b396 (particle physics + 0x3ee4b relocate) */
uint32_t spawn_particle(uint32_t eax, uint32_t ecx, uint32_t edx, uint32_t ebx); /* 0x4b4e9 (point spawn) */
uint32_t spawn_particle_on_edge(uint32_t eax, uint32_t edx, uint32_t ebx);    /* 0x4b5b4 (edge-lerp spawn) */
uint32_t alloc_face_effect(uint16_t key, uint32_t mode, uint32_t base);       /* 0x3296b (gather+alloc+copy) */
void     tick_move_floorceil(uint32_t rec);                                   /* 0x340f3 (floor/ceil move tick) */
uint32_t tick_moving_sector(uint32_t rec);                                    /* 0x32c05 (sector-scroll tick) */
int32_t  swap_cell_state_linked_pair(uint32_t rec);           /* 0x33571 (swap rec<->2 geom cells; ret old B[0xa]) */
int32_t  find_object_record_by_id(uint16_t id);               /* 0x34531 (object-table id search) */
int32_t  cmd_default_nop(uint32_t rec);                       /* 0x30ab0 (dispatch default/NOP) */
int32_t  tick_apply_geometry_effect(uint32_t rec);            /* 0x34322 (collect + apply + rerun) */
int32_t  tick_rerun_command_execute(uint32_t rec);            /* 0x34086 (re-dispatch via 0x30780) */
int32_t  FUN_000340b6(uint32_t rec);                          /* 0x340b6 (alias -> tick_rerun) */
int32_t  tick_resolve_state_and_rerun(uint32_t rec);          /* 0x340bc (resolve + swap + rerun) */
int32_t  tick_delay_timer(uint32_t rec);                      /* 0x32221 (eax=rec; deferred-queue enqueue) */
int32_t  fire_queued_command(uint32_t rec, uint32_t slot);    /* 0x30b7c (walk + enqueue + countdown) */
int32_t  register_object_state_effect(uint32_t rec);          /* 0x31176 (bridges 0x32a20) */
int32_t  tick_register_object_state(uint32_t rec);            /* 0x3405e (composes 0x31176) */
int32_t  tick_register_timed_effect(uint32_t rec);            /* 0x30b27 (alloc slot + countdown) */
void     apply_geometry_move_with_player(uint32_t edi, int32_t ecx, uint16_t bx, uint32_t edx_in); /* 0x343b4 */
void     apply_floor_move_to_group(uint32_t edi, uint16_t bx, uint32_t edx_in); /* 0x3423e */
int32_t  apply_cell_move_to_player(uint32_t flags, uint32_t celloff);            /* 0x348ed */
int32_t  apply_cell_move_to_player_portalcheck(uint32_t flags, uint32_t celloff); /* 0x348f9 */
int32_t  tick_cache_effect_base(uint32_t rec);                /* 0x31ccd (latch rec base value) */
void     init_command_timer_countdown(uint32_t rec, uint32_t out); /* 0x30b45 (timer countdown init) */
int32_t  reset_command_chain_no_source(uint16_t ax);          /* 0x305a1 (reset variant) */
uint32_t alloc_effect_record(uint32_t size);                  /* 0x34464 (alloc+prepend list A) */
uint32_t register_damage_emitter(uint32_t size);              /* 0x344c9 (alloc+prepend list C) */
uint32_t build_effect_record_from_matches(uint16_t key, uint32_t base); /* 0x329d5 */
uint32_t build_damage_emitter_from_matches(uint16_t key, uint32_t base); /* 0x310bc */
int32_t  tick_spawn_damage_emitter(uint32_t rec);             /* 0x3107d (build + resolve faces) */
uint32_t alloc_effect_record_list_b(uint32_t size);           /* 0x34499 (alloc+prepend list B) */
void     free_effect_list(uint32_t head_ptr);                 /* 0x34434 (free a handle list + clear head) */
void     unlink_finished_effect(uint32_t node, uint32_t prev_link); /* 0x344f9 (splice + free one handle) */
void     free_effect_pools(void);                             /* 0x343f5 (free all 3 lists + deferred) */
int32_t  apply_light_delta_to_record_list(uint32_t list, uint32_t edx_delta);              /* 0x4f4a4 */
int32_t  apply_flag_mask_to_record_list(uint32_t list, uint32_t ebx_clear, uint32_t edx_set); /* 0x4f4e8 */
int32_t  step_count_command(uint32_t rec, uint16_t seed);     /* 0x3192d (Count step body) */
int32_t  flush_pending_command_record(void);                  /* 0x31963 (Count flush tail) */
int32_t  step_count_apply_to_primary_cells(uint32_t rec, uint16_t seed);     /* 0x319c0 */
int32_t  step_count_apply_to_secondary_records(uint32_t rec, uint16_t seed); /* 0x31a94 */
int32_t  step_count_apply_to_geometry_faces(uint32_t rec, uint16_t seed);    /* 0x31b4f */
int32_t  cmd_smash_face_texture(uint32_t rec);               /* 0x31676 (RAW cmd base 0x2e) */
int32_t  cmd_change_face_texture(uint32_t rec);             /* 0x32738 (RAW cmd base 0x34) */
int32_t  resolve_command_objects(uint16_t id, uint32_t cap, uint32_t out); /* 0x34c19 (object resolver) */
uint32_t compute_player_object_bearing(uint32_t esi);        /* 0x30389 (player->object facing) */
void     mark_object_trigger_links(uint32_t objptr, uint16_t id); /* 0x30bb7 (rebuild trigger-link bits) */
int32_t  cmd_change_object_id(uint32_t rec);                 /* 0x31000 (RAW cmd base 0x3a) */
int32_t  cmd_rotate_object(uint32_t rec);                    /* 0x3146d (RAW cmd base 0x24) */
int32_t  cmd_06_empty_noop(uint32_t rec);                    /* 0x30f51 (RAW cmd base 0x06, inert) */
int32_t  cmd_empty_allow_sfx(uint32_t rec);                  /* 0x30b23 (RAW cmd base 0x3e, inert) */
int32_t  cmd_dbase100_if_next_fails(uint32_t rec);           /* 0x31326 (RAW cmd base 0x36) */
int32_t  cmd_map_transition(uint32_t rec);                   /* 0x3104a (RAW cmd base 0x3b) */
int32_t  cmd_toggle_command(uint32_t rec);                   /* 0x31563 (RAW cmd base 0x17) */
int32_t  cmd_player_rotation(uint32_t rec);                  /* 0x30acb (RAW cmd base 0x3f) */
int32_t  cmd_set_flag(uint32_t rec);                         /* 0x35617 (RAW cmd base 0x26) */
int32_t  cmd_spawn_object(uint32_t rec);                     /* 0x313b4 (RAW cmd base 0x16) */
int32_t  cmd_texture_change_count(uint32_t rec);             /* 0x3198e (RAW cmd base 0x1f, Count) */
int32_t  collect_secondary_state_records_by_key(uint16_t key, uint32_t cap, uint32_t out); /* 0x34c97 */
int32_t  cmd_count_addl_arg(uint32_t rec);                  /* 0x31a62 (RAW cmd base 0x22, Count) */
int32_t  collect_connected_geometry_group(uint16_t key, uint32_t cap, uint32_t out); /* 0x4f3d0 (ES) */
int32_t  cmd_animate_facegroup_texture(uint32_t rec);       /* 0x31b1a (RAW cmd base 0x21, Count) */
int32_t  cmd_cycle_texture(uint32_t rec);                   /* 0x3179c (RAW cmd base 0x1c) */
int32_t  cmd_sync_facegroup_texture(uint32_t rec);          /* 0x315c4 (RAW cmd base 0x14, LATENT) */
uint32_t walk_command_chain_flow(uint16_t ax);              /* 0x353c4 (spine flow/condition pre-pass) */
int32_t  finalize_command_chain(uint16_t ax);              /* 0x3065e (executor inner entry; ax preset) */
uint32_t find_secondary_state_record_by_key(uint16_t key);  /* 0x34d14 (first match -> host ptr) */
int32_t  cmd_cycle_object_texture(uint32_t rec);            /* 0x31700 (RAW cmd base 0x20) */
int32_t  execute_command_chain(uint32_t edi);              /* 0x3065a (the spine executor loop) */
int32_t  run_command_chain(uint32_t edi);                 /* 0x305f0 (the spine seeding wrapper; pusha-framed) */
int32_t  reset_command_chain_state(uint16_t ax);          /* 0x305b6 (reset state + run from AX) */
uint32_t texture_anim_command_hook(uint32_t eax_cwde, uint32_t edx_block); /* 0x33cf3 (code-ptr-only, via [0x90a34]; eax=sext16 id, edx=block) */
uint32_t texture_id_remap_hook(uint32_t id);              /* 0x33dde (code-ptr-only, via [0x8a2a0]; returns remapped-or-input id) */
uint32_t process_deferred_command_queue(void);            /* 0x3484b (swap+drain the deferred queue; ret residual eax) */
int32_t  register_command_save_link(uint32_t esi, uint16_t ax); /* 0x31f3c (reset+flush chain, latch save-link) */
int32_t  fire_trigger_on_contact(uint32_t esi);           /* 0x31fb0 (flow pre-pass -> chain run / gated tail) */
int32_t  fire_trigger_on_interact(uint32_t esi, uint32_t eax_in); /* 0x31ffe (interact variant; shares gated tail) */
int32_t  fire_command_contact_trigger(uint32_t rec);      /* 0x351c3 (contact entry; warp+active) */
int32_t  exec_object_contact_trigger(uint32_t rec);       /* 0x351cd (contact entry; inner, active) */
int32_t  exec_object_trigger_no_source(uint32_t rec);     /* 0x35201 (contact entry; warp, no active) */
int32_t  exec_object_trigger(uint32_t rec);               /* 0x3520b (contact entry; inner, no active) */
int32_t  begin_object_command_chain(uint32_t src);        /* 0x35303 (seed src ctx + fire resolved target) */
int32_t  run_object_commands_by_id(uint16_t ax);          /* 0x30549 (scan object table by id, run chain) */
int32_t  fire_wall_object_trigger(uint32_t rec, uint32_t vtxpair); /* 0x3534b (wall midpoint warp + fire) */
int32_t  tick_change_lighting(uint32_t rec);              /* 0x33b3b (lighting fade step) */
int32_t  tick_light_switch(uint32_t rec);                /* 0x33be2 (light-switch step + finalize) */
int32_t  tick_command_timer_queue(uint32_t count);       /* 0x345e2 (step the 0x89fc0 timer queue) */
int32_t  tick_cmd_45(uint32_t rec, uint32_t eax_in);     /* 0x339ff (connected-geometry light/flag pulse) */
void     flood_fill_geometry_neighbors(uint32_t es_base, uint16_t rec, int32_t *budget, uint8_t **cursor); /* 0x4f42d */
uint32_t snapshot_keyed_secondary_records(uint16_t key, uint32_t base); /* 0x32a20 (collect keyed obj recs -> chunk) */
int32_t  apply_object_state_to_group(uint32_t rec, uint32_t desc); /* 0x328aa (propagate member0 state to group) */
int32_t  tick_scroll_face_texture(uint32_t rec);          /* 0x32592 (UV scroll accumulator over a face group) */
int32_t  tick_scroll_sector_texture(uint32_t rec);        /* 0x324d2 (UV scroll w/ per-face direction flags) */
int32_t  tick_change_face_texture_adv(uint32_t rec);      /* 0x3354a (cell linked-pair swap + finalize tail) */
int32_t  tick_change_object_texture(uint32_t rec);        /* 0x3286b (object-texture group tick) */
void     swap_cell_state_group_v1(uint32_t cellrec, uint32_t ebp); /* 0x33255 (group state swap+broadcast) */
int32_t  tick_change_floor_texture(uint32_t rec);         /* 0x33229 (floor-texture tick; swap v1 + tail) */
void     swap_cell_state_group_v2(uint32_t cellrec, uint32_t ebp); /* 0x333ec (group swap variant 2) */
int32_t  tick_change_floor_texture_b(uint32_t rec);       /* 0x333c0 (floor-texture tick; swap v2 + tail) */
int32_t  tick_rotate_object(uint32_t rec);                /* 0x33188 (rotate-object tick; step / player-bearing) */
int32_t  tick_change_object_height(uint32_t rec);         /* 0x33091 (object-height ramp tick; 16-bit clamp) */
int32_t  tick_change_height(uint32_t rec);                /* 0x32d7d (sector floor/ceil ramp + player carry) */
int32_t  tick_modify_sector(uint32_t rec);               /* 0x335ec (sector modify: floor/ceil/light + carry) */
int32_t  fire_object_use_trigger(const regs_t *in);      /* 0x10d1e (trigger firer; ABI_FIRER, full reg ctx) */
int32_t  fire_entity_pending_trigger(const regs_t *in);  /* 0x42793 (entity pending-trigger firer) */
int32_t  fire_sector_trigger(const regs_t *in);          /* 0x31e8f (walk-over/sector trigger firer) */
int32_t  run_leftclick_object_trigger(const regs_t *in); /* 0x303ff (left-click object trigger firer) */
int32_t  fire_tracked_object_trigger(const regs_t *in);  /* 0x35260 (tracked-object contact trigger firer) */
int32_t  dispatch_entry_command_trigger(const regs_t *in);/* 0x34d75 (directional entry-trigger dispatcher) */
int32_t  dispatch_entry_command_trigger_b(const regs_t *in);/* 0x34f5a (category-B entry-trigger dispatcher) */
int32_t  tick_world_effects(const regs_t *in);           /* 0x3464c (per-frame active-effect orchestrator) */
void     init_sector_object_state(uint32_t geom, uint32_t objects); /* 0x4f1a0 (level-load per-object reset) */
int32_t  object_has_active_trigger_link(uint32_t rec);    /* 0x30c1a (read-only trigger-link predicate) */
uint32_t alloc_particle(void);                            /* 0x4b485 (claim a free particle slot) */
int32_t  resolve_object_template_record(uint32_t eax);    /* 0x1de59 (dbase100 template lookup by id) */
int32_t  tick_flash_lights(uint32_t rec);                 /* 0x32324 (per-frame light-flash effect) */
int32_t  set_state_record_count(uint32_t eax);            /* 0x33c3e (latch count + reset bridge) */
void     rebuild_pool_a_object_pointers(void);            /* 0x42cf9 (rebuild GS-arena back-ptr caches) */
void     reserve_object_buffer_space(void);              /* 0x420e1 (compact/grow the object buffer) */
void     remove_secondary_state_record(uint32_t eax, uint32_t ebx); /* 0x42056 (drop a record + shift tail) */
void     release_object_secondary_state(uint32_t eax, uint32_t ebx); /* 0x42014 (drop record + clear empty flag) */
void     insert_object_arena_gap(uint32_t ecx, uint32_t edx);  /* 0x4222b (open a gap in the GS object arena) */
uint32_t alloc_object_record_slot(uint32_t eax);          /* 0x42199 (reserve next record slot, ret GS off) */
uint32_t alloc_object_record_ensuring_space(uint32_t eax); /* 0x42189 (ensure space then reserve slot) */
void     add_secondary_state_record(uint32_t eax, uint32_t ecx, uint32_t edx); /* 0x42c72 (move record into pool) */
void     apply_floorceil_move_to_group(uint32_t esi, uint32_t ebp); /* 0x3427b (drive floor/ceiling group move) */
int32_t  resolve_conditional_command(uint32_t eax, uint32_t rec); /* 0x35580 (pre-evaluated conditional cmd) */
uint32_t point_to_wall_distance_sq(uint32_t rec, uint16_t radius); /* 0x3e03f (GS; player->wall dist^2) */
int32_t  cmd_apply_damage(uint32_t rec);                  /* 0x320e6 (RAW cmd base 0x33) */
int32_t  cmd_face_emits_damage(uint32_t rec);             /* 0x30f83 (RAW cmd base 0x35) */
uint32_t alloc_active_effect(uint16_t key, uint32_t size, uint32_t flag); /* 0x32910 (effect alloc) */
int32_t  cmd_light_switch(uint32_t rec);                  /* 0x33b94 (RAW cmd base 0x02, registrant) */
int32_t  cmd_change_floor_texture(uint32_t rec);          /* 0x32626 (RAW cmd base 0x0a/0x0b, registrant) */
int32_t  cmd_scroll_sector_texture(uint32_t rec);         /* 0x32473 (RAW cmd base 0x0e, registrant) */
int32_t  cmd_change_lighting(uint32_t rec);               /* 0x33ac4 (RAW cmd base 0x1d, registrant) */
int32_t  cmd_delay_timer(uint32_t rec);                  /* 0x32195 (RAW cmd base 0x12, fixed-alloc registrant) */
int32_t  cmd_flash_lights(uint32_t rec);                 /* 0x32269 (RAW cmd base 0x11, face registrant) */
int32_t  cmd_change_height(uint32_t rec);                /* 0x3121c (RAW cmd base 0x07, height registrant) */
int32_t  cmd_change_object_height(uint32_t rec);         /* 0x31107 (RAW cmd base 0x23, obj-height registrant) */
int32_t  cmd_modify_sector(uint32_t rec);                /* 0x312a1 (RAW cmd base 0x03, =0x07 w/ SFX key @+0x14) */
int32_t  cmd_change_object_texture(uint32_t rec);        /* 0x327f8 (RAW cmd base 0x0d, obj-texture registrant) */
int32_t  cmd_move_sector(uint32_t rec);                  /* 0x32ac5 (RAW cmd base 0x09, sector-move registrant) */
int32_t  cmd_scroll_face_texture(uint32_t rec);          /* 0x324a7 (RAW cmd base 0x0f, FACE registrant) */
int32_t  cmd_if_not_item(uint32_t rec);                           /* 0x35544 (RAW cmd base 0x27, multi-path conditional) */
int32_t  cmd_if_not_flag(uint32_t rec);                           /* 0x355a7 (RAW cmd base 0x28, flag-query conditional) */
int32_t  cmd_give_item(uint32_t rec);                    /* 0x35437 (RAW cmd base 0x29, inventory give + drop pos) */
int32_t  cmd_remove_item(uint32_t rec);                  /* 0x354d3 (RAW cmd base 0x2a, inventory remove) */
int32_t  cmd_set_inventory_filter(uint32_t rec);         /* 0x30f63 (RAW cmd base 0x42, "Take Inventory" = display filter) */
int32_t  cmd_particle_effect(uint32_t rec);             /* 0x311ad (RAW cmd base 0x2d, particle burst) */
int32_t  cmd_activate_sfx_node(uint32_t rec);           /* 0x31339 (RAW cmd base 0x10, SFX-node toggle) */
int32_t  cmd_run_indexed_object_command(uint32_t rec);  /* 0x304b8 (RAW cmd base 0x40, mini-executor) */
int32_t  cmd_modify_count(uint32_t rec);                /* 0x31c31 (RAW cmd base 0x1e, arm+re-run a Count cmd) */
int32_t  cmd_open_door(uint32_t rec);                   /* 0x33a69 (RAW cmd base 0x2f, register a door swing) */
int32_t  run_command_dbase100_record(uint32_t rec);     /* 0x3540b (RAW cmd base 0x2b, run the global DBASE100 record cmd+8 points to) */
int32_t  cmd_change_face_texture_adv(uint32_t rec);     /* 0x32645 (RAW cmd base 0x0c; register path native, immediate path bridged-UB) */
int32_t  cmd_spawn_object_adv(uint32_t rec);            /* 0x30d10 (RAW cmd base 0x3c; spawn object/projectile/unhide) */
/* RAW effect-TICK shared "mark records by key" leaves (each entry stub fixes the OR mask; the collector
 * 0x34c97 is already declared above). */
void     mark_geometry_records_by_id(uint32_t rec);     /* 0x31cdd (mask from rec[7] -> byte[rec+0x17]) */
void     mark_geom_faces_b20(uint32_t rec);             /* 0x31d2b -> mark_geometry_faces_by_key cl=0x20 */
void     mark_geom_faces_b10(uint32_t rec);             /* 0x31d2f -> cl=0x10 */
void     mark_raw_state_b04(uint32_t rec);              /* 0x31d7a -> mark_raw_state_records_by_key cl=0x4 */
void     mark_raw_state_b02(uint32_t rec);              /* 0x31d7e -> cl=0x2 */
void     mark_raw_state_b01(uint32_t rec);              /* 0x31d82 -> cl=0x1 */
void     mark_objects_b08(uint32_t rec);                /* 0x31dcb -> mark_object_records_by_key cl=0x8 */
void     mark_objects_b04(uint32_t rec);                /* 0x31dcf -> cl=0x4 */
void     mark_objects_b20(uint32_t rec);                /* 0x31dd3 -> cl=0x20 */
void     copy_path_ensure_trailing_slash(const uint8_t *esi, uint8_t *edi); /* 0x11057 */
void     copy_nonzero_bytes_2x(uint8_t *dst, uint32_t ebx, const uint8_t *src); /* 0x1428a */
void     begin_item_pickup_lock(uint32_t eax, uint32_t edx, uint32_t ebx, uint16_t cx); /* 0x1622d */
void     compute_screen_extents_7e8b0(void);                   /* 0x115b5 */
void     compute_view_offsets_90a74(void);                     /* 0x12179 */

/* Batch 22 — clamps, pool search, codecs. */
int32_t  approach_value(int32_t eax, int32_t edx, int32_t ebx);          /* 0x1c630 */
int32_t  clamp_symmetric_26f2d(int32_t eax, int32_t edx, int32_t ebx);   /* 0x26f2d */
uint32_t find_active_effect(uint8_t al, uint32_t esi);                   /* 0x32606 */
uint32_t decode_dpcm_block(uint32_t eax, const uint8_t *src, int16_t *dest, int32_t count); /* 0x4e4cd */
void     interpolate_words_43dd8(int16_t *dst, const int16_t *src, int32_t ebx);  /* 0x43dd8 */
void     write_ror_ramp_3bb1e(uint32_t edi, uint16_t cx, uint32_t ebp);  /* 0x3bb1e */

/* Batch 23 — mid-size game logic. */
void     apply_damage_to_player(uint32_t eax, uint8_t dl, uint32_t ecx);  /* 0x32023 */
uint32_t find_free_inventory_slot(void);                                  /* 0x1ce43 */
uint32_t stack_onto_inventory_slot(uint32_t eax_in);                      /* 0x1ce14 */
void     init_sprite_render_queue(void);                                  /* 0x3c294 */
void     swap_voice_double_buffers(void);                                 /* 0x1e3b0 */
void     build_game_path(uint8_t *edi, const uint8_t *esi, const uint8_t *ebx); /* 0x2fb7f */
int      check_entity_sector_clearance(uint32_t eax, uint32_t edi);       /* 0x42c04 (CF) */

/* Batch 24 — named game logic. */
void     build_dpcm_step_table(void);                                     /* 0x4bb62 */
void     setup_sfx_nodes(void);                                           /* 0x43c46 */
void     reset_entity_pools(void);                                        /* 0x4263e */

/* Batch 25 — clean leaves + case-1 FS palette remap. */
uint32_t memcpy_return_dest(uint8_t *dst, const uint8_t *src, uint32_t n); /* 0x4f2b7 */
void     repair_das_rle_frame_count(uint16_t *p, int32_t size);            /* 0x41631 */
void     apply_ui_palette_rect(int32_t x0, int32_t y0, int32_t y1,
                                      uint32_t level, int32_t x1, uint8_t *fs);    /* 0x12c36 (FS self-load) */

/* Batch 26 — inventory reset + case-1 self-load SEG cluster (map-load metadata). */
void     reset_inventory(void);                                            /* 0x1c57e */
void     fixup_raw_sectors_after_load(uint8_t *fs);                        /* 0x2f782 (FS self-load) */
void     init_player_position_from_metadata(uint8_t *gs);                  /* 0x2f8a2 (GS self-load) */
void     init_map_lighting_from_metadata(uint8_t *gs);                     /* 0x2f8fa (GS self-load) */

/* Batch 27 — Pool allocator constructors (the DAS-cache handle pool, magic 0x506f6f6c). */
uint32_t pool_init(uint32_t *pool, int32_t hdrsize, int32_t size);         /* 0x35b68 */
uint32_t pool_create(uint32_t *block, int32_t nhandles, int32_t size);     /* 0x36088 */

/* Batch 28 — Pool chunk free + coalesce (g_pool_check_enabled=0 -> get_pool_descriptor is identity). */
uint32_t pool_free_chunk(uint32_t *pool, uint8_t *block);                  /* 0x35d80 */

/* Batch 29 — Pool free worker (shared body, 2nd entry) + max-free stats helper. */
uint32_t pool_release_chunk(uint32_t *pool, uint8_t *block);               /* 0x35d96 */
void     pool_recompute_max_free(uint8_t *pool);                           /* 0x35d52 */

/* Batch 30 — Pool handle free (the 47-caller object-free hub). */
uint32_t pool_free_handle(uint32_t *pool, uint32_t *handle);               /* 0x360b3 */

/* Batch 31 — Pool alloc path (carve a chunk from the free space). */
uint32_t pool_find_free_chunk(uint32_t *pool, int32_t size);               /* 0x35cb4 (raw; last-fit, alloc high) */
uint32_t pool_carve_chunk(uint32_t *pool, int32_t size);                   /* 0x35c1d (handle; first-fit, alloc low) */

/* Batch 32 — Pool handle alloc hub (fast path verified; slow path bridges pool_coalesce_free). */
uint32_t pool_alloc_handle(uint32_t *pool, int32_t size);                  /* 0x360f9 (26-caller hub) */
uint32_t pool_alloc_handle_sized(uint32_t *pool, int32_t size);            /* 0x3618c */

/* Batch 33 — Pool compaction (slides allocated chunks down, one trailing free chunk). */
uint32_t pool_coalesce_free(uint32_t *pool);                               /* 0x361f7 */

/* Batch 34 — dbase100 id lookup + entity-player contact test. */
uint32_t lookup_dbase100_record_by_id(uint32_t id);                        /* 0x1dcac */
int32_t  check_entity_player_contact(uint32_t eax, const uint8_t *esi, const int16_t *edi); /* 0x43413 */
extern int g_os_contact_sf;   /* SF at 0x43413's ret — the caller's `js` predicate (NOT the EAX32 sign) */

/* Batch 35 — combat: entity-sector refresh + projectile hit damage. */
int      revalidate_entity_def(uint8_t *esi, uint8_t *ebx);               /* 0x426fc (CF; mismatch bridges entity_def_cache_lookup) */
uint32_t compute_projectile_hit_damage(uint32_t *param1, uint16_t dx);     /* 0x427f3 */

/* entity_ai cluster A — pool/def-cache leaves (lift_entity_ai.c).
 * All call-closed (dbase100 callees already lifted). EAX returns are real (corpus shows void — A1). */
uint32_t entity_def_cache_build_entry(uint32_t dest, uint32_t id);          /* 0x1e2a4 (0 = no record; else build's 0/1) */
uint32_t resolve_object_owner_sector(uint32_t objrec);                      /* 0x4f263 (sector rec offset; quirk: last word on miss) */
uint32_t entity_def_cache_lookup(uint32_t id);                              /* 0x1e2f6 (LRU node ptr / 0; MTF mutates links) */

/* dos_runtime cluster C — the DOS/DPMI memory-block allocators (lift_dos_runtime.c), over the
 * C2 call-API (os_api.h). CRT-klass helpers lifted for call-closure marked (CRT). */
uint32_t alloc_dpmi_block(uint32_t size);                                   /* 0x35a74 (CRT; hdr list 0x8a270) */
uint32_t alloc_dos_block(uint32_t size);                                    /* 0x35a12 */
uint32_t free_os_block(uint32_t ptr);                                       /* 0x35b0a (ret 0) */
uint32_t free_os_block_guarded(uint32_t ptr);                               /* 0x35af2 (pool 0x8a274 else free_os_block) */
uint32_t dpmi_alloc_dos_memory(uint32_t bytes, uint32_t *sel_io);           /* 0x40a34 (CRT; DX in/out, untouched on fail) */
void     ensure_dos_transfer_buffer(uint32_t edx_in);                       /* 0x40b08 (0x8c1f8/0x8c1fc) */

/* dos_runtime cluster D — the timer-tick ISR chain (in-game tier; ISR bodies never int3-swapped). */
void     vsync_timer_tick(void);                                            /* 0x122e3 (tick + cursor/movement + hook bridge) */
void     wait_one_timer_tick(void);                                         /* 0x2e91a (INTERACTIVE busy-wait) */
void     game_heartbeat_timer_isr(void);                                    /* 0x12336 (INT 8 body; runtime_support) */

/* dos_runtime cluster D tail — the exception / critical-error handler pairs (C2). */
void     install_exception_handler(uint32_t eax, uint32_t edx);             /* 0x416d3 (#0 hook; saves to 0x90d1c..) */
void     restore_exception_handler_and_report(uint32_t eax);                /* 0x41674 (bit0 = divide-count report) */
void     install_critical_error_handler(void);                              /* 0x436e8 (INT 24h auto-FAIL stub; 0x911cc..) */
void     restore_critical_error_handler(void);                              /* 0x43775 */

/* dpmi_dos_os — the thin DOS file wrappers lifted over the C2 file service (os_api.h;
 * lift_dpmi_dos_os.c). C2 deliverables (call-closure), NOT rows in the 1171 engine gate. Watcom
 * register ABI per the disasm; verified by the c2_mock file-I/O differential (test_c2_fileio.c). */
uint32_t dos_open_file(uint32_t path, uint32_t mode);                        /* 0x41ae5 (EAX=path, EDX=mode {0=rd,1=create,2=rdwr+seekEND}) */
void     dos_close_handle(uint32_t handle);                                  /* 0x41b41 (EAX=handle; skips fd<=2) */
uint32_t dos_read_items(uint32_t buf, uint32_t isz, uint32_t n, uint32_t handle);   /* 0x41b53 (EAX=buf,EDX=isz,EBX=n,ECX=handle -> items) */
uint32_t dos_write_items(uint32_t buf, uint32_t isz, uint32_t n, uint32_t handle);  /* 0x41b7a (same ABI; guards handle==0 only) */
uint32_t dos_lseek(uint32_t handle, uint32_t off, uint32_t whence);          /* 0x41b9a (EAX=handle,EDX=off,BL=whence -> pos) */
/* the DAS-cache block LDT-selector pair (over os_api.h 0000/0007/0008/0001) + the
 * DOS-memory free freebie. ESI = record; setup returns CF (1=fail) and frees the descriptor on a
 * set-base/set-limit failure; refresh is void (CF leaked at ret, no consumer). */
int      setup_das_block_selector(uint32_t rec, uint32_t endp);              /* 0x41191 (ESI=rec, EDI=endp -> CF) */
void     refresh_das_block_selector_base(uint32_t rec);                      /* 0x412ed (ESI=rec) */
void     dpmi_free_dos_memory(uint32_t sel);                                 /* 0x40a50 (AX=selector; skip 0) */

/* entity_ai clusters B+D — spawn/destroy + the think loop (lift_entity_ai.c). All
 * call-closed; LIVE-SWAP tier (group ROTH_LIFT=entity_ai — non-idempotent, no oracle). */
void     destroy_dynamic_entity(uint32_t eax_obj, uint32_t edx_dest);       /* 0x41f24 (EDX = 16-byte dest scratch) */
uint32_t spawn_entity_at_position(const regs_t *in);                        /* 0x4254e (6 reg args -> EAX rec / 0) */
int32_t  spawn_entity_into_state_pool_a(const regs_t *in);                  /* 0x4f00d (AH idx + ESI obj -> EDX backref / 0) */
void     update_dynamic_entities(void);                                     /* 0x42872 (walker/projectile/dying body) */
int32_t  tick_dynamic_entities(const regs_t *in);                           /* 0x42d74 (per-frame entry; returns in->eax) */

/* entity_ai cluster C — locomotion/aim (lift_entity_ai.c). All call-closed. */
void reset_entity_state_with_sound(uint32_t edi, uint32_t edx);             /* 0x4273e (EDI=entity, EDX=threshold) */
void collide_entity_and_steer(int32_t ecx_dx, int32_t edx_dy, uint8_t *frame); /* 0x3ea10 (frame >= 0x40 B — the caller reads +0/+2, the orig's dead-EBP channel) */
void move_entity_with_collision(int32_t dx, int32_t dy, uint32_t ebx, uint32_t ecx); /* 0x3e590 (EBX=entity, ECX=ctx; pushal/popal -> void) */
void entity_apply_vertical_movement(int32_t dx, int32_t dy, uint32_t esi, uint32_t edi); /* 0x43187 (ESI=entity, EDI=subrec; ctx block 0x911a4) */
void tick_entity_vertical_only(uint32_t edi_entity);                        /* 0x43402 */
void aim_enemy_at_player(uint32_t limit_eax, uint32_t edi);                 /* 0x4355a (D1: LCG 0x72730) */
void update_actor_movement_ai(uint32_t eax, uint32_t edi_entity);           /* 0x43326 (EAX threaded into the contact test) */

/* Batch 36 — config asset-name + small leaves. */
void     set_cfg_asset_name(const uint8_t *esi, uint8_t *edi, uint32_t edx_ext); /* 0x10584 */
uint32_t rng_next(uint32_t eax);                                           /* 0x4b4cb (LCG, AX) */
void     clear_list_field30(void);                                         /* 0x4b378 (list @0x91864) */
void     bounded_string_copy(const uint32_t *ebp, uint8_t *edi, int8_t cl); /* 0x27e0b */

/* Batch 37 — RNG-range, dual-array clear, RLE literal-run emitters. */
uint32_t rng_range(uint32_t range);                                        /* 0x16d79 */
void     clear_dual_array_80afc(void);                                     /* 0x1c59e */
uint8_t *emit_literal_run_3cf86(uint32_t count, uint8_t *esi_end, uint8_t *edi); /* 0x3cf86 */
uint8_t *emit_literal_run_4ee9c(uint32_t count, uint8_t *esi_end, uint8_t *edi); /* 0x4ee9c */

/* Batch 38 — table searches + locomotion reset. */
uint32_t is_in_83ed4_table(uint32_t key);                                  /* 0x2778d */
uint32_t find_sfx_node_by_key(uint16_t key);                            /* 0x43b0b */
void     reset_player_locomotion_state(void);                              /* 0x1c96f */

/* ---- math_util subsystem (lift_math_util.c) — shared math/string/format leaves. ---- */
/* Layer 1 — pure leaves. */
void     shared_epilogue_6reg(void);                                       /* 0x18a23 (naked Watcom epilogue thunk) */
void     thunk_build_atan_table(void);                                     /* 0x3c28c (-> build_atan_table) */
uint32_t isqrt_fixed_wrapper_3bfd6(uint32_t eax);                          /* 0x3bfd6 (movzx isqrt_fixed) */
void    *mem_fill(void *dst, uint32_t val, uint32_t count);                /* 0x4b360 (memset wrapper, EAX=dst) */
void     rotate_point_2d_shifted(int16_t x_in, int16_t y_in,
                                        int32_t *eax_out, int32_t *edx_out);       /* 0x2b25b (EAX/EDX pair) */
int32_t  stricmp(const uint8_t *s1, const uint8_t *s2);                     /* 0x5607b (EAX=s1,EDX=s2) */
int32_t  compare_name_token_ci(const uint8_t *arg1, const uint8_t *arg2);  /* 0x1063d (basename ci cmp -> 0/-1) */
uint32_t match_word_in_list_ci(const uint8_t *word, const uint8_t *list);  /* 0x150b8 (-> entry index|0) */
uint32_t copy_switch_token_upper(const uint8_t *src, const uint8_t **end); /* 0x10920 (ESI=src; buf@0x76719/len@0x76718; *end=advanced ESI) */
/* Layer 2 — the vsprintf chain. */
void     format_decimal_grouped(int32_t eax, uint8_t cl, uint8_t **edi_io); /* 0x27e46 (int->decimal w/ grouping) */
uint32_t vsprintf_core(const uint8_t *fmt, uint8_t *out, const uint32_t *args); /* 0x27ca6 (printf engine; shared by entry stubs 0x27c91/0x27c98/0x27ca0) */

/* ---- memory_pool subsystem (lift_memory_pool.c) — game-heap / 'Pool' allocator wrappers. ---- */
/* Layer 1 — leaves (over the already-lifted Pool core). */
uint32_t query_game_heap_free(void);                                       /* 0x151a5 */
void     game_free_if_not_null(uint8_t *block);                            /* 0x40a2a (EAX=block) */
uint32_t game_heap_alloc_round4(int32_t size);                             /* 0x40a17 (EAX=size -> ptr) */
uint32_t alloc_resource_pool_block(int32_t size);                          /* 0x26b38 (EAX=size) */
uint32_t free_resource_chunk(uint8_t *block);                              /* 0x26b4c (EAX=block) */
uint32_t init_resource_chunk_pool(uint32_t *block, int32_t size);          /* 0x35b53 (EAX=block,EDX=size) */
void     free_block_or_pool(uint8_t *block);                               /* 0x15280 (EAX=block; dos path bridged) */
uint32_t get_pool_descriptor(uint32_t eax);                                /* 0x35f04 (debug integrity walker -> pool) */
/* Layer 2/3 — mid + teardown. */
uint32_t compute_heap_grow_size(uint32_t *ptr);                            /* 0x57002 (EAX=ptr -> bool; 0x56ce5 bridged) */
void     free_resource_buffers(void);                                      /* 0x26c8c (teardown; dos/das/audio bridged) */
void     free_heap_block(uint8_t *block);                                  /* 0x35bfa (null-safe free_os_block bridge) */
uint32_t alloc_block_or_heap(int32_t size);                                /* 0x15210 (alloc_dos_block bridge | game_heap_alloc) */
uint32_t alloc_largest_heap_block(uint32_t eax_in);                        /* 0x35ff9 (DPMI startup heap alloc; in-game) */

/* ---- collision_physics subsystem (lift_collision_physics.c) — wall/sector/object collision + sweep. ---- */
/* Layer A — distance / clip / scan leaves (pure fixed-point geometry). */
int      roth_test_ray_reached_target(void);                                    /* 0x405f6 (target-reached predicate -> CF) */
int      scan_sector_edges_at_y(uint32_t sector_off, void *ebp_scratch);   /* 0x3efb0 (point-in-sector scanline test -> CF) */
int      scan_portal_walls_near_query(uint32_t eax_sector, uint16_t dx);   /* 0x3db20 (portal-wall proximity -> CF) */
int      clip_query_circle_to_edge(void *ebp_frame);                       /* 0x3fe2d (circle-vs-edge sweep clip) */
int      clip_locate_query_to_object(uint32_t obj_ptr, void *ebp_frame);   /* 0x3fdb0 (clip vs object 4-edge poly) */
void     collide_ray_walls_recursive(uint32_t sector, void *ebp_frame);    /* 0x3fae0 (portal-recursive wall cast) */
void     collide_point_walls_recursive(uint32_t sector, void *ebp_frame);  /* 0x3f0e0 (portal-recursive point sweep + push-out/slide) */
/* Layer B — sector search / location. */
int      search_sector_from_hint(uint32_t hint_eax, void *ebp_scratch, uint32_t *out_sector); /* 0x3eceb (-> CF) */
int      search_sector_neighbors(uint32_t sector_eax, void *ebp_scratch, uint32_t *out_sector);/* 0x3f090 (-> CF) */
int      search_sector_global(void *ebp_scratch, uint32_t *out_sector);                       /* 0x3edf0 (-> CF) */
int      find_query_sector(uint32_t hint_eax, void *ebp_scratch, uint32_t *out_sector);       /* 0x3ec40 (hint->nbr->global) */
/* Layer C — collision resolve / dispatch. */
uint32_t collide_sector_walls(uint32_t sector, void *ebp_frame);                              /* 0x3ef21 (seed Z-window + dispatch point/ray walker) */
int      collide_ray_entities(void *ebp_frame);                                               /* 0x4066f (tracked-object list: clip/distance -> hit flags -> CF) */
int      resolve_collisions_against_objects(void *ebp_frame);                                 /* 0x401cf (iterative box/circle push-out vs worklist, up to 4 passes -> CF) */
void     resolve_collisions_in_sector(uint32_t sector, void *ebp_frame);                      /* 0x3eeb0 (per-sector collision orchestrator: walker + worklist resolve, looped) */
int      find_sector_and_collide(uint32_t query_ctx, void *ebp_frame);                        /* 0x3ec5a (locate sector [hint/nbr/global] -> resolve_collisions_in_sector -> CF) */
void     collide_substep_track_sector(uint32_t query_ctx, void *ebp_frame);                   /* 0x3ecc0 (find_sector_and_collide + cache sector in g_player_sector_cache 0x8c1d6) */
void     probe_collision_step(int32_t ecx_dx, int32_t edx_dy);                                /* 0x3eb90 (collision probe substep: own frame, in-sector test + portal cross + restore) */
void     sweep_move_with_collision(uint32_t velX, uint32_t dest_ptr, uint32_t velY,
                                          uint32_t magnitude, uint32_t velZ, uint32_t entity_ptr); /* 0x3e351 (swept entity move: substep + probe -> writeback) */
void     move_player_with_collision(uint32_t query_ctx);                                   /* 0x3e796 (player move: vel-queue integrate via track_sector + view-bob) */
uint32_t locate_sector_at_position(uint32_t eax_x, uint32_t edx_y, uint32_t ebx_z, uint32_t ecx_hint); /* 0x3ee4b (entry) */

/* automap subsystem; lift_automap.c. The dev-mode overhead map. */
void draw_bresenham_line(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx);  /* 0x2ed21 (AX,BX,CX,DX coords) */
/* clip leaves: coords in/out via low 16 of *io_e{ax..dx}; ESI=rect bound ptr; return CF (1=reject). */
int  map_line_clip_test(uint32_t *io_eax, uint32_t *io_ebx, uint32_t *io_ecx, uint32_t *io_edx, uint32_t esi); /* 0x2eebc */
int  clip_map_line     (uint32_t *io_eax, uint32_t *io_ebx, uint32_t *io_ecx, uint32_t *io_edx, uint32_t esi); /* 0x2ee66 */
void map_draw_world_edge(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx);  /* 0x2ec5f (AX,BX,CX,DX world coords) */
void map_draw_player_marker(uint32_t eax, uint32_t ecx, uint32_t edx);             /* 0x2eba7 (EAX=angle,ECX=offx,EDX=offy) */
void automap_draw_doors(void);                                                     /* 0x2eb3a */
void automap_draw_entity_markers(void);                                            /* 0x2ea9f */
void render_map_geometry(uint32_t eax, uint32_t edx, uint32_t ebx);                /* 0x2e954 (EAX=buf,EDX=pitch,EBX=descriptor) */
void draw_map_overlay(void);                                                       /* 0x10dce (entry) */

/* doors subsystem; lift_doors.c. Door pool + swing + open/close + queries.
 * Layer A leaves: return record ptr via *out_eax + CF (return value; 1 = not found). */
int find_door_by_sector(uint16_t si, uint32_t eax_in, uint32_t *out_eax);          /* 0x3cfcc (SI=sector) */
int find_next_door(uint32_t cur, uint16_t si, uint32_t *out_eax);                  /* 0x3cff6 (EAX=cur,SI=sector) */
int is_door_open(uint16_t di, uint32_t eax_in, uint32_t *out_eax);                 /* 0x3d03c (DI=sector; AL=open bit) */
void init_door_pool(void);                                                         /* 0x3d433 (reset pools + build 6 record templates) */
int  compute_door_quad_bounds(uint32_t edi);                                       /* 0x3de36 (EDI=record; query-in-quad? ret CF) */
int  update_door_swing(uint32_t eax, uint32_t edi);                                /* 0x3de31 (rotate_quad then compute_door_quad_bounds) */
uint32_t lookup_door_record_by_sector(uint32_t eax_in, uint32_t fs_base);          /* 0x3d4da (EAX=geom off; FS chained deref -> key; scan both pools) */
/* corner-surface pair: EAX=out record, EBX/EDX=geom indices, EDI=di index, EBP=frame, fs=geom base. */
void setup_door_corner_surface_b(uint32_t eax, uint32_t ebx, uint32_t edi, uint32_t ebp, uint32_t fs); /* 0x3d3a8 */
void setup_door_corner_surface_a(uint32_t eax, uint32_t ebx, uint32_t edx,
                                        uint32_t edi, uint32_t ebp, uint32_t fs);          /* 0x3d387 (flow_succ -> _b tail) */
int  resolve_door_neighbor_sector(uint32_t eax_in, uint32_t fs);                   /* 0x3d749 (FS graph walk -> neighbour sector; obj3 0x8c0cc; ret CF) */
uint32_t collect_doors_near_query(uint32_t ebx, uint32_t esi_rec, uint32_t edi_out); /* 0x3fa62 (append nearby walls; ret advanced EDI) */
void gather_nearby_doors(void);                                                    /* 0x3f93b (build the nearby-interactables worklist; composes the leaves) */
int  setup_door_swing_geometry(uint32_t eax_in, uint32_t esi_rec, uint32_t edx_in,
                                      uint32_t edi_in, uint32_t fs, uint32_t gs);          /* 0x3d147 (build the swing record from FS graph + GS verts; ret CF) */
int  test_door_query_near_player(uint16_t bx, uint32_t edi);                        /* 0x3dafb (Z gate -> scan_portal_walls_near_query; ret CF) */
uint32_t alloc_door_record(uint32_t eax_in, uint32_t edx_in, uint32_t edi_in,
                                  uint32_t fs, uint32_t gs);                               /* 0x3d6c3 (alloc primary door slot + setup + open SFX; ret slot|0) */
uint32_t alloc_secondary_door_slot(uint16_t di, uint32_t fs);                       /* 0x3d7b9 (alloc+init secondary door slot from FS graph; ret slot|0) */
uint32_t spawn_door_instance(uint32_t eax_ax, uint32_t ecx, uint32_t ebx_in,
                                    uint32_t edx, uint32_t fs, uint32_t gs);               /* 0x3d586 (door-spawn orchestrator; ret slot|0) */
uint32_t register_door_swing(uint32_t eax_ax, uint32_t ecx, uint32_t ebx_in,
                                    uint32_t edx_params, uint32_t edi_param, uint32_t fs, uint32_t gs); /* 0x3d54b (param entry -> spawn) */
void dev_open_nearest_door(uint32_t fs_walk, uint32_t gs_walk,
                                  uint32_t fs_spawn, uint32_t gs_spawn);                    /* 0x3df96 (dev: spawn a door on the nearest in-front portal) */
/* Layer-D per-frame ticks + open-toggle (LIVE-SWAP ONLY; mutate live world
 * geometry + play sounds, so NOT in the oracle). The two ticks resolve FS internally via g_os_sel_base. */
uint32_t toggle_door_open_state(uint32_t eax_in);                                   /* 0x3d93f (EAX=rec off; bridges 0x3d8f2) */
void tick_secondary_doors(uint32_t edx_in);                                         /* 0x3d98f (EDX=frame step; sliding doors) */
void tick_swinging_doors(uint32_t edx_in);                                          /* 0x3db8d (EDX=frame step; hinged doors + pool compaction) */
void tick_doors_for_frame(void);                                                    /* 0x3d959 (frame dispatcher -> both ticks) */

/* savegame subsystem; lift_savegame.c. Save/load serialization.
 * Layer A — state-chunk leaves: take a single flat RUNTIME pointer (raw-state buffer / object
 * table); deref RAW (A4). Oracle-verified by obj3 write-set diff vs call_orig over a staged buffer. */
void strip_transient_flags_for_save(uint32_t buf);                                  /* 0x31e14 (EAX=raw-state buf; clear transient flag bits) */
uint32_t write_player_state_chunk(uint32_t rec);                                    /* 0x3e0f0 (EAX=dest 0x30-byte record; serialize player globals; ret 0x30) */
void read_player_state_chunk(uint32_t rec);                                         /* 0x3e1a0 (EAX=src 0x30-byte record; restore player globals) */
uint32_t write_state_object_links(uint32_t obj, uint32_t rec, uint8_t type, uint32_t dst); /* 0x35735 (EAX=obj,EDX=rec,CL=type,EDI=dst; ret advanced EDI) */
uint32_t write_state_record_list(uint32_t dst);                                    /* 0x35648 (EAX=dst stream; walk link directory + trailer; ret chunk byte size) */
void build_screenshot_filename(void);                                              /* 0x114e2 (build "C:\ANIM<n>.lbm" into g_snapshot_filename_buf 0x8b370) */
void load_state_dynamic_entities(uint32_t rec);                                     /* 0x4ef61 (EAX=count-prefixed chunk; restore entity pools 0x91e04/0x90fe4, reloc + bridge def cache) */
uint32_t load_state_record_list(uint32_t list);                                     /* 0x3580c (Layer C, LIVE-SWAP; EAX=stream -> walk records calling load_state_object_links) */
int32_t  load_state_object_links(const regs_t *in);                                 /* 0x35839 (Layer C; ABI_REGS_EAX, EAX=u16 idx/EDI=stream -> EAX bytes; typed link deserialize) */
int32_t  resolve_state_link_target(const regs_t *in);                               /* 0x359ad (Layer C; ABI_REGS_EAX, ESI=obj/EDI=stream -> EDX target; bridges RAW 0x30780 dispatch) */
void write_snapshot_lbm(void);                                                      /* 0x3cb85 (build "C:\Snap<n>.lbm", counter++, flag 0x90c04, alloc 64KB capture buf -> 0x8b35c) */
void save_snapshot_file(void);                                                      /* 0x3cc02 (Layer C, LIVE-SWAP; deferred ILBM writer via g_os_soft_int int 0x21; PackBits BODY) */
void take_snapshot(void);                                                           /* 0x11135 (Layer C, LIVE-SWAP + INTERACTIVE; int 0x10 mode-sets, drives the menu, arms the capture) */
void snapshot_menu_and_save(void);                                                  /* 0x111a0 (Layer C; interactive DOS-console menu; called directly by lifted take_snapshot, cold int3) */
void capture_screen_thumbnail(uint32_t dst, uint32_t src, uint32_t lut);            /* 0x142b7 (EAX=dst; 2x2-avg downscale screen -> 78x56 thumbnail via fs blend LUT) */
uint32_t bundle_level_states(uint32_t save_handle);                                 /* 0x2198e (Layer C, LIVE-SWAP; EAX=save handle -> 1 ok/0 fail; bundle per-level temp state files) */
uint32_t copy_save_chunk_to_file(uint32_t name, uint32_t size, uint32_t src_handle, uint32_t buffer); /* 0x21afd (Layer C; EAX=name,EDX=size,EBX=src,ECX=buf -> 1/0; extract a chunk to a temp file) */
uint32_t read_savegame_slot_names(uint32_t buf);                                    /* 0x21cc5 (Layer C; EAX=slot-name buffer; read each slot's name for the menu) */
uint32_t read_savegame_thumbnail(uint32_t slot);                                    /* 0x21b9f (Layer C; EAX=slot -> das-cache handle/0; load a slot's thumbnail) */
uint32_t prompt_save_overwrite(const regs_t *in);                                   /* 0x26349 (Layer C, INTERACTIVE; ABI_REGS_EAX -> EAX choice; wraps show_message_box, needs interactive-lift mode) */
uint32_t load_savegame_file(uint32_t slot);                                         /* 0x22129 (Layer C, LIVE-SWAP; EAX=slot -> 1 ok/0 fail; .SAV chunk-loop loader + warp setup) */
uint32_t write_savegame_file(uint32_t slot);                                        /* 0x21dc6 (Layer C, LIVE-SWAP + INTERACTIVE overwrite prompt; EAX=slot -> 1; .SAV chunk writer) */

/* player subsystem; lift_player.c. Physics + camera half of the player
 * update (movement-intent half already lifted). Bottom-up: L1 camera-math leaves (oracle) -> L2
 * vertical physics + view bob (oracle) -> L3 movement hub + flash (live-swap) -> L4 tick (live-swap). */
void update_view_transform_params(void);                                           /* 0x3e2ba (L1; pure camera-transform math -> camera record 0x89eec + pitch 0x89ee8/0x89ee6) */
void update_turn_view_scale(void);                                                 /* 0x3e22c (L1; commit discrete turn step; tail-call configure_render_viewport 0x408d1 bridge) */
void apply_view_camera_params(void);                                               /* 0x2a952 (L1; commit view/camera records 0x8526c/0x85270/0x85274 -> render globals; one-time selector alloc) */
uint32_t update_player_vertical_physics(uint32_t eax, uint32_t edx, uint32_t ebx); /* 0x1c648 (L2; EAX=floor,EDX=ceil_raw,EBX=pos -> new pos; gravity/jump/crouch/fall-damage state machine) */
void update_player_view_bob(uint32_t ebp);                                         /* 0x3ecfe (L2; EBP=&{floor,ceil} -> N vertical-physics steps + walk view-bob; commits 0x90a92/0x8c112) */
void clear_damage_flash(void);                                                     /* 0x179d2 (L3; clear pain accumulator 0x89f3b + refresh_palette_dac bridge) */
void update_player_movement(void);                                                 /* 0x1035a (L3; 5-call movement sequencer; 2 player [L] commits + collision/game_core/audio bridges) */
void update_player_tick(void);                                                     /* 0x1729c (L4; per-frame player hub; weapon fire/cooldown/cycle + viewmodel; LIVE-SWAP, state-machine oracle-checked) */

/* dbase100_interpreter subsystem; lift_dbase100.c.
 * Threaded-bytecode scripting brain. Bottom-up: A record-access leaves (oracle) -> D entity-def
 * builder (oracle) -> C dialogue script/queue (mix) -> B interpreter core (live-swap). */
int32_t filter_dbase100_active_records(uint32_t cursor_p, uint32_t count,
                                              uint32_t out_p, uint32_t result_p);          /* 0x1d146 (A; pre-walk chain -> active-record segments; EAX=&cursor,EDX=count,EBX=out,ECX=&n -> EAX=final counter) */
uint32_t build_entity_def_record(uint32_t dest, uint32_t record);                   /* 0x1e128 (D; EAX=dest 0x6c def,EDX=record -> scatter AsMonster(0x0a) sub-opcodes into def; ret 1 found/0 none) */
uint32_t build_entity_def_by_id(uint32_t dest, uint32_t id);                        /* 0x1e2bd (D; EAX=dest,EDX=id -> resolve id->record (0x81e1c/0x81e20) then build_entity_def_record) */
void close_dialogue_script(void);                                                   /* 0x1e8ae (C leaf; close dbase400 handle 0x824c5; bridges dos_close_handle) */
void free_dbase100_data(void);                                                      /* 0x1e0a9 (C; free base 0x81e1c + bitmap 0x81e28 via game_heap_free; tail-calls close_dialogue_script) */
void finish_dialogue_record_eval(void);                                             /* 0x1db5e (C leaf; clear queue-active 0x81e18; gated overlay teardown bridge) */
uint32_t finalize_dbase100_chain(uint32_t arg);                                     /* 0x1d25e (B; EAX=arg passthrough; commit pending topic 0x81eb2 -> load_dbase300_chunk + gated audio bridges) */
void open_dialogue_script(void);                                                    /* 0x1e874 (C leaf; open dbase400.dat once -> handle 0x824c5; bridges build_game_path + dos_open_file) */
/* dbase100 live-swap cluster (LIVE-SWAP ONLY: in-game ROTH_LIFT=dbase100). The 1722B
 * interpreter execute_dbase100_chain 0x1d430 is BRIDGED (call_orig) by these; lifting it later
 * switches these to the C interpreter. */
uint32_t read_next_dialogue_line(uint32_t dest, uint32_t maxlen, uint32_t voice_off, uint32_t flag); /* 0x1e8cc (reads dbase400.dat; bridges DOS/audio/dialogue_ui) */
uint32_t resolve_dbase100_text(uint32_t dest, uint32_t maxlen, uint32_t index, uint32_t flag);       /* 0x1f818 (base text table[index] -> read_next_dialogue_line) */
uint32_t eval_dialogue_record_by_id(uint32_t id);                                   /* 0x1dc73 (id -> dialogue record (0x81e24) -> eval_or_queue) */
uint32_t eval_or_queue_dialogue_record_commands(uint32_t record, uint32_t flag);    /* 0x1daea (EAX=record,EDX=flag; queue or run the chain) */
void run_dialogue_action_queue(void);                                               /* 0x1d2aa (drain the deferred dialogue-action queue) */
void advance_dialogue_action_queue(void);                                           /* 0x1db98 (run+pop one queue entry) */
uint32_t execute_dialogue_branch(uint32_t index);                                   /* 0x1dc02 (EAX=branch index; run the selected opcode-9 branch of the choice record) */
void close_dialogue_and_run_branch(void);                                           /* 0x1fbe4 (clear overlay flags -> execute_dialogue_branch[0x8313d]) */
uint32_t eval_dialogue_record_condition_with_cleanup(uint32_t eax, uint32_t edx, uint32_t *ebx_io, uint32_t *ecx_io);   /* 0x1db89 (eval_or_queue then finish; EAX=result. EBX/ECX PRESERVED (input==output) -> ebx_io/ecx_io pass-through out-params, NULL when unused) */
uint32_t execute_dbase100_chain(uint32_t chain, uint32_t count, uint32_t flags);    /* 0x1d430 (B; THE threaded-bytecode interpreter; EAX=chain cursor,EDX=count,EBX=flags -> EAX=acc/-1/1) */
int32_t eval_or_queue_reg(const regs_t *in);                                        /* ABI_REGS_EAX wrapper for 0x1daea (EAX=record,EDX=flag) */
int32_t eval_dialogue_record_condition_reg(const regs_t *in);                       /* ABI_REGS_EAX wrapper for 0x1db89 (EAX=record,EDX=flag) */

/* gdv_cutscene subsystem; lift_gdv_cutscene.c.
 * Gremlin Digital Video FMV player. Bottom-up: A codec leaves (oracle) -> codec core -> B geometry
 * (blitter = call_orig bridge) -> C video/palette -> D container IO+loop+entries -> E audio glue
 * -> F menu/UI. STORED-POINTER hazard (A4) dominates: d6c/d68/d50/d5c/d40 hold raw host addrs. */
void gdv_advance_chunk_ptr_inner(void);                                             /* 0x4dd38 (A leaf; chunk-ring cursor advance; PURE; EAX=new [d6c], clears CF) */
void gdv_init_pixel_tables(void);                                                   /* 0x4bef0 (C leaf; build colour LUT @d3c + zero decode buf @d40; PURE; A4 stored ptrs) */
void gdv_reformat_pixel_buffer(void);                                               /* 0x4d541 (A leaf; in-place half-res expand/halve of decode buf @d40; PURE; A4; pushal/popal void) */
void gdv_init_frame_geometry(void);                                                 /* 0x4bf4c (B leaf; centering/clip/scale + 8 source-offset ptrs d74..d90; PURE void(void); A4) */
void gdv_decode_video_chunk(void);                                                  /* 0x4d384 (A core; LZ+RLE bitstream codec; ALL fmts 0/1/2/3/5/6/8 lifted; CLOSED — seq-oracle + synth + in-game) */
uint32_t find_gdv_error_index(const uint8_t *word);                                 /* 0x26a20 (D; EAX=word -> category row 0..3 via 4 ci word-lists [83ea8]/[83eac], or 0x32; PURE leaf, calls lifted match_word_in_list_ci) */
uint32_t gdv_setup_decode_buffers(uint32_t *ebp_out);                                /* 0x4ba30 (C buffer layout; void -> CF [+EBP err 0x12 on fail in *ebp_out]; audio scratch/scale-tbl/work base/chunk-ring + frame count; reads d34/d2c/dc8/dca/cc4/d94/d9c/d44/d0c/da4) */
/* palette/DAC cluster (sub-C) — IN-GAME LIVE-SWAP ONLY (DAC `out` via g_os_port_out; fade fns spin on
 * the timer accumulator [0x91dbc], pumped by the host interactive surrogate). See lift_gdv_cutscene.c. */
void gdv_clear_vga_palette(void);                                                    /* 0x4c392 (DEAD; black DAC entries 0..[dec]; 8-bpp; no spin) */
void gdv_settle_palette_fade(void);                                                  /* 0x4d2e0 (DEAD; countdown [de4] -> blank (clear) or restore (bridged write_vga_palette 0x4c334); no spin) */
void gdv_write_scaled_palette(uint32_t ebx);                                         /* 0x4e11a (LIVE; EBX=fade level -> 256 DAC entries scaled; spins on [dbc]; COLD entry — only fade_in/out call it) */
void gdv_fade_in_palette(void);                                                      /* 0x4e08e (LIVE; ramp fade 0->0x40 via write_scaled; INTERACTIVE) */
void gdv_fade_out_palette(void);                                                     /* 0x4e0c2 (LIVE; settle spin + ramp 0x40->0 via write_scaled; INTERACTIVE) */
void init_gdv_video_context(uint32_t eax);                                           /* 0x4ec70 (C leaf; EAX=ctx ptr -> seed display-geometry fields [eax+0x19..0x74]; PURE, oracle) */
void swap_cutscene_display_buffers(uint32_t eax);                                    /* 0x4ed38 (C leaf; EAX=ctx ptr -> swap 2 double-buffer page pairs [71f04/06/08/0a]; PURE, oracle) */
/* container lifecycle CLOSE path (sub-D) — IN-GAME LIVE-SWAP ONLY (DOS file close + audio/timer teardown). */
void gdv_free_decode_buffer(void);                                                   /* 0x4b9a6 (COLD; free decode buf [d34] via game_heap_free; guard [d22]&1) */
void gdv_close_input(void);                                                          /* 0x4beb3 (COLD; bridge uninstall_timer_sync + audio_shutdown) */
void gdv_decoder_close(void);                                                        /* 0x4b95e (LIVE; teardown: close_input + free_decode_buffer + int21 0x3e close via g_os_soft_int) */
/* container OPEN/parse (sub-D) — IN-GAME LIVE-SWAP ONLY (4 inline int 0x21 via g_os_soft_int). */
uint32_t gdv_read_file_header(uint32_t *ebp_out);                                    /* 0x4bbb1 (LIVE; open/lseek + parse header [d44] + palette + dims table 0x72770 + audio/buffer sizes; void -> CF [+EBP err in *ebp_out]) */
uint32_t gdv_read_frame_chunk(uint32_t *ebp_out, uint32_t *ecx_out);                 /* 0x4c3ba (LIVE; fetch next frame chunk into ring [d60] (buffered or int21 read+retry) + advance ring + audio-pacing spin; void -> CF [+EBP err, +ECX bytes]; INTERACTIVE) */
uint32_t gdv_decoder_open(uint32_t eax);                                             /* 0x4b710 (LIVE; cutscene OPEN orchestrator: state reset + ctx-> globals + alloc/read_header/setup/init_audio/init_pixel/preload; EAX=ctx -> EAX result 0=ok) */
/* present/loop KEYSTONE (sub-E) — IN-GAME LIVE-SWAP ONLY. Publishes via g_os_publish_frame (replaces the
 * host's GDV_EMIT_SITE entry-int3 publish), then bridges the host_video_driver blitter + (popup) callback. */
void gdv_emit_decoded_frame(void);                                                   /* 0x4dcfc (LIVE; publish + blit 0x4c788 bridge [engine side-effects] or callback 0x4dd71 [popup] + dec [0x91cb8]) */
uint32_t gdv_invoke_user_callback(uint32_t action);                                  /* 0x4dd71 (LIVE; ABI_EAX; sets ebx/ecx/edx, bridges the indirect user callback [0x91d00], latches EOS on al&2) */
int      gdv_callback_frame_boundary(void);                                          /* 0x4e041 (LIVE; ABI_ZF; per-frame loop gate -> ZF=EOS; calls invoke_user_callback(2) as direct C) */
void gdv_advance_chunk_ptr(void);                                                    /* 0x4dd33 (LIVE; emit() + advance_chunk_ptr_inner() — 5B call+fall-through) */
void gdv_decode_subframe(void);                                                      /* 0x4e00d (LIVE; decode_video_chunk + [db4<6] emit + dec db4/dba + advance_inner; db4/dba 16-bit) */
void gdv_drain_pending_subframes(void);                                              /* 0x4dff4 (LIVE; audio catch-up loop: decode_subframe while [db4]>=2; bounded) */
void gdv_prime_first_frame(void);                                                    /* 0x4de7a (LIVE; seed ring ptrs + decode_video_chunk + advance + reset db4; both callees lifted -> C) */
void gdv_present_first_frame(void);                                                  /* 0x4c75c (LIVE; one-shot frame-0 present: popup callback(4) or bridge main blit 0x4c7a5) */
uint32_t gdv_begin_playback(void);                                                   /* 0x4e17f (LIVE; bridge setup_video_mode [host_video_driver, CF] + init_frame_geometry + present_first_frame; ABI_CF) */
#ifdef ROTH_STANDALONE
/* GDV video mode-set cluster — IMAGE-FREE native bodies. All
 * host_video_driver; guarded so the trap-lane object stays byte-identical (its call_orig bridge is kept). */
uint32_t gdv_setup_video_mode(void);                                                 /* 0x4e67e (host_video_driver; mode picker: INT10h->soft_int + write-set; ABI_CF, EBP dropped) */
uint16_t gdv_probe_vesa_bank_granularity(void);                                      /* 0x4eba9 (host_video_driver; VESA bank probe via 0xa0000 markers + 4F05; returns DX) */
void     gdv_init_modex_unchained(void);                                             /* 0x4ea34 (host_video_driver; mode 13h + VGA seq/CRTC unchained setup via port I/O) */
void     gdv_clear_display_surface(void);                                            /* 0x4c59d (host_video_driver; surface clear -> tail-jmp settle_palette_fade 0x4d2e0) */
void     gdv_blit_frame_to_vga(void);                                                /* 0x4c7a5 (host_video_driver; side-effects only [de4/de5 + palette copy]; pixel blit skipped) */
void     gdv_blit_frame_to_vga_alt(void);                                            /* 0x4c788 (host_video_driver; format-dispatch alt entry into 0x4c7a5 side-effects) */
uint32_t gdv_inspect_frame_callback(uint32_t action, uint32_t ebx);                  /* 0x18e09 (mid-entry into 0x18cb9; the inspect popup's [0x91d00] callback — dispatched from BOTH gdv_invoke_user_callback AND write_vga_palette) */
#endif
/* audio-open glue (sub-D) — COLD entries (reached only via lifted decoder_open->init->configure as C). */
uint32_t gdv_configure_audio_device(void);                                           /* 0x4e192 (LIVE; copy ctx audio descriptor + SOS detect/load drivers [bridged]; -> CF; ABI_CF) */
uint32_t gdv_init_audio_output(uint32_t *ebp_out);                                   /* 0x4bddf (LIVE; PIT timer-sync [bridge] OR configure + SOS setup_voices [bridge]; -> CF + EBP err; ABI_CF_EBP) */
uint32_t gdv_alloc_decode_buffer(uint32_t *ebp_out);                                 /* 0x4b9d4 (LIVE/COLD; alloc+zero decode buf via game_heap_alloc [bridge]; ctx<-[0x91d14]; -> CF + EBP err 0x10/0x11; ABI_CF_EBP) */
/* cutscene container/control (sub-F) — IN-GAME LIVE-SWAP ONLY (write live back buffer + dirty rects; bridge video_display/das/blit). */
void clear_cutscene_region(void);                                                    /* 0x1fc5c (LIVE; black-fill letterbox rect [83b3c/40/44/48] via lifted mem_fill + add_dirty_rect [bridge]; clear flag [83b44]) */
void present_cutscene_frame(void);                                                   /* 0x20a8a (LIVE; splash present: remap+scaled-blit image handle [83c74] OR full dirty -> flush+flip(0x103)+dac; 8 cross-subsys bridges) */
void exit_cutscene_overlay_mode(void);                                               /* 0x20905 (LIVE/INTERACTIVE; overlay teardown: clear+present+slide-out [play_screen_slide_out, frame-tick paced]+resume music+free handles; ~10 bridges) */
uint32_t show_cutscene_error_box(uint32_t param_1);                                  /* 0x26aef (LIVE/INTERACTIVE; categorize GDV load error by basename + show_message_box; EAX=path -> EAX 0/1; ABI_EAX) */
uint32_t show_cutscene_playback_menu(uint32_t param_1);                              /* 0x26356 (LIVE/INTERACTIVE; the FMV cutscene gallery — list seen cutscenes + show_message_box list box; EAX=flags -> EAX 0; ABI_EAX) */
uint32_t gdv_decode_frame(uint32_t ignored);                                         /* 0x4b8c1 (LIVE/INTERACTIVE; whole-cutscene play: rebase + begin/prime/fades [C] + run_playback_loop/present_streamed_frame [lifted C]; -> EAX 0/1/0x100; ABI_EAX) */
uint32_t gdv_run_playback_loop(void);                                                /* 0x4deb5 (LIVE/INTERACTIVE; THE per-pass frame loop; PRODUCER spinning on decoder head [0x91d68]; -> CF; ABI_CF; surrogate pumps the decode) */
uint32_t gdv_present_streamed_frame(void);                                           /* 0x4c2d1 (LIVE/INTERACTIVE; streamed-mode self-contained read->codec->emit loop; -> CF; ABI_CF) */
uint32_t gdv_preload_frame_window(uint32_t *ebp_out);                                /* 0x4dddd (LIVE; PURE read-ahead ring fill via read_frame_chunk [no pump/spin -> no hang]; -> CF+EBP; ABI_CF_EBP; called by decoder_open) */
uint32_t play_gdv_cutscene(uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx);  /* 0x2059d (LIVE/INTERACTIVE; top-level GDV player: build descriptor + open[retry]/play[decode_frame]/close + scale&cache still frame; EAX=name EBX=record; ABI_EAX4) */
uint32_t play_record_gdv_cutscene(uint32_t eax);                                     /* 0x20c16 (LIVE/INTERACTIVE; record-driven player: gallery reg + caption/voice load + fb backup/slide + path build -> play_gdv_cutscene; EAX=record -> EAX 0/1; ABI_EAX) */
/* GDV decode-pump timer-ISR bodies (carved engine; frameless — int-frame = host_timer_driver).
 * NO call-site: the host timer driver (traps.c shm_tick surrogate) calls these directly. */
void gdv_advance_decode_pump(void);                                                  /* 0x4e310 (decode-pacing core; [db0]/[dbe] budget -> codec+advance; called by 0x4e60b when [d0c]&2) */
void gdv_decode_timer_isr(void);                                                     /* 0x4e60b (int-8 audio timer ISR body: pump if [d0c]&2 + [dbc]+=0xa00 + [884]+=[880]) */
void gdv_decode_timer_isr_noaudio(void);                                             /* 0x4e24b (no-audio SOS timer ISR body: [dbc]+=0xbaa + [db0]/[dbe] decode gated on [dba] + [884]+=[880]) */
void gdv_tick_timer_isr(void);                                                       /* 0x4e2ed (no-decode tick ISR body: [dbc]+=0x1400 + [884]+=[880]; the fade/read pump) */

/* ---- weapon_combat subsystem; lift_weapon_combat.c.
 * Player weapon equip/arm/fire pipeline, projectiles (player+enemy), damage-to-player, enemy-attack
 * trigger, and the weapon HUD + first-person viewmodel renderer. Bottom-up: C damage math (pure,
 * oracle) -> B projectiles -> D enemy attack -> A equip+fire -> E HUD/viewmodel. 4 fns already lifted
 * (arm_weapon_fire, compute_projectile_hit_damage, apply_damage_to_player, setup_player_viewmodel_sprite). */
/* C — damage to player (pure HP math; oracle). */
void apply_direct_damage_to_player(uint32_t eax);     /* 0x32058 (shared flow_succ tail: pain-flash + HP sub/clamp) */
void apply_reduced_damage_to_player(uint32_t eax);    /* 0x320c6 (scale damage by [0x81e30]/2048 then -> direct) */
void damage_player_from_emitter(void);                /* 0x34579 (walk emitter list; point_to_wall_distance_sq bridge -> apply_damage_to_player) */
/* B — projectiles (spawn/resolve/hit; entity-pool + audio bridged). */
int  check_projectile_sector_clearance(uint32_t eax, uint32_t edi);  /* 0x42c3c (CF; sibling of 0x42c04) */
void resolve_projectile_target_entity(uint32_t eax, uint32_t d2);    /* 0x4271c (revalidate_entity_def direct-C; play_distance_variant_sound direct-C, d2=inherited ECX=caller dmg) */
int  init_projectile_from_item(uint32_t esi);                        /* 0x42b8c (CF=0; init_inventory_item_object + play_entity_sound bridges) */
/* D — enemy attack (flow_succ pair; shared tail 0x43500; revalidate_entity_def + play_entity_sound bridges). */
void begin_enemy_attack(uint32_t edi);                               /* 0x434d8 */
void launch_enemy_attack_animation(uint32_t edi);                    /* 0x4347e (advances LCG 0x72730) */
/* A — weapon equip + fire trigger. */
void     reset_weapon_fire_timing(void);                            /* 0x1765c (unconditional arm; alt-entry of 0x17629) */
void     arm_weapon_and_cache_def(uint32_t eax);                    /* 0x17668 (arm + cache def fields) */
uint32_t apply_weapon_action_attributes(uint32_t eax, uint32_t edx, uint32_t ebx); /* 0x18260 (PURE WeaponAction 0x05 parser) */
void     activate_weapon_item(uint32_t eax, uint32_t edx);          /* 0x184ab (equip: parse attrs + ammo + arm) */
void     show_no_ammo_message(uint32_t eax);                        /* 0x1f8cb (resolve text + timed message) */
void     rebuild_weapon_inventory_list(void);                       /* 0x2245c (scan inventory -> weapon list 0x83d84) */
/* E — weapon HUD + viewmodel renderer (leaves; big renderers are in-game). */
void     free_hud_weapon_das_handle(void);                          /* 0x2268e (pool_free_handle bridge) */
void     redraw_weapon_hud_panel(void);                             /* 0x23869 (render_ui_texture_panel bridge) */
void     key_toggle_weapon_overlay(void);                           /* 0x175d8 (DEAD; weapon raise/lower state toggle) */
void     equip_first_usable_weapon(void);                           /* 0x1bd8c (scan inventory + equip first usable weapon) */
/* B/A — in-game tier (entity-pool spawn + fire pipeline; verify via ROTH_LIFT=weapon_combat). */
uint32_t spawn_projectile_from_aim(uint32_t eax, uint32_t edx, uint32_t ebx);  /* 0x42400 */
uint32_t spawn_object_projectile_at_player(uint32_t eax, uint32_t edx);        /* 0x42300 */
uint32_t spawn_player_projectile_flagged(uint32_t eax, uint32_t edx);          /* 0x422ec */
uint32_t fire_pending_weapon_shot(uint32_t param_1);                           /* 0x16da2 EAX=record ptr */
uint32_t trigger_weapon_fire(uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx);  /* 0x1768a (674B fire keystone) */
void     key_fire_weapon(void);                                                /* 0x14cb6 (input keybind -> trigger) */
void     reset_weapon_hud(void);                                               /* 0x1be8e (activate(0,0) + render_weapon_hud; bridges) */
void     tick_weapon_hud_ammo_anim(void);                                      /* 0x2250e (HUD ammo-counter interpolation; oracle) */
void     render_weapon_hud(uint32_t eax, uint32_t edx);                        /* 0x24165 (HUD panel compositor; in-game fb) */
void     draw_player_viewmodel_sprite(uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx); /* 0x139a0 (viewmodel sprite blit; in-game fb) */
uint32_t render_weapon_view(uint32_t eax);                                     /* 0x22e7b (1905B viewmodel/ammo compositor; in-game fb) */

/* ---- inventory subsystem; lift_inventory.c.
 * Carried-item storage + the 5-tab grid UI + pick-up/use/combine/stack + icon rendering + equip.
 * Bottom-up: A data model (give/remove/query) -> B cursor-held (use/combine/swap) -> C rendering
 * (panel/grid/icons) -> D screen lifecycle. flow_succ chains + A1 multi-reg returns; dbase100/
 * weapon_combat/das_assets bridges. Lift lens: docs/reference/lift/inventory.md. */
/* A — item data model. */
void     reset_item_pickup_lock(void);                          /* 0x18003 (clears lock + clear_list_field30) */
uint32_t is_item_id_pickable(uint32_t eax);                     /* 0x1dd50 (pure template query -> EAX) */
uint32_t resolve_record_conditional_op2(uint32_t eax);          /* 0x1a028 (bridges dbase100 0x1db89 -> EAX) */
void     tick_item_pickup_lock(void);                           /* 0x15efe (per-frame pickup-anim; pure obj3) */
uint32_t init_inventory_item_object(uint32_t eax, uint32_t edx);/* 0x18598 (EAX=id,EDX=obj; mem_fill bridge -> EAX) */
/* A mid — give/remove/query (within-subsystem direct calls; weapon_combat + icon bridges). */
void     remove_inventory_item(uint32_t eax, uint32_t edx);     /* 0x1ce6b (EAX=slot,EDX=template; void) */
uint32_t remove_item(uint32_t eax);                            /* 0x1d077 (EAX=id -> -1/0) */
uint32_t consume_held_item(uint32_t eax);                      /* 0x1d0fd (EAX=slot -> 1/0) */
uint32_t give_item_by_dbase_id(uint32_t eax, uint32_t edx);   /* 0x1dcef (EAX=dbase id,EDX=ctx -> EAX) */
uint32_t give_item(uint32_t eax, uint32_t edx);               /* 0x1cedc (EAX=entry idx,EDX=ctx -> EAX) */
uint32_t query_player_inventory(uint32_t eax, uint32_t edx);  /* 0x1ccf7 (EAX=id,EDX=flags -> EAX count; GAP#1: EAX-only) */
/* B — cursor-held interaction. */
uint32_t resolve_item_use_action(uint32_t eax, uint32_t edx, uint32_t ebx);  /* 0x18060 (rec,target,&out -> packed EAX; GAP#2) */
uint32_t get_item_tab_index(uint32_t eax);                    /* 0x1b0b2 (EAX=index -> tab 0..4/0) */
void     free_active_item_hud_icon(void);                     /* 0x1823a (free [0x7fed0] icon handle) */
void     commit_held_cursor_item(void);                       /* 0x1a88b (equip/select held cursor item) */
void     restore_active_held_item(void);                      /* 0x1818d (re-establish held item; DAS/DOS; in-game) */
void     swap_inventory_entries(uint32_t eax, uint32_t edx);  /* 0x1b007 (swap 2 cursor entries; GAP#6 shared epilogue) */
void     use_item_on_self(void);                              /* 0x1b141 (use held item; dbase100 effects; in-game) */
void     combine_held_item_with_target(uint32_t eax);         /* 0x1b26d (combine held+target; in-game) */
/* C — rendering (oracle-able pieces; framebuffer compositors below are in-game). */
void     encode_item_icon_to_spans(uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx); /* 0x13e35 */
void     blit_item_icon(uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx); /* 0x13544 (icon blit primitive) */
void     draw_item_icon_in_slot(uint32_t eax, uint32_t edx, uint32_t ebx); /* 0x19f34 (icon in grid slot) */
void     draw_item_icon_centered(uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx); /* 0x19fcf */
void     draw_equipped_item_left(void);                       /* 0x1a2ef (equipped item icon) */
void     draw_panel_slot_tile(uint32_t eax);                  /* 0x19ee6 (slot tile bg; das; in-game) */
void     draw_equipped_item_right(void);                      /* 0x1bfaa (right equipped icon) */
void     set_inventory_list_filter(uint32_t eax);             /* 0x1ca2e (per-slot filter flags) */
/* C/D in-game framebuffer + lifecycle renderers (ROTH_LIFT_DIFF tier; callees bridged). */
void     update_selected_item_icon(void);                     /* 0x1bb4b */
void     draw_held_item_icon(void);                           /* 0x1bcc4 */
void     draw_inventory_tabs(void);                           /* 0x1a2d2 */
void     select_inventory_tab(void);                          /* 0x1a2b5 (-> draw_inventory_tabs) */
void     draw_inventory_entry_label(uint32_t eax);            /* 0x1c020 */
void     refresh_inventory_grid(void);                        /* 0x1c469 */
void     redraw_inventory_cursor_cell(void);                  /* 0x1a178 */
void     refresh_inventory_panel(void);                       /* 0x1bf7b (-> draw_equipped_item_right) */
void     close_inventory_panel(void);                         /* 0x1a7a1 */
void     build_inventory_entry_list(uint32_t eax);            /* 0x19d30 (tab filter + icon load) */
uint32_t find_or_autoselect_inventory_item(uint32_t eax, uint32_t edx, uint32_t ebx); /* 0x1cb6c (RNG) */
void     render_inventory_grid(void);                         /* 0x1c163 (10-cell grid + scroll arrows) */
void     render_inventory_panel(void);                        /* 0x1a399 (the panel compositor; opens) */
void     update_inventory_screen(void);                       /* 0x1a8e5 (per-frame input state machine) */
uint32_t load_item_icon_resource(uint32_t eax, uint32_t edx); /* 0x1816a (resolve + load_das; EDX=handle passthrough) */
void     free_cursor_entry_icons(void);                       /* 0x19ca7 (free cursor icon handles + dedup) */
uint32_t format_inventory_item_label(uint32_t eax, uint32_t edx); /* 0x1c0b1 (label text format) */

/* C — game_core subsystem (lift_game_core.c): the integration spine (entry/startup/lifecycle/main loop).
 * Almost all are ABI_VOID orchestrators; reset_and_start_new_game returns 0/-1 -> ABI_REGS_EAX. All but
 * update_frame_time_scale (oracle-verified) are in-game live-swap only (they ARE the game loop). */
void     update_frame_time_scale(void);                       /* 0x24f5e (Layer 1; ORACLE) */
void     tick_ambient_render_and_map(void);                   /* 0x10382 (Layer 1; in-game) */
void     init_game_databases(void);                           /* 0x1dfc2 (Layer 1; in-game — DOS file layer) */
int32_t  reset_and_start_new_game(const regs_t *in);          /* 0x1107e (Layer 2; ABI_REGS_EAX, 0/-1) */
void     gameplay_frame_step(void);                           /* 0x1792c (Layer 3; in-game) */
void     run_gameplay_frame(void);                            /* 0x1691c (Layer 3; in-game) */
void     game_play_loop(void);                                /* 0x179ee (Layer 4; in-game) */
void     roth_main_sequence(void);                            /* 0x100f6 (Layer 5; in-game) */
void     roth_game_startup(void);                             /* 0x10010 (Layer 5; in-game) */
void     roth_main(void);                                          /* 0x15110 (Layer 6; in-game — program entry) */

/* ---- audio subsystem (lift_audio.c) — the game-side SOS client (SFX / voice / MIDI / glue).
 * lift-lens: docs/reference/lift/audio.md. ABIs from the disasm. ---- */
/* A. SFX — Layer 1 leaves (oracle: test_audio.c) */
uint32_t compute_sound_pan_from_position(uint32_t pos, uint32_t dist);       /* 0x43cce (EAX=pos,EDX=dist -> EAX=pan) */
int32_t  compute_sound_volume_pan(uint32_t rec);                             /* 0x26f48 (EAX=rec -> EAX=vol; writes rec+0xc; shared flow_succ tail) */
uint32_t find_free_voice_descriptor(void);                                   /* 0x2799c (-> EAX, AL=free idx | 0xff) */
uint32_t release_voice_descriptor(uint32_t eax_in);                          /* 0x279e4 (AL=idx -> EAX=0/1/2) */
uint32_t collect_sfx_nodes_by_key(uint32_t key, uint32_t out, uint32_t cap); /* 0x43ab4 (EAX,EDX,EBX -> EAX=written) */
void     sort_sfx_query_by_distance(uint32_t q);                             /* 0x43c89 (EAX=q) */
uint32_t load_sfx_node_active_state(uint32_t src);                           /* 0x43d98 (EAX=src -> EAX; save_ shared tail) */
/* D. SOS voice wrappers (lifted early; each = reg marshal + ONE host_audio_driver bridge) */
uint32_t sos_submit_voice(uint32_t voice, uint32_t cb_off, uint32_t cb_sel); /* 0x15a4a (-> sos_voice_start 0x4a641) */
uint32_t sos_stop_voice(uint32_t voice);                                     /* 0x15a65 (-> 0x4ac55) */
uint32_t sos_voice_set_callback(uint32_t eax, uint32_t edx, uint32_t ebx);   /* 0x15a92 (-> 0x4ad03; EBX passthrough, ECX=DS) */
uint32_t sos_voice_set_w32(uint32_t voice, uint32_t val);                    /* 0x15a9c (volume -> 0x49fe9) */
uint32_t sos_voice_set_w54(uint32_t voice, uint32_t val);                    /* 0x15ab2 (pan -> 0x4a28c) */
uint32_t sos_voice_get_w34_wrapper(uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx); /* 0x15a79 (jmp 0x4a54a) */
/* D. audio stream buffers (call-closed callees of the bank loader) */
void     alloc_audio_stream_buffers(void);                                   /* 0x30051 */
void     free_audio_stream_buffers(void);                                    /* 0x30162 */
/* A. SFX mids */
uint32_t resolve_sound_sample(uint32_t id);                                  /* 0x27951 (AL=slot|0xff) */
uint32_t load_sound_sample(uint32_t id);                                     /* 0x277db (AL=slot|0xff; DOS bridges) */
uint32_t evict_oldest_voice_descriptor(void);                                /* 0x27a3e (AL=slot|0xff) */
uint32_t stop_sounds_for_sample_slot(uint32_t al_in);                        /* 0x26eaf (AL=slot key) */
uint32_t stop_sound_handle_voice(uint32_t emitter);                          /* 0x26d3e (-> 1/0) */
uint32_t stop_sound_by_id(uint32_t id);                                      /* 0x26d8a (-> 1/0) */
uint32_t query_sfx_emitters_in_range(uint32_t q, uint32_t posx, uint32_t posy); /* 0x43b3b (-> count) */
uint32_t start_sound_voice_vol(uint32_t rec, uint32_t vol, uint32_t userdata, uint32_t ebx_in); /* 0x275cc */
uint32_t start_sound_voice(uint32_t rec, uint32_t userdata);                 /* 0x276a2 */
/* A. SFX play-entry tower (flow_succ bodies shared as statics in lift_audio.c) */
uint32_t play_sound_effect(uint32_t id, uint32_t param);                     /* 0x27270 */
uint32_t start_persistent_looping_sound(uint32_t id, uint32_t param);        /* 0x271fb */
uint32_t play_object_sound(uint32_t id_in, uint32_t param, uint32_t ebx_x, uint32_t ecx_y,
                                  uint32_t emitter);                                /* 0x270ca (stack arg; ret 4) */
uint32_t play_object_sound_thunk(uint32_t id, uint32_t param, uint32_t ebx_x, uint32_t ecx_y,
                                        uint32_t emitter);                          /* 0x271e2 */
uint32_t play_entity_sound(uint32_t id, uint32_t param, uint32_t bx, uint32_t cx); /* 0x271c4 */
uint32_t play_entity_object_sound(uint32_t id, uint32_t param, uint32_t bx, uint32_t cx); /* 0x271e8 */
uint32_t play_command_sound(uint32_t id, uint32_t param, uint32_t bx, uint32_t cx); /* 0x2730b */
uint32_t play_sound_unique(uint32_t entry, uint32_t param);                  /* 0x273f0 */
uint32_t play_world_sound_at_pos(uint32_t pos, uint32_t param);              /* 0x27207 */
uint32_t play_world_sound_squared_dist(uint32_t pos, uint32_t param);        /* 0x2721c */
void     play_sound_sequence_group(uint32_t q);                              /* 0x27080 */
void     play_nearby_sfx_emitters(void);                                     /* 0x151c9 */
void     restart_door_open_sound(uint32_t door);                             /* 0x3d8f2 */
void     play_distance_variant_sound(uint32_t coords, uint32_t tab, uint32_t d2); /* 0x4269b (RNG-read D1) */
void     play_distance_variant_sound_regs(uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx); /* registry shim */
void     update_active_sounds(void);                                         /* 0x27b05 (per-frame) */
/* A. SFX bank load/reload */
uint32_t load_sound_effect_bank(uint32_t name);                              /* 0x26b66 (DOS bridges) */
uint32_t reload_sfx_bank_if_pending(uint32_t name);                          /* 0x15805 */
uint32_t load_sfx_file_wrapper(uint32_t name);                               /* 0x10c51 */
/* B. voice / speech streaming (dbase500 clip streamer) */
uint32_t decode_voice_block(uint32_t dst, uint32_t remaining, uint32_t maxlen); /* 0x1e3df (-> raw bytes) */
void     close_voice_file(void);                                             /* 0x1e774 */
uint32_t install_voice_sos_callback(uint32_t voice);                         /* 0x27c0e */
void     voice_stream_sos_callback(uint32_t voice, uint32_t type, uint32_t arg3); /* 0x1e487 (FAR retf root) */
uint32_t prime_voice_clip(uint32_t clip);                                    /* 0x1e54d (-> 1/0; DOS+UI bridges) */
void     voice_stream_pump(void);                                            /* 0x1e9b5 (per-frame) */
void     dialogue_voice_force_end(uint32_t eax_in, uint32_t edx_in);         /* 0x1f671 */
void     dialogue_voice_stop_all(void);                                      /* 0x1f6cc */
uint32_t try_interrupt_dialogue_voice(void);                                 /* 0x18a2a (-> 1/0) */
/* C. MIDI music sequencer (driver tables via FAR gs helpers; timer/dispatch = host bridges) */
uint32_t decode_midi_varlen(uint32_t src_off, uint32_t src_sel, uint32_t out_off, uint32_t out_sel); /* 0x47150 */
uint32_t emit_audio_sequence_event(uint32_t seq, uint32_t event);            /* 0x4627d */
uint32_t clear_music_sequence_slot(uint32_t slot);                           /* 0x46cce */
uint32_t step_audio_sequence(uint32_t seq);                                  /* 0x46d18 (timer bridge) */
uint32_t parse_music_sequence_tracks(uint32_t seq, uint32_t pair, uint32_t pair_sel); /* 0x46eb3 */
uint32_t teardown_music_sequence(uint32_t seq);                              /* 0x46da7 */
uint32_t finalize_audio_sequence(uint32_t vol);                              /* 0x471da */
uint32_t init_audio_sequence(uint32_t pair, uint32_t pair_sel, uint32_t map_off, uint32_t map_sel,
                                    uint32_t out_off, uint32_t out_sel);            /* 0x464f9 (ret 8) */
uint32_t emit_music_sequence_event(uint32_t event);                          /* 0x1555f */
uint32_t set_music_master_volume(uint32_t vol);                              /* 0x1556f */
void     process_audio_sequence_chunk(void);                                 /* 0x1558d (per-frame) */
void     stop_music_sequence(void);                                          /* 0x15630 */
uint32_t resume_music_sequence(void);                                        /* 0x15671 */
uint32_t finalize_audio_sequence_ref(void);                                  /* 0x15689 */
uint32_t is_music_sequence_finished(void);                                   /* 0x15699 */
void     service_audio_sequence(void);                                       /* 0x156bd */
uint32_t register_music_timer_event(uint32_t cb_off, uint32_t cb_sel);       /* 0x159fa */
void     remove_music_timer_event(void);                                     /* 0x15a30 */
/* D. lifecycle / glue */
uint32_t apply_audio_volume_settings(void);                                  /* 0x2626f */
void     free_sfx_scratch_buffer(void);                                      /* 0x15ec4 */
void     resolve_dbase100_sound_ids(void);                                   /* 0x1def8 (oracle) */
void     sos_user_callback_trampoline(uint32_t voice, uint32_t type, uint32_t arg3); /* 0x27501 (FAR root) */
uint32_t install_sos_driver_vtables(uint32_t path_off, uint32_t path_sel, uint32_t ebx_in); /* 0x443a7 */
void     noop_ret_stub_1548c(void);                                          /* 0x1548c (epilogue tail; dead) */
uint32_t sos_load_driver(void);                                              /* 0x15290 (in-game; startup) */
void     sos_unload_driver(void);                                            /* 0x15702 */
void     sos_audio_shutdown(void);                                           /* 0x15ac8 */
uint32_t sos_audio_init(void);                                               /* 0x15813 (in-game; startup) */

/* ============================== das_assets (lift_das_assets.c) ==============================
 * DAS asset cache / sprite blit / animation.
 * Cluster A — cache core (CF returns as int: 1 = CF set): */
int      read_das_block_payload(uint32_t handle, uint32_t count, uint32_t block);       /* 0x41317 (EBX,ECX,EDX -> CF; int21 AH=3F) */
int      read_das_block_with_size_prefix(uint32_t handle, uint32_t count, uint32_t block); /* 0x41331 (shared stc tail w/ 0x41317) */
int      free_das_cache_entry(uint32_t status_ptr, uint32_t slot_entry);                /* 0x41413 (EDI,ESI -> CF=0; int31 free-selector) */
int      release_das_cache_slot_resources(uint8_t idx, uint32_t slot_entry);            /* 0x413fd (AL,ESI -> CF; flow_succ body above) */
uint32_t free_das_cache_handle(uint32_t handle);                                        /* 0x13136 (EAX -> 0) */
void     refresh_moved_das_cache_block(uint32_t block);                                 /* 0x41250 (ESI; 0x412ed selector refresh bridged) */
void     das_cache_make_room(uint32_t need);                                            /* 0x414b6 (EAX; flow_succ tail of ensure) */
void     ensure_das_cache_heap_space(uint32_t need);                                    /* 0x414d2 (EAX; the 17-caller make-room hub) */
int      postprocess_loaded_das_block(uint32_t blk);                                    /* 0x41051 (EDX=block -> CF; selector mint bridged) */
int      load_das_block_for_fat_index(uint32_t eax_in);                                 /* 0x40d7c (AX=FAT index -> CF; the loader spine) */
void     init_das_cache_heap(void);                                                     /* 0x30114 (startup; render-arena bridge) */
void     reset_das_entry_status_table(void);                                            /* 0x3001b */
/* Cluster B — file loaders: */
void     close_das_file_handle(void);                                                   /* 0x2fd6b (int21 3E via soft-int) */
uint32_t alloc_image_view_descriptor(uint32_t rec);                                     /* 0x40b5c (EAX=dims rec -> EAX=desc) */
uint32_t init_backdrop_image_surface(void);                                             /* 0x2fd21 */
void     release_das_and_geometry_buffers(void);                                        /* 0x30149 (render bridge) */
void     close_das_handles_and_buffers(void);                                           /* 0x2f163 */
uint32_t load_das_file_wrapper(uint32_t name_arg);                                      /* 0x10c32 (EAX -> EBX path arg; -> CF from load_map_das_file) */
void     load_ademo_das_wrapper(uint32_t name_arg);                                     /* 0x10c70 */
void     load_icons_all(void);                                                          /* 0x1602e (leftover-reg threading, gotcha H) */
uint32_t spawn_object_from_das_resource(uint32_t obj, uint32_t id, uint32_t variant);   /* 0x302e0 (EAX,DX,EBX -> -1/0; EDX-return bridge) */
int      read_das_palette(void);                                                        /* 0x2f379 (-> CF; low-mem palette buffer) */
int      allocate_das_worker_buffers(void);                                             /* 0x2fa29 (DPMI selector mint; startup; -> CF) */
void     load_ademo_das_file(uint32_t path);                                            /* 0x2effb (raw int21 open/reads) */
uint32_t load_das_cache_resource(uint32_t idx, uint32_t handle);                        /* 0x1869b (EAX,EDX -> handle/0; error-box retry) */
uint32_t decode_das_to_padded_buffer(uint32_t src_h, uint32_t *out_w, uint32_t *out_h); /* 0x1874d (frees the src handle) */
void     load_dbase200_sprite_cached(uint32_t idx);                                     /* 0x16087 (-> [0x7f574]; flow_succ tail = video epilogue) */
/* Cluster C — sprite blit + animation: */
void     blit_das_image_to_buffer(uint32_t img, uint32_t dst, uint32_t ebx_in, uint32_t ecx_in); /* 0x1325b (EAX,EDX,EBX=stride[packed],ECX=mode|shade) */
void     blit_das_image_auto_scale(uint32_t dst, uint32_t img, uint32_t ebx_in);        /* 0x18e48 (EAX=dst,EDX=img swap; mode from 0x90cbd) */
void     blit_das_image_at_xy(uint32_t img, int32_t x, int32_t y);                      /* 0x1a10a (EAX,EDX=x,EBX=y; flow_succ into 0x18e5c) */
void     blit_reloc_das_image(uint32_t dst, uint32_t off_slot, uint32_t ebx_in);        /* 0x18e68 (EAX=dst, EDX=offset slot in [0x7f56c]) */
void     rle_compress_byterun1(uint32_t esi, uint32_t edi);                             /* 0x3cf30 (one [0x85498]-byte row) */
void     advance_das_sprite_animation_frame(uint32_t blk);                              /* 0x38fec (ESI=animated block; render-driven) */
uint32_t decode_das_anim_frame(uint32_t ax_in, uint32_t rec);                           /* 0x2c5c5 (AX=frame, EDX=record) -> EAX = resolved frame ptr [[rec+0x10]] */
void     rescale_das_frame(uint32_t rec);                                               /* 0x1384d (EAX=frame record; in-place h-rescale) */
void     draw_das_panel_slide_reveal(uint32_t rec);                                     /* 0x187d1 (EAX=panel rec; dirty-rect/shade bridged) */
/* Cluster E — handle teardown: */
uint32_t free_das_image_handle(uint32_t handle);                                        /* 0x21cb1 (EAX; no null guard) */
void     free_hud_panel_das_handles(void);                                              /* 0x22633 (0x83d74/0x83d78; d78 zeroed unconditionally) */
uint32_t free_das_cache_handle_if(uint32_t handle);                                     /* 0x26266 (null guard over 0x13136) */
void     flush_object_das_handles(void);                                                /* 0x26cd4 (sample-table walk; call-closed) */
/* Cluster D — backdrop/parallax-sky codec: all take EBP = the shared 0x64-byte descriptor
 * (a raw runtime address `d`; field map atop lift_das_assets.c). */
void     backdrop_refill_buffer(uint32_t d);                                 /* 0x4b30b (int21 AH=3F via g_os_soft_int) */
void     backdrop_decode_rle(uint32_t d);                                    /* 0x4b27d */
uint32_t backdrop_ensure_source_bytes(uint32_t d, uint32_t ecx);             /* 0x4b269 (ECX=count -> EAX=src ptr) */
int      blit_backdrop_row(uint32_t d, uint32_t src, uint32_t *edi_io);      /* 0x4b1ee (EAX=src, EDI in/out -> ZF: 1=done) */
void     blit_backdrop_rows(uint32_t d);                                     /* 0x4b1b7 */
void     build_backdrop_hscale_table(uint32_t d);                            /* 0x4b322 */

/* ============ input (lift_input.c) — keyboard/mouse/keymap dispatch hub ============ */
/* Cluster A — keyboard ISR + scancode ring: */
int      keybit_mask_for(uint32_t eax);            /* 0x12976 (AL=scancode -> ZF; nonzero ret => ZF set = not held) */
uint32_t dequeue_translated_key(uint32_t *eax_io); /* 0x129ca (EAX in/out -> returns CF: 1 = translated key) */
void     dispatch_held_key_actions(void);          /* 0x128fb (held-bitmap walk -> table 0x707f1 handler dispatch) */
void     keyboard_int9_isr(uint8_t sc);            /* 0x12393 (INT 9 ISR BODY; sc = the port-0x60 byte; never int3-swapped) */
void     install_keyboard_int9(void);              /* 0x1246a (save INT 9 vector + point it at 0x12393; CRT vector-op bridges) */
void     restore_keyboard_int9(void);              /* 0x12498 (put the saved INT 9 vector back) */
/* Cluster B — keymap dispatch + per-key handlers (all void(void), keymap-table targets): */
void     keymap_dispatch(void);                    /* 0x14525 (ring drain -> keymap table 0x7093d -> handler dispatch [A4]) */
void     key_toggle_wireframe_map(void);           /* 0x1444c */
void     key_n_vestigial(void);                    /* 0x145a4 (DEAD) */
void     key_set_view_scale(void);                 /* 0x14573 (bridges configure_render_viewport) */
void     key_gamma_increase(void);                 /* 0x14601 (DEAD; flow_succ body — shared DAC tail) */
void     key_gamma_decrease(void);                 /* 0x145ec (floors at 0, falls into the increase tail) */
void     key_f7_noop(void);                        /* 0x17fd1 (DEAD; 1-byte ret) */
void     key_toggle_help_overlay(void);            /* 0x17fc0 (DEAD; flow_succ -> key_f7_noop) */
void     key_toggle_mouse_swap(void);              /* 0x17fd2 (bridges the show_status_message_wrap tail 0x17fe5) */
void     key_toggle_sprint(void);                  /* 0x17fed (same wrap tail; msg 2/3) */
void     key_toggle_subtitles(void);               /* 0x1f89b (falls into show_no_ammo_message [L] — direct C) */
void     key_toggle_easy_select(void);             /* 0x1801c (show_timed_message [L] — direct C) */
void     key_show_version(void);                   /* 0x14c9c (DEAD; version string 0x767c0) */
void     key_flush_texture_cache(void);            /* 0x144fb (DEAD; das reset+flush [L] — call-closed) */
void     key_quickload(void);                      /* 0x14cc9 (arm savegame request: mode 1) */
void     key_quicksave(void);                      /* 0x14ce5 (arm savegame request: mode 2) */
void     key_view_size_increase(void);             /* 0x14613 (menu/wireframe/normal modes; 0x17453+0x408d1 bridges) */
void     key_view_size_decrease(void);             /* 0x14678 */
void     key_cycle_display_type(void);             /* 0x14794 (video_display bridges) */
void     key_cycle_display_type_alt(void);         /* 0x147a8 */
void     check_snapshot_key(void);                 /* 0x11124 (bridges take_snapshot — INTERACTIVE live-swap) */
void     use_enter_key_handler(void);              /* 0x173f4 (dialogue accept / voice-end + map-menu commit) */
/* Cluster C — mouse (INT 33h via g_os_soft_int -> host mouse_int33). NOTE:
 * compute_screen_extents_7e8b0 0x115b5 (re-pinned input) was already lifted
 * in the early corpus-direct batch — declared above at its original spot; called direct-C. */
void     poll_mouse_motion(void);                  /* 0x117db (AX=0xB deltas -> hw accumulate / sw cursor math) */
void     blit_scaled_sprite_at_mouse(uint32_t desc);/* 0x115ea (EAX=cursor descriptor, raw ptr [A4]) */
uint32_t init_software_mouse(void);                /* 0x11594 (AX=0 detect -> configure @0x7079f; EAX 0/-1) */
void     poll_mouse_input(void);                   /* 0x11fae (AX=3 buttons -> held/edge flags; B-A swap) */
void     drain_input_and_clear_clicks(void);       /* 0x2057a (ring drain + one poll + clear edges; call-closed) */
void     begin_frame_then_init_mouse(void);        /* 0x103c8 (begin_screen_draw bridge + init chain) */
/* Cluster D — cursor world use/examine (non-idempotent trigger firers; bridges threaded
 * via regs_t — the callees return the live record cursor in EDX [A1/CONCAT44]): */
void     examine_world_object(uint32_t rec);       /* 0x35235 (EAX=record; dialogue eval + interact trigger) */
int32_t  examine_object_under_cursor(const regs_t *in); /* 0x10cb3 (RMB examine; -1 found / 0 not) */
int32_t  activate_targeted_object(const regs_t *in);    /* 0x164c9 (LMB use; returns cursor code / live EDX) */
int32_t  classify_cursor_target_object(const regs_t *in);/* 0x1624d (per-frame cursor-shape classifier) */
void     handle_cursor_click(void);                /* 0x1661f (dialogue/inspect UI click router) */
/* Cluster E — console field editing (editors are BLOCKING ring-spinners; statically DEAD): */
void     console_read_key_crlf(uint32_t eax);      /* 0x11337 (echo AL + LF + CR via dos_print_char bridge) */
uint32_t console_read_key(uint32_t eax);           /* 0x11548 (dequeue+translate, uppercase fold; EAX) */
void     console_edit_field(uint32_t ecx, uint32_t esi);      /* 0x113c0 (ECX=max, ESI=buffer; blocking editor) */
void     console_edit_text_field(uint32_t ecx, uint32_t esi); /* 0x113b9 (alnum-mode entry stub) */
void     console_edit_numeric_field(uint32_t esi); /* 0x11462 (ESI=value word; render/edit/parse-back) */

/* ============================== blit_2d (lift_blit_2d.c) ==============================
 * 2D fills/blitters. Framebuffer = [0x90a98] (A4 raw host ptr),
 * pitch [0x85498], height [0x8549c], hires doubling [0x90cbd]. */
void fill_rect_solid(uint32_t x, uint32_t y, uint32_t width, uint32_t height); /* 0x12cd4 (fill byte [0x7f355]) */
void clear_buffer_rows(uint32_t base, uint32_t off, uint32_t row0, uint32_t stride,
                              uint32_t count, uint32_t rows);                         /* 0x22760 (2 stack args; ret 8) */
void fill_span_solid(uint32_t ecx, uint32_t edi, uint8_t *es_base);            /* 0x39fc0 (DEAD; span emitter, word [0x89f10]) */
/* translucent overlays: FS = blend-LUT REGISTER INPUT (D5b) -> resolved base param; hires
 * displacement immediates are SMC-patched by 0x12dde and read LIVE from the code bytes (C1) */
void blit_translucent_overlay_block(int32_t x, int32_t y, const uint8_t *src,
                                           const uint8_t *fs_base);                   /* 0x12ec2 (4x4 corner) */
void draw_shadow_border_edge_h(int32_t x, int32_t y, uint32_t count,
                                          const uint8_t *src, const uint8_t *fs_base); /* 0x12f6d (<=4 rows x count) */
void draw_shadow_border_edge_v(int32_t x, int32_t y, uint32_t height,
                                            const uint8_t *src, const uint8_t *fs_base); /* 0x13010 (4 cols x height) */
void draw_popup_shadow_border_smc(int32_t x, int32_t y, int32_t w, int32_t h);      /* 0x12dde (shadow border; SMC patcher; fs=[0x90c0e]) */
/* Group B leaves: */
uint32_t clip_sprite_extents_to_screen(int32_t *x_io, int32_t *y_io, uint32_t *src_io,
                                              uint32_t wh);                           /* 0x11c3c (A1 multi-reg in/out; ret packed ECX, 0=clipped) */
void rle_decode_scroll_segment(uint8_t *d);                                    /* 0x1374f (EBP descriptor +0/+4/+8/+0x14/+0x24) */
void blit_opaque_rect(uint32_t src, uint32_t dst, uint32_t width, uint32_t rows); /* 0x13183 (hires: both lines, 2*pitch) */
void blit_transparent_sprite_clipped(uint32_t colrec, uint32_t src, uint32_t dst,
                                            int32_t rows, int32_t *desc);             /* 0x13be0 (viewmodel; desc +0x20 mutated) */
void blit_transparent_sprite_clipped_x2(uint32_t colrec, uint32_t src, uint32_t dst,
                                               int32_t rows, int32_t *desc);          /* 0x13c60 (x2 horizontal) */
void blit_transparent_sprite_clipped_shaded(uint32_t shade_al, uint32_t colrec,
                                                   uint32_t src, uint32_t dst,
                                                   int32_t rows, int32_t *desc);      /* 0x13cf0 (AL=shade -> fs=[0x90c0e] LUT row; +0x28) */
void blit_transparent_sprite_clipped_x2_shaded(uint32_t shade_al, uint32_t colrec,
                                                      uint32_t src, uint32_t dst,
                                                      int32_t rows, int32_t *desc);   /* 0x13d90 */
void draw_translucent_icon_spans(uint32_t dst, uint32_t src, uint32_t stride,
                                        int32_t rows);                                /* 0x13e81 (fs=[0x90be2] blend; outline 0s) */
void blit_image_scaled_to_framebuffer(uint32_t src, uint32_t x, uint32_t y,
                                             uint32_t width, int32_t rows,
                                             int32_t mode, int32_t submode);          /* 0x202d5 (3 stack args; ret 0xc) */
/* Group B mid: */
void blit_remapped_cursor_glyph(void);                                         /* 0x10d90 (glyph remap -> blit_scaled_sprite_at_mouse [L] direct-C) */
void blit_scaled_viewport_to_framebuffer(void);                                /* 0x2db40 (display-mode scaler; add_dirty_rect bridge) */
void blit_panel_image(uint32_t src, int32_t *panel, uint32_t dest_base);       /* 0x23cff (copy_nonzero [L] direct-C; dirty-rect bridges) */
void blit_image_to_video_target(uint32_t width, uint32_t row, uint32_t xoff,
                                       uint32_t rows, uint32_t src, uint32_t edi_in); /* 0x2de3c (LFB/banked/raw/planar; vesa-bank bridges, port hook) */
/* Group D — save-under: */
uint32_t save_framebuffer_region(uint32_t x, uint32_t y, uint32_t width,
                                        uint32_t height, uint32_t *cf_out);          /* 0x13062 (CALL-CLOSED; EAX=pool handle, CF=alloc fail) */
void blit_saved_ui_block(void);                                                /* 0x18a64 (globals 0x810xx; blit_opaque_rect [L] direct-C) */
void blit_sprite_save_under(uint32_t y, uint32_t x, uint32_t wh, uint32_t sprite,
                                   uint32_t screen_base, uint32_t rec);               /* 0x11945 (cursor draw+save; LFB/raw/banked/planar) */
/* Group E — software cursor chain: */
void blit_software_cursor(uint32_t rec);                                       /* 0x11ce1 (save-under RESTORE; type-checked per mode) */
void restore_cursor_region_if_set(uint32_t rec);                               /* 0x11cd2 (armed-record gate) */
void draw_software_cursor_sprite(uint32_t rec, uint32_t dx_seg);               /* 0x118a9 (SMC stride patcher; DX=surface segment) */
void restore_and_redraw_cursor(void);                                          /* 0x11f49 (LFB path = the 0x11f19 cross-TU tail, inlined) */
void redraw_view_region_shadow_border(void);                                      /* 0x12b20 (shadow border from 0x85ce0..dc — direct C) */
void update_software_cursor(void);                                             /* 0x116b6 (DEAD; two-page redraw; planar latch save via port hooks) */
void force_cursor_redraw(void);                                                /* 0x116aa (poison last-x -> update) */
void set_cursor_shape(uint32_t id_in);                                         /* 0x12a08 (AX=id; RLE decode -> 0x7e950; 0x115dd [L] direct-C) */
void draw_current_mouse_cursor_sprite(void);                                   /* 0x18bb2 (held-item icon at mouse; save_fb_region+icon direct C) */
/* Group F — screen slide transitions (the play_* entries + snapshot BLOCK on the frame tick
 * [0x90bcc] -> live-swap INTERACTIVE tier, G3; the leaf is oracle-verified): */
void blit_slide_transition(int32_t *desc, int32_t phase);                      /* 0x12d27 (venetian wipe; fs=[0x90c0e] fade LUT) */
void play_screen_slide_in(uint32_t music_flag, uint32_t srcp);                 /* 0x1ffb7 (INTERACTIVE; audio [L] + dirty/flip bridges) */
void play_screen_slide_out(uint32_t srcp);                                     /* 0x20134 (INTERACTIVE; EAX = ptr-to-src deref'd once) */
void snapshot_screen_and_slide_out(void);                                      /* 0x20b91 (INTERACTIVE; pool snapshot + slide_out; call-closed) */

/* ============================== video_display (lift_video_display.c) ==============================
 * Mode set / page flip / palette DAC / dirty-rect present.
 * Dirty-rect list: count [0x7f57c], rects [0x7f580] stride 0x10 (l/t/r/b); screen pitch [0x85498]
 * height [0x854a0]; hires doubling [0x90cbd]. */
/* A. Dirty-rect present pipeline */
void add_dirty_rect(uint32_t left, int32_t top, uint32_t right, uint32_t bottom);   /* 0x15b69 (RECURSIVE coalescer; PURE obj3 list) */
void register_dirty_rect(uint32_t left, int32_t top, uint32_t right, uint32_t bottom); /* 0x15b5b (hires-double prefix -> add_dirty_rect) */
/* B. Palette DAC + RGB remap. g_palette_rgb_ptr [0x85488] (A4 stored ptr, 256*3 6-bit RGB). */
uint8_t find_nearest_palette_color(const uint16_t *table, int32_t r, int32_t g,
                                          int32_t b, int32_t count);                        /* 0x20437 (Manhattan; ret AL, ret 4) */
uint8_t find_nearest_palette_index(uint32_t idx, const uint8_t *table);              /* 0x2ffae (weighted 4G+2R+B; ret AL) */
void build_view_grayscale_lut(void);                                                 /* 0x12be2 (grayscale LUT 0x7f254 + valid 0x7f354) */
void remap_pixels_to_palette(const uint8_t *src_pal, const uint8_t *dst_pal,
                                    uint8_t *out, const uint8_t *stream, int32_t count);     /* 0x204a3 (expand+double+nearest; call-closed) */
void grayscale_background_view(void);                                                 /* 0x12b45 (desaturate view region via LUT; add_dirty_rect + build LUT) */
void remap_rgb_to_palette_indices(int32_t count, const uint8_t *src, uint8_t *out);   /* 0x301eb (EAX=count EDX=src EBX=out; ref pal [0x90bca]<<4 low-mem) */
uint32_t copy_vesa_mode_list_block(const uint8_t *info_block, uint8_t *dest);         /* 0x27f9b (EAX=VBE block, ES:EDI=dest; ret ECX=0x80) */
/* B (hardware half) — palette DAC upload, IN-GAME live-swap (port I/O via g_os_port_out). */
void upload_palette_dac(void);                                                        /* 0x2febe (void; [0x90bca]<<4 -> DAC, lock [0x89f3b]) */
void upload_dac_palette_6bit(uint32_t src);                                           /* 0x2fefb (EDI=src, 6-bit direct; 0-caller) */
uint32_t upload_dac_palette_8to6(uint32_t src);                                       /* 0x2feff (EAX=src, 8->6-bit; 0-caller) */
void refresh_palette_dac(void);                                                       /* 0x2ff38 (-> upload_palette_dac) */
void refresh_palette_dac_wrapper(void);                                               /* 0x2fea0 (-> upload_palette_dac) */
void write_vga_palette(uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx);       /* 0x4c334 (EDX=src ECX=count EBX=idx; callback [0x91d00]) */
/* A (mid/entry) — dirty-rect present pipeline, IN-GAME live-swap (framebuffer/cursor + planar port I/O). */
void begin_screen_draw(void);                                                         /* 0x11ca9 */
void end_screen_draw(void);                                                           /* 0x11cc6 */
void redraw_cursor_after_blit(void);                                                  /* 0x2ddce */
void present_dirty_cursor_region(void);                                               /* 0x11eec (planar out 0x3c4/0x3ce) */
void blit_screen_rect(uint32_t left, int32_t top, uint32_t right, uint32_t bottom);   /* 0x2dddd */
void blit_dirty_rect_list(uint32_t list, int32_t count);                              /* 0x2dd85 */
void flush_predraw_hook(void);                                                        /* 0x3cf19 (save_snapshot_file [L]) */
void flush_dirty_rects(void);                                                         /* 0x15dd9 (present ENTRY) */
void mark_overlay_dirty_rects(void);                                                  /* 0x1f330 (register_dirty_rect + free_das_cache_handle) */
void remap_builtin_palette_image(void);                                               /* 0x10d67 (remap glyph + blit_remapped_cursor_glyph) */
/* C / D — mode set / resolution + framebuffer surfaces, IN-GAME live-swap (DPMI/VESA/DOS bridges). */
void build_scanline_dest_offset_table(void);                                          /* 0x2fb49 (ORACLE; table + SMC patch) */
void set_resolution_index_and_cycle_display(uint32_t idx);                            /* 0x147e6 */
void set_resolution_index_and_cycle(uint32_t idx);                                    /* 0x147f4 */
void init_video_mode_table_once(void);                                                /* 0x14772 */
uint32_t init_video_surface(void);                                                    /* 0x2fc98 (-> CF from alloc_framebuffer_surface tail) */
void alloc_scene_render_arena(void);                                                  /* 0x2a909 (DPMI selector) */
uint32_t alloc_framebuffer_surface(void);                                             /* 0x2fbe8 (ret CF) */
void free_framebuffer_surface(void);                                                  /* 0x2fce9 */
void match_vesa_video_modes(uint32_t table, uint32_t count);                          /* 0x27fbf (EAX=table EDX=count) */
void cycle_screen_resolution(void);                                                   /* 0x1480c (733B mode-set orchestrator) */

/* ---- menu_hud_ui — lift_menu_hud_ui.c ---- */
/* Layer A — UI panel primitives (leaves). */
int32_t  hit_test_ui_element(int32_t *cells, int32_t y, uint32_t x, uint32_t count,
                                    uint32_t xbase, uint32_t flags, int32_t ybox, int32_t xbox); /* 0x24b1e (ORACLE; row/frame hit-test) */
void     scroll_entry_into_view(uint32_t *param_1);                                       /* 0x1b0e3 (ORACLE; clicked-entry scroll clamp) */
void     draw_ui_panel_image_at_xy(uint32_t dest_base, uint32_t img_id, int32_t x, int32_t y,
                                          uint32_t pitch);                                       /* 0x2271d (in-game; resolve+blit, ret 4) */
void     draw_ui_panel_image_block(uint32_t dest_base, uint32_t img, int32_t x, int32_t y,
                                          uint32_t pitch);                                       /* 0x227b1 (in-game; blit block, ret 4) */
void     draw_ui_panel_count_element(void);                                               /* 0x1a0ab (in-game; HUD count badge) */
void     update_ui_overlay(void);                                                         /* 0x1a132 (in-game; inventory panel gate) */
void     show_status_message_wrap(uint32_t eax);                                          /* 0x17fe5 (DEAD; -> show_no_ammo_message) */
void     reset_hud_icon_state(void);                                                      /* 0x1bc91 (DEAD; invalidate HUD icons) */
/* Layer B — HUD panels (in-game). */
void     clear_corner_peek_icon(void);                                                    /* 0x167d7 */
void     restore_corner_peek_icon(void);                                                  /* 0x16807 */
void     draw_character_portrait_corner(int32_t param_1);                                 /* 0x16831 (EAX=anim offset) */
void     render_health_status_panel(uint32_t handle, uint32_t desc);                      /* 0x22c84 (EAX=handle, EDX=desc) */
void     render_player_health_bar(uint32_t base, uint32_t desc);                          /* 0x235fe (EAX=base, EDX=desc) */
void     draw_active_ui_panels(void);                                                     /* 0x240d7 (defined w/ Layer D deps) */
void     refresh_hud_layout(void);                                                        /* 0x243be */
void     repaint_hud_and_present(void);                                                   /* 0x17317 */
/* Layer D — panel renderers referenced by Layer B/C (defined in Layer D). */
void     render_ui_texture_panel(uint32_t handle, uint32_t edx);                          /* 0x227e9 */
void     render_ui_panel_text(uint32_t a, uint32_t b, uint32_t panel);                    /* 0x23897 */
void     draw_scroll_indicators(uint32_t p1, int32_t p2, int32_t p3);                     /* 0x24ebe */
void     draw_menu_value_bar(int32_t x, int32_t y, uint32_t marker, int32_t value);       /* 0x247bc */
void     render_menu_entry_list(uint32_t p1, int32_t p2, uint32_t count, uint32_t p4);    /* 0x2491f */
uint32_t render_text_input_field(uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4, int32_t p5); /* 0x244da (INTERACTIVE; ret 4 -> EAX) */
/* Layer C — message boxes + fullscreen image. */
void     draw_menu_box_zoom_anim(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t desc);    /* 0x24fb1 (ret 4) */
uint32_t load_and_center_fullscreen_image(uint32_t p1);                                           /* 0x20e98 (EAX->EAX) */
uint32_t show_fullscreen_image(uint32_t p1);                                                      /* 0x20f81 (INTERACTIVE; EAX->EAX) */
uint32_t show_message_box(uint32_t desc, uint32_t flags);                                         /* 0x2508f (INTERACTIVE; EAX=desc EDX=flags -> EAX) */
uint32_t show_resource_error_box(void);                        /* 0x2632a (-> button EAX; was void) */
uint32_t show_simple_message_box(void);                        /* 0x2633c (-> button EAX; was void) */
uint32_t run_settings_menu(uint32_t item, uint32_t default_ret);                                  /* 0x24c72 (INTERACTIVE; AL=case + EDX=default -> EAX; orig preserves ebx/ecx) */
uint32_t run_options_menu(uint32_t eax);                                                          /* 0x26501 (INTERACTIVE; no args -> EAX code) */
uint32_t run_main_menu(uint32_t eax);                                                             /* 0x26628 (INTERACTIVE; no args -> EAX transition code) */

/* ---- map_load (lift_map_load.c) — level-load / warp / raw-state stream ---- */
/* Layer A leaves */
void     parse_map_das_filename(char *src, char *dst);                     /* 0x1078a (EAX=src, EDX=dst; +.DAS) */
uint32_t lookup_map_raw_filename(const char *name);                        /* 0x10686 (EAX=name -> EAX=cursor|0; scratch 0x701ec +.RAW) */
void     select_map_entry_by_index(uint32_t index);                        /* 0x1073a (EAX=index; 0x701ec+.RAW, 0x7023c+.DAS) */
void     flag_sectors_with_objects(uint8_t *geom, int16_t *objects);       /* 0x4f221 (EAX=geom, EDX=objects) */
void     flag_referenced_object_textures(void);                            /* 0x33c49 (ESI=g_object_table_header) */
void     init_movement_tuning_from_first_map(uint8_t *gs);                 /* 0x2f7bb (GS self-load metadata) */
/* orchestration (in-game live-swap; bridge DOS/DPMI/cross-subsystem via call_orig) */
void     init_loaded_map_state(void);                                      /* 0x2f6e6 */
void     release_raw_state_and_setup_sfx(void);                            /* 0x2f708 */
void     unload_map_geometry(void);                                        /* 0x2f459 (DPMI selectors) */
void     rebuild_object_pointer_array(void);                               /* 0x33ea1 (pool handle array) */
uint32_t open_raw_state_temp(void);                                        /* 0x218de (-> EAX handle) */
void     write_raw_state_temp(void);                                       /* 0x21879 */
uint32_t load_raw_state_from_temp(uint32_t handle);                        /* 0x21934 (EAX=handle -> EAX status) */
void     load_map_list(const uint8_t *src);                                /* 0x1059b (ESI=src) */
void     switch_map_das_resources(uint32_t name_eax, uint32_t das_edx);    /* 0x105c0 (EAX=name, EDX=das) */
void     init_loaded_object_table(void);                                   /* 0x33f0e (obj-link resolve + type dispatch) */
uint32_t load_map_das_file(uint32_t name);                                 /* 0x2f1b4 (EAX=name -> CF; inline int21) */
uint32_t load_raw_map_file(uint32_t name);                                 /* 0x2f4b4 (EAX=name -> CF+AX; DOS+DPMI) */
uint32_t write_raw_state_stream(uint32_t handle);                          /* 0x2114e (EAX=handle) */
uint32_t read_raw_state_stream(uint32_t handle);                           /* 0x214b9 (EAX=handle -> EAX) */
uint32_t relocate_player_to_warp_sector(void);                             /* 0x10a4d (-> CF=0) */
uint32_t process_map_warp_or_load(void);                                   /* 0x1096f (-> CF) */

/* ---- dialogue_ui (lift_dialogue_ui.c) — inspect popup / choice menu / timed-text / cursor dispatch
 *      Canon VAs; runtime = canon + OBJ_DELTA. ---- */
/* Sub-A: choice-menu logic (oracle-able). */
int32_t  node_has_available_choice(uint32_t node);        /* 0x1fa91 (EAX=node -> EAX bool) */
void     choice_select_prev(void);                        /* 0x1fb1e */
void     choice_select_next(void);                        /* 0x1fc16 */
void     choice_accept_selected(void);                    /* 0x1fbba (tail-calls execute_dialogue_branch) */
void     activate_selected_choice_record(uint32_t idx);   /* 0x1badd (EAX=1-based idx) */
uint32_t update_dialogue_choice_highlight(int32_t p1, int32_t p2); /* 0x1f71d (EAX,EDX -> EAX) */
uint32_t build_available_choice_menu(uint32_t node);      /* 0x1f950 (EAX=node -> EAX count) */
void     dialogue_window_open_hook(void);                 /* 0x1ef9e (DEAD; shared epilogue) */
/* Sub-B: timed-message text cache (oracle-able). */
int32_t  lookup_cached_timed_message(uint32_t dest, int32_t key); /* 0x1e7c3 (EAX=dest,EDX=key -> EAX len) */
void     store_cached_timed_message(uint32_t text, int32_t key,
                                           int32_t len, uint8_t color);  /* 0x1e827 (EAX,EDX,EBX,CL) */
int32_t  layout_timed_message_text(int32_t *meta, uint8_t *out,
                                          const uint8_t *src, int32_t maxw, int32_t maxlines); /* 0x1f3d3 */
/* Sub-D: inspect-popup oracle-able leaves. */
uint32_t copy_record_block_op7(uint32_t record, uint32_t dest, uint32_t cap); /* 0x1854b (EAX,EDX,EBX) */
uint32_t find_oninspect_block_by_id(uint32_t id);                             /* 0x1ddeb (EAX=id) */
/* Sub-E: cursor hit-test (pure leaf, oracle-able). */
int32_t  hit_test_dialogue_ui_action(int32_t p1, int32_t p2, uint32_t p3);    /* 0x1ad2f (EAX,EDX,EBX) */
/* In-game-tier lifts (register in lift_registry.c; DAS/framebuffer/input/dbase100 bridges). */
void     free_inspect_overlay_image(uint32_t param_1, uint32_t param_2);       /* 0x18cb9 (EDX=flags) */
void     free_inspect_popup_and_redraw(uint32_t handle, uint32_t param_2);     /* 0x19678 (EAX,EDX) */
void     render_active_timed_message(void);                                    /* 0x1ed9c (timed-msg overlay) */
void     render_dialogue_text_panel(int32_t param_1);                          /* 0x1ec78 (EAX; 1=no fade) */
void     render_choice_text_panel(void);                                       /* 0x1efa4 */
void     render_text_ui(int32_t param_1);                                      /* 0x1f0e8 (EAX; dispatcher) */
void     draw_reloc_ui_row(int32_t param_1, int32_t param_2);                  /* 0x1c512 (EAX=y,EDX=off) */
void     update_dialogue_cursor_and_click(void);                               /* 0x16585 (per-frame cursor) */
uint32_t dbase100_open_dialogue_window(uint8_t *param_1);                      /* 0x1eabc (EAX -> 1) */
uint32_t dbase100_open_dialogue_window_alt(uint8_t *param_1);                  /* 0x1ebb4 (EAX -> 1) */
uint32_t update_inspect_popup_choices(void);                                   /* 0x18ada (-> 1/0) */
uint32_t render_inspect_popup_window(uint32_t width, uint32_t height,
                                            uint32_t node, uint32_t p4);                 /* 0x18e9e (EAX,EDX,EBX,ECX) */
uint32_t dispatch_dialogue_ui_action(uint32_t action, uint32_t p2,
                                            uint32_t p3, uint32_t p4);                    /* 0x1b4e5 (EAX=action,EDX=flags -> EAX) */
int32_t  load_inspect_document_page(uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4); /* 0x1951d */
uint32_t run_timed_message_sequence(uint32_t mode, uint32_t p2, uint32_t p3, uint32_t p4); /* 0x1fce2 */

/* ---- file_config ---- */
/* Sub-A: CONFIG.INI parse stack (shared-EBP-frame family; inner three are pure, oracle-able). */
int32_t  parse_config_number(uint8_t **edi_io, int32_t *ecx_io);               /* 0x41ed1 (EDI=str,ECX=count -> EAX; EDI/ECX threaded out) */
uint8_t *parse_config_typed_value(uint8_t type, uint8_t *field, uint8_t *edi,  /* 0x41dec (AL=type,EDX=field, */
                                         int32_t *ecx_io, uint8_t **bump);            /*   [ebp+0x14]=bump; -> new EDI) */
uint8_t *parse_config_line(uint8_t *edi, int32_t *ecx_io, uint8_t *tmpl,       /* 0x41d44 (EDI=text, [ebp+8]=tmpl, */
                                  uint8_t *struct_base, uint8_t **bump);              /*   [ebp+0x10]=struct; -> new EDI) */
uint32_t parse_config_keywords(uint8_t *filename, uint8_t *tmpl,               /* 0x41c5c (EAX=file,EDX=tmpl, */
                                      uint8_t *struct_base, int32_t bufsize);         /*   EBX=struct,ECX=cap -> EAX=struct/0; int21) */
/* Sub-B: leaf file utilities (DOS IO bound — in-game live-swap tier). */
uint32_t dispatch_arg_command(uint32_t dx);                                   /* 0x107b3 (DX=tok len -> CF; table dispatch) */
uint32_t load_raw_file_wrapper(uint32_t name);                                 /* 0x10c13 (EAX=name -> CF) */
uint32_t load_file_fully(uint32_t filename);                                   /* 0x1522d (EAX=name -> EAX=buf/0) */
uint32_t load_file_blob(uint32_t filename);                                    /* 0x143b0 (EAX=name -> EAX=buf/0; raw int21) */
uint32_t enumerate_files_by_pattern(uint32_t pattern, uint32_t dest);          /* 0x217bc (EAX=pat,EDX=dst -> EAX=count) */
/* Sub-C: config-field dispatch + mid-level loaders (interactive / DOS IO -> in-game). */
uint32_t dispatch_config_field_key(uint32_t eax);      /* 0x11382 (AL=key in -> CF; key was dropped) */
uint32_t dispatch_config_field_key_alt(uint32_t eax);  /* 0x1134d (AL=key in -> CF; flow_succ pair) */
void     delete_temp_files(void);                                              /* 0x21806 (find+delete *.TMP) */
uint32_t load_dbase300_chunk(uint32_t index);                                  /* 0x15492 (EAX=idx -> EAX 0/1) */
/* Sub-D: config entry points (startup/settings; DOS+pool+UI -> in-game). */
void     parse_config_ini_paths(void);                                         /* 0x10f6c (INI paths -> g_dir_*) */
uint32_t load_disk_path_config(uint32_t p1, uint32_t p2);                       /* 0x26965 (-> EAX 0/-1) */
void     write_roth_ini(void);                                                 /* 0x266ec (serialize settings) */
void     read_roth_ini(void);                                                  /* 0x267f4 (load settings) */
void     load_roth_res(void);                                                  /* 0x10458 (master startup config) */
/* Sub-E: inspect-document modal (in-game live-swap only; interactive-lift mode). */
uint32_t load_dbase300_resource_at_offset(uint32_t offset, uint32_t param_2,   /* 0x196b9 (EAX,EDX,EBX=record, */
                                                 uint32_t record, uint32_t param_4);  /*   ECX=p4; modal popup) */

/* ---- dos_runtime (lift_dos_runtime.c): the pure, oracle-verifiable OS-glue leaves. ---- */
uint32_t set_errno(uint32_t value);                             /* 0x55ba1 (EAX=val -> *errno; EAX=&errno) */
uint32_t set_doserrno(uint32_t value);                          /* 0x55bc4 (EAX=val -> *_doserrno; EAX=&_doserrno) */
uint32_t memset_fill_dwords(uint32_t dst, uint32_t val, uint32_t count); /* 0x55277 (EAX/EDX/ECX -> EAX=dst+count*4) */
uint32_t build_dos_command_tail(uint32_t argv, uint32_t dest, uint32_t mode); /* 0x55a33 (EDX/EBX/ECX) */
uint32_t map_dos_error_to_errno(uint32_t dos_err, uint32_t flag);/* 0x5612f+0x56133 (EAX=err,EDX=flag -> EAX) */

#endif /* ROTH_LIFTED_H */
