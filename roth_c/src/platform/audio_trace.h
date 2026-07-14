/* audio_trace.h — the audio co-dev trace tooling.
 *
 * Host-lane dev tool that instruments the 25 call_orig veneers in audio_c2_bridge.c so an
 * in-game session yields per-call ground truth — return, game-memory
 * write-set (before/after snapshots), branch arm and ISR interleave — for retiring the
 * [W4-FALLBACK] audio veneers cluster by cluster.
 *
 * The ring, snapshot pool, ISR sampler and drain live in src/platform/audio_trace.c and
 * link ONLY into roth-host (never the imgfree audio slice). audio_c2_bridge.c carries only the two
 * thin, env-gated taps below (au_trace_enter/exit); it must call NOTHING else from this file. The
 * host-lane sampler/drain hooks (au_trace_tick / au_trace_drain) are called from src/platform/audio.c.
 *
 * Zero cost when ROTH_AU_TRACE is unset: every entry point is one cached-int early return.
 */
#ifndef ROTH_AUDIO_TRACE_H
#define ROTH_AUDIO_TRACE_H

#include <stdint.h>

/* ---- bridge-side taps (audio_c2_bridge.c only calls these two) ------------------------------
 * Wrap ONE veneer call. au_trace_enter records entry (canon VA + the 4 marshalled Watcom arg
 * regs; the tool itself reads the branch predicates, g_irq_eip, phase and the before-snapshot for
 * that VA), then returns an opaque token. au_trace_exit records the return + after-snapshot.
 * Both are no-ops (single cached-int check) when tracing is off; the token is <0 then. */
int  au_trace_enter(uint32_t canon_va, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
void au_trace_exit(int token, uint32_t ret);

/* A/B swap selector (ROTH_AU_AB=native|1). Returns nonzero when the bridge should run a
 * VA's STAGED retired host-C body (os_audio.c) instead of the call_orig veneer. DEFAULT 0 =
 * veneer, so the default path is byte-identical until the sign-off flips the default.
 * Read-once cached; safe to call on the hot path. */
int  au_ab_native(void);
int  au_ab_veneer(void);   /* ROTH_AU_AB=veneer/0: force the call_orig veneers (escape hatch) */

/* Per-VA native decision — the SINGLE predicate every bridge dispatch site consults. Returns nonzero
 * when the os_audio_* for canon `va` should run its native host-C body instead of the call_orig
 * veneer. Composes ROTH_AU_AB (whole-batch A/B) with ROTH_AU_AB_VA (the per-VA bisect: a comma list
 * of canon VAs that, when ROTH_AU_AB is unset, are the ONLY sites that run native). Default (all
 * unset) = the prior hold: dispatch_simple + voice_start native, everything else veneer. Read-once
 * cached; safe on the hot path. */
int  au_ab_va(uint32_t va);

/* ---- host-lane hooks (src/platform/audio.c calls these) --------------------------------------
 * au_trace_tick: one line at the top of audio_tick() (the shm_tick/SIGALRM host beat). Caches the
 *   audio poll tick for record timestamps and, under ROTH_AU_ISR_SAMPLE, does the read-only
 *   memcpy of the SOS timer-event table into a rolling sample buffer (passive ISR witness).
 * au_trace_drain: one line at the MAGIC_POLL safe drain point in audio_trap (the same spot
 *   audio_profile_dump emits — non-nesting, never mid-call_orig). Appends any newly-committed
 *   records + ISR samples to ROTH_AU_TRACE_FILE. */
void au_trace_tick(uint32_t g_mtick);
void au_trace_drain(void);

#endif /* ROTH_AUDIO_TRACE_H */
