/* lift_game_core.c — the ROTH `game_core` subsystem (program entry / startup / lifecycle / the
 * per-frame gameplay loop) lifted to verified C. Own TU per docs/operating/recomp.md §4.6.
 * lift-lens: docs/reference/lift/game_core.md.
 *
 * game_core is the INTEGRATION SPINE — it is almost pure call sequencing: it calls into nearly every
 * other subsystem in a fixed per-frame/startup order. It owns no complex structs; it threads the shared
 * global game state owned by the callees. So the risk here is ORDERING / side-effect fidelity, not
 * arithmetic (lift-lens §7). Lifted LAST, after all per-frame callees are lifted-or-bridged.
 *
 * METHOD — the faithful "register machine": every orchestrator threads ONE persistent regs_t `r` through
 * its bridges. call_orig round-trips the full GP file (trampoline.S), so Watcom callee-saved esi/edi/ebp
 * persist across a bridge exactly like the original, and scratch eax/edx/ebx/ecx take the callee's real
 * outputs. That means a value the original consumes from a *bridged* callee's return (Ghidra drops these,
 * gotcha A0/§8) is reproduced automatically — e.g. game_play_loop's `ebx` state selector comes out of
 * clear_damage_flash 0x179d2, and init_game_databases' mem_fill length comes out of resolve_dbase100_sound_ids.
 * The transcription mirrors the disasm block-for-block (goto labels = canon addrs) so nothing is reordered.
 *
 * ADDRESSING (verified against recomp/build/obj1.bin): an address IMMEDIATE the original loads is
 * relocated at runtime (`mov edx,0x410345`, `mov eax,0x475c44`), so pass it as GADDR(canon); a pure
 * numeric constant (a size/count/id like 0x25800/8/0x4d2/0x800) is NOT relocated -> pass it raw. Stored-
 * pointer globals (framebuffer ptr 0x90a98, dbase base 0x81e1c) hold runtime addresses -> read via G32 and
 * deref RAW (gotcha A4).  Verification tier: nearly all in-game live-swap only (it IS the game loop, non-
 * idempotent); update_frame_time_scale is oracle-verified (test_game_core.c).
 */
#include "common.h"
#include "engine.h"
#include <string.h>
#ifdef ROTH_STANDALONE
#include "os_api.h"   /* os_dos_print_string — the imgfree dispatch for the two AH=9 print thunks */
#endif

extern uint16_t g_os_game_ds;   /* game DS captured per dispatch (roth_game_startup stores it to a code global) */
extern uint16_t g_os_game_cs;   /* game CS captured per dispatch (roth_main_sequence far-ptr timer-event reg) */

#ifdef ROTH_STANDALONE
/* image-free boot: with no original CODE bytes mapped there is
 * no call_orig bridge to fall back on, so an un-dispatched (in-game-only) gc_call target on the
 * boot-to-title path is a host abort. Defined by the roth-host imgfree TU. */
void roth_unreachable(uint32_t canon);
#endif

/* Bridge into a callee at `canon`, threading the persistent register file `r` (inputs already staged in
 * r; on return r holds the callee's outputs incl. eflags — CF = bit 0).
 *
 * CALL-CLOSED game_core: a game_core function that calls ANOTHER game_core function invokes it as DIRECT C
 * (not a call_orig bridge to the original), so a whole-tier live-swap (ROTH_LIFT=game_play_loop, or =main)
 * runs the entire spine as verified C inside ONE dispatch. That matters for CORRECTNESS of the frame
 * timing: the per-frame loop paces on the frame-tick counter 0x90bcc, which during a lift dispatch only
 * advances via shm_tick (the real int-8 ISR is frozen). Running the loop in a SINGLE dispatch gives ONE
 * tick source; single-swapping a per-frame sub-function instead double-counts the tick (shm_tick during the
 * dispatch + the real ISR between dispatches) -> ~2x speed. So the intended verification tier for these is
 * the full loop, and call-closure is what makes that tier exercise the sub-functions' C. (This is also the
 * workstream-D "re-point bridge -> direct C" step for the 8 game_core-internal edges.) All cross-subsystem
 * callees (audio/render/input/DOS/...) stay bridged. */
