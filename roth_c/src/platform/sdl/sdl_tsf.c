/* sdl_tsf.c — TinySoundFont implementation TU for the in-process SDL3 audio consumer
 * (task #102 / M2; docs/SDL3_FOLD_DESIGN.md §2.5).
 *
 * This TU exists solely to instantiate TinySoundFont's implementation (the same header-only tsf.h
 * the external viewer uses via `#define TSF_IMPLEMENTATION` in viewer.c:15-16). It is compiled as
 * a SEPARATE object with warnings suppressed (-w in the Makefile) so the vendored third-party code
 * never trips the imgfree "warning-free" gate, while sdl_audio.c — which #includes tsf.h WITHOUT
 * TSF_IMPLEMENTATION to get only the (extern) API prototypes — is compiled with full -Wall -Wextra.
 * The two objects link together: the tsf_* symbols are defined here, referenced there.
 *
 * No SDL, no ROTH_STANDALONE, no shm — pure C float DSP. Reused verbatim from the viewer; the exact
 * synth used cross-process today, now in-process. Needs -lm (expf/powf/sqrtf).
 */
#define TSF_IMPLEMENTATION
#include "../viewer/tsf.h"
