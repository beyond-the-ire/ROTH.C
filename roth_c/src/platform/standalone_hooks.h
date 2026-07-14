/* standalone_hooks.h — trap-free host hooks shared by the trap host and the image-free host.
 *
 * host_soft_int() is carved out of lift_registry.c (which carries the live-swap / call_orig deps the
 * image-free binary must not link). It is itself trap-free
 * (marshals a regs_t into a synthetic ucontext and calls the host int21/int10/int31/int33 services), so
 * both roth-host (via lift_install) and the moddable engine binary (roth) (via roth_boot) reach the ONE definition
 * here. roth_unreachable() / roth_sprintf() are the image-free-only hooks. */
#ifndef STANDALONE_HOOKS_H
#define STANDALONE_HOOKS_H

#include "engine.h"   /* regs_t */

/* the g_os_soft_int hook body (carved out of lift_registry.c; behaviourally identical) */
uint32_t host_soft_int(uint8_t vec, regs_t *io);

/* the mod-layer int33 seam: NULL (inert) unless the image-free mods_apply() registers a mod's
 * filter — the trap host never links the mod layer, so it stays NULL there by construction. */
extern void (*g_mods_int33_filter)(uint16_t ax, regs_t *io);

/* image-free fail-loud: an in-game-only bridge target was reached on the boot-to-title path with no
 * original CODE bytes mapped — LOGE the canon VA + reason, then abort(). */
void roth_unreachable(uint32_t canon);

/* image-free Watcom-faithful sprintf shim (0x27c53): CDECL, honours the specifier set the game's
 * boot/title path actually uses (%d, %D long-decimal, %u/%x, %s, %c, %%), fail-loud on anything else.
 * Returns chars written (the sprintf contract — write_roth_ini uses it as the write length). */
int roth_sprintf(char *dst, const char *fmt, ...);

#endif /* STANDALONE_HOOKS_H */