static void gc_call(regs_t *r, uint32_t canon)
{
    switch (canon) {
    case 0x15110: roth_main();                        return;   /* (entry; only reached if something calls main) */
    case 0x10010: roth_game_startup();           return;
    case 0x100f6: roth_main_sequence();          return;
    case 0x179ee: game_play_loop();              return;
    case 0x1dfc2: init_game_databases();         return;
    case 0x1792c: gameplay_frame_step();         return;
    case 0x1691c: run_gameplay_frame();          return;
#ifdef ROTH_STANDALONE
    /* --- in-game frame-tick tier (imgfree-only; reached once the render pipeline completes and
     * gameplay_frame_step runs). Each dispatches to its already-lifted body with the disasm ABI, but
     * kept IMGFREE-ONLY so the trap lane / game_core live-swap stays byte-identical. --- */
    case 0x42d74: r->eax = (uint32_t)tick_dynamic_entities(r); return;  /* tick_dynamic_entities (entity_ai) */
    /* gameplay_frame_step callees (disasm: each pushes its scratch regs and reads only globals unless noted) */
    case 0x15efe: tick_item_pickup_lock();                     return;  /* push ebx/ecx/edx/esi; globals only -> void(void) */
    case 0x1729c: update_player_tick();                        return;  /* entry calls 0x1035a(pushal)+0x2db40(global-gated) then push-frame; no reg-in -> void(void) */
    case 0x1bcc4: draw_held_item_icon();                       return;  /* push ebx/ecx/edx; globals 0x85cdc/0x819bc.. -> void(void) */
    case 0x240d7: draw_active_ui_panels();                     return;  /* full push frame; no reg-in -> void(void) */
    case 0x1f0e8: render_text_ui((int32_t)r->eax);             return;  /* mov edi,eax @0x1f0f1 -> EAX=mode in */
    case 0x16831: draw_character_portrait_corner((int32_t)r->eax); return;  /* mov esi,eax @0x16836 -> EAX=anim in */
    case 0x15dd9: flush_dirty_rects();                         return;  /* enter 0x404; globals only -> void(void) */
    case 0x16807: restore_corner_peek_icon();                  return;  /* gated on [0x7fdbc]; globals only -> void(void) */
    case 0x1f330: mark_overlay_dirty_rects();                  return;  /* gated on [0x827e9]; globals only -> void(void) */
    case 0x2ff38: refresh_palette_dac();                       return;  /* push ecx/edx/eax around 0x2febe (reads [0x89f3b]) -> void(void), eax preserved = C no-touch */
    case 0x2e1e8: host_flip_video_page(r->eax);                       return;  /* flip_video_page(EAX=mode): imgfree host present hook (== lift_file_config.c fc route) */
    /* game_play_loop pre-menu / outer-loop callees */
    case 0x1c57e: reset_inventory();                           return;  /* stages its own ebx/eax/edx for mem_fill -> void(void) */
    case 0x1818d: restore_active_held_item();                  return;  /* push ebx/ecx/edx/esi; call 0x1823a then globals -> void(void) */
    case 0x1e2bd: r->eax = build_entity_def_by_id(r->eax, r->edx); return;  /* mov ebx,eax; test edx -> EAX=dest, EDX=id -> EAX (caller's ebx=2 is NOT an input: overwritten @0x1e2bf) */
    case 0x1c96f: reset_player_locomotion_state();             return;  /* pure global stores -> void(void) */
    case 0x124dd: reset_movement_velocity_queues();            return;  /* pure global stores -> void(void) */
    case 0x179d2: clear_damage_flash();                        return;  /* [0x89f3b] gate + tail-jmp 0x2ff38; touches no GP reg -> void(void); caller-read ebx passes through untouched, matching the original preserve */
    case 0x26628: r->eax = run_main_menu(r->eax);              return;  /* ebx<-0x201, eax<-imm @entry (no reg-in) -> EAX=transition code out */
    /* render_dev_map_selector_ui 0x1754d — NO lifted body: the name's "selector" matched the DPMI
     * regex (tools/classify_functions.py:179) so it was classed dpmi_dos_os and skipped by the engine
     * denominator. Faithful transcription of 0x1754d..0x175c7 (the 0x103ea precedent); all three
     * callees ARE lifted. No reg-in (push ebx/ecx/edx; enter 0x400 = the stack text buffer). */
    case 0x1754d: {
        if (G32(VA_g_help_overlay_enabled) != 0) {                                               /* 0x17554 timed-message gate */
            if (G32(VA_g_timed_message_timer) == 0)
                show_no_ammo_message(0x51);                             /* 0x17566: eax=0x51 -> call 0x1f8cb */
            else
                G32(VA_g_timed_message_timer) = 0x64;                                           /* 0x17572 */
        }
        if (G32(VA_g_map_menu_active) != 0 && G32(VA_g_map_list_ptr) != 0) {                          /* 0x1757c/0x17585 menu-mode gates */
            uint8_t buf[0x400];                                                /* [ebp-0x400] (lift_input.c:405 idiom) */
            build_map_selector_menu((char *)buf);                       /* 0x17594: eax=&buf -> call 0x17453 */
            draw_text_to_buffer((uint32_t)(uintptr_t)buf,               /* 0x175be: call 0x14d04(EAX/EDX/EBX/ECX) */
                                       (uint32_t)G32(VA_g_render_target_buffer) + 0xa,           /* edx = [0x85414]+0xa (stored ptr, A4) */
                                       (uint32_t)G32(VA_g_screen_pitch),                 /* ebx = pitch */
                                       (G8(VA_g_hires_line_doubling_flag) != 0) ? 1u : 0u);          /* ecx = ([0x90cbd]!=0) 0x1759b..0x175a4 */
        }
        return;
    }
    /* --- inner per-frame loop (L17c16) + mode branches --- */
    case 0x156bd: service_audio_sequence();                    return;  /* eax<-[0x7f468] @entry; globals only -> void(void) */
    case 0x1e9b5: voice_stream_pump();                         return;  /* gated [0x82010]; globals only -> void(void) */
    case 0x24165: render_weapon_hud(r->eax, r->edx);           return;  /* EAX=mode, EDX=panel rec (registry ABI_EAX4; caller reads no return) */
    case 0x14525: keymap_dispatch();                           return;  /* push fs/gs/pushal; ring drain -> void(void) */
    case 0x1c59e: clear_dual_array_80afc();                    return;  /* stages own eax/edx/ebx -> void(void) */
    case 0x184ab: activate_weapon_item(r->eax, r->edx);        return;  /* ecx<-eax, eax<-edx @0x184ad -> (EAX,EDX) in */
    case 0x1bb4b: update_selected_item_icon();                 return;  /* enter 0x1004 stack buf; globals only -> void(void) */
    case 0x1661f: handle_cursor_click();                       return;  /* push ebx/ecx/edx; xor ecx @entry -> void(void) */
    case 0x1a178: redraw_inventory_cursor_cell();              return;  /* gated [0x80b2c]; globals only -> void(void) */
    case 0x18bb2: draw_current_mouse_cursor_sprite();          return;  /* full push frame; globals only -> void(void) */
    case 0x18cb9: free_inspect_overlay_image(r->eax, r->edx);  return;  /* registry ABI_EAX_EDX (EDX=flags) */
    case 0x1a8e5: update_inventory_screen();                   return;  /* [0x80b30]=0 @entry; globals only -> void(void) */
    case 0x16585: update_dialogue_cursor_and_click();          return;  /* edx/eax<-button-edge globals @entry -> void(void) */
    case 0x1299a: r->eax = (uint32_t)input_ring_dequeue();     return;  /* xor ax,ax; al<-ring byte -> AL out (C caller reads (uint8_t)) */
    case 0x173f4: use_enter_key_handler();                     return;  /* push edx; global-gated -> void(void) */
    case 0x1fb1e: choice_select_prev();                        return;  /* eax<-[0x83115] @entry; globals only -> void(void) */
    case 0x1fc16: choice_select_next();                        return;  /* eax<-[0x83115] @entry; globals only -> void(void) */
    case 0x22129: r->eax = load_savegame_file(r->eax);         return;  /* EAX=slot -> EAX 1 ok/0 fail (registry ABI_EAX; caller test eax) */
    case 0x21dc6: r->eax = write_savegame_file(r->eax);        return;  /* EAX=slot -> EAX (registry ABI_EAX) */
    case 0x1096f: r->eax = process_map_warp_or_load();         return;  /* pushal-framed but writes the frame EAX slot: registry ABI_EAX "callers test eax" */
    case 0x147e6: set_resolution_index_and_cycle_display(r->eax); return;  /* pushal; dec eax @entry -> EAX=idx in, void */
    /* --- run_gameplay_frame (0x1691c) callees --- */
    case 0x167d7: clear_corner_peek_icon();                    return;  /* push edx; global-gated -> void(void) */
    case 0x1f71d: r->eax = update_dialogue_choice_highlight((int32_t)r->eax, (int32_t)r->edx); return;  /* EAX,EDX -> EAX (registry ABI_EAX4) */
    case 0x10c8f: r->eax = render_world_view(r->es, r->fs, r->gs); return;  /* ABI_RVIEW: segs ignored imgfree (callees reload from globals); pushal preserves rest; EAX=&0x90a48 out */
    case 0x1624d: r->eax = (uint32_t)classify_cursor_target_object(r); return;  /* ABI_FIRER: full reg snapshot in (EAX=hit rec, EDX=type byte) -> EAX */
    case 0x12a08: set_cursor_shape(r->eax);                    return;  /* AX=id (body masks 0xffff @0x12a0f) -> void */
    case 0x1a132: update_ui_overlay();                         return;  /* global-gated ([0x83115]/[0x83125]) -> void(void) */
    case 0x10cb3: r->eax = (uint32_t)examine_object_under_cursor(r); return;  /* ABI_FIRER: regs in (EBP<-EDX @0x10cbf) -> EAX -1 found/0 not */
    case 0x1768a: r->eax = trigger_weapon_fire(r->eax, r->edx, r->ebx, r->ecx); return;  /* registry ABI_EAX4 (674B fire keystone) */
    case 0x164c9: r->eax = (uint32_t)activate_targeted_object(r); return;  /* ABI_FIRER: regs in (EAX=hit rec; reads [eax+0x20]) -> EAX cursor code */
    case 0x12179: compute_view_offsets_90a74();                return;  /* push eax; globals only -> void(void) */
    case 0x10dce: draw_map_overlay();                          return;  /* push eax/ebx/edx + stack buf; globals only -> void(void) (debug map, gated [0x7f36e]) */
    case 0x1f671: dialogue_voice_force_end(r->eax, r->edx);    return;  /* (EAX,EDX) in (registry ABI_VOID4; only eax/edx live) */
    case 0x1f6cc: dialogue_voice_stop_all();                   return;  /* zeroes its own eax/edx then calls 0x1f671 -> void(void) */
    /* --- quit/death/teardown tier --- */
    case 0x1823a: free_active_item_hud_icon();                 return;  /* push edx; global-gated -> void(void) */
    case 0x26cd4: flush_object_das_handles();                  return;  /* edx<-[0x848f4] @entry; globals only -> void(void) */
    case 0x4263e: reset_entity_pools();                        return;  /* ecx<-[0x90fe0] @entry; globals only -> void(void) */
    case 0x40bc7: free_geometry_buffer_and_selector(r->eax);   return;  /* EAX=buffer (g_image_surface); + releases the [0x85294] geometry
                                                                                * selector — 0x2a8e9/0x40adf = sel-base heap free + c2 descriptor free
                                                                                * (body in lift_render_world.c; was the Quit-to-DOS fail-loud stop) */
#endif
    case 0x24f5e: update_frame_time_scale();     return;
    case 0x1107e: r->eax = (uint32_t)reset_and_start_new_game(NULL); return;  /* returns 0/-1 */

    /* ========================================================================================
     * the boot-spine (main/game_startup/main_sequence)
     * cross-subsystem targets that already have call_orig-CLEAN lifted bodies dispatch to DIRECT
     * lifted C — so the image-free boot needs no bridge for them. Per-target ABI is taken from
     * DISASM (tools/roth_disasm.py), not decomp: each case threads r exactly as call_orig would
     * (arg regs in; EAX and any CF/ZF the caller reads out). The two targets whose caller reads a FLAG
     * their once-void lifted body dropped (their tail-call CF) — 0x10c32 load_das_file_wrapper and
     * 0x2fc98 init_video_surface, both `jb` in main_sequence — now return that CF and dispatch via the
     * imgfree CF-exposure cases below; the oracle/default build still bridges both.
     * ======================================================================================== */
    /* --- program entry / heap (memory_pool, renderer.c) --- */
    case 0x35ff9: r->eax = alloc_largest_heap_block(r->eax);           return;  /* EAX=size -> EAX=handle */
    case 0x35fd9: r->eax = block_payload_size(r->eax);                 return;  /* EAX=ptr  -> EAX=size   */
    case 0x35bfa: free_heap_block((uint8_t *)(uintptr_t)r->eax);       return;  /* EAX=block             */
    case 0x40a2a: game_free_if_not_null((uint8_t *)(uintptr_t)r->eax); return;  /* EAX=block             */
    /* --- critical-error handler (dos_runtime) --- */
    case 0x436e8: install_critical_error_handler();                    return;
    case 0x43775: restore_critical_error_handler();                    return;
    /* --- config / INI / disk paths (file_config) --- */
    case 0x10458: load_roth_res();                                     return;
    case 0x10f6c: parse_config_ini_paths();                            return;
    case 0x21806: delete_temp_files();                                 return;
    case 0x267f4: read_roth_ini();                                     return;
    case 0x266ec: write_roth_ini();                                    return;
    case 0x26965: r->eax = load_disk_path_config(r->eax, r->edx);      return;  /* -> EAX 0/-1 (caller `!= -1`) */
    case 0x10c13: {                                                                     /* load_raw_file_wrapper(EAX=name) -> CF (caller `jb`) */
        uint32_t cf = load_raw_file_wrapper(r->eax);
        r->eflags = (r->eflags & ~1u) | (cf & 1u);
        return;
    }
    /* --- DAS assets / icons / backdrop / cache (das_assets) --- */
    case 0x10c70: load_ademo_das_wrapper(r->eax);                      return;  /* EAX=name */
    case 0x1602e: load_icons_all();                                    return;
    case 0x2fd21: {                                                                     /* init_backdrop_image_surface -> EAX; caller reads ZF=(EAX==0) via `or eax,eax; ret` */
        uint32_t v = init_backdrop_image_surface();
        r->eax = v;
        r->eflags = (r->eflags & ~0x40u) | (v == 0 ? 0x40u : 0u);
        return;
    }
    case 0x2fd6b: close_das_file_handle();                             return;
    case 0x2f163: close_das_handles_and_buffers();                     return;
    case 0x30114: init_das_cache_heap();                              return;
    case 0x30149: release_das_and_geometry_buffers();                  return;
    case 0x3001b: reset_das_entry_status_table();                      return;  /* reads no incoming reg (disasm) */
    /* --- audio (audio) --- */
    case 0x10c51: r->eax = load_sfx_file_wrapper(r->eax);              return;  /* EAX=name */
    case 0x1558d: process_audio_sequence_chunk();                      return;
    case 0x15813: r->eax = sos_audio_init();                          return;
    case 0x159fa: r->eax = register_music_timer_event(r->eax, r->edx); return;  /* EAX=cb_off, EDX=cb_sel -> EAX (caller `== 0`) */
    case 0x15a30: remove_music_timer_event();                          return;
    case 0x15ac8: sos_audio_shutdown();                                return;
    case 0x15ec4: free_sfx_scratch_buffer();                           return;
    case 0x2626f: r->eax = apply_audio_volume_settings();              return;
    /* --- video / surface / resolution (video_display) --- */
    case 0x10d67: remap_builtin_palette_image();                       return;
    case 0x11ca9: begin_screen_draw();                                 return;
    case 0x11cc6: end_screen_draw();                                   return;
    case 0x147f4: set_resolution_index_and_cycle(r->eax);              return;  /* EAX=idx */
    /* --- input / mouse (input) --- */
    case 0x103c8: begin_frame_then_init_mouse();                       return;
    /* --- map geometry (map_load) --- */
    case 0x2f459: unload_map_geometry();                               return;
    case 0x2f6e6: init_loaded_map_state();                            return;
    case 0x2f7bb:                                                                       /* init_movement_tuning: original self-loads GS=sel[0x90be8]; supply its flat base */
        if (g_os_sel_base) {                                                          /* g_os_sel_base(G16(0x90be8)) idiom (lift_doors.c:916, same selector) */
            init_movement_tuning_from_first_map(
                (uint8_t *)(uintptr_t)g_os_sel_base(G16(VA_g_geometry_selector)));
            return;
        }
        break;   /* no selector hook (oracle default): fall through to the bridge — always set on the imgfree path */
    /* --- databases / commands / math / misc-engine --- */
    case 0x1e0a9: free_dbase100_data();                               return;
    case 0x33c3e: r->eax = (uint32_t)set_state_record_count(r->eax);   return;  /* EAX=count */
    case 0x3c28c: thunk_build_atan_table();                            return;
    case 0x2f42b: reset_renderer_tables();                            return;
    case 0x2f962: init_render_struct_89ed0();                          return;

#ifdef ROTH_STANDALONE
    /* ========================================================================================
     * the boot-spine's SECOND-ORDER targets — their
     * lifted bodies existed but bridged one level deeper into host-boundary vector / DPMI-selector /
     * exception installs. Those bodies now carry #ifdef ROTH_STANDALONE guards (this wave) making them
     * call_orig-CLEAN image-free — the vector/exception installs become no-ops (Q4: the always-
     * interactive host-mode SIGALRM/shm-ring IS the ISR; host owns faults) and the DPMI selector mint
     * routes to the os call contract. So they dispatch to DIRECT lifted C HERE, standalone-only: the
     * oracle/default build keeps bridging every one of these via the call_orig tail below, UNCHANGED.
     * ======================================================================================== */
    case 0x1246a: install_keyboard_int9();                            return;  /* ring-reset kept; vector install no-op */
    case 0x12498: restore_keyboard_int9();                            return;  /* no-op image-free */
    case 0x12437: return;   /* install_timer_int8 (host_timer_driver): SIGALRM is the timer -> no int-8 vector/PIT program */
    case 0x124b5: return;   /* restore_timer_int8 (host_timer_driver): no-op image-free */
    case 0x416d3: install_exception_handler(r->eax, r->edx);          return;  /* #0 via c2; CRT #d/#e install no-op */
    case 0x41674: restore_exception_handler_and_report(r->eax);       return;  /* #0 via c2; CRT #d/#e restore no-op */
    case 0x2fcd4: shutdown_render_subsystem();                        return;  /* selector-free no-op; off-title-path teardown */
    case 0x2fa29: r->eflags = (r->eflags & ~1u) | ((uint32_t)allocate_das_worker_buffers() & 1u); return;  /* DPMI mint -> c2; CF out */
    case 0x10c32: r->eflags = (r->eflags & ~1u) | (load_das_file_wrapper(r->eax) & 1u);           return;  /* das FAT loader; CF out */
    case 0x2fc98: r->eflags = (r->eflags & ~1u) | (init_video_surface() & 1u);                    return;  /* init_video_surface; CF out (alloc_framebuffer_surface tail) */
    /* the two DOS AH=9 print thunks (OBJ1-A boot finding): both are `int 0x21` wrappers the host's
     * dos_int21 case 0x09 services — os_dos_print_string IS that call. Per disasm: 0x100e4 prints
     * EDX unless the quiet flag word [0x76758] is set (sole writer 0x107e7); 0x27c48 (CRT) prints EAX
     * unconditionally. */
    case 0x100e4: if (G16(VA_g_player_movement_enabled + 0xe) == 0) os_dos_print_string(r->edx);        return;  /* dos_print_dollar_string */
    case 0x27c48: os_dos_print_string(r->eax);                               return;  /* dos_print_string (CRT) */
    case 0x2fb7f: build_game_path((uint8_t *)(uintptr_t)r->eax,                /* build_game_path (clean lifted body; */
                                         (const uint8_t *)(uintptr_t)r->edx,          /*  EAX=dst EDX=dir EBX=name) */
                                         (const uint8_t *)(uintptr_t)r->ebx); return;
    case 0x1e874: open_dialogue_script();                             return;  /* dbase400 open-once (C leaf; its 0x2fb7f bridge routes in db_bridge) */
    case 0x1db5e: finish_dialogue_record_eval();                      return;  /* clear queue-active; overlay teardown gated on 0x83c70 */
    case 0x283a0: r->eax = xchg_849a4(r->eax);                        return;  /* xchg [0x849a4] swap — the frame-step code-ptr token store */
    case 0x17317: repaint_hud_and_present();                          return;  /* HUD repaint + present (its flip already routes, text via mh) */
    case 0x287b6: render_world_view_pass(r->eax);                     return;  /* the per-frame 3D render entry — body 166/166-lifted;
                                                                                       * inner fail-louds (SMC mappers / Class-B bridges) are the first image-free render targets */
    case 0x41ae5: r->eax = dos_open_file(r->eax, r->edx);             return;  /* dos_open_file -> handle (C2 wrapper) */
    case 0x41b41: dos_close_handle(r->eax);                           return;  /* dos_close_handle */
    case 0x41b53: r->eax = dos_read_items(r->eax, r->edx, r->ebx, r->ecx); return;  /* dos_read_items */
    case 0x41bc1: {                                /* dos_get_file_size: lseek(h,0,END) -> size; lseek(h,0,SET) back */
        uint32_t size = dos_lseek(r->eax, 0, 2);
        dos_lseek(r->eax, 0, 0);
        r->eax = size;
        return;
    }
    case 0x1517d: r->eax = game_heap_alloc((int32_t)r->eax);          return;  /* game_heap_alloc */
    case 0x15191: game_heap_free((uint8_t *)(uintptr_t)r->eax);       return;  /* game_heap_free */
    case 0x1def8: resolve_dbase100_sound_ids();                       return;  /* EBX push/popped by the original = preserved; the caller's mem_fill len survives */
    case 0x4b360: mem_fill((void *)(uintptr_t)r->eax, r->edx, r->ebx); return; /* mem_fill(dst, val, len) */
    case 0x2ec1a: mark_geom_sentinel_entries(                                      /* geometry segment = the ES selector 0x90be8's base */
                      (uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_map_geometry_buffer));         return;  /*  = g_map_geometry_buffer (same re-point as map_load's) */
    case 0x15b50: G32(VA_g_mouse_button_swap) = 1;                                          return;  /* set_default_mouse_button_swap: mov dword[0x7feb8],1; ret (leaf state-set, boot config) */
    case 0x15b69: add_dirty_rect(r->eax, (int32_t)r->edx, r->ebx, r->ecx); return;  /* add_dirty_rect(left,top,right,bottom) — clean lifted body */
    case 0x1dc73: r->eax = eval_dialogue_record_by_id(r->eax);         return;  /* eval_dialogue_record_by_id(EAX=id) -> EAX — clean lifted body */
    /* detect_vga_display_subtype (host_video_driver, NO lifted body — faithful transcription of
     * 0x103ea over the host int10 service): [0x89f0c]=8; int10 ax=1a00 (get display combination;
     * the host answers AL=0x1a BL=8 "VGA analog color", dpmi.c video_int10); BL=8 -> [0x7674c]=0xffff
     * + $-print 0x700e1 / BL=7 (mono) -> [0x7674c]=0x3f3f + $-print 0x700f4, CLC; anything else STC.
     * The prints go through the 0x100e4 quiet-flag guard, exactly as the original call does. */
    case 0x103ea: {
        G16(VA_g_roth_error_code) = 8;
        regs_t v; memset(&v, 0, sizeof v); v.eax = 0x1a00;
        g_os_soft_int(0x10, &v);
        uint8_t al = (uint8_t)v.eax, bl = (uint8_t)v.ebx;
        if (al == 0x1a && (bl == 7 || bl == 8)) {
            G16(VA_g_player_movement_enabled + 0x2) = (bl == 7) ? 0x3f3f : 0xffff;
            if (G16(VA_g_player_movement_enabled + 0xe) == 0)
                os_dos_print_string((uint32_t)GADDR(bl == 7 ? 0x700f4 : 0x700e1));
            r->eflags &= ~1u;                      /* clc */
        } else
            r->eflags |= 1u;                       /* stc: no VGA */
        return;
    }
#endif

    default: break;
    }
#ifndef ROTH_STANDALONE
    r->va = canon + OBJ_DELTA; call_orig(r);       /* oracle/default: bridge any un-dispatched target */
#else
    roth_unreachable(canon);                      /* image-free: the boot-to-title path must never hit this */
#endif
}

