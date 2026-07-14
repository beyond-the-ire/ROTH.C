/* os_api.h — workstream C2: the OS surface as a CALLABLE native C API.
 *
 * Phase-2 lifted code reaches the OS by *calling* these, never by trapping an original INT
 * instruction (README scorecard C2 — the gate between "lifted-but-bridged" and "call-closed").
 * First service: the DPMI memory surface (the alloc_largest_heap_block / DPMI-block-cluster
 * close; dos_runtime cluster C).
 *
 * Two link-time bindings, one contract:
 *   - HOST build:   src/platform/os_api.c — thin veneers over the trap-host's existing DPMI
 *                    services (dpmi.c), reused as calls (synthetic cpu_t register frames).
 *   - ORACLE build: recomp/lifted/c2_mock.c — a deterministic, knob-controlled test double; the
 *                    SAME knob state also services the original's patched `int 0x31` sites via a
 *                    SIGTRAP servicer, so orig and lift see identical canned DPMI (the recipe's
 *                    step-4 true differential).
 * The imgfree build links os_api.c once its slices need the OS (none yet).
 */
#ifndef OS_API_H
#define OS_API_H

#include <stdint.h>

/* int31 AX=0500: fill a free-memory info block (0x30 bytes; dword 0 = largest free bytes). */
void os_dpmi_get_free_mem_info(uint8_t *buf);

/* int31 AX=0501: allocate `bytes` of linear memory. Returns 0 + base/handle on success,
 * nonzero on failure (CF). */
int  os_dpmi_alloc_linear(uint32_t bytes, uint32_t *base, uint32_t *handle);

/* int31 AX=0502: free a linear block by handle. (The host leaks these, same as the trap.) */
void os_dpmi_free_linear(uint32_t handle);

/* int31 AX=0100: allocate BX paragraphs of DOS conventional memory. Returns 0 + seg/sel on
 * success; nonzero on failure (CF) with seg/sel UNTOUCHED (the original leaves the caller's
 * registers alone on the fail path — ensure_dos_transfer_buffer relies on that). */
int  os_dpmi_alloc_dos(uint32_t paragraphs, uint32_t *seg, uint32_t *sel);

/* int31 AX=0101: free a DOS-memory block by selector. */
void os_dpmi_free_dos(uint32_t sel);

/* ---- second service round: the exception/critical-error handler surface ----
 * (dos_runtime cluster D tail — install/restore_exception_handler + the INT 24h pair) */

/* int31 AX=0202 BL=vec: get processor-exception handler -> CX:EDX. */
void os_dpmi_get_exc_handler(uint32_t vec, uint16_t *cs, uint32_t *off);

/* int31 AX=0203 BL=vec CX:EDX: set processor-exception handler. Returns nonzero on CF
 * (install_exception_handler's `jb` gates the installed flag on it). */
int  os_dpmi_set_exc_handler(uint32_t vec, uint16_t cs, uint32_t off);

/* int31 AX=0204 BL=vec: get protected-mode interrupt vector -> CX:EDX. */
void os_dpmi_get_pm_vector(uint32_t vec, uint16_t *cs, uint32_t *off);

/* int31 AX=0205 BL=vec CX:EDX: set protected-mode interrupt vector. */
void os_dpmi_set_pm_vector(uint32_t vec, uint16_t cs, uint32_t off);

/* int21 AH=09: print the '$'-terminated string at `edx` (a runtime flat address). */
void os_dos_print_string(uint32_t edx);

/* Real-mode low memory (IVT / BIOS data area). The originals poke it with plain absolute
 * moves (e.g. the RM INT 24h vector dword at 0x90, the BDA byte 0x43e); under the trap host
 * those accesses SIGSEGV into the g_lowmem shadow (traps.c emulate_lowmem) — the host binding
 * reads/writes that same shadow directly, the oracle binding a mock arena. */
uint32_t os_lowmem_read32(uint32_t ea);
void     os_lowmem_write32(uint32_t ea, uint32_t v);
void     os_lowmem_write8(uint32_t ea, uint8_t v);

