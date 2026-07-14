/* os_audio_standalone.c — the ALWAYS-NATIVE dispatcher shim, the standalone analogue of c2_mock.c's dispatchers. audio_c2_bridge.c cannot be in any
 * image-free closure (its `au_ab_va(va) ? native : call_orig` keeps call_orig retained); the oracle's
 * c2_mock defines the dispatchers as canned doubles, not the natives. So the slice binds each os_audio_*
 * dispatcher a native REACHES straight to its _native body — no au_ab_va, no au_trace, no call_orig.
 *
 * FULL retired-native set (this file supersedes the single-dispatcher version): every os_audio_* the
 * retired natives in os_audio.c reach is bound to its _native body
 * here so a wider image-free closure (the entity think-tier that plays sound, the loaders) stops
 * being blocked on audio dispatchers. gc-sections drops the ones a given slice does not reach, so the
 * existing slices are unaffected (cand_gdvcodec still keeps only voice_start; cand_autimer links
 * none of these). The dispatchers with NO retired _native — os_audio_driver_call_m4 (0x44cad, never
 * observed) and os_audio_gdv_load_drivers (0x55360, parked link-time decision) — are DELIBERATELY absent:
 * a closure that reaches them must fail the image-free link (the honest "audio not fully image-free"
 * signal — the audio residual). The pure host-C dispatchers (gdv_init_silence / detect_driver /
 * stop_voice / shutdown) are defined in os_audio.c itself (no call_orig) — not shimmed here.
 */
#include "os_api.h"
#include "audio.h"   /* sfx_trace_voice_start: SFX-dropout trace kit voice-START observability (v2) */

/* ---- MIDI router cluster (natives: 0x44e0d / 0x4594d / 0x45dc5 / 0x45f1d) ---- */
uint32_t os_audio_midi_dispatch(uint32_t seq, uint32_t dev, uint32_t msg, uint16_t sel)
{ return os_audio_midi_dispatch_native(seq, dev, msg, sel); }
uint32_t os_audio_midi_all_notes_off(uint32_t seq)
{ return os_audio_midi_all_notes_off_native(seq); }
uint32_t os_audio_midi_restore_volumes(uint32_t ctx)
{ return os_audio_midi_restore_volumes_native(ctx); }
uint32_t os_audio_midi_mute_volumes(uint32_t ctx)
{ return os_audio_midi_mute_volumes_native(ctx); }

/* ---- voice open/close + output callback (natives: 0x47dae / 0x48666 / 0x47cf5 / 0x47d6e) ---- */
uint32_t os_audio_open_voices(uint32_t desc, uint32_t req_ptr, uint32_t size_ptr,
                              uint32_t handle_out, uint16_t sel)
{ return os_audio_open_voices_native(desc, req_ptr, size_ptr, handle_out, sel); }
uint32_t os_audio_close_voices(uint32_t handle, uint32_t a1, uint32_t a2)
{ return os_audio_close_voices_native(handle, a1, a2); }
uint32_t os_audio_enable_callback(uint32_t svc, uint16_t sel)
{ return os_audio_enable_callback_native(svc, sel); }
uint32_t os_audio_disable_callback(void)
{ return os_audio_disable_callback_native(); }

/* ---- driver detection (natives: 0x48b21 / 0x48c6b / 0x48f79) ---- */
uint32_t os_audio_load_detection_driver(uint32_t path, uint16_t sel)
{ return os_audio_load_detection_driver_native(path, sel); }
uint32_t os_audio_unload_detection_driver(void)
{ return os_audio_unload_detection_driver_native(); }
uint32_t os_audio_find_driver_for_device(uint32_t dev, uint32_t out, uint16_t sel)
{ return os_audio_find_driver_for_device_native(dev, out, sel); }

/* ---- HMI timer cluster (natives: 0x49923 / 0x49ca4 / 0x4980d / 0x498e9) ---- */
uint32_t os_audio_timer_register_event(uint32_t rate, uint32_t cb_off, uint16_t cb_sel,
                                       uint32_t handle_out, uint16_t out_sel)
{ return os_audio_timer_register_event_native(rate, cb_off, cb_sel, handle_out, out_sel); }
uint32_t os_audio_timer_remove_event(uint32_t event)
{ return os_audio_timer_remove_event_native(event); }
uint32_t os_audio_configure_timer_rate(uint32_t rate, uint32_t flags)
{ return os_audio_configure_timer_rate_native(rate, flags); }
uint32_t os_audio_stop_timer_service(void)
{ return os_audio_stop_timer_service_native(); }

/* ---- voice start / load (natives: 0x4a641 / 0x4ad03) ----
 * os_audio_voice_start is reached via gdv_begin_playback's inlined tail (gdv_start_voice); the
 * composition then runs the retired native, image-free. */
uint32_t os_audio_voice_start(uint32_t handle, uint32_t voice, uint16_t ds)
{ uint32_t r = os_audio_voice_start_native(handle, voice, ds);
  sfx_trace_voice_start(r);   /* SFX-dropout trace (ROTH_SFX_TRACE): gated no-op when disarmed */
  return r; }
uint32_t os_audio_voice_load_to_slot(uint32_t handle, uint32_t edx, uint32_t queued, uint16_t ds)
{ return os_audio_voice_load_to_slot_native(handle, edx, queued, ds); }

/* ---- driver-slot install / teardown / dispatch (natives: 0x44553 / 0x44a81 / 0x45d28) ---- */
uint32_t os_audio_alloc_driver_slot(uint32_t device, uint32_t drv_ptr, uint32_t info_ptr,
                                    uint32_t slot_out, uint16_t sel)
{ return os_audio_alloc_driver_slot_native(device, drv_ptr, info_ptr, slot_out, sel); }
uint32_t os_audio_free_driver_slot(uint32_t slot, uint32_t flag)
{ return os_audio_free_driver_slot_native(slot, flag); }
uint32_t os_audio_driver_dispatch_simple(uint32_t slot, uint32_t cmd, uint32_t param, uint16_t sel)
{ return os_audio_driver_dispatch_simple_native(slot, cmd, param, sel); }

/* ---- GDV audio orchestration (natives: 0x4e066 / 0x55440) ---- */
uint32_t os_audio_gdv_begin_playback(void)
{ return os_audio_gdv_begin_playback_native(); }
uint32_t os_audio_gdv_setup_voices(uint32_t dev, uint32_t edx, uint32_t ebx, uint32_t ecx)
{ return os_audio_gdv_setup_voices_native(dev, edx, ebx, ecx); }

/* ---- the 4 voice-field leaves (natives: 0x4ac55 / 0x49fe9 / 0x4a28c / 0x4a54a) ----
 * op map = audio_c2_bridge.c's os_audio_voice_field switch (0=deactivate, 1=xchg_w32, 2=xchg_w54,
 * 3=get_w34), each straight to its _native body. */
uint32_t os_audio_voice_field(uint32_t op, uint32_t handle, uint32_t voice, uint32_t val)
{
    switch (op) {
    case 0:  return os_audio_voice_deactivate_native(handle, voice);
    case 1:  return os_audio_voice_xchg_w32_native(handle, voice, val);
    case 2:  return os_audio_voice_xchg_w54_native(handle, voice, val);
    case 3:  return os_audio_voice_get_w34_native(handle, voice);
    default: return 0;
    }
}