/* raw flat deref of a runtime pointer + byte offset (struct fields off a heap/stored pointer) */
static inline int32_t  ld32(uint32_t p) { return *(volatile int32_t *)(uintptr_t)p; }
static inline uint8_t  ld8 (uint32_t p) { return *(volatile uint8_t  *)(uintptr_t)p; }

/* ============================================================ Layer 1 — leaf helpers */

/* update_frame_time_scale 0x24f5e (83 B) — per-frame timing tick. Spins until the frame-tick delta
 * g_frame_time_scale (0x85324) = counter(0x90bcc) - last(0x85320) is nonzero (the timer ISR advances
 * 0x90bcc), stamps last=counter, and decays the damage-flash level (0x89f3b) by 4*delta (clamp >=0),
 * refreshing the palette via refresh_palette_dac 0x2ff38. Leaf (only bridge = refresh_palette_dac).
 * ORACLE-VERIFIED (test_game_core.c). */
void update_frame_time_scale(void)
{
    int32_t scale;
    do {
        scale = (int32_t)(int16_t)(uint16_t)G16(VA_g_frame_tick_counter) - (int32_t)(int16_t)(uint16_t)G16(VA_g_last_frame_tick);
        G32(VA_g_frame_time_scale) = scale;                     /* mov [0x85324],edx (each spin) */
    } while (scale == 0);                          /* je back to reload (spins on the ISR-driven counter) */
    G16(VA_g_last_frame_tick) = (uint16_t)G16(VA_g_frame_tick_counter);         /* mov ax,[0x90bcc]; mov [0x85320],ax (16-bit) */
    if (G32(VA_g_damage_flash_level) != 0) {                       /* damage flash active */
        int32_t s4 = scale << 2;
        int32_t dmg = G32(VA_g_damage_flash_level) - s4;
        if (dmg < 0) dmg = 0;                      /* jge skip; mov 0 */
        G32(VA_g_damage_flash_level) = dmg;
        regs_t r; memset(&r, 0, sizeof r);
        r.eax = (uint32_t)s4; r.edx = (uint32_t)scale;
        gc_call(&r, 0x2ff38);                      /* refresh_palette_dac(scale*4, scale) */
    }
}

/* tick_ambient_render_and_map 0x10382 (69 B) — advance the ambient render-anim phase counter 0x8a355 by
 * (low byte of 0x85328)>>1, wrapping on byte overflow, forcing it to 1 while the high byte of 0x85328 < 3;
 * then render the main world view (render_world_view_pass &0x89ed0) and, when g_debug_map_enabled 0x7f36e
 * is set, draw the debug map overlay (draw_map_overlay 0x10dce). Both are bridges. */
void tick_ambient_render_and_map(void)
{
    uint16_t w  = (uint16_t)G16(VA_g_frame_time_scale + 0x4);
    uint8_t  al = (uint8_t)((w & 0xff) >> 1);      /* shr al,1 */
    uint8_t  ah = (uint8_t)(w >> 8);
    int cmp_ah = 1;
    if (G8(VA_g_span_blend_mode_flag + 0x1) != 0) {
        uint32_t sum = (uint32_t)(uint8_t)G8(VA_g_span_blend_mode_flag + 0x1) + al;   /* add byte[0x8a355],al */
        G8(VA_g_span_blend_mode_flag + 0x1) = (uint8_t)sum;
        if (sum <= 0xff) cmp_ah = 0;               /* jae (no carry) -> straight to render */
        else G8(VA_g_span_blend_mode_flag + 0x1) = 0;                      /* carry -> wrap to 0, then the cmp-ah block */
    }
    if (cmp_ah) {
        if (ah <= 2) G8(VA_g_span_blend_mode_flag + 0x1) = 1;              /* ja skips; else force to 1 (high byte < 3) */
    }
    /* NB (in-game): live-swapping THIS fn (or the update_player_tick hub above it)
     * per-frame is functionally correct but ran in extreme slow-motion — the whole world render
     * executes inside the lift dispatch, where the game's int-8/int-9 ISRs are frozen
     * (inject_irq bails on g_in_handler; the shm_tick stand-in only bumped 0x90bcc + the ring).
     * That was the documented HOST-lane surrogate gap, NOT a lift bug.
     * HOST FIX: tick_ambient_render_and_map 0x10382 + update_player_tick 0x1729c +
     * run_gameplay_frame 0x1691c are now in lift_is_interactive (lift_registry.c), so shm_tick runs
     * the 70 Hz heartbeat/tick DURING their render dispatch -> full-speed single-swap (no 2x: the
     * shm_tick and real-ISR tick sources are complementary-gated by g_in_handler; the old 2x was the
     * pre-70 Hz-down-sample double-count). Pending in-game confirmation. */
    regs_t r; memset(&r, 0, sizeof r);
    r.eax = (uint32_t)GADDR(VA_g_das_cache_slots + 0x5a0);
    gc_call(&r, 0x287b6);                          /* render_world_view_pass(&0x89ed0) */
    if (G8(VA_g_debug_map_enabled) != 0)
        gc_call(&r, 0x10dce);                      /* draw_map_overlay */
}

/* init_game_databases 0x1dfc2 (231 B) — startup DB load: build_game_path the dbase200/300 DAS filenames,
 * open the dbase400 dialogue script, then open dbase100.dat, heap-alloc it into g_dbase100_base (0x81e1c),
 * resolve its section pointers (header +0x14 -> inventory table 0x81e20, +0x1c -> dialogue table 0x81e24,
 * +0xc -> record-bitmap size 0x81e2c), alloc + zero the record bitmap 0x81e28, and resolve_dbase100_sound_ids.
 * All file-I/O + heap + dbase callees are bridges. NOTE the tail is a flow_succ into the SHARED epilogue
 * 0x1d0f7 (pop 5 regs; ret) borrowed from remove_item — NOT a shared body; the C prologue/epilogue handles
 * the saves, so we do NOT lift remove_item. IN-GAME (needs the host DOS file layer; see §6). */