/* ---- the DOS file service (int21 3c/3d/3e/3f/40/41/42) ----
 * The 7 raw DOS file primitives. The engine's thin file wrappers (dos_open_file 0x41ae5,
 * dos_read_items 0x41b53, dos_write_items 0x41b7a, dos_close_handle 0x41b41, dos_lseek 0x41b9a)
 * are lifted as C over these (lift_dpmi_dos_os.c). `path`/`buf` are flat runtime host addresses
 * (the guest runs flat DS=0), so dos.c derefs them directly — no OBJ_DELTA (gotcha A4). */

/* int21 AH=3d: open `path` (flat addr) with `access` mode (0=RO,1=WO,2=RW). Returns 0 +
 * *handle on success; nonzero on failure (CF) with *handle UNTOUCHED. */
int  os_dos_open(uint32_t path, uint8_t access, uint32_t *handle);

/* int21 AH=3c: create/truncate `path` (flat addr) with CX = `attr`. 0 + *handle on success;
 * nonzero on failure (CF) with *handle UNTOUCHED. */
int  os_dos_create(uint32_t path, uint16_t attr, uint32_t *handle);

/* int21 AH=41: delete `path` (flat addr). Returns nonzero on CF (the create path ignores it). */
int  os_dos_delete(uint32_t path);

/* int21 AH=3f: read `count` bytes from `handle` into `buf` (flat). 0 + *got = bytes read on
 * success; nonzero on failure (CF) with *got UNTOUCHED. */
int  os_dos_read(uint32_t handle, uint32_t buf, uint32_t count, uint32_t *got);

/* int21 AH=40: write `count` bytes from `buf` (flat) to `handle`. 0 + *put = bytes written on
 * success; nonzero on failure (CF) with *put UNTOUCHED. (count==0 is a real call — DOS truncates
 * the file at the current position — so it always issues, *put = 0.) */
int  os_dos_write(uint32_t handle, uint32_t buf, uint32_t count, uint32_t *put);

/* int21 AH=3e: close `handle`. The engine wrappers ignore its result, so this is void. */
void os_dos_close(uint32_t handle);

/* int21 AH=42: seek `handle` to `off` from `whence` (0=SET,1=CUR,2=END). Returns the CF flag
 * (0 = success, nonzero = failure). *pos (may be NULL) receives the raw result register the DOS
 * call leaves in EAX regardless of CF: the 32-bit new position (DX:AX) on success, or the raw DOS
 * error code on failure. The dos_lseek wrapper returns EAX unconditionally (its CF-zero tail is
 * dead code), so a faithful lift returns *pos directly. */
int  os_dos_lseek(uint32_t handle, int32_t off, uint8_t whence, uint32_t *pos);

/* ---- the DPMI selector service (int31 0000/0007/0008/0001) ----
 * The 4 raw LDT-descriptor primitives. The DAS-cache block selector helpers (setup_das_block_selector
 * 0x41191, refresh_das_block_selector_base 0x412ed) are lifted as C over these (lift_dpmi_dos_os.c). */

/* int31 AX=0000: allocate `count` consecutive LDT descriptors. Returns 0 + *sel (the first
 * selector) on success; nonzero on failure (CF) with *sel UNTOUCHED. */
int  os_dpmi_alloc_descriptors(uint16_t count, uint16_t *sel);

/* int31 AX=0007 BX=sel CX:DX=base: set the linear base of `sel`. Returns nonzero on CF. */
int  os_dpmi_set_segment_base(uint16_t sel, uint32_t base);

/* int31 AX=0008 BX=sel CX:DX=limit: set the byte limit of `sel`. Returns nonzero on CF. */
int  os_dpmi_set_segment_limit(uint16_t sel, uint32_t limit);

/* int31 AX=0001 BX=sel: free a single LDT descriptor. (setup_das_block_selector frees the
 * descriptor it just allocated when a set-base/set-limit leg fails — 0x411d8: `mov ax,1; int 0x31`
 * with BX still holding the selector.) */
void os_dpmi_free_descriptor(uint16_t sel);

