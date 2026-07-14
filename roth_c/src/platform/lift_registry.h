/* lift_registry — in-host live-swap of verified C reimplementations.
 *
 * When ROTH_LIFT names a lifted function (or "all"), lift_install() patches an
 * int3 at that function's entry; the fault handler calls lift_dispatch(), which
 * runs the C reimplementation (from recomp/lifted/renderer.c), marshals the Watcom
 * registers, simulates the function's `ret`, and resumes — so the verified C
 * runs *in the game* in place of the original bytes. Default OFF.
 *
 * Host integration (the only shared-file touch):
 *   - main.c   (startup, after image map): call lift_install();
 *   - traps.c  (int3/SIGTRAP path, early):  if (lift_dispatch(c)) return; // resume
 */
#ifndef ROTH_LIFT_REGISTRY_H
#define ROTH_LIFT_REGISTRY_H

#include "roth_host.h"   /* cpu_t, R_* register accessors, OBJ_DELTA, LOGE */

/* Patch int3 at each ROTH_LIFT-enabled lifted function. Call once after the
 * objects are mapped and before entering the game. No-op if ROTH_LIFT unset. */
void lift_install(void);

/* If the trapping EIP is a lifted function entry, run its C reimplementation,
 * simulate its ret, and return 1 (handled). Otherwise return 0 (not ours). */
int lift_dispatch(cpu_t *c);

#endif /* ROTH_LIFT_REGISTRY_H */