void init_game_databases(void)
{
    regs_t r; memset(&r, 0, sizeof r);
    G32(VA_g_cutscenes_seen_count) = 1;                              /* g_cutscenes_seen_count = 1 */
    /* build_game_path(eax=dst, edx=dir, ebx=name) x2 */
    r.eax = GADDR(VA_g_dbase200_filename); r.edx = GADDR(VA_g_dir_gdv + 0x50); r.ebx = GADDR(VA_g_heap_free_list + 0x824); gc_call(&r, 0x2fb7f);
    r.eax = GADDR(VA_g_dbase300_filename); r.edx = GADDR(VA_g_dir_gdv + 0xa0); r.ebx = GADDR(VA_g_heap_free_list + 0x831); gc_call(&r, 0x2fb7f);
    gc_call(&r, 0x1e874);                          /* open_dialogue_script() */

    r.eax = GADDR(VA_g_heap_free_list + 0x83e); r.edx = 0; gc_call(&r, 0x41ae5);   /* dos_open_file(name, 0) -> handle */
    r.ecx = r.eax; r.edi = r.eax;                  /* mov ecx,eax; mov edi,eax (handle preserved as edi) */
    if (r.eax == 0) return;                        /* je 0x1e09f: handle==0 -> return g_dbase100_base */

    gc_call(&r, 0x41bc1);                          /* dos_get_file_size(handle) -> size (eax) */
    r.edx = r.eax;                                 /* mov edx,eax */
    gc_call(&r, 0x1517d);                          /* game_heap_alloc(eax=size) -> ptr */
    r.esi = r.eax;                                 /* mov esi,eax */
    G32(VA_g_dbase100_base) = (int32_t)r.eax;                 /* g_dbase100_base = ptr */
    if (r.eax != 0) {
        r.ebx = 1; gc_call(&r, 0x41b53);           /* dos_read_items(ptr, count=1, handle=edi) */
        r.ebx = (uint32_t)G32(VA_g_dbase100_base);            /* mov ebx,[0x81e1c] */
        r.edx = (uint32_t)ld32(r.ebx + 0x14);      /* mov edx,[ebx+0x14] */
        r.eax = r.esi; r.edx -= 4; r.eax += r.edx; /* mov eax,esi; sub edx,4; add eax,edx */
        G32(VA_g_dbase100_inventory_table) = (int32_t)r.eax;             /* g_dbase100_inventory_table = ptr + (*(base+0x14)-4) */
        r.edx = (uint32_t)ld32(r.ebx + 0x1c);      /* mov edx,[ebx+0x1c] */
        r.edx -= 4; r.esi += r.edx;                /* sub edx,4; add esi,edx */
        G32(VA_g_dbase100_dialogue_table) = (int32_t)r.esi;             /* g_dbase100_dialogue_table = ptr + (*(base+0x1c)-4) */
    }
    r.eax = r.edi; gc_call(&r, 0x41b41);           /* dos_close_handle(handle) */
    r.ebx = (uint32_t)G32(VA_g_dbase100_base);                /* mov ebx,[0x81e1c] */
    r.ebx = (uint32_t)ld32(r.ebx + 0xc);           /* mov ebx,[ebx+0xc] (record count) */
    r.ebx += 0x20;                                 /* add ebx,0x20 */
    r.ebx &= 0xffffffe0u;                          /* and bl,0xe0  ==  ebx &= 0xffffffe0 (only low byte changes) */
    r.ebx = (uint32_t)((int32_t)r.ebx >> 3);       /* sar ebx,3 (arithmetic) */
    r.eax = r.ebx; G32(VA_g_dbase100_record_bitmap + 0x4) = (int32_t)r.ebx;  /* mov eax,ebx; mov [0x81e2c],ebx */
    gc_call(&r, 0x1517d);                          /* game_heap_alloc(size) -> bitmap */
    G32(VA_g_dbase100_record_bitmap) = (int32_t)r.eax;                 /* g_dbase100_record_bitmap = bitmap */
    gc_call(&r, 0x1def8);                          /* resolve_dbase100_sound_ids() (leaves ebx = mem_fill len) */
    if (G32(VA_g_dbase100_record_bitmap) != 0) {
        r.eax = (uint32_t)G32(VA_g_dbase100_record_bitmap); r.edx = 0; /* mov eax,[0x81e28]; xor edx,edx */
        gc_call(&r, 0x4b360);                      /* mem_fill(bitmap, 0, ebx-from-resolve) */
    }
    /* return g_dbase100_base in eax — ABI_VOID (caller ignores it; also mirrored in the global) */
}

/* ============================================================ Layer 2 — mid */

/* reset_and_start_new_game 0x1107e (165 B) — full new-game teardown+reload. Copies the DEFAULT map/das arg
 * strings into the working buffers (0x7037c->0x701ec raw-name, 0x7032c->0x7023c das-arg), tears down every
 * subsystem, re-inits the databases + loads the first level, returns 0 ok / -1 on a load failure. pushal/
 * popal-framed -> all regs preserved except eax (the 0/-1 return) => ABI_REGS_EAX. game_play_loop tests the
 * return, so it MUST be reproduced. Every callee is a bridge. IN-GAME. */
int32_t reset_and_start_new_game(const regs_t *in)
{
    (void)in;                                      /* pushal saves/restores all inputs; only eax is the result */
    regs_t r; memset(&r, 0, sizeof r);
    memcpy((void *)(uintptr_t)GADDR(VA_g_cfg_file_arg), (void *)(uintptr_t)GADDR(VA_g_cfg_das2_arg + 0xa0), 0x50);  /* 0x7037c -> 0x701ec */
    memcpy((void *)(uintptr_t)GADDR(VA_g_cfg_das_arg), (void *)(uintptr_t)GADDR(VA_g_cfg_das2_arg + 0x50), 0x50);  /* 0x7032c -> 0x7023c */
    gc_call(&r, 0x21806);                          /* delete_temp_files */
    gc_call(&r, 0x1e0a9);                          /* free_dbase100_data */
    gc_call(&r, 0x3001b);                          /* reset_das_entry_status_table */
    gc_call(&r, 0x4263e);                          /* reset_entity_pools */
    gc_call(&r, 0x2fd6b);                          /* close_das_file_handle */
    gc_call(&r, 0x2f459);                          /* unload_map_geometry */
    gc_call(&r, 0x2f42b);                          /* reset_renderer_tables */
    r.eax = (uint32_t)G32(VA_g_map_das_dir_table_buffer);                /* mov eax,[0x85ce8] */
    G32(VA_g_map_das_dir_table_buffer) = 0;                              /* mov [0x85ce8],0 */
    gc_call(&r, 0x40a2a);                          /* game_free_if_not_null(g_map_das_dir_table_buffer) */
    gc_call(&r, 0x1dfc2);                          /* init_game_databases */
    r.eax = GADDR(VA_g_cfg_das_arg); gc_call(&r, 0x10c32);  /* load_das_file_wrapper(&0x7023c) -> CF */
    if (r.eflags & 1) return -1;                   /* jb 0x1111c -> fail */
    r.eax = GADDR(VA_g_cfg_file_arg); gc_call(&r, 0x10c13);  /* load_raw_file_wrapper(&0x701ec) -> CF */
    if (r.eflags & 1) return -1;
    r.eax = 0; gc_call(&r, 0x33c3e);               /* set_state_record_count(0) */
    gc_call(&r, 0x2f6e6);                          /* init_loaded_map_state */
    gc_call(&r, 0x2ec1a);                          /* mark_geom_sentinel_entries */
    gc_call(&r, 0x2f7bb);                          /* init_movement_tuning_from_first_map */
    gc_call(&r, 0x10d67);                          /* remap_builtin_palette_image */
    gc_call(&r, 0x2f962);                          /* init_render_struct_89ed0 */
    return 0;                                      /* popal; sub eax,eax; ret */
}

/* ============================================================ Layer 3 — per-frame steps */

/* gameplay_frame_step 0x1792c (166 B) — decay damage flash, tick entities + item-pickup lock, then (only
 * in the gameplay/inventory/dead movement modes 1/8/0x20, and not the modal-block 0x7fe24) run the player
 * tick + HUD/UI draw + present (flush dirty rects, flip page, mark overlay). All callees are bridges. IN-GAME. */
void gameplay_frame_step(void)
{
    regs_t r; memset(&r, 0, sizeof r);
    if (G32(VA_g_damage_flash_level) != 0) {                       /* damage-flash decay (== update_frame_time_scale's block) */
        int32_t s4 = G32(VA_g_frame_time_scale) << 2;
        int32_t dmg = G32(VA_g_damage_flash_level) - s4;
        if (dmg < 0) dmg = 0;
        G32(VA_g_damage_flash_level) = dmg;
        r.eax = (uint32_t)s4; gc_call(&r, 0x2ff38);/* refresh_palette_dac */
    }
    gc_call(&r, 0x42d74);                          /* tick_dynamic_entities */
    gc_call(&r, 0x15efe);                          /* tick_item_pickup_lock */
    uint32_t mode = (uint8_t)G8(VA_g_player_movement_enabled);          /* g_player_movement_enabled */
    if (mode != 1 && mode != 8 && mode != 0x20) return;
    if (G32(VA_g_pending_fire_aim + 0x14) != 0) return;                 /* modal-block gate */
    gc_call(&r, 0x1729c);                          /* update_player_tick */
    gc_call(&r, 0x1bcc4);                          /* draw_held_item_icon */
    gc_call(&r, 0x240d7);                          /* draw_active_ui_panels */
    gc_call(&r, 0x1754d);                          /* render_dev_map_selector_ui */
    r.eax = 0; gc_call(&r, 0x1f0e8);               /* render_text_ui(0) */
    if (G32(VA_g_corner_icon_saveunder + 0x4) != 0) {
        r.eax = (uint32_t)G32(VA_g_corner_icon_saveunder + 0x4); gc_call(&r, 0x16831);  /* draw_character_portrait_corner */
        gc_call(&r, 0x15dd9);                      /* flush_dirty_rects */
        gc_call(&r, 0x16807);                      /* restore_corner_peek_icon */
    } else {
        gc_call(&r, 0x15dd9);                      /* flush_dirty_rects */
    }
    r.eax = 3; gc_call(&r, 0x2e1e8);               /* flip_video_page(3) */
    gc_call(&r, 0x1f330);                          /* mark_overlay_dirty_rects (tail) */
}

/* run_gameplay_frame 0x1691c (1117 B) — the in-level per-frame tick: advance the corner-peek timer (0x7fdc0)
 * on the motion counters (0x707b3/0x707b7), resolve the interaction-cursor state machine (0x7e932) from the
 * mouse-button edges (0x7e938/0x7e939/0x7e929), render the world (render_world_view 0x10c8f), fire/examine/
 * activate targets, and commit the cursor shape (set_cursor_shape 0x12a08). Register-machine transcription;
 * goto labels = canon addrs; the 5-way cursor-type switch is the jump table @0x16908. NOTE esi stays 0
 * throughout (only the entry `xor esi,esi`), so every `test esi,esi; jne` falls through. IN-GAME. */