/* ============================ the audio / HMI-SOS driver service ============================
 * Replaces the au_bridge/call_orig seam into the original SOS library (host_audio_driver, 0x44xxx-
 * 0x55xxx) with a callable host C surface over the M4 virtual driver (src/platform/audio.c). Each call is
 * ONE SOS-driver function the lifted client (lift_audio.c / lift_gdv_cutscene.c) reaches directly. The
 * ~20 transitively-reached plumbing fns (DPMI real-mode thunks, PIT `out`, buffer locks, the dispatch-
 * computer) have NO API — they evaporate when the SOS layer becomes host C over the virtual driver.
 * Returns are the EAX the client reads; `sel`/`ds` args are the
 * live selectors the original passed (cur_ds()/cur_cs()).
 *
 * Two link-time bindings, one contract:
 *   - INTERIM host build:  recomp/lifted/audio_c2_bridge.c — each os_audio_* rebuilds the regs_t frame
 *                          the current au_bridge site builds and call_origs the original SOS VA (zero
 *                          in-game behaviour change). Cannot link image-free (call_orig undefined there).
 *   - ORACLE build:        recomp/lifted/c2_mock.c — canned-return doubles reproducing test_audio.c's
 *                          stub_driver() semantics (the 6 voice VAs = ST_RET EAX-passthrough) + per-call
 *                          arg logs, so the client re-point differential is byte-identical.
 *   - FINAL host + imgfree: os_audio.c — host-C over audio.c's haudio_* services; retires
 *                          audio_c2_bridge.c and makes the imgfree audio slice real.
 *
 * EVERY ABI below is transcribed from the au_bridge/au_bridge_stk call site AND disasm-confirmed at the
 * SOS-function entry. Corpus gotcha:
 * decompiles drop non-EAX/CF returns — but all these client wrappers consume only EAX (`uint32_t r =
 * au_bridge(...)`), so the API returns the callee EAX and nothing else. */

/* ---- MIDI event / channel volume (host MIDI-event ring + channel-volume state) ---- */
/* 0x44e0d sos_dispatch_midi_event: submit the `msg` (flat, selector `sel`) parsed MIDI message for
 * sequencer `seq`, device `dev`. Entry stores EAX=seq/CX=sel/EBX=msg/EDX=dev (disasm 0x44e18..0x44e22);
 * the sole client site (emit_audio_sequence_event 0x4627d) passes the 3-byte controller message length
 * as ONE stack dword (=3), which the bridge supplies. Returns the driver EAX. */
uint32_t os_audio_midi_dispatch(uint32_t seq, uint32_t dev, uint32_t msg, uint16_t sel);
/* 0x4594d sos_midi_all_notes_off: per-sequencer all-notes-off (EAX=seq only; EDX push is callee-save,
 * disasm 0x4594d..0x4595b). */
uint32_t os_audio_midi_all_notes_off(uint32_t seq);
/* 0x45dc5 sos_midi_restore_channel_volumes: EAX=ctx, orig EDX=1 (site au_bridge(0x45dc5,ctx,1,0,0)). */
uint32_t os_audio_midi_restore_volumes(uint32_t ctx);
/* 0x45f1d sos_midi_mute_channel_volumes: EAX=ctx, orig EDX=0. */
uint32_t os_audio_midi_mute_volumes(uint32_t ctx);
/* RETIRED (staged) image-free host-C bodies for the MIDI-router cluster — defined in
 * os_audio.c. Each of the four originals far-calls the per-device SOS MIDI vtable method 0
 * ([dev*0x48+0x92f9c] = MAGIC_MIDI+0 under the host); the natives replace that far-call with
 * haudio_dispatch_simple (reads the live vtable entry, routes to haudio_midi_send = the synth ring
 * when it is a MAGIC_MIDI handler) — the driver_dispatch_simple precedent. midi_dispatch: the two
 * [0x951cc] arms transcribed 1:1 (alloc-mode==0 direct-passthrough is the r1-observed arm; the
 * alloc!=0 dynamic virtual-channel allocation arm is faithful but UNOBSERVED). all_notes_off /
 * restore / mute manage the channel-volume state (0x93104/0x93124/0x970f4/0x7372c) and call the
 * router internally. STAGED ONLY: native under ROTH_AU_AB=native (or AB_VA=<va>); NOT in
 * au_va_retired_default — a paired A/B gates any default flip. */
