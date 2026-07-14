/* common.h — shared machinery for every verified-C lift translation unit.
 *
 * The lifts began as one monolithic renderer.c. As subsystems land they split into
 * per-subsystem TUs (lift_render.c, lift_<subsystem>.c, ...) so parallel work and
 * AI context stay manageable. Each lift TU #includes THIS header for the canon->
 * runtime addressing macros + shared types/hooks, then defines its lifts (declared
 * in engine.h). renderer.c remains the "core" TU: early/leaf lifts + the call_orig
 * definition + the int3-hook globals.
 *
 * GADDR override (image-free build): recomp/imgfree force-includes gmacros_symbol.h
 * (gcc -include) which #defines GADDR before this header is reached; the #ifndef
 * guard below keeps that override, so ONE lift source compiles both absolute-
 * addressed (oracle/host, image mapped at fixed VAs) and symbol-addressed (imgfree,
 * data reached through a linker-placed obj3_image symbol). */
#ifndef LIFT_COMMON_H
#define LIFT_COMMON_H

#include "engine.h"   /* OBJ_DELTA, regs_t, call_orig/call_orig_raw, the lift fn decls */

/* GADDR(a): map a canon engine address to a runtime address. Default = the absolute
 * rebased form (a)+OBJ_DELTA. (imgfree predefines GADDR to resolve through a linked
 * data symbol instead — the guard below keeps it.) */
#ifndef GADDR
#define GADDR(a) ((uintptr_t)((a) + OBJ_DELTA))
#endif

/* Game globals; volatile so every faithful read-modify-write is actually emitted
 * (write-set fidelity). canon address resolved through GADDR. */
#define G8(a)  (*(volatile uint8_t  *)GADDR(a))
#define G16(a) (*(volatile uint16_t *)GADDR(a))
#define G32(a) (*(volatile int32_t  *)GADDR(a))

/* int3 suspend/resume hooks: NULL in the oracle (no live-swap), set by the host's
 * lift_install. Defined in renderer.c; used by call_orig. */
extern void (*g_os_suspend_int3s)(void);
extern void (*g_os_resume_int3s)(void);

#ifdef ROTH_STANDALONE
/* host hooks reached ONLY by the image-free build (no original CODE bytes mapped, no
 * call_orig). The guarded bridge helpers below route their few genuine on-path targets here and
 * abort every un-dispatched (in-game-only) one. Defined by the roth-host imgfree object set:
 * roth_unreachable/roth_sprintf in boot.c, host_flip_video_page in traps.c. */
void roth_unreachable(uint32_t canon);               /* fail-loud: LOGE(canon) + abort() */
void host_flip_video_page(uint32_t eax);              /* flip_video_page 0x2e1e8 host present */
void host_blank_active_video_page(void);              /* blank_active_video_page 0x2e140 host body */
int  roth_sprintf(char *dst, const char *fmt, ...);/* Watcom-faithful sprintf shim (0x27c53); returns chars written */
uint32_t vd_standalone_set_fb_bases(uint32_t base);      /* 0x2fdfc transcription (lift_video_display.c); returns CF */
/* M3 §3.3 two-value dispatch shims for the map-load-installed texture->RAW hook slots (defined in
 * lift_raw_commands.c next to the hook lifts; the trap lane keeps its `call [slot]` bridges). */
uint32_t rwss_span_callback_dispatch(uint32_t eax_cwde, uint32_t edx_block); /* [0x90a34] -> 0x33cf3 (else eax through / fail-loud) */
uint32_t rwss_id_remap_dispatch(uint32_t id);
uint32_t rawcmd_dispatch_30780(uint32_t handler_rt, uint32_t rec);           /* the 0x30780 RAW handler table (64 lifted bodies, fail-loud else) */                                /* [0x8a2a0] -> 0x33dde (else id through / fail-loud) */
#endif

#endif /* LIFT_COMMON_H */