void run_gameplay_frame(void)
{
    regs_t r; memset(&r, 0, sizeof r);
    int32_t ebp_m4 = 0;                            /* [ebp-4] local */

    r.ebx = 0x240; r.esi = 0; r.edx = 0;           /* mov ebx,0x240; xor esi; xor edx */
    if (G32(VA_g_mouse_x) >= 0x20) goto L16965;         /* cmp[0x707b3],0x20; jge (signed) */
    if (G32(VA_g_mouse_y) >= 0x20) goto L16965;
    r.eax = (uint32_t)G32(VA_g_frame_time_scale);
    G32(VA_g_corner_icon_saveunder + 0x4) += (int32_t)r.eax;                /* add[0x7fdc0],eax */
    if (G32(VA_g_corner_icon_saveunder + 0x4) > 0x19) G32(VA_g_corner_icon_saveunder + 0x4) = 0x19;  /* jle skip; clamp */
    r.edx = 1;
    goto L16987;
L16965:
    if (G32(VA_g_corner_icon_saveunder + 0x4) == 0) goto L16987;
    r.eax = (uint32_t)G32(VA_g_frame_time_scale);
    G32(VA_g_corner_icon_saveunder + 0x4) -= (int32_t)r.eax;
    if (G32(VA_g_corner_icon_saveunder + 0x4) > 0) goto L16987;             /* jg (signed) */
    gc_call(&r, 0x167d7);                          /* clear_corner_peek_icon */
L16987:
    if (G32(VA_g_dev_mode_flag + 0x4) != 2) goto L16997;
    r.ebx = 0x268;
    goto L169a5;
L16997:
    if (G32(VA_g_dev_mode_flag + 0x4) != 1) goto L169a5;
    r.ebx = 0x108;
L169a5:
    ebp_m4 = (uint8_t)G8(VA_g_cursor_primary_action_flag);                 /* movzx eax,byte[0x7e938]; mov[ebp-4],eax; mov edi,[ebp-4] */
    r.edi = (uint32_t)ebp_m4;
    r.edi |= (uint8_t)G8(VA_g_cursor_secondary_action_flag);                 /* or edi,eax */
    r.eax = ((uint8_t)G8(VA_g_mouse_buttons_prev)) & 3;            /* mov al,byte[0x7e929]; and al,3; movzx */
    r.eax |= r.edi;                                /* or eax,edi */
    if (r.eax != 0) goto L16a8e;
    G8(VA_g_mouse_relative_mode) = 0;
    G32(VA_g_dev_mode_flag + 0x4) = (int32_t)r.eax;                 /* eax == 0 */
    G32(VA_g_cursor_interaction_flags + 0xc) = (int32_t)r.eax;
    if (r.edx == 0) goto L169f3;                   /* test edx,edx; je */
    G8(VA_g_interaction_cursor_type) = 3;
    r.ebx = 0x248;
    goto L16a74;
L169f3:
    G8(VA_g_interaction_cursor_type) = 0;
    if (G32(VA_g_dialogue_busy_flag) == 0) goto L16a54;
    if (G32(VA_g_move_freeze_gate) >= 0x6ffff) goto L16a2e;      /* signed */
    if (G32(VA_g_dialogue_action_queue_count) != 0) goto L16a2e;
    goto L16a54;
L16a1a:
    r.eax = 0x248;
    goto L16a76;
L16a24:
    r.eax = 0x240;
    goto L16a76;
L16a2e:
    r.edx = (uint32_t)G32(VA_g_mouse_y);
    r.eax = (uint32_t)G32(VA_g_mouse_x);
    gc_call(&r, 0x1f71d);                          /* update_dialogue_choice_highlight -> eax */
    if (r.eax < 1) goto L16a4a;
    if (r.eax <= 1) goto L16a1a;                   /* == 1 */
    if (r.eax == 2) goto L16a24;
L16a4a:
    r.eax = 8;
    goto L16a76;
L16a54:
    gc_call(&r, 0x10c8f);                          /* render_world_view -> eax=hit-record ptr */
    r.edx = (uint32_t)ld8(r.eax + 1);              /* movzx edx,byte[eax+1] */
    if (r.edx == 0) goto L16a6d;
    gc_call(&r, 0x1624d);                          /* classify_cursor_target_object(eax=ptr, edx=byte) -> eax */
L16a66:
    r.ebx = r.eax;                                 /* mov ebx,eax */
    goto L16a74;
L16a6d:
    G8(VA_g_interaction_cursor_type) = 1;
    /* fall through to L16a74 */
L16a74:
    r.eax = r.ebx;                                 /* mov eax,ebx */
L16a76:
    gc_call(&r, 0x12a08);                          /* set_cursor_shape(eax) */
L16a7b:
    G8(VA_g_cursor_primary_action_flag) = 0;                               /* clear the two mouse-button edge flags, then return */
    G8(VA_g_cursor_secondary_action_flag) = 0;
    return;                                        /* jmp 0x15e96 (shared epilogue) */

L16a8e:                                            /* a button edge is pending: massage the cursor type first */
    if (G8(VA_g_cursor_primary_action_flag) == 0) goto L16ab5;
    if (G32(VA_g_cursor_interaction_flags + 0x4) != 0) goto L16ad1;
    if ((uint8_t)G8(VA_g_interaction_cursor_type) != 5) goto L16ad1;
    G8(VA_g_interaction_cursor_type) = 1;
    goto L16ad1;
L16ab5:
    if (G8(VA_g_cursor_secondary_action_flag) == 0) goto L16ad1;
    if ((uint8_t)G8(VA_g_interaction_cursor_type) != 3) goto L16ad1;
    G8(VA_g_interaction_cursor_type) = 4;
L16ad1:
    r.eax = (uint8_t)G8(VA_g_interaction_cursor_type);
    if (r.eax == 1) goto L16b84;
    ebp_m4 = (uint8_t)G8(VA_g_cursor_primary_action_flag);                 /* movzx;mov[ebp-4]; movzx;or eax,[ebp-4] */
    r.eax = (uint8_t)G8(VA_g_cursor_secondary_action_flag) | (uint32_t)ebp_m4;
    if (r.eax == 0) goto L16b84;
    if (G32(VA_g_dialogue_busy_flag) == 0) goto L16b84;
    if (G32(VA_g_move_freeze_gate) != 0x6ffff) goto L16b44;
    r.edx = (uint32_t)G32(VA_g_mouse_y);                /* dialogue-active + freeze full: force-end voice, cursor 0x268 */
    r.eax = (uint32_t)G32(VA_g_mouse_x);
    gc_call(&r, 0x1f671);                          /* dialogue_voice_force_end */
    r.eax = 0x268;
    G8(VA_g_mouse_buttons_prev + 0x7) = 0;
    gc_call(&r, 0x12a08);                          /* set_cursor_shape(0x268) */
    G32(VA_g_dev_mode_flag + 0x4) = 2;
    goto L16a7b;
L16b44:
    if (G32(VA_g_dialogue_action_queue_count) != 0) goto L16b59;
    if (G32(VA_g_move_freeze_gate) < 0x6ffff) goto L16b84;       /* signed */
L16b59:
    r.edx = (uint32_t)G32(VA_g_mouse_y);
    r.eax = (uint32_t)G32(VA_g_mouse_x);
    gc_call(&r, 0x1f671);                          /* dialogue_voice_force_end */
    G8(VA_g_mouse_buttons_prev + 0x7) = 0;
    r.eax = 0x108;
    G32(VA_g_dev_mode_flag + 0x4) = 1;
    goto L16a76;
L16b84:
    if ((uint8_t)G8(VA_g_interaction_cursor_type) != 3) goto L16bb0;
    if (G32(VA_g_cursor_interaction_flags + 0x4) != 0) goto L16bb0;
    if (r.edx != 0) goto L16bb0;                   /* test edx,edx; jne */
    G8(VA_g_interaction_cursor_type) = 1;
L16bb0:
    {
        uint8_t al = (uint8_t)((uint8_t)G8(VA_g_interaction_cursor_type) - 1);   /* dec al */
        if (al > 4) goto L16a74;                   /* ja (unsigned) -> default */
        switch (al) {                              /* jmp [eax*4 + 0x16908] */
        case 0: goto L16cb1;                       /* cursor type 1 */
        case 1: goto L16d6a;                       /* cursor type 2 */
        case 2: goto L16bca;                       /* cursor type 3 */
        case 3: goto L16c71;                       /* cursor type 4 */
        case 4: goto L16c14;                       /* cursor type 5 */
        }
    }
L16bca:                                            /* cursor type 3 */
    r.ebx = 0x270;
    r.edi = (uint8_t)G8(VA_g_cursor_secondary_action_flag);
    r.eax = (uint8_t)G8(VA_g_cursor_primary_action_flag);
    r.eax |= r.edi;
    if (r.eax == 0) goto L16a74;
    if (r.edx != 0) goto L16be9;                   /* test edx,edx; jne */
    goto L16bfa;
L16be9:
    gc_call(&r, 0x1a132);                          /* update_ui_overlay */
L16bee:
    G8(VA_g_mouse_buttons_prev + 0x7) = 0;
    goto L16a74;
L16bfa:
    G32(VA_g_console_input_numeric_only + 0x4) = (int32_t)r.edx;                 /* edx == 0 */
    if (r.esi != 0) goto L16c0b;                   /* esi always 0 -> falls through */
    gc_call(&r, 0x10c8f);                          /* render_world_view -> eax */
    r.ecx = r.eax;
L16c0b:
    r.eax = r.ecx;
    gc_call(&r, 0x10cb3);                          /* examine_object_under_cursor */
    goto L16bee;
L16c14:                                            /* cursor type 5 */
    r.ebx = 0x268;
    if (G8(VA_g_cursor_primary_action_flag) != 0) goto L16bca;
    if (G8(VA_g_cursor_secondary_action_flag) == 0) goto L16a74;
    r.eax = (uint32_t)G32(VA_g_mouse_x);
    r.edx = (uint32_t)G32(VA_g_mouse_y);
    if (r.eax == 0) r.eax = 1;                     /* test;jne;mov 1 */
    if (r.edx == 0) r.edx = 1;
    G32(VA_g_console_input_numeric_only + 0x4) = 0;
    G8(VA_g_mouse_buttons_prev + 0x7) = 0;
    G32(VA_g_dev_mode_flag + 0x8) = 1;
    gc_call(&r, 0x1768a);                          /* trigger_weapon_fire */
    goto L16a74;
L16c71:                                            /* cursor type 4 */
    r.ebx = 0x268;
    if (G8(VA_g_cursor_secondary_action_flag) == 0) goto L16a74;
    if (r.edx != 0) goto L16be9;
    G32(VA_g_console_input_numeric_only + 0x4) = (int32_t)r.edx;                 /* edx == 0 */
    if (r.esi != 0) goto L16c9c;                   /* esi always 0 -> falls through */
    gc_call(&r, 0x10c8f);                          /* render_world_view -> eax */
    r.ecx = r.eax;
L16c9c:
    r.edx = r.ebx;
    r.eax = r.ecx;
    gc_call(&r, 0x164c9);                          /* activate_targeted_object */
    G8(VA_g_mouse_buttons_prev + 0x7) = 0;
    goto L16a66;
L16cb1:                                            /* cursor type 1 */
    if ((G8(VA_g_mouse_buttons_prev) & 1) == 0) goto L16a74;
    r.eax = (uint32_t)G32(VA_g_frame_time_scale);
    G32(VA_g_corner_icon_saveunder + 0xc) += (int32_t)r.eax;
    if (G8(VA_g_mouse_relative_mode) != 0) goto L16cf7;
    G32(VA_g_mouse_dx) = 0;
    G32(VA_g_mouse_dy) = 0;
    G32(VA_g_corner_icon_saveunder + 0xc) = 0;
    G8(VA_g_mouse_relative_mode) = 1;
L16cf7:
    r.ebx = 0x220;
    if ((uint32_t)G32(VA_g_corner_icon_saveunder + 0xc) >= 0xc) goto L16d23; /* jae (unsigned) */
    if ((G8(VA_g_mouse_buttons_prev) & 2) == 0) goto L16d23;
    r.eax = (r.eax & 0xffff0000u) | (uint16_t)G16(VA_g_player_angle);  /* mov ax,[0x90a8a] */
    G8(VA_g_interaction_cursor_type) = 2;
    G16(VA_g_saved_int9_offset + 0xc) = (uint16_t)r.eax;
    goto L16d6a;
L16d23:
    if (G8(VA_g_cursor_secondary_action_flag) == 0) goto L16d5b;
    if (G32(VA_g_corner_icon_saveunder + 0x8) != 0) goto L16a74;
    r.edx = 0; r.eax = 0;
    G32(VA_g_dev_mode_flag + 0x8) = 0;
    gc_call(&r, 0x1768a);                          /* trigger_weapon_fire(0,0) */
    G32(VA_g_corner_icon_saveunder + 0x8) = 1;
    goto L16a74;
L16d5b:
    G32(VA_g_corner_icon_saveunder + 0x8) = 0;
    goto L16a74;
L16d6a:                                            /* cursor type 2 */
    gc_call(&r, 0x12179);                          /* compute_view_offsets_90a74 */
    r.ebx = 0x220;
    goto L16a74;
}