uint32_t os_audio_midi_dispatch_native(uint32_t seq, uint32_t dev, uint32_t msg, uint16_t sel);
uint32_t os_audio_midi_all_notes_off_native(uint32_t seq);
uint32_t os_audio_midi_restore_volumes_native(uint32_t ctx);
uint32_t os_audio_midi_mute_volumes_native(uint32_t ctx);

/* ---- digital driver open / callback / detect (audio_trap fn0xa / fn2 / fn8) ---- */
/* 0x47dae sos_driver_open_voices: EAX=desc, EDX=0, EBX=req_ptr, CX=sel, + FOUR stack dwords forming two
 * {off,sel} far pairs the entry `lgs`es (disasm 0x47dc4 `lgs eax,[ebp+0x14]`). Site (0x421xx) passes
 * req_ptr=&0x7f47c, size_ptr=&0x7f48c, handle_out=&0x7f4dc, all with sel=cur_ds. Returns the driver
 * handle. (Design draft's 4-arg form could not carry both far pairs — 5 args finalized from the disasm.) */
uint32_t os_audio_open_voices(uint32_t desc, uint32_t req_ptr, uint32_t size_ptr,
                              uint32_t handle_out, uint16_t sel);
/* RETIRED (staged) image-free host-C body — defined in os_audio.c. Transcribes
 * the game-store half of open_voices 1:1 (slot scan, the 0x4fd30 fan-out into 0x97440, the
 * [h*4+0x9740c] guard, the per-slot bookkeeping + descriptor write-backs, handle_out) over the NEW
 * haudio_open_voices_service, which supplies the runtime host-determined values the veneer's DPMI
 * allocs + fn-0xa produce (the far-args selector + base, cb {MAGIC_POLL, CS}, voices/pos offsets,
 * moot DMA/streaming descriptors). STAGED ONLY: native under ROTH_AU_AB=native (or AB_VA 0x47dae) —
 * NOT in au_va_retired_default; the poll-cb the still-original master-timer ISR far-calls is the
 * unprovable-without-in-game dependency (a paired run gates any default flip). */
uint32_t os_audio_open_voices_native(uint32_t desc, uint32_t req_ptr, uint32_t size_ptr,
                                     uint32_t handle_out, uint16_t sel);
/* 0x48666 sos_driver_close_voices: EAX=handle, EDX=a1, EBX=a2 (site passes a1=1,a2=1). */
uint32_t os_audio_close_voices(uint32_t handle, uint32_t a1, uint32_t a2);
/* 0x47cf5 sos_enable_audio_callback: EAX=svc (flat), DX=sel (disasm 0x47d01 `mov [ebp-8],dx`). */
uint32_t os_audio_enable_callback(uint32_t svc, uint16_t sel);
/* 0x47d6e sos_disable_audio_callback: no args (site au_bridge(0x47d6e,0,0,0,0)). */
uint32_t os_audio_disable_callback(void);
/* RETIRED (staged) image-free host-C bodies for enable/disable_callback + close_voices —
 * defined in os_audio.c. The DPMI lock/unlock + driver/DPMI teardown far-calls are moot under
 * the host (dpmi.c 0x600/0x601/0x502 no-ops leaving CF clear; the host mixer stops a voice on its
 * active bit, not via close's frees), so the bodies transcribe only the client-observable game-memory
 * stores: enable -> [0x97b30]=1 + strcpy the service descriptor to [0x7420c] (or clear it); disable ->
 * [0x97b30]=0; close -> zero the per-slot bookkeeping (0x972a4/c4/97420/9730c/9732c/97824/97374 +
 * conditional 0x97b1c) + ret 0/1. The bridge dispatches to these ONLY under ROTH_AU_AB=native (pending
 * class; veneer default) until the A/B sign-off. (open_voices 0x47dae STAYS the
 * call_orig veneer — STOPPED: it needs a host driver-open service that allocates the far-args
 * segment + registers the poll cb the live master-timer ISR consumes; see audio_c2_bridge.c.) */
