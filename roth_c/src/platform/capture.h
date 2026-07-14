/* capture — entry-state snapshots for TRACE-REPLAY verification of orchestration
 * functions (the renderer drivers) that the per-call differential oracle can't
 * verify standalone (their inputs are real scene geometry built by the bridged
 * edge-walker). DIFFERENT from calltrace.c: calltrace logs *which* functions ran
 * (coverage); capture records the *state* at one function's entry so the oracle
 * can replay that exact invocation through both the original bytes and the lifted
 * C and diff the framebuffer.
 *
 * When ROTH_CAPTURE is set, `kill -USR1 <pid>` arms capture: the next N entries of
 * the target function (default draw_scaled_sprite_spans 0x39610) each dump a
 * snapshot to <pid>_roth_capture_<seq>.bin (registers + selector bases + the whole
 * game-visible memory it can touch: low memory 0x10000..0xc0000 = DOSMEM+VGA, and
 * obj3). Because the host maps the game at fixed linear addresses, the replay
 * harness restores those regions at the SAME addresses — no heap relocation.
 *
 *   ROTH_CAPTURE=1            enable (then kill -USR1 to arm)
 *   ROTH_CAPTURE_VA=0x39610   override target (canon VA)
 *   ROTH_CAPTURE_N=4          snapshots per arm (default 4)
 *
 * Mutually exclusive with ROTH_TRACE / ROTH_LIFT / --probe-blend (each plants its
 * own int3 and grabs SIGUSR1).
 */
#ifndef CAPTURE_H
#define CAPTURE_H
#include "roth_host.h"

void capture_init(void);          /* if ROTH_CAPTURE set: save target byte, install SIGUSR1 */
void capture_poll(void);          /* service a pending arm request (call from trap handler) */
int  capture_dispatch(cpu_t *c);  /* handle a capture int3 in the SIGTRAP handler; 1 if handled */

/* On-disk snapshot header, followed by lo_size bytes (@lo_addr) then obj3_size bytes (@obj3_addr). */
struct cap_hdr {
    char     magic[8];            /* "ROTHCAP1" */
    uint32_t canon_va, seq;
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp, eip, eflags;
    uint32_t cs, ds, es, fs, gs, ss;                 /* selector values at entry */
    uint32_t ds_base, es_base, fs_base, gs_base, ss_base;  /* their LDT bases */
    uint32_t lo_addr, lo_size;    /* runtime addr+size of the low-memory dump (DOSMEM+VGA) */
    uint32_t obj3_addr, obj3_size;/* runtime addr+size of the obj3 data dump */
    /* Render selectors the driver/inner-loops load from obj3 globals (e.g. GS colormap
     * @0x8a2a8, ES blend @0x90be2). Their LDT bases (point into the dumped memory) can't be
     * recovered from obj3 alone (the selector NUMBER won't be a valid LDT entry in the replay
     * process), so we record (global canon VA, selector value, base) here; replay recreates an
     * equivalent LDT selector at the captured base and rewrites the global. */
    uint32_t nrsel;
    uint32_t rsel_va[6];          /* canon VA of the selector global */
    uint32_t rsel_sel[6];         /* selector value stored there at entry */
    uint32_t rsel_base[6];        /* its LDT base (a runtime address inside the dumped regions) */
};

#endif