/* ============================================================ Layer 4 — the main gameplay loop */

/* game_play_loop 0x179ee (1462 B) — THE per-frame game loop, driven by roth_main_sequence. Outer do{} =
 * level/restart loop; run_main_menu 0x26628 (when the state selector `ebx`==2) yields a transition code
 * (1=quit -> ret 2; 3=load; 10=new game). Inner do{}while([0x7f360]==0) = the per-frame loop, dispatching
 * on g_player_movement_enabled 0x7674a (1=gameplay, 3=UI/inventory, 4/5=dialogue, 0x20=dead). Threads the
 * FULL register file: the `ebx` state selector + `ecx` restart flag come out of bridged callees (Ghidra
 * drops them). Returns 1 (mid-load) / 2 (quit) in eax; roth_main_sequence ignores it -> ABI_VOID. IN-GAME. */
void game_play_loop(void)
{
    regs_t r; memset(&r, 0, sizeof r);
    int32_t ebp_m4 = 0;                            /* [ebp-4] menu-loop continue flag */

    gc_call(&r, 0x15b50);                          /* set_default_mouse_button_swap */
    r.ebx = (uint32_t)(G32(VA_g_screen_pitch) * G32(VA_g_screen_height));           /* ebx = pitch*height */
    r.eax = (uint32_t)G32(VA_g_framebuffer_ptr);                /* eax = g_framebuffer_ptr (A4 stored ptr) */
    r.edx = 0; r.esi = 0;
    gc_call(&r, 0x4b360);                          /* mem_fill(fb, 0, pitch*height) */
    r.ecx = (uint32_t)G32(VA_g_screen_height);                /* height */
    r.ebx = (uint32_t)G32(VA_g_screen_pitch);                /* pitch */
    r.edx = 0; r.eax = 0; r.ecx--; r.ebx--; r.edi = 0;        /* dec ecx; dec ebx */
    gc_call(&r, 0x15b69);                          /* add_dirty_rect(0,0,pitch-1,height-1) */
    r.eax = 1; G32(VA_g_dialogue_busy_flag + 0x36) = 0x4d2;
    gc_call(&r, 0x1dc73);                          /* eval_dialogue_record_by_id(1) */
    gc_call(&r, 0x1db5e);                          /* finish_dialogue_record_eval */
    r.eax = GADDR(0x1792b); G32(VA_g_dialogue_busy_flag + 0x36) = (int32_t)r.esi;    /* esi==0 */
    gc_call(&r, 0x283a0);                          /* xchg_849a4(&0x1792b) */
    gc_call(&r, 0x17317);                          /* repaint_hud_and_present */
    G8(VA_g_player_movement_enabled) = 8;                               /* g_player_movement_enabled = 8 */
    gc_call(&r, 0x1792c);                          /* gameplay_frame_step */
    gc_call(&r, 0x1c57e);                          /* reset_inventory */
    gc_call(&r, 0x1db5e);                          /* finish_dialogue_record_eval */
    gc_call(&r, 0x1818d);                          /* restore_active_held_item */
    r.eax = GADDR(VA_g_help_overlay_enabled + 0x4); r.edx = (uint32_t)G32(VA_g_selected_item_primary + 0x10); r.ebx = 2;
    gc_call(&r, 0x1e2bd);                          /* build_entity_def_by_id(&0x7fe3c, [0x81054], 2) */
    if (G32(VA_g_help_overlay_enabled + 0xc) == 0) G32(VA_g_help_overlay_enabled + 0xc) = 0x800;
    r.eax = (uint32_t)G32(VA_g_help_overlay_enabled + 0xc);
    G32(VA_g_value_reduction_factor) = 0;
    G32(VA_g_player_health) = (int32_t)r.eax;                 /* g_player_health = [0x7fe44] */
    gc_call(&r, 0x1c96f);                          /* reset_player_locomotion_state */

L17aba:                                            /* --- OUTER LOOP TOP --- */
    G32(VA_g_pending_game_action + 0xc) = 0;
    G8(VA_g_player_movement_enabled) = 8;
    G32(VA_g_inventory_panel_open) = 0;
    gc_call(&r, 0x124dd);                          /* reset_movement_velocity_queues */
    gc_call(&r, 0x179d2);                          /* clear_damage_flash (leaves ebx = the state selector) */
    r.ecx = 0;
    if (r.ebx != 1) r.ecx = 1;                     /* ecx = (ebx != 1) */
    if (r.ebx == 2) goto L17b4a;                   /* ebx==2 -> menu */
    goto L17b72;

L17af5:                                            /* menu code 3 (load): slot = ebx>>16 */
    r.eax = r.ebx; r.eax >>= 0x10;
    gc_call(&r, 0x22129);                          /* load_savegame_file(slot) */
    if (r.eax == 0) goto L17b6c;
    gc_call(&r, 0x1096f);                          /* process_map_warp_or_load */
    if (r.eax != 0) goto L17b6c;
    ebp_m4 = (int32_t)r.eax;                       /* [ebp-4] = 0 */
    r.ecx = 0;
    goto L17b6c;
L17b17:                                            /* menu code 10 (new game) */
    r.ecx = 1;
    ebp_m4 = 0;
    goto L17b6c;
L17b25:                                            /* menu code 1 (quit) */
    gc_call(&r, 0x1823a);                          /* free_active_item_hud_icon */
    gc_call(&r, 0x1f6cc);                          /* dialogue_voice_stop_all */
    r.eax = 2; gc_call(&r, 0x1dc73);               /* eval_dialogue_record_by_id(2) */
    gc_call(&r, 0x1db5e);                          /* finish_dialogue_record_eval */
    G8(VA_g_player_movement_enabled) = 0;
    goto L17f62;                                   /* -> return 2 */
L17b4a:                                            /* ebx==2: run the main menu */
    gc_call(&r, 0x26628);                          /* run_main_menu -> eax = transition code */
    r.ebx = r.eax;
    ebp_m4 = (int32_t)r.eax;
    r.eax &= 0xff;
    if (r.eax >= 3) {                              /* cmp eax,3; jb 0x17b67 */
        if (r.eax <= 3) goto L17af5;               /* jbe (==3) -> load */
        if (r.eax == 0xa) goto L17b17;             /* ==10 -> new game */
        goto L17b6c;
    }
    if (r.eax == 1) goto L17b25;                   /* ==1 -> quit */
L17b6c:
    if (ebp_m4 != 0) goto L17b4a;                  /* loop the menu while [ebp-4]!=0 */
L17b72:
    if (r.ecx == 0) goto L17be1;                   /* test ecx,ecx; je */
    G8(VA_g_render_sector_walk_mode + 0x7) = 1;                               /* ecx!=0: restart / new-game setup */
    gc_call(&r, 0x1c59e);                          /* clear_dual_array_80afc */
    gc_call(&r, 0x1c57e);                          /* reset_inventory */
    r.eax = 3; G32(VA_g_dialogue_busy_flag + 0x36) = 0;
    gc_call(&r, 0x1dc73);                          /* eval_dialogue_record_by_id(3) */
    gc_call(&r, 0x1db5e);                          /* finish_dialogue_record_eval */
    gc_call(&r, 0x1818d);                          /* restore_active_held_item */
    if (G32(VA_g_help_overlay_enabled + 0xc) == 0) G32(VA_g_help_overlay_enabled + 0xc) = 0x800;
    r.eax = (uint32_t)G32(VA_g_help_overlay_enabled + 0xc); r.edx = 0;
    G32(VA_g_player_health) = (int32_t)r.eax;                 /* g_player_health */
    r.eax = 0; G32(VA_g_value_reduction_factor) = 0;
    gc_call(&r, 0x184ab);                          /* activate_weapon_item(0,0) */
    G32(VA_g_dialogue_busy_flag + 0x36) = 0;
    goto L17be6;
L17be1:
    gc_call(&r, 0x1818d);                          /* restore_active_held_item */
L17be6:
    G32(VA_g_active_weapon_ammo_cap + 0x8) = 1;
    G16(VA_g_screen_resolution_index + 0x8) = 0;
    gc_call(&r, 0x17317);                          /* repaint_hud_and_present */
    r.ebx = 0;
    G32(VA_g_held_item_icon_width + 0x4) = 1;
    gc_call(&r, 0x1bb4b);                          /* update_selected_item_icon */
    G8(VA_g_player_movement_enabled) = 1;                               /* g_player_movement_enabled = 1 (gameplay) */

L17c16:                                            /* --- INNER (per-frame) LOOP TOP --- */
    gc_call(&r, 0x156bd);                          /* service_audio_sequence */
    gc_call(&r, 0x1e9b5);                          /* voice_stream_pump */
    r.edi += (uint32_t)G32(VA_g_frame_time_scale);               /* add edi,[0x85324] (frame-time accum) */
    if ((int32_t)r.esi >= 0xf) {                   /* cmp esi,0xf; jge (signed) */
        r.edi = (uint32_t)((int32_t)r.edi >> 4);   /* sar edi,4 (~fps) */
        r.esi = 0;
        G32(VA_g_font_descriptor + 0x2f2) = (int32_t)r.edi;
        if (r.edi == 0) G32(VA_g_font_descriptor + 0x2f2) += 1;         /* jne skip; inc */
        r.edi = 0;
    } else {
        r.esi++;                                   /* inc esi */
    }
    if (G8(VA_g_reloc_base + 0x4) != 0) gc_call(&r, 0x17317);    /* repaint_hud_and_present */
    {
        uint8_t al = (uint8_t)G8(VA_g_player_movement_enabled);         /* g_player_movement_enabled dispatch */
        if (al < 3) {                              /* cmp al,3; jb 0x17d86 */
            if (al == 1) goto L17c7b;              /* (0x17d86: cmp al,1; je) */
            goto L17d8e;
        }
        if (al == 3) goto L17cb1;                  /* jbe 0x17cb1 */
        if (al == 4) goto L17d1a;                  /* cmp al,4; jbe 0x17d1a */
        if (al == 5) goto L17d15;                  /* cmp al,5; je 0x17d15 */
        goto L17d8e;
    }
L17c7b:                                            /* al==1: gameplay */
    if (G32(VA_g_inspect_popup_state + 0x4) != 0) {
        r.edx = GADDR(VA_g_active_weapon_attrs); r.eax = 1;
        G32(VA_g_inspect_popup_state + 0x4) = 0;
        gc_call(&r, 0x24165);                      /* render_weapon_hud(1, &0x811b4) */
    }
    gc_call(&r, 0x1792c);                          /* gameplay_frame_step */
    gc_call(&r, 0x1691c);                          /* run_gameplay_frame */
    gc_call(&r, 0x14525);                          /* keymap_dispatch */
    goto L17d8e;
L17cb1:                                            /* al==3: UI / inventory */
    if (G32(VA_g_inventory_panel_open) == 0) { G8(VA_g_player_movement_enabled) = 1; goto L17d8e; }  /* -> back to gameplay */
    G8(VA_g_mouse_relative_mode) = 0;
    gc_call(&r, 0x24f5e);                          /* update_frame_time_scale */
    gc_call(&r, 0x1661f);                          /* handle_cursor_click */
    gc_call(&r, 0x1a178);                          /* redraw_inventory_cursor_cell */
    gc_call(&r, 0x18bb2);                          /* draw_current_mouse_cursor_sprite */
    r.eax = 0; gc_call(&r, 0x1f0e8);               /* render_text_ui(0) */
    gc_call(&r, 0x15dd9);                          /* flush_dirty_rects */
    r.eax = 3; gc_call(&r, 0x2e1e8);               /* flip_video_page(3) */
    gc_call(&r, 0x1f330);                          /* mark_overlay_dirty_rects */
    gc_call(&r, 0x18cb9);                          /* free_inspect_overlay_image */
    G32(VA_g_console_input_numeric_only + 0x4) = 0;
    gc_call(&r, 0x1a8e5);                          /* update_inventory_screen */
    goto L17d8e;
L17d15:                                            /* al==5: dialogue cursor/click, then fall into 4 */
    gc_call(&r, 0x16585);                          /* update_dialogue_cursor_and_click */
L17d1a:                                            /* al==4 (or fallthrough from 5): dialogue text + keys */
    r.eax = 1;
    G8(VA_g_mouse_relative_mode) = 0;
    gc_call(&r, 0x1f0e8);                          /* render_text_ui(1) */
    gc_call(&r, 0x15dd9);                          /* flush_dirty_rects */
    r.eax = 3; gc_call(&r, 0x2e1e8);               /* flip_video_page(3) */
    gc_call(&r, 0x1f330);                          /* mark_overlay_dirty_rects */
    gc_call(&r, 0x1299a);                          /* input_ring_dequeue -> al */
    {
        uint8_t al = (uint8_t)r.eax;
        if (al < 0x48) {                           /* cmp al,0x48; jb 0x17d80 */
            if (al == 0x1c) {                      /* 0x17d80: cmp al,0x1c; je 0x17d70 */
                if (G32(VA_g_dialogue_busy_flag) != 0) gc_call(&r, 0x173f4);  /* use_enter_key_handler */
            }
            goto L17d8e;
        }
        if (al == 0x48) {                          /* jbe 0x17d50 */
            if (G32(VA_g_dialogue_busy_flag) != 0) gc_call(&r, 0x1fb1e);      /* choice_select_prev */
            goto L17d8e;
        }
        if (al == 0x50) {                          /* cmp al,0x50; je 0x17d60 */
            if (G32(VA_g_dialogue_busy_flag) != 0) gc_call(&r, 0x1fc16);      /* choice_select_next */
        }
        goto L17d8e;
    }
L17d8e:                                            /* --- per-frame tail: pending game actions --- */
    if (G32(VA_g_pending_fire_aim + 0x14) != 0) G8(VA_g_reloc_base + 0x4) += 1;
    if (G32(VA_g_pending_game_action) != 0) {                       /* g_pending_game_action */
        if (G8(VA_g_pending_game_action) & 2) {                     /* test byte[0x7fea8],2 -> load/write branch */
            r.eax = (uint32_t)G32(VA_g_pending_game_action + 0x4);
            if (r.eax >= 1) {                      /* cmp eax,1; jb 0x17df0 */
                if (r.eax <= 1) {                  /* jbe 0x17db5: ==1 -> load */
                    r.eax = (uint32_t)G32(VA_g_pending_game_action + 0x8);
                    gc_call(&r, 0x22129);          /* load_savegame_file([0x7feb0]) */
                    if (r.eax != 0) {
                        gc_call(&r, 0x1096f);      /* process_map_warp_or_load */
                        if (r.eax != 0) goto L17dcc;
                    }
                } else if (r.eax == 2) {           /* ==2 -> write */
                    r.eax = 0;
                    gc_call(&r, 0x21dc6);          /* write_savegame_file(0) */
                }
            }
        }
        if (G8(VA_g_pending_game_action) & 4) {                     /* resolution change */
            r.eax = (uint32_t)G32(VA_g_selected_video_mode);
            gc_call(&r, 0x147e6);                  /* set_resolution_index_and_cycle_display */
        }
        if (G8(VA_g_pending_game_action) & 8) r.ebx = 3;            /* set post-loop action = 3 */
        if (G8(VA_g_pending_game_action) & 1) {                     /* warp request */
            int32_t v = (int32_t)(uint8_t)G8(VA_g_warp_dest_a) + G32(VA_g_map_first_load_flag);
            if (v != 0) {                          /* je 0x17e3c */
                r.eax = (uint32_t)v;
                gc_call(&r, 0x1096f);              /* process_map_warp_or_load */
                if (r.eax != 0) goto L17dcc;
                if (G16(VA_g_geometry_selector) == 0) goto L17dcc;
            }
        }
        G32(VA_g_pending_game_action) = 0;                          /* g_pending_game_action = 0 (0x17e3c) */
    }
    /* 0x17e46 */
    if (G32(VA_g_player_health) <= 0) { G16(VA_g_screen_resolution_index + 0x8) = 1; r.ebx = 2; }   /* player dead -> exit inner loop, action=2 */
    if (G16(VA_g_screen_resolution_index + 0x8) == 0) goto L17c16;            /* inner loop while [0x7f360]==0 */

    /* 0x17e6b — inner loop exited */
    if (G32(VA_g_player_health) <= 0) {                       /* jg 0x17f27 skips the death sequence */
        r.edx = 0; r.eax = 0;
        G8(VA_g_player_movement_enabled) = 0x20;                        /* dead */
        gc_call(&r, 0x184ab);                      /* activate_weapon_item(0,0) */
        gc_call(&r, 0x124dd);                      /* reset_movement_velocity_queues */
        r.eax = 0x220; G16(VA_g_move_speed_accum) = 0;
        gc_call(&r, 0x12a08);                      /* set_cursor_shape(0x220) */
        r.eax = 4; r.ecx = 0;
        gc_call(&r, 0x1dc73);                      /* eval_dialogue_record_by_id(4) */
        gc_call(&r, 0x1db5e);                      /* finish_dialogue_record_eval */
        if (G32(VA_g_pending_game_action + 0xc) == 0 && G32(VA_g_pending_fire_aim + 0x14) != 0)
            gc_call(&r, 0x17317);                  /* repaint_hud_and_present */
        G32(VA_g_pending_fire_aim + 0x14) = 0;
        G8(VA_g_player_movement_enabled) = 0x20;
        if (G32(VA_g_pending_game_action + 0xc) == 0) {
L17ee6:                                            /* death animation loop (<= 300 ms) */
            r.ecx += (uint32_t)G32(VA_g_frame_time_scale);       /* add ecx,[0x85324] */
            gc_call(&r, 0x156bd);                  /* service_audio_sequence */
            gc_call(&r, 0x1792c);                  /* gameplay_frame_step */
            if (G32(VA_g_pending_fire_aim + 0x14) != 0) G8(VA_g_reloc_base + 0x4) += 1;
            gc_call(&r, 0x1299a);                  /* input_ring_dequeue -> al */
            {
                uint8_t al = (uint8_t)r.eax;       /* cmp al,1: al==0 -> continue; al==1 -> exit; 0x39 -> quickload */
                if (al == 1) {
                    r.ecx = 0x12c;                 /* jbe 0x17f9a: mov ecx,0x12c -> exit the death loop */
                } else if (al == 0x39) {           /* F9 (0x17f6c): quickload */
                    gc_call(&r, 0x1f6cc);          /* dialogue_voice_stop_all */
                    if (G32(VA_g_choice_selected_index + 0x1c) != -1) {
                        r.eax = (uint32_t)G32(VA_g_choice_selected_index + 0x1c);
                        gc_call(&r, 0x22129);      /* load_savegame_file([0x7138c]) */
                        if (r.eax != 0) {
                            gc_call(&r, 0x1096f);  /* process_map_warp_or_load */
                            if (r.eax != 0) goto L17dcc;   /* -> return 1 */
                            r.ebx = 1;             /* 0x17f95: ONLY on load+warp success — the original's
                                                    * je 0x17f9a at 0x17f86 (load FAILED) jumps PAST it,
                                                    * leaving ebx>1 so the 0x17f33 `cmp ebx,1; jbe` falls
                                                    * through to reset_and_start_new_game (0x1107e). */
                        }
                    }
                    r.ecx = 0x12c;                 /* 0x17f9a -> exit the death loop */
                }
                /* al==0, or al>1 && al!=0x39: ecx unchanged -> continue if < 300 */
            }
            if (r.ecx < 0x12c) goto L17ee6;        /* 0x17f18: cmp ecx,0x12c; jb (unsigned) */
        }
        G8(VA_g_player_movement_enabled) = 8;
    }
    /* 0x17f27 — reached by fall-through (health>0 skips the death block; death loop also falls here) */
    gc_call(&r, 0x26cd4);                          /* flush_object_das_handles */
    G8(VA_g_player_movement_enabled) = 0;
    if (r.ebx > 1) {                               /* cmp ebx,1; jbe 0x17f46 (unsigned) */
        gc_call(&r, 0x1f6cc);                      /* dialogue_voice_stop_all */
        gc_call(&r, 0x1107e);                      /* reset_and_start_new_game -> eax (0 ok / -1 fail) */
        if (r.eax != 0) goto L17f62;               /* fail -> return 2 */
    }
    if (r.ebx != 0) goto L17aba;                   /* test ebx,ebx; jne -> outer loop top */
    gc_call(&r, 0x179d2);                          /* clear_damage_flash */
    r.eax = 2; gc_call(&r, 0x1dc73);               /* eval_dialogue_record_by_id(2) */
    gc_call(&r, 0x1db5e);                          /* finish_dialogue_record_eval */
L17f62:
    /* eax = 2 (quit) — ABI_VOID (roth_main_sequence ignores the return) */
    return;
L17dcc:
    /* eax = 1 (mid-load transition) — ABI_VOID */
    return;
}