uint32_t os_audio_enable_callback_native(uint32_t svc, uint16_t sel);
uint32_t os_audio_disable_callback_native(void);
uint32_t os_audio_close_voices_native(uint32_t handle, uint32_t a1, uint32_t a2);
/* 0x48b21 sos_load_detection_driver: EAX=path (flat), DX=sel (disasm 0x48b2e `mov [ebp-4],dx`). */
uint32_t os_audio_load_detection_driver(uint32_t path, uint16_t sel);
/* 0x48c6b sos_unload_detection_driver: no args. */
uint32_t os_audio_unload_detection_driver(void);
/* 0x48f79 sos_find_driver_for_device: EAX=dev, EDX=0, EBX=out (flat), CX=sel (disasm 0x48f85..0x48f8f). */
uint32_t os_audio_find_driver_for_device(uint32_t dev, uint32_t out, uint16_t sel);
/* RETIRED (staged) image-free host-C bodies for the detection trio — defined in
 * os_audio.c. The dispatch-computer (0x4fcd3) is virtualized (audio.c returns MAGIC_OFF; the real
 * .386 never runs), so the HMIDET.386 file I/O + DPMI plumbing are moot; the bodies reproduce the
 * client-observable effect only: load/unload the 0x97aec loaded-flag (+ ret 0), find stages the SB16
 * descriptor into `out` via haudio_stage_driver_descriptor (+ ret 0). The bridge dispatches to these
 * ONLY under ROTH_AU_AB=native (pending class; veneer default) until the A/B sign-off. */
uint32_t os_audio_load_detection_driver_native(uint32_t path, uint16_t sel);
uint32_t os_audio_unload_detection_driver_native(void);
uint32_t os_audio_find_driver_for_device_native(uint32_t dev, uint32_t out, uint16_t sel);

/* ---- HMI timer service (host SIGALRM tick + software event table 0x979c4/0x979c8/0x97a24) ---- */
/* 0x49923 sos_timer_register_event: entry stores EAX=rate, CX=cb_sel, EBX=cb_off + TWO stack dwords =
 * the {off,sel} handle-out far pair (disasm 0x4992f..0x49936; NOTE EDX is push-saved, NOT read — the 3
 * client sites vary EDX freely, so it is dropped from the API). Multi-use: music-tick (cb=0x51ad5,
 * cs sel), user cb, voice-service (0x49eaf). Returns the event index / error. */
uint32_t os_audio_timer_register_event(uint32_t rate, uint32_t cb_off, uint16_t cb_sel,
                                       uint32_t handle_out, uint16_t out_sel);
/* 0x49ca4 sos_timer_remove_event: EAX=event handle. */
uint32_t os_audio_timer_remove_event(uint32_t event);
/* 0x4980d sos_configure_timer_rate: EAX=rate, EDX=flags (site passes rate=0xff00, flags=0). */
uint32_t os_audio_configure_timer_rate(uint32_t rate, uint32_t flags);
/* 0x498e9 sos_stop_timer_service: no meaningful args (EAX push-saved; site passes 0). */
uint32_t os_audio_stop_timer_service(void);
/* Image-free host-C natives for the four timer VAs. STAGED ONLY (reached
 * via ROTH_AU_AB=native / AB_VA=<va>; NOT in au_va_retired_default). Each brackets its SOS
 * timer-event-table edit with au_timer_lock()/au_timer_unlock() (the host fence for the live SOS
 * master-timer ISR 0x49eaf). See os_audio.c. */
uint32_t os_audio_timer_register_event_native(uint32_t rate, uint32_t cb_off, uint16_t cb_sel,
                                              uint32_t handle_out, uint16_t out_sel);
uint32_t os_audio_timer_remove_event_native(uint32_t event);
uint32_t os_audio_configure_timer_rate_native(uint32_t rate, uint32_t flags);
uint32_t os_audio_stop_timer_service_native(void);

/* ---- digital voice start (host virtual driver; ST_RET passthrough in the oracle) ---- */
/* 0x4a641 sos_voice_start: EAX=handle, CX=ds, EBX=voice (disasm 0x4a64d..0x4a654; EDX push-saved). Site
 * (sos_submit_voice) sets EAX==EDX==handle, so the bridge mirrors that. voice_start allocates a
 * voice slot (returns 0..6) into the 0x97440 far-ptr table that open_voices (0x47dae) populated; it is
 * retired-native pure game code (the voice-field leaves ride that same populated table). The oracle mock returns the
 * input handle (test_audio.c stub_driver: 0x4a641 = ST_RET). */
