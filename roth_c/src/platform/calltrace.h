/* calltrace — runtime per-event call coverage for function labeling.
 *
 * First-hit coverage: when ROTH_TRACE is set, `kill -USR1 <pid>` arms a capture
 * window (plants int3 at every obj1 function entry); each function traps at most
 * once (we log its canon VA + direct/indirect tag, restore the byte, and let it
 * run native thereafter), so even a full render frame costs <=1339 traps. A
 * `kill -USR2 <pid>` ends the window and dumps the fired set to roth_trace_<n>.txt.
 *
 * Diffing two windows' files isolates feature-specific functions (e.g. "face a
 * wall" minus "face a floor" -> the wall-draw path). Because it's a live trace it
 * also captures indirect-dispatch targets (renderer span/column fn-ptrs, entity
 * vtables) that static analysis can't follow. Each VA is tagged D (reached via a
 * direct `call rel32`) or I (indirect dispatch / other) — feeds dispatch_edges.json.
 *
 * Mutually exclusive with ROTH_LIFT / --probe-blend (they plant their own int3).
 */
#ifndef CALLTRACE_H
#define CALLTRACE_H
#include "roth_host.h"

void calltrace_init(void);          /* if ROTH_TRACE set: save entry bytes, install SIGUSR1/2 */
void calltrace_poll(void);          /* service pending arm/dump requests (call from trap handler) */
int  calltrace_dispatch(cpu_t *c);  /* handle a trace int3 in the SIGTRAP handler; 1 if handled */

#endif