/* ============================================================ Layer 5 — startup sequence */

/* roth_main_sequence 0x100f6 (591 B) — init every subsystem, run the game loop, then tear everything down.
 * The reachability-root boundary fn. Two inline BIOS video calls (int 0x10) go through g_os_soft_int; the
 * MIDI timer-event handler is registered as a far pointer (offset 0x1231b : cs) -> edx low = g_os_game_cs.
 * Threads CF from the several `jb` error gates. Every subsystem callee is a bridge. IN-GAME. */
void roth_main_sequence(void)
{
    regs_t r; memset(&r, 0, sizeof r);
    gc_call(&r, 0x10f6c);                          /* parse_config_ini_paths */
    gc_call(&r, 0x21806);                          /* delete_temp_files */
    gc_call(&r, 0x2fa29);                          /* allocate_das_worker_buffers -> CF */
    if (r.eflags & 1) goto shutdown;               /* jb 0x102ab */
    gc_call(&r, 0x2f42b);                          /* reset_renderer_tables */
    gc_call(&r, 0x267f4);                          /* read_roth_ini */
    if (G8(VA_g_player_movement_enabled + 0x10) == 0) {                        /* cmp byte[0x7675a],0; jne 0x10137 (skip audio init) */
        r.edx = GADDR(VA_g_msg_installing_sound); gc_call(&r, 0x100e4);   /* dos_print_dollar_string */
        gc_call(&r, 0x15813);                      /* sos_audio_init */
        r.edx = GADDR(VA_g_msg_ok); gc_call(&r, 0x100e4);
    }
    gc_call(&r, 0x2fc98);                          /* init_video_surface -> CF */
    if (r.eflags & 1) goto shutdown;               /* jb 0x102ab */
    G16(VA_g_frame_tick_counter + 0x2) = 0;
    r.eax = GADDR(VA_g_cfg_snd_arg); gc_call(&r, 0x10c51);  /* load_sfx_file_wrapper(&g_cfg_snd_arg) */
    gc_call(&r, 0x1602e);                          /* load_icons_all */
    gc_call(&r, 0x2fd21);                          /* init_backdrop_image_surface -> ZF */
    if (r.eflags & 0x40) goto shutdown;            /* je 0x102ab (ZF set) */
    r.edx = GADDR(VA_g_msg_starting_midi); gc_call(&r, 0x100e4);  /* dos_print_dollar_string */
    gc_call(&r, 0x1558d);                          /* process_audio_sequence_chunk */
    r.edx = GADDR(VA_g_msg_initiating_das); gc_call(&r, 0x100e4);
    gc_call(&r, 0x1dfc2);                          /* init_game_databases */
    r.eax = GADDR(VA_g_cfg_das2_arg); gc_call(&r, 0x10c70);  /* load_ademo_das_wrapper(&g_cfg_das2_arg) */
    /* copy the working map/das arg buffers back to the defaults (0x701ec->0x7037c, 0x7023c->0x7032c) */
    memcpy((void *)(uintptr_t)GADDR(VA_g_cfg_das2_arg + 0xa0), (void *)(uintptr_t)GADDR(VA_g_cfg_file_arg), 0x50);
    memcpy((void *)(uintptr_t)GADDR(VA_g_cfg_das2_arg + 0x50), (void *)(uintptr_t)GADDR(VA_g_cfg_das_arg), 0x50);
    r.eax = GADDR(VA_g_cfg_das_arg); gc_call(&r, 0x10c32);  /* load_das_file_wrapper(&g_cfg_das_arg) -> CF */
    if (r.eflags & 1) goto shutdown;               /* jb 0x102ab */
    r.eax = GADDR(VA_g_cfg_file_arg); gc_call(&r, 0x10c13);  /* load_raw_file_wrapper -> CF */
    if (r.eflags & 1) goto shutdown;
    gc_call(&r, 0x2f6e6);                          /* init_loaded_map_state */
    gc_call(&r, 0x2ec1a);                          /* mark_geom_sentinel_entries */
    gc_call(&r, 0x2f7bb);                          /* init_movement_tuning_from_first_map */
    gc_call(&r, 0x10d67);                          /* remap_builtin_palette_image */
    gc_call(&r, 0x2f962);                          /* init_render_struct_89ed0 */
    gc_call(&r, 0x1246a);                          /* install_keyboard_int9 */
    /* register_music_timer_event(eax=0x41231b handler, dx=cs): far pointer (offset:selector) */
    r.eax = GADDR(0x1231b); r.edx = (r.edx & 0xffff0000u) | g_os_game_cs;
    gc_call(&r, 0x159fa);                          /* register_music_timer_event -> eax (0 => install timer) */
    if (r.eax == 0)
        gc_call(&r, 0x12437);                      /* install_timer_int8 */
    /* mov ah,0xf; int 0x10 -> get current video mode; store AX to 0x76756 */
    r.eax = (r.eax & 0xffff00ffu) | 0x0f00u;
    if (g_os_soft_int) g_os_soft_int(0x10, &r);
    G16(VA_g_player_movement_enabled + 0xc) = (uint16_t)r.eax;
    r.edx = GADDR(VA_g_msg_detecting_vesa); gc_call(&r, 0x100e4);  /* dos_print_dollar_string */
    r.eax = 7;                                     /* pick the startup resolution index */
    if (G32(VA_g_video_linear_flag) == 0) r.eax = 2;
    if ((uint8_t)G8(VA_g_rawscreen_flag) == 0xff) {            /* cmp byte[0x90c08],0xff; jne 0x1023d */
        r.eax = 0;
    } else {
        r.edx = (uint32_t)G32(VA_g_selected_video_mode);
        if (r.edx != 0) {                          /* or edx,edx; je 0x10255 */
            r.eax = r.edx;
            if ((uint32_t)r.edx < 0x64) G8(VA_g_vesa_mode_table_built) = 0;    /* cmp edx,0x64; jae skip; mov byte[0x14770],0 */
        }
    }
    gc_call(&r, 0x147f4);                          /* set_resolution_index_and_cycle(eax) */
    G16(VA_g_roth_error_code) = 0;
    gc_call(&r, 0x11cc6);                          /* (surface/backdrop finalize) -> eax */
    /* zero the framebuffer: edi=[0x90a98], eax=0, ecx=[0x85478]>>2, rep stosd */
    {
        volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_framebuffer_ptr);
        uint32_t n = (uint32_t)G32(VA_g_framebuffer_bytes) >> 2;
        for (uint32_t i = 0; i < n; i++) fb[i] = 0;
    }
    gc_call(&r, 0x30114);                          /* init_das_cache_heap */
    gc_call(&r, 0x26965);                          /* load_disk_path_config -> eax */
    r.ebp = 0;                                     /* sub ebp,ebp */
    if (r.eax != (uint32_t)-1) {                   /* cmp eax,-1; je 0x102ab */
        r.eax = 0; gc_call(&r, 0x33c3e);           /* set_state_record_count(0) */
        gc_call(&r, 0x2626f);                      /* apply_audio_volume_settings */
        gc_call(&r, 0x179ee);                      /* game_play_loop */
        gc_call(&r, 0x266ec);                      /* write_roth_ini */
    }