uint32_t os_audio_voice_start(uint32_t handle, uint32_t voice, uint16_t ds);
/* 0x4ad03 sos_voice_load_to_slot: EAX=handle, EDX, EBX=queued voice-struct ptr, CX=ds (disasm
 * 0x4ad0e..0x4ad18). Host-replaced like voice_start; the `cmp [h*4+0x9740c],0xe106; jb` guard skips the
 * `lgs` on an empty slot, so it is a safe c2 call (NOT a permanent leaf). Oracle: ST_RET passthrough. */
uint32_t os_audio_voice_load_to_slot(uint32_t handle, uint32_t edx, uint32_t queued, uint16_t ds);
/* RETIRED (staged) image-free host-C bodies for 0x4a641 / 0x4ad03 — defined in
 * os_audio.c as a 1:1 disasm transcription of the call-free far-ptr field copy (the free-slot
 * scan / direct-slot populate; no host service to route to). The bridge dispatches to these under
 * ROTH_AU_AB=native; the call_orig veneer stays default until the in-game A/B over the
 * new voice-struct snapshot window signs off. voice_start's A/B rides the 90-call trace evidence;
 * voice_load_to_slot's is PENDING coverage (0 traced calls so far). */
uint32_t os_audio_voice_start_native(uint32_t handle, uint32_t voice, uint16_t ds);
uint32_t os_audio_voice_load_to_slot_native(uint32_t handle, uint32_t edx, uint32_t queued, uint16_t ds);

/* ---- SOS driver-slot management (directly bridged from lift_audio.c; all dispatch through the driver
 * vtable @[slot*0x48 + 0x92f9c..0x92fb4]).
 * class (b) -> c2 calls; the host binding delegates to audio.c's driver services. ---- */
/* 0x44553 sos_alloc_driver_slot: EAX=device, EDX=0, EBX=drv_ptr, CX=sel + FOUR stack dwords = two
 * {off,sel} far pairs (info_ptr, slot_out) the entry `lgs`es (disasm 0x44569 `lgs eax,[ebp+0x14]`).
 * Site (sos_load_driver 0x159??) passes drv_ptr=&0x7f3a0, info_ptr=&0x7f37c, slot_out=&0x7f3b0. */
uint32_t os_audio_alloc_driver_slot(uint32_t device, uint32_t drv_ptr, uint32_t info_ptr,
                                    uint32_t slot_out, uint16_t sel);
/* 0x44a81 sos_free_driver_slot: EAX=slot, EDX=flag (site au_bridge(0x44a81,slot,1,0,0)); early-outs EAX=1
 * when [slot*4+0x920b4]==0 (disasm 0x44a94..0x44aa3), else dispatches [slot*0x48+0x92fa8]. */
uint32_t os_audio_free_driver_slot(uint32_t slot, uint32_t flag);
/* 0x44cad sos_driver_call_m4: EAX=slot, EDX=cmd, EBX=param, CX=sel; dispatches [slot*0x48+0x92fb4] with
 * {ebx,cx,slot,cmd} pushed (disasm 0x44cc5..0x44cd7). Returns the vtable-call EAX. */
uint32_t os_audio_driver_call_m4(uint32_t slot, uint32_t cmd, uint32_t param, uint16_t sel);
/* 0x45d28 sos_driver_dispatch_simple: EAX=slot, EDX=cmd, EBX=param, CX=sel; dispatches [slot*0x48+
 * 0x92f9c] then ALWAYS returns 0 (disasm 0x45d5b `mov [ebp-0x14],0`). */
uint32_t os_audio_driver_dispatch_simple(uint32_t slot, uint32_t cmd, uint32_t param, uint16_t sel);
/* RETIRED (staged) image-free host-C body for 0x45d28 — defined in os_audio.c over
 * haudio_dispatch_simple. The bridge above dispatches to it under ROTH_AU_AB=native; the veneer
 * stays default until the SFX re-test flips it. */
uint32_t os_audio_driver_dispatch_simple_native(uint32_t slot, uint32_t cmd, uint32_t param,
                                                uint16_t sel);
/* RETIRED (staged) image-free host-C bodies for the driver-slot install/teardown pair — defined in
 * os_audio.c over the haudio_driver_install_service (host half: descriptor alloc-once + MAGIC vtable
 * stamp) + haudio_dispatch_method (the live-vtable method-1/2 far-calls). The bridge dispatches to these
 * under ROTH_AU_AB=native / AB_VA=0x44553,0x44a81; the veneer stays default until the flip. */
uint32_t os_audio_alloc_driver_slot_native(uint32_t device, uint32_t drv_ptr, uint32_t info_ptr,
                                           uint32_t slot_out, uint16_t sel);
uint32_t os_audio_free_driver_slot_native(uint32_t slot, uint32_t flag);

/* ---- GDV cutscene audio (host streaming + fn0xa; reached from lift_gdv_cutscene.c io.va seams) ----
 * (ABIs finalized from the io.eax/io.edx call sites — most take NO GP args and drive off the
 * audio globals.) */
/* 0x4e066 gdv_audio_begin_playback: no args — reads globals 0x91dc8/0x91d50/0x91cb0/0x91d30, then
 * jmp gdv_audio_start_voice. RETIRED (staged) to the image-free host-C body below,
 * SUPERSEDING the earlier STOP: `call [0x91898]` is a LIVE hook holding exactly 3 game-code values
 * (`roth_disasm.py refs 0x91898 write`) — CANON(0x4e45f) a bare `ret` no-op, CANON(0x4e460) the 16-bit
 * DPCM mix decoder, CANON(0x4e519) the 8-bit decoder — all PURE game-code memory ops (no MAGIC/DPMI), so
 * the native reads GP32(0x91898) live and dispatches to a faithful C re-lift of each. The tail jmp
 * gdv_audio_start_voice (0x55670) is the SOLE-caller continuation (`callsto 0x55670` = 1) and is inlined,
 * composing its only sub-call (voice_start 0x4a641) through os_audio_voice_start. STAGED ONLY (NOT in
 * au_va_retired_default); the ROTH_LIFT=gdv paired run gates any default flip. */
uint32_t os_audio_gdv_begin_playback(void);
uint32_t os_audio_gdv_begin_playback_native(void);
/* 0x4e4f1 gdv_audio_init_silence: no args — fills the silence buffer from 0x91d2c/0x91d38 (all-zero site). */
uint32_t os_audio_gdv_init_silence(void);
/* 0x552f0 gdv_audio_detect_driver: EAX=dev (=[g_gdv_context+4]), EDX=drvid (=word[0x97be4]); the entry
 * `mov esi,edx` consumes EDX (site sets io.eax/io.edx). */
uint32_t os_audio_gdv_detect_driver(uint32_t dev, uint32_t drvid);
/* 0x55360 gdv_audio_load_drivers: no args — derives EDX=ds internally (`mov edx,ds`); all-zero site.
 * STOPPED (stays the call_orig veneer): the descriptor block it produces is NOT reproducible
 * image-free. The helpers 0x4910f/0x49587 read the loaded driver-image records off DAT_00097b00 (the
 * detection-driver fd) — but under NATIVE detection (0x48b21) no file is opened, so there is no fd — and
 * the device fields [0x97b70/74/78/7c] come from 0x49587's fn=1 dispatch far-call (0x4fe8a): under the
 * virtual driver that lands at MAGIC_OFF fn=1 = audio_trap's `default` (R_EAX=0, ECX/EDX untouched), so
 * 0x728d4=0 and 0x728dc/d8/e0 = register leftovers — 0x49587's tail even returns error 0x11 unless
 * 0x728dc lands in [2,0xf]. 0x97be4 (the id setup_voices reads) is NOT written by this path at all; it is
 * a boot-detected value the GDV path reuses. In practice the client takes the [ctx+0x30]!=0 detect branch
 * (descriptor pre-cached at boot), so this path is rarely reached — hence r1 saw 0 calls. Keeping the
 * veneer is the correct "not image-free yet" signal; a native would invent values. */
uint32_t os_audio_gdv_load_drivers(void);
/* 0x553b0 gdv_audio_shutdown: no args (reads 0x91dc2 flag bits + 0x97cd4/0x97cdc/0x97ce0). */
uint32_t os_audio_gdv_shutdown(void);
/* 0x55440 gdv_audio_setup_voices: EAX=dev (=[g_gdv_context+4]), EDX (=[0x91d30]), EBX (=0x5622 const),
 * ECX (=0x3c) — all four consumed (disasm 0x55446..0x5544c; site sets io.eax/edx/ebx/ecx). */