shutdown:                                          /* 0x102ab — teardown chain (always reached) */
    gc_call(&r, 0x1e0a9);                          /* free_dbase100_data */
    gc_call(&r, 0x30149);                          /* release_das_and_geometry_buffers */
    G16(VA_g_player_movement_enabled) = 0;
    gc_call(&r, 0x11ca9);                          /* begin_screen_draw */
    gc_call(&r, 0x12498);                          /* restore_keyboard_int9 */
    gc_call(&r, 0x15a30);                          /* remove_music_timer_event */
    gc_call(&r, 0x124b5);                          /* restore_timer_int8 */
    if (G16(VA_g_player_movement_enabled + 0xe) == 0) {                       /* mov eax,3; int 0x10 -> set text mode */
        r.eax = 3;
        if (g_os_soft_int) g_os_soft_int(0x10, &r);
    }
    if (G16(VA_g_player_movement_enabled + 0xe) == 0) {                        /* build the error-report arg from g_roth_error_code */
        int32_t idx = (int32_t)(uint16_t)G16(VA_g_roth_error_code) * 2;
        r.eax = (uint32_t)((int32_t)(int16_t)ld32(GADDR(VA_g_init_stage_error_strings + 0xf4) + (uint32_t)idx) + 0x71ec6);
    }
    gc_call(&r, 0x3001b);                          /* reset_das_entry_status_table (eax=report arg) */
    r.eax = (uint32_t)G32(VA_g_image_surface); gc_call(&r, 0x40bc7);   /* free_geometry_buffer_and_selector(g_image_surface) */
    gc_call(&r, 0x2fcd4);                          /* shutdown_render_subsystem */
    gc_call(&r, 0x15ac8);                          /* sos_audio_shutdown */
    gc_call(&r, 0x2f459);                          /* unload_map_geometry */
    gc_call(&r, 0x2fd6b);                          /* close_das_file_handle */
    gc_call(&r, 0x2f163);                          /* close_das_handles_and_buffers */
    r.eax = (uint32_t)G32(VA_g_map_das_dir_table_buffer);                /* g_map_das_dir_table_buffer */
    G32(VA_g_map_das_dir_table_buffer) = 0;
    gc_call(&r, 0x40a2a);                          /* game_free_if_not_null */
    gc_call(&r, 0x15ec4);                          /* free_sfx_scratch_buffer */
}

/* roth_game_startup 0x10010 (212 B) — top-level startup: load ROTH.RES, apply the raw-screen / video-mode /
 * blur config (printing '$'-strings for each), init the mouse, and (when 0x7674e is set) build the atan
 * table + detect the VGA subtype, then enter roth_main_sequence. Stores the game DS selector into a code
 * global (0x2ef54) -> g_os_game_ds. IN-GAME. */
void roth_game_startup(void)
{
    regs_t r; memset(&r, 0, sizeof r);
    G16(VA_g_game_ds_selector) = g_os_game_ds;                 /* mov word[0x2ef54],ds */
    G8(VA_g_vesa_mode_table_built) = 2;                               /* mov byte[0x14770],2 */
    G32(VA_g_object_table_header + 0x4) = 0x1000;
    gc_call(&r, 0x10458);                          /* load_roth_res */
    r.edx = GADDR(VA_g_startup_banner); gc_call(&r, 0x100e4);  /* dos_print_dollar_string */
    if (G8(VA_g_rawscreen_flag) != 0) {                        /* raw-screen flag */
        G16(VA_g_video_mode_flags) = 0;
        r.edx = GADDR(VA_g_msg_direct_screen); gc_call(&r, 0x100e4);
    }
    if (G16(VA_g_video_mode_flags) & 3) {                        /* video-mode flags &3 */
        G8(VA_g_hires_line_doubling_flag) = 0;
        r.edx = GADDR(VA_g_msg_overscan_screen); gc_call(&r, 0x100e4);
    }
    if (G16(VA_g_video_mode_flags) & 4) {
        r.edx = GADDR(VA_g_msg_hires_screen); gc_call(&r, 0x100e4);
    }
    if (G16(VA_g_blur_flag) != 0) {                        /* blur flag */
        r.edx = GADDR(VA_g_msg_blur_selected); gc_call(&r, 0x100e4);
    }
    gc_call(&r, 0x103c8);                          /* begin_frame_then_init_mouse */
    if (G16(VA_g_player_movement_enabled + 0x4) != 0) {
        gc_call(&r, 0x3c28c);                      /* thunk_build_atan_table */
        gc_call(&r, 0x103ea);                      /* detect_vga_display_subtype -> CF */
        if (r.eflags & 1) goto report;             /* jb 0x100cf — the ONLY path that skips the print */
        gc_call(&r, 0x100f6);                      /* roth_main_sequence — 0x100c0 FALLS THROUGH to the
                                                    * 0x100c5 print below (no jump in the disasm); the old
                                                    * `goto report` here dropped the exit-time $-print. */
    }
    r.edx = GADDR(VA_g_startup_device_msgs); gc_call(&r, 0x100e4);  /* dos_print_dollar_string (0x100c5, both paths) */
report:
    r.eax = 0; gc_call(&r, 0x41674);               /* restore_exception_handler_and_report(0) */
}

/* ============================================================ Layer 6 — program entry */

/* main 0x15110 (109 B) — the C entry point. Install the exception handler, grab the big game heap
 * (alloc_largest_heap_block 0x25800 -> g_game_heap_handle 0x7f374), abort with a message if < 3 MB
 * (warn if < 4 MB), then install the critical-error handler, run roth_game_startup, restore the handler,
 * and free the heap. Lifted LAST (it IS the program entry). IN-GAME (the game booting = verified). */
void roth_main(void)
{
    regs_t r; memset(&r, 0, sizeof r);
    r.edx = GADDR(0x10345); r.eax = 8;
    gc_call(&r, 0x416d3);                          /* install_exception_handler(8, &LAB_00010345) */
    r.eax = 0x25800; gc_call(&r, 0x35ff9);         /* alloc_largest_heap_block(0x25800) -> handle */
    G32(VA_g_game_heap_handle) = (int32_t)r.eax;                 /* g_game_heap_handle */
    gc_call(&r, 0x35fd9);                          /* block_payload_size(handle) -> size */
    if ((int32_t)r.eax < 0x300000) {               /* < 3 MB: abort */
        r.eax = GADDR(VA_g_heap_free_list + 0x634); gc_call(&r, 0x27c48);   /* dos_print_string("Not enough memory...") */
        goto freeheap;                             /* jmp 0x15171 */
    }
    r.eax = (uint32_t)G32(VA_g_game_heap_handle); gc_call(&r, 0x35fd9);   /* block_payload_size(handle) -> size */
    if ((int32_t)r.eax < 0x400000)                 /* < 4 MB: warn */
        { r.eax = GADDR(VA_g_heap_free_list + 0x655); gc_call(&r, 0x27c48); }   /* dos_print_string("WARNING memory very low") */
    gc_call(&r, 0x436e8);                          /* install_critical_error_handler */
    gc_call(&r, 0x10010);                          /* roth_game_startup */
    gc_call(&r, 0x43775);                          /* restore_critical_error_handler */
freeheap:
    r.eax = (uint32_t)G32(VA_g_game_heap_handle); gc_call(&r, 0x35bfa);   /* free_heap_block(handle) */
}