uint32_t os_audio_gdv_setup_voices(uint32_t dev, uint32_t edx, uint32_t ebx, uint32_t ecx);
/* RETIRED (staged) image-free host-C body — defined in os_audio.c. The GDV driver-open
 * orchestrator: it is pure COMPOSITION over the c2 dispatch API (the KEY COMPOSITION RULE) plus the
 * disasm's own game-memory stores (0x91dc2 flag mask, 0x91d10 error, 0x91dc8/0x91df0/0x97cd0/0x97cdc/
 * 0x97b6c/0x9187c) and a stack request descriptor built from the 0x97cec template. Its sub-calls route
 * BACK through the os_audio_* dispatchers so the per-VA au_ab_va precedence holds: enable_callback
 * (0x47cf5) + open_voices (0x47dae) are RETIRED natives reached via os_audio_enable_callback /
 * os_audio_open_voices; configure_timer_rate (0x4980d) + the two timer registrations (0x49923) STAY on
 * their call_orig veneers via os_audio_configure_timer_rate / os_audio_timer_register_event (the timer
 * cluster retires LAST). STAGED ONLY: native under ROTH_AU_AB=native (or AB_VA=0x55440); NOT in
 * au_va_retired_default — the ROTH_LIFT=gdv paired run gates any default flip. */
uint32_t os_audio_gdv_setup_voices_native(uint32_t dev, uint32_t edx, uint32_t ebx, uint32_t ecx);
/* 0x55640 gdv_audio_stop_voice: no args — reads 0x91dc2/0x97cdc/0x97ce0 (all-zero site). */
uint32_t os_audio_gdv_stop_voice(void);

/* ---- the 4 voice-field leaves — PURE GAME CODE, RETIRED to image-free host-C.
 * The "permanent host-boundary" framing was FALSE (it conflated 0x97440 with 0x9740c). Each leaf is a
 * straight-line far-ptr field op (zero call/int) on the per-voice struct at
 * `far[bank*0xc0 + voice*6 + 0x97440]`. That far-ptr table is populated at driver-open by open_voices
 * (0x47dae; r1 boot write-set seq 5 fills 0x9740c and 0x97440.. with a live selector 0x005f),
 * INDEPENDENT of voice_start — so the leaves resolve a VALID pointer and read/write real voice structs
 * (r1: 5,756 calls, non-zero chained returns). There is no #GP / null-SOS path. Like voice_start they
 * retire to a 1:1 disasm transcription in os_audio.c (the runtime-base voice-struct window
 * already captures their write-set). The bridge (os_audio_voice_field) dispatches per op: get_w34 +
 * both xchg ops are RETIRED-DEFAULT (native default; escape hatch ROTH_AU_AB=veneer), deactivate is
 * pending-class (native only under ROTH_AU_AB=native) until the one "voice stops" A/B action.
 * The ORACLE still links the c2_mock (test_audio.c stubs; the differential runs against the mock).
 * op selects the leaf; only bank(handle)+voice are read by 0x4ac55/0x4a54a, EBX=val also by 0x49fe9/0x4a28c. */
uint32_t os_audio_voice_field(uint32_t op, uint32_t handle, uint32_t voice, uint32_t val);
/* op: 0=deactivate(0x4ac55) 1=xchg_w32(0x49fe9) 2=xchg_w54(0x4a28c) 3=get_w34(0x4a54a). */
/* RETIRED (staged) image-free host-C bodies for the 4 leaves — defined in os_audio.c as a
 * 1:1 disasm transcription of the far-ptr field ops (bank=EAX handle, voice=EDX, val=EBX). */
uint32_t os_audio_voice_get_w34_native(uint32_t bank, uint32_t voice);
uint32_t os_audio_voice_xchg_w32_native(uint32_t bank, uint32_t voice, uint32_t val);
uint32_t os_audio_voice_xchg_w54_native(uint32_t bank, uint32_t voice, uint32_t val);
uint32_t os_audio_voice_deactivate_native(uint32_t bank, uint32_t voice);

#endif /* OS_API_H */
